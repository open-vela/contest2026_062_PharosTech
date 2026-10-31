/****************************************************************************
 * drivers/include/gt911.h
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
 * Goodix GT911 capacitive touch controller driver, public interface.
 *
 * The controller sits on an I2C bus and raises an INT line for every scan
 * period in which the coordinate buffer changed.  Unlike the upstream
 * drivers/input/gt9xx.c driver (which registers its own file_operations and
 * reports a single point of fixed length), this driver is a lower half of the
 * generic NuttX touchscreen framework (CONFIG_INPUT_TOUCHSCREEN) and reports
 * all contacts in the frame at the sample length the framework and LVGL
 * expect.  See drivers/drivers/gt911/gt911.c for the contract details.
 ****************************************************************************/

#ifndef __BOARDS_RK3576_DRIVERS_INCLUDE_GT911_H
#define __BOARDS_RK3576_DRIVERS_INCLUDE_GT911_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The GT911 slave address is not strapped by a pin: it is latched from the
 * level of the INT line while RST is released (datasheet 4.2).  Both values
 * below are the 7-bit addresses the i2c_master_s API expects; the datasheet
 * quotes the same addresses shifted left by one (0x28/0x29 and 0xba/0xbb)
 * because it counts the read/write bit as part of the address.
 *
 *   INT driven high across reset release -> 0x14
 *   INT driven low  across reset release -> 0x5d
 */

#define GT911_I2C_ADDR_INT_HIGH 0x14
#define GT911_I2C_ADDR_INT_LOW  0x5d

/* Length of the configuration payload carried in struct gt911_board_s.config:
 * the configuration area is registers 0x8047..0x80fe.  The two registers that
 * follow it in a vendor table -- 0x80ff (byte sum complement) and 0x8100 (the
 * apply flag) -- are derived by the driver, not supplied by the board.
 */

#define GT911_CONFIG_LEN 184u

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One candidate module configuration table, as supplied by the module vendor.
 * The name is only used in the bring-up log, so a board can say which table
 * was being tried when something did or did not work.
 */

struct gt911_config_s
{
  FAR const char *name;
  FAR const uint8_t *data;
  size_t len;
};

/* Coordinate orientation.
 *
 * The controller reports coordinates in the frame its own configuration table
 * describes, which is a property of the touch sensor.  How the module is
 * mounted relative to the framebuffer is a property of the enclosure, and it
 * is not discoverable at run time -- the controller has no "which way up am
 * I" register -- so the board supplies it here.  It is a parameter rather
 * than a driver option because the same physical fact has to be applied to
 * the display as well; a board that derives both from one setting cannot get
 * them out of step (see the kickpi-k7 board Kconfig for an example).
 *
 * The transform is a plain composition, applied in this order:
 *
 *   x' = mirrorx ? xres - 1 - x : x
 *   y' = mirrory ? yres - 1 - y : y
 *   (x, y) = (y', x')                            if swapxy
 *
 * so the mirrors are expressed in the controller's own frame and the swap
 * comes last.  That ordering is what makes each field mean one thing on its
 * own, and the eight combinations cover the four rotations and their mirrors:
 *
 *   both mirrors on     - the module is mounted 180 degrees round
 *   one mirror on       - a single axis of the picture is reversed
 *   swapxy on           - the controller's axes are 90 degrees from the
 *                         display's
 *
 * xres/yres are the ranges the mirrors are computed against, i.e. the largest
 * coordinate the controller can report, which is the X/Y Output Max its
 * configuration programs.  The driver prefers the range the controller reports
 * about itself at probe time (registers 0x8146/0x8148, logged as "GT911 output
 * range ..."), because that is the frame the coordinates actually arrive in
 * and it follows a module whose table is not the one the board expected; the
 * values here are the fallback for a controller that came up without a range,
 * and a disagreement between the two is logged rather than ignored.
 *
 * A board whose sensor and framebuffer already agree can pass NULL to
 * gt911_register() instead of filling this in.
 */

struct gt911_orientation_s
{
  uint16_t xres; /* Mirror range on X; 0 = use the controller's own */
  uint16_t yres; /* Mirror range on Y; 0 = use the controller's own */
  bool swapxy;   /* Exchange the axes after mirroring */
  bool mirrorx;  /* Reverse the X axis */
  bool mirrory;  /* Reverse the Y axis */
};

