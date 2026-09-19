/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_weixin.c
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

#include "channels/weixin_channel.h"
#include "infra/config_store.h"
#include "ny_agent.h"
#include <errno.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mutex_t g_weixin_lock = NXMUTEX_INITIALIZER;
static bool g_weixin_requested;
static bool g_weixin_running;
static int g_weixin_error;
static char g_weixin_qrcode[192];
static char g_weixin_qr_url[1024];
static int g_weixin_login_state;

static void ny_agent_weixin_receive(const char *user, const char *request,
                                    const char *context, const char *text);
static cJSON *ny_agent_weixin_status(void);

/****************************************************************************
 * Name: ny_agent_weixin_receive
 ****************************************************************************/

static void ny_agent_weixin_receive(const char *user, const char *request,
                                    const char *context, const char *text)
{
  ny_agent_channel_receive("weixin", user, request, context, text);
}

/****************************************************************************
 * Name: ny_agent_weixin_status
 ****************************************************************************/

static cJSON *ny_agent_weixin_status(void)
{
  char host[128] = "ilinkai.weixin.qq.com";
  char port[8] = "443";
  char token[256] = { 0 };
  claw_config_get("weixin.host", host, sizeof(host));
  claw_config_get("weixin.port", port, sizeof(port));
  claw_config_get("weixin.token", token, sizeof(token));
  cJSON *result = cJSON_CreateObject();
  if (result == NULL)
    return NULL;
  cJSON_AddStringToObject(result, "host",
                          host[0] ? host : "ilinkai.weixin.qq.com");
  cJSON_AddStringToObject(result, "port", port[0] ? port : "443");
  cJSON_AddBoolToObject(result, "tokenSet", token[0] != 0);
  cJSON_AddBoolToObject(result, "requested", g_weixin_requested);
  cJSON_AddBoolToObject(result, "running", g_weixin_running);
  cJSON_AddBoolToObject(result, "connected", weixin_channel_connected());
  cJSON_AddNumberToObject(result, "lastError",
                          g_weixin_error ? g_weixin_error
                                         : weixin_channel_last_error());
  cJSON_AddNumberToObject(result, "loginState", g_weixin_login_state);
  cJSON_AddStringToObject(result, "qrContent", g_weixin_qr_url);
  return result;
}

/****************************************************************************
 * Name: ny_agent_weixin
 ****************************************************************************/

int ny_agent_weixin(const char *topic, const cJSON *data, cJSON **result)
{
#ifndef CONFIG_AI_AGENT_WEIXIN
  (void)topic;
  (void)data;
  (void)result;
  return -ENOSYS;
#else
  int ret = 0;
  nxmutex_lock(&g_weixin_lock);
  bool stopped = !g_weixin_running && !g_weixin_requested;
  if (!strcmp(topic, "agent.channels.weixin.get"))
    {
    }
  else if (!strcmp(topic, "agent.channels.weixin.stop"))
    {
      g_weixin_requested = false;
      weixin_channel_request_stop();
    }
  else if (!stopped)
    ret = -EBUSY;
  else if (!strcmp(topic, "agent.channels.weixin.save"))
    {
      const cJSON *h = cJSON_GetObjectItemCaseSensitive(data, "host");
      const cJSON *p = cJSON_GetObjectItemCaseSensitive(data, "port");
      const cJSON *t = cJSON_GetObjectItemCaseSensitive(data, "token");
      char token[256] = { 0 };
      claw_config_get("weixin.token", token, sizeof(token));
      if (!cJSON_IsString(h) || !h->valuestring[0] ||
          strlen(h->valuestring) > 127 || !cJSON_IsString(p) ||
          !p->valuestring[0] || strlen(p->valuestring) > 5 ||
          strspn(p->valuestring, "0123456789") != strlen(p->valuestring) ||
          atoi(p->valuestring) < 1 || atoi(p->valuestring) > 65535 ||
          (t != NULL && (!cJSON_IsString(t) || strlen(t->valuestring) > 255)))
        ret = -EINVAL;
      else
        {
          const char *keys[] = { "weixin.host", "weixin.port",
                                 "weixin.token" };
          const char *values[] = { h->valuestring, p->valuestring,
                                   t ? t->valuestring : token };
          ret = claw_config_set_many(keys, values, 3) == 0 ? 0 : -EIO;
        }
    }
  else if (!strcmp(topic, "agent.channels.weixin.start"))
    {
      char token[256] = { 0 };
      claw_config_get("weixin.token", token, sizeof(token));
      if (!token[0])
        ret = -ENODATA;
      else
        g_weixin_requested = true;
    }
  else if (!strcmp(topic, "agent.channels.weixin.login"))
    {
      g_weixin_qrcode[0] = g_weixin_qr_url[0] = 0;
      g_weixin_login_state = 0;
      ret = weixin_channel_init();
      if (ret == 0)
        ret = weixin_channel_login(g_weixin_qr_url, sizeof(g_weixin_qr_url),
                                   g_weixin_qrcode, sizeof(g_weixin_qrcode));
      if (ret < 0)
        ret = -EIO;
    }
  else if (!strcmp(topic, "agent.channels.weixin.login.poll"))
    {
      if (!g_weixin_qrcode[0])
        ret = -ENODATA;
      else
        {
          g_weixin_login_state = weixin_channel_poll_login(g_weixin_qrcode);
          if (g_weixin_login_state == 1 || g_weixin_login_state == -3)
            g_weixin_qr_url[0] = 0;
          if (g_weixin_login_state < 0 && g_weixin_login_state != -3)
            ret = -EIO;
        }
    }
  else
    ret = -ENOSYS;
  if (strcmp(topic, "agent.channels.weixin.get") || ret < 0)
    g_weixin_error = ret;
  if (ret == 0)
    {
      *result = ny_agent_weixin_status();
      if (*result == NULL)
        ret = -ENOMEM;
    }
  nxmutex_unlock(&g_weixin_lock);
  return ret;
#endif
}

/****************************************************************************
 * Name: ny_agent_weixin_tick
 ****************************************************************************/

void ny_agent_weixin_tick(void)
{
  nxmutex_lock(&g_weixin_lock);
  if (g_weixin_requested && !g_weixin_running)
    {
      weixin_channel_set_receiver(ny_agent_weixin_receive);
      g_weixin_error = weixin_channel_init();
      if (g_weixin_error == 0)
        g_weixin_error = weixin_channel_start();
      g_weixin_running = g_weixin_error == 0;
      if (!g_weixin_running)
        g_weixin_requested = false;
    }
  if (g_weixin_running && weixin_channel_exited())
    {
      weixin_channel_stop();
      if (g_weixin_requested)
        g_weixin_error = -EIO;
      g_weixin_requested = false;
      g_weixin_running = false;
    }
  nxmutex_unlock(&g_weixin_lock);
}
