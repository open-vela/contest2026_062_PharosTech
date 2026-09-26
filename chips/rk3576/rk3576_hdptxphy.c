/****************************************************************************
 * chips/rk3576/rk3576_hdptxphy.c
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
 * RK3576 HDMI/eDP combo PHY (HDPTX PHY) driver -- HDMI TMDS mode.
 *
 * Implements the PHY start-up sequence and the TMDS PLL programming described
 * by TRM Part 2, sections 25.6.1 ("SFR Settings for HDMI mode") and 25.6.2.1
 * ("Start-Up Sequence for PHY").
 *
 * ---------------------------------------------------------------------------
 * Why the PLL is table-driven
 * ---------------------------------------------------------------------------
 * The TMDS PLL (ROPLL) is an integer divider plus a sigma-delta fractional
 * stage:
 *
 *   fout = fref * mdiv                                       (integer part)
 *   fout += fref * sdc_deno * mdiv * sdm_num
 *           / (16 * sdm_deno * (sdc_deno * sdc_n - sdc_num)) (fraction)
 *   pixclk = fout * 2 * 8 / (sdiv * 10 * bpc)
 *
 * (Note the literal `2 * 8 / (sdiv * 10 * bpc)` in the divisor: it encodes the
 * PLL's fixed output scaling together with the serialiser ratio, and is why
 * pixclk depends on the colour depth.)
 *
 * Not every pixel clock is expressible, the VCO must stay inside 2..4 GHz, and
 * the integer divider must be in [20, 255]; so instead of computing a setting
 * at runtime this driver matches the requested TMDS character rate against a
 * table of validated settings, and refuses the mode if there is no exact
 * match.
 *
 * The 1920x1080p60 entry is worth spelling out, because it is the bring-up
 * target and because a wrong fractional setting fails in a way that looks like
 * a dead board rather than a wrong clock:
 *
 *   rate = 148500000 (TMDS character rate = 148.5 MHz pixel clock at 8 bpc)
 *   mdiv = 123, sdiv = 3 (post divider 4), sdm_en = 1, sign = positive
 *   sdm_deno = 4, sdm_num = 3, sdc_n = 5, sdc_num = 5, sdc_deno = 16
 *
 *   fout  = 24 MHz * 123 = 2952 MHz
 *   denom = 16 * 4 * (16 * (5 + 3) - 5) = 64 * 123 = 7872
 *   sdm   = 24 MHz * 16 * 123 * 3 / 7872 = 18 MHz
 *   fout  = 2952 + 18 = 2970 MHz            (inside the 2..4 GHz window)
 *   pixclk = 2970 MHz * 16 / (4 * 10 * 8) = 148.5 MHz   -- exact
 *
 * The useful property of sampling a 24 MHz reference through a sigma-delta
 * stage is that rates like 148.5 MHz (6.1875 x 24 MHz) land exactly rather
 * than on the nearest integer multiple.
 *
 * ---------------------------------------------------------------------------
 * Ready handshakes
 * ---------------------------------------------------------------------------
 * Bringing the PHY up is a three-stage handshake against HDPTXPHY_GRF_STATUS0,
 * and each stage must be awaited before the next:
 *
 *   release init reset  -> enable PLL       -> wait o_phy_clk_rdy
 *   release cmn reset                         (datapath clocks stable)
 *   release lane reset  -> enable lanes     -> wait o_phy_rdy &&
 *o_pll_lock_done
 *
 * A caller that skips the first wait and starts the HDMI controller anyway
 * gets a PHY whose serialisers are still coming up; the controller then
 * reports a plausible-looking video FIFO error, which points at the
 * controller rather than at the PHY.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>
#include <sys/param.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_hdptxphy.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_hdptxphy.h"

#ifdef CONFIG_RK3576_HDPTXPHY

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Delays from the TRM start-up sequence (25.6.2.1).  It asks for 10 us after
 * each enable step and 20..30 us around the APB reset pulse.
 */

#define RK3576_HDPTXPHY_APB_RESET_US 25
#define RK3576_HDPTXPHY_STEP_US      15
#define RK3576_HDPTXPHY_SETTLE_US    10

/* Number of serialiser lanes (three TMDS data + one TMDS clock). */

#define RK3576_HDPTXPHY_NLANES 4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One register/value pair of a PHY SFR programming sequence. */

struct rk3576_hdptxphy_seq_s
{
  uint32_t offset;
  uint32_t value;
};

/* ROPLL setting for one TMDS character rate.  Field order follows the TRM
 * register summary so values can be compared directly.
 */

struct rk3576_hdptxphy_ropll_s
{
  uint32_t rate;        /* TMDS character rate, Hz */
  uint8_t mdiv;         /* ropll_pms_mdiv                */
  uint8_t mdiv_afc;     /* ropll_pms_mdiv_afc (AFC copy) */
  uint8_t pdiv;         /* ana_ropll_pms_pdiv            */
  uint8_t refdiv;       /* ana_ropll_pms_refdiv          */
  uint8_t sdiv;         /* ropll_pms_sdiv (post divider - 1) */
  uint8_t sdm_en;       /* sigma-delta stage enable      */
  uint8_t sdm_deno;     /* sigma-delta denominator       */
  uint8_t sdm_num_sign; /* 0 = positive, 1 = negative    */
  uint8_t sdm_num;      /* sigma-delta numerator         */
  uint8_t sdc_n;        /* SDC divide ratio, stored as n-3 */
  uint8_t sdc_num;      /* SDC numerator                 */
  uint8_t sdc_deno;     /* SDC denominator               */
};

struct rk3576_hdptxphy_s
{
  mutex_t lock;

