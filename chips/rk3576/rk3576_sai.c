/****************************************************************************
 * chips/rk3576/rk3576_sai.c
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
 * RK3576 SAI (Serial Audio Interface, "sai-v1") lower-half I2S driver.
 * Implements NuttX struct i2s_dev_s so drivers/audio/audio_i2s.c can bind it
 * to an ES8388 codec and register a PCM device.  SAI1 is the I2S bus master
 * (it sources MCLK / BCLK / LRCK); the ES8388 is a slave clocked from it.
 *
 * Audio samples move between an ap_buffer_s and the SAI FIFO through the
 * RK3576 PL330 DMA driver (rk3576_dma.c), one single-shot DMA transfer per
 * queued buffer.  Playback (TX, memory -> SAI_TXDR, M2P) is the primary
 * path; capture (RX, SAI_RXDR -> memory, P2M) mirrors it.  The structure
 * (pending / active / done buffer queues, per-buffer DMA, work-queue
 * completion callback, cached channels/rate/width applied while stopped)
 * follows arch/arm/src/stm32f7/stm32_sai.c.
 *
 * Framing (fixed for bring-up): standard I2S, 2 slots per frame, 32-bit
 * slots carrying left-justified valid data (16/24/32-bit), MCLK = 256 * fs.
 * With 32-bit slots the SAI internal mclk-divider is constant for a given
 * channel count (BCLK = fs * channels * 32, MCLK/BCLK = 256/(channels*32)),
 * which keeps the divider independent of sample rate.
 *
 * Deferred bring-up item: generating an exact audio MCLK (e.g. 12.288 MHz
 * for 48 kHz) from the CRU audio-fractional PLL is not done here.  This
 * driver only ungates the SAI1 hclk/mclk and deasserts its resets, assuming
 * the loader left a usable mclk parent running; see rk3576_sai_clockconfig()
 * for where the fractional-divider programming must be filled in once the
 * mclk parent tree is characterised on hardware.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/queue.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_sai.h"
#include "rk3576_dma.h"
#include "rk3576_sai.h"

#ifdef CONFIG_RK3576_SAI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SCHED_WORKQUEUE
#error Work queue support is required (CONFIG_SCHED_WORKQUEUE)
#endif

#ifndef CONFIG_AUDIO
#error CONFIG_AUDIO required by this driver
#endif

#ifndef CONFIG_I2S
#error CONFIG_I2S required by this driver
#endif

#ifndef CONFIG_RK3576_DMA
#error CONFIG_RK3576_DMA required by this driver
#endif

/* Maximum number of buffers that may be queued in flight. */

#ifndef CONFIG_RK3576_SAI_MAXINFLIGHT
#define CONFIG_RK3576_SAI_MAXINFLIGHT 8
#endif

/* Default framing / clocking. */

#define RK3576_SAI_DEF_SAMPLERATE 48000
#define RK3576_SAI_DEF_DATALEN    16
#define RK3576_SAI_DEF_CHANNELS   2

#define RK3576_SAI_MCLK_FS        256 /* MCLK = 256 * sample rate      */
#define RK3576_SAI_SLOT_BITS      32  /* Fixed 32-bit slots            */

/* PL330 burst length (beats) and matching SAI FIFO DMA watermarks.  The SAI
 * FIFO holds 32 entries (per the 6-bit level fields).  The PL330 driver
 * issues burst DMA (DMAWFP burst + DMALDB/DMASTB), so the SAI watermarks are
 * chosen so a full burst can always be serviced when a request is asserted:
 *   TX: request when FIFO level <= TDL; with TDL=16 free space = 32-16 = 16
 *       >= burst (8), so a burst always fits.
 *   RX: request when FIFO level >= RDL+1; with RDL=7 there are >= 8 entries,
 *       so a burst can always be drained.
 * burst_len is reduced per-transfer if the byte count is not a whole number
 * of bursts; a smaller burst is still always serviceable at these levels.
 */

#define RK3576_SAI_DMA_BURST    8
#define RK3576_SAI_TX_WATERMARK 16
#define RK3576_SAI_RX_WATERMARK 7

/* Poll bound for the self-clearing SAI_CLR logic-clear operation. */

#define RK3576_SAI_CLR_RETRIES 1000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One queued audio buffer (a container that links an ap_buffer_s with its
 * completion callback, threaded onto the pend/act/done singly-linked lists).
 */

struct rk3576_sai_buffer_s
{
  struct rk3576_sai_buffer_s *flink; /* Singly linked list support         */
  i2s_callback_t callback;           /* Client completion callback        */
  uint32_t timeout;                  /* Transfer timeout (ticks)          */
  void *arg;                         /* Client callback argument          */
  struct ap_buffer_s *apb;           /* The audio buffer                  */
  int result;                        /* Transfer result                   */
};

/* State for one SAI controller instance. */

