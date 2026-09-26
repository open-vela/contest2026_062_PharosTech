/****************************************************************************
 * chips/rk3576/rk3576_hdmi.h
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
 * RK3576 HDMI TX controller driver public interface.
 *
 * The controller is a DesignWare HDMI QP transmitter.  It does not own the
 * pixel clock and it does not generate timings; it consumes the pixel stream
 * the VOP pushes over the IPI bus and serialises it into TMDS on request from
 * the HDPTX PHY.
 *
 * ---------------------------------------------------------------------------
 * Bring-up order (what a board init must call, and in this order)
 * ---------------------------------------------------------------------------
 *   1. rk3576_hdmi_initialize()
 *        Clocks, CRU resets, VO0_GRF routing fields, controller register-file
 *        init, and finally the PHY's own APB/GRF bring-up (power_off state).
 *   2. rk3576_hdmi_enable(pixel_clock, bpc)
 *        Programs the PHY PLL for the requested pixel rate, selects DVI mode
 *        in the controller and releases the video datapath.
 *   3. rk3576_vop_initialize() with iface = RK3576_VOP_IFACE_HDMI
 *        Routing the VOP last (rather than before step 2) means the pixel
 *        stream only starts once the PHY is already producing a stable clock,
 *        which avoids the sink latching onto a runt first frame.
 *
 * The pixel clock is a *parameter of the mode*, not something the caller can
 * query from the CRU: on the HDMI path the PHY's PLL generates it, so the
 * caller must pass the same value it hands to rk3576_vop_initialize()
 * (cfg.pixel_clock).  rk3576_hdmi_pixel_clock_hz() reports back what was
 * actually programmed.
 *
 * The VOP side must reparent dclk_vp0_sel onto clk_hdmiphy_pixel0_o and must
 * NOT call clk_set_rate() on dclk_vp0 -- see the long note in
 * rk3576_clk_register_hdmi() in rk3576_clk_tree.c.
 *
 * Scope: DVI mode over TMDS only.  HDMI infoframes, audio, HDCP, SCDC
 * scrambling and HDMI 2.1 FRL are not implemented; a sink that requires any
 * of them will not produce a picture.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_HDMI

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_hdmi_initialize
 *
 * Description:
 *   Bring the HDMI TX controller up to the point where it can accept a mode,
 *   and put the shared HDPTX PHY into its powered-off state.
 *
 *   Enables the controller's clocks, releases its CRU resets, programs the
 *   VO0_GRF routing fields (TMDS mode, 8 bpc RGB, VOP as the pixel source,
 *   DDC pads owned by the controller's I2C master), initialises the
 *   controller's register file and its embedded I2C master, and then brings
 *   up the PHY itself.
 *
 *   Safe to call more than once; the second and later calls return 0 without
 *   touching the hardware.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure:
 *     -EIO     a clock could not be obtained or enabled
 *     -ENODEV  the controller did not answer (CORE_ID read back as zero)
 *     -ETIMEDOUT  the controller's internal clock tree did not start
 *
 * Assumptions:
 *   Called from task context during board bring-up, after the CRU clock tree
 *   has been registered (rk3576_clk_initialize()).
 *
 ****************************************************************************/

int rk3576_hdmi_initialize(void);

/****************************************************************************
 * Name: rk3576_hdmi_enable
 *
 * Description:
 *   Start outputting a DVI (TMDS) video stream at the requested pixel rate.
 *
 *   Programs the HDPTX PHY PLL for pixel_clock_hz at bpc bits per colour
 *   component, sets the VO0_GRF colour depth to match, selects DVI mode in
 *   the controller and releases the video datapath.  Nothing is programmed
 *   into the controller's timing or video-interface registers: the pixel
 *   data, syncs and bus format all arrive over the IPI from the VOP.
 *
 * Input Parameters:
 *   pixel_clock_hz - Pixel clock of the mode in Hz.  Must be one of the rates
 *                    the PHY's TMDS PLL table can synthesise exactly; for
 *                    1080p60 this is 148500000.
 *   bpc            - Bits per colour component: 8 or 10.  DVI sinks are
 *                    always 8 bpc, so 8 is the value to use for the first
 *                    bring-up.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure:
 *     -EINVAL     bpc is not 8 or 10, or the pixel clock is not synthesitable
 *     -EIO        the controller clocks are not running (initialize() was not
 *                 called, or it failed)
 *     -ETIMEDOUT  the PHY PLL did not lock or the lanes did not come ready
 *
 * Assumptions:
 *   rk3576_hdmi_initialize() has already succeeded.
 *
 ****************************************************************************/

int rk3576_hdmi_enable(uint32_t pixel_clock_hz, uint8_t bpc);

/****************************************************************************
 * Name: rk3576_hdmi_uninitialize
 *
 * Description:
 *   Tear the controller down: stop any running stream, park the PHY, release
 *   the controller's CRU resets and disable its clocks.
 *
 *   Provided for symmetry and for a future power-management path.  Board
 *   initialisation never calls it -- there is no display driver to own the
 *   lifecycle yet.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success.
 *
 ****************************************************************************/

int rk3576_hdmi_uninitialize(void);

/****************************************************************************
 * Name: rk3576_hdmi_disable
 *
 * Description:
 *   Stop the video stream: power the PHY down, hold its lanes in reset and
 *   return the controller to its idle (DVI-mode, no-stream) state.
 *
 *   The controller's register file is left configured, so a subsequent
 *   rk3576_hdmi_enable() is enough to start a new mode; only
 *   rk3576_hdmi_uninitialize() tears the controller down.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success, even if no stream was running.
 *
 ****************************************************************************/

