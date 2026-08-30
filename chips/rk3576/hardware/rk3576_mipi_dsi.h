/****************************************************************************
 * chips/rk3576/hardware/rk3576_mipi_dsi.h
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
 * RK3576 MIPI DSI-2 Host Controller hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM, Part 2, Chapter 18 "MIPI DSI-2 Host
 * Controller".
 *
 * The RK3576 DSI is a DSI-2 (V1.1) host: D-PHY 4-lane @2.5Gbps and C-PHY
 * 3-trio @1.7Gsps, behind the shared MIPI D/C-PHY combo PHY (DCPHY).
 * Commands are transferred over the APB "Command Interface" (CRI) register
 * bank (DSI2_CRI_TX_HDR/PLD for write, DSI2_CRI_RX_HDR/PLD for read),
 * rather than through a dedicated payload FIFO.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MIPI_DSI_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MIPI_DSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (relative to RK3576_DSIHOST_ADDR = 0x27D80000). */

#define RK3576_DSI2_CORE_ID              0x0000 /* Core identification */
#define RK3576_DSI2_VERSION              0x0004 /* Core version */
#define RK3576_DSI2_PWR_UP               0x000C /* Power-up control */
#define RK3576_DSI2_SOFT_RESET           0x0010 /* Soft reset */
#define RK3576_DSI2_INT_ST_MAIN          0x0014 /* Main interrupt source */
#define RK3576_DSI2_MODE_CTRL            0x0018 /* Operating mode request */
#define RK3576_DSI2_MODE_STATUS          0x001C /* Operating mode status */
#define RK3576_DSI2_CORE_STATUS          0x0020 /* Core/busy status */
#define RK3576_DSI2_MANUAL_MODE_CFG      0x0024 /* Manual timing select */
#define RK3576_DSI2_OBS_FSM_STATUS_SEL   0x0028 /* FSM status select */
#define RK3576_DSI2_OBS_FSM_STATUS       0x002C /* FSM status */
#define RK3576_DSI2_OBS_FSM_CFG          0x0030 /* FSM auto init */
#define RK3576_DSI2_OBS_FSM_CTRL         0x0034 /* FSM manual init */
#define RK3576_DSI2_OBS_FIFO_STATUS_SEL  0x0038 /* FIFO status select */
#define RK3576_DSI2_OBS_FIFO_STATUS      0x003C /* FIFO status */
#define RK3576_DSI2_OBS_FIFO_CFG         0x0040 /* FIFO auto flush */
#define RK3576_DSI2_OBS_FIFO_CTRL        0x0044 /* FIFO manual flush */
#define RK3576_DSI2_TIMEOUT_HSTX_CFG     0x0048 /* HS TX timeout */
#define RK3576_DSI2_TIMEOUT_HSTXRDY_CFG  0x004C /* HS TX Ready timeout */
#define RK3576_DSI2_TIMEOUT_LPRX_CFG     0x0050 /* LP RX timeout */
#define RK3576_DSI2_TIMEOUT_LPTXRDY_CFG  0x0054 /* LP TX DATA timeout */
#define RK3576_DSI2_TIMEOUT_LPTXTRIG_CFG 0x0058 /* LP TX TRIGGER timeout */
#define RK3576_DSI2_TIMEOUT_LPTXULPS_CFG 0x005C /* LP TX ULPS timeout */
#define RK3576_DSI2_TIMEOUT_BTA_CFG      0x0060 /* Bus turnaround timeout */

/* PHY configuration registers. */

