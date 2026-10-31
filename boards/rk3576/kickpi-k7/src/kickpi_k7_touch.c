/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_touch.c
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
 * On-board Goodix GT911 capacitive touch controller bring-up for the
 * kickpi-k7 (RK3576).
 *
 * Wiring (from the board schematic; pin mux confirmed against RK3576 TRM
 * Part1 "PMU1_IOC_GPIO0C/0D_IOMUX_SEL" and the I2C interface table):
 *   - I2C0 in M1 muxing:
 *       SCL = GPIO0_C1, AF9 = I2C0_SCL_M1
 *       SDA = GPIO0_C2, AF9 = I2C0_SDA_M1
 *     I2C0 is otherwise unused on this board: the RK806 PMIC is on I2C1,
 *     the PCF8563/HYM8563TS RTC on I2C2 and the ES8388 codec on I2C3.
 *   - TP_INT = GPIO0_C5 (controller interrupt; the module's table selects a
 *     falling edge and the schematic pulls the line up, so the idle level is
 *     high).  The driver arms both edges, so the polarity does not matter
 *     either way.
 *   - TP_RST = GPIO0_D0 (active low)
 *
 * The controller is on the same module as the MIPI DSI panel and is powered
 * by the panel supply, so this file owns only the reset and interrupt lines;
 * it does not drive the panel's PWREN (kickpi_k7_mipi_dsi.c does, before
 * calling in here).
 *
 * GPIO0_C5 and GPIO0_D0 both live in the PMU1_IOC block (GPIO0C/GPIO0D) and
 * are not claimed by any other board function.
 *
 * Slave address: the GT911 latches its I2C address from the level of INT while
 * RST is released (datasheet 4.2).  This board drives INT high for the whole
 * reset window and therefore talks to it at 0x14; see the *_ADDR macros in
 * drivers/include/gt911.h.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/irq.h>

#include "gt911.h"
#include "kickpi_k7.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"

#ifdef CONFIG_KICKPI_K7_TOUCH

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C0 in M1 muxing (AF9) */

#define KICKPI_K7_TP_I2C_BUS 0
#define KICKPI_K7_TP_SCL_AF  9
#define KICKPI_K7_TP_SDA_AF  9

#define KICKPI_K7_TP_SCL_PIN (GPIO_PORT0 | GPIO_PIN_C1)
#define KICKPI_K7_TP_SDA_PIN (GPIO_PORT0 | GPIO_PIN_C2)

/* Reset and interrupt lines */

#define KICKPI_K7_TP_INT_PIN (GPIO_PORT0 | GPIO_PIN_C5)
#define KICKPI_K7_TP_RST_PIN (GPIO_PORT0 | GPIO_PIN_D0)

/* The address the controller latches is not a strapping pin: it is read from
 * the level of INT while RST is released (datasheet 4.2), high -> 0x14 and
 * low -> 0x5d.  The level driven during reset is therefore derived from the
 * address this board wants, so the two cannot drift apart.
 */

#define KICKPI_K7_TP_ADDR     GT911_I2C_ADDR_INT_HIGH
#define KICKPI_K7_TP_ADDR_INT (KICKPI_K7_TP_ADDR == GT911_I2C_ADDR_INT_HIGH)

/* Reset timing.
 *
 * The datasheet's figure (sections 4.1-4.3) gives RST low as "more than
 * 100 us", RST released as "more than 5 ms" and INT released as "more than
 * 50 ms before the master may talk".  The two independent drivers that are
 * known to bring this controller up on real panels are more generous about the
 * first window: Linux mainline's goodix_reset_no_int_sync() holds RST low for
 * msleep(20) with the comment "T2: > 10ms", and Goodix's own driver resets
 * with the same 20 ms.
 *
 * More importantly, both of them hold INT LOW for 50 ms after releasing reset
 * (mainline's goodix_int_sync(), "T5: 50ms") before handing it back to the
 * controller, and that handshake is what makes the difference on this board.
 * Without it the controller answers every I2C read -- product id, firmware
 * version, even a configuration written into it and read back byte for byte --
 * and still never starts its sensing engine: no coordinate range, no frames,
 * no interrupt, which looks exactly like a module that was delivered without a
 * configuration.  See the KICKPI_K7_TOUCH_CFG help text for how that was
 * wrongly diagnosed for a while.
 */

