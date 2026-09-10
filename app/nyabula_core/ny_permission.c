/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_permission.c
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

#include "ny_permission.h"
#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
#include "ny_state.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_GRANT_STORE_LIMIT 65536

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_permission_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ny_permission_valid_id(const char *id);
static int ny_permission_write_all(int fd, const char *buffer, size_t length);
static int ny_permission_read_all(int fd, char *buffer, size_t length);
static int ny_permission_read(cJSON **store);
static int ny_permission_validate_store(const cJSON *store);
static int ny_permission_parse_mask(const cJSON *array, uint64_t *mask);
static int ny_permission_ensure_parent(void);
static int ny_permission_save(cJSON *store);
static int ny_permission_update(const char *id, const char *permission,
                                bool grant);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ny_permission_valid_id(const char *id)
{
  size_t index;
  size_t length;

  if (id == NULL || (length = strlen(id)) == 0 ||
      length >= NY_PLUGIN_ID_SIZE || id[0] == '.' || id[length - 1] == '.')
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      char value = id[index];

      if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
            value == '.' || value == '_' || value == '-'))
        {
          return false;
        }
    }

  return true;
}

static int ny_permission_write_all(int fd, const char *buffer, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t count = write(fd, buffer + offset, length - offset);

      if (count < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (count == 0)
        {
          return -EIO;
        }

      offset += (size_t)count;
    }

  return 0;
}

static int ny_permission_read_all(int fd, char *buffer, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t count = read(fd, buffer + offset, length - offset);

      if (count < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (count == 0)
        {
          return -EIO;
        }

      offset += (size_t)count;
    }

  return 0;
}

static int ny_permission_validate_store(const cJSON *store)
{
  const cJSON *entry;

  if (!cJSON_IsObject(store))
    {
      return -EINVAL;
    }

  cJSON_ArrayForEach(entry, store)
  {
    const cJSON *other;

    if (entry->string == NULL || !ny_permission_valid_id(entry->string) ||
        !cJSON_IsArray(entry))
      {
        return -EINVAL;
      }

    for (other = entry->next; other != NULL; other = other->next)
      {
        if (other->string != NULL && strcmp(entry->string, other->string) == 0)
          {
            return -EINVAL;
          }
      }
  }

  return 0;
}

static int ny_permission_read(cJSON **store)
{
  struct stat status;
  char *content;
  const char *end;
  int ret;
  int fd;

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  size_t stored_length;
  ret = ny_state_read(CONFIG_NYABULA_CORE_GRANT_STORE, &content,
                      &stored_length, NY_GRANT_STORE_LIMIT);
  if (ret == 0)
    {
      goto parse;
    }

  if (ret != -ENOENT)
    {
      return ret;
    }
#endif

  fd = open(CONFIG_NYABULA_CORE_GRANT_STORE, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      if (errno == ENOENT)
        {
          *store = cJSON_CreateObject();
          return *store == NULL ? -ENOMEM : 0;
        }

      return -errno;
    }

  if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || status.st_size > NY_GRANT_STORE_LIMIT)
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

  ret = ny_permission_read_all(fd, content, (size_t)status.st_size);
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
  ret = ny_permission_validate_store(*store);
  if (ret < 0)
    {
      cJSON_Delete(*store);
      *store = NULL;
    }

  return ret;
}

static int ny_permission_parse_mask(const cJSON *array, uint64_t *mask)
{
  const cJSON *item;

  if (!cJSON_IsArray(array))
    {
      return -EINVAL;
    }

  *mask = 0;
  cJSON_ArrayForEach(item, array)
  {
    uint64_t value;

    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        ny_manifest_permission_value(item->valuestring, &value) < 0 ||
        (*mask & value) != 0)
      {
        return -EINVAL;
      }

    *mask |= value;
  }

  return 0;
}

