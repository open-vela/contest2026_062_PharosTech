/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_companion.c
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

#include "ny_agent.h"
#include "ny_product.h"
#include "ny_product_store.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <string.h>

#define NY_COMPANION_CLOCK_MIN 1577836800000ULL
#define NY_COMPANION_TICK_MS   1000
static mutex_t g_companion_lock = NXMUTEX_INITIALIZER;
static uint64_t g_companion_tick;

static const char *ny_companion_text(const cJSON *row, const char *key);
static double ny_companion_number(const cJSON *row, const char *key);
static bool ny_companion_set_number(cJSON *row, const char *key, double value);
static bool ny_companion_set_string(cJSON *row, const char *key,
                                    const char *value);
static bool ny_companion_set_bool(cJSON *row, const char *key, bool value);
static int ny_companion_load(cJSON **root, uint64_t *revision);
static bool ny_companion_quiet(const cJSON *root, uint64_t now);
static bool ny_companion_roll_day(cJSON *root, uint64_t now);
static cJSON *ny_companion_public(const cJSON *root, uint64_t revision,
                                  uint64_t now);

/****************************************************************************
 * Name: ny_companion_text
 ****************************************************************************/

static const char *ny_companion_text(const cJSON *row, const char *key)
{
  const char *value =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
  return value ? value : "";
}

static double ny_companion_number(const cJSON *row, const char *key)
{
  return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, key));
}

static bool ny_companion_set_number(cJSON *row, const char *key, double value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(row, key);
  if (cJSON_IsNumber(item))
    {
      cJSON_SetNumberValue(item, value);
      return true;
    }
  return cJSON_AddNumberToObject(row, key, value) != NULL;
}

static bool ny_companion_set_string(cJSON *row, const char *key,
                                    const char *value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(row, key);
  return cJSON_IsString(item)
             ? cJSON_SetValuestring(item, value) != NULL
             : cJSON_AddStringToObject(row, key, value) != NULL;
}

static bool ny_companion_set_bool(cJSON *row, const char *key, bool value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(row, key);
  if (cJSON_IsBool(item))
    {
      item->type = (item->type & ~0xff) | (value ? cJSON_True : cJSON_False);
      return true;
    }
  return cJSON_AddBoolToObject(row, key, value) != NULL;
}

/****************************************************************************
 * Name: ny_companion_load
 ****************************************************************************/

static int ny_companion_load(cJSON **root, uint64_t *revision)
{
  int ret = ny_product_store_read("companion", root, revision);
  if (ret < 0)
    return ret;
  if (!*root)
    {
      *root = cJSON_CreateObject();
      if (!*root || !ny_companion_set_bool(*root, "enabled", false) ||
          !ny_companion_set_string(*root, "mode", "quiet") ||
          !ny_companion_set_number(*root, "quiet_start", 1320) ||
          !ny_companion_set_number(*root, "quiet_end", 480) ||
          !ny_companion_set_number(*root, "utc_offset_minutes", 0) ||
          !ny_companion_set_number(*root, "minimum_interval_minutes", 180) ||
          !ny_companion_set_number(*root, "daily_limit", 3) ||
          !ny_companion_set_number(*root, "day_index", 0) ||
          !ny_companion_set_number(*root, "daily_count", 0) ||
          !ny_companion_set_number(*root, "next_at", 0) ||
          !ny_companion_set_number(*root, "last_at", 0) ||
          !ny_companion_set_number(*root, "last_error", 0) ||
          !ny_companion_set_string(*root, "pending_run", "") ||
          !ny_companion_set_string(*root, "owner", ""))
        return -ENOMEM;
    }
  const char *mode = ny_companion_text(*root, "mode");
  double quiet_start = ny_companion_number(*root, "quiet_start");
  double quiet_end = ny_companion_number(*root, "quiet_end");
  double offset = ny_companion_number(*root, "utc_offset_minutes");
  double interval = ny_companion_number(*root, "minimum_interval_minutes");
  double limit = ny_companion_number(*root, "daily_limit");
  if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(*root, "enabled")) ||
      (strcmp(mode, "quiet") && strcmp(mode, "interactive") &&
       strcmp(mode, "story")) ||
      !isfinite(quiet_start) || quiet_start < 0 || quiet_start > 1439 ||
      !isfinite(quiet_end) || quiet_end < 0 || quiet_end > 1439 ||
      !isfinite(offset) || offset < -720 || offset > 840 ||
      !isfinite(interval) || interval < 30 || interval > 1440 ||
      !isfinite(limit) || limit < 1 || limit > 8)
    return -EBADMSG;
  return 0;
}

