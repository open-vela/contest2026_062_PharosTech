/****************************************************************************
 * app/camenc/camenc_main.c
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
 * Camera to H.264, through the hardware encoder, to a file.
 *
 * The chain is two devices and a muxer:
 *
 *   /dev/video0   the camera, which hands over NV12 frames
 *   /dev/video1   the encoder, which takes NV12 on its output side and
 *                 returns an annex-B H.264 access unit on its capture side
 *   camenc_stream turns those access units into fragmented MP4
 *
 * Each frame goes round a loop: take one from the camera, copy it into the
 * encoder's output buffer, queue that -- which encodes it, because the
 * encoder does its work inside the ioctl and not on a work queue -- then take
 * the bitstream from the encoder's capture side, mux it, and hand both
 * buffers back.
 *
 * Two things about this loop are not obvious and are the reason for most of
 * the comments below.
 *
 * The two devices disagree about how a buffer is named when it is queued.
 * The capture device finds the buffer from `index`, and that is all an
 * application has to set.  The encoder finds it from `m.offset`, which is the
 * value VIDIOC_QUERYBUF reported, and it ignores `index` entirely -- so a
 * queue operation that carries only an index gives it an address formed from
 * zero.  The file keeps the queried descriptor and queues that, which is what
 * ffmpeg's v4l2m2m backend does and what the encoder expects.
 *
 * The frame is copied rather than passed through.  The encoder's buffers
 * belong to the encoder, and it validates that what it is given is one of
 * them, so a camera buffer cannot be handed to it directly; the copy costs a
 * few hundred microseconds a frame, which at this size is not worth trading
 * the check away for.
 *
 * Nothing here is specific to a file: the muxer calls a write callback, and
 * where that writes is the only thing to change to have it feed a socket
 * instead.  See camenc_stream.h.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <sys/videoio.h>

#include "cam3a.h"
#include "camenc_3a.h"
#include "camenc_stream.h"
#include "camenc_ws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMENC_CAM_DEVPATH "/dev/video0"
#define CAMENC_ENC_DEVPATH "/dev/video1"

/* The sensor's limits, which the exposure loop needs in order to know where
 * one of the two ways of getting more light runs out and the other has to
 * take over.  These are the OV5647's: exposure in lines, bounded by the
 * mode's frame length; gain a ten-bit fixed-point multiplier whose unity is
 * sixteen.
 *
 * Named here rather than asked for, because the sensor reports neither.  The
 * exposure ceiling is the mode's VTS less a few lines; the driver clamps to
 * the same figure, so a loop that asks for more is corrected rather than
 * believed.
 */

#define CAMENC_3A_EXPOSURE_MIN 4u
#define CAMENC_3A_EXPOSURE_MAX 500u /* VTS - 4 in the 640x480 mode */
#define CAMENC_3A_GAIN_MIN     16u
#define CAMENC_3A_GAIN_MAX     1023u
#define CAMENC_3A_GAIN_ONE     16u

/* The brightness the loop steers to, on the 0..255 scale the capture driver
 * measures in.  About half of the range leaves room to be wrong in both
 * directions; a target near the top spends its time clipped.
 */

#define CAMENC_3A_TARGET_DEFAULT 110u

/* Whether the loop runs when the caller does not say.
 *
 * On, because a camera whose exposure and gain have to be chosen by hand is
 * not much use on a scene that changes -- which is the whole reason the loop
 * exists.  A caller that wants fixed settings still has them: -A 0 leaves the
 * exposure and gain exactly where they were put.  On a board whose capture
 * driver has no 3A control plane the flag has nothing to act on, and the
 * absence is reported once rather than every frame.
 */

#define CAMENC_3A_DEFAULT 1

/* How much of the remaining error the two loops take per frame, as a shift,
 * and how close to the target counts as arrived.
 *
 * A quarter of the error each frame is a compromise: it settles a step change
 * in about ten frames, which at this frame rate is under a fifth of a second,
 * and it does not visibly hunt on the noise between two frames of the same
 * scene.  See camenc_3a.h.
 */

#define CAMENC_3A_AE_SHIFT  2u
#define CAMENC_3A_AWB_SHIFT 2u
#define CAMENC_3A_SETTLE    4u

/* The least signal a channel can carry and still be worth steering the white
 * balance by, as a channel mean on the 0..255 scale.
 *
 * Twelve counts is about 5% of full scale.  Below that a channel's mean is
 * dominated by sensor noise and by any black-level mismatch between the
 * channels, so the ratios between the channels stop describing the colour of
 * the light and start describing the noise.  See camenc_3a.h.
 */

#define CAMENC_3A_AWB_MIN_MEAN 12u

#define CAMENC_DEFAULT_WIDTH   640
#define CAMENC_DEFAULT_HEIGHT  480
#define CAMENC_DEFAULT_FRAMES  300
#define CAMENC_DEFAULT_QP      26

/* The encoder's default group length, and this program keeps it.
 *
 * Two pictures per group is where the saving is: 1454 kbit/s to 747 on the
 * capture it was measured from, and a longer group adds 5.6% on top of that,
 * because one P in every group of four costs as much as the IDR that opened
 * the group.  chips/rk3576/vepu/README.md has the numbers.
 *
 * A longer group also trades away a decoder's ability to join the stream late
 * and to recover from a lost fragment, and which of those matters more is a
 * property of how the stream is being watched rather than of the encoder --
 * so it is the caller's question, not this program's default.
 */

#define CAMENC_DEFAULT_GOP 2

/* Three, as the capture tool uses: enough that a frame being worked on does
 * not stall the one arriving, few enough that the latency stays short.
 */

#define CAMENC_BUFFERS 3

/* The encoder's private controls.  v4l2_m2m.c carries control ids in a
 * uint16_t, so the standard V4L2_CID_MPEG_* range is unreachable and the
 * driver numbers its own from 0x1000; the numbers are in
 * chips/rk3576/vepu/rk3576_vepu_codec.h and an application is expected to be
 * told them, because the framework's control enumeration is not wired up.
 * Only the QP is used here; the rest have usable defaults.
 */

#define CAMENC_CID_QP  0x1000
#define CAMENC_CID_GOP 0x1004

/* The largest segment the streaming server will carry.
 *
 * A segment is copied whole into each client's transmit buffer, so this is
 * what one client costs in memory, and a segment larger than it drops that
 * client rather than stalling the capture loop.  The encoder advertises its
 * capture buffer as a frame's size, which is many times what a frame at this
 * resolution occupies -- around three kilobytes at the quantiser below -- so
 * sizing the server for the advertised figure would cost megabytes for
 * nothing.  The limit is logged when it is reached, which is what keeps it
 * from being a silent ceiling.
 */

#define CAMENC_WS_MAX_SEGMENT (64 * 1024)

#define CAMENC_DEFAULT_PORT   8080

/* How often the loop says anything at all, in frames.  A hundred is about
 * a second and a half at the rate this camera runs, which is often enough
 * to see that it is alive and rare enough to see anything else.
 */

#define CAMENC_REPORT_FRAMES 100

/* Room for a status line and its terminator.  The line is a little over a
 * hundred bytes today; the room is here so that adding a field to it is not
 * a change to two buffers.
 */

#define CAMENC_STATUS_MAX 224

/* How far apart status lines may be when nothing has changed.
 *
 * The line goes out whenever it differs from the one before, which on a
 * moving scene is often and on a settled one is never.  A client that
 * connects while the loop is settled has never been told anything, so one is
 * sent anyway at this interval -- a second or so at this frame rate, which is
 * nothing on the wire and is what makes the page's sliders sit where the
 * hardware is.
 */

#define CAMENC_STATUS_FRAMES 64

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct camenc_buf_s
{
  FAR uint8_t *start;
  size_t length;

  /* The descriptor QUERYBUF returned, kept whole.  The encoder's queue
   * operation needs the offset out of it, so it is not enough to remember
   * the mapped pointer.
   */

  struct v4l2_buffer desc;
};

/* Where the muxer's output goes.  A file today; a socket in the same shape,
 * which is why the muxer knows only about this one call.
 */

