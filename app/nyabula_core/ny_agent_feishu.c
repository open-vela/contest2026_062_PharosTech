/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_feishu.c
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
#include "infra/config_store.h"
#include "ny_agent.h"
#include <errno.h>
#include <nuttx/mutex.h>
#include <string.h>

static mutex_t g_feishu_lock = NXMUTEX_INITIALIZER;
static bool g_feishu_requested;
static bool g_feishu_running;
static int g_feishu_error;

static void ny_agent_feishu_receive(const char *chat, const char *request,
                                    const char *text);
static cJSON *ny_agent_feishu_status(void);

/****************************************************************************
 * Name: ny_agent_feishu_receive
 ****************************************************************************/

static void ny_agent_feishu_receive(const char *chat, const char *request,
                                    const char *text)
{
  ny_agent_channel_receive("feishu", chat, request, "", text);
}

/****************************************************************************
 * Name: ny_agent_feishu_status
 ****************************************************************************/

static cJSON *ny_agent_feishu_status(void)
{
  char app[64] = { 0 };
  char secret[64] = { 0 };
  claw_config_get(AGENT_CFG_KEY_FEISHU_APP_ID, app, sizeof(app));
  claw_config_get(AGENT_CFG_KEY_FEISHU_APP_SECRET, secret, sizeof(secret));
  cJSON *result = cJSON_CreateObject();
  if (result == NULL)
    return NULL;
  cJSON_AddStringToObject(result, "appId", app);
  cJSON_AddBoolToObject(result, "secretSet", secret[0] != 0);
  cJSON_AddBoolToObject(result, "requested", g_feishu_requested);
  cJSON_AddBoolToObject(result, "running", g_feishu_running);
  cJSON_AddBoolToObject(result, "connected", feishu_bot_connected());
  cJSON_AddNumberToObject(result, "lastError",
                          g_feishu_error ? g_feishu_error
                                         : feishu_bot_last_error());
  return result;
}

/****************************************************************************
 * Name: ny_agent_feishu
 ****************************************************************************/

int ny_agent_feishu(const char *topic, const cJSON *data, cJSON **result)
{
#ifndef CONFIG_AI_AGENT_FEISHU
  (void)topic;
  (void)data;
  (void)result;
  return -ENOSYS;
#else
  int ret = 0;
  nxmutex_lock(&g_feishu_lock);
  if (!strcmp(topic, "agent.channels.feishu.get"))
    {
    }
  else if (!strcmp(topic, "agent.channels.feishu.stop"))
    {
      g_feishu_requested = false;
      feishu_bot_request_stop();
    }
  else if (g_feishu_running || g_feishu_requested)
    ret = -EBUSY;
  else if (!strcmp(topic, "agent.channels.feishu.save"))
    {
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "appId");
      const cJSON *key = cJSON_GetObjectItemCaseSensitive(data, "secret");
      char saved[64] = { 0 };
      claw_config_get(AGENT_CFG_KEY_FEISHU_APP_SECRET, saved, sizeof(saved));
      if (!cJSON_IsString(id) || !id->valuestring[0] ||
          strlen(id->valuestring) > 63 ||
          (key != NULL &&
           (!cJSON_IsString(key) || strlen(key->valuestring) > 63)))
        ret = -EINVAL;
      else
        {
          const char *keys[] = { AGENT_CFG_KEY_FEISHU_APP_ID,
                                 AGENT_CFG_KEY_FEISHU_APP_SECRET };
          const char *values[] = { id->valuestring,
                                   key ? key->valuestring : saved };
          ret = claw_config_set_many(keys, values, 2) == 0 ? 0 : -EIO;
#ifdef CONFIG_AI_AGENT_SIM_HTTP_FIXTURE
          const cJSON *port =
              cJSON_GetObjectItemCaseSensitive(data, "fixturePort");
          if (ret == 0 && cJSON_IsString(port))
            ret = claw_config_set("feishu.fixturePort", port->valuestring) == 0
                      ? 0
                      : -EIO;
#endif
        }
    }
  else if (!strcmp(topic, "agent.channels.feishu.start"))
    {
      char app[64] = { 0 }, secret[64] = { 0 };
      claw_config_get(AGENT_CFG_KEY_FEISHU_APP_ID, app, sizeof(app));
      claw_config_get(AGENT_CFG_KEY_FEISHU_APP_SECRET, secret, sizeof(secret));
      if (!app[0] || !secret[0])
        ret = -ENODATA;
      else
        g_feishu_requested = true;
    }
  else
    ret = -ENOSYS;
  if (strcmp(topic, "agent.channels.feishu.get") || ret < 0)
    g_feishu_error = ret;
  if (ret == 0)
    {
      *result = ny_agent_feishu_status();
      if (*result == NULL)
        ret = -ENOMEM;
    }
  nxmutex_unlock(&g_feishu_lock);
  return ret;
#endif
}

/****************************************************************************
 * Name: ny_agent_feishu_tick
 ****************************************************************************/

void ny_agent_feishu_tick(void)
{
  nxmutex_lock(&g_feishu_lock);
  if (g_feishu_requested && !g_feishu_running)
    {
      feishu_set_receiver(ny_agent_feishu_receive);
      g_feishu_error = feishu_bot_init();
      if (g_feishu_error == 0)
        g_feishu_error = feishu_bot_start();
      g_feishu_running = g_feishu_error == 0;
      if (!g_feishu_running)
        g_feishu_requested = false;
    }
  if (g_feishu_running && feishu_bot_exited())
    {
      feishu_bot_stop();
      if (g_feishu_requested)
        g_feishu_error = -EIO;
      g_feishu_requested = false;
      g_feishu_running = false;
    }
  nxmutex_unlock(&g_feishu_lock);
}
