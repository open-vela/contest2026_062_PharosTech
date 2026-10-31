/****************************************************************************
 * app/nyabula_dsp/nyadsp_resample.c
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

#include "nyadsp_resample.h"
#include <math.h>
#include <stdlib.h>

#define NYADSP_SRC_RATE 48000u
#define NYADSP_SRC_TAPS 48u
#define NYADSP_SRC_PI   3.14159265358979323846

struct nyadsp_resample_s
{
  uint32_t rate;
  uint32_t phases;
  uint32_t until;
  unsigned int head;
  float history[NYADSP_SRC_TAPS][2];
  float coefficients[];
};

static uint32_t nyadsp_src_gcd(uint32_t a, uint32_t b);

/****************************************************************************
 * Name: nyadsp_src_gcd
 ****************************************************************************/

static uint32_t nyadsp_src_gcd(uint32_t a, uint32_t b)
{
  while (b != 0)
    {
      uint32_t next = a % b;
      a = b;
      b = next;
    }

  return a;
}

/****************************************************************************
 * Name: nyadsp_resample_create
 ****************************************************************************/

struct nyadsp_resample_s *nyadsp_resample_create(uint32_t rate)
{
  struct nyadsp_resample_s *src;
  uint32_t phases;
  uint32_t phase;
  uint32_t tap;
  double cutoff;

  if (rate < 8000 || rate > 192000)
    {
      return NULL;
    }

  phases = NYADSP_SRC_RATE / nyadsp_src_gcd(rate, NYADSP_SRC_RATE);
  if (phases > 160)
    {
      phases = 160;
    }

  src =
      calloc(1, sizeof(*src) + (phases + 1) * NYADSP_SRC_TAPS * sizeof(float));
  if (src == NULL)
    {
      return NULL;
    }

  src->rate = rate;
  src->phases = phases;
  cutoff = 0.94 * fmin(1.0, (double)NYADSP_SRC_RATE / rate);
  for (phase = 0; phase <= phases; phase++)
    {
      double sum = 0;
      for (tap = 0; tap < NYADSP_SRC_TAPS; tap++)
        {
          double x = tap - 23.0 - (double)phase / phases;
          double sinc = fabs(x) < 1e-12 ? cutoff
                                        : sin(NYADSP_SRC_PI * cutoff * x) /
                                              (NYADSP_SRC_PI * x);
          double window = 0.42 - 0.5 * cos(2 * NYADSP_SRC_PI * tap / 47) +
                          0.08 * cos(4 * NYADSP_SRC_PI * tap / 47);
          src->coefficients[phase * NYADSP_SRC_TAPS + tap] = sinc * window;
          sum += sinc * window;
        }

      for (tap = 0; tap < NYADSP_SRC_TAPS; tap++)
        {
          src->coefficients[phase * NYADSP_SRC_TAPS + tap] /= sum;
        }
    }

  return src;
}

/****************************************************************************
 * Name: nyadsp_resample_push
 ****************************************************************************/

size_t nyadsp_resample_push(struct nyadsp_resample_s *src,
                            const float input[2], float output[12])
{
  size_t count = 0;
  uint32_t tap;

  if (src->rate == NYADSP_SRC_RATE)
    {
      output[0] = input[0];
      output[1] = input[1];
      return 1;
    }

  src->head = (src->head + 1) % NYADSP_SRC_TAPS;
  src->history[src->head][0] = input[0];
  src->history[src->head][1] = input[1];

  /* until is the time to the next output, in units of 1 / 48000 input
   * frames. Integer phase arithmetic is exact, including 160 / 147.
   */

  while (src->until < NYADSP_SRC_RATE)
    {
      double position = (double)(NYADSP_SRC_RATE - src->until) * src->phases /
                        NYADSP_SRC_RATE;
      uint32_t phase = (uint32_t)position;
      float fraction = position - phase;
      const float *coeff = src->coefficients + phase * NYADSP_SRC_TAPS;
      float left = 0;
      float right = 0;
      for (tap = 0; tap < NYADSP_SRC_TAPS; tap++)
        {
          unsigned int index =
              (src->head + NYADSP_SRC_TAPS - tap) % NYADSP_SRC_TAPS;
          float weight = coeff[tap];
          if (fraction > 0)
            {
              weight += fraction * (coeff[NYADSP_SRC_TAPS + tap] - weight);
            }

          left += src->history[index][0] * weight;
          right += src->history[index][1] * weight;
        }

      output[2 * count] = left;
      output[2 * count + 1] = right;
      count++;
      src->until += src->rate;
    }

  src->until -= NYADSP_SRC_RATE;
  return count;
}

/****************************************************************************
 * Name: nyadsp_resample_destroy
 ****************************************************************************/

void nyadsp_resample_destroy(struct nyadsp_resample_s *src) { free(src); }