struct camenc_sink_s
{
  FILE *file;
  FAR struct camenc_ws_s *ws;
  uint64_t bytes;
  uint32_t segments;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct camenc_buf_s g_cam[CAMENC_BUFFERS];
static struct camenc_buf_s g_out[CAMENC_BUFFERS];
static struct camenc_buf_s g_cap[CAMENC_BUFFERS];

/* The server keeps a receive buffer for each client it may have, which makes
 * it 8464 bytes -- more than the whole of this task's stack, which is
 * CONFIG_EXAMPLES_CAMENC_STACKSIZE and 8192 by default.  As a local variable
 * it overran: the frame alone was 9520 bytes, so the first store into it
 * ran off the end of the stack into whatever the heap had put there, and
 * the allocation that followed walked the damage and faulted.  It is here
 * for the same reason the buffers above are: one camenc at a time, and it
 * cannot fail to be there.
 */

static struct camenc_ws_s g_ws;

/* The settings the page is driving.
 *
 * Nothing here is applied where it arrives.  The command callback runs
 * inside the server's poll, and the poll runs inside the capture loop
 * between frames, so an ioctl from there would put an I2C transaction in the
 * path between two frames -- the same reason the loop's own writes are held
 * back until the frame has been encoded.  A command therefore records what
 * was asked for and the loop applies it, a frame later at the most.
 *
 * The exposure and gain here are kept equal to what is in force, not to what
 * was last asked for.  A slider moved while the loop is running has to be a
 * change from the current picture, and a page that only ever heard its own
 * commands echoed back would ask for the gain it wanted with the gain that
 * was right several seconds ago.
 */

struct camenc_ui_s
{
  bool ae_auto;
  bool awb_auto;
  bool have_ae;
  bool have_awb;
  uint32_t exposure;
  uint32_t gain;
  uint32_t wb[3];
};

static struct camenc_ui_s g_ui;

/* The last status line sent, so that the same line is not sent every frame.
 * The server does not keep one client's state, so the comparison lives here.
 */

static char g_status_last[CAMENC_STATUS_MAX];
static uint32_t g_status_at;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t camenc_now_us(void)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

/* Read the number that follows `name` in a control message.
 *
 * A message is one name and one number, separated by a space, and a name
 * that does not match is not an error -- see camenc_ui_command.  What is an
 * error is a name with nothing readable after it, so the two are told apart
 * by whether the conversion consumed anything.
 */

static bool camenc_ui_arg(FAR const char *text, FAR const char *name,
                          FAR int32_t *value)
{
  size_t n = strlen(name);
  FAR char *end;
  long v;

  if (strncmp(text, name, n) != 0 || text[n] != ' ')
    {
      return false;
    }

  v = strtol(text + n + 1u, &end, 10);

  if (end == text + n + 1u)
    {
      return false;
    }

  *value = (int32_t)v;
  return true;
}

/* A control message from the page.
 *
 * The vocabulary is one name and one number:
 *
 *   ae <0|1>      the exposure and gain loop off, or on again
 *   e <lines>     the exposure by hand, which turns that loop off
 *   g <gain>      the gain by hand, which does the same
 *   aw <0|1>      the white balance loop off, or on again
 *   kr <gain>     the red white balance gain by hand, which turns that loop
 *   kg <gain>     off -- as are the other two.  256 is unity, and the names
 *   kb <gain>     are the ones the status line reports them under
 *
 * Setting a value by hand stops the loop for it rather than moving a slider
 * that the loop will pull back on the next frame.  That is what a user means
 * by dragging it, and it is the difference between a control and a display.
 *
 * Anything else is ignored in silence.  The page and this program are the
 * only two ends of this, and the alternative -- refusing, or disconnecting --
 * would drop a viewer that is watching the stream perfectly well because it
 * sent a message with a name in a newer version.
 */

static void camenc_ui_command(FAR const char *text, FAR void *arg)
{
  FAR struct camenc_ui_s *ui = (FAR struct camenc_ui_s *)arg;
  int32_t value;

  if (camenc_ui_arg(text, "ae", &value))
    {
      ui->ae_auto = (value != 0);
    }
  else if (camenc_ui_arg(text, "aw", &value))
    {
      ui->awb_auto = (value != 0);
    }
  else if (camenc_ui_arg(text, "e", &value))
    {
      ui->exposure = (uint32_t)value;
      ui->have_ae = true;
      ui->ae_auto = false;
    }
  else if (camenc_ui_arg(text, "g", &value))
    {
      ui->gain = (uint32_t)value;
      ui->have_ae = true;
      ui->ae_auto = false;
    }
  else if (camenc_ui_arg(text, "kr", &value))
    {
      ui->wb[0] = (uint32_t)value;
      ui->have_awb = true;
      ui->awb_auto = false;
    }
  else if (camenc_ui_arg(text, "kg", &value))
    {
      ui->wb[1] = (uint32_t)value;
      ui->have_awb = true;
      ui->awb_auto = false;
    }
  else if (camenc_ui_arg(text, "kb", &value))
    {
      ui->wb[2] = (uint32_t)value;
      ui->have_awb = true;
      ui->awb_auto = false;
    }
}

/* Send the page what the loops are holding, if it is not what they were told
 * last time.
 *
 * Sent whenever the line changes, which is what makes a slider follow an
 * automatic loop in real time, and every CAMENC_STATUS_FRAMES when it does not
 * change, which is what a client that arrives on a settled scene needs -- it
 * has been told nothing at all, and without the second case its sliders would
 * sit wherever the page put them when the document loaded.
 */

static void camenc_status_publish(FAR struct camenc_ws_s *ws,
                                  FAR const struct camenc_3a_s *a,
                                  uint32_t frame)
{
  char line[CAMENC_STATUS_MAX];
  size_t n;

  n = camenc_3a_status(a, line, sizeof(line));
  if (n == 0u)
    {
      return;
    }

  if (n == strlen(g_status_last) && memcmp(line, g_status_last, n) == 0 &&
      frame - g_status_at < CAMENC_STATUS_FRAMES)
    {
      return;
    }

  camenc_ws_status(ws, line, n);

  memcpy(g_status_last, line, n + 1u);
  g_status_at = frame;
}

/* Where a frame's time goes.
 *
 * The loop is serial -- one frame is taken from the camera, copied, encoded,
 * muxed and served before the next one starts -- so its frame rate is the
 * reciprocal of the sum of these, and when that rate is short of the
 * camera's, these are what say which of them is over budget.
 *
 * The encoder is the one that cannot be reasoned about from the source code:
 * its cost depends on what is being filmed, so a scene with motion in it can
 * take twice what a still one does.  Which makes it the one that has to be
 * measured rather than assumed.
 *
 * The camera wait is kept as a stage rather than left out because it is the
 * only evidence that the camera itself is running at the rate it was asked
 * for: if it is not, every other figure here is against the wrong clock.
 */

enum camenc_stage_e
{
  CAMENC_STAGE_CAMERA,
  CAMENC_STAGE_COPY,
  CAMENC_STAGE_ENCODE,
  CAMENC_STAGE_DRAIN,

  /* Muxing is the whole of handing a frame to the muxer, and it includes the
   * two stages after it: a muxer with nowhere to put its output would report
   * an empty mux stage, which is true of the muxer and useless as a diagnosis
   * of why the frame rate is short.
   */

