/****************************************************************************
 * app/nyabula_dsp/nyadsp.c
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
#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NYADSP_PI              3.14159265358979323846
#define NYADSP_BUTTERWORTH_Q   0.70710678118654752440
#define NYADSP_RELEASE_SECONDS 0.080
#define NYADSP_STATE_FLOOR     1.0e-20f

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct nyadsp_biquad_s
{
  float b0;
  float b1;
  float b2;
  float a1;
  float a2;
  float z1;
  float z2;
};

struct nyadsp_s
{
  struct nyadsp_config_s config;
  struct nyadsp_biquad_s input_eq[2][NYADSP_EQ_BANDS];
  struct nyadsp_biquad_s crossover[NYADSP_OUTPUTS][2];
  struct nyadsp_biquad_s output_eq[NYADSP_OUTPUTS][NYADSP_EQ_BANDS];
  float *delay[NYADSP_OUTPUTS];
  uint32_t cursor[NYADSP_OUTPUTS];
  float gain[NYADSP_OUTPUTS];
  float master_gain;
  float limiter_gain;
  float release;
  struct nyadsp_stats_s stats;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int nyadsp_range(float value, float lower, float upper);
static int nyadsp_validate_eq(const struct nyadsp_eq_s *eq);
static int nyadsp_validate(const struct nyadsp_config_s *config);
static void nyadsp_coefficients(struct nyadsp_biquad_s *filter, double b0,
                                double b1, double b2, double a0, double a1,
                                double a2);
static void nyadsp_make_crossover(struct nyadsp_biquad_s *filter,
                                  float frequency, int highpass);
static void nyadsp_make_peak(struct nyadsp_biquad_s *filter,
                             const struct nyadsp_eq_s *eq);
static float nyadsp_filter(struct nyadsp_biquad_s *filter, float sample);
static float nyadsp_equalize(struct nyadsp_biquad_s *filters, float sample);
static void nyadsp_clear_filters(struct nyadsp_biquad_s *filters,
                                 size_t count);

/****************************************************************************
 * Name: nyadsp_range
 ****************************************************************************/

static int nyadsp_range(float value, float lower, float upper)
{
  return isfinite(value) && value >= lower && value <= upper;
}

/****************************************************************************
 * Name: nyadsp_validate_eq
 ****************************************************************************/

static int nyadsp_validate_eq(const struct nyadsp_eq_s *eq)
{
  return nyadsp_range(eq->frequency_hz, 20.0f, 18000.0f) &&
         nyadsp_range(eq->q, 0.2f, 10.0f) &&
         nyadsp_range(eq->gain_db, -12.0f, 12.0f);
}

/****************************************************************************
 * Name: nyadsp_validate
 ****************************************************************************/

static int nyadsp_validate(const struct nyadsp_config_s *config)
{
  size_t channel;
  size_t band;

  if (config == NULL || config->sample_rate != NYADSP_SAMPLE_RATE ||
      !nyadsp_range(config->crossover_hz, 40.0f, 5000.0f) ||
      !nyadsp_range(config->master_gain_db, -30.0f, 0.0f) ||
      !nyadsp_range(config->ceiling, 0.1f, 0.99f) ||
      (config->bypass != 0 && config->bypass != 1))
    {
      return 0;
    }

  for (band = 0; band < NYADSP_EQ_BANDS; band++)
    {
      if (!nyadsp_validate_eq(&config->eq[band]))
        {
          return 0;
        }
    }

  for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
    {
      const struct nyadsp_output_s *output = &config->output[channel];

      if (!nyadsp_range(output->gain_db, -24.0f, 12.0f) ||
          (output->polarity != 1 && output->polarity != -1) ||
          output->delay_frames > NYADSP_MAX_DELAY_FRAMES)
        {
          return 0;
        }

      for (band = 0; band < NYADSP_EQ_BANDS; band++)
        {
          if (!nyadsp_validate_eq(&output->eq[band]))
            {
              return 0;
            }
        }
    }

  return 1;
}

/****************************************************************************
 * Name: nyadsp_coefficients
 ****************************************************************************/

