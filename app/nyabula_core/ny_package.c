/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_package.c
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

#include "ny_manifest.h"
#include "ny_package.h"
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

static mutex_t g_package_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ny_package_valid_version(const char *version);
static bool ny_package_valid_id(const char *id);
static bool ny_package_mutable_name(const char *name);
static int ny_package_join(char *path, size_t size, const char *left,
                           const char *right);
static int ny_package_mkdir(const char *path);
static int ny_package_mkdir_parents(const char *path);
static int ny_package_write_all(int fd, const void *buffer, size_t length);
static int ny_package_copy_file(const char *source, const char *destination);
static int ny_package_copy_tree(const char *source, const char *destination,
                                bool root);
static int ny_package_remove_tree(const char *path);
static void ny_package_clean_staging(const char *plugin_root);
static int ny_package_sync_directory(const char *path);
static int ny_package_read_marker(const char *plugin_root, const char *name,
                                  char *version, size_t size);
static int ny_package_write_marker(const char *plugin_root, const char *name,
                                   const char *version);
static int ny_package_version_path(const char *id, const char *version,
                                   char *plugin_root, size_t root_size,
                                   char *path, size_t path_size);
static int ny_package_activate_locked(const char *id, const char *version,
                                      bool update_last_good);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ny_package_valid_version(const char *version)
{
  size_t index;
  size_t length;

  if (version == NULL || (length = strlen(version)) == 0 ||
      length >= NY_PLUGIN_VERSION_SIZE || version[0] == '.' ||
      version[length - 1] == '.')
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      char value = version[index];

      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '_' ||
            value == '-'))
        {
          return false;
        }
    }

  return true;
}

static bool ny_package_valid_id(const char *id)
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

static bool ny_package_mutable_name(const char *name)
{
  return strcmp(name, "data") == 0 || strcmp(name, "cache") == 0 ||
         strcmp(name, "tmp") == 0;
}

static int ny_package_join(char *path, size_t size, const char *left,
                           const char *right)
{
  int length = snprintf(path, size, "%s/%s", left, right);

  return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

static int ny_package_mkdir(const char *path)
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

static int ny_package_mkdir_parents(const char *path)
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
      ret = ny_package_mkdir(copy);
      *cursor = '/';
      if (ret < 0)
        {
          return ret;
        }
    }

  return ny_package_mkdir(copy);
}

static int ny_package_write_all(int fd, const void *buffer, size_t length)
{
  const unsigned char *bytes = buffer;
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t count = write(fd, bytes + offset, length - offset);

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

static int ny_package_copy_file(const char *source, const char *destination)
{
  unsigned char buffer[4096];
  int source_fd;
  int destination_fd;
  int ret = 0;

  source_fd = open(source, O_RDONLY | O_NOFOLLOW);
  if (source_fd < 0)
    {
      return -errno;
    }

  destination_fd =
      open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (destination_fd < 0)
    {
      ret = -errno;
      close(source_fd);
      return ret;
    }

  for (;;)
    {
      ssize_t count = read(source_fd, buffer, sizeof(buffer));

      if (count < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          break;
        }

      if (count == 0)
        {
          break;
        }

      ret = ny_package_write_all(destination_fd, buffer, (size_t)count);
      if (ret < 0)
        {
          break;
        }
    }

  if (ret >= 0 && fsync(destination_fd) < 0)
    {
      ret = -errno;
    }

  close(destination_fd);
  close(source_fd);
  return ret;
}

static int ny_package_copy_tree(const char *source, const char *destination,
                                bool root)
{
  struct dirent *entry;
  DIR *directory;
  int ret;

  ret = ny_package_mkdir(destination);
  if (ret < 0)
    {
      return ret;
    }

  directory = opendir(source);
  if (directory == NULL)
    {
      return -errno;
    }

  ret = 0;
  while ((entry = readdir(directory)) != NULL)
    {
      char source_path[PATH_MAX];
      char destination_path[PATH_MAX];
      struct stat status;

      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0 ||
          (root && ny_package_mutable_name(entry->d_name)))
        {
          continue;
        }

      ret = ny_package_join(source_path, sizeof(source_path), source,
                            entry->d_name);
      if (ret < 0 ||
          (ret = ny_package_join(destination_path, sizeof(destination_path),
                                 destination, entry->d_name)) < 0)
        {
          break;
        }

      if (lstat(source_path, &status) < 0)
        {
          ret = -errno;
          break;
        }

      if (S_ISREG(status.st_mode))
        {
          ret = ny_package_copy_file(source_path, destination_path);
        }
      else if (S_ISDIR(status.st_mode))
        {
          ret = ny_package_copy_tree(source_path, destination_path, false);
        }
      else
        {
          ret = -EPERM;
        }

      if (ret < 0)
        {
          break;
        }
    }

  closedir(directory);
  return ret < 0 ? ret : ny_package_sync_directory(destination);
}

