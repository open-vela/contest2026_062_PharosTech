/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/src/kickpi_k7_lcd.c
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
#include <nuttx/config.h>
#include <stdbool.h>

#ifdef CONFIG_KICKPI_K7_LCD

#include "rk3576_gpio.h"
#include <errno.h>
#include <nuttx/ioexpander/gpio.h>
#include <nuttx/spi/qspi.h>
#include <semaphore.h>
#include <string.h>
#include <sys/boardctl.h>
#include <syslog.h>

#include "rk3576_fspi.h"

#include "st77916.h"
#include <nuttx/clk/clk.h>
#include <nuttx/signal.h>

#include <nuttx/lcd/lcd_dev.h>

#include "rk3576_pwm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* FSPI1 pins are muxed on GPIO2_A with IOMUX alternate function AF2.  The
 * pinset macros carry only the pin identity (port + pin); the AF is applied
 * via rk3576_gpio_set_af().  NOTE: these pins share GPIO2_A with SDMMC, so
 * the SD card must be removed before any FSPI operation.
 */

#define GPIO_FSPI1_D0   (GPIO_PORT2 | GPIO_PIN_A0)
#define GPIO_FSPI1_D1   (GPIO_PORT2 | GPIO_PIN_A1)
#define GPIO_FSPI1_D2   (GPIO_PORT2 | GPIO_PIN_A2)
#define GPIO_FSPI1_D3   (GPIO_PORT2 | GPIO_PIN_A3)
#define GPIO_FSPI1_CSN0 (GPIO_PORT2 | GPIO_PIN_A4)
#define GPIO_FSPI1_CLK  (GPIO_PORT2 | GPIO_PIN_A5)

#define GPIO_LCD_SEL    (GPIO_PORT4 | GPIO_PIN_A6)
#define GPIO_LCD_RST    (GPIO_PORT4 | GPIO_PIN_A4)

/* TE (vertical-sync) pins.  The panels output an HSYNC TE which the on-board
 * CPLD converts to a per-screen VSYNC TE before routing it to these GPIOs:
 *   - LCD0 TE -> GPIO3_C6
 *   - LCD1 TE -> GPIO3_C5
 * The VSYNC TE is a level signal: HIGH = blanking, LOW = scanning.  Both
 * edges carry a meaningful event (rising = blank-start, falling =
 * scan-start), so the pins are configured as both-edge interrupt inputs
 * with a driver-level callback (kickpi_k7_te_int_handler) that classifies
 * the edge direction at interrupt time and posts the per-direction
 * semaphore registered by the application. */

#define GPIO_LCD_TE0 (GPIO_PORT3 | GPIO_PIN_C6)
#define GPIO_LCD_TE1 (GPIO_PORT3 | GPIO_PIN_C5)

/* IOMUX alternate function of the FSPI1 signal group */

#define KICKPI_K7_LCD_FSPI1_AF 2

/* Backlight control.  The display backlight is driven by GPIO2_D7, which
 * is muxed to PWM2 CH7 (PWM2_CH7_M2, IOMUX alternate function 0xd = 13).
 * The backlight is active-low: HIGH = off, LOW = on.  The RK3576 PWM
 * channel outputs active-high by default, so the user layer controls
 * brightness by inverting the duty (duty = full → off, duty = 0 → fully
 * on).  The channel is registered as /dev/pwm0.
 */

#define GPIO_LCD_BACKLIGHT               (GPIO_PORT2 | GPIO_PIN_D7)
#define KICKPI_K7_LCD_BACKLIGHT_AF       13

#define KICKPI_K7_LCD_BACKLIGHT_PWM_CTRL 2
#define KICKPI_K7_LCD_BACKLIGHT_PWM_CH   7

#define KICKPI_K7_LCD_BACKLIGHT_DEVPATH  "/dev/pwm0"

/* Initial PWM output frequency (Hz).  Only meaningful until the user layer
 * reconfigures the channel; 10 kHz is a comfortable LED/backlight rate.
 */

#define KICKPI_K7_LCD_BACKLIGHT_INIT_FREQ 10000

/* Claim a pin (once) and mux it to the given IOMUX alternate function.
 * The handle is retained for the lifetime of the FSPI/LCD peripheral, so
 * it is never released here.
 */

