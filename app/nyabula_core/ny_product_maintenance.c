/****************************************************************************
 * app/nyabula_core/ny_product_maintenance.c
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

/* What the settings pages of the panel ask about the device itself:
 *
 *   storage.status    any role   volumes and what the known directories use
 *   storage.cleanup   owner      {"target": "tmp"}; empties a clearable one
 *   update.status     any role   the firmware that is running, the A/B
 *                                slots of both domains, what can be
 *                                updated and the progress of an apply
 *   update.apply      owner      {"sha256": hex, "target": id,
 *                                "advanced": bool, "force": bool}; write
 *                                the uploaded image to its target
 *   update.confirm    owner      {"target": "nuttx"|"amp"}; mark the
 *                                running NuttX slot, or the active AMP
 *                                slot, as known good
 *   update.reboot     owner      reply, then reset
 *   logs.tail         owner      {"after": seq, "limit": n}; system log lines
 *   cloud.status      any role   the stored relay settings
 *   cloud.config      owner      {"enabled": bool, "url": "ws[s]://..."}
 *
 * Nothing here pretends: the firmware has no online update service and no
 * relay client, and the answers say so.
 *
 * The update.* topics other than status exist only with NYABULA_CORE_OTA.
 * Without it they fall through as unknown, which is how the panel tells a
 * firmware that cannot be updated this way from one that refused.
 *
 * update.apply writes one of the targets update.status lists, and the list
 * is the whole of what it writes.  Two of them are ordinary: the NuttX
 * firmware and the AMP image, each staged into the slot of its domain that
 * is not in use, with its bootctrl record.  The rest can leave the device
 * unable to start and are refused unless the request says "advanced": true,
 * which the panel only sends after the owner has been told so:
 *
 *   EADVANCED   an advanced target without that acknowledgement
 *   ERUNNING    the partition holds the NuttX image that is running
 *   EMOUNTED    a filesystem is mounted from the partition and the request
 *               does not say "force": true
 *   EBLOCKED    the target is never written from here (the partition the
 *               uploaded file itself lives on)
 *   ENOTIMAGE   the staged file is not what the target takes
 *   ETOOLARGE   the staged file does not fit the target
 *
 * Nothing is ever unmounted to make room for a write.  A forced write goes
 * under the mounted filesystem, and the reply says what that means.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_product.h"
#include "ny_product_store.h"

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef CONFIG_NYABULA_CORE_OTA
#include <pthread.h>
#include <sys/boardctl.h>

#include "nbootctl_bootctrl.h"
#include "nbootctl_part.h"
#include "ny_web_ota.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_MAINT_WALK_DEPTH   6
#define NY_MAINT_BUILD_INFO   "/data/nyabula/build.json"
#define NY_MAINT_LOG_DEVICE   "/dev/kmsg"
#define NY_MAINT_LOG_LINES    400
#define NY_MAINT_LOG_WIDTH    192
#define NY_MAINT_LOG_REPLY    200
#define NY_MAINT_CLOUD_DOMAIN "cloud"
#define NY_MAINT_URL_MAX      160

/* The reply to update.reboot has to leave the device before the reset does
 * away with the socket it travels on.
 */

#define NY_MAINT_REBOOT_DELAY_US 500000
#define NY_MAINT_WORKER_STACK    16384
#define NY_MAINT_BOOT_DOMAIN     "nuttx"
#define NY_MAINT_AMP_DOMAIN      "amp"

/* What the owner is told when a write goes under a mounted filesystem.  It
 * is for people, so it is in the language of the panel.
 */

#define NY_MAINT_FORCE_NOTICE                                                                                    \
  "该分区的文件系统仍处于挂载状态，设备不会卸载它。新内容直接写到已挂载的"    \
  "文件系统之下：重启前这个文件系统读到的内容不可信，它的任何一次写入都可能" \
  "破坏刚写入的镜像。写入完成后请立即重启。"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_maint_volume_s
{
  const char *id;
  const char *label;
  const char *path;
};

struct ny_maint_usage_s
{
  const char *id;
  const char *label;
  const char *path;
  bool clearable;
};

struct ny_maint_log_s
{
  uint32_t seq;
  char text[NY_MAINT_LOG_WIDTH];
};

#ifdef CONFIG_NYABULA_CORE_OTA
enum ny_maint_apply_e
{
  NY_MAINT_APPLY_IDLE = 0,
  NY_MAINT_APPLY_WRITING,
  NY_MAINT_APPLY_DONE,
  NY_MAINT_APPLY_FAILED,
};

/* One apply at a time, guarded by the claim in ny_web_ota, so the job is a
 * single static and not an allocation a failed thread start could leak.
 */

struct ny_maint_apply_s
{
  enum ny_maint_apply_e state;
  int error;          /* positive errno of the last failure, or 0 */
  const char *reason; /* short word for the panel, "" when none */
  const struct ny_web_ota_target_s *target; /* NULL before the first one */
  bool forced; /* written under a mounted filesystem */
  unsigned int medium;
  unsigned int running_slot;
  char sha256[NY_WEB_OTA_SHA256_HEX + 1];
};

/* Why a target cannot be written right now, as update.status says it and as
 * update.apply refuses it.
 */

enum ny_maint_refusal_e
{
  NY_MAINT_REFUSAL_NONE = 0,
  NY_MAINT_REFUSAL_BLOCKED,    /* never written from here */
  NY_MAINT_REFUSAL_NO_HANDOFF, /* the running slot is not known */
  NY_MAINT_REFUSAL_RUNNING,    /* holds the image that is running */
};
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct ny_maint_volume_s g_maint_volumes[] = {
  { "data", "数据", "/data" },
  { "config", "配置", "/config" },
};

