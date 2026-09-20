/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_timers.c
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
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <string.h>

#define NY_TIMER_LIMIT     16
#define NY_TIMER_LAP_LIMIT 32
#define NY_TIMER_MAX_MS    604800000.0

static mutex_t g_timer_lock = NXMUTEX_INITIALIZER;
static cJSON *g_timers;
static uint64_t g_timer_revision;
static uint32_t g_timer_clock_step;

static bool ny_timer_set(cJSON *row, const char *key, double value);
static bool ny_timer_string(cJSON *row, const char *key, const char *value);
static double ny_timer_number(const cJSON *row, const char *key);
static const char *ny_timer_text(const cJSON *row, const char *key);
static int ny_timer_load(void);
static int ny_timer_save(cJSON **candidate);
static void ny_timer_project(cJSON *row, uint64_t now);
static cJSON *ny_timer_result(void);

/****************************************************************************
 * Name: ny_timer_set
 ****************************************************************************/

static bool ny_timer_set(cJSON *row, const char *key, double value)
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
 * Name: ny_timer_string
 ****************************************************************************/

static bool ny_timer_string(cJSON *row, const char *key, const char *value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(row, key);
  return cJSON_IsString(item)
             ? cJSON_SetValuestring(item, value) != NULL
             : cJSON_AddStringToObject(row, key, value) != NULL;
}

/****************************************************************************
 * Name: ny_timer_number
 ****************************************************************************/

static double ny_timer_number(const cJSON *row, const char *key)
{
  return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, key));
}

/****************************************************************************
 * Name: ny_timer_text
 ****************************************************************************/

static const char *ny_timer_text(const cJSON *row, const char *key)
{
  const char *text =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
  return text == NULL ? "" : text;
}

/****************************************************************************
 * Name: ny_timer_load
 ****************************************************************************/

static int ny_timer_load(void)
{
  cJSON *items = NULL;
  cJSON *row;
  uint64_t revision = 0;
  uint64_t mono = ny_product_time_ms(true);
  uint64_t wall = ny_product_time_ms(false);

  /* An anchor is a reading of the wall clock, stored so that a timer can
   * be picked up after a restart.  It is only worth anything if the clock
   * could be believed when it was taken -- one from before this firmware
   * was built was not -- and if it can be believed now.
   */

  bool clock_valid = ny_product_clock_valid();
  double floor_ms = (double)ny_product_clock_floor_ms();
  int ret;
  if (g_timers != NULL)
    {
      return 0;
    }

  ret = ny_product_store_read("timers", &items, &revision);
  if (ret < 0)
    {
      return ret;
    }

  if (items == NULL)
    {
      items = cJSON_CreateArray();
    }

  if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > NY_TIMER_LIMIT)
    {
      cJSON_Delete(items);
      return -EBADMSG;
    }

  cJSON_ArrayForEach(row, items)
  {
    const char *kind = ny_timer_text(row, "kind");
    const char *status = ny_timer_text(row, "status");
    const char *id = ny_timer_text(row, "id");
    const cJSON *laps = cJSON_GetObjectItemCaseSensitive(row, "laps");
    const char *fields[] = {
      "duration_ms", "remaining_ms", "elapsed_ms",
      "due_at",      "anchor_at",    "base_elapsed_ms"
    };
    if (!cJSON_IsObject(row) || id[0] == '\0' || strlen(id) > 31 ||
        strlen(ny_timer_text(row, "label")) > 96 ||
        (strcmp(kind, "countdown") != 0 && strcmp(kind, "stopwatch") != 0 &&
         strcmp(kind, "sleep") != 0) ||
        (strcmp(status, "running") != 0 && strcmp(status, "paused") != 0 &&
         strcmp(status, "finished") != 0 &&
         strcmp(status, "waiting-clock") != 0) ||
        !cJSON_IsArray(laps) || cJSON_GetArraySize(laps) > NY_TIMER_LAP_LIMIT)
      {
        ret = -EBADMSG;
        goto fail;
      }

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      {
        double value = ny_timer_number(row, fields[i]);
        if (!isfinite(value) || value < 0 || value > 9007199254740991.0)
          {
            ret = -EBADMSG;
            goto fail;
          }
      }

    if (strcmp(status, "running") == 0 || strcmp(status, "waiting-clock") == 0)
      {
        bool stopwatch = strcmp(kind, "stopwatch") == 0;
        double anchor =
            ny_timer_number(row, stopwatch ? "anchor_at" : "due_at");
        if (clock_valid && anchor >= floor_ms)
          {
            if (stopwatch)
              {
                double elapsed = ny_timer_number(row, "elapsed_ms") +
                                 fmax(0, (double)wall - anchor);
                if (!ny_timer_set(row, "base_elapsed_ms", elapsed) ||
                    !ny_timer_set(row, "anchor_mono_ms", mono))
                  {
                    ret = -ENOMEM;
                    goto fail;
                  }
              }
            else if (!ny_timer_set(row, "deadline_mono_ms",
                                   mono + fmax(0, anchor - wall)))
              {
                ret = -ENOMEM;
                goto fail;
              }
          }
        else
          {
            if (!ny_timer_string(row, "status",
                                 anchor >= floor_ms ? "waiting-clock"
                                                    : "paused") ||
                !ny_timer_string(row, "recovery", "clock-unavailable"))
              {
                ret = -ENOMEM;
                goto fail;
              }
          }
      }
  }

  g_timers = items;
  g_timer_revision = revision;
  return 0;
