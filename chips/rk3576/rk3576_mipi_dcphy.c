/****************************************************************************
 * chips/rk3576/rk3576_mipi_dcphy.c
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
 * RK3576 MIPI D/C-PHY combo PHY (DCPHY) driver — TX (DSI) side.
 *
 * Implements the DCPHY start-up sequence from TRM Part 2, section 21.6.3
 * "Start Up Sequence".  The PHY is shared between the DSI (display, TX)
 * and CSI (camera, RX) hosts; this driver brings up the TX side.
 *
 * The TX PLL is programmed for the requested high-speed data rate
 * (in Hz) provided by the DSI host at power-on time.  The M/K/S/P divider
 * values are computed at run time from the D-PHY PLL equation (TRM
 * 21.6.2):
 *
 *   Fvco = ((M + K/65536) * 2 * Fin) / P
 *   Fout = Fvco / 2^S
 *
 * subject to 2600 MHz <= Fvco <= 6600 MHz.  The D-PHY HS/LP lane timing
 * parameters are then looked up from a table indexed by the resulting
 * lane data rate (per MIPI D-PHY Supplement Guide).
 *
 * The reference clock is the 24 MHz oscillator (PHY internals select it
 * by default, so the CRU clock tree only models the two APB gates).
 *
 * Register access: DCPHY APB registers are 16-bit wide but are addressed
 * on 32-bit boundaries, so they are accessed as 32-bit words via
 * getreg32()/putreg32().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_mipi_dcphy.h"
#include "rk3576_mipi_dcphy.h"

#ifdef CONFIG_RK3576_MIPI_DCPHY

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* M_RESETN (TX PLL + TX lanes) is PMU1CRU_SOFTRST_CON01[3]. */

#define RK3576_DCPHY_M_RESETN_BIT (3)

/* Poll timeout for PLL lock / PHY ready. */

#define RK3576_DCPHY_POLL_LOOPS (1000000)

/* D-PHY reference clock (24 MHz oscillator). */

#define RK3576_DCPHY_REF_CLK_HZ (24000000)

/* PLL constraint: 2600 MHz <= Fvco <= 6600 MHz (TRM 21.6.2). */

#define RK3576_DCPHY_FVCO_MIN_HZ (2600000000ULL)
#define RK3576_DCPHY_FVCO_MAX_HZ (6600000000ULL)

/* D-PHY TX high-speed rate limit per lane (RK3576 PLL is 2.5 Gbps). */

#define RK3576_DCPHY_DPHY_MAX_HZ (2500000000ULL)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Resolved TX PLL divider set (Fout = ((M + K/65536) * 2 * Fin) /
 * (P * 2^S)).
 */

struct rk3576_dcphy_pll_s
{
  uint32_t rate; /* Resulting Fout in Hz */
  uint8_t p;     /* Pre-divider P (1..4) */
  uint16_t m;    /* Main divider M (64..1023) */
  int16_t k;     /* DSM K (two's complement) */
  uint8_t s;     /* Scaler S (0..6) */
};

/* D-PHY lane timing parameters for one lane-rate bin (per MIPI D-PHY
 * Supplement Guide).  The table is indexed by max lane Mbps.
 */

struct rk3576_dcphy_dphy_timing_s
{
  uint16_t max_lane_mbps; /* Upper bound (inclusive) of this bin */
  uint8_t clk_prepare;
  uint8_t clk_zero;
  uint8_t clk_post;
  uint8_t clk_trail;
  uint8_t hs_prepare;
  uint8_t hs_zero;
  uint8_t hs_trail;
  uint8_t lpx;
  uint8_t hs_exit;
};

