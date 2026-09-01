/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_wdt.h
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
 * The RK3576 WDT (TRM Part1, Chapter 15) is an APB slave peripheral with a
 * 32-bit down-counter.  The counter counts down from a preset timeout value
 * to zero; reaching zero drives either a system reset or (optionally) an
 * interrupt followed by a reset.  The watchdog is kicked by writing the key
 * 0x76 to WDT_CRR.
 *
 * Six instances exist, each with an independent 0x18 register block:
 *
 *   PMU_WDT (0x27340000) - also resets PMU_MCU
 *   NPU_WDT (0x27780000) - also resets NPU_MCU
 *   DDR_WDT (0x2A040000) - also resets DDR_MCU
 *   WDT_S   (0x2A4C0000) - secure
 *   WDT_NS  (0x2ACE0000) - non-secure
 *   BUS_WDT (0x2AEB0000) - also resets BUS_MCU
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_WDT_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_WDT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "rk3576_memorymap.h"
#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (relative to the instance base) *******************/

#define RK3576_WDT_CR   0x0000 /* Control register (W)              */
#define RK3576_WDT_TORR 0x0004 /* Timeout range register (W)        */
#define RK3576_WDT_CCVR 0x0008 /* Current counter value register    */
#define RK3576_WDT_CRR  0x000c /* Counter restart register (WO)     */
#define RK3576_WDT_STAT 0x0010 /* Interrupt status register (RO)    */
#define RK3576_WDT_EOI  0x0014 /* Interrupt clear register (RO)     */

/* WDT_CR (0x00) bits *************************************************/

#define WDT_CR_ENABLE                (1 << 0) /* WDT enable                 */
#define WDT_CR_RESP_MODE             (1 << 1) /* 0=reset, 1=interrupt+reset */
#define WDT_CR_RST_PLUSE_LEN_SHIFT   (2)      /* Reset pulse length [4:2]   */
#define WDT_CR_RST_PLUSE_LEN_MASK    (7 << WDT_CR_RST_PLUSE_LEN_SHIFT)
#define WDT_CR_RST_PLUSE_LEN(n)      ((n) << WDT_CR_RST_PLUSE_LEN_SHIFT)
#define WDT_CR_RST_PLUSE_LEN_DEFAULT WDT_CR_RST_PLUSE_LEN(0x2) /* 8 pclk */
#define WDT_CR_RST_PLUSE_LEN_MAX     0x7                       /* 256   */

/* WDT_TORR (0x04) bits ***********************************************/

/* Timeout period field [3:0].  Each code selects the counter reload
 * value from a 16-entry table (TRM 15.4.3).  The change takes effect only
 * after the next restart (kick).
 */

#define WDT_TORR_TIMEOUT_PERIOD_SHIFT 0
#define WDT_TORR_TIMEOUT_PERIOD_MASK  0x0f
#define WDT_TORR_TIMEOUT_PERIOD(n) \
  (((n) << WDT_TORR_TIMEOUT_PERIOD_SHIFT) & WDT_TORR_TIMEOUT_PERIOD_MASK)

/* Maximum reload count for a given timeout_period code (TRM 15.4.3).
 * The table reload values are 2^(p+16) - 1 for code p:
 *   p=0 -> 0x0000ffff, p=1 -> 0x0001ffff, ... p=15 -> 0x7fffffff.
 */

#define WDT_TORR_CNT_MAX(p) \
  ((UINT32_C(1) << (WDT_TORR_TIMEOUT_PERIOD(p) + 16)) - UINT32_C(1))

/* WDT_CRR (0x0c) restart key *****************************************/

#define WDT_CRR_KICK_KEY 0x76

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_WDT_H */
