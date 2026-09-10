/****************************************************************************
 * tools/nyabula_plugin/sdk/nyabula_wasm.h
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

#ifndef NYABULA_WASM_H
#define NYABULA_WASM_H

#include <stdint.h>

#define NYABULA_WASM_ABI_VERSION 1

#if defined(__wasm__)
#define NYABULA_IMPORT(name) \
  __attribute__((import_module("nyabula"), import_name(name)))
#define NYABULA_EXPORT(name) __attribute__((export_name(name)))
#else
#define NYABULA_IMPORT(name)
#define NYABULA_EXPORT(name)
#endif

/* Positive tokens belong to one instance. Negative results are target errno
 * values. Do not truncate tokens to 32 bits or reuse them across instances.
 */
typedef int64_t nyabula_token_t;

NYABULA_IMPORT("core_log")
int32_t nyabula_core_log(const char *message, uint32_t length);

NYABULA_IMPORT("storage_get")
int32_t nyabula_storage_get(const char *key, uint32_t key_length,
                            uint8_t *output, uint32_t output_capacity);
NYABULA_IMPORT("storage_put")
int32_t nyabula_storage_put(const char *key, uint32_t key_length,
                            const uint8_t *value, uint32_t value_length);

/* Legacy synchronous network entry. Real HTTP configurations return
 * -ENOTSUP; use nyabula_http_request instead.
 */

NYABULA_IMPORT("network_request")
int32_t nyabula_network_request(const uint8_t *input, uint32_t input_length,
                                uint8_t *output, uint32_t output_capacity);

NYABULA_IMPORT("ui_notify")
int32_t nyabula_ui_notify(const char *message, uint32_t length);
NYABULA_IMPORT("ui_eye")
int32_t nyabula_ui_eye(const char *json, uint32_t length);
NYABULA_IMPORT("ai_invoke")
int32_t nyabula_ai_invoke(const char *prompt, uint32_t prompt_length,
                          uint8_t *response, uint32_t response_capacity);

NYABULA_IMPORT("lifecycle_defer")
nyabula_token_t nyabula_lifecycle_defer(void);
NYABULA_IMPORT("lifecycle_complete")
int32_t nyabula_lifecycle_complete(nyabula_token_t token, int32_t result);

NYABULA_IMPORT("http_request")
nyabula_token_t nyabula_http_request(const char *url, uint32_t length);
NYABULA_IMPORT("http_cancel")
int32_t nyabula_http_cancel(nyabula_token_t token);

/* Define this export when importing http_request. Body is borrowed only
 * during this callback and must not be freed. Copy it to retain it.
 * Errors carry no body. Explicit cancellation and stop discard responses.
 */
NYABULA_EXPORT("ny_on_response")
void ny_on_response(nyabula_token_t token, int32_t result, uint32_t status,
                    const uint8_t *body, uint32_t length);

/* Plugin lifecycle exports. ny_on_start is required; the others are optional.
 * Event data is borrowed for the duration of ny_on_event.
 */

NYABULA_EXPORT("ny_on_start")
void ny_on_start(void);
NYABULA_EXPORT("ny_on_event")
void ny_on_event(const uint8_t *event, uint32_t length);
NYABULA_EXPORT("ny_on_stop")
void ny_on_stop(void);

#endif