struct rk3576_dcphy_s
{
  mutex_t lock;                  /* Serializes PHY state transitions */
  uintptr_t base;                /* DCPHY APB base (0x2B020000) */
  uintptr_t pmu1cru;             /* PMU1CRU base (0x27220000) */
  struct clk_s *pclk_phy;        /* pclk_mipi_dcphy */
  struct clk_s *pclk_grf;        /* pclk_dcphy_grf */
  struct rk3576_dcphy_pll_s pll; /* Resolved TX PLL parameters */
  bool initialized;              /* BIAS/PLL configured once */
  bool powered;                  /* Lanes enabled */
  uint8_t lanes;                 /* Enabled data lanes */
  bool dphy;                     /* true: D-PHY, false: C-PHY */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_dcphy_s g_dcphy = {
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dcphy_getreg / putreg / modifyreg
 ****************************************************************************/

static inline uint32_t rk3576_dcphy_getreg(uintptr_t base, uint32_t offset)
{
  return getreg32(base + offset);
}

static inline void rk3576_dcphy_putreg(uintptr_t base, uint32_t offset,
                                       uint32_t value)
{
  putreg32(value, base + offset);
}

/****************************************************************************
 * Name: rk3576_dcphy_assert_reset / deassert_reset
 *
 * Description:
 *   Assert/deassert M_RESETN (PMU1CRU_SOFTRST_CON01[3]).  Per TRM the
 *   M_RESETN controls the reset to the PLL block, TX clock lane block and
 *   data lane 0/1/2/3 blocks.
 ****************************************************************************/

static void rk3576_dcphy_assert_reset(struct rk3576_dcphy_s *priv)
{
  /* Hiword-mask write: clear bit (active-low reset -> assert by writing 0
   * to the reset bit via the 16-bit hiword write-1-to-set convention used
   * by the CRU SOFTRST registers).
   *
   * Rockchip CRU soft-reset registers use hiword(-mask) semantics: the
   * upper 16 bits are the write-mask, the lower 16 bits carry the value.
   * For an active-low reset line, "assert" = set the reset bit to 0.
   */

  putreg32((1u << (16 + RK3576_DCPHY_M_RESETN_BIT)) |
               (0u << RK3576_DCPHY_M_RESETN_BIT),
           priv->pmu1cru + RK3576_PMU1CRU_SOFTRST_CON(1));
}

static void rk3576_dcphy_deassert_reset(struct rk3576_dcphy_s *priv)
{
  /* Deassert = set the reset bit to 1 (active-low). */

  putreg32((1u << (16 + RK3576_DCPHY_M_RESETN_BIT)) |
               (1u << RK3576_DCPHY_M_RESETN_BIT),
           priv->pmu1cru + RK3576_PMU1CRU_SOFTRST_CON(1));
}

/****************************************************************************
 * Name: rk3576_dcphy_configure_bias
 *
 * Description:
 *   Program the shared BIAS block (TRM 21.6.4.1 step 2).
 ****************************************************************************/

static void rk3576_dcphy_configure_bias(struct rk3576_dcphy_s *priv)
{
  uintptr_t base = priv->base;

  rk3576_dcphy_putreg(base, RK3576_DCPHY_BIAS_CON0, 0x0010);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_BIAS_CON1, 0x0110);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_BIAS_CON2, 0x3223);
}

/****************************************************************************
 * Name: rk3576_dcphy_pll_calc
 *
 * Description:
 *   Resolve the TX PLL dividers (P, M, K, S) for the requested D-PHY
 *   high-speed data rate (in Hz).  Uses the DCPHY PLL equation from
 *   TRM 21.6.2:
 *
 *     Fvco = ((M + K/65536) * 2 * Fin) / P
 *     Fout = Fvco / 2^S
 *
 *   with Fin = 24 MHz and 2600 MHz <= Fvco <= 6600 MHz.  The divider
 *   search follows the standard DCPHY PLL loop (SSC modulation excluded
 *   — not required at <= 2.5 Gbps).
 *
 *   The requested rate is rounded up in 100 kHz steps until a valid PLL
 *   setting is found; the caller's `pll->rate` is filled with the actual
 *   achievable rate (>= requested).
 *
 * Input Parameters:
 *   rate - Requested lane high-speed data rate in Hz (1..2.5e9).
 *   pll  - Output: resolved divider set and actual rate.
 *
 * Returned Value:
 *   OK on success; -EINVAL if no valid PLL setting exists.
 ****************************************************************************/

