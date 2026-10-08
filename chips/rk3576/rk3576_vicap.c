/****************************************************************************
 * chips/rk3576/rk3576_vicap.c
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
 * RK3576 VICAP capture driver (NuttX capture framework, SoC side).
 *
 * Data flow:
 *
 *   D-PHY RX -> CSI HOST -> VICAP -> RAW (DDR, ping-pong)
 *                                        |
 *                          CPU bilinear demosaic
 *                                        |
 *                                        v
 *                              NV12 (framework buffer)
 *
 * VICAP's own frame buffers are private to this driver, and its hardware
 * ping-pong alternates FRAME0/FRAME1 by itself: the driver hands over two
 * buffer addresses and the "dma end" interrupt says which one just filled.
 * The framework's buffer is never touched by DMA, which is what makes the
 * demosaic-in-the-middle design cheap to reason about -- there is no cache
 * maintenance question about a buffer two masters share.
 *
 * The demosaicer runs on the low-priority work queue, never in the
 * interrupt handler: the framework's completion callback is taken under a
 * spinlock and must not sleep, so the ISR only records which buffer filled
 * up and defers the real work.
 *
 * The demosaic pass is also where the 3A measurement is taken, and this
 * driver is where the 3A control plane lives -- see <arch/chip/cam3a.h>
 * for why the two ends are split the way they are.  Briefly: the channel sums
 * come out of the loop that is already reading every RAW sample, so measuring
 * costs nothing, and they have to be taken before the white balance gains are
 * applied and cannot be recovered from the NV12 that comes out.  Exposure and
 * gain are not here at all; they are the sensor's, and reachable through the
 * ordinary V4L2 controls on the capture device.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

/* The demosaicer has a vector path on AArch64.  Asking the compiler rather
 * than the architecture is what makes the two agree: a build that has had
 * the FP/SIMD registers taken away from it -- -mgeneral-regs-only -- would
 * reject the intrinsics, so it must fall back to the scalar form as well.
 */

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include <arch/chip/cam3a.h>
#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/fs/fs.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/imgdata.h>
#include <nuttx/wqueue.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_vicap.h"
#include "rk3576_csi_host.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_vicap.h"

#ifdef CONFIG_RK3576_VICAP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PMU (RK3576_PMU_ADDR) power-domain control for PD_VI.
 *
 * The whole VI domain -- VICAP, the five CSI HOSTs and the ISP -- shares one
 * power gate and one memory-repair initial reset.  The initial reset bit
 * resets to 0, i.e. HELD IN RESET, and until it is released every register
 * in the domain reads back as zero.  The VOP driver has the same shape for
 * its own domains, and it was bitten by exactly that.
 *
 * Writes use the hiword write-enable scheme: bits [31:16] mask which of the
 * low 16 bits are written.
 */

#define RK3576_VICAP_PMU_PWR_GATE_CON1_OFF        0x20204
#define RK3576_VICAP_PMU_PWR_GATE_SFTCON0_OFF     0x20210
#define RK3576_VICAP_PMU_BIAS_INITRST_SFTCON1_OFF 0x20544
#define RK3576_VICAP_PMU_BIU_IDLE_SFTCON0_OFF     0x20110
#define RK3576_VICAP_PMU_PWR_GATE_STS_OFF         0x20230

#define RK3576_VICAP_PD_VI_DWN_ENA                (1u << 1) /* 0 = keep powered */
#define RK3576_VICAP_PD_VI_DWN_SFTENA             (1u << 1) /* 0 = keep powered */
#define RK3576_VICAP_PD_VI_INITRST                (1u << 1) /* 1 = normal (release) */
#define RK3576_VICAP_BIU_VI_IDLE_REQ              (1u << 9) /* 0 = BIU active */
#define RK3576_VICAP_PD_VI_DWN_STAT               (1u << 17)

#define RK3576_VICAP_PMU_HWM(bits)                ((bits) << 16)

/* Poll step for the power domain coming up.  A microsecond does not come in
 * ones -- up_udelay(1) costs a call as well as the wait -- so the step and the
 * count are stated separately, and the product is the timeout.  A million
 * singles is not a second in practice; it is closer to twenty.
 */

#define RK3576_VICAP_PMU_POLL_STEP_US 10
#define RK3576_VICAP_PMU_POLL_LOOPS   (100000) /* 10 us * 1e5 = 1 s */

/* VICAP core clock.
 *
 * This is the rate the vendor device tree assigns to dclk_vicap
 * (assigned-clock-rates on the rkcif node in rk3576.dtsi), and it is the
 * only clock that node assigns -- aclk_vicap and hclk_vicap are left at
 * their reset-derived values, which is what the rest of this driver relies
 * on.  Matching the vendor's rate is deliberate: this is a new capture path
 * and there is no reason to introduce a second difference from the
 * configuration that is known to work.
 *
 * dclk_vicap selects between GPLL and CPLL, and GPLL is 1188 MHz on this
 * clock tree, so the closest achievable rate at or below 600 MHz is
 * GPLL / 2 = 594 MHz.  The driver logs the rate it actually got, which is
 * what the divider search settles on.
 */

#define RK3576_VICAP_DCLK_HZ 600000000u

/* Line stride alignment.  The TRM requires at least 8 bytes for both the
 * start address and the stride and recommends 64 for DDR efficiency; the
 * DMA heap allocator already guarantees 64, and aligning the stride to 64
 * keeps every line on the same cache-line phase.
 */

#define RK3576_VICAP_STRIDE_ALIGN 64u

/* What the DMA heap can promise for a buffer's start address, and therefore
 * the largest alignment the framework's frame heap request may ask for.  The
 * heap allocates on 64-byte granules because that is the AArch64 D-Cache
 * line; a request for more is not refused by the heap, it is simply not
 * honoured, so it is refused here instead -- see rk3576_vicap_alloc().
 */

#define RK3576_VICAP_DMA_HEAP_ALIGN 64u

/* Two ping-pong frame buffers, as the hardware expects. */

#define RK3576_VICAP_NBUF 2

/* Slack at the end of each DMA buffer, in lines.
 *
 * The DMA can be told to take the frame height from the interface rather
 * than from the height this driver configured, and when it does, what
 * bounds the write is the sensor's frame instead of our settings.  The
 * sensor's vertical total is the real ceiling on that, so each buffer
 * carries that much room past one frame.  The frame itself is still
 * rawlen bytes wide: this is the size of the allocation, not of the
 * picture, and nothing reads past rawlen.
 */

#define RK3576_VICAP_DMA_SLACK_LINES 64u

/* White balance.
 *
 * A raw sensor's three channels do not respond equally to the same light, so
 * reconstructing colour from Bayer data leaves a cast unless something
 * corrects for it.  On a sensor that outputs raw the correction belongs on
 * the host side -- the sensor's own white balance block is switched off in
 * the camera driver, and upstream hands the raw stream to userspace for
 * exactly this reason.
 *
 * The correction here is deliberately simple: it compares the frame's three
 * channel averages and pulls them together, which is the grey-world
 * assumption that a typical scene averages to neutral.  That assumption is
 * not always true, so the gains move only part of the way each frame -- the
 * result settles towards neutral without chasing whatever the current scene
 * happens to be.  Gains are eight-bit fixed point with one as 256, and are
 * bounded so a scene that really is a single colour cannot run them away.
 */

#define RK3576_VICAP_WB_ONE        256u
#define RK3576_VICAP_WB_MIN        64u   /* 0.25x */
#define RK3576_VICAP_WB_MAX        1024u /* 4x    */
#define RK3576_VICAP_WB_STEP_SHIFT 1u    /* Move 1/2 of the way per frame */

/* The largest gain the demosaic can apply at all, as opposed to the largest
 * the driver's own loop will choose.
 *
 * The two bounds are different questions and only the first is the driver's
 * to answer.  The vector path scales the gain by four and narrows it to
 * sixteen bits, so a gain above this arrives in the picture as a different
 * number than the one asked for -- which is a correctness limit this file
 * has to enforce.  Where within that range the gains should sit is a
 * question about the scene, and that one belongs to whoever is choosing
 * them.
 *
 * The number comes from the interface rather than being written here.  It is
 * a property of the control plane, and the application needs the same bound
 * to size a slider with; two copies of it would be one copy too many, and
 * the copy that drifts is always the one nobody is looking at.  See
 * CAM3A_WB_MAX in <arch/chip/cam3a.h>.
 */

#define RK3576_VICAP_WB_HARD_MAX CAM3A_WB_MAX

/* A sample at or above this is at the top of the sensor's range, where the
 * code it reports stops depending on how much light arrived.  Samples from
 * there are kept out of the white balance measurement: past full scale there
 * is no colour left to measure, only the ceiling.
 *
 * The demosaicer works in eight bits taken from the top of the ten, so this
 * is the ten-bit value 1000 shifted down by two.
 */

#define RK3576_VICAP_WB_SAT_OWN 250u

/* How much room the demosaic has to leave inside a frame interval before the
 * frame may be read where it lies.
 *
 * The just-finished buffer is stable for one frame interval: the DMA fills
 * the other buffer meanwhile, and only begins overwriting this one when the
 * frame after next starts, one interval later.  The margin is a safety
 * factor on top of that window, covering what the prediction cannot: the
 * interval is an average rather than this frame's, it came from a previous
 * frame, and the worker can be scheduled late.
 *
 * A wrong answer here is not a slow frame but a torn one, and the check that
 * is meant to catch it cannot.  That check compares the DMA-end count either
 * side of the read, and a DMA end is raised when a write *finishes* -- so a
 * write that has begun into the buffer being read, but has not finished
 * before the read does, raises nothing and is counted as no race at all.
 * The read would then be of a buffer that is partly one frame and partly
 * another, which is a region at a different level, on the frames where it
 * happens, alternating with the buffer it happens in -- and none of that is
 * something the check can see.
 *
 * Three is therefore the value in use: it does not admit a demosaic that
 * takes more than a third of the interval, which at 1296x960 (15 ms against
 * 31 ms) means every frame is copied rather than read in place.  Copying
 * costs about 8 ms a frame and drops the stream to 26 fps, which is why the
 * value below is two.
 *
 * That experiment has been run, and the answer was no: with every frame
 * copied, so that the demosaic could not have been reading a buffer the DMA
 * was writing, the fault was still there.  The read path is not where this
 * comes from, and the question above about the check's blind spot, while
 * still true, is no longer what is being chased.
 */

#define RK3576_VICAP_INPLACE_MARGIN 2u

/* How many demosaics to average before deciding whether a frame may be read
 * where it lies.
 *
 * One sample is not enough.  The first demosaics of a stream run with a cold
 * cache and are the slowest of them all, and the answer is latched, so a
 * single unlucky frame would keep the stream copying for its whole length.
 * A handful of frames costs a fraction of a second at the start and makes
 * the figure describe the mode rather than the frame.
 */

#define RK3576_VICAP_INPLACE_SAMPLES 8u

/* Bayer 2x2 colour at (row parity, column parity); 0 = R, 1 = G, 2 = B. */