  uintptr_t base; /* PHY SFR bank        (RK3576_HDPTXPHY_ADDR)     */
  uintptr_t grf;  /* PHY control/status  (RK3576_HDPTXPHY_GRF_ADDR) */
  uintptr_t pmu1; /* PMU1CRU             (RK3576_PMU1_CRU_ADDR)     */
  uintptr_t cru;  /* main CRU            (RK3576_CRU_ADDR)          */

  struct clk_s *pclk_apb; /* pclk_hdptx_apb */
  struct clk_s *pclk_grf; /* pclk_hdptx_grf */

  uint32_t pixel_clock; /* Pixel clock programmed by power_on(), Hz */
  uint8_t bpc;          /* Colour depth power_on() was called with  */

  bool initialized; /* Clocks and APB/GRF resets released */
  bool powered;     /* PLL, lanes and clock generation enabled */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_hdptxphy_s g_hdptxphy = {
  .lock = NXMUTEX_INITIALIZER,
};

/* TMDS ROPLL settings, ordered by descending TMDS character rate.  The rate
 * key is the
 * TMDS character rate (pixel clock at 8 bpc), not the pixel clock at any other
 * depth; the caller's bpc is folded in before the lookup.
 */

static const struct rk3576_hdptxphy_ropll_s g_hdptxphy_tmds_ropll[] = {
  /*     rate,     mdiv, mdafc, pdiv, rdiv, sdiv, en, deno, nsig, num,  n,
   *   num, deno
   */

  { 594000000u, 124, 124, 1, 1, 0, 1, 62, 1, 16, 5, 0,
    1 }, /* 3840x2160p30 / 4096x2160p30 */
  { 461101250u, 97, 97, 1, 1, 0, 1, 71, 1, 53, 2, 6, 35 },
  { 371250000u, 155, 155, 1, 1, 1, 1, 62, 1, 16, 5, 0,
    1 }, /* 1920x1080p120 / 2560x1440p60 */
  { 297000000u, 124, 124, 1, 1, 1, 1, 62, 1, 16, 5, 0,
    1 }, /* 3840x2160p30 / 1920x1080p60 (12 bpc) */
  { 185625000u, 155, 155, 1, 1, 3, 1, 62, 1, 16, 5, 0, 1 },
  { 162000000u, 135, 135, 1, 1, 3, 0, 4, 0, 3, 5, 5, 16 },
  { 154000000u, 193, 193, 1, 1, 5, 1, 193, 1, 32, 2, 1, 1 },
  { 148500000u, 123, 123, 1, 1, 3, 1, 4, 0, 3, 5, 5,
    16 }, /* 1920x1080p60  (bring-up target)     */
  { 146250000u, 122, 122, 1, 1, 3, 1, 244, 1, 16, 2, 1, 1 },
  { 119000000u, 149, 149, 1, 1, 5, 1, 149, 1, 16, 2, 1, 1 },
  { 108000000u, 135, 135, 1, 1, 5, 0, 9, 0, 5, 0, 20, 24 },
  { 106500000u, 89, 89, 1, 1, 3, 1, 89, 1, 16, 1, 0, 1 },
  { 92812500u, 155, 155, 1, 1, 7, 1, 62, 1, 16, 5, 0, 1 },
  { 85500000u, 214, 214, 1, 1, 11, 1, 214, 1, 16, 2, 1, 1 },
  { 83500000u, 105, 105, 1, 1, 5, 1, 42, 1, 16, 1, 0, 1 },
  { 74250000u, 124, 124, 1, 1, 7, 1, 62, 1, 16, 5, 0,
    1 }, /* 1920x1080p50 / 1280x720p60      */
  { 65000000u, 162, 162, 1, 1, 11, 1, 54, 0, 16, 4, 1, 1 },
  { 50250000u, 84, 84, 1, 1, 7, 1, 11, 1, 4, 5, 4, 11 },
  { 40000000u, 100, 100, 1, 1, 11, 0, 9, 0, 5, 0, 20, 24 },
  { 33750000u, 112, 112, 1, 1, 15, 1, 2, 0, 1, 5, 1, 1 },
  { 27000000u, 90, 90, 1, 1, 15, 0, 9, 0, 5, 0, 20, 24 },
  { 25175000u, 84, 84, 1, 1, 15, 1, 168, 1, 16, 4, 1, 1 }, /* 640x480p60 */
};

/* Common CMN block programming, applied before the per-mode CMN table.  These
 * are analogue bias/band-gap/PLL-helper settings; the TRM does not document
 * them field by field, so they are kept as-is.
 */

static const struct rk3576_hdptxphy_seq_s g_hdptxphy_common_cmn_seq[] = {
  { RK3576_HDPTXPHY_CMN(0x09), 0x0c }, { RK3576_HDPTXPHY_CMN(0x0a), 0x83 },
  { RK3576_HDPTXPHY_CMN(0x0b), 0x06 }, { RK3576_HDPTXPHY_CMN(0x0c), 0x20 },
  { RK3576_HDPTXPHY_CMN(0x0d), 0xb8 }, { RK3576_HDPTXPHY_CMN(0x0e), 0x0f },
  { RK3576_HDPTXPHY_CMN(0x0f), 0x0f }, { RK3576_HDPTXPHY_CMN(0x10), 0x04 },
  { RK3576_HDPTXPHY_CMN(0x12), 0x26 }, { RK3576_HDPTXPHY_CMN(0x13), 0x22 },
  { RK3576_HDPTXPHY_CMN(0x14), 0x24 }, { RK3576_HDPTXPHY_CMN(0x15), 0x77 },
  { RK3576_HDPTXPHY_CMN(0x16), 0x08 }, { RK3576_HDPTXPHY_CMN(0x18), 0x04 },
  { RK3576_HDPTXPHY_CMN(0x19), 0x48 }, { RK3576_HDPTXPHY_CMN(0x1a), 0x01 },
  { RK3576_HDPTXPHY_CMN(0x1b), 0x00 }, { RK3576_HDPTXPHY_CMN(0x1c), 0x01 },
  { RK3576_HDPTXPHY_CMN(0x1d), 0x64 }, { RK3576_HDPTXPHY_CMN(0x1f), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x29), 0x01 }, { RK3576_HDPTXPHY_CMN(0x35), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x38), 0x00 }, { RK3576_HDPTXPHY_CMN(0x39), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x3a), 0x00 }, { RK3576_HDPTXPHY_CMN(0x3b), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x3c), 0x80 }, { RK3576_HDPTXPHY_CMN(0x3e), 0x0c },
  { RK3576_HDPTXPHY_CMN(0x3f), 0x83 }, { RK3576_HDPTXPHY_CMN(0x40), 0x06 },
  { RK3576_HDPTXPHY_CMN(0x41), 0x20 }, { RK3576_HDPTXPHY_CMN(0x43), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x44), 0x46 }, { RK3576_HDPTXPHY_CMN(0x45), 0x24 },
  { RK3576_HDPTXPHY_CMN(0x47), 0x00 }, { RK3576_HDPTXPHY_CMN(0x49), 0xfa },
  { RK3576_HDPTXPHY_CMN(0x4a), 0x08 }, { RK3576_HDPTXPHY_CMN(0x4b), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x4c), 0x01 }, { RK3576_HDPTXPHY_CMN(0x4d), 0x64 },
  { RK3576_HDPTXPHY_CMN(0x4f), 0x00 }, { RK3576_HDPTXPHY_CMN(0x50), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x5f), 0x01 }, { RK3576_HDPTXPHY_CMN(0x75), 0x20 },
  { RK3576_HDPTXPHY_CMN(0x76), 0x30 }, { RK3576_HDPTXPHY_CMN(0x77), 0x08 },
  { RK3576_HDPTXPHY_CMN(0x78), 0x0c }, { RK3576_HDPTXPHY_CMN(0x79), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x7b), 0x00 }, { RK3576_HDPTXPHY_CMN(0x7c), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x7d), 0x00 }, { RK3576_HDPTXPHY_CMN(0x7e), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x7f), 0x00 }, { RK3576_HDPTXPHY_CMN(0x80), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x82), 0x04 }, { RK3576_HDPTXPHY_CMN(0x83), 0x24 },
  { RK3576_HDPTXPHY_CMN(0x84), 0x20 }, { RK3576_HDPTXPHY_CMN(0x85), 0x03 },
  { RK3576_HDPTXPHY_CMN(0x8a), 0x55 }, { RK3576_HDPTXPHY_CMN(0x8b), 0x25 },
  { RK3576_HDPTXPHY_CMN(0x8c), 0x2c }, { RK3576_HDPTXPHY_CMN(0x8d), 0x22 },
  { RK3576_HDPTXPHY_CMN(0x8e), 0x14 }, { RK3576_HDPTXPHY_CMN(0x8f), 0x20 },
  { RK3576_HDPTXPHY_CMN(0x90), 0x00 }, { RK3576_HDPTXPHY_CMN(0x91), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x92), 0x00 }, { RK3576_HDPTXPHY_CMN(0x93), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x9a), 0x11 },
};

