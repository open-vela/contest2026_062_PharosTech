/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_manifest.c
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

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netutils/cJSON.h>

#include "ny_manifest.h"
#include "ny_permission.h"
#include "ny_revocation.h"
#include "ny_signature.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_permission_name_s
{
  const char *name;
  uint64_t value;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct ny_permission_name_s g_permission_names[] = {
  { "core.log", NY_PERMISSION_CORE_LOG },
  { "storage.read", NY_PERMISSION_STORAGE_READ },
  { "storage.write", NY_PERMISSION_STORAGE_WRITE },
  { "network.request", NY_PERMISSION_NETWORK_REQUEST },
  { "ui.notify", NY_PERMISSION_UI_NOTIFY },
  { "ai.invoke", NY_PERMISSION_AI_INVOKE },
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_manifest_read_file(const char *path, char **content);
static bool ny_manifest_valid_id(const char *id);
static bool ny_manifest_valid_entry_path(const char *entry);
static int ny_manifest_member_count(const cJSON *object, const char *name);
static int ny_manifest_copy_string(const cJSON *object, const char *name,
                                   char *destination, size_t size);
static int ny_manifest_parse_permissions(const cJSON *root,
                                         uint64_t *permissions);
static int ny_manifest_parse_limits(const cJSON *root,
                                    struct ny_plugin_config_s *config);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ny_manifest_read_file(const char *path, char **content)
{
  FILE *stream;
  char *buffer;
  long size;
  size_t count;

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      return -errno;
    }

  if (fseek(stream, 0, SEEK_END) < 0 || (size = ftell(stream)) < 0 ||
      fseek(stream, 0, SEEK_SET) < 0)
    {
      int error = errno;
      fclose(stream);
      return -error;
    }

  if (size <= 0 || size > CONFIG_NYABULA_CORE_MANIFEST_LIMIT)
    {
      fclose(stream);
      return -EFBIG;
    }

  buffer = malloc((size_t)size + 1);
  if (buffer == NULL)
    {
      fclose(stream);
      return -ENOMEM;
    }

  count = fread(buffer, 1, (size_t)size, stream);
  fclose(stream);
  if (count != (size_t)size)
    {
      free(buffer);
      return -EIO;
    }

  buffer[count] = '\0';
  *content = buffer;
  return 0;
}

static bool ny_manifest_valid_id(const char *id)
{
  size_t index;
  size_t length = strlen(id);

  if (length == 0 || length >= NY_PLUGIN_ID_SIZE || id[0] == '.' ||
      id[length - 1] == '.')
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      unsigned char value = (unsigned char)id[index];

      if (!(islower(value) || isdigit(value) || value == '.' || value == '_' ||
            value == '-'))
        {
          return false;
        }
    }

  return true;
}

static bool ny_manifest_valid_entry_path(const char *entry)
{
  size_t length = strlen(entry);

  return length > 0 && length < PATH_MAX && entry[0] != '/' &&
         strchr(entry, '\\') == NULL && strstr(entry, "//") == NULL &&
         strcmp(entry, ".") != 0 && strcmp(entry, "..") != 0 &&
         strncmp(entry, "../", 3) != 0 && strstr(entry, "/../") == NULL &&
         entry[length - 1] != '/';
}

static int ny_manifest_member_count(const cJSON *object, const char *name)
{
  const cJSON *child;
  int count = 0;

  cJSON_ArrayForEach(child, object)
  {
    if (child->string != NULL && strcmp(child->string, name) == 0)
      {
        count++;
      }
  }

  return count;
}

static int ny_manifest_copy_string(const cJSON *object, const char *name,
                                   char *destination, size_t size)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

  if (!cJSON_IsString(item) || item->valuestring == NULL ||
      strlen(item->valuestring) >= size)
    {
      return -EINVAL;
    }

  strlcpy(destination, item->valuestring, size);
  return 0;
}

static int ny_manifest_parse_permissions(const cJSON *root,
                                         uint64_t *permissions)
{
  const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "permissions");
  const cJSON *item;

  if (array == NULL)
    {
      *permissions = 0;
      return 0;
    }

  if (!cJSON_IsArray(array))
    {
      return -EINVAL;
    }

  *permissions = 0;
  cJSON_ArrayForEach(item, array)
  {
    uint64_t value;

    if (!cJSON_IsString(item) || item->valuestring == NULL)
      {
        return -EINVAL;
      }

    if (ny_manifest_permission_value(item->valuestring, &value) < 0)
      {
        return -ENOTSUP;
      }

    *permissions |= value;
  }

  return 0;
}

static int ny_manifest_parse_limits(const cJSON *root,
                                    struct ny_plugin_config_s *config)
{
  const cJSON *limits = cJSON_GetObjectItemCaseSensitive(root, "limits");
  const cJSON *item;
  double value;

  if (limits == NULL)
    {
      return 0;
    }

