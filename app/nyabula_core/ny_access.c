/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_access.c
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

#include "ny_access.h"
#include "ny_product_store.h"
#include <errno.h>
#include <fcntl.h>
#include <nuttx/mutex.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef CONFIG_CRYPTO_MBEDTLS
#include <mbedtls/constant_time.h>
#include <mbedtls/md.h>
#endif

#define NY_ACCESS_MAX_MEMBERS 16
#define NY_ACCESS_TOKEN_BYTES 32
#define NY_ACCESS_HEX_SIZE    65
static mutex_t g_access_lock = NXMUTEX_INITIALIZER;
static atomic_uint g_access_generation;
static const char *ny_access_string(const cJSON *object, const char *name);
static int ny_access_digest(const char *token, char *output);
static int ny_access_random(char *token);
static int ny_access_read(cJSON **members, uint64_t *revision);
static cJSON *ny_access_public(const cJSON *members);

/****************************************************************************
 * Name: ny_access_string
 ****************************************************************************/
static const char *ny_access_string(const cJSON *object, const char *name)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsString(item) ? item->valuestring : NULL;
}

/****************************************************************************
 * Name: ny_access_digest
 ****************************************************************************/
static int ny_access_digest(const char *token, char *output)
{
#ifdef CONFIG_CRYPTO_MBEDTLS
  unsigned char digest[NY_ACCESS_TOKEN_BYTES];
  if (token == NULL || strlen(token) != NY_ACCESS_HEX_SIZE - 1)
    return -EACCES;
  if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                 (const unsigned char *)token, strlen(token), digest) != 0)
    return -EIO;
  for (size_t i = 0; i < sizeof(digest); i++)
    snprintf(output + i * 2, 3, "%02x", digest[i]);
  memset(digest, 0, sizeof(digest));
  return 0;
#else
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: ny_access_random
 ****************************************************************************/
static int ny_access_random(char *token)
{
  unsigned char bytes[NY_ACCESS_TOKEN_BYTES];
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return -errno;
  size_t used = 0;
  while (used < sizeof(bytes))
    {
      ssize_t count = read(fd, bytes + used, sizeof(bytes) - used);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        {
          close(fd);
          return -EIO;
        }
      used += count;
    }
  close(fd);
  for (size_t i = 0; i < sizeof(bytes); i++)
    snprintf(token + i * 2, 3, "%02x", bytes[i]);
  memset(bytes, 0, sizeof(bytes));
  return 0;
}

/****************************************************************************
 * Name: ny_access_read
 ****************************************************************************/
static int ny_access_read(cJSON **members, uint64_t *revision)
{
  int ret = ny_product_store_read("access", members, revision);
  if (ret < 0)
    return ret;
  if (*members == NULL)
    *members = cJSON_CreateArray();
  return *members == NULL ? -ENOMEM : cJSON_IsArray(*members) ? 0 : -EIO;
}

/****************************************************************************
 * Name: ny_access_public
 ****************************************************************************/
static cJSON *ny_access_public(const cJSON *members)
{
  cJSON *result = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(result, "members");
  const cJSON *entry;
  if (list == NULL)
    {
      cJSON_Delete(result);
      return NULL;
    }
  cJSON_ArrayForEach(entry, members)
  {
    cJSON *copy = cJSON_Duplicate(entry, true);
    if (copy == NULL)
      {
        cJSON_Delete(result);
        return NULL;
      }
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "digest");
    cJSON_AddItemToArray(list, copy);
  }
  return result;
}

/****************************************************************************
 * Name: ny_access_generation
 ****************************************************************************/
uint32_t ny_access_generation(void)
{
  return atomic_load(&g_access_generation);
}

/****************************************************************************
 * Name: ny_access_role_name
 ****************************************************************************/
const char *ny_access_role_name(enum ny_product_role_e role)
{
  return role == NY_PRODUCT_OWNER    ? "owner"
         : role == NY_PRODUCT_FAMILY ? "family"
                                     : "guest";
}

/****************************************************************************
 * Name: ny_access_auth
 ****************************************************************************/
int ny_access_auth(const char *token, char *id, size_t size,
                   enum ny_product_role_e *role, uint32_t *generation)
{
  char digest[NY_ACCESS_HEX_SIZE];
  cJSON *members = NULL;
  cJSON *entry;
  uint64_t revision;
  int ret = ny_access_digest(token, digest);
  if (ret < 0)
    return ret;
  nxmutex_lock(&g_access_lock);
  ret = ny_access_read(&members, &revision);
  if (ret == 0)
    {
      ret = -EACCES;
      cJSON_ArrayForEach(entry, members)
      {
        const char *saved = ny_access_string(entry, "digest");
        const char *name = ny_access_string(entry, "id");
        const char *level = ny_access_string(entry, "role");
        if (saved == NULL || name == NULL || level == NULL ||
            strlen(saved) != sizeof(digest) - 1 || strlen(name) >= size)
          continue;
#ifdef CONFIG_CRYPTO_MBEDTLS
        if (mbedtls_ct_memcmp(saved, digest, sizeof(digest) - 1) == 0)
          {
            if (strcmp(level, "family") != 0 && strcmp(level, "guest") != 0)
              continue;
            strlcpy(id, name, size);
            *role = strcmp(level, "family") == 0 ? NY_PRODUCT_FAMILY
                                                 : NY_PRODUCT_GUEST;
            *generation = ny_access_generation();
            ret = 0;
            break;
          }
#endif
      }
    }
  cJSON_Delete(members);
  nxmutex_unlock(&g_access_lock);
  memset(digest, 0, sizeof(digest));
  return ret;
}