struct rk3576_sai_s
{
  struct i2s_dev_s dev;       /* Externally visible I2S interface      */
  uintptr_t base;             /* Controller register base              */
  int irq;                    /* SAI interrupt number                  */
  unsigned int dma_tx_req;    /* dmac0 TX peripheral-request line       */
  unsigned int dma_rx_req;    /* dmac0 RX peripheral-request line       */
  struct rk3576_dmach_s *dma; /* Active DMA channel (NULL when idle)   */
  mutex_t lock;               /* Exclusive access to the SAI           */
  uint32_t mclk_freq;         /* Current/target master clock (Hz)      */
  uint32_t samplerate;        /* Sample rate (Hz)                      */
  uint8_t datalen;            /* Valid data width (bits)               */
  uint8_t channels;           /* Channels (slots per frame)            */
  bool txenab;                /* True: current run is TX               */
  bool rxenab;                /* True: current run is RX               */
  bool running;               /* True: controller armed for a run      */
  struct wdog_s dog;          /* Transfer timeout watchdog             */
  sq_queue_t pend;            /* Queue of pending transfers            */
  sq_queue_t act;             /* Queue of active transfers             */
  sq_queue_t done;            /* Queue of completed transfers          */
  struct work_s work;         /* Completion work-queue item            */
  sem_t bufsem;               /* Buffer-container counting semaphore   */
  struct rk3576_sai_buffer_s *freelist;
  struct rk3576_sai_buffer_s containers[CONFIG_RK3576_SAI_MAXINFLIGHT];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register helpers */

static uint32_t rk3576_sai_getreg(struct rk3576_sai_s *priv,
                                  unsigned int offset);
static void rk3576_sai_putreg(struct rk3576_sai_s *priv, unsigned int offset,
                              uint32_t value);
static void rk3576_sai_modifyreg(struct rk3576_sai_s *priv,
                                 unsigned int offset, uint32_t clrbits,
                                 uint32_t setbits);
static void rk3576_sai_clearlogic(struct rk3576_sai_s *priv, uint32_t clrbit);

/* Buffer container management */

static struct rk3576_sai_buffer_s *
rk3576_sai_bufallocate(struct rk3576_sai_s *priv);
static void rk3576_sai_buffree(struct rk3576_sai_s *priv,
                               struct rk3576_sai_buffer_s *bfc);
static void rk3576_sai_bufinit(struct rk3576_sai_s *priv);

/* Clock / hardware configuration */

static void rk3576_sai_clockconfig(struct rk3576_sai_s *priv);
static uint32_t rk3576_sai_mclkdivider(struct rk3576_sai_s *priv);
static int rk3576_sai_startup(struct rk3576_sai_s *priv);
static void rk3576_sai_stop(struct rk3576_sai_s *priv);

/* Transfer path */

static void rk3576_sai_dmasetup(struct rk3576_sai_s *priv);
static void rk3576_sai_drain(struct rk3576_sai_s *priv, int result);
static void rk3576_sai_dmacallback(struct rk3576_dmach_s *ch, int result,
                                   void *arg);
static void rk3576_sai_schedule(struct rk3576_sai_s *priv, int result);
static void rk3576_sai_worker(void *arg);
static void rk3576_sai_timeout(wdparm_t arg);
static int rk3576_sai_interrupt(int irq, void *context, void *arg);

/* I2S methods */

static int rk3576_sai_txchannels(struct i2s_dev_s *dev, uint8_t channels);
static uint32_t rk3576_sai_txsamplerate(struct i2s_dev_s *dev, uint32_t rate);
static uint32_t rk3576_sai_txdatawidth(struct i2s_dev_s *dev, int bits);
static int rk3576_sai_send(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                           i2s_callback_t callback, void *arg,
                           uint32_t timeout);
static int rk3576_sai_rxchannels(struct i2s_dev_s *dev, uint8_t channels);
static uint32_t rk3576_sai_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate);
static uint32_t rk3576_sai_rxdatawidth(struct i2s_dev_s *dev, int bits);
static int rk3576_sai_receive(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                              i2s_callback_t callback, void *arg,
                              uint32_t timeout);
