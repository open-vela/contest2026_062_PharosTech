/****************************************************************************
 * chips/rk3576/hardware/rk3576_cru.h
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
 * RK3576 Clock & Reset Unit (CRU) hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM, Chapter 5 "Clock & Reset Unit".
 * Cross-checked against mainline drivers/clk/rockchip/clk-rk3576.c.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "rk3576_memorymap.h"
#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* -----------------------------------------------------------------------
 * CRU register offset macros (16-bit hiword-mask write scheme).
 * -----------------------------------------------------------------------
 *
 * The "unit" macros (CON0..CON5) produce the slice for one PLL/clock
 * unit; the "array" macros (CLKSEL, GATE, SOFTRST) produce the n-th
 * register of a contiguous table, e.g. CLKSEL(3) = 0x030C.
 */

/* CRU: PLLs */

#define RK3576_CRU_BPLL_CON(n)  (0x0000 + ((n)*4))
#define RK3576_CRU_VPLL_CON(n)  (0x0160 + ((n)*4))
#define RK3576_CRU_AUPLL_CON(n) (0x0180 + ((n)*4))
#define RK3576_CRU_CPLL_CON(n)  (0x01A0 + ((n)*4))
#define RK3576_CRU_GPLL_CON(n)  (0x01C0 + ((n)*4))

/* CRU: Internal PLL mode select register */

#define RK3576_CRU_MODE_CON (0x0280)

/* CRU: Internal clock select and division register */

#define RK3576_CRU_CLKSEL_CON(n) (0x0300 + ((n)*4))

/* CRU: Internal clock gate and division register */

#define RK3576_CRU_GATE_CON(n) (0x0800 + ((n)*4))

/* CRU: Internal clock reset register */

#define RK3576_CRU_SOFTRST_CON(n) (0x0A00 + ((n)*4))

/* CRU: System control register */

#define RK3576_CRU_GLB_CNT_TH              (0x0C00)
#define RK3576_CRU_GLBRST_ST               (0x0C04)
#define RK3576_CRU_GLB_SRST_FST_VALUE      (0x0C08)
#define RK3576_CRU_GLB_SRST_SND_VALUE      (0x0C0C)
#define RK3576_CRU_GLB_RST_CON             (0x0C10)
#define RK3576_CRU_GLBRST_ST_NCLR          (0x0C14)
#define RK3576_CRU_NON_SECURE_GATING_CON00 (0x0C48)

/* CRU: Qchannel control register 1 */

#define RK3576_CRU_QCHANNEL_CON01 (0x0CA4)

/* CRU: Smoothdiv control register */

#define RK3576_CRU_SMOTH_DIVFREE_CON(n) (0x0CC0 + ((n)*4))

/* CRU: Fracdiv 24bit control register */

#define RK3576_CRU_HIGH16BIT_AUDIO_FRAC(n) (0x0CD4 + ((n)*4))

/* CRU: Auto clock switch control */

