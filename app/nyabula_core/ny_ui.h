/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_ui.h
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

#ifndef __NYABULA_CORE_NY_UI_H
#define __NYABULA_CORE_NY_UI_H

#include "ny_broker.h"
#include <netutils/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#define NY_UI_DOCUMENT_LIMIT 24576

int ny_ui_publish(const struct ny_broker_client_s *client, uint32_t generation,
                  const char *json, size_t length);
void ny_ui_clear(const char *id, uint32_t generation);
uint64_t ny_ui_revision(void);
void ny_ui_changed(void);
int ny_ui_tree(const char *id, cJSON **tree);
int ny_ui_event(const char *id, const cJSON *event);

#endif /* __NYABULA_CORE_NY_UI_H */