/* Board-specific operations.
 *
 * All three callbacks are optional: a board that has already powered and
 * reset the controller (for example one on a supply it does not own) may leave
 * set_power NULL, and a board that polls instead of wiring INT may leave the
 * two interrupt callbacks NULL.  The driver only needs either the interrupt
 * or its own idle poll timer to make progress; both together give the lowest
 * latency.
 */

struct gt911_board_s
{
  /* Attach the interrupt handler for the controller INT line and enable the
   * underlying interrupt source.  Called before irq_enable() so the handler
   * is in place before the first edge can arrive.
   *
   * The isr passed here has the standard NuttX xcpt_t signature; a board
   * whose GPIO layer uses a different callback type must bridge to it, as
   * kickpi_k7_touch.c does for the RK3576 GPIO driver.
   */

  CODE int (*irq_attach)(FAR const struct gt911_board_s *state, xcpt_t isr,
                         FAR void *arg);

  /* Unmask (enable) or mask (disable) the INT line.  Called once with
   * enable=true after the driver is registered and its work item is running,
   * so no edge can be delivered before there is somewhere to put it.
   */

  CODE void (*irq_enable)(FAR const struct gt911_board_s *state, bool enable);

  /* Power-cycle and reset the controller, driving INT to the level that
   * selects the address passed to gt911_register().  Must leave RST released
   * and INT floating (the GT911 needs INT as a high-impedance input).  The
   * driver calls this once, before its first I2C transfer.
   *
   * With on=false the board should hold the controller in reset; a board that
   * shares the panel supply with the display may implement that as driving
   * RST low and nothing else.
   */

  CODE int (*set_power)(FAR const struct gt911_board_s *state, bool on);

  /* Module configuration tables the board wants tried, in order.
   *
   * Each entry is the payload of the controller's configuration area, i.e.
   * GT911_CONFIG_LEN bytes for registers 0x8047..0x80fe in the order the
   * vendor programmed them; the driver derives the checksum (0x80ff) and the
   * apply flag (0x8100) itself.
   *
   * A table is needed because a module whose configuration area came back
   * blank answers every read and yet senses nothing at all: with no X/Y output
   * range and no sensing channel map there is no scan, so no coordinate frame
   * and no interrupt is ever produced, and the driver looks perfectly healthy
   * while the touchscreen is silently deaf.
   *
   * The configuration encodes properties of the touch sensor itself -- channel
   * map, frequency, gains -- so which table belongs to a given module is not
   * something the host can derive; it comes from the module vendor.  Where the
   * vendor supplies more than one (one per panel variant) the entries are
   * tried in order, because a table that does not match the sensor can leave
   * the controller answering on I2C without ever starting its sensing engine
   * -- which is exactly the state that ordering is there to resolve.
   *
   * A module that already reports a valid configuration of its own is left
   * alone unless CONFIG_INPUT_GT911_CONFIG_FORCE asks for a rewrite.
   */

  FAR const struct gt911_config_s *configs;
  size_t nconfigs;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_register
 *
 * Description:
 *   Register the GT911 as a touchscreen input device (by convention
 *   /dev/input0) and start sampling it.
 *
 *   The panel is powered, reset and probed here, so the caller must have
 *   brought up the I2C bus and its pins first.  The function returns only
 *   after the device node exists and the first sample has been scheduled; the
 *   INT line is unmasked last.
 *
 * Input Parameters:
 *   devpath      - Device path (e.g. "/dev/input0").
 *   i2c          - I2C master the controller is attached to.
 *   i2c_devaddr  - 7-bit address the controller latched at reset; use
 *                  GT911_I2C_ADDR_INT_HIGH/LOW, or 0 to accept either one.
 *   board        - Board-specific callbacks (may not be NULL).
 *   oriented     - How to map the controller's coordinates onto the display,
 *                  or NULL when the two frames already agree.  The structure
 *                  is copied, so it has no lifetime requirement.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int gt911_register(FAR const char *devpath, FAR struct i2c_master_s *i2c,
                   uint8_t i2c_devaddr, FAR const struct gt911_board_s *board,
                   FAR const struct gt911_orientation_s *oriented);

#endif /* __BOARDS_RK3576_DRIVERS_INCLUDE_GT911_H */
