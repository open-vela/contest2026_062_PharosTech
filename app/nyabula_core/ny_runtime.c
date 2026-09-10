/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_runtime.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ny_broker.h"
#include "ny_capability.h"
#include "ny_http.h"
#include "ny_module.h"
#include "ny_runtime.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static atomic_uint_fast32_t g_ny_runtime_generation = 1;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t ny_runtime_now_ns(void);
static void ny_runtime_begin_event(struct ny_plugin_s *plugin);
static int ny_runtime_pause_execution(struct ny_plugin_s *plugin);
static int ny_runtime_deliver(struct ny_plugin_s *plugin, uint64_t token,
                              int status, const char *payload, size_t length,
                              unsigned int http_status);
static int ny_runtime_interrupt(JSRuntime *runtime, void *opaque);
static void ny_runtime_dump_exception(struct ny_plugin_s *plugin,
                                      const char *operation);
static void ny_runtime_rejection_tracker(JSContext *context,
                                         JSValueConst promise,
                                         JSValueConst reason,
                                         JS_BOOL is_handled, void *opaque);
static int ny_runtime_drain_jobs(struct ny_plugin_s *plugin);
static JSValue ny_runtime_settled(JSContext *context, JSValueConst this_value,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValue *data);
static int ny_runtime_observe_result(struct ny_plugin_s *plugin,
                                     JSValueConst value);
static int ny_runtime_call(struct ny_plugin_s *plugin, const char *name,
                           int argc, JSValueConst *argv);
static int ny_runtime_read_source(const char *path, char **source,
                                  size_t *length);

/****************************************************************************

 * * Private Functions

 * ****************************************************************************/

static JSValue ny_runtime_settled(JSContext *context, JSValueConst this_value,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValue *data)
{
  struct ny_plugin_s *plugin = JS_GetContextOpaque(context);
  uint32_t generation;

  if (JS_ToUint32(context, &generation, data[0]) == 0 &&
      generation == plugin->lifecycle_generation &&
      plugin->lifecycle_result == -EINPROGRESS)
    {
      plugin->lifecycle_result = magic ? -EFAULT : 0;
    }

  return JS_UNDEFINED;
}

static int ny_runtime_observe_result(struct ny_plugin_s *plugin,
                                     JSValueConst value)
{
  JSValue then;
  JSValue handlers[2];
  JSValue result;
  JSValue generation;
  int ret;

  if (plugin->lifecycle_generation == UINT32_MAX)
    {
      return -EOVERFLOW;
    }

  plugin->lifecycle_generation++;
  plugin->lifecycle_result = 0;
  if (!JS_IsObject(value))
    {
      return 0;
    }

  then = JS_GetPropertyStr(plugin->context, value, "then");
  if (JS_IsException(then))
    {
      ny_runtime_dump_exception(plugin, "lifecycle result");
      return -EFAULT;
    }

  if (!JS_IsFunction(plugin->context, then))
    {
      JS_FreeValue(plugin->context, then);
      return 0;
    }

  plugin->lifecycle_result = -EINPROGRESS;
  generation = JS_NewUint32(plugin->context, plugin->lifecycle_generation);
  handlers[0] = JS_NewCFunctionData(plugin->context, ny_runtime_settled, 1, 0,
                                    1, &generation);
  handlers[1] = JS_NewCFunctionData(plugin->context, ny_runtime_settled, 1, 1,
                                    1, &generation);
  JS_FreeValue(plugin->context, generation);
  if (JS_IsException(handlers[0]) || JS_IsException(handlers[1]))
    {
      ret = -ENOMEM;
    }
  else
    {
      result = JS_Call(plugin->context, then, value, 2, handlers);
      ret = JS_IsException(result) ? -EFAULT : 0;
      JS_FreeValue(plugin->context, result);
    }

  JS_FreeValue(plugin->context, handlers[0]);
  JS_FreeValue(plugin->context, handlers[1]);
  JS_FreeValue(plugin->context, then);
  return ret;
}

