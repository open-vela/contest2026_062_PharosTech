/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_storage.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/partition.h>
#include <nuttx/sdio.h>
#include <nuttx/usb/usbhost.h>
#include <nuttx/wqueue.h>

#include "kickpi_k7.h"
#include "rk3576_sdmmc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KICKPI_K7_STORAGE_MAX_PARTITIONS 16
#define KICKPI_K7_STORAGE_PATH_MAX       32
#define KICKPI_K7_STORAGE_SETTLE_MS      500
#define KICKPI_K7_STORAGE_RETRY_MS       1000
#define KICKPI_K7_STORAGE_RETRIES        10

#define KICKPI_K7_STORAGE_DATA_MOUNT     "/data"
#define KICKPI_K7_STORAGE_CONFIG_MOUNT   "/config"
#define KICKPI_K7_STORAGE_SD_MOUNT       "/sd"
#define KICKPI_K7_STORAGE_EMMC_MOUNT     "/emmc"
#define KICKPI_K7_STORAGE_USB_MOUNT      "/usb"
#define KICKPI_K7_STORAGE_PERSIST_TMP    KICKPI_K7_STORAGE_DATA_MOUNT "/tmp"

/* Partition holding provisioning, identity and persona.  It is mounted
 * separately from the bulk store and is never a candidate for /data: if it
 * were, a medium whose data partition was missing or unreadable would
 * mount the configuration in its place, and the device would look
 * provisioned while having nowhere to put a model.
 */
#define KICKPI_K7_STORAGE_CONFIG_PART "config"
#define KICKPI_K7_STORAGE_DATA_SCORE  100

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kickpi_k7_storage_candidate_s
{
  char path[KICKPI_K7_STORAGE_PATH_MAX];
  int score;
  size_t nblocks;
};

struct kickpi_k7_storage_media_s
{
  FAR const char *name;
  FAR const char *blockdev;
  FAR const char *secondary_mount;
  FAR struct sdio_dev_s *sdio;
  struct kickpi_k7_storage_candidate_s
      candidates[KICKPI_K7_STORAGE_MAX_PARTITIONS];
  size_t ncandidates;
  bool removable;
  bool mounted;
  bool primary;
  bool fixed_mount;
  uint8_t retries;
  char mountpoint[KICKPI_K7_STORAGE_PATH_MAX];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct kickpi_k7_storage_media_s g_sd_media = {
  .name = "sd",
  .blockdev = "/dev/mmcsd0",
  .secondary_mount = KICKPI_K7_STORAGE_SD_MOUNT,
  .removable = true,
};

static struct kickpi_k7_storage_media_s g_emmc_media = {
  .name = "emmc",
  .blockdev = "/dev/mmcsd1",
  .secondary_mount = KICKPI_K7_STORAGE_EMMC_MOUNT,
};

#ifdef CONFIG_USBHOST_MSC_NOTIFIER
static struct kickpi_k7_storage_media_s g_usb_media = {
  .name = "usb",
  .blockdev = "/dev/sda",
  .secondary_mount = KICKPI_K7_STORAGE_USB_MOUNT,
  .removable = true,
  .fixed_mount = true,
};
#endif

static FAR struct kickpi_k7_storage_media_s *g_primary_media;
static struct work_s g_sd_work;
static struct work_s g_start_work;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool kickpi_k7_storage_system_partition(FAR const char *name)
{
  static FAR const char *const system_names[] = {
    "boot", "trust",    "uboot",  "loader", "idblock",
    "misc", "recovery", "vbmeta", "rpmb",
  };
  size_t i;

  for (i = 0; i < sizeof(system_names) / sizeof(system_names[0]); i++)
    {
      if (strcasecmp(name, system_names[i]) == 0)
        {
          return true;
        }
    }