static const uint8_t g_vicap_bayer[4][4] = {
  { 2, 1, 1, 0 }, /* BGGR */
  { 1, 2, 0, 1 }, /* GBRG */
  { 1, 0, 2, 1 }, /* GRBG */
  { 0, 1, 1, 2 }, /* RGGB */
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_vicap_s
{
  struct imgdata_s data; /* Must be first: the framework's handle */
  mutex_t lock;          /* Serialises the ioctl-facing entry points */
  spinlock_t irqlock;    /* Guards raw_pending against the ISR */

  uintptr_t base;
  struct rk3576_vicap_config cfg;

  struct clk_s *dclk;
  struct clk_s *aclk;
  struct clk_s *hclk;
  struct clk_s *iclk;

  uint32_t stride; /* Line stride in bytes */
  size_t rawlen;   /* One RAW frame, stride * height */
  size_t dmaalloc; /* Bytes per DMA buffer, rawlen plus the slack above */
  FAR uint8_t *raw[RK3576_VICAP_NBUF];

  /* A private copy of the frame being demosaiced.
   *
   * The hardware's two buffers only hold a frame still for one frame
   * interval, and the demosaic is longer than that, so the frame has to be
   * taken out of the DMA's reach before it can be worked on.
   */

  FAR uint8_t *snap;

  FAR uint8_t *dst; /* Framework's NV12 buffer for the next frame */
  size_t dstsize;

  imgdata_capture_t callback;
  FAR void *arg;

  struct work_s work;
  volatile int raw_pending; /* RAW buffer index awaiting demosaic, -1 none */

  /* The ping-pong buffer the last DMA end landed in.
   *
   * The pair alternates, and the hardware says which one finished by setting
   * that buffer's interrupt bit -- which is enough while the handler keeps up.
   * It is not enough when it does not: two frames can finish before this runs,
   * both bits arrive together, and the bits carry no order, so reading one of
   * them picks a buffer by accident.  Half the time that is the one the DMA is
   * writing into at that moment, and demosaicing it yields a frame whose top
   * rows belong to the previous picture -- a torn frame, which is what a
   * horizontal band of rubbish at the top and a broken Bayer grid look like.
   *
   * Remembering the last index is what makes the both-set case decidable: the
   * pair alternates, so the newer of the two is the other one.
   */

  volatile int dma_idx; /* -1 until the first DMA end of a stream */

  volatile bool frame_scheduled;

  /* Bumped on every start and every stop.  A frame carries the value it was
   * claimed under, so one that outlived its stream can be recognised and
   * dropped rather than delivered to a framework that has moved on.
   */

  uint32_t stream_epoch;

  uint32_t framecount; /* Frames delivered to the framework */
  uint32_t dropcount;  /* Frames VICAP finished but we could not take */

  /* Why those frames were dropped, counted apart.
   *
   * A single total cannot be acted on: a frame missed because the worker had
   * not finished the previous one means the host is too slow, one missed
   * because the framework had not offered a buffer means the application is,
   * and the two call for opposite responses.  Reported as a breakdown so the
   * question can be answered from the log instead of guessed at.
   */

  uint32_t drop_overrun; /* Previous frame still being demosaiced */
  uint32_t drop_nobuf;   /* Framework had not offered a buffer */
  uint32_t drop_stale;   /* Stream ended under the frame */
  uint32_t drop_queue;   /* Work queue refused the frame */
  uint32_t errstat;      /* Sticky OR of VICAP error interrupt bits */

  /* The error bits already reported to the log.
   *
   * The sticky mask above is the record; this is the record of what has been
   * said about it.  Keeping the two apart is what lets the report be made
   * from the worker rather than from the interrupt -- a bit set while the
   * worker is printing is still unseen and is picked up on the next frame,
   * where a read-and-clear would lose it.
   */

  uint32_t errstat_reported;
  uint32_t frame_us; /* Accumulated CPU time spent producing frames */

  /* Frame boundaries seen, counted where they are noticed.  A copy taken
   * across one of these may straddle two frames, so comparing the count
   * either side of the copy is what turns that from an assumption into a
   * measurement.
   */

  volatile uint32_t isr_frames;
  uint32_t racecount; /* Copies that spanned a frame boundary */
  uint32_t copy_us;   /* Accumulated time spent copying frames out */

  /* Whether a frame may be demosaiced where it lies.
   *
   * The hardware ping-pongs two buffers and alternates them itself, so the
   * one just finished is not written again until the other frame has been
   * captured: it can be read in place for one frame interval and no longer.
   * Whether the demosaic fits in that interval is a property of the mode and
   * the frame rate, so the interval and the cost are both measured and the
   * answer is derived from them rather than assumed.
   */

  volatile uint32_t isr_ticks; /* Tick at the last DMA end */
  uint32_t interval_ticks;     /* Spacing measured between them */
  uint32_t debayer_us;         /* What the last demosaic cost */
  uint32_t debayer_us_sum;     /* Accumulated demosaic time */

  /* Whether a frame may be demosaiced where it lies.
   *
   * The frame interval does not change while a stream runs, so this is a
   * property of the stream rather than of the frame: it is worked out once,
   * from the average of several demosaics and the interval measured between
   * DMA ends, and then latched.  Deciding it afresh on every frame makes it
   * flicker -- the measurement moves by a tick either way, and any threshold
   * near the demosaic time then alternates between the two paths from one
   * frame to the next.  The two paths are also measured apart, so that the
   * cost of each is known rather than only the average of both.
   */

  bool inplace_decided;
  bool inplace_ok;
  uint32_t inplace_samples; /* Demosaics summed so far */
  uint32_t inplace_frames;
  uint32_t inplace_us_sum;
  uint32_t copy_frames;
  uint32_t copy_demosaic_us_sum;

  /* The interface's own frame boundaries, counted separately from the DMA
   * ends.  The two are independent inputs to the frame that arrives: a DMA
   * end says a buffer filled up, an interface boundary says where the
   * sensor's frame actually began.  If only the first ever counts, the
   * picture is being cut by a line count rather than by the sensor's
   * frame, and the result is a rotated image.
   */

  volatile uint32_t fs_count;
  volatile uint32_t fe_count;

  /* White balance gains in eight-bit fixed point, one being 256.  Applied
   * while demosaicing and adjusted afterwards from what the frame turned
   * out to contain.
   *
   * Read *before* the gains are applied to steer them, and written in two
   * places: the driver's own loop in rk3576_vicap_wb_update(), and the
   * application through the cam3a interface.  Whether the driver is
   * steering is what awb_active says.
   */

  uint32_t wb[3];
  bool awb_active;

  /* What the last demosaiced frame measured, published for the cam3a
   * interface.  Written at the end of the demosaic pass and read from task
   * context, both under irqlock: the two are concurrent, and a caller that
   * mixed one frame's sums with the next frame's count would compute a mean
   * belonging to neither.
   *
   * stat_seq is bumped as part of the same critical section rather than by a
   * counter of its own, so that a caller comparing it can rely on the rest
   * of the snapshot having moved with it.
   */

  uint32_t stat_seq;
  uint32_t stat_sum[3];
  uint32_t stat_count;
  uint32_t stat_w;
  uint32_t stat_h;

  bool initialized;
  bool capturing;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_vicap_s g_vicap = {
  .lock = NXMUTEX_INITIALIZER,
  .raw_pending = -1,
  .wb = { RK3576_VICAP_WB_ONE, RK3576_VICAP_WB_ONE, RK3576_VICAP_WB_ONE },
  .awb_active = true,
};

/* The one outstanding capture frame heap, and how large it was.
 *
 * The framework calls alloc() once when the application asks for buffers and
 * free() once when it lets them go, and free() is handed the address and
 * nothing else.  The granule allocator needs the length as well, so it is
 * remembered here.  One slot is enough because there is one heap per device
 * at a time, and having more than one outstanding would be a fault in the
 * caller rather than a case to support -- see rk3576_vicap_alloc().
 */

static FAR void *g_vicap_frameheap;
static size_t g_vicap_frameheap_size;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_vicap_init(FAR struct imgdata_s *data);
static int rk3576_vicap_uninit(FAR struct imgdata_s *data);
static int rk3576_vicap_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR uint8_t *addr, uint32_t size);
static int rk3576_vicap_validate_frame_setting(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval);
static int rk3576_vicap_start_capture(FAR struct imgdata_s *data,
                                      uint8_t nr_datafmts,
                                      FAR imgdata_format_t *datafmts,
                                      FAR imgdata_interval_t *interval,
                                      FAR imgdata_capture_t callback,
                                      FAR void *arg);
static int rk3576_vicap_stop_capture(FAR struct imgdata_s *data);
static FAR void *rk3576_vicap_alloc(FAR struct imgdata_s *data,
                                    uint32_t align_size, uint32_t size);
static void rk3576_vicap_free(FAR struct imgdata_s *data, FAR void *addr);
static void rk3576_vicap_wb_update(FAR struct rk3576_vicap_s *priv,
                                   uint64_t accr, uint64_t accg, uint64_t accb,
                                   uint64_t accn);

static const struct imgdata_ops_s g_rk3576_vicap_ops = {
  .init = rk3576_vicap_init,
  .uninit = rk3576_vicap_uninit,
  .set_buf = rk3576_vicap_set_buf,
  .validate_frame_setting = rk3576_vicap_validate_frame_setting,
  .start_capture = rk3576_vicap_start_capture,
  .stop_capture = rk3576_vicap_stop_capture,

  /* The NV12 frames come from the DMA heap rather than the system heap.
   *
   * Everything about the demosaic is still the CPU's: it writes the picture
   * in place and cleans the cache afterwards, exactly as before, and a
   * reader of that memory through mmap sees the same bytes either way.  What
   * changes is who can be *given* the memory.  The encoder's MMU is left in
   * pass-through, so the addresses its registers carry have to be physical,
   * and this is the only heap on the part that is identity-mapped -- it is
   * how the encoder already gets its own working memory.  Putting the frames
   * here is therefore what makes it possible to hand a finished frame
   * straight to the encoder with no copy between them.
   *
   * The system heap could not be used for that even in principle: an address
   * from it is neither contiguous nor low enough in physical memory to
   * program into a DMA master.
   */

  .alloc = rk3576_vicap_alloc,
  .free = rk3576_vicap_free,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vicap_alloc
 *
 * Description:
 *   Provide the framework's capture buffer heap from the DMA heap.
 *
 *   Called once per REQBUFS, before any buffer is mapped or used, and the
 *   region it returns is what every capture buffer is carved from.  The
 *   alignment the framework asks for is a cache-line one; the DMA heap
 *   already promises 64 bytes and the AArch64 line is 64, so the request is
 *   checked rather than acted on.
 *
 *   A second allocation without an intervening free is refused rather than
 *   satisfied.  The framework does free first, so seeing one means the two
 *   callers disagree about the shape of the protocol, and quietly leaking
 *   the first heap -- or worse, handing back the same address twice -- would
 *   hide that.
 *
 ****************************************************************************/

static FAR void *rk3576_vicap_alloc(FAR struct imgdata_s *data,
                                    uint32_t align_size, uint32_t size)
{
  FAR void *addr;

  UNUSED(data);

  if (size == 0)
    {
      return NULL;
    }

  if (align_size > RK3576_VICAP_DMA_HEAP_ALIGN)
    {
      _err("ERROR: VICAP: a %" PRIu32 "-byte frame heap alignment was asked"
           " for, and the DMA heap can only promise %u\n",
           align_size, (unsigned int)RK3576_VICAP_DMA_HEAP_ALIGN);
      return NULL;
    }

  if (g_vicap_frameheap != NULL)
    {
      _err("ERROR: VICAP: a frame heap is already outstanding at %p (%zu"
           " bytes); the framework frees before it allocates\n",
           g_vicap_frameheap, g_vicap_frameheap_size);
      return NULL;
    }

  addr = rk3576_dma_alloc((size_t)size);
  if (addr == NULL)
    {
      _err("ERROR: VICAP: no %" PRIu32 "-byte run in the DMA heap for the"
           " frame buffers\n",
           size);
      return NULL;
    }

  g_vicap_frameheap = addr;
  g_vicap_frameheap_size = (size_t)size;

  _info("VICAP: frame heap %p..%p (%" PRIu32 " bytes, DMA heap)\n", addr,
        (FAR uint8_t *)addr + size, size);

  return addr;
}

/****************************************************************************
 * Name: rk3576_vicap_free
 *
 * Description:
 *   Return the capture frame heap to the DMA heap.
 *
 *   Only the region this driver handed out is accepted.  A free of anything
 *   else is a fault in the caller and not something to pass on to the
 *   granule allocator, which would take the address at its word and corrupt
 *   whatever allocation really owns it.
 *
 ****************************************************************************/

static void rk3576_vicap_free(FAR struct imgdata_s *data, FAR void *addr)
{
  UNUSED(data);

  if (addr == NULL)
    {
      return;
    }

  if (addr != g_vicap_frameheap)
    {
      _err("ERROR: VICAP: asked to free %p, which is not the frame heap"
           " (%p)\n",
           addr, g_vicap_frameheap);
      return;
    }

  rk3576_dma_free(addr, g_vicap_frameheap_size);

  g_vicap_frameheap = NULL;
  g_vicap_frameheap_size = 0;
}

/****************************************************************************
 * Name: rk3576_vicap_getreg / putreg
 ****************************************************************************/

static inline uint32_t rk3576_vicap_getreg(uintptr_t base, uint32_t offset)
{
  return getreg32(base + offset);
}

static inline void rk3576_vicap_putreg(uintptr_t base, uint32_t offset,
                                       uint32_t value)
{
  putreg32(value, base + offset);
}

/****************************************************************************
 * Name: rk3576_vicap_power_domain_on
 *
 * Description:
 *   Release the PD_VI memory-repair initial reset, make sure the domain is
 *   not asked to power down, and leave the VI bus interface unit active.
 *   Until the initial reset is released every register in the domain reads
 *   back zero, which looks exactly like a driver that never programmed
 *   anything.
 *
 ****************************************************************************/

static int rk3576_vicap_power_domain_on(void)
{
  uintptr_t pmu = RK3576_PMU_ADDR;
  int loops = RK3576_VICAP_PMU_POLL_LOOPS;

  /* 1. Release the memory-repair initial reset (write 1 = Normal). */

  putreg32(RK3576_VICAP_PMU_HWM(RK3576_VICAP_PD_VI_INITRST) |
               RK3576_VICAP_PD_VI_INITRST,
           pmu + RK3576_VICAP_PMU_BIAS_INITRST_SFTCON1_OFF);
  up_udelay(20);

  /* 2. Keep the domain powered: both the hardware and the software power
   *    down request bits are written to 0.
   */

  putreg32(RK3576_VICAP_PMU_HWM(RK3576_VICAP_PD_VI_DWN_ENA),
           pmu + RK3576_VICAP_PMU_PWR_GATE_CON1_OFF);
  putreg32(RK3576_VICAP_PMU_HWM(RK3576_VICAP_PD_VI_DWN_SFTENA),
           pmu + RK3576_VICAP_PMU_PWR_GATE_SFTCON0_OFF);

  /* 3. Do not hold the VI bus interface unit idle.  A module whose BIU is
   *    held idle programs and reads its registers perfectly while being
   *    unable to issue a single AXI transaction.
   */

  putreg32(RK3576_VICAP_PMU_HWM(RK3576_VICAP_BIU_VI_IDLE_REQ),
           pmu + RK3576_VICAP_PMU_BIU_IDLE_SFTCON0_OFF);
  up_udelay(20);

  /* 4. Wait for the domain to report powered up. */

  while (loops-- > 0)
    {
      if ((getreg32(pmu + RK3576_VICAP_PMU_PWR_GATE_STS_OFF) &
           RK3576_VICAP_PD_VI_DWN_STAT) == 0)
        {
          return OK;
        }

      up_udelay(RK3576_VICAP_PMU_POLL_STEP_US);
    }

  _err("ERROR: VICAP PD_VI never reported powered up\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_vicap_release_resets
 ****************************************************************************/

static void rk3576_vicap_release_resets(uint8_t input)
{
  uintptr_t cru = RK3576_CRU_ADDR;
  uint32_t vicap_bits = (1u << RK3576_VICAP_RST_ARESETN_BIT) |
                        (1u << RK3576_VICAP_RST_HRESETN_BIT) |
                        (1u << RK3576_VICAP_RST_DRESETN_BIT);
  uint32_t input_bits = (1u << RK3576_VICAP_RST_CIFIN_BIT) |
                        (1u << (RK3576_VICAP_RST_I0CLK_BIT + input));

  /* Both registers are "when high, reset", so the hiword mask selects the
   * bits and the low word carries 0 to release them.
   */

  putreg32(vicap_bits << 16,
           cru + RK3576_CRU_SOFTRST_CON(RK3576_VICAP_RST_CON));
  putreg32(input_bits << 16,
           cru + RK3576_CRU_SOFTRST_CON(RK3576_VICAP_INPUT_RST_CON));
  up_udelay(20);
}

/****************************************************************************
 * Name: rk3576_vicap_mmu_configure
 *
 * Description:
 *   Leave the MMU in its reset state, which is "paging disabled" and
 *   therefore pass-through, and mask its interrupts.
 *
 *   This driver programs physical addresses taken from the RK3576 DMA heap
 *   (identity mapped, below 4GB, physically contiguous), so no translation
 *   is wanted.  Paging is only ever enabled by issuing MMU_ENABLE_PAGING,
 *   so not issuing it is the whole configuration.
 *
 *   The interrupts are masked rather than handled because a page fault
 *   would raise the separate VICAP MMU interrupt line, which nothing
 *   services.  The status is still polled on the error path.
 *
 ****************************************************************************/

static void rk3576_vicap_mmu_configure(uintptr_t base)
{
  rk3576_vicap_putreg(base, RK3576_VICAP_MMU_DTE_ADDR, 0);
  rk3576_vicap_putreg(base, RK3576_VICAP_MMU_INT_CLEAR,
                      RK3576_VICAP_MMU_INT_ALL);
  rk3576_vicap_putreg(base, RK3576_VICAP_MMU_INT_MASK,
                      RK3576_VICAP_MMU_INT_ALL);
}

/****************************************************************************
 * Name: rk3576_vicap_enable_clocks
 ****************************************************************************/

static int rk3576_vicap_enable_clocks(FAR struct rk3576_vicap_s *priv,
                                      uint8_t input)
{
  char iclk_name[24];
  int ret;

  priv->aclk = clk_get("aclk_vicap");
  priv->hclk = clk_get("hclk_vicap");
  priv->dclk = clk_get("dclk_vicap");

  if (priv->aclk == NULL || priv->hclk == NULL || priv->dclk == NULL)
    {
      _err("ERROR: VICAP failed to look up its core clocks\n");
      return -ENODEV;
    }

  snprintf(iclk_name, sizeof(iclk_name), "clk_vicap_i%uclk",
           (unsigned int)input);

  priv->iclk = clk_get(iclk_name);
  if (priv->iclk == NULL)
    {
      _err("ERROR: VICAP failed to look up %s\n", iclk_name);
      return -ENODEV;
    }

  /* The MIPI input clock comes straight from the CSI HOST's hard-wired
   * clkdata output and has no CRU register of its own, so there is no rate
   * to set on it -- only the gate.
   */

  ret = clk_set_rate(priv->dclk, RK3576_VICAP_DCLK_HZ);
  if (ret < 0)
    {
      _err("ERROR: VICAP failed to set dclk_vicap: %d\n", ret);
      return ret;
    }

  ret = clk_enable(priv->dclk);
  if (ret < 0)
    {
      _err("ERROR: VICAP failed to enable dclk_vicap: %d\n", ret);
      return ret;
    }

  ret = clk_enable(priv->aclk);
  if (ret < 0)
    {
      _err("ERROR: VICAP failed to enable aclk_vicap: %d\n", ret);
      clk_disable(priv->dclk);
      return ret;
    }

  ret = clk_enable(priv->hclk);
  if (ret < 0)
    {
      _err("ERROR: VICAP failed to enable hclk_vicap: %d\n", ret);
      clk_disable(priv->aclk);
      clk_disable(priv->dclk);
      return ret;
    }

  ret = clk_enable(priv->iclk);
  if (ret < 0)
    {
      _err("ERROR: VICAP failed to enable %s: %d\n", iclk_name, ret);
      clk_disable(priv->hclk);
      clk_disable(priv->aclk);
      clk_disable(priv->dclk);
      return ret;
    }

  _info("VICAP: dclk_vicap=%" PRIu32 " aclk_vicap=%" PRIu32
        " hclk_vicap=%" PRIu32 "\n",
        (uint32_t)clk_get_rate(priv->dclk), (uint32_t)clk_get_rate(priv->aclk),
        (uint32_t)clk_get_rate(priv->hclk));

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_input_disable
 *
 * Description:
 *   Turn the capture path off in the order that cannot lose data: stop the
 *   DMA (which is downstream), then the interface capture enable, then the
 *   per-ID capture.
 *
 ****************************************************************************/

static void rk3576_vicap_input_disable(FAR struct rk3576_vicap_s *priv)
{
  uintptr_t base = priv->base;
  uint8_t input = priv->cfg.input;
  uint8_t id = priv->cfg.id;

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_CTRL(input), 0);

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_ID0_CTRL0(input) + id * 8u, 0);

  /* Clear any status the stop itself may have latched. */

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_INTSTAT(input), 0xffffffffu);
}

/****************************************************************************
 * Name: rk3576_vicap_px
 *
 * Description:
 *   Fetch one RAW sample as an 8-bit value, clamping the coordinates.  The
 *   raw data is stored uncompacted with the sample high-aligned, so a plain
 *   16-bit load shifted right by 8 gives the top 8 bits of the sample --
 *   which is what the demosaicer works in.
 *
 ****************************************************************************/

static inline uint32_t rk3576_vicap_px(FAR const uint16_t *raw, int stride_px,
                                       int w, int h, int x, int y)
{
  if (x < 0)
    {
      x = 0;
    }
  else if (x >= w)
    {
      x = w - 1;
    }

  if (y < 0)
    {
      y = 0;
    }
  else if (y >= h)
    {
      y = h - 1;
    }

  return (uint32_t)(raw[(size_t)y * (size_t)stride_px + (size_t)x] >> 8);
}

static inline uint8_t rk3576_vicap_clamp(int v)
{
  if (v < 0)
    {
      return 0;
    }

  if (v > 255)
    {
      return 255;
    }

  return (uint8_t)v;
}

/****************************************************************************
 * Name: rk3576_vicap_debayer_scalar
 *
 * Description:
 *   Bilinear demosaic of one RAW10 frame into an NV12 frame.
 *
 *   For every site the missing colours are interpolated from the nearest
 *   neighbours that do carry them: green from the four orthogonal pixels,
 *   the other chroma from the four diagonal ones, and at a green site each
 *   of the remaining two colours from its own pair of neighbours.  This is
 *   the cheapest demosaic that does not produce obvious colour artefacts on
 *   flat areas; it is deliberately not an edge-adaptive algorithm, because
 *   the goal here is a correct pipeline, not final image quality.
 *
 *   Chroma is averaged over each 2x2 block into the NV12 half-resolution
 *   plane as the luma is written, so the frame is produced in a single pass
 *   with no scratch buffer.
 *
 ****************************************************************************/

static void rk3576_vicap_debayer_scalar(FAR struct rk3576_vicap_s *priv,
                                        FAR const uint16_t *raw,
                                        FAR uint8_t *dst)
{
  FAR uint8_t *yplane = dst;
  FAR uint8_t *uvplane = dst + (size_t)priv->cfg.width * priv->cfg.height;
  FAR const uint8_t *pat = g_vicap_bayer[priv->cfg.bayer & 3u];
  uint32_t wbsnap[3];
  FAR const uint32_t *wb = wbsnap;
  int w = priv->cfg.width;
  int h = priv->cfg.height;
  int stride_px = (int)(priv->stride / 2u);
  uint64_t accr = 0;
  uint64_t accg = 0;
  uint64_t accb = 0;
  uint64_t accn = 0;
  int x;
  int y;

  /* The gains are taken once, before the frame is walked, rather than read
   * per pixel.
   *
   * The application can change them at any time through the cam3a
   * interface, and a pass that read them as it went would then apply two
   * different sets to one frame -- in the middle of a row, since the change
   * lands wherever the work queue happens to be.  One frame with the old
   * gains whole is a colour that is a frame late; one frame with a mixture
   * is a seam across the picture.
   *
   * The vector path already worked this way, because it has to load the
   * gains into vectors before the loop in any case.
   */

  wbsnap[0] = priv->wb[0];
  wbsnap[1] = priv->wb[1];
  wbsnap[2] = priv->wb[2];

  for (y = 0; y < h; y += 2)
    {
      for (x = 0; x < w; x += 2)
        {
          int sumr = 0;
          int sumg = 0;
          int sumb = 0;
          int wbok = 1;
          int dy;
          int dx;

          /* Whether this 2x2 block may steer the white balance.
           *
           * A saturated sample carries no colour: past the top of its range
           * the sensor reports the same code for any amount of light, so the
           * ratios between channels there describe the saturation and not
           * the scene.  Letting those samples into the measurement drags it
           * towards whatever the brightest part of the picture happens to
           * be, and in a frame with a large saturated area -- a bright
           * ceiling, a window -- that is most of the measurement, which then
           * corrects the rest of the picture to suit it.
           *
           * One saturated sample disqualifies the whole block, because the
           * demosaicer rebuilds each site from its neighbours: a saturated
           * neighbour corrupts a site as surely as a saturated site does.
           */

          for (dy = 0; dy < 2 && wbok != 0; dy++)
            {
              for (dx = 0; dx < 2; dx++)
                {
                  if (rk3576_vicap_px(raw, stride_px, w, h, x + dx, y + dy) >=
                      RK3576_VICAP_WB_SAT_OWN)
                    {
                      wbok = 0;
                      break;
                    }
                }
            }

          for (dy = 0; dy < 2; dy++)
            {
              for (dx = 0; dx < 2; dx++)
                {
                  int px = x + dx;
                  int py = y + dy;
                  int c = pat[(py & 1) * 2 + (px & 1)];
                  uint32_t own = rk3576_vicap_px(raw, stride_px, w, h, px, py);
                  uint32_t r;
                  uint32_t g;
                  uint32_t b;

                  if (c == 1)
                    {
                      /* Green site: the two neighbours that share the row
                       * carry one of the remaining colours and the two in
                       * the column carry the other.
                       */

                      int hcol = pat[(py & 1) * 2 + ((px & 1) ^ 1)];
                      uint32_t horiz =
                          (rk3576_vicap_px(raw, stride_px, w, h, px - 1, py) +
                           rk3576_vicap_px(raw, stride_px, w, h, px + 1, py)) /
                          2;
                      uint32_t vert =
                          (rk3576_vicap_px(raw, stride_px, w, h, px, py - 1) +
                           rk3576_vicap_px(raw, stride_px, w, h, px, py + 1)) /
                          2;

                      g = own;

                      if (hcol == 0)
                        {
                          r = horiz;
                          b = vert;
                        }
                      else
                        {
                          b = horiz;
                          r = vert;
                        }
                    }
                  else
                    {
                      uint32_t orth =
                          (rk3576_vicap_px(raw, stride_px, w, h, px - 1, py) +
                           rk3576_vicap_px(raw, stride_px, w, h, px + 1, py) +
                           rk3576_vicap_px(raw, stride_px, w, h, px, py - 1) +
                           rk3576_vicap_px(raw, stride_px, w, h, px, py + 1)) /
                          4;
                      uint32_t diag = (rk3576_vicap_px(raw, stride_px, w, h,
                                                       px - 1, py - 1) +
                                       rk3576_vicap_px(raw, stride_px, w, h,
                                                       px + 1, py - 1) +
                                       rk3576_vicap_px(raw, stride_px, w, h,
                                                       px - 1, py + 1) +
                                       rk3576_vicap_px(raw, stride_px, w, h,
                                                       px + 1, py + 1)) /
                                      4;

                      g = orth;

                      if (c == 0)
                        {
                          r = own;
                          b = diag;
                        }
                      else
                        {
                          b = own;
                          r = diag;
                        }
                    }

                  /* The channel sums that steer the white balance are
                   * taken *before* the gains are applied.
                   *
                   * Measuring after would make the loop read its own
                   * output: a gain that is already too high raises that
                   * channel's average, which lowers the correction it
                   * asks for, and the loop settles on the square root of
                   * the gain it is supposed to find.  The gain is
                   * recomputed from each frame on the assumption that the
                   * frame is ungained, so that is what has to be
                   * measured.
                   */

                  if (wbok != 0)
                    {
                      accr += r;
                      accg += g;
                      accb += b;
                      accn++;
                    }

                  /* White balance, before anything is derived from the
                   * colour: the gains correct the sensor's channel
                   * imbalance, so the luma and chroma below are computed
                   * from channels that are on a common scale.
                   */

                  r = (r * wb[0]) >> 8;
                  g = (g * wb[1]) >> 8;
                  b = (b * wb[2]) >> 8;

                  /* Hold the channels to the range before deriving anything
                   * from them.
                   *
                   * The luma is clamped in any case, since it is what can be
                   * displayed, but clamping only there leaves the chroma
                   * computed from values that no longer exist: a channel the
                   * gain pushed past the top of its range keeps its full
                   * weight in the colour difference, so the hue runs off to
                   * something the luma cannot support.  Clamping the channels
                   * first keeps the two telling the same story -- a
                   * saturated region then reads as white at full brightness
                   * instead of as a colour.
                   */

                  r = rk3576_vicap_clamp((int)r);
                  g = rk3576_vicap_clamp((int)g);
                  b = rk3576_vicap_clamp((int)b);

                  yplane[(size_t)py * w + px] = rk3576_vicap_clamp(
                      ((66 * (int)r + 129 * (int)g + 25 * (int)b + 128) >> 8) +
                      16);

                  sumr += (int)r;
                  sumg += (int)g;
                  sumb += (int)b;
                }
            }

          /* Chroma of the 2x2 block, from the average colour. */

          {
            int r = sumr / 4;
            int g = sumg / 4;
            int b = sumb / 4;
            size_t uvoff =
                ((size_t)(y / 2) * (size_t)(w / 2) + (size_t)(x / 2)) * 2u;

            uvplane[uvoff + 0] = rk3576_vicap_clamp(
                ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uvplane[uvoff + 1] = rk3576_vicap_clamp(
                ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
          }
        }
    }

  rk3576_vicap_wb_update(priv, accr, accg, accb, accn);
}

/****************************************************************************
 * Name: rk3576_vicap_wb_update
 *
 * Description:
 *   Steer the gains towards a frame whose three channels average alike.
 *
 * The common value the channels are steered to is the one that
 * reproduces the frame's original luma, not simply any equal value.
 * Requiring only that the three be equal would also change the
 * brightness of the picture, and correcting a colour cast should not
 * re-expose the frame -- the level already belongs to the exposure, and
 * raising it would push the brighter parts of the scene into clipping.
 * The weights below are the same ones the luma computation uses, so the
 * level being preserved is the one the picture actually has.
 *
 * The averages are over the samples that were counted, not over the
 * frame: the saturated ones were left out above, and dividing by the
 * whole frame would treat them as if they had contributed zero, which
 * would scale every gain down and darken the picture.
 *
 * A fractional step rather than a full one is what stops the loop from
 * oscillating between frames and from swinging on a scene that genuinely
 * is not neutral.
 *
 * No counted samples, or a luma that rounds to zero, means the frame
 * carried no usable signal; either way there is nothing to measure and
 * the gains are best left as they are.
 *
 * Both demosaicers hand their measurement here rather than each carrying
 * their own copy: they differ in how the accumulators are built, not in
 * what is done with them.
 */

static void rk3576_vicap_wb_update(FAR struct rk3576_vicap_s *priv,
                                   uint64_t accr, uint64_t accg, uint64_t accb,
                                   uint64_t accn)
{
  uint32_t wbnew[3];
  irqstate_t flags;
  bool usable;

  /* Publish the measurement first, and whether or not the driver is the one
   * acting on it: an application's loop needs it precisely when the driver's
   * is not running.
   *
   * Under irqlock, because this runs on the work queue while the cam3a
   * interface reads the same fields from task context.  A caller that mixed
   * one frame's sums with the next frame's count would compute a channel
   * mean belonging to neither, and stat_seq moving inside the same critical
   * section is what lets a caller tell that it has a whole frame's worth.
   */

  flags = spin_lock_irqsave(&priv->irqlock);

  priv->stat_sum[0] = (uint32_t)accr;
  priv->stat_sum[1] = (uint32_t)accg;
  priv->stat_sum[2] = (uint32_t)accb;
  priv->stat_count = (uint32_t)accn;
  priv->stat_w = priv->cfg.width;
  priv->stat_h = priv->cfg.height;
  priv->stat_seq++;

  wbnew[0] = priv->wb[0];
  wbnew[1] = priv->wb[1];
  wbnew[2] = priv->wb[2];

  spin_unlock_irqrestore(&priv->irqlock, flags);

  /* A frame with no counted samples carries no colour to measure: either
   * every block held a saturated sample, or the frame was black.  Either way
   * there is nothing to steer by, so the gains are left where they are.
   */

  usable = accn > 0u && accr > 0u && accg > 0u && accb > 0u;

  if (!priv->awb_active || !usable)
    {
      return;
    }

  {
    uint64_t acc[3];
    uint64_t luma;
    uint32_t level;
    unsigned int i;

    acc[0] = accr;
    acc[1] = accg;
    acc[2] = accb;

    luma = 66u * accr + 129u * accg + 25u * accb;
    level = (uint32_t)(luma / (220u * accn));

    if (level == 0u)
      {
        return;
      }

    for (i = 0; i < 3u; i++)
      {
        uint64_t want = (uint64_t)RK3576_VICAP_WB_ONE * level * accn / acc[i];
        int32_t current = (int32_t)wbnew[i];
        int32_t target;
        int32_t diff;
        int32_t step;

        /* The bound has to be applied while the value is still wide
         * enough to hold it.  A channel that is nearly black asks
         * for a very large gain -- larger than 32 bits -- and
         * narrowing it first would wrap it to something small that
         * then passes the bound.
         */

        if (want > (uint64_t)RK3576_VICAP_WB_MAX)
          {
            want = RK3576_VICAP_WB_MAX;
          }

        target = (int32_t)want;

        if (target < (int32_t)RK3576_VICAP_WB_MIN)
          {
            target = (int32_t)RK3576_VICAP_WB_MIN;
          }

        /* The step is a fraction of the distance, so the gains ease
         * towards the measurement instead of jumping straight to it.
         *
         * The arithmetic has to be signed: once the gains have
         * corrected the cast the measurements come back towards
         * unity, which is a target *below* the current gain.  Taking
         * that difference in unsigned arithmetic would wrap to a
         * huge positive number and drive the gain up instead of
         * down.
         */

        diff = target - current;

        if (diff >= 0)
          {
            step = diff >> RK3576_VICAP_WB_STEP_SHIFT;
          }
        else
          {
            step = -((-diff) >> RK3576_VICAP_WB_STEP_SHIFT);
          }

        wbnew[i] = (uint32_t)(current + step);
      }
  }

  /* Store, but only if the driver still owns the white balance.
   *
   * The application can take it over at any moment, and a gain it has just
   * set is newer than this measurement: writing ours over it would undo a
   * correction the caller has already made, one frame after it made it.
   * The window is read-then-write around the loop above, which is the only
   * place these gains are not read and written together.
   */

  flags = spin_lock_irqsave(&priv->irqlock);

  if (priv->awb_active)
    {
      priv->wb[0] = wbnew[0];
      priv->wb[1] = wbnew[1];
      priv->wb[2] = wbnew[2];
    }

  spin_unlock_irqrestore(&priv->irqlock, flags);
}

#if defined(__ARM_NEON__) || defined(__ARM_NEON)

/****************************************************************************
 * Name: rk3576_vicap_nbr
 *
 * Description:
 *   Load the eight samples of one row that begin at column `x`, together
 *   with the same eight shifted one column either way.
 *
 *   The shifted copies are the neighbours the demosaicer reads.  At the two
 *   ends of a row they would step outside it, so there the sample at the
 *   edge stands in for the one past it -- the same substitution the scalar
 *   version makes by clamping the coordinate.  Doing it here keeps every
 *   load inside the row, which is what leaves the body of the frame free of
 *   both the test and the clamp.
 *
 ****************************************************************************/

static inline void rk3576_vicap_nbr(FAR const uint16_t *row, int x, int w,
                                    FAR uint16x8_t *lm1, FAR uint16x8_t *l0,
                                    FAR uint16x8_t *lp1)
{
  uint16x8_t v = vld1q_u16(row + x);

  *l0 = v;

  if (x > 0)
    {
      *lm1 = vld1q_u16(row + x - 1);
    }
  else
    {
      *lm1 = vsetq_lane_u16(vgetq_lane_u16(v, 0), vextq_u16(v, v, 7), 0);
    }

  if (x + 8 < w)
    {
      *lp1 = vld1q_u16(row + x + 1);
    }
  else
    {
      *lp1 = vsetq_lane_u16(vgetq_lane_u16(v, 7), vextq_u16(v, v, 1), 7);
    }
}

/****************************************************************************
 * Name: rk3576_vicap_mask
 *
 * Description:
 *   Build the lane mask that selects the sites of one colour in one of the
 *   two row parities.
 *
 *   Site colours repeat every two lanes, so this is the pair a row alternates
 *   between, repeated four times.  It is built through a temporary array and
 *   loaded once per stream rather than per group: a mask is the same for
 *   every group in the frame.
 *
 ****************************************************************************/

static inline uint16x8_t rk3576_vicap_mask(FAR const uint8_t *pat, int rp,
                                           int want)
{
  uint16_t t[8];
  int j;

  for (j = 0; j < 8; j++)
    {
      t[j] = (uint16_t)(pat[rp * 2 + (j & 1)] == want ? 0xffffu : 0u);
    }

  return vld1q_u16(t);
}

/****************************************************************************
 * Name: rk3576_vicap_hred
 *
 * Description:
 *   Whether the neighbour in the same row as a green site is red rather than
 *   blue, which is what settles whether that site takes red from the
 *   horizontal pair or the vertical one.
 *
 *   A row has green sites in one column parity only, and their neighbours in
 *   the other, so this is one fact about the row rather than one per site.
 *   Either parity can be the green one depending on the order, so both are
 *   checked; a row with no green in it at all cannot occur in a Bayer
 *   pattern.
 *
 ****************************************************************************/

static inline int rk3576_vicap_hred(FAR const uint8_t *pat, int rp)
{
  int even = pat[rp * 2 + 0];
  int odd = pat[rp * 2 + 1];

  if (even == 1)
    {
      return (odd == 0) ? 1 : 0;
    }

  if (odd == 1)
    {
      return (even == 0) ? 1 : 0;
    }

  return 0;
}

/****************************************************************************
 * Name: rk3576_vicap_pick
 *
 * Description:
 *   Choose the three channel values of eight consecutive samples of one row
 *   out of the four interpolations every site has available.
 *
 *   Which interpolation each colour comes from depends only on the parity
 *   of the row and column, so it is expressed with masks rather than
 *   branches.  Out of the four: at a red site the site itself is red, green
 *   is the average of the orthogonal neighbours and blue the average of the
 *   diagonal ones; at a blue site red and blue swap; and at a green site the
 *   site itself is green while the other two come from the horizontal and
 *   vertical pairs.
 *
 *   Which of those two pairs a green site takes red from is the caller's to
 *   settle, not this function's.  It follows from the colour of the
 *   neighbour in the same row, and since every green site in a row shares a
 *   column parity, that neighbour is the same colour at all of them -- one
 *   fact about the row rather than a per-lane one.  Handing the pairs over
 *   already in the order red wants them therefore costs nothing and saves a
 *   mask and two selects on every row.
 *
 ****************************************************************************/

static inline void rk3576_vicap_pick(uint16x8_t mred, uint16x8_t mblue,
                                     uint16x8_t mgreen, uint16x8_t own,
                                     uint16x8_t hr, uint16x8_t br,
                                     uint16x8_t orth, uint16x8_t dia,
                                     FAR uint16x8_t *r, FAR uint16x8_t *g,
                                     FAR uint16x8_t *b)
{
  /* At a red or a blue site the two pairs are overwritten below, so what
   * they hold there does not matter.
   */

  *r = vbslq_u16(mblue, dia, hr);
  *r = vbslq_u16(mred, own, *r);

  *g = vbslq_u16(mgreen, own, orth);

  *b = vbslq_u16(mred, dia, br);
  *b = vbslq_u16(mblue, own, *b);
}

/****************************************************************************
 * Name: rk3576_vicap_gain
 *
 * Description:
 *   Apply one of the white balance gains to eight channel values, dropping
 *   whatever the gain pushes past full scale.
 *
 *   The gains are eight-bit fixed point with one as 256, so the scalar form
 *   is (v * gain) >> 8.  The doubling multiply high is (2 * a * b) >> 16
 *   without rounding, so scaling one side by 32 and the other by 4 lands on
 *   exactly that value: a sample is at most 255 and a gain at most 1024, so
 *   neither side overflows and the product cannot reach the point where the
 *   doubling saturates.  The saturating variant is used because it is the
 *   one that does not round.
 *
 ****************************************************************************/

static inline uint16x8_t rk3576_vicap_gain(uint16x8_t c, int16x8_t gain)
{
  return vminq_u16(vreinterpretq_u16_s16(vqdmulhq_s16(
                       vshlq_n_s16(vreinterpretq_s16_u16(c), 5), gain)),
                   vdupq_n_u16(255));
}

/****************************************************************************
 * Name: rk3576_vicap_debayer_neon
 *
 * Description:
 *   The same bilinear demosaic, white balance and NV12 conversion as
 *   rk3576_vicap_debayer_scalar, eight pixels and two rows at a time.
 *
 *   The scalar version spends most of its time not on arithmetic but on
 *   getting hold of the neighbours: the five to eight samples a site needs
 *   are each fetched through a helper that clamps both coordinates and
 *   multiplies out the address, so the same sample is found, clamped and
 *   converted again and again for every site that uses it.  Here the
 *   neighbours arrive as three shifted loads per row, the clamping is
 *   confined to the two ends of the row, and the eight-bit samples stay in
 *   sixteen-bit lanes where a halving add cannot overflow -- so a pair of
 *   neighbours costs one shift rather than one shift per sample.
 *
 *   Two rows are done together because that is the unit the chroma is
 *   averaged over, and because the row below a pair is the row above the
 *   next one: the four rows a pair needs are the whole working set.
 *
 ****************************************************************************/

static void rk3576_vicap_debayer_neon(FAR struct rk3576_vicap_s *priv,
                                      FAR const uint16_t *raw,
                                      FAR uint8_t *dst)
{
  FAR uint8_t *yplane = dst;
  FAR uint8_t *uvplane = dst + (size_t)priv->cfg.width * priv->cfg.height;
  FAR const uint8_t *pat = g_vicap_bayer[priv->cfg.bayer & 3u];
  const int w = priv->cfg.width;
  const int h = priv->cfg.height;
  const int stride_px = (int)(priv->stride / 2u);

  /* What each lane of a group takes, as all-ones or all-zero masks.
   *
   * A site's colour depends only on the parity of its row and column, so
   * within a group of eight the masks repeat every two lanes and the two
   * rows of a pair take one set each.  They are therefore the same for
   * every group in the frame, and are built once.  Three per row are needed:
   * the site is red, the site is blue, the site is green.  The fourth thing
   * the pattern decides -- which pair a green site takes red from -- is the
   * same at every green site in a row, and is carried as a flag rather than
   * as a mask.
   */

  uint16x8_t mredu;
  uint16x8_t mblueu;
  uint16x8_t mgreenu;
  uint16x8_t mredl;
  uint16x8_t mbluel;
  uint16x8_t mgreenl;
  int hredu;
  int hredl;
  int16x8_t gain[3];
  uint32x4_t vacc[3];
  uint32x4_t vcnt;
  uint64_t accr = 0;
  uint64_t accg = 0;
  uint64_t accb = 0;
  uint64_t accn = 0;
  int i;
  int y;

  mredu = rk3576_vicap_mask(pat, 0, 0);
  mblueu = rk3576_vicap_mask(pat, 0, 2);
  mgreenu = rk3576_vicap_mask(pat, 0, 1);
  mredl = rk3576_vicap_mask(pat, 1, 0);
  mbluel = rk3576_vicap_mask(pat, 1, 2);
  mgreenl = rk3576_vicap_mask(pat, 1, 1);
  hredu = rk3576_vicap_hred(pat, 0);
  hredl = rk3576_vicap_hred(pat, 1);

  for (i = 0; i < 3; i++)
    {
      gain[i] = vdupq_n_s16((int16_t)(priv->wb[i] << 2));
      vacc[i] = vdupq_n_u32(0);
    }

  vcnt = vdupq_n_u32(0);

  for (y = 0; y < h; y += 2)
    {
      /* The rows above and below the pair are what the interpolation
       * reaches into; at the top and bottom of the frame the edge row
       * stands in for the one past it.
       */

      FAR const uint16_t *ra = raw + (size_t)(y > 0 ? y - 1 : 0) * stride_px;
      FAR const uint16_t *rb = raw + (size_t)y * stride_px;
      FAR const uint16_t *rc = raw + (size_t)(y + 1) * stride_px;
      FAR const uint16_t *rd =
          raw + (size_t)(y + 2 < h ? y + 2 : h - 1) * stride_px;
      FAR uint8_t *o0 = yplane + (size_t)y * w;
      FAR uint8_t *o1 = o0 + w;
      FAR uint8_t *uv = uvplane + (size_t)(y / 2) * w;
      int x;

      for (x = 0; x < w; x += 8)
        {
          uint16x8_t p0m, p0, p0p;
          uint16x8_t p1m, p1, p1p;
          uint16x8_t p2m, p2, p2p;
          uint16x8_t p3m, p3, p3p;
          uint16x8_t q0, q1, q2, q3;
          uint16x8_t s0, s1;
          uint16x8_t ownu, ownl, hu, vu, ou, du, hl, vl, ol, dl;
          uint16x8_t ru, gu, bu, rl, gl, bl;
          uint16x8_t mask;
          uint16x8_t t;
          uint16x8_t yv;

          rk3576_vicap_nbr(ra, x, w, &p0m, &p0, &p0p);
          rk3576_vicap_nbr(rb, x, w, &p1m, &p1, &p1p);
          rk3576_vicap_nbr(rc, x, w, &p2m, &p2, &p2p);
          rk3576_vicap_nbr(rd, x, w, &p3m, &p3, &p3p);

          /* Only the top eight bits of each sample are ever wanted, so
           * they are brought down before anything is added.
           *
           * Adding the sixteen-bit words first and shifting afterwards
           * does not work, and not for the obvious reason.  Two
           * full-scale samples will not fit in sixteen bits, but a halving
           * add sidesteps that by computing (a + b) / 2 -- and that is
           * exactly where it goes wrong.  Halving a raw word lifts the bit
           * that is about to be discarded into the part being kept, so a
           * pair whose sum lands just below a multiple of 256 comes out one
           * too high once the remaining seven places are dropped.  The
           * scalar reads each sample as eight bits and only then adds, and
           * the two agree only if this does the same.
           */

          p0m = vshrq_n_u16(p0m, 8);
          p0 = vshrq_n_u16(p0, 8);
          p0p = vshrq_n_u16(p0p, 8);
          p1m = vshrq_n_u16(p1m, 8);
          p1 = vshrq_n_u16(p1, 8);
          p1p = vshrq_n_u16(p1p, 8);
          p2m = vshrq_n_u16(p2m, 8);
          p2 = vshrq_n_u16(p2, 8);
          p2p = vshrq_n_u16(p2p, 8);
          p3m = vshrq_n_u16(p3m, 8);
          p3 = vshrq_n_u16(p3, 8);
          p3p = vshrq_n_u16(p3p, 8);

          q0 = vaddq_u16(p0m, p0p);
          q1 = vaddq_u16(p1m, p1p);
          q2 = vaddq_u16(p2m, p2p);
          q3 = vaddq_u16(p3m, p3p);
          s0 = vaddq_u16(p0, p2);
          s1 = vaddq_u16(p1, p3);

          ownu = p1;
          ownl = p2;

          hu = vshrq_n_u16(q1, 1);                /* horizontal pair */
          vu = vshrq_n_u16(s0, 1);                /* vertical pair   */
          ou = vshrq_n_u16(vaddq_u16(q1, s0), 2); /* orthogonal four */
          du = vshrq_n_u16(vaddq_u16(q0, q2), 2); /* diagonal four   */

          hl = vshrq_n_u16(q2, 1);
          vl = vshrq_n_u16(s1, 1);
          ol = vshrq_n_u16(vaddq_u16(q2, s1), 2);
          dl = vshrq_n_u16(vaddq_u16(q1, q3), 2);

          rk3576_vicap_pick(mredu, mblueu, mgreenu, ownu, hredu ? hu : vu,
                            hredu ? vu : hu, ou, du, &ru, &gu, &bu);
          rk3576_vicap_pick(mredl, mbluel, mgreenl, ownl, hredl ? hl : vl,
                            hredl ? vl : hl, ol, dl, &rl, &gl, &bl);

          /* Which 2x2 blocks may steer the gains.  A block is kept out of
           * the measurement if any of its four samples is at the top of
           * the range, so the decision is taken on the largest of them.
           * The four decisions come out in the low half; they are counted
           * there, before being spread over the eight lanes so that each
           * sample of the block carries its block's verdict.
           */

          mask = vcltq_u16(vmaxq_u16(vpmaxq_u16(p1, p1), vpmaxq_u16(p2, p2)),
                           vdupq_n_u16(RK3576_VICAP_WB_SAT_OWN));
          vcnt = vaddw_u16(vcnt, vshr_n_u16(vget_low_u16(mask), 15));
          mask = vzipq_u16(mask, mask).val[0];

          /* The channel sums that steer the white balance, taken before
           * the gains are applied: measuring after would make the loop
           * read its own output.
           */

          t = vandq_u16(ru, mask);
          vacc[0] = vaddw_u16(vacc[0], vget_low_u16(t));
          vacc[0] = vaddw_u16(vacc[0], vget_high_u16(t));
          t = vandq_u16(gu, mask);
          vacc[1] = vaddw_u16(vacc[1], vget_low_u16(t));
          vacc[1] = vaddw_u16(vacc[1], vget_high_u16(t));
          t = vandq_u16(bu, mask);
          vacc[2] = vaddw_u16(vacc[2], vget_low_u16(t));
          vacc[2] = vaddw_u16(vacc[2], vget_high_u16(t));

          t = vandq_u16(rl, mask);
          vacc[0] = vaddw_u16(vacc[0], vget_low_u16(t));
          vacc[0] = vaddw_u16(vacc[0], vget_high_u16(t));
          t = vandq_u16(gl, mask);
          vacc[1] = vaddw_u16(vacc[1], vget_low_u16(t));
          vacc[1] = vaddw_u16(vacc[1], vget_high_u16(t));
          t = vandq_u16(bl, mask);
          vacc[2] = vaddw_u16(vacc[2], vget_low_u16(t));
          vacc[2] = vaddw_u16(vacc[2], vget_high_u16(t));

          ru = rk3576_vicap_gain(ru, gain[0]);
          gu = rk3576_vicap_gain(gu, gain[1]);
          bu = rk3576_vicap_gain(bu, gain[2]);
          rl = rk3576_vicap_gain(rl, gain[0]);
          gl = rk3576_vicap_gain(gl, gain[1]);
          bl = rk3576_vicap_gain(bl, gain[2]);

          /* Rec.601 luma in studio range.  Holding the channels to their
           * range first is also what keeps the sum below the top of the
           * expression's range, so this needs no clamp of its own.
           */

          yv = vdupq_n_u16(128);
          yv = vmlaq_n_u16(yv, ru, 66);
          yv = vmlaq_n_u16(yv, gu, 129);
          yv = vmlaq_n_u16(yv, bu, 25);
          vst1_u8(o0 + x,
                  vmovn_u16(vaddq_u16(vshrq_n_u16(yv, 8), vdupq_n_u16(16))));

          yv = vdupq_n_u16(128);
          yv = vmlaq_n_u16(yv, rl, 66);
          yv = vmlaq_n_u16(yv, gl, 129);
          yv = vmlaq_n_u16(yv, bl, 25);
          vst1_u8(o1 + x,
                  vmovn_u16(vaddq_u16(vshrq_n_u16(yv, 8), vdupq_n_u16(16))));

          /* Chroma of each 2x2 block, from the average of its four gained
           * and clamped samples.  Every intermediate here stays inside a
           * signed sixteen-bit value, and the narrowing store at the end is
           * what holds the result to 0..255 -- the same clamp the scalar
           * version applies by hand.
           */

          {
            uint16x4_t rs = vshr_n_u16(
                vadd_u16(vpadd_u16(vget_low_u16(ru), vget_high_u16(ru)),
                         vpadd_u16(vget_low_u16(rl), vget_high_u16(rl))),
                2);
            uint16x4_t gs = vshr_n_u16(
                vadd_u16(vpadd_u16(vget_low_u16(gu), vget_high_u16(gu)),
                         vpadd_u16(vget_low_u16(gl), vget_high_u16(gl))),
                2);
            uint16x4_t bs = vshr_n_u16(
                vadd_u16(vpadd_u16(vget_low_u16(bu), vget_high_u16(bu)),
                         vpadd_u16(vget_low_u16(bl), vget_high_u16(bl))),
                2);
            int16x4_t cb;
            int16x4_t cr;
            int16x4x2_t z;

            cb = vmla_n_s16(vdup_n_s16(128), vreinterpret_s16_u16(rs), -38);
            cb = vmla_n_s16(cb, vreinterpret_s16_u16(gs), -74);
            cb = vmla_n_s16(cb, vreinterpret_s16_u16(bs), 112);
            cb = vadd_s16(vshr_n_s16(cb, 8), vdup_n_s16(128));

            cr = vmla_n_s16(vdup_n_s16(128), vreinterpret_s16_u16(rs), 112);
            cr = vmla_n_s16(cr, vreinterpret_s16_u16(gs), -94);
            cr = vmla_n_s16(cr, vreinterpret_s16_u16(bs), -18);
            cr = vadd_s16(vshr_n_s16(cr, 8), vdup_n_s16(128));

            z = vzip_s16(cb, cr);
            vst1_u8(uv + x, vqmovun_s16(vcombine_s16(z.val[0], z.val[1])));
          }
        }

      /* Fold the row pair's measurement into the frame's.  Reducing a row
       * pair at a time is what keeps a lane from ever reaching the top of
       * its 32-bit accumulator, however wide the frame is.  The blocks are
       * counted rather than the samples, so four pixels are added for each
       * one that was counted.
       */

      accr += vaddlvq_u32(vacc[0]);
      accg += vaddlvq_u32(vacc[1]);
      accb += vaddlvq_u32(vacc[2]);
      accn += (uint64_t)4 * vaddlvq_u32(vcnt);

      for (i = 0; i < 3; i++)
        {
          vacc[i] = vdupq_n_u32(0);
        }

      vcnt = vdupq_n_u32(0);
    }

  rk3576_vicap_wb_update(priv, accr, accg, accb, accn);
}

#endif /* __ARM_NEON__ || __ARM_NEON */

/****************************************************************************
 * Name: rk3576_vicap_debayer
 *
 * Description:
 *   Demosaic one RAW frame into NV12, by whichever path the build and the
 *   frame geometry allow.
 *
 ****************************************************************************/

static void rk3576_vicap_debayer(FAR struct rk3576_vicap_s *priv,
                                 FAR const uint16_t *raw, FAR uint8_t *dst)
{
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
  /* The vector path works in whole 2x2 blocks and whole row pairs, so it
   * needs a width that is a multiple of eight and an even height.  Both
   * hold for the modes this driver accepts; anything else, or a build
   * without NEON, falls back to the scalar version.
   */

  if ((priv->cfg.width & 7u) == 0u && (priv->cfg.height & 1u) == 0u)
    {
      rk3576_vicap_debayer_neon(priv, raw, dst);
      return;
    }
#endif

  rk3576_vicap_debayer_scalar(priv, raw, dst);
}

/****************************************************************************
 * Name: rk3576_vicap_report_errors
 *
 * Description:
 *   Say, once, which error conditions the interface has reported.
 *
 *   The block reports the things that mean "the picture is wrong" rather than
 *   "the picture arrived": a FIFO that overflowed, bandwidth the DMA could not
 *   keep up with, and a frame whose size was not the size the capture path was
 *   told to expect.  They are the hardware's own account of a frame that is
 *not what it should be, which is the only account available for a fault whose
 *   symptom is in the pixels.
 *
 *   Saying it as it happens rather than only in the closing dump is the point.
 *   A size error is what a frame boundary that lands on the wrong line looks
 *   like from here, and the picture that follows has its row and column parity
 *   shifted: the wrong colour, with the first row belonging to the frame
 *before. That symptom invites a search through the demosaic and the sensor,
 *neither of which is where the fault is.
 *
 *   Called from the worker, not the interrupt, and only for bits that have not
 *   been reported before.
 *
 ****************************************************************************/

static void rk3576_vicap_report_errors(FAR struct rk3576_vicap_s *priv)
{
  uint32_t fresh;

  fresh = priv->errstat & ~priv->errstat_reported;

  if (fresh == 0u)
    {
      return;
    }

  priv->errstat_reported |= fresh;

  if ((fresh & RK3576_VICAP_MIPI_INT_DMA_Y_FIFO_OVF) != 0u)
    {
      _err("ERROR: VICAP luma FIFO overflowed -- the DMA is not keeping up "
           "with the interface\n");
    }

  if ((fresh & RK3576_VICAP_MIPI_INT_DMA_UV_FIFO_OVF) != 0u)
    {
      _err("ERROR: VICAP chroma FIFO overflowed -- the DMA is not keeping up "
           "with the interface\n");
    }

  if ((fresh & RK3576_VICAP_MIPI_INT_BANDWIDTH_LACK) != 0u)
    {
      _err("ERROR: VICAP reported insufficient bandwidth\n");
    }

  if ((fresh & RK3576_VICAP_MIPI_INT_CSI2RX_FIFO_OVF) != 0u)
    {
      _err("ERROR: VICAP CSI-2 receive FIFO overflowed\n");
    }

  if ((fresh & RK3576_VICAP_MIPI_INT_SIZE_ERR_ID0) != 0u)
    {
      _err("ERROR: VICAP received a frame whose size is not the %ux%u it was "
           "configured for.  This is what a frame boundary that lands on the "
           "wrong line looks like from here, and the picture that follows has "
           "its row parity shifted -- the wrong colour, with the first row "
           "belonging to the frame before\n",
           priv->cfg.width, priv->cfg.height);
    }
}

/****************************************************************************
 * Name: rk3576_vicap_worker
 *
 * Description:
 *   Off-interrupt half of the frame completion: demosaic the RAW buffer the
 *   DMA just finished and hand the finished NV12 frame to the framework.
 *
 ****************************************************************************/

static void rk3576_vicap_worker(FAR void *arg)
{
  FAR struct rk3576_vicap_s *priv = (FAR struct rk3576_vicap_s *)arg;
  struct timeval t0;
  struct timeval t1;
  struct timeval td0;
  struct timeval td1;
  FAR const uint16_t *src;
  FAR uint8_t *dst;
  size_t dstsize;
  imgdata_capture_t callback;
  FAR void *cbarg;
  uint32_t epoch;
  uint32_t frames_before;
  bool inplace;
  int idx;
  irqstate_t flags;

  /* Say what the interface has reported since the last frame, if anything.
   * Done here rather than in the interrupt so that the printing -- which goes
   * to a console that may block -- stays off the interrupt path.
   */

  rk3576_vicap_report_errors(priv);

  /* Claim this frame together with the buffer the framework has offered for
   * it.
   *
   * SET_BUF is the framework's way of saying "here is a buffer, fill it".
   * It arrives once before the stream starts and again after every frame the
   * framework accepts, so taking the buffer here both pairs the frame with
   * the buffer meant for it and throttles the driver to the rate the
   * framework can absorb.  Delivering a frame nobody asked for is not
   * harmless: the completion path dereferences the framework's "next buffer"
   * pointer without checking it, and that pointer is NULL once the queued
   * buffers have all been consumed.
   *
   * The buffer is consumed in the same critical section, so a frame arriving
   * before the framework offers a new one is dropped by the check below.
   */

  flags = spin_lock_irqsave(&priv->irqlock);
  idx = priv->raw_pending;
  priv->raw_pending = -1;
  priv->frame_scheduled = false;
  dst = priv->dst;
  dstsize = priv->dstsize;
  priv->dst = NULL;
  priv->dstsize = 0;
  epoch = priv->stream_epoch;
  spin_unlock_irqrestore(&priv->irqlock, flags);

  if (idx < 0)
    {
      return;
    }

  if (dst == NULL)
    {
      /* Nothing was offered, so there is nowhere to put this frame.  This is
       * ordinary back-pressure, not an error: the framework is simply not
       * ready for another frame yet.  Counted apart from an overrun because
       * the two mean the opposite thing -- this one says the application is
       * behind, an overrun says this driver is.
       */

      priv->dropcount++;
      priv->drop_nobuf++;
      return;
    }

  /* Read the frame where it lies, when the timing allows it.
   *
   * VICAP writes DDR directly (its MMU is in pass-through), so the CPU's
   * view of the RAW buffer has to be invalidated before reading it either
   * way.
   *
   * The ping-pong that gives this driver one frame at a time is also what
   * makes reading in place possible: with two buffers alternating, the one
   * just finished is not written again until the other frame has been
   * captured, so it is stable for exactly one frame interval.  Whether the
   * demosaic fits in that interval is a property of the mode and the frame
   * rate rather than of this code, so it is decided from what the last frame
   * actually cost, and never assumed.  Getting it wrong is not a slow frame
   * but a wrong one -- the DMA would overwrite the bottom of the buffer
   * while the top was being read -- which is why the earlier version of this
   * driver copied unconditionally.
   *
   * It is worth getting right, because the copy is not cheap here.  It costs
   * as much traffic again as the demosaic itself, the frame being read,
   * written and then read back, and this loop spends most of its time
   * waiting on memory rather than computing.
   */

  up_invalidate_dcache((uintptr_t)priv->raw[idx],
                       (uintptr_t)priv->raw[idx] + priv->rawlen);

  gettimeofday(&t0, NULL);

  /* The decision is taken once the first few frames have been measured, and
   * until then, and for a mode too slow for its frame rate afterwards, every
   * frame takes the copy.  See the note on the fields.
   */

  if (!priv->inplace_decided &&
      priv->inplace_samples >= RK3576_VICAP_INPLACE_SAMPLES &&
      priv->interval_ticks != 0u)
    {
      uint32_t average = priv->debayer_us_sum / priv->inplace_samples;

      priv->inplace_decided = true;
      priv->inplace_ok = (average * RK3576_VICAP_INPLACE_MARGIN <
                          priv->interval_ticks * USEC_PER_TICK);
    }

  inplace = priv->inplace_ok;

  frames_before = priv->isr_frames;

  if (inplace)
    {
      src = (FAR const uint16_t *)priv->raw[idx];
    }
  else
    {
      struct timeval tc0;
      struct timeval tc1;

      /* Timed because the copy is what has to outrun the DMA: it is safe
       * only while it stays a small fraction of a frame interval, and that
       * is worth being able to check rather than assume.
       */

      gettimeofday(&tc0, NULL);
      memcpy(priv->snap, priv->raw[idx], priv->rawlen);
      gettimeofday(&tc1, NULL);

      priv->copy_us += (uint32_t)((tc1.tv_sec - tc0.tv_sec) * 1000000 +
                                  (tc1.tv_usec - tc0.tv_usec));

      /* A frame boundary crossed during the copy means it may hold parts of
       * two frames.  The copy is private from here on, so this is the whole
       * of the copy's exposure and the check belongs right here.
       */

      if (priv->isr_frames != frames_before)
        {
          priv->racecount++;
        }

      src = (FAR const uint16_t *)priv->snap;
    }

  /* Timed separately from the frame as a whole, because this is the number
   * the choice above is made from and it has to be the demosaic alone --
   * counting the copy in with it would set the threshold by the thing being
   * avoided.
   */

  gettimeofday(&td0, NULL);
  rk3576_vicap_debayer(priv, src, dst);
  gettimeofday(&td1, NULL);

  priv->debayer_us = (uint32_t)((td1.tv_sec - td0.tv_sec) * 1000000 +
                                (td1.tv_usec - td0.tv_usec));
  priv->debayer_us_sum += priv->debayer_us;
  priv->inplace_samples++;

  /* Reading where the frame lies is exposed for as long as the read takes,
   * which is the whole of the demosaic, so the boundary is checked after it
   * rather than around it.  The margin above is what is meant to keep this
   * from ever firing; this is what says whether it did.
   *
   * A boundary inside the window means the DMA has begun overwriting the
   * buffer that was being read, which is the one way this path can fail.  It
   * is also the only thing that settles the question -- a prediction from
   * timings can be unlucky, an observed boundary cannot -- so it ends the
   * experiment for this stream instead of only being counted.
   */

  if (inplace)
    {
      priv->inplace_frames++;
      priv->inplace_us_sum += priv->debayer_us;

      if (priv->isr_frames != frames_before)
        {
          priv->racecount++;
          priv->inplace_ok = false;
        }
    }
  else
    {
      priv->copy_frames++;
      priv->copy_demosaic_us_sum += priv->debayer_us;
    }

  up_clean_dcache((uintptr_t)dst, (uintptr_t)dst + dstsize);

  gettimeofday(&t1, NULL);
  priv->frame_us += (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
                               (t1.tv_usec - t0.tv_usec));

  /* Look the callback up only now, after the long demosaic.
   *
   * It is re-read rather than remembered, because a stop may have happened
   * while this frame was being demosaiced: the framework stops the stream
   * when it runs out of buffers, and releases them when the device is closed.
   * A frame handed over after that would be a call into state that is gone.
   * Checking under the same lock the stop uses, and discarding the frame if
   * the stream it belonged to has ended, is what keeps that from happening.
   */

  flags = spin_lock_irqsave(&priv->irqlock);
  if (!priv->capturing || priv->callback == NULL ||
      priv->stream_epoch != epoch)
    {
      spin_unlock_irqrestore(&priv->irqlock, flags);
      priv->dropcount++;
      priv->drop_stale++;
      return;
    }

  callback = priv->callback;
  cbarg = priv->arg;
  spin_unlock_irqrestore(&priv->irqlock, flags);

  priv->framecount++;

  callback(0, (uint32_t)dstsize, &t0, cbarg);
}

/****************************************************************************
 * Name: rk3576_vicap_isr
 *
 * Description:
 *   Frame-boundary and error interrupt.  The only work done here is to note
 *   which ping-pong buffer the DMA just finished and to defer the rest.
 *
 ****************************************************************************/

static int rk3576_vicap_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3576_vicap_s *priv = &g_vicap;
  uint32_t status;
  uint8_t input = priv->cfg.input;

  status = rk3576_vicap_getreg(priv->base, RK3576_VICAP_MIPI_INTSTAT(input));

  if (status == 0)
    {
      return OK;
    }

  /* INTSTAT is write-one-to-clear, so writing the value read back drops
   * exactly the bits that were pending.
   */

  rk3576_vicap_putreg(priv->base, RK3576_VICAP_MIPI_INTSTAT(input), status);

  if ((status & (RK3576_VICAP_MIPI_INT_FRAME0_DMA_END_ID0 |
                 RK3576_VICAP_MIPI_INT_FRAME1_DMA_END_ID0)) != 0)
    {
      bool both = (status & (RK3576_VICAP_MIPI_INT_FRAME0_DMA_END_ID0 |
                             RK3576_VICAP_MIPI_INT_FRAME1_DMA_END_ID0)) ==
                  (RK3576_VICAP_MIPI_INT_FRAME0_DMA_END_ID0 |
                   RK3576_VICAP_MIPI_INT_FRAME1_DMA_END_ID0);
      int idx;

      if (both)
        {
          /* Two boundaries in one interrupt: this handler did not run between
           * them, so a frame was missed.  The bits say which buffers finished
           * but not in what order, and the pair alternates -- so the newer is
           * the one the last DMA end was not.
           *
           * Counted as an overrun rather than ignored: a frame nobody
           * demosaiced is exactly what the figure is for, and a stream that
           * reports none while producing torn pictures would be a report that
           * argues against its own evidence.
           */

          priv->dropcount++;
          priv->drop_overrun++;
          idx = priv->dma_idx >= 0 ? !priv->dma_idx : 0;
        }
      else
        {
          idx =
              (status & RK3576_VICAP_MIPI_INT_FRAME0_DMA_END_ID0) != 0 ? 0 : 1;
        }

      priv->dma_idx = idx;

      /* Count the boundary where it is noticed, so the demosaic can tell
       * whether a copy straddled one.
       */

      priv->isr_frames++;

      /* The spacing between DMA ends is the interval the demosaic has to
       * fit into.  Measured here rather than taken from the requested frame
       * rate, because it is the hardware's actual cadence that decides how
       * long the finished buffer stays untouched.
       */

      {
        uint32_t now = (uint32_t)clock_systime_ticks();

        if (priv->isr_ticks != 0u && now > priv->isr_ticks)
          {
            priv->interval_ticks = now - priv->isr_ticks;
          }

        priv->isr_ticks = now;
      }

      if (priv->raw_pending >= 0)
        {
          /* The previous frame has not been demosaiced yet.  Counting it
           * makes an overrun visible instead of silent.
           */

          priv->dropcount++;
          priv->drop_overrun++;
        }

      priv->raw_pending = idx;

      if (!priv->frame_scheduled)
        {
          priv->frame_scheduled = true;

          if (work_queue(LPWORK, &priv->work, rk3576_vicap_worker, priv, 0) <
              0)
            {
              priv->frame_scheduled = false;
              priv->dropcount++;
              priv->drop_queue++;
            }
        }
    }

  if ((status & RK3576_VICAP_MIPI_INT_ERRORS) != 0)
    {
      /* Accumulate only the error bits.
       *
       * The frame start/end bits share this register and are set on every
       * healthy frame, so OR-ing the whole status in would leave the
       * diagnostic permanently non-zero and hide whether anything is
       * actually wrong.
       *
       * Only recorded here.  Saying it is the worker's job: printing from an
       * interrupt handler means writing to a console that may block, on a
       * path that has already shown it can fall behind.
       */

      priv->errstat |= (status & RK3576_VICAP_MIPI_INT_ERRORS);
    }

  /* The interface's own frame boundaries are counted rather than acted on.
   * They answer the other half of the frame question: the DMA end above
   * says a buffer filled, these say where the sensor's frame began.
   */

  if ((status & RK3576_VICAP_MIPI_INT_FRAME_START_ID0) != 0)
    {
      priv->fs_count++;
    }

  if ((status & RK3576_VICAP_MIPI_INT_FRAME_END_ID0) != 0)
    {
      priv->fe_count++;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_input_configure
 *
 * Description:
 *   Program the MIPI input's ID slot for RAW capture.
 *
 ****************************************************************************/

static void rk3576_vicap_input_configure(FAR struct rk3576_vicap_s *priv)
{
  uintptr_t base = priv->base;
  uint8_t input = priv->cfg.input;
  uint8_t id = priv->cfg.id;
  uint32_t parse_type;
  uint32_t ctrl0;
  uint32_t ctrl1;

  switch (priv->cfg.raw_bits)
    {
      case 8:
        parse_type = RK3576_VICAP_PARSE_TYPE_RAW8;
        break;

      case 12:
        parse_type = RK3576_VICAP_PARSE_TYPE_RAW12;
        break;

      case 14:
        parse_type = RK3576_VICAP_PARSE_TYPE_RAW14;
        break;

      default:
        parse_type = RK3576_VICAP_PARSE_TYPE_RAW10;
        break;
    }

  /* IDn_CTRL1 carries the VC/DT filter.  This is the block that decides
   * which packets belong to this path -- the CSI HOST hands everything over
   * unfiltered.
   */

  ctrl1 =
      ((uint32_t)(priv->cfg.vc & 0x3u) << RK3576_VICAP_ID_CTRL1_VC_SHIFT) |
      ((uint32_t)(priv->cfg.dt & 0x3fu) << RK3576_VICAP_ID_CTRL1_DT_SHIFT) |
      (RK3576_VICAP_HDR_MODE_VC << RK3576_VICAP_ID_CTRL1_HDR_MODE_SHIFT);

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_ID0_CTRL1(input) + id * 8u,
                      ctrl1);

  /* IDn_CTRL0: how to parse the payload, how to write it to DDR, and only
   * then the enables.
   */

  ctrl0 = (parse_type << RK3576_VICAP_PARSE_TYPE_SHIFT) |
          ((priv->cfg.uncompact ? RK3576_VICAP_WRDDR_TYPE_RAW_UNCOMPACT
                                : RK3576_VICAP_WRDDR_TYPE_RAW_COMPACT)
           << RK3576_VICAP_WRDDR_TYPE_SHIFT);

  if (priv->cfg.uncompact && priv->cfg.align_high)
    {
      ctrl0 |= RK3576_VICAP_ID_CTRL0_ALIGN_HIGH;
    }

  /* Crop is left enabled, as the TRM recommends, and this driver therefore
   * programs the 4-aligned start/size below.
   */

  ctrl0 |= RK3576_VICAP_ID_CTRL0_CROP_EN;

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_ID0_CTRL0(input) + id * 8u,
                      ctrl0);

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_ID0_CROP_START(input) + id * 4u,
                      0);

  rk3576_vicap_putreg(
      base, RK3576_VICAP_MIPI_ID0_SET_SIZE(input) + id * 4u,
      ((uint32_t)priv->cfg.height << RK3576_VICAP_SET_SIZE_HEIGHT_SHIFT) |
          ((uint32_t)priv->cfg.width & RK3576_VICAP_SET_SIZE_WIDTH_MASK));

  /* Ping-pong frame addresses.  Only the Y/RAW plane is used: a RAW frame
   * is a single plane, so FRAME0/1_ADDR_UV is left at zero.
   */

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_FRAME0_ADDR_Y(input),
                      (uint32_t)(uintptr_t)priv->raw[0]);
  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_FRAME1_ADDR_Y(input),
                      (uint32_t)(uintptr_t)priv->raw[1]);
  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_FRAME0_ADDR_UV(input), 0);
  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_FRAME1_ADDR_UV(input), 0);

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_VLW(input),
                      priv->stride & RK3576_VICAP_MIPI_VLW_MASK);
}