#define RK3576_DSI2_PHY_MODE_CFG          0x0100 /* PHY interface config */
#define RK3576_DSI2_PHY_CLK_CFG           0x0104 /* LP clock div + clk type */
#define RK3576_DSI2_PHY_STATUS            0x0108 /* PHY status */
#define RK3576_DSI2_PHY_LP2HS_MAN_CFG     0x010C /* LP->HS time (manual) */
#define RK3576_DSI2_PHY_LP2HS_AUTO_CFG    0x0110 /* LP->HS time (auto) */
#define RK3576_DSI2_PHY_HS2LP_MAN_CFG     0x0114 /* HS->LP time (manual) */
#define RK3576_DSI2_PHY_HS2LP_AUTO_CFG    0x0118 /* HS->LP time (auto) */
#define RK3576_DSI2_PHY_MAX_RD_T_MAN_CFG  0x011C /* Max read time (manual) */
#define RK3576_DSI2_PHY_MAX_RD_T_AUTO     0x0120 /* Max read time (auto) */
#define RK3576_DSI2_PHY_ESC_CMD_T_MAN_CFG 0x0124 /* Esc cmd time (manual) */
#define RK3576_DSI2_PHY_ESC_CMD_T_AUTO    0x0128 /* Esc cmd time (auto) */
#define RK3576_DSI2_PHY_ESC_BYTE_T_MAN_CFG                               \
  0x012C                                       /* Esc byte time (manual) \
                                                */
#define RK3576_DSI2_PHY_ESC_BYTE_T_AUTO 0x0130 /* Esc byte time (auto) */
#define RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG                                    \
  0x0134                                          /* HSTX/IPI ratio (manual) \
                                                   */
#define RK3576_DSI2_PHY_IPI_RATIO_MAN_AUTO 0x0138 /* HSTX/IPI ratio (auto) */
#define RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG                                    \
  0x013C                                          /* HSTX/SYS ratio (manual) \
                                                   */
#define RK3576_DSI2_PHY_SYS_RATIO_MAN_AUTO 0x0140 /* HSTX/SYS ratio (auto) */

/* PHY Protocol Interface (PRI) registers. */

#define RK3576_DSI2_PRI_TX_CMD    0x01C0 /* PHY request transmit */
#define RK3576_DSI2_PRI_RX_CMD    0x01C4 /* PHY trigger received */
#define RK3576_DSI2_PRI_CAL_CTRL  0x01C8 /* Calibration time */
#define RK3576_DSI2_PRI_ULPS_CTRL 0x01CC /* ULPS entry/exit */

/* DSI generic configuration registers. */

#define RK3576_DSI2_DSI_GENERAL_CFG    0x0200 /* BTA/EoTp enable */
#define RK3576_DSI2_DSI_VCID_CFG       0x0204 /* TX virtual channel */
#define RK3576_DSI2_DSI_SCRAMBLING_CFG 0x0208 /* Scrambling */
#define RK3576_DSI2_DSI_VID_TX_CFG     0x020C /* Video TX options */
#define RK3576_DSI2_DSI_MAX_RPS_CFG    0x0210 /* Max RX packet size */

/* Command interface (CRI) registers. */

#define RK3576_DSI2_CRI_TX_HDR 0x02C0 /* TX packet header */
#define RK3576_DSI2_CRI_TX_PLD 0x02C4 /* TX packet payload */
#define RK3576_DSI2_CRI_RX_HDR 0x02C8 /* RX packet header */
#define RK3576_DSI2_CRI_RX_PLD 0x02CC /* RX packet payload */

/* Image Pixel Interface (IPI) configuration registers. */

