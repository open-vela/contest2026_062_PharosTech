/****************************************************************************
 * drivers/drivers/ov5647/ov5647.h
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
 * OmniVision OV5647 MIPI CSI-2 camera sensor -- public interface.
 ****************************************************************************/

#ifndef __DRIVERS_PLATFORM_DRIVERS_OV5647_OV5647_H
#define __DRIVERS_PLATFORM_DRIVERS_OV5647_OV5647_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/imgsensor.h>

#ifdef CONFIG_VIDEO_OV5647

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The four Bayer orders, named by the colour of the first two pixels of the
 * first row.
 *
 * This describes the buffer the capture path delivers, which is not always
 * the sensor's native order: a mode whose readout is mirrored swaps the two
 * columns of every 2x2 tile, and one that is flipped as well swaps the rows.
 * The order is therefore a property of the mode rather than of the part, and
 * it is carried per mode rather than assumed.  Getting it wrong does not
 * fail -- it costs the picture its colour -- so it is worth stating where it
 * can be read rather than wherever it happens to be needed.
 */

enum ov5647_bayer_e
{
  OV5647_BAYER_BGGR = 0,
  OV5647_BAYER_GBRG = 1,
  OV5647_BAYER_GRBG = 2,
  OV5647_BAYER_RGGB = 3,
};

/* One capture mode: a geometry, the rate it runs at, and the link that
 * carries both.
 *
 * The sensor has no scaler and no format choice, so a mode is the whole of
 * what it can be asked to do, and it is what everything downstream has to
 * agree with: the board's D-PHY rate, the capture engine's geometry and
 * Bayer order, and the application's exposure ceiling are all derived from
 * these figures and from nothing else.  Stating them once, here, is what
 * keeps the three from drifting apart.
 *
 * The figures are sensor facts.  They come from the mode's register set and
 * its pixel clock -- the same values the upstream Linux driver programs in
 * its ov5647_modes[] table -- and are not the board's or the application's
 * to choose.
 */

struct ov5647_mode_s
{
  uint16_t width;  /* Output width, in pixels */
  uint16_t height; /* Output height, in pixels */
  uint16_t fps;    /* Frame rate, as advertised to the framework */
  uint16_t hts;    /* Horizontal total, in pixel clocks */
  uint16_t vts;    /* Vertical total, in lines */

  /* Register 0x3036: the PLL multiplier that sets the pixel clock.
   *
   * It is here because it is the one figure that ties the sensor to the link:
   * the pixel clock is this multiplied by a fixed reference, the link runs at
   * a rate that follows the pixel clock, and the D-PHY's timing parameters are
   * programmed from that rate.  A sensor left on another mode's multiplier is
   * therefore a receiver configured for a rate nothing is sending at, which is
   * worth being able to check rather than only being able to reason about.
   */

  uint8_t pll;

  uint32_t link_freq; /* MIPI link frequency, in Hz */
  uint8_t bayer;      /* enum ov5647_bayer_e, of the delivered buffer */
};

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The modes this driver implements, as indices into ov5647_mode().
 *
 * There are seven, in four geometries, and each geometry runs at two frame
 * rates.  That is a cheaper kind of change than it looks: a frame lasts
 * HTS * VTS pixel clocks, so holding everything else still and lengthening
 * VTS is the whole of it, and the receiver's link rate, the capture engine's
 * geometry and the encoder all stay exactly as they were.  Lengthening VTS
 * is also the only way this sensor can be given more exposure time than one
 * frame's worth of lines, which is what the second entry of each pair is
 * for.
 *
 * How far that can go is bounded by the width of the VTS field, and how far
 * it has to go is set by the frame rates wanted.  VTS is eleven bits, so a
 * frame is at most 2047 lines.  At the three geometries that run the same
 * 87.5 MHz pixel clock that is 22.5 fps, which is too fast for a 15 fps
 * entry; those three therefore lengthen HTS as well, to 2844 -- the figure
 * the upstream driver programs for the sensor's full-resolution mode, and
 * so a value that is known to work rather than a value derived here.  The
 * VGA geometry has a slower pixel clock and reaches 30 fps on VTS alone.
 * The mode table in ov5647.c works each of these out in full.
 *
 * The 1920x1080 entry is the one geometry that changes without changing the
 * link rate: it runs the same PLL multiplier as the binned modes, so the
 * D-PHY has nothing to settle and only the capture engine's buffers move.
 * It is also the only mode here that does not bin, which costs it light.
 *
 * The 1280x720 geometry changes the frame size, and not the link rate
 * either, for the same reason and by the same route as 1920x1080 -- but it
 * keeps the binned readout and crops it instead of reading the array out in
 * full, so it trades field of view for the pipeline's margin rather than
 * trading light.  The mode table works that out as well; what matters here
 * is that it is the one geometry whose reason for existing is what runs
 * after the sensor.
 *
 * The order here is the order the modes are enumerated to the application
 * in.  It is also what the framework's own initial format and frame rate are
 * read from, since it takes the first entry of each of the two lists the
 * sensor publishes (see initialize_frame_setting() in v4l2_cap.c), and the
 * first entry of each of those is this entry's geometry and this entry's
 * rate.  That is what OV5647_MODE_DEFAULT below names, and why it is the
 * first entry rather than a choice.  It is not a constraint on what can be
 * asked for -- every mode here is reachable at run time -- and an application
 * may name a different one before it opens the camera.
 *
 * The numbers are also what the application and the board pass to each other
 * when a stream changes modes, so a mode that is added goes at the end of
 * this list.  Renumbering an existing one would silently repoint requests
 * that name it by number rather than by name.
 */

