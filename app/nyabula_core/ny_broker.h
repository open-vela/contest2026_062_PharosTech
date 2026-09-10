/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_broker.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_BROKER_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_BROKER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_broker_client_s
{
  const char *id;
  const char *storage_root;
  uint64_t permissions;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_broker_storage_get(const struct ny_broker_client_s *client,
                          const char *key, char **value, size_t *length);
int ny_broker_storage_put(const struct ny_broker_client_s *client,
                          const char *key, const char *value, size_t length);
int ny_broker_network_request(const struct ny_broker_client_s *client);
struct ny_broker_http_s;

/* Trusted runtime adapters supply a fresh permission snapshot and generation
 * on every step. An IPC server must derive these from its authenticated
 * session, never from fields supplied by a plugin. Close is owner-thread only.
 */
int ny_broker_http_open(const struct ny_broker_client_s *client,
                        uint32_t permission_generation, const char *url,
                        uint32_t timeout_ms, struct ny_broker_http_s **out);
int ny_broker_http_step(struct ny_broker_http_s *request,
                        const struct ny_broker_client_s *client,
                        uint32_t permission_generation, unsigned int *status,
                        const void **body, size_t *length);
int ny_broker_http_close(struct ny_broker_http_s *request);
int ny_broker_ui_notify(const struct ny_broker_client_s *client,
                        const char *message, size_t length);
int ny_broker_ui_eye(const struct ny_broker_client_s *client,
                     const char *command, size_t length);
int ny_broker_ai_invoke(const struct ny_broker_client_s *client,
                        const char *prompt, size_t prompt_length,
                        char *response, size_t response_capacity);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_BROKER_H */
