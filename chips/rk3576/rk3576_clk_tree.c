/****************************************************************************
 * chips/rk3576/rk3576_clk_tree.c
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
 * RK3576 Clock Tree — NuttX CLK Framework integration.
 *
 * Registers the RK3576 clock tree using the standard NuttX clk_register_*
 * helpers.  The implementation mirrors the register knowledge already
 * present in rk3576_cru.c, but wraps it in the CLK framework so that
 * peripheral drivers can use clk_get() / clk_enable() / clk_set_rate().
 *
 * Rockchip uses a hiword-mask write scheme:  bits [31:16] are the write-
 * enable mask, bits [15:0] are the value.  The NuttX CLK framework's
 * CLK_GATE_HIWORD_MASK / CLK_MUX_HIWORD_MASK flags match this exactly.
 *
 * Reference: Rockchip RK3576 TRM, Chapter 2 "Clock and Reset Unit".
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <debug.h>
#include <errno.h>
#include <nuttx/config.h>
#include <stdint.h>
#include <sys/param.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clk/clk_provider.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_clk_tree.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PLL private data passed via clk->private_data.
 *
 * RK3576 has three PLL types with different formulas:
 *   FRACPLL:  FOUT = ((m + k/65536) * FIN) / (p * 2^s)
 *   DDRPLL:   FOUT = ((m + k/65536) * 2 * FIN) / (p * 2^s)
 *   INTPLL:   FOUT = (m * FIN) / (p * 2^s)
 *
 * GPLL and CPLL are both FRACPLLs.  Other types will be added later.
 *
 * Register layout for FRACPLL:
 *   CON0[9:0]    = m (FBDIV, 10-bit main divider, 64 <= m <= 1023)
 *   CON1[5:0]    = p (REFDIV, 6-bit pre-divider, 1 <= p <= 63)
 *   CON1[8:6]    = s (POSTDIV2 exponent, 3-bit scaler, 0 <= s <= 6)
 *   CON2[15:0]   = k (FRAC, 16-bit two's complement DSM value)
 */

struct rk3576_fracpll_s
{
  uintptr_t con_base; /* CON0 register address (CON1/2 are +4/+8) */
  uintptr_t lock_reg; /* Lock status register address (optional) */
  uint8_t lock_bit;   /* Lock bit position in lock_reg (0-31, 0 if unused) */
};

/* Forward declaration */

static uint32_t rk3576_fracpll_recalc_rate(struct clk_s *clk,
                                           uint32_t parent_rate);
static int rk3576_fracpll_set_rate(FAR struct clk_s *clk, uint32_t rate,
                                   uint32_t parent_rate);
static uint32_t rk3576_fracpll_round_rate(FAR struct clk_s *clk, uint32_t rate,
                                          FAR uint32_t *parent_rate);

/* Rate table entry for FRACPLL */
struct rk3576_pll_rate
{
  uint32_t rate; /* Target frequency in Hz */
  uint8_t p;     /* REFDIV */
  uint16_t m;    /* FBDIV */
  uint8_t s;     /* POSTDIV2 */
  int16_t k;     /* FRAC (two's complement) */
};

/* LPLL rate table — verified against Linux rockchip rk3576_pll_rates[]
 * (drivers/clk/rockchip/clk-rk3576.c).  Each entry is (rate, p, m, s, k)
 * in the Linux (rate, _p, _m, _s, _k) encoding.  Only frequencies present
 * in the upstream table are kept; the FRACPLL formula is
 *   FOUT = ((m + k/65536) * 24MHz) / (p * 2^s)
 * so m must be large enough for the VCO to reach the FRACPLL lock range
 * (950MHz..3800MHz) — this is why m is ~200-368, never the CPU divider
 * value that was previously (incorrectly) placed here.
 */
/* clang-format off */
static const struct rk3576_pll_rate g_lpll_rate_table[] = {
  { 2400000000, 2, 200, 0, 0 },
  { 2304000000, 2, 192, 0, 0 },
  { 2208000000, 2, 368, 1, 0 },
  { 2184000000, 2, 364, 1, 0 },
  { 2088000000, 2, 348, 1, 0 },
  { 2040000000, 2, 340, 1, 0 },
  { 2016000000, 2, 336, 1, 0 },
  { 1992000000, 2, 332, 1, 0 },
  { 1896000000, 2, 316, 1, 0 },
  { 1800000000, 2, 300, 1, 0 },
  { 1704000000, 2, 284, 1, 0 },
  { 1608000000, 2, 268, 1, 0 },
  { 1584000000, 2, 264, 1, 0 },
  { 1560000000, 2, 260, 1, 0 },
  { 1536000000, 2, 256, 1, 0 },
  { 1512000000, 2, 252, 1, 0 },
  { 1488000000, 2, 248, 1, 0 },
  { 1464000000, 2, 244, 1, 0 },
  { 1440000000, 2, 240, 1, 0 },
  { 1416000000, 2, 236, 1, 0 },
  { 1392000000, 2, 232, 1, 0 },
  { 1320000000, 2, 220, 1, 0 },
  { 1200000000, 2, 200, 1, 0 },
  { 1008000000, 2, 336, 2, 0 },
  {  816000000, 2, 272, 2, 0 },
  {  600000000, 2, 200, 2, 0 },
  {  408000000, 2, 272, 3, 0 },
  {  312000000, 2, 208, 3, 0 },
  {  216000000, 2, 288, 4, 0 },
  {   96000000, 2, 256, 5, 0 },
};
/* clang-format on */

#define G_LPLL_RATE_TABLE_SIZE nitems(g_lpll_rate_table)

/* Read-only ops for GPLL/CPLL/AUPLL — prevents propagation from child clocks
 */
static const struct clk_ops_s g_rk3576_fracpll_readonly_ops = {
  .recalc_rate = rk3576_fracpll_recalc_rate,
};

/* Configurable ops for LPLL only — allows clk_set_rate(clk_lpll, ...) */
static const struct clk_ops_s g_rk3576_fracpll_configurable_ops = {
  .recalc_rate = rk3576_fracpll_recalc_rate,
  .round_rate = rk3576_fracpll_round_rate,
  .set_rate = rk3576_fracpll_set_rate,
};

/****************************************************************************
 * Name: rk3576_fracpll_recalc_rate
 *
 * Description:
 *   Recalculate FRACPLL output frequency from CON0..CON2 registers.
 *   FOUT = ((m + k/65536) * FIN) / (p * 2^s)
 *   Where m=CON0[9:0], p=CON1[5:0], s=CON1[8:6], k=CON2[15:0].
 *
 *   parent_rate = FIN (xin_osc0 = 24 MHz).
 *
 *   k is a 16-bit two's complement integer, so we treat it as int16_t.
 *   To avoid floating-point, compute:
 *     FOUT = ((m * 65536 + k) * FIN) / (p * 65536 * (1 << s))
 *   using 64-bit arithmetic to prevent overflow.
 ****************************************************************************/

static uint32_t rk3576_fracpll_recalc_rate(struct clk_s *clk,
                                           uint32_t parent_rate)
{
  struct rk3576_fracpll_s *pll = clk->private_data;
  uint32_t con0;
  uint32_t con1;
  uint32_t con2;
  uint32_t m;
  uint32_t p;
  uint32_t s;
  int16_t k;
  uint64_t numerator;
  uint64_t denominator;

  DEBUGASSERT(pll);
  DEBUGASSERT(parent_rate == CONFIG_RK3576_OSC_FREQ);

  con0 = getreg32(pll->con_base);     /* CON0 */
  con1 = getreg32(pll->con_base + 4); /* CON1 */
  con2 = getreg32(pll->con_base + 8); /* CON2 */

  m = con0 & 0x3ff;             /* CON0[9:0]   */
  p = con1 & 0x3f;              /* CON1[5:0]   */
  s = (con1 >> 6) & 0x7;        /* CON1[8:6]   */
  k = (int16_t)(con2 & 0xffff); /* CON2[15:0], two's complement */

  /* Guard against invalid register values. */

  if (p == 0 || m < 64 || m > 1023 || s > 6)
    {
      return 0;
    }

  /* FOUT = ((m * 65536 + k) * FIN) / (p * 65536 * (1 << s))
   *
   * Compute numerator and denominator separately in 64-bit to
   * preserve precision, then divide.
   */

  numerator = (uint64_t)parent_rate * ((uint64_t)m * 65536 + (int64_t)k);
  denominator = (uint64_t)p * 65536 * (1ULL << s);

  return (uint32_t)(numerator / denominator);
}

/****************************************************************************
 * Name: rk3576_fracpll_find_rate
 *
 * Description:
 *   Find the closest matching rate from the LPLL rate table.
 *   Returns pointer to the best match, or NULL if no valid entry found.
 ****************************************************************************/

static const struct rk3576_pll_rate *
rk3576_fracpll_find_rate(uint32_t target_rate)
{
  const struct rk3576_pll_rate *best = NULL;
  uint32_t best_diff = UINT32_MAX;
  int i;

  for (i = 0; i < G_LPLL_RATE_TABLE_SIZE; i++)
    {
      uint32_t diff;
      uint32_t table_rate = g_lpll_rate_table[i].rate;

      if (table_rate == target_rate)
        {
          return &g_lpll_rate_table[i];
        }

      diff = (target_rate > table_rate) ? (target_rate - table_rate)
                                        : (table_rate - target_rate);

      if (diff < best_diff)
        {
          best_diff = diff;
          best = &g_lpll_rate_table[i];
        }
    }

  return best;
}

/****************************************************************************
 * Name: rk3576_fracpll_round_rate
 *
 * Description:
 *   Round the requested rate to the nearest supported frequency from the
 *   LPLL rate table.  This is called by the CLK framework during rate
 *   negotiation to determine what rate the PLL can actually produce.
 ****************************************************************************/

static uint32_t rk3576_fracpll_round_rate(FAR struct clk_s *clk, uint32_t rate,
                                          FAR uint32_t *parent_rate)
{
  const struct rk3576_pll_rate *entry;

  DEBUGASSERT(clk);
  DEBUGASSERT(parent_rate);
  DEBUGASSERT(*parent_rate == CONFIG_RK3576_OSC_FREQ);

  entry = rk3576_fracpll_find_rate(rate);
  if (!entry)
    {
      _err("CLK: no valid LPLL rate found for %u Hz\n", rate);
      return 0;
    }

  /* Return the actual achievable rate */
  return entry->rate;
}

/****************************************************************************
 * Name: rk3576_fracpll_set_rate
 *
 * Description:
 *   Reprogram the FRACPLL to the target frequency.
 *
 *   Sequence (reference: Linux rockchip_rk3588_pll_set_params, which the
 *   RK3576 LPLL (pll_rk3588_core) uses):
 *   1. Power down PLL     (CON1[13] PWRDOWN = 1)
 *   2. Write new m/p/s/k to CON0/1/2 using hiword-mask
 *      (CON1 must keep PWRDOWN=1 while being reprogrammed)
 *   3. Power up PLL       (CON1[13] PWRDOWN = 0)
 *   4. Poll lock bit (CON6[15]) until PLL locks (timeout ~1ms)
 *
 *   bit13 of CON1 is PWRDOWN (active high).  Setting it powers the PLL
 *   down; clearing it lets the PLL run.  Getting this backwards means the
 *   PLL is left powered down and can never lock.
 *
 *   NOTE: Caller must ensure CPU is on a safe clock source before calling
 *   this function (see rk3576_clk_set_litcore_cpufreq).
 ****************************************************************************/

/* Program LPLL m/p/s/k parameters using the hiword-mask idiom, leaving the
 * PLL powered down while the new values are written, then power it up and
 * poll the lock bit.  Returns OK once locked, or -ETIMEDOUT on lock timeout.
 *
 * The caller (rk3576_clk_set_litcore_cpufreq) must keep the CPU on a safe
 * clock source (GPLL) while this runs; on failure the previous parameters
 * are programmed back so callers can safely resume the old frequency.
 */

static int rk3576_fracpll_program(FAR struct clk_s *clk,
                                  const struct rk3576_pll_rate *entry)
{
  struct rk3576_fracpll_s *pll = clk->private_data;
  uint32_t con0, con1, con2;
  uint32_t regval;
  int timeout;

  DEBUGASSERT(pll);
  DEBUGASSERT(entry);

  /* Step 1: Power down PLL (CON1[13] PWRDOWN = 1). */

  regval = getreg32(pll->con_base + 4); /* CON1 */
  regval |= RK3576_LPLL_CON1_PWRDOWN;
  putreg32(regval | (0xffff << 16), pll->con_base + 4);

  /* Small delay to ensure power-down takes effect */
  up_udelay(1);

  /* Step 2: Write new parameters using hiword-mask. */

  /* CON0: m[9:0], bypass cleared */
  con0 = (entry->m & RK3576_LPLL_CON0_M_MASK) << RK3576_LPLL_CON0_M_SHIFT;
  putreg32(con0 | (0xffff << 16), pll->con_base);

  /* CON1: p[5:0], s[8:6] — keep PWRDOWN=1 so the PLL stays off while being
   * reprogrammed.  Do NOT clear PWRDOWN here or the PLL may attempt to run
   * with intermediate/undefined dividers.
   */
  con1 = ((entry->p & RK3576_LPLL_CON1_P_MASK) << RK3576_LPLL_CON1_P_SHIFT) |
         ((entry->s & RK3576_LPLL_CON1_S_MASK) << RK3576_LPLL_CON1_S_SHIFT) |
         RK3576_LPLL_CON1_PWRDOWN;
  putreg32(con1 | (0xffff << 16), pll->con_base + 4);

  /* CON2: k[15:0] */
  con2 = (uint16_t)entry->k;
  putreg32(con2 | (0xffff << 16), pll->con_base + 8);

  /* Step 3: Power up PLL (CON1[13] PWRDOWN = 0). */

  regval = getreg32(pll->con_base + 4); /* CON1 */
  regval &= ~RK3576_LPLL_CON1_PWRDOWN;
  putreg32(regval | (0xffff << 16), pll->con_base + 4);

  /* Step 4: Poll lock bit (CON6[15]) */

  if (pll->lock_reg && pll->lock_bit < 32)
    {
      timeout = 1000; /* 1ms timeout */
      while (timeout-- > 0)
        {
          regval = getreg32(pll->lock_reg);
          if (regval & (1 << pll->lock_bit))
            {
              break;
            }
          up_udelay(1);
        }

      if (timeout <= 0)
        {
          _err("CLK: LPLL failed to lock!\n");
          return -ETIMEDOUT;
        }
    }

  _info("CLK: LPLL locked at %u Hz\n", entry->rate);
  return OK;
}

