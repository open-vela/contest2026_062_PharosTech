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
#define KICKPI_K7_STORAGE_SD_MOUNT       "/sd"
#define KICKPI_K7_STORAGE_EMMC_MOUNT     "/emmc"
#define KICKPI_K7_STORAGE_PERSIST_TMP    KICKPI_K7_STORAGE_DATA_MOUNT "/tmp"

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
      return 100;
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
      kickpi_k7_storage_system_partition(part->name))
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

static int kickpi_k7_storage_mount(FAR struct kickpi_k7_storage_media_s *media)
{
  FAR struct kickpi_k7_storage_candidate_s *candidate;
  FAR const char *mountpoint;
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

  mountpoint = g_primary_media == NULL ? KICKPI_K7_STORAGE_DATA_MOUNT
                                       : media->secondary_mount;

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
      candidate->score = -1;
      if (mount(candidate->path, mountpoint, "vfat", 0, NULL) == 0)
        {
          source = candidate->path;
          goto mounted;
        }
    }

  if (mount(media->blockdev, mountpoint, "vfat", 0, NULL) < 0)
    {
      ret = -errno;
      kickpi_k7_storage_remove_partitions(media);
      if (mountpoint_created)
        {
          rmdir(mountpoint);
        }

      return ret;
    }

mounted:
  strlcpy(media->mountpoint, mountpoint, sizeof(media->mountpoint));
  media->mounted = true;
  media->primary = g_primary_media == NULL;
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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_storage_initialize(FAR struct sdio_dev_s *sdmmc,
                                 FAR struct sdio_dev_s *emmc)
{
  int ret;

  if (mount(NULL, "/tmp", "tmpfs", 0, NULL) < 0 && errno != EBUSY)
    {
      syslog(LOG_ERR, "ERROR: storage: mount tmpfs at /tmp failed: %d\n",
             errno);
    }

  g_emmc_media.sdio = emmc;

  g_sd_media.sdio = sdmmc;
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

  ret = work_queue(LPWORK, &g_start_work, kickpi_k7_storage_start_worker, NULL,
                   MSEC2TICK(KICKPI_K7_STORAGE_SETTLE_MS));
  return ret < 0 ? ret : OK;
}
