/****************************************************************************
 * app/nyabula_core/ny_product_speaker.h
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

#ifndef __APP_NYABULA_CORE_NY_PRODUCT_SPEAKER_H
#define __APP_NYABULA_CORE_NY_PRODUCT_SPEAKER_H
#include "ny_product.h"
#include "nyamp_protocol.h"
int ny_product_speaker_initialize(void);
void ny_product_speaker_tick(void);
void ny_product_speaker_shutdown(void);
bool ny_product_speaker_on_wake(uint64_t stream_id,
                                const struct nyamp_kws_detected_s *wake,
                                uint32_t probe_id);
int ny_product_speaker_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result);
#endif
