/****************************************************************************
 * chips/rk3576/hardware/rk3576_spi.h
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
 * Rockchip RK3576 SPI (Serial Peripheral Interface) hardware register
 * definitions.
 *
 * Reference:
 *   - Rockchip RK3576 TRM Part1 V1.2, Chapter 30 "Serial Peripheral
 *     Interface (SPI)"
 *
 * The SPI IP is a Synopsys DesignWare SSI (dw_ssi) compatible controller.
 * RK3576 has five controllers: SPI0..SPI4.  Each controller supports two
 * slave-select lines (SS_N0 / SS_N1) and a 64-entry TX / 64-entry RX FIFO.
 *
 * Master mode only is implemented by this driver; the Motorola SPI frame
 * format (frf=00) with 8-bit data frames is used.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of SPI controllers */

#define RK3576_SPI_NUM_CONTROLLERS 5
#define RK3576_SPI_NUM_CHIPSELECTS 2

/* --- SPI Registers (TRM §30.4.2) --- */

#define RK3576_SPI_CTRLR0_OFFSET  0x0000 /* Control Register 0 */
#define RK3576_SPI_CTRLR1_OFFSET  0x0004 /* Control Register 1 */
#define RK3576_SPI_ENR_OFFSET     0x0008 /* SPI Enable Register */
#define RK3576_SPI_SER_OFFSET     0x000C /* Slave Enable Register */
#define RK3576_SPI_BAUDR_OFFSET   0x0010 /* Baud Rate Select */
#define RK3576_SPI_TXFTLR_OFFSET  0x0014 /* TX FIFO Threshold Level */
#define RK3576_SPI_RXFTLR_OFFSET  0x0018 /* RX FIFO Threshold Level */
#define RK3576_SPI_TXFLR_OFFSET   0x001C /* TX FIFO Level */
#define RK3576_SPI_RXFLR_OFFSET   0x0020 /* RX FIFO Level */
#define RK3576_SPI_SR_OFFSET      0x0024 /* SPI Status */
#define RK3576_SPI_IPR_OFFSET     0x0028 /* Interrupt Polarity */
#define RK3576_SPI_IMR_OFFSET     0x002C /* Interrupt Mask */
#define RK3576_SPI_ISR_OFFSET     0x0030 /* Interrupt Status */
#define RK3576_SPI_RISR_OFFSET    0x0034 /* Raw Interrupt Status */
#define RK3576_SPI_ICR_OFFSET     0x0038 /* Interrupt Clear */
#define RK3576_SPI_DMACR_OFFSET   0x003C /* DMA Control */
#define RK3576_SPI_DMATDLR_OFFSET 0x0040 /* DMA TX Data Level */
#define RK3576_SPI_DMARDLR_OFFSET 0x0044 /* DMA RX Data Level */
#define RK3576_SPI_VERSION_OFFSET 0x0048 /* IP Version */
#define RK3576_SPI_TIMEOUT_OFFSET 0x004C /* Timeout Control */
#define RK3576_SPI_BYPASS_OFFSET  0x0050 /* Bypass Control */
#define RK3576_SPI_TXDR_OFFSET    0x0400 /* Transmit FIFO Data */
#define RK3576_SPI_RXDR_OFFSET    0x0800 /* Receive FIFO Data */

/* --- SPI_CTRLR0 bit fields (TRM §30.4.3) --- */

#define RK3576_SPI_CTRLR0_DFS_SHIFT   0  /* Data frame size */
#define RK3576_SPI_CTRLR0_CFS_SHIFT   2  /* Microwire control word size */
#define RK3576_SPI_CTRLR0_SCPH_SHIFT  6  /* Serial clock phase */
#define RK3576_SPI_CTRLR0_SCPOL_SHIFT 7  /* Serial clock polarity */
#define RK3576_SPI_CTRLR0_CSM_SHIFT   8  /* Chip-select mode */
#define RK3576_SPI_CTRLR0_SSD_SHIFT   10 /* Slave enable delay */
#define RK3576_SPI_CTRLR0_EM_SHIFT    11 /* Endian mode */
#define RK3576_SPI_CTRLR0_FBM_SHIFT   12 /* First bit mode (MSB/LSB) */
#define RK3576_SPI_CTRLR0_BHT_SHIFT \
  13 /* APB 8/16-bit access for 8-bit frame */
