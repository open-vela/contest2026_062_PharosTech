/****************************************************************************
 * chips/rk3576/hardware/rk3576_vicap.h
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
 * RK3576 VICAP (Video Capture) hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM Part 2, Chapter 6 "Video Capture (VICAP)".
 *
 * VICAP takes the parallel pixel stream produced by a CSI HOST (or by the
 * DVP port), filters it by virtual channel and data type, crops it, and
 * DMAs it to DDR.  It is the block that owns the frame buffer addresses.
 *
 * Address map (base RK3576_VICAP_ADDR = 0x27c10000):
 *
 *   Global / DVP   0x0000 .. 0x00ff
 *   MIPI0          0x0100 .. 0x01ff
 *   MIPI1          0x0300 .. 0x03ff
 *   MIPI2          0x0400 .. 0x04ff
 *   MIPI3          0x0500 .. 0x05ff
 *   MIPI4          0x0600 .. 0x06ff
 *   Scale / TOISP  0x0700 .. 0x07ff
 *   MMU0           0x0800 .. 0x08ff
 *
 * The MIPI blocks are structurally identical and are parameterised by
 * RK3576_VICAP_MIPI_BASE(n).  Each carries four "ID" slots (ID0..ID3),
 * which is how one MIPI input can carry several interleaved virtual
 * channels or data types.
 *
 * Unlike the GRF and CRU blocks, VICAP's `sw_` prefixed bits are ordinary
 * read/write bits -- there is no hiword write-enable scheme here.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VICAP_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VICAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* -----------------------------------------------------------------------
 * Global block (0x0000)
 * -----------------------------------------------------------------------
 */

#define RK3576_VICAP_GLB_CTRL           0x0000
#define RK3576_VICAP_GLB_INTEN          0x0004
#define RK3576_VICAP_GLB_INTST          0x0008
#define RK3576_VICAP_GLB_TOLE           0x000c
#define RK3576_VICAP_DDR_FREQ_CYCLE_NUM 0x07fc

/* GLB_CTRL bits.  Reset value 0x00006e00: dma_group_en, dma_adapt_en,
 * press_en and the hurry/press values are all set out of reset.
 */

#define RK3576_VICAP_GLB_CTRL_SW_CAP_EN           (1u << 0)
#define RK3576_VICAP_GLB_CTRL_SW_SOFT_RST         (1u << 1)
#define RK3576_VICAP_GLB_CTRL_SOFT_RST_MODE_SHIFT 2
#define RK3576_VICAP_GLB_CTRL_SW_CLK_GATING_DIS   (1u << 4)
#define RK3576_VICAP_GLB_CTRL_DMA_GROUP_EN        (1u << 5)
#define RK3576_VICAP_GLB_CTRL_HURRY_EN            (1u << 8)
#define RK3576_VICAP_GLB_CTRL_PRESS_EN            (1u << 12)
#define RK3576_VICAP_GLB_CTRL_DMA_ADAPT_EN        (1u << 16)
#define RK3576_VICAP_GLB_CTRL_INT_MODE            (1u << 17)
#define RK3576_VICAP_GLB_CTRL_ACSA_DIS            (1u << 18)
#define RK3576_VICAP_GLB_CTRL_ACS_ISP_C_DIS       (1u << 19)
#define RK3576_VICAP_GLB_CTRL_DPHY1_2X2LANE_EN    (1u << 25)
#define RK3576_VICAP_GLB_CTRL_DPHY2_2X2LANE_EN    (1u << 26)
#define RK3576_VICAP_GLB_CTRL_RO_AXI_IDLE         (1u << 31)

/* Soft-reset mode: 00 = reset while protecting TOISP frame integrity,
 * 01 = the same and also lower sw_cap_en, 10 = reset directly.
 */

#define RK3576_VICAP_GLB_CTRL_SOFT_RST_MODE_DIRECT 2u

/* -----------------------------------------------------------------------
 * MIPI input blocks.  All five are identical; only the base changes.
 * -----------------------------------------------------------------------
 */

#define RK3576_VICAP_MIPI_STRIDE 0x200
#define RK3576_VICAP_MIPI_BASE(n) \
  ((n) == 0 ? 0x0100u : (0x0100u + (n)*RK3576_VICAP_MIPI_STRIDE))

