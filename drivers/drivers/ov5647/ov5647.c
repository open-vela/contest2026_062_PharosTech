/****************************************************************************
 * drivers/drivers/ov5647/ov5647.c
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
 * OmniVision OV5647 MIPI CSI-2 camera sensor driver.
 *
 * The sensor implements the NuttX capture framework's sensor side
 * (struct imgsensor_s).  Configuration goes over SCCB -- an I2C subset with
 * a 16-bit register address and no acknowledgement of the data phase --
 * which the standard NuttX I2C driver drives directly.  Image data goes
 * over two MIPI CSI-2 data lanes and is never seen here: the SoC side
 * (VICAP) owns the frame buffers.
 *
 * Only the 640x480 mode is implemented.  It is the 2x2 binned, subsampled,
 * full field of view mode, which is the cheapest to receive and the natural
 * starting point for bringing a capture path up.
 *
 * Format reporting
 * ----------------
 * The framework is told "NV12", not "RAW10", even though the sensor emits
 * RAW10.  The capture framework has no RAW pixel format at all, and an
 * unrecognised fourcc is silently mapped to JPEG_WITH_SUBIMG rather than
 * rejected, so declaring RAW would be a trap rather than an expression of
 * intent.  The SoC-side driver demosaics, and NV12 is what the pair
 * delivers together -- which is exactly what this declaration says.
 *
 * Register tables
 * ---------------
 * The register sequences below are the ones the upstream Linux ov5647
 * driver uses for the same mode (drivers/media/i2c/ov5647.c):
 * ov5647_common_regs and ov5647_640x480_10bpp.  The values are not
 * re-derived here; the datasheet only documents individual registers, and
 * a mode's register set is the vendor's calibration, not something to
 * reconstruct from first principles.
 *
 * Frame rate: the mode's own timing gives 504 lines of vertical total
 * against a 58.333 MHz pixel clock and 1852 pixels of horizontal total,
 * which is 62.5 frames/s of sensor timing before the pack's own blanking;
 * the framework side is asked for 30 fps, which is comfortably within it.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/videoio.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>
#include <nuttx/video/imgsensor.h>

#include "ov5647.h"

#ifdef CONFIG_VIDEO_OV5647

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register addresses. */

#define OV5647_SW_STANDBY   0x0100
#define OV5647_SW_RESET     0x0103
#define OV5647_CHIP_ID_HIGH 0x300a
#define OV5647_CHIP_ID_LOW  0x300b
#define OV5647_PAD_OUT      0x300d
#define OV5647_EXPOSURE     0x3500
#define OV5647_AEC_AGC      0x3503
#define OV5647_ANALOG_GAIN  0x350a

/* 0x350c/0x350d hold a frame-length difference the sensor adds of its own
 * accord while its own AEC is running.  The datasheet asks for zero when the
 * host owns the frame length, which this driver always does.
 */

#define OV5647_VTS_DIFF 0x350c

/* The three bits of 0x3503, each of which selects manual control of one
 * thing.  A bit that is set means the host writes it and the sensor's own
 * loop leaves it alone.
 */

#define OV5647_VTS_MANUAL       (1u << 2)
#define OV5647_AGC_MANUAL       (1u << 1)
#define OV5647_AEC_MANUAL       (1u << 0)
#define OV5647_HTS              0x380c
#define OV5647_VTS              0x380e
#define OV5647_FRAME_OFF_NUMBER 0x4202
#define OV5647_MIPI_CTRL00      0x4800
#define OV5647_MIPI_CTRL14      0x4814
#define OV5647_ISP_AWB          0x5001

#define OV5647_CHIP_ID          0x5647

/* MIPI_CTRL00 bits.  With the clock lane left ungated the receiver sees a
 * continuous high-speed clock, which is the least surprising thing to hand
 * a D-PHY: there is no lane-state transition to re-synchronise on between
 * frames.
 */

#define OV5647_MIPI_CTRL00_CLOCK_LANE_DISABLE (1u << 0)
#define OV5647_MIPI_CTRL00_BUS_IDLE           (1u << 2)
#define OV5647_MIPI_CTRL00_LINE_SYNC_ENABLE   (1u << 4)
#define OV5647_MIPI_CTRL00_CLOCK_LANE_GATE    (1u << 5)

/* Software reset settling time.  The datasheet asks for the reset to be
 * given time to complete before the register set is written.
 */

#define OV5647_RESET_DELAY_US 5000

/* The modes this driver implements.  Exactly one is selected by
 * CONFIG_OV5647_MODE_* at build time; the sensor has no scaler, so the mode
 * is what the SoC-side demosaicer and encoder must be configured to match.
 *
 * Mode timing.  One frame lasts HTS * VTS pixel clocks, which is where the
 * frame rate comes from.  Both registers have to be written for that to
 * hold at all -- the reset values are the sensor's full-resolution timing --
 * and VTS is also what bounds the longest usable exposure time.
 *
 * The figures are the same ones the upstream Linux driver programs (see its
 * ov5647_modes[] table), and the link frequency the board must run its D-PHY
 * at follows each mode:
 *   - 640x480:  pixel clock 58.333 MHz, link 145.833 MHz, 2 lanes
 *   - 1296x972: pixel clock 87.5 MHz,   link 218.75 MHz,  2 lanes
 */

#if defined(CONFIG_OV5647_MODE_1296x960)

/* 1296x960: 2x2 binned full field of view, ~30 fps.  The binned array is
 * 1296x972, and the output height is trimmed to 960 so that the whole
 * pipeline -- VICAP stride, the encoder's 16-pixel macroblock grid, and
 * camenc's copy -- lands on a multiple of sixteen with no crop in hardware
 * and no row-wise copy in software.  Twelve rows (1.2%) fall off the bottom,
 * which is imperceptible.
 *
 * The pixel clock is 87.5 MHz and HTS*VTS = 1896 * 1435, so the frame rate
 * is 87.5e6 / (1896 * 1435) = 32.2 fps, advertised as 30.
 */

