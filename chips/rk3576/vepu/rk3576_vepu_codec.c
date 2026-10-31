/****************************************************************************
 * chips/rk3576/vepu/rk3576_vepu_codec.c
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
 * RK3576 VEPU510 as a V4L2 memory-to-memory video encoder.
 *
 * A job is started by the ioctl that queued it and finished by a work queue
 * item when the interrupt arrives.  It used to be encoded synchronously,
 * inside that ioctl, and the reason it is not any more is the application:
 * the wait is about 24 ms on a 1296x960 picture, and a program that is also
 * serving its sockets cannot spend that inside a queue operation.  The queue
 * operation now returns as soon as the encoder is running, and the frame is
 * published -- codec_capture_put_buf(), which is what moves the container
 * past vbuf_next and wakes a poller -- from the completion instead.
 *
 * That moves the job across two contexts, which is the whole of the cost: the
 * containers and the picture description have to live in the per-open state
 * between the halves, and the two contexts have to be kept out of each
 * other's way (priv->lock).  The split in rk3576_vepu.c between
 * rk3576_vepu_start() and rk3576_vepu_finish() is what makes the halves
 * possible; the hardware's own lock is held between them, so one job is in
 * the encoder at a time and the completion is what lets the next one in.
 *
 * Waiting for the bitstream is therefore the application's, and the device's
 * poll() is how it waits -- it is not a queue operation that blocks until a
 * frame is ready.
 *
 * Two container states are worth being precise about, because the framework
 * and this driver have to agree on them:
 *
 *   vbuf_empty  the pool of buffer containers, refilled by VIDIOC_DQBUF and
 *               drawn from by VIDIOC_QBUF
 *   vbuf_next   the container the driver is currently working on
 *
 * A queued container is not dequeueable until vbuf_next has moved past it,
 * which video_framebuff_capture_done() does.  So putting a frame in a
 * capture buffer and then calling codec_capture_put_buf() is what publishes
 * it; without that call the application would never see it, and calling it
 * before the data is in place would publish a half-written buffer.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_RK3576_VEPU_CODEC

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/videoio.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/lib/lib.h>
#include <nuttx/mutex.h>
#include <nuttx/video/v4l2_m2m.h>
#include <nuttx/wqueue.h>

#include "rk3576_dma_alloc.h"
#include "rk3576_vepu.h"
#include "rk3576_vepu_codec.h"
#include "vepu510_regs.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Geometry has to land on the encoder's 16-pixel macroblock grid, and the
 * stride is taken to 16 bytes so that the chroma plane of an NV12 frame
 * starts on a boundary the hardware is happy with.
 */

#define RK3576_VEPU_ALIGN_W      16u
#define RK3576_VEPU_ALIGN_STRIDE 16u

/* The AArch64 D-Cache line, in bytes.  A buffer the encoder is given has to
 * start and end on one, because the cache maintenance around a job is done a
 * line at a time: a buffer that began or ended part-way through a line would
 * have the rest of that line cleaned or invalidated along with it.
 */

#define RK3576_VEPU_CACHE_LINE 64u

/* How many buffers each side offers.  Four is enough for an application to
 * keep the encoder fed while it drains the other side, and a queue this deep
 * is now what lets frames be queued ahead of the one being encoded rather
 * than one at a time.
 */

#define RK3576_VEPU_BUF_CNT 4

/* Upper bound on one encoded frame, as a fraction of the raw picture.
 *
 * Incompressible noise measured about 45% of the raw size at QP 26, so a
 * full raw picture is a bound with room to spare, and it is the same number
 * the application is told to allocate.  The encoder is given one byte less
 * than this as its limit, so a frame that somehow did not fit would stop at
 * the limit rather than run off the end of the buffer.
 */

#define RK3576_VEPU_BS_FACTOR_NUM 3u
#define RK3576_VEPU_BS_FACTOR_DEN 2u

/* How many allocations this driver will have outstanding at once.
 *
 * The framework allocates one heap per side and frees it before replacing
 * it, so two would be enough; four leaves room for an application that
 * rearranges its buffers without the table silently refusing an allocation
 * it should have been able to make.
 */

#define RK3576_VEPU_HEAP_MAX 4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-open state.  The format belongs to the file handle rather than to the
 * device, as it does in V4L2 generally, so two opens cannot be made to
 * disagree about the geometry by one of them setting it.
 */

struct rk3576_vepu_codec_priv_s
{
  FAR void *cookie; /* what the framework passes to codec_* */
  bool output_streaming;
  bool capture_streaming;

  /* Negotiated geometry and format. */

  uint32_t width; /* coded width, on the macroblock grid */
  uint32_t height;
  uint32_t y_stride; /* bytes per luma row                    */
  uint32_t v_stride; /* luma rows allocated                   */
  uint32_t src_size; /* NV12 bytes                            */
  uint32_t dst_size; /* capture buffer size we advertise      */

  /* Encoder configuration, in the form the register layer consumes. */

  struct rk3576_h264_cfg_s cfg;

  /* The buffer a frame is encoded into before it is copied out.  Encoding
   * into the application's capture buffer directly would tie the encode to
   * the availability of one, and the application may well not have returned
   * one yet when it queues the frame being encoded.
   */

  FAR uint8_t *staging;

  /* The parameter sets, built when the configuration changes and emitted in
   * front of the stream's first frame.
   */

  uint8_t header[RK3576_H264_SPS_MAX + RK3576_H264_PPS_MAX];
  uint32_t header_len;

  /* What this driver has handed out from the DMA heap.  The framework frees
   * by address alone, and this heap needs the size back to know how much to
   * release, so the pairs are kept here and looked up by address.
   */

  struct
  {
    FAR void *addr;
    size_t size;
  } heap[RK3576_VEPU_HEAP_MAX];

  /* The group of pictures.
   *
   * This is the whole of the driver's encoder-side GOP state, and it is
   * exactly what MPP's h264e_dpb.c derives: an IDR resets frame_num and the
   * picture order count, and every other picture in the group continues
   * them.  frame_num advances by one per picture and the POC by two,
   * because the picture order count this driver uses counts in frames on the
   * same scale as frame_num and there are no B pictures to interleave.
   *
   * gop_index is the picture's place in its group.  gop of 1 makes every
   * picture an IDR, which is the all-intra stream this driver produced
   * before it could predict; gop of 2 gives one IDR and one P that predicts
   * from it, which is the driver's default and the shortest predictive group
   * there is.  Everything else follows from gop_index: whether this picture is
   * an
   * IDR is gop_index == 0, and nothing in the driver needs to know the group
   * length for any other purpose.
   */

  uint32_t gop;
  uint32_t gop_index;
  uint32_t frame_num;
  uint32_t poc_lsb;
  uint32_t idr_pic_id;
  bool force_idr; /* the reference chain is broken */

  uint32_t frame_index; /* numbers the buffers handed to the app */
  bool header_sent;     /* has SPS+PPS gone out this stream      */
  bool eos_pending;     /* flush was asked for                   */

  /* The job that is with the hardware, while it is there.
   *
   * An encode is now started by the queue operation that feeds it and
   * finished later, when the interrupt has been taken, so everything the
   * second half needs has to survive the first half's stack frame.  That is
   * the two containers the job was given -- they must not be looked up twice,
   * and the frame must not be published before the encoder has finished
   * writing it -- and the picture and slice descriptions the finish call
   * takes, which are copied here rather than kept alive on a caller's stack.
   *
   * job_eos is the flush decision as it stood when the job was started.  A
   * flush ends the stream on the last frame that was queued when it arrived,
   * so which frame that is has to be recorded at the start; asking at
   * completion time would answer for whatever was queued by then instead.
   *
   * lock is what keeps two contexts out of this state at once.  The queue
   * operation that starts a job and the work item that finishes it are
   * different threads, and both have to look at the pending counters and
   * take containers from the framework's queues.
   */