  return false;
}

static int kickpi_k7_storage_partition_score(FAR const char *name)
{
  if (strcasecmp(name, "data") == 0)
    {
      return KICKPI_K7_STORAGE_DATA_SCORE;
    }

  if (strcasecmp(name, "userdata") == 0)
    {
      return 90;
    }

  if (strcasecmp(name, "rootfs") == 0)
    {
      return 80;
    }

  return name[0] == '\0' ? 20 : 10;
}

static void kickpi_k7_storage_partition_handler(FAR struct partition_s *part,
                                                FAR void *arg)
{
  FAR struct kickpi_k7_storage_media_s *media = arg;
  FAR struct kickpi_k7_storage_candidate_s *candidate;
  char path[KICKPI_K7_STORAGE_PATH_MAX];
  int ret;

  if (part->index >= KICKPI_K7_STORAGE_MAX_PARTITIONS ||
      kickpi_k7_storage_system_partition(part->name) ||
      strcasecmp(part->name, KICKPI_K7_STORAGE_CONFIG_PART) == 0)
    {
      return;
    }

  snprintf(path, sizeof(path), "%sp%u", media->blockdev,
           (unsigned int)part->index + 1);
  unregister_blockdriver(path);
  ret = register_blockpartition(path, 0660, media->blockdev, part->firstblock,
                                part->nblocks);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: storage: register %s failed: %d\n", path,
             ret);
      return;
    }

  candidate = &media->candidates[media->ncandidates++];
  strlcpy(candidate->path, path, sizeof(candidate->path));
  candidate->score = kickpi_k7_storage_partition_score(part->name);
  candidate->nblocks = part->nblocks;

  syslog(LOG_INFO, "INFO: storage: %s partition %s label=%s blocks=%lu\n",
         media->name, path, part->name[0] == '\0' ? "<none>" : part->name,
         (unsigned long)part->nblocks);
}

static FAR struct kickpi_k7_storage_candidate_s *
kickpi_k7_storage_best_candidate(FAR struct kickpi_k7_storage_media_s *media)
{
  FAR struct kickpi_k7_storage_candidate_s *best = NULL;
  size_t i;

  for (i = 0; i < media->ncandidates; i++)
    {
      FAR struct kickpi_k7_storage_candidate_s *candidate =
          &media->candidates[i];

      if (candidate->score >= 0 &&
          (best == NULL || candidate->score > best->score ||
           (candidate->score == best->score &&
            candidate->nblocks > best->nblocks)))
        {
          best = candidate;
        }
    }

  return best;
}

static void kickpi_k7_storage_publish_tmp(void)
{
  int ret;

  ret = mkdir(KICKPI_K7_STORAGE_PERSIST_TMP, 0770);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_WARNING, "WARNING: storage: mkdir %s failed: %d\n",
             KICKPI_K7_STORAGE_PERSIST_TMP, errno);
      return;
    }

  syslog(LOG_INFO, "INFO: storage: persistent temporary directory is %s\n",
         KICKPI_K7_STORAGE_PERSIST_TMP);
}

static void kickpi_k7_storage_remove_partitions(
    FAR struct kickpi_k7_storage_media_s *media)
{
  size_t i;

  for (i = 0; i < media->ncandidates; i++)
    {
      unregister_blockdriver(media->candidates[i].path);
    }

  media->ncandidates = 0;
}

/****************************************************************************
 * Name: kickpi_k7_storage_partition_is_blank
 *
 * Description:
 *   Report whether a block device has never been written to.
 *
 *   Reads the first sector and looks for anything non-zero.  This is what
 *   separates "no filesystem was ever created here" from "a filesystem is
 *   here and cannot be read" -- both make mount() fail, but only the first
 *   is safe to reformat without a human deciding.
 *
 ****************************************************************************/