  if (!cJSON_IsObject(limits))
    {
      return -EINVAL;
    }

  cJSON_ArrayForEach(item, limits)
  {
    if (item->string == NULL || (strcmp(item->string, "memoryKiB") != 0 &&
                                 strcmp(item->string, "stackKiB") != 0 &&
                                 strcmp(item->string, "cpuMsPerEvent") != 0 &&
                                 strcmp(item->string, "eventsPerMinute") != 0))
      {
        return -EINVAL;
      }
  }

  item = cJSON_GetObjectItemCaseSensitive(limits, "memoryKiB");
  if (item != NULL)
    {
      if (ny_manifest_member_count(limits, "memoryKiB") != 1 ||
          !cJSON_IsNumber(item))
        {
          return -EINVAL;
        }

      value = cJSON_GetNumberValue(item);
      if (value < 64 || value > CONFIG_NYABULA_CORE_PLUGIN_MEMORY / 1024 ||
          value != (double)(size_t)value)
        {
          return -ERANGE;
        }

      config->memory_limit = (size_t)value * 1024;
    }

  item = cJSON_GetObjectItemCaseSensitive(limits, "stackKiB");
  if (item != NULL)
    {
      if (ny_manifest_member_count(limits, "stackKiB") != 1 ||
          !cJSON_IsNumber(item))
        {
          return -EINVAL;
        }

      value = cJSON_GetNumberValue(item);
      if (value < 16 || value > CONFIG_NYABULA_CORE_PLUGIN_STACK / 1024 ||
          value != (double)(size_t)value)
        {
          return -ERANGE;
        }

      config->stack_limit = (size_t)value * 1024;
    }

  item = cJSON_GetObjectItemCaseSensitive(limits, "cpuMsPerEvent");
  if (item != NULL)
    {
      if (ny_manifest_member_count(limits, "cpuMsPerEvent") != 1 ||
          !cJSON_IsNumber(item))
        {
          return -EINVAL;
        }

      value = cJSON_GetNumberValue(item);
      if (value < 1 || value > CONFIG_NYABULA_CORE_EVENT_TIMEOUT_MS ||
          value != (double)(uint32_t)value)
        {
          return -ERANGE;
        }

      config->event_timeout_ms = (uint32_t)value;
    }

