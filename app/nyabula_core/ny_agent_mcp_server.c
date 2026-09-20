/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_mcp_server.c
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

/* Deliberately local: pre-provisioned independent credentials over loopback
 * or a trusted tunnel. This is not a public OAuth/TLS authorization server.
 */
#include "ny_agent_mcp_in.h"
#include "ny_product_store.h"
#include "ny_utf8.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <nuttx/mutex.h>
#include <poll.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define NY_IN_HTTP_HEADER   4096
#define NY_IN_HTTP_BODY     8192
#define NY_IN_HTTP_REPLY    32768
#define NY_IN_HTTP_DEADLINE 5000

static mutex_t g_server_lock = NXMUTEX_INITIALIZER;
static bool g_server_running;
static bool g_server_stop;
static unsigned int g_server_port;

static bool ny_in_server_stopping(void);
static int ny_in_http_io(int fd, char *buffer, size_t length, bool write,
                         uint64_t deadline);
static int ny_in_http_reply(int fd, int status, const cJSON *reply,
                            uint64_t deadline);
static bool ny_in_http_host(const char *value);
static int ny_in_http_client(int fd);

/****************************************************************************
 * Name: ny_mcp_in_listener
 ****************************************************************************/

unsigned int ny_mcp_in_listener(void)
{
  nxmutex_lock(&g_server_lock);
  unsigned int port = g_server_running && !g_server_stop ? g_server_port : 0;
  nxmutex_unlock(&g_server_lock);
  return port;
}

/****************************************************************************
 * Name: ny_in_server_stopping
 ****************************************************************************/

static bool ny_in_server_stopping(void)
{
  nxmutex_lock(&g_server_lock);
  bool stopping = g_server_stop;
  nxmutex_unlock(&g_server_lock);
  return stopping;
}

/****************************************************************************
 * Name: ny_in_http_io
 ****************************************************************************/

static int ny_in_http_io(int fd, char *buffer, size_t length, bool write,
                         uint64_t deadline)
{
  size_t offset = 0;
  while (offset < length)
    {
      if (ny_in_server_stopping())
        return -ECANCELED;
      if (ny_product_time_ms(true) >= deadline)
        return -ETIMEDOUT;
      ssize_t count = write ? send(fd, buffer + offset, length - offset, 0)
                            : recv(fd, buffer + offset, length - offset, 0);
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0 && errno == EAGAIN)
        {
          struct pollfd pfd = { fd, write ? POLLOUT : POLLIN, 0 };
          int ready = poll(&pfd, 1, 100);
          if (ready < 0 && errno != EINTR)
            return -errno;
          if (ready > 0 && !(pfd.revents & pfd.events))
            return -EIO;
          continue;
        }
      if (count <= 0)
        return count == 0 ? -ECONNRESET : -errno;
      offset += count;
    }
  return 0;
}

/****************************************************************************
 * Name: ny_in_http_reply
 ****************************************************************************/

static int ny_in_http_reply(int fd, int status, const cJSON *reply,
                            uint64_t deadline)
{
  char *body = reply ? cJSON_PrintUnformatted(reply) : NULL;
  if (reply && !body)
    status = 500;
  if (body && strlen(body) > NY_IN_HTTP_REPLY)
    {
      free(body);
      body = NULL;
      status = 500;
    }
  const char *reason = status == 200   ? "OK"
                       : status == 202 ? "Accepted"
                       : status == 401 ? "Unauthorized"
                       : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed"
                       : status == 503 ? "Service Unavailable"
                       : status == 500 ? "Internal Server Error"
                                       : "Bad Request";
  char header[384];
  int count = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
      "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
      "%s\r\n",
      status, reason, body ? strlen(body) : 0,
      status == 401   ? "WWW-Authenticate: Bearer realm=\"nyabot-local\"\r\n"
      : status == 405 ? "Allow: POST\r\n"
                      : "");
  int ret = count < 0 || (size_t)count >= sizeof(header)
                ? -E2BIG
                : ny_in_http_io(fd, header, count, true, deadline);
  if (ret == 0 && body)
    ret = ny_in_http_io(fd, body, strlen(body), true, deadline);
  free(body);
  return ret;
}

/****************************************************************************
 * Name: ny_in_http_host
 ****************************************************************************/

