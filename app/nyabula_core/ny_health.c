/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_health.c
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
#include <nuttx/mutex.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ny_health.h"
#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
#include "ny_state.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_health_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ny_health_valid_component(const char *value, size_t limit,
                                      bool uppercase);
static int ny_health_path(const char *id, const char *version, char *path,
                          size_t size);
static int ny_health_mkdir(const char *path);
static int ny_health_mkdir_parents(const char *path);
static int ny_health_ensure_store(const char *id);
static int ny_health_read(const char *path, unsigned int *failures);
static int ny_health_write(const char *path, unsigned int failures);
static int ny_health_sync_parent(const char *path);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ny_health_valid_component(const char *value, size_t limit,
                                      bool uppercase)
{
  size_t index;
  size_t length;

  if (value == NULL || (length = strlen(value)) == 0 || length >= limit ||
      value[0] == '.' || value[length - 1] == '.')
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      char item = value[index];

      if (!((item >= 'a' && item <= 'z') ||
            (uppercase && item >= 'A' && item <= 'Z') ||
            (item >= '0' && item <= '9') || item == '.' || item == '_' ||
            item == '-'))
        {
          return false;
        }
    }

  return true;
}

static int ny_health_path(const char *id, const char *version, char *path,
                          size_t size)
{
  int length;

  if (!ny_health_valid_component(id, NY_PLUGIN_ID_SIZE, false) ||
      !ny_health_valid_component(version, NY_PLUGIN_VERSION_SIZE, true))
    {
      return -EINVAL;
    }

  length = snprintf(path, size, "%s/%s/%s.failures",
                    CONFIG_NYABULA_CORE_HEALTH_STORE, id, version);
  return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

static int ny_health_mkdir(const char *path)
{
  struct stat status;

  if (mkdir(path, 0700) == 0)
    {
      return 0;
    }

  if (errno != EEXIST || lstat(path, &status) < 0 || !S_ISDIR(status.st_mode))
    {
      return -errno;
    }

  return 0;
}

static int ny_health_mkdir_parents(const char *path)
{
  char copy[PATH_MAX];
  char *cursor;
  int ret;

  if (path == NULL || path[0] != '/' ||
      strlcpy(copy, path, sizeof(copy)) >= sizeof(copy))
    {
      return -EINVAL;
    }

  for (cursor = copy + 1; *cursor != '\0'; cursor++)
    {
      if (*cursor != '/')
        {
          continue;
        }

      *cursor = '\0';
      ret = ny_health_mkdir(copy);
      *cursor = '/';
      if (ret < 0)
        {
          return ret;
        }
    }

  return ny_health_mkdir(copy);
}

static int ny_health_ensure_store(const char *id)
{
  struct stat status;
  char plugin_store[PATH_MAX];
  int length;
  int ret;

  ret = ny_health_mkdir_parents(CONFIG_NYABULA_CORE_HEALTH_STORE);
  if (ret < 0)
    {
      return ret;
    }

  length = snprintf(plugin_store, sizeof(plugin_store), "%s/%s",
                    CONFIG_NYABULA_CORE_HEALTH_STORE, id);
  if (length < 0 || length >= (int)sizeof(plugin_store))
    {
      return -ENAMETOOLONG;
    }

  if (mkdir(plugin_store, 0700) == 0)
    {
      return 0;
    }

  if (errno != EEXIST || lstat(plugin_store, &status) < 0 ||
      !S_ISDIR(status.st_mode))
    {
      return -errno;
    }

  return 0;
}

static int ny_health_read(const char *path, unsigned int *failures)
{
  char content[24];
  char *end;
  size_t offset = 0;
  unsigned long value;
  int fd;

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  char *stored;
  int ret = ny_state_read(path, &stored, &offset, sizeof(content) - 1);
  if (ret == 0)
    {
      memcpy(content, stored, offset);
      free(stored);
      goto parse;
    }

  if (ret != -ENOENT)
    {
      return ret;
    }
#endif

  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      if (errno == ENOENT)
        {
          *failures = 0;
          return 0;
        }

      return -errno;
    }

  while (offset + 1 < sizeof(content))
    {
      ssize_t count = read(fd, content + offset, sizeof(content) - offset - 1);

      if (count < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          close(fd);
          return -errno;
        }

      if (count == 0)
        {
          break;
        }

      offset += (size_t)count;
    }

  close(fd);
#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
parse:
#endif
  if (offset == 0 || offset + 1 == sizeof(content))
    {
      return -EINVAL;
    }

  content[offset] = '\0';
  errno = 0;
  value = strtoul(content, &end, 10);
  if (errno != 0 || value > UINT_MAX ||
      !(*end == '\0' || (*end == '\n' && end[1] == '\0')))
    {
      return -EINVAL;
    }

  *failures = (unsigned int)value;
  return 0;
}

