/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_provider.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "ny_provider.h"
#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_provider_lock = NXMUTEX_INITIALIZER;
static const struct ny_provider_ops_s *g_provider_ops;
static void *g_provider_context;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_provider_register(const struct ny_provider_ops_s *ops, void *context)
{
  int ret;

  if (ops == NULL || ops->ui_notify == NULL || ops->ai_invoke == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_provider_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_provider_ops != NULL)
    {
      nxmutex_unlock(&g_provider_lock);
      return -EBUSY;
    }

  g_provider_context = context;
  g_provider_ops = ops;
  nxmutex_unlock(&g_provider_lock);
  return 0;
}

int ny_provider_ui_notify(const char *plugin_id, const char *message,
                          size_t length)
{
  if (g_provider_ops != NULL)
    {
      return g_provider_ops->ui_notify(g_provider_context, plugin_id, message,
                                       length);
    }

#ifdef CONFIG_NYABULA_CORE_EYE
  return nyabula_eye_service_notify(plugin_id, message, length);
#endif

#ifdef CONFIG_NYABULA_CORE_MOCK_CAPABILITIES
  if (length > INT_MAX)
    {
      return -EFBIG;
    }

  printf("nymock-ui[%s]: %.*s\n", plugin_id, (int)length, message);
  return 0;
#else
  return -ENOSYS;
#endif
}

int ny_provider_ai_invoke(const char *plugin_id, const char *prompt,
                          size_t prompt_length, char *response,
                          size_t response_capacity)
{
  if (g_provider_ops != NULL)
    {
      return g_provider_ops->ai_invoke(g_provider_context, plugin_id, prompt,
                                       prompt_length, response,
                                       response_capacity);
    }

#ifdef CONFIG_NYABULA_CORE_MOCK_CAPABILITIES
  if (prompt_length > response_capacity || prompt_length > INT_MAX)
    {
      return -ENOSPC;
    }

  memcpy(response, prompt, prompt_length);
  return (int)prompt_length;
#else
  return -ENOSYS;
#endif
}
