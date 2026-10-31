/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_automation.c
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
#include "ny_product_store.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <string.h>

#define NY_AUTO_LIMIT    16
#define NY_AUTO_MIN_TIME 1577836800000ULL
#define NY_AUTO_MAX_TIME 4102444800000ULL

static mutex_t g_auto_lock = NXMUTEX_INITIALIZER;
static bool g_auto_initialized;
static uint64_t g_auto_next_tick;

static const char *ny_auto_text(const cJSON *row, const char *key);
static double ny_auto_number(const cJSON *row, const char *key);
static bool ny_auto_string(cJSON *row, const char *key, const char *value);
static bool ny_auto_num(cJSON *row, const char *key, double value);
static bool ny_auto_bool(cJSON *row, const char *key, bool value);
static bool ny_auto_id(const char *id);
static cJSON *ny_auto_find(cJSON *root, const char *id);
static int ny_auto_load(cJSON **root, uint64_t *revision);
static int ny_auto_reserve(cJSON *row, uint64_t revision, uint64_t now);
static int ny_auto_edit(cJSON *root, const struct ny_product_caller_s *caller,
                        const char *topic, const cJSON *data,
                        uint64_t revision);
static int ny_auto_dispatch(cJSON *root, cJSON *row, uint64_t revision);

/****************************************************************************
 * Name: ny_auto_text
 ****************************************************************************/

static const char *ny_auto_text(const cJSON *row, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

/****************************************************************************
 * Name: ny_auto_number
 ****************************************************************************/

static double ny_auto_number(const cJSON *row, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);
  return cJSON_IsNumber(value) && isfinite(value->valuedouble)
             ? value->valuedouble
             : -1;
}

/****************************************************************************
 * Name: ny_auto_string
 ****************************************************************************/

static bool ny_auto_string(cJSON *row, const char *key, const char *value)
{
  cJSON_DeleteItemFromObjectCaseSensitive(row, key);
  return cJSON_AddStringToObject(row, key, value) != NULL;
}

/****************************************************************************
 * Name: ny_auto_num
 ****************************************************************************/

static bool ny_auto_num(cJSON *row, const char *key, double value)
{
  cJSON_DeleteItemFromObjectCaseSensitive(row, key);
  return cJSON_AddNumberToObject(row, key, value) != NULL;
}

/****************************************************************************
 * Name: ny_auto_bool
 ****************************************************************************/

static bool ny_auto_bool(cJSON *row, const char *key, bool value)
{
  cJSON_DeleteItemFromObjectCaseSensitive(row, key);
  return cJSON_AddBoolToObject(row, key, value) != NULL;
}

/****************************************************************************
 * Name: ny_auto_id
 ****************************************************************************/

static bool ny_auto_id(const char *id)
{
  if (!id[0] || strlen(id) > 24)
    return false;
  for (const char *p = id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
          *p == '_'))
      return false;
  return true;
}

/****************************************************************************
 * Name: ny_auto_find
 ****************************************************************************/

static cJSON *ny_auto_find(cJSON *root, const char *id)
{
  cJSON *row;
  cJSON_ArrayForEach(row,
                     cJSON_GetObjectItemCaseSensitive(
                         root, "items")) if (strcmp(ny_auto_text(row, "id"),
                                                    id) == 0) return row;
  return NULL;
}

/****************************************************************************
 * Name: ny_auto_load
 ****************************************************************************/

static int ny_auto_load(cJSON **root, uint64_t *revision)
{
  int ret = ny_product_store_read("automations", root, revision);
  if (ret < 0)
    return ret;
  if (*root == NULL)
    {
      *root = cJSON_CreateObject();
      if (*root == NULL || !cJSON_AddArrayToObject(*root, "items"))
        return -ENOMEM;
    }
  return cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(*root, "items"))
             ? 0
             : -EBADMSG;
}

/****************************************************************************
 * Name: ny_auto_reserve
 ****************************************************************************/

