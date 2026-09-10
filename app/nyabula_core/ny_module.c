/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_module.c
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

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ny_capability.h"
#include "ny_module.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ny_module_is_capability(const char *name);
static bool ny_module_safe_relative(const char *name);
static bool ny_module_inside_root(struct ny_plugin_s *plugin,
                                  const char *path);
static char *ny_module_normalize(JSContext *context, const char *base_name,
                                 const char *module_name, void *opaque);
static int ny_module_read(JSContext *context, const char *path, char **source,
                          size_t *length);
static JSModuleDef *ny_module_load(JSContext *context, const char *module_name,
                                   void *opaque);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ny_module_is_capability(const char *name)
{
  return strcmp(name, "@nyabula/core") == 0 ||
         strcmp(name, "@nyabula/storage") == 0 ||
         strcmp(name, "@nyabula/network") == 0 ||
         strcmp(name, "@nyabula/ui") == 0 || strcmp(name, "@nyabula/ai") == 0;
}

static bool ny_module_safe_relative(const char *name)
{
  size_t length = strlen(name);

  return length >= 3 && name[0] != '/' && strchr(name, '\\') == NULL &&
         strchr(name, ':') == NULL && strstr(name, "//") == NULL &&
         strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
         strncmp(name, "../", 3) != 0 && strstr(name, "/../") == NULL &&
         strstr(name, "/./") == NULL && strcmp(name + length - 3, ".js") == 0;
}

static bool ny_module_inside_root(struct ny_plugin_s *plugin, const char *path)
{
  size_t root_length = strlen(plugin->root);

  return root_length > 0 && strncmp(path, plugin->root, root_length) == 0 &&
         path[root_length] == '/';
}

static char *ny_module_normalize(JSContext *context, const char *base_name,
                                 const char *module_name, void *opaque)
{
  struct ny_plugin_s *plugin = opaque;
  const char *separator;
  const char *relative;
  char path[PATH_MAX];
  char *normalized;
  size_t directory_length;
  int ret;

  if (ny_module_is_capability(module_name))
    {
      normalized = js_malloc(context, strlen(module_name) + 1);
      if (normalized != NULL)
        {
          strcpy(normalized, module_name);
        }

      return normalized;
    }

  if (module_name[0] != '.' || module_name[1] != '/' ||
      !ny_module_safe_relative(module_name + 2) ||
      !ny_module_inside_root(plugin, base_name))
    {
      JS_ThrowReferenceError(context, "module path denied: %s", module_name);
      return NULL;
    }

  separator = strrchr(base_name, '/');
  if (separator == NULL)
    {
      JS_ThrowReferenceError(context, "invalid module base");
      return NULL;
    }

  relative = module_name + 2;
  directory_length = (size_t)(separator - base_name);
  ret = snprintf(path, sizeof(path), "%.*s/%s", (int)directory_length,
                 base_name, relative);
  if (ret < 0 || ret >= (int)sizeof(path) ||
      !ny_module_inside_root(plugin, path))
    {
      JS_ThrowReferenceError(context, "module path too long");
      return NULL;
    }

  normalized = js_malloc(context, (size_t)ret + 1);
  if (normalized != NULL)
    {
      memcpy(normalized, path, (size_t)ret + 1);
    }

  return normalized;
}

static int ny_module_read(JSContext *context, const char *path, char **source,
                          size_t *length)
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

  if (size <= 0 || size > CONFIG_NYABULA_CORE_SOURCE_LIMIT)
    {
      fclose(stream);
      return -EFBIG;
    }

  buffer = js_malloc(context, (size_t)size + 1);
  if (buffer == NULL)
    {
      fclose(stream);
      return -ENOMEM;
    }

  count = fread(buffer, 1, (size_t)size, stream);
  fclose(stream);
  if (count != (size_t)size)
    {
      js_free(context, buffer);
      return -EIO;
    }

  buffer[count] = '\0';
  *source = buffer;
  *length = count;
  return 0;
}

static JSModuleDef *ny_module_load(JSContext *context, const char *module_name,
                                   void *opaque)
{
  struct ny_plugin_s *plugin = opaque;
  JSModuleDef *module;
  JSValue compiled;
  char *source = NULL;
  size_t length = 0;
  int ret;

  if (ny_module_is_capability(module_name))
    {
      return ny_capability_load_module(context, module_name);
    }

  if (!ny_module_inside_root(plugin, module_name))
    {
      JS_ThrowReferenceError(context, "module outside package");
      return NULL;
    }

  ret = ny_module_read(context, module_name, &source, &length);
  if (ret < 0)
    {
      JS_ThrowReferenceError(context, "module load failed: %s (%d)",
                             module_name, ret);
      return NULL;
    }

  compiled = JS_Eval(context, source, length, module_name,
                     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  js_free(context, source);
  if (JS_IsException(compiled))
    {
      return NULL;
    }

  module = JS_VALUE_GET_PTR(compiled);
  JS_FreeValue(context, compiled);
  return module;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ny_module_configure(struct ny_plugin_s *plugin)
{
  JS_SetModuleLoaderFunc(plugin->runtime, ny_module_normalize, ny_module_load,
                         plugin);
}

JSValue ny_module_evaluate(struct ny_plugin_s *plugin, const char *source,
                           size_t length)
{
  JSValue compiled;

  compiled = JS_Eval(plugin->context, source, length, plugin->path,
                     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled))
    {
      return compiled;
    }

  plugin->module = JS_VALUE_GET_PTR(compiled);
  return JS_EvalFunction(plugin->context, compiled);
}
