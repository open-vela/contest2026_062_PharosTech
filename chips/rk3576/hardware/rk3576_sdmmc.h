/****************************************************************************
 * chips/rk3576/hardware/rk3576_sdmmc.h
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
 * RK3576 SDMMC/SDIO controller registers -- Synopsys DesignWare MSHC
 * (dw_mmc).  Same IP as rk3288/rk3399 with the standard register layout;
 * see Linux drivers/mmc/host/dw_mmc.h for reference.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDMMC_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDMMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "rk3576_memorymap.h"
#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (DW MSHC) ********************************************/

#define RK3576_SDMMC_CTRL    0x000 /* Control */
#define RK3576_SDMMC_PWREN   0x004 /* Power enable */
#define RK3576_SDMMC_CLKDIV  0x008 /* Clock divider */
#define RK3576_SDMMC_CLKSRC  0x00c /* Clock source */
#define RK3576_SDMMC_CLKENA  0x010 /* Clock enable */
#define RK3576_SDMMC_TMOUT   0x014 /* Timeout */
#define RK3576_SDMMC_CTYPE   0x018 /* Card bus width */
#define RK3576_SDMMC_BLKSIZ  0x01c /* Block size */
#define RK3576_SDMMC_BYTCNT  0x020 /* Byte count */
#define RK3576_SDMMC_INTMASK 0x024 /* Interrupt mask */
#define RK3576_SDMMC_CMDARG  0x028 /* Command argument */
#define RK3576_SDMMC_CMD     0x02c /* Command */
#define RK3576_SDMMC_RESP0   0x030 /* Response 0 */
#define RK3576_SDMMC_RESP1   0x034 /* Response 1 */
#define RK3576_SDMMC_RESP2   0x038 /* Response 2 */
#define RK3576_SDMMC_RESP3   0x03c /* Response 3 */
#define RK3576_SDMMC_MINTSTS 0x040 /* Masked interrupt status */
#define RK3576_SDMMC_RINTSTS \
  0x044                           /* Raw interrupt status (write 1 to clear) */
#define RK3576_SDMMC_STATUS 0x048 /* Status (FIFO/busy) */
#define RK3576_SDMMC_FIFOTH 0x04c /* FIFO threshold */
#define RK3576_SDMMC_CDETECT \
  0x050 /* Card detect (bit0=0 means card present) */
#define RK3576_SDMMC_WRTPRT     0x054 /* Write protect */
#define RK3576_SDMMC_TCBCNT     0x05c /* Transferred card byte count */
#define RK3576_SDMMC_TBBCNT     0x060 /* Transferred host (FIFO) byte count */
#define RK3576_SDMMC_DEBNCE     0x064 /* Debounce */
#define RK3576_SDMMC_USRID      0x068 /* User ID */
#define RK3576_SDMMC_VERID      0x06c /* Version ID */
#define RK3576_SDMMC_HCON       0x070 /* Hardware configuration */
#define RK3576_SDMMC_UHS_REG    0x074 /* UHS-1 */
#define RK3576_SDMMC_RST_N      0x078 /* Card hardware reset */
#define RK3576_SDMMC_BMOD       0x080 /* Internal DMA bus mode */
#define RK3576_SDMMC_DBADDR     0x088 /* Descriptor base address */
#define RK3576_SDMMC_IDSTS      0x08c /* IDMAC status */
#define RK3576_SDMMC_IDINTEN    0x090 /* IDMAC interrupt enable */
#define RK3576_SDMMC_CARDTHRCTL 0x100 /* Card read threshold */

/* Data FIFO: on RK, dw_mmc places it at 0x200 (determined by HCON,
 * RK3288/3399/3576 = 0x200)
 */

#define RK3576_SDMMC_DATA 0x200

/* CTRL bits ************************************************************/

#define SDMMC_CTRL_RESET      (1 << 0)  /* Controller reset */
#define SDMMC_CTRL_FIFO_RESET (1 << 1)  /* FIFO reset */
#define SDMMC_CTRL_DMA_RESET  (1 << 2)  /* DMA reset */
#define SDMMC_CTRL_INT_ENABLE (1 << 4)  /* Global interrupt enable */
#define SDMMC_CTRL_USE_IDMAC  (1 << 25) /* Use internal DMA */
#define SDMMC_CTRL_RESET_ALL \
  (SDMMC_CTRL_RESET | SDMMC_CTRL_FIFO_RESET | SDMMC_CTRL_DMA_RESET)

