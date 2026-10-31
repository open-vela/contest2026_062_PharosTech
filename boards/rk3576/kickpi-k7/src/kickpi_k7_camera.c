/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_camera.c
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
 * On-board MIPI CSI camera bring-up for the kickpi-k7 (RK3576).
 *
 * The board's CSI0 connector is a 22-pin, Raspberry Pi compatible FFC
 * socket, and it is the only camera connector fed by the DCPHY's RX
 * (slave) lanes.  The full chain is:
 *
 *   connector CSI0 -> DCPHY RX (SD0/SD1) -> CSIHOST0 -> VICAP MIPI0 -> DDR
 *
 * Board-level responsibilities, and nothing else:
 *
 *   - the module's one control GPIO (CSI0_PDN_H on GPIO0_D2).  The socket
 *     has no power-enable and no reset pin, and the module carries its own
 *     25 MHz oscillator, so there is no master clock and no reset line to
 *     drive either;
 *   - the SCCB (I2C) pin multiplexing and controller start-up;
 *   - handing the resulting imgdata and imgsensor handles to the capture
 *     framework.
 *
 * The PHY, the CSI HOST and VICAP are configured by their own drivers; the
 * board never touches their registers.
 *
 * What the board does not do is choose a mode.  It brings the chain up on
 * one -- the capture engine has no scaler and a device node needs a buffer
 * layout, so there is no way to bring it up without a geometry -- but that is
 * a placeholder rather than a decision, and every mode a stream actually runs
 * at is named by the application, before it opens the camera.  Closing the
 * device is the application's half of the same arrangement, and it needs no
 * help from the board: the framework puts the sensor into software standby
 * when the last user lets go, and the chain is left as it was, so the next
 * stream costs only the sensor its own start-up.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/v4l2_cap.h>

#include "kickpi_k7.h"
#include "ov5647.h"
#include "rk3576_csi_host.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk3576_vicap.h"

#ifdef CONFIG_KICKPI_K7_CAMERA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The camera module's enable line: CSI0_PDN_H on GPIO0_D2.  Driving it high
 * powers the module; pulling it low makes the sensor disappear from the
 * SCCB bus entirely.
 */

#define KICKPI_K7_CAM_PDN_PIN (GPIO_PORT0 | GPIO_PIN_D2)

/* SCCB control bus: I2C4 in its M3 pin multiplexing (SCL on GPIO3_C0, SDA
 * on GPIO3_B7, both at alternate function 11).  Neither pad is used by
 * anything else on this board.
 */

#define KICKPI_K7_CAM_I2C_BUS 4
#define KICKPI_K7_CAM_I2C_AF  11
#define KICKPI_K7_CAM_SCL_PIN (GPIO_PORT3 | GPIO_PIN_C0)
#define KICKPI_K7_CAM_SDA_PIN (GPIO_PORT3 | GPIO_PIN_B7)

/* Number of MIPI data lanes.  Every mode the sensor offers runs on two, so
 * this is a property of the wiring rather than of the mode.
 */

#define KICKPI_K7_CAM_LANES 2

/* CSI-2 data type for RAW10. */

#define KICKPI_K7_CAM_DT_RAW10 0x2b

#define KICKPI_K7_CAM_DEVPATH  "/dev/video0"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct gpio_dev_s *g_kickpi_k7_cam_pdn;

/* The capture mode the receive chain is currently configured for, as an index
 * into the sensor driver's mode table.
 *
 * Everything the two SoC-side drivers have to be told -- the link rate the
 * D-PHY runs at, the geometry and Bayer order the capture engine is
 * programmed with, and the size of its buffers -- follows from the entry this
 * names, so this is the single thing that says which set of them is current.
 *
 * The sensor keeps its own view of the mode, which is whatever the
 * application asked for when it started the stream.  The two are moved
 * together by the application telling the board before it opens the device,
 * which is what kickpi_k7_camera_set_mode() is for; there is no shared state
 * between them to get out of step, only this one direction of travel.
 *
 * The value it starts at is the placeholder the sensor driver publishes for
 * this, and not a choice -- see the note on this at the top of the file.  A
 * device node has to exist before an application can ask for anything, and
 * the capture engine it exposes has no scaler, so the chain cannot be brought
 * up without a geometry; this is the one it is brought up with.  The first
 * application to name a mode replaces it, which is what makes it a
 * placeholder rather than a decision.
 */

static unsigned int g_kickpi_k7_cam_mode = OV5647_MODE_DEFAULT;