#define RK3576_SPI_CTRLR0_RSD_SHIFT 14 /* RX sample delay */
#define RK3576_SPI_CTRLR0_FRF_SHIFT 16 /* Frame format */
#define RK3576_SPI_CTRLR0_XFM_SHIFT 18 /* Transmit and receive mode */
#define RK3576_SPI_CTRLR0_OPM_SHIFT 20 /* Master/slave mode */
#define RK3576_SPI_CTRLR0_MTM_SHIFT 21 /* Microwire sequential transfer */
#define RK3576_SPI_CTRLR0_SM_SHIFT  22 /* SCLK masked by SS_N */
#define RK3576_SPI_CTRLR0_SOI_SHIFT 23 /* ss_in output inverted */
#define RK3576_SPI_CTRLR0_LBK_SHIFT 25 /* Loop-back mode */

#define RK3576_SPI_CTRLR0_DFS_MASK  (0x3 << RK3576_SPI_CTRLR0_DFS_SHIFT)
#define RK3576_SPI_CTRLR0_SCPH      (1 << RK3576_SPI_CTRLR0_SCPH_SHIFT)
#define RK3576_SPI_CTRLR0_SCPOL     (1 << RK3576_SPI_CTRLR0_SCPOL_SHIFT)
#define RK3576_SPI_CTRLR0_CSM_MASK  (0x3 << RK3576_SPI_CTRLR0_CSM_SHIFT)
#define RK3576_SPI_CTRLR0_SSD       (1 << RK3576_SPI_CTRLR0_SSD_SHIFT)
#define RK3576_SPI_CTRLR0_EM        (1 << RK3576_SPI_CTRLR0_EM_SHIFT)
#define RK3576_SPI_CTRLR0_FBM       (1 << RK3576_SPI_CTRLR0_FBM_SHIFT)
#define RK3576_SPI_CTRLR0_BHT       (1 << RK3576_SPI_CTRLR0_BHT_SHIFT)
#define RK3576_SPI_CTRLR0_RSD_MASK  (0x3 << RK3576_SPI_CTRLR0_RSD_SHIFT)
#define RK3576_SPI_CTRLR0_FRF_MASK  (0x3 << RK3576_SPI_CTRLR0_FRF_SHIFT)
#define RK3576_SPI_CTRLR0_XFM_MASK  (0x3 << RK3576_SPI_CTRLR0_XFM_SHIFT)
#define RK3576_SPI_CTRLR0_OPM       (1 << RK3576_SPI_CTRLR0_OPM_SHIFT)

/* Control Register 0 sub-field encodings */

#define RK3576_SPI_CTRLR0_DFS_4BITS  0x0 /* 4-bit data frame */
#define RK3576_SPI_CTRLR0_DFS_8BITS  0x1 /* 8-bit data frame */
#define RK3576_SPI_CTRLR0_DFS_16BITS 0x2 /* 16-bit data frame */

#define RK3576_SPI_CTRLR0_FRF_SPI    0x0 /* Motorola SPI */
#define RK3576_SPI_CTRLR0_FRF_TI     0x1 /* Texas Instruments SSP */
#define RK3576_SPI_CTRLR0_FRF_MICROWIRE \
  0x2 /* National Semiconductors Microwire */

#define RK3576_SPI_CTRLR0_XFM_TXRX 0x0 /* Transmit & Receive */
#define RK3576_SPI_CTRLR0_XFM_TX   0x1 /* Transmit only */
#define RK3576_SPI_CTRLR0_XFM_RX   0x2 /* Receive only */

#define RK3576_SPI_CTRLR0_CSM_KEEP_LOW \
  0x0 /* SS_N stays low between frames \
       */
#define RK3576_SPI_CTRLR0_CSM_HALF_HIGH \
  0x1 /* SS_N high 1/2 SCLK between frames */
