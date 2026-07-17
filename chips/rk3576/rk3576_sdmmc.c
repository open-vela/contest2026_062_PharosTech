/****************************************************************************
 * chips/rk3576/rk3576_sdmmc.c
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
 * RK3576 SDMMC (SD card) host controller driver -- Synopsys DesignWare MSHC
 * (dw_mmc), the same IP as rk3288/rk3399.  Implements NuttX struct
 * sdio_dev_s.  Data transfer uses PIO (FIFO) by default, with optional IDMAC
 * DMA enabled by CONFIG_SDIO_DMA.  Card detection uses the controller's
 * CDETECT register (bit0 = 0 means a card is present).
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
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mmcsd.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_sdmmc.h"
#include "rk3576_sdmmc.h"

#ifdef CONFIG_RK3576_SDMMC

/***************************************************************************
 * Pre-processor Definitions
 ***************************************************************************/

/* Base clock frequency (the bootloader has configured the ciu clock source).
 * RK dw-mshc commonly uses 50MHz; this value is only used to compute the
 * CLKDIV divider, the actual frequency follows the loader.
 */

#define RK3576_SDMMC_CLKIN 50000000

/* Target card clock for each stage */

#define RK3576_SDMMC_ID_FREQ  400000   /* ID mode <400KHz */
#define RK3576_SDMMC_MMC_FREQ 25000000 /* Normal transfer */
#define RK3576_SDMMC_SD1_FREQ 25000000 /* SD 1-bit */
#define RK3576_SDMMC_SD4_FREQ 50000000 /* SD 4-bit */

/* FIFO depth (HCON[10:7]*2).  RK3576 dw-mshc is typically 256 x 32bit; the
 * value is used to derive the half-full trigger threshold.
 */

#define RK3576_SDMMC_FIFO_DEPTH 256
#define RK3576_SDMMC_FIFO_HALF  (RK3576_SDMMC_FIFO_DEPTH / 2)

/* IDMAC descriptor table entry count: 256 entries x 4KB = 1MB max per DMA
 * transfer */

#define RK3576_IDMAC_NDESC 256

/* Polling timeout (busy-wait loop limit) */

#define RK3576_SDMMC_SPIN 1000000

/* Command/data interrupt combinations */

#define SDMMC_CMDDONE_INTS (SDMMC_INT_CMD_DONE)
#define SDMMC_RESPERR_INTS (SDMMC_INT_RE | SDMMC_INT_RCRC | SDMMC_INT_RTO)
#define SDMMC_XFRDONE_INTS (SDMMC_INT_DTO)
#define SDMMC_DATAERR_INTS                                           \
  (SDMMC_INT_DCRC | SDMMC_INT_DRTO | SDMMC_INT_SBE | SDMMC_INT_EBE | \
   SDMMC_INT_FRUN | SDMMC_INT_HLE)

/***************************************************************************
 * Private Types
 ***************************************************************************/

/* RK3576 SDMMC host private state.  The first member is sdio_dev_s for
 * upcasting. */

struct rk3576_sdmmc_dev_s
{
  struct sdio_dev_s dev; /* Standard SDIO interface (must be first member) */

  uintptr_t base; /* Controller register base address */
  int irq;        /* Controller interrupt number */

  /* Event wait support */

  sem_t waitsem;          /* Semaphore to wait for interrupt events */
  struct wdog_s waitwdog; /* Wait timeout watchdog */
  volatile sdio_eventset_t waitevents; /* Enabled wait event set */
  volatile sdio_eventset_t wkupevent;  /* Wakeup event set */

  /* Media change callback */

  sdio_eventset_t cbevents; /* Enabled callback event set */
  worker_t callback;        /* Registered callback function */
  void *cbarg;              /* Callback argument */
  struct work_s cbwork;     /* Callback work queue item */

  /* Data transfer (PIO) state */

  uint32_t *buffer;          /* Transfer buffer (32bit aligned) */
  volatile size_t remaining; /* Remaining byte count */
  volatile uint32_t xfrints; /* Interrupts enabled during data transfer */
  bool widebus;              /* 4-bit bus */

#ifdef CONFIG_SDIO_DMA
  /* IDMAC (internal DMA) state */

  volatile bool dmamode; /* Current transfer uses IDMAC */
  bool dmaread;          /* DMA direction: true=read (DMA writes memory) */
  uintptr_t dmabuf;      /* DMA buffer start address */
  size_t dmalen;         /* DMA buffer length */
  struct rk3576_sdmmc_idmac_desc_s
      *descs; /* Descriptor table (points to static aligned array) */
#endif
  uint32_t blocksize; /* Current block size (recorded by blocksetup) */
};

/***************************************************************************
 * Private Function Prototypes
 ***************************************************************************/

/* Register access */

static inline uint32_t rk3576_sdmmc_getreg(struct rk3576_sdmmc_dev_s *priv,
                                           unsigned int offset);
static inline void rk3576_sdmmc_putreg(struct rk3576_sdmmc_dev_s *priv,
                                       unsigned int offset, uint32_t value);

/* Low-level helpers */

static int rk3576_sdmmc_ciu_update(struct rk3576_sdmmc_dev_s *priv,
                                   uint32_t cmdflags);
static void rk3576_sdmmc_setclock(struct rk3576_sdmmc_dev_s *priv,
                                  uint32_t freq);
static void rk3576_sdmmc_configwaitints(struct rk3576_sdmmc_dev_s *priv,
                                        uint32_t waitints,
                                        sdio_eventset_t waitevents,
                                        sdio_eventset_t wkupevent);
static void rk3576_sdmmc_configxfrints(struct rk3576_sdmmc_dev_s *priv,
                                       uint32_t xfrints);
static void rk3576_sdmmc_endwait(struct rk3576_sdmmc_dev_s *priv,
                                 sdio_eventset_t wkupevent);
static void rk3576_sdmmc_eventtimeout(wdparm_t arg);
static void rk3576_sdmmc_recvfifo(struct rk3576_sdmmc_dev_s *priv);
static void rk3576_sdmmc_sendfifo(struct rk3576_sdmmc_dev_s *priv);
static void rk3576_sdmmc_callback(struct rk3576_sdmmc_dev_s *priv);

#ifdef CONFIG_SDIO_DMA
static void rk3576_sdmmc_dma_disable(struct rk3576_sdmmc_dev_s *priv);
static bool rk3576_sdmmc_dma_ok(const uint8_t *buffer, size_t buflen);
static int rk3576_sdmmc_dma_build(struct rk3576_sdmmc_dev_s *priv,
                                  const uint8_t *buffer, size_t buflen);
static int rk3576_sdmmc_dma_common(struct rk3576_sdmmc_dev_s *priv,
                                   const uint8_t *buffer, size_t buflen,
                                   bool write);
#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
static int rk3576_sdmmc_dmapreflight(struct sdio_dev_s *dev,
                                     const uint8_t *buffer, size_t buflen);
