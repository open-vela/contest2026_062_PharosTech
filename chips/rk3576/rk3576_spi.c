/****************************************************************************
 * chips/rk3576/rk3576_spi.c
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
 * RK3576 SPI (Serial Peripheral Interface) master driver.
 *
 * Implements the NuttX SPI lower-half interface (struct spi_dev_s /
 * struct spi_ops_s) for the Rockchip RK3576 SPI controller, a Synopsys
 * DesignWare SSI compatible IP (TRM Chapter 30).
 *
 * Master mode only; Motorola SPI frame format (frf=00) with 8-bit data
 * frames.  Transfers run over the 32-entry TX/RX FIFOs with polled status
 * (no interrupt/DMA path).  All five controllers (SPI0..SPI4) and both
 * slave-select lines per controller are supported; the driver is
 * data-driven at runtime.
 *
 * Pin muxing (SS_N, SCK, MOSI, MISO) is the board's responsibility; this
 * driver only owns the controller and its PCLK/SCLK clock gates.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <assert.h>
#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clk/clk_provider.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "arm64_arch.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_spi.h"
#include "rk3576_spi.h"

#ifdef CONFIG_RK3576_SPI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_SPI_FIFO_DEPTH 64      /* TX/RX FIFO depth (entries) */
#define RK3576_SPI_POLL_LIMIT 1000000 /* busy-wait iterations */
#define RK3576_SPI_NUM_CLKSRC 4       /* clock source mux parent count */

/* SPI controller base addresses (TRM §30.4.1). */

static const uintptr_t g_rk3576_spi_base[RK3576_SPI_NUM_CONTROLLERS] = {
  RK3576_SPI0_ADDR, RK3576_SPI1_ADDR, RK3576_SPI2_ADDR,
  RK3576_SPI3_ADDR, RK3576_SPI4_ADDR,
};

/* SPI controller GIC IRQ numbers (TRM, SPI0..SPI4).  These exist in
 * include/irq.h as RK3576_IRQ_SPI0..SPI4 but are only needed for the
 * interrupt path; this driver is polled and does not use them.
 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_spi_priv_s
{
  struct spi_dev_s dev; /* Externally visible SPI interface (must be 1st) */
  uintptr_t base;       /* Controller base address */
  struct clk_s *pclk;   /* PCLK — APB bus clock */
  struct clk_s *sclk;   /* SCLK — serial (functional) clock */
  mutex_t lock;         /* Shield bus; guards transfers AND one-time init */
  uint32_t frequency;   /* Configured SCLK frequency */
  enum spi_mode_e mode; /* SPI mode (CPOL/CPHA) */
  int nbits;            /* Bits per word (currently 8) */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* g_rk3576_spi[].lock is statically initialized (NXMUTEX_INITIALIZER): it
 * must already be legal from before the scheduler starts so that the very
 * same per-bus mutex can both
 *   - serialize concurrent bring-up of the same bus in
 *     rk3576_spi_initialize() (check/init/publish is atomic under it), and
 *   - guard every steady-state transfer via rk3576_spi_lock().
 * A single instance never holds the lock across the lock/unlock boundary
 * (initialize() always unlocks before handing the handle back), so this
 * non-recursive mutex is never re-entered on the same thread.
 */

static struct rk3576_spi_priv_s g_rk3576_spi[RK3576_SPI_NUM_CONTROLLERS] = {
  [0] = { .lock = NXMUTEX_INITIALIZER }, [1] = { .lock = NXMUTEX_INITIALIZER },
  [2] = { .lock = NXMUTEX_INITIALIZER }, [3] = { .lock = NXMUTEX_INITIALIZER },
  [4] = { .lock = NXMUTEX_INITIALIZER },
};

