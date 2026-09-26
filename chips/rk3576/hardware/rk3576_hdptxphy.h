/****************************************************************************
 * chips/rk3576/hardware/rk3576_hdptxphy.h
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
 * RK3576 HDMI/eDP Combo PHY (HDPTX PHY) hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM Part 2, Chapter 25 "HDMI TX/eDP Combo PHY"
 * (register summary 25.4.2, detail 25.4.3, SFR settings 25.6.1) and Part 1
 * 5.22 "HDPTXPHY_GRF Register Description".
 *
 * ---------------------------------------------------------------------------
 * Register addressing
 * ---------------------------------------------------------------------------
 * The PHY exposes four SFR banks at one APB base (RK3576_HDPTXPHY_ADDR):
 *
 *   bank        TRM name prefix          index range   byte offsets
 *   ---------   ----------------------   -----------   --------------
 *   CMN         HDPTXPHY_CMN_REGnnnn     0000..00a7    0x0000..0x029c
 *   SB          HDPTXPHY_SB_REGnnnn      0100..0129    0x0400..0x04a4
 *   LNTOP       HDPTXPHY_INTOP_REGnnnn   0200..0229    0x0800..0x08a4
 *   LANE        HDPTXPHY_LANE_REG0nnn    0300..062d    0x0c00..0x18b4
 *
 * The TRM's four-hex-digit index is NOT the byte offset: the byte offset is
 * four times the index (CMN_REG0009 -> 0x24).  The RK3576_HDPTXPHY_CMN() /
 * _SB() / _LNTOP() / _LANE() macros below apply that scaling, so a table
 * entry written as RK3576_HDPTXPHY_CMN(0x51) reads as 0x144 and corresponds
 * one-to-one with the TRM's HDPTXPHY_CMN_REG0051.  Keeping the index rather
 * than the offset in the source makes every entry directly checkable against
 * the TRM.
 *
 * LANE is four identical per-lane banks, 0x400 bytes apart:
 *
 *   lane 0 = LANE_REG(0300..032d)   lane 1 = LANE_REG(0400..042d)
 *   lane 2 = LANE_REG(0500..052d)   lane 3 = LANE_REG(0600..062d)
 *
 * so lane N's copy of lane-0 register 03xx is LANE_REG(03xx + 0x100 * N).
 * All four banks are 32-bit registers on 32-bit boundaries.
 *
 * ---------------------------------------------------------------------------
 * Write masking
 * ---------------------------------------------------------------------------
 * HDPTXPHY_GRF (a separate block, RK3576_HDPTXPHY_GRF_ADDR) uses the usual
 * Rockchip hiword write mask: bit 16+N gates bit N.  Every GRF field this
 * driver touches is a single bit, so RK3576_HDPTXPHY_GRF_WM16() takes a bit
 * number rather than a mask.
 *
 * The PHY SFR banks themselves have NO write mask -- a plain 32-bit store
 * updates every bit -- which is why the driver only ever writes whole words
 * to them.
 *
 * ---------------------------------------------------------------------------
 * Field naming
 * ---------------------------------------------------------------------------
 * Field names below keep the TRM spelling (ana_ropll_pms_pdiv, and so on) so
 * that a value can be checked against the TRM without translation.  TRM Part 1
 * names the combo PHY "HDPTXPHY".
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_HDPTXPHY_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_HDPTXPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Register addressing
 ****************************************************************************/

/* Zero-based bit-field helpers.  GENMASK(h, l) yields the inclusive bit
 * range; the hiword writer sets the GRF write-enable bit and the value bit
 * in a single 32-bit store.
 */

#define RK3576_HDPTXPHY_GENMASK(h, l) \
  ((((uint32_t)1u << ((h) - (l) + 1u)) - 1u) << (l))

/* Byte offset of SFR index n within each bank. */

#define RK3576_HDPTXPHY_REG(n)   ((uintptr_t)((n)*4u))

#define RK3576_HDPTXPHY_CMN(n)   RK3576_HDPTXPHY_REG(n) /* CMN   bank */
#define RK3576_HDPTXPHY_SB(n)    RK3576_HDPTXPHY_REG(n) /* SB    bank */
#define RK3576_HDPTXPHY_LNTOP(n) RK3576_HDPTXPHY_REG(n) /* LNTOP bank */
#define RK3576_HDPTXPHY_LANE(n)  RK3576_HDPTXPHY_REG(n) /* LANE  bank */

/* Byte stride between the four LANE banks.  Adding this to a LANE_REG(03xx)
 * index yields the same register in lane 1/2/3.
 */

#define RK3576_HDPTXPHY_LANE_STRIDE 0x400u

/* GRF hiword write mask: bit 16+N enables writing bit N. */

#define RK3576_HDPTXPHY_GRF_WM16(bit, val) \
  (((uint32_t)1u << ((bit) + 16u)) | (((val)&1u) << (bit)))

/****************************************************************************
 * HDPTXPHY_GRF (RK3576_HDPTXPHY_GRF_ADDR)
 ****************************************************************************/

/* Control register 0.  Every field here is a single bit. */

#define RK3576_HDPTXPHY_GRF_CON0_OFF       0x00

