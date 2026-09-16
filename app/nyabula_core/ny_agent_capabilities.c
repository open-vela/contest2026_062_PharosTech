/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_capabilities.c
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
#include <nuttx/config.h>
#include <stdbool.h>

struct ny_agent_capability_s
{
  const char *id;
  const char *group;
  const char *reason;
  bool compiled;
  bool enabled;
};

/****************************************************************************
 * Name: ny_agent_capabilities
 ****************************************************************************/

cJSON *ny_agent_capabilities(void)
{
  static const struct ny_agent_capability_s entries[] = {
    { "core-read", "tools", "owner_only", true, true },
#ifdef CONFIG_NYABULA_CORE_EYE
    { "eye-expression", "tools", "core_eye_expression", true, true },
#else
    { "eye-expression", "tools", "hardware_provider_required", false, false },
#endif
    { "core-write", "tools", "per_action_owner_approval", true, true },
    { "file-shell", "tools", "core_workspace_tools", true, true },
    { "web-search-fetch-weather", "tools", "core_network_tools", true, true },
    { "device-sensors", "tools", "hardware_provider_required", true, false },
#ifdef CONFIG_NYABULA_CORE_MEDIA
    { "music", "tools", "core_native_wav_output_required", true, true },
#else
    { "music", "tools", "not_compiled", false, false },
#endif
    { "skills", "skills", "owner_managed_guidance", true, true },
    { "memory-persona", "memory", "core_profile_context", true, true },
    { "cron-heartbeat", "automation", "core_durable_scheduler", true, true },
    { "voice-asr-tts", "channels", "audio_provider_required", true, false },
#ifdef CONFIG_AI_AGENT_CAMERA
    { "vision-camera", "channels", "hardware_provider_required", true, false },
#else
    { "vision-camera", "channels", "not_compiled", false, false },
#endif
    { "mcp-client", "mcp", "core_scoped_mcp_2025", true, true },
    { "mcp-server", "mcp", "core_scoped_mcp_in", true, true },
#ifdef CONFIG_AI_AGENT_NODE
    { "openclaw-node", "nodes", "core_signed_openclaw_node", true, true },
#else
    { "openclaw-node", "nodes", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_FEISHU
    { "feishu", "channels", "core_feishu_channel", true, true },
#else
    { "feishu", "channels", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_WEIXIN
    { "weixin", "channels", "core_weixin_channel", true, true },
#else
    { "weixin", "channels", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_MQTT
    { "mqtt", "channels", "core_owner_mqtt_channel", true, true },
#else
    { "mqtt", "channels", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_SKILL_SYNC
    { "skill-sync", "skills", "synchronization_policy_required", true, false },
#else
    { "skill-sync", "skills", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_BLE_GATT
    { "ble-gatt", "connections", "hardware_provider_required", true, false },
#else
    { "ble-gatt", "connections", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_BLE_NET
    { "ble-network", "connections", "hardware_provider_required", true,
      false },
#else
    { "ble-network", "connections", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
    { "lvgl-channel", "channels", "display_provider_required", true, false },
#else
    { "lvgl-channel", "channels", "not_compiled", false, false },
#endif
#ifdef CONFIG_AI_AGENT_REST_API
    { "rest-diagnostics", "diagnostics", "protected_endpoint_required", true,
      false },
#else
    { "rest-diagnostics", "diagnostics", "not_compiled", false, false },
#endif
    { "quickapp", "channels", "quickapp_runtime_required", false, false },
    { "web-cli", "channels", "core_authenticated_transport", true, true },
    { "model-router", "models", "core_model_router", true, true }
  };
  cJSON *root = cJSON_CreateObject();
  cJSON *items = cJSON_AddArrayToObject(root, "items");
  if (root == NULL || items == NULL)
    {
      cJSON_Delete(root);
      return NULL;
    }

  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
    {
      const struct ny_agent_capability_s *entry = &entries[i];
      cJSON *item = cJSON_CreateObject();
      if (item == NULL ||
          cJSON_AddStringToObject(item, "id", entry->id) == NULL ||
          cJSON_AddStringToObject(item, "group", entry->group) == NULL ||
          cJSON_AddStringToObject(item, "reason", entry->reason) == NULL ||
          cJSON_AddBoolToObject(item, "compiled", entry->compiled) == NULL ||
          cJSON_AddBoolToObject(item, "enabled", entry->enabled) == NULL ||
          !cJSON_AddItemToArray(items, item))
        {
          cJSON_Delete(item);
          cJSON_Delete(root);
          return NULL;
        }
    }
  return root;
}
