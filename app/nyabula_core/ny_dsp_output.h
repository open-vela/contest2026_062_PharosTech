/****************************************************************************
 * app/nyabula_core/ny_dsp_output.h
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

#ifndef __NYABULA_CORE_NY_DSP_OUTPUT_H
#define __NYABULA_CORE_NY_DSP_OUTPUT_H
#include "ny_audio_stream.h"
struct ny_product_dsp_settings_s;
struct nyadsp_s;
struct ny_dsp_output_s;
struct nyadsp_s *
ny_dsp_output_create(const struct ny_product_dsp_settings_s *settings);
int ny_dsp_output_open(struct ny_dsp_output_s **output,
                       const struct ny_audio_stream_config_s *config);
ssize_t ny_dsp_output_write(struct ny_dsp_output_s *output, const void *pcm,
                            size_t bytes, int timeout_ms);
size_t ny_dsp_output_queued(struct ny_dsp_output_s *output);
int ny_dsp_output_pause(struct ny_dsp_output_s *output, bool paused);
int ny_dsp_output_drain(struct ny_dsp_output_s *output, int timeout_ms);
void ny_dsp_output_stats(struct ny_dsp_output_s *output,
                         struct ny_audio_stream_stats_s *stats);
void ny_dsp_output_close(struct ny_dsp_output_s *output);
#endif