fail:
  cJSON_Delete(items);
  return ret;
}

/****************************************************************************
 * Name: ny_timer_project
 ****************************************************************************/

static void ny_timer_project(cJSON *row, uint64_t now)
{
  if (strcmp(ny_timer_text(row, "status"), "running") != 0)
    {
      return;
    }

  if (strcmp(ny_timer_text(row, "kind"), "stopwatch") == 0)
    {
      ny_timer_set(
          row, "elapsed_ms",
          ny_timer_number(row, "base_elapsed_ms") +
              fmax(0, (double)now - ny_timer_number(row, "anchor_mono_ms")));
    }
  else
    {
      ny_timer_set(row, "remaining_ms",
                   fmax(0, ny_timer_number(row, "deadline_mono_ms") - now));
    }
}

/****************************************************************************
 * Name: ny_timer_save
 ****************************************************************************/

static int ny_timer_save(cJSON **candidate)
{
  cJSON *row;
  uint64_t revision;
  uint64_t mono = ny_product_time_ms(true);
  uint64_t wall = ny_product_time_ms(false);
  bool clock_valid = ny_product_clock_valid();
  cJSON_ArrayForEach(row, *candidate)
  {
    ny_timer_project(row, mono);
    if (strcmp(ny_timer_text(row, "status"), "running") == 0)
      {
        if (!ny_timer_set(row, "due_at",
                          clock_valid
                              ? wall + ny_timer_number(row, "remaining_ms")
                              : 0) ||
            !ny_timer_set(row, "anchor_at", clock_valid ? wall : 0))
          {
            return -ENOMEM;
          }
      }
  }

  int ret = ny_product_store_write("timers", *candidate, g_timer_revision,
                                   &revision);
  if (ret == 0)
    {
      cJSON_Delete(g_timers);
      g_timers = *candidate;
      *candidate = NULL;
      g_timer_revision = revision;
    }

  return ret;
}

/****************************************************************************
 * Name: ny_timer_result
 ****************************************************************************/

static cJSON *ny_timer_result(void)
{
  cJSON *result = cJSON_CreateObject();
  cJSON *items = cJSON_Duplicate(g_timers, true);
  cJSON *row;
  uint64_t mono = ny_product_time_ms(true);
  if (result == NULL || items == NULL ||
      !cJSON_AddNumberToObject(result, "revision", g_timer_revision) ||
      !cJSON_AddNumberToObject(result, "uptime_ms", mono) ||
      !cJSON_AddItemToObject(result, "items", items))
    {
      cJSON_Delete(result);
      cJSON_Delete(items);
      return NULL;
    }

  cJSON_ArrayForEach(row, items)
  {
    ny_timer_project(row, mono);
    cJSON_DeleteItemFromObjectCaseSensitive(row, "deadline_mono_ms");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "anchor_mono_ms");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "base_elapsed_ms");
  }

  return result;
}

/****************************************************************************
 * Name: ny_product_timers_request
 ****************************************************************************/

