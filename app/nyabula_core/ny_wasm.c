/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_wasm.c
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

#include <nuttx/clock.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ny_manifest.h"
#include "ny_wasm.h"

#ifdef CONFIG_NYABULA_CORE_WAMR

#include <wasm_export.h>

#include "ny_broker.h"
#include "ny_http.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_WASM_ERROR_SIZE           192
#define NY_WASM_LOG_LIMIT            1024
#define NY_WASM_BROKER_PAYLOAD_LIMIT 4096

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_wasm_lock = NXMUTEX_INITIALIZER;
static bool g_wasm_initialized;
static atomic_uint g_wasm_generation = 1;

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_wasm_watchdog_s
{
  wasm_module_inst_t instance;
  struct wdog_s timer;
  struct work_s work;
  atomic_bool timed_out;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int32_t ny_wasm_core_log(wasm_exec_env_t environment, int32_t offset,
                                int32_t length);
static int32_t ny_wasm_storage_get(wasm_exec_env_t environment,
                                   int32_t key_offset, int32_t key_length,
                                   int32_t output_offset,
                                   int32_t output_capacity);
static int32_t ny_wasm_storage_put(wasm_exec_env_t environment,
                                   int32_t key_offset, int32_t key_length,
                                   int32_t value_offset, int32_t value_length);
static int32_t ny_wasm_network_request(wasm_exec_env_t environment,
                                       int32_t input_offset,
                                       int32_t input_length,
                                       int32_t output_offset,
                                       int32_t output_capacity);
static int32_t ny_wasm_ui_notify(wasm_exec_env_t environment, int32_t offset,
                                 int32_t length);
static int32_t ny_wasm_ui_eye(wasm_exec_env_t environment, int32_t offset,
                              int32_t length);
static int32_t ny_wasm_ai_invoke(wasm_exec_env_t environment,
                                 int32_t input_offset, int32_t input_length,
                                 int32_t output_offset,
                                 int32_t output_capacity);
static int ny_wasm_memory(wasm_exec_env_t environment, int32_t offset,
                          int32_t length, int32_t limit, void **pointer);
static void ny_wasm_client(struct ny_wasm_plugin_s *plugin,
                           struct ny_broker_client_s *client);
static int ny_wasm_initialize(void);
static int ny_wasm_read(const char *path, uint8_t **binary, uint32_t *size);
static uint64_t ny_wasm_now(void);
static int64_t ny_wasm_defer(wasm_exec_env_t environment);
static int32_t ny_wasm_complete(wasm_exec_env_t environment, int64_t token,
                                int32_t result);
static int ny_wasm_lifecycle(struct ny_wasm_plugin_s *plugin,
                             wasm_function_inst_t function, uint32_t argc,
                             uint32_t *argv);
static void ny_wasm_cancel_requests(struct ny_wasm_plugin_s *plugin);
#ifdef CONFIG_NYABULA_CORE_HTTP
static int64_t ny_wasm_http_request(wasm_exec_env_t environment,
                                    int32_t offset, int32_t length);
static int32_t ny_wasm_http_cancel(wasm_exec_env_t environment, int64_t token);
#endif
static void ny_wasm_watchdog(wdparm_t argument);
static void ny_wasm_terminate_work(void *argument);
static int ny_wasm_execute(struct ny_wasm_plugin_s *plugin,
                           wasm_function_inst_t function, uint32_t argc,
                           uint32_t *argv);
static int ny_wasm_call(struct ny_wasm_plugin_s *plugin, const char *name,
                        bool required);
static int ny_wasm_call_event(struct ny_wasm_plugin_s *plugin,
                              const char *event);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static NativeSymbol g_wasm_symbols[] = {
  { "lifecycle_defer", (void *)ny_wasm_defer, "()I", NULL },
  { "lifecycle_complete", (void *)ny_wasm_complete, "(Ii)i", NULL },
#ifdef CONFIG_NYABULA_CORE_HTTP
  { "http_request", (void *)ny_wasm_http_request, "(ii)I", NULL },
  { "http_cancel", (void *)ny_wasm_http_cancel, "(I)i", NULL },
#endif
  { "core_log", (void *)ny_wasm_core_log, "(ii)i", NULL },
  { "storage_get", (void *)ny_wasm_storage_get, "(iiii)i", NULL },
  { "storage_put", (void *)ny_wasm_storage_put, "(iiii)i", NULL },
  { "network_request", (void *)ny_wasm_network_request, "(iiii)i", NULL },
  { "ui_notify", (void *)ny_wasm_ui_notify, "(ii)i", NULL },
  { "ui_eye", (void *)ny_wasm_ui_eye, "(ii)i", NULL },
  { "ai_invoke", (void *)ny_wasm_ai_invoke, "(iiii)i", NULL },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int32_t ny_wasm_core_log(wasm_exec_env_t environment, int32_t offset,
                                int32_t length)
{
  struct ny_wasm_plugin_s *plugin;
  wasm_module_inst_t instance;
  const char *message;

  plugin = wasm_runtime_get_user_data(environment);
  instance = wasm_runtime_get_module_inst(environment);
  if (plugin == NULL ||
      (atomic_load(&plugin->permissions) & NY_PERMISSION_CORE_LOG) == 0)
    {
      return -EACCES;
    }

  if (offset < 0 || length < 0 || length > NY_WASM_LOG_LIMIT ||
      !wasm_runtime_validate_app_addr(instance, (uint32_t)offset,
                                      (uint32_t)length))
    {
      return -EINVAL;
    }

  message = wasm_runtime_addr_app_to_native(instance, (uint32_t)offset);
  printf("nyplugin[%s]: %.*s\n", plugin->id, length, message);
  return 0;
}

static int ny_wasm_memory(wasm_exec_env_t environment, int32_t offset,
                          int32_t length, int32_t limit, void **pointer)
{
  wasm_module_inst_t instance = wasm_runtime_get_module_inst(environment);

  if (offset < 0 || length < 0 || length > limit ||
      !wasm_runtime_validate_app_addr(instance, (uint32_t)offset,
                                      (uint32_t)length))
    {
      return -EINVAL;
    }

  *pointer = wasm_runtime_addr_app_to_native(instance, (uint32_t)offset);
  return *pointer == NULL && length != 0 ? -EFAULT : 0;
}

static void ny_wasm_client(struct ny_wasm_plugin_s *plugin,
                           struct ny_broker_client_s *client)
{
  client->id = plugin->id;
  client->storage_root = plugin->storage_root;
  client->permissions = atomic_load(&plugin->permissions);
}

static int32_t ny_wasm_storage_get(wasm_exec_env_t environment,
                                   int32_t key_offset, int32_t key_length,
                                   int32_t output_offset,
                                   int32_t output_capacity)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  struct ny_broker_client_s client;
  char key[64];
  char *value = NULL;
  const void *key_memory;
  void *output;
  size_t length;
  int ret;

  if (plugin == NULL || key_length <= 0 || key_length >= sizeof(key))
    {
      return -EINVAL;
    }

  ret = ny_wasm_memory(environment, key_offset, key_length, sizeof(key) - 1,
                       (void **)&key_memory);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(key, key_memory, (size_t)key_length);
  key[key_length] = '\0';
  ny_wasm_client(plugin, &client);
  ret = ny_broker_storage_get(&client, key, &value, &length);
  if (ret < 0)
    {
      return ret;
    }

  if (output_capacity < 0 || length > (size_t)output_capacity)
    {
      free(value);
      return -ENOSPC;
    }

  ret = ny_wasm_memory(environment, output_offset, output_capacity,
                       CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT, &output);
  if (ret >= 0)
    {
      memcpy(output, value, length);
      ret = (int)length;
    }

  free(value);
  return ret;
}

static int32_t ny_wasm_storage_put(wasm_exec_env_t environment,
                                   int32_t key_offset, int32_t key_length,
                                   int32_t value_offset, int32_t value_length)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  struct ny_broker_client_s client;
  char key[64];
  const void *key_memory;
  const void *value;
  int ret;

  if (plugin == NULL || key_length <= 0 || key_length >= sizeof(key))
    {
      return -EINVAL;
    }

  ret = ny_wasm_memory(environment, key_offset, key_length, sizeof(key) - 1,
                       (void **)&key_memory);
  if (ret < 0 || (ret = ny_wasm_memory(environment, value_offset, value_length,
                                       CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT,
                                       (void **)&value)) < 0)
    {
      return ret;
    }

  memcpy(key, key_memory, (size_t)key_length);
  key[key_length] = '\0';
  ny_wasm_client(plugin, &client);
  ret = ny_broker_storage_put(&client, key, value, (size_t)value_length);
  if (ret < 0)
    {
      fprintf(stderr, "nycore: wasm storage_put rejected: %d\n", ret);
    }

  return ret;
}