/* TMDS-specific CMN programming: selects the ROPLL (rather than the LC PLL)
 * as the clock source and sets the analogue blocks for TMDS rates.  This is
 * the table whose register 0086/0097/0099 values encode "ROPLL alone mode,
 * TMDS, divide-by-10 clock", matching TRM Table 25-3's dig_clk_sel = 1.
 */

static const struct rk3576_hdptxphy_seq_s g_hdptxphy_tmds_cmn_seq[] = {
  { RK3576_HDPTXPHY_CMN(0x08), 0x00 }, { RK3576_HDPTXPHY_CMN(0x11), 0x01 },
  { RK3576_HDPTXPHY_CMN(0x17), 0x20 }, { RK3576_HDPTXPHY_CMN(0x1e), 0x14 },
  { RK3576_HDPTXPHY_CMN(0x20), 0x00 }, { RK3576_HDPTXPHY_CMN(0x21), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x22), 0x11 }, { RK3576_HDPTXPHY_CMN(0x23), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x24), 0x00 }, { RK3576_HDPTXPHY_CMN(0x25), 0x53 },
  { RK3576_HDPTXPHY_CMN(0x26), 0x00 }, { RK3576_HDPTXPHY_CMN(0x27), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x28), 0x01 }, { RK3576_HDPTXPHY_CMN(0x2a), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x2b), 0x00 }, { RK3576_HDPTXPHY_CMN(0x2c), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x2d), 0x00 }, { RK3576_HDPTXPHY_CMN(0x2e), 0x04 },
  { RK3576_HDPTXPHY_CMN(0x2f), 0x00 }, { RK3576_HDPTXPHY_CMN(0x30), 0x20 },
  { RK3576_HDPTXPHY_CMN(0x31), 0x30 }, { RK3576_HDPTXPHY_CMN(0x32), 0x0b },
  { RK3576_HDPTXPHY_CMN(0x33), 0x23 }, { RK3576_HDPTXPHY_CMN(0x34), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x3d), 0x40 }, { RK3576_HDPTXPHY_CMN(0x42), 0x78 },
  { RK3576_HDPTXPHY_CMN(0x46), 0xdd }, { RK3576_HDPTXPHY_CMN(0x48), 0x11 },
  { RK3576_HDPTXPHY_CMN(0x4e), 0x34 }, { RK3576_HDPTXPHY_CMN(0x5c), 0x25 },
  { RK3576_HDPTXPHY_CMN(0x5d), 0x0c }, { RK3576_HDPTXPHY_CMN(0x5e), 0x4f },
  { RK3576_HDPTXPHY_CMN(0x6b), 0x04 }, { RK3576_HDPTXPHY_CMN(0x73), 0x30 },
  { RK3576_HDPTXPHY_CMN(0x74), 0x04 }, { RK3576_HDPTXPHY_CMN(0x81), 0x01 },
  { RK3576_HDPTXPHY_CMN(0x86), 0x01 }, { RK3576_HDPTXPHY_CMN(0x87), 0x04 },
  { RK3576_HDPTXPHY_CMN(0x89), 0x00 }, { RK3576_HDPTXPHY_CMN(0x95), 0x00 },
  { RK3576_HDPTXPHY_CMN(0x97), 0x02 }, { RK3576_HDPTXPHY_CMN(0x99), 0x04 },
  { RK3576_HDPTXPHY_CMN(0x9b), 0x00 },
};

