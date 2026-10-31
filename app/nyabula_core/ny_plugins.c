/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_plugins.c
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
#include <mbedtls/base64.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ny_package.h"
#include "ny_permission.h"
#include "ny_plugin_upload.h"
#include "ny_plugins.h"
#include "ny_scheduler.h"
#include "ny_ui.h"
#ifdef CONFIG_NYABULA_CORE_AUTHOR
#include "ny_author.h"
#endif

static mutex_t g_plugins_lock = NXMUTEX_INITIALIZER;

static const char *ny_plugins_string(const cJSON *data, const char *name);
static int ny_plugins_visit(const char *id, const char *version,
                            const char *path, bool current, bool last_good,
                            void *opaque);
static int ny_plugins_list(cJSON **result);
static int ny_plugins_icon(const char *id, cJSON **result);
static int ny_plugins_start(const char *id);
static int ny_plugins_permission(const char *id, const char *version,
                                 const char *permission, bool grant);

/****************************************************************************
 * Name: ny_plugins_string
 ****************************************************************************/

static const char *ny_plugins_string(const cJSON *data, const char *name)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, name);
  return cJSON_IsString(value) ? value->valuestring : NULL;
}

/****************************************************************************
 * Name: ny_plugins_visit
 ****************************************************************************/

static int ny_plugins_visit(const char *id, const char *version,
                            const char *path, bool current, bool last_good,
                            void *opaque)
{
  cJSON *plugins = opaque;
  cJSON *plugin = NULL;
  cJSON *candidate;
  cJSON *versions;
  cJSON *entry;
  cJSON *permissions;
  cJSON *ui;
  struct ny_plugin_config_s config;
  struct ny_scheduler_status_s status;
  uint64_t bit;
  int ret;

  cJSON_ArrayForEach(candidate, plugins)
  {
    const char *candidate_id = ny_plugins_string(candidate, "id");
    if (candidate_id != NULL && strcmp(candidate_id, id) == 0)
      {
        plugin = candidate;
        break;
      }
  }

  if (plugin == NULL)
    {
      plugin = cJSON_CreateObject();
      if (plugin == NULL || !cJSON_AddItemToArray(plugins, plugin))
        {
          cJSON_Delete(plugin);
          return -ENOMEM;
        }

      if (!cJSON_AddStringToObject(plugin, "id", id) ||
          !cJSON_AddStringToObject(plugin, "name", id) ||
          !cJSON_AddArrayToObject(plugin, "versions"))
        {
          return -ENOMEM;
        }
    }

  versions = cJSON_GetObjectItemCaseSensitive(plugin, "versions");
  entry = cJSON_CreateObject();
  if (entry == NULL || !cJSON_AddItemToArray(versions, entry))
    {
      cJSON_Delete(entry);
      return -ENOMEM;
    }

  if (!cJSON_AddStringToObject(entry, "version", version) ||
      !cJSON_AddBoolToObject(entry, "current", current) ||
      !cJSON_AddBoolToObject(entry, "lastGood", last_good))
    {
      return -ENOMEM;
    }

  /* Prefer the active version, independent of directory enumeration order. */

  if (!current && cJSON_GetObjectItemCaseSensitive(plugin, "version") != NULL)
    {
      return 0;
    }

  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "version");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "active");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "state");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "permissions");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "ui");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "lastError");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "runtime");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "runtimeVersion");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "queued");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "signatureValid");
  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "icon");

  ret = ny_manifest_load(path, &config);
  ny_scheduler_status(id, &status);
  permissions = cJSON_AddArrayToObject(plugin, "permissions");
  ui = cJSON_AddArrayToObject(plugin, "ui");
  if (permissions == NULL || ui == NULL ||
      !cJSON_AddStringToObject(plugin, "version", version) ||
      !cJSON_AddBoolToObject(plugin, "active", current) ||
      !cJSON_AddBoolToObject(plugin, "signatureValid", ret == 0) ||
      !cJSON_AddStringToObject(plugin, "state",
                               ret < 0 ? "invalid" : status.state) ||
      !cJSON_AddNumberToObject(plugin, "lastError",
                               ret < 0 ? ret : status.last_error) ||
      !cJSON_AddStringToObject(plugin, "runtimeVersion", status.version) ||
      !cJSON_AddNumberToObject(plugin, "queued", status.queued))
    {
      return -ENOMEM;
    }

  if (ret < 0)
    {
      return 0;
    }

  cJSON_DeleteItemFromObjectCaseSensitive(plugin, "name");
  if (!cJSON_AddStringToObject(plugin, "name", config.name))
    {
      return -ENOMEM;
    }

  if (config.icon_type[0] != '\0')
    {
      cJSON *icon = cJSON_AddObjectToObject(plugin, "icon");
      if (icon == NULL ||
          !cJSON_AddStringToObject(icon, "type", config.icon_type) ||
          !cJSON_AddStringToObject(icon, "path", config.icon) ||
          !cJSON_AddStringToObject(icon, "codepoint", config.icon_codepoint))
        {
          return -ENOMEM;
        }
    }
  else if (!cJSON_AddStringToObject(plugin, "icon", config.icon))
    {
      return -ENOMEM;
    }

  if (!cJSON_AddStringToObject(
          plugin, "runtime",
          config.runtime == NY_PLUGIN_RUNTIME_QUICKJS ? "quickjs" : "wamr"))
    {
      return -ENOMEM;
    }

  for (bit = 1; bit != 0; bit <<= 1)
    {
      const char *name = ny_manifest_permission_name(bit);
      if ((config.requested_permissions & bit) == 0 || name == NULL)
        {
          continue;
        }

      entry = cJSON_CreateObject();
      if (entry == NULL || !cJSON_AddItemToArray(permissions, entry))
        {
          cJSON_Delete(entry);
          return -ENOMEM;
        }

      if (!cJSON_AddStringToObject(entry, "name", name) ||
          !cJSON_AddBoolToObject(entry, "granted",
                                 (config.permissions & bit) != 0))
        {
          return -ENOMEM;
        }
    }

  if ((config.requested_permissions &
       (NY_PERMISSION_UI_PAGE | NY_PERMISSION_UI_HTML)) != 0)
    {
      cJSON *page = cJSON_CreateString("page");
      if (page == NULL || !cJSON_AddItemToArray(ui, page))
        {
          cJSON_Delete(page);
          return -ENOMEM;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: ny_plugins_list
 ****************************************************************************/

static int ny_plugins_list(cJSON **result)
{
  cJSON *list;
  int ret;
  *result = cJSON_CreateObject();
  list = *result != NULL ? cJSON_AddArrayToObject(*result, "plugins") : NULL;
  if (list == NULL)
    {
      return -ENOMEM;
    }

  ret = ny_package_visit(ny_plugins_visit, list);
  return ret;
}

/****************************************************************************
 * Name: ny_plugins_icon
 *
 * Description:
 *   Read only the signed active package's declared icon. Lazy retrieval keeps
 *   the plugin inventory bounded independently of the number of image icons.
 ****************************************************************************/

static int ny_plugins_icon(const char *id, cJSON **result)
{
  struct ny_plugin_config_s config;
  struct stat status;
  char root[PATH_MAX];
  char path[PATH_MAX];
  unsigned char *bytes = NULL;
  unsigned char *encoded = NULL;
  const char *mime = NULL;
  FILE *stream = NULL;
  size_t length;
  size_t encoded_length;
  char *part;
  int ret = ny_package_resolve(id, root, sizeof(root));

  if (ret < 0 || (ret = ny_manifest_load(root, &config)) < 0)
    {
      return ret;
    }

  if (config.icon_type[0] == '\0')
    {
      return -ENOENT;
    }

  ret = snprintf(path, sizeof(path), "%s/%s", root, config.icon);
  if (ret < 0 || ret >= (int)sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  for (part = path + strlen(root) + 1;; part++)
    {
      if (*part == '/' || *part == '\0')
        {
          char saved = *part;
          *part = '\0';
          ret = lstat(path, &status);
          *part = saved;
          if (ret < 0 || S_ISLNK(status.st_mode))
            {
              return ret < 0 ? -errno : -EACCES;
            }

          if (saved == '\0')
            {
              break;
            }
        }
    }

  if (!S_ISREG(status.st_mode) || status.st_size < 12 ||
      status.st_size > NY_PLUGIN_ICON_ASSET_LIMIT)
    {
      return -EFBIG;
    }

  length = status.st_size;
  bytes = malloc(length);
  encoded = malloc(4 * ((length + 2) / 3) + 1);
  if (bytes == NULL || encoded == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  stream = fopen(path, "rb");
  if (stream == NULL || fread(bytes, 1, length, stream) != length)
    {
      ret = stream == NULL ? -errno : -EIO;
      goto out;
    }

  if (strcmp(config.icon_type, "iconfont") == 0)
    {
      if (length >= 48 && memcmp(bytes, "wOF2", 4) == 0)
        {
          mime = "font/woff2";
        }
    }
  else if (memcmp(bytes, "\211PNG\r\n\032\n", 8) == 0)
    {
      mime = "image/png";
    }
  else if (bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
    {
      mime = "image/jpeg";
    }
  else if (memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WEBP", 4) == 0)
    {
      mime = "image/webp";
    }

  if (mime == NULL)
    {
      ret = -ENOTSUP;
      goto out;
    }

  ret = mbedtls_base64_encode(encoded, 4 * ((length + 2) / 3) + 1,
                              &encoded_length, bytes, length);
  if (ret != 0)
    {
      ret = -EIO;
      goto out;
    }

  *result = cJSON_CreateObject();
  ret = *result != NULL &&
                cJSON_AddStringToObject(*result, "version", config.version) &&
                cJSON_AddStringToObject(*result, "mime", mime) &&
                cJSON_AddStringToObject(*result, "data", (const char *)encoded)
            ? 0
            : -ENOMEM;
out:
  if (stream != NULL)
    {
      fclose(stream);
    }

  free(encoded);
  free(bytes);
  return ret;
}

/****************************************************************************
 * Name: ny_plugins_start
 ****************************************************************************/

static int ny_plugins_start(const char *id)
{
  struct ny_scheduler_status_s status;
  char path[PATH_MAX];
  char storage[PATH_MAX];
  int ret = ny_scheduler_status(id, &status);
  if (ret == 0)
    {
      if (strcmp(status.state, "running") == 0)
        {
          return 0;
        }

      ret = ny_scheduler_stop(id);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = ny_package_resolve(id, path, sizeof(path));
  if (ret == 0)
    {
      ret = ny_package_storage_root(id, storage, sizeof(storage));
    }

  return ret < 0 ? ret : ny_scheduler_start_installed(path, storage);
}

/****************************************************************************
 * Name: ny_plugins_permission
 ****************************************************************************/

static int ny_plugins_permission(const char *id, const char *version,
                                 const char *permission, bool grant)
{
  struct ny_plugin_config_s config;
  char path[PATH_MAX];
  uint64_t bit;
  int ret = ny_manifest_permission_value(permission, &bit);
  if (ret < 0)
    {
      return ret;
    }

  ret = version != NULL
            ? ny_package_resolve_version(id, version, path, sizeof(path))
            : ny_package_resolve(id, path, sizeof(path));
  if (ret == 0)
    {
      ret = ny_manifest_load(path, &config);
    }

  if (ret < 0 || (config.requested_permissions & bit) == 0)
    {
      return ret < 0 ? ret : -EACCES;
    }

  ret = grant ? ny_permission_grant(id, permission)
              : ny_permission_revoke(id, permission);
  if (!grant && ret == -ENOENT)
    {
      ret = 0;
    }

  if (ret == 0)
    {
      ret = ny_scheduler_refresh_permissions(id);
      if (!grant &&
          (bit == NY_PERMISSION_UI_PAGE || bit == NY_PERMISSION_UI_HTML))
        {
          ny_ui_clear(id, 0);
        }
    }

  return ret;
}

/****************************************************************************
 * Name: ny_plugins_request
 ****************************************************************************/

int ny_plugins_request(const struct ny_product_caller_s *caller,
                       const char *topic, const cJSON *data, cJSON **result)
{
  const char *id = ny_plugins_string(data, "pluginId");
  const char *version = ny_plugins_string(data, "version");
  struct ny_scheduler_status_s status;
  int ret = -ENOSYS;

#ifdef CONFIG_NYABULA_CORE_AUTHOR
  if (!strncmp(topic, "plugin.author.", 14))
    {
      if (caller->role != NY_PRODUCT_OWNER)
        return -EACCES;
      nxmutex_lock(&g_plugins_lock);
      ret = ny_author_recover();
      if (ret == 0)
        ret = ny_author_request(caller, topic, data, result);
      nxmutex_unlock(&g_plugins_lock);
      return ret;
    }
#endif

  if (strcmp(topic, "plugin.list") == 0)
    {
      nxmutex_lock(&g_plugins_lock);
      ret = ny_plugins_list(result);
      nxmutex_unlock(&g_plugins_lock);
      return ret;
    }

  if (strcmp(topic, "plugin.icon") == 0)
    {
      nxmutex_lock(&g_plugins_lock);
      ret = id != NULL ? ny_plugins_icon(id, result) : -EINVAL;
      nxmutex_unlock(&g_plugins_lock);
      return ret;
    }

  if (strncmp(topic, "plugin.", 7) != 0 && strncmp(topic, "ui.", 3) != 0)
    {
      return -ENOSYS;
    }

  if (strcmp(topic, "ui.tree") == 0)
    {
      return id != NULL ? ny_ui_tree(id, result) : -EINVAL;
    }

  if (strcmp(topic, "ui.event") == 0)
    {
      if (caller->role < NY_PRODUCT_FAMILY)
        {
          return -EACCES;
        }

      nxmutex_lock(&g_plugins_lock);
      ret = id != NULL ? ny_ui_event(id, data) : -EINVAL;
      nxmutex_unlock(&g_plugins_lock);
      if (ret == 0)
        {
          *result = cJSON_CreateObject();
          if (*result == NULL ||
              !cJSON_AddBoolToObject(*result, "queued", true))
            {
              return -ENOMEM;
            }
        }

      return ret;
    }

  if (caller->role != NY_PRODUCT_OWNER)
    {
      return -EACCES;
    }

  nxmutex_lock(&g_plugins_lock);
#ifdef CONFIG_NYABULA_CORE_AUTHOR
  ret = ny_author_recover();
  if (ret < 0)
    goto out;
#endif
  if (strncmp(topic, "plugin.upload.", 14) == 0)
    {
      ret = ny_plugin_upload_request(caller, topic, data, result);
      if (ret == 0 && strcmp(topic, "plugin.upload.commit") == 0)
        {
          ny_ui_changed();
        }

      goto out;
    }
  else if (strcmp(topic, "plugin.install") == 0)
    {
      const char *path = ny_plugins_string(data, "path");
      struct ny_plugin_config_s config;
      char active[PATH_MAX];
      if (!caller->local_transport || path == NULL)
        {
          ret = -EACCES;
          goto out;
        }

      ret = ny_manifest_load(path, &config);
      if (ret == 0)
        {
          ret = ny_package_install(path);
        }

      if (ret == 0 &&
          ny_package_resolve(config.id, active, sizeof(active)) == -ENOENT)
        {
          ret = ny_package_activate(config.id, config.version);
        }
    }
  else if (id == NULL)
    {
      ret = -EINVAL;
    }
  else if (strcmp(topic, "plugin.start") == 0)
    {
      ret = ny_plugins_start(id);
    }
  else if (strcmp(topic, "plugin.stop") == 0)
    {
      ret = ny_scheduler_stop(id);
      if (ret == -ENOENT)
        {
          ret = 0;
        }
    }
  else if (strcmp(topic, "plugin.activate") == 0 ||
           strcmp(topic, "plugin.rollback") == 0)
    {
      if (ny_scheduler_status(id, &status) == 0 &&
          strcmp(status.state, "running") == 0)
        {
          ret = -EBUSY;
        }
      else
        {
          ret = strcmp(topic, "plugin.rollback") == 0 ? ny_package_rollback(id)
                : version != NULL ? ny_package_activate(id, version)
                                  : -EINVAL;
        }
    }
  else if (strcmp(topic, "plugin.grant") == 0 ||
           strcmp(topic, "plugin.revoke") == 0)
    {
      const char *permission = ny_plugins_string(data, "permission");
      ret = permission != NULL
                ? ny_plugins_permission(id, version, permission,
                                        strcmp(topic, "plugin.grant") == 0)
                : -EINVAL;
    }

  if (ret == 0)
    {
      ny_ui_changed();
      ret = ny_plugins_list(result);
    }

out:
  nxmutex_unlock(&g_plugins_lock);
  return ret;
}
