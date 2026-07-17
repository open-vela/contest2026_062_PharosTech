/****************************************************************************
 * chips/rk3576/hardware/rk3576_memorymap.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Fixed crystal oscillator, source of the ARM generic timer */

#define RK3576_OSC_FREQ 24000000

/* GPIO banks (TRM) */

#define RK3576_GPIO0_ADDR 0x27320000
#define RK3576_GPIO1_ADDR 0x2AE10000
#define RK3576_GPIO2_ADDR 0x2AE20000
#define RK3576_GPIO3_ADDR 0x2AE30000
#define RK3576_GPIO4_ADDR 0x2AE40000
#define RK3576_PIO_ADDR   RK3576_GPIO0_ADDR

/* DesignWare 16550 UARTs (TRM). UART0 = debug console (vendor DTS earlycon).
 */

#define RK3576_UART0_ADDR  0x2AD40000
#define RK3576_UART1_ADDR  0x27310000
#define RK3576_UART2_ADDR  0x2AD50000
#define RK3576_UART3_ADDR  0x2AD60000
#define RK3576_UART4_ADDR  0x2AD70000
#define RK3576_UART5_ADDR  0x2AD80000
#define RK3576_UART6_ADDR  0x2AD90000
#define RK3576_UART7_ADDR  0x2ADA0000
#define RK3576_UART8_ADDR  0x2ADB0000
#define RK3576_UART9_ADDR  0x2ADC0000
#define RK3576_UART10_ADDR 0x2AFC0000
#define RK3576_UART11_ADDR 0x2AFD0000

/* Synopsys DesignWare MSHC (dw_mmc, same IP as rk3288/rk3399) */

#define RK3576_SDMMC_ADDR 0x2A310000 /* SD/MMC host (dw-mshc) */
#define RK3576_SDIO_ADDR  0x2A320000 /* SDIO host (dw-mshc)   */
#define RK3576_EMMC_ADDR  0x2A330000 /* eMMC host (dwcmshc)   */

/* I2C controller (Synopsys/Rockchip RK I2C, "rk3399-i2c" compatible). */

#define RK3576_I2C0_ADDR 0x27300000
#define RK3576_I2C1_ADDR 0x2ac40000
#define RK3576_I2C2_ADDR 0x2ac50000
#define RK3576_I2C3_ADDR 0x2ac60000
#define RK3576_I2C4_ADDR 0x2ac70000
#define RK3576_I2C5_ADDR 0x2ac80000
#define RK3576_I2C6_ADDR 0x2ac90000
#define RK3576_I2C7_ADDR 0x2aca0000
#define RK3576_I2C8_ADDR 0x2acb0000
#define RK3576_I2C9_ADDR 0x2ae80000

/* Clock & Reset Unit */

#define RK3576_CRU_ADDR         0x27200000
#define RK3576_PPLL_CRU_ADDR    0x27208000
#define RK3576_SECURE_CRU_ADDR  0x27210000
#define RK3576_PMU1_CRU_ADDR    0x27220000
#define RK3576_DDR0_CRU_ADDR    0x27228000
#define RK3576_DDR1_CRU_ADDR    0x27230000
#define RK3576_BIGCORE_CRU_ADDR 0x27238000
#define RK3576_LITCORE_CRU_ADDR 0x27240000
#define RK3576_CCI_CRU_ADDR     0x27248000

/* IOMUX */
#define RK3576_IOC_ADDR 0x26040000

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H */