static bool g_rk3576_spi_inited[RK3576_SPI_NUM_CONTROLLERS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: spi_getreg / spi_putreg
 ****************************************************************************/

static inline uint32_t spi_getreg(struct rk3576_spi_priv_s *priv,
                                  unsigned int off)
{
  return getreg32(priv->base + off);
}

static inline void spi_putreg(struct rk3576_spi_priv_s *priv, unsigned int off,
                              uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_spi_setctr0
 *
 * Description:
 *   Program CTRLR0 for the current mode / bits configuration.  Uses the
 *   Motorola SPI frame format (frf=00), 8-bit data frames, master mode,
 *   transmit-and-receive (xfm=00), MSB-first, little-endian.
 ****************************************************************************/

static void rk3576_spi_setctr0(struct rk3576_spi_priv_s *priv)
{
  uint32_t ctrlr0;

  ctrlr0 = RK3576_SPI_CTRLR0_FRF_SPI << RK3576_SPI_CTRLR0_FRF_SHIFT;
  ctrlr0 |= RK3576_SPI_CTRLR0_XFM_TXRX << RK3576_SPI_CTRLR0_XFM_SHIFT;
  ctrlr0 |= RK3576_SPI_CTRLR0_DFS_8BITS << RK3576_SPI_CTRLR0_DFS_SHIFT;

  /* CPOL / CPHA */

  if (priv->mode == SPIDEV_MODE2 || priv->mode == SPIDEV_MODE3)
    {
      ctrlr0 |= RK3576_SPI_CTRLR0_SCPOL; /* SCPOL = 1 */
    }

  if (priv->mode == SPIDEV_MODE1 || priv->mode == SPIDEV_MODE3)
    {
      ctrlr0 |= RK3576_SPI_CTRLR0_SCPH; /* SCPH = 1 */
    }

  /* Chip-select mode: keep SS_N low across consecutive frames in a burst
   * (csm=00).  The NuttX SPI upper-half asserts SS via SPI_SELECT around a
   * whole exchange, so we want SS_N stable for the entire burst.
   */

  ctrlr0 |= RK3576_SPI_CTRLR0_CSM_KEEP_LOW << RK3576_SPI_CTRLR0_CSM_SHIFT;

  /* Enable APB-to-SPI 8-bit access (bht=1) so a 32-bit TXDR/RXDR access
   * carries 4 data frames like Linux's rockchip spi driver.
   */

  ctrlr0 |= RK3576_SPI_CTRLR0_BHT;

  spi_putreg(priv, RK3576_SPI_CTRLR0_OFFSET, ctrlr0);
}

/****************************************************************************
 * Name: rk3576_spi_setmode
 *
 * Description:
 *   Apply the SPI mode (CPOL/CPHA) immediately.  The DW SSI CTRLR0 register
 *   is read-only while the controller is enabled (SSIENR=1), so the
 *   controller is disabled around the write.  Applying in-place (rather than
 *   deferring to exchange) keeps the CPOL transition from happening while
 *   the chip-select is already asserted under software CS control, which
 *   would corrupt the bus.
 ****************************************************************************/

static void rk3576_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (priv->mode == mode)
    {
      return;
    }

  priv->mode = mode;

  /* Disable, write CTRLR0, re-enable. */

  spi_putreg(priv, RK3576_SPI_ENR_OFFSET, 0);
  rk3576_spi_setctr0(priv);
  spi_putreg(priv, RK3576_SPI_ENR_OFFSET, RK3576_SPI_ENR_EN);
}

/****************************************************************************
 * Name: rk3576_spi_setbaudr
 *
 * Description:
 *   Pick the source clock that best reproduces the requested frequency and
 *   program the DW SSI BAUDR divider.
 *
 *   The RK3576 SPI functional clock (clk_spiN) is a mux-only clock: the CRU
 *   only selects one of several fixed pll/osc sources (~200/100/50/24 MHz)
 *   and has no divider of its own (unlike FSPI).  The actual SCK is produced
 *   by the controller's 16-bit BAUDR divider, which the CLK framework does
 *   not model:
 *
 *     Fsclk_out = Fspi_clk / BAUDR,  BAUDR even, 2 <= BAUDR <= 65534.
 *
 *   Because BAUDR lives outside the CLK framework, a single clk_set_rate()
 *   cannot hit an arbitrary SCK -- it can only switch the coarse CRU source.
 *   The requested rate is therefore reached in two stages here:
 *     1. Walk the source mux parents (clk_get_parent_by_index, no string
 *        lookup) and, for each, find the smallest even BAUDR >= Fspi_clk/f,
 *        which yields the largest SCK that stays <= the requested rate
 *        (never exceed the request).
 *     2. Select the source that gives the closest (largest, non-overshoot)
 *        SCK and program its BAUDR.
 ****************************************************************************/

static void rk3576_spi_setbaudr(struct rk3576_spi_priv_s *priv)
{
  struct clk_s *mux;
  uint32_t freq = priv->frequency;
  uint32_t sr;
  uint32_t k;
  uint32_t actual;
  uint32_t best_baudr = 0;
  uint32_t best_actual = 0;
  int best_idx = -1;
  int i;

  /* Sizes track the SPI source mux (RK3576 has 4 pll/osc parents). */

  struct clk_s *srcs[RK3576_SPI_NUM_CLKSRC] = { NULL, NULL, NULL, NULL };
  uint32_t rates[RK3576_SPI_NUM_CLKSRC];
  int nsr; /* number of usable entries in srcs[]/rates[] */

  DEBUGASSERT(freq);
  DEBUGASSERT(priv->sclk);

  if (freq == 0 || priv->sclk == NULL)
    {
      return;
    }

  /* clk_spiN (priv->sclk) is a gate whose parent is the source mux
   * clk_spiN_sel.  Fetch it once; it is not cached in priv because this
   * path only runs on setfrequency() (low frequency).
   */

  mux = clk_get_parent(priv->sclk);
  if (mux == NULL)
    {
      return;
    }

  DEBUGASSERT(mux->num_parents <= RK3576_SPI_NUM_CLKSRC);

  /* Collect each source handle and its live rate once. */

  nsr = 0;
  for (i = 0; i < mux->num_parents; i++)
    {
      srcs[nsr] = clk_get_parent_by_index(mux, i);
      rates[nsr] = srcs[nsr] != NULL ? clk_get_rate(srcs[nsr]) : 0;
      if (srcs[nsr] != NULL && rates[nsr] != 0)
        {
          nsr++;
        }
    }

  if (nsr == 0)
    {
      /* No usable source clock at all -- nothing to configure. */
      spiwarn("SPI: No parent clock source available");
      return;
    }

  /* Smallest even BAUDR >= sr/freq -> actual SCK <= freq, never over.
   * k is the ceiling, bumped to even when it lands on an odd value.
   */

  for (i = 0; i < nsr; i++)
    {
      sr = rates[i];

      k = (sr + freq - 1) / freq; /* ceil(sr/freq) */
      if (k < 2)
        {
          k = 2;
        }
      else if (k & 1u)
        {
          k++; /* force even: BAUDR <= 65534 still holds unless k is huge */
        }

      if (k > RK3576_SPI_BAUDR_MAX)
        {
          /* Even at the greatest (largest even) divider this source runs
           * too fast to stay at or below the request -- skip it as an
           * overshoot candidate.
           */
          continue;
        }

      actual = sr / k;
      if (actual > best_actual)
        {
          best_actual = actual;
          best_baudr = k;
          best_idx = i;
        }
    }

  /* No source can reach the request without overshoot (the request is below
   * the slowest source / 65534).  Fall back to a fully-open divider on the
   * slowest source, which overshoots the least.
   */

  if (best_idx < 0)
    {
      best_idx = 0;
      for (i = 1; i < nsr; i++)
        {
          if (rates[i] < rates[best_idx])
            {
              best_idx = i;
            }
        }

      spiwarn("SPI: cannot reach %lu Hz without overshoot; using slowest\n",
              (unsigned long)freq);
      best_baudr = RK3576_SPI_BAUDR_MAX;
    }

  /* Switch to the chosen source */

  clk_set_parent(mux, srcs[best_idx]);

  spi_putreg(priv, RK3576_SPI_BAUDR_OFFSET, best_baudr);
}

/****************************************************************************
 * Name: rk3576_spi_tx_ready / rk3576_spi_rx_ready / rk3576_spi_tx_idle
 ****************************************************************************/

static inline bool rk3576_spi_tx_ready(struct rk3576_spi_priv_s *priv)
{
  return (spi_getreg(priv, RK3576_SPI_SR_OFFSET) & RK3576_SPI_SR_TFF) == 0;
}

static inline bool rk3576_spi_rx_ready(struct rk3576_spi_priv_s *priv)
{
  return (spi_getreg(priv, RK3576_SPI_SR_OFFSET) & RK3576_SPI_SR_RFE) == 0;
}

/* Wait until every byte pushed into the TX FIFO has been shifted out onto
 * the serial bus (TFE = TX FIFO empty).  Used as the tail of a transmit-only
 * transfer where there is no RX stream to synchronise on.
 */

static void rk3576_spi_tx_idle(struct rk3576_spi_priv_s *priv)
{
  uint32_t t;

  for (t = 0; t < RK3576_SPI_POLL_LIMIT; t++)
    {
      if (spi_getreg(priv, RK3576_SPI_SR_OFFSET) & RK3576_SPI_SR_TFE)
        {
          return;
        }
    }
}

/****************************************************************************
 * Name: rk3576_spi_exchange
 *
 * Description:
 *   Exchange nwords 8-bit frames over the wire.  The transfer is driven as a
 *   streaming pipeline: while there is free space in the TX FIFO keep feeding
 *   it, and while the RX FIFO holds data keep draining it, until every word
 *   has been pushed and every expected byte read back.  This keeps the, at
 *   most, 64-deep FIFOs busy without per-byte handshaking.
 ****************************************************************************/

static void rk3576_spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                                void *rxbuffer, size_t nwords)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  const uint8_t *tx = (const uint8_t *)txbuffer;
  uint8_t *rx = (uint8_t *)rxbuffer;
  size_t txoff = 0;
  size_t rxoff = 0;
  bool auto_cs = false;

  spiinfo("priv=%p tx=%p rx=%p nwords=%zu\n", priv, tx, rx, nwords);

  /* Without any SS_N enabled the DW SSI controller generates no SCLK, so the
   * RX FIFO never fills and the loop below would spin forever.  If the caller
   * did not select a chip-select (SER == 0), enable CS0 for the duration of
   * this transfer, and clear it again at the end.  Otherwise the caller owns
   * the CS state and we leave it untouched.
   */

  if (spi_getreg(priv, RK3576_SPI_SER_OFFSET) == 0)
    {
      spi_putreg(priv, RK3576_SPI_SER_OFFSET, RK3576_SPI_SER_SER(0));
      auto_cs = true;
    }

  while (txoff < nwords || rxoff < nwords)
    {
      /* Feed the TX FIFO as long as there is room. */

      while (txoff < nwords && rk3576_spi_tx_ready(priv))
        {
          uint8_t txb = (tx != NULL) ? tx[txoff] : 0xff;
          spi_putreg(priv, RK3576_SPI_TXDR_OFFSET, txb);
          txoff++;
        }

      /* Drain the RX FIFO as long as it holds data. */

      while (rxoff < nwords && rk3576_spi_rx_ready(priv))
        {
          uint8_t rxb =
              (uint8_t)(spi_getreg(priv, RK3576_SPI_RXDR_OFFSET) & 0xff);
          if (rx != NULL)
            {
              rx[rxoff] = rxb;
            }

          rxoff++;
        }
    }

  /* A transmit-only transfer has no RX stream to synchronise on, so make
   * sure the last bytes have actually left the shift register before the
   * upper-half deasserts the chip-select.
   */

  if (rx == NULL)
    {
      rk3576_spi_tx_idle(priv);
    }

  /* Clear the chip-select if we selected it automatically. */

  if (auto_cs)
    {
      spi_putreg(priv, RK3576_SPI_SER_OFFSET, 0);
    }
}

