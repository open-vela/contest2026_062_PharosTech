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
 * to a codec and register a PCM device.  Each SAI controller is configured
 * as the I2S bus master, sourcing MCLK / BCLK / LRCK to the (slave) codec;
 * which controller is wired to which codec, plus pin muxing and routing
 * MCLK to a physical output pin, are decided by the board layer.
 *
 * Audio samples move between an ap_buffer_s and the SAI FIFO through the
 * RK3576 PL330 DMA driver (rk3576_dma.c), one single-shot DMA transfer per
 * queued buffer (playback: memory -> SAI_TXDR, M2P; capture: SAI_RXDR ->
 * memory, P2M).  The structure (pending / active / done buffer queues,
 * per-buffer DMA, work-queue completion callback, cached channels/rate/
 * width applied while stopped) follows arch/arm/src/stm32f7/stm32_sai.c.
 *
 * Framing: standard I2S with two slots per frame (the codec selects its
 * MCLK/LRCK ratio for each sample-rate family, and the SAI derives BCLK
 * from the resulting MCLK at run time).  16-bit streams use 16-bit slots;
 * 24/32-bit streams use 32-bit slots.  Controller clocks are obtained from
 * the RK3576 common clock framework.
 *
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
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/clk/clk.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/queue.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
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

/* SAI FIFO DMA watermarks.  The generic PL330 backend uses peripheral-paced
 *
 * single-beat transfers, so the thresholds leave ample headroom on both
 *
 * sides of the 32-entry FIFO.
 */

#define RK3576_SAI_TX_WATERMARK 16
#define RK3576_SAI_RX_WATERMARK 7

/* Poll bound for the self-clearing SAI_CLR logic-clear operation. */

#define RK3576_SAI_CLR_RETRIES     1000
#define RK3576_SAI_IDLE_RETRIES    2000
#define RK3576_SAI_IDLE_POLL_US    10
#define RK3576_SAI_PREFILL_RETRIES 100

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
  bool mono;                         /* Select one sample per RX frame    */
  uint8_t sample_bytes;              /* RX PCM width, zero for TX         */
  uint32_t *txwords;                 /* Expanded packed-24 TX samples     */
  size_t txbytes;                    /* Expanded DMA transfer length      */
  bool packed_tx;                    /* Packed-24 transfer, possibly empty */
};

/* State for one SAI controller instance. */