  item = cJSON_GetObjectItemCaseSensitive(limits, "eventsPerMinute");
  if (item != NULL)
    {
      if (ny_manifest_member_count(limits, "eventsPerMinute") != 1 ||
          !cJSON_IsNumber(item))
        {
          return -EINVAL;
        }

      value = cJSON_GetNumberValue(item);
      if (value < 1 || value > CONFIG_NYABULA_CORE_MAX_EVENTS_PER_MINUTE ||
          value != (double)(uint32_t)value)
        {
          return -ERANGE;
        }

      config->events_per_minute = (uint32_t)value;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ny_manifest_default_config(struct ny_plugin_config_s *config,
                                const char *entry)
{
  memset(config, 0, sizeof(*config));
  strlcpy(config->id, "local.raw", sizeof(config->id));
  strlcpy(config->version, "0", sizeof(config->version));
  strlcpy(config->entry, entry, sizeof(config->entry));
  config->runtime = NY_PLUGIN_RUNTIME_QUICKJS;
  config->permissions = NY_PERMISSION_CORE_LOG;
  config->requested_permissions = NY_PERMISSION_CORE_LOG;
  config->module = false;
  config->memory_limit = CONFIG_NYABULA_CORE_PLUGIN_MEMORY;
  config->stack_limit = CONFIG_NYABULA_CORE_PLUGIN_STACK;
  config->event_timeout_ms = CONFIG_NYABULA_CORE_EVENT_TIMEOUT_MS;
  config->events_per_minute = CONFIG_NYABULA_CORE_EVENTS_PER_MINUTE;
}

int ny_manifest_load(const char *package_path,
                     struct ny_plugin_config_s *config)
{
  char path[PATH_MAX];
  char *content = NULL;
  const char *parse_end;
  const cJSON *item;
  cJSON *root = NULL;
  int ret;

  if (package_path == NULL || config == NULL || package_path[0] == '\0' ||
      strlen(package_path) >= PATH_MAX)
    {
      return -EINVAL;
    }

#ifdef CONFIG_NYABULA_CORE_REQUIRE_SIGNATURE
  ret = ny_signature_verify_package(package_path);
  if (ret < 0)
    {
      return ret;
    }
#endif

  memset(config, 0, sizeof(*config));
  ret = snprintf(path, sizeof(path), "%s/manifest.json", package_path);
  if (ret < 0 || ret >= (int)sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  ret = ny_manifest_read_file(path, &content);
  if (ret < 0)
    {
      return ret;
    }

  root = cJSON_ParseWithOpts(content, &parse_end, true);
  free(content);
  if (!cJSON_IsObject(root))
    {
      ret = -EINVAL;
      goto out;
    }

  if (ny_manifest_member_count(root, "id") != 1 ||
      ny_manifest_member_count(root, "version") != 1 ||
      ny_manifest_member_count(root, "entry") != 1 ||
      ny_manifest_member_count(root, "runtime") != 1 ||
      ny_manifest_member_count(root, "apiVersion") != 1 ||
      ny_manifest_member_count(root, "background") > 1 ||
      ny_manifest_member_count(root, "permissions") > 1 ||
      ny_manifest_member_count(root, "limits") > 1)
    {
      ret = -EINVAL;
      goto out;
    }

  ret = ny_manifest_copy_string(root, "id", config->id, sizeof(config->id));
  if (ret < 0 || !ny_manifest_valid_id(config->id))
    {
      ret = -EINVAL;
      goto out;
    }

  ret = ny_manifest_copy_string(root, "version", config->version,
                                sizeof(config->version));
  if (ret < 0)
    {
      goto out;
    }

  if (config->version[0] == '\0')
    {
      ret = -EINVAL;
      goto out;
    }

  ret = ny_manifest_copy_string(root, "entry", config->entry,
                                sizeof(config->entry));
  if (ret < 0 || !ny_manifest_valid_entry_path(config->entry))
    {
      ret = -EINVAL;
      goto out;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "runtime");
  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      ret = -EINVAL;
      goto out;
    }

  if (strcmp(item->valuestring, "quickjs") == 0)
    {
      config->runtime = NY_PLUGIN_RUNTIME_QUICKJS;
      if (strlen(config->entry) < 3 ||
          strcmp(config->entry + strlen(config->entry) - 3, ".js") != 0)
        {
          ret = -EINVAL;
          goto out;
        }
    }
  else if (strcmp(item->valuestring, "wamr") == 0)
    {
#ifdef CONFIG_NYABULA_CORE_WAMR
      config->runtime = NY_PLUGIN_RUNTIME_WAMR;
      if (strlen(config->entry) < 5 ||
          strcmp(config->entry + strlen(config->entry) - 5, ".wasm") != 0)
        {
          ret = -EINVAL;
          goto out;
        }
#else
      ret = -ENOTSUP;
      goto out;
#endif
    }
  else
    {
      ret = -ENOTSUP;
      goto out;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "apiVersion");
  if (!cJSON_IsNumber(item) || item->valuedouble != NY_PLUGIN_API_VERSION)
    {
      ret = -EPROTONOSUPPORT;
      goto out;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "background");
  if (item != NULL && !cJSON_IsBool(item))
    {
      ret = -EINVAL;
      goto out;
    }

  config->background = cJSON_IsTrue(item);

  ret = ny_manifest_parse_permissions(root, &config->requested_permissions);
  if (ret < 0)
    {
      goto out;
    }

  config->memory_limit = CONFIG_NYABULA_CORE_PLUGIN_MEMORY;
  config->stack_limit = CONFIG_NYABULA_CORE_PLUGIN_STACK;
  config->event_timeout_ms = CONFIG_NYABULA_CORE_EVENT_TIMEOUT_MS;
  config->events_per_minute = CONFIG_NYABULA_CORE_EVENTS_PER_MINUTE;
  config->module = true;
  ret = ny_manifest_parse_limits(root, config);
  if (ret < 0)
    {
      goto out;
    }

  ret = ny_revocation_check_package(config->id, config->version);
  if (ret < 0)
    {
      goto out;
    }

  strlcpy(config->root, package_path, sizeof(config->root));
  strlcpy(config->storage_root, package_path, sizeof(config->storage_root));
  ret = snprintf(path, sizeof(path), "%s/%s", package_path, config->entry);
  if (ret < 0 || ret >= (int)sizeof(path))
    {
      ret = -ENAMETOOLONG;
      goto out;
    }

  strlcpy(config->entry, path, sizeof(config->entry));
  ret = ny_permission_apply(config);

out:
  cJSON_Delete(root);
  return ret;
}

int ny_manifest_permission_value(const char *name, uint64_t *value)
{
  size_t index;

  if (name == NULL || value == NULL)
    {
      return -EINVAL;
    }

  for (index = 0;
       index < sizeof(g_permission_names) / sizeof(g_permission_names[0]);
       index++)
    {
      if (strcmp(name, g_permission_names[index].name) == 0)
        {
          *value = g_permission_names[index].value;
          return 0;
        }
    }

  return -ENOENT;
}

const char *ny_manifest_permission_name(uint64_t value)
{
  size_t index;

  for (index = 0;
       index < sizeof(g_permission_names) / sizeof(g_permission_names[0]);
       index++)
    {
      if (value == g_permission_names[index].value)
        {
          return g_permission_names[index].name;
        }
    }

  return NULL;
}