  struct work_s work; /* the completion, queued when the job starts */
  mutex_t lock;
  bool job_running; /* the hardware has this job and has not finished it */
  bool job_eos;     /* this job carries the end of the stream */
  FAR struct v4l2_buffer *job_cbuf;
  FAR struct v4l2_buffer *job_obuf;
  struct rk3576_vepu510_frame_s job_frm;
  struct rk3576_vepu510_slice_s job_slice;
  struct rk3576_vepu_result_s job_result;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int vepu_open(FAR void *cookie, FAR void **priv);
static int vepu_close(FAR void *priv);
static int vepu_querycap(FAR void *priv, FAR struct v4l2_capability *cap);
static int vepu_output_enum_fmt(FAR void *priv, FAR struct v4l2_fmtdesc *fmt);
static int vepu_capture_enum_fmt(FAR void *priv, FAR struct v4l2_fmtdesc *fmt);
static int vepu_output_try_fmt(FAR void *priv, FAR struct v4l2_format *fmt);
static int vepu_output_s_fmt(FAR void *priv, FAR struct v4l2_format *fmt);
static int vepu_output_g_fmt(FAR void *priv, FAR struct v4l2_format *fmt);
static int vepu_capture_try_fmt(FAR void *priv, FAR struct v4l2_format *fmt);
static int vepu_capture_s_fmt(FAR void *priv, FAR struct v4l2_format *fmt);
static int vepu_capture_g_fmt(FAR void *priv, FAR struct v4l2_format *fmt);
static size_t vepu_output_g_bufsize(FAR void *priv);
static size_t vepu_capture_g_bufsize(FAR void *priv);
static size_t vepu_output_g_bufcnt(FAR void *priv);
static size_t vepu_capture_g_bufcnt(FAR void *priv);
static FAR void *vepu_alloc_buf(FAR void *priv, size_t size);
static void vepu_free_buf(FAR void *priv, FAR void *addr);
static int vepu_output_streamon(FAR void *priv);
static int vepu_capture_streamon(FAR void *priv);
static int vepu_output_streamoff(FAR void *priv);
static int vepu_capture_streamoff(FAR void *priv);
static int vepu_output_available(FAR void *priv);
static int vepu_capture_available(FAR void *priv);
static int vepu_output_s_parm(FAR void *priv,
                              FAR struct v4l2_streamparm *parm);
static int vepu_output_g_parm(FAR void *priv,
                              FAR struct v4l2_streamparm *parm);
static int vepu_g_ext_ctrls(FAR void *priv,
                            FAR struct v4l2_ext_controls *ctrls);
static int vepu_s_ext_ctrls(FAR void *priv,
                            FAR struct v4l2_ext_controls *ctrls);
static int vepu_encoder_cmd(FAR void *priv, FAR struct v4l2_encoder_cmd *cmd);
static int vepu_output_try_memory(FAR void *priv, enum v4l2_memory mem);
static int vepu_capture_try_memory(FAR void *priv, enum v4l2_memory mem);

/* The completion of a job, which the start half queues as work -- see
 * vepu_service_locked() for why it is separate.
 */

static void vepu_job_work(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Designated initializers rather than positional ones: this table is forty
 * entries long and several of its members have names similar enough that a
 * positional list is one insertion away from silently shifting everything
 * after it onto the wrong handler.
 */

static const struct codec_ops_s g_vepu_ops = {
  .open = vepu_open,
  .close = vepu_close,
  .querycap = vepu_querycap,

  .output_enum_fmt = vepu_output_enum_fmt,
  .capture_enum_fmt = vepu_capture_enum_fmt,
  .output_try_fmt = vepu_output_try_fmt,
  .output_s_fmt = vepu_output_s_fmt,
  .output_g_fmt = vepu_output_g_fmt,
  .capture_try_fmt = vepu_capture_try_fmt,
  .capture_s_fmt = vepu_capture_s_fmt,
  .capture_g_fmt = vepu_capture_g_fmt,

  .output_g_bufsize = vepu_output_g_bufsize,
  .capture_g_bufsize = vepu_capture_g_bufsize,
  .output_g_bufcnt = vepu_output_g_bufcnt,
  .capture_g_bufcnt = vepu_capture_g_bufcnt,
  .alloc_buf = vepu_alloc_buf,
  .free_buf = vepu_free_buf,

  .output_streamon = vepu_output_streamon,
  .capture_streamon = vepu_capture_streamon,
  .output_streamoff = vepu_output_streamoff,
  .capture_streamoff = vepu_capture_streamoff,
  .output_available = vepu_output_available,
  .capture_available = vepu_capture_available,

  .output_s_parm = vepu_output_s_parm,
  .output_g_parm = vepu_output_g_parm,
  .g_ext_ctrls = vepu_g_ext_ctrls,
  .s_ext_ctrls = vepu_s_ext_ctrls,
  .encoder_cmd = vepu_encoder_cmd,
  .output_try_memory = vepu_output_try_memory,
  .capture_try_memory = vepu_capture_try_memory,
};

static struct codec_s g_vepu_codec = {
  .ops = &g_vepu_ops,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: vepu_align
 ****************************************************************************/

static uint32_t vepu_align(uint32_t value, uint32_t alignment)
{
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static void vepu_apply_geometry(FAR struct rk3576_vepu_codec_priv_s *priv,
                                uint32_t width, uint32_t height)
{
  priv->width = width;
  priv->height = height;
  priv->y_stride = vepu_align(width, RK3576_VEPU_ALIGN_STRIDE);
  priv->v_stride = vepu_align(height, RK3576_VEPU_ALIGN_W);

  priv->src_size = priv->y_stride * priv->v_stride *
                   RK3576_VEPU_BS_FACTOR_NUM / RK3576_VEPU_BS_FACTOR_DEN;
  priv->dst_size = priv->src_size;

  priv->cfg.width = width;
  priv->cfg.height = height;
}

/****************************************************************************
 * Name: vepu_build_header
 *
 * Description:
 *   Generate the parameter sets for the current configuration and keep them
 *   for the start of the stream.
 *
 *   Generated here, when the format or a syntax control changes, rather than
 *   when the first frame is encoded: the parameter sets have to match the
 *   configuration they are emitted with, and doing it at the point the
 *   configuration changes is the only place that can be guaranteed.
 *
 ****************************************************************************/

static void vepu_build_header(FAR struct rk3576_vepu_codec_priv_s *priv)
{
  uint32_t len = 0;

  priv->header_len = 0;

  if (rk3576_h264_sps_write(priv->header, sizeof(priv->header), &priv->cfg,
                            &len) == 0)
    {
      priv->header_len += len;
    }

  if (rk3576_h264_pps_write(priv->header + priv->header_len,
                            sizeof(priv->header) - priv->header_len,
                            &priv->cfg, &len) == 0)
    {
      priv->header_len += len;
    }

  /* The parameter sets go in front of the first frame of a stream, so they
   * have to be regenerated whenever the stream restarts.  A stream that
   * began from a streamoff may have been watched by nobody; the next one
   * starts with its own header.
   */

  priv->header_sent = false;
}

/****************************************************************************
 * Name: vepu_try_pix
 *
 * Description:
 *   Negotiate a picture format on the output side.
 *
 *   Only NV12 is accepted, because the register layer only produces correct
 *   source-plane offsets for the formats it was ported and compared against
 *   -- and because the camera path this exists to serve delivers NV12.
 *
 ****************************************************************************/

static int vepu_try_pix(FAR struct v4l2_pix_format *pix)
{
  uint32_t y_stride;
  uint32_t v_stride;

  if (pix->pixelformat != V4L2_PIX_FMT_NV12)
    {
      return -EINVAL;
    }

  if (pix->width == 0 || pix->height == 0)
    {
      return -EINVAL;
    }

  y_stride = vepu_align(pix->width, RK3576_VEPU_ALIGN_STRIDE);
  v_stride = vepu_align(pix->height, RK3576_VEPU_ALIGN_W);

  pix->bytesperline = y_stride;
  pix->sizeimage = y_stride * v_stride * RK3576_VEPU_BS_FACTOR_NUM /
                   RK3576_VEPU_BS_FACTOR_DEN;
  pix->field = V4L2_FIELD_NONE;
  pix->colorspace = V4L2_COLORSPACE_REC709;

  return OK;
}

/****************************************************************************
 * Name: vepu_buffer_reachable
 *
 * Description:
 *   Say whether the encoder's own hardware can be given a buffer: cache-line
 *   aligned at both ends, and physically inside the low 4 GiB.
 *
 *   This is the requirement the DMA masters on this part share, and it is
 *   stated here in terms of the address rather than in terms of which
 *   allocator produced it, because that is what the hardware sees.  A buffer
 *   from the DMA heap satisfies it; so would one from anywhere else that did,
 *   which is the point -- the frame the capture driver demosaiced is one
 *   such buffer, and a check that asked "is this mine" would refuse it.
 *
 *   The whole extent has to fit, not just its start: the address register is
 *   32 bits wide, and a buffer that began below 4 GiB and crossed the
 *   boundary would make the encoder wrap and write somewhere unrelated.
 *
 *   The same test is made on the other side, by the driver that allocates the
 *   frame heap, and the two are the same rule -- changing one without the
 *   other would let a buffer through one door and not the other.
 *
 ****************************************************************************/

static bool vepu_buffer_reachable(FAR const void *addr, size_t size)
{
  uintptr_t phys;

  if (addr == NULL || size == 0)
    {
      return false;
    }

  if (((uintptr_t)addr & (RK3576_VEPU_CACHE_LINE - 1u)) != 0 ||
      (size & (RK3576_VEPU_CACHE_LINE - 1u)) != 0)
    {
      return false;
    }

  /* A page-table walk in a build with a kernel address space, the identity
   * in a flat one; either way this is the number the hardware is given.
   */

  phys = up_addrenv_va_to_pa((FAR void *)addr);

  return phys <= 0xffffffffu && phys + size - 1u <= 0xffffffffu;
}

/****************************************************************************
 * Name: vepu_buffer_usable
 *
 * Description:
 *   Say whether a buffer is one the encoder may be pointed at.
 *
 *   The framework makes a queued buffer's address out of the offset the
 *   application supplied, and accepts whatever that produces without
 *   looking at it.  An application that queues by index alone -- which is
 *   what a V4L2 application does, index being the field that decides which
 *   buffer is meant there -- leaves the offset at zero, and the framework
 *   then hands this driver an address that is simply some other address.
 *   Nothing goes wrong at that point, because the address is only stored;
 *   it goes wrong when the frame is read from it or the bitstream is written
 *   to it, which is far away from the mistake and looks like a hardware
 *   fault.
 *
 *   So the address is checked, and what it is checked against is the
 *   hardware's own requirement -- 64-byte alignment, and the whole buffer
 *   physically inside the low 4 GiB -- rather than against a list of this
 *   driver's allocations.  The two answer different questions, and the one
 *   that matters here is the hardware's.  The reason is the input side: a
 *   frame the capture driver demosaiced can be encoded where it lies, with
 *   no copy between them, and the address the application then queues was
 *   produced by another driver.  Asking "did I allocate this" would have
 *   refused exactly that buffer by construction, while the hardware question
 *   accepts it and still refuses the address the mistake above produces.
 *
 *   A buffer of this driver's own is still recognised as such first, because
 *   the table of allocations knows each one's extent and the hardware test
 *   cannot -- it is given a length and trusts it.
 *
 *   Alignment is not decoration either: the cache maintenance around a job
 *   is done a line at a time, so a buffer that starts or ends part-way
 *   through a line would have that line cleaned or invalidated along with
 *   whatever shares it.
 *
 * Input Parameters:
 *   state - the per-open state, holding the table of its own allocations
 *   addr  - the address to check
 *   size  - how many bytes the encoder will read or write there
 *
 * Returned Value:
 *   true if the encoder may be given that buffer
 *
 ****************************************************************************/

static bool vepu_buffer_usable(FAR struct rk3576_vepu_codec_priv_s *state,
                               FAR const void *addr, size_t size)
{
  uintptr_t a = (uintptr_t)addr;
  int i;

  if (addr == NULL)
    {
      return false;
    }

  /* This driver's own allocations first, because the table records each
   * one's extent and so can be checked exactly, where the hardware test
   * below is only told the length it is given.
   */

  for (i = 0; i < RK3576_VEPU_HEAP_MAX; i++)
    {
      uintptr_t base = (uintptr_t)state->heap[i].addr;

      if (base != 0 && a >= base && a < base + state->heap[i].size)
        {
          return true;
        }
    }

  return vepu_buffer_reachable(addr, size);
}

/****************************************************************************
 * Name: vepu_check_buf
 *
 * Description:
 *   Check a queued buffer and complain usefully if it is not one the
 *   encoder can be pointed at.
 *
 *   The message names the one thing an application has to get right here:
 *   the offset a queue operation carries is not decoration, it is how the
 *   framework finds the buffer, and it only has the right value if the
 *   application has asked for the buffer first and kept what it was told.
 *   A queue operation built from scratch with only the index in it does not
 *   fail, and does not look wrong -- which is why saying so is worth the
 *   lines.
 *
 * Input Parameters:
 *   state - the per-open state, holding the table of allocations
 *   side  - "output" or "capture", for the message
 *   buf   - the queued buffer
 *   size  - how many bytes the encoder will read or write there
 * Returned Value:
 *   true if the buffer may be used
 *
 ****************************************************************************/

static bool vepu_check_buf(FAR struct rk3576_vepu_codec_priv_s *state,
                           FAR const char *side,
                           FAR const struct v4l2_buffer *buf, size_t size)
{
  if (vepu_buffer_usable(state, buf->m.vaddr, size))
    {
      return true;
    }

  _err("ERROR: VEPU0 codec %s buffer %" PRIu32 " resolved to %p, which is"
       " not memory the encoder can be given: %zu bytes at a 64-byte aligned"
       " address, wholly below 4 GiB, is what it needs.  A queue operation"
       " that names a buffer this driver allocated must also carry the"
       " m.offset that VIDIOC_QUERYBUF reported for it, not the index"
       " alone.\n",
       side, buf->index, buf->m.vaddr, size);

  return false;
}

/****************************************************************************
 * Name: vepu_flag_eos
 *
 * Description:
 *   Mark a capture buffer as the end of the stream, and announce it.
 *
 *   A flush is a protocol marker rather than a picture, so a buffer that
 *   carries the marker and nothing else has to say it carries nothing: its
 *   length is cleared, because leaving the field alone would leave whatever
 *   the container's last use wrote there, and the application would take it
 *   for a frame of that length.  A buffer that also holds the stream's last
 *   frame keeps its length and takes the marker on top of the frame's own
 *   flag.
 *
 *   Both the flag and the event are needed.  The flag is what tells a
 *   consumer that reads buffers that this is the end of them, and the event
 *   is what tells one that is waiting on the descriptor rather than counting.
 *
 ****************************************************************************/

static void vepu_flag_eos(FAR struct rk3576_vepu_codec_priv_s *priv,
                          FAR struct v4l2_buffer *cbuf, bool encoded)
{
  struct v4l2_event evt;

  if (encoded)
    {
      cbuf->flags |= V4L2_BUF_FLAG_LAST;
    }
  else
    {
      cbuf->bytesused = 0;
      cbuf->flags = V4L2_BUF_FLAG_LAST;
    }

  priv->eos_pending = false;

  memset(&evt, 0, sizeof(evt));
  evt.type = V4L2_EVENT_EOS;
  codec_queue_event(priv->cookie, &evt);
}

/****************************************************************************
 * Name: vepu_service_locked
 *
 * Description:
 *   Start one queued frame, if there is one and somewhere to put it.
 *
 *   Called from whichever of the two sides has just been given a buffer,
 *   because either can be the last thing the application does before the
 *   encoder can proceed: frames without capture buffers would otherwise sit
 *   pending forever, and so would capture buffers behind a missing frame.
 *
 *   This is the starting half of a job and returns as soon as the hardware
 *   has been given one.  The wait, and everything that publishes or discards
 *   the result, belongs to vepu_job_work(), which is queued here as work and
 *   runs once the interrupt has been taken.  The reason for the split is the
 *   caller: this runs inside the queue operation the application is blocked
 *   on, and the ~24 ms a picture takes is precisely the time that
 *   application needed for everything else it does.
 *
 *   Nothing is dequeued or published by this half, so a failure before the
 *   job starts leaves both queues as they were and the application's next
 *   queue operation retries the same frame.
 *
 *   Called with priv->lock held -- see vepu_service().
 *
 ****************************************************************************/

static int vepu_service_locked(FAR struct rk3576_vepu_codec_priv_s *priv)
{
  FAR struct v4l2_buffer *obuf;
  FAR struct v4l2_buffer *cbuf;
  struct rk3576_vepu510_frame_s frm;
  struct rk3576_vepu510_slice_s slice;
  bool idr;
  int ret;

  if (!priv->output_streaming || !priv->capture_streaming)
    {
      return OK;
    }

  /* One job at a time, which is the hardware's rule rather than this
   * driver's.  A frame that arrives while one is running is left where it
   * is, in the framework's queue, and the completion starts it when the
   * encoder is free again -- so a refusal here is the same back pressure a
   * capture buffer that has not been returned yet applies, and not a lost
   * frame.
   */

  if (priv->job_running)
    {
      return OK;
    }

  /* The capture buffer is taken first.  It is what the frame is copied
   * into, so a frame is only worth encoding once there is somewhere for it
   * to go -- and with this order a full capture queue simply means the frame
   * stays pending, which is back pressure rather than a lost frame.
   */

  cbuf = codec_capture_get_buf(priv->cookie);
  if (cbuf == NULL)
    {
      return OK;
    }

  if (!vepu_check_buf(priv, "capture", cbuf, priv->dst_size))
    {
      return -EINVAL;
    }

  /* A queued frame is what there is to encode, and the framework's output
   * queue is what says whether there is one.  A flag of this driver's own
   * kept in step with that queue is not the same question: it can say "one
   * more" and no more than that, so two frames queued before the first has
   * been started read as no frame at all, and the second is left behind.
   */

  obuf = codec_output_get_buf(priv->cookie);

  if (obuf == NULL)
    {
      /* Nothing to encode.  A flush that has nothing left in front of it ends
       * the stream here, on an empty buffer: there is no frame to carry the
       * marker, so the buffer itself becomes it.
       *
       * With no flush pending there is nothing to do at all, and the capture
       * buffer must be left where it is.  Handing it back would count as
       * publishing it -- video_framebuff_capture_done() moves the queue's
       * cursor past it -- and the frame the driver is waiting for would find
       * the container it was going to fill had already been passed over.
       */

      if (priv->eos_pending)
        {
          vepu_flag_eos(priv, cbuf, false);
          codec_capture_put_buf(priv->cookie, cbuf);
        }

      return OK;
    }

  if (!vepu_check_buf(priv, "output", obuf, priv->src_size))
    {
      return -EINVAL;
    }

  memset(&frm, 0, sizeof(frm));
  frm.src_fmt = RK3576_VEPU510_FMT_YUV420SP;
  frm.rbuv_swap = 0; /* NV12: the chroma pairs are already in Cb,Cr order */
  frm.width = priv->width;
  frm.height = priv->height;
  frm.y_stride = priv->y_stride;
  frm.v_stride = priv->v_stride;
  frm.src_phys = (uint32_t)(uintptr_t)obuf->m.vaddr;
  frm.src_size = priv->src_size;
  frm.dst_phys = (uint32_t)(uintptr_t)priv->staging;
  frm.dst_size = priv->dst_size;
  frm.dst_offset = 0;

  /* Where this picture sits in its group of pictures.
   *
   * An IDR is asked for at the start of every group, after a failure that
   * broke the reference chain (force_idr), and whenever the group is a
   * single picture.  The three conditions are the same decision, so they are
   * made once here rather than re-derived at each use below.
   */

  idr = priv->force_idr || priv->gop_index == 0;
  priv->force_idr = false;

  memset(&slice, 0, sizeof(slice));
  slice.idr = idr;

  if (idr)
    {
      /* frame_num and the picture order count both restart, and the picture
       * id has to differ from the previous IDR's -- consecutively so, which
       * is why it is the last one's that is remembered rather than a counter
       * that could be reset by anything else. */

      priv->frame_num = 0;
      priv->poc_lsb = 0;
      priv->idr_pic_id = (priv->idr_pic_id + 1u) & 1u;
    }

  slice.frame_num = priv->frame_num;
  slice.poc_lsb = priv->poc_lsb;
  slice.idr_pic_id = priv->idr_pic_id;

  ret = rk3576_vepu_start(&frm, &priv->cfg, &slice);
  if (ret < 0)
    {
      /* The reference chain is only as good as the last picture in it, and
       * this picture did not become one -- possibly after writing part of
       * itself into a reconstruction buffer.  Whatever is in there now cannot
       * be predicted from, so the stream has to restart with an IDR rather
       * than build P pictures on it.  Nothing is published: the frame stays
       * queued and the application's next queue operation retries it, which
       * is also how it learns that the queue operation itself failed.
       */

      priv->force_idr = true;
      priv->gop_index = 0;
      return ret;
    }

  /* The job is with the hardware, and the mutex inside rk3576_vepu_start()
   * is held for it until the finish half lets it go.  Everything that half
   * needs is kept here, because the stack it would have been on is about to
   * be gone.  job_eos is the flush decision as it stands now rather than as
   * it will stand when the job ends: a flush ends the stream on the last
   * frame that was queued when it arrived, and a frame queued after it is
   * not that frame.  Taking the pending flag with it is what lets a second
   * flush, arriving while this job runs, be answered on its own.
   */

  priv->job_cbuf = cbuf;
  priv->job_obuf = obuf;
  priv->job_frm = frm;
  priv->job_slice = slice;
  priv->job_eos = priv->eos_pending;
  priv->eos_pending = false;
  priv->job_running = true;

  /* HPWORK rather than LPWORK, deliberately.  The completion is what lets go
   * of the lock the encoder is held by, so the next frame cannot start until
   * it has run; LPWORK belongs to the camera driver's demosaic job of about
   * 15 ms, and queueing behind that would add it to every frame period.  What
   * the completion does is one cache invalidation, one copy of the bitstream
   * and a few list operations, so the high-priority queue delays nothing for
   * long.
   */

  work_queue(HPWORK, &priv->work, vepu_job_work, priv, 0);
  return OK;
}

/****************************************************************************
 * Name: vepu_service
 *
 * Description:
 *   Start the next frame, if the encoder is free for one.
 *
 *   The lock is taken here because the callers are different threads: the
 *   queue operations that run in the application's context, and the
 *   completion that runs in the work queue's.  Both look at the pending
 *   counters and draw containers from the framework's queues, so they have
 *   to be one at a time.
 *
 ****************************************************************************/

static int vepu_service(FAR struct rk3576_vepu_codec_priv_s *priv)
{
  int ret;

  nxmutex_lock(&priv->lock);
  ret = vepu_service_locked(priv);
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: vepu_job_complete
 *
 * Description:
 *   The finishing half of a job: everything vepu_service() used to do after
 *   the encoder returned.
 *
 *   The order is what makes the result visible at the right moment.  The
 *   frame is copied into the capture buffer first, so that the buffer is
 *   whole before anything can find it; the output container goes back next,
 *   because the source picture has been read and the application is waiting
 *   for it; and the capture container goes back last, because that call is
 *   what publishes the frame and wakes whoever is waiting for it.
 *
 *   A job that failed has no error return left to make: the queue operation
 *   that submitted it returned to the application before the hardware had
 *   finished.  What the application gets instead is a buffer with nothing in
 *   it, which is the same "nothing came of this one" a flush's marker uses
 *   and something an application already has to handle; what it cannot learn
 *   from that buffer is why, which is why the failure is logged where it is
 *   found and the consequence is logged here.
 *
 *   Called with priv->lock held, from vepu_job_work().
 *
 ****************************************************************************/

static void vepu_job_complete(FAR struct rk3576_vepu_codec_priv_s *priv,
                              int ret)
{
  FAR struct v4l2_buffer *cbuf = priv->job_cbuf;
  uint32_t total;
  bool encoded = false;

  if (ret >= 0)
    {
      /* The parameter sets go in front of the stream's first frame, so that
       * what comes out of the device is something a decoder can be pointed
       * at directly instead of needing them supplied separately.  Only the
       * first: an application that wants them repeated can ask for that when
       * it needs it, and repeating them per frame would put them inside
       * anything that later tries to mux the stream into a container.
       *
       * The order matters.  The encoded frame is already at the start of the
       * staging buffer, because that is where the encoder was told to write
       * it, so the header cannot simply be put there -- doing that would
       * overwrite the first bytes of the frame with a start code and a
       * sequence parameter set, and the result would still begin with a start
       * code and still parse far enough to look plausible.  The frame is
       * moved up to make room and the header goes in front of it.
       */

      total = priv->job_result.bs_length;

      if (!priv->header_sent)
        {
          total += priv->header_len;

          if (total <= priv->dst_size)
            {
              memmove(priv->staging + priv->header_len, priv->staging,
                      priv->job_result.bs_length);
              memcpy(priv->staging, priv->header, priv->header_len);
              priv->header_sent = true;
            }
        }

      if (total > priv->dst_size)
        {
          /* The frame is whole and the encoder did its job, but it cannot be
           * handed over: the parameter sets have to go in front of it and
           * there is nowhere for them to go.  This is the one failure that is
           * not the hardware's, and the one thing that must not happen is
           * publishing half of it. */

          _err("ERROR: VEPU0 codec %" PRIu32 "-byte frame and a %" PRIu32
               "-byte header do not fit a %" PRIu32 "-byte capture buffer\n",
               priv->job_result.bs_length, priv->header_len, priv->dst_size);
        }
      else
        {
          memcpy(cbuf->m.vaddr, priv->staging, total);

          cbuf->bytesused = total;

          /* Whether a consumer may start here.  It is the one thing about
           * this frame that cannot be discovered by reading it -- a picture's
           * own bytes do not say whether anything after it predicts from it
           * -- and it is what an application needs in order to put fragment
           * boundaries or container key-frame markers in the right places.
           * Reported from the same flag that chose the slice type, so the two
           * cannot disagree.
           */

          cbuf->flags = priv->job_slice.idr ? V4L2_BUF_FLAG_KEYFRAME
                                            : V4L2_BUF_FLAG_PFRAME;

          /* This picture is now the reference the next one predicts from, so
           * the group advances.  The order matters: the counters are moved on
           * only after the encode succeeded, because a picture that was never
           * reconstructed is not a place in the sequence.
           */

          priv->gop_index++;
          if (priv->gop_index >= priv->gop)
            {
              priv->gop_index = 0;
            }

          priv->frame_num = (priv->frame_num + 1u) &
                            rk3576_vepu510_frame_num_mask(&priv->cfg);
          priv->poc_lsb =
              (priv->poc_lsb + 2u) & rk3576_vepu510_poc_lsb_mask(&priv->cfg);

          encoded = true;
        }
    }

  if (!encoded)
    {
      _err("ERROR: VEPU0 codec job %" PRIu32
           " produced no frame; an empty buffer goes back in its place and"
           " the stream restarts with an IDR\n",
           priv->frame_index);

      priv->force_idr = true;
      priv->gop_index = 0;

      /* Nothing valid to publish, so the buffer is made to say so.  Clearing
       * the flags as well as the length matters: whatever flag the container
       * carried from its last use would otherwise be read as a property of
       * this frame. */

      cbuf->bytesused = 0;
      cbuf->flags = 0;
    }

  /* The buffer's place in the sequence of buffers handed over, which advances
   * for a failed job as well: the application is given that buffer whether or
   * not there is a frame in it, and two buffers arriving with the same number
   * would be a thing a consumer could reasonably read something into.
   */

  cbuf->sequence = priv->frame_index;
  priv->frame_index++;

  codec_output_put_buf(priv->cookie, priv->job_obuf);

  if (priv->job_eos)
    {
      vepu_flag_eos(priv, cbuf, encoded);
    }

  codec_capture_put_buf(priv->cookie, cbuf);
}

/****************************************************************************
 * Name: vepu_job_work
 *
 * Description:
 *   Finish the job vepu_service_locked() started, on the work queue.
 *
 *   Neither half of this can be done in the interrupt handler.  The wait has
 *   to be somewhere that is allowed to sleep, and the tail does cache
 *   maintenance over the staging buffer and touches the framework's buffer
 *   lists, which an interrupt handler may not do either.  The interrupt only
 *   posts the semaphore the wait is on; the work is what collects it.
 *
 *   See vepu_service_locked() for why this is on HPWORK rather than LPWORK.
 *
 ****************************************************************************/

static void vepu_job_work(FAR void *arg)
{
  FAR struct rk3576_vepu_codec_priv_s *priv = arg;
  int ret;

  ret =
      rk3576_vepu_finish(&priv->job_frm, &priv->job_slice, &priv->job_result);

  nxmutex_lock(&priv->lock);
  vepu_job_complete(priv, ret);
  priv->job_running = false;
  nxmutex_unlock(&priv->lock);

  /* The hardware is free again, and a frame that arrived while this job was
   * running has been waiting for exactly this.  Nothing else is going to
   * look at the queues now, so the next job is started from here.
   */

  ret = vepu_service(priv);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 codec could not start the next frame: %d\n", ret);
    }
}

/****************************************************************************
 * Name: vepu_job_settle
 *
 * Description:
 *   Wait for the job in flight, if there is one, and drop what it produced.
 *
 *   Called where the device is being wound down -- a stream stop, or the last
 *   open being closed.  The two containers the job holds are about to stop
 *   meaning anything: the framework frees them when the last file is closed,
 *   and an application that stopped the stream may ask for its buffers back
 *   and free the pool before a completion left in the work queue would have
 *   reached it.  So the completion is taken off the queue here, or waited for
 *   if it is already running, and whatever is still with the hardware is
 *   waited for; the frame it produces is not published, because the stream it
 *   would have belonged to is over.
 *
 *   A job cannot be stopped once started -- there is no abort that leaves the
 *   encoder usable -- so the wait is the job's own, tens of milliseconds at
 *   worst.
 *
 ****************************************************************************/

static void vepu_job_settle(FAR struct rk3576_vepu_codec_priv_s *priv)
{
  /* A queued completion is removed and a running one is waited for, which is
   * also what covers a completion that started the next job before it
   * noticed the stream was stopping: that job is then still running and is
   * waited for below.  The call has nothing to report -- no completion queued
   * is the ordinary case and not a fault.
   */

  work_cancel_sync(HPWORK, &priv->work);

  nxmutex_lock(&priv->lock);
  if (priv->job_running)
    {
      rk3576_vepu_finish(&priv->job_frm, &priv->job_slice, &priv->job_result);
      priv->job_running = false;
    }

  nxmutex_unlock(&priv->lock);
}

static int vepu_open(FAR void *cookie, FAR void **priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state;

  state = kmm_zalloc(sizeof(*state));
  if (state == NULL)
    {
      return -ENOMEM;
    }

  /* The framework hands the cookie to open() and to nothing else, so it is
   * kept: every later call needs it to reach the buffer queues.
   */

  state->cookie = cookie;

  /* A usable default, so that an application which only sets a format has a
   * complete configuration rather than one with zeroed syntax fields in it.
   */

  state->cfg.profile_idc = RK3576_VEPU_DEFAULT_PROFILE;
  state->cfg.level_idc = RK3576_VEPU_DEFAULT_LEVEL;
  state->cfg.pic_init_qp = RK3576_VEPU_DEFAULT_QP;
  state->cfg.frame_qp = RK3576_VEPU_DEFAULT_QP;
  state->cfg.log2_max_frame_num_minus4 = 12;
  state->cfg.log2_max_poc_lsb_minus4 = 12;
  state->cfg.poc_type = 0;
  state->cfg.num_ref_frames = 1;
  state->cfg.direct8x8_inference = 1;
  state->cfg.entropy_coding_mode = 0;
  state->cfg.deblocking_filter_control = 1;
  state->cfg.vui_en = 1;
  state->cfg.full_range = 0;
  state->cfg.fps_num = 30;
  state->cfg.fps_den = 1;

  /* The profile and entropy mode are left at the defaults set with the rest
   * of this block: Baseline, CAVLC.  That is deliberate rather than an
   * oversight, and the reason is worth keeping because the combination looks
   * like a gap.
   *
   * The vendor's own stack runs Main profile with CABAC, and switching this
   * driver to match is not a one-line change: a CABAC picture that is not an
   * I picture has to carry cabac_init_idc in its slice header, and the syntax
   * layer does not write that field.  Turning the entropy mode on without it
   * produces a malformed stream, not a faster one.  Baseline with CAVLC is
   * legal, this driver's I pictures decode exactly, and -- after the
   * anti-smear correction in vepu510_regs.c -- so do its P pictures.  So the
   * profile is not what was wrong and is not worth changing blind.
   */

  /* MPP's own tuning defaults, so the encoder runs with the rate-distortion
   * setup the register layer was verified against rather than with whatever
   * a zeroed structure happens to mean. */

  state->cfg.tune.scene_mode = 0; /* not IPC */
  state->cfg.tune.atl_str = 1;
  state->cfg.tune.atr_str_i = 1;
  state->cfg.tune.atf_str = 1;
  state->cfg.tune.lambda_idx_i = 6;

  /* The deblur tuning, which the anti-smear thresholds are derived from.
   * MPP's defaults, and left at them: MPP's own name for deblur_en by the time
   * the register layer sees it is qpmap_en, and it is what selects between two
   * of those threshold sets.  Nothing here exposes it to an application,
   * because the feature it belongs to is a per-block refinement that this
   * driver has no way to configure the rest of.
   */

  state->cfg.tune.deblur_en = RK3576_H264_DEBLUR_EN_DEFAULT;
  state->cfg.tune.deblur_str = RK3576_H264_DEBLUR_STR_DEFAULT;

  /* The default group length, which is the driver's rather than this layer's
   * -- see RK3576_VEPU_DEFAULT_GOP.  An application that knows how its stream
   * is watched should ask for what it wants; the streaming application in
   * app/camenc does.
   */

  state->gop = RK3576_VEPU_DEFAULT_GOP;

  vepu_apply_geometry(state, 640, 480);

  /* The lock the application's context and the work queue share.  A zeroed
   * mutex is not an unlocked one, so this is not something the allocation
   * could have left to do.
   */

  nxmutex_init(&state->lock);

  *priv = state;
  return OK;
}

static int vepu_close(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  if (state == NULL)
    {
      return OK;
    }

  /* A frame may still be with the hardware, and its completion may still be
   * in the work queue.  Both have to be over before the state they use is
   * freed: the completion would otherwise run against freed memory, and the
   * hardware's lock would stay held for a stream that no longer exists.  The
   * frame the job was producing is dropped -- nobody is left to read it.
   */

  vepu_job_settle(state);

  if (state->staging != NULL)
    {
      rk3576_dma_free(state->staging, state->dst_size);
    }

  nxmutex_destroy(&state->lock);
  kmm_free(state);
  return OK;
}

static int vepu_querycap(FAR void *priv, FAR struct v4l2_capability *cap)
{
  UNUSED(priv);

  strlcpy((FAR char *)cap->driver, "rk3576-vepu", sizeof(cap->driver));
  strlcpy((FAR char *)cap->card, "RK3576 VEPU510 H.264 encoder",
          sizeof(cap->card));
  strlcpy((FAR char *)cap->bus_info, "platform:vepu0", sizeof(cap->bus_info));
  cap->version = 1;

  /* Memory-to-memory and streaming: the encoder is fed from memory and
   * writes to memory, and it uses the buffer queue model rather than read().
   * Not device-caps-before-media-bus, because there is no media bus here.
   */

  cap->capabilities = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
  cap->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;

  return OK;
}

static int vepu_output_enum_fmt(FAR void *priv, FAR struct v4l2_fmtdesc *fmt)
{
  UNUSED(priv);

  if (fmt->index != 0)
    {
      return -EINVAL;
    }

  fmt->pixelformat = V4L2_PIX_FMT_NV12;
  strlcpy((FAR char *)fmt->description, "Y/CbCr 4:2:0",
          sizeof(fmt->description));

  return OK;
}

static int vepu_capture_enum_fmt(FAR void *priv, FAR struct v4l2_fmtdesc *fmt)
{
  UNUSED(priv);

  if (fmt->index != 0)
    {
      return -EINVAL;
    }

  /* The stream is annex-B: every NAL unit carries a start code, including
   * the parameter sets, which means it can be written to a file and played
   * without anything having to re-add them.
   */

  fmt->pixelformat = V4L2_PIX_FMT_H264;
  strlcpy((FAR char *)fmt->description, "H.264 annex-B",
          sizeof(fmt->description));

  return OK;
}

static int vepu_output_try_fmt(FAR void *priv, FAR struct v4l2_format *fmt)
{
  UNUSED(priv);

  if (fmt->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
    {
      return -EINVAL;
    }

  return vepu_try_pix(&fmt->fmt.pix);
}

static int vepu_output_s_fmt(FAR void *priv, FAR struct v4l2_format *fmt)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;
  int ret;

  ret = vepu_output_try_fmt(priv, fmt);
  if (ret < 0)
    {
      return ret;
    }

  /* The staging buffer belongs to the geometry.  An application is entitled
   * to set the same format twice, and allocating again without releasing
   * what is already there would leak the whole frame buffer each time.
   */

  if (state->staging != NULL && state->dst_size != fmt->fmt.pix.sizeimage)
    {
      rk3576_dma_free(state->staging, state->dst_size);
      state->staging = NULL;
    }

  vepu_apply_geometry(state, fmt->fmt.pix.width, fmt->fmt.pix.height);

  if (state->staging == NULL)
    {
      state->staging = rk3576_dma_alloc(state->dst_size);
      if (state->staging == NULL)
        {
          _err("ERROR: VEPU0 codec out of DMA heap for a %" PRIu32
               "-byte frame buffer\n",
               state->dst_size);
          return -ENOMEM;
        }
    }

  vepu_build_header(state);
  return OK;
}

static int vepu_output_g_fmt(FAR void *priv, FAR struct v4l2_format *fmt)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  if (fmt->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
    {
      return -EINVAL;
    }

  memset(&fmt->fmt.pix, 0, sizeof(fmt->fmt.pix));
  fmt->fmt.pix.width = state->width;
  fmt->fmt.pix.height = state->height;
  fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
  fmt->fmt.pix.bytesperline = state->y_stride;
  fmt->fmt.pix.sizeimage = state->src_size;
  fmt->fmt.pix.field = V4L2_FIELD_NONE;
  fmt->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;

  return OK;
}

static int vepu_capture_try_fmt(FAR void *priv, FAR struct v4l2_format *fmt)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
    {
      return -EINVAL;
    }

  if (fmt->fmt.pix.pixelformat != V4L2_PIX_FMT_H264)
    {
      return -EINVAL;
    }

  /* The capture geometry follows the output's.  There is no scaler in this
   * path, so a capture size that disagrees with the coded size is not a
   * smaller picture, it is a size the encoder cannot be told to produce.
   */

  if (fmt->fmt.pix.width != 0 && fmt->fmt.pix.width != state->width)
    {
      return -EINVAL;
    }

  if (fmt->fmt.pix.height != 0 && fmt->fmt.pix.height != state->height)
    {
      return -EINVAL;
    }

  fmt->fmt.pix.width = state->width;
  fmt->fmt.pix.height = state->height;
  fmt->fmt.pix.sizeimage = state->dst_size;
  fmt->fmt.pix.field = V4L2_FIELD_NONE;
  fmt->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;

  return OK;
}

static int vepu_capture_s_fmt(FAR void *priv, FAR struct v4l2_format *fmt)
{
  return vepu_capture_try_fmt(priv, fmt);
}

static int vepu_capture_g_fmt(FAR void *priv, FAR struct v4l2_format *fmt)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
    {
      return -EINVAL;
    }

  memset(&fmt->fmt.pix, 0, sizeof(fmt->fmt.pix));
  fmt->fmt.pix.width = state->width;
  fmt->fmt.pix.height = state->height;
  fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
  fmt->fmt.pix.sizeimage = state->dst_size;
  fmt->fmt.pix.field = V4L2_FIELD_NONE;
  fmt->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;

  return OK;
}

static size_t vepu_output_g_bufsize(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  return state->src_size;
}

static size_t vepu_capture_g_bufsize(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  return state->dst_size;
}

static size_t vepu_output_g_bufcnt(FAR void *priv)
{
  UNUSED(priv);
  return RK3576_VEPU_BUF_CNT;
}

static size_t vepu_capture_g_bufcnt(FAR void *priv)
{
  UNUSED(priv);
  return RK3576_VEPU_BUF_CNT;
}

static FAR void *vepu_alloc_buf(FAR void *priv, size_t size)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;
  FAR void *addr;
  int i;

  /* Every buffer the encoder touches has to be physically contiguous, below
   * 4 GB and 64-byte aligned, which is what the DMA heap provides and what
   * the pass-through MMU route requires. */

  addr = rk3576_dma_alloc(size);
  if (addr == NULL)
    {
      return NULL;
    }

  for (i = 0; i < RK3576_VEPU_HEAP_MAX; i++)
    {
      if (state->heap[i].addr == NULL)
        {
          state->heap[i].addr = addr;
          state->heap[i].size = size;
          return addr;
        }
    }

  /* Out of bookkeeping, not out of memory: refusing here keeps the table
   * honest about what it can free again, which matters more than the
   * allocation. */

  _err("ERROR: VEPU0 codec is tracking %d allocations already\n",
       RK3576_VEPU_HEAP_MAX);
  rk3576_dma_free(addr, size);
  return NULL;
}

static void vepu_free_buf(FAR void *priv, FAR void *addr)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;
  int i;