static int ny_health_write(const char *path, unsigned int failures)
{
  char temporary[PATH_MAX];
  char content[24];
  int length;
  int fd;
  int ret;

  length = snprintf(content, sizeof(content), "%u\n", failures);
  if (length <= 0 || length >= (int)sizeof(content) ||
      snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
          (int)sizeof(temporary))
    {
      return -EINVAL;
    }

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  return ny_state_write(path, content, (size_t)length);
#endif

  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  ret = 0;
  for (size_t offset = 0; offset < (size_t)length;)
    {
      ssize_t count = write(fd, content + offset, (size_t)length - offset);

      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          ret = count < 0 ? -errno : -EIO;
          break;
        }

      offset += (size_t)count;
    }
  if (ret >= 0 && fsync(fd) < 0)
    {
      ret = -errno;
    }

  close(fd);
  if (ret < 0 || rename(temporary, path) < 0)
    {
      if (ret >= 0)
        {
          ret = -errno;
        }

      unlink(temporary);
      return ret;
    }

  return ny_health_sync_parent(path);
}

static int ny_health_sync_parent(const char *path)
{
  char parent[PATH_MAX];
  char *separator;
  int fd;
  int ret;

  if (strlcpy(parent, path, sizeof(parent)) >= sizeof(parent) ||
      (separator = strrchr(parent, '/')) == NULL)
    {
      return -EINVAL;
    }

  *separator = '\0';
  fd = open(parent, O_RDONLY);
  if (fd < 0)
    {
      /* Best effort only: NuttX VFS rejects opening directories (EISDIR)
       * and FAT provides no directory sync semantics.
       */

      return 0;
    }

  ret = fsync(fd) < 0 && errno != EINVAL ? -errno : 0;
  close(fd);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_health_check(const struct ny_plugin_config_s *config)
{
  char path[PATH_MAX];
  unsigned int failures;
  int ret;

  if (config == NULL || !config->module)
    {
      return 0;
    }

  ret = ny_health_path(config->id, config->version, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_health_lock);
  ret = ny_health_read(path, &failures);
  nxmutex_unlock(&g_health_lock);
  return ret < 0                                               ? ret
         : failures >= CONFIG_NYABULA_CORE_QUARANTINE_FAILURES ? -EACCES
                                                               : 0;
}

int ny_health_record_failure(const struct ny_plugin_config_s *config)
{
  char path[PATH_MAX];
  unsigned int failures;
  int ret;

  if (config == NULL || !config->module)
    {
      return 0;
    }

  ret = ny_health_path(config->id, config->version, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_health_lock);
  ret = ny_health_ensure_store(config->id);
  if (ret >= 0 && (ret = ny_health_read(path, &failures)) >= 0)
    {
      if (failures < UINT_MAX)
        {
          failures++;
        }

      ret = ny_health_write(path, failures);
    }

  nxmutex_unlock(&g_health_lock);
  return ret;
}

int ny_health_record_success(const struct ny_plugin_config_s *config)
{
  if (config == NULL || !config->module)
    {
      return 0;
    }

  return ny_health_reset(config->id, config->version);
}

int ny_health_show(const char *id, const char *version)
{
  char path[PATH_MAX];
  unsigned int failures;
  int ret;

  ret = ny_health_path(id, version, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_health_lock);
  ret = ny_health_read(path, &failures);
  nxmutex_unlock(&g_health_lock);
  if (ret >= 0)
    {
      printf("%s %s: failures=%u quarantined=%s\n", id, version, failures,
             failures >= CONFIG_NYABULA_CORE_QUARANTINE_FAILURES ? "yes"
                                                                 : "no");
    }

  return ret;
}

int ny_health_reset(const char *id, const char *version)
{
  char path[PATH_MAX];
  bool removed = false;
  int ret;

  ret = ny_health_path(id, version, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_health_lock);
#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  ret = ny_health_write(path, 0);
  nxmutex_unlock(&g_health_lock);
  return ret;
#endif

  if (unlink(path) == 0)
    {
      removed = true;
      ret = 0;
    }
  else
    {
      ret = errno == ENOENT ? 0 : -errno;
    }

  if (ret >= 0 && removed)
    {
      ret = ny_health_sync_parent(path);
    }

  nxmutex_unlock(&g_health_lock);
  return ret;
}