#define KICKPI_K7_TP_RESET_LOW_MS  20
#define KICKPI_K7_TP_RESET_HIGH_MS 10
#define KICKPI_K7_TP_INT_SYNC_MS   50
#define KICKPI_K7_TP_BOOT_MS       50

/* Device node.  By convention the touchscreen framework registers at
 * /dev/inputN, and LVGL defaults to /dev/input0 (see the LVGL Kconfig option
 * LV_USE_NUTTX_TOUCHSCREEN and lvgldemo's
 * CONFIG_EXAMPLES_LVGLDEMO_INPUT_DEVPATH).
 */

#define KICKPI_K7_TP_DEVPATH "/dev/input0"

/* How the module's coordinates have to be mapped onto the display.
 *
 * The panel is a physical part of the enclosure, so "which way round is it" is
 * one fact with two consequences -- the picture has to be mirrored and so do
 * the coordinates -- and the board states it once, as
 * KICKPI_K7_PANEL_ORIENTATION.  The two derived symbols below are the same
 * ones kickpi_k7_mipi_dsi.c inverts its MADCTL scan bits with, which is what
 * keeps the two ends from drifting apart.
 *
 * IS_ENABLED() is not available outside the devicetree helpers, hence the
 * conditionals.
 */

#ifdef CONFIG_KICKPI_K7_PANEL_MIRROR_X
#define KICKPI_K7_TP_MIRRORX true
#else
#define KICKPI_K7_TP_MIRRORX false
#endif

#ifdef CONFIG_KICKPI_K7_PANEL_MIRROR_Y
#define KICKPI_K7_TP_MIRRORY true
#else
#define KICKPI_K7_TP_MIRRORY false
#endif

/* The controller's own frame, and how it maps onto the display's.
 *
 * swapxy stays false: the module's configuration table sets the X2Y bit
 * (0x804d = 0x35), so the controller already exchanges the axes itself and a
 * second swap here would put them back.
 *
 * xres/yres describe the frame the sensor is configured for, i.e. the table's
 * X/Y Output Max, and are only a fallback -- the driver prefers the range the
 * controller reports about itself at probe time, because that is the frame the
 * coordinates actually arrive in.
 */

static const struct gt911_orientation_s g_kickpi_k7_tp_orientation = {
  .xres = 720,
  .yres = 1280,
  .swapxy = false,
  .mirrorx = KICKPI_K7_TP_MIRRORX,
  .mirrory = KICKPI_K7_TP_MIRRORY,
};

#ifdef CONFIG_KICKPI_K7_TOUCH_CFG_VENDOR

/* Module configuration table.
 *
 * The GT911 stores its sensing configuration (X/Y output range, contact limit,
 * sensing channel map, frequency, gains) in its own non-volatile memory, and a
 * module can ship with that area blank: it answers every I2C read and senses
 * nothing, because with no output range and no channel map there is no scan --
 * no coordinate frame and no interrupt, which is indistinguishable from a
 * broken driver.
 *
 * This module did NOT arrive blank: once the reset sequence was right (see
 * below) it reported its own configuration, and the driver left it alone.  The
 * table is still kept here because the driver only downloads it when the
 * controller has nothing usable, so it costs nothing and it is what a blank
 * module would need.
 *
 * The table below is the panel vendor's own configuration for this module,
 * "GT911_Config_720X1280_0x49" (2025-11-05).  It is the 184-byte payload of
 * registers 0x8047..0x80fe; the driver derives the checksum (0x80ff) and sets
 * the apply flag (0x8100).  The vendor's value for that checksum, 0xc8, is
 * what (0 - sum) & 0xff produces over this payload, which is also how the
 * vendor's own Linux tables are built -- the driver's implementation is
 * checked against it rather than assumed.
 *
 * What the table says:
 *   - X output max 720, Y output max 1280: the controller is configured for
 *     portrait, i.e. the same frame as the display, so the coordinates need no
 *     rescaling.  These are the values in g_kickpi_k7_tp_orientation above.
 *   - 0x804d = 0x35 has the X2Y bit set, so the CONTROLLER exchanges the axes
 *     itself before reporting, which is why the orientation's swapxy is off.
 *   - Contact limit 5, which is what a GT911 supports and what
 *     INPUT_GT911_MAX_POINTS reports.
 *
 * The 180 degree mounting of this enclosure is NOT in the table: it is a
 * property of how the glass sits in the product, and it reaches the driver as
 * the orientation argument (see above) rather than as part of the module's own
 * configuration.
 *
 * Version 0x49 is also what governs the order in which tables may be applied:
 * the controller only stores a configuration whose version is at least the one
 * it already holds, so this table works on a blank module while a higher
 * version written first would lock it out permanently.
 */