static int ny_package_remove_tree(const char *path)
{
  struct stat status;
  struct dirent *entry;
  DIR *directory;
  int ret = 0;

  if (lstat(path, &status) < 0)
    {
      return errno == ENOENT ? 0 : -errno;
    }

  if (!S_ISDIR(status.st_mode))
    {
      return unlink(path) < 0 ? -errno : 0;
    }

  directory = opendir(path);
  if (directory == NULL)
    {
      return -errno;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      char child[PATH_MAX];

      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
          continue;
        }

      ret = ny_package_join(child, sizeof(child), path, entry->d_name);
      if (ret < 0 || (ret = ny_package_remove_tree(child)) < 0)
        {
          break;
        }
    }

  closedir(directory);
  return ret < 0 ? ret : (rmdir(path) < 0 ? -errno : 0);
}

static void ny_package_clean_staging(const char *plugin_root)
{
  struct dirent *entry;
  DIR *directory = opendir(plugin_root);

  if (directory == NULL)
    {
      return;
    }

  /* Best effort: reclaim staging trees left behind by crashed installs.
   * The install lock is held, so no live staging directory can exist.
   */

  while ((entry = readdir(directory)) != NULL)
    {
      char child[PATH_MAX];

      if (strncmp(entry->d_name, ".staging.", 9) == 0 &&
          ny_package_join(child, sizeof(child), plugin_root, entry->d_name) >=
              0)
        {
          ny_package_remove_tree(child);
        }
    }

  closedir(directory);
}

static int ny_package_sync_directory(const char *path)
{
  int fd = open(path, O_RDONLY);
  int ret;

  if (fd < 0)
    {
      /* Best effort only: NuttX VFS rejects opening directories (EISDIR)
       * and FAT provides no directory sync semantics.  Durability of the
       * directory entry itself cannot be guaranteed here.
       */

      return 0;
    }

  ret = fsync(fd) < 0 && errno != EINVAL ? -errno : 0;
  close(fd);
  return ret;
}

static int ny_package_read_marker(const char *plugin_root, const char *name,
                                  char *version, size_t size)
{
  char path[PATH_MAX];
  size_t offset = 0;
  int fd;
  int ret;

  ret = ny_package_join(path, sizeof(path), plugin_root, name);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  char *stored;
  if (size == 0)
    {
      return -EINVAL;
    }

  ret = ny_state_read(path, &stored, &offset, size - 1);
  if (ret == 0)
    {
      memcpy(version, stored, offset);
      free(stored);
      version[offset] = '\0';
      if (offset != 0 && version[offset - 1] == '\n')
        {
          version[offset - 1] = '\0';
        }

      return ny_package_valid_version(version) ? 0 : -EINVAL;
    }

  if (ret != -ENOENT)
    {
      return ret;
    }
#endif

  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      return -errno;
    }

  while (offset + 1 < size)
    {
      ssize_t count = read(fd, version + offset, size - offset - 1);

      if (count < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          close(fd);
          return ret;
        }

      if (count == 0)
        {
          break;
        }

      offset += (size_t)count;
    }

  version[offset] = '\0';
  if (offset == 0 ||
      (offset + 1 == size && read(fd, version + offset, 1) != 0))
    {
      close(fd);
      return -EINVAL;
    }

  close(fd);
  if (version[offset - 1] == '\n')
    {
      version[offset - 1] = '\0';
    }

  return ny_package_valid_version(version) ? 0 : -EINVAL;
}