#define RK3576_CRU_AUTOCS_HCLK_TOP_BIU_CON         (0x0D10)
#define RK3576_CRU_AUTOCS_ACLK_BUS_ROOT_CON        (0x0D20)
#define RK3576_CRU_AUTOCS_PCLK_BUS_ROOT_CON        (0x0D28)
#define RK3576_CRU_AUTOCS_HCLK_RKNN_ROOT_CON       (0x0D38)
#define RK3576_CRU_AUTOCS_ACLK_NVM_ROOT_CON        (0x0D40)
#define RK3576_CRU_AUTOCS_ACLK_PHP_ROOT_CON        (0x0D48)
#define RK3576_CRU_AUTOCS_ACLK_RKVDEC_ROOT_CON     (0x0D50)
#define RK3576_CRU_AUTOCS_ACLK_USB_ROOT_CON        (0x0D68)
#define RK3576_CRU_AUTOCS_ACLK_VPU_ROOT_CON        (0x0D70)
#define RK3576_CRU_AUTOCS_ACLK_VPU_LOW_ROOT_CON    (0x0D78)
#define RK3576_CRU_AUTOCS_ACLK_JPEG_ROOT_CON       (0x0D80)
#define RK3576_CRU_AUTOCS_ACLK_VEPU0_ROOT_CON      (0x0D88)
#define RK3576_CRU_AUTOCS_ACLK_VI_ROOT_CON         (0x0D98)
#define RK3576_CRU_AUTOCS_ACLK_VOP_ROOT_CON        (0x0DA0)
#define RK3576_CRU_AUTOCS_ACLK_VPU_MID_ROOT_CON    (0x0DB0)
#define RK3576_CRU_AUTOCS_ACLK_TOP_BIU_CON         (0x0DB8)
#define RK3576_CRU_AUTOCS_ACLK_CENTER_ROOT_CON     (0x0DE8)
#define RK3576_CRU_AUTOCS_ACLK_CENTER_LOW_ROOT_CON (0x0DF0)
#define RK3576_CRU_AUTOCS_HCLK_BUS_CM0_ROOT_CON    (0x0E18)
#define RK3576_CRU_AUTOCS_PCLK_NPU_TOP_ROOT_CON    (0x0E20)
#define RK3576_CRU_AUTOCS_HCLK_NPU_CM0_ROOT_CON    (0x0E28)
#define RK3576_CRU_AUTOCS_HCLK_NVM_ROOT_CON        (0x0E30)
#define RK3576_CRU_AUTOCS_PCLK_PHP_ROOT_CON        (0x0E38)
#define RK3576_CRU_AUTOCS_HCLK_RKVDEC_ROOT_CON     (0x0E48)
#define RK3576_CRU_AUTOCS_PCLK_TOP_ROOT_CON        (0x0E58)
#define RK3576_CRU_AUTOCS_PCLK_USB_ROOT_CON        (0x0E70)
#define RK3576_CRU_AUTOCS_HCLK_VPU_ROOT_CON        (0x0E78)
#define RK3576_CRU_AUTOCS_HCLK_VEPU0_ROOT_CON      (0x0E80)
#define RK3576_CRU_AUTOCS_HCLK_VI_ROOT_CON         (0x0E90)
#define RK3576_CRU_AUTOCS_PCLK_VI_ROOT_CON         (0x0E98)
#define RK3576_CRU_AUTOCS_HCLK_VOP_ROOT_CON        (0x0EA8)
#define RK3576_CRU_AUTOCS_PCLK_VOP_ROOT_CON        (0x0EB0)
#define RK3576_CRU_AUTOCS_HCLK_VO0_ROOT_CON        (0x0EB8)
#define RK3576_CRU_AUTOCS_ACLK_TOP_MID_BIU_CON     (0x0EE0)
#define RK3576_CRU_AUTOCS_PCLK_VO1_ROOT_CON        (0x0EE8)
#define RK3576_CRU_AUTOCS_HCLK_VO1_ROOT_CON        (0x0EF0)
#define RK3576_CRU_AUTOCS_HCLK_DDR_ROOT_CON        (0x0F00)
#define RK3576_CRU_AUTOCS_PCLK_CENTER_ROOT_CON     (0x0F08)
#define RK3576_CRU_AUTOCS_ACLK_DDR_ROOT_CON        (0x0F10)
#define RK3576_CRU_AUTOCS_ACLK_VO1_ROOT_CON        (0x0F18)
#define RK3576_CRU_AUTOCS_HCLK_CENTER_ROOT_CON     (0x0F20)
#define RK3576_CRU_AUTOCS_CLK_GPU_INNER_CON        (0x0F30)
#define RK3576_CRU_AUTOCS_HCLK_AUDIO_ROOT_CON      (0x0F40)
#define RK3576_CRU_AUTOCS_PCLK_DDR_ROOT_CON        (0x0F50)
#define RK3576_CRU_AUTOCS_CLK_ISPCORE_ROOT_CON     (0x0F58)
#define RK3576_CRU_AUTOCS_CLK_VEPU0_CORE_ROOT_CON  (0x0F60)
#define RK3576_CRU_AUTOCS_ACLK_VEPU1_ROOT_CON      (0x0F68)
#define RK3576_CRU_AUTOCS_HCLK_VEPU1_ROOT_CON      (0x0F70)
#define RK3576_CRU_AUTOCS_CLK_VEPU1_CORE_ROOT_CON  (0x0F78)

