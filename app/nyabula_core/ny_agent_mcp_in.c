/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_mcp_in.c
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

#include "ny_agent_mcp_in.h"
#include "ny_agent.h"
#include "ny_product_store.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NY_MCP_IN_CLIENTS     8
#define NY_MCP_IN_AUDIT       32
#define NY_MCP_IN_TOKEN       64
#define NY_MCP_IN_MIN_TIME    1577836800000.0
#define NY_MCP_IN_MAX_TIME    4102444800000.0
#define NY_MCP_IN_MAX_TTL     2592000000.0
#define NY_MCP_IN_MAX_INTEGER 9007199254740991.0

struct ny_mcp_in_tool_s
{
  const char *name;
  const char *topic;
  const char *description;
};

static const struct ny_mcp_in_tool_s g_in_tools[] = {
  { "nyabula_time", "system.time.get", "Read device time." },
  { "nyabula_system", "sys.info", "Read device system and memory status." },
  { "nyabula_timers", "timer.list", "Read all device timers." },
  { "nyabula_tasks", "task.list", "Read all personal task records." },
  { "nyabula_calendar", "calendar.list",
    "Read all personal calendar records." },
  { "nyabula_chat", "agent.chat",
    "Start an isolated model conversation. Uses the device model account." },
  { "nyabula_run", "agent.run.get", "Read only this client's model run." },
  { "nyabula_cancel", "agent.cancel", "Cancel only this client's model run." }
};
static mutex_t g_in_lock = NXMUTEX_INITIALIZER;

static const char *ny_in_string(const cJSON *object, const char *key);
static int ny_in_load(cJSON **root, uint64_t *revision);
static bool ny_in_revision(const cJSON *data, uint64_t revision);
static cJSON *ny_in_find(cJSON *items, const char *id);
static int ny_in_public(const cJSON *root, uint64_t revision, cJSON **result);
static int ny_in_digest(const char *token, char hash[65]);
static bool ny_in_scope(const cJSON *client, const char *name);
static bool ny_in_id(const char *id);
static int ny_in_save(cJSON *root, const cJSON *data, bool local,
                      uint64_t revision);
static cJSON *ny_in_principal(cJSON *root, const char *principal);
static cJSON *ny_in_envelope(const cJSON *id, cJSON *result, int error);
static cJSON *ny_in_tools(const cJSON *client);
static cJSON *ny_in_call(cJSON *root, cJSON *client, const cJSON *params,
                         uint64_t revision);

/****************************************************************************
 * Name: ny_in_string
 ****************************************************************************/

static const char *ny_in_string(const cJSON *object, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

/****************************************************************************
 * Name: ny_in_load
 ****************************************************************************/

static int ny_in_load(cJSON **root, uint64_t *revision)
{
  int ret = ny_product_store_read("mcp-in", root, revision);
  if (ret < 0)
    return ret;
  if (!*root)
    {
      *root = cJSON_Parse("{\"enabled\":false,\"clients\":[],\"audit\":[]}");
      if (!*root)
        return -ENOMEM;
    }
  return cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(*root, "enabled")) &&
                 cJSON_IsArray(
                     cJSON_GetObjectItemCaseSensitive(*root, "clients")) &&
                 cJSON_IsArray(
                     cJSON_GetObjectItemCaseSensitive(*root, "audit"))
             ? 0
             : -EBADMSG;
}

/****************************************************************************
 * Name: ny_in_revision
 ****************************************************************************/

static bool ny_in_revision(const cJSON *data, uint64_t revision)
{
  const cJSON *n = cJSON_GetObjectItemCaseSensitive(data, "revision");
  return cJSON_IsNumber(n) && isfinite(n->valuedouble) &&
         n->valuedouble >= 0 && n->valuedouble <= NY_MCP_IN_MAX_INTEGER &&
         n->valuedouble == (double)revision;
}

/****************************************************************************
 * Name: ny_in_find
 ****************************************************************************/

static cJSON *ny_in_find(cJSON *items, const char *id)
{
  cJSON *item;
  cJSON_ArrayForEach(
      item, items) if (!strcmp(ny_in_string(item, "id"), id)) return item;
  return NULL;
}

