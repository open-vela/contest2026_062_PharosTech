/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_scheduler.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_SCHEDULER_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_scheduler_start(const char *id, const char *path);
int ny_scheduler_start_package(const char *package_path);
int ny_scheduler_start_installed(const char *package_path,
                                 const char *storage_root);
int ny_scheduler_dispatch(const char *id, const char *event);
/* Task-context provider entry: copies bytes; never accepts JS values. */

int ny_scheduler_complete(const char *id, uint64_t token, int status,
                          const char *payload, size_t length);
int ny_scheduler_stop(const char *id);
int ny_scheduler_stop_all(void);
int ny_scheduler_refresh_permissions(const char *id);
void ny_scheduler_list(void);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_SCHEDULER_H */
