/****************************************************************************
 * app/nyabula_core/ny_web_auth.h
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
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __NYABULA_CORE_NY_WEB_AUTH_H
#define __NYABULA_CORE_NY_WEB_AUTH_H

#include <stdbool.h>
#include <stdint.h>

#define NY_WEB_AUTH_TOKEN_SIZE   64
#define NY_WEB_AUTH_PASSWORD_MIN 8
#define NY_WEB_AUTH_PASSWORD_MAX 64

/* The owner's password, and the session tokens that follow from it.
 *
 * Two credentials open the panel.  The pair token is the one the device
 * shows as a QR code on its eyes: reading it means standing in front of the
 * device, which is what entitles its holder to set or reset the password.
 * A session token is what a correct password is exchanged for, and is what
 * a browser keeps.  It is derived from the pair token and the password
 * hash, so it survives a restart without being stored and stops working
 * the moment the password changes.
 */

/* Read the stored password record.  A missing record is not an error: it
 * is a device whose owner has not chosen a password yet.
 */

int ny_web_auth_load(const char *path);

bool ny_web_auth_has_password(void);

/* Check a password.  0 if it matches; -EACCES if it does not; -EAGAIN if
 * attempts are being refused, with *retry_after_ms saying for how long;
 * -ENOENT if no password has been set.  retry_after_ms may be NULL.
 */

int ny_web_auth_check(const char *password, uint32_t *retry_after_ms);

/* Whether attempts are currently refused, and for how much longer. */

bool ny_web_auth_locked(uint32_t *retry_after_ms);

/* Replace the password and store it.  -EINVAL if it is shorter than
 * NY_WEB_AUTH_PASSWORD_MIN or longer than NY_WEB_AUTH_PASSWORD_MAX.
 */

int ny_web_auth_set(const char *password);

/* Derive the session token for the current password.  out must hold
 * NY_WEB_AUTH_TOKEN_SIZE + 1 bytes.  -ENOENT if no password has been set.
 */

int ny_web_auth_session_token(const char *pair_token, char *out);

#endif /* __NYABULA_CORE_NY_WEB_AUTH_H */