/****************************************************************************
 * Name: ny_in_public
 ****************************************************************************/

static int ny_in_public(const cJSON *root, uint64_t revision, cJSON **result)
{
  *result = cJSON_Duplicate(root, true);
  if (!*result)
    return -ENOMEM;
  cJSON *item;
  cJSON_ArrayForEach(item,
                     cJSON_GetObjectItemCaseSensitive(*result, "clients"))
      cJSON_DeleteItemFromObjectCaseSensitive(item, "hash");
  cJSON *available = cJSON_AddArrayToObject(*result, "available");
  if (!available || !cJSON_AddNumberToObject(*result, "revision", revision) ||
      !cJSON_AddStringToObject(*result, "transport", "loopback-tunnel-only") ||
      !cJSON_AddNumberToObject(*result, "listenerPort",
                               ny_mcp_in_listener()) ||
      !cJSON_AddBoolToObject(*result, "chatEnabled", true))
    return -ENOMEM;
  for (size_t i = 0; i < sizeof(g_in_tools) / sizeof(g_in_tools[0]); i++)
    {
      cJSON *entry = cJSON_CreateObject();
      if (!entry ||
          !cJSON_AddStringToObject(entry, "name", g_in_tools[i].name) ||
          !cJSON_AddStringToObject(entry, "description",
                                   g_in_tools[i].description) ||
          !cJSON_AddItemToArray(available, entry))
        {
          cJSON_Delete(entry);
          return -ENOMEM;
        }
    }
  return 0;
}

/****************************************************************************
 * Name: ny_in_digest
 ****************************************************************************/

static int ny_in_digest(const char *token, char hash[65])
{
  unsigned char bytes[crypto_hash_sha256_BYTES];
  if (!token || strlen(token) != NY_MCP_IN_TOKEN)
    return -EINVAL;
  for (size_t i = 0; i < NY_MCP_IN_TOKEN; i++)
    if (!((token[i] >= '0' && token[i] <= '9') ||
          (token[i] >= 'a' && token[i] <= 'f')))
      return -EINVAL;
  crypto_hash_sha256(bytes, (const unsigned char *)token, NY_MCP_IN_TOKEN);
  sodium_bin2hex(hash, 65, bytes, sizeof(bytes));
  sodium_memzero(bytes, sizeof(bytes));
  return 0;
}

/****************************************************************************
 * Name: ny_in_scope
 ****************************************************************************/

static bool ny_in_scope(const cJSON *client, const char *name)
{
  const cJSON *scope;
  cJSON_ArrayForEach(scope,
                     cJSON_GetObjectItemCaseSensitive(
                         client, "scopes")) if (cJSON_IsString(scope) &&
                                                !strcmp(scope->valuestring,
                                                        name)) return true;
  return false;
}

/****************************************************************************
 * Name: ny_in_principal
 ****************************************************************************/

