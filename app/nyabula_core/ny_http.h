/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_http.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_HTTP_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_HTTP_H

#include <stddef.h>
#include <stdint.h>

struct ny_http_s;
#define NY_HTTP_BODY_LIMIT 4096

/* All operations belong to the creating broker thread. This transport does
 * not authorize callers. The broker must check permissions before opening.
 * step returns -EAGAIN while pending. A terminal result is retained until
 * close; successful body memory belongs to the request and is not a string.
 */

int ny_http_open(const char *url, uint32_t timeout_ms, struct ny_http_s **out);
int ny_http_step(struct ny_http_s *request, unsigned int *status,
                 const void **body, size_t *length);
int ny_http_cancel(struct ny_http_s *request);
int ny_http_close(struct ny_http_s *request);

#endif