static const uint8_t g_kickpi_k7_tp_config[] = {
  0x49, 0xd0, 0x02, 0x00, 0x05, 0x05, 0x35, 0x00, 0x01, 0x08, 0x1e, 0x0a, 0x50,
  0x3c, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x1a, 0x1e,
  0x14, 0x8a, 0x2a, 0x0c, 0x96, 0x98, 0xb2, 0x04, 0x00, 0x00, 0x00, 0x98, 0x02,
  0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78,
  0xaa, 0x94, 0xc5, 0x02, 0x07, 0x00, 0x00, 0x04, 0x96, 0x7c, 0x00, 0x8d, 0x85,
  0x00, 0x86, 0x8e, 0x00, 0x7d, 0x99, 0x00, 0x76, 0xa4, 0x00, 0x76, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x16, 0x14, 0x12, 0x10,
  0x0e, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
  0x02, 0x04, 0x06, 0x08, 0x0a, 0x0f, 0x10, 0x12, 0x13, 0x24, 0x22, 0x21, 0x20,
  0x1f, 0x1e, 0x1d, 0x1c, 0x18, 0x16, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff,
};

/* Size check: the driver rejects a table of any other length, and a wrong one
 * here would otherwise only show up as a runtime error at boot.
 */

_Static_assert(sizeof(g_kickpi_k7_tp_config) == GT911_CONFIG_LEN,
               "touch configuration table must be GT911_CONFIG_LEN bytes");

#if defined(CONFIG_KICKPI_K7_TOUCH_CFG_ALTERNATES)

/* The vendor's other tables, kept as candidates.
 *
 * The vendor's Linux device trees in the Debian image shipped for this board
 * carry one table per panel variant, and the panel whose init sequence matches
 * this board's panel is paired there with an 800 x 1280 sensor table rather
 * than with the 720 x 1280 table supplied to us separately.  A configuration
 * describes the touch sensor -- its channel map, frequency and gains -- so
 * only the vendor can attribute a sensor to a module, and picking the wrong
 * table produces exactly the state this board shows: a controller that
 * answers, accepts and applies a table and still never starts scanning.
 *
 * Nothing is lost by trying them: this module does not retain a configuration
 * across a reset, so no version ordering can be locked in by an attempt, and
 * the driver reports a table as correct only when the controller's engine
 * starts reporting a coordinate range.
 */

static const uint8_t g_kickpi_k7_tp_config_alt_800p10[] = {
  0x00, 0x20, 0x03, 0x00, 0x05, 0x0a, 0x05, 0x00, 0x01, 0x08, 0x28, 0x05, 0x50,
  0x32, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x8b, 0x2b, 0x0e, 0x17, 0x15, 0x31, 0x0d, 0x00, 0x00, 0x01, 0xbb, 0x03,
  0x2d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x64, 0x32, 0x00, 0x00, 0x00, 0x0f,
  0x23, 0x94, 0xc5, 0x02, 0x07, 0x00, 0x00, 0x04, 0xa0, 0x10, 0x00, 0x8b, 0x13,
  0x00, 0x7c, 0x16, 0x00, 0x6a, 0x1b, 0x00, 0x5e, 0x20, 0x00, 0x5e, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x1a, 0x18, 0x16, 0x14,
  0x12, 0x10, 0x0e, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0f, 0x10, 0x12, 0x13, 0x16, 0x26, 0x24,
  0x22, 0x21, 0x20, 0x1f, 0x1e, 0x1d, 0x1c, 0x18, 0xff, 0xff, 0xff, 0xff, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00,
};