/* CMD bits *************************************************************/

#define SDMMC_CMD_INDEX_SHIFT   0
#define SDMMC_CMD_RESP_EXPECT   (1 << 6)  /* Response expected */
#define SDMMC_CMD_RESP_LONG     (1 << 7)  /* Long response (R2) */
#define SDMMC_CMD_RESP_CRC      (1 << 8)  /* Check response CRC */
#define SDMMC_CMD_DATA_EXPECTED (1 << 9)  /* Data transfer present */
#define SDMMC_CMD_WRITE         (1 << 10) /* Write direction (0=read) */
#define SDMMC_CMD_SEND_STOP     (1 << 12) /* Send auto STOP */
#define SDMMC_CMD_WAIT_PRV      (1 << 13) /* Wait for previous data to complete */
#define SDMMC_CMD_INIT          (1 << 15) /* Send 80-clock initialization sequence */
#define SDMMC_CMD_UPD_CLK       (1 << 21) /* Update clock registers only */
#define SDMMC_CMD_USE_HOLD_REG  (1 << 29) /* Use hold register */
#define SDMMC_CMD_START         (1 << 31) /* Start command (hardware self-clears) */

/* RINTSTS / INTMASK bits **********************************************/

#define SDMMC_INT_CD       (1 << 0)  /* Card detect */
#define SDMMC_INT_RE       (1 << 1)  /* Response error */
#define SDMMC_INT_CMD_DONE (1 << 2)  /* Command done */
#define SDMMC_INT_DTO      (1 << 3)  /* Data transfer over */
#define SDMMC_INT_TXDR     (1 << 4)  /* Transmit FIFO data request */
#define SDMMC_INT_RXDR     (1 << 5)  /* Receive FIFO data request */
#define SDMMC_INT_RCRC     (1 << 6)  /* Response CRC error */
#define SDMMC_INT_DCRC     (1 << 7)  /* Data CRC error */
#define SDMMC_INT_RTO      (1 << 8)  /* Response timeout */
#define SDMMC_INT_DRTO     (1 << 9)  /* Data read timeout */
#define SDMMC_INT_HTO      (1 << 10) /* Data starvation-by-host timeout */
#define SDMMC_INT_FRUN     (1 << 11) /* FIFO overrun/underrun */
#define SDMMC_INT_HLE      (1 << 12) /* Hardware locked write error */
#define SDMMC_INT_SBE      (1 << 13) /* Start bit error */
#define SDMMC_INT_ACD      (1 << 14) /* Auto command done */
#define SDMMC_INT_EBE      (1 << 15) /* End bit error */
#define SDMMC_INT_SDIO     (1 << 16) /* SDIO card interrupt (function 1) */

#define SDMMC_INT_ALL      0xffffffff

/* STATUS bits *********************************************************/

#define SDMMC_STATUS_FIFO_EMPTY (1 << 2)
#define SDMMC_STATUS_FIFO_FULL  (1 << 3)
#define SDMMC_STATUS_DATA_BUSY  (1 << 9) /* card_data_busy */

/* FIFOTH field shifts (DW-MSHC: [30:28]=DW_MSIZE, [27:16]=RX_WMARK,
 * [11:0]=TX_WMARK)
 */

#define SDMMC_FIFOTH_MSIZE_SHIFT   28
#define SDMMC_FIFOTH_RXWMARK_SHIFT 16
#define SDMMC_FIFOTH_TXWMARK_SHIFT 0

/* CTYPE bits (bus width) **********************************************/

#define SDMMC_CTYPE_1BIT 0
#define SDMMC_CTYPE_4BIT (1 << 0)
#define SDMMC_CTYPE_8BIT (1 << 16)

/* CLKENA bits *********************************************************/

#define SDMMC_CLKENA_ENABLE (1 << 0)  /* Card clock enable */
#define SDMMC_CLKENA_LOWPWR (1 << 16) /* Low power (stop clock when idle) */

/* CDETECT: bit0 = 0 means a card is inserted (bit0 = 1 means no card) */

