/****************************************************************************
 * chips/rk3576/hardware/rk3576_mipi_dcphy.h
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
 * RK3576 MIPI D/C-PHY combo PHY (DCPHY) hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM, Part 2, Chapter 21 "MIPI D/C-PHY Combo
 * PHY".
 *
 * The DCPHY is a D-PHY + C-PHY combo PHY shared between the DSI (display,
 * TX) and CSI (camera, RX) hosts.  Each register is at most 16 bits wide
 * and is accessed over an AMBA 3.0 APB interface.
 *
 * Register banks:
 *   - BIAS control (shared bias generation)
 *   - PLL control (M0, master TX PLL) + PLL status
 *   - Per-lane banks for the master clock lane (MC) and data lanes (MD0..3)
 *
 * Lane bank stride is 0x100; within a lane the offsets are:
 *   GNR_CON0/1 (general/timing), ANA_CON0..3 (analog), TIME_CON0..4
 *   (timing), DESKEW_CON0 (skew calibration), each separated per the TRM
 *   register summary.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MIPI_DCPHY_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MIPI_DCPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (relative to RK3576_DCPHY_ADDR = 0x2B020000). */

/* BIAS control. */

#define RK3576_DCPHY_BIAS_CON0 0x0000
#define RK3576_DCPHY_BIAS_CON1 0x0004
#define RK3576_DCPHY_BIAS_CON2 0x0008
#define RK3576_DCPHY_BIAS_CON4 0x0010

/* TX PLL control (M0 master). */

#define RK3576_DCPHY_PLL_CON0  0x0100
#define RK3576_DCPHY_PLL_CON1  0x0104
#define RK3576_DCPHY_PLL_CON2  0x0108
#define RK3576_DCPHY_PLL_CON3  0x010C
#define RK3576_DCPHY_PLL_CON4  0x0110
#define RK3576_DCPHY_PLL_CON5  0x0114
#define RK3576_DCPHY_PLL_CON6  0x0118
#define RK3576_DCPHY_PLL_CON7  0x011C
#define RK3576_DCPHY_PLL_CON8  0x0120
#define RK3576_DCPHY_PLL_STAT0 0x0140

/* Master TX clock lane (DPHY_MC) bank. */

#define RK3576_DCPHY_MC_GNR_CON0    0x0300
#define RK3576_DCPHY_MC_GNR_CON1    0x0304
#define RK3576_DCPHY_MC_ANA_CON0    0x0308
#define RK3576_DCPHY_MC_ANA_CON1    0x030C
#define RK3576_DCPHY_MC_ANA_CON2    0x0310
#define RK3576_DCPHY_MC_ANA_CON3    0x0314
#define RK3576_DCPHY_MC_TIME_CON0   0x0330
#define RK3576_DCPHY_MC_TIME_CON1   0x0334
#define RK3576_DCPHY_MC_TIME_CON2   0x0338
#define RK3576_DCPHY_MC_TIME_CON3   0x033C
#define RK3576_DCPHY_MC_TIME_CON4   0x0340
#define RK3576_DCPHY_MC_DESKEW_CON0 0x0350

/* Master TX data lane N (COMBO_MD0..3) banks; stride = 0x100. */

#define RK3576_DCPHY_MD_LANE_STRIDE 0x100
#define RK3576_DCPHY_MD0_GNR_CON0   0x0400
#define RK3576_DCPHY_MD0_GNR_CON1   0x0404
#define RK3576_DCPHY_MD0_ANA_CON0   0x0408
#define RK3576_DCPHY_MD0_ANA_CON1   0x040C
#define RK3576_DCPHY_MD0_ANA_CON2   0x0410
#define RK3576_DCPHY_MD0_TIME_CON0  0x0430
#define RK3576_DCPHY_MD0_TIME_CON1  0x0434
#define RK3576_DCPHY_MD0_TIME_CON2  0x0438
#define RK3576_DCPHY_MD0_TIME_CON3  0x043C
#define RK3576_DCPHY_MD0_TIME_CON4  0x0440

