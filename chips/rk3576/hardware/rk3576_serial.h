/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_serial.h
 *
 *  SPDX-License-Identifier: Apache-2.0
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

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_SERIAL_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_SERIAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* UART Register Offsets (DW 16550) ***************************************/

#define RK3576_UART_THR_OFFSET 0x00 /* Tx Holding Register */
#define RK3576_UART_RBR_OFFSET 0x00 /* Rx Buffer Register */
#define RK3576_UART_DLL_OFFSET 0x00 /* Divisor Latch Low (DLAB=1) */
#define RK3576_UART_DLH_OFFSET 0x04 /* Divisor Latch High (DLAB=1) */
#define RK3576_UART_IER_OFFSET 0x04 /* Interrupt Enable Register */
#define RK3576_UART_IIR_OFFSET 0x08 /* Interrupt Identity Register */
#define RK3576_UART_FCR_OFFSET 0x08 /* FIFO Control Register */
#define RK3576_UART_LCR_OFFSET 0x0c /* Line Control Register */
#define RK3576_UART_LSR_OFFSET 0x14 /* Line Status Register */
#define RK3576_UART_MSR_OFFSET 0x18 /* Modem Status Register */
#define RK3576_UART_USR_OFFSET 0x7c /* UART Status Register (DW-specific) */

/* UART Register Address Macros (base + offset) ***************************/

#define RK3576_UART_THR(base) ((base) + RK3576_UART_THR_OFFSET)
#define RK3576_UART_RBR(base) ((base) + RK3576_UART_RBR_OFFSET)
#define RK3576_UART_DLL(base) ((base) + RK3576_UART_DLL_OFFSET)
#define RK3576_UART_DLH(base) ((base) + RK3576_UART_DLH_OFFSET)
#define RK3576_UART_IER(base) ((base) + RK3576_UART_IER_OFFSET)
#define RK3576_UART_IIR(base) ((base) + RK3576_UART_IIR_OFFSET)
#define RK3576_UART_FCR(base) ((base) + RK3576_UART_FCR_OFFSET)
#define RK3576_UART_LCR(base) ((base) + RK3576_UART_LCR_OFFSET)
#define RK3576_UART_LSR(base) ((base) + RK3576_UART_LSR_OFFSET)
#define RK3576_UART_MSR(base) ((base) + RK3576_UART_MSR_OFFSET)
#define RK3576_UART_USR(base) ((base) + RK3576_UART_USR_OFFSET)

/* IER – Interrupt Enable Register ****************************************/

#define RK3576_UART_IER_ERBFI \
  (1 << 0) /* Enable Received Data Available Interrupt */
#define RK3576_UART_IER_ETBEI \
  (1 << 1) /* Enable Transmit Holding Register Empty Interrupt */
#define RK3576_UART_IER_ELSI \
  (1 << 2) /* Enable Receiver Line Status Interrupt */
#define RK3576_UART_IER_EDSSI (1 << 3) /* Enable Modem Status Interrupt */

/* IIR – Interrupt Identity Register **************************************/

#define RK3576_UART_IIR_IID_SHIFT 0
#define RK3576_UART_IIR_IID_MASK  (15 << RK3576_UART_IIR_IID_SHIFT)
#define RK3576_UART_IIR_IID_MODEM \
  (0 << RK3576_UART_IIR_IID_SHIFT) /* Modem status */
#define RK3576_UART_IIR_IID_NONE \
  (1 << RK3576_UART_IIR_IID_SHIFT) /* No interrupt pending */
#define RK3576_UART_IIR_IID_TXEMPTY \
  (2 << RK3576_UART_IIR_IID_SHIFT) /* THR empty */
#define RK3576_UART_IIR_IID_RECV \
  (4 << RK3576_UART_IIR_IID_SHIFT) /* Received data available */
#define RK3576_UART_IIR_IID_LINESTATUS \
  (6 << RK3576_UART_IIR_IID_SHIFT) /* Receiver line status */
#define RK3576_UART_IIR_IID_BUSY \
  (7 << RK3576_UART_IIR_IID_SHIFT) /* Busy detect */
#define RK3576_UART_IIR_IID_TIMEOUT \
  (12 << RK3576_UART_IIR_IID_SHIFT) /* Character timeout */

