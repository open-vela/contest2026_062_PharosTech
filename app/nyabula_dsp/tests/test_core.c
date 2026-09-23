/****************************************************************************
 * app/nyabula_dsp/tests/test_core.c
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

#include "nyadsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test)                                                          \
  do                                                                         \
    {                                                                        \
      if (!(test))                                                           \
        {                                                                    \
          fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #test); \
          return EXIT_FAILURE;                                               \
        }                                                                    \
    }                                                                        \
  while (0)

#define TEST_CANARY 0x13579bdfu

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(void)
{
  struct nyadsp_config_s config;
  struct nyadsp_config_s invalid;
  struct nyadsp_s *dsp;
  struct nyadsp_s *reference;
  struct nyadsp_stats_s before;
  struct nyadsp_stats_s after;
  struct
  {
    uint32_t head;
    float data[NYADSP_MAX_BLOCK_FRAMES * NYADSP_OUTPUTS];
    uint32_t tail;
  } output;
  float input[NYADSP_MAX_BLOCK_FRAMES * 2];
  float expected[NYADSP_MAX_BLOCK_FRAMES * NYADSP_OUTPUTS];
  int error = 123;
  size_t i;
  size_t channel;
  size_t block;

  nyadsp_default_config(&config);
  CHECK(nyadsp_api_version() == NYADSP_API_VERSION);
  CHECK(nyadsp_create(NULL, &error) == NULL && error == NYADSP_EINVAL);
  invalid = config;
  invalid.sample_rate = 44100;
  CHECK(nyadsp_create(&invalid, NULL) == NULL);
  invalid = config;
  invalid.eq[0].frequency_hz = NAN;
  CHECK(nyadsp_create(&invalid, NULL) == NULL);
  invalid = config;
  invalid.output[0].delay_frames = NYADSP_MAX_DELAY_FRAMES + 1;
  CHECK(nyadsp_create(&invalid, NULL) == NULL);
  invalid = config;
  invalid.output[2].polarity = 0;
  CHECK(nyadsp_create(&invalid, NULL) == NULL);

  dsp = nyadsp_create(&config, &error);
  reference = nyadsp_create(&config, NULL);
  CHECK(dsp != NULL && reference != NULL && error == NYADSP_OK);
  CHECK(nyadsp_process(dsp, NULL, NULL, 0) == NYADSP_OK);
  CHECK(nyadsp_process(NULL, NULL, NULL, 0) == NYADSP_EINVAL);
  CHECK(nyadsp_get_stats(NULL, &before) == NYADSP_EINVAL);
  CHECK(nyadsp_get_stats(dsp, NULL) == NYADSP_EINVAL);

  output.head = TEST_CANARY;
  output.tail = TEST_CANARY;
  for (i = 0; i < NYADSP_MAX_BLOCK_FRAMES * 2; i++)
    {
      input[i] = (float)sin((double)i * 0.035) * 0.2f;
    }

  for (i = 0; i < NYADSP_MAX_BLOCK_FRAMES * NYADSP_OUTPUTS; i++)
    {
      output.data[i] = 0.123f;
    }

  CHECK(nyadsp_get_stats(dsp, &before) == NYADSP_OK);
  CHECK(nyadsp_process(dsp, input, input, 10) == NYADSP_EINVAL);
  CHECK(nyadsp_process(dsp, input, input + 1, 10) == NYADSP_EINVAL);
  CHECK(nyadsp_process(dsp, NULL, output.data, 1) == NYADSP_EINVAL);
  CHECK(nyadsp_process(dsp, input, NULL, 1) == NYADSP_EINVAL);
  CHECK(nyadsp_process(dsp, input, output.data, NYADSP_MAX_BLOCK_FRAMES + 1) ==
        NYADSP_EINVAL);
  input[199] = INFINITY;
  CHECK(nyadsp_process(dsp, input, output.data, 100) == NYADSP_EINVAL);
  input[199] = 1.01f;
  CHECK(nyadsp_process(dsp, input, output.data, 100) == NYADSP_EINVAL);
  CHECK(output.data[0] == 0.123f && output.data[299] == 0.123f);
  CHECK(nyadsp_get_stats(dsp, &after) == NYADSP_OK);
  CHECK(before.processed_frames == after.processed_frames);
  input[199] = 0.0f;
  CHECK(nyadsp_process(dsp, input, output.data, 100) == NYADSP_OK);
  CHECK(nyadsp_process(reference, input, expected, 100) == NYADSP_OK);
  CHECK(memcmp(output.data, expected, 300 * sizeof(float)) == 0);
  nyadsp_destroy(dsp);
  nyadsp_destroy(reference);

  for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
    {
      config.output[channel].delay_frames = NYADSP_MAX_DELAY_FRAMES;
      config.output[channel].gain_db = 12.0f;
      config.output[channel].eq[0].gain_db = 12.0f;
    }

  config.master_gain_db = 0.0f;
  config.eq[0].gain_db = 12.0f;
  dsp = nyadsp_create(&config, NULL);
  CHECK(dsp != NULL);
  for (i = 0; i < NYADSP_MAX_BLOCK_FRAMES * 2; i++)
    {
      input[i] = (float)sin((double)(i / 2) * 2.0 * 3.14159265358979323846 *
                            1000.0 / NYADSP_SAMPLE_RATE);
    }

  for (block = 0; block < 12; block++)
    {
      CHECK(nyadsp_process(dsp, input, output.data, NYADSP_MAX_BLOCK_FRAMES) ==
            NYADSP_OK);
      for (i = 0; i < NYADSP_MAX_BLOCK_FRAMES * NYADSP_OUTPUTS; i++)
        {
          CHECK(isfinite(output.data[i]));
          CHECK(fabsf(output.data[i]) <= config.ceiling);
        }
    }

  CHECK(output.head == TEST_CANARY && output.tail == TEST_CANARY);
  CHECK(nyadsp_get_stats(dsp, &after) == NYADSP_OK);
  CHECK(after.limited_frames > 0);
  CHECK(after.processed_frames == 12 * NYADSP_MAX_BLOCK_FRAMES);
  nyadsp_reset(dsp);
  CHECK(nyadsp_get_stats(dsp, &after) == NYADSP_OK);
  CHECK(after.processed_frames == 0 && after.limited_frames == 0);
  memset(input, 0, sizeof(input));
  CHECK(nyadsp_process(dsp, input, output.data, NYADSP_MAX_BLOCK_FRAMES) ==
        NYADSP_OK);
  for (i = 0; i < NYADSP_MAX_BLOCK_FRAMES * NYADSP_OUTPUTS; i++)
    {
      CHECK(output.data[i] == 0.0f);
    }

  nyadsp_destroy(dsp);
  nyadsp_destroy(NULL);
  nyadsp_reset(NULL);
  nyadsp_default_config(NULL);
  puts("DSP_BOUNDARIES_PASS");
  return EXIT_SUCCESS;
}