struct rk3576_sai_s
{
  struct i2s_dev_s dev;      /* Externally visible I2S interface      */
  uintptr_t base;            /* Controller register base              */
  int irq;                   /* SAI interrupt number                  */
  unsigned int dma_tx_req;   /* DMA TX peripheral-request line        */
  unsigned int dma_rx_req;   /* DMA RX peripheral-request line        */
  unsigned int dma_ctrl;     /* Owning DMA controller index (0/1/2)   */
  unsigned int softrst;      /* CRU_SOFTRST_CONn index                */
  uint32_t mrst_bit;         /* Soft mresetn_saiN mask                */
  uint32_t hrst_bit;         /* Soft hresetn_saiN mask                */
  bool tx_cap;               /* True: hardware has a TX DMA request   */
  bool rx_cap;               /* True: hardware has an RX DMA request  */
  int busno;                 /* SAI controller index                   */
  struct clk_s *hclk;        /* APB register-interface clock           */
  struct clk_s *mclk_gate;   /* Audio master-clock output gate         */
  struct dma_dev_s *dma_dev; /* PL330 generic DMA controller          */
  struct dma_chan_s *dma;    /* Active DMA channel (NULL when idle)   */
  mutex_t lock;              /* Exclusive access to the SAI           */
  uint32_t mclk_freq;        /* Current/target master clock (Hz)      */
  uint32_t samplerate;       /* Sample rate (Hz)                      */
  uint8_t datalen;           /* Valid data width (bits)               */
  uint8_t channels;          /* Channels (slots per frame)            */
  uint8_t txpartial[6];      /* Incomplete packed-24 stereo frame     */
  uint8_t ntxpartial;        /* Bytes retained across submissions     */
  bool txenab;               /* True: current run is TX               */
  bool rxenab;               /* True: current run is RX               */
  bool running;              /* True: controller armed for a run      */
  bool prepared;             /* Explicit TX lifecycle owns clocks      */
  bool initialized;          /* Controller setup completed            */
  struct rk3576_sai_s *peer; /* Opposite direction, shared registers   */
  struct wdog_s dog;         /* Transfer timeout watchdog             */
  sq_queue_t pend;           /* Queue of pending transfers            */
  sq_queue_t act;            /* Queue of active transfers             */
  sq_queue_t done;           /* Queue of completed transfers          */
  struct work_s work;        /* Completion work-queue item            */
  sem_t bufsem;              /* Buffer-container counting semaphore   */
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
static int rk3576_sai_clearlogic(struct rk3576_sai_s *priv, uint32_t clrbit);

/* Buffer container management */

static struct rk3576_sai_buffer_s *
rk3576_sai_bufallocate(struct rk3576_sai_s *priv);
static void rk3576_sai_buffree(struct rk3576_sai_s *priv,
                               struct rk3576_sai_buffer_s *bfc);
static void rk3576_sai_bufinit(struct rk3576_sai_s *priv);

/* Clock / hardware configuration */

static int rk3576_sai_clockconfig(struct rk3576_sai_s *priv);
static int rk3576_sai_setclock(struct rk3576_sai_s *priv, uint32_t frequency);
static int rk3576_sai_startup(struct rk3576_sai_s *priv);
static void rk3576_sai_stop(struct rk3576_sai_s *priv);

/* Transfer path */

static void rk3576_sai_dmasetup(struct rk3576_sai_s *priv);
static void rk3576_sai_drain(struct rk3576_sai_s *priv, int result);
static void rk3576_sai_dmacallback(struct dma_chan_s *chan, void *arg,
                                   ssize_t len);
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
static int rk3576_sai_wait_idle(struct rk3576_sai_s *priv, bool frame);
static int rk3576_sai_pack24(struct rk3576_sai_s *priv,
                             struct rk3576_sai_buffer_s *bfc,
                             struct ap_buffer_s *apb);

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

/* One SAI controller instance descriptor in g_rk3576_sai[].
 *
 * busno / base / irq / dma_ctrl / dma_tx_req / dma_rx_req / tx_cap / rx_cap
 * are fixed per-controller hardware attributes (TRM table 24.4.1 and the
 * DMAC request mapping).  rx_req is unused (set to 0) when the controller
 * has no RX request line, and vice-versa for a missing TX request; the
 * *_cap flags gate which directions rk3576_sai_send()/receive() accept.
 */

#define RK3576_SAI_INSTANCE(_busno, _base, _irq, _dma_ctrl, _tx_req, _rx_req, \
                            _tx_cap, _rx_cap, _softrst, _mrst, _hrst)         \
  {                                                                           \
    .dev.ops = &g_rk3576_sai_ops, .base = (_base), .irq = (_irq),             \
    .dma_tx_req = (_tx_req), .dma_rx_req = (_rx_req),                         \
    .dma_ctrl = (_dma_ctrl), .busno = (_busno), .tx_cap = (_tx_cap),          \
    .rx_cap = (_rx_cap), .softrst = (_softrst), .mrst_bit = (_mrst),          \
    .hrst_bit = (_hrst), .lock = NXMUTEX_INITIALIZER,                         \
    .samplerate = RK3576_SAI_DEF_SAMPLERATE,                                  \
    .datalen = RK3576_SAI_DEF_DATALEN, .channels = RK3576_SAI_DEF_CHANNELS,   \
    .mclk_freq = RK3576_SAI_DEF_SAMPLERATE * RK3576_SAI_MCLK_FS,              \
    .bufsem = SEM_INITIALIZER(CONFIG_RK3576_SAI_MAXINFLIGHT),                 \
  }

static struct rk3576_sai_s g_rk3576_sai[10] = {
  RK3576_SAI_INSTANCE(0, RK3576_SAI0_ADDR, RK3576_IRQ_SAI0, 0,
                      RK3576_SAI0_DMA_TX_REQ, RK3576_SAI0_DMA_RX_REQ, true,
                      true, RK3576_CRU_SAI0_SOFTRST, RK3576_CRU_SAI0_MRST_BIT,
                      RK3576_CRU_SAI0_HRST_BIT),
  RK3576_SAI_INSTANCE(1, RK3576_SAI1_ADDR, RK3576_IRQ_SAI1, 0,
                      RK3576_SAI1_DMA_TX_REQ, RK3576_SAI1_DMA_RX_REQ, true,
                      true, RK3576_CRU_SAI1_SOFTRST, RK3576_CRU_SAI1_MRST_BIT,
                      RK3576_CRU_SAI1_HRST_BIT),
  RK3576_SAI_INSTANCE(2, RK3576_SAI2_ADDR, RK3576_IRQ_SAI2, 1,
                      RK3576_SAI2_DMA_TX_REQ, RK3576_SAI2_DMA_RX_REQ, true,
                      true, RK3576_CRU_SAI2_SOFTRST, RK3576_CRU_SAI2_MRST_BIT,
                      RK3576_CRU_SAI2_HRST_BIT),
  RK3576_SAI_INSTANCE(3, RK3576_SAI3_ADDR, RK3576_IRQ_SAI3, 1,
                      RK3576_SAI3_DMA_TX_REQ, RK3576_SAI3_DMA_RX_REQ, true,
                      true, RK3576_CRU_SAI3_SOFTRST, RK3576_CRU_SAI3_MRST_BIT,
                      RK3576_CRU_SAI3_HRST_BIT),
  RK3576_SAI_INSTANCE(4, RK3576_SAI4_ADDR, RK3576_IRQ_SAI4, 2,
                      RK3576_SAI4_DMA_TX_REQ, RK3576_SAI4_DMA_RX_REQ, true,
                      true, RK3576_CRU_SAI4_SOFTRST, RK3576_CRU_SAI4_MRST_BIT,
                      RK3576_CRU_SAI4_HRST_BIT),
  RK3576_SAI_INSTANCE(5, RK3576_SAI5_ADDR, RK3576_IRQ_SAI5, 2, 0,
                      RK3576_SAI5_DMA_RX_REQ, false, true,
                      RK3576_CRU_SAI5_SOFTRST, RK3576_CRU_SAI5_MRST_BIT,
                      RK3576_CRU_SAI5_HRST_BIT),
  RK3576_SAI_INSTANCE(6, RK3576_SAI6_ADDR, RK3576_IRQ_SAI6, 2,
                      RK3576_SAI6_DMA_TX_REQ, RK3576_SAI6_DMA_RX_REQ, true,
                      true, RK3576_CRU_SAI6_SOFTRST, RK3576_CRU_SAI6_MRST_BIT,
                      RK3576_CRU_SAI6_HRST_BIT),
  RK3576_SAI_INSTANCE(7, RK3576_SAI7_ADDR, RK3576_IRQ_SAI7, 2,
                      RK3576_SAI7_DMA_TX_REQ, 0, true, false,
                      RK3576_CRU_SAI7_SOFTRST, RK3576_CRU_SAI7_MRST_BIT,
                      RK3576_CRU_SAI7_HRST_BIT),
  RK3576_SAI_INSTANCE(8, RK3576_SAI8_ADDR, RK3576_IRQ_SAI8, 1,
                      RK3576_SAI8_DMA_TX_REQ, 0, true, false,
                      RK3576_CRU_SAI8_SOFTRST, RK3576_CRU_SAI8_MRST_BIT,
                      RK3576_CRU_SAI8_HRST_BIT),
  RK3576_SAI_INSTANCE(9, RK3576_SAI9_ADDR, RK3576_IRQ_SAI9, 0,
                      RK3576_SAI9_DMA_TX_REQ, 0, true, false,
                      RK3576_CRU_SAI9_SOFTRST, RK3576_CRU_SAI9_MRST_BIT,
                      RK3576_CRU_SAI9_HRST_BIT),
};

