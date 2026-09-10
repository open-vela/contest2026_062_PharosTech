/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_broker.c
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ny_broker.h"
#include "ny_manifest.h"
#include "ny_provider.h"
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

static mutex_t g_storage_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ny_broker_valid_key(const char *key);
static int ny_broker_data_path(const struct ny_broker_client_s *client,
                               const char *key, char *path, size_t size);
static int ny_broker_write_all(int fd, const char *buffer, size_t length);
static int ny_broker_storage_budget(const char *path, const char *key,
                                    size_t length);
static int ny_broker_storage_read(const struct ny_broker_client_s *client,
                                  const char *key, char **value,
                                  size_t *length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ny_broker_valid_key(const char *key)
{
  size_t index;
  size_t length;

  if (key == NULL || (length = strlen(key)) == 0 || length >= 64 ||
      strcmp(key, ".") == 0 || strcmp(key, "..") == 0)
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      char value = key[index];

      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '_' ||
            value == '-'))
        {
          return false;
        }
    }

  return true;
}

static int ny_broker_data_path(const struct ny_broker_client_s *client,
                               const char *key, char *path, size_t size)
{
  struct stat status;
  char directory[PATH_MAX];
  int ret;

  if (client == NULL || client->storage_root == NULL ||
      client->storage_root[0] == '\0' || !ny_broker_valid_key(key))
    {
      return -EINVAL;
    }

  ret =
      snprintf(directory, sizeof(directory), "%s/data", client->storage_root);
  if (ret < 0 || ret >= (int)sizeof(directory))
    {
      return -ENAMETOOLONG;
    }

  if (mkdir(directory, 0770) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  if (lstat(directory, &status) < 0 || !S_ISDIR(status.st_mode))
    {
      return -ENOTDIR;
    }

  ret = snprintf(path, size, "%s/%s", directory, key);
  return ret < 0 || ret >= (int)size ? -ENAMETOOLONG : 0;
}

static int ny_broker_storage_budget(const char *path, const char *key,
                                    size_t length)
{
  char directory[PATH_MAX];
  char entry_path[PATH_MAX];
  struct dirent *entry;
  struct stat status;
  DIR *stream;
  uint64_t bytes = length;
  size_t keys = 1;
  char *slash;
  int ret = 0;

  strlcpy(directory, path, sizeof(directory));
  slash = strrchr(directory, '/');
  if (slash == NULL)
    {
      return -EINVAL;
    }

  *slash = '\0';
  stream = opendir(directory);
  if (stream == NULL)
    {
      return -errno;
    }

  for (;;)
    {
      errno = 0;
      entry = readdir(stream);
      if (entry == NULL)
        {
          ret = errno == 0 ? 0 : -errno;
          break;
        }

      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
          continue;
        }

      if (snprintf(entry_path, sizeof(entry_path), "%s/%s", directory,
                   entry->d_name) >= (int)sizeof(entry_path))
        {
          ret = -ENAMETOOLONG;
          break;
        }

      if (lstat(entry_path, &status) < 0)
        {
          ret = -errno;
          break;
        }

      if (!S_ISREG(status.st_mode) || status.st_size < 0)
        {
          ret = -EINVAL;
          break;
        }

      if (strcmp(entry->d_name, key) != 0)
        {
          /* Check before addition so even corrupt sizes cannot overflow. */

          if ((uint64_t)status.st_size >
              CONFIG_NYABULA_CORE_STORAGE_BYTES_LIMIT - bytes)
            {
              ret = -EDQUOT;
              break;
            }

          bytes += status.st_size;
          keys++;
        }

      if (keys > CONFIG_NYABULA_CORE_STORAGE_KEYS_LIMIT)
        {
          ret = -EDQUOT;
          break;
        }
    }

  closedir(stream);
  return ret;
}

