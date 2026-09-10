/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_module.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_MODULE_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_MODULE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_runtime.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void ny_module_configure(struct ny_plugin_s *plugin);
JSValue ny_module_evaluate(struct ny_plugin_s *plugin, const char *source,
                           size_t length);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_MODULE_H */