static const struct ny_maint_usage_s g_maint_usage[] = {
  { "www", "控制面板", "/data/www", false },
  { "models", "模型", "/data/models", false },
  { "music", "音乐", "/data/music", false },
  { "apps", "应用与插件", "/data/nyabula/apps", false },
  { "state", "设备记录", "/data/nyabula", false },
  { "agent", "Nyabot", "/data/agent", false },
  { "tmp", "临时文件", "/data/tmp", true },
};

static mutex_t g_maint_log_lock = NXMUTEX_INITIALIZER;
static struct ny_maint_log_s *g_maint_log;
static uint32_t g_maint_log_next = 1; /* seq of the next line stored */
static uint32_t g_maint_log_count;
static int g_maint_log_fd = -1;
static char g_maint_log_partial[NY_MAINT_LOG_WIDTH];
static size_t g_maint_log_partial_used;

#ifdef CONFIG_NYABULA_CORE_OTA
static mutex_t g_maint_apply_lock = NXMUTEX_INITIALIZER;
static struct ny_maint_apply_s g_maint_apply = {
  .reason = "",
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_maint_walk
 *
 * Description:
 *   Add up the files below a directory, or remove them.  The directory
 *   itself stays.  Bounded in depth: the tree is ours and shallow, and a
 *   loop in it must not take the caller's stack with it.
 *
 ****************************************************************************/

static uint64_t ny_maint_walk(const char *path, int depth, bool remove_files)
{
  uint64_t total = 0;
  DIR *dir;
  struct dirent *entry;
  if (depth > NY_MAINT_WALK_DEPTH || (dir = opendir(path)) == NULL)
    {
      return 0;
    }

  while ((entry = readdir(dir)) != NULL)
    {
      char child[PATH_MAX];
      struct stat status;
      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0 ||
          snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
              (int)sizeof(child) ||
          stat(child, &status) < 0)
        {
          continue;
        }

      if (S_ISDIR(status.st_mode))
        {
          total += ny_maint_walk(child, depth + 1, remove_files);
          if (remove_files)
            {
              rmdir(child);
            }
        }
      else
        {
          total += (uint64_t)status.st_size;
          if (remove_files)
            {
              unlink(child);
            }
        }
    }

  closedir(dir);
  return total;
}

/****************************************************************************
 * Name: ny_maint_storage
 ****************************************************************************/

static cJSON *ny_maint_storage(void)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *volumes = cJSON_AddArrayToObject(root, "volumes");
  cJSON *usage = cJSON_AddArrayToObject(root, "usage");
  if (root == NULL || volumes == NULL || usage == NULL)
    {
      cJSON_Delete(root);
      return NULL;
    }

  for (size_t i = 0; i < sizeof(g_maint_volumes) / sizeof(g_maint_volumes[0]);
       i++)
    {
      struct statfs info;
      cJSON *row;
      if (statfs(g_maint_volumes[i].path, &info) < 0 || info.f_blocks == 0)
        {
          continue;
        }

      row = cJSON_CreateObject();
      if (row == NULL)
        {
          continue;
        }

      double block = (double)info.f_bsize;
      cJSON_AddStringToObject(row, "id", g_maint_volumes[i].id);
      cJSON_AddStringToObject(row, "label", g_maint_volumes[i].label);
      cJSON_AddStringToObject(row, "path", g_maint_volumes[i].path);
      cJSON_AddNumberToObject(row, "total", block * (double)info.f_blocks);
      cJSON_AddNumberToObject(row, "free", block * (double)info.f_bavail);
      cJSON_AddNumberToObject(row, "used",
                              block * (double)(info.f_blocks - info.f_bfree));
      cJSON_AddItemToArray(volumes, row);
    }

  for (size_t i = 0; i < sizeof(g_maint_usage) / sizeof(g_maint_usage[0]); i++)
    {
      struct stat status;
      cJSON *row;
      if (stat(g_maint_usage[i].path, &status) < 0 ||
          (row = cJSON_CreateObject()) == NULL)
        {
          continue;
        }

      uint64_t bytes = ny_maint_walk(g_maint_usage[i].path, 0, false);

      /* "state" holds "apps"; report what is left so rows add up. */

      if (strcmp(g_maint_usage[i].id, "state") == 0)
        {
          uint64_t apps = ny_maint_walk("/data/nyabula/apps", 0, false);
          bytes = bytes > apps ? bytes - apps : 0;
        }

      cJSON_AddStringToObject(row, "id", g_maint_usage[i].id);
      cJSON_AddStringToObject(row, "label", g_maint_usage[i].label);
      cJSON_AddStringToObject(row, "path", g_maint_usage[i].path);
      cJSON_AddNumberToObject(row, "bytes", (double)bytes);
      cJSON_AddBoolToObject(row, "clearable", g_maint_usage[i].clearable);
      cJSON_AddItemToArray(usage, row);
    }

  return root;
}

#ifdef CONFIG_NYABULA_CORE_OTA
/****************************************************************************
 * Name: ny_maint_mounted
 *
 * Description:
 *   Whether a filesystem is mounted at a path.  A directory of the pseudo
 *   filesystem answers statfs() too, but with no blocks.
 *
 *   This does not ask which medium the filesystem comes from.  A board that
 *   started from the card may have /config from the eMMC, and then the
 *   card's config partition is not in use; it is reported as mounted all the
 *   same, because being asked to force a write that was safe costs a click
 *   and the opposite mistake costs the partition.
 *
 ****************************************************************************/

static bool ny_maint_mounted(const char *path)
{
  struct statfs info;

  return path != NULL && statfs(path, &info) == 0 && info.f_blocks != 0;
}

