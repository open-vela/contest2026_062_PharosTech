/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_nas.h
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

#ifndef __NYABULA_CORE_NY_NAS_H
#define __NYABULA_CORE_NY_NAS_H

#include "ny_product.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NY_NAS_PATH_MAX 192

struct ny_nas_file_s;

int ny_nas_request(const struct ny_product_caller_s *caller, const char *topic,
                   const cJSON *data, cJSON **result);
int ny_nas_tick(void);
int ny_nas_file_open(const char *path, bool writing, uint64_t expected,
                     struct ny_nas_file_s **file, uint64_t *size);
ssize_t ny_nas_file_read(struct ny_nas_file_s *file, void *data, size_t size);
ssize_t ny_nas_file_write(struct ny_nas_file_s *file, const void *data,
                          size_t size);
int ny_nas_file_seek(struct ny_nas_file_s *file, uint64_t offset);
int ny_nas_file_close(struct ny_nas_file_s *file, bool commit);
bool ny_nas_ticket_authorized(const char *path, const char *ticket);

#endif
