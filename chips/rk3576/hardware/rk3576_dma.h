/****************************************************************************
 * chips/rk3576/hardware/rk3576_dma.h
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
 * The RK3576 DMA controllers (dmac0/1/2) are standard ARM PL330 (DMA-330)
 * micro-coded DMA engines.  The register map and the micro-code instruction
 * set below follow "ARM DDI 0424 DMA-330 Technical Reference Manual".  Only
 * the register block (offsets) and micro-code opcodes are chip independent;
 * the base addresses and peripheral-request wiring are RK3576 specific and
 * live in rk3576_memorymap.h / the driver.
 *
 * A PL330 channel has no simple SAR/DAR/count registers to program; instead
 * the CPU assembles a small micro-code program in DMA-visible memory and
 * launches it via the debug interface (DBGINST0/1 + DBGCMD).  The opcode and
 * CCR (channel control) definitions here are what the assembler in
 * rk3576_dma.c emits.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DMA_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DMA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller geometry (PL330 configured on RK3576) ***********************/

#define RK3576_DMA_NCHANNELS       8    /* CR0.num_chnls (0..7)            */
#define RK3576_DMA_NEVENTS         8    /* CR1.num_events / irq lines      */
#define RK3576_DMA_NPERIPH         32   /* CRD.num_periph_req              */

/* Register offsets from a controller base (ARM DDI 0424) *****************/

#define RK3576_DMA_DSR             0x000  /* DMA manager status            */
#define RK3576_DMA_DPC             0x004  /* DMA manager program counter   */
#define RK3576_DMA_INTEN           0x020  /* Interrupt enable              */
#define RK3576_DMA_INT_EVENT_RIS   0x024  /* Event/irq raw status          */
#define RK3576_DMA_INTMIS          0x028  /* Interrupt masked status       */
#define RK3576_DMA_INTCLR          0x02c  /* Interrupt clear               */
#define RK3576_DMA_FSRD            0x030  /* Fault status DMA manager      */
#define RK3576_DMA_FSRC            0x034  /* Fault status DMA channels     */
#define RK3576_DMA_FTRD            0x038  /* Fault type DMA manager        */
#define RK3576_DMA_FTR(n)          (0x040 + ((n) << 2)) /* Fault type ch n */

#define RK3576_DMA_CSR(n)          (0x100 + ((n) << 3)) /* Channel status  */
#define RK3576_DMA_CPC(n)          (0x104 + ((n) << 3)) /* Channel PC      */

#define RK3576_DMA_SAR(n)          (0x400 + (n) * 0x20) /* Source addr     */
#define RK3576_DMA_DAR(n)          (0x404 + (n) * 0x20) /* Dest addr       */
#define RK3576_DMA_CCR(n)          (0x408 + (n) * 0x20) /* Channel control */
#define RK3576_DMA_LC0(n)          (0x40c + (n) * 0x20) /* Loop counter 0  */
#define RK3576_DMA_LC1(n)          (0x410 + (n) * 0x20) /* Loop counter 1  */

#define RK3576_DMA_DBGSTATUS       0xd00  /* Debug status                  */
#define RK3576_DMA_DBGCMD          0xd04  /* Debug command                 */
#define RK3576_DMA_DBGINST0        0xd08  /* Debug instruction 0           */
#define RK3576_DMA_DBGINST1        0xd0c  /* Debug instruction 1           */

#define RK3576_DMA_CR0             0xe00  /* Configuration register 0      */
#define RK3576_DMA_CR1             0xe04  /* Configuration register 1      */
#define RK3576_DMA_CR2             0xe08  /* Configuration register 2      */
#define RK3576_DMA_CR3             0xe0c  /* Configuration register 3      */
#define RK3576_DMA_CR4             0xe10  /* Configuration register 4      */
#define RK3576_DMA_CRD             0xe14  /* Configuration register DMA    */

/* DSR - DMA manager status ***********************************************/

#define DMA_DSR_STATUS_SHIFT       0
#define DMA_DSR_STATUS_MASK        (0xf << DMA_DSR_STATUS_SHIFT)
#  define DMA_DSR_STOPPED          0x0
#  define DMA_DSR_EXECUTING        0x1
#  define DMA_DSR_FAULTING         0xf

/* CSRn - channel status (low nibble) ************************************/

#define DMA_CSR_STATUS_SHIFT       0
#define DMA_CSR_STATUS_MASK        (0xf << DMA_CSR_STATUS_SHIFT)
#  define DMA_CSR_STOPPED          0x0
#  define DMA_CSR_EXECUTING        0x1
#  define DMA_CSR_FAULTING         0xe
#  define DMA_CSR_FAULTCOMPLETE    0xf

/* DBGSTATUS - debug status **********************************************/

#define DMA_DBGSTATUS_BUSY         (1 << 0) /* 1 = debug instr in progress */

/* DBGCMD - debug command ************************************************/

#define DMA_DBGCMD_EXECUTE         0x0      /* Execute the DBGINST0/1 pair */

/* DBGINST0 - debug instruction word 0 **********************************/

#define DMA_DBGINST0_THREAD_MGR    0        /* bit0 = 0: DMA manager       */
#define DMA_DBGINST0_THREAD_CHAN   1        /* bit0 = 1: DMA channel       */
#define DMA_DBGINST0_CHAN_SHIFT    8        /* bits[10:8] channel number   */
#define DMA_DBGINST0_INSB0_SHIFT   16       /* bits[23:16] instr byte0     */
#define DMA_DBGINST0_INSB1_SHIFT   24       /* bits[31:24] instr byte1     */

