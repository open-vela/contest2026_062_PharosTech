/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_mcp_wire.c
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

#include "ny_agent_mcp_wire.h"
#include "infra/vela_tls.h"
#include "ny_product_store.h"
#include <errno.h>
#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct ny_mcp_event_s
{
  const char *id;
  cJSON *result;
  int error;
};

static bool ny_mcp_ascii(const char *value, size_t maximum);
static int ny_mcp_envelope(const char *body, size_t length,
                           struct ny_mcp_event_s *event);
static int ny_mcp_event(void *context, const char *body, size_t length);
static int ny_mcp_exchange(struct ny_mcp_peer_s *peer, const char *method,
                           const cJSON *params, bool notification,
                           cJSON **result);
static int ny_mcp_list(struct ny_mcp_peer_s *peer, const char *kind,
                       cJSON *catalog);

/****************************************************************************
 * Name: ny_mcp_ascii
 ****************************************************************************/

static bool ny_mcp_ascii(const char *value, size_t maximum)
{
  if (!value || !value[0] || strlen(value) > maximum)
    return false;
  for (const unsigned char *p = (const unsigned char *)value; *p; p++)
    if (*p < 33 || *p > 126)
      return false;
  return true;
}

/****************************************************************************
 * Name: ny_mcp_peer_configure
 ****************************************************************************/

int ny_mcp_peer_configure(struct ny_mcp_peer_s *peer, const char *url,
                          const char *secret)
{
  if (!peer || !ny_mcp_ascii(url, 768) || !secret ||
      (secret[0] && !ny_mcp_ascii(secret, 512)))
    return -EINVAL;
  memset(peer, 0, sizeof(*peer));
  const char *host;
  if (strncmp(url, "https://", 8) == 0)
    host = url + 8;
#ifdef CONFIG_AI_AGENT_SIM_HTTP_FIXTURE
  else if (strncmp(url, "http://127.0.0.1:", 17) == 0)
    {
      host = url + 7;
      peer->fixture = true;
    }
#endif
  else
    return -EPERM;
  const char *path = strchr(host, '/');
  size_t authority = path ? (size_t)(path - host) : strlen(host);
  const char *colon = memchr(host, ':', authority);
  size_t host_length = colon ? (size_t)(colon - host) : authority;
  if (!host_length || host_length >= sizeof(peer->host))
    return -EINVAL;
  for (size_t i = 0; i < host_length; i++)
    if (!((host[i] >= 'A' && host[i] <= 'Z') ||
          (host[i] >= 'a' && host[i] <= 'z') ||
          (host[i] >= '0' && host[i] <= '9') || host[i] == '.' ||
          host[i] == '-'))
      return -EINVAL;
  memcpy(peer->host, host, host_length);
  if (colon)
    {
      size_t length = authority - host_length - 1;
      if (!length || length >= sizeof(peer->port))
        return -EINVAL;
      memcpy(peer->port, colon + 1, length);
      for (size_t i = 0; i < length; i++)
        if (peer->port[i] < '0' || peer->port[i] > '9')
          return -EINVAL;
      if (atoi(peer->port) < 1 || atoi(peer->port) > 65535)
        return -EINVAL;
    }
  else
    strcpy(peer->port, "443");
  if (path && (strlen(path) >= sizeof(peer->path) || strchr(path, '#')))
    return -EINVAL;
  strcpy(peer->path, path ? path : "/");
  if (secret[0])
    snprintf(peer->bearer, sizeof(peer->bearer), "Bearer %s", secret);
  return 0;
}

/****************************************************************************
 * Name: ny_mcp_envelope
 ****************************************************************************/

