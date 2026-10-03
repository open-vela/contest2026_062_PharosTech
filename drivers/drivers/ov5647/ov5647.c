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

#include <assert.h>
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

#define OV5647_VTS_MANUAL           (1u << 2)
#define OV5647_AGC_MANUAL           (1u << 1)
#define OV5647_AEC_MANUAL           (1u << 0)
#define OV5647_HTS                  0x380c
#define OV5647_VTS                  0x380e
#define OV5647_TIMING_X_OUTPUT_SIZE 0x3808
#define OV5647_TIMING_Y_OUTPUT_SIZE 0x380a
#define OV5647_PLL_MULTIPLIER       0x3036
#define OV5647_FRAME_OFF_NUMBER     0x4202
#define OV5647_MIPI_CTRL00          0x4800
#define OV5647_MIPI_CTRL14          0x4814
#define OV5647_ISP_AWB              0x5001

/* The registers that decide the Bayer phase of the delivered frame.
 *
 * Which of the four positions in a 2x2 tile holds which colour follows from
 * where the readout window begins and from whether the readout mirrors or
 * flips it.  Both of those are parity questions: a window that starts on an
 * odd column -- or on an odd row -- moves green from one diagonal of the tile
 * to the other, and a reconstruction that expects it where it was no longer
 * finds it.  The picture loses its colour and nothing fails, which is what
 * makes these worth reading rather than reasoning about.
 *
 * None of them is written from the driver's state, so what comes back is what
 * the mode's own table left.  The two increments and the two subsampling
 * values are here for the same reason: a mode whose table omits a register
 * another mode writes is a mode running with the other mode's value, and that
 * is a mistake this pair of tables has already made once.
 */

#define OV5647_WINDOW_X_START 0x3800
#define OV5647_WINDOW_Y_START 0x3802
#define OV5647_ISP_X_OFFSET   0x3811
#define OV5647_ISP_Y_OFFSET   0x3813
#define OV5647_TIMING_X_INC   0x3814
#define OV5647_TIMING_Y_INC   0x3815
#define OV5647_SUBSAMPLE_0    0x3708
#define OV5647_SUBSAMPLE_1    0x3709
#define OV5647_FLIP           0x3820
#define OV5647_MIRROR         0x3821

#define OV5647_CHIP_ID        0x5647

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

/* The mode the driver leaves the sensor in when it is bound.
 *
 * The application chooses a mode for the stream it wants, so this only
 * decides what a caller that never asks for anything gets, and what the
 * sensor is left in between streams.  The check below turns a Kconfig index
 * that names no mode into a build error, rather than into a null pointer
 * dereference in the middle of the driver.
 */

#if CONFIG_OV5647_DEFAULT_MODE >= OV5647_NUM_MODES
#error \
    "CONFIG_OV5647_DEFAULT_MODE names no mode; see OV5647_MODE_* in ov5647.h"
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

#if defined(CONFIG_OV5647_EXPOSURE_LINES) && CONFIG_OV5647_EXPOSURE_LINES > 0
#define OV5647_EXPOSURE_INIT ((uint32_t)CONFIG_OV5647_EXPOSURE_LINES)
#else
/* Zero is not an exposure the sensor can be given -- see
 * OV5647_EXPOSURE_MIN -- so it serves as "the mode's own maximum", which is
 * what ov5647_initialize() resolves it to once a mode is in force.
 */

#define OV5647_EXPOSURE_INIT 0u
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

  /* The mode the sensor is programmed for.  Resolved from what the framework
   * asks for when the stream starts, because that is the first moment the
   * request is known: the application sets the format and the frame rate
   * after opening the device and before starting the stream, and the sensor
   * is not listened to until the stream starts.  Until then this is the mode
   * the driver was bound with.
   *
   * A mode is geometry, frame rate and link rate together, so this pointer
   * is what the exposure ceiling, the frame interval reported to the
   * framework and the register set written at stream start are all read
   * from.  It refers to read-only storage and is only ever reassigned.
   */

  FAR const struct ov5647_mode_s *mode;

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
 *
 * Both modes' register sets are compiled in.  Which one is written is
 * decided when the stream starts, from what the framework was asked for, so
 * that the sensor follows the application rather than the build; see
 * ov5647_start_capture().
 */

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

/* 1296x960 (2x2 binned) 10-bit mode: full field of view, no subsampling.
 * Copied from the upstream Linux driver's ov5647_2x2binned_10bpp table, with
 * one deliberate change: the output height (0x380a/0x380b) is trimmed from
 * 0x03cc (972) to 0x03c0 (960).  The binned array is 1296x972 and 972 is not
 * a multiple of sixteen, so the whole pipeline -- this mode's output, the
 * capture engine's stride, the encoder's macroblock grid and the
 * application's frame copy -- would each have to decide separately what to
 * do with the remainder.  Trimming in the sensor makes the answer the same
 * everywhere and costs twelve rows of 972, or 1.2%, off the bottom.  It is
 * also why the height is written here rather than taken from upstream:
 * upstream has no such constraint.
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
 * reason the VGA mode leaves them out.  HTS and VTS are not here either,
 * because they are what a mode's frame rate is: they are written from the
 * mode that ends up in force, by ov5647_write_timing() below.
 */

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

