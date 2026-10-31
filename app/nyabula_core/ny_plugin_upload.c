/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_plugin_upload.c
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mbedtls/base64.h>
#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ny_manifest.h"
#include "ny_package.h"
#include "ny_plugin_upload.h"

#define NY_UPLOAD_MAX_BYTES  (8 * 1024 * 1024)
#define NY_UPLOAD_MAX_FILES  128
#define NY_UPLOAD_PATH_MAX   240
#define NY_UPLOAD_OWNER_SIZE 129
#define NY_UPLOAD_CHUNK_SIZE 6144
#define NY_UPLOAD_TIMEOUT_MS (5 * 60 * 1000)

struct ny_plugin_upload_s
{
  char root[PATH_MAX];
  char owner[NY_UPLOAD_OWNER_SIZE];
  char path[NY_UPLOAD_PATH_MAX + 1];
  size_t offset;
  size_t bytes;
  unsigned int files;
  uint64_t touched;
};

/* Serialized by the plugin manager lock, including commit and cancellation. */

static struct ny_plugin_upload_s g_upload;
static const char *ny_plugin_upload_string(const cJSON *data,
                                           const char *name);
static int ny_plugin_upload_cleanup(const char *root);
static bool ny_plugin_upload_path(const char *path);
static int ny_plugin_upload_chunk(const cJSON *data);

/****************************************************************************
 * Name: ny_plugin_upload_string
 ****************************************************************************/

static const char *ny_plugin_upload_string(const cJSON *data, const char *name)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, name);
  return cJSON_IsString(value) ? value->valuestring : NULL;
}

/****************************************************************************
 * Name: ny_plugin_upload_cleanup
 ****************************************************************************/

static int ny_plugin_upload_cleanup(const char *root)
{
  struct dirent *entry;
  struct stat st;
  char path[PATH_MAX];
  DIR *dir = opendir(root);
  int ret = 0;

  if (dir == NULL)
    {
      return -errno;
    }

  while ((entry = readdir(dir)) != NULL)
    {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        {
          continue;
        }

      if (snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >=
          sizeof(path))
        {
          ret = -ENAMETOOLONG;
          break;
        }

      if (lstat(path, &st) < 0)
        {
          ret = -errno;
          break;
        }

      ret = S_ISDIR(st.st_mode) ? ny_plugin_upload_cleanup(path)
            : unlink(path) < 0  ? -errno
                                : 0;
      if (ret < 0)
        {
          break;
        }
    }

  closedir(dir);
  return ret < 0 ? ret : rmdir(root) < 0 ? -errno : 0;
}

/****************************************************************************
 * Name: ny_plugin_upload_path
 ****************************************************************************/

static bool ny_plugin_upload_path(const char *path)
{
  const char *part = path;
  const char *cursor;
  size_t length;

  if (path == NULL || (length = strlen(path)) == 0 ||
      length > NY_UPLOAD_PATH_MAX)
    {
      return false;
    }

  for (cursor = path;; cursor++)
    {
      if (*cursor == '/' || *cursor == '\0')
        {
          size_t size = cursor - part;
          if (size == 0 || (size == 1 && *part == '.') ||
              (size == 2 && part[0] == '.' && part[1] == '.'))
            {
              return false;
            }

          part = cursor + 1;
          if (*cursor == '\0')
            {
              return true;
            }
        }
      else if ((unsigned char)*cursor < 32 || *cursor == '\\' ||
               *cursor == ':')
        {
          return false;
        }
    }
}

/****************************************************************************
 * Name: ny_plugin_upload_chunk
 ****************************************************************************/