static int ny_auto_reserve(cJSON *row, uint64_t revision, uint64_t now)
{
  char request[64];
  int count = snprintf(request, sizeof(request), "auto-%s-%" PRIu64,
                       ny_auto_text(row, "id"), revision + 1);
  if (count < 0 || count >= sizeof(request))
    return -E2BIG;
  bool valid = ny_auto_string(row, "requestId", request);
  valid &= ny_auto_bool(row, "pending", true);
  valid &= ny_auto_string(row, "state", "pending");
  valid &= ny_auto_num(row, "lastTriggeredAt", now);
  valid &= ny_auto_num(row, "error", 0);
  if (strcmp(ny_auto_text(row, "kind"), "at") == 0)
    {
      valid &= ny_auto_bool(row, "enabled", false);
      valid &= ny_auto_num(row, "nextAt", 0);
    }
  else
    valid &= ny_auto_num(row, "nextAt",
                         now + ny_auto_number(row, "intervalSeconds") * 1000);
  return valid ? 0 : -ENOMEM;
}

/****************************************************************************
 * Name: ny_auto_edit
 ****************************************************************************/

static int ny_auto_edit(cJSON *root, const struct ny_product_caller_s *caller,
                        const char *topic, const cJSON *data,
                        uint64_t revision)
{
  const char *id = ny_auto_text(data, "id");
  if (!ny_auto_id(id))
    return -EINVAL;
  cJSON *row = ny_auto_find(root, id);
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
  uint64_t now = ny_product_time_ms(false);
  if (strcmp(topic, "agent.automation.delete") == 0)
    {
      if (row == NULL)
        return -ENOENT;
      cJSON_Delete(cJSON_DetachItemViaPointer(items, row));
      return 0;
    }
  if (strcmp(topic, "agent.automation.run") == 0)
    {
      if (row == NULL)
        return -ENOENT;
      if (!ny_product_clock_valid() || now > NY_AUTO_MAX_TIME)
        return -ENODATA;
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "pending")))
        return -EBUSY;
      return ny_auto_reserve(row, revision, now);
    }
  if (strcmp(topic, "agent.automation.enable") == 0)
    {
      if (row == NULL)
        return -ENOENT;
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
      if (!cJSON_IsBool(enabled))
        return -EINVAL;
      bool active = cJSON_IsTrue(enabled);
      if (active && (!ny_product_clock_valid() || now > NY_AUTO_MAX_TIME))
        return -ENODATA;
      double next = 0;
      if (active)
        {
          next = strcmp(ny_auto_text(row, "kind"), "at") == 0
                     ? ny_auto_number(row, "atMs")
                     : now + ny_auto_number(row, "intervalSeconds") * 1000;
          if (next <= now)
            return -EINVAL;
        }
      bool valid = ny_auto_bool(row, "enabled", active);
      valid &= ny_auto_bool(row, "pending", false);
      valid &= ny_auto_num(row, "nextAt", next);
      valid &= ny_auto_num(row, "error", 0);
      valid &= ny_auto_string(row, "state", active ? "scheduled" : "paused");
      return valid ? 0 : -ENOMEM;
    }
  if (strcmp(topic, "agent.automation.save") != 0)
    return -ENOSYS;
  const char *title = ny_auto_text(data, "title");
  const char *prompt = ny_auto_text(data, "prompt");
  const char *kind = ny_auto_text(data, "kind");
  bool once = strcmp(kind, "at") == 0;
  double interval = ny_auto_number(data, "intervalSeconds");
  double at = ny_auto_number(data, "atMs");
  if (!title[0] || strlen(title) > 96 || !prompt[0] || strlen(prompt) > 1024 ||
      (!once && strcmp(kind, "every") != 0 && strcmp(kind, "heartbeat") != 0))
    return -EINVAL;
  if (once
          ? at < NY_AUTO_MIN_TIME || at > NY_AUTO_MAX_TIME || floor(at) != at
          : interval < 30 || interval > 2592000 || floor(interval) != interval)
    return -EINVAL;
  if (row == NULL)
    {
      if (cJSON_GetArraySize(items) >= NY_AUTO_LIMIT)
        return -ENOSPC;
      row = cJSON_CreateObject();
      if (row == NULL)
        return -ENOMEM;
      if (!cJSON_AddItemToArray(items, row))
        {
          cJSON_Delete(row);
          return -ENOMEM;
        }
      if (!ny_auto_string(row, "id", id))
        return -ENOMEM;
    }
  bool valid = ny_auto_string(row, "title", title);
  valid &= ny_auto_string(row, "prompt", prompt);
  valid &= ny_auto_string(row, "kind", kind);
  valid &= ny_auto_string(row, "owner", caller->id);
  valid &= ny_auto_num(row, "intervalSeconds", once ? 0 : interval);
  valid &= ny_auto_num(row, "atMs", once ? at : 0);
  valid &= ny_auto_num(row, "nextAt", 0);
  valid &= ny_auto_bool(row, "enabled", false);
  valid &= ny_auto_bool(row, "pending", false);
  valid &= ny_auto_string(row, "state", "paused");
  valid &= ny_auto_num(row, "error", 0);
  return valid ? 0 : -ENOMEM;
}

