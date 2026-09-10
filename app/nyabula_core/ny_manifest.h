/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_manifest.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_MANIFEST_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_MANIFEST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_PLUGIN_ID_SIZE             64
#define NY_PLUGIN_VERSION_SIZE        32
#define NY_PLUGIN_API_VERSION         1

#define NY_PERMISSION_CORE_LOG        (1ull << 0)
#define NY_PERMISSION_STORAGE_READ    (1ull << 1)
#define NY_PERMISSION_STORAGE_WRITE   (1ull << 2)
#define NY_PERMISSION_NETWORK_REQUEST (1ull << 3)
#define NY_PERMISSION_UI_NOTIFY       (1ull << 4)
#define NY_PERMISSION_AI_INVOKE       (1ull << 5)

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum ny_plugin_runtime_e
{
  NY_PLUGIN_RUNTIME_QUICKJS = 0,
  NY_PLUGIN_RUNTIME_WAMR
};

struct ny_plugin_config_s
{
  char id[NY_PLUGIN_ID_SIZE];
  char version[NY_PLUGIN_VERSION_SIZE];
  char root[PATH_MAX];
  char storage_root[PATH_MAX];
  char entry[PATH_MAX];
  uint64_t requested_permissions;
  uint64_t permissions;
  enum ny_plugin_runtime_e runtime;
  bool module;
  bool background;
  size_t memory_limit;
  size_t stack_limit;
  uint32_t event_timeout_ms;
  uint32_t events_per_minute;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void ny_manifest_default_config(struct ny_plugin_config_s *config,
                                const char *entry);
int ny_manifest_load(const char *package_path,
                     struct ny_plugin_config_s *config);
int ny_manifest_permission_value(const char *name, uint64_t *value);
const char *ny_manifest_permission_name(uint64_t value);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_MANIFEST_H */