static int32_t ny_wasm_network_request(wasm_exec_env_t environment,
                                       int32_t input_offset,
                                       int32_t input_length,
                                       int32_t output_offset,
                                       int32_t output_capacity)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  struct ny_broker_client_s client;
  const void *input;
  void *output;
  int ret;

  if (plugin == NULL || output_capacity < input_length)
    {
      return plugin == NULL ? -EINVAL : -ENOSPC;
    }

  ret = ny_wasm_memory(environment, input_offset, input_length,
                       NY_WASM_BROKER_PAYLOAD_LIMIT, (void **)&input);
  if (ret < 0 ||
      (ret = ny_wasm_memory(environment, output_offset, output_capacity,
                            NY_WASM_BROKER_PAYLOAD_LIMIT, &output)) < 0)
    {
      return ret;
    }

  ny_wasm_client(plugin, &client);
#ifdef CONFIG_NYABULA_CORE_HTTP
  ret = -ENOTSUP;
#else
  ret = ny_broker_network_request(&client);
#endif
  if (ret >= 0)
    {
      memcpy(output, input, (size_t)input_length);
      ret = input_length;
    }

  return ret;
}

static int32_t ny_wasm_ui_notify(wasm_exec_env_t environment, int32_t offset,
                                 int32_t length)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  struct ny_broker_client_s client;
  const void *message;
  int ret;

  if (plugin == NULL)
    {
      return -EINVAL;
    }

  ret = ny_wasm_memory(environment, offset, length,
                       NY_WASM_BROKER_PAYLOAD_LIMIT, (void **)&message);
  if (ret >= 0)
    {
      ny_wasm_client(plugin, &client);
      ret = ny_broker_ui_notify(&client, message, (size_t)length);
    }

  return ret;
}