/* The sensor names its Bayer orders, and the capture engine names the same
 * four in its own enum, because they are separate interfaces.
 *
 * What the engine has to be told is the order of the buffer it is about to
 * demosaic, which is the sensor's order after the mode's readout has had its
 * way with it -- so the mode's answer is the right one to hand over directly.
 * The check makes the shorthand safe rather than lucky: if either enum is
 * ever renumbered, this stops the build instead of leaving the picture to
 * quietly lose its colour, which is what a wrong order looks like.
 */

static_assert((unsigned int)OV5647_BAYER_BGGR ==
                      (unsigned int)RK3576_VICAP_BAYER_BGGR &&
                  (unsigned int)OV5647_BAYER_GBRG ==
                      (unsigned int)RK3576_VICAP_BAYER_GBRG &&
                  (unsigned int)OV5647_BAYER_GRBG ==
                      (unsigned int)RK3576_VICAP_BAYER_GRBG &&
                  (unsigned int)OV5647_BAYER_RGGB ==
                      (unsigned int)RK3576_VICAP_BAYER_RGGB,
              "the sensor's Bayer order and the capture engine's must agree");

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_camera_csi_config / kickpi_k7_camera_vicap_config
 *
 * Description:
 *   Describe one capture mode to the two drivers that have to know it.
 *
 *   Every figure comes from the sensor's mode table, which is the only place
 *   that knows what the sensor has been programmed to output: the geometry it
 *   delivers, the rate its link runs at, and the Bayer order its readout
 *   leaves in the buffer.  Stating any of them again here would be a second
 *   answer to a question that already has one, and the two would drift.
 *
 *   A D-PHY link is double data rate, so the per-lane rate the PHY is
 *   configured for is twice the link frequency.  Getting it wrong does not
 *   stop the link from coming up; it leaves the receiver's timing parameters
 *   in the wrong rate band, which shows up as intermittent packet errors
 *   rather than as anything that looks like a configuration mistake.
 *
 ****************************************************************************/

static void kickpi_k7_camera_csi_config(unsigned int index,
                                        FAR struct rk3576_csi_config *config)
{
  FAR const struct ov5647_mode_s *mode = ov5647_mode(index);

  const struct rk3576_csi_config cfg = {
    .host = 0,
    .lanes = KICKPI_K7_CAM_LANES,
    .hs_rate = 2u * mode->link_freq,
  };

  *config = cfg;
}

/* VICAP is told the sensor's real geometry and Bayer order.  The samples are
 * taken uncompacted and high-aligned, which puts a 10-bit value in bits
 * [15:6] of each 16-bit word; the CPU demosaicer shifts it down from there.
 *
 * The order is GBRG, for two reasons that have to hold together.
 *
 * First the sensor.  Its native tile is BGGR, and these modes mirror the
 * readout horizontally -- their register tables set r_mirror_snr (0x3821
 * bit 1) and clear r_vflip_snr (0x3820 bit 1).  A horizontal mirror swaps the
 * two columns of every tile, and BGGR mirrored is GBRG.  The upstream Linux
 * driver reports the same thing for the same modes: its HFLIP control is
 * inverted precisely because the sensor has this flip built in, so its
 * default (hflip = 0, vflip = 0) resolves to MEDIA_BUS_FMT_SGBRG10.  Both
 * register tables carry the identical bits, so the answer is the same one for
 * every mode -- which is a thing to have checked rather than assumed, and the
 * sensor's mode table is where it is written down.
 *
 * Then the buffer.  What VICAP has to be told is the order of the *buffer's*
 * first row, and the buffer begins where the sensor's frame begins only
 * because sw_dma_adapt_en is set -- with it clear the DMA counts its own
 * lines instead and the buffer starts at a fixed offset from the frame, and
 * any odd offset would reverse the tile.  With the DMA following the
 * interface the two origins coincide, so the buffer's tile is the sensor's
 * tile and GBRG is the order to give.
 *
 * Getting this wrong is worth spelling out, because it does not look like a
 * broken setting.  With the tile reversed, the sites the demosaicer calls
 * red and blue are both really green, so red and blue are reconstructed
 * from the same samples and come out as the same signal: the picture loses
 * its colour and turns into a slightly tinted grey, while the white balance
 * -- measuring those same two channels -- reports near-unity gains and
 * looks like it has nothing left to do.  Nothing errors out.
 *
 * The driver prints the raw plane's four parity classes for exactly this
 * check.  Green is the pair of classes that sit above the average of the
 * other two, because green is a Bayer sensor's most sensitive channel; if
 * that pair is the one the configured order does not call green at all,
 * this setting is the one to change.
 *
 * It is worth checking again after every change of mode rather than once per
 * board.  The order depends on where the frame starts, so a mode whose
 * readout window begins elsewhere can reverse the tile even when its mirror
 * bits read the same.
 */

