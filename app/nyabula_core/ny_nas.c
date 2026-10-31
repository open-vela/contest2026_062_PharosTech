/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_nas.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#include "ny_nas.h"
#include "ny_product_store.h"
#include "ny_utf8.h"
#include "ny_web_auth.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#ifdef CONFIG_USBHOST_MSC_NOTIFIER
#include <nuttx/usb/usbhost.h>
#include <nuttx/wqueue.h>
#endif

#ifndef CONFIG_NYABULA_CORE_NAS_MOUNT
#define CONFIG_NYABULA_CORE_NAS_MOUNT "/mnt/nyabula-usb"
#endif

#define NY_NAS_MOUNT      CONFIG_NYABULA_CORE_NAS_MOUNT
#define NY_NAS_LIST_LIMIT 48
#define NY_NAS_TRANSFERS  2
#define NY_NAS_TICKETS    8
#define NY_NAS_TICKET_MS  60000ULL

struct ny_nas_ticket_s
{
  char path[NY_NAS_PATH_MAX + 1];
  char token[65];
  uint64_t expires;
  uint32_t auth_generation;
  unsigned int mount_generation;
};

struct ny_nas_file_s
{
  int fd;
  bool writing;
  uint64_t expected;
  uint64_t written;
  char destination[PATH_MAX];
  char temporary[PATH_MAX];
};

static mutex_t g_nas_lock = NXMUTEX_INITIALIZER;
static bool g_nas_loaded;
static bool g_nas_enabled;
static bool g_nas_readonly = true;
static bool g_nas_mounted;
static unsigned int g_nas_transfers;
static unsigned int g_nas_uploads;
static unsigned int g_nas_nonce;
static uint64_t g_nas_revision;
static uint64_t g_nas_next;
static int g_nas_error;
static char g_nas_selected[16] = "auto";
static char g_nas_device[16];
static atomic_bool g_nas_lost;
static atomic_uint g_nas_generation;
static struct ny_nas_ticket_s g_nas_tickets[NY_NAS_TICKETS];
#ifdef CONFIG_USBHOST_MSC_NOTIFIER
static int g_nas_notifier;
#endif

static const char *ny_nas_string(const cJSON *data, const char *key);
static bool ny_nas_device_name(const char *path);
static int ny_nas_load(void);
static cJSON *ny_nas_devices(void);
static cJSON *ny_nas_status(void);
static int ny_nas_reconcile(void);
static int ny_nas_path(const char *relative, bool missing, char *path);
static int ny_nas_list(const cJSON *data, cJSON **result);
static bool ny_nas_ready(void);
static int ny_nas_ticket(const cJSON *data, cJSON **result);
#ifdef CONFIG_USBHOST_MSC_NOTIFIER
static void ny_nas_disconnect(void *generation);
#endif

/****************************************************************************
 * Name: ny_nas_string
 ****************************************************************************/

static const char *ny_nas_string(const cJSON *data, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

/****************************************************************************
 * Name: ny_nas_device_name
 * Description: Only USB MSC block names, never mmcsd/eMMC or arbitrary paths.
 ****************************************************************************/

static bool ny_nas_device_name(const char *path)
{
  if (strncmp(path, "/dev/sd", 7) || strlen(path) < 8 || strlen(path) > 10 ||
      path[7] < 'a' || path[7] > 'z')
    return false;
  for (const char *p = path + 8; *p; p++)
    if (*p < '0' || *p > '9')
      return false;
  return true;
}

/****************************************************************************
 * Name: ny_nas_load
 ****************************************************************************/

static int ny_nas_load(void)
{
  if (g_nas_loaded)
    return 0;
  cJSON *config = NULL;
  int ret = ny_product_store_read("nas", &config, &g_nas_revision);
  if (ret < 0)
    return ret;
  if (config)
    {
      const char *device = ny_nas_string(config, "device");
      if ((!ny_nas_device_name(device) && strcmp(device, "auto")) ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(config, "enabled")) ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(config, "readOnly")))
        ret = -EBADMSG;
      else
        {
          snprintf(g_nas_selected, sizeof(g_nas_selected), "%s", device);
          g_nas_enabled = cJSON_IsTrue(
              cJSON_GetObjectItemCaseSensitive(config, "enabled"));
          g_nas_readonly = cJSON_IsTrue(
              cJSON_GetObjectItemCaseSensitive(config, "readOnly"));
        }
    }
  cJSON_Delete(config);
  g_nas_loaded = ret == 0;
  return ret;
}