/* Offsets within a MIPI block. */

#define RK3576_VICAP_MIPI_ID0_CTRL0(n) (RK3576_VICAP_MIPI_BASE(n) + 0x000)
#define RK3576_VICAP_MIPI_ID0_CTRL1(n) (RK3576_VICAP_MIPI_BASE(n) + 0x004)
#define RK3576_VICAP_MIPI_ID1_CTRL0(n) (RK3576_VICAP_MIPI_BASE(n) + 0x008)
#define RK3576_VICAP_MIPI_ID1_CTRL1(n) (RK3576_VICAP_MIPI_BASE(n) + 0x00c)
#define RK3576_VICAP_MIPI_ID2_CTRL0(n) (RK3576_VICAP_MIPI_BASE(n) + 0x010)
#define RK3576_VICAP_MIPI_ID2_CTRL1(n) (RK3576_VICAP_MIPI_BASE(n) + 0x014)
#define RK3576_VICAP_MIPI_ID3_CTRL0(n) (RK3576_VICAP_MIPI_BASE(n) + 0x018)
#define RK3576_VICAP_MIPI_ID3_CTRL1(n) (RK3576_VICAP_MIPI_BASE(n) + 0x01c)

/* Path control.  Reset 0x00001200 (drop_frame_m = drop_frame_n = 1). */

#define RK3576_VICAP_MIPI_CTRL(n)                  (RK3576_VICAP_MIPI_BASE(n) + 0x020)

#define RK3576_VICAP_MIPI_CTRL_CAP_EN              (1u << 0)
#define RK3576_VICAP_MIPI_CTRL_DROP_FRAME_EN       (1u << 8)
#define RK3576_VICAP_MIPI_CTRL_DROP_FRAME_M_SHIFT  12
#define RK3576_VICAP_MIPI_CTRL_DROP_FRAME_N_SHIFT  9
#define RK3576_VICAP_MIPI_CTRL_DMA_RST             (1u << 16)
#define RK3576_VICAP_MIPI_CTRL_SOFT_RST            (1u << 17)
#define RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_SHIFT 18
#define RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_MASK  (3u << 18)
#define RK3576_VICAP_MIPI_CTRL_WATER_LINE_SHIFT    20
#define RK3576_VICAP_MIPI_CTRL_DEBUG_EN            (1u << 31)

/* What sw_soft_rst_mode selects.
 *
 * The two preserving modes exist for the path into the ISP, which has frames
 * of its own to keep intact.  This driver writes RAW to DDR and never uses
 * that path, so the plain reset is the one it wants -- and it is the one to
 * ask for explicitly, because the field's reset value names a preserving mode
 * and a reset that waits for a frame boundary is a reset that never happens on
 * an idle path.
 */

#define RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_PRESERVE          0u
#define RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_PRESERVE_DROP_CAP 1u
#define RK3576_VICAP_MIPI_CTRL_SOFT_RST_MODE_DIRECT            2u

/* Frame buffer addresses, ping-pong: the hardware alternates FRAME0 (even)
 * frames and FRAME1 (odd) frames by itself, so the driver only has to give
 * it two buffers and watch which one the "dma end" interrupt refers to.
 *
 * "Y" is the raw/Y/RGB plane, "UV" the chroma plane.  For a RAW capture
 * only the Y address matters: raw data is a single plane, so FRAME0_ADDR_Y
 * is the one that carries it.
 */

#define RK3576_VICAP_MIPI_FRAME0_ADDR_Y(n)  (RK3576_VICAP_MIPI_BASE(n) + 0x024)
#define RK3576_VICAP_MIPI_FRAME1_ADDR_Y(n)  (RK3576_VICAP_MIPI_BASE(n) + 0x028)
#define RK3576_VICAP_MIPI_FRAME0_ADDR_UV(n) (RK3576_VICAP_MIPI_BASE(n) + 0x02c)
#define RK3576_VICAP_MIPI_FRAME1_ADDR_UV(n) (RK3576_VICAP_MIPI_BASE(n) + 0x030)