static int32_t ny_wasm_ui_eye(wasm_exec_env_t environment, int32_t offset,
                              int32_t length)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  struct ny_broker_client_s client;
  void *command;
  int ret;
  if (plugin == NULL)
    {
      return -EINVAL;
    }
  ret = ny_wasm_memory(environment, offset, length,
                       NY_WASM_BROKER_PAYLOAD_LIMIT, &command);
  if (ret == 0)
    {
      ny_wasm_client(plugin, &client);
      ret = ny_broker_ui_eye(&client, command, (size_t)length);
    }
  return ret;
}

static int32_t ny_wasm_ai_invoke(wasm_exec_env_t environment,
                                 int32_t input_offset, int32_t input_length,
                                 int32_t output_offset,
                                 int32_t output_capacity)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  struct ny_broker_client_s client;
  const void *input;
  void *output;
  int ret;

  if (plugin == NULL || output_capacity < input_length)
    {
      return plugin == NULL ? -EINVAL : -ENOSPC;
    }

  ret = ny_wasm_memory(environment, input_offset, input_length,
                       NY_WASM_BROKER_PAYLOAD_LIMIT, (void **)&input);
  if (ret < 0 ||
      (ret = ny_wasm_memory(environment, output_offset, output_capacity,
                            NY_WASM_BROKER_PAYLOAD_LIMIT, &output)) < 0)
    {
      return ret;
    }

  ny_wasm_client(plugin, &client);
  ret = ny_broker_ai_invoke(&client, input, (size_t)input_length, output,
                            (size_t)output_capacity);

  return ret;
}

static int ny_wasm_call_event(struct ny_wasm_plugin_s *plugin,
                              const char *event)
{
  wasm_module_inst_t instance = plugin->instance;
  wasm_function_inst_t function;
  wasm_valkind_t types[2];
  const char *exception;
  void *native;
  uint64_t offset;
  uint32_t arguments[2];
  size_t length = strlen(event);
  int ret;

  if (length >= CONFIG_NYABULA_CORE_EVENT_SIZE)
    {
      return -E2BIG;
    }

  function = wasm_runtime_lookup_function(instance, "ny_on_event");
  if (function == NULL)
    {
      return 0;
    }

  if (wasm_func_get_param_count(function, instance) != 2 ||
      wasm_func_get_result_count(function, instance) != 0)
    {
      return -EPROTO;
    }

  wasm_func_get_param_types(function, instance, types);
  if (types[0] != WASM_I32 || types[1] != WASM_I32)
    {
      return -EPROTO;
    }

  offset =
      wasm_runtime_module_malloc(instance, length == 0 ? 1 : length, &native);
  if (offset == 0 || offset > UINT32_MAX || native == NULL)
    {
      return -ENOMEM;
    }

  memcpy(native, event, length);
  arguments[0] = (uint32_t)offset;
  arguments[1] = (uint32_t)length;
  ret = ny_wasm_lifecycle(plugin, function, 2, arguments);
  if (ret < 0)
    {
      exception = wasm_runtime_get_exception(instance);
      fprintf(stderr, "nycore: wasm ny_on_event failed: %s\n",
              exception == NULL ? "unknown exception" : exception);
      wasm_runtime_module_free(instance, offset);
      return ret;
    }

  wasm_runtime_module_free(instance, offset);
  return 0;
}