/****************************************************************************
 * Name: ny_nas_devices
 ****************************************************************************/

static cJSON *ny_nas_devices(void)
{
  cJSON *devices = cJSON_CreateArray();
  if (!devices)
    return NULL;
  DIR *directory = opendir("/dev");
  if (!directory)
    {
      cJSON_Delete(devices);
      return NULL;
    }
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL)
    {
      char path[32];
      if (snprintf(path, sizeof(path), "/dev/%s", entry->d_name) >=
              (int)sizeof(path) ||
          !ny_nas_device_name(path))
        continue;
      struct stat st;
      if (lstat(path, &st) < 0 || !S_ISBLK(st.st_mode))
        continue;
      cJSON *device = cJSON_CreateObject();
      if (!device || !cJSON_AddStringToObject(device, "path", path) ||
          !cJSON_AddStringToObject(device, "name", entry->d_name) ||
          !cJSON_AddItemToArray(devices, device))
        {
          cJSON_Delete(device);
          cJSON_Delete(devices);
          devices = NULL;
          break;
        }
    }
  closedir(directory);
  return devices;
}

/****************************************************************************
 * Name: ny_nas_ready
 ****************************************************************************/

static bool ny_nas_ready(void)
{
  struct stat st;
  return g_nas_enabled && g_nas_mounted && !atomic_load(&g_nas_lost) &&
         lstat(g_nas_device, &st) == 0 && S_ISBLK(st.st_mode);
}

/****************************************************************************
 * Name: ny_nas_status
 ****************************************************************************/

static cJSON *ny_nas_status(void)
{
  cJSON *result = cJSON_CreateObject();
  cJSON *devices = ny_nas_devices();
  bool ready = ny_nas_ready();
  if (!result || !devices ||
      !cJSON_AddItemToObject(result, "devices", devices))
    {
      cJSON_Delete(result);
      cJSON_Delete(devices);
      return NULL;
    }
  bool ok =
      cJSON_AddBoolToObject(result, "enabled", g_nas_enabled) &&
      cJSON_AddBoolToObject(result, "mounted", g_nas_mounted) &&
      cJSON_AddBoolToObject(result, "ready", ready) &&
      cJSON_AddBoolToObject(result, "readOnly", g_nas_readonly) &&
      cJSON_AddStringToObject(result, "device", g_nas_selected) &&
      cJSON_AddStringToObject(result, "mountedDevice", g_nas_device) &&
      cJSON_AddStringToObject(result, "protocol", "authenticated-http") &&
      cJSON_AddNumberToObject(result, "revision", g_nas_revision) &&
      cJSON_AddNumberToObject(result, "activeTransfers", g_nas_transfers) &&
      cJSON_AddNumberToObject(result, "lastError", g_nas_error);
  if (ready)
    {
      struct statfs fs;
      if (statfs(NY_NAS_MOUNT, &fs) == 0)
        ok = ok &&
             cJSON_AddNumberToObject(result, "totalBytes",
                                     (double)fs.f_blocks * fs.f_bsize) &&
             cJSON_AddNumberToObject(result, "freeBytes",
                                     (double)fs.f_bavail * fs.f_bsize);
    }
  if (!ok)
    {
      cJSON_Delete(result);
      return NULL;
    }
  return result;
}

#ifdef CONFIG_USBHOST_MSC_NOTIFIER
/****************************************************************************
 * Name: ny_nas_disconnect
 ****************************************************************************/

static void ny_nas_disconnect(void *generation)
{
  if ((unsigned int)(uintptr_t)generation == atomic_load(&g_nas_generation))
    atomic_store(&g_nas_lost, true);
}
#endif