#endif
static int rk3576_sdmmc_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                     size_t buflen);
static int rk3576_sdmmc_dmasendsetup(struct sdio_dev_s *dev,
                                     const uint8_t *buffer, size_t buflen);
#endif
#ifdef CONFIG_SDIO_BLOCKSETUP
static void rk3576_sdmmc_blocksetup(struct sdio_dev_s *dev,
                                    unsigned int blocklen,
                                    unsigned int nblocks);
#endif

/* Interrupt handling */

static int rk3576_sdmmc_interrupt(int irq, void *context, void *arg);

/* sdio_dev_s methods */

static void rk3576_sdmmc_reset(struct sdio_dev_s *dev);
static sdio_capset_t rk3576_sdmmc_capabilities(struct sdio_dev_s *dev);
static sdio_statset_t rk3576_sdmmc_status(struct sdio_dev_s *dev);
static void rk3576_sdmmc_widebus(struct sdio_dev_s *dev, bool enable);
static void rk3576_sdmmc_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate);
static int rk3576_sdmmc_attach(struct sdio_dev_s *dev);

static int rk3576_sdmmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t arg);
static int rk3576_sdmmc_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                  size_t nbytes);
static int rk3576_sdmmc_sendsetup(struct sdio_dev_s *dev,
                                  const uint8_t *buffer, size_t nbytes);
static int rk3576_sdmmc_cancel(struct sdio_dev_s *dev);

static int rk3576_sdmmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd);
static int rk3576_sdmmc_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                                  uint32_t *rshort);
static int rk3576_sdmmc_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                                 uint32_t rlong[4]);

static void rk3576_sdmmc_waitenable(struct sdio_dev_s *dev,
                                    sdio_eventset_t eventset,
                                    uint32_t timeout);
static sdio_eventset_t rk3576_sdmmc_eventwait(struct sdio_dev_s *dev);
static void rk3576_sdmmc_callbackenable(struct sdio_dev_s *dev,
                                        sdio_eventset_t eventset);
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int rk3576_sdmmc_registercallback(struct sdio_dev_s *dev,
                                         worker_t callback, void *arg);
#endif
static void rk3576_sdmmc_gotextcsd(struct sdio_dev_s *dev,
                                   const uint8_t *buffer);

/***************************************************************************
 * Private Data
 ***************************************************************************/

#ifdef CONFIG_SDIO_DMA
/* IDMAC descriptor table: cache-line (64B) aligned, shared by CPU and DMA */

static struct rk3576_sdmmc_idmac_desc_s
    g_idmac_descs[RK3576_IDMAC_NDESC] aligned_data(64);
#endif

/* Single instance: RK3576 has only one SD card slot (SDMMC0). */

static struct rk3576_sdmmc_dev_s g_sdmmcdev =
{
  .dev =
  {
    .reset           = rk3576_sdmmc_reset,
    .capabilities    = rk3576_sdmmc_capabilities,
    .status          = rk3576_sdmmc_status,
    .widebus         = rk3576_sdmmc_widebus,
    .clock           = rk3576_sdmmc_clock,
    .attach          = rk3576_sdmmc_attach,
    .sendcmd         = rk3576_sdmmc_sendcmd,
    .recvsetup       = rk3576_sdmmc_recvsetup,
    .sendsetup       = rk3576_sdmmc_sendsetup,
    .cancel          = rk3576_sdmmc_cancel,
    .waitresponse    = rk3576_sdmmc_waitresponse,
    .recv_r1         = rk3576_sdmmc_recvshort,
    .recv_r2         = rk3576_sdmmc_recvlong,
    .recv_r3         = rk3576_sdmmc_recvshort,
    .recv_r4         = rk3576_sdmmc_recvshort,
    .recv_r5         = rk3576_sdmmc_recvshort,
    .recv_r6         = rk3576_sdmmc_recvshort,
    .recv_r7         = rk3576_sdmmc_recvshort,
    .waitenable      = rk3576_sdmmc_waitenable,
    .eventwait       = rk3576_sdmmc_eventwait,
    .callbackenable  = rk3576_sdmmc_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
    .registercallback = rk3576_sdmmc_registercallback,
#endif
    .gotextcsd       = rk3576_sdmmc_gotextcsd,
#ifdef CONFIG_SDIO_DMA
#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
    .dmapreflight    = rk3576_sdmmc_dmapreflight,
#endif
    .dmarecvsetup    = rk3576_sdmmc_dmarecvsetup,
    .dmasendsetup    = rk3576_sdmmc_dmasendsetup,
#endif
#ifdef CONFIG_SDIO_BLOCKSETUP
    .blocksetup      = rk3576_sdmmc_blocksetup,
#endif
  },
  .base = RK3576_SDMMC_ADDR,
  .irq  = RK3576_IRQ_SDMMC,
};

/***************************************************************************
 * Private Functions
 ***************************************************************************/

/***************************************************************************
 * Name: rk3576_sdmmc_getreg / rk3576_sdmmc_putreg
 *
 * Description:
 *   Controller register read/write helpers.
 ***************************************************************************/

static inline uint32_t rk3576_sdmmc_getreg(struct rk3576_sdmmc_dev_s *priv,
                                           unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void rk3576_sdmmc_putreg(struct rk3576_sdmmc_dev_s *priv,
                                       unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/***************************************************************************
 * Name: rk3576_sdmmc_ciu_update
 *
 * Description:
 *   Issue a "clock update only" command (SDMMC_CMD_UPD_CLK) and wait for the
 *   CMD_START bit to self-clear.  On dw-mshc, after modifying CLKDIV/CLKENA
 *this command must be sent for the clock change to take effect.
 ***************************************************************************/

static int rk3576_sdmmc_ciu_update(struct rk3576_sdmmc_dev_s *priv,
                                   uint32_t cmdflags)
{
  uint32_t cmd =
      SDMMC_CMD_START | SDMMC_CMD_UPD_CLK | SDMMC_CMD_WAIT_PRV | cmdflags;
  int i;

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CMD, cmd);

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CMD) & SDMMC_CMD_START) == 0)
        {
          return OK;
        }
    }

  mcerr("ERROR: CIU clock update timed out\n");
  return -ETIMEDOUT;
}

/***************************************************************************
 * Name: rk3576_sdmmc_setclock
 *
 * Description:
 *   Set the card clock frequency.  Disable card clock -> change CLKDIV ->
 *enable card clock, sending UPD_CLK after each step.  On dw-mshc the actual
 *card clock = ciu_clk / (2 * CLKDIV) (CLKDIV=0 means bypass).
 ***************************************************************************/