static int ny_wasm_initialize(void)
{
  int ret = 0;

  nxmutex_lock(&g_wasm_lock);
  if (!g_wasm_initialized)
    {
      if (!wasm_runtime_init() ||
          !wasm_runtime_register_natives("nyabula", g_wasm_symbols,
                                         sizeof(g_wasm_symbols) /
                                             sizeof(g_wasm_symbols[0])))
        {
          ret = -EIO;
        }
      else
        {
          g_wasm_initialized = true;
        }
    }

  nxmutex_unlock(&g_wasm_lock);
  return ret;
}

static int ny_wasm_read(const char *path, uint8_t **binary, uint32_t *size)
{
  struct stat status;
  size_t offset = 0;
  int fd;

  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || status.st_size > CONFIG_NYABULA_CORE_SOURCE_LIMIT)
    {
      close(fd);
      return -EFBIG;
    }

  *binary = malloc((size_t)status.st_size);
  if (*binary == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  while (offset < (size_t)status.st_size)
    {
      ssize_t count =
          read(fd, *binary + offset, (size_t)status.st_size - offset);

      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          free(*binary);
          *binary = NULL;
          close(fd);
          return count < 0 ? -errno : -EIO;
        }

      offset += (size_t)count;
    }

  close(fd);
  *size = (uint32_t)status.st_size;
  return 0;
}

static void ny_wasm_watchdog(wdparm_t argument)
{
  struct ny_wasm_watchdog_s *watchdog =
      (struct ny_wasm_watchdog_s *)(uintptr_t)argument;

  work_queue(HPWORK, &watchdog->work, ny_wasm_terminate_work, watchdog, 0);
}

static void ny_wasm_terminate_work(void *argument)
{
  struct ny_wasm_watchdog_s *watchdog = argument;

  atomic_store(&watchdog->timed_out, true);
  wasm_runtime_terminate(watchdog->instance);
}

static int ny_wasm_execute(struct ny_wasm_plugin_s *plugin,
                           wasm_function_inst_t function, uint32_t argc,
                           uint32_t *argv)
{
  struct ny_wasm_watchdog_s watchdog;
  bool called;
  uint64_t before = ny_wasm_now();
  uint64_t budget = (uint64_t)plugin->event_timeout_ms * 1000000ull;
  uint64_t elapsed;
  int ret;

  memset(&watchdog, 0, sizeof(watchdog));
  if (!pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }
  watchdog.instance = plugin->instance;
  atomic_init(&watchdog.timed_out, false);
  if (plugin->lifecycle_active)
    {
      if (before >= plugin->lifecycle_deadline_ns ||
          plugin->execution_remaining_ns == 0)
        {
          return -ETIMEDOUT;
        }
      budget = plugin->execution_remaining_ns;
      if (budget > plugin->lifecycle_deadline_ns - before)
        {
          budget = plugin->lifecycle_deadline_ns - before;
        }
    }
  ret = wd_start(&watchdog.timer, MSEC2TICK((budget + 999999) / 1000000),
                 ny_wasm_watchdog, (wdparm_t)(uintptr_t)&watchdog);
  if (ret < 0)
    {
      /* wd_start() already returns a negated errno value. */

      return ret;
    }

  wasm_runtime_clear_exception(plugin->instance);
  called = wasm_runtime_call_wasm(plugin->environment, function, argc, argv);
  wd_cancel(&watchdog.timer);
  work_cancel_sync(HPWORK, &watchdog.work);
  ret = atomic_load(&watchdog.timed_out) ? -ETIMEDOUT : called ? 0 : -EFAULT;
  elapsed = ny_wasm_now() - before;
  if (plugin->lifecycle_active)
    {
      plugin->execution_remaining_ns =
          elapsed < plugin->execution_remaining_ns
              ? plugin->execution_remaining_ns - elapsed
              : 0;
      if ((plugin->execution_remaining_ns == 0 ||
           ny_wasm_now() >= plugin->lifecycle_deadline_ns) &&
          ret == 0)
        {
          ret = -ETIMEDOUT;
        }
    }
  return ret;
}

