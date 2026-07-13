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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CRU register-bank offset formulas (verified against RK3576 TRM Part1 V1.2):
 *   CLKSEL_CON(n)  = CRU + 0x0300 + n*4   (clock mux / divider)
 *   GATE_CON(n)    = CRU + 0x0800 + n*4   (clock gating)
 *   SOFTRST_CON(n) = CRU + 0x0A00 + n*4   (soft reset)
 * Cross-checked: CLKSEL_CON105@0x04A4 / GATE_CON42@0x08A8 / SOFTRST_CON42@0x0AA8.
 * All registers use the Rockchip 16-bit write-mask scheme: write
 * (mask << 16) | (val & mask); the upper 16 bits are the per-bit write enable,
 * so only bits set in the mask are actually written.
 */

#define RK3576_CRU_CLKSEL_CON(n)   (RK3576_CRU_ADDR + 0x0300 + ((n) * 4))
#define RK3576_CRU_GATE_CON(n)     (RK3576_CRU_ADDR + 0x0800 + ((n) * 4))
#define RK3576_CRU_SOFTRST_CON(n)  (RK3576_CRU_ADDR + 0x0A00 + ((n) * 4))

/* SDIO controller clocks (cclk_src_sdio / hclk_sdio).
 * Source: RK3576 TRM Part1 cross-checked with the Linux mainline
 * drivers/clk/rockchip/clk-rk3576.c:
 *   COMPOSITE(CCLK_SRC_SDIO, gpll_cpll_24m_p,
 *             CLKSEL_CON(104), sel[7:6], div[5:0], GATE_CON(42) bit11)
 *   GATE(HCLK_SDIO,          GATE_CON(42) bit12)
 *   hresetn_sdio =           SOFTRST_CON(42) bit12
 */

#define RK3576_CRU_SDIO_CLKSEL     104
#define RK3576_CRU_SDIO_SEL_SHIFT  6      /* cclk_src_sdio_sel[7:6] */
#define RK3576_CRU_SDIO_SEL_MASK   (0x3 << RK3576_CRU_SDIO_SEL_SHIFT)
#define RK3576_CRU_SDIO_SEL_GPLL   0      /* parent0 = gpll */
#define RK3576_CRU_SDIO_SEL_CPLL   1      /* parent1 = cpll */
#define RK3576_CRU_SDIO_SEL_24M    2      /* parent2 = xin24m */
#define RK3576_CRU_SDIO_DIV_SHIFT  0      /* cclk_src_sdio_div[5:0] */
#define RK3576_CRU_SDIO_DIV_MASK   (0x3f << RK3576_CRU_SDIO_DIV_SHIFT)

#define RK3576_CRU_SDIO_GATE       42
#define RK3576_CRU_SDIO_HCLK_BIT   (1 << 12)   /* hclk_sdio_en */
#define RK3576_CRU_SDIO_CCLK_BIT   (1 << 11)   /* cclk_src_sdio_en */

#define RK3576_CRU_SDIO_SOFTRST    42
#define RK3576_CRU_SDIO_RST_BIT    (1 << 12)   /* hresetn_sdio */

/* gpll frequency (verified from the DTS assigned-clock-rates value
 * 0x46CF7100 = 1188 MHz).  cclk_src_sdio = gpll / (div_con + 1).  We pick
 * div_con=23 -> 1188/24 = 49.5 MHz, close to the CLKIN=50 MHz constant reused
 * from rk3576_sdmmc.c, and low enough for the DW-MSHC 8-bit CLKDIV to reach
 * the 400 kHz card-identification clock (the reset default gpll/3 = 396 MHz is
 * above that limit, so the divider must be reprogrammed).
 */

#define RK3576_CRU_GPLL_FREQ       1188000000u
#define RK3576_CRU_SDIO_DIV_CON    23
#define RK3576_CRU_SDIO_CCLK_FREQ  (RK3576_CRU_GPLL_FREQ / (RK3576_CRU_SDIO_DIV_CON + 1))

/* I2C2 controller clocks (pclk_i2c2 / clk_i2c2).  The hym8563 RTC sits on
 * this bus and its 32.768 kHz CLKOUT is the RTL8822CS Wi-Fi sleep clock, so
 * the bus must be clocked before it can be programmed.  Source: RK3576 TRM
 * Part1 cross-checked with Linux mainline drivers/clk/rockchip/clk-rk3576.c:
 *   GATE(PCLK_I2C2,            GATE_CON(12) bit1)
 *   COMPOSITE_NODIV(CLK_I2C2,  mux[200/100/50/24 MHz] CLKSEL_CON(57) sel[3:2],
 *                              GATE_CON(12) bit13)
 */

#define RK3576_CRU_I2C2_GATE       12
#define RK3576_CRU_I2C2_PCLK_BIT   (1 << 1)    /* pclk_i2c2_en  */
#define RK3576_CRU_I2C2_CCLK_BIT   (1 << 13)   /* clk_i2c2_en   */

#define RK3576_CRU_I2C2_CLKSEL     57
#define RK3576_CRU_I2C2_SEL_SHIFT  2           /* clk_i2c2_sel[3:2] */
#define RK3576_CRU_I2C2_SEL_MASK   (0x3 << RK3576_CRU_I2C2_SEL_SHIFT)
#define RK3576_CRU_I2C2_SEL_100M   1           /* parent1 = 100 MHz */
#define RK3576_CRU_I2C2_SEL_24M    3           /* parent3 = xin24m (always on) */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H */
