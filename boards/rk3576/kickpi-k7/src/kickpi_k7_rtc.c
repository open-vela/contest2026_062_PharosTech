/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_rtc.c
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
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include "kickpi_k7.h"
#include "pcf8563.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"

#ifdef CONFIG_KICKPI_K7_RTC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The on-board PCF8563/HYM8563TS RTC INT pin.  The PCF8563 INT output is
 * open-drain, active-low; the alarm asserts it low, so we trigger on the
 * falling edge and enable the on-chip pull-up.
 */

#define KICKPI_K7_RTC_INT_PIN (GPIO_PORT0 | GPIO_PIN_A0)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_rtc_int_handler
 *
 * Description:
 *   Driver-level GPIO interrupt callback for the PCF8563 INT pin.  Its
 *   signature is the RK3576 GPIO irq callback type (mirrors NuttX
 *   pin_interrupt_t).  It bridges the (gpio_dev_s *, pin) signature that
 *   the rk3576 GPIO layer invokes to the chip-agnostic
 *   pcf8563_alarm_service() entry point, which defers the I2C work to a
 *   worker.  Runs in interrupt context; must not sleep.
 *
 ****************************************************************************/

static int kickpi_k7_rtc_int_handler(FAR struct gpio_dev_s *dev, uint8_t pin)
{
  UNUSED(dev);
  UNUSED(pin);
  pcf8563_alarm_service(NULL);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_rtc_initialize
 *
 * Description:
 *   Mux the I2C2 SCL/SDA pins (GPIO0_B7/GPIO0_C0, alt function 9), bring
 *   up the I2C2 controller and register the on-board PCF8563/HYM8563TS
 *   RTC as /dev/rtc0.  When CONFIG_RTC_ALARM is selected, the RTC alarm
 *   interrupt (open-drain INT on GPIO0_A0) is also wired into the driver.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.  Failures are
 *   reported via syslog; the caller must not treat a failure as fatal
 *   to board startup.
 *
 ****************************************************************************/

int kickpi_k7_rtc_initialize(void)
{
  struct i2c_master_s *i2c;
  struct gpio_dev_s *int_gpio;
  int ret;

  /* Mux the I2C2 control bus (SCL GPIO0_B7 / SDA GPIO0_C0, AF9) using the
   * handle-based GPIO API.  The handles are intentionally not put(): the
   * pins now belong to the I2C2 controller, and keeping them claimed
   * prevents a second consumer from grabbing them.
   */

#define _SET_GPIO_AF(pinset, af)                                      \
  do                                                                  \
    {                                                                 \
      FAR struct gpio_dev_s *_h;                                      \
      ret = rk3576_gpio_get((pinset), &_h);                           \
      if (ret < 0)                                                    \
        {                                                             \
          syslog(LOG_ERR, "ERROR: RTC rk3576_gpio_get(%u) fail %d\n", \
                 (unsigned int)(pinset), ret);                        \
          return ret;                                                 \
        }                                                             \
      rk3576_gpio_set_af(_h, (af));                                   \
    }                                                                 \
  while (0)

  _SET_GPIO_AF(GPIO_PORT0 | GPIO_PIN_B7, 9);
  _SET_GPIO_AF(GPIO_PORT0 | GPIO_PIN_C0, 9);

#undef _SET_GPIO_AF

  i2c = rk3576_i2c_initialize(2);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: I2C2 init failed\n");
      return -ENODEV;
    }

  ret = pcf8563_rtc_initialize(i2c);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: pcf8563_rtc_initialize failed: %d\n", ret);
      return ret;
    }

#ifdef CONFIG_RTC_ALARM
  /* Claim and configure the PCF8563 INT pin (open-drain, active-low).
   * Claim it as an input with pull-up, trigger on the falling edge, attach
   * the bridge callback, then unmask the interrupt.
   */

  ret = rk3576_gpio_get(KICKPI_K7_RTC_INT_PIN, &int_gpio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: RTC INT rk3576_gpio_get failed: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_pull(int_gpio, RK3576_GPIO_PULLUP);
  rk3576_gpio_set_int_type(int_gpio, RK3576_GPIO_INT_EDGE);
  rk3576_gpio_set_int_pol(int_gpio, RK3576_GPIO_INT_LOW_FALLING);

  ret = rk3576_gpio_irq_attach(int_gpio, kickpi_k7_rtc_int_handler);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: RTC INT irq_attach failed: %d\n", ret);
      return ret;
    }

  rk3576_gpio_irq_enable(int_gpio);
#endif /* CONFIG_RTC_ALARM */

  syslog(LOG_INFO, "INFO: RTC ready: /dev/rtc0 (PCF8563 on I2C2)\n");
  return OK;
}

#endif /* CONFIG_KICKPI_K7_RTC */