static int ny_wasm_call(struct ny_wasm_plugin_s *plugin, const char *name,
                        bool required)
{
  wasm_function_inst_t function;
  const char *exception;
  int ret;

  function = wasm_runtime_lookup_function(plugin->instance, name);
  if (function == NULL)
    {
      return required ? -ENOEXEC : 0;
    }

  if (wasm_func_get_param_count(function, plugin->instance) != 0 ||
      wasm_func_get_result_count(function, plugin->instance) != 0)
    {
      return -EPROTO;
    }

  ret = ny_wasm_lifecycle(plugin, function, 0, NULL);
  if (ret < 0)
    {
      exception = wasm_runtime_get_exception(plugin->instance);
      fprintf(stderr, "nycore: wasm %s failed: %s\n", name,
              exception == NULL ? "unknown exception" : exception);
      return ret;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static uint64_t ny_wasm_now(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

static int64_t ny_wasm_defer(wasm_exec_env_t environment)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  if (plugin == NULL || !plugin->lifecycle_active || plugin->stopping)
    {
      return -ECANCELED;
    }
  if (plugin->lifecycle_deferred)
    {
      return -EALREADY;
    }
  plugin->lifecycle_deferred = true;
  plugin->lifecycle_waiting = true;
  plugin->lifecycle_result = -EINPROGRESS;
  return plugin->lifecycle_token;
}

static int32_t ny_wasm_complete(wasm_exec_env_t environment, int64_t token,
                                int32_t result)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  if (plugin == NULL || !plugin->lifecycle_active ||
      !plugin->lifecycle_waiting || token <= 0 ||
      (uint64_t)token != plugin->lifecycle_token)
    {
      return -ESTALE;
    }
  if (result > 0)
    {
      return -EINVAL;
    }
  plugin->lifecycle_result = result;
  plugin->lifecycle_waiting = false;
  return 0;
}

static int ny_wasm_lifecycle(struct ny_wasm_plugin_s *plugin,
                             wasm_function_inst_t function, uint32_t argc,
                             uint32_t *argv)
{
  int ret;
  if (!pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }
  if (plugin->lifecycle_active || plugin->next_lifecycle == 0)
    {
      return plugin->lifecycle_active ? -EBUSY : -EOVERFLOW;
    }
  plugin->lifecycle_token =
      ((uint64_t)plugin->generation << 32) | plugin->next_lifecycle;
  plugin->next_lifecycle =
      plugin->next_lifecycle > UINT32_MAX - 2 ? 0 : plugin->next_lifecycle + 2;
  plugin->lifecycle_active = true;
  plugin->lifecycle_deferred = false;
  plugin->lifecycle_waiting = false;
  plugin->lifecycle_result = 0;
  plugin->execution_remaining_ns =
      (uint64_t)plugin->event_timeout_ms * 1000000ull;
  plugin->lifecycle_deadline_ns =
      ny_wasm_now() +
      (uint64_t)CONFIG_NYABULA_CORE_ASYNC_TIMEOUT_MS * 1000000ull;
  ret = ny_wasm_execute(plugin, function, argc, argv);
  while (ret >= 0 && plugin->lifecycle_waiting)
    {
      if (ny_wasm_now() >= plugin->lifecycle_deadline_ns)
        {
          ret = -ETIMEDOUT;
          break;
        }
      if (plugin->pump != NULL)
        {
          ret =
              plugin->pump(plugin->pump_opaque, plugin->lifecycle_deadline_ns);
        }
      else
        {
          ret = ny_wasm_poll(plugin);
          if (ret == -EAGAIN)
            {
              usleep(1000);
              ret = 0;
            }
        }
    }
  if (ret >= 0)
    {
      ret = plugin->lifecycle_result;
    }
  plugin->lifecycle_active = false;
  return ret;
}

bool ny_wasm_pending(struct ny_wasm_plugin_s *plugin)
{
  size_t index;
  if (plugin == NULL)
    {
      return false;
    }
  for (index = 0; index < NY_WASM_MAX_PENDING; index++)
    {
      if (plugin->pending[index].http != NULL)
        {
          return true;
        }
    }
  return false;
}

static void ny_wasm_cancel_requests(struct ny_wasm_plugin_s *plugin)
{
#ifdef CONFIG_NYABULA_CORE_HTTP
  size_t index;
  for (index = 0; index < NY_WASM_MAX_PENDING; index++)
    {
      ny_broker_http_close(plugin->pending[index].http);
      memset(&plugin->pending[index], 0, sizeof(plugin->pending[index]));
    }
#endif
}

#ifdef CONFIG_NYABULA_CORE_HTTP
static int64_t ny_wasm_http_request(wasm_exec_env_t environment,
                                    int32_t offset, int32_t length)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  struct ny_broker_client_s client;
  wasm_function_inst_t response;
  wasm_valkind_t types[5];
  const void *input;
  char url[256];
  size_t index;
  int ret;
  if (plugin == NULL || plugin->stopping)
    {
      return -ECANCELED;
    }
  if (length <= 0 || length >= sizeof(url))
    {
      return -EINVAL;
    }
  ret = ny_wasm_memory(environment, offset, length, sizeof(url) - 1,
                       (void **)&input);
  if (ret < 0)
    {
      return ret;
    }
  if (memchr(input, '\0', length) != NULL)
    {
      return -EINVAL;
    }
  response = wasm_runtime_lookup_function(plugin->instance, "ny_on_response");
  if (response == NULL ||
      wasm_func_get_param_count(response, plugin->instance) != 5 ||
      wasm_func_get_result_count(response, plugin->instance) != 0)
    {
      return -EPROTO;
    }
  wasm_func_get_param_types(response, plugin->instance, types);
  for (index = 0; index < 5; index++)
    {
      if (types[index] != (index == 0 ? WASM_I64 : WASM_I32))
        {
          return -EPROTO;
        }
    }
  for (index = 0; index < NY_WASM_MAX_PENDING; index++)
    {
      if (plugin->pending[index].http == NULL)
        {
          break;
        }
    }
  if (index == NY_WASM_MAX_PENDING || plugin->next_request == 0)
    {
      return index == NY_WASM_MAX_PENDING ? -EAGAIN : -EOVERFLOW;
    }
  memcpy(url, input, length);
  url[length] = '\0';
  ny_wasm_client(plugin, &client);
  plugin->pending[index].permission_generation =
      atomic_load(&plugin->permission_generation);
  ret = ny_broker_http_open(
      &client, plugin->pending[index].permission_generation, url,
      CONFIG_NYABULA_CORE_ASYNC_TIMEOUT_MS, &plugin->pending[index].http);
  if (ret < 0)
    {
      return ret;
    }
  plugin->pending[index].request = plugin->next_request;
  plugin->next_request =
      plugin->next_request > UINT32_MAX - 2 ? 0 : plugin->next_request + 2;
  return ((uint64_t)plugin->generation << 32) | plugin->pending[index].request;
}

