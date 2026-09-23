/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_records.c
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NY_PRODUCT_RECORDS_MAX   128
#define NY_PRODUCT_RECORD_ID_MAX 32

static int ny_product_records_shape(const char *domain, cJSON *record);
static bool ny_product_records_text(const cJSON *record, const char *name,
                                    size_t maximum, bool required);
static bool ny_product_records_number(const cJSON *record, const char *name,
                                      double minimum, double maximum,
                                      bool required);
static double ny_product_records_now(void);

/****************************************************************************
 * Name: ny_product_records_now
 ****************************************************************************/

static double ny_product_records_now(void)
{
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (double)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: ny_product_records_text
 ****************************************************************************/

static bool ny_product_records_text(const cJSON *record, const char *name,
                                    size_t maximum, bool required)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(record, name);
  return item == NULL
             ? !required
             : cJSON_IsString(item) && strlen(item->valuestring) <= maximum &&
                   (!required || item->valuestring[0] != '\0');
}

/****************************************************************************
 * Name: ny_product_records_number
 ****************************************************************************/

static bool ny_product_records_number(const cJSON *record, const char *name,
                                      double minimum, double maximum,
                                      bool required)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(record, name);
  return item == NULL ? !required
                      : cJSON_IsNumber(item) && isfinite(item->valuedouble) &&
                            item->valuedouble >= minimum &&
                            item->valuedouble <= maximum;
}

/****************************************************************************
 * Name: ny_product_records_shape
 ****************************************************************************/

static int ny_product_records_shape(const char *domain, cJSON *record)
{
  const cJSON *item;
  const char *allowed;
  char wrapped[48];
  if (!cJSON_IsObject(record))
    {
      return -EINVAL;
    }

  if (strcmp(domain, "memory") == 0)
    {
      allowed = "|id|text|tag|at|updated_at|source|confidence|";
      if (!ny_product_records_text(record, "text", 2048, true) ||
          !ny_product_records_text(record, "tag", 32, false) ||
          !ny_product_records_number(record, "confidence", 0, 1, false))
        {
          return -EINVAL;
        }
    }
  else if (strcmp(domain, "tasks") == 0)
    {
      allowed = "|id|title|state|progress|at|updated_at|source|";
      const char *state = cJSON_GetStringValue(
          cJSON_GetObjectItemCaseSensitive(record, "state"));
      if (!ny_product_records_text(record, "title", 160, true) ||
          !ny_product_records_number(record, "progress", 0, 100, false) ||
          state == NULL ||
          (strcmp(state, "queued") != 0 && strcmp(state, "running") != 0 &&
           strcmp(state, "confirm") != 0 && strcmp(state, "done") != 0 &&
           strcmp(state, "failed") != 0 && strcmp(state, "cancelled") != 0))
        {
          return -EINVAL;
        }
    }
  else if (strcmp(domain, "calendar") == 0)
    {
      allowed = "|id|title|detail|start_at|end_at|remind_before_ms|at|updated_"
                "at|source|";
      if (!ny_product_records_text(record, "title", 160, true) ||
          !ny_product_records_text(record, "detail", 1024, false) ||
          !ny_product_records_number(record, "start_at", 0, 4102444800000.0,
                                     true) ||
          !ny_product_records_number(record, "end_at", 0, 4102444800000.0,
                                     false) ||
          !ny_product_records_number(record, "remind_before_ms", 0, 604800000,
                                     false))
        {
          return -EINVAL;
        }

      const cJSON *start =
          cJSON_GetObjectItemCaseSensitive(record, "start_at");
      const cJSON *end = cJSON_GetObjectItemCaseSensitive(record, "end_at");
      if (end != NULL && end->valuedouble < start->valuedouble)
        {
          return -EINVAL;
        }
    }
  else
    {
      return -ENOSYS;
    }

  cJSON_ArrayForEach(item, record)
  {
    if (item->string == NULL || strlen(item->string) > 32 ||
        strchr(item->string, '|') != NULL)
      {
        return -EINVAL;
      }

    snprintf(wrapped, sizeof(wrapped), "|%s|", item->string);
    if (strstr(allowed, wrapped) == NULL)
      {
        return -EINVAL;
      }

    for (const cJSON *next = item->next; next != NULL; next = next->next)
      {
        if (next->string != NULL && strcmp(item->string, next->string) == 0)
          {
            return -EINVAL;
          }
      }
  }

  return 0;
}

/****************************************************************************
 * Name: ny_product_records_request
 ****************************************************************************/

