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
 *   update.status     any role   the firmware that is running
 *   logs.tail         owner      {"after": seq, "limit": n}; system log lines
 *   cloud.status      any role   the stored relay settings
 *   cloud.config      owner      {"enabled": bool, "url": "ws[s]://..."}
 *
 * Nothing here pretends: the firmware has no online update service and no
 * relay client, and the answers say so.
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

  cJSON_AddStringToObject(current, "slot", "");
  cJSON_AddArrayToObject(root, "slots");
  cJSON_AddStringToObject(root, "channel", "manual");
  cJSON_AddBoolToObject(root, "online", false);
  cJSON_AddStringToObject(root, "detail",
                          "no online update service; firmware arrives as an "
                          "OTA package or over USB");
  cJSON_Delete(build);
  return root;
}

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
              uint64_t freed = ny_maint_walk(g_maint_usage[i].path, 0, true);
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
