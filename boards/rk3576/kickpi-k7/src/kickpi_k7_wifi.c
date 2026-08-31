/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_wifi.c
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
 * Board glue for the on-board SeekWave SV6621 (SWT6621-S) WiFi/BT combo.
 * Owns the module pin environment (SDIO bus mux + drive strength, the
 * companion BT-side pins the combo boot ROM samples, and the hym8563 RTC
 * 32.768 kHz sleep clock), supplies the CP firmware images, and drives
 * WL_REG_ON for the core driver's power callback.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <debug.h>
#include <errno.h>
#include <nuttx/config.h>

#include <nuttx/power/pm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"

#include "kickpi_k7.h"
#include "kickpi_k7_sv6621_transport.h"
#include "pcf8563.h"
#include "rk3576_gpio.h"
#include "sv6621.h"

#ifdef CONFIG_KICKPI_K7_WIFI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_SDIO_PIN(pin)      (GPIO_PORT1 | (pin))

#define WIFI_WL_REG_ON          (GPIO_PORT1 | GPIO_PIN_C6)
#define WIFI_BT_RST             (GPIO_PORT1 | GPIO_PIN_C7)
#define WIFI_HOST_WAKE          (GPIO_PORT1 | GPIO_PIN_D5)

#define WIFI_HOST_WAKE_ACTIVITY 1

/* IOC drive-strength registers for the SDIO bus pins (max drive). */

#define WIFI_IOC_DRV0 0x26046210
#define WIFI_IOC_DRV1 0x26046214
#define WIFI_IOC_DRV2 0x26046218
#define WIFI_IOC_DRV3 0x2604621c

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* CP firmware images, linked into .rodata from the extracted SeekWave
 * blobs (redistributed with the product; copyright SeekWave, loaded only).
 */

__asm__("  .section .rodata, \"a\"\n"
        "  .align 4\n"
        "  .global g_sv6621_iram_start\n"
        "g_sv6621_iram_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_IRAM "\"\n"
        "  .global g_sv6621_iram_end\n"
        "g_sv6621_iram_end:\n"
        "  .align 4\n"
        "  .global g_sv6621_dram_start\n"
        "g_sv6621_dram_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_DRAM "\"\n"
        "  .global g_sv6621_dram_end\n"
        "g_sv6621_dram_end:\n"
        "  .align 4\n"
        "  .global g_sv6621_nv_start\n"
        "g_sv6621_nv_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_NV "\"\n"
        "  .global g_sv6621_nv_end\n"
        "g_sv6621_nv_end:\n"
        "  .align 4\n"
        "  .global g_sv6621_calib_start\n"
        "g_sv6621_calib_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_CALIB "\"\n"
        "  .global g_sv6621_calib_end\n"
        "g_sv6621_calib_end:\n"
        "  .previous\n");

extern const uint8_t g_sv6621_iram_start[];
extern const uint8_t g_sv6621_iram_end[];
extern const uint8_t g_sv6621_dram_start[];
extern const uint8_t g_sv6621_dram_end[];
extern const uint8_t g_sv6621_nv_start[];
extern const uint8_t g_sv6621_nv_end[];
extern const uint8_t g_sv6621_calib_start[];
extern const uint8_t g_sv6621_calib_end[];

static const gpio_pinset_t g_wifi_sdio_pins[] = {
  WIFI_SDIO_PIN(GPIO_PIN_B4), /* D0 */
  WIFI_SDIO_PIN(GPIO_PIN_B5), /* D1 */
  WIFI_SDIO_PIN(GPIO_PIN_B6), /* D2 */
  WIFI_SDIO_PIN(GPIO_PIN_B7), /* D3 */
  WIFI_SDIO_PIN(GPIO_PIN_C0), /* CMD */
  WIFI_SDIO_PIN(GPIO_PIN_C1), /* CLK */
};

static const gpio_pinset_t g_wifi_companion_pins[] = {
  WIFI_BT_RST,
  GPIO_PORT1 | GPIO_PIN_D4,
  GPIO_PORT1 | GPIO_PIN_C2,
  GPIO_PORT1 | GPIO_PIN_C3,
  GPIO_PORT1 | GPIO_PIN_C4,
  GPIO_PORT1 | GPIO_PIN_C5,
};

static FAR struct gpio_dev_s *g_wifi_sdio_handles[nitems(g_wifi_sdio_pins)];
static FAR struct gpio_dev_s
    *g_wifi_companion_handles[nitems(g_wifi_companion_pins)];
static FAR struct gpio_dev_s *g_wifi_wl_reg_on;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool kickpi_k7_wifi_blob_is_zero(FAR const uint8_t *data,
                                        size_t length);