#undef RK3576_SAI_INSTANCE

#define RK3576_SAI_NINSTANCES (sizeof(g_rk3576_sai) / sizeof(g_rk3576_sai[0]))

static struct rk3576_sai_s g_rk3576_sai_rx[RK3576_SAI_NINSTANCES];

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

static int rk3576_sai_clearlogic(struct rk3576_sai_s *priv, uint32_t clrbit)
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
      return -ETIMEDOUT;
    }

  return OK;
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
 *   Resolve the per-instance clock nodes, program MCLK through
 *the common
 *   clock framework, enable the bus and functional gates, then
 *pulse the SAI
 *   resets.  Board-specific MCLK-to-pin routing is
 *intentionally not done
 *   here.
 *
 ****************************************************************************/

static int rk3576_sai_setclock(struct rk3576_sai_s *priv, uint32_t frequency)
{
  uint32_t round;
  uint32_t error;
  int ret;

  round = clk_round_rate(priv->mclk_gate, frequency);
  error = round > frequency ? round - frequency : frequency - round;
  if (error > frequency / 100000)
    {
      i2serr("ERROR: SAI%d MCLK %" PRIu32
             " Hz not achievable (framework rounds to %" PRIu32 " Hz)\n",
             priv->busno, frequency, round);
      return -ERANGE;
    }

  ret = clk_set_rate(priv->mclk_gate, frequency);
  if (ret < 0)
    {
      i2serr("ERROR: SAI%d failed to set MCLK to %" PRIu32 " Hz: %d\n",
             priv->busno, frequency, ret);
      return ret;
    }

  /* Read back the actually programmed rate for diagnostics / bookkeeping. */

  priv->mclk_freq = clk_get_rate(priv->mclk_gate);

  return OK;
}

