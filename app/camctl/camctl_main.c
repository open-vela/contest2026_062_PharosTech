/****************************************************************************
 * app/camctl/camctl_main.c
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
 * camctl -- capture frames from the MIPI CSI camera and report what arrived.
 *
 * This is the acceptance tool for the capture path.  It talks plain V4L2 to
 * /dev/videoN through the NuttX capture framework, so it knows nothing about
 * VICAP, the CSI HOST or the sensor -- which is the point: if camctl can
 * stream, then the whole chain underneath it is working, and if it cannot,
 * the failure is visible one stage at a time.
 *
 * Beyond counting frames it reports two things that separate "frames are
 * arriving" from "the picture is real":
 *
 *   - the mean luma of the Y plane, which is ~0 for a black frame and
 *     identical frame to frame for a frozen one;
 *   - the mean absolute difference against the previous frame, which is ~0
 *     when the same buffer is being handed back each time.
 *
 * A frame count alone cannot tell those apart, and a camera that streams
 * black frames at 30 fps looks perfect by that measure.
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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMCTL_DEFAULT_DEVPATH "/dev/video0"
#define CAMCTL_DEFAULT_COUNT   30
#define CAMCTL_BUFFERS         3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct camctl_buffer_s
{
  FAR void *start;
  size_t length;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct camctl_buffer_s g_buffers[CAMCTL_BUFFERS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t camctl_now_us(void)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static void camctl_usage(void)
{
  printf("Usage: camctl [-d devpath] [-c frames] [-o outfile]"
         " [-e lines] [-g gain]\n");
  printf("  -d  capture device (default %s)\n", CAMCTL_DEFAULT_DEVPATH);
  printf("  -c  number of frames to capture (default %d)\n",
         CAMCTL_DEFAULT_COUNT);
  printf("  -o  write the last captured frame to a file as NV12\n");
  printf("  -e  exposure in lines, overrides the driver default\n");
  printf("  -g  analog gain, 16 = 1.0x and 1023 = 64x\n\n");
  printf(
      "The exposure and gain are applied to the sensor before the stream\n");
  printf("starts, so they can be swept without rebuilding the firmware.\n");
}

/****************************************************************************
 * Name: camctl_show_capability / camctl_show_formats
 ****************************************************************************/

static int camctl_show_capability(int fd)
{
  struct v4l2_capability cap;

  memset(&cap, 0, sizeof(cap));

  if (ioctl(fd, VIDIOC_QUERYCAP, (unsigned long)&cap) < 0)
    {
      printf("camctl: VIDIOC_QUERYCAP failed: %d\n", errno);
      return -errno;
    }

  printf("driver:       %s\n", cap.driver);
  printf("card:         %s\n", cap.card);
  printf("bus info:     %s\n", cap.bus_info);
  printf("capabilities: 0x%08" PRIx32 "\n", cap.capabilities);

  return 0;
}

static int camctl_show_formats(int fd)
{
  struct v4l2_fmtdesc fmt;
  struct v4l2_frmsizeenum size;
  unsigned int i;

  for (i = 0;; i++)
    {
      memset(&fmt, 0, sizeof(fmt));
      fmt.index = i;
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(fd, VIDIOC_ENUM_FMT, (unsigned long)&fmt) < 0)
        {
          break;
        }

      printf("format[%u]:   0x%08" PRIx32 " %s\n", i, fmt.pixelformat,
             fmt.description);
    }

  /* Frame sizes are only reported for the first format, which is the one
   * the driver is built around.
   */

  for (i = 0;; i++)
    {
      memset(&size, 0, sizeof(size));
      size.index = i;
      size.pixel_format = 0;

      if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, (unsigned long)&size) < 0)
        {
          break;
        }

      if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
        {
          printf("framesize[%u]: %ux%u\n", i, size.discrete.width,
                 size.discrete.height);
        }
    }

  return 0;
}

/****************************************************************************
 * Name: camctl_set_ctrl
 *
 * Description:
 *   Send one sensor control through VIDIOC_S_CTRL, which reaches the sensor
 *   driver's set_value().  This is what makes the exposure and gain
 *   adjustable on a running board: choosing a value for a scene means trying
 *   several, and rebuilding the firmware for each is not a practical way to
 *   do that.
 *
 *   The sensor rejects controls it does not implement rather than ignoring
 *   them, so a failure here is worth reporting and not stepping over.
 *
 ****************************************************************************/

static int camctl_set_ctrl(int fd, uint32_t id, int value,
                           FAR const char *name)
{
  struct v4l2_control ctrl;

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = id;
  ctrl.value = value;

  if (ioctl(fd, VIDIOC_S_CTRL, (unsigned long)&ctrl) < 0)
    {
      printf("camctl: setting %s = %d failed: %d\n", name, value, errno);
      return -errno;
    }

  printf("sensor:       %s = %d\n", name, value);
  return 0;
}

/****************************************************************************
 * Name: camctl_set_format
 ****************************************************************************/