static int rk3576_dcphy_pll_calc(uint32_t rate,
                                 FAR struct rk3576_dcphy_pll_s *pll)
{
  const uint64_t fin = RK3576_DCPHY_REF_CLK_HZ;
  uint64_t best_delta = UINT64_MAX;
  uint64_t fout;
  uint64_t fvco;
  uint32_t s;
  uint32_t p;
  uint64_t m;
  int64_t k;
  bool found = false;

  DEBUGASSERT(pll != NULL);

  /* Clamp to the D-PHY TX range (80 Mbps .. 2.5 Gbps). */

  if (rate > RK3576_DCPHY_DPHY_MAX_HZ)
    {
      rate = RK3576_DCPHY_DPHY_MAX_HZ;
    }

  if (rate < 80000000)
    {
      rate = 80000000;
    }

  /* Try increasing target rates in 100 kHz steps until a setting is found. */

  while (!found)
    {
      fout = rate;

      for (s = 0; s <= 6; s++)
        {
          fvco = fout << s;

          if (fvco < RK3576_DCPHY_FVCO_MIN_HZ ||
              fvco > RK3576_DCPHY_FVCO_MAX_HZ)
            {
              continue;
            }

          /* Pre-divider P: 6 MHz <= Fin/P <= 30 MHz, P in {1..4}. */

          for (p = 1; p <= 4; p++)
            {
              uint64_t fref = fin / p;
              uint64_t delta;
              uint64_t actual;

              if (fref < 6000000 || fref > 30000000)
                {
                  continue;
                }

              /* M = round(Fvco * P / (2 * Fin)). */

              m = (fvco * p + fin) / (2 * fin);
              if (m < 64 || m > 1023)
                {
                  continue;
                }

              /* K = the signed fractional remainder, two's-complement
               * DSM value: K = (Fvco*P/(2*Fin) - M) * 65536.
               */

              k = (int64_t)(fvco * p) - (int64_t)(2 * m * fin);
              k = (k * 65536) / (int64_t)(2 * fin);
              if (k < -32768 || k > 32767)
                {
                  continue;
                }

              /* Actual Fout = ((M + K/65536) * 2 * Fin) / (P * 2^S).
               * (M + K/65536) is always positive: M >= 64 and K >= -32768.
               */

              actual = (uint64_t)((int64_t)m * 65536 + k);
              actual = (actual * 2 * fin) / p;
              actual = actual / 65536;
              actual = actual >> s;

              delta = (actual > rate) ? (actual - rate) : (rate - actual);

              if (delta < best_delta)
                {
                  best_delta = delta;
                  pll->p = p;
                  pll->m = m;
                  pll->k = (int16_t)k;
                  pll->s = s;
                  pll->rate = (uint32_t)actual;
                  found = true;
                }
            }
        }

      if (!found)
        {
          if (rate >= RK3576_DCPHY_DPHY_MAX_HZ)
            {
              return -EINVAL;
            }

          rate += 100000;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_dcphy_dphy_timing_lookup
 *
 * Description:
 *   Return the D-PHY timing parameters for the given lane high-speed rate
 *   (in Mbps).  The values follow the MIPI D-PHY Supplement Guide; the
 *   T_PREPARE / T_ZERO / T_TRAIL / T_LPX / T_HS_EXIT / T_CLK_POST counters
 *   are computed from the standard D-PHY timing parameters for the RK3576
 *   TX range (80..2500 Mbps).  The table is indexed by the upper bound of
 *   each bin; the first bin whose bound >= lane_mbps wins.
 ****************************************************************************/

static const struct rk3576_dcphy_dphy_timing_s *
rk3576_dcphy_dphy_timing_lookup(uint32_t lane_mbps)
{
  static const struct rk3576_dcphy_dphy_timing_s table[] = {
    /* {max_mbps, clk_prep, clk_zero, clk_post, clk_trail,
     *  hs_prep, hs_zero, hs_trail, lpx, hs_exit} */
    { 100, 3, 0, 0, 29, 5, 0, 22, 2, 6 },
    { 200, 7, 1, 0, 33, 9, 0, 26, 5, 11 },
    { 300, 11, 3, 0, 36, 13, 0, 29, 8, 16 },
    { 400, 15, 5, 0, 39, 17, 0, 33, 11, 21 },
    { 500, 19, 6, 1, 43, 22, 1, 36, 15, 26 },
    { 750, 29, 11, 2, 51, 30, 3, 45, 22, 38 },
    { 1000, 39, 16, 3, 60, 40, 6, 53, 29, 50 },
    { 1250, 49, 20, 5, 68, 49, 8, 62, 37, 62 },
    { 1500, 7, 24, 6, 7, 7, 10, 6, 5, 7 },
    { 1750, 8, 29, 7, 8, 8, 13, 7, 6, 9 },
    { 2000, 9, 34, 8, 9, 9, 15, 8, 7, 10 },
    { 2250, 10, 39, 10, 10, 18, 9, 8, 15, 11 },
    { 2500, 12, 43, 11, 11, 11, 20, 10, 9, 13 },
  };

  uint32_t i;

  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
      if (lane_mbps <= table[i].max_lane_mbps)
        {
          return &table[i];
        }
    }

  /* Above the highest bin: use the last entry. */

  return &table[(sizeof(table) / sizeof(table[0])) - 1];
}

/****************************************************************************
 * Name: rk3576_dcphy_configure_pll
 *
 * Description:
 *   Program the TX PLL registers from the resolved PLL parameters.
 *   PLL_EN (PLL_CON0[12]) is intentionally left clear; the DCPHY start-up
 *   sequence enables the PLL later (see rk3576_dcphy_power_on()).
 ****************************************************************************/

static void rk3576_dcphy_configure_pll(struct rk3576_dcphy_s *priv)
{
  uintptr_t base = priv->base;
  uint32_t con0;

  con0 = ((uint32_t)priv->pll.s << DCPHY_PLL_CON0_S_SHIFT) |
         ((uint32_t)priv->pll.p << DCPHY_PLL_CON0_P_SHIFT);

  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON0, con0);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON1,
                      (uint32_t)(uint16_t)priv->pll.k);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON2,
                      ((uint32_t)priv->pll.m << DCPHY_PLL_CON2_M_SHIFT) &
                          DCPHY_PLL_CON2_M_MASK);
}