  if (addr == NULL)
    {
      return;
    }

  for (i = 0; i < RK3576_VEPU_HEAP_MAX; i++)
    {
      if (state->heap[i].addr == addr)
        {
          rk3576_dma_free(addr, state->heap[i].size);
          state->heap[i].addr = NULL;
          state->heap[i].size = 0;
          return;
        }
    }

  /* Freeing something this driver did not hand out means the size is not
   * known, and guessing one would corrupt the heap.  Said out loud rather
   * than ignored, because it would mean the framework and the driver
   * disagree about who owns a buffer.
   */

  _err("ERROR: VEPU0 codec asked to free %p, which it did not allocate\n",
       addr);
}

static int vepu_output_streamon(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;
  int ret;

  if (state->staging == NULL)
    {
      return -EINVAL; /* no format set, so nothing to encode into */
    }

  /* A stream begins with a picture that stands on its own.  A decoder
   * pointed at the first buffer of this stream has nothing before it to
   * predict from, and whatever group the output was in the middle of is
   * gone -- so the counters restart with it rather than continuing from a
   * stream that no longer exists.
   */

  state->gop_index = 0;
  state->frame_num = 0;
  state->poc_lsb = 0;
  state->force_idr = true;

  /* And the hardware begins where the first stream of the run began.
   *
   * Restarting the counters is not enough, for the same reason that rewriting
   * the registers is not: the block and the working buffers it reads both
   * carry something from the stream before this one, and neither is cleared
   * by anything in a mode change.  A stream started after the capture mode
   * changed was wrong while the first stream of the run was right, and
   * switching back did not repair it -- which is the shape of state that
   * outlives the stream that made it.  Closing this device node releases a
   * staging buffer and nothing else, and opening it again writes the same
   * registers the first stream did.
   *
   * So a stream start is a cold start: the block is soft reset here as well
   * as in rk3576_vepu_recn_alloc(), which zeroes the working set it takes
   * when the geometry changes.  The two are separate because they cover
   * different cases -- a restart at the same size reuses the buffers and
   * never goes near the allocator -- and the cost is a few register writes
   * per stream, against a fault that is invisible until a decoder shows it.
   */

  ret = rk3576_vepu_reset();
  if (ret < 0)
    {
      _err("ERROR: VEPU0 could not be reset for a new stream: %d\n", ret);
      return ret;
    }

  state->output_streaming = true;
  return vepu_service(state);
}

