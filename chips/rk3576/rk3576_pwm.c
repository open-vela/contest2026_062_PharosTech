/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pwm.c
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
 * RK3576 PWM (Rockchip PWM v4) lower-half driver.  Implements the NuttX
 * struct pwm_lowerhalf_s so the upper-half (drivers/pwm) can register a
 * /dev/pwmN device.  Continuous-mode output only (no capture, no one-shot
 * pulse count).  The counting clock is the fixed 24 MHz oscillator scaled
 * down by two (12 MHz), which gives a wide frequency range at good
 * resolution without touching a PLL.  Pin muxing is board-specific and is
 * performed by the board before rk3576_pwm_initialize() is called.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/timers/pwm.h>

#include "arm64_internal.h"
#include "hardware/rk3576_pwm.h"

#ifdef CONFIG_RK3576_PWM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Counting clock = osc (24 MHz) with prescale = 0 (/1) and scale = 1 (/2). */

#define RK3576_PWM_SCALE        1
#define RK3576_PWM_CLK_HZ       (RK3576_PWM_OSC_HZ / (2 * RK3576_PWM_SCALE))

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One PWM channel.  The first member is the lower-half ops pointer so a
 * struct pwm_lowerhalf_s pointer can be up-cast to this structure.
 */

struct rk3576_pwm_s
{
  const struct pwm_ops_s *ops;   /* Lower-half operations (must be first)  */
  uintptr_t               base;   /* Channel register base address          */
  bool                    started;/* Output currently running               */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_pwm_getreg(struct rk3576_pwm_s *priv, unsigned int off);
static void rk3576_pwm_putreg(struct rk3576_pwm_s *priv, unsigned int off,
                              uint32_t val);

static int  rk3576_pwm_setup(struct pwm_lowerhalf_s *dev);
static int  rk3576_pwm_shutdown(struct pwm_lowerhalf_s *dev);
static int  rk3576_pwm_start(struct pwm_lowerhalf_s *dev,
                             const struct pwm_info_s *info);
static int  rk3576_pwm_stop(struct pwm_lowerhalf_s *dev);
static int  rk3576_pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,
                             unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pwm_ops_s g_rk3576_pwm_ops =
{
  .setup    = rk3576_pwm_setup,
  .shutdown = rk3576_pwm_shutdown,
  .start    = rk3576_pwm_start,
  .stop     = rk3576_pwm_stop,
  .ioctl    = rk3576_pwm_ioctl,
};

/* One instance per PWM1 channel; only the channels a board actually uses
 * are handed out by rk3576_pwm_initialize().
 */

static struct rk3576_pwm_s g_rk3576_pwm[RK3576_PWM1_NCHAN];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_pwm_getreg(struct rk3576_pwm_s *priv, unsigned int off)
{
  return getreg32(priv->base + off);
}

static void rk3576_pwm_putreg(struct rk3576_pwm_s *priv, unsigned int off,
                              uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_pwm_setup
 *
 * Description:
 *   Ungate the PWM1 clocks (pclk + 24 MHz oscillator), select the
 *   oscillator as the counting-clock source and program the fixed scale.
 *   The controller register block is accessible once pclk is running.
 ****************************************************************************/

static int rk3576_pwm_setup(struct pwm_lowerhalf_s *dev)
{
  struct rk3576_pwm_s *priv = (struct rk3576_pwm_s *)dev;

  /* Ungate pclk_pwm1 + clk_osc_pwm1 (CRU gate bits are active-high-disable,
   * so write the target bits into the mask and 0 into the value.
   */

  putreg32(PWM_HIWORD_CLR(RK3576_PWM1_PCLK_BIT | RK3576_PWM1_OSCCLK_BIT),
           RK3576_PWM_CRU_BASE + RK3576_PWM_CRU_GATE16);

  /* Clock control: oscillator source, prescale = 0, scale = 1 (-> 12 MHz). */

  rk3576_pwm_putreg(priv, RK3576_PWM_CLK_CTRL,
                    PWM_HIWORD((PWM_CLK_SRC_OSC << PWM_CLK_SRC_SEL_SHIFT) |
                               (RK3576_PWM_SCALE << PWM_CLK_SCALE_SHIFT)));

  pwminfo("PWM setup base=%08" PRIxPTR " version=%08" PRIx32 "\n",
          priv->base, rk3576_pwm_getreg(priv, RK3576_PWM_VERSION));
  return OK;
}

/****************************************************************************
 * Name: rk3576_pwm_shutdown
 ****************************************************************************/

static int rk3576_pwm_shutdown(struct pwm_lowerhalf_s *dev)
{
  return rk3576_pwm_stop(dev);
}

/****************************************************************************
 * Name: rk3576_pwm_start
 *
 * Description:
 *   Program the period and duty for the requested frequency/duty and start
 *   continuous output.  frequency is in Hz; duty is a Q16 fraction
 *   (0..65536 maps to 0..100%).
 ****************************************************************************/

static int rk3576_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info)
{
  struct rk3576_pwm_s *priv = (struct rk3576_pwm_s *)dev;
  uint32_t period;
  uint32_t duty;

  if (info->frequency == 0)
    {
      return -EINVAL;
    }

  period = RK3576_PWM_CLK_HZ / info->frequency;
  if (period < 2)
    {
      pwmerr("ERROR: frequency %" PRIu32 " too high for %u Hz clock\n",
             info->frequency, RK3576_PWM_CLK_HZ);
      return -ERANGE;
    }

  duty = (uint32_t)(((uint64_t)period * info->duty) >> 16);

  /* Program period/duty, select continuous mode (active-high output), then
   * enable the clock + channel and latch the new settings (ctrl_update).
   */

  rk3576_pwm_putreg(priv, RK3576_PWM_PERIOD, period);
  rk3576_pwm_putreg(priv, RK3576_PWM_DUTY, duty);
  rk3576_pwm_putreg(priv, RK3576_PWM_CTRL,
                    PWM_HIWORD(PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_DUTY_POL));
  rk3576_pwm_putreg(priv, RK3576_PWM_ENABLE,
                    PWM_HIWORD(PWM_ENABLE_CLK_EN | PWM_ENABLE_EN |
                               PWM_ENABLE_CTRL_UPDATE));

  priv->started = true;
  pwminfo("start freq=%" PRIu32 " duty=%04x period=%" PRIu32 " dutyc=%"
          PRIu32 "\n", info->frequency, info->duty, period, duty);
  return OK;
}

/****************************************************************************
 * Name: rk3576_pwm_stop
 ****************************************************************************/

static int rk3576_pwm_stop(struct pwm_lowerhalf_s *dev)
{
  struct rk3576_pwm_s *priv = (struct rk3576_pwm_s *)dev;

  /* Clear pwm_en + clk_en (leaves the output at the inactive polarity). */

  rk3576_pwm_putreg(priv, RK3576_PWM_ENABLE,
                    PWM_HIWORD_CLR(PWM_ENABLE_EN | PWM_ENABLE_CLK_EN));
  priv->started = false;
  return OK;
}

/****************************************************************************
 * Name: rk3576_pwm_ioctl
 ****************************************************************************/

static int rk3576_pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,
                            unsigned long arg)
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
 * Name: rk3576_pwm_initialize
 *
 * Description:
 *   Return the lower-half handle for one PWM1 channel (0..5), for the board
 *   to pass to pwm_register().  The board is responsible for muxing the
 *   channel's output pin before the device is used.
 ****************************************************************************/

struct pwm_lowerhalf_s *rk3576_pwm_initialize(int channel)
{
  struct rk3576_pwm_s *priv;

  if (channel < 0 || channel >= RK3576_PWM1_NCHAN)
    {
      return NULL;
    }

  priv = &g_rk3576_pwm[channel];
  priv->ops     = &g_rk3576_pwm_ops;
  priv->base    = RK3576_PWM1_ADDR + channel * RK3576_PWM_CH_STRIDE;
  priv->started = false;

  return (struct pwm_lowerhalf_s *)priv;
}

#endif /* CONFIG_RK3576_PWM */