  CAMENC_STAGE_MUX,
  CAMENC_STAGE_FILE,
  CAMENC_STAGE_CLIENTS,
  CAMENC_STAGE_SERVE,
  CAMENC_STAGE_MAX
};

static const char *const g_stage_name[CAMENC_STAGE_MAX] = {
  "camera", "copy", "encode", "drain", "mux", "file", "clients", "serve"
};

static uint64_t g_stage_us[CAMENC_STAGE_MAX];
static uint64_t g_stage_max[CAMENC_STAGE_MAX];

static void camenc_stage_account(enum camenc_stage_e stage, uint64_t start)
{
  uint64_t us = camenc_now_us() - start;

  g_stage_us[stage] += us;
  if (us > g_stage_max[stage])
    {
      g_stage_max[stage] = us;
    }
}

static void camenc_stage_reset(void)
{
  int s;

  for (s = 0; s < CAMENC_STAGE_MAX; s++)
    {
      g_stage_us[s] = 0;
      g_stage_max[s] = 0;
    }
}

static void camenc_usage(void)
{
  printf("Usage: camenc [-d camdev] [-e encdev] [-o outfile] [-n frames]\n");
  printf("              [-w width] [-h height] [-q qp] [-x exposure]"
         " [-g gain]\n");
  printf("  -d  capture device (default %s)\n", CAMENC_CAM_DEVPATH);
  printf("  -e  encoder device (default %s)\n", CAMENC_ENC_DEVPATH);
  printf("  -o  write fragmented MP4 here (default none, encode only)\n");
  printf("  -p  also serve the stream on this port as WebSocket"
         " (default %d, 0 for none)\n",
         CAMENC_DEFAULT_PORT);
  printf("  -n  frames to encode (default %d)\n", CAMENC_DEFAULT_FRAMES);
  printf("  -w  width (default %d, must be a multiple of 16)\n",
         CAMENC_DEFAULT_WIDTH);
  printf("  -h  height (default %d, must be a multiple of 16)\n",
         CAMENC_DEFAULT_HEIGHT);
  printf("  -q  quantiser, 0..51 (default %d)\n", CAMENC_DEFAULT_QP);
  printf("  -G  pictures per group, 1..1000 (default %d; 1 makes every"
         " frame an\n      IDR, a larger value makes the first frame of"
         " each group one and\n      the rest predict from the frame"
         " before them.  2 is worth about\n      half the bit rate of 1"
         " and longer groups flatten out after that)\n",
         CAMENC_DEFAULT_GOP);
  printf("  -x  sensor exposure in lines, overrides the driver default\n");
  printf("  -g  sensor analog gain, 16 = 1.0x and 1023 = 64x\n");
  printf("  -A  automatic exposure, gain and white balance, 0 or 1"
         " (default %d)\n",
         CAMENC_3A_DEFAULT);
  printf("  -t  brightness the automatic loop steers to, 0..255"
         " (default %u)\n",
         CAMENC_3A_TARGET_DEFAULT);
  printf("\n");
  printf("With -A the exposure and gain above are only where the loop "
         "starts;\n");
  printf("with -A 0 they are what the camera runs at, which is how a "
         "fixed\n");
  printf("exposure with an automatic gain is asked for: set -x and "
         "leave -g to\n");
  printf("the default.\n\n");
  printf(
      "The output is fragmented MP4 -- an initialisation segment followed\n");
  printf(
      "by one fragment per frame -- which is what a browser plays through\n");
  printf(
      "Media Source Extensions, and what the WebSocket carries.  With -p,\n");
  printf("point a browser at http://<board address>:<port>/ and it plays.\n");

  printf("\nThat page carries the 3A controls beside the picture: exposure "
         "and\n");
  printf("gain, and the white balance as its three channel gains, each "
         "with a\n");
  printf("switch back to automatic.  A slider follows whatever the "
         "loop\n");
  printf(
      "decides -- it is a readout of the loop while the loop is running --\n");
  printf("and moving one takes that control over, which is the same as what "
         "-x\n");
  printf("and -g do at startup.  Handing it back resumes from where it was\n");
  printf("left, so the switch is not a jump.\n");

  printf("\nThe white balance is set as its three channel gains, not as a "
         "colour\n");
  printf("temperature, and the output says why in camenc_3a.h: a kelvin "
         "needs\n");
  printf("this sensor calibrated against a lamp of known colour, and "
         "without\n");
  printf("that it is a number that cannot be derived from the gains -- so "
         "it\n");
  printf("could only ever report what was last typed into it.  A product "
         "would\n");
  printf("do the calibration; that is the note, rather than a "
         "plausible-looking\n");
  printf("number in its place.\n");

  printf("\nA board whose capture driver has no 3A control plane says so on "
         "the\n");
  printf("page and leaves the controls out of it.\n");
}

/* Set one control.
 *
 * Through VIDIOC_S_EXT_CTRLS and not VIDIOC_S_CTRL, even though there is
 * only one control -- because the encoder answers the second with ENOTTY.
 * The capture framework implements both, the encoder only the extended one:
 * its table has no plain set-control entry, and the encoder's own controls
 * are private ones that are not in any standard class.
 *
 * This is the same kind of asymmetry as the one between the two devices'
 * queue operations, and it fails the same way -- an error from an ioctl the
 * application had no reason to think was unsupported.
 */

/* The same, without saying so.
 *
 * The exposure loop writes these as it converges, which can be most frames
 * for the first second and nothing at all afterwards; a line per write would
 * be a line per frame during exactly the part of the run that a reader is
 * trying to watch.
 */

static int camenc_set_ctrl_quiet(int fd, uint32_t id, int value)
{
  struct v4l2_ext_control ctrl;
  struct v4l2_ext_controls ctrls;

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = (uint16_t)id;
  ctrl.value = value;

  memset(&ctrls, 0, sizeof(ctrls));
  ctrls.count = 1;
  ctrls.controls = &ctrl;

  if (ioctl(fd, VIDIOC_S_EXT_CTRLS, (unsigned long)&ctrls) < 0)
    {
      return -errno;
    }

  return 0;
}

static int camenc_set_ctrl(int fd, uint32_t id, int value,
                           FAR const char *name)
{
  int ret = camenc_set_ctrl_quiet(fd, id, value);

  if (ret < 0)
    {
      printf("camenc: setting %s = %d failed: %d\n", name, value, errno);
    }

  return ret;
}

/* Read one control back, or leave the value alone and say so.
 *
 * The capture framework implements this -- capture_g_ext_ctrls() passes the
 * id to the sensor's get_value() -- which is what makes it possible for the
 * exposure loop to be seeded from what the sensor is actually holding rather
 * than from a guess at it.  See the seeding below for why that matters.
 *
 * Returns OK when the value was read, and a negated errno when it was not,
 * in which case *value is untouched.
 */

static int camenc_get_ctrl(int fd, uint32_t id, FAR int *value)
{
  struct v4l2_ext_control ctrl;
  struct v4l2_ext_controls ctrls;

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = (uint16_t)id;

  memset(&ctrls, 0, sizeof(ctrls));
  ctrls.count = 1;
  ctrls.controls = &ctrl;

  if (ioctl(fd, VIDIOC_G_EXT_CTRLS, (unsigned long)&ctrls) < 0)
    {
      return -errno;
    }

  *value = ctrl.value;
  return OK;
}

/* Negotiate one side of the encoder and report what it settled on.
 *
 * The output side has to be set first: the capture geometry follows it,
 * because there is no scaler.
 */

static int camenc_set_format(int fd, uint32_t type, uint32_t pixelformat,
                             uint32_t width, uint32_t height,
                             FAR const char *what, FAR struct v4l2_format *out)
{
  struct v4l2_format fmt;

  memset(&fmt, 0, sizeof(fmt));
  fmt.type = type;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = pixelformat;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (ioctl(fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      printf("camenc: %s VIDIOC_S_FMT failed: %d\n", what, errno);
      return -errno;
    }

  if (out != NULL)
    {
      *out = fmt;
    }

  return 0;
}

static void camenc_describe_format(FAR const char *what,
                                   FAR const struct v4l2_format *fmt)
{
  printf("%-9s %" PRIu32 "x%" PRIu32 ", stride %" PRIu32 ", size %" PRIu32
         "\n",
         what, fmt->fmt.pix.width, fmt->fmt.pix.height,
         fmt->fmt.pix.bytesperline, fmt->fmt.pix.sizeimage);
}

/* How many bytes one NV12 frame of this geometry occupies.
 *
 * From the geometry, not from bytesperline: the capture framework never
 * fills that field in -- it does not appear in the file at all -- so a size
 * taken from it is zero, and a copy of zero bytes is not an error anywhere.
 * The stride is used when a driver does report one, because then it is the
 * authoritative figure; the width is the fallback, and for a geometry that
 * is already a multiple of sixteen the two are the same.
 */

static size_t camenc_frame_size(FAR const struct v4l2_format *fmt)
{
  size_t stride = fmt->fmt.pix.bytesperline != 0 ? fmt->fmt.pix.bytesperline
                                                 : fmt->fmt.pix.width;

  return stride * fmt->fmt.pix.height * 3u / 2u;
}

/* Request buffers and map them.
 *
 * `by_offset` says which of the two conventions this device uses: the encoder
 * is given the descriptor QUERYBUF produced, the camera is given nothing but
 * an index.  Both are kept in `desc` so that either can be queued later
 * without the caller having to remember which.
 */

static int camenc_setup_buffers(int fd, uint32_t type,
                                FAR struct camenc_buf_s *bufs, int count,
                                FAR uint32_t *granted, FAR const char *what)
{
  struct v4l2_requestbuffers req;
  int i;

  memset(&req, 0, sizeof(req));
  req.count = (uint32_t)count;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0)
    {
      printf("camenc: %s VIDIOC_REQBUFS failed: %d\n", what, errno);
      return -errno;
    }

  if (req.count < 1)
    {
      printf("camenc: %s granted %" PRIu32 " buffers\n", what, req.count);
      return -EINVAL;
    }

  if (req.count < (uint32_t)count)
    {
      printf("camenc: %s granted %" PRIu32 " of %d buffers\n", what, req.count,
             count);
    }

  /* Only what was granted is mapped and recorded: a slot the driver did not
   * hand out stays NULL, and nothing afterwards may reach for it.
   */

  *granted = 0;

  for (i = 0; i < count && i < (int)req.count; i++)
    {
      struct v4l2_buffer buf;

      memset(&buf, 0, sizeof(buf));
      buf.type = type;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = (uint32_t)i;

      if (ioctl(fd, VIDIOC_QUERYBUF, (unsigned long)&buf) < 0)
        {
          printf("camenc: %s VIDIOC_QUERYBUF %d failed: %d\n", what, i, errno);
          return -errno;
        }

      /* The length QUERYBUF reports is a real buffer's, where the format's
       * sizeimage is a request the driver may or may not have filled in.  The
       * buffer is what actually has to be big enough, so it is what is
       * checked.
       */

      bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, buf.m.offset);
      if (bufs[i].start == MAP_FAILED)
        {
          printf("camenc: %s mmap %d failed: %d\n", what, i, errno);
          bufs[i].start = NULL;
          return -errno;
        }

      bufs[i].length = buf.length;
      bufs[i].desc = buf;
      *granted = (uint32_t)(i + 1);
    }

