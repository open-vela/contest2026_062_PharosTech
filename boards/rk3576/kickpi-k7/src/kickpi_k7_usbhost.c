/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_usbhost.c
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

#include <nuttx/config.h>

#ifdef CONFIG_KICKPI_K7_USBHOST

#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/usb/usbhost.h>

#include "kickpi_k7.h"
#include "rk3576_gpio.h"
#include "rk3576_usb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KICKPI_K7_USBHOST_VBUS           (GPIO_PORT3 | GPIO_PIN_D6)

#define KICKPI_K7_USBHOST_POWER_DELAY_MS 600
#define KICKPI_K7_USBHOST_POWER_OFF_MS   100

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct usbhost_connection_s *g_kickpi_k7_usbhost;
static FAR struct gpio_dev_s *g_kickpi_k7_usbhost_vbus;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_usbhost_initialize(void)
{
  int ret;

  if (g_kickpi_k7_usbhost != NULL)
    {
      return OK;
    }

  if (g_kickpi_k7_usbhost_vbus == NULL)
    {
      ret = rk3576_gpio_get(KICKPI_K7_USBHOST_VBUS, &g_kickpi_k7_usbhost_vbus);
      if (ret < 0)
        {
          return ret;
        }
    }

  rk3576_gpio_set_af(g_kickpi_k7_usbhost_vbus, 0);
  rk3576_gpio_set_pull(g_kickpi_k7_usbhost_vbus, RK3576_GPIO_PULLDOWN);
  rk3576_gpio_write_bit(g_kickpi_k7_usbhost_vbus, false);
  rk3576_gpio_set_mode(g_kickpi_k7_usbhost_vbus, RK3576_GPIO_OUTPUT);
  up_mdelay(KICKPI_K7_USBHOST_POWER_OFF_MS);
  rk3576_gpio_write_bit(g_kickpi_k7_usbhost_vbus, true);

  /* GPIO3_D6 enables both load switches supplying VBUS to the three
   * external downstream ports.  Allow the GL3510 crystal and regulators
   * to settle before xHCI probes an already-connected root port.
   */

  up_mdelay(KICKPI_K7_USBHOST_POWER_DELAY_MS);

  g_kickpi_k7_usbhost = rk3576_usbhost_initialize();
  if (g_kickpi_k7_usbhost == NULL)
    {
      rk3576_gpio_write_bit(g_kickpi_k7_usbhost_vbus, false);
      return -ENODEV;
    }

  return OK;
}

#endif /* CONFIG_KICKPI_K7_USBHOST */