/****************************************************************************
 * Name: ny_maint_refusal
 *
 * Description:
 *   Why a target cannot be written whatever the request says.  One answer
 *   for update.status and update.apply, so the panel never offers what the
 *   device then refuses.
 *
 ****************************************************************************/

static enum ny_maint_refusal_e
ny_maint_refusal(const struct ny_web_ota_target_s *target, bool handoff,
                 unsigned int running_slot)
{
  if (target->blocked || ny_web_ota_target_capacity(target) == 0)
    {
      return NY_MAINT_REFUSAL_BLOCKED;
    }

  /* Every write needs the medium, and the medium comes with the handoff. */

  if (!handoff)
    {
      return NY_MAINT_REFUSAL_NO_HANDOFF;
    }

  /* Staging a slot never touches the one that runs.  A raw write would,
   * and the image in it is the one a failed update falls back to.
   */

  if (target->kind == NY_WEB_OTA_KIND_PARTITION &&
      strcmp(target->partition, running_slot ? "nuttx_b" : "nuttx_a") == 0)
    {
      return NY_MAINT_REFUSAL_RUNNING;
    }

  return NY_MAINT_REFUSAL_NONE;
}

/****************************************************************************
 * Name: ny_maint_staging_room
 *
 * Description:
 *   What an upload can use: the free space, plus the image that is already
 *   staged, because the next upload removes it first.
 *
 ****************************************************************************/

static uint64_t ny_maint_staging_room(void)
{
  uint64_t room = ny_web_ota_staging_space();
  struct stat status;

  if (stat(NY_WEB_OTA_FILE, &status) == 0 && S_ISREG(status.st_mode) &&
      status.st_size > 0)
    {
      room += (uint64_t)status.st_size;
    }

  return room;
}

/****************************************************************************
 * Name: ny_maint_slot_rows
 *
 * Description:
 *   The two slots of one domain.  running is -1 for a domain that has no
 *   running slot to speak of.
 *
 *   tries_remaining is deliberately absent.  N-Boot chooses by priority
 *   alone; showing a retry count would describe a fallback that does not
 *   exist.
 *
 ****************************************************************************/

static void ny_maint_slot_rows(cJSON *rows,
                               const struct nbootctl_slot_state_s *slots,
                               unsigned int active, int running)
{
  for (unsigned int i = 0; rows != NULL && i < 2; i++)
    {
      const struct nbootctl_slot_state_s *slot = &slots[i];
      cJSON *row = cJSON_CreateObject();
      if (row == NULL)
        {
          break;
        }

      cJSON_AddStringToObject(row, "name", i ? "b" : "a");
      cJSON_AddBoolToObject(row, "active", active == i);
      cJSON_AddBoolToObject(row, "running", running == (int)i);
      cJSON_AddBoolToObject(row, "bootable", slot->priority != 0);
      cJSON_AddBoolToObject(row, "successful", slot->successful);
      cJSON_AddNumberToObject(row, "priority", slot->priority);
      cJSON_AddNumberToObject(row, "version", (double)slot->image_version);
      cJSON_AddNumberToObject(row, "size", (double)slot->image_size);
      cJSON_AddItemToArray(rows, row);
    }
}

/****************************************************************************
 * Name: ny_maint_target_rows
 *
 * Description:
 *   "targets" of update.status: everything update.apply can be asked to
 *   write, and whether it would agree right now.  The panel renders from
 *   this and knows no target of its own.
 *
 ****************************************************************************/

static void ny_maint_target_rows(cJSON *root,
                                 const struct nbootctl_state_s *state,
                                 bool usable)
{
  static const char *const kinds[] = {
    "slot",
    "nboot",
    "partition",
  };

  static const char *const formats[] = {
    "raw", "arm64", "fit", "bootctrl", "fat",
  };

  static const char *const reasons[] = {
    "",
    "blocked",
    "no-handoff",
    "running",
  };

  const struct ny_web_ota_target_s *target;
  cJSON *rows = cJSON_AddArrayToObject(root, "targets");
  uint64_t room = ny_maint_staging_room();

  cJSON_AddNumberToObject(root, "stagingFree", (double)room);
  for (size_t i = 0;
       rows != NULL && (target = ny_web_ota_target_at(i)) != NULL; i++)
    {
      enum ny_maint_refusal_e refusal =
          ny_maint_refusal(target, usable, state->running_slot);
      uint64_t capacity = ny_web_ota_target_capacity(target);
      const char *slot = "";
      cJSON *row = cJSON_CreateObject();
      if (row == NULL)
        {
          break;
        }

      /* The slot a staged image would replace: for NuttX the one that is
       * not running, for AMP the one that is not active.
       */

      if (usable && target->kind == NY_WEB_OTA_KIND_SLOT)
        {
          unsigned int busy = strcmp(target->domain, NY_MAINT_AMP_DOMAIN) == 0
                                  ? state->amp_active
                                  : state->running_slot;
          slot = busy ? "a" : "b";
        }

      cJSON_AddStringToObject(row, "id", target->id);
      cJSON_AddStringToObject(row, "label", target->label);
      cJSON_AddStringToObject(row, "description", target->description);
      cJSON_AddStringToObject(row, "kind", kinds[target->kind]);
      cJSON_AddStringToObject(row, "format", formats[target->magic]);
      cJSON_AddBoolToObject(row, "advanced", target->advanced);

      /* An image has to fit the staging volume before it can fit the
       * partition, so the smaller of the two is the limit that is true.
       */

      cJSON_AddNumberToObject(row, "maxBytes",
                              (double)(capacity < room ? capacity : room));
      cJSON_AddNumberToObject(row, "capacity", (double)capacity);
      cJSON_AddStringToObject(row, "slot", slot);
      cJSON_AddBoolToObject(row, "available",
                            refusal == NY_MAINT_REFUSAL_NONE);
      cJSON_AddStringToObject(row, "reason", reasons[refusal]);
      cJSON_AddBoolToObject(row, "mounted", ny_maint_mounted(target->mount));
      cJSON_AddItemToArray(rows, row);
    }
}