static int32_t ny_wasm_http_cancel(wasm_exec_env_t environment, int64_t token)
{
  struct ny_wasm_plugin_s *plugin = wasm_runtime_get_user_data(environment);
  size_t index;
  int ret;
  if (plugin == NULL || token <= 0 ||
      (uint32_t)((uint64_t)token >> 32) != plugin->generation)
    {
      return -ESTALE;
    }
  for (index = 0; index < NY_WASM_MAX_PENDING; index++)
    {
      if (plugin->pending[index].http != NULL &&
          plugin->pending[index].request == (uint32_t)token)
        {
          ret = ny_broker_http_close(plugin->pending[index].http);
          if (ret == 0)
            {
              memset(&plugin->pending[index], 0,
                     sizeof(plugin->pending[index]));
            }
          return ret;
        }
    }
  return -ESTALE;
}
#endif

int ny_wasm_poll(struct ny_wasm_plugin_s *plugin)
{
#ifdef CONFIG_NYABULA_CORE_HTTP
  struct ny_broker_client_s client;
  const void *body;
  void *native = NULL;
  size_t length;
  size_t index;
  unsigned int status;
  uint64_t offset;
  uint32_t args[6];
  int ret;
  if (plugin == NULL || !pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }
  if (plugin->stopping)
    {
      return -ECANCELED;
    }
  for (index = 0; index < NY_WASM_MAX_PENDING; index++)
    {
      if (plugin->pending[index].http == NULL)
        {
          continue;
        }
      ny_wasm_client(plugin, &client);
      ret = ny_broker_http_step(plugin->pending[index].http, &client,
                                atomic_load(&plugin->permission_generation),
                                &status, &body, &length);
      if (ret == -EAGAIN)
        {
          continue;
        }
      if (atomic_load(&plugin->permission_generation) !=
          plugin->pending[index].permission_generation)
        {
          ret = -EACCES;
        }
      offset = 0;
      if (ret == 0 && length != 0)
        {
          offset =
              wasm_runtime_module_malloc(plugin->instance, length, &native);
          if (offset == 0 || offset > UINT32_MAX || native == NULL)
            {
              if (offset != 0)
                {
                  wasm_runtime_module_free(plugin->instance, offset);
                }
              offset = 0;
              ret = -ENOMEM;
            }
          else
            {
              memcpy(native, body, length);
            }
        }
      args[0] = plugin->pending[index].request;
      args[1] = plugin->generation;
      args[2] = ret;
      args[3] = ret < 0 ? 0 : status;
      args[4] = offset;
      args[5] = ret < 0 ? 0 : length;
      ny_broker_http_close(plugin->pending[index].http);
      memset(&plugin->pending[index], 0, sizeof(plugin->pending[index]));
      ret = ny_wasm_execute(
          plugin,
          wasm_runtime_lookup_function(plugin->instance, "ny_on_response"), 6,
          args);
      if (offset != 0)
        {
          wasm_runtime_module_free(plugin->instance, offset);
        }
      return ret;
    }
#endif
  return -EAGAIN;
}