#ifndef CONFIG_SPI_EXCHANGE

/****************************************************************************
 * Name: rk3576_spi_sndblock
 ****************************************************************************/

static void rk3576_spi_sndblock(struct spi_dev_s *dev, const void *buffer,
                                size_t nwords)
{
  rk3576_spi_exchange(dev, buffer, NULL, nwords);
}

/****************************************************************************
 * Name: rk3576_spi_recvblock
 ****************************************************************************/

static void rk3576_spi_recvblock(struct spi_dev_s *dev, void *buffer,
                                 size_t nwords)
{
  rk3576_spi_exchange(dev, NULL, buffer, nwords);
}

#endif /* CONFIG_SPI_EXCHANGE */

/****************************************************************************
 * Name: rk3576_spi_send
 ****************************************************************************/

static uint32_t rk3576_spi_send(struct spi_dev_s *dev, uint32_t wd)
{
  uint8_t txb = (uint8_t)wd;
  uint8_t rx = 0;

  rk3576_spi_exchange(dev, &txb, &rx, 1);
  return rx;
}

/****************************************************************************
 * Name: rk3576_spi_lock
 ****************************************************************************/

static int rk3576_spi_lock(struct spi_dev_s *dev, bool lock)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (lock)
    {
      return nxmutex_lock(&priv->lock);
    }

  return nxmutex_unlock(&priv->lock);
}