static void nyadsp_coefficients(struct nyadsp_biquad_s *filter, double b0,
                                double b1, double b2, double a0, double a1,
                                double a2)
{
  filter->b0 = (float)(b0 / a0);
  filter->b1 = (float)(b1 / a0);
  filter->b2 = (float)(b2 / a0);
  filter->a1 = (float)(a1 / a0);
  filter->a2 = (float)(a2 / a0);
}

/****************************************************************************
 * Name: nyadsp_make_crossover
 *
 * Two Butterworth sections per branch form the fourth-order LR crossover.
 * Coefficients use the bilinear transform with frequency prewarping.
 ****************************************************************************/

static void nyadsp_make_crossover(struct nyadsp_biquad_s *filter,
                                  float frequency, int highpass)
{
  double omega = 2.0 * NYADSP_PI * frequency / NYADSP_SAMPLE_RATE;
  double cosine = cos(omega);
  double alpha = sin(omega) / (2.0 * NYADSP_BUTTERWORTH_Q);
  double edge = highpass ? 1.0 + cosine : 1.0 - cosine;

  nyadsp_coefficients(filter, edge / 2.0, highpass ? -edge : edge, edge / 2.0,
                      1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
}

/****************************************************************************
 * Name: nyadsp_make_peak
 *
 * RBJ peaking equalizer coefficient equations; zero gain is exact bypass.
 * See https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
 ****************************************************************************/

static void nyadsp_make_peak(struct nyadsp_biquad_s *filter,
                             const struct nyadsp_eq_s *eq)
{
  double omega;
  double alpha;
  double cosine;
  double amplitude;

  if (eq->gain_db == 0.0f)
    {
      filter->b0 = 1.0f;
      return;
    }

  omega = 2.0 * NYADSP_PI * eq->frequency_hz / NYADSP_SAMPLE_RATE;
  alpha = sin(omega) / (2.0 * eq->q);
  cosine = cos(omega);
  amplitude = pow(10.0, eq->gain_db / 40.0);
  nyadsp_coefficients(filter, 1.0 + alpha * amplitude, -2.0 * cosine,
                      1.0 - alpha * amplitude, 1.0 + alpha / amplitude,
                      -2.0 * cosine, 1.0 - alpha / amplitude);
}

/****************************************************************************
 * Name: nyadsp_filter
 ****************************************************************************/

static float nyadsp_filter(struct nyadsp_biquad_s *filter, float sample)
{
  float result = filter->b0 * sample + filter->z1;

  filter->z1 = filter->b1 * sample - filter->a1 * result + filter->z2;
  filter->z2 = filter->b2 * sample - filter->a2 * result;

  /* Flush inaudible tails to avoid denormal-dependent execution time. */

  if (fabsf(filter->z1) < NYADSP_STATE_FLOOR)
    {
      filter->z1 = 0.0f;
    }

  if (fabsf(filter->z2) < NYADSP_STATE_FLOOR)
    {
      filter->z2 = 0.0f;
    }

  return result;
}

/****************************************************************************
 * Name: nyadsp_equalize
 ****************************************************************************/

static float nyadsp_equalize(struct nyadsp_biquad_s *filters, float sample)
{
  size_t band;

  for (band = 0; band < NYADSP_EQ_BANDS; band++)
    {
      sample = nyadsp_filter(&filters[band], sample);
    }

  return sample;
}

/****************************************************************************
 * Name: nyadsp_clear_filters
 ****************************************************************************/

static void nyadsp_clear_filters(struct nyadsp_biquad_s *filters, size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      filters[i].z1 = 0.0f;
      filters[i].z2 = 0.0f;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nyadsp_api_version
 ****************************************************************************/

uint32_t nyadsp_api_version(void) { return NYADSP_API_VERSION; }

/****************************************************************************
 * Name: nyadsp_default_config
 ****************************************************************************/

void nyadsp_default_config(struct nyadsp_config_s *config)
{
  size_t channel;
  size_t band;

  if (config == NULL)
    {
      return;
    }

  memset(config, 0, sizeof(*config));
  config->sample_rate = NYADSP_SAMPLE_RATE;
  config->crossover_hz = 500.0f;
  config->master_gain_db = -6.0f;
  config->ceiling = 0.9f;

  for (band = 0; band < NYADSP_EQ_BANDS; band++)
    {
      config->eq[band].frequency_hz = 1000.0f;
      config->eq[band].q = 1.0f;
    }

  for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
    {
      config->output[channel].polarity = 1;
      memcpy(config->output[channel].eq, config->eq, sizeof(config->eq));
    }
}

/****************************************************************************
 * Name: nyadsp_create
 ****************************************************************************/

struct nyadsp_s *nyadsp_create(const struct nyadsp_config_s *config,
                               int *error)
{
  struct nyadsp_s *dsp;
  size_t channel;
  size_t band;

  if (error != NULL)
    {
      *error = NYADSP_EINVAL;
    }

  if (!nyadsp_validate(config))
    {
      return NULL;
    }

  dsp = calloc(1, sizeof(*dsp));
  if (dsp == NULL)
    {
      if (error != NULL)
        {
          *error = NYADSP_ENOMEM;
        }

      return NULL;
    }

  dsp->config = *config;
  dsp->master_gain = powf(10.0f, config->master_gain_db / 20.0f);
  dsp->limiter_gain = 1.0f;
  dsp->release =
      (float)(1.0 - exp(-1.0 / (NYADSP_SAMPLE_RATE * NYADSP_RELEASE_SECONDS)));

  for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
    {
      dsp->gain[channel] =
          powf(10.0f, config->output[channel].gain_db / 20.0f) *
          config->output[channel].polarity;

      if (config->output[channel].delay_frames > 0)
        {
          dsp->delay[channel] =
              calloc(config->output[channel].delay_frames, sizeof(float));

          if (dsp->delay[channel] == NULL)
            {
              nyadsp_destroy(dsp);
              if (error != NULL)
                {
                  *error = NYADSP_ENOMEM;
                }

              return NULL;
            }
        }

      for (band = 0; band < 2; band++)
        {
          nyadsp_make_crossover(&dsp->crossover[channel][band],
                                config->crossover_hz, channel < 2);
        }

      for (band = 0; band < NYADSP_EQ_BANDS; band++)
        {
          nyadsp_make_peak(&dsp->output_eq[channel][band],
                           &config->output[channel].eq[band]);
          if (channel < 2)
            {
              nyadsp_make_peak(&dsp->input_eq[channel][band],
                               &config->eq[band]);
            }
        }
    }

  if (error != NULL)
    {
      *error = NYADSP_OK;
    }

  return dsp;
}

/****************************************************************************
 * Name: nyadsp_reset
 ****************************************************************************/

void nyadsp_reset(struct nyadsp_s *dsp)
{
  size_t channel;

  if (dsp == NULL)
    {
      return;
    }

  memset(&dsp->stats, 0, sizeof(dsp->stats));
  dsp->limiter_gain = 1.0f;
  for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
    {
      nyadsp_clear_filters(dsp->crossover[channel], 2);
      nyadsp_clear_filters(dsp->output_eq[channel], NYADSP_EQ_BANDS);
      if (channel < 2)
        {
          nyadsp_clear_filters(dsp->input_eq[channel], NYADSP_EQ_BANDS);
        }

      if (dsp->delay[channel] != NULL)
        {
          memset(dsp->delay[channel], 0,
                 dsp->config.output[channel].delay_frames * sizeof(float));
        }

      dsp->cursor[channel] = 0;
    }
}

/****************************************************************************
 * Name: nyadsp_destroy
 ****************************************************************************/

void nyadsp_destroy(struct nyadsp_s *dsp)
{
  size_t channel;

  if (dsp == NULL)
    {
      return;
    }

  for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
    {
      free(dsp->delay[channel]);
    }

  free(dsp);
}

/****************************************************************************
 * Name: nyadsp_process
 ****************************************************************************/

int nyadsp_process(struct nyadsp_s *dsp, const float *input, float *output,
                   size_t frames)
{
  size_t i;
  size_t channel;
  uintptr_t inaddr = (uintptr_t)input;
  uintptr_t outaddr = (uintptr_t)output;
  size_t insize;
  size_t outsize;

  if (dsp == NULL || frames > NYADSP_MAX_BLOCK_FRAMES)
    {
      return NYADSP_EINVAL;
    }

  if (frames == 0)
    {
      return NYADSP_OK;
    }

  insize = frames * 2 * sizeof(float);
  outsize = frames * NYADSP_OUTPUTS * sizeof(float);
  if (input == NULL || output == NULL || inaddr > UINTPTR_MAX - insize ||
      outaddr > UINTPTR_MAX - outsize ||
      (inaddr < outaddr + outsize && outaddr < inaddr + insize))
    {
      return NYADSP_EINVAL;
    }

  /* Preflight the entire block before mutating state or caller output. */

  for (i = 0; i < frames * 2; i++)
    {
      if (!nyadsp_range(input[i], -1.0f, 1.0f))
        {
          return NYADSP_EINVAL;
        }
    }

  for (i = 0; i < frames; i++)
    {
      float left = input[2 * i] * dsp->master_gain;
      float right = input[2 * i + 1] * dsp->master_gain;
      float values[NYADSP_OUTPUTS];
      float peak = 0.0f;
      float desired;

      dsp->stats.input_peak =
          fmaxf(dsp->stats.input_peak,
                fmaxf(fabsf(input[2 * i]), fabsf(input[2 * i + 1])));

      if (dsp->config.bypass)
        {
          values[0] = left;
          values[1] = right;
          values[2] = 0.0f;
        }
      else
        {
          left = nyadsp_equalize(dsp->input_eq[0], left);
          right = nyadsp_equalize(dsp->input_eq[1], right);
          values[0] = left;
          values[1] = right;
          values[2] = 0.5f * (left + right);

          for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
            {
              float sample = values[channel];

              sample = nyadsp_filter(&dsp->crossover[channel][0], sample);
              sample = nyadsp_filter(&dsp->crossover[channel][1], sample);
              sample = nyadsp_equalize(dsp->output_eq[channel], sample);
              sample *= dsp->gain[channel];

              if (dsp->delay[channel] != NULL)
                {
                  uint32_t cursor = dsp->cursor[channel];
                  float delayed = dsp->delay[channel][cursor];

                  dsp->delay[channel][cursor] = sample;
                  dsp->cursor[channel] =
                      (cursor + 1) % dsp->config.output[channel].delay_frames;
                  sample = delayed;
                }

              values[channel] = sample;
            }
        }

      for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
        {
          peak = fmaxf(peak, fabsf(values[channel]));
        }

      desired = peak > dsp->config.ceiling ? dsp->config.ceiling / peak : 1.0f;
      if (desired < dsp->limiter_gain)
        {
          dsp->limiter_gain = desired;
        }
      else
        {
          dsp->limiter_gain += dsp->release * (desired - dsp->limiter_gain);
        }

      if (dsp->limiter_gain < 0.99999f)
        {
          dsp->stats.limited_frames++;
        }

      for (channel = 0; channel < NYADSP_OUTPUTS; channel++)
        {
          float sample = values[channel] * dsp->limiter_gain;

          /* Guard rounding at the ceiling, not a substitute for limiting. */

          sample =
              fmaxf(-dsp->config.ceiling, fminf(dsp->config.ceiling, sample));
          output[NYADSP_OUTPUTS * i + channel] = sample;
          dsp->stats.output_peak =
              fmaxf(dsp->stats.output_peak, fabsf(sample));
        }
    }

  dsp->stats.processed_frames += frames;
  return NYADSP_OK;
}

/****************************************************************************
 * Name: nyadsp_get_stats
 ****************************************************************************/

int nyadsp_get_stats(const struct nyadsp_s *dsp, struct nyadsp_stats_s *stats)
{
  if (dsp == NULL || stats == NULL)
    {
      return NYADSP_EINVAL;
    }

  *stats = dsp->stats;
  return NYADSP_OK;
}