static bool kickpi_k7_wifi_has_placeholders(void);
static void kickpi_k7_wifi_warn_placeholders(void);
static int kickpi_k7_wifi_config_alt(gpio_pinset_t pinset, unsigned int af,
                                     enum rk3576_gpio_pull_e pull,
                                     FAR struct gpio_dev_s **handle);
static int kickpi_k7_wifi_config_output(gpio_pinset_t pinset, bool value,
                                        FAR struct gpio_dev_s **handle);
static int kickpi_k7_wifi_power_on(FAR void *arg);
static void kickpi_k7_wifi_power_off(FAR void *arg);
static int kickpi_k7_wifi_load_address(FAR void *arg,
                                       uint8_t address[SV6621_MAC_LENGTH]);
static int
kickpi_k7_wifi_store_address(FAR void *arg,
                             FAR const uint8_t address[SV6621_MAC_LENGTH]);
#ifdef CONFIG_SV6621_PM
static int kickpi_k7_wifi_host_wake_isr(FAR struct gpio_dev_s *dev,
                                        uint8_t pin);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_wifi_blob_is_zero
 *
 * Description:
 *   Return true only when a non-empty embedded image is entirely zero.
 ****************************************************************************/

static bool kickpi_k7_wifi_blob_is_zero(FAR const uint8_t *data, size_t length)
{
  size_t i;

  if (length == 0)
    {
      return false;
    }

  for (i = 0; i < length; i++)
    {
      if (data[i] != 0)
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: kickpi_k7_wifi_has_placeholders
 *
 * Description:
 *   Detect the complete zero-filled firmware set generated by public CI.
 *   Requiring every image to be all-zero keeps partial or corrupt real
 *   firmware on the normal error path.
 ****************************************************************************/

static bool kickpi_k7_wifi_has_placeholders(void)
{
  return kickpi_k7_wifi_blob_is_zero(
             g_sv6621_iram_start,
             (size_t)(g_sv6621_iram_end - g_sv6621_iram_start)) &&
         kickpi_k7_wifi_blob_is_zero(
             g_sv6621_dram_start,
             (size_t)(g_sv6621_dram_end - g_sv6621_dram_start)) &&
         kickpi_k7_wifi_blob_is_zero(
             g_sv6621_nv_start,
             (size_t)(g_sv6621_nv_end - g_sv6621_nv_start)) &&
         kickpi_k7_wifi_blob_is_zero(
             g_sv6621_calib_start,
             (size_t)(g_sv6621_calib_end - g_sv6621_calib_start));
}

/****************************************************************************
 * Name: kickpi_k7_wifi_warn_placeholders
 *
 * Description:
 *   Explain the expected firmware-free public-build behavior and recovery.
 ****************************************************************************/

static void kickpi_k7_wifi_warn_placeholders(void)
{
  syslog(LOG_WARNING, "======================================================="
                      "=================\n");
  syslog(LOG_WARNING, "WARNING: SV6621 firmware load was skipped.\n");
  syslog(LOG_WARNING, "\n");
  syslog(LOG_WARNING,
         "A zero-filled firmware placeholder was detected. These placeholder "
         "files\n");
  syslog(LOG_WARNING,
         "are generated only to verify firmware-free public builds and "
         "cannot boot\n");
  syslog(LOG_WARNING, "the SeekWave co-processor.\n");
  syslog(LOG_WARNING, "\n");
  syslog(LOG_WARNING,
         "Wi-Fi will remain unavailable for this boot. System startup will "
         "continue;\n");
  syslog(LOG_WARNING,
         "the serial console, ADB, storage, audio, USB, and other unrelated "
         "features\n");
  syslog(LOG_WARNING, "are not affected.\n");
  syslog(LOG_WARNING, "\n");
  syslog(LOG_WARNING, "TIP:\n");
  syslog(LOG_WARNING,
         "Obtain the authorized SV6621/SWT6621-S firmware from SeekWave or "
         "your board\n");
  syslog(LOG_WARNING, "vendor, then place all four files in:\n");
  syslog(LOG_WARNING, "\n");
  syslog(LOG_WARNING, "  drivers/drivers/sv6621/firmware/\n");
  syslog(LOG_WARNING, "\n");
  syslog(LOG_WARNING, "Required files:\n");
  syslog(LOG_WARNING, "  SWT6621S_IRAM_SDIO.bin\n");
  syslog(LOG_WARNING, "  SWT6621S_DRAM_SDIO.bin\n");
  syslog(LOG_WARNING, "  SWT6621S_NV_SDIO_ALONE.bin\n");
  syslog(LOG_WARNING, "  SWT6621S_SEEKWAVE_R00001.bin\n");
  syslog(LOG_WARNING, "\n");
  syslog(LOG_WARNING,
         "Replace every placeholder file and rebuild the image to enable "
         "Wi-Fi.\n");
  syslog(LOG_WARNING, "======================================================="
                      "=================\n");
}

static int kickpi_k7_wifi_config_alt(gpio_pinset_t pinset, unsigned int af,
                                     enum rk3576_gpio_pull_e pull,
                                     FAR struct gpio_dev_s **handle)
{
  int ret;

  if (*handle == NULL)
    {
      ret = rk3576_gpio_get(pinset, handle);
      if (ret < 0)
        {
          return ret;
        }
    }

  rk3576_gpio_set_pull(*handle, pull);
  rk3576_gpio_set_schmitt(*handle, true);
  rk3576_gpio_set_af(*handle, af);
  return OK;
}

static int kickpi_k7_wifi_config_output(gpio_pinset_t pinset, bool value,
                                        FAR struct gpio_dev_s **handle)
{
  int ret;

  if (*handle == NULL)
    {
      ret = rk3576_gpio_get(pinset, handle);
      if (ret < 0)
        {
          return ret;
        }
    }

  rk3576_gpio_set_af(*handle, 0);
  rk3576_gpio_write_bit(*handle, value);
  rk3576_gpio_set_mode(*handle, RK3576_GPIO_OUTPUT);
  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_wifi_power_on
 *
 * Description:
 *   Reset the combo through WL_REG_ON and wait for the boot ROM to settle.
 ****************************************************************************/

static int kickpi_k7_wifi_power_on(FAR void *arg)
{
  (void)arg;
  rk3576_gpio_write_bit(g_wifi_wl_reg_on, false);
  up_mdelay(1000);
  rk3576_gpio_write_bit(g_wifi_wl_reg_on, true);
  up_mdelay(200);
  return 0;
}

/****************************************************************************
 * Name: kickpi_k7_wifi_power_off
 ****************************************************************************/

static void kickpi_k7_wifi_power_off(FAR void *arg)
{
  (void)arg;
  rk3576_gpio_write_bit(g_wifi_wl_reg_on, false);
}

/****************************************************************************
 * Name: kickpi_k7_wifi_load_address
 ****************************************************************************/

static int kickpi_k7_wifi_load_address(FAR void *arg,
                                       uint8_t address[SV6621_MAC_LENGTH])
{
  FAR uint8_t *saved = arg;

  if (saved[0] == 0)
    {
      return -ENOENT;
    }

  memcpy(address, saved, SV6621_MAC_LENGTH);
  return 0;
}

/****************************************************************************
 * Name: kickpi_k7_wifi_store_address
 ****************************************************************************/

static int
kickpi_k7_wifi_store_address(FAR void *arg,
                             FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  memcpy(arg, address, SV6621_MAC_LENGTH);
  return 0;
}

#ifdef CONFIG_SV6621_PM
/****************************************************************************
 * Name: kickpi_k7_wifi_host_wake_isr
 *
 * Description:
 *   Report activity when the combo asserts its dedicated active-high
 *   host-wake line.  The interrupt wakes the CPU from WFI; the platform PM
 *   path subsequently enters PM_RESTORE and resumes the Wi-Fi transport.
 *   No SDIO transaction or driver resume is permitted here.
 ****************************************************************************/

static int kickpi_k7_wifi_host_wake_isr(FAR struct gpio_dev_s *dev,
                                        uint8_t pin)
{
  UNUSED(dev);
  UNUSED(pin);
  pm_activity(PM_IDLE_DOMAIN, WIFI_HOST_WAKE_ACTIVITY);
  return OK;
}
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_kickpi_k7_wifi_address[SV6621_MAC_LENGTH];
static FAR struct sv6621_dev_s *g_kickpi_k7_wifi_dev;
#ifdef CONFIG_SV6621_PM
static FAR struct gpio_dev_s *g_kickpi_k7_wifi_host_wake;
static const struct sv6621_suspend_s g_kickpi_k7_wifi_suspend = {
  .wake_enabled = true,
  .wake_flags = SV6621_WAKE_DISCONNECT | SV6621_WAKE_MAGIC_PACKET |
                SV6621_WAKE_GTK_REKEY_FAILURE,
};
#endif

static const struct sv6621_board_ops_s g_kickpi_k7_wifi_board_ops = {
  .power_on = kickpi_k7_wifi_power_on,
  .power_off = kickpi_k7_wifi_power_off,
  .load_address = kickpi_k7_wifi_load_address,
  .store_address = kickpi_k7_wifi_store_address,
};

static const struct sv6621_regulatory_domain_s
    g_kickpi_k7_wifi_regulatory_domains[] = {
  {
    .country = { 'C', 'N' },
    .rule_count = 4,
    .rules = {
      { 1, 13, 20, 0, 0 },
      { 36, 13, 23, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 149, 17, 33, 0, 0 },
    },
  },
  {
    .country = { '0', '0' },
    .rule_count = 7,
    .rules = {
      { 1, 11, 20, 0, 0 },
      { 12, 2, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                        SV6621_REGULATORY_FLAG_AUTO_BW },
      { 14, 1, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                        SV6621_REGULATORY_FLAG_NO_OFDM },
      { 36, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                         SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                         SV6621_REGULATORY_FLAG_DFS |
                         SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 45, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                          SV6621_REGULATORY_FLAG_DFS },
      { 149, 17, 20, 0, SV6621_REGULATORY_FLAG_NO_IR },
    },
  },
  {
    .country = { 'U', 'S' },
    .rule_count = 5,
    .rules = {
      { 1, 11, 30, 0, 0 },
      { 36, 13, 23, 0, SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 24, 0, SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 45, 24, 0, SV6621_REGULATORY_FLAG_DFS },
      { 149, 17, 30, 0, SV6621_REGULATORY_FLAG_AUTO_BW },
    },
  },
  {
    .country = { 'D', 'E' },
    .rule_count = 5,
    .rules = {
      { 1, 13, 20, 0, 0 },
      { 36, 13, 23, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 41, 27, 0, SV6621_REGULATORY_FLAG_DFS },
      { 149, 25, 14, 0, 0 },
    },
  },
  {
    .country = { 'J', 'P' },
    .rule_count = 5,
    .rules = {
      { 1, 13, 20, 0, 0 },
      { 14, 1, 20, 0, SV6621_REGULATORY_FLAG_NO_OFDM },
      { 36, 13, 20, 0, SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 45, 23, 0, SV6621_REGULATORY_FLAG_DFS },
    },
  },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_wifi_initialize
 *
 * Description:
 *   Set up the SV6621 pin environment and hand off to the SeekWave core
 *   driver.  Muxes the SDIO bus at full drive, parks the BT-side companion
 *   pins the combo boot ROM samples (BT_RST low, BT_WAKE high, UART4 lines
 *   idle-high), enables the 32 kHz sleep clock, and brings up WiFi.
 ****************************************************************************/

int kickpi_k7_wifi_initialize(void)
{
  static bool initialized;
  static int init_result;
  struct sv6621_config_s config;
  int ret;
  int i;

  /* The pinmux / 32 kHz / I2C bring-up must run once: re-running it on a
   * live system races the running driver and hangs the I2C poll.
   */

  if (initialized)
    {
      return init_result;
    }

  /* Public CI embeds an exact-size, zero-filled set so the complete board
   * integration is compiled without redistributing restricted firmware.
   * Detect it before touching pins, clocks, or power.  This expected build
   * mode disables only Wi-Fi and must not turn an otherwise usable image
   * into a board-startup failure.
   */

  if (kickpi_k7_wifi_has_placeholders())
    {
      kickpi_k7_wifi_warn_placeholders();
      init_result = OK;
      initialized = true;
      return OK;
    }

  /* SDIO bus mux (GPIO1, func 2, pull-up) + max drive strength. */

  for (i = 0;
       i < (int)(sizeof(g_wifi_sdio_pins) / sizeof(g_wifi_sdio_pins[0])); i++)
    {
      ret = kickpi_k7_wifi_config_alt(
          g_wifi_sdio_pins[i], 2, RK3576_GPIO_PULLUP, &g_wifi_sdio_handles[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV0);
  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV1);
  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV2);
  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV3);

  /* Companion pin environment the SV6160lite boot ROM samples.  Keep the
   * Bluetooth side in reset while Wi-Fi boots, matching the last known-good
   * networking build; BT wake and the UART4 lines remain idle-high.
   */

  for (i = 0; i < (int)nitems(g_wifi_companion_pins); i++)
    {
      ret = kickpi_k7_wifi_config_output(g_wifi_companion_pins[i], i != 0,
                                         &g_wifi_companion_handles[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = kickpi_k7_wifi_config_output(WIFI_WL_REG_ON, false, &g_wifi_wl_reg_on);
  if (ret < 0)
    {
      return ret;
    }

  /* Enable the 32.768 kHz sleep clock via the PCF8563 driver (CLKOUT),
   * rather than driving the I2C2 bus manually, so the I2C bus / GPIO pin
   * claims remain owned solely by the RTC driver.
   */

  ret = pcf8563_clkout_set(PCF8563_CLKOUT_32768HZ);
  if (ret < 0)
    {
      wlwarn("WARNING: WiFi sleep clock setup failed: %d; continuing\n", ret);
    }

  memset(&config, 0, sizeof(config));
  config.transport = kickpi_k7_sv6621_transport();
  config.board_ops = &g_kickpi_k7_wifi_board_ops;
  config.board_arg = g_kickpi_k7_wifi_address;
  config.iram.data = g_sv6621_iram_start;
  config.iram.length = g_sv6621_iram_end - g_sv6621_iram_start;
  config.dram.data = g_sv6621_dram_start;
  config.dram.length = g_sv6621_dram_end - g_sv6621_dram_start;
  config.nvram.data = g_sv6621_nv_start;
  config.nvram.length = g_sv6621_nv_end - g_sv6621_nv_start;
  config.calibration.data = g_sv6621_calib_start;
  config.calibration.length = g_sv6621_calib_end - g_sv6621_calib_start;
  config.regulatory = &g_kickpi_k7_wifi_regulatory_domains[0];
  config.regulatory_domains = g_kickpi_k7_wifi_regulatory_domains;
  config.regulatory_domain_count =
      sizeof(g_kickpi_k7_wifi_regulatory_domains) /
      sizeof(g_kickpi_k7_wifi_regulatory_domains[0]);
#ifdef CONFIG_SV6621_PM
  config.system_suspend = g_kickpi_k7_wifi_suspend;
#endif

  ret = sv6621_create(&config, &g_kickpi_k7_wifi_dev);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_start(g_kickpi_k7_wifi_dev);
  if (ret < 0)
    {
      sv6621_destroy(g_kickpi_k7_wifi_dev);
      g_kickpi_k7_wifi_dev = NULL;
      return ret;
    }

#ifdef CONFIG_SV6621_PM
  ret = rk3576_gpio_get(WIFI_HOST_WAKE, &g_kickpi_k7_wifi_host_wake);
  if (ret < 0)
    {
      goto stop_driver;
    }

  rk3576_gpio_set_mode(g_kickpi_k7_wifi_host_wake, RK3576_GPIO_INPUT);
  rk3576_gpio_set_pull(g_kickpi_k7_wifi_host_wake, RK3576_GPIO_PULLDOWN);
  rk3576_gpio_set_schmitt(g_kickpi_k7_wifi_host_wake, true);
  rk3576_gpio_set_int_type(g_kickpi_k7_wifi_host_wake, RK3576_GPIO_INT_EDGE);
  rk3576_gpio_set_int_pol(g_kickpi_k7_wifi_host_wake,
                          RK3576_GPIO_INT_HIGH_RISING);

  ret = rk3576_gpio_irq_attach(g_kickpi_k7_wifi_host_wake,
                               kickpi_k7_wifi_host_wake_isr);
  if (ret < 0)
    {
      goto stop_driver;
    }

  rk3576_gpio_irq_enable(g_kickpi_k7_wifi_host_wake);

#endif

  init_result = ret;
  initialized = true;

  return ret;

#ifdef CONFIG_SV6621_PM
stop_driver:
  if (g_kickpi_k7_wifi_host_wake != NULL)
    {
      rk3576_gpio_irq_attach(g_kickpi_k7_wifi_host_wake, NULL);
      rk3576_gpio_put(g_kickpi_k7_wifi_host_wake);
      g_kickpi_k7_wifi_host_wake = NULL;
    }

  sv6621_stop(g_kickpi_k7_wifi_dev);
  sv6621_destroy(g_kickpi_k7_wifi_dev);
  g_kickpi_k7_wifi_dev = NULL;
  return ret;
#endif
}

#ifdef CONFIG_SV6621_PM
/****************************************************************************
 * Name: kickpi_k7_wifi_prepare_sleep
 ****************************************************************************/

int kickpi_k7_wifi_prepare_sleep(void)
{
  if (g_kickpi_k7_wifi_dev == NULL)
    {
      return -ENODEV;
    }

  return sv6621_suspend(g_kickpi_k7_wifi_dev, &g_kickpi_k7_wifi_suspend);
}

/****************************************************************************
 * Name: kickpi_k7_wifi_abort_sleep
 ****************************************************************************/

int kickpi_k7_wifi_abort_sleep(void)
{
  if (g_kickpi_k7_wifi_dev == NULL)
    {
      return -ENODEV;
    }

  return sv6621_resume(g_kickpi_k7_wifi_dev);
}
#endif

#endif /* CONFIG_KICKPI_K7_WIFI */