static bool kickpi_k7_storage_partition_is_blank(FAR const char *path)
{
  uint8_t sector[512];
  struct inode *inode = NULL;
  ssize_t nread;
  size_t i;
  int ret;

  ret = open_blockdriver(path, 0, &inode);
  if (ret < 0)
    {
      return false;
    }

  nread = inode->u.i_bops->read(inode, sector, 0, 1);
  close_blockdriver(inode);

  if (nread < 1)
    {
      return false;
    }

  for (i = 0; i < sizeof(sector); i++)
    {
      if (sector[i] != 0)
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: kickpi_k7_storage_format
 *
 * Description:
 *   Create a FAT filesystem on a block device.
 *
 *   The formatter lives in the application tree, and a board source file
 *   has no business depending on that direction: it would drag the apps
 *   directory into every configuration that uses this board, including the
 *   ones that exclude it.  The hook is weak so a build without a formatter
 *   still links and simply reports that the partition could not be
 *   prepared; the strong definition lives with the tool that also exposes
 *   the operation to an operator.
 *
 ****************************************************************************/

int __attribute__((weak)) kickpi_k7_storage_format_hook(FAR const char *path)
{
  (void)path;
  return -ENOSYS;
}

static int kickpi_k7_storage_format(FAR const char *path)
{
  return kickpi_k7_storage_format_hook(path);
}

/****************************************************************************
 * Name: kickpi_k7_storage_config_find
 *
 * Description:
 *   Record the config partition's geometry.  Used as the callback for
 *   parse_block_partition() when locating it, so that nothing is
 *   registered as a side effect of the search.
 *
 ****************************************************************************/

struct kickpi_k7_storage_config_lookup_s
{
  struct partition_s part;
  bool found;
};

static void kickpi_k7_storage_config_find(FAR struct partition_s *part,
                                          FAR void *arg)
{
  FAR struct kickpi_k7_storage_config_lookup_s *lookup = arg;

  if (strcasecmp(part->name, KICKPI_K7_STORAGE_CONFIG_PART) == 0)
    {
      memcpy(&lookup->part, part, sizeof(*part));
      lookup->found = true;
    }
}

/****************************************************************************
 * Name: kickpi_k7_storage_config_mount
 *
 * Description:
 *   Mount the configuration partition at /config.
 *
 *   Unlike the bulk store this is not a candidate for /data: it holds the
 *   provisioning state, and mounting it in place of a missing data
 *   partition would leave the device apparently provisioned with nowhere
 *   to put anything.
 *
 *   If the partition carries no filesystem yet -- which is what a factory
 *   image built with an empty config produces -- one is created.  That is
 *   the only case in which anything is written here, and it is decided by
 *   mount() failing with EINVAL rather than by inspecting the contents: a
 *   filesystem that exists but cannot be read is a different problem, and
 *   formatting it would destroy settings rather than repair them.
 *
 ****************************************************************************/

static int kickpi_k7_storage_config_mount(void)
{
  char path[KICKPI_K7_STORAGE_PATH_MAX];
  struct kickpi_k7_storage_config_lookup_s lookup;
  int ret;

  /* Walk the table looking for the config partition only.  Registering
   * every partition again is not an option: the bulk store's driver nodes
   * were created when it mounted, and a second pass would collide with
   * them.
   */

  memset(&lookup, 0, sizeof(lookup));
  ret = parse_block_partition(g_emmc_media.blockdev,
                              kickpi_k7_storage_config_find, &lookup);
  if (ret < 0)
    {
      return ret;
    }

  if (!lookup.found)
    {
      syslog(LOG_INFO, "INFO: storage: no %s partition\n",
             KICKPI_K7_STORAGE_CONFIG_PART);
      return -ENOENT;
    }

  snprintf(path, sizeof(path), "%sp%u", g_emmc_media.blockdev,
           (unsigned int)lookup.part.index + 1);
  unregister_blockdriver(path);
  ret = register_blockpartition(path, 0660, g_emmc_media.blockdev,
                                lookup.part.firstblock, lookup.part.nblocks);
  if (ret < 0)
    {
      return ret;
    }

  if (mkdir(KICKPI_K7_STORAGE_CONFIG_MOUNT, 0770) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  if (mount(path, KICKPI_K7_STORAGE_CONFIG_MOUNT, "vfat", 0, NULL) == 0)
    {
      return 0;
    }

  /* An unformatted partition is expected on a device whose config was
   * cleared, and a product cannot ask the owner to open a console.  Create
   * the filesystem and retry -- but only when the partition is genuinely
   * blank.
   *
   * The distinction matters.  mount() failing says the volume could not be
   * read, which is equally true of a filesystem that is damaged.  Erasing
   * a damaged config destroys the provisioning someone might still recover;
   * erasing a blank one destroys nothing.  So read the first sector: all
   * zeroes means nothing was ever written here, which is the only case
   * that is safe to reformat automatically.
   */

  if (kickpi_k7_storage_partition_is_blank(path))
    {
      syslog(LOG_INFO,
             "INFO: storage: %s is unformatted, creating a "
             "filesystem\n",
             KICKPI_K7_STORAGE_CONFIG_PART);

      if (kickpi_k7_storage_format(path) == 0 &&
          mount(path, KICKPI_K7_STORAGE_CONFIG_MOUNT, "vfat", 0, NULL) == 0)
        {
          return 0;
        }

      syslog(LOG_WARNING, "WARNING: storage: could not create /config\n");
      return -EIO;
    }

  syslog(LOG_WARNING,
         "WARNING: storage: /config mount failed: %d "
         "(partition is not blank; refusing to reformat)\n",
         errno);
  return -errno;
}

/****************************************************************************
 * Name: kickpi_k7_storage_mount
 ****************************************************************************/

static int kickpi_k7_storage_mount(FAR struct kickpi_k7_storage_media_s *media)
{
  FAR struct kickpi_k7_storage_candidate_s *candidate;
  FAR const char *mountpoint;
  FAR const char *fstype;
  FAR const char *source = media->blockdev;
  bool mountpoint_created = false;
  int ret;

  media->ncandidates = 0;
  ret = parse_block_partition(media->blockdev,
                              kickpi_k7_storage_partition_handler, media);
  if (ret < 0)
    {
      syslog(LOG_INFO, "INFO: storage: %s has no supported partition table\n",
             media->name);
    }

  mountpoint = !media->fixed_mount && g_primary_media == NULL
                   ? KICKPI_K7_STORAGE_DATA_MOUNT
                   : media->secondary_mount;
  fstype = media->fixed_mount ? "fatfs" : "vfat";

  if (mkdir(mountpoint, 0770) == 0)
    {
      mountpoint_created = true;
    }
  else if (errno != EEXIST)
    {
      return -errno;
    }

  while ((candidate = kickpi_k7_storage_best_candidate(media)) != NULL)
    {
      bool labelled_data = candidate->score == KICKPI_K7_STORAGE_DATA_SCORE;

      candidate->score = -1;
      if (mount(candidate->path, mountpoint, fstype, 0, NULL) == 0)
        {
          source = candidate->path;
          goto mounted;
        }

      /* The bulk store is initialised on the board, not by an image: a
       * filesystem written from a host is as large as the image was, not
       * as large as the partition, and the partition is where the models
       * go.  So a partition labelled "data" that has never been written
       * gets a filesystem of its full size here.  Only a blank one: a
       * volume that fails to mount for any other reason may still hold
       * something its owner wants back.
       */

      if (labelled_data && !media->removable &&
          kickpi_k7_storage_partition_is_blank(candidate->path))
        {
          syslog(LOG_INFO,
                 "INFO: storage: %s is unformatted, creating a "
                 "filesystem\n",
                 candidate->path);
          if (kickpi_k7_storage_format(candidate->path) == 0 &&
              mount(candidate->path, mountpoint, fstype, 0, NULL) == 0)
            {
              source = candidate->path;
              goto mounted;
            }

          syslog(LOG_WARNING,
                 "WARNING: storage: could not create a "
                 "filesystem on %s\n",
                 candidate->path);
        }
    }

  if (mount(media->blockdev, mountpoint, fstype, 0, NULL) < 0)
    {
      ret = -errno;
      if (mountpoint_created)
        {
          rmdir(mountpoint);
        }

      return ret;
    }

mounted:
  strlcpy(media->mountpoint, mountpoint, sizeof(media->mountpoint));
  media->mounted = true;
  media->primary = !media->fixed_mount && g_primary_media == NULL;
  if (media->primary)
    {
      g_primary_media = media;
      kickpi_k7_storage_publish_tmp();
    }

  syslog(LOG_INFO, "INFO: storage: mounted %s from %s at %s\n", media->name,
         source, media->mountpoint);
  return OK;
}

static int
kickpi_k7_storage_unmount(FAR struct kickpi_k7_storage_media_s *media)
{
  int ret = OK;

  if (umount2(media->mountpoint, MNT_FORCE) < 0 && errno != ENOENT &&
      errno != EINVAL)
    {
      ret = -errno;
    }

  if (media->primary)
    {
      g_primary_media = NULL;
    }

  kickpi_k7_storage_remove_partitions(media);
  media->mounted = false;
  media->primary = false;
  media->mountpoint[0] = '\0';

  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO, "INFO: storage: unmounted %s\n", media->name);
  return OK;
}

static void kickpi_k7_storage_sd_worker(FAR void *arg)
{
  FAR struct kickpi_k7_storage_media_s *media = arg;
  bool inserted;
  int ret = OK;

  inserted = (SDIO_STATUS(media->sdio) & SDIO_STATUS_PRESENT) != 0;

  if (inserted && !media->mounted)
    {
      ret = kickpi_k7_storage_mount(media);
    }
  else if (!inserted && media->mounted)
    {
      ret = kickpi_k7_storage_unmount(media);
    }
  else if (!inserted)
    {
      kickpi_k7_storage_remove_partitions(media);
    }

  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: storage: %s transition failed: %d\n",
             media->name, ret);
      if ((ret == -ENOENT || ret == -ENODEV || ret == -ENOTBLK ||
           ret == -EAGAIN || ret == -EBUSY || ret == -ETIMEDOUT) &&
          media->retries++ < KICKPI_K7_STORAGE_RETRIES)
        {
          work_queue(LPWORK, &g_sd_work, kickpi_k7_storage_sd_worker, media,
                     MSEC2TICK(KICKPI_K7_STORAGE_RETRY_MS));
        }
    }
  else
    {
      media->retries = 0;
    }
}

