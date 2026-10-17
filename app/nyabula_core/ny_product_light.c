/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_light.c
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

/* A photodiode on a SARADC channel makes the cat's pupils react to the room:
 * a narrow slit in bright light, wide and round in the dark.
 *
 *   light.status  any role  the reading and everything derived from it
 *   light.config  owner     {"enabled": bool,
 *                            "polarity": "auto"|"bright-high"|"bright-low",
 *                            "min": 0..4095, "max": 0..4095,  pins the range
 *                            "pinned": false,                 learn it again
 *                            "gamma": 0.2..3,
 *                            "utc_offset_minutes": -720..840,
 *                            "reset": true}                   forget learning
 *
 * light.config takes any of its members and answers with the status that
 * results.
 *
 * Nothing is known about the circuit, so nothing is assumed: the arithmetic
 * in ny_light_math.c learns the range and the direction, and the raw reading
 * is always reported so that both can be checked against a covered and a
 * lit sensor.  This file owns the converter, the record in the product
 * store, the topics and the time.
 *
 * The eyes are told through the same eyes.ambient command a panel's slider
 * sends.  The Eye Engine eases the level in its own animation tick, so the
 * few reports a second from here arrive as one motion, and the pupil it
 * computes from the level sits under every expression that draws a pupil
 * of its own.  The command is queued and never waited for: the confirmed
 * eyes.expression path may block a second, this one must not.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_product_light.h"
#include "ny_light_math.h"
#include "ny_product_store.h"

#include <nuttx/config.h>
#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/mutex.h>

#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_PLIGHT_DOMAIN          "light"
#define NY_PLIGHT_SCHEMA          1
#define NY_PLIGHT_DEVICE          CONFIG_NYABULA_CORE_LIGHT_DEVICE
#define NY_PLIGHT_EYE_SOURCE      "light"

/* The worker ticks every 100 ms; this lands on every second tick, which is
 * five readings a second.
 */

#define NY_PLIGHT_SAMPLE_MS       180

/* The ADC nodes are registered by board initialization, which may finish
 * after the product worker starts: ask often at first, then rarely.
 */

#define NY_PLIGHT_OPEN_FAST_MS    2000
#define NY_PLIGHT_OPEN_SLOW_MS    10000
#define NY_PLIGHT_OPEN_FAST       15
#define NY_PLIGHT_STORE_RETRY_MS  5000

/* What was learnt is written when it moved by this much and not more often
 * than this: the range creeps all day, and the store is flash.
 */

#define NY_PLIGHT_SAVE_INTERVAL_MS 600000
#define NY_PLIGHT_SAVE_RANGE       82.0f   /* 2 % of the converter */
#define NY_PLIGHT_SAVE_EVIDENCE    1800.0f /* Seconds of day or night */

/* A panel's slider, or an eye service that restarted, leaves the eyes with
 * a level that is not ours.  Look this often and put it right.
 */

#define NY_PLIGHT_VERIFY_MS       10000
#define NY_PLIGHT_VERIFY_STEP     0.005f

/* The narrowest range the owner may pin, in counts. */

#define NY_PLIGHT_PIN_SPAN        16.0