#define OV5647_MODE_1920x1080_15 0u
#define OV5647_MODE_1296x960_30  1u
#define OV5647_MODE_1296x960_15  2u
#define OV5647_MODE_1280x720_30  3u
#define OV5647_MODE_1280x720_15  4u
#define OV5647_MODE_640x480_60   5u
#define OV5647_MODE_640x480_30   6u
#define OV5647_NUM_MODES         7u

/* The mode the sensor is left in when nothing has been asked for, and the one
 * a capture device is opened with.
 *
 * This is not a setting, and it is not a choice about what a stream will
 * look like.  A capture device has to exist before an application can ask it
 * for anything, and the engine behind this sensor has no scaler -- so a device
 * node implies a geometry, and something has to name one before the node can
 * be registered.  This names it.  It is the last decision the driver makes:
 * from the moment an application names its own mode, which it does before it
 * opens the camera, nothing here has any bearing on the stream.
 *
 * Being the table's first entry is the whole of the requirement on it.  The
 * framework reads the first entry of the frame sizes and of the frame rates as
 * the format a device is opened with, and those two entries are this mode's
 * own geometry and rate -- so a client that sets neither of the two is still
 * given a pair the capture engine was programmed for.  This, the two lists and
 * the mode table's order therefore move together: a mode inserted at the front
 * of the table is a change to all four.
 */

#define OV5647_MODE_DEFAULT OV5647_MODE_1920x1080_15

/* The shortest exposure the sensor can be asked for, in lines.  Four is the
 * sensor's own minimum, and it is also the margin the maximum leaves: a
 * frame cannot be exposed for the whole of itself, because the lines it is
 * read out on take time of their own.
 */

#define OV5647_EXPOSURE_MIN 4u

/* The longest exposure a mode can be given, in lines.  A mode's vertical
 * total is the length of one of its frames, and the exposure is bounded by
 * the frame it sits in.
 */

#define OV5647_MODE_EXPOSURE_MAX(m) ((uint32_t)(m)->vts - OV5647_EXPOSURE_MIN)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ov5647_mode
 *
 * Description:
 *   Describe one of the driver's capture modes.  The figures are sensor
 *   facts, and they are what the board's D-PHY rate, the capture engine's
 *   geometry and the application's exposure ceiling have to be derived
 *   from; reading them from here rather than repeating them is what keeps
 *   the three in step.
 *
 * Input Parameters:
 *   index - Which mode, one of OV5647_MODE_*.
 *
 * Returned Value:
 *   The mode, or NULL if the index names no mode.  The pointer refers to
 *   read-only storage and stays valid for the life of the program.
 *
 ****************************************************************************/

FAR const struct ov5647_mode_s *ov5647_mode(unsigned int index);

/****************************************************************************
 * Name: ov5647_initialize
 *
 * Description:
 *   Bind the OV5647 sensor driver to an I2C (SCCB) bus.  This only records
 *   the bus and the address; no hardware access happens here, so the board
 *   may call it before the sensor's power or clock is up.  The first real
 *   transaction is the chip-id probe in is_available(), which the capture
 *   framework performs when the device is registered.
 *
 * Input Parameters:
 *   i2c - The SCCB bus.  The driver keeps the pointer for the lifetime of
 *         the capture device; the board must not release it.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int ov5647_initialize(FAR struct i2c_master_s *i2c);

/****************************************************************************
 * Name: ov5647_sensor
 *
 * Description:
 *   Return the sensor handle to pass to capture_register().  Valid only
 *   after a successful ov5647_initialize().
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   The sensor handle, or NULL if the driver has not been initialised.
 *
 ****************************************************************************/

FAR struct imgsensor_s *ov5647_sensor(void);

#endif /* CONFIG_VIDEO_OV5647 */
#endif /* __DRIVERS_PLATFORM_DRIVERS_OV5647_OV5647_H */