static void
kickpi_k7_camera_vicap_config(unsigned int index,
                              FAR struct rk3576_vicap_config *config)
{
  FAR const struct ov5647_mode_s *mode = ov5647_mode(index);

  const struct rk3576_vicap_config cfg = {
    .input = RK3576_VICAP_INPUT_MIPI0,
    .id = 0,
    .bayer = mode->bayer,
    .raw_bits = 10,
    .width = mode->width,
    .height = mode->height,
    .vc = 0,
    .dt = KICKPI_K7_CAM_DT_RAW10,
    .uncompact = true,
    .align_high = true,
  };

  *config = cfg;
}

/****************************************************************************
 * Name: kickpi_k7_camera_now_us
 *
 * Description:
 *   The system tick as microseconds, for timing the steps of a mode change.
 *   Coarse -- a tick is ten milliseconds here -- which is the point: what is
 *   being measured is a path that either takes about as long as a frame or
 *   takes seconds, and a ten-millisecond ruler says which of the two it is.
 *
 ****************************************************************************/

static uint64_t kickpi_k7_camera_now_us(void)
{
  return (uint64_t)clock_systime_ticks() * (1000000ULL / TICK_PER_SEC);
}

/****************************************************************************
 * Name: kickpi_k7_camera_module_enable
 *
 * Description:
 *   Claim the module's enable line and drive it high.
 *
 *   The pin is claimed once and kept: it stays an output for as long as the
 *   board runs, so there is nothing to hand back, and a second claim would
 *   be refused by the GPIO driver anyway.
 *
 ****************************************************************************/

