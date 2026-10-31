/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_dsp.h
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

#ifndef __NYABULA_CORE_NY_PRODUCT_DSP_H
#define __NYABULA_CORE_NY_PRODUCT_DSP_H
#include "ny_product.h"
#include "nyadsp.h"
int ny_product_dsp_request(const struct ny_product_caller_s *caller,
                           const char *topic, const cJSON *data,
                           cJSON **result);
struct ny_product_dsp_settings_s
{
  struct nyadsp_config_s filter;
  struct nyadsp_crossover_s crossover;
  struct nyadsp_eq_s input[NYADSP_INPUT_EQ_BANDS];
  enum nyadsp_filter_e kinds[NYADSP_INPUT_EQ_BANDS];
  bool enabled[NYADSP_INPUT_EQ_BANDS];
  bool bass;
  bool eq_enabled;
};

/* Shared output ownership and immutable configuration snapshots. */
int ny_product_dsp_acquire(const char *source,
                           struct ny_product_dsp_settings_s *settings,
                           uint32_t *revision);
void ny_product_dsp_snapshot(struct ny_product_dsp_settings_s *settings,
                             uint32_t *revision);
struct nyadsp_s *
ny_product_dsp_take_update(struct ny_product_dsp_settings_s *settings,
                           uint32_t *revision);
void ny_product_dsp_progress(size_t frames, float peak, double ppm,
                             uint32_t starved, size_t satellite_frames,
                             size_t bass_frames);
void ny_product_dsp_release(int error);
void ny_product_dsp_shutdown(void);
int ny_product_dsp_tick(void);
#endif
