/****************************************************************************
 * apps/system/nbootctl/nbootctl_bootctrl.c
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

#include <nuttx/config.h>

#include <crypto/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <nuttx/crc32.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nbootctl_bootctrl.h"

#define NBOOTCTL_MAGIC          "K7ABCTRL"
#define NBOOTCTL_FORMAT_VERSION 1
#define NBOOTCTL_RECORD_SIZE    4096
#define NBOOTCTL_RECORD_SECTORS 8
#define NBOOTCTL_COPY_COUNT     2
#define NBOOTCTL_SHA256_SIZE    32
#define NBOOTCTL_VERIFY_SECTORS 128
#define NBOOTCTL_SECTOR_SIZE    512
#define NBOOTCTL_UBOOT_START    16384
#define NBOOTCTL_UBOOT_SECTORS  8192
#define NBOOTCTL_FIT_MAGIC      0xd00dfeedu
#define NBOOTCTL_REBOOT_MAGIC   0x4e425200u

/* N-Boot publishes this boot's medium, domain and slot in PMU1 GRF scratch
 * registers immediately before it enters NuttX, or, for an AMP image, before
 * it starts the first Linux CPU.
 *
 *   31:16 magic  15:12 version  11:8 reason  7:4 medium  3:2 domain
 *   1 reserved, zero  0 slot
 *
 * The domain bits were zero in every record written before AMP handoffs, so
 * a NuttX-slot record reads the same as it always did.
 */

#define NBOOTCTL_HANDOFF_REG        0x26026234ul
#define NBOOTCTL_GENERATION_LO_REG  0x26026238ul
#define NBOOTCTL_GENERATION_HI_REG  0x2602623cul
#define NBOOTCTL_HANDOFF_MAGIC      0x4e480000u
#define NBOOTCTL_HANDOFF_MAGIC_MASK 0xffff0000u
#define NBOOTCTL_HANDOFF_VERSION    2u

struct nbootctl_slot_s
{
  uint8_t priority;
  uint8_t tries_remaining;
  uint8_t successful;
  uint8_t reserved;
  uint64_t image_size;
  uint64_t image_version;
  uint8_t sha256[NBOOTCTL_SHA256_SIZE];
} __attribute__((packed));

struct nbootctl_domain_s
{
  uint8_t active_slot;
  uint8_t reserved[3];
  struct nbootctl_slot_s slots[2];
} __attribute__((packed));

struct nbootctl_record_s
{
  uint8_t magic[8];
  uint16_t format_version;
  uint16_t header_size;
  uint64_t generation;
  struct nbootctl_domain_s domains[2];
  uint8_t padding[NBOOTCTL_RECORD_SIZE - 4 - 20 -
                  sizeof(struct nbootctl_domain_s) * 2];
  uint32_t crc32;
} __attribute__((packed));

_Static_assert(sizeof(struct nbootctl_record_s) == NBOOTCTL_RECORD_SIZE,
               "bootctrl record size changed");
_Static_assert(sizeof(struct nbootctl_slot_s) == 52,
               "bootctrl slot size changed");
_Static_assert(sizeof(struct nbootctl_domain_s) == 108,
               "bootctrl domain size changed");
_Static_assert(offsetof(struct nbootctl_record_s, generation) == 12,
               "bootctrl generation offset changed");
_Static_assert(offsetof(struct nbootctl_record_s, domains) == 20,
               "bootctrl domain offset changed");
_Static_assert(offsetof(struct nbootctl_record_s, crc32) == 4092,
               "bootctrl CRC offset changed");
_Static_assert(offsetof(struct nbootctl_record_s, padding) == 236,
               "bootctrl request offset changed");

/* Private Function Prototypes */

static uint32_t g_nbootctl_handoff_header;
static uint64_t g_nbootctl_handoff_generation;
static volatile bool g_nbootctl_handoff_latched;

static uint32_t nbootctl_reg_read(uintptr_t address);
static void nbootctl_slot_export(const struct nbootctl_slot_s *from,
                                 struct nbootctl_slot_state_s *to);
static const char *nbootctl_bootctrl_path(unsigned int medium);
static const char *nbootctl_disk_path(unsigned int medium);
static uint32_t nbootctl_be32(const uint8_t *value);
static uint32_t nbootctl_crc32(const void *data, size_t size);
static const char *nbootctl_slot_path(unsigned int medium, int domain,
                                      unsigned int slot);
