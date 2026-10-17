/****************************************************************************
 * app/nyabula_core/ny_product_clock.c
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

/* What the product knows about its own wall clock.
 *
 * A clock that shows a plausible year is not thereby right: the board's
 * RTC keeps counting from whatever it was last given, and a device that was
 * never told the time reported 2021 as valid because 2021 is after 2020.
 * So validity is tied to where the time came from.  It is valid when
 * something set it during this boot -- a time server or the owner's panel --
 * or when the RTC both says it has been set and shows a time that is not
 * before this firmware was built, which no correct clock can show.
 *
 * Every change of the clock goes through ny_product_clock_set(), which also
 * makes sure the RTC holds the new time, so that the next boot starts from
 * it.
 */

#include "ny_product.h"
#include "ny_sntp.h"

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_RTC_DRIVER
#include <nuttx/timers/rtc.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_NYABULA_CORE_CLOCK_RTC_PATH
#define CONFIG_NYABULA_CORE_CLOCK_RTC_PATH "/dev/rtc0"
#endif

/* The floor used when the build date cannot be had, and always on the
 * simulator: its tests replay fixed dates, which a floor that moves with
 * the calendar would start refusing one day.
 */

#define NY_CLOCK_FIXED_FLOOR_MS 1577836800000ULL /* 2020-01-01 */

/* __DATE__ is the build machine's local date, so it can be up to a day
 * ahead of UTC.  The floor must never be later than the real time.
 */

#define NY_CLOCK_FLOOR_MARGIN_MS 86400000ULL

/* A change smaller than this is the clock being trimmed, not moved: nobody
 * who scheduled something needs to hear about it.
 */

#define NY_CLOCK_STEP_NOTICE_MS 2000

/* The RTC counts whole seconds, so it agrees with the system clock when it
 * is within a second either way; allow one more for the two reads.
 */