static int ny_plugin_upload_chunk(const cJSON *data)
{
  const char *path = ny_plugin_upload_string(data, "path");
  const char *encoded = ny_plugin_upload_string(data, "data");
  const cJSON *offset = cJSON_GetObjectItemCaseSensitive(data, "offset");
  unsigned char bytes[NY_UPLOAD_CHUNK_SIZE];
  char target[PATH_MAX];
  char *cursor;
  size_t length;
  size_t written = 0;
  bool fresh;
  int fd;
  int ret;

  if (!ny_plugin_upload_path(path) || encoded == NULL ||
      strlen(encoded) > NY_UPLOAD_CHUNK_SIZE * 4 / 3 ||
      !cJSON_IsNumber(offset) || offset->valuedouble < 0 ||
      offset->valuedouble > NY_UPLOAD_MAX_BYTES ||
      offset->valuedouble != (size_t)offset->valuedouble)
    {
      return -EINVAL;
    }

  ret = mbedtls_base64_decode(bytes, sizeof(bytes), &length,
                              (const unsigned char *)encoded, strlen(encoded));
  if (ret != 0 || g_upload.bytes + length > NY_UPLOAD_MAX_BYTES)
    {
      return -EFBIG;
    }

  fresh = g_upload.files == 0 || strcmp(path, g_upload.path) != 0;
  if ((fresh &&
       (offset->valuedouble != 0 || g_upload.files >= NY_UPLOAD_MAX_FILES)) ||
      (!fresh && offset->valuedouble != g_upload.offset))
    {
      return -EINVAL;
    }

  if (snprintf(target, sizeof(target), "%s/%s", g_upload.root, path) >=
      sizeof(target))
    {
      return -ENAMETOOLONG;
    }

  for (cursor = target + strlen(g_upload.root) + 1; *cursor; cursor++)
    {
      if (*cursor == '/')
        {
          *cursor = '\0';
          ret = mkdir(target, 0700);
          *cursor = '/';
          if (ret < 0 && errno != EEXIST)
            {
              return -errno;
            }
        }
    }

  fd = open(target,
            O_WRONLY | O_APPEND | O_NOFOLLOW | (fresh ? O_CREAT | O_EXCL : 0),
            0600);
  if (fd < 0)
    {
      return -errno;
    }

  if (fresh)
    {
      g_upload.files++;
      g_upload.offset = 0;
      strlcpy(g_upload.path, path, sizeof(g_upload.path));
    }

  while (written < length)
    {
      ssize_t count = write(fd, bytes + written, length - written);
      if (count <= 0)
        {
          ret = count < 0 ? -errno : -EIO;
          close(fd);
          return ret;
        }

      written += count;
      g_upload.offset += count;
      g_upload.bytes += count;
    }

  return close(fd) < 0 ? -errno : 0;
}

/****************************************************************************
 * Name: ny_plugin_upload_request
 ****************************************************************************/

int ny_plugin_upload_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result)
{
  const char *id = ny_plugin_upload_string(data, "uploadId");
  const char *token;
  uint64_t now = ny_product_time_ms(true);
  int ret;

  if (caller->role != NY_PRODUCT_OWNER || !caller->local_transport ||
      caller->id == NULL || strlen(caller->id) >= sizeof(g_upload.owner))
    {
      return -EACCES;
    }

  if (g_upload.root[0] && now - g_upload.touched > NY_UPLOAD_TIMEOUT_MS)
    {
      ny_plugin_upload_cleanup(g_upload.root);
      memset(&g_upload, 0, sizeof(g_upload));
    }

  if (strcmp(topic, "plugin.upload.begin") == 0)
    {
      if (g_upload.root[0])
        {
          return -EBUSY;
        }

      /* /tmp is the board's private RAM staging filesystem. */

      strlcpy(g_upload.root, "/tmp/ny-plugin-XXXXXX", sizeof(g_upload.root));
      if (mkdtemp(g_upload.root) == NULL)
        {
          g_upload.root[0] = '\0';
          return -errno;
        }

      strlcpy(g_upload.owner, caller->id, sizeof(g_upload.owner));
      g_upload.touched = now;
      token = strrchr(g_upload.root, '/') + 1;
      *result = cJSON_CreateObject();
      if (*result == NULL ||
          !cJSON_AddStringToObject(*result, "uploadId", token))
        {
          ny_plugin_upload_cleanup(g_upload.root);
          memset(&g_upload, 0, sizeof(g_upload));
          return -ENOMEM;
        }

      return 0;
    }

  token = strrchr(g_upload.root, '/');
  if (id == NULL || token == NULL || strcmp(id, token + 1) != 0 ||
      strcmp(caller->id, g_upload.owner) != 0)
    {
      return -ENOENT;
    }

  g_upload.touched = now;
  if (strcmp(topic, "plugin.upload.chunk") == 0)
    {
      ret = ny_plugin_upload_chunk(data);
    }
  else if (strcmp(topic, "plugin.upload.commit") == 0)
    {
      struct ny_plugin_config_s config;
      char active[PATH_MAX];
      ret = ny_manifest_load(g_upload.root, &config);
      if (ret == 0)
        {
          ret = ny_package_install(g_upload.root);
        }

      if (ret == 0 &&
          ny_package_resolve(config.id, active, sizeof(active)) == -ENOENT)
        {
          ret = ny_package_activate(config.id, config.version);
        }

      ny_plugin_upload_cleanup(g_upload.root);
      memset(&g_upload, 0, sizeof(g_upload));
    }
  else if (strcmp(topic, "plugin.upload.cancel") == 0)
    {
      ret = ny_plugin_upload_cleanup(g_upload.root);
      memset(&g_upload, 0, sizeof(g_upload));
    }
  else
    {
      return -ENOSYS;
    }

  if (ret == 0)
    {
      *result = cJSON_CreateObject();
      if (*result == NULL)
        {
          return -ENOMEM;
        }
    }

  return ret;
}
