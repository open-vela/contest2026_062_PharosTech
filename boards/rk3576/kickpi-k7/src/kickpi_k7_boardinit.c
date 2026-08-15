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
#include <stdint.h>
#include <syslog.h>

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

#ifdef CONFIG_KICKPI_K7_RTC
#include "pcf8563.h"
#include "rk3576_i2c.h"
#include <nuttx/i2c/i2c_master.h>
#endif

#ifdef CONFIG_KICKPI_K7_RTC
/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The on-board PCF8563/HYM8563TS RTC INT pin.  The PCF8563 INT output is
 * open-drain, active-low; the alarm asserts it low, so we trigger on the
 * falling edge and enable the on-chip pull-up.
 */

#define KICKPI_K7_RTC_INT_PIN (GPIO_PORT0 | GPIO_PIN_A0)

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
  pcf8563_alarm_service(NULL);
  return OK;
}
#endif /* CONFIG_KICKPI_K7_RTC */

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
    emmc = rk3576_emmc_initialize(0);
    if (emmc == NULL)
      {
        syslog(LOG_ERR, "ERROR: rk3576_emmc_initialize failed\n");
      }
    else if (mmcsd_slotinitialize(1, emmc) < 0)
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
  /* Mux the I2C2 SCL/SDA pins (GPIO0_B7/GPIO0_C0, alt function 9), bring
   * up the I2C2 controller and register the on-board PCF8563/HYM8563TS
   * RTC as /dev/rtc0 before the initial application starts.  The RTC alarm
   * interrupt (open-drain INT on GPIO0_A0) is then wired into the driver.
   */

  {
    struct i2c_master_s *i2c;
    struct gpio_dev_s *int_gpio;
    int ret;

    /* Mux the I2C2 control bus (SCL GPIO0_B7 / SDA GPIO0_C0, AF9) using the
     * new handle-based GPIO API.  The handles are intentionally not put():
     * the pins now belong to the I2C2 controller, and keeping them claimed
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
          return;                                                     \
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
        return;
      }

    ret = pcf8563_rtc_initialize(i2c);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: pcf8563_rtc_initialize failed: %d\n", ret);
        return;
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
        return;
      }

    rk3576_gpio_set_pull(int_gpio, RK3576_GPIO_PULLUP);
    rk3576_gpio_set_int_type(int_gpio, RK3576_GPIO_INT_EDGE);
    rk3576_gpio_set_int_pol(int_gpio, RK3576_GPIO_INT_LOW_FALLING);

    ret = rk3576_gpio_irq_attach(int_gpio, kickpi_k7_rtc_int_handler);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: RTC INT irq_attach failed: %d\n", ret);
        return;
      }

    rk3576_gpio_irq_enable(int_gpio);
#endif /* CONFIG_RTC_ALARM */

    syslog(LOG_INFO, "INFO: RTC ready: /dev/rtc0 (PCF8563 on I2C2)\n");
  }
#endif /* CONFIG_KICKPI_K7_RTC */
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */
