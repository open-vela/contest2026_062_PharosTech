/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_channels.c
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
#include "channels/feishu_bot.h"
#include "channels/mqtt_channel.h"
#include "channels/weixin_channel.h"
#include "infra/config_store.h"
#include "ny_agent.h"
#include <errno.h>
#include <nuttx/mutex.h>
#include <string.h>

static mutex_t g_channels_lock = NXMUTEX_INITIALIZER;
static bool g_mqtt_requested;
static bool g_mqtt_running;
static int g_mqtt_error;
static const char *const g_mqtt_fields[] = {
  "broker", "clientId", "topicIn", "topicOut", "username", "password"
};
static const char *const g_mqtt_keys[] = {
  AGENT_CFG_KEY_MQTT_BROKER,   AGENT_CFG_KEY_MQTT_CLIENT_ID,
  AGENT_CFG_KEY_MQTT_TOPIC_IN, AGENT_CFG_KEY_MQTT_TOPIC_OUT,
  AGENT_CFG_KEY_MQTT_USERNAME, AGENT_CFG_KEY_MQTT_PASSWORD
};
static const size_t g_mqtt_limits[] = { 127, 63, 127, 127, 63, 127 };

static cJSON *ny_agent_channels_status(void);

/****************************************************************************
 * Name: ny_agent_channels_status
 ****************************************************************************/

static cJSON *ny_agent_channels_status(void)
{
  cJSON *result = cJSON_CreateObject();
  if (result == NULL)
    return NULL;
  for (size_t i = 0; i < 6; i++)
    {
      char value[192] = { 0 };
      claw_config_get(g_mqtt_keys[i], value, sizeof(value));
      if (i == 5)
        cJSON_AddBoolToObject(result, "passwordSet", value[0] != 0);
      else
        cJSON_AddStringToObject(result, g_mqtt_fields[i], value);
    }
  cJSON_AddBoolToObject(result, "requested", g_mqtt_requested);
  cJSON_AddBoolToObject(result, "running", g_mqtt_running);
  cJSON_AddBoolToObject(result, "connected", mqtt_channel_connected());
  cJSON_AddNumberToObject(result, "lastError", g_mqtt_error);
  return result;
}

/****************************************************************************
 * Name: ny_agent_channels
 ****************************************************************************/

int ny_agent_channels(const char *topic, const cJSON *data, cJSON **result)
{
  if (!strncmp(topic, "agent.channels.weixin.", 22))
    return ny_agent_weixin(topic, data, result);
  if (!strncmp(topic, "agent.channels.feishu.", 22))
    return ny_agent_feishu(topic, data, result);
#ifndef CONFIG_AI_AGENT_MQTT
  (void)topic;
  (void)data;
  (void)result;
  return -ENOSYS;
#else
  int ret = 0;
  nxmutex_lock(&g_channels_lock);
  if (!strcmp(topic, "agent.channels.mqtt.save"))
    {
      const char *values[6];
      char password[128] = { 0 };
      claw_config_get(AGENT_CFG_KEY_MQTT_PASSWORD, password, sizeof(password));
      for (size_t i = 0; i < 6; i++)
        {
          const cJSON *item =
              cJSON_GetObjectItemCaseSensitive(data, g_mqtt_fields[i]);
          values[i] = cJSON_IsString(item) ? item->valuestring : "";
          if (i == 5 && item == NULL)
            values[i] = password;
          if (strlen(values[i]) > g_mqtt_limits[i] || (i < 4 && !values[i][0]))
            ret = -EINVAL;
        }
      if (g_mqtt_running || g_mqtt_requested)
        ret = -EBUSY;
      if (ret == 0)
        ret = claw_config_set_many(g_mqtt_keys, values, 6) == 0 ? 0 : -EIO;
    }
  else if (!strcmp(topic, "agent.channels.mqtt.start"))
    {
      char broker[192] = { 0 };
      claw_config_get(AGENT_CFG_KEY_MQTT_BROKER, broker, sizeof(broker));
      if (!broker[0])
        ret = -ENODATA;
      else
        g_mqtt_requested = true;
    }
  else if (!strcmp(topic, "agent.channels.mqtt.stop"))
    g_mqtt_requested = false;
  else if (strcmp(topic, "agent.channels.mqtt.get"))
    ret = -ENOSYS;
  if (ret == 0)
    {
      *result = ny_agent_channels_status();
      if (*result == NULL)
        ret = -ENOMEM;
    }
  nxmutex_unlock(&g_channels_lock);
  return ret;
#endif
}

/****************************************************************************
 * Name: ny_agent_channels_tick
 ****************************************************************************/

void ny_agent_channels_tick(void)
{
  ny_agent_weixin_tick();
  ny_agent_feishu_tick();
  ny_agent_node_tick();
  /* Run in the persistent agent task, which owns the channel pthreads. */
  nxmutex_lock(&g_channels_lock);
  if (g_mqtt_requested && !g_mqtt_running)
    {
      mqtt_channel_set_receiver(ny_agent_mqtt_receive);
      g_mqtt_error = mqtt_channel_init();
      if (g_mqtt_error == 0)
        g_mqtt_error = mqtt_channel_start();
      g_mqtt_running = g_mqtt_error == 0;
      if (!g_mqtt_running)
        g_mqtt_requested = false;
    }
  else if (!g_mqtt_requested && g_mqtt_running)
    {
      mqtt_channel_stop();
      g_mqtt_running = false;
    }
  nxmutex_unlock(&g_channels_lock);
}

/****************************************************************************
 * Name: ny_agent_channel_send
 ****************************************************************************/

int ny_agent_channel_send(const char *channel, const char *chat,
                          const char *context, const char *text)
{
  return !strcmp(channel, "mqtt") ? mqtt_channel_send(chat, text)
         : !strcmp(channel, "weixin")
             ? weixin_channel_send(chat, context, text)
         : !strcmp(channel, "feishu") ? feishu_send_message(chat, text)
                                      : -ENOSYS;
}