static int ny_package_write_marker(const char *plugin_root, const char *name,
                                   const char *version)
{
  char temporary[PATH_MAX];
  char path[PATH_MAX];
  char content[NY_PLUGIN_VERSION_SIZE + 1];
  int length;
  int fd;
  int ret;

  ret = ny_package_join(path, sizeof(path), plugin_root, name);
  if (ret < 0 || snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
                     (int)sizeof(temporary))
    {
      return ret < 0 ? ret : -ENAMETOOLONG;
    }

  length = snprintf(content, sizeof(content), "%s\n", version);
  if (length <= 0 || length >= (int)sizeof(content))
    {
      return -EINVAL;
    }

  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  ret = ny_package_write_all(fd, content, (size_t)length);
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

  return ny_package_sync_directory(plugin_root);
}

static int ny_package_version_path(const char *id, const char *version,
                                   char *plugin_root, size_t root_size,
                                   char *path, size_t path_size)
{
  char versions[PATH_MAX];
  int ret;

  if (!ny_package_valid_id(id) || !ny_package_valid_version(version))
    {
      return -EINVAL;
    }

  ret = ny_package_join(plugin_root, root_size,
                        CONFIG_NYABULA_CORE_PACKAGE_STORE, id);
  if (ret < 0 || (ret = ny_package_join(versions, sizeof(versions),
                                        plugin_root, "versions")) < 0)
    {
      return ret;
    }

  return ny_package_join(path, path_size, versions, version);
}

