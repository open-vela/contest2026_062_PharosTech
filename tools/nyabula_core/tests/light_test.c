/****************************************************************************
 * tools/nyabula_core/tests/light_test.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the pure half of the ambient light sensor, ny_light_math.c:
 * median, smoothing, the learnt range, which way the reading moves with
 * light, level to pupil dilation, hysteresis and when the eyes are told.
 * Built by light_test.py; not part of the firmware.
 *
 ****************************************************************************/

#include "ny_light_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(list) (sizeof(list) / sizeof((list)[0]))

#define CHECK(condition, ...)                                   \
  do                                                            \
    {                                                           \
      g_checks++;                                               \
      if (!(condition))                                         \
        {                                                       \
          g_failures++;                                         \
          fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);       \
          fprintf(stderr, __VA_ARGS__);                         \
          fprintf(stderr, "\n");                                \
        }                                                       \
    }                                                           \
  while (0)

#define NEAR(a, b, tolerance) (fabs((double)(a) - (double)(b)) <= (tolerance))

/* The sensor task's cadence. */

#define STEP 0.2f

static unsigned long g_checks;
static unsigned long g_failures;
static uint32_t g_seed = 0x4e796162u;

static uint32_t test_random(void)
{
  g_seed = g_seed * 1664525u + 1013904223u;
  return g_seed >> 8;
}

/* `value` with up to +-`noise` counts of noise, inside the converter. */

static uint16_t test_noisy(int value, int noise)
{
  int reading = value + (int)(test_random() % (uint32_t)(2 * noise + 1)) -
                noise;

  return (uint16_t)(reading < 0 ? 0
                    : reading > NY_LIGHT_RAW_MAX ? NY_LIGHT_RAW_MAX
                                                 : reading);
}

static void test_feed(struct ny_light_filter_s *filter, int value, int noise,
                      float seconds, enum ny_light_period_e period)
{
  int samples = (int)(seconds / STEP + 0.5f);
  int index;

  for (index = 0; index < samples; index++)
    {
      ny_light_sample(filter,
                      ny_light_median3(test_noisy(value, noise),
                                       test_noisy(value, noise),
                                       test_noisy(value, noise)),
                      STEP, period);
    }
}

/****************************************************************************
 * Median
 ****************************************************************************/

static void test_median(void)
{
  static const uint16_t values[] = { 0, 1, 7, 7, 2048, 4095 };
  size_t a;
  size_t b;
  size_t c;

  for (a = 0; a < COUNT(values); a++)
    {
      for (b = 0; b < COUNT(values); b++)
        {
          for (c = 0; c < COUNT(values); c++)
            {
              uint16_t x = values[a];
              uint16_t y = values[b];
              uint16_t z = values[c];
              uint16_t low = x < y ? (x < z ? x : z) : (y < z ? y : z);
              uint16_t high = x > y ? (x > z ? x : z) : (y > z ? y : z);
              uint16_t middle = (uint16_t)(x + y + z - low - high);

              CHECK(ny_light_median3(x, y, z) == middle,
                    "median(%u, %u, %u) = %u", x, y, z,
                    ny_light_median3(x, y, z));
            }
        }
    }

  /* What it is for: one wild conversion in a burst changes nothing. */

  CHECK(ny_light_median3(1000, 4095, 1002) == 1002, "spike high");
  CHECK(ny_light_median3(0, 1000, 1002) == 1000, "spike low");
}

/****************************************************************************
 * Smoothing
 ****************************************************************************/

