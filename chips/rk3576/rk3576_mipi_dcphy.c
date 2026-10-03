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
#include <sys/param.h>

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

/* M_RESETN (TX PLL + TX lanes) is PMU1CRU_SOFTRST_CON01[3]; S_RESETN (the
 * slave RX clock and data lane blocks) is the next bit up, [4].  Both are
 * "When high, reset": writing 1 asserts the reset, writing 0 releases it.
 * The two sides are independent, so neither the DSI nor the CSI driver has
 * to know the other exists.
 */

#define RK3576_DCPHY_M_RESETN_BIT (3)
#define RK3576_DCPHY_S_RESETN_BIT (4)

/* Poll timeout for PLL lock / PHY ready.
 *
 * A microsecond does not come in ones: up_udelay(1) costs a call and a
 * calibration lookup as well as the microsecond it waits for, so a loop of a
 * million of them runs for something closer to twenty seconds than to the one
 * second the count suggests.  That mattered: a caller can be the application
 * changing the camera's mode, and it is blocked for the whole of this --
 * including the server it is also running, so a switch that could not bring
 * the PHY up looked to a browser like a board that had gone away.
 *
 * The timeout is stated as a time and stepped in tens of microseconds, which
 * is what makes the arithmetic above honest.  A second is generous by orders
 * of magnitude for a PLL lock or a lane reaching its ready state; what it is
 * for is bounding a failure, not accommodating one.
 */

#define RK3576_DCPHY_POLL_STEP_US 10
#define RK3576_DCPHY_POLL_LOOPS   (100000) /* 100000 * 10 us = 1 s */

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
  uintptr_t grf;                 /* DCPHY GRF base (0x26034000) */
  uintptr_t pmu1cru;             /* PMU1CRU base (0x27220000) */
  struct clk_s *pclk_phy;        /* pclk_mipi_dcphy */
  struct clk_s *pclk_grf;        /* pclk_dcphy_grf */
  struct rk3576_dcphy_pll_s pll; /* Resolved TX PLL parameters */
  uint32_t lane_mbps;            /* Lane rate the timing table was built for */
  bool initialized;              /* BIAS/PLL configured once */
  bool powered;                  /* TX lanes enabled */
  uint8_t lanes;                 /* Enabled TX data lanes */
  bool dphy;                     /* true: D-PHY, false: C-PHY */
  bool bias_ready;               /* Shared BIAS block programmed */
  bool rx_powered;               /* RX (slave) lanes enabled */
  uint8_t rx_lanes;              /* Enabled RX data lanes */
  uint32_t rx_lane_mbps;         /* Rate the RX settle value was built for */
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
  /* Assert M_RESETN.  PMU1CRU_SOFTRST_CON01[3] is "When high, reset relative
   * logic" (TRM Part 1): writing 1 asserts the reset, writing 0 releases it.
   * This is the standard Rockchip CRU soft-reset polarity (bit=1 -> reset),
   * NOT an active-low line.  The hiword mask (bit 16+3) makes bit 3 writable.
   */

  putreg32((1u << (16 + RK3576_DCPHY_M_RESETN_BIT)) |
               (1u << RK3576_DCPHY_M_RESETN_BIT),
           priv->pmu1cru + RK3576_PMU1CRU_SOFTRST_CON(1));
}

