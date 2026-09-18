/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_mcp.c
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

#include "ny_agent_mcp.h"
#include "ny_agent_mcp_wire.h"
#include "ny_product_store.h"
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdlib.h>
#include <string.h>

#define NY_MCP_SOURCE_MAX        4
#define NY_MCP_EXACT_INTEGER_MAX 9007199254740991.0

static mutex_t g_mcp_lock = NXMUTEX_INITIALIZER;
static const char *ny_mcp_string(const cJSON *object, const char *key);
static cJSON *ny_mcp_find(cJSON *items, const char *key, const char *value);
static int ny_mcp_load(cJSON **root, uint64_t *revision);
static bool ny_mcp_revision(const cJSON *object, const char *key,
                            uint64_t value);
static bool ny_mcp_id(const char *id);
static int ny_mcp_put(cJSON *object, const char *key, cJSON *value);
static int ny_mcp_save(cJSON *root, const cJSON *data, bool local,
                       uint64_t next);
static int ny_mcp_grant(cJSON *item, const cJSON *data);
static int ny_mcp_refresh(const cJSON *data, cJSON **result);
static int ny_mcp_public(cJSON *root, uint64_t revision, cJSON **result);
static bool ny_mcp_granted(const cJSON *item, const char *kind,
                           const char *name);

/****************************************************************************
 * Name: ny_mcp_string
 ****************************************************************************/

static const char *ny_mcp_string(const cJSON *object, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

/****************************************************************************
 * Name: ny_mcp_find
 ****************************************************************************/

static cJSON *ny_mcp_find(cJSON *items, const char *key, const char *value)
{
  cJSON *item;
  cJSON_ArrayForEach(
      item, items) if (!strcmp(ny_mcp_string(item, key), value)) return item;
  return NULL;
}

/****************************************************************************
 * Name: ny_mcp_load
 ****************************************************************************/

static int ny_mcp_load(cJSON **root, uint64_t *revision)
{
  int ret = ny_product_store_read("mcp-out", root, revision);
  if (ret < 0)
    return ret;
  if (!*root)
    {
      *root = cJSON_CreateObject();
      if (!*root || !cJSON_AddArrayToObject(*root, "items"))
        return -ENOMEM;
    }
  return cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(*root, "items"))
             ? 0
             : -EBADMSG;
}

/****************************************************************************
 * Name: ny_mcp_revision
 ****************************************************************************/

static bool ny_mcp_revision(const cJSON *object, const char *key,
                            uint64_t value)
{
  const cJSON *number = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(number) && isfinite(number->valuedouble) &&
         number->valuedouble >= 0 &&
         number->valuedouble <= NY_MCP_EXACT_INTEGER_MAX &&
         number->valuedouble == (double)value;
}

/****************************************************************************
 * Name: ny_mcp_id
 ****************************************************************************/

static bool ny_mcp_id(const char *id)
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
 * Name: ny_mcp_put
 ****************************************************************************/

static int ny_mcp_put(cJSON *object, const char *key, cJSON *value)
{
  if (!value)
    return -ENOMEM;
  cJSON_DeleteItemFromObjectCaseSensitive(object, key);
  if (!cJSON_AddItemToObject(object, key, value))
    {
      cJSON_Delete(value);
      return -ENOMEM;
    }
  return 0;
}

/****************************************************************************
 * Name: ny_mcp_save
 ****************************************************************************/

