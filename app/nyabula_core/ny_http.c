/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_http.c
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

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <netutils/netlib.h>
#include <netutils/webclient.h>

#include "ny_http.h"

#define NY_HTTP_URL_LIMIT     256
#define NY_HTTP_BUFFER_SIZE   1024
#define NY_HTTP_TIMEOUT_LIMIT 60000

struct ny_http_s
{
  struct webclient_context context;
  pthread_t owner;
  uint64_t deadline_ns;
  bool done;
  bool pending;
  int result;
  unsigned int status;
  size_t length;
  char url[NY_HTTP_URL_LIMIT];
  char buffer[NY_HTTP_BUFFER_SIZE];
  unsigned char body[NY_HTTP_BODY_LIMIT];
};

static uint64_t ny_http_now(void);
static bool ny_http_hostname(const char *host);
static int ny_http_url(const char *url);
static int ny_http_sink(char **buffer, int offset, int end, int *length,
                        void *opaque);
static int ny_http_header(const char *line, bool truncated, void *opaque);

static uint64_t ny_http_now(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

static bool ny_http_hostname(const char *host)
{
  size_t index;
  size_t length = strlen(host);

  if (length == 0 || host[0] == '.' || host[length - 1] == '.' ||
      host[0] == '-' || host[length - 1] == '-')
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      unsigned char character = host[index];

      if (character == '.')
        {
          if (host[index - 1] == '.' || host[index + 1] == '.' ||
              host[index - 1] == '-' || host[index + 1] == '-')
            {
              return false;
            }
        }
      else if (!isalnum(character) && character != '-')
        {
          return false;
        }
    }

  return true;
}

static int ny_http_url(const char *url)
{
  struct url_s parsed;
  struct in_addr address;
  char scheme[8];
  char host[64];
  char path[NY_HTTP_URL_LIMIT];
  const char *authority;
  const char *end;
  const char *port;
  uint32_t number = 0;
  size_t index;
  int ret;

  if (url == NULL || strlen(url) >= NY_HTTP_URL_LIMIT)
    {
      return -EINVAL;
    }
  for (index = 0; url[index] != '\0'; index++)
    {
      if ((unsigned char)url[index] <= 32 || (unsigned char)url[index] >= 127)
        {
          return -EINVAL;
        }
    }
  memset(&parsed, 0, sizeof(parsed));
  parsed.scheme = scheme;
  parsed.schemelen = sizeof(scheme);
  parsed.host = host;
  parsed.hostlen = sizeof(host);
  parsed.path = path;
  parsed.pathlen = sizeof(path);
  ret = netlib_parseurl(url, &parsed);
  if (ret < 0)
    {
      return ret;
    }

  /* Upstream non-blocking webclient resolves names synchronously before the
   * socket state machine starts. DNS is therefore bounded by the resolver,
   * not this request deadline. HTTPS and redirects remain unsupported.
   */

  if (strcmp(scheme, "http") != 0)
    {
      return -ENOTSUP;
    }

  if (inet_pton(AF_INET, host, &address) != 1 && !ny_http_hostname(host))
    {
      return -EINVAL;
    }

  /* The upstream parser accumulates ports in uint16_t without overflow
   * rejection. Reject ambiguous endpoints before opening a connection.
   */

  authority = strstr(url, "://") + 3;
  end = authority + strcspn(authority, "/");
  port = memchr(authority, ':', end - authority);
  if (port != NULL)
    {
      for (port++; port < end; port++)
        {
          if (*port < '0' || *port > '9')
            {
              return -EINVAL;
            }
          number = number * 10 + *port - '0';
          if (number > UINT16_MAX)
            {
              return -EINVAL;
            }
        }
      if (number == 0)
        {
          return -EINVAL;
        }
    }
  return strchr(url, '#') == NULL ? 0 : -EINVAL;
}

static int ny_http_sink(char **buffer, int offset, int end, int *length,
                        void *opaque)
{
  struct ny_http_s *request = opaque;
  size_t count;
  if (offset < 0 || end < offset || end > *length)
    {
      return -EPROTO;
    }
  count = end - offset;
  if (count > sizeof(request->body) - request->length)
    {
      return -EFBIG;
    }
  memcpy(request->body + request->length, *buffer + offset, count);
  request->length += count;
  return 0;
}

static int ny_http_header(const char *line, bool truncated, void *opaque)
{
  if (truncated)
    {
      return -EFBIG;
    }
  return strncasecmp(line, "Location:", 9) == 0 ? -ENOTSUP : 0;
}

int ny_http_open(const char *url, uint32_t timeout_ms, struct ny_http_s **out)
{
  struct ny_http_s *request;
  int ret;
  if (out == NULL)
    {
      return -EINVAL;
    }
  *out = NULL;
  ret = ny_http_url(url);
  if (ret < 0 || timeout_ms == 0 || timeout_ms > NY_HTTP_TIMEOUT_LIMIT)
    {
      return ret < 0 ? ret : -EINVAL;
    }
  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      return -ENOMEM;
    }
  request->owner = pthread_self();
  request->deadline_ns = ny_http_now() + (uint64_t)timeout_ms * 1000000ull;
  strlcpy(request->url, url, sizeof(request->url));
  webclient_set_defaults(&request->context);
  request->context.url = request->url;
  request->context.buffer = request->buffer;
  request->context.buflen = sizeof(request->buffer);
  request->context.flags = WEBCLIENT_FLAG_NON_BLOCKING;
  request->context.sink_callback = ny_http_sink;
  request->context.sink_callback_arg = request;
  request->context.header_callback = ny_http_header;
  request->context.header_callback_arg = request;
  *out = request;
  return 0;
}

int ny_http_step(struct ny_http_s *request, unsigned int *status,
                 const void **body, size_t *length)
{
  if (status == NULL || body == NULL || length == NULL)
    {
      return -EINVAL;
    }
  *status = 0;
  *body = NULL;
  *length = 0;
  if (request == NULL)
    {
      return -EINVAL;
    }
  if (!pthread_equal(request->owner, pthread_self()))
    {
      return -EPERM;
    }
  if (!request->done)
    {
      if (ny_http_now() < request->deadline_ns)
        {
          request->result = webclient_perform(&request->context);
          request->pending = request->result == -EAGAIN;
        }
      if (ny_http_now() >= request->deadline_ns)
        {
          request->result = -ETIMEDOUT;
        }
      if (request->result == -EAGAIN)
        {
          return -EAGAIN;
        }
      request->status = request->context.http_status;
      request->done = true;
      if (request->pending)
        {
          webclient_abort(&request->context);
          request->pending = false;
        }
    }
  if (request->result == 0)
    {
      *status = request->status;
      *body = request->body;
      *length = request->length;
    }
  return request->result;
}

int ny_http_cancel(struct ny_http_s *request)
{
  if (request == NULL)
    {
      return -EINVAL;
    }
  if (!pthread_equal(request->owner, pthread_self()))
    {
      return -EPERM;
    }
  if (!request->done)
    {
      if (request->pending)
        {
          webclient_abort(&request->context);
          request->pending = false;
        }
      request->done = true;
      request->result = -ECANCELED;
    }
  return 0;
}

int ny_http_close(struct ny_http_s *request)
{
  int ret;
  if (request == NULL)
    {
      return 0;
    }
  ret = ny_http_cancel(request);
  if (ret == 0)
    {
      free(request);
    }
  return ret;
}