/****************************************************************************
 * Name: rk3576_vicap_init / uninit
 ****************************************************************************/

static int rk3576_vicap_init(FAR struct imgdata_s *data)
{
  FAR struct rk3576_vicap_s *priv =
      (FAR struct rk3576_vicap_s *)((uintptr_t)data -
                                    offsetof(struct rk3576_vicap_s, data));

  /* Everything expensive happens in rk3576_vicap_initialize(); the
   * framework calls this on the first open(), and it only has to make sure
   * the block is usable.
   */

  return priv->initialized ? OK : -ENODEV;
}

static int rk3576_vicap_uninit(FAR struct imgdata_s *data)
{
  /* Called when the last user closes the device, in task context.  This is
   * where the bring-up dump belongs: stop_capture() is also reached from the
   * completion path, where printing would be unsafe, so reporting the state
   * of the last capture is deliberately not done there.
   */

  UNUSED(data);
  rk3576_vicap_dump();
  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_validate_frame_setting
 ****************************************************************************/

static int rk3576_vicap_validate_frame_setting(
    FAR struct imgdata_s *data, uint8_t nr_datafmts,
    FAR imgdata_format_t *datafmts, FAR imgdata_interval_t *interval)
{
  FAR struct rk3576_vicap_s *priv =
      (FAR struct rk3576_vicap_s *)((uintptr_t)data -
                                    offsetof(struct rk3576_vicap_s, data));
  int i;

  if (datafmts == NULL || nr_datafmts == 0)
    {
      return -EINVAL;
    }

  /* This driver delivers demodisaiced frames only, and only at the geometry
   * the sensor was configured for: VICAP is programmed once, with the
   * sensor's frame size, and there is no scaler in the path.
   */

  for (i = 0; i < nr_datafmts; i++)
    {
      if (datafmts[i].pixelformat != IMGDATA_PIX_FMT_NV12 &&
          datafmts[i].pixelformat != IMGDATA_PIX_FMT_YUV420P)
        {
          return -EINVAL;
        }

      if (datafmts[i].width != priv->cfg.width ||
          datafmts[i].height != priv->cfg.height)
        {
          return -EINVAL;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_set_buf
 ****************************************************************************/

static int rk3576_vicap_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR uint8_t *addr, uint32_t size)
{
  FAR struct rk3576_vicap_s *priv =
      (FAR struct rk3576_vicap_s *)((uintptr_t)data -
                                    offsetof(struct rk3576_vicap_s, data));
  irqstate_t flags;

  if (addr == NULL || size == 0)
    {
      return -EINVAL;
    }

  /* The framework calls this once before each capture and again after every
   * completed frame, each time with the buffer it wants filled next.  Note
   * that IMGDATA_SET_BUF reports failure as NULL, not as a negative errno,
   * so the return value here must stay a plain status.
   *
   * The after-every-frame call arrives from inside complete_capture(), which
   * holds a spinlock with preemption disabled, so this must not block.  The
   * driver's mutex is therefore unusable here: a contended nxmutex_lock()
   * would try to suspend a thread that cannot be suspended, which asserts.
   * Two word stores are all this needs, and the spinlock that guards the
   * rest of the frame state covers them.
   */

  flags = spin_lock_irqsave(&priv->irqlock);
  priv->dst = addr;
  priv->dstsize = size;
  spin_unlock_irqrestore(&priv->irqlock, flags);

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_start_capture
 ****************************************************************************/

static int rk3576_vicap_start_capture(FAR struct imgdata_s *data,
                                      uint8_t nr_datafmts,
                                      FAR imgdata_format_t *datafmts,
                                      FAR imgdata_interval_t *interval,
                                      FAR imgdata_capture_t callback,
                                      FAR void *arg)
{
  FAR struct rk3576_vicap_s *priv =
      (FAR struct rk3576_vicap_s *)((uintptr_t)data -
                                    offsetof(struct rk3576_vicap_s, data));
  uintptr_t base = priv->base;
  uint8_t input = priv->cfg.input;
  uint8_t id = priv->cfg.id;
  irqstate_t flags;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODEV;
    }

  /* The state below is shared with the completion path, which runs without
   * the driver mutex, so publish it under the spinlock that path uses.  The
   * interrupt is not enabled yet, so the ISR cannot be racing these stores.
   *
   * The buffer the framework offered is deliberately left alone: it arrives
   * through SET_BUF before this call, and consuming it here would throw the
   * first frame away.  The epoch is bumped so that a frame left over from a
   * previous stream cannot be delivered into this one.
   */

  flags = spin_lock_irqsave(&priv->irqlock);
  priv->callback = callback;
  priv->arg = arg;
  priv->capturing = true;
  priv->raw_pending = -1;
  priv->dma_idx = -1;
  priv->frame_scheduled = false;
  priv->errstat = 0;
  priv->errstat_reported = 0;
  priv->fs_count = 0;
  priv->fe_count = 0;
  priv->isr_frames = 0;
  priv->stream_epoch++;

  /* The two measurements the in-place decision is made from start empty, so
   * the first frame of a stream takes the copy: the interval is not known
   * until two DMA ends have been seen, and the demosaic cost is not known
   * until one has been run.  Carrying either across streams would apply one
   * mode's timing to another.
   */

  priv->isr_ticks = 0;
  priv->interval_ticks = 0;
  priv->debayer_us = 0;
  priv->inplace_decided = false;
  priv->inplace_ok = false;
  priv->inplace_samples = 0;

  /* The counts start again with the stream so that the report at the end of
   * it describes that stream rather than a lifetime total: a mode and a
   * frame rate belong to the stream, and an average over several of them
   * describes neither.
   */

  priv->framecount = 0;
  priv->dropcount = 0;
  priv->drop_overrun = 0;
  priv->drop_nobuf = 0;
  priv->drop_stale = 0;
  priv->drop_queue = 0;
  priv->racecount = 0;
  priv->frame_us = 0;
  priv->copy_us = 0;
  priv->debayer_us_sum = 0;
  priv->inplace_frames = 0;
  priv->inplace_us_sum = 0;
  priv->copy_frames = 0;
  priv->copy_demosaic_us_sum = 0;

  /* The white balance starts again from unity on every stream.
   *
   * The gains are measured from the frames themselves, so they are only ever
   * as valid as the channel accounting that produced them.  Carrying them
   * across streams would mean a stale set survives whatever invalidated it --
   * a different sensor mode, or a corrected Bayer order -- and then corrects
   * the colour in the wrong direction from the first frame.  The loop reaches
   * within a percent of its target in seven frames, which is shorter than any
   * capture worth measuring.
   */

  priv->wb[0] = RK3576_VICAP_WB_ONE;
  priv->wb[1] = RK3576_VICAP_WB_ONE;
  priv->wb[2] = RK3576_VICAP_WB_ONE;

  spin_unlock_irqrestore(&priv->irqlock, flags);

  /* Drop anything a previous run may have left pending. */

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_INTSTAT(input), 0xffffffffu);

  /* Reset the interface and its DMA before programming them.
   *
   * The interface keeps its own idea of where a frame begins, and that idea
   * outlives a stream.  A sensor restarted between streams is not a receiver
   * restarted with it, so the receiver's boundary can lock onto the truncated
   * first frame the restart produces -- and because the DMA is told to follow
   * the boundary (sw_dma_adapt_en), everything the receiver then carries is
   * displaced by that one line.
   *
   * What that looks like is worth stating, because it names none of this: the
   * picture keeps its brightness and loses its colour, gains a fine grid over
   * it, and has a first row belonging to the frame before.  The demosaic is
   * reading the right order at the wrong phase -- one line of displacement
   * puts the sites it calls red and blue both on green -- so no white balance
   * setting improves it and nothing fails.  The frame counters agree
   * throughout, which is why this is worth resetting on the way in rather
   * than diagnosing on the way out.
   *
   * The reset covers the interface and its DMA but not the register file, so
   * the geometry and addresses programmed below survive it.  It is done first
   * so that what gets programmed is what the fresh state sees.
   *
   * A stream that begins on a receiver that has just been initialised comes
   * out right, and that is the state this reproduces for every stream: the
   * same reset the power-on path leaves behind.  Nothing here is specific to
   * a geometry or a frame rate -- the displacement follows where the boundary
   * was left, not how long the frame is.
   */

  {
    uint32_t saved = rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_CTRL(input));

    rk3576_vicap_putreg(
        base, RK3576_VICAP_MIPI_CTRL(input),
        (saved & ~RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_MASK) |
            ((uint32_t)RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_DIRECT
             << RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_SHIFT) |
            RK3576_VICAP_MIPI_CTRL_SOFT_RST | RK3576_VICAP_MIPI_CTRL_DMA_RST);

    /* Long enough for a reset of the block's own clock domain to settle, and
     * short enough that it cannot be mistaken for a frame's worth of time.
     */

    up_udelay(10);

    /* Releasing dma_rst is what completes the DMA's reset, and restoring the
     * saved value is what keeps the drop-frame counters and cap_en as they
     * were rather than zeroing them.
     */

    rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_CTRL(input),
                        saved & ~(RK3576_VICAP_MIPI_CTRL_SOFT_RST |
                                  RK3576_VICAP_MIPI_CTRL_DMA_RST));
  }

  rk3576_vicap_input_configure(priv);

  /* The ID slot stays disabled while the addresses and geometry are being
   * written; this driver writes them in one go, but keeping capture off
   * until the very end costs nothing and removes the ordering question.
   */

  rk3576_vicap_putreg(
      base, RK3576_VICAP_MIPI_ID0_CTRL0(input) + id * 8u,
      rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_ID0_CTRL0(input) + id * 8u) |
          RK3576_VICAP_ID_CTRL0_DMA_EN | RK3576_VICAP_ID_CTRL0_CAP_EN);

  /* Unmask only the frame boundaries and the conditions that mean "the
   * picture is wrong", not every error bit.
   */

  rk3576_vicap_putreg(base, RK3576_VICAP_MIPI_INTEN(input),
                      RK3576_VICAP_MIPI_INT_FRAME_START_ID0 |
                          RK3576_VICAP_MIPI_INT_FRAME_END_ID0 |
                          RK3576_VICAP_MIPI_INT_FRAME0_DMA_END_ID0 |
                          RK3576_VICAP_MIPI_INT_FRAME1_DMA_END_ID0 |
                          RK3576_VICAP_MIPI_INT_DMA_Y_FIFO_OVF |
                          RK3576_VICAP_MIPI_INT_DMA_UV_FIFO_OVF |
                          RK3576_VICAP_MIPI_INT_BANDWIDTH_LACK |
                          RK3576_VICAP_MIPI_INT_CSI2RX_FIFO_OVF |
                          RK3576_VICAP_MIPI_INT_SIZE_ERR_ID0);

  up_enable_irq(RK3576_IRQ_VICAP);

  /* Enable capture on this path.  This is a field write rather than a plain
   * store: MIPI_CTRL's reset value carries the drop-frame counters, and the
   * full-register form would silently zero them.
   */

  rk3576_vicap_putreg(
      base, RK3576_VICAP_MIPI_CTRL(input),
      rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_CTRL(input)) |
          RK3576_VICAP_MIPI_CTRL_CAP_EN);

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_dump_locked
 *
 * Description:
 *   Log the state that distinguishes the two ways a capture looks wrong:
 *   the sensor never sending anything, or the data arriving and being
 *   mistranslated into a picture nobody asked for.  The frame counters on
 *   their own cannot tell those apart -- both show frames completing -- so
 *   the parts that decide it are the measured pixel/line counts and a
 *   sample of the RAW buffer the DMA actually filled.
 *
 *   The caller must already hold priv->lock; this exists separately from
 *   rk3576_vicap_dump() so that the capture path can report while it is
 *   holding the lock, instead of taking it twice.
 *
 ****************************************************************************/

