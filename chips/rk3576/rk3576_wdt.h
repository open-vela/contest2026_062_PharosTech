/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_wdt.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_WDT_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_WDT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/timers/watchdog.h>

#ifdef CONFIG_RK3576_WDT

/* WDT instance enumeration (used as rk3576_wdt_initialize() param).
 *
 * The RK3576 TRM exposes SIX hardware instances, but only two are usable
 * from NuttX:
 *
 *   RK3576_WDT_NS  - non-secure world WDT (24 MHz).  The natural watchdog
 *                    for the NuttX kernel/user space.
 *   RK3576_WDT_PMU - PMU-domain WDT (32 kHz deep-sleep clock).  The only
 *                    WDT that keeps counting while the system is asleep.
 *
 * The remaining instances are NOT usable from NuttX:
 *   RK3576_WDT_S   - secure-world only; not accessible from the
 *                    non-secure NuttX context.
 *   RK3576_WDT_NPU - resets NPU_MCU (subordinate MCU), not the main CPU.
 *   RK3576_WDT_DDR - resets DDR_MCU (subordinate MCU), not the main CPU.
 *   RK3576_WDT_BUS - resets BUS_MCU (subordinate MCU), not the main CPU.
 *
 * Consequently rk3576_wdt_initialize() only accepts RK3576_WDT_NS and
 * RK3576_WDT_PMU; any other instance is rejected.
 */

#define RK3576_WDT_PMU  0
#define RK3576_WDT_NPU  1
#define RK3576_WDT_DDR  2
#define RK3576_WDT_S    3
#define RK3576_WDT_NS   4
#define RK3576_WDT_BUS  5
#define RK3576_WDT_NWDT 6 /* Total number of hardware WDT instances */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_wdt_initialize
 *
 * Description:
 *   Return the lower-half handle for one watchdog timer instance for the
 *   board to register with watchdog_register().  The initial state is
 *   disabled.
 *
 * Input Parameters:
 *   instance - WDT instance index.  Only RK3576_WDT_NS and RK3576_WDT_PMU
 *              are valid from NuttX; all other instances (secure-world S,
 *              or NPU/DDR/BUS which reset subordinate MCUs) are rejected.
 *
 * Returned Values:
 *   A watchdog_lowerhalf_s handle on success; NULL on invalid instance.
 *
 ****************************************************************************/

FAR struct watchdog_lowerhalf_s *rk3576_wdt_initialize(int instance);

#endif /* CONFIG_RK3576_WDT */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_WDT_H */