static int rk3576_sai_clockconfig(struct rk3576_sai_s *priv)
{
  char name[32];
  int ret;

  snprintf(name, sizeof(name), "hclk_sai%d", priv->busno);
  priv->hclk = clk_get(name);
  if (priv->hclk == NULL)
    {
      i2serr("ERROR: SAI%d failed to get %s\n", priv->busno, name);
      return -ENODEV;
    }

  snprintf(name, sizeof(name), "mclk_sai%d", priv->busno);
  priv->mclk_gate = clk_get(name);
  if (priv->mclk_gate == NULL)
    {
      i2serr("ERROR: SAI%d failed to get %s\n", priv->busno, name);
      return -ENODEV;
    }

  ret = rk3576_sai_setclock(priv, priv->mclk_freq);
  if (ret < 0)
    {
      return ret;
    }

  ret = clk_enable(priv->hclk);
  if (ret < 0)
    {
      i2serr("ERROR: SAI%d failed to enable HCLK: %d\n", priv->busno, ret);
      return ret;
    }

  ret = clk_enable(priv->mclk_gate);
  if (ret < 0)
    {
      i2serr("ERROR: SAI%d failed to enable MCLK: %d\n", priv->busno, ret);
      clk_disable(priv->hclk);
      return ret;
    }

  /* Reset control is not represented by the clock framework: pulse the
   * per-instance soft resets (mresetn/hresetn) manually.
   */

  putreg32(((priv->mrst_bit | priv->hrst_bit) << 16) |
               (priv->mrst_bit | priv->hrst_bit),
           RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(priv->softrst));
  up_udelay(20);

  putreg32(((priv->mrst_bit | priv->hrst_bit) << 16),
           RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(priv->softrst));
  up_udelay(20);

  return OK;
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
  uint32_t slotbits;
  bool shared = priv->peer != NULL && priv->peer->running;
  int ret;

  /* Ensure the transfer engine is stopped and its logic is cleared before
   *
   * touching the (XFER-guarded) configuration fields.  SAI_CLR is handled
   *
   * in the serial-clock domain, so briefly run the clock while requesting
   *
   * the clear, then stop it again before reconfiguration.
   */

  if (shared && (priv->samplerate != priv->peer->samplerate ||
                 priv->datalen != priv->peer->datalen ||
                 priv->mclk_freq != priv->peer->mclk_freq))
    {
      return -EBUSY;
    }

  if (!shared)
    {
      rk3576_sai_putreg(priv, RK3576_SAI_XFER, 0);
      rk3576_sai_putreg(priv, RK3576_SAI_XFER, SAI_XFER_CLK);
    }
  up_udelay(2);
  ret =
      rk3576_sai_clearlogic(priv, (priv->txenab ? SAI_CLR_TXC : SAI_CLR_RXC) |
                                      (shared ? 0 : SAI_CLR_FSC));
  if (!shared)
    {
      rk3576_sai_putreg(priv, RK3576_SAI_XFER, 0);
    }
  if (ret < 0)
    {
      return ret;
    }

  /* Operation-control register (identical TX/RX layout): one data lane,
   * MSB first, 'channels' slots per frame, 32-bit slots holding
   * left-justified valid data of priv->datalen bits.  16-bit streams use
   * 16-bit slots; 24/32-bit streams use 32-bit slots, so that the
   * mclk/bclk ratio stays an exact integer and is easy to divide.
   */

  slotbits = priv->datalen <= 16 ? 16 : 32;
  /* I2S retains two wire slots even for hardware-selected mono samples. */

  xcr = SAI_XCR_DELAY_EN | SAI_XCR_CSR(1) | SAI_XCR_SNB(2) | SAI_XCR_VDJ |
        SAI_XCR_SBW(slotbits) | SAI_XCR_VDW(priv->datalen);

  /* SCLK = MCLK / N; N is the integer mclk/bclk ratio for the current
   * sample rate / channels / slot width.  Fall back to 1 (no division)
   * when the ratio is not an exact integer.
   */

  mdiv = 2 * slotbits * priv->samplerate;
  mdiv =
      (mdiv == 0 || priv->mclk_freq % mdiv != 0) ? 1 : priv->mclk_freq / mdiv;

  if (!shared)
    {
      rk3576_sai_putreg(priv, RK3576_SAI_FSCR,
                        SAI_FSCR_EDGE_SEL | SAI_FSCR_FW(2 * slotbits) |
                            SAI_FSCR_FPW(slotbits));
      rk3576_sai_putreg(priv, RK3576_SAI_CKR, SAI_CKR_MDIV(mdiv));
    }

  if (priv->txenab)
    {
      rk3576_sai_putreg(priv, RK3576_SAI_TXCR, xcr);
      rk3576_sai_modifyreg(priv, RK3576_SAI_MONO_CR, SAI_MONO_CR_TX_MONO_EN,
                           priv->channels == 1 && priv->datalen != 24
                               ? SAI_MONO_CR_TX_MONO_EN
                               : 0);

      rk3576_sai_putreg(priv, RK3576_SAI_TX_SHIFT, SAI_XSHIFT_FS_1CYCLE);

      /* Path select routing (Debian golden). */

      if (!shared)
        {
          rk3576_sai_putreg(priv, RK3576_SAI_PATH_SEL, 0x0000e4e4);
        }

      rk3576_sai_modifyreg(
          priv, RK3576_SAI_DMACR, SAI_DMACR_TDE | SAI_DMACR_TDL_MASK,
          SAI_DMACR_TDE | SAI_DMACR_TDL(RK3576_SAI_TX_WATERMARK));

      /* Enable the TX-underrun interrupt for diagnostics. */

      rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR, 0, SAI_INTCR_TXUIE);
    }
  else
    {
      rk3576_sai_putreg(priv, RK3576_SAI_RXCR, xcr);
      rk3576_sai_modifyreg(priv, RK3576_SAI_MONO_CR, SAI_MONO_CR_RX_MONO_EN,
                           priv->channels == 1 ? SAI_MONO_CR_RX_MONO_EN : 0);
      rk3576_sai_putreg(priv, RK3576_SAI_RX_SHIFT, SAI_XSHIFT_FS_1CYCLE);

      /* RX DMA request when FIFO level >= watermark + 1. */

      rk3576_sai_modifyreg(
          priv, RK3576_SAI_DMACR, SAI_DMACR_RDE | SAI_DMACR_RDL_MASK,
          SAI_DMACR_RDE | SAI_DMACR_RDL(RK3576_SAI_RX_WATERMARK));

      rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR, 0, SAI_INTCR_RXOIE);
    }

  /* Acquire a DMA channel bound to the active direction's request line. */

  priv->dma_dev = rk3576_dma_initialize(priv->dma_ctrl);
  priv->dma = DMA_GET_CHAN(priv->dma_dev,
                           priv->txenab ? priv->dma_tx_req : priv->dma_rx_req);
  if (priv->dma == NULL)
    {
      i2serr("ERROR: no free DMA channel\n");
      rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR,
                           priv->txenab ? SAI_INTCR_TXUIE : SAI_INTCR_RXOIE,
                           0);
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
  priv->prepared = false;
  if (!priv->running)
    {
      return;
    }

  rk3576_sai_modifyreg(priv, RK3576_SAI_XFER,
                       priv->txenab ? SAI_XFER_TXS : SAI_XFER_RXS, 0);
  rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR,
                       priv->txenab ? SAI_INTCR_TXUIE : SAI_INTCR_RXOIE, 0);
  rk3576_sai_modifyreg(priv, RK3576_SAI_DMACR,
                       priv->txenab ? SAI_DMACR_TDE : SAI_DMACR_RDE, 0);
  if (priv->peer == NULL || !priv->peer->running)
    {
      rk3576_sai_putreg(priv, RK3576_SAI_XFER, 0);
    }

  if (priv->dma != NULL)
    {
      DMA_PUT_CHAN(priv->dma_dev, priv->dma);
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
  struct dma_config_s cfg;
  uintptr_t samp;
  size_t nbytes;
  uint8_t width;
  int ret;

  /* A transfer is already active: nothing to launch now. */

  if (!sq_empty(&priv->act))
    {
      return;
    }

again:
  /* No pending work: quiesce the controller. */

  if (sq_empty(&priv->pend))
    {
      if (priv->prepared)
        {
          rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR, SAI_INTCR_TXUIE, 0);
        }
      else
        {
          rk3576_sai_stop(priv);
        }
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

  if (bfc->packed_tx && bfc->txbytes == 0)
    {
      sq_addlast((sq_entry_t *)bfc, &priv->act);
      rk3576_sai_schedule(priv, OK);
      goto again;
    }

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
  if (priv->txenab)
    {
      nbytes = apb->nbytes - apb->curbyte;
      if (bfc->packed_tx)
        {
          samp = (uintptr_t)bfc->txwords;
          nbytes = bfc->txbytes;
        }
      cfg.direction = DMA_MEM_TO_DEV;
      cfg.dst_drq = priv->dma_tx_req;

      /* The apb sample buffer was filled by the CPU (cached); flush it to
       * memory so the (non-coherent) PL330 reads the real samples instead of
       * stale memory.
       */

      up_clean_dcache(samp, samp + nbytes);
    }
  else
    {
      nbytes = apb->nmaxbytes - apb->curbyte;
      cfg.direction = DMA_DEV_TO_MEM;
      cfg.src_drq = priv->dma_rx_req;

      /* Invalidate so the CPU sees what the DMA writes, not stale cache. */

      up_invalidate_dcache(samp, samp + nbytes);
    }

  ret = DMA_CONFIG(priv->dma, &cfg);
  if (ret < 0)
    {
      i2serr("ERROR: DMA_CONFIG failed: %d\n", ret);
      bfc->result = ret;
      sq_addlast((sq_entry_t *)bfc, &priv->done);
      rk3576_sai_stop(priv);
      rk3576_sai_drain(priv, ret);
      return;
    }

  sq_addlast((sq_entry_t *)bfc, &priv->act);

  if (priv->txenab)
    {
      ret = DMA_START(priv->dma, rk3576_sai_dmacallback, priv,
                      priv->base + RK3576_SAI_TXDR, samp, nbytes);
    }
  else
    {
      ret = DMA_START(priv->dma, rk3576_sai_dmacallback, priv, samp,
                      priv->base + RK3576_SAI_RXDR, nbytes);
    }

  if (ret >= 0 && priv->txenab &&
      (rk3576_sai_getreg(priv, RK3576_SAI_XFER) & SAI_XFER_TXS) == 0)
    {
      unsigned int minimum = priv->datalen > 16 ? 2 : 1;
      unsigned int attempt;

      minimum = MIN(minimum, nbytes / sizeof(uint32_t));
      for (attempt = 0; attempt < RK3576_SAI_PREFILL_RETRIES; attempt++)
        {
          uint32_t levels = rk3576_sai_getreg(priv, RK3576_SAI_TXFIFOLR) &
                            SAI_TXFIFOLR_LEVEL_MASK;
          unsigned int words = 0;
          while (levels != 0)
            {
              words += levels & SAI_TXFIFOLR_LANE0_MASK;
              levels >>= SAI_TXFIFOLR_GROUP_BITS;
            }
          if (words >= minimum)
            {
              break;
            }
          up_udelay(1);
        }
      if (attempt == RK3576_SAI_PREFILL_RETRIES)
        {
          DMA_STOP(priv->dma);
          ret = -ETIMEDOUT;
        }
    }

  if (ret < 0)
    {
      i2serr("ERROR: DMA start or FIFO prefill failed: %d\n", ret);
      sq_rem((sq_entry_t *)bfc, &priv->act);
      bfc->result = ret;
      sq_addlast((sq_entry_t *)bfc, &priv->done);
      rk3576_sai_stop(priv);
      rk3576_sai_drain(priv, ret);
      return;
    }

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

static void rk3576_sai_dmacallback(struct dma_chan_s *chan, void *arg,
                                   ssize_t len)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)arg;
  struct rk3576_sai_buffer_s *bfc;
  uintptr_t samp;
  int result = len < 0 ? (int)len : OK;

  UNUSED(chan);
  DEBUGASSERT(priv != NULL);

  if (result == OK && priv->rxenab && !sq_empty(&priv->act))
    {
      bfc = (struct rk3576_sai_buffer_s *)sq_peek(&priv->act);
      samp = (uintptr_t)&bfc->apb->samp[bfc->apb->curbyte];
      up_invalidate_dcache(samp, samp + len);
    }

  wd_cancel(&priv->dog);
  rk3576_sai_schedule(priv, result);
  if (priv->prepared && sq_empty(&priv->pend))
    {
      rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR, SAI_INTCR_TXUIE, 0);
    }
  if (result == OK && priv->running && !sq_empty(&priv->pend))
    {
      /* Keep the FIFO serviced while the completion worker is pending. */

      rk3576_sai_dmasetup(priv);
    }
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
      DMA_STOP(priv->dma);
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
      if (bfc->result == OK && bfc->sample_bytes != 0 &&
          (bfc->mono || bfc->sample_bytes == 3))
        {
          uint8_t *dest = &bfc->apb->samp[bfc->apb->curbyte];
          size_t length = bfc->apb->nbytes - bfc->apb->curbyte;
          size_t stride = bfc->mono ? 2 : 1;
          size_t container_bytes = bfc->sample_bytes == 2 ? 2 : 4;
          size_t count = length / container_bytes / stride;
          size_t i;

          for (i = 0; i < count; i++)
            {
              uint32_t sample;
              size_t byte;

              if (container_bytes == 2)
                {
                  sample = ((uint16_t *)dest)[i * stride];
                }
              else
                {
                  sample = ((uint32_t *)dest)[i * stride];
                }

              for (byte = 0; byte < bfc->sample_bytes; byte++)
                {
                  dest[i * bfc->sample_bytes + byte] = sample >> (8 * byte);
                }
            }

          bfc->apb->nbytes = bfc->apb->curbyte + count * bfc->sample_bytes;
        }

      kmm_free(bfc->txwords);
      bfc->txwords = NULL;
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

  if (priv->running && (priv->rxenab || priv->channels != channels))
    {
      return -EBUSY;
    }

  if (priv->channels != channels)
    {
      priv->ntxpartial = 0;
    }

  priv->channels = channels;
  return OK;
}