/* Sideband timing-generator delay settings.  HDMI does not need the sideband
 * for video, but they are zeroed before the lane bring-up and
 * the analog side-band block shares the CMN power rails, so this is kept.
 */

static const struct rk3576_hdptxphy_seq_s g_hdptxphy_common_sb_seq[] = {
  { RK3576_HDPTXPHY_SB(0x114), 0x00 },
  { RK3576_HDPTXPHY_SB(0x115), 0x00 },
  { RK3576_HDPTXPHY_SB(0x116), 0x00 },
  { RK3576_HDPTXPHY_SB(0x117), 0x00 },
};

/* LNTOP equaliser settings for rates up to 340 MHz (HDMI 1.4b) and above it.
 * The difference is the serialiser clock ratio: <= 340 MHz uses a 1/10
 * bit-rate clock, above it a 1/40 one, which changes the equaliser tap
 * programming.
 */

static const struct rk3576_hdptxphy_seq_s g_hdptxphy_tmds_lntop_lowbr_seq[] = {
  { RK3576_HDPTXPHY_LNTOP(0x201), 0x07 },
  { RK3576_HDPTXPHY_LNTOP(0x202), 0xc1 },
  { RK3576_HDPTXPHY_LNTOP(0x203), 0xf0 },
  { RK3576_HDPTXPHY_LNTOP(0x204), 0x7c },
  { RK3576_HDPTXPHY_LNTOP(0x205), 0x1f },
};

static const struct rk3576_hdptxphy_seq_s
    g_hdptxphy_tmds_lntop_highbr_seq[] = {
      { RK3576_HDPTXPHY_LNTOP(0x201), 0x00 },
      { RK3576_HDPTXPHY_LNTOP(0x202), 0x00 },
      { RK3576_HDPTXPHY_LNTOP(0x203), 0x0f },
      { RK3576_HDPTXPHY_LNTOP(0x204), 0xff },
      { RK3576_HDPTXPHY_LNTOP(0x205), 0xff },
    };

/* Per-lane programming, written to all four LANE banks by adding
 * RK3576_HDPTXPHY_LANE_STRIDE per lane.  The common table is applied first and
 * the TMDS table second, so where the two overlap the TMDS value wins (common
 * sets 0303 = 0x0c, TMDS then sets 0303 = 0x2f).
 */

static const struct rk3576_hdptxphy_seq_s g_hdptxphy_common_lane_seq[] = {
  { RK3576_HDPTXPHY_LANE(0x303), 0x0c }, { RK3576_HDPTXPHY_LANE(0x307), 0x20 },
  { RK3576_HDPTXPHY_LANE(0x30a), 0x17 }, { RK3576_HDPTXPHY_LANE(0x30b), 0x77 },
  { RK3576_HDPTXPHY_LANE(0x30c), 0x77 }, { RK3576_HDPTXPHY_LANE(0x30d), 0x77 },
  { RK3576_HDPTXPHY_LANE(0x30e), 0x38 }, { RK3576_HDPTXPHY_LANE(0x310), 0x03 },
  { RK3576_HDPTXPHY_LANE(0x311), 0x0f }, { RK3576_HDPTXPHY_LANE(0x316), 0x02 },
  { RK3576_HDPTXPHY_LANE(0x31b), 0x01 }, { RK3576_HDPTXPHY_LANE(0x31f), 0x15 },
  { RK3576_HDPTXPHY_LANE(0x320), 0xa0 },
};

static const struct rk3576_hdptxphy_seq_s g_hdptxphy_tmds_lane_seq[] = {
  { RK3576_HDPTXPHY_LANE(0x312), 0x00 }, { RK3576_HDPTXPHY_LANE(0x303), 0x2f },
  { RK3576_HDPTXPHY_LANE(0x305), 0x03 }, { RK3576_HDPTXPHY_LANE(0x306), 0x1c },
  { RK3576_HDPTXPHY_LANE(0x31e), 0x02 },
};

/* Lane 3's skew-detection mode differs from the other three: it carries the
 * TMDS clock rather than data, so it must not be told to track inter-pair
 * skew the same way (0x0a instead of 0x02 in LANE_REG(031e)).
 */

#define RK3576_HDPTXPHY_LANE3_SKEW_SEQ_INDEX 4
#define RK3576_HDPTXPHY_LANE3_SKEW_VALUE     0x0a

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdptxphy_getreg / putreg / modifyreg
 *
 * Description:
 *   PHY SFR access.  The SFR banks are 32-bit registers without a write mask,
 *   so modifyreg() is safe here (unlike the GRF block).
 ****************************************************************************/

static uint32_t rk3576_hdptxphy_getreg(struct rk3576_hdptxphy_s *priv,
                                       uint32_t offset)
{
  return getreg32(priv->base + offset);
}