int ny_wasm_run(const char *path, const char *id, uint64_t permissions)
{
  struct ny_plugin_config_s config;

  if (path == NULL || id == NULL || id[0] == '\0')
    {
      return -EINVAL;
    }

  memset(&config, 0, sizeof(config));
  strlcpy(config.id, id, sizeof(config.id));
  strlcpy(config.entry, path, sizeof(config.entry));
  config.permissions = permissions;
  config.memory_limit = CONFIG_NYABULA_CORE_PLUGIN_MEMORY;
  config.stack_limit = CONFIG_NYABULA_CORE_PLUGIN_STACK;
  config.event_timeout_ms = CONFIG_NYABULA_CORE_EVENT_TIMEOUT_MS;
  return ny_wasm_run_config(&config);
}

int ny_wasm_run_config(const struct ny_plugin_config_s *config)
{
  struct ny_wasm_plugin_s plugin;
  int ret;

  if (config == NULL)
    {
      return -EINVAL;
    }

  ret = ny_wasm_load(&plugin, config);
  if (ret >= 0)
    {
      ret = ny_wasm_start(&plugin);
    }

  if (ret >= 0)
    {
      ret = ny_wasm_stop(&plugin);
    }

  ny_wasm_destroy(&plugin);
  return ret;
}

int ny_wasm_load(struct ny_wasm_plugin_s *plugin,
                 const struct ny_plugin_config_s *config)
{
  InstantiationArgs arguments;
  char error[NY_WASM_ERROR_SIZE];
  unsigned int generation;
  int ret;

  if (plugin == NULL)
    {
      return -EINVAL;
    }

  /* Every failed load must leave an object safe for destroy. */

  memset(plugin, 0, sizeof(*plugin));
  plugin->owner = pthread_self();
  if (config == NULL || config->id[0] == '\0' || config->entry[0] == '\0' ||
      config->stack_limit == 0 || config->stack_limit > UINT32_MAX ||
      config->memory_limit < 65536 || config->event_timeout_ms == 0)
    {
      return -EINVAL;
    }

  generation = atomic_load(&g_wasm_generation);
  while (generation <= INT32_MAX &&
         !atomic_compare_exchange_weak(&g_wasm_generation, &generation,
                                       generation + 1))
    {
    }
  if (generation > INT32_MAX)
    {
      return -EOVERFLOW;
    }
  plugin->generation = generation;
  plugin->next_request = 1;
  plugin->next_lifecycle = 2;
  atomic_init(&plugin->permission_generation, 1);
  strlcpy(plugin->id, config->id, sizeof(plugin->id));
  strlcpy(plugin->storage_root, config->storage_root,
          sizeof(plugin->storage_root));
  atomic_init(&plugin->permissions, config->permissions);

  ret = ny_wasm_initialize();
  if (ret < 0 || (ret = ny_wasm_read(config->entry, &plugin->binary,
                                     &plugin->binary_size)) < 0)
    {
      return ret;
    }

  if (!wasm_runtime_thread_env_inited())
    {
      if (!wasm_runtime_init_thread_env())
        {
          ny_wasm_destroy(plugin);
          return -EIO;
        }

      plugin->thread_initialized = true;
    }

  plugin->module = wasm_runtime_load(plugin->binary, plugin->binary_size,
                                     error, sizeof(error));
  if (plugin->module == NULL)
    {
      fprintf(stderr, "nycore: wasm load failed: %s\n", error);
      ny_wasm_destroy(plugin);
      return -ENOEXEC;
    }

  memset(&arguments, 0, sizeof(arguments));
  arguments.default_stack_size = (uint32_t)config->stack_limit;
  arguments.host_managed_heap_size = 65536;
  arguments.max_memory_pages = (uint32_t)(config->memory_limit / (64 * 1024));
  plugin->instance = wasm_runtime_instantiate_ex(plugin->module, &arguments,
                                                 error, sizeof(error));
  if (plugin->instance == NULL)
    {
      fprintf(stderr, "nycore: wasm instantiate failed: %s\n", error);
      ny_wasm_destroy(plugin);
      return -ENOMEM;
    }

  plugin->environment = wasm_runtime_create_exec_env(
      plugin->instance, (uint32_t)config->stack_limit);
  if (plugin->environment == NULL)
    {
      ny_wasm_destroy(plugin);
      return -ENOMEM;
    }

  wasm_runtime_set_user_data(plugin->environment, plugin);
  plugin->event_timeout_ms = config->event_timeout_ms;
  return 0;
}

