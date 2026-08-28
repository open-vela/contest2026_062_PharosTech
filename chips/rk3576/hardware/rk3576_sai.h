/****************************************************************************
 * chips/rk3576/hardware/rk3576_sai.h
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
 * The RK3576 SAI (Serial Audio Interface, "rockchip,sai-v1") is a
 * multi-slot I2S/TDM controller.  Each instance is a 0x1000 register window
 * A SAI controller is the I2S bus master, sourcing MCLK / BCLK / LRCK
 * to a slave codec; which controller is wired to which codec is decided
 * by the board layer.
 *
 * Register field encodings below are transcribed from the RK3576 TRM Part1
 * V1.2 section 24 (SAI).  The TX / RX operation-control registers (SAI_TXCR
 * @0x00, SAI_RXCR @0x08) share an identical bit layout, so a single set of
 * SAI_XCR_* macros describes both.  Several fields (slot number / slot width
 * / valid-data width / dividers) may only be written while the corresponding
 * XFER enable bit is 0 -- the driver always configures them while stopped.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_SAI_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_SAI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DMA peripheral-request lines for every SAI controller, from the TRM
 * Table 1-4 DMAC Request Mapping.  Each SAI hangs off one of the three
 * PL330 controllers (dmac0/1/2) with a (tx, rx) request pair.
 *
 * Note: SAI5 has only an RX request and SAI7/SAI8/SAI9 only a TX request --
 * those controllers are inherently single-direction and the respective
 * partner request line is undefined (ND).
 */

/* SAI0, SAI1 : DMAC0 */
#define RK3576_SAI0_DMA_TX_REQ 0
#define RK3576_SAI0_DMA_RX_REQ 1
#define RK3576_SAI1_DMA_TX_REQ 2
#define RK3576_SAI1_DMA_RX_REQ 3

/* SAI2, SAI3, SAI8 : DMAC1 */
#define RK3576_SAI2_DMA_TX_REQ 0
#define RK3576_SAI2_DMA_RX_REQ 1
#define RK3576_SAI3_DMA_TX_REQ 2
#define RK3576_SAI3_DMA_RX_REQ 3
#define RK3576_SAI8_DMA_TX_REQ 7

/* SAI4, SAI5, SAI6, SAI7 : DMAC2 */
#define RK3576_SAI4_DMA_TX_REQ 0
#define RK3576_SAI4_DMA_RX_REQ 1
#define RK3576_SAI5_DMA_RX_REQ 3
#define RK3576_SAI6_DMA_TX_REQ 4
#define RK3576_SAI6_DMA_RX_REQ 5
#define RK3576_SAI7_DMA_TX_REQ 19

/* SAI9 : DMAC0 (single TX request line). */
#define RK3576_SAI9_DMA_TX_REQ 26

/* Register offsets (per instance window) ******************************/

