/****************************************************************************
 * app/nyabula_core/ny_product_privacy.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/


#include "ny_product_privacy.h"
#ifdef CONFIG_NYABULA_CORE_SPEAKER
#include "ny_product_speaker.h"
#endif
#ifdef CONFIG_NYABULA_CORE_VISION
#include "ny_product_vision.h"
#endif
#include "ny_product_store.h"
#ifdef CONFIG_NYABULA_CORE_CAMERA
#include "ny_product_camera.h"
#endif
#ifdef CONFIG_NYABULA_CORE_INTERCOM
#include "ny_product_intercom.h"
#endif
#ifdef CONFIG_NYABULA_CORE_VOICE
#include "ny_voice.h"
#endif
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <string.h>

static mutex_t g_privacy_lock = NXMUTEX_INITIALIZER;
static mutex_t g_privacy_write_lock = NXMUTEX_INITIALIZER;
static bool g_privacy_loaded;
static bool g_privacy_camera;
static bool g_privacy_mic;
static uint64_t g_privacy_revision;

static int ny_privacy_load(void);
static cJSON *ny_privacy_status(void);

/****************************************************************************
 * Name: ny_privacy_load
 ****************************************************************************/

static int ny_privacy_load(void)
{
  if (g_privacy_loaded)
    return 0;
  cJSON *saved = NULL;
  uint64_t revision = 0;
  int ret = ny_product_store_read("privacy", &saved, &revision);
  if (ret == 0 && saved == NULL)
    {
      g_privacy_camera = g_privacy_mic = true;
      ret = 0;
    }
  else if (ret == 0)
    {
      const cJSON *camera = cJSON_GetObjectItemCaseSensitive(saved, "camera");
      const cJSON *mic = cJSON_GetObjectItemCaseSensitive(saved, "mic");
      if (!cJSON_IsBool(camera) || !cJSON_IsBool(mic))
        ret = -EINVAL;
      else
        {
          g_privacy_camera = cJSON_IsTrue(camera);
          g_privacy_mic = cJSON_IsTrue(mic);
        }
    }
  cJSON_Delete(saved);
  if (ret == 0)
    {
      g_privacy_revision = revision;
      g_privacy_loaded = true;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_product_privacy_camera_allowed
 ****************************************************************************/

bool ny_product_privacy_camera_allowed(void)
{
  nxmutex_lock(&g_privacy_lock);
  bool allowed = ny_privacy_load() == 0 && g_privacy_camera;
  nxmutex_unlock(&g_privacy_lock);
  return allowed;
}

/****************************************************************************
 * Name: ny_product_privacy_mic_allowed
 ****************************************************************************/

bool ny_product_privacy_mic_allowed(void)
{
  nxmutex_lock(&g_privacy_lock);
  bool allowed = ny_privacy_load() == 0 && g_privacy_mic;
  nxmutex_unlock(&g_privacy_lock);
  return allowed;
}

/****************************************************************************
 * Name: ny_privacy_status
 ****************************************************************************/

static cJSON *ny_privacy_status(void)
{
  cJSON *out = cJSON_CreateObject();
  if (out == NULL)
    return NULL;
  bool ok = cJSON_AddBoolToObject(out, "camera", g_privacy_camera) &&
            cJSON_AddBoolToObject(out, "mic", g_privacy_mic) &&
            cJSON_AddNumberToObject(out, "revision", g_privacy_revision);
  if (!ok)
    {
      cJSON_Delete(out);
      return NULL;
    }
  return out;
}

/****************************************************************************
 * Name: ny_product_privacy_request
 ****************************************************************************/

int ny_product_privacy_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result)
{
  bool write = strcmp(topic, "privacy.set") == 0;
  if (!write && strcmp(topic, "privacy.status"))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  nxmutex_lock(&g_privacy_write_lock);
  nxmutex_lock(&g_privacy_lock);
  int ret = ny_privacy_load();
  bool camera = g_privacy_camera;
  bool mic = g_privacy_mic;
  bool old_mic = mic;
  if (ret == 0 && write)
    {
      const cJSON *a = cJSON_GetObjectItemCaseSensitive(data, "camera");
      const cJSON *b = cJSON_GetObjectItemCaseSensitive(data, "mic");
      const cJSON *rev = cJSON_GetObjectItemCaseSensitive(data, "revision");
      if ((!a && !b) || (a && !cJSON_IsBool(a)) ||
          (b && !cJSON_IsBool(b)) || !cJSON_IsNumber(rev) ||
          !isfinite(rev->valuedouble) || rev->valuedouble < 0 ||
          rev->valuedouble > 9007199254740991.0 ||
          floor(rev->valuedouble) != rev->valuedouble)
        ret = -EINVAL;
      else if ((uint64_t)rev->valuedouble != g_privacy_revision)
        ret = -ESTALE;
      if (ret == 0)
        {
          camera = a ? cJSON_IsTrue(a) : camera;
          mic = b ? cJSON_IsTrue(b) : mic;
          cJSON *saved = cJSON_CreateObject();
          if (!saved || !cJSON_AddBoolToObject(saved, "camera", camera) ||
              !cJSON_AddBoolToObject(saved, "mic", mic))
            ret = -ENOMEM;
          else
            {
              uint64_t updated = 0;
              ret = ny_product_store_write("privacy", saved,
                        g_privacy_revision, &updated);
              if (ret == 0)
                g_privacy_revision = updated;
            }
          cJSON_Delete(saved);
          if (ret == 0)
            {
              g_privacy_camera = camera;
              g_privacy_mic = mic;
            }
        }
    }
  nxmutex_unlock(&g_privacy_lock);
  /* Publish the gates before stopping producers. No producer lock is held
   * while loading or writing the persistent policy.
   */
  if (ret == 0 && write)
    {
#ifdef CONFIG_NYABULA_CORE_VISION
      if (!camera)
        ny_product_vision_shutdown();
#endif
#ifdef CONFIG_NYABULA_CORE_CAMERA
      if (!camera)
        ny_product_camera_shutdown();
#endif
#ifdef CONFIG_NYABULA_CORE_SPEAKER
      if (!mic)
        ny_product_speaker_shutdown();
#endif
#ifdef CONFIG_NYABULA_CORE_INTERCOM
      if (!mic)
        ny_product_intercom_shutdown();
#endif
#ifdef CONFIG_NYABULA_CORE_VOICE
      if (!mic)
        ret = ny_voice_stop();
      else if (!old_mic)
        ret = ny_voice_start();
#else
      (void)old_mic;
#endif
    }
  nxmutex_lock(&g_privacy_lock);
  if (ret == 0)
    {
      *result = ny_privacy_status();
      if (*result == NULL)
        ret = -ENOMEM;
    }
  nxmutex_unlock(&g_privacy_lock);
  nxmutex_unlock(&g_privacy_write_lock);
  return ret;
}