#define RK3576_DSI2_IPI_COLOR_MAN_CFG     0x0300 /* IPI color depth/format */
#define RK3576_DSI2_IPI_VID_HSA_MAN_CFG   0x0304 /* HSA (manual) */
#define RK3576_DSI2_IPI_VID_HSA_AUTO      0x0308 /* HSA (auto) */
#define RK3576_DSI2_IPI_VID_HBP_MAN_CFG   0x030C /* HBP (manual) */
#define RK3576_DSI2_IPI_VID_HBP_AUTO      0x0310 /* HBP (auto) */
#define RK3576_DSI2_IPI_VID_HACT_MAN_CFG  0x0314 /* HACT (manual) */
#define RK3576_DSI2_IPI_VID_HACT_AUTO     0x0318 /* HACT (auto) */
#define RK3576_DSI2_IPI_VID_HLINE_MAN_CFG 0x031C /* HLINE (manual) */
#define RK3576_DSI2_IPI_VID_HLINE_AUTO    0x0320 /* HLINE (auto) */
#define RK3576_DSI2_IPI_VID_VSA_MAN_CFG   0x0324 /* VSA (manual) */
#define RK3576_DSI2_IPI_VID_VSA_AUTO      0x0328 /* VSA (auto) */
#define RK3576_DSI2_IPI_VID_VBP_MAN_CFG   0x032C /* VBP (manual) */
#define RK3576_DSI2_IPI_VID_VBP_AUTO      0x0330 /* VBP (auto) */
#define RK3576_DSI2_IPI_VID_VACT_MAN_CFG  0x0334 /* VACT (manual) */
#define RK3576_DSI2_IPI_VID_VACT_AUTO     0x0338 /* VACT (auto) */
#define RK3576_DSI2_IPI_VID_VFP_MAN_CFG   0x033C /* VFP (manual) */
#define RK3576_DSI2_IPI_VID_VFP_AUTO      0x0340 /* VFP (auto) */
#define RK3576_DSI2_IPI_PIX_PKT_CFG       0x0344 /* Pixels per packet */

/* Interrupt group registers (state / mask / force). */

#define RK3576_DSI2_INT_ST_PHY     0x0400
#define RK3576_DSI2_INT_MASK_PHY   0x0404
#define RK3576_DSI2_INT_FORCE_PHY  0x0408
#define RK3576_DSI2_INT_ST_TO      0x0410
#define RK3576_DSI2_INT_MASK_TO    0x0414
#define RK3576_DSI2_INT_FORCE_TO   0x0418
#define RK3576_DSI2_INT_ST_ACK     0x0420
#define RK3576_DSI2_INT_MASK_ACK   0x0424
#define RK3576_DSI2_INT_FORCE_ACK  0x0428
#define RK3576_DSI2_INT_ST_IPI     0x0430
#define RK3576_DSI2_INT_MASK_IPI   0x0434
#define RK3576_DSI2_INT_FORCE_IPI  0x0438
#define RK3576_DSI2_INT_ST_FIFO    0x0440
#define RK3576_DSI2_INT_MASK_FIFO  0x0444
#define RK3576_DSI2_INT_FORCE_FIFO 0x0448
#define RK3576_DSI2_INT_ST_PRI     0x0450
#define RK3576_DSI2_INT_MASK_PRI   0x0454
#define RK3576_DSI2_INT_FORCE_PRI  0x0458
#define RK3576_DSI2_INT_ST_CRI     0x0460
#define RK3576_DSI2_INT_MASK_CRI   0x0464
#define RK3576_DSI2_INT_FORCE_CRI  0x0468

/* -----------------------------------------------------------------------
 * DSI2_PWR_UP (0x000C)
 * -----------------------------------------------------------------------
 */

#define DSI2_PWR_UP_PWR_UP_SHIFT (0)
#define DSI2_PWR_UP_PWR_UP       (1u << 0) /* 1: power-up, 0: reset */

/* -----------------------------------------------------------------------
 * DSI2_SOFT_RESET (0x0010) — bits are active-low resets (1 = released)
 * -----------------------------------------------------------------------
 */

#define DSI2_SOFT_RESET_PHY_RSTN (1u << 1)
#define DSI2_SOFT_RESET_SYS_RSTN (1u << 2)
#define DSI2_SOFT_RESET_IPI_RSTN (1u << 0)

/* -----------------------------------------------------------------------
 * DSI2_MODE_CTRL (0x0018) [2:0] — operating mode request
 * -----------------------------------------------------------------------
 */

#define DSI2_MODE_IDLE       0x0 /* Idle mode */
#define DSI2_MODE_AUTOCALC   0x1 /* Auto-calculation mode */
#define DSI2_MODE_COMMAND    0x2 /* Command mode */
#define DSI2_MODE_VIDEO      0x3 /* Video mode */
#define DSI2_MODE_DATASTREAM 0x4 /* Data stream mode */