static uint32_t rk3576_sai_getmclk(struct i2s_dev_s *dev);
static uint32_t rk3576_sai_setmclk(struct i2s_dev_s *dev, uint32_t frequency);
static int rk3576_sai_ioctl(struct i2s_dev_s *dev, int cmd, unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2s_ops_s g_rk3576_sai_ops = {
  /* Receiver methods */

  .i2s_rxchannels = rk3576_sai_rxchannels,
  .i2s_rxsamplerate = rk3576_sai_rxsamplerate,
  .i2s_rxdatawidth = rk3576_sai_rxdatawidth,
  .i2s_receive = rk3576_sai_receive,

  /* Transmitter methods */

  .i2s_txchannels = rk3576_sai_txchannels,
  .i2s_txsamplerate = rk3576_sai_txsamplerate,
  .i2s_txdatawidth = rk3576_sai_txdatawidth,
  .i2s_send = rk3576_sai_send,

  /* Master-clock methods */

  .i2s_getmclkfrequency = rk3576_sai_getmclk,
  .i2s_setmclkfrequency = rk3576_sai_setmclk,

  /* Ioctl */

  .i2s_ioctl = rk3576_sai_ioctl,
};

/* SAI1 instance state. */

static struct rk3576_sai_s g_rk3576_sai1 = {
  .dev.ops = &g_rk3576_sai_ops,
  .base = RK3576_SAI1_BASE,
  .irq = RK3576_IRQ_SAI1,
  .dma_tx_req = RK3576_SAI1_DMA_TX_REQ,
  .dma_rx_req = RK3576_SAI1_DMA_RX_REQ,
  .lock = NXMUTEX_INITIALIZER,
  .samplerate = RK3576_SAI_DEF_SAMPLERATE,
  .datalen = RK3576_SAI_DEF_DATALEN,
  .channels = RK3576_SAI_DEF_CHANNELS,
  .mclk_freq = RK3576_SAI_DEF_SAMPLERATE * RK3576_SAI_MCLK_FS,
  .bufsem = SEM_INITIALIZER(CONFIG_RK3576_SAI_MAXINFLIGHT),
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_sai_getreg / rk3576_sai_putreg / rk3576_sai_modifyreg
 ****************************************************************************/

static uint32_t rk3576_sai_getreg(struct rk3576_sai_s *priv,
                                  unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static void rk3576_sai_putreg(struct rk3576_sai_s *priv, unsigned int offset,
                              uint32_t value)
{
  putreg32(value, priv->base + offset);
}

static void rk3576_sai_modifyreg(struct rk3576_sai_s *priv,
                                 unsigned int offset, uint32_t clrbits,
                                 uint32_t setbits)
{
  uint32_t regval = rk3576_sai_getreg(priv, offset);

  regval &= ~clrbits;
  regval |= setbits;
  rk3576_sai_putreg(priv, offset, regval);
}

/****************************************************************************
 * Name: rk3576_sai_clearlogic
 *
 * Description:
 *   Reset the TX or RX sclk-domain logic through the self-clearing SAI_CLR
 *   register.  Per the TRM the clear bit must be written 1 then polled until
 *   it reads back 0 before any other register is (re)configured.
 *
 ****************************************************************************/

static void rk3576_sai_clearlogic(struct rk3576_sai_s *priv, uint32_t clrbit)
{
  int retries = RK3576_SAI_CLR_RETRIES;

  rk3576_sai_putreg(priv, RK3576_SAI_CLR, clrbit);

  while ((rk3576_sai_getreg(priv, RK3576_SAI_CLR) & clrbit) != 0 &&
         --retries > 0)
    {
      up_udelay(1);
    }

  if (retries == 0)
    {
      i2serr("ERROR: SAI_CLR bit 0x%08" PRIx32 " did not self-clear\n",
             clrbit);
    }
}

/****************************************************************************
 * Name: rk3576_sai_bufallocate
 *
 * Description:
 *   Take a buffer container off the head of the free list, blocking on the
 *   counting semaphore until one is available.  Must NOT be called with the
 *   SAI lock held.
 *
 ****************************************************************************/

static struct rk3576_sai_buffer_s *
rk3576_sai_bufallocate(struct rk3576_sai_s *priv)
{
  struct rk3576_sai_buffer_s *bfc;
  irqstate_t flags;

  nxsem_wait_uninterruptible(&priv->bufsem);

  flags = enter_critical_section();
  bfc = priv->freelist;
  DEBUGASSERT(bfc != NULL);
  priv->freelist = bfc->flink;
  leave_critical_section(flags);

  return bfc;
}

/****************************************************************************
 * Name: rk3576_sai_buffree
 ****************************************************************************/

static void rk3576_sai_buffree(struct rk3576_sai_s *priv,
                               struct rk3576_sai_buffer_s *bfc)
{
  irqstate_t flags;

  flags = enter_critical_section();
  bfc->flink = priv->freelist;
  priv->freelist = bfc;
  leave_critical_section(flags);

  nxsem_post(&priv->bufsem);
}

/****************************************************************************
 * Name: rk3576_sai_bufinit
 ****************************************************************************/

static void rk3576_sai_bufinit(struct rk3576_sai_s *priv)
{
  int i;

  priv->freelist = NULL;
  for (i = 0; i < CONFIG_RK3576_SAI_MAXINFLIGHT; i++)
    {
      rk3576_sai_buffree(priv, &priv->containers[i]);
    }
}

/****************************************************************************
 * Name: rk3576_sai_clockconfig
 *
 * Description:
 *   Program the CRU so mclk_sai1 = 256 * fs (12.288 MHz for 48 kHz), then
 *   ungate the SAI1 hclk + mclk and deassert its module/bus resets so the
 *   register block is accessible and the serializer can run.  All writes are
 *   hiword-masked (upper 16 bits are the per-bit write-enable) like the rest
 *   of rk3576_cru.c.
 *
 *   Clock path: AUPLL (393.216 MHz reset rate) -> clk_matrix_audio_frac_0
 *   (fractional /N) -> mclk_sai1_src (/1) -> mclk_sai1.  Because AUPLL is an
 *   exact integer multiple of the target MCLK, the fractional divider is
 *   programmed as num=1 / den=N, which produces a clean integer /N divide
 *   (48 kHz: 393.216 MHz / 32 = 12.288 MHz).  The SAI_CKR mdiv then derives
 *   SCLK = MCLK / mdiv (see rk3576_sai_mclkdivider()).
 *
 *   Only SAI1 uses audio_frac_0 during bring-up; route each of SAI2..9 to a
 *   different audio_frac / audio_int source here if they are brought up.
 *
 ****************************************************************************/

static void rk3576_sai_clockconfig(struct rk3576_sai_s *priv)
{
  UNUSED(priv);

  /* Clock tree matches the on-board Debian golden values for SAI1 @ 48 kHz:
   *   AUPLL(786.432M) -> clk_audio_frac_1 (/16 = 49.152M, 48k family)
   *                   -> mclk_sai1_src (/4 = 12.288M) -> mclk_sai1.
   * NOTE: clk_audio_frac_0 is the 44.1k-family fraction (GPLL-sourced on this
   * board); using it for 48 kHz leaves the serializer without a usable clock
   * (TX data counter stuck at 0).  clk_audio_frac_1 is the AUPLL-sourced
   * 48k-family fraction and is what the vendor kernel routes to SAI1.
   */

  /* 0. Ungate clk_matrix_audio_frac_1 (CRU_GATE_CON01 bit11, gate high =
   *    disabled).  Debian leaves frac_1 enabled and frac_0 gated off.
   */

  putreg32((1u << 11) << 16, RK3576_CRU_GATE_CON(1));

  /* 1. Point clk_matrix_audio_frac_1 at AUPLL (CRU_CLKSEL_CON15[1:0] = 2). */

  putreg32((0x3u << 16) | 0x2u, RK3576_CRU_CLKSEL_CON(15));

  /* 2. Fraction: fout = AUPLL * num/den = 786.432M * 1/16 = 49.152 MHz.
   *    CRU_CLKSEL_CON14 = numerator[31:16] | denominator[15:0], no write
   *    mask (the whole 32-bit register is the fraction).  Golden 0x00010010.
   */

  putreg32(0x00010010u, RK3576_CRU_CLKSEL_CON(14));

  /* 3. mclk_sai1_src <- clk_audio_frac_1, /4 -> 12.288 MHz; mclk_sai1_sel
   *    uses mclk_sai1_src.  CRU_CLKSEL_CON46 golden value 0x0203
   *    (src_sel=0b010=frac_1 at bit9, src_div=3 => /4).
   */

  putreg32((0x0fffu << 16) | 0x0203u, RK3576_CRU_CLKSEL_CON(46));

  /* 4. Ungate mclk_sai1_src + mclk_sai1 + hclk_sai1 (gate bit high =
   *    disabled, so write 0 into the masked bits to enable them).
   */

  putreg32(((RK3576_CRU_SAI1_MCLK_SRC_BIT | RK3576_CRU_SAI1_MCLK_BIT |
             RK3576_CRU_SAI1_HCLK_BIT)
            << 16),
           RK3576_CRU_GATE_CON(RK3576_CRU_SAI1_GATE));

  /* 5. Pulse mresetn_sai1 + hresetn_sai1: assert -> delay -> deassert. */

  putreg32(((RK3576_CRU_SAI1_MRST_BIT | RK3576_CRU_SAI1_HRST_BIT) << 16) |
               (RK3576_CRU_SAI1_MRST_BIT | RK3576_CRU_SAI1_HRST_BIT),
           RK3576_CRU_SOFTRST_CON(RK3576_CRU_SAI1_SOFTRST));
  up_udelay(20);

  putreg32(((RK3576_CRU_SAI1_MRST_BIT | RK3576_CRU_SAI1_HRST_BIT) << 16),
           RK3576_CRU_SOFTRST_CON(RK3576_CRU_SAI1_SOFTRST));
  up_udelay(20);

  /* 6. Route MCLK to the physical IO pin for the codec.  DTS mclkout-sai1
   *    "rockchip,clk-out" GRF register @0x26046400, bit-shift 1,
   *    bit-set-to-disable: clear bit1 (hiword-masked) to enable the
   *    mclk_sai1_to_io output.  Without this the ES8388 never receives MCLK
   *    and stays silent even though SCLK/LRCK/SDO carry valid I2S.  This GRF
   *    register is hiword-masked (verified on hardware: a plain RMW write
   *    changes nothing and it reads back 0x1f); write the bit1 mask with a
   *    zero value to clear it (Debian golden readback: 0x1d).
   */

  putreg32((1u << 1) << 16, 0x26046400);
}

/****************************************************************************
 * Name: rk3576_sai_mclkdivider
 *
 * Description:
 *   Return the SAI internal mclk divider N (SCLK = MCLK / N) for the current
 *   channel count, with fixed 32-bit slots and MCLK = 256 * fs.  BCLK =
 *   fs * channels * 32, so N = MCLK / BCLK = 256 / (channels * 32).
 *
 ****************************************************************************/

static uint32_t rk3576_sai_mclkdivider(struct rk3576_sai_s *priv)
{
  uint32_t bits = (uint32_t)priv->channels * RK3576_SAI_SLOT_BITS;
  uint32_t div = RK3576_SAI_MCLK_FS / bits;

  if (div < 1)
    {
      div = 1;
    }

  return div;
}

/****************************************************************************
 * Name: rk3576_sai_startup
 *
 * Description:
 *   Configure the controller for a fresh TX or RX run while it is stopped,
 *   following the TRM 24.6.1 transmit-operation flow:
 *     disable XFER -> clear logic (poll) -> write xCR/FSCR/CKR -> write
 *     DMACR -> (caller then arms DMA and sets XFER).
 *   Acquires a DMA channel for the active direction and enables the FIFO
 *   under/overrun interrupt.  priv->txenab / priv->rxenab must already be
 *   set by the caller.
 *
 * Assumptions:
 *   Interrupts are disabled (called from rk3576_sai_dmasetup).
 *
 ****************************************************************************/

static int rk3576_sai_startup(struct rk3576_sai_s *priv)
{
  uint32_t xcr;
  uint32_t mdiv;

  /* Ensure the transfer engine is stopped and its logic is cleared before
   * touching the (XFER-guarded) configuration fields.
   */

  rk3576_sai_putreg(priv, RK3576_SAI_XFER, 0);
  rk3576_sai_clearlogic(priv, priv->txenab ? SAI_CLR_TXC : SAI_CLR_RXC);

  /* Operation-control register (identical TX/RX layout): one data lane,
   * MSB first, 'channels' slots per frame, 32-bit slots holding left-
   * justified valid data of priv->datalen bits.
   */

  xcr = SAI_XCR_CSR(1) | SAI_XCR_SNB(priv->channels) | SAI_XCR_VDJ |
        SAI_XCR_SBW(RK3576_SAI_SLOT_BITS) | SAI_XCR_VDW(priv->datalen);

  mdiv = rk3576_sai_mclkdivider(priv);

  if (priv->txenab)
    {
      /* TEMP: use the on-board Debian golden register values for a 48 kHz /
       * 16-bit / 2-channel I2S TX (matches the test stream), to prove the
       * data path end to end before deriving them from priv fields again.
       */

      UNUSED(xcr);
      UNUSED(mdiv);
      rk3576_sai_putreg(priv, RK3576_SAI_TXCR, 0x00400fef);
      rk3576_sai_putreg(priv, RK3576_SAI_FSCR, 0x0101f03f);
      rk3576_sai_putreg(priv, RK3576_SAI_CKR, 0x00000018);
      rk3576_sai_putreg(priv, RK3576_SAI_MONO_CR, 0);

      /* Path select routing (Debian golden). */

      rk3576_sai_putreg(priv, RK3576_SAI_PATH_SEL, 0x0000e4e4);

      /* TX DMA control (Debian golden 0x000F0110: TDE + level fields). */

      rk3576_sai_putreg(priv, RK3576_SAI_DMACR, 0x000f0110);

      /* Enable the TX-underrun interrupt for diagnostics. */

      rk3576_sai_putreg(priv, RK3576_SAI_INTCR, SAI_INTCR_TXUIE);
    }
  else
    {
      rk3576_sai_putreg(priv, RK3576_SAI_RXCR, xcr);
      rk3576_sai_putreg(priv, RK3576_SAI_FSCR,
                        SAI_FSCR_FW(priv->channels * RK3576_SAI_SLOT_BITS) |
                            SAI_FSCR_FPW(RK3576_SAI_SLOT_BITS));
      rk3576_sai_putreg(priv, RK3576_SAI_CKR, SAI_CKR_MDIV(mdiv));
      rk3576_sai_putreg(priv, RK3576_SAI_MONO_CR,
                        priv->channels == 1 ? SAI_MONO_CR_RX_MONO_EN : 0);

      /* RX DMA request when FIFO level >= watermark + 1. */

      rk3576_sai_putreg(priv, RK3576_SAI_DMACR,
                        SAI_DMACR_RDE |
                            SAI_DMACR_RDL(RK3576_SAI_RX_WATERMARK));

      rk3576_sai_putreg(priv, RK3576_SAI_INTCR, SAI_INTCR_RXOIE);
    }

  /* Acquire a DMA channel bound to the active direction's request line. */

  priv->dma = rk3576_dma_get_channel(priv->txenab ? priv->dma_tx_req
                                                  : priv->dma_rx_req);
  if (priv->dma == NULL)
    {
      i2serr("ERROR: no free DMA channel\n");
      rk3576_sai_putreg(priv, RK3576_SAI_INTCR, 0);
      return -EBUSY;
    }

  priv->running = true;
  return OK;
}

/****************************************************************************
 * Name: rk3576_sai_stop
 *
 * Description:
 *   Tear down after a run has fully drained: stop the transfer engine,
 *   disable interrupts and release the DMA channel.  Only called when both
 *   the active and pending queues are empty, so no transfer is in flight
 *   (the TRM forbids clearing XFER mid-transfer).
 *
 ****************************************************************************/

static void rk3576_sai_stop(struct rk3576_sai_s *priv)
{
  if (!priv->running)
    {
      return;
    }

  rk3576_sai_putreg(priv, RK3576_SAI_XFER, 0);
  rk3576_sai_putreg(priv, RK3576_SAI_INTCR, 0);

  if (priv->dma != NULL)
    {
      rk3576_dma_free_channel(priv->dma);
      priv->dma = NULL;
    }

  priv->running = false;
  priv->txenab = false;
  priv->rxenab = false;
}

/****************************************************************************
 * Name: rk3576_sai_dmasetup
 *
 * Description:
 *   Start the next pending buffer's DMA transfer if the controller is idle.
 *   Pops one buffer from the pending queue, programs a single-shot PL330
 *   transfer to/from the SAI FIFO, moves the buffer to the active queue,
 *   launches the DMA, enables the transfer engine and arms the timeout.
 *
 * Assumptions:
 *   Interrupts are disabled.
 *
 ****************************************************************************/

static void rk3576_sai_dmasetup(struct rk3576_sai_s *priv)
{
  struct rk3576_sai_buffer_s *bfc;
  struct ap_buffer_s *apb;
  struct rk3576_dma_config_s cfg;
  uintptr_t samp;
  size_t nbytes;
  uint8_t width;
  uint8_t burst;
  int ret;

  /* A transfer is already active: nothing to launch now. */

  if (!sq_empty(&priv->act))
    {
      return;
    }

  /* No pending work: quiesce the controller. */

  if (sq_empty(&priv->pend))
    {
      rk3576_sai_stop(priv);
      return;
    }

  /* Arm the controller for the first buffer of a fresh run. */

  if (!priv->running)
    {
      ret = rk3576_sai_startup(priv);
      if (ret < 0)
        {
          /* Cannot start this run: fail every queued buffer. */

          priv->txenab = false;
          priv->rxenab = false;
          rk3576_sai_drain(priv, ret);
          return;
        }
    }

  bfc = (struct rk3576_sai_buffer_s *)sq_remfirst(&priv->pend);
  DEBUGASSERT(bfc != NULL && bfc->apb != NULL);
  apb = bfc->apb;

  samp = (uintptr_t)&apb->samp[apb->curbyte];

  /* The SAI TX/RX FIFO data register is 32 bits wide and packs the audio
   * samples (two 16-bit samples per 32-bit FIFO word); a narrower DMA beat
   * does not advance the FIFO correctly (faint/distorted output).  Always
   * move the FIFO 32 bits at a time.
   */

  width = 4;

  memset(&cfg, 0, sizeof(cfg));
  cfg.src_width = width;
  cfg.dst_width = width;
  cfg.callback = rk3576_sai_dmacallback;
  cfg.arg = priv;

  if (priv->txenab)
    {
      nbytes = apb->nbytes - apb->curbyte;
      cfg.direction = RK3576_DMA_M2P;
      cfg.src = samp;
      cfg.dst = priv->base + RK3576_SAI_TXDR;

      /* The apb sample buffer was filled by the CPU (cached); flush it to
       * memory so the (non-coherent) PL330 reads the real samples instead of
       * stale memory.
       */

      up_clean_dcache(samp, samp + nbytes);
    }
  else
    {
      nbytes = apb->nmaxbytes - apb->curbyte;
      cfg.direction = RK3576_DMA_P2M;
      cfg.src = priv->base + RK3576_SAI_RXDR;
      cfg.dst = samp;

      /* Invalidate so the CPU sees what the DMA writes, not stale cache. */

      up_invalidate_dcache(samp, samp + nbytes);
    }

  /* PL330 requires nbytes to be a whole number of bursts; use the largest
   * burst up to RK3576_SAI_DMA_BURST that divides evenly.  A smaller burst
   * is still always serviceable at the configured FIFO watermarks.
   */

  burst = RK3576_SAI_DMA_BURST;
  while (burst > 1 && (nbytes % ((size_t)width * burst)) != 0)
    {
      burst >>= 1;
    }

  cfg.burst_len = burst;
  cfg.nbytes = nbytes;

  ret = rk3576_dma_setup(priv->dma, &cfg);
  if (ret < 0)
    {
      i2serr("ERROR: rk3576_dma_setup failed: %d\n", ret);
      bfc->result = ret;
      sq_addlast((sq_entry_t *)bfc, &priv->done);
      rk3576_sai_stop(priv);
      rk3576_sai_drain(priv, ret);
      return;
    }

  sq_addlast((sq_entry_t *)bfc, &priv->act);

  rk3576_dma_start(priv->dma);

  /* Enable the transfer engine: start clk + frame-sync + the active
   * direction.  Idempotent across consecutive buffers of the same run.
   */

  rk3576_sai_modifyreg(priv, RK3576_SAI_XFER, 0,
                       SAI_XFER_CLK | SAI_XFER_FSS |
                           (priv->txenab ? SAI_XFER_TXS : SAI_XFER_RXS));

  if (bfc->timeout > 0)
    {
      ret = wd_start(&priv->dog, bfc->timeout, rk3576_sai_timeout,
                     (wdparm_t)priv);
      if (ret < 0)
        {
          i2serr("ERROR: wd_start failed: %d\n", ret);
        }
    }
}

/****************************************************************************
 * Name: rk3576_sai_drain
 *
 * Description:
 *   Move every still-pending buffer to the done queue with the given result
 *   and schedule the completion worker.  Used when a run cannot be started.
 *
 * Assumptions:
 *   Interrupts are disabled.
 *
 ****************************************************************************/

static void rk3576_sai_drain(struct rk3576_sai_s *priv, int result)
{
  struct rk3576_sai_buffer_s *bfc;
  int ret;

  while (!sq_empty(&priv->pend))
    {
      bfc = (struct rk3576_sai_buffer_s *)sq_remfirst(&priv->pend);
      bfc->result = result;
      sq_addlast((sq_entry_t *)bfc, &priv->done);
    }

  if (work_available(&priv->work))
    {
      ret = work_queue(HPWORK, &priv->work, rk3576_sai_worker, priv, 0);
      if (ret != 0)
        {
          i2serr("ERROR: failed to queue work: %d\n", ret);
        }
    }
}

/****************************************************************************
 * Name: rk3576_sai_dmacallback
 *
 * Description:
 *   PL330 completion callback (interrupt context).  Cancels the timeout and
 *   schedules completion processing on the work queue.
 *
 ****************************************************************************/

static void rk3576_sai_dmacallback(struct rk3576_dmach_s *ch, int result,
                                   void *arg)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)arg;

  UNUSED(ch);
  DEBUGASSERT(priv != NULL);

  wd_cancel(&priv->dog);
  rk3576_sai_schedule(priv, result);
}

/****************************************************************************
 * Name: rk3576_sai_timeout
 *
 * Description:
 *   Watchdog handler: a transfer failed to complete in time.  Aborts the
 *   DMA and schedules the buffer's completion with -ETIMEDOUT.
 *
 ****************************************************************************/

static void rk3576_sai_timeout(wdparm_t arg)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)arg;

  DEBUGASSERT(priv != NULL);

  if (priv->dma != NULL)
    {
      rk3576_dma_stop(priv->dma);
    }

  rk3576_sai_schedule(priv, -ETIMEDOUT);
}

