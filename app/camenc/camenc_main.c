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
 * Each frame goes round the loop twice.  One round takes a frame from the
 * camera, copies it into the encoder's output buffer and queues it, which
 * starts the hardware and returns; the next round waits for the encoder, takes
 * the bitstream from its capture side, muxes it, and hands both containers
 * back.  The wait is the loop's single poll(), so the sockets the server owns
 * are serviced for as long as the encoder takes -- which is the point of the
 * encode no longer happening inside the queue operation.
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
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/boardctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <sys/videoio.h>

#include "cam3a.h"
#include "camenc_3a.h"
#include "camenc_stream.h"
#include "camenc_ws.h"
#include "ov5647.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMENC_CAM_DEVPATH "/dev/video0"
#define CAMENC_ENC_DEVPATH "/dev/video1"

/* The synthetic source's fixed parameters -- see camenc_probe_s for what the
 * pattern is and why it is the way it is.  Here rather than beside the code
 * that uses them because the usage text is above that.
 */

#define CAMENC_PROBE_SIDE     64
#define CAMENC_PROBE_SIDE_Y   32
#define CAMENC_PROBE_RADIUS   120
#define CAMENC_PROBE_REV      60
#define CAMENC_PROBE_REV_SLOW 120
#define CAMENC_PROBE_AMP      5

/* The orbit the pattern was originally specified with: the square travels a
 * radius of half its own side, so it never leaves the middle, over twice as
 * many frames.  It is here as a comparison rather than as a default, because
 * at two pixels a frame the square barely moves between one frame and the one
 * two before it -- and if the reference being read is stale by a frame, that
 * is exactly the difference the artifact would consist of, so it would be
 * measured as nothing.  See the note on the structure itself.
 */

#define CAMENC_PROBE_RADIUS_SLOW 32

/* The sensor's limits, which the exposure loop needs in order to know where
 * one of the two ways of getting more light runs out and the other has to
 * take over.  These are the OV5647's: exposure in lines, bounded by the
 * mode's frame length; gain a ten-bit fixed-point multiplier whose unity is
 * sixteen.
 *
 * Named here rather than asked for, because the sensor reports neither.  The
 * exposure ceiling is a mode's own frame length less room to read out, which
 * is OV5647_MODE_EXPOSURE_MAX() in the sensor's header -- it moves with the
 * mode, so it is read from the mode in force rather than written down here.
 * The driver clamps to the same figure, so a loop that asks for more is
 * corrected rather than believed.
 */

#define CAMENC_3A_EXPOSURE_MIN 4u

#define CAMENC_3A_GAIN_MIN     16u
#define CAMENC_3A_GAIN_MAX     1023u
#define CAMENC_3A_GAIN_ONE     16u

/* The brightness the loop steers to, on the 0..255 scale the capture driver
 * measures in -- a scale that counts light and not what the eye makes of it.
 *
 * That distinction is the whole of this number.  The counts are proportional
 * to the light that arrived, so a value halfway up the range is not the
 * middle of anything: photographic mid grey is an 18% reflectance and lands
 * at 0.18 * 255, which is 46, and the eye's own half-way point -- sRGB 128 --
 * is 55 in these units.  A target of 128 is more than a stop above mid grey
 * and spends its time in clipping, and the first version of this line asked
 * for 110, which is 43% of full scale and the same mistake made smaller.
 *
 * Sixty is mid grey with about a third of a stop of headroom: bright enough
 * to read a room by, and dark enough that a lamp or a window in the frame
 * stays a minority of the measurement rather than most of it.
 *
 * The quantity being steered is the whole frame's brightness, with any
 * clipped sample counted at the top of the range -- see camenc_3a_measure.
 * So a scene with blown highlights reads brighter than its surviving detail
 * does, which is the point: the figure this is compared against is a claim
 * about the whole picture and not only about the part of it that came back.
 */

#define CAMENC_3A_TARGET_DEFAULT 60u

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

/* The sensor mode this program starts in, as an index into the sensor
 * driver's mode table.
 *
 * This is the application's choice and not the driver's.  The board brings its
 * receive chain up on the mode the sensor driver publishes as its placeholder
 * -- a device node needs a geometry before an application can ask for
 * anything -- but that is not a decision about this run.  A mode is a
 * geometry, a frame rate and a link rate together, and this program names the
 * one it wants to the board before it opens the camera; so the mode a run
 * films in is the one below and nothing else, and the two are deliberately
 * separate settings that need not agree.  When they do not, the cost is one
 * reconfiguration at start-up, which is the same thing a change of mode later
 * costs.
 *
 * Which one it is set to is a compromise between the picture and the pipeline
 * behind it, and is the application's to make.  The seven, and what each is
 * for, are listed in ov5647.h and repeated by -m's line in the usage text;
 * what the entry here is set to is in the line below it and nowhere else.
 *
 * It is the 16:9 geometry at thirty frames a second, and it is the default
 * because of what runs behind the sensor rather than in front of it.  It is
 * the only geometry the encoder needs no compromise for -- 1280 is a multiple
 * of the 64 pixels its reconstruction working set is sized in, where 1296 is
 * not, and 720 is a whole number of 16-row macroblocks, where 1080 is not --
 * so nothing is padded sideways or cropped back, and it is the lightest of the
 * seven on every stage the picture passes through.  A run that is watched
 * live is the one that notices; a page that would rather have a different
 * trade asks for it, and any of the seven can be had that way.
 *
 * It is named by the header's constant rather than by its number so that the
 * two cannot drift apart -- a mode inserted into the table would otherwise
 * silently repoint this at another one.
 */

#define CAMENC_DEFAULT_MODE OV5647_MODE_1280x720_30

/* The board's request to point the capture path at another sensor mode; the
 * argument is a mode index.
 *
 * One past BOARDIOC_USER, which is the last command the framework itself
 * defines.  There is no shared header for this pair, so the two halves name
 * each other: the receiving half is BOARDIOC_CAMERA_SET_MODE in the board's
 * kickpi_k7_boardctl.c, which is also where the reasons for a mode being the
 * board's business rather than this program's are written down.
 */

#define CAMENC_BOARDIOC_SET_MODE (BOARDIOC_USER + 1)

#define CAMENC_DEFAULT_FRAMES    300
#define CAMENC_DEFAULT_QP        26

/* The group length this program streams at.
 *
 * Fifteen pictures, which is half a second at the mode's rate.  The
 * measurement behind it is in chips/rk3576/vepu/README.md; what matters here
 * is the two sides of the trade.
 *
 * The saving is nearly all of what a group can give.  Halving the rate needs
 * only that the group be long compared with how many pictures an IDR is worth,
 * and on this encoder a P is under one per cent of an IDR -- a group of two
 * and a group of four were measured at 3799 and 1937 kbit/s, and the two
 * numbers fit `rate = fps * 8 * (IDR/G + P)` closely enough to say what a
 * longer group does without measuring it.  What that model also says is where
 * the diminishing returns are not: the rate keeps falling with the group,
 * because the term that falls is the IDR's, and it falls as 1/G.
 *
 * What stops being worth it is the other side of the same coin.  A group is
 * also how long a decoder that joined late, or that missed a fragment, has to
 * wait before it can start: the pictures after a missing one predict from a
 * picture the client never received, so what it sees is not one missing frame
 * but everything up to the next picture a stream may start on.  At two that is
 * a fifteenth of a second and nobody notices.  At fifteen it is half a second,
 * which is a hiccup.  At sixty it is two seconds of garbage or black, and the
 * page this is streamed to would also have to be changed: it keeps a second of
 * history, which is two groups at fifteen and would be less than one at sixty.
 *
 * So fifteen, and not higher, for a stream watched live.  A caller that is
 * not watching live -- writing a file, say -- can pass any group it likes.
 */

#define CAMENC_DEFAULT_GOP 15

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
 * client rather than stalling the capture loop.  The limit is logged when it
 * is reached, which is what keeps it from being a silent ceiling.
 *
 * One segment is one access unit plus its fMP4 boxes: a moof, an mdat header
 * and the sample.  The size therefore follows the resolution, and follows the
 * quantiser harder still -- and since the sensor's mode can be changed while
 * the program runs, it follows the mode as well.  The server's transmit
 * buffer is sized once, when the server starts, and is the caller's to
 * choose; so this cannot follow anything.  It is the largest of the modes.
 *
 * 64 KiB was sized for 640x480, where a frame is a few kilobytes.  At
 * 1296x960 with the default quantiser a frame is around 57 KiB and the first
 * IDR reaches 71 KiB, which is larger than the whole buffer the old figure
 * produced -- so no segment fitted and no client could be served at all.  512
 * KiB covers that with room for a much lower quantiser; a segment beyond it
 * drops the client, which is the designed degradation rather than a fault.
 *
 * The largest mode is now 1920x1080, whose pictures are about 1.67 times the
 * area of 1296x960's and so larger by roughly that factor at the same
 * quantiser -- still well inside 512 KiB, and the margin is why the figure
 * does not have to be recomputed when a mode is added.
 *
 * Sizing for the largest mode is paid whichever mode is running, and it is
 * the price of not having to know, when the server starts, which mode it will
 * end up serving.  It is also why a mode change does not have to restart the
 * server: the buffer it takes a segment into is already big enough for any of
 * them.
 */

#define CAMENC_WS_MAX_SEGMENT (512 * 1024)

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

/* Whether the encoder is given the camera's own buffer instead of one of
 * g_out[] with the frame copied into it.
 *
 * The two devices have to agree on the frame's layout for that, and the
 * question is asked once per stream rather than here -- see the comparison
 * beside enc_vstride, which is where both layouts are finally known.  What
 * this flag is for is that the loop has to do one of two quite different
 * things with each frame, and the thing it does not do is the copy that
 * dominates the loop.
 *
 * It is false for a source that is not the camera.  A file or the drawn
 * pattern is a frame in this program's own memory, written by the same code
 * that stores it, and the encoder will not take an address from the system
 * heap at all -- the capture driver's buffers are in the DMA heap, which is
 * the only memory on the part whose addresses are physical.
 */

static bool g_zerocopy;

/* Where the frames come from, when they do not come from the camera.
 *
 * -i names a file of bare NV12 frames, one picture after another with nothing
 * in front of them, and the run then behaves as if the capture device had
 * handed those frames over.  Non-NULL is the whole of the test: it is what
 * decides whether the loop reads a file or dequeues the camera, whether the
 * camera's buffers are mapped or allocated, and whether there is a camera to
 * start at all.
 *
 * What it is for is measurement rather than convenience.  A camera can only
 * show that a picture is wrong; a file can say how wrong and where, because
 * its contents are known in advance -- so what the bitstream shows that the
 * input does not is the encoder's own doing.  It is also the only way to put
 * the same pixels through two encoders, and a comparison between encoders
 * means nothing on two different pictures.
 */

static FILE *g_src_file;
static size_t g_src_frame;

/* Whether g_cam[]'s buffers are this program's own allocation rather than the
 * capture driver's mapping.
 *
 * It is set as soon as the first one is allocated, which is what makes it
 * safe to use on the failure path as well: a source that could not be opened,
 * or could only be half-allocated, still leaves the teardown with a correct
 * answer about how to release whatever is there.
 */

static bool g_src_owned;

/* Where a frame comes from.
 *
 * There are three answers and they are not variations on each other: a camera
 * that is running, a file that is not, and a picture this program draws
 * itself.  The last one exists because the first two cannot both be had at
 * once -- the camera cannot be asked for a picture whose contents are known
 * in advance, and a file of such a picture is as large as the picture, which
 * is 28 MB for four seconds of VGA.  A generator is the same frames with
 * nothing to carry.
 */

