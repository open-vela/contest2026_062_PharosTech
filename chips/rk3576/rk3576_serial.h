/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_serial.h
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

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef CONFIG_ARCH_CHIP_RK3576

/* IRQ for RK3576 UART - see include/rk3576/irq.h (TRM Part1 V1.2 Table 1-3) */

#define RK3576_UART0_IRQ       RK3576_IRQ_UART0
#define RK3576_UART1_IRQ       RK3576_IRQ_UART1
#define RK3576_UART2_IRQ       RK3576_IRQ_UART2
#define RK3576_UART3_IRQ       RK3576_IRQ_UART3
#define RK3576_UART4_IRQ       RK3576_IRQ_UART4

#endif /* CONFIG_ARCH_CHIP_RK3576 */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#endif /* __ASSEMBLY__ */
#define RK3576_UART0_ADDR      0x2AD40000
#define RK3576_UART1_ADDR      0x27310000
#define RK3576_UART2_ADDR      0x2AD50000
#define RK3576_UART3_ADDR      0x2AD60000
#define RK3576_UART4_ADDR      0x2AD70000
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H */