static void rk3576_dcphy_deassert_reset(struct rk3576_dcphy_s *priv)
{
  /* Deassert M_RESETN: write 0 to release the reset. */

  putreg32((1u << (16 + RK3576_DCPHY_M_RESETN_BIT)) |
               (0u << RK3576_DCPHY_M_RESETN_BIT),
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
     *  hs_prep, hs_zero, hs_trail, lpx, hs_exit}
     *
     * WARNING: the D-PHY timing table has an 11th per-entry field
     * "hs_settle" in some transcriptions, which the D-PHY TX path
     * does NOT program (it is RX-settle-only); it must NOT be confused with
     * hs_exit here.  The previous hand-authored values had hs_exit (T_HS_EXIT,
     * the clock lane HS->LP-11 exit time, panel Table 46 THS-EXIT ~100ns)
     * wrong by an order of magnitude (e.g. 21 instead of 1 at 400 Mbps),
     * which kept the clock lane from ever returning to LP-11: clk_stopstate
     * stayed 0, phy_tx_ready FSM stuck at INIT, all-black video.
     *
     * NOTE: this is a PARTIAL table -- only the 100 Mbps-decade rows
     * are carried.  The "first bound >= lane_mbps" rule below therefore lands
     * one bin high whenever the rate falls between two rows (384 Mbps picks
     * the 400 row).  The resulting difference is a single counter LSB in
     * t_lpx /
     * t_clk_zero / t_hs_trail, i.e. a few nanoseconds, so it is not a
     * plausible cause of a dead TX path.  If a panel ever
     * lands on a boundary where this matters, add the missing rows rather
     * than guessing.
     */
    { 100, 3, 0, 0, 29, 5, 0, 22, 2, 0 },
    { 200, 7, 1, 0, 33, 9, 0, 26, 5, 0 },
    { 300, 11, 3, 0, 36, 13, 0, 29, 8, 1 },
    { 400, 15, 5, 0, 39, 17, 0, 33, 11, 1 },
    { 500, 19, 6, 1, 43, 22, 1, 36, 15, 2 },
    { 750, 29, 11, 2, 51, 30, 3, 45, 22, 4 },
    { 1000, 39, 16, 3, 60, 40, 6, 53, 29, 5 },
    { 1250, 49, 20, 5, 68, 49, 8, 62, 37, 7 },

    /* Same row: the 10th field is hs_exit = 10, NOT the 11th (hs_settle = 7)
     * that stood here before.
     * Identical failure mode to the original hs_exit bug -- reading
     * hs_settle as hs_exit -- so it is called out explicitly. */

    { 1500, 7, 24, 6, 7, 7, 10, 6, 5, 10 },
    { 1750, 8, 29, 7, 8, 8, 13, 7, 6, 8 },
    { 2000, 9, 34, 8, 9, 9, 15, 8, 7, 9 },
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

  /* PLL gate/reset source select: tie to the ENABLE phase. */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON5,
                      DCPHY_PLL_CON5_RESET_N_SEL |
                          DCPHY_PLL_CON5_PLL_ENABLE_SEL);

  /* PLL lock / stabilization counters (TRM 21.6.3 step 4).  Reset values
   * are 0, which would assert PLL_LOCK immediately and skip the ~200us
   * PLL stabilization window; program them so
   * the shared clocks (incl. M_TXWORDCLKHS) are truly stable before use.
   */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON7,
                      DCPHY_PLL_CON7_LOCK_CNT_DEFAULT);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_PLL_CON8,
                      DCPHY_PLL_CON8_STB_CNT_DEFAULT);
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

  /* GNR_CON1 = T_PHY_READY timeout.  T_PHY_READY(0x2000) is programmed for the
   * clock lane as well as every data lane; leaving it at the reset value 0
   * gives the PHY_READY handshake a zero-cycle budget, which is not a state
   * the hardware is specified for.
   */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_GNR_CON1,
                      RK3576_DCPHY_T_PHY_READY_DEFAULT);

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_ANA_CON0,
                      RK3576_DCPHY_ANA_CON0_CLK_LANE);

  /* ANA_CON1 = 0x0001 is only programmed when the
   * lane rate is >= 4500 Mbps; at the panel rates used here
   * (<= 400 Mbps) it must stay at reset.
   */

  if (lane_mbps >= 4500)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_MC_ANA_CON1, 0x0001);
    }

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

  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_GNR_CON1(lane),
                      RK3576_DCPHY_T_PHY_READY_DEFAULT);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_ANA_CON0(lane),
                      RK3576_DCPHY_ANA_CON0_DATA_LANE);

  /* ANA_CON1 = 0x0001 is reserved for lane rates >= 4500 Mbps (reference
   * driver gates it the same way).  Also see the clock lane. */

  if (lane_mbps >= 4500)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_MD_ANA_CON1(lane), 0x0001);
    }

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

      up_udelay(RK3576_DCPHY_POLL_STEP_US);
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

      up_udelay(RK3576_DCPHY_POLL_STEP_US);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_dcphy_assert_s_reset / deassert_s_reset
 *
 * Description:
 *   Assert/deassert S_RESETN (PMU1CRU_SOFTRST_CON01[4]), the reset of the
 *   slave (RX) clock and data lane blocks.  Same polarity as M_RESETN:
 *   "when high, reset".  The reset is held while the lane banks are being
 *   programmed and released once every lane reports PHY_READY, which is
 *   the order the TRM's RX sequence prescribes (unlike the TX sequence,
 *   which releases at the very end of a different set of steps).
 ****************************************************************************/