enum camenc_source_e
{
  CAMENC_SRC_CAMERA,
  CAMENC_SRC_FILE,
  CAMENC_SRC_PROBE
};

static enum camenc_source_e g_src_kind = CAMENC_SRC_CAMERA;

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

  /* The sensor mode the page has asked for, and whether it has asked.
   *
   * Recorded rather than acted on.  The callback that fills this in runs
   * inside camenc_ws_ready(), which the loop calls from its wait; changing a
   * mode closes both devices, has the board tear down and rebuild the capture
   * chain, and replaces every buffer and the muxer with them -- far more than
   * a callback the loop is in the middle of may do.  So the loop carries it
   * out itself, which is what the second field tells it.
   */

  int32_t want_mode;
  bool mode_pending;
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
 *   mo <index>    the sensor mode to switch to, by its index in the sensor
 *                 driver's mode table
 *
 * Setting a value by hand stops the loop for it rather than moving a slider
 * that the loop will pull back on the next frame.  That is what a user means
 * by dragging it, and it is the difference between a control and a display.
 *
 * A mode is not a value, so it is not set by hand the same way; what the two
 * have in common is that both are recorded here and applied by the loop, and
 * that the status line reports them under the same names that set them.  The
 * status line reports the mode in force, which is what the menu shows and
 * what the next command is a move away from.
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
  else if (camenc_ui_arg(text, "mo", &value))
    {
      /* Range is the board's to check, and it will: it refuses an index that
       * names no mode before it takes anything down.  Refusing here as well
       * would need this file to know how many modes there are, which is the
       * sensor's business and not the page protocol's.
       */

      ui->want_mode = value;
      ui->mode_pending = true;
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
                                  uint32_t frame, uint32_t mode)
{
  char line[CAMENC_STATUS_MAX];
  size_t n;

  n = camenc_3a_status(a, line, sizeof(line));
  if (n == 0u)
    {
      return;
    }

  /* The sensor mode travels in the same line rather than in one of its own.
   * It is a setting like the others -- the page has a control for it and the
   * control has to show what is in force -- and a second line would be a
   * second thing to keep in step with the first, including in the comparison
   * below that decides when to send.
   */

  if (n + sizeof(" mo=4294967295") < sizeof(line))
    {
      int m = snprintf(line + n, sizeof(line) - n, " mo=%" PRIu32, mode);

      if (m > 0)
        {
          n += (size_t)m;
        }
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
  printf("              [-m mode] [-q qp] [-x exposure]"
         " [-g gain]\n");
  printf("  -d  capture device (default %s)\n", CAMENC_CAM_DEVPATH);
  printf("  -e  encoder device (default %s)\n", CAMENC_ENC_DEVPATH);
  printf("  -o  write fragmented MP4 here (default none, encode only)\n");
  printf("  -i  read the frames from this file of bare NV12 frames instead\n");
  printf("      of from the camera, which is how a picture whose contents\n");
  printf(
      "      are known exactly is put through the encoder with no sensor,\n");
  printf("      lens, focus or scene in the way\n");
  printf(
      "  -S  encode a picture this program draws itself instead of one it\n");
  printf("      is given: a flat background with one 64x64 dark square\n");
  printf("      moving slowly round it, and +-5 of noise over both.  Same\n");
  printf("      purpose as -i, with nothing to carry and a known answer\n");
  printf("      in every 16x16 block.  The name picks the background:\n");
  printf("      probe (green, %d frames/rev), literal (green, the original\n",
         CAMENC_PROBE_REV);
  printf("      slower orbit), gray, red, blue, magenta.  See the source\n");
  printf("      for why green and why the orbit is the speed it is\n");
  printf("  -R  orbit radius for -S, in pixels (default %d)\n",
         CAMENC_PROBE_RADIUS);
  printf("  -N  noise amplitude for -S, 0..64 (default %d); 0 leaves the\n",
         CAMENC_PROBE_AMP);
  printf("      background perfectly flat\n");
  printf("  -r  rate to hand the frames over at, in fps, for -i and -S\n");
  printf(
      "      (default the mode's own, to keep the encoder's timing as it\n");
  printf("      would be; 0 runs it back to back, which is faster and is a\n");
  printf("      different timing environment)\n");
  printf("  -l  start the source again when it runs out, for a run longer\n");
  printf("      than it is.  A defect that is intermittent needs chances\n");
  printf("      rather than length, and this is how a short source stands\n");
  printf("      for a long run -- see -n\n");
  printf("  -p  also serve the stream on this port as WebSocket"
         " (default %d, 0 for none)\n",
         CAMENC_DEFAULT_PORT);
  printf("  -n  frames to encode (default %d)\n", CAMENC_DEFAULT_FRAMES);
  printf("  -m  sensor mode, by its index in the sensor driver's mode"
         " table\n");
  printf("      (default %d: 0 = 1920x1080 at 15 fps, 1 = 1296x960 at"
         " 30 fps,\n      2 = 1296x960 at 15 fps, 3 = 1280x720 at 30 fps,"
         " 4 = 1280x720 at\n      15 fps, 5 = 640x480 at 60 fps,"
         " 6 = 640x480 at 30 fps)\n",
         CAMENC_DEFAULT_MODE);
  printf("  -q  quantiser, 0..51 (default %d)\n", CAMENC_DEFAULT_QP);
  printf("  -G  pictures per group, 1..1000 (default %d; 1 makes every"
         " frame an\n      IDR, a larger value makes the first frame of"
         " each group one and\n      the rest predict from the frame"
         " before them.  A P picture costs\n      under one per cent of"
         " an IDR on this encoder, so the rate is\n      roughly"
         " proportional to the group's length -- but a group is also"
         " how\n      long a client that joins late waits for a picture it"
         " can start on)\n",
         CAMENC_DEFAULT_GOP);
  printf(
      "  -W  give the encoder the leftmost W columns of the mode's picture\n"
      "      rather than all of them (W a multiple of 16).  The encoder's\n"
      "      reconstruction working set is sized in units of 64 pixels, so\n"
      "      a mode whose width is not a multiple of 64 hands it a padded\n"
      "      picture; this is how the two are told apart\n");
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

/* Ask for a frame rate.
 *
 * This is not decoration, and leaving it out is not a small omission: the
 * sensor's frame size and its frame rate are two separate halves of what
 * picks one of its modes, and this is the only call that states the second
 * half.  A capture stream that never makes it runs at whatever rate the
 * sensor driver lists first -- which for the OV5647 is the fastest of the
 * modes with that geometry -- so the modes that differ only in frame rate
 * are not merely hard to reach, they are unreachable, while every log line
 * and every menu entry goes on naming the one that was asked for.
 *
 * The framework validates this against the format in force and refuses it
 * unless the stream is stopped, so it belongs after the format and before
 * the buffers.
 */

static int camenc_set_interval(int fd, uint32_t type, uint16_t fps,
                               FAR const char *what)
{
  struct v4l2_streamparm parm;

  memset(&parm, 0, sizeof(parm));
  parm.type = type;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = fps;

  if (ioctl(fd, VIDIOC_S_PARM, (unsigned long)&parm) < 0)
    {
      printf("camenc: %s VIDIOC_S_PARM to %u fps failed: %d\n", what,
             (unsigned int)fps, errno);
      return -errno;
    }

  /* The driver's answer, not the request: a sensor that cannot run at the
   * rate asked for is entitled to say so, and the frame rate decides how long
   * an exposure may be, so the two must not disagree.
   */

  if (parm.parm.capture.timeperframe.denominator != fps)
    {
      printf("camenc: %s runs at %" PRIu32 " fps, not the %u asked for\n",
             what, parm.parm.capture.timeperframe.denominator,
             (unsigned int)fps);
    }

  return 0;
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

/* Move one NV12 frame from the camera's buffer to the encoder's.
 *
 * The two sides describe a frame the same way and lay it out differently.  The
 * camera packs its rows and puts the chroma plane directly after the picture's
 * own height; the encoder puts it after its *allocated* height, which is the
 * picture rounded up to its macroblock grid.  Those are the same number for
 * every mode whose height is already on that grid, and not for all of them:
 * 1080 rows is not a multiple of sixteen, so the encoder allocates 1088.  A
 * chroma plane written at the picture height in that case sits eight rows
 * above the one the encoder reads, which leaves the last eight rows of the
 * encoder's chroma holding whatever the buffer last had -- and the picture
 * comes out with a band of that at the bottom.  dst_vstride is the allocated
 * height, and it is what the destination's plane offset is computed from.
 *
 * When the strides agree and there is no padding this is one block copy of the
 * whole frame.  A crop -- the encoder given fewer columns than the mode has --
 * makes the strides differ, and a block copy would then shear the picture
 * instead of cropping it, so the rows are copied one at a time.  The chroma
 * plane is the same byte width as the luma's and half as many rows, which is
 * what 4:2:0 sampling is.
 *
 * The return value is what the caller reports as the frame's length: all of
 * the destination, padding included, because that is what the encoder reads --
 * its luma plane is dst_vstride rows tall and its chroma follows it.
 */

static size_t camenc_copy_frame(FAR uint8_t *dst, FAR const uint8_t *src,
                                uint32_t dst_stride, uint32_t dst_vstride,
                                uint32_t src_stride, uint32_t width,
                                uint32_t height)
{
  size_t total = (size_t)dst_stride * dst_vstride * 3u / 2u;
  uint32_t rows = height / 2u;
  uint32_t r;

  if (dst_stride == src_stride && dst_vstride == height && width == src_stride)
    {
      memcpy(dst, src, total);
      return total;
    }

  for (r = 0; r < height; r++)
    {
      memcpy(dst + (size_t)r * dst_stride, src + (size_t)r * src_stride,
             width);
    }

  dst += (size_t)dst_vstride * dst_stride;
  src += (size_t)height * src_stride;

  for (r = 0; r < rows; r++)
    {
      memcpy(dst + (size_t)r * dst_stride, src + (size_t)r * src_stride,
             width);
    }

  return total;
}

/* Open the file that stands in for the camera, and give the loop the buffers
 * the capture driver would otherwise have handed it.
 *
 * A frame in the file is the same bytes the camera's buffers hold, so the only
 * thing that can be checked about the file is that it is a whole number of
 * them: its length is what says whether the geometry is the one that was
 * asked for, and a length that is not a whole number of frames means it is
 * not.
 */

static int camenc_source_open(FAR const char *path, size_t frame_size,
                              FAR size_t *frames_in_file)
{
  long long size;
  size_t n;
  int k;

  g_src_file = fopen(path, "rb");
  if (g_src_file == NULL)
    {
      printf("camenc: cannot open %s: %d\n", path, errno);
      return -errno;
    }

  if (fseek(g_src_file, 0, SEEK_END) != 0 ||
      (size = (long long)ftell(g_src_file)) < 0 ||
      fseek(g_src_file, 0, SEEK_SET) != 0)
    {
      printf("camenc: cannot measure %s: %d\n", path, errno);
      goto bad;
    }

  if ((size_t)size < frame_size || (size_t)size % frame_size != 0)
    {
      printf("camenc: %s is %lld bytes, which is not a whole number of"
             " %zu-byte NV12 frames for this mode\n",
             path, size, frame_size);
      goto bad;
    }

  n = (size_t)size / frame_size;
  *frames_in_file = n;

  /* One buffer per slot the camera would have offered, so that the rotation
   * through them, the copy into the encoder, and everything after that are
   * the code they already were.
   */

  g_src_owned = true;

  for (k = 0; k < CAMENC_BUFFERS; k++)
    {
      g_cam[k].start = malloc(frame_size);
      if (g_cam[k].start == NULL)
        {
          printf("camenc: no room for a %zu-byte source frame\n", frame_size);
          goto bad;
        }

      g_cam[k].length = frame_size;
    }

  g_src_frame = frame_size;
  return OK;

bad:
  fclose(g_src_file);
  g_src_file = NULL;
  return -EINVAL;
}

/* A picture this program draws itself.
 *
 * What it is for
 * --------------
 * A camera can show that a picture is wrong; it cannot say how wrong, because
 * there is nothing to compare it against.  This pattern can, because its
 * contents are known exactly: a flat background with one dark square on it,
 * so every 16x16 block's correct value is known before the encoder sees it
 * and any block that comes back different is a number rather than an
 * impression.
 *
 * The three decisions in it that are not free choices
 * ---------------------------------------------------
 * 1. The background is a flat colour and it is green.  Flat, because a defect
 *    that shifts a region's level is otherwise indistinguishable from the
 *    picture; green, because the offset a colour defect usually amounts to
 *    (+Cb, +Cr) is the direction green is furthest from -- in YCbCr the
 *    inverse transform is R = Y + 1.402 (Cr-128) and B = Y + 1.772 (Cb-128),
 *    so a green background has the most to lose in both.  The square is dark
 *    for the same reason: it is where those differences are largest relative
 *    to its own luma, and a small chroma error on a dark, saturated region
 *    reads as a saturated magenta block.
 *
 * 2. The square moves far between one frame and the frame before it.  A
 *    two-slot reference pair -- which is what this encoder is given -- means
 *    the slot read for frame N last held frame N-2, so if the reference being
 *    read is stale by one frame, what appears on screen is the difference
 *    between the two pictures.  A square that barely moves makes that
 *    difference vanish, and the defect with it.
 *
 * 3. The noise is why the background is not free to code.  A perfectly flat
 *    picture can be predicted perfectly by copying one block, so a defect
 *    that only appears when the encoder has real work to do would not appear
 *    at all.  +-5 is small enough that a block's mean barely moves and large
 *    enough that nothing is exactly flat.
 *
 * The circle is walked by rotating a vector rather than by calling a sine,
 * which is one multiply per frame instead of a linkage to a maths library,
 * and is exact enough that the orbit does not spiral: the step is rounded to
 * twenty fractional bits, so the radius is short by about one part in a
 * million over a whole revolution.
 */

struct camenc_probe_s
{
  uint32_t y; /* background luma */
  uint32_t u; /* background chroma, both planes  */
  uint32_t v;
  int cx; /* orbit centre */
  int cy;
  int radius;   /* orbit radius, pixels */
  int side;     /* square side, pixels */
  int side_y;   /* square luma */
  int amp;      /* noise amplitude, +- this */
  int32_t c;    /* cosine of the angle, Q20 */
  int32_t s;    /* sine of the angle, Q20 */
  int32_t dc;   /* cosine of one step, Q20 */
  int32_t ds;   /* sine of one step, Q20 */
  uint32_t rng; /* the noise's own state */
  uint32_t frame;
};

/* The named backgrounds, as RGB.  Green is the default and the reason is in
 * the structure's comment; the others are there to be compared against it,
 * because a defect that is really an offset in one chroma plane shows up on
 * one of these and not on the others.
 */

static const char *const g_probe_bg_name[] = { "probe", "literal", "gray",
                                               "red",   "blue",    "magenta" };

static const int g_probe_bg_rgb[][3] = {
  { 0, 255, 0 },     /* probe: green */
  { 0, 255, 0 },     /* literal: green, the slower orbit */
  { 128, 128, 128 }, /* gray: no chroma to shift */
  { 255, 0, 0 },     { 0, 0, 255 }, { 255, 0, 255 }
};

#define CAMENC_PROBE_NB (sizeof(g_probe_bg_name) / sizeof(g_probe_bg_name[0]))

/* The orbit and the square.  The square is 64 pixels because a 16-pixel block
 * is what a defect is measured in and four of them are wide enough to tell a
 * region from an edge block; its luma is 32 rather than the 16 that would be
 * black, because a +-5 noise added to 16 is clipped on the way up and that
 * would put a bias of its own on the very quantity being measured.  The
 * numbers themselves are at the top of this file.
 */

static struct camenc_probe_s g_probe;

/* BT.601 studio swing, in integers.
 *
 * The coefficients are the standard ones -- for luma 0.299/0.587/0.114 scaled
 * by 219/255, for chroma the usual -0.168736/-0.331264/0.5 and its pair --
 * taken to twelve fractional bits.  Eight bits, which is the usual choice, is
 * not enough here: at 66/129/25 a green background comes out at luma 144
 * where the arithmetic it stands for says 145, and the host side is given
 * these numbers rather than a picture.  At twelve bits every background this
 * program offers agrees with the float form to the count, which is what makes
 * the board's frames and the host's comparable at all.
 */

static void camenc_probe_rgb(int r, int g, int b, uint32_t *y, uint32_t *u,
                             uint32_t *v)
{
  *y = (uint32_t)(16 + ((1052 * r + 2065 * g + 401 * b + 2048) >> 12));
  *u = (uint32_t)(128 + ((-607 * r - 1192 * g + 1799 * b + 2048) >> 12));
  *v = (uint32_t)(128 + ((1799 * r - 1506 * g - 293 * b + 2048) >> 12));
}

/* Round to the nearest even coordinate.
 *
 * The square is 64 pixels and even, and its centre is even, so no 2x2 chroma
 * sample ever straddles its edge and the chroma of an edge block is exactly
 * one colour rather than a mixture.  Without it every edge block's expected
 * value would depend on the square's position within the pixel grid, which is
 * the one thing the measurement cannot afford.
 */

static int camenc_probe_even(int v)
{
  return v >= 0 ? (v + 1) & ~1 : -((-v + 1) & ~1);
}

static uint32_t camenc_probe_rand(struct camenc_probe_s *p)
{
  uint32_t x = p->rng;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  p->rng = x;
  return x;
}

static uint8_t camenc_probe_clamp(int v)
{
  return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void camenc_probe_open(struct camenc_probe_s *p, int which, int radius,
                              int amp, uint32_t width, uint32_t height)
{
  int r = g_probe_bg_rgb[which][0];
  int g = g_probe_bg_rgb[which][1];
  int b = g_probe_bg_rgb[which][2];
  int rev = which == 1 ? CAMENC_PROBE_REV_SLOW : CAMENC_PROBE_REV;
  int rad = which == 1 ? CAMENC_PROBE_RADIUS_SLOW : CAMENC_PROBE_RADIUS;

  memset(p, 0, sizeof(*p));

  camenc_probe_rgb(r, g, b, &p->y, &p->u, &p->v);

  p->cx = (int)width / 2;
  p->cy = (int)height / 2;
  p->radius = radius > 0 ? radius : rad;
  p->side = CAMENC_PROBE_SIDE;
  p->side_y = CAMENC_PROBE_SIDE_Y;
  p->amp = amp;

  /* The angle starts at zero and the step is a constant, so the orbit is
   * walked by rotating a vector: one multiply and one add per frame, no
   * library call, and no angle to reduce.  The step is cos and sin of a whole
   * revolution divided by the frames it takes, rounded to twenty fractional
   * bits -- 6 degrees for the short way round and 3 for the long one, which
   * are the only two rates this is used at.
   */

  {
    static const int32_t step[][2] = {
      { 1042766, 109596 }, /* 6 degrees, Q20 */
      { 1047154, 54841 }   /* 3 degrees, Q20 */
    };

    int k = rev == CAMENC_PROBE_REV ? 0 : 1;

    p->c = 1 << 20;
    p->s = 0;
    p->dc = step[k][0];
    p->ds = step[k][1];
  }

  p->rng = 0x5eed1234u * (uint32_t)(which + 1);
}

/* Fill one NV12 frame of the pattern.
 *
 * The background is one constant and the square is another, so most of this
 * is two fills; the noise is the only per-byte work, and it is applied to the
 * whole picture rather than to the background alone so that the square's own
 * edges are not the only thing the encoder has to code.
 */

static void camenc_probe_fill(struct camenc_probe_s *p, uint8_t *buf,
                              uint32_t width, uint32_t height)
{
  uint32_t ysize = width * height;
  uint32_t uvsize = ysize / 2;
  int half = p->side / 2;
  int64_t oc = (int64_t)p->c * p->radius >> 20;
  int64_t os = (int64_t)p->s * p->radius >> 20;
  int x0 = camenc_probe_even(p->cx + (int)oc - half);
  int y0 = camenc_probe_even(p->cy + (int)os - half);
  int x1 = x0 + p->side;
  int y1 = y0 + p->side;
  uint32_t r;
  uint32_t i;

  /* Clipped rather than refused: the orbit is inside the picture for both of
   * the rates this is used with, and a square that ran off the edge would
   * change its own uncovered area from frame to frame, which is exactly the
   * kind of unmodelled movement this pattern exists to avoid.  The clamp
   * keeps the arithmetic honest if the radius is asked to be larger.
   */

  if (x0 < 0)
    {
      x0 = 0;
    }

  if (y0 < 0)
    {
      y0 = 0;
    }

  if (x1 > (int)width)
    {
      x1 = (int)width;
    }

  if (y1 > (int)height)
    {
      y1 = (int)height;
    }

  /* Luma: the background, then the square's rows over it. */

  memset(buf, (int)p->y, ysize);

  for (r = (uint32_t)y0; r < (uint32_t)y1; r++)
    {
      memset(buf + (size_t)r * width + (uint32_t)x0, p->side_y,
             (size_t)(x1 - x0));
    }

  /* Chroma: one sample covers two rows, so a chroma row is the square's
   * colour only when both of its rows are inside it -- which is what halving
   * the bounds does, and why the square's sides are even.
   */

  {
    uint8_t *uv = buf + ysize;

    for (i = 0; i < uvsize; i += 2)
      {
        uv[i] = (uint8_t)p->u;
        uv[i + 1] = (uint8_t)p->v;
      }

    for (r = (uint32_t)(y0 / 2); r < (uint32_t)(y1 / 2); r++)
      {
        uint8_t *row = uv + (size_t)r * width;

        for (i = (uint32_t)x0; i < (uint32_t)x1; i += 2)
          {
            row[i] = 128;
            row[i + 1] = 128;
          }
      }
  }

  if (p->amp > 0)
    {
      /* Noise on both planes.  Four samples come out of each state step, so
       * this is a quarter of the work it would otherwise be, and nothing
       * about the sequence matters -- only that no two frames are given the
       * same one, which a longer run cannot repeat.
       */

      int amp = p->amp;
      size_t n = (size_t)ysize + uvsize;

      for (i = 0; i < n; i += 4)
        {
          uint32_t v = camenc_probe_rand(p);
          size_t k;

          for (k = 0; k < 4 && i + k < n; k++)
            {
              buf[i + k] = camenc_probe_clamp(
                  (int)buf[i + k] + (int)(v & 0xffu) % (2 * amp + 1) - amp);
              v >>= 8;
            }
        }
    }

  /* One step round the circle, and the same rotation for the next frame. */

  {
    int64_t c = ((int64_t)p->c * p->dc - (int64_t)p->s * p->ds) >> 20;
    int64_t s = ((int64_t)p->s * p->dc + (int64_t)p->c * p->ds) >> 20;

    p->c = (int32_t)c;
    p->s = (int32_t)s;
  }

  p->frame++;
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

/* Ask the encoder to take its input frames from addresses this program names
 * rather than from a heap of its own.
 *
 * The encoder allocates its buffers in the DMA heap and, until this, refused
 * anything else -- correctly, because the MMU it programs is in pass-through
 * and an address from the system heap is neither physical nor contiguous.
 * The capture driver's frames are in that same heap, so they are addresses it
 * can be given; this is what says it will accept one, and it is what makes the
 * copy between the two devices unnecessary.
 *
 * The number of containers is still requested, and still bounds how many
 * frames may be in flight.  What is not allocated is the backing store: the
 * addresses arrive with each frame instead.
 */

static int camenc_setup_import(int fd, uint32_t type, int count,
                               FAR uint32_t *granted, FAR const char *what)
{
  struct v4l2_requestbuffers req;

  memset(&req, 0, sizeof(req));
  req.count = (uint32_t)count;
  req.type = type;
  req.memory = V4L2_MEMORY_USERPTR;

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

  *granted = req.count;
  return 0;
}

/* Queue one frame the encoder is to read from an address given here.
 *
 * Built from scratch rather than kept, because there is nothing to keep: a
 * QUERYBUF on this side reports the container's own address, which for an
 * imported buffer is not where the frame is.  The address is the frame's and
 * it is what the encoder reads -- see camenc_setup_import().
 */

static int camenc_queue_import(int fd, uint32_t type, FAR uint8_t *addr,
                               int index, FAR const char *what)
{
  struct v4l2_buffer q;

  memset(&q, 0, sizeof(q));
  q.type = type;
  q.memory = V4L2_MEMORY_USERPTR;
  q.index = (uint32_t)index;
  q.m.userptr = (unsigned long)(uintptr_t)addr;

  if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&q) < 0)
    {
      printf("camenc: %s VIDIOC_QBUF %d failed: %d\n", what, index, errno);
      return -errno;
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
      /* EAGAIN is not a fault and is not reported: a descriptor opened
       * non-blocking answers this way when nothing is ready, which is a
       * question the caller asked rather than a device that failed -- see
       * camenc_wait() and the camera source below.
       */

      if (errno == EAGAIN)
        {
          return -EAGAIN;
        }

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

/* How long the loop is willing to wait when nothing at all is happening.
 *
 * A bound rather than a polling interval, and the difference matters.  A
 * waiting poll costs nothing: the task is suspended on a semaphore and is not
 * scheduled again until an event posts it or this expires.  It will not
 * normally expire at all, because the camera has a frame ready thirty times a
 * second whatever this says.  What it is for is the case where the camera has
 * stopped too, so that a board that has gone quiet still comes back round
 * instead of sitting in the wait for ever.
 */

#define CAMENC_WAIT_MS 500

/* The descriptors one wait can cover: the camera or the encoder, the
 * listening socket, and one per client.  See camenc_ws_fds().
 *
 * One short of this does not fail: it silently stops asking about the last
 * client, which is a viewer that never receives anything and never goes away.
 */

#define CAMENC_WAIT_FDS (CAMENC_WS_MAX_CLIENTS + 2)

/* Wait for the next thing to do, and do the part of it the server owns.
 *
 * This is the only place the loop sleeps, and it sleeps on everything that can
 * wake it at once: whatever the frame in progress is waiting for, and every
 * socket the server has.  What the frame is waiting for depends on where it
 * is.  With no frame in flight that is the camera's next frame; with one it is
 * the encoder, which is where the job that was started when the frame was
 * queued reports back -- POLLIN when the bitstream is ready to be taken and
 * POLLOUT when the input container has been returned.
 *
 * The camera is deliberately not waited for while a frame is in flight.  The
 * next frame cannot be taken until this one is out of the way, and in the
 * modes that encode where the frame lies the buffer the camera would hand over
 * is the one the encoder is reading.
 *
 * Returns true when the camera has a frame waiting to be taken, which is only
 * ever true in the first of the two states.  A source that is not the camera
 * has its own pacing and is never waited for; for those, and for a frame in
 * flight, this only serves the sockets and returns false, which the paths that
 * do not need a camera frame ignore.
 */

static bool camenc_wait(int camfd, int encfd, bool inflight)
{
  struct pollfd fds[CAMENC_WAIT_FDS];
  int nwait = 0;
  int n;
  uint64_t mark;

  if (inflight)
    {
      fds[0].fd = encfd;
      fds[0].events = POLLIN | POLLOUT;
      fds[0].revents = 0;
      nwait = 1;
    }
  else if (camfd >= 0)
    {
      fds[0].fd = camfd;
      fds[0].events = POLLIN;
      fds[0].revents = 0;
      nwait = 1;
    }

  n = nwait + camenc_ws_fds(&g_ws, &fds[nwait], CAMENC_WAIT_FDS - nwait);

  mark = camenc_now_us();

  if (poll(fds, (nfds_t)n, nwait != 0 ? CAMENC_WAIT_MS : 0) < 0 &&
      errno != EINTR)
    {
      /* Nothing to say and nothing to do about it: the loop has no frame
       * either way, and the next round asks again.
       */

      return false;
    }

  /* The wait is accounted as the camera stage, which is what that stage has
   * always measured -- the time the loop spends with nothing to do.  It now
   * covers the encode as well, because that wait is made here rather than
   * inside the queue operation; the dequeue that follows no longer waits, so
   * the figure stays comparable with runs that measured a blocking one.
   */

  camenc_stage_account(CAMENC_STAGE_CAMERA, mark);

  mark = camenc_now_us();

  camenc_ws_ready(&g_ws, &fds[nwait], n - nwait);

  camenc_stage_account(CAMENC_STAGE_SERVE, mark);

  return !inflight && nwait != 0 && (fds[0].revents & POLLIN) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *camdev = CAMENC_CAM_DEVPATH;
  FAR const char *encdev = CAMENC_ENC_DEVPATH;
  FAR const char *infile = NULL;
  FAR const char *outfile = NULL;
  FAR const struct ov5647_mode_s *cur = NULL;
  struct camenc_stream_s st;
  struct camenc_sink_s sink;
  bool serving = false;
  bool codec_set = false;
  struct v4l2_format fmt;
  int out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  int cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int cam_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  FAR uint8_t *scratch = NULL;
  uint32_t mode = CAMENC_DEFAULT_MODE;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t nbuf_cam = 0;
  uint32_t nbuf_out = 0;
  uint32_t nbuf_cap = 0;

  /* How much of the picture the encoder is given.  Zero asks for all of it,
   * which is the ordinary case; anything else is the leftmost that many
   * columns, and exists so that a mode whose width is not the multiple of 64
   * the encoder's working set is sized in can be compared against one that is
   * without a sensor mode to match.
   */

  uint32_t enc_width_req = 0;
  uint32_t enc_width = 0;
  uint32_t enc_stride = 0;
  uint32_t enc_vstride = 0;
  uint32_t cam_stride = 0;
  size_t src_size;
  size_t max_au;
  int frames = CAMENC_DEFAULT_FRAMES;
  int qp = CAMENC_DEFAULT_QP;
  int gop = CAMENC_DEFAULT_GOP;
  int file_rate = -1;
  int file_frame = 0;
  uint64_t file_t0 = 0;
  bool loop_source = false;
  bool synthetic = false;
  int probe_bg = 0;
  int probe_radius = 0;
  int probe_amp = CAMENC_PROBE_AMP;
  int port = 0;
  int exposure = -1;
  int gain = -1;
  bool threea_wanted = CAMENC_3A_DEFAULT != 0;
  bool threea = CAMENC_3A_DEFAULT != 0;
  uint32_t target = CAMENC_3A_TARGET_DEFAULT;
  int fd3a = -1;
  bool threea_started = false;
  struct camenc_3a_s threea_state;
  int camfd = -1;
  int encfd = -1;
  int opt;
  int ret;
  int i;
  int k;
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

  /* When the page asked for the current change of mode.  A change of mode is
   * the one path in this program that answers a request rather than a clock,
   * so it is the one path whose duration is not implied by anything else, and
   * a switch that takes seconds is otherwise indistinguishable from a board
   * that has gone away -- the server this task also runs stops answering while
   * it waits.  Reported in stages so that the answer is which stage.
   */

  uint64_t switch_t0 = 0;

  /* Whether a stream has already seeded the page's controls, and whether one
   * has converged a white balance worth carrying into the next.  Both are
   * about what a change of mode should keep: the settings a person chose, and
   * the colour of the light, neither of which the frame size has anything to
   * do with.
   */

  bool ui_seeded = false;
  bool wb_converged = false;

  /* Whether this pass ended because the page asked for another mode, as
   * opposed to running out of frames or failing.
   */

  bool switching = false;

  /* Whether the board has been told which mode this run wants the receive
   * chain to be configured for.  It is told once, before the first stream
   * opens the camera; a later change of mode goes through the same board
   * command but from the other end of the stream loop, where a refusal is
   * recoverable rather than fatal -- which is why the two are not one call
   * site.
   */

  bool board_told = false;

  while ((opt = getopt(argc, argv, "d:e:i:lS:o:n:m:q:x:g:p:G:A:t:W:r:R:N:")) !=
         -1)
    {
      switch (opt)
        {
          case 'd':
            camdev = optarg;
            break;

          case 'e':
            encdev = optarg;
            break;

          case 'i':
            infile = optarg;
            break;

          case 'S':
            for (probe_bg = 0; probe_bg < (int)CAMENC_PROBE_NB; probe_bg++)
              {
                if (strcmp(optarg, g_probe_bg_name[probe_bg]) == 0)
                  {
                    break;
                  }
              }

            if (probe_bg >= (int)CAMENC_PROBE_NB)
              {
                printf("camenc: -S %s is not one of the backgrounds:", optarg);

                for (probe_bg = 0; probe_bg < (int)CAMENC_PROBE_NB; probe_bg++)
                  {
                    printf(" %s", g_probe_bg_name[probe_bg]);
                  }

                printf("\n");
                return EXIT_FAILURE;
              }

            synthetic = true;
            break;

          case 'R':
            probe_radius = atoi(optarg);
            if (probe_radius <= 0)
              {
                printf("camenc: the orbit radius must be positive\n");
                return EXIT_FAILURE;
              }
            break;

          case 'N':
            probe_amp = atoi(optarg);
            if (probe_amp < 0 || probe_amp > 64)
              {
                printf("camenc: the noise amplitude must be 0..64\n");
                return EXIT_FAILURE;
              }
            break;

          case 'l':
            loop_source = true;
            break;

          case 'r':
            file_rate = atoi(optarg);
            if (file_rate < 0)
              {
                printf("camenc: the source rate cannot be negative\n");
                return EXIT_FAILURE;
              }
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

          case 'm':
            mode = (uint32_t)atoi(optarg);
            break;

          case 'q':
            qp = atoi(optarg);
            break;

          case 'G':
            gop = atoi(optarg);
            break;

          case 'W':
            enc_width_req = (uint32_t)atoi(optarg);
            break;

          case 'x':
            exposure = atoi(optarg);
            break;

          case 'g':
            gain = atoi(optarg);
            break;

          case 'A':
            threea_wanted = atoi(optarg) != 0;
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

  /* The mode has to be one the sensor has, because it is what the board is
   * asked to configure the receive chain for and what the geometry of every
   * buffer follows from.  Checking it here rather than leaving it to the board
   * keeps a typo from getting as far as taking a running stream down.
   */

  cur = ov5647_mode(mode);
  if (cur == NULL)
    {
      printf("camenc: mode %" PRIu32 " is not one of the sensor's\n", mode);
      camenc_usage();
      return EXIT_FAILURE;
    }

  width = cur->width;
  height = cur->height;

  /* The encoder works on whole macroblocks, so a geometry that is not a
   * multiple of sixteen is not refused but padded: the driver rounds both
   * dimensions up to its grid and tells a decoder to crop the padding back
   * off again, in the sequence parameter set.  What is left for this program
   * is to size its own buffers for the padded height, which it does from the
   * encoder's own reported stride and sizeimage further down -- see
   * enc_vstride.  The 1920x1080 mode is the one that needs it: 1080 rows is
   * eight short of its 1088-row grid, and the mode's own register table says
   * why the remainder is left for the rest of the path rather than trimmed
   * in the sensor.
   *
   * A width off the grid is still refused, and the difference is not an
   * oversight.  A padded height is rows the encoder reads out of memory it
   * allocated; a padded width would be columns of a row the camera hands
   * over, and the copy between the two is where the two sides have to agree
   * byte for byte.  Every mode the sensor offers has a width that is a
   * multiple of sixteen -- 1920, 1296, 1280 and 640 -- so this costs nothing
   * today and is the check that would catch a mode that broke it.
   */

  if (frames <= 0 || (width & 15u) != 0)
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
  /* The server, and the file                                          */
  /* ---------------------------------------------------------------- */

  /* Both are set up once for the whole run rather than once per stream.
   *
   * The server is what a change of mode arrives through, so stopping it in
   * order to change modes would be a way of never being able to change modes
   * again; and its transmit buffer is already sized for the largest mode, so
   * nothing about it depends on which one is running.  The connections
   * themselves are dropped when the mode changes -- see the end of the stream
   * loop -- because the browser has to rebuild its demuxer around a new frame
   * size, and it reconnects by itself.
   *
   * The file stays open across a change of mode, so a run that switches modes
   * while writing one writes two streams into it, each with its own
   * initialisation segment.  That is not one file a player can follow, and the
   * alternative -- quietly starting the file again -- would throw away what
   * had already been written.  In practice the two do not meet: a change of
   * mode only ever arrives from a page, and a page is only being served when
   * the file is not the point of the run.
   */

  if (port > 0)
    {
      /* Room for the largest segment, and for the initialisation segment that
       * goes to a client which has just connected.
       *
       * Both are in a client's buffer at the same time whenever its socket is
       * slow to drain, so it is their sum rather than either one: a buffer
       * sized for one segment alone is a buffer that drops a client on the
       * frame that follows its handshake, which is what this used to do.
       */

      ret = camenc_ws_start(&g_ws, (uint16_t)port,
                            CAMENC_WS_INIT_MAX + CAMENC_WS_MAX_SEGMENT +
                                CAMENC_WS_TX_HEADER);
      if (ret < 0)
        {
          printf("camenc: cannot serve on port %d: %d\n", port, ret);
          return EXIT_FAILURE;
        }

      serving = true;
      sink.ws = &g_ws;

      /* Control messages from the page land here.  Nothing is applied in the
       * callback -- see camenc_ui_s.
       */

      camenc_ws_set_command(&g_ws, camenc_ui_command, &g_ui);

      printf("serving:  http://<board address>:%d/  (%u clients at most,\n"
             "          %u-byte segments)\n",
             port, CAMENC_WS_MAX_CLIENTS, CAMENC_WS_MAX_SEGMENT);
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

  /* ---------------------------------------------------------------- */
  /* One pass of what follows is one stream                            */
  /* ---------------------------------------------------------------- */

  /* It is reached again when the page asks for another sensor mode, which is a
   * different stream in every way that matters -- a different frame size,
   * stride, buffer set, muxer and initialisation segment -- so the setup below
   * is written to be run more than once rather than to be the start of the
   * program.  Frames are counted across the passes, so changing modes does not
   * extend the run.
   */

stream_start:
  threea = threea_wanted;

  /* Whatever a previous pass left behind that a stale value would make
   * dangerous.  The descriptors are the ones that matter: closing one that has
   * already been closed closes whatever the task that has since taken the
   * number has open, and device numbers are handed out again as soon as they
   * are freed.
   */

  camfd = -1;
  encfd = -1;
  fd3a = -1;
  threea_started = false;
  codec_set = false;
  have_prev = false;

  /* `last_report` is set to the frame the stream starts on rather than to
   * zero, because frames are counted across streams and the first report has
   * to be over a window that belongs to this stream.  `reported_at` is zero
   * for the opposite reason: the timestamps restart with the stream, so the
   * first span is the time since its own first frame. */

  last_report = i;
  reported_at = 0;
  encoded = 0;
  dropped = 0;
  win_idr = 0;
  win_p = 0;
  win_idr_bytes = 0;
  win_p_bytes = 0;
  camenc_stage_reset();

  cur = ov5647_mode(mode);
  width = cur->width;
  height = cur->height;

  /* The encode width, clamped to this mode.  A change of mode can make it too
   * wide -- the page's menu moves between a 1920-column mode and a 640-column
   * one -- and the right answer then is the whole picture rather than a
   * refusal, because the mode is the thing that was asked for and the crop is
   * only ever a way of comparing two ways of sizing the encoder's working set.
   */

  enc_width = enc_width_req;

  if (enc_width == 0u || enc_width > width)
    {
      if (enc_width_req != 0u && enc_width_req > width)
        {
          printf("camenc: cannot encode %" PRIu32 " columns of a %" PRIu32
                 "-column mode; using all of them\n",
                 enc_width_req, width);
        }

      enc_width = width;
    }

  /* ---------------------------------------------------------------- */
  /* The camera                                                        */
  /* ---------------------------------------------------------------- */

  if (switch_t0 != 0)
    {
      printf("camenc: mode change to reopening the devices took %" PRIu64
             " ms\n",
             (camenc_now_us() - switch_t0) / 1000u);
      switch_t0 = 0;
    }

  /* A file of frames instead of a camera.
   *
   * Everything the camera section below does -- opening the device, choosing
   * the frame size and the rate, setting the exposure and the gain, and
   * running the 3A loop -- is about producing a picture, and a file already
   * holds one.  So the whole of it is skipped, the buffers the copy reads
   * from are allocated here instead of mapped, and the run joins the camera
   * path again at the encoder.
   *
   * The jump is a goto rather than an else so that the camera's own path is
   * not indented a level deeper for the benefit of the case that does not use
   * it; below this point the two share the encoder, the muxer and the server
   * and nothing else.
   */

  if (infile != NULL || synthetic)
    {
      size_t in_file = 0;

      if (infile != NULL && synthetic)
        {
          printf("camenc: -i and -S cannot both be the source\n");
          return EXIT_FAILURE;
        }

      if (threea_wanted)
        {
          printf("camenc: -A has no effect on a source that is not a camera:"
                 " nothing is exposing these frames\n");
        }

      threea = false;

      /* The frames are handed over at the mode's own rate unless something
       * else was asked for, so that the encoder is given the spacing it would
       * have been given with the sensor running.  See the pacing in the loop:
       * a source that quietly changed the timing would change the thing being
       * investigated along with it.
       */

      if (file_rate < 0)
        {
          file_rate = (int)cur->fps;
        }

      printf("source:   %s (file), mode %" PRIu32 " (%" PRIu32 "x%" PRIu32
             ")\n",
             infile, mode, width, height);

      if (enc_width != width)
        {
          printf("encoder:  encoding the leftmost %" PRIu32
                 " columns, %" PRIu32 " fewer than the mode has\n",
                 enc_width, width - enc_width);
        }

      /* What the camera's buffers would have been: its stride is its width,
       * because it reports none, and a frame is an NV12 picture.  Both of
       * these sources are the same size, which is what lets everything after
       * this point not care which of them it is.
       */

      src_size = (size_t)width * height * 3u / 2u;
      cam_stride = width;

      if (synthetic)
        {
          g_src_kind = CAMENC_SRC_PROBE;
          g_src_frame = src_size;

          camenc_probe_open(&g_probe, probe_bg, probe_radius, probe_amp, width,
                            height);

          printf("source:   probe %s, mode %" PRIu32 " (%" PRIu32 "x%" PRIu32
                 ")\n",
                 g_probe_bg_name[probe_bg], mode, width, height);
          printf("          background Y%" PRIu32 " Cb%" PRIu32 " Cr%" PRIu32
                 ", square %dx%d at Y%d, centre (%d,%d)\n",
                 g_probe.y, g_probe.u, g_probe.v, g_probe.side, g_probe.side,
                 g_probe.side_y, g_probe.cx, g_probe.cy);
          printf(
              "          orbit radius %d, %d pixels per frame,"
              " noise +-%d\n",
              g_probe.radius,
              (int)(((int64_t)g_probe.radius * g_probe.ds + (1 << 19)) >> 20),
              g_probe.amp);

          g_src_owned = true;

          for (k = 0; k < CAMENC_BUFFERS; k++)
            {
              g_cam[k].start = malloc(src_size);
              if (g_cam[k].start == NULL)
                {
                  printf("camenc: no room for a %zu-byte frame\n", src_size);
                  ret = -ENOMEM;
                  goto errout;
                }

              g_cam[k].length = src_size;
            }
        }
      else
        {
          g_src_kind = CAMENC_SRC_FILE;

          ret = camenc_source_open(infile, src_size, &in_file);
          if (ret < 0)
            {
              goto errout;
            }

          /* A run longer than the file would read past its end, so the file
           * sets the length when it is the shorter of the two -- unless the
           * loop was asked for, in which case the file is started again and
           * the length is whatever -n said.  A stream that quietly stopped
           * short of what was asked for would read as a failure to keep up.
           */

          if ((int)in_file < frames)
            {
              if (loop_source)
                {
                  printf("camenc: %s holds %zu frames and will be repeated;"
                         " encoding %d\n",
                         infile, in_file, frames);
                }
              else
                {
                  printf("camenc: %s holds %zu frames; encoding those rather"
                         " than the %d asked for\n",
                         infile, in_file, frames);
                  frames = (int)in_file;
                }
            }
        }

      nbuf_cam = CAMENC_BUFFERS;

      goto source_ready;
    }

  /* Tell the board which mode this run wants, before the camera is opened.
   *
   * A mode is not only the sensor's business.  It brings a link rate and a
   * frame size with it, and the D-PHY, the CSI HOST and the capture engine
   * all have to be moved to match -- which the board is the only place able
   * to do together and in the right order.  Nothing in the framework carries
   * the link rate that goes with a sensor mode, and the capture engine has no
   * scaler, so it refuses a frame size it was not programmed for: a mode
   * chosen here and set on the sensor alone is therefore a mode the capture
   * engine will reject on the very next call.
   *
   * This is what makes the mode the application's choice at all.  The board
   * brings its chain up on the mode its own configuration names, and that is
   * where a run that never asks for anything would stay -- so a caller that
   * does ask has to say so before the device is opened, which is here.
   *
   * A refusal is fatal, unlike the one on the switch path below.  There the
   * stream that was already running is still configured and can carry on;
   * here nothing has been opened yet, and a mode this program cannot select
   * is a mode it cannot film in.
   */

  if (!board_told)
    {
      uint64_t t_board = camenc_now_us();

      if (boardctl(CAMENC_BOARDIOC_SET_MODE, (uintptr_t)mode) < 0)
        {
          printf("camenc: the board will not take mode %" PRIu32 ": %d\n",
                 mode, errno);
          ret = -EINVAL;
          goto errout;
        }

      board_told = true;

      printf("camenc: board took %" PRIu64 " ms to select mode %" PRIu32 "\n",
             (camenc_now_us() - t_board) / 1000u, mode);
    }

  printf("camera:   %s, mode %" PRIu32 " (%" PRIu32 "x%" PRIu32
         " at %u fps)\n",
         camdev, mode, width, height, (unsigned int)cur->fps);

  if (enc_width != width)
    {
      printf("encoder:  encoding the leftmost %" PRIu32 " columns, %" PRIu32
             " fewer than the mode has\n",
             enc_width, width - enc_width);
    }

  /* Non-blocking, because the loop waits in poll() rather than in the
   * dequeue.  The wait has to cover the server's sockets as well, and a call
   * that sleeps on one thing cannot be woken by another -- see camenc_wait().
   * A dequeue with no frame ready then answers EAGAIN, which the loop reads
   * as "not yet" rather than as a failure.
   */

  camfd = open(camdev, O_RDWR | O_NONBLOCK);
  if (camfd < 0)
    {
      printf("camenc: cannot open %s: %d\n", camdev, errno);
      ret = -errno;
      goto errout;
    }

  ret = camenc_set_format(camfd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                          V4L2_PIX_FMT_NV12, width, height, "camera", &fmt);
  if (ret < 0)
    {
      goto errout;
    }

  camenc_describe_format("source", &fmt);

  /* The other half of the mode.  The frame size above is what selects between
   * geometries; this is what selects between the rates a geometry can run at,
   * and without it a stream always runs at the first rate the sensor lists --
   * see camenc_set_interval().
   */

  ret = camenc_set_interval(camfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, cur->fps,
                            "camera");
  if (ret < 0)
    {
      goto errout;
    }

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
       * This is not tidiness.  The sensor's own default gain is
       * OV5647_ANALOG_GAIN_DEFAULT, 256, while the loop's own minimum is
       * 16 -- so a loop that guessed would start believing the picture was
       * sixteen times darker than it is, ask for sixteen times too much
       * light in its first correction, and visibly darken the picture
       * before climbing back.  Starting from a wrong belief about the
       * hardware is the one thing a loop cannot compute its way out of.
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
      uint32_t keep_wb[3];
      bool seeded_from_hw;

      seeded_from_hw =
          camenc_get_ctrl(camfd, V4L2_CID_EXPOSURE_ABSOLUTE, &read_exp) ==
              OK &&
          camenc_get_ctrl(camfd, V4L2_CID_ISO_SENSITIVITY, &read_gain) == OK &&
          read_exp > 0 && read_gain > 0;

      seed_exp = seeded_from_hw  ? (uint32_t)read_exp
                 : exposure >= 0 ? (uint32_t)exposure
                                 : OV5647_MODE_EXPOSURE_MAX(cur);
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

          /* The ceiling is the mode's own frame length less room to read out,
           * so it moves when the mode does.  Read from the same expression
           * the sensor driver clamps with, in the sensor's header, rather than
           * written down again here -- the two have to agree for the loop to
           * be able to trust what it reads back.
           */

          cfg3a.exposure_max = OV5647_MODE_EXPOSURE_MAX(cur);
          cfg3a.gain_min = CAMENC_3A_GAIN_MIN;
          cfg3a.gain_max = CAMENC_3A_GAIN_MAX;
          cfg3a.gain_one = CAMENC_3A_GAIN_ONE;
          cfg3a.ae_shift = CAMENC_3A_AE_SHIFT;
          cfg3a.awb_shift = CAMENC_3A_AWB_SHIFT;
          cfg3a.settle = CAMENC_3A_SETTLE;
          cfg3a.awb_min_mean = CAMENC_3A_AWB_MIN_MEAN;

          /* The white balance is the one setting a change of mode has no
           * reason to disturb: the colour of the light has not moved because
           * the frame it is measured in got bigger.  So a loop that has
           * already converged hands its gains to the next stream as the
           * starting point, and only a first stream takes what the driver
           * holds.  They are copied out first because the seed argument would
           * otherwise alias the array that is about to be written.
           */

          if (wb_converged)
            {
              keep_wb[0] = threea_state.wb[0];
              keep_wb[1] = threea_state.wb[1];
              keep_wb[2] = threea_state.wb[2];
            }

          camenc_3a_init(&threea_state, &cfg3a, seed_exp, seed_gain,
                         wb_converged ? keep_wb : probe.wb, probe.sequence);

          /* The page starts from the settings the loop starts from, so that
           * the first slider moved is a change from what is in force rather
           * than from a zero that would clamp.  On a first stream only,
           * though: after that the page is the one holding the settings, and
           * a change of mode must not put back an automatic loop that
           * somebody had taken by hand.
           */

          if (!ui_seeded)
            {
              g_ui.ae_auto = true;
              g_ui.awb_auto = true;
              g_ui.exposure = threea_state.exposure;
              g_ui.gain = threea_state.gain;
              g_ui.wb[0] = threea_state.wb[0];
              g_ui.wb[1] = threea_state.wb[1];
              g_ui.wb[2] = threea_state.wb[2];

              ui_seeded = true;
            }

          threea_started = true;
          wb_converged = true;

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

  /* The camera's own bytes per row, kept here because the encoder's format
   * calls below overwrite the shared descriptor.  The camera reports nothing,
   * so this is its width -- see camenc_frame_size().
   */

  cam_stride = fmt.fmt.pix.bytesperline != 0 ? fmt.fmt.pix.bytesperline
                                             : fmt.fmt.pix.width;

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

  /* Both sources arrive here: one with its buffers mapped from the capture
   * driver, the other with its own.  The test for which is the source
   * itself, and it is made everywhere the two differ.
   */

source_ready:

  /* ---------------------------------------------------------------- */
  /* The encoder                                                       */
  /* ---------------------------------------------------------------- */

  /* Left blocking, which makes no difference and is worth a word.  This
   * framework's dequeue does not wait whether the descriptor says it may or
   * not: it answers EAGAIN when the queue is empty, so what makes the loop
   * non-blocking is that it waits in poll() before asking, not the flag.  The
   * camera's framework is the other way round, which is why that descriptor
   * does need O_NONBLOCK.
   */

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
                          enc_width, height, "encoder input", &fmt);
  if (ret < 0)
    {
      goto errout;
    }

  camenc_describe_format("input", &fmt);

  /* The encoder does report a stride, unlike the camera.  It is what the copy
   * writes rows at, and it is the width rounded up to sixteen when the driver
   * has nothing more to say.
   */

  enc_stride = fmt.fmt.pix.bytesperline != 0 ? fmt.fmt.pix.bytesperline
                                             : (enc_width + 15u) & ~15u;

  /* And it lays its luma plane out over its *allocated* height rather than the
   * picture's: the height rounded up to its macroblock grid, with the chroma
   * plane after that many rows.  For every mode whose height is already on the
   * grid the two are the same number; for 1920x1080 they are not, and a chroma
   * plane written at the picture height would sit eight rows above the one the
   * encoder reads.  See camenc_copy_frame().
   *
   * Its sizeimage is the whole allocation, so dividing the row size out of it
   * recovers that height from the driver's own answer rather than from a
   * second copy of its rounding rule -- and that matters, because this is the
   * one figure in the copy the two sides do not agree on, so guessing it is
   * the only way to get it wrong without noticing.  The rounding is kept as
   * the fallback for a driver that reports no size; a value that does not
   * divide into whole rows, or that is shorter than the picture, is refused
   * rather than used, since either would put the chroma plane where nothing
   * reads it.
   */

  enc_vstride = (height + 15u) & ~15u;

  if (fmt.fmt.pix.sizeimage != 0 &&
      fmt.fmt.pix.sizeimage % ((size_t)enc_stride * 3u / 2u) == 0)
    {
      uint32_t rows =
          (uint32_t)(fmt.fmt.pix.sizeimage / ((size_t)enc_stride * 3u / 2u));

      if (rows >= height)
        {
          enc_vstride = rows;
        }
    }

  /* Whether the frame the encoder reads may be the camera's own buffer.
   *
   * This is a comparison of two layouts rather than a switch.  The capture
   * driver writes a frame at the picture's width and height -- its luma plane
   * is one row of `width` bytes and its chroma plane begins after `height` of
   * them -- and the encoder reads one at the width rounded up to its
   * macroblock grid and the height rounded up to its own.  While the two
   * agree the same bytes are a frame to both, so the encoder can be given the
   * address the demosaic left the picture at and the copy between them is
   * pure overhead.
   *
   * They agree for every mode whose geometry is already on the encoder's
   * grid, which is all but one of the sensor's: 1296x960, 1280x720 and
   * 640x480 all have a height that is a multiple of sixteen.  They do not
   * agree for 1920x1080, where the encoder's luma plane is 1088 rows and the
   * capture driver's is 1080 -- and there the frame has to be copied still,
   * because the two sides would otherwise disagree about where the chroma
   * plane begins.  That is the same disagreement that put a magenta band at
   * the bottom of a 1080p picture; see camenc_copy_frame().
   *
   * A crop is the other way the two differ: given fewer columns than the mode
   * has, the encoder reads rows the camera's rows are longer than, and a
   * block copy would shear the picture rather than crop it.
   *
   * The camera's stride is its width, because the capture driver reports
   * none -- see camenc_frame_size().
   */

  g_zerocopy = g_src_kind == CAMENC_SRC_CAMERA && enc_width == width &&
               enc_stride == cam_stride && enc_vstride == height;

  /* The encoder can only be given a buffer it can reach and maintain: 64-byte
   * aligned, and wholly inside the low 4 GiB, because its address register is
   * 32 bits wide and the cache maintenance around a job is done a line at a
   * time.  Its driver enforces exactly that and will refuse a frame that
   * breaks it -- but the refusal would arrive at the first queue operation,
   * half-way into a stream.  Asking the same question here instead means such
   * a frame is copied and the run goes on.
   *
   * This is a check on the arrangement rather than on a case that happens:
   * the capture driver's frames are allocated for exactly this and satisfy
   * both conditions by construction.  It is written out because the two
   * mirrored rules have to stay the same rule.
   */

  if (g_zerocopy &&
      (((uintptr_t)g_cam[0].start & 63u) != 0 ||
       (uintptr_t)g_cam[0].start + g_cam[0].length - 1u > 0xffffffffu))
    {
      printf("camenc: the camera's buffers are at %p, %zu bytes, which the"
             " encoder cannot be given; copying the frames instead\n",
             (FAR void *)g_cam[0].start, g_cam[0].length);
      g_zerocopy = false;
    }

  printf("input:    %s\n",
         g_zerocopy ? "the camera's frames are encoded where they lie"
                    : "copied into the encoder's own buffers");

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

  if (g_zerocopy)
    {
      ret = camenc_setup_import(encfd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                                CAMENC_BUFFERS, &nbuf_out, "encoder input");
    }
  else
    {
      ret = camenc_setup_buffers(encfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, g_out,
                                 CAMENC_BUFFERS, &nbuf_out, "encoder input");
    }

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

  /* The destination has to be able to hold what the copy writes into it.  The
   * two sides no longer have to agree on a size, and must not be made to: the
   * encoder's frame is its own geometry's, enc_stride by enc_vstride, and the
   * camera's is the picture's, and for a height that is not on the encoder's
   * grid those differ by the padding -- which is the ordinary case for this
   * mode rather than a mistake.  What is required is that the destination is
   * at least the size the copy addresses, and QUERYBUF's length is that
   * buffer's real length.
   */

  if (g_zerocopy)
    {
      /* The frame the encoder reads is the camera's buffer, so it is that
       * buffer which has to be as large as the encoder's geometry asks.  The
       * two are the same size whenever the layouts agree -- that is part of
       * what agreeing means -- but the check is kept, because the failure it
       * would catch is the one this whole arrangement rests on.
       */

      if (g_cam[0].length < (size_t)enc_stride * enc_vstride * 3u / 2u)
        {
          printf("camenc: the camera's buffers are %zu bytes, and a"
                 " %" PRIu32 "x%" PRIu32 " frame with a %" PRIu32
                 "-row luma plane needs %zu\n",
                 g_cam[0].length, enc_width, height, enc_vstride,
                 (size_t)enc_stride * enc_vstride * 3u / 2u);
          ret = -EINVAL;
          goto errout;
        }
    }
  else if (g_out[0].length < (size_t)enc_stride * enc_vstride * 3u / 2u)
    {
      printf("camenc: the encoder's input buffers are %zu bytes, and a"
             " %" PRIu32 "x%" PRIu32 " frame with a %" PRIu32
             "-row luma plane needs %zu\n",
             g_out[0].length, enc_width, height, enc_vstride,
             (size_t)enc_stride * enc_vstride * 3u / 2u);
      ret = -EINVAL;
      goto errout;
    }

  /* A crop reads columns the camera's rows have, which is the one thing the
   * copy below cannot check for itself.
   */

  if (cam_stride < enc_width)
    {
      printf("camenc: the camera's rows are %" PRIu32 " bytes and the"
             " encoder was asked for %" PRIu32
             "; it cannot be given a picture that wide\n",
             cam_stride, enc_width);
      ret = -EINVAL;
      goto errout;
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

  ret = camenc_stream_init(&st, enc_width, height, (uint32_t)max_au, scratch,
                           (size_t)max_au + CAMENC_STREAM_SLACK, camenc_emit,
                           &sink);
  if (ret < 0)
    {
      printf("camenc: muxer init failed: %d\n", ret);
      goto errout;
    }

  /* The mode in force is what the page's menu has to show, so that the next
   * command is a move away from the picture on the screen rather than from
   * whatever the page happened to load with.
   */

  g_ui.want_mode = (int32_t)mode;

  /* ---------------------------------------------------------------- */
  /* Streams                                                           */
  /* ---------------------------------------------------------------- */

  /* The encoder's capture side is started before its output side.  This is no
   * longer forced by the driver -- a frame queued with nowhere for the
   * bitstream to go is simply left queued until a capture buffer is offered,
   * and starting the output first would work -- but it is the order the
   * buffers are arranged in here: every capture buffer is offered before any
   * picture is, so that the first frame has somewhere to go the moment it is
   * taken.  The output side is started only after those buffers are in the
   * queue, which is what this order puts it after.
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
   *
   * A file source has neither half of that.  There is nothing to start, and
   * nothing to offer: its buffers are filled by the loop and never leave it,
   * which is also why they are not released with munmap at the end -- see the
   * teardown.
   */

  if (camfd >= 0)
    {
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
    }

  printf("\nencoding %d frames at qp %d, groups of %d\n", frames, qp, gop);

  t0 = camenc_now_us();

  /* ---------------------------------------------------------------- */
  /* The loop                                                          */
  /* ---------------------------------------------------------------- */

  /* The frame number is advanced by the body rather than by the loop, because
   * a round that served the server and found nothing else to do has not used
   * one.  It is advanced when a frame has been muxed and handed over, which is
   * the only point at which the loop has consumed a frame number.
   *
   * The frame the loop is carrying lives out here for the same reason.  The
   * encoder is given a picture and answers later, so a frame spans two rounds
   * -- the one that queues it and the one that collects the bitstream -- and
   * what has to survive between them is which camera buffer is out, so that it
   * can be handed back as soon as the encoder has finished reading it, and the
   * timestamp the frame arrived with, which is what its place in the stream's
   * timeline is built from.  Everything else about a frame, the containers and
   * the copy included, is born and dies within the round that uses it.
   */

  bool inflight = false; /* a frame is with the encoder, not yet collected */
  uint32_t camidx = 0;   /* the camera buffer that frame came from          */
  uint64_t pts = 0;      /* when that frame arrived                         */

  for (; i < frames;)
    {
      struct v4l2_buffer cbuf;
      struct v4l2_buffer obuf;
      struct v4l2_buffer ebuf;
      struct camenc_buf_s *out = NULL;
      struct camenc_buf_s *cap;
      uint64_t mark;
      bool camera_ready;

      /* A change of mode, asked for by the page and recorded by the server's
       * callback.
       *
       * Checked at the top of the frame rather than where the command arrives,
       * for two reasons.  The callback runs inside camenc_ws_ready(), which is
       * called from the wait, so a mode change cannot be done there; and doing
       * it here, before a frame is taken, is what keeps the last frames of the
       * old mode out of the new stream -- they would be a different size, and
       * the muxer has already been told what size to write.
       *
       * It is also only done with no frame in flight.  A frame that is with
       * the encoder belongs to the stream being replaced, and the request can
       * wait the few milliseconds for it to come back rather than abandoning
       * it; the flag is left set until then, so nothing is lost.
       *
       * A request for the mode already in force is not a change and does not
       * interrupt anything; the menu sends one whenever it is redrawn.
       */

      if (!inflight && g_ui.mode_pending)
        {
          int32_t want = g_ui.want_mode;

          g_ui.mode_pending = false;

          if (want != (int32_t)mode)
            {
              if (g_src_kind != CAMENC_SRC_CAMERA)
                {
                  /* The frames are of this mode's geometry and no board is
                   * being asked for another one, so a change of mode cannot
                   * be honoured.  Refusing it here leaves the stream running
                   * on the mode it is on, which is the mode the page will be
                   * shown.
                   */

                  printf("camenc: mode %" PRId32 " was asked for, but the"
                         " source is not a camera\n",
                         want);
                }
              else
                {
                  printf("camenc: mode %" PRId32 " requested; ending this"
                         " stream\n",
                         want);
                  switch_t0 = camenc_now_us();
                  switching = true;
                  break;
                }
            }
        }

      /* Wait for something to do, which is a frame, a bitstream or a client,
       * and serve whoever asked while waiting.  This is the loop's only sleep,
       * and everything below runs because it returned.
       */

      camera_ready = camenc_wait(camfd, encfd, inflight);

      if (inflight)
        {
          /* The finishing half of the frame the previous round started.
           *
           * Nothing here blocks on the encoder: the wait above is what slept,
           * and what woke the loop when one of these containers was ready.
           * -EAGAIN still means "not yet", as it does for the camera, because
           * a wake-up can be for one container and not the other -- the
           * driver hands the input container back before the bitstream, and
           * those are two separate wake-ups.
           *
           * The input container is taken first and the camera's buffer goes
           * back with it.  That is the earliest moment the loop has finished
           * reading the frame, and in the modes that encode where the frame
           * lies that buffer is the encoder's source, so the capture driver
           * cannot be given it back a moment sooner than this.
           */

          mark = camenc_now_us();

          ret = camenc_dequeue(encfd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &obuf,
                               "encoder input");
          if (ret == 0)
            {
              if (obuf.index != (uint32_t)(i % (int)nbuf_out))
                {
                  printf("camenc: encoder returned input buffer %" PRIu32
                         " where %d was queued\n",
                         obuf.index, i % (int)nbuf_out);
                }

              if (camfd >= 0 && camenc_queue(camfd, &g_cam[camidx], "camera",
                                             (int)camidx) < 0)
                {
                  ret = -EIO;
                  break;
                }
            }
          else if (ret != -EAGAIN)
            {
              break;
            }

          /* The bitstream. */

          ret = camenc_dequeue(encfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &ebuf,
                               "encoder output");

          camenc_stage_account(CAMENC_STAGE_DRAIN, mark);

          if (ret == -EAGAIN)
            {
              /* The input container came back before the frame did, which is
               * the order the driver hands them over in.  The round ends with
               * the camera's buffer already returned and the frame still to
               * collect.
               */

              continue;
            }

          if (ret < 0)
            {
              break;
            }

          if (ebuf.index >= nbuf_cap)
            {
              printf("camenc: encoder returned buffer %" PRIu32 "\n",
                     ebuf.index);
              ret = -EINVAL;
              break;
            }

          cap = &g_cap[ebuf.index];

          /* A buffer with nothing in it is the marker the flush leaves behind,
           * not a frame; there is nothing to mux.  It is also what a job that
           * failed comes back as, there being no error return left for the
           * queue operation to carry, and it is counted as a drop like any
           * other buffer with no frame in it.
           */

          if ((ebuf.flags & V4L2_BUF_FLAG_LAST) != 0 && ebuf.bytesused == 0)
            {
              if (camenc_queue(encfd, cap, "encoder output", (int)ebuf.index) <
                  0)
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
              /* Whether the encoder may be started at this frame, which is
               * the encoder's answer to give rather than something read out
               * of the bytes: a picture does not say whether anything after
               * it predicts from it.  It arrives as a buffer flag for exactly
               * this reason.
               *
               * The distinction decides where a fragment may begin, so
               * getting it wrong is not cosmetic.  A fragment a player cannot
               * start on is a fragment it decodes to noise, and the muxer
               * cannot tell after the fact -- hence taking it from the
               * source.
               */

              bool key = (ebuf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;

              mark = camenc_now_us();

              ret = camenc_stream_write(&st, cap->start, ebuf.bytesused, pts,
                                        key);
              if (ret < 0)
                {
                  printf("camenc: muxing frame %d failed: %d\n", i, ret);
                  break;
                }

              camenc_stage_account(CAMENC_STAGE_MUX, mark);

              /* What the group length is buying on this content.
               *
               * Counted here rather than assumed from the configuration: the
               * claim a P picture makes is that it costs a fraction of an
               * IDR, and the only place that can be checked is a stream that
               * was encoded from something that moved.  A P as large as its
               * IDR means prediction is not being used, whatever the
               * registers say.
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

                  if (camenc_stream_codec_string(&st, codec, sizeof(codec)) >
                      0)
                    {
                      camenc_ws_set_codec(&g_ws, codec);
                      codec_set = true;
                      printf("camenc: stream codec is %s\n", codec);
                    }
                }

              encoded++;
            }

          /* The capture container goes back, so the pool the encoder draws
           * from stays as deep as the application keeps it.  This is not what
           * gates the next frame -- the encoder runs one job at a time by
           * itself -- but a container it is not given is one it cannot fill.
           */

          if (camenc_queue(encfd, cap, "encoder output", (int)ebuf.index) < 0)
            {
              ret = -EIO;
              break;
            }

          inflight = false;
        }
      else
        {
          /* A frame: from the camera, from the file, or drawn here.  Either
           * way what comes out is `camidx` and a buffer in g_cam[] holding one
           * NV12 picture, which is all the rest of the loop knows about any of
           * them.
           *
           * The camera is the one source that may have nothing to give, since
           * the wait is answered before the dequeue is asked and the two can
           * disagree.  A dequeue with nothing there answers EAGAIN, which is
           * not a failure.
           */

          mark = camenc_now_us();

          if (g_src_kind == CAMENC_SRC_PROBE)
            {
              camidx = (uint32_t)(i % (int)nbuf_cam);
              camenc_probe_fill(&g_probe, g_cam[camidx].start, enc_width,
                                height);
            }
          else if (g_src_kind == CAMENC_SRC_FILE)
            {
              camidx = (uint32_t)(i % (int)nbuf_cam);

              if (fread(g_cam[camidx].start, 1, g_src_frame, g_src_file) !=
                  g_src_frame)
                {
                  /* Running out of file ends the run, unless -l asked for the
                   * file to be started again.  The defect being looked for is
                   * intermittent, so how many frames it is given is the whole
                   * of how likely it is to appear at all, and repeating a
                   * short file is the cheapest way to give it a long run.
                   */

                  if (!loop_source || fseek(g_src_file, 0, SEEK_SET) != 0 ||
                      fread(g_cam[camidx].start, 1, g_src_frame, g_src_file) !=
                          g_src_frame)
                    {
                      printf("camenc: the source file ended after %d frames\n",
                             i);
                      ret = -EIO;
                      break;
                    }

                  printf("camenc: the source file started again at frame %d\n",
                         i);
                }
            }
          else
            {
              if (!camera_ready)
                {
                  /* Nothing to take.  Serving the server was the whole of what
                   * this round had to do, and the frame number is not
                   * advanced, so the next round waits for the same frame.
                   */

                  continue;
                }

              ret = camenc_dequeue(camfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cbuf,
                                   "camera");
              if (ret == -EAGAIN)
                {
                  continue;
                }

              if (ret < 0)
                {
                  break;
                }

              camidx = cbuf.index;
            }

          camenc_stage_account(CAMENC_STAGE_CAMERA, mark);

          if (camidx >= nbuf_cam || g_cam[camidx].start == NULL)
            {
              printf("camenc: camera returned unusable buffer %" PRIu32 "\n",
                     camidx);
              ret = -EINVAL;
              break;
            }

          /* The frame's timestamp: the camera's clock when it arrived, or a
           * made one when the frames come from a file.
           */

          if (g_src_kind != CAMENC_SRC_CAMERA)
            {
              /* The camera's frames arrive on a clock and the muxer wants the
               * spacing that came with them.  A file has no clock, so one is
               * made from the rate the frames are handed over at, and the
               * frame is held back until its moment: a loop that ran the
               * encoder back to back would be a different timing environment
               * from the one being investigated, and timing is not a thing
               * this source can afford to change quietly.  -r 0 asks for that
               * anyway, and for a much shorter run.
               *
               * The timestamp is also what the stream's timeline is built
               * from, so it is taken from the frame's place in the file and
               * not from when it was read -- the second would put the
               * encoder's own timing into the file's duration.
               */

              if (file_rate > 0)
                {
                  uint64_t at = camenc_now_us();

                  if (file_frame == 0)
                    {
                      file_t0 = at;
                    }
                  else
                    {
                      uint64_t due = file_t0 + (uint64_t)file_frame *
                                                   1000000u /
                                                   (uint32_t)file_rate;

                      if (due > at)
                        {
                          usleep((useconds_t)(due - at));
                        }
                    }
                }

              pts = (uint64_t)file_frame * 1000000u /
                    (uint32_t)(file_rate > 0 ? file_rate : (int)cur->fps);
              file_frame++;
            }
          else
            {
              /* The time the frame arrived, measured from the first frame
               * rather than from the epoch or from the start of the loop.
               *
               * It is taken here rather than derived from the frame number, so
               * that the stream's timeline is the one the frames actually
               * arrived on: a camera that stutters produces a stream that
               * stutters in the same places instead of one that runs at the
               * wrong speed throughout.
               *
               * The origin matters as much as the spacing.  gettimeofday()
               * returns seconds since 1970 here, so an absolute timestamp puts
               * the stream's first sample a hundred and fifty years in, and a
               * player then reports a start time and a duration to match.
               * Measuring from the first frame makes the stream begin at zero,
               * which is what a live stream should look like and what a player
               * can seek within.
               */

              now = camenc_now_us();

              if (!have_prev)
                {
                  media_t0 = now;
                }

              pts = now - media_t0;
            }

          /* The encoder's input buffer for this frame, taken round-robin. With
           * as many as the camera has, the one just freed is the one used
           * next, so a frame is never copied over one still being encoded.
           *
           * When the two devices agree on the layout there is nothing to copy
           * and no buffer of ours to copy into: the frame is the camera's own
           * buffer, which is still ours -- it is handed back to the camera at
           * the end of this pass -- and the encoder is given its address
           * instead. See g_zerocopy above the loop for what makes them agree.
           */

          if (!g_zerocopy)
            {
              out = &g_out[i % (int)nbuf_out];

              mark = camenc_now_us();

              out->desc.bytesused = (uint32_t)camenc_copy_frame(
                  out->start, g_cam[camidx].start, enc_stride, enc_vstride,
                  cam_stride, enc_width, height);

              camenc_stage_account(CAMENC_STAGE_COPY, mark);
            }

          /* Queueing this starts the job.  It returns as soon as the hardware
           * has the picture, so this stage measures the submission and no
           * longer the encode: the encoder's time is now the wait at the top
           * of the loop, which is where the loop is asleep for it.
           */

          mark = camenc_now_us();

          if (g_zerocopy)
            {
              ret = camenc_queue_import(encfd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                                        g_cam[camidx].start, i % (int)nbuf_out,
                                        "encoder input");
            }
          else
            {
              ret =
                  camenc_queue(encfd, out, "encoder input", i % (int)nbuf_out);
            }

          if (ret < 0)
            {
              ret = -EIO;
              break;
            }

          camenc_stage_account(CAMENC_STAGE_ENCODE, mark);

          /* The frame is with the encoder now, and this round is over.  The
           * next one waits for the job to report back, which is the round
           * after this one or later still, and takes the bitstream from there.
           * Not taking the camera's next frame until then is what keeps one
           * frame in the encoder rather than a queue of them, and in the modes
           * that encode in place it is what keeps the camera from writing over
           * the source.
           */

          inflight = true;
          continue;
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
              camenc_status_publish(&g_ws, &threea_state, (uint32_t)i, mode);
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

      /* One whole frame was taken, encoded, muxed and handed over, so this is
       * where the frame number advances -- see the loop above.
       *
       * The server used to be serviced here, once per frame whether or not it
       * had anything to do.  It is serviced by the wait at the top of the
       * loop now, when it has.
       */

      i++;
    }

  /* A change of mode ends this stream rather than the run.
   *
   * There is nothing to flush and nothing worth reporting about the part that
   * was interrupted: the frames that would follow belong to a different
   * stream, with a different frame size, its own initialisation segment and a
   * timeline of its own, so the stream is taken down and the next one built in
   * its place.
   */

  if (switching)
    {
      ret = OK;
      goto errout_camoff;
    }

  t1 = camenc_now_us();

  if (ret < 0)
    {
      goto errout_camoff;
    }

  /* ---------------------------------------------------------------- */
  /* Flush                                                             */
  /* ---------------------------------------------------------------- */

  /* No frame is in flight and none is queued: the loop only leaves one behind
   * when it is ending for some other reason, and those paths have already gone
   * their own way.  So the flush ends the stream on a buffer of its own -- an
   * empty one carrying the marker -- published by the flush itself and
   * collected by the dequeue below.
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
  if (camfd >= 0)
    {
      camenc_stream_off(camfd, &cam_type);
    }

errout_streamoff:
  camenc_stream_off(encfd, &out_type);
  camenc_stream_off(encfd, &cap_type);

errout:
  /* The stream's own resources, released whether it ended by running out of
   * frames, by being interrupted for a change of mode, or by failing.
   *
   * The server and the file are deliberately not among them: both outlive a
   * change of mode, and are released once at the end of the run below.  That
   * is also why nothing here returns: on a change of mode this is the middle
   * of the program rather than its end.
   */

  free(scratch);
  scratch = NULL;

  for (i = 0; i < CAMENC_BUFFERS; i++)
    {
      if (g_cam[i].start != NULL)
        {
          /* The camera's buffers were mapped from the driver and the file
           * source's are its own allocation, so which release they need is
           * decided by which way they were obtained -- not by the buffer.
           */

          if (g_src_owned)
            {
              free(g_cam[i].start);
            }
          else
            {
              munmap(g_cam[i].start, g_cam[i].length);
            }

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

  /* Whichever way the buffers above were obtained, the next pass starts with
   * none of them: a file source allocates its own again when it is re-opened,
   * and a camera's are mapped again by the capture setup.
   */

  g_src_owned = false;

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

  /* A change of mode is not the end of the run.
   *
   * The board is asked to make it, and asked rather than told: a mode is not
   * only the sensor's business.  It brings a link rate and a frame size with
   * it, and the D-PHY, the CSI HOST and the capture engine all have to be
   * moved to match -- which the board is the only place able to do together
   * and in the right order.  It also refuses an index that names no mode,
   * which is why the answer is checked rather than assumed: a request the
   * board will not take has to leave the run on the mode it already has,
   * rather than on one nothing is configured for.
   */

  if (switching)
    {
      uint32_t want = (uint32_t)g_ui.want_mode;
      uint64_t t_board;

      switching = false;

      printf("camenc: stream down after %" PRIu64 " ms\n",
             (camenc_now_us() - switch_t0) / 1000u);

      t_board = camenc_now_us();

      if (boardctl(CAMENC_BOARDIOC_SET_MODE, (uintptr_t)want) < 0)
        {
          /* The board refused, and it says so only after leaving the receive
           * chain as it found it -- so the mode that was running is still the
           * mode that is configured, and the run continues on it.
           *
           * This used to end the program, which was wrong in a way worth
           * naming: a request the board would not take is a fact about that
           * request, not a reason to stop filming.  The page reconnects,
           * asks for the status, and is told which mode is in force -- which
           * is what makes the menu snap back to the picture on the screen
           * instead of showing a mode nothing is running.
           */

          printf("camenc: the board will not switch to mode %" PRIu32
                 ": %d; staying on mode %" PRIu32 "\n",
                 want, errno, mode);
        }
      else
        {
          mode = want;

          /* The clients are watching a stream that is about to stop existing,
           * and the one replacing it has a different frame size and a
           * different initialisation segment.  Letting them go is not a
           * failure to serve them: the page has exactly one way to rebuild its
           * demuxer, which is to reconnect, and it already does that for a
           * server that went away.  See camenc_ws_drop_clients().
           */

          if (serving)
            {
              camenc_ws_drop_clients(&g_ws);
              printf("camenc: clients released; the page will reconnect\n");
            }

          printf("camenc: switching to mode %" PRIu32 "\n", mode);
        }

      printf("camenc: board took %" PRIu64 " ms\n",
             (camenc_now_us() - t_board) / 1000u);

      goto stream_start;
    }

  /* The whole run's resources, released once however it ended.  What reaches
   * here is either the loop running out of frames or a failure inside a
   * stream; a change of mode goes back up instead of through.
   */

  if (serving)
    {
      camenc_ws_stop(&g_ws);
      serving = false;
    }

  if (sink.file != NULL)
    {
      fclose(sink.file);
      sink.file = NULL;
    }

  if (g_src_file != NULL)
    {
      fclose(g_src_file);
      g_src_file = NULL;
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
