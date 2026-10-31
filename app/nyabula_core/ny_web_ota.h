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

/* The volume that file lives on.  A partition that backs it can never be
 * written from it.
 */

#define NY_WEB_OTA_VOLUME "/data"

/* What an upload and an apply mean when they name no target: what they
 * meant before there was more than one.
 */

#define NY_WEB_OTA_TARGET_DEFAULT "nuttx"

#define NY_WEB_OTA_SHA256_HEX     64

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* How an image reaches the medium. */

enum ny_web_ota_kind_e
{
  NY_WEB_OTA_KIND_SLOT = 0,  /* staged into the inactive A/B slot, with its
                              * bootctrl record */
  NY_WEB_OTA_KIND_NBOOT,     /* the boot loader, in place */
  NY_WEB_OTA_KIND_PARTITION, /* raw, no bootctrl record */
};

/* The cheap test that tells the right kind of file from a wrong one. */

enum ny_web_ota_magic_e
{
  NY_WEB_OTA_MAGIC_NONE = 0,
  NY_WEB_OTA_MAGIC_ARM64,    /* "ARM\x64" at offset 56 */
  NY_WEB_OTA_MAGIC_FIT,      /* d00dfeed at offset 0 */
  NY_WEB_OTA_MAGIC_BOOTCTRL, /* "K7ABCTRL" at offset 0 */
  NY_WEB_OTA_MAGIC_FAT,      /* 55 aa at offset 510 */
};

/* One thing the panel can update.  The table is the device's, and the panel
 * renders from it: a target that is not listed here cannot be uploaded or
 * applied, whatever a client sends.
 */

struct ny_web_ota_target_s
{
  const char *id;          /* what the client names it by */
  const char *label;       /* for people, shown by the panel */
  const char *description; /* one paragraph, shown before the upload */
  enum ny_web_ota_kind_e kind;
  enum ny_web_ota_magic_e magic;
  const char *domain;       /* bootctrl domain of a slot target, or NULL */
  const char *partition;    /* partition that is written, or whose size
                             * bounds the image */
  const char *const *nodes; /* image nodes a FIT must have, or NULL */
  const char *mount;        /* where its filesystem is mounted, or NULL */
  bool advanced;            /* can leave the device unbootable */
  bool blocked;             /* listed so the panel can say why, never
                             * written */
};

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
 * worker that writes it to the medium.  The same claim covers every change
 * to the bootctrl record, because staging holds that record in memory for
 * as long as the write takes and would overwrite anything stored meanwhile;
 * a raw write to the bootctrl partition is covered for the same reason.
 * -EBUSY when it is taken.
 */

int ny_web_ota_claim(void);
void ny_web_ota_release(void);

/* The targets, in the order the panel shows them.  NULL past the last. */

const struct ny_web_ota_target_s *ny_web_ota_target_at(size_t position);

/* The target a client named.  NULL when there is none by that name. */

const struct ny_web_ota_target_s *ny_web_ota_target_find(const char *id,
                                                         size_t length);

/* The largest image a target takes: the size of its partition.  0 when that
 * is unknown, which is never an invitation to write.
 */

uint64_t ny_web_ota_target_capacity(const struct ny_web_ota_target_s *target);

/* What the staging volume can hold right now, the safety margin taken off.
 * An image has to fit here before it can fit anywhere else.
 */

uint64_t ny_web_ota_staging_space(void);

/* Whether a file is what a target takes: size, magic and, for a FIT, the
 * image nodes.  0, -EFBIG, -ENOEXEC, or the errno of a failed read.  The
 * upload checks the same magic on the stream; this is for the file as it is
 * when it is about to be written, which may be after a restart.
 */

int ny_web_ota_file_check(const struct ny_web_ota_target_s *target,
                          const char *path);

/* SHA-256 of a file as lowercase hex.  hex must hold
 * NY_WEB_OTA_SHA256_HEX + 1 bytes.  size may be NULL.
 */

int ny_web_ota_file_digest(const char *path, char *hex, uint64_t *size);

/* Whether text is exactly NY_WEB_OTA_SHA256_HEX hex digits. */

bool ny_web_ota_digest_ok(const char *text, size_t length);

#endif /* __NYABULA_CORE_NY_WEB_OTA_H */
