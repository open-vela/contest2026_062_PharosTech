/****************************************************************************
 * drivers/drivers/pcf8563/pcf8563.c
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

/****************************************************************************
 * PCF8563 / HYM8563TS I2C real-time clock driver (lower half).
 *
 * HYM8563TS (Huayi) is a register-compatible replacement for the NXP
 * PCF8563.  16 BCD-coded registers (00h..0Fh), fixed I2C slave address
 * 0x51.
 *
 * ALARM GRANULARITY (important, not a bug):
 *   The PCF8563 alarm matches only the enabled fields among minute, hour,
 *   day and weekday; SECONDS NEVER PARTICIPATE in the comparison.  The
 *   alarm flag (AF) is asserted at the instant the time counter increments
 *   to a state that matches the programmed alarm fields (for a minute alarm
 *   this is the top of the target minute, i.e. hh:mm:00).
 *
 *   A relative-alarm request (RTC_SET_RELATIVE, e.g. "alarm N") can only be
 *   honored to minute accuracy: a target with a nonzero second component is
 *   rounded UP to the next minute (see pcf8563_setalarm_internal(), which
 *   clamps both absolute and relative alarms), so the alarm fires at a
 *   minute boundary 0..59 seconds AFTER the wall-clock second anticipated
 *   by a sub-minute request -- it never fires early.  Use a system timer
 *   (sleep/hrtimer/watchdog) if second-accurate wakeups are required.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/timers/arch_rtc.h>
#include <nuttx/timers/rtc.h>
#include <nuttx/wqueue.h>

#include "pcf8563.h"

#ifdef CONFIG_RTC_PCF8563

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* This RTC driver supports only date/time RTC hardware. */

#ifndef CONFIG_RTC_DATETIME
#error CONFIG_RTC_DATETIME must be set to use this driver
#endif

#ifdef CONFIG_RTC_HIRES
#error CONFIG_RTC_HIRES must NOT be set with this driver
#endif

#ifndef CONFIG_PCF8563_I2C_FREQUENCY
#define CONFIG_PCF8563_I2C_FREQUENCY 400000
#endif

/* When clearing AF/AIE fails the PCF8563 keeps the INT pin low.  The board
 * interrupt is falling-edge sensitive, so no new edge ever arrives to re-arm
 * the worker; the alarm would otherwise be lost forever.  The worker instead
 * leaves the pending latch set and re-queues itself after this delay to retry
 * the (typically transient) I2C failure.
 */

#define PCF8563_ALARM_RETRY_DELAY_MSEC 200

/* The PCF8563 alarm compares only minute/hour/day fields (no month/year),
 * so a one-shot alarm fires at the next occurrence of the programmed
 * day-of-month + time after it is armed.  To keep that behaviour
 * unambiguous we reject any target whose offset from "now" cannot be
 * represented by a single, well-defined future match (see the delta check
 * in pcf8563_setalarm_internal()): a zero/negative offset would match an
 * already-passed minute and fire immediately/stale, and an offset beyond
 * PCF8563_ALARM_MAX_SECONDS (28 days) might first match an intermediate
 * day-of-month in a stale month and falsely consume the one-shot alarm.
 *
 * The 28-day cap is chosen so that a target day-of-month that has already
 * passed this month but still falls within the window resolves to its next
 * (i.e. the intended) occurrence in the following month, while never being
 * far enough out to alias onto an unintended earlier month.  This makes the
 * API predictable at the cost of at most ~3 days of maximum alarm range.
 */

#define PCF8563_ALARM_MAX_SECONDS (28 * 24 * 60 * 60)

/* I2C 7-bit slave address (fixed, not configurable on the part). */

#define PCF8563_I2C_ADDRESS 0x51

/* Register map (auto-incrementing address). */

#define PCF8563_REG_CTRL_STATUS1   0x00 /* Control/status 1 */
#define PCF8563_REG_CTRL_STATUS2   0x01 /* Control/status 2 */
#define PCF8563_REG_VL_SECONDS     0x02 /* VL + BCD seconds */
#define PCF8563_REG_MINUTES        0x03 /* BCD minutes */
#define PCF8563_REG_HOURS          0x04 /* BCD hours */
#define PCF8563_REG_DAYS           0x05 /* BCD day of month */
#define PCF8563_REG_WEEKDAYS       0x06 /* Weekday 0..6 */
#define PCF8563_REG_CENTURY_MONTHS 0x07 /* Century + BCD month */
#define PCF8563_REG_YEARS          0x08 /* BCD year 0..99 */
#define PCF8563_REG_MINUTE_ALARM   0x09 /* Minute alarm */
#define PCF8563_REG_HOUR_ALARM     0x0a /* Hour alarm */
#define PCF8563_REG_DAY_ALARM      0x0b /* Day alarm */
#define PCF8563_REG_WEEKDAY_ALARM  0x0c /* Weekday alarm */
#define PCF8563_REG_CLKOUT         0x0d /* CLKOUT control */

/* CLKOUT_control (0Dh) bits. */

#define PCF8563_CLKOUT_FE      (1 << 7) /* Frequency enable (0=off/hi-Z) */
#define PCF8563_CLKOUT_FD_MASK 0x03     /* Frequency select FD[1:0] */

/* Control/status 1 (00h) bits. */

#define PCF8563_CS1_STOP (1 << 5) /* Stop RTC clock */

/* Control/status 2 (01h) bits. */

#define PCF8563_CS2_TI_TP (1 << 4) /* Timer INT pulse mode (unused here) */
#define PCF8563_CS2_AF    (1 << 3) /* Alarm flag (write 0 to clear) */
#define PCF8563_CS2_TF    (1 << 2) /* Timer flag (write 0 to clear) */
#define PCF8563_CS2_AIE   (1 << 1) /* Alarm interrupt enable */
#define PCF8563_CS2_TIE   (1 << 0) /* Timer interrupt enable (unused here) */

/* Alarm registers (09h..0Ch) "enable" bits.  Writing 0 enables that field
 * to participate in the alarm comparison; writing 1 disables it.
 */