static int rk3576_sai_rxchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct rk3576_sai_s *priv =
      &g_rk3576_sai_rx[((struct rk3576_sai_s *)dev)->busno];

  if (channels < 1 || channels > 2)
    {
      return -EINVAL;
    }

  if (priv->running && (priv->txenab || priv->channels != channels))
    {
      return -EBUSY;
    }

  priv->channels = channels;
  return OK;
}

/****************************************************************************
 * Name: rk3576_sai_txsamplerate / rk3576_sai_rxsamplerate
 ****************************************************************************/

static uint32_t rk3576_sai_txsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;
  uint32_t mclk;

  DEBUGASSERT(rate > 0);

  if (priv->running)
    {
      return priv->samplerate == rate ? rate * priv->datalen : 0;
    }

  if (priv->peer != NULL && priv->peer->running)
    {
      if (priv->peer->samplerate != rate)
        {
          return 0;
        }

      priv->samplerate = rate;
      priv->mclk_freq = priv->peer->mclk_freq;
      return rate * priv->datalen;
    }

  /* The codec sets its required MCLK before setting the sample rate.  Keep

   * * that rate when it is an integer multiple of the requested LRCK; high

   * * sample rates use 128fs rather than the 256fs default.
   */

  mclk = priv->mclk_freq;
  if (mclk == 0 || mclk % rate != 0)
    {
      mclk = rate * RK3576_SAI_MCLK_FS;
    }

  if (rk3576_sai_setclock(priv, mclk) < 0)
    {
      return 0;
    }

  priv->samplerate = rate;

  return rate * priv->datalen;
}