/****************************************************************************
 * Name: rk3576_dcphy_configure_tx_clock_lane
 *
 * Description:
 *   Program the TX clock lane (TRM 21.6.4.1 step 4), including the D-PHY
 *   timing counters looked up from the lane data rate.
 ****************************************************************************/

static void rk3576_dcphy_configure_tx_clock_lane(struct rk3576_dcphy_s *priv,
                                                 uint32_t lane_mbps)
{
  const struct rk3576_dcphy_dphy_timing_s *timing;
  uintptr_t base = priv->base;
  uint32_t val;

  timing = rk3576_dcphy_dphy_timing_lookup(lane_mbps);

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_GNR_CON0, 0xf000);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_ANA_CON0, 0x7133);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_ANA_CON1, 0x0001);

  /* TIME_CON0: HSTX_CLK_SEL (serial clock divider) + T_LPX. */

  val = 0;
  if (lane_mbps < 1500)
    {
      val = DCPHY_MC_TIME_CON0_HSTX_CLK_SEL;
    }

  val |= ((uint32_t)timing->lpx << DCPHY_MC_TIME_CON0_T_LPX_SHIFT) &
         DCPHY_MC_TIME_CON0_T_LPX_MASK;
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_TIME_CON0, val);

  /* TIME_CON1: T_CLK_ZERO | T_CLK_PREPARE. */

  val = ((uint32_t)timing->clk_zero << DCPHY_MC_TIME_CON1_T_CLK_ZERO_SHIFT) |
        ((uint32_t)timing->clk_prepare
         << DCPHY_MC_TIME_CON1_T_CLK_PREPARE_SHIFT);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_TIME_CON1, val);

  /* TIME_CON2: T_HS_EXIT | T_CLK_TRAIL. */

  val = ((uint32_t)timing->hs_exit << DCPHY_MC_TIME_CON2_T_HS_EXIT_SHIFT) |
        ((uint32_t)timing->clk_trail << DCPHY_MC_TIME_CON2_T_CLK_TRAIL_SHIFT);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_TIME_CON2, val);

  /* TIME_CON3: T_CLK_POST. */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_TIME_CON3,
                      (uint32_t)timing->clk_post);

  /* Escape clock = 20 MHz. */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_TIME_CON4,
                      DCPHY_TIME_CON4_ESC_CLK_DIV);

  /* Deskew calibration is only used above 1.5 Gbps. */

  if (lane_mbps > 1500)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_DESKEW_CON0, 0x9cb1);
    }
}

/****************************************************************************
 * Name: rk3576_dcphy_configure_tx_data_lane
 *
 * Description:
 *   Program one TX data lane (TRM 21.6.4.1 step 5), including the D-PHY
 *   timing counters looked up from the lane data rate.
 ****************************************************************************/