#define RK3576_SAI_TXCR          0x0000 /* Transmit operation control      */
#define RK3576_SAI_FSCR          0x0004 /* Transmit frame-sync control     */
#define RK3576_SAI_RXCR          0x0008 /* Receive operation control       */
#define RK3576_SAI_MONO_CR       0x000c /* Mono-mode control               */
#define RK3576_SAI_XFER          0x0010 /* Transfer start                  */
#define RK3576_SAI_CLR           0x0014 /* Sclk-domain logic clear         */
#define RK3576_SAI_CKR           0x0018 /* Clock generation                */
#define RK3576_SAI_TXFIFOLR      0x001c /* TX FIFO level (RO)              */
#define RK3576_SAI_RXFIFOLR      0x0020 /* RX FIFO level (RO)              */
#define RK3576_SAI_DMACR         0x0024 /* DMA control                     */
#define RK3576_SAI_INTCR         0x0028 /* Interrupt control               */
#define RK3576_SAI_INTSR         0x002c /* Interrupt status (RO)           */
#define RK3576_SAI_TXDR          0x0030 /* Transmit FIFO data (WO)         */
#define RK3576_SAI_RXDR          0x0034 /* Receive FIFO data (RO)          */
#define RK3576_SAI_PATH_SEL      0x0038 /* Path select                     */
#define RK3576_SAI_TX_SLOT_MASK0 0x003c /* Transmit slot 0~31 mask         */
#define RK3576_SAI_TX_SLOT_MASK1 0x0040 /* Transmit slot 32~63 mask        */
#define RK3576_SAI_TX_SLOT_MASK2 0x0044 /* Transmit slot 64~95 mask        */
#define RK3576_SAI_TX_SLOT_MASK3 0x0048 /* Transmit slot 96~127 mask       */
#define RK3576_SAI_RX_SLOT_MASK0 0x004c /* Receive slot 0~31 mask          */
#define RK3576_SAI_RX_SLOT_MASK1 0x0050 /* Receive slot 32~63 mask         */
#define RK3576_SAI_RX_SLOT_MASK2 0x0054 /* Receive slot 64~95 mask         */
#define RK3576_SAI_RX_SLOT_MASK3 0x0058 /* Receive slot 96~127 mask        */
#define RK3576_SAI_TX_DATA_CNT   0x005c /* TX data counter                 */
#define RK3576_SAI_RX_DATA_CNT   0x0060 /* RX data counter                 */
#define RK3576_SAI_TX_SHIFT      0x0064 /* TX shift                        */
#define RK3576_SAI_RX_SHIFT      0x0068 /* RX shift                        */
#define RK3576_SAI_RD_STATUS     0x006c /* Read status                     */
#define RK3576_SAI_VERSION       0x0070 /* Version (RO, reset 0x23073576)  */
#define RK3576_SAI_STATUS        0x006c /* Stream idle status, v2307+     */
#define RK3576_SAI_VER_2307      0x23073576
#define RK3576_SAI_VER_2311      0x23112118
#define SAI_STATUS_TX_IDLE       (1 << 2)
#define SAI_STATUS_FS_IDLE       (1 << 1)
#define SAI_XFER_TX_IDLE         (1 << 7)
#define SAI_XFER_FS_IDLE         (1 << 6)
#define SAI_TXFIFOLR_LANE0_MASK  0x3f
#define SAI_TXFIFOLR_LEVEL_MASK  0x00ffffff
#define SAI_TXFIFOLR_GROUP_BITS  6

/* SAI_TXCR (0x00) / SAI_RXCR (0x08) shared layout *********************/

#define SAI_XCR_DELAY_EN  (1 << 22) /* Delay fsync pos               */
#define SAI_XCR_CSR_SHIFT 20        /* [21:20] parallel data lanes-1 */
#define SAI_XCR_CSR_MASK  (0x3 << SAI_XCR_CSR_SHIFT)
#define SAI_XCR_CSR(n)    (((n)-1) << SAI_XCR_CSR_SHIFT)
#define SAI_XCR_SJM       (1 << 19) /* Store 1=left/0=right justified*/
#define SAI_XCR_FBM       (1 << 18) /* First bit 0=MSB / 1=LSB       */
#define SAI_XCR_SNB_SHIFT 11        /* [17:11] slots per frame-1     */
#define SAI_XCR_SNB_MASK  (0x7f << SAI_XCR_SNB_SHIFT)
#define SAI_XCR_SNB(n)    (((n)-1) << SAI_XCR_SNB_SHIFT)
#define SAI_XCR_VDJ       (1 << 10) /* Valid data 1=left justified   */
#define SAI_XCR_SBW_SHIFT 5         /* [9:5] slot bit width-1        */
#define SAI_XCR_SBW_MASK  (0x1f << SAI_XCR_SBW_SHIFT)
#define SAI_XCR_SBW(bits) (((bits)-1) << SAI_XCR_SBW_SHIFT)
#define SAI_XCR_VDW_SHIFT 0 /* [4:0] valid data width-1      */
#define SAI_XCR_VDW_MASK  (0x1f << SAI_XCR_VDW_SHIFT)
#define SAI_XCR_VDW(bits) (((bits)-1) << SAI_XCR_VDW_SHIFT)

/* Slot / valid-data width encodings: 16bit..32bit map to 0x0f..0x1f, i.e.
 * the raw field value is (bits - 1).  Values below 16 bits are reserved.
 */

/* SAI_FSCR (0x04) ****************************************************/

#define SAI_FSCR_EDGE_SEL  (1 << 24) /* 0=pos / 1=pos and neg         */
#define SAI_FSCR_FPW_SHIFT 12        /* [23:12] frame pulse width-1   */
#define SAI_FSCR_FPW_MASK  (0xfff << SAI_FSCR_FPW_SHIFT)
#define SAI_FSCR_FPW(bits) (((bits)-1) << SAI_FSCR_FPW_SHIFT)
#define SAI_FSCR_FW_SHIFT  0 /* [11:0] frame width-1          */
#define SAI_FSCR_FW_MASK   (0xfff << SAI_FSCR_FW_SHIFT)
#define SAI_FSCR_FW(bits)  (((bits)-1) << SAI_FSCR_FW_SHIFT)