static void rk3576_hdptxphy_putreg(struct rk3576_hdptxphy_s *priv,
                                   uint32_t offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

static void rk3576_hdptxphy_modifyreg(struct rk3576_hdptxphy_s *priv,
                                      uint32_t offset, uint32_t mask,
                                      uint32_t value)
{
  uint32_t regval = rk3576_hdptxphy_getreg(priv, offset);

  rk3576_hdptxphy_putreg(priv, offset, (regval & ~mask) | (value & mask));
}

/****************************************************************************
 * Name: rk3576_hdptxphy_rst_write
 *
 * Description:
 *   Assert or release one CRU soft-reset bit.  All CRU reset registers use the
 *   hiword write mask (bit 16+N gates bit N) and reset polarity "1 = reset".
 *
 * Input Parameters:
 *   priv   - Driver state
 *   con    - SOFTRST register index within the CRU block
 *   bit    - Bit position
 *   assert - true to assert (hold in reset), false to release
 ****************************************************************************/

static void rk3576_hdptxphy_rst_write(struct rk3576_hdptxphy_s *priv,
                                      uint32_t con, uint32_t bit, bool assert)
{
  putreg32((1u << (16u + bit)) | (assert ? (1u << bit) : 0u),
           priv->pmu1 + RK3576_PMU1CRU_SOFTRST_CON(con));
}

/****************************************************************************
 * Name: rk3576_hdptxphy_grf_con0
 *
 * Description:
 *   Set or clear one single-bit field of HDPTXPHY_GRF_CON0 using the hiword
 *   write mask.  Every field the driver touches in this register is one bit.
 ****************************************************************************/

static void rk3576_hdptxphy_grf_con0(struct rk3576_hdptxphy_s *priv,
                                     uint32_t bit, bool value)
{
  putreg32(RK3576_HDPTXPHY_GRF_WM16(bit, value ? 1u : 0u),
           priv->grf + RK3576_HDPTXPHY_GRF_CON0_OFF);
}

/****************************************************************************
 * Name: rk3576_hdptxphy_status
 *
 * Description:
 *   Read HDPTXPHY_GRF_STATUS0, the PHY's ready handshakes.
 ****************************************************************************/

static uint32_t rk3576_hdptxphy_status(struct rk3576_hdptxphy_s *priv)
{
  return getreg32(priv->grf + RK3576_HDPTXPHY_GRF_STATUS0_OFF);
}

/****************************************************************************
 * Name: rk3576_hdptxphy_poll
 *
 * Description:
 *   Wait for the given status bits to become set.
 *
 * Input Parameters:
 *   priv    - Driver state
 *   mask    - Bits that must all be set
 *   tries   - Number of attempts
 *   delay_us - Delay between attempts, microseconds
 *
 * Returned Value:
 *   OK once the bits are set, -ETIMEDOUT if they never appear.
 ****************************************************************************/

static int rk3576_hdptxphy_poll(struct rk3576_hdptxphy_s *priv, uint32_t mask,
                                uint32_t tries, uint32_t delay_us)
{
  uint32_t i;

  for (i = 0; i < tries; i++)
    {
      if ((rk3576_hdptxphy_status(priv) & mask) == mask)
        {
          return OK;
        }

      up_udelay(delay_us);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_hdptxphy_write_seq / _write_lane_seq
 *
 * Description:
 *   Apply a register/value programming sequence to the PHY SFR banks.  The
 *   lane variant repeats its sequence once per LANE bank, adding one bank
 *   stride each time, and applies the lane-3 skew override on the last pass.
 ****************************************************************************/

static void rk3576_hdptxphy_write_seq(struct rk3576_hdptxphy_s *priv,
                                      const struct rk3576_hdptxphy_seq_s *seq,
                                      size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      rk3576_hdptxphy_putreg(priv, seq[i].offset, seq[i].value);
    }
}

static void
rk3576_hdptxphy_write_lane_seq(struct rk3576_hdptxphy_s *priv,
                               const struct rk3576_hdptxphy_seq_s *seq,
                               size_t n)
{
  uint32_t lane;
  size_t i;

  for (lane = 0; lane < RK3576_HDPTXPHY_NLANES; lane++)
    {
      for (i = 0; i < n; i++)
        {
          uint32_t value = seq[i].value;

          /* Lane 3 carries the TMDS clock, so its inter-pair-skew tracking
           * mode differs; only the TMDS lane table has an entry for it.
           */

          if (lane == (RK3576_HDPTXPHY_NLANES - 1u) &&
              seq[i].offset == RK3576_HDPTXPHY_LANE(0x31e) && value == 0x02)
            {
              value = RK3576_HDPTXPHY_LANE3_SKEW_VALUE;
            }

          rk3576_hdptxphy_putreg(
              priv, seq[i].offset + lane * RK3576_HDPTXPHY_LANE_STRIDE, value);
        }
    }
}

/****************************************************************************
 * Name: rk3576_hdptxphy_pre_power_up
 *
 * Description:
 *   TRM 25.6.2.1 steps 1-3: pulse the APB reset, hold the init/common/lane
 *   resets asserted, and make sure the PLL, bias and band-gap blocks are off
 *   before their registers are programmed.
 ****************************************************************************/

static void rk3576_hdptxphy_pre_power_up(struct rk3576_hdptxphy_s *priv)
{
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_APB_CON,
                            RK3576_HDPTXPHY_PMU1RST_APB_BIT, true);
  up_udelay(RK3576_HDPTXPHY_APB_RESET_US);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_APB_CON,
                            RK3576_HDPTXPHY_PMU1RST_APB_BIT, false);

  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_LANE_BIT, true);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_CMN_BIT, true);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_INIT_BIT, true);

  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_PLL_EN, false);
  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BIAS_EN, false);
  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BGR_EN, false);
}