static int ny_mcp_envelope(const char *body, size_t length,
                           struct ny_mcp_event_s *event)
{
  if (ny_product_json_check(body, length) < 0)
    return -EBADMSG;
  char *copy = malloc(length + 1);
  if (!copy)
    return -ENOMEM;
  memcpy(copy, body, length);
  copy[length] = 0;
  cJSON *root = cJSON_ParseWithOpts(copy, NULL, true);
  free(copy);
  int ret = -EBADMSG;
  const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "jsonrpc");
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "result");
  const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
  if (!cJSON_IsObject(root) || !cJSON_IsString(version) ||
      strcmp(version->valuestring, "2.0"))
    goto done;
  /* Ambiguous duplicate envelope fields are never accepted. */
  const cJSON *item;
  cJSON_ArrayForEach(item, root)
  {
    const cJSON *other;
    for (other = item->next; other; other = other->next)
      if (!strcmp(item->string, other->string))
        goto done;
  }
  if (!id &&
      cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "method")) &&
      !value && !error)
    {
      ret = 0;
      goto done;
    }
  if (!cJSON_IsString(id) || strcmp(id->valuestring, event->id) ||
      (!value == !error) || cJSON_GetObjectItemCaseSensitive(root, "method"))
    goto done;
  if (error)
    {
      ret = -EREMOTEIO;
      goto done;
    }
  event->result = cJSON_Duplicate(value, true);
  ret = event->result ? 1 : -ENOMEM;
done:
  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_mcp_event
 ****************************************************************************/

static int ny_mcp_event(void *context, const char *body, size_t length)
{
  struct ny_mcp_event_s *event = context;
  char *data = malloc(length + 1);
  if (!data)
    {
      event->error = -ENOMEM;
      return -1;
    }
  size_t position = 0;
  size_t used = 0;
  int ret = 0;
  while (position < length)
    {
      const char *newline = memchr(body + position, '\n', length - position);
      if (!newline)
        break;
      size_t line_length = (size_t)(newline - body - position);
      if (line_length && body[position + line_length - 1] == '\r')
        line_length--;
      if (!line_length && used)
        {
          data[used - 1] = 0;
          ret = ny_mcp_envelope(data, used - 1, event);
          if (ret != 0)
            break;
          used = 0;
        }
      else if (line_length >= 5 && !memcmp(body + position, "data:", 5))
        {
          size_t skip = line_length > 5 && body[position + 5] == ' ' ? 6 : 5;
          memcpy(data + used, body + position + skip, line_length - skip);
          used += line_length - skip;
          data[used++] = '\n';
        }
      position = (size_t)(newline - body) + 1;
    }
  free(data);
  if (ret < 0)
    event->error = ret;
  return ret;
}

/****************************************************************************
 * Name: ny_mcp_exchange
 ****************************************************************************/

