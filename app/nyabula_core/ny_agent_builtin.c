/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_builtin.c
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

#include "agent_config.h"
#include "infra/config_store.h"
#include "ny_agent.h"
#include "tools/tool_fetch_url.h"
#include "tools/tool_files.h"
#include "tools/tool_shell.h"
#include "tools/tool_web_search.h"
#include <errno.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NY_AGENT_WORKSPACE   AGENT_DATA_DIR "/workspace"
#define NY_AGENT_TOOL_OUTPUT 4096
static mutex_t g_builtin_lock = NXMUTEX_INITIALIZER;

struct ny_agent_builtin_s
{
  const char *name;
  const char *description;
  const char *example;
  bool write;
  bool file;
  int (*execute)(const char *, char *, size_t);
};

static const struct ny_agent_builtin_s g_builtin[] = {
  { "read_file", "Read a workspace file", "{\"path\":\"notes.txt\"}", false,
    true, tool_read_file_execute },
  { "list_dir", "List workspace files", "{\"prefix\":\"\"}", false, true,
    tool_list_dir_execute },
  { "write_file", "Create or replace a workspace file",
    "{\"path\":\"notes.txt\",\"content\":\"text\"}", true, true,
    tool_write_file_execute },
  { "edit_file", "Replace the first matching text in a workspace file",
    "{\"path\":\"notes.txt\",\"old_string\":\"before\",\"new_string\":"
    "\"after\"}",
    true, true, tool_edit_file_execute },
  { "run_shell", "Run a command under the compiled upstream shell policy",
    "{\"command\":\"uptime\"}", true, false, tool_run_shell_execute },
  { "web_search", "Search using configured SerpAPI, Exa or Tavily",
    "{\"query\":\"openvela\"}", false, false, tool_web_search_execute },
  { "news_search", "Search using configured NewsAPI",
    "{\"query\":\"technology\",\"top_headlines\":false}", false, false,
    tool_news_search_execute },
  { "get_weather", "Read current weather using wttr.in",
    "{\"location\":\"Hong Kong\"}", false, false, tool_get_weather_execute },
  { "fetch_url", "Fetch a public HTTPS text URL",
    "{\"url\":\"https://example.com/\"}", false, false,
    tool_fetch_url_execute }
};
static const char *const g_provider_fields[] = { "serp", "exa", "tavily",
                                                 "news" };
static const char *const g_provider_keys[] = { AGENT_CFG_KEY_SERP_KEY,
                                               AGENT_CFG_KEY_EXA_KEY,
                                               AGENT_CFG_KEY_TAVILY_KEY,
                                               AGENT_CFG_KEY_NEWS_KEY };

static const char *ny_agent_builtin_string(const cJSON *data, const char *key);
static int ny_agent_builtin_catalog(cJSON **result);
static int ny_agent_builtin_providers(const char *topic, const cJSON *data,
                                      cJSON **result);
static int ny_agent_builtin_request(const char *topic, const cJSON *data,
                                    cJSON **result);

/****************************************************************************
 * Name: ny_agent_builtin_string
 ****************************************************************************/

