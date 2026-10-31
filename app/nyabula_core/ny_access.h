/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_access.h
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

#ifndef __NYABULA_CORE_NY_ACCESS_H
#define __NYABULA_CORE_NY_ACCESS_H
#include "ny_product.h"
#include <stdint.h>
int ny_access_auth(const char *token, char *id, size_t size,
                   enum ny_product_role_e *role, uint32_t *generation);
uint32_t ny_access_generation(void);
bool ny_access_allowed(enum ny_product_role_e role, const char *topic);
const char *ny_access_role_name(enum ny_product_role_e role);
int ny_access_request(const struct ny_product_caller_s *caller,
                      const char *topic, const cJSON *data, cJSON **result);
#endif