static int ny_mcp_save(cJSON *root, const cJSON *data, bool local,
                       uint64_t next)
{
  const char *id = ny_mcp_string(data, "id");
  const char *url = ny_mcp_string(data, "url");
  const char *title = ny_mcp_string(data, "title");
  if (!ny_mcp_id(id) || !title[0] || strlen(title) > 96)
    return -EINVAL;
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
  cJSON *item = ny_mcp_find(items, "id", id);
  const cJSON *key = cJSON_GetObjectItemCaseSensitive(data, "secret");
  if (key && (!local || !cJSON_IsString(key)))
    return -EACCES;
  const char *secret = key ? key->valuestring : ny_mcp_string(item, "secret");
  if (!local && secret[0] && strcmp(url, ny_mcp_string(item, "url")))
    return -EACCES;
  struct ny_mcp_peer_s *peer = calloc(1, sizeof(*peer));
  if (!peer)
    return -ENOMEM;
  int ret = ny_mcp_peer_configure(peer, url, secret);
  memset(peer, 0, sizeof(*peer));
  free(peer);
  if (ret < 0)
    return ret;
  if (!item)
    {
      if (cJSON_GetArraySize(items) >= NY_MCP_SOURCE_MAX)
        return -ENOSPC;
      item = cJSON_CreateObject();
      if (!item || !cJSON_AddItemToArray(items, item))
        {
          cJSON_Delete(item);
          return -ENOMEM;
        }
      if (!cJSON_AddStringToObject(item, "id", id))
        return -ENOMEM;
    }
  /* Duplicate before replacing a secret borrowed from the existing item. */
  cJSON *secret_copy = cJSON_CreateString(secret);
  if (!secret_copy)
    return -ENOMEM;
  ret = ny_mcp_put(item, "secret", secret_copy);
  if (ret == 0)
    ret = ny_mcp_put(item, "url", cJSON_CreateString(url));
  if (ret == 0)
    ret = ny_mcp_put(item, "title", cJSON_CreateString(title));
  if (ret == 0)
    ret = ny_mcp_put(item, "generation", cJSON_CreateNumber(next));
  if (ret == 0)
    ret = ny_mcp_put(item, "grants", cJSON_CreateArray());
  cJSON_DeleteItemFromObjectCaseSensitive(item, "catalog");
  return ret;
}

/****************************************************************************
 * Name: ny_mcp_grant
 ****************************************************************************/

static int ny_mcp_grant(cJSON *item, const cJSON *data)
{
  const cJSON *generation =
      cJSON_GetObjectItemCaseSensitive(item, "generation");
  if (!cJSON_IsNumber(generation) ||
      !ny_mcp_revision(data, "generation", (uint64_t)generation->valuedouble))
    return -ESTALE;
  const cJSON *grants = cJSON_GetObjectItemCaseSensitive(data, "grants");
  if (!cJSON_IsArray(grants) || cJSON_GetArraySize(grants) > 96)
    return -EINVAL;
  cJSON *catalog = cJSON_GetObjectItemCaseSensitive(item, "catalog");
  const cJSON *grant;
  cJSON_ArrayForEach(grant, grants)
  {
    const char *kind = ny_mcp_string(grant, "kind");
    const char *name = ny_mcp_string(grant, "name");
    if (strcmp(kind, "tools") && strcmp(kind, "resources") &&
        strcmp(kind, "prompts"))
      return -EINVAL;
    if (!name[0] ||
        !ny_mcp_find(cJSON_GetObjectItemCaseSensitive(catalog, kind),
                     !strcmp(kind, "resources") ? "uri" : "name", name))
      return -ENOENT;
    for (const cJSON *prior = grants->child; prior != grant;
         prior = prior->next)
      if (!strcmp(ny_mcp_string(prior, "kind"), kind) &&
          !strcmp(ny_mcp_string(prior, "name"), name))
        return -EINVAL;
  }
  return ny_mcp_put(item, "grants", cJSON_Duplicate(grants, true));
}

/****************************************************************************
 * Name: ny_mcp_public
 ****************************************************************************/