static cJSON *ny_in_principal(cJSON *root, const char *principal)
{
  if (!principal || !principal[0] ||
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enabled")))
    return NULL;
  double now = ny_product_time_ms(false);
  cJSON *client;
  cJSON_ArrayForEach(client, cJSON_GetObjectItemCaseSensitive(root, "clients"))
  {
    const cJSON *expiry =
        cJSON_GetObjectItemCaseSensitive(client, "expiresAt");
    if (!strcmp(ny_in_string(client, "principal"), principal) &&
        ny_in_scope(client, "nyabula_chat") && cJSON_IsNumber(expiry) &&
        isfinite(expiry->valuedouble) && now >= NY_MCP_IN_MIN_TIME &&
        now < expiry->valuedouble)
      return client;
  }
  return NULL;
}

/****************************************************************************
 * Name: ny_mcp_in_active
 ****************************************************************************/

bool ny_mcp_in_active(const char *principal)
{
  nxmutex_lock(&g_in_lock);
  cJSON *root = NULL;
  uint64_t revision;
  bool active = ny_in_load(&root, &revision) == 0 &&
                ny_in_principal(root, principal) != NULL;
  cJSON_Delete(root);
  nxmutex_unlock(&g_in_lock);
  return active;
}

/****************************************************************************
 * Name: ny_mcp_in_model_tools
 ****************************************************************************/

char *ny_mcp_in_model_tools(const char *principal)
{
  nxmutex_lock(&g_in_lock);
  cJSON *root = NULL;
  uint64_t revision;
  cJSON *client = ny_in_load(&root, &revision) == 0
                      ? ny_in_principal(root, principal)
                      : NULL;
  cJSON *list = cJSON_Parse(
      "[{\"name\":\"nyabula_read\","
      "\"description\":\"Read explicitly granted device records.\","
      "\"input_schema\":{\"type\":\"object\",\"properties\":{"
      "\"topic\":{\"type\":\"string\",\"enum\":[]}},"
      "\"required\":[\"topic\"],\"additionalProperties\":false}}]");
  cJSON *schema = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, 0),
                                                   "input_schema");
  cJSON *topics = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(schema, "properties"), "topic"),
      "enum");
  bool valid = list && topics;
  for (size_t i = 0; valid && i < sizeof(g_in_tools) / sizeof(g_in_tools[0]);
       i++)
    if (strncmp(g_in_tools[i].topic, "agent.", 6) && client &&
        ny_in_scope(client, g_in_tools[i].name))
      {
        cJSON *topic = cJSON_CreateString(g_in_tools[i].topic);
        if (!topic || !cJSON_AddItemToArray(topics, topic))
          {
            cJSON_Delete(topic);
            valid = false;
          }
      }
  if (valid && !cJSON_GetArraySize(topics))
    cJSON_DeleteItemFromArray(list, 0);
  char *encoded = valid ? cJSON_PrintUnformatted(list) : NULL;
  cJSON_Delete(list);
  cJSON_Delete(root);
  nxmutex_unlock(&g_in_lock);
  return encoded;
}

/****************************************************************************
 * Name: ny_mcp_in_read
 ****************************************************************************/