static int camctl_set_format(int fd, FAR struct v4l2_format *fmt)
{
  fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (ioctl(fd, VIDIOC_S_FMT, (unsigned long)fmt) < 0)
    {
      printf("camctl: VIDIOC_S_FMT failed: %d\n", errno);
      return -errno;
    }

  printf("negotiated:   %" PRIu32 "x%" PRIu32 " format 0x%08" PRIx32
         " sizeimage %" PRIu32 "\n",
         fmt->fmt.pix.width, fmt->fmt.pix.height, fmt->fmt.pix.pixelformat,
         fmt->fmt.pix.sizeimage);

  return 0;
}

/****************************************************************************
 * Name: camctl_luma_stats
 *
 * Description:
 *   Mean luma of the Y plane and mean absolute difference against the
 *   previous frame.  Both are computed over a bounded sample: the purpose is
 *   to distinguish black from frozen from live, not to measure the picture,
 *   and walking every pixel of every frame would make the tool's own CPU
 *   time part of what it is measuring.
 *
 ****************************************************************************/

static void camctl_luma_stats(FAR const uint8_t *frame, size_t ysize,
                              FAR const uint8_t *prev, FAR int *mean_out,
                              FAR int *diff_out)
{
  size_t step = ysize / 4096u;
  uint64_t sum = 0;
  uint64_t diff = 0;
  size_t n = 0;
  size_t i;

  if (step == 0)
    {
      step = 1;
    }

  for (i = 0; i < ysize; i += step)
    {
      sum += frame[i];

      if (prev != NULL)
        {
          diff += (uint64_t)abs((int)frame[i] - (int)prev[i]);
        }

      n++;
    }

  *mean_out = n != 0 ? (int)(sum / n) : 0;
  *diff_out = (n != 0 && prev != NULL) ? (int)(diff / n) : -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *devpath = CAMCTL_DEFAULT_DEVPATH;
  FAR const char *outfile = NULL;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  FAR uint8_t *prev = NULL;
  int count = CAMCTL_DEFAULT_COUNT;
  int buftype = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int exposure = -1;
  int gain = -1;
  int fd;
  int opt;
  int ret;
  int i;
  int got = 0;
  int gaps = 0;
  int last_seq = -1;
  uint32_t seq = 0;
  uint64_t t0;
  uint64_t t1;
  size_t ysize;

  while ((opt = getopt(argc, argv, "d:c:o:e:g:")) != -1)
    {
      switch (opt)
        {
          case 'd':
            devpath = optarg;
            break;

          case 'c':
            count = atoi(optarg);
            break;

          case 'o':
            outfile = optarg;
            break;

          case 'e':
            exposure = atoi(optarg);
            break;

          case 'g':
            gain = atoi(optarg);
            break;

          default:
            camctl_usage();
            return EXIT_FAILURE;
        }
    }

  if (count <= 0)
    {
      camctl_usage();
      return EXIT_FAILURE;
    }

  fd = open(devpath, O_RDWR);
  if (fd < 0)
    {
      printf("camctl: cannot open %s: %d\n", devpath, errno);
      return EXIT_FAILURE;
    }

  ret = camctl_show_capability(fd);
  if (ret < 0)
    {
      goto errout_close;
    }

  camctl_show_formats(fd);

  /* Ask for NV12 at the size the driver is built for; the driver is the one
   * that decides, and the value it reports back is what the frames really
   * are.
   */

  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = 640;
  fmt.fmt.pix.height = 480;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  ret = camctl_set_format(fd, &fmt);
  if (ret < 0)
    {
      goto errout_close;
    }

  /* Apply the requested sensor settings before the stream starts.  Doing it
   * here rather than before the format is deliberate: the exposure ceiling
   * depends on the mode's line count, so the mode has to be settled first.
   */

  if (exposure >= 0)
    {
      ret = camctl_set_ctrl(fd, V4L2_CID_EXPOSURE_ABSOLUTE, exposure,
                            "exposure");
      if (ret < 0)
        {
          goto errout_close;
        }
    }

  if (gain >= 0)
    {
      ret = camctl_set_ctrl(fd, V4L2_CID_ISO_SENSITIVITY, gain, "gain");
      if (ret < 0)
        {
          goto errout_close;
        }
    }

  ysize = (size_t)fmt.fmt.pix.width * fmt.fmt.pix.height;

  memset(&req, 0, sizeof(req));
  req.count = CAMCTL_BUFFERS;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0)
    {
      printf("camctl: VIDIOC_REQBUFS failed: %d\n", errno);
      ret = -errno;
      goto errout_close;
    }

  printf("buffers:      %" PRIu32 "\n", req.count);

  for (i = 0; i < (int)req.count && i < CAMCTL_BUFFERS; i++)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;

      if (ioctl(fd, VIDIOC_QUERYBUF, (unsigned long)&buf) < 0)
        {
          printf("camctl: VIDIOC_QUERYBUF(%d) failed: %d\n", i, errno);
          ret = -errno;
          goto errout_close;
        }

      g_buffers[i].length = buf.length;
      g_buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);

      if (g_buffers[i].start == MAP_FAILED)
        {
          printf("camctl: mmap(%d) failed: %d\n", i, errno);
          g_buffers[i].start = NULL;
          ret = -errno;
          goto errout_close;
        }
    }

  /* Check the buffer size the driver actually handed out, not the sizeimage
   * it reported from S_FMT.
   *
   * QUERYBUF returns the length of a real buffer that has already been
   * allocated, so it is the authority on how much room a frame has.  The
   * sizeimage field in the format structure is a request/echo field: V4L2
   * lets a caller leave it zero and have the driver fill it in, so treating
   * it as a guarantee is only safe with drivers that do.  This tool used to
   * do exactly that and refused to run on a perfectly good device.
   */

  if (g_buffers[0].length < ysize + ysize / 2u)
    {
      printf("camctl: driver handed out %zu-byte buffers, too small for "
             "NV12 at %" PRIu32 "x%" PRIu32 " (%zu bytes)\n",
             g_buffers[0].length, fmt.fmt.pix.width, fmt.fmt.pix.height,
             ysize + ysize / 2u);
      ret = -EINVAL;
      goto errout_close;
    }

  for (i = 0; i < (int)req.count && i < CAMCTL_BUFFERS; i++)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;

      if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
        {
          printf("camctl: VIDIOC_QBUF(%d) failed: %d\n", i, errno);
          ret = -errno;
          goto errout_streamoff;
        }
    }

  if (ioctl(fd, VIDIOC_STREAMON, (unsigned long)&buftype) < 0)
    {
      printf("camctl: VIDIOC_STREAMON failed: %d\n", errno);
      ret = -errno;
      goto errout_close;
    }

  printf("\ncapturing %d frames...\n", count);

  t0 = camctl_now_us();

  for (i = 0; i < count; i++)
    {
      uint32_t idx;
      int mean;
      int diff;

      memset(&buf, 0, sizeof(buf));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;

      if (ioctl(fd, VIDIOC_DQBUF, (unsigned long)&buf) < 0)
        {
          printf("camctl: VIDIOC_DQBUF(%d) failed: %d\n", i, errno);
          ret = -errno;
          break;
        }

      if (buf.index >= CAMCTL_BUFFERS || g_buffers[buf.index].start == NULL)
        {
          printf("camctl: driver returned unusable buffer index %" PRIu32 "\n",
                 buf.index);
          ret = -EINVAL;
          break;
        }

      idx = buf.index;

      /* A sequence number that does not advance by one means a frame was
       * dropped somewhere between the sensor and here.
       */

      if (last_seq >= 0 && (int)buf.sequence != last_seq + 1)
        {
          gaps++;
        }

      last_seq = (int)buf.sequence;
      seq = buf.sequence;

      camctl_luma_stats(g_buffers[idx].start, ysize, prev, &mean, &diff);

      /* Save the last frame rather than the first.
       *
       * The settings written just before the stream started reach the sensor
       * over the following frame or two, so the first frames still carry the
       * previous configuration.  Saving one of those shows something other
       * than what was asked for, which is how a settings change can look as
       * though it had no effect.
       */

      if (outfile != NULL && i == count - 1)
        {
          int ofd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);

          if (ofd < 0)
            {
              printf("camctl: cannot create %s: %d\n", outfile, errno);
            }
          else
            {
              size_t nwritten =
                  write(ofd, g_buffers[idx].start, g_buffers[idx].length);

              close(ofd);
              printf("wrote frame %d (%zu of %zu bytes) to %s\n", i, nwritten,
                     g_buffers[idx].length, outfile);
            }
        }

      if (prev == NULL)
        {
          prev = malloc(ysize);
        }

      if (prev != NULL)
        {
          memcpy(prev, g_buffers[idx].start, ysize);
        }

      printf("frame %3d: seq=%-5" PRIu32 " bytesused=%-8" PRIu32
             " luma=%-4d diff=%d\n",
             got, seq, buf.bytesused, mean, diff);

      got++;

      /* Hand the buffer straight back: the framework stops the stream when
       * it runs out of vacant buffers, so holding one here would end the
       * capture rather than slow it down.
       */

      memset(&buf, 0, sizeof(buf));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = idx;

      if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
        {
          printf("camctl: VIDIOC_QBUF(%" PRIu32 ") failed: %d\n", idx, errno);
          ret = -errno;
          break;
        }
    }

  t1 = camctl_now_us();

  printf("\ncaptured %d frames, last sequence %" PRIu32 ", gaps %d\n", got,
         seq, gaps);

  if (got > 1)
    {
      uint64_t dt = t1 - t0;

      printf("elapsed %" PRIu64 " us, %.2f fps\n", dt,
             dt != 0 ? (double)got * 1000000.0 / (double)dt : 0.0);
    }

  ret = OK;

errout_streamoff:
  ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&buftype);

errout_close:
  for (i = 0; i < CAMCTL_BUFFERS; i++)
    {
      if (g_buffers[i].start != NULL)
        {
          munmap(g_buffers[i].start, g_buffers[i].length);
          g_buffers[i].start = NULL;
        }
    }

  if (prev != NULL)
    {
      free(prev);
    }

  close(fd);
  return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