static uint32_t rk3576_sai_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct rk3576_sai_s *rx =
      &g_rk3576_sai_rx[((struct rk3576_sai_s *)dev)->busno];
  return rk3576_sai_txsamplerate(&rx->dev, rate);
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
  priv->ntxpartial = 0;
  return priv->samplerate * bits;
}

static uint32_t rk3576_sai_rxdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct rk3576_sai_s *rx =
      &g_rk3576_sai_rx[((struct rk3576_sai_s *)dev)->busno];
  return rk3576_sai_txdatawidth(&rx->dev, bits);
}

/****************************************************************************
 * Name: rk3576_sai_pack24
 *
 * Description:
 *   Expand packed little-endian PCM in thread context.  Preserve partial
 *   frames across buffers; never expand the caller's allocation in place.
 *
 ****************************************************************************/

static int rk3576_sai_pack24(struct rk3576_sai_s *priv,
                             struct rk3576_sai_buffer_s *bfc,
                             struct ap_buffer_s *apb)
{
  const uint8_t *src = &apb->samp[apb->curbyte];
  size_t length = apb->nbytes - apb->curbyte;
  size_t previous = priv->ntxpartial;
  size_t total = length + previous;
  size_t frame = 3 * priv->channels;
  size_t complete = total / frame * frame;
  size_t slots = priv->channels == 1 ? 2 : 1;
  size_t i;