/****************************************************************************
 * Name: rk3576_hdptxphy_post_enable_pll
 *
 * Description:
 *   TRM 25.6.2.1 steps 3-5: enable the reference blocks, release the init and
 *   common resets in order, enable the PLL, and wait for the datapath clocks
 *   to stabilise (o_phy_clk_rdy).
 ****************************************************************************/

static int rk3576_hdptxphy_post_enable_pll(struct rk3576_hdptxphy_s *priv)
{
  int ret;

  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BIAS_EN, true);
  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BGR_EN, true);

  up_udelay(RK3576_HDPTXPHY_STEP_US);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_INIT_BIT, false);

  up_udelay(RK3576_HDPTXPHY_STEP_US);
  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_PLL_EN, true);

  up_udelay(RK3576_HDPTXPHY_STEP_US);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_CMN_BIT, false);

  ret = rk3576_hdptxphy_poll(priv, RK3576_HDPTXPHY_GRF_PHY_CLK_RDY,
                             RK3576_HDPTXPHY_PLL_LOCK_TRIES,
                             RK3576_HDPTXPHY_PLL_LOCK_DELAY);
  if (ret < 0)
    {
      _err("ERROR: HDPTXPHY datapath clock never became ready "
           "(STATUS0=%08" PRIx32 ")\n",
           rk3576_hdptxphy_status(priv));
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdptxphy_post_enable_lane
 *
 * Description:
 *   TRM 25.6.2.1 step 6: release the lane reset, enable all serialiser lanes,
 *   and wait for o_phy_rdy and o_pll_lock_done.
 ****************************************************************************/

static int rk3576_hdptxphy_post_enable_lane(struct rk3576_hdptxphy_s *priv)
{
  int ret;

  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_LANE_BIT, false);

  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BIAS_EN, true);
  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BGR_EN, true);

  rk3576_hdptxphy_modifyreg(priv, RK3576_HDPTXPHY_LNTOP0207_OFF,
                            RK3576_HDPTXPHY_LNTOP0207_LANE_MASK,
                            RK3576_HDPTXPHY_LNTOP0207_ALL_LANES);

  ret = rk3576_hdptxphy_poll(
      priv, RK3576_HDPTXPHY_GRF_PHY_RDY | RK3576_HDPTXPHY_GRF_PLL_LOCK_DONE,
      RK3576_HDPTXPHY_LANE_RDY_TRIES, RK3576_HDPTXPHY_LANE_RDY_DELAY);
  if (ret < 0)
    {
      _err("ERROR: HDPTXPHY lanes never became ready "
           "(STATUS0=%08" PRIx32 ")\n",
           rk3576_hdptxphy_status(priv));
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdptxphy_ropll_lookup
 *
 * Description:
 *   Find the validated ROPLL setting for a TMDS character rate.
 *
 * Input Parameters:
 *   char_rate - TMDS character rate in Hz (pixel clock at 8 bpc).
 *
 * Returned Value:
 *   Pointer to the setting, or NULL when the rate cannot be generated.
 ****************************************************************************/

static const struct rk3576_hdptxphy_ropll_s *
rk3576_hdptxphy_ropll_lookup(uint32_t char_rate)
{
  size_t i;

  for (i = 0; i < nitems(g_hdptxphy_tmds_ropll); i++)
    {
      if (g_hdptxphy_tmds_ropll[i].rate == char_rate)
        {
          return &g_hdptxphy_tmds_ropll[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: rk3576_hdptxphy_ropll_config
 *
 * Description:
 *   Program the ROPLL for a TMDS character rate and start it: apply the common
 *   and TMDS CMN tables, load the dividers, enable the clock generator, then
 *   run the PLL half of the power-up handshake.
 *
 *   Caller must hold priv->lock.
 *
 * Input Parameters:
 *   priv - Driver state
 *   bpc  - Colour depth, used for the pixel clock generator's divide ratio.
 *   cfg  - Resolved ROPLL setting.
 *
 * Returned Value:
 *   OK on success, or the handshake's negated errno on failure.
 ****************************************************************************/

static int
rk3576_hdptxphy_ropll_config(struct rk3576_hdptxphy_s *priv, uint8_t bpc,
                             const struct rk3576_hdptxphy_ropll_s *cfg)
{
  rk3576_hdptxphy_pre_power_up(priv);

  /* Take the ROPLL reference from the 24 MHz crystal rather than the LC PLL
   * (i.e. no cascade): the TMDS path does not use the LC PLL at all.
   */

  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_LC_REF_CLK_SEL, false);

  rk3576_hdptxphy_write_seq(priv, g_hdptxphy_common_cmn_seq,
                            nitems(g_hdptxphy_common_cmn_seq));
  rk3576_hdptxphy_write_seq(priv, g_hdptxphy_tmds_cmn_seq,
                            nitems(g_hdptxphy_tmds_cmn_seq));

  /* Divider chain: integer main divider (plus its AFC copy), pre/ref divider,
   * post divider, then the sigma-delta fraction.
   */

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_CMN(0x51), cfg->mdiv);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_CMN(0x55), cfg->mdiv_afc);

  rk3576_hdptxphy_putreg(
      priv, RK3576_HDPTXPHY_CMN0059_OFF,
      (((uint32_t)cfg->pdiv << 4) |
       (uint32_t)cfg->refdiv) /* PDIV is [7:4], REFDIV is [3:0] */);

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_CMN005A_OFF,
                         ((uint32_t)cfg->sdiv << 4) /* SDIV_RBR is [7:4] */);

  rk3576_hdptxphy_modifyreg(priv, RK3576_HDPTXPHY_CMN005E_OFF,
                            RK3576_HDPTXPHY_CMN005E_SDM_EN,
                            cfg->sdm_en ? RK3576_HDPTXPHY_CMN005E_SDM_EN : 0u);

  /* With the fractional stage off its numerator/sign/ratio fields must be
   * cleared, otherwise a stale fraction from a previous mode is re-applied.
   */

  if (!cfg->sdm_en)
    {
      rk3576_hdptxphy_modifyreg(priv, RK3576_HDPTXPHY_CMN005E_OFF,
                                RK3576_HDPTXPHY_CMN005E_RSVD_LOW_NIBBLE, 0u);
    }

  rk3576_hdptxphy_modifyreg(
      priv, RK3576_HDPTXPHY_CMN0064_OFF, RK3576_HDPTXPHY_CMN0064_NUM_SIGN_RBR,
      cfg->sdm_num_sign ? RK3576_HDPTXPHY_CMN0064_NUM_SIGN_RBR : 0u);

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_CMN(0x60), cfg->sdm_deno);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_CMN(0x65), cfg->sdm_num);

  rk3576_hdptxphy_modifyreg(priv, RK3576_HDPTXPHY_CMN0069_OFF,
                            RK3576_HDPTXPHY_CMN0069_SDC_N_MASK,
                            (uint32_t)cfg->sdc_n);

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_CMN(0x6c), cfg->sdc_num);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_CMN(0x70), cfg->sdc_deno);

  /* Pixel clock generator: post divider mirrors the PLL's, the divide ratio
   * follows the colour depth, and the output must finally be enabled.
   */

  rk3576_hdptxphy_modifyreg(
      priv, RK3576_HDPTXPHY_CMN0086_OFF, RK3576_HDPTXPHY_CMN0086_POSTDIV_MASK,
      ((uint32_t)cfg->sdiv << 4) /* POSTDIV_SEL is [7:4] */);

  rk3576_hdptxphy_modifyreg(
      priv, RK3576_HDPTXPHY_CMN0086_OFF, RK3576_HDPTXPHY_CMN0086_CLK_SEL_MASK,
      ((uint32_t)((bpc - 8u) >> 1u) << 1u) /* CLK_SEL is [3:1] */);

  rk3576_hdptxphy_modifyreg(priv, RK3576_HDPTXPHY_CMN0086_OFF,
                            RK3576_HDPTXPHY_CMN0086_CLK_EN,
                            RK3576_HDPTXPHY_CMN0086_CLK_EN);

  _info("HDPTXPHY TMDS ROPLL: rate=%" PRIu32 " mdiv=%u sdiv=%u sdm_en=%u "
        "num_sign=%u num=%u deno=%u sdc_n=%u\n",
        cfg->rate, cfg->mdiv, cfg->sdiv + 1u, cfg->sdm_en, cfg->sdm_num_sign,
        cfg->sdm_num, cfg->sdm_deno, cfg->sdc_n + 3u);

  return rk3576_hdptxphy_post_enable_pll(priv);
}