int ny_product_records_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result)
{
  const char *domain;
  const char *operation;
  const char *id;
  cJSON *items = NULL;
  cJSON *record = NULL;
  cJSON *response = NULL;
  uint64_t revision = 0;
  uint64_t updated = 0;
  int found = -1;
  int ret;

  if (result == NULL)
    {
      return -EINVAL;
    }

  *result = NULL;
  if (caller == NULL || caller->id == NULL || caller->id[0] == '\0' ||
      strlen(caller->id) > 63 || topic == NULL || !cJSON_IsObject(data))
    {
      return -EINVAL;
    }

  if (strncmp(topic, "memory.", 7) == 0)
    {
      domain = "memory";
      operation = topic + 7;
    }
  else if (strncmp(topic, "task.", 5) == 0)
    {
      domain = "tasks";
      operation = topic + 5;
    }
  else if (strncmp(topic, "calendar.", 9) == 0)
    {
      domain = "calendar";
      operation = topic + 9;
    }
  else
    {
      return -ENOSYS;
    }

  if (caller->role <
      (strcmp(domain, "memory") == 0 ? NY_PRODUCT_OWNER : NY_PRODUCT_FAMILY))
    {
      return -EACCES;
    }

  if (strcmp(operation, "list") != 0 && strcmp(operation, "create") != 0 &&
      strcmp(operation, "update") != 0 && strcmp(operation, "delete") != 0)
    {
      return -ENOSYS;
    }

  ret = ny_product_store_read(domain, &items, &revision);
  if (ret < 0)
    {
      return ret;
    }

  if (items == NULL)
    {
      items = cJSON_CreateArray();
    }

  if (items == NULL)
    {
      return -ENOMEM;
    }

  if (!cJSON_IsArray(items) ||
      cJSON_GetArraySize(items) > NY_PRODUCT_RECORDS_MAX)
    {
      ret = -EBADMSG;
      goto out;
    }

  response = cJSON_CreateObject();
  if (response == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (strcmp(operation, "list") == 0)
    {
      if (!cJSON_AddNumberToObject(response, "revision", revision) ||
          !cJSON_AddItemToObject(response, "items", items))
        {
          ret = -ENOMEM;
          goto out;
        }

      items = NULL;
      *result = response;
      return 0;
    }

  const cJSON *expected = cJSON_GetObjectItemCaseSensitive(data, "revision");
  if (!cJSON_IsNumber(expected) || !isfinite(expected->valuedouble) ||
      expected->valuedouble < 0 ||
      expected->valuedouble > 9007199254740991.0 ||
      floor(expected->valuedouble) != expected->valuedouble)
    {
      ret = -EINVAL;
      goto out;
    }

  if ((uint64_t)expected->valuedouble != revision)
    {
      ret = -ESTALE;
      goto out;
    }

  id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "id"));
  if (strcmp(operation, "create") != 0)
    {
      if (id == NULL || id[0] == '\0' ||
          strlen(id) >= NY_PRODUCT_RECORD_ID_MAX)
        {
          ret = -EINVAL;
          goto out;
        }

      for (int i = 0; i < cJSON_GetArraySize(items); i++)
        {
          const char *existing =
              cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                  cJSON_GetArrayItem(items, i), "id"));
          if (existing != NULL && strcmp(existing, id) == 0)
            {
              found = i;
              break;
            }
        }

      if (found < 0)
        {
          ret = -ENOENT;
          goto out;
        }
    }

  if (strcmp(operation, "delete") == 0)
    {
      cJSON_DeleteItemFromArray(items, found);
    }
  else
    {
      const cJSON *input = cJSON_GetObjectItemCaseSensitive(data, "record");
      record = cJSON_Duplicate(input, true);
      if (record == NULL)
        {
          ret = input == NULL ? -EINVAL : -ENOMEM;
          goto out;
        }

      ret = ny_product_records_shape(domain, record);
      if (ret < 0)
        {
          goto out;
        }

      /* Server-controlled metadata is never accepted from the client. */

      cJSON_DeleteItemFromObjectCaseSensitive(record, "id");
      cJSON_DeleteItemFromObjectCaseSensitive(record, "source");
      cJSON_DeleteItemFromObjectCaseSensitive(record, "at");
      cJSON_DeleteItemFromObjectCaseSensitive(record, "updated_at");
      char created_id[NY_PRODUCT_RECORD_ID_MAX];
      double created_at = ny_product_records_now();
      if (found < 0)
        {
          if (cJSON_GetArraySize(items) >= NY_PRODUCT_RECORDS_MAX)
            {
              ret = -ENOSPC;
              goto out;
            }

          snprintf(created_id, sizeof(created_id), "%c%llu", domain[0],
                   (unsigned long long)(revision + 1));
          id = created_id;
        }
      else
        {
          const cJSON *old = cJSON_GetArrayItem(items, found);
          const cJSON *at = cJSON_GetObjectItemCaseSensitive(old, "at");
          if (cJSON_IsNumber(at))
            {
              created_at = at->valuedouble;
            }
        }

      if (!cJSON_AddStringToObject(record, "id", id) ||
          !cJSON_AddStringToObject(record, "source", caller->id) ||
          !cJSON_AddNumberToObject(record, "at", created_at) ||
          !cJSON_AddNumberToObject(record, "updated_at",
                                   ny_product_records_now()))
        {
          ret = -ENOMEM;
          goto out;
        }

      bool attached = found < 0
                          ? cJSON_AddItemToArray(items, record)
                          : cJSON_ReplaceItemInArray(items, found, record);
      if (!attached)
        {
          ret = -ENOMEM;
          goto out;
        }

      record = NULL;
    }

  ret = ny_product_store_write(domain, items, revision, &updated);
  if (ret == 0)
    {
      if (!cJSON_AddNumberToObject(response, "revision", updated) ||
          !cJSON_AddItemToObject(response, "items", items))
        {
          ret = -ENOMEM;
          goto out;
        }

      items = NULL;
      *result = response;
      response = NULL;
    }

out:
  cJSON_Delete(record);
  cJSON_Delete(items);
  cJSON_Delete(response);
  return ret;
}
