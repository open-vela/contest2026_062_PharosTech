/****************************************************************************
 * app/nyabula/src/nyabula_eye_service.c
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

#include "../include/nyabula_eye_service.h"
#include <errno.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static mutex_t g_eye_lock = NXMUTEX_INITIALIZER;
static struct nyabula_eye_engine_s *g_eye_engine;
static struct nyabula_core_s *g_eye_control;
static pthread_t g_eye_owner;

static int nyabula_eye_service_check_depth(const char *json, size_t length);
static int nyabula_eye_service_enqueue(struct nyabula_core_command_s *command,
                                       const char *source);

/* Bound cJSON recursion before entering its recursive parser. */

static int nyabula_eye_service_check_depth(const char *json, size_t length)
{
  unsigned int depth = 0;
  bool quoted = false;
  bool escaped = false;

  for (size_t i = 0; i < length; i++)
    {
      if (quoted)
        {
          if (escaped)
            {
              escaped = false;
            }
          else if (json[i] == '\\')
            {
              escaped = true;
            }
          else if (json[i] == '"')
            {
              quoted = false;
            }
        }
      else if (json[i] == '"')
        {
          quoted = true;
        }
      else if (json[i] == '{' || json[i] == '[')
        {
          if (++depth > NYABULA_EYE_JSON_DEPTH)
            {
              return -E2BIG;
            }
        }
      else if (json[i] == '}' || json[i] == ']')
        {
          if (depth == 0)
            {
              return -EINVAL;
            }
          depth--;
        }
    }

  return quoted || depth != 0 ? -EINVAL : 0;
}

static int nyabula_eye_service_enqueue(struct nyabula_core_command_s *command,
                                       const char *source)
{
  int ret;
  if (source == NULL || source[0] == '\0' ||
      strlen(source) >= sizeof(command->source))
    {
      return -EINVAL;
    }

  /* Reset is an administrative operation, not a plugin capability. */

  if (command->action == NYABULA_CORE_ACTION_RESET)
    {
      return -EACCES;
    }

  strlcpy(command->source, source, sizeof(command->source));
  ret = nxmutex_lock(&g_eye_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = g_eye_control == NULL ? -ENODEV
                              : nyabula_core_submit(g_eye_control, command);
  nxmutex_unlock(&g_eye_lock);
  return ret;
}

int nyabula_eye_service_attach(lv_obj_t *left, lv_obj_t *right)
{
  int ret;
  if (left == NULL || right == NULL || left == right)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_eye_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_eye_control != NULL)
    {
      nxmutex_unlock(&g_eye_lock);
      return -EBUSY;
    }

  g_eye_engine = nyabula_eye_engine_create_dual(left, right);
  if (g_eye_engine == NULL)
    {
      nxmutex_unlock(&g_eye_lock);
      return -ENOMEM;
    }

  g_eye_control = nyabula_core_create(g_eye_engine);
  if (g_eye_control == NULL)
    {
      nyabula_eye_engine_destroy(g_eye_engine);
      g_eye_engine = NULL;
      nxmutex_unlock(&g_eye_lock);
      return -ENOMEM;
    }

  g_eye_owner = pthread_self();
  nxmutex_unlock(&g_eye_lock);
  return 0;
}

int nyabula_eye_service_tick(void)
{
  int ret = nxmutex_lock(&g_eye_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_eye_control == NULL)
    {
      ret = -ENODEV;
    }
  else if (!pthread_equal(g_eye_owner, pthread_self()))
    {
      ret = -EPERM;
    }
  else
    {
      nyabula_core_tick(g_eye_control);
    }

  nxmutex_unlock(&g_eye_lock);
  return ret;
}

int nyabula_eye_service_detach(void)
{
  int ret = nxmutex_lock(&g_eye_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_eye_control != NULL)
    {
      if (!pthread_equal(g_eye_owner, pthread_self()))
        {
          nxmutex_unlock(&g_eye_lock);
          return -EPERM;
        }

      nyabula_core_destroy(g_eye_control);
      g_eye_control = NULL;
      nyabula_eye_engine_destroy(g_eye_engine);
      g_eye_engine = NULL;
    }

  nxmutex_unlock(&g_eye_lock);
  return 0;
}

int nyabula_eye_service_submit(const char *source, const char *json,
                               size_t length)
{
  struct nyabula_core_command_s command;
  char error[NYABULA_CORE_ERROR_MAX];
  cJSON *object;
  char *input;
  int ret;

  if (json == NULL || length == 0 || length > NYABULA_EYE_JSON_LIMIT ||
      memchr(json, '\0', length) != NULL)
    {
      return -EINVAL;
    }

  ret = nyabula_eye_service_check_depth(json, length);
  if (ret < 0)
    {
      return ret;
    }

  input = malloc(length + 1);
  if (input == NULL)
    {
      return -ENOMEM;
    }

  memcpy(input, json, length);
  input[length] = '\0';
  object = cJSON_ParseWithOpts(input, NULL, true);
  if (object == NULL)
    {
      free(input);
      return -EINVAL;
    }

  ret = nyabula_eye_json_parse_command(object, &command, error, sizeof(error));
  if (ret == 0)
    {
      if (cJSON_GetObjectItemCaseSensitive(object, "lease_ms") == NULL)
        {
          command.lease_ms = NYABULA_EYE_DEFAULT_LEASE_MS;
        }
      ret = nyabula_eye_service_enqueue(&command, source);
    }

  cJSON_Delete(object);
  free(input);
  return ret;
}

int nyabula_eye_service_notify(const char *source, const char *text,
                               size_t length)
{
  struct nyabula_core_command_s command = { 0 };
  if (text == NULL || length >= NYABULA_EYE_TEXT_MEDIUM ||
      memchr(text, '\0', length) != NULL)
    {
      return -EMSGSIZE;
    }

  command.action = NYABULA_CORE_ACTION_SCENE_SHOW;
  command.priority = 50;
  command.lease_ms = NYABULA_EYE_DEFAULT_LEASE_MS;
  command.data.scene_show.request.scene = NYABULA_EYE_SCENE_CAPTION;
  command.data.scene_show.request.style = NYABULA_EYE_SCENE_STYLE_MINIMAL;
  memcpy(command.data.scene_show.request.payload.current_line, text, length);
  return nyabula_eye_service_enqueue(&command, source);
}

int nyabula_eye_service_snapshot(struct nyabula_core_snapshot_s *snapshot)
{
  int ret = nxmutex_lock(&g_eye_lock);
  if (ret < 0)
    {
      return ret;
    }
  ret = g_eye_control == NULL
            ? -ENODEV
            : nyabula_core_get_snapshot(g_eye_control, snapshot);
  nxmutex_unlock(&g_eye_lock);
  return ret;
}
