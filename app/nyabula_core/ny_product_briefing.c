/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_briefing.c
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
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>
#endif

#define NY_BRIEFING_LIMIT   12
#define NY_BRIEFING_CARD_MS 8000
static mutex_t g_briefing_lock = NXMUTEX_INITIALIZER;
static uint64_t g_briefing_tick;

static const char *ny_briefing_text(const cJSON *row, const char *key);
static int ny_briefing_load(cJSON **root, uint64_t *revision);
static bool ny_briefing_add(cJSON *items, const char *title, const char *text,
                            const char *source);
static int ny_briefing_generate(const struct ny_product_caller_s *caller,
                                const cJSON *settings, cJSON **result);
static bool ny_briefing_number(cJSON *root, const char *key, double value);

/****************************************************************************
 * Name: ny_briefing_text
 ****************************************************************************/

static const char *ny_briefing_text(const cJSON *row, const char *key)
{
  const char *text =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
  return text ? text : "";
}

/****************************************************************************
 * Name: ny_briefing_number
 ****************************************************************************/

static bool ny_briefing_number(cJSON *root, const char *key, double value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsNumber(item))
    {
      cJSON_SetNumberValue(item, value);
      return true;
    }
  return cJSON_AddNumberToObject(root, key, value) != NULL;
}

/****************************************************************************
 * Name: ny_briefing_load
 ****************************************************************************/

static int ny_briefing_load(cJSON **root, uint64_t *revision)
{
  int ret = ny_product_store_read("briefing", root, revision);
  if (ret < 0)
    return ret;
  if (!*root)
    {
      *root = cJSON_CreateObject();
      if (!*root || !cJSON_AddArrayToObject(*root, "items") ||
          !cJSON_AddBoolToObject(*root, "playing", false) ||
          !cJSON_AddBoolToObject(*root, "schedule_enabled", false) ||
          !ny_briefing_number(*root, "morning_minute", 480) ||
          !ny_briefing_number(*root, "evening_minute", 1200) ||
          !ny_briefing_number(*root, "utc_offset_minutes", 0) ||
          !cJSON_AddStringToObject(*root, "last_slot", "") ||
          !cJSON_AddStringToObject(*root, "owner", "") ||
          !ny_briefing_number(*root, "index", 0) ||
          !ny_briefing_number(*root, "next_at", 0))
        return -ENOMEM;
    }
  cJSON *items = cJSON_GetObjectItemCaseSensitive(*root, "items");
  double index =
      cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(*root, "index"));
  if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > NY_BRIEFING_LIMIT ||
      !isfinite(index) || index < 0 || floor(index) != index ||
      index > cJSON_GetArraySize(items) ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(*root, "playing")))
    return -EBADMSG;
  double morning = cJSON_GetNumberValue(
      cJSON_GetObjectItemCaseSensitive(*root, "morning_minute"));
  double evening = cJSON_GetNumberValue(
      cJSON_GetObjectItemCaseSensitive(*root, "evening_minute"));
  double offset = cJSON_GetNumberValue(
      cJSON_GetObjectItemCaseSensitive(*root, "utc_offset_minutes"));
  if (!cJSON_IsBool(
          cJSON_GetObjectItemCaseSensitive(*root, "schedule_enabled")) ||
      !isfinite(morning) || morning < 0 || morning > 1439 ||
      !isfinite(evening) || evening < 0 || evening > 1439 ||
      !isfinite(offset) || offset < -720 || offset > 840)
    return -EBADMSG;
  return 0;
}

/****************************************************************************
 * Name: ny_briefing_add
 ****************************************************************************/

static bool ny_briefing_add(cJSON *items, const char *title, const char *text,
                            const char *source)
{
  if (cJSON_GetArraySize(items) >= NY_BRIEFING_LIMIT)
    return true;
  cJSON *row = cJSON_CreateObject();
  if (!row || !cJSON_AddItemToArray(items, row))
    {
      cJSON_Delete(row);
      return false;
    }
  char id[24];
  snprintf(id, sizeof(id), "brief-%d", cJSON_GetArraySize(items));
  return cJSON_AddStringToObject(row, "id", id) &&
         cJSON_AddStringToObject(row, "title", title) &&
         cJSON_AddStringToObject(row, "text", text) &&
         cJSON_AddStringToObject(row, "source", source);
}

/****************************************************************************
 * Name: ny_briefing_generate
 ****************************************************************************/

