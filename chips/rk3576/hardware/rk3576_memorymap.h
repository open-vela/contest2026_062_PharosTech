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

/* PWM (Rockchip PWM v4) */
#define RK3576_PWM0_ADDR 0x27330000
#define RK3576_PWM1_ADDR 0x2ADD0000
#define RK3576_PWM2_ADDR 0x2ADE0000

/* Synopsys DesignWare MSHC (dw_mmc, same IP as rk3288/rk3399) */

#define RK3576_SDMMC_ADDR 0x2A310000 /* SD/MMC host (dw-mshc) */
#define RK3576_SDIO_ADDR  0x2A320000 /* SDIO host (dw-mshc)   */
#define RK3576_EMMC_ADDR  0x2A330000 /* eMMC host (dwcmshc)   */

/* USB OTG (Synopsys DesignWare USB3 / DWC3) */

#define RK3576_USB0_ADDR 0x23000000 /* USB OTG0 (DWC3)       */

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

/* Serial Audio Interface controllers. */

#define RK3576_SAI0_ADDR 0x2a600000
#define RK3576_SAI1_ADDR 0x2a610000
#define RK3576_SAI2_ADDR 0x2a620000
#define RK3576_SAI3_ADDR 0x2a630000
#define RK3576_SAI4_ADDR 0x2a640000
#define RK3576_SAI5_ADDR 0x27d40000
#define RK3576_SAI6_ADDR 0x27d50000
#define RK3576_SAI7_ADDR 0x27ed0000
#define RK3576_SAI8_ADDR 0x27ee0000
#define RK3576_SAI9_ADDR 0x27ef0000

/* Rockchip FSPI (Flexible Serial Peripheral Interface) */

#define RK3576_FSPI0_ADDR 0x2A340000
#define RK3576_FSPI1_ADDR 0x2A300000

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

/* Generic programmable interval timers (TRM).  Only the non-secure NS
 * instances are exported; each block has 6 independent channels at a
 * 0x1000 stride (see rk3576_timer.h).
 */

#define RK3576_TIMER_NS0_ADDR 0x2ACC0000
#define RK3576_TIMER_NS1_ADDR 0x2ACD0000

/* High precision timer */
#define RK3576_HPTIMER_ADDR 0x27400000

/* Temperature-Sensor ADC (TS-ADC), TRM Chapter 19 */

#define RK3576_TSADC_ADDR 0x2AE70000

/* IOMUX */
#define RK3576_IOC_ADDR 0x26040000

/* DMA controller (three PL330 instances, non-secure bases) ***************/

#define RK3576_DMAC0_ADDR 0x2ab90000
#define RK3576_DMAC1_ADDR 0x2abb0000
#define RK3576_DMAC2_ADDR 0x2abd0000

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H */