/* The modes, in the order OV5647_MODE_* names them.
 *
 * These are sensor facts: the register set and the pixel clock behind each
 * one, in the form everything downstream has to agree with.  The board's
 * D-PHY rate, the capture engine's geometry and Bayer order, and the
 * application's exposure ceiling are all read from here and from nowhere
 * else, which is what keeps the three in step.
 *
 * The figures are the ones the upstream Linux driver programs (see its
 * ov5647_modes[] table).  One relation is worth writing out because most of
 * the behaviour follows from it: a frame lasts HTS * VTS pixel clocks, so
 * 1296x960's 1896 * 1435 at 87.5 MHz is 32.2 fps -- advertised as 30, which
 * is what the pack can be relied on to carry -- and 640x480's 1852 * 504 at
 * 58.333 MHz is 62.5 fps, advertised as 60.
 *
 * VTS is also the exposure ceiling, because an exposure cannot outlast the
 * frame it is part of.  That is the whole difference between the two
 * 1296x960 entries: the second lengthens VTS and so lengthens the exposure
 * the sensor can be given, at the cost of frame rate.  They share a link
 * frequency, a Bayer order and a register set, so moving between them
 * changes nothing outside the sensor -- no D-PHY rate to settle, no capture
 * buffers to resize.  A scene that pins the gain at its ceiling in the
 * 30 fps entry has somewhere to go in the 22 fps one; whether that trade is
 * worth making is the application's to judge, which is why both are offered
 * rather than one being chosen here.
 *
 * ★ How far that second entry can go is bounded by the width of the VTS
 * field, and the bound is not obvious from the datasheet.
 *
 * 0x380e is described as "Bit[1:0]: Total vertical size[9:8]", which would
 * make VTS ten bits and cap a frame at 1023 lines.  Its own reset value says
 * otherwise: 0x07b0 is 1968, which does not fit in ten bits.  The
 * description is a slip and the field is [2:0], so VTS is eleven bits and
 * the longest frame these timings can describe is 2047 lines -- 1896 * 2047
 * at 87.5 MHz, or 44.4 ms, which is 22.5 frames a second.  Twenty-two is
 * what the pack can be relied on to carry, rounded down the same way 32.2 is
 * advertised as 30.
 *
 * A longer frame is not refused by the sensor, which is what makes this
 * worth writing down.  The part above 2047 is silently discarded: 2870
 * reaches the sensor as 822, a frame shorter than the 960 active lines it
 * has to hold.  The picture that comes out has broken colour and artifacts,
 * because a frame that cannot contain its own readout is not a frame.  This
 * entry was first written that way, to reach 15 fps, and that symptom is why
 * ov5647_start_capture() now reads the timing registers back and complains.
 *
 * So 22 fps is the floor at this HTS, and the exposure ceiling that comes
 * with it is 2043 lines -- 1.43 times the 30 fps entry's.  Anything slower
 * needs a slower pixel clock, which is a different link rate and therefore
 * no longer a change that stays inside the sensor.
 *
 * A D-PHY link is double data rate, so the rate the receiver is configured
 * for per lane is twice the link frequency.  The two 1296x960 entries leave
 * it at 218.75 MHz; 640x480 runs its link at 145.833 MHz, which is the one
 * figure a mode change cannot make cheap.
 *
 * The Bayer order is per mode because a mode's readout can mirror or flip
 * the sensor's native tile -- both of these do, see the register tables --
 * and the order in the delivered buffer is what the capture engine has to be
 * told.  It is stated here rather than derived at the point of use so that
 * the one place that knows about mirroring is the place that also knows the
 * answer.
 */

static const struct ov5647_mode_s g_ov5647_modes[OV5647_NUM_MODES] = {
  {
      .width = 1296,
      .height = 960,
      .fps = 30,
      .hts = 1896u,
      .vts = 1435u,
      .pll = 0x69,
      .link_freq = 218750000u,
      .bayer = OV5647_BAYER_GBRG,
  },
  {
      .width = 1296,
      .height = 960,
      .fps = 22,
      .hts = 1896u,
      .vts = 2047u,
      .pll = 0x69,
      .link_freq = 218750000u,
      .bayer = OV5647_BAYER_GBRG,
  },
  {
      .width = 640,
      .height = 480,
      .fps = 60,
      .hts = 1852u,
      .vts = 504u,
      .pll = 0x46,
      .link_freq = 145833300u,
      .bayer = OV5647_BAYER_GBRG,
  },
};

