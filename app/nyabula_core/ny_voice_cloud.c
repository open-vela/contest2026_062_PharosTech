/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_voice_cloud.c
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

#include <errno.h>
#include <nuttx/mutex.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ny_mimo.h"
#include "ny_voice_cloud.h"

#define NY_VOICE_CLOUD_TEXT_SIZE 4096
#define NY_VOICE_CLOUD_RATE      16000U
#define NY_VOICE_CLOUD_STACK     32768

static mutex_t g_cloud_lock = NXMUTEX_INITIALIZER;
static bool g_cloud_busy;
static bool g_cloud_cancelled;
static uint32_t g_cloud_request;
static int16_t *g_cloud_pcm;
static size_t g_cloud_samples;
static int g_cloud_error;
static char g_cloud_text[NY_VOICE_CLOUD_TEXT_SIZE];

static int ny_voice_cloud_worker(int argc, char **argv);

/****************************************************************************
 * Name: ny_voice_cloud_worker
 ****************************************************************************/

static int ny_voice_cloud_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  nxmutex_lock(&g_cloud_lock);
  int16_t *pcm = g_cloud_pcm;
  size_t samples = g_cloud_samples;
  nxmutex_unlock(&g_cloud_lock);
  char *text = calloc(1, NY_VOICE_CLOUD_TEXT_SIZE);
  int ret = text ? ny_mimo_transcribe(pcm, samples, NY_VOICE_CLOUD_RATE, text,
                                      NY_VOICE_CLOUD_TEXT_SIZE)
                 : -ENOMEM;
  free(pcm);
  nxmutex_lock(&g_cloud_lock);
  g_cloud_pcm = NULL;
  g_cloud_error = g_cloud_cancelled ? -ECANCELED : ret;
  if (!g_cloud_error)
    strcpy(g_cloud_text, text);
  g_cloud_busy = false;
  nxmutex_unlock(&g_cloud_lock);
  free(text);
  return 0;
}

/****************************************************************************
 * Name: ny_voice_cloud_submit
 ****************************************************************************/

int ny_voice_cloud_submit(int16_t *pcm, size_t samples, uint32_t *request)
{
  if (!pcm || !samples || samples > NY_VOICE_CLOUD_RATE * 30 || !request)
    return -EINVAL;
  nxmutex_lock(&g_cloud_lock);
  if (g_cloud_busy)
    {
      nxmutex_unlock(&g_cloud_lock);
      return -EBUSY;
    }
  g_cloud_pcm = pcm;
  g_cloud_samples = samples;
  g_cloud_busy = true;
  g_cloud_cancelled = false;
  g_cloud_error = 0;
  g_cloud_text[0] = '\0';
  if (++g_cloud_request == 0)
    ++g_cloud_request;
  int ret = task_create("nycloudasr", 80, NY_VOICE_CLOUD_STACK,
                        ny_voice_cloud_worker, NULL);
  if (ret < 0)
    {
      ret = -errno;
      g_cloud_busy = false;
      g_cloud_pcm = NULL;
    }
  else
    {
      *request = g_cloud_request;
      ret = 0;
    }
  nxmutex_unlock(&g_cloud_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_voice_cloud_poll
 ****************************************************************************/

int ny_voice_cloud_poll(uint32_t request, char *text, size_t capacity)
{
  if (!request || !text || !capacity)
    return -EINVAL;
  nxmutex_lock(&g_cloud_lock);
  int ret = request != g_cloud_request ? -ESTALE
            : g_cloud_cancelled        ? -ECANCELED
            : g_cloud_busy             ? 0
            : g_cloud_error            ? g_cloud_error
                                       : 1;
  if (ret == 1)
    {
      if (strlen(g_cloud_text) >= capacity)
        ret = -EOVERFLOW;
      else
        strcpy(text, g_cloud_text);
    }
  nxmutex_unlock(&g_cloud_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_voice_cloud_cancel
 ****************************************************************************/

void ny_voice_cloud_cancel(uint32_t request)
{
  nxmutex_lock(&g_cloud_lock);
  if (request && request == g_cloud_request)
    {
      g_cloud_cancelled = true;
      g_cloud_text[0] = '\0';
    }
  nxmutex_unlock(&g_cloud_lock);
}