/****************************************************************************
 * Name: ny_nas_reconcile
 ****************************************************************************/

static int ny_nas_reconcile(void)
{
  struct stat st;
  bool absent =
      g_nas_mounted && (atomic_load(&g_nas_lost) ||
                        lstat(g_nas_device, &st) < 0 || !S_ISBLK(st.st_mode));
  if (g_nas_mounted && (!g_nas_enabled || absent))
    {
      if (g_nas_transfers)
        return -EBUSY;
#ifdef CONFIG_USBHOST_MSC_NOTIFIER
      if (g_nas_notifier > 0)
        usbhost_msc_notifier_teardown(g_nas_notifier);
      g_nas_notifier = 0;
#endif
      atomic_fetch_add(&g_nas_generation, 1);
      if (umount(NY_NAS_MOUNT) < 0)
        return -errno;
      g_nas_mounted = false;
      g_nas_device[0] = '\0';
    }
  if (!g_nas_enabled || g_nas_mounted)
    return 0;
  cJSON *devices = ny_nas_devices();
  if (!devices)
    return -EIO;
  int count = cJSON_GetArraySize(devices);
  const char *selected = g_nas_selected;
  int ret = 0;
  if (!strcmp(selected, "auto"))
    {
      if (count != 1)
        ret = count == 0 ? -ENODEV : -EEXIST;
      else
        selected = ny_nas_string(cJSON_GetArrayItem(devices, 0), "path");
    }
  bool found = false;
  const cJSON *device;
  cJSON_ArrayForEach(device, devices) found |=
      !strcmp(selected, ny_nas_string(device, "path"));
  if (ret == 0 && !found)
    ret = -ENODEV;
  if (ret == 0)
    {
      if (mkdir("/mnt", 0755) < 0 && errno != EEXIST)
        ret = -errno;
      if (ret == 0 && mkdir(NY_NAS_MOUNT, 0700) < 0 && errno != EEXIST)
        ret = -errno;
      if (ret == 0)
        {
          DIR *empty = NULL;
          if (lstat(NY_NAS_MOUNT, &st) < 0 || !S_ISDIR(st.st_mode))
            ret = -ENOTDIR;
          else
            empty = opendir(NY_NAS_MOUNT);
          if (ret == 0 && !empty)
            ret = -errno;
          if (empty)
            {
              struct dirent *entry;
              while ((entry = readdir(empty)) != NULL)
                if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
                  {
                    ret = -ENOTEMPTY;
                    break;
                  }
              closedir(empty);
            }
        }
      if (ret == 0 && mount(selected, NY_NAS_MOUNT, "fatfs",
                            g_nas_readonly ? MS_RDONLY : 0, NULL) < 0)
        ret = -errno;
      if (ret == 0)
        {
          snprintf(g_nas_device, sizeof(g_nas_device), "%s", selected);
          g_nas_mounted = true;
          atomic_store(&g_nas_lost, false);
          unsigned int generation = atomic_fetch_add(&g_nas_generation, 1) + 1;
#ifdef CONFIG_USBHOST_MSC_NOTIFIER
          g_nas_notifier = usbhost_msc_notifier_setup(
              ny_nas_disconnect, WORK_USB_MSC_DISCONNECT, selected[7],
              (void *)(uintptr_t)generation);
          if (g_nas_notifier < 0)
            {
              ret = g_nas_notifier;
              atomic_store(&g_nas_lost, true);
            }
#endif
          (void)generation;
        }
    }
  cJSON_Delete(devices);
  return ret;
}

/****************************************************************************
 * Name: ny_nas_path
 ****************************************************************************/