static void rk3576_dcphy_configure_tx_data_lane(struct rk3576_dcphy_s *priv,
                                                unsigned int lane,
                                                uint32_t lane_mbps)
{
  const struct rk3576_dcphy_dphy_timing_s *timing;
  uintptr_t base = priv->base;
  uint32_t val;

  DEBUGASSERT(lane < 4);

  timing = rk3576_dcphy_dphy_timing_lookup(lane_mbps);

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_GNR_CON1(lane), 0x2000);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_ANA_CON0(lane), 0x7133);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_ANA_CON1(lane), 0x0001);

  /* TIME_CON0: HSTX_CLK_SEL + T_LPX. */

  val = 0;
  if (lane_mbps < 1500)
    {
      val = DCPHY_MD_TIME_CON0_HSTX_CLK_SEL;
    }

  val |= ((uint32_t)timing->lpx << DCPHY_MD_TIME_CON0_T_LPX_SHIFT) &
         DCPHY_MD_TIME_CON0_T_LPX_MASK;
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_TIME_CON0(lane), val);

  /* TIME_CON1: T_HS_ZERO | T_HS_PREPARE. */

  val =
      ((uint32_t)timing->hs_zero << DCPHY_MD_TIME_CON1_T_HS_ZERO_SHIFT) |
      ((uint32_t)timing->hs_prepare << DCPHY_MD_TIME_CON1_T_HS_PREPARE_SHIFT);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_TIME_CON1(lane), val);

  /* TIME_CON2: T_HS_EXIT | T_HS_TRAIL. */

  val = ((uint32_t)timing->hs_exit << DCPHY_MD_TIME_CON2_T_HS_EXIT_SHIFT) |
        ((uint32_t)timing->hs_trail << DCPHY_MD_TIME_CON2_T_HS_TRAIL_SHIFT);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_TIME_CON2(lane), val);

  /* TIME_CON3: TTA-GET/TTA-GO default (0x30). */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_TIME_CON3(lane),
                      (0x3 << DCPHY_MD_TIME_CON3_T_TA_GET_SHIFT) |
                          (0x0 << DCPHY_MD_TIME_CON3_T_TA_GO_SHIFT));

  /* Escape clock = 20 MHz. */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_TIME_CON4(lane),
                      DCPHY_TIME_CON4_ESC_CLK_DIV);
}

/****************************************************************************
 * Name: rk3576_dcphy_wait_pll_lock
 ****************************************************************************/