int ny_wasm_start(struct ny_wasm_plugin_s *plugin)
{
  int ret;

  if (plugin == NULL || plugin->environment == NULL)
    {
      return -EINVAL;
    }

  ret = ny_wasm_call(plugin, "_initialize", false);
  return ret < 0 ? ret : ny_wasm_call(plugin, "ny_on_start", true);
}

int ny_wasm_dispatch(struct ny_wasm_plugin_s *plugin, const char *event)
{
  return plugin == NULL || plugin->environment == NULL || event == NULL
             ? -EINVAL
             : ny_wasm_call_event(plugin, event);
}

int ny_wasm_stop(struct ny_wasm_plugin_s *plugin)
{
  if (plugin != NULL && !pthread_equal(plugin->owner, pthread_self()))
    {
      return -EPERM;
    }
  if (plugin != NULL)
    {
      plugin->stopping = true;
      ny_wasm_cancel_requests(plugin);
    }
  return plugin == NULL || plugin->environment == NULL
             ? -EINVAL
             : ny_wasm_call(plugin, "ny_on_stop", false);
}

void ny_wasm_set_permissions(struct ny_wasm_plugin_s *plugin,
                             uint64_t permissions)
{
  if (plugin != NULL)
    {
      atomic_fetch_add(&plugin->permission_generation, 1);
      atomic_store(&plugin->permissions, permissions);
    }
}

void ny_wasm_destroy(struct ny_wasm_plugin_s *plugin)
{
  if (plugin == NULL)
    {
      return;
    }

  ny_wasm_cancel_requests(plugin);
  if (plugin->environment != NULL)
    {
      wasm_runtime_destroy_exec_env(plugin->environment);
    }

  if (plugin->instance != NULL)
    {
      wasm_runtime_deinstantiate(plugin->instance);
    }

  if (plugin->module != NULL)
    {
      wasm_runtime_unload(plugin->module);
    }

  if (plugin->thread_initialized)
    {
      wasm_runtime_destroy_thread_env();
    }

  free(plugin->binary);
  memset(plugin, 0, sizeof(*plugin));
}

#else

bool ny_wasm_pending(struct ny_wasm_plugin_s *plugin) { return false; }
int ny_wasm_poll(struct ny_wasm_plugin_s *plugin) { return -EAGAIN; }

int ny_wasm_run(const char *path, const char *id, uint64_t permissions)
{
  return -ENOTSUP;
}

int ny_wasm_run_config(const struct ny_plugin_config_s *config)
{
  return -ENOTSUP;
}

int ny_wasm_load(struct ny_wasm_plugin_s *plugin,
                 const struct ny_plugin_config_s *config)
{
  return -ENOTSUP;
}

int ny_wasm_start(struct ny_wasm_plugin_s *plugin) { return -ENOTSUP; }

int ny_wasm_dispatch(struct ny_wasm_plugin_s *plugin, const char *event)
{
  return -ENOTSUP;
}

int ny_wasm_stop(struct ny_wasm_plugin_s *plugin) { return -ENOTSUP; }

void ny_wasm_set_permissions(struct ny_wasm_plugin_s *plugin,
                             uint64_t permissions)
{
}

void ny_wasm_destroy(struct ny_wasm_plugin_s *plugin)
{
  if (plugin != NULL)
    {
      memset(plugin, 0, sizeof(*plugin));
    }
}

#endif
