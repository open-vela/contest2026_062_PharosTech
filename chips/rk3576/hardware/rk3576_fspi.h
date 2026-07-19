/****************************************************************************
 * chips/rk3576/hardware/rk3576_fspi.h
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
 * Rockchip RK3576 FSPI (Flexible Serial Peripheral Interface) / SFC
 * (Serial Flash Controller) hardware register definitions.
 *
 * Reference:
 *   - Rockchip RK3576 TRM Part1 V1.2, Chapter 29 "FSPI"
 *   - Linux kernel: drivers/spi/spi-rockchip-sfc.c
 *   - Rockchip BSP: drivers/rkflash/sfc.h
 *
 * The FSPI is Rockchip's proprietary Serial Flash Controller IP,
 * version >= SFC_VER_6 on RK3576.  It supports:
 *   - 1/2/4 data lanes (Quad SPI)
 *   - PIO (FIFO poll) and DMA modes
 *   - SPI NOR / SPI NAND flash, and general QSPI peripherals (e.g. LCD)
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_FSPI_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_FSPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/bits.h>
#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of FSPI controllers and chip selects */

#define RK3576_FSPI_NUM_CONTROLLERS 2
#define RK3576_FSPI_NUM_CHIPSELECTS 2

/* --- FSPI Registers --- */

#define RK3576_FSPI_CTRL0           0x0000 /* Control register */
#define RK3576_FSPI_IMR             0x0004 /* Interrupt mask */
#define RK3576_FSPI_ICLR            0x0008 /* Interrupt clear */
#define RK3576_FSPI_FTLR            0x000C /* FIFO threshold level */
#define RK3576_FSPI_RCVR            0x0010 /* Recover register */
#define RK3576_FSPI_AX0             0x0014 /* Auxiliary data value */
#define RK3576_FSPI_ABIT0           0x0018 /* Extend address bits */
#define RK3576_FSPI_ISR             0x001C /* Interrupt status */
#define RK3576_FSPI_FSR             0x0020 /* FIFO status */
#define RK3576_FSPI_SR              0x0024 /* Status register */
#define RK3576_FSPI_RISR            0x0028 /* Raw interrupt status */
#define RK3576_FSPI_VER             0x002C /* Version register */
#define RK3576_FSPI_QOP             0x0030 /* Quad line IO level preset*/
#define RK3576_FSPI_EXT_CTRL        0x0034 /* Extended control */
#define RK3576_FSPI_DLL_CTRL0       0x003C /* Delay line controller */
#define RK3576_FSPI_EXT_AX          0x0044 /* Extend auxiliary data control */
#define RK3576_FSPI_SCLK_INATM_CNT  0x0048 /* SCLK inactive timeout counter */
#define RK3576_FSPI_XMMC_WCMD0      0x0050 /* Memory mapped control W cmd */
#define RK3576_FSPI_XMMC_RCMD0      0x0054 /* Memory mapped control R cmd */
#define RK3576_FSPI_XMMC_CTRL       0x0058 /* Memory mapped control */
#define RK3576_FSPI_MODE            0x005C /* Controller working mode */
#define RK3576_FSPI_DEVRGN          0x0060 /* Device region size */
#define RK3576_FSPI_DEVSIZE0        0x0064 /* Device size */
#define RK3576_FSPI_TME0            0x0068 /* Timeout enable control */
#define RK3576_FSPI_XMMC_RX_WTMRK   0x0070 /* XMMC RX FIFO water mark */
#define RK3576_FSPI_DUMM_CTRL       0x0074 /* Dummy cycle control*/
#define RK3576_FSPI_CMD_EXT         0x0078 /* Command extend */
#define RK3576_FSPI_TRC_CTRL        0x007C /* TRC cycle control */
#define RK3576_FSPI_DMATR           0x0080 /* DMA trigger */
#define RK3576_FSPI_DMAADDR         0x0084 /* DMA source/dest address */
#define RK3576_FSPI_LEN_CTRL        0x0088 /* Length control */
#define RK3576_FSPI_LEN_EXT         0x008C /* Length extension */
#define RK3576_FSPI_XMMCSR          0x0094 /* Memory mapped status */
#define RK3576_FSPI_HYPER_RSVD_ADDR 0x0098 /* Hyperbus reserved address */
#define RK3576_FSPI_VDMC0           0x009C /* Vendor device mode control */
#define RK3576_FSPI_CMD             0x0100 /* Command register */
#define RK3576_FSPI_ADDR            0x0104 /* Address register */
#define RK3576_FSPI_DATA            0x0108 /* Data register */

/* CS registers */

#define RK3576_FSPI_CS_OFFSET(cs) ((cs)*0x200)
#define RK3576_FSPI_CTRL(cs) \
  RK3576_FSPI_CTRL0 + RK3576_FSPI_CS_OFFSET(cs) /* Control register */