#define RK3576_UART_IIR_FEFLAG_SHIFT   6
#define RK3576_UART_IIR_FEFLAG_MASK    (3 << RK3576_UART_IIR_FEFLAG_SHIFT)
#define RK3576_UART_IIR_FEFLAG_DISABLE (0 << RK3576_UART_IIR_FEFLAG_SHIFT)
#define RK3576_UART_IIR_FEFLAG_ENABLE  (3 << RK3576_UART_IIR_FEFLAG_SHIFT)

/* FCR – FIFO Control Register ********************************************/

#define RK3576_UART_FCR_FIFOE     (1 << 0) /* FIFO Enable */
#define RK3576_UART_FCR_RFIFOR    (1 << 1) /* RCVR FIFO Reset */
#define RK3576_UART_FCR_XFIFOR    (1 << 2) /* XMIT FIFO Reset */
#define RK3576_UART_FCR_DMAM      (1 << 3) /* DMA Mode */
#define RK3576_UART_FCR_TFT_SHIFT 4        /* TX Empty Trigger */
#define RK3576_UART_FCR_TFT_MASK  (3 << RK3576_UART_FCR_TFT_SHIFT)
#define RK3576_UART_FCR_TFT_EMPTY \
  (0 << RK3576_UART_FCR_TFT_SHIFT) /* FIFO empty */
#define RK3576_UART_FCR_TFT_TWO \
  (1 << RK3576_UART_FCR_TFT_SHIFT) /* 2 chars in FIFO */
#define RK3576_UART_FCR_TFT_QUARTER \
  (2 << RK3576_UART_FCR_TFT_SHIFT) /* FIFO 1/4 full */
#define RK3576_UART_FCR_TFT_HALF \
  (3 << RK3576_UART_FCR_TFT_SHIFT) /* FIFO 1/2 full */

#define RK3576_UART_FCR_RT_SHIFT 6 /* RCVR Trigger */
#define RK3576_UART_FCR_RT_MASK  (3 << RK3576_UART_FCR_RT_SHIFT)
#define RK3576_UART_FCR_RT_ONE \
  (0 << RK3576_UART_FCR_RT_SHIFT) /* 1 char in FIFO */
#define RK3576_UART_FCR_RT_QUARTER \
  (1 << RK3576_UART_FCR_RT_SHIFT) /* FIFO 1/4 full */
#define RK3576_UART_FCR_RT_HALF \
  (2 << RK3576_UART_FCR_RT_SHIFT) /* FIFO 1/2 full */
#define RK3576_UART_FCR_RT_MINUS2 \
  (3 << RK3576_UART_FCR_RT_SHIFT) /* FIFO-2 less than full */

/* LCR – Line Control Register ********************************************/

#define RK3576_UART_LCR_DLS_SHIFT 0
#define RK3576_UART_LCR_DLS_MASK  (3 << RK3576_UART_LCR_DLS_SHIFT)
#define RK3576_UART_LCR_DLS_5BITS            \
  (0 << RK3576_UART_LCR_DLS_SHIFT) /* 5 bits \
                                    */
#define RK3576_UART_LCR_DLS_6BITS            \
  (1 << RK3576_UART_LCR_DLS_SHIFT) /* 6 bits \
                                    */
#define RK3576_UART_LCR_DLS_7BITS            \
  (2 << RK3576_UART_LCR_DLS_SHIFT) /* 7 bits \
                                    */
#define RK3576_UART_LCR_DLS_8BITS            \
  (3 << RK3576_UART_LCR_DLS_SHIFT) /* 8 bits \
                                    */

#define RK3576_UART_LCR_STOP (1 << 2) /* Number of stop bits */
#define RK3576_UART_LCR_PEN  (1 << 3) /* Parity Enable */
#define RK3576_UART_LCR_EPS  (1 << 4) /* Even Parity Select */
#define RK3576_UART_LCR_BC   (1 << 6) /* Break Control Bit */
#define RK3576_UART_LCR_DLAB (1 << 7) /* Divisor Latch Access Bit */

/* LSR – Line Status Register *********************************************/

#define RK3576_UART_LSR_DR   (1 << 0) /* Data Ready */
#define RK3576_UART_LSR_THRE (1 << 5) /* Transmit Holding Register Empty */

/* USR – UART Status Register (DesignWare-specific) ***********************/

#define RK3576_UART_USR_BUSY (1 << 0) /* UART Busy */

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_SERIAL_H */