static void rk3576_dcphy_assert_s_reset(struct rk3576_dcphy_s *priv)
{
  putreg32((1u << (16 + RK3576_DCPHY_S_RESETN_BIT)) |
               (1u << RK3576_DCPHY_S_RESETN_BIT),
           priv->pmu1cru + RK3576_PMU1CRU_SOFTRST_CON(1));
}

static void rk3576_dcphy_deassert_s_reset(struct rk3576_dcphy_s *priv)
{
  putreg32((1u << (16 + RK3576_DCPHY_S_RESETN_BIT)) |
               (0u << RK3576_DCPHY_S_RESETN_BIT),
           priv->pmu1cru + RK3576_PMU1CRU_SOFTRST_CON(1));
}

/****************************************************************************
 * Name: rk3576_dcphy_ensure_bias
 *
 * Description:
 *   Enable the PHY APB clocks and program the shared BIAS block.  Both
 *   directions need the BIAS block, and its start-up values are the same
 *   for TX and RX, so this is written once and remembered.
 *
 *   Must be called with priv->lock held.
 *
 * Input Parameters:
 *   priv - PHY state
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_dcphy_ensure_bias(struct rk3576_dcphy_s *priv)
{
  int ret;

  priv->base = RK3576_DCPHY_ADDR;
  priv->grf = RK3576_DCPHY_GRF_ADDR;
  priv->pmu1cru = RK3576_PMU1_CRU_ADDR;

  if (priv->pclk_phy == NULL)
    {
      priv->pclk_phy = clk_get("pclk_mipi_dcphy");
      if (priv->pclk_phy == NULL)
        {
          gerr("ERROR: DCPHY failed to get pclk_mipi_dcphy\n");
          return -ENODEV;
        }

      ret = clk_enable(priv->pclk_phy);
      if (ret < 0)
        {
          gerr("ERROR: DCPHY failed to enable pclk_mipi_dcphy: %d\n", ret);
          priv->pclk_phy = NULL;
          return ret;
        }
    }

  if (priv->pclk_grf == NULL)
    {
      priv->pclk_grf = clk_get("pclk_dcphy_grf");
      if (priv->pclk_grf == NULL)
        {
          gerr("ERROR: DCPHY failed to get pclk_dcphy_grf\n");
          return -ENODEV;
        }

      ret = clk_enable(priv->pclk_grf);
      if (ret < 0)
        {
          gerr("ERROR: DCPHY failed to enable pclk_dcphy_grf: %d\n", ret);
          priv->pclk_grf = NULL;
          return ret;
        }
    }

  if (!priv->bias_ready)
    {
      rk3576_dcphy_configure_bias(priv);
      priv->bias_ready = true;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_dcphy_rx_settle_value / rk3576_dcphy_rx_dlysel
 *
 * Description:
 *   Resolve the two rate-dependent RX lane parameters.
 *
 *   The data-lane T_HS_SETTLE field (SD*_TIME_CON0) is a counter whose
 *   clock is derived from the received serial clock; bit 8 of the value
 *   picks the divider (serial clock / 2 below 1.5 Gbps, / 16 at and above
 *   it) and bits [7:0] are the count.  The table below encodes both.
 *
 *   It cross-checks against the TRM rather than being guesswork: the
 *   documented 2.5 Gbps RX case programs SD*_TIME_CON0 = 0x00d, and 0x00d
 *   is exactly the entry selected here for 2500 Mbps.  The clock lane's
 *   settle register is fixed at 0x301 on both sides (the vendor driver
 *   writes the same constant with the comment "clk settle fix").
 *
 *   ANA_CON2 bits [9:8] are a receiver delay select that follows the same
 *   rate bands; the low bits are a termination code that does not change
 *   with rate.
 *
 ****************************************************************************/

struct rk3576_dcphy_rx_settle_s
{
  uint32_t max_mbps;
  uint16_t value;
};