static int nbootctl_domain_index(const char *domain);
static bool nbootctl_record_valid(const struct nbootctl_record_s *record);
static int nbootctl_read_records(unsigned int medium, struct inode **inode,
                                 struct nbootctl_record_s *records,
                                 int *selected);
static int nbootctl_write_records(struct inode *inode,
                                  struct nbootctl_record_s *records,
                                  int selected);
static int nbootctl_bootctrl_update(unsigned int medium, const char *domain,
                                    unsigned int slot, bool mark_successful);

/****************************************************************************
 * Name: nbootctl_reg_read
 ****************************************************************************/

static uint32_t nbootctl_reg_read(uintptr_t address)
{
  return *(volatile uint32_t *)address;
}

/****************************************************************************
 * Name: nbootctl_slot_export
 ****************************************************************************/

static void nbootctl_slot_export(const struct nbootctl_slot_s *from,
                                 struct nbootctl_slot_state_s *to)
{
  to->priority = from->priority;
  to->successful = from->successful != 0;
  to->image_size = from->image_size;
  to->image_version = from->image_version;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_path
 ****************************************************************************/

static const char *nbootctl_bootctrl_path(unsigned int medium)
{
  return medium == 1 ? "/dev/mmcsd0p3" : medium == 2 ? "/dev/mmcsd1p3" : NULL;
}

/****************************************************************************
 * Name: nbootctl_disk_path
 ****************************************************************************/

static const char *nbootctl_disk_path(unsigned int medium)
{
  return medium == 1 ? "/dev/mmcsd0" : medium == 2 ? "/dev/mmcsd1" : NULL;
}

/****************************************************************************
 * Name: nbootctl_be32
 ****************************************************************************/

static uint32_t nbootctl_be32(const uint8_t *value)
{
  return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
         (uint32_t)value[2] << 8 | value[3];
}

/****************************************************************************
 * Name: nbootctl_crc32
 ****************************************************************************/

static uint32_t nbootctl_crc32(const void *data, size_t size)
{
  return crc32part(data, size, UINT32_MAX) ^ UINT32_MAX;
}

/****************************************************************************
 * Name: nbootctl_slot_path
 ****************************************************************************/

static const char *nbootctl_slot_path(unsigned int medium, int domain,
                                      unsigned int slot)
{
  static const char *const paths[2][2][2] = {
    { { "/dev/mmcsd0p4", "/dev/mmcsd0p5" },
      { "/dev/mmcsd0p6", "/dev/mmcsd0p7" } },
    { { "/dev/mmcsd1p4", "/dev/mmcsd1p5" },
      { "/dev/mmcsd1p6", "/dev/mmcsd1p7" } },
  };

  return medium >= 1 && medium <= 2 && domain >= 0 && domain <= 1 && slot <= 1
             ? paths[medium - 1][domain][slot]
             : NULL;
}

/****************************************************************************
 * Name: nbootctl_domain_index
 ****************************************************************************/

static int nbootctl_domain_index(const char *domain)
{
  if (strcmp(domain, "nuttx") == 0)
    {
      return 0;
    }

  return strcmp(domain, "amp") == 0 ? 1 : -EINVAL;
}

/****************************************************************************
 * Name: nbootctl_record_valid
 ****************************************************************************/

static bool nbootctl_record_valid(const struct nbootctl_record_s *record)
{
  uint32_t checksum;

  if (memcmp(record->magic, NBOOTCTL_MAGIC, sizeof(record->magic)) != 0 ||
      record->format_version != NBOOTCTL_FORMAT_VERSION ||
      record->header_size != 20)
    {
      return false;
    }

  checksum = nbootctl_crc32(record, offsetof(struct nbootctl_record_s, crc32));
  return checksum == record->crc32;
}

/****************************************************************************
 * Name: nbootctl_read_records
 ****************************************************************************/

static int nbootctl_read_records(unsigned int medium, struct inode **inode,
                                 struct nbootctl_record_s *records,
                                 int *selected)
{
  const char *path = nbootctl_bootctrl_path(medium);
  int best = -1;
  int index;
  int ret;

  if (path == NULL)
    {
      return -EINVAL;
    }

  ret = open_blockdriver(path, 0, inode);
  if (ret < 0 || (*inode)->u.i_bops->read == NULL ||
      (*inode)->u.i_bops->write == NULL)
    {
      if (ret >= 0)
        {
          close_blockdriver(*inode);
          *inode = NULL;
        }

      return ret < 0 ? ret : -ENOSYS;
    }

  for (index = 0; index < NBOOTCTL_COPY_COUNT; index++)
    {
      if ((*inode)->u.i_bops->read(*inode, (uint8_t *)&records[index],
                                   index * NBOOTCTL_RECORD_SECTORS,
                                   NBOOTCTL_RECORD_SECTORS) !=
              NBOOTCTL_RECORD_SECTORS ||
          !nbootctl_record_valid(&records[index]))
        {
          continue;
        }

      if (best < 0 || records[index].generation > records[best].generation)
        {
          best = index;
        }
    }

  if (best < 0)
    {
      close_blockdriver(*inode);
      *inode = NULL;
      return -EBADMSG;
    }

  *selected = best;
  return 0;
}

/****************************************************************************
 * Name: nbootctl_write_records
 ****************************************************************************/

static int nbootctl_write_records(struct inode *inode,
                                  struct nbootctl_record_s *records,
                                  int selected)
{
  struct nbootctl_record_s *record = &records[selected];
  struct nbootctl_record_s *verify = &records[1 - selected];
  int order[2] = { 1 - selected, selected };
  int index;

  /* Both records are allocated on the aligned heap by the caller. A record
   * alone fills the task's entire 4 KiB stack and must never be local here.
   */

  record->generation++;
  record->crc32 =
      nbootctl_crc32(record, offsetof(struct nbootctl_record_s, crc32));

  for (index = 0; index < NBOOTCTL_COPY_COUNT; index++)
    {
      if (inode->u.i_bops->write(inode, (const uint8_t *)record,
                                 order[index] * NBOOTCTL_RECORD_SECTORS,
                                 NBOOTCTL_RECORD_SECTORS) !=
              NBOOTCTL_RECORD_SECTORS ||
          inode->u.i_bops->read(
              inode, (uint8_t *)verify, order[index] * NBOOTCTL_RECORD_SECTORS,
              NBOOTCTL_RECORD_SECTORS) != NBOOTCTL_RECORD_SECTORS ||
          memcmp(record, verify, sizeof(*record)) != 0)
        {
          return -EIO;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: nbootctl_handoff_read
 ****************************************************************************/

int nbootctl_handoff_read(unsigned int *medium, unsigned int *domain,
                          unsigned int *slot, unsigned int *reason,
                          uint64_t *generation)
{
  uint32_t header;
  uint32_t confirm;
  uint64_t value;
  unsigned int boot_reason;
  unsigned int boot_medium;
  unsigned int boot_domain;
  unsigned int boot_slot;

  if (g_nbootctl_handoff_latched)
    {
      header = g_nbootctl_handoff_header;
      value = g_nbootctl_handoff_generation;
    }
  else
    {
      /* N-Boot writes the generation words first and the header last.
       * Reading the header on both sides of them rejects a handoff caught
       * mid-update.
       */

      header = nbootctl_reg_read(NBOOTCTL_HANDOFF_REG);
      value = nbootctl_reg_read(NBOOTCTL_GENERATION_LO_REG);
      value |= (uint64_t)nbootctl_reg_read(NBOOTCTL_GENERATION_HI_REG) << 32;
      confirm = nbootctl_reg_read(NBOOTCTL_HANDOFF_REG);
      if (header != confirm)
        {
          return -ENODEV;
        }
    }

  if ((header & NBOOTCTL_HANDOFF_MAGIC_MASK) != NBOOTCTL_HANDOFF_MAGIC ||
      ((header >> 12) & 0xf) != NBOOTCTL_HANDOFF_VERSION)
    {
      return -ENODEV;
    }

  boot_reason = (header >> 8) & 0xf;
  boot_medium = (header >> 4) & 0xf;
  boot_domain = (header >> 2) & 0x3;
  boot_slot = header & 0x1;
  if (boot_medium < 1 || boot_medium > 2 ||
      boot_reason > NBOOTCTL_REASON_RAM ||
      boot_domain > NBOOTCTL_DOMAIN_AMP || (header & 0x2) != 0)
    {
      return -EBADMSG;
    }

  /* Only bootamp starts an image from RAM, and such an image has no slot:
   * the slot bit of that record must be zero and means nothing.
   */

  if (boot_reason == NBOOTCTL_REASON_RAM)
    {
      if (boot_domain != NBOOTCTL_DOMAIN_AMP || boot_slot != 0)
        {
          return -EBADMSG;
        }

      boot_slot = NBOOTCTL_SLOT_NONE;
    }

  /* Both writers store the same values, so two tasks getting here together
   * do no harm; the flag goes last.
   */

  if (!g_nbootctl_handoff_latched)
    {
      g_nbootctl_handoff_header = header;
      g_nbootctl_handoff_generation = value;
      g_nbootctl_handoff_latched = true;
    }

  if (domain != NULL)
    {
      *domain = boot_domain;
    }

  if (reason != NULL)
    {
      *reason = boot_reason;
    }

  if (medium != NULL)
    {
      *medium = boot_medium;
    }

  if (slot != NULL)
    {
      *slot = boot_slot;
    }

  if (generation != NULL)
    {
      *generation = value;
    }

  return 0;
}

/****************************************************************************
 * Name: nbootctl_running_slot
 ****************************************************************************/

unsigned int nbootctl_running_slot(unsigned int running_domain,
                                   unsigned int running_slot,
                                   unsigned int domain)
{
  return running_domain == domain && running_slot <= 1 ? running_slot
                                                       : NBOOTCTL_SLOT_NONE;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_snapshot
 ****************************************************************************/

int nbootctl_bootctrl_snapshot(struct nbootctl_state_s *state)
{
  struct nbootctl_record_s *records;
  const struct nbootctl_record_s *record;
  struct inode *inode = NULL;
  int selected;
  int slot;
  int ret;

  memset(state, 0, sizeof(*state));
  if (nbootctl_handoff_read(&state->medium, &state->running_domain,
                            &state->running_slot, NULL, NULL) < 0)
    {
      /* Not an error: an image started without N-Boot has no handoff, and
       * then there is no telling which medium holds bootctrl.
       */

      return 0;
    }

  /* A record fills a 4 KiB task stack on its own; both live on the heap. */

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  if (records == NULL)
    {
      return -ENOMEM;
    }

  ret = nbootctl_read_records(state->medium, &inode, records, &selected);
  if (ret < 0)
    {
      free(records);
      return ret;
    }

  record = &records[selected];
  state->handoff_valid = true;
  state->bootctrl_generation = record->generation;
  state->nuttx_active = record->domains[0].active_slot ? 1 : 0;
  state->amp_active = record->domains[1].active_slot ? 1 : 0;
  for (slot = 0; slot < 2; slot++)
    {
      nbootctl_slot_export(&record->domains[0].slots[slot],
                           &state->nuttx[slot]);
      nbootctl_slot_export(&record->domains[1].slots[slot],
                           &state->amp[slot]);
    }

  close_blockdriver(inode);
  free(records);
  return 0;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_status
 ****************************************************************************/

int nbootctl_bootctrl_status(unsigned int medium)
{
  struct nbootctl_record_s *records;
  struct nbootctl_record_s *record;
  struct inode *inode = NULL;
  int selected;
  int domain;
  int slot;
  int ret;

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  if (records == NULL)
    {
      return -ENOMEM;
    }

  ret = nbootctl_read_records(medium, &inode, records, &selected);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: bootctrl read failed: %d\n", ret);
      free(records);
      return ret;
    }

  record = &records[selected];
  printf("bootctrl_generation=%llu\n", (unsigned long long)record->generation);
  for (domain = 0; domain < 2; domain++)
    {
      const char *name = domain == 0 ? "nuttx" : "amp";

      printf("%s_active=%c\n", name,
             record->domains[domain].active_slot ? 'b' : 'a');
      for (slot = 0; slot < 2; slot++)
        {
          const struct nbootctl_slot_s *entry =
              &record->domains[domain].slots[slot];

          printf("%s_%c priority=%u successful=%u size=%llu version=%llu\n",
                 name, slot ? 'b' : 'a', entry->priority, entry->successful,
                 (unsigned long long)entry->image_size,
                 (unsigned long long)entry->image_version);
        }
    }

  close_blockdriver(inode);
  free(records);
  return 0;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_request
 ****************************************************************************/

int nbootctl_bootctrl_request(unsigned int medium, unsigned int target)
{
  struct nbootctl_record_s *records;
  struct inode *inode = NULL;
  uint32_t request = target ? NBOOTCTL_REBOOT_MAGIC | target : 0;
  int selected;
  int ret;

  if (target > 4)
    {
      return -EINVAL;
    }

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  if (records == NULL)
    {
      return -ENOMEM;
    }

  ret = nbootctl_read_records(medium, &inode, records, &selected);
  if (ret == 0)
    {
      memcpy(records[selected].padding, &request, sizeof(request));
      ret = nbootctl_write_records(inode, records, selected);
    }

  if (inode != NULL)
    {
      close_blockdriver(inode);
    }

  free(records);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_verify
 ****************************************************************************/

int nbootctl_bootctrl_verify(unsigned int medium, const char *domain,
                             unsigned int slot)
{
  struct nbootctl_record_s *records;
  struct nbootctl_slot_s *entry;
  struct inode *control = NULL;
  struct inode *payload = NULL;
  SHA2_CTX context;
  uint8_t digest[SHA256_DIGEST_LENGTH];
  uint8_t *buffer;
  uint64_t remaining;
  size_t bytes;
  size_t sectors;
  int selected;
  int domain_index;
  int ret;

  domain_index = nbootctl_domain_index(domain);
  if (domain_index < 0 || slot > 1)
    {
      return -EINVAL;
    }

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  buffer = memalign(64, NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (records == NULL || buffer == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = nbootctl_read_records(medium, &control, records, &selected);
  if (ret < 0)
    {
      goto out;
    }

  entry = &records[selected].domains[domain_index].slots[slot];
  if (entry->image_size == 0)
    {
      ret = -ENOENT;
      goto out;
    }

  ret = open_blockdriver(nbootctl_slot_path(medium, domain_index, slot), 0,
                         &payload);
  if (ret < 0 || payload->u.i_bops->read == NULL)
    {
      ret = ret < 0 ? ret : -ENOSYS;
      goto out;
    }

  remaining = entry->image_size;
  sha256init(&context);
  while (remaining > 0)
    {
      bytes = remaining > NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  ? NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  : (size_t)remaining;
      sectors = (bytes + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
      if (payload->u.i_bops->read(payload, buffer,
                                  (entry->image_size - remaining) /
                                      NBOOTCTL_SECTOR_SIZE,
                                  sectors) != (ssize_t)sectors)
        {
          ret = -EIO;
          goto out;
        }

      sha256update(&context, buffer, bytes);
      remaining -= bytes;
    }

  sha256final(digest, &context);
  ret = memcmp(digest, entry->sha256, sizeof(digest)) == 0 ? 0 : -EBADMSG;

out:
  if (payload != NULL)
    {
      close_blockdriver(payload);
    }

  if (control != NULL)
    {
      close_blockdriver(control);
    }

  free(buffer);
  free(records);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_update
 ****************************************************************************/

static int nbootctl_bootctrl_update(unsigned int medium, const char *domain,
                                    unsigned int slot, bool mark_successful)
{
  struct nbootctl_record_s *records;
  struct nbootctl_domain_s *entry;
  struct inode *inode = NULL;
  int selected;
  int domain_index;
  int ret;

  domain_index = nbootctl_domain_index(domain);
  if (domain_index < 0 || slot > 1)
    {
      return -EINVAL;
    }

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  if (records == NULL)
    {
      return -ENOMEM;
    }

  ret = nbootctl_read_records(medium, &inode, records, &selected);
  if (ret < 0)
    {
      goto out;
    }

  entry = &records[selected].domains[domain_index];
  if (entry->slots[slot].image_size == 0)
    {
      ret = -ENOENT;
      goto out;
    }

  if (mark_successful)
    {
      entry->slots[slot].successful = 1;
      entry->slots[slot].tries_remaining = 0;
    }
  else
    {
      entry->active_slot = slot;
      entry->slots[slot].priority = 15;
      if (entry->slots[1 - slot].priority >= 15)
        {
          entry->slots[1 - slot].priority = 14;
        }
    }

  ret = nbootctl_write_records(inode, records, selected);

out:
  if (inode != NULL)
    {
      close_blockdriver(inode);
    }

  free(records);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_set_active
 ****************************************************************************/

int nbootctl_bootctrl_set_active(unsigned int medium, const char *domain,
                                 unsigned int slot)
{
  return nbootctl_bootctrl_update(medium, domain, slot, false);
}

/****************************************************************************
 * Name: nbootctl_bootctrl_mark_successful
 ****************************************************************************/

int nbootctl_bootctrl_mark_successful(unsigned int medium, const char *domain,
                                      unsigned int slot)
{
  return nbootctl_bootctrl_update(medium, domain, slot, true);
}

/****************************************************************************
 * Name: nbootctl_bootctrl_stage
 ****************************************************************************/

int nbootctl_bootctrl_stage(unsigned int medium, const char *domain,
                            unsigned int running_slot, const char *path)
{
  struct nbootctl_record_s *records;
  struct nbootctl_domain_s *domain_entry;
  struct nbootctl_slot_s *slot_entry;
  struct inode *control = NULL;
  struct inode *payload = NULL;
  struct geometry geometry;
  struct stat file_info;
  SHA2_CTX write_hash;
  SHA2_CTX read_hash;
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint8_t *buffer;
  uint64_t offset;
  uint64_t version;
  size_t bytes;
  size_t sectors;
  int selected;
  int target;
  int domain_index;
  int source = -1;
  int ret;

  domain_index = nbootctl_domain_index(domain);
  if (domain_index < 0 || stat(path, &file_info) < 0 || file_info.st_size <= 0)
    {
      return -EINVAL;
    }

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  buffer = memalign(64, NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (records == NULL || buffer == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = nbootctl_read_records(medium, &control, records, &selected);
  if (ret < 0)
    {
      goto out;
    }

  domain_entry = &records[selected].domains[domain_index];
  /* Never the slot this image came from.  A domain nothing runs from keeps
   * the slot N-Boot would take next: that one is the way back if the new
   * image turns out not to start.
   */

  target = running_slot <= 1 ? 1 - (int)running_slot
                             : 1 - (domain_entry->active_slot ? 1 : 0);
  ret = open_blockdriver(nbootctl_slot_path(medium, domain_index, target), 0,
                         &payload);
  if (ret < 0 || payload->u.i_bops->read == NULL ||
      payload->u.i_bops->write == NULL || payload->u.i_bops->geometry == NULL)
    {
      ret = ret < 0 ? ret : -ENOSYS;
      goto out;
    }

  ret = payload->u.i_bops->geometry(payload, &geometry);
  if (ret < 0 || !geometry.geo_available || !geometry.geo_writeenabled ||
      geometry.geo_sectorsize != NBOOTCTL_SECTOR_SIZE ||
      (uint64_t)file_info.st_size >
          (uint64_t)geometry.geo_nsectors * geometry.geo_sectorsize)
    {
      ret = ret < 0 ? ret : -EFBIG;
      goto out;
    }

  source = open(path, O_RDONLY);
  if (source < 0)
    {
      ret = -errno;
      goto out;
    }

  /* A partially replaced payload must not remain a boot candidate. */

  domain_entry->slots[target].priority = 0;
  domain_entry->slots[target].successful = 0;
  ret = nbootctl_write_records(control, records, selected);
  if (ret < 0)
    {
      goto out;
    }

  sha256init(&write_hash);
  for (offset = 0; offset < (uint64_t)file_info.st_size; offset += bytes)
    {
      bytes = (uint64_t)file_info.st_size - offset >
                      NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  ? NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  : (size_t)((uint64_t)file_info.st_size - offset);
      sectors = (bytes + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
      memset(buffer, 0, sectors * NBOOTCTL_SECTOR_SIZE);
      if (read(source, buffer, bytes) != (ssize_t)bytes)
        {
          ret = -EIO;
          goto out;
        }

      sha256update(&write_hash, buffer, bytes);
      if (payload->u.i_bops->write(payload, buffer,
                                   offset / NBOOTCTL_SECTOR_SIZE,
                                   sectors) != (ssize_t)sectors)
        {
          ret = -EIO;
          goto out;
        }
    }

  sha256final(expected, &write_hash);
  sha256init(&read_hash);
  for (offset = 0; offset < (uint64_t)file_info.st_size; offset += bytes)
    {
      bytes = (uint64_t)file_info.st_size - offset >
                      NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  ? NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  : (size_t)((uint64_t)file_info.st_size - offset);
      sectors = (bytes + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
      if (payload->u.i_bops->read(payload, buffer,
                                  offset / NBOOTCTL_SECTOR_SIZE,
                                  sectors) != (ssize_t)sectors)
        {
          ret = -EIO;
          goto out;
        }

      sha256update(&read_hash, buffer, bytes);
    }

  sha256final(actual, &read_hash);
  if (memcmp(expected, actual, sizeof(actual)) != 0)
    {
      ret = -EBADMSG;
      goto out;
    }

  slot_entry = &domain_entry->slots[target];
  version = domain_entry->slots[0].image_version;
  if (domain_entry->slots[1].image_version > version)
    {
      version = domain_entry->slots[1].image_version;
    }

  slot_entry->priority = 15;
  slot_entry->tries_remaining = 0;
  slot_entry->successful = 0;
  slot_entry->image_size = file_info.st_size;
  slot_entry->image_version = version + 1;
  memcpy(slot_entry->sha256, expected, sizeof(expected));
  domain_entry->active_slot = target;
  if (domain_entry->slots[1 - target].priority >= 15)
    {
      domain_entry->slots[1 - target].priority = 14;
    }

  ret = nbootctl_write_records(control, records, selected);
  if (ret == 0)
    {
      printf("stage %s %c: %lld bytes, version %llu, activated\n", domain,
             target ? 'b' : 'a', (long long)file_info.st_size,
             (unsigned long long)slot_entry->image_version);
    }

out:
  if (source >= 0)
    {
      close(source);
    }

  if (payload != NULL)
    {
      close_blockdriver(payload);
    }

  if (control != NULL)
    {
      close_blockdriver(control);
    }

  free(buffer);
  free(records);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_clone
 ****************************************************************************/

int nbootctl_bootctrl_clone(unsigned int medium, const char *domain,
                            unsigned int source, unsigned int target)
{
  struct nbootctl_record_s *records;
  struct nbootctl_domain_s *domain_entry;
  struct nbootctl_slot_s *source_entry;
  struct inode *control = NULL;
  struct inode *source_inode = NULL;
  struct inode *target_inode = NULL;
  SHA2_CTX source_hash;
  SHA2_CTX target_hash;
  uint8_t source_digest[NBOOTCTL_SHA256_SIZE];
  uint8_t target_digest[NBOOTCTL_SHA256_SIZE];
  uint8_t target_priority;
  uint8_t *buffer;
  uint64_t offset;
  uint64_t size;
  size_t bytes;
  size_t sectors;
  int selected;
  int domain_index;
  int ret;

  domain_index = nbootctl_domain_index(domain);
  if (domain_index < 0 || source > 1 || target > 1 || source == target)
    {
      return -EINVAL;
    }

  ret = nbootctl_bootctrl_verify(medium, domain, source);
  if (ret < 0)
    {
      return ret;
    }

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  buffer = memalign(64, NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (records == NULL || buffer == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = nbootctl_read_records(medium, &control, records, &selected);
  if (ret < 0)
    {
      goto out;
    }

  domain_entry = &records[selected].domains[domain_index];
  source_entry = &domain_entry->slots[source];
  size = source_entry->image_size;
  if (size == 0)
    {
      ret = -ENOENT;
      goto out;
    }

  ret = open_blockdriver(nbootctl_slot_path(medium, domain_index, source), 0,
                         &source_inode);
  if (ret < 0)
    {
      goto out;
    }

  ret = open_blockdriver(nbootctl_slot_path(medium, domain_index, target), 0,
                         &target_inode);
  if (ret < 0 || source_inode->u.i_bops->read == NULL ||
      target_inode->u.i_bops->read == NULL ||
      target_inode->u.i_bops->write == NULL)
    {
      ret = ret < 0 ? ret : -ENOSYS;
      goto out;
    }

  target_priority = domain_entry->slots[target].priority;
  domain_entry->slots[target].priority = 0;
  domain_entry->slots[target].successful = 0;
  ret = nbootctl_write_records(control, records, selected);
  if (ret < 0)
    {
      goto out;
    }

  sha256init(&source_hash);
  for (offset = 0; offset < size; offset += bytes)
    {
      bytes = size - offset > NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  ? NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  : (size_t)(size - offset);
      sectors = (bytes + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
      if (source_inode->u.i_bops->read(source_inode, buffer,
                                       offset / NBOOTCTL_SECTOR_SIZE,
                                       sectors) != (ssize_t)sectors)
        {
          ret = -EIO;
          goto out;
        }

      sha256update(&source_hash, buffer, bytes);
      if (target_inode->u.i_bops->write(target_inode, buffer,
                                        offset / NBOOTCTL_SECTOR_SIZE,
                                        sectors) != (ssize_t)sectors)
        {
          ret = -EIO;
          goto out;
        }
    }

  sha256final(source_digest, &source_hash);
  if (memcmp(source_digest, source_entry->sha256, sizeof(source_digest)) != 0)
    {
      ret = -EBADMSG;
      goto out;
    }

  sha256init(&target_hash);
  for (offset = 0; offset < size; offset += bytes)
    {
      bytes = size - offset > NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  ? NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  : (size_t)(size - offset);
      sectors = (bytes + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
      if (target_inode->u.i_bops->read(target_inode, buffer,
                                       offset / NBOOTCTL_SECTOR_SIZE,
                                       sectors) != (ssize_t)sectors)
        {
          ret = -EIO;
          goto out;
        }

      sha256update(&target_hash, buffer, bytes);
    }

  sha256final(target_digest, &target_hash);
  if (memcmp(source_digest, target_digest, sizeof(source_digest)) != 0)
    {
      ret = -EBADMSG;
      goto out;
    }

  domain_entry->slots[target] = *source_entry;
  domain_entry->slots[target].priority = target_priority;
  domain_entry->slots[target].successful = 0;

  ret = nbootctl_write_records(control, records, selected);
  if (ret == 0)
    {
      printf("clone %s %c -> %c: %llu bytes, verified\n", domain,
             source ? 'b' : 'a', target ? 'b' : 'a', (unsigned long long)size);
    }

out:
  if (target_inode != NULL)
    {
      close_blockdriver(target_inode);
    }

  if (source_inode != NULL)
    {
      close_blockdriver(source_inode);
    }

  if (control != NULL)
    {
      close_blockdriver(control);
    }

  free(buffer);
  free(records);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_update_nboot
 ****************************************************************************/

int nbootctl_update_nboot(unsigned int medium, const char *path)
{
  const char *disk_path = nbootctl_disk_path(medium);
  struct inode *disk = NULL;
  struct stat file_info;
  uint8_t magic[4];
  uint8_t *buffer;
  uint8_t *verify;
  uint64_t offset;
  size_t bytes;
  size_t sectors;
  int source = -1;
  int ret;

  if (disk_path == NULL || stat(path, &file_info) < 0 ||
      file_info.st_size <= 0 ||
      file_info.st_size > NBOOTCTL_UBOOT_SECTORS * NBOOTCTL_SECTOR_SIZE)
    {
      return -EINVAL;
    }

  source = open(path, O_RDONLY);
  if (source < 0 || read(source, magic, sizeof(magic)) != sizeof(magic) ||
      nbootctl_be32(magic) != NBOOTCTL_FIT_MAGIC ||
      lseek(source, 0, SEEK_SET) < 0)
    {
      ret = source < 0 ? -errno : -EINVAL;
      goto out;
    }

  buffer = memalign(64, NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE);
  verify = memalign(64, NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (buffer == NULL || verify == NULL)
    {
      ret = -ENOMEM;
      goto out_buffers;
    }

  ret = open_blockdriver(disk_path, 0, &disk);
  if (ret < 0 || disk->u.i_bops->read == NULL || disk->u.i_bops->write == NULL)
    {
      ret = ret < 0 ? ret : -ENOSYS;
      goto out_buffers;
    }

  for (offset = 0; offset < (uint64_t)file_info.st_size; offset += bytes)
    {
      bytes = (uint64_t)file_info.st_size - offset >
                      NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  ? NBOOTCTL_VERIFY_SECTORS * NBOOTCTL_SECTOR_SIZE
                  : (size_t)((uint64_t)file_info.st_size - offset);
      sectors = (bytes + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
      memset(buffer, 0, sectors * NBOOTCTL_SECTOR_SIZE);
      if (read(source, buffer, bytes) != (ssize_t)bytes ||
          disk->u.i_bops->write(disk, buffer,
                                NBOOTCTL_UBOOT_START +
                                    offset / NBOOTCTL_SECTOR_SIZE,
                                sectors) != (ssize_t)sectors ||
          disk->u.i_bops->read(disk, verify,
                               NBOOTCTL_UBOOT_START +
                                   offset / NBOOTCTL_SECTOR_SIZE,
                               sectors) != (ssize_t)sectors ||
          memcmp(buffer, verify, sectors * NBOOTCTL_SECTOR_SIZE) != 0)
        {
          ret = -EIO;
          goto out_buffers;
        }
    }

  printf("update-nboot: %lld bytes written and verified on %s\n",
         (long long)file_info.st_size, medium == 1 ? "sd" : "emmc");
  ret = 0;

out_buffers:
  if (disk != NULL)
    {
      close_blockdriver(disk);
    }

  free(verify);
  free(buffer);
out:
  if (source >= 0)
    {
      close(source);
    }

  return ret;
}