/****************************************************************************
 * Name: rk3576_sai_schedule
 *
 * Description:
 *   Move all active buffers to the done queue with the given result and
 *   queue the completion worker.
 *
 * Assumptions:
 *   Interrupts are disabled and any timeout has been cancelled.
 *
 ****************************************************************************/

static void rk3576_sai_schedule(struct rk3576_sai_s *priv, int result)
{
  struct rk3576_sai_buffer_s *bfc;
  int ret;

  while (!sq_empty(&priv->act))
    {
      bfc = (struct rk3576_sai_buffer_s *)sq_remfirst(&priv->act);
      bfc->result = result;
      sq_addlast((sq_entry_t *)bfc, &priv->done);
    }

  if (work_available(&priv->work))
    {
      ret = work_queue(HPWORK, &priv->work, rk3576_sai_worker, priv, 0);
      if (ret != 0)
        {
          i2serr("ERROR: failed to queue work: %d\n", ret);
        }
    }
}

/****************************************************************************
 * Name: rk3576_sai_worker
 *
 * Description:
 *   Completion worker: start the next pending transfer (if idle) and invoke
 *   the client callbacks for every completed buffer.
 *
 ****************************************************************************/

static void rk3576_sai_worker(void *arg)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)arg;
  struct rk3576_sai_buffer_s *bfc;
  irqstate_t flags;

  DEBUGASSERT(priv != NULL);

  /* Kick the next pending transfer while the engine is idle. */

  if (sq_empty(&priv->act))
    {
      flags = enter_critical_section();
      rk3576_sai_dmasetup(priv);
      leave_critical_section(flags);
    }

  /* Deliver completions. */

  while (sq_peek(&priv->done) != NULL)
    {
      flags = enter_critical_section();
      bfc = (struct rk3576_sai_buffer_s *)sq_remfirst(&priv->done);
      leave_critical_section(flags);

      DEBUGASSERT(bfc != NULL && bfc->callback != NULL);
      bfc->callback(&priv->dev, bfc->apb, bfc->arg, bfc->result);

      apb_free(bfc->apb);
      rk3576_sai_buffree(priv, bfc);
    }
}