#define RK3576_FSPI_AX(cs) \
  RK3576_FSPI_AX0 + RK3576_FSPI_CS_OFFSET(cs) /* Auxiliary data */
#define RK3576_FSPI_ABIT(cs) \
  RK3576_FSPI_ABIT0 + RK3576_FSPI_CS_OFFSET(cs) /* Address bits */
#define RK3576_FSPI_DLL_CTRL(cs) \
  RK3576_FSPI_DLL_CTRL0 + RK3576_FSPI_CS_OFFSET(cs) /* DLL */
#define RK3576_FSPI_XMMC_WCMD(cs) \
  RK3576_FSPI_XMMC_WCMD0 + RK3576_FSPI_CS_OFFSET(cs) /* XMMC_WCMD */
#define RK3576_FSPI_XMMC_RCMD(cs) \
  RK3576_FSPI_XMMC_RCMD0 + RK3576_FSPI_CS_OFFSET(cs) /* XMMC_RCMD */
#define RK3576_FSPI_DEVSIZE(cs) \
  RK3576_FSPI_DEVSIZE0 + RK3576_FSPI_CS_OFFSET(cs) /* DEVSIZE */
#define RK3576_FSPI_TME(cs) \
  RK3576_FSPI_TME0 + RK3576_FSPI_CS_OFFSET(cs) /* TME */
#define RK3576_FSPI_VDMC(cs) \
  RK3576_FSPI_VDMC0 + RK3576_FSPI_CS_OFFSET(cs) /* VDMC */

/* =========================================================================
 * CTRL register bit-fields (offset 0x00)
 * ========================================================================= */

#define FSPI_CTRL_SPIM_SHIFT         0 /* SPI mode */
#define FSPI_CTRL_SPIM_MASK          (1 << FSPI_CTRL_SPIM_SHIFT)
#define FSPI_CTRL_SPIM_MODE_0        (0 << FSPI_CTRL_SPIM_SHIFT)
#define FSPI_CTRL_SPIM_MODE_3        (1 << FSPI_CTRL_SPIM_SHIFT)
#define FSPI_CTRL_SHIFTPHASE_SHIFT   1 /* Shift phase of data input */
#define FSPI_CTRL_SHIFTPHASE_MASK    (1 << FSPI_CTRL_SHIFTPHASE_SHIFT)
#define FSPI_CTRL_SHIFTPHASE_POSEDGE (0 << FSPI_CTRL_SHIFTPHASE_SHIFT)
#define FSPI_CTRL_SHIFTPHASE_NEGEDGE (1 << FSPI_CTRL_SHIFTPHASE_SHIFT)
#define FSPI_CTRL_CMDB_SHIFT         8 /* CMD line width */
#define FSPI_CTRL_CMDB_MASK          (0x3 << FSPI_CTRL_CMDB_SHIFT)
#define FSPI_CTRL_CMDB_X1            (0 << FSPI_CTRL_CMDB_SHIFT)
#define FSPI_CTRL_CMDB_X2            (1 << FSPI_CTRL_CMDB_SHIFT)
#define FSPI_CTRL_CMDB_X4            (2 << FSPI_CTRL_CMDB_SHIFT)
#define FSPI_CTRL_ADDRB_SHIFT        10 /* ADDR line width */
#define FSPI_CTRL_ADDRB_MASK         (0x3 << FSPI_CTRL_ADDRB_SHIFT)
#define FSPI_CTRL_ADDRB_X1           (0 << FSPI_CTRL_ADDRB_SHIFT)
#define FSPI_CTRL_ADDRB_X2           (1 << FSPI_CTRL_ADDRB_SHIFT)
#define FSPI_CTRL_ADDRB_X4           (2 << FSPI_CTRL_ADDRB_SHIFT)
#define FSPI_CTRL_DATAB_SHIFT        12 /* DATA line width */
#define FSPI_CTRL_DATAB_MASK         (0x3 << FSPI_CTRL_DATAB_SHIFT)
#define FSPI_CTRL_DATAB_X1           (0 << FSPI_CTRL_DATAB_SHIFT)
#define FSPI_CTRL_DATAB_X2           (1 << FSPI_CTRL_DATAB_SHIFT)
#define FSPI_CTRL_DATAB_X4           (2 << FSPI_CTRL_DATAB_SHIFT)

/* =========================================================================
 * Interrupt bits (IMR / ICLR / ISR / RISR, offset 0x04/0x08/0x1c/0x28)
 * ========================================================================= */