static void rk3576_vicap_dump_locked(FAR struct rk3576_vicap_s *priv)
{
  uintptr_t base = priv->base;
  uint8_t input = priv->cfg.input;
  uint8_t id = priv->cfg.id;
  uint32_t size_num;
  uint32_t lo = 0xffffu;
  uint32_t hi = 0;
  uint32_t sum = 0;
  uint32_t n = 0;
  uint32_t unaligned = 0;
  int first_unaligned_row = -1;
  int first_unaligned_col = -1;
  unsigned int i;

  size_num = rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_SIZE_NUM_ID0(input) +
                                           id * 4u);

  _info("VICAP: frames %" PRIu32 ", drops %" PRIu32 " (overrun %" PRIu32
        ", no buffer %" PRIu32 ", stale %" PRIu32 ", queue %" PRIu32
        "), errstat 0x%08" PRIx32 ", %" PRIu32 " us/frame, races %" PRIu32
        "%s\n",
        priv->framecount, priv->dropcount, priv->drop_overrun,
        priv->drop_nobuf, priv->drop_stale, priv->drop_queue, priv->errstat,
        priv->framecount > 0u ? priv->frame_us / priv->framecount : 0u,
        priv->racecount,
        priv->inplace_decided
            ? (priv->inplace_ok ? ", reading in place" : ", copying")
            : ", no interval yet");

  /* The two paths timed apart, because the choice between them is made from
   * these numbers and an average over both says nothing about either.  If
   * reading in place is not cheaper than copying then the copy is not a cost
   * to be removed but the thing that makes the read fast, and the choice
   * above is the wrong way round.
   */

  {
    uint32_t interval_us = priv->interval_ticks * USEC_PER_TICK;
    uint32_t demosaic_us =
        priv->framecount > 0u ? priv->debayer_us_sum / priv->framecount : 0u;
    uint32_t inplace_us = priv->inplace_frames > 0u
                              ? priv->inplace_us_sum / priv->inplace_frames
                              : 0u;
    uint32_t copy_us = priv->copy_frames > 0u
                           ? priv->copy_demosaic_us_sum / priv->copy_frames
                           : 0u;

    _info("VICAP: interval %" PRIu32 " us, demosaic %" PRIu32
          " us (in place %" PRIu32 " us over %" PRIu32 ", copied %" PRIu32
          " us over %" PRIu32 "), copy %" PRIu32 " us\n",
          interval_us, demosaic_us, inplace_us, priv->inplace_frames, copy_us,
          priv->copy_frames,
          priv->framecount > 0u ? priv->copy_us / priv->framecount : 0u);
  }

  /* Both frame sources are printed together on purpose.  A DMA-end count
   * on its own only proves that buffers are filling; it is the interface
   * counts that prove the sensor's frame boundaries are arriving, and that
   * is the difference between a frame aligned to the sensor and one cut
   * out of it at an arbitrary line.
   */

  _info("VICAP: frame starts %" PRIu32 ", frame ends %" PRIu32
        ", dma ends %" PRIu32 "\n",
        priv->fs_count, priv->fe_count, priv->isr_frames);

  _info(
      "VICAP: glb 0x%08" PRIx32 ", mipi_ctrl 0x%08" PRIx32
      ", id0_ctrl0 0x%08" PRIx32 ", id0_ctrl1 0x%08" PRIx32 "\n",
      rk3576_vicap_getreg(base, RK3576_VICAP_GLB_CTRL),
      rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_CTRL(input)),
      rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_ID0_CTRL0(input) + id * 8u),
      rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_ID0_CTRL1(input) + id * 8u));

  /* size_num is the hardware's own measurement of the last frame, and it is
   * only meaningful while a frame is being captured.
   *
   * This dump runs after the stream has stopped, and by then the block is
   * idle -- which is the same reason id0_ctrl0 reads back as zero however it
   * was programmed, and why every hardware field below is worth less than it
   * looks.  The line field has held at the configured height over runs, but
   * the payload field has read 800 and 648 on two runs of the same geometry,
   * so it is reported as read rather than converted into a pixel count that
   * its behaviour does not support.
   */

  _info("VICAP: set_size 0x%08" PRIx32 ", vlw %" PRIu32
        ", size_num 0x%08" PRIx32 " (line %" PRIu32 ", payload %" PRIu32
        " as read)\n",
        rk3576_vicap_getreg(base,
                            RK3576_VICAP_MIPI_ID0_SET_SIZE(input) + id * 4u),
        priv->stride, size_num,
        (size_num >> RK3576_VICAP_SIZE_NUM_LINE_SHIFT) &
            RK3576_VICAP_SIZE_NUM_FIELD_MASK,
        (size_num >> RK3576_VICAP_SIZE_NUM_PIX_SHIFT) &
            RK3576_VICAP_SIZE_NUM_FIELD_MASK);

  _info("VICAP: frame_num_vc0 0x%08" PRIx32 ", addr0 %p, addr1 %p\n",
        rk3576_vicap_getreg(base,
                            RK3576_VICAP_MIPI_FRAME_NUM_VC0(input) + id * 4u),
        priv->raw[0], priv->raw[1]);

  _info("VICAP: inten 0x%08" PRIx32 ", intstat 0x%08" PRIx32
        ", mmu_status 0x%08" PRIx32 ", mmu_int 0x%08" PRIx32 "\n",
        rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_INTEN(input)),
        rk3576_vicap_getreg(base, RK3576_VICAP_MIPI_INTSTAT(input)),
        rk3576_vicap_getreg(base, RK3576_VICAP_MMU_STATUS),
        rk3576_vicap_getreg(base, RK3576_VICAP_MMU_INT_STATUS));

  if (priv->raw[0] == NULL)
    {
      return;
    }

  /* The DMA wrote this buffer, not the CPU, so drop whatever the cache may
   * still be holding before looking at it.
   */

  up_invalidate_dcache((uintptr_t)priv->raw[0],
                       (uintptr_t)priv->raw[0] + priv->rawlen);

  /* A run of identical words, or all zeroes, means the buffer is not what
   * the sensor sent however healthy the frame counters look.
   */

  for (i = 0; i < 32u; i += 8u)
    {
      FAR const uint16_t *w = (FAR const uint16_t *)priv->raw[0] + i;

      _info("VICAP: raw[0]+%4u = %04x %04x %04x %04x %04x %04x %04x %04x\n", i,
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }

  /* The four parity classes of the raw mosaic, each averaged over the whole
   * frame.
   *
   * Every site of a Bayer tile belongs to one of four classes, so these four
   * numbers say directly which sites carry which colour -- which is the one
   * thing the demosaicer cannot be asked, because it is told the answer and
   * has no way to notice when the answer is wrong.  Green is the pair of
   * classes that agree with each other and sit above the average of the
   * other two: green is the most sensitive channel of a Bayer sensor, so on
   * any ordinary scene its two classes are the high pair.
   *
   * This is what separates "the tile is the other way round" from "the tile
   * is right and something else is wrong".  The two look identical in the
   * finished picture -- with the tile reversed the sites called red and blue
   * are both really green, so the frame loses its colour and turns into a
   * tinted grey while the white balance reports nothing to correct.
   *
   * Printed as read, with no assumption about which class is which: the
   * whole point is to see that rather than to have it decided for us.
   */

  {
    FAR const uint16_t *rp = (FAR const uint16_t *)priv->raw[0];
    size_t stride_px = priv->stride / 2u;
    uint64_t csum[4] = { 0, 0, 0, 0 };
    uint32_t cnum[4] = { 0, 0, 0, 0 };
    int row;
    int col;

    for (row = 0; row < (int)priv->cfg.height; row++)
      {
        for (col = 0; col < (int)priv->cfg.width; col++)
          {
            unsigned int k =
                ((unsigned int)row & 1u) * 2u | ((unsigned int)col & 1u);

            csum[k] +=
                (uint64_t)(rp[(size_t)row * stride_px + (size_t)col] >> 6);
            cnum[k]++;
          }
      }

    _info("VICAP: raw classes ee %" PRIu32 " eo %" PRIu32 " oe %" PRIu32
          " oo %" PRIu32 " (10-bit means of raw[0])\n",
          (uint32_t)(csum[0] / cnum[0]), (uint32_t)(csum[1] / cnum[1]),
          (uint32_t)(csum[2] / cnum[2]), (uint32_t)(csum[3] / cnum[3]));
  }

  /* Scan only the pixels the sensor filled.
   *
   * The stride is wider than the frame -- 1312 pixels carrying 1296 -- and
   * the allocation is taller than the frame, so walking the whole buffer
   * counts two regions no one ever wrote: the sixteen pixels of padding at
   * the end of every row, and the sixty-four slack rows below the picture.
   * Whatever the DMA heap happened to contain there is not raw data and
   * cannot be unaligned raw data either, and counting it made this figure
   * read in the tens of thousands on every healthy frame -- which is the
   * same as not having it.
   *
   * Over the active area it means what it says, and it is the one check here
   * that looks at the sensor's own words rather than at anything derived
   * from them: a DMA that drops or misplaces part of a line leaves words
   * whose low six bits are not clear.  A non-zero count over the picture is
   * therefore a capture fault, and the first one's position says where.
   */

  {
    FAR const uint16_t *rp = (FAR const uint16_t *)priv->raw[0];
    size_t stride_px = priv->stride / 2u;
    int row;
    int col;

    for (row = 0; row < (int)priv->cfg.height; row++)
      {
        for (col = 0; col < (int)priv->cfg.width; col++)
          {
            uint16_t v = rp[(size_t)row * stride_px + (size_t)col];

            /* Uncompacted RAW10 is written high-aligned, so the sample
             * occupies bits [15:6] and the low six bits are left clear.  A
             * word that breaks that is not a pixel value at all, however
             * plausible its magnitude looks, so it is counted and kept out
             * of the range below -- otherwise one stray word decides what
             * the frame's peak signal appears to be.
             */

            if ((v & 0x3fu) != 0u)
              {
                if (unaligned == 0u)
                  {
                    first_unaligned_row = row;
                    first_unaligned_col = col;
                  }

                unaligned++;
                continue;
              }

            /* Report the range in the sensor's own units, where full scale
             * is 1023.  That is the number that says whether the light
             * reaching the sensor is anywhere near filling it, which
             * absolute 16-bit words do not make obvious.
             */

            v >>= 6;

            if (v < lo)
              {
                lo = v;
              }

            if (v > hi)
              {
                hi = v;
              }

            sum += v;
            n++;
          }
      }
  }

  /* Peak and mean in sensor units.  A capture that is merely dim shows a
   * low peak; one that is broken or unlit shows a peak sitting on the black
   * level.  Comparing a run against a brighter scene is what tells those
   * apart, so the peak is quoted as the headline number.
   */

  _info("VICAP: raw[0] peak %" PRIu32 "/1023, min %" PRIu32 ", mean %" PRIu32
        ", unaligned words %" PRIu32 " (first at row %d col %d)\n",
        hi, lo, n > 0 ? sum / n : 0u, unaligned, first_unaligned_row,
        first_unaligned_col);

  _info("VICAP: wb gains %" PRIu32 "/256 %" PRIu32 "/256 %" PRIu32 "/256\n",
        priv->wb[0], priv->wb[1], priv->wb[2]);

  rk3576_csi_host_dump();
}