static void rk3576_sdmmc_setclock(struct rk3576_sdmmc_dev_s *priv,
                                  uint32_t freq)
{
  uint32_t div;

  /* 1) Disable card clock */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CLKENA, 0);
  rk3576_sdmmc_ciu_update(priv, 0);

  if (freq == 0)
    {
      return;
    }

  /* 2) Compute divider: div = ceil(clkin / (2*freq)), 0 means bypass (=clkin)
   */

  if (freq >= RK3576_SDMMC_CLKIN)
    {
      div = 0;
    }
  else
    {
      div = (RK3576_SDMMC_CLKIN + (2 * freq) - 1) / (2 * freq);
      if (div > 0xff)
        {
          div = 0xff;
        }
    }

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CLKDIV, div);
  rk3576_sdmmc_ciu_update(priv, 0);

  /* 3) Enable card clock (low-power bit: auto-stop clock when idle) */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CLKENA,
                      SDMMC_CLKENA_ENABLE | SDMMC_CLKENA_LOWPWR);
  rk3576_sdmmc_ciu_update(priv, 0);
}

/***************************************************************************
 * Name: rk3576_sdmmc_configwaitints
 *
 * Description:
 *   Atomically configure the wait event set and enable the corresponding
 *   hardware interrupts.
 ***************************************************************************/

static void rk3576_sdmmc_configwaitints(struct rk3576_sdmmc_dev_s *priv,
                                        uint32_t waitints,
                                        sdio_eventset_t waitevents,
                                        sdio_eventset_t wkupevent)
{
  irqstate_t flags;

  flags = enter_critical_section();
  priv->waitevents = waitevents;
  priv->wkupevent = wkupevent;

  /* Merge in the data-transfer interrupts and write INTMASK together */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_INTMASK, priv->xfrints | waitints);
  leave_critical_section(flags);
}

/***************************************************************************
 * Name: rk3576_sdmmc_configxfrints
 *
 * Description:
 *   Configure the hardware interrupts used during data transfer.
 ***************************************************************************/

static void rk3576_sdmmc_configxfrints(struct rk3576_sdmmc_dev_s *priv,
                                       uint32_t xfrints)
{
  irqstate_t flags;

  flags = enter_critical_section();
  priv->xfrints = xfrints;

  /* Keep the current wait interrupts (deriving them from waitevents is
   * tedious, so we just OR in the full set here): simplification -- during a
   * transfer the wait interrupts are set by configwaitints, here we only add
   * xfrints.
   */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_INTMASK,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_INTMASK) |
                          xfrints);
  if (xfrints == 0)
    {
      /* Disable data-transfer interrupts (other bits such as the SDIO card
       * interrupt are not handled here and are left untouched) */

      rk3576_sdmmc_putreg(priv, RK3576_SDMMC_INTMASK,
                          rk3576_sdmmc_getreg(priv, RK3576_SDMMC_INTMASK) &
                              ~(SDMMC_XFRDONE_INTS | SDMMC_DATAERR_INTS |
                                SDMMC_INT_TXDR | SDMMC_INT_RXDR));
    }

  leave_critical_section(flags);
}

/***************************************************************************
 * Name: rk3576_sdmmc_endwait
 *
 * Description:
 *   End one event wait in interrupt context: stop the watchdog, disable
 *   interrupts, wake the waiting thread.
 ***************************************************************************/

static void rk3576_sdmmc_endwait(struct rk3576_sdmmc_dev_s *priv,
                                 sdio_eventset_t wkupevent)
{
  wd_cancel(&priv->waitwdog);

  /* Disable the interrupts of interest for this wait and record the wakeup
   * event */

  rk3576_sdmmc_configwaitints(priv, 0, 0, wkupevent);
  nxsem_post(&priv->waitsem);
}

/***************************************************************************
 * Name: rk3576_sdmmc_eventtimeout
 *
 * Description:
 *   Wait-timeout watchdog callback (software timeout).
 ***************************************************************************/

static void rk3576_sdmmc_eventtimeout(wdparm_t arg)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)arg;

  if ((priv->waitevents & SDIOWAIT_TIMEOUT) != 0)
    {
      rk3576_sdmmc_endwait(priv, SDIOWAIT_TIMEOUT);
      mcerr("ERROR: Event wait timed out\n");
    }
}

/***************************************************************************
 * Name: rk3576_sdmmc_recvfifo
 *
 * Description:
 *   Read data from the FIFO into the receive buffer (PIO), until the FIFO is
 *   empty or the buffer is full.
 ***************************************************************************/

static void rk3576_sdmmc_recvfifo(struct rk3576_sdmmc_dev_s *priv)
{
  while (priv->remaining >= sizeof(uint32_t) &&
         (rk3576_sdmmc_getreg(priv, RK3576_SDMMC_STATUS) &
          SDMMC_STATUS_FIFO_EMPTY) == 0)
    {
      *priv->buffer++ = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_DATA);
      priv->remaining -= sizeof(uint32_t);
    }
}

/***************************************************************************
 * Name: rk3576_sdmmc_sendfifo
 *
 * Description:
 *   Write data from the send buffer into the FIFO (PIO), until the FIFO is
 *full or the buffer is empty.
 ***************************************************************************/

static void rk3576_sdmmc_sendfifo(struct rk3576_sdmmc_dev_s *priv)
{
  while (priv->remaining >= sizeof(uint32_t) &&
         (rk3576_sdmmc_getreg(priv, RK3576_SDMMC_STATUS) &
          SDMMC_STATUS_FIFO_FULL) == 0)
    {
      rk3576_sdmmc_putreg(priv, RK3576_SDMMC_DATA, *priv->buffer++);
      priv->remaining -= sizeof(uint32_t);
    }
}

/***************************************************************************
 * Name: rk3576_sdmmc_callback
 *
 * Description:
 *   If the conditions are met, schedule the media-change callback onto the
 *work queue for execution.
 ***************************************************************************/

static void rk3576_sdmmc_callback(struct rk3576_sdmmc_dev_s *priv)
{
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  if (priv->callback)
    {
      sdio_statset_t status = rk3576_sdmmc_status(&priv->dev);
      bool present = (status & SDIO_STATUS_PRESENT) != 0;

      if ((present && (priv->cbevents & SDIOMEDIA_INSERTED) != 0) ||
          (!present && (priv->cbevents & SDIOMEDIA_EJECTED) != 0))
        {
          priv->cbevents = 0;
          work_queue(HPWORK, &priv->cbwork, priv->callback, priv->cbarg, 0);
        }
    }
#endif
}

/***************************************************************************
 * Name: rk3576_sdmmc_interrupt
 *
 * Description:
 *   Controller interrupt handler: handles command done/response errors, PIO
 *data movement, transfer done/errors, and card detection.
 ***************************************************************************/

static int rk3576_sdmmc_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)arg;
  uint32_t pending;
  uint32_t enabled;

  /* Read the enabled interrupt status (RINTSTS & INTMASK) */

  enabled = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_INTMASK);
  pending = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RINTSTS) & enabled;

  /* Write 1 to clear the status bits we just read */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_RINTSTS, pending);

