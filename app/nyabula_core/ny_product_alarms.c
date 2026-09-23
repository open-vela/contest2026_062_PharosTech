/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_alarms.c
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

#define NY_ALARM_LIMIT    32
#define NY_ALARM_DAY_MS   86400000LL
#define NY_ALARM_GRACE_MS 300000ULL

/* How long an alarm rings before it gives up and marks itself missed.
 * Ringing is durable state that survives a reboot, and a one-shot alarm
 * switches itself off the moment it fires: without this an alarm nobody
 * dismissed kept the chime and the caption on the eyes for good, and the
 * panel showed the row as "off", so there was nothing obvious to press.
 */

#define NY_ALARM_RING_MS 600000ULL

/* An occurrence is never scheduled more than a week ahead, so a next_at
 * within this distance of a reading of the clock was worked out from that
 * reading.
 */

#define NY_ALARM_FRAME_MS (8 * NY_ALARM_DAY_MS)

static mutex_t g_alarm_lock = NXMUTEX_INITIALIZER;
static cJSON *g_alarms;
static uint64_t g_alarm_revision;
static bool g_alarm_sounding;
static uint32_t g_alarm_clock_step;
#ifdef CONFIG_NYABULA_CORE_EYE
static uint64_t g_alarm_eye_refresh;
#endif
static const char *g_alarm_days[] = { "sun", "mon", "tue", "wed",
                                      "thu", "fri", "sat" };

static double ny_alarm_number(const cJSON *row, const char *key);
static const char *ny_alarm_text(const cJSON *row, const char *key);
static bool ny_alarm_set(cJSON *row, const char *key, double value);
static bool ny_alarm_status(cJSON *row, const char *value);
static bool ny_alarm_valid(const cJSON *row);
static uint64_t ny_alarm_next(const cJSON *row, uint64_t after);
static bool ny_alarm_stale(uint64_t due, bool moved, uint64_t before,
                           uint64_t now);
static int ny_alarm_load(void);
static int ny_alarm_save(cJSON **candidate);
static cJSON *ny_alarm_result(void);

/****************************************************************************
 * Name: ny_alarm_number
 ****************************************************************************/

static double ny_alarm_number(const cJSON *row, const char *key)
{
  return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, key));
}

/****************************************************************************
 * Name: ny_alarm_text
 ****************************************************************************/

static const char *ny_alarm_text(const cJSON *row, const char *key)
{
  const char *value =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
  return value ? value : "";
}

/****************************************************************************
 * Name: ny_alarm_set
 ****************************************************************************/

static bool ny_alarm_set(cJSON *row, const char *key, double value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(row, key);
  if (cJSON_IsNumber(item))
    {
      cJSON_SetNumberValue(item, value);
      return true;
    }
  return cJSON_AddNumberToObject(row, key, value) != NULL;
}

/****************************************************************************
 * Name: ny_alarm_status
 ****************************************************************************/

static bool ny_alarm_status(cJSON *row, const char *value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(row, "status");
  return item ? cJSON_SetValuestring(item, value) != NULL
              : cJSON_AddStringToObject(row, "status", value) != NULL;
}

/****************************************************************************
 * Name: ny_alarm_valid
 ****************************************************************************/

static bool ny_alarm_valid(const cJSON *row)
{
  const char *time = ny_alarm_text(row, "time");
  double offset = ny_alarm_number(row, "utc_offset_minutes");
  const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(row, "repeat");
  const cJSON *day;
  unsigned seen = 0;
  if (!cJSON_IsObject(row) || strlen(time) != 5 || time[2] != ':' ||
      time[0] < '0' || time[0] > '2' || time[1] < '0' || time[1] > '9' ||
      (time[0] == '2' && time[1] > '3') || time[3] < '0' || time[3] > '5' ||
      time[4] < '0' || time[4] > '9' ||
      strlen(ny_alarm_text(row, "label")) > 96 ||
      !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(row, "label")) ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(row, "enabled")) ||
      !isfinite(offset) || floor(offset) != offset || offset < -720 ||
      offset > 840 || !cJSON_IsArray(repeat) || cJSON_GetArraySize(repeat) > 7)
    return false;
  cJSON_ArrayForEach(day, repeat)
  {
    const char *text = cJSON_GetStringValue(day);
    int index;
    for (index = 0; index < 7; index++)
      if (text && !strcmp(text, g_alarm_days[index]))
        break;
    if (index == 7 || (seen & (1u << index)))
      return false;
    seen |= 1u << index;
  }
  return true;
}

/****************************************************************************
 * Name: ny_alarm_next
 * Description: Fixed UTC offset; strictly later than the supplied instant.
 ****************************************************************************/