/****************************************************************************
 * Name: ny_access_allowed
 ****************************************************************************/
bool ny_access_allowed(enum ny_product_role_e role, const char *topic)
{
  static const char *const read_topics[] = {
    "sys.ping",    "sys.info",    "eyes.state.get", "plugin.list",
    "plugin.icon", "ui.tree",     "device.status",  "network.status",
    "weather.get", "light.status"
  };
  static const char *const family_topics[] = {
    "ui.event",          "eyes.expression", "eyes.gaze",     "eyes.blink",
    "eyes.auto_blink",   "eyes.ambient",    "eyes.iris",     "eyes.scene.show",
    "eyes.scene.update", "eyes.scene.hide", "alarm.list",    "alarm.create",
    "alarm.update",      "alarm.delete",    "alarm.snooze",  "alarm.dismiss",
    "timer.list",        "timer.create",    "timer.pause",   "timer.resume",
    "timer.delete",      "timer.lap",       "music.library", "music.status",
    "music.play",        "music.pause",     "music.resume",  "music.stop",
    "music.volume",      "dsp.status",      "dsp.config",    "dsp.play",
    "dsp.stop",          "bt.status"
  };
  if (role == NY_PRODUCT_OWNER)
    return true;
  for (size_t i = 0; i < sizeof(read_topics) / sizeof(read_topics[0]); i++)
    if (strcmp(topic, read_topics[i]) == 0)
      return true;
  if (role == NY_PRODUCT_FAMILY)
    for (size_t i = 0; i < sizeof(family_topics) / sizeof(family_topics[0]);
         i++)
      if (strcmp(topic, family_topics[i]) == 0)
        return true;
  return false;
}

/****************************************************************************
 * Name: ny_access_request
 ****************************************************************************/
int ny_access_request(const struct ny_product_caller_s *caller,
                      const char *topic, const cJSON *data, cJSON **result)
{
  cJSON *members = NULL;
  cJSON *entry = NULL;
  uint64_t revision;
  char token[NY_ACCESS_HEX_SIZE] = { 0 };
  char digest[NY_ACCESS_HEX_SIZE];
  const char *id = ny_access_string(data, "memberId");
  const char *role = ny_access_string(data, "role");
  const char *name = ny_access_string(data, "name");
  int ret;
  if (strncmp(topic, "access.", 7) != 0)
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  nxmutex_lock(&g_access_lock);
  ret = ny_access_read(&members, &revision);
  if (ret < 0)
    goto out;
  if (strcmp(topic, "access.members") == 0)
    goto reply;
  if (strcmp(topic, "access.create") == 0)
    {
      if (cJSON_GetArraySize(members) >= NY_ACCESS_MAX_MEMBERS)
        {
          ret = -ENOSPC;
          goto out;
        }
      if (name == NULL || strlen(name) == 0 || strlen(name) > 63 ||
          role == NULL || (strcmp(role, "family") && strcmp(role, "guest")))
        {
          ret = -EINVAL;
          goto out;
        }
      ret = ny_access_random(token);
      if (ret == 0)
        ret = ny_access_digest(token, digest);
      if (ret < 0)
        goto out;
      entry = cJSON_CreateObject();
      if (entry == NULL)
        {
          ret = -ENOMEM;
          goto out;
        }
      cJSON_AddItemToArray(members, entry);
      char member_id[24];
      snprintf(member_id, sizeof(member_id), "member-%.16s", digest);
      if (!cJSON_AddStringToObject(entry, "id", member_id) ||
          !cJSON_AddStringToObject(entry, "name", name) ||
          !cJSON_AddStringToObject(entry, "role", role) ||
          !cJSON_AddStringToObject(entry, "digest", digest))
        {
          ret = -ENOMEM;
          goto out;
        }
    }
  else if (strcmp(topic, "access.revoke") == 0 ||
           strcmp(topic, "access.role") == 0)
    {
      if (id == NULL)
        {
          ret = -EINVAL;
          goto out;
        }
      cJSON_ArrayForEach(entry, members)
      {
        const char *saved = ny_access_string(entry, "id");
        if (saved != NULL && strcmp(saved, id) == 0)
          break;
      }
      if (entry == NULL)
        {
          ret = -ENOENT;
          goto out;
        }
      if (strcmp(topic, "access.revoke") == 0)
        {
          cJSON_DetachItemViaPointer(members, entry);
          cJSON_Delete(entry);
        }
      else
        {
          if (role == NULL ||
              (strcmp(role, "family") && strcmp(role, "guest")))
            {
              ret = -EINVAL;
              goto out;
            }
          cJSON *value = cJSON_CreateString(role);
          if (value == NULL)
            {
              ret = -ENOMEM;
              goto out;
            }
          cJSON_ReplaceItemInObjectCaseSensitive(entry, "role", value);
        }
    }
  else
    {
      ret = -ENOSYS;
      goto out;
    }
  ret = ny_product_store_write("access", members, revision, &revision);
  if (ret < 0)
    goto out;
  atomic_fetch_add(&g_access_generation, 1);
reply:
  *result = ny_access_public(members);
  if (*result == NULL)
    ret = -ENOMEM;
  else if (token[0] && !cJSON_AddStringToObject(*result, "token", token))
    ret = -ENOMEM;
out:
  memset(token, 0, sizeof(token));
  memset(digest, 0, sizeof(digest));
  cJSON_Delete(members);
  nxmutex_unlock(&g_access_lock);
  return ret;
}