/****************************************************************************
 * Name: rk3576_sai_interrupt
 *
 * Description:
 *   SAI interrupt handler.  Reports and clears FIFO under/overrun errors;
 *   normal completion is delivered through the DMA callback, so this handler
 *   is purely diagnostic.
 *
 ****************************************************************************/

static int rk3576_sai_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)arg;
  uint32_t sr;

  UNUSED(irq);
  UNUSED(context);

  sr = rk3576_sai_getreg(priv, RK3576_SAI_INTSR);

  if ((sr & SAI_INTSR_TXUI) != 0)
    {
      i2serr("ERROR: SAI TX underrun\n");
      rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR, 0, SAI_INTCR_TXUIC);
    }

  if ((sr & SAI_INTSR_RXOI) != 0)
    {
      i2serr("ERROR: SAI RX overrun\n");
      rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR, 0, SAI_INTCR_RXOIC);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_sai_txchannels / rk3576_sai_rxchannels
 ****************************************************************************/

static int rk3576_sai_txchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;

  if (channels < 1 || channels > 2)
    {
      return -EINVAL;
    }

  /* Cached; applied in rk3576_sai_startup() while the engine is stopped. */

  priv->channels = channels;
  return OK;
}

static int rk3576_sai_rxchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  return rk3576_sai_txchannels(dev, channels);
}

