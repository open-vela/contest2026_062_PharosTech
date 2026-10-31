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
 * ---------------------------------------------------------------------------
 * Sink timings (EDID)
 * ---------------------------------------------------------------------------
 * The controller also owns the DDC master, so it is the natural place to
 * decode what the sink says it can display.  rk3576_hdmi_read_sink_timing()
 * reads the sink's EDID base block over DDC and returns its preferred detailed
 * timing; the CALLER owns the policy (which pixel clock cap applies, which
 * rates the PHY can synthesise, what to fall back to).
 *
 * This split matters because the two halves have different failure stories: a
 * DDC read can simply not answer (no sink, no +5 V, a sink that holds the
 *bus), while the PHY can only generate the discrete rates in its PLL table.
 *Keeping the policy with the board lets a failed read fall back to a
 *known-good mode instead of failing the display bring-up.
 *
 * The decode is deliberately local rather than reusing <nuttx/video/edid.h>.
 * That header's detailed-timing horizontal helpers use the _SHIFT constants as
 * masks (`byte & 4` instead of `byte & 0xf0`, and `byte & 0` for the blanking
 * nibble), so edid_parse() reports a horizontally wrong active width for every
 * DTD; it also stores a DTD's pixel clock in the EDID's native 10 kHz units
 * while the rest of the videomode code assumes kHz.  Both are upstream quirks
 * this driver cannot fix from here, so the ~30 lines of DTD decoding live in
 * rk3576_hdmi.c where their units and bit placement are visible and testable.
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

#include <nuttx/compiler.h>

#ifdef CONFIG_RK3576_HDMI

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One display timing, in the terms the VOP and the interface both need:
 * visible geometry, porch and sync lengths, and the pixel clock.  This is the
 * bridge between "what the sink advertised" and struct rk3576_vop_config.
 *
 * The pixel clock is the mode's dot clock in Hz -- already converted out of
 * the EDID's 10 kHz units -- and is NOT necessarily a rate the PHY can
 * synthesise.  Callers must snap it (rk3576_hdptxphy_snap_pixel_clock())
 * before using it.
 */

struct rk3576_hdmi_timing_s
{
  uint32_t pixel_clock; /* Dot clock in Hz                          */

  uint16_t xres; /* Horizontal active pixels                 */
  uint16_t yres; /* Vertical active lines                    */

  uint16_t hsync_len;    /* HSYNC pulse width, pixels          */
  uint16_t hfront_porch; /* Horizontal front porch, pixels     */
  uint16_t hback_porch;  /* Horizontal back porch, pixels      */
  uint16_t vsync_len;    /* VSYNC pulse width, lines           */
  uint16_t vfront_porch; /* Vertical front porch, lines        */
  uint16_t vback_porch;  /* Vertical back porch, lines         */

  bool hsync_positive; /* Sync polarities, true = positive   */
  bool vsync_positive;

  /* Manufacturer ID from the EDID header, NUL terminated.  Carried only so
   * the board's log can name the sink it read the numbers from.
   */

  char manufacturer[4];
};

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
 * Name: rk3576_hdmi_start_video_path
 *
 * Description:
 *   Program the parts of the controller that only become reachable once the
 *   interface clock is running, and start the packet scheduler.
 *
 *   Must be called AFTER the VOP is scanning: everything in the
 *   0x0800-0x0aff block is clock-gated until the CMU's video clock domains
 *   come up, and MEASURED, programming it while gated is accepted by the bus
 *   but breaks the NEXT controller access.  See the implementation for the
 *   measurements.
 *
 *   Nothing in 0x0800-0x0aff is read anywhere in this driver: the reads of
 *   0x08e0, 0x0804 and 0x0814 abort in BOTH clock states, so a read is
 *   unreliable in a way a write is not, and nothing here needs one.  Every
 *   value the driver checks comes from VO0_GRF / CRU / VOP (plain syscon
 *   blocks) or from the always-on 0x0000-0x03ff block.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success; -EIO if no stream has been enabled.
 *
 ****************************************************************************/

int rk3576_hdmi_start_video_path(void);

/****************************************************************************
 * Name: rk3576_hdmi_hpd_connected
 *
 * Description:
 *   Report whether the sink is asserting hot-plug detect.
 *
 *   Read straight from the IOC's HDMITX_HPD_STATUS bit 3, the debounced level
 *   -- the other fields of that register are a raw pad level, an edge counter
 *   and an interrupt flag, none of which answer "is a sink attached".
 *
 *   Deliberately advisory.  On this board HPD has read low even with a monitor
 *   plugged in and terminating the link, because at boot the sink is often
 *   still asleep and releases HPD; and it stays low if the HPD pad is not
 *muxed to its HDMI function.  It is therefore reported alongside the EDID read
 *   rather than used to skip it -- see rk3576_hdmi_read_sink_timing().
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   true if the debounced hot-plug-detect level is asserted.
 *
 ****************************************************************************/

bool rk3576_hdmi_hpd_connected(void);

/****************************************************************************
 * Name: rk3576_hdmi_read_sink_timing
 *
 * Description:
 *   Read the sink's EDID base block over DDC and decode its preferred detailed
 *   timing.
 *
 *   Requires rk3576_hdmi_initialize() to have run: that is what enables the
 *   controller's APB clock, hands the DDC pads to the built-in I2C master and
 *   configures its SCL timing.  It does NOT require a mode to be enabled, so
 *   it must be called BEFORE rk3576_hdmi_enable() -- the pixel clock has to be
 *   known before the PHY is programmed with it.
 *
 *   DDC is driven even when rk3576_hdmi_hpd_connected() reports no sink.  That
 *   is deliberate: HPD on this board has been an unreliable indicator (see the
 *   note on that function, and the pad-mux note in rk3576_hdmi_routing()), and
 *   an absent sink costs only one NACK, whereas gating on HPD would silently
 *   disable EDID for exactly the case this function exists to serve.  The HPD
 *   level is logged either way.
 *
 *   The whole base block is logged before anything is decoded from it, so a
 *   surprising mode can be explained from the log alone.
 *
 *   The timing returned is the FIRST usable detailed timing, found by scanning
 *   all four descriptor slots.  EDID 1.3+ requires the preferred timing to be
 *   descriptor 1, but cheap EDID 1.0/1.2 sinks predate that rule and put their
 *   monitor name there instead; reading only slot 1 makes those look like they
 *   have no timing at all.  "First" is what "preferred" means, since the
 *   descriptors are ordered and the first timing is the sink's native mode --
 *   choosing a later one would need a preference of our own, and falling back
 *   to the caller's known-good default is a more predictable outcome than
 *   driving a secondary mode.
 *
 * Input Parameters:
 *   timing - Receives the decoded timing on success.  Must not be NULL; left
 *            unmodified on failure.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure:
 *     -EINVAL  NULL argument, the EDID header or checksum is wrong, or a
 *timing descriptor was malformed (zero or absurd active size, a missing porch,
 *...) -ENOTSUP no descriptor held a timing this driver can express -- either
 *              there was none (which also reports as -ENOENT) or every one
 *              found was interlaced or did not use digital separate sync
 *     -ENOENT  the block was valid but contained no detailed timing at all
 *     -ETIMEDOUT  the DDC transfer did not complete
 *     -EIO     the sink did not acknowledge
 *
 * Assumptions:
 *   Called from task context, after rk3576_hdmi_initialize() has succeeded.
 *   The sink is powered (the board drives the connector's +5 V rail).
 *
 ****************************************************************************/

int rk3576_hdmi_read_sink_timing(FAR struct rk3576_hdmi_timing_s *timing);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* CONFIG_RK3576_HDMI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H */
