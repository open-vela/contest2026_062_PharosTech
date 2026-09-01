/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_boardinit.c
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

#include "kickpi_k7.h"
#include "rk3576_clk_tree.h"
#include "rk3576_gpio.h"
#include "rk3576_serial.h"
#include <arch/board/board.h>
#include <nuttx/board.h>
#include <nuttx/config.h>
#include <syslog.h>

#ifdef CONFIG_RK3576_TSADC
#include "rk3576_tsadc.h"
#include <nuttx/sensors/sensor.h>
#endif

#ifdef CONFIG_DEV_GPIO
#include <nuttx/ioexpander/gpio.h>
#endif

#ifdef CONFIG_RK3576_SDMMC
#include "rk3576_sdmmc.h"
#include <nuttx/mmcsd.h>
#endif

#ifdef CONFIG_RK3576_EMMC
#include "rk3576_emmc.h"
#include <nuttx/mmcsd.h>
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_BOARD_LATE_INITIALIZE) && defined(CONFIG_RK3576_EMMC)
static int kickpi_k7_emmc_pinmux(void);
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_memory_initialize
 *
 * Description:
 *   All RK3576 architectures must provide the following entry point.  This
 *   entry point is called early in the initialization before memory has
 *   been configured.  This board-specific function is responsible for
 *   configuring any on-board memories.
 *
 *   Logic in rk3576_memory_initialize must be careful to avoid using any
 *   global variables because those will be uninitialized at the time this
 *   function is called.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_memory_initialize(void)
{
  /* SDRAM was already init by bootloader in supported configuration */
}

/****************************************************************************
 * Name: rk3576_board_initialize
 *
 * Description:
 *   All RK3576 architectures must provide the following entry point.  This
 *   entry point is called in the initialization phase -- after
 *   rk3576_memory_initialize and after all memory has been configured and
 *   mapped but before any devices have been initialized.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_board_initialize(void)
{
#ifdef CONFIG_ARCH_LEDS
  /* Configure on-board LEDs if LED support has been selected. */

  /* board_autoled_initialize(); */
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   If CONFIG_BOARD_LATE_INITIALIZE is selected, then an additional
 *   initialization call will be performed in the boot-up sequence to a
 *   function called board_late_initialize(). board_late_initialize() will be
 *   called immediately after up_initialize() is called and just before the
 *   initial application is started.  This additional initialization phase
 *   may be used, for example, to initialize board-specific device drivers.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
#ifdef CONFIG_RK3576_EMMC

/****************************************************************************
 * Name: kickpi_k7_emmc_pinmux
 *
 * Description:
 *   Configure the board-wired eMMC DAT0..7, CMD and CLK pins from the vendor
 *   DTS settings before the host controller starts card enumeration.
 ****************************************************************************/

static int kickpi_k7_emmc_pinmux(void)
{
  gpio_pinset_t common =
      GPIO_PORT1 | GPIO_ALT | GPIO_AF1 | GPIO_PULLUP | GPIO_DRV_STRENGTH_66OHM;
  FAR struct gpio_dev_s *handle;
  unsigned int pin;
  int ret;

  /* Vendor DTS: GPIO1_A0..A7 = DAT0..7, GPIO1_B0 = CMD and GPIO1_B1 = CLK.
   * All use mux function 1 with pull-up and drive level 2.
   */

  for (pin = 0; pin <= 9; pin++)
    {
      ret = rk3576_gpio_get(common | (pin << GPIO_PIN_SHIFT), &handle);
      if (ret < 0)
        {
          return ret;
        }

      rk3576_gpio_set_pull(handle, RK3576_GPIO_PULLUP);
      ret = rk3576_gpio_set_drive(handle, RK3576_GPIO_DRIVE_66OHM);
      if (ret < 0)
        {
          rk3576_gpio_put(handle);
          return ret;
        }

      rk3576_gpio_set_schmitt(handle, false);
      rk3576_gpio_set_af(handle, 1);
      rk3576_gpio_put(handle);
    }

  return OK;
}
#endif