/* CR0 - configuration ***************************************************/

#define DMA_CR0_PERIPH_REQ         (1 << 0)  /* Peripheral request i/f     */
#define DMA_CR0_NUM_CHNLS_SHIFT    4
#define DMA_CR0_NUM_CHNLS_MASK     (0x7 << DMA_CR0_NUM_CHNLS_SHIFT)
#define DMA_CR0_NUM_PERIPH_SHIFT   12
#define DMA_CR0_NUM_PERIPH_MASK    (0x1f << DMA_CR0_NUM_PERIPH_SHIFT)

/* CCR - channel control register value (built with DMAMOV CCR) *********
 *
 * This is the 32-bit value moved into the channel's CCR by the DMAMOV
 * micro-code instruction, not a memory-mapped register write.  Layout per
 * ARM DDI 0424:
 *   [0]     src_inc            [14]    dst_inc
 *   [3:1]   src_burst_size     [17:15] dst_burst_size   (log2 bytes)
 *   [7:4]   src_burst_len      [21:18] dst_burst_len    (beats - 1)
 *   [8]     src_prot privileged[22]    dst_prot privileged
 *   [9]     src_prot ns        [23]    dst_prot ns
 *   [10]    src_prot alloc     [24]    dst_prot alloc
 *   [13:11] src_cache_ctrl     [27:25] dst_cache_ctrl
 *   [30:28] endian_swap_size
 */

#define DMA_CCR_SRC_INC            (1 << 0)
#define DMA_CCR_SRC_BURSTSIZE_SHIFT 1
#define DMA_CCR_SRC_BURSTLEN_SHIFT  4
#define DMA_CCR_SRC_PROT_PRIV      (1 << 8)
#define DMA_CCR_SRC_PROT_NS        (1 << 9)
#define DMA_CCR_SRC_PROT_ALLOC     (1 << 10)
#define DMA_CCR_SRC_CACHE_SHIFT    11
#define DMA_CCR_DST_INC            (1 << 14)
#define DMA_CCR_DST_BURSTSIZE_SHIFT 15
#define DMA_CCR_DST_BURSTLEN_SHIFT  18
#define DMA_CCR_DST_PROT_PRIV      (1 << 22)
#define DMA_CCR_DST_PROT_NS        (1 << 23)
#define DMA_CCR_DST_PROT_ALLOC     (1 << 24)
#define DMA_CCR_DST_CACHE_SHIFT    25
#define DMA_CCR_ENDIAN_SWAP_SHIFT  28

/* PL330 micro-code opcodes (ARM DDI 0424) ******************************
 *
 * The low bits of several opcodes carry a condition/qualifier:
 *   LD/ST : bit0 = 1 -> conditional, bit1 = 0 single / 1 burst
 *   WFP   : 00 = single, 10 = burst, 01 = peripheral (peri decides)
 *   LPEND : bit0/1 as LD/ST, bit2 = loop-counter select, bit4 = nf(finite)
 */

#define DMA_OP_DMAEND              0x00     /* 1 byte                       */
#define DMA_OP_DMAKILL             0x01     /* 1 byte (debug only)          */
#define DMA_OP_DMALD               0x04     /* 1 byte, |1 cond |2 burst     */
#define DMA_OP_DMALDB              0x07     /* DMALD burst                  */
#define DMA_OP_DMALDS              0x05     /* DMALD single                 */
#define DMA_OP_DMAST               0x08     /* 1 byte, |1 cond |2 burst     */
#define DMA_OP_DMASTB              0x0b     /* DMAST burst                  */
#define DMA_OP_DMASTS              0x09     /* DMAST single                 */
#define DMA_OP_DMANOP              0x18     /* 1 byte                       */
#define DMA_OP_DMALP0              0x20     /* 2 bytes, loop counter 0      */
#define DMA_OP_DMALP1              0x22     /* 2 bytes, loop counter 1      */
#define DMA_OP_DMALDP              0x25     /* 2 bytes, LD + notify periph  */
#define DMA_OP_DMALPEND            0x28     /* 2 bytes, base opcode         */
#define DMA_OP_DMASTP              0x29     /* 2 bytes, ST + notify periph  */
#define DMA_OP_DMAWFP              0x30     /* 2 bytes, |1 peri |2 burst    */
#define DMA_OP_DMASEV              0x34     /* 2 bytes                      */
#define DMA_OP_DMAFLUSHP           0x35     /* 2 bytes                      */
#define DMA_OP_DMAMOV              0xbc     /* 6 bytes: op, rd, imm32       */
#define DMA_OP_DMAGO               0xa0     /* 6 bytes: op|ns<<1, cn, imm32 */

/* DMALPEND qualifier bits */

#define DMA_LPEND_NF               (1 << 4) /* Finite loop (not forever)    */
#define DMA_LPEND_LC1              (1 << 2) /* Use loop counter 1           */

/* DMAWFP qualifier bits */

#define DMA_WFP_BURST              (1 << 1) /* Wait for burst request       */
#define DMA_WFP_PERIPH             (1 << 0) /* Peripheral decides s/b       */

/* DMAGO qualifier bit */

#define DMA_GO_NS                  (1 << 1) /* Launch in non-secure state   */

/* DMAMOV destination register selector (2nd byte) */

#define DMA_MOV_SAR                0
#define DMA_MOV_CCR                1
#define DMA_MOV_DAR                2

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DMA_H */
