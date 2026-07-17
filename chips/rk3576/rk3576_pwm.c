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

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/timers/pwm.h>

#include "arm64_internal.h"
#include "hardware/rk3576_pwm.h"
#include "rk3576_cru.h"

#ifdef CONFIG_RK3576_PWM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Static per-controller descriptor — base address and channel count. */

struct rk3576_pwm_ctrl_desc_s
{
  uintptr_t base_addr; /* Controller register base (PWMn_ADDR)      */
  unsigned int nchan;  /* Number of channels                        */
};

/* One PWM channel.  The first member is the lower-half ops pointer so a
 * struct pwm_lowerhalf_s pointer can be up-cast to this structure.
 */

struct rk3576_pwm_s
{
  const struct pwm_ops_s *ops; /* Lower-half operations (must be first) */
  uintptr_t base;              /* Channel register base address          */
  unsigned int ctrl;           /* Parent controller index (0..2)         */
  bool started;                /* Output currently running              */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_pwm_getreg(struct rk3576_pwm_s *priv, unsigned int off);
static void rk3576_pwm_putreg(struct rk3576_pwm_s *priv, unsigned int off,
                              uint32_t val);

static int rk3576_pwm_setup(struct pwm_lowerhalf_s *dev);
static int rk3576_pwm_shutdown(struct pwm_lowerhalf_s *dev);
static int rk3576_pwm_start(struct pwm_lowerhalf_s *dev,
                            const struct pwm_info_s *info);
static int rk3576_pwm_stop(struct pwm_lowerhalf_s *dev);
static int rk3576_pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,
                            unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pwm_ops_s g_rk3576_pwm_ops = {
  .setup = rk3576_pwm_setup,
  .shutdown = rk3576_pwm_shutdown,
  .start = rk3576_pwm_start,
  .stop = rk3576_pwm_stop,
  .ioctl = rk3576_pwm_ioctl,
};

/* Per-controller static descriptor table (PWM0, PWM1, PWM2). */

static const struct rk3576_pwm_ctrl_desc_s g_pwm_ctrls[RK3576_PWM_NCTRL] =
{
  [RK3576_PWM0] =
  {
    .base_addr     = RK3576_PWM0_ADDR,
    .nchan         = 2,
  },
  [RK3576_PWM1] =
  {
    .base_addr     = RK3576_PWM1_ADDR,
    .nchan         = 6,
  },
  [RK3576_PWM2] =
  {
    .base_addr     = RK3576_PWM2_ADDR,
    .nchan         = 8,
  },
};

/* Per-channel instance array; only the slots a board actually uses are
 * handed out by rk3576_pwm_initialize().  Dimensioned for the worst-case
 * (PWM2, 8 channels).
 */

static struct rk3576_pwm_s g_rk3576_pwm[RK3576_PWM_NCTRL][RK3576_PWM_NSLOTS];

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

  /* Ungate pclk + osc_clk for this controller via the CRU driver.
   * PLL clock (clk_pwm) and RC clock are left gated — only the 24 MHz
   * oscillator is used as the counting-clock source.
   *
   * The clock divisor (prescale/scale) is computed per-start in
   * rk3576_pwm_start() based on the requested frequency.
   */

  /* TODO: don't use hard-coded clock sel
   * allow users to select clock
   */

  rk3576_cru_set_pwm_clock_gate(priv->ctrl, true, /* pclk    */
                                false,            /* clk_pwm */
                                false,            /* rc_clk  */
                                true);            /* osc_clk */