#define PCF8563_AE_M (1 << 7) /* Minute alarm enable (0=enabled) */
#define PCF8563_AE_H (1 << 7) /* Hour alarm enable */
#define PCF8563_AE_D (1 << 7) /* Day alarm enable */
#define PCF8563_AE_W (1 << 7) /* Weekday alarm enable */

/* VL_seconds (02h) bit. */

#define PCF8563_VL_SECONDS_VL   (1 << 7) /* Clock integrity not guaranteed */
#define PCF8563_VL_SECONDS_MASK 0x7f     /* BCD seconds */

/* Century_months (07h) bit. */

#define PCF8563_CM_CENTURY (1 << 7) /* Century flag */

/* Field masks for BCD registers (bit position of "tens" digit varies). */

#define PCF8563_HOURS_MASK    0x3f /* bits 5..0: tens(2bits)+units */
#define PCF8563_DAYS_MASK     0x3f /* bits 5..0 */
#define PCF8563_WEEKDAYS_MASK 0x07 /* bits 2..0 */
#define PCF8563_MONTHS_MASK   0x1f /* bits 4..0 */

/* Number of time/date registers read in one burst (02h .. 08h). */

#define PCF8563_TIMEREGS 7

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* State of the PCF8563 chip.  Only a single RTC is supported. */

struct pcf8563_dev_s
{
  FAR struct i2c_master_s *i2c; /* Contained I2C bus driver reference */

#ifdef CONFIG_RTC_ALARM
  /* Alarm state.  setalarm(), cancelalarm() and the alarm worker share the
   * callback/private pointers and the status-register updates; 'lock'
   * serializes all of them so a cancel cannot race a queued callback and a
   * setalarm cannot race a running worker.  'pending_lock' makes the
   * ISR-side test-and-set of alarm_pending atomic on SMP (the GPIO interrupt
   * may be delivered on any CPU and race the worker's own clear of the
   * latch); it is a spinlock because pcf8563_alarm_service() runs in
   * interrupt context where sleeping is not allowed.
   */

