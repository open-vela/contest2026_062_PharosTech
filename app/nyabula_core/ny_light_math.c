/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_light_math.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_light_math.h"

#include <math.h>
#include <string.h>

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static float ny_light_clamp(float value, float low, float high);
static void ny_light_track(struct ny_light_calibration_s *cal, float value,
                           float seconds);
static void ny_light_observe(struct ny_light_calibration_s *cal, float value,
                             float seconds, enum ny_light_period_e period);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_light_clamp
 *
 * Description:
 *   A NaN comes out as `low`: both comparisons are false for it, and nothing
 *   downstream is prepared for one.
 *
 ****************************************************************************/

static float ny_light_clamp(float value, float low, float high)
{
  if (!(value > low))
    {
      return low;
    }

  return value < high ? value : high;
}

/****************************************************************************
 * Name: ny_light_track
 *
 * Description:
 *   Learn the range.  A new extreme is taken at once: the smoothing already
 *   removed the spikes, and a pupil that ignores the brightest light it ever
 *   saw until some average catches up would look broken.  Coming back is
 *   slow, and stops at the narrowest span that is still believed.
 *
 ****************************************************************************/

static void ny_light_track(struct ny_light_calibration_s *cal, float value,
                           float seconds)
{
  float weight;
  float low;
  float high;

  if (cal->pinned)
    {
      return;
    }

  if (!cal->ranged)
    {
      cal->min = value;
      cal->max = value;
      cal->ranged = true;
      return;
    }

  if (value < cal->min)
    {
      cal->min = value;
    }

  if (value > cal->max)
    {
      cal->max = value;
    }

  weight = ny_light_clamp(seconds / NY_LIGHT_RANGE_TAU, 0.0f, 1.0f);
  low = cal->min + (value - cal->min) * weight;
  high = cal->max + (value - cal->max) * weight;
  if (high - low >= NY_LIGHT_MIN_SPAN)
    {
      cal->min = low;
      cal->max = high;
    }
}

/****************************************************************************
 * Name: ny_light_observe
 *
 * Description:
 *   Which way the reading moves with light cannot be seen in the reading.
 *   What can be known is that around noon there is more light than in the
 *   small hours, in nearly every room a device stands in.  Keep a fading
 *   mean of each and, once both hold enough and lie clearly apart, take the
 *   sign of the difference.  An unclear difference changes nothing, so a
 *   decision made once survives a week of drawn curtains.
 *
 ****************************************************************************/