int rk3576_hdmi_disable(void);

/****************************************************************************
 * Name: rk3576_hdmi_is_ready
 *
 * Description:
 *   Report whether the HDMI transmit path is currently producing a stream.
 *
 *   True only when a mode has been enabled AND the PHY's PLL is locked AND
 *   the PHY reports its parallel output ready AND the controller's video
 *   interface has latched the sync polarities from the incoming IPI stream.
 *   That last condition is what distinguishes "the PHY is running" from "the
 *   VOP is actually feeding us", which is the difference between a black
 *   screen and a picture.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   true if a valid stream is on the wire.
 *
 ****************************************************************************/

bool rk3576_hdmi_is_ready(void);

/****************************************************************************
 * Name: rk3576_hdmi_pixel_clock_hz
 *
 * Description:
 *   Return the pixel clock of the mode currently enabled, in Hz, or 0 if no
 *   mode is enabled.
 *
 *   In practice this is the value that was passed to rk3576_hdmi_enable();
 *   it exists as an accessor because "what rate is the display path actually
 *   running at" is otherwise unanswerable on the HDMI path -- the CRU has no
 *   divider between the PHY and the video port (clk_hdmiphy_pixel0_o is a
 *   fixed-rate placeholder that does not track the live PHY rate).
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Pixel clock in Hz, or 0.
 *
 ****************************************************************************/

uint32_t rk3576_hdmi_pixel_clock_hz(void);

/****************************************************************************
 * Name: rk3576_hdmi_ref_clock_hz
 *
 * Description:
 *   Return the frequency in Hz of the controller's reference clock
 *   (CLK_HDMITX0_REF), which is what the controller's internal timers count
 *   against and what rk3576_hdmi_initialize() programmed into
 *   TIMER_BASE_CONFIG0.
 *
 *   Exposed mainly for diagnostics: a value other than 396 MHz means the
 *   aclk_vo0_root divider is not at its reset setting, and the reference
 *   driver's own limits on which pixel rates are reachable are relative to
 *   this value.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Reference clock in Hz, or 0 if the driver is not initialised.
 *
 ****************************************************************************/

uint32_t rk3576_hdmi_ref_clock_hz(void);

/****************************************************************************
 * Name: rk3576_hdmi_report_status
 *
 * Description:
 *   Log the controller's internal clock state, evaluated against the vendor
 *   driver's documented readiness test (CMU_STATUS & 0x15) == 0x15.
 *
 *   Must NOT read anything in the 0x0800-0x08ff or 0x0a00-0x0aff blocks.
 *   Three measured panics came from reading HDCP2LOGIC_CONFIG0 (0x08e0),
 *   VIDEO_INTERFACE_STATUS0 (0x0814) and VIDEO_INTERFACE_CONFIG1 (0x0804);
 *   the last one aborted even with ipi_clk/vidqpclk/linkqpclk all confirmed
 *   running at 37125 Hz, so the upper block does not become readable merely
 *   because the clocks came up -- it is a power-domain boundary, not a clock
 *   gate.  Only CMU_STATUS (0xb0), MAINUNIT_STATUS0/1 (0x180/0x3010/0x3020),
 *   RESET_MANAGER_* and the IOC HPD register (0x440) are safe here.
 *
 *   Writes into that window are accepted, though; the packet scheduler is
 *   programmed successfully.  It is only the read path that aborts.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success; -EIO if no stream has been enabled.
 *
 ****************************************************************************/

int rk3576_hdmi_report_status(void);

/****************************************************************************
 * Name: rk3576_hdmi_sweep_ipi_format
 *
 * Description:
 *   Step VO0_GRF SOC_CON8's grf_hdmitx_iipi_format field through every value
 *   the TRM names (0 RGB, 1 YCbCr 4:2:2, 2 YCbCr 4:4:4, 3 YCbCr 4:2:0),
 *   dwelling on each long enough to be seen on the glass, then restore the
 *   value the driver normally programs.
 *
 *   This exists because the IPI FORMAT is the last known difference between
 *   this driver and the vendor firmware that lights a display: the vendor
 *   sets SOC_CON8 = 0x00000020 (format 2, YCbCr 4:4:4) because its VOP emits
 *   YUV444, while this driver sends RGB888 and therefore says format 0.  That
 *   reasoning looks sound, but the equivalent reasoning about the colour
 *   DEPTH was wrong once already -- the depth in SOC_CON8 and the depth used
 *   by the MIPI path live in different fields for different consumers, and a
 *   value proven good on one says nothing about the other.
 *
 *   So rather than argue it, watch it.  Call this AFTER the framebuffer holds
 *   a visible test pattern, and have the observer report whether the screen
 *   ever changes from black during the sweep.  A single "it changed when the
 *   log said format=N" settles the question; a screen that stays black
 *   through all four values rules the IPI format out entirely.
 *
 *   The write is to VO0_GRF, a plain syscon block, so it is safe to do while
 *   the display pipeline is running, and the field is latched by the
 *   controller on the next frame.
 *
 * Input Parameters:
 *   dwell_ms - Time to hold each value, in milliseconds.  Use 0 for a
 *              default of 2000 ms, which is long enough to notice.
 *
 * Returned Value:
 *   Zero (OK) on success; -EIO if no stream has been enabled.
 *
 ****************************************************************************/

int rk3576_hdmi_sweep_ipi_format(uint32_t dwell_ms);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* CONFIG_RK3576_HDMI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H */