#ifdef CONFIG_SDIO_DMA
  /* --- IDMAC status: handle DMA errors (RX/TX completion is finalized by DTO
   * below) --- */

  if (priv->dmamode)
    {
      uint32_t idsts = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_IDSTS);

      rk3576_sdmmc_putreg(priv, RK3576_SDMMC_IDSTS,
                          idsts & SDMMC_IDMAC_INT_ALL);

      if ((idsts & SDMMC_IDMAC_INT_ERR) != 0)
        {
          rk3576_sdmmc_dma_disable(priv);
          priv->remaining = 0;
          priv->buffer = NULL;
          rk3576_sdmmc_configxfrints(priv, 0);

          if ((priv->waitevents & (SDIOWAIT_TRANSFERDONE | SDIOWAIT_ERROR)) !=
              0)
            {
              rk3576_sdmmc_endwait(priv, SDIOWAIT_ERROR);
            }
        }
    }
#endif

  /* --- PIO data movement --- */

  if ((pending & SDMMC_INT_RXDR) != 0 && priv->buffer != NULL)
    {
      rk3576_sdmmc_recvfifo(priv);
    }

  if ((pending & SDMMC_INT_TXDR) != 0 && priv->buffer != NULL)
    {
      rk3576_sdmmc_sendfifo(priv);
    }

  /* --- Data transfer error --- */

  if ((pending & SDMMC_DATAERR_INTS) != 0)
    {
#ifdef CONFIG_SDIO_DMA
      if (priv->dmamode)
        {
          rk3576_sdmmc_dma_disable(priv);
        }
#endif
      priv->remaining = 0;
      priv->buffer = NULL;
      rk3576_sdmmc_configxfrints(priv, 0);

      if ((priv->waitevents & (SDIOWAIT_TRANSFERDONE | SDIOWAIT_ERROR)) != 0)
        {
          rk3576_sdmmc_endwait(priv, SDIOWAIT_ERROR);
        }
    }

  /* --- Data transfer done --- */

  else if ((pending & SDMMC_INT_DTO) != 0)
    {
#ifdef CONFIG_SDIO_DMA
      if (priv->dmamode)
        {
          /* DMA read: invalidate once more after DTO to make sure the CPU
           * reads the fresh data DMA wrote into memory (not stale cache
           * lines). */

          if (priv->dmaread)
            {
              up_invalidate_dcache(priv->dmabuf, priv->dmabuf + priv->dmalen);
            }

          rk3576_sdmmc_dma_disable(priv);
        }
#endif
      /* Wrap up: drain any remaining data from the FIFO (PIO only) */

      if (priv->buffer != NULL)
        {
          rk3576_sdmmc_recvfifo(priv);
        }

      priv->remaining = 0;
      priv->buffer = NULL;
      rk3576_sdmmc_configxfrints(priv, 0);

      if ((priv->waitevents & SDIOWAIT_TRANSFERDONE) != 0)
        {
          rk3576_sdmmc_endwait(priv, SDIOWAIT_TRANSFERDONE);
        }
    }

  /* --- Command response error --- */

  if ((pending & SDMMC_RESPERR_INTS) != 0)
    {
      if ((priv->waitevents &
           (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE | SDIOWAIT_ERROR)) != 0)
        {
          rk3576_sdmmc_endwait(priv, SDIOWAIT_ERROR);
        }
    }

  /* --- Command done --- */

  else if ((pending & SDMMC_INT_CMD_DONE) != 0)
    {
      if ((priv->waitevents & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE)) != 0)
        {
          rk3576_sdmmc_endwait(priv,
                               priv->waitevents &
                                   (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE));
        }
    }

  /* --- Card-detect change --- */

  if ((pending & SDMMC_INT_CD) != 0)
    {
      rk3576_sdmmc_callback(priv);
    }

  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_reset
 *
 * Description:
 *   Reset the controller and FIFO, clear interrupts, restore default clock/bus
 *   width.  Does not include CRU clock gating/soft reset or pinctrl; relies
 *   on the bootloader having configured them.
 ***************************************************************************/

static void rk3576_sdmmc_reset(struct sdio_dev_s *dev)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  irqstate_t flags;
  int i;

  flags = enter_critical_section();

  /* Reset controller + FIFO + DMA */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTRL, SDMMC_CTRL_RESET_ALL);

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) &
           SDMMC_CTRL_RESET_ALL) == 0)
        {
          break;
        }
    }

  /* Power on: enable card power */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_PWREN, 1);

  /* Clear all raw interrupts, mask all interrupts */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_RINTSTS, SDMMC_INT_ALL);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_INTMASK, 0);

  /* FIFO thresholds: RX/TX half-full trigger, burst length 1 (DW_MSHC FIFOTH
   * format: [30:28]=DW_MSIZE, [27:16]=RX_WMARK, [11:0]=TX_WMARK). */

  rk3576_sdmmc_putreg(
      priv, RK3576_SDMMC_FIFOTH,
      (2 << SDMMC_FIFOTH_MSIZE_SHIFT) |
          ((RK3576_SDMMC_FIFO_HALF - 1) << SDMMC_FIFOTH_RXWMARK_SHIFT) |
          (RK3576_SDMMC_FIFO_HALF << SDMMC_FIFOTH_TXWMARK_SHIFT));

  /* Set data/response timeout to maximum */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_TMOUT, 0xffffffff);

  /* Default to 1-bit bus */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTYPE, SDMMC_CTYPE_1BIT);
  priv->widebus = false;

  /* Global interrupt enable (controller level) */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTRL,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) |
                          SDMMC_CTRL_INT_ENABLE);

  /* Reset the driver software state */

  priv->buffer = NULL;
  priv->remaining = 0;
  priv->xfrints = 0;
  priv->waitevents = 0;
  priv->wkupevent = 0;

#ifdef CONFIG_SDIO_DMA
  /* Initialize IDMAC: soft-reset and wait for self-clear, clear status, enable
   * RX/TX-done + error interrupts.  Reset does not select IDMAC by default
   * (USE_IDMAC=0); PIO is the default path. */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BMOD, SDMMC_IDMAC_SWRESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_BMOD) &
           SDMMC_IDMAC_SWRESET) == 0)
        {
          break;
        }
    }

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_IDSTS, SDMMC_IDMAC_INT_ALL);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_IDINTEN, SDMMC_IDMAC_INT_ENA);
  priv->dmamode = false;
#endif
  priv->blocksize = 0;

  /* ID-mode initial clock (<400KHz) */

  rk3576_sdmmc_setclock(priv, RK3576_SDMMC_ID_FREQ);

  leave_critical_section(flags);
}

/***************************************************************************
 * Name: rk3576_sdmmc_capabilities
 *
 * Description:
 *   Report host capabilities: 4-bit bus, PIO by default and IDMAC DMA when
 *   CONFIG_SDIO_DMA is enabled.
 ***************************************************************************/