/* OV5647_NUM_MODES is what ov5647_mode() bounds its indices with, and it
 * lives in the public header where the board and the application read it.
 * The table is here, so the two are tied together at compile time rather
 * than by a comment.
 */

static_assert(sizeof(g_ov5647_modes) / sizeof(g_ov5647_modes[0]) ==
                  OV5647_NUM_MODES,
              "OV5647_NUM_MODES must match the mode table");

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

/* The geometries, each listed once.  The two 1296x960 modes share one, so
 * they share an entry: this is the frame size the sensor can produce, and a
 * frame rate is not part of it.
 */

static struct v4l2_frmsizeenum g_ov5647_frmsizes[] =
{
  {
    .index  = 0,
    .type   = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width  = 1296,
        .height = 960,
      },
  },
  {
    .index  = 1,
    .type   = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
      {
        .width  = 640,
        .height = 480,
      },
  },
};

/* The frame rates, one per entry in the mode table and in the same order.
 *
 * The framework hands these out as a flat list without filtering by frame
 * size -- its VIDIOC_ENUM_FRAMEINTERVALS ignores the width and height that
 * come with the request -- so what is here is the union over all the modes
 * rather than a per-size list.  Which of them a particular stream runs at is
 * settled by the frame size and this rate together, in
 * ov5647_start_capture().
 *
 * The first entry is the rate the framework starts a capture stream at, so
 * it has to be one the driver accepts; that is why this list is written in
 * the mode table's order rather than sorted.
 */

static struct v4l2_frmivalenum g_ov5647_frmintervals[] =
{
  {
    .index     = 0,
    .type      = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete =
      {
        .numerator   = 1,
        .denominator = 30,
      },
  },
  {
    .index     = 1,
    .type      = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete =
      {
        .numerator   = 1,
        .denominator = 22,
      },
  },
  {
    .index     = 2,
    .type      = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete =
      {
        .numerator   = 1,
        .denominator = 60,
      },
  },
};

