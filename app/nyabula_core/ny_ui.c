/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_ui.c
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

#include <errno.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ny_manifest.h"
#include "ny_permission.h"
#include "ny_scheduler.h"
#include "ny_ui.h"

struct ny_ui_slot_s
{
  char id[NY_PLUGIN_ID_SIZE];
  uint32_t generation;
  uint64_t permission;
  cJSON *tree;
};

struct ny_ui_validation_s
{
  const char *ids[200];
  size_t ids_count;
  size_t nodes;
};

static mutex_t g_ui_lock = NXMUTEX_INITIALIZER;
static struct ny_ui_slot_s g_ui_slots[CONFIG_NYABULA_CORE_MAX_PLUGINS];
static uint64_t g_ui_revision = 1;

static const char *ny_ui_string(const cJSON *object, const char *name);
static int ny_ui_check_strings(const cJSON *value, unsigned int depth);
static int ny_ui_check_node(const cJSON *node, unsigned int depth,
                            struct ny_ui_validation_s *validation);
static int ny_ui_check_document(const char *json, size_t length);
static int ny_ui_validate_view(const cJSON *tree, uint64_t *permission);
static int ny_ui_validate_views(const cJSON *tree, uint64_t *permission);
static int ny_ui_authorized(const char *id, uint64_t permission);
static struct ny_ui_slot_s *ny_ui_find(const char *id);
static const cJSON *ny_ui_find_node(const cJSON *node, const char *id);

/****************************************************************************
 * Name: ny_ui_string
 ****************************************************************************/

static const char *ny_ui_string(const cJSON *object, const char *name)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsString(value) ? value->valuestring : NULL;
}

/****************************************************************************
 * Name: ny_ui_check_strings
 ****************************************************************************/

static int ny_ui_check_strings(const cJSON *value, unsigned int depth)
{
  const cJSON *child;
  if (depth > 32 ||
      (cJSON_IsString(value) && strlen(value->valuestring) > 4096))
    {
      return -EFBIG;
    }

  cJSON_ArrayForEach(child, value)
  {
    if (child->string != NULL && strlen(child->string) > 64)
      {
        return -EFBIG;
      }

    if (ny_ui_check_strings(child, depth + 1) < 0)
      {
        return -EFBIG;
      }
  }

  return 0;
}

/****************************************************************************
 * Name: ny_ui_check_node
 ****************************************************************************/

static int ny_ui_check_node(const cJSON *node, unsigned int depth,
                            struct ny_ui_validation_s *validation)
{
  const char *type = ny_ui_string(node, "type");
  const char *id = ny_ui_string(node, "id");
  const cJSON *children = cJSON_GetObjectItemCaseSensitive(node, "children");
  const cJSON *on = cJSON_GetObjectItemCaseSensitive(node, "on");
  const cJSON *child;
  size_t index;
  if (!cJSON_IsObject(node) || type == NULL || strlen(type) > 64 ||
      depth > 12 || ++validation->nodes > 200)
    {
      return -EINVAL;
    }

  if (id != NULL)
    {
      if (*id == '\0' || strlen(id) > 63)
        {
          return -EINVAL;
        }

      for (index = 0; index < validation->ids_count; index++)
        {
          if (strcmp(validation->ids[index], id) == 0)
            {
              return -EINVAL;
            }
        }

      validation->ids[validation->ids_count++] = id;
    }

  if (on != NULL)
    {
      if (id == NULL || !cJSON_IsObject(on))
        {
          return -EINVAL;
        }

      cJSON_ArrayForEach(child, on)
      {
        const char *command = ny_ui_string(child, "command");
        if (child->string == NULL || strlen(child->string) > 63 ||
            command == NULL || *command == '\0' || strlen(command) > 128)
          {
            return -EINVAL;
          }
      }
    }

  if (children != NULL)
    {
      if (!cJSON_IsArray(children) ||
          (strcmp(type, "column") != 0 && strcmp(type, "row") != 0 &&
           strcmp(type, "grid") != 0 && strcmp(type, "card") != 0 &&
           strcmp(type, "section") != 0 && strcmp(type, "listSection") != 0))
        {
          return -EINVAL;
        }

      cJSON_ArrayForEach(child, children)
      {
        int ret = ny_ui_check_node(child, depth + 1, validation);
        if (ret < 0)
          {
            return ret;
          }
      }
    }

  return 0;
}

/****************************************************************************
 * Name: ny_ui_check_document
 *
 * Bound nesting before cJSON allocates or recurses over untrusted input.
 ****************************************************************************/