#define OV5647_MODE_WIDTH  1296
#define OV5647_MODE_HEIGHT 960
#define OV5647_MODE_FPS    30
#define OV5647_MODE_HTS    1896u
#define OV5647_MODE_VTS    1435u

#else /* CONFIG_OV5647_MODE_640x480 */

/* 640x480: 2x2 binned and subsampled, full field of view, 60 fps.  The
 * pixel clock is 58.333 MHz and HTS*VTS = 1852 * 504, which is 62.5 fps,
 * advertised as 60.
 */

#define OV5647_MODE_WIDTH  640
#define OV5647_MODE_HEIGHT 480
#define OV5647_MODE_FPS    60
#define OV5647_MODE_HTS    1852u
#define OV5647_MODE_VTS    504u

#endif

/* AEC and AGC are switched off by the common register table, so the exposure
 * time and the gain have to be set explicitly: there is no auto loop to fall
 * back on, and a sensor given neither integrates for whatever the reset value
 * happens to be.  A reset exposure of zero streams a stable, nearly black
 * frame -- which looks like a frozen picture rather than an unprogrammed
 * sensor.  These are the upstream control defaults.
 *
 * The two loops can be handed back to the sensor at run time through
 * V4L2_CID_EXPOSURE_AUTO and V4L2_CID_ISO_SENSITIVITY_AUTO; see
 * ov5647_set_value().  Off is the default because a stream that is suddenly
 * exposed by a loop the application cannot see is a different stream, and an
 * application that wants one should ask.
 *
 * The frame length stays manual either way.  The sensor's AEC is entitled to
 * lengthen a frame to integrate for longer -- the datasheet describes night
 * mode as "slowing down the original frame rate" -- and this driver's frame
 * rate is a promise made in the stream's VUI and relied on by whoever is
 * pacing frames.  Letting the loop move VTS would break that promise from
 * inside the sensor, where nothing can observe it.  So VTS is held manual
 * and the frame length is written by the driver in every configuration; the
 * exposure the AEC may choose is bounded by the frame it sits in.
 */

/* The exposure and gain are the only controls over how bright the picture
 * comes out, because the sensor's own loops are switched off.  Both are
 * board-level decisions rather than fixed properties of the part: how much
 * light reaches the sensor depends on the lens and the scene, so they are
 * configurable and default to values that are merely reasonable.
 *
 * Exposure is in lines; the mode's maximum leaves the frame just long enough
 * to read out.  Gain runs sixteen to one, so a default of 32 is 2.0x.
 *
 * Both are only the starting point.  They are also reachable at run time
 * through the sensor control interface, which is what makes them usable for
 * tuning: finding the right gain for a given scene means trying several, and
 * a setting that can only be changed by rebuilding costs a flash cycle each
 * time.
 */

#define OV5647_EXPOSURE_MIN 4u

#if defined(CONFIG_OV5647_EXPOSURE_LINES) && CONFIG_OV5647_EXPOSURE_LINES > 0
#define OV5647_EXPOSURE_INIT ((uint32_t)CONFIG_OV5647_EXPOSURE_LINES)
#else
#define OV5647_EXPOSURE_INIT (OV5647_MODE_VTS - 4u)
#endif

#ifdef CONFIG_OV5647_ANALOG_GAIN
#define OV5647_ANALOG_GAIN_INIT ((uint32_t)CONFIG_OV5647_ANALOG_GAIN)
#else
#define OV5647_ANALOG_GAIN_INIT 32u
#endif

/* Gain is a ten-bit value covering sixteen to one, per the sensor's own
 * scale; the whole range is reachable.
 */

#define OV5647_ANALOG_GAIN_MIN 16u
#define OV5647_ANALOG_GAIN_MAX 1023u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ov5647_reg_s
{
  uint16_t reg;
  uint8_t value;
};

struct ov5647_s
{
  struct imgsensor_s sensor; /* Must be first */
  mutex_t lock;
  FAR struct i2c_master_s *i2c;
  uint8_t addr;
  bool bound; /* ov5647_initialize() has run */
  bool streaming;

  /* Current manual settings.  Held here rather than only in the registers so
   * that a change made through the control interface survives a stop and
   * start of the stream.
   */

  uint32_t exposure_lines;
  uint32_t analog_gain;

  /* Whether the sensor's own loops are running, and so whether the two
   * values above are what the picture is being exposed with.  False is the
   * default: the settings in them are the ones this driver wrote.
   *
   * The frame length is not here because it is manual in every
   * configuration -- see the note above the register table.
   */