static int ny_permission_ensure_parent(void)
{
  char path[PATH_MAX];
  char *separator;

  strlcpy(path, CONFIG_NYABULA_CORE_GRANT_STORE, sizeof(path));
  separator = strrchr(path, '/');
  if (separator == NULL || separator == path)
    {
      return 0;
    }

  *separator = '\0';
  if (mkdir(path, 0770) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

static int ny_permission_save(cJSON *store)
{
  char parent[PATH_MAX];
  char temporary[PATH_MAX];
  char *separator;
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
  if (length == 0 || length > NY_GRANT_STORE_LIMIT ||
      snprintf(temporary, sizeof(temporary), "%s.tmp",
               CONFIG_NYABULA_CORE_GRANT_STORE) >= (int)sizeof(temporary))
    {
      free(content);
      return -EFBIG;
    }

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  ret = ny_state_write(CONFIG_NYABULA_CORE_GRANT_STORE, content, length);
  free(content);
  return ret;
#endif

  ret = ny_permission_ensure_parent();
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

  ret = ny_permission_write_all(fd, content, length);
  if (ret >= 0 && fsync(fd) < 0)
    {
      ret = -errno;
    }

  close(fd);
  free(content);
  if (ret < 0)
    {
      unlink(temporary);
      return ret;
    }

  if (rename(temporary, CONFIG_NYABULA_CORE_GRANT_STORE) < 0)
    {
      ret = -errno;
      unlink(temporary);
      return ret;
    }

  strlcpy(parent, CONFIG_NYABULA_CORE_GRANT_STORE, sizeof(parent));
  separator = strrchr(parent, '/');
  if (separator != NULL)
    {
      *separator = separator == parent ? '/' : '\0';
      fd = open(parent, O_RDONLY);
      if (fd >= 0)
        {
          ret = fsync(fd) < 0 && errno != EINVAL ? -errno : 0;
          close(fd);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return 0;
}

static int ny_permission_update(const char *id, const char *permission,
                                bool grant)
{
  cJSON *store = NULL;
  cJSON *array;
  cJSON *item;
  cJSON *permission_item;
  uint64_t value;
  int index = 0;
  int ret;

  if (!ny_permission_valid_id(id) ||
      ny_manifest_permission_value(permission, &value) < 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_permission_lock);
  ret = ny_permission_read(&store);
  if (ret < 0)
    {
      goto out;
    }

  array = cJSON_GetObjectItemCaseSensitive(store, id);
  if (array == NULL && grant)
    {
      array = cJSON_AddArrayToObject(store, id);
    }

  if (!cJSON_IsArray(array))
    {
      ret = array == NULL ? -ENOENT : -EINVAL;
      goto out;
    }

  cJSON_ArrayForEach(item, array)
  {
    if (cJSON_IsString(item) && item->valuestring != NULL &&
        strcmp(item->valuestring, permission) == 0)
      {
        if (grant)
          {
            ret = 0;
            goto out;
          }

        cJSON_DeleteItemFromArray(array, index);
        ret = ny_permission_save(store);
        goto out;
      }

    index++;
  }

  if (!grant)
    {
      ret = -ENOENT;
      goto out;
    }

  permission_item = cJSON_CreateString(permission);
  if (permission_item == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  cJSON_AddItemToArray(array, permission_item);

  ret = ny_permission_save(store);

out:
  cJSON_Delete(store);
  nxmutex_unlock(&g_permission_lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_permission_apply(struct ny_plugin_config_s *config)
{
  cJSON *store = NULL;
  const cJSON *array;
  uint64_t granted = 0;
  int ret;

  if (config == NULL || !ny_permission_valid_id(config->id))
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_permission_lock);
  ret = ny_permission_read(&store);
  if (ret >= 0)
    {
      array = cJSON_GetObjectItemCaseSensitive(store, config->id);
      if (array != NULL)
        {
          ret = ny_permission_parse_mask(array, &granted);
        }
    }

  if (ret >= 0)
    {
      config->permissions = config->requested_permissions & granted;
    }

  cJSON_Delete(store);
  nxmutex_unlock(&g_permission_lock);
  return ret;
}

int ny_permission_grant(const char *id, const char *permission)
{
  return ny_permission_update(id, permission, true);
}

int ny_permission_revoke(const char *id, const char *permission)
{
  return ny_permission_update(id, permission, false);
}

int ny_permission_show(const char *id)
{
  cJSON *store = NULL;
  const cJSON *array;
  uint64_t mask = 0;
  uint64_t bit;
  int ret;

  if (!ny_permission_valid_id(id))
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_permission_lock);
  ret = ny_permission_read(&store);
  if (ret >= 0)
    {
      array = cJSON_GetObjectItemCaseSensitive(store, id);
      ret = array == NULL ? 0 : ny_permission_parse_mask(array, &mask);
    }

  nxmutex_unlock(&g_permission_lock);
  cJSON_Delete(store);
  if (ret < 0)
    {
      return ret;
    }

  printf("%s:", id);
  for (bit = 1; bit != 0; bit <<= 1)
    {
      const char *name;

      if ((mask & bit) != 0 &&
          (name = ny_manifest_permission_name(bit)) != NULL)
        {
          printf(" %s", name);
        }
    }

  putchar('\n');
  return 0;
}
