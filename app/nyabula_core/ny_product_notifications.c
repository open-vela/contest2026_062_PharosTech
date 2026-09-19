/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_notifications.c
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

#include "ny_product.h"
#include "ny_product_store.h"
#include "ny_utf8.h"
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>
#endif

#define NY_NOTICE_ITEMS        16
#define NY_NOTICE_RECEIPTS     96
#define NY_NOTICE_RETENTION_MS 604800000ULL

static mutex_t g_notice_lock = NXMUTEX_INITIALIZER;
static cJSON *g_notices;
static uint64_t g_notice_revision;
static uint64_t g_notice_next_eye;

static const char *ny_notice_text(const cJSON *row, const char *key);
static int ny_notice_load(void);
static int ny_notice_save(cJSON **candidate);
static cJSON *ny_notice_result(void);

/****************************************************************************
 * Name: ny_notice_text
 ****************************************************************************/

static const char *ny_notice_text(const cJSON *row, const char *key)
{
  const char *text =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
  return text ? text : "";
}

/****************************************************************************
 * Name: ny_notice_load
 ****************************************************************************/

static int ny_notice_load(void)
{
  if (g_notices)
    return 0;
  cJSON *root = NULL;
  uint64_t revision;
  int ret = ny_product_store_read("notifications", &root, &revision);
  if (ret < 0)
    return ret;
  if (!root)
    {
      root = cJSON_CreateObject();
      if (!root || !cJSON_AddArrayToObject(root, "items") ||
          !cJSON_AddArrayToObject(root, "receipts"))
        {
          cJSON_Delete(root);
          return -ENOMEM;
        }
    }
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
  cJSON *receipts = cJSON_GetObjectItemCaseSensitive(root, "receipts");
  if (!cJSON_IsObject(root) || !cJSON_IsArray(items) ||
      !cJSON_IsArray(receipts) ||
      cJSON_GetArraySize(items) > NY_NOTICE_ITEMS ||
      cJSON_GetArraySize(receipts) > NY_NOTICE_RECEIPTS)
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }
  cJSON *row;
  cJSON_ArrayForEach(
      row,
      items) if (!cJSON_IsObject(row) || !ny_notice_text(row, "id")[0] ||
                 strlen(ny_notice_text(row, "id")) > 96 ||
                 !cJSON_IsString(
                     cJSON_GetObjectItemCaseSensitive(row, "title")) ||
                 !cJSON_IsBool(
                     cJSON_GetObjectItemCaseSensitive(row, "read")) ||
                 !cJSON_IsBool(
                     cJSON_GetObjectItemCaseSensitive(row, "eye_sent")) ||
                 !isfinite(cJSON_GetNumberValue(
                     cJSON_GetObjectItemCaseSensitive(row, "expires_at"))))
  {
    cJSON_Delete(root);
    return -EBADMSG;
  }
  cJSON_ArrayForEach(
      row, receipts) if (!ny_notice_text(row, "id")[0] ||
                         strlen(ny_notice_text(row, "id")) > 96 ||
                         !isfinite(cJSON_GetNumberValue(
                             cJSON_GetObjectItemCaseSensitive(row, "until"))))
  {
    cJSON_Delete(root);
    return -EBADMSG;
  }
  g_notices = root;
  g_notice_revision = revision;
  return 0;
}

/****************************************************************************
 * Name: ny_notice_save
 ****************************************************************************/

static int ny_notice_save(cJSON **candidate)
{
  uint64_t revision;
  int ret = ny_product_store_write("notifications", *candidate,
                                   g_notice_revision, &revision);
  if (!ret)
    {
      cJSON_Delete(g_notices);
      g_notices = *candidate;
      *candidate = NULL;
      g_notice_revision = revision;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_notice_result
 ****************************************************************************/

static cJSON *ny_notice_result(void)
{
  cJSON *root = cJSON_Duplicate(g_notices, true);
  if (!root)
    return NULL;
  cJSON_DeleteItemFromObjectCaseSensitive(root, "receipts");
  if (!cJSON_AddNumberToObject(root, "revision", g_notice_revision))
    {
      cJSON_Delete(root);
      return NULL;
    }
  return root;
}

/****************************************************************************
 * Name: ny_product_notification_post
 * Description: Atomically persist inbox item and deduplication receipt.
 ****************************************************************************/

int ny_product_notification_post(const char *id, const char *source,
                                 const char *title, uint64_t expires)
{
  if (!id || !id[0] || strlen(id) > 96 || !source || strlen(source) > 24 ||
      !title || !title[0] ||
      !ny_utf8_valid((const unsigned char *)title, strlen(title)))
    return -EINVAL;
  int ret = nxmutex_lock(&g_notice_lock);
  if (ret < 0)
    return ret;
  cJSON *candidate = NULL;
  cJSON *row;
  ret = ny_notice_load();
  if (ret < 0)
    goto out;
  cJSON_ArrayForEach(
      row, cJSON_GetObjectItemCaseSensitive(
               g_notices, "receipts")) if (!strcmp(ny_notice_text(row, "id"),
                                                   id)) goto out;
  candidate = cJSON_Duplicate(g_notices, true);
  if (!candidate)
    {
      ret = -ENOMEM;
      goto out;
    }
  cJSON *items = cJSON_GetObjectItemCaseSensitive(candidate, "items");
  cJSON *receipts = cJSON_GetObjectItemCaseSensitive(candidate, "receipts");
  uint64_t now = ny_product_time_ms(false);
  for (int i = cJSON_GetArraySize(receipts) - 1; i >= 0; i--)
    {
      row = cJSON_GetArrayItem(receipts, i);
      double until =
          cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "until"));
      if (until > 0 && until < now)
        cJSON_DeleteItemFromArray(receipts, i);
    }
  if (cJSON_GetArraySize(receipts) >= NY_NOTICE_RECEIPTS)
    {
      ret = -ENOSPC;
      goto out;
    }
  if (cJSON_GetArraySize(items) >= NY_NOTICE_ITEMS)
    {
      int remove = -1;
      for (int i = 0; i < cJSON_GetArraySize(items); i++)
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                cJSON_GetArrayItem(items, i), "eye_sent")))
          {
            remove = i;
            break;
          }
      if (remove < 0)
        {
          ret = -ENOSPC;
          goto out;
        }
      cJSON_DeleteItemFromArray(items, remove);
    }
  char short_title[193];
  size_t length = strlen(title);
  if (length >= sizeof(short_title))
    length = sizeof(short_title) - 1;
  while (length && !ny_utf8_valid((const unsigned char *)title, length))
    length--;
  memcpy(short_title, title, length);
  short_title[length] = 0;
  row = cJSON_CreateObject();
  if (!row || !cJSON_AddItemToArray(items, row))
    {
      cJSON_Delete(row);
      ret = -ENOMEM;
      goto out;
    }
  if (!cJSON_AddStringToObject(row, "id", id) ||
      !cJSON_AddStringToObject(row, "source", source) ||
      !cJSON_AddStringToObject(row, "title", short_title) ||
      !cJSON_AddNumberToObject(row, "at", now) ||
      !cJSON_AddNumberToObject(row, "expires_at", expires) ||
      !cJSON_AddBoolToObject(row, "read", false) ||
      !cJSON_AddBoolToObject(row, "eye_sent", false))
    {
      ret = -ENOMEM;
      goto out;
    }
  row = cJSON_CreateObject();
  if (!row || !cJSON_AddItemToArray(receipts, row))
    {
      cJSON_Delete(row);
      ret = -ENOMEM;
      goto out;
    }
  if (!cJSON_AddStringToObject(row, "id", id) ||
      !cJSON_AddNumberToObject(row, "until",
                               (expires > now ? expires : now) +
                                   NY_NOTICE_RETENTION_MS))
    {
      ret = -ENOMEM;
      goto out;
    }
  ret = ny_notice_save(&candidate);
