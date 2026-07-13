/****************************************************************************
 * chips/rk3576/hardware/rk3576_cru.h
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
 * RK3576 Clock & Reset Unit (CRU) hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM, Chapter 5 "Clock & Reset Unit".
 * Cross-checked against mainline drivers/clk/rockchip/clk-rk3576.c.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CRU base address */

#define RK3576_CRU_BASE         RK3576_CRU_ADDR

/* -----------------------------------------------------------------------
 * CRU register offset macros (16-bit hiword-mask write scheme).
 * ----------------------------------------------------------------------- */

/* CLKSEL_CON: clock source select / divider configuration
 * Offset: 0x0300 + n*4  (n = 0 .. N)
 */

#define RK3576_CRU_CLKSEL(n)    (RK3576_CRU_BASE + 0x0300 + ((n) * 4))

/* GATE_CON: clock gating control
 * Offset: 0x0800 + n*4  (n = 0 .. N)
 */

#define RK3576_CRU_GATE(n)      (RK3576_CRU_BASE + 0x0800 + ((n) * 4))

/* SOFTRST_CON: soft reset control
 * Offset: 0x0A00 + n*4  (n = 0 .. N)
 */

#define RK3576_CRU_SOFTRST(n)   (RK3576_CRU_BASE + 0x0A00 + ((n) * 4))

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRU_H */
