/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_store.c
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

#include "ny_product_store.h"
#include "ny_state.h"
#include <errno.h>
#include <math.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NY_PRODUCT_VALUE_LIMIT  24576
#define NY_PRODUCT_REVISION_MAX 9007199254740991ULL
#define NY_PRODUCT_JSON_DEPTH   16

static mutex_t g_product_store_lock = NXMUTEX_INITIALIZER;
static int ny_product_store_key(const char *domain, char *key,
                                size_t capacity);
static int ny_product_store_load(const char *key, cJSON **value,
                                 uint64_t *revision);

/****************************************************************************
 * Name: ny_product_store_key
 ****************************************************************************/

static int ny_product_store_key(const char *domain, char *key, size_t capacity)
{
  static const char *const domains[] = {
    "timers",         "alarms",         "calendar",       "tasks",
    "memory",         "preferences",    "audio",          "privacy",
    "identity",       "pairing",        "agent",          "companion",
    "briefing",       "cloud",          "network",        "devices",
    "home",           "updates",        "skills",         "automations",
    "mcp-out",        "mcp-in",         "agent-profile",  "media",
    "weather-config", "weather",        "notifications",  "weather-now",
    "weather-daily",  "weather-hourly", "weather-alerts", "wifi"
  };

  if (domain == NULL)
    {
      return -EINVAL;
    }

  for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++)
    {
      if (strcmp(domain, domains[i]) == 0)
        {
          int count = snprintf(key, capacity, "product/v1/%s", domain);
          return count >= 0 && (size_t)count < capacity ? 0 : -ENAMETOOLONG;
        }
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: ny_product_json_check
 ****************************************************************************/

int ny_product_json_check(const char *text, size_t length)
{
  unsigned int depth = 0;
  bool quoted = false;
  bool escaped = false;
  if (text == NULL || length == 0 || length > NY_PRODUCT_VALUE_LIMIT)
    {
      return -EINVAL;
    }
  for (size_t i = 0; i < length; i++)
    {
      if (text[i] == '\0')
        {
          return -EBADMSG;
        }

      if (quoted)
        {
          if (escaped)
            {
              escaped = false;
            }
          else if (text[i] == '\\')
            {
              escaped = true;
            }
          else if (text[i] == '"')
            {
              quoted = false;
            }
        }
      else if (text[i] == '"')
        {
          quoted = true;
        }
      else if (text[i] == '{' || text[i] == '[')
        {
          if (++depth > NY_PRODUCT_JSON_DEPTH)
            {
              return -E2BIG;
            }
        }
      else if (text[i] == '}' || text[i] == ']')
        {
          if (depth == 0)
            {
              return -EBADMSG;
            }

          depth--;
        }
    }

  return quoted || depth != 0 ? -EBADMSG : 0;
}

/****************************************************************************
 * Name: ny_product_store_load
 ****************************************************************************/

static int ny_product_store_load(const char *key, cJSON **value,
                                 uint64_t *revision)
{
  char *text = NULL;
  size_t length = 0;
  cJSON *root;
  cJSON *version;
  cJSON *number;
  cJSON *payload;
  int ret = ny_state_read(key, &text, &length, NY_PRODUCT_VALUE_LIMIT);

  *value = NULL;
  *revision = 0;
  if (ret == -ENOENT)
    {
      return 0;
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = ny_product_json_check(text, length);
  if (ret < 0)
    {
      free(text);
      return ret;
    }

  root = cJSON_ParseWithOpts(text, NULL, true);
  free(text);
  version = cJSON_GetObjectItemCaseSensitive(root, "schema");
  number = cJSON_GetObjectItemCaseSensitive(root, "revision");
  payload = cJSON_GetObjectItemCaseSensitive(root, "value");
  if (!cJSON_IsObject(root) || !cJSON_IsNumber(version) ||
      version->valuedouble != 1 || !cJSON_IsNumber(number) ||
      !isfinite(number->valuedouble) || number->valuedouble < 1 ||
      number->valuedouble > NY_PRODUCT_REVISION_MAX ||
      floor(number->valuedouble) != number->valuedouble ||
      (!cJSON_IsObject(payload) && !cJSON_IsArray(payload)))
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }

  *revision = (uint64_t)number->valuedouble;
  *value = cJSON_DetachItemFromObjectCaseSensitive(root, "value");
  cJSON_Delete(root);
  return 0;
}

/****************************************************************************
 * Name: ny_product_store_read
 ****************************************************************************/

int ny_product_store_read(const char *domain, cJSON **value,
                          uint64_t *revision)
{
  char key[64];
  int ret;
  if (value == NULL || revision == NULL)
    {
      return -EINVAL;
    }

  *value = NULL;
  *revision = 0;
  ret = ny_product_store_key(domain, key, sizeof(key));
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&g_product_store_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_product_store_load(key, value, revision);
  nxmutex_unlock(&g_product_store_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_store_write
 ****************************************************************************/

int ny_product_store_write(const char *domain, const cJSON *value,
                           uint64_t expected, uint64_t *revision)
{
  char key[64];
  char *text = NULL;
  cJSON *old = NULL;
  cJSON *root = NULL;
  cJSON *copy = NULL;
  uint64_t current = 0;
  int ret;
  if (revision == NULL || (!cJSON_IsObject(value) && !cJSON_IsArray(value)))
    {
      return -EINVAL;
    }

  *revision = 0;
  ret = ny_product_store_key(domain, key, sizeof(key));
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&g_product_store_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_product_store_load(key, &old, &current);
  cJSON_Delete(old);
  if (ret < 0)
    {
      goto out;
    }

  if (current != expected)
    {
      ret = -ESTALE;
      goto out;
    }

  if (current >= NY_PRODUCT_REVISION_MAX)
    {
      ret = -EOVERFLOW;
      goto out;
    }

  root = cJSON_CreateObject();
  copy = cJSON_Duplicate(value, true);
  if (root == NULL || copy == NULL ||
      !cJSON_AddNumberToObject(root, "schema", 1) ||
      !cJSON_AddNumberToObject(root, "revision", current + 1) ||
      !cJSON_AddItemToObject(root, "value", copy))
    {
      ret = -ENOMEM;
      goto out;
    }

  copy = NULL;
  text = malloc(NY_PRODUCT_VALUE_LIMIT);
  if (text == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (!cJSON_PrintPreallocated(root, text, NY_PRODUCT_VALUE_LIMIT, false))
    {
      ret = -E2BIG;
      goto out;
    }

  ret = ny_product_json_check(text, strlen(text));
  if (ret == 0)
    {
      ret = ny_state_write(key, text, strlen(text));
    }

  if (ret == 0)
    {
      *revision = current + 1;
    }

out:
  free(text);
  cJSON_Delete(copy);
  cJSON_Delete(root);
  nxmutex_unlock(&g_product_store_lock);
  return ret;
}