  bool aec_auto;
  bool agc_auto;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Common register set: the sensor-wide configuration that is written before
 * any mode's own registers.  Note the first two entries: software standby
 * is entered and then the sensor is soft-reset, so the reset clears any
 * state a previous user left behind and the rest of the table rebuilds from
 * a known point.
 */

static const struct ov5647_reg_s g_ov5647_common_regs[] = {
  { 0x0100, 0x00 },
  { 0x0103, 0x01 },
  { 0x3034, 0x1a },
  { 0x3035, 0x21 },
  { 0x303c, 0x11 },
  { 0x3106, 0xf5 },
  { 0x3827, 0xec },
  { 0x370c, 0x03 },
  { 0x5000, 0x06 },
  { 0x5003, 0x08 },
  { 0x5a00, 0x08 },
  { 0x3000, 0x00 },
  { 0x3001, 0x00 },
  { 0x3002, 0x00 },
  { 0x3016, 0x08 },
  { 0x3017, 0xe0 },
  { 0x3018, 0x44 },
  { 0x301c, 0xf8 },
  { 0x301d, 0xf0 },
  { 0x3a18, 0x00 },
  { 0x3a19, 0xf8 },
  { 0x3c01, 0x80 },
  { 0x3b07, 0x0c },
  { 0x3630, 0x2e },
  { 0x3632, 0xe2 },
  { 0x3633, 0x23 },
  { 0x3634, 0x44 },
  { 0x3636, 0x06 },
  { 0x3620, 0x64 },
  { 0x3621, 0xe0 },
  { 0x3600, 0x37 },
  { 0x3704, 0xa0 },
  { 0x3703, 0x5a },
  { 0x3715, 0x78 },
  { 0x3717, 0x01 },
  { 0x3731, 0x02 },
  { 0x370b, 0x60 },
  { 0x3705, 0x1a },
  { 0x3f05, 0x02 },
  { 0x3f06, 0x10 },
  { 0x3f01, 0x0a },
  { 0x3a08, 0x01 },
  { 0x3a0f, 0x58 },
  { 0x3a10, 0x50 },
  { 0x3a1b, 0x58 },
  { 0x3a1e, 0x50 },
  { 0x3a11, 0x60 },
  { 0x3a1f, 0x28 },
  { 0x4001, 0x02 },
  { 0x4000, 0x09 },

  /* Pinned before the manual-control register that selects it, so that the
   * sensor never has a frame length of its own to add to the one this driver
   * writes.  The reset value of 0x350c/0x350d is not zero.
   */

  { OV5647_VTS_DIFF, 0x00 },
  { OV5647_VTS_DIFF + 1, 0x00 },

  /* AEC and AGC manual, VTS manual.  See the note above the table. */

  { OV5647_AEC_AGC,
    OV5647_VTS_MANUAL | OV5647_AGC_MANUAL | OV5647_AEC_MANUAL },
};

/* 640x480 10-bit mode: 2x2 binned and subsampled, full field of view.
 * The final two entries leave software standby and, in the upstream
 * driver, would also set MIPI_CTRL00 -- this driver writes MIPI_CTRL00
 * itself when streaming starts, because the clock-lane behaviour is a
 * receiver-side decision rather than a property of the mode.
 */

#if !defined(CONFIG_OV5647_MODE_1296x960)
static const struct ov5647_reg_s g_ov5647_640x480_regs[] = {
  { 0x3036, 0x46 }, { 0x3821, 0x03 }, { 0x3820, 0x41 }, { 0x3612, 0x59 },
  { 0x3618, 0x00 }, { 0x3814, 0x35 }, { 0x3815, 0x35 }, { 0x3708, 0x64 },
  { 0x3709, 0x52 }, { 0x3800, 0x00 }, { 0x3801, 0x10 }, { 0x3802, 0x00 },
  { 0x3803, 0x00 }, { 0x3804, 0x0a }, { 0x3805, 0x2f }, { 0x3806, 0x07 },
  { 0x3807, 0x9f }, { 0x3808, 0x02 }, { 0x3809, 0x80 }, { 0x380a, 0x01 },
  { 0x380b, 0xe0 }, { 0x3a09, 0x2e }, { 0x3a0a, 0x00 }, { 0x3a0b, 0xfb },
  { 0x3a0d, 0x02 }, { 0x3a0e, 0x01 }, { 0x4004, 0x02 }, { 0x4800, 0x34 },
  { 0x0100, 0x01 },
};
#endif

/* 1296x960 (2x2 binned) 10-bit mode: full field of view, no subsampling.
 * Copied from the upstream Linux driver's ov5647_2x2binned_10bpp table, with
 * one deliberate change: the output height (0x380a/0x380b) is trimmed from
 * 0x03cc (972) to 0x03c0 (960) so it falls on the encoder's 16-pixel
 * macroblock grid, for the reason given in the mode comment above.
 *
 * The mirror/flip bits (0x3821 = 0x03, 0x3820 = 0x41) are the same as the
 * VGA mode's, so the Bayer order the SoC side reconstructs is the same GBRG
 * as the VGA mode reports -- see the board's kickpi_k7_camera.c for the
 * full reasoning.  Unlike the VGA mode this one needs no subsampling
 * registers (0x3708/0x3709, 0x3814/0x3815) because the 2x2 bin alone
 * reaches 1296x972.
 *
 * The exposure/gain registers are not here: they are sensor settings the
 * driver owns and writes from its state on every stream start, for the same
 * reason the VGA mode leaves them out.
 */

#if defined(CONFIG_OV5647_MODE_1296x960)
static const struct ov5647_reg_s g_ov5647_1296x960_regs[] = {
  { 0x3036, 0x69 }, { 0x3821, 0x03 }, { 0x3820, 0x41 }, { 0x3612, 0x59 },
  { 0x3618, 0x00 }, { 0x5002, 0x41 }, { 0x3800, 0x00 }, { 0x3801, 0x00 },
  { 0x3802, 0x00 }, { 0x3803, 0x00 }, { 0x3804, 0x0a }, { 0x3805, 0x3f },
  { 0x3806, 0x07 }, { 0x3807, 0xa3 }, { 0x3808, 0x05 }, { 0x3809, 0x10 },
  { 0x380a, 0x03 }, { 0x380b, 0xc0 }, { 0x3811, 0x0c }, { 0x3813, 0x06 },
  { 0x3814, 0x31 }, { 0x3815, 0x31 }, { 0x3a09, 0x28 }, { 0x3a0a, 0x00 },
  { 0x3a0b, 0xf6 }, { 0x3a0d, 0x08 }, { 0x3a0e, 0x06 }, { 0x4004, 0x04 },
  { 0x4837, 0x16 }, { 0x4800, 0x24 }, { 0x0100, 0x01 },
};
#endif

/* The mode's timing, written every time the sensor starts streaming.  The
 * exposure and the gain are deliberately not here: they are the settings a
 * user may change, so they live in the driver's state and are written by
 * ov5647_write_exposure() and ov5647_write_gain() below.
 *
 * The multi-byte registers are big-endian, which is why each value appears
 * one byte per entry.
 */

static const struct ov5647_reg_s g_ov5647_timing_regs[] = {
  { OV5647_HTS, (OV5647_MODE_HTS >> 8) & 0xffu },
  { OV5647_HTS + 1, OV5647_MODE_HTS & 0xffu },
  { OV5647_VTS, (OV5647_MODE_VTS >> 8) & 0xffu },
  { OV5647_VTS + 1, OV5647_MODE_VTS & 0xffu },
};

/* What the framework asks the application for, and what it validates
 * against.  The list is what the pair (this sensor + the SoC-side
 * demosaicer) delivers, which is the only thing the application can
 * observe.
 */

static struct v4l2_fmtdesc g_ov5647_fmtdescs[] = {
  {
      .index = 0,
      .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
      .description = "YUV 4:2:0 planar (NV12)",
      .pixelformat = V4L2_PIX_FMT_NV12,
  },
};

static struct v4l2_frmsizeenum g_ov5647_frmsizes[] =
{
  {
    .index  = 0,
    .type   = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width  = OV5647_MODE_WIDTH,
        .height = OV5647_MODE_HEIGHT,
      },
  },
};