/****************************************************************************
 * Name: ny_companion_quiet
 ****************************************************************************/

static bool ny_companion_quiet(const cJSON *root, uint64_t now)
{
  int64_t local =
      (int64_t)now +
      (int64_t)ny_companion_number(root, "utc_offset_minutes") * 60000;
  int minute = (int)((local % 86400000 + 86400000) % 86400000) / 60000;
  int start = (int)ny_companion_number(root, "quiet_start");
  int end = (int)ny_companion_number(root, "quiet_end");
  return start == end || (start < end ? minute >= start && minute < end
                                      : minute >= start || minute < end);
}

static bool ny_companion_roll_day(cJSON *root, uint64_t now)
{
  int64_t local =
      (int64_t)now +
      (int64_t)ny_companion_number(root, "utc_offset_minutes") * 60000;
  int64_t day = local / 86400000;
  if (ny_companion_number(root, "day_index") != day)
    {
      ny_companion_set_number(root, "day_index", day);
      ny_companion_set_number(root, "daily_count", 0);
      return true;
    }
  return false;
}

/****************************************************************************
 * Name: ny_companion_public
 ****************************************************************************/

static cJSON *ny_companion_public(const cJSON *root, uint64_t revision,
                                  uint64_t now)
{
  cJSON *copy = cJSON_Duplicate(root, true);
  if (!copy)
    return NULL;
  cJSON_DeleteItemFromObjectCaseSensitive(copy, "owner");
  if (!cJSON_AddNumberToObject(copy, "revision", revision) ||
      !cJSON_AddBoolToObject(copy, "quiet_now",
                             now >= NY_COMPANION_CLOCK_MIN &&
                                 ny_companion_quiet(root, now)))
    {
      cJSON_Delete(copy);
      return NULL;
    }
  return copy;
}

/****************************************************************************
 * Name: ny_product_companion_request
 ****************************************************************************/

