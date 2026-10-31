/****************************************************************************
 * app/nyabula_dsp/nyadsp_resample.h
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

#ifndef __APP_NYABULA_DSP_NYADSP_RESAMPLE_H
#define __APP_NYABULA_DSP_NYADSP_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

/* Streaming stereo windowed-sinc SRC to 48 kHz. One owner; no allocation
 * in push. An input frame produces at most six output frames (8 kHz).
 * Flush with 48 zero input frames at end of stream, not between packets.
 */

#define NYADSP_RESAMPLE_MAX_OUTPUT 6
struct nyadsp_resample_s;
struct nyadsp_resample_s *nyadsp_resample_create(uint32_t rate);
void nyadsp_resample_destroy(struct nyadsp_resample_s *src);
size_t nyadsp_resample_push(struct nyadsp_resample_s *src,
                            const float input[2], float output[12]);
#endif
