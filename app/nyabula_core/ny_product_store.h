/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_store.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __NYABULA_CORE_NY_PRODUCT_STORE_H
#define __NYABULA_CORE_NY_PRODUCT_STORE_H

#include <netutils/cJSON.h>
#include <stddef.h>
#include <stdint.h>

/* Reads return owned JSON. Writes borrow JSON and use compare-and-swap.
 * A failed write never changes the stored revision or payload.
 */
int ny_product_store_read(const char *domain, cJSON **value,
                          uint64_t *revision);
int ny_product_store_write(const char *domain, const cJSON *value,
                           uint64_t expected, uint64_t *revision);
int ny_product_json_check(const char *text, size_t length);

#endif