#define RK3576_HDPTXPHY_GRF_LC_REF_CLK_SEL 11 /* ROPLL ref: 0 = crystal */
#define RK3576_HDPTXPHY_GRF_PLL_EN         7  /* PLL block enable      */
#define RK3576_HDPTXPHY_GRF_BIAS_EN        6  /* BIAS block enable     */
#define RK3576_HDPTXPHY_GRF_BGR_EN         5  /* band-gap ref enable   */
#define RK3576_HDPTXPHY_GRF_MODE_SEL       0  /* 0 = HDMI, 1 = DP      */

/* Status register: the PHY's ready handshakes. */

#define RK3576_HDPTXPHY_GRF_STATUS0_OFF   0x80

#define RK3576_HDPTXPHY_GRF_PLL_LOCK_DONE (1u << 3) /* PLL locked, clks OK */
#define RK3576_HDPTXPHY_GRF_PHY_CLK_RDY   (1u << 2) /* datapath clks OK   */
#define RK3576_HDPTXPHY_GRF_PHY_RDY       (1u << 1) /* lanes ready for TX */
#define RK3576_HDPTXPHY_GRF_SB_RDY        (1u << 0) /* sideband ready     */

/* Power-up handshake timeouts.  The values are generous multiples of the times
 * the TRM's start-up sequence asks for (25.6.2.1: 10 us between each step).
 */

#define RK3576_HDPTXPHY_PLL_LOCK_TRIES 400u  /* x 20 us  = 8 ms   */
#define RK3576_HDPTXPHY_PLL_LOCK_DELAY 20u   /* us                */
#define RK3576_HDPTXPHY_LANE_RDY_TRIES 5000u /* x 100 us = 500 ms */
#define RK3576_HDPTXPHY_LANE_RDY_DELAY 100u  /* us                */

/****************************************************************************
 * CMN bank -- common block: reference clock, PLLs, clock generation
 ****************************************************************************/

/* CMN_REG(0059): ROPLL pre-divider and reference divider. */

#define RK3576_HDPTXPHY_CMN0059_OFF RK3576_HDPTXPHY_CMN(0x59)
#define RK3576_HDPTXPHY_CMN0059_PDIV_MASK \
  RK3576_HDPTXPHY_GENMASK(7, 4) /* ana_ropll_pms_pdiv   */
#define RK3576_HDPTXPHY_CMN0059_REFDIV_MASK \
  RK3576_HDPTXPHY_GENMASK(3, 0) /* ana_ropll_pms_refdiv */

/* CMN_REG(005a): ROPLL post divider, one field per link-rate bank.  Only the
 * RBR field is used for TMDS (see CMN_REG(0059)/dp_tx_link_bw note below).
 */

#define RK3576_HDPTXPHY_CMN005A_OFF RK3576_HDPTXPHY_CMN(0x5a)
#define RK3576_HDPTXPHY_CMN005A_SDIV_RBR_MASK \
  RK3576_HDPTXPHY_GENMASK(7, 4) /* ropll_pms_sdiv_rbr */
#define RK3576_HDPTXPHY_CMN005A_SDIV_HBR_MASK \
  RK3576_HDPTXPHY_GENMASK(3, 0) /* ropll_pms_sdiv_hbr */

/* CMN_REG(005e): SDM (sigma-delta) fractional-divider enable and resets. */

#define RK3576_HDPTXPHY_CMN005E_OFF             RK3576_HDPTXPHY_CMN(0x5e)
#define RK3576_HDPTXPHY_CMN005E_SDM_EN          (1u << 6)
#define RK3576_HDPTXPHY_CMN005E_RSVD_LOW_NIBBLE 0x0fu

/* CMN_REG(0064): sign of the SDM numerator, per link-rate bank. */

#define RK3576_HDPTXPHY_CMN0064_OFF          RK3576_HDPTXPHY_CMN(0x64)
#define RK3576_HDPTXPHY_CMN0064_NUM_SIGN_RBR (1u << 3)

/* CMN_REG(0069): SDC (clock generation) divide-ratio selection. */

#define RK3576_HDPTXPHY_CMN0069_OFF RK3576_HDPTXPHY_CMN(0x69)
#define RK3576_HDPTXPHY_CMN0069_SDC_N_MASK \
  RK3576_HDPTXPHY_GENMASK(2, 0) /* ropll_sdc_n_rbr */

/* CMN_REG(0086): pixel clock generator.  clk_sel selects the divide ratio
 * between the PLL and the pixel clock; the driver derives it from the colour
 * depth as (bpc - 8) / 2.
 */

#define RK3576_HDPTXPHY_CMN0086_OFF RK3576_HDPTXPHY_CMN(0x86)
#define RK3576_HDPTXPHY_CMN0086_POSTDIV_MASK \
  RK3576_HDPTXPHY_GENMASK(7, 4) /* pll_pcg_postdiv_sel */
#define RK3576_HDPTXPHY_CMN0086_CLK_SEL_MASK \
  RK3576_HDPTXPHY_GENMASK(3, 1)                  /* pll_pcg_clk_sel   */