static int ny_briefing_generate(const struct ny_product_caller_s *caller,
                                const cJSON *settings, cJSON **result)
{
  uint64_t now = ny_product_time_ms(false);
  if (!ny_product_clock_valid())
    return -EAGAIN;
  cJSON *root = cJSON_CreateObject();
  cJSON *items = root ? cJSON_AddArrayToObject(root, "items") : NULL;
  cJSON *empty = cJSON_CreateObject();
  if (!items || !empty ||
      !cJSON_AddNumberToObject(root, "generated_at", now) ||
      !cJSON_AddBoolToObject(root, "playing", false) ||
      !ny_briefing_number(root, "index", 0) ||
      !ny_briefing_number(root, "next_at", 0))
    {
      cJSON_Delete(root);
      cJSON_Delete(empty);
      return -ENOMEM;
    }
  const char *keys[] = { "morning_minute", "evening_minute",
                         "utc_offset_minutes" };
  bool schedule = settings && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                  settings, "schedule_enabled"));
  if (!cJSON_AddBoolToObject(root, "schedule_enabled", schedule) ||
      !cJSON_AddStringToObject(
          root, "last_slot",
          settings ? ny_briefing_text(settings, "last_slot") : "") ||
      !cJSON_AddStringToObject(
          root, "owner", settings ? ny_briefing_text(settings, "owner") : ""))
    {
      cJSON_Delete(root);
      cJSON_Delete(empty);
      return -ENOMEM;
    }
  for (int i = 0; i < 3; i++)
    if (!ny_briefing_number(
            root, keys[i],
            settings ? cJSON_GetNumberValue(
                           cJSON_GetObjectItemCaseSensitive(settings, keys[i]))
                     : (i == 0   ? 480
                        : i == 1 ? 1200
                                 : 0)))
      {
        cJSON_Delete(root);
        cJSON_Delete(empty);
        return -ENOMEM;
      }
  cJSON *records = NULL;
  int ret = 0;
#ifdef CONFIG_NYABULA_CORE_WEATHER
  ret = ny_product_weather_request(caller, "weather.get", empty, &records);
  if (!ret)
    {
      const cJSON *temperature =
          cJSON_GetObjectItemCaseSensitive(records, "temperature");
      double value = cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(temperature, "value"));
      double fetched = cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(records, "fetched_at"));
      char text[256];
      if (isfinite(value) && isfinite(fetched))
        {
          snprintf(text, sizeof(text), "%s %s %.1f °C%s",
                   ny_briefing_text(
                       cJSON_GetObjectItemCaseSensitive(records, "location"),
                       "name"),
                   ny_briefing_text(
                       cJSON_GetObjectItemCaseSensitive(records, "condition"),
                       "text"),
                   value,
                   (now > fetched + 1200000 ||
                    cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                        records, "last_error")) != 0)
                       ? "（缓存可能过时）"
                       : "");
          if (!ny_briefing_add(items, "天气", text, "QWeather"))
            ret = -ENOMEM;
        }
      else if (!ny_briefing_add(items, "天气",
                                "尚未获取天气，不推测当前天气。", "device"))
        ret = -ENOMEM;
    }
  else if (ret == -ENODATA)
    ret = 0;
  cJSON_Delete(records);
  records = NULL;
#endif
  const char *topics[] = { "calendar.list", "task.list", "memory.list" };
  for (int domain = 0; ret == 0 && domain < 3; domain++)
    {
      ret =
          ny_product_records_request(caller, topics[domain], empty, &records);
      if (ret < 0)
        break;
      cJSON *row;
      int count = 0;
      cJSON_ArrayForEach(row,
                         cJSON_GetObjectItemCaseSensitive(records, "items"))
      {
        if (count >= 3)
          break;
        if (domain == 0)
          {
            double start = cJSON_GetNumberValue(
                cJSON_GetObjectItemCaseSensitive(row, "start_at"));
            if (!isfinite(start) || start < now || start >= now + 86400000ULL)
              continue;
            char text[256];
            snprintf(text, sizeof(text), "%s（约 %.0f 分钟后）",
                     ny_briefing_text(row, "title"), (start - now) / 60000);
            if (!ny_briefing_add(items, "未来 24 小时日程", text, "calendar"))
              ret = -ENOMEM;
          }
        else if (domain == 1)
          {
            const char *state = ny_briefing_text(row, "state");
            if (strcmp(state, "queued") && strcmp(state, "running"))
              continue;
            if (!ny_briefing_add(items, "待办", ny_briefing_text(row, "title"),
                                 "tasks"))
              ret = -ENOMEM;
          }
        else if (!ny_briefing_add(items, "你保存的记忆",
                                  ny_briefing_text(row, "text"), "memory"))
          ret = -ENOMEM;
        if (ret < 0)
          break;
        count++;
      }
      cJSON_Delete(records);
      records = NULL;
    }
  if (!ret && !cJSON_GetArraySize(items) &&
      !ny_briefing_add(items, "今日简报", "暂时没有待办、日程或已保存记忆。",
                       "device"))
    ret = -ENOMEM;
  cJSON_Delete(records);
  cJSON_Delete(empty);
  if (ret < 0)
    cJSON_Delete(root);
  else
    *result = root;
  return ret;
}