/* -----------------------------------------------------------------------
 * DSI2_CORE_STATUS (0x0020)
 * -----------------------------------------------------------------------
 */

#define DSI2_CORE_STATUS_PRI_RD_DATA_AVAIL    (1u << 24)
#define DSI2_CORE_STATUS_PRI_FIFOS_NOT_EMPTY  (1u << 23)
#define DSI2_CORE_STATUS_PRI_BUSY             (1u << 22)
#define DSI2_CORE_STATUS_CRI_RD_DATA_AVAIL    (1u << 18)
#define DSI2_CORE_STATUS_CRI_FIFOS_NOT_EMPTY  (1u << 17)
#define DSI2_CORE_STATUS_CRI_BUSY             (1u << 16)
#define DSI2_CORE_STATUS_IPI_FIFOS_NOT_EMPTY  (1u << 9)
#define DSI2_CORE_STATUS_IPI_BUSY             (1u << 8)
#define DSI2_CORE_STATUS_CORE_FIFOS_NOT_EMPTY (1u << 1)
#define DSI2_CORE_STATUS_CORE_BUSY            (1u << 0)

/* -----------------------------------------------------------------------
 * DSI2_MANUAL_MODE_CFG (0x0024)
 * -----------------------------------------------------------------------
 */

#define DSI2_MANUAL_MODE_EN (1u << 0)

/* -----------------------------------------------------------------------
 * DSI2_PHY_MODE_CFG (0x0100)
 * -----------------------------------------------------------------------
 */

#define DSI2_PHY_MODE_PPI_WIDTH_SHIFT (8)
#define DSI2_PHY_MODE_PPI_WIDTH_MASK  (0x3 << 8)
#define DSI2_PHY_MODE_PPI_WIDTH_8     (0 << 8)
#define DSI2_PHY_MODE_PPI_WIDTH_16    (1 << 8)
#define DSI2_PHY_MODE_PPI_WIDTH_32    (2 << 8)
#define DSI2_PHY_MODE_PHY_LANES_SHIFT (4)
#define DSI2_PHY_MODE_PHY_LANES_MASK  (0x3 << 4)
#define DSI2_PHY_MODE_PHY_LANES(n)    (((n)-1) << 4) /* n = 1..4 */

/* phy_type field [0]: selects the PHY interface (TRM 18.4.3). */

#define DSI2_PHY_MODE_PHY_TYPE_SHIFT (0)
#define DSI2_PHY_MODE_PHY_TYPE_DPHY  (0u << 0) /* D-PHY */
#define DSI2_PHY_MODE_PHY_TYPE_CPHY  (1u << 0) /* C-PHY */

/* -----------------------------------------------------------------------
 * DSI2_PHY_CLK_CFG (0x0104)
 * -----------------------------------------------------------------------
 */

#define DSI2_PHY_CLK_LPTX_DIV_SHIFT     (8)
#define DSI2_PHY_CLK_LPTX_DIV_MASK      (0x1f << 8)
#define DSI2_PHY_CLK_LPTX_DIV(n)        (((n) / 2) << 8) /* div = 2n */
#define DSI2_PHY_CLK_TYPE_NONCONTINUOUS (1u << 0)

/* -----------------------------------------------------------------------
 * DSI2_DSI_GENERAL_CFG (0x0200)
 * -----------------------------------------------------------------------
 */

#define DSI2_GENERAL_BTA_EN     (1u << 1)
#define DSI2_GENERAL_EOTP_TX_EN (1u << 0)

/* -----------------------------------------------------------------------
 * DSI2_CRI_TX_HDR (0x02C0)
 * -----------------------------------------------------------------------
 */