static const struct rk3576_dcphy_rx_settle_s g_rk3576_dcphy_rx_settle[] = {
  { 80, 0x105 },   { 100, 0x106 },  { 120, 0x107 },  { 140, 0x108 },
  { 160, 0x109 },  { 180, 0x10a },  { 200, 0x10b },  { 220, 0x10c },
  { 240, 0x10d },  { 270, 0x10e },  { 290, 0x10f },  { 310, 0x110 },
  { 330, 0x111 },  { 350, 0x112 },  { 370, 0x113 },  { 390, 0x114 },
  { 410, 0x115 },  { 430, 0x116 },  { 450, 0x117 },  { 470, 0x118 },
  { 490, 0x119 },  { 510, 0x11a },  { 540, 0x11b },  { 560, 0x11c },
  { 580, 0x11d },  { 600, 0x11e },  { 620, 0x11f },  { 640, 0x120 },
  { 660, 0x121 },  { 680, 0x122 },  { 700, 0x123 },  { 720, 0x124 },
  { 740, 0x125 },  { 760, 0x126 },  { 790, 0x127 },  { 810, 0x128 },
  { 830, 0x129 },  { 850, 0x12a },  { 870, 0x12b },  { 890, 0x12c },
  { 910, 0x12d },  { 930, 0x12e },  { 950, 0x12f },  { 970, 0x130 },
  { 990, 0x131 },  { 1010, 0x132 }, { 1030, 0x133 }, { 1060, 0x134 },
  { 1080, 0x135 }, { 1100, 0x136 }, { 1120, 0x137 }, { 1140, 0x138 },
  { 1160, 0x139 }, { 1180, 0x13a }, { 1200, 0x13b }, { 1220, 0x13c },
  { 1240, 0x13d }, { 1260, 0x13e }, { 1280, 0x13f }, { 1310, 0x140 },
  { 1330, 0x141 }, { 1350, 0x142 }, { 1370, 0x143 }, { 1390, 0x144 },
  { 1410, 0x145 }, { 1430, 0x146 }, { 1450, 0x147 }, { 1470, 0x148 },
  { 1490, 0x149 }, { 1580, 0x007 }, { 1740, 0x008 }, { 1910, 0x009 },
  { 2070, 0x00a }, { 2240, 0x00b }, { 2410, 0x00c }, { 2570, 0x00d },
  { 2740, 0x00e }, { 2910, 0x00f }, { 3070, 0x010 }, { 3240, 0x011 },
  { 3410, 0x012 }, { 3570, 0x013 }, { 3740, 0x014 }, { 3890, 0x015 },
  { 4070, 0x016 }, { 4240, 0x017 }, { 4400, 0x018 }, { 4500, 0x019 },
};

static uint16_t rk3576_dcphy_rx_settle_value(uint32_t lane_mbps)
{
  size_t i;

  for (i = 0; i < nitems(g_rk3576_dcphy_rx_settle); i++)
    {
      if (g_rk3576_dcphy_rx_settle[i].max_mbps >= lane_mbps)
        {
          return g_rk3576_dcphy_rx_settle[i].value;
        }
    }

  return g_rk3576_dcphy_rx_settle[nitems(g_rk3576_dcphy_rx_settle) - 1].value;
}