static int ny_nas_path(const char *relative, bool missing, char *path)
{
  size_t length = strlen(relative);
  if (length > NY_NAS_PATH_MAX || relative[0] == '/' ||
      !ny_utf8_valid((const unsigned char *)relative, length))
    return -EINVAL;
  for (size_t i = 0; i < length; i++)
    if ((unsigned char)relative[i] < 32 || relative[i] == '\\' ||
        relative[i] == 127)
      return -EINVAL;
  size_t root = strlen(NY_NAS_MOUNT);
  if (root + length + 2 > PATH_MAX)
    return -ENAMETOOLONG;
  memcpy(path, NY_NAS_MOUNT, root + 1);
  if (!length)
    return 0;
  path[root] = '/';
  memcpy(path + root + 1, relative, length + 1);
  char *part = path + root + 1;
  for (char *p = part;; p++)
    if (*p == '/' || !*p)
      {
        bool last = !*p;
        char saved = *p;
        *p = '\0';
        if (!part[0] || !strcmp(part, ".") || !strcmp(part, "..") ||
            !strncmp(part, ".nyabula-upload-", 16))
          return -EINVAL;
        struct stat st;
        int ret = lstat(path, &st);
        if (ret < 0)
          {
            if (!(last && missing && errno == ENOENT))
              return -errno;
          }
        else if (S_ISLNK(st.st_mode) || (!last && !S_ISDIR(st.st_mode)))
          return -EACCES;
        *p = saved;
        if (last)
          break;
        part = p + 1;
      }
  return 0;
}

/****************************************************************************
 * Name: ny_nas_list
 ****************************************************************************/

static int ny_nas_list(const cJSON *data, cJSON **result)
{
  if (!ny_nas_ready())
    return -ENODEV;
  const cJSON *offset = cJSON_GetObjectItemCaseSensitive(data, "offset");
  if (offset && (!cJSON_IsNumber(offset) || !isfinite(offset->valuedouble) ||
                 offset->valuedouble < 0 || offset->valuedouble > 100000 ||
                 floor(offset->valuedouble) != offset->valuedouble))
    return -EINVAL;
  unsigned int skip = offset ? (unsigned int)offset->valuedouble : 0;
  const char *relative = ny_nas_string(data, "path");
  char path[PATH_MAX];
  int ret = ny_nas_path(relative, false, path);
  if (ret < 0)
    return ret;
  DIR *directory = opendir(path);
  if (!directory)
    return -errno;
  *result = cJSON_CreateObject();
  cJSON *entries = cJSON_AddArrayToObject(*result, "entries");
  if (!*result || !entries)
    ret = -ENOMEM;
  struct dirent *entry;
  unsigned int seen = 0, added = 0;
  bool more = false;
  while (ret == 0 && (entry = readdir(directory)) != NULL)
    {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
          !strncmp(entry->d_name, ".nyabula-upload-", 16))
        continue;
      char child[NY_NAS_PATH_MAX + 1], absolute[PATH_MAX];
      int size = snprintf(child, sizeof(child), "%s%s%s", relative,
                          relative[0] ? "/" : "", entry->d_name);
      if (size < 0 || size >= (int)sizeof(child) ||
          ny_nas_path(child, false, absolute) < 0)
        continue;
      struct stat st;
      if (lstat(absolute, &st) < 0 ||
          (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
        continue;
      if (seen++ < skip)
        continue;
      if (added == NY_NAS_LIST_LIMIT)
        {
          more = true;
          break;
        }
      cJSON *item = cJSON_CreateObject();
      if (!item || !cJSON_AddStringToObject(item, "name", entry->d_name) ||
          !cJSON_AddStringToObject(item, "path", child) ||
          !cJSON_AddBoolToObject(item, "directory", S_ISDIR(st.st_mode)) ||
          !cJSON_AddNumberToObject(item, "size", st.st_size) ||
          !cJSON_AddNumberToObject(item, "modifiedAt",
                                   (double)st.st_mtime * 1000) ||
          !cJSON_AddItemToArray(entries, item))
        {
          cJSON_Delete(item);
          ret = -ENOMEM;
        }
      added++;
    }
  closedir(directory);
  if (ret == 0 &&
      (!cJSON_AddStringToObject(*result, "path", relative) ||
       !cJSON_AddBoolToObject(*result, "more", more) ||
       !cJSON_AddNumberToObject(*result, "nextOffset", skip + added)))
    ret = -ENOMEM;
  return ret;
}

/****************************************************************************
 * Name: ny_nas_ticket
 ****************************************************************************/

static int ny_nas_ticket(const cJSON *data, cJSON **result)
{
  if (!ny_nas_ready())
    return -ENODEV;
  const char *relative = ny_nas_string(data, "path");
  char path[PATH_MAX];
  int ret = relative[0] ? ny_nas_path(relative, false, path) : -EINVAL;
  struct stat st;
  if (ret == 0 && (lstat(path, &st) < 0 || !S_ISREG(st.st_mode)))
    ret = -EINVAL;
  if (ret < 0)
    return ret;
  unsigned char random[32];
  size_t offset = 0;
  while (offset < sizeof(random))
    {
      ssize_t count = getrandom(random + offset, sizeof(random) - offset, 0);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        return -EIO;
      offset += count;
    }
  size_t selected = 0;
  for (size_t i = 1; i < NY_NAS_TICKETS; i++)
    if (g_nas_tickets[i].expires < g_nas_tickets[selected].expires)
      selected = i;
  struct ny_nas_ticket_s *ticket = &g_nas_tickets[selected];
  snprintf(ticket->path, sizeof(ticket->path), "%s", relative);
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(random); i++)
    {
      ticket->token[i * 2] = hex[random[i] >> 4];
      ticket->token[i * 2 + 1] = hex[random[i] & 15];
    }
  ticket->token[64] = '\0';
  memset(random, 0, sizeof(random));
  ticket->expires = ny_product_time_ms(true) + NY_NAS_TICKET_MS;
  ticket->auth_generation = ny_web_auth_generation();
  ticket->mount_generation = atomic_load(&g_nas_generation);
  *result = cJSON_CreateObject();
  return *result && cJSON_AddStringToObject(*result, "path", relative) &&
                 cJSON_AddStringToObject(*result, "ticket", ticket->token) &&
                 cJSON_AddNumberToObject(*result, "expiresInMs",
                                         NY_NAS_TICKET_MS)
             ? 0
             : -ENOMEM;
}

