/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_wasm.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_WASM_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_WASM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "ny_manifest.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

#define NY_WASM_MAX_PENDING 8

struct ny_wasm_pending_s
{
  struct ny_broker_http_s *http;
  uint32_t request;
  uint32_t permission_generation;
};

struct ny_wasm_plugin_s
{
  pthread_t owner;
  int (*pump)(void *opaque, uint64_t deadline_ns);
  void *pump_opaque;
  uint32_t generation;
  uint32_t next_request;
  uint32_t next_lifecycle;
  uint64_t lifecycle_token;
  uint64_t lifecycle_deadline_ns;
  uint64_t execution_remaining_ns;
  int lifecycle_result;
  bool lifecycle_active;
  bool lifecycle_deferred;
  bool lifecycle_waiting;
  bool stopping;
  atomic_uint permission_generation;
  struct ny_wasm_pending_s pending[NY_WASM_MAX_PENDING];
  void *module;
  void *instance;
  void *environment;
  uint8_t *binary;
  uint32_t binary_size;
  uint32_t event_timeout_ms;
  atomic_uint_fast64_t permissions;
  bool thread_initialized;
  char id[NY_PLUGIN_ID_SIZE];
  char storage_root[PATH_MAX];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_wasm_run(const char *path, const char *id, uint64_t permissions);
int ny_wasm_run_config(const struct ny_plugin_config_s *config);
int ny_wasm_load(struct ny_wasm_plugin_s *plugin,
                 const struct ny_plugin_config_s *config);
int ny_wasm_start(struct ny_wasm_plugin_s *plugin);
int ny_wasm_dispatch(struct ny_wasm_plugin_s *plugin, const char *event);
int ny_wasm_stop(struct ny_wasm_plugin_s *plugin);
bool ny_wasm_pending(struct ny_wasm_plugin_s *plugin);
int ny_wasm_poll(struct ny_wasm_plugin_s *plugin);
void ny_wasm_set_permissions(struct ny_wasm_plugin_s *plugin,
                             uint64_t permissions);
void ny_wasm_destroy(struct ny_wasm_plugin_s *plugin);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_WASM_H */
