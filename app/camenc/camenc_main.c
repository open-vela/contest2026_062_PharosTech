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

#include "camenc_stream.h"
#include "camenc_ws.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMENC_CAM_DEVPATH    "/dev/video0"
#define CAMENC_ENC_DEVPATH    "/dev/video1"

#define CAMENC_DEFAULT_WIDTH  640
#define CAMENC_DEFAULT_HEIGHT 480
#define CAMENC_DEFAULT_FRAMES 300
#define CAMENC_DEFAULT_QP     26

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

#define CAMENC_CID_QP 0x1000

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
 * CONFIG_SYSTEM_CAMENC_STACKSIZE and 8192 by default.  As a local variable
 * it overran: the frame alone was 9520 bytes, so the first store into it
 * ran off the end of the stack into whatever the heap had put there, and
 * the allocation that followed walked the damage and faulted.  It is here
 * for the same reason the buffers above are: one camenc at a time, and it
 * cannot fail to be there.
 */

static struct camenc_ws_s g_ws;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t camenc_now_us(void)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
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
  printf("  -x  sensor exposure in lines, overrides the driver default\n");
  printf("  -g  sensor analog gain, 16 = 1.0x and 1023 = 64x\n\n");
  printf(
      "The output is fragmented MP4 -- an initialisation segment followed\n");
  printf(
      "by one fragment per frame -- which is what a browser plays through\n");
  printf(
      "Media Source Extensions, and what the WebSocket carries.  With -p,\n");
  printf("point a browser at http://<board address>:<port>/ and it plays.\n");
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

static int camenc_set_ctrl(int fd, uint32_t id, int value,
                           FAR const char *name)
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
      printf("camenc: setting %s = %d failed: %d\n", name, value, errno);
      return -errno;
    }

  return 0;
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

  if (sink->file != NULL && fwrite(data, 1, len, sink->file) != len)
    {
      return -EIO;
    }

  if (sink->ws != NULL && camenc_ws_publish(sink->ws, seg, data, len, key) < 0)
    {
      return -EIO;
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
  int port = 0;
  int exposure = -1;
  int gain = -1;
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

  while ((opt = getopt(argc, argv, "d:e:o:n:w:h:q:x:g:p:")) != -1)
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

          case 'x':
            exposure = atoi(optarg);
            break;

          case 'g':
            gain = atoi(optarg);
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
      ret = camenc_ws_start(&g_ws, (uint16_t)port,
                            CAMENC_WS_MAX_SEGMENT + CAMENC_WS_TX_HEADER);
      if (ret < 0)
        {
          printf("camenc: cannot serve on port %d: %d\n", port, ret);
          goto errout;
        }

      serving = true;
      sink.ws = &g_ws;

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
      uint32_t camidx;

      /* A frame from the camera. */

      ret =
          camenc_dequeue(camfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cbuf, "camera");
      if (ret < 0)
        {
          break;
        }

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

      memcpy(out->start, g_cam[camidx].start, copy_size);
      out->desc.bytesused = (uint32_t)copy_size;

      /* Queueing this encodes it. */

      if (camenc_queue(encfd, out, "encoder input", i % (int)nbuf_out) < 0)
        {
          ret = -EIO;
          break;
        }

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
          /* Every picture this encoder produces is an IDR, so every fragment
           * is one a stream may be started on.  That will stop being true
           * when it learns to emit P pictures; the muxer already carries the
           * distinction, and this is where it will come from.
           */

          ret =
              camenc_stream_write(&st, cap->start, ebuf.bytesused, pts, true);
          if (ret < 0)
            {
              printf("camenc: muxing frame %d failed: %d\n", i, ret);
              break;
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

          printf("frame %6d: %6" PRIu32 " bytes, %5.1f fps, %" PRIu64
                 " bytes out\n",
                 i, ebuf.bytesused,
                 span != 0
                     ? 1000000.0 * (double)(i - last_report) / (double)span
                     : 0.0,
                 sink.bytes);

          last_report = i;
          reported_at = pts;
        }

      /* Service the server between frames: take new connections, notice the
       * ones that have gone, and write out what is queued for the rest.  It
       * never blocks, because a stalled viewer must not stall the camera.
       */

      if (serving)
        {
          camenc_ws_poll(&g_ws);
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

  if (camfd >= 0)
    {
      close(camfd);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
