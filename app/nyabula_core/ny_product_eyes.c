/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_eyes.c
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
#include <errno.h>
#include <nuttx/config.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_NYABULA_CORE_EYE
#include <inttypes.h>
#include <nyabula_eye_service.h>
#include <nyabula_eye_wire.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define NY_PRODUCT_EYE_SOURCE     "nyabot"
#define NY_PRODUCT_EYE_PRIORITY   30
#define NY_PRODUCT_EYE_CONFIRM_MS 1000
static atomic_uint g_eye_request;
#endif

/****************************************************************************
 * Name: ny_product_eyes_request
 ****************************************************************************/

int ny_product_eyes_request(const struct ny_product_caller_s *caller,
                            const char *topic, const cJSON *data,
                            cJSON **result)
{
  bool expression = strcmp(topic, "eyes.expression") == 0;
  if (!expression && strcmp(topic, "eyes.status") != 0)
    return -ENOSYS;
#ifndef CONFIG_NYABULA_CORE_EYE
  (void)caller;
  (void)data;
  (void)result;
  return -ENODEV;
#else
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  struct nyabula_core_snapshot_s snapshot;
  int ret = nyabula_eye_service_snapshot(&snapshot);
  if (ret < 0)
    return ret;
  if (expression)
    {
      char id[48];
      snprintf(id, sizeof(id), "agent-eye-%u",
               atomic_fetch_add(&g_eye_request, 1));
      cJSON *command = cJSON_CreateObject();
      cJSON *params = cJSON_Duplicate(data, true);
      if (command == NULL || params == NULL ||
          !cJSON_AddItemToObject(command, "params", params))
        {
          cJSON_Delete(command);
          cJSON_Delete(params);
          return -ENOMEM;
        }
      bool valid = cJSON_AddStringToObject(command, "action", topic) != NULL;
      valid &= cJSON_AddStringToObject(command, "id", id) != NULL;
      valid &= cJSON_AddStringToObject(command, "source",
                                       NY_PRODUCT_EYE_SOURCE) != NULL;
      valid &= cJSON_AddNumberToObject(command, "priority",
                                       NY_PRODUCT_EYE_PRIORITY) != NULL;
      valid &= cJSON_AddNumberToObject(command, "lease_ms",
                                       NYABULA_EYE_DEFAULT_LEASE_MS) != NULL;
      char *json = valid ? cJSON_PrintUnformatted(command) : NULL;
      cJSON_Delete(command);
      if (json == NULL)
        return -ENOMEM;
      ret = nyabula_eye_service_submit(NY_PRODUCT_EYE_SOURCE, json,
                                       strlen(json));
      free(json);
      if (ret < 0)
        return ret;
      uint64_t deadline = ny_product_time_ms(true) + NY_PRODUCT_EYE_CONFIRM_MS;
      do
        {
          ret = nyabula_eye_service_snapshot(&snapshot);
          if (ret < 0)
            return ret;
          if (!strcmp(snapshot.last_request_id, id))
            {
              if (snapshot.last_status < 0)
                return snapshot.last_status;
              break;
            }
          if (ny_product_time_ms(true) >= deadline)
            return -ETIMEDOUT;
          usleep(10000);
        }
      while (true);
    }
  *result = nyabula_eye_wire_snapshot(&snapshot);
  return *result == NULL ? -ENOMEM : 0;
#endif
}
