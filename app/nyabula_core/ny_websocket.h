/****************************************************************************
 * app/nyabula_core/ny_websocket.h
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

#ifndef __APP_NYABULA_CORE_NY_WEBSOCKET_H
#define __APP_NYABULA_CORE_NY_WEBSOCKET_H
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NYABULA_WS_MESSAGE_MAX 32768

/* Returned by nyabula_eye_ws_upgrade() for a complete, well-formed request
 * that is not for the WebSocket endpoint.  The request head is left in the
 * scratch buffer exactly as received, NUL-terminated.
 */

#define NYABULA_WS_NOT_UPGRADE (-ENOTSUP)

uint64_t nyabula_eye_ws_now(void);
int nyabula_eye_ws_upgrade(int fd, const char *origin, char *scratch,
                           size_t capacity);
int nyabula_eye_ws_send(int fd, uint8_t opcode, const void *data,
                        size_t length);
int nyabula_eye_ws_receive(int fd, unsigned char *data, size_t capacity,
                           uint8_t *opcode, bool *final);
bool nyabula_eye_ws_utf8(const unsigned char *data, size_t length);
#endif