void board_late_initialize(void)
{
#ifdef CONFIG_RK3576_SDMMC
  FAR struct sdio_dev_s *sdmmc = NULL;
#endif
#ifdef CONFIG_RK3576_EMMC
  FAR struct sdio_dev_s *emmc = NULL;
#endif

  /* Register the RK3576 clock tree with the NuttX CLK framework.
   * Must be done before any peripheral driver calls clk_get().
   * This cannot run in arm64_chip_boot() because clk_register()
   * depends on kmm_zalloc(), which requires the kernel heap to
   * be initialized (done in nx_start() -> memory_initialize()).
   */

  rk3576_clk_tree_initialize();

  /* Set the LITTLE-core (litcore) CPU frequency once at boot.  This is a
   * one-shot configuration (no DVFS): the desired rate comes from
   * CONFIG_KICKPI_K7_LITCORE_CPU_FREQ_MHZ (MHz) and is applied by looking
   * it up in the LPLL frequency table, switching the CPU clock source
   * safely through GPLL while LPLL is being reprogrammed.  Must run after
   * rk3576_clk_tree_initialize() and before starting external/peripheral
   * clocks that depend on the final CPU rate.
   * Disable CONFIG_KICKPI_K7_LITCORE_CPU_FREQ_SET to skip this and keep
   * the bootloader-configured rate.
   * NOTE: LIT core only — the big-core cluster (BPLL) has its own path.
   */

#ifdef CONFIG_KICKPI_K7_LITCORE_CPU_FREQ_SET
  {
    /* Build-time guard: reject a Kconfig frequency that is not one of the
     * standard RK3576 LPLL gears, so a misconfiguration fails the compile
     * instead of being silently ignored at runtime.
     */

    _Static_assert(
        RK3576_LITCORE_CPU_FREQ_IS_VALID(
            CONFIG_KICKPI_K7_LITCORE_CPU_FREQ_MHZ),
        "CONFIG_KICKPI_K7_LITCORE_CPU_FREQ_MHZ is not a supported RK3576 "
        "LPLL gear (e.g. 1800, 1200, 1008, 600, 96 ...)");

    int ret =
        rk3576_clk_set_litcore_cpufreq(CONFIG_KICKPI_K7_LITCORE_CPU_FREQ_MHZ);
    if (ret < 0)
      {
        syslog(LOG_ERR,
               "ERROR: rk3576_clk_set_litcore_cpufreq(%d MHz) failed: %d\n",
               CONFIG_KICKPI_K7_LITCORE_CPU_FREQ_MHZ, ret);
      }
  }
#endif

#ifdef CONFIG_RK3576_UART
  /* Register UART0 through the unified serial registration path.
   * UART0 is special: it was initialized early in arm64_earlyserialinit()
   * for boot logging using the bootloader-configured clocks.  Now that the
   * clock tree is ready, calling rk3576_serial_register(UART_PORT_0, ...)
   * initializes the clocks through the NuttX CLK framework so the clock
   * framework is aware of the UART0 clock enable state.
   * The statically-allocated port/dev (g_uart0priv/g_uart0port) is reused;
   * /dev/console and /dev/ttyS0 are already registered by arm64_serialinit.
   */

  {
    int ret = rk3576_serial_register(UART_PORT_0, CONFIG_UART0_BAUD,
                                     CONFIG_UART0_BITS, CONFIG_UART0_PARITY,
                                     CONFIG_UART0_2STOP, 0, 0);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: rk3576_serial_register(UART0) failed: %d\n",
               ret);
      }
  }
#endif /* CONFIG_RK3576_UART */

  /* Perform board initialization */

#ifdef CONFIG_RK3576_TSADC
  {
    FAR struct rk3576_tsadc_sensor_s *sensors;
    int num;
    int ret;
    int i;

    ret = rk3576_tsadc_initialize(&sensors, &num);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: TSADC initialization failed: %d\n", ret);
      }
    else
      {
        /* Publish one on-die temperature sensor node per TSADC channel. */

        for (i = 0; i < num; i++)
          {
            ret = sensor_custom_register(sensors[i].lower, sensors[i].name,
                                         sizeof(struct sensor_temp));
            if (ret < 0)
              {
                syslog(LOG_ERR, "ERROR: TSADC register %s failed: %d\n",
                       sensors[i].name, ret);
              }
            else
              {
                syslog(LOG_INFO, "INFO: TSADC registered %s\n",
                       sensors[i].name);
              }
          }
      }
  }