static void kickpi_k7_storage_start_worker(FAR void *arg)
{
  int ret;

  (void)arg;

  if (g_emmc_media.sdio != NULL &&
      (SDIO_STATUS(g_emmc_media.sdio) & SDIO_STATUS_PRESENT) != 0)
    {
      ret = kickpi_k7_storage_mount(&g_emmc_media);
      if (ret < 0)
        {
          syslog(LOG_INFO, "INFO: storage: eMMC not mountable: %d\n", ret);
        }

      /* Configuration is mounted whether or not the bulk store came up.
       * The two are independent, and a device that cannot reach its models
       * still needs to know its own name and how to join a network.
       */

      ret = kickpi_k7_storage_config_mount();
      if (ret < 0)
        {
          syslog(LOG_INFO, "INFO: storage: /config not mountable: %d\n", ret);
        }
    }

  if (g_sd_media.sdio != NULL)
    {
      kickpi_k7_storage_sd_worker(&g_sd_media);
    }
}

static void kickpi_k7_storage_sd_event(FAR void *arg, bool inserted)
{
  FAR struct kickpi_k7_storage_media_s *media = arg;

  (void)inserted;
  work_queue(LPWORK, &g_sd_work, kickpi_k7_storage_sd_worker, media,
             MSEC2TICK(KICKPI_K7_STORAGE_SETTLE_MS));
}