/****************************************************************************
 * Name: rk3576_spi_select
 *
 * Description:
 *   Assert/deassert the slave-select line for the given devid.  Only the
 *   2-bit index portion of devid (SPIDEVID_INDEX) is used as the SS_N line
 *   (0 or 1 per controller).
 ****************************************************************************/

static void rk3576_spi_select(struct spi_dev_s *dev, uint32_t devid,
                              bool selected)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  uint32_t ser;

  uint32_t cs = SPIDEVID_INDEX(devid);

  DEBUGASSERT(cs < RK3576_SPI_NUM_CHIPSELECTS);

  if (cs >= RK3576_SPI_NUM_CHIPSELECTS)
    {
      cs = 0;
    }

  ser = spi_getreg(priv, RK3576_SPI_SER_OFFSET);
  if (selected)
    {
      ser |= RK3576_SPI_SER_SER(cs);
    }
  else
    {
      ser &= ~RK3576_SPI_SER_SER(cs);
    }

  spi_putreg(priv, RK3576_SPI_SER_OFFSET, ser);
}

/****************************************************************************
 * Name: rk3576_spi_setfrequency
 ****************************************************************************/

static uint32_t rk3576_spi_setfrequency(struct spi_dev_s *dev,
                                        uint32_t frequency)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  priv->frequency = frequency;
  rk3576_spi_setbaudr(priv);

  return priv->frequency;
}