#endif /* CONFIG_RK3576_TSADC */

#ifdef CONFIG_DEV_GPIO
  /* Claim the LED GPIO pin and register it as /dev/gpio0.  The handle is
   * owned by this board code; gpio_pin_register() attaches the upper-half
   * file operations so user space can drive it.
   */

  do
    {
      FAR struct gpio_dev_s *led;
      int ret;

      ret = rk3576_gpio_get(GPIO_PORT0 | GPIO_PIN_B4, &led);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: rk3576_gpio_get(LED) failed: %d\n", ret);
          break;
        }
      /* Configure the pin as an output before exposing it to user space. */

      rk3576_gpio_set_mode(led, RK3576_GPIO_OUTPUT);

      ret = gpio_pin_register(led, 0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: gpio_pin_register(LED) failed: %d\n", ret);
        }
    }
  while (0);
#endif

#ifdef CONFIG_RK3576_SDMMC
  /* Initialize the SD card slot (SDMMC0).  The SD card is an
   * optional peripheral: on failure only warn, do not block the boot (booting
   * to NSH must succeed even with no card inserted).
   */

  {
    sdmmc = rk3576_sdmmc_initialize(0);
    if (sdmmc == NULL)
      {
        syslog(LOG_ERR, "ERROR: rk3576_sdmmc_initialize failed\n");
      }
    else if (mmcsd_slotinitialize(0, sdmmc) < 0)
      {
        syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize failed\n");
      }
  }

#endif

#ifdef CONFIG_RK3576_EMMC
  /* Initialize the on-board eMMC (dwcmshc / SDHCI) as /dev/mmcsd1. */

  {
    int ret = kickpi_k7_emmc_pinmux();
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: K7 eMMC pinmux failed: %d\n", ret);
      }
    else
      {
        emmc = rk3576_emmc_initialize(0);
      }

    if (ret >= 0 && emmc == NULL)
      {
        syslog(LOG_ERR, "ERROR: rk3576_emmc_initialize failed\n");
      }
    else if (emmc != NULL && mmcsd_slotinitialize(1, emmc) < 0)
      {
        syslog(LOG_ERR, "ERROR: eMMC mmcsd_slotinitialize failed\n");
      }
  }
#endif

#ifdef CONFIG_KICKPI_K7_AUDIO
  /* Register the on-board ES8388 PCM device before the initial application
   * starts.  The initializer is idempotent, so board utilities may call it
   * again safely.
   */

  {
    int ret = kickpi_k7_audio_initialize();
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: kickpi_k7_audio_initialize failed: %d\n", ret);
      }
  }
#endif

#ifdef CONFIG_KICKPI_K7_STORAGE_AUTOMOUNT
  {
    int ret = kickpi_k7_storage_initialize(
#ifdef CONFIG_RK3576_SDMMC
        sdmmc,
#else
        NULL,
#endif
#ifdef CONFIG_RK3576_EMMC
        emmc
#else
        NULL
#endif
    );

    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: storage initialization failed: %d\n", ret);
      }
  }
#endif

#ifdef CONFIG_KICKPI_K7_RTC
  /* Register the on-board PCF8563/HYM8563TS RTC as /dev/rtc0 (I2C2).  A
   * failure only logs; it must not abort the rest of board startup.
   */

  {
    int ret = kickpi_k7_rtc_initialize();
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: kickpi_k7_rtc_initialize failed: %d\n", ret);
      }
  }
#endif /* CONFIG_KICKPI_K7_RTC */

#ifdef CONFIG_KICKPI_K7_WIFI
  {
    int ret = kickpi_k7_wifi_initialize();
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: kickpi_k7_wifi_initialize failed: %d\n", ret);
      }
  }
#endif
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */
