/****************************************************************************
 * app/nyabula_dsp/tests/test_resample.c
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
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define NYADSP_TEST_PI 3.14159265358979323846

static double nyadsp_test_gain(unsigned int rate, double frequency);

/****************************************************************************
 * Name: nyadsp_test_gain
 ****************************************************************************/

static double nyadsp_test_gain(unsigned int rate, double frequency)
{
  struct nyadsp_resample_s *src = nyadsp_resample_create(rate);
  double power = 0;
  size_t measured = 0;
  unsigned int i;
  assert(src != NULL);
  for (i = 0; i < rate; i++)
    {
      float value = 0.25 * sin(2 * NYADSP_TEST_PI * frequency * i / rate);
      float input[2] = { value, value };
      float output[NYADSP_RESAMPLE_MAX_OUTPUT * 2];
      size_t count = nyadsp_resample_push(src, input, output);
      size_t j;
      if (i > rate / 10)
        {
          for (j = 0; j < count; j++)
            {
              power += output[2 * j] * output[2 * j];
              measured++;
            }
        }
    }

  nyadsp_resample_destroy(src);
  return sqrt(power / measured) * sqrt(2.0) / 0.25;
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(void)
{
  const unsigned int rates[] = { 8000,  16000, 24000,  32000, 44100,
                                 48000, 96000, 192000, 12345 };
  unsigned int r;
  for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
    {
      struct nyadsp_resample_s *src = nyadsp_resample_create(rates[r]);
      size_t total = 0;
      double power = 0;
      size_t measured = 0;
      unsigned int crossings = 0;
      float last = 0;
      unsigned int i;
      assert(src != NULL);
      for (i = 0; i < rates[r]; i++)
        {
          float v = 0.25 * sin(2 * NYADSP_TEST_PI * 997 * i / rates[r]);
          float input[2] = { v, -v };
          float output[12];
          size_t count = nyadsp_resample_push(src, input, output);
          size_t j;
          assert(count <= NYADSP_RESAMPLE_MAX_OUTPUT);
          for (j = 0; j < count; j++)
            {
              assert(isfinite(output[2 * j]));
              assert(fabs(output[2 * j] + output[2 * j + 1]) < 1e-7);
              if (total > 4800)
                {
                  power += output[2 * j] * output[2 * j];
                  measured++;
                  if (last <= 0 && output[2 * j] > 0)
                    crossings++;
                }
              last = output[2 * j];
              total++;
            }
        }
      assert(total == 48000);
      assert(fabs(sqrt(power / measured) - 0.25 / sqrt(2.0)) < 0.002);
      assert(crossings >= 896 && crossings <= 898);
      printf("SRC %u -> 48000: frames=%zu, RMS=%.6f, crossings=%u\n", rates[r],
             total, sqrt(power / measured), crossings);
      nyadsp_resample_destroy(src);
    }
  {
    double pass = nyadsp_test_gain(44100, 15000);
    double stop96 = nyadsp_test_gain(96000, 30000);
    double stop192 = nyadsp_test_gain(192000, 40000);
    printf("SRC 15k pass=%.3f dB, 96k alias=%.1f dB, 192k alias=%.1f dB\n",
           20 * log10(pass), 20 * log10(stop96), 20 * log10(stop192));
    assert(fabs(20 * log10(pass)) < 0.1);
    assert(stop96 < 0.001 && stop192 < 0.001);
  }
  assert(nyadsp_resample_create(0) == NULL);
  assert(nyadsp_resample_create(192001) == NULL);
  return 0;
}
