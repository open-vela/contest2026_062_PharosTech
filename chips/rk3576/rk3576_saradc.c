/****************************************************************************
 * chips/rk3576/rk3576_saradc.c
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
 * RK3576 SAR-ADC (SARADC) lower-half driver.  Implements the NuttX
 * struct adc_ops_s (drivers/analog/adc.c upper-half) and binds a
 * /dev/adcN character device.
 *
 * The SARADC is an 8-channel single-ended 12-bit SAR A/D converter (TRM
 * Chapter 18).  This driver uses the controller's single conversion mode:
 * on ANIOC_TRIGGER it selects the device's channel, asserts start_doc,
 * waits for the conversion to complete (polling the STATUS busy bit), reads
 * SARADC_DATAn, and pushes the result up through the upper-half callback.
 * End-of-conversion interrupts are optional and only enabled while a
 * conversion is outstanding.
 *
 * Each of the 8 channels is exposed as an *independent* adc_dev_s with its
 * own receive FIFO, so multiple consumer processes can read different
 * channels without stepping on each other's data.  Underneath, however,
 * they all share a single SARADC controller core (one set of registers and
 * one clk_saradc / pclk_saradc): the hardware converts channels one at a
 * time, so the conversion path is serialised with a core mutex.
 *
 * Clocks are obtained through the NuttX CLK framework (rk3576_clk_tree.c
 * registers clk_saradc_sel/div, clk_saradc and pclk_saradc).  The desired
 * conversion clock rate is fixed at build time via
 * CONFIG_RK3576_SARADC_CLK_RATE.  The driver keeps its own reference count
 * (core->users) rather than relying on the CLK framework's: on the 0 -> 1
 * transition the clocks are enabled, the rate is applied, and the CRU
 * soft-reset is pulsed; on 1 -> 0 the clocks are gated off.  The soft-reset
 * is pulsed directly because the reset-controller framework has no CRU
 * provider (same scheme as rk3576_sai.c).  After a clock gate-off the FSM is
 * in an undefined state, so the re-enable path always re-asserts the reset.
 *
 * Pin muxing is the board's responsibility.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/irq.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_saradc.h"
#include "rk3576_saradc.h"

#include <nuttx/mutex.h>

#ifdef CONFIG_RK3576_SARADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Status poll limit (busy-wait iterations). */

#define SARADC_POLL_LIMIT 1000000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Shared SARADC controller core.  All 8 channels share a single controller
 * (and thus a single clk_saradc / pclk_saradc): the clock handles, the
 * conversion clock rate, and the CRU soft-reset are owned here.  The core
 * tracks its own reference count (users) so the clocks are enabled and the
 * soft-reset is asserted exactly on the 0 -> 1 transition, and the clocks
 * are disabled again on the 1 -> 0 transition, independent of the CLK
 * framework's internal counting.  Conversion of the actual channels is
 * serialised with core->lock because the hardware converts one channel at a
 * time.
 */

struct rk3576_saradc_core_s
{
  mutex_t lock;           /* Serialises the SARADC registers   */
  uintptr_t base;         /* SARADC register base              */
  FAR struct clk_s *pclk; /* APB bus clock (pclk_saradc)       */
  FAR struct clk_s *clk;  /* Conv clock (clk_saradc)           */
  uint32_t clk_rate;      /* Conv clock rate (build-time fixed)*/
  uint8_t users;          /* Open-channel reference count      */
};

/* Per-channel adc_dev_s private state. */