/****************************************************************************
 * Name: rk3576_sai_txsamplerate / rk3576_sai_rxsamplerate
 ****************************************************************************/

static uint32_t rk3576_sai_txsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;

  DEBUGASSERT(rate > 0);

  priv->samplerate = rate;

  /* Target MCLK follows the sample rate; the exact CRU programming to
   * realise it is a deferred bring-up item (see rk3576_sai_clockconfig()).
   */

  priv->mclk_freq = rate * RK3576_SAI_MCLK_FS;
  return rate * priv->datalen;
}

static uint32_t rk3576_sai_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  return rk3576_sai_txsamplerate(dev, rate);
}

/****************************************************************************
 * Name: rk3576_sai_txdatawidth / rk3576_sai_rxdatawidth
 ****************************************************************************/

static uint32_t rk3576_sai_txdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;

  if (bits != 16 && bits != 24 && bits != 32)
    {
      i2serr("ERROR: unsupported data width: %d\n", bits);
      return 0;
    }

  priv->datalen = bits;
  return priv->samplerate * bits;
}

static uint32_t rk3576_sai_rxdatawidth(struct i2s_dev_s *dev, int bits)
{
  return rk3576_sai_txdatawidth(dev, bits);
}

/****************************************************************************
 * Name: rk3576_sai_send
 *
 * Description:
 *   Enqueue an audio buffer for playback and start the transfer if idle.
 *   Returns after enqueuing; the completion callback fires from the work
 *   queue when the DMA finishes.
 *
 ****************************************************************************/