/* Helper macros to derive a data-lane register from lane index 0..3.  The
 * lane banks are contiguous (MD0 @ 0x0400, MD1 @ 0x0500, MD2 @ 0x0600,
 * MD3 @ 0x0700) with a constant intra-lane offset for each register type.
 */

#define RK3576_DCPHY_MD_GNR_CON0(n)  (0x0400 + ((n)*0x100) + 0x000)
#define RK3576_DCPHY_MD_GNR_CON1(n)  (0x0400 + ((n)*0x100) + 0x004)
#define RK3576_DCPHY_MD_ANA_CON0(n)  (0x0400 + ((n)*0x100) + 0x008)
#define RK3576_DCPHY_MD_ANA_CON1(n)  (0x0400 + ((n)*0x100) + 0x00C)
#define RK3576_DCPHY_MD_ANA_CON2(n)  (0x0400 + ((n)*0x100) + 0x010)
#define RK3576_DCPHY_MD_TIME_CON0(n) (0x0400 + ((n)*0x100) + 0x030)
#define RK3576_DCPHY_MD_TIME_CON1(n) (0x0400 + ((n)*0x100) + 0x034)
#define RK3576_DCPHY_MD_TIME_CON2(n) (0x0400 + ((n)*0x100) + 0x038)
#define RK3576_DCPHY_MD_TIME_CON3(n) (0x0400 + ((n)*0x100) + 0x03C)
#define RK3576_DCPHY_MD_TIME_CON4(n) (0x0400 + ((n)*0x100) + 0x040)

/* -----------------------------------------------------------------------
 * PLL_CON0 (0x0100)
 * -----------------------------------------------------------------------
 */

#define DCPHY_PLL_CON0_PLL_EN  (1u << 12)
#define DCPHY_PLL_CON0_S_SHIFT (8)
#define DCPHY_PLL_CON0_S_MASK  (0x7 << 8)
#define DCPHY_PLL_CON0_P_SHIFT (0)
#define DCPHY_PLL_CON0_P_MASK  (0x3f)

/* -----------------------------------------------------------------------
 * PLL_CON1 (0x0104) — 16-bit DSM K value
 * -----------------------------------------------------------------------
 */

#define DCPHY_PLL_CON1_K_SHIFT (0)
#define DCPHY_PLL_CON1_K_MASK  (0xffff)

/* -----------------------------------------------------------------------
 * PLL_CON2 (0x0108) — 10-bit main divider M at [9:0]
 * -----------------------------------------------------------------------
 */

#define DCPHY_PLL_CON2_M_SHIFT (0)
#define DCPHY_PLL_CON2_M_MASK  (0x3ff)

/* -----------------------------------------------------------------------
 * PLL_CON5 (0x0114) — PLL clock gating / reset source select.
 *
 *   RESET_N_SEL    [10]: 0 = release clock divider reset when PLL_EN is
 *                         set; 1 = release when ENABLE(GNR_CON0[0]) is set.
 *   PLL_ENABLE_SEL [8] : 0 = enable PLL buffering when PLL_EN is set;
 *                         1 = enable when ENABLE(GNR_CON0[0]) is set.
 *
 * The Rockchip reference driver (phy-rockchip-samsung-dcphy.c) always
 * programs RESET_N_SEL | PLL_ENABLE_SEL to tie PLL gate/reset to the
 * ENABLE phase; keep the same value for alignment.
 * -----------------------------------------------------------------------
 */

#define DCPHY_PLL_CON5_RESET_N_SEL    (1u << 10)
#define DCPHY_PLL_CON5_PLL_ENABLE_SEL (1u << 8)