static void test_smoothing(void)
{
  struct ny_light_filter_s one;
  struct ny_light_filter_s many;
  int index;

  ny_light_init(&one);
  ny_light_sample(&one, 1000, 0.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(one.primed && NEAR(one.smoothed, 1000, 0.001),
        "the first reading is taken as it is: %f", (double)one.smoothed);

  /* One time constant after a step: 1 - 1/e of the way. */

  many = one;
  for (index = 0; index < 3; index++)
    {
      ny_light_sample(&many, 2000, STEP, NY_LIGHT_PERIOD_NONE);
    }

  CHECK(NEAR(many.smoothed, 1000 + 1000 * (1 - exp(-1.0)), 0.5),
        "step response after one tau: %f", (double)many.smoothed);

  /* The same time in one late tick weighs the same. */

  ny_light_sample(&one, 2000, 3 * STEP, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(one.smoothed, many.smoothed, 0.5), "late tick: %f against %f",
        (double)one.smoothed, (double)many.smoothed);

  /* Sampling that was off for an hour starts from the new reading. */

  ny_light_sample(&one, 300, 3600.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(one.smoothed, 300, 0.5), "after a long gap: %f",
        (double)one.smoothed);

  /* A time that runs backwards, or is no number, moves nothing. */

  ny_light_sample(&one, 4000, -1.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(one.smoothed, 300, 0.5), "negative time: %f",
        (double)one.smoothed);
  ny_light_sample(&one, 4000, (float)NAN, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(one.smoothed, 300, 0.5), "NaN time: %f", (double)one.smoothed);

  /* Noise comes out smaller than it went in. */

  ny_light_init(&many);
  test_feed(&many, 2000, 40, 20.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(many.smoothed, 2000, 20), "noise of 40 left %f",
        (double)many.smoothed);

  /* A reading the converter cannot produce is clipped, not believed. */

  ny_light_init(&many);
  ny_light_sample(&many, 60000, 0.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(many.raw == NY_LIGHT_RAW_MAX && NEAR(many.smoothed, 4095, 0.001),
        "clipped to %u", many.raw);
}

/****************************************************************************
 * The learnt range
 ****************************************************************************/

static void test_range(void)
{
  struct ny_light_filter_s filter;
  float low;
  float high;
  float before;

  /* Nothing seen: the whole converter. */

  ny_light_init(&filter);
  ny_light_span(&filter.cal, &low, &high);
  CHECK(NEAR(low, 0, 0.001) && NEAR(high, NY_LIGHT_RAW_MAX, 0.001),
        "unlearnt span %f..%f", (double)low, (double)high);

  /* One room seen: a span of the minimum width around it, the reading in
   * the middle of it, so neither a slit nor a saucer on the first boot.
   */

  test_feed(&filter, 1500, 0, 2.0f, NY_LIGHT_PERIOD_NONE);
  ny_light_span(&filter.cal, &low, &high);
  CHECK(filter.cal.ranged && NEAR(high - low, NY_LIGHT_MIN_SPAN, 0.01) &&
            NEAR((low + high) / 2, 1500, 0.01),
        "first span %f..%f", (double)low, (double)high);
  CHECK(NEAR(filter.level, 0.5, 0.01), "first level %f",
        (double)filter.level);

  /* At either end of the converter the span is shifted, not cut. */

  ny_light_init(&filter);
  test_feed(&filter, 0, 0, 1.0f, NY_LIGHT_PERIOD_NONE);
  ny_light_span(&filter.cal, &low, &high);
  CHECK(NEAR(low, 0, 0.01) && NEAR(high, NY_LIGHT_MIN_SPAN, 0.01),
        "span at the bottom %f..%f", (double)low, (double)high);
  ny_light_init(&filter);
  test_feed(&filter, NY_LIGHT_RAW_MAX, 0, 1.0f, NY_LIGHT_PERIOD_NONE);
  ny_light_span(&filter.cal, &low, &high);
  CHECK(NEAR(high, NY_LIGHT_RAW_MAX, 0.01) &&
            NEAR(high - low, NY_LIGHT_MIN_SPAN, 0.01),
        "span at the top %f..%f", (double)low, (double)high);

  /* A new extreme is taken as soon as the smoothing shows it. */

  ny_light_init(&filter);
  test_feed(&filter, 400, 3, 10.0f, NY_LIGHT_PERIOD_NONE);
  test_feed(&filter, 3200, 3, 10.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(filter.cal.min < 410 && filter.cal.max > 3190,
        "learnt %f..%f", (double)filter.cal.min, (double)filter.cal.max);
  CHECK(filter.level > 0.99f, "brightest seen reads %f",
        (double)filter.level);
  test_feed(&filter, 400, 3, 10.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(filter.level < 0.01f, "darkest seen reads %f", (double)filter.level);

  /* An hour in one light barely moves it; days do, and never below the
   * narrowest span that is believed.
   */

  before = filter.cal.max;
  test_feed(&filter, 400, 0, 3600.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(filter.cal.max < before && filter.cal.max > before - 300,
        "an hour moved the maximum from %f to %f", (double)before,
        (double)filter.cal.max);
  for (before = 0; before < 24 * 30; before += 1)
    {
      ny_light_sample(&filter, 400, 3600.0f, NY_LIGHT_PERIOD_NONE);
    }

  CHECK(filter.cal.max - filter.cal.min >= NY_LIGHT_MIN_SPAN - 0.01f &&
            filter.cal.max - filter.cal.min < NY_LIGHT_MIN_SPAN * 1.2f,
        "a month in the dark left %f..%f", (double)filter.cal.min,
        (double)filter.cal.max);
  CHECK(filter.cal.min <= filter.smoothed + 0.01f &&
            filter.smoothed <= filter.cal.max + 0.01f,
        "the reading left its own range");

  /* Light coming back is answered at once all the same. */

  test_feed(&filter, 3200, 3, 10.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(filter.cal.max > 3190 && filter.level > 0.99f,
        "after the month: max %f level %f", (double)filter.cal.max,
        (double)filter.level);

  /* A pinned range is the owner's: not tracked, not widened. */

  ny_light_init(&filter);
  filter.cal.ranged = true;
  filter.cal.pinned = true;
  filter.cal.min = 1000;
  filter.cal.max = 1100;
  test_feed(&filter, 1050, 0, 5.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(filter.level, 0.5, 0.01), "pinned middle %f",
        (double)filter.level);
  test_feed(&filter, 3000, 0, 3600.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(filter.cal.min, 1000, 0.001) && NEAR(filter.cal.max, 1100, 0.001),
        "pinned range moved to %f..%f", (double)filter.cal.min,
        (double)filter.cal.max);
  CHECK(NEAR(filter.level, 1.0, 0.001), "above a pinned range %f",
        (double)filter.level);

  /* A reset forgets the learning and the pin, not the owner's choices. */

  filter.cal.polarity = NY_LIGHT_POLARITY_LOW;
  filter.cal.gamma = 1.5f;
  filter.cal.detected = NY_LIGHT_POLARITY_HIGH;
  filter.cal.day_seconds = 5000;
  ny_light_reset(&filter);
  CHECK(!filter.cal.ranged && !filter.cal.pinned && !filter.primed &&
            filter.cal.detected == NY_LIGHT_POLARITY_AUTO &&
            NEAR(filter.cal.day_seconds, 0, 0.001),
        "reset left learning behind");
  CHECK(filter.cal.polarity == NY_LIGHT_POLARITY_LOW &&
            NEAR(filter.cal.gamma, 1.5, 0.001),
        "reset took the owner's choices");
}

/****************************************************************************
 * Which way the reading moves
 ****************************************************************************/

static void test_polarity(void)
{
  struct ny_light_filter_s filter;
  int minute;

  /* The hours that count. */

  for (minute = 0; minute < 1440; minute++)
    {
      enum ny_light_period_e want =
          minute >= 600 && minute < 960   ? NY_LIGHT_PERIOD_DAY
          : minute >= 60 && minute < 300 ? NY_LIGHT_PERIOD_NIGHT
                                         : NY_LIGHT_PERIOD_NONE;
      if (ny_light_period(minute) != want)
        {
          CHECK(false, "minute %d is period %d", minute,
                (int)ny_light_period(minute));
          break;
        }
    }

  CHECK(ny_light_period(-5) == NY_LIGHT_PERIOD_NONE &&
            ny_light_period(5000) == NY_LIGHT_PERIOD_NONE,
        "minutes outside the day");

  /* Undecided reads as "higher is brighter", and says it is undecided. */

  ny_light_init(&filter);
  CHECK(ny_light_resolved(&filter.cal) == NY_LIGHT_POLARITY_HIGH &&
            filter.cal.detected == NY_LIGHT_POLARITY_AUTO,
        "the default");

  /* A circuit that reads high in the dark: noon low, night high. */

  test_feed(&filter, 600, 10, 1300.0f, NY_LIGHT_PERIOD_DAY);
  CHECK(filter.cal.detected == NY_LIGHT_POLARITY_AUTO,
        "decided on daylight alone");
  test_feed(&filter, 3500, 10, 1100.0f, NY_LIGHT_PERIOD_NIGHT);
  CHECK(filter.cal.detected == NY_LIGHT_POLARITY_AUTO,
        "decided on %f s of night", (double)filter.cal.night_seconds);
  test_feed(&filter, 3500, 10, 200.0f, NY_LIGHT_PERIOD_NIGHT);
  CHECK(filter.cal.detected == NY_LIGHT_POLARITY_LOW &&
            ny_light_resolved(&filter.cal) == NY_LIGHT_POLARITY_LOW,
        "night high, noon low: detected %d", (int)filter.cal.detected);
  CHECK(NEAR(filter.cal.day_mean, 600, 15) &&
            NEAR(filter.cal.night_mean, 3500, 15),
        "means %f and %f", (double)filter.cal.day_mean,
        (double)filter.cal.night_mean);

  /* ... and then the dark is dark: high reading, low level, wide pupil. */

  CHECK(filter.level < 0.02f && filter.dilation > 0.85f,
        "inverted dark: level %f dilation %f", (double)filter.level,
        (double)filter.dilation);
  test_feed(&filter, 600, 10, 10.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(filter.level > 0.98f && filter.dilation < 0.03f,
        "inverted bright: level %f dilation %f", (double)filter.level,
        (double)filter.dilation);

  /* Hours that are neither teach nothing. */

  test_feed(&filter, 3500, 10, 5000.0f, NY_LIGHT_PERIOD_NONE);
  CHECK(NEAR(filter.cal.day_mean, 600, 15), "evening moved the day mean");

  /* A week of drawn curtains makes day and night alike: no new verdict,
   * and the old one stands.
   */

  test_feed(&filter, 3400, 10, NY_LIGHT_EVIDENCE_CAP * 3,
            NY_LIGHT_PERIOD_DAY);
  CHECK(NEAR(filter.cal.day_mean, 3400, 60), "day mean did not fade: %f",
        (double)filter.cal.day_mean);
  CHECK(filter.cal.detected == NY_LIGHT_POLARITY_LOW,
        "an unclear difference overturned the verdict");
  CHECK(filter.cal.day_seconds <= NY_LIGHT_EVIDENCE_CAP,
        "evidence grew without bound: %f", (double)filter.cal.day_seconds);

  /* Moved to a circuit the other way round, it comes round too. */

  test_feed(&filter, 3400, 10, NY_LIGHT_EVIDENCE_CAP * 3,
            NY_LIGHT_PERIOD_DAY);
  test_feed(&filter, 500, 10, NY_LIGHT_EVIDENCE_CAP * 3,
            NY_LIGHT_PERIOD_NIGHT);
  CHECK(filter.cal.detected == NY_LIGHT_POLARITY_HIGH, "did not come round");

  /* The owner overrules the evidence, both ways. */

  filter.cal.polarity = NY_LIGHT_POLARITY_LOW;
  CHECK(ny_light_resolved(&filter.cal) == NY_LIGHT_POLARITY_LOW, "owner low");
  CHECK(ny_light_level(&filter.cal, filter.cal.max) < 0.001f,
        "owner low: the maximum is dark");
  filter.cal.polarity = NY_LIGHT_POLARITY_HIGH;
  filter.cal.detected = NY_LIGHT_POLARITY_LOW;
  CHECK(ny_light_resolved(&filter.cal) == NY_LIGHT_POLARITY_HIGH,
        "owner high");
  CHECK(ny_light_level(&filter.cal, filter.cal.max) > 0.999f,
        "owner high: the maximum is bright");
}

/****************************************************************************
 * Level to dilation
 ****************************************************************************/

static void test_dilation(void)
{
  float previous = 2.0f;
  int index;

  CHECK(NEAR(ny_light_dilation(0.0f, NY_LIGHT_GAMMA_DEFAULT), 1.0, 1e-6),
        "dark is wide open");
  CHECK(NEAR(ny_light_dilation(1.0f, NY_LIGHT_GAMMA_DEFAULT), 0.0, 1e-6),
        "bright is a slit");

  /* The point of the gamma: a lit room is a few percent of what a window
   * at noon gives, and must not look like the dark.
   */

  CHECK(NEAR(ny_light_dilation(0.25f, NY_LIGHT_GAMMA_DEFAULT), 0.5, 1e-4),
        "a quarter of the range is half open: %f",
        (double)ny_light_dilation(0.25f, NY_LIGHT_GAMMA_DEFAULT));
  CHECK(ny_light_dilation(0.03f, NY_LIGHT_GAMMA_DEFAULT) < 0.85f &&
            ny_light_dilation(0.03f, 1.0f) > 0.95f,
        "a lit room: %f with the gamma, %f without",
        (double)ny_light_dilation(0.03f, NY_LIGHT_GAMMA_DEFAULT),
        (double)ny_light_dilation(0.03f, 1.0f));

  for (index = 0; index <= 1000; index++)
    {
      float value =
          ny_light_dilation((float)index / 1000.0f, NY_LIGHT_GAMMA_DEFAULT);
      if (!(value < previous) || value < 0.0f || value > 1.0f)
        {
          CHECK(false, "not falling at level %d: %f after %f", index,
                (double)value, (double)previous);
          break;
        }

      previous = value;
    }

  /* Out of range input is clamped, not propagated. */

  CHECK(NEAR(ny_light_dilation(-3.0f, 0.5f), 1.0, 1e-6) &&
            NEAR(ny_light_dilation(7.0f, 0.5f), 0.0, 1e-6),
        "levels outside 0..1");
  CHECK(NEAR(ny_light_dilation((float)NAN, 0.5f), 1.0, 1e-6), "NaN level");
  CHECK(NEAR(ny_light_dilation(0.5f, 0.0f),
             ny_light_dilation(0.5f, NY_LIGHT_GAMMA_MIN), 1e-6) &&
            NEAR(ny_light_dilation(0.5f, 99.0f),
                 ny_light_dilation(0.5f, NY_LIGHT_GAMMA_MAX), 1e-6) &&
            NEAR(ny_light_dilation(0.5f, (float)NAN),
                 ny_light_dilation(0.5f, NY_LIGHT_GAMMA_MIN), 1e-6),
        "gamma outside its limits");
}

/****************************************************************************
 * Hysteresis and telling the eyes
 ****************************************************************************/

static void test_hysteresis(void)
{
  struct ny_light_filter_s filter;
  float output = 0.5f;
  float held;
  float pushed;
  uint64_t now;
  uint64_t pushed_at;
  int pushes;
  int index;

  /* Inside the band nothing moves; outside it the output trails by it. */

  output = ny_light_backlash(output, 0.52f, NY_LIGHT_HYSTERESIS);
  CHECK(NEAR(output, 0.5, 1e-6), "moved inside the band: %f", (double)output);
  output = ny_light_backlash(output, 0.60f, NY_LIGHT_HYSTERESIS);
  CHECK(NEAR(output, 0.57, 1e-6), "trailing up: %f", (double)output);
  output = ny_light_backlash(output, 0.58f, NY_LIGHT_HYSTERESIS);
  CHECK(NEAR(output, 0.57, 1e-6), "a small way back moved it: %f",
        (double)output);
  output = ny_light_backlash(output, 0.40f, NY_LIGHT_HYSTERESIS);
  CHECK(NEAR(output, 0.43, 1e-6), "trailing down: %f", (double)output);

  /* Both ends can be reached. */

  CHECK(NEAR(ny_light_backlash(0.5f, 1.0f, NY_LIGHT_HYSTERESIS), 1.0, 1e-6) &&
            NEAR(ny_light_backlash(0.5f, 0.0f, NY_LIGHT_HYSTERESIS), 0, 1e-6) &&
            NEAR(ny_light_backlash(0.5f, 4.0f, NY_LIGHT_HYSTERESIS), 1.0, 1e-6),
        "the ends");

  /* When the eyes are told. */

  CHECK(!ny_light_push_due(0.5f, 0.2f, NY_LIGHT_PUSH_INTERVAL_MS - 1),
        "pushed inside the rate limit");
  CHECK(ny_light_push_due(0.5f, 0.2f, NY_LIGHT_PUSH_INTERVAL_MS),
        "a visible step was not pushed");
  CHECK(!ny_light_push_due(0.5f, 0.5f + NY_LIGHT_PUSH_STEP * 0.9f, 60000),
        "an invisible step was pushed");
  CHECK(ny_light_push_due(1.0f, 0.99f, 60000) &&
            ny_light_push_due(0.0f, 0.01f, 60000) &&
            !ny_light_push_due(1.0f, 1.0f, 60000),
        "the last bit of the way to an end");

  /* The whole chain in a room: a sensor with 12 counts of noise in a range
   * of 2800, a lamp switched on and off.  The pupil must hold still while
   * the light does, and move one way only while it changes.
   */

  ny_light_init(&filter);
  test_feed(&filter, 400, 12, 5.0f, NY_LIGHT_PERIOD_NONE);
  test_feed(&filter, 3200, 12, 5.0f, NY_LIGHT_PERIOD_NONE);
  test_feed(&filter, 1200, 12, 10.0f, NY_LIGHT_PERIOD_NONE);

  held = filter.dilation;
  pushed = filter.dilation;
  pushed_at = 0;
  now = 0;
  pushes = 0;
  for (index = 0; index < 5 * 600; index++)
    {
      test_feed(&filter, 1200, 12, STEP, NY_LIGHT_PERIOD_NONE);
      now += 200;
      if (ny_light_push_due(filter.dilation, pushed, now - pushed_at))
        {
          pushed = filter.dilation;
          pushed_at = now;
          pushes++;
        }
    }

  CHECK(pushes == 0, "ten minutes of steady light pushed %d times", pushes);
  CHECK(NEAR(filter.dilation, held, NY_LIGHT_HYSTERESIS),
        "steady light drifted from %f to %f", (double)held,
        (double)filter.dilation);

  /* Without the hysteresis the same noise would have shown. */

  {
    float low = 1.0f;
    float high = 0.0f;
    for (index = 0; index < 5 * 600; index++)
      {
        float target;

        test_feed(&filter, 1200, 12, STEP, NY_LIGHT_PERIOD_NONE);
        target = ny_light_dilation(filter.level, filter.cal.gamma);
        low = target < low ? target : low;
        high = target > high ? target : high;
      }

    CHECK(high - low > 0.0f && high - low < NY_LIGHT_HYSTERESIS,
          "noise on the target spans %f: the band must cover it",
          (double)(high - low));
  }

  /* Lamp on: the pupil narrows, never widens on the way, arrives within
   * three seconds, and the eyes hear of it a handful of times.
   */

  held = filter.dilation;
  pushes = 0;
  for (index = 0; index < 15; index++)
    {
      float before = filter.dilation;

      test_feed(&filter, 3200, 12, STEP, NY_LIGHT_PERIOD_NONE);
      now += 200;
      CHECK(filter.dilation <= before + 1e-6f, "widened while brightening");
      if (ny_light_push_due(filter.dilation, pushed, now - pushed_at))
        {
          pushed = filter.dilation;
          pushed_at = now;
          pushes++;
        }
    }

  CHECK(filter.dilation < 0.05f && held > 0.3f,
        "lamp on: from %f to %f", (double)held, (double)filter.dilation);
  CHECK(pushes >= 2 && pushes <= 8, "lamp on pushed %d times", pushes);
}

/****************************************************************************
 * The rails, and whatever else can be thrown at it
 ****************************************************************************/

static void test_limits(void)
{
  struct ny_light_filter_s filter;
  int round;

  CHECK(ny_light_saturated(0) == -1 && ny_light_saturated(8) == -1 &&
            ny_light_saturated(9) == 0 && ny_light_saturated(2000) == 0 &&
            ny_light_saturated(4086) == 0 && ny_light_saturated(4087) == 1 &&
            ny_light_saturated(4095) == 1,
        "the rails");

  ny_light_init(&filter);
  for (round = 0; round < 200000; round++)
    {
      uint32_t seed = test_random();
      float seconds = (seed & 7) == 0 ? (float)(seed >> 4 & 0xffff)
                                      : (float)(seed >> 4 & 0xff) / 100.0f;

      if ((seed & 0xfff) == 0)
        {
          ny_light_reset(&filter);
        }

      if ((seed & 0x3ff) == 1)
        {
          filter.cal.polarity = (enum ny_light_polarity_e)(seed >> 12 & 3) %
                                3;
        }

      ny_light_sample(&filter, (uint16_t)(test_random() & 0x1fff), seconds,
                      (enum ny_light_period_e)(test_random() % 3));
      if (!(filter.level >= 0.0f && filter.level <= 1.0f) ||
          !(filter.dilation >= 0.0f && filter.dilation <= 1.0f) ||
          !(filter.cal.min <= filter.cal.max) ||
          !(filter.cal.min >= 0.0f && filter.cal.max <= NY_LIGHT_RAW_MAX) ||
          !(filter.smoothed >= 0.0f && filter.smoothed <= NY_LIGHT_RAW_MAX) ||
          !isfinite(filter.cal.day_mean) || !isfinite(filter.cal.night_mean) ||
          ny_light_resolved(&filter.cal) == NY_LIGHT_POLARITY_AUTO)
        {
          CHECK(false, "storm round %d: level %f dilation %f range %f..%f",
                round, (double)filter.level, (double)filter.dilation,
                (double)filter.cal.min, (double)filter.cal.max);
          break;
        }
    }

  CHECK(round == 200000, "storm ended early");
}

int main(void)
{
  test_median();
  test_smoothing();
  test_range();
  test_polarity();
  test_dilation();
  test_hysteresis();
  test_limits();
  printf("light_test: %lu checks, %lu failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