static uint64_t ny_alarm_next(const cJSON *row, uint64_t after)
{
  if (!ny_product_clock_valid() ||
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "enabled")))
    return 0;
  const char *time = ny_alarm_text(row, "time");
  int64_t offset = (int64_t)ny_alarm_number(row, "utc_offset_minutes") * 60000;
  int64_t date = ((int64_t)after + offset) / NY_ALARM_DAY_MS;
  int minutes = ((time[0] - '0') * 10 + time[1] - '0') * 60 +
                (time[3] - '0') * 10 + time[4] - '0';
  const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(row, "repeat");
  const cJSON *day;
  for (int i = 0; i <= 7; i++)
    {
      int64_t candidate =
          (date + i) * NY_ALARM_DAY_MS + minutes * 60000LL - offset;
      bool matches = cJSON_GetArraySize(repeat) == 0;
      cJSON_ArrayForEach(day,
                         repeat) if (!strcmp(day->valuestring,
                                             g_alarm_days[(date + i + 4) % 7]))
          matches = true;
      if (matches && candidate > (int64_t)after)
        return candidate;
    }
  return 0;
}

/****************************************************************************
 * Name: ny_alarm_stale
 *
 * Description:
 *   Whether an occurrence was worked out from a clock that has since turned
 *   out to be wrong.  When the clock is moved by years -- a device that
 *   counted on from 2021 is told the real date -- everything scheduled
 *   before the move is in the past, and taking that at face value would
 *   report an alarm set an hour ago as missed and switch a one-off alarm
 *   off without it ever having had its day.  Such an occurrence is worked
 *   out again instead.  One that was scheduled against a correct clock in
 *   an earlier boot is nowhere near the wrong reading, and is left to be
 *   rung or reported as missed on its merits.
 *
 ****************************************************************************/

static bool ny_alarm_stale(uint64_t due, bool moved, uint64_t before,
                           uint64_t now)
{
  uint64_t jump = now > before ? now - before : before - now;
  uint64_t apart = due > before ? due - before : before - due;
  return moved && due != 0 && jump > NY_ALARM_FRAME_MS &&
         apart <= NY_ALARM_FRAME_MS;
}

/****************************************************************************
 * Name: ny_alarm_load
 ****************************************************************************/

static int ny_alarm_load(void)
{
  if (g_alarms)
    return 0;
  cJSON *items = NULL;
  uint64_t revision;
  int ret = ny_product_store_read("alarms", &items, &revision);
  if (ret < 0)
    return ret;
  if (!items)
    items = cJSON_CreateArray();
  cJSON *row;
  if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > NY_ALARM_LIMIT)
    goto invalid;
  cJSON_ArrayForEach(row, items)
  {
    const char *status = ny_alarm_text(row, "status");
    double due = ny_alarm_number(row, "next_at");
    if (!ny_alarm_valid(row) || !ny_alarm_text(row, "id")[0] ||
        !isfinite(due) || due < 0 || due > 9007199254740991.0 ||
        (strcmp(status, "scheduled") && strcmp(status, "ringing") &&
         strcmp(status, "dismissed") && strcmp(status, "snoozed") &&
         strcmp(status, "missed")))
      goto invalid;
  }
  g_alarms = items;
  g_alarm_revision = revision;
  return 0;
invalid:
  cJSON_Delete(items);
  return -EBADMSG;
}

/****************************************************************************
 * Name: ny_alarm_save
 ****************************************************************************/