  if ((apb->flags & AUDIO_APB_FINAL) != 0 && complete != total)
    {
      return -EINVAL;
    }

  bfc->txbytes = complete / 3 * slots * sizeof(uint32_t);
  if (bfc->txbytes != 0)
    {
      bfc->txwords = kmm_malloc(bfc->txbytes);
      if (bfc->txwords == NULL)
        {
          return -ENOMEM;
        }
    }

  for (i = 0; i < complete / 3; i++)
    {
      uint32_t sample = 0;
      size_t byte;

      for (byte = 0; byte < 3; byte++)
        {
          size_t offset = i * 3 + byte;
          uint8_t value = offset < previous ? priv->txpartial[offset]
                                            : src[offset - previous];
          sample |= (uint32_t)value << (byte * 8);
        }

      /* The FIFO still consumes both I2S slots in mono mode. */

      bfc->txwords[i * slots] = sample;
      if (slots == 2)
        {
          bfc->txwords[i * slots + 1] = sample;
        }
    }

  for (i = complete; i < total; i++)
    {
      priv->txpartial[i - complete] =
          i < previous ? priv->txpartial[i] : src[i - previous];
    }

  priv->ntxpartial = total - complete;
  return OK;
}

/****************************************************************************
 * Name: rk3576_sai_send
 *
 * Description:
 *   Queue a playback buffer.  Completion runs on the work queue.
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

  /* This SAI hardware has no TX request line (e.g. SAI5), so playback is
   * not possible.
   */

  if (!priv->tx_cap)
    {
      return -ENODEV;
    }

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
  bfc->mono = false;
  bfc->sample_bytes = 0;
  bfc->txwords = NULL;
  bfc->packed_tx = priv->datalen == 24;
  if (bfc->packed_tx)
    {
      ret = rk3576_sai_pack24(priv, bfc, apb);
      if (ret < 0)
        {
          goto errout;
        }
    }

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
  struct rk3576_sai_s *priv =
      &g_rk3576_sai_rx[((struct rk3576_sai_s *)dev)->busno];
  struct rk3576_sai_buffer_s *bfc;
  irqstate_t flags;
  int ret;

  DEBUGASSERT(priv != NULL && apb != NULL);

  /* This SAI hardware has no RX request line (e.g. SAI7/8/9), so capture
   * is not possible.
   */

  if (!priv->rx_cap)
    {
      return -ENODEV;
    }

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
  bfc->mono = priv->channels == 1;
  bfc->sample_bytes = priv->datalen / 8;
  bfc->txwords = NULL;
  bfc->packed_tx = false;

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

  if (priv->running || (priv->peer != NULL && priv->peer->running))
    {
      return frequency == priv->mclk_freq ? frequency : 0;
    }

  if (rk3576_sai_setclock(priv, frequency) < 0)
    {
      return 0;
    }

  if (priv->peer != NULL)
    {
      priv->peer->mclk_freq = priv->mclk_freq;
    }

  return priv->mclk_freq;
}

/****************************************************************************
 * Name: rk3576_sai_wait_idle
 ****************************************************************************/

