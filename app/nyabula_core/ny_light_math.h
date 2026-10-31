/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_light_math.h
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

#ifndef __NYABULA_CORE_NY_LIGHT_MATH_H
#define __NYABULA_CORE_NY_LIGHT_MATH_H

/****************************************************************************
 * From a photodiode reading to how wide the cat's pupils are.
 *
 * Everything here is pure: no NuttX header, no configuration symbol, no
 * allocation, no lock, no clock.  That is what lets tools/nyabula_core/tests
 * build the very same file on the host under ASan and UBSan;
 * ny_product_light.c owns the ADC, the store, the topics and the time.
 *
 * Nothing is assumed about the circuit the photodiode sits in: not which way
 * the reading moves with light, not how much of the converter's range it
 * uses.  The range is learnt from what was seen, the direction from whether
 * noon reads higher than the small hours, and the owner can overrule both.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The converter is 12 bits wide. */

#define NY_LIGHT_RAW_MAX 4095

/* A reading this close to either end says the circuit ran out of range, not
 * how much light there is.
 */

#define NY_LIGHT_RAW_RAIL 8

/* Time constant of the smoothing, seconds.  Lamps flicker at twice the mains
 * frequency and a few samples a second alias that into a slow wobble; this
 * is long enough to flatten it and short enough that switching a lamp on is
 * answered within a second.
 */

#define NY_LIGHT_SMOOTH_TAU 0.6f

/* A learnt range narrower than this many counts is mostly noise, and
 * stretching it over the whole pupil would turn that noise into motion.  The
 * range is widened to this around its middle until more has been seen.
 */

#define NY_LIGHT_MIN_SPAN 256.0f

/* The learnt range follows a new extreme at once and creeps back towards
 * the current reading with this time constant, seconds: a torch held to the
 * sensor once must not make every room read as dark for ever.
 */

#define NY_LIGHT_RANGE_TAU 43200.0f

/* dilation = 1 - level ^ gamma.  A photodiode is linear in the light and an
 * eye is not: a lit room is a few percent of daylight and should still look
 * like a lit room.  0.5 puts a quarter of the range at a half open pupil.
 */

#define NY_LIGHT_GAMMA_DEFAULT 0.5f
#define NY_LIGHT_GAMMA_MIN     0.2f
#define NY_LIGHT_GAMMA_MAX     3.0f

/* The dilation only moves once the light moved it this far, and then trails
 * by as much (backlash): noise that stays inside the band moves nothing.
 */

#define NY_LIGHT_HYSTERESIS 0.03f

/* What the eyes are told: a change of at least this much, and not more often
 * than this.  Every push is a state broadcast to every open panel.
 */

#define NY_LIGHT_PUSH_STEP        0.02f
#define NY_LIGHT_PUSH_INTERVAL_MS 400U

/* Local hours taken for daylight and for night, as minutes of the day, and
 * how much of each must have been seen before the two are compared.  The
 * means fade with NY_LIGHT_EVIDENCE_CAP seconds of memory so that a moved
 * device is judged by where it stands now.
 */

#define NY_LIGHT_DAY_FROM     (10 * 60)
#define NY_LIGHT_DAY_TO       (16 * 60)
#define NY_LIGHT_NIGHT_FROM   (1 * 60)
#define NY_LIGHT_NIGHT_TO     (5 * 60)
#define NY_LIGHT_EVIDENCE_MIN 1200.0f
#define NY_LIGHT_EVIDENCE_CAP 21600.0f

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum ny_light_polarity_e
{
  NY_LIGHT_POLARITY_AUTO = 0, /* Not chosen / not decided yet */
  NY_LIGHT_POLARITY_HIGH,     /* More light reads higher */
  NY_LIGHT_POLARITY_LOW       /* More light reads lower */
};

enum ny_light_period_e
{
  NY_LIGHT_PERIOD_NONE = 0, /* Unknown time, or neither of the two */
  NY_LIGHT_PERIOD_DAY,
  NY_LIGHT_PERIOD_NIGHT
};

/* What is kept across a power cycle. */

struct ny_light_calibration_s
{
  enum ny_light_polarity_e polarity; /* The owner's choice */
  enum ny_light_polarity_e detected; /* What day and night said so far */
  bool ranged;                       /* min and max hold something */
  bool pinned;                       /* The owner set them: do not track */
  float min;
  float max;
  float gamma;
  float day_mean;
  float day_seconds;
  float night_mean;
  float night_seconds;
};

struct ny_light_filter_s
{
  struct ny_light_calibration_s cal;
  bool primed;  /* smoothed holds a reading */
  bool settled; /* dilation holds a value */
  uint16_t raw; /* The last median */
  float smoothed;
  float level;    /* 0..1, 1 = bright */
  float dilation; /* 0..1, 1 = wide, after the hysteresis */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

uint16_t ny_light_median3(uint16_t a, uint16_t b, uint16_t c);

void ny_light_init(struct ny_light_filter_s *filter);

/* Forget what was learnt about the range and the direction, and the owner's
 * pin with it.  The owner's polarity and gamma are choices, not learning:
 * they stay.
 */

void ny_light_reset(struct ny_light_filter_s *filter);

/* One median reading, `seconds` after the one before. */

void ny_light_sample(struct ny_light_filter_s *filter, uint16_t raw,
                     float seconds, enum ny_light_period_e period);

/* Which way the reading is taken to move: the owner's choice, else what was
 * detected, else higher for brighter.  Never NY_LIGHT_POLARITY_AUTO.
 */

enum ny_light_polarity_e
ny_light_resolved(const struct ny_light_calibration_s *cal);

/* The range levels are measured against, never narrower than
 * NY_LIGHT_MIN_SPAN and never outside the converter's.
 */

void ny_light_span(const struct ny_light_calibration_s *cal, float *low,
                   float *high);

float ny_light_level(const struct ny_light_calibration_s *cal, float value);
float ny_light_dilation(float level, float gamma);
float ny_light_backlash(float output, float input, float band);

enum ny_light_period_e ny_light_period(int minute_of_day);

/* True when the eyes should be told: `dilation` is a visible step away from
 * what they were told last, `elapsed_ms` ago.
 */

bool ny_light_push_due(float dilation, float pushed, uint64_t elapsed_ms);

/* -1 pegged low, 1 pegged high, 0 inside the converter's range. */

int ny_light_saturated(uint16_t raw);

#endif /* __NYABULA_CORE_NY_LIGHT_MATH_H */
