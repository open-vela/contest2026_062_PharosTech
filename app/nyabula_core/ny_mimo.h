/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_mimo.h
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

#ifndef __NYABULA_CORE_NY_MIMO_H
#define __NYABULA_CORE_NY_MIMO_H

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NY_MIMO_AUDIO_RATE 24000U

typedef int (*ny_mimo_pcm_t)(void *context, const int16_t *pcm,
                             size_t samples);

bool ny_mimo_host_supported(const char *host);
int ny_mimo_config(const char *topic, const cJSON *data, cJSON **result);
bool ny_mimo_tts_enabled(void);
bool ny_mimo_asr_enabled(void);
int ny_mimo_transcribe(const int16_t *pcm, size_t samples, unsigned int rate,
                       char *text, size_t capacity);
int ny_mimo_speak(const char *text, ny_mimo_pcm_t callback, void *context);
int ny_mimo_vision_tool(const char *input, char *output, size_t capacity);

#endif