/****************************************************************************
 * Name: rk3576_vicap_stop_capture
 ****************************************************************************/

static int rk3576_vicap_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct rk3576_vicap_s *priv =
      (FAR struct rk3576_vicap_s *)((uintptr_t)data -
                                    offsetof(struct rk3576_vicap_s, data));
  bool was_capturing;
  irqstate_t flags;

  /* This is reachable from complete_capture(), which holds a spinlock with
   * preemption disabled: the framework stops the stream from there when it
   * cannot find a vacant buffer to receive the frame it just finished.
   * Nothing here may block, so the driver's mutex is not taken at all.  The
   * flags shared with the completion path live under the spinlock, and the
   * register writes below are plain stores.
   *
   * Clearing the offered buffer matters as much as clearing the callback:
   * once the framework has stopped, no new buffer is coming, and a frame that
   * still believed it had one would be delivered into a stopped stream.
   */

  flags = spin_lock_irqsave(&priv->irqlock);
  was_capturing = priv->capturing;
  priv->capturing = false;
  priv->callback = NULL;
  priv->arg = NULL;
  priv->dst = NULL;
  priv->dstsize = 0;
  priv->stream_epoch++;
  spin_unlock_irqrestore(&priv->irqlock, flags);

  if (!was_capturing)
    {
      return OK;
    }

  rk3576_vicap_input_disable(priv);
  up_disable_irq(RK3576_IRQ_VICAP);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vicap_cam3a_ioctl
 *
 * Description:
 *   The 3A control plane: the measurement the demosaicer took out, and the
 *   white balance in.  See <arch/chip/cam3a.h> for why measurement and
 *   application are split the way they are; exposure and gain are not here
 *   at all, because they are the sensor's and reachable through the ordinary
 *   V4L2 controls on the capture device.
 *
 ****************************************************************************/