static int rk3576_sai_send(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                           i2s_callback_t callback, void *arg,
                           uint32_t timeout)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;
  struct rk3576_sai_buffer_s *bfc;
  irqstate_t flags;
  int ret;

  DEBUGASSERT(priv != NULL && apb != NULL);

  bfc = rk3576_sai_bufallocate(priv);
  DEBUGASSERT(bfc != NULL);

  nxmutex_lock(&priv->lock);

  /* A run may only be TX or RX at a time. */

  if (priv->running && priv->rxenab)
    {
      i2serr("ERROR: SAI busy receiving\n");
      ret = -EAGAIN;
      goto errout;
    }

  priv->txenab = true;

  apb_reference(apb);

  bfc->callback = callback;
  bfc->timeout = timeout;
  bfc->arg = arg;
  bfc->apb = apb;
  bfc->result = -EBUSY;

  flags = enter_critical_section();
  sq_addlast((sq_entry_t *)bfc, &priv->pend);
  rk3576_sai_dmasetup(priv);
  leave_critical_section(flags);

  nxmutex_unlock(&priv->lock);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  rk3576_sai_buffree(priv, bfc);
  return ret;
}

/****************************************************************************
 * Name: rk3576_sai_receive
 *
 * Description:
 *   Enqueue an audio buffer for capture and start the transfer if idle.
 *
 ****************************************************************************/