struct rk3576_saradc_s
{
  FAR struct rk3576_saradc_core_s *core; /* Shared controller core          */
  FAR const struct adc_callback_s *cb;   /* Upper-half callbacks            */
  uint8_t ch;                            /* This device's channel (0..7)    */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t saradc_getreg(FAR struct rk3576_saradc_core_s *core,
                              unsigned int off);
static void saradc_putreg(FAR struct rk3576_saradc_core_s *core,
                          unsigned int off, uint32_t val);

static int rk3576_saradc_bind(FAR struct adc_dev_s *dev,
                              FAR const struct adc_callback_s *callback);
static void rk3576_saradc_reset(FAR struct adc_dev_s *dev);
static int rk3576_saradc_setup(FAR struct adc_dev_s *dev);
static void rk3576_saradc_shutdown(FAR struct adc_dev_s *dev);
static void rk3576_saradc_rxint(FAR struct adc_dev_s *dev, bool enable);
static int rk3576_saradc_ioctl(FAR struct adc_dev_s *dev, int cmd,
                               unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct adc_ops_s g_saradc_ops = {
  .ao_bind = rk3576_saradc_bind,
  .ao_reset = rk3576_saradc_reset,
  .ao_setup = rk3576_saradc_setup,
  .ao_shutdown = rk3576_saradc_shutdown,
  .ao_rxint = rk3576_saradc_rxint,
  .ao_ioctl = rk3576_saradc_ioctl,
};

/* Static allocation: RK3576 has only one SARADC controller, but each of the
 * 8 channels is exposed as an independent adc_dev_s with its own FIFO.  All
 * channels share the single g_saradc_core.
 */

static struct rk3576_saradc_core_s g_saradc_core = { .lock =
                                                         NXMUTEX_INITIALIZER };

static struct rk3576_saradc_s g_saradc_chan[RK3576_SARADC_NCHANNELS];
static struct adc_dev_s g_saradc_dev[RK3576_SARADC_NCHANNELS];
static bool g_saradc_claimed[RK3576_SARADC_NCHANNELS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t saradc_getreg(FAR struct rk3576_saradc_core_s *core,
                                     unsigned int off)
{
  return getreg32(core->base + off);
}

static inline void saradc_putreg(FAR struct rk3576_saradc_core_s *core,
                                 unsigned int off, uint32_t val)
{
  putreg32(val, core->base + off);
}

/****************************************************************************
 * Name: rk3576_saradc_trigger
 *
 * Description:
 *   Start a single conversion on one channel and return the 12-bit result.
 *   Configures CONV_CON for single mode, asserts start_adc, polls the
 *   STATUS busy bit until idle, then reads the channel data register.
 *
 ****************************************************************************/

static int rk3576_saradc_trigger(FAR struct rk3576_saradc_core_s *core,
                                 uint8_t ch, FAR uint16_t *result)
{
  uint32_t reg;
  uint32_t data_off;
  int t;

  /* Clear the sticky end-of-conversion status bit (W1C) *before* starting,
   * so the completion test below cannot be fooled by a conversion that
   * finished while we were still setting up.
   */

  saradc_putreg(core, RK3576_SARADC_END_INT_ST, 1u << SARADC_END_INT_ST_BIT);

  /* Select channel and enable single-PD mode, then start.  All fields used
   * here sit in the lower 16 bits with their write-enable bits set in the
   * upper 16 (hiword-mask scheme): bits [3:0] channel_sel, bit4 start_adc,
   * bit5 single_pd_mode.
   */

  reg = (SARADC_CONV_CHSEL_MASK << SARADC_CONV_EN_SHIFT) | /* wren [3:0] */
        (SARADC_CONV_START << SARADC_CONV_EN_SHIFT) |      /* wren bit4 */
        (SARADC_CONV_SINGLE_PD << SARADC_CONV_EN_SHIFT) |  /* wren bit5 */
        (uint32_t)ch |                                     /* channel_sel */
        SARADC_CONV_START | SARADC_CONV_SINGLE_PD;
  saradc_putreg(core, RK3576_SARADC_CONV_CON, reg);

  /* single_pd_mode asserts PD after each conversion, so every trigger here
   * is a power-up sequence: PD is deasserted, the SAR CDAC/reference must
   * settle (~1 us per TRM 18.5.1), then SOC is asserted and the FSM runs.
   *
   * Do NOT use a two-phase "wait for conv_st == 1 then conv_st == 0" poll:
   * at the 20 MHz conversion clock a 12-bit conversion can complete before
   * the first STATUS read, so conv_st may already have returned to 0 and the
   * loop would falsely report "conversion never started".  Instead, wait a
   * fixed lower-bound delay (>= the PD deassert + CDAC settle time) so the
   * FSM has definitely started, then do a single-phase busy-until-idle poll.
   */

  up_udelay(2);

  for (t = 0; t < SARADC_POLL_LIMIT; t++)
    {
      if ((saradc_getreg(core, RK3576_SARADC_STATUS) & SARADC_STATUS_BUSY) ==
          0)
        {
          break;
        }
    }

  if (t >= SARADC_POLL_LIMIT)
    {
      aerr("SARADC: conversion timeout on channel %u\n", ch);
      return -ETIMEDOUT;
    }

  /* Read the channel data register (12-bit, right-aligned). */

  data_off = RK3576_SARADC_DATA0 + (unsigned int)ch * 4;
  *result = (uint16_t)(saradc_getreg(core, data_off) & SARADC_DATA_MASK);

  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_bind
 ****************************************************************************/

static int rk3576_saradc_bind(FAR struct adc_dev_s *dev,
                              FAR const struct adc_callback_s *callback)
{
  FAR struct rk3576_saradc_s *priv =
      (FAR struct rk3576_saradc_s *)dev->ad_priv;

  DEBUGASSERT(priv != NULL);
  priv->cb = callback;
  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_core_reset
 *
 * Description:
 *   Pulse the SARADC CRU soft-reset lines (presetn/resetn_saradc) directly.
 *   The reset-controller framework has no CRU provider, so this follows the
 *   same scheme as rk3576_sai.c: assert both per-instance resets via
 *   CRU_SOFTRST_CON13, wait, then deassert.  This returns the controller to
 *   its reset defaults.  The caller must hold core->lock and must have the
 *   SARADC pclk enabled first (the APB block is not otherwise clocked).
 *
 ****************************************************************************/

static void rk3576_saradc_core_reset(FAR struct rk3576_saradc_core_s *core)
{
  uintptr_t cru_sofrst =
      RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_CRU_SARADC_RESET_CON);
  uint32_t rst_mask = (1u << RK3576_CRU_SARADC_PRESETN_BIT) |
                      (1u << RK3576_CRU_SARADC_RESETN_BIT);

  /* Assert both reset lines (hiword-mask: wren + data = 1). */

  putreg32((rst_mask << 16) | rst_mask, cru_sofrst);
  up_udelay(20);

  /* Deassert (hiword-mask: wren = 1, data = 0). */

  putreg32(rst_mask << 16, cru_sofrst);
  up_udelay(20);
}

/****************************************************************************
 * Name: rk3576_saradc_reset
 *
 * Description:
 *   ao_reset callback.  The SARADC controller is shared by all channels, so
 *   the CRU soft-reset must run exactly once per core power-up, on the
 *   core->users 0 -> 1 transition.  The actual soft-reset also requires
 *   pclk, which is only enabled later in ao_setup(); therefore the pulse is
 *   deferred to ao_setup() and this callback is a no-op (kept for the
 *   upper-half contract).
 *
 ****************************************************************************/

static void rk3576_saradc_reset(FAR struct adc_dev_s *dev) { UNUSED(dev); }

/****************************************************************************
 * Name: rk3576_saradc_setup
 *
 * Description:
 *   Called on open of a channel.  Bump the core's own reference count; on
 *   the 0 -> 1 transition enable the SARADC clocks (pclk + conversion
 *   clock), set the build-time conversion rate, pulse the CRU soft-reset,
 *   and restore the controller to single-conversion defaults.  Later opens
 *   (users > 1) only bump the count: the clocks were never gated off, so no
 *   re-reset is needed.  All reference-count transitions happen under
 *   core->lock.
 *
 ****************************************************************************/

static int rk3576_saradc_setup(FAR struct adc_dev_s *dev)
{
  FAR struct rk3576_saradc_s *priv =
      (FAR struct rk3576_saradc_s *)dev->ad_priv;
  FAR struct rk3576_saradc_core_s *core = priv->core;
  int ret;

  ret = nxmutex_lock(&core->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (core->users == 0)
    {
      /* Core is being powered up: enable the clocks, then set the rate and
       * pulse the soft-reset once the APB clock is running, because the FSM
       * is guaranteed to be in an undefined state after a clock gate-off.
       */

      ret = clk_enable(core->pclk);
      if (ret < 0)
        {
          aerr("ERROR: failed to enable clock pclk_saradc: %d\n", ret);
          goto out;
        }

      ret = clk_enable(core->clk);
      if (ret < 0)
        {
          aerr("ERROR: failed to enable clock clk_saradc: %d\n", ret);
          clk_disable(core->pclk);
          goto out;
        }

      /* Clamp the conversion clock to the build-time rate (<= 20 MHz per
       * TRM Chapter 18).  GPLL is read-only upstream, so clk_set_rate()
       * only tunes clk_saradc_div and has no side effects on shared PLL
       * sources.  The core lock also serialises this against the conversion
       * path, so set_rate/reset never race a conversion.
       */

      ret = clk_set_rate(core->clk, core->clk_rate);
      if (ret < 0)
        {
          aerr("ERROR: failed to set clk_saradc to %u Hz: %d\n",
               core->clk_rate, ret);
          clk_disable(core->clk);
          clk_disable(core->pclk);
          goto out;
        }

      /* Pulse the soft-reset and restore the controller to single-conversion
       * defaults.  CONV_CON bits [0..7] are hiword-masked: write-enable them
       * all while clearing auto_channel_mode (bit6) and end_conv (bit7),
       * along with channel_sel/start/single_pd, so no stale mode or pending
       * conversion survives.  Disable the end-of-conversion interrupt.
       */

      rk3576_saradc_core_reset(core);

      putreg32((0xffu << 16), core->base + RK3576_SARADC_CONV_CON);
      putreg32((1u << 16), core->base + RK3576_SARADC_END_INT_EN);
    }

  core->users++;

out:
  nxmutex_unlock(&core->lock);

  return ret;
}

/****************************************************************************
 * Name: rk3576_saradc_shutdown
 *
 * Description:
 *   Called on close of a channel.  Drop the core's own reference count; on
 *   the 1 -> 0 transition gate off the conversion clock and pclk.  Sibling
 *   channels still open (users > 0) leave the clocks running.
 *
 ****************************************************************************/

static void rk3576_saradc_shutdown(FAR struct adc_dev_s *dev)
{
  FAR struct rk3576_saradc_s *priv =
      (FAR struct rk3576_saradc_s *)dev->ad_priv;
  FAR struct rk3576_saradc_core_s *core = priv->core;

  nxmutex_lock(&core->lock);

  DEBUGASSERT(core->users > 0);

  core->users--;

  if (core->users == 0)
    {
      /* Last channel closed: gate off the conversion clock and pclk.  The
       * next open will re-enable them and re-assert the soft-reset.
       */

      clk_disable(core->clk);
      clk_disable(core->pclk);
    }

  nxmutex_unlock(&core->lock);
}

/****************************************************************************
 * Name: rk3576_saradc_rxint
 *
 * Description:
 *   Enable/disable the end-of-conversion interrupt.  The driver converts on
 *   demand (polled), so this remains a no-op; kept for upper-half contract.
 *
 ****************************************************************************/

static void rk3576_saradc_rxint(FAR struct adc_dev_s *dev, bool enable)
{
  UNUSED(dev);
  UNUSED(enable);
}

/****************************************************************************
 * Name: rk3576_saradc_read_channel
 *
 * Description:
 *   Trigger a single conversion on this device's channel and deliver the
 *   result through the upper-half receive callback.  The whole trigger is
 *   serialised under core->lock because the controller converts channels
 *   one at a time.
 *
 ****************************************************************************/

static int rk3576_saradc_read_channel(FAR struct adc_dev_s *dev,
                                      FAR struct rk3576_saradc_s *priv)
{
  uint16_t result;
  int ret;

  ret = nxmutex_lock(&priv->core->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_saradc_trigger(priv->core, priv->ch, &result);

  nxmutex_unlock(&priv->core->lock);

  if (ret < 0)
    {
      return ret;
    }

  if (priv->cb != NULL && priv->cb->au_receive != NULL)
    {
      ret = priv->cb->au_receive(dev, priv->ch, (int32_t)result);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_saradc_ioctl
 ****************************************************************************/

static int rk3576_saradc_ioctl(FAR struct adc_dev_s *dev, int cmd,
                               unsigned long arg)
{
  FAR struct rk3576_saradc_s *priv =
      (FAR struct rk3576_saradc_s *)dev->ad_priv;
  int ret;

  switch (cmd)
    {
      case ANIOC_TRIGGER:
        return rk3576_saradc_read_channel(dev, priv);

      case ANIOC_GET_NCHANNELS:
        ret = 1;
        break;

      default:
        aerr("SARADC: unrecognized ioctl 0x%x\n", cmd);
        ret = -EINVAL;
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_saradc_initialize
 ****************************************************************************/

FAR struct adc_dev_s *rk3576_saradc_initialize(enum rk3576_saradc_ch_e channel)
{
  FAR struct rk3576_saradc_core_s *core = &g_saradc_core;
  FAR struct rk3576_saradc_s *priv;
  FAR struct adc_dev_s *adcdev;

  /* Validate the channel up front so a misconfigured caller fails cleanly. */

  if (channel < RK3576_SARADC_CH0 || channel > RK3576_SARADC_CH7)
    {
      aerr("ERROR: SARADC invalid channel %d\n", (int)channel);
      return NULL;
    }

  /* Mutual exclusion: each channel may be initialised at most once.  This
   * both prevents a duplicate adc_dev_s racing on the same channel's data
   * register, and serialises the one-time core setup against concurrent
   * initialisation of sibling channels during bring-up.
   */

  nxmutex_lock(&core->lock);

  if (g_saradc_claimed[channel])
    {
      nxmutex_unlock(&core->lock);
      aerr("ERROR: SARADC channel %d already initialised\n", (int)channel);
      return NULL;
    }

  /* One-time core setup: resolve the clock handles and stash the build-time
   * conversion rate.  This runs only on the first channel registration
   * (core->base is the sentinel).  The clocks themselves are enabled/disabled
   * in ao_setup()/ao_shutdown() on the core->users 0 <-> non-0 transitions,
   * at which point the rate is applied and the soft-reset is pulsed.
   */

  if (core->base == 0)
    {
      FAR struct clk_s *pclk;
      FAR struct clk_s *clk;

      /* Resolve *all* resources into locals first; only commit the core
       * state once every resource has been obtained successfully.  Writing
       * core->base (the "initialised" sentinel) or any clock handle before
       * the others would leave a partially-initialised core visible to
       * sibling channels if a later clk_get() failed, so never touch core
       * state on a failure path here.
       */

      pclk = clk_get("pclk_saradc");
      if (pclk == NULL)
        {
          nxmutex_unlock(&core->lock);
          aerr("ERROR: failed to get clock pclk_saradc\n");
          return NULL;
        }

      clk = clk_get("clk_saradc");
      if (clk == NULL)
        {
          nxmutex_unlock(&core->lock);
          aerr("ERROR: failed to get clock clk_saradc\n");
          return NULL;
        }

      core->base = RK3576_SARADC_ADDR;
      core->clk_rate = CONFIG_RK3576_SARADC_CLK_RATE;
      core->pclk = pclk;
      core->clk = clk;
    }

  priv = &g_saradc_chan[channel];
  adcdev = &g_saradc_dev[channel];

  priv->core = core;
  priv->cb = NULL;
  priv->ch = (uint8_t)channel;

  adcdev->ad_ops = &g_saradc_ops;
  adcdev->ad_priv = priv;

  g_saradc_claimed[channel] = true;

  nxmutex_unlock(&core->lock);

  return adcdev;
}

#endif /* CONFIG_RK3576_SARADC */