  mutex_t lock;                  /* Serializes alarm state & register writes */
  spinlock_t pending_lock;       /* Guards alarm_pending test-and-set (ISR) */
  rtc_alarm_callback_t alarm_cb; /* Upper-half alarm callback */
  FAR void *alarm_priv;          /* Opaque arg for the callback */
  struct work_s work;            /* Deferred alarm handling work */
  volatile bool alarm_pending;   /* True: an alarm service is queued */
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The rtc_lowerhalf_s is the container the upper half binds to.  It must
 * be the first member so the cast is compatible; the per-instance state
 * follows it.
 */

struct pcf8563_lower_s
{
  struct rtc_lowerhalf_s lower; /* Must be first */
  struct pcf8563_dev_s dev;     /* Private driver state */
};

/* The single RTC instance. */

static struct pcf8563_lower_s g_pcf8563;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pcf8563_bin2bcd
 *
 * Description:
 *   Convert a binary value (0..99) to packed BCD.
 *
 ****************************************************************************/

static uint8_t pcf8563_bin2bcd(unsigned int value)
{
  return (uint8_t)(((value / 10) << 4) | (value % 10));
}

/****************************************************************************
 * Name: pcf8563_bcd2bin
 *
 * Description:
 *   Convert a packed BCD value to binary.
 *
 ****************************************************************************/

static unsigned int pcf8563_bcd2bin(uint8_t value)
{
  return (unsigned int)(((value >> 4) * 10) + (value & 0x0f));
}

/****************************************************************************
 * Name: pcf8563_validate_time
 *
 * Description:
 *   Validate a broken-out time's fields against the ranges this chip can
 *   represent, and (unlike a bare 1..31 day check) reject impossible dates
 *   such as Feb 30 or Apr 31 by comparing the day against the actual length
 *   of the target month, accounting for leap-year February.
 *
 *   The PCF8563 stores time/date as BCD with a two-digit year plus a century
 *   bit, so the representable year window is 1900..2099, mirrored here via
 *   tm_year (0..199, years since 1900).
 *
 * Input Parameters:
 *   tm         - Broken-out time to validate (struct tm / struct rtc_time
 *                are cast-compatible for this purpose).
 *   check_wday - Also validate tm_wday (0..6).  The alarm path does not set
 *                or consume weekday, so it passes false.
 *
 * Returned Value:
 *   Zero (OK) on success; -EINVAL if any field is out of range or the day
 *   exceeds the length of its month.
 *
 ****************************************************************************/

static int pcf8563_validate_time(FAR const struct tm *tm, bool check_wday)
{
  static const uint8_t month_days[12] = { 31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31 };

  uint8_t max_day;

  if (tm->tm_sec < 0 || tm->tm_sec > 59 || tm->tm_min < 0 || tm->tm_min > 59 ||
      tm->tm_hour < 0 || tm->tm_hour > 23 || tm->tm_mday < 1 ||
      tm->tm_mday > 31 || tm->tm_mon < 0 || tm->tm_mon > 11 ||
      tm->tm_year < 0 || tm->tm_year > 199)
    {
      return -EINVAL;
    }

  if (check_wday && (tm->tm_wday < 0 || tm->tm_wday > 6))
    {
      return -EINVAL;
    }

  /* Reject impossible dates (e.g. Feb 30, Apr 31) that the bare 1..31
   * check above still accepts: compare the day against the length of the
   * target month, extending February to 29 on leap years.
   */

  max_day = month_days[tm->tm_mon];
  if (tm->tm_mon == 1)
    {
      int year = tm->tm_year + 1900;

      if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
        {
          max_day = 29;
        }
    }

  if (tm->tm_mday > max_day)
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: pcf8563_read_reg
 *
 * Description:
 *   Read one or more consecutive registers starting at regaddr.
 *
 * Input Parameters:
 *   regaddr - First register address to read.
 *   buffer  - Destination buffer.
 *   buflen  - Number of registers to read.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

static int pcf8563_read_reg(uint8_t regaddr, FAR uint8_t *buffer, int buflen)
{
  struct i2c_msg_s msg[2];

  /* Point the register pointer at regaddr. */

  msg[0].frequency = CONFIG_PCF8563_I2C_FREQUENCY;
  msg[0].addr = PCF8563_I2C_ADDRESS;
  msg[0].flags = 0;
  msg[0].buffer = &regaddr;
  msg[0].length = 1;

  /* Read buflen consecutive registers in one burst. */

  msg[1].frequency = CONFIG_PCF8563_I2C_FREQUENCY;
  msg[1].addr = PCF8563_I2C_ADDRESS;
  msg[1].flags = I2C_M_READ;
  msg[1].buffer = buffer;
  msg[1].length = buflen;

  return I2C_TRANSFER(g_pcf8563.dev.i2c, msg, 2);
}

/****************************************************************************
 * Name: pcf8563_write_reg
 *
 * Description:
 *   Write one or more consecutive registers starting at regaddr.  The
 *   buffer's first byte is the register address, followed by the data.
 *
 * Input Parameters:
 *   regaddr - First register address to write.
 *   buffer  - Source buffer (register address + data).  buffer[0] must
 *             already equal regaddr.
 *   buflen  - Number of bytes to write (1 + number of data registers).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

static int pcf8563_write_reg(uint8_t regaddr, FAR const uint8_t *buffer,
                             int buflen)
{
  struct i2c_msg_s msg[1];

  msg[0].frequency = CONFIG_PCF8563_I2C_FREQUENCY;
  msg[0].addr = PCF8563_I2C_ADDRESS;
  msg[0].flags = 0;
  msg[0].buffer = (FAR uint8_t *)buffer;
  msg[0].length = buflen;

  return I2C_TRANSFER(g_pcf8563.dev.i2c, msg, 1);
}

/****************************************************************************
 * Name: pcf8563_get_datetime
 *
 * Description:
 *   Read the time/date registers (02h..08h) in a single burst access and
 *   convert them to a broken-out struct rtc_time.
 *
 ****************************************************************************/

static int pcf8563_get_datetime(FAR struct rtc_time *rtctime)
{
  uint8_t buffer[PCF8563_TIMEREGS];
  unsigned int year;
  int ret;

  /* Read all seven time/date registers (02h..08h) in one burst. */

  ret = pcf8563_read_reg(PCF8563_REG_VL_SECONDS, buffer, PCF8563_TIMEREGS);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_read_reg failed: %d\n", ret);
      return ret;
    }

  /* XX: buffer[0] = VL_seconds, buffer[5] = century/months. */

  rtctime->tm_sec = (int)pcf8563_bcd2bin(buffer[0] & PCF8563_VL_SECONDS_MASK);
  rtctime->tm_min = (int)pcf8563_bcd2bin(buffer[1]);
  rtctime->tm_hour = (int)pcf8563_bcd2bin(buffer[2] & PCF8563_HOURS_MASK);
  rtctime->tm_mday = (int)pcf8563_bcd2bin(buffer[3] & PCF8563_DAYS_MASK);
  rtctime->tm_wday = (int)pcf8563_bcd2bin(buffer[4] & PCF8563_WEEKDAYS_MASK);
  rtctime->tm_mon = (int)pcf8563_bcd2bin(buffer[5] & PCF8563_MONTHS_MASK) - 1;

  /* Century flag (bit 7 of the century/months register): per the datasheet
   * the C bit toggles when the two-digit year wraps 99->00.
   * C=0 means 20xx and C=1 means 19xx, keeping tm_year correct over
   * the 1900-2099 range the part can represent (an inverted mapping
   * would mislabel e.g. 2024 as 2124).
   */

  if (buffer[5] & PCF8563_CM_CENTURY)
    {
      year = 1900 + pcf8563_bcd2bin(buffer[6]); /* 19xx */
    }
  else
    {
      year = 2000 + pcf8563_bcd2bin(buffer[6]); /* 20xx */
    }

  rtctime->tm_year = (int)(year - 1900); /* years since 1900 */

  /* Unused fields. */

  rtctime->tm_yday = 0;
  rtctime->tm_isdst = 0;

  return OK;
}

/****************************************************************************
 * Name: pcf8563_set_datetime
 *
 * Description:
 *   Convert a broken-out struct rtc_time and write the time/date registers
 *   (02h..08h) in a single burst access.
 *
 ****************************************************************************/

static int pcf8563_set_datetime(FAR const struct rtc_time *rtctime)
{
  uint8_t buffer[1 + PCF8563_TIMEREGS];
  unsigned int year;
  int ret;

  /* Validate ranges (including month length) before packing. */

  if (pcf8563_validate_time((FAR const struct tm *)rtctime, true) < 0)
    {
      return -EINVAL;
    }

  /* First byte is the register address (02h = VL_seconds).  Writing the
   * VL_seconds register also clears the VL flag (bit 7 written as 0).
   */

  buffer[0] = PCF8563_REG_VL_SECONDS;

  /* BCD seconds.  Write bit 7 = 0 so the VL flag is cleared. */

  buffer[1] =
      pcf8563_bin2bcd((unsigned int)rtctime->tm_sec) & PCF8563_VL_SECONDS_MASK;

  /* BCD minutes. */

  buffer[2] = pcf8563_bin2bcd((unsigned int)rtctime->tm_min);

  /* BCD hours (24-hour format). */

  buffer[3] = pcf8563_bin2bcd((unsigned int)rtctime->tm_hour);

  /* BCD day of month. */

  buffer[4] = pcf8563_bin2bcd((unsigned int)rtctime->tm_mday);

  /* Weekday (0..6). */

  buffer[5] = (uint8_t)(rtctime->tm_wday & PCF8563_WEEKDAYS_MASK);

  /* Month (1..12). */

  buffer[6] = pcf8563_bin2bcd((unsigned int)(rtctime->tm_mon + 1));

  /* Map tm_year (years since 1900) to the two-digit year plus the century
   * bit, matching the convention in pcf8563_get_datetime(): 1900-1999 ->
   * C=1, 2000-2099 -> C=0.
   */

  year = (unsigned int)rtctime->tm_year;
  if (year < 100)
    {
      /* 1900-1999: C=1 */

      buffer[6] |= PCF8563_CM_CENTURY;
      buffer[7] = pcf8563_bin2bcd(year);
    }
  else
    {
      /* 2000-2099: C=0, lower two digits */

      buffer[7] = pcf8563_bin2bcd(year - 100);
    }

  ret =
      pcf8563_write_reg(PCF8563_REG_VL_SECONDS, buffer, 1 + PCF8563_TIMEREGS);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_write_reg failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: pcf8563_rdtime
 *
 * Description:
 *   Read the current time from the RTC.
 *
 ****************************************************************************/

static int pcf8563_rdtime(FAR struct rtc_lowerhalf_s *lower,
                          FAR struct rtc_time *rtctime)
{
  return pcf8563_get_datetime(rtctime);
}

/****************************************************************************
 * Name: pcf8563_settime
 *
 * Description:
 *   Set the RTC time.
 *
 ****************************************************************************/

static int pcf8563_settime(FAR struct rtc_lowerhalf_s *lower,
                           FAR const struct rtc_time *rtctime)
{
  int ret;

  /* Serialize against alarm register access: rdalarm() derives the next
   * alarm fire time from the *current* time plus the programmed alarm
   * fields, so a concurrent change of the time would otherwise race that
   * derivation and produce a day/minute off-by-one.
   */

  nxmutex_lock(&g_pcf8563.dev.lock);
  ret = pcf8563_set_datetime(rtctime);
  nxmutex_unlock(&g_pcf8563.dev.lock);

  return ret;
}

/****************************************************************************
 * Name: pcf8563_havesettime
 *
 * Description:
 *   Check whether the RTC time has been set.
 *
 *   The PCF8563 does not expose a "time has been set" flag.  We use the VL
 *   (voltage-low / clock integrity) bit as the indicator: if VL is set, the
 *   oscillator stopped or VDD dropped below Vlow, so the time is not
 *   trustworthy; once a valid time has been written (which clears VL), we
 *   report true.  On a fresh part VL is 1 after reset.
 *
 ****************************************************************************/

static bool pcf8563_havesettime(FAR struct rtc_lowerhalf_s *lower)
{
  uint8_t vlseconds;
  int ret;

  ret = pcf8563_read_reg(PCF8563_REG_VL_SECONDS, &vlseconds, 1);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_read_reg (VL) failed: %d\n", ret);
      return false;
    }

  return (vlseconds & PCF8563_VL_SECONDS_VL) == 0;
}

#ifdef CONFIG_RTC_ALARM
/****************************************************************************
 * Name: pcf8563_setalarm_internal
 *
 * Description:
 *   Program the alarm registers and enable/arm the alarm interrupt.  The
 *   caller MUST already hold g_pcf8563.dev.lock; this function neither locks
 *   nor unlocks.
 *
 *   The PCF8563 compares each *enabled* alarm field against the current time
 *   and raises the alarm flag once all match.  We enable the minute, hour
 *   and day fields and leave the weekday field disabled.  (The PCF8563 has
 *   no month alarm field, so day-of-month + time is the closest
 *   representation of a one-shot absolute alarm.)
 *
 *   The alarm target must lie within PCF8563_ALARM_MAX_SECONDS (28 days) of
 *   'now'; requests further out (or in the past) are rejected with -ERANGE.
 *   Within that window the target day-of-month occurs exactly once, so the
 *   alarm cannot fire early at a stale intermediate month.
 *
 * Input Parameters:
 *   lower     - Lower half state (unused here, kept for symmetry).
 *   alarminfo - Absolute alarm time + callback + private arg.
 *   now       - Current time snapshot, or NULL to read it now (still under
 *               the caller-held lock).
 *
 ****************************************************************************/

static int
pcf8563_setalarm_internal(FAR struct rtc_lowerhalf_s *lower,
                          FAR const struct lower_setalarm_s *alarminfo,
                          FAR const struct rtc_time *now)
{
  uint8_t regs[4 + 1];
  struct rtc_time now_local;
  struct tm now_tm;
  struct tm target_tm;
  time_t now_t;
  time_t target_t;
  time_t delta;
  int ret;

  /* The caller may already have read the current time (setrelative reuses
   * it), otherwise read it here; both paths run under the caller-held lock
   * so the snapshot is stable.
   */

  if (now == NULL)
    {
      ret = pcf8563_get_datetime(&now_local);
      if (ret < 0)
        {
          rtcerr("ERROR: pcf8563_get_datetime (setalarm) failed: %d\n", ret);
          return ret;
        }

      now = &now_local;
    }

  /* Reject targets that cannot be represented unambiguously by the
   * minute/hour/day comparison: compute the offset from "now" and require it
   * to be strictly within the 28-day window.  A target in the past (negative
   * offset) would immediately match an already-passed minute and fire early,
   * so it is rejected too.
   */

  /* timegm() normalizes its argument in place, so convert local copies to
   * avoid corrupting the caller's 'now' snapshot and the const
   * alarminfo->time (which would otherwise be written through a cast).
   */

  now_tm = *(FAR struct tm *)now;
  target_tm = *(FAR struct tm *)&alarminfo->time;

  /* The RTC upper half does not validate the alarm fields, so reject any
   * out-of-range component (and impossible dates such as Feb 30) before
   * timegm() silently normalizes it (e.g. a tm_sec of 60 would be carried
   * into the next minute and then round up once more, programming two
   * minutes past the intent).  Validate the full target, including the
   * 1900-2099 year window.
   */

  if (pcf8563_validate_time(&target_tm, false) < 0)
    {
      return -EINVAL;
    }

  /* The PCF8563 alarm compares only minute/hour/day; seconds never
   * participate (AF is asserted at the increment to the programmed minute,
   * i.e. at second 0).  A target with a nonzero second component is rounded
   * UP to the next minute so the alarm never fires early: it fires at the
   * next minute boundary, up to 59 s after the sub-minute target.  This is
   * done here in the one shared path, so both the absolute (setalarm) and
   * relative (setrelative) entries are clamped identically.
   */

  if (target_tm.tm_sec != 0)
    {
      target_t = timegm(&target_tm);
      target_t = target_t - target_tm.tm_sec + 60;

      if (gmtime_r(&target_t, &target_tm) == NULL)
        {
          return -EINVAL;
        }
    }

  now_t = timegm(&now_tm);
  target_t = timegm(&target_tm);

  delta = target_t - now_t;
  if (delta <= 0 || delta > PCF8563_ALARM_MAX_SECONDS)
    {
      rtcerr("ERROR: pcf8563_setalarm target out of range (delta=%lld)\n",
             (long long)delta);
      return -ERANGE;
    }

  /* Register base address 09h, followed by the four alarm registers:
   *   [0] 09h minute_alarm   (AE_M + BCD minute)
   *   [1] 0ah hour_alarm     (AE_H + BCD hour)
   *   [2] 0bh day_alarm      (AE_D + BCD day)
   *   [3] 0ch weekday_alarm  (AE_W, disabled)
   */

  regs[0] = PCF8563_REG_MINUTE_ALARM;
  regs[1] = pcf8563_bin2bcd((unsigned int)target_tm.tm_min);
  regs[2] = pcf8563_bin2bcd((unsigned int)target_tm.tm_hour);
  regs[3] = pcf8563_bin2bcd((unsigned int)target_tm.tm_mday);
  regs[4] = PCF8563_AE_W; /* Weekday alarm disabled */

  ret = pcf8563_write_reg(PCF8563_REG_MINUTE_ALARM, regs, sizeof(regs));
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_write_reg (alarm) failed: %d\n", ret);
      return ret;
    }