static const uint8_t g_kickpi_k7_tp_config_alt_800p5[] = {
  0x70, 0x20, 0x03, 0x00, 0x05, 0x05, 0x35, 0x00, 0x01, 0x0a, 0x28, 0x0f, 0x5a,
  0x3c, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x18, 0x1a, 0x1e,
  0x14, 0x8c, 0x2e, 0x0e, 0xb0, 0xb2, 0xb2, 0x04, 0x00, 0x00, 0x00, 0x98, 0x02,
  0x1c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x64, 0x32, 0x00, 0x00, 0x00, 0xa0,
  0xf5, 0x94, 0xd5, 0x02, 0x07, 0x00, 0x00, 0x04, 0x83, 0xa7, 0x00, 0x7e, 0xb5,
  0x00, 0x78, 0xc6, 0x00, 0x73, 0xd7, 0x00, 0x6f, 0xea, 0x00, 0x6f, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x1a, 0x18, 0x16, 0x14,
  0x12, 0x10, 0x0e, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
  0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0f, 0x10, 0x12, 0x13, 0x14, 0x16, 0x2a,
  0x29, 0x28, 0x26, 0x24, 0x22, 0x21, 0x20, 0x1f, 0x1e, 0x1d, 0x1c, 0x18, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff,
};

#endif /* CONFIG_KICKPI_K7_TOUCH_CFG_ALTERNATES */

/* Candidate configuration tables, in the order the driver should try them. The
 * first entry is the table the panel vendor supplied for this module; it
 * should be right, but nothing in the bring-up log so far confirms the
 * controller is using it, so the vendor's other tables follow as candidates
 * rather than being discarded.  Which entry works is decided at run time by
 * the controller's own registers and reported by name.
 */

static const struct gt911_config_s g_kickpi_k7_tp_configs[] = {
  { "panel vendor 720x1280 v0x49", g_kickpi_k7_tp_config,
    sizeof(g_kickpi_k7_tp_config) },

#if defined(CONFIG_KICKPI_K7_TOUCH_CFG_ALTERNATES)
  { "vendor-DT 800x1280 v0x00", g_kickpi_k7_tp_config_alt_800p10,
    sizeof(g_kickpi_k7_tp_config_alt_800p10) },
  { "vendor-DT 800x1280 v0x70", g_kickpi_k7_tp_config_alt_800p5,
    sizeof(g_kickpi_k7_tp_config_alt_800p5) },
#endif
};

#endif /* CONFIG_KICKPI_K7_TOUCH_CFG_VENDOR */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Claimed GPIO handles.  They are deliberately never released: the pins stay
 * owned by the touch controller for the lifetime of the system, and holding
 * them keeps a second consumer from stealing them.
 */

static FAR struct gpio_dev_s *g_tp_scl;
static FAR struct gpio_dev_s *g_tp_sda;
static FAR struct gpio_dev_s *g_tp_int;
static FAR struct gpio_dev_s *g_tp_rst;

/* Interrupt bridge state.  The GT911 driver hands over an xcpt_t handler, but
 * the RK3576 GPIO layer delivers rk3576_gpio_irq_callback_t
 * (struct gpio_dev_s *, pin); the two are bridged through these.
 */

static xcpt_t g_tp_isr;
static FAR void *g_tp_isr_arg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_touch_int_handler
 *
 * Description:
 *   Driver-level GPIO interrupt callback for the TP_INT pin.  Bridges the
 *   (gpio_dev_s *, pin) signature the RK3576 GPIO layer invokes to the
 *   chip-agnostic xcpt_t handler the GT911 driver supplied.  Runs in
 *   interrupt context; must not sleep.
 *
 ****************************************************************************/