static const char *ny_agent_builtin_string(const cJSON *data, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

/****************************************************************************
 * Name: ny_agent_builtin_catalog
 ****************************************************************************/

static int ny_agent_builtin_catalog(cJSON **result)
{
  *result = cJSON_CreateObject();
  cJSON *items = cJSON_AddArrayToObject(*result, "items");
  if (*result == NULL || items == NULL)
    return -ENOMEM;
  cJSON_AddStringToObject(*result, "workspace", NY_AGENT_WORKSPACE);
  cJSON_AddNumberToObject(*result, "shellPolicy", AGENT_SHELL_SECURITY);
  for (size_t i = 0; i < sizeof(g_builtin) / sizeof(g_builtin[0]); i++)
    {
      const struct ny_agent_builtin_s *tool = &g_builtin[i];
      cJSON *item = cJSON_CreateObject();
      if (item == NULL)
        return -ENOMEM;
      cJSON_AddStringToObject(item, "name", tool->name);
      cJSON_AddStringToObject(item, "description", tool->description);
      cJSON_AddBoolToObject(item, "approval", tool->write);
      cJSON_AddItemToObject(item, "example", cJSON_Parse(tool->example));
      if (!cJSON_AddItemToArray(items, item))
        {
          cJSON_Delete(item);
          return -ENOMEM;
        }
    }
  return 0;
}

/****************************************************************************
 * Name: ny_agent_builtin_providers
 ****************************************************************************/

static int ny_agent_builtin_providers(const char *topic, const cJSON *data,
                                      cJSON **result)
{
  char values[4][128] = { 0 };
  const char *pointers[4];
  for (size_t i = 0; i < 4; i++)
    {
      claw_config_get(g_provider_keys[i], values[i], sizeof(values[i]));
      const cJSON *value =
          cJSON_GetObjectItemCaseSensitive(data, g_provider_fields[i]);
      if (value != NULL &&
          (!cJSON_IsString(value) || strlen(value->valuestring) > 127))
        return -EINVAL;
      pointers[i] = value != NULL ? value->valuestring : values[i];
    }
  if (!strcmp(topic, "agent.tools.providers.save"))
    {
      if (claw_config_set_many(g_provider_keys, pointers, 4) != 0)
        return -EIO;
      tool_web_search_init();
    }
  else if (strcmp(topic, "agent.tools.providers.get"))
    return -ENOSYS;
  *result = cJSON_CreateObject();
  if (*result == NULL)
    return -ENOMEM;
  for (size_t i = 0; i < 4; i++)
    if (cJSON_AddBoolToObject(*result, g_provider_fields[i],
                              pointers[i][0] != 0) == NULL)
      return -ENOMEM;
  return 0;
}

/****************************************************************************
 * Name: ny_agent_builtin_request
 ****************************************************************************/

static int ny_agent_builtin_request(const char *topic, const cJSON *data,
                                    cJSON **result)
{
  if (!strcmp(topic, "agent.tools.catalog"))
    return ny_agent_builtin_catalog(result);
  if (!strncmp(topic, "agent.tools.providers.", 22))
    return ny_agent_builtin_providers(topic, data, result);
  bool write = !strcmp(topic, "agent.tools.call");
  if (!write && strcmp(topic, "agent.tools.read"))
    return -ENOSYS;
  const char *name = ny_agent_builtin_string(data, "name");
  const struct ny_agent_builtin_s *tool = NULL;
  for (size_t i = 0; i < sizeof(g_builtin) / sizeof(g_builtin[0]); i++)
    if (!strcmp(name, g_builtin[i].name))
      tool = &g_builtin[i];
  const cJSON *input = cJSON_GetObjectItemCaseSensitive(data, "arguments");
  if (tool == NULL || !cJSON_IsObject(input))
    return -EINVAL;
  if (tool->write != write)
    return -EACCES;
  cJSON *copy = cJSON_Duplicate(input, true);
  if (copy == NULL)
    return -ENOMEM;
  if (tool->file)
    {
      const char *field = !strcmp(name, "list_dir") ? "prefix" : "path";
      const char *relative = ny_agent_builtin_string(input, field);
      char absolute[256];
      if (strstr(relative, "..") || strchr(relative, '\\') ||
          relative[0] == '/' || (!relative[0] && strcmp(name, "list_dir")) ||
          snprintf(absolute, sizeof(absolute), "%s/%s", NY_AGENT_WORKSPACE,
                   relative) >= sizeof(absolute))
        {
          cJSON_Delete(copy);
          return -EINVAL;
        }
      mkdir(NY_AGENT_WORKSPACE, 0755);
      cJSON_DeleteItemFromObjectCaseSensitive(copy, field);
      if (cJSON_AddStringToObject(copy, field, absolute) == NULL)
        {
          cJSON_Delete(copy);
          return -ENOMEM;
        }
    }
  char *encoded = cJSON_PrintUnformatted(copy);
  cJSON_Delete(copy);
  char *output = calloc(1, NY_AGENT_TOOL_OUTPUT);
  if (encoded == NULL || output == NULL)
    {
      free(encoded);
      free(output);
      return -ENOMEM;
    }
  int ret = tool->execute(encoded, output, NY_AGENT_TOOL_OUTPUT);
  free(encoded);
  *result = cJSON_CreateObject();
  bool valid = *result != NULL &&
               cJSON_AddStringToObject(*result, "text", output) != NULL;
  valid &= cJSON_AddBoolToObject(*result, "ok", ret == 0) != NULL;
  free(output);
  return !valid ? -ENOMEM : ret == 0 ? 0 : -EIO;
}

/****************************************************************************
 * Name: ny_agent_builtin
 ****************************************************************************/

int ny_agent_builtin(const char *topic, const cJSON *data, cJSON **result)
{
  nxmutex_lock(&g_builtin_lock);
  int ret = ny_agent_builtin_request(topic, data, result);
  nxmutex_unlock(&g_builtin_lock);
  return ret;
}