#define RK3576_HDPTXPHY_CMN0086_CLK_EN (1u << 0) /* pll_pcg_clk_en    */

/****************************************************************************
 * LNTOP bank -- lane top: protocol selection, data width, lane enables
 ****************************************************************************/

/* LNTOP_REG(0200): protocol and TMDS/FRL selection.  For TMDS the whole
 * register is written as 0x06, i.e. protocol_sel = 1
 * (HDMI, not DP) with the TMDS/FRL select bit clear.
 */

#define RK3576_HDPTXPHY_LNTOP0200_OFF          RK3576_HDPTXPHY_LNTOP(0x200)
#define RK3576_HDPTXPHY_LNTOP0200_PROTOCOL_SEL (1u << 2) /* 1 = HDMI  */
#define RK3576_HDPTXPHY_LNTOP0200_TMDS_FRL_SEL (1u << 1) /* 1 = TMDS  */
#define RK3576_HDPTXPHY_LNTOP0200_TMDS_VALUE   0x06u

/* LNTOP_REG(0206): serialiser data-bus width.  TMDS uses 40 bits. */

#define RK3576_HDPTXPHY_LNTOP0206_OFF RK3576_HDPTXPHY_LNTOP(0x206)
#define RK3576_HDPTXPHY_LNTOP0206_BUS_WIDTH_MASK \
  RK3576_HDPTXPHY_GENMASK(2, 1) /* data_bus_width     */
#define RK3576_HDPTXPHY_LNTOP0206_BUS_WIDTH_SEL \
  (1u << 0)                                        /* bus_width_sel      */
#define RK3576_HDPTXPHY_LNTOP0206_TMDS_VALUE 0x07u /* 40-bit, sel = 1    */

/* LNTOP_REG(0207): per-lane enable.  One bit per serialiser lane; HDMI TMDS
 * enables all four (three data + one clock).
 */

#define RK3576_HDPTXPHY_LNTOP0207_OFF       RK3576_HDPTXPHY_LNTOP(0x207)
#define RK3576_HDPTXPHY_LNTOP0207_LANE_MASK 0x0fu
#define RK3576_HDPTXPHY_LNTOP0207_ALL_LANES 0x0fu

/****************************************************************************
 * Resets (PMU1CRU)
 ****************************************************************************/

/* The combo PHY lives in the PMU1CRU reset domain.  Two registers matter:
 *
 *   PMU1CRU_SOFTRST_CON(0): [1] presetn_hdptx_apb, [0] presetn_hdptx_grf
 *   PMU1CRU_SOFTRST_CON(1): [9] resetn_hdptx_init
 *                           [10] resetn_hdptx_cmn
 *                           [11] resetn_hdptx_lane
 *
 * All are the standard CRU polarity: 1 asserts reset, 0 releases it.  The
 * TRM's start-up sequence (25.6.2.1) requires init -> cmn -> lane to be
 * released in that order, waiting for the PLL to lock between cmn and lane.
 *
 * The PHY's link-symbol clock reset lives in the main CRU instead
 * (CRU_SOFTRST_CON(75)[1], resetn_linksym_hdmitxphy0); it is released by
 * rk3576_hdptxphy.c because the reset-controller framework has no CRU
 * provider, so drivers pulse CRU reset lines directly (the same scheme
 * rk3576_sai.c and rk3576_saradc.c use).
 */

#define RK3576_HDPTXPHY_PMU1RST_APB_CON     0
#define RK3576_HDPTXPHY_PMU1RST_APB_BIT     1
#define RK3576_HDPTXPHY_PMU1RST_GRF_BIT     0

#define RK3576_HDPTXPHY_PMU1RST_SEQ_CON     1
#define RK3576_HDPTXPHY_PMU1RST_INIT_BIT    9
#define RK3576_HDPTXPHY_PMU1RST_CMN_BIT     10
#define RK3576_HDPTXPHY_PMU1RST_LANE_BIT    11

#define RK3576_HDPTXPHY_CRU_LINKSYM_RST_CON 75
#define RK3576_HDPTXPHY_CRU_LINKSYM_RST_BIT 1

/****************************************************************************
 * Operating constraints
 ****************************************************************************/

/* PLL reference is the 24 MHz crystal: HDPTXPHY_GRF_CON0.lc_ref_clk_sel
 * (bit 8) and .ro_ref_clk_sel (bits 11:10) both reset to the crystal, and the
 * TMDS path programs lc_ref_clk_sel = 0 explicitly.
 */

#define RK3576_HDPTXPHY_REF_CLK_HZ 24000000u

/* TMDS character-rate ceilings.  CE = the TMDS character rate, which equals
 * pixel_clock * bpc / 8 and therefore equals the pixel clock at 8 bpc.
 * 340 MHz is the HDMI 1.4b limit and also the point above which the
 * serialiser switches to a 1/40 bit-rate clock; 600 MHz is the
 * HDMI 2.0 (TMDS) limit.
 */

#define RK3576_HDPTXPHY_TMDS_HDMI14_MAX_HZ 340000000u
#define RK3576_HDPTXPHY_TMDS_HDMI20_MAX_HZ 600000000u

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_HDPTXPHY_H */