#define NY_PLIGHT_OFFSET_MIN      (-720)
#define NY_PLIGHT_OFFSET_MAX      840

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_plight_state_s
{
  struct ny_light_filter_s filter;
  struct ny_light_calibration_s stored; /* What the store holds */
  bool enabled;
  bool offset_known;      /* utc_offset holds the owner's time zone */
  int utc_offset;         /* Minutes east of UTC */
  int fd;                 /* The ADC node, or -1 */
  int error;              /* Negated errno of the last failure, 0 when fine */
  const char *failed;     /* What failed: "open", "trigger" or "read" */
  bool loaded;            /* The store was read */
  bool dirty;             /* A choice of the owner differs from the store */
  bool saved;             /* The last write to the store succeeded */
  bool pushed_valid;      /* pushed holds what the eyes were told */
  bool push_now;          /* Tell the eyes whatever the rate limit says */
  float pushed;
  unsigned int attempts;  /* Failed opens so far */
  uint32_t samples;
  uint64_t open_after;    /* Monotonic ms before which not to open */
  uint64_t store_after;   /* Monotonic ms before which not to use the store */
  uint64_t sampled_at;    /* Monotonic ms of the last reading, 0 for none */
  uint64_t pushed_at;
  uint64_t verify_at;
  uint64_t saved_at;
  uint64_t revision;      /* Store revision of the record */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static const char *ny_plight_polarity_name(enum ny_light_polarity_e value,
                                           bool detected);
static int ny_plight_polarity_index(const cJSON *item);
static bool ny_plight_number(const cJSON *item, double low, double high,
                             double *value);
static void ny_plight_close(void);
static void ny_plight_fail(const char *what, int error);
static int ny_plight_open(void);
static int ny_plight_convert(uint16_t *value);
static enum ny_light_period_e ny_plight_period(void);
static int ny_plight_push(float dilation);
static void ny_plight_tell_eyes(uint64_t now);
static void ny_plight_sample(uint64_t now);
static void ny_plight_decode(const cJSON *root);
static void ny_plight_load(void);
static bool ny_plight_learnt(void);
static void ny_plight_save(uint64_t now);
static int ny_plight_configure(const cJSON *data);
static bool ny_plight_add_number(cJSON *root, const char *key, bool known,
                                 double value);
static cJSON *ny_plight_status(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *const g_plight_polarity_names[] =
{
  [NY_LIGHT_POLARITY_AUTO] = "auto",
  [NY_LIGHT_POLARITY_HIGH] = "bright-high",
  [NY_LIGHT_POLARITY_LOW]  = "bright-low",
};

#define NY_PLIGHT_POLARITY_NAMES \
  (sizeof(g_plight_polarity_names) / sizeof(g_plight_polarity_names[0]))

static mutex_t g_plight_lock = NXMUTEX_INITIALIZER;

static struct ny_plight_state_s g_plight =
{
  .filter =
  {
    .cal =
    {
      .gamma = NY_LIGHT_GAMMA_DEFAULT,
    },
  },
  .stored =
  {
    .gamma = NY_LIGHT_GAMMA_DEFAULT,
  },
  .enabled = true,
  .fd = -1,
  .saved = true,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_plight_polarity_name
 *
 * Description:
 *   The owner's choice has an "auto"; what was detected has not been yet.
 *
 ****************************************************************************/

static const char *ny_plight_polarity_name(enum ny_light_polarity_e value,
                                           bool detected)
{
  if (detected && value == NY_LIGHT_POLARITY_AUTO)
    {
      return "unknown";
    }

  return (size_t)value < NY_PLIGHT_POLARITY_NAMES
             ? g_plight_polarity_names[value]
             : "auto";
}

/****************************************************************************
 * Name: ny_plight_polarity_index
 ****************************************************************************/

static int ny_plight_polarity_index(const cJSON *item)
{
  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      return -EINVAL;
    }

  for (size_t i = 0; i < NY_PLIGHT_POLARITY_NAMES; i++)
    {
      if (strcmp(item->valuestring, g_plight_polarity_names[i]) == 0)
        {
          return (int)i;
        }
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: ny_plight_number
 ****************************************************************************/

static bool ny_plight_number(const cJSON *item, double low, double high,
                             double *value)
{
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
      item->valuedouble < low || item->valuedouble > high)
    {
      return false;
    }

  *value = item->valuedouble;
  return true;
}

/****************************************************************************
 * Name: ny_plight_close
 ****************************************************************************/

static void ny_plight_close(void)
{
  if (g_plight.fd >= 0)
    {
      close(g_plight.fd);
      g_plight.fd = -1;
    }
}

/****************************************************************************
 * Name: ny_plight_fail
 *
 * Description:
 *   Drop the node and come back later.  A converter that timed out spent
 *   its whole poll budget busy-waiting inside this tick; it must not get to
 *   do that five times a second.
 *
 ****************************************************************************/

static void ny_plight_fail(const char *what, int error)
{
  uint64_t now = ny_product_time_ms(true);

  ny_plight_close();
  g_plight.failed = what;
  g_plight.error = error;
  if (g_plight.attempts < NY_PLIGHT_OPEN_FAST)
    {
      g_plight.attempts++;
    }

  g_plight.open_after = now + (g_plight.attempts < NY_PLIGHT_OPEN_FAST
                                   ? NY_PLIGHT_OPEN_FAST_MS
                                   : NY_PLIGHT_OPEN_SLOW_MS);
}

/****************************************************************************
 * Name: ny_plight_open
 ****************************************************************************/

static int ny_plight_open(void)
{
  struct adc_msg_s stale;

  if (g_plight.fd >= 0)
    {
      return 0;
    }

  if (ny_product_time_ms(true) < g_plight.open_after)
    {
      return -EAGAIN;
    }

  /* Non-blocking, because a read that finds the FIFO empty would otherwise
   * wait for a conversion nobody is going to start, with the worker and
   * every product service behind it.
   */

  g_plight.fd = open(NY_PLIGHT_DEVICE, O_RDONLY | O_NONBLOCK);
  if (g_plight.fd < 0)
    {
      /* The web layer reports -ENOENT as an unknown topic; a node that is
       * not registered is a device that is not there.
       */

      int error = errno == ENOENT ? -ENODEV : -errno;
      ny_plight_fail("open", error);
      return error;
    }

  /* Results somebody else left in the FIFO are not readings of ours, and a
   * full FIFO makes the next trigger fail.
   */

  while (read(g_plight.fd, &stale, sizeof(stale)) == (ssize_t)sizeof(stale))
    {
    }

  return 0;
}

/****************************************************************************
 * Name: ny_plight_convert
 *
 * Description:
 *   One conversion.  The driver converts inside the ioctl, busy-waiting on
 *   the end-of-conversion bit (about 32 cycles of a 20 MHz clock: a couple
 *   of microseconds), and hands the result to the FIFO that read() empties.
 *
 ****************************************************************************/

static int ny_plight_convert(uint16_t *value)
{
  struct adc_msg_s message;
  ssize_t got;

  if (ioctl(g_plight.fd, ANIOC_TRIGGER, 0) < 0)
    {
      int error = -errno;
      ny_plight_fail("trigger", error);
      return error;
    }

  got = read(g_plight.fd, &message, sizeof(message));
  if (got != (ssize_t)sizeof(message))
    {
      int error = got < 0 ? -errno : -EIO;
      ny_plight_fail("read", error);
      return error;
    }

  *value = message.am_data < 0 ? 0
           : message.am_data > NY_LIGHT_RAW_MAX
               ? NY_LIGHT_RAW_MAX
               : (uint16_t)message.am_data;
  return 0;
}

/****************************************************************************
 * Name: ny_plight_period
 *
 * Description:
 *   Day, night or neither, by the owner's clock.  Without a time that can be
 *   believed and a time zone there is no telling, and guessing would teach
 *   the direction wrong.
 *
 ****************************************************************************/

static enum ny_light_period_e ny_plight_period(void)
{
  int64_t minutes;

  if (!g_plight.offset_known || !ny_product_clock_valid())
    {
      return NY_LIGHT_PERIOD_NONE;
    }

  minutes = (int64_t)(ny_product_time_ms(false) / 60000) + g_plight.utc_offset;
  return ny_light_period((int)(((minutes % 1440) + 1440) % 1440));
}

/****************************************************************************
 * Name: ny_plight_push
 *
 * Description:
 *   Queue eyes.ambient.  No id: nobody waits for this command, and the eye
 *   service keeps the last id it answered for callers that do.
 *
 ****************************************************************************/

static int ny_plight_push(float dilation)
{
#ifdef CONFIG_NYABULA_CORE_EYE
  cJSON *command = cJSON_CreateObject();
  cJSON *params = command != NULL
                      ? cJSON_AddObjectToObject(command, "params")
                      : NULL;
  char *json = NULL;
  int ret = -ENOMEM;

  if (params != NULL &&
      cJSON_AddStringToObject(command, "action", "eyes.ambient") != NULL &&
      cJSON_AddStringToObject(command, "source",
                              NY_PLIGHT_EYE_SOURCE) != NULL &&
      cJSON_AddNumberToObject(command, "priority", 0) != NULL &&
      cJSON_AddNumberToObject(command, "lease_ms", 0) != NULL &&
      cJSON_AddNumberToObject(params, "level",
                              round((1.0 - dilation) * 1000.0) / 1000.0) !=
          NULL)
    {
      json = cJSON_PrintUnformatted(command);
    }

  if (json != NULL)
    {
      ret = nyabula_eye_service_submit(NY_PLIGHT_EYE_SOURCE, json,
                                       strlen(json));
      free(json);
    }

  cJSON_Delete(command);
  return ret;
#else
  (void)dilation;
  return -ENODEV;
#endif
}

/****************************************************************************
 * Name: ny_plight_tell_eyes
 *
 * Description:
 *   Called with g_plight_lock held, after a reading.
 *
 ****************************************************************************/

static void ny_plight_tell_eyes(uint64_t now)
{
  float dilation = g_plight.filter.dilation;
  bool due = !g_plight.pushed_valid || g_plight.push_now ||
             ny_light_push_due(dilation, g_plight.pushed,
                               now - g_plight.pushed_at);

#ifdef CONFIG_NYABULA_CORE_EYE
  if (!due && now >= g_plight.verify_at)
    {
      struct nyabula_core_snapshot_s snapshot;

      g_plight.verify_at = now + NY_PLIGHT_VERIFY_MS;
      due = nyabula_eye_service_snapshot(&snapshot) == 0 &&
            fabsf(snapshot.ambient_light - (1.0f - g_plight.pushed)) >
                NY_PLIGHT_VERIFY_STEP;
    }
#endif

  /* An eye service that is not up yet refuses; the next reading asks
   * again, which costs nothing.
   */

  if (due && ny_plight_push(dilation) == 0)
    {
      g_plight.pushed = dilation;
      g_plight.pushed_at = now;
      g_plight.pushed_valid = true;
      g_plight.push_now = false;
      g_plight.verify_at = now + NY_PLIGHT_VERIFY_MS;
    }
}

/****************************************************************************
 * Name: ny_plight_sample
 *
 * Description:
 *   Three conversions back to back and their median: a converter glitch is
 *   one reading wide.  Called with g_plight_lock held.
 *
 ****************************************************************************/

static void ny_plight_sample(uint64_t now)
{
  uint16_t reading[3];
  float seconds;

  if (ny_plight_open() < 0)
    {
      return;
    }

  for (size_t i = 0; i < 3; i++)
    {
      if (ny_plight_convert(&reading[i]) < 0)
        {
          return;
        }
    }

  seconds = g_plight.sampled_at == 0
                ? 0.0f
                : (float)(now - g_plight.sampled_at) / 1000.0f;
  g_plight.sampled_at = now;
  g_plight.samples++;
  g_plight.attempts = 0;
  g_plight.error = 0;
  g_plight.failed = NULL;
  ny_light_sample(&g_plight.filter,
                  ny_light_median3(reading[0], reading[1], reading[2]),
                  seconds, ny_plight_period());
  ny_plight_tell_eyes(now);
}

/****************************************************************************
 * Name: ny_plight_decode
 *
 * Description:
 *   Take a stored record, whole or not at all.
 *
 ****************************************************************************/

static void ny_plight_decode(const cJSON *root)
{
  struct ny_light_calibration_s cal;
  const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
  const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
  const cJSON *pinned = cJSON_GetObjectItemCaseSensitive(root, "pinned");
  const cJSON *offset =
      cJSON_GetObjectItemCaseSensitive(root, "utc_offset_minutes");
  const cJSON *min = cJSON_GetObjectItemCaseSensitive(root, "min");
  int polarity = ny_plight_polarity_index(
      cJSON_GetObjectItemCaseSensitive(root, "polarity"));
  int detected = ny_plight_polarity_index(
      cJSON_GetObjectItemCaseSensitive(root, "detected"));
  double number[7];
  double minutes = 0;
  bool valid;

  memset(&cal, 0, sizeof(cal));
  memset(number, 0, sizeof(number));
  valid = cJSON_IsNumber(schema) && schema->valueint == NY_PLIGHT_SCHEMA &&
          cJSON_IsBool(enabled) && cJSON_IsBool(pinned) && polarity >= 0 &&
          detected >= 0 &&
          ny_plight_number(cJSON_GetObjectItemCaseSensitive(root, "gamma"),
                           NY_LIGHT_GAMMA_MIN, NY_LIGHT_GAMMA_MAX,
                           &number[0]) &&
          ny_plight_number(cJSON_GetObjectItemCaseSensitive(root, "dayMean"),
                           0, NY_LIGHT_RAW_MAX, &number[1]) &&
          ny_plight_number(
              cJSON_GetObjectItemCaseSensitive(root, "daySeconds"), 0,
              NY_LIGHT_EVIDENCE_CAP, &number[2]) &&
          ny_plight_number(
              cJSON_GetObjectItemCaseSensitive(root, "nightMean"), 0,
              NY_LIGHT_RAW_MAX, &number[3]) &&
          ny_plight_number(
              cJSON_GetObjectItemCaseSensitive(root, "nightSeconds"), 0,
              NY_LIGHT_EVIDENCE_CAP, &number[4]);

  /* A range is stored once there is one. */

  if (valid && min != NULL)
    {
      valid = ny_plight_number(min, 0, NY_LIGHT_RAW_MAX, &number[5]) &&
              ny_plight_number(cJSON_GetObjectItemCaseSensitive(root, "max"),
                               number[5], NY_LIGHT_RAW_MAX, &number[6]);
      cal.ranged = true;
    }

  if (valid && offset != NULL)
    {
      valid = ny_plight_number(offset, NY_PLIGHT_OFFSET_MIN,
                               NY_PLIGHT_OFFSET_MAX, &minutes);
    }

  if (!valid || (cJSON_IsTrue(pinned) && !cal.ranged))
    {
      return;
    }

  cal.polarity = (enum ny_light_polarity_e)polarity;
  cal.detected = (enum ny_light_polarity_e)detected;
  cal.pinned = cJSON_IsTrue(pinned);
  cal.gamma = (float)number[0];
  cal.day_mean = (float)number[1];
  cal.day_seconds = (float)number[2];
  cal.night_mean = (float)number[3];
  cal.night_seconds = (float)number[4];
  cal.min = (float)number[5];
  cal.max = (float)number[6];

  /* Readings taken before the store could be read are a few seconds of one
   * room; the record is the device's whole history.
   */

  g_plight.filter.cal = cal;
  g_plight.filter.settled = false;
  g_plight.stored = cal;
  g_plight.enabled = cJSON_IsTrue(enabled);
  g_plight.offset_known = offset != NULL;
  g_plight.utc_offset = (int)minutes;
}

/****************************************************************************
 * Name: ny_plight_load
 *
 * Description:
 *   Read the stored record once.  Called with g_plight_lock held, and only
 *   from callers with a product-sized stack: the store is SQLite.
 *
 ****************************************************************************/

static void ny_plight_load(void)
{
  uint64_t now = ny_product_time_ms(true);
  uint64_t revision = 0;
  cJSON *root = NULL;
  int ret;

  if (g_plight.loaded || now < g_plight.store_after)
    {
      return;
    }

  ret = ny_product_store_read(NY_PLIGHT_DOMAIN, &root, &revision);
  if (ret < 0 && ret != -EBADMSG)
    {
      g_plight.store_after = now + NY_PLIGHT_STORE_RETRY_MS;
      return;
    }

  /* A record that does not parse leaves the defaults in charge. */

  if (ret == 0 && root != NULL)
    {
      ny_plight_decode(root);
    }

  cJSON_Delete(root);
  g_plight.revision = revision;
  g_plight.loaded = true;
}

/****************************************************************************
 * Name: ny_plight_learnt
 *
 * Description:
 *   True when what was learnt since the last write is worth a write.
 *
 ****************************************************************************/

static bool ny_plight_learnt(void)
{
  const struct ny_light_calibration_s *now = &g_plight.filter.cal;
  const struct ny_light_calibration_s *then = &g_plight.stored;

  return now->ranged != then->ranged || now->detected != then->detected ||
         (now->ranged &&
          (fabsf(now->min - then->min) >= NY_PLIGHT_SAVE_RANGE ||
           fabsf(now->max - then->max) >= NY_PLIGHT_SAVE_RANGE)) ||
         fabsf(now->day_seconds - then->day_seconds) >=
             NY_PLIGHT_SAVE_EVIDENCE ||
         fabsf(now->night_seconds - then->night_seconds) >=
             NY_PLIGHT_SAVE_EVIDENCE;
}

/****************************************************************************
 * Name: ny_plight_save
 *
 * Description:
 *   Write the record when the owner changed something, or when enough was
 *   learnt and the last write is long enough ago.  Called with
 *   g_plight_lock held.  A failure leaves it for the worker tick.
 *
 ****************************************************************************/

static void ny_plight_save(uint64_t now)
{
  const struct ny_light_calibration_s *cal = &g_plight.filter.cal;
  uint64_t revision = 0;
  cJSON *root;
  bool valid;
  int ret;

  if (!g_plight.loaded || now < g_plight.store_after)
    {
      return;
    }

  if (!g_plight.dirty &&
      (!ny_plight_learnt() ||
       (g_plight.saved_at != 0 &&
        now - g_plight.saved_at < NY_PLIGHT_SAVE_INTERVAL_MS)))
    {
      return;
    }

  root = cJSON_CreateObject();
  valid =
      root != NULL &&
      cJSON_AddNumberToObject(root, "schema", NY_PLIGHT_SCHEMA) != NULL &&
      cJSON_AddBoolToObject(root, "enabled", g_plight.enabled) != NULL &&
      cJSON_AddStringToObject(
          root, "polarity",
          ny_plight_polarity_name(cal->polarity, false)) != NULL &&
      cJSON_AddStringToObject(
          root, "detected",
          ny_plight_polarity_name(cal->detected, false)) != NULL &&
      cJSON_AddBoolToObject(root, "pinned", cal->pinned) != NULL &&
      cJSON_AddNumberToObject(root, "gamma", cal->gamma) != NULL &&
      cJSON_AddNumberToObject(root, "dayMean", cal->day_mean) != NULL &&
      cJSON_AddNumberToObject(root, "daySeconds", cal->day_seconds) != NULL &&
      cJSON_AddNumberToObject(root, "nightMean", cal->night_mean) != NULL &&
      cJSON_AddNumberToObject(root, "nightSeconds",
                              cal->night_seconds) != NULL;
  if (valid && cal->ranged)
    {
      valid = cJSON_AddNumberToObject(root, "min", cal->min) != NULL &&
              cJSON_AddNumberToObject(root, "max", cal->max) != NULL;
    }

  if (valid && g_plight.offset_known)
    {
      valid = cJSON_AddNumberToObject(root, "utc_offset_minutes",
                                      g_plight.utc_offset) != NULL;
    }

  ret = valid ? ny_product_store_write(NY_PLIGHT_DOMAIN, root,
                                       g_plight.revision, &revision)
              : -ENOMEM;
  if (ret == -ESTALE)
    {
      /* Someone else wrote the record.  This is what the sensor is running
       * with, so it wins; only the revision was wrong.
       */

      cJSON *other = NULL;
      ret = ny_product_store_read(NY_PLIGHT_DOMAIN, &other,
                                  &g_plight.revision);
      cJSON_Delete(other);
      if (ret == 0)
        {
          ret = ny_product_store_write(NY_PLIGHT_DOMAIN, root,
                                       g_plight.revision, &revision);
        }
    }

  cJSON_Delete(root);
  g_plight.saved = ret == 0;
  if (ret == 0)
    {
      g_plight.revision = revision;
      g_plight.stored = *cal;
      g_plight.saved_at = now;
      g_plight.dirty = false;
    }
  else
    {
      g_plight.store_after = now + NY_PLIGHT_STORE_RETRY_MS;
    }
}

/****************************************************************************
 * Name: ny_plight_configure
 *
 * Description:
 *   light.config.  Everything is checked before anything is changed.  Called
 *   with g_plight_lock held.
 *
 ****************************************************************************/

static int ny_plight_configure(const cJSON *data)
{
  const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
  const cJSON *polarity = cJSON_GetObjectItemCaseSensitive(data, "polarity");
  const cJSON *min = cJSON_GetObjectItemCaseSensitive(data, "min");
  const cJSON *max = cJSON_GetObjectItemCaseSensitive(data, "max");
  const cJSON *pinned = cJSON_GetObjectItemCaseSensitive(data, "pinned");
  const cJSON *gamma = cJSON_GetObjectItemCaseSensitive(data, "gamma");
  const cJSON *reset = cJSON_GetObjectItemCaseSensitive(data, "reset");
  const cJSON *offset =
      cJSON_GetObjectItemCaseSensitive(data, "utc_offset_minutes");
  struct ny_light_filter_s *filter = &g_plight.filter;
  bool forget = cJSON_IsTrue(reset);
  double low = filter->cal.min;
  double high = filter->cal.max;
  double power = filter->cal.gamma;
  double minutes = g_plight.utc_offset;
  int choice = filter->cal.polarity;

  if (enabled == NULL && polarity == NULL && min == NULL && max == NULL &&
      pinned == NULL && gamma == NULL && reset == NULL && offset == NULL)
    {
      return -EINVAL;
    }

  /* Pinning one end keeps the other where learning left it, which needs
   * something to have been learnt; and a reset forgets exactly that.
   */

  if ((enabled != NULL && !cJSON_IsBool(enabled)) ||
      (reset != NULL && !cJSON_IsBool(reset)) ||
      (pinned != NULL && !cJSON_IsFalse(pinned)) ||
      (polarity != NULL &&
       (choice = ny_plight_polarity_index(polarity)) < 0) ||
      (min != NULL && !ny_plight_number(min, 0, NY_LIGHT_RAW_MAX, &low)) ||
      (max != NULL && !ny_plight_number(max, 0, NY_LIGHT_RAW_MAX, &high)) ||
      (gamma != NULL && !ny_plight_number(gamma, NY_LIGHT_GAMMA_MIN,
                                          NY_LIGHT_GAMMA_MAX, &power)) ||
      (offset != NULL &&
       (!ny_plight_number(offset, NY_PLIGHT_OFFSET_MIN, NY_PLIGHT_OFFSET_MAX,
                          &minutes) ||
        floor(minutes) != minutes)))
    {
      return -EINVAL;
    }

  if (min != NULL || max != NULL)
    {
      if (pinned != NULL || ((min == NULL || max == NULL) &&
                             (forget || !filter->cal.ranged)) ||
          high - low < NY_PLIGHT_PIN_SPAN)
        {
          return -EINVAL;
        }
    }

  if (forget)
    {
      ny_light_reset(filter);
    }

  if (enabled != NULL)
    {
      g_plight.enabled = cJSON_IsTrue(enabled);
    }

  if (offset != NULL)
    {
      g_plight.utc_offset = (int)minutes;
      g_plight.offset_known = true;
    }

  filter->cal.polarity = (enum ny_light_polarity_e)choice;
  filter->cal.gamma = (float)power;
  if (min != NULL || max != NULL)
    {
      filter->cal.min = (float)low;
      filter->cal.max = (float)high;
      filter->cal.ranged = true;
      filter->cal.pinned = true;
    }
  else if (pinned != NULL)
    {
      filter->cal.pinned = false;
    }

  /* The owner is looking at the eyes while turning these: show the result
   * now, not after the hysteresis and the rate limit had their say.
   */

  if (filter->primed)
    {
      filter->level = ny_light_level(&filter->cal, filter->smoothed);
      filter->dilation = ny_light_dilation(filter->level, filter->cal.gamma);
      filter->settled = true;
    }

  g_plight.push_now = true;
  g_plight.dirty = true;
  g_plight.saved = false;
  return 0;
}

/****************************************************************************
 * Name: ny_plight_add_number
 *
 * Description:
 *   A number, or null for something that is not known yet: a panel must be
 *   able to tell "no reading" from a reading of zero.
 *
 ****************************************************************************/

static bool ny_plight_add_number(cJSON *root, const char *key, bool known,
                                 double value)
{
  return (known ? cJSON_AddNumberToObject(root, key, value)
                : cJSON_AddNullToObject(root, key)) != NULL;
}

/****************************************************************************
 * Name: ny_plight_status
 *
 * Description:
 *   Called with g_plight_lock held.
 *
 ****************************************************************************/

static cJSON *ny_plight_status(void)
{
  const struct ny_light_filter_s *filter = &g_plight.filter;
  const struct ny_light_calibration_s *cal = &filter->cal;
  uint64_t now = ny_product_time_ms(true);
  bool live = filter->primed && g_plight.sampled_at != 0;
  int rail = ny_light_saturated(filter->raw);
  char error[48];
  cJSON *root = cJSON_CreateObject();
  cJSON *evidence =
      root != NULL ? cJSON_AddObjectToObject(root, "evidence") : NULL;
  bool valid;

  snprintf(error, sizeof(error), "%s failed (%d)",
           g_plight.failed != NULL ? g_plight.failed : "sensor",
           g_plight.error);
  valid =
      evidence != NULL &&
      cJSON_AddBoolToObject(root, "available", g_plight.fd >= 0) != NULL &&
      cJSON_AddBoolToObject(root, "enabled", g_plight.enabled) != NULL &&
      cJSON_AddStringToObject(root, "device", NY_PLIGHT_DEVICE) != NULL &&
      ny_plight_add_number(root, "raw", live, filter->raw) &&
      ny_plight_add_number(root, "smoothed", live,
                           round(filter->smoothed * 10.0) / 10.0) &&
      ny_plight_add_number(root, "min", cal->ranged,
                           round(cal->min * 10.0) / 10.0) &&
      ny_plight_add_number(root, "max", cal->ranged,
                           round(cal->max * 10.0) / 10.0) &&
      ny_plight_add_number(root, "level", live,
                           round(filter->level * 1000.0) / 1000.0) &&
      ny_plight_add_number(root, "dilation", live,
                           round(filter->dilation * 1000.0) / 1000.0) &&
      cJSON_AddStringToObject(
          root, "polarity",
          ny_plight_polarity_name(cal->polarity, false)) != NULL &&
      cJSON_AddStringToObject(
          root, "resolved",
          ny_plight_polarity_name(ny_light_resolved(cal), false)) != NULL &&
      cJSON_AddStringToObject(
          root, "detected",
          ny_plight_polarity_name(cal->detected, true)) != NULL &&
      cJSON_AddBoolToObject(root, "pinned", cal->pinned) != NULL &&
      cJSON_AddNumberToObject(root, "gamma",
                              round(cal->gamma * 1000.0) / 1000.0) != NULL &&
      (!live || rail == 0
           ? cJSON_AddNullToObject(root, "saturated")
           : cJSON_AddStringToObject(root, "saturated",
                                     rail < 0 ? "low" : "high")) != NULL &&
      cJSON_AddNumberToObject(root, "samples", g_plight.samples) != NULL &&
      ny_plight_add_number(root, "ageMs", live,
                           (double)(now - g_plight.sampled_at)) &&
      ny_plight_add_number(root, "utc_offset_minutes", g_plight.offset_known,
                           g_plight.utc_offset) &&
      cJSON_AddNumberToObject(evidence, "daySeconds",
                              round(cal->day_seconds)) != NULL &&
      cJSON_AddNumberToObject(evidence, "nightSeconds",
                              round(cal->night_seconds)) != NULL &&
      ny_plight_add_number(evidence, "dayMean", cal->day_seconds > 0.0f,
                           round(cal->day_mean)) &&
      ny_plight_add_number(evidence, "nightMean", cal->night_seconds > 0.0f,
                           round(cal->night_mean)) &&
      cJSON_AddBoolToObject(root, "settingsSaved", g_plight.saved) != NULL &&
      (g_plight.error == 0 ? cJSON_AddNullToObject(root, "error")
                           : cJSON_AddStringToObject(root, "error", error)) !=
          NULL;
  if (!valid)
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_product_light_request
 ****************************************************************************/

int ny_product_light_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result)
{
  bool config = strcmp(topic, "light.config") == 0;
  int ret;

  if (!config && strcmp(topic, "light.status") != 0)
    {
      return -ENOSYS;
    }

  if (config && caller->role != NY_PRODUCT_OWNER)
    {
      return -EACCES;
    }

  ret = nxmutex_lock(&g_plight_lock);
  if (ret < 0)
    {
      return ret;
    }

  ny_plight_load();
  if (config)
    {
      /* A choice made before the record could be read would be undone by
       * it, or would overwrite a history it never saw.
       */

      ret = g_plight.loaded ? ny_plight_configure(data) : -EAGAIN;
      if (ret == 0)
        {
          uint64_t now = ny_product_time_ms(true);

          if (g_plight.enabled && g_plight.filter.primed)
            {
              ny_plight_tell_eyes(now);
            }

          ny_plight_save(now);
        }
    }

  if (ret == 0)
    {
      *result = ny_plight_status();
      ret = *result != NULL ? 0 : -ENOMEM;
    }

  nxmutex_unlock(&g_plight_lock);

  /* -ENOSYS would hand a topic that is ours to the next product module. */

  return ret == -ENOSYS ? -ENODEV : ret;
}

/****************************************************************************
 * Name: ny_product_light_tick
 *
 * Description:
 *   Called by the product worker.  A sensor that is not there is not an
 *   error of the worker, so nothing is reported for it; light.status says.
 *
 ****************************************************************************/

int ny_product_light_tick(void)
{
  uint64_t now;

  if (nxmutex_lock(&g_plight_lock) < 0)
    {
      return 0;
    }

  ny_plight_load();
  now = ny_product_time_ms(true);
  if (!g_plight.enabled)
    {
      /* Keep the node open all the same: whether there is a sensor is worth
       * knowing before the owner switches it on.
       */

      ny_plight_open();
    }
  else if (now - g_plight.sampled_at >= NY_PLIGHT_SAMPLE_MS)
    {
      ny_plight_sample(now);
    }

  ny_plight_save(now);
  nxmutex_unlock(&g_plight_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_product_light_shutdown
 ****************************************************************************/

void ny_product_light_shutdown(void)
{
  if (nxmutex_lock(&g_plight_lock) < 0)
    {
      return;
    }

  /* Closing the last open channel gates the converter's clocks. */

  ny_plight_close();
  g_plight.sampled_at = 0;
  g_plight.pushed_valid = false;
  nxmutex_unlock(&g_plight_lock);
}

/****************************************************************************
 * Name: ny_product_light_describe
 ****************************************************************************/

bool ny_product_light_describe(cJSON *root)
{
  bool valid = true;

  if (nxmutex_lock(&g_plight_lock) < 0)
    {
      return true;
    }

  if (g_plight.enabled && g_plight.fd >= 0 && g_plight.filter.primed)
    {
      cJSON *light = cJSON_AddObjectToObject(root, "light");
      valid = light != NULL &&
              cJSON_AddNumberToObject(
                  light, "level",
                  round(g_plight.filter.level * 1000.0) / 1000.0) != NULL &&
              cJSON_AddNumberToObject(
                  light, "dilation",
                  round(g_plight.filter.dilation * 1000.0) / 1000.0) != NULL;
    }

  nxmutex_unlock(&g_plight_lock);
  return valid;
}