static struct ov5647_s g_ov5647 = {
  .lock = NXMUTEX_INITIALIZER,
  .addr = CONFIG_OV5647_I2C_ADDR,
  .mode = &g_ov5647_modes[CONFIG_OV5647_DEFAULT_MODE],
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
static int ov5647_write_timing(FAR struct ov5647_s *priv);
static int ov5647_write_mode_regs(FAR struct ov5647_s *priv);
static int ov5647_program(FAR struct ov5647_s *priv);
static void ov5647_clamp_exposure(FAR struct ov5647_s *priv);
static FAR const struct ov5647_mode_s *
ov5647_resolve_mode(uint16_t width, uint16_t height, uint32_t fps);
static uint32_t ov5647_interval_fps(FAR const imgsensor_interval_t *interval);
static bool ov5647_rate_supported(uint32_t fps);

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

/****************************************************************************
 * Name: ov5647_program
 *
 * Description:
 *   Configure the sensor for the mode in force, from scratch.  The caller
 *   must hold the driver lock.
 *
 *   ★ Why this is the whole configuration and not just the mode's own
 *   register set.
 *
 *   The two mode tables are not supersets of each other.  Each writes
 *   registers the other does not, and those registers are not written again
 *   by anything else:
 *
 *     1296x960 writes 0x3811/0x3813 (ISP output offsets), 0x4837 and 0x5002;
 *     640x480  writes 0x3708/0x3709 (subsampling), and neither the other's.
 *
 *   Writing only the incoming mode's table therefore leaves the outgoing
 *   mode's private registers in place, and the sensor ends up in a state
 *   neither table describes.  That is the whole failure mode:
 *
 *     - 1296x960 then 640x480: the ISP offsets stay at the binned mode's
 *       values, and the picture comes out displaced and garbled -- every
 *       time, because the register state is the same every time.
 *
 *     - 640x480 then 1296x960: the subsampling stages stay enabled in a mode
 *       that must not subsample.  Whether it shows depends on what the mode
 *       was before, which is why this one looks intermittent.
 *
 *     - and if what is left behind is the PLL multiplier (0x3036), the
 *       sensor emits no clock, the D-PHY never reports its lanes ready, and
 *       the receive chain spends its whole timeout failing to come up.  That
 *       one is also intermittent, and it is the one that makes a switch take
 *       twenty seconds instead of a fraction of a second.
 *
 *   The common table starts with software standby and then a software reset,
 *   which returns every register to its reset value.  Writing it before the
 *   mode's own table is therefore what makes a stream start equivalent to a
 *   cold start: nothing survives from the mode that was running before, and
 *   the only registers that differ afterwards are the ones a table states.
 *
 *   The cost is the reset's settling time and about eighty register writes,
 *   once per stream.  That is paid at stream start, not per frame, and it buys
 *   the property that a mode change cannot be affected by its history -- which
 *   is what the alternative, writing only what changed, would have to be
 *   proven correct about for every pair of modes.
 *
 ****************************************************************************/

static int ov5647_program(FAR struct ov5647_s *priv)
{
  int ret;

  /* 1. The sensor-wide configuration, which begins by putting the sensor in
   *    software standby and resetting it.  The mode's registers below are
   *    written on top of a known state because of those two writes, and the
   *    standby is the state the mode table expects to be written in.
   */

  ret = ov5647_putregs(priv, g_ov5647_common_regs,
                       sizeof(g_ov5647_common_regs) /
                           sizeof(g_ov5647_common_regs[0]));
  if (ret < 0)
    {
      return ret;
    }

  /* A software reset needs settling time before the rest is programmed on
   * top of it.
   */

  usleep(OV5647_RESET_DELAY_US);

  /* 2. The mode's own register set, which ends by taking the sensor out of
   *    software standby.
   */

  ret = ov5647_write_mode_regs(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* 3. Virtual channel 0: the driver reports everything on channel 0, and
   *    the SoC side filters for exactly that.
   */

  ret = ov5647_putreg(priv, OV5647_MIPI_CTRL14, 0x00);
  if (ret < 0)
    {
      return ret;
    }

  /* 4. Leave the sensor's own white balance block switched off.
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

  return ov5647_putreg(priv, OV5647_ISP_AWB, 0x00);
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

  /* The mode the driver was bound with, which is not necessarily the mode the
   * first stream will run at: the application states what it wants after
   * opening the device, and ov5647_start_capture() programs that.  What this
   * is for is leaving the sensor in a configuration a receiver can make sense
   * of in the meantime, rather than in the reset default, which is the whole
   * 2592x1944 array.
   */

  ret = ov5647_program(priv);
  if (ret < 0)
    {
      goto out;
    }

  _info("OV5647: initialised, %ux%u @ %u fps RAW10, 2 lanes\n",
        (unsigned int)priv->mode->width, (unsigned int)priv->mode->height,
        (unsigned int)priv->mode->fps);

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
 * Name: ov5647_resolve_mode
 *
 * Description:
 *   Turn a frame size and a frame rate into one of the modes, if the sensor
 *   can produce that size at all.
 *
 *   The frame size has to match a mode exactly: the sensor has no scaler, so
 *   a size it was not programmed for is not one it can deliver.
 *
 *   The frame rate is a preference among the modes that share that size,
 *   not a requirement, and the distinction is deliberate.  The framework
 *   validates a format and a frame rate with one call while holding the two
 *   from different moments -- a format set now is checked against the rate in
 *   force, and a rate set now against the format in force -- so a change from
 *   one mode to another always produces at least one pairing that is not a
 *   mode.  Refusing those would leave no way to change modes, and guessing
 *   silently would hide a request that could not be met.  So the size decides,
 *   the rate chooses among the candidates, and a rate that matches none is
 *   reported by the caller rather than smoothed over.
 *
 * Input Parameters:
 *   width  - Requested width, in pixels.
 *   height - Requested height, in pixels.
 *   fps    - Requested frame rate, or 0 for "whatever this size runs at".
 *
 * Returned Value:
 *   The mode, or NULL if no mode has that frame size.
 *
 ****************************************************************************/

static FAR const struct ov5647_mode_s *
ov5647_resolve_mode(uint16_t width, uint16_t height, uint32_t fps)
{
  FAR const struct ov5647_mode_s *nearest = NULL;
  uint32_t nearest_delta = 0;
  unsigned int i;

  for (i = 0; i < OV5647_NUM_MODES; i++)
    {
      FAR const struct ov5647_mode_s *mode = &g_ov5647_modes[i];
      uint32_t delta;

      if (mode->width != width || mode->height != height)
        {
          continue;
        }

      if (fps == 0u || (uint32_t)mode->fps == fps)
        {
          return mode;
        }

      delta = (uint32_t)mode->fps > fps ? (uint32_t)mode->fps - fps
                                        : fps - (uint32_t)mode->fps;

      if (nearest == NULL || delta < nearest_delta)
        {
          nearest = mode;
          nearest_delta = delta;
        }
    }

  return nearest;
}

/****************************************************************************
 * Name: ov5647_interval_fps / ov5647_rate_supported
 *
 * Description:
 *   The frame rate an interval asks for, and whether any mode runs at it.
 *   A zero numerator or denominator is the framework's way of saying the
 *   rate is not part of the request, and comes back as 0.
 *
 *   The rates are the modes', not a free choice: the frame length is written
 *   by this driver and never by the sensor's own loop, so a rate that no mode
 *   states is one the sensor cannot be given.
 *
 ****************************************************************************/

static uint32_t ov5647_interval_fps(FAR const imgsensor_interval_t *interval)
{
  if (interval == NULL || interval->numerator == 0u ||
      interval->denominator == 0u)
    {
      return 0u;
    }

  return interval->denominator / interval->numerator;
}

static bool ov5647_rate_supported(uint32_t fps)
{
  unsigned int i;

  for (i = 0; i < OV5647_NUM_MODES; i++)
    {
      if ((uint32_t)g_ov5647_modes[i].fps == fps)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: ov5647_clamp_exposure
 *
 * Description:
 *   Keep the stored exposure inside the mode that is in force.  The caller
 *   must hold the driver lock.
 *
 *   An exposure cannot outlast the frame it is part of.  A line count that
 *   was legal in the previous mode may not be in this one -- the mode with
 *   the larger vertical total exists precisely to allow longer exposures --
 *   and a sensor told to integrate for longer than a frame truncates, with
 *   nothing to say so.  Clamping here is what keeps the value reported
 *   through the control interface equal to the one in the registers.
 *
 ****************************************************************************/

static void ov5647_clamp_exposure(FAR struct ov5647_s *priv)
{
  uint32_t max = OV5647_MODE_EXPOSURE_MAX(priv->mode);

  if (priv->exposure_lines < OV5647_EXPOSURE_MIN)
    {
      priv->exposure_lines = OV5647_EXPOSURE_MIN;
    }
  else if (priv->exposure_lines > max)
    {
      priv->exposure_lines = max;
    }
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
  uint32_t fps = ov5647_interval_fps(interval);
  int i;

  if (datafmts == NULL || nr_datafmts == 0)
    {
      return -EINVAL;
    }

  /* The sensor has no scaler and no format choice: it delivers the size it
   * was programmed for, and the SoC side demosaics to a fixed format.
   * Everything the framework can ask for is therefore either something one of
   * the modes produces or impossible.
   *
   * That the two are checked against the modes separately, rather than as a
   * pair, is the point of this function rather than an oversight in it.  It
   * is reached from VIDIOC_S_FMT with whatever frame rate is in force and
   * from VIDIOC_S_PARM with whatever format is in force, so during a change
   * of mode one of the two calls always arrives carrying the other's previous
   * value.  Requiring the pair to name a mode would reject that call, and the
   * first of the two could never be accepted -- which is to say, no mode
   * change would be possible at all.  Which mode the pair eventually names is
   * settled once, when the stream starts; see ov5647_start_capture().
   */

  for (i = 0; i < nr_datafmts; i++)
    {
      if (datafmts[i].pixelformat != IMGSENSOR_PIX_FMT_NV12 &&
          datafmts[i].pixelformat != IMGSENSOR_PIX_FMT_YUV420P)
        {
          return -EINVAL;
        }

      if (ov5647_resolve_mode(datafmts[i].width, datafmts[i].height, 0u) ==
          NULL)
        {
          return -EINVAL;
        }
    }

  if (fps != 0u && !ov5647_rate_supported(fps))
    {
      return -EINVAL;
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
 * Name: ov5647_write_timing
 *
 * Description:
 *   Write the frame length of the mode in force.  The caller must hold the
 *   driver lock.
 *
 *   HTS and VTS are where the frame rate comes from -- a frame lasts
 *   HTS * VTS pixel clocks -- so they are written from the mode rather than
 *   from a table fixed at build time.  That is what makes a change of frame
 *   rate cheap: the clock, the link and the readout window all stay exactly
 *   as they were, and only the number of lines in a frame moves.
 *
 *   VTS is also the exposure ceiling, which is why the two travel together
 *   in the mode rather than one of them being derived at the point of use.
 *
 *   The multi-byte registers are big-endian, which is why each value appears
 *   one byte per entry.
 *
 ****************************************************************************/

static int ov5647_write_timing(FAR struct ov5647_s *priv)
{
  struct ov5647_reg_s regs[4] = {
    { OV5647_HTS, (priv->mode->hts >> 8) & 0xffu },
    { OV5647_HTS + 1, priv->mode->hts & 0xffu },
    { OV5647_VTS, (priv->mode->vts >> 8) & 0xffu },
    { OV5647_VTS + 1, priv->mode->vts & 0xffu },
  };

  return ov5647_putregs(priv, regs, 4);
}

/****************************************************************************
 * Name: ov5647_write_mode_regs
 *
 * Description:
 *   Program the register set of the mode in force.  The caller must hold the
 *   driver lock.
 *
 *   There are two sets and the mode's geometry says which applies: the 2x2
 *   binned mode needs no subsampling registers because the bin alone reaches
 *   its size, and the 2x2 binned and subsampled mode does.  A mode that
 *   names neither is one this driver has no registers for, which is worth an
 *   error rather than a guess -- the sensor would otherwise be left in
 *   whatever the previous mode set up, streaming a geometry nobody asked for.
 *
 *   The set ends by taking the sensor out of software standby, so the caller
 *   has to have put it in.  These registers include the PLL multiplier and
 *   the readout window; a sensor still streaming while they change drives the
 *   link at the old rate with the new geometry for as long as the writes take.
 *
 ****************************************************************************/

static int ov5647_write_mode_regs(FAR struct ov5647_s *priv)
{
  FAR const struct ov5647_reg_s *regs;
  unsigned int num;

  if (priv->mode->width == 640u && priv->mode->height == 480u)
    {
      regs = g_ov5647_640x480_regs;
      num = sizeof(g_ov5647_640x480_regs) / sizeof(g_ov5647_640x480_regs[0]);
    }
  else if (priv->mode->width == 1296u && priv->mode->height == 960u)
    {
      regs = g_ov5647_1296x960_regs;
      num = sizeof(g_ov5647_1296x960_regs) / sizeof(g_ov5647_1296x960_regs[0]);
    }
  else
    {
      _err("ERROR: OV5647 has no register set for %ux%u\n", priv->mode->width,
           priv->mode->height);
      return -EINVAL;
    }

  return ov5647_putregs(priv, regs, num);
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
  FAR const struct ov5647_mode_s *mode;
  uint32_t fps;
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

  if (datafmts == NULL || nr_datafmts == 0u)
    {
      nxmutex_unlock(&priv->lock);
      return -EINVAL;
    }

  /* Which mode this stream runs at, from the frame size and the frame rate
   * the application set.
   *
   * This is the first moment the two are known to belong to the same
   * request: the framework validates a format and a frame rate apart from
   * each other, so a pairing that names no mode is normal on the way here --
   * see ov5647_validate_frame_setting().  Resolving once, at this point, is
   * what lets the two be set in either order and still arrive at a mode.
   *
   * A rate that matches no mode is not refused.  It cannot be honoured, so
   * something has to be chosen, and taking the nearest rate for the size the
   * application asked for gives up only the part that was impossible while
   * keeping the shape of the picture -- which is the part a person watching
   * can see.  It is said out loud, because a frame rate that has quietly
   * followed something other than the request is exactly the kind of thing
   * that turns into a mystery later.
   */

  fps = ov5647_interval_fps(interval);
  mode = ov5647_resolve_mode(datafmts[0].width, datafmts[0].height, fps);
  if (mode == NULL)
    {
      _err("ERROR: OV5647 cannot produce %ux%u\n", datafmts[0].width,
           datafmts[0].height);
      nxmutex_unlock(&priv->lock);
      return -EINVAL;
    }

  if (fps != 0u && (uint32_t)mode->fps != fps)
    {
      _warn("WARNING: OV5647 %ux%u cannot run at %u fps; using %u fps\n",
            (unsigned int)datafmts[0].width, (unsigned int)datafmts[0].height,
            (unsigned int)fps, (unsigned int)mode->fps);
    }

  priv->mode = mode;

  /* The sensor is configured from scratch, not patched.
   *
   * This is the point at which the request is known -- the application states
   * the format and the frame rate after opening the device -- and it is also
   * the point at which the previous stream stopped, so it is the point at
   * which the configuration has to become the new mode's and nothing else's.
   * ov5647_program() is what makes that true regardless of what ran before;
   * see the note there for what happens when only the incoming mode's own
   * registers are written.
   *
   * The frame length and the exposure/gain pair follow, in that order,
   * because the exposure is bounded by the mode and the pair is written from
   * the driver's own state rather than from either table.
   */

  ret = ov5647_program(priv);
  if (ret < 0)
    {
      goto out;
    }

  ret = ov5647_write_timing(priv);
  if (ret < 0)
    {
      goto out;
    }

  /* The exposure and gain come from the driver's state rather than from the
   * tables above, so that a value set through the control interface is what
   * the next stream actually uses.  The exposure is clamped to the mode first:
   * it was stored while some mode was in force, and this may not be that one.
   */

  ov5647_clamp_exposure(priv);

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
   *
   * The mode is printed with them because this line is also the one evidence
   * that a change of mode took effect: the HTS and VTS that come back are the
   * frame length the sensor is really running, and the pair of them is what
   * names the mode.
   */

  {
    uint8_t exposure[3] = { 0, 0, 0 };
    uint8_t gain[2] = { 0, 0 };
    uint8_t hts[2] = { 0, 0 };
    uint8_t vts[2] = { 0, 0 };
    uint8_t outw[2] = { 0, 0 };
    uint8_t outh[2] = { 0, 0 };
    uint8_t pll = 0;
    uint8_t aec_agc = 0;
    uint8_t mipi = 0;
    uint32_t got_hts;
    uint32_t got_vts;
    uint32_t got_w;
    uint32_t got_h;

    ov5647_getreg(priv, OV5647_EXPOSURE, &exposure[0]);
    ov5647_getreg(priv, OV5647_EXPOSURE + 1, &exposure[1]);
    ov5647_getreg(priv, OV5647_EXPOSURE + 2, &exposure[2]);
    ov5647_getreg(priv, OV5647_ANALOG_GAIN, &gain[0]);
    ov5647_getreg(priv, OV5647_ANALOG_GAIN + 1, &gain[1]);
    ov5647_getreg(priv, OV5647_HTS, &hts[0]);
    ov5647_getreg(priv, OV5647_HTS + 1, &hts[1]);
    ov5647_getreg(priv, OV5647_VTS, &vts[0]);
    ov5647_getreg(priv, OV5647_VTS + 1, &vts[1]);
    ov5647_getreg(priv, OV5647_TIMING_X_OUTPUT_SIZE, &outw[0]);
    ov5647_getreg(priv, OV5647_TIMING_X_OUTPUT_SIZE + 1, &outw[1]);
    ov5647_getreg(priv, OV5647_TIMING_Y_OUTPUT_SIZE, &outh[0]);
    ov5647_getreg(priv, OV5647_TIMING_Y_OUTPUT_SIZE + 1, &outh[1]);
    ov5647_getreg(priv, OV5647_PLL_MULTIPLIER, &pll);
    ov5647_getreg(priv, OV5647_AEC_AGC, &aec_agc);
    ov5647_getreg(priv, OV5647_MIPI_CTRL00, &mipi);

    got_hts = ((uint32_t)hts[0] << 8) | hts[1];
    got_vts = ((uint32_t)vts[0] << 8) | vts[1];
    got_w = ((uint32_t)outw[0] << 8) | outw[1];
    got_h = ((uint32_t)outh[0] << 8) | outh[1];

    _info("OV5647: streaming %ux%u @ %u fps, readback exposure"
          " 0x%02x%02x%02x gain 0x%02x%02x hts %u vts %u out %ux%u pll 0x%02x"
          " aec_agc 0x%02x mipi_ctrl00 0x%02x\n",
          (unsigned int)priv->mode->width, (unsigned int)priv->mode->height,
          (unsigned int)priv->mode->fps, exposure[0], exposure[1], exposure[2],
          gain[0], gain[1], got_hts, got_vts, got_w, got_h, pll, aec_agc,
          mipi);

    /* Read back the registers that place the tile.
     *
     * The timing above says when a frame happens; these say where the tile
     * sits inside it.  A mode change that leaves one of them at the previous
     * mode's value -- or at a value whose parity the mode's table did not
     * mean -- produces a picture that is off-colour with no failure anywhere,
     * so the values are printed rather than assumed, and the two that are
     * parity questions are checked.
     */

    {
      uint8_t winx = 0;
      uint8_t winy = 0;
      uint8_t offx = 0;
      uint8_t offy = 0;
      uint8_t incx = 0;
      uint8_t incy = 0;
      uint8_t flip = 0;
      uint8_t mirror = 0;
      uint8_t ss0 = 0;
      uint8_t ss1 = 0;

      ov5647_getreg(priv, OV5647_WINDOW_X_START + 1, &winx);
      ov5647_getreg(priv, OV5647_WINDOW_Y_START + 1, &winy);
      ov5647_getreg(priv, OV5647_ISP_X_OFFSET, &offx);
      ov5647_getreg(priv, OV5647_ISP_Y_OFFSET, &offy);
      ov5647_getreg(priv, OV5647_TIMING_X_INC, &incx);
      ov5647_getreg(priv, OV5647_TIMING_Y_INC, &incy);
      ov5647_getreg(priv, OV5647_FLIP, &flip);
      ov5647_getreg(priv, OV5647_MIRROR, &mirror);
      ov5647_getreg(priv, OV5647_SUBSAMPLE_0, &ss0);
      ov5647_getreg(priv, OV5647_SUBSAMPLE_1, &ss1);

      _info("OV5647: phase: window x lo %u y lo %u, isp offset x %u y %u,"
            " inc x 0x%02x y 0x%02x, flip 0x%02x mirror 0x%02x,"
            " subsample 0x%02x 0x%02x\n",
            winx, winy, offx, offy, incx, incy, flip, mirror, ss0, ss1);

      /* An odd window start is the one form of this that can be called out
       * exactly: the low byte of the start address is the window's own
       * column or row, so its parity is the parity of the tile.
       */

      if ((winx & 1u) != 0u)
        {
          _err("ERROR: OV5647 readout window starts on an odd column (x start"
               " low byte %u).  That swaps the two columns of the Bayer tile,"
               " so green lands on the other diagonal and the order"
               " configured downstream no longer describes the frame.\n",
               winx);
        }

      if ((winy & 1u) != 0u)
        {
          _err("ERROR: OV5647 readout window starts on an odd row (y start"
               " low byte %u).  That swaps the two rows of the Bayer tile, so"
               " green lands on the other diagonal and the order configured"
               " downstream no longer describes the frame.\n",
               winy);
        }
    }

    /* And check that what came back describes the mode that was asked for.
     *
     * The registers that carry a mode are narrower than the values a mode may
     * want to state, they are not all written by every mode's table, and none
     * of them is refused when the value does not fit.  The result is a sensor
     * that is not the sensor the rest of the path was configured for, and
     * every downstream figure -- the frame rate, the exposure ceiling, the
     * D-PHY's timing band, the geometry the capture engine waits for -- is
     * then computed from something that is not true.
     *
     * Four registers are checked, and each catches a different way of being
     * wrong:
     *
     *   - 0x3036, the PLL multiplier, sets the pixel clock and so the link
     *     rate.  Wrong, and the receiver is listening at a rate nothing is
     *     sending at -- which shows up as a failed bring-up, not a wrong
     *     picture, because the D-PHY's lanes never come ready.
     *
     *   - the output size is what the capture engine has been told to expect.
     *     Wrong, and the frame is cut or padded, which is the picture coming
     *     out displaced or torn rather than merely off-colour.
     *
     *   - HTS and VTS are the frame length, and VTS is also the exposure
     *     ceiling.  VTS above 2047 wraps, because 0x380e carries only three of
     *     its bits; that is the case this driver was first written to miss.
     *
     * The readback above is therefore not only a log line.
     */

    if ((((uint32_t)pll) != (uint32_t)priv->mode->pll) ||
        got_w != priv->mode->width || got_h != priv->mode->height ||
        got_hts != priv->mode->hts || got_vts != priv->mode->vts)
      {
        _err("ERROR: OV5647 is not configured for %ux%u @ %u fps.  Asked for"
             " pll 0x%02x hts %u vts %u out %ux%u, read back pll 0x%02x hts %u"
             " vts %u out %ux%u.  The frame rate, the exposure ceiling and the"
             " link rate are all wrong until this is fixed.  VTS is an eleven"
             " bit field (0x380e[2:0] with 0x380f), so anything above 2047"
             " wraps; a mode whose table omits a register another mode writes"
             " can also be the cause.\n",
             (unsigned int)priv->mode->width, (unsigned int)priv->mode->height,
             (unsigned int)priv->mode->fps, (unsigned int)priv->mode->pll,
             (unsigned int)priv->mode->hts, (unsigned int)priv->mode->vts,
             (unsigned int)priv->mode->width, (unsigned int)priv->mode->height,
             (unsigned int)pll, got_hts, got_vts, got_w, got_h);
      }
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
  FAR struct ov5647_s *priv = (FAR struct ov5647_s *)sensor;

  if (interval == NULL)
    {
      return -EINVAL;
    }

  /* The mode in force, which is the one the last stream was started with.
   * The framework only asks while a stream is running, and the mode is
   * settled before it starts, so this is the rate the frames are really
   * arriving at rather than a table entry that may not apply.
   */

  interval->numerator = 1;
  interval->denominator = priv->mode->fps;

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

        priv->exposure_lines = (uint32_t)value.value32;
        ov5647_clamp_exposure(priv);
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
 * Name: ov5647_mode
 ****************************************************************************/

FAR const struct ov5647_mode_s *ov5647_mode(unsigned int index)
{
  return index < OV5647_NUM_MODES ? &g_ov5647_modes[index] : NULL;
}

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

  /* The exposure the driver starts from.
   *
   * Zero is what CONFIG_OV5647_EXPOSURE_LINES means by "unset", and resolves
   * to the mode's own maximum -- which is the right default for a bring-up
   * where the light is unknown.  A configured value is clamped to the same
   * ceiling rather than trusted, because a line count that is legal in one
   * mode is not necessarily legal in another, and this setting is stated
   * without reference to any particular mode.
   *
   * The gain needs no resolving: its range is the part's rather than a
   * mode's.
   */

  priv->exposure_lines = OV5647_EXPOSURE_INIT;
  if (priv->exposure_lines == 0u)
    {
      priv->exposure_lines = OV5647_MODE_EXPOSURE_MAX(priv->mode);
    }

  ov5647_clamp_exposure(priv);

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