/****************************************************************************
 * Name: rk3576_spi_setbits
 *
 * Description:
 *   Only 8-bit data frames are currently supported; clamp others to 8.
 ****************************************************************************/

static void rk3576_spi_setbits(struct spi_dev_s *dev, int nbits)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (nbits != 8)
    {
      spiwarn("WARNING: nbits=%d unsupported, using 8\n", nbits);
    }

  priv->nbits = 8;
}

/****************************************************************************
 * Name: rk3576_spi_registercallback
 ****************************************************************************/

static int rk3576_spi_registercallback(struct spi_dev_s *dev,
                                       spi_mediachange_t callback, void *arg)
{
  /* Media change callbacks are not supported. */

  return -ENOSYS;
}

/****************************************************************************
 * Private Data — SPI operations vtable
 ****************************************************************************/

static const struct spi_ops_s g_rk3576_spi_ops = {
  .lock = rk3576_spi_lock,
  .select = rk3576_spi_select,
  .setfrequency = rk3576_spi_setfrequency,
  .setmode = rk3576_spi_setmode,
  .setbits = rk3576_spi_setbits,
  .status = NULL, /* not implemented */
#ifdef CONFIG_SPI_CMDDATA
  .cmddata = NULL, /* not implemented */
#endif
  .send = rk3576_spi_send,
#ifdef CONFIG_SPI_EXCHANGE
  .exchange = rk3576_spi_exchange,
#else
  .sndblock = rk3576_spi_sndblock,
  .recvblock = rk3576_spi_recvblock,
#endif
  .registercallback = rk3576_spi_registercallback,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_spi_initialize
 ****************************************************************************/

FAR struct spi_dev_s *rk3576_spi_initialize(int bus)
{
  struct rk3576_spi_priv_s *priv;
  char name[32];
  int ret;

  if (bus < 0 || bus >= RK3576_SPI_NUM_CONTROLLERS)
    {
      spierr("ERROR: unsupported SPI bus %d\n", bus);
      return NULL;
    }

  priv = &g_rk3576_spi[bus];

  /* Take the per-bus mutex and re-check the flag (double-checked bring-up).
   * Holding priv->lock across the whole check/init/publish section makes it
   * atomic against a concurrent rk3576_spi_initialize() of the same bus from
   * another thread or, on SMP, another CPU -- only one caller ever runs the
   * one-time register/clock setup, the rest observe the flag set and return
   * the already-published handle.
   *
   * This is the same lock later used to serialize steady-state transfers
   * (rk3576_spi_lock).  It is statically initialized (head of this file) so
   * it is legal from before the scheduler starts.  All error paths fall
   * through to errout so the lock is always released before returning.
   */

  nxmutex_lock(&priv->lock);

  if (g_rk3576_spi_inited[bus])
    {
      nxmutex_unlock(&priv->lock);
      return &priv->dev;
    }

  /* Bring up PCLK (APB bus clock gate) */

  snprintf(name, sizeof(name), "pclk_spi%d", bus);
  priv->pclk = clk_get(name);
  if (!priv->pclk)
    {
      spierr("ERROR: SPI%d: failed to get clock %s\n", bus, name);
      goto err_lock;
    }

  ret = clk_enable(priv->pclk);
  if (ret < 0)
    {
      spierr("ERROR: SPI%d: failed to enable clock %s\n", bus, name);
      goto err_lock;
    }

  /* Bring up SCLK (serial functional clock) */

  snprintf(name, sizeof(name), "clk_spi%d", bus);
  priv->sclk = clk_get(name);
  if (!priv->sclk)
    {
      spierr("ERROR: SPI%d: failed to get clock %s\n", bus, name);
      goto err_pclk;
    }

  ret = clk_enable(priv->sclk);
  if (ret < 0)
    {
      spierr("ERROR: SPI%d: failed to enable clock %s\n", bus, name);
      goto err_pclk;
    }

  priv->dev.ops = &g_rk3576_spi_ops;
  priv->base = g_rk3576_spi_base[bus];
  priv->frequency = 1000000; /* 1 MHz default */
  priv->mode = SPIDEV_MODE0;
  priv->nbits = 8;

  /* priv->lock is already statically initialized; do NOT nxmutex_init() it
   * again here -- it is used by this function itself to serialize bring-up. */

  /* Disable the controller, then apply the safe default config. */

  spi_putreg(priv, RK3576_SPI_ENR_OFFSET, 0);

  /* Mask all interrupts (polled driver). */

  spi_putreg(priv, RK3576_SPI_IMR_OFFSET, 0);

  /* Set TX/RX FIFO thresholds for polled operation. */

  spi_putreg(priv, RK3576_SPI_TXFTLR_OFFSET, 0);
  spi_putreg(priv, RK3576_SPI_RXFTLR_OFFSET, 0);

  rk3576_spi_setctr0(priv);
  rk3576_spi_setbaudr(priv);

  /* Enable the controller. */

  spi_putreg(priv, RK3576_SPI_ENR_OFFSET, RK3576_SPI_ENR_EN);

  /* Publish: any later/locked-out caller now observes the bus brought up and
   * simply returns the handle below without repeating the setup. */

  g_rk3576_spi_inited[bus] = true;

  nxmutex_unlock(&priv->lock);
  return &priv->dev;

err_pclk:
  clk_disable(priv->pclk);
err_lock:
  /* Any partial bring-up failure. Also release the bus guard so a later
   * rk3576_spi_initialize() of this (or another) controller is not stuck. */

  nxmutex_unlock(&priv->lock);
  return NULL;
}

#endif /* CONFIG_RK3576_SPI */