static int rk3576_sai_receive(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                              i2s_callback_t callback, void *arg,
                              uint32_t timeout)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;
  struct rk3576_sai_buffer_s *bfc;
  irqstate_t flags;
  int ret;

  DEBUGASSERT(priv != NULL && apb != NULL);

  bfc = rk3576_sai_bufallocate(priv);
  DEBUGASSERT(bfc != NULL);

  nxmutex_lock(&priv->lock);

  if (priv->running && priv->txenab)
    {
      i2serr("ERROR: SAI busy transmitting\n");
      ret = -EAGAIN;
      goto errout;
    }

  priv->rxenab = true;

  /* Report the full buffer as filled on success (single-shot capture). */

  apb->nbytes = apb->nmaxbytes;

  apb_reference(apb);

  bfc->callback = callback;
  bfc->timeout = timeout;
  bfc->arg = arg;
  bfc->apb = apb;
  bfc->result = -EBUSY;

  flags = enter_critical_section();
  sq_addlast((sq_entry_t *)bfc, &priv->pend);
  rk3576_sai_dmasetup(priv);
  leave_critical_section(flags);

  nxmutex_unlock(&priv->lock);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  rk3576_sai_buffree(priv, bfc);
  return ret;
}

/****************************************************************************
 * Name: rk3576_sai_getmclk / rk3576_sai_setmclk
 ****************************************************************************/

static uint32_t rk3576_sai_getmclk(struct i2s_dev_s *dev)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;

  return priv->mclk_freq;
}

static uint32_t rk3576_sai_setmclk(struct i2s_dev_s *dev, uint32_t frequency)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;

  /* Record the requested MCLK.  Realising an arbitrary MCLK from the CRU
   * audio PLL is a deferred bring-up item (see rk3576_sai_clockconfig());
   * for now the value is cached and reported back unchanged.
   */

  priv->mclk_freq = frequency;
  return frequency;
}

/****************************************************************************
 * Name: rk3576_sai_ioctl
 ****************************************************************************/

static int rk3576_sai_ioctl(struct i2s_dev_s *dev, int cmd, unsigned long arg)
{
  UNUSED(dev);
  UNUSED(cmd);
  UNUSED(arg);
  return -ENOTTY;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_sai_initialize
 *
 * Description:
 *   Initialize a SAI controller and return its lower-half I2S interface.
 *   See rk3576_sai.h.
 *
 ****************************************************************************/

struct i2s_dev_s *rk3576_sai_initialize(int busno)
{
  struct rk3576_sai_s *priv;
  int ret;

  if (busno != 1)
    {
      i2serr("ERROR: unsupported SAI bus %d\n", busno);
      return NULL;
    }

  priv = &g_rk3576_sai1;

  /* Buffer pool + controller clocks. */

  rk3576_sai_bufinit(priv);
  rk3576_sai_clockconfig(priv);

  /* Leave the transfer engine stopped and interrupts masked until a run
   * begins.
   */

  rk3576_sai_putreg(priv, RK3576_SAI_XFER, 0);
  rk3576_sai_putreg(priv, RK3576_SAI_INTCR, 0);

  ret = irq_attach(priv->irq, rk3576_sai_interrupt, priv);
  if (ret < 0)
    {
      i2serr("ERROR: irq_attach failed: %d\n", ret);
      return NULL;
    }

  up_enable_irq(priv->irq);

  i2sinfo("SAI%d ready: base=0x%08" PRIxPTR " version=0x%08" PRIx32 "\n",
          busno, priv->base, rk3576_sai_getreg(priv, RK3576_SAI_VERSION));

  return &priv->dev;
}

#endif /* CONFIG_RK3576_SAI */