static uint32_t rk3576_dcphy_rx_dlysel(uint32_t lane_mbps)
{
  if (lane_mbps < 1500)
    {
      return 0;
    }
  else if (lane_mbps < 2000)
    {
      return 3u << 8;
    }
  else if (lane_mbps < 3000)
    {
      return 2u << 8;
    }
  else if (lane_mbps < 4000)
    {
      return 1u << 8;
    }

  return 0;
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

  /* Bring up the APB clocks and the shared BIAS block.  Both directions
   * need this and both write the same values, so it is shared and
   * idempotent -- the CSI path may well have run it first.
   */

  ret = rk3576_dcphy_ensure_bias(priv);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  /* Assert M_RESETN while programming (TRM 21.6.4.1 step 1). */

  rk3576_dcphy_assert_reset(priv);

  priv->initialized = true;

  nxmutex_unlock(&priv->lock);
  return OK;
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
  priv->lane_mbps = lane_mbps;

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

  /* After releasing M_RESETN, the initial deskew calibration (TSKEWCAL)
   * runs for up to ~100us; wait for it to settle before the DSI host
   * starts issuing commands, otherwise the TX HS state machine lacks a
   * stable clock.
   */

  up_udelay(150);

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

/****************************************************************************
 * Name: rk3576_dcphy_rx_power_on
 *
 * Description:
 *   RX (slave lane) start-up, following the TRM's 21.6.4.3 "Case 3" RX
 *   sequence.  Steps are numbered as the TRM numbers them.
 *
 ****************************************************************************/

int rk3576_dcphy_rx_power_on(uint8_t lanes, uint32_t lane_mbps)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  uintptr_t base;
  uint16_t settle;
  uint32_t dlysel;
  unsigned int lane;
  int ret;

  if (lanes < 1 || lanes > 4)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_dcphy_ensure_bias(priv);
  if (ret < 0)
    {
      goto errout_unlock;
    }

  base = priv->base;

  /* D-PHY mode is the reset value, so this only matters if something else
   * left the slave side in C-PHY mode.  The GRF needs the hiword write
   * enable.
   */

  putreg32(RK3576_DCPHY_GRF_HWM(RK3576_DCPHY_GRF_CON0_S_CPHY_MODE),
           priv->grf + RK3576_DCPHY_GRF_CON0_OFF);

  /* Step 1: hold the slave lanes in reset while programming. */

  rk3576_dcphy_assert_s_reset(priv);

  /* Step 2: shared BIAS block.  Written again here to follow the TRM
   * sequence literally; the values are the same as the TX side's, so the
   * repeat is harmless.
   */

  rk3576_dcphy_configure_bias(priv);

  /* Steps 3-7: analog, readiness and timing parameters.  The clock lane
   * first, then every enabled data lane.
   */

  settle = rk3576_dcphy_rx_settle_value(lane_mbps);
  dlysel = rk3576_dcphy_rx_dlysel(lane_mbps);

  rk3576_dcphy_putreg(base, RK3576_DCPHY_SC_GNR_CON1,
                      RK3576_DCPHY_RX_GNR_CON1_VAL);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_SC_ANA_CON1,
                      RK3576_DCPHY_RX_SC_ANA_CON1_VAL);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_SC_ANA_CON2,
                      RK3576_DCPHY_RX_SC_ANA_CON2_VAL);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_SC_ANA_CON3,
                      RK3576_DCPHY_RX_SC_ANA_CON3_VAL);
  rk3576_dcphy_putreg(base, RK3576_DCPHY_SC_TIME_CON0,
                      RK3576_DCPHY_RX_SC_TIME_CON0_VAL);

  for (lane = 0; lane < lanes; lane++)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_GNR_CON1(lane),
                          RK3576_DCPHY_RX_GNR_CON1_VAL);
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_ANA_CON1(lane),
                          RK3576_DCPHY_RX_SD_ANA_CON1_VAL);
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_ANA_CON2(lane),
                          dlysel | RK3576_DCPHY_RX_SD_ANA_CON2_TERM);
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_ANA_CON3(lane),
                          RK3576_DCPHY_RX_SD_ANA_CON3_VAL);
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_TIME_CON0(lane), settle);
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_TIME_CON1(lane),
                          RK3576_DCPHY_RX_SD_TIME_CON1_VAL);

      /* ANA_CON7 and the deskew pair exist only on the COMBO data lane
       * banks (SD0..SD2); SD3 has a reduced layout.  Deskew calibration is
       * only needed from 1.5 Gbps up, so below that the registers are left
       * at reset.
       */

      if (lane < 3)
        {
          rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_ANA_CON7(lane),
                              RK3576_DCPHY_RX_SD_ANA_CON7_VAL);

          if (lane_mbps >= RK3576_DCPHY_RX_DESKEW_MIN_MBPS)
            {
              rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_DESKEW_CON0(lane),
                                  RK3576_DCPHY_RX_SD_DESKEW_CON0_VAL);
              rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_DESKEW_CON4(lane),
                                  RK3576_DCPHY_RX_SD_DESKEW_CON4_VAL);
            }
        }
    }

  /* Step 8: enable the clock lane and the data lanes. */

  rk3576_dcphy_putreg(base, RK3576_DCPHY_SC_GNR_CON0, DCPHY_GNR_CON0_ENABLE);

  for (lane = 0; lane < lanes; lane++)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_GNR_CON0(lane),
                          DCPHY_GNR_CON0_ENABLE);
    }

  /* Step 9: wait for PHY_READY on the clock lane and on every data lane.
   * This is the step that proves the lanes really powered up; without it a
   * mis-powered PHY would fail later, inside the CSI HOST, where the cause
   * is much harder to see.
   */

  ret = rk3576_dcphy_wait_lane_ready(base, RK3576_DCPHY_SC_GNR_CON0);
  if (ret < 0)
    {
      _err("DCPHY: RX clock lane PHY_READY timeout\n");
      goto errout_unlock;
    }

  for (lane = 0; lane < lanes; lane++)
    {
      ret = rk3576_dcphy_wait_lane_ready(base, RK3576_DCPHY_SD_GNR_CON0(lane));
      if (ret < 0)
        {
          _err("DCPHY: RX data lane %u PHY_READY timeout\n", lane);
          goto errout_unlock;
        }
    }

  /* Step 10: release S_RESETN.  The pads then settle into LP-11, which is
   * what the CSI HOST reports as the stop state.
   */

  rk3576_dcphy_deassert_s_reset(priv);

  priv->rx_lanes = lanes;
  priv->rx_lane_mbps = lane_mbps;
  priv->rx_powered = true;

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_dcphy_rx_power_off
 ****************************************************************************/

