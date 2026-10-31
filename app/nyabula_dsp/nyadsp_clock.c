/****************************************************************************
 * app/nyabula_dsp/nyadsp_clock.c
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

#include "nyadsp_clock.h"
#include <math.h>
#include <string.h>

/****************************************************************************
 * Name: nyadsp_clock_init
 ****************************************************************************/

void nyadsp_clock_init(struct nyadsp_clock_s *clock)
{
  memset(clock, 0, sizeof(*clock));
  clock->ratio = 1.0;
}

/****************************************************************************
 * Name: nyadsp_clock_process
 *
 * Description:
 *   The satellite device paces the producer. A bounded, smoothed queue
 *   correction resamples the mono bass for the independent USB clock.
 *   Interpolation history crosses blocks; callers serialize this state.
 ****************************************************************************/

size_t nyadsp_clock_process(struct nyadsp_clock_s *clock,
                           const float *interleaved, size_t frames,
                           float *mono, size_t capacity,
                           size_t queued_frames, size_t target_frames)
{
  double correction;
  size_t count = 0;

  if (clock == NULL || interleaved == NULL || mono == NULL ||
      frames == 0 || frames > 4096 || target_frames == 0 ||
      capacity < (size_t)ceil(frames * 1.005) + 2)
    {
      return 0;
    }

  correction = ((double)target_frames - queued_frames) /
               512.0 * 0.005;
  correction = fmin(0.005, fmax(-0.005, correction));
  clock->ratio += (1.0 + correction - clock->ratio) * 0.02;
  if (!clock->initialized)
    {
      clock->previous = interleaved[2];
      clock->initialized = true;
    }

  while (clock->position < (double)frames - 1.0)
    {
      int index = (int)floor(clock->position);
      double fraction = clock->position - index;
      float left = index < 0 ? clock->previous : interleaved[index * 3 + 2];
      float right = interleaved[(index + 1) * 3 + 2];
      mono[count++] = (float)(left + (right - left) * fraction);
      clock->position += 1.0 / clock->ratio;
    }

  clock->position -= frames;
  clock->previous = interleaved[(frames - 1) * 3 + 2];
  return count;
}