/****************************************************************************
 * Name: ny_nas_ticket_authorized
 ****************************************************************************/

bool ny_nas_ticket_authorized(const char *path, const char *offered)
{
  if (!path || !offered || strlen(offered) != 64)
    return false;
  nxmutex_lock(&g_nas_lock);
  bool accepted = false;
  uint64_t now = ny_product_time_ms(true);
  uint32_t auth = ny_web_auth_generation();
  unsigned int mount_generation = atomic_load(&g_nas_generation);
  for (size_t i = 0; i < NY_NAS_TICKETS; i++)
    {
      const struct ny_nas_ticket_s *ticket = &g_nas_tickets[i];
      unsigned int difference = 0;
      for (size_t j = 0; j < 64; j++)
        difference |=
            (unsigned char)offered[j] ^ (unsigned char)ticket->token[j];
      accepted |= difference == 0 && now < ticket->expires &&
                  auth == ticket->auth_generation &&
                  mount_generation == ticket->mount_generation &&
                  !strcmp(path, ticket->path);
    }
  accepted &= ny_nas_ready();
  nxmutex_unlock(&g_nas_lock);
  return accepted;
}

/****************************************************************************
 * Name: ny_nas_request
 ****************************************************************************/

int ny_nas_request(const struct ny_product_caller_s *caller, const char *topic,
                   const cJSON *data, cJSON **result)
{
  if (strncmp(topic, "nas.", 4))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  nxmutex_lock(&g_nas_lock);
  int ret = ny_nas_load();
  if (ret == 0 && !strcmp(topic, "nas.configure"))
    {
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
      const cJSON *readonly =
          cJSON_GetObjectItemCaseSensitive(data, "readOnly");
      const char *device = ny_nas_string(data, "device");
      double expected = cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(data, "revision"));
      if (!caller->local_transport || !cJSON_IsBool(enabled) ||
          !cJSON_IsBool(readonly) ||
          (!ny_nas_device_name(device) && strcmp(device, "auto")))
        ret = -EINVAL;
      else if (!isfinite(expected) || expected != (double)g_nas_revision)
        ret = -ESTALE;
      else if (g_nas_transfers)
        ret = -EBUSY;
      else
        {
          bool previous_enabled = g_nas_enabled;
          g_nas_enabled = false;
          ret = ny_nas_reconcile();
          g_nas_enabled = previous_enabled;
          if (ret == 0)
            {
              cJSON *config = cJSON_CreateObject();
              bool valid = config &&
                           cJSON_AddStringToObject(config, "device", device) &&
                           cJSON_AddBoolToObject(config, "enabled",
                                                 cJSON_IsTrue(enabled)) &&
                           cJSON_AddBoolToObject(config, "readOnly",
                                                 cJSON_IsTrue(readonly));
              uint64_t revision;
              ret = valid ? ny_product_store_write("nas", config,
                                                   g_nas_revision, &revision)
                          : -ENOMEM;
              if (ret == 0)
                g_nas_revision = revision;
              cJSON_Delete(config);
            }
          if (ret == 0)
            {
              g_nas_enabled = cJSON_IsTrue(enabled);
              g_nas_readonly = cJSON_IsTrue(readonly);
              snprintf(g_nas_selected, sizeof(g_nas_selected), "%s", device);
              g_nas_error = ny_nas_reconcile();
              g_nas_next = ny_product_time_ms(true) + 2000;
            }
        }
    }
  if (ret == 0 && !strcmp(topic, "nas.list"))
    ret = ny_nas_list(data, result);
  else if (ret == 0 && !strcmp(topic, "nas.download"))
    ret = ny_nas_ticket(data, result);
  else if (ret == 0 && !strcmp(topic, "nas.mkdir"))
    {
      char path[PATH_MAX];
      const char *relative = ny_nas_string(data, "path");
      ret = !ny_nas_ready()  ? -ENODEV
            : g_nas_readonly ? -EROFS
            : !relative[0]   ? -EINVAL
                             : ny_nas_path(relative, true, path);
      if (ret == 0 && mkdir(path, 0700) < 0)
        ret = -errno;
      if (ret == 0)
        *result = cJSON_CreateObject();
    }
  else if (ret == 0 &&
           (!strcmp(topic, "nas.status") || !strcmp(topic, "nas.devices") ||
            !strcmp(topic, "nas.configure")))
    *result = ny_nas_status();
  else if (ret == 0)
    ret = -ENOSYS;
  if (ret == 0 && !*result)
    ret = -ENOMEM;
  nxmutex_unlock(&g_nas_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_nas_tick
 ****************************************************************************/

int ny_nas_tick(void)
{
  nxmutex_lock(&g_nas_lock);
  int ret = ny_nas_load();
  uint64_t now = ny_product_time_ms(true);
  if (ret == 0 && now >= g_nas_next)
    {
      g_nas_error = ny_nas_reconcile();
      g_nas_next =
          now + (g_nas_error < 0 && g_nas_error != -ENODEV ? 30000 : 2000);
    }
  nxmutex_unlock(&g_nas_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_nas_file_open
 ****************************************************************************/

int ny_nas_file_open(const char *relative, bool writing, uint64_t expected,
                     struct ny_nas_file_s **file, uint64_t *size)
{
  if (!relative || !relative[0] || !file || !size ||
      expected >
          (sizeof(off_t) >= 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX))
    return -EINVAL;
  *file = NULL;
  nxmutex_lock(&g_nas_lock);
  int ret = !ny_nas_ready()             ? -ENODEV
            : writing && g_nas_readonly ? -EROFS
            : g_nas_transfers >= NY_NAS_TRANSFERS || (writing && g_nas_uploads)
                ? -EBUSY
                : 0;
  struct ny_nas_file_s *job = ret == 0 ? calloc(1, sizeof(*job)) : NULL;
  if (ret == 0 && !job)
    ret = -ENOMEM;
  if (ret == 0)
    {
      job->fd = -1;
      job->writing = writing;
      job->expected = expected;
      ret = ny_nas_path(relative, writing, job->destination);
    }
  struct stat st;
  if (ret == 0 && writing)
    {
      if (lstat(job->destination, &st) == 0)
        ret = -EEXIST;
      else if (errno != ENOENT)
        ret = -errno;
      struct statfs fs;
      if (ret == 0 && statfs(NY_NAS_MOUNT, &fs) < 0)
        ret = -errno;
      if (ret == 0 && expected > (uint64_t)fs.f_bavail * fs.f_bsize)
        ret = -ENOSPC;
      if (ret == 0)
        {
          for (int attempt = 0; attempt < 16; attempt++)
            {
              snprintf(job->temporary, sizeof(job->temporary),
                       NY_NAS_MOUNT "/.nyabula-upload-%u", ++g_nas_nonce);
              job->fd =
                  open(job->temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
              if (job->fd >= 0 || errno != EEXIST)
                break;
            }
        }
      *size = expected;
    }
  else if (ret == 0)
    {
      if (lstat(job->destination, &st) < 0)
        ret = -errno;
      else if (!S_ISREG(st.st_mode) || st.st_size < 0)
        ret = -EINVAL;
      else
        {
          job->fd = open(job->destination, O_RDONLY);
          *size = st.st_size;
        }
    }
  if (ret == 0 && job->fd < 0)
    ret = -errno;
  if (ret == 0)
    {
      g_nas_transfers++;
      if (writing)
        g_nas_uploads++;
      *file = job;
    }
  else
    free(job);
  nxmutex_unlock(&g_nas_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_nas_file_read / ny_nas_file_write / ny_nas_file_seek
 ****************************************************************************/

ssize_t ny_nas_file_read(struct ny_nas_file_s *file, void *data, size_t size)
{
  if (!file || file->writing)
    return -EINVAL;
  if (atomic_load(&g_nas_lost))
    return -ENODEV;
  ssize_t ret = read(file->fd, data, size);
  if (ret < 0 && errno == ENODEV)
    atomic_store(&g_nas_lost, true);
  return ret < 0 ? -errno : ret;
}

ssize_t ny_nas_file_write(struct ny_nas_file_s *file, const void *data,
                          size_t size)
{
  if (!file || !file->writing || size > file->expected - file->written)
    return -EINVAL;
  if (atomic_load(&g_nas_lost))
    return -ENODEV;
  ssize_t ret = write(file->fd, data, size);
  if (ret < 0 && errno == ENODEV)
    atomic_store(&g_nas_lost, true);
  if (ret > 0)
    file->written += ret;
  return ret < 0 ? -errno : ret;
}

int ny_nas_file_seek(struct ny_nas_file_s *file, uint64_t offset)
{
  if (!file || file->writing ||
      offset >
          (sizeof(off_t) >= 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX))
    return -EINVAL;
  return lseek(file->fd, (off_t)offset, SEEK_SET) < 0 ? -errno : 0;
}

/****************************************************************************
 * Name: ny_nas_file_close
 ****************************************************************************/

int ny_nas_file_close(struct ny_nas_file_s *file, bool commit)
{
  if (!file)
    return -EINVAL;
  int ret = 0;
  if (file->writing && commit)
    {
      if (file->written != file->expected)
        ret = -ENODATA;
      else if (fsync(file->fd) < 0)
        ret = -errno;
    }
  if (close(file->fd) < 0 && ret == 0)
    ret = -errno;
  nxmutex_lock(&g_nas_lock);
  if (file->writing)
    {
      if (ret == 0 && commit)
        {
          struct stat st;
          if (!ny_nas_ready())
            ret = -ENODEV;
          else if (lstat(file->destination, &st) == 0)
            ret = -EEXIST;
          else if (errno != ENOENT)
            ret = -errno;
          else if (rename(file->temporary, file->destination) < 0)
            ret = -errno;
        }
      if (!commit || ret < 0)
        unlink(file->temporary);
      g_nas_uploads--;
    }
  g_nas_transfers--;
  nxmutex_unlock(&g_nas_lock);
  free(file);
  return ret;
}