static sdio_capset_t rk3576_sdmmc_capabilities(struct sdio_dev_s *dev)
{
  sdio_capset_t caps = 0;

#ifndef CONFIG_SDIO_WIDTH_D1_ONLY
  caps |= SDIO_CAPS_4BIT;
#endif

  /* DMABEFOREWRITE must be set (both for the PIO and IDMAC paths): it makes
   * mmcsd defer the write command
   * (CMD24/25) until after SENDSETUP.  Otherwise mmcsd would issue CMD24
   * before SENDSETUP, and when the controller enters the write-data state the
   * TX FIFO is still empty -> underrun -> data error (SENDSETUP prefill may
   * even collide with the buffer cleared by the error interrupt and crash).
   * With it set: SENDSETUP prefills the FIFO -> then CMD24 is issued -> no
   * underrun. */

  caps |= SDIO_CAPS_DMABEFOREWRITE;

#ifdef CONFIG_SDIO_DMA
  /* IDMAC moves data in hardware: speedup + fixes multi-block write underrun.
   * Misaligned/over-capacity buffers are rejected by dmapreflight, and mmcsd
   * automatically falls back to the PIO path. */

  caps |= SDIO_CAPS_DMASUPPORTED;
#endif

  return caps;
}

/***************************************************************************
 * Name: rk3576_sdmmc_status
 *
 * Description:
 *   Return card present/write-protect status, using the controller CDETECT
 *   (bit0=0 = present) and WRTPRT.
 ***************************************************************************/

static sdio_statset_t rk3576_sdmmc_status(struct sdio_dev_s *dev)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  sdio_statset_t status = 0;

  if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CDETECT) &
       SDMMC_CDETECT_NOCARD) == 0)
    {
      status |= SDIO_STATUS_PRESENT; /* bit0=0 => card present */
    }

  if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_WRTPRT) &
       SDMMC_WRTPRT_PROTECTED) != 0)
    {
      status |= SDIO_STATUS_WRPROTECTED;
    }

  return status;
}

/***************************************************************************
 * Name: rk3576_sdmmc_widebus
 ***************************************************************************/

static void rk3576_sdmmc_widebus(struct sdio_dev_s *dev, bool enable)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTYPE,
                      enable ? SDMMC_CTYPE_4BIT : SDMMC_CTYPE_1BIT);
  priv->widebus = enable;
}

/***************************************************************************
 * Name: rk3576_sdmmc_clock
 ***************************************************************************/

static void rk3576_sdmmc_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  uint32_t freq;

  switch (rate)
    {
      default:
      case CLOCK_SDIO_DISABLED:
        freq = 0;
        break;

      case CLOCK_IDMODE:
        freq = RK3576_SDMMC_ID_FREQ;
        break;

      case CLOCK_MMC_TRANSFER:
        freq = RK3576_SDMMC_MMC_FREQ;
        break;

      case CLOCK_SD_TRANSFER_1BIT:
        freq = RK3576_SDMMC_SD1_FREQ;
        break;

      case CLOCK_SD_TRANSFER_4BIT:
        freq = RK3576_SDMMC_SD4_FREQ;
        break;
    }

  rk3576_sdmmc_setclock(priv, freq);
}

/***************************************************************************
 * Name: rk3576_sdmmc_attach
 *
 * Description:
 *   Attach and enable the controller interrupt.
 ***************************************************************************/

static int rk3576_sdmmc_attach(struct sdio_dev_s *dev)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  int ret;

  ret = irq_attach(priv->irq, rk3576_sdmmc_interrupt, priv);
  if (ret < 0)
    {
      mcerr("ERROR: irq_attach failed irq=%d ret=%d\n", priv->irq, ret);
      return ret;
    }

  /* Mask all sources, clear pending status, then open the controller-level
   * interrupt gate */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_INTMASK, 0);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_RINTSTS, SDMMC_INT_ALL);

  up_enable_irq(priv->irq);
  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_sendcmd
 *
 * Description:
 *   Send a command.  Set the response/data bits of the dw-mshc CMD register
 *   according to the NuttX command encoding.
 ***************************************************************************/

static int rk3576_sdmmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t arg)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  uint32_t regval;
  uint32_t cmdidx;
  int i;

  cmdidx = (cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT;
  regval = (cmdidx << SDMMC_CMD_INDEX_SHIFT) | SDMMC_CMD_START |
           SDMMC_CMD_USE_HOLD_REG | SDMMC_CMD_WAIT_PRV;

  /* Response type */

  switch (cmd & MMCSD_RESPONSE_MASK)
    {
      case MMCSD_NO_RESPONSE:
        break;

      case MMCSD_R2_RESPONSE:
        /* Long response (136-bit), check CRC */

        regval |=
            SDMMC_CMD_RESP_EXPECT | SDMMC_CMD_RESP_LONG | SDMMC_CMD_RESP_CRC;
        break;

      case MMCSD_R3_RESPONSE:
      case MMCSD_R4_RESPONSE:
        /* Short response, no CRC check (OCR class) */

        regval |= SDMMC_CMD_RESP_EXPECT;
        break;

      default:
        /* R1/R1B/R5/R6/R7: short response + CRC */

        regval |= SDMMC_CMD_RESP_EXPECT | SDMMC_CMD_RESP_CRC;
        break;
    }

  /* Data transfer direction */

  if ((cmd & MMCSD_DATAXFR_MASK) != 0)
    {
      regval |= SDMMC_CMD_DATA_EXPECTED;
      if ((cmd & MMCSD_WRXFR) != 0)
        {
          regval |= SDMMC_CMD_WRITE;
        }
    }

  /* Stop transfer command (CMD12) */

  if ((cmd & MMCSD_STOPXFR) != 0)
    {
      regval |= SDMMC_CMD_SEND_STOP;
    }

  /* CMD0 requires sending the 80-clock init sequence */

  if (cmdidx == MMCSD_CMDIDX0)
    {
      regval |= SDMMC_CMD_INIT;
    }

  /* Write the argument, clear command-done/response status bits, then start
   * the command */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_RINTSTS,
                      SDMMC_INT_CMD_DONE | SDMMC_RESPERR_INTS);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CMDARG, arg);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CMD, regval);

  /* Wait for the hardware to accept the command (CMD_START self-clears) */

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CMD) & SDMMC_CMD_START) == 0)
        {
          return OK;
        }
    }

  mcerr("ERROR: Command issue timed out cmd=%08" PRIx32 "\n", cmd);
  return -ETIMEDOUT;
}

/***************************************************************************
 * Name: rk3576_sdmmc_recvsetup
 *
 * Description:
 *   PIO receive setup: reset FIFO, set block size/byte count, register the
 *buffer and enable the RX interrupt.
 ***************************************************************************/

