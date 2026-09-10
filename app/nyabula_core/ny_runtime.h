/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_runtime.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_RUNTIME_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_RUNTIME_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <quickjs/quickjs.h>

#include "ny_manifest.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum ny_plugin_state_e
{
  NY_PLUGIN_EMPTY = 0,
  NY_PLUGIN_LOADED,
  NY_PLUGIN_RUNNING,
  NY_PLUGIN_STOPPED,
  NY_PLUGIN_FAILED
};

#define NY_PLUGIN_MAX_PENDING 8

struct ny_plugin_pending_s
{
#ifdef CONFIG_NYABULA_CORE_HTTP
  struct ny_broker_http_s *http;
  int http_error;
#endif
  bool occupied;
  uint32_t request;
  uint64_t permission;
  uint32_t permission_generation;
  JSValue resolve;
  JSValue reject;
};

struct ny_plugin_s
{
  JSRuntime *runtime;
  JSContext *context;
  JSModuleDef *module;
  pthread_t owner;
  int (*pump)(void *opaque, uint64_t deadline_ns);
  void *pump_opaque;
  enum ny_plugin_state_e state;
  uint64_t deadline_ns;
  uint64_t execution_remaining_ns;
  uint64_t lifecycle_deadline_ns;
  bool lifecycle_active;
  uint64_t requested_permissions;
  atomic_uint_fast64_t permissions;
  atomic_uint permission_generation;
  size_t memory_limit;
  size_t stack_limit;
  uint32_t event_timeout_ms;
  uint32_t unhandled_rejections;
  uint32_t generation;
  uint32_t next_request;
  int lifecycle_result;
  uint32_t lifecycle_generation;
  bool module_entry;
  bool cancelling;
  atomic_bool cancelled;
  char id[NY_PLUGIN_ID_SIZE];
  char version[NY_PLUGIN_VERSION_SIZE];
  char root[PATH_MAX];
  char storage_root[PATH_MAX];
  char path[PATH_MAX];
  struct ny_plugin_pending_s pending[NY_PLUGIN_MAX_PENDING];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_plugin_load(struct ny_plugin_s *plugin, const char *path);
int ny_plugin_load_config(struct ny_plugin_s *plugin,
                          const struct ny_plugin_config_s *config);
int ny_plugin_start(struct ny_plugin_s *plugin);
int ny_plugin_dispatch(struct ny_plugin_s *plugin, const char *event);
int ny_plugin_stop(struct ny_plugin_s *plugin);
int ny_plugin_async_begin(struct ny_plugin_s *plugin, JSValue *promise,
                          uint64_t *token);
int ny_plugin_async_begin_authorized(struct ny_plugin_s *plugin,
                                     uint64_t permission, JSValue *promise,
                                     uint64_t *token);
int ny_plugin_async_deliver(struct ny_plugin_s *plugin, uint64_t token,
                            int status, const char *payload, size_t length);
int ny_plugin_async_complete(struct ny_plugin_s *plugin, uint64_t token,
                             bool rejected, JSValueConst value);
int ny_plugin_async_cancel_all(struct ny_plugin_s *plugin);
#ifdef CONFIG_NYABULA_CORE_HTTP
/* HTTP operations and pending inspection belong to the runtime owner. */
int ny_plugin_http_request(struct ny_plugin_s *plugin, const char *url,
                           JSValue *promise);
bool ny_plugin_http_pending(struct ny_plugin_s *plugin);
int ny_plugin_http_poll(struct ny_plugin_s *plugin);
#endif
void ny_plugin_destroy(struct ny_plugin_s *plugin);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_RUNTIME_H */