static int ny_ui_check_document(const char *json, size_t length)
{
  bool quoted = false;
  bool escaped = false;
  unsigned int depth = 0;
  size_t index;
  if (json == NULL || length == 0 || length > NY_UI_DOCUMENT_LIMIT ||
      memchr(json, 0, length) != NULL)
    {
      return -EFBIG;
    }

  for (index = 0; index < length; index++)
    {
      char ch = json[index];
      if (quoted)
        {
          if (escaped)
            {
              escaped = false;
            }
          else if (ch == '\\')
            {
              escaped = true;
            }
          else if (ch == '"')
            {
              quoted = false;
            }
        }
      else if (ch == '"')
        {
          quoted = true;
        }
      else if (ch == '{' || ch == '[')
        {
          if (++depth > 40)
            {
              return -EFBIG;
            }
        }
      else if (ch == '}' || ch == ']')
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

/****************************************************************************
 * Name: ny_ui_authorized
 ****************************************************************************/

static int ny_ui_authorized(const char *id, uint64_t permission)
{
  struct ny_plugin_config_s config;
  int ret;
  memset(&config, 0, sizeof(config));
  strlcpy(config.id, id, sizeof(config.id));
  config.requested_permissions = permission;
  ret = ny_permission_apply(&config);
  return ret < 0                                           ? ret
         : (config.permissions & permission) == permission ? 0
                                                           : -EACCES;
}

/****************************************************************************
 * Name: ny_ui_find
 ****************************************************************************/

static struct ny_ui_slot_s *ny_ui_find(const char *id)
{
  size_t index;
  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (g_ui_slots[index].tree != NULL &&
          strcmp(g_ui_slots[index].id, id) == 0)
        {
          return &g_ui_slots[index];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: ny_ui_find_node
 ****************************************************************************/

static const cJSON *ny_ui_find_node(const cJSON *node, const char *id)
{
  const char *own_id = ny_ui_string(node, "id");
  const cJSON *child;
  if (own_id != NULL && strcmp(own_id, id) == 0)
    {
      return node;
    }

  cJSON_ArrayForEach(child, cJSON_GetObjectItemCaseSensitive(node, "children"))
  {
    const cJSON *found = ny_ui_find_node(child, id);
    if (found != NULL)
      {
        return found;
      }
  }

  return NULL;
}

/****************************************************************************
 * Name: ny_ui_validate_view
 ****************************************************************************/

static int ny_ui_validate_view(const cJSON *tree, uint64_t *permissions)
{
  struct ny_ui_validation_s validation = { 0 };
  const cJSON *root;
  const cJSON *html;
  const cJSON *version;
  uint64_t permission;
  int ret;
  if (cJSON_GetObjectItemCaseSensitive(tree, "views") != NULL)
    return -EINVAL;
  root = cJSON_GetObjectItemCaseSensitive(tree, "root");
  html = cJSON_GetObjectItemCaseSensitive(tree, "html");
  version = cJSON_GetObjectItemCaseSensitive(tree, "nyaui");
  permission = html != NULL ? NY_PERMISSION_UI_HTML : NY_PERMISSION_UI_PAGE;
  if (!cJSON_IsObject(tree) || !cJSON_IsNumber(version) ||
      version->valuedouble != 1 || (html != NULL && root != NULL) ||
      (html != NULL && !cJSON_IsString(html)))
    {
      ret = -EINVAL;
      return ret;
    }

  if (html == NULL)
    {
      ret = ny_ui_check_strings(tree, 0);
      if (ret >= 0)
        {
          ret = ny_ui_check_node(root, 1, &validation);
        }
    }
  else
    {
      const cJSON *events = cJSON_GetObjectItemCaseSensitive(tree, "events");
      const cJSON *event;
      ret = events == NULL || cJSON_IsObject(events) ? 0 : -EINVAL;
      cJSON_ArrayForEach(event, events)
      {
        const char *command = ny_ui_string(event, "command");
        if (event->string == NULL || strlen(event->string) > 63 ||
            command == NULL || *command == '\0' || strlen(command) > 128 ||
            ny_ui_check_strings(event, 0) < 0)
          {
            ret = -EINVAL;
            break;
          }
      }
    }

  *permissions = permission;
  return ret;
}

/****************************************************************************
 * Name: ny_ui_validate_views
 ****************************************************************************/

static int ny_ui_validate_views(const cJSON *tree, uint64_t *permission)
{
  const cJSON *views = cJSON_GetObjectItemCaseSensitive(tree, "views");
  const cJSON *version = cJSON_GetObjectItemCaseSensitive(tree, "nyaui");
  const cJSON *view;
  if (views == NULL)
    return ny_ui_validate_view(tree, permission);
  if (!cJSON_IsObject(views) || !cJSON_IsNumber(version) ||
      version->valuedouble != 1 || cJSON_GetArraySize(views) < 1 ||
      cJSON_GetArraySize(views) > 2 ||
      cJSON_GetObjectItemCaseSensitive(tree, "root") != NULL ||
      cJSON_GetObjectItemCaseSensitive(tree, "html") != NULL)
    return -EINVAL;
  *permission = 0;
  cJSON_ArrayForEach(view, views)
  {
    uint64_t needed;
    int ret = ny_ui_validate_view(view, &needed);
    if (ret < 0)
      return ret;
    if (view->string == NULL || (*permission & needed) != 0 ||
        (needed == NY_PERMISSION_UI_PAGE &&
         strcmp(view->string, "component")) ||
        (needed == NY_PERMISSION_UI_HTML && strcmp(view->string, "html")))
      return -EINVAL;
    *permission |= needed;
  }
  return ny_ui_check_strings(cJSON_GetObjectItemCaseSensitive(tree, "page"),
                             0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_ui_publish(const struct ny_broker_client_s *client, uint32_t generation,
                  const char *json, size_t length)
{
  struct ny_ui_slot_s *slot;
  cJSON *tree;
  uint64_t permission;
  size_t index;
  int ret = ny_ui_check_document(json, length);
  if (ret < 0 || client == NULL || client->id == NULL || generation == 0)
    {
      return ret < 0 ? ret : -EINVAL;
    }

  const char *end = NULL;
  tree = cJSON_ParseWithLengthOpts(json, length, &end, false);
  while (end != NULL && end < json + length &&
         (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
    end++;
  if (end != json + length)
    {
      cJSON_Delete(tree);
      return -EINVAL;
    }
  if (tree == NULL)
    {
      return -EINVAL;
    }

  ret = ny_ui_validate_views(tree, &permission);

  if (ret < 0 || (client->permissions & permission) != permission)
    {
      ret = ret < 0 ? ret : -EACCES;
      goto out;
    }

  nxmutex_lock(&g_ui_lock);
  ret = ny_ui_authorized(client->id, permission);
  slot = ny_ui_find(client->id);
  if (ret == 0 && slot != NULL && slot->generation != generation)
    {
      ret = -EBUSY;
    }

  if (ret == 0 && slot == NULL)
    {
      for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
        {
          if (g_ui_slots[index].tree == NULL)
            {
              slot = &g_ui_slots[index];
              break;
            }
        }

      if (slot == NULL)
        {
          ret = -ENOSPC;
        }
    }

  if (ret == 0)
    {
      cJSON_Delete(slot->tree);
      strlcpy(slot->id, client->id, sizeof(slot->id));
      slot->generation = generation;
      slot->permission = permission;
      slot->tree = tree;
      tree = NULL;
      g_ui_revision++;
    }

  nxmutex_unlock(&g_ui_lock);
out:
  cJSON_Delete(tree);
  return ret;
}

void ny_ui_clear(const char *id, uint32_t generation)
{
  struct ny_ui_slot_s *slot;
  nxmutex_lock(&g_ui_lock);
  slot = ny_ui_find(id);
  if (slot != NULL && (generation == 0 || slot->generation == generation))
    {
      cJSON_Delete(slot->tree);
      memset(slot, 0, sizeof(*slot));
      g_ui_revision++;
    }

  nxmutex_unlock(&g_ui_lock);
}

uint64_t ny_ui_revision(void)
{
  uint64_t revision;
  nxmutex_lock(&g_ui_lock);
  revision = g_ui_revision;
  nxmutex_unlock(&g_ui_lock);
  return revision;
}

void ny_ui_changed(void)
{
  nxmutex_lock(&g_ui_lock);
  g_ui_revision++;
  nxmutex_unlock(&g_ui_lock);
}

int ny_ui_tree(const char *id, cJSON **tree)
{
  struct ny_scheduler_status_s status;
  struct ny_ui_slot_s *slot;
  int ret = ny_scheduler_status(id, &status);
  *tree = NULL;
  if (ret < 0 || strcmp(status.state, "running") != 0)
    {
      return -ENOENT;
    }

  nxmutex_lock(&g_ui_lock);
  slot = ny_ui_find(id);
  ret = slot != NULL && slot->generation == status.generation
            ? ny_ui_authorized(id, slot->permission)
            : -ENOENT;
  if (ret == 0)
    {
      *tree = cJSON_Duplicate(slot->tree, true);
      ret = *tree != NULL ? 0 : -ENOMEM;
    }

  nxmutex_unlock(&g_ui_lock);
  return ret;
}

int ny_ui_event(const char *id, const cJSON *event)
{
  const char *name = ny_ui_string(event, "event");
  const char *component = ny_ui_string(event, "componentId");
  const char *supplied = ny_ui_string(event, "command");
  const cJSON *binding = NULL;
  const cJSON *document;
  const cJSON *views;
  const cJSON *values = cJSON_GetObjectItemCaseSensitive(event, "values");
  const cJSON *arguments;
  const cJSON *item;
  const char *command;
  cJSON *tree = NULL;
  cJSON *message = NULL;
  cJSON *args = NULL;
  char *text = NULL;
  int ret;
  if (name == NULL || strlen(name) > 63 || ny_ui_check_strings(event, 0) < 0)
    {
      return -EINVAL;
    }

  ret = ny_ui_tree(id, &tree);
  if (ret < 0)
    {
      return ret;
    }

  document = tree;
  views = cJSON_GetObjectItemCaseSensitive(tree, "views");
  if (views != NULL)
    {
      const char *view_name = ny_ui_string(event, "view");
      if (view_name == NULL)
        view_name =
            cJSON_GetObjectItemCaseSensitive(views, "component") != NULL
                ? "component"
                : "html";
      document = cJSON_GetObjectItemCaseSensitive(views, view_name);
      if (document == NULL)
        {
          ret = -EACCES;
          goto out;
        }
    }

  if (cJSON_GetObjectItemCaseSensitive(document, "html") != NULL)
    {
      binding = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(document, "events"), name);
    }
  else if (component != NULL)
    {
      const cJSON *node = ny_ui_find_node(
          cJSON_GetObjectItemCaseSensitive(document, "root"), component);
      const cJSON *props = cJSON_GetObjectItemCaseSensitive(node, "props");
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "disabled")))
        {
          ret = -EACCES;
          goto out;
        }

      binding = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(node, "on"), name);
    }

  command = ny_ui_string(binding, "command");
  if (command == NULL || (supplied != NULL && strcmp(supplied, command) != 0))
    {
      ret = -EACCES;
      goto out;
    }

  message = cJSON_CreateObject();
  args = cJSON_CreateObject();
  if (message == NULL || args == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  arguments = cJSON_GetObjectItemCaseSensitive(binding, "args");
  cJSON_ArrayForEach(item, arguments)
  {
    const char *key = item->string;
    const cJSON *value = item;
    cJSON *copy;
    if (key == NULL)
      {
        ret = -EINVAL;
        goto out;
      }

    if (key[0] == '$' && cJSON_IsString(item))
      {
        key++;
        value = cJSON_GetObjectItemCaseSensitive(values, item->valuestring);
      }

    copy = value != NULL ? cJSON_Duplicate(value, true) : cJSON_CreateNull();
    if (copy == NULL || !cJSON_AddItemToObject(args, key, copy))
      {
        cJSON_Delete(copy);
        ret = -ENOMEM;
        goto out;
      }
  }

  if (!cJSON_AddStringToObject(message, "type", "ui.event") ||
      !cJSON_AddStringToObject(message, "command", command) ||
      !cJSON_AddStringToObject(message, "event", name) ||
      !cJSON_AddStringToObject(message, "componentId",
                               component != NULL ? component : "") ||
      !cJSON_AddItemToObject(message, "args", args))
    {
      ret = -ENOMEM;
      goto out;
    }

  args = NULL;
  if (views != NULL &&
      !cJSON_AddStringToObject(message, "view", document->string))
    {
      ret = -ENOMEM;
      goto out;
    }
  if (values != NULL)
    {
      cJSON *copy = cJSON_Duplicate(values, true);
      if (copy == NULL || !cJSON_AddItemToObject(message, "values", copy))
        {
          cJSON_Delete(copy);
          ret = -ENOMEM;
          goto out;
        }
    }

  text = cJSON_PrintUnformatted(message);
  ret = text == NULL ? -ENOMEM
        : strlen(text) >= CONFIG_NYABULA_CORE_EVENT_SIZE
            ? -EFBIG
            : ny_scheduler_dispatch(id, text);
out:
  free(text);
  cJSON_Delete(args);
  cJSON_Delete(message);
  cJSON_Delete(tree);
  return ret;
}
