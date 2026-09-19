/****************************************************************************
 * apps/system/nbootctl/nbootctl_bootctrl.h
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

#ifndef __APPS_SYSTEM_NBOOTCTL_NBOOTCTL_BOOTCTRL_H
#define __APPS_SYSTEM_NBOOTCTL_NBOOTCTL_BOOTCTRL_H

#include <stdbool.h>
#include <stdint.h>

/* One slot as a caller needs it.  tries_remaining is left out on purpose:
 * N-Boot selects by priority alone and never reads it.
 */

struct nbootctl_slot_state_s
{
  uint8_t priority; /* 0 = not bootable */
  bool successful;
  uint64_t image_size;
  uint64_t image_version;
};

/* The handoff N-Boot published for this boot plus the newest valid bootctrl
 * record.  When handoff_valid is false nothing else is meaningful: without
 * the handoff the medium that holds bootctrl is unknown.
 */

struct nbootctl_state_s
{
  bool handoff_valid;
  unsigned int medium;       /* 1 = SD, 2 = eMMC */
  unsigned int running_slot; /* 0 = A, 1 = B */
  uint64_t bootctrl_generation;
  unsigned int nuttx_active;
  struct nbootctl_slot_state_s nuttx[2];
  unsigned int amp_active;
  struct nbootctl_slot_state_s amp[2];
};

/* Read the handoff header twice around the generation words, so a header
 * caught mid-update is rejected.  0 on success, -ENODEV when there is no
 * valid handoff, -EBADMSG when its fields are out of range.  Any output
 * pointer may be NULL.  Prints nothing.
 */

int nbootctl_handoff_read(unsigned int *medium, unsigned int *slot,
                          unsigned int *reason, uint64_t *generation);

/* Fill *state without printing.  0 with handoff_valid false when there is
 * no handoff; a negative errno when bootctrl itself cannot be read.
 */

int nbootctl_bootctrl_snapshot(struct nbootctl_state_s *state);

int nbootctl_bootctrl_status(unsigned int medium);
int nbootctl_bootctrl_request(unsigned int medium, unsigned int target);
int nbootctl_bootctrl_verify(unsigned int medium, const char *domain,
                             unsigned int slot);
int nbootctl_bootctrl_set_active(unsigned int medium, const char *domain,
                                 unsigned int slot);
int nbootctl_bootctrl_mark_successful(unsigned int medium, const char *domain,
                                      unsigned int slot);
int nbootctl_bootctrl_stage(unsigned int medium, const char *domain,
                            unsigned int running_slot, const char *path);
int nbootctl_bootctrl_clone(unsigned int medium, const char *domain,
                            unsigned int source, unsigned int target);
int nbootctl_update_nboot(unsigned int medium, const char *path);

#endif