#ifdef CONFIG_USBHOST_MSC_NOTIFIER
static void kickpi_k7_storage_usb_connect(FAR void *arg)
{
  FAR struct kickpi_k7_storage_media_s *media = arg;
  int ret;

  usbhost_msc_notifier_setup(kickpi_k7_storage_usb_connect,
                             WORK_USB_MSC_CONNECT, 'a', media);

  if (!media->mounted)
    {
      ret = kickpi_k7_storage_mount(media);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "WARNING: storage: USB mount failed: %d\n", ret);
        }
    }
}

static void kickpi_k7_storage_usb_disconnect(FAR void *arg)
{
  FAR struct kickpi_k7_storage_media_s *media = arg;
  int ret = OK;

  usbhost_msc_notifier_setup(kickpi_k7_storage_usb_disconnect,
                             WORK_USB_MSC_DISCONNECT, 'a', media);

  if (media->mounted)
    {
      ret = kickpi_k7_storage_unmount(media);
    }
  else
    {
      kickpi_k7_storage_remove_partitions(media);
    }

  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: storage: USB unmount failed: %d\n", ret);
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_storage_initialize(FAR struct sdio_dev_s *sdmmc,
                                 FAR struct sdio_dev_s *emmc)
{
  int ret;

#ifdef CONFIG_USBHOST_MSC_NOTIFIER
  ret = usbhost_msc_notifier_setup(kickpi_k7_storage_usb_connect,
                                   WORK_USB_MSC_CONNECT, 'a', &g_usb_media);
  if (ret < 0)
    {
      return ret;
    }

  ret = usbhost_msc_notifier_setup(kickpi_k7_storage_usb_disconnect,
                                   WORK_USB_MSC_DISCONNECT, 'a', &g_usb_media);
  if (ret < 0)
    {
      return ret;
    }
#endif

  if (mount(NULL, "/tmp", "tmpfs", 0, NULL) < 0 && errno != EBUSY)
    {
      syslog(LOG_ERR, "ERROR: storage: mount tmpfs at /tmp failed: %d\n",
             errno);
    }

  g_emmc_media.sdio = emmc;

  g_sd_media.sdio = sdmmc;

#ifdef CONFIG_RK3576_SDMMC
  if (sdmmc != NULL)
    {
      ret = rk3576_sdmmc_register_media_callback(
          sdmmc, kickpi_k7_storage_sd_event, &g_sd_media);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: storage: register SD listener failed: %d\n",
                 ret);
          return ret;
        }
    }
#else
  UNUSED(kickpi_k7_storage_sd_event);
#endif /* CONFIG_RK3576_SDMMC */

  ret = work_queue(LPWORK, &g_start_work, kickpi_k7_storage_start_worker, NULL,
                   MSEC2TICK(KICKPI_K7_STORAGE_SETTLE_MS));
  return ret < 0 ? ret : OK;
}
