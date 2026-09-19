/****************************************************************************
 * app/nyabula_core/ny_web_ota.h
 *
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

#ifndef __NYABULA_CORE_NY_WEB_OTA_H
#define __NYABULA_CORE_NY_WEB_OTA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Where an uploaded image waits until the owner applies it.  It is a file
 * and not the slot itself on purpose: the digest of a stream is only known
 * at its end, and by then a direct write would already have replaced the
 * slot.  A transfer that fails here costs a temporary file and nothing else.
 */

#define NY_WEB_OTA_FILE "/data/tmp/ota.bin"

/* Size of a NuttX A/B slot partition on the KICKPI-K7 layout. */

#define NY_WEB_OTA_MAX_BYTES  (64ul * 1024ul * 1024ul)

#define NY_WEB_OTA_SHA256_HEX 64

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Whether a request head is for this module: its target is under /ota/. */

bool ny_web_ota_claims(const char *head);

/* Answer one request under /ota/ and return.  body and body_length are the
 * part of the request body that was read together with the head.
 */

int ny_web_ota_serve(int fd, const char *head, const void *body,
                     size_t body_length, const char *pair_token);

/* The staged file has one user at a time: an upload, a removal, or the
 * worker that writes it to a slot.  The same claim covers every change to
 * the bootctrl record, because staging holds that record in memory for as
 * long as the write takes and would overwrite anything stored meanwhile.
 * -EBUSY when it is taken.
 */

int ny_web_ota_claim(void);
void ny_web_ota_release(void);

/* SHA-256 of a file as lowercase hex.  hex must hold
 * NY_WEB_OTA_SHA256_HEX + 1 bytes.  size may be NULL.
 */

int ny_web_ota_file_digest(const char *path, char *hex, uint64_t *size);

/* Whether text is exactly NY_WEB_OTA_SHA256_HEX hex digits. */

bool ny_web_ota_digest_ok(const char *text, size_t length);

#endif /* __NYABULA_CORE_NY_WEB_OTA_H */