int rk3576_dcphy_rx_power_off(void)
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

  if (!priv->rx_powered)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  base = priv->base;

  /* Hold the slave lanes in reset, then disable the data lanes and the
   * clock lane.  The BIAS block and the APB clocks are deliberately left
   * alone: the DSI side may still be running on them.
   */

  rk3576_dcphy_assert_s_reset(priv);

  for (lane = 0; lane < priv->rx_lanes; lane++)
    {
      rk3576_dcphy_putreg(base, RK3576_DCPHY_SD_GNR_CON0(lane), 0x0000);
    }

  rk3576_dcphy_putreg(base, RK3576_DCPHY_SC_GNR_CON0, 0x0000);

  priv->rx_powered = false;
  priv->rx_lanes = 0;

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_dcphy_rx_is_ready
 ****************************************************************************/

bool rk3576_dcphy_rx_is_ready(uint8_t lanes)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  unsigned int lane;
  bool ready = true;
  int ret;

  if (lanes < 1 || lanes > 4)
    {
      return false;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return false;
    }

  if (!priv->rx_powered || priv->rx_lanes < lanes)
    {
      ready = false;
      goto out;
    }

  if ((rk3576_dcphy_getreg(priv->base, RK3576_DCPHY_SC_GNR_CON0) &
       DCPHY_GNR_CON0_PHY_READY) == 0)
    {
      ready = false;
      goto out;
    }

  for (lane = 0; lane < lanes; lane++)
    {
      if ((rk3576_dcphy_getreg(priv->base, RK3576_DCPHY_SD_GNR_CON0(lane)) &
           DCPHY_GNR_CON0_PHY_READY) == 0)
        {
          ready = false;
          break;
        }
    }

out:
  nxmutex_unlock(&priv->lock);
  return ready;
}

/****************************************************************************
 * Name: rk3576_dcphy_rx_read_grf_status0 / _read_grf_status2
 ****************************************************************************/

uint32_t rk3576_dcphy_rx_read_grf_status0(void)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  uint32_t value = 0;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return 0;
    }

  if (priv->grf != 0)
    {
      value = rk3576_dcphy_getreg(priv->grf, RK3576_DCPHY_GRF_STATUS0_OFF);
    }

  nxmutex_unlock(&priv->lock);
  return value;
}

uint32_t rk3576_dcphy_rx_read_grf_status2(void)
{
  struct rk3576_dcphy_s *priv = &g_dcphy;
  uint32_t value = 0;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return 0;
    }

  if (priv->grf != 0)
    {
      value = rk3576_dcphy_getreg(priv->grf, RK3576_DCPHY_GRF_STATUS2_OFF);
    }

  nxmutex_unlock(&priv->lock);
  return value;
}

#endif /* CONFIG_RK3576_MIPI_DCPHY */