  /* Enable the alarm interrupt (AIE) and clear any stale alarm flag (AF),
   * preserving the timer state.  A raw whole-byte write to Control/status 2
   * is wrong: the PCF8563 performs a logic AND on register writes (a 1 keeps
   * the bit, a 0 clears it) to protect the AF/TF flags, so writing a fixed
   * byte would (a) never set AIE if it was previously 0, and (b) clobber an
   * active timer (TIE/TF).  Do a read-modify-write instead: read the current
   * value, set AIE, clear only AF, and leave TIE/TF/TI_TP untouched.
   */

  {
    uint8_t status2;

    ret = pcf8563_read_reg(PCF8563_REG_CTRL_STATUS2, &status2, 1);
    if (ret < 0)
      {
        rtcerr("ERROR: pcf8563_read_reg (status2) failed: %d\n", ret);
        return ret;
      }

    status2 |= PCF8563_CS2_AIE; /* Enable alarm interrupt */
    status2 &= ~PCF8563_CS2_AF; /* Clear stale alarm flag */

    {
      uint8_t data[2] = { PCF8563_REG_CTRL_STATUS2, status2 };

      ret = pcf8563_write_reg(data[0], data, sizeof(data));
      if (ret < 0)
        {
          rtcerr("ERROR: pcf8563_write_reg (AIE) failed: %d\n", ret);
          return ret;
        }
    }
  }