#define RK3576_SPI_CTRLR0_CSM_ONE_HIGH \
  0x2 /* SS_N high 1 SCLK between frames */

/* --- SPI_ENR --- */

#define RK3576_SPI_ENR_EN (1 << 0)

/* --- SPI_SER --- */

#define RK3576_SPI_SER_SER(cs) (1 << (cs))

/* --- SPI_BAUDR --- */

/* Full 16-bit field mask.  The hardware only accepts even divisors
 * (TRM: BAUDR is any even value between 2 and 65534; the LSB is ignored
 * on write), so MASK itself (0xffff) is NOT a valid max value.  Use
 * RK3576_SPI_BAUDR_MAX (the largest even value) as an upper bound.
 */

#define RK3576_SPI_BAUDR_MASK 0xffff
#define RK3576_SPI_BAUDR_MAX  0xfffe /* largest even BAUDR (65534) */

/* --- SPI_SR bit fields (TRM §30.4.3) --- */

#define RK3576_SPI_SR_BSF (1 << 0) /* Busy flag */
#define RK3576_SPI_SR_TFF (1 << 1) /* TX FIFO full */
#define RK3576_SPI_SR_TFE (1 << 2) /* TX FIFO empty */
#define RK3576_SPI_SR_RFE (1 << 3) /* RX FIFO empty */
#define RK3576_SPI_SR_RFF (1 << 4) /* RX FIFO full */
#define RK3576_SPI_SR_STB (1 << 5) /* Slave TX busy */
#define RK3576_SPI_SR_SSI (1 << 6) /* ss_in_n state */

/* --- SPI_IMR / SPI_ISR / SPI_RISR bit fields (TRM §30.4.3) --- */

#define RK3576_SPI_IMR_TXE (1 << 0) /* TX FIFO empty */
#define RK3576_SPI_IMR_TXO (1 << 1) /* TX FIFO overflow */
#define RK3576_SPI_IMR_RXU (1 << 2) /* RX FIFO underflow */
#define RK3576_SPI_IMR_RXO (1 << 3) /* RX FIFO overflow */
#define RK3576_SPI_IMR_RXF (1 << 4) /* RX FIFO full */
#define RK3576_SPI_IMR_TO  (1 << 5) /* Timeout */
#define RK3576_SPI_IMR_SSP (1 << 6) /* ss_in_n posedge */
#define RK3576_SPI_IMR_TXF (1 << 7) /* TX finish */

#define RK3576_SPI_ISR_TXE (1 << 0)
#define RK3576_SPI_ISR_TXO (1 << 1)
#define RK3576_SPI_ISR_RXU (1 << 2)
#define RK3576_SPI_ISR_RXO (1 << 3)
#define RK3576_SPI_ISR_RXF (1 << 4)
#define RK3576_SPI_ISR_TO  (1 << 5)
#define RK3576_SPI_ISR_SSP (1 << 6)
#define RK3576_SPI_ISR_TXF (1 << 7)

/* --- SPI_ICR --- */

#define RK3576_SPI_ICR_CTFOI (1 << 0) /* Clear TX FIFO overflow */
#define RK3576_SPI_ICR_CRFOI (1 << 1) /* Clear RX FIFO overflow */
#define RK3576_SPI_ICR_CRFUI (1 << 2) /* Clear RX FIFO underflow */
#define RK3576_SPI_ICR_CTOI  (1 << 3) /* Clear timeout */
#define RK3576_SPI_ICR_CSSPI (1 << 4) /* Clear ss_in_n posedge */
#define RK3576_SPI_ICR_CTXFI (1 << 5) /* Clear TX finish */
#define RK3576_SPI_ICR_CCI   (1 << 6) /* Clear combined interrupt */

/* --- SPI_DMACR --- */

#define RK3576_SPI_DMACR_RDE (1 << 0) /* RX DMA enable */
#define RK3576_SPI_DMACR_TDE (1 << 1) /* TX DMA enable */

/* --- SPI_DMATDLR / SPI_DMARDLR watermark mask --- */

#define RK3576_SPI_DMATDLR_MASK 0x3f
#define RK3576_SPI_DMARDLR_MASK 0x3f

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H */
