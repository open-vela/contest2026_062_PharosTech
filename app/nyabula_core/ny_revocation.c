/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_revocation.c
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

#include <netutils/cJSON.h>

#include "ny_manifest.h"
#include "ny_revocation.h"
#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
#include "ny_state.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_REVOCATION_LIMIT   65536
#define NY_KEY_ID_SIZE        64
#define NY_PACKAGE_TOKEN_SIZE (NY_PLUGIN_ID_SIZE + NY_PLUGIN_VERSION_SIZE + 1)

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_revocation_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ny_revocation_valid_component(const char *value, size_t limit,
                                          bool uppercase);
static int ny_revocation_package_token(const char *id, const char *version,
                                       char *token, size_t size);
static bool ny_revocation_valid_package_token(const char *token);
static int ny_revocation_read_all(int fd, char *buffer, size_t length);
static int ny_revocation_write_all(int fd, const char *buffer, size_t length);
static int ny_revocation_validate_array(const cJSON *array, size_t limit,
                                        bool package);
static int ny_revocation_validate(const cJSON *store);
static int ny_revocation_read(cJSON **store);
static int ny_revocation_mkdir(const char *path);
static int ny_revocation_ensure_parent(void);
static int ny_revocation_sync_parent(void);
static int ny_revocation_save(cJSON *store);
static int ny_revocation_contains(const char *kind, const char *value);
static int ny_revocation_update(const char *kind, const char *value,
                                bool revoked);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ny_revocation_valid_component(const char *value, size_t limit,
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

static int ny_revocation_package_token(const char *id, const char *version,
                                       char *token, size_t size)
{
  int length;

  if (!ny_revocation_valid_component(id, NY_PLUGIN_ID_SIZE, false) ||
      !ny_revocation_valid_component(version, NY_PLUGIN_VERSION_SIZE, true))
    {
      return -EINVAL;
    }

  length = snprintf(token, size, "%s@%s", id, version);
  return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

static bool ny_revocation_valid_package_token(const char *token)
{
  char id[NY_PLUGIN_ID_SIZE];
  char version[NY_PLUGIN_VERSION_SIZE];
  const char *separator = strchr(token, '@');
  size_t id_length;

  if (separator == NULL || strchr(separator + 1, '@') != NULL)
    {
      return false;
    }

  id_length = (size_t)(separator - token);
  if (id_length == 0 || id_length >= sizeof(id) ||
      strlcpy(version, separator + 1, sizeof(version)) >= sizeof(version))
    {
      return false;
    }

  memcpy(id, token, id_length);
  id[id_length] = '\0';
  return ny_revocation_valid_component(id, sizeof(id), false) &&
         ny_revocation_valid_component(version, sizeof(version), true);
}

static int ny_revocation_read_all(int fd, char *buffer, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t count = read(fd, buffer + offset, length - offset);

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

static int ny_revocation_write_all(int fd, const char *buffer, size_t length)
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

static int ny_revocation_validate_array(const cJSON *array, size_t limit,
                                        bool package)
{
  const cJSON *item;

  if (!cJSON_IsArray(array))
    {
      return -EINVAL;
    }

  cJSON_ArrayForEach(item, array)
  {
    const cJSON *other;

    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) == 0 || strlen(item->valuestring) >= limit ||
        (!package && !ny_revocation_valid_component(item->valuestring,
                                                    NY_KEY_ID_SIZE, true)))
      {
        return -EINVAL;
      }

    if (package && !ny_revocation_valid_package_token(item->valuestring))
      {
        return -EINVAL;
      }

    for (other = item->next; other != NULL; other = other->next)
      {
        if (cJSON_IsString(other) && other->valuestring != NULL &&
            strcmp(item->valuestring, other->valuestring) == 0)
          {
            return -EINVAL;
          }
      }
  }

  return 0;
}

static int ny_revocation_validate(const cJSON *store)
{
  const cJSON *item;
  int keys = 0;
  int packages = 0;

  if (!cJSON_IsObject(store))
    {
      return -EINVAL;
    }

  cJSON_ArrayForEach(item, store)
  {
    if (item->string != NULL && strcmp(item->string, "keys") == 0)
      {
        keys++;
        if (ny_revocation_validate_array(item, NY_KEY_ID_SIZE, false) < 0)
          {
            return -EINVAL;
          }
      }
    else if (item->string != NULL && strcmp(item->string, "packages") == 0)
      {
        packages++;
        if (ny_revocation_validate_array(item, NY_PACKAGE_TOKEN_SIZE, true) <
            0)
          {
            return -EINVAL;
          }
      }
    else
      {
        return -EINVAL;
      }
  }

  return keys == 1 && packages == 1 ? 0 : -EINVAL;
}

static int ny_revocation_read(cJSON **store)
{
  struct stat status;
  char *content;
  const char *end;
  int fd;
  int ret;

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  size_t stored_length;
  ret = ny_state_read(CONFIG_NYABULA_CORE_REVOCATION_STORE, &content,
                      &stored_length, NY_REVOCATION_LIMIT);
  if (ret == 0)
    {
      goto parse;
    }

  if (ret != -ENOENT)
    {
      return ret;
    }
#endif

  fd = open(CONFIG_NYABULA_CORE_REVOCATION_STORE, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      if (errno != ENOENT)
        {
          return -errno;
        }

      *store = cJSON_CreateObject();
      if (*store == NULL || cJSON_AddArrayToObject(*store, "keys") == NULL ||
          cJSON_AddArrayToObject(*store, "packages") == NULL)
        {
          cJSON_Delete(*store);
          *store = NULL;
          return -ENOMEM;
        }

      return 0;
    }

  if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || status.st_size > NY_REVOCATION_LIMIT)
    {
      close(fd);
      return -EINVAL;
    }

  content = malloc((size_t)status.st_size + 1);
  if (content == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  ret = ny_revocation_read_all(fd, content, (size_t)status.st_size);
  close(fd);
  if (ret < 0)
    {
      free(content);
      return ret;
    }

  content[status.st_size] = '\0';
#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
parse:
#endif
  *store = cJSON_ParseWithOpts(content, &end, true);
  free(content);
  ret = ny_revocation_validate(*store);
  if (ret < 0)
    {
      cJSON_Delete(*store);
      *store = NULL;
    }

  return ret;
}

static int ny_revocation_mkdir(const char *path)
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

static int ny_revocation_ensure_parent(void)
{
  char path[PATH_MAX];
  char *cursor;
  char *separator;
  int ret;

  if (strlcpy(path, CONFIG_NYABULA_CORE_REVOCATION_STORE, sizeof(path)) >=
          sizeof(path) ||
      (separator = strrchr(path, '/')) == NULL)
    {
      return -EINVAL;
    }

  *separator = '\0';
  for (cursor = path + 1; *cursor != '\0'; cursor++)
    {
      if (*cursor != '/')
        {
          continue;
        }

      *cursor = '\0';
      ret = ny_revocation_mkdir(path);
      *cursor = '/';
      if (ret < 0)
        {
          return ret;
        }
    }

  return ny_revocation_mkdir(path);
}

static int ny_revocation_sync_parent(void)
{
  char path[PATH_MAX];
  char *separator;
  int fd;
  int ret;

  if (strlcpy(path, CONFIG_NYABULA_CORE_REVOCATION_STORE, sizeof(path)) >=
          sizeof(path) ||
      (separator = strrchr(path, '/')) == NULL)
    {
      return -EINVAL;
    }

  *separator = '\0';
  fd = open(path, O_RDONLY);
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

static int ny_revocation_save(cJSON *store)
{
  char temporary[PATH_MAX];
  char *content;
  size_t length;
  int fd;
  int ret;

  content = cJSON_PrintUnformatted(store);
  if (content == NULL)
    {
      return -ENOMEM;
    }

  length = strlen(content);
  if (length == 0 || length > NY_REVOCATION_LIMIT ||
      snprintf(temporary, sizeof(temporary), "%s.tmp",
               CONFIG_NYABULA_CORE_REVOCATION_STORE) >= (int)sizeof(temporary))
    {
      free(content);
      return -EFBIG;
    }

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  ret = ny_state_write(CONFIG_NYABULA_CORE_REVOCATION_STORE, content, length);
  free(content);
  return ret;
#endif

  ret = ny_revocation_ensure_parent();
  if (ret < 0)
    {
      free(content);
      return ret;
    }

  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0)
    {
      free(content);
      return -errno;
    }

  ret = ny_revocation_write_all(fd, content, length);
  if (ret >= 0 && fsync(fd) < 0)
    {
      ret = -errno;
    }

  close(fd);
  free(content);
  if (ret < 0 || rename(temporary, CONFIG_NYABULA_CORE_REVOCATION_STORE) < 0)
    {
      if (ret >= 0)
        {
          ret = -errno;
        }

      unlink(temporary);
      return ret;
    }

  return ny_revocation_sync_parent();
}

static int ny_revocation_contains(const char *kind, const char *value)
{
  cJSON *store = NULL;
  const cJSON *array;
  const cJSON *item;
  int ret;

  nxmutex_lock(&g_revocation_lock);
  ret = ny_revocation_read(&store);
  if (ret >= 0)
    {
      array = cJSON_GetObjectItemCaseSensitive(store, kind);
      cJSON_ArrayForEach(item, array)
      {
        if (strcmp(item->valuestring, value) == 0)
          {
            ret = -EACCES;
            break;
          }
      }
    }

  cJSON_Delete(store);
  nxmutex_unlock(&g_revocation_lock);
  return ret;
}

static int ny_revocation_update(const char *kind, const char *value,
                                bool revoked)
{
  cJSON *store = NULL;
  cJSON *array;
  cJSON *item;
  int index = 0;
  int ret;

  nxmutex_lock(&g_revocation_lock);
  ret = ny_revocation_read(&store);
  if (ret < 0)
    {
      goto out;
    }

  array = cJSON_GetObjectItemCaseSensitive(store, kind);
  cJSON_ArrayForEach(item, array)
  {
    if (strcmp(item->valuestring, value) == 0)
      {
        if (!revoked)
          {
            cJSON_DeleteItemFromArray(array, index);
            ret = ny_revocation_save(store);
          }

        goto out;
      }

    index++;
  }

  if (!revoked)
    {
      ret = 0;
      goto out;
    }

  item = cJSON_CreateString(value);
  if (item == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  cJSON_AddItemToArray(array, item);
  ret = ny_revocation_save(store);

out:
  cJSON_Delete(store);
  nxmutex_unlock(&g_revocation_lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_revocation_check_key(const char *key_id)
{
  return ny_revocation_valid_component(key_id, NY_KEY_ID_SIZE, true)
             ? ny_revocation_contains("keys", key_id)
             : -EINVAL;
}

int ny_revocation_check_package(const char *id, const char *version)
{
  char token[NY_PACKAGE_TOKEN_SIZE];
  int ret = ny_revocation_package_token(id, version, token, sizeof(token));

  return ret < 0 ? ret : ny_revocation_contains("packages", token);
}

int ny_revocation_update_key(const char *key_id, bool revoked)
{
  return ny_revocation_valid_component(key_id, NY_KEY_ID_SIZE, true)
             ? ny_revocation_update("keys", key_id, revoked)
             : -EINVAL;
}

int ny_revocation_update_package(const char *id, const char *version,
                                 bool revoked)
{
  char token[NY_PACKAGE_TOKEN_SIZE];
  int ret = ny_revocation_package_token(id, version, token, sizeof(token));

  return ret < 0 ? ret : ny_revocation_update("packages", token, revoked);
}

int ny_revocation_show(void)
{
  cJSON *store = NULL;
  const cJSON *array;
  const cJSON *item;
  int ret;

  nxmutex_lock(&g_revocation_lock);
  ret = ny_revocation_read(&store);
  if (ret >= 0)
    {
      array = cJSON_GetObjectItemCaseSensitive(store, "keys");
      cJSON_ArrayForEach(item, array)
      {
        printf("key %s\n", item->valuestring);
      }

      array = cJSON_GetObjectItemCaseSensitive(store, "packages");
      cJSON_ArrayForEach(item, array)
      {
        printf("package %s\n", item->valuestring);
      }
    }

  cJSON_Delete(store);
  nxmutex_unlock(&g_revocation_lock);
  return ret;
}