int ny_mcp_in_read(const char *principal, const char *topic, cJSON **result)
{
  if (!result || !topic)
    return -EINVAL;
  *result = NULL;
  nxmutex_lock(&g_in_lock);
  cJSON *root = NULL;
  uint64_t revision;
  cJSON *client = ny_in_load(&root, &revision) == 0
                      ? ny_in_principal(root, principal)
                      : NULL;
  int ret = -EACCES;
  for (size_t i = 0; client && i < sizeof(g_in_tools) / sizeof(g_in_tools[0]);
       i++)
    if (strncmp(g_in_tools[i].topic, "agent.", 6) &&
        !strcmp(topic, g_in_tools[i].topic) &&
        ny_in_scope(client, g_in_tools[i].name))
      {
        cJSON *empty = cJSON_CreateObject();
        const struct ny_product_caller_s caller = { principal,
                                                    NY_PRODUCT_FAMILY, false };
        ret = empty ? ny_product_request(&caller, topic, empty, result)
                    : -ENOMEM;
        cJSON_Delete(empty);
        break;
      }
  cJSON_Delete(root);
  nxmutex_unlock(&g_in_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_in_id
 ****************************************************************************/

static bool ny_in_id(const char *id)
{
  if (!id[0] || strlen(id) > 40)
    return false;
  for (const char *p = id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
          *p == '_'))
      return false;
  return true;
}

/****************************************************************************
 * Name: ny_in_save
 ****************************************************************************/

static int ny_in_save(cJSON *root, const cJSON *data, bool local,
                      uint64_t revision)
{
  const char *id = ny_in_string(data, "id");
  const char *title = ny_in_string(data, "title");
  const cJSON *expires = cJSON_GetObjectItemCaseSensitive(data, "expiresAt");
  const cJSON *scopes = cJSON_GetObjectItemCaseSensitive(data, "scopes");
  const cJSON *token = cJSON_GetObjectItemCaseSensitive(data, "token");
  double now = ny_product_time_ms(false);
  if (!ny_in_id(id) || !title[0] || strlen(title) > 96 ||
      now < NY_MCP_IN_MIN_TIME || now > NY_MCP_IN_MAX_TIME ||
      !cJSON_IsNumber(expires) || !isfinite(expires->valuedouble) ||
      expires->valuedouble <= now ||
      expires->valuedouble > now + NY_MCP_IN_MAX_TTL ||
      floor(expires->valuedouble) != expires->valuedouble ||
      !cJSON_IsArray(scopes) ||
      cJSON_GetArraySize(scopes) >
          (int)(sizeof(g_in_tools) / sizeof(g_in_tools[0])))
    return -EINVAL;
  cJSON *clients = cJSON_GetObjectItemCaseSensitive(root, "clients");
  cJSON *old = ny_in_find(clients, id);
  if (!old && cJSON_GetArraySize(clients) >= NY_MCP_IN_CLIENTS)
    return -ENOSPC;
  char hash[65];
  if (token)
    {
      if (!local)
        return -EACCES;
      if (!cJSON_IsString(token) || ny_in_digest(token->valuestring, hash) < 0)
        return -EINVAL;
      cJSON *client;
      cJSON_ArrayForEach(client,
                         clients) if (client != old &&
                                      !strcmp(ny_in_string(client, "hash"),
                                              hash)) return -EEXIST;
    }
  else
    {
      if (!old || strlen(ny_in_string(old, "hash")) != 64)
        return -EINVAL;
      memcpy(hash, ny_in_string(old, "hash"), sizeof(hash));
    }
  const cJSON *scope;
  cJSON_ArrayForEach(scope, scopes)
  {
    bool known = false;
    if (!cJSON_IsString(scope))
      return -EINVAL;
    for (size_t i = 0; i < sizeof(g_in_tools) / sizeof(g_in_tools[0]); i++)
      known |= !strcmp(scope->valuestring, g_in_tools[i].name);
    if (!known)
      return -EINVAL;
    for (const cJSON *previous = scopes->child; previous != scope;
         previous = previous->next)
      if (!strcmp(previous->valuestring, scope->valuestring))
        return -EINVAL;
  }
  cJSON *item = cJSON_CreateObject();
  char principal[32];
  if (ny_in_string(old, "principal")[0])
    snprintf(principal, sizeof(principal), "%s",
             ny_in_string(old, "principal"));
  else
    snprintf(principal, sizeof(principal), "mcpin-%" PRIu64, revision + 1);
  cJSON *copy = cJSON_Duplicate(scopes, true);
  if (!item || !copy || !cJSON_AddItemToObject(item, "scopes", copy))
    {
      cJSON_Delete(item);
      cJSON_Delete(copy);
      return -ENOMEM;
    }
  if (!cJSON_AddStringToObject(item, "id", id) ||
      !cJSON_AddStringToObject(item, "principal", principal) ||
      !cJSON_AddStringToObject(item, "title", title) ||
      !cJSON_AddStringToObject(item, "hash", hash) ||
      !cJSON_AddNumberToObject(item, "expiresAt", expires->valuedouble) ||
      !cJSON_AddNumberToObject(item, "updatedAt", now))
    {
      cJSON_Delete(item);
      return -ENOMEM;
    }
  if (old)
    {
      if (!cJSON_ReplaceItemViaPointer(clients, old, item))
        {
          cJSON_Delete(item);
          return -ENOMEM;
        }
    }
  else if (!cJSON_AddItemToArray(clients, item))
    {
      cJSON_Delete(item);
      return -ENOMEM;
    }
  return 0;
}

/****************************************************************************
 * Name: ny_agent_mcp_in
 ****************************************************************************/

int ny_agent_mcp_in(const struct ny_product_caller_s *caller,
                    const char *topic, const cJSON *data, cJSON **result)
{
  if (!result)
    return -EINVAL;
  *result = NULL;
  if (!caller || caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  if (!topic || !cJSON_IsObject(data))
    return -EINVAL;
  bool list = !strcmp(topic, "agent.mcp.in.list");
  bool save = !strcmp(topic, "agent.mcp.in.save");
  bool remove = !strcmp(topic, "agent.mcp.in.delete");
  bool enable = !strcmp(topic, "agent.mcp.in.enable");
  if (!list && !save && !remove && !enable)
    return -ENOSYS;
  nxmutex_lock(&g_in_lock);
  uint64_t revision;
  cJSON *root = NULL;
  int ret = ny_in_load(&root, &revision);
  if (ret == 0 && !list && !ny_in_revision(data, revision))
    ret = -ESTALE;
  if (ret == 0 && save)
    ret = ny_in_save(root, data, caller->local_transport, revision);
  else if (ret == 0 && remove)
    {
      cJSON *clients = cJSON_GetObjectItemCaseSensitive(root, "clients");
      cJSON *item = ny_in_find(clients, ny_in_string(data, "id"));
      if (!item)
        ret = -ENOENT;
      else
        cJSON_Delete(cJSON_DetachItemViaPointer(clients, item));
    }
  else if (ret == 0 && enable)
    {
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
      if (!cJSON_IsBool(enabled))
        ret = -EINVAL;
      else
        {
          cJSON *value = cJSON_CreateBool(cJSON_IsTrue(enabled));
          if (!value ||
              !cJSON_ReplaceItemInObjectCaseSensitive(root, "enabled", value))
            {
              cJSON_Delete(value);
              ret = -ENOMEM;
            }
        }
    }
  if (ret == 0 && !list)
    ret = ny_product_store_write("mcp-in", root, revision, &revision);
  if (ret == 0)
    ret = ny_in_public(root, revision, result);
  cJSON_Delete(root);
  nxmutex_unlock(&g_in_lock);
  if (ret < 0)
    {
      cJSON_Delete(*result);
      *result = NULL;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_in_envelope
 ****************************************************************************/

static cJSON *ny_in_envelope(const cJSON *id, cJSON *result, int error)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *copy = id ? cJSON_Duplicate(id, true) : cJSON_CreateNull();
  if (!root || !copy || !cJSON_AddItemToObject(root, "id", copy))
    {
      cJSON_Delete(copy);
      cJSON_Delete(root);
      cJSON_Delete(result);
      return NULL;
    }
  if (!cJSON_AddStringToObject(root, "jsonrpc", "2.0"))
    {
      cJSON_Delete(root);
      cJSON_Delete(result);
      return NULL;
    }
  if (error)
    {
      cJSON_Delete(result);
      cJSON *detail = cJSON_AddObjectToObject(root, "error");
      if (!detail || !cJSON_AddNumberToObject(detail, "code", error) ||
          !cJSON_AddStringToObject(detail, "message",
                                   error == -32601
                                       ? "Method not available"
                                       : "Invalid request or parameters"))
        {
          cJSON_Delete(root);
          return NULL;
        }
    }
  else if (!result || !cJSON_AddItemToObject(root, "result", result))
    {
      cJSON_Delete(result);
      cJSON_Delete(root);
      return NULL;
    }
  return root;
}

/****************************************************************************
 * Name: ny_in_tools
 ****************************************************************************/

static cJSON *ny_in_tools(const cJSON *client)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *tools = cJSON_AddArrayToObject(root, "tools");
  if (!root || !tools)
    goto fail;
  for (size_t i = 0; i < sizeof(g_in_tools) / sizeof(g_in_tools[0]); i++)
    {
      if (!ny_in_scope(client, g_in_tools[i].name))
        continue;
      cJSON *tool = cJSON_CreateObject();
      if (!tool || !cJSON_AddItemToArray(tools, tool))
        {
          cJSON_Delete(tool);
          goto fail;
        }
      cJSON *schema = cJSON_AddObjectToObject(tool, "inputSchema");
      cJSON *annotations = cJSON_AddObjectToObject(tool, "annotations");
      if (!schema || !annotations ||
          !cJSON_AddStringToObject(tool, "name", g_in_tools[i].name) ||
          !cJSON_AddStringToObject(tool, "description",
                                   g_in_tools[i].description) ||
          !cJSON_AddStringToObject(schema, "type", "object") ||
          !cJSON_AddObjectToObject(schema, "properties") ||
          !cJSON_AddBoolToObject(schema, "additionalProperties", false) ||
          !cJSON_AddBoolToObject(
              annotations, "readOnlyHint",
              strcmp(g_in_tools[i].topic, "agent.chat") &&
                  strcmp(g_in_tools[i].topic, "agent.cancel")) ||
          !cJSON_AddBoolToObject(annotations, "openWorldHint",
                                 !strcmp(g_in_tools[i].topic, "agent.chat")))
        goto fail;
      if (!strncmp(g_in_tools[i].topic, "agent.", 6))
        {
          bool chat = !strcmp(g_in_tools[i].topic, "agent.chat");
          cJSON *properties =
              cJSON_GetObjectItemCaseSensitive(schema, "properties");
          cJSON *required = cJSON_AddArrayToObject(schema, "required");
          const char *fields[] = { "requestId", "conversationId", "text" };
          for (size_t j = 0; j < (chat ? 3u : 1u); j++)
            {
              const char *name = chat ? fields[j] : "id";
              cJSON *field = cJSON_AddObjectToObject(properties, name);
              cJSON *key = cJSON_CreateString(name);
              if (!field || !required || !key ||
                  !cJSON_AddStringToObject(field, "type", "string") ||
                  !cJSON_AddItemToArray(required, key))
                {
                  cJSON_Delete(key);
                  goto fail;
                }
            }
        }
    }
  return root;
fail:
  cJSON_Delete(root);
  return NULL;
}

/****************************************************************************
 * Name: ny_in_call
 ****************************************************************************/

static cJSON *ny_in_call(cJSON *root, cJSON *client, const cJSON *params,
                         uint64_t revision)
{
  const char *name = ny_in_string(params, "name");
  const cJSON *arguments =
      cJSON_GetObjectItemCaseSensitive(params, "arguments");
  const struct ny_mcp_in_tool_s *tool = NULL;
  for (size_t i = 0; i < sizeof(g_in_tools) / sizeof(g_in_tools[0]); i++)
    if (!strcmp(g_in_tools[i].name, name))
      tool = &g_in_tools[i];
  int ret = tool && ny_in_scope(client, name) ? 0 : -EACCES;
  bool agent = tool && !strncmp(tool->topic, "agent.", 6);
  if (ret == 0 && !agent && arguments &&
      (!cJSON_IsObject(arguments) || arguments->child))
    ret = -EINVAL;

  /* A bounded fixed table, never caller-supplied Core topics or roles.
   * Keep the policy lock through dispatch so completed revocation wins.
   */
  cJSON *data = cJSON_CreateObject();
  cJSON *output = NULL;
  const struct ny_product_caller_s caller = { ny_in_string(client, "id"),
                                              NY_PRODUCT_FAMILY, false };
  if (ret == 0)
    ret = agent  ? ny_agent_remote_request(ny_in_string(client, "principal"),
                                           tool->topic, arguments, &output)
          : data ? ny_product_request(&caller, tool->topic, data, &output)
                 : -ENOMEM;
  cJSON_Delete(data);
  cJSON *audit = cJSON_GetObjectItemCaseSensitive(root, "audit");
  cJSON *event = cJSON_CreateObject();
  if (!event || !cJSON_AddStringToObject(event, "client", caller.id) ||
      !cJSON_AddStringToObject(event, "tool",
                               tool ? tool->name : "unavailable") ||
      !cJSON_AddNumberToObject(event, "at", ny_product_time_ms(false)) ||
      !cJSON_AddNumberToObject(event, "status", ret) ||
      !cJSON_AddItemToArray(audit, event))
    {
      cJSON_Delete(event);
      cJSON_Delete(output);
      return NULL;
    }
  while (cJSON_GetArraySize(audit) > NY_MCP_IN_AUDIT)
    cJSON_DeleteItemFromArray(audit, 0);
  if (ny_product_store_write("mcp-in", root, revision, &revision) < 0)
    {
      cJSON_Delete(output);
      return NULL;
    }
  char *text = ret == 0 && output ? cJSON_PrintUnformatted(output) : NULL;
  cJSON_Delete(output);
  cJSON *result = cJSON_CreateObject();
  cJSON *content = cJSON_AddArrayToObject(result, "content");
  cJSON *entry = cJSON_CreateObject();
  if (!result || !content || !entry ||
      !cJSON_AddBoolToObject(result, "isError", ret != 0 || !text) ||
      !cJSON_AddStringToObject(entry, "type", "text") ||
      !cJSON_AddStringToObject(entry, "text",
                               text ? text
                                    : "Tool unavailable, not authorized, "
                                      "invalid arguments, or read failed") ||
      !cJSON_AddItemToArray(content, entry))
    {
      cJSON_Delete(entry);
      cJSON_Delete(result);
      result = NULL;
    }
  free(text);
  return result;
}

/****************************************************************************
 * Name: ny_mcp_in_rpc
 ****************************************************************************/

int ny_mcp_in_rpc(const char *token, const cJSON *request, cJSON **reply)
{
  if (!reply)
    return 500;
  *reply = NULL;
  char hash[65];
  if (ny_in_digest(token, hash) < 0)
    return 401;
  nxmutex_lock(&g_in_lock);
  cJSON *root = NULL;
  uint64_t revision;
  int status = 503;
  if (ny_in_load(&root, &revision) < 0 ||
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "enabled")))
    goto out;
  status = 401;
  cJSON *client = NULL;
  cJSON *candidate;
  double now = ny_product_time_ms(false);
  cJSON_ArrayForEach(candidate,
                     cJSON_GetObjectItemCaseSensitive(root, "clients"))
  {
    const char *stored = ny_in_string(candidate, "hash");
    const cJSON *expiry =
        cJSON_GetObjectItemCaseSensitive(candidate, "expiresAt");
    if (strlen(stored) == 64 && sodium_memcmp(stored, hash, 64) == 0 &&
        cJSON_IsNumber(expiry) && isfinite(expiry->valuedouble) &&
        now >= NY_MCP_IN_MIN_TIME && now < expiry->valuedouble)
      client = candidate;
  }
  if (!client)
    goto out;
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
  const char *method = ny_in_string(request, "method");
  bool valid_id = (cJSON_IsString(id) && strlen(id->valuestring) <= 128) ||
                  (cJSON_IsNumber(id) && isfinite(id->valuedouble) &&
                   fabs(id->valuedouble) <= NY_MCP_IN_MAX_INTEGER &&
                   floor(id->valuedouble) == id->valuedouble);
  int error = 0;
  cJSON *result = NULL;
  status = 200;
  if (!cJSON_IsObject(request) ||
      strcmp(ny_in_string(request, "jsonrpc"), "2.0") || !method[0] ||
      (params && !cJSON_IsObject(params)) || (id && !valid_id))
    error = -32600;
  else if (!id)
    {
      status = !strcmp(method, "notifications/initialized") ? 202 : 400;
      goto out;
    }
  else if (!strcmp(method, "initialize"))
    {
      if (!cJSON_IsString(
              cJSON_GetObjectItemCaseSensitive(params, "protocolVersion")) ||
          !cJSON_IsObject(
              cJSON_GetObjectItemCaseSensitive(params, "capabilities")) ||
          !cJSON_IsObject(
              cJSON_GetObjectItemCaseSensitive(params, "clientInfo")))
        error = -32602;
      else
        result = cJSON_Parse(
            "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{"
            "}},\"serverInfo\":{\"name\":\"Nyabot scoped "
            "Core\",\"version\":\"1\"},\"instructions\":\"Only tools "
            "explicitly granted to this credential. Chat is isolated. No "
            "owner, shell, "
            "approvals, memory, or outbound MCP access.\"}");
    }
  else if (!strcmp(method, "ping"))
    result = cJSON_CreateObject();
  else if (!strcmp(method, "tools/list"))
    {
      if (params && params->child)
        error = -32602;
      else
        result = ny_in_tools(client);
    }
  else if (!strcmp(method, "tools/call"))
    result = ny_in_call(root, client, params, revision);
  else
    error = -32601;
  if (!error && !result)
    {
      status = 500;
      goto out;
    }
  *reply = ny_in_envelope(valid_id ? id : NULL, result, error);
  if (!*reply)
    status = 500;
out:
  cJSON_Delete(root);
  sodium_memzero(hash, sizeof(hash));
  nxmutex_unlock(&g_in_lock);
  return status;
}