static struct v4l2_frmivalenum g_ov5647_frmintervals[] =
{
  {
    .index     = 0,
    .type      = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete =
      {
        .numerator   = 1,
        .denominator = OV5647_MODE_FPS,
      },
  },
};

static struct ov5647_s g_ov5647 = {
  .lock = NXMUTEX_INITIALIZER,
  .addr = CONFIG_OV5647_I2C_ADDR,
  .exposure_lines = OV5647_EXPOSURE_INIT,
  .analog_gain = OV5647_ANALOG_GAIN_INIT,
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ov5647_is_available(FAR struct imgsensor_s *sensor);
static int ov5647_init(FAR struct imgsensor_s *sensor);
static int ov5647_uninit(FAR struct imgsensor_s *sensor);
static const char *ov5647_get_driver_name(FAR struct imgsensor_s *sensor);
static int ov5647_validate_frame_setting(FAR struct imgsensor_s *sensor,
                                         imgsensor_stream_type_t type,
                                         uint8_t nr_datafmts,
                                         FAR imgsensor_format_t *datafmts,
                                         FAR imgsensor_interval_t *interval);
static int ov5647_start_capture(FAR struct imgsensor_s *sensor,
                                imgsensor_stream_type_t type,
                                uint8_t nr_datafmts,
                                FAR imgsensor_format_t *datafmts,
                                FAR imgsensor_interval_t *interval);
static int ov5647_stop_capture(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type);
static int ov5647_get_frame_interval(FAR struct imgsensor_s *sensor,
                                     imgsensor_stream_type_t type,
                                     FAR imgsensor_interval_t *interval);
static int ov5647_get_supported_value(FAR struct imgsensor_s *sensor,
                                      uint32_t id,
                                      FAR imgsensor_supported_value_t *value);
static int ov5647_get_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, FAR imgsensor_value_t *value);
static int ov5647_set_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, imgsensor_value_t value);

static int ov5647_write_exposure(FAR struct ov5647_s *priv);
static int ov5647_write_gain(FAR struct ov5647_s *priv);

static const struct imgsensor_ops_s g_ov5647_ops = {
  .is_available = ov5647_is_available,
  .init = ov5647_init,
  .uninit = ov5647_uninit,
  .get_driver_name = ov5647_get_driver_name,
  .validate_frame_setting = ov5647_validate_frame_setting,
  .start_capture = ov5647_start_capture,
  .stop_capture = ov5647_stop_capture,
  .get_frame_interval = ov5647_get_frame_interval,
  .get_supported_value = ov5647_get_supported_value,
  .get_value = ov5647_get_value,
  .set_value = ov5647_set_value,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ov5647_putreg / ov5647_getreg
 *
 * Description:
 *   Single register access.  The SCCB register address is 16 bits and the
 *   data is one byte, so a write is a three-byte transaction.
 *
 ****************************************************************************/

static int ov5647_putreg(FAR struct ov5647_s *priv, uint16_t reg,
                         uint8_t value)
{
  struct i2c_msg_s msg;
  uint8_t buf[3];
  int ret;

  buf[0] = (uint8_t)(reg >> 8);
  buf[1] = (uint8_t)(reg & 0xffu);
  buf[2] = value;

  msg.frequency = CONFIG_OV5647_I2C_FREQUENCY;
  msg.addr = priv->addr;
  msg.flags = 0;
  msg.buffer = buf;
  msg.length = 3;

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);

  /* Normalise the result before returning it.
   *
   * NuttX documents I2C_TRANSFER() as "OK, or a negated errno", but in
   * practice some master drivers report the number of messages transferred
   * instead, and at least one reports an internal status mask.  A caller
   * that forwards that value straight up to the capture framework therefore
   * risks reporting success as a failure: the framework treats a sensor
   * init() result of anything other than OK as "no sensor present".
   *
   * Collapsing everything non-negative to OK makes this driver's contract
   * unambiguous regardless of which master it is bound to.
   */

  return ret < 0 ? ret : OK;
}