static void ny_light_observe(struct ny_light_calibration_s *cal, float value,
                             float seconds, enum ny_light_period_e period)
{
  float *mean;
  float *total;
  float need;
  float diff;
  float low;
  float high;

  if (period == NY_LIGHT_PERIOD_NONE || !(seconds > 0.0f))
    {
      return;
    }

  mean = period == NY_LIGHT_PERIOD_DAY ? &cal->day_mean : &cal->night_mean;
  total =
      period == NY_LIGHT_PERIOD_DAY ? &cal->day_seconds : &cal->night_seconds;

  *total = ny_light_clamp(*total + seconds, 0.0f, NY_LIGHT_EVIDENCE_CAP);
  *mean += (value - *mean) * ny_light_clamp(seconds / *total, 0.0f, 1.0f);

  if (cal->day_seconds < NY_LIGHT_EVIDENCE_MIN ||
      cal->night_seconds < NY_LIGHT_EVIDENCE_MIN)
    {
      return;
    }

  ny_light_span(cal, &low, &high);
  need = (high - low) * 0.25f;
  if (need < NY_LIGHT_MIN_SPAN * 0.5f)
    {
      need = NY_LIGHT_MIN_SPAN * 0.5f;
    }

  diff = cal->day_mean - cal->night_mean;
  if (diff >= need)
    {
      cal->detected = NY_LIGHT_POLARITY_HIGH;
    }
  else if (diff <= -need)
    {
      cal->detected = NY_LIGHT_POLARITY_LOW;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_light_median3
 ****************************************************************************/

uint16_t ny_light_median3(uint16_t a, uint16_t b, uint16_t c)
{
  uint16_t low = a < b ? a : b;
  uint16_t high = a < b ? b : a;

  return c < low ? low : c > high ? high : c;
}

/****************************************************************************
 * Name: ny_light_init
 ****************************************************************************/

void ny_light_init(struct ny_light_filter_s *filter)
{
  memset(filter, 0, sizeof(*filter));
  filter->cal.gamma = NY_LIGHT_GAMMA_DEFAULT;
}

/****************************************************************************
 * Name: ny_light_reset
 ****************************************************************************/

void ny_light_reset(struct ny_light_filter_s *filter)
{
  enum ny_light_polarity_e polarity = filter->cal.polarity;
  float gamma = filter->cal.gamma;

  ny_light_init(filter);
  filter->cal.polarity = polarity;
  filter->cal.gamma = gamma;
}

/****************************************************************************
 * Name: ny_light_sample
 ****************************************************************************/

void ny_light_sample(struct ny_light_filter_s *filter, uint16_t raw,
                     float seconds, enum ny_light_period_e period)
{
  float target;

  if (raw > NY_LIGHT_RAW_MAX)
    {
      raw = NY_LIGHT_RAW_MAX;
    }

  if (!(seconds > 0.0f))
    {
      seconds = 0.0f;
    }

  filter->raw = raw;
  if (!filter->primed)
    {
      filter->smoothed = (float)raw;
      filter->primed = true;
    }
  else
    {
      /* Written with the elapsed time so that a late tick, or sampling that
       * was switched off for an hour, weighs the new reading as it should.
       */

      filter->smoothed += ((float)raw - filter->smoothed) *
                          (1.0f - expf(-seconds / NY_LIGHT_SMOOTH_TAU));
    }

  ny_light_track(&filter->cal, filter->smoothed, seconds);
  ny_light_observe(&filter->cal, filter->smoothed, seconds, period);

  filter->level = ny_light_level(&filter->cal, filter->smoothed);
  target = ny_light_dilation(filter->level, filter->cal.gamma);
  if (!filter->settled)
    {
      filter->dilation = target;
      filter->settled = true;
    }
  else
    {
      filter->dilation =
          ny_light_backlash(filter->dilation, target, NY_LIGHT_HYSTERESIS);
    }
}

/****************************************************************************
 * Name: ny_light_resolved
 ****************************************************************************/

enum ny_light_polarity_e
ny_light_resolved(const struct ny_light_calibration_s *cal)
{
  if (cal->polarity != NY_LIGHT_POLARITY_AUTO)
    {
      return cal->polarity;
    }

  return cal->detected == NY_LIGHT_POLARITY_LOW ? NY_LIGHT_POLARITY_LOW
                                                : NY_LIGHT_POLARITY_HIGH;
}

/****************************************************************************
 * Name: ny_light_span
 ****************************************************************************/

void ny_light_span(const struct ny_light_calibration_s *cal, float *low,
                   float *high)
{
  float from = cal->ranged ? cal->min : 0.0f;
  float to = cal->ranged ? cal->max : (float)NY_LIGHT_RAW_MAX;

  /* A range the owner pinned is the owner's: a circuit that only swings a
   * hundred counts needs exactly that, and only the owner can know it.
   */

  if (!cal->pinned && to - from < NY_LIGHT_MIN_SPAN)
    {
      float middle = (from + to) * 0.5f;

      from = middle - NY_LIGHT_MIN_SPAN * 0.5f;
      to = middle + NY_LIGHT_MIN_SPAN * 0.5f;
      if (from < 0.0f)
        {
          to -= from;
          from = 0.0f;
        }

      if (to > (float)NY_LIGHT_RAW_MAX)
        {
          from -= to - (float)NY_LIGHT_RAW_MAX;
          to = (float)NY_LIGHT_RAW_MAX;
        }
    }

  *low = from;
  *high = to;
}

/****************************************************************************
 * Name: ny_light_level
 ****************************************************************************/

float ny_light_level(const struct ny_light_calibration_s *cal, float value)
{
  float low;
  float high;
  float level;

  ny_light_span(cal, &low, &high);
  if (!(high - low > 0.0f))
    {
      return 0.5f;
    }

  level = ny_light_clamp((value - low) / (high - low), 0.0f, 1.0f);
  return ny_light_resolved(cal) == NY_LIGHT_POLARITY_LOW ? 1.0f - level
                                                         : level;
}

/****************************************************************************
 * Name: ny_light_dilation
 ****************************************************************************/

float ny_light_dilation(float level, float gamma)
{
  level = ny_light_clamp(level, 0.0f, 1.0f);
  gamma = ny_light_clamp(gamma, NY_LIGHT_GAMMA_MIN, NY_LIGHT_GAMMA_MAX);
  return ny_light_clamp(1.0f - powf(level, gamma), 0.0f, 1.0f);
}

/****************************************************************************
 * Name: ny_light_backlash
 *
 * Description:
 *   The output is dragged along once the input is more than `band` away and
 *   otherwise stays where it is.  The two ends are exempt, or a pupil could
 *   never quite reach fully open or fully shut.
 *
 ****************************************************************************/

float ny_light_backlash(float output, float input, float band)
{
  input = ny_light_clamp(input, 0.0f, 1.0f);
  if (input <= 0.0f || input >= 1.0f)
    {
      return input;
    }

  return ny_light_clamp(output, input - band, input + band);
}

/****************************************************************************
 * Name: ny_light_period
 ****************************************************************************/

enum ny_light_period_e ny_light_period(int minute_of_day)
{
  if (minute_of_day >= NY_LIGHT_DAY_FROM && minute_of_day < NY_LIGHT_DAY_TO)
    {
      return NY_LIGHT_PERIOD_DAY;
    }

  if (minute_of_day >= NY_LIGHT_NIGHT_FROM &&
      minute_of_day < NY_LIGHT_NIGHT_TO)
    {
      return NY_LIGHT_PERIOD_NIGHT;
    }

  return NY_LIGHT_PERIOD_NONE;
}

/****************************************************************************
 * Name: ny_light_push_due
 ****************************************************************************/

bool ny_light_push_due(float dilation, float pushed, uint64_t elapsed_ms)
{
  float step = fabsf(dilation - pushed);

  if (elapsed_ms < NY_LIGHT_PUSH_INTERVAL_MS)
    {
      return false;
    }

  /* The last bit of the way to either end is less than a step and still
   * the difference between a pupil that is round and one that nearly is.
   */

  return step >= NY_LIGHT_PUSH_STEP ||
         (step > 0.0f && (dilation <= 0.0f || dilation >= 1.0f));
}

/****************************************************************************
 * Name: ny_light_saturated
 ****************************************************************************/

int ny_light_saturated(uint16_t raw)
{
  return raw <= NY_LIGHT_RAW_RAIL                      ? -1
         : raw >= NY_LIGHT_RAW_MAX - NY_LIGHT_RAW_RAIL ? 1
                                                       : 0;
}
