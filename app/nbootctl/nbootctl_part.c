/****************************************************************************
 * apps/system/nbootctl/nbootctl_part.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* Partition-level writes driven from a host download.
 *
 * The host computes a SHA-256 of the file it is about to send and the board
 * compares that against what it actually received before anything is
 * written.  Only on a match does the payload go to the medium, and the
 * region is read back and compared afterwards.  A transfer that is cut
 * short, reordered or corrupted therefore cannot reach the flash: it fails
 * the comparison while the image is still sitting in a file.
 *
 * Two digest checks guard every write, and they catch different things:
 *
 *   - source vs expected: did the bytes arrive intact?
 *   - medium vs source:   did the bytes land intact?
 *
 * Neither is a substitute for the other -- a bad cable explains the first,
 * a bad sector the second.
 *
 * The medium is reached through the block driver itself, the way the
 * bootctrl code reaches it, and not through open() on the device node.
 * That path needs the block-to-character proxy, which a configuration is
 * free to leave out, and it would put a second, unverified I/O path next
 * to the one that stages slots on real hardware.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <crypto/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nbootctl_part.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NBOOTCTL_PART_SECTOR_SIZE   512
#define NBOOTCTL_PART_CHUNK_SECTORS 128
#define NBOOTCTL_PART_CHUNK_BYTES \
  (NBOOTCTL_PART_CHUNK_SECTORS * NBOOTCTL_PART_SECTOR_SIZE)

/* Protective MBR, primary header and 128 entries of 128 bytes. */

#define NBOOTCTL_PART_GPT_SECTORS 34

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Partition index within the GPT, which is what NuttX uses to name the
 * device node: the layout in parameter.txt lists uboot first, so it becomes
 * /dev/mmcsdNp1.  Names are matched here and translated to an index; the
 * names are never used as node names directly.
 *
 * config sits between the AMP slots and data.  It holds provisioning and
 * identity, which a factory reset must preserve or deliberately clear,
 * while data holds models that an OTA can replace.  Keeping them in
 * separate partitions is what lets one be wiped without the other.
 */

struct nbootctl_partition_s
{
  const char *name;
  unsigned int index;
  uint64_t blocks; /* 0 = grows to the end, size not enforced */
};

/* A window of a block device: every sector number below is relative to
 * `base`, and nothing at or beyond `sectors` is ever touched.
 */