#define _SET_GPIO_AF(pinset, af)                        \
  do                                                    \
    {                                                   \
      static FAR struct gpio_dev_s *handle;             \
      if (handle == NULL)                               \
        {                                               \
          ret = rk3576_gpio_get((pinset), &handle);     \
          if (ret < 0)                                  \
            {                                           \
              syslog(LOG_ERR,                           \
                     "ERROR: failed to get fspi gpio, " \
                     "pinset: %u\n",                    \
                     (pinset));                         \
              return ret;                               \
            }                                           \
        }                                               \
      rk3576_gpio_set_af(handle, (af));                 \
    }                                                   \
  while (0)

/* qspi frequency */
#define LCD_QSPI_FREQ 80000000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* LCD device pointers for dual-screen support */

static FAR struct lcd_dev_s *g_lcd[2];

/* GPIO handles for the LCD panel/reset GPIOs (claimed in
 * kickpi_k7_lcd_initialize and kept for the driver lifetime). */

static FAR struct gpio_dev_s *g_lcd_sel_gpio;
static FAR struct gpio_dev_s *g_lcd_rst_gpio;

/* TE (vertical-sync) interrupt-capable GPIO handles.  Claimed and kept for
 * the LCD driver lifetime; registered as /dev/gpio1 and /dev/gpio2. */

static FAR struct gpio_dev_s *g_lcd_te_gpio[2];

/* GPIO handle for the backlight PWM pin (GPIO2_D7, claimed and kept for the
 * driver lifetime). */

static FAR struct gpio_dev_s *g_lcd_backlight_gpio;

static void lcd0_select_cb(bool sel)
{
  if (sel)
    {
      rk3576_gpio_write_bit(g_lcd_sel_gpio, false);
    }
}

static void lcd1_select_cb(bool sel)
{
  if (sel)
    {
      rk3576_gpio_write_bit(g_lcd_sel_gpio, true);
    }
}

/* TE pin driver-level interrupt callback.  Signature mirrors NuttX
 * pin_interrupt_t / rk3576_gpio_irq_callback_t.  It identifies the screen by
 * matching the dev pointer against g_lcd_te_gpio[], reads the pin level to
 * classify the edge direction at ISR time, and posts the matching semaphore
 * registered by the application.  Runs in interrupt context: only
 * sem_post() (interrupt-safe) and rk3576_gpio_read_bit() are used. */

static int kickpi_k7_te_int_handler(FAR struct gpio_dev_s *dev, uint8_t pin)
{
  int sid;
  bool level;
#ifdef CONFIG_BOARDCTL_IOCTL
  FAR sem_t *sem;
#endif

  UNUSED(pin);

  sid = (dev == g_lcd_te_gpio[1]) ? 1 : 0;

  /* Read the pin level now, while the edge is fresh: HIGH = blanking,
   * LOW = scanning. */

  level = rk3576_gpio_read_bit(dev);

#ifdef CONFIG_BOARDCTL_IOCTL
  /* Select the matching sem: bs = rising (blanking begins), ss = falling
   * (scan begins).  Layout: index = sid*2 + (bs ? 0 : 1). */

  sem = g_te_sem[(sid << 1) | (level ? 0 : 1)];
  if (sem != NULL)
    {
      sem_post(sem);
    }
#endif

  return OK;
}

/* Claim one TE pin and attach it as a both-edge interrupt with a
 * driver-level callback.  The panels output an HSYNC TE; the on-board CPLD
 * converts it to a per-screen VSYNC TE before arriving here (HIGH blanking,
 * LOW scanning), so both edges carry a meaningful event.  The handle is
 * retained in g_lcd_te_gpio[] for the LCD driver lifetime. */

static int kickpi_k7_lcd_te_register(int sid)
{
  gpio_pinset_t pinset = (sid == 0) ? GPIO_LCD_TE0 : GPIO_LCD_TE1;
  FAR struct gpio_dev_s *handle;
  int ret;

  ret = rk3576_gpio_get(pinset, &handle);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to get LCD TE%d gpio: %d\n", sid, ret);
      return ret;
    }

  /* Configure as an input, both-edge external interrupt. */

  rk3576_gpio_set_mode(handle, RK3576_GPIO_INPUT);
  rk3576_gpio_set_pull(handle, RK3576_GPIO_FLOAT);
  rk3576_gpio_set_int_type(handle, RK3576_GPIO_INT_EDGE);
  rk3576_gpio_set_int_pol(handle, RK3576_GPIO_INT_BOTH_EDGE);

  /* Attach the driver-level interrupt callback (posting the app sem) and
   * keep the handle for the ISR's screen identification. */

  ret = rk3576_gpio_irq_attach(handle, kickpi_k7_te_int_handler);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TE%d irq_attach failed: %d\n", sid, ret);
      rk3576_gpio_put(handle);
      return ret;
    }

  rk3576_gpio_irq_enable(handle);

  g_lcd_te_gpio[sid] = handle;

  syslog(LOG_INFO, "LCD TE%d: irq attached (%p)\n", sid, handle);

  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_lcd_backlight_register
 *
 * Description:
 *   Mux the backlight pin (GPIO2_D7 -> PWM2 CH7, alternate function 13)
 *   and register the RK3576 PWM channel as the /dev/pwm0 character device
 *   for the user layer to drive the display backlight.
 *
 *   The backlight is active-low (HIGH = off, LOW = on) while the RK3576
 *   PWM channel outputs active-high, so the caller controls brightness by
 *   inverting the duty cycle: duty = 65536 (full) turns the backlight off,
 *   duty = 0 turns it fully on.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any
 *   failure.
 *
 ****************************************************************************/

