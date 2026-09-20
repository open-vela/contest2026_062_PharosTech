/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_runtime.c
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
#ifdef CONFIG_NYABULA_CORE_COMPUTE
#include "ny_compute.h"
#endif
#include <errno.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <sched.h>
#include <stdbool.h>
#include <unistd.h>

static mutex_t g_product_runtime_lock = NXMUTEX_INITIALIZER;
static bool g_product_running;
static bool g_product_stopping;
static int g_product_last_error;

static int ny_product_worker(int argc, char **argv);

/****************************************************************************
 * Name: ny_product_worker
 ****************************************************************************/

static int ny_product_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  for (;;)
    {
      nxmutex_lock(&g_product_runtime_lock);
      bool stop = g_product_stopping;
      nxmutex_unlock(&g_product_runtime_lock);
      if (stop)
        {
          break;
        }

      int ret = ny_product_timers_tick();
      int alarm_ret = ny_product_alarms_tick();
      int notice_ret = ny_product_notifications_tick();
      int briefing_ret = ny_product_briefing_tick();
#ifdef CONFIG_NYABULA_CORE_AGENT
      int companion_ret = ny_product_companion_tick();
      if (ret == 0)
        ret = companion_ret;
#endif
      if (ret == 0)
        ret = briefing_ret;
#ifdef CONFIG_NYABULA_CORE_WEATHER
      int weather_ret = ny_product_weather_tick();
      if (ret == 0)
        ret = weather_ret;
#endif
      if (ret == 0)
        ret = notice_ret;
      if (ret == 0)
        ret = alarm_ret;
#ifdef CONFIG_NYABULA_CORE_AUDIO
      /* Ahead of the player: a track must start at the restored volume. */

      int audio_ret = ny_product_audio_tick();
      if (ret == 0)
        ret = audio_ret;
#endif
      int media_ret = ny_product_media_tick();
      if (ret == 0)
        ret = media_ret;
#ifdef CONFIG_NYABULA_CORE_NETWORK
      int network_ret = ny_product_network_tick();
      if (ret == 0)
        ret = network_ret;
#endif
#ifdef CONFIG_NYABULA_CORE_AGENT
      int agent_ret = ny_agent_automation_tick();
      if (ret == 0)
        ret = agent_ret;
#endif
      nxmutex_lock(&g_product_runtime_lock);
      g_product_last_error = ret;
      nxmutex_unlock(&g_product_runtime_lock);
      usleep(100000);
    }

  ny_product_media_shutdown();
#ifdef CONFIG_NYABULA_CORE_COMPUTE
  /* The compute link lives exactly as long as the product services do. */

  ny_compute_stop();
#endif
#ifdef CONFIG_NYABULA_CORE_NETWORK
  ny_product_network_shutdown();
#endif
#ifdef CONFIG_NYABULA_CORE_WEATHER
  ny_product_weather_shutdown();
#endif
  nxmutex_lock(&g_product_runtime_lock);
  g_product_running = false;
  g_product_stopping = false;
  nxmutex_unlock(&g_product_runtime_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_product_start
 ****************************************************************************/

int ny_product_start(void)
{
  int ret = nxmutex_lock(&g_product_runtime_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_product_running)
    {
      ret = g_product_stopping ? -EBUSY : 0;
    }
  else
    {
      g_product_stopping = false;
      g_product_running = true;
      ret = task_create("nyproduct", 90, 32768, ny_product_worker, NULL);
      if (ret < 0)
        {
          g_product_running = false;
          ret = -errno;
        }
      else
        {
          ret = 0;
#ifdef CONFIG_NYABULA_CORE_COMPUTE
          /* Best effort: a firmware booted without the compute domain still
           * runs every other product service, and compute.status says why
           * the link is down.
           */

          ny_compute_start();
#endif
        }
    }

  nxmutex_unlock(&g_product_runtime_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_stop
 ****************************************************************************/

int ny_product_stop(void)
{
  int ret = nxmutex_lock(&g_product_runtime_lock);
  if (ret < 0)
    {
      return ret;
    }

  g_product_stopping = g_product_running;
  nxmutex_unlock(&g_product_runtime_lock);
  uint64_t deadline = ny_product_time_ms(true) + 3000;
  for (;;)
    {
      nxmutex_lock(&g_product_runtime_lock);
      bool running = g_product_running;
      nxmutex_unlock(&g_product_runtime_lock);
      if (!running)
        {
          return 0;
        }

      if (ny_product_time_ms(true) >= deadline)
        {
          return -ETIMEDOUT;
        }

      usleep(10000);
    }
}

/****************************************************************************
 * Name: ny_product_runtime_status
 ****************************************************************************/

cJSON *ny_product_runtime_status(void)
{
  cJSON *state = cJSON_CreateObject();
  if (state == NULL)
    {
      return NULL;
    }

  int ret = nxmutex_lock(&g_product_runtime_lock);
  if (ret < 0)
    {
      cJSON_Delete(state);
      return NULL;
    }

  bool ok = cJSON_AddBoolToObject(state, "running", g_product_running) &&
            cJSON_AddBoolToObject(state, "stopping", g_product_stopping) &&
            cJSON_AddNumberToObject(state, "last_error", g_product_last_error);
  nxmutex_unlock(&g_product_runtime_lock);
  if (!ok)
    {
      cJSON_Delete(state);
      return NULL;
    }

  return state;
}
