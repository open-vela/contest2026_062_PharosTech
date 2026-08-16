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
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_spi.h"
#include "rk3576_spi.h"

#ifdef CONFIG_RK3576_SPI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_SPI_FIFO_DEPTH 32      /* TX/RX FIFO depth (entries) */
#define RK3576_SPI_POLL_LIMIT 1000000 /* busy-wait iterations */

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
  mutex_t lock;         /* Serialize bus access */
  uint32_t frequency;   /* Configured SCLK frequency */
  enum spi_mode_e mode; /* SPI mode (CPOL/CPHA) */
  int nbits;            /* Bits per word (currently 8) */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_spi_priv_s g_rk3576_spi[RK3576_SPI_NUM_CONTROLLERS];
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
 * Name: rk3576_spi_setbaudr
 *
 * Description:
 *   Program BAUDR from the configured frequency.  Fsclk_out = Fspi_clk /
 *BAUDR, where BAUDR is an even value between 2 and 65534.
 ****************************************************************************/

static void rk3576_spi_setbaudr(struct rk3576_spi_priv_s *priv)
{
  uint32_t rate;
  uint32_t baudr;

  rate = clk_get_rate(priv->sclk);
  if (rate == 0 || priv->frequency == 0)
    {
      return;
    }

  /* Round to the nearest allowed BAUDR (must be even, >= 2). */

  baudr = rate / priv->frequency;
  if (baudr < 2)
    {
      baudr = 2;
    }

  baudr &= ~1u; /* Force even */

  spi_putreg(priv, RK3576_SPI_BAUDR_OFFSET, baudr & RK3576_SPI_BAUDR_MASK);
}

/****************************************************************************
 * Name: rk3576_spi_tx_ready / rk3576_spi_rx_ready / rk3576_spi_busy
 ****************************************************************************/

static inline bool rk3576_spi_tx_ready(struct rk3576_spi_priv_s *priv)
{
  return (spi_getreg(priv, RK3576_SPI_SR_OFFSET) & RK3576_SPI_SR_TFF) == 0;
}

static inline bool rk3576_spi_rx_ready(struct rk3576_spi_priv_s *priv)
{
  return (spi_getreg(priv, RK3576_SPI_SR_OFFSET) & RK3576_SPI_SR_RFE) == 0;
}

static inline bool rk3576_spi_busy(struct rk3576_spi_priv_s *priv)
{
  return (spi_getreg(priv, RK3576_SPI_SR_OFFSET) & RK3576_SPI_SR_BSF) != 0;
}

/****************************************************************************
 * Name: rk3576_spi_exchange_one
 *
 * Description:
 *   Exchange one byte (8-bit frame): push a TX byte (or 0 if no txbuffer),
 *   wait for RX data, optionally capture the received byte.
 ****************************************************************************/

static void rk3576_spi_exchange_one(struct rk3576_spi_priv_s *priv, uint8_t tx,
                                    uint8_t *rx)
{
  uint32_t t;

  /* Wait until TX FIFO has room */

  for (t = 0; t < RK3576_SPI_POLL_LIMIT && !rk3576_spi_tx_ready(priv); t++)
    {
    }

  spi_putreg(priv, RK3576_SPI_TXDR_OFFSET, tx);

  /* Wait until a RX data entry is available */

  for (t = 0; t < RK3576_SPI_POLL_LIMIT && !rk3576_spi_rx_ready(priv); t++)
    {
    }

  if (rx != NULL)
    {
      *rx = (uint8_t)(spi_getreg(priv, RK3576_SPI_RXDR_OFFSET) & 0xff);
    }
  else
    {
      spi_getreg(priv, RK3576_SPI_RXDR_OFFSET);
    }
}

/****************************************************************************
 * Name: rk3576_spi_exchange
 ****************************************************************************/

static void rk3576_spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                                void *rxbuffer, size_t nwords)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  const uint8_t *tx = (const uint8_t *)txbuffer;
  uint8_t *rx = (uint8_t *)rxbuffer;
  size_t i;

  spiinfo("priv=%p tx=%p rx=%p nwords=%zu\n", priv, tx, rx, nwords);

  for (i = 0; i < nwords; i++)
    {
      uint8_t txb = (tx != NULL) ? tx[i] : 0xff;
      uint8_t rxb;

      rk3576_spi_exchange_one(priv, txb, &rxb);

      if (rx != NULL)
        {
          rx[i] = rxb;
        }
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
  uint8_t rx;

  rk3576_spi_exchange_one((struct rk3576_spi_priv_s *)dev, (uint8_t)wd, &rx);
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
 * Name: rk3576_spi_setmode
 ****************************************************************************/

static void rk3576_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  priv->mode = mode;
  rk3576_spi_setctr0(priv);
}

/****************************************************************************
 * Name: rk3576_spi_setbits
 *
 * Description:
 *   Only 8-bit data frames are currently supported; silently clamp others
 *   to 8 and still reconfigure (the hardware supports 4/16-bit but this is
 *   not wired up).
 ****************************************************************************/

static void rk3576_spi_setbits(struct spi_dev_s *dev, int nbits)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (nbits != 8)
    {
      spiwarn("WARNING: nbits=%d unsupported, using 8\n", nbits);
    }

  priv->nbits = 8;
  rk3576_spi_setctr0(priv);
}

/****************************************************************************
 * Name: rk3576_spi_status
 ****************************************************************************/

static uint8_t rk3576_spi_status(struct spi_dev_s *dev, uint32_t devid)
{
  return SPI_STATUS_PRESENT;
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
  .status = rk3576_spi_status,
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

  if (!g_rk3576_spi_inited[bus])
    {
      /* Bring up PCLK (APB bus clock gate) */

      snprintf(name, sizeof(name), "pclk_spi%d", bus);
      priv->pclk = clk_get(name);
      if (!priv->pclk)
        {
          spierr("ERROR: SPI%d: failed to get clock %s\n", bus, name);
          return NULL;
        }

      ret = clk_enable(priv->pclk);
      if (ret < 0)
        {
          spierr("ERROR: SPI%d: failed to enable clock %s\n", bus, name);
          return NULL;
        }

      /* Bring up SCLK (serial functional clock) */

      snprintf(name, sizeof(name), "clk_spi%d", bus);
      priv->sclk = clk_get(name);
      if (!priv->sclk)
        {
          spierr("ERROR: SPI%d: failed to get clock %s\n", bus, name);
          clk_disable(priv->pclk);
          return NULL;
        }

      ret = clk_enable(priv->sclk);
      if (ret < 0)
        {
          spierr("ERROR: SPI%d: failed to enable clock %s\n", bus, name);
          clk_disable(priv->pclk);
          return NULL;
        }

      priv->dev.ops = &g_rk3576_spi_ops;
      priv->base = g_rk3576_spi_base[bus];
      priv->frequency = 1000000; /* 1 MHz default */
      priv->mode = SPIDEV_MODE0;
      priv->nbits = 8;

      nxmutex_init(&priv->lock);

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

      g_rk3576_spi_inited[bus] = true;
    }

  return &priv->dev;
}

#endif /* CONFIG_RK3576_SPI */
