/****************************************************************************
 * chips/rk3576/hardware/rk3576_csi_host.h
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
 * RK3576 MIPI CSI HOST hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM Part 2, Chapter 19 "MIPI CSI HOST".
 *
 * The CSI HOST is the CSI-2 protocol layer: it deserialises the incoming
 * packet stream from a D-PHY, checks the packet header ECC and the payload
 * CRC, merges the data lanes and re-emits the payload as a parallel pixel
 * stream with sync signals.  It does NOT demosaic and it does NOT filter by
 * virtual channel or data type -- that filtering happens in VICAP
 * (VICAP_MIPIn_IDn_CTRL1), which is what consumes this block's output.
 *
 * The block is therefore small: thirteen 32-bit registers, no interrupt
 * status register of its own (the error registers are the status, and the
 * mask registers are the enables), and no per-VC or per-DT configuration.
 *
 * There are five instances.  Only CSIHOST0 is fed by the DCPHY; the other
 * four are fed by the two RX-only CSIDPHYs (HOST1+HOST2 share CSIDPHY0,
 * HOST3+HOST4 share CSIDPHY1).
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CSI_HOST_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CSI_HOST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets, relative to RK3576_CSIHOSTn_ADDR.
 *
 * There is deliberately no interrupt enable/status pair: the error
 * registers are the status and MSK1/MSK2 are the per-bit enables.  An error
 * bit that is not masked raises the instance's GIC line directly.
 */

#define RK3576_CSIHOST_VERSION     0x0000 /* Controller version (RO)      */
#define RK3576_CSIHOST_N_LANES     0x0004 /* Active data lanes            */
#define RK3576_CSIHOST_CSI2_RESETN 0x0010 /* CSI2 controller reset        */
#define RK3576_CSIHOST_PHY_STATE   0x0014 /* D-PHY lane state, clock etc. */
#define RK3576_CSIHOST_ERR1        0x0020 /* Error state 1                */
#define RK3576_CSIHOST_ERR2        0x0024 /* Error state 2                */
#define RK3576_CSIHOST_MSK1        0x0028 /* Masks for error 1            */
#define RK3576_CSIHOST_MSK2        0x002C /* Masks for error 2            */
#define RK3576_CSIHOST_CONTROL     0x0040 /* Control                      */
#define RK3576_CSIHOST_HEADER_0    0x0050 /* Most recent packet header    */
#define RK3576_CSIHOST_HEADER_1    0x0054
#define RK3576_CSIHOST_HEADER_2    0x0058
#define RK3576_CSIHOST_HEADER_3    0x005C /* Oldest of the four           */

/* -----------------------------------------------------------------------
 * N_LANES (0x0004)
 *
 * Reset 0.  The TRM is explicit that this may only be changed while the
 * D-PHY lane is in the stop state, so the driver writes it before releasing
 * CSI2_RESETN and while the PHY is still held in reset.
 * -----------------------------------------------------------------------
 */

#define RK3576_CSIHOST_N_LANES_MASK (0x3u)

/* -----------------------------------------------------------------------
 * CSI2_RESETN (0x0010)
 *
 * Reset 0.  Active low: writing 1 releases the controller.  The TRM's
 * application note calls out the ordering -- configure N_LANES *before*
 * pulling this up.
 * -----------------------------------------------------------------------
 */

#define RK3576_CSIHOST_CSI2_RESETN_RELEASE (1u << 0)

/* -----------------------------------------------------------------------
 * PHY_STATE (0x0014), all read-only except bit 31
 *
 *   [31]    bypass_2ecc_tst     payload bypass test mode (double ECC)
 *   [14:11] phy_rxactivehs_3..0 data lane N is actively receiving HS
 *   [10]    phy_stopstateclk    clock lane is in the stop state
 *   [9]     phy_rxulpsclknot    clock lane in ultra-low-power (active low)
 *   [8]     phy_rxclkactivehs   clock lane is receiving a DDR clock
 *   [7:4]   phy_stopstatedata_3..0
 *   [3:0]   phy_rxulpsesc_3..0
 *
 * This register is the only place the RX clock lane's stop state can be
 * seen: the DCPHY's own GRF_STATUS0 has no slave clock lane bit.
 * -----------------------------------------------------------------------
 */

#define RK3576_CSIHOST_PHY_STATE_RXULPSCLKS_SHIFT   9
#define RK3576_CSIHOST_PHY_STATE_STOPSTATECLK       (1u << 10)
#define RK3576_CSIHOST_PHY_STATE_RXACTIVEHS_0       (1u << 11)
#define RK3576_CSIHOST_PHY_STATE_RXACTIVEHS_SHIFT   11
#define RK3576_CSIHOST_PHY_STATE_RXCLKACTIVEHS      (1u << 8)
#define RK3576_CSIHOST_PHY_STATE_STOPSTATEDATA_BASE 4

/* -----------------------------------------------------------------------
 * ERR1 (0x0020) and its masks in MSK1 (0x0028)
 *
 * The mask bits line up one-for-one with the error bits, so the driver
 * only needs the error bit positions.
 * -----------------------------------------------------------------------
 */