static int rk3576_fracpll_set_rate(FAR struct clk_s *clk, uint32_t rate,
                                   uint32_t parent_rate)
{
  struct rk3576_fracpll_s *pll = clk->private_data;
  const struct rk3576_pll_rate *entry;
  uint32_t saved_con0, saved_con1, saved_con2;
  int ret;

  DEBUGASSERT(pll);
  DEBUGASSERT(parent_rate == CONFIG_RK3576_OSC_FREQ);

  /* Find the closest supported rate */
  entry = rk3576_fracpll_find_rate(rate);
  if (!entry)
    {
      _err("CLK: unsupported LPLL rate %u Hz\n", rate);
      return -EINVAL;
    }

  _info("CLK: reprogramming LPLL from %u Hz to %u Hz (m=%u p=%u s=%u k=%d)\n",
        clk->rate, entry->rate, entry->m, entry->p, entry->s, entry->k);

  /* Snapshot the current CON0/1/2 so they can be restored on lock failure.
   * Mask off the hiword-mask (write-enable) bits so the raw parameter
   * values remain; PWRDOWN is re-applied during restore programming.
   */

  saved_con0 = getreg32(pll->con_base) & RK3576_LPLL_CON0_M_MASK;
  saved_con1 = getreg32(pll->con_base + 4) &
               (RK3576_LPLL_CON1_PWRDOWN |
                (RK3576_LPLL_CON1_S_MASK << RK3576_LPLL_CON1_S_SHIFT) |
                RK3576_LPLL_CON1_P_MASK);
  saved_con2 = getreg32(pll->con_base + 8) & 0xffff;

  ret = rk3576_fracpll_program(clk, entry);
  if (ret < 0)
    {
      /* Lock failed — the PLL is now programmed with parameters that did
       * not lock.  Rewrite the previous parameters so the PLL returns to
       * its old (known-good) frequency, and report the failure so the
       * caller keeps the CPU on the safe source instead of switching back
       * to a possibly-unlocked PLL.
       */

      struct rk3576_pll_rate old = {
        .rate = clk->rate,
        .m = saved_con0,
        .p = saved_con1 & RK3576_LPLL_CON1_P_MASK,
        .s =
            (saved_con1 >> RK3576_LPLL_CON1_S_SHIFT) & RK3576_LPLL_CON1_S_MASK,
        .k = (int16_t)saved_con2,
      };

      _err("CLK: LPLL lock failed, restoring previous parameters\n");

      /* Best-effort restore: ignore its return value; the primary error is
       * still the original lock timeout.  Even if the restore also fails to
       * lock, the caller will still see a failure and keep the CPU on GPLL.
       */

      rk3576_fracpll_program(clk, &old);
      return ret;
    }

  return OK;
}

/* Shared parent name arrays for muxes.
 * Order matches the hardware 2-bit select encoding.
 * I2C:  00=GPLL/6, 01=CPLL/10, 10=CPLL/20, 11=XIN_OSC0
 * PWM:  00=CPLL/10, 01=CPLL/20, 10=XIN_OSC0, 11=invalid
 */

#ifdef CONFIG_RK3576_I2C
static const char *g_i2c_sel_parents[] = {
  "clk_gpll_div6",  /* 0b00 */
  "clk_cpll_div10", /* 0b01 */
  "clk_cpll_div20", /* 0b10 */
  "xin_osc0",       /* 0b11 */
};
#endif

#ifdef CONFIG_RK3576_PWM
static const char *g_pwm_sel_parents[] = {
  "clk_cpll_div10", /* 0b00 */
  "clk_cpll_div20", /* 0b01 */
  "xin_osc0",       /* 0b10 */
  "xin_osc0",       /* 0b11 — undefined, fallback */
};
#endif

#ifdef CONFIG_RK3576_UART

/* UART frac clock source selection */

static const char *g_matrix_uart_frac_sel_parents[] = {
  "clk_gpll",  /* 0b00: clk_gpll_mux */
  "clk_cpll",  /* 0b01: clk_cpll_mux */
  "clk_aupll", /* 0b10: clk_aupll_mux */
  "xin_osc0",  /* 0b11: xin_osc0_func_mux */
};

/* UART sclk source selection (7 parents, 3-bit select).
 * Used by UART0, 2–11 sclk_uartN_sel muxes (NOT UART1).
 * Order matches TRM encoding:
 *   0b000: clk_gpll_mux
 *   0b001: clk_cpll_mux
 *   0b010: clk_aupll_mux
 *   0b011: xin_osc0_func_mux
 *   0b100: clk_matrix_uart_frac_0
 *   0b101: clk_matrix_uart_frac_1
 *   0b110: clk_matrix_uart_frac_2
 *
 * UART1 uses a different, two-level mux structure — see
 * rk3576_clk_register_uart() for details.
 */

static const char *g_uart_sclk_sel_parents[] = {
  "clk_gpll",               /* 0b000 */
  "clk_cpll",               /* 0b001 */
  "clk_aupll",              /* 0b010 */
  "xin_osc0",               /* 0b011 */
  "clk_matrix_uart_frac_0", /* 0b100 */
  "clk_matrix_uart_frac_1", /* 0b101 */
  "clk_matrix_uart_frac_2", /* 0b110 */
};

/* UART1 sclk parent list — used by sclk_uart1_sel mux.
 * 0 = clk_uart1_src_top (programmable), 1 = xin_osc0 (24 MHz bypass).
 */

static const char *g_uart1_sclk_parents[] = {
  "clk_uart1_src_top", /* 1'b0 */
  "xin_osc0",          /* 1'b1 */
};

#endif /* CONFIG_RK3576_UART */

/* Audio frac clock source selection */

static const char *g_matrix_audio_frac_sel_parents[] = {
  "clk_gpll",  /* 0b00: clk_gpll_mux */
  "clk_cpll",  /* 0b01: clk_cpll_mux */
  "clk_aupll", /* 0b10: clk_aupll_mux */
  "xin_osc0",  /* 0b11: xin_osc0_func_mux */
};

/* SAI mclk source selection (8 parents, 3-bit select).
 * Used by all SAI0~9 mclk_saiX_src_sel muxes.
 * Order matches TRM encoding:
 *   0b000: xin_osc0
 *   0b001: clk_matrix_audio_frac_0
 *   0b010: clk_matrix_audio_frac_1
 *   0b011: clk_matrix_audio_frac_2
 *   0b100: clk_matrix_audio_frac_3
 *   0b101: clk_matrix_audio_int_0
 *   0b110: clk_matrix_audio_int_1
 *   0b111: clk_matrix_audio_int_2
 */

#ifdef CONFIG_RK3576_SAI
static const char *g_sai_mclk_src_parents[] = {
  "xin_osc0",                /* 0b000 */
  "clk_matrix_audio_frac_0", /* 0b001 */
  "clk_matrix_audio_frac_1", /* 0b010 */
  "clk_matrix_audio_frac_2", /* 0b011 */
  "clk_matrix_audio_frac_3", /* 0b100 */
  "clk_matrix_audio_int_0",  /* 0b101 */
  "clk_matrix_audio_int_1",  /* 0b110 */
  "clk_matrix_audio_int_2",  /* 0b111 */
};
#endif

/* FSPI: 00=GPLL, 01=CPLL, 10=XIN_OSC0, 11=invalid. */