static int vepu_capture_streamon(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  state->capture_streaming = true;
  return vepu_service(state);
}

static int vepu_output_streamoff(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  /* Dropping a queued frame rather than finishing it is deliberate: the
   * application has said it no longer wants what it queued, and the next
   * stream starts with its own parameter sets.
   *
   * The frame that is already with the hardware cannot be dropped the same
   * way -- there is no stopping one once it has started -- so it is waited
   * for and its result discarded.  Clearing the stream flag first is what
   * keeps the completion, if it is still to run, from starting another job
   * behind this one.
   */

  state->output_streaming = false;
  state->eos_pending = false;

  vepu_job_settle(state);

  return OK;
}

static int vepu_capture_streamoff(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  state->capture_streaming = false;

  /* Either side stopping is enough to stop encoding, so this is also a place
   * a job can still be in flight -- see vepu_output_streamoff().  A stream
   * that is restarted gets its parameter sets again, because a consumer of
   * the new stream was not necessarily watching the old one. */

  vepu_job_settle(state);

  state->header_sent = false;

  return OK;
}

static int vepu_output_available(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  return vepu_service(state);
}

static int vepu_capture_available(FAR void *priv)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  return vepu_service(state);
}

static int vepu_output_s_parm(FAR void *priv, FAR struct v4l2_streamparm *parm)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;
  uint32_t num;
  uint32_t den;

  if (parm->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
    {
      return -EINVAL;
    }

  num = parm->parm.output.timeperframe.numerator;
  den = parm->parm.output.timeperframe.denominator;

  if (num != 0 && den != 0)
    {
      /* V4L2 gives seconds per frame; the syntax layer wants frames per
       * second, as a fraction it can write into the VUI.  Keeping it a
       * fraction rather than dividing means 30000/1001 stays exact.
       */

      state->cfg.fps_num = den;
      state->cfg.fps_den = num;
      vepu_build_header(state);
    }

  return vepu_output_g_parm(priv, parm);
}

