/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_voice_cloud.h
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

#ifndef __NYABULA_CORE_NY_VOICE_CLOUD_H
#define __NYABULA_CORE_NY_VOICE_CLOUD_H

#include <stddef.h>
#include <stdint.h>

/* A successful submit takes ownership of pcm. Poll returns 1 when complete,
 * 0 while pending, or a negative errno. Cancellation discards late results;
 * the bounded HTTPS request remains the worker's responsibility.
 */

int ny_voice_cloud_submit(int16_t *pcm, size_t samples, uint32_t *request);
int ny_voice_cloud_poll(uint32_t request, char *text, size_t capacity);
void ny_voice_cloud_cancel(uint32_t request);

#endif