  /* Vector the alarm callback through the compacted lower-half state.  The
   * callback is invoked later from the worker (thread context).
   */

  g_pcf8563.dev.alarm_cb = alarminfo->cb;
  g_pcf8563.dev.alarm_priv = alarminfo->priv;

  return OK;
}

/****************************************************************************
 * Name: pcf8563_setalarm
 *
 * Description:
 *   Set a one-shot absolute alarm.  Acquires the lock, then delegates to
 *   pcf8563_setalarm_internal() with 'now' = NULL (read under lock).
 *
 ****************************************************************************/

static int pcf8563_setalarm(FAR struct rtc_lowerhalf_s *lower,
                            FAR const struct lower_setalarm_s *alarminfo)
{
  int ret;

  nxmutex_lock(&g_pcf8563.dev.lock);
  ret = pcf8563_setalarm_internal(lower, alarminfo, NULL);
  nxmutex_unlock(&g_pcf8563.dev.lock);

  return ret;
}

/****************************************************************************
 * Name: pcf8563_setrelative
 *
 * Description:
 *   Set an alarm relative to the current time, then program the hardware
 *   via pcf8563_setalarm().
 *
 ****************************************************************************/

static int pcf8563_setrelative(FAR struct rtc_lowerhalf_s *lower,
                               FAR const struct lower_setrelative_s *alarminfo)
{
  struct lower_setalarm_s lalarm;
  struct rtc_time rtctime;
  struct tm setalarm;
  time_t time;
  int ret;

  /* Reject relative targets outside the representable window early: the
   * alarm compares only minute/hour/day, so anything beyond 28 days (or a
   * non-positive offset) cannot fire at a well-defined single date.  A
   * non-positive reltime would program an already-passed minute and fire
   * immediately/stale.
   */

  if (alarminfo->reltime <= 0 ||
      alarminfo->reltime > PCF8563_ALARM_MAX_SECONDS)
    {
      rtcerr("ERROR: pcf8563_setrelative reltime out of range (%lld)\n",
             (long long)alarminfo->reltime);
      return -ERANGE;
    }

  /* Read the current time and program the target under one lock so the
   * read and the register write are a single stable snapshot.
   */

  nxmutex_lock(&g_pcf8563.dev.lock);

  ret = pcf8563_get_datetime(&rtctime);
  if (ret < 0)
    {
      nxmutex_unlock(&g_pcf8563.dev.lock);
      return ret;
    }

  /* Convert the broken-out time to a time_t, add the relative offset, then
   * convert back to broken-out time.  struct rtc_time is cast-compatible
   * with struct tm for timegm()/gmtime_r().
   */

  time = timegm((FAR struct tm *)&rtctime);
  time += alarminfo->reltime;

  if (gmtime_r(&time, &setalarm) == NULL)
    {
      nxmutex_unlock(&g_pcf8563.dev.lock);
      return -EINVAL;
    }

  /* Any nonzero second component of the computed target is rounded up to the
   * next minute inside pcf8563_setalarm_internal(), the single shared
   * clamping point for both absolute and relative alarms.
   */

  /* Reuse the already-read current time (rtctime) as the delta annotation
   * for pcf8563_setalarm_internal(), avoiding a second I2C read.
   */

  lalarm.id = alarminfo->id;
  lalarm.cb = alarminfo->cb;
  lalarm.priv = alarminfo->priv;
  lalarm.time = *(FAR struct rtc_time *)&setalarm;

  ret = pcf8563_setalarm_internal(lower, &lalarm, &rtctime);

  nxmutex_unlock(&g_pcf8563.dev.lock);

  return ret;
}

