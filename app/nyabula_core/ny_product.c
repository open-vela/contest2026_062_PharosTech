/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product.c
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
#ifdef CONFIG_NYABULA_CORE_AGENT
#include "ny_agent.h"
#endif
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#define NY_PRODUCT_TIME_MIN 1577836800000.0
#define NY_PRODUCT_TIME_MAX 4102444800000.0

static int ny_product_system(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result);

/****************************************************************************
 * Name: ny_product_time_ms
 ****************************************************************************/

uint64_t ny_product_time_ms(bool monotonic)
{
  struct timespec now;
  clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: ny_product_system
 ****************************************************************************/

static int ny_product_system(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result)
{
  bool set_time = strcmp(topic, "system.time.set") == 0;
  if (!set_time && strcmp(topic, "system.time.get") != 0 &&
      strcmp(topic, "sys.info") != 0)
    {
      return -ENOSYS;
    }

  if (set_time)
    {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, "unix_ms");
      struct timespec now;
      if (caller->role != NY_PRODUCT_OWNER)
        {
          return -EACCES;
        }

      if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
          value->valuedouble < NY_PRODUCT_TIME_MIN ||
          value->valuedouble > NY_PRODUCT_TIME_MAX ||
          floor(value->valuedouble) != value->valuedouble)
        {
          return -EINVAL;
        }

      now.tv_sec = (time_t)(value->valuedouble / 1000);
      now.tv_nsec = (long)((uint64_t)value->valuedouble % 1000) * 1000000;
      if (clock_settime(CLOCK_REALTIME, &now) < 0)
        {
          return -errno;
        }
    }

  cJSON *root = cJSON_CreateObject();
  uint64_t wall = ny_product_time_ms(false);
  if (root == NULL)
    {
      return -ENOMEM;
    }

  if (!cJSON_AddNumberToObject(root, "unix_ms", wall) ||
      !cJSON_AddNumberToObject(root, "uptime_ms", ny_product_time_ms(true)) ||
      !cJSON_AddBoolToObject(root, "clock_valid", wall >= NY_PRODUCT_TIME_MIN))
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  if (strcmp(topic, "sys.info") == 0)
    {
      struct utsname system;
      struct mallinfo heap = mallinfo();
      cJSON *device = cJSON_AddObjectToObject(root, "device");
      cJSON *memory = cJSON_AddObjectToObject(root, "memory");
      if (uname(&system) < 0 || device == NULL || memory == NULL ||
          !cJSON_AddStringToObject(device, "name", "Nyabula") ||
          !cJSON_AddStringToObject(device, "coreVersion", "product-v1") ||
          !cJSON_AddStringToObject(device, "os", system.sysname) ||
          !cJSON_AddStringToObject(device, "arch", system.machine) ||
          !cJSON_AddNumberToObject(root, "uptime",
                                   ny_product_time_ms(true) / 1000) ||
          !cJSON_AddNumberToObject(memory, "total", heap.arena) ||
          !cJSON_AddNumberToObject(memory, "used", heap.uordblks) ||
          !cJSON_AddNumberToObject(memory, "free", heap.fordblks))
        {
          cJSON_Delete(root);
          return -ENOMEM;
        }
    }

  *result = root;
  return 0;
}

/****************************************************************************
 * Name: ny_product_request
 ****************************************************************************/

int ny_product_request(const struct ny_product_caller_s *caller,
                       const char *topic, const cJSON *data, cJSON **result)
{
  int ret;
  if (result == NULL)
    {
      return -EINVAL;
    }

  *result = NULL;
  if (caller == NULL || caller->id == NULL || caller->id[0] == '\0' ||
      caller->role < NY_PRODUCT_GUEST || caller->role > NY_PRODUCT_OWNER ||
      strlen(caller->id) > 63 || topic == NULL || strlen(topic) > 64 ||
      !cJSON_IsObject(data))
    {
      return -EINVAL;
    }

  if (strcmp(topic, "product.status") == 0 ||
      strcmp(topic, "product.start") == 0 ||
      strcmp(topic, "product.stop") == 0)
    {
      if (caller->role != NY_PRODUCT_OWNER)
        {
          return -EACCES;
        }

      ret = strcmp(topic, "product.start") == 0  ? ny_product_start()
            : strcmp(topic, "product.stop") == 0 ? ny_product_stop()
                                                 : 0;
      if (ret < 0)
        {
          return ret;
        }

      *result = ny_product_runtime_status();
      return *result == NULL ? -ENOMEM : 0;
    }

  ret = ny_product_system(caller, topic, data, result);
  if (ret != -ENOSYS)
    {
      return ret;
    }

  ret = ny_product_eyes_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;

  ret = ny_product_device_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;

  ret = ny_product_media_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;

#ifdef CONFIG_NYABULA_CORE_AGENT
  ret = ny_agent_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    {
      return ret;
    }
#endif

  ret = ny_product_records_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    {
      return ret;
    }

#ifdef CONFIG_NYABULA_CORE_WEATHER
  ret = ny_product_weather_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;
#endif
#ifdef CONFIG_NYABULA_CORE_AGENT
  ret = ny_product_companion_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;
#endif
  ret = ny_product_briefing_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;
  ret = ny_product_notifications_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;
  ret = ny_product_alarms_request(caller, topic, data, result);
  if (ret != -ENOSYS)
    return ret;
  return ny_product_timers_request(caller, topic, data, result);
}