static int ny_package_activate_locked(const char *id, const char *version,
                                      bool update_last_good)
{
  struct ny_plugin_config_s config;
  char plugin_root[PATH_MAX];
  char destination[PATH_MAX];
  char current[NY_PLUGIN_VERSION_SIZE + 1];
  int ret;

  ret = ny_package_version_path(id, version, plugin_root, sizeof(plugin_root),
                                destination, sizeof(destination));
  if (ret < 0 || (ret = ny_manifest_load(destination, &config)) < 0)
    {
      return ret;
    }

  if (strcmp(config.id, id) != 0 || strcmp(config.version, version) != 0)
    {
      return -EINVAL;
    }

  ret =
      ny_package_read_marker(plugin_root, "current", current, sizeof(current));
  if (ret >= 0 && strcmp(current, version) == 0)
    {
      return 0;
    }

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
  if (ret >= 0 || ret == -ENOENT)
    {
      struct ny_state_update_s updates[2];
      char last_good_path[PATH_MAX];
      size_t count = ret >= 0 && update_last_good ? 2 : 1;

      ret = ny_package_join(destination, sizeof(destination), plugin_root,
                            "current");
      if (ret < 0)
        {
          return ret;
        }

      updates[0].key = destination;
      updates[0].value = version;
      updates[0].length = strlen(version);
      ret = ny_package_join(last_good_path, sizeof(last_good_path),
                            plugin_root, "last-good");
      if (ret < 0)
        {
          return ret;
        }

      updates[1].key = last_good_path;
      updates[1].value = current;
      updates[1].length = count == 2 ? strlen(current) : 0;
      return ny_state_write_many(updates, count);
    }

  return ret;
#endif

  if (ret >= 0)
    {
      /* Rollback must not record the version being abandoned as
       * last-known-good, or a failing rollback target would ping-pong
       * between two bad versions.
       */

      if (update_last_good)
        {
          ret = ny_package_write_marker(plugin_root, "last-good", current);
          if (ret < 0)
            {
              return ret;
            }
        }
    }
  else if (ret != -ENOENT)
    {
      return ret;
    }

  return ny_package_write_marker(plugin_root, "current", version);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_package_install(const char *source)
{
  struct ny_plugin_config_s config;
  char plugin_root[PATH_MAX];
  char versions[PATH_MAX];
  char staging[PATH_MAX];
  char destination[PATH_MAX];
  int ret;

  ret = ny_manifest_load(source, &config);
  if (ret < 0 || !ny_package_valid_version(config.version))
    {
      return ret < 0 ? ret : -EINVAL;
    }

  nxmutex_lock(&g_package_lock);
  ret = ny_package_mkdir_parents(CONFIG_NYABULA_CORE_PACKAGE_STORE);
  if (ret < 0 ||
      (ret = ny_package_join(plugin_root, sizeof(plugin_root),
                             CONFIG_NYABULA_CORE_PACKAGE_STORE, config.id)) <
          0 ||
      (ret = ny_package_mkdir(plugin_root)) < 0 ||
      (ret = ny_package_join(versions, sizeof(versions), plugin_root,
                             "versions")) < 0 ||
      (ret = ny_package_mkdir(versions)) < 0 ||
      (ret = ny_package_join(destination, sizeof(destination), versions,
                             config.version)) < 0 ||
      snprintf(staging, sizeof(staging), "%s/.staging.%ld", plugin_root,
               (long)getpid()) >= (int)sizeof(staging))
    {
      goto out;
    }

  if (access(destination, F_OK) == 0)
    {
      ret = -EEXIST;
      goto out;
    }

  ny_package_clean_staging(plugin_root);
  ret = ny_package_remove_tree(staging);
  if (ret < 0 || (ret = ny_package_copy_tree(source, staging, true)) < 0)
    {
      ny_package_remove_tree(staging);
      goto out;
    }

  ret = ny_manifest_load(staging, &config);
  if (ret < 0)
    {
      ny_package_remove_tree(staging);
      goto out;
    }

  if (rename(staging, destination) < 0)
    {
      ret = -errno;
      ny_package_remove_tree(staging);
      goto out;
    }

  ret = ny_package_sync_directory(versions);

out:
  nxmutex_unlock(&g_package_lock);
  return ret;
}

int ny_package_activate(const char *id, const char *version)
{
  int ret;

  nxmutex_lock(&g_package_lock);
  ret = ny_package_activate_locked(id, version, true);
  nxmutex_unlock(&g_package_lock);
  return ret;
}

int ny_package_rollback(const char *id)
{
  char plugin_root[PATH_MAX];
  char unused[PATH_MAX];
  char version[NY_PLUGIN_VERSION_SIZE + 1];
  int ret;

  nxmutex_lock(&g_package_lock);
  ret = ny_package_version_path(id, "0", plugin_root, sizeof(plugin_root),
                                unused, sizeof(unused));
  if (ret >= 0)
    {
      ret = ny_package_read_marker(plugin_root, "last-good", version,
                                   sizeof(version));
    }

  if (ret >= 0)
    {
      ret = ny_package_activate_locked(id, version, false);
    }

  nxmutex_unlock(&g_package_lock);
  return ret;
}

int ny_package_resolve(const char *id, char *path, size_t size)
{
  char plugin_root[PATH_MAX];
  char version[NY_PLUGIN_VERSION_SIZE + 1];
  int ret;

  if (path == NULL || size == 0 || !ny_package_valid_id(id))
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_package_lock);
  ret = ny_package_join(plugin_root, sizeof(plugin_root),
                        CONFIG_NYABULA_CORE_PACKAGE_STORE, id);
  if (ret >= 0)
    {
      ret = ny_package_read_marker(plugin_root, "current", version,
                                   sizeof(version));
    }

  if (ret >= 0)
    {
      ret = ny_package_version_path(id, version, plugin_root,
                                    sizeof(plugin_root), path, size);
    }

  nxmutex_unlock(&g_package_lock);
  return ret;
}