static int kickpi_k7_lcd_backlight_register(void)
{
  FAR struct pwm_lowerhalf_s *pwm;
  struct pwm_info_s info;
  int ret;

  /* Claim the backlight pin and mux it to PWM2_CH7_M2 (AF 13). */

  ret = rk3576_gpio_get(GPIO_LCD_BACKLIGHT, &g_lcd_backlight_gpio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to get backlight gpio: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_af(g_lcd_backlight_gpio, KICKPI_K7_LCD_BACKLIGHT_AF);

  /* Get the PWM2 CH7 lower-half and register it as /dev/pwm0. */

  pwm = rk3576_pwm_initialize(KICKPI_K7_LCD_BACKLIGHT_PWM_CTRL,
                              KICKPI_K7_LCD_BACKLIGHT_PWM_CH);
  if (pwm == NULL)
    {
      syslog(LOG_ERR, "ERROR: rk3576_pwm_initialize(PWM2 CH%d) failed\n",
             KICKPI_K7_LCD_BACKLIGHT_PWM_CH);
      return -ENODEV;
    }

  /* Set the initial output to full duty (active-high, so HIGH = backlight
   * off).  This keeps the backlight dark until the user layer explicitly
   * drives it.
   */

  memset(&info, 0, sizeof(info));
  info.frequency = KICKPI_K7_LCD_BACKLIGHT_INIT_FREQ;
  info.duty = 0; /* 0 % duty, backlight fully on */

  ret = pwm->ops->start(pwm, &info);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: backlight initial PWM start failed: %d\n", ret);
      return ret;
    }

  ret = pwm_register(KICKPI_K7_LCD_BACKLIGHT_DEVPATH, pwm);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: pwm_register(%s) failed: %d\n",
             KICKPI_K7_LCD_BACKLIGHT_DEVPATH, ret);
      return ret;
    }

  syslog(LOG_INFO, "LCD backlight: %s registered (PWM2 CH%d)\n",
         KICKPI_K7_LCD_BACKLIGHT_DEVPATH, KICKPI_K7_LCD_BACKLIGHT_PWM_CH);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_lcd_initialize
 *
 * Description:
 *   Initialize the ST77916 LCD panel over the FSPI1 CS0 (QSPI) bus.
 *   This is called from board_app_initialize() when CONFIG_KICKPI_K7_LCD
 *   is enabled.
 *
 *   NOTE: FSPI1 pins and SDMMC share GPIO2_A, so the SD card must be
 *   removed before any FSPI operation.  The GPIO configuration is safe
 *   at boot (the SDMMC driver only touches pins when a card is inserted),
 *   but the actual SPI transfers are deferred until this function is called.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 ****************************************************************************/