/****************************************************************************
 * Name: ny_maint_update_slots
 *
 * Description:
 *   The A/B half of update.status: which slot is running, what bootctrl
 *   says about the slots of both domains, what can be written and how an
 *   apply is going.
 *
 ****************************************************************************/

static void ny_maint_update_slots(cJSON *root, cJSON *current)
{
  static const char *const states[] = {
    "idle",
    "writing",
    "done",
    "failed",
  };

  struct nbootctl_state_s state;
  const struct ny_web_ota_target_s *applied;
  enum ny_maint_apply_e progress;
  const char *reason;
  const char *detail;
  cJSON *slots = cJSON_AddArrayToObject(root, "slots");
  cJSON *amp_slots = cJSON_AddArrayToObject(root, "ampSlots");
  cJSON *apply = cJSON_AddObjectToObject(root, "apply");
  bool forced;
  int error;
  int ret = nbootctl_bootctrl_snapshot(&state);
  bool usable = ret == 0 && state.handoff_valid;

  cJSON_AddStringToObject(current, "slot",
                          !usable              ? ""
                          : state.running_slot ? "b"
                                               : "a");
  if (usable)
    {
      const struct nbootctl_slot_state_s *amp = &state.amp[state.amp_active];

      ny_maint_slot_rows(slots, state.nuttx, state.nuttx_active,
                         (int)state.running_slot);

      /* No AMP slot is ever "running" here: this is the image of a NuttX
       * slot, and the handoff says nothing about the other domain.
       */

      ny_maint_slot_rows(amp_slots, state.amp, state.amp_active, -1);

      /* N-Boot tries the AMP domain before the NuttX slots, and takes its
       * active slot whenever that one is a boot candidate.  So this is
       * what the next start will be, not what this one was.
       */

      cJSON_AddBoolToObject(root, "ampActive",
                            amp->priority != 0 && amp->image_size != 0);
      cJSON_AddStringToObject(root, "ampTarget", state.amp_active ? "a" : "b");
    }
  else
    {
      cJSON_AddBoolToObject(root, "ampActive", false);
      cJSON_AddStringToObject(root, "ampTarget", "");
    }

  ny_maint_target_rows(root, &state, usable);
  if (usable)
    {
      /* The slot an upload would replace: never the one that is running. */

      cJSON_AddStringToObject(root, "target", state.running_slot ? "a" : "b");
      detail = "no online update service; an uploaded image is written to "
               "the slot that is not running, verified from the media and "
               "then made active";
    }
  else if (ret < 0)
    {
      detail = "the boot control record could not be read; updates are "
               "refused";
    }
  else
    {
      detail = "this image was not started by N-Boot, so the running slot "
               "is unknown; updates are refused";
    }

  cJSON_AddStringToObject(root, "channel", "upload");
  cJSON_AddBoolToObject(root, "online", false);
  cJSON_AddBoolToObject(root, "upload", usable);

  /* For a panel from before there were targets: the limit of the one it
   * knows about.
   */

  cJSON_AddNumberToObject(
      root, "maxBytes",
      (double)ny_web_ota_target_capacity(ny_web_ota_target_find(
          NY_WEB_OTA_TARGET_DEFAULT, sizeof(NY_WEB_OTA_TARGET_DEFAULT) - 1)));
  cJSON_AddStringToObject(root, "detail", detail);

  nxmutex_lock(&g_maint_apply_lock);
  progress = g_maint_apply.state;
  error = g_maint_apply.error;
  reason = g_maint_apply.reason;
  applied = g_maint_apply.target;
  forced = g_maint_apply.forced;
  nxmutex_unlock(&g_maint_apply_lock);
  if (apply != NULL)
    {
      cJSON_AddStringToObject(apply, "state", states[progress]);
      cJSON_AddNumberToObject(apply, "error", error);
      cJSON_AddStringToObject(apply, "reason", reason);
      cJSON_AddStringToObject(apply, "target",
                              applied != NULL ? applied->id : "");
      cJSON_AddBoolToObject(apply, "forced", forced);
    }
}
#endif /* CONFIG_NYABULA_CORE_OTA */

/****************************************************************************
 * Name: ny_maint_update
 ****************************************************************************/

static cJSON *ny_maint_update(void)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *current = cJSON_AddObjectToObject(root, "current");
  struct utsname system;
  char text[256];
  cJSON *build = NULL;
  int fd;
  if (root == NULL || current == NULL)
    {
      cJSON_Delete(root);
      return NULL;
    }

  /* The image that seeded /data says which release this is; the kernel says
   * when the code that is running was compiled.
   */

  fd = open(NY_MAINT_BUILD_INFO, O_RDONLY | O_CLOEXEC);
  if (fd >= 0)
    {
      ssize_t count = read(fd, text, sizeof(text) - 1);
      close(fd);
      if (count > 0)
        {
          text[count] = 0;
          build = cJSON_Parse(text);
        }
    }

  const cJSON *version = cJSON_GetObjectItemCaseSensitive(build, "version");
  const cJSON *built = cJSON_GetObjectItemCaseSensitive(build, "built");
  cJSON_AddStringToObject(current, "version",
                          cJSON_IsString(version) ? version->valuestring : "");
  cJSON_AddStringToObject(current, "imageBuiltAt",
                          cJSON_IsString(built) ? built->valuestring : "");
  if (uname(&system) == 0)
    {
      cJSON_AddStringToObject(current, "builtAt", system.version);
      cJSON_AddStringToObject(current, "os", system.sysname);
      cJSON_AddStringToObject(current, "arch", system.machine);
    }

