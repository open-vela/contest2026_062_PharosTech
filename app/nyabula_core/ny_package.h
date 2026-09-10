/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_package.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_PACKAGE_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_PACKAGE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_package_install(const char *source);
int ny_package_activate(const char *id, const char *version);
int ny_package_rollback(const char *id);
int ny_package_resolve(const char *id, char *path, size_t size);
int ny_package_resolve_version(const char *id, const char *version, char *path,
                               size_t size);
int ny_package_storage_root(const char *id, char *path, size_t size);
int ny_package_list(void);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_PACKAGE_H */