static bool ny_in_http_host(const char *value)
{
  const char *port;
  if (!strncmp(value, "127.0.0.1:", 10))
    port = value + 10;
  else if (!strncmp(value, "localhost:", 10))
    port = value + 10;
  else
    return false;
  if (!*port || strlen(port) > 5)
    return false;
  for (const char *p = port; *p; p++)
    if (*p < '0' || *p > '9')
      return false;
  long number = strtol(port, NULL, 10);
  return number > 0 && number <= 65535;
}

/****************************************************************************
 * Name: ny_in_http_client
 ****************************************************************************/

static int ny_in_http_client(int fd)
{
  uint64_t deadline = ny_product_time_ms(true) + NY_IN_HTTP_DEADLINE;
  char *header = calloc(1, NY_IN_HTTP_HEADER + 1);
  char *body = NULL;
  cJSON *request = NULL;
  cJSON *reply = NULL;
  int status = 400;
  int ret = 0;
  if (!header)
    return -ENOMEM;
  size_t used = 0;
  size_t header_length = 0;
  while (used < NY_IN_HTTP_HEADER)
    {
      if (ny_in_server_stopping() || ny_product_time_ms(true) >= deadline)
        {
          ret = -ETIMEDOUT;
          goto out;
        }
      struct pollfd pfd = { fd, POLLIN, 0 };
      int ready = poll(&pfd, 1, 100);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0 || (ready > 0 && !(pfd.revents & POLLIN)))
        {
          ret = -EIO;
          goto out;
        }
      if (!ready)
        continue;
      ssize_t chunk =
          recv(fd, header + used, NY_IN_HTTP_HEADER - used, MSG_DONTWAIT);
      if (chunk < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      if (chunk <= 0)
        {
          ret = -ECONNRESET;
          goto out;
        }
      used += chunk;
      char *marker = strstr(header, "\r\n\r\n");
      if (marker)
        {
          header_length = marker + 4 - header;
          break;
        }
    }
  if (!header_length)
    goto respond;
  for (size_t i = 0; i < header_length; i++)
    if (!header[i] || (unsigned char)header[i] > 127)
      goto respond;
  char *line = strstr(header, "\r\n");
  if (!line)
    goto respond;
  *line = '\0';
  bool post = !strcmp(header, "POST /mcp HTTP/1.1");
  bool get = !strcmp(header, "GET /mcp HTTP/1.1");
  bool remove = !strcmp(header, "DELETE /mcp HTTP/1.1");
  if (!post && !get && !remove)
    {
      status = 404;
      goto respond;
    }
  char *host = NULL, *authorization = NULL, *length = NULL;
  char *type = NULL, *accept = NULL, *version = NULL;
  char *next = line + 2;
  while (*next && strncmp(next, "\r\n", 2))
    {
      char *end = strstr(next, "\r\n");
      char *colon = strchr(next, ':');
      if (!end || !colon || colon >= end || colon == next || *next == ' ' ||
          *next == '\t')
        goto respond;
      *end = '\0';
      *colon = '\0';
      for (char *p = next; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-'))
          goto respond;
      char *value = colon + 1;
      while (*value == ' ' || *value == '\t')
        value++;
      for (char *p = value; *p; p++)
        if ((unsigned char)*p < 32 || *p == 127)
          goto respond;
      char **target = NULL;
      if (!strcasecmp(next, "Host"))
        target = &host;
      else if (!strcasecmp(next, "Authorization"))
        target = &authorization;
      else if (!strcasecmp(next, "Content-Length"))
        target = &length;
      else if (!strcasecmp(next, "Content-Type"))
        target = &type;
      else if (!strcasecmp(next, "Accept"))
        target = &accept;
      else if (!strcasecmp(next, "MCP-Protocol-Version"))
        target = &version;
      else if (!strcasecmp(next, "Origin"))
        {
          status = 403;
          goto respond;
        }
      else if (!strcasecmp(next, "Transfer-Encoding") ||
               !strcasecmp(next, "Expect") ||
               !strcasecmp(next, "MCP-Session-Id"))
        goto respond;
      if (target)
        {
          if (*target)
            goto respond;
          *target = value;
        }
      next = end + 2;
    }
  if (!host || !ny_in_http_host(host))
    {
      status = 403;
      goto respond;
    }
  if (!authorization || strncmp(authorization, "Bearer ", 7) ||
      strlen(authorization + 7) != 64)
    {
      status = 401;
      goto respond;
    }
  if (version && strcmp(version, "2025-11-25"))
    goto respond;
  if (!post)
    {
      request =
          cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"ping\"}");
      status = ny_mcp_in_rpc(authorization + 7, request, &reply);
      cJSON_Delete(reply);
      reply = NULL;
      if (status == 200)
        status = 405;
      goto respond;
    }
  if (!length || !*length || strlen(length) > 5 || !type ||
      (strcmp(type, "application/json") &&
       strcmp(type, "application/json; charset=utf-8")) ||
      !accept || !strstr(accept, "application/json") ||
      !strstr(accept, "text/event-stream"))
    goto respond;
  for (const char *p = length; *p; p++)
    if (*p < '0' || *p > '9')
      goto respond;
  long count = strtol(length, NULL, 10);
  if (count < 1 || count > NY_IN_HTTP_BODY)
    goto respond;
  body = calloc(1, count + 1);
  if (!body)
    {
      status = 500;
      goto respond;
    }
  size_t pending = used - header_length;
  if (pending > (size_t)count)
    goto respond;
  memcpy(body, header + header_length, pending);
  ret = ny_in_http_io(fd, body + pending, count - pending, false, deadline);
  if (ret < 0)
    goto out;
  if (!ny_utf8_valid((const unsigned char *)body, count) ||
      ny_product_json_check(body, count) < 0)
    goto respond;
  /* cJSON stores strings as C strings. Reject escaped NUL before parsing
   * so a request ID or object key cannot be silently truncated.
   */
  for (long i = 0; i + 1 < count; i++)
    if (body[i] == '\\')
      {
        if (i + 5 < count && !memcmp(body + i + 1, "u0000", 5))
          goto respond;
        i++;
      }
  const char *end;
  request = cJSON_ParseWithOpts(body, &end, true);
  if (!request)
    goto respond;
  const cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
  if ((!cJSON_IsString(method) || strcmp(method->valuestring, "initialize")) &&
      !version)
    goto respond;
  status = ny_mcp_in_rpc(authorization + 7, request, &reply);
respond:
  ret = ny_in_http_reply(fd, status, reply, deadline);
out:
  cJSON_Delete(request);
  cJSON_Delete(reply);
  if (header)
    sodium_memzero(header, NY_IN_HTTP_HEADER + 1);
  free(header);
  free(body);
  return ret;
}