/****************************************************************************
 * Name: ny_agent_automation
 ****************************************************************************/

int ny_agent_automation(const struct ny_product_caller_s *caller,
                        const char *topic, const cJSON *data, cJSON **result)
{
  *result = NULL;
  if (strcmp(topic, "agent.automation.list") != 0 &&
      strcmp(topic, "agent.automation.save") != 0 &&
      strcmp(topic, "agent.automation.enable") != 0 &&
      strcmp(topic, "agent.automation.delete") != 0 &&
      strcmp(topic, "agent.automation.run") != 0)
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  int ret = ny_product_start();
  if (ret < 0)
    return ret;
  nxmutex_lock(&g_auto_lock);
  cJSON *root = NULL;
  uint64_t revision = 0;
  ret = ny_auto_load(&root, &revision);
  if (ret == 0 && strcmp(topic, "agent.automation.list") != 0)
    {
      double expected = ny_auto_number(data, "revision");
      if (expected < 0 || floor(expected) != expected)
        ret = -EINVAL;
      else if (expected != (double)revision)
        ret = -ESTALE;
      else
        ret = ny_auto_edit(root, caller, topic, data, revision);
      if (ret == 0)
        ret = ny_product_store_write("automations", root, revision, &revision);
    }
  if (ret == 0 && !cJSON_AddNumberToObject(root, "revision", revision))
    ret = -ENOMEM;
  if (ret == 0)
    *result = root;
  else
    cJSON_Delete(root);
  nxmutex_unlock(&g_auto_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_auto_dispatch
 ****************************************************************************/

static int ny_auto_dispatch(cJSON *root, cJSON *row, uint64_t revision)
{
  cJSON *request = cJSON_CreateObject();
  cJSON *result = NULL;
  char conversation[40];
  char prompt[1280];
  snprintf(conversation, sizeof(conversation), "automation-%s",
           ny_auto_text(row, "id"));
  snprintf(
      prompt, sizeof(prompt), "%s%s",
      strcmp(ny_auto_text(row, "kind"), "heartbeat") == 0
          ? "Proactive check. If nothing actionable changed, reply exactly "
            "HEARTBEAT_OK. Do not fabricate changes.\n"
          : "",
      ny_auto_text(row, "prompt"));
  bool valid = request != NULL;
  valid &= cJSON_AddStringToObject(request, "requestId",
                                   ny_auto_text(row, "requestId")) != NULL;
  valid &=
      cJSON_AddStringToObject(request, "conversationId", conversation) != NULL;
  valid &= cJSON_AddStringToObject(request, "text", prompt) != NULL;
  struct ny_product_caller_s caller = { ny_auto_text(row, "owner"),
                                        NY_PRODUCT_OWNER, false };
  int ret = valid ? ny_agent_request(&caller, "agent.chat", request, &result)
                  : -ENOMEM;
  cJSON_Delete(request);
  if ((ret == -EAGAIN || ret == -EBUSY || ret == -ENODATA || ret == -ENOMEM ||
       ret == -EIO) &&
      ny_auto_number(row, "error") == ret)
    {
      cJSON_Delete(result);
      return 0;
    }
  valid = ny_auto_num(row, "error", ret);
  if (ret == 0)
    {
      valid &= ny_auto_bool(row, "pending", false);
      valid &= ny_auto_string(row, "lastRun", ny_auto_text(result, "id"));
      valid &= ny_auto_string(row, "state", "submitted");
    }
  else if (ret == -ENOSPC || ret == -E2BIG || ret == -EALREADY ||
           ret == -EEXIST || ret == -EINVAL)
    {
      valid &= ny_auto_bool(row, "pending", false);
      valid &= ny_auto_bool(row, "enabled", false);
      valid &= ny_auto_string(row, "state", "blocked");
    }
  cJSON_Delete(result);
  /* The reservation survives uncertain submit outcomes. Retrying its exact
   * request ID cannot duplicate a persisted execution or a deleted request.
   */
  return valid
             ? ny_product_store_write("automations", root, revision, &revision)
             : -ENOMEM;
}

/****************************************************************************
 * Name: ny_agent_automation_tick
 ****************************************************************************/

int ny_agent_automation_tick(void)
{
  uint64_t monotonic = ny_product_time_ms(true);
  nxmutex_lock(&g_auto_lock);
  if (monotonic < g_auto_next_tick)
    {
      nxmutex_unlock(&g_auto_lock);
      return 0;
    }
  g_auto_next_tick = monotonic + 1000;
  uint64_t now = ny_product_time_ms(false);
  if (!ny_product_clock_valid() || now > NY_AUTO_MAX_TIME)
    {
      nxmutex_unlock(&g_auto_lock);
      return 0;
    }
  cJSON *root = NULL;
  uint64_t revision;
  int ret = ny_auto_load(&root, &revision);
  bool changed = false;
  cJSON *selected = NULL;
  cJSON *row;
  if (ret == 0)
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(root, "items"))
    {
      bool pending =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "pending"));
      bool enabled =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "enabled"));
      double next = ny_auto_number(row, "nextAt");
      if (!g_auto_initialized && !pending && enabled && next <= now)
        {
          bool once = strcmp(ny_auto_text(row, "kind"), "at") == 0;
          bool valid = ny_auto_bool(row, "enabled", !once);
          valid &= ny_auto_num(
              row, "nextAt",
              once ? 0 : now + ny_auto_number(row, "intervalSeconds") * 1000);
          valid &= ny_auto_string(row, "state", "missed");
          if (!valid)
            {
              ret = -ENOMEM;
              break;
            }
          changed = true;
          continue;
        }
      if (pending || (enabled && next > 0 && next <= now))
        {
          if (selected == NULL || pending ||
              ny_auto_number(selected, "nextAt") > next)
            selected = row;
          if (pending)
            break;
        }
    }
  if (ret == 0 && changed)
    ret = ny_product_store_write("automations", root, revision, &revision);
  if (ret == 0)
    g_auto_initialized = true;
  if (ret == 0 && selected != NULL)
    {
      if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(selected, "pending")))
        {
          ret = ny_auto_reserve(selected, revision, now);
          if (ret == 0)
            ret = ny_product_store_write("automations", root, revision,
                                         &revision);
        }
      if (ret == 0)
        ret = ny_auto_dispatch(root, selected, revision);
    }
  cJSON_Delete(root);
  nxmutex_unlock(&g_auto_lock);
  return ret;
}