static int ny_mcp_public(cJSON *root, uint64_t revision, cJSON **result)
{
  cJSON *copy = cJSON_Duplicate(root, true);
  if (!copy)
    return -ENOMEM;
  cJSON *item;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(copy, "items"))
  {
    bool secret = ny_mcp_string(item, "secret")[0] != 0;
    cJSON_DeleteItemFromObjectCaseSensitive(item, "secret");
    if (!cJSON_AddBoolToObject(item, "hasSecret", secret))
      {
        cJSON_Delete(copy);
        return -ENOMEM;
      }
  }
  if (!cJSON_AddNumberToObject(copy, "revision", revision))
    {
      cJSON_Delete(copy);
      return -ENOMEM;
    }
  *result = copy;
  return 0;
}

/****************************************************************************
 * Name: ny_mcp_refresh
 ****************************************************************************/

static int ny_mcp_refresh(const cJSON *data, cJSON **result)
{
  cJSON *root = NULL;
  cJSON *catalog = NULL;
  uint64_t revision = 0;
  struct ny_mcp_peer_s *peer = calloc(1, sizeof(*peer));
  if (!peer)
    return -ENOMEM;
  nxmutex_lock(&g_mcp_lock);
  int ret = ny_mcp_load(&root, &revision);
  cJSON *item = ny_mcp_find(cJSON_GetObjectItemCaseSensitive(root, "items"),
                            "id", ny_mcp_string(data, "id"));
  if (ret == 0 && !ny_mcp_revision(data, "revision", revision))
    ret = -ESTALE;
  if (ret == 0 && revision >= NY_MCP_EXACT_INTEGER_MAX)
    ret = -EOVERFLOW;
  if (ret == 0 && !item)
    ret = -ENOENT;
  if (ret == 0)
    ret = ny_mcp_peer_configure(peer, ny_mcp_string(item, "url"),
                                ny_mcp_string(item, "secret"));
  nxmutex_unlock(&g_mcp_lock);
  if (ret == 0)
    ret = ny_mcp_peer_initialize(peer);
  if (ret == 0)
    ret = ny_mcp_peer_catalog(peer, &catalog);
  memset(peer, 0, sizeof(*peer));
  free(peer);
  nxmutex_lock(&g_mcp_lock);
  if (ret == 0)
    {
      ret = ny_mcp_put(item, "catalog", catalog);
      catalog = NULL;
      if (ret == 0)
        ret = ny_mcp_put(item, "grants", cJSON_CreateArray());
      if (ret == 0)
        ret = ny_mcp_put(item, "generation", cJSON_CreateNumber(revision + 1));
      if (ret == 0)
        ret = ny_product_store_write("mcp-out", root, revision, &revision);
      if (ret == 0)
        ret = ny_mcp_public(root, revision, result);
    }
  nxmutex_unlock(&g_mcp_lock);
  cJSON_Delete(catalog);
  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_agent_mcp
 ****************************************************************************/

int ny_agent_mcp(const struct ny_product_caller_s *caller, const char *topic,
                 const cJSON *data, cJSON **result)
{
  *result = NULL;
  if (!caller || caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  if (!strcmp(topic, "agent.mcp.out.refresh"))
    return ny_mcp_refresh(data, result);
  bool list = !strcmp(topic, "agent.mcp.out.list");
  bool save = !strcmp(topic, "agent.mcp.out.save");
  bool remove = !strcmp(topic, "agent.mcp.out.delete");
  bool grant = !strcmp(topic, "agent.mcp.out.grant");
  if (!list && !save && !remove && !grant)
    return -ENOSYS;
  cJSON *root = NULL;
  uint64_t revision = 0;
  nxmutex_lock(&g_mcp_lock);
  int ret = ny_mcp_load(&root, &revision);
  if (ret == 0 && !list && !ny_mcp_revision(data, "revision", revision))
    ret = -ESTALE;
  if (ret == 0 && !list && revision >= NY_MCP_EXACT_INTEGER_MAX)
    ret = -EOVERFLOW;
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
  cJSON *item = ny_mcp_find(items, "id", ny_mcp_string(data, "id"));
  if (ret == 0 && save)
    ret = ny_mcp_save(root, data, caller->local_transport, revision + 1);
  if (ret == 0 && (remove || grant) && !item)
    ret = -ENOENT;
  if (ret == 0 && remove)
    cJSON_Delete(cJSON_DetachItemViaPointer(items, item));
  if (ret == 0 && grant)
    ret = ny_mcp_grant(item, data);
  if (ret == 0 && !list)
    ret = ny_product_store_write("mcp-out", root, revision, &revision);
  if (ret == 0)
    ret = ny_mcp_public(root, revision, result);
  nxmutex_unlock(&g_mcp_lock);
  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_mcp_granted
 ****************************************************************************/

static bool ny_mcp_granted(const cJSON *item, const char *kind,
                           const char *name)
{
  const cJSON *grant;
  cJSON_ArrayForEach(
      grant,
      cJSON_GetObjectItemCaseSensitive(
          item, "grants")) if (!strcmp(ny_mcp_string(grant, "kind"), kind) &&
                               !strcmp(ny_mcp_string(grant, "name"),
                                       name)) return true;
  return false;
}

/****************************************************************************
 * Name: ny_agent_mcp_catalog
 ****************************************************************************/

int ny_agent_mcp_catalog(const cJSON *data, cJSON **result)
{
  *result = NULL;
  const char *server = ny_mcp_string(data, "server");
  const char *kind = ny_mcp_string(data, "kind");
  const char *name = ny_mcp_string(data, "name");
  const cJSON *field;
  cJSON_ArrayForEach(field,
                     data) if (!cJSON_IsString(field) || !field->string ||
                               (strcmp(field->string, "server") &&
                                strcmp(field->string, "kind") &&
                                strcmp(field->string, "name"))) return -EINVAL;
  if ((kind[0] || name[0]) && !server[0])
    return -EINVAL;
  if (name[0] && !kind[0])
    return -EINVAL;
  cJSON *root = NULL;
  cJSON *output = cJSON_CreateObject();
  cJSON *rows = output ? cJSON_AddArrayToObject(output, "items") : NULL;
  if (!rows)
    {
      cJSON_Delete(output);
      return -ENOMEM;
    }
  uint64_t revision = 0;
  nxmutex_lock(&g_mcp_lock);
  int ret = ny_mcp_load(&root, &revision);
  bool found = false;
  cJSON *item;
  if (ret == 0)
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "items"))
    {
      if (server[0] && strcmp(server, ny_mcp_string(item, "id")))
        continue;
      const cJSON *grants = cJSON_GetObjectItemCaseSensitive(item, "grants");
      if (cJSON_GetArraySize(grants) == 0)
        continue;
      cJSON *row = cJSON_CreateObject();
      bool valid = row && cJSON_AddStringToObject(row, "server",
                                                  ny_mcp_string(item, "id"));
      valid &= ny_mcp_put(row, "generation",
                          cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(
                                              item, "generation"),
                                          true)) == 0;
      if (server[0] && name[0])
        {
          if (!ny_mcp_granted(item, kind, name))
            {
              cJSON_Delete(row);
              ret = -EACCES;
              break;
            }
          cJSON *catalog = cJSON_GetObjectItemCaseSensitive(item, "catalog");
          cJSON *entry =
              ny_mcp_find(cJSON_GetObjectItemCaseSensitive(catalog, kind),
                          !strcmp(kind, "resources") ? "uri" : "name", name);
          valid &= ny_mcp_put(row, "entry", cJSON_Duplicate(entry, true)) == 0;
        }
      else if (server[0])
        valid &= ny_mcp_put(row, "grants", cJSON_Duplicate(grants, true)) == 0;
      else
        valid &= cJSON_AddStringToObject(row, "title",
                                         ny_mcp_string(item, "title")) != NULL;
      if (!valid || !cJSON_AddItemToArray(rows, row))
        {
          cJSON_Delete(row);
          ret = -ENOMEM;
          break;
        }
      found = true;
    }
  if (ret == 0 && server[0] && !found)
    ret = -ENOENT;
  nxmutex_unlock(&g_mcp_lock);
  cJSON_Delete(root);
  if (ret < 0)
    cJSON_Delete(output);
  else
    *result = output;
  return ret;
}