static int rk3576_sdmmc_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                  size_t nbytes)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  DEBUGASSERT(buffer != NULL && nbytes > 0);
  DEBUGASSERT(((uintptr_t)buffer & 3) == 0); /* 32bit aligned */

  /* Reset FIFO */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTRL,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) |
                          SDMMC_CTRL_FIFO_RESET);

  priv->buffer = (uint32_t *)buffer;
  priv->remaining = nbytes;

  /* Set block size and byte count: block size uses the value recorded by
   * blocksetup (defaults to nbytes) */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BLKSIZ,
                      priv->blocksize ? priv->blocksize : nbytes);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BYTCNT, nbytes);

  /* Enable data-receive interrupts: RXDR + DTO + errors */

  rk3576_sdmmc_configxfrints(priv, SDMMC_INT_RXDR | SDMMC_XFRDONE_INTS |
                                       SDMMC_DATAERR_INTS);
  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_sendsetup
 *
 * Description:
 *   PIO send setup: reset FIFO, set block size/byte count, register the buffer
 *   and enable the TX interrupt.
 ***************************************************************************/

static int rk3576_sdmmc_sendsetup(struct sdio_dev_s *dev,
                                  const uint8_t *buffer, size_t nbytes)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  int i;

  DEBUGASSERT(buffer != NULL && nbytes > 0);
  DEBUGASSERT(((uintptr_t)buffer & 3) == 0);

  /* Reset FIFO and wait for the bit to self-clear, otherwise the subsequently
   * prefilled data would be wiped out by the reset */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTRL,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) |
                          SDMMC_CTRL_FIFO_RESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) &
           SDMMC_CTRL_FIFO_RESET) == 0)
        {
          break;
        }
    }

  priv->buffer = (uint32_t *)buffer;
  priv->remaining = nbytes;

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BLKSIZ,
                      priv->blocksize ? priv->blocksize : nbytes);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BYTCNT, nbytes);

  /* Prefill TX FIFO: on DW-MSHC, a PIO write must push data into the FIFO
   * before issuing the command, otherwise after CMD24 the TX FIFO is empty ->
   * controller underrun (FRUN) -> data error -> write fails.  A single block
   * (<= FIFO capacity) is filled at once; larger transfers are continued by
   * later TXDR interrupts.  The prefill must happen BEFORE enabling
   * interrupts: otherwise once TXDR is enabled an empty FIFO immediately
   * raises an interrupt, and the sendfifo in the ISR would race with this
   * prefill over priv->buffer/remaining. */

  rk3576_sdmmc_sendfifo(priv);

  /* After prefilling, enable data-send interrupts: TXDR (refill) + DTO +
   * errors */

  rk3576_sdmmc_configxfrints(priv, SDMMC_INT_TXDR | SDMMC_XFRDONE_INTS |
                                       SDMMC_DATAERR_INTS);
  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_cancel
 *
 * Description:
 *   Cancel an outstanding data transfer: disable interrupts, stop the
 *watchdog, clear software state.
 ***************************************************************************/

static int rk3576_sdmmc_cancel(struct sdio_dev_s *dev)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  rk3576_sdmmc_configxfrints(priv, 0);
  rk3576_sdmmc_configwaitints(priv, 0, 0, 0);
  wd_cancel(&priv->waitwdog);

  priv->buffer = NULL;
  priv->remaining = 0;
  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_waitresponse
 *
 * Description:
 *   Poll-wait until the command response is ready, checking for response
 *   timeout/CRC errors.
 ***************************************************************************/

static int rk3576_sdmmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  uint32_t status;
  int i;

  /* No-response command: only wait for command done */

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      status = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RINTSTS);

      if ((status & SDMMC_INT_RTO) != 0)
        {
          mcerr("ERROR: Response timed out cmd=%08" PRIx32 "\n", cmd);
          return -ETIMEDOUT;
        }

      if ((status & SDMMC_INT_RCRC) != 0 &&
          (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R3_RESPONSE &&
          (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R4_RESPONSE)
        {
          mcerr("ERROR: Response CRC error cmd=%08" PRIx32 "\n", cmd);
          return -EIO;
        }

      if ((status & SDMMC_INT_CMD_DONE) != 0)
        {
          return OK;
        }
    }

  return -ETIMEDOUT;
}

/***************************************************************************
 * Name: rk3576_sdmmc_recvshort / rk3576_sdmmc_recvlong
 *
 * Description:
 *   Read the command response.  Short response reads RESP0; long response (R2)
 *   reads RESP0..3.  On dw-mshc the long-response RESP0 is the lowest word
 *while NuttX expects rlong[0] to be the highest word, so we fill in reverse
 *order.
 ***************************************************************************/

static int rk3576_sdmmc_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                                  uint32_t *rshort)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  uint32_t status = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RINTSTS);

  if ((status & SDMMC_INT_RTO) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((status & SDMMC_INT_RCRC) != 0 &&
      (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R3_RESPONSE &&
      (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R4_RESPONSE)
    {
      return -EIO;
    }

  if (rshort != NULL)
    {
      *rshort = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RESP0);
    }

  return OK;
}

static int rk3576_sdmmc_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                                 uint32_t rlong[4])
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  uint32_t status = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RINTSTS);

  if ((status & SDMMC_INT_RTO) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((status & SDMMC_INT_RCRC) != 0)
    {
      return -EIO;
    }

  if (rlong != NULL)
    {
      /* RESP3 is the highest word -> rlong[0] */

      rlong[0] = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RESP3);
      rlong[1] = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RESP2);
      rlong[2] = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RESP1);
      rlong[3] = rk3576_sdmmc_getreg(priv, RK3576_SDMMC_RESP0);
    }

  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_waitenable
 *
 * Description:
 *   Enable a set of wait events and start the software timeout watchdog.
 ***************************************************************************/

static void rk3576_sdmmc_waitenable(struct sdio_dev_s *dev,
                                    sdio_eventset_t eventset, uint32_t timeout)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  uint32_t waitints = 0;

  /* Map wait events to hardware interrupts */

  if ((eventset & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE)) != 0)
    {
      waitints |= SDMMC_INT_CMD_DONE | SDMMC_RESPERR_INTS;
    }

  if ((eventset & SDIOWAIT_TRANSFERDONE) != 0)
    {
      waitints |= SDMMC_XFRDONE_INTS | SDMMC_DATAERR_INTS;
    }

  rk3576_sdmmc_configwaitints(priv, waitints, eventset, 0);

  /* Start the timeout watchdog (only when TIMEOUT is of interest and
   * timeout>0) */

  if ((eventset & SDIOWAIT_TIMEOUT) != 0 && timeout > 0)
    {
      int ret = wd_start(&priv->waitwdog, MSEC2TICK(timeout),
                         rk3576_sdmmc_eventtimeout, (wdparm_t)priv);
      if (ret < 0)
        {
          mcerr("ERROR: wd_start failed ret=%d\n", ret);
        }
    }
}