#define FSPI_INT_RX_FULL      BIT(0)
#define FSPI_INT_RX_UNDERFLOW BIT(1)
#define FSPI_INT_TX_OVERFLOW  BIT(2)
#define FSPI_INT_TX_EMPTY     BIT(3)
#define FSPI_INT_TRAN_FINISH  BIT(4)
#define FSPI_INT_BUS_ERR      BIT(5)
#define FSPI_INT_NSPI_ERR     BIT(6)
#define FSPI_INT_DMA          BIT(7)

/* =========================================================================
 * RCVR (Reset) register, offset 0x10
 * ========================================================================= */

#define FSPI_RCVR_RESET BIT(0)

/* =========================================================================
 * FSR (FIFO Status) register, offset 0x20
 * ========================================================================= */

#define FSPI_FSR_TX_FULL    BIT(0)
#define FSPI_FSR_TX_EMPTY   BIT(1)
#define FSPI_FSR_RX_EMPTY   BIT(2)
#define FSPI_FSR_RX_FULL    BIT(3)
#define FSPI_FSR_TXLV_SHIFT 8
#define FSPI_FSR_TXLV_MASK  (0x1f << FSPI_FSR_TXLV_SHIFT)
#define FSPI_FSR_RXLV_SHIFT 16
#define FSPI_FSR_RXLV_MASK  (0x1f << FSPI_FSR_RXLV_SHIFT)

/* =========================================================================
 * SR (FSM Status) register, offset 0x24
 * ========================================================================= */

#define FSPI_SR_IS_IDLE 0x0
#define FSPI_SR_IS_BUSY 0x1

/* =========================================================================
 * EXT_CTRL (Extended Control) register, offset 0x34
 * ========================================================================= */

#define FSPI_SCLK_X2_BYPASS BIT(24)

/* =========================================================================
 * DMA registers (offset 0x80 / 0x84 / 0x88 / 0x8c)
 * ========================================================================= */

#define FSPI_DMA_TRIGGER_START 1
#define FSPI_LEN_CTRL_TRB_SEL  1

/* =========================================================================
 * CMD register bit-fields (offset 0x100)
 * ========================================================================= */

#define FSPI_CMD_OPCODE_SHIFT 0
#define FSPI_CMD_OPCODE_MASK  (0xff << FSPI_CMD_OPCODE_SHIFT)
#define FSPI_CMD_DUMMY_SHIFT  8
#define FSPI_CMD_DUMMY_MASK   (0xf << FSPI_CMD_DUMMY_SHIFT)
#define FSPI_CMD_DIR_SHIFT    12
#define FSPI_CMD_DIR_RD       (0 << FSPI_CMD_DIR_SHIFT)
#define FSPI_CMD_DIR_WR       (1 << FSPI_CMD_DIR_SHIFT)
#define FSPI_CMD_CONT_SHIFT   13 /* continuous mode*/
#define FSPI_CMD_CONT_DISABLE (0 << FSPI_CMD_CONT_SHIFT)
#define FSPI_CMD_CONT_ENABLE  (1 << FSPI_CMD_CONT_SHIFT)
#define FSPI_CMD_ADDR_SHIFT   14
#define FSPI_CMD_ADDR_0BITS   (0 << FSPI_CMD_ADDR_SHIFT)
#define FSPI_CMD_ADDR_24BITS  (1 << FSPI_CMD_ADDR_SHIFT)
#define FSPI_CMD_ADDR_32BITS  (2 << FSPI_CMD_ADDR_SHIFT)
#define FSPI_CMD_ADDR_XBITS   (3 << FSPI_CMD_ADDR_SHIFT)
#define FSPI_CMD_TRB_SHIFT    16
#define FSPI_CMD_TRB_MASK     (0x3fff << FSPI_CMD_TRB_SHIFT)
#define FSPI_CMD_CS_SHIFT     30
#define FSPI_CMD_CS_MASK      (0x3 << FSPI_CMD_CS_SHIFT)

#define FSPI_CMD_CS(cs) \
  (((uint32_t)(cs) << FSPI_CMD_CS_SHIFT) & FSPI_CMD_CS_MASK)

/* =========================================================================
 * FTLR (FIFO Threshold Level) register, offset 0x0C
 * ========================================================================= */

#define FSPI_FTLR_TX_SHIFT 0
#define FSPI_FTLR_TX_MASK  0x1f
#define FSPI_FTLR_RX_SHIFT 8
#define FSPI_FTLR_RX_MASK  (0x1f << FSPI_FTLR_RX_SHIFT)

/* =========================================================================
 * Driver configuration constants
 * ========================================================================= */

#define FSPI_MAX_CHIPSELECT 2
#define FSPI_FIFO_DEPTH     16 /* 16 words (64 bytes) */
#define FSPI_MAX_SPEED_HZ   (150 * 1000 * 1000)
#define FSPI_DMA_THRESHOLD  (0x40)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_FSPI_H */
