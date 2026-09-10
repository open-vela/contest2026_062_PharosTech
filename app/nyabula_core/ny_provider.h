/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_provider.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_PROVIDER_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_PROVIDER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_provider_ops_s
{
  int (*ui_notify)(void *context, const char *plugin_id, const char *message,
                   size_t length);
  int (*ai_invoke)(void *context, const char *plugin_id, const char *prompt,
                   size_t prompt_length, char *response,
                   size_t response_capacity);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Providers are registered during Core startup and remain valid for the
 * process lifetime. Runtime replacement is intentionally unsupported.
 */

int ny_provider_register(const struct ny_provider_ops_s *ops, void *context);
int ny_provider_ui_notify(const char *plugin_id, const char *message,
                          size_t length);
int ny_provider_ai_invoke(const char *plugin_id, const char *prompt,
                          size_t prompt_length, char *response,
                          size_t response_capacity);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_PROVIDER_H */