static int ny_alarm_save(cJSON **candidate)
{
  uint64_t revision;
  int ret = ny_product_store_write("alarms", *candidate, g_alarm_revision,
                                   &revision);
  if (!ret)
    {
      cJSON_Delete(g_alarms);
      g_alarms = *candidate;
      *candidate = NULL;
      g_alarm_revision = revision;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_alarm_result
 ****************************************************************************/

static cJSON *ny_alarm_result(void)
{
  cJSON *result = cJSON_CreateObject();
  cJSON *items = cJSON_Duplicate(g_alarms, true);
  if (!result || !items ||
      !cJSON_AddNumberToObject(result, "revision", g_alarm_revision) ||
      !cJSON_AddBoolToObject(result, "clock_valid",
                             ny_product_clock_valid()) ||
      !cJSON_AddItemToObject(result, "items", items))
    {
      cJSON_Delete(result);
      cJSON_Delete(items);
      return NULL;
    }
  return result;
}

/****************************************************************************
 * Name: ny_product_alarms_request
 ****************************************************************************/

int ny_product_alarms_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result)
{
  if (strncmp(topic, "alarm.", 6))
    return -ENOSYS;
  if (caller->role < NY_PRODUCT_FAMILY)
    return -EACCES;
  const char *op = topic + 6;
  if (strcmp(op, "list") && strcmp(op, "create") && strcmp(op, "update") &&
      strcmp(op, "delete") && strcmp(op, "dismiss") && strcmp(op, "snooze"))
    return -ENOSYS;
  int ret = nxmutex_lock(&g_alarm_lock);
  if (ret < 0)
    return ret;
  cJSON *candidate = NULL;
  ret = ny_alarm_load();
  if (ret < 0)
    goto out;
  if (!strcmp(op, "list"))
    goto result;
  double revision = ny_alarm_number(data, "revision");
  if (!isfinite(revision) || revision < 0 || floor(revision) != revision ||
      revision > 9007199254740991.0)
    {
      ret = -EINVAL;
      goto out;
    }
  if (revision != g_alarm_revision)
    {
      ret = -ESTALE;
      goto out;
    }
  candidate = cJSON_Duplicate(g_alarms, true);
  if (!candidate)
    {
      ret = -ENOMEM;
      goto out;
    }
  int index = -1;
  cJSON *row = NULL;
  bool create = !strcmp(op, "create");
  if (!create)
    {
      for (int i = 0; i < cJSON_GetArraySize(candidate); i++)
        if (!strcmp(ny_alarm_text(cJSON_GetArrayItem(candidate, i), "id"),
                    ny_alarm_text(data, "id")))
          {
            index = i;
            break;
          }
      if (index < 0)
        {
          ret = -ENOENT;
          goto out;
        }
      row = cJSON_GetArrayItem(candidate, index);
    }
  uint64_t now = ny_product_time_ms(false);
  if (create || !strcmp(op, "update"))
    {
      const cJSON *record = cJSON_GetObjectItemCaseSensitive(data, "record");
      if (!ny_alarm_valid(record))
        {
          ret = -EINVAL;
          goto out;
        }
      if (create && cJSON_GetArraySize(candidate) >= NY_ALARM_LIMIT)
        {
          ret = -ENOSPC;
          goto out;
        }
      char id[40];
      if (create)
        snprintf(id, sizeof(id), "alarm-%llu",
                 (unsigned long long)(g_alarm_revision + 1));
      else
        snprintf(id, sizeof(id), "%s", ny_alarm_text(row, "id"));
      cJSON *fresh = cJSON_CreateObject();
      cJSON *repeat = cJSON_Duplicate(
          cJSON_GetObjectItemCaseSensitive(record, "repeat"), true);
      if (!fresh || !repeat || !cJSON_AddItemToObject(fresh, "repeat", repeat))
        {
          cJSON_Delete(fresh);
          cJSON_Delete(repeat);
          ret = -ENOMEM;
          goto out;
        }
      bool ok =
          cJSON_AddStringToObject(fresh, "id", id) &&
          cJSON_AddStringToObject(fresh, "time",
                                  ny_alarm_text(record, "time")) &&
          cJSON_AddStringToObject(fresh, "label",
                                  ny_alarm_text(record, "label")) &&
          cJSON_AddBoolToObject(fresh, "enabled",
                                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                    record, "enabled"))) &&
          ny_alarm_set(fresh, "utc_offset_minutes",
                       ny_alarm_number(record, "utc_offset_minutes")) &&
          ny_alarm_status(fresh, "scheduled") &&
          ny_alarm_set(fresh, "next_at", ny_alarm_next(fresh, now));
      if (!ok)
        {
          cJSON_Delete(fresh);
          ret = -ENOMEM;
          goto out;
        }
      if (create)
        ok = cJSON_AddItemToArray(candidate, fresh);
      else
        ok = cJSON_ReplaceItemInArray(candidate, index, fresh);
      if (!ok)
        {
          cJSON_Delete(fresh);
          ret = -ENOMEM;
          goto out;
        }
    }
  else if (!strcmp(op, "delete"))
    cJSON_DeleteItemFromArray(candidate, index);
  else
    {
      if (strcmp(ny_alarm_text(row, "status"), "ringing"))
        {
          ret = -EINVAL;
          goto out;
        }
      bool snooze = !strcmp(op, "snooze");
      if (snooze && !ny_product_clock_valid())
        {
          ret = -EAGAIN;
          goto out;
        }
      if (!ny_alarm_status(row, snooze ? "snoozed" : "dismissed") ||
          (snooze && !ny_alarm_set(row, "next_at", now + NY_ALARM_GRACE_MS)))
        {
          ret = -ENOMEM;
          goto out;
        }
    }
  ret = ny_alarm_save(&candidate);
  if (ret < 0)
    goto out;
result:
  *result = ny_alarm_result();
  ret = *result ? 0 : -ENOMEM;