int kickpi_k7_lcd_initialize(void)
{
  FAR struct qspi_dev_s *qspi;
  FAR struct lcd_dev_s *lcd0;
  FAR struct lcd_dev_s *lcd1;
  int ret;

  /* Register the backlight PWM (GPIO2_D7 -> PWM2 CH7) as /dev/pwm0 for the
   * user layer to drive the display backlight. */

  ret = kickpi_k7_lcd_backlight_register();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ST77916: backlight register failed: %d\n", ret);
      return ret;
    }

  /* Mux the FSPI1 data/control pins to their IOMUX alternate function
   * (AF2).  Note that FSPI1 pins share GPIO2_A with SDMMC.
   */

  _SET_GPIO_AF(GPIO_FSPI1_D0, KICKPI_K7_LCD_FSPI1_AF);
  _SET_GPIO_AF(GPIO_FSPI1_D1, KICKPI_K7_LCD_FSPI1_AF);
  _SET_GPIO_AF(GPIO_FSPI1_D2, KICKPI_K7_LCD_FSPI1_AF);
  _SET_GPIO_AF(GPIO_FSPI1_D3, KICKPI_K7_LCD_FSPI1_AF);
  _SET_GPIO_AF(GPIO_FSPI1_CSN0, KICKPI_K7_LCD_FSPI1_AF);
  _SET_GPIO_AF(GPIO_FSPI1_CLK, KICKPI_K7_LCD_FSPI1_AF);

  /* sel - panel select output.  The handle is kept for the select
   * callbacks (lcd0/lcd1_select_cb).
   */

  ret = rk3576_gpio_get(GPIO_LCD_SEL, &g_lcd_sel_gpio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to get LCD_SEL gpio: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_mode(g_lcd_sel_gpio, RK3576_GPIO_OUTPUT);

  /* rst - LCD reset output, driven high (out of reset) */

  ret = rk3576_gpio_get(GPIO_LCD_RST, &g_lcd_rst_gpio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to get LCD_RST gpio: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_mode(g_lcd_rst_gpio, RK3576_GPIO_OUTPUT);

  /* Assert reset and hold for 100 us */
  rk3576_gpio_write_bit(g_lcd_rst_gpio, false);
  nxsig_usleep(100);

  /* Deassert reset */
  rk3576_gpio_write_bit(g_lcd_rst_gpio, true);

  /* Wait for the initiallization of CPLD and LCD controller  */
  nxsig_usleep(20000);

  qspi = rk3576_fspi_initialize(1, 0);

  if (qspi == NULL)
    {
      syslog(LOG_ERR, "ST77916: rk3576_fspi_initialize() returned NULL\n");
      return -ENODEV;
    }

  QSPI_LOCK(qspi, true);
  QSPI_SETFREQUENCY(qspi, LCD_QSPI_FREQ);
  QSPI_LOCK(qspi, false);

  /* Initialize lcd0 */

  lcd0 =
      st77916_lcdinitialize(qspi, FB_FMT_RGB16_565, 360, 360, 0, 0, 0, false,
                            true, ST77916_TE_MODE_HSYNC, lcd0_select_cb);
  if (lcd0 == NULL)
    {
      syslog(LOG_ERR, "ST77916: st77916_lcdinitialize() failed\n");
      return -ENODEV;
    }
  g_lcd[0] = lcd0;

  ret = lcddev_register(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ST77916: lcddev_register(/dev/lcd0) failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "ST77916: /dev/lcd0 registered (%p)\n", lcd0);

  /* Initialize lcd1 */

  lcd1 =
      st77916_lcdinitialize(qspi, FB_FMT_RGB16_565, 360, 360, 0, 0, 0, false,
                            true, ST77916_TE_MODE_HSYNC, lcd1_select_cb);
  if (lcd1 == NULL)
    {
      syslog(LOG_ERR, "ST77916: st77916_lcdinitialize() failed\n");
      return -ENODEV;
    }
  g_lcd[1] = lcd1;

  ret = lcddev_register(1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ST77916: lcddev_register(/dev/lcd1) failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "ST77916: /dev/lcd1 registered (%p)\n", lcd1);

  /* Register the TE (vertical-sync) pins as both-edge interrupts with a
   * driver-level callback that posts the application's per-edge semaphores
   * (registered later via board_ioctl/BOARDIOC_USER).  Consumed by the
   * nyabula_display app as its vertical-sync source. */

  ret = kickpi_k7_lcd_te_register(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ST77916: TE0 register failed: %d\n", ret);
      return ret;
    }

  ret = kickpi_k7_lcd_te_register(1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ST77916: TE1 register failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: board_lcd_getdev
 *
 * Description:
 *   Return a reference to the LCD object for the specified LCD.
 *   This allows support for multiple LCD devices (dual-screen).
 *
 * Input Parameters:
 *   lcddev - LCD device number (0 or 1)
 *
 * Returned Value:
 *   LCD device pointer if success or NULL if failed.
 *
 ****************************************************************************/

#ifdef CONFIG_LCD
FAR struct lcd_dev_s *board_lcd_getdev(int lcddev)
{
  if (lcddev < 0 || lcddev > 1 || g_lcd[lcddev] == NULL)
    {
      syslog(LOG_ERR, "board_lcd_getdev: invalid lcddev %d\n", lcddev);
      return NULL;
    }

  return g_lcd[lcddev];
}
#endif

#endif /* CONFIG_KICKPI_K7_LCD */