/****************************************************************************
 * Name: ny_product_briefing_request
 ****************************************************************************/

int ny_product_briefing_request(const struct ny_product_caller_s *caller,
                                const char *topic, const cJSON *data,
                                cJSON **result)
{
  if (strncmp(topic, "briefing.", 9))
    return -ENOSYS;
  if (caller->role < NY_PRODUCT_FAMILY)
    return -EACCES;
  const char *op = topic + 9;
  if (strcmp(op, "get") && strcmp(op, "generate") && strcmp(op, "configure") &&
      strcmp(op, "start") && strcmp(op, "stop") && strcmp(op, "next"))
    return -ENOSYS;
  int ret = nxmutex_lock(&g_briefing_lock);
  if (ret < 0)
    return ret;
  cJSON *root = NULL;
  uint64_t revision;
  ret = ny_briefing_load(&root, &revision);
  if (ret < 0)
    goto out;
  if (strcmp(op, "get"))
    {
      double expected = cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(data, "revision"));
      if (!isfinite(expected) || expected < 0 || floor(expected) != expected)
        {
          ret = -EINVAL;
          goto out;
        }
      if (expected != revision)
        {
          ret = -ESTALE;
          goto out;
        }
      if (!strcmp(op, "generate"))
        {
          cJSON *generated = NULL;
          ret = ny_briefing_generate(caller, root, &generated);
          if (ret < 0)
            goto out;
          cJSON_Delete(root);
          root = generated;
        }
      else if (!strcmp(op, "configure"))
        {
          if (caller->role != NY_PRODUCT_OWNER || !caller->local_transport)
            {
              ret = -EACCES;
              goto out;
            }
          const cJSON *enabled =
              cJSON_GetObjectItemCaseSensitive(data, "schedule_enabled");
          double morning = cJSON_GetNumberValue(
              cJSON_GetObjectItemCaseSensitive(data, "morning_minute"));
          double evening = cJSON_GetNumberValue(
              cJSON_GetObjectItemCaseSensitive(data, "evening_minute"));
          double offset = cJSON_GetNumberValue(
              cJSON_GetObjectItemCaseSensitive(data, "utc_offset_minutes"));
          if (!cJSON_IsBool(enabled) || !isfinite(morning) || morning < 0 ||
              morning > 1439 || floor(morning) != morning ||
              !isfinite(evening) || evening < 0 || evening > 1439 ||
              floor(evening) != evening || !isfinite(offset) ||
              offset < -720 || offset > 840 || floor(offset) != offset ||
              !ny_briefing_number(root, "morning_minute", morning) ||
              !ny_briefing_number(root, "evening_minute", evening) ||
              !ny_briefing_number(root, "utc_offset_minutes", offset) ||
              !cJSON_ReplaceItemInObjectCaseSensitive(
                  root, "schedule_enabled",
                  cJSON_CreateBool(cJSON_IsTrue(enabled))) ||
              !cJSON_ReplaceItemInObjectCaseSensitive(
                  root, "owner", cJSON_CreateString(caller->id)))
            {
              ret = -EINVAL;
              goto out;
            }
        }
      else
        {
          cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
          cJSON *playing = cJSON_GetObjectItemCaseSensitive(root, "playing");
          if (!strcmp(op, "stop"))
            playing->type = (playing->type & ~0xff) | cJSON_False;
          else
            {
              if (!cJSON_GetArraySize(items))
                {
                  ret = -ENODATA;
                  goto out;
                }
              playing->type = (playing->type & ~0xff) | cJSON_True;
              double index = !strcmp(op, "start")
                                 ? 0
                                 : fmod(cJSON_GetNumberValue(
                                            cJSON_GetObjectItemCaseSensitive(
                                                root, "index")) +
                                            1,
                                        cJSON_GetArraySize(items));
              if (!ny_briefing_number(root, "index", index) ||
                  !ny_briefing_number(root, "next_at", 0))
                {
                  ret = -ENOMEM;
                  goto out;
                }
            }
        }
      ret = ny_product_store_write("briefing", root, revision, &revision);
      if (ret < 0)
        goto out;
    }
  if (!cJSON_AddNumberToObject(root, "revision", revision))
    {
      ret = -ENOMEM;
      goto out;
    }
  *result = root;
  root = NULL;