  pwminfo("PWM%u setup base=%08" PRIxPTR " version=%08" PRIx32 "\n",
          priv->ctrl, priv->base, rk3576_pwm_getreg(priv, RK3576_PWM_VERSION));
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
 * Name: _pwm_auto_scale
 *
 * Description:
 *   Find the (prescale, scale, period) combination that best approximates
 *   the target output frequency for a given input clock.
 *
 *   The PWM counting clock is derived as:
 *
 *     f_cnt = f_clk / (2^prescale * 2 * scale)          (1)
 *
 *   and the output waveform frequency is:
 *
 *     f_target = f_cnt / period                         (2)
 *
 *   Substituting (1) into (2) and rearranging gives the error term
 *   minimised by this function:
 *
 *     |f_clk - f_target * 2^(prescale+1) * scale * period|
 *
 *   The search iterates over all valid (prescale, scale) pairs — from
 *   the fastest clock (prescale=0, scale=1) to the slowest — and picks
 *   the combination whose rounded period yields the smallest absolute
 *   frequency error.  Because the outer loop starts at the fastest clock
 *   and diff is strictly decreasing, ties are broken in favour of higher
 *   counting-clock resolution, which gives better duty-cycle accuracy.
 *
 * Input Parameters:
 *   target_freq     - Desired PWM output frequency in Hz.
 *   clk_freq        - Input clock frequency before the pre-scaler (Hz).
 *   p_best_prescale - [out] Optimal prescale value (0..7).
 *   p_best_scale    - [out] Optimal scale value (1..256).
 *   p_best_period   - [out] Optimal PWM_PERIOD register value.
 *
 * Returned Value:
 *   OK on success; -ERANGE if no valid configuration exists.
 *
 ****************************************************************************/
static int _pwm_auto_scale(uint32_t target_freq, uint32_t clk_freq,
                           uint32_t *p_best_prescale, uint32_t *p_best_scale,
                           uint32_t *p_best_period)
{
  uint32_t best_period, best_scale, best_prescale;
  bool best_valid = false;
  uint64_t best_diff = UINT64_MAX;
  const uint64_t max_period = 0xFFFFFFFFu;

  if (target_freq == 0 || clk_freq == 0)
    {
      goto out_of_range;
    }

  for (uint8_t pre = 0; pre <= 7; pre++)
    {
      uint32_t shift = (1u << (pre + 1));
      for (uint32_t sc = 1; sc <= 256; sc++)
        {
          uint64_t div = (uint64_t)shift * sc;
          uint64_t denom = (uint64_t)target_freq * div;
          if (denom == 0)
            {
              continue;
            }

          uint64_t per = (uint64_t)clk_freq / denom;

          /* Check candidates: per and per+1.
           * per directly represents the PWM_PERIOD register value,
           * i.e. the number of counting-clock ticks per output cycle. */
          for (int adj = 0; adj <= 1; adj++)
            {
              uint64_t per_cand = per + adj;
              if (per_cand < 1 || per_cand > max_period)
                {
                  continue;
                }

              uint64_t actual = (uint64_t)target_freq * div * per_cand;
              uint64_t diff = (clk_freq > actual) ? (clk_freq - actual)
                                                  : (actual - clk_freq);
              if (diff < best_diff)
                {
                  best_diff = diff;
                  best_period = (uint32_t)per_cand;
                  best_scale = (uint8_t)sc;
                  best_prescale = pre;
                  best_valid = true;
                }
            }
        }
    }

  if (!best_valid)
    {
    out_of_range:
      pwmerr("Unable to generate pwm freq %u with pwm clock freq %u",
             target_freq, clk_freq);
      return -ERANGE;
    }

  if (p_best_prescale)
    {
      *p_best_prescale = best_prescale;
    }

  if (p_best_scale)
    {
      *p_best_scale = best_scale;
    }

  if (p_best_period)
    {
      *p_best_period = best_period;
    }

  return OK;
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
  uint32_t prescale, scale, period, duty;
  uint32_t clk_ctrl_reg_bits = 0;

  int ret = _pwm_auto_scale(info->frequency, RK3576_PWM_OSC_HZ, &prescale,
                            &scale, &period);
  if (ret < 0)
    {
      return ret;
    }

  duty = (uint32_t)(((uint64_t)period * info->duty) >> 16);

  /* Step 1 (TRM §34.6.3): Disable the channel before reconfiguration. */

  if (priv->started)
    {
      rk3576_pwm_putreg(priv, RK3576_PWM_ENABLE,
                        PWM_HIWORD_CLR(PWM_ENABLE_EN | PWM_ENABLE_CLK_EN));
    }

  /* Step 2: Program clock divisor.  */

  clk_ctrl_reg_bits |= (prescale << PWM_CLK_PRESCALE_SHIFT);
  clk_ctrl_reg_bits |= (PWM_CLK_PRESCALE_MASK << 16);

  clk_ctrl_reg_bits |= (scale << PWM_CLK_SCALE_SHIFT);
  clk_ctrl_reg_bits |= (PWM_CLK_SCALE_MASK << 16);

  /* TODO: don't use hard-coded clock sel
   * allow users to select clock
   */
  clk_ctrl_reg_bits |= PWM_CLK_SRC_SEL_CLK_OSC;
  clk_ctrl_reg_bits |= (PWM_CLK_SRC_SEL_MASK << 16);

  rk3576_pwm_putreg(priv, RK3576_PWM_CLK_CTRL, clk_ctrl_reg_bits);

  /* Step 3: Select output mode (left-aligned), duty polarity (active-
   * high), and inactive polarity (active-low — safe default for most
   * peripherals).  pwm_mode is not set here — that comes in step 6.
   *
   * Explicitly clear output_mode, aligned_vld_n and mode bits rather
   * than relying on reset values, so the channel is forced back to
   * left-aligned mode after stop/start cycles.
   */

  rk3576_pwm_putreg(priv, RK3576_PWM_CTRL,
                    PWM_HIWORD(PWM_CTRL_DUTY_POL | PWM_CTRL_INACTIVE_POL) |
                        PWM_HIWORD_CLR(PWM_CTRL_OUTPUT_MODE |
                                       PWM_CTRL_ALIGNED_VLD |
                                       PWM_CTRL_MODE_MASK));

  /* Step 4: Write period and duty registers. */

  rk3576_pwm_putreg(priv, RK3576_PWM_PERIOD, period);
  rk3576_pwm_putreg(priv, RK3576_PWM_DUTY, duty);

  /* Step 5: rpt_first_dimensional — not used (no reload support). */

  /* Step 6: Select continuous mode. */

  rk3576_pwm_putreg(priv, RK3576_PWM_CTRL,
                    PWM_CTRL_MODE_CONTINUOUS | (PWM_CTRL_MODE_MASK << 16));

  /* Step 7: Enable clock and channel together. */

  rk3576_pwm_putreg(priv, RK3576_PWM_ENABLE,
                    PWM_HIWORD(PWM_ENABLE_CLK_EN | PWM_ENABLE_EN));

  priv->started = true;
  return OK;
}

/****************************************************************************
 * Name: rk3576_pwm_stop
 ****************************************************************************/

static int rk3576_pwm_stop(struct pwm_lowerhalf_s *dev)
{
  struct rk3576_pwm_s *priv = (struct rk3576_pwm_s *)dev;

  /* TRM §34.6.3 step 9: Clear pwm_en + clk_en to disable the channel.
   * The output reverts to the inactive polarity (PWM_CTRL.inactive_pol).
   */

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
 *   Return the lower-half handle for one PWM channel for the board
 *   to pass to pwm_register().  The board is responsible for muxing the
 *   channel's output pin before the device is used.
 *
 * Input Parameters:
 *   controller - PWM controller index (RK3576_PWM0, RK3576_PWM1, RK3576_PWM2)
 *   channel    - Channel index within the controller (0-based)
 *
 * Returned Value:
 *   A pwm_lowerhalf_s handle on success; NULL on an invalid controller or
 *   channel.
 ****************************************************************************/

struct pwm_lowerhalf_s *rk3576_pwm_initialize(int pwm_controller_id,
                                              int channel)
{
  const struct rk3576_pwm_ctrl_desc_s *desc;
  struct rk3576_pwm_s *priv;

  if (pwm_controller_id < 0 || pwm_controller_id >= RK3576_PWM_NCTRL)
    {
      pwmerr("ERROR: Invalid PWM controller: %d\n", pwm_controller_id);
      return NULL;
    }

  desc = &g_pwm_ctrls[pwm_controller_id];

  if (channel < 0 || (unsigned int)channel >= desc->nchan)
    {
      pwmerr("ERROR: PWM%u channel %d out of range (max %u)\n",
             pwm_controller_id, channel, desc->nchan);
      return NULL;
    }

  priv = &g_rk3576_pwm[pwm_controller_id][channel];
  priv->ops = &g_rk3576_pwm_ops;
  priv->base = desc->base_addr + channel * RK3576_PWM_CH_STRIDE;
  priv->ctrl = pwm_controller_id;
  priv->started = false;

  return (struct pwm_lowerhalf_s *)priv;
}

#endif /* CONFIG_RK3576_PWM */
