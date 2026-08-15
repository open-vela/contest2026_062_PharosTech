/****************************************************************************
 * drivers/include/pcf8563.h
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
 * Public interface for the PCF8563 / HYM8563 RTC driver.
 *
 * HYM8563TS (Huayi) is a pin- and register-compatible drop-in replacement
 * for the NXP PCF8563 I2C real-time clock / calendar.  The chip exposes
 * 16 BCD-coded registers (00h..0Fh): time/date, alarm, timer and CLKOUT
 * control.  I2C 7-bit slave address is fixed to 0x51.
 *
 ****************************************************************************/

#ifndef __INCLUDE_DRIVERS_PCF8563_H
#define __INCLUDE_DRIVERS_PCF8563_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <nuttx/config.h>

#ifdef CONFIG_RTC_PCF8563

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* CLKOUT frequency selection for pcf8563_clkout_set(). */

enum pcf8563_clkout_freq_e
{
  PCF8563_CLKOUT_DISABLE = -1, /* CLKOUT off (hi-Z) */
  PCF8563_CLKOUT_32768HZ = 0,  /* 32.768 kHz */
  PCF8563_CLKOUT_1024HZ = 1,   /* 1.024 kHz */
  PCF8563_CLKOUT_32HZ = 2,     /* 32 Hz */
  PCF8563_CLKOUT_1HZ = 3       /* 1 Hz */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: pcf8563_rtc_initialize
 *
 * Description:
 *   Initialize the PCF8563/HYM8563 hardware RTC.
 *
 *   This function is called once during the OS initialization sequence by
 *   board-specific logic.  After it returns, the board logic should call
 *   clock_synchronize() to seed the system timer from the RTC.
 *
 * Input Parameters:
 *   i2c - An I2C master instance the RTC is attached to.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

struct i2c_master_s; /* Forward reference */
int pcf8563_rtc_initialize(FAR struct i2c_master_s *i2c);

/****************************************************************************
 * Name: pcf8563_clkout_set
 *
 * Description:
 *   Configure the PCF8563 programmable clock output (CLKOUT pin).
 *
 *   Driver-layer interface (not exposed through RTC ioctl) so that another
 *   kernel driver can source a fixed reference clock from the chip.
 *
 * Input Parameters:
 *   freq - pcf8563_clkout_freq_e value, or PCF8563_CLKOUT_DISABLE to turn
 *          the output off (hi-Z).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int pcf8563_clkout_set(enum pcf8563_clkout_freq_e freq);

#ifdef CONFIG_RTC_ALARM
/****************************************************************************
 * Name: pcf8563_alarm_service
 *
 * Description:
 *   Alarm interrupt service entry point.
 *
 *   Called by board-specific logic from the GPIO interrupt that is wired to
 *   the PCF8563 open-drain INT pin.  This function only schedules a worker
 *   that performs the actual I2C register access (reading/clearing the alarm
 *   flag and invoking the RTC upper-half alarm callback) -- doing I2C
 *   transfers directly in interrupt context is unsafe.
 *
 *   It is chip-agnostic: the driver does not depend on any particular SoC
 *   GPIO or interrupt API.  The board layer bridges whatever interrupt
 *   callback signature it has to this entry point.
 *
 * Input Parameters:
 *   arg - Unused (may be NULL).  Reserved for future use.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

void pcf8563_alarm_service(FAR void *arg);
#endif /* CONFIG_RTC_ALARM */

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RTC_PCF8563 */
#endif /* __INCLUDE_DRIVERS_PCF8563_H */