/****************************************************************************
 * Name: rk3576_hdptxphy_tmds_mode_config
 *
 * Description:
 *   Configure the serialisers for TMDS and complete the lane half of the
 *   power-up handshake.
 *
 *   Caller must hold priv->lock.
 *
 * Input Parameters:
 *   priv - Driver state
 *   rate - TMDS character rate in Hz, used to pick the equaliser table for the
 *          serialiser clock ratio.
 *
 * Returned Value:
 *   OK on success, or the handshake's negated errno on failure.
 ****************************************************************************/

static int rk3576_hdptxphy_tmds_mode_config(struct rk3576_hdptxphy_s *priv,
                                            uint32_t rate)
{
  rk3576_hdptxphy_write_seq(priv, g_hdptxphy_common_sb_seq,
                            nitems(g_hdptxphy_common_sb_seq));

  /* Protocol select: HDMI (not DP) with the TMDS rather than FRL datapath. */

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LNTOP0200_OFF,
                         RK3576_HDPTXPHY_LNTOP0200_TMDS_VALUE);

  if (rate > RK3576_HDPTXPHY_TMDS_HDMI14_MAX_HZ)
    {
      rk3576_hdptxphy_write_seq(priv, g_hdptxphy_tmds_lntop_highbr_seq,
                                nitems(g_hdptxphy_tmds_lntop_highbr_seq));
    }
  else
    {
      rk3576_hdptxphy_write_seq(priv, g_hdptxphy_tmds_lntop_lowbr_seq,
                                nitems(g_hdptxphy_tmds_lntop_lowbr_seq));
    }

  /* 40-bit serialiser data bus, bus-width select asserted. */

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LNTOP0206_OFF,
                         RK3576_HDPTXPHY_LNTOP0206_TMDS_VALUE);

  rk3576_hdptxphy_write_lane_seq(priv, g_hdptxphy_common_lane_seq,
                                 nitems(g_hdptxphy_common_lane_seq));
  rk3576_hdptxphy_write_lane_seq(priv, g_hdptxphy_tmds_lane_seq,
                                 nitems(g_hdptxphy_tmds_lane_seq));

  return rk3576_hdptxphy_post_enable_lane(priv);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_hdptxphy_initialize(void)
{
  struct rk3576_hdptxphy_s *priv = &g_hdptxphy;
  int ret = OK;

  nxmutex_lock(&priv->lock);

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  priv->base = RK3576_HDPTXPHY_ADDR;
  priv->grf = RK3576_HDPTXPHY_GRF_ADDR;
  priv->pmu1 = RK3576_PMU1_CRU_ADDR;
  priv->cru = RK3576_CRU_ADDR;

  priv->pclk_apb = clk_get("pclk_hdptx_apb");
  priv->pclk_grf = clk_get("pclk_hdptx_grf");
  if (priv->pclk_apb == NULL || priv->pclk_grf == NULL)
    {
      _err("ERROR: HDPTXPHY missing APB/GRF clock\n");
      ret = -ENODEV;
      goto err_unlock;
    }

  /* The PHY cannot be programmed without its APB and GRF clocks. */

  ret = clk_enable(priv->pclk_grf);
  if (ret < 0)
    {
      _err("ERROR: HDPTXPHY failed to enable GRF clock: %d\n", ret);
      goto err_unlock;
    }

  ret = clk_enable(priv->pclk_apb);
  if (ret < 0)
    {
      _err("ERROR: HDPTXPHY failed to enable APB clock: %d\n", ret);
      clk_disable(priv->pclk_grf);
      goto err_unlock;
    }

  /* Release the GRF and APB resets.  These are normally left released
   * already; doing it here keeps the driver self-contained.
   */

  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_APB_CON,
                            RK3576_HDPTXPHY_PMU1RST_GRF_BIT, false);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_APB_CON,
                            RK3576_HDPTXPHY_PMU1RST_APB_BIT, false);

  /* Release the PHY link-symbol clock reset in the main CRU (CRU_SOFTRST_
   * CON(75)[1]).  Its gate lives in VO0_GRF_SOC_CON8[3] and is owned by the
   * HDMI controller driver, which programs it together with the rest of the
   * VO0_GRF HDMI fields.
   */

  putreg32((1u << (16u + RK3576_HDPTXPHY_CRU_LINKSYM_RST_BIT)) | 0u,
           priv->cru +
               RK3576_CRU_SOFTRST_CON(RK3576_HDPTXPHY_CRU_LINKSYM_RST_CON));

  priv->initialized = true;

  nxmutex_unlock(&priv->lock);
  return OK;