#define DSI2_CRI_TX_HDR_LONG         (1u << 29) /* long packet */
#define DSI2_CRI_TX_HDR_RD           (1u << 28) /* read request */
#define DSI2_CRI_TX_HDR_TX_MODE      (1u << 24) /* 0: HS, 1: LP */
#define DSI2_CRI_TX_HDR_WC_MSB_SHIFT (16)
#define DSI2_CRI_TX_HDR_WC_LSB_SHIFT (8)
#define DSI2_CRI_TX_HDR_VC_SHIFT     (6)
#define DSI2_CRI_TX_HDR_DT_SHIFT     (0)

#define DSI2_CRI_TX_HDR_DT_MASK      (0x3f)
#define DSI2_CRI_TX_HDR_VC_MASK      (0x3)

/* -----------------------------------------------------------------------
 * DSI2_DSI_VID_TX_CFG (0x020C)
 * -----------------------------------------------------------------------
 */

#define DSI2_VID_TX_LPDT_DISPLAY_CMD_EN (1u << 20) /* display cmds in LP */
#define DSI2_VID_TX_BLK_VFP_HS_EN       (1u << 14)
#define DSI2_VID_TX_BLK_VBP_HS_EN       (1u << 13)
#define DSI2_VID_TX_BLK_VSA_HS_EN       (1u << 12)
#define DSI2_VID_TX_BLK_HFP_HS_EN       (1u << 6)
#define DSI2_VID_TX_BLK_HBP_HS_EN       (1u << 5)
#define DSI2_VID_TX_BLK_HSA_HS_EN       (1u << 4)
#define DSI2_VID_TX_VID_MODE_TYPE_SHIFT (0)
#define DSI2_VID_TX_VID_MODE_TYPE_MASK  (0x3)

/* vid_mode_type values (TRM 18.3.2.3). */

#define DSI2_VID_MODE_NON_BURST_SYNC_PULSES (0x0)
#define DSI2_VID_MODE_NON_BURST_SYNC_EVENTS (0x1)
#define DSI2_VID_MODE_BURST                 (0x2)

/* -----------------------------------------------------------------------
 * DSI2_IPI_COLOR_MAN_CFG (0x0300)
 * -----------------------------------------------------------------------
 */

#define DSI2_IPI_COLOR_DEPTH_SHIFT  (4)
#define DSI2_IPI_COLOR_DEPTH_MASK   (0xf << 4)
#define DSI2_IPI_COLOR_DEPTH_565    (0x2 << 4) /* 5-6-5 bits */
#define DSI2_IPI_COLOR_DEPTH_6      (0x3 << 4) /* 6 bits */
#define DSI2_IPI_COLOR_DEPTH_8      (0x5 << 4) /* 8 bits */
#define DSI2_IPI_COLOR_DEPTH_10     (0x6 << 4) /* 10 bits */
#define DSI2_IPI_COLOR_FORMAT_SHIFT (0)
#define DSI2_IPI_COLOR_FORMAT_MASK  (0xf)
#define DSI2_IPI_COLOR_FORMAT_RGB   (0x0)

/* -----------------------------------------------------------------------
 * DSI2_IPI video timing (0x0304..0x0340) — horizontal periods are measured
 * in cycles of phy_hstx_clk and stored as fixed-point with 13 integral and
 * 16 fractional bits; vertical periods are counted in lines.
 * -----------------------------------------------------------------------
 */

#define DSI2_IPI_HSA_TIME_SHIFT   (0)
#define DSI2_IPI_HSA_TIME_MASK    (0x3fffffffu)
#define DSI2_IPI_HBP_TIME_SHIFT   (0)
#define DSI2_IPI_HBP_TIME_MASK    (0x3fffffffu)
#define DSI2_IPI_HACT_TIME_SHIFT  (0)
#define DSI2_IPI_HACT_TIME_MASK   (0x3fffffffu)
#define DSI2_IPI_HLINE_TIME_SHIFT (0)
#define DSI2_IPI_HLINE_TIME_MASK  (0xffffffffu)
#define DSI2_IPI_VSA_LINES_SHIFT  (0)
#define DSI2_IPI_VSA_LINES_MASK   (0x3ffu) /* [9:0] */
#define DSI2_IPI_VBP_LINES_SHIFT  (0)
#define DSI2_IPI_VBP_LINES_MASK   (0x3ffu) /* [9:0] */
#define DSI2_IPI_VACT_LINES_SHIFT (0)
#define DSI2_IPI_VACT_LINES_MASK  (0x3fffu) /* [13:0] */
#define DSI2_IPI_VFP_LINES_SHIFT  (0)
#define DSI2_IPI_VFP_LINES_MASK   (0x1fffu) /* [12:0] */