  return 0;
}

static int camenc_queue(int fd, FAR struct camenc_buf_s *buf,
                        FAR const char *what, int index)
{
  struct v4l2_buffer q;
  int ret;

  /* The descriptor that came back from QUERYBUF is reused, with only the
   * fields that a queue operation is allowed to change touched.  For the
   * camera that amounts to the index; for the encoder it also carries
   * m.offset, which is the only thing it looks at.
   *
   * bytesused is left as the caller set it.  Neither device reads it when a
   * buffer is queued -- the camera fills it in on the way out, and the
   * encoder works from its own geometry -- but overwriting it with the
   * buffer's capacity would make the field say something untrue.
   */

  q = buf->desc;
  q.index = (uint32_t)index;
  q.flags = 0;

  ret = ioctl(fd, VIDIOC_QBUF, (unsigned long)&q);
  if (ret < 0)
    {
      printf("camenc: %s VIDIOC_QBUF %d failed: %d\n", what, index, errno);
      return -errno;
    }

  return 0;
}

static int camenc_dequeue(int fd, uint32_t type, FAR struct v4l2_buffer *buf,
                          FAR const char *what)
{
  memset(buf, 0, sizeof(*buf));
  buf->type = type;
  buf->memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_DQBUF, (unsigned long)buf) < 0)
    {
      printf("camenc: %s VIDIOC_DQBUF failed: %d\n", what, errno);
      return -errno;
    }

  return 0;
}

static int camenc_stream_on(int fd, int *type, FAR const char *what)
{
  if (ioctl(fd, VIDIOC_STREAMON, (unsigned long)type) < 0)
    {
      printf("camenc: %s VIDIOC_STREAMON failed: %d\n", what, errno);
      return -errno;
    }

  return 0;
}

static int camenc_stream_off(int fd, int *type)
{
  return ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)type) < 0 ? -errno : 0;
}

/* The muxer's output goes here.  Handing over a segment is the whole of the
 * interface, which is why the same muxer can feed a file and a socket.
 */

