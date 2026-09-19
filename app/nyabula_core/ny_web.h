/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_web.h
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

#ifndef __NYABULA_CORE_NY_WEB_H
#define __NYABULA_CORE_NY_WEB_H
#include <stddef.h>

int ny_web_run(int argc, char **argv);
int ny_web_stop(void);

/* Token of the running product service, for the pairing QR only: never to
 * be forwarded to a client.  size must exceed 64.  -ENOENT if not running.
 */

int ny_web_product_token(char *out, size_t size);

/* Number of panels currently open and authenticated. */

int ny_web_panel_count(void);

/* Answer one plain HTTP request and return.  head is the request head as
 * received; the connection is not kept alive.
 *
 * body points at the body_length bytes of a request body that were read
 * off the socket together with the head; the remainder is still unread.
 * pair_token is the credential of the running service, which the one
 * request that acts on the device (the firmware upload) is checked against.
 */

int ny_web_http_serve(int fd, const char *head, const void *body,
                      size_t body_length, const char *pair_token);

/* Send a whole buffer, giving up when the peer stops reading. */

int ny_web_http_write(int fd, const void *data, size_t length);

/* Find a header in a raw request head.  Returns a pointer to its value and
 * sets *length to the value's length, or returns NULL.  The value is not
 * NUL-terminated.
 */

const char *ny_web_http_header(const char *head, const char *name,
                               size_t *length);
#endif
