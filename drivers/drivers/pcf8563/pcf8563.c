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
 *   Consequently a relative-alarm request (RTC_SET_RELATIVE, e.g. "alarm
 *   N") can only be honored to minute accuracy: the seconds component of the
 *   computed absolute target is discarded, so the alarm fires early by
 *   (target seconds) mod 60, i.e. 0..59 seconds before the wall-clock
 *   second anticipated by a sub-minute request.  When the target minute
 *   equals the current minute, the alarm may fire almost immediately (at
 *   the next minute increment).  This is inherent to the part, not a driver
 *   defect.  Use a system timer (sleep/hrtimer/watchdog) if second-accurate
 *   wakeups are required.
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
#include <time.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
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
#define PCF8563_CS2_AF    (1 << 3) /* Alarm flag (write 1 to clear) */
#define PCF8563_CS2_TF    (1 << 2) /* Timer flag (write 1 to clear) */
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
  /* Alarm callback state.  When an alarm is set, the upper half provides a
   * callback + private arg that must be invoked (from a worker, not the raw
   * ISR) when the alarm fires.
   */

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

  /* Years since 1900.  The century bit (C) is inspected to distinguish
   * 20xx from 21xx.  PCF8563 does not hold the full century, only a
   * toggling flag; assume 2000..2099 by default, extending to the next
   * century when the flag is set.
   */

  year = pcf8563_bcd2bin(buffer[6]);
  if (buffer[5] & PCF8563_CM_CENTURY)
    {
      year += 100;
    }

  rtctime->tm_year = (int)year;

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

  /* Validate ranges before packing. */

  if (rtctime->tm_sec < 0 || rtctime->tm_sec > 59 || rtctime->tm_min < 0 ||
      rtctime->tm_min > 59 || rtctime->tm_hour < 0 || rtctime->tm_hour > 23 ||
      rtctime->tm_mday < 1 || rtctime->tm_mday > 31 || rtctime->tm_mon < 0 ||
      rtctime->tm_mon > 11 || rtctime->tm_wday < 0 || rtctime->tm_wday > 6)
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

  /* Month (1..12) + century flag. */

  year = (unsigned int)rtctime->tm_year;
  buffer[6] = pcf8563_bin2bcd((unsigned int)(rtctime->tm_mon + 1));
  if (year >= 100)
    {
      buffer[6] |= PCF8563_CM_CENTURY;
    }

  /* Year (00..99), lower two digits. */

  buffer[7] = pcf8563_bin2bcd(year % 100);

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
  return pcf8563_set_datetime(rtctime);
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
 * Name: pcf8563_setalarm
 *
 * Description:
 *   Set an alarm on the PCF8563.
 *
 *   The PCF8563 compares each *enabled* alarm field against the current time
 *   and raises the alarm flag once all match.  Alarms are one-shot in the
 *   NuttX RTC model (mirroring pl031): it fires once when the time reaches
 *   the programmed minute/hour/day.
 *
 *   We enable the minute, hour and day fields and leave the weekday field
 *   disabled.  (The PCF8563 has no month alarm field, so day-of-month + time
 *   is the closest representation of a one-shot absolute alarm.)  When the
 *   programmed time is reached, AF is set and -- if AIE is enabled -- the
 *   INT pin asserts, which the board routes to pcf8563_alarm_service().
 *
 ****************************************************************************/

static int pcf8563_setalarm(FAR struct rtc_lowerhalf_s *lower,
                            FAR const struct lower_setalarm_s *alarminfo)
{
  uint8_t regs[4 + 1];
  int ret;

  /* Vector the alarm callback through the compacted lower-half state.  The
   * callback is invoked later from the worker (interrupt context).
   */

  g_pcf8563.dev.alarm_cb = alarminfo->cb;
  g_pcf8563.dev.alarm_priv = alarminfo->priv;

  /* Register base address 09h, followed by the four alarm registers:
   *   [0] 09h minute_alarm   (AE_M + BCD minute)
   *   [1] 0ah hour_alarm     (AE_H + BCD hour)
   *   [2] 0bh day_alarm      (AE_D + BCD day)
   *   [3] 0ch weekday_alarm  (AE_W, disabled)
   */

  regs[0] = PCF8563_REG_MINUTE_ALARM;
  regs[1] = pcf8563_bin2bcd((unsigned int)alarminfo->time.tm_min);
  regs[2] = pcf8563_bin2bcd((unsigned int)alarminfo->time.tm_hour);
  regs[3] = pcf8563_bin2bcd((unsigned int)alarminfo->time.tm_mday);
  regs[4] = PCF8563_AE_W; /* Weekday alarm disabled */

  ret = pcf8563_write_reg(PCF8563_REG_MINUTE_ALARM, regs, sizeof(regs));
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_write_reg (alarm) failed: %d\n", ret);
      return ret;
    }

  /* Enable the alarm interrupt (AIE).  Write the register address (01h) as
   * the first byte followed by the new value.  Note: pcf8563_write_reg()
   * treats buffer[0] as the register address and the remaining bytes as
   * data, so the data must be offset by one.
   */

  {
    uint8_t status2[2] = { PCF8563_REG_CTRL_STATUS2, PCF8563_CS2_AIE };

    ret = pcf8563_write_reg(status2[0], status2, sizeof(status2));
    if (ret < 0)
      {
        rtcerr("ERROR: pcf8563_write_reg (AIE) failed: %d\n", ret);
        return ret;
      }
  }

  return OK;
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
  struct rtc_time rtctime;
  struct tm setalarm;
  time_t time;
  int ret;

  /* Read the current time first. */

  ret = pcf8563_get_datetime(&rtctime);
  if (ret < 0)
    {
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
      return -EINVAL;
    }

  {
    struct lower_setalarm_s lalarm;

    lalarm.id = alarminfo->id;
    lalarm.cb = alarminfo->cb;
    lalarm.priv = alarminfo->priv;
    lalarm.time = *(FAR struct rtc_time *)&setalarm;

    return pcf8563_setalarm(lower, &lalarm);
  }
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
      return ret;
    }

  /* Disable the alarm interrupt. */

  regs[0] = PCF8563_REG_CTRL_STATUS2;
  regs[1] = 0;
  ret = pcf8563_write_reg(PCF8563_REG_CTRL_STATUS2, regs, 2);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_write_reg (AIE clear) failed: %d\n", ret);
      return ret;
    }

  /* Drop the callback reference. */

  g_pcf8563.dev.alarm_cb = NULL;
  g_pcf8563.dev.alarm_priv = NULL;

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
  uint8_t buffer[4];
  int ret;

  ret = pcf8563_read_reg(PCF8563_REG_MINUTE_ALARM, buffer, 4);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_read_reg (alarm) failed: %d\n", ret);
      return ret;
    }

  alarminfo->time->tm_min = (int)pcf8563_bcd2bin(buffer[0] & ~PCF8563_AE_M);
  alarminfo->time->tm_hour = (int)pcf8563_bcd2bin(buffer[1] & ~PCF8563_AE_H);
  alarminfo->time->tm_mday = (int)pcf8563_bcd2bin(buffer[2] & ~PCF8563_AE_D);

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