static int vepu_output_g_parm(FAR void *priv, FAR struct v4l2_streamparm *parm)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  if (parm->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
    {
      return -EINVAL;
    }

  memset(&parm->parm.output, 0, sizeof(parm->parm.output));
  parm->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
  parm->parm.output.timeperframe.numerator = state->cfg.fps_den;
  parm->parm.output.timeperframe.denominator = state->cfg.fps_num;

  return OK;
}

static int vepu_g_ext_ctrls(FAR void *priv,
                            FAR struct v4l2_ext_controls *ctrls)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;
  uint16_t i;

  for (i = 0; i < ctrls->count; i++)
    {
      FAR struct v4l2_ext_control *ctrl = &ctrls->controls[i];

      switch (ctrl->id)
        {
          case RK3576_VEPU_CID_QP:
            ctrl->value = (int32_t)state->cfg.frame_qp;
            break;

          case RK3576_VEPU_CID_PROFILE:
            ctrl->value = (int32_t)state->cfg.profile_idc;
            break;

          case RK3576_VEPU_CID_LEVEL:
            ctrl->value = (int32_t)state->cfg.level_idc;
            break;

          case RK3576_VEPU_CID_DEBLOCK:
            ctrl->value = (int32_t)state->cfg.deblocking_filter_control;
            break;

          case RK3576_VEPU_CID_GOP:
            ctrl->value = (int32_t)state->gop;
            break;

          default:
            ctrls->error_idx = i;
            return -EINVAL;
        }
    }

  return OK;
}