static int rk3576_sai_wait_idle(struct rk3576_sai_s *priv, bool frame)
{
  uint32_t version = rk3576_sai_getreg(priv, RK3576_SAI_VERSION);
  uint32_t offset = RK3576_SAI_XFER;
  uint32_t mask = frame ? SAI_XFER_FS_IDLE : SAI_XFER_TX_IDLE;
  int i;

  if (version >= RK3576_SAI_VER_2307)
    {
      offset = RK3576_SAI_STATUS;
      mask = frame ? SAI_STATUS_FS_IDLE : SAI_STATUS_TX_IDLE;
      if (version >= RK3576_SAI_VER_2311)
        {
          mask >>= 1;
        }
    }

  for (i = 0; i < RK3576_SAI_IDLE_RETRIES; i++)
    {
      if ((rk3576_sai_getreg(priv, offset) & mask) != 0)
        {
          return OK;
        }
      up_udelay(RK3576_SAI_IDLE_POLL_US);
    }
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_sai_ioctl
 *
 * Description:
 *   TX prepare/drain/shutdown, using the generic audio-I2S ioctl convention.
 *   Poll only from thread context, keeping the peer's frame clock intact.
 *
 ****************************************************************************/

static int rk3576_sai_ioctl(struct i2s_dev_s *dev, int cmd, unsigned long arg)
{
  struct rk3576_sai_s *priv = (struct rk3576_sai_s *)dev;
  int ret = OK;
  int i;

  if (arg == 0 || (cmd != AUDIOIOC_START && cmd != AUDIOIOC_STOP &&
                   cmd != AUDIOIOC_SHUTDOWN))
    {
      return -ENOTTY;
    }
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }
  if (!sq_empty(&priv->act) || !sq_empty(&priv->pend))
    {
      ret = -EBUSY;
      goto out;
    }

  if (cmd == AUDIOIOC_START)
    {
      priv->txenab = true;
      priv->rxenab = false;
      if (!priv->running)
        {
          ret = rk3576_sai_startup(priv);
        }
      if (ret >= 0)
        {
          priv->prepared = true;
          up_udelay(20);
          rk3576_sai_modifyreg(priv, RK3576_SAI_XFER, 0,
                               SAI_XFER_CLK | SAI_XFER_FSS);
        }
    }
  else if (priv->running)
    {
      rk3576_sai_modifyreg(priv, RK3576_SAI_INTCR, SAI_INTCR_TXUIE, 0);
      for (i = 0; i < RK3576_SAI_IDLE_RETRIES; i++)
        {
          if ((rk3576_sai_getreg(priv, RK3576_SAI_TXFIFOLR) &
               SAI_TXFIFOLR_LEVEL_MASK) == 0)
            {
              break;
            }
          up_udelay(RK3576_SAI_IDLE_POLL_US);
        }
      ret = i == RK3576_SAI_IDLE_RETRIES ? -ETIMEDOUT : OK;
      rk3576_sai_modifyreg(priv, RK3576_SAI_XFER, SAI_XFER_TXS, 0);
      if (rk3576_sai_wait_idle(priv, false) < 0)
        {
          ret = -ETIMEDOUT;
        }

      if (cmd == AUDIOIOC_SHUTDOWN)
        {
          priv->prepared = false;
          if (priv->peer == NULL || !priv->peer->running)
            {
              rk3576_sai_modifyreg(priv, RK3576_SAI_XFER, SAI_XFER_FSS, 0);
              if (rk3576_sai_wait_idle(priv, true) < 0)
                {
                  ret = -ETIMEDOUT;
                }
            }
          rk3576_sai_stop(priv);
        }
    }
out:
  if (cmd == AUDIOIOC_SHUTDOWN && !priv->running)
    {
      priv->prepared = false;
    }
  if (ret < 0)
    {
      i2serr("ERROR: TX lifecycle command %d failed: %d\n", cmd, ret);
    }
  nxmutex_unlock(&priv->lock);
  return ret;
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
  struct rk3576_sai_s *rx;
  int ret;

  if (busno < 0 || busno >= RK3576_SAI_NINSTANCES)
    {
      i2serr("ERROR: unsupported SAI bus %d\n", busno);
      return NULL;
    }

  priv = &g_rk3576_sai[busno];
  if (priv->initialized)
    {
      return &priv->dev;
    }

  rx = &g_rk3576_sai_rx[busno];
  rx->dev.ops = &g_rk3576_sai_ops;
  rx->base = priv->base;
  rx->irq = priv->irq;
  rx->busno = priv->busno;
  rx->dma_ctrl = priv->dma_ctrl;
  rx->dma_rx_req = priv->dma_rx_req;
  rx->rx_cap = priv->rx_cap;
  rx->samplerate = priv->samplerate;
  rx->datalen = priv->datalen;
  rx->channels = priv->channels;
  rx->mclk_freq = priv->mclk_freq;
  rx->peer = priv;
  priv->peer = rx;
  nxmutex_init(&rx->lock);
  nxsem_init(&rx->bufsem, 0, 0);
  nxsem_reset(&priv->bufsem, 0);
  rk3576_sai_bufinit(rx);

  /* Buffer pool + controller clocks. */

  rk3576_sai_bufinit(priv);
  ret = rk3576_sai_clockconfig(priv);
  if (ret < 0)
    {
      return NULL;
    }

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
  rx->hclk = priv->hclk;
  rx->mclk_gate = priv->mclk_gate;
  priv->initialized = true;
  rx->initialized = true;

  i2sinfo("SAI%d ready: base=0x%08" PRIxPTR " version=0x%08" PRIx32 "\n",
          busno, priv->base, rk3576_sai_getreg(priv, RK3576_SAI_VERSION));

  return &priv->dev;
}

#endif /* CONFIG_RK3576_SAI */