/* Virtual line width: the line stride in bytes.  Must be at least 8-byte
 * aligned (64 is what the TRM recommends for DDR efficiency).  A frame
 * occupies stride * height bytes.
 */

#define RK3576_VICAP_MIPI_VLW(n)                (RK3576_VICAP_MIPI_BASE(n) + 0x034)
#define RK3576_VICAP_MIPI_VLW_MASK              0xfffffu
#define RK3576_VICAP_MIPI_VLW_ADDR_FORCE_UPDATE (1u << 31)

/* Interrupt enable / status.  INTSTAT is write-one-to-clear. */

#define RK3576_VICAP_MIPI_INTEN(n)               (RK3576_VICAP_MIPI_BASE(n) + 0x074)
#define RK3576_VICAP_MIPI_INTSTAT(n)             (RK3576_VICAP_MIPI_BASE(n) + 0x078)

#define RK3576_VICAP_MIPI_INT_FRAME_START_ID0    (1u << 0)
#define RK3576_VICAP_MIPI_INT_FRAME_END_ID0      (1u << 4)
#define RK3576_VICAP_MIPI_INT_FRAME0_DMA_END_ID0 (1u << 8)
#define RK3576_VICAP_MIPI_INT_FRAME1_DMA_END_ID0 (1u << 9)
#define RK3576_VICAP_MIPI_INT_DMA_Y_FIFO_OVF     (1u << 16)
#define RK3576_VICAP_MIPI_INT_DMA_UV_FIFO_OVF    (1u << 17)
#define RK3576_VICAP_MIPI_INT_BANDWIDTH_LACK     (1u << 18)
#define RK3576_VICAP_MIPI_INT_CSI2RX_FIFO_OVF    (1u << 19)
#define RK3576_VICAP_MIPI_INT_LINE_ID0           (1u << 20)
#define RK3576_VICAP_MIPI_INT_SIZE_ERR_ID0       (1u << 24)

/* The bits that mean "the picture is wrong" rather than "a frame boundary
 * happened".  Kept together so the diagnostic accumulator cannot quietly
 * absorb the ordinary frame start/end bits.
 */

#define RK3576_VICAP_MIPI_INT_ERRORS                                        \
  (RK3576_VICAP_MIPI_INT_DMA_Y_FIFO_OVF |                                   \
   RK3576_VICAP_MIPI_INT_DMA_UV_FIFO_OVF |                                  \
   RK3576_VICAP_MIPI_INT_BANDWIDTH_LACK |                                   \
   RK3576_VICAP_MIPI_INT_CSI2RX_FIFO_OVF | RK3576_VICAP_MIPI_INT_LINE_ID0 | \
   RK3576_VICAP_MIPI_INT_SIZE_ERR_ID0)

/* Crop start / frame size.  Both the x start and the width must be
 * 4-pixel aligned; the TRM also recommends leaving crop enabled.
 */

#define RK3576_VICAP_MIPI_ID0_CROP_START(n) (RK3576_VICAP_MIPI_BASE(n) + 0x090)
#define RK3576_VICAP_CROP_START_X_SHIFT     0
#define RK3576_VICAP_CROP_START_X_MASK      0x3fffu
#define RK3576_VICAP_CROP_START_Y_SHIFT     16
#define RK3576_VICAP_CROP_START_Y_MASK      (0x3fffu << 16)

#define RK3576_VICAP_MIPI_ID0_SET_SIZE(n)   (RK3576_VICAP_MIPI_BASE(n) + 0x0a0)
#define RK3576_VICAP_SET_SIZE_WIDTH_SHIFT   0
#define RK3576_VICAP_SET_SIZE_WIDTH_MASK    0x3fffu
#define RK3576_VICAP_SET_SIZE_HEIGHT_SHIFT  16
#define RK3576_VICAP_SET_SIZE_HEIGHT_MASK   (0x3fffu << 16)

/* Read-only per-frame diagnostics.  These are what make bring-up possible
 * without a logic analyser: line/pixel counts prove whether the geometry
 * the sensor sends matches the geometry VICAP was told to expect.
 */