static int rk3576_vicap_cam3a_ioctl(FAR struct file *filep, int cmd,
                                    unsigned long arg)
{
  FAR struct rk3576_vicap_s *priv = &g_vicap;
  irqstate_t flags;
  int ret = OK;

  switch (cmd)
    {
      case CAM3A_GET_STATS:
        {
          FAR struct cam3a_stats_s *stats =
              (FAR struct cam3a_stats_s *)(uintptr_t)arg;

          if (stats == NULL)
            {
              return -EINVAL;
            }

          /* Copied out under the lock the demosaicer publishes under, so the
           * caller gets one frame's measurement rather than a mixture.
           */

          flags = spin_lock_irqsave(&priv->irqlock);

          stats->sequence = priv->stat_seq;
          stats->width = priv->stat_w;
          stats->height = priv->stat_h;
          stats->count = priv->stat_count;
          stats->sum[0] = priv->stat_sum[0];
          stats->sum[1] = priv->stat_sum[1];
          stats->sum[2] = priv->stat_sum[2];
          stats->wb[0] = priv->wb[0];
          stats->wb[1] = priv->wb[1];
          stats->wb[2] = priv->wb[2];
          stats->flags = priv->awb_active ? CAM3A_FLAG_AWB_ACTIVE : 0u;

          spin_unlock_irqrestore(&priv->irqlock, flags);
        }
        break;

      case CAM3A_SET_WB:
        {
          FAR const struct cam3a_wb_s *wb =
              (FAR const struct cam3a_wb_s *)(uintptr_t)arg;
          unsigned int i;

          if (wb == NULL)
            {
              return -EINVAL;
            }

          for (i = 0; i < 3u; i++)
            {
              /* Below the floor is refused rather than applied.  A gain of
               * zero would black the channel out, and no later correction
               * can bring it back, because multiplying by zero has already
               * destroyed what was there.  It is never what a caller meant.
               *
               * The floor is CAM3A_WB_MIN from the interface, which is one,
               * so this is the same test the interface documents rather than
               * a policy of this file's.
               */

              if (wb->gain[i] < CAM3A_WB_MIN)
                {
                  _err("ERROR: VICAP: white balance gain %u is %u, under the "
                       "%u the interface allows\n",
                       i, wb->gain[i], CAM3A_WB_MIN);
                  return -EINVAL;
                }

              /* The upper bound is not a policy limit but the largest gain
               * the demosaic can apply.  The vector path narrows the gain to
               * sixteen bits after scaling it by four, so anything above
               * RK3576_VICAP_WB_HARD_MAX would arrive in the picture as a
               * different number -- a gain the caller did not ask for,
               * applied silently.  Refusing is the only answer that does not
               * lie about what was applied.
               */

              if (wb->gain[i] > RK3576_VICAP_WB_HARD_MAX)
                {
                  _err("ERROR: VICAP: white balance gain %u is %u, over the "
                       "%u the demosaic can apply\n",
                       i, wb->gain[i], RK3576_VICAP_WB_HARD_MAX);
                  return -EINVAL;
                }
            }

          flags = spin_lock_irqsave(&priv->irqlock);

          priv->wb[0] = wb->gain[0];
          priv->wb[1] = wb->gain[1];
          priv->wb[2] = wb->gain[2];

          /* Setting the gains is taking them over.  A caller computing them
           * itself must not have the driver's own loop overwrite them one
           * frame later, and doing this as part of the same critical section
           * is what leaves no window in which both are steering.
           */

          priv->awb_active = false;

          spin_unlock_irqrestore(&priv->irqlock, flags);
        }
        break;

      case CAM3A_SET_AWB:
        {
          FAR const int *enable = (FAR const int *)(uintptr_t)arg;

          if (enable == NULL)
            {
              return -EINVAL;
            }

          flags = spin_lock_irqsave(&priv->irqlock);
          priv->awb_active = (*enable != 0);
          spin_unlock_irqrestore(&priv->irqlock, flags);
        }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

static const struct file_operations g_rk3576_vicap_cam3a_fops = {
  .ioctl = rk3576_vicap_cam3a_ioctl,
};

/****************************************************************************
 * Name: rk3576_vicap_cam3a_register
 *
 * Description:
 *   Publish the 3A control plane.  Called from the capture driver's own
 *   initialisation so that the interface appears with the block it belongs
 *   to, rather than being a board's separate decision: a board that brings
 *   VICAP up gets the control plane, and one that does not gets neither.
 *
 ****************************************************************************/

static int rk3576_vicap_cam3a_register(void)
{
  int ret;

  ret = register_driver(CAM3A_DEVPATH, &g_rk3576_vicap_cam3a_fops, 0666, NULL);
  if (ret < 0)
    {
      _err("ERROR: VICAP: failed to register %s: %d\n", CAM3A_DEVPATH, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_vicap_buffers_free
 *
 * Description:
 *   Hand back the frame buffers and the demosaic copy.  The caller must hold
 *   the driver lock, and must have stopped the capture path first: these are
 *   the addresses the DMA writes to.
 *
 ****************************************************************************/

static void rk3576_vicap_buffers_free(FAR struct rk3576_vicap_s *priv)
{
  unsigned int i;

  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      if (priv->raw[i] != NULL)
        {
          rk3576_dma_free(priv->raw[i], priv->dmaalloc);
          priv->raw[i] = NULL;
        }
    }

  if (priv->snap != NULL)
    {
      kmm_free(priv->snap);
      priv->snap = NULL;
    }
}

/****************************************************************************
 * Name: rk3576_vicap_buffers_alloc
 *
 * Description:
 *   Work out the frame layout a mode needs and allocate for it.  The caller
 *   must hold the driver lock and must have stopped the capture path, and
 *   must release the buffers the driver is holding itself.
 *
 *   The new buffers are obtained into locals and only published once every
 *   one of them has succeeded.  That ordering is what makes this safe to run
 *   on a driver that is already up: a failure halfway leaves the driver's own
 *   pointers and sizes exactly as they were, so a caller that was receiving
 *   frames can carry on receiving them instead of being left with nothing.
 *
 *   The DMA is pointed straight at physical DDR addresses, so the frame
 *   buffers come from the DMA heap: physically contiguous, below 4GB and
 *   already 64-byte aligned.  The demosaic copy is ordinary memory -- nothing
 *   but the CPU touches it -- and is aligned to a cache line for tidiness
 *   rather than necessity.
 *
 * Input Parameters:
 *   priv   - Driver state.  Its stride, rawlen, dmaalloc, raw[] and snap are
 *            replaced on success and left untouched on failure.
 *   config - The mode to lay the buffers out for.
 *
 * Returned Value:
 *   OK on success; -ENOMEM if the heaps could not supply the buffers.
 *
 ****************************************************************************/

static int
rk3576_vicap_buffers_alloc(FAR struct rk3576_vicap_s *priv,
                           FAR const struct rk3576_vicap_config *config)
{
  FAR uint8_t *raw[RK3576_VICAP_NBUF];
  FAR uint8_t *snap = NULL;
  uint32_t raw_stride;
  size_t stride;
  size_t rawlen;
  size_t dmaalloc;
  unsigned int i;

  /* RAW frame geometry.  Uncompacted samples take one 16-bit word each;
   * compacted RAW10 packs four samples into five bytes.
   */

  if (config->uncompact)
    {
      raw_stride = (uint32_t)config->width * 2u;
    }
  else
    {
      raw_stride =
          ((uint32_t)config->width * (uint32_t)config->raw_bits + 7u) / 8u;
    }

  stride = (raw_stride + RK3576_VICAP_STRIDE_ALIGN - 1u) &
           ~(RK3576_VICAP_STRIDE_ALIGN - 1u);
  rawlen = stride * config->height;
  dmaalloc = stride * (config->height + RK3576_VICAP_DMA_SLACK_LINES);

  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      raw[i] = NULL;
    }

  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      raw[i] = rk3576_dma_alloc(dmaalloc);
      if (raw[i] == NULL)
        {
          _err("ERROR: VICAP out of DMA heap (%zu bytes x %u)\n", dmaalloc,
               (unsigned int)RK3576_VICAP_NBUF);
          goto errout;
        }
    }

  snap = kmm_memalign(64, rawlen);
  if (snap == NULL)
    {
      _err("ERROR: VICAP out of memory for a %zu-byte frame copy\n", rawlen);
      goto errout;
    }

  /* Everything was obtained, so the driver's own layout can be replaced. */

  priv->stride = stride;
  priv->rawlen = rawlen;
  priv->dmaalloc = dmaalloc;

  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      priv->raw[i] = raw[i];
    }

  priv->snap = snap;
  return OK;

errout:
  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      if (raw[i] != NULL)
        {
          rk3576_dma_free(raw[i], dmaalloc);
        }
    }

  return -ENOMEM;
}

