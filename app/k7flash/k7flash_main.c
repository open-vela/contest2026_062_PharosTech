/****************************************************************************
 * app/k7flash/k7flash_main.c
 *
 * KICKPI-K7 on-board firmware hot-update flasher. Writes a new firmware
 * image (a Rockchip FIT, i.e. the uboot_nuttx.img produced by build_sd)
 * into the SD card uboot slot, reads it back to verify, then reboots to
 * take effect.
 *
 * Safety guardrails (mandatory):
 *   1. Target block device is hardcoded to "/dev/mmcsd0" (SD card) and the
 *      start sector is hardcoded to 16384 (uboot slot). The command line
 *      only accepts <file>; no device/offset argument is taken -- no
 *      whole-disk default, no wildcard.
 *   2. The eMMC host is never instantiated in this BSP, so the code cannot
 *      physically reach the eMMC (0x2A330000).
 *   3. The write range never crosses into the trust slot (sector 24576),
 *      and never touches idbloader (sector 64) or the GPT (sector 0).
 *   4. Before writing, verify it is a valid FIT (magic 0xd00dfeed); after
 *      writing, read back and verify sector by sector.
 *   5. On any failure: keep running, warn, and do NOT reboot (avoid a
 *      half-written brick).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <nuttx/config.h>
#include <nuttx/fs/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/boardctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef CONFIG_FS_TMPFS
#include <sys/mount.h>
#endif

/****************************************************************************
 * Guardrail constants (hardcoded, not overridable from command line)
 ****************************************************************************/

#define K7_TARGET_DEV   "/dev/mmcsd0" /* Write SD card only, never eMMC */
#define K7_UBOOT_SECTOR 16384         /* uboot slot start sector */
#define K7_TRUST_SECTOR                 \
  24576 /* trust slot start (write must \
         * not cross this) */
#define K7_SECTOR_SIZE 512
#define K7_FIT_MAGIC   0xd00dfeedu /* Rockchip/U-Boot FIT(FDT) magic */
#define K7_MAX_BYTES   ((K7_TRUST_SECTOR - K7_UBOOT_SECTOR) * K7_SECTOR_SIZE)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t k7_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/****************************************************************************
 * k7flash_main
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct inode *bnode = NULL;
  uint8_t *buf = NULL;
  uint8_t *vbuf = NULL;
  const char *path;
  struct stat st;
  size_t nbytes;
  size_t nsectors;
  ssize_t rn;
  int fd = -1;
  int ret;

  if (argc != 2)
    {
      fprintf(stderr,
              "usage: k7flash <fw.img>  (firmware = uboot_nuttx.img/FIT "
              "produced by build_sd)\n");
      return 1;
    }

    /* Mount tmpfs at /tmp so this tool is self-contained: the Ymodem receiver
     * (rb) drops the firmware here and k7flash reads it back.  The mount is
     * idempotent, so a re-run (or a board that already mounted it) is fine.
     */

#ifdef CONFIG_FS_TMPFS
  mount(NULL, "/tmp", "tmpfs", 0, NULL);
#endif

  path = argv[1];

  /* 1) Read the firmware into memory */

  if (stat(path, &st) < 0)
    {
      fprintf(stderr, "error: cannot open %s: %d\n", path, errno);
      return 1;
    }

  nbytes = (size_t)st.st_size;
  if (nbytes == 0 || nbytes > K7_MAX_BYTES)
    {
      fprintf(stderr,
              "error: invalid firmware size %zu (must be 1..%d bytes, "
              "i.e. must not cross the trust slot)\n",
              nbytes, K7_MAX_BYTES);
      return 1;
    }

  nsectors = (nbytes + K7_SECTOR_SIZE - 1) / K7_SECTOR_SIZE;

  /* Align the buffer to a cache line (64B) to satisfy the SDMMC IDMAC
   * dmapreflight check and take the multi-block DMA path; otherwise the
   * driver automatically falls back to single-block PIO (still works,
   * just slower).
   */

  buf = memalign(64, nsectors * K7_SECTOR_SIZE);
  vbuf = memalign(64, nsectors * K7_SECTOR_SIZE);
  if (buf == NULL || vbuf == NULL)
    {
      fprintf(stderr, "error: out of memory\n");
      goto errout;
    }

  memset(buf, 0, nsectors * K7_SECTOR_SIZE); /* zero-pad last sector */
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "error: open %s: %d\n", path, errno);
      goto errout;
    }

  if (read(fd, buf, nbytes) != (ssize_t)nbytes)
    {
      fprintf(stderr, "error: incomplete firmware read\n");
      goto errout;
    }

  close(fd);
  fd = -1;

  /* 2) Verify it is a valid FIT (magic) */

  if (k7_be32(buf) != K7_FIT_MAGIC)
    {
      fprintf(stderr,
              "error: not a valid FIT (magic=%08x, expected %08x), "
              "refusing to write\n",
              k7_be32(buf), K7_FIT_MAGIC);
      goto errout;
    }

  /* 3) Get the block device (hardcoded SD, no argument accepted) */

  ret = find_blockdriver(K7_TARGET_DEV, 0, &bnode);
  if (ret < 0 || bnode->u.i_bops->write == NULL ||
      bnode->u.i_bops->read == NULL)
    {
      fprintf(stderr, "error: no readable/writable block device %s\n",
              K7_TARGET_DEV);
      goto errout;
    }

  printf("k7flash: writing %zu bytes (%zu sectors) to %s @sector %d ...\n",
         nbytes, nsectors, K7_TARGET_DEV, K7_UBOOT_SECTOR);

  /* 4) Multi-block write (buffer is cache-line aligned, so the SDMMC IDMAC
   * moves multiple sectors at once; when unaligned the driver falls back
   * to single-block PIO automatically).
   */

  rn = bnode->u.i_bops->write(bnode, buf, K7_UBOOT_SECTOR, nsectors);
  if (rn != (ssize_t)nsectors)
    {
      fprintf(stderr, "error: write failed (rn=%zd, expected %zu)\n", rn,
              nsectors);
      goto errout;
    }

  /* 5) Multi-block read-back verification */

  rn = bnode->u.i_bops->read(bnode, vbuf, K7_UBOOT_SECTOR, nsectors);
  if (rn != (ssize_t)nsectors || memcmp(buf, vbuf, nbytes) != 0)
    {
      fprintf(stderr,
              "error: verification failed (rn=%zd) -- not rebooting, "
              "please retry\n",
              rn);
      goto errout;
    }

  free(buf);
  free(vbuf);
  buf = NULL;
  vbuf = NULL;

  /* 6) Update succeeded: remove the temporary firmware to reclaim tmpfs
   * space (/tmp is RAM).
   */

  if (unlink(path) == 0)
    {
      printf("k7flash: cleaned up %s, space reclaimed\n", path);
    }

  printf("k7flash: write + verify passed. Rebooting in 3 seconds ...\n");
  fflush(stdout);
  sleep(3);

  /* 6) Reboot -> boot into the new firmware */

#ifdef CONFIG_BOARDCTL_RESET
  boardctl(BOARDIOC_RESET, 0);
#else
  printf("k7flash: BOARDCTL_RESET not enabled, please reset manually\n");
#endif
  return 0;

errout:
  if (fd >= 0)
    {
      close(fd);
    }

  if (bnode != NULL)
    {
      close_blockdriver(bnode);
    }

  free(buf);
  free(vbuf);
  return 1;
}
