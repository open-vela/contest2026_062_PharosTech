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

#include "arm64_gic.h"
#include "arm64_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* UART port identifiers for rk3576_serial_register(). */

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

#define UART_PORT_MAX UART_PORT_11

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

/***************************************************************************
 * Name: rk3576_serial_register
 *
 * Description:
 *   Dynamically allocate and register a UART port.
 *
 *   For UART1~11 (UART_PORT_1 .. UART_PORT_11), this function heap-
 *   allocates the port private data, uart_dev_s, and I/O buffers, then
 *   initializes clocks and registers the device as /dev/ttySx.
 *
 *   For UART0 (UART_PORT_0), the function reuses the statically-
 *   allocated g_uart0priv/g_uart0port (already registered as
 *   /dev/console and /dev/ttyS0 during arm64_serialinit).  It only
 *   initializes the clocks through the NuttX CLK framework so the
 *   framework is aware of the UART0 clock enable state.
 *   All other parameters (baud, bits, parity, stop2, buffer sizes)
 *   are ignored for UART0 — its configuration is fixed at compile
 *   time via Kconfig.
 *
 *   This function uses kmalloc and clk_get(), so it must be called
 *   after the heap and clock tree are available (e.g. from
 *   board_late_initialize).
 *
 * Input Parameters:
 *   port_id       - UART port identifier (UART_PORT_0 ~ UART_PORT_11)
 *   baud          - Baud rate (e.g. 115200)
 *   bits          - Data bits (5, 6, 7, or 8)
 *   parity        - Parity (0=none, 1=odd, 2=even)
 *   stop2         - true = 2 stop bits, false = 1 stop bit
 *   rx_buffer_size - RX buffer size (0 = default 256)
 *   tx_buffer_size - TX buffer size (0 = default 256)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_serial_register(uint8_t port_id, uint32_t baud, uint8_t bits,
                           uint8_t parity, bool stop2, uint16_t rx_buffer_size,
                           uint16_t tx_buffer_size);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H */
