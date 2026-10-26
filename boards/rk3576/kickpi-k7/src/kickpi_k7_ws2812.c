/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_ws2812.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
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

#include <nuttx/spi/spi.h>

#include "kickpi_k7.h"
#include "rk3576_gpio.h"
#include "rk3576_spi.h"
#include "ws2812.h"

#ifdef CONFIG_KICKPI_K7_WS2812

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SPI3 in its M0 pin multiplexing: the strip's data line is GPIO3_A1
 * (SPI3_MOSI_M0, IOMUX alternate function 10), which is header pin 24.
 * VCCIO4 supplies both SPI3 M0 pins at 3.3 V, so no level shifter is
 * needed for a WS2812 strip.
 *
 * Only the data line is muxed.  A WS2812 strip has no clock, no chip select
 * and never returns data, so GPIO3_A0 (SPI3_CLK_M0), GPIO3_A2 (SPI3_MISO_M0,
 * not on the header) and the SPI3 chip selects are left in their default
 * GPIO multiplexing.
 *
 * Between frames the same pin is parked as a GPIO output driving 0.  The
 * strip latches a frame on a low of at least 50 us, and a data line left
 * high is decoded as a never-ending run of '1' bits -- the whole strip turns
 * white at the next reset.  The RK3576 SPI controller does not keep MOSI
 * driven low once a transfer is over (the pin is released and only the pad
 * pull would hold a level), so leaving the pin on the SPI function between
 * frames is not an option.
 */

#define KICKPI_K7_WS2812_DEVPATH  "/dev/ws2812"
#define KICKPI_K7_WS2812_SPI_BUS  3
#define KICKPI_K7_WS2812_MOSI_PIN (GPIO_PORT3 | GPIO_PIN_A1)
#define KICKPI_K7_WS2812_MOSI_AF  10

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The MOSI pin is claimed once and kept for the driver lifetime, exactly
 * like the other board-owned pins: releasing it would leave the pad muxed
 * to SPI3 while making it claimable by someone else.
 */

static FAR struct gpio_dev_s *g_ws2812_mosi;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_ws2812_select
 *
 * Description:
 *   Select/deselect notification from the character driver.  Select ==
 *   true hands the pad to the SPI function; false hands it back to GPIO,
 *   which was programmed at init to output 0, so the data line stays low
 *   until the next frame.
 *
 *   The callback runs with the SPI bus lock held, so the pad cannot change
 *   state in the middle of another strip's frame.
 *
 ****************************************************************************/

static void kickpi_k7_ws2812_select(bool select)
{
  rk3576_gpio_set_af(g_ws2812_mosi, select ? KICKPI_K7_WS2812_MOSI_AF : 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_ws2812_initialize
 *
 * Description:
 *   Register the board's WS2812 strip as /dev/ws2812.  The data line is
 *   SPI3 MOSI in its M0 multiplexing (GPIO3_A1); the SPI3 controller is
 *   brought up by the chip driver and shared through the character
 *   driver's per-frame bus lock.
 *
 *   The select callback owns the pin multiplexing for the data line: the
 *   pin idles as a GPIO output driving 0 and is only muxed to SPI3 while a
 *   frame is being transmitted.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_ws2812_initialize(void)
{
  FAR struct spi_dev_s *spi;
  int ret;

  ret = rk3576_gpio_get(KICKPI_K7_WS2812_MOSI_PIN, &g_ws2812_mosi);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ws2812: claim GPIO3_A1 failed: %d\n", ret);
      return ret;
    }

  /* Park the data line low before the first frame.  Set the level and the
   * direction while the pad is not looking at the GPIO block yet, then
   * switch the mux so the pin comes up already driving 0.  The pull-down
   * covers the short windows where the pad is on the SPI function but no
   * transfer is driving it.
   */

  rk3576_gpio_write_bit(g_ws2812_mosi, false);
  rk3576_gpio_set_mode(g_ws2812_mosi, RK3576_GPIO_OUTPUT);
  rk3576_gpio_set_pull(g_ws2812_mosi, RK3576_GPIO_PULLDOWN);
  rk3576_gpio_set_af(g_ws2812_mosi, 0);

  spi = rk3576_spi_initialize(KICKPI_K7_WS2812_SPI_BUS);
  if (spi == NULL)
    {
      syslog(LOG_ERR, "ERROR: ws2812: rk3576_spi_initialize(%d) failed\n",
             KICKPI_K7_WS2812_SPI_BUS);
      return -ENODEV;
    }

  ret =
      ws2812_register(KICKPI_K7_WS2812_DEVPATH, spi, kickpi_k7_ws2812_select);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ws2812: register %s failed: %d\n",
             KICKPI_K7_WS2812_DEVPATH, ret);
      return ret;
    }

  return OK;
}

#endif /* CONFIG_KICKPI_K7_WS2812 */