/* -----------------------------------------------------------------------
 * PLL_CON7 (0x011C) — PLL_LOCK_CNT [15:0]
 *   TRM 21.6.3 step (4): the PLL lock counter counts up after PLL_EN is
 *   set and PLL_LOCK (PLL_STAT0[0]) is asserted when it reaches this
 *   value.  Roughly 200us is needed to lock; 0xf000 matches the Rockchip
 *   reference driver.
 * -----------------------------------------------------------------------
 */

#define DCPHY_PLL_CON7_LOCK_CNT_DEFAULT 0xf000u

/* -----------------------------------------------------------------------
 * PLL_CON8 (0x0120) — PLL_STB_CNT [15:0]
 *   PLL stabilization counter (frequency-hop / stabilization timing).
 *   0xf000 matches the Rockchip reference driver.
 * -----------------------------------------------------------------------
 */

#define DCPHY_PLL_CON8_STB_CNT_DEFAULT 0xf000u

/* -----------------------------------------------------------------------
 * PLL_STAT0 (0x0140)
 * -----------------------------------------------------------------------
 */

#define DCPHY_PLL_STAT0_PLL_LOCK (1u << 0)

/* -----------------------------------------------------------------------
 * Per-lane GNR_CON0 / GNR_CON1
 * -----------------------------------------------------------------------
 */

#define DCPHY_GNR_CON0_ENABLE    (1u << 0)
#define DCPHY_GNR_CON0_PHY_READY (1u << 1)

/* GNR_CON1 = T_PHY_READY: the PHY_READY handshake timeout, in cycles.
 * The reference driver (samsung_mipi_dphy_lane_enable) programs 0x2000 on
 * the clock lane and on every data lane; the reset value 0 is not a state
 * the handshake is specified for.
 */

#define RK3576_DCPHY_T_PHY_READY_DEFAULT 0x2000u

/* -----------------------------------------------------------------------
 * ANA_CON0 (MC_ANA_CON0 @ 0x0308, MD*_ANA_CON0 @ 0x0408/.../0x0708)
 * -----------------------------------------------------------------------
 *
 * Drive-strength / voltage-amplitude select for the HS transmitter:
 *
 *   [14:12] EDGE_CON      = 7
 *   [9]     EDGE_CON_DIR  = 0
 *   [8]     EDGE_CON_EN   = 1
 *   [7:4]   RES_UP        = driver-up resistor code
 *   [3:0]   RES_DN        = driver-down resistor code
 *
 * The resistor code is NOT the same for the clock and data lanes:
 * reference pdata for RK3576 (rk3576_dphy_hs_drv_res_cfg) uses 52 ohm
 * (code 0x3) for the clock lane but 39 ohm (code 0xe) for the data lanes.
 * The two encodings therefore differ:
 *
 *   clock lane: EDGE_CON(7)|EDGE_CON_EN|RES_UP(3)|RES_DN(3) = 0x7133
 *   data lane : EDGE_CON(7)|EDGE_CON_EN|RES_UP(e)|RES_DN(e) = 0x71ee
 *
 * Using the clock-lane value on the data lanes (52 ohm instead of 39 ohm)
 * mis-terminates every HS data output, which corrupts the HS eye at the
 * panel and can keep it from ever locking onto the pixel stream.
 */

#define RK3576_DCPHY_ANA_CON0_CLK_LANE  0x7133u /* 52 ohm up/down */
#define RK3576_DCPHY_ANA_CON0_DATA_LANE 0x71eeu /* 39 ohm up/down */

/* -----------------------------------------------------------------------
 * D-PHY TX timing registers (MC_TIME_CON* / MD*_TIME_CON*).
 *
 * The timing values depend on the lane high-speed data rate and are
 * looked up from a table indexed by lane Mbps (see the DCPHY driver).
 * Field encodings follow the MIPI D-PHY Supplement Guide and match the
 * Rockchip reference driver.
 * -----------------------------------------------------------------------
 */