/***************************************************************************
 * Name: rk3576_sdmmc_eventwait
 *
 * Description:
 *   Block waiting for one of the enabled events to occur (or a timeout).
 *   Returns the wakeup event set.
 ***************************************************************************/

static sdio_eventset_t rk3576_sdmmc_eventwait(struct sdio_dev_s *dev)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;
  sdio_eventset_t wkupevent;

  /* Wait for an interrupt to post waitsem (done in endwait) */

  for (;;)
    {
      nxsem_wait_uninterruptible(&priv->waitsem);

      /* A non-zero wkupevent means an event really woke us */

      wkupevent = priv->wkupevent;
      if (wkupevent != 0)
        {
          break;
        }
    }

  /* Wait finished, clear all wait interrupts */

  rk3576_sdmmc_configwaitints(priv, 0, 0, 0);
  wd_cancel(&priv->waitwdog);
  priv->wkupevent = 0;

  return wkupevent;
}

/***************************************************************************
 * Name: rk3576_sdmmc_callbackenable
 ***************************************************************************/

static void rk3576_sdmmc_callbackenable(struct sdio_dev_s *dev,
                                        sdio_eventset_t eventset)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  priv->cbevents = eventset;
  rk3576_sdmmc_callback(priv);
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
/***************************************************************************
 * Name: rk3576_sdmmc_registercallback
 ***************************************************************************/

static int rk3576_sdmmc_registercallback(struct sdio_dev_s *dev,
                                         worker_t callback, void *arg)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  priv->cbevents = 0;
  priv->cbarg = arg;
  priv->callback = callback;
  return OK;
}
#endif

/***************************************************************************
 * Name: rk3576_sdmmc_gotextcsd
 *
 * Description:
 *   Receive EXT CSD notification (used by eMMC).  Not needed for SD cards,
 *   empty implementation.
 ***************************************************************************/

static void rk3576_sdmmc_gotextcsd(struct sdio_dev_s *dev,
                                   const uint8_t *buffer)
{
  UNUSED(dev);
  UNUSED(buffer);
}

#ifdef CONFIG_SDIO_DMA

/***************************************************************************
 * Name: rk3576_sdmmc_dma_disable
 *
 * Description:
 *   Disable IDMAC: clear the DE bit in BMOD and USE_IDMAC in CTRL, exit DMA
 *mode.
 ***************************************************************************/

static void rk3576_sdmmc_dma_disable(struct rk3576_sdmmc_dev_s *priv)
{
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTRL,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) &
                          ~SDMMC_CTRL_USE_IDMAC);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BMOD,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_BMOD) &
                          ~SDMMC_IDMAC_ENABLE);
  priv->dmamode = false;
}

/***************************************************************************
 * Name: rk3576_sdmmc_dma_build
 *
 * Description:
 *   Split [buffer, buffer+buflen) into <=4KB segments and fill the static
 *   descriptor chain (32-bit chained mode).  The first segment gets FD, the
 *last gets LD, non-last segments get DIC (only the last raises a completion
 *   interrupt).  After filling, clean the descriptor table's dcache so IDMAC
 *reads the descriptors written by the CPU.
 ***************************************************************************/

static int rk3576_sdmmc_dma_build(struct rk3576_sdmmc_dev_s *priv,
                                  const uint8_t *buffer, size_t buflen)
{
  struct rk3576_sdmmc_idmac_desc_s *desc = priv->descs;
  uintptr_t addr = (uintptr_t)buffer;
  size_t remaining = buflen;
  unsigned int n;
  unsigned int i;

  n = (buflen + RK3576_IDMAC_BUFSZ - 1) / RK3576_IDMAC_BUFSZ;
  if (n == 0 || n > RK3576_IDMAC_NDESC)
    {
      return -EFAULT;
    }

  for (i = 0; i < n; i++)
    {
      size_t seg =
          remaining > RK3576_IDMAC_BUFSZ ? RK3576_IDMAC_BUFSZ : remaining;
      uint32_t ctrl = IDMAC_DES0_OWN | IDMAC_DES0_CH;

      desc[i].des1 = IDMAC_DES1_BS1(seg);
      desc[i].des2 = (uint32_t)addr; /* buffer1 physical address (low 4G) */

      if (i == 0)
        {
          ctrl |= IDMAC_DES0_FD;
        }

      if (i == n - 1)
        {
          ctrl |= IDMAC_DES0_LD; /* Last segment: stop after completion */
          desc[i].des3 = 0;
        }
      else
        {
          ctrl |= IDMAC_DES0_DIC; /* Non-last segment raises no completion
                                     interrupt */
          desc[i].des3 = (uint32_t)(uintptr_t)&desc[i + 1];
        }

      desc[i].des0 = ctrl;

      addr += seg;
      remaining -= seg;
    }

  /* The descriptors are written by the CPU and read by DMA -- the descriptor
   * table must be flushed back to memory */

  up_clean_dcache((uintptr_t)desc,
                  (uintptr_t)desc +
                      n * sizeof(struct rk3576_sdmmc_idmac_desc_s));
  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_dma_common
 *
 * Description:
 *   Common flow for dmarecvsetup/dmasendsetup: reset FIFO+DMA, build the
 *   descriptor chain, handle cache, enable IDMAC, set block size/byte
 *   count, and switch to DTO+error interrupts (disabling the PIO RXDR/TXDR).
 ***************************************************************************/

static int rk3576_sdmmc_dma_common(struct rk3576_sdmmc_dev_s *priv,
                                   const uint8_t *buffer, size_t buflen,
                                   bool write)
{
  uint32_t blksz;
  int ret;
  int i;

  /* Enter DMA mode: clear the PIO buffer pointer so the ISR does not go
   * through recvfifo/sendfifo */

  priv->buffer = NULL;
  priv->remaining = 0;
  priv->dmabuf = (uintptr_t)buffer;
  priv->dmalen = buflen;
  priv->dmaread = !write;
  priv->dmamode = true;

  /* Reset FIFO + DMA and wait for self-clear */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTRL,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) |
                          SDMMC_CTRL_FIFO_RESET | SDMMC_CTRL_DMA_RESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) &
           (SDMMC_CTRL_FIFO_RESET | SDMMC_CTRL_DMA_RESET)) == 0)
        {
          break;
        }
    }

  /* Soft-reset IDMAC, wait for self-clear */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BMOD, SDMMC_IDMAC_SWRESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_sdmmc_getreg(priv, RK3576_SDMMC_BMOD) &
           SDMMC_IDMAC_SWRESET) == 0)
        {
          break;
        }
    }

  /* Build the descriptor chain */

  ret = rk3576_sdmmc_dma_build(priv, buffer, buflen);
  if (ret < 0)
    {
      priv->dmamode = false;
      return ret;
    }

  /* Cache coherency:
   *   send (DMA reads memory) -> clean, flush CPU cache to memory;
   *   receive (DMA writes memory) -> invalidate, to avoid dirty lines writing
   *                        back over DMA data; after DTO the ISR invalidates
   * once more so the CPU reads the fresh data. */

  if (write)
    {
      up_clean_dcache((uintptr_t)buffer, (uintptr_t)buffer + buflen);
    }
  else
    {
      up_invalidate_dcache((uintptr_t)buffer, (uintptr_t)buffer + buflen);
    }

  /* Enable IDMAC interrupts (RX/TX done + error) */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_IDSTS, SDMMC_IDMAC_INT_ALL);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_IDINTEN, SDMMC_IDMAC_INT_ENA);

  /* Descriptor base address -> DBADDR */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_DBADDR,
                      (uint32_t)(uintptr_t)priv->descs);

  /* CTRL selects internal DMA; BMOD enables DE (+ fixed burst) */

  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_CTRL,
                      rk3576_sdmmc_getreg(priv, RK3576_SDMMC_CTRL) |
                          SDMMC_CTRL_USE_IDMAC);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BMOD,
                      SDMMC_IDMAC_FB | SDMMC_IDMAC_ENABLE);

  /* Block size/byte count: blocksize recorded by blocksetup, defaults to 512
   */

  blksz = priv->blocksize ? priv->blocksize : buflen;
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BLKSIZ, blksz);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BYTCNT, buflen);

  /* Data completion uses DTO, errors use DATAERR; DMA mode does not enable
   * RXDR/TXDR */

  rk3576_sdmmc_configxfrints(priv, SDMMC_XFRDONE_INTS | SDMMC_DATAERR_INTS);
  return OK;
}

