/****************************************************************************
 * app/nyabula_dsp/nyadsp.h
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

#ifndef __APP_NYABULA_DSP_NYADSP_H
#define __APP_NYABULA_DSP_NYADSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NYADSP_API_VERSION      1
#define NYADSP_SAMPLE_RATE      48000
#define NYADSP_OUTPUTS          3
#define NYADSP_EQ_BANDS         4
#define NYADSP_MAX_DELAY_FRAMES 4800
#define NYADSP_MAX_BLOCK_FRAMES 4096

#define NYADSP_OK               0
#define NYADSP_EINVAL           (-1)
#define NYADSP_ENOMEM           (-2)

/* Channel order is left satellite, right satellite, mono bass. */

struct nyadsp_eq_s
{
  float frequency_hz;
  float q;
  float gain_db;
};

struct nyadsp_output_s
{
  float gain_db;
  int32_t polarity; /* +1 or -1 */
  uint32_t delay_frames;
  struct nyadsp_eq_s eq[NYADSP_EQ_BANDS];
};

struct nyadsp_config_s
{
  uint32_t sample_rate;
  float crossover_hz;   /* LR4, 40..5000 Hz */
  float master_gain_db; /* -30..0 dB */
  float ceiling;        /* Linked sample peak limiter, 0.1..0.99 */
  int32_t bypass;       /* Dry stereo + muted bass; master/limiter stay */
  struct nyadsp_eq_s eq[NYADSP_EQ_BANDS];
  struct nyadsp_output_s output[NYADSP_OUTPUTS];
};

struct nyadsp_stats_s
{
  uint64_t processed_frames; /* Includes any caller-provided drain silence */
  uint64_t limited_frames;
  float input_peak;
  float output_peak;
};

struct nyadsp_s;

uint32_t nyadsp_api_version(void);
void nyadsp_default_config(struct nyadsp_config_s *config);

/* Create/reset/destroy must run outside the realtime processing callback.
 * Configuration is copied, immutable for this instance, and fully validated.
 * A single owner must serialize all calls for a given instance.
 */

struct nyadsp_s *nyadsp_create(const struct nyadsp_config_s *config,
                               int *error);
void nyadsp_reset(struct nyadsp_s *dsp);
void nyadsp_destroy(struct nyadsp_s *dsp);

/* Input: interleaved stereo float samples in [-1, 1].
 * Output: interleaved left/right/bass float samples, frames * 3 elements.
 * Buffers must not overlap. No allocation, I/O or blocking occurs here.
 * Invalid blocks leave state and output unchanged. Up to 4096 frames per call.
 * Zero frames are accepted without sample buffers. Output has no automatic
 * tail flushing: the caller supplies silence to drain filters/delays.
 */

int nyadsp_process(struct nyadsp_s *dsp, const float *input, float *output,
                   size_t frames);
int nyadsp_get_stats(const struct nyadsp_s *dsp, struct nyadsp_stats_s *stats);

#ifdef __cplusplus
}
#endif

#endif /* __APP_NYABULA_DSP_NYADSP_H */