/****************************************************************************
 * Name: pcf8563_cancelalarm
 *
 * Description:
 *   Cancel the currently armed alarm and disable its interrupt.
 *
 ****************************************************************************/

static int pcf8563_cancelalarm(FAR struct rtc_lowerhalf_s *lower, int alarmid)
{
  uint8_t regs[5];
  int ret;

  nxmutex_lock(&g_pcf8563.dev.lock);

  regs[0] = PCF8563_REG_MINUTE_ALARM;

  /* Disable all four alarm fields (AE_x = 1). */

  regs[1] = PCF8563_AE_M | 0x00; /* minute disabled */
  regs[2] = PCF8563_AE_H;        /* hour disabled */
  regs[3] = PCF8563_AE_D;        /* day disabled */
  regs[4] = PCF8563_AE_W;        /* weekday disabled */

  ret = pcf8563_write_reg(PCF8563_REG_MINUTE_ALARM, regs, sizeof(regs));
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_write_reg (alarm clear) failed: %d\n", ret);
      nxmutex_unlock(&g_pcf8563.dev.lock);
      return ret;
    }

  /* Disable the alarm interrupt (AIE) and clear any pending alarm flag (AF),
   * preserving the timer state.  As in setalarm(), a raw whole-byte write is
   * wrong for the same AND-write reason: writing 0 would also clear TIE/TF
   * and break a running timer.  Read-modify-write instead keeps TIE/TF.
   */

  {
    uint8_t status2;

    ret = pcf8563_read_reg(PCF8563_REG_CTRL_STATUS2, &status2, 1);
    if (ret < 0)
      {
        rtcerr("ERROR: pcf8563_read_reg (status2) failed: %d\n", ret);
        nxmutex_unlock(&g_pcf8563.dev.lock);
        return ret;
      }

    status2 &= ~(PCF8563_CS2_AIE | PCF8563_CS2_AF); /* Disable, clear AF */

    {
      uint8_t data[2] = { PCF8563_REG_CTRL_STATUS2, status2 };

      ret = pcf8563_write_reg(data[0], data, sizeof(data));
      if (ret < 0)
        {
          rtcerr("ERROR: pcf8563_write_reg (AIE clear) failed: %d\n", ret);
          nxmutex_unlock(&g_pcf8563.dev.lock);
          return ret;
        }
    }
  }

  /* Drop the callback reference. */

  g_pcf8563.dev.alarm_cb = NULL;
  g_pcf8563.dev.alarm_priv = NULL;

  nxmutex_unlock(&g_pcf8563.dev.lock);

  return OK;
}

/****************************************************************************
 * Name: pcf8563_rdalarm
 *
 * Description:
 *   Query the currently programmed alarm.
 *
 ****************************************************************************/

static int pcf8563_rdalarm(FAR struct rtc_lowerhalf_s *lower,
                           FAR struct lower_rdalarm_s *alarminfo)
{
  struct rtc_time now;
  struct tm candidate;
  uint8_t buffer[4];
  time_t now_t;
  time_t cand_t;
  int amin;
  int ahour;
  int aday;
  int ret;

  /* The PCF8563 alarm has no month/year compare registers, so the registers
   * only tell us minute/hour/day.  We rebuild the complete next fire time by
   * combining those field values with the *current* date: the alarm fires at
   * the next occurrence (this month, or next month if already past) of the
   * programmed day + hour + minute.  The current time and the alarm
   * registers must be read under the same lock so a concurrent settime() or
   * time rollover cannot skew the result by a day or a minute.
   */

  nxmutex_lock(&g_pcf8563.dev.lock);

  ret = pcf8563_get_datetime(&now);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_get_datetime (rdalarm) failed: %d\n", ret);
      nxmutex_unlock(&g_pcf8563.dev.lock);
      return ret;
    }

  ret = pcf8563_read_reg(PCF8563_REG_MINUTE_ALARM, buffer, 4);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_read_reg (alarm) failed: %d\n", ret);
      nxmutex_unlock(&g_pcf8563.dev.lock);
      return ret;
    }

  amin = (int)pcf8563_bcd2bin(buffer[0] & ~PCF8563_AE_M);
  ahour = (int)pcf8563_bcd2bin(buffer[1] & ~PCF8563_AE_H);
  aday = (int)pcf8563_bcd2bin(buffer[2] & ~PCF8563_AE_D);

  nxmutex_unlock(&g_pcf8563.dev.lock);

  /* Build this month's candidate as year/month/day + hour:min, with seconds
   * forced to 0 (the alarm asserts at the top of the target minute).  The
   * alarm fields carry no month/year, so we anchor the candidate to the
   * current month/year and resolve "already passed" below by rolling forward
   * one month.  struct rtc_time in this driver holds UTC (see
   * pcf8563_get_datetime), so timegm() (which treats fields as UTC and
   * ignores tm_isdst) is used rather than local-time mktime().
   */

  memset(&candidate, 0, sizeof(candidate));
  candidate.tm_year = now.tm_year;
  candidate.tm_mon = now.tm_mon;
  candidate.tm_mday = aday;
  candidate.tm_hour = ahour;
  candidate.tm_min = amin;
  candidate.tm_sec = 0;

  /* timegm() normalizes out-of-range fields: a day larger than the current
   * month's length overflows into the following month, and a day already
   * passed this month is handled by the explicit one-month roll forward
   * below.
   */

  now_t = timegm((FAR struct tm *)&now);
  cand_t = timegm(&candidate);

  /* If the candidate is strictly before the current time, the alarm fires
   * next month instead (timegm() normalizes a month overflow, carrying to
   * the next year when crossing December).
   */

  if (cand_t < now_t)
    {
      candidate.tm_mon += 1;
      cand_t = timegm(&candidate);
    }

  /* Rebuild the full broken-out time from the normalized epoch value. */

  if (gmtime_r(&cand_t, &candidate) == NULL)
    {
      return -EINVAL;
    }

  alarminfo->time->tm_sec = candidate.tm_sec;
  alarminfo->time->tm_min = candidate.tm_min;
  alarminfo->time->tm_hour = candidate.tm_hour;
  alarminfo->time->tm_mday = candidate.tm_mday;
  alarminfo->time->tm_mon = candidate.tm_mon;
  alarminfo->time->tm_year = candidate.tm_year;
  alarminfo->time->tm_wday = candidate.tm_wday;
  alarminfo->time->tm_yday = candidate.tm_yday;
  alarminfo->time->tm_isdst = candidate.tm_isdst;

  return OK;
}