/* SAI_TX_SHIFT (0x64) / SAI_RX_SHIFT (0x68) ************************/
/* [22:0] Transfer frame sync pulse shift control:
 *   0: Normal mode
 *   1: Fs 1/2 cycle shift left
 *   2: Fs 1 cycle shift left
 *   3: Fs 3/2 cycle shift left ...
 * Standard I2S uses "1 cycle shift left" (value 2) so that the first
 * data bit follows FS one full BCLK later than the FS transition edge.
 */

#define SAI_XSHIFT_FS_SHIFT  0 /* [22:0] fsync pulse shift        */
#define SAI_XSHIFT_FS_MASK   (0x7fffff << SAI_XSHIFT_FS_SHIFT)
#define SAI_XSHIFT_FS(n)     ((n) << SAI_XSHIFT_FS_SHIFT)
#define SAI_XSHIFT_FS_1CYCLE SAI_XSHIFT_FS(2) /* Fs 1 cycle shift left */

/* SAI_MONO_CR (0x0c) ***********************************************/

#define SAI_MONO_CR_RX_SLOT_SHIFT 2        /* [8:2] rx mono valid slot sel  */
#define SAI_MONO_CR_RX_MONO_EN    (1 << 1) /* Receive mono enable           */
#define SAI_MONO_CR_TX_MONO_EN    (1 << 0) /* Transmit mono enable          */

/* SAI_XFER (0x10) *************************************************/

#define SAI_XFER_RX_DCNT_EN (1 << 5) /* Start rx data counter         */
#define SAI_XFER_TX_DCNT_EN (1 << 4) /* Start tx data counter         */
#define SAI_XFER_RXS        (1 << 3) /* Start RX transfer             */
#define SAI_XFER_TXS        (1 << 2) /* Start TX transfer             */
#define SAI_XFER_FSS        (1 << 1) /* Start frame sync              */
#define SAI_XFER_CLK        (1 << 0) /* Start clk (clk_gate_en)       */

/* SAI_CLR (0x14) -- self-clearing, poll until read back 0 *********/

#define SAI_CLR_FSC (1 << 2) /* Clear frame-sync logic        */
#define SAI_CLR_RXC (1 << 1) /* Clear RX logic                */
#define SAI_CLR_TXC (1 << 0) /* Clear TX logic                */

/* SAI_CKR (0x18) *************************************************/

#define SAI_CKR_MDIV_SHIFT 3 /* [14:3] mclk divider           */
#define SAI_CKR_MDIV_MASK  (0xfff << SAI_CKR_MDIV_SHIFT)
#define SAI_CKR_MDIV(n)    (((n)-1) << SAI_CKR_MDIV_SHIFT) /* sclk=mclk/n */
#define SAI_CKR_MSS        (1 << 2) /* 0=master(sclk out) 1=slave    */
#define SAI_CKR_CKP        (1 << 1) /* Sclk polarity                 */
#define SAI_CKR_FSP        (1 << 0) /* Frame-sync polarity 0=low act */

/* SAI_DMACR (0x24) **********************************************/

#define SAI_DMACR_RDE       (1 << 24) /* Receive DMA enable            */
#define SAI_DMACR_RDL_SHIFT 16        /* [20:16] rx watermark          */
#define SAI_DMACR_RDL_MASK  (0x1f << SAI_DMACR_RDL_SHIFT)
#define SAI_DMACR_RDL(n)    ((n) << SAI_DMACR_RDL_SHIFT) /* req: lvl>=n+1 */
#define SAI_DMACR_TDE       (1 << 8) /* Transmit DMA enable           */
#define SAI_DMACR_TDL_SHIFT 0        /* [4:0] tx watermark            */
#define SAI_DMACR_TDL_MASK  (0x1f << SAI_DMACR_TDL_SHIFT)
#define SAI_DMACR_TDL(n)    ((n) << SAI_DMACR_TDL_SHIFT) /* req: lvl<=n */

/* SAI_INTCR (0x28) **********************************************/

