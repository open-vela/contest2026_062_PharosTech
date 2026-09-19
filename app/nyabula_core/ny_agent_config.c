/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_config.c
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

#include "agent_config.h"
#include "infra/config_store.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "ny_agent.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ny_agent_config_string(const cJSON *data, const char *key);
static bool ny_agent_config_ascii(const char *value, size_t maximum);
static bool ny_agent_config_endpoint(const cJSON *data);
static int ny_agent_router(const char *topic, const cJSON *data,
                           cJSON **result);

/****************************************************************************
 * Name: ny_agent_config_string
 ****************************************************************************/

static const char *ny_agent_config_string(const cJSON *data, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

/****************************************************************************
 * Name: ny_agent_config_ascii
 ****************************************************************************/

static bool ny_agent_config_ascii(const char *value, size_t maximum)
{
  size_t length = strlen(value);
  if (length == 0 || length > maximum)
    return false;
  for (size_t i = 0; i < length; i++)
    {
      if ((unsigned char)value[i] < 33 || (unsigned char)value[i] > 126)
        return false;
    }
  return true;
}

/****************************************************************************
 * Name: ny_agent_config_endpoint
 ****************************************************************************/

static bool ny_agent_config_endpoint(const cJSON *data)
{
  const char *host = ny_agent_config_string(data, "host");
  const char *path = ny_agent_config_string(data, "path");
  const char *port = ny_agent_config_string(data, "port");
  const char *model = ny_agent_config_string(data, "model");
  const char *key = ny_agent_config_string(data, "key");
  if (!ny_agent_config_ascii(host, 127) || !ny_agent_config_ascii(path, 127) ||
      path[0] != '/' || !ny_agent_config_ascii(port, 5) ||
      !ny_agent_config_ascii(model, 63) ||
      (key[0] && !ny_agent_config_ascii(key, 127)))
    return false;
  for (const char *p = host; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
      return false;
  for (const char *p = port; *p; p++)
    if (*p < '0' || *p > '9')
      return false;
  return atoi(port) >= 1 && atoi(port) <= 65535;
}

/****************************************************************************
 * Name: ny_agent_router
 ****************************************************************************/

static int ny_agent_router(const char *topic, const cJSON *data,
                           cJSON **result)
{
  int ret = 0;
  if (!strcmp(topic, "agent.config.router.profile"))
    {
      const char *profile = ny_agent_config_string(data, "profile");
      int value = !strcmp(profile, "auto")      ? LLM_ROUTE_AUTO
                  : !strcmp(profile, "eco")     ? LLM_ROUTE_ECO
                  : !strcmp(profile, "premium") ? LLM_ROUTE_PREMIUM
                                                : -1;
      ret = value < 0 ? -EINVAL : llm_router_set_profile(value) ? -EIO : 0;
    }
  else if (!strcmp(topic, "agent.config.router.save") ||
           !strcmp(topic, "agent.config.router.delete"))
    {
      const cJSON *index = cJSON_GetObjectItemCaseSensitive(data, "index");
      if (!cJSON_IsNumber(index) || !isfinite(index->valuedouble) ||
          floor(index->valuedouble) != index->valuedouble ||
          index->valuedouble < 0 ||
          index->valuedouble >= LLM_ROUTER_MAX_BACKENDS)
        return -EINVAL;
      int slot = (int)index->valuedouble;
      if (!strcmp(topic, "agent.config.router.delete"))
        ret = llm_router_remove_backend(slot) ? -EIO : 0;
      else
        {
          const cJSON *priority =
              cJSON_GetObjectItemCaseSensitive(data, "priority");
          const cJSON *tier =
              cJSON_GetObjectItemCaseSensitive(data, "cost_tier");
          const cJSON *enabled =
              cJSON_GetObjectItemCaseSensitive(data, "enabled");
          if (!ny_agent_config_endpoint(data) || !cJSON_IsBool(enabled) ||
              !cJSON_IsNumber(priority) || !isfinite(priority->valuedouble) ||
              priority->valuedouble < 0 || priority->valuedouble > 100 ||
              floor(priority->valuedouble) != priority->valuedouble ||
              !cJSON_IsNumber(tier) || !isfinite(tier->valuedouble) ||
              tier->valuedouble < 0 || tier->valuedouble > 3 ||
              floor(tier->valuedouble) != tier->valuedouble)
            return -EINVAL;
          llm_backend_t backend = { 0 };
          llm_router_get_backend(slot, &backend);
          snprintf(backend.host, sizeof(backend.host), "%s",
                   ny_agent_config_string(data, "host"));
          snprintf(backend.path, sizeof(backend.path), "%s",
                   ny_agent_config_string(data, "path"));
          snprintf(backend.port, sizeof(backend.port), "%s",
                   ny_agent_config_string(data, "port"));
          snprintf(backend.model, sizeof(backend.model), "%s",
                   ny_agent_config_string(data, "model"));
          const char *key = ny_agent_config_string(data, "key");
          if (key[0])
            snprintf(backend.api_key, sizeof(backend.api_key), "%s", key);
          backend.priority = (int)priority->valuedouble;
          backend.cost_tier = (int)tier->valuedouble;
          backend.enabled = cJSON_IsTrue(enabled);
          ret = llm_router_set_backend(slot, &backend) ? -EIO : 0;
          memset(&backend, 0, sizeof(backend));
        }
    }
  else if (strcmp(topic, "agent.config.router.get"))
    return -ENOSYS;
  if (ret == 0)
    {
      char *encoded = llm_router_status_json();
      *result = encoded ? cJSON_Parse(encoded) : NULL;
      free(encoded);
      ret = *result ? 0 : -ENOMEM;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_agent_config
 ****************************************************************************/

int ny_agent_config(const struct ny_product_caller_s *caller,
                    const char *topic, const cJSON *data, cJSON **result)
{
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  if (!strncmp(topic, "agent.config.router.", 20))
    return ny_agent_router(topic, data, result);
  if (strcmp(topic, "agent.config.get") != 0 &&
      strcmp(topic, "agent.config.set") != 0)
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  if (strcmp(topic, "agent.config.set") == 0)
    {
      const char *host = ny_agent_config_string(data, "host");
      const char *path = ny_agent_config_string(data, "path");
      const char *port = ny_agent_config_string(data, "port");
      const char *model = ny_agent_config_string(data, "model");
      const char *key = ny_agent_config_string(data, "key");
      if (!ny_agent_config_endpoint(data))
        return -EINVAL;
      if (llm_set_all(host, path, port, key, model) != OK)
        return -EIO;
    }

  static const char *const fields[] = { "host", "path", "port", "model" };
  static const char *const keys[] = { AGENT_CFG_KEY_LLM_HOST,
                                      AGENT_CFG_KEY_LLM_PATH, "llm_port",
                                      AGENT_CFG_KEY_MODEL };
  cJSON *root = cJSON_CreateObject();
  bool valid = root != NULL;
  char value[128];
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
      value[0] = 0;
      claw_config_get(keys[i], value, sizeof(value));
      valid &= cJSON_AddStringToObject(root, fields[i], value) != NULL;
    }
  value[0] = 0;
  bool key_set =
      claw_config_get(AGENT_CFG_KEY_API_KEY, value, sizeof(value)) == OK;
  memset(value, 0, sizeof(value));
  valid &= cJSON_AddBoolToObject(root, "keySet", key_set) != NULL;
  valid &= cJSON_AddBoolToObject(root, "canSave", true) != NULL;
  if (!valid)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }
  *result = root;
  return 0;
}