#ifdef CONFIG_RK3576_FSPI
static const char *g_fspi_sel_parents[] = {
  "clk_gpll", /* 0b00 */
  "clk_cpll", /* 0b01 */
  "xin_osc0", /* 0b10 */
  "xin_osc0", /* 0b11 — undefined, fallback */
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_clk_register_pll_factors
 *
 * Description:
 *   Register PLLs as dynamically-calculated sources (rate derived from PLL CON
 *   registers at runtime), and register their post-dividers (fixed-factor
 *   clocks).  PLL CON registers are read-only from the driver's perspective —
 *   the bootloader owns the PLL configuration.
 ****************************************************************************/

static void rk3576_clk_register_pll_factors(void)
{
  struct clk_s *gpll, *cpll, *aupll;
  static struct rk3576_fracpll_s gpll_priv, cpll_priv, aupll_priv;
  static const char *g_pll_parents[] = { "xin_osc0" };

  /* Root oscillator — 24 MHz */

  clk_register_fixed_rate("xin_osc0", NULL, CLK_NAME_IS_STATIC,
                          CONFIG_RK3576_OSC_FREQ);

  /* GPLL (FRACPLL) — rate derived from GPLL_CON(0..2) at runtime.
   * Uses read-only ops to prevent child clocks from changing PLL frequency.
   * Parent is xin_osc0 so the CLK framework provides 24 MHz to recalc_rate.
   */

  gpll_priv.con_base = RK3576_CRU_ADDR + RK3576_CRU_GPLL_CON(0);
  gpll_priv.lock_reg = 0;
  gpll_priv.lock_bit = 0;

  gpll = clk_register("clk_gpll", g_pll_parents, 1,
                      CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                      &g_rk3576_fracpll_readonly_ops, &gpll_priv,
                      sizeof(gpll_priv));
  DEBUGASSERT(gpll);
  UNUSED(gpll);

  clk_register_fixed_factor("clk_gpll_div2", "clk_gpll", CLK_NAME_IS_STATIC, 1,
                            2);
  clk_register_fixed_factor("clk_gpll_div3", "clk_gpll", CLK_NAME_IS_STATIC, 1,
                            3);
  clk_register_fixed_factor("clk_gpll_div4", "clk_gpll", CLK_NAME_IS_STATIC, 1,
                            4);
  clk_register_fixed_factor("clk_gpll_div6", "clk_gpll", CLK_NAME_IS_STATIC, 1,
                            6);
  clk_register_fixed_factor("clk_gpll_div8", "clk_gpll", CLK_NAME_IS_STATIC, 1,
                            8);

  /* CPLL (FRACPLL) — rate derived from CPLL_CON(0..2) at runtime.
   * Uses read-only ops to prevent child clocks from changing PLL frequency.
   * Parent is xin_osc0 so the CLK framework provides 24 MHz to recalc_rate.
   */

  cpll_priv.con_base = RK3576_CRU_ADDR + RK3576_CRU_CPLL_CON(0);
  cpll_priv.lock_reg = 0;
  cpll_priv.lock_bit = 0;

  cpll = clk_register("clk_cpll", g_pll_parents, 1,
                      CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                      &g_rk3576_fracpll_readonly_ops, &cpll_priv,
                      sizeof(cpll_priv));
  DEBUGASSERT(cpll);
  UNUSED(cpll);

  clk_register_fixed_factor("clk_cpll_div2", "clk_cpll", CLK_NAME_IS_STATIC, 1,
                            2);
  clk_register_fixed_factor("clk_cpll_div4", "clk_cpll", CLK_NAME_IS_STATIC, 1,
                            4);
  clk_register_fixed_factor("clk_cpll_div10", "clk_cpll", CLK_NAME_IS_STATIC,
                            1, 10);
  clk_register_fixed_factor("clk_cpll_div20", "clk_cpll", CLK_NAME_IS_STATIC,
                            1, 20);

  /* AUPLL (FRACPLL) — rate derived from AUPLL_CON(0..2) at runtime.
   * Uses read-only ops to prevent child clocks from changing PLL frequency.
   * Parent is xin_osc0 so the CLK framework provides 24 MHz to recalc_rate.
   */
  aupll_priv.con_base = RK3576_CRU_ADDR + RK3576_CRU_AUPLL_CON(0);
  aupll_priv.lock_reg = 0;
  aupll_priv.lock_bit = 0;
  aupll = clk_register("clk_aupll", g_pll_parents, 1,
                       CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                       &g_rk3576_fracpll_readonly_ops, &aupll_priv,
                       sizeof(aupll_priv));

  DEBUGASSERT(aupll);
  UNUSED(aupll);
}

/****************************************************************************
 * Name: rk3576_clk_register_axi
 *
 * Description:
 *   Register the AXI (aclk) bus root clocks.  These are the parent clocks
 *   for all AXI-domain peripherals (DMA controllers, etc.).  Each root clock
 *   is a (mux + optional divider) chain fed by the PLL / PLL-divider sources
 *   registered in rk3576_clk_register_pll_factors().
 *
 *   Register map (TRM Chapter 2):
 *     aclk_top_biu      CLKSEL_CON09  sel[6:5] div[4:0]
 *     aclk_bus_root     CLKSEL_CON55  sel[9]   div[8:4]
 *     aclk_center_root  sel@CON168[7:5]  div@CON167[13:9]
 *       (mux and divider sit in different CLKSEL registers)
 ****************************************************************************/

static void rk3576_clk_register_axi(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* AXI top BIU: 2-bit mux (GPLL/CPLL/AUPLL) + 5-bit divider. */

  {
    static const char *parents[] = {
      "clk_gpll",  /* 2'b00 */
      "clk_cpll",  /* 2'b01 */
      "clk_aupll", /* 2'b10 */
    };

    clk_register_mux("aclk_top_biu_sel", parents, nitems(parents),
                     CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                     cru + RK3576_CRU_CLKSEL_CON(9), 5, 2,
                     CLK_MUX_HIWORD_MASK);
    clk_register_divider("aclk_top_biu", "aclk_top_biu_sel",
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(9), 0, 5,
                         CLK_DIVIDER_HIWORD_MASK);
  }

  /* AXI bus root: 1-bit mux (GPLL/CPLL) + 5-bit divider. */

  {
    static const char *parents[] = {
      "clk_gpll", /* 1'b0 */
      "clk_cpll", /* 1'b1 */
    };

    clk_register_mux("aclk_bus_root_sel", parents, nitems(parents),
                     CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                     cru + RK3576_CRU_CLKSEL_CON(55), 9, 1,
                     CLK_MUX_HIWORD_MASK);
    clk_register_divider("aclk_bus_root", "aclk_bus_root_sel",
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(55), 4, 5,
                         CLK_DIVIDER_HIWORD_MASK);
  }

  /* AXI center root: 3-bit mux + 5-bit divider.
   *   mux (aclk_center_root_sel) @ CLKSEL_CON168[7:5]
   *   div (aclk_center_root_div) @ CLKSEL_CON167[13:9]  (div_con + 1)
   *   Unlike aclk_top_biu/aclk_bus_root, the center-root mux and divider
   *   live in DIFFERENT CLKSEL registers (per TRM CON167/CON168).
   */

  {
    static const char *parents[] = {
      "clk_gpll",  /* 3'b000 */
      "clk_cpll",  /* 3'b001 */
      "clk_cpll",  /* 3'b010 — clk_spll_mux, not yet modelled */
      "clk_aupll", /* 3'b011 */
      "clk_cpll",  /* 3'b100 — clk_bpll_src, not yet modelled */
    };

    clk_register_mux("aclk_center_root_sel", parents, nitems(parents),
                     CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                     cru + RK3576_CRU_CLKSEL_CON(168), 5, 3,
                     CLK_MUX_HIWORD_MASK);
    clk_register_divider("aclk_center_root", "aclk_center_root_sel",
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(167), 9, 5,
                         CLK_DIVIDER_HIWORD_MASK);
  }
}

/****************************************************************************
 * Name: rk3576_clk_register_ahb
 *
 * Description:
 *   Register the AHB (hclk) bus root clocks.  These are the parent clocks
 *   for all AHB-domain peripherals (FSPI, etc.).  No divider is modelled —
 *   the AHB roots are pure muxes fed by GPLL/CPLL post-dividers.
 *
 *   Register map (TRM Chapter 2):
 *     hclk_top_biu      CLKSEL_CON19  sel[3:2]
 *     hclk_bus_root     CLKSEL_CON55  sel[1:0]
 *     hclk_center_root  CLKSEL_CON168 sel[11:10]
 ****************************************************************************/

static void rk3576_clk_register_ahb(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* Common 2-bit AHB source list:
   * 2'b00 = GPLL/6, 2'b01 = CPLL/10, 2'b10 = CPLL/20, 2'b11 = XIN_OSC0.
   */

  static const char *parents[] = {
    "clk_gpll_div6",
    "clk_cpll_div10",
    "clk_cpll_div20",
    "xin_osc0",
  };

  clk_register_mux("hclk_top_biu", parents, nitems(parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(19), 2, 2, CLK_MUX_HIWORD_MASK);

  clk_register_mux("hclk_bus_root", parents, nitems(parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(55), 0, 2, CLK_MUX_HIWORD_MASK);

  clk_register_mux("hclk_center_root", parents, nitems(parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(168), 10, 2,
                   CLK_MUX_HIWORD_MASK);

  /* Audio-domain AHB root — parent of SAI/PDM/etc. bus interface gates.
   * Source mux only (no divider).
   */

  clk_register_mux("hclk_audio_root", parents, nitems(parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(42), 0, 2, CLK_MUX_HIWORD_MASK);
}

/****************************************************************************
 * Name: rk3576_clk_register_apb
 *
 * Description:
 *   Register the APB (pclk) bus root clocks.  These are the parent clocks
 *   for all APB-domain peripherals (UART, I2C, PWM, timers, etc.).
 *
 *   Register map (TRM Chapter 2):
 *     pclk_top_root     CLKSEL_CON08  sel[8:7]
 *     pclk_bus_root     CLKSEL_CON55  sel[3:2]
 *     pclk_center_root  CLKSEL_CON168 sel[13:12]
 ****************************************************************************/

static void rk3576_clk_register_apb(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* Common 2-bit APB source list:
   * 2'b00 = CPLL/10, 2'b01 = CPLL/20, 2'b10 = XIN_OSC0.
   */

  static const char *parents[] = {
    "clk_cpll_div10",
    "clk_cpll_div20",
    "xin_osc0",
  };

  clk_register_mux("pclk_top_root", parents, nitems(parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(8), 7, 2, CLK_MUX_HIWORD_MASK);

  clk_register_mux("pclk_bus_root", parents, nitems(parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(55), 2, 2, CLK_MUX_HIWORD_MASK);

  clk_register_mux("pclk_center_root", parents, nitems(parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(168), 12, 2,
                   CLK_MUX_HIWORD_MASK);

  /* PMU1-domain APB root — parent of PMU-domain peripherals (pclk_uart1,
   * pclk_i2c0, pclk_pwm0).  Source mux only (no divider).
   */

  {
    const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;

    clk_register_mux("pclk_pmu0_root_src", parents, nitems(parents),
                     CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                     pmu1 + RK3576_PMU1CRU_CLKSEL_CON(20), 0, 2,
                     CLK_MUX_HIWORD_MASK);
  }
}

/* LITCORE (little-core power domain) clock sources.
 *
 * The little-core cluster (aclk_m_litcore / clk_litcore / pclk_litcore /
 * pclk_dbg_litcore) is clocked from the LITCORE_CRU (0x27240000).
 *
 * Parent selection for clk_litcore_src_sel (2-bit, CLKSEL_CON00[13:12]):
 *   2'b00: clk_lpll_mux
 *   2'b01: clk_gpll_mux
 *   2'b10: clk_litcore_pvtpll_src
 *
 * clk_litcore_sel (2-bit, CLKSEL_CON01[7:6]):
 *   2'b00: clk_litcore_src_out
 *   2'b01: clk_litcore_pvtpll_src
 *   2'b10: clk_litcore_clean
 */

static const char *g_litcore_src_sel_parents[] = {
  "clk_lpll",               /* 2'b00: clk_lpll_mux */
  "clk_gpll",               /* 2'b01: clk_gpll_mux */
  "clk_litcore_pvtpll_src", /* 2'b10: clk_litcore_pvtpll_src */
};

static const char *g_litcore_sel_parents[] = {
  "clk_litcore_src_out",    /* 2'b00 */
  "clk_litcore_pvtpll_src", /* 2'b01 */
  "clk_litcore_clean",      /* 2'b10 */
};

/****************************************************************************
 * Name: rk3576_clk_register_litcore
 *
 * Description:
 *   Register the LITCORE_CRU (little-core power domain) clock tree.  This
 *   covers the little-core cluster clock sources — clk_litcore, the AXI
 *   matrix clock aclk_m_litcore, and the APB clock pclk_litcore_root (plus
 *   the debug APB clock pclk_dbg_litcore).
 *
 *   Clock topology (LITCORE_CRU, base 0x27240000):
 *
 *     clk_litcore_src_sel  : 2-bit mux (CLKSEL_CON00[13:12])
 *                            lpll / gpll / litcore_pvtpll_src
 *     clk_litcore_src_div  : 5-bit divider (CLKSEL_CON00[11:7], div+1)
 *     clk_litcore_src_en   : gate (GATE_CON00[2], SET_TO_DISABLE)
 *     clk_litcore_sel      : 2-bit mux (CLKSEL_CON01[7:6])
 *                            src_out / pvtpll_src / clean
 *     clk_litcore          : gate (GATE_CON00[5], SET_TO_DISABLE)  <- CPU clk
 *
 *     aclk_m_litcore_div   : 5-bit divider (CLKSEL_CON01[12:8], div+1)
 *     aclk_m_litcore       : gate (GATE_CON00[14], SET_TO_DISABLE)
 *
 *     pclk_litcore_root_sel : 1-bit mux (CLKSEL_CON01[5])
 *                              gpll / lpll
 *     pclk_litcore_root_div : 5-bit divider (CLKSEL_CON01[4:0], div+1)
 *     pclk_litcore_root     : gate (GATE_CON00[4], SET_TO_DISABLE)
 *
 *     pclk_dbg_litcore_div  : 5-bit divider (CLKSEL_CON02[4:0], div+1)
 *     pclk_dbg_litcore      : gate (GATE_CON00[15], SET_TO_DISABLE)
 *
 *   clk_litcore_pvtpll_src_sel : 1-bit mux (CLKSEL_CON01[13])
 *                                 deepslow / litcore_pvtpll
 ****************************************************************************/

static void rk3576_clk_register_litcore(void)
{
  const unsigned long litcore = RK3576_LITCORE_CRU_ADDR;
  struct rk3576_fracpll_s lpll_priv;
  struct clk_s *lpll;
  static const char *lpll_parents[] = { "xin_osc0" };

  /* LPLL (FRACPLL) — lives in the CCI_CRU domain at 0x27248000.
   * Rate is derived from LPLL_CON(0..2) registers at runtime using the
   * FRACPLL formula: FOUT = ((m + k/65536) * FIN) / (p * 2^s).
   * Uses configurable ops to allow clk_set_rate(clk_lpll, ...) for CPU freq.
   * Parent is xin_osc0 so the CLK framework provides 24 MHz to recalc_rate.
   */

  lpll_priv.con_base = RK3576_CCI_CRU_ADDR + RK3576_CCICRU_LPLL_CON(0);
  lpll_priv.lock_reg = RK3576_CCI_CRU_ADDR + RK3576_CCICRU_LPLL_CON(6);
  lpll_priv.lock_bit = 15; /* LPLL_CON6[15] = lpll_lock */

  lpll = clk_register("clk_lpll", lpll_parents, 1,
                      CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                      &g_rk3576_fracpll_configurable_ops, &lpll_priv,
                      sizeof(lpll_priv));
  DEBUGASSERT(lpll);
  UNUSED(lpll);

  /* clk_litcore_pvtpll : PVT (Process-Voltage-Temperature) monitoring PLL.
   * This PLL's frequency varies with process corner, supply voltage, and
   * die temperature.  It is NOT a stable clock source — it is used for
   * performance monitoring and dynamic frequency scaling feedback.
   * Register with NULL parent and 0 Hz to indicate unknown/dynamic rate.
   */

  clk_register_fixed_rate("clk_litcore_pvtpll", NULL, CLK_NAME_IS_STATIC, 0);

  /* clk_litcore_src_sel : 2-bit mux (CLKSEL_CON00[13:12]). */

  clk_register_mux("clk_litcore_src_sel", g_litcore_src_sel_parents,
                   nitems(g_litcore_src_sel_parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                   litcore + RK3576_LITCORECRU_CLKSEL_CON(0), 12, 2,
                   CLK_MUX_HIWORD_MASK);

  /* clk_litcore_src_div : 5-bit divider (CLKSEL_CON00[11:7], div+1).
   * NOTE: No CLK_SET_RATE_PARENT — this divider is fixed per the CPU
   * frequency table.  Changing it should NOT propagate to LPLL.
   */

  clk_register_divider("clk_litcore_src_div", "clk_litcore_src_sel",
                       CLK_NAME_IS_STATIC,
                       litcore + RK3576_LITCORECRU_CLKSEL_CON(0), 7, 5,
                       CLK_DIVIDER_HIWORD_MASK);

  /* clk_litcore_src_out : source gate (GATE_CON00[2]). */

  clk_register_gate("clk_litcore_src_out", "clk_litcore_src_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    litcore + RK3576_LITCORECRU_GATE_CON(0), 2,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* clk_litcore_sel : 2-bit mux (CLKSEL_CON01[7:6]). */

  clk_register_mux(
      "clk_litcore_sel", g_litcore_sel_parents, nitems(g_litcore_sel_parents),
      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
      litcore + RK3576_LITCORECRU_CLKSEL_CON(1), 6, 2, CLK_MUX_HIWORD_MASK);

  /* clk_litcore : CPU clock gate (GATE_CON00[5]). */

  clk_register_gate("clk_litcore", "clk_litcore_sel",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    litcore + RK3576_LITCORECRU_GATE_CON(0), 5,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* aclk_m_litcore_div : 5-bit divider (CLKSEL_CON01[12:8], div+1).
   * NOTE: No CLK_SET_RATE_PARENT — changing aclk rate should NOT propagate
   * to LPLL.  The divider is fixed per the CPU frequency table.
   */

  clk_register_divider("aclk_m_litcore_div", "clk_litcore_src_sel",
                       CLK_NAME_IS_STATIC,
                       litcore + RK3576_LITCORECRU_CLKSEL_CON(1), 8, 5,
                       CLK_DIVIDER_HIWORD_MASK);

  /* aclk_m_litcore : AXI matrix gate (GATE_CON00[14]). */

  clk_register_gate("aclk_m_litcore", "aclk_m_litcore_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    litcore + RK3576_LITCORECRU_GATE_CON(0), 14,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* pclk_litcore_root_sel : 1-bit mux (CLKSEL_CON01[5]). */

  {
    static const char *litcore_root_sel_parents[] = {
      "clk_gpll", /* 1'b0 */
      "clk_lpll", /* 1'b1 */
    };

    clk_register_mux("pclk_litcore_root_sel", litcore_root_sel_parents,
                     nitems(litcore_root_sel_parents),
                     CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                     litcore + RK3576_LITCORECRU_CLKSEL_CON(1), 5, 1,
                     CLK_MUX_HIWORD_MASK);
  }

  /* pclk_litcore_root_div : 5-bit divider (CLKSEL_CON01[4:0], div+1).
   * NOTE: No CLK_SET_RATE_PARENT — changing APB rate should NOT propagate
   * to LPLL.  The divider is fixed per the CPU frequency table.
   */

  clk_register_divider("pclk_litcore_root_div", "pclk_litcore_root_sel",
                       CLK_NAME_IS_STATIC,
                       litcore + RK3576_LITCORECRU_CLKSEL_CON(1), 0, 5,
                       CLK_DIVIDER_HIWORD_MASK);

  /* pclk_litcore_root : APB root gate (GATE_CON00[4]). */

  clk_register_gate("pclk_litcore_root", "pclk_litcore_root_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    litcore + RK3576_LITCORECRU_GATE_CON(0), 4,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* pclk_dbg_litcore_div : 5-bit divider (CLKSEL_CON02[4:0], div+1).
   * NOTE: No CLK_SET_RATE_PARENT — changing debug APB rate should NOT
   * propagate to LPLL.  The divider is fixed per the CPU frequency table.
   */

  clk_register_divider("pclk_dbg_litcore_div", "pclk_litcore_root",
                       CLK_NAME_IS_STATIC,
                       litcore + RK3576_LITCORECRU_CLKSEL_CON(2), 0, 5,
                       CLK_DIVIDER_HIWORD_MASK);

  /* pclk_dbg_litcore : debug APB gate (GATE_CON00[15]). */

  clk_register_gate("pclk_dbg_litcore", "pclk_dbg_litcore_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    litcore + RK3576_LITCORECRU_GATE_CON(0), 15,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}

/**
 * Macro: RK3576_CLK_REGISTER_I2C_ONE
 *
 * Register one I2C bus clock tree (mux + pclk gate + sclk gate).
 * Uses #bus stringification so all clock names are compile-time constants
 * — no snprintf required.
 *
 * Parameters:
 *   bus       - bus index (0..9), used as both integer and name suffix
 *   sel_reg   - CLKSEL register address
 *   sel_shift - MUX select field bit offset
 *   pclk_reg  - pclk GATE register address
 *   pclk_bit  - pclk GATE bit
 *   clk_reg   - sclk GATE register address
 *   clk_bit   - sclk GATE bit
 */

#define RK3576_CLK_REGISTER_I2C_ONE(bus, sel_reg, sel_shift, pclk_reg,       \
                                    pclk_bit, clk_reg, clk_bit, pclk_parent) \
  do                                                                         \
    {                                                                        \
      struct clk_s *_mux;                                                    \
                                                                             \
      _mux = clk_register_mux("clk_i2c" #bus "_sel", g_i2c_sel_parents,      \
                              nitems(g_i2c_sel_parents),                     \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,      \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK);   \
      if (!_mux)                                                             \
        {                                                                    \
          _err("CLK: failed to register clk_i2c" #bus "_sel\n");             \
          break;                                                             \
        }                                                                    \
                                                                             \
      clk_register_gate("pclk_i2c" #bus, pclk_parent, CLK_NAME_IS_STATIC,    \
                        pclk_reg, pclk_bit,                                  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
                                                                             \
      clk_register_gate("clk_i2c" #bus, "clk_i2c" #bus "_sel",               \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, clk_reg,   \
                        clk_bit,                                             \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
    }                                                                        \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_i2c
 *
 * Description:
 *   Register all I2C0–I2C9 clock muxes and gates.  The register mapping
 *   matches _get_i2c_clock_sel_register() and _get_i2c_clock_gate_register()
 *   from rk3576_cru.c.
 *
 *   I2C0 lives in PMU1_CRU domain; I2C1-8 share CLKSEL_CON(57);
 *   I2C9 uses CLKSEL_CON(58).
 *
 *   Each I2C has:
 *   - clk_i2cX_sel   : 2-bit mux (GPLL/6, CPLL/10, CPLL/20, XIN_OSC0)
 *   - pclk_i2cX      : APB bus interface gate
 *   - clk_i2cX       : SCL functional clock gate
 ****************************************************************************/

#ifdef CONFIG_RK3576_I2C
static void rk3576_clk_register_i2c(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;

  /* I2C0 — PMU1 domain */

  RK3576_CLK_REGISTER_I2C_ONE(0, pmu1 + RK3576_PMU1CRU_CLKSEL_CON(6),
                              7,                                    /* mux */
                              pmu1 + RK3576_PMU1CRU_GATE_CON(5), 1, /* pclk */
                              pmu1 + RK3576_PMU1CRU_GATE_CON(5), 2, /* clk  */
                              "pclk_pmu0_root_src");

  /* I2C1–8 — main CRU domain, CLKSEL_CON(57) consecutive 2-bit slots */

  RK3576_CLK_REGISTER_I2C_ONE(
      1, cru + RK3576_CRU_CLKSEL_CON(57), 0, cru + RK3576_CRU_GATE_CON(12), 0,
      cru + RK3576_CRU_GATE_CON(12), 12, "pclk_bus_root");

  RK3576_CLK_REGISTER_I2C_ONE(
      2, cru + RK3576_CRU_CLKSEL_CON(57), 2, cru + RK3576_CRU_GATE_CON(12), 1,
      cru + RK3576_CRU_GATE_CON(12), 13, "pclk_bus_root");

  RK3576_CLK_REGISTER_I2C_ONE(
      3, cru + RK3576_CRU_CLKSEL_CON(57), 4, cru + RK3576_CRU_GATE_CON(12), 2,
      cru + RK3576_CRU_GATE_CON(12), 14, "pclk_bus_root");

  RK3576_CLK_REGISTER_I2C_ONE(
      4, cru + RK3576_CRU_CLKSEL_CON(57), 6, cru + RK3576_CRU_GATE_CON(12), 3,
      cru + RK3576_CRU_GATE_CON(12), 15, "pclk_bus_root");

  RK3576_CLK_REGISTER_I2C_ONE(
      5, cru + RK3576_CRU_CLKSEL_CON(57), 8, cru + RK3576_CRU_GATE_CON(12), 4,
      cru + RK3576_CRU_GATE_CON(13), 0, "pclk_bus_root");

  RK3576_CLK_REGISTER_I2C_ONE(
      6, cru + RK3576_CRU_CLKSEL_CON(57), 10, cru + RK3576_CRU_GATE_CON(12), 5,
      cru + RK3576_CRU_GATE_CON(13), 1, "pclk_bus_root");

  RK3576_CLK_REGISTER_I2C_ONE(
      7, cru + RK3576_CRU_CLKSEL_CON(57), 12, cru + RK3576_CRU_GATE_CON(12), 6,
      cru + RK3576_CRU_GATE_CON(13), 2, "pclk_bus_root");

  RK3576_CLK_REGISTER_I2C_ONE(
      8, cru + RK3576_CRU_CLKSEL_CON(57), 14, cru + RK3576_CRU_GATE_CON(12), 7,
      cru + RK3576_CRU_GATE_CON(13), 3, "pclk_bus_root");

  /* I2C9 — CLKSEL_CON(58) */

  RK3576_CLK_REGISTER_I2C_ONE(
      9, cru + RK3576_CRU_CLKSEL_CON(58), 0, cru + RK3576_CRU_GATE_CON(12), 8,
      cru + RK3576_CRU_GATE_CON(13), 4, "pclk_bus_root");
}
#endif /* CONFIG_RK3576_I2C */

#undef RK3576_CLK_REGISTER_I2C_ONE

/**
 * Macro: RK3576_CLK_REGISTER_FSPI_ONE
 *
 * Register one FSPI controller clock tree.
 * Uses #id stringification for compile-time constant clock names.
 *
 * FSPI has a unique register layout:  MUX, DIV, and GATE exist in
 * separate registers but MUX and DIV share CLKSEL_CON.  To avoid
 * concurrent register access, we register the divider as a
 * clk_divider that shares the same register as the clk_mux but
 * occupies a non-overlapping bitfield.
 *
 * Parameters:
 *   id          - FSPI controller index (0 or 1), used in name suffix
 *   sel_reg     - CLKSEL register address (shared by MUX + DIV)
 *   sel_shift   - MUX select field bit offset
 *   gate_reg    - GATE register address (HCLK and SCLK share this)
 *   hclk_bit    - HCLK GATE bit
 *   sclk_bit    - SCLK GATE bit
 */

#define RK3576_CLK_REGISTER_FSPI_ONE(id, sel_reg, sel_shift, gate_reg,       \
                                     hclk_bit, sclk_bit)                     \
  do                                                                         \
    {                                                                        \
      struct clk_s *_mux;                                                    \
                                                                             \
      /* SCLK MUX: 2-bit selector, same register as divider */               \
      _mux = clk_register_mux("sclk_fspi" #id "_x2_sel", g_fspi_sel_parents, \
                              nitems(g_fspi_sel_parents),                    \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |     \
                                  CLK_PARENT_NAME_IS_STATIC,                 \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK);   \
      if (!_mux)                                                             \
        {                                                                    \
          _err("CLK: failed to register sclk_fspi" #id "_x2_sel\n");         \
          break;                                                             \
        }                                                                    \
                                                                             \
      /* SCLK_x2 Divider: bits[5:0], same register as MUX.                   \
       * Registered as a clk_divider so clk_set_rate() works.                \
       * The divider is (value + 1), 6 bits wide.                            \
       * TRM name: sclk_fspiX_x2_div, output is f_sclk_fspi_x2 = PLL/(n+1)   \
       * which is 2x the actual SCLK rate.                                   \
       */                                                                    \
      clk_register_divider("sclk_fspi" #id "_x2_div",                        \
                           "sclk_fspi" #id "_x2_sel",                        \
                           CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |        \
                               CLK_PARENT_NAME_IS_STATIC,                    \
                           sel_reg, 0, 6, CLK_DIVIDER_HIWORD_MASK);          \
                                                                             \
      /* SCLK_x2 functional gate: parent is divider output */                \
      clk_register_gate("sclk_fspi" #id "_x2", "sclk_fspi" #id "_x2_div",    \
                        CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC |     \
                            CLK_SET_RATE_PARENT,                             \
                        gate_reg, sclk_bit,                                  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
                                                                             \
      /* HCLK gate: AHB bus clock, parent is hclk_bus_root */                \
      clk_register_gate("hclk_fspi" #id, "hclk_bus_root",                    \
                        CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,      \
                        gate_reg, hclk_bit,                                  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
    }                                                                        \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_fspi
 *
 * Description:
 *   Register FSPI0–FSPI1 clock muxes, dividers, and gates.  The register
 *   mapping matches _get_fspi_clock_sel_register(),
 *   _get_fspi_clock_gate_register(), and _get_fspi_sclk_div_register()
 *   from rk3576_cru.c.
 *
 *   Each FSPI has:
 *   - sclk_fspiX_x2_sel  : 2-bit mux (GPLL, CPLL, XIN_OSC0)
 *   - sclk_fspiX_x2_div  : 6-bit SCLK_x2 divider (div + 1)
 *   - sclk_fspiX_x2      : SCLK_x2 functional clock gate
 *   - hclk_fspiX         : AHB bus clock gate
 *
 *   FSPI0: CLKSEL_CON(89) mux@[7:6] div@[5:0], GATE_CON(33) hclk@7 sclk@6
 *   FSPI1: CLKSEL_CON(106) mux@[7:6] div@[5:0], GATE_CON(43) hclk@4 sclk@3
 ****************************************************************************/

#ifdef CONFIG_RK3576_FSPI
static void rk3576_clk_register_fspi(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* FSPI0 */

  RK3576_CLK_REGISTER_FSPI_ONE(0, cru + RK3576_CRU_CLKSEL_CON(89), 6,
                               cru + RK3576_CRU_GATE_CON(33), 7, 6);

  /* FSPI1 */

  RK3576_CLK_REGISTER_FSPI_ONE(1, cru + RK3576_CRU_CLKSEL_CON(106), 6,
                               cru + RK3576_CRU_GATE_CON(43), 4, 3);
}
#endif /* CONFIG_RK3576_FSPI */

#undef RK3576_CLK_REGISTER_FSPI_ONE

/**
 * Macro: RK3576_CLK_REGISTER_PWM_ONE
 *
 * Register one PWM controller clock tree (mux + pclk + clk + osc + rc gates).
 * Uses #ctrl stringification for compile-time constant clock names.
 *
 * Parameters:
 *   ctrl       - PWM controller index (0..2), used in name suffix
 *   sel_reg    - CLKSEL register address
 *   sel_shift  - MUX select field bit offset
 *   gate_reg   - primary GATE register address (pclk/clk/osc)
 *   pclk_bit   - pclk GATE bit
 *   clk_bit    - primary clk GATE bit
 *   osc_bit    - osc clk GATE bit
 *   rc_reg     - RC clock GATE register address
 *   rc_bit     - rc clk GATE bit
 */

#define RK3576_CLK_REGISTER_PWM_ONE(ctrl, sel_reg, sel_shift, gate_reg,     \
                                    pclk_bit, clk_bit, osc_bit, rc_reg,     \
                                    rc_bit, pclk_parent)                    \
  do                                                                        \
    {                                                                       \
      struct clk_s *_mux;                                                   \
                                                                            \
      _mux = clk_register_mux("clk_pwm" #ctrl "_sel", g_pwm_sel_parents,    \
                              nitems(g_pwm_sel_parents),                    \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,     \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK);  \
      if (!_mux)                                                            \
        {                                                                   \
          _err("CLK: failed to register clk_pwm" #ctrl "_sel\n");           \
          break;                                                            \
        }                                                                   \
                                                                            \
      clk_register_gate("pclk_pwm" #ctrl, pclk_parent, CLK_NAME_IS_STATIC,  \
                        gate_reg, pclk_bit,                                 \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      clk_register_gate("clk_pwm" #ctrl, "clk_pwm" #ctrl "_sel",            \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, gate_reg, \
                        clk_bit,                                            \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      clk_register_gate("clk_pwm" #ctrl "_osc", "xin_osc0",                 \
                        CLK_NAME_IS_STATIC, gate_reg, osc_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      /* NOTE: clk_pwmX_rc is registered but currently unusable.            \
       * The upstream clock source has not been proven to produce           \
       * a valid clock on the PWM output.  Scope measurements showed no     \
       * waveform even with the gate enabled and PWM_CLK_CTRL set to        \
       * RC source.  Until the full clock chain is verified, this gate      \
       * is effectively dead code in the tree.                              \
       * Do NOT rely on clk_pwmX_rc for production use.                     \
       */                                                                   \
      clk_register_gate("clk_pwm" #ctrl "_rc", NULL, CLK_NAME_IS_STATIC,    \
                        rc_reg, rc_bit,                                     \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
    }                                                                       \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_pwm
 *
 * Description:
 *   Register all PWM0–PWM2 clock muxes and gates.  The register mapping
 *   matches _get_pwm_clock_sel_reg() and _get_pwm_clock_gate_reg() from
 *   rk3576_cru.c.
 *
 *   Each PWM has:
 *   - clk_pwmX_sel   : 2-bit mux (CPLL/10, CPLL/20, XIN_OSC0)
 *   - pclk_pwmX      : APB bus interface gate
 *   - clk_pwmX       : Primary PWM functional gate
 *   - clk_pwmX_osc   : External oscillator alternative gate
 *   - clk_pwmX_rc    : Internal RC oscillator alternative gate
 ****************************************************************************/

#ifdef CONFIG_RK3576_PWM
static void rk3576_clk_register_pwm(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;

  /* PWM0 — PMU1 domain */

  RK3576_CLK_REGISTER_PWM_ONE(0, pmu1 + RK3576_PMU1CRU_CLKSEL_CON(5), 2,
                              pmu1 + RK3576_PMU1CRU_GATE_CON(4), 11, 12, 13,
                              pmu1 + RK3576_PMU1CRU_GATE_CON(5), 7,
                              "pclk_pmu0_root_src");

  /* PWM1 — main CRU domain */

  RK3576_CLK_REGISTER_PWM_ONE(
      1, cru + RK3576_CRU_CLKSEL_CON(71), 8, cru + RK3576_CRU_GATE_CON(16), 10,
      11, 13, cru + RK3576_CRU_GATE_CON(16), 15, "pclk_bus_root");

  /* PWM2 — main CRU domain */

  RK3576_CLK_REGISTER_PWM_ONE(
      2, cru + RK3576_CRU_CLKSEL_CON(74), 6, cru + RK3576_CRU_GATE_CON(20), 4,
      5, 7, cru + RK3576_CRU_GATE_CON(20), 6, "pclk_bus_root");
}
#endif /* CONFIG_RK3576_PWM */

#undef RK3576_CLK_REGISTER_PWM_ONE

/**
 * Macro: RK3576_CLK_REGISTER_MATRIX_UART_FRAC_ONE
 *
 * Register one clk_matrix_uart_frac_N clock tree
 * (mux + fractional divider + gate).
 *
 * Register layout from TRM:
 *   CLKSEL_CON(21 + 2*N)     : fractional divider register
 *                               [31:16] = numerator (16-bit)
 *                               [15:0]  = denominator (16-bit)
 *   CLKSEL_CON(22 + 2*N)     : mux select register
 *                               [1:0]   = parent select (2-bit)
 *                               [31:16] = hiword write mask
 *
 * Parent selection (2-bit):
 *   0b00: gpll / 0b01: cpll / 0b10: aupll / 0b11: xin_osc0
 *
 * Gate bits (CRU_GATE_CON02, 0x0808, SET_TO_DISABLE):
 *   _0: bit 4  /  _1: bit 5  /  _2: bit 6
 */

#define RK3576_CLK_REGISTER_MATRIX_UART_FRAC_ONE(index, div_reg, sel_reg,    \
                                                 gate_bit)                   \
  do                                                                         \
    {                                                                        \
      struct clk_s *_mux;                                                    \
                                                                             \
      _mux = clk_register_mux("clk_matrix_uart_frac_" #index "_sel",         \
                              g_matrix_uart_frac_sel_parents,                \
                              nitems(g_matrix_uart_frac_sel_parents),        \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,      \
                              sel_reg, 0, 2, CLK_MUX_HIWORD_MASK);           \
      if (!_mux)                                                             \
        {                                                                    \
          _err("CLK: failed to register "                                    \
               "clk_matrix_uart_frac_" #index "_sel\n");                     \
          break;                                                             \
        }                                                                    \
                                                                             \
      clk_register_fractional_divider("clk_matrix_uart_frac_" #index "_div", \
                                      "clk_matrix_uart_frac_" #index "_sel", \
                                      CLK_SET_RATE_PARENT |                  \
                                          CLK_NAME_IS_STATIC,                \
                                      div_reg, 16, 16, 0, 16, 0);            \
                                                                             \
      clk_register_gate("clk_matrix_uart_frac_" #index,                      \
                        "clk_matrix_uart_frac_" #index "_div",               \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,            \
                        RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(2), gate_bit,  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
    }                                                                        \
  while (0)

/**
 * Name: rk3576_clk_register_matrix_uart
 *
 * Description:
 *   Register all clk_matrix_uart_frac_0..2 (mux + fractional divider +
 *   gate).
 *
 *   Each UART frac clock has:
 *   - A 2-bit mux selecting between GPLL/CPLL/AUPLL/XIN_OSC0
 *   - A fractional divider (16+16 bit)
 *   - A gate
 *
 *   Register mapping (TRM):
 *     clk_matrix_uart_frac_0: div=CON21(0x0354), sel=CON22(0x0358)
 *     clk_matrix_uart_frac_1: div=CON23(0x035C), sel=CON24(0x0360)
 *     clk_matrix_uart_frac_2: div=CON25(0x0364), sel=CON26(0x0368)
 */

#ifdef CONFIG_RK3576_UART
static void rk3576_clk_register_matrix_uart(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* UART frac clocks (mux + frac divider + gate) */

  RK3576_CLK_REGISTER_MATRIX_UART_FRAC_ONE(0, cru + RK3576_CRU_CLKSEL_CON(21),
                                           cru + RK3576_CRU_CLKSEL_CON(22), 4);

  RK3576_CLK_REGISTER_MATRIX_UART_FRAC_ONE(1, cru + RK3576_CRU_CLKSEL_CON(23),
                                           cru + RK3576_CRU_CLKSEL_CON(24), 5);

  RK3576_CLK_REGISTER_MATRIX_UART_FRAC_ONE(2, cru + RK3576_CRU_CLKSEL_CON(25),
                                           cru + RK3576_CRU_CLKSEL_CON(26), 6);
}
#endif

#undef RK3576_CLK_REGISTER_MATRIX_UART_FRAC_ONE

/**
 * Macro: RK3576_CLK_REGISTER_UART_ONE
 *
 * Register one UART controller clock tree
 * (sclk mux + sclk divider + sclk gate + pclk gate).
 *
 * Parameters:
 *   index     - UART index (0..11), used in clock name suffix
 *   sel_reg   - CLKSEL register address (holds both src_sel and div)
 *   src_shift - sclk_uartN_sel bit offset in sel_reg (3-bit field)
 *   div_shift - sclk_uartN_div bit offset in sel_reg (8-bit field)
 *   pclk_reg  - pclk GATE register address
 *   pclk_bit  - pclk GATE bit
 *   sclk_reg  - sclk GATE register address
 *   sclk_bit  - sclk GATE bit
 */

#define RK3576_CLK_REGISTER_UART_ONE(index, sel_reg, src_shift, div_shift,   \
                                     pclk_reg, pclk_bit, sclk_reg, sclk_bit) \
  do                                                                         \
    {                                                                        \
      struct clk_s *_src_sel;                                                \
      struct clk_s *_div;                                                    \
                                                                             \
      _src_sel = clk_register_mux(                                           \
          "sclk_uart" #index "_sel", g_uart_sclk_sel_parents,                \
          nitems(g_uart_sclk_sel_parents),                                   \
          CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, sel_reg, src_shift, 3,   \
          CLK_MUX_HIWORD_MASK);                                              \
      if (!_src_sel)                                                         \
        {                                                                    \
          _err("CLK: failed to register sclk_uart" #index "_sel\n");         \
          break;                                                             \
        }                                                                    \
                                                                             \
      _div = clk_register_divider(                                           \
          "sclk_uart" #index "_div", "sclk_uart" #index "_sel",              \
          CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, sel_reg, div_shift, 8,   \
          CLK_DIVIDER_HIWORD_MASK);                                          \
      if (!_div)                                                             \
        {                                                                    \
          _err("CLK: failed to register sclk_uart" #index "_div\n");         \
          break;                                                             \
        }                                                                    \
                                                                             \
      clk_register_gate("pclk_uart" #index, "pclk_bus_root",                 \
                        CLK_NAME_IS_STATIC, pclk_reg, pclk_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
                                                                             \
      clk_register_gate("sclk_uart" #index, "sclk_uart" #index "_div",       \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, sclk_reg,  \
                        sclk_bit,                                            \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
    }                                                                        \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_uart
 *
 * Description:
 *   Register all UART0–UART11 clock trees.
 *
 *   UART clock registers are spread across:
 *     UART0:  CLKSEL_CON60  / GATE_CON13 (pclk) + GATE_CON14 (sclk)
 *     UART1:  special — see below
 *     UART2:  CLKSEL_CON61  / GATE_CON13 (pclk) + GATE_CON14 (sclk)
 *     UART3:  CLKSEL_CON62  / GATE_CON13 (pclk) + GATE_CON14 (sclk)
 *     UART4:  CLKSEL_CON63  / GATE_CON13 (pclk) + GATE_CON14 (sclk)
 *     UART5:  CLKSEL_CON64  / GATE_CON13 (pclk) + GATE_CON14 (sclk)
 *     UART6:  CLKSEL_CON65  / GATE_CON13 (pclk) + GATE_CON15 (sclk)
 *     UART7:  CLKSEL_CON66  / GATE_CON14 (pclk) + GATE_CON15 (sclk)
 *     UART8:  CLKSEL_CON67  / GATE_CON14 (pclk) + GATE_CON15 (sclk)
 *     UART9:  CLKSEL_CON68  / GATE_CON14 (pclk) + GATE_CON15 (sclk)
 *     UART10: CLKSEL_CON69  / GATE_CON14 (pclk) + GATE_CON15 (sclk)
 *     UART11: CLKSEL_CON70  / GATE_CON14 (pclk) + GATE_CON15 (sclk)
 *
 *   UART0, 2–11: each has sclk_src_sel (3-bit mux) + sclk_src_div
 *   (8-bit divider) + sclk gate + pclk gate.
 *   All sclk_src_sel fields at [10:8], sclk_src_div at [7:0].
 *
 *   UART1 is special — it has a two-level mux structure with NO local
 *   divider.  The hardware chain is:
 *
 *     Level 1 (CRU domain, CLKSEL_CON27):
 *       clk_uart1_src_top_sel (3-bit mux, [15:13])
 *         Parents: gpll / cpll / aupll / xin_osc0 /
 *                  matrix_uart_frac_0 / _1 / _2
 *         -> clk_uart1_src_top_div (8-bit divider, [12:5], div_con+1)
 *           -> clk_uart1_src_top (gate, CRU_GATE_CON02[13])
 *
 *     Level 2 (PMU1CRU domain, PMU1CRU_CLKSEL_CON08):
 *       sclk_uart1_sel (1-bit mux, [0])
 *         0: clk_uart1_src_top (from level 1)
 *         1: xin_osc0_func    (bypass, 24 MHz direct)
 *         -> sclk_uart1 (gate, PMU1CRU_GATE_CON05[5])
 *
 *   UART1 has no divider of its own — the division is performed by
 *   clk_uart1_src_top_div upstream.
 *
 *   pclk gate: PMU1CRU_GATE_CON05[6]
 *
 *   All gates use SET_TO_DISABLE (high = clock off).
 ****************************************************************************/

#ifdef CONFIG_RK3576_UART
static void rk3576_clk_register_uart(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* UART0 — CLKSEL_CON60 (0x03F0), GATE_CON13/14 */

  RK3576_CLK_REGISTER_UART_ONE(0, cru + RK3576_CRU_CLKSEL_CON(60),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(13), 10, /* pclk */
                               cru + RK3576_CRU_GATE_CON(14), 5); /* sclk */

  /* UART2 — CLKSEL_CON61 (0x03F4), GATE_CON13/14 */

  RK3576_CLK_REGISTER_UART_ONE(2, cru + RK3576_CRU_CLKSEL_CON(61),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(13), 11, /* pclk */
                               cru + RK3576_CRU_GATE_CON(14), 6); /* sclk */

  /* UART3 — CLKSEL_CON62 (0x03F8), GATE_CON13/14 */

  RK3576_CLK_REGISTER_UART_ONE(3, cru + RK3576_CRU_CLKSEL_CON(62),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(13), 12, /* pclk */
                               cru + RK3576_CRU_GATE_CON(14), 9); /* sclk */

  /* UART4 — CLKSEL_CON63 (0x03FC), GATE_CON13/14 */

  RK3576_CLK_REGISTER_UART_ONE(4, cru + RK3576_CRU_CLKSEL_CON(63),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(13), 13,  /* pclk */
                               cru + RK3576_CRU_GATE_CON(14), 12); /* sclk */

  /* UART5 — CLKSEL_CON64 (0x0400), GATE_CON13/14 */

  RK3576_CLK_REGISTER_UART_ONE(5, cru + RK3576_CRU_CLKSEL_CON(64),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(13), 14,  /* pclk */
                               cru + RK3576_CRU_GATE_CON(14), 15); /* sclk */

  /* UART6 — CLKSEL_CON65 (0x0404), GATE_CON13/15 */

  RK3576_CLK_REGISTER_UART_ONE(6, cru + RK3576_CRU_CLKSEL_CON(65),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(13), 15, /* pclk */
                               cru + RK3576_CRU_GATE_CON(15), 2); /* sclk */

  /* UART7 — CLKSEL_CON66 (0x0408), GATE_CON14/15 */

  RK3576_CLK_REGISTER_UART_ONE(7, cru + RK3576_CRU_CLKSEL_CON(66),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(14), 0,  /* pclk */
                               cru + RK3576_CRU_GATE_CON(15), 5); /* sclk */

  /* UART8 — CLKSEL_CON67 (0x040C), GATE_CON14/15 */

  RK3576_CLK_REGISTER_UART_ONE(8, cru + RK3576_CRU_CLKSEL_CON(67),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(14), 1,  /* pclk */
                               cru + RK3576_CRU_GATE_CON(15), 8); /* sclk */

  /* UART9 — CLKSEL_CON68 (0x0410), GATE_CON14/15 */

  RK3576_CLK_REGISTER_UART_ONE(9, cru + RK3576_CRU_CLKSEL_CON(68),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(14), 2,  /* pclk */
                               cru + RK3576_CRU_GATE_CON(15), 9); /* sclk */

  /* UART10 — CLKSEL_CON69 (0x0414), GATE_CON14/15 */

  RK3576_CLK_REGISTER_UART_ONE(10, cru + RK3576_CRU_CLKSEL_CON(69),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(14), 3,   /* pclk */
                               cru + RK3576_CRU_GATE_CON(15), 10); /* sclk */

  /* UART11 — CLKSEL_CON70 (0x0418), GATE_CON14/15 */

  RK3576_CLK_REGISTER_UART_ONE(11, cru + RK3576_CRU_CLKSEL_CON(70),
                               8, /* src_sel [10:8] */
                               0, /* div [7:0] */
                               cru + RK3576_CRU_GATE_CON(14), 4,   /* pclk */
                               cru + RK3576_CRU_GATE_CON(15), 11); /* sclk */

  /* ---- UART1 — special two-level mux, no local divider ----
   *
   * Level 1: clk_uart1_src_top (CRU domain)
   *   CLKSEL_CON27 (0x036C):
   *     [15:13] = clk_uart1_src_top_sel (3-bit mux, 7 parents)
   *     [12:5]  = clk_uart1_src_top_div (8-bit, div_con+1)
   *   GATE_CON02 (0x0808):
   *     [13]    = clk_uart1_src_top  (SET_TO_DISABLE)
   *
   * Level 2: sclk_uart1 (PMU1CRU domain)
   *   PMU1CRU_CLKSEL_CON08 (0x27220320):
   *     [0]     = sclk_uart1_sel (1-bit mux)
   *               0 = clk_uart1_src_top, 1 = xin_osc0_func
   *   PMU1CRU_GATE_CON05:
   *     [5]     = sclk_uart1  (SET_TO_DISABLE)
   *     [6]     = pclk_uart1  (SET_TO_DISABLE)
   */

  /* Level 1: clk_uart1_src_top_sel mux (3-bit, 7 parents) */
  {
    const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;
    struct clk_s *mux;

    mux = clk_register_mux("clk_uart1_src_top_sel", g_uart_sclk_sel_parents,
                           nitems(g_uart_sclk_sel_parents),
                           CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                           cru + RK3576_CRU_CLKSEL_CON(27), 13, 3,
                           CLK_MUX_HIWORD_MASK);
    if (!mux)
      {
        _err("CLK: failed to register clk_uart1_src_top_sel\n");
      }

    /* Level 1: clk_uart1_src_top_div (8-bit integer divider, [12:5]) */

    clk_register_divider("clk_uart1_src_top_div", "clk_uart1_src_top_sel",
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(27), 5, 8,
                         CLK_DIVIDER_HIWORD_MASK);

    /* Level 1: clk_uart1_src_top gate (CRU_GATE_CON02[13]) */

    clk_register_gate("clk_uart1_src_top", "clk_uart1_src_top_div",
                      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                      cru + RK3576_CRU_GATE_CON(2), 13,
                      CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

    /* Level 2: sclk_uart1_sel mux (1-bit, PMU1CRU_CLKSEL_CON08[0])
     *   0 = clk_uart1_src_top, 1 = xin_osc0_func
     */

    mux = clk_register_mux(
        "sclk_uart1_sel", g_uart1_sclk_parents, nitems(g_uart1_sclk_parents),
        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
        pmu1 + RK3576_PMU1CRU_CLKSEL_CON(8), 0, 1, CLK_MUX_HIWORD_MASK);
    if (!mux)
      {
        _err("CLK: failed to register sclk_uart1_sel\n");
      }

    /* sclk_uart1 gate */

    clk_register_gate("sclk_uart1", "sclk_uart1_sel",
                      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                      pmu1 + RK3576_PMU1CRU_GATE_CON(5), 5,
                      CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

    /* pclk_uart1 gate */

    clk_register_gate("pclk_uart1", "pclk_pmu0_root_src", CLK_NAME_IS_STATIC,
                      pmu1 + RK3576_PMU1CRU_GATE_CON(5), 6,
                      CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
  }
}
#endif /* CONFIG_RK3576_UART */

#undef RK3576_CLK_REGISTER_UART_ONE

/**
 * Macro: RK3576_CLK_REGISTER_MATRIX_AUDIO_FRAC_ONE
 *
 * Register one clk_matrix_audio_frac_N clock tree
 * (mux + fractional divider + gate).
 *
 * Register layout from TRM:
 *   CLKSEL_CON(12 + 2*N)     : fractional divider register
 *                               [31:16] = numerator (16-bit)
 *                               [15:0]  = denominator (16-bit)
 *   CLKSEL_CON(13 + 2*N)     : mux select register
 *                               [1:0]   = parent select (2-bit)
 *                               [31:16] = hiword write mask
 *
 * Parent selection (2-bit):
 *   0b00: gpll / 0b01: cpll / 0b10: aupll / 0b11: xin_osc0
 *
 * Gate bits (CRU_GATE_CON01, 0x0804, SET_TO_DISABLE):
 *   _0: bit 10  /  _1: bit 11  /  _2: bit 12  /  _3: bit 13
 */

#define RK3576_CLK_REGISTER_MATRIX_AUDIO_FRAC_ONE(index, div_reg, sel_reg,    \
                                                  gate_bit)                   \
  do                                                                          \
    {                                                                         \
      struct clk_s *_mux;                                                     \
                                                                              \
      _mux = clk_register_mux("clk_matrix_audio_frac_" #index "_sel",         \
                              g_matrix_audio_frac_sel_parents,                \
                              nitems(g_matrix_audio_frac_sel_parents),        \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,       \
                              sel_reg, 0, 2, CLK_MUX_HIWORD_MASK);            \
      if (!_mux)                                                              \
        {                                                                     \
          _err("CLK: failed to register "                                     \
               "clk_matrix_audio_frac_" #index "_sel\n");                     \
          break;                                                              \
        }                                                                     \
                                                                              \
      clk_register_fractional_divider("clk_matrix_audio_frac_" #index "_div", \
                                      "clk_matrix_audio_frac_" #index "_sel", \
                                      CLK_SET_RATE_PARENT |                   \
                                          CLK_NAME_IS_STATIC,                 \
                                      div_reg, 16, 16, 0, 16, 0);             \
                                                                              \
      clk_register_gate("clk_matrix_audio_frac_" #index,                      \
                        "clk_matrix_audio_frac_" #index "_div",               \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,             \
                        RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(1), gate_bit,   \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);      \
    }                                                                         \
  while (0)

/**
 * Macro: RK3576_CLK_REGISTER_MATRIX_AUDIO_INT_ONE
 *
 * Register one clk_matrix_audio_int_N clock tree
 * (integer divider + gate).
 *
 * Unlike the frac clocks which each have their own 2-bit MUX selecting
 * among GPLL/CPLL/AUPLL/XIN_OSC0, each integer clock is hard-wired to a
 * single PLL parent (per the Linux reference implementation):
 *   int_0 -> gpll  (fixed integer divider from GPLL)
 *   int_1 -> cpll  (fixed integer divider from CPLL)
 *   int_2 -> aupll (fixed integer divider from AUPLL)
 *
 * Register layout from TRM:
 *   CLKSEL_CON28 (0x0370) : integer divider register
 *     _0_div: [4:0]  /  _1_div: [9:5]  /  _2_div: [14:10]
 *     div = div_con + 1
 *
 * Gate bits (TRM CRU_GATE_CON02/03, SET_TO_DISABLE):
 *   _0: GATE_CON02 bit 14  /  _1: GATE_CON02 bit 15  /  _2: GATE_CON03 bit 0
 */

#define RK3576_CLK_REGISTER_MATRIX_AUDIO_INT_ONE(                           \
    index, parent_name, div_reg, div_shift, gate_reg, gate_bit)             \
  do                                                                        \
    {                                                                       \
      clk_register_divider("clk_matrix_audio_int_" #index "_div",           \
                           parent_name, CLK_NAME_IS_STATIC, div_reg,        \
                           div_shift, 5, CLK_DIVIDER_HIWORD_MASK);          \
                                                                            \
      clk_register_gate("clk_matrix_audio_int_" #index,                     \
                        "clk_matrix_audio_int_" #index "_div",              \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, gate_reg, \
                        gate_bit,                                           \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
    }                                                                       \
  while (0)

/**
 * Name: rk3576_clk_register_matrix_audio
 *
 * Description:
 *   Register all clk_matrix_audio_frac_0..3 (mux + fractional divider +
 *   gate) and clk_matrix_audio_int_0..2 (integer divider + gate).
 *
 *   Fractional clocks each have:
 *   - A 2-bit mux selecting between GPLL/CPLL/AUPLL/XIN_OSC0
 *   - A fractional divider (16+16 bit)
 *   - A gate
 *
 *   Integer clocks are each hard-wired to a single PLL (no mux):
 *   - int_0 parent: gpll
 *   - int_1 parent: cpll
 *   - int_2 parent: aupll
 *   Each has a 5-bit integer divider and a gate.
 *
 *   Register mapping (TRM):
 *     clk_matrix_audio_frac_0: div=CON12(0x0330), sel=CON13(0x0334)
 *     clk_matrix_audio_frac_1: div=CON14(0x0338), sel=CON15(0x033C)
 *     clk_matrix_audio_frac_2: div=CON16(0x0340), sel=CON17(0x0344)
 *     clk_matrix_audio_frac_3: div=CON18(0x0348), sel=CON19(0x034C)
 *     clk_matrix_audio_int_0..2: div=CON28(0x0370) [4:0]/[9:5]/[14:10]
 */

static void rk3576_clk_register_matrix_audio(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long int_div_reg = cru + RK3576_CRU_CLKSEL_CON(28);

  /* Fractional clocks (mux + frac divider + gate) */

  RK3576_CLK_REGISTER_MATRIX_AUDIO_FRAC_ONE(
      0, cru + RK3576_CRU_CLKSEL_CON(12), cru + RK3576_CRU_CLKSEL_CON(13), 10);

  RK3576_CLK_REGISTER_MATRIX_AUDIO_FRAC_ONE(
      1, cru + RK3576_CRU_CLKSEL_CON(14), cru + RK3576_CRU_CLKSEL_CON(15), 11);

  RK3576_CLK_REGISTER_MATRIX_AUDIO_FRAC_ONE(
      2, cru + RK3576_CRU_CLKSEL_CON(16), cru + RK3576_CRU_CLKSEL_CON(17), 12);

  RK3576_CLK_REGISTER_MATRIX_AUDIO_FRAC_ONE(
      3, cru + RK3576_CRU_CLKSEL_CON(18), cru + RK3576_CRU_CLKSEL_CON(19), 13);

  /* Integer clocks (int divider + gate, each hard-wired to one PLL). */

  RK3576_CLK_REGISTER_MATRIX_AUDIO_INT_ONE(0, "clk_gpll", int_div_reg, 0,
                                           cru + RK3576_CRU_GATE_CON(2), 14);

  RK3576_CLK_REGISTER_MATRIX_AUDIO_INT_ONE(1, "clk_cpll", int_div_reg, 5,
                                           cru + RK3576_CRU_GATE_CON(2), 15);

  RK3576_CLK_REGISTER_MATRIX_AUDIO_INT_ONE(2, "clk_aupll", int_div_reg, 10,
                                           cru + RK3576_CRU_GATE_CON(3), 0);
}

#undef RK3576_CLK_REGISTER_MATRIX_AUDIO_FRAC_ONE
#undef RK3576_CLK_REGISTER_MATRIX_AUDIO_INT_ONE

/**
 * Macro: RK3576_CLK_REGISTER_SAI_ONE
 *
 * Register one SAI controller clock tree
 * (src mux + src divider + src gate + mclk mux + mclk gate + hclk gate).
 *
 * Hardware chain:
 *   mclk_saiX_src_sel (3-bit MUX, 8 parents)            -- _src_sel (mux)
 *     -> mclk_saiX_src_div (8-bit divider, div_con + 1) -- _src_div (div)
 *       -> mclk_saiX_src     (source gate)
 *         -> mclk_saiX_sel   (final mclk mux, 1 or 2 bits)
 *           -> mclk_saiX       (GATE, most downstream mclk)
 *   hclk_saiX                                          (GATE, bus clock)
 *
 * Register layout (single CLKSEL_CON reg holds mclk_sel, src_sel, src_div):
 *   [msel_shift + width - 1 : msel_shift]  = mclk_saiX_sel (mclk mux)
 *   [src_sel_shift + 2 : src_sel_shift]    = mclk_saiX_src_sel (3-bit mux)
 *   [src_div_shift + 7 : src_div_shift]    = mclk_saiX_src_div (8-bit div)
 *
 * Parameters:
 *   index          - SAI index (0..9), used in clock name suffix
 *   sel_reg        - CLKSEL register address (holds msel + src_sel + div)
 *   msel_shift     - mclk_saiX_sel mux bit offset in sel_reg
 *   msel_width     - mclk_saiX_sel mux width (1 or 2 bits)
 *   src_sel_shift  - mclk_saiX_src_sel bit offset in sel_reg (3-bit field)
 *   src_div_shift  - mclk_saiX_src_div bit offset in sel_reg (8-bit field)
 *   src_reg/src_bit- mclk_saiX_src GATE register address / bit
 *   mclk_reg/mclk_bit- mclk_saiX GATE register address / bit
 *   hclk_reg/hclk_bit- hclk_saiX GATE register address / bit
 */

#define RK3576_CLK_REGISTER_SAI_ONE(                                          \
    index, sel_reg, msel_shift, msel_width, src_sel_shift, src_div_shift,     \
    src_reg, src_bit, mclk_reg, mclk_bit, hclk_reg, hclk_bit)                 \
  do                                                                          \
    {                                                                         \
      static const char *_m_sel_parents[] = {                                 \
        "mclk_sai" #index "_src", /* bit0: mclk_saiX_src (default) */         \
        (msel_width == 2) ? "sai" #index "_mclkin" : "sai1_mclkin",           \
        "sai1_mclkin", /* bit2: only valid for 2-bit muxes */                 \
      };                                                                      \
      const int _m_sel_parents_cnt = (msel_width == 2) ? 3 : 2;               \
      struct clk_s *_src_sel;                                                 \
      struct clk_s *_src_div;                                                 \
                                                                              \
      /* mclk_saiX_src_sel : 3-bit source mux (8 parents). */                 \
      _src_sel = clk_register_mux(                                            \
          "mclk_sai" #index "_src_sel", g_sai_mclk_src_parents,               \
          nitems(g_sai_mclk_src_parents),                                     \
          CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |                          \
              CLK_PARENT_NAME_IS_STATIC,                                      \
          sel_reg, src_sel_shift, 3, CLK_MUX_HIWORD_MASK);                    \
      if (!_src_sel)                                                          \
        {                                                                     \
          _err("CLK: failed to register mclk_sai" #index "_src_sel\n");       \
          break;                                                              \
        }                                                                     \
                                                                              \
      /* mclk_saiX_src_div : 8-bit source divider (div_con + 1). */           \
      _src_div = clk_register_divider(                                        \
          "mclk_sai" #index "_src_div", "mclk_sai" #index "_src_sel",         \
          CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |                          \
              CLK_PARENT_NAME_IS_STATIC,                                      \
          sel_reg, src_div_shift, 8,                                          \
          CLK_DIVIDER_HIWORD_MASK | CLK_DIVIDER_ROUND_CLOSEST);               \
      if (!_src_div)                                                          \
        {                                                                     \
          _err("CLK: failed to register mclk_sai" #index "_src_div\n");       \
          break;                                                              \
        }                                                                     \
                                                                              \
      /* mclk_saiX_src : source gate. */                                      \
      clk_register_gate(                                                      \
          "mclk_sai" #index "_src", "mclk_sai" #index "_src_div",             \
          CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |                          \
              CLK_PARENT_NAME_IS_STATIC,                                      \
          src_reg, src_bit, CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);  \
                                                                              \
      /* mclk_saiX_sel : final mclk mux (1 or 2 bits), defaults to src.       \
       *   2-bit (SAI0, SAI2-4): 00=src, 01=saiX_mclkin, 10=sai1_mclkin       \
       *   1-bit (SAI1, SAI5-9): 0=src, 1=sai1_mclkin                         \
       */                                                                     \
      clk_register_mux("mclk_sai" #index "_sel", _m_sel_parents,              \
                       _m_sel_parents_cnt,                                    \
                       CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |             \
                           CLK_PARENT_NAME_IS_STATIC,                         \
                       sel_reg, msel_shift, msel_width, CLK_MUX_HIWORD_MASK); \
                                                                              \
      /* mclk_saiX : final mclk gate. */                                      \
      clk_register_gate("mclk_sai" #index, "mclk_sai" #index "_sel",          \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |            \
                            CLK_PARENT_NAME_IS_STATIC,                        \
                        mclk_reg, mclk_bit,                                   \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);      \
                                                                              \
      /* hclk_saiX : bus gate. */                                             \
      clk_register_gate("hclk_sai" #index, "hclk_audio_root",                 \
                        CLK_NAME_IS_STATIC, hclk_reg, hclk_bit,               \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);      \
    }                                                                         \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_sai
 *
 * Description:
 *   Register all SAI0–SAI9 clock trees (src mux + div + hclk gate +
 *   mclk gate).
 *
 *   SAI clock registers are spread across:
 *     SAI0:       CLKSEL_CON44  / GATE_CON07
 *     SAI1:       CLKSEL_CON46  / GATE_CON08
 *     SAI2:       CLKSEL_CON47  / GATE_CON08
 *     SAI3:       CLKSEL_CON48  / GATE_CON08
 *     SAI4:       CLKSEL_CON49  / GATE_CON08 (src) + GATE_CON09 (mclk/hclk)
 *     SAI5:       CLKSEL_CON154 / GATE_CON65
 *     SAI6:       CLKSEL_CON155 / GATE_CON65
 *     SAI7:       CLKSEL_CON159 / GATE_CON67
 *     SAI8:       CLKSEL_CON157 / GATE_CON66
 *     SAI9:       CLKSEL_CON162 / GATE_CON68
 *
 *   All SAI mclk_src_sel fields are at [10:8] (3-bit), except SAI5 which
 *   uses [12:10].
 *
 *   All SAI mclk_src_div fields are at [7:0] (8-bit), except SAI5 which
 *   uses [9:2].
 *
 *   All gates use SET_TO_DISABLE (high = clock off).
 ****************************************************************************/

#ifdef CONFIG_RK3576_SAI
static void rk3576_clk_register_sai(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* SAI0 — CLKSEL_CON44 (0x03B0), GATE_CON07.
   *   mclk_sel [12:11] (2-bit), src_sel [10:8], src_div [7:0]. */

  RK3576_CLK_REGISTER_SAI_ONE(0, cru + RK3576_CRU_CLKSEL_CON(44), /* sel */
                              11, 2, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(7), 11,  /* src */
                              cru + RK3576_CRU_GATE_CON(7), 12,  /* mclk */
                              cru + RK3576_CRU_GATE_CON(7), 13); /* hclk */

  /* SAI1 — CLKSEL_CON46 (0x03B8), GATE_CON08.
   *   mclk_sel [11] (1-bit), src_sel [10:8], src_div [7:0]. */

  RK3576_CLK_REGISTER_SAI_ONE(1, cru + RK3576_CRU_CLKSEL_CON(46), /* sel */
                              11, 1, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(8), 4,  /* src */
                              cru + RK3576_CRU_GATE_CON(8), 5,  /* mclk */
                              cru + RK3576_CRU_GATE_CON(8), 6); /* hclk */

  /* SAI2 — CLKSEL_CON47 (0x03BC), GATE_CON08.
   *   mclk_sel [12:11] (2-bit), src_sel [10:8], src_div [7:0]. */

  RK3576_CLK_REGISTER_SAI_ONE(2, cru + RK3576_CRU_CLKSEL_CON(47), /* sel */
                              11, 2, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(8), 7,   /* src */
                              cru + RK3576_CRU_GATE_CON(8), 8,   /* mclk */
                              cru + RK3576_CRU_GATE_CON(8), 10); /* hclk */

  /* SAI3 — CLKSEL_CON48 (0x03C0), GATE_CON08.
   *   mclk_sel [12:11] (2-bit), src_sel [10:8], src_div [7:0]. */

  RK3576_CLK_REGISTER_SAI_ONE(3, cru + RK3576_CRU_CLKSEL_CON(48), /* sel */
                              11, 2, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(8), 11,  /* src */
                              cru + RK3576_CRU_GATE_CON(8), 12,  /* mclk */
                              cru + RK3576_CRU_GATE_CON(8), 14); /* hclk */

  /* SAI4 — CLKSEL_CON49 (0x03C4), src gate GATE_CON08:15,
   *         hclk/mclk GATE_CON09:2/0.
   *   mclk_sel [12:11] (2-bit), src_sel [10:8], src_div [7:0]. */

  RK3576_CLK_REGISTER_SAI_ONE(4, cru + RK3576_CRU_CLKSEL_CON(49), /* sel */
                              11, 2, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(8), 15, /* src */
                              cru + RK3576_CRU_GATE_CON(9), 0,  /* mclk */
                              cru + RK3576_CRU_GATE_CON(9), 2); /* hclk */

  /* SAI5 — CLKSEL_CON154 (0x0568), GATE_CON65.
   *   mclk_sel [13] (1-bit), src_sel [12:10], src_div [9:2]. */

  RK3576_CLK_REGISTER_SAI_ONE(5, cru + RK3576_CRU_CLKSEL_CON(154), /* sel */
                              13, 1, /* mclk_sel shift/width */
                              10, 2, /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(65), 3,  /* src */
                              cru + RK3576_CRU_GATE_CON(65), 4,  /* mclk */
                              cru + RK3576_CRU_GATE_CON(65), 5); /* hclk */

  /* SAI6 — CLKSEL_CON155 (0x056C), GATE_CON65.
   *   mclk_sel [11] (1-bit), src_sel [10:8], src_div [7:0]. */

  RK3576_CLK_REGISTER_SAI_ONE(6, cru + RK3576_CRU_CLKSEL_CON(155), /* sel */
                              11, 1, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(65), 7,  /* src */
                              cru + RK3576_CRU_GATE_CON(65), 8,  /* mclk */
                              cru + RK3576_CRU_GATE_CON(65), 9); /* hclk */

  /* SAI7 — CLKSEL_CON159 (0x057C), GATE_CON67.
   *   mclk_sel [11] (1-bit), src_sel [10:8], src_div [7:0]. */

  RK3576_CLK_REGISTER_SAI_ONE(7, cru + RK3576_CRU_CLKSEL_CON(159), /* sel */
                              11, 1, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(67), 8,   /* src */
                              cru + RK3576_CRU_GATE_CON(67), 9,   /* mclk */
                              cru + RK3576_CRU_GATE_CON(67), 10); /* hclk */

  /* SAI8 — CLKSEL_CON157 (0x0574), GATE_CON66.
   *   mclk_sel [11] (1-bit), src_sel [10:8], src_div [7:0].
   *   NOTE: gate bits are hclk=0, src_en=1, mclk=2 (non-contiguous). */

  RK3576_CLK_REGISTER_SAI_ONE(8, cru + RK3576_CRU_CLKSEL_CON(157), /* sel */
                              11, 1, /* mclk_sel shift/width */
                              8, 0,  /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(66), 1,  /* src */
                              cru + RK3576_CRU_GATE_CON(66), 2,  /* mclk */
                              cru + RK3576_CRU_GATE_CON(66), 0); /* hclk */

  /* SAI9 — CLKSEL_CON162 (0x0588), GATE_CON68.
   *   mclk_sel [11] (1-bit), src_sel [10:8], src_div [7:0].
   *   NOTE: gate bits are mclk=11, src_en=10, hclk=9. */

  RK3576_CLK_REGISTER_SAI_ONE(9, cru + RK3576_CRU_CLKSEL_CON(162), 11,
                              1,    /* mclk_sel shift/width */
                              8, 0, /* src_sel shift, src_div shift */
                              cru + RK3576_CRU_GATE_CON(68), 10, /* src */
                              cru + RK3576_CRU_GATE_CON(68), 11, /* mclk */
                              cru + RK3576_CRU_GATE_CON(68), 9); /* hclk */

  /* saiX_mclkin : clock input from gpio (frequency unknown, set to 0)
   * there is no mclkin for sai5~9 */
  clk_register_fixed_rate("sai0_mclkin", NULL, CLK_NAME_IS_STATIC, 0);
  clk_register_fixed_rate("sai1_mclkin", NULL, CLK_NAME_IS_STATIC, 0);
  clk_register_fixed_rate("sai2_mclkin", NULL, CLK_NAME_IS_STATIC, 0);
  clk_register_fixed_rate("sai3_mclkin", NULL, CLK_NAME_IS_STATIC, 0);
  clk_register_fixed_rate("sai4_mclkin", NULL, CLK_NAME_IS_STATIC, 0);
}
#endif /* CONFIG_RK3576_SAI */

#undef RK3576_CLK_REGISTER_SAI_ONE

/****************************************************************************
 * Name: rk3576_clk_register_sdio
 *
 * Description:
 *   Register the SDIO card-clock source and AHB bus gate.  CLKSEL_CON104
 *   contains a two-bit parent selector and a six-bit divider; GATE_CON42
 *   controls the downstream card and bus clocks.
 ****************************************************************************/

#ifdef CONFIG_RK3576_SDIO
static void rk3576_clk_register_sdio(void)
{
  static const char *g_sdio_parents[] = {
    "clk_gpll", /* 0b00 */
    "clk_cpll", /* 0b01 */
    "xin_osc0", /* 0b10; 0b11 is undefined */
  };
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long sel = cru + RK3576_CRU_CLKSEL_CON(104);
  FAR struct clk_s *mux;

  /* CLKSEL_CON104 (0x04a0): parent select [7:6], divider [5:0]. */

  mux = clk_register_mux(
      "cclk_src_sdio_sel", g_sdio_parents, nitems(g_sdio_parents),
      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
      sel, 6, 2, CLK_MUX_HIWORD_MASK);
  if (mux == NULL)
    {
      _err("CLK: failed to register cclk_src_sdio_sel\n");
      return;
    }

  clk_register_divider(
      "cclk_src_sdio_div", "cclk_src_sdio_sel",
      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
      sel, 0, 6, CLK_DIVIDER_HIWORD_MASK | CLK_DIVIDER_ROUND_CLOSEST);

  /* GATE_CON42 (0x08a8): card clock bit 11, AHB clock bit 12. */

  clk_register_gate("cclk_src_sdio", "cclk_src_sdio_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |
                        CLK_PARENT_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(42), 11,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("hclk_sdio", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(42), 12,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_SDIO */

/****************************************************************************
 * Name: rk3576_clk_register_emmc
 *
 * Description:
 *   Register the RK3576 NVM roots and complete eMMC clock domain.  The card
 *   source is a GPLL/CPLL/24 MHz mux followed by a six-bit divider.  The
 *   controller also consumes AHB, AXI, bus and timer clocks whose gates are
 *   kept under the common NuttX clock framework.
 ****************************************************************************/

#ifdef CONFIG_RK3576_EMMC
static void rk3576_clk_register_emmc(void)
{
  static const char *pll_parents[] = {
    "clk_gpll", /* 0b0 */
    "clk_cpll", /* 0b1 */
  };
  static const char *nvm_bus_parents[] = {
    "clk_gpll_div6",  /* 0b00 */
    "clk_cpll_div10", /* 0b01 */
    "clk_cpll_div20", /* 0b10 */
    "xin_osc0",       /* 0b11 */
  };
  static const char *emmc_card_parents[] = {
    "clk_gpll", /* 0b00 */
    "clk_cpll", /* 0b01 */
    "xin_osc0", /* 0b10; 0b11 is undefined */
  };
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long nvm_sel = cru + RK3576_CRU_CLKSEL_CON(88);
  const unsigned long card_sel = cru + RK3576_CRU_CLKSEL_CON(89);
  FAR struct clk_s *mux;

  /* HCLK_NVM_ROOT: CLKSEL_CON88 parent [1:0], GATE_CON33 bit 0. */

  clk_register_mux(
      "hclk_nvm_root_sel", nvm_bus_parents, nitems(nvm_bus_parents),
      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
      nvm_sel, 0, 2, CLK_MUX_HIWORD_MASK);
  clk_register_gate("hclk_nvm_root", "hclk_nvm_root_sel",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |
                        CLK_PARENT_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(33), 0,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* ACLK_NVM_ROOT: CLKSEL_CON88 parent bit 7, divider [6:2], gate bit 1. */

  clk_register_mux("aclk_nvm_root_sel", pll_parents, nitems(pll_parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |
                       CLK_PARENT_NAME_IS_STATIC,
                   nvm_sel, 7, 1, CLK_MUX_HIWORD_MASK);
  clk_register_divider(
      "aclk_nvm_root_div", "aclk_nvm_root_sel",
      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
      nvm_sel, 2, 5, CLK_DIVIDER_HIWORD_MASK | CLK_DIVIDER_ROUND_CLOSEST);
  clk_register_gate("aclk_nvm_root", "aclk_nvm_root_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |
                        CLK_PARENT_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(33), 1,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* CCLK_SRC_EMMC: CLKSEL_CON89 parent [15:14], divider [13:8], gate bit 8. */

  mux = clk_register_mux(
      "cclk_src_emmc_sel", emmc_card_parents, nitems(emmc_card_parents),
      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
      card_sel, 14, 2, CLK_MUX_HIWORD_MASK);
  if (mux == NULL)
    {
      _err("CLK: failed to register cclk_src_emmc_sel\n");
      return;
    }

  clk_register_divider(
      "cclk_src_emmc_div", "cclk_src_emmc_sel",
      CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
      card_sel, 8, 6, CLK_DIVIDER_HIWORD_MASK | CLK_DIVIDER_ROUND_CLOSEST);
  clk_register_gate("cclk_src_emmc", "cclk_src_emmc_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |
                        CLK_PARENT_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(33), 8,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* Controller interface and timing clocks. */

  clk_register_gate("hclk_emmc", "hclk_nvm_root", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(33), 9,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
  clk_register_gate("aclk_emmc", "aclk_nvm_root", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(33), 10,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_mux("bclk_emmc_sel", nvm_bus_parents, nitems(nvm_bus_parents),
                   CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC |
                       CLK_PARENT_NAME_IS_STATIC,
                   cru + RK3576_CRU_CLKSEL_CON(90), 0, 2, CLK_MUX_HIWORD_MASK);
  clk_register_gate("bclk_emmc", "bclk_emmc_sel", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(33), 11,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
  clk_register_gate("tclk_emmc", "xin_osc0", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(33), 12,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_EMMC */

/****************************************************************************
 * Name: rk3576_clk_register_dmac
 *
 * Description:
 *   Register the three DMA controllers' aclk gate clocks (aclk_dmac0/1/2)
 *   with the NuttX CLK framework.  Each gate is a single hiword-mask bit in
 *   CRU_GATE_CON19, SET_TO_DISABLE (high = clock off); enabling the clock
 *   writes 0 to the gate bit.
 *
 *   Per the TRM the aclk gates (aclk_dmac0/1/2_en) sit at GATE_CON19 bits
 *   1/2/3.  The parent clock is aclk_bus_root (AXI bus domain).
 ****************************************************************************/

#ifdef CONFIG_RK3576_DMA
static void rk3576_clk_register_dmac(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* aclk_dmac0 — GATE_CON19 bit 1. */

  clk_register_gate("aclk_dmac0", "aclk_bus_root", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(19), 1,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* aclk_dmac1 — GATE_CON19 bit 2. */

  clk_register_gate("aclk_dmac1", "aclk_bus_root", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(19), 2,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* aclk_dmac2 — GATE_CON19 bit 3. */

  clk_register_gate("aclk_dmac2", "aclk_bus_root", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(19), 3,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_DMA */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_clk_set_litcore_cpufreq
 *
 * Description:
 *   Set the LITTLE-core (litcore) CPU frequency once at boot by specifying
 *   the desired frequency in MHz (e.g. 1200 for 1.2 GHz).  The value must
 *   match one of the LPLL frequency table entries exactly (see
 *   enum rk3576_litcore_rate_e); otherwise -EINVAL is returned and the CPU
 *   frequency is left at the bootloader-configured value.
 *
 *   This is a one-shot configuration helper — it does NOT implement DVFS.
 *   It configures the LIT core only; the big-core cluster (BPLL) has its
 *   own clock path and is not touched here.
 *
 *   Scope note: this helper reprograms LPLL and normalizes the CPU source
 *   divider only.  It deliberately does NOT re-scale the dependent bus
 *   dividers (aclk_m_litcore / pclk_dbg_litcore / CCI) nor change the
 *   regulator OPP voltage, both of which a full DVFS transition would
 *   require.  All LPLL gears remain selectable; whether a given gear is
 *   stable on a particular part depends on its silicon quality and supply
 *   voltage and is left for the caller to validate.
 *
 *   The switch sequence mirrors the Linux CPUFreq transition model.  While
 *   LPLL is being reprogrammed the PLL is momentarily unlocked, so the
 *   CPU clock source must first be moved to a safe parent (clk_gpll) and
 *   switched back to clk_lpll only after the PLL has re-locked:
 *
 *     1. Reparent clk_litcore_src_sel -> clk_gpll  (safe source)
 *     2. clk_set_rate(clk_lpll,      target rate) (reprogram LPLL)
 *     3. Reparent clk_litcore_src_sel -> clk_lpll  (trim-to-lock, switch back)
 *     4. clk_set_rate(clk_litcore_src_div, target) (normalize to 1:1)
 *
 *   Step 4 must run AFTER switching back to clk_lpll (rather than while the
 *   source is clk_gpll) so the divider's parent_rate is the new LPLL rate
 *   and the framework picks div_con = 0 (divide-by-1), giving CPU == LPLL.
 *
 *   NOTE: On arm64, up_udelay() and the systick are driven by the generic
 *   arch timer whose frequency is constant — a one-time CPU clock change
 *   therefore does not require re-calibrating loops_per_msec.
 *
 * Input Parameters:
 *   mhz - Desired LITTLE-core CPU frequency in MHz.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure (-EINVAL if the frequency
 *   is not an exact entry of the LPLL table).
 ****************************************************************************/

int rk3576_clk_set_litcore_cpufreq(uint32_t mhz)
{
  FAR struct clk_s *litcore_src_sel;
  FAR struct clk_s *gpll;
  FAR struct clk_s *lpll;
  FAR struct clk_s *src_div;
  uint32_t want_hz = mhz * 1000000UL;
  uint32_t target_rate = 0;
  int i;
  int ret;

  /* Look up the requested MHz in the LPLL rate table.  Only an exact match
   * is accepted so a typo in the Kconfig value cannot silently pick a
   * slightly-different gear.
   */

  for (i = 0; i < G_LPLL_RATE_TABLE_SIZE; i++)
    {
      if (g_lpll_rate_table[i].rate == want_hz)
        {
          target_rate = want_hz;
          break;
        }
    }

  if (target_rate == 0)
    {
      _err("CLK: no LPLL frequency table entry for %" PRIu32 " MHz\n", mhz);
      return -EINVAL;
    }

  litcore_src_sel = clk_get("clk_litcore_src_sel");
  gpll = clk_get("clk_gpll");
  lpll = clk_get("clk_lpll");
  src_div = clk_get("clk_litcore_src_div");
  if (!litcore_src_sel || !gpll || !lpll || !src_div)
    {
      _err("CLK: failed to resolve litcore CPU clocks\n");
      return -ENOENT;
    }

  /* Step 1 — reparent to GPLL (safe source) so the CPU keeps running
   * while LPLL is power-cycled and reprogrammed.
   */

  ret = clk_set_parent(litcore_src_sel, gpll);
  if (ret < 0)
    {
      _err("CLK: failed to switch CPU source to GPLL: %d\n", ret);
      return ret;
    }

  /* Step 2 — reprogram LPLL to the target frequency.  LVGL/board init is
   * single-threaded at this point, so there is no race with other drivers.
   */

  ret = clk_set_rate(lpll, target_rate);
  if (ret < 0)
    {
      _err("CLK: failed to set LPLL to %" PRIu32 " Hz: %d\n", target_rate,
           ret);

      /* clk_set_rate() may fail while the PLL is mid-reprogram and briefly
       * unlocked (e.g. LPLL lock timeout).  Switching the CPU back to LPLL
       * here would hang it.  Keep the CPU on the safe GPLL source and bail
       * out — rk3576_fracpll_set_rate() has already restored the previous
       * PLL parameters, so LPLL is back to a known-good frequency, but we
       * do NOT blindly re-parent to it without first confirming a lock.
       */

      return ret;
    }

  /* Step 3 — reparent back to LPLL now that it is stable. */

  ret = clk_set_parent(litcore_src_sel, lpll);
  if (ret < 0)
    {
      _err("CLK: failed to switch CPU source back to LPLL: %d\n", ret);
      return ret;
    }

  /* Step 4 — normalize clk_litcore_src_div to 1:1 so the CPU runs at the
   * exact LPLL output frequency.  This divider has no CLK_SET_RATE_PARENT,
   * so set it explicitly, AFTER switching back to clk_lpll.  Now
   * parent_rate == target_rate so the divider clamps to div_con = 0
   * (divide-by-1) and CPU rate == LPLL rate.
   */

  ret = clk_set_rate(src_div, target_rate);
  if (ret < 0)
    {
      _err("CLK: failed to normalize litcore src_div: %d\n", ret);
      return ret;
    }

  _info("CLK: litcore CPU running at %" PRIu32 " Hz\n", target_rate);
  return OK;
}

/****************************************************************************
 * Name: rk3576_clk_tree_initialize
 *
 * Description:
 *   Register the full RK3576 clock tree with the NuttX CLK framework.
 *   Call this once during board/chip init, before any peripheral driver
 *   calls clk_get().
 ****************************************************************************/

void rk3576_clk_tree_initialize(void)
{
  rk3576_clk_register_pll_factors();

  /* Register the AXI/AHB/APB bus root clocks (aclk_/hclk_/pclk_ roots) so
   * that peripheral bus-interface gates can attach to their real parents
   * instead of NULL.
   */

  rk3576_clk_register_axi();
  rk3576_clk_register_ahb();
  rk3576_clk_register_apb();
  rk3576_clk_register_litcore();

  rk3576_clk_register_matrix_audio();

#ifdef CONFIG_RK3576_I2C
  rk3576_clk_register_i2c();
#endif

#ifdef CONFIG_RK3576_PWM
  rk3576_clk_register_pwm();
#endif

#ifdef CONFIG_RK3576_UART
  rk3576_clk_register_matrix_uart();
  rk3576_clk_register_uart();
#endif

#ifdef CONFIG_RK3576_SAI
  rk3576_clk_register_sai();
#endif

#ifdef CONFIG_RK3576_DMA
  rk3576_clk_register_dmac();
#endif

#ifdef CONFIG_RK3576_SDIO
  rk3576_clk_register_sdio();
#endif

#ifdef CONFIG_RK3576_EMMC
  rk3576_clk_register_emmc();
#endif

#ifdef CONFIG_RK3576_FSPI
  rk3576_clk_register_fspi();
#endif
}
