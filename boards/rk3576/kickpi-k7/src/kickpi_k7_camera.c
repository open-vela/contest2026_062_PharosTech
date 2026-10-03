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
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <syslog.h>

#include <nuttx/arch.h>
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

/* OV5647 capture mode, matched to the sensor driver's CONFIG_OV5647_MODE_*
 * selection.  The geometry and the link frequency must agree with what the
 * sensor is actually programmed to output, since the SoC-side receiver is
 * configured once from these figures.
 *
 * The sensor's link frequency follows the mode:
 *   - 640x480:  145.833 MHz (pixel clock 58.333 MHz)
 *   - 1296x960: 218.75 MHz  (pixel clock 87.5 MHz)
 *
 * A D-PHY link is double data rate, so the per-lane data rate the PHY has
 * to be configured for is twice the link frequency.  Getting this wrong does
 * not stop the link from coming up; it leaves the receiver's timing
 * parameters in the wrong rate band, which shows up as intermittent packet
 * errors.
 */

#if defined(CONFIG_OV5647_MODE_1296x960)
#define KICKPI_K7_CAM_WIDTH     1296
#define KICKPI_K7_CAM_HEIGHT    960
#define KICKPI_K7_CAM_LINK_FREQ 218750000u
#else
#define KICKPI_K7_CAM_WIDTH     640
#define KICKPI_K7_CAM_HEIGHT    480
#define KICKPI_K7_CAM_LINK_FREQ 145833300u
#endif

#define KICKPI_K7_CAM_LANES   2
#define KICKPI_K7_CAM_HS_RATE (2u * KICKPI_K7_CAM_LINK_FREQ)

/* CSI-2 data type for RAW10. */

#define KICKPI_K7_CAM_DT_RAW10 0x2b

#define KICKPI_K7_CAM_DEVPATH  "/dev/video0"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct gpio_dev_s *g_kickpi_k7_cam_pdn;

static const struct rk3576_csi_config g_kickpi_k7_cam_csi = {
  .host = 0,
  .lanes = KICKPI_K7_CAM_LANES,
  .hs_rate = KICKPI_K7_CAM_HS_RATE,
};

/* VICAP is told the sensor's real geometry and Bayer order.  The samples are
 * taken uncompacted and high-aligned, which puts a 10-bit value in bits
 * [15:6] of each 16-bit word; the CPU demosaicer shifts it down from there.
 *
 * The order is GBRG, for two reasons that have to hold together.
 *
 * First the sensor.  Its native tile is BGGR, and this mode mirrors the
 * readout horizontally -- the mode's register table sets r_mirror_snr
 * (0x3821 bit 1) and clears r_vflip_snr (0x3820 bit 1).  A horizontal
 * mirror swaps the two columns of every tile, and BGGR mirrored is GBRG.
 * The upstream Linux driver reports the same thing for this mode: its HFLIP
 * control is inverted precisely because the sensor has this flip built in,
 * so its default (hflip = 0, vflip = 0) resolves to MEDIA_BUS_FMT_SGBRG10.
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
 */

static const struct rk3576_vicap_config g_kickpi_k7_cam_vicap = {
  .input = RK3576_VICAP_INPUT_MIPI0,
  .id = 0,
  .bayer = RK3576_VICAP_BAYER_GBRG,
  .raw_bits = 10,
  .width = KICKPI_K7_CAM_WIDTH,
  .height = KICKPI_K7_CAM_HEIGHT,
  .vc = 0,
  .dt = KICKPI_K7_CAM_DT_RAW10,
  .uncompact = true,
  .align_high = true,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

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

  /* 3. The receive chain.  The PHY comes up first inside the CSI HOST
   *    driver, because the host's lane count may only be changed while the
   *    D-PHY lanes are in the stop state -- which is exactly the state they
   *    are in while the sensor is not yet streaming.
   *
   *    The DCPHY is shared with the DSI output.  Neither side resets or
   *    reconfigures the other's, so a running display is not disturbed by
   *    bringing the camera up (and vice versa).
   */

  ret = rk3576_csi_host_initialize(&g_kickpi_k7_cam_csi);
  if (ret < 0)
    {
      _err("ERROR: camera failed to bring up CSI HOST0: %d\n", ret);
      return ret;
    }

  /* 4. The capture engine, and the device file the application opens. */

  ret = rk3576_vicap_initialize(&g_kickpi_k7_cam_vicap, &data);
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

  syslog(LOG_INFO, "kickpi-k7: OV5647 on CSI0 ready as %s (%ux%u)\n",
         KICKPI_K7_CAM_DEVPATH, KICKPI_K7_CAM_WIDTH, KICKPI_K7_CAM_HEIGHT);

  return OK;
}

#endif /* CONFIG_KICKPI_K7_CAMERA */