static uint64_t ny_runtime_now_ns(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void ny_runtime_begin_event(struct ny_plugin_s *plugin)
{
  atomic_store(&plugin->cancelled, false);
  plugin->execution_remaining_ns =
      (uint64_t)plugin->event_timeout_ms * 1000000ull;
  plugin->deadline_ns = ny_runtime_now_ns() + plugin->execution_remaining_ns;
}

static int ny_runtime_pause_execution(struct ny_plugin_s *plugin)
{
  uint64_t now = ny_runtime_now_ns();

  if (plugin->deadline_ns != 0)
    {
      plugin->execution_remaining_ns =
          plugin->deadline_ns > now ? plugin->deadline_ns - now : 0;
      plugin->deadline_ns = 0;
    }

  return plugin->execution_remaining_ns == 0 ? -ETIMEDOUT : 0;
}

static int ny_runtime_interrupt(JSRuntime *runtime, void *opaque)
{
  struct ny_plugin_s *plugin = opaque;
  uint64_t now = ny_runtime_now_ns();

  if (atomic_load(&plugin->cancelled))
    {
      return 1;
    }

  return (plugin->deadline_ns != 0 && now >= plugin->deadline_ns) ||
         (plugin->lifecycle_active && now >= plugin->lifecycle_deadline_ns);
}

static void ny_runtime_dump_exception(struct ny_plugin_s *plugin,
                                      const char *operation)
{
  JSValue exception;
  const char *message;

  exception = JS_GetException(plugin->context);
  message = JS_ToCString(plugin->context, exception);
  fprintf(stderr, "nycore: %s failed: %s\n", operation,
          message == NULL ? "unknown exception" : message);

  if (message != NULL)
    {
      JS_FreeCString(plugin->context, message);
    }

  JS_FreeValue(plugin->context, exception);
}

static void ny_runtime_rejection_tracker(JSContext *context,
                                         JSValueConst promise,
                                         JSValueConst reason,
                                         JS_BOOL is_handled, void *opaque)
{
  struct ny_plugin_s *plugin = opaque;
  const char *message;

  if (plugin->cancelling)
    {
      return;
    }

  if (is_handled)
    {
      if (plugin->unhandled_rejections > 0)
        {
          plugin->unhandled_rejections--;
        }

      return;
    }

  plugin->unhandled_rejections++;
  message = JS_ToCString(context, reason);
  fprintf(stderr, "nycore: unhandled rejection: %s\n",
          message == NULL ? "unknown reason" : message);
  if (message != NULL)
    {
      JS_FreeCString(context, message);
    }
}

static int ny_runtime_drain_jobs(struct ny_plugin_s *plugin)
{
  JSContext *context;
  int count = 0;

  while (JS_IsJobPending(plugin->runtime))
    {
      int ret;

      if (count++ >= CONFIG_NYABULA_CORE_JOB_LIMIT)
        {
          fprintf(stderr, "nycore: microtask limit exceeded\n");
          return -ELOOP;
        }

      ret = JS_ExecutePendingJob(plugin->runtime, &context);
      if (ret < 0)
        {
          ny_runtime_dump_exception(plugin, "microtask");
          return -EFAULT;
        }
    }

  return plugin->unhandled_rejections == 0 ? 0 : -EFAULT;
}

static int ny_runtime_call(struct ny_plugin_s *plugin, const char *name,
                           int argc, JSValueConst *argv)
{
  JSValue global;
  JSValue function;
  JSValue result;
  uint64_t lifecycle_deadline;
  int ret = 0;

  global = JS_GetGlobalObject(plugin->context);
  if (plugin->module_entry)
    {
      function = JS_GetModuleExport(plugin->context, plugin->module, name);
    }
  else
    {
      function = JS_GetPropertyStr(plugin->context, global, name);
    }
  if (JS_IsException(function))
    {
      ny_runtime_dump_exception(plugin, name);
      ret = -EFAULT;
      goto out;
    }

  if (JS_IsUndefined(function))
    {
      goto out;
    }

  if (!JS_IsFunction(plugin->context, function))
    {
      fprintf(stderr, "nycore: %s is not a function\n", name);
      ret = -EINVAL;
      goto out;
    }

  ny_runtime_begin_event(plugin);
  plugin->lifecycle_active = true;
  lifecycle_deadline =
      ny_runtime_now_ns() +
      (uint64_t)CONFIG_NYABULA_CORE_ASYNC_TIMEOUT_MS * 1000000ull;
  plugin->lifecycle_deadline_ns = lifecycle_deadline;
  plugin->unhandled_rejections = 0;
  result = JS_Call(plugin->context, function, global, argc, argv);
  if (JS_IsException(result))
    {
      ny_runtime_dump_exception(plugin, name);
      ret = -EFAULT;
    }
  else
    {
      ret = ny_runtime_observe_result(plugin, result);
      if (ret >= 0)
        {
          ret = ny_runtime_drain_jobs(plugin);
        }

      if (ret >= 0)
        {
          while (plugin->lifecycle_result == -EINPROGRESS &&
                 (plugin->pump != NULL
#ifdef CONFIG_NYABULA_CORE_HTTP
                  || ny_plugin_http_pending(plugin)
#endif
                      ))
            {
              ret = ny_runtime_pause_execution(plugin);
              if (ret < 0)
                {
                  break;
                }

              if (plugin->pump != NULL)
                {
                  ret = plugin->pump(plugin->pump_opaque, lifecycle_deadline);
                }
#ifdef CONFIG_NYABULA_CORE_HTTP
              else
                {
                  ret = ny_plugin_http_poll(plugin);
                  if (ret == -EAGAIN)
                    {
                      usleep(1000);
                      ret = 0;
                    }
                }
#endif
              if (ret >= 0 && (plugin->execution_remaining_ns == 0 ||
                               ny_runtime_now_ns() >= lifecycle_deadline))
                {
                  ret = -ETIMEDOUT;
                }
              if (ret < 0)
                {
                  break;
                }

              plugin->deadline_ns =
                  ny_runtime_now_ns() + plugin->execution_remaining_ns;
            }

          if (ret >= 0)
            {
              ret = plugin->lifecycle_result;
            }
        }
    }

  if ((ny_runtime_pause_execution(plugin) < 0 ||
       ny_runtime_now_ns() >= lifecycle_deadline) &&
      ret >= 0)
    {
      ret = -ETIMEDOUT;
    }
  plugin->lifecycle_active = false;

  JS_FreeValue(plugin->context, result);

out:
  JS_FreeValue(plugin->context, function);
  JS_FreeValue(plugin->context, global);
  return ret;
}

static int ny_runtime_read_source(const char *path, char **source,
                                  size_t *length)
{
  FILE *stream;
  long size;
  char *buffer;
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

  if (size == 0 || size > CONFIG_NYABULA_CORE_SOURCE_LIMIT)
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
  *source = buffer;
  *length = count;
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_plugin_load(struct ny_plugin_s *plugin, const char *path)
{
  struct ny_plugin_config_s config;

  if (path == NULL)
    {
      if (plugin != NULL)
        {
          memset(plugin, 0, sizeof(*plugin));
        }

      return -EINVAL;
    }

  ny_manifest_default_config(&config, path);
  return ny_plugin_load_config(plugin, &config);
}

int ny_plugin_load_config(struct ny_plugin_s *plugin,
                          const struct ny_plugin_config_s *config)
{
  JSValue result;
  char *source = NULL;
  size_t length = 0;
  int ret;

  if (plugin == NULL)
    {
      return -EINVAL;
    }

  /* Zero the plugin before any validation so callers may safely destroy
   * the plugin on every load failure path.
   */

  memset(plugin, 0, sizeof(*plugin));

  if (config == NULL || config->entry[0] == '\0' ||
      config->memory_limit == 0 || config->stack_limit == 0 ||
      config->event_timeout_ms == 0)
    {
      return -EINVAL;
    }

  plugin->state = NY_PLUGIN_EMPTY;
  plugin->owner = pthread_self();
  atomic_init(&plugin->cancelled, false);
  plugin->generation = atomic_fetch_add(&g_ny_runtime_generation, 1);
  if (plugin->generation == 0)
    {
      plugin->generation = atomic_fetch_add(&g_ny_runtime_generation, 1);
    }

  plugin->next_request = 1;
  plugin->requested_permissions = config->requested_permissions;
  atomic_init(&plugin->permissions, config->permissions);
  atomic_init(&plugin->permission_generation, 0);
  plugin->memory_limit = config->memory_limit;
  plugin->stack_limit = config->stack_limit;
  plugin->event_timeout_ms = config->event_timeout_ms;
  plugin->module_entry = config->module;
  strlcpy(plugin->id, config->id, sizeof(plugin->id));
  strlcpy(plugin->version, config->version, sizeof(plugin->version));
  strlcpy(plugin->root, config->root, sizeof(plugin->root));
  strlcpy(plugin->storage_root, config->storage_root,
          sizeof(plugin->storage_root));
  strlcpy(plugin->path, config->entry, sizeof(plugin->path));

  ret = ny_runtime_read_source(plugin->path, &source, &length);
  if (ret < 0)
    {
      return ret;
    }

  plugin->runtime = JS_NewRuntime();
  if (plugin->runtime == NULL)
    {
      free(source);
      return -ENOMEM;
    }

  JS_SetMemoryLimit(plugin->runtime, plugin->memory_limit);
  JS_SetMaxStackSize(plugin->runtime, plugin->stack_limit);
  JS_SetInterruptHandler(plugin->runtime, ny_runtime_interrupt, plugin);
  JS_SetHostPromiseRejectionTracker(plugin->runtime,
                                    ny_runtime_rejection_tracker, plugin);

  plugin->context = JS_NewContext(plugin->runtime);
  if (plugin->context == NULL)
    {
      free(source);
      ny_plugin_destroy(plugin);
      return -ENOMEM;
    }

  JS_SetContextOpaque(plugin->context, plugin);
  ret = ny_capability_register(plugin);
  if (ret < 0)
    {
      free(source);
      ny_plugin_destroy(plugin);
      return ret;
    }

  ny_module_configure(plugin);

  ny_runtime_begin_event(plugin);
  plugin->unhandled_rejections = 0;
  result = plugin->module_entry ? ny_module_evaluate(plugin, source, length)
                                : JS_Eval(plugin->context, source, length,
                                          plugin->path, JS_EVAL_TYPE_GLOBAL);
  free(source);
  if (JS_IsException(result))
    {
      plugin->deadline_ns = 0;
      ny_runtime_dump_exception(plugin, "load");
      JS_FreeValue(plugin->context, result);
      plugin->state = NY_PLUGIN_FAILED;
      return -EFAULT;
    }

  JS_FreeValue(plugin->context, result);
  ret = ny_runtime_drain_jobs(plugin);
  plugin->deadline_ns = 0;
  if (ret < 0)
    {
      plugin->state = NY_PLUGIN_FAILED;
      return ret;
    }

  plugin->state = NY_PLUGIN_LOADED;
  return 0;
}

int ny_plugin_start(struct ny_plugin_s *plugin)
{
  int ret;

  if (plugin == NULL || plugin->state != NY_PLUGIN_LOADED)
    {
      return -EINVAL;
    }

  ret = ny_runtime_call(plugin, "ny_on_start", 0, NULL);
  plugin->state = ret < 0 ? NY_PLUGIN_FAILED : NY_PLUGIN_RUNNING;
  return ret;
}

int ny_plugin_dispatch(struct ny_plugin_s *plugin, const char *event)
{
  JSValue argument;
  int ret;

  if (plugin == NULL || event == NULL || plugin->state != NY_PLUGIN_RUNNING)
    {
      return -EINVAL;
    }

  argument = JS_NewString(plugin->context, event);
  if (JS_IsException(argument))
    {
      ny_runtime_dump_exception(plugin, "event allocation");
      plugin->state = NY_PLUGIN_FAILED;
      return -ENOMEM;
    }

  ret = ny_runtime_call(plugin, "ny_on_event", 1, &argument);
  JS_FreeValue(plugin->context, argument);
  if (ret < 0)
    {
      plugin->state = NY_PLUGIN_FAILED;
    }

  return ret;
}

int ny_plugin_stop(struct ny_plugin_s *plugin)
{
  int ret = 0;

  if (plugin == NULL)
    {
      return -EINVAL;
    }

  if (plugin->state == NY_PLUGIN_RUNNING)
    {
      ret = ny_runtime_call(plugin, "ny_on_stop", 0, NULL);
    }

  if (ny_plugin_async_cancel_all(plugin) < 0 && ret >= 0)
    {
      ret = -EFAULT;
    }

  atomic_store(&plugin->cancelled, true);
  plugin->deadline_ns = 0;
  plugin->state = ret < 0 ? NY_PLUGIN_FAILED : NY_PLUGIN_STOPPED;
  return ret;
}

int ny_plugin_async_begin(struct ny_plugin_s *plugin, JSValue *promise,
                          uint64_t *token)
{
  return ny_plugin_async_begin_authorized(plugin, 0, promise, token);
}

int ny_plugin_async_begin_authorized(struct ny_plugin_s *plugin,
                                     uint64_t permission, JSValue *promise,
                                     uint64_t *token)
{
  JSValue resolving[2];
  uint32_t permission_generation;
  size_t index;

  if (plugin == NULL || promise == NULL || token == NULL ||
      plugin->context == NULL || plugin->state == NY_PLUGIN_STOPPED ||
      plugin->state == NY_PLUGIN_FAILED)
    {
      return -EINVAL;
    }

  if (plugin->cancelling || atomic_load(&plugin->cancelled))
    {
      return -ECANCELED;
    }

  if (!pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }

  permission_generation = atomic_load(&plugin->permission_generation);
  if ((atomic_load(&plugin->permissions) & permission) != permission)
    {
      return -EACCES;
    }

  if (plugin->next_request == 0)
    {
      return -EOVERFLOW;
    }

  for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
    {
      if (!plugin->pending[index].occupied)
        {
          break;
        }
    }

  if (index == NY_PLUGIN_MAX_PENDING)
    {
      return -EAGAIN;
    }

  *promise = JS_NewPromiseCapability(plugin->context, resolving);
  if (JS_IsException(*promise))
    {
      return -ENOMEM;
    }

  plugin->pending[index].occupied = true;
  plugin->pending[index].request = plugin->next_request++;
  plugin->pending[index].permission = permission;
  plugin->pending[index].permission_generation = permission_generation;
  plugin->pending[index].resolve = resolving[0];
  plugin->pending[index].reject = resolving[1];
  *token =
      ((uint64_t)plugin->generation << 32) | plugin->pending[index].request;
  return 0;
}

int ny_plugin_async_complete(struct ny_plugin_s *plugin, uint64_t token,
                             bool rejected, JSValueConst value)
{
  JSValue callback;
  JSValue resolve;
  JSValue reject;
  JSValue result;
  uint32_t generation = (uint32_t)(token >> 32);
  uint32_t request = (uint32_t)token;
  size_t index;

  if (plugin == NULL || plugin->context == NULL || generation == 0 ||
      request == 0 || generation != plugin->generation)
    {
      return -ESTALE;
    }

  if (!pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }

  for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
    {
      if (plugin->pending[index].occupied &&
          plugin->pending[index].request == request)
        {
          break;
        }
    }

  if (index == NY_PLUGIN_MAX_PENDING)
    {
      return -ENOENT;
    }

  /* Release the pending slot before running plugin code: the callback may
   * re-enter the host and complete or cancel the same token, which would
   * otherwise double-free resolve/reject.
   */

  resolve = plugin->pending[index].resolve;
  reject = plugin->pending[index].reject;
#ifdef CONFIG_NYABULA_CORE_HTTP
  if (plugin->pending[index].http != NULL)
    {
      int ret = ny_broker_http_close(plugin->pending[index].http);
      if (ret < 0)
        {
          return ret;
        }
    }
#endif
  memset(&plugin->pending[index], 0, sizeof(plugin->pending[index]));

  callback = rejected ? reject : resolve;
  result = JS_Call(plugin->context, callback, JS_UNDEFINED, 1, &value);
  JS_FreeValue(plugin->context, resolve);
  JS_FreeValue(plugin->context, reject);
  if (JS_IsException(result))
    {
      ny_runtime_dump_exception(plugin, "async completion");
      JS_FreeValue(plugin->context, result);
      return -EFAULT;
    }

  JS_FreeValue(plugin->context, result);
  return 0;
}

int ny_plugin_async_deliver(struct ny_plugin_s *plugin, uint64_t token,
                            int status, const char *payload, size_t length)
{
  return ny_runtime_deliver(plugin, token, status, payload, length, 0);
}

static int ny_runtime_deliver(struct ny_plugin_s *plugin, uint64_t token,
                              int status, const char *payload, size_t length,
                              unsigned int http_status)
{
  uint64_t previous_deadline;
  JSValue value;
  size_t index;
  int ret;

  if (plugin == NULL || plugin->context == NULL ||
      (uint32_t)(token >> 32) != plugin->generation)
    {
      return -ESTALE;
    }

  if (!pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }

  if ((payload == NULL && length != 0) ||
      length > (http_status != 0 ? NY_HTTP_BODY_LIMIT
                                 : CONFIG_NYABULA_CORE_EVENT_SIZE) ||
      status > 0)
    {
      return -EINVAL;
    }

  for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
    {
      if (plugin->pending[index].occupied &&
          plugin->pending[index].request == (uint32_t)token)
        {
          break;
        }
    }

  if (index == NY_PLUGIN_MAX_PENDING)
    {
      return -ENOENT;
    }

  if ((plugin->pending[index].permission != 0 &&
       atomic_load(&plugin->permission_generation) !=
           plugin->pending[index].permission_generation) ||
      (atomic_load(&plugin->permissions) &
       plugin->pending[index].permission) != plugin->pending[index].permission)
    {
      status = -EACCES;
    }

  previous_deadline = plugin->deadline_ns;
  if (previous_deadline == 0)
    {
      if (plugin->lifecycle_active)
        {
          if (plugin->execution_remaining_ns == 0)
            {
              return -ETIMEDOUT;
            }
          plugin->deadline_ns =
              ny_runtime_now_ns() + plugin->execution_remaining_ns;
        }
      else
        {
          ny_runtime_begin_event(plugin);
        }
    }

  /* Only the owning worker creates JS values. Provider errors carry errno. */

  value = status < 0 ? JS_NewInt32(plugin->context, status)
                     : JS_NewStringLen(plugin->context,
                                       payload == NULL ? "" : payload, length);
  if (status == 0 && http_status != 0 && !JS_IsException(value))
    {
      JSValue response = JS_NewObject(plugin->context);
      if (JS_IsException(response))
        {
          JS_FreeValue(plugin->context, value);
          value = response;
        }
      else if (JS_SetPropertyStr(plugin->context, response, "body", value) <
                   0 ||
               JS_SetPropertyStr(plugin->context, response, "status",
                                 JS_NewUint32(plugin->context, http_status)) <
                   0)
        {
          JS_FreeValue(plugin->context, response);
          value = JS_EXCEPTION;
        }
      else
        {
          value = response;
        }
    }
  if (JS_IsException(value))
    {
      ny_runtime_dump_exception(plugin, "async payload");
      ret = -ENOMEM;
    }
  else
    {
      ret = ny_plugin_async_complete(plugin, token, status < 0, value);
      if (ret >= 0)
        {
          ret = ny_runtime_drain_jobs(plugin);
        }
    }

  JS_FreeValue(plugin->context, value);
  if (previous_deadline == 0 && ny_runtime_pause_execution(plugin) < 0 &&
      ret >= 0)
    {
      ret = -ETIMEDOUT;
    }
  plugin->deadline_ns = previous_deadline;
  return ret;
}

#ifdef CONFIG_NYABULA_CORE_HTTP
bool ny_plugin_http_pending(struct ny_plugin_s *plugin)
{
  size_t index;
  if (plugin == NULL)
    {
      return false;
    }
  for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
    {
      if (plugin->pending[index].occupied &&
          (plugin->pending[index].http != NULL ||
           plugin->pending[index].http_error < 0))
        {
          return true;
        }
    }
  return false;
}

int ny_plugin_http_request(struct ny_plugin_s *plugin, const char *url,
                           JSValue *promise)
{
  struct ny_broker_client_s client;
  uint64_t token;
  size_t index;
  int ret;

  ret = ny_plugin_async_begin_authorized(plugin, NY_PERMISSION_NETWORK_REQUEST,
                                         promise, &token);
  if (ret < 0)
    {
      return ret;
    }
  for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
    {
      if (plugin->pending[index].occupied &&
          plugin->pending[index].request == (uint32_t)token)
        {
          break;
        }
    }
  if (index == NY_PLUGIN_MAX_PENDING)
    {
      return -EFAULT;
    }
  client.id = plugin->id;
  client.storage_root = plugin->storage_root;
  client.permissions = atomic_load(&plugin->permissions);
  ret = ny_broker_http_open(
      &client, plugin->pending[index].permission_generation, url,
      CONFIG_NYABULA_CORE_ASYNC_TIMEOUT_MS, &plugin->pending[index].http);
  if (ret < 0)
    {
      plugin->pending[index].http_error = ret;
    }
  return 0;
}

int ny_plugin_http_poll(struct ny_plugin_s *plugin)
{
  struct ny_broker_client_s client;
  const void *body;
  size_t length;
  size_t index;
  unsigned int http_status;
  int ret;
  if (plugin == NULL || plugin->context == NULL)
    {
      return -EINVAL;
    }
  if (!pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }
  if (atomic_load(&plugin->cancelled))
    {
      return -ECANCELED;
    }
  for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
    {
      uint64_t token;
      if (!plugin->pending[index].occupied ||
          (plugin->pending[index].http == NULL &&
           plugin->pending[index].http_error == 0))
        {
          continue;
        }
      client.id = plugin->id;
      client.storage_root = plugin->storage_root;
      client.permissions = atomic_load(&plugin->permissions);
      body = NULL;
      length = 0;
      http_status = 0;
      ret = plugin->pending[index].http_error;
      if (ret == 0)
        {
          ret =
              ny_broker_http_step(plugin->pending[index].http, &client,
                                  atomic_load(&plugin->permission_generation),
                                  &http_status, &body, &length);
        }
      if (ret == -EAGAIN)
        {
          continue;
        }
      if (ret == 0 && http_status == 0)
        {
          ret = -EPROTO;
        }
      token = ((uint64_t)plugin->generation << 32) |
              plugin->pending[index].request;
      return ny_runtime_deliver(plugin, token, ret, body, length, http_status);
    }
  return -EAGAIN;
}
#endif

int ny_plugin_async_cancel_all(struct ny_plugin_s *plugin)
{
  JSValue error;
  uint64_t previous_deadline;
  bool cancelled = false;
  int first_error = 0;
  size_t index;

  if (plugin == NULL || plugin->context == NULL)
    {
      return 0;
    }

  if (plugin->cancelling)
    {
      return 0;
    }

  plugin->cancelling = true;
  previous_deadline = plugin->deadline_ns;
  if (previous_deadline == 0)
    {
      plugin->deadline_ns = ny_runtime_now_ns() +
                            (uint64_t)plugin->event_timeout_ms * 1000000ull;
    }

  error = JS_NewError(plugin->context);
  if (JS_IsException(error))
    {
      /* Clearing host references must not depend on allocating an Error. */

      error = JS_GetException(plugin->context);
    }
  else if (JS_SetPropertyStr(plugin->context, error, "message",
                             JS_NewString(plugin->context, "plugin stopped")) <
           0)
    {
      JSValue exception = JS_GetException(plugin->context);
      JS_FreeValue(plugin->context, exception);
    }
  for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
    {
      uint64_t token;
      int ret;

      if (!plugin->pending[index].occupied)
        {
          continue;
        }

      token = ((uint64_t)plugin->generation << 32) |
              plugin->pending[index].request;
      cancelled = true;
      ret = ny_plugin_async_complete(plugin, token, true, error);
      if (ret < 0 && first_error == 0)
        {
          first_error = ret;
        }
    }

  JS_FreeValue(plugin->context, error);
  if (cancelled && first_error == 0)
    {
      first_error = ny_runtime_drain_jobs(plugin);
    }

  plugin->cancelling = false;
  plugin->deadline_ns = previous_deadline;
  return first_error;
}

void ny_plugin_destroy(struct ny_plugin_s *plugin)
{
  if (plugin == NULL)
    {
      return;
    }

  if (plugin->context != NULL)
    {
      size_t index;

      /* Destruction runs no plugin code, including rejection handlers. */

      atomic_store(&plugin->cancelled, true);
      for (index = 0; index < NY_PLUGIN_MAX_PENDING; index++)
        {
          if (plugin->pending[index].occupied)
            {
#ifdef CONFIG_NYABULA_CORE_HTTP
              ny_broker_http_close(plugin->pending[index].http);
#endif
              JS_FreeValue(plugin->context, plugin->pending[index].resolve);
              JS_FreeValue(plugin->context, plugin->pending[index].reject);
              memset(&plugin->pending[index], 0,
                     sizeof(plugin->pending[index]));
            }
        }

      JS_FreeContext(plugin->context);
    }

  if (plugin->runtime != NULL)
    {
      JS_FreeRuntime(plugin->runtime);
    }

  memset(plugin, 0, sizeof(*plugin));
  plugin->state = NY_PLUGIN_EMPTY;
}