int ny_product_timers_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result)
{
  const char *operation;
  cJSON *candidate = NULL;
  cJSON *row = NULL;
  uint64_t mono = ny_product_time_ms(true);
  const char *id;
  int index = -1;
  int ret;
  if (strncmp(topic, "timer.", 6) != 0)
    {
      return -ENOSYS;
    }

  if (caller->role < NY_PRODUCT_FAMILY)
    {
      return -EACCES;
    }

  operation = topic + 6;
  if (strcmp(operation, "list") != 0 && strcmp(operation, "create") != 0 &&
      strcmp(operation, "pause") != 0 && strcmp(operation, "resume") != 0 &&
      strcmp(operation, "delete") != 0 && strcmp(operation, "lap") != 0)
    {
      return -ENOSYS;
    }

  ret = nxmutex_lock(&g_timer_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_timer_load();
  if (ret < 0)
    {
      goto out;
    }

  if (strcmp(operation, "list") == 0)
    {
      *result = ny_timer_result();
      ret = *result == NULL ? -ENOMEM : 0;
      goto out;
    }

  double expected = ny_timer_number(data, "revision");
  if (!isfinite(expected) || expected < 0 || floor(expected) != expected ||
      expected > 9007199254740991.0)
    {
      ret = -EINVAL;
      goto out;
    }

  if ((uint64_t)expected != g_timer_revision)
    {
      ret = -ESTALE;
      goto out;
    }

  candidate = cJSON_Duplicate(g_timers, true);
  if (candidate == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (strcmp(operation, "create") == 0)
    {
      const char *kind = ny_timer_text(data, "kind");
      const char *label = ny_timer_text(data, "label");
      double duration = ny_timer_number(data, "duration_ms");
      char generated[32];
      if ((strcmp(kind, "countdown") != 0 && strcmp(kind, "stopwatch") != 0 &&
           strcmp(kind, "sleep") != 0) ||
          strlen(label) > 96 ||
          (strcmp(kind, "stopwatch") != 0 &&
           (!isfinite(duration) || duration < 1 ||
            duration > NY_TIMER_MAX_MS || floor(duration) != duration)))
        {
          ret = -EINVAL;
          goto out;
        }

      if (cJSON_GetArraySize(candidate) >= NY_TIMER_LIMIT)
        {
          ret = -ENOSPC;
          goto out;
        }

      if (strcmp(kind, "stopwatch") == 0)
        {
          duration = 0;
        }

      snprintf(generated, sizeof(generated), "timer-%llu",
               (unsigned long long)(g_timer_revision + 1));
      row = cJSON_CreateObject();
      if (row == NULL || !cJSON_AddItemToArray(candidate, row))
        {
          cJSON_Delete(row);
          ret = -ENOMEM;
          goto out;
        }

      if (!ny_timer_string(row, "id", generated) ||
          !ny_timer_string(row, "kind", kind) ||
          !ny_timer_string(row, "label", label) ||
          !ny_timer_string(row, "source", caller->id) ||
          !ny_timer_string(row, "status", "running") ||
          !ny_timer_set(row, "duration_ms", duration) ||
          !ny_timer_set(row, "remaining_ms", duration) ||
          !ny_timer_set(row, "elapsed_ms", 0) ||
          !ny_timer_set(row, "base_elapsed_ms", 0) ||
          !ny_timer_set(row, "deadline_mono_ms", mono + duration) ||
          !ny_timer_set(row, "anchor_mono_ms", mono) ||
          !ny_timer_set(row, "due_at", 0) ||
          !ny_timer_set(row, "anchor_at", 0) ||
          !cJSON_AddArrayToObject(row, "laps"))
        {
          ret = -ENOMEM;
          goto out;
        }
    }
  else
    {
      id = ny_timer_text(data, "id");
      for (int i = 0; i < cJSON_GetArraySize(candidate); i++)
        {
          cJSON *item = cJSON_GetArrayItem(candidate, i);
          if (strcmp(ny_timer_text(item, "id"), id) == 0)
            {
              row = item;
              index = i;
              break;
            }
        }

      if (index < 0)
        {
          ret = -ENOENT;
          goto out;
        }

      ny_timer_project(row, mono);
      bool running = strcmp(ny_timer_text(row, "status"), "running") == 0;
      if (strcmp(operation, "delete") == 0)
        {
          cJSON_DeleteItemFromArray(candidate, index);
        }
      else if (strcmp(operation, "pause") == 0)
        {
          if (!running)
            {
              ret = -EINVAL;
              goto out;
            }

          if (!ny_timer_string(row, "status", "paused"))
            {
              ret = -ENOMEM;
              goto out;
            }
        }
      else if (strcmp(operation, "resume") == 0)
        {
          if (strcmp(ny_timer_text(row, "status"), "paused") != 0)
            {
              ret = -EINVAL;
              goto out;
            }

          if (!ny_timer_string(row, "status", "running") ||
              !ny_timer_set(row, "anchor_mono_ms", mono) ||
              !ny_timer_set(row, "base_elapsed_ms",
                            ny_timer_number(row, "elapsed_ms")) ||
              !ny_timer_set(row, "deadline_mono_ms",
                            mono + ny_timer_number(row, "remaining_ms")))
            {
              ret = -ENOMEM;
              goto out;
            }

          cJSON_DeleteItemFromObjectCaseSensitive(row, "recovery");
        }
      else
        {
          cJSON *laps = cJSON_GetObjectItemCaseSensitive(row, "laps");
          if (!running || strcmp(ny_timer_text(row, "kind"), "stopwatch") != 0)
            {
              ret = -EINVAL;
              goto out;
            }

          if (cJSON_GetArraySize(laps) >= NY_TIMER_LAP_LIMIT)
            {
              ret = -ENOSPC;
              goto out;
            }

          cJSON *lap = cJSON_CreateNumber(ny_timer_number(row, "elapsed_ms"));
          if (lap == NULL || !cJSON_AddItemToArray(laps, lap))
            {
              cJSON_Delete(lap);
              ret = -ENOMEM;
              goto out;
            }
        }
    }

  ret = ny_timer_save(&candidate);
  if (ret == 0)
    {
      *result = ny_timer_result();
      ret = *result == NULL ? -ENOMEM : 0;
    }

out:
  cJSON_Delete(candidate);
  nxmutex_unlock(&g_timer_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_timers_tick
 ****************************************************************************/

int ny_product_timers_tick(void)
{
  cJSON *candidate = NULL;
  cJSON *row;
  uint64_t now = ny_product_time_ms(true);
  uint64_t wall = ny_product_time_ms(false);
  bool clock_valid = ny_product_clock_valid();
  uint32_t step = ny_product_clock_step(NULL, NULL);
  bool due = false;
  bool asleep = false;
  bool chime = false;
  int ret = nxmutex_lock(&g_timer_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_timer_load();
  if (ret < 0)
    {
      goto out;
    }

  cJSON_ArrayForEach(row, g_timers)
  {
    if (strcmp(ny_timer_text(row, "status"), "waiting-clock") == 0 &&
        clock_valid)
      {
        due = true;
        break;
      }

    /* A running timer counts on the monotonic clock and does not care that
     * the wall clock was moved, but the anchors it was stored with were
     * taken from the old one.  Saving takes them again.
     */

    if (strcmp(ny_timer_text(row, "status"), "running") == 0 &&
        step != g_timer_clock_step)
      {
        due = true;
        break;
      }

    if (strcmp(ny_timer_text(row, "status"), "running") == 0 &&
        strcmp(ny_timer_text(row, "kind"), "stopwatch") != 0 &&
        ny_timer_number(row, "deadline_mono_ms") <= now)
      {
        due = true;
        break;
      }
  }

  if (!due)
    {
      g_timer_clock_step = step;
      goto out;
    }

  candidate = cJSON_Duplicate(g_timers, true);
  if (candidate == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  cJSON_ArrayForEach(row, candidate)
  {
    if (strcmp(ny_timer_text(row, "status"), "waiting-clock") == 0 &&
        clock_valid)
      {
        bool stopwatch = strcmp(ny_timer_text(row, "kind"), "stopwatch") == 0;
        double anchor =
            ny_timer_number(row, stopwatch ? "anchor_at" : "due_at");
        if (!ny_timer_string(row, "status", "running") ||
            !ny_timer_set(row, "anchor_mono_ms", now) ||
            !ny_timer_set(
                row, "base_elapsed_ms",
                ny_timer_number(row, "elapsed_ms") +
                    (stopwatch ? fmax(0, (double)wall - anchor) : 0)) ||
            !ny_timer_set(row, "deadline_mono_ms",
                          now + fmax(0, anchor - wall)))
          {
            ret = -ENOMEM;
            goto out;
          }

        cJSON_DeleteItemFromObjectCaseSensitive(row, "recovery");
      }

    if (strcmp(ny_timer_text(row, "status"), "running") == 0 &&
        strcmp(ny_timer_text(row, "kind"), "stopwatch") != 0 &&
        ny_timer_number(row, "deadline_mono_ms") <= now)
      {
        if (!ny_timer_string(row, "status", "finished") ||
            !ny_timer_set(row, "remaining_ms", 0) ||
            !ny_timer_set(row, "completed_at", ny_product_time_ms(false)))
          {
            ret = -ENOMEM;
            goto out;
          }

        if (strcmp(ny_timer_text(row, "kind"), "sleep") == 0)
          {
            asleep = true;
          }
        else
          {
            chime = true;
          }
      }
  }

  ret = ny_timer_save(&candidate);
  if (ret == 0)
    {
      g_timer_clock_step = step;
    }

  /* Act only on a transition that reached storage: a finish that was not
   * saved is found again by the next tick, and would then sound twice.
   */

  if (ret == 0 && asleep)
    {
      ny_product_media_sleep();
    }

  if (ret == 0 && chime)
    {
      ny_product_media_alert(NY_PRODUCT_MEDIA_ALERT_ONCE);
    }

out:
  cJSON_Delete(candidate);
  nxmutex_unlock(&g_timer_lock);
  return ret;
}