/****************************************************************************
 * Name: rk3576_vicap_initialize
 ****************************************************************************/

int rk3576_vicap_initialize(FAR const struct rk3576_vicap_config *config,
                            FAR struct imgdata_s **data)
{
  FAR struct rk3576_vicap_s *priv = &g_vicap;
  int ret;

  if (config == NULL || data == NULL)
    {
      return -EINVAL;
    }

  if (config->input > RK3576_VICAP_INPUT_MIPI4 || config->id > 3 ||
      config->width == 0 || config->height == 0 || (config->width & 3u) != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  priv->base = RK3576_VICAP_ADDR;
  priv->cfg = *config;

  /* Bring the power domain up before touching any register in it: until the
   * initial reset is released the whole domain reads back zero.
   */

  ret = rk3576_vicap_power_domain_on();
  if (ret < 0)
    {
      goto errout_unlock;
    }

  rk3576_vicap_release_resets(config->input);

  ret = rk3576_vicap_enable_clocks(priv, config->input);
  if (ret < 0)
    {
      goto errout_unlock;
    }

  ret = rk3576_vicap_buffers_alloc(priv, config);
  if (ret < 0)
    {
      goto errout_free;
    }

  /* Global control: capture enabled, no soft reset, and the DMA taking its
   * frame height from the interface.
   *
   * DMA_ADAPT_EN decides where the DMA's idea of a frame begins.  Enabled
   * -- which is also its reset value -- the DMA takes the frame boundary
   * from the interface, so a buffer starts filling where the sensor's
   * frame starts.  Disabled, the DMA counts sw_height lines of its own
   * instead, and that count has no relationship to the sensor's frame:
   * both agree on how many lines a frame holds, so nothing is lost, but
   * the two boundaries sit at a fixed offset from each other and the
   * picture comes out rotated by it.  Clearing the bit does not make the
   * transfer deterministic, it decouples the transfer from the frame.
   *
   * Following the interface is also what makes the height a measurement
   * rather than a setting, which is why the buffers carry slack.
   */

  {
    uint32_t glb = rk3576_vicap_getreg(priv->base, RK3576_VICAP_GLB_CTRL);

    glb &= ~RK3576_VICAP_GLB_CTRL_SW_SOFT_RST;
    glb |=
        RK3576_VICAP_GLB_CTRL_SW_CAP_EN | RK3576_VICAP_GLB_CTRL_DMA_ADAPT_EN;

    rk3576_vicap_putreg(priv->base, RK3576_VICAP_GLB_CTRL, glb);
  }

  rk3576_vicap_mmu_configure(priv->base);

  /* The MIPI path's own control.  Its reset value already has soft_rst clear
   * (the only bit this driver wants off), so a field write that clears that
   * one bit leaves the drop-frame counters at their documented defaults
   * instead of flattening the whole register.
   */

  rk3576_vicap_putreg(
      priv->base, RK3576_VICAP_MIPI_CTRL(config->input),
      rk3576_vicap_getreg(priv->base, RK3576_VICAP_MIPI_CTRL(config->input)) &
          ~RK3576_VICAP_MIPI_CTRL_SOFT_RST);

  rk3576_vicap_putreg(priv->base, RK3576_VICAP_MIPI_INTEN(config->input), 0);
  rk3576_vicap_putreg(priv->base, RK3576_VICAP_MIPI_INTSTAT(config->input),
                      0xffffffffu);

  ret = irq_attach(RK3576_IRQ_VICAP, rk3576_vicap_isr, NULL);
  if (ret < 0)
    {
      _err("ERROR: VICAP failed to attach its interrupt: %d\n", ret);
      goto errout_free;
    }

  priv->initialized = true;
  priv->data.ops = &g_rk3576_vicap_ops;

  *data = &priv->data;

  _info("VICAP: MIPI%u ID%u, %ux%u RAW%u %s%s, vc=%u dt=0x%02x, "
        "stride=%" PRIu32 ", frame=%zu bytes\n",
        (unsigned int)config->input, (unsigned int)config->id, config->width,
        config->height, (unsigned int)config->raw_bits,
        config->uncompact ? "uncompact" : "compact",
        (config->uncompact && config->align_high) ? " high-align" : "",
        (unsigned int)config->vc, (unsigned int)config->dt, priv->stride,
        priv->rawlen);

  nxmutex_unlock(&priv->lock);

  /* The control plane is published last, so that it only appears once the
   * capture path behind it is complete -- an application that finds the
   * device and reads it must not find a half-initialised block.
   *
   * A failure here is logged and does not fail the initialisation.  The
   * camera captures without it; an application that wants 3A should not be
   * denied the picture because a device node could not be created, and the
   * log is where the difference between "no 3A" and "no camera" is visible.
   */

  rk3576_vicap_cam3a_register();

  return OK;

errout_free:
  rk3576_vicap_buffers_free(priv);

  if (priv->iclk != NULL)
    {
      clk_disable(priv->iclk);
    }

  if (priv->hclk != NULL)
    {
      clk_disable(priv->hclk);
    }

  if (priv->aclk != NULL)
    {
      clk_disable(priv->aclk);
    }

  if (priv->dclk != NULL)
    {
      clk_disable(priv->dclk);
    }

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_vicap_uninitialize
 ****************************************************************************/

int rk3576_vicap_uninitialize(void)
{
  FAR struct rk3576_vicap_s *priv = &g_vicap;
  unsigned int i;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  rk3576_vicap_input_disable(priv);
  up_disable_irq(RK3576_IRQ_VICAP);
  irq_detach(RK3576_IRQ_VICAP);

  /* Take the frame buffers away only once the DMA has stopped, so no
   * in-flight write can land in freed memory.
   */

  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      if (priv->raw[i] != NULL)
        {
          rk3576_dma_free(priv->raw[i], priv->dmaalloc);
          priv->raw[i] = NULL;
        }
    }

  if (priv->snap != NULL)
    {
      kmm_free(priv->snap);
      priv->snap = NULL;
    }

  if (priv->iclk != NULL)
    {
      clk_disable(priv->iclk);
    }

  if (priv->hclk != NULL)
    {
      clk_disable(priv->hclk);
    }

  if (priv->aclk != NULL)
    {
      clk_disable(priv->aclk);
    }

  if (priv->dclk != NULL)
    {
      clk_disable(priv->dclk);
    }

  priv->initialized = false;
  priv->capturing = false;

  nxmutex_unlock(&priv->lock);

  /* Withdraw the control plane with the block it describes, so that a
   * leftover open cannot reach a driver that is no longer there.  After the
   * unlock rather than before it: the ioctl path takes irqlock and not this
   * mutex, so holding it while unregistering would serialise two things that
   * do not need to be.
   */

  unregister_driver(CAM3A_DEVPATH);

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_reconfigure
 ****************************************************************************/

int rk3576_vicap_reconfigure(FAR const struct rk3576_vicap_config *config)
{
  FAR struct rk3576_vicap_s *priv = &g_vicap;
  FAR uint8_t *oldraw[RK3576_VICAP_NBUF];
  FAR uint8_t *oldsnap;
  size_t olddmaalloc;
  unsigned int i;
  int ret;

  if (config == NULL)
    {
      return -EINVAL;
    }

  if (config->input > RK3576_VICAP_INPUT_MIPI4 || config->id > 3 ||
      config->width == 0 || config->height == 0 || (config->width & 3u) != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODEV;
    }

  /* Two things may not change here, and both are refused rather than
   * attempted.
   *
   * A different input would be a different MIPI port, with its own clock
   * gate, its own reset line and its own set of registers: that is a
   * bring-up, not a reconfiguration.
   *
   * And a stream has to be off.  The frame buffers below are the addresses
   * the DMA writes to, so replacing them under a running capture would hand
   * the hardware memory that is no longer its own.
   */

  if (config->input != priv->cfg.input || config->id != priv->cfg.id)
    {
      _err("ERROR: VICAP cannot reconfigure from MIPI%u ID%u to MIPI%u ID%u;"
           " that needs a bring-up\n",
           (unsigned int)priv->cfg.input, (unsigned int)priv->cfg.id,
           (unsigned int)config->input, (unsigned int)config->id);
      nxmutex_unlock(&priv->lock);
      return -EINVAL;
    }

  if (priv->capturing)
    {
      _err("ERROR: VICAP cannot reconfigure while a stream is running\n");
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  /* Stop the path and drop anything it may have latched, so that nothing is
   * still writing to the buffers that are about to go away.  Idempotent, and
   * cheap next to what follows.
   */

  rk3576_vicap_input_disable(priv);

  /* Obtain the new buffers before releasing the old ones.
   *
   * A frame that arrived despite the stop above would otherwise be able to
   * write into a block that the heap had already taken back and handed to the
   * next caller; in this order the worst it can reach is memory that is still
   * VICAP's own and is about to be released anyway.
   *
   * The old buffers are kept here rather than read from the driver afterwards
   * for a second reason: they have to be released with the size they were
   * taken at, and the allocation is about to replace that size along with
   * everything else.
   */

  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      oldraw[i] = priv->raw[i];
    }

  oldsnap = priv->snap;
  olddmaalloc = priv->dmaalloc;

  ret = rk3576_vicap_buffers_alloc(priv, config);
  if (ret < 0)
    {
      /* Nothing was published and nothing was released, so the driver is
       * still on the mode it had, buffers and all.  A reconfiguration that
       * could not be afforded therefore does not cost the caller the stream
       * it already had.
       */

      _err("ERROR: VICAP could not reconfigure to %ux%u: %d\n", config->width,
           config->height, ret);
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  for (i = 0; i < RK3576_VICAP_NBUF; i++)
    {
      if (oldraw[i] != NULL)
        {
          rk3576_dma_free(oldraw[i], olddmaalloc);
        }
    }

  if (oldsnap != NULL)
    {
      kmm_free(oldsnap);
    }

  /* Only now, with buffers that belong to the new geometry in place, is the
   * new geometry the one the driver is on.  Everything that reads the
   * configuration per frame -- the demosaic, the white balance update, the
   * frame validation -- follows it from here on.
   */

  priv->cfg = *config;

  rk3576_vicap_mmu_configure(priv->base);
  rk3576_vicap_input_configure(priv);

  _info("VICAP: reconfigured to MIPI%u ID%u, %ux%u RAW%u %s%s, stride=%" PRIu32
        ", frame=%zu bytes\n",
        (unsigned int)config->input, (unsigned int)config->id, config->width,
        config->height, (unsigned int)config->raw_bits,
        config->uncompact ? "uncompact" : "compact",
        (config->uncompact && config->align_high) ? " high-align" : "",
        priv->stride, priv->rawlen);

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_dump
 ****************************************************************************/

void rk3576_vicap_dump(void)
{
  FAR struct rk3576_vicap_s *priv = &g_vicap;

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return;
    }

  if (!priv->initialized)
    {
      _info("VICAP: not initialised\n");
      nxmutex_unlock(&priv->lock);
      return;
    }

  rk3576_vicap_dump_locked(priv);

  nxmutex_unlock(&priv->lock);
}

#endif /* CONFIG_RK3576_VICAP */
