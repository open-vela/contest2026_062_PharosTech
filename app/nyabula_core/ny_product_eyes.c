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

/* The iris colour the owner chose outlives a reboot.  The engine keeps it
 * only in memory and anybody may change it -- the panel, the agent, a
 * plugin -- so the record follows the snapshot instead of any one of them.
 */

#include "ny_product_store.h"

#define NY_PRODUCT_IRIS_DOMAIN    "eyes"
#define NY_PRODUCT_IRIS_SOURCE    "nyiris"
#define NY_PRODUCT_IRIS_PERIOD_MS 2000
#define NY_PRODUCT_IRIS_DEFAULT   0x56ffb2

static uint64_t g_iris_next;
static uint64_t g_iris_revision;
static uint32_t g_iris_saved[NYABULA_EYE_COUNT];
static uint32_t g_iris_seen[NYABULA_EYE_COUNT];
static bool g_iris_restored;
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

#ifdef CONFIG_NYABULA_CORE_EYE
/****************************************************************************
 * Name: ny_product_iris_submit
 ****************************************************************************/

static int ny_product_iris_submit(const char *eyes, uint32_t rgb)
{
  char json[192];
  int length = snprintf(json, sizeof(json),
                        "{\"source\":\"" NY_PRODUCT_IRIS_SOURCE "\","
                        "\"id\":\"iris-%s\",\"action\":\"eyes.iris\","
                        "\"priority\":%d,\"lease_ms\":0,"
                        "\"params\":{\"eyes\":\"%s\",\"rgb\":\"#%06" PRIx32
                        "\"}}",
                        eyes, NY_PRODUCT_EYE_PRIORITY, eyes, rgb & 0xffffff);

  if (length < 0 || (size_t)length >= sizeof(json))
    {
      return -EOVERFLOW;
    }

  return nyabula_eye_service_submit(NY_PRODUCT_IRIS_SOURCE, json, length);
}
#endif

/****************************************************************************
 * Name: ny_product_eyes_tick
 *
 * Description:
 *   Put the remembered iris colour back once the engine is up, then write
 *   down a new one after it has stayed the same for a whole period: a colour
 *   picker being dragged passes over hundreds.
 *
 ****************************************************************************/

int ny_product_eyes_tick(void)
{
#ifdef CONFIG_NYABULA_CORE_EYE
  struct nyabula_core_snapshot_s snapshot;
  uint64_t now = ny_product_time_ms(true);
  int eye;

  if (now < g_iris_next)
    {
      return 0;
    }

  g_iris_next = now + NY_PRODUCT_IRIS_PERIOD_MS;
  if (nyabula_eye_service_snapshot(&snapshot) < 0)
    {
      return 0; /* The engine is not up yet. */
    }

  if (!g_iris_restored)
    {
      cJSON *root = NULL;
      int ret = ny_product_store_read(NY_PRODUCT_IRIS_DOMAIN, &root,
                                      &g_iris_revision);

      if (ret < 0 && ret != -ENOENT && ret != -EBADMSG)
        {
          return 0; /* The store is not there yet either. */
        }

      for (eye = 0; eye < NYABULA_EYE_COUNT; eye++)
        {
          const cJSON *item = cJSON_GetObjectItemCaseSensitive(
              root, eye == 0 ? "left" : "right");

          g_iris_saved[eye] = cJSON_IsNumber(item) && item->valuedouble >= 0 &&
                                      item->valuedouble <= 0xffffff
                                  ? (uint32_t)item->valuedouble
                                  : NY_PRODUCT_IRIS_DEFAULT;
          g_iris_seen[eye] = g_iris_saved[eye];
          if (g_iris_saved[eye] != snapshot.iris_rgb[eye])
            {
              ny_product_iris_submit(eye == 0 ? "left" : "right",
                                     g_iris_saved[eye]);
            }
        }

      cJSON_Delete(root);
      g_iris_restored = true;
      return 0;
    }

  if (memcmp(snapshot.iris_rgb, g_iris_seen, sizeof(g_iris_seen)) != 0)
    {
      memcpy(g_iris_seen, snapshot.iris_rgb, sizeof(g_iris_seen));
      return 0; /* Still moving: look again in a period. */
    }

  if (memcmp(g_iris_seen, g_iris_saved, sizeof(g_iris_saved)) != 0)
    {
      cJSON *root = cJSON_CreateObject();

      if (root != NULL &&
          cJSON_AddNumberToObject(root, "left", g_iris_seen[0]) != NULL &&
          cJSON_AddNumberToObject(root, "right", g_iris_seen[1]) != NULL &&
          ny_product_store_write(NY_PRODUCT_IRIS_DOMAIN, root, g_iris_revision,
                                 &g_iris_revision) == 0)
        {
          memcpy(g_iris_saved, g_iris_seen, sizeof(g_iris_saved));
        }

      cJSON_Delete(root);
    }
#endif

  return 0;
}