err_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

int rk3576_hdptxphy_power_on(uint32_t pixel_clock_hz, uint8_t bpc)
{
  struct rk3576_hdptxphy_s *priv = &g_hdptxphy;
  const struct rk3576_hdptxphy_ropll_s *cfg;
  uint64_t char_rate;
  int ret;

  if (pixel_clock_hz == 0)
    {
      return -EINVAL;
    }

  if (bpc != 8 && bpc != 10 && bpc != 12 && bpc != 16)
    {
      _err("ERROR: HDPTXPHY unsupported colour depth: %u bpc\n", bpc);
      return -EINVAL;
    }

  /* The PLL table is keyed by the TMDS character rate, which scales with the
   * colour depth: at 10 bpc a 148.5 MHz pixel clock is carried as a 185.625
   * MHz character rate.  Divide by 8 before multiplying by bpc so the
   * intermediate value cannot overflow 32 bits.
   */

  char_rate = ((uint64_t)pixel_clock_hz / 8u) * bpc;

  cfg = rk3576_hdptxphy_ropll_lookup((uint32_t)char_rate);
  if (cfg == NULL)
    {
      _err("ERROR: HDPTXPHY no TMDS PLL setting for pixel clock %" PRIu32
           " Hz at %u bpc (character rate %" PRIu64 " Hz)\n",
           pixel_clock_hz, bpc, char_rate);
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);

  if (!priv->initialized)
    {
      _err("ERROR: HDPTXPHY not initialized\n");
      nxmutex_unlock(&priv->lock);
      return -EINVAL;
    }

  /* Not in DP mode: the combo PHY's mode select is a PHY-wide bit and HDMI is
   * 0 (TRM HDPTXPHY_GRF_CON0.hdptx_mode_sel).
   */

  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_MODE_SEL, false);

  ret = rk3576_hdptxphy_ropll_config(priv, bpc, cfg);
  if (ret < 0)
    {
      goto err_unlock;
    }

  ret = rk3576_hdptxphy_tmds_mode_config(priv, cfg->rate);
  if (ret < 0)
    {
      goto err_unlock;
    }

  priv->pixel_clock = pixel_clock_hz;
  priv->bpc = bpc;
  priv->powered = true;

  nxmutex_unlock(&priv->lock);
  return OK;

err_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

int rk3576_hdptxphy_power_off(void)
{
  struct rk3576_hdptxphy_s *priv = &g_hdptxphy;
  int ret = OK;

  nxmutex_lock(&priv->lock);

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EINVAL;
    }

  if (!priv->powered)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  /* Disable the lanes before collapsing the analog blocks, so the serialisers
   * are not driving while their bias disappears.
   */

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LNTOP0207_OFF, 0x00);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LANE(0x300), 0x82);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_SB(0x10f), 0xc1);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_SB(0x110), 0x01);

  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LANE(0x301), 0x80);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LANE(0x401), 0x80);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LANE(0x501), 0x80);
  rk3576_hdptxphy_putreg(priv, RK3576_HDPTXPHY_LANE(0x601), 0x80);

  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_LANE_BIT, true);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_CMN_BIT, true);
  rk3576_hdptxphy_rst_write(priv, RK3576_HDPTXPHY_PMU1RST_SEQ_CON,
                            RK3576_HDPTXPHY_PMU1RST_INIT_BIT, true);

  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_PLL_EN, false);
  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BIAS_EN, false);
  rk3576_hdptxphy_grf_con0(priv, RK3576_HDPTXPHY_GRF_BGR_EN, false);

  priv->powered = false;
  priv->pixel_clock = 0;

  nxmutex_unlock(&priv->lock);
  return ret;
}

bool rk3576_hdptxphy_is_ready(void)
{
  return (rk3576_hdptxphy_status(&g_hdptxphy) &
          (RK3576_HDPTXPHY_GRF_PHY_RDY | RK3576_HDPTXPHY_GRF_PLL_LOCK_DONE)) ==
         (RK3576_HDPTXPHY_GRF_PHY_RDY | RK3576_HDPTXPHY_GRF_PLL_LOCK_DONE);
}

uint32_t rk3576_hdptxphy_pixel_clock_hz(void)
{
  return g_hdptxphy.powered ? g_hdptxphy.pixel_clock : 0;
}

#endif /* CONFIG_RK3576_HDPTXPHY */