#ifdef CONFIG_NYABULA_CORE_OTA
  ny_maint_update_slots(root, current);
#else
  cJSON_AddStringToObject(current, "slot", "");
  cJSON_AddArrayToObject(root, "slots");
  cJSON_AddStringToObject(root, "channel", "manual");
  cJSON_AddBoolToObject(root, "online", false);
  cJSON_AddStringToObject(root, "detail",
                          "no online update service; firmware arrives as an "
                          "OTA package or over USB");
#endif
  cJSON_Delete(build);
  return root;
}

#ifdef CONFIG_NYABULA_CORE_OTA
/****************************************************************************
 * Name: ny_maint_apply_worker
 *
 * Description:
 *   Write the staged image to its target.
 *
 *   This takes as long as writing and reading back the whole image takes,
 *   which is longer than a panel waits for an answer, so it runs on its own
 *   thread and update.status reports how it is going.  The caller took the
 *   staging claim and decided that the target may be written; the claim is
 *   given back here.
 *
 ****************************************************************************/

static void *ny_maint_apply_worker(void *argument)
{
  struct ny_maint_apply_s *job = argument;
  const struct ny_web_ota_target_s *target = job->target;
  char actual[NY_WEB_OTA_SHA256_HEX + 1];
  const char *reason = "";
  int ret;

  /* The digest was compared when the file arrived.  It is compared again
   * because what is about to be written is the file as it is now, and the
   * owner confirmed one particular image.
   */

  ret = ny_web_ota_file_digest(NY_WEB_OTA_FILE, actual, NULL);
  if (ret == 0 && memcmp(actual, job->sha256, NY_WEB_OTA_SHA256_HEX) != 0)
    {
      unlink(NY_WEB_OTA_FILE);
      reason = "digest";
      ret = -EBADMSG;
    }

  if (ret < 0 && reason[0] == '\0')
    {
      reason = "staged-file";
    }

  if (ret == 0)
    {
      /* Checked when update.apply was accepted, and again here for the same
       * reason as the digest: this is the last look before the medium.
       */

      ret = ny_web_ota_file_check(target, NY_WEB_OTA_FILE);
      if (ret < 0)
        {
          reason = ret == -EFBIG ? "too-large" : "format";
        }
    }

  if (ret == 0)
    {
      switch (target->kind)
        {
          case NY_WEB_OTA_KIND_SLOT:

            /* Staging clears the target's priority before the first sector
             * is written and restores it only after the read-back matches,
             * so a failure at any point leaves the other slot the one that
             * boots.
             */

            ret = nbootctl_bootctrl_stage(job->medium, target->domain,
                                          job->running_slot, NY_WEB_OTA_FILE);
            break;

          case NY_WEB_OTA_KIND_NBOOT:

            /* One region, replaced in place, each chunk read back as it is
             * written.  There is no second copy to fall back to: from the
             * first sector until the last, a loss of power leaves a board
             * that does not start.
             */

            ret = nbootctl_update_nboot(job->medium, NY_WEB_OTA_FILE);
            break;

          default:

            /* Raw: bounded by the partition's own device node, and read
             * back against the digest the owner confirmed.
             */

            ret = nbootctl_part_write(job->medium, target->partition,
                                      NY_WEB_OTA_FILE, job->sha256);
            break;
        }

      if (ret == 0)
        {
          unlink(NY_WEB_OTA_FILE);
        }
      else
        {
          reason = ret == -EFBIG                             ? "too-large"
                   : ret == -EBADMSG || ret == -EKEYREJECTED ? "verify"
                                                             : "io";
        }
    }

  nxmutex_lock(&g_maint_apply_lock);
  job->state = ret == 0 ? NY_MAINT_APPLY_DONE : NY_MAINT_APPLY_FAILED;
  job->error = ret == 0 ? 0 : -ret;
  job->reason = reason;
  nxmutex_unlock(&g_maint_apply_lock);
  ny_web_ota_release();
  return NULL;
}

/****************************************************************************
 * Name: ny_maint_reboot_worker
 ****************************************************************************/

static void *ny_maint_reboot_worker(void *argument)
{
  (void)argument;
  usleep(NY_MAINT_REBOOT_DELAY_US);
  boardctl(BOARDIOC_RESET, 0);

  /* Only reached if the board refused: let the panel try something else. */

  ny_web_ota_release();
  return NULL;
}

/****************************************************************************
 * Name: ny_maint_spawn
 ****************************************************************************/

static int ny_maint_spawn(void *(*entry)(void *), void *argument)
{
  pthread_attr_t attributes;
  pthread_t thread;
  int ret;

  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, NY_MAINT_WORKER_STACK);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&thread, &attributes, entry, argument);
  pthread_attr_destroy(&attributes);
  return -ret;
}

/****************************************************************************
 * Name: ny_maint_apply
 ****************************************************************************/