static int kickpi_k7_touch_int_handler(FAR struct gpio_dev_s *dev, uint8_t pin)
{
  UNUSED(dev);
  UNUSED(pin);

  if (g_tp_isr != NULL)
    {
      g_tp_isr(0, NULL, g_tp_isr_arg);
    }

  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_touch_set_power
 *
 * Description:
 *   Reset the controller, driving INT to the level that selects its I2C
 *   address, and leave both lines in their operating state: RST released and
 *   INT a floating input.
 *
 *   Re-running this is a power cycle of the controller, which is what the
 *   driver's set_power(false)/set_power(true) pair means; the address is
 *   re-latched identically each time.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int kickpi_k7_touch_set_power(FAR const struct gt911_board_s *state,
                                     bool on)
{
  UNUSED(state);

  if (!on)
    {
      /* Hold the controller in reset.  The supply is shared with the DSI
       * panel, so this is all this board can do about "power off".
       */

      rk3576_gpio_set_mode(g_tp_rst, RK3576_GPIO_OUTPUT);
      rk3576_gpio_write_bit(g_tp_rst, false);
      return OK;
    }

  /* Step 1: assert reset.  The address is latched from the INT level, so INT
   * must be driven to the wanted level (high -> 0x14) while RST is low and
   * stay there until after RST is released -- which is why the level is set
   * here, after RST is already low, and not before.
   */

  rk3576_gpio_set_mode(g_tp_rst, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_tp_rst, false);

  /* Step 2: hold reset low. */

  up_mdelay(KICKPI_K7_TP_RESET_LOW_MS);

  /* Step 3: take control of INT and drive the address-select level. */

  rk3576_gpio_set_mode(g_tp_int, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_tp_int, KICKPI_K7_TP_ADDR_INT);
  up_udelay(200); /* T3: > 100 us */

  /* Step 4: release reset and give the controller more than 5 ms to finish
   * its internal start-up.
   */

  rk3576_gpio_write_bit(g_tp_rst, true);
  up_mdelay(KICKPI_K7_TP_RESET_HIGH_MS);

  /* Step 5: INT sync.  Both mainline (goodix_int_sync(), "T5: 50ms") and
   * Goodix's own driver hold INT LOW for 50 ms after the reset and only then
   * release it to the controller.  This is a handshake rather than a delay:
   * nothing in the datasheet's timing table asks for it, and a driver can skip
   * it and still read a valid product id over I2C -- which is exactly the
   * state this board was in, with the controller answering every register read
   * while its sensing engine never started.
   */

  rk3576_gpio_set_mode(g_tp_int, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_tp_int, false);
  up_mdelay(KICKPI_K7_TP_INT_SYNC_MS);

  /* Step 6: release INT.  The datasheet requires it to float -- with an
   * internal pull it would fight the controller's own drive -- and the
   * controller output is only sampled as an interrupt, so no pull is wanted.
   * The board's own pull-up holds the line at its idle level, which this
   * module's configuration (0x804d bits 1:0 = 01) reads as a falling edge; the
   * driver arms both edges, so the polarity does not have to match.
   */

  rk3576_gpio_set_mode(g_tp_int, RK3576_GPIO_INPUT);
  rk3576_gpio_set_pull(g_tp_int, RK3576_GPIO_FLOAT);
  up_udelay(200);

  /* Step 7: the controller runs its auto-calibration during start-up; the
   * datasheet allows the master to start talking 50 ms after INT is released,
   * and an earlier read can find the bus unresponsive.
   */

  up_mdelay(KICKPI_K7_TP_BOOT_MS);

  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_touch_irq_attach
 *
 * Description:
 *   Configure the TP_INT pin as a floating input and route the driver's
 *   handler into the RK3576 GPIO interrupt.
 *
 *   Both edges are armed.  The controller pulses INT once per scan period
 *   while a frame is unread, so either edge is a valid "read me now" trigger;
 *   arming both means the driver does not depend on the trigger edge the
 *   module happens to be configured for (register 0x804d).
 *
 ****************************************************************************/

static int kickpi_k7_touch_irq_attach(FAR const struct gt911_board_s *state,
                                      xcpt_t isr, FAR void *arg)
{
  int ret;

  UNUSED(state);

  DEBUGASSERT(isr != NULL);

  rk3576_gpio_set_mode(g_tp_int, RK3576_GPIO_INPUT);
  rk3576_gpio_set_pull(g_tp_int, RK3576_GPIO_FLOAT);
  rk3576_gpio_set_int_type(g_tp_int, RK3576_GPIO_INT_EDGE);
  rk3576_gpio_set_int_pol(g_tp_int, RK3576_GPIO_INT_BOTH_EDGE);

  g_tp_isr = isr;
  g_tp_isr_arg = arg;

  ret = rk3576_gpio_irq_attach(g_tp_int, kickpi_k7_touch_int_handler);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TP irq_attach failed: %d\n", ret);
      g_tp_isr = NULL;
      g_tp_isr_arg = NULL;
    }

  return ret;
}

/****************************************************************************
 * Name: kickpi_k7_touch_irq_enable
 ****************************************************************************/

static void kickpi_k7_touch_irq_enable(FAR const struct gt911_board_s *state,
                                       bool enable)
{
  UNUSED(state);

  if (enable)
    {
      rk3576_gpio_irq_enable(g_tp_int);
    }
  else
    {
      rk3576_gpio_irq_disable(g_tp_int);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_touch_initialize
 *
 * Description:
 *   Mux the I2C0 M1 pins (SCL GPIO0_C1 / SDA GPIO0_C2, AF9), bring up the I2C0
 *   controller, claim the reset and interrupt pins and register the on-board
 *   GT911 as /dev/input0.
 *
 *   The panel supply is shared with the DSI panel, so this must be called
 *   after the display bring-up has powered the module; see
 *   kickpi_k7_mipi_dsi_initialize().
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_touch_initialize(void)
{
  static const struct gt911_board_s g_touch_board = {
    .irq_attach = kickpi_k7_touch_irq_attach,
    .irq_enable = kickpi_k7_touch_irq_enable,
    .set_power = kickpi_k7_touch_set_power,

  /* The candidate tables are selected under "On-board GT911 touch module
   * configuration table"; with CONFIG_KICKPI_K7_TOUCH_CFG_NONE there are none
   * and the driver only reports what the module says about itself.
   */

#if defined(CONFIG_KICKPI_K7_TOUCH_CFG_VENDOR)
    .configs = g_kickpi_k7_tp_configs,
    .nconfigs =
        sizeof(g_kickpi_k7_tp_configs) / sizeof(g_kickpi_k7_tp_configs[0]),
#else
    .configs = NULL,
    .nconfigs = 0,
#endif
  };

  FAR struct i2c_master_s *i2c;
  int ret;

  /* Mux the I2C0 pins.  The handles are kept (not put) so the pins stay owned
   * by the I2C0 controller; AF9 is I2C0 in its M1 muxing on both pins.
   */

  ret = rk3576_gpio_get(KICKPI_K7_TP_SCL_PIN, &g_tp_scl);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TP SCL rk3576_gpio_get failed: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_af(g_tp_scl, KICKPI_K7_TP_SCL_AF);

  ret = rk3576_gpio_get(KICKPI_K7_TP_SDA_PIN, &g_tp_sda);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TP SDA rk3576_gpio_get failed: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_af(g_tp_sda, KICKPI_K7_TP_SDA_AF);

  /* Claim the reset and interrupt pins.  INT is claimed as an input with no
   * pull, which is also the idle state the controller expects to see; the
   * reset sequence in kickpi_k7_touch_set_power() drives INT as an output for
   * the address strap and puts it back.
   */

  ret = rk3576_gpio_get(KICKPI_K7_TP_RST_PIN, &g_tp_rst);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TP RST rk3576_gpio_get failed: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_mode(g_tp_rst, RK3576_GPIO_OUTPUT);

  ret = rk3576_gpio_get(KICKPI_K7_TP_INT_PIN, &g_tp_int);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TP INT rk3576_gpio_get failed: %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_mode(g_tp_int, RK3576_GPIO_INPUT);
  rk3576_gpio_set_pull(g_tp_int, RK3576_GPIO_FLOAT);

  /* I2C0.  Its clock is ungated there; the pins above are already muxed. */

  i2c = rk3576_i2c_initialize(KICKPI_K7_TP_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: I2C%d init failed\n", KICKPI_K7_TP_I2C_BUS);
      return -ENODEV;
    }

  /* Register the touch controller.  The driver resets the panel, probes it and
   * publishes /dev/input0; see drivers/include/gt911.h.  The orientation is
   * what maps its coordinates onto the display, and it is built from the same
   * board option the panel's MADCTL is built from.
   */

  ret = gt911_register(KICKPI_K7_TP_DEVPATH, i2c, KICKPI_K7_TP_ADDR,
                       &g_touch_board, &g_kickpi_k7_tp_orientation);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: gt911_register failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "INFO: touch ready: %s (GT911 on I2C%d M1)\n",
         KICKPI_K7_TP_DEVPATH, KICKPI_K7_TP_I2C_BUS);
  return OK;
}

#endif /* CONFIG_KICKPI_K7_TOUCH */
