/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_broker_http.c
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

#include <nuttx/config.h>

#include <errno.h>
#include <nuttx/mutex.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ny_broker.h"
#include "ny_http.h"
#include "ny_manifest.h"

#define NY_BROKER_HTTP_PER_PLUGIN 8
#define NY_BROKER_HTTP_TOTAL \
  (CONFIG_NYABULA_CORE_MAX_PLUGINS * NY_BROKER_HTTP_PER_PLUGIN)

struct ny_broker_http_s
{
  struct ny_broker_http_s *next;
  struct ny_http_s *transport;
  pthread_t owner;
  uint32_t permission_generation;
  bool revoked;
  char id[NY_PLUGIN_ID_SIZE];
};

static mutex_t g_http_lock = NXMUTEX_INITIALIZER;
static struct ny_broker_http_s *g_http_requests;

int ny_broker_http_open(const struct ny_broker_client_s *client,
                        uint32_t permission_generation, const char *url,
                        uint32_t timeout_ms, struct ny_broker_http_s **out)
{
  struct ny_broker_http_s *cursor;
  struct ny_broker_http_s *request;
  size_t total = 0;
  size_t count = 0;
  int ret;

  if (out == NULL)
    {
      return -EINVAL;
    }
  *out = NULL;
  if (client == NULL || client->id == NULL || client->id[0] == '\0' ||
      strlen(client->id) >= NY_PLUGIN_ID_SIZE ||
      (client->permissions & NY_PERMISSION_NETWORK_REQUEST) == 0)
    {
      return -EACCES;
    }

  ret = nxmutex_lock(&g_http_lock);
  if (ret < 0)
    {
      return ret;
    }
  for (cursor = g_http_requests; cursor != NULL; cursor = cursor->next)
    {
      total++;
      count += strcmp(cursor->id, client->id) == 0;
    }
  if (total >= NY_BROKER_HTTP_TOTAL || count >= NY_BROKER_HTTP_PER_PLUGIN)
    {
      nxmutex_unlock(&g_http_lock);
      return -EDQUOT;
    }
  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      nxmutex_unlock(&g_http_lock);
      return -ENOMEM;
    }
  ret = ny_http_open(url, timeout_ms, &request->transport);
  if (ret < 0)
    {
      free(request);
      nxmutex_unlock(&g_http_lock);
      return ret;
    }
  request->owner = pthread_self();
  request->permission_generation = permission_generation;
  strlcpy(request->id, client->id, sizeof(request->id));
  request->next = g_http_requests;
  g_http_requests = request;
  nxmutex_unlock(&g_http_lock);
  *out = request;
  return 0;
}

int ny_broker_http_step(struct ny_broker_http_s *request,
                        const struct ny_broker_client_s *client,
                        uint32_t permission_generation, unsigned int *status,
                        const void **body, size_t *length)
{
  if (status == NULL || body == NULL || length == NULL)
    {
      return -EINVAL;
    }
  *status = 0;
  *body = NULL;
  *length = 0;
  if (request == NULL || client == NULL || client->id == NULL)
    {
      return -EINVAL;
    }
  if (!pthread_equal(request->owner, pthread_self()))
    {
      return -EPERM;
    }
  if (strcmp(client->id, request->id) != 0)
    {
      return -EACCES;
    }
  if (request->revoked ||
      (client->permissions & NY_PERMISSION_NETWORK_REQUEST) == 0 ||
      permission_generation != request->permission_generation)
    {
      request->revoked = true;
      ny_http_cancel(request->transport);
      return -EACCES;
    }
  return ny_http_step(request->transport, status, body, length);
}

int ny_broker_http_close(struct ny_broker_http_s *request)
{
  struct ny_broker_http_s **cursor;
  int ret;
  if (request == NULL)
    {
      return 0;
    }
  if (!pthread_equal(request->owner, pthread_self()))
    {
      return -EPERM;
    }
  ret = nxmutex_lock(&g_http_lock);
  if (ret < 0)
    {
      return ret;
    }
  ret = ny_http_close(request->transport);
  if (ret < 0)
    {
      nxmutex_unlock(&g_http_lock);
      return ret;
    }

  /* Keep the quota charged until transport resources have been released. */
  for (cursor = &g_http_requests; *cursor != NULL; cursor = &(*cursor)->next)
    {
      if (*cursor == request)
        {
          *cursor = request->next;
          break;
        }
    }
  nxmutex_unlock(&g_http_lock);
  free(request);
  return 0;
}
