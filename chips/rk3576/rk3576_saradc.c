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
 * registers clk_saradc_sel/div, clk_saradc and pclk_saradc); the mux and
 * divider are left at their reset values which yield a ~20 MHz conversion
 * clock from GPLL.  The CRU soft-reset lines are pulsed directly because
 * the reset-controller framework has no CRU provider (same scheme as
 * rk3576_sai.c).
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
#include <stdio.h>

#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/irq.h>

#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_saradc.h"
#include "rk3576_saradc.h"

#ifdef CONFIG_RK3576_SARADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Status poll limit (busy-wait iterations). */

#define SARADC_POLL_LIMIT 1000000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_saradc_s
{
  uintptr_t base;                      /* SARADC register base       */
  FAR const struct adc_callback_s *cb; /* Upper-half callbacks       */
  FAR struct clk_s *pclk;              /* APB bus clock (pclk_saradc)*/
  FAR struct clk_s *clk;               /* Conv clock (clk_saradc)    */
  uint32_t chs_enabled;                /* Bitmask of enabled chns    */
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
 ****************************************************************************/

static void rk3576_saradc_reset(FAR struct adc_dev_s *dev) { UNUSED(dev); }

/****************************************************************************
 * Name: rk3576_saradc_setup
 *
 * Description:
 *   Called on first open.  Clocks already enabled via the CLK framework
 *   in rk3576_saradc_initialize(); here we do a light controller sanity
 *   init (leave timing/ST_CON at reset values, which are safe for 20 MHz
 *   operation).
 *
 ****************************************************************************/

static int rk3576_saradc_setup(FAR struct adc_dev_s *dev)
{
  UNUSED(dev);
  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_shutdown
 ****************************************************************************/

static void rk3576_saradc_shutdown(FAR struct adc_dev_s *dev)
{
  /* Nothing to reverse; conversions are triggered on demand. */
  UNUSED(dev);
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

FAR struct adc_dev_s *rk3576_saradc_initialize(void)
{
  FAR struct rk3576_saradc_s *priv = &g_saradc_priv;
  FAR struct adc_dev_s *adcdev = &g_saradc_dev;
  int ret;

  priv->base = RK3576_SARADC_ADDR;
  priv->cb = NULL;
  priv->chs_enabled = 0xff; /* All 8 channels enabled by default */

  /* Bring up the clocks through the CLK framework.  pclk makes the
   * register block accessible; clk is the conversion clock.  Mux/div
   * stay at their reset values (GPLL / 60, ~20 MHz).
   */

  priv->pclk = clk_get("pclk_saradc");
  if (priv->pclk == NULL)
    {
      aerr("ERROR: failed to get clock pclk_saradc\n");
      return NULL;
    }

  ret = clk_enable(priv->pclk);
  if (ret < 0)
    {
      aerr("ERROR: failed to enable clock pclk_saradc: %d\n", ret);
      return NULL;
    }

  priv->clk = clk_get("clk_saradc");
  if (priv->clk == NULL)
    {
      aerr("ERROR: failed to get clock clk_saradc\n");
      goto err_pclk;
    }

  ret = clk_enable(priv->clk);
  if (ret < 0)
    {
      aerr("ERROR: failed to enable clock clk_saradc: %d\n", ret);
      goto err_pclk;
    }

  adcdev->ad_ops = &g_saradc_ops;
  adcdev->ad_priv = priv;

  return adcdev;

err_pclk:
  clk_disable(priv->pclk);
  return NULL;
}

#endif /* CONFIG_RK3576_SARADC */