static int kickpi_k7_camera_module_enable(void)
{
  int ret;

  if (g_kickpi_k7_cam_pdn == NULL)
    {
      ret = rk3576_gpio_get(KICKPI_K7_CAM_PDN_PIN, &g_kickpi_k7_cam_pdn);
      if (ret < 0)
        {
          _err("ERROR: camera failed to claim GPIO0_D2: %d\n", ret);
          return ret;
        }
    }

  rk3576_gpio_set_mode(g_kickpi_k7_cam_pdn, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_kickpi_k7_cam_pdn, true);

  /* The module's own oscillator and regulator need a moment before the
   * sensor answers on SCCB.
   */

  up_mdelay(20);

  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_camera_i2c_start
 *
 * Description:
 *   Mux the SCCB pins and start the I2C controller.
 *
 ****************************************************************************/

static FAR struct i2c_master_s *kickpi_k7_camera_i2c_start(void)
{
  FAR struct gpio_dev_s *scl = NULL;
  FAR struct gpio_dev_s *sda = NULL;
  FAR struct i2c_master_s *i2c;
  int ret;

  /* The handles are deliberately not released: once muxed, the pads belong
   * to the I2C controller for as long as it runs.
   */

  ret = rk3576_gpio_get(KICKPI_K7_CAM_SCL_PIN, &scl);
  if (ret < 0)
    {
      _err("ERROR: camera failed to claim SCL (GPIO3_C0): %d\n", ret);
      return NULL;
    }

  rk3576_gpio_set_af(scl, KICKPI_K7_CAM_I2C_AF);

  ret = rk3576_gpio_get(KICKPI_K7_CAM_SDA_PIN, &sda);
  if (ret < 0)
    {
      _err("ERROR: camera failed to claim SDA (GPIO3_B7): %d\n", ret);
      return NULL;
    }

  rk3576_gpio_set_af(sda, KICKPI_K7_CAM_I2C_AF);

  i2c = rk3576_i2c_initialize(KICKPI_K7_CAM_I2C_BUS);
  if (i2c == NULL)
    {
      _err("ERROR: camera failed to start I2C%u\n", KICKPI_K7_CAM_I2C_BUS);
      return NULL;
    }

  return i2c;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_camera_initialize
 ****************************************************************************/

int kickpi_k7_camera_initialize(void)
{
  FAR struct i2c_master_s *i2c;
  FAR struct imgsensor_s *sensors[1];
  FAR struct imgdata_s *data = NULL;
  struct rk3576_csi_config csi;
  struct rk3576_vicap_config vicap;
  int ret;

  /* 1. Power the module, then give it time to settle before it is
   *    addressed over SCCB.
   */

  ret = kickpi_k7_camera_module_enable();
  if (ret < 0)
    {
      return ret;
    }

  /* 2. Sensor control bus. */

  i2c = kickpi_k7_camera_i2c_start();
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  ret = ov5647_initialize(i2c);
  if (ret < 0)
    {
      _err("ERROR: camera failed to bind the OV5647 driver: %d\n", ret);
      return ret;
    }

  /* 3. The receive chain, configured for the placeholder mode the board
   *    starts in -- see the note on g_kickpi_k7_cam_mode for why it has to
   *    name some mode rather than none.  The PHY comes up first inside the
   *    CSI HOST driver, because the host's lane count may only be changed
   *    while the D-PHY lanes are in the stop state -- which is exactly the
   *    state they are in while the sensor is not yet streaming.
   *
   *    The DCPHY is shared with the DSI output.  Neither side resets or
   *    reconfigures the other's, so a running display is not disturbed by
   *    bringing the camera up (and vice versa).
   */

  g_kickpi_k7_cam_mode = OV5647_MODE_DEFAULT;
  kickpi_k7_camera_csi_config(g_kickpi_k7_cam_mode, &csi);

  ret = rk3576_csi_host_initialize(&csi);
  if (ret < 0)
    {
      _err("ERROR: camera failed to bring up CSI HOST0: %d\n", ret);
      return ret;
    }

  /* 4. The capture engine, and the device file the application opens.
   *
   *    The interface that comes back is a member of the capture driver's own
   *    instance, so its address is the same one every time this pair is taken
   *    down and brought back up.  That is what lets a later change of mode
   *    re-run this step without re-registering the device file: the capture
   *    framework was handed this address once and it stays the right one.
   */

  kickpi_k7_camera_vicap_config(g_kickpi_k7_cam_mode, &vicap);

  ret = rk3576_vicap_initialize(&vicap, &data);
  if (ret < 0)
    {
      _err("ERROR: camera failed to bring up VICAP: %d\n", ret);
      rk3576_csi_host_uninitialize();
      return ret;
    }

  sensors[0] = ov5647_sensor();

  ret = capture_register(KICKPI_K7_CAM_DEVPATH, data, sensors, 1);
  if (ret < 0)
    {
      _err("ERROR: camera failed to register %s: %d\n", KICKPI_K7_CAM_DEVPATH,
           ret);
      rk3576_vicap_uninitialize();
      rk3576_csi_host_uninitialize();
      return ret;
    }

  syslog(LOG_INFO, "kickpi-k7: OV5647 on CSI0 ready as %s (%ux%u mode %u)\n",
         KICKPI_K7_CAM_DEVPATH, ov5647_mode(g_kickpi_k7_cam_mode)->width,
         ov5647_mode(g_kickpi_k7_cam_mode)->height, g_kickpi_k7_cam_mode);

  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_camera_set_mode
 *
 * Description:
 *   Point the receive chain at another of the sensor's capture modes.
 *
 *   How much has to change depends on how far apart the two modes are, and
 *   this decides that rather than making the caller know:
 *
 *     - Modes that share a geometry and a link rate differ only in frame
 *       rate, and a frame rate is a register the sensor driver writes when the
 *       stream starts.  The link rate, the capture geometry and the buffer
 *       sizes are all the same, so nothing outside the sensor moves and this
 *       is done.
 *
 *     - Modes with different geometries also differ in link rate, in the
 *       geometry the capture engine is programmed with, and in the size of
 *       the buffers it holds.  Both drivers are asked to move, and neither is
 *       taken down: the lane rate is a PHY power cycle and the geometry is a
 *       buffer change, so the block's clocks, resets, power domain,
 *       interrupt and 3A device all stay exactly as they were.
 *
 *   The lane rate goes first, because it is the D-PHY that has to be
 *   reprogrammed before the sensor starts driving the lanes again.
 *
 *   The caller has to have closed the capture device first.  Not because the
 *   drivers would be left inconsistent -- the 3A device now outlives a mode
 *   change -- but because the sensor has to be stopped for the D-PHY to
 *   accept new timing parameters at all.
 *
 *   It is used at two moments, and cannot tell them apart or need to.  One is
 *   the application's first request, before it opens the camera for the first
 *   time: the chain is on the placeholder mode then, and the stream is about
 *   to be the mode the application asked for rather than the one the board
 *   was brought up with.  The other is a change while the program runs, when
 *   the page has asked for a different mode and the application closes the
 *   camera, asks for the new one, and opens it again.  What the two share is
 *   the only precondition there is: that no capture device is open.
 *
 * Input Parameters:
 *   index - The mode to switch to, one of the sensor's OV5647_MODE_*.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.  On failure the receive
 *   chain is left as it was found, so the caller can carry on with the mode
 *   that is still configured.
 *
 ****************************************************************************/

int kickpi_k7_camera_set_mode(unsigned int index)
{
  FAR const struct ov5647_mode_s *mode = ov5647_mode(index);
  FAR const struct ov5647_mode_s *current = ov5647_mode(g_kickpi_k7_cam_mode);
  struct rk3576_csi_config csi;
  struct rk3576_vicap_config vicap;
  uint64_t started;
  int ret;

  if (mode == NULL)
    {
      _err("ERROR: camera mode %u does not exist\n", index);
      return -EINVAL;
    }

  if (index == g_kickpi_k7_cam_mode)
    {
      return OK;
    }

  /* A change of frame rate alone never leaves the sensor.  It is worth
   * recognising rather than treating as a small mode change: it is the one
   * transition that cannot fail, cannot lose the stream for longer than the
   * sensor takes to reload its frame length, and needs no buffers moved.
   */

  if (current != NULL && mode->width == current->width &&
      mode->height == current->height && mode->link_freq == current->link_freq)
    {
      g_kickpi_k7_cam_mode = index;

      _info("kickpi-k7: camera mode %u (%ux%u @ %u fps), link unchanged\n",
            index, (unsigned int)mode->width, (unsigned int)mode->height,
            (unsigned int)mode->fps);
      return OK;
    }

  /* A different geometry, and with it a different link rate and a different
   * set of frame buffers.
   *
   * Both are changed in place rather than by taking the receive chain down
   * and building it again.  The chain's clocks, resets, power domain,
   * interrupt and 3A control device do not depend on the geometry, so
   * releasing and re-acquiring them buys nothing and costs a window in which
   * the camera does not exist; and each of those steps is code that only ever
   * runs on a mode change, which is the worst place for it to be wrong.
   *
   * The lane rate goes first.  It is the D-PHY that has to be reprogrammed
   * before the sensor starts driving the lanes, and the sensor is not
   * streaming here -- the capture device is closed, which is what "not
   * streaming" means, and it is also what makes the lanes sit in the stop
   * state the PHY wants for new timing parameters.
   */

  kickpi_k7_camera_csi_config(index, &csi);
  kickpi_k7_camera_vicap_config(index, &vicap);

  started = kickpi_k7_camera_now_us();

  ret = rk3576_csi_host_set_lane_rate(csi.hs_rate);
  if (ret < 0)
    {
      _err("ERROR: camera failed to move CSI HOST0 to %u Hz/lane for mode"
           " %u: %d\n",
           (unsigned int)csi.hs_rate, index, ret);
      return ret;
    }

  _info("kickpi-k7: lane rate to %u Hz took %" PRIu64 " ms\n",
        (unsigned int)csi.hs_rate,
        (kickpi_k7_camera_now_us() - started) / 1000u);

  ret = rk3576_vicap_reconfigure(&vicap);
  if (ret < 0)
    {
      _err("ERROR: camera failed to reconfigure VICAP for mode %u: %d\n",
           index, ret);

      /* The capture engine refused, so the mode is not changing and the link
       * rate must not be left at the rate that belongs to a mode nothing else
       * was moved to.  Put it back, and report the failure the caller cares
       * about rather than whatever that put-back returns.
       */

      if (current != NULL)
        {
          (void)rk3576_csi_host_set_lane_rate(2u * current->link_freq);
        }

      return ret;
    }

  /* Only now is the change complete, so only now is it recorded.  The
   * elapsed time is reported for the same reason the two steps above are
   * timed: a change of mode is the one path in this driver that only runs
   * when someone asks for it, so it is the one path with no frame rate to
   * notice a stall against, and a switch that takes seconds looks from the
   * application exactly like a board that has gone away.
   *
   * Recording it is what makes the next switch decide correctly.  Leaving it
   * out does not fail here -- it fails on the switch after, when the "only
   * the frame rate changed" test compares the new mode against a mode the
   * board stopped being on, decides nothing needs reconfiguring, and leaves
   * the capture engine on a geometry the sensor is no longer producing.
   */

  g_kickpi_k7_cam_mode = index;

  _info("kickpi-k7: camera mode %u (%ux%u @ %u fps), link %u Hz,"
        " reconfigure %" PRIu64 " ms\n",
        index, (unsigned int)mode->width, (unsigned int)mode->height,
        (unsigned int)mode->fps, (unsigned int)csi.hs_rate,
        (kickpi_k7_camera_now_us() - started) / 1000u);

  return OK;
}

#endif /* CONFIG_KICKPI_K7_CAMERA */
