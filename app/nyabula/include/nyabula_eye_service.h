/****************************************************************************
 * app/nyabula/include/nyabula_eye_service.h
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

#ifndef __APP_NYABULA_EYE_SERVICE_H
#define __APP_NYABULA_EYE_SERVICE_H

#include "nyabula_core.h"
#include <netutils/cJSON.h>
#include <stddef.h>

#define NYABULA_EYE_JSON_LIMIT       4096
#define NYABULA_EYE_JSON_DEPTH       16
#define NYABULA_EYE_DEFAULT_LEASE_MS 5000

#ifdef __cplusplus
extern "C" {
#endif

/* attach/tick/detach run only on the LVGL owner thread. Submit and snapshot
 * copy data and may be called from Core workers. Success means queued;
 * snapshot.last_status reports application on the display owner thread.
 */

int nyabula_eye_service_attach(lv_obj_t *left, lv_obj_t *right);
int nyabula_eye_service_tick(void);
int nyabula_eye_service_detach(void);
int nyabula_eye_service_submit(const char *source, const char *json,
                               size_t length);
int nyabula_eye_service_notify(const char *source, const char *text,
                               size_t length);
int nyabula_eye_service_snapshot(struct nyabula_core_snapshot_s *snapshot);
int nyabula_eye_json_parse_command(cJSON *json,
                                   struct nyabula_core_command_s *command,
                                   char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
