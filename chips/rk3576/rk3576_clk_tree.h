/****************************************************************************
 * chips/rk3576/rk3576_clk_tree.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CLK_TREE_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CLK_TREE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#ifdef CONFIG_CLK

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* LPLL (little-core PLL) supported frequencies.  Each constant's value is
 * the frequency in MHz and matches the `rate` field of the corresponding
 * entry in g_lpll_rate_table[] in rk3576_clk_tree.c — see that table for
 * the exact m/p/s/k programming parameters.
 *
 * These are the valid inputs to rk3576_clk_set_litcore_cpufreq() to select
 * the LITTLE-core CPU frequency once at boot.  The litcore cluster runs
 * directly off LPLL, so setting the LPLL output frequency sets the LIT
 * core CPU frequency.
 *
 * NOTE: This is LITTLE-core only.  The big-core cluster (BPLL) has its own
 * clock path and is NOT configured through this enum/driver.
 */

enum rk3576_litcore_rate_e
{
  RK3576_LITCORE_2400_MHZ = 2400,
  RK3576_LITCORE_2304_MHZ = 2304,
  RK3576_LITCORE_2208_MHZ = 2208,
  RK3576_LITCORE_2184_MHZ = 2184,
  RK3576_LITCORE_2088_MHZ = 2088,
  RK3576_LITCORE_2040_MHZ = 2040,
  RK3576_LITCORE_2016_MHZ = 2016,
  RK3576_LITCORE_1992_MHZ = 1992,
  RK3576_LITCORE_1896_MHZ = 1896,
  RK3576_LITCORE_1800_MHZ = 1800,
  RK3576_LITCORE_1704_MHZ = 1704,
  RK3576_LITCORE_1608_MHZ = 1608,
  RK3576_LITCORE_1584_MHZ = 1584,
  RK3576_LITCORE_1560_MHZ = 1560,
  RK3576_LITCORE_1536_MHZ = 1536,
  RK3576_LITCORE_1512_MHZ = 1512,
  RK3576_LITCORE_1488_MHZ = 1488,
  RK3576_LITCORE_1464_MHZ = 1464,
  RK3576_LITCORE_1440_MHZ = 1440,
  RK3576_LITCORE_1416_MHZ = 1416,
  RK3576_LITCORE_1392_MHZ = 1392,
  RK3576_LITCORE_1320_MHZ = 1320,
  RK3576_LITCORE_1200_MHZ = 1200,
  RK3576_LITCORE_1008_MHZ = 1008,
  RK3576_LITCORE_816_MHZ = 816,
  RK3576_LITCORE_600_MHZ = 600,
  RK3576_LITCORE_408_MHZ = 408,
  RK3576_LITCORE_312_MHZ = 312,
  RK3576_LITCORE_216_MHZ = 216,
  RK3576_LITCORE_96_MHZ = 96,
};

/****************************************************************************
 * Public Macros
 ****************************************************************************/

/* Compile-time validator for a LITTLE-core CPU frequency (MHz).  Returns
 * true only if the value is one of the enum rk3576_litcore_rate_e values
 * above (whose values ARE the MHz frequencies), so the list never drifts
 * out of sync with the enum.
 *
 * This lets a Kconfig-supplied frequency be checked at build time (via
 * _Static_assert) so a typo or an out-of-range value fails the compile
 * instead of being silently rejected at runtime.
 */

#define RK3576_LITCORE_CPU_FREQ_IS_VALID(mhz)                              \
  ((mhz) == RK3576_LITCORE_2400_MHZ || (mhz) == RK3576_LITCORE_2304_MHZ || \
   (mhz) == RK3576_LITCORE_2208_MHZ || (mhz) == RK3576_LITCORE_2184_MHZ || \
   (mhz) == RK3576_LITCORE_2088_MHZ || (mhz) == RK3576_LITCORE_2040_MHZ || \
   (mhz) == RK3576_LITCORE_2016_MHZ || (mhz) == RK3576_LITCORE_1992_MHZ || \
   (mhz) == RK3576_LITCORE_1896_MHZ || (mhz) == RK3576_LITCORE_1800_MHZ || \
   (mhz) == RK3576_LITCORE_1704_MHZ || (mhz) == RK3576_LITCORE_1608_MHZ || \
   (mhz) == RK3576_LITCORE_1584_MHZ || (mhz) == RK3576_LITCORE_1560_MHZ || \
   (mhz) == RK3576_LITCORE_1536_MHZ || (mhz) == RK3576_LITCORE_1512_MHZ || \
   (mhz) == RK3576_LITCORE_1488_MHZ || (mhz) == RK3576_LITCORE_1464_MHZ || \
   (mhz) == RK3576_LITCORE_1440_MHZ || (mhz) == RK3576_LITCORE_1416_MHZ || \
   (mhz) == RK3576_LITCORE_1392_MHZ || (mhz) == RK3576_LITCORE_1320_MHZ || \
   (mhz) == RK3576_LITCORE_1200_MHZ || (mhz) == RK3576_LITCORE_1008_MHZ || \
   (mhz) == RK3576_LITCORE_816_MHZ || (mhz) == RK3576_LITCORE_600_MHZ ||   \
   (mhz) == RK3576_LITCORE_408_MHZ || (mhz) == RK3576_LITCORE_312_MHZ ||   \
   (mhz) == RK3576_LITCORE_216_MHZ || (mhz) == RK3576_LITCORE_96_MHZ)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_clk_tree_initialize
 *
 * Description:
 *   Register the full RK3576 clock tree with the NuttX CLK framework.
 *   Must be called once during board/chip init, before any peripheral
 *   driver calls clk_get().
 ****************************************************************************/

void rk3576_clk_tree_initialize(void);

/****************************************************************************
 * Name: rk3576_clk_set_litcore_cpufreq
 *
 * Description:
 *   Set the LITTLE-core (litcore) CPU frequency once at boot by specifying
 *   the desired frequency in MHz (e.g. 1200 for 1.2 GHz).
 *
 *   The requested frequency must match one of the LPLL frequency table
 *   entries exactly (the enum rk3576_litcore_rate_e values); otherwise
 *   -EINVAL is returned and the CPU frequency is left unchanged (at the
 *   bootloader-configured value).  This is a one-shot configuration — it
 *   does NOT implement DVFS and is expected to run during board bring-up
 *   after rk3576_clk_tree_initialize(), before load-sensitive peripheral
 *   drivers are started.
 *
 *   The switch sequence mirrors the Linux CPUFreq transition model:
 *     1. Reparent clk_litcore_src_sel to clk_gpll (safe source) so the
 *        CPU does NOT lose its clock while LPLL is being reprogrammed.
 *     2. clk_set_rate(clk_lpll, target) to reprogram LPLL.
 *     3. Drive clk_litcore_src_div to 1:1 so CPU rate == LPLL rate.
 *     4. Reparent clk_litcore_src_sel back to clk_lpll.
 *
 *   NOTE: This configures the LIT core only.  The big-core cluster (BPLL)
 *   has its own clock path and is not touched here.
 *
 *   On arm64, up_udelay()/systick are driven by the generic arch timer
 *   (constant frequency), so a one-time CPU clock change does not require
 *   re-calibrating loops_per_msec.
 *
 * Input Parameters:
 *   mhz - Desired LITTLE-core CPU frequency in MHz.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure (-EINVAL if the frequency
 *   is not an exact entry of the LPLL table).
 ****************************************************************************/

int rk3576_clk_set_litcore_cpufreq(uint32_t mhz);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_CLK */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CLK_TREE_H */