out:
  cJSON_Delete(candidate);
  nxmutex_unlock(&g_notice_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_notifications_request
 ****************************************************************************/

int ny_product_notifications_request(const struct ny_product_caller_s *caller,
                                     const char *topic, const cJSON *data,
                                     cJSON **result)
{
  if (strcmp(topic, "notification.list") && strcmp(topic, "notification.read"))
    return -ENOSYS;
  if (caller->role < NY_PRODUCT_FAMILY)
    return -EACCES;
  int ret = nxmutex_lock(&g_notice_lock);
  if (ret < 0)
    return ret;
  cJSON *candidate = NULL;
  ret = ny_notice_load();
  if (ret < 0)
    goto out;
  if (!strcmp(topic, "notification.read"))
    {
      candidate = cJSON_Duplicate(g_notices, true);
      if (!candidate)
        {
          ret = -ENOMEM;
          goto out;
        }
      cJSON *row;
      bool found = false;
      cJSON_ArrayForEach(
          row,
          cJSON_GetObjectItemCaseSensitive(
              candidate, "items")) if (!strcmp(ny_notice_text(row, "id"),
                                               ny_notice_text(data, "id")))
      {
        cJSON *read = cJSON_GetObjectItemCaseSensitive(row, "read");
        read->type = (read->type & ~0xff) | cJSON_True;
        found = true;
      }
      if (!found)
        {
          ret = -ENOENT;
          goto out;
        }
      ret = ny_notice_save(&candidate);
      if (ret < 0)
        goto out;
    }
  *result = ny_notice_result();
  ret = *result ? 0 : -ENOMEM;
out:
  cJSON_Delete(candidate);
  nxmutex_unlock(&g_notice_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_notifications_tick
 ****************************************************************************/

int ny_product_notifications_tick(void)
{
  if (ny_product_time_ms(true) < g_notice_next_eye)
    return 0;
  int ret = nxmutex_lock(&g_notice_lock);
  if (ret < 0)
    return ret;
  cJSON *candidate = NULL;
  ret = ny_notice_load();
  if (ret < 0)
    goto out;
  cJSON *row;
  int index = 0;
  cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(g_notices, "items"))
  {
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "eye_sent")))
      {
        double expires = cJSON_GetNumberValue(
            cJSON_GetObjectItemCaseSensitive(row, "expires_at"));
#ifdef CONFIG_NYABULA_CORE_EYE
        if (expires > ny_product_time_ms(false))
          {
            char text[96];
            const char *title = ny_notice_text(row, "title");
            size_t length = strlen(title);
            if (length >= sizeof(text))
              length = sizeof(text) - 1;
            while (length &&
                   !ny_utf8_valid((const unsigned char *)title, length))
              length--;
            memcpy(text, title, length);
            text[length] = 0;
            ret = nyabula_eye_service_notify("nynotice", text, length);
            if (ret < 0)
              goto out;
          }
#else
        (void)expires;
#endif
        candidate = cJSON_Duplicate(g_notices, true);
        if (!candidate)
          {
            ret = -ENOMEM;
            goto out;
          }
        cJSON *sent = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetArrayItem(
                cJSON_GetObjectItemCaseSensitive(candidate, "items"), index),
            "eye_sent");
        sent->type = (sent->type & ~0xff) | cJSON_True;
        ret = ny_notice_save(&candidate);
        g_notice_next_eye = ny_product_time_ms(true) + 6000;
        break;
      }
    index++;
  }
out:
  cJSON_Delete(candidate);
  nxmutex_unlock(&g_notice_lock);
  return ret;
}