static int camenc_emit(void *arg, enum camenc_seg_e seg, const uint8_t *data,
                       size_t len, bool key)
{
  FAR struct camenc_sink_s *sink = arg;

  if (sink->file != NULL)
    {
      uint64_t mark = camenc_now_us();

      if (fwrite(data, 1, len, sink->file) != len)
        {
          return -EIO;
        }

      /* Timed around the call rather than around the flush, because the
       * flush is the whole question: stdio buffers, so what this call costs
       * is the write() that a full buffer forces, not the copy into it.
       */

      camenc_stage_account(CAMENC_STAGE_FILE, mark);
    }

  if (sink->ws != NULL)
    {
      uint64_t mark = camenc_now_us();

      if (camenc_ws_publish(sink->ws, seg, data, len, key) < 0)
        {
          return -EIO;
        }

      camenc_stage_account(CAMENC_STAGE_CLIENTS, mark);
    }

  sink->bytes += len;
  sink->segments++;

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *camdev = CAMENC_CAM_DEVPATH;
  FAR const char *encdev = CAMENC_ENC_DEVPATH;
  FAR const char *outfile = NULL;
  struct camenc_stream_s st;
  struct camenc_sink_s sink;
  bool serving = false;
  bool codec_set = false;
  struct v4l2_format fmt;
  int out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  int cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int cam_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  FAR uint8_t *scratch = NULL;
  uint32_t width = CAMENC_DEFAULT_WIDTH;
  uint32_t height = CAMENC_DEFAULT_HEIGHT;
  uint32_t nbuf_cam = 0;
  uint32_t nbuf_out = 0;
  uint32_t nbuf_cap = 0;
  size_t src_size;
  size_t copy_size;
  size_t max_au;
  int frames = CAMENC_DEFAULT_FRAMES;
  int qp = CAMENC_DEFAULT_QP;
  int gop = CAMENC_DEFAULT_GOP;
  int port = 0;
  int exposure = -1;
  int gain = -1;
  int threea = CAMENC_3A_DEFAULT;
  uint32_t target = CAMENC_3A_TARGET_DEFAULT;
  int fd3a = -1;
  bool threea_started = false;
  struct camenc_3a_s threea_state;
  int camfd = -1;
  int encfd = -1;
  int opt;
  int ret;
  int i;
  int encoded = 0;
  int dropped = 0;
  uint64_t t0;
  uint64_t t1;
  uint64_t now;
  uint64_t media_t0 = 0;
  bool have_prev = false;
  int last_report = 0;
  uint64_t reported_at = 0;
  uint32_t win_idr = 0;
  uint32_t win_p = 0;
  uint64_t win_idr_bytes = 0;
  uint64_t win_p_bytes = 0;

  while ((opt = getopt(argc, argv, "d:e:o:n:w:h:q:x:g:p:G:A:t:")) != -1)
    {
      switch (opt)
        {
          case 'd':
            camdev = optarg;
            break;

          case 'e':
            encdev = optarg;
            break;

          case 'o':
            outfile = optarg;
            break;

          case 'p':
            port = atoi(optarg);
            break;

          case 'n':
            frames = atoi(optarg);
            break;

          case 'w':
            width = (uint32_t)atoi(optarg);
            break;

          case 'h':
            height = (uint32_t)atoi(optarg);
            break;

          case 'q':
            qp = atoi(optarg);
            break;

          case 'G':
            gop = atoi(optarg);
            break;

          case 'x':
            exposure = atoi(optarg);
            break;

          case 'g':
            gain = atoi(optarg);
            break;

          case 'A':
            threea = atoi(optarg) != 0;
            break;

          case 't':
            target = (uint32_t)atoi(optarg);
            if (target == 0u || target > 255u)
              {
                printf("camenc: brightness must be 1..255, not %" PRIu32 "\n",
                       target);
                return EXIT_FAILURE;
              }
            break;

          default:
            camenc_usage();
            return EXIT_FAILURE;
        }
    }

  /* The encoder works on whole macroblocks, so a geometry that is not a
   * multiple of sixteen would be rounded and the picture would not be the one
   * that was asked for.  Refusing is clearer than rounding quietly.
   */

  if (frames <= 0 || width == 0 || height == 0 || (width & 15u) != 0 ||
      (height & 15u) != 0)
    {
      camenc_usage();
      return EXIT_FAILURE;
    }

  if (qp < 0 || qp > 51)
    {
      printf("camenc: quantiser must be 0..51, not %d\n", qp);
      return EXIT_FAILURE;
    }

  if (gop < 1 || gop > 1000)
    {
      printf("camenc: group length must be 1..1000, not %d\n", gop);
      return EXIT_FAILURE;
    }

  memset(&sink, 0, sizeof(sink));

  /* ---------------------------------------------------------------- */
  /* The camera                                                        */
  /* ---------------------------------------------------------------- */

  camfd = open(camdev, O_RDWR);
  if (camfd < 0)
    {
      printf("camenc: cannot open %s: %d\n", camdev, errno);
      return EXIT_FAILURE;
    }

  printf("camera:   %s\n", camdev);

  ret = camenc_set_format(camfd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                          V4L2_PIX_FMT_NV12, width, height, "camera", &fmt);
  if (ret < 0)
    {
      goto errout;
    }

  camenc_describe_format("source", &fmt);

  /* The sensor's own defaults are used unless asked otherwise.  Its
   * exposure and gain are in manual mode, so these are the only things that
   * set them; the camera driver writes them when the stream starts.
   */

  if (exposure >= 0 && camenc_set_ctrl(camfd, V4L2_CID_EXPOSURE_ABSOLUTE,
                                       exposure, "exposure") < 0)
    {
      goto errout;
    }

  if (gain >= 0 &&
      camenc_set_ctrl(camfd, V4L2_CID_ISO_SENSITIVITY, gain, "gain") < 0)
    {
      goto errout;
    }

  /* The 3A control plane, and the loop that uses it.
   *
   * Opened before the stream starts so that the loop can be seeded from what
   * the sensor is actually holding.  A loop that assumed its own defaults
   * would compute its first correction against a setting that is not in
   * force, which shows as the picture stepping when it should not.
   *
   * Without a control plane the loop is simply not started, and the camera
   * runs on whatever the exposure and gain were set to -- which is what this
   * program did before the loop existed.
   */

  if (threea)
    {
      struct cam3a_stats_s probe;
      struct camenc_3a_cfg_s cfg3a;

      /* The loop is seeded from what the sensor is *actually* holding, read
       * back rather than assumed.
       *
       * This is not tidiness.  The board's sensor default gain comes from
       * CONFIG_OV5647_ANALOG_GAIN, which is 256 here while the loop's own
       * minimum is 16 -- so a loop that guessed would start believing the
       * picture was sixteen times darker than it is, ask for sixteen times
       * too much light in its first correction, and visibly darken the
       * picture before climbing back.  Starting from a wrong belief about
       * the hardware is the one thing a loop cannot compute its way out of.
       *
       * Where the readback is not available the seed values are written to
       * the sensor instead, so that the belief is made true rather than
       * hoped to be.  Either way the two agree before the first frame is
       * measured.
       */

      int read_exp = 0;
      int read_gain = 0;
      uint32_t seed_exp;
      uint32_t seed_gain;
      bool seeded_from_hw;

      seeded_from_hw =
          camenc_get_ctrl(camfd, V4L2_CID_EXPOSURE_ABSOLUTE, &read_exp) ==
              OK &&
          camenc_get_ctrl(camfd, V4L2_CID_ISO_SENSITIVITY, &read_gain) == OK &&
          read_exp > 0 && read_gain > 0;

      seed_exp = seeded_from_hw  ? (uint32_t)read_exp
                 : exposure >= 0 ? (uint32_t)exposure
                                 : CAMENC_3A_EXPOSURE_MAX;
      seed_gain = seeded_from_hw ? (uint32_t)read_gain
                  : gain >= 0    ? (uint32_t)gain
                                 : CAMENC_3A_GAIN_MIN;

      fd3a = open(CAM3A_DEVPATH, O_RDWR);
      if (fd3a < 0)
        {
          printf("camenc: no 3A control plane at %s (%d); running with "
                 "fixed exposure and gain\n",
                 CAM3A_DEVPATH, errno);
          threea = 0;
        }
      else
        {
          if (!seeded_from_hw)
            {
              /* Put the sensor where the loop thinks it is.  This is worth
               * saying out loud: a loop seeded from a value the hardware
               * does not hold is the case all of this exists to avoid.
               */

              printf("camenc: the sensor's exposure and gain could not be "
                     "read back; setting them to %" PRIu32 " and %" PRIu32
                     "\n",
                     seed_exp, seed_gain);

              (void)camenc_set_ctrl_quiet(camfd, V4L2_CID_EXPOSURE_ABSOLUTE,
                                          (int)seed_exp);
              (void)camenc_set_ctrl_quiet(camfd, V4L2_CID_ISO_SENSITIVITY,
                                          (int)seed_gain);
            }

          memset(&probe, 0, sizeof(probe));
          if (ioctl(fd3a, CAM3A_GET_STATS, (unsigned long)&probe) < 0)
            {
              /* Not fatal: a driver that cannot report yet still measures
               * from the next frame on, and the loop starts from unity
               * gains.  Said once, because it is a fact about the board and
               * not about any frame.
               */

              printf("camenc: reading the 3A statistics failed (%d); "
                     "starting from unity white balance\n",
                     errno);
            }

          memset(&cfg3a, 0, sizeof(cfg3a));
          cfg3a.target = target;
          cfg3a.exposure_min = CAMENC_3A_EXPOSURE_MIN;
          cfg3a.exposure_max = CAMENC_3A_EXPOSURE_MAX;
          cfg3a.gain_min = CAMENC_3A_GAIN_MIN;
          cfg3a.gain_max = CAMENC_3A_GAIN_MAX;
          cfg3a.gain_one = CAMENC_3A_GAIN_ONE;
          cfg3a.ae_shift = CAMENC_3A_AE_SHIFT;
          cfg3a.awb_shift = CAMENC_3A_AWB_SHIFT;
          cfg3a.settle = CAMENC_3A_SETTLE;
          cfg3a.awb_min_mean = CAMENC_3A_AWB_MIN_MEAN;

          camenc_3a_init(&threea_state, &cfg3a, seed_exp, seed_gain, probe.wb,
                         probe.sequence);

          /* The page starts from the settings the loop starts from, so that
           * the first slider moved is a change from what is in force rather
           * than from a zero that would clamp.
           */

          g_ui.ae_auto = true;
          g_ui.awb_auto = true;
          g_ui.exposure = threea_state.exposure;
          g_ui.gain = threea_state.gain;
          g_ui.wb[0] = threea_state.wb[0];
          g_ui.wb[1] = threea_state.wb[1];
          g_ui.wb[2] = threea_state.wb[2];

          threea_started = true;

          /* The gains are handed over explicitly, which is also what stops
           * the driver's own loop from steering them.  Leaving both running
           * would be two controllers on one register.
           */

          {
            struct cam3a_wb_s wb3a;

            wb3a.gain[0] = threea_state.wb[0];
            wb3a.gain[1] = threea_state.wb[1];
            wb3a.gain[2] = threea_state.wb[2];

            if (ioctl(fd3a, CAM3A_SET_WB, (unsigned long)&wb3a) < 0)
              {
                printf("camenc: taking over the white balance failed: %d\n",
                       errno);
              }
          }

          printf("camenc: 3A loop on, target %" PRIu32
                 ", starting at exposure %" PRIu32 " gain %" PRIu32 " (%s)\n",
                 target, seed_exp, seed_gain,
                 seeded_from_hw ? "read from the sensor"
                                : "set by this program");
        }
    }

  ret = camenc_setup_buffers(camfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, g_cam,
                             CAMENC_BUFFERS, &nbuf_cam, "camera");
  if (ret < 0)
    {
      goto errout;
    }

  src_size = camenc_frame_size(&fmt);

  /* The length QUERYBUF reported is a real buffer's, so it is what says
   * whether a frame fits.  The format's sizeimage is a request the driver
   * may or may not have filled in; the camera leaves it zero.
   */

  if (g_cam[0].length < src_size)
    {
      printf("camenc: camera buffers are %zu bytes, an NV12 frame at %" PRIu32
             "x%" PRIu32 " needs %zu\n",
             g_cam[0].length, fmt.fmt.pix.width, fmt.fmt.pix.height, src_size);
      ret = -EINVAL;
      goto errout;
    }

  if (fmt.fmt.pix.bytesperline == 0)
    {
      printf("camera:   no stride reported, using the width; %zu bytes per"
             " frame\n",
             src_size);
    }

  /* ---------------------------------------------------------------- */
  /* The encoder                                                       */
  /* ---------------------------------------------------------------- */

  encfd = open(encdev, O_RDWR);
  if (encfd < 0)
    {
      printf("camenc: cannot open %s: %d\n", encdev, errno);
      ret = -errno;
      goto errout;
    }

  printf("encoder:  %s\n", encdev);

  /* Output side first: it is what the capture side's geometry follows. */

  ret = camenc_set_format(encfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_NV12,
                          width, height, "encoder input", &fmt);
  if (ret < 0)
    {
      goto errout;
    }

  camenc_describe_format("input", &fmt);

  ret = camenc_set_format(encfd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                          V4L2_PIX_FMT_H264, 0, 0, "encoder output", &fmt);
  if (ret < 0)
    {
      goto errout;
    }

  camenc_describe_format("output", &fmt);

  /* A quantiser is the one encoder setting with no usable default: it
   * decides the bit rate, and the right value depends on what is being
   * filmed.
   */

  if (camenc_set_ctrl(encfd, CAMENC_CID_QP, qp, "qp") < 0)
    {
      goto errout;
    }

  /* Set unconditionally rather than only when asked for: the driver's own
   * default is the same value, so this changes nothing unless it was asked
   * to -- and having one place that states the group length in use means the
   * log says what the stream is, not what it would have been.
   */

  if (camenc_set_ctrl(encfd, CAMENC_CID_GOP, gop, "gop") < 0)
    {
      goto errout;
    }

  ret = camenc_setup_buffers(encfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, g_out,
                             CAMENC_BUFFERS, &nbuf_out, "encoder input");
  if (ret < 0)
    {
      goto errout;
    }

  ret = camenc_setup_buffers(encfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, g_cap,
                             CAMENC_BUFFERS, &nbuf_cap, "encoder output");
  if (ret < 0)
    {
      goto errout;
    }

  /* The two devices have to agree on how big a frame is, because the loop
   * copies one into the other.  They derive it separately -- the camera from
   * its own stride, the encoder from its geometry rounded up to macroblocks
   * -- so a geometry they disagree about would be a buffer overrun rather
   * than a wrong picture.  Both are multiples of sixteen here, which is what
   * makes them agree; the copy is bounded by the smaller of the two anyway.
   */

  copy_size = g_out[0].length < src_size ? g_out[0].length : src_size;

  if (g_out[0].length != src_size)
    {
      printf("camenc: camera frames are %zu bytes, the encoder's input is"
             " %zu; copying %zu\n",
             src_size, g_out[0].length, copy_size);
    }

  /* The encoder refuses to produce more than the buffer it advertises, so
   * that size bounds an access unit and therefore sizes the muxer's
   * workspace.  One fragment is the boxes plus the unit itself, with its
   * start codes widened to lengths.
   */

  max_au = (uint32_t)g_cap[0].length;
  scratch = malloc((size_t)max_au + CAMENC_STREAM_SLACK);
  if (scratch == NULL)
    {
      printf("camenc: no room for a %zu-byte mux workspace\n",
             (size_t)max_au + CAMENC_STREAM_SLACK);
      ret = -ENOMEM;
      goto errout;
    }

  if (outfile != NULL)
    {
      sink.file = fopen(outfile, "wb");
      if (sink.file == NULL)
        {
          printf("camenc: cannot create %s: %d\n", outfile, errno);
          ret = -errno;
          goto errout;
        }
    }

  ret = camenc_stream_init(&st, width, height, (uint32_t)max_au, scratch,
                           (size_t)max_au + CAMENC_STREAM_SLACK, camenc_emit,
                           &sink);
  if (ret < 0)
    {
      printf("camenc: muxer init failed: %d\n", ret);
      goto errout;
    }

  /* The stream is served from the same buffers it is muxed into; the server
   * takes a copy of each segment for each client, so nothing outlives the
   * call that produced it.
   */

  if (port > 0)
    {
      /* Room for the largest segment, and for the initialisation segment
       * that goes to a client which has just connected.
       *
       * Both are in a client's buffer at the same time whenever its socket
       * is slow to drain, so it is their sum rather than either one: a
       * buffer sized for one segment alone is a buffer that drops a client
       * on the frame that follows its handshake, which is what this used to
       * do.
       */

      ret = camenc_ws_start(&g_ws, (uint16_t)port,
                            CAMENC_WS_INIT_MAX + CAMENC_WS_MAX_SEGMENT +
                                CAMENC_WS_TX_HEADER);
      if (ret < 0)
        {
          printf("camenc: cannot serve on port %d: %d\n", port, ret);
          goto errout;
        }

      serving = true;
      sink.ws = &g_ws;

      /* Control messages from the page land here.  Nothing is applied in
       * the callback -- see camenc_ui_s.
       */

      camenc_ws_set_command(&g_ws, camenc_ui_command, &g_ui);

      printf("serving:  http://<board address>:%d/  (%u clients at most,\n"
             "          %u-byte segments)\n",
             port, CAMENC_WS_MAX_CLIENTS, CAMENC_WS_MAX_SEGMENT);
    }

  /* ---------------------------------------------------------------- */
  /* Streams                                                           */
  /* ---------------------------------------------------------------- */

  /* The encoder's capture side is started before its output side.  A frame
   * queued on the output with nowhere for the bitstream to go would have to
   * be held, and this driver encodes inside the queue operation, so there
   * would be nothing to hold it with.
   */

  ret = camenc_stream_on(encfd, &cap_type, "encoder output");
  if (ret < 0)
    {
      goto errout;
    }

  ret = camenc_stream_on(encfd, &out_type, "encoder input");
  if (ret < 0)
    {
      goto errout;
    }

  /* Every capture buffer is offered up front: the encoder takes one per frame
   * and the application is expected to keep a pool of them in flight.
   */

  for (i = 0; i < (int)nbuf_cap; i++)
    {
      if (camenc_queue(encfd, &g_cap[i], "encoder output", i) < 0)
        {
          ret = -EIO;
          goto errout_streamoff;
        }
    }

  /* The camera's buffers are offered too, but only after the stream starts:
   * the capture framework stops a stream it has no vacant buffers for, so
   * filling the queue before STREAMON would give it nothing to start with.
   */

  ret = camenc_stream_on(camfd, &cam_type, "camera");
  if (ret < 0)
    {
      goto errout_streamoff;
    }

  for (i = 0; i < (int)nbuf_cam; i++)
    {
      if (camenc_queue(camfd, &g_cam[i], "camera", i) < 0)
        {
          ret = -EIO;
          goto errout_camoff;
        }
    }

  printf("\nencoding %d frames at qp %d...\n", frames, qp);

  t0 = camenc_now_us();

  /* ---------------------------------------------------------------- */
  /* The loop                                                          */
  /* ---------------------------------------------------------------- */

  for (i = 0; i < frames; i++)
    {
      struct v4l2_buffer cbuf;
      struct v4l2_buffer obuf;
      struct v4l2_buffer ebuf;
      struct camenc_buf_s *out;
      struct camenc_buf_s *cap;
      uint64_t pts;
      uint64_t mark;
      uint32_t camidx;

      /* A frame from the camera. */

      mark = camenc_now_us();

      ret =
          camenc_dequeue(camfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cbuf, "camera");
      if (ret < 0)
        {
          break;
        }

      camenc_stage_account(CAMENC_STAGE_CAMERA, mark);

      camidx = cbuf.index;

      if (camidx >= nbuf_cam || g_cam[camidx].start == NULL)
        {
          printf("camenc: camera returned unusable buffer %" PRIu32 "\n",
                 camidx);
          ret = -EINVAL;
          break;
        }

      /* The time the frame arrived, measured from the first frame rather
       * than from the epoch or from the start of the loop.
       *
       * It is taken here rather than derived from the frame number, so that
       * the stream's timeline is the one the frames actually arrived on: a
       * camera that stutters produces a stream that stutters in the same
       * places instead of one that runs at the wrong speed throughout.
       *
       * The origin matters as much as the spacing.  gettimeofday() returns
       * seconds since 1970 here, so an absolute timestamp puts the stream's
       * first sample a hundred and fifty years in, and a player then reports
       * a start time and a duration to match.  Measuring from the first
       * frame makes the stream begin at zero, which is what a live stream
       * should look like and what a player can seek within.
       */

      now = camenc_now_us();

      if (!have_prev)
        {
          media_t0 = now;
        }

      pts = now - media_t0;

      /* The encoder's input buffer for this frame, taken round-robin.  With
       * as many as the camera has, the one just freed is the one used next,
       * so a frame is never copied over one still being encoded.
       */

      out = &g_out[i % (int)nbuf_out];

      mark = camenc_now_us();

      memcpy(out->start, g_cam[camidx].start, copy_size);
      out->desc.bytesused = (uint32_t)copy_size;

      camenc_stage_account(CAMENC_STAGE_COPY, mark);

      /* Queueing this encodes it, and the encode is waited for inside the
       * queue operation -- which is why this is the stage that decides the
       * frame rate rather than the one after it.
       */

      mark = camenc_now_us();

      if (camenc_queue(encfd, out, "encoder input", i % (int)nbuf_out) < 0)
        {
          ret = -EIO;
          break;
        }

      camenc_stage_account(CAMENC_STAGE_ENCODE, mark);

      /* Take the input buffer back.
       *
       * This is not bookkeeping.  A buffer the encoder has finished with is
       * marked done and left for the application to dequeue, and only the
       * dequeue returns it to the pool -- so a loop that queues input
       * buffers and never takes them back runs out of them after as many
       * frames as it queued, and the next queue operation fails with
       * EAGAIN.  Every buffer this loop takes out is put back, on both
       * sides; this is the one that is easy to leave out, because the
       * encoder never hands it to the application as a result.
       *
       * The encode happens inside the queue operation above, so the buffer
       * is already done by the time this runs and comes straight back.
       */

      mark = camenc_now_us();

      ret = camenc_dequeue(encfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &obuf,
                           "encoder input");
      if (ret < 0)
        {
          break;
        }

      if (obuf.index != (uint32_t)(i % (int)nbuf_out))
        {
          printf("camenc: encoder returned input buffer %" PRIu32
                 " where %d was queued\n",
                 obuf.index, i % (int)nbuf_out);
        }

      /* The bitstream. */

      ret = camenc_dequeue(encfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &ebuf,
                           "encoder output");
      if (ret < 0)
        {
          break;
        }

      if (ebuf.index >= nbuf_cap)
        {
          printf("camenc: encoder returned buffer %" PRIu32 "\n", ebuf.index);
          ret = -EINVAL;
          break;
        }

      camenc_stage_account(CAMENC_STAGE_DRAIN, mark);

      cap = &g_cap[ebuf.index];

      /* A buffer with nothing in it is the marker the flush leaves behind,
       * not a frame; there is nothing to mux.
       */

      if ((ebuf.flags & V4L2_BUF_FLAG_LAST) != 0 && ebuf.bytesused == 0)
        {
          if (camenc_queue(encfd, cap, "encoder output", (int)ebuf.index) < 0)
            {
              ret = -EIO;
              break;
            }

          break;
        }

      if (ebuf.bytesused == 0 || ebuf.bytesused > max_au)
        {
          printf("camenc: encoder returned %" PRIu32 " bytes\n",
                 ebuf.bytesused);
          dropped++;
        }
      else
        {
          /* Whether the encoder may be started at this frame, which is the
           * encoder's answer to give rather than something read out of the
           * bytes: a picture does not say whether anything after it predicts
           * from it.  It arrives as a buffer flag for exactly this reason.
           *
           * The distinction decides where a fragment may begin, so getting it
           * wrong is not cosmetic.  A fragment a player cannot start on is a
           * fragment it decodes to noise, and the muxer cannot tell after the
           * fact -- hence taking it from the source.
           */

          bool key = (ebuf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;

          mark = camenc_now_us();

          ret = camenc_stream_write(&st, cap->start, ebuf.bytesused, pts, key);
          if (ret < 0)
            {
              printf("camenc: muxing frame %d failed: %d\n", i, ret);
              break;
            }

          camenc_stage_account(CAMENC_STAGE_MUX, mark);

          /* What the group length is buying on this content.
           *
           * Counted here rather than assumed from the configuration: the
           * claim a P picture makes is that it costs a fraction of an IDR,
           * and the only place that can be checked is a stream that was
           * encoded from something that moved.  A P as large as its IDR
           * means prediction is not being used, whatever the registers say.
           */

          if (key)
            {
              win_idr++;
              win_idr_bytes += ebuf.bytesused;
            }
          else
            {
              win_p++;
              win_p_bytes += ebuf.bytesused;
            }

          /* The page has to name the stream's codec before a browser will
           * accept the data, and the codec is only known once a parameter
           * set has been seen -- which is the first frame.
           */

          if (serving && !codec_set)
            {
              char codec[16];

              if (camenc_stream_codec_string(&st, codec, sizeof(codec)) > 0)
                {
                  camenc_ws_set_codec(&g_ws, codec);
                  codec_set = true;
                  printf("camenc: stream codec is %s\n", codec);
                }
            }

          encoded++;
        }

      /* Both buffers go back, the encoder's first: holding either one is what
       * stops the next frame, and the encoder's is needed before the next
       * queue operation can complete.
       */

      if (camenc_queue(encfd, cap, "encoder output", (int)ebuf.index) < 0)
        {
          ret = -EIO;
          break;
        }

      if (camenc_queue(camfd, &g_cam[camidx], "camera", (int)camidx) < 0)
        {
          ret = -EIO;
          break;
        }

      have_prev = true;

      /* The 3A loop, one frame behind by construction.
       *
       * Run here, after the frame has been encoded, rather than as soon as
       * it was captured: the settings go out over I2C, and the cost of that
       * inside the capture-to-encode path is time the encoder sits idle.
       * Nothing is lost by waiting, because the frame about to be encoded
       * has already been exposed.
       *
       * A failure stops the loop rather than being retried every frame.  A
       * sensor that will not take a control will not take it on the next
       * frame either, and the alternative is a log line per frame for the
       * rest of the run.
       */

      if (threea)
        {
          struct cam3a_stats_s stats;
          uint32_t ch = 0;

          /* The modes, then anything the page has asked for since the last
           * frame, and only then the measurement.
           *
           * In that order because the three interact: a slider moved
           * between two frames takes effect on this one rather than the
           * next, and a mode change that arrives together with a value
           * cannot fight it -- setting a value by hand turns its loop off,
           * so applying the values first and the modes after would undo it.
           */

          camenc_3a_set_ae(&threea_state, g_ui.ae_auto);
          camenc_3a_set_awb(&threea_state, g_ui.awb_auto);

          if (g_ui.have_ae)
            {
              ch |=
                  camenc_3a_manual_ae(&threea_state, g_ui.exposure, g_ui.gain);
              g_ui.have_ae = false;
            }

          if (g_ui.have_awb)
            {
              ch |= camenc_3a_manual_wb(&threea_state, g_ui.wb);
              g_ui.have_awb = false;
            }

          memset(&stats, 0, sizeof(stats));

          if (ioctl(fd3a, CAM3A_GET_STATS, (unsigned long)&stats) < 0)
            {
              printf("camenc: reading the 3A statistics failed (%d); "
                     "leaving the exposure where it is\n",
                     errno);
              threea = 0;
            }
          else
            {
              ch |= camenc_3a_update(&threea_state, &stats);

              if ((ch & CAMENC_3A_EXPOSURE) != 0 &&
                  camenc_set_ctrl_quiet(camfd, V4L2_CID_EXPOSURE_ABSOLUTE,
                                        (int)threea_state.exposure) < 0)
                {
                  printf("camenc: the sensor will not take an exposure of "
                         "%" PRIu32 " (%d); stopping the loop\n",
                         threea_state.exposure, errno);
                  threea = 0;
                }

              if (threea != 0 && (ch & CAMENC_3A_GAIN) != 0 &&
                  camenc_set_ctrl_quiet(camfd, V4L2_CID_ISO_SENSITIVITY,
                                        (int)threea_state.gain) < 0)
                {
                  printf("camenc: the sensor will not take a gain of "
                         "%" PRIu32 " (%d); stopping the loop\n",
                         threea_state.gain, errno);
                  threea = 0;
                }

              if (threea != 0 && (ch & CAMENC_3A_WB) != 0)
                {
                  struct cam3a_wb_s wb3a;

                  wb3a.gain[0] = threea_state.wb[0];
                  wb3a.gain[1] = threea_state.wb[1];
                  wb3a.gain[2] = threea_state.wb[2];

                  if (ioctl(fd3a, CAM3A_SET_WB, (unsigned long)&wb3a) < 0)
                    {
                      printf("camenc: the capture driver will not take a "
                             "white balance of %" PRIu32 "/%" PRIu32
                             "/%" PRIu32 " (%d); stopping the loop\n",
                             wb3a.gain[0], wb3a.gain[1], wb3a.gain[2], errno);
                      threea = 0;
                    }
                }
            }

          /* The page's copy of the settings follows what is in force, not
           * what it last asked for.  A slider moved later is then a change
           * from the picture on the screen, and a page that only ever heard
           * its own commands echoed back would pair the value it wants with
           * one that was right several seconds ago.
           */

          g_ui.exposure = threea_state.exposure;
          g_ui.gain = threea_state.gain;

          if (serving)
            {
              camenc_status_publish(&g_ws, &threea_state, (uint32_t)i);
            }
        }

      /* Say something every so often rather than every frame.
       *
       * At this frame rate a line per frame is sixty lines a second, which
       * buries anything that matters -- a client connecting, a segment too
       * large to send, an error -- in a scroll that nobody can read.  What
       * is worth knowing continuously is that the loop is running and how
       * fast, and a line every few seconds says that just as well.
       */

      if (i - last_report >= CAMENC_REPORT_FRAMES)
        {
          uint64_t span = pts - reported_at;
          uint32_t window = (uint32_t)(i - last_report);
          uint64_t total = 0;
          int s;

          printf("frame %6d: %6" PRIu32 " bytes, %5.1f fps, %" PRIu64
                 " bytes out\n",
                 i, ebuf.bytesused,
                 span != 0
                     ? 1000000.0 * (double)(i - last_report) / (double)span
                     : 0.0,
                 sink.bytes);

          /* The breakdown, per frame, as mean and worst case.  Both, because
           * they answer different questions: the mean says which stage is
           * over the frame period, and the worst case says whether the loop
           * is one slow frame away from missing it.
           *
           * The sum of the stages is printed against the period for the same
           * reason the total is worth having at all: a stage nobody thought
           * to time is only visible as the difference between the two.
           */

          printf("  us/frame:");

          for (s = 0; s < CAMENC_STAGE_MAX; s++)
            {
              uint64_t mean = g_stage_us[s] / window;

              printf(" %s %" PRIu64 "/%" PRIu64, g_stage_name[s], mean,
                     g_stage_max[s]);

              if (s != CAMENC_STAGE_FILE && s != CAMENC_STAGE_CLIENTS)
                {
                  total += mean;
                }
            }

          printf(" sum %" PRIu64 " vs period %" PRIu64 "\n", total,
                 (uint64_t)(window != 0 ? span / window : 0));

          printf("  frames: %" PRIu32 " IDR, %" PRIu64 " B mean; %" PRIu32
                 " P, %" PRIu64 " B mean"
                 " (%" PRIu64 "%% of an IDR)\n",
                 win_idr, win_idr != 0 ? win_idr_bytes / win_idr : 0, win_p,
                 win_p != 0 ? win_p_bytes / win_p : 0,
                 win_idr != 0 && win_p != 0 ? (100u * (win_p_bytes / win_p)) /
                                                  (win_idr_bytes / win_idr)
                                            : 0);

          /* What the loop is holding, and whether it is still moving.
           *
           * The brightness is reported next to the target rather than only a
           * verdict, because "settled" means the loop stopped changing and
           * not that it arrived: a scene out of range settles at its
           * ceilings while sitting below the target, and the two are worth
           * telling apart without having to work it out from the numbers.
           */

          if (threea_started)
            {
              printf("  3a: exposure %" PRIu32 " gain %" PRIu32 " wb %" PRIu32
                     "/%" PRIu32 "/%" PRIu32 " level %" PRIu32 "/%" PRIu32
                     " %s%s\n",
                     threea_state.exposure, threea_state.gain,
                     threea_state.wb[0], threea_state.wb[1],
                     threea_state.wb[2], threea_state.last_level, target,
                     threea == 0            ? "loop stopped"
                     : threea_state.settled ? "settled"
                                            : "moving",
                     threea != 0 && threea_state.settled &&
                             !camenc_3a_at_target(&threea_state)
                         ? " (out of range)"
                         : "");
            }

          /* Whether the stream is actually reaching anyone.
           *
           * Worth a line beside the timings rather than only on failure,
           * because a browser that receives everything and one that receives
           * nothing cost this loop exactly the same: the sends are
           * non-blocking, so a client that is behind shows up nowhere else.
           */

          if (serving)
            {
              camenc_ws_report(&g_ws);
            }

          last_report = i;
          reported_at = pts;
          win_idr = 0;
          win_p = 0;
          win_idr_bytes = 0;
          win_p_bytes = 0;
          camenc_stage_reset();
        }

      /* Service the server between frames: take new connections, notice the
       * ones that have gone, and write out what is queued for the rest.  It
       * never blocks, because a stalled viewer must not stall the camera.
       */

      if (serving)
        {
          mark = camenc_now_us();

          camenc_ws_poll(&g_ws);

          camenc_stage_account(CAMENC_STAGE_SERVE, mark);
        }
    }

  t1 = camenc_now_us();

  if (ret < 0)
    {
      goto errout_camoff;
    }

  /* ---------------------------------------------------------------- */
  /* Flush                                                             */
  /* ---------------------------------------------------------------- */

  /* Nothing is buffered, so this ends the stream on the next buffer handed
   * back -- which is the one already in flight.
   */

  {
    struct v4l2_encoder_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = V4L2_ENC_CMD_STOP;

    if (ioctl(encfd, VIDIOC_ENCODER_CMD, (unsigned long)&cmd) >= 0)
      {
        struct v4l2_buffer ebuf;

        if (camenc_dequeue(encfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &ebuf,
                           "encoder output") == 0)
          {
            if (ebuf.index < nbuf_cap)
              {
                camenc_queue(encfd, &g_cap[ebuf.index], "encoder output",
                             (int)ebuf.index);
              }
          }
      }
  }

  printf("\n%d frames encoded, %d dropped\n", encoded, dropped);

  if (encoded > 0)
    {
      uint64_t dt = t1 - t0;

      printf("elapsed %" PRIu64 " us, %.2f encoding steps/s, %.2f Mbps\n", dt,
             dt != 0 ? (double)encoded * 1000000.0 / (double)dt : 0.0,
             dt != 0 ? (double)sink.bytes * 8.0 / (double)dt : 0.0);
    }

  if (outfile != NULL)
    {
      printf("wrote %" PRIu64 " bytes in %" PRIu32 " segments to %s\n",
             sink.bytes, sink.segments, outfile);
    }

  if (serving)
    {
      printf("served %" PRIu32 " client(s), dropped %" PRIu32 "\n",
             g_ws.clients_served, g_ws.clients_dropped);
    }

  ret = OK;

  /* ---------------------------------------------------------------- */
  /* Teardown                                                          */
  /* ---------------------------------------------------------------- */

errout_camoff:
  camenc_stream_off(camfd, &cam_type);

errout_streamoff:
  camenc_stream_off(encfd, &out_type);
  camenc_stream_off(encfd, &cap_type);

errout:
  if (serving)
    {
      camenc_ws_stop(&g_ws);
      serving = false;
    }

  if (sink.file != NULL)
    {
      fclose(sink.file);
    }

  free(scratch);

  for (i = 0; i < CAMENC_BUFFERS; i++)
    {
      if (g_cam[i].start != NULL)
        {
          munmap(g_cam[i].start, g_cam[i].length);
          g_cam[i].start = NULL;
        }

      if (g_out[i].start != NULL)
        {
          munmap(g_out[i].start, g_out[i].length);
          g_out[i].start = NULL;
        }

      if (g_cap[i].start != NULL)
        {
          munmap(g_cap[i].start, g_cap[i].length);
          g_cap[i].start = NULL;
        }
    }

  if (encfd >= 0)
    {
      close(encfd);
    }

  /* The white balance goes back to the driver's own loop on the way out.
   *
   * The gains this program leaves behind are the ones it converged to, and
   * they were chosen for the scene that was in front of the camera at the
   * time.  Handing the loop back means the next program to open the camera
   * starts from a driver that is looking after itself again, rather than
   * from a fixed correction for a scene that has gone.
   */

  if (threea_started)
    {
      int enable = 1;

      ioctl(fd3a, CAM3A_SET_AWB, (unsigned long)&enable);
    }

  if (fd3a >= 0)
    {
      close(fd3a);
    }

  if (camfd >= 0)
    {
      close(camfd);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