#define RK3576_VICAP_MIPI_SIZE_NUM_ID0(n) (RK3576_VICAP_MIPI_BASE(n) + 0x0c0)
#define RK3576_VICAP_SIZE_NUM_PIX_SHIFT   0
#define RK3576_VICAP_SIZE_NUM_PIX_MASK    0x3fffu
#define RK3576_VICAP_SIZE_NUM_LINE_SHIFT  16
#define RK3576_VICAP_SIZE_NUM_LINE_MASK   (0x3fffu << 16)

/* Width of either field once it has been shifted down.  The shifted masks
 * above are for reading the register in place; this one is for extracting a
 * field, and using the wrong one silently reads back zero.
 */

#define RK3576_VICAP_SIZE_NUM_FIELD_MASK   0x3fffu

#define RK3576_VICAP_MIPI_FRAME_NUM_VC0(n) (RK3576_VICAP_MIPI_BASE(n) + 0x0d0)

/* -----------------------------------------------------------------------
 * IDn_CTRL0 -- the data path configuration for one ID slot.
 * -----------------------------------------------------------------------
 */

#define RK3576_VICAP_ID_CTRL0_CAP_EN        (1u << 0)
#define RK3576_VICAP_ID_CTRL0_CROP_EN       (1u << 1)
#define RK3576_VICAP_ID_CTRL0_DROP_FRAME_EN (1u << 2)
#define RK3576_VICAP_ID_CTRL0_DMA_EN        (1u << 3)

/* sw_parse_type: how the incoming payload is interpreted. */

#define RK3576_VICAP_PARSE_TYPE_SHIFT            4
#define RK3576_VICAP_PARSE_TYPE_MASK             (0xfu << 4)
#define RK3576_VICAP_PARSE_TYPE_RAW8             0u
#define RK3576_VICAP_PARSE_TYPE_RAW10            1u
#define RK3576_VICAP_PARSE_TYPE_RAW12            2u
#define RK3576_VICAP_PARSE_TYPE_RAW14            3u
#define RK3576_VICAP_PARSE_TYPE_RAW16            4u
#define RK3576_VICAP_PARSE_TYPE_RGB888           7u
#define RK3576_VICAP_PARSE_TYPE_YUV422_8BIT_PROG 8u
#define RK3576_VICAP_PARSE_TYPE_YUV422_8BIT_INT  9u
#define RK3576_VICAP_PARSE_TYPE_YUV420_8BIT      10u

/* sw_wrddr_type: how the payload is written to DDR. */

#define RK3576_VICAP_WRDDR_TYPE_SHIFT         8
#define RK3576_VICAP_WRDDR_TYPE_MASK          (0x7u << 8)
#define RK3576_VICAP_WRDDR_TYPE_RAW_COMPACT   0u
#define RK3576_VICAP_WRDDR_TYPE_RAW_UNCOMPACT 1u
#define RK3576_VICAP_WRDDR_TYPE_YUV_PACKET    2u
#define RK3576_VICAP_WRDDR_TYPE_YUV400        3u
#define RK3576_VICAP_WRDDR_TYPE_YUV422SP      4u
#define RK3576_VICAP_WRDDR_TYPE_YUV420SP      5u

/* sw_align: where the RAW sample sits inside the 16-bit DDR word.  Only
 * meaningful in the uncompact write mode.
 */

#define RK3576_VICAP_ID_CTRL0_ALIGN_HIGH  (1u << 11)
#define RK3576_VICAP_ID_CTRL0_UVDS_EN     (1u << 16)
#define RK3576_VICAP_ID_CTRL0_FIELD_ORDER (1u << 17)

/* -----------------------------------------------------------------------
 * IDn_CTRL1 -- the VC/DT filter and HDR mode.
 *
 * This is where the "which packets belong to me" decision is made; the CSI
 * HOST deliberately does no filtering at all.
 * -----------------------------------------------------------------------
 */

#define RK3576_VICAP_ID_CTRL1_VC_SHIFT          0
#define RK3576_VICAP_ID_CTRL1_VC_MASK           0x3u
#define RK3576_VICAP_ID_CTRL1_DT_SHIFT          2
#define RK3576_VICAP_ID_CTRL1_DT_MASK           (0x3fu << 2)
#define RK3576_VICAP_ID_CTRL1_HDR_MODE_SHIFT    8
#define RK3576_VICAP_ID_CTRL1_ON_HDR_MODE_SHIFT 10
#define RK3576_VICAP_ID_CTRL1_COMMAND_MODE_EN   (1u << 14)