#define RK3576_CSIHOST_ERR1_PHY_ERRSOTSYNCHS_BASE  0 /* [3:0] lane 3..0 */
#define RK3576_CSIHOST_ERR1_ERR_F_BNDRY_MATCH_BASE 4
#define RK3576_CSIHOST_ERR1_ERR_F_SEQ_BASE         8
#define RK3576_CSIHOST_ERR1_ERR_FRAME_DATA_BASE    12
#define RK3576_CSIHOST_ERR1_VC_ERR_CRC_BASE        24 /* [27:24] VC3..VC0 */
#define RK3576_CSIHOST_ERR1_ERR_ECC_DOUBLE         (1u << 28)
#define RK3576_CSIHOST_ERR1_PH_CRC_LANE0           (1u << 29)
#define RK3576_CSIHOST_ERR1_PH_CRC_LANE1           (1u << 30)
#define RK3576_CSIHOST_ERR1_PH_CRC_LANE2           (1u << 31)

/* -----------------------------------------------------------------------
 * ERR2 (0x0024) and its masks in MSK2 (0x002C)
 * -----------------------------------------------------------------------
 */

#define RK3576_CSIHOST_ERR2_PHY_ERRESC_BASE     0 /* [3:0] lane 3..0 */
#define RK3576_CSIHOST_ERR2_PHY_ERRSOTHS_BASE   4
#define RK3576_CSIHOST_ERR2_VC_ERR_ECC_BASE     8 /* corrected, [11:8] */
#define RK3576_CSIHOST_ERR2_ERR_ID_VC_BASE      12
#define RK3576_CSIHOST_ERR2_PHY_ERRCONTROL_BASE 16
#define RK3576_CSIHOST_ERR2_CPHY_ERRCODEHS_BASE 24
#define RK3576_CSIHOST_ERR2_FIFO_OVERFLOW       (1u << 28)

/* -----------------------------------------------------------------------
 * CONTROL (0x0040), reset 0x0c204000
 *
 * For CSI-2 reception over a D-PHY the only bits that matter are
 * sw_dsi_en (clear) and sw_cphy_en (clear); the datatype fields only
 * apply to the DSI-receive mode and the debug bit is a test mode.
 * -----------------------------------------------------------------------
 */

#define RK3576_CSIHOST_CONTROL_SW_CPHY_EN     (1u << 0)
#define RK3576_CSIHOST_CONTROL_SW_DSI_EN      (1u << 4)
#define RK3576_CSIHOST_CONTROL_SW_ADAPTER_MUX (1u << 5)
#define RK3576_CSIHOST_CONTROL_SW_DEBUG_EN    (1u << 6)

/* sw_datatype_fs [13:8], _fe [19:14], _ls [25:20], _le [31:26].  Reset
 * values 0x00 / 0x01 / 0x02 / 0x03 respectively. */

#define RK3576_CSIHOST_CONTROL_DATATYPE_FS_SHIFT 8
#define RK3576_CSIHOST_CONTROL_DATATYPE_FS_MASK  (0x3fu << 8)
#define RK3576_CSIHOST_CONTROL_DATATYPE_FE_SHIFT 14
#define RK3576_CSIHOST_CONTROL_DATATYPE_FE_MASK  (0x3fu << 14)
#define RK3576_CSIHOST_CONTROL_DATATYPE_LS_SHIFT 20
#define RK3576_CSIHOST_CONTROL_DATATYPE_LS_MASK  (0x3fu << 20)
#define RK3576_CSIHOST_CONTROL_DATATYPE_LE_SHIFT 26
#define RK3576_CSIHOST_CONTROL_DATATYPE_LE_MASK  (0x3fu << 26)

#define RK3576_CSIHOST_CONTROL_RESET             (0x0c204000u)

/* -----------------------------------------------------------------------
 * HEADER_0..3 (0x0050..0x005C)
 *
 * Each holds one received CSI-2 packet header: {2'b0, ecc[5:0],
 * word_count[15:0], vc[1:0], data_type[5:0]}.  HEADER_0 is the most recent
 * and HEADER_3 the oldest, which makes this the cheapest possible "is the
 * sensor actually sending what we think, on the VC/DT we expect" probe.
 * -----------------------------------------------------------------------
 */

#define RK3576_CSIHOST_HEADER_DATA_TYPE_MASK   (0x3fu)
#define RK3576_CSIHOST_HEADER_VC_SHIFT         6
#define RK3576_CSIHOST_HEADER_VC_MASK          (0x3u << 6)
#define RK3576_CSIHOST_HEADER_WORD_COUNT_SHIFT 8
#define RK3576_CSIHOST_HEADER_WORD_COUNT_MASK  (0xffffu << 8)
#define RK3576_CSIHOST_HEADER_ECC_SHIFT        24
#define RK3576_CSIHOST_HEADER_ECC_MASK         (0x3fu << 24)

/* CSI-2 data type codes the capture path cares about. */

#define RK3576_CSIHOST_DT_RAW8        0x2a
#define RK3576_CSIHOST_DT_RAW10       0x2b
#define RK3576_CSIHOST_DT_RAW12       0x2c
#define RK3576_CSIHOST_DT_YUV422_8BIT 0x1e

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CSI_HOST_H */
