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
 * multi-slot I2S/TDM controller.  Each instance is a 0x1000 register window;
 * SAI1 lives at 0x2a610000 and on the KICKPI-K7 drives an ES8388 codec (SAI1
 * is the bus master: it sources MCLK / BCLK / LRCK, the codec is the slave).
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

/* dmac0 peripheral-request lines for SAI1 (DTS: dmas = <&dmac0 2 &dmac0 3>,
 * dma-names "tx", "rx").  Passed to rk3576_dma_get_channel().
 */

#define RK3576_SAI1_DMA_TX_REQ 2
#define RK3576_SAI1_DMA_RX_REQ 3

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

/* SAI1 clock control in the CRU (0x27200000).  Gate/reset bits verified
 * against RK3576 TRM Part1 CRU_GATE_CON08 (0x0820) and CRU_SOFTRST_CON08
 * (0x0a20); the mclk mux/divider lives in CRU_CLKSEL_CON46 (0x03b8).  Gate
 * bits are active-high-disable (write 0 to ungate).  Use with the
 * RK3576_CRU_GATE_CON()/SOFTRST_CON()/CLKSEL_CON() helpers from
 * hardware/rk3576_cru.h.
 */

#define RK3576_CRU_SAI1_GATE           8
#define RK3576_CRU_SAI1_MCLK_SRC_BIT   (1 << 4) /* mclk_sai1_src_en          */
#define RK3576_CRU_SAI1_MCLK_BIT       (1 << 5) /* mclk_sai1_en              */
#define RK3576_CRU_SAI1_HCLK_BIT       (1 << 6) /* hclk_sai1_en              */

#define RK3576_CRU_SAI1_SOFTRST        8
#define RK3576_CRU_SAI1_MRST_BIT       (1 << 5) /* mresetn_sai1              */
#define RK3576_CRU_SAI1_HRST_BIT       (1 << 6) /* hresetn_sai1              */

#define RK3576_CRU_SAI1_CLKSEL         46
#define RK3576_CRU_SAI1_MCLK_SEL_BIT   (1 << 11) /* 0=mclk_sai1_src 1=mclkin */
#define RK3576_CRU_SAI1_MSRC_SEL_SHIFT 8 /* [10:8] src parent mux     */
#define RK3576_CRU_SAI1_MSRC_SEL_MASK  (0x7 << RK3576_CRU_SAI1_MSRC_SEL_SHIFT)
#define RK3576_CRU_SAI1_MSRC_DIV_SHIFT 0 /* [7:0] src /= div_con+1    */
#define RK3576_CRU_SAI1_MSRC_DIV_MASK  (0xff << RK3576_CRU_SAI1_MSRC_DIV_SHIFT)

/* mclk_sai1_src parent select values for CRU_CLKSEL_CON46[10:8]
 * (RK3576_CRU_SAI1_MSRC_SEL_SHIFT).  Only the ones we use are named; the
 * full list is xin_osc0(0), audio_frac_0..3(1..4), audio_int_0..2(5..7).
 */

#define RK3576_CRU_SAI1_MSRC_SEL_AFRAC0 0x1 /* clk_matrix_audio_frac_0   */

/* Audio fractional divider 0 (clk_matrix_audio_frac_0).  This is the parent
 * we drive mclk_sai1 from.  TRM Part1:
 *   CRU_CLKSEL_CON12 (0x0330): 32-bit fraction, [31:16]=numerator,
 *                              [15:0]=denominator; fout = fin * num / den.
 *   CRU_CLKSEL_CON13 (0x0334)[1:0]: fraction input mux
 *        00=gpll 01=cpll 10=aupll 11=xin_osc0.
 * SAI1 is the only audio consumer during bring-up, so appropriating frac_0
 * is safe; add per-SAI frac assignments here if SAI2..9 are brought up.
 */

#define RK3576_CRU_AFRAC0_DIV_CON   12 /* CRU_CLKSEL_CON12          */
#define RK3576_CRU_AFRAC0_SEL_CON   13 /* CRU_CLKSEL_CON13          */
#define RK3576_CRU_AFRAC0_SEL_SHIFT 0  /* [1:0] fraction input mux  */
#define RK3576_CRU_AFRAC0_SEL_MASK  (0x3 << RK3576_CRU_AFRAC0_SEL_SHIFT)
#define RK3576_CRU_AFRAC0_SEL_AUPLL 0x2 /* clk_aupll_mux             */

/* AUPLL runs at its reset rate = 24 MHz * (M + K/65536) / (P * 2^S) with
 * M=131 P=1 S=3 K=4719 => 24 MHz * 16.384 = 393.216 MHz, a canonical audio
 * rate equal to 32 * 12.288 MHz.  An integer ratio (fin/fout) makes the
 * fractional divider degenerate to a clean /N divide (num=1, den=N), so the
 * resulting MCLK is exact and jitter-free.  Verify on hardware that the
 * loader has not repurposed AUPLL (read CRU_AUPLL_CON0..2) before relying
 * on this constant.
 */

/* AUPLL rate as left by the boot loader, read back from CRU_AUPLL_CON0..2 on
 * the KICKPI-K7: M=131, P=1, S=2, K=4719 => 24MHz*(131+4719/65536)/(1*2^2) =
 * 786.432 MHz.  786.432 / 64 = 12.288 MHz (48kHz*256), an exact integer
 * divide, so the audio_frac_0 fraction degenerates to num=1/den=64.
 */

#define RK3576_AUPLL_FREQ 786432000u

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_SAI_H */