#define SDMMC_CDETECT_NOCARD (1 << 0)

/* WRTPRT: bit0 = 1 means the card is write-protected */

#define SDMMC_WRTPRT_PROTECTED (1 << 0)

/* ===== IDMAC (internal DMA) definitions -- see Linux
 * drivers/mmc/host/dw_mmc.h =====
 *
 * The DW-MSHC internal DMA controller (IDMAC) uses 32-bit chained
 * descriptors.  The bit definitions below strictly follow the
 * IDMAC_DES0_* / IDMAC_INT_* / BMOD fields in Linux dw_mmc.h; do not
 * change them from memory.
 */

/* BMOD (0x080) bus mode */

#define SDMMC_IDMAC_SWRESET (1 << 0) /* Software reset of IDMAC */
#define SDMMC_IDMAC_FB      (1 << 1) /* Fixed burst */
#define SDMMC_IDMAC_ENABLE  (1 << 7) /* DE: enable internal DMA */

/* IDSTS (0x08c) / IDINTEN (0x090) bits -- identical bit layout */

#define SDMMC_IDMAC_INT_TI  (1 << 0) /* Transmit interrupt (done) */
#define SDMMC_IDMAC_INT_RI  (1 << 1) /* Receive interrupt (done) */
#define SDMMC_IDMAC_INT_FBE (1 << 2) /* Fatal bus error */
#define SDMMC_IDMAC_INT_DU  (1 << 4) /* Descriptor unavailable */
#define SDMMC_IDMAC_INT_CES (1 << 5) /* Card error summary */
#define SDMMC_IDMAC_INT_NI  (1 << 8) /* Normal interrupt summary */
#define SDMMC_IDMAC_INT_AI  (1 << 9) /* Abnormal interrupt summary */
#define SDMMC_IDMAC_INT_ALL 0x337    /* All status bits (write 1 to clear) */

#define SDMMC_IDMAC_INT_ERR \
  (SDMMC_IDMAC_INT_FBE | SDMMC_IDMAC_INT_DU | SDMMC_IDMAC_INT_CES)

/* Enabled IDMAC interrupts: receive/transmit done + all errors + summaries */

#define SDMMC_IDMAC_INT_ENA                                        \
  (SDMMC_IDMAC_INT_TI | SDMMC_IDMAC_INT_RI | SDMMC_IDMAC_INT_FBE | \
   SDMMC_IDMAC_INT_DU | SDMMC_IDMAC_INT_CES | SDMMC_IDMAC_INT_NI | \
   SDMMC_IDMAC_INT_AI)

/* 32-bit chained descriptor DES0 bits (Linux IDMAC_DES0_*) */

#define IDMAC_DES0_DIC (1 << 1) /* Disable interrupt on completion */
#define IDMAC_DES0_LD  (1 << 2) /* Last descriptor */
#define IDMAC_DES0_FD  (1 << 3) /* First descriptor */
#define IDMAC_DES0_CH \
  (1 << 4) /* Chained (second address points to next descriptor) */
#define IDMAC_DES0_ER  (1 << 5)   /* End of ring */
#define IDMAC_DES0_CES (1 << 30)  /* Card error summary */
#define IDMAC_DES0_OWN (1u << 31) /* Ownership held by DMA */

/* DES1 buffer1 size field (bits 12:0), max 8191B per descriptor; this
 * driver uses 4KB per descriptor
 */

#define IDMAC_DES1_BS_MASK 0x1fff
#define IDMAC_DES1_BS1(x)  ((x)&IDMAC_DES1_BS_MASK)

/* Bytes moved per descriptor: 4KB (page-aligned, safely < 8191) */

#define RK3576_IDMAC_BUFSZ 4096

/* Upper bound for 32-bit IDMAC descriptor addresses (buffer must stay below
 * the low 4G boundary)
 */

#define RK3576_IDMAC_DMA_LIMIT 0x100000000ull

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 32-bit chained descriptor structure */

struct rk3576_sdmmc_idmac_desc_s
{
  uint32_t des0; /* Control/status bits */
  uint32_t des1; /* buffer1 byte count (BS1) */
  uint32_t des2; /* buffer1 physical address */
  uint32_t des3; /* Chained mode: physical address of next descriptor */
};

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDMMC_H */
