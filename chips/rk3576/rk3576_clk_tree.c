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
#include <stdio.h>
#include <sys/param.h>

#include <nuttx/clk/clk.h>
#include <nuttx/clk/clk_provider.h>

#include "arm64_internal.h"
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
};

/* Forward declaration */

static uint32_t rk3576_fracpll_recalc_rate(struct clk_s *clk,
                                           uint32_t parent_rate);

static const struct clk_ops_s g_rk3576_fracpll_ops = {
  .recalc_rate = rk3576_fracpll_recalc_rate,
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
   * Parent is xin_osc0 so the CLK framework provides 24 MHz to recalc_rate.
   */

  gpll_priv.con_base = RK3576_CRU_ADDR + RK3576_CRU_GPLL_CON(0);

  gpll = clk_register("clk_gpll", g_pll_parents, 1,
                      CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                      &g_rk3576_fracpll_ops, &gpll_priv, sizeof(gpll_priv));
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
   * Parent is xin_osc0 so the CLK framework provides 24 MHz to recalc_rate.
   */

  cpll_priv.con_base = RK3576_CRU_ADDR + RK3576_CRU_CPLL_CON(0);

  cpll = clk_register("clk_cpll", g_pll_parents, 1,
                      CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                      &g_rk3576_fracpll_ops, &cpll_priv, sizeof(cpll_priv));
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
   * Parent is xin_osc0 so the CLK framework provides 24 MHz to recalc_rate.
   */
  aupll_priv.con_base = RK3576_CRU_ADDR + RK3576_CRU_AUPLL_CON(0);
  aupll = clk_register("clk_aupll", g_pll_parents, 1,
                       CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                       &g_rk3576_fracpll_ops, &aupll_priv, sizeof(aupll_priv));

  DEBUGASSERT(aupll);
  UNUSED(aupll);
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

#define RK3576_CLK_REGISTER_I2C_ONE(bus, sel_reg, sel_shift, pclk_reg,     \
                                    pclk_bit, clk_reg, clk_bit)            \
  do                                                                       \
    {                                                                      \
      struct clk_s *_mux;                                                  \
                                                                           \
      _mux = clk_register_mux("clk_i2c" #bus "_sel", g_i2c_sel_parents,    \
                              nitems(g_i2c_sel_parents),                   \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,    \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK); \
      if (!_mux)                                                           \
        {                                                                  \
          _err("CLK: failed to register clk_i2c" #bus "_sel\n");           \
          break;                                                           \
        }                                                                  \
                                                                           \
      clk_register_gate("pclk_i2c" #bus "_en", NULL, CLK_NAME_IS_STATIC,   \
                        pclk_reg, pclk_bit,                                \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
                                                                           \
      clk_register_gate("clk_i2c" #bus "_en", "clk_i2c" #bus "_sel",       \
                        CLK_NAME_IS_STATIC, clk_reg, clk_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
    }                                                                      \
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
 *   - pclk_i2cX_en   : APB bus interface gate
 *   - clk_i2cX_en    : SCL functional clock gate
 ****************************************************************************/

#ifdef CONFIG_RK3576_I2C
static void rk3576_clk_register_i2c(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;

  /* I2C0 — PMU1 domain */

  RK3576_CLK_REGISTER_I2C_ONE(0, pmu1 + RK3576_PMU1CRU_CLKSEL_CON(6),
                              7,                                     /* mux */
                              pmu1 + RK3576_PMU1CRU_GATE_CON(5), 1,  /* pclk */
                              pmu1 + RK3576_PMU1CRU_GATE_CON(5), 2); /* clk  */

  /* I2C1–8 — main CRU domain, CLKSEL_CON(57) consecutive 2-bit slots */

  RK3576_CLK_REGISTER_I2C_ONE(1, cru + RK3576_CRU_CLKSEL_CON(57), 0,
                              cru + RK3576_CRU_GATE_CON(12), 0,
                              cru + RK3576_CRU_GATE_CON(12), 12);

  RK3576_CLK_REGISTER_I2C_ONE(2, cru + RK3576_CRU_CLKSEL_CON(57), 2,
                              cru + RK3576_CRU_GATE_CON(12), 1,
                              cru + RK3576_CRU_GATE_CON(12), 13);

  RK3576_CLK_REGISTER_I2C_ONE(3, cru + RK3576_CRU_CLKSEL_CON(57), 4,
                              cru + RK3576_CRU_GATE_CON(12), 2,
                              cru + RK3576_CRU_GATE_CON(12), 14);

  RK3576_CLK_REGISTER_I2C_ONE(4, cru + RK3576_CRU_CLKSEL_CON(57), 6,
                              cru + RK3576_CRU_GATE_CON(12), 3,
                              cru + RK3576_CRU_GATE_CON(12), 15);

  RK3576_CLK_REGISTER_I2C_ONE(5, cru + RK3576_CRU_CLKSEL_CON(57), 8,
                              cru + RK3576_CRU_GATE_CON(12), 4,
                              cru + RK3576_CRU_GATE_CON(13), 0);

  RK3576_CLK_REGISTER_I2C_ONE(6, cru + RK3576_CRU_CLKSEL_CON(57), 10,
                              cru + RK3576_CRU_GATE_CON(12), 5,
                              cru + RK3576_CRU_GATE_CON(13), 1);

  RK3576_CLK_REGISTER_I2C_ONE(7, cru + RK3576_CRU_CLKSEL_CON(57), 12,
                              cru + RK3576_CRU_GATE_CON(12), 6,
                              cru + RK3576_CRU_GATE_CON(13), 2);

  RK3576_CLK_REGISTER_I2C_ONE(8, cru + RK3576_CRU_CLKSEL_CON(57), 14,
                              cru + RK3576_CRU_GATE_CON(12), 7,
                              cru + RK3576_CRU_GATE_CON(13), 3);

  /* I2C9 — CLKSEL_CON(58) */

  RK3576_CLK_REGISTER_I2C_ONE(9, cru + RK3576_CRU_CLKSEL_CON(58), 0,
                              cru + RK3576_CRU_GATE_CON(12), 8,
                              cru + RK3576_CRU_GATE_CON(13), 4);
}
#endif /* CONFIG_RK3576_I2C */

#undef RK3576_CLK_REGISTER_I2C_ONE

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
                                    rc_bit)                                 \
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
      clk_register_gate("pclk_pwm" #ctrl "_en", NULL, CLK_NAME_IS_STATIC,   \
                        gate_reg, pclk_bit,                                 \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      clk_register_gate("clk_pwm" #ctrl "_en", "clk_pwm" #ctrl "_sel",      \
                        CLK_NAME_IS_STATIC, gate_reg, clk_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      clk_register_gate("clk_pwm" #ctrl "_osc_en", "xin_osc0",              \
                        CLK_NAME_IS_STATIC, gate_reg, osc_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      /* NOTE: clk_pwmX_rc_en is registered but currently unusable.         \
       * The upstream clock source has not been proven to produce           \
       * a valid clock on the PWM output.  Scope measurements showed no     \
       * waveform even with the gate enabled and PWM_CLK_CTRL set to        \
       * RC source.  Until the full clock chain is verified, this gate      \
       * is effectively dead code in the tree.                              \
       * Do NOT rely on clk_pwmX_rc_en for production use.                  \
       */                                                                   \
      clk_register_gate("clk_pwm" #ctrl "_rc_en", NULL, CLK_NAME_IS_STATIC, \
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
 *   - clk_pwmX_sel    : 2-bit mux (CPLL/10, CPLL/20, XIN_OSC0)
 *   - pclk_pwmX_en    : APB bus interface gate
 *   - clk_pwmX_en     : Primary PWM functional gate
 *   - clk_pwmX_osc_en : External oscillator alternative gate
 *   - clk_pwmX_rc_en  : Internal RC oscillator alternative gate
 ****************************************************************************/

#ifdef CONFIG_RK3576_PWM
static void rk3576_clk_register_pwm(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;

  /* PWM0 — PMU1 domain */

  RK3576_CLK_REGISTER_PWM_ONE(0, pmu1 + RK3576_PMU1CRU_CLKSEL_CON(5), 2,
                              pmu1 + RK3576_PMU1CRU_GATE_CON(4), 11, 12, 13,
                              pmu1 + RK3576_PMU1CRU_GATE_CON(5), 7);

  /* PWM1 — main CRU domain */

  RK3576_CLK_REGISTER_PWM_ONE(1, cru + RK3576_CRU_CLKSEL_CON(71), 8,
                              cru + RK3576_CRU_GATE_CON(16), 10, 11, 13,
                              cru + RK3576_CRU_GATE_CON(16), 15);

  /* PWM2 — main CRU domain */

  RK3576_CLK_REGISTER_PWM_ONE(2, cru + RK3576_CRU_CLKSEL_CON(74), 6,
                              cru + RK3576_CRU_GATE_CON(20), 4, 5, 7,
                              cru + RK3576_CRU_GATE_CON(20), 6);
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

#define RK3576_CLK_REGISTER_MATRIX_UART_FRAC_ONE(index, div_reg, sel_reg,     \
                                                 gate_bit)                    \
  do                                                                          \
    {                                                                         \
      struct clk_s *_mux;                                                     \
                                                                              \
      _mux = clk_register_mux("clk_matrix_uart_frac_" #index "_sel",          \
                              g_matrix_uart_frac_sel_parents,                 \
                              nitems(g_matrix_uart_frac_sel_parents),         \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,       \
                              sel_reg, 0, 2, CLK_MUX_HIWORD_MASK);            \
      if (!_mux)                                                              \
        {                                                                     \
          _err("CLK: failed to register "                                     \
               "clk_matrix_uart_frac_" #index "_sel\n");                      \
          break;                                                              \
        }                                                                     \
                                                                              \
      clk_register_fractional_divider("clk_matrix_uart_frac_" #index,         \
                                      "clk_matrix_uart_frac_" #index "_sel",  \
                                      CLK_NAME_IS_STATIC, div_reg, 16, 16, 0, \
                                      16, 0);                                 \
                                                                              \
      clk_register_gate("clk_matrix_uart_frac_" #index "_en",                 \
                        "clk_matrix_uart_frac_" #index, CLK_NAME_IS_STATIC,   \
                        RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(2), gate_bit,   \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);      \
    }                                                                         \
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
          "sclk_uart" #index, "sclk_uart" #index "_sel",                     \
          CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, sel_reg, div_shift, 8,   \
          CLK_DIVIDER_HIWORD_MASK);                                          \
      if (!_div)                                                             \
        {                                                                    \
          _err("CLK: failed to register sclk_uart" #index "\n");             \
          break;                                                             \
        }                                                                    \
                                                                             \
      clk_register_gate("pclk_uart" #index "_en", NULL, CLK_NAME_IS_STATIC,  \
                        pclk_reg, pclk_bit,                                  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);     \
                                                                             \
      clk_register_gate("sclk_uart" #index "_en", "sclk_uart" #index,        \
                        CLK_NAME_IS_STATIC, sclk_reg, sclk_bit,              \
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
 *           -> clk_uart1_src_top_en (gate, CRU_GATE_CON02[13])
 *
 *     Level 2 (PMU1CRU domain, PMU1CRU_CLKSEL_CON08):
 *       sclk_uart1_sel (1-bit mux, [0])
 *         0: clk_uart1_src_top (from level 1)
 *         1: xin_osc0_func    (bypass, 24 MHz direct)
 *         -> sclk_uart1_en (gate, PMU1CRU_GATE_CON05[5])
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
   *     [13]    = clk_uart1_src_top_en  (SET_TO_DISABLE)
   *
   * Level 2: sclk_uart1 (PMU1CRU domain)
   *   PMU1CRU_CLKSEL_CON08 (0x27220320):
   *     [0]     = sclk_uart1_sel (1-bit mux)
   *               0 = clk_uart1_src_top, 1 = xin_osc0_func
   *   PMU1CRU_GATE_CON05:
   *     [5]     = sclk_uart1_en  (SET_TO_DISABLE)
   *     [6]     = pclk_uart1_en  (SET_TO_DISABLE)
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

    clk_register_divider("clk_uart1_src_top", "clk_uart1_src_top_sel",
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(27), 5, 8,
                         CLK_DIVIDER_HIWORD_MASK);

    /* Level 1: clk_uart1_src_top_en gate (CRU_GATE_CON02[13]) */

    clk_register_gate("clk_uart1_src_top_en", "clk_uart1_src_top",
                      CLK_NAME_IS_STATIC, cru + RK3576_CRU_GATE_CON(2), 13,
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
    /* sclk_uart1 — pass-through (div=1, mul=1) to preserve the
     * "sclk_uartN" naming convention used by all other UARTs.
     * UART1 has no local divider; the mux output IS the sclk.
     */

    clk_register_fixed_factor("sclk_uart1", "sclk_uart1_sel",
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC, 1, 1);

    /* sclk_uart1_en gate */

    clk_register_gate("sclk_uart1_en", "sclk_uart1", CLK_NAME_IS_STATIC,
                      pmu1 + RK3576_PMU1CRU_GATE_CON(5), 5,
                      CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

    /* pclk_uart1_en gate */

    clk_register_gate("pclk_uart1_en", NULL, CLK_NAME_IS_STATIC,
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
      clk_register_fractional_divider("clk_matrix_audio_frac_" #index,        \
                                      "clk_matrix_audio_frac_" #index "_sel", \
                                      CLK_NAME_IS_STATIC, div_reg, 16, 16, 0, \
                                      16, 0);                                 \
                                                                              \
      clk_register_gate("clk_matrix_audio_frac_" #index "_en",                \
                        "clk_matrix_audio_frac_" #index, CLK_NAME_IS_STATIC,  \
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
      clk_register_divider("clk_matrix_audio_int_" #index, parent_name,     \
                           CLK_NAME_IS_STATIC, div_reg, div_shift, 5,       \
                           CLK_DIVIDER_HIWORD_MASK);                        \
                                                                            \
      clk_register_gate("clk_matrix_audio_int_" #index "_en",               \
                        "clk_matrix_audio_int_" #index, CLK_NAME_IS_STATIC, \
                        gate_reg, gate_bit,                                 \
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
 * (src mux + src divider + mclk gate + hclk gate).
 *
 * The mclk_saiX_sel (external mclkin) mux layer is omitted for now;
 * the divider output is directly exposed as "mclk_saiX".  When external
 * mclkin support is needed, insert a mux here whose output is named
 * "mclk_saiX" — the driver clock name stays the same.
 *
 * Hardware chain:
 *   mclk_saiX_src_sel (3-bit MUX, 8 parents)
 *     -> mclk_saiX_src_div (8-bit divider, div_con + 1)
 *       -> mclk_saiX            (div output, exposed to driver)
 *         -> mclk_saiX_en       (GATE)
 *   hclk_saiX_en                (GATE, bus clock, no parent)
 *
 * Parameters:
 *   index     - SAI index (0..9), used in clock name suffix
 *   sel_reg   - CLKSEL register address (holds both src_sel and div)
 *   src_shift - mclk_saiX_src_sel bit offset in sel_reg (3-bit field)
 *   div_shift - mclk_saiX_src_div bit offset in sel_reg (8-bit field)
 *   hclk_reg  - hclk GATE register address
 *   hclk_bit  - hclk GATE bit
 *   mclk_reg  - mclk GATE register address
 *   mclk_bit  - mclk GATE bit
 */

#define RK3576_CLK_REGISTER_SAI_ONE(index, sel_reg, src_shift, div_shift,   \
                                    hclk_reg, hclk_bit, mclk_reg, mclk_bit) \
  do                                                                        \
    {                                                                       \
      struct clk_s *_src_sel;                                               \
      struct clk_s *_div;                                                   \
                                                                            \
      _src_sel = clk_register_mux(                                          \
          "mclk_sai" #index "_src_sel", g_sai_mclk_src_parents,             \
          nitems(g_sai_mclk_src_parents), CLK_NAME_IS_STATIC, sel_reg,      \
          src_shift, 3, CLK_MUX_HIWORD_MASK);                               \
      if (!_src_sel)                                                        \
        {                                                                   \
          _err("CLK: failed to register mclk_sai" #index "_src_sel\n");     \
          break;                                                            \
        }                                                                   \
                                                                            \
      _div = clk_register_divider(                                          \
          "mclk_sai" #index, "mclk_sai" #index "_src_sel",                  \
          CLK_NAME_IS_STATIC, sel_reg, div_shift, 8,                        \
          CLK_DIVIDER_HIWORD_MASK | CLK_DIVIDER_ROUND_CLOSEST);             \
      if (!_div)                                                            \
        {                                                                   \
          _err("CLK: failed to register mclk_sai" #index "\n");             \
          break;                                                            \
        }                                                                   \
                                                                            \
      clk_register_gate("hclk_sai" #index "_en", NULL, CLK_NAME_IS_STATIC,  \
                        hclk_reg, hclk_bit,                                 \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      clk_register_gate("mclk_sai" #index "_en", "mclk_sai" #index,         \
                        CLK_NAME_IS_STATIC, mclk_reg, mclk_bit,             \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
    }                                                                       \
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

  /* SAI0 — CLKSEL_CON44 (0x03B0), GATE_CON07 */

  RK3576_CLK_REGISTER_SAI_ONE(0, cru + RK3576_CRU_CLKSEL_CON(44),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(7), 13,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(7), 12); /* mclk */

  /* SAI1 — CLKSEL_CON46 (0x03B8), GATE_CON08 */

  RK3576_CLK_REGISTER_SAI_ONE(1, cru + RK3576_CRU_CLKSEL_CON(46),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(8), 8,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(8), 7); /* mclk */

  /* SAI2 — CLKSEL_CON47 (0x03BC), GATE_CON08 */

  RK3576_CLK_REGISTER_SAI_ONE(2, cru + RK3576_CRU_CLKSEL_CON(47),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(8), 14,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(8), 11); /* mclk */

  /* SAI3 — CLKSEL_CON48 (0x03C0), GATE_CON08 */

  RK3576_CLK_REGISTER_SAI_ONE(3, cru + RK3576_CRU_CLKSEL_CON(48),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(8), 15,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(8), 13); /* mclk */

  /* SAI4 — CLKSEL_CON49 (0x03C4), GATE_CON08 (mclk src) +
   *        GATE_CON09 (hclk + mclk) */

  RK3576_CLK_REGISTER_SAI_ONE(4, cru + RK3576_CRU_CLKSEL_CON(49),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(9), 2,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(9), 0); /* mclk */

  /* SAI5 — CLKSEL_CON154 (0x0568), GATE_CON65
   *        src_sel at [12:10], div at [9:2] (different from SAI0~4). */

  RK3576_CLK_REGISTER_SAI_ONE(5, cru + RK3576_CRU_CLKSEL_CON(154),
                              10, /* src_sel [12:10] */
                              2,  /* div [9:2] */
                              cru + RK3576_CRU_GATE_CON(65), 5,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(65), 4); /* mclk */

  /* SAI6 — CLKSEL_CON155 (0x056C), GATE_CON65 */

  RK3576_CLK_REGISTER_SAI_ONE(6, cru + RK3576_CRU_CLKSEL_CON(155),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(65), 9,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(65), 8); /* mclk */

  /* SAI7 — CLKSEL_CON159 (0x057C), GATE_CON67 */

  RK3576_CLK_REGISTER_SAI_ONE(7, cru + RK3576_CRU_CLKSEL_CON(159),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(67), 10, /* hclk */
                              cru + RK3576_CRU_GATE_CON(67), 9); /* mclk */

  /* SAI8 — CLKSEL_CON157 (0x0574), GATE_CON66 */

  RK3576_CLK_REGISTER_SAI_ONE(8, cru + RK3576_CRU_CLKSEL_CON(157),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(66), 0,  /* hclk */
                              cru + RK3576_CRU_GATE_CON(66), 2); /* mclk */

  /* SAI9 — CLKSEL_CON162 (0x0588), GATE_CON68 */

  RK3576_CLK_REGISTER_SAI_ONE(9, cru + RK3576_CRU_CLKSEL_CON(162),
                              8, /* src_sel [10:8] */
                              0, /* div [7:0] */
                              cru + RK3576_CRU_GATE_CON(68), 9,   /* hclk */
                              cru + RK3576_CRU_GATE_CON(68), 11); /* mclk */
}
#endif /* CONFIG_RK3576_SAI */

#undef RK3576_CLK_REGISTER_SAI_ONE

/****************************************************************************
 * Public Functions
 ****************************************************************************/

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
}
