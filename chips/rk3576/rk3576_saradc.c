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
 * on ANIOC_TRIGGER it selects each enabled channel in turn, asserts
 * start_doc, waits for the conversion to complete (polling the STATUS
 * busy bit), reads SARADC_DATAn, and pushes the result up through the
 * upper-half callback.  End-of-conversion interrupts are optional and only
 * enabled while a conversion is outstanding.
 *
 * Clocks are obtained through the NuttX CLK framework (rk3576_clk_tree.c
 * registers clk_saradc_sel/div, clk_saradc and pclk_saradc).  The mux and
 * divider reset to GPLL / 60 (~20 MHz); the desired conversion clock rate
 * and the set of enabled channels are passed in by the caller to
 * rk3576_saradc_initialize().  Only the enabled channels are converted on
 * each ANIOC_TRIGGER, so channels the application does not use cost nothing.
 * The CRU soft-reset lines are pulsed directly because the reset-controller
 * framework has no CRU provider (same scheme as rk3576_sai.c).
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

#ifdef CONFIG_RK3576_SARADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Status poll limit (busy-wait iterations). */

#define SARADC_POLL_LIMIT 1000000

/* TRM Chapter 18 conversion-clock ceiling (Hz). */

#define SARADC_MAX_CLK 20000000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_saradc_s
{
  uintptr_t base;                      /* SARADC register base       */
  FAR const struct adc_callback_s *cb; /* Upper-half callbacks       */
  FAR struct clk_s *pclk;              /* APB bus clock (pclk_saradc)*/
  FAR struct clk_s *clk;               /* Conv clock (clk_saradc)    */
  uint8_t chs_enabled;                 /* Bitmask of enabled chns    */
  uint32_t clk_rate;                   /* Desired conv clock (Hz)    */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t saradc_getreg(struct rk3576_saradc_s *priv, unsigned int off);
static void saradc_putreg(struct rk3576_saradc_s *priv, unsigned int off,
                          uint32_t val);

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

/* Static allocation: RK3576 has only one SARADC peripheral. */

static struct rk3576_saradc_s g_saradc_priv;
static struct adc_dev_s g_saradc_dev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t saradc_getreg(struct rk3576_saradc_s *priv,
                                     unsigned int off)
{
  return getreg32(priv->base + off);
}

static inline void saradc_putreg(struct rk3576_saradc_s *priv,
                                 unsigned int off, uint32_t val)
{
  putreg32(val, priv->base + off);
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

static int rk3576_saradc_trigger(struct rk3576_saradc_s *priv, uint8_t ch,
                                 FAR uint16_t *result)
{
  uint32_t reg;
  uint32_t data_off;
  int t;

  if (ch >= RK3576_SARADC_NCHANNELS)
    {
      return -EINVAL;
    }

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
  saradc_putreg(priv, RK3576_SARADC_CONV_CON, reg);

  /* Wait for the FSM to finish (conv_st -> 0). */

  for (t = 0; t < SARADC_POLL_LIMIT; t++)
    {
      if ((saradc_getreg(priv, RK3576_SARADC_STATUS) & SARADC_STATUS_BUSY) ==
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
  *result = (uint16_t)(saradc_getreg(priv, data_off) & SARADC_DATA_MASK);

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
 * Name: rk3576_saradc_reset
 *
 * Description:
 *   Pulse the SARADC CRU soft-reset lines (presetn/resetn_saradc) directly.
 *   The reset-controller framework has no CRU provider, so this follows the
 *   same scheme as rk3576_sai.c: assert both per-instance resets via
 *   CRU_SOFTRST_CON13, wait, then deassert.  This returns the controller to
 *   its reset defaults.  No SARADC register is touched here (the APB block
 *   may not be clocked yet at registration time); any controller state
 *   cleanup that depends on pclk happens in ao_setup().
 *
 ****************************************************************************/

static void rk3576_saradc_reset(FAR struct adc_dev_s *dev)
{
  uintptr_t cru_sofrst =
      RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_CRU_SARADC_RESET_CON);
  uint32_t rst_mask = (1u << RK3576_CRU_SARADC_PRESETN_BIT) |
                      (1u << RK3576_CRU_SARADC_RESETN_BIT);

  UNUSED(dev);

  /* Assert both reset lines (hiword-mask: wren + data = 1). */

  putreg32((rst_mask << 16) | rst_mask, cru_sofrst);
  up_udelay(20);

  /* Deassert (hiword-mask: wren = 1, data = 0). */

  putreg32(rst_mask << 16, cru_sofrst);
  up_udelay(20);
}

/****************************************************************************
 * Name: rk3576_saradc_setup
 *
 * Description:
 *   Called on first open.  Enable the SARADC clocks (pclk + conversion
 *   clock), clamp the conversion clock to CONFIG_RK3576_SARADC_CLK_RATE,
 *   and restore the controller to single-conversion defaults, since the
 *   CRU soft-reset in ao_reset() runs at registration time before the
 *   SARADC block is clocked.  Timing/ST_CON are left at their reset values,
 *   which are safe for <= 20 MHz operation.
 *
 ****************************************************************************/

static int rk3576_saradc_setup(FAR struct adc_dev_s *dev)
{
  FAR struct rk3576_saradc_s *priv =
      (FAR struct rk3576_saradc_s *)dev->ad_priv;
  int ret;

  ret = clk_enable(priv->pclk);
  if (ret < 0)
    {
      aerr("ERROR: failed to enable clock pclk_saradc: %d\n", ret);
      return ret;
    }

  ret = clk_enable(priv->clk);
  if (ret < 0)
    {
      aerr("ERROR: failed to enable clock clk_saradc: %d\n", ret);
      clk_disable(priv->pclk);
      return ret;
    }

  /* Clamp the conversion clock to the requested rate (<= 20 MHz per
   * TRM Chapter 18).  GPLL is read-only upstream, so clk_set_rate() only
   * tunes clk_saradc_div and has no side effects on shared PLL sources.
   */

  ret = clk_set_rate(priv->clk, priv->clk_rate);
  if (ret < 0)
    {
      aerr("ERROR: failed to set clk_saradc to %u Hz: %d\n", priv->clk_rate,
           ret);
      clk_disable(priv->clk);
      clk_disable(priv->pclk);
      return ret;
    }

  /* Explicitly restore controller state to single-conversion defaults.
   * CONV_CON bits [0..7] are hiword-masked: write-enable them all while
   * clearing auto_channel_mode (bit6) and end_conv (bit7), along with
   * channel_sel/start/single_pd, so no stale mode or pending conversion
   * survives.  Disable the end-of-conversion interrupt (END_INT_EN bit0).
   */

  putreg32((0xffu << 16), priv->base + RK3576_SARADC_CONV_CON);
  putreg32((1u << 16), priv->base + RK3576_SARADC_END_INT_EN);

  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_shutdown
 *
 * Description:
 *   Called on the last close.  Reverse ao_setup(): gate off the conversion
 *   clock and pclk so the peripheral does not draw power while unopened.
 *
 ****************************************************************************/

static void rk3576_saradc_shutdown(FAR struct adc_dev_s *dev)
{
  FAR struct rk3576_saradc_s *priv =
      (FAR struct rk3576_saradc_s *)dev->ad_priv;

  clk_disable(priv->clk);
  clk_disable(priv->pclk);
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
 * Name: rk3576_saradc_read_channels
 *
 * Description:
 *   Trigger a single conversion on each enabled channel and deliver the
 *   result through the upper-half receive callback.
 *
 ****************************************************************************/

static int rk3576_saradc_read_channels(FAR struct adc_dev_s *dev,
                                       FAR struct rk3576_saradc_s *priv)
{
  uint16_t result;
  int ret;

  for (uint8_t i = 0; i < RK3576_SARADC_NCHANNELS; i++)
    {
      if ((priv->chs_enabled & (1u << i)) == 0)
        {
          continue;
        }

      ret = rk3576_saradc_trigger(priv, i, &result);
      if (ret < 0)
        {
          return ret;
        }

      if (priv->cb != NULL && priv->cb->au_receive != NULL)
        {
          ret = priv->cb->au_receive(dev, i, (int32_t)result);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return OK;
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
        return rk3576_saradc_read_channels(dev, priv);

      case ANIOC_GET_NCHANNELS:
        ret = RK3576_SARADC_NCHANNELS;
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

FAR struct adc_dev_s *rk3576_saradc_initialize(uint8_t channel_mask,
                                               uint32_t clk_rate)
{
  FAR struct rk3576_saradc_s *priv = &g_saradc_priv;
  FAR struct adc_dev_s *adcdev = &g_saradc_dev;

  /* Validate the conversion clock rate up front so a misconfigured caller
   * fails cleanly instead of mid-conversion.  channel_mask is a uint8_t, so
   * it can only ever address the 8 physical channels.
   */

  if (clk_rate < 1 || clk_rate > SARADC_MAX_CLK)
    {
      aerr("ERROR: SARADC clk_rate %u out of range [1, %d] Hz\n", clk_rate,
           SARADC_MAX_CLK);
      return NULL;
    }

  priv->base = RK3576_SARADC_ADDR;
  priv->cb = NULL;
  priv->chs_enabled = channel_mask;
  priv->clk_rate = clk_rate;

  /* Resolve the SARADC clock handles through the CLK framework.  The
   * clocks themselves are gated on in ao_setup() and off in ao_shutdown()
   * so the peripheral draws no power while unopened.  The conversion clock
   * rate is applied in ao_setup() to priv->clk_rate.
   */

  priv->pclk = clk_get("pclk_saradc");
  if (priv->pclk == NULL)
    {
      aerr("ERROR: failed to get clock pclk_saradc\n");
      return NULL;
    }

  priv->clk = clk_get("clk_saradc");
  if (priv->clk == NULL)
    {
      aerr("ERROR: failed to get clock clk_saradc\n");
      return NULL;
    }

  adcdev->ad_ops = &g_saradc_ops;
  adcdev->ad_priv = priv;

  return adcdev;
}

#endif /* CONFIG_RK3576_SARADC */