/****************************************************************************
 * Name: pcf8563_alarm_worker
 *
 * Description:
 *   Deferred worker that handles a PCF8563 alarm interrupt.  Runs in the
 *   high-priority work queue (process/thread context) where I2C transfers
 *   are safe.
 *
 *   It reads the control/status register to confirm the alarm flag (AF) is
 *   set, clears AF, and invokes the upper-half alarm callback so the RTC
 *   upper half can signal the waiting task.
 *
 ****************************************************************************/

static void pcf8563_alarm_release_pending_latch(void)
{
  irqstate_t flags;

  /* The latch is shared with the ISR-side test-and-set, so it must be
   * cleared atomically on SMP.
   */

  flags = spin_lock_irqsave(&g_pcf8563.dev.pending_lock);
  g_pcf8563.dev.alarm_pending = false;
  spin_unlock_irqrestore(&g_pcf8563.dev.pending_lock, flags);
}

static void pcf8563_alarm_worker(FAR void *arg)
{
  uint8_t status2;
  uint8_t regs[5];
  rtc_alarm_callback_t cb;
  FAR void *priv;
  bool need_retry;
  int ret;

  /* Serialize register access and callback state with setalarm() /
   * cancelalarm().  The upper-half callback is invoked after the lock is
   * released so it may itself re-arm or cancel the alarm.
   */

  nxmutex_lock(&g_pcf8563.dev.lock);

  /* Read Control/status 2. */

  ret = pcf8563_read_reg(PCF8563_REG_CTRL_STATUS2, &status2, 1);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_read_reg (status2) failed: %d\n", ret);

      /* AF may still be asserted, holding the INT pin low.  With the
       * falling-edge board interrupt no new edge will re-enter the ISR, so
       * must not just drop the latch: stall it and retry shortly.
       */

      need_retry = true;
      goto out;
    }

  /* Was the alarm flag set?  If not, nothing to do (e.g. a spurious or
   * software-induced interrupt edge).
   */

  if ((status2 & PCF8563_CS2_AF) == 0)
    {
      need_retry = false;
      goto out;
    }

  /* First (one-shot) alarm fire.  The PCF8563 re-asserts AF and keeps INT
   * active every time the clock matches the programmed alarm fields, so to
   * honor the one-shot NuttX RTC model we must, while handling this fire:
   *   - clear AF and disable AIE (status2 read-modify-write, keeping
   *     TIE/TF/TI_TP), releasing the INT pin;
   *   - disable all alarm fields (AE_x = 1) so a later match cannot
   *     re-trigger;
   *   - drop the callback so it is not invoked again.
   */

  status2 &= ~(PCF8563_CS2_AF | PCF8563_CS2_AIE);
  {
    uint8_t data[2] = { PCF8563_REG_CTRL_STATUS2, status2 };

    ret = pcf8563_write_reg(data[0], data, sizeof(data));
    if (ret < 0)
      {
        rtcerr("ERROR: pcf8563_write_reg (clear AF/AIE) failed: %d\n", ret);

        /* AF/AIE are still set, so INT stays low (falling-edge board) and no
         * new edge will ever come.  Stall the latch and retry shortly.
         */

        need_retry = true;
        goto out;
      }
  }

  /* From here the fire has been acknowledged and consumed: AF is cleared and
   * AIE disabled, so INT is released and no notification can be re-triggered.
   * Snapshot and drop the callback now so a later register error cannot
   * swallow the upper-half notification below.
   */

  cb = g_pcf8563.dev.alarm_cb;
  priv = g_pcf8563.dev.alarm_priv;
  g_pcf8563.dev.alarm_cb = NULL;
  g_pcf8563.dev.alarm_priv = NULL;

  /* Disable all four alarm fields (AE_x = 1) so the one-shot alarm cannot
   * fire again on a later (e.g. next minute/day) match.
   */

  regs[0] = PCF8563_REG_MINUTE_ALARM;
  regs[1] = PCF8563_AE_M;
  regs[2] = PCF8563_AE_H;
  regs[3] = PCF8563_AE_D;
  regs[4] = PCF8563_AE_W;

  ret = pcf8563_write_reg(PCF8563_REG_MINUTE_ALARM, regs, sizeof(regs));
  if (ret < 0)
    {
      /* The alarm is already consumed (AF/AIE cleared), so this cannot cause
       * a repeated notification.  The only residual risk is re-matching the
       * fields on a later minute/day, which is benign and erased by the next
       * setalarm().  Log and continue to deliver the notification.
       */

      rtcerr("ERROR: pcf8563_write_reg (alarm disable) failed, "
             "notification still delivered: %d\n",
             ret);
    }

  need_retry = false;

  nxmutex_unlock(&g_pcf8563.dev.lock);

  /* The fire is fully consumed: release the pending latch.  Notify the upper
   * half afterwards; the alarm is one-shot and will not re-fire until
   * setalarm() is called again.
   */

  pcf8563_alarm_release_pending_latch();

  if (cb != NULL)
    {
      cb(priv, 0);
    }

  return;