int ny_product_companion_request(const struct ny_product_caller_s *caller,
                                 const char *topic, const cJSON *data,
                                 cJSON **result)
{
  if (strcmp(topic, "companion.get") && strcmp(topic, "companion.configure") &&
      strcmp(topic, "companion.run"))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  bool write = strcmp(topic, "companion.get") != 0;
  if (write && !caller->local_transport)
    return -EACCES;
  int ret = nxmutex_lock(&g_companion_lock);
  if (ret < 0)
    return ret;
  cJSON *root = NULL;
  uint64_t revision;
  uint64_t now = ny_product_time_ms(false);
  ret = ny_companion_load(&root, &revision);
  if (ret < 0)
    goto out;
  if (!strcmp(topic, "companion.configure"))
    {
      double expected = ny_companion_number(data, "revision");
      const char *mode = ny_companion_text(data, "mode");
      const double fields[] = {
        ny_companion_number(data, "quiet_start"),
        ny_companion_number(data, "quiet_end"),
        ny_companion_number(data, "utc_offset_minutes"),
        ny_companion_number(data, "minimum_interval_minutes"),
        ny_companion_number(data, "daily_limit")
      };
      if (!isfinite(expected) || floor(expected) != expected || expected < 0 ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(data, "enabled")) ||
          (strcmp(mode, "quiet") && strcmp(mode, "interactive") &&
           strcmp(mode, "story")) ||
          !isfinite(fields[0]) || fields[0] < 0 || fields[0] > 1439 ||
          floor(fields[0]) != fields[0] || !isfinite(fields[1]) ||
          fields[1] < 0 || fields[1] > 1439 || floor(fields[1]) != fields[1] ||
          !isfinite(fields[2]) || fields[2] < -720 || fields[2] > 840 ||
          floor(fields[2]) != fields[2] || !isfinite(fields[3]) ||
          fields[3] < 30 || fields[3] > 1440 ||
          floor(fields[3]) != fields[3] || !isfinite(fields[4]) ||
          fields[4] < 1 || fields[4] > 8 || floor(fields[4]) != fields[4])
        {
          ret = -EINVAL;
          goto out;
        }
      if (expected != revision)
        {
          ret = -ESTALE;
          goto out;
        }
      bool enabled =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "enabled"));
      bool ok =
          ny_companion_set_bool(root, "enabled", enabled) &&
          ny_companion_set_string(root, "mode", mode) &&
          ny_companion_set_number(root, "quiet_start", fields[0]) &&
          ny_companion_set_number(root, "quiet_end", fields[1]) &&
          ny_companion_set_number(root, "utc_offset_minutes", fields[2]) &&
          ny_companion_set_number(root, "minimum_interval_minutes",
                                  fields[3]) &&
          ny_companion_set_number(root, "daily_limit", fields[4]) &&
          ny_companion_set_string(root, "owner", caller->id) &&
          ny_companion_set_number(root, "next_at",
                                  enabled && now >= NY_COMPANION_CLOCK_MIN
                                      ? now + (uint64_t)fields[3] * 60000
                                      : 0);
      ret = ok ? ny_product_store_write("companion", root, revision, &revision)
               : -ENOMEM;
      if (ret < 0)
        goto out;
    }
  else if (!strcmp(topic, "companion.run"))
    {
      double expected = ny_companion_number(data, "revision");
      if (!isfinite(expected) || floor(expected) != expected ||
          expected != revision)
        {
          ret = expected == revision ? -EINVAL : -ESTALE;
          goto out;
        }
      ny_companion_roll_day(root, now);
      if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enabled")) ||
          now < NY_COMPANION_CLOCK_MIN || ny_companion_quiet(root, now) ||
          ny_companion_number(root, "daily_count") >=
              ny_companion_number(root, "daily_limit") ||
          ny_companion_text(root, "pending_run")[0])
        {
          ret = -EAGAIN;
          goto out;
        }
      if (!ny_companion_set_number(root, "next_at", now))
        {
          ret = -ENOMEM;
          goto out;
        }
      ret = ny_product_store_write("companion", root, revision, &revision);
      if (ret < 0)
        goto out;
    }
  *result = ny_companion_public(root, revision, now);
  if (!*result)
    ret = -ENOMEM;