static int vepu_s_ext_ctrls(FAR void *priv,
                            FAR struct v4l2_ext_controls *ctrls)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;
  bool syntax_changed = false;
  uint16_t i;

  /* Validated in full before anything is applied, so that a set containing
   * one bad control does not leave the configuration half-changed -- the
   * application is told which control was rejected and the encoder is still
   * describing the stream it was describing before.
   */

  for (i = 0; i < ctrls->count; i++)
    {
      FAR struct v4l2_ext_control *ctrl = &ctrls->controls[i];

      switch (ctrl->id)
        {
          case RK3576_VEPU_CID_QP:
            if (ctrl->value < 0 || ctrl->value > 51)
              {
                ctrls->error_idx = i;
                return -EINVAL;
              }
            break;

          case RK3576_VEPU_CID_PROFILE:
            if (ctrl->value != 66 && ctrl->value != 77 && ctrl->value != 100)
              {
                ctrls->error_idx = i;
                return -EINVAL;
              }
            break;

          case RK3576_VEPU_CID_LEVEL:
            if (ctrl->value < 0 || ctrl->value > 62)
              {
                ctrls->error_idx = i;
                return -EINVAL;
              }
            break;

          case RK3576_VEPU_CID_DEBLOCK:
            if (ctrl->value != 0 && ctrl->value != 1)
              {
                ctrls->error_idx = i;
                return -EINVAL;
              }
            break;

          case RK3576_VEPU_CID_GOP:
            if (ctrl->value < 1 || ctrl->value > RK3576_VEPU_GOP_MAX)
              {
                ctrls->error_idx = i;
                return -EINVAL;
              }
            break;

          default:
            ctrls->error_idx = i;
            return -EINVAL;
        }
    }

  for (i = 0; i < ctrls->count; i++)
    {
      FAR struct v4l2_ext_control *ctrl = &ctrls->controls[i];

      switch (ctrl->id)
        {
          case RK3576_VEPU_CID_QP:
            state->cfg.pic_init_qp = (uint32_t)ctrl->value;
            state->cfg.frame_qp = (uint32_t)ctrl->value;
            break;

          case RK3576_VEPU_CID_PROFILE:
            state->cfg.profile_idc = (uint32_t)ctrl->value;
            syntax_changed = true;
            break;

          case RK3576_VEPU_CID_LEVEL:
            state->cfg.level_idc = (uint32_t)ctrl->value;
            syntax_changed = true;
            break;

          case RK3576_VEPU_CID_DEBLOCK:
            state->cfg.deblocking_filter_control = (uint32_t)ctrl->value;
            syntax_changed = true;
            break;

          case RK3576_VEPU_CID_GOP:
            state->gop = (uint32_t)ctrl->value;

            /* Changing the group length does not change what any parameter
             * set says, so the sets do not need rebuilding.  It does mean
             * the picture being encoded when the control arrives might be in
             * the wrong place for the new length, which is why the group
             * restarts instead of being resized underneath itself.
             */

            state->gop_index = 0;
            break;
        }
    }

  /* Only the syntax controls change what the parameter sets say, so only
   * they need them regenerated.  QP is carried in the slice header the
   * hardware writes, which is why it is not with them. */

  if (syntax_changed)
    {
      vepu_build_header(state);
    }

  return OK;
}