int ny_package_resolve_version(const char *id, const char *version, char *path,
                               size_t size)
{
  struct ny_plugin_config_s config;
  char plugin_root[PATH_MAX];
  int ret;

  if (path == NULL || size == 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_package_lock);
  ret = ny_package_version_path(id, version, plugin_root, sizeof(plugin_root),
                                path, size);
  if (ret >= 0)
    {
      ret = ny_manifest_load(path, &config);
    }

  if (ret >= 0 &&
      (strcmp(config.id, id) != 0 || strcmp(config.version, version) != 0))
    {
      ret = -EINVAL;
    }

  nxmutex_unlock(&g_package_lock);
  return ret;
}

int ny_package_storage_root(const char *id, char *path, size_t size)
{
  int ret;

  if (path == NULL || size == 0 || !ny_package_valid_id(id))
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_package_lock);
  ret = ny_package_join(path, size, CONFIG_NYABULA_CORE_PACKAGE_STORE, id);
  nxmutex_unlock(&g_package_lock);
  return ret;
}

int ny_package_list(void)
{
  struct dirent *plugin_entry;
  DIR *store;
  int ret = 0;

  nxmutex_lock(&g_package_lock);
  store = opendir(CONFIG_NYABULA_CORE_PACKAGE_STORE);
  if (store == NULL)
    {
      ret = errno == ENOENT ? 0 : -errno;
      goto out;
    }

  while ((plugin_entry = readdir(store)) != NULL)
    {
      struct dirent *version_entry;
      char plugin_root[PATH_MAX];
      char versions[PATH_MAX];
      char current[NY_PLUGIN_VERSION_SIZE + 1] = "";
      char last_good[NY_PLUGIN_VERSION_SIZE + 1] = "";
      struct stat status;
      DIR *directory;

      if (strcmp(plugin_entry->d_name, ".") == 0 ||
          strcmp(plugin_entry->d_name, "..") == 0)
        {
          continue;
        }

      if (!ny_package_valid_id(plugin_entry->d_name) ||
          ny_package_join(plugin_root, sizeof(plugin_root),
                          CONFIG_NYABULA_CORE_PACKAGE_STORE,
                          plugin_entry->d_name) < 0 ||
          lstat(plugin_root, &status) < 0 || !S_ISDIR(status.st_mode) ||
          ny_package_join(versions, sizeof(versions), plugin_root,
                          "versions") < 0)
        {
          ret = -EINVAL;
          break;
        }

      ret = ny_package_read_marker(plugin_root, "current", current,
                                   sizeof(current));
      if (ret == -ENOENT)
        {
          ret = 0;
        }

      if (ret < 0)
        {
          break;
        }

      ret = ny_package_read_marker(plugin_root, "last-good", last_good,
                                   sizeof(last_good));
      if (ret == -ENOENT)
        {
          ret = 0;
        }

      if (ret < 0)
        {
          break;
        }

      directory = opendir(versions);
      if (directory == NULL)
        {
          ret = -errno;
          break;
        }

      while ((version_entry = readdir(directory)) != NULL)
        {
          char version_path[PATH_MAX];

          if (strcmp(version_entry->d_name, ".") == 0 ||
              strcmp(version_entry->d_name, "..") == 0)
            {
              continue;
            }

          if (!ny_package_valid_version(version_entry->d_name) ||
              ny_package_join(version_path, sizeof(version_path), versions,
                              version_entry->d_name) < 0 ||
              lstat(version_path, &status) < 0 || !S_ISDIR(status.st_mode))
            {
              ret = -EINVAL;
              break;
            }

          printf("%s %s current=%s last-good=%s\n", plugin_entry->d_name,
                 version_entry->d_name,
                 strcmp(version_entry->d_name, current) == 0 ? "yes" : "no",
                 strcmp(version_entry->d_name, last_good) == 0 ? "yes" : "no");
        }

      closedir(directory);
      if (ret < 0)
        {
          break;
        }
    }

  closedir(store);

out:
  nxmutex_unlock(&g_package_lock);
  return ret;
}