static int ny_mcp_exchange(struct ny_mcp_peer_s *peer, const char *method,
                           const cJSON *params, bool notification,
                           cJSON **result)
{
  char id[32];
  snprintf(id, sizeof(id), "ny-%u", ++peer->sequence);
  struct ny_mcp_event_s event = { .id = id };
  cJSON *request = cJSON_CreateObject();
  cJSON *arguments =
      params ? cJSON_Duplicate(params, true) : cJSON_CreateObject();
  bool valid = request && arguments;
  valid &= cJSON_AddStringToObject(request, "jsonrpc", "2.0") != NULL;
  valid &= cJSON_AddStringToObject(request, "method", method) != NULL;
  if (!notification)
    valid &= cJSON_AddStringToObject(request, "id", id) != NULL;
  if (!arguments || !cJSON_AddItemToObject(request, "params", arguments))
    {
      cJSON_Delete(arguments);
      valid = false;
    }
  char *encoded = valid ? cJSON_PrintUnformatted(request) : NULL;
  cJSON_Delete(request);
  if (!encoded)
    return -ENOMEM;
  if (strlen(encoded) > 8192)
    {
      free(encoded);
      return -E2BIG;
    }
  char *body = malloc(NY_MCP_WIRE_CAPACITY);
  if (!body)
    {
      free(encoded);
      return -ENOMEM;
    }
  vela_header_t headers[7];
  size_t n = 0;
  headers[n++] = (vela_header_t){ "Content-Type", "application/json" };
  headers[n++] =
      (vela_header_t){ "Accept", "application/json, text/event-stream" };
  headers[n++] = (vela_header_t){ "MCP-Protocol-Version", NY_MCP_PROTOCOL };
  if (peer->bearer[0])
    headers[n++] = (vela_header_t){ "Authorization", peer->bearer };
  if (peer->session[0])
    headers[n++] = (vela_header_t){ "Mcp-Session-Id", peer->session };
  headers[n] = (vela_header_t){ NULL, NULL };
  struct vela_http_response_s response;
  int status;
#ifdef CONFIG_AI_AGENT_SIM_HTTP_FIXTURE
  if (peer->fixture)
    status = vela_http_loopback_post_once(
        peer->port, peer->path, headers, encoded, body, NY_MCP_WIRE_CAPACITY,
        &response, notification ? NULL : ny_mcp_event, &event);
  else
#endif
    status =
        vela_https_request_once(peer->host, peer->port, peer->path, headers,
                                encoded, body, NY_MCP_WIRE_CAPACITY, &response,
                                notification ? NULL : ny_mcp_event, &event);
  free(encoded);
  int ret = status < 0 ? (event.error ? event.error : -EIO) : -EPROTO;
  if (notification)
    {
      if (status == 202 && response.body_length == 0)
        ret = 0;
    }
  else if (status == 200)
    {
      if (!strncasecmp(response.content_type, "application/json", 16) &&
          (!response.content_type[16] || response.content_type[16] == ';'))
        {
          ret = ny_mcp_envelope(body, response.body_length, &event);
          ret = ret == 1 ? 0 : ret < 0 ? ret : -EPROTO;
        }
      else if (event.result)
        ret = 0;
    }
  if (ret == 0 && response.session_id[0])
    {
      if (peer->session[0] && strcmp(peer->session, response.session_id))
        ret = -EPROTO;
      else
        strcpy(peer->session, response.session_id);
    }
  free(body);
  if (ret == 0 && result)
    *result = event.result;
  else
    cJSON_Delete(event.result);
  return ret;
}

/****************************************************************************
 * Name: ny_mcp_peer_request
 ****************************************************************************/

int ny_mcp_peer_request(struct ny_mcp_peer_s *peer, const char *method,
                        const cJSON *params, cJSON **result)
{
  *result = NULL;
  return ny_mcp_exchange(peer, method, params, false, result);
}

/****************************************************************************
 * Name: ny_mcp_peer_initialize
 ****************************************************************************/

int ny_mcp_peer_initialize(struct ny_mcp_peer_s *peer)
{
  const char *json =
      "{\"protocolVersion\":\"" NY_MCP_PROTOCOL "\","
      "\"capabilities\":{},\"clientInfo\":{\"name\":\"Nyabula Core\","
      "\"version\":\"1\"}}";
  cJSON *params = cJSON_Parse(json);
  cJSON *result = NULL;
  int ret = params ? ny_mcp_peer_request(peer, "initialize", params, &result)
                   : -ENOMEM;
  cJSON_Delete(params);
  const cJSON *version =
      cJSON_GetObjectItemCaseSensitive(result, "protocolVersion");
  const cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
  if (ret == 0 &&
      (!cJSON_IsString(version) ||
       strcmp(version->valuestring, NY_MCP_PROTOCOL) || !cJSON_IsObject(caps)))
    ret = -EPROTONOSUPPORT;
  if (ret == 0)
    {
      peer->tools =
          cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "tools"));
      peer->resources =
          cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "resources"));
      peer->prompts =
          cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(caps, "prompts"));
      ret =
          ny_mcp_exchange(peer, "notifications/initialized", NULL, true, NULL);
    }
  cJSON_Delete(result);
  return ret;
}

/****************************************************************************
 * Name: ny_mcp_list
 ****************************************************************************/