/***************************************************************************
 * Name: rk3576_sdmmc_dma_ok
 *
 * Description:
 *   Decide whether the buffer can use IDMAC: cache-line aligned, length
 *   aligned, within descriptor capacity, and the physical address in the low
 *   4G (32-bit descriptors).
 ***************************************************************************/

static bool rk3576_sdmmc_dma_ok(const uint8_t *buffer, size_t buflen)
{
  uintptr_t addr = (uintptr_t)buffer;
  size_t line = up_get_dcache_linesize();

  if (line == 0)
    {
      line = 64; /* armv8-a L1 D-cache line */
    }

  return (addr & (line - 1)) == 0 && (buflen & (line - 1)) == 0 &&
         buflen <= (size_t)RK3576_IDMAC_NDESC * RK3576_IDMAC_BUFSZ &&
         (uint64_t)addr + buflen <= RK3576_IDMAC_DMA_LIMIT;
}

/***************************************************************************
 * Name: rk3576_sdmmc_dmapreflight
 *
 * Description:
 *   Validate whether the buffer is suitable for IDMAC.  Always returns OK
 *   here; the real DMA-vs-PIO decision is made in the setup path.
 ***************************************************************************/

#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
static int rk3576_sdmmc_dmapreflight(struct sdio_dev_s *dev,
                                     const uint8_t *buffer, size_t buflen)
{
  /* Always return OK: mmcsd does NOT fall back to PIO on preflight failure (it
   * returns EFAULT directly), so we must not reject any buffer here.  Whether
   * DMA is actually usable is decided in the setup below; when not satisfied
   * (misaligned/over-capacity/above 4G) the driver itself falls back to PIO,
   * so any buffer can be read/written. */

  UNUSED(dev);
  UNUSED(buffer);
  UNUSED(buflen);
  return OK;
}
#endif

/***************************************************************************
 * Name: rk3576_sdmmc_dmarecvsetup / rk3576_sdmmc_dmasendsetup
 ***************************************************************************/

static int rk3576_sdmmc_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                     size_t buflen)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  DEBUGASSERT(buffer != NULL && buflen > 0);

  /* Buffer unsuitable for DMA -> the driver falls back to PIO itself (mmcsd
   * will not fall back for us) */

  if (!rk3576_sdmmc_dma_ok(buffer, buflen))
    {
      priv->dmamode = false;
      return rk3576_sdmmc_recvsetup(dev, buffer, buflen);
    }

  return rk3576_sdmmc_dma_common(priv, buffer, buflen, false);
}

static int rk3576_sdmmc_dmasendsetup(struct sdio_dev_s *dev,
                                     const uint8_t *buffer, size_t buflen)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  DEBUGASSERT(buffer != NULL && buflen > 0);

  if (!rk3576_sdmmc_dma_ok(buffer, buflen))
    {
      priv->dmamode = false;
      return rk3576_sdmmc_sendsetup(dev, buffer, buflen);
    }

  return rk3576_sdmmc_dma_common(priv, buffer, buflen, true);
}

#endif /* CONFIG_SDIO_DMA */

#ifdef CONFIG_SDIO_BLOCKSETUP
/***************************************************************************
 * Name: rk3576_sdmmc_blocksetup
 *
 * Description:
 *   Record the block size and preset BLKSIZ/BYTCNT.  Both the DMA and PIO
 *paths set the block size based on this.
 ***************************************************************************/

static void rk3576_sdmmc_blocksetup(struct sdio_dev_s *dev,
                                    unsigned int blocklen,
                                    unsigned int nblocks)
{
  struct rk3576_sdmmc_dev_s *priv = (struct rk3576_sdmmc_dev_s *)dev;

  priv->blocksize = blocklen;
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BLKSIZ, blocklen);
  rk3576_sdmmc_putreg(priv, RK3576_SDMMC_BYTCNT, blocklen * nblocks);
}
#endif

/***************************************************************************
 * Public Functions
 ***************************************************************************/

/***************************************************************************
 * Name: rk3576_sdmmc_initialize
 *
 * Description:
 *   Initialize the RK3576 SDMMC (SD card) host and return an sdio_dev_s for
 *the mmcsd layer to mount.
 *
 * Input Parameters:
 *   slotno - Slot number (RK3576 has only one SD card slot, use 0).
 *
 * Returned Value:
 *   On success returns an sdio_dev_s pointer, on failure returns NULL.
 ***************************************************************************/

struct sdio_dev_s *rk3576_sdmmc_initialize(int slotno)
{
  struct rk3576_sdmmc_dev_s *priv = &g_sdmmcdev;

  DEBUGASSERT(slotno == 0);

#ifdef CONFIG_SDIO_DMA
  priv->descs = g_idmac_descs;
#endif

  nxmutex_init(&priv->dev.mutex);
  nxsem_init(&priv->waitsem, 0, 0);
  wd_init(&priv->waitwdog);

  /* Reset the controller to a known state */

  rk3576_sdmmc_reset(&priv->dev);

  mcinfo("RK3576 SDMMC initialization complete base=%08" PRIxPTR " irq=%d\n",
         priv->base, priv->irq);

  return &priv->dev;
}

#endif /* CONFIG_RK3576_SDMMC */