static void pcf8563_alarm_worker(FAR void *arg)
{
  uint8_t status2;
  int ret;

  g_pcf8563.dev.alarm_pending = false;

  /* Read Control/status 2. */

  ret = pcf8563_read_reg(PCF8563_REG_CTRL_STATUS2, &status2, 1);
  if (ret < 0)
    {
      rtcerr("ERROR: pcf8563_read_reg (status2) failed: %d\n", ret);
      return;
    }

  /* Was the alarm flag set?  If not, nothing to do (e.g. a spurious or
   * software-induced interrupt edge).
   */

  if ((status2 & PCF8563_CS2_AF) == 0)
    {
      return;
    }

  /* Clear the alarm flag (and timer flag).  Writing 1 to AF/TF clears them;
   * buffer layout is [address, data] to match pcf8563_write_reg().
   */

  {
    uint8_t clear[2] = { PCF8563_REG_CTRL_STATUS2,
                         PCF8563_CS2_AF | PCF8563_CS2_TF };

    ret = pcf8563_write_reg(clear[0], clear, sizeof(clear));
    if (ret < 0)
      {
        rtcerr("ERROR: pcf8563_write_reg (clear AF) failed: %d\n", ret);
        return;
      }
  }

  /* Notify the upper half.  The alarm is one-shot; it will not re-fire until
   * setalarm() is called again.  The callback may be NULL if the alarm was
   * cancelled between arming and firing.
   */

  if (g_pcf8563.dev.alarm_cb != NULL)
    {
      rtc_alarm_callback_t cb = g_pcf8563.dev.alarm_cb;
      FAR void *priv = g_pcf8563.dev.alarm_priv;

      cb(priv, 0);
    }
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
  if (g_pcf8563.dev.alarm_pending)
    {
      return;
    }

  g_pcf8563.dev.alarm_pending = true;
  work_queue(HPWORK, &g_pcf8563.dev.work, pcf8563_alarm_worker, NULL, 0);
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