struct nbootctl_region_s
{
  struct inode *inode;
  uint64_t base;
  uint64_t sectors;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static const char *nbootctl_part_disk_path(unsigned int medium);
static const struct nbootctl_partition_s *
nbootctl_part_find(const char *partition);
static int nbootctl_part_hex_digit(char c);
static int nbootctl_part_parse_hex(const char *text, uint8_t *out,
                                   size_t size);
static void nbootctl_part_print_hex(FILE *stream, const uint8_t *digest);
static int nbootctl_part_hash_file(const char *path, uint8_t *digest,
                                   uint64_t *size_out);
static int nbootctl_part_region_open(const char *node, uint64_t lba,
                                     uint64_t limit, bool writable,
                                     struct nbootctl_region_s *region);
static void nbootctl_part_region_close(struct nbootctl_region_s *region);
static int nbootctl_part_region_hash(const struct nbootctl_region_s *region,
                                     uint64_t bytes, uint8_t *buffer,
                                     uint8_t *digest);
static int nbootctl_part_region_fill(const struct nbootctl_region_s *region,
                                     const char *path, uint64_t size,
                                     uint8_t *buffer, uint8_t *digest);
static int nbootctl_part_write_file(const char *node, uint64_t lba,
                                    uint64_t limit, const char *path,
                                    const char *hex);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct nbootctl_partition_s g_nbootctl_partitions[] = {
  { "uboot", 1, 8192 },     /* 4 MiB, the N-Boot FIT */
  { "trust", 2, 8192 },     /* 4 MiB */
  { "bootctrl", 3, 2048 },  /* 1 MiB, two 4096 byte records */
  { "nuttx_a", 4, 131072 }, /* 64 MiB */
  { "nuttx_b", 5, 131072 }, /* 64 MiB */
  { "amp_a", 6, 1048576 },  /* 512 MiB */
  { "amp_b", 7, 1048576 },  /* 512 MiB */
  { "config", 8, 65536 },   /* 32 MiB */
  { "data", 9, 0 },         /* the rest of the medium */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nbootctl_part_disk_path
 ****************************************************************************/

static const char *nbootctl_part_disk_path(unsigned int medium)
{
  return medium == 1 ? "/dev/mmcsd0" : medium == 2 ? "/dev/mmcsd1" : NULL;
}

/****************************************************************************
 * Name: nbootctl_part_find
 ****************************************************************************/

static const struct nbootctl_partition_s *
nbootctl_part_find(const char *partition)
{
  size_t i;

  if (partition == NULL)
    {
      return NULL;
    }

  for (i = 0;
       i < sizeof(g_nbootctl_partitions) / sizeof(g_nbootctl_partitions[0]);
       i++)
    {
      if (strcmp(partition, g_nbootctl_partitions[i].name) == 0)
        {
          return &g_nbootctl_partitions[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: nbootctl_part_hex_digit
 ****************************************************************************/

static int nbootctl_part_hex_digit(char c)
{
  if (c >= '0' && c <= '9')
    {
      return c - '0';
    }

  if (c >= 'a' && c <= 'f')
    {
      return c - 'a' + 10;
    }

  if (c >= 'A' && c <= 'F')
    {
      return c - 'A' + 10;
    }

  return -1;
}

/****************************************************************************
 * Name: nbootctl_part_parse_hex
 *
 * Description:
 *   Turn a hex digest into bytes.  Either case is accepted; the host may
 *   format it both ways.
 *
 ****************************************************************************/

static int nbootctl_part_parse_hex(const char *text, uint8_t *out, size_t size)
{
  size_t i;

  if (text == NULL || strlen(text) != size * 2)
    {
      return -EINVAL;
    }

  for (i = 0; i < size; i++)
    {
      int high = nbootctl_part_hex_digit(text[i * 2]);
      int low = nbootctl_part_hex_digit(text[i * 2 + 1]);

      if (high < 0 || low < 0)
        {
          return -EINVAL;
        }

      out[i] = (uint8_t)((high << 4) | low);
    }

  return 0;
}

/****************************************************************************
 * Name: nbootctl_part_print_hex
 ****************************************************************************/

static void nbootctl_part_print_hex(FILE *stream, const uint8_t *digest)
{
  int i;

  for (i = 0; i < NBOOTCTL_SHA256_SIZE; i++)
    {
      fprintf(stream, "%02x", digest[i]);
    }
}

/****************************************************************************
 * Name: nbootctl_part_hash_file
 ****************************************************************************/

static int nbootctl_part_hash_file(const char *path, uint8_t *digest,
                                   uint64_t *size_out)
{
  struct stat file_info;
  SHA2_CTX hash;
  uint8_t *buffer;
  uint64_t size;
  uint64_t offset;
  int fd;
  int ret = 0;

  if (path == NULL || stat(path, &file_info) < 0 ||
      !S_ISREG(file_info.st_mode) || file_info.st_size <= 0)
    {
      return -EINVAL;
    }

  size = (uint64_t)file_info.st_size;
  buffer = memalign(64, NBOOTCTL_PART_CHUNK_BYTES);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      ret = -errno;
      free(buffer);
      return ret;
    }

  sha256init(&hash);
  for (offset = 0; offset < size;)
    {
      size_t bytes = size - offset > NBOOTCTL_PART_CHUNK_BYTES
                         ? NBOOTCTL_PART_CHUNK_BYTES
                         : (size_t)(size - offset);

      if (read(fd, buffer, bytes) != (ssize_t)bytes)
        {
          ret = -EIO;
          break;
        }

      sha256update(&hash, buffer, bytes);
      offset += bytes;
    }

  if (ret == 0)
    {
      sha256final(digest, &hash);
      *size_out = size;
    }

  close(fd);
  free(buffer);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_part_region_open
 *
 * Description:
 *   Open a block device and describe the window starting at `lba`.
 *
 *   `limit` is how many sectors the caller is prepared to touch, 0 for "to
 *   the end of the device".  The window is the smaller of that and what the
 *   device really has, so a layout table that disagrees with the medium can
 *   never push a write past the end of a partition node.
 *
 ****************************************************************************/

static int nbootctl_part_region_open(const char *node, uint64_t lba,
                                     uint64_t limit, bool writable,
                                     struct nbootctl_region_s *region)
{
  struct geometry geometry;
  uint64_t total;
  int ret;

  memset(region, 0, sizeof(*region));
  ret = open_blockdriver(node, writable ? 0 : MS_RDONLY, &region->inode);
  if (ret < 0)
    {
      region->inode = NULL;
      return ret;
    }

  if (region->inode->u.i_bops->read == NULL ||
      region->inode->u.i_bops->geometry == NULL ||
      (writable && region->inode->u.i_bops->write == NULL))
    {
      ret = -ENOSYS;
      goto fail;
    }

  ret = region->inode->u.i_bops->geometry(region->inode, &geometry);
  if (ret < 0)
    {
      goto fail;
    }

  if (!geometry.geo_available ||
      geometry.geo_sectorsize != NBOOTCTL_PART_SECTOR_SIZE ||
      (writable && !geometry.geo_writeenabled))
    {
      ret = -ENODEV;
      goto fail;
    }

  total = (uint64_t)geometry.geo_nsectors;
  if (lba >= total)
    {
      ret = -EINVAL;
      goto fail;
    }

  region->base = lba;
  region->sectors = total - lba;
  if (limit != 0 && limit < region->sectors)
    {
      region->sectors = limit;
    }

  return 0;

fail:
  nbootctl_part_region_close(region);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_part_region_close
 ****************************************************************************/

static void nbootctl_part_region_close(struct nbootctl_region_s *region)
{
  if (region->inode != NULL)
    {
      close_blockdriver(region->inode);
      region->inode = NULL;
    }
}

/****************************************************************************
 * Name: nbootctl_part_region_hash
 *
 * Description:
 *   SHA-256 of the first `bytes` bytes of a region.  Whole sectors are
 *   read, but only the payload is hashed: the zero padding behind an image
 *   whose size is not a multiple of the sector size is not part of the
 *   digest the host computed, and hashing it would fail every such image
 *   after it had already been written.
 *
 ****************************************************************************/

static int nbootctl_part_region_hash(const struct nbootctl_region_s *region,
                                     uint64_t bytes, uint8_t *buffer,
                                     uint8_t *digest)
{
  SHA2_CTX hash;
  uint64_t offset;

  sha256init(&hash);
  for (offset = 0; offset < bytes;)
    {
      size_t take = bytes - offset > NBOOTCTL_PART_CHUNK_BYTES
                        ? NBOOTCTL_PART_CHUNK_BYTES
                        : (size_t)(bytes - offset);
      size_t whole =
          (take + NBOOTCTL_PART_SECTOR_SIZE - 1) / NBOOTCTL_PART_SECTOR_SIZE;

      if (region->inode->u.i_bops->read(region->inode, buffer,
                                        region->base +
                                            offset / NBOOTCTL_PART_SECTOR_SIZE,
                                        whole) != (ssize_t)whole)
        {
          return -EIO;
        }

      sha256update(&hash, buffer, take);
      offset += take;
    }

  sha256final(digest, &hash);
  return 0;
}

/****************************************************************************
 * Name: nbootctl_part_region_fill
 *
 * Description:
 *   Copy a file to the start of a region and return the digest of what was
 *   copied.  The tail of the last sector is zero-padded.  A short read is
 *   an error: the size came from the file, so anything less means the file
 *   changed underneath us.
 *
 ****************************************************************************/

static int nbootctl_part_region_fill(const struct nbootctl_region_s *region,
                                     const char *path, uint64_t size,
                                     uint8_t *buffer, uint8_t *digest)
{
  SHA2_CTX hash;
  uint64_t offset;
  int source;
  int ret = 0;

  source = open(path, O_RDONLY);
  if (source < 0)
    {
      return -errno;
    }

  sha256init(&hash);
  for (offset = 0; offset < size;)
    {
      size_t take = size - offset > NBOOTCTL_PART_CHUNK_BYTES
                        ? NBOOTCTL_PART_CHUNK_BYTES
                        : (size_t)(size - offset);
      size_t whole =
          (take + NBOOTCTL_PART_SECTOR_SIZE - 1) / NBOOTCTL_PART_SECTOR_SIZE;

      memset(buffer, 0, whole * NBOOTCTL_PART_SECTOR_SIZE);
      if (read(source, buffer, take) != (ssize_t)take)
        {
          ret = -EIO;
          break;
        }

      sha256update(&hash, buffer, take);
      if (region->inode->u.i_bops->write(
              region->inode, buffer,
              region->base + offset / NBOOTCTL_PART_SECTOR_SIZE,
              whole) != (ssize_t)whole)
        {
          ret = -EIO;
          break;
        }

      offset += take;
    }

  close(source);
  if (ret == 0)
    {
      sha256final(digest, &hash);
    }

  return ret;
}

/****************************************************************************
 * Name: nbootctl_part_write_file
 *
 * Description:
 *   The one write path.  A partition is its own device node starting at
 *   LBA 0, so NuttX bounds the write to it; a raw region is the whole disk
 *   at an offset, bounded by `limit`.
 *
 ****************************************************************************/

static int nbootctl_part_write_file(const char *node, uint64_t lba,
                                    uint64_t limit, const char *path,
                                    const char *hex)
{
  struct nbootctl_region_s region;
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint8_t *buffer;
  uint64_t file_size;
  uint64_t sectors;
  int ret;

  if (nbootctl_part_parse_hex(hex, expected, sizeof(expected)) < 0)
    {
      return -EINVAL;
    }

  ret = nbootctl_part_hash_file(path, actual, &file_size);
  if (ret < 0)
    {
      return ret;
    }

  /* Refuse before opening the medium: a mismatched digest means the host
   * and the board disagree about what is being written, and nothing about
   * that is safe to proceed with.
   */

  if (memcmp(actual, expected, sizeof(actual)) != 0)
    {
      fprintf(stderr, "nbootctl: source digest mismatch\n");
      return -EKEYREJECTED;
    }

  ret = nbootctl_part_region_open(node, lba, limit, true, &region);
  if (ret < 0)
    {
      return ret;
    }

  sectors =
      (file_size + NBOOTCTL_PART_SECTOR_SIZE - 1) / NBOOTCTL_PART_SECTOR_SIZE;
  if (sectors > region.sectors)
    {
      fprintf(stderr, "nbootctl: payload does not fit (%llu > %llu blocks)\n",
              (unsigned long long)sectors, (unsigned long long)region.sectors);
      nbootctl_part_region_close(&region);
      return -EFBIG;
    }

  buffer = memalign(64, NBOOTCTL_PART_CHUNK_BYTES);
  if (buffer == NULL)
    {
      nbootctl_part_region_close(&region);
      return -ENOMEM;
    }

  /* The file is hashed a second time while it is copied: what reached the
   * medium is this read of it, not the one the first digest came from.
   */

  ret = nbootctl_part_region_fill(&region, path, file_size, buffer, actual);
  if (ret == 0 && memcmp(actual, expected, sizeof(actual)) != 0)
    {
      fprintf(stderr, "nbootctl: source changed while it was written\n");
      ret = -EKEYREJECTED;
    }

  if (ret == 0)
    {
      ret = nbootctl_part_region_hash(&region, file_size, buffer, actual);
    }

  if (ret == 0 && memcmp(actual, expected, sizeof(actual)) != 0)
    {
      fprintf(stderr, "nbootctl: read-back digest mismatch\n");
      ret = -EKEYREJECTED;
    }

  free(buffer);
  nbootctl_part_region_close(&region);
  if (ret == 0)
    {
      printf("write: %llu bytes to %s at lba %llu, digest ",
             (unsigned long long)file_size, node, (unsigned long long)lba);
      nbootctl_part_print_hex(stdout, expected);
      printf(", readback OK\n");
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nbootctl_part_digest
 ****************************************************************************/

int nbootctl_part_digest(const char *path)
{
  uint8_t digest[NBOOTCTL_SHA256_SIZE];
  uint64_t size;
  int ret;

  ret = nbootctl_part_hash_file(path, digest, &size);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: cannot hash %s: %d\n",
              path != NULL ? path : "(null)", ret);
      return ret;
    }

  printf("%llu ", (unsigned long long)size);
  nbootctl_part_print_hex(stdout, digest);
  printf("\n");
  return 0;
}

/****************************************************************************
 * Name: nbootctl_part_verify
 ****************************************************************************/

int nbootctl_part_verify(const char *path, const char *hex)
{
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint64_t size;
  int ret;

  if (nbootctl_part_parse_hex(hex, expected, sizeof(expected)) < 0)
    {
      return -EINVAL;
    }

  ret = nbootctl_part_hash_file(path, actual, &size);
  if (ret < 0)
    {
      return ret;
    }

  if (memcmp(actual, expected, sizeof(actual)) != 0)
    {
      fprintf(stderr, "nbootctl: digest mismatch: got ");
      nbootctl_part_print_hex(stderr, actual);
      fprintf(stderr, "\n");
      return -EKEYREJECTED;
    }

  printf("verify: %llu bytes match\n", (unsigned long long)size);
  return 0;
}

/****************************************************************************
 * Name: nbootctl_part_device_path
 ****************************************************************************/

int nbootctl_part_device_path(unsigned int medium, const char *partition,
                              char *out, size_t size)
{
  const char *disk_path = nbootctl_part_disk_path(medium);
  const struct nbootctl_partition_s *entry;
  int count;

  if (disk_path == NULL || partition == NULL || out == NULL || size == 0)
    {
      return -EINVAL;
    }

  entry = nbootctl_part_find(partition);
  if (entry == NULL)
    {
      return -ENOENT;
    }

  count = snprintf(out, size, "%sp%u", disk_path, entry->index);
  return count < 0 || (size_t)count >= size ? -ENAMETOOLONG : 0;
}

/****************************************************************************
 * Name: nbootctl_part_size
 ****************************************************************************/

int nbootctl_part_size(const char *partition, uint64_t *bytes)
{
  const struct nbootctl_partition_s *entry = nbootctl_part_find(partition);

  if (bytes == NULL)
    {
      return -EINVAL;
    }

  if (entry == NULL)
    {
      return -ENOENT;
    }

  *bytes = entry->blocks * NBOOTCTL_PART_SECTOR_SIZE;
  return 0;
}

/****************************************************************************
 * Name: nbootctl_part_write
 ****************************************************************************/

int nbootctl_part_write(unsigned int medium, const char *partition,
                        const char *path, const char *hex)
{
  const struct nbootctl_partition_s *entry = nbootctl_part_find(partition);
  char node[NBOOTCTL_FORMAT_PATH_MAX];
  int ret;

  if (entry == NULL)
    {
      fprintf(stderr, "nbootctl: unknown partition %s\n",
              partition != NULL ? partition : "(null)");
      return -EINVAL;
    }

  /* Write through the partition's own device node.  NuttX names block
   * partitions by their index in the table, so /dev/mmcsdNp1 is uboot and
   * /dev/mmcsdNp3 is bootctrl -- the name the user typed never reaches the
   * filesystem.  Using the partition node rather than the whole disk at an
   * offset means NuttX itself bounds the write to the partition.
   */

  ret = nbootctl_part_device_path(medium, partition, node, sizeof(node));
  if (ret < 0)
    {
      return ret;
    }

  return nbootctl_part_write_file(node, 0, entry->blocks, path, hex);
}

/****************************************************************************
 * Name: nbootctl_part_write_raw
 ****************************************************************************/

int nbootctl_part_write_raw(unsigned int medium, uint64_t lba,
                            uint64_t sectors, const char *path,
                            const char *hex)
{
  const char *disk_path = nbootctl_part_disk_path(medium);

  /* A raw write without a bound is a write to the rest of the disk. */

  if (disk_path == NULL || sectors == 0)
    {
      return -EINVAL;
    }

  return nbootctl_part_write_file(disk_path, lba, sectors, path, hex);
}

/****************************************************************************
 * Name: nbootctl_part_write_gpt
 ****************************************************************************/

int nbootctl_part_write_gpt(unsigned int medium, const char *path,
                            const char *hex)
{
  const char *disk_path = nbootctl_part_disk_path(medium);

  /* The table occupies LBA 0 through the end of the entry array.  With the
   * standard 128 entries of 128 bytes that is 34 sectors; the file may be
   * shorter than that, never longer.
   */

  if (disk_path == NULL)
    {
      return -EINVAL;
    }

  return nbootctl_part_write_file(disk_path, 0, NBOOTCTL_PART_GPT_SECTORS,
                                  path, hex);
}

/****************************************************************************
 * Name: nbootctl_part_check_raw
 ****************************************************************************/

int nbootctl_part_check_raw(unsigned int medium, uint64_t lba,
                            uint64_t sectors, const char *hex)
{
  const char *disk_path = nbootctl_part_disk_path(medium);
  struct nbootctl_region_s region;
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint8_t *buffer;
  int ret;

  if (disk_path == NULL || sectors == 0 ||
      nbootctl_part_parse_hex(hex, expected, sizeof(expected)) < 0)
    {
      return -EINVAL;
    }

  ret = nbootctl_part_region_open(disk_path, lba, sectors, false, &region);
  if (ret < 0)
    {
      return ret;
    }

  if (region.sectors < sectors)
    {
      /* The region asked for runs off the end of the medium. */

      nbootctl_part_region_close(&region);
      return -EINVAL;
    }

  buffer = memalign(64, NBOOTCTL_PART_CHUNK_BYTES);
  if (buffer == NULL)
    {
      nbootctl_part_region_close(&region);
      return -ENOMEM;
    }

  ret = nbootctl_part_region_hash(&region, sectors * NBOOTCTL_PART_SECTOR_SIZE,
                                  buffer, actual);
  free(buffer);
  nbootctl_part_region_close(&region);
  if (ret == 0 && memcmp(actual, expected, sizeof(actual)) != 0)
    {
      ret = -EKEYREJECTED;
    }

  if (ret == 0)
    {
      printf("check: lba %llu..%llu matches\n", (unsigned long long)lba,
             (unsigned long long)(lba + sectors - 1));
    }
  else if (ret == -EKEYREJECTED)
    {
      fprintf(stderr, "nbootctl: region digest mismatch\n");
    }

  return ret;
}