/* HDR modes, indexed by sw_hdr_mode. */

#define RK3576_VICAP_HDR_MODE_VC   0u /* distinguished by virtual channel */
#define RK3576_VICAP_HDR_MODE_LINE 1u /* distinguished by line number      */
#define RK3576_VICAP_HDR_MODE_SONY 2u /* distinguished by the first 4 px   */

/* -----------------------------------------------------------------------
 * MMU0 (0x0800)
 *
 * Paging resets DISABLED, and is only turned on by explicitly issuing the
 * MMU_ENABLE_PAGING command.  Left alone, the MMU therefore passes the
 * addresses through untranslated, which is what this driver relies on: it
 * programs physical addresses taken from the RK3576 DMA heap.
 *
 * The interrupt bits are still masked explicitly, because a page fault
 * would otherwise raise the separate VICAP MMU interrupt line
 * (RK3576_IRQ_VICAP_MMU) that nothing services.
 * -----------------------------------------------------------------------
 */

#define RK3576_VICAP_MMU_DTE_ADDR                 0x0800
#define RK3576_VICAP_MMU_STATUS                   0x0804
#define RK3576_VICAP_MMU_COMMAND                  0x0808
#define RK3576_VICAP_MMU_PAGE_FAULT_ADDR          0x080c
#define RK3576_VICAP_MMU_ZAP_ONE_LINE             0x0810
#define RK3576_VICAP_MMU_INT_RAWSTAT              0x0814
#define RK3576_VICAP_MMU_INT_CLEAR                0x0818
#define RK3576_VICAP_MMU_INT_MASK                 0x081c
#define RK3576_VICAP_MMU_INT_STATUS               0x0820
#define RK3576_VICAP_MMU_AUTO_GATING              0x0824

#define RK3576_VICAP_MMU_CMD_ENABLE_PAGING        0u
#define RK3576_VICAP_MMU_CMD_DISABLE_PAGING       1u
#define RK3576_VICAP_MMU_CMD_ENABLE_STALL         2u
#define RK3576_VICAP_MMU_CMD_DISABLE_STALL        3u
#define RK3576_VICAP_MMU_CMD_ZAP_CACHE            4u
#define RK3576_VICAP_MMU_CMD_PAGE_FAULT_DONE      5u

#define RK3576_VICAP_MMU_STATUS_PAGING_ENABLED    (1u << 0)
#define RK3576_VICAP_MMU_STATUS_PAGE_FAULT_ACTIVE (1u << 1)
#define RK3576_VICAP_MMU_STATUS_STALL_ACTIVE      (1u << 2)
#define RK3576_VICAP_MMU_STATUS_IDLE              (1u << 3)

#define RK3576_VICAP_MMU_INT_PAGE_FAULT           (1u << 0)
#define RK3576_VICAP_MMU_INT_READ_BUS_ERROR       (1u << 1)
#define RK3576_VICAP_MMU_INT_ALL                  0x3u

/* -----------------------------------------------------------------------
 * CRU reset bits used by the capture path (TRM Part 1).
 *
 *   SOFTRST_CON53[7] aresetn_vicap   [8] hresetn_vicap   [6] dresetn_vicap
 *   SOFTRST_CON59[1] resetn_vicap_i0clk   [0] resetn_cifin
 *
 * All of them are "when high, reset", so releasing means writing 0
 * through the hiword mask.  Only the release side is ever written: nothing
 * here is running yet, but there is also no reason to pulse a reset whose
 * assert side has no defined purpose in this driver.
 * -----------------------------------------------------------------------
 */

#define RK3576_VICAP_RST_CON         53
#define RK3576_VICAP_RST_ARESETN_BIT 7
#define RK3576_VICAP_RST_HRESETN_BIT 8
#define RK3576_VICAP_RST_DRESETN_BIT 6

#define RK3576_VICAP_INPUT_RST_CON   59
#define RK3576_VICAP_RST_CIFIN_BIT   0
#define RK3576_VICAP_RST_I0CLK_BIT   1

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VICAP_H */