out:
  cJSON_Delete(root);
  nxmutex_unlock(&g_companion_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_companion_tick
 ****************************************************************************/

int ny_product_companion_tick(void)
{
  uint64_t mono = ny_product_time_ms(true);
  if (mono < g_companion_tick)
    return 0;
  g_companion_tick = mono + NY_COMPANION_TICK_MS;
  int ret = nxmutex_lock(&g_companion_lock);
  if (ret < 0)
    return ret;
  cJSON *root = NULL;
  cJSON *agent_result = NULL;
  uint64_t revision;
  uint64_t now = ny_product_time_ms(false);
  ret = ny_companion_load(&root, &revision);
  if (ret < 0 || now < NY_COMPANION_CLOCK_MIN)
    goto out;
  bool rolled = ny_companion_roll_day(root, now);
  const char *pending = ny_companion_text(root, "pending_run");
  const char *owner = ny_companion_text(root, "owner");
  struct ny_product_caller_s agent_caller = { owner, NY_PRODUCT_OWNER, false };
  if (pending[0])
    {
      cJSON *query = cJSON_CreateObject();
      if (!query || !cJSON_AddStringToObject(query, "id", pending))
        {
          cJSON_Delete(query);
          ret = -ENOMEM;
          goto out;
        }
      ret = ny_agent_request(&agent_caller, "agent.run.get", query,
                             &agent_result);
      cJSON_Delete(query);
      if (ret < 0)
        goto save_error;
      const char *state = ny_companion_text(agent_result, "state");
      if (strcmp(state, "succeeded") && strcmp(state, "failed") &&
          strcmp(state, "cancelled"))
        {
          ret = 0;
          goto out;
        }
      if (!strcmp(state, "succeeded"))
        {
          char notice_id[96];
          snprintf(notice_id, sizeof(notice_id), "companion:%s", pending);
          ret = ny_product_notification_post(
              notice_id, "companion", ny_companion_text(agent_result, "reply"),
              now + 86400000);
          if (ret < 0)
            goto save_error;
        }
      bool ok =
          ny_companion_set_string(root, "pending_run", "") &&
          ny_companion_set_number(root, "last_error",
                                  !strcmp(state, "succeeded") ? 0 : -EIO) &&
          ny_companion_set_number(root, "next_at",
                                  now + (uint64_t)ny_companion_number(
                                            root, "minimum_interval_minutes") *
                                            60000);
      ret = ok ? ny_product_store_write("companion", root, revision, &revision)
               : -ENOMEM;
      goto out;
    }
  if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enabled")) ||
      ny_companion_quiet(root, now) ||
      ny_companion_number(root, "daily_count") >=
          ny_companion_number(root, "daily_limit") ||
      ny_companion_number(root, "next_at") > now || !owner[0])
    {
      ret = rolled ? ny_product_store_write("companion", root, revision,
                                            &revision)
                   : 0;
      goto out;
    }
  char request_id[64];
  snprintf(request_id, sizeof(request_id), "companion-%" PRIu64 "-%" PRIu64,
           revision, now / 1000);
  cJSON *request = cJSON_CreateObject();
  const char *mode = ny_companion_text(root, "mode");
  char prompt[768];
  snprintf(prompt, sizeof(prompt),
           "Send one brief, gentle proactive companion message in Chinese. "
           "Mode: %s. "
           "Use weather, alarms, tasks, calendar, or user-saved memory only "
           "when the "
           "tools provide it. Never claim to see the user, infer mood, "
           "diagnose health, "
           "or invent a change. Do not ask more than one question.",
           mode);
  bool valid = request &&
               cJSON_AddStringToObject(request, "requestId", request_id) &&
               cJSON_AddStringToObject(request, "conversationId",
                                       "proactive-companion") &&
               cJSON_AddStringToObject(request, "text", prompt);
  ret = valid ? ny_agent_request(&agent_caller, "agent.chat", request,
                                 &agent_result)
              : -ENOMEM;
  cJSON_Delete(request);
  if (ret < 0)
    goto save_error;
  valid =
      ny_companion_set_string(root, "pending_run",
                              ny_companion_text(agent_result, "id")) &&
      ny_companion_set_number(root, "daily_count",
                              ny_companion_number(root, "daily_count") + 1) &&
      ny_companion_set_number(root, "last_at", now) &&
      ny_companion_set_number(root, "next_at",
                              now + (uint64_t)ny_companion_number(
                                        root, "minimum_interval_minutes") *
                                        60000) &&
      ny_companion_set_number(root, "last_error", 0);
  ret = valid ? ny_product_store_write("companion", root, revision, &revision)
              : -ENOMEM;
  goto out;
save_error:
  ny_companion_set_number(root, "last_error", ret);
  ny_companion_set_number(root, "next_at", now + 60000);
  ny_product_store_write("companion", root, revision, &revision);
out:
  cJSON_Delete(agent_result);
  cJSON_Delete(root);
  nxmutex_unlock(&g_companion_lock);
  return ret == -EAGAIN || ret == -EBUSY || ret == -ENODATA ? 0 : ret;
}