/* -----------------------------------------------------------------------
 * DSI2_IPI_PIX_PKT_CFG (0x0344)
 * -----------------------------------------------------------------------
 */

#define DSI2_IPI_PIX_PKT_MAX_SHIFT (0)
#define DSI2_IPI_PIX_PKT_MAX_MASK  (0xffff)

/* -----------------------------------------------------------------------
 * DSI2_PHY_IPI_RATIO_MAN_CFG (0x0134) / DSI2_PHY_SYS_RATIO_MAN_CFG (0x013C)
 * — ratio of HSTX clock to IPI / SYS clock.  Fixed-point with 6 integral
 * and 16 fractional bits.
 * -----------------------------------------------------------------------
 */

#define DSI2_PHY_IPI_RATIO_SHIFT (0)
#define DSI2_PHY_IPI_RATIO_MASK  (0x3fffffu)
#define DSI2_PHY_SYS_RATIO_SHIFT (0)
#define DSI2_PHY_SYS_RATIO_MASK  (0x3fffffu)

/* -----------------------------------------------------------------------
 * VO0_GRF_SOC_CON10 (VO0_GRF base 0x2601A000 + offset 0x0028)
 * — DSI host gating + IPI pixel-interface fields.  The IPI color depth /
 * format fields must be programmed to match the VOP pixel output, otherwise
 * the DSI-2 IPI interface samples the wrong width and the video stream is
 * dropped (black screen) even though the DCS command link works.
 *
 * GRF registers use the hiword write-mask scheme: bits [31:16] select which
 * low bits [15:0] are actually written (bit 16+N enables bit N).
 * -----------------------------------------------------------------------
 */

#define RK3576_VO0_GRF_SOC_CON10_OFF   0x0028

#define RK3576_VO0_GRF_IPI_GATING_EN   (1u << 0)  /* IPI memory clk gating */
#define RK3576_VO0_GRF_TXREQCLKHS_MASK (1u << 1)  /* deskew request mask */
#define RK3576_VO0_GRF_IPI_COLORM      (1u << 2)  /* IPI color mode */
#define RK3576_VO0_GRF_IPI_SHUTDN      (1u << 3)  /* IPI shutdown */
#define RK3576_VO0_GRF_IPI_FORMAT_MASK (0xfu << 4) /* IPI pixel format */
#define RK3576_VO0_GRF_IPI_FORMAT_DSC  (0x1u << 4) /* DSC-compressed */
#define RK3576_VO0_GRF_IPI_DEPTH_MASK  (0xfu << 8) /* IPI color depth */

/* IPI color depth values (bits [11:8]), matching DSI2_IPI_COLOR_DEPTH_*:

 *   RGB888 -> 8-bit -> 0x5
 *   RGB666 -> 6-bit -> 0x3
 *   RGB565 -> 5-6-5 -> 0x2
 */

#define RK3576_VO0_GRF_IPI_DEPTH_8     (0x5u << 8)
#define RK3576_VO0_GRF_IPI_DEPTH_6     (0x3u << 8)
#define RK3576_VO0_GRF_IPI_DEPTH_565   (0x2u << 8)

/* Hiword write-enable mask for GRF (bit 16+N enables low bit N). */

#define RK3576_GRF_HWM(bits) ((bits) << 16)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MIPI_DSI_H */