static int rk3576_dcphy_wait_pll_lock(struct rk3576_dcphy_s *priv)
{
  uintptr_t base = priv->base;
  int loops = RK3576_DCPHY_POLL_LOOPS;

  while (loops-- > 0)
    {
      if ((rk3576_dcphy_getreg(base, RK3576_DCPHY_PLL_STAT0) &
           DCPHY_PLL_STAT0_PLL_LOCK) != 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_dcphy_wait_lane_ready
 ****************************************************************************/

static int rk3576_dcphy_wait_lane_ready(uintptr_t base, uint32_t gnr_con0)
{
  int loops = RK3576_DCPHY_POLL_LOOPS;

  while (loops-- > 0)
    {
      if ((rk3576_dcphy_getreg(base, gnr_con0) & DCPHY_GNR_CON0_PHY_READY) !=
          0)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dcphy_init
 ****************************************************************************/

int rk3576_dcphy_init(void)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Exclusive initialization: reject a second init (do not re-run the
   * BIAS/PLL/reset sequence on an already-initialized PHY).
   */

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  priv->base = RK3576_DCPHY_ADDR;
  priv->pmu1cru = RK3576_PMU1_CRU_ADDR;

  /* Enable the PHY APB and GRF clocks. */

  priv->pclk_phy = clk_get("pclk_mipi_dcphy");
  if (priv->pclk_phy == NULL)
    {
      gerr("ERROR: DCPHY failed to get pclk_mipi_dcphy\n");
      ret = -ENODEV;
      goto errout_unlock;
    }

  priv->pclk_grf = clk_get("pclk_dcphy_grf");
  if (priv->pclk_grf == NULL)
    {
      gerr("ERROR: DCPHY failed to get pclk_dcphy_grf\n");
      ret = -ENODEV;
      goto errout_unlock;
    }

  ret = clk_enable(priv->pclk_phy);
  if (ret < 0)
    {
      gerr("ERROR: DCPHY failed to enable pclk_mipi_dcphy: %d\n", ret);
      goto errout_unlock;
    }

  ret = clk_enable(priv->pclk_grf);
  if (ret < 0)
    {
      gerr("ERROR: DCPHY failed to enable pclk_dcphy_grf: %d\n", ret);
      clk_disable(priv->pclk_phy);
      goto errout_unlock;
    }

  /* Assert M_RESETN while programming (TRM 21.6.4.1 step 1). */

  rk3576_dcphy_assert_reset(priv);

  /* Configure the shared BIAS block (step 2).  The PLL and lane timing are
   * programmed at power-on time once the data rate is known (see
   * rk3576_dcphy_power_on()).
   */

  rk3576_dcphy_configure_bias(priv);

  priv->initialized = true;

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_dcphy_power_on
 ****************************************************************************/

int rk3576_dcphy_power_on(uint8_t lanes, bool dphy, uint32_t hs_rate)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  uintptr_t base;
  unsigned int lane;
  uint32_t lane_mbps;
  uint32_t con0;
  int ret;

  DEBUGASSERT(lanes >= 1 && lanes <= 4);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      ret = -EPERM;
      goto errout_unlock;
    }

  base = priv->base;

  /* Resolve the TX PLL for the requested high-speed data rate. */

  ret = rk3576_dcphy_pll_calc(hs_rate, &priv->pll);
  if (ret < 0)
    {
      _err("DCPHY: failed to resolve PLL for %u Hz\n", hs_rate);
      goto errout_unlock;
    }

  lane_mbps = priv->pll.rate / 1000000;

  /* Configure PLL (step 3), clock lane (step 4) and data lanes (step 5). */

  rk3576_dcphy_configure_pll(priv);
  rk3576_dcphy_configure_tx_clock_lane(priv, lane_mbps);

  for (lane = 0; lane < lanes; lane++)
    {
      rk3576_dcphy_configure_tx_data_lane(priv, lane, lane_mbps);
    }

  /* Enable PLL (step 6): set PLL_EN while preserving the P/S divider
   * already programmed into PLL_CON0.
   */

  con0 = ((uint32_t)priv->pll.s << DCPHY_PLL_CON0_S_SHIFT) |
         ((uint32_t)priv->pll.p << DCPHY_PLL_CON0_P_SHIFT);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON0,
                      con0 | DCPHY_PLL_CON0_PLL_EN);

  /* Wait for PLL lock (step 7). */

  ret = rk3576_dcphy_wait_pll_lock(priv);
  if (ret < 0)
    {
      _err("DCPHY: PLL lock timeout\n");
      goto errout_unlock;
    }

  /* Enable clock lane and data lanes (step 8). */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_GNR_CON0,
                      0xf000 | DCPHY_GNR_CON0_ENABLE);

  for (lane = 0; lane < lanes; lane++)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_GNR_CON0(lane),
                          DCPHY_GNR_CON0_ENABLE);
    }

  /* Wait for clock lane PHY_READY (step 9). */

  ret = rk3576_dcphy_wait_lane_ready(base, RK3576_DCPHY_MC_GNR_CON0);
  if (ret < 0)
    {
      _err("DCPHY: clock lane ready timeout\n");
      goto errout_unlock;
    }

  for (lane = 0; lane < lanes; lane++)
    {
      ret = rk3576_dcphy_wait_lane_ready(base, RK3576_DCPHY_MD_GNR_CON0(lane));
      if (ret < 0)
        {
          _err("DCPHY: data lane %u ready timeout\n", lane);
          goto errout_unlock;
        }
    }

  /* Deassert M_RESETN (step 10). */

  rk3576_dcphy_deassert_reset(priv);

  priv->lanes = lanes;
  priv->dphy = dphy;
  priv->powered = true;

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_dcphy_power_off
 ****************************************************************************/

int rk3576_dcphy_power_off(void)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  uintptr_t base;
  unsigned int lane;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized || !priv->powered)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  base = priv->base;

  /* Disable data lanes, clock lane, then the PLL. */

  for (lane = 0; lane < priv->lanes; lane++)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_GNR_CON0(lane), 0x0000);
    }

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_GNR_CON0, 0x0000);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON0, 0x0000);

  priv->powered = false;
  priv->lanes = 0;

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_dcphy_is_ready
 ****************************************************************************/

bool rk3576_dcphy_is_ready(void)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  bool ready;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return false;
    }

  ready = priv->initialized && priv->powered &&
          ((rk3576_dcphy_getreg(priv->base, RK3576_DCPHY_PLL_STAT0) &
            DCPHY_PLL_STAT0_PLL_LOCK) != 0);

  nxmutex_unlock(&priv->lock);
  return ready;
}

#endif /* CONFIG_RK3576_MIPI_DCPHY */