out:
  cJSON_Delete(candidate);
  nxmutex_unlock(&g_alarm_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_alarms_tick
 * Description: Persist occurrence and ringing state in one transaction.
 ****************************************************************************/

int ny_product_alarms_tick(void)
{
  uint64_t now = ny_product_time_ms(false);
  if (!ny_product_clock_valid())
    return 0;
  int ret = nxmutex_lock(&g_alarm_lock);
  if (ret < 0)
    return ret;
  cJSON *candidate = NULL;
  cJSON *row;
  bool changed = false;
  uint64_t before = 0;
  uint32_t step = ny_product_clock_step(&before, NULL);
  bool moved = step != g_alarm_clock_step;
  ret = ny_alarm_load();
  if (ret < 0)
    goto out;
  cJSON_ArrayForEach(row, g_alarms)
  {
    uint64_t due = (uint64_t)ny_alarm_number(row, "next_at");
    if ((cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "enabled")) ||
         !strcmp(ny_alarm_text(row, "status"), "snoozed")) &&
        strcmp(ny_alarm_text(row, "status"), "ringing") &&
        (due <= now || ny_alarm_stale(due, moved, before, now)))
      {
        changed = true;
        break;
      }
  }
  if (!changed)
    {
      g_alarm_clock_step = step;
      goto out;
    }
  candidate = cJSON_Duplicate(g_alarms, true);
  if (!candidate)
    {
      ret = -ENOMEM;
      goto out;
    }
  cJSON_ArrayForEach(row, candidate)
  {
    bool enabled =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "enabled"));
    bool snoozed = !strcmp(ny_alarm_text(row, "status"), "snoozed");
    uint64_t due = (uint64_t)ny_alarm_number(row, "next_at");
    if (!strcmp(ny_alarm_text(row, "status"), "ringing"))
      {
        /* An alarm rings for a while, not for ever.  Nobody came. */

        uint64_t since = (uint64_t)ny_alarm_number(row, "last_fired_at");
        if (since && now > since && now - since >= NY_ALARM_RING_MS &&
            !ny_alarm_status(row, "missed"))
          {
            ret = -ENOMEM;
            goto out;
          }

        continue;
      }

    if (!enabled && !snoozed)
      continue;
    if (ny_alarm_stale(due, moved, before, now))
      {
        /* A snooze is a promise to ring again shortly, and the moment it
         * was made for can no longer be found: ring now.
         */

        if (!ny_alarm_set(row, "next_at",
                          snoozed ? now : ny_alarm_next(row, now)))
          {
            ret = -ENOMEM;
            goto out;
          }
        continue;
      }
    if (due > now)
      continue;
    bool ok = true;
    if (due)
      {
        ok = ny_alarm_set(row, "last_fired_at", due) &&
             ny_alarm_status(row, now - due <= NY_ALARM_GRACE_MS ? "ringing"
                                                                 : "missed");
        if (!cJSON_GetArraySize(
                cJSON_GetObjectItemCaseSensitive(row, "repeat")))
          {
            cJSON *enabled_item =
                cJSON_GetObjectItemCaseSensitive(row, "enabled");
            enabled_item->type = (enabled_item->type & ~0xff) | cJSON_False;
          }
      }
    if (!ok || !ny_alarm_set(row, "next_at", ny_alarm_next(row, now)))
      {
        ret = -ENOMEM;
        goto out;
      }
  }
  ret = ny_alarm_save(&candidate);
  if (!ret)
    g_alarm_clock_step = step;
out:
  cJSON_Delete(candidate);

  /* Sound follows the durable ringing state rather than the transition into
   * it, so an alarm that was ringing when the board restarted rings again,
   * and dismissing or snoozing one -- which only edits the rows -- silences
   * it on the next tick.  Only changes are forwarded: the media service keeps
   * what it was last told.
   */

  if (!ret)
    {
      bool ringing = false;
      cJSON_ArrayForEach(
          row, g_alarms) if (!strcmp(ny_alarm_text(row, "status"), "ringing"))
      {
        ringing = true;
        break;
      }
      if (ringing != g_alarm_sounding)
        {
          g_alarm_sounding = ringing;
          ny_product_media_alert(ringing ? NY_PRODUCT_MEDIA_ALERT_LOOP
                                         : NY_PRODUCT_MEDIA_ALERT_OFF);
        }
    }
#ifdef CONFIG_NYABULA_CORE_EYE
  /* Ringing is durable state; the display is a renewable projection. */
  if (!ret && ny_product_time_ms(true) >= g_alarm_eye_refresh)
    {
      cJSON_ArrayForEach(
          row, g_alarms) if (!strcmp(ny_alarm_text(row, "status"), "ringing"))
      {
        char text[128];
        snprintf(text, sizeof(text), "%s %s", ny_alarm_text(row, "time"),
                 ny_alarm_text(row, "label"));
        nyabula_eye_service_notify("nyalarm", text, strlen(text));
        break;
      }
      g_alarm_eye_refresh = ny_product_time_ms(true) + 4000;
    }
#endif
  nxmutex_unlock(&g_alarm_lock);
  return ret;
}