#define NY_CLOCK_RTC_TOLERANCE_S 2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_clock_s
{
  bool ready;
  bool rtc_trusted; /* The RTC says its time has been set */
  uint64_t floor_ms;
  enum ny_product_clock_source_e set_by; /* NONE until set in this boot */
  uint64_t synced_at_ms;
  uint32_t generation; /* Counts the steps worth telling others about */
  uint64_t step_before_ms;
  uint64_t step_after_ms;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void ny_clock_iso(uint64_t unix_ms, char *out, size_t size);
static bool ny_clock_rtc_probe(void);
static void ny_clock_rtc_store(const struct timespec *now);
static void ny_clock_prepare(void);
static enum ny_product_clock_source_e ny_clock_source_locked(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_clock_lock = NXMUTEX_INITIALIZER;
static struct ny_clock_s g_clock;

static const char *const g_clock_source_names[] = { "none", "rtc", "panel",
                                                    "sntp" };

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_clock_iso
 ****************************************************************************/

static void ny_clock_iso(uint64_t unix_ms, char *out, size_t size)
{
  time_t seconds = (time_t)(unix_ms / 1000);
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  gmtime_r(&seconds, &tm);
  snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/****************************************************************************
 * Name: ny_clock_rtc_probe
 *
 * Description:
 *   Whether the RTC vouches for its own time.  The PCF8563 raises a flag
 *   when its supply dropped far enough for the oscillator to have stopped,
 *   and keeps it until a time is written; with the flag up, what the
 *   registers hold is whatever they held when the power came back.
 *
 ****************************************************************************/

static bool ny_clock_rtc_probe(void)
{
#ifdef CONFIG_RTC_DRIVER
  bool set = false;
  int fd = open(CONFIG_NYABULA_CORE_CLOCK_RTC_PATH, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      return false;
    }

  /* A driver that cannot tell is given the benefit of the doubt: its time
   * still has to get past the build date.
   */

  if (ioctl(fd, RTC_HAVE_SET_TIME, (unsigned long)(uintptr_t)&set) < 0)
    {
      set = true;
    }

  close(fd);
  return set;
#else
  return false;
#endif
}

/****************************************************************************
 * Name: ny_clock_rtc_store
 *
 * Description:
 *   Make sure the RTC holds the time the system clock was just given.
 *
 *   With CONFIG_RTC_ARCH and a lower half handed to up_rtc_set_lowerhalf(),
 *   as this board does, clock_settime() has already written it.  That is a
 *   property of one configuration, and when it is absent nothing says so:
 *   the clock is right until the power goes and wrong for ever after.  So
 *   the RTC is read back, and written directly if it does not agree.
 *
 ****************************************************************************/

static void ny_clock_rtc_store(const struct timespec *now)
{
#ifdef CONFIG_RTC_DRIVER
  struct rtc_time rtc;
  struct tm tm;
  int64_t drift = 0;
  int ret;
  int fd = open(CONFIG_NYABULA_CORE_CLOCK_RTC_PATH, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      syslog(LOG_WARNING,
             "nyclock: no RTC at %s (%d), the time will not "
             "survive a power cycle\n",
             CONFIG_NYABULA_CORE_CLOCK_RTC_PATH, -errno);
      return;
    }

  memset(&rtc, 0, sizeof(rtc));
  ret = ioctl(fd, RTC_RD_TIME, (unsigned long)(uintptr_t)&rtc);
  if (ret == 0)
    {
      memset(&tm, 0, sizeof(tm));
      tm.tm_sec = rtc.tm_sec;
      tm.tm_min = rtc.tm_min;
      tm.tm_hour = rtc.tm_hour;
      tm.tm_mday = rtc.tm_mday;
      tm.tm_mon = rtc.tm_mon;
      tm.tm_year = rtc.tm_year;
      drift = (int64_t)timegm(&tm) - (int64_t)now->tv_sec;
    }

  if (ret == 0 && drift >= -NY_CLOCK_RTC_TOLERANCE_S &&
      drift <= NY_CLOCK_RTC_TOLERANCE_S)
    {
      syslog(LOG_INFO, "nyclock: RTC holds the new time\n");
      close(fd);
      return;
    }

  memset(&tm, 0, sizeof(tm));
  gmtime_r(&now->tv_sec, &tm);
  memset(&rtc, 0, sizeof(rtc));
  rtc.tm_sec = tm.tm_sec;
  rtc.tm_min = tm.tm_min;
  rtc.tm_hour = tm.tm_hour;
  rtc.tm_mday = tm.tm_mday;
  rtc.tm_mon = tm.tm_mon;
  rtc.tm_year = tm.tm_year;
  rtc.tm_wday = tm.tm_wday;
  rtc.tm_yday = tm.tm_yday;
  ret = ioctl(fd, RTC_SET_TIME, (unsigned long)(uintptr_t)&rtc);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "nyclock: RTC write failed (%d), the time will not "
             "survive a power cycle\n",
             -errno);
    }
  else
    {
      syslog(LOG_INFO, "nyclock: RTC written directly, it was %lld s off\n",
             (long long)drift);
    }

  close(fd);
#else
  (void)now;
#endif
}

/****************************************************************************
 * Name: ny_clock_prepare
 *
 * Description:
 *   Work out, once, what this boot started with (lock held).
 *
 ****************************************************************************/

static void ny_clock_prepare(void)
{
  char wall_text[40];
  char floor_text[40];
  if (g_clock.ready)
    {
      return;
    }

  g_clock.floor_ms = NY_CLOCK_FIXED_FLOOR_MS;
#ifndef CONFIG_ARCH_SIM
  int64_t built = ny_sntp_date_ms(__DATE__);
  if (built > (int64_t)(NY_CLOCK_FIXED_FLOOR_MS + NY_CLOCK_FLOOR_MARGIN_MS))
    {
      g_clock.floor_ms = (uint64_t)built - NY_CLOCK_FLOOR_MARGIN_MS;
    }
#endif

  g_clock.rtc_trusted = ny_clock_rtc_probe();
  g_clock.ready = true;
  ny_clock_iso(ny_product_time_ms(false), wall_text, sizeof(wall_text));
  ny_clock_iso(g_clock.floor_ms, floor_text, sizeof(floor_text));
  syslog(LOG_INFO, "nyclock: boot clock %s, floor %s, rtc_set=%d -> %s\n",
         wall_text, floor_text, g_clock.rtc_trusted,
         g_clock_source_names[ny_clock_source_locked()]);
}

/****************************************************************************
 * Name: ny_clock_source_locked
 ****************************************************************************/

static enum ny_product_clock_source_e ny_clock_source_locked(void)
{
  if (g_clock.set_by != NY_PRODUCT_CLOCK_NONE)
    {
      return g_clock.set_by;
    }

  return g_clock.rtc_trusted && ny_product_time_ms(false) >= g_clock.floor_ms
             ? NY_PRODUCT_CLOCK_RTC
             : NY_PRODUCT_CLOCK_NONE;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_product_clock_floor_ms
 *
 * Description:
 *   The earliest time a correct clock can show: the day before this file
 *   was compiled.  __DATE__ is fixed when the object is built, so after an
 *   incremental build it may be older than the image; that only makes the
 *   floor more lenient, never wrong.
 *
 ****************************************************************************/

uint64_t ny_product_clock_floor_ms(void)
{
  uint64_t floor_ms = NY_CLOCK_FIXED_FLOOR_MS;
  if (nxmutex_lock(&g_clock_lock) == 0)
    {
      ny_clock_prepare();
      floor_ms = g_clock.floor_ms;
      nxmutex_unlock(&g_clock_lock);
    }

  return floor_ms;
}

/****************************************************************************
 * Name: ny_product_clock_source
 ****************************************************************************/

enum ny_product_clock_source_e ny_product_clock_source(void)
{
  enum ny_product_clock_source_e source = NY_PRODUCT_CLOCK_NONE;
  if (nxmutex_lock(&g_clock_lock) == 0)
    {
      ny_clock_prepare();
      source = ny_clock_source_locked();
      nxmutex_unlock(&g_clock_lock);
    }

  return source;
}

/****************************************************************************
 * Name: ny_product_clock_valid
 ****************************************************************************/

bool ny_product_clock_valid(void)
{
  return ny_product_clock_source() != NY_PRODUCT_CLOCK_NONE;
}

/****************************************************************************
 * Name: ny_product_clock_set
 *
 * Description:
 *   Step the clock.  The detail is for the log: who said so.
 *
 ****************************************************************************/

int ny_product_clock_set(uint64_t unix_ms,
                         enum ny_product_clock_source_e source,
                         const char *detail)
{
  struct timespec now;
  char before[40];
  char after[40];
  uint64_t old_ms;
  int64_t delta;
  int ret;
  if (source != NY_PRODUCT_CLOCK_PANEL && source != NY_PRODUCT_CLOCK_SNTP)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_clock_lock);
  if (ret < 0)
    {
      return ret;
    }

  ny_clock_prepare();
  if (unix_ms < g_clock.floor_ms || unix_ms > NY_PRODUCT_CLOCK_MAX_MS)
    {
      nxmutex_unlock(&g_clock_lock);
      return -ERANGE;
    }

  old_ms = ny_product_time_ms(false);
  now.tv_sec = (time_t)(unix_ms / 1000);
  now.tv_nsec = (long)(unix_ms % 1000) * 1000000;
  if (clock_settime(CLOCK_REALTIME, &now) < 0)
    {
      ret = -errno;
      nxmutex_unlock(&g_clock_lock);
      return ret;
    }

  delta = (int64_t)unix_ms - (int64_t)old_ms;
  g_clock.set_by = source;
  g_clock.synced_at_ms = unix_ms;
  if (delta >= NY_CLOCK_STEP_NOTICE_MS || delta <= -NY_CLOCK_STEP_NOTICE_MS)
    {
      g_clock.generation++;
      g_clock.step_before_ms = old_ms;
      g_clock.step_after_ms = unix_ms;
    }

  ny_clock_iso(old_ms, before, sizeof(before));
  ny_clock_iso(unix_ms, after, sizeof(after));
  syslog(LOG_INFO, "nyclock: %s set %s -> %s (%c%lld.%03lld s) %s\n",
         g_clock_source_names[source], before, after, delta < 0 ? '-' : '+',
         (long long)((delta < 0 ? -delta : delta) / 1000),
         (long long)((delta < 0 ? -delta : delta) % 1000),
         detail != NULL ? detail : "");

  /* Under the lock: two sources setting the time at once must not leave
   * the RTC with the one that lost.
   */

  ny_clock_rtc_store(&now);
  g_clock.rtc_trusted = true;
  nxmutex_unlock(&g_clock_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_product_clock_step
 *
 * Description:
 *   The last time the clock was moved rather than trimmed.  The return
 *   value changes with every such step, so a service that scheduled
 *   something against the wall clock can tell that it has to look again,
 *   and from where to where the clock went.
 *
 ****************************************************************************/

uint32_t ny_product_clock_step(uint64_t *before_ms, uint64_t *after_ms)
{
  uint32_t generation = 0;
  if (nxmutex_lock(&g_clock_lock) == 0)
    {
      generation = g_clock.generation;
      if (before_ms != NULL)
        {
          *before_ms = g_clock.step_before_ms;
        }

      if (after_ms != NULL)
        {
          *after_ms = g_clock.step_after_ms;
        }

      nxmutex_unlock(&g_clock_lock);
    }

  return generation;
}

/****************************************************************************
 * Name: ny_product_clock_describe
 *
 * Description:
 *   Add what is known about the clock to a result.  False means out of
 *   memory.
 *
 ****************************************************************************/

bool ny_product_clock_describe(cJSON *root)
{
  enum ny_product_clock_source_e source;
  uint64_t synced_at_ms;
  uint64_t floor_ms;
  if (nxmutex_lock(&g_clock_lock) < 0)
    {
      return false;
    }

  ny_clock_prepare();
  source = ny_clock_source_locked();
  synced_at_ms = g_clock.synced_at_ms;
  floor_ms = g_clock.floor_ms;
  nxmutex_unlock(&g_clock_lock);
  return cJSON_AddBoolToObject(root, "clock_valid",
                               source != NY_PRODUCT_CLOCK_NONE) != NULL &&
         cJSON_AddStringToObject(root, "clock_source",
                                 g_clock_source_names[source]) != NULL &&
         cJSON_AddNumberToObject(root, "synced_at_ms", (double)synced_at_ms) !=
             NULL &&
         cJSON_AddNumberToObject(root, "clock_floor_ms", (double)floor_ms) !=
             NULL;
}