/* MC_TIME_CON0: HSTX_CLK_SEL selects the serial-clock divider.  Use the
 * divide-by-2 clock when the data rate is under 1500 Mbps, otherwise the
 * divide-by-16 clock.  T_LPX is the LP transmit pulse width.
 */

#define DCPHY_MC_TIME_CON0_HSTX_CLK_SEL (1u << 12)
#define DCPHY_MC_TIME_CON0_T_LPX_SHIFT  (4)
#define DCPHY_MC_TIME_CON0_T_LPX_MASK   (0xff << 4)

/* MC_TIME_CON1: T_CLK_ZERO [15:8], T_CLK_PREPARE [7:0]. */

#define DCPHY_MC_TIME_CON1_T_CLK_ZERO_SHIFT    (8)
#define DCPHY_MC_TIME_CON1_T_CLK_ZERO_MASK     (0xff << 8)
#define DCPHY_MC_TIME_CON1_T_CLK_PREPARE_SHIFT (0)
#define DCPHY_MC_TIME_CON1_T_CLK_PREPARE_MASK  (0xff)

/* MC_TIME_CON2: T_HS_EXIT [15:8], T_CLK_TRAIL [7:0]. */

#define DCPHY_MC_TIME_CON2_T_HS_EXIT_SHIFT   (8)
#define DCPHY_MC_TIME_CON2_T_HS_EXIT_MASK    (0xff << 8)
#define DCPHY_MC_TIME_CON2_T_CLK_TRAIL_SHIFT (0)
#define DCPHY_MC_TIME_CON2_T_CLK_TRAIL_MASK  (0xff)

/* MC_TIME_CON3: T_CLK_POST [7:0]. */

#define DCPHY_MC_TIME_CON3_T_CLK_POST_SHIFT (0)
#define DCPHY_MC_TIME_CON3_T_CLK_POST_MASK  (0xff)

/* MD*_TIME_CON0: same HSTX_CLK_SEL / T_LPX layout as MC_TIME_CON0. */

#define DCPHY_MD_TIME_CON0_HSTX_CLK_SEL (1u << 12)
#define DCPHY_MD_TIME_CON0_T_LPX_SHIFT  (4)
#define DCPHY_MD_TIME_CON0_T_LPX_MASK   (0xff << 4)

/* MD*_TIME_CON1: T_HS_ZERO [15:8], T_HS_PREPARE [7:0]. */

#define DCPHY_MD_TIME_CON1_T_HS_ZERO_SHIFT    (8)
#define DCPHY_MD_TIME_CON1_T_HS_ZERO_MASK     (0xff << 8)
#define DCPHY_MD_TIME_CON1_T_HS_PREPARE_SHIFT (0)
#define DCPHY_MD_TIME_CON1_T_HS_PREPARE_MASK  (0xff)

/* MD*_TIME_CON2: T_HS_EXIT [15:8], T_HS_TRAIL [7:0]. */

#define DCPHY_MD_TIME_CON2_T_HS_EXIT_SHIFT  (8)
#define DCPHY_MD_TIME_CON2_T_HS_EXIT_MASK   (0xff << 8)
#define DCPHY_MD_TIME_CON2_T_HS_TRAIL_SHIFT (0)
#define DCPHY_MD_TIME_CON2_T_HS_TRAIL_MASK  (0xff)

/* MD*_TIME_CON3: T_TA_GET [7:4], T_TA_GO [3:0]. */

#define DCPHY_MD_TIME_CON3_T_TA_GET_SHIFT (4)
#define DCPHY_MD_TIME_CON3_T_TA_GET_MASK  (0xf << 4)
#define DCPHY_MD_TIME_CON3_T_TA_GO_SHIFT  (0)
#define DCPHY_MD_TIME_CON3_T_TA_GO_MASK   (0xf)

/* Escape clock divisor: 0x1f4 programs a 20 MHz escape clock. */

#define DCPHY_TIME_CON4_ESC_CLK_DIV 0x1f4

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MIPI_DCPHY_H */
