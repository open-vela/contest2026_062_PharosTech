/****************************************************************************
 * chips/rk3576/rk3576_mipi_dcphy.h
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
 * RK3576 MIPI D/C-PHY combo PHY (DCPHY) driver public interface.
 *
 * The DCPHY is a D-PHY + C-PHY combo PHY shared between the DSI (display,
 * TX) and CSI (camera, RX) hosts.  This driver implements the TX (DSI)
 * side for now; the RX (CSI) side can be added later without changing the
 * DSI host driver, which depends only on the functions declared here.
 *
 * The interface is intentionally a plain function API (no ops table): the
 * DSI host driver calls these functions directly once registered.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DCPHY_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DCPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_MIPI_DCPHY

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dcphy_init
 *
 * Description:
 *   Bring the DCPHY out of reset and configure the shared BIAS block and
 *   the TX PLL.  This must be called exactly once before the PHY can be
 *   powered on.  The M_RESETN is asserted while programming and is left
 *   deasserted after lane enable (see rk3576_dcphy_power_on()).
 *
 *   The PHY PLL reference is the 24 MHz oscillator by default.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_dcphy_init(void);

/****************************************************************************
 * Name: rk3576_dcphy_power_on
 *
 * Description:
 *   Enable the TX PLL, then enable and wait for the clock lane and the
 *   requested number of data lanes to become PHY-ready.  The PHY reset is
 *   released once all lanes are ready.
 *
 *   The PLL is programmed for the requested high-speed data rate
 *   (in Hz) at this point: the DCPHY driver resolves the M/K/S/P dividers
 *   and programs the D-PHY lane timing counters from the resulting lane
 *   data rate.  Hence this is a one-time configuration (not a dynamic rate
 *   change) performed when the DSI host brings the PHY up.
 *
 * Input Parameters:
 *   lanes   - Number of active data lanes (1..4).
 *   dphy    - true for D-PHY mode, false for C-PHY mode.
 *   hs_rate - Requested lane high-speed data rate in Hz (80 Mbps..2.5 Gbps).
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_dcphy_power_on(uint8_t lanes, bool dphy, uint32_t hs_rate);

/****************************************************************************
 * Name: rk3576_dcphy_power_off
 *
 * Description:
 *   Disable the TX lanes and power down the PLL.  Used when the DSI host
 *   is stopped/suspended.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_dcphy_power_off(void);

/****************************************************************************
 * Name: rk3576_dcphy_is_ready
 *
 * Description:
 *   Report whether the TX PLL is locked and all enabled lanes are
 *   PHY-ready.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   true if the PHY is locked and ready, false otherwise.
 *
 ****************************************************************************/

bool rk3576_dcphy_is_ready(void);

/****************************************************************************
 * Name: rk3576_dcphy_dump
 *
 * Description:
 *   Bring-up diagnostic (read-only): print the TX PLL registers and the
 *   clock/data lane configuration actually present in the DCPHY, so the
 *   PHY side can be verified from the console instead of being assumed.
 *
 *   Everything that determines whether the PHY can emit a valid HS burst
 *   lives in this block: the PLL dividers (lane rate), the per-lane
 *   enable/ready state, the HS drive-strength code (clock lane 52 ohm vs
 *   data lanes 39 ohm), and the LP/HS timing counters (T_LPX, T_CLK_ZERO,
 *   T_CLK_PREPARE, T_CLK_TRAIL, T_HS_ZERO, T_HS_PREPARE, T_HS_EXIT,
 *   T_HS_TRAIL, escape clock).  A wrong T_HS_EXIT / T_HS_TRAIL / escape
 *   clock directly prevents a lane from returning to LP-11, which is
 *   exactly the "all lanes stuck out of stopstate" symptom.
 *
 *   Safe to call any time after rk3576_dcphy_init(); read-only.
 *
 ****************************************************************************/

void rk3576_dcphy_dump(void);

#endif /* CONFIG_RK3576_MIPI_DCPHY */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DCPHY_H */
