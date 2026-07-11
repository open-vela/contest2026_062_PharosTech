/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_serial.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "arm64_internal.h"
#include "arm64_gic.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* UART port identifiers for rk3576_serial_register().
 * UART0 is the console (registered statically) and is intentionally omitted.
 */

#define UART_PORT_0   0
#define UART_PORT_1   1
#define UART_PORT_2   2
#define UART_PORT_3   3
#define UART_PORT_4   4
#define UART_PORT_5   5
#define UART_PORT_6   6
#define UART_PORT_7   7
#define UART_PORT_8   8
#define UART_PORT_9   9
#define UART_PORT_10  10
#define UART_PORT_11  11

#define UART_PORT_MAX  UART_PORT_11

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/


/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_serial_register
 *
 * Description:
 *   Dynamically register a non-console UART port (UART1~11).
 *   UART0 is the console and is registered statically by the chip init
 *   code; do NOT call this function for UART0.
 *
 *   This function uses kmalloc to allocate the port structure and I/O
 *   buffers, so it must be called after the heap is available (e.g. from
 *   board_late_initialize, not from arm64_earlyserialinit).
 *
 * Input Parameters:
 *   port_id - UART port identifier (UART_PORT_1 ~ UART_PORT_11)
 *   baud    - Baud rate (e.g. 115200)
 *   bits    - Data bits (5, 6, 7, or 8)
 *   parity  - Parity (0=none, 1=odd, 2=even)
 *   stop2   - true = 2 stop bits, false = 1 stop bit
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_serial_register(uint8_t port_id, uint32_t baud,
                           uint8_t bits, uint8_t parity, bool stop2);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H */