out:
  cJSON_Delete(root);
  nxmutex_unlock(&g_briefing_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_briefing_tick
 ****************************************************************************/

int ny_product_briefing_tick(void)
{
  if (ny_product_time_ms(true) < g_briefing_tick)
    return 0;
  g_briefing_tick = ny_product_time_ms(true) + 1000;
  int ret = nxmutex_lock(&g_briefing_lock);
  if (ret < 0)
    return ret;
  cJSON *root = NULL;
  uint64_t revision;
  ret = ny_briefing_load(&root, &revision);
  if (ret < 0)
    goto out;
  uint64_t now = ny_product_time_ms(false);
  if (!ny_product_clock_valid())
    goto out;
  if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "playing")) &&
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "schedule_enabled")))
    {
      int64_t local =
          (int64_t)now +
          (int64_t)cJSON_GetNumberValue(
              cJSON_GetObjectItemCaseSensitive(root, "utc_offset_minutes")) *
              60000;
      int minute = (int)((local % 86400000 + 86400000) % 86400000) / 60000;
      int slot = -1;
      int morning = (int)cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(root, "morning_minute"));
      int evening = (int)cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(root, "evening_minute"));
      if (minute >= morning && minute < morning + 5)
        slot = 0;
      else if (minute >= evening && minute < evening + 5)
        slot = 1;
      char slot_id[40];
      snprintf(slot_id, sizeof(slot_id), "%lld-%d",
               (long long)(local / 86400000), slot);
      if (slot >= 0 && strcmp(slot_id, ny_briefing_text(root, "last_slot")))
        {
          struct ny_product_caller_s caller = {
            ny_briefing_text(root, "owner"), NY_PRODUCT_OWNER, false
          };
          cJSON *generated = NULL;
          ret = caller.id[0] ? ny_briefing_generate(&caller, root, &generated)
                             : -EINVAL;
          if (ret < 0)
            goto out;
          cJSON_Delete(root);
          root = generated;
          if (!ny_briefing_text(root, "last_slot")[0])
            cJSON_AddStringToObject(root, "last_slot", slot_id);
          else
            cJSON_SetValuestring(
                cJSON_GetObjectItemCaseSensitive(root, "last_slot"), slot_id);
          cJSON *playing = cJSON_GetObjectItemCaseSensitive(root, "playing");
          playing->type = (playing->type & ~0xff) | cJSON_True;
          ret = ny_product_store_write("briefing", root, revision, &revision);
          if (ret < 0)
            goto out;
        }
    }
  if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "playing")))
    goto out;
  double next =
      cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "next_at"));
  if (next > now)
    goto out;
  int index = (int)cJSON_GetNumberValue(
      cJSON_GetObjectItemCaseSensitive(root, "index"));
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
  if (next > 0)
    index++;
  if (index >= cJSON_GetArraySize(items) || (next > 0 && now > next + 60000))
    {
      cJSON *playing = cJSON_GetObjectItemCaseSensitive(root, "playing");
      playing->type = (playing->type & ~0xff) | cJSON_False;
    }
  else
    {
#ifdef CONFIG_NYABULA_CORE_EYE
      const char *text =
          ny_briefing_text(cJSON_GetArrayItem(items, index), "text");
      /* The compact display shows a title; full content stays in the Web. */
      const char *title =
          ny_briefing_text(cJSON_GetArrayItem(items, index), "title");
      (void)text;
      ret = nyabula_eye_service_notify("nybriefing", title, strlen(title));
      if (ret < 0)
        goto out;
#endif
      if (!ny_briefing_number(root, "index", index) ||
          !ny_briefing_number(root, "next_at", now + NY_BRIEFING_CARD_MS))
        {
          ret = -ENOMEM;
          goto out;
        }
    }
  ret = ny_product_store_write("briefing", root, revision, &revision);
out:
  cJSON_Delete(root);
  nxmutex_unlock(&g_briefing_lock);
  return ret;
}