static int vepu_encoder_cmd(FAR void *priv, FAR struct v4l2_encoder_cmd *cmd)
{
  FAR struct rk3576_vepu_codec_priv_s *state = priv;

  switch (cmd->cmd)
    {
      case V4L2_ENC_CMD_STOP:
        /* Where the stream ends is marked here, but when it ends is not: a
         * frame already with the hardware is the last one, and this call
         * returns before it has been encoded.  The flag is therefore taken
         * up by that job's completion -- or by this call, if there is no
         * frame in flight to carry it -- and the application waits for the
         * marked buffer the same way it waits for any other.
         */

        state->eos_pending = true;
        return vepu_service(state);

      case V4L2_ENC_CMD_START:
      case V4L2_ENC_CMD_PAUSE:
      case V4L2_ENC_CMD_RESUME:
        /* Nothing to do: encoding is driven by queued frames, and there is
         * no separate encoder state to start, pause or resume.  Accepted
         * rather than rejected, because an application that sends them
         * expects the stream to continue, which it does. */

        return OK;

      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Name: vepu_try_memory
 *
 * Description:
 *   Accept only MMAP, and say why the alternatives are refused.
 *
 *   The encoder's MMU is left in pass-through, so the addresses it is given
 *   have to be physical.  Only this driver knows how to produce memory that
 *   is, and a USERPTR is an address the application chose -- one that a
 *   userspace malloc would satisfy while being neither contiguous nor low
 *   enough in physical memory.  Programming that into the encoder writes the
 *   bitstream somewhere unpredictable, so it is refused at the point the
 *   application asks for it rather than going wrong later.
 *
 *   The framework's default accepts both, which is why this has to be
 *   implemented rather than left out.
 *
 ****************************************************************************/

static int vepu_try_memory(enum v4l2_memory mem)
{
  return mem == V4L2_MEMORY_MMAP ? OK : -ENOTTY;
}

static int vepu_output_try_memory(FAR void *priv, enum v4l2_memory mem)
{
  UNUSED(priv);

  /* The input side accepts memory the application supplies as well as memory
   * this driver allocates.
   *
   * That is what a frame handed over in place needs: the capture driver
   * demosaiced into its own buffer, and there is nothing to copy into a
   * buffer of ours, so the application names the address it already holds.
   * The address still has to be one the encoder can reach -- see
   * vepu_buffer_usable() and vepu_check_buf(), which stand between this and
   * the registers -- so a plain malloc'd pointer is refused where it is used
   * rather than here, where the format it has to fit is not yet known.
   *
   * The capture side stays MMAP-only: the bitstream is written by the encoder
   * and the application has no address to name for it beforehand.
   */

  return (mem == V4L2_MEMORY_MMAP || mem == V4L2_MEMORY_USERPTR) ? OK
                                                                 : -ENOTTY;
}

static int vepu_capture_try_memory(FAR void *priv, enum v4l2_memory mem)
{
  UNUSED(priv);
  return vepu_try_memory(mem);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_vepu_codec_register(FAR const char *devpath)
{
  return codec_register(devpath, &g_vepu_codec);
}

int rk3576_vepu_codec_unregister(FAR const char *devpath)
{
  return codec_unregister(devpath);
}

#ifdef CONFIG_RK3576_VEPU_CODEC_SELFTEST

/****************************************************************************
 * The acceptance test.
 *
 * It drives the device the way an application has to: negotiate both
 * formats, ask for buffers, map them, queue them, stream, and collect what
 * comes back.  A shortcut through any of those steps would leave the step
 * untested, and the steps are the point -- the hardware is already covered
 * by rk3576_vepu_selftest().
 ****************************************************************************/

#define RK3576_VEPU_ST_FRAMES 3

static uint32_t g_vepu_st_len[RK3576_VEPU_ST_FRAMES];

static void rk3576_vepu_st_fill(FAR uint8_t *y, uint32_t width,
                                uint32_t height, uint32_t pattern, uint32_t w)
{
  uint32_t lcg = pattern + 1u;
  uint32_t row;
  uint32_t col;

  for (row = 0; row < height; row++)
    {
      for (col = 0; col < width; col++)
        {
          switch (pattern)
            {
              case 1: /* gradient */
                y[row * w + col] =
                    (uint8_t)((row * 255u) / height + (col * 255u) / width);
                break;

              case 2: /* noise */
                lcg = lcg * 1664525u + 1013904223u;
                y[row * w + col] = (uint8_t)((lcg >> 16) & 0xffu);
                break;

              default: /* flat */
                y[row * w + col] = 128;
                break;
            }
        }
    }
}

static int rk3576_vepu_st_mmap(int fd, FAR struct v4l2_buffer *buf,
                               FAR uint8_t **addr)
{
  *addr = mmap(NULL, buf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               (off_t)buf->m.offset);
  if (*addr == MAP_FAILED)
    {
      *addr = NULL;
      return -errno;
    }

  return OK;
}

/****************************************************************************
 * Wait for the encoder to have something on one of its queues.
 *
 * A frame is not ready when the queue operation that submitted it returns any
 * more: the job is started there and finished by the interrupt, so the
 * bitstream has to be waited for, and this is the wait an application makes --
 * poll() on the same descriptor.  The test drives it that way rather than
 * retrying the dequeue, because the wake-up is half of what the asynchronous
 * driver has to get right, and a test that never waits for it would not notice
 * it regressing.
 *
 * The wait is bounded, generously above the driver's own job timeout, so that
 * a job which never completes fails the test where it says why rather than
 * sitting in the boot for ever.
 ****************************************************************************/

#define RK3576_VEPU_ST_WAIT_MS 5000

static bool rk3576_vepu_st_wait(int fd, short events)
{
  struct pollfd pfd;
  int ret;

  pfd.fd = fd;
  pfd.events = events;
  pfd.revents = 0;

  do
    {
      ret = poll(&pfd, 1, RK3576_VEPU_ST_WAIT_MS);
    }
  while (ret < 0 && errno == EINTR);

  return ret > 0 && (pfd.revents & events) != 0;
}

int rk3576_vepu_codec_selftest(FAR const char *devpath)
{
  FAR uint8_t *obuf[RK3576_VEPU_BUF_CNT];
  FAR uint8_t *cbuf[RK3576_VEPU_BUF_CNT];

  /* The QUERYBUF results are kept and handed back on QBUF, because that is
   * what this framework's QBUF uses.  codec_qbuf() takes whichever container
   * is free and forms the buffer's address from m.offset, ignoring index
   * entirely -- so an application that queued by index alone, as V4L2
   * applications do, would have the address formed from a zero offset.  The
   * consequence is not an error return: the container is queued with a wrong
   * address in it, and the next write to it goes wherever that address
   * points.
   */

  struct v4l2_buffer ob[RK3576_VEPU_BUF_CNT];
  struct v4l2_buffer cb[RK3576_VEPU_BUF_CNT];
  struct v4l2_requestbuffers req;
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  struct v4l2_encoder_cmd cmd;
  uint32_t width = 320;
  uint32_t height = 240;
  uint32_t y_size = width * height;
  uint32_t i;
  int fd;
  int ret;

  fd = open(devpath, O_RDWR);
  if (fd < 0)
    {
      _err("ERROR: VEPU0 codec cannot open %s: %d\n", devpath, errno);
      return -errno;
    }

  ret = ioctl(fd, VIDIOC_QUERYCAP, (unsigned long)&cap);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 codec QUERYCAP failed: %d\n", errno);
      goto errout;
    }

  vinfo("VEPU0 codec: %s / %s, capabilities 0x%08" PRIx32 "\n", cap.driver,
        cap.card, cap.capabilities);

  if ((cap.capabilities & V4L2_CAP_VIDEO_M2M) == 0 ||
      (cap.capabilities & V4L2_CAP_STREAMING) == 0)
    {
      _err("ERROR: VEPU0 codec does not report mem-to-mem streaming\n");
      ret = -EIO;
      goto errout;
    }

  /* Negotiate.  The output side is the picture going in, the capture side
   * the bitstream coming out. */

  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;

  ret = ioctl(fd, VIDIOC_S_FMT, (unsigned long)&fmt);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 codec rejected NV12 %ux%u: %d\n", width, height,
           errno);
      goto errout;
    }

  vinfo("VEPU0 codec: output NV12 %" PRIu32 "x%" PRIu32 ", stride %" PRIu32
        ", size %" PRIu32 "\n",
        fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
        fmt.fmt.pix.sizeimage);

  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;

  ret = ioctl(fd, VIDIOC_S_FMT, (unsigned long)&fmt);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 codec rejected H264 capture: %d\n", errno);
      goto errout;
    }

  vinfo("VEPU0 codec: capture H264, buffer size %" PRIu32 "\n",
        fmt.fmt.pix.sizeimage);

  /* Buffers on both sides.  MMAP is the only memory mode the device takes,
   * so asking for anything else has to fail here rather than at queue time.
   */

  memset(&req, 0, sizeof(req));
  req.count = RK3576_VEPU_BUF_CNT;
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  req.memory = V4L2_MEMORY_MMAP;

  ret = ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 codec output REQBUFS failed: %d\n", errno);
      goto errout;
    }

  if (req.count < RK3576_VEPU_ST_FRAMES)
    {
      _err("ERROR: VEPU0 codec offered only %" PRIu32
           " output buffers, needs %u\n",
           req.count, (unsigned)RK3576_VEPU_ST_FRAMES);
      ret = -EIO;
      goto errout;
    }

  for (i = 0; i < req.count; i++)
    {
      memset(&ob[i], 0, sizeof(ob[i]));
      ob[i].type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      ob[i].memory = V4L2_MEMORY_MMAP;
      ob[i].index = i;

      ret = ioctl(fd, VIDIOC_QUERYBUF, (unsigned long)&ob[i]);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec output QUERYBUF %" PRIu32 " failed: %d\n",
               i, errno);
          goto errout;
        }

      ret = rk3576_vepu_st_mmap(fd, &ob[i], (FAR uint8_t **)&obuf[i]);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec output mmap %" PRIu32 " failed: %d\n", i,
               errno);
          goto errout;
        }
    }

  memset(&req, 0, sizeof(req));
  req.count = RK3576_VEPU_BUF_CNT;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  ret = ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 codec capture REQBUFS failed: %d\n", errno);
      goto errout;
    }

  if (req.count < RK3576_VEPU_BUF_CNT)
    {
      _err("ERROR: VEPU0 codec offered only %" PRIu32
           " capture buffers, needs %u\n",
           req.count, (unsigned)RK3576_VEPU_BUF_CNT);
      ret = -EIO;
      goto errout;
    }

  for (i = 0; i < req.count; i++)
    {
      memset(&cb[i], 0, sizeof(cb[i]));
      cb[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      cb[i].memory = V4L2_MEMORY_MMAP;
      cb[i].index = i;

      ret = ioctl(fd, VIDIOC_QUERYBUF, (unsigned long)&cb[i]);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec capture QUERYBUF %" PRIu32 " failed: %d\n",
               i, errno);
          goto errout;
        }

      ret = rk3576_vepu_st_mmap(fd, &cb[i], (FAR uint8_t **)&cbuf[i]);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec capture mmap %" PRIu32 " failed: %d\n", i,
               errno);
          goto errout;
        }
    }

  /* Stream on, capture first: a frame can only be encoded once there is
   * somewhere for the bitstream to go. */

  {
    uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(fd, VIDIOC_STREAMON, (unsigned long)&type);
    if (ret < 0)
      {
        _err("ERROR: VEPU0 codec capture STREAMON failed: %d\n", errno);
        goto errout;
      }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = ioctl(fd, VIDIOC_STREAMON, (unsigned long)&type);
    if (ret < 0)
      {
        _err("ERROR: VEPU0 codec output STREAMON failed: %d\n", errno);
        goto errout;
      }
  }

  /* Every capture buffer is put in the queue up front, which is what an
   * application does and what the driver relies on to have somewhere to
   * write. */

  for (i = 0; i < RK3576_VEPU_BUF_CNT; i++)
    {
      ret = ioctl(fd, VIDIOC_QBUF, (unsigned long)&cb[i]);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec capture QBUF %" PRIu32 " failed: %d\n", i,
               errno);
          goto errout;
        }
    }

  for (i = 0; i < RK3576_VEPU_ST_FRAMES; i++)
    {
      struct v4l2_buffer buf;

      /* A picture and a queue operation.  The frame is handed over there and
       * the bitstream follows from the interrupt, so the wait is between the
       * two: the queue operation has returned long before the encoder has
       * finished with the picture. */

      rk3576_vepu_st_fill(obuf[i], width, height, i, width);
      memset(obuf[i] + y_size, 128, y_size / 2);

      ob[i].bytesused = y_size * 3u / 2u;

      ret = ioctl(fd, VIDIOC_QBUF, (unsigned long)&ob[i]);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec output QBUF %" PRIu32 " failed: %d\n", i,
               errno);
          goto errout;
        }

      if (!rk3576_vepu_st_wait(fd, POLLIN))
        {
          _err("ERROR: VEPU0 codec frame %" PRIu32 " did not arrive\n", i);
          ret = -ETIMEDOUT;
          goto errout;
        }

      memset(&buf, 0, sizeof(buf));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;

      ret = ioctl(fd, VIDIOC_DQBUF, (unsigned long)&buf);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec DQBUF after frame %" PRIu32 " failed: %d\n",
               i, errno);
          goto errout;
        }

      g_vepu_st_len[i] = buf.bytesused;

      /* The bitstream has to open with a start code.  Where the parameter
       * sets appear is only checked on the first frame, because that is the
       * only place they are promised. */

      if (buf.bytesused < 5 || cbuf[buf.index][0] != 0 ||
          cbuf[buf.index][1] != 0 || cbuf[buf.index][2] != 0 ||
          cbuf[buf.index][3] != 1)
        {
          _err("ERROR: VEPU0 codec frame %" PRIu32 " has no start code\n", i);
          ret = -EIO;
          goto errout;
        }

      if (i == 0 && (cbuf[buf.index][4] & 0x1fu) != 7)
        {
          _err("ERROR: VEPU0 codec stream does not begin with a sequence"
               " parameter set (NAL type %u)\n",
               (unsigned)(cbuf[buf.index][4] & 0x1fu));
          ret = -EIO;
          goto errout;
        }

      vinfo("VEPU0 codec: frame %" PRIu32 " -> %" PRIu32
            " bytes, NAL type %u\n",
            i, buf.bytesused, (unsigned)(cbuf[buf.index][4] & 0x1fu));

      /* Hand the capture buffer straight back, so the next frame has
       * somewhere to go. */

      ret = ioctl(fd, VIDIOC_QBUF, (unsigned long)&buf);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 codec capture re-QBUF failed: %d\n", errno);
          goto errout;
        }
    }

  /* And the ordering, which is what says the encoder is responding to the
   * picture rather than to something else. */

  if (!(g_vepu_st_len[0] < g_vepu_st_len[1] &&
        g_vepu_st_len[1] < g_vepu_st_len[2]))
    {
      _err("ERROR: VEPU0 codec sizes not ordered: flat %" PRIu32
           ", gradient %" PRIu32 ", noise %" PRIu32 "\n",
           g_vepu_st_len[0], g_vepu_st_len[1], g_vepu_st_len[2]);
      ret = -EIO;
      goto errout;
    }

  /* Flush.  Nothing is buffered, so this ends the stream on the next buffer
   * handed back. */

  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = V4L2_ENC_CMD_STOP;

  ret = ioctl(fd, VIDIOC_ENCODER_CMD, (unsigned long)&cmd);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 codec ENCODER_CMD STOP failed: %d\n", errno);
      goto errout;
    }

  {
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(fd, VIDIOC_DQBUF, (unsigned long)&buf);
    if (ret < 0)
      {
        _err("ERROR: VEPU0 codec DQBUF after flush failed: %d\n", errno);
        goto errout;
      }

    if ((buf.flags & V4L2_BUF_FLAG_LAST) == 0)
      {
        _err("ERROR: VEPU0 codec flush did not mark the last buffer\n");
        ret = -EIO;
        goto errout;
      }

    /* One line, because this is the result and not a trace of it: the claim
     * the test makes is that an application can drive the encoder through
     * its own device node, and the number of frames and bytes that came back
     * is what says so.  The negotiation and per-frame detail above is behind
     * vinfo.
     *
     * The length is summed over the array rather than indexed, so that the
     * count in the message is the count the loop used.
     */

    {
      uint32_t total = 0;
      uint32_t k;

      for (k = 0; k < RK3576_VEPU_ST_FRAMES; k++)
        {
          total += g_vepu_st_len[k];
        }

      _info("VEPU0 codec self-test: %" PRIu32 " frames through the device"
            " node, %" PRIu32 " bytes of bitstream\n",
            i, total);
    }
  }

  ret = OK;

errout:
  close(fd);
  return ret;
}

#endif /* CONFIG_RK3576_VEPU_CODEC_SELFTEST */

#endif /* CONFIG_RK3576_VEPU_CODEC */