/****************************************************************************
 * Name: ny_agent_mcp_call
 ****************************************************************************/

int ny_agent_mcp_call(const cJSON *data, cJSON **result, bool *uncertain)
{
  *result = NULL;
  *uncertain = false;
  const char *id = ny_mcp_string(data, "server");
  const char *kind = ny_mcp_string(data, "kind");
  const char *name = ny_mcp_string(data, "name");
  const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(data, "arguments");
  const char *method = !strcmp(kind, "tools")       ? "tools/call"
                       : !strcmp(kind, "resources") ? "resources/read"
                       : !strcmp(kind, "prompts")   ? "prompts/get"
                                                    : NULL;
  if (!method || !ny_mcp_id(id) || !name[0] || !cJSON_IsObject(arguments) ||
      (!strcmp(kind, "resources") && cJSON_GetArraySize(arguments)))
    return -EINVAL;
  cJSON *root = NULL;
  cJSON *fresh = NULL;
  cJSON *current = NULL;
  cJSON *params = NULL;
  uint64_t revision = 0;
  uint64_t latest = 0;
  struct ny_mcp_peer_s *peer = calloc(1, sizeof(*peer));
  if (!peer)
    return -ENOMEM;
  nxmutex_lock(&g_mcp_lock);
  int ret = ny_mcp_load(&root, &revision);
  cJSON *item =
      ny_mcp_find(cJSON_GetObjectItemCaseSensitive(root, "items"), "id", id);
  const cJSON *generation =
      cJSON_GetObjectItemCaseSensitive(item, "generation");
  if (ret == 0 && (!item || !cJSON_IsNumber(generation) ||
                   !ny_mcp_revision(data, "generation",
                                    (uint64_t)generation->valuedouble) ||
                   !ny_mcp_granted(item, kind, name)))
    ret = -EACCES;
  if (ret == 0)
    ret = ny_mcp_peer_configure(peer, ny_mcp_string(item, "url"),
                                ny_mcp_string(item, "secret"));
  nxmutex_unlock(&g_mcp_lock);
  if (ret == 0)
    ret = ny_mcp_peer_initialize(peer);
  if (ret == 0)
    ret = ny_mcp_peer_catalog(peer, &fresh);
  if (ret == 0 &&
      !cJSON_Compare(fresh, cJSON_GetObjectItemCaseSensitive(item, "catalog"),
                     true))
    ret = -ESTALE;
  if (ret == 0)
    {
      params = cJSON_CreateObject();
      if (!params ||
          !cJSON_AddStringToObject(
              params, !strcmp(kind, "resources") ? "uri" : "name", name))
        ret = -ENOMEM;
      if (ret == 0 && strcmp(kind, "resources"))
        ret =
            ny_mcp_put(params, "arguments", cJSON_Duplicate(arguments, true));
    }
  /* This check is the dispatch boundary. Revocation cannot undo a request
   * already dispatched. Never hold the store lock across network I/O.
   */
  nxmutex_lock(&g_mcp_lock);
  if (ret == 0)
    ret = ny_mcp_load(&current, &latest);
  if (ret == 0 && latest != revision)
    ret = -ESTALE;
  nxmutex_unlock(&g_mcp_lock);
  if (ret == 0)
    {
      *uncertain = true;
      ret = ny_mcp_peer_request(peer, method, params, result);
      if (ret == 0 &&
          !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(*result, "isError")))
        *uncertain = false;
      else if (ret == 0)
        ret = -EREMOTEIO;
    }
  memset(peer, 0, sizeof(*peer));
  free(peer);
  cJSON_Delete(params);
  cJSON_Delete(current);
  cJSON_Delete(fresh);
  cJSON_Delete(root);
  return ret;
}