static int ny_broker_write_all(int fd, const char *buffer, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t count = write(fd, buffer + offset, length - offset);

      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          return count < 0 ? -errno : -EIO;
        }

      offset += (size_t)count;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_broker_storage_get(const struct ny_broker_client_s *client,
                          const char *key, char **value, size_t *length)
{
  int ret;

  if (value != NULL)
    {
      *value = NULL;
    }

  if (length != NULL)
    {
      *length = 0;
    }

  ret = nxmutex_lock(&g_storage_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_broker_storage_read(client, key, value, length);
  nxmutex_unlock(&g_storage_lock);
  return ret;
}

static int ny_broker_storage_read(const struct ny_broker_client_s *client,
                                  const char *key, char **value,
                                  size_t *length)
{
  char path[PATH_MAX];
  struct stat status;
  size_t offset = 0;
  size_t size;
  char extra;
  ssize_t count;
  int fd;
  int ret;

  if (client == NULL || value == NULL || length == NULL ||
      (client->permissions & NY_PERMISSION_STORAGE_READ) == 0)
    {
      return -EACCES;
    }

  ret = ny_broker_data_path(client, key, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  ret = ny_state_read(path, value, length,
                      CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT);
  if (ret != -ENOENT)
    {
      return ret;
    }
#endif

  if (lstat(path, &status) < 0)
    {
      return -errno;
    }

  if (!S_ISREG(status.st_mode))
    {
      return -EINVAL;
    }

  fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &status) < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      status.st_size > CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT)
    {
      ret = S_ISREG(status.st_mode) ? -EFBIG : -EINVAL;
      close(fd);
      return ret;
    }

  size = (size_t)status.st_size;
  *value = malloc(size + 1);
  if (*value == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  ret = 0;
  while (offset < size)
    {
      count = read(fd, *value + offset, size - offset);
      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          ret = count < 0 ? -errno : -EIO;
          break;
        }

      offset += count;
    }

  if (ret == 0)
    {
      do
        {
          count = read(fd, &extra, 1);
        }
      while (count < 0 && errno == EINTR);

      ret = count < 0 ? -errno : count != 0 ? -EFBIG : 0;
    }

  close(fd);
  if (ret < 0)
    {
      free(*value);
      *value = NULL;
      return ret;
    }

  (*value)[size] = '\0';
  *length = size;
  return 0;
}

int ny_broker_storage_put(const struct ny_broker_client_s *client,
                          const char *key, const char *value, size_t length)
{
  char path[PATH_MAX];
  int fd;
  int ret;

  if (client == NULL || value == NULL ||
      (client->permissions & NY_PERMISSION_STORAGE_WRITE) == 0)
    {
      return -EACCES;
    }

  if (length > CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT)
    {
      return -EFBIG;
    }

  if (length > CONFIG_NYABULA_CORE_STORAGE_BYTES_LIMIT)
    {
      return -EDQUOT;
    }

  ret = nxmutex_lock(&g_storage_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_broker_data_path(client, key, path, sizeof(path));
#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  if (ret >= 0)
    {
      ret = ny_state_storage_write(path, value, length,
                                   CONFIG_NYABULA_CORE_STORAGE_BYTES_LIMIT,
                                   CONFIG_NYABULA_CORE_STORAGE_KEYS_LIMIT);
    }
  nxmutex_unlock(&g_storage_lock);
  return ret;
#endif

  if (ret >= 0)
    {
      ret = ny_broker_storage_budget(path, key, length);
    }

  if (ret < 0)
    {
      nxmutex_unlock(&g_storage_lock);
      return ret;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0660);
  if (fd < 0)
    {
      ret = -errno;
      nxmutex_unlock(&g_storage_lock);
      return ret;
    }

  ret = ny_broker_write_all(fd, value, length);
  close(fd);
  nxmutex_unlock(&g_storage_lock);
  return ret;
}

int ny_broker_network_request(const struct ny_broker_client_s *client)
{
  if (client == NULL ||
      (client->permissions & NY_PERMISSION_NETWORK_REQUEST) == 0)
    {
      return -EACCES;
    }

#ifdef CONFIG_NYABULA_CORE_MOCK_CAPABILITIES
  return 0;
#else
  return -ENOSYS;
#endif
}

int ny_broker_ui_notify(const struct ny_broker_client_s *client,
                        const char *message, size_t length)
{
  if (client == NULL || client->id == NULL || message == NULL ||
      length > INT_MAX || (client->permissions & NY_PERMISSION_UI_NOTIFY) == 0)
    {
      return -EACCES;
    }

  return ny_provider_ui_notify(client->id, message, length);
}

int ny_broker_ai_invoke(const struct ny_broker_client_s *client,
                        const char *prompt, size_t prompt_length,
                        char *response, size_t response_capacity)
{
  int ret;

  if (client == NULL || client->id == NULL || prompt == NULL ||
      response == NULL || (client->permissions & NY_PERMISSION_AI_INVOKE) == 0)
    {
      return -EACCES;
    }

  ret = ny_provider_ai_invoke(client->id, prompt, prompt_length, response,
                              response_capacity);
  return ret >= 0 && (size_t)ret > response_capacity ? -EOVERFLOW : ret;
}