/****************************************************************************
 * Name: ny_mcp_in_serve
 ****************************************************************************/

int ny_mcp_in_serve(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "stop"))
    {
      nxmutex_lock(&g_server_lock);
      g_server_stop = true;
      nxmutex_unlock(&g_server_lock);
      return 0;
    }
  if (argc != 2)
    {
      fprintf(stderr, "Usage: nyabula_mcp <loopback-port|stop>\n");
      return 1;
    }
  char *end;
  long port = strtol(argv[1], &end, 10);
  if (!argv[1][0] || *end || port < 1024 || port > 65535)
    return 1;
  nxmutex_lock(&g_server_lock);
  if (g_server_running)
    {
      nxmutex_unlock(&g_server_lock);
      return 1;
    }
  g_server_running = true;
  g_server_stop = false;
  g_server_port = port;
  nxmutex_unlock(&g_server_lock);
  int server = socket(AF_INET, SOCK_STREAM, 0);
  int ret = -errno;
  if (server >= 0)
    {
      struct sockaddr_in address = { .sin_family = AF_INET,
                                     .sin_port = htons(port),
                                     .sin_addr.s_addr =
                                         htonl(INADDR_LOOPBACK) };
      int reuse = 1;
      setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
          listen(server, 4) < 0 || fcntl(server, F_SETFL, O_NONBLOCK) < 0)
        ret = -errno;
      else
        {
          ret = 0;
          printf(
              "Nyabot MCP: 127.0.0.1:%ld/mcp, scoped credentials required\n",
              port);
          while (!ny_in_server_stopping())
            {
              struct pollfd pfd = { server, POLLIN, 0 };
              int ready = poll(&pfd, 1, 100);
              if (ready < 0 && errno == EINTR)
                continue;
              if (ready < 0)
                {
                  ret = -errno;
                  break;
                }
              if (!ready || !(pfd.revents & POLLIN))
                continue;
              int client = accept(server, NULL, NULL);
              if (client < 0)
                continue;
              if (fcntl(client, F_SETFL, O_NONBLOCK) == 0)
                ny_in_http_client(client);
              close(client);
            }
        }
      close(server);
    }
  nxmutex_lock(&g_server_lock);
  g_server_running = false;
  g_server_port = 0;
  nxmutex_unlock(&g_server_lock);
  return ret < 0 ? 1 : 0;
}