out:
  nxmutex_unlock(&g_pcf8563.dev.lock);

  if (need_retry)
    {
      /* The alarm is not yet consumed (AF/AIE still set, INT held low) and
       * the board interrupt is falling-edge, so no new edge will re-enter
       * the ISR.  Keep the latch asserted to stop the ISR stacking further
       * workers, and re-queue ourselves after a short delay to retry the
       * (typically transient) I2C failure; otherwise the alarm would be
       * dropped forever.
       */

      ret = work_queue(HPWORK, &g_pcf8563.dev.work, pcf8563_alarm_worker, NULL,
                       MSEC2TICK(PCF8563_ALARM_RETRY_DELAY_MSEC));
      if (ret == OK)
        {
          return;
        }
    }

  pcf8563_alarm_release_pending_latch();
}

/****************************************************************************
 * Name: pcf8563_alarm_service
 *
 * Description:
 *   Alarm interrupt service entry point, called from board logic (the GPIO
 *   interrupt wired to the PCF8563 INT pin).  Only schedules the worker;
 *   all I2C work happens in pcf8563_alarm_worker().
 *
 ****************************************************************************/

void pcf8563_alarm_service(FAR void *arg)
{
  irqstate_t flags;
  int ret;

  /* Test-and-set the pending latch atomically: on SMP the GPIO interrupt
   * may be delivered on any CPU and race the worker's own clear of the
   * latch, so a plain volatile read-modify-write is not sufficient.
   * spin_lock_irqsave() is safe here (interrupt context; the critical
   * section is only a boolean).
   */

  flags = spin_lock_irqsave(&g_pcf8563.dev.pending_lock);
  if (g_pcf8563.dev.alarm_pending)
    {
      spin_unlock_irqrestore(&g_pcf8563.dev.pending_lock, flags);
      return;
    }

  g_pcf8563.dev.alarm_pending = true;
  spin_unlock_irqrestore(&g_pcf8563.dev.pending_lock, flags);

  ret = work_queue(HPWORK, &g_pcf8563.dev.work, pcf8563_alarm_worker, NULL, 0);
  if (ret < 0)
    {
      /* Enqueue failed: release the latch so a later interrupt can retry;
       * otherwise alarm_pending stays set forever and every subsequent
       * alarm would be silently dropped.
       */

      pcf8563_alarm_release_pending_latch();
    }
}
#endif /* CONFIG_RTC_ALARM */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The lower-half operations vtable.  Unsupported methods are left NULL. */

static const struct rtc_ops_s g_pcf8563_ops = {
  .rdtime = pcf8563_rdtime,
  .settime = pcf8563_settime,
  .havesettime = pcf8563_havesettime,

#ifdef CONFIG_RTC_ALARM
  .setalarm = pcf8563_setalarm,
  .setrelative = pcf8563_setrelative,
  .cancelalarm = pcf8563_cancelalarm,
  .rdalarm = pcf8563_rdalarm,
#endif

#ifdef CONFIG_RTC_PERIODIC
/* TODO: .setperiodic / .cancelperiodic */
#endif

#ifdef CONFIG_RTC_IOCTL
/* TODO: .ioctl */
#endif

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
/* TODO: .destroy */
#endif
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pcf8563_clkout_set
 *
 * Description:
 *   Configure the PCF8563 programmable clock output (CLKOUT pin, register
 *   0Dh).
 *
 *   This is a driver-layer interface (not exposed via RTC ioctl): the
 *   application does not need it, but another kernel driver may request a
 *   fixed-frequency reference clock (e.g. 32.768 kHz / 1.024 kHz / 32 Hz /
 *   1 Hz) from the chip.
 *
 * Input Parameters:
 *   freq - One of the pcf8563_clkout_freq_e values, or
 *          PCF8563_CLKOUT_DISABLE to turn the output off (hi-Z).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int pcf8563_clkout_set(enum pcf8563_clkout_freq_e freq)
{
  uint8_t regs[2];
  int ret;

  /* CLKOUT_control: [address, data].  Disable does not program a frequency;
   * clear FE and leave FD[1:0] at 0 (32.768 kHz default when re-enabled).
   */

  regs[0] = PCF8563_REG_CLKOUT;
  regs[1] = 0;

  if (freq != PCF8563_CLKOUT_DISABLE)
    {
      regs[1] = PCF8563_CLKOUT_FE | (uint8_t)freq;
    }

  ret = pcf8563_write_reg(regs[0], regs, sizeof(regs));
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_write_reg (CLKOUT) failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: pcf8563_rtc_initialize
 *
 * Description:
 *   Initialize the PCF8563/HYM8563 hardware RTC and register it with the
 *   upper half RTC driver at /dev/rtcN.
 *
 * Input Parameters:
 *   i2c - An I2C master instance the RTC is attached to.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int pcf8563_rtc_initialize(FAR struct i2c_master_s *i2c)
{
  int ret;

  /* Remember the I2C device. */

  g_pcf8563.dev.i2c = i2c;

#ifdef CONFIG_RTC_ALARM
  /* Initialize the alarm state locks: the mutex serializes register/callback
   * updates between threads; the spinlock makes the ISR-side pending latch
   * test-and-set atomic on SMP.
   */

  nxmutex_init(&g_pcf8563.dev.lock);
  spin_lock_init(&g_pcf8563.dev.pending_lock);
#endif

  /* Bind the ops vtable to the lower-half instance. */

  g_pcf8563.lower.ops = &g_pcf8563_ops;

  /* Register with the upper half RTC driver.  Minor number 0 -> /dev/rtc0. */

  ret = rtc_initialize(0, &g_pcf8563.lower);
  if (ret < 0)
    {
      rtcerr("ERROR: rtc_initialize failed: %d\n", ret);
      return ret;
    }

  /* Bridge the lower-half to the arch RTC interface so the clock subsystem
   * can seed and update the system time from this RTC.  When CONFIG_RTC_ARCH
   * is set, at this point the arch-side weak up_rtc_getdatetime/settime and
   * g_rtc_enabled are backed by our lower-half.  The 'sync' flag makes the
   * kernel re-read the wall time right away (external RTC became available
   * only now, after boot).
   */

  up_rtc_set_lowerhalf(&g_pcf8563.lower, true);

  return OK;
}

#endif /* CONFIG_RTC_PCF8563 */
