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
 * Only these two values are meaningful to a caller:
 *
 *   RK3576_WDT_NS  - non-secure world WDT (fixed 24 MHz counting clock;
 *                    pclk/tclk gates opened by the bootloader).  The
 *                    natural watchdog for the NuttX kernel/user space and
 *                    the only instance this driver implements.
 *
 *   RK3576_WDT_PMU - kept purely as a future-proof API input; it is NOT
 *                    supported (initializing it returns NULL - add PMU
 *                    support in the driver if it is ever needed, e.g. to
 *                    keep the watchdog counting while the SoC is asleep).
 *
 * The remaining hardware instances (WDT_S, NPU/DDR/BUS) can never be used
 * by this non-secure NuttX build, so they are intentionally not part of
 * this API: WDT_S is secure-world-only (not accessible from the non-secure
 * NuttX context), and NPU/DDR/BUS reset their subordinate MCUs rather than
 * the main CPU.
 */

#define RK3576_WDT_NS  0
#define RK3576_WDT_PMU 1

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_wdt_initialize
 *
 * Description:
 *   Return the lower-half handle for the watchdog timer instance for the
 *   board to register with watchdog_register().  The initial state is
 *   disabled.
 *
 *   Only RK3576_WDT_NS is implemented.  RK3576_WDT_PMU is accepted as an
 *   input for API future-proofing but is not yet supported and returns
 *   NULL.  Any other value is invalid.
 *
 * Input Parameters:
 *   instance - WDT instance index (RK3576_WDT_NS to use; RK3576_WDT_PMU
 *              reserved for future use).
 *
 * Returned Values:
 *   A watchdog_lowerhalf_s handle on success; NULL on an un-implemented or
 *   invalid instance.
 *
 ****************************************************************************/

FAR struct watchdog_lowerhalf_s *rk3576_wdt_initialize(int instance);

#endif /* CONFIG_RK3576_WDT */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_WDT_H */