static int ny_maint_apply(const cJSON *data, cJSON **result)
{
  const cJSON *digest = cJSON_GetObjectItemCaseSensitive(data, "sha256");
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(data, "target");
  const cJSON *advanced = cJSON_GetObjectItemCaseSensitive(data, "advanced");
  const cJSON *force = cJSON_GetObjectItemCaseSensitive(data, "force");
  const struct ny_web_ota_target_s *target;
  const char *id = NY_WEB_OTA_TARGET_DEFAULT;
  struct stat status;
  unsigned int medium = 0;
  unsigned int slot = 0;
  bool handoff;
  bool forced = false;
  int ret;

  if (!cJSON_IsString(digest) ||
      !ny_web_ota_digest_ok(digest->valuestring,
                            strlen(digest->valuestring)) ||
      (name != NULL && !cJSON_IsString(name)))
    {
      return -EINVAL;
    }

  if (name != NULL)
    {
      id = name->valuestring;
    }

  /* Not -ENOENT for a target that does not exist: that would reach the
   * panel as "no such topic", which it reads as a firmware too old to be
   * updated this way.
   */

  target = ny_web_ota_target_find(id, strlen(id));
  if (target == NULL)
    {
      return -EINVAL;
    }

  /* The acknowledgement comes before anything about the device, so that a
   * client that did not send it learns only that it is needed.
   */

  if (target->advanced && !cJSON_IsTrue(advanced))
    {
      return -ENOKEY;
    }

  handoff = nbootctl_handoff_read(&medium, &slot, NULL, NULL) == 0;
  switch (ny_maint_refusal(target, handoff, slot))
    {
      case NY_MAINT_REFUSAL_BLOCKED:
        return -EXDEV;

      case NY_MAINT_REFUSAL_NO_HANDOFF:

        /* Not started by N-Boot: which medium holds the partitions, and
         * which slot is running and must be left alone, is not known.
         */

        return -ENODEV;

      case NY_MAINT_REFUSAL_RUNNING:
        return -ETXTBSY;

      default:
        break;
    }

  /* Never unmounted to make way, and never written under without being
   * told to: the owner either agrees to what NY_MAINT_FORCE_NOTICE says or
   * the partition stays as it is.
   */

  if (ny_maint_mounted(target->mount))
    {
      if (!cJSON_IsTrue(force))
        {
          return -ENOTEMPTY;
        }

      forced = true;
    }

  /* From here on the claim is held.  It also keeps a raw write to the
   * bootctrl partition apart from staging and update.confirm, which hold
   * that record in memory while they work.
   */

  ret = ny_web_ota_claim();
  if (ret < 0)
    {
      return ret;
    }

  /* A missing image is "nothing to apply", not "no such topic": -ENOENT
   * would reach the panel as the latter.
   */

  if (stat(NY_WEB_OTA_FILE, &status) < 0 || !S_ISREG(status.st_mode))
    {
      ny_web_ota_release();
      return -ENODATA;
    }

  /* The upload was checked against the target it named.  This is the
   * target that will be written, and nothing ties the two together but
   * the file itself.
   */

  ret = ny_web_ota_file_check(target, NY_WEB_OTA_FILE);
  if (ret < 0)
    {
      ny_web_ota_release();
      return ret == -ENOEXEC ? -EMEDIUMTYPE : ret == -ENOENT ? -ENODATA : ret;
    }

  nxmutex_lock(&g_maint_apply_lock);
  g_maint_apply.state = NY_MAINT_APPLY_WRITING;
  g_maint_apply.error = 0;
  g_maint_apply.reason = "";
  g_maint_apply.target = target;
  g_maint_apply.forced = forced;
  g_maint_apply.medium = medium;
  g_maint_apply.running_slot = slot;
  for (size_t i = 0; i < NY_WEB_OTA_SHA256_HEX; i++)
    {
      char c = digest->valuestring[i];
      g_maint_apply.sha256[i] = c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c;
    }

  g_maint_apply.sha256[NY_WEB_OTA_SHA256_HEX] = '\0';
  nxmutex_unlock(&g_maint_apply_lock);

  ret = ny_maint_spawn(ny_maint_apply_worker, &g_maint_apply);
  if (ret < 0)
    {
      nxmutex_lock(&g_maint_apply_lock);
      g_maint_apply.state = NY_MAINT_APPLY_FAILED;
      g_maint_apply.error = -ret;
      g_maint_apply.reason = "memory";
      nxmutex_unlock(&g_maint_apply_lock);
      ny_web_ota_release();
      return ret;
    }

  *result = ny_maint_update();
  if (*result == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(*result, "started", true);

  /* Which target is in "apply" already; what forcing it means is said
   * here, once, in the answer to the request that forced it.
   */

  if (forced)
    {
      cJSON_AddStringToObject(*result, "notice", NY_MAINT_FORCE_NOTICE);
    }

  return 0;
}

/****************************************************************************
 * Name: ny_maint_confirm
 ****************************************************************************/

static int ny_maint_confirm(const cJSON *data, cJSON **result)
{
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(data, "target");
  const char *domain = NY_MAINT_BOOT_DOMAIN;
  struct nbootctl_state_s state;
  unsigned int medium;
  unsigned int slot;
  int ret;

  if (name != NULL && !cJSON_IsString(name))
    {
      return -EINVAL;
    }

  if (nbootctl_handoff_read(&medium, &slot, NULL, NULL) < 0)
    {
      return -ENODEV;
    }

  if (name != NULL && strcmp(name->valuestring, NY_MAINT_AMP_DOMAIN) == 0)
    {
      /* The AMP domain has no running slot as seen from here: what the
       * owner vouches for, after watching it work, is the active one.
       */

      ret = nbootctl_bootctrl_snapshot(&state);
      if (ret < 0 || !state.handoff_valid)
        {
          return ret < 0 ? ret : -ENODEV;
        }

      domain = NY_MAINT_AMP_DOMAIN;
      slot = state.amp_active;
    }
  else if (name != NULL &&
           strcmp(name->valuestring, NY_MAINT_BOOT_DOMAIN) != 0)
    {
      return -EINVAL;
    }

  /* Staging keeps the bootctrl record in memory while it writes and stores
   * it again at the end; a change made in between would be lost.
   */

  ret = ny_web_ota_claim();
  if (ret < 0)
    {
      return ret;
    }

  ret = nbootctl_bootctrl_mark_successful(medium, domain, slot);
  ny_web_ota_release();
  if (ret < 0)
    {
      return ret == -ENOENT ? -ENODATA : ret;
    }

  *result = ny_maint_update();
  if (*result == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(*result, "confirmed", true);
  return 0;
}

/****************************************************************************
 * Name: ny_maint_reboot
 ****************************************************************************/

static int ny_maint_reboot(cJSON **result)
{
  int ret;

  /* Not while an image is arriving or being written.  The claim is kept:
   * nothing else should start in the half second that is left.
   */

  ret = ny_web_ota_claim();
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_maint_spawn(ny_maint_reboot_worker, NULL);
  if (ret < 0)
    {
      ny_web_ota_release();
      return ret;
    }

  *result = cJSON_CreateObject();
  if (*result == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(*result, "rebooting", true);
  cJSON_AddNumberToObject(*result, "delayMs", NY_MAINT_REBOOT_DELAY_US / 1000);
  return 0;
}
#endif /* CONFIG_NYABULA_CORE_OTA */

/****************************************************************************
 * Name: ny_maint_log_store
 ****************************************************************************/

static void ny_maint_log_store(const char *text, size_t length)
{
  struct ny_maint_log_s *slot =
      &g_maint_log[(g_maint_log_next - 1) % NY_MAINT_LOG_LINES];
  if (length >= sizeof(slot->text))
    {
      length = sizeof(slot->text) - 1;
    }

  memcpy(slot->text, text, length);
  slot->text[length] = 0;
  slot->seq = g_maint_log_next++;
  if (g_maint_log_count < NY_MAINT_LOG_LINES)
    {
      g_maint_log_count++;
    }
}

/****************************************************************************
 * Name: ny_maint_log_drain
 *
 * Description:
 *   Move what the system log has gathered since the last call into the
 *   numbered line buffer (lock held).  The log device keeps a read position
 *   per open file, so the descriptor stays open for the life of the task.
 *
 ****************************************************************************/

static int ny_maint_log_drain(void)
{
  char chunk[512];
  ssize_t count;
  if (g_maint_log == NULL)
    {
      g_maint_log = calloc(NY_MAINT_LOG_LINES, sizeof(*g_maint_log));
      if (g_maint_log == NULL)
        {
          return -ENOMEM;
        }
    }

  if (g_maint_log_fd < 0)
    {
      g_maint_log_fd =
          open(NY_MAINT_LOG_DEVICE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (g_maint_log_fd < 0)
        {
          return -errno;
        }
    }

  while ((count = read(g_maint_log_fd, chunk, sizeof(chunk))) > 0)
    {
      for (ssize_t i = 0; i < count; i++)
        {
          char c = chunk[i];
          if (c == '\r')
            {
              continue;
            }

          if (c == '\n' ||
              g_maint_log_partial_used == sizeof(g_maint_log_partial) - 1)
            {
              if (g_maint_log_partial_used > 0)
                {
                  ny_maint_log_store(g_maint_log_partial,
                                     g_maint_log_partial_used);
                }

              g_maint_log_partial_used = 0;
              if (c == '\n')
                {
                  continue;
                }
            }

          /* Keep the reply valid JSON text whatever a driver printed. */

          g_maint_log_partial[g_maint_log_partial_used++] =
              ((unsigned char)c < 0x20 && c != '\t') ? ' ' : c;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: ny_maint_logs
 ****************************************************************************/

static int ny_maint_logs(const cJSON *data, cJSON **result)
{
  const cJSON *after_item = cJSON_GetObjectItemCaseSensitive(data, "after");
  const cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(data, "limit");
  uint32_t after = cJSON_IsNumber(after_item) && after_item->valuedouble > 0
                       ? (uint32_t)after_item->valuedouble
                       : 0;
  uint32_t limit = cJSON_IsNumber(limit_item) && limit_item->valuedouble >= 1
                       ? (uint32_t)limit_item->valuedouble
                       : NY_MAINT_LOG_REPLY;
  cJSON *root;
  cJSON *lines;
  int ret;
  if (limit > NY_MAINT_LOG_REPLY)
    {
      limit = NY_MAINT_LOG_REPLY;
    }

  ret = nxmutex_lock(&g_maint_log_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_maint_log_drain();
  if (ret < 0)
    {
      nxmutex_unlock(&g_maint_log_lock);
      return ret == -ENOENT ? -ENOSYS : ret;
    }

  root = cJSON_CreateObject();
  lines = cJSON_AddArrayToObject(root, "lines");
  if (root == NULL || lines == NULL)
    {
      nxmutex_unlock(&g_maint_log_lock);
      cJSON_Delete(root);
      return -ENOMEM;
    }

  /* Lines are numbered from 1 and the oldest kept is next - count.  A
   * reader that fell behind further than that is told so.
   */

  uint32_t oldest = g_maint_log_next - g_maint_log_count;
  uint32_t first = after + 1 > oldest ? after + 1 : oldest;
  if (after >= g_maint_log_next)
    {
      first = oldest; /* the device restarted; start over */
    }

  if (g_maint_log_next - first > limit)
    {
      first = after == 0 ? g_maint_log_next - limit : first;
    }

  uint32_t last = first;
  for (uint32_t seq = first; seq < g_maint_log_next && seq - first < limit;
       seq++)
    {
      const struct ny_maint_log_s *slot =
          &g_maint_log[(seq - 1) % NY_MAINT_LOG_LINES];
      cJSON *row = cJSON_CreateObject();
      if (row == NULL)
        {
          break;
        }

      cJSON_AddNumberToObject(row, "seq", seq);
      cJSON_AddStringToObject(row, "text", slot->text);
      cJSON_AddItemToArray(lines, row);
      last = seq + 1;
    }

  cJSON_AddNumberToObject(root, "next", last > 0 ? last - 1 : 0);
  cJSON_AddBoolToObject(root, "dropped",
                        after != 0 && after + 1 < oldest &&
                            after < g_maint_log_next);
  nxmutex_unlock(&g_maint_log_lock);
  *result = root;
  return 0;
}

/****************************************************************************
 * Name: ny_maint_cloud
 ****************************************************************************/

static int ny_maint_cloud(const cJSON *update, cJSON **result)
{
  cJSON *stored = NULL;
  uint64_t revision = 0;
  bool enabled = false;
  char url[NY_MAINT_URL_MAX + 1] = "";
  cJSON *root;
  int ret = ny_product_store_read(NY_MAINT_CLOUD_DOMAIN, &stored, &revision);
  if (ret < 0)
    {
      return ret;
    }

  const cJSON *item = cJSON_GetObjectItemCaseSensitive(stored, "enabled");
  enabled = cJSON_IsTrue(item);
  item = cJSON_GetObjectItemCaseSensitive(stored, "url");
  if (cJSON_IsString(item))
    {
      strlcpy(url, item->valuestring, sizeof(url));
    }

  cJSON_Delete(stored);
  if (update != NULL)
    {
      const cJSON *want = cJSON_GetObjectItemCaseSensitive(update, "enabled");
      const cJSON *where = cJSON_GetObjectItemCaseSensitive(update, "url");
      cJSON *value;
      if (cJSON_IsBool(want))
        {
          enabled = cJSON_IsTrue(want);
        }

      if (cJSON_IsString(where))
        {
          const char *text = where->valuestring;
          if (strlen(text) > NY_MAINT_URL_MAX ||
              (text[0] != 0 && strncmp(text, "ws://", 5) != 0 &&
               strncmp(text, "wss://", 6) != 0))
            {
              return -EINVAL;
            }

          strlcpy(url, text, sizeof(url));
        }

      value = cJSON_CreateObject();
      if (value == NULL)
        {
          return -ENOMEM;
        }

      cJSON_AddBoolToObject(value, "enabled", enabled);
      cJSON_AddStringToObject(value, "url", url);
      ret = ny_product_store_write(NY_MAINT_CLOUD_DOMAIN, value, revision,
                                   &revision);
      cJSON_Delete(value);
      if (ret < 0)
        {
          return ret;
        }
    }

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  /* The settings are kept for the firmware that will use them.  This one
   * has no relay client, and must not look as if it had.
   */

  cJSON_AddBoolToObject(root, "enabled", enabled);
  cJSON_AddStringToObject(root, "url", url);
  cJSON_AddBoolToObject(root, "connected", false);
  cJSON_AddStringToObject(root, "state", "unsupported");
  cJSON_AddStringToObject(root, "detail",
                          "relay client is not part of this firmware");
  *result = root;
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_product_maintenance_request
 ****************************************************************************/

int ny_product_maintenance_request(const struct ny_product_caller_s *caller,
                                   const char *topic, const cJSON *data,
                                   cJSON **result)
{
  bool owner = caller->role == NY_PRODUCT_OWNER;
  if (strcmp(topic, "storage.status") == 0)
    {
      *result = ny_maint_storage();
      return *result == NULL ? -ENOMEM : 0;
    }

  if (strcmp(topic, "storage.cleanup") == 0)
    {
      const cJSON *target = cJSON_GetObjectItemCaseSensitive(data, "target");
      if (!owner)
        {
          return -EACCES;
        }

      if (!cJSON_IsString(target))
        {
          return -EINVAL;
        }

      for (size_t i = 0; i < sizeof(g_maint_usage) / sizeof(g_maint_usage[0]);
           i++)
        {
          if (g_maint_usage[i].clearable &&
              strcmp(g_maint_usage[i].id, target->valuestring) == 0)
            {
              uint64_t freed;
#ifdef CONFIG_NYABULA_CORE_OTA
              /* The clearable directory is where an uploaded image waits.
               * Emptying it under an upload or an apply would pull the file
               * out from under them, so it waits its turn like they do.
               */

              int claimed = ny_web_ota_claim();
              if (claimed < 0)
                {
                  return claimed;
                }
#endif

              freed = ny_maint_walk(g_maint_usage[i].path, 0, true);
#ifdef CONFIG_NYABULA_CORE_OTA
              ny_web_ota_release();
#endif
              *result = ny_maint_storage();
              if (*result == NULL)
                {
                  return -ENOMEM;
                }

              cJSON_AddNumberToObject(*result, "freed", (double)freed);
              return 0;
            }
        }

      return -EINVAL;
    }

  if (strcmp(topic, "update.status") == 0)
    {
      *result = ny_maint_update();
      return *result == NULL ? -ENOMEM : 0;
    }

#ifdef CONFIG_NYABULA_CORE_OTA
  if (strcmp(topic, "update.apply") == 0)
    {
      return owner ? ny_maint_apply(data, result) : -EACCES;
    }

  if (strcmp(topic, "update.confirm") == 0)
    {
      return owner ? ny_maint_confirm(data, result) : -EACCES;
    }

  if (strcmp(topic, "update.reboot") == 0)
    {
      return owner ? ny_maint_reboot(result) : -EACCES;
    }
#endif

  if (strcmp(topic, "logs.tail") == 0)
    {
      return owner ? ny_maint_logs(data, result) : -EACCES;
    }

  if (strcmp(topic, "cloud.status") == 0)
    {
      return ny_maint_cloud(NULL, result);
    }

  if (strcmp(topic, "cloud.config") == 0)
    {
      return owner ? ny_maint_cloud(data, result) : -EACCES;
    }

  return -ENOSYS;
}
