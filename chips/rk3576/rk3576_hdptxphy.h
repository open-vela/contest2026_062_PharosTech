/****************************************************************************
 * chips/rk3576/rk3576_hdptxphy.h
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
 * RK3576 HDMI/eDP combo PHY (HDPTX PHY) driver public interface.
 *
 * The combo PHY turns the parallel pixel stream produced by the video output
 * controller into HDMI TMDS (or eDP) serial lanes.  It owns the PLL that
 * generates the pixel clock, so it is the PHY -- not the CRU -- that decides
 * the pixel rate on the HDMI path: dclk_vp0/1/2 can be switched onto the PHY's
 * pixel-clock output (clk_hdmiphy_pixel0_o, CLKSEL_CON147[13:11] = 1'b1), and
 * there is no divider between the two.
 *
 * The PHY is shared with the eDP controller: the TRM states that the VOP "can
 * only work in HDMI or eDP mode", so only one consumer may bring it up.
 *
 * This interface is a plain function API (no ops table), matching
 * rk3576_mipi_dcphy.h.
 *
 * Scope: TMDS (HDMI/DVI) mode only.  DP mode and HDMI 2.1 FRL are not
 * implemented.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_HDPTXPHY_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_HDPTXPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_HDPTXPHY

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdptxphy_initialize
 *
 * Description:
 *   Enable the PHY's APB and GRF clocks, release their resets and release the
 *   PHY link-symbol clock reset.  This must be called once before any other
 *   function in this file.
 *
 *   The PHY itself is left powered down: the PLL, bias and band-gap blocks
 *   stay disabled, and the init/cmn/lane resets stay asserted, until
 *   rk3576_hdptxphy_power_on() runs.  That keeps the PHY off for a board that
 *   never uses HDMI, while still allowing its APB registers to be read.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_hdptxphy_initialize(void);

/****************************************************************************
 * Name: rk3576_hdptxphy_power_on
 *
 * Description:
 *   Program the TMDS PLL for the requested pixel rate and bring the PHY up:
 *   enable the reference/bias/band-gap blocks, release the init, common and
 *   lane resets in order, and enable all four serialiser lanes.
 *
 *   On success the PHY is transmitting a TMDS clock, its pixel-clock output
 *   is running at (nominally) pixel_clock_hz, and
 *   rk3576_hdptxphy_pixel_clock_hz() reports the rate actually achieved.  The
 *   caller is expected to have already routed the video port onto that clock
 *   (or to do so immediately afterwards) and to program its own TMDS timing
 *   from the same nominal pixel clock.
 *
 *   Rate selection is by exact match against a table of validated PLL
 *   settings, because the PLL has an integer main divider plus a sigma-delta
 *   fraction and only certain rates are reachable within its VCO window.  The
 *   1920x1080p60 setting in that table is exact; see the table's comment in
 *   rk3576_hdptxphy.c for the arithmetic.
 *
 * Input Parameters:
 *   pixel_clock_hz - Nominal pixel clock of the video mode, in Hz (e.g.
 *                    148500000 for 1920x1080p60).  This is the mode's dot
 *                    clock, NOT the TMDS character rate; the function applies
 *                    the color-depth scaling itself.
 *   bpc            - Bits per component: 8, 10, 12 or 16.
 *
 * Returned Value:
 *   OK on success; -EINVAL if no PLL setting is available for the resulting
 *   TMDS character rate, or a negated errno value if the PHY fails to come
 *   ready.
 *
 ****************************************************************************/

int rk3576_hdptxphy_power_on(uint32_t pixel_clock_hz, uint8_t bpc);

/****************************************************************************
 * Name: rk3576_hdptxphy_power_off
 *
 * Description:
 *   Disable all four lanes, assert the lane/common/init resets and disable the
 *   PLL, bias and band-gap blocks.  Used when the display path is stopped.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_hdptxphy_power_off(void);

/****************************************************************************
 * Name: rk3576_hdptxphy_is_ready
 *
 * Description:
 *   Report whether the PLL is locked and all lanes have signalled ready.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   true if the PHY is locked and ready, false otherwise (including when the
 *   PHY has not been powered on).
 *
 ****************************************************************************/

bool rk3576_hdptxphy_is_ready(void);

/****************************************************************************
 * Name: rk3576_hdptxphy_pixel_clock_hz
 *
 * Description:
 *   Report the pixel clock the PHY is currently configured to generate, in Hz,
 *   or 0 when the PHY has not been powered on.
 *
 *   The rate is the one the PLL was programmed for, i.e. the pixel_clock_hz
 *   most recently passed to rk3576_hdptxphy_power_on().  The CRU clock tree's
 *   clk_hdmiphy_pixel0_o node is a fixed-rate placeholder and does not track
 *   this value; callers that need the live rate must use this function.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Configured pixel clock in Hz, or 0 if the PHY is off.
 *
 ****************************************************************************/

uint32_t rk3576_hdptxphy_pixel_clock_hz(void);

#endif /* CONFIG_RK3576_HDPTXPHY */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_HDPTXPHY_H */