/* PMU1CRU: Internal clock select and division */

#define RK3576_PMU1CRU_CLKSEL_CON(n) \
  (((n) == 30) ? (0x4000) : (((n) == 32) ? (0x4008) : ((0x0300 + ((n)*4)))))

/* PMU1CRU: Internal clock gate and division register */

#define RK3576_PMU1CRU_GATE_CON(n) \
  (((n) == 10) ? (0x4028) : (((n) == 12) ? (0x4030) : ((0x0800 + ((n)*4)))))

/* PMU1CRU: Internal clock reset register */

#define RK3576_PMU1CRU_SOFTRST_CON(n) \
  (((n) == 10) ? (0x4050) : (((n) == 12) ? (0x4058) : ((0x0A00 + ((n)*4)))))

/* PMU1CRU: Auto clock switch control */

#define RK3576_PMU1CRU_AUTOCS_PCLK_PMU0_ROOT_SRC_CON    (0x0B00)
#define RK3576_PMU1CRU_AUTOCS_HCLK_PMU1_ROOT_SRC_CON    (0x0B08)
#define RK3576_PMU1CRU_AUTOCS_HCLK_PMU_CM0_ROOT_SRC_CON (0x0B10)

/* Deepslow detect */

#define RK3576_PMU1CRU_DEEPSLOW_DETECT_CON (0x0B40)
#define RK3576_PMU1CRU_DEEPSLOW_DETECT_ST  (0x0B44)

/* -----------------------------------------------------------------------
 * LITCORE_CRU registers (little-core power domain, 0x27240000).
 *
 * The little-core (LIT) cluster clocks (aclk_m_litcore, clk_litcore,
 * pclk_litcore_root, pclk_dbg_litcore, etc.) are generated inside the
 * LITCORE_CRU.  Its register layout mirrors the main CRU with a MODE_CON,
 * CLKSEL_CON[] and GATE_CON[] tables at the same relative offsets.
 *
 * Reference: RK3576 TRM Chapter 2.11 "LITCORE_CRU".
 */

#define RK3576_LITCORECRU_MODE_CON       (0x0280)
#define RK3576_LITCORECRU_CLKSEL_CON(n)  (0x0300 + ((n)*4))
#define RK3576_LITCORECRU_GATE_CON(n)    (0x0800 + ((n)*4))
#define RK3576_LITCORECRU_SOFTRST_CON(n) (0x0A00 + ((n)*4))

/* -----------------------------------------------------------------------
 * CCI_CRU registers (CCI power domain, 0x27248000).
 *
 * The LPLL (Little-core PLL) resides in the CCI_CRU and feeds the
 * little-core cluster.  It is a FRACPLL with the same register layout
 * as other Rockchip FRACPLLs.
 *
 * Reference: RK3576 TRM Chapter 2.12 "CCI_CRU".
 */

#define RK3576_CCICRU_LPLL_CON(n) (0x0040 + ((n)*4))

/* LPLL CON register bit fields
 *
 * Bit 13 of CON1 is the PLL power-down (PWRDOWN) control, active high:
 *   1 = PLL powered down (not running), 0 = PLL running.
 * This matches the RK3588 PLL definition (RK3588_PLLCON1_PWRDOWN = BIT(13)).
 * See the FRACPLL set_rate sequence in rk3576_clk_tree.c.
 */
#define RK3576_LPLL_CON0_BYPASS  BIT(15)
#define RK3576_LPLL_CON0_M_SHIFT 0
#define RK3576_LPLL_CON0_M_MASK  0x3ff

#define RK3576_LPLL_CON1_PWRDOWN BIT(13)
#define RK3576_LPLL_CON1_S_SHIFT 6
#define RK3576_LPLL_CON1_S_MASK  0x7
#define RK3576_LPLL_CON1_P_SHIFT 0
#define RK3576_LPLL_CON1_P_MASK  0x3f

#define RK3576_LPLL_CON6_LOCK    BIT(15)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H */