#define SAI_INTCR_RFT_SHIFT 20        /* [24:20] rx fifo threshold     */
#define SAI_INTCR_RXOIC     (1 << 18) /* W1: clear rx overrun          */
#define SAI_INTCR_RXOIE     (1 << 17) /* Rx overrun int enable         */
#define SAI_INTCR_RXFIE     (1 << 16) /* Rx full int enable            */
#define SAI_INTCR_TFT_SHIFT 4         /* [8:4] tx fifo threshold       */
#define SAI_INTCR_TXUIC     (1 << 2)  /* W1: clear tx underrun         */
#define SAI_INTCR_TXUIE     (1 << 1)  /* Tx underrun int enable        */
#define SAI_INTCR_TXEIE     (1 << 0)  /* Tx empty int enable           */

/* SAI_INTSR (0x2c) **********************************************/

#define SAI_INTSR_RXOI (1 << 17) /* Rx overrun interrupt active   */
#define SAI_INTSR_RXFI (1 << 16) /* Rx full interrupt active      */
#define SAI_INTSR_TXUI (1 << 1)  /* Tx underrun interrupt active  */
#define SAI_INTSR_TXEI (1 << 0)  /* Tx empty interrupt active     */

/* SAI soft-reset control.  Per the TRM each SAI has a (mresetn, hresetn)
 * pair in one CRU_SOFTRST_CONn register; the driver pulses both when a
 * controller is brought up.  The clock framework (rk3576_clk.c) is the
 * source of truth for the GATE/CLKSEL bits, so only the reset indices and
 * masks are needed here.  Use with RK3576_CRU_SOFTRST_CON() from
 * hardware/rk3576_cru.h.
 *
 *   SAI0 : CON07 (0x0A1C)  mresetn=12 hresetn=13
 *   SAI1 : CON08 (0x0A20)  mresetn=5  hresetn=6
 *   SAI2 : CON08 (0x0A20)  mresetn=9  hresetn=10
 *   SAI3 : CON08 (0x0A20)  mresetn=12 hresetn=13
 *   SAI4 : CON09 (0x0A24)  mresetn=0  hresetn=2
 *   SAI5 : CON65 (0x0B04)  mresetn=4  hresetn=5
 *   SAI6 : CON65 (0x0B04)  mresetn=8  hresetn=9
 *   SAI7 : CON67 (0x0B0C)  mresetn=5  hresetn=6
 *   SAI8 : CON66 (0x0B08)  mresetn=2  hresetn=0
 *   SAI9 : CON68 (0x0B10)  mresetn=11 hresetn=9
 */

#define RK3576_CRU_SAI0_SOFTRST  7
#define RK3576_CRU_SAI0_MRST_BIT (1 << 12)
#define RK3576_CRU_SAI0_HRST_BIT (1 << 13)

#define RK3576_CRU_SAI1_SOFTRST  8
#define RK3576_CRU_SAI1_MRST_BIT (1 << 5)
#define RK3576_CRU_SAI1_HRST_BIT (1 << 6)

#define RK3576_CRU_SAI2_SOFTRST  8
#define RK3576_CRU_SAI2_MRST_BIT (1 << 9)
#define RK3576_CRU_SAI2_HRST_BIT (1 << 10)

#define RK3576_CRU_SAI3_SOFTRST  8
#define RK3576_CRU_SAI3_MRST_BIT (1 << 12)
#define RK3576_CRU_SAI3_HRST_BIT (1 << 13)

#define RK3576_CRU_SAI4_SOFTRST  9
#define RK3576_CRU_SAI4_MRST_BIT (1 << 0)
#define RK3576_CRU_SAI4_HRST_BIT (1 << 2)

#define RK3576_CRU_SAI5_SOFTRST  65
#define RK3576_CRU_SAI5_MRST_BIT (1 << 4)
#define RK3576_CRU_SAI5_HRST_BIT (1 << 5)

#define RK3576_CRU_SAI6_SOFTRST  65
#define RK3576_CRU_SAI6_MRST_BIT (1 << 8)
#define RK3576_CRU_SAI6_HRST_BIT (1 << 9)

#define RK3576_CRU_SAI7_SOFTRST  67
#define RK3576_CRU_SAI7_MRST_BIT (1 << 5)
#define RK3576_CRU_SAI7_HRST_BIT (1 << 6)

#define RK3576_CRU_SAI8_SOFTRST  66
#define RK3576_CRU_SAI8_MRST_BIT (1 << 2)
#define RK3576_CRU_SAI8_HRST_BIT (1 << 0)

#define RK3576_CRU_SAI9_SOFTRST  68
#define RK3576_CRU_SAI9_MRST_BIT (1 << 11)
#define RK3576_CRU_SAI9_HRST_BIT (1 << 9)

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_SAI_H */