static int ov5647_getreg(FAR struct ov5647_s *priv, uint16_t reg,
                         FAR uint8_t *value)
{
  struct i2c_msg_s msg[2];
  uint8_t addr[2];
  int ret;

  addr[0] = (uint8_t)(reg >> 8);
  addr[1] = (uint8_t)(reg & 0xffu);

  msg[0].frequency = CONFIG_OV5647_I2C_FREQUENCY;
  msg[0].addr = priv->addr;
  msg[0].flags = 0;
  msg[0].buffer = addr;
  msg[0].length = 2;

  msg[1].frequency = CONFIG_OV5647_I2C_FREQUENCY;
  msg[1].addr = priv->addr;
  msg[1].flags = I2C_M_READ;
  msg[1].buffer = value;
  msg[1].length = 1;

  ret = I2C_TRANSFER(priv->i2c, msg, 2);
  if (ret < 0)
    {
      return ret;
    }

  return OK;
}

static int ov5647_putregs(FAR struct ov5647_s *priv,
                          FAR const struct ov5647_reg_s *regs, size_t nregs)
{
  size_t i;

  for (i = 0; i < nregs; i++)
    {
      int ret = ov5647_putreg(priv, regs[i].reg, regs[i].value);

      if (ret < 0)
        {
          _err("ERROR: OV5647 write 0x%04x failed: %d\n", regs[i].reg, ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: ov5647_probe
 ****************************************************************************/

static int ov5647_probe(FAR struct ov5647_s *priv)
{
  uint8_t high;
  uint8_t low;
  int ret;

  ret = ov5647_getreg(priv, OV5647_CHIP_ID_HIGH, &high);
  if (ret < 0)
    {
      return ret;
    }

  ret = ov5647_getreg(priv, OV5647_CHIP_ID_LOW, &low);
  if (ret < 0)
    {
      return ret;
    }

  if ((((uint16_t)high << 8) | low) != OV5647_CHIP_ID)
    {
      _err("ERROR: OV5647 wrong chip id 0x%02x%02x (expected 0x5647)\n", high,
           low);
      return -ENODEV;
    }

  return OK;
}

/****************************************************************************
 * Name: ov5647_is_available
 ****************************************************************************/

static bool ov5647_is_available(FAR struct imgsensor_s *sensor)
{
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;
  bool available;

  if (!priv->bound)
    {
      return false;
    }

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return false;
    }

  available = ov5647_probe(priv) == OK;

  nxmutex_unlock(&priv->lock);
  return available;
}

/****************************************************************************
 * Name: ov5647_init
 ****************************************************************************/

static int ov5647_init(FAR struct imgsensor_s *sensor)
{
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ov5647_probe(priv);
  if (ret < 0)
    {
      goto out;
    }

  /* The module has its own 25 MHz oscillator and no SoC-controlled power or
   * reset line, so a software reset over SCCB is the only reset available.
   * The common table's first entries perform it.
   */

  ret = ov5647_putregs(priv, g_ov5647_common_regs,
                       sizeof(g_ov5647_common_regs) /
                           sizeof(g_ov5647_common_regs[0]));
  if (ret < 0)
    {
      goto out;
    }

  /* A software reset needs settling time before the mode's own registers
   * are programmed on top of it.
   */

  usleep(OV5647_RESET_DELAY_US);

#if defined(CONFIG_OV5647_MODE_1296x960)
  ret = ov5647_putregs(priv, g_ov5647_1296x960_regs,
                       sizeof(g_ov5647_1296x960_regs) /
                           sizeof(g_ov5647_1296x960_regs[0]));
#else
  ret = ov5647_putregs(priv, g_ov5647_640x480_regs,
                       sizeof(g_ov5647_640x480_regs) /
                           sizeof(g_ov5647_640x480_regs[0]));
#endif
  if (ret < 0)
    {
      goto out;
    }

  /* Virtual channel 0: the driver reports everything on channel 0, and the
   * SoC side filters for exactly that.
   */

  ret = ov5647_putreg(priv, OV5647_MIPI_CTRL14, 0x00);
  if (ret < 0)
    {
      goto out;
    }

  /* Leave the sensor's own white balance block switched off.
   *
   * This register enables an automatic white balance that scales the colour
   * channels inside the sensor, and it runs on statistics the sensor gathers
   * itself.  On a part that outputs raw Bayer, that scaling lands in the
   * middle of the data the host is going to reconstruct colour from, and a
   * mismatch between the channels shows up as a cast whose size follows the
   * signal -- faint in the shadows, strong in the midtones, gone again once
   * the channels clip together.
   *
   * The upstream driver leaves this off, and the reason is not hard to see:
   * with the raw stream intact the host decides the colour, and any gain
   * applied here is a decision taken twice.  Turning it on was an extra
   * addition to this driver rather than something the reference does.
   */

  ret = ov5647_putreg(priv, OV5647_ISP_AWB, 0x00);
  if (ret < 0)
    {
      goto out;
    }

  _info("OV5647: initialised, %ux%u RAW10, 2 lanes\n", OV5647_MODE_WIDTH,
        OV5647_MODE_HEIGHT);

  /* Report success explicitly rather than letting the last register write
   * stand in for it.  This is the one return value in the driver that the
   * capture framework compares against OK exactly, so it is the wrong place
   * to depend on whatever the final I2C transfer happened to report.
   */

  ret = OK;

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: ov5647_uninit
 ****************************************************************************/

static int ov5647_uninit(FAR struct imgsensor_s *sensor)
{
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->bound)
    {
      /* Put the sensor into software standby so it stops driving the MIPI
       * lanes; the receiver side is torn down by its own driver.
       */

      ov5647_putreg(priv, OV5647_SW_STANDBY, 0x00);
    }

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: ov5647_get_driver_name
 ****************************************************************************/

static const char *ov5647_get_driver_name(FAR struct imgsensor_s *sensor)
{
  return "OV5647";
}

/****************************************************************************
 * Name: ov5647_validate_frame_setting
 ****************************************************************************/

static int ov5647_validate_frame_setting(FAR struct imgsensor_s *sensor,
                                         imgsensor_stream_type_t type,
                                         uint8_t nr_datafmts,
                                         FAR imgsensor_format_t *datafmts,
                                         FAR imgsensor_interval_t *interval)
{
  int i;

  if (datafmts == NULL || nr_datafmts == 0)
    {
      return -EINVAL;
    }

  /* The sensor has no scaler in this mode and no format choice: the mode is
   * what it is, and the SoC side demosaics to a fixed format.  Everything
   * the framework can ask for is therefore either exactly right or
   * impossible.
   */

  for (i = 0; i < nr_datafmts; i++)
    {
      if (datafmts[i].pixelformat != IMGSENSOR_PIX_FMT_NV12 &&
          datafmts[i].pixelformat != IMGSENSOR_PIX_FMT_YUV420P)
        {
          return -EINVAL;
        }

      if (datafmts[i].width != OV5647_MODE_WIDTH ||
          datafmts[i].height != OV5647_MODE_HEIGHT)
        {
          return -EINVAL;
        }
    }

  if (interval != NULL && interval->denominator != 0 &&
      interval->numerator != 0)
    {
      /* Only the exact mode rate is offered; a slower request would need
       * the sensor to be re-timed, which this driver does not do.
       */

      if (interval->denominator >
          OV5647_MODE_FPS * (uint32_t)interval->numerator)
        {
          return -EINVAL;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: ov5647_write_exposure / ov5647_write_gain
 *
 * Description:
 *   Push the current manual exposure and gain into the sensor.  The caller
 *   must hold the driver lock.
 *
 *   The exposure register is 20 bits with the bottom four counting fractions
 *   of a line, so a whole number of lines is written shifted up by four.
 *   Both registers are big-endian.
 *
 ****************************************************************************/

static int ov5647_write_exposure(FAR struct ov5647_s *priv)
{
  uint32_t value = priv->exposure_lines * 16u;
  struct ov5647_reg_s regs[3] = {
    { OV5647_EXPOSURE, (value >> 16) & 0xffu },
    { OV5647_EXPOSURE + 1, (value >> 8) & 0xffu },
    { OV5647_EXPOSURE + 2, value & 0xffu },
  };

  return ov5647_putregs(priv, regs, 3);
}

static int ov5647_write_gain(FAR struct ov5647_s *priv)
{
  struct ov5647_reg_s regs[2] = {
    { OV5647_ANALOG_GAIN, (priv->analog_gain >> 8) & 0xffu },
    { OV5647_ANALOG_GAIN + 1, priv->analog_gain & 0xffu },
  };

  return ov5647_putregs(priv, regs, 2);
}

/****************************************************************************
 * Name: ov5647_write_manual_ctrl
 *
 * Description:
 *   Push the choice of which of the sensor's own loops may run.  The caller
 *   must hold the driver lock.
 *
 *   The frame length is manual in every configuration, and that is a
 *   decision rather than an omission -- see the note above the register
 *   table.  It is written here rather than in the table alone so that the
 *   bit is reasserted whenever the other two change, which is the only way
 *   it can be relied on: 0x3503 is written as a whole byte.
 *
 ****************************************************************************/

static int ov5647_write_manual_ctrl(FAR struct ov5647_s *priv)
{
  uint8_t value = (uint8_t)OV5647_VTS_MANUAL;

  if (!priv->agc_auto)
    {
      value |= (uint8_t)OV5647_AGC_MANUAL;
    }

  if (!priv->aec_auto)
    {
      value |= (uint8_t)OV5647_AEC_MANUAL;
    }

  return ov5647_putreg(priv, OV5647_AEC_AGC, value);
}

/****************************************************************************
 * Name: ov5647_start_capture
 ****************************************************************************/

static int ov5647_start_capture(FAR struct imgsensor_s *sensor,
                                imgsensor_stream_type_t type,
                                uint8_t nr_datafmts,
                                FAR imgsensor_format_t *datafmts,
                                FAR imgsensor_interval_t *interval)
{
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->bound)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODEV;
    }

  /* The mode's timing and the exposure/gain pair.
   *
   * Upstream applies its control defaults at exactly this point in the
   * sequence -- after the mode registers, before the interface is opened.
   * The order matters: MIPI_CTRL00 is what lets the sensor leave bus-idle,
   * and it should not do so until there is a complete configuration behind
   * it, or the first frames are exposed with whatever the previous run left.
   */

  ret = ov5647_putregs(priv, g_ov5647_timing_regs,
                       sizeof(g_ov5647_timing_regs) /
                           sizeof(g_ov5647_timing_regs[0]));
  if (ret < 0)
    {
      goto out;
    }

  /* The exposure and gain come from the driver's state rather than from the
   * table above, so that a value set through the control interface is what
   * the next stream actually uses.
   */

  ret = ov5647_write_exposure(priv);
  if (ret < 0)
    {
      goto out;
    }

  ret = ov5647_write_gain(priv);
  if (ret < 0)
    {
      goto out;
    }

  /* MIPI_CTRL00: the clock lane is left ungated, so the receiver sees a
   * continuous high-speed clock and has no lane-state transition to
   * resynchronise on between frames.  This is what upstream writes when the
   * clock is not non-continuous; the clock-lane gate and the line-sync bit
   * belong to the other configuration and stay clear.
   */

  ret = ov5647_putreg(priv, OV5647_MIPI_CTRL00, OV5647_MIPI_CTRL00_BUS_IDLE);
  if (ret < 0)
    {
      goto out;
    }

  /* Group parameter hold off / release: these two are the pair the upstream
   * driver toggles around the stream, and they gate the sensor's own
   * double-buffered parameter update.
   */

  ret = ov5647_putreg(priv, OV5647_FRAME_OFF_NUMBER, 0x00);
  if (ret < 0)
    {
      goto out;
    }

  ret = ov5647_putreg(priv, OV5647_PAD_OUT, 0x00);
  if (ret < 0)
    {
      goto out;
    }

  /* Read back what the sensor is actually holding.
   *
   * The exposure and gain written above are the only things that decide how
   * bright the picture comes out, and a write that silently did not land
   * looks exactly like a scene that is too dark.  Since the two are
   * indistinguishable from the captured image alone, print the register
   * values instead of assuming them.  This runs once per stream start, in
   * task context, so the transfers are cheap and allowed to sleep.
   */

  {
    uint8_t exposure[3] = { 0, 0, 0 };
    uint8_t gain[2] = { 0, 0 };
    uint8_t hts[2] = { 0, 0 };
    uint8_t vts[2] = { 0, 0 };
    uint8_t aec_agc = 0;
    uint8_t mipi = 0;

    ov5647_getreg(priv, OV5647_EXPOSURE, &exposure[0]);
    ov5647_getreg(priv, OV5647_EXPOSURE + 1, &exposure[1]);
    ov5647_getreg(priv, OV5647_EXPOSURE + 2, &exposure[2]);
    ov5647_getreg(priv, OV5647_ANALOG_GAIN, &gain[0]);
    ov5647_getreg(priv, OV5647_ANALOG_GAIN + 1, &gain[1]);
    ov5647_getreg(priv, OV5647_HTS, &hts[0]);
    ov5647_getreg(priv, OV5647_HTS + 1, &hts[1]);
    ov5647_getreg(priv, OV5647_VTS, &vts[0]);
    ov5647_getreg(priv, OV5647_VTS + 1, &vts[1]);
    ov5647_getreg(priv, OV5647_AEC_AGC, &aec_agc);
    ov5647_getreg(priv, OV5647_MIPI_CTRL00, &mipi);

    _info("OV5647: readback exposure 0x%02x%02x%02x gain 0x%02x%02x"
          " hts 0x%02x%02x vts 0x%02x%02x aec_agc 0x%02x mipi_ctrl00 0x%02x\n",
          exposure[0], exposure[1], exposure[2], gain[0], gain[1], hts[0],
          hts[1], vts[0], vts[1], aec_agc, mipi);
  }

  priv->streaming = true;

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: ov5647_stop_capture
 ****************************************************************************/

static int ov5647_stop_capture(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type)
{
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;

  /* Stop the stream, not the sensor.
   *
   * The framework calls this from complete_capture(), which runs under a
   * spinlock with preemption disabled -- it stops the stream there when it
   * cannot find a vacant buffer for the frame it just finished.  A sensor
   * write is an I2C transfer, which sleeps, so it cannot be done here at
   * all; and taking the driver mutex would be just as wrong.  Leaving the
   * sensor streaming is the established answer for an I2C sensor (the
   * upstream esp32s3 OV2640 imgsensor does the same): the SoC-side capture
   * is what stops, and ov5647_start_capture() rewrites the interface
   * registers, so the sensor is reprogrammed before it is listened to again.
   */

  priv->streaming = false;
  return OK;
}

/****************************************************************************
 * Name: ov5647_get_frame_interval
 ****************************************************************************/

static int ov5647_get_frame_interval(FAR struct imgsensor_s *sensor,
                                     imgsensor_stream_type_t type,
                                     FAR imgsensor_interval_t *interval)
{
  if (interval == NULL)
    {
      return -EINVAL;
    }

  interval->numerator = 1;
  interval->denominator = OV5647_MODE_FPS;

  return OK;
}

/****************************************************************************
 * Name: ov5647_get_supported_value / get_value / set_value
 *
 * Description:
 *   The sensor-side control interface.
 *
 *   Four controls are meaningful: the exposure time and the analog gain,
 *   which the driver writes; and the two auto controls, which select whether
 *   the sensor's own loops write them instead.  They reach here from
 *   VIDIOC_S_CTRL, which makes them adjustable on a running system -- the
 *   point of having them at all, since choosing an exposure or a gain for a
 *   scene means trying a few, and rebuilding for each is not a practical way
 *   to do that.
 *
 *   Notice what is not here: a target brightness, an exposure meter, a
 *   convergence rate.  Those are the decisions of an exposure loop, and the
 *   loop belongs to the application, which is the only party that knows what
 *   the picture is for.  What the driver contributes is the mechanism --
 *   write these registers, and here is what the capture side measured the
 *   frame to be (see drivers/include/cam3a.h).  The one exception is the
 *   sensor's own AEC, which is offered because it exists and can be selected,
 *   but is off by default: the application's loop is the one that can be
 *   reasoned about, and two loops steering the same registers cannot.
 *
 *   Anything else is reported as unsupported rather than silently ignored,
 *   so a caller is never left believing a request took effect.
 *
 *   Note that the framework does not pass a meaningful size alongside the
 *   value for the single-control ioctl, so the size argument is not checked;
 *   the value itself is read through the union's 32-bit member, which is
 *   what that path fills in.
 *
 ****************************************************************************/

static int ov5647_get_supported_value(FAR struct imgsensor_s *sensor,
                                      uint32_t id,
                                      FAR imgsensor_supported_value_t *value)
{
  return -ENOTSUP;
}

static int ov5647_get_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, FAR imgsensor_value_t *value)
{
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;

  if (value == NULL)
    {
      return -EINVAL;
    }

  switch (id)
    {
      case IMGSENSOR_ID_EXPOSURE_ABSOLUTE:
        value->value32 = (int32_t)priv->exposure_lines;
        break;

      case IMGSENSOR_ID_ISO_SENSITIVITY:
        value->value32 = (int32_t)priv->analog_gain;
        break;

        /* While a loop the driver does not run is in charge, the two values
         * above are the last ones this driver wrote and not what the picture
         * is being exposed with.  Reading the sensor's own choice back is not
         * attempted: the datasheet describes 0x350a/0x350b as a real-gain
         * output but does not say the exposure registers are updated by the
         * loop, so a readback would be a guess presented as a measurement.
         * The two controls below are how a caller tells which case it is in.
         */

      case IMGSENSOR_ID_EXPOSURE_AUTO:
        value->value32 =
            priv->aec_auto ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL;
        break;

      case IMGSENSOR_ID_ISO_SENSITIVITY_AUTO:
        value->value32 = priv->agc_auto ? V4L2_ISO_SENSITIVITY_AUTO
                                        : V4L2_ISO_SENSITIVITY_MANUAL;
        break;

      default:
        return -ENOTSUP;
    }

  return OK;
}

static int ov5647_set_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, imgsensor_value_t value)
{
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;
  uint32_t wanted;
  bool auto_wanted;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->bound)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODEV;
    }

  switch (id)
    {
      case IMGSENSOR_ID_EXPOSURE_ABSOLUTE:
        /* The exposure cannot outlast the frame it is part of, so the mode's
         * line count is the ceiling.  Asking for more is clamped rather than
         * refused: a caller sweeping the range should see it stop getting
         * brighter, not start failing.
         */

        wanted = (uint32_t)value.value32;
        if (wanted < OV5647_EXPOSURE_MIN)
          {
            wanted = OV5647_EXPOSURE_MIN;
          }
        else if (wanted > OV5647_MODE_VTS - 4u)
          {
            wanted = OV5647_MODE_VTS - 4u;
          }

        priv->exposure_lines = wanted;
        ret = ov5647_write_exposure(priv);
        break;

      case IMGSENSOR_ID_ISO_SENSITIVITY:
        wanted = (uint32_t)value.value32;
        if (wanted < OV5647_ANALOG_GAIN_MIN)
          {
            wanted = OV5647_ANALOG_GAIN_MIN;
          }
        else if (wanted > OV5647_ANALOG_GAIN_MAX)
          {
            wanted = OV5647_ANALOG_GAIN_MAX;
          }

        priv->analog_gain = wanted;
        ret = ov5647_write_gain(priv);
        break;

      case IMGSENSOR_ID_EXPOSURE_AUTO:
        /* Only the two modes the hardware has.
         *
         * Shutter priority and aperture priority are not things this part
         * can be asked for.  There is no iris, and the sensor's loop drives
         * exposure and gain together -- so a fixed exposure with a floating
         * gain is a policy a caller builds on top of manual mode, not a
         * mode that can be selected.  Accepting them would be claiming a
         * control that does not exist.
         */

        if ((uint32_t)value.value32 == V4L2_EXPOSURE_MANUAL)
          {
            auto_wanted = false;
          }
        else if ((uint32_t)value.value32 == V4L2_EXPOSURE_AUTO)
          {
            auto_wanted = true;
          }
        else
          {
            nxmutex_unlock(&priv->lock);
            return -ENOTSUP;
          }

        if (priv->aec_auto != auto_wanted)
          {
            priv->aec_auto = auto_wanted;
            ret = ov5647_write_manual_ctrl(priv);

            /* Handing the exposure back to this driver: the sensor is
             * holding whatever its loop last chose, so what the driver
             * believes in is written again rather than being left as
             * whatever that happened to be.
             */

            if (ret >= 0 && !auto_wanted)
              {
                ret = ov5647_write_exposure(priv);
              }
          }
        break;

      case IMGSENSOR_ID_ISO_SENSITIVITY_AUTO:
        /* Independent of the exposure control at the register level, and
         * paired with it in practice: the sensor runs the gain up only once
         * the exposure has run out, so an auto gain with a manual exposure
         * is a combination the hardware will honour but that leads
         * somewhere the caller may not expect.  It is allowed anyway, since
         * refusing it would be this driver inventing a rule the part does
         * not have.
         */

        if ((uint32_t)value.value32 == V4L2_ISO_SENSITIVITY_MANUAL)
          {
            auto_wanted = false;
          }
        else if ((uint32_t)value.value32 == V4L2_ISO_SENSITIVITY_AUTO)
          {
            auto_wanted = true;
          }
        else
          {
            nxmutex_unlock(&priv->lock);
            return -ENOTSUP;
          }

        if (priv->agc_auto != auto_wanted)
          {
            priv->agc_auto = auto_wanted;
            ret = ov5647_write_manual_ctrl(priv);

            if (ret >= 0 && !auto_wanted)
              {
                ret = ov5647_write_gain(priv);
              }
          }
        break;

      default:
        ret = -ENOTSUP;
        break;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ov5647_initialize
 ****************************************************************************/

int ov5647_initialize(FAR struct i2c_master_s *i2c)
{
  FAR struct ov5647_s *priv = &g_ov5647;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  priv->i2c = i2c;

  priv->sensor.ops = &g_ov5647_ops;
  priv->sensor.fmtdescs_num =
      sizeof(g_ov5647_fmtdescs) / sizeof(g_ov5647_fmtdescs[0]);
  priv->sensor.fmtdescs = g_ov5647_fmtdescs;
  priv->sensor.frmsizes_num =
      sizeof(g_ov5647_frmsizes) / sizeof(g_ov5647_frmsizes[0]);
  priv->sensor.frmsizes = g_ov5647_frmsizes;
  priv->sensor.frmintervals_num =
      sizeof(g_ov5647_frmintervals) / sizeof(g_ov5647_frmintervals[0]);
  priv->sensor.frmintervals = g_ov5647_frmintervals;

  priv->bound = true;

  return OK;
}

/****************************************************************************
 * Name: ov5647_sensor
 ****************************************************************************/

FAR struct imgsensor_s *ov5647_sensor(void)
{
  return g_ov5647.bound ? &g_ov5647.sensor : NULL;
}

#endif /* CONFIG_VIDEO_OV5647 */