static int ny_mcp_list(struct ny_mcp_peer_s *peer, const char *kind,
                       cJSON *catalog)
{
  char method[32];
  char cursor[257] = { 0 };
  snprintf(method, sizeof(method), "%s/list", kind);
  cJSON *items = cJSON_AddArrayToObject(catalog, kind);
  if (!items)
    return -ENOMEM;
  bool enabled = !strcmp(kind, "tools")       ? peer->tools
                 : !strcmp(kind, "resources") ? peer->resources
                                              : peer->prompts;
  if (!enabled)
    return 0;
  for (unsigned int page = 0; page < 4; page++)
    {
      cJSON *params = cJSON_CreateObject();
      cJSON *response = NULL;
      if (!params)
        return -ENOMEM;
      if (cursor[0] && !cJSON_AddStringToObject(params, "cursor", cursor))
        {
          cJSON_Delete(params);
          return -ENOMEM;
        }
      int ret = ny_mcp_peer_request(peer, method, params, &response);
      cJSON_Delete(params);
      const cJSON *rows = cJSON_GetObjectItemCaseSensitive(response, kind);
      if (ret == 0 && !cJSON_IsArray(rows))
        ret = -EBADMSG;
      const cJSON *row;
      if (ret == 0)
        cJSON_ArrayForEach(row, rows)
        {
          if (!cJSON_IsObject(row) || cJSON_GetArraySize(items) >= 32)
            {
              ret = -E2BIG;
              break;
            }
          const char *key = !strcmp(kind, "resources") ? "uri" : "name";
          const cJSON *name = cJSON_GetObjectItemCaseSensitive(row, key);
          if (!cJSON_IsString(name) || !ny_mcp_ascii(name->valuestring, 256))
            {
              ret = -EBADMSG;
              break;
            }
          const cJSON *prior;
          cJSON_ArrayForEach(
              prior,
              items) if (!strcmp(cJSON_GetObjectItemCaseSensitive(prior, key)
                                     ->valuestring,
                                 name->valuestring)) ret = -EBADMSG;
          if (ret < 0)
            break;
          if (!strcmp(kind, "tools") &&
              !cJSON_IsObject(
                  cJSON_GetObjectItemCaseSensitive(row, "inputSchema")))
            {
              ret = -EBADMSG;
              break;
            }
          cJSON *copy = cJSON_Duplicate(row, true);
          if (!copy || !cJSON_AddItemToArray(items, copy))
            {
              cJSON_Delete(copy);
              ret = -ENOMEM;
              break;
            }
        }
      const cJSON *next =
          cJSON_GetObjectItemCaseSensitive(response, "nextCursor");
      bool more = next != NULL;
      if (ret == 0 && more)
        {
          if (!cJSON_IsString(next) || !ny_mcp_ascii(next->valuestring, 256) ||
              !strcmp(cursor, next->valuestring))
            ret = -EBADMSG;
          else
            strcpy(cursor, next->valuestring);
        }
      cJSON_Delete(response);
      if (ret < 0 || !more)
        return ret;
    }
  return -E2BIG;
}

/****************************************************************************
 * Name: ny_mcp_peer_catalog
 ****************************************************************************/

int ny_mcp_peer_catalog(struct ny_mcp_peer_s *peer, cJSON **catalog)
{
  *catalog = cJSON_CreateObject();
  if (!*catalog)
    return -ENOMEM;
  int ret = ny_mcp_list(peer, "tools", *catalog);
  if (ret == 0)
    ret = ny_mcp_list(peer, "resources", *catalog);
  if (ret == 0)
    ret = ny_mcp_list(peer, "prompts", *catalog);
  char *encoded = ret == 0 ? cJSON_PrintUnformatted(*catalog) : NULL;
  if (ret == 0 && (!encoded || strlen(encoded) > 12000))
    ret = encoded ? -E2BIG : -ENOMEM;
  free(encoded);
  if (ret < 0)
    {
      cJSON_Delete(*catalog);
      *catalog = NULL;
    }
  return ret;
}
