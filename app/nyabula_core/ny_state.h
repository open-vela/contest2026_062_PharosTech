/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_state.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_STATE_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_STATE_H

#include <stddef.h>

struct ny_state_update_s
{
  const char *key;
  const void *value;
  size_t length;
};

/* SQLite is owned exclusively by the Core broker process. */

int ny_state_read(const char *key, char **value, size_t *length, size_t limit);
int ny_state_write(const char *key, const void *value, size_t length);
int ny_state_write_many(const struct ny_state_update_s *updates, size_t count);
int ny_state_storage_write(const char *key, const void *value, size_t length,
                           size_t byte_limit, size_t key_limit);

#endif
