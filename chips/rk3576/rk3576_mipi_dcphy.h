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
 * Name: rk3576_dcphy_rx_power_on
 *
 * Description:
 *   Bring up the DCPHY's slave (RX) lanes for MIPI CSI-2 reception,
 *   following the RX start-up sequence of TRM 21.6.4.3 "Case 3".
 *
 *   This is independent of the TX (DSI) side: the slave lanes have their
 *   own reset (S_RESETN), their own register banks (SC / COMBO_SD*), and
 *   they do not use the TX PLL at all.  The one shared resource is the
 *   BIAS block, whose start-up values are identical on both sides, so
 *   programming it here as well is idempotent.
 *
 *   The per-lane T_HS_SETTLE value is rate dependent; the caller supplies
 *   the lane rate and the driver looks the value up.  Data-lane deskew
 *   calibration is only programmed at 1.5 Gbps and above.
 *
 *   The APB clocks and the BIAS block are brought up on demand, so this may
 *   be called without rk3576_dcphy_init() having run (the CSI path does not
 *   need the TX PLL), and it is safe to call after it (the DSI path).
 *
 * Input Parameters:
 *   lanes     - Number of active RX data lanes (1..4).
 *   lane_mbps - Lane high-speed data rate in Mbps (used for the settle
 *               value and the deskew threshold).
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure (-ETIMEDOUT if a lane
 *   fails to reach PHY_READY).
 *
 ****************************************************************************/

int rk3576_dcphy_rx_power_on(uint8_t lanes, uint32_t lane_mbps);

/****************************************************************************
 * Name: rk3576_dcphy_rx_power_off
 *
 * Description:
 *   Disable the RX clock and data lanes.  The BIAS block and the APB clocks
 *   are left alone because the TX side may still be using them.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_dcphy_rx_power_off(void);

/****************************************************************************
 * Name: rk3576_dcphy_rx_is_ready
 *
 * Description:
 *   Report whether the given number of RX data lanes are enabled and have
 *   all reached PHY_READY.
 *
 * Input Parameters:
 *   lanes - Number of data lanes to check (1..4).
 *
 * Returned Value:
 *   true if every checked lane reports PHY_READY.
 *
 ****************************************************************************/

bool rk3576_dcphy_rx_is_ready(uint8_t lanes);

/****************************************************************************
 * Name: rk3576_dcphy_rx_read_grf_status0 / _read_grf_status2
 *
 * Description:
 *   Read the DCPHY GRF lane status registers, for bring-up diagnosis.
 *
 *   STATUS0 carries the slave data lane stop-state in bits [7:4] and the
 *   master clock/data lane stop-states in bits [8] and [3:0].  Note that
 *   there is no slave *clock* lane stop bit -- read the CSI HOST's
 *   PHY_STATE.phy_stopstateclk for that.
 *
 *   STATUS2 carries the slave data lane error flags in bits [15:12].
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   The raw register value (0 if the PHY has not been initialised).
 *
 ****************************************************************************/

uint32_t rk3576_dcphy_rx_read_grf_status0(void);
uint32_t rk3576_dcphy_rx_read_grf_status2(void);

#endif /* CONFIG_RK3576_MIPI_DCPHY */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DCPHY_H */
