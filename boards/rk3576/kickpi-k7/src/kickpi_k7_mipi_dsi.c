/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_mipi_dsi.c
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
 * On-board MIPI DSI LCD panel bring-up for the kickpi-k7 (RK3576).
 *
 * Wiring map (see kickpi-k7-dsi-display.txt):
 *   - DSI data/clock lanes use dedicated pins (no GPIO mux needed).
 *   - LCD_BL_PWM : GPIO0_B5 / PWM1_CH1  (backlight)
 *   - LCD_RST    : GPIO0_A2             (panel reset, active-low)
 *   - LCD_PWREN  : GPIO0_C6             (panel power enable)
 *
 * Panel: WKS50HD072-WCT (5.0", 720 x 1280, DSI video mode).
 *
 * NOTE: The DSI panel is NOT a fixed on-board accessory; users may swap in a
 * different panel module.  However, this board code currently supports only
 * ONE model -- the WKS50HD072-WCT -- whose DCS init sequence and video-mode
 * timing are hard-coded below.  If a different panel is attached, its data
 * sheet must be consulted and the timing / init sequence re-tuned; this code
 * is NOT a drop-in match for arbitrary DSI panels.
 *
 * WKS50HD072-WCT:
 *   - 720 x 1280 portrait, RGB888, 4 data lanes, 62 MHz pixel clock
 *   - Timing (vendor-supplied; identical to the mainline 5" ILI9881D modes
 *     bananapi,lhr050h41 and startek,kd050hdfia020):
 *             HFP=10 HSYNC=20 HBP=30 (htotal=780),
 *             VFP=10 VSYNC=10 VBP=20 (vtotal=1320)  -> ~60 Hz
 *   - 780 and 1320 are both 4-pixel aligned, which the RK3576 DSI-2 TRM
 *     18.2.1 requires for HTOTAL/HACTIVE/HSYNC/HBP/HFP.  The blog-derived
 *     timing used before had htotal = 823 (823 % 4 == 3).
 *
 * Bring-up sequence (per the RK3576 chip drivers; the DSI driver owns the
 * DCPHY, so board code never touches the PHY directly):
 *   1. rk3576_mipi_dsi_initialize(cfg)          -- DSI-2 host; brings up the
 *                                               DCPHY and enters Command mode
 *   2. mipi_dsi_host_register / device_register -- bind panel device
 *   3. kickpi_k7_mipi_dsi_panel_cycle()         -- hard panel power/reset
 *                                               cycle, then the DCS init
 *                                               sequence; verified via the
 *                                               DCS power-mode read-back and
 *                                               retried (with another power
 *                                               cycle) if DISPON did not stick
 *   4. rk3576_mipi_dsi_enable_video(...)        -- program IPI timing, then
 *                                               transition to Video mode
 *   5. rk3576_vop_initialize(...)               -- register /dev/fbN
 *   6. kickpi_k7_mipi_dsi_backlight_enable()  -- backlight on (so the panel
 *                                               is visible during step 7)
 *   7. kickpi_k7_mipi_dsi_health_timeline()   -- poll the live video path
 *                                               for a few seconds so a stream
 *                                               that dies is caught in the act
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <assert.h>
#include <errno.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/video/mipi_display.h>
#include <nuttx/video/mipi_dsi.h>

#include "rk3576_gpio.h"
#include "rk3576_mipi_dsi.h"
#include "rk3576_pwm.h"
#include "rk3576_vop.h"

/* The hardware register map, for the VOP interrupt bit definitions.  They are
 * needed here because the per-VP and SYS groups use DIFFERENT bit orders and
 * conflating them is what made this driver mis-read POST_BUF_EMPTY; naming the
 * bits from the single authoritative header is the point. */

#include "hardware/rk3576_vop.h"

#ifdef CONFIG_KICKPI_K7_MIPI_DSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DSI-2 video-mode packet type.  The RK3576 DSI-2 controller encodes the
 * video transmission mode in DSI2_VID_TX_CFG.vid_mode_type (TRM 18.3.2.3)
 * as: 0 = non-burst with sync pulses, 1 = non-burst with sync events,
 * 2 = burst.  The value is passed verbatim through rk3576_dsi_config.
 */

#define KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES (0x0)
#define KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS (0x1)
#define KICKPI_K7_DSI_VID_MODE_BURST                 (0x2)

/* Active selection.
 *
 * *** WHY THIS IS BACK TO BURST ***
 *
 * Measured with SYNC_PULSES (the panel's nominal mode) on the corrected
 * 780/1320 timing, the failure is now REPRODUCIBLE and very specific:
 *
 *   ipi_data = 768 (full)   phy_txhs = 0 (empty)   lanes all in LP-11
 *   ipi_vid_fsm = state 9, counter saturated, STUCK bit set
 *   sys_main / sys_pkt_build / phy_tx_ready = state 0 (never started)
 *   INT_ST_TO = 0 (no timeout)
 *
 * That is the signature of "the IPI accepted pixels into its FIFO but never saw
 * the event that starts a line": data is clocked in regardless, while the FSM
 * sits waiting for a boundary it never observes, so no packet is ever built.
 * In a NON-burst mode the controller derives its line boundaries from the
 * hsync/vsync it must receive over the VOP -> IPI interface; BURST does not
 * depend on them.
 *
 * Two consequences:
 *   1. The earlier conclusion "BURST is the only mode that transmits" was
 *      reached by the throwaway sweep on the WRONG geometry (htotal = 823, not
 *      4-pixel aligned) and was then retracted.  It now looks correct for a
 *      different reason: this VOP/IPI combination does not deliver usable
 *      per-line sync, so non-burst modes cannot work regardless of geometry.
 *   2. EVERY earlier BURST test is invalid as a verdict, because they all ran
 *      with the mid-stream IPI re-timing that has since been removed from
 *      rk3576_vop_enable_clocks() (it rewrote HACT/HLINE and PHY_IPI_RATIO
 *      while the pixel datapath was live -- see that function).  BURST has
 *      never been measured on the corrected timing WITH that race removed.
 *
 * So: BURST, same clock-lane setting as before, corrected timing, no mid-stream
 * register rewrite.  This is a single-variable change from the previous build.
 *
 * mode_type is reported on the console by enable_video()
 * ("dsi: VIDEO CFG mode_type=..."), so every captured log states which one was
 * actually programmed. */

/* *** NOW NON-BURST WITH SYNC *EVENTS* -- on the panel vendor's own word. ***
 *
 * The choice above was made by analogy: mainline panels whose porches and init
 * table match this one are driven as SYNC_PULSE, so this panel was assumed to
 * be in that group too.  The panel manufacturer has since supplied the actual
 * device tree for this module (kickpi-k7-dsi-display.txt, section dated
 * 2026-09-25, "下面是联系屏幕厂商之后得到的香橙派开发板的设备树节选"), and it
 * says:
 *
 *     dsi,flags = <(MIPI_DSI_MODE_VIDEO)>;     // VIDEO, and NOTHING else
 *     dsi,lanes = <4>;  dsi,format = <0>;
 *     clock-frequency = <62000000>;  ... 720x1280, porches 10/20/30, 10/10/20
 *
 * No MIPI_DSI_MODE_VIDEO_BURST and no MIPI_DSI_MODE_VIDEO_SYNC_PULSE.  Both
 * Linux DSI host drivers that consume this property (Synopsys dw-mipi-dsi2 and
 * Allwinner's) select the transmission type with the same if/else-if chain, so
 * "VIDEO and nothing else" resolves to its FINAL else branch:
 *
 *     dw_mipi_dsi2_set_vid_mode(): BURST if VIDEO_BURST, else SYNC_PULSES if
 *                                  VIDEO_SYNC_PULSE, else **SYNC_EVENTS**
 *
 * i.e. the vendor's working configuration is vid_mode_type = 1, and this board
 * has been running mode_type = 0.  The two modes are not interchangeable: with
 * sync PULSES the controller emits explicit HSS *and* HSE packets around each
 * line, while with sync EVENTS it emits no sync packets at all and the panel
 * derives line timing from the video packet stream itself.  A panel whose TCON
 * expects one of them discards the other -- with a clean link, zero error
 * counters and no picture, which is exactly what this project has measured for
 * forty rounds.
 *
 * Everything else in that device tree already matches this driver: the init
 * sequence is the one already sent verbatim, the timing is the 780 x 1320
 * geometry already programmed, format 0 is RGB888, and the clock lane is
 * continuous (no MIPI_DSI_CLOCK_NON_CONTINUOUS flag, matching
 * KICKPI_K7_DSI_CONTINUOUS_CLK below).  mode_type is therefore the ONLY
 * remaining difference from the vendor's own configuration, which is why this
 * is now the single variable changed in this build.
 *
 * The evidence that chose SYNC_PULSES is recorded so it can be re-weighed if
 * this turns out wrong: it came from startek,kd050hdfia020 and
 * bananapi,lhr050h41, two mainline panels with very similar porches and init
 * tables -- but they are DIFFERENT panels, and neither is this one. */

#define KICKPI_K7_DSI_VID_MODE KICKPI_K7_DSI_VID_MODE_BURST

/* Clock-lane behaviour, as a single switch (the other half of the axis above).
 *
 * *** WHY THIS IS NOW TRUE (CONTINUOUS) ***
 *
 * BURST mode is active (see above), and in the mainline Linux ILI9881D driver
 * the clock-lane type is not free: every panel flagged VIDEO_BURST leaves
 * MIPI_DSI_CLOCK_NON_CONTINUOUS UNSET, i.e. it is driven with a CONTINUOUS
 * clock lane:
 *
 *   wanchanglong,w552946aba : VIDEO | VIDEO_BURST | LPM | NO_EOT_PACKET
 *   bestar,bsd1218-a101kl68 : VIDEO | VIDEO_BURST | LPM | NO_EOT_PACKET
 *
 * The opposite is equally consistent: the only panels that explicitly request
 * a NON-continuous clock (ampire,am8001280g and waveshare,7.0-dsi-touch-a) are
 * both NON-burst.  So the two axes are coupled -- a returning clock lane goes
 * with per-line sync, a free-running one goes with burst -- which makes sense:
 * in burst mode the panel has no per-line edge to re-synchronise on and
 * relies on the continuously present HS clock instead.
 *
 * The previous value (false) was justified by the panel spec's THS-EXIT
 * parameter plus the two mainline panels whose TIMING matches this one -- but
 * those two are sync-pulse panels, so they say nothing about how a burst
 * stream should be clocked.  Measured states, for the record:
 *   BURST + non-continuous : lanes DO transmit (14/16 samples out of LP-11,
 *                            and they return to LP-11 as soon as the VOP feed
 *                            is gated) but the panel stays dark;
 *   SYNC_PULSES + non-cont : the IPI wedges in ipi_vid_fsm state 9 before any
 *                            packet is built (see the video-mode note above).
 *
 * This is the single variable changed relative to the previous build. */

#define KICKPI_K7_DSI_CONTINUOUS_CLK false

/* Default EoTp setting for the board's own configuration (the matrix below
 * tests both).  FALSE, because every reference value that exists for this
 * panel IC and for this SoC has EoTp OFF:
 *
 *   Rockchip's DTS for an ILI9881D panel (rv1126-evb-v10.dtsi) sets
 *   MIPI_DSI_MODE_EOT_PACKET, which Linux documents as "disable EoT packets in
 *   HS mode"; and every RK3576 reference panel node (rk3576-tablet.dtsi,
 *   rk3576s-tablet-v10.dts, rk3576-toybrick-d0-linux.dts) sets
 *   MIPI_DSI_MODE_NO_EOT_PACKET.  This driver had only ever run with EoTp
 *   ENABLED. */

#define KICKPI_K7_DSI_DEFAULT_EOTP false

/* Panel geometry and link configuration. */
#define KICKPI_K7_MIPI_DSI_XRES   720
#define KICKPI_K7_MIPI_DSI_YRES   1280
#define KICKPI_K7_MIPI_DSI_LANES  4
#define KICKPI_K7_MIPI_DSI_FORMAT MIPI_DSI_FMT_RGB888
#define KICKPI_K7_MIPI_DSI_PIXCLK 62526316u /* Hz -- see the note below */

/* Why the pixel clock is 62.526 MHz: because the panel's vendor says 62 MHz,
 * and 62.526 MHz is the closest rate this board can actually produce.
 *
 * The panel manufacturer's own device tree for this module
 * (kickpi-k7-dsi-display.txt, section dated 2026-09-25) specifies
 *
 *     clock-frequency = <62000000>;
 *
 * and every other part of that tree already matches this driver byte for byte
 * (porches 10/20/30 and 10/10/20, the init sequence, RGB888, video mode,
 * continuous clock lane).  The clock is therefore the one quantitative value
 * where this board and the vendor's reference disagree, and the disagreement
 * was -4.2%:
 *
 *   request 64000000 -> 1188/19 = 62526316 (1.8% low)
 *   request 62000000 -> 1188/20 = 59400000 (4.2% low)   <- what this board ran
 *   request 62526316 -> 1188/19 = 62526316 (EXACT)      <- now (+0.85% vs 62M)
 *
 * (clk_set_rate(dclk_vp0) keeps the largest divisor of gpll whose output is
 * still <= the request, so the request is an upper bound.  1188/19 = 62.5263
 * MHz is ABOVE 62.000 MHz, which is why requesting exactly 62000000 silently
 * landed on divisor 20 and cost 4.2%.  Requesting the value the divider chain
 * can actually hit is what makes it exact.)
 *
 * WHY THIS IS WORTH CHANGING NOW, rather than a refresh-rate nit: a panel with
 * no frame memory has no clock of its own -- it recovers one from the DSI
 * high-speed clock and times its TCON from that.  The panel's datasheet makes
 * the coupling explicit (frame rate is given in terms of the link bit rate,
 * lanes and pixel format), i.e. the rate the LINK implies IS the pixel rate the
 * panel believes it is receiving:
 *
 *     implied rate = hs_rate * lanes / bpp = 372 M * 4 / 24 = 62.0 Mpx/s
 *     delivered                                                   59.4 Mpx/s
 *
 * so the panel has been told to expect 62 Mpx/s while the host delivered 59.4:
 * a permanent 4.4% disagreement about how long a line lasts.  MAKING THAT
 * DISAGREEMENT ZERO IS THE POINT, which is also why the link rate below is now
 * derived from this clock instead of being kept at a round 372 Mbps -- for the
 * panel to derive 62.526 MHz, the link must carry 62.526 * 24 / 4 = 375.2 Mbps.
 *
 * which is the clock error itself and not a separate fault. */

/* Link rate implied directly by the pixel stream, derived from the pixel clock
 * rather than chosen for roundness:
 *   62526316 Hz * 24 bpp / 4 lanes = 375157896 bps per lane.
 * The board asks for 375.16 Mbps so that the rate the panel DERIVES from the
 * link (hs_rate * lanes / bpp = 62.5267 Mpx/s) equals the rate the VOP
 * delivers.  Keeping the round 372 Mbps here would have preserved the very
 * 0.85% framing disagreement this change exists to remove.
 *
 * Burst mode then takes x10/9 of that below, exactly as the reference driver
 * does, so a burst build runs at 416.8 Mbps. */

#define KICKPI_K7_MIPI_DSI_HS_RATE_RAW 375160000u

/* Burst mode needs link headroom.  Burst transmission time-compresses the
 * active pixels of each line into one packet sent as fast as possible, so
 * the link must be able to deliver pixels FASTER than the raw pixel rate,
 * otherwise the compressed burst cannot fit inside the line's own timing
 * and every horizontal boundary shifts.  The reference driver applies
 * exactly this factor (dw_mipi_dsi2_get_lane_mbps():
 *   "take 1 / 0.9, since Mbps must big than bandwidth of RGB").
 *
 * Note that a NON-burst stream has no such margin by design: its payload
 * occupies the active period and only the blanking is left for packet
 * overhead, which is why the data lanes stay in high speed across the whole
 * active frame at this ratio (measured: one LP-11 -> HS transition per FRAME,
 * high speed 100% of the active period).  That is a property of the mode, NOT
 * a fault, and it is worth stating because an earlier round read it as
 * "the panel gets no per-line edge": the per-line LP-11 gaps a burst stream
 * shows come from its time compression, and the panel's per-line structure
 * comes from the video packets themselves, not from LP transitions.
 *
 * Set the numerator/denominator to 1/1 to run burst at the raw rate -- a
 * deliberately different experiment (does the panel need the margin, or only
 * the mode?). */

#define KICKPI_K7_MIPI_DSI_BURST_HEADROOM_NUM 10u
#define KICKPI_K7_MIPI_DSI_BURST_HEADROOM_DEN 9u

/* Burst mode needs link headroom.  Burst transmission time-compresses the
 * active pixels of each line into one packet sent as fast as possible, so
 * the link must be able to deliver pixels FASTER than the raw pixel rate,
 * otherwise the compressed burst cannot fit inside the line's own timing
 * and every horizontal boundary shifts.  The reference driver applies
 * exactly this factor (dw_mipi_dsi2_get_lane_mbps():
 *   "take 1 / 0.9, since Mbps must big than bandwidth of RGB").
 *
 * Set the numerator/denominator to 1/1 to run burst at the raw rate -- a
 * deliberate experiment that isolates "the panel needs burst mode" from
 * "the panel needs the rate margin on top of burst mode".  The resulting
 * rate is printed by enable_video() ("dsi: VIDEO CFG ... hs_rate=...").
 */

#define KICKPI_K7_MIPI_DSI_BURST_HEADROOM_NUM 10u
#define KICKPI_K7_MIPI_DSI_BURST_HEADROOM_DEN 9u

/* The link rate actually handed to the DSI host, derived from the active
 * video mode: burst mode takes the headroom above, the non-burst modes use
 * the raw rate (they preserve the panel timing 1:1, so they need no
 * compression headroom). */

#if KICKPI_K7_DSI_VID_MODE == KICKPI_K7_DSI_VID_MODE_BURST
#  define KICKPI_K7_MIPI_DSI_HS_RATE                                    \
     (KICKPI_K7_MIPI_DSI_HS_RATE_RAW * KICKPI_K7_MIPI_DSI_BURST_HEADROOM_NUM / \
      KICKPI_K7_MIPI_DSI_BURST_HEADROOM_DEN)
#else
#  define KICKPI_K7_MIPI_DSI_HS_RATE KICKPI_K7_MIPI_DSI_HS_RATE_RAW
#endif

/* Panel timing (porches / sync, in pixels / lines).
 *
 * Vendor-supplied values.  They match the mainline Linux modes for the two
 * 5" 720x1280 ILI9881D panels (bananapi,lhr050h41 and
 * startek,kd050hdfia020) EXACTLY: clock = 62000000, hsync_start = 720 + 10,
 * hsync_end = 720 + 10 + 20, htotal = 720 + 10 + 20 + 30; vsync_start =
 * 1280 + 10, vsync_end = 1280 + 10 + 10, vtotal = 1280 + 10 + 10 + 20.
 * The vendor DT also leaves the HSYNC/VSYNC/DE polarity properties unset,
 * i.e. DRM's default (positive / active-high) -- which is what the VOP's
 * MIPI0_INFACE_CTRL already programs (hsync_pol = vsync_pol = 1).
 */

#define KICKPI_K7_HSYNC_LEN    20
#define KICKPI_K7_HFRONT_PORCH 10
#define KICKPI_K7_HBACK_PORCH  30
#define KICKPI_K7_VSYNC_LEN    10
#define KICKPI_K7_VFRONT_PORCH 10
#define KICKPI_K7_VBACK_PORCH  20

/* One full line, used to derive the burst payload fraction that the
 * observation fingerprint compares the measured lane duty against. */

#define KICKPI_K7_HTOTAL                                              \
  (KICKPI_K7_MIPI_DSI_XRES + KICKPI_K7_HFRONT_PORCH +                 \
   KICKPI_K7_HSYNC_LEN + KICKPI_K7_HBACK_PORCH)

/* ipi_data FIFO level (words) at which the incoming pixel stream is
 * considered backed up.  The FIFO holds 768 words -- one line needs 540
 * (720 px * 3 B / 4), so anything above ~512 means more than a line is
 * waiting and the packet builder is behind the source, not merely buffering. */

#define KICKPI_K7_IPI_DATA_BACKED_UP 512u

/* DSI host bus number (the RK3576 driver sets host.bus = 0). */

#define KICKPI_K7_DSI_BUS 0
#define KICKPI_K7_DSI_VC  0 /* Virtual channel */

/* Panel control pins. */

#define KICKPI_K7_MIPI_DSI_RST   (GPIO_PORT0 | GPIO_PIN_A2)
#define KICKPI_K7_MIPI_DSI_PWREN (GPIO_PORT0 | GPIO_PIN_C6)

/* Panel power / reset sequencing timings (ms).
 *
 * The panel is driven through a real power cycle on EVERY boot: PWREN is
 * brought low, held there long enough for the rails to discharge, and only
 * then raised with RST still asserted.  Driving PWREN high without ever
 * lowering it (as this code used to) gives the panel no power-on reset on a
 * warm reboot, so the panel IC can silently stay in whatever state the
 * previous session left it in and refuse to latch the init sequence -- a
 * classic cause of "the display worked once and never again".
 *
 * Timings follow the vendor DT (power-delay-ms = <10>) and mainline Linux
 * ili9881c_prepare() (5/5 ms rails, then a 20 ms reset assert and a 20 ms
 * release), extended on the release side because this panel's DCS table needs
 * more settle time before its first command.
 */

#define KICKPI_K7_POWER_OFF_MS    30 /* PWREN low: let the rails decay */
#define KICKPI_K7_PWREN_SETTLE_MS 20 /* rails good before reset release */
#define KICKPI_K7_RST_ASSERT_MS   20 /* reset held low */
#define KICKPI_K7_RST_RELEASE_MS  120 /* after release, before first command */

/* How many times to power-cycle the panel and re-send the init sequence when
 * the DCS power-mode read-back shows DISPON never latched. */

#define KICKPI_K7_INIT_RETRIES 3

/* Extra settle delay inserted after EVERY init command, on top of the
 * per-entry delay.
 *
 * ENABLED this round (0 -> 16) and it is the ONLY panel-side change in it.
 * Reason: the vendor device tree gives EVERY init command `39 10 ...`, i.e.
 * 16 ms of settle, and this table reproduces the same commands with 0 except
 * at page switches.  The host side has now been aligned with the reference
 * driver as far as it can be, the link is demonstrably sending, and the panel
 * shows a picture that does NOT follow the framebuffer -- which is exactly
 * what a panel looks like when part of its programming never latched.  This is
 * the last untried variable on the panel side, so it goes in now rather than
 * after another round.
 *
 * Cost: ~190 commands x 16 ms = ~3 s of extra boot time.  DISPON latching is
 * still verified after the sequence, so a settle problem cannot pass silently.
 * The history worth remembering: with delay 0 the sequence once only worked
 * because debug logging happened to provide the settle (DISPON 0x9C vs 0x0C).
 * Set back to 0 to bisect. */

#define KICKPI_K7_INIT_CMD_SETTLE_MS 16

/* Panel DCS status registers worth reading back while the video stream runs.
 *
 * The single most valuable one is RDNUMED (05h): the panel counts every
 * received packet whose ECC/CRC failed, and READING IT CLEARS IT, so repeated
 * reads measure an error RATE.  That splits the two remaining explanations for
 * a dark panel apart:
 *
 *   RDNUMED non-zero  -> video packets ARE arriving, but damaged.  The fault
 *                        is in the physical link / lane timing, not the panel.
 *   RDNUMED zero      -> no corrupt packet was received.
 *
 * Read the zero case carefully: it is NOT proof that the panel is receiving
 * good video.  If nothing were transmitted at all, the counter would also stay
 * zero.  It must therefore be combined with the "did the data lanes ever leave
 * LP-11" evidence from the health snapshot before drawing a conclusion.
 */

#define KICKPI_K7_DCS_RDNUMED  0x05 /* Errors on DSI since the last read */
#define KICKPI_K7_DCS_RDDPM    0x0a /* Display power mode */
#define KICKPI_K7_DCS_RDDCOLMOD 0x0c /* Interface pixel format (expect 0x77) */
#define KICKPI_K7_DCS_RDIM     0x0d /* Display image mode */
#define KICKPI_K7_DCS_RDDSM    0x0e /* Display signal mode (bit0 = last pkt error) */
#define KICKPI_K7_DCS_RDDSDR   0x0f /* Self-diagnostic result */

/* Post-start health timeline: poll the live video path N times every P ms so
 * a pixel stream that stops (which makes the panel decay) is caught at the
 * moment it happens instead of being inferred from a frozen image. */

/* Ladder used by the VOP-off experiment and the payload probe.
 *
 * One sample cannot separate "the level is low because the consumer keeps up"
 * from "the level is frozen because nobody consumes it", and that difference
 * decides the whole diagnosis.  A ladder every 10 ms for 60 ms does separate
 * them, and its readings are printed as plain numbers so a truncated console
 * line still carries most of the answer -- the earlier single-sample version
 * of this test lost exactly the number that mattered. */

#define KICKPI_K7_GATE_LADDER_SAMPLES 6
#define KICKPI_K7_GATE_LADDER_STEP_MS 10

/* PHY_STATUS reads used for the lane-episode measurement.  6000 reads run for
 * a couple of milliseconds, so the 13-bit VOP scan counter cannot wrap, while
 * the number of lines that elapse is large enough for the comparison to be
 * meaningful. */

#define KICKPI_K7_EPISODE_SAMPLES 6000

/* Stream-ENTRY colour probe.
 *
 * WHY THIS EXISTS.  Every content probe in this project has so far changed the
 * framebuffer while the stream kept running, and none of them ever made the
 * panel show anything.  The FIRST time the panel responded to our content at
 * all was immediately after a stream RE-ENTRY: the operator reported, in the
 * SYNC_EVENTS build, "1/3 black, 2/3 white, the black turning white, pure
 * white, then fading to black".  A spatially structured image that then decays
 * is the signature of a panel that received a frame and then stopped being
 * refreshed -- which is a completely different fault from "receives nothing",
 * and it is exactly the shape of the ignored hypothesis from an early round:
 * the panel consumes its first frames after a stream entry and then stops.
 *
 * THE HYPOTHESIS THIS PROBE TESTS, in one sentence: the panel latches a frame
 * on each stream ENTRY, so a colour painted BEFORE a re-entry will be displayed
 * (for as long as the panel holds it), while the same colour painted during a
 * steady stream never appears at all.
 *
 * HOW IT STAYS READABLE.  Nothing here is subtle: each step paints ONE
 * whole-screen colour, FORCES a stream re-entry, announces the colour, and
 * holds for KICKPI_K7_ENTRY_DWELL_MS so the operator has time to look.  The
 * outcome is one of three, and each one names a different next step:
 *
 *   each colour appears after its own entry, then decays
 *       -> the panel consumes our pixels and the fault is the FRAMING: it
 *          latches once per entry and cannot hold a stream.  The work then
 *          moves to the per-line packetisation / framing question
 *   only WHITE appears, whatever was painted
 *       -> something saturates the pixel data to all-ones (a format or
 *          bit-order fault); solid colours would not be colour-specific
 *   nothing appears at all, for any colour
 *       -> the panel is not consuming on entry either: the steady-stream and
 *          entry paths fail the same way, and the fault is upstream
 *
 * The re-entry is done with the driver's own enable/disable pair, i.e. exactly
 * the transition that produced the observation, so the probe does not invent a
 * new mechanism to interpret. */

#define KICKPI_K7_ENTRY_PROBE 0
#define KICKPI_K7_ENTRY_DWELL_MS 3000

/* Mode x EoTp matrix -- the last host-side knob with a REFERENCE VALUE for
 * this panel IC.
 *
 * WHY THIS MATRIX, AND WHY NOW.  Every documented aspect of this panel's
 * configuration has been matched to a vendor source and none of them fixed the
 * picture: the init sequence is byte-identical to the module maker's own
 * device tree (verified mechanically, see the round that diffed it), the video
 * timing is identical, the pixel format is identical, the pixel clock now
 * equals what the link implies (so the panel and the host agree on how long a
 * line is), and the mode type was switched to what the vendor's DT resolves to.
 * The one host-side bit that still DISAGREES WITH A REFERENCE VALUE is EoTp:
 *
 *   Rockchip's own DTS for an ILI9881D panel (arch/arm/boot/dts/
 *   rv1126-evb-v10.dtsi, "ilitek,ili9881d"):
 *       dsi,flags = <MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
 *                    MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET>;
 *   and Linux defines MIPI_DSI_MODE_EOT_PACKET as "disable EoT packets in HS
 *   mode".  So the SoC vendor, driving THIS PANEL IC on THEIR OWN silicon,
 *   uses BURST with EoTp DISABLED -- while this driver has always written
 *   BTA_EN | EOTP_TX_EN, and every mode test it ever ran therefore had EoTp on.
 *   "Burst with EoTp off" has never been measured here.
 *
 *   The mainline ILI9881C tables agree with that split: the BURST panels set
 *   NO_EOT_PACKET, the SYNC_PULSE panels do not.
 *
 * HOW IT IS READ.  Six states, each with its own whole-screen colour announced
 * before it is shown, and each preceded by a real disable/enable re-entry so
 * the mode and the EoTp bit are actually applied.  Colours rather than patterns
 * because a solid colour is immune to geometry, shear, ordering and packing
 * faults -- so a solid colour appearing means the panel consumed our pixels,
 * full stop.  The transmitted structure is logged for each state too, which
 * says at no extra cost whether the re-entry changed the wire.
 *
 *   order: from the current configuration outwards, most likely first
 *     1 sync-events + EoTp on   (the configuration that has been running)
 *     2 burst       + EoTp on   (as historically tested)
 *     3 burst       + EoTp OFF  <-- Rockchip's own choice for this panel IC
 *     4 sync-events + EoTp OFF
 *     5 sync-pulses + EoTp OFF
 *     6 sync-pulses + EoTp on
 *
 * The configured default is restored at the end. */

/* *** TURNED OFF (1 -> 0) -- and why, as a correction to this round's own work.
 *
 * The matrix is OFF because it invalidates the measurement that used to come
 * AFTER it.  Each of its twelve states stops the stream, re-programs the link
 * and restarts it, and the black/white strobe was running after all twelve --
 * so "the strobe produced no visible change" was measured on a panel that had
 * just been stopped and restarted twelve times, which is exactly the
 * disturbance an earlier round identified as the thing that makes this panel
 * "work once and never again".  A verdict cannot rest on that.
 *
 * It also has no discriminating power left: all twelve combinations produce
 * the same picture.  The content tests below -- DRAM witness, pixel probe,
 * strobe, address echo -- now run on a stream that has been left alone.
 *
 * Re-enable (1) only to re-run the comparison; the code is unchanged. */

#define KICKPI_K7_MODE_EOTP_MATRIX 0
#define KICKPI_K7_MODE_EOTP_DWELL_MS 2500

/* Black/white strobe -- the plainest possible content test.
 *
 * WHY THIS AFTER THE MATRIX.  The matrix left one question open in a way that
 * earlier probes could not settle: the reports have been inconsistent about
 * whether the picture follows our CONTENT.  "Static garble that ignores content"
 * and "pattern changes when the configuration changes" are different faults, and
 * they point in different directions, so it needs an answer that cannot be
 * misread or half-observed.
 *
 * The test is a full-screen BLACK/WHITE alternation every 400 ms on an
 * UNDISTURBED stream -- nothing is stopped, re-entered or re-programmed during
 * it.  Two properties make the result unambiguous:
 *
 *   1. black and white are BYTE-UNIFORM (all 00 or all ff), so no byte-level
 *      fault can disguise them -- not bytes-per-pixel, not lane order, not lane
 *      skew, not bit order, not packing.  Whatever else is wrong, a stream that
 *      reaches the glass must make the screen flash.
 *   2. 400 ms is far too slow to miss and far too fast to mistake for the slow
 *      re-entry transient that every earlier observation was tangled with.
 *
 * The outcome is binary and names the fault class:
 *   the screen strobes black/white   -> the glass IS driven by our bytes, and
 *                                       the fault is in HOW they are framed or
 *                                       packed (geometry, format, timing)
 *   the screen does NOT strobe       -> our bytes are not reaching the glass at
 *                                       all, and no amount of content mapping
 *                                       work can fix that; the fault is upstream
 *                                       in the frame-assembly or PHY path
 *
 * Runs last, and does not restore anything: it is read-only with respect to the
 * stream and only writes framebuffer memory. */

/* Strobe -- ANSWERED, kept OFF.
 *
 * The black/white strobe was run on an undisturbed stream and produced NO
 * visible change.  That is a much stronger result than it first appears, and
 * it is positive evidence rather than a failed test: black and white are
 * BYTE-UNIFORM, and no receiver-side fault can turn a uniform image into
 * garble -- lane swap, lane count, byte order, bit order, packing, wrong
 * bytes-per-pixel, shear, frame folding and FIFO underrun all permute or
 * re-group IDENTICAL bytes and leave a uniform image uniform.
 *
 * So "garble that survives pure black and pure white" is only explainable one
 * way: the glass is not showing our bytes.  Which is what
 * kickpi_k7_mipi_dsi_address_echo_test() was built to confirm, and it did:
 * re-pointing the layer at a DIFFERENT physical buffer painted WHITE changed
 * nothing either.
 *
 * Between them the two tests close the whole framebuffer-to-panel path: the
 * bytes are correct in DRAM (verified), and neither their VALUE nor their
 * ADDRESS reaches the glass.  The remaining question is therefore not about
 * content at all but about WHICH PART OF THE COMPOSITOR supplies the pixels at
 * all, which is what rk3576_vop_source_probe() now asks.  Re-enable (1) to
 * re-run. */

#define KICKPI_K7_STROBE_TEST 0
#define KICKPI_K7_STROBE_MS 400
#define KICKPI_K7_STROBE_STEPS 20

/* Address echo -- does the ESMART layer fetch from the address we program?
 *
 * WHY THIS IS NOW THE FIRST-RANKED SUSPECT.  One fault explains every
 * observation this board has produced, and it explains them all at once:
 *
 *   - the image is static and never follows our content;
 *   - whole-screen colours, and even a byte-uniform black/white strobe,
 *     produce no visible change at all;
 *   - the picture DOES change when the link mode changes;
 *   - and yet the panel is perfectly able to show a clean image when its own
 *     generator drives it (BIST).
 *
 * That is exactly what you see if the layer fetches from a DIFFERENT physical
 * address than the one the driver painted: the panel faithfully displays some
 * other, arbitrary part of DRAM -- static, content-independent, and unaffected
 * by anything we write into the framebuffer.  Changing the link mode changes
 * how those bytes are framed, so the pattern shifts.  BIST bypasses our stream
 * entirely, so it is clean.
 *
 * Note what is NOT a witness here: the REGION0_YRGB_MST readback reports the
 * MIRROR register, and the mirror->real load is precisely the step this
 * driver's own comments warn can stay pending forever.  A correct readback and
 * a layer fetching from nowhere are fully consistent.
 *
 * The test alternates the fetch between two physically separate buffers --
 * the primary (painted BLACK) and a second buffer from the same identity-mapped
 * DMA heap (painted WHITE) -- announcing each switch.
 *
 * Binary outcome, and it does not depend on the panel decoding anything:
 *   the picture alternates black/white -> the fetch follows the register, so
 *       the layer IS reading our buffers and the fault is further downstream
 *       (framing / PHY / panel), NOT memory mapping;
 *   the picture never changes          -> the fetch does not follow the
 *       register.  The layer is reading elsewhere and every framebuffer result
 *       in this project has been measured on a buffer the display never saw.
 *
 * Runs last, because it repoints the layer's fetch address. */

/* Address echo -- ANSWERED (no reaction to either buffer), kept OFF.
 *
 * Alternating the ESMART layer's fetch between two physically separate
 * buffers -- the primary painted BLACK, a second buffer painted WHITE -- for
 * six announced steps produced NO change on the screen at all.
 *
 * Read together with the DRAM witness (MATCH: the bytes really are in DRAM,
 * so the clean does write back and the layer is not reading a stale buffer),
 * that closes the fetch-address axis: the layer is not being fed the address
 * we program, or it is not the window that feeds the POST.  Either way, every
 * content experiment in this project wrote `priv->fbmem` through ESMART0 and
 * was therefore structurally unable to change the picture.
 *
 * The next question is consequently not "which address" but "which source",
 * asked by rk3576_vop_source_probe().  Re-enable (1) to re-run. */

#define KICKPI_K7_ADDR_ECHO_TEST 0
#define KICKPI_K7_ADDR_ECHO_DWELL_MS 700
#define KICKPI_K7_ADDR_ECHO_STEPS 6

/* Auto-timing probe: OFF, and it must stay off unless someone re-opens the
 * question deliberately.
 *
 * It was built to make the controller report its own view of the IPI timing,
 * and it settled that question in one boot: clearing manual_mode_en is NOT the
 * AutoCalculation procedure (that is a separate operating mode which stops all
 * data reception), so the AUTO registers are not a measurement -- and worse,
 * with manual_mode_en=0 the controller STOPPED DRIVING THE DATA LANES
 * altogether (0% high speed, measured).  Leaving it enabled therefore kills a
 * live stream at the end of every boot, which is a high price for a probe that
 * answers nothing.  It also, ironically, produced the first positive panel
 * observation ever seen -- a stream re-entry makes the panel show a frame -- so
 * its most useful property has been moved into the deliberately-designed
 * stream-entry probe above. */

#define KICKPI_K7_AUTO_PROBE 0

#define KICKPI_K7_HEALTH_SAMPLES   12 /* 12 x 250 ms = 3 s */
#define KICKPI_K7_HEALTH_PERIOD_MS 250

/* Samples per observation AFTER a forced rearm.  Shorter than the boot window
 * because the rearm cycles are about comparing states, not about watching a
 * stream die, and every millisecond here delays the panel coming up. */

#define KICKPI_K7_REARM_SAMPLES 6 /* 6 x 250 ms = 1.5 s */

/* Samples per matrix entry: long enough for the panel to show a frame or two
 * and for the fingerprints to be meaningful, short enough that six entries
 * plus their panel reads still finish in a few seconds. */

#define KICKPI_K7_MATRIX_SAMPLES 4 /* 4 x 250 ms = 1 s */

/* Walk through every (video mode x clock lane type) combination at boot,
 * giving each one its own screen colour and its own panel error tally.
 *
 * *** SET TO 0 AS SOON AS A CONFIGURATION WORKS: *** re-programming a panel
 * that is already displaying is what previously produced "the display worked
 * once and never again".  With this at 0 only the single baseline
 * observation runs. */

/* Turned OFF this round.
 *
 * All six (mode x clock) combinations produced the SAME verdict (LINK RUNNING)
 * and the SAME picture, so the sweep has no discriminating power left; and
 * each entry stops, re-programs and restarts a panel that is already lit,
 * which is exactly what made the display "work once and never again" in an
 * earlier round.  What is in doubt now is not WHICH mode is used but whether
 * the panel follows the framebuffer at all, and that question needs an
 * UNDISTURBED stream -- seven restarts per boot are the opposite of that.
 *
 * Re-enable (1) only to re-run the comparison; the code is unchanged. */

#define KICKPI_K7_DSI_LINK_MATRIX 0

/* Content sweep.
 *
 * Replaces the single-mode colour test, because the BIST result moved the
 * question: the panel's own display chain is demonstrably healthy (BIST showed
 * clean white/black/red/green), the manufacturer registers demonstrably latch
 * (FRM_EN read back as written), the link is demonstrably clean (zero bad
 * packets over 12.5 s of undisturbed streaming) -- and yet whole-screen
 * colours painted into the framebuffer never appear.
 *
 * What is left is that the panel does not accept the STREAM as video: it
 * receives the packets, checks them, and does not put them on the glass.  So
 * the thing to vary is the stream's video mode, and the thing to look at is
 * whether a WHOLE-SCREEN colour appears.  Whole-screen matters: geometry and
 * byte-order faults cannot change a solid colour, so a colour appearing at all
 * proves the panel is consuming our pixels -- and a colour NOT appearing rules
 * out every ordering hypothesis at once.
 *
 * Which mode should win is not a guess.  This panel's init table and timing
 * match the mainline ILI9881D panels driven as SYNC_PULSE, and the closest
 * twin is `startek,kd050hdfia020` (720x1280, 4 lanes, RGB888, exactly our
 * porches) whose mode_flags are
 *
 *     MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_LPM
 *
 * i.e. NON-BURST with sync pulses and a CONTINUOUS clock.  This board has been
 * driving BURST, which is what the two `wanchanglong` and `bestar` panels use
 * -- and whose init tables differ from ours at exactly the register block that
 * carries the panel's own timing.
 *
 * Three candidates are swept in ONE boot, so a single flash answers it:
 * sync-pulses (the reference answer), sync-events and burst (what we ran).
 * Each is given the same three colours -- RED, GREEN, WHITE -- and the colours
 * are announced before they are painted, so the observer compares four states
 * per mode: the mode's own initial image, then red, then green, then white.
 *
 * The panel is read once at the end; per-mode error tallies are not needed to
 * answer this question and every extra stop/restart is another chance to
 * disturb the stream being judged. */

/* *** TURNED OFF (1 -> 0): the sweep has now been run and it cannot
 * discriminate.  All four (mode x clock) combinations produced the SAME
 * picture -- static garble that ignores whole-screen colours -- and the same
 * healthy fingerprint, so it has no measurement left to make.  And every entry
 * stops, re-programs and restarts a live panel (eight times per boot), which is
 * both wasted boot time and the one thing that must not happen while the pixel
 * probe below is being judged.
 *
 * Re-enable (1) only to re-run the comparison; the code is unchanged. */

#define KICKPI_K7_CONTENT_SWEEP 0
#define KICKPI_K7_CONTENT_DWELL_MS 1500

/* Pixel probe -- the experiment that replaces the mode/clock sweep.
 *
 * WHY A NEW PROBE IS NEEDED.  After all these rounds the control registers, the
 * PHY, the link occupancy and the panel's own command path are each verified,
 * and the one thing that has never been measured is the obvious one: do the
 * BYTES of our framebuffer reach the glass at all?  The picture alone cannot
 * answer it, because a solid colour is immune to almost every fault -- geometry,
 * shear, sync, sampling phase, refresh rate all leave a solid colour solid.
 *
 * THE ONE FAULT A SOLID COLOUR IS NOT IMMUNE TO is a byte-level misalignment: a
 * wrong number of bytes per pixel, a lane swap or lane-to-lane skew, or a byte-
 * or bit-order error.  Each of those re-arranges the bytes AROUND a solid
 * colour, turning a constant R,G,B stream into a fixed repeating multi-colour
 * dither.  That is exactly the signature this panel shows -- and it is also why
 * red, green and blue can all look identically garbled: the dither is built
 * from the same three bytes in a different rotation.
 *
 * A byte stream whose bytes are all EQUAL is immune even to that.  So the probe
 * paints eight full-height bars, each with every byte set to one constant
 * (00, 24, 49, 6d, 92, b6, db, ff -- a dark-to-bright ramp).  No byte-level
 * fault can turn those into anything but eight clean bands; only their
 * brightness can be wrong.  The answer is therefore binary:
 *
 *   eight clean bands            -> the glass IS driven by our bytes, so the
 *                                   fault is a byte-level misalignment (pixel
 *                                   format / lane order / skew): concrete and
 *                                   fixable
 *   eight bands, not a ramp      -> our bytes arrive and the panel packs them
 *                                   with a different bytes-per-pixel; the
 *                                   wrong mapping is visible in the levels
 *   noise, no bands at all       -> our bytes are NOT driving the glass, and
 *                                   every content test in this project so far
 *                                   has measured the panel's own output
 *
 * The probe then repeats the old whole-screen colour test and the eight-colour
 * bar pattern ON THE SAME UNDISTURBED STREAM, so the three observations can be
 * compared directly instead of across boots.
 *
 * Nothing here stops, re-times or re-programs the stream: it only writes the
 * framebuffer.  That is deliberate -- an earlier round destroyed its own
 * measurement by re-programming a panel that was already lit. */

#define KICKPI_K7_PIXEL_PROBE 1
#define KICKPI_K7_PIXEL_PROBE_DWELL_MS 2500

/* The mode sweep now varies the CLOCK LANE type as well, because that is the
 * axis with a physical argument behind it that has not been tested since the
 * panel init was corrected:
 *
 *   the panel's own datasheet documents THS-EXIT -- "time to drive LP-11 after
 *   HS burst" -- i.e. its clock lane DOES leave high speed after each burst.
 *   A clock lane that returns to LP-11 gives the panel a per-line and
 *   per-frame edge to re-synchronise on, which is what a RAM-less panel needs
 *   in order to frame a video stream at all.  This board has been running a
 *   CONTINUOUS clock lane, where that edge never occurs: the panel sees a
 *   free-running clock and an unbroken HS burst, and has nothing to lock a
 *   frame to.
 *
 * The earlier non-continuous measurements predate both the corrected init
 * sequence (0x39 long writes with 16 ms settle) and the corrected pixel
 * clock, so they are not evidence about the current configuration.
 *
 * Four states, each announced, each painting whole-screen RED then GREEN then
 * WHITE:
 *
 *   sync-pulses + non-continuous   (the datasheet's own clock behaviour)
 *   sync-events + non-continuous
 *   sync-pulses + continuous       (what the board runs today)
 *   burst       + non-continuous
 *
 * A whole-screen colour appearing in ANY of them is the answer; whole-screen
 * matters because no geometry or ordering fault can change a solid colour, so
 * a solid colour appearing proves the panel consumed our pixels. */

/* MIPI lane sequence / polarity / lane-count sweep -- KEPT DISABLED.
 *
 * Run once, and it turned the screen black from its second state onward: it
 * selected page 4 R00h (MIPI_LANE_SEL, the lane COUNT) before touching the
 * lane ORDER, the count change stopped reception, and every state after it
 * therefore measured a panel that had already lost the stream.  A confounded
 * experiment -- and a reminder to change one register at a time and to read
 * each one back first so its original value can be restored.
 *
 * It was also, in hindsight, chasing the wrong thing: the panel's own error
 * counter has been zero across 12.5 s of streaming, and the DSI payload CRC
 * covers the byte stream IN TRANSMISSION ORDER, so a wrong lane de-interleave
 * or byte order would fail the CRC and show up in that counter.  Ordering
 * faults are already excluded by the clean error count.
 *
 * The body is kept for reference.  The display-source test below replaced it.
 *
 * THIS WAS THE LAST UNTRIED CLASS OF REGISTER, and it is the classic cause of
 * exactly the signature this bring-up has spent many rounds on: the link is
 * clean (zero bad packets over 12.5 s), the panel is healthy (its own BIST
 * pattern displays perfectly), the manufacturer registers demonstrably latch,
 * the packets are well formed -- and the picture is garbled in a way that
 * ignores content, because a solid colour still comes out as a repeating
 * pattern while a whole-screen colour change produces no visible change.
 *
 * ILI9881D datasheet 3.2.1 / 3.3 / 5.4.12: the MIPI receiver's LANE SEQUENCE
 * AND POLARITY are not fixed -- they are selected by the hardware pins IM[2:0]
 * ("IM[2:0] pins are used to configure lane sequence and polarity"), and page 1
 * register B6h can override those pins from software:
 *
 *   B6h: IM_SW_EN (bit 7) | IM_SW[2:0] | RS_SW_EN | RS_SW[1:0]
 *        "IM_SW_EN: Enable/Disable the lane sequence and polarity from internal
 *         command setting. The external hardware pin IM[2:0] has no effect when
 *         IM_SW_EN is 1."
 *   B7h: LANSEL_SW_EN | LANSEL_SW  (lane number)
 *   page 4 R00h: MIPI_LANE_SEL "MIPI DSI lane number selection"
 *
 * If the module's IM strapping does not match the lane order this SoC drives,
 * every byte lands in the wrong lane: each 4-byte group is permuted, so a
 * solid colour becomes a stable four-colour repeat, white/black stay
 * white/black (all bytes equal), and no packet is ever malformed -- which is
 * precisely what has been observed, including the earlier report that the
 * black/white strobe came out as stripes while the framebuffer held eight
 * black-and-white-ish bars.
 *
 * Neither this driver nor the vendor device tree ever writes these registers,
 * so the panel has been relying on its hardware pins throughout.
 *
 * The sweep walks every IM_SW value because the mapping table in the datasheet
 * is an image and cannot be read from the text; a hit is unambiguous (a clean
 * full-screen colour appears where every other state shows garble).  Every
 * state is announced, and the states are applied the same way the mode sweep
 * applies its own: stop the stream (the only state in which CRI writes are
 * reliable), write, restart, paint one whole-screen colour.
 *
 * Set to 0 once answered. */

#define KICKPI_K7_LANE_SWEEP 0
#define KICKPI_K7_LANE_DWELL_MS 2000

/* Display-source test: is the glass being driven by the panel's OWN frame
 * memory (command mode), or by our video stream?
 *
 * WHY THIS IS THE QUESTION TO ASK, after everything else came back clean:
 *
 *   - the panel's own BIST pattern displays perfectly, so its source drivers,
 *     TCON, power and glass are all fine;
 *   - the init table is byte-for-byte the vendor's and manufacturer writes
 *     demonstrably latch, so the panel is configured as its vendor intends;
 *   - the link is clean -- zero bad packets over 12.5 s.  That is a stronger
 *     statement than it looks: the payload CRC covers the byte stream IN
 *     TRANSMISSION ORDER, so a wrong lane de-interleave, lane swap or byte
 *     order would make the CRC fail.  A clean error count therefore already
 *     rules out lane sequence/polarity and byte ordering, which is also why
 *     the lane sweep in the previous build was misdirected (and it turned the
 *     screen black from its second state onward, because it changed the lane
 *     COUNT register first, which stopped reception and invalidated every
 *     state after it -- a confounded experiment, worth recording as such).
 *
 *   - and yet the picture never follows our content: whole-screen solid
 *     colours never appear, and the image is static garble.
 *
 * An ILI9881D can be driven two ways.  In VIDEO mode it displays the incoming
 * packet stream directly.  In COMMAND mode it displays its own frame memory
 * (GRAM), which after power-on contains whatever it powered up with -- a
 * STATIC garbled image that ignores incoming video data, while still
 * accepting, checking and acknowledging those packets without reporting a
 * single error, because they are well formed; they are simply not what drives
 * the glass.
 *
 * That is a complete explanation of every measurement this bring-up has, and
 * it is testable with two standard DCS commands:
 *
 *   0x23 All Pixels On   -- in COMMAND mode the whole panel turns WHITE;
 *                           in VIDEO mode the incoming video wins, so nothing
 *                           changes.
 *   0x22 All Pixels Off  -- the same in black.
 *
 * If those do drive the screen, the panel is showing its GRAM, and the second
 * half of this test writes a solid colour into GRAM over DCS: if THAT appears
 * on the glass, the project has a working display path (command mode, written
 * by us) regardless of the video-mode question, and the picture can then be
 * brought up from there.
 *
 * Set to 0 once answered.
 *
 * *** TURNED OFF (1 -> 0) this round. ***  The premise of this test -- that the
 * garble is a stale frame memory being displayed -- is void: this panel module
 * has NO frame memory, so the glass is driven directly by the incoming stream
 * and there is no GRAM image to explain anything.  Keeping it would also cost
 * six seconds of boot time and leave the panel in a state (All Pixels On/Off,
 * then Normal Display On) that is not the state the pixel probe below is meant
 * to be judging.
 *
 * The output-stage question is not lost: the pixel probe answers it for free,
 * by distinguishing "the glass is driven by our bytes" from "the glass is
 * driven by something of the panel's own" without touching the panel at all.
 * The body is kept for reference. */

#define KICKPI_K7_GRAM_TEST   0
#define KICKPI_K7_GRAM_DWELL_MS 3000

/* *** VOP DIAGNOSTIC PROBES: OFF.  THE ROOT CAUSE IS FOUND. ***
 *
 * The layer's fetch was being starved by an outstanding-transaction cap this
 * driver placed on the VOP's AXI0 CHANNEL, the very channel ESMART0 and ESMART1
 * fetch through:
 *
 *     SYS_AXI0_CTRL_IMD  outstanding_en = 1, axi0_outstanding_num = 16
 *
 * A register dump from a working configuration reads 0 there -- no cap -- and
 * the working driver never writes the register at all.  The cap was added to
 * this driver on the theory that the scan-out DMA could monopolise the NoC and
 * starve the CPU; an earlier round tested that theory and rejected it, but the
 * write stayed.
 *
 * It was identified by putting it back: from the corrected configuration, one
 * rung re-set the window's own AXI deviations and another the permanently
 * asserted hurry request -- the colour bars SURVIVED both -- and the last rung
 * re-applied this SYS-level cap, at which point the bars vanished and the
 * garbage returned.  One register, added back on its own, and the screen broke.
 *
 * The correction is in the driver, so these probes now only disturb the screen
 * and cost boot time.  Set this to 1 to bring the whole diagnostic sequence
 * back; the panel must be lit, so anything added in future has to run after
 * step 6.
 */

#define KICKPI_K7_VOP_PROBES  0

/* First-frame capture test.
 *
 * This exists because of a detail in the reports that deserves to be followed
 * up: during the black/white strobe of an earlier round the screen showed
 * "black and white STRIPES", and this driver's own test pattern is eight
 * vertical stripes.  If the panel is capturing our very first frames, garbling
 * them, and then never updating again, everything else observed fits:
 *
 *   - the image is STATIC (frames after the first never frame correctly);
 *   - it does not follow content (nothing newer ever reaches the glass);
 *   - the panel's own pattern is perfect (the display chain is fine);
 *   - the link is clean (the packets are well-formed; it is the FRAME
 *     boundary the panel cannot use).
 *
 * That is a very different fault from "the panel ignores the stream", and the
 * two are easy to tell apart: paint ONE solid colour into the framebuffer
 * BEFORE video mode starts, so the panel's first frames are unmistakably
 * solid, then change the colour after the stream is running.
 *
 *   the screen takes on the pre-video colour  -> the panel DID consume our
 *       first frames; the fault is that it stops afterwards, which points at
 *       the frame/vertical boundary and not at the pixel format;
 *   the screen never changes at all          -> the panel never consumed them.
 *
 * Solid colours are the right probe for the same reason as before: no
 * ordering, packing or shear fault can turn a solid frame into stripes. */

#define KICKPI_K7_CAPTURE_TEST   0
#define KICKPI_K7_CAPTURE_DWELL_MS 4000

/* Panel built-in self-test pattern.
 *
 * The one experiment that takes OUR stream out of the loop completely, and the
 * only remaining way to split the two possibilities that every measurement so
 * far is consistent with:
 *
 *   the panel's own display chain is correctly programmed, and it is the
 *   incoming video stream it cannot use
 *       -> its internal pattern must appear, clean and static;
 *   the panel is not correctly programmed at all (its timing/power registers
 *   never latched, or it is running on OTP defaults)
 *       -> the pattern will not appear, or will itself be garbage, and every
 *          host-side result so far has been measured under a false premise.
 *
 * ILI9881D spec section 5.7.6 / 8.1 ("BIST Mode Function", page 4):
 *   0x2C = FRM_PT[7:0]   which patterns to enable
 *   0x2D = FRM_PT[15:8]
 *   0x2E = FRM_PT[17:16] (+ white_box_in_gray, FRM_CYC)
 *   0x2F bit0 = FRM_EN  enable the BIST free-running pattern
 * Table 33 names the patterns: FRM_PT[0]=White, [1]=Black, [2]=Red,
 * [3]=Green, [4]=Blue, [5]=Gray128, [6]=Gray127 -- so 0xFF enables all of
 * them and produces the panel's combined test image.
 *
 * It runs in COMMAND mode (no video stream, no pixel data at all) because that
 * is the only state in which the panel free-runs the pattern, and it is also
 * the state in which CRI writes are reliable.  The backlight is switched on
 * for the duration and BIST is disabled afterwards, so the normal bring-up
 * continues unchanged.
 *
 * Set to 0 once answered; it costs ~5 s of boot time. */

#define KICKPI_K7_BIST_TEST     0
#define KICKPI_K7_BIST_DWELL_MS 4000

/* Backlight: PWM1 channel 1 -> GPIO0_B5.  The controller index 1 selects
 * the second RK3576 PWM controller (PWM1 @ 0x2ADD0000, per the chip
 * hardware/rk3576_pwm.h enumeration RK3576_PWM1).  Registers the PWM as
 * /dev/pwm0 and starts it through the lower-half ops.
 */

#define KICKPI_K7_BL_PWM_CTRL 1 /* RK3576_PWM1 (PWM1 @ 0x2ADD0000) */
#define KICKPI_K7_BL_PWM_CH   1
#define KICKPI_K7_BL_DEVNAME  "pwm0"
#define KICKPI_K7_BL_FREQ_HZ  1000
#define KICKPI_K7_BL_DUTY     (1 << 15) /* 50% duty (ub16_t) */

/* Backlight PWM output pin: GPIO0_B5 muxed to PWM1_CH1_M0 (AF 0xc = 12,
 * per the RK3576 TRM IOMUX table for GPIO0_B5). */

#define KICKPI_K7_BL_PWM_PIN (GPIO_PORT0 | GPIO_PIN_B5)
#define KICKPI_K7_BL_PWM_AF  12

/* MIPI packet types used by this panel's DCS init sequence.
 *
 * CHANGED this round: every entry now goes out as 0x39, which is what the
 * vendor device tree actually says for the whole sequence
 * (`39 10 04 FF 98 81 03`, `39 10 02 01 00`, ...).  Until now the 2-byte
 * register writes used 0x23 -- "generic short write, 2 parameters" -- a form
 * the vendor table never uses.
 *
 * Why that was worth changing rather than dismissing: 0x23 and 0x39 carry the
 * same two bytes, so it is tempting to call them equivalent, and the evidence
 * that the sequence "works" (DISPON sets, RDDCOLMOD reads back) comes from
 * standard DCS commands.  Every manufacturer register this panel needs is
 * reached through the vendor's 0x39 form, and a page write that silently does
 * not latch is invisible from the host: the panel answers DCS reads, reports
 * no errors, and displays whatever its unprogrammed timing registers produce
 * -- a static garble that ignores the incoming pixels.  Reproducing the
 * vendor sequence VERBATIM removes that entire class of doubt, and costs
 * nothing when the two forms are indeed equivalent.
 *
 * 0x39 = DCS long write: payload[0] is the command, the rest are parameters.
 * 0x23 = generic short write, 2 parameters.  Both are still named here
 * because the page selects are 4-byte payloads and the register writes 2-byte;
 * after this change the two macros hold the same value, which is exactly what
 * the vendor table does.
 */

#define KICKPI_K7_PKT_GEN_LONG 0x39 /* DCS long write (4-byte page select) */
#define KICKPI_K7_PKT_DCS_LONG 0x39 /* DCS long write (2-byte register write) */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One entry of the panel DCS init sequence.  `data` carries the payload
 * bytes (after the DSI type/delay/word-count header), `type` is the raw
 * MIPI packet type to transmit (kept verbatim from the panel DT node), and
 * `delay_ms` is the wait after sending the command.
 */

struct kickpi_k7_mipi_dsi_cmd_s
{
  uint8_t type;            /* MIPI packet data type */
  uint8_t delay_ms;        /* Post-command delay in ms */
  uint8_t len;             /* Payload length (bytes) */
  FAR const uint8_t *data; /* Payload */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define _PANEL_INIT(...)              \
  .data = (uint8_t[]){ __VA_ARGS__ }, \
  .len = sizeof((uint8_t[]){ __VA_ARGS__ }) / sizeof(uint8_t)

/* Consolidated init sequence.  Each entry reproduces one line of the
 * `panel-init-sequence` DT node verbatim.  `type` is the raw MIPI packet
 * type (0x23 = DCS write, 0x39 = generic long write), `delay_ms` is the
 * post-command wait decoded from the DT second byte (0x78 = 120 ms for
 * sleep-out, 0x14 = 20 ms for display-on), and `data`/`len` are the payload
 * bytes that follow the word-count field.
 */

/* NOTE on page-switch timing: every vendor page switch (0xFF 98 81 xx)
 * MUST carry its own post-command delay.  The panel needs a settle time
 * after the page register is re-targeted before the following register
 * writes are latched; with delay_ms = 0 the sequence only worked by
 * accident, because the debug probe that used to follow the first
 * page-switch burned a few ms of UART time.  Removing that probe made the
 * DISPON bit stop sticking (GET_POWER_MODE read back 0x0C instead of
 * 0x9C).  The delay is therefore explicit below -- never rely on logging
 * to provide it. */

static const struct kickpi_k7_mipi_dsi_cmd_s g_kickpi_k7_mipi_dsi_init[] = {
  { KICKPI_K7_PKT_GEN_LONG, 5, _PANEL_INIT(0xFF, 0x98, 0x81, 0x03) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x01, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x02, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x03, 0x73) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x04, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x05, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x06, 0x0a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x07, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x08, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x09, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0a, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0b, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0c, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0d, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0e, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x0f, 0x1d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x10, 0x1d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x11, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x12, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x13, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x14, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x15, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x16, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x17, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x18, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x19, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1a, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1b, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1c, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1d, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1e, 0x40) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x1f, 0x80) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x20, 0x06) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x21, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x22, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x23, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x24, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x25, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x26, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x27, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x28, 0x33) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x29, 0x03) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2a, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2b, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2c, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2d, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2e, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x2f, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x30, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x31, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x32, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x33, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x34, 0x04) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x35, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x36, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x37, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x38, 0x3c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x39, 0x35) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3a, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3b, 0x40) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3c, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3d, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3e, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3f, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x40, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x41, 0x88) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x42, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x43, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x44, 0x1f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x50, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x51, 0x23) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x52, 0x45) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x53, 0x67) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x54, 0x89) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x55, 0xab) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x56, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x57, 0x23) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x58, 0x45) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x59, 0x67) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5a, 0x89) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5b, 0xab) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5c, 0xcd) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5d, 0xef) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5e, 0x11) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x5f, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x60, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x61, 0x15) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x62, 0x14) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x63, 0x0e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x64, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x65, 0x0c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x66, 0x0d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x67, 0x06) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x68, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x69, 0x07) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6a, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6b, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6c, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6d, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6e, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x6f, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x70, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x71, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x72, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x73, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x74, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x75, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x76, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x77, 0x14) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x78, 0x15) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x79, 0x0e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7a, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7b, 0x0c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7c, 0x0d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7d, 0x06) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7e, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x7f, 0x07) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x80, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x81, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x82, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x83, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x84, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x85, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x86, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x87, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x88, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x89, 0x02) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x8A, 0x02) },

  /* Page 0x04 register block. */
  { KICKPI_K7_PKT_GEN_LONG, 5, _PANEL_INIT(0xff, 0x98, 0x81, 0x04) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x70, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x71, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x82, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x84, 0x0f) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x85, 0x0d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x32, 0xac) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x8c, 0x80) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3c, 0xf5) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xb5, 0x07) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x31, 0x45) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3a, 0x24) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x88, 0x33) },

  /* Page 0x01 gamma correction block. */
  { KICKPI_K7_PKT_GEN_LONG, 5, _PANEL_INIT(0xff, 0x98, 0x81, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x22, 0x09) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x31, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x53, 0x8a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x55, 0xa2) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x50, 0x81) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x51, 0x85) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x62, 0x0D) },

  /* The vendor table selects page 0x01 a SECOND time here, immediately
   * before the positive/negative gamma blocks.  Reproduced verbatim: a
   * repeated page select is harmless if redundant, but it is a real
   * difference from the blog table and hints that the vendor sequence was
   * assembled from two independently released blocks. */
  { KICKPI_K7_PKT_GEN_LONG, 5, _PANEL_INIT(0xff, 0x98, 0x81, 0x01) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA0, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA1, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA2, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA3, 0x13) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA4, 0x16) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA5, 0x29) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA6, 0x1d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA7, 0x1e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA8, 0x84) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xA9, 0x1c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAA, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAB, 0x75) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAC, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAD, 0x19) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAE, 0x4d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xAF, 0x22) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB0, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB1, 0x54) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB2, 0x66) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xB3, 0x39) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC0, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC1, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC2, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC3, 0x13) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC4, 0x16) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC5, 0x29) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC6, 0x1d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC7, 0x1e) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC8, 0x84) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xC9, 0x1c) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCA, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCB, 0x75) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCC, 0x1a) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCD, 0x19) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCE, 0x4d) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xCF, 0x22) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD0, 0x28) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD1, 0x54) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD2, 0x66) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0xD3, 0x39) },

  /* Page 0x00 (normal). */
  { KICKPI_K7_PKT_GEN_LONG, 5, _PANEL_INIT(0xff, 0x98, 0x81, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x35, 0x00) },
  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x36, 0x03) },

  /* Set the interface pixel format explicitly (3Ah = 0x77 = 24 bpp RGB888).
   *
   * The vendor table does not carry this command, and ILI9881D's RDDCOLMOD
   * (0Ch) reset default is 0x07, which is ALSO 24 bpp -- so on a factory-fresh
   * panel this is normally a no-op.  It is written anyway because "the panel
   * is set to something other than what the host transmits" is otherwise an
   * invisible degree of freedom: the host sends RGB888 either way, and any
   * TCON configured for 16/18 bpp would reinterpret every pixel.  Reading
   * 0Ch back (see the panel status probes) now closes that question instead
   * of assuming it. */

  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x3a, 0x77) },

  { KICKPI_K7_PKT_DCS_LONG, 120,
    _PANEL_INIT(0x11, 0x00) }, /* 23 78: sleep out, 120ms  */
  { KICKPI_K7_PKT_DCS_LONG, 20,
    _PANEL_INIT(0x29, 0x00) }, /* 23 14: display on, 20ms  */
};

#undef _PANEL_INIT

/* Panel video-mode timing (feeding both the DSI IPI and the VOP). */

static const struct rk3576_dsi_video_timing g_kickpi_k7_mipi_dsi_timing = {
  .hactive = KICKPI_K7_MIPI_DSI_XRES,
  .hfront_porch = KICKPI_K7_HFRONT_PORCH,
  .hback_porch = KICKPI_K7_HBACK_PORCH,
  .hsync_len = KICKPI_K7_HSYNC_LEN,
  .vactive = KICKPI_K7_MIPI_DSI_YRES,
  .vfront_porch = KICKPI_K7_VFRONT_PORCH,
  .vback_porch = KICKPI_K7_VBACK_PORCH,
  .vsync_len = KICKPI_K7_VSYNC_LEN,
  .pixel_clock = KICKPI_K7_MIPI_DSI_PIXCLK,
};

/* GPIO handles claimed once in configure_pins() and cached here.  They are
 * deliberately NOT re-acquired later: the RK3576 GPIO driver enforces
 * single-occupancy, so a second rk3576_gpio_get() on the same pin fails
 * with -EBUSY (and the board code would otherwise silently fail to release
 * the panel reset).
 */

static FAR struct gpio_dev_s *g_kickpi_k7_mipi_dsi_pwren = NULL;
static FAR struct gpio_dev_s *g_kickpi_k7_mipi_dsi_rst = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_configure_pins
 *
 * Description:
 *   Claim and drive the panel control pins: LCD_RST and LCD_PWREN as
 *   outputs.  The DSI data/clock lanes use dedicated pins (no GPIO mux).
 *   The backlight PWM pin is muxed by the PWM driver path.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_configure_pins(void)
{
  int ret;

  /* Power enable: claim it and drive it LOW (panel hard off).  The actual
   * power-up happens in kickpi_k7_mipi_dsi_panel_cycle(), which drives the
   * full off -> on -> reset -> release sequence. */

  if (!g_kickpi_k7_mipi_dsi_pwren)
    {
      ret = rk3576_gpio_get(KICKPI_K7_MIPI_DSI_PWREN,
                            &g_kickpi_k7_mipi_dsi_pwren);
      DEBUGASSERT(ret == OK);
    }

  rk3576_gpio_set_mode(g_kickpi_k7_mipi_dsi_pwren, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_pwren, false);

  /* Reset: claim it and hold it asserted (LCD_RST driven low = panel reset).
   * The handle is claimed once here and cached; the panel_cycle() helper
   * reuses it instead of re-acquiring, because a second rk3576_gpio_get() on
   * the same pin would be rejected by the single-occupancy check (-EBUSY).
   */

  if (!g_kickpi_k7_mipi_dsi_rst)
    {
      ret = rk3576_gpio_get(KICKPI_K7_MIPI_DSI_RST, &g_kickpi_k7_mipi_dsi_rst);
      DEBUGASSERT(ret == OK);
    }

  rk3576_gpio_set_mode(g_kickpi_k7_mipi_dsi_rst, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_rst, false);
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_panel_cycle
 *
 * Description:
 *   Run the panel through a complete power-off / power-on / reset cycle so
 *   that the following DCS init sequence is applied to a panel that has had
 *   a genuine power-on reset:
 *
 *     PWREN low + RST asserted  ->  settle (rails decay)
 *     PWREN high                ->  settle (t_vdd stable; DT power-delay-ms)
 *     RST released              ->  settle (before the first DCS command)
 *
 *   Taking PWREN low first is the point: without it, a warm reboot leaves the
 *   panel powered, so its own POR never runs and the init writes can be
 *   silently ignored.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_panel_cycle(void)
{
  /* Off: power removed and reset asserted simultaneously. */

  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_rst, false);
  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_pwren, false);
  up_mdelay(KICKPI_K7_POWER_OFF_MS);

  /* Power up, reset still asserted. */

  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_pwren, true);
  up_mdelay(KICKPI_K7_PWREN_SETTLE_MS);

  /* Hold reset asserted long enough to be seen, then release it. */

  up_mdelay(KICKPI_K7_RST_ASSERT_MS);
  rk3576_gpio_write_bit(g_kickpi_k7_mipi_dsi_rst, true);
  up_mdelay(KICKPI_K7_RST_RELEASE_MS);
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_backlight_enable
 *
 * Description:
 *   Mux GPIO0_B5 to the PWM1_CH1 alternate function, then bring up PWM1
 *   channel 1 as the backlight driver and start it at the configured duty
 *   cycle.  The pin mux must be done before rk3576_pwm_initialize() — the
 *   PWM lower-half driver never configures GPIO (see its header comment).
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_backlight_enable(void)
{
  static FAR struct gpio_dev_s *bl_handle = NULL;
  static FAR struct pwm_lowerhalf_s *bl_pwm = NULL;
  struct pwm_info_s info;
  int ret;

  /* Everything here must be idempotent, because this function is called twice
   * per boot: once before the panel self-test (so its pattern is visible) and
   * once before the health watch.  A second pwm_register() with the same
   * device name fails, and failing there used to skip setup()/start() and log
   * "backlight PWM register failed" -- a line that reads like a fault in every
   * log while the backlight is in fact running from the first call. */

  /* Claim the backlight pin once and route it to PWM1_CH1_M0 (AF12). */

  if (bl_handle == NULL)
    {
      ret = rk3576_gpio_get(KICKPI_K7_BL_PWM_PIN, &bl_handle);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: backlight GPIO claim failed: %d\n", ret);
          return;
        }
    }

  rk3576_gpio_set_af(bl_handle, KICKPI_K7_BL_PWM_AF);

  if (bl_pwm == NULL)
    {
      bl_pwm = rk3576_pwm_initialize(KICKPI_K7_BL_PWM_CTRL,
                                     KICKPI_K7_BL_PWM_CH);
      if (bl_pwm == NULL)
        {
          syslog(LOG_ERR, "ERROR: backlight PWM initialize failed\n");
          return;
        }

      if (pwm_register(KICKPI_K7_BL_DEVNAME, bl_pwm) < 0)
        {
          syslog(LOG_ERR, "ERROR: backlight PWM register failed\n");
          return;
        }

      bl_pwm->ops->setup(bl_pwm);

      syslog(LOG_INFO, "kickpi-k7: backlight PWM set up (%u Hz, 50%% duty)\n",
             (unsigned)KICKPI_K7_BL_FREQ_HZ);
    }

  memset(&info, 0, sizeof(info));
  info.frequency = KICKPI_K7_BL_FREQ_HZ;
#ifdef CONFIG_PWM_MULTICHAN
  info.channels[0].duty = KICKPI_K7_BL_DUTY;
#else
  info.duty = KICKPI_K7_BL_DUTY;
#endif

  bl_pwm->ops->start(bl_pwm, &info);
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_send_init_sequence
 *
 * Description:
 *   Send the panel DCS init sequence verbatim.  Each entry is transmitted
 *   with its original packet type via mipi_dsi_transfer(), then any
 *   decoded delay is applied with up_mdelay().
 *
 ****************************************************************************/

static int
kickpi_k7_mipi_dsi_send_init_sequence(FAR struct mipi_dsi_device *device)
{
  int i;
  int ncmds = nitems(g_kickpi_k7_mipi_dsi_init);

#if KICKPI_K7_INIT_CMD_SETTLE_MS > 0
  syslog(LOG_INFO,
         "kickpi-k7: init: %d commands, +%d ms settle each = ~%d ms total\n",
         ncmds, KICKPI_K7_INIT_CMD_SETTLE_MS,
         (ncmds * KICKPI_K7_INIT_CMD_SETTLE_MS) / 1000);
#endif

  for (i = 0; i < ncmds; i++)
    {
      FAR const struct kickpi_k7_mipi_dsi_cmd_s *cmd =
          &g_kickpi_k7_mipi_dsi_init[i];
      struct mipi_dsi_msg msg;
      ssize_t ret;

      memset(&msg, 0, sizeof(msg));
      msg.channel = KICKPI_K7_DSI_VC;
      msg.type = cmd->type;
      msg.tx_buf = cmd->data;
      msg.tx_len = cmd->len;

      ret = mipi_dsi_transfer(device, &msg);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: DSI init cmd %d failed: %zd\n", i, ret);
          return (int)ret;
        }

      /* Settle after EVERY command, using whichever is longer: the entry's own
       * delay or the configured floor.
       *
       * The vendor table carries `10` (16 ms) after every single line, page
       * selects included; our page selects carried 5 ms, so before this change
       * they were the only entries that did NOT get the vendor's settle time.
       * Taking the maximum reproduces the vendor sequence exactly instead of
       * leaving one class of command short -- and a page select is precisely
       * the command whose failure is silent (the following writes then land in
       * the wrong page and the panel's timing registers keep their defaults).
       */

      {
        uint32_t settle = cmd->delay_ms;

#if KICKPI_K7_INIT_CMD_SETTLE_MS > 0
        if (settle < KICKPI_K7_INIT_CMD_SETTLE_MS)
          {
            settle = KICKPI_K7_INIT_CMD_SETTLE_MS;
          }
#endif

        if (settle > 0)
          {
            up_mdelay(settle);
          }
      }
    }

  return OK;
}

static void kickpi_k7_mipi_dsi_panel_status(FAR struct mipi_dsi_device *dev,
                                           FAR const char *tag);
static void kickpi_k7_mipi_dsi_cmd_fsm_report(void);

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_panel_bringup
 *
 * Description:
 *   Power-cycle the panel, send the DCS init sequence, then VERIFY that the
 *   panel actually latched it by reading MIPI_DCS_GET_POWER_MODE (0x0A) back
 *   over the link (a bus-turnaround read, so it only succeeds if the panel is
 *   genuinely alive and answering).
 *
 *   Healthy panels read back 0x9C here; the DISPON bit (0x80) is what the
 *   trailing DCS 0x29 in the init table sets, and it is precisely the bit that
 *   has been observed to drop to 0x0C when the panel did not latch the
 *   sequence.  So on a failed verify, power-cycle the panel again and re-send
 *   the whole table -- which needs no host-side state changes because the DSI
 *   host is still in Command mode at this point.
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_panel_bringup(FAR struct mipi_dsi_device *dev)
{
  int attempt;

  /* One-time legend for the compact per-read line printed below.  The console
   * drops characters from long lines, so the explanation lives here once and
   * the per-read line carries only the values. */

  syslog(LOG_INFO,
         "kickpi-k7: panel reg map: 0e=RDDSM 0f=RDDSDR(self-test) 0a=RDDPM "
         "(DISPON=bit7) 0c=RDDCOLMOD(07=RGB888) 0d=RDIM 05=RDNUMED "
         "(bad-packet count, read-clears); 0xff = the read failed\n");

  for (attempt = 0; attempt <= KICKPI_K7_INIT_RETRIES; attempt++)
    {
      uint8_t pwrmode = 0;
      ssize_t n;
      int ret;

      kickpi_k7_mipi_dsi_panel_cycle();

      ret = kickpi_k7_mipi_dsi_send_init_sequence(dev);
      if (ret < 0)
        {
          syslog(LOG_WARNING,
                 "kickpi-k7: init attempt %d failed to drain (%d)\n",
                 attempt + 1, ret);
          continue;
        }

      n = mipi_dsi_dcs_read(dev, MIPI_DCS_GET_POWER_MODE, &pwrmode, 1);
      if (n == 1 && (pwrmode & 0x80) != 0)
        {
          syslog(LOG_INFO,
                 "kickpi-k7: panel up on attempt %d, power mode = 0x%02x\n",
                 attempt + 1, pwrmode);

          /* The DSI host is still in Command mode here, which is the ONLY
           * safe place to interrogate the panel: in video mode a CRI
           * transfer needs a BLLP window, and a wedged video transmitter
           * never provides one (see the health timeline). */

          kickpi_k7_mipi_dsi_panel_status(dev, "init");

          /* The ~200 HS commands just sent are also the control group for
           * the "is the phy_tx_ready FSM observable at all?" experiment. */

          kickpi_k7_mipi_dsi_cmd_fsm_report();
          return OK;
        }

      syslog(LOG_WARNING,
             "kickpi-k7: init attempt %d: power mode read %d, value 0x%02x "
             "(DISPON not set) -- power-cycling the panel and retrying\n",
             attempt + 1, (int)n, pwrmode);
    }

  syslog(LOG_ERR,
         "kickpi-k7: panel never reported DISPON after %d attempts\n",
         KICKPI_K7_INIT_RETRIES + 1);
  return -EIO;
}

#if KICKPI_K7_DSI_LINK_MATRIX
static int kickpi_k7_mipi_dsi_reenter(FAR struct mipi_dsi_host *host,
                                      FAR struct mipi_dsi_device *dev,
                                      FAR const char *tag, uint8_t mode,
                                      bool continuous, uint32_t rgb);
#endif
static void kickpi_k7_mipi_dsi_observe(FAR const char *tag, int samples,
                                       bool with_vop_off);
#if KICKPI_K7_LANE_SWEEP
static void kickpi_k7_mipi_dsi_lane_sweep(FAR struct mipi_dsi_host *host,
                                          FAR struct mipi_dsi_device *dev);
#endif
#if KICKPI_K7_GRAM_TEST
static int kickpi_k7_mipi_dsi_gram_test(FAR struct mipi_dsi_device *dev);
#endif
#if KICKPI_K7_CONTENT_SWEEP
static void kickpi_k7_mipi_dsi_content_sweep(FAR struct mipi_dsi_host *host);
#endif
#if KICKPI_K7_CAPTURE_TEST
static void kickpi_k7_mipi_dsi_capture_test(void);
#endif
#if KICKPI_K7_BIST_TEST
static int kickpi_k7_mipi_dsi_bist_test(FAR struct mipi_dsi_device *dev);
#endif

/* Defined further down; declared here because bist_clear() needs it and is
 * deliberately placed next to the BIST switch it exists to clean up. */

static int kickpi_k7_mipi_dsi_write2(FAR struct mipi_dsi_device *dev,
                                     uint8_t reg, uint8_t value);

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_bist_clear
 *
 * Description:
 *   *** MAKE SURE THE PANEL IS NOT RUNNING ITS OWN TEST PATTERN. ***
 *
 *   This panel's BIST is a FREE-RUNNING pattern that the panel generates from
 *   its own registers.  While it is enabled the panel has no use for the
 *   incoming video stream, so ANYTHING on the glass is the panel's own image
 *   and every host-side conclusion drawn from looking at the screen is void.
 *
 *   The BIST probe in this file enables it on EVERY boot and turns it off about
 *   four seconds later -- and the turn-off was written as a bare
 *  
 *       kickpi_k7_mipi_dsi_write2(dev, 0x2f, 0x00);
 *
 *   with the return value discarded, no read-back, and FRM_PT (0x2C/0x2D/0x2E)
 *   LEFT AT 0xFF -- i.e. every pattern still armed.  So there was no evidence
 *   whatsoever that the panel ever left BIST mode, and if it did not, the panel
 *   has been free-running a pattern that looks like "a regular white/black test
 *   matrix" over the top of the entire bring-up.  The BIST probe is now
 *   disabled; this function exists so that state cannot survive anyway.
 *
 *   It clears the pattern selection AND the enable, then READS THE ENABLE BACK
 *   and reports it, because an unchecked write is exactly how the previous
 *   version came to be trusted.
 *
 * Returned Value:
 *   OK if the enable reads back clear, -EIO otherwise.
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_bist_clear(FAR struct mipi_dsi_device *dev)
{
  static const uint8_t page4[4] = { 0xff, 0x98, 0x81, 0x04 };
  static const uint8_t page0[4] = { 0xff, 0x98, 0x81, 0x00 };
  struct mipi_dsi_msg msg;
  uint8_t rb;
  uint8_t before = 0xa5;
  int ret;

  memset(&msg, 0, sizeof(msg));
  msg.channel = KICKPI_K7_DSI_VC;
  msg.type = KICKPI_K7_PKT_GEN_LONG;
  msg.tx_buf = page4;
  msg.tx_len = sizeof(page4);

  if (mipi_dsi_transfer(dev, &msg) < 0)
    {
      syslog(LOG_ERR,
             "kickpi-k7: BIST CLEAR: page 4 select failed -- cannot tell "
             "whether the panel is\n");
      syslog(LOG_ERR,
             "kickpi-k7:   free-running its own test pattern ***\n");
      return -EIO;
    }

  /* Read the enable BEFORE clearing, so the log says whether the panel had in
   * fact been left in BIST mode. */

  if (mipi_dsi_dcs_read(dev, 0x2f, &before, 1) != 1)
    {
      before = 0xa5;
    }

  /* Clear the pattern selection first, then the enable -- and never leave
   * FRM_PT armed the way the old turn-off did. */

  (void)kickpi_k7_mipi_dsi_write2(dev, 0x2c, 0x00);
  (void)kickpi_k7_mipi_dsi_write2(dev, 0x2d, 0x00);
  (void)kickpi_k7_mipi_dsi_write2(dev, 0x2e, 0x00);

  if (kickpi_k7_mipi_dsi_write2(dev, 0x2f, 0x00) < 0)
    {
      syslog(LOG_ERR,
             "kickpi-k7: BIST CLEAR: the enable write itself FAILED\n");
    }

  up_udelay(200);

  rb = 0xff;
  ret = mipi_dsi_dcs_read(dev, 0x2f, &rb, 1);

  syslog(LOG_WARNING,
         "kickpi-k7: BIST CLEAR: page4 FRM_EN was 0x%02x before, write 0x00, "
         "reads 0x%02x%s\n",
         (unsigned)before, (unsigned)rb,
         ret == 1 ? "" : " (read failed)");

  if (ret == 1 && rb != 0x00)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   *** THE PANEL IS STILL IN BIST MODE: FRM_EN reads "
             "0x%02x after being\n", (unsigned)rb);
      syslog(LOG_ERR,
             "kickpi-k7:   cleared.  While it is set the panel free-runs its "
             "OWN pattern and\n");
      syslog(LOG_ERR,
             "kickpi-k7:   IGNORES our video stream, so everything seen on "
             "the glass describes the\n");
      syslog(LOG_ERR,
             "kickpi-k7:   panel's test image, not this driver's output.  "
             "This would invalidate every\n");
      syslog(LOG_ERR,
             "kickpi-k7:   screen observation made in this project ***\n");
    }
  else if (before == 0x01)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   *** AND IT WAS SET (0x01) AT THIS POINT: a "
             "previous boot left the panel\n");
      syslog(LOG_ERR,
             "kickpi-k7:   free-running its test pattern, so the pattern seen "
             "until now was the\n");
      syslog(LOG_ERR,
             "kickpi-k7:   PANEL'S OWN, not a framebuffer image.  It has now "
             "been cleared ***\n");
    }

  /* Back to page 0 so the normal sequence is unaffected. */

  memset(&msg, 0, sizeof(msg));
  msg.channel = KICKPI_K7_DSI_VC;
  msg.type = KICKPI_K7_PKT_GEN_LONG;
  msg.tx_buf = page0;
  msg.tx_len = sizeof(page0);
  (void)mipi_dsi_transfer(dev, &msg);

  return (ret == 1 && rb == 0x00) ? OK : -EIO;
}
static int kickpi_k7_mipi_dsi_panel_probe(FAR struct mipi_dsi_host *host,
                                          FAR struct mipi_dsi_device *dev,
                                          FAR const char *tag);

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_write2
 *
 * Description:
 *   Send one panel register write as a DCS long write: payload = [reg, value].
 *   This is the vendor table's own form (`39 10 02 XX YY`), and the DSI driver
 *   routes 0x39 through the long-packet path with word count = len - 1.
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_write2(FAR struct mipi_dsi_device *dev,
                                     uint8_t reg, uint8_t value)
{
  uint8_t payload[2];
  struct mipi_dsi_msg msg;

  payload[0] = reg;
  payload[1] = value;

  memset(&msg, 0, sizeof(msg));
  msg.channel = KICKPI_K7_DSI_VC;
  msg.type = KICKPI_K7_PKT_DCS_LONG;
  msg.tx_buf = payload;
  msg.tx_len = sizeof(payload);

  return mipi_dsi_transfer(dev, &msg) < 0 ? -EIO : OK;
}

#if KICKPI_K7_BIST_TEST
/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_bist_test
 *
 * Description:
 *   Turn on the panel's built-in self-test pattern, wait, and turn it off.
 *   See KICKPI_K7_BIST_TEST for what the two possible outcomes mean.
 *
 *   Runs in Command mode with no video stream running, so nothing we transmit
 *   can influence what is on the screen: whatever appears is generated by the
 *   panel from its own register state.
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_bist_test(FAR struct mipi_dsi_device *dev)
{
  static const struct
  {
    uint8_t reg;
    uint8_t value;
  } on[] =
  {
    /* Page 4, then the pattern. */
    { 0x2c, 0xff }, /* FRM_PT[7:0]: enable every listed pattern */
    { 0x2d, 0x00 }, /* FRM_PT[15:8] */
    { 0x2e, 0x00 }, /* FRM_PT[17:16], white_box_in_gray, FRM_CYC */
    { 0x2f, 0x01 }, /* FRM_EN */
  };

  int i;
  int ret;

  syslog(LOG_WARNING,
         "kickpi-k7: BIST TEST: asking the PANEL to generate its own test "
         "pattern -- our stream is NOT involved\n");

  /* Page select 4: the same 4-byte form the vendor table uses. */

  {
    static const uint8_t page4[4] = { 0xff, 0x98, 0x81, 0x04 };
    struct mipi_dsi_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.channel = KICKPI_K7_DSI_VC;
    msg.type = KICKPI_K7_PKT_GEN_LONG;
    msg.tx_buf = page4;
    msg.tx_len = sizeof(page4);

    if (mipi_dsi_transfer(dev, &msg) < 0)
      {
        syslog(LOG_ERR,
               "kickpi-k7: BIST: page 4 select failed -- cannot run the "
               "test\n");
        return -EIO;
      }
  }

  for (i = 0; i < (int)nitems(on); i++)
    {
      if (kickpi_k7_mipi_dsi_write2(dev, on[i].reg, on[i].value) < 0)
        {
          syslog(LOG_ERR, "kickpi-k7: BIST: write %02x=%02x failed\n",
                 (unsigned)on[i].reg, (unsigned)on[i].value);
          return -EIO;
        }
    }

  syslog(LOG_WARNING,
         "kickpi-k7: BIST ON for %d ms -- WATCH THE SCREEN NOW and remember "
         "exactly what it shows\n", KICKPI_K7_BIST_DWELL_MS);

  /* Read a manufacturer register back while it is set.
   *
   * This is the one measurement that turns "did the init sequence actually
   * latch?" from an assumption into a reading.  Everything verified so far
   * (DISPON, RDDCOLMOD) is a STANDARD DCS command; the registers that carry
   * this panel's timing, power and gamma settings are manufacturer registers
   * reached through page selects, and a write to those that silently does not
   * land leaves no trace whatsoever on the host side -- the panel still
   * answers DCS reads, still reports zero errors, and still displays an image,
   * just not one derived from our pixels.
   *
   * FRM_EN (page 4, 0x2F) is readable (the spec marks the BIST block "W/R"),
   * so it is a fair witness for the whole block: if it reads back as written,
   * the page mechanism and the write path work; if it reads back 0x00 the
   * manufacturer writes are NOT reaching the registers, and every host-side
   * result obtained so far was measured under that condition.
   */

  {
    uint8_t rb = 0xff;

    if (mipi_dsi_dcs_read(dev, 0x2f, &rb, 1) == 1)
      {
        if (rb == 0x01)
          {
            syslog(LOG_WARNING,
                   "kickpi-k7: LATCH CHECK: page4 FRM_EN read back 0x%02x = "
                   "as written -> manufacturer register writes DO land\n",
                   (unsigned)rb);
          }
        else
          {
            syslog(LOG_ERR,
                   "kickpi-k7: LATCH CHECK: page4 FRM_EN read back 0x%02x but "
                   "0x01 was written -> manufacturer writes may NOT be "
                   "latching; the panel is running on its own defaults and "
                   "every content test so far measured that, not our stream\n",
                   (unsigned)rb);
          }
      }
    else
      {
        syslog(LOG_WARNING,
               "kickpi-k7: LATCH CHECK: page4 FRM_EN is not readable this way "
               "-- inconclusive, no conclusion drawn\n");
      }
  }

  up_mdelay(KICKPI_K7_BIST_DWELL_MS);

  /* Turn it off and return to page 0 so the normal sequence is unaffected. */

  kickpi_k7_mipi_dsi_write2(dev, 0x2f, 0x00);

  {
    static const uint8_t page0[4] = { 0xff, 0x98, 0x81, 0x00 };
    struct mipi_dsi_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.channel = KICKPI_K7_DSI_VC;
    msg.type = KICKPI_K7_PKT_GEN_LONG;
    msg.tx_buf = page0;
    msg.tx_len = sizeof(page0);
    (void)mipi_dsi_transfer(dev, &msg);
  }

  syslog(LOG_WARNING, "kickpi-k7: BIST OFF -- back to normal operation\n");
  ret = OK;
  return ret;
}
#endif /* KICKPI_K7_BIST_TEST */

#if KICKPI_K7_CAPTURE_TEST

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_capture_test
 *
 * Description:
 *   With the stream already running, change the whole screen to three
 *   different solid colours and announce each.  See KICKPI_K7_CAPTURE_TEST:
 *   the caller has already painted a fourth colour BEFORE video mode started,
 *   so the observer can say whether the very first frames made it to the glass
 *   and whether anything after them did.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_capture_test(void)
{
  static const struct
  {
    uint32_t rgb;
    FAR const char *name;
  } steps[] =
  {
    { 0x00ffff, "CYAN" },
    { 0xffff00, "YELLOW" },
    { 0x000000, "BLACK" },
  };

  int i;

  syslog(LOG_WARNING,
         "kickpi-k7: CAPTURE TEST: the frames BEFORE video mode were "
         "MAGENTA; now changing the whole screen.\n");
  syslog(LOG_WARNING,
         "kickpi-k7: READ THIS CAREFULLY: if the screen is MAGENTA (or "
         "magenta-ish) now, the panel consumed our first frames and then "
         "froze -- that is a frame-boundary fault.\n");
  syslog(LOG_WARNING,
         "kickpi-k7: if it is stripes/noise and never changes below, the "
         "panel never consumed our pixels at all.\n");

  for (i = 0; i < (int)nitems(steps); i++)
    {
      rk3576_vop_fill(steps[i].rgb);

      syslog(LOG_WARNING,
             "kickpi-k7: CAPTURE %d/%d -> whole screen %s -- LOOK NOW\n",
             i + 1, (int)nitems(steps), steps[i].name);

      up_mdelay(KICKPI_K7_CAPTURE_DWELL_MS);
    }

  syslog(LOG_WARNING,
         "kickpi-k7: CAPTURE TEST DONE -- did ANY of cyan/yellow/black "
         "appear?\n");
}

#endif /* KICKPI_K7_CAPTURE_TEST */

#if KICKPI_K7_GRAM_TEST

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_dcs_short
 *
 * Description:
 *   Send a DCS command with no parameters (a short write, data type 0x05).
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_dcs_short(FAR struct mipi_dsi_device *dev,
                                        uint8_t cmd)
{
  struct mipi_dsi_msg msg;

  memset(&msg, 0, sizeof(msg));
  msg.channel = KICKPI_K7_DSI_VC;
  msg.type = 0x05; /* DCS short write, no parameters */
  msg.tx_buf = &cmd;
  msg.tx_len = 1;

  return mipi_dsi_transfer(dev, &msg) < 0 ? -EIO : OK;
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_gram_test
 *
 * Description:
 *   Ask whether the glass is being driven by the panel's own frame memory.
 *   See KICKPI_K7_GRAM_TEST for why this is the remaining question and why it
 *   is answered cheaply.
 *
 *   Two standard DCS commands, no ambiguity:
 *     0x23 All Pixels On   -> whole panel WHITE if the output stage obeys
 *     0x22 All Pixels Off  -> whole panel BLACK
 *     0x13 Normal Display On -> back to normal
 *
 *   This is a SOURCE-DRIVER OUTPUT OVERRIDE, so it is valid whether or not
 *   the panel has frame memory: if it flips the screen white/black, the
 *   panel's output stage is responsive and simply is not being driven by our
 *   video stream.  If it does nothing, then commands cannot reach the output
 *   stage in this state either, which is a different finding again.
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_gram_test(FAR struct mipi_dsi_device *dev)
{
  int ret;

  syslog(LOG_WARNING,
         "kickpi-k7: GRAM TEST part 1: 0x23 = All Pixels ON.  If the panel "
         "is showing its OWN frame memory, the whole screen goes WHITE now.\n");

  ret = kickpi_k7_mipi_dsi_dcs_short(dev, 0x23);
  if (ret < 0)
    {
      syslog(LOG_ERR, "kickpi-k7: GRAM: 0x23 failed (%d)\n", ret);
      return ret;
    }

  up_mdelay(KICKPI_K7_GRAM_DWELL_MS);

  syslog(LOG_WARNING, "kickpi-k7: GRAM part 1: 0x22 = All Pixels OFF -> "
         "expect BLACK if the above was white\n");
  (void)kickpi_k7_mipi_dsi_dcs_short(dev, 0x22);
  up_mdelay(KICKPI_K7_GRAM_DWELL_MS);

  (void)kickpi_k7_mipi_dsi_dcs_short(dev, 0x13); /* normal display mode */
  up_mdelay(300);

  /* Part 2 removed: this panel is RAM-less (the glass is driven directly by
   * the incoming stream), so there is no frame memory to write and no stale
   * GRAM image to explain the garble.  Part 1 stays because it is valid either
   * way and costs two commands: 0x23/0x22 are source-driver output overrides,
   * so if they DO flip the screen white/black, the panel's output stage is
   * responsive to commands and is therefore not being driven by our video
   * stream.  If they do nothing at all, the output stage is not accepting
   * commands in this state either, which is a different finding again. */

  return OK;
}

#endif /* KICKPI_K7_GRAM_TEST */

#if 0 /* kept for reference -- see the note above KICKPI_K7_LANE_SWEEP */

static void kickpi_k7_mipi_dsi_lane_sweep(FAR struct mipi_dsi_host *host,
                                          FAR struct mipi_dsi_device *dev)
{
  /* Each entry: a human label, the page to select, the register, its value,
   * and whether it must be re-written as "restore".  IM_SW_EN=0 returns lane
   * control to the hardware pins, which is the state every earlier round ran
   * in, so it is both the baseline and the final state. */

  struct lane_state_s
  {
    FAR const char *label;
    uint8_t page;
    uint8_t reg;
    uint8_t value;
  };

  static const struct lane_state_s states[] =
  {
    { "baseline (IM_SW_EN=0, no lane override)", 1, 0xb6, 0x00 },
    { "page4 MIPI_LANE_SEL=0x80", 4, 0x00, 0x80 },
    { "IM_SW_EN=1 IM_SW=0", 1, 0xb6, 0x80 },
    { "IM_SW_EN=1 IM_SW=1", 1, 0xb6, 0x90 },
    { "IM_SW_EN=1 IM_SW=2", 1, 0xb6, 0xa0 },
    { "IM_SW_EN=1 IM_SW=3", 1, 0xb6, 0xb0 },
    { "IM_SW_EN=1 IM_SW=4", 1, 0xb6, 0xc0 },
    { "IM_SW_EN=1 IM_SW=5", 1, 0xb6, 0xd0 },
    { "IM_SW_EN=1 IM_SW=6", 1, 0xb6, 0xe0 },
    { "IM_SW_EN=1 IM_SW=7", 1, 0xb6, 0xf0 },
    { "restore IM_SW_EN=0", 1, 0xb6, 0x00 },
  };

  int i;

  /* First read the three registers back, unmodified: the only direct look at
   * what the module's strapping currently selects. */

  {
    struct mipi_dsi_msg msg;
    uint8_t buf[2];
    uint8_t rb = 0;

    /* Page 1, then page 4, then back to page 0 -- reads only. */

    static const uint8_t pages[3][4] =
    {
      { 0xff, 0x98, 0x81, 0x01 },
      { 0xff, 0x98, 0x81, 0x04 },
      { 0xff, 0x98, 0x81, 0x00 },
    };

    /* Page 1 B6h (lane sequence/polarity override) and page 4 R00h (lane
     * count).  The read stays on the page it selected, so the page order
     * matters only for which read lands where. */

    memset(&msg, 0, sizeof(msg));
    msg.channel = KICKPI_K7_DSI_VC;
    msg.type = KICKPI_K7_PKT_GEN_LONG;
    msg.tx_buf = pages[0];
    msg.tx_len = 4;
    (void)mipi_dsi_transfer(dev, &msg);

    buf[0] = 0xb6;
    buf[1] = 0x00;
    memset(&msg, 0, sizeof(msg));
    msg.channel = KICKPI_K7_DSI_VC;
    msg.type = KICKPI_K7_PKT_DCS_LONG;
    msg.tx_buf = buf;
    msg.tx_len = 2;
    (void)mipi_dsi_transfer(dev, &msg);

    if (mipi_dsi_dcs_read(dev, 0xb6, &rb, 1) == 1)
      {
        syslog(LOG_WARNING,
               "kickpi-k7: LANE regs: page1 B6h reads 0x%02x "
               "(IM_SW_EN=%u IM_SW=%u) before the sweep\n",
               (unsigned)rb, (unsigned)((rb >> 7) & 1u),
               (unsigned)((rb >> 4) & 0x7u));
      }
    else
      {
        syslog(LOG_WARNING,
               "kickpi-k7: LANE regs: page1 B6h not readable this way\n");
      }

    memset(&msg, 0, sizeof(msg));
    msg.channel = KICKPI_K7_DSI_VC;
    msg.type = KICKPI_K7_PKT_GEN_LONG;
    msg.tx_buf = pages[1];
    msg.tx_len = 4;
    (void)mipi_dsi_transfer(dev, &msg);

    if (mipi_dsi_dcs_read(dev, 0x00, &rb, 1) == 1)
      {
        syslog(LOG_WARNING,
               "kickpi-k7: LANE regs: page4 R00h (MIPI_LANE_SEL) reads 0x%02x "
               "before the sweep\n", (unsigned)rb);
      }

    memset(&msg, 0, sizeof(msg));
    msg.channel = KICKPI_K7_DSI_VC;
    msg.type = KICKPI_K7_PKT_GEN_LONG;
    msg.tx_buf = pages[2];
    msg.tx_len = 4;
    (void)mipi_dsi_transfer(dev, &msg);
  }

  syslog(LOG_WARNING,
         "kickpi-k7: LANE SWEEP: %d states, whole-screen RED each time.\n",
         (int)nitems(states));
  syslog(LOG_WARNING,
         "kickpi-k7: NOTE WHICH LABEL shows a CLEAN full-screen red -- the "
         "rest will show garble.\n");

  for (i = 0; i < (int)nitems(states); i++)
    {
      static const uint8_t pages[5][4] =
      {
        { 0xff, 0x98, 0x81, 0x00 },
        { 0xff, 0x98, 0x81, 0x01 },
        { 0xff, 0x98, 0x81, 0x02 },
        { 0xff, 0x98, 0x81, 0x03 },
        { 0xff, 0x98, 0x81, 0x04 },
      };

      struct mipi_dsi_msg msg;
      uint8_t buf[2];
      uint8_t rb = 0xff;

      /* Stop the stream: the ONLY state in which panel register writes are
       * reliable (a CRI transfer needs a BLLP window). */

      if (rk3576_mipi_dsi_disable_video(host) < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: LANE SWEEP: could not stop the stream -- "
                 "aborting\n");
          return;
        }

      /* Select the page using the vendor's own 4-byte form. */

      memset(&msg, 0, sizeof(msg));
      msg.channel = KICKPI_K7_DSI_VC;
      msg.type = KICKPI_K7_PKT_GEN_LONG;
      msg.tx_buf = pages[states[i].page];
      msg.tx_len = 4;
      (void)mipi_dsi_transfer(dev, &msg);

      buf[0] = states[i].reg;
      buf[1] = states[i].value;
      memset(&msg, 0, sizeof(msg));
      msg.channel = KICKPI_K7_DSI_VC;
      msg.type = KICKPI_K7_PKT_DCS_LONG;
      msg.tx_buf = buf;
      msg.tx_len = 2;
      (void)mipi_dsi_transfer(dev, &msg);

      /* Read it back so a write that silently did not land cannot be mistaken
       * for "this configuration does not work". */

      if (mipi_dsi_dcs_read(dev, states[i].reg, &rb, 1) != 1)
        {
          rb = 0xff;
        }

      /* Back to page 0 before the stream starts, so the panel is in its normal
       * page while displaying. */

      memset(&msg, 0, sizeof(msg));
      msg.channel = KICKPI_K7_DSI_VC;
      msg.type = KICKPI_K7_PKT_GEN_LONG;
      msg.tx_buf = pages[0];
      msg.tx_len = 4;
      (void)mipi_dsi_transfer(dev, &msg);

      if (rk3576_mipi_dsi_enable_video(host,
                                       &g_kickpi_k7_mipi_dsi_timing) < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: LANE SWEEP: could not restart the stream\n");
          return;
        }

      up_mdelay(200);

      rk3576_vop_fill(0xff0000);

      syslog(LOG_WARNING,
             "kickpi-k7: LANE %d/%d [%s] wrote %02x=%02x, read back %02x -- "
             "LOOK NOW: is the screen a CLEAN RED?\n",
             i + 1, (int)nitems(states), states[i].label,
             (unsigned)states[i].reg, (unsigned)states[i].value,
             (unsigned)rb);

      up_mdelay(KICKPI_K7_LANE_DWELL_MS);
    }

  syslog(LOG_WARNING,
         "kickpi-k7: LANE SWEEP DONE -- report the label that gave clean "
         "red, if any\n");
}

#endif /* 0 -- lane sweep kept for reference */

#if KICKPI_K7_CONTENT_SWEEP

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_content_sweep
 *
 * Description:
 *   For each candidate video mode: restart the stream in that mode, paint
 *   RED, then GREEN, then WHITE across the whole screen, announcing each one.
 *   See KICKPI_K7_CONTENT_SWEEP above for why the mode is the variable and a
 *   solid colour is the measurement.
 *
 *   The screen is left BLACK at the end of each mode so the transition to the
 *   next mode's first colour is visible as a change rather than as more of the
 *   same.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_content_sweep(FAR struct mipi_dsi_host *host)
{
  static const struct
  {
    uint8_t mode;
    bool continuous;
    FAR const char *tag;
  } modes[] =
  {
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, false,
      "sync-pulses+noncont" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS, false,
      "sync-events+noncont" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, true,
      "sync-pulses+cont" },
    { KICKPI_K7_DSI_VID_MODE_BURST, false, "burst+noncont" },
  };

  static const struct
  {
    uint32_t rgb;
    FAR const char *name;
  } colours[] =
  {
    { 0xff0000, "RED" },
    { 0x00ff00, "GREEN" },
    { 0xffffff, "WHITE" },
  };

  int m;
  int c;

  for (m = 0; m < (int)nitems(modes); m++)
    {
      /* Restart the stream in this mode.  The mode may only be staged in
       * Command mode (see rk3576_mipi_dsi_set_link_mode), so stop, stage,
       * start -- in that order, with the pixel clock never gated off. */

      if (rk3576_mipi_dsi_disable_video(host) < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: SWEEP: could not stop the stream -- aborting\n");
          return;
        }

      rk3576_mipi_dsi_set_link_mode(host, modes[m].mode, modes[m].continuous);

      if (rk3576_mipi_dsi_enable_video(host,
                                       &g_kickpi_k7_mipi_dsi_timing) < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: SWEEP: could not restart in %s -- aborting\n",
                 modes[m].tag);
          return;
        }

      up_mdelay(200);

      /* Verify from the readback that the mode really is live, so a silently
       * refused change cannot be mistaken for "this mode does not work". */

      {
        struct rk3576_dsi_health_s live;

        rk3576_mipi_dsi_get_health(&live);

        if ((live.vid_tx_cfg & 0x3u) != modes[m].mode)
          {
            syslog(LOG_ERR,
                   "kickpi-k7: SWEEP [%s]: NOT APPLIED (vid_mode_type=%u) -- "
                   "its result is meaningless\n",
                   modes[m].tag, (unsigned)(live.vid_tx_cfg & 0x3u));
          }
      }

      syslog(LOG_WARNING,
             "kickpi-k7: SWEEP [%s] running: watch for %d whole-screen "
             "colours\n", modes[m].tag, (int)nitems(colours));

      for (c = 0; c < (int)nitems(colours); c++)
        {
          rk3576_vop_fill(colours[c].rgb);

          syslog(LOG_WARNING,
                 "kickpi-k7: [%s] colour %d/%d -> %s -- LOOK NOW\n",
                 modes[m].tag, c + 1, (int)nitems(colours), colours[c].name);

          up_mdelay(KICKPI_K7_CONTENT_DWELL_MS);
        }

      /* Brief black, so the next mode's first colour is a visible change. */

      rk3576_vop_fill(0x000000);
      up_mdelay(300);
    }

  /* Leave the board in its configured mode, not the last one swept. */

  if (rk3576_mipi_dsi_disable_video(host) == OK)
    {
      rk3576_mipi_dsi_set_link_mode(host, KICKPI_K7_DSI_VID_MODE,
                                    KICKPI_K7_DSI_CONTINUOUS_CLK);
      if (rk3576_mipi_dsi_enable_video(host,
                                       &g_kickpi_k7_mipi_dsi_timing) < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: SWEEP: could not restore the configured mode\n");
        }
    }

  syslog(LOG_WARNING,
         "kickpi-k7: SWEEP DONE -- report: which mode tag, if any, showed a "
         "whole-screen colour?\n");
}

#endif /* KICKPI_K7_CONTENT_SWEEP */

#if KICKPI_K7_PIXEL_PROBE

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_pixel_probe
 *
 * Description:
 *   Ask whether the BYTES of our framebuffer reach the glass, without touching
 *   the stream at all.  See KICKPI_K7_PIXEL_PROBE for the reasoning; in one
 *   line: a constant byte is invariant under every byte-level fault, so eight
 *   bars of constant bytes must come out as eight clean bands if the panel is
 *   consuming our pixels -- while a solid colour need not.
 *
 *   Step 2 deliberately includes both coloured and byte-uniform fills (WHITE,
 *   GREY, BLACK), so within ONE uninterrupted observation the two kinds of
 *   content can be compared against each other.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_pixel_probe(void)
{
  static const struct
  {
    uint32_t rgb;
    FAR const char *name;
  } colours[] =
  {
    { 0xff0000, "RED" },
    { 0x00ff00, "GREEN" },
    { 0xffffff, "WHITE byte-uniform" },
    { 0x808080, "GREY byte-uniform" },
    { 0x000000, "BLACK byte-uniform" },
  };

  int i;

  syslog(LOG_WARNING, "kickpi-k7: PIXEL PROBE 1/3: BYTE-UNIFORM RAMP, "
         "8 full-height bars\n");
  syslog(LOG_WARNING, "kickpi-k7:   every byte of one bar is ONE constant "
         "value\n");
  syslog(LOG_WARNING, "kickpi-k7:   left to right: 00 24 49 6d 92 b6 db ff\n");
  syslog(LOG_WARNING, "kickpi-k7:   a constant byte survives ANY byte-level "
         "fault (bytes/px,\n");
  syslog(LOG_WARNING, "kickpi-k7:   lane swap, lane skew, byte order), so 8 "
         "clean bands MUST appear\n");
  syslog(LOG_WARNING, "kickpi-k7:   if the glass is driven by our bytes at "
         "all; only their\n");
  syslog(LOG_WARNING, "kickpi-k7:   brightness can be wrong\n");
  syslog(LOG_WARNING, "kickpi-k7:   REPORT ONE OF:\n");
  syslog(LOG_WARNING, "kickpi-k7:     (a) 8 clean bands, dark->bright ramp = "
         "pixels are ours\n");
  syslog(LOG_WARNING, "kickpi-k7:     (b) 8 clean bands but NOT a ramp = our "
         "bytes arrive, packing differs\n");
  syslog(LOG_WARNING, "kickpi-k7:     (c) noise, no bands = our bytes never "
         "reach the glass\n");

  if (rk3576_vop_paint_byte_probe() < 0)
    {
      syslog(LOG_ERR, "kickpi-k7: PIXEL PROBE: could not paint the bars\n");
      return;
    }

  up_mdelay(KICKPI_K7_PIXEL_PROBE_DWELL_MS);

  syslog(LOG_WARNING, "kickpi-k7: PIXEL PROBE 2/3: whole-screen colours on "
         "the SAME undisturbed stream\n");

  for (i = 0; i < (int)nitems(colours); i++)
    {
      rk3576_vop_fill(colours[i].rgb);

      syslog(LOG_WARNING, "kickpi-k7:   %d/%d -> %s -- LOOK NOW\n",
             i + 1, (int)nitems(colours), colours[i].name);

      up_mdelay(KICKPI_K7_PIXEL_PROBE_DWELL_MS);
    }

  rk3576_vop_paint_test_pattern(0xffffff);

  syslog(LOG_WARNING, "kickpi-k7: PIXEL PROBE 3/3: 8 colour bars "
         "(white/yellow/cyan/green/\n");
  syslog(LOG_WARNING, "kickpi-k7:   magenta/red/blue/black) with a white band "
         "on top; white and black\n");
  syslog(LOG_WARNING, "kickpi-k7:   are byte-uniform, so under a packing fault "
         "they are the only\n");
  syslog(LOG_WARNING, "kickpi-k7:   bars that should look clean\n");

  up_mdelay(KICKPI_K7_PIXEL_PROBE_DWELL_MS);

  syslog(LOG_WARNING, "kickpi-k7: PIXEL PROBE DONE -- report (a)/(b)/(c) from "
         "step 1; that decides\n");
  syslog(LOG_WARNING, "kickpi-k7:   which half of the system to work on "
         "next\n");
}

#endif /* KICKPI_K7_PIXEL_PROBE */

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_panel_probe
 *
 * Description:
 *   Stop the video stream, read the panel, restart the stream with the SAME
 *   configuration -- and nothing else.  This is reenter() without the mode
 *   change, and it exists to answer a question the log has raised but cannot
 *   currently attribute: WHERE do the panel's bad packets come from?
 *
 *   The first run that ever reported a non-zero RDNUMED showed 152 bad packets
 *   accumulated across the boot phase (3 s of streaming, which includes the
 *   deliberate mid-stream VOP-off gate) plus the 8.4 s content strobe, while
 *   every 1 s observation inside the mode matrix reported 0.  That pattern
 *   fits "the errors are created by an EVENT" (the first stream entry, or the
 *   clock gating) rather than by link quality -- but it is also compatible
 *   with "the stream needs seconds before it corrupts anything".  Reading the
 *   counter after an UNDISTURBED interval of known length separates them, and
 *   which one it is matters enormously:
 *
 *     errors only across start/gate events -> the transmit path is fine; the
 *       fault is elsewhere (content), and the error counter is not evidence
 *       about the picture at all;
 *     errors accumulating during a quiet stream -> the LINK itself is
 *       marginal (rate, drive strength, timing), which is a completely
 *       different investigation and would also explain a panel that receives
 *       frames it cannot use.
 *
 *   Caveat, stated because it limits the attribution: this call's own stop and
 *   restart may inject errors that the NEXT read will report.  Reading twice
 *   in a row with a known quiet interval in between lets that be subtracted.
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_panel_probe(FAR struct mipi_dsi_host *host,
                                          FAR struct mipi_dsi_device *dev,
                                          FAR const char *tag)
{
  int ret;

  ret = rk3576_mipi_dsi_disable_video(host);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: probe[%s]: could not stop the stream (%d) -- no "
             "panel read this time\n", tag, ret);
      return ret;
    }

  kickpi_k7_mipi_dsi_panel_status(dev, tag);

  ret = rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "kickpi-k7: probe[%s]: restarting the stream failed: %d\n",
             tag, ret);
      return ret;
    }

  up_mdelay(100);
  return OK;
}

#if KICKPI_K7_DSI_LINK_MATRIX

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_link_matrix
 *
 * Description:
 *   Work through the (video mode x clock lane type) combinations at boot,
 *   giving EACH one its own solid screen colour, its own pipeline fingerprint
 *   and its own panel error tally.
 *
 * *** THIS MUST BE TURNED OFF ONCE THE PANEL WORKS. ***
 *   A probe that re-programs a live, working panel is exactly what made the
 *   display "light up once, then never again" in an earlier round: it wrote
 *   deliberately-broken configurations into a panel that was already
 *   displaying.  Set KICKPI_K7_DSI_LINK_MATRIX to 0 and keep only the
 *   single "boot" observation.
 *
 *   Why this is worth doing again, when a sweep was already done long ago:
 *   every earlier sweep ran while the pixel datapath was NOT working (the IPI
 *   input FIFO saturated at 768 words and no line packet was ever built), so
 *   none of them measured the mode's effect on a functioning stream -- they
 *   measured the effect of the mode on a broken one.  The sending path is now
 *   verifiably healthy (lane duty at the expected burst fraction, both FIFOs
 *   cycling, the two-sided VOP-off experiment passing), which makes this the
 *   first time the comparison is meaningful.
 *
 *   Two design choices keep it a clean experiment:
 *
 *   1. THE PIXEL CLOCK NEVER STOPS.  Each entry is applied through
 *      disable_video()/enable_video() with the VOP still scanning.  Entering
 *      video mode before the pixel clock exists is what wedged the pipeline
 *      for twenty-odd rounds (see kickpi_k7_mipi_dsi_initialize()); a sweep
 *      that repeats that mistake would "measure" the ordering bug instead of
 *      the mode.
 *
 *   2. THE LANE RATE IS HELD CONSTANT.  hs_rate is baked into the DCPHY PLL
 *      at PHY power-on, so switching it at runtime would desynchronise the
 *      PHY from the controller's hstx-cycle registers.  Every entry therefore
 *      runs at the burst-headroom rate; a non-burst stream simply gets extra
 *      link margin, which the timing absorbs.
 *
 *   The visual anchor matters because a dark panel is the whole symptom: if
 *   the panel lights up, the colour says WHICH configuration did it.
 *
 ****************************************************************************/

static void
kickpi_k7_mipi_dsi_link_matrix(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *dev)
{
  struct kickpi_k7_matrix_entry_s
  {
    uint8_t mode;
    bool continuous;
    uint32_t marker;
    FAR const char *tag;
    FAR const char *colour;
  };

  /* Ordered by prior probability, best first: the panel datasheet documents a
   * clock lane that returns to LP-11 after every burst (THS-EXIT), which a
   * continuous clock never gives it, so non-continuous variants come first.
   * The current default (burst + continuous) leads anyway, so that entry
   * doubles as a check that the healthy state survives a rearm.
   *
   * Every entry now paints the SAME eight colour bars and differs only in the
   * marker band across the top, so the bars themselves become the measurement:
   * they are read out against the panel's own scan, and the way they come out
   * names the fault (clean / permuted / rolled / noise) instead of merely
   * saying that some pixels arrived.  The marker colours are deliberately not
   * among the bar colours, so a marker can never be confused with a bar. */

  static const struct kickpi_k7_matrix_entry_s combos[] =
  {
    { KICKPI_K7_DSI_VID_MODE_BURST, true,  0xff8000, "burst+cont",
      "orange" },
    { KICKPI_K7_DSI_VID_MODE_BURST, false, 0x8000ff, "burst+noncont",
      "violet" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS, false, 0x804000,
      "sync-events+noncont", "brown" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, false, 0x008080,
      "sync-pulses+noncont", "teal" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS, true, 0xffc0cb,
      "sync-events+cont", "pink" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, true, 0x404040,
      "sync-pulses+cont", "dark grey" },
  };

  FAR const char *pending = "pre-matrix";
  int i;

  for (i = 0; i < (int)nitems(combos); i++)
    {
      /* One call does the whole sequence for one entry: stop the stream, read
       * the panel for the period that just ended (tagged with ITS name),
       * stage the new mode, paint the identifying colour, restart, and verify
       * the change from the readback.  The ORDER inside that call is the whole
       * experiment -- see its description. */

      if (kickpi_k7_mipi_dsi_reenter(host, dev, pending, combos[i].mode,
                                     combos[i].continuous,
                                     combos[i].marker) < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: matrix[%d/%d] %s: could not restart the stream "
                 "-- stopping the matrix\n",
                 i + 1, (int)nitems(combos), combos[i].tag);
          return;
        }

      pending = combos[i].tag;

      kickpi_k7_mipi_dsi_observe(combos[i].tag, KICKPI_K7_MATRIX_SAMPLES,
                                 false);
    }

  /* Collect the tally for the last entry, then put the board's configured
   * default back so the state the system runs in is the one the source
   * describes. */

  kickpi_k7_mipi_dsi_reenter(host, dev, pending, KICKPI_K7_DSI_VID_MODE,
                             KICKPI_K7_DSI_CONTINUOUS_CLK, 0x808080);

  syslog(LOG_WARNING,
         "kickpi-k7: matrix: done -- the last image painted is the eight bars "
         "with a mid-grey band.  If any entry showed a stable, readable image, "
         "its marker colour names the configuration that works\n");
}

#endif /* KICKPI_K7_DSI_LINK_MATRIX */

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_health_timeline
 *
 * Description:
 *   Poll the live video path for a few seconds after the stream starts and
 *   log a compact line each time, plus a warning the moment a previously
 *   live stream stops.
 *
 *   Why this exists: a panel that lights up and then fades away has STOPPED
 *   BEING REFRESHED (an LCD decays, fastest in the corners), so the real
 *   fault is a dead pixel stream, not a bad image.  One-shot bring-up dumps
 *   can only report "it is stopped now"; this reports WHEN it stopped and
 *   what the controller latched at that instant (INT_ST_TO), which is the
 *   difference between a theory and a diagnosis.
 *
 *   Liveness needs BOTH halves: the VOP scan counter must be advancing (the
 *   timing generator runs) and the DSI must be in video mode with the IPI
 *   being fed.  A frozen scan counter means the VOP stopped; DSI mode
 *   dropping out of video means the host was reset/lost.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_panel_status(FAR struct mipi_dsi_device *dev,
                                           FAR const char *tag)
{
  uint8_t sig = 0xff;
  uint8_t diag = 0xff;
  uint8_t pwr = 0xff;
  uint8_t colmod = 0xff;
  uint8_t img = 0xff;
  uint8_t err = 0xff;
  uint8_t tmp = 0;

  /* Each value stays 0xff when its read failed, so a missing answer is
   * distinguishable from a genuine 0x00. */

  if (mipi_dsi_dcs_read(dev, KICKPI_K7_DCS_RDDSM, &tmp, 1) == 1)
    {
      sig = tmp;
    }

  if (mipi_dsi_dcs_read(dev, KICKPI_K7_DCS_RDDSDR, &tmp, 1) == 1)
    {
      diag = tmp;
    }

  if (mipi_dsi_dcs_read(dev, KICKPI_K7_DCS_RDDPM, &tmp, 1) == 1)
    {
      pwr = tmp;
    }

  if (mipi_dsi_dcs_read(dev, KICKPI_K7_DCS_RDDCOLMOD, &tmp, 1) == 1)
    {
      colmod = tmp;
    }

  if (mipi_dsi_dcs_read(dev, KICKPI_K7_DCS_RDIM, &tmp, 1) == 1)
    {
      img = tmp;
    }

  /* Read RDNUMED LAST: reading it clears the error counter (and the "last
   * packet had an error" flag in 0Eh), so everything above must be sampled
   * before this one or the sequence would erase its own evidence. */

  if (mipi_dsi_dcs_read(dev, KICKPI_K7_DCS_RDNUMED, &tmp, 1) == 1)
    {
      err = tmp;
    }

  syslog(LOG_INFO,
         "kickpi-k7: panel[%s] 0e=%02x 0f=%02x 0a=%02x 0c=%02x 0d=%02x "
         "05=%02x\n",
         tag, (unsigned)sig, (unsigned)diag, (unsigned)pwr, (unsigned)colmod,
         (unsigned)img, (unsigned)err);
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_cmd_fsm_report
 *
 * Description:
 *   Print the result of the "is the phy_tx_ready FSM observable at all?"
 *   control experiment (see rk3576_mipi_dsi_get_cmd_probe()).
 *
 *   Why this is worth a dedicated experiment: the driver samples
 *   DSI2_OBS_FSM_SEL_PHY_TX_READY in video mode and always reads 0, and that
 *   reading has been treated as "the PPI readiness handshake never starts".
 *   But the same controller's Command-mode DCS path demonstrably works (the
 *   panel latches its init sequence and answers bus-turnaround reads), so the
 *   FSM is given every opportunity to move during that ~200-command burst.
 *   If it still never leaves INIT, the field simply does not track the DCS
 *   transmit path, and every earlier verdict built on "phy_tx_ready stuck at
 *   INIT" has to be discarded instead of chased further.
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_cmd_fsm_report(void)
{
  struct rk3576_dsi_cmd_probe_s probe;

  rk3576_mipi_dsi_get_cmd_probe(&probe);

  syslog(LOG_INFO,
         "kickpi-k7: cmd-fsm probe: %u CRI commands, phy_tx_ready state "
         "max=%u prev_max=%u noninit=%u prev_noninit=%u\n",
         (unsigned)probe.cmds, (unsigned)probe.txr_max,
         (unsigned)probe.txr_prev_max, (unsigned)probe.txr_noninit,
         (unsigned)probe.txr_prev_noninit);

  if (probe.cmds == 0)
    {
      return;
    }

  if (!probe.txr_noninit && !probe.txr_prev_noninit)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: cmd-fsm verdict: stayed INIT over %u HS cmds the "
             "panel received\n",
             (unsigned)probe.cmds);
      syslog(LOG_WARNING,
             "kickpi-k7: cmd-fsm => the field does NOT track the command "
             "path; not usable as video-path evidence\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "kickpi-k7: cmd-fsm verdict: the FSM DOES move during command "
             "traffic (max=%u prev=%u) -> the field is live, so 0 in video "
             "mode is a real PPI stall\n",
             (unsigned)probe.txr_max, (unsigned)probe.txr_prev_max);
    }
}

#if KICKPI_K7_DSI_LINK_MATRIX

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_reenter
 *
 * Description:
 *   Stop the video stream, ask the PANEL what it received while the stream was
 *   running, apply a new link configuration and screen colour, restart the
 *   stream, and VERIFY from the readback that the new configuration is live.
 *
 *   Three jobs in one call, and all three are needed:
 *
 *   1. THE PANEL VIEW OF THE PERIOD THAT JUST ENDED.  Everything else in the
 *      log describes what the host believes it sent; RDNUMED (05h) is the
 *      panel's own count of packets that arrived with a bad ECC/CRC, it
 *      ACCUMULATES for as long as the panel is powered, and reading it CLEARS
 *      it -- so one read per period gives that period its own error tally:
 *
 *        err(05) != 0 -> packets DO arrive, but damaged.  The fault is in the
 *                        physical link or lane timing, not the panel.
 *        err(05) == 0 -> no damaged packet was received.  AMBIGUOUS by itself
 *                        (a link that sends nothing also counts zero), so it
 *                        must be read together with the lane duty and the two
 *                        FIFOs from the observation of the same period.
 *        pwr no longer has bit7 (DISPON) -> the panel dropped display-on
 *                        during that period: look at power/reset sequencing,
 *                        and at whether the stream itself is resetting it.
 *
 *   2. A CONFIGURATION CHANGE, WITH THE ORDER THAT MAKES IT REAL.  Staging a
 *      mode change is REFUSED while the host is in Video mode (describing one
 *      mode while transmitting another is worse than not changing it), so the
 *      change has to happen inside the window this function opens: stop, then
 *      stage, then restart.  An earlier version of the caller staged the
 *      change BEFORE the stop, every entry was silently rejected, and six
 *      "different" configurations all ran the same mode while looking like six
 *      data points.  Do not reorder these steps, and do not remove the
 *      readback check below -- together they make that failure impossible to
 *      mistake for a real result.
 *
 *   3. A SCREEN COLOUR per configuration, so a human observer can identify an
 *      entry ("it showed green") without correlating console timing.
 *
 ****************************************************************************/

static int
kickpi_k7_mipi_dsi_reenter(FAR struct mipi_dsi_host *host,
                           FAR struct mipi_dsi_device *dev,
                           FAR const char *tag, uint8_t mode, bool continuous,
                           uint32_t marker)
{
  struct rk3576_dsi_health_s before;
  struct rk3576_dsi_health_s applied;
  uint32_t want_clk = continuous ? 0u : 1u;
  int ret;

  rk3576_mipi_dsi_get_health(&before);

  ret = rk3576_mipi_dsi_disable_video(host);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: reenter[%s]: could not leave video mode (%d) -- "
             "skipping the panel read (the transmitter is wedged)\n", tag, ret);
      return ret;
    }

  /* The host is in Command mode now: the panel is reachable again.  This read
   * covers exactly the period that just ended, because reading RDNUMED clears
   * it -- so each observation window gets its own error tally. */

  kickpi_k7_mipi_dsi_panel_status(dev, tag);

  /* Command mode is also the ONLY state in which the link configuration may
   * be staged: changing it in Video mode is refused (telling the controller
   * one mode while it transmits another is worse than not changing it).
   * Nothing in the rest of this file may reorder these steps -- an earlier
   * version staged the change BEFORE the stop, every entry was silently
   * rejected, and six "different" configurations all ran the same mode while
   * looking like six data points. */

  rk3576_mipi_dsi_set_link_mode(host, mode, continuous);

  /* Paint the measurement image: eight colour bars with a band across the top
   * in the colour that identifies this configuration.  A human observer can
   * therefore name both the state of the picture AND which entry produced it
   * from the screen alone -- "readable bars, violet band" identifies
   * burst+noncont without anyone having to correlate console timing. */

  if (rk3576_vop_paint_test_pattern(marker) < 0)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: reenter[%s]: could not paint the test pattern -- the "
             "colour anchor is unavailable, watch the console instead\n",
             tag);
    }

  syslog(LOG_WARNING,
         "kickpi-k7: reenter[%s]: watch the screen -- the top band should be "
         "0x%06x and below it eight vertical bars, white / yellow / cyan / "
         "green / magenta / red / blue / black from left to right\n",
         tag, (unsigned)marker);

  ret = rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "kickpi-k7: reenter[%s]: re-enabling video failed: %d -- the "
             "panel will stay dark for the rest of the boot\n", tag, ret);
      return ret;
    }

  up_mdelay(100);
  rk3576_mipi_dsi_get_health(&applied);

  /* Verify against the READBACK that the requested configuration is the one
   * actually live.  Without this check a refused or ignored change looks
   * identical to a configuration that simply does not drive the panel -- i.e.
   * a fake data point, which is exactly what happened once already. */

  if ((applied.vid_tx_cfg & 0x3u) != mode ||
      (applied.phy_clk_cfg & 0x1u) != want_clk)
    {
      syslog(LOG_ERR,
             "kickpi-k7: reenter[%s]: NOT APPLIED -- readback says "
             "vid_mode_type=%u clk_type=%u but %u/%u was requested.  The "
             "observation that follows describes the OLD configuration; fix "
             "this before trusting its verdict\n",
             tag, (unsigned)(applied.vid_tx_cfg & 0x3u),
             (unsigned)(applied.phy_clk_cfg & 0x1u), (unsigned)mode,
             (unsigned)want_clk);
    }
  else
    {
      syslog(LOG_WARNING,
             "kickpi-k7: reenter[%s]: link now vid_mode_type=%u clk_type=%u "
             "(applied as requested); lanes out-of-LP-11 %u/16 -> %u/16, "
             "phy_txhs peak %u -> %u -- %s\n",
             tag, (unsigned)(applied.vid_tx_cfg & 0x3u),
             (unsigned)(applied.phy_clk_cfg & 0x1u),
             (unsigned)before.data_hs_hits, (unsigned)applied.data_hs_hits,
             (unsigned)before.phy_txhs_max, (unsigned)applied.phy_txhs_max,
             (applied.mode_status == 3 && applied.data_hs_hits > 0)
                 ? "the stream came back"
                 : "the stream did NOT come back");
    }

  return OK;
}

#endif /* KICKPI_K7_DSI_LINK_MATRIX */

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_observe
 *
 * Description:
 *   Poll the live video path for `samples` windows and log a compact line per
 *   sample, a warning the moment a previously live stream stops, and finally
 *   a per-state fingerprint that can be compared against the other
 *   observations in the same boot.
 *
 *   Why polling instead of one dump: a panel that lights up and then fades
 *   away has STOPPED BEING REFRESHED (an LCD decays, fastest in the corners),
 *   so the real fault is a dead pixel stream, not a bad image.  One-shot
 *   dumps can only report "it is stopped now"; this reports WHEN it stopped
 *   and what the controller latched at that instant (INT_ST_TO), which is the
 *   difference between a theory and a diagnosis.
 *
 *   Liveness needs BOTH halves: the VOP scan counter must be advancing (the
 *   timing generator runs) and the DSI must be in video mode with the IPI
 *   being fed.  A frozen scan counter means the VOP stopped; DSI mode
 *   dropping out of video means the host was reset/lost.
 *
 *   The panel is NOT polled from inside this loop.  A CRI transfer needs a
 *   BLLP window once the host is in video mode; when the video transmitter is
 *   wedged there is no BLLP, so every read times out AND the attempt itself
 *   perturbs the datapath.  That is exactly what the previous round produced:
 *   the first panel read succeeded, after which every sample reported
 *   "wait cri idle timeout" and every per-sample error value was 0xff (read
 *   failure) -- the experiment yielded no data.  The panel is therefore read
 *   in Command mode only: once at bring-up
 *   (kickpi_k7_mipi_dsi_panel_bringup()) and once per observation window, by
 *   the caller's own stop/read/restart sequence
 *   (kickpi_k7_mipi_dsi_reenter()).
 *
 * Input Parameters:
 *   tag         - Observation name, used as the log tag so states from the
 *                 same boot can be matched up.
 *   samples     - Number of 250 ms windows to poll.
 *   with_vop_off- Also run the two-sided "gate the pixel feed" experiment
 *                 (only worth it once, on the first observation).
 *
 ****************************************************************************/

static void kickpi_k7_mipi_dsi_observe(FAR const char *tag, int samples,
                                       bool with_vop_off)
{
  struct rk3576_dsi_health_s health;
  uint32_t vcnt_before;
  uint32_t vcnt_after;
  uint32_t vcnt_peak = 0;
  uint32_t ipi_data_peak = 0;
  uint32_t ipi_data_hot_windows = 0;
  uint32_t ipi_data_nonempty_hits = 0;
  uint32_t phy_txhs_nonempty_hits = 0;
  uint32_t ipi_event_peak = 0;
  uint32_t phy_txhs_peak = 0;
  uint32_t txhs_full_hits = 0;
  uint32_t data_hs_hits = 0;
  uint32_t data_hs_min_window = 0xffffffffu;
  uint32_t clk_hs_hits = 0;
  uint32_t duty_hits = 0;
  uint32_t duty_total = 0;
  uint32_t to_seen = 0;
  uint32_t cri_seen = 0;
  uint32_t fne_seen = 0;
  bool ipi_seen = false;
  bool ipi_event_ever = false;
  bool phy_txhs_ever = false;
  bool alive = true;
  struct rk3576_vop_int_s vop_int;
  uint32_t vop_int_seen = 0;
  uint32_t vop_bus_err_seen = 0;
  uint32_t vop_flags = 0;  /* bit0 = AXI read error, bit1 = POST underrun */
  int i;

  if (samples <= 0)
    {
      return;
    }

  vcnt_before = rk3576_vop_get_scan_counter();

  for (i = 0; i < samples; i++)
    {
      uint32_t lines;
      bool now;

      /* Sample the scan counter over a SHORT window: it is 13 bits and wraps
       * every ~8192 lines, so a long interval would make the delta garbage.
       * Only "did it move at all" is meaningful -- the delay is rounded up to
       * a scheduler tick, so the delta is not a reliable line rate. */

      up_mdelay(2);
      vcnt_after = rk3576_vop_get_scan_counter();
      lines = (vcnt_after - vcnt_before) & 0x1fffu;
      vcnt_before = vcnt_after;

      up_mdelay(KICKPI_K7_HEALTH_PERIOD_MS - 2);

      if (rk3576_mipi_dsi_get_health(&health) < 0)
        {
          break;
        }

      /* The controller's own timeouts are read-clear, so accumulate them.
       * err_to_hstxrdy = "I asked the PHY to transmit in HS and it never
       * became ready" (PPI handshake); err_to_hstx = "an HS transmission
       * started and never finished" (the packet was starved of data).  These
       * discriminate the two ways the video packet can wedge. */

      to_seen |= health.int_st_to;
      cri_seen += (health.core_status >> 16) & 1u;

      /* The VOP's own error/event latches.  Read (and clear) once per window
       * so an event is attributed to the period it happened in.
       *
       * This is the missing positive evidence for "the stream is being sent
       * but carries no picture": a window whose AXI reads fail, or a POST
       * buffer that underruns, produces exactly that -- valid packets with
       * nothing in them -- while the DSI side, the lane activity and the
       * panel's CRC/ECC counter all stay clean. */

      if (rk3576_vop_get_int_status(&vop_int, true) == OK)
        {
          vop_int_seen |= vop_int.vp_raw;
          vop_bus_err_seen |= vop_int.sys0;

          /* Print ONLY when the VOP reports a REAL fault.
           *
           * The frame-start / line-flag / window-empty / POST-buffer-empty
           * bits are per-line EVENTS latched in the same register, so they are
           * set on every line of a healthy stream.  Printing them per window
           * floods the console with values that look alarming and mean
           * nothing -- and the console is the instrument here, so a line long
           * enough to be dropped by the UART is worse than no line at all.
           * Only the AXI/bus error bits (SYS0/SYS1) are faults. */

          if (vop_int.bus_error || vop_int.sys0 != 0 || vop_int.sys1 != 0)
            {
              syslog(LOG_WARNING,
                     "kickpi-k7: health[%s:%d] VOP FAULT vp=%08x sys0=%08x "
                     "sys1=%08x\n",
                     tag, i, (unsigned)vop_int.vp_raw, vop_int.sys0,
                     vop_int.sys1);
            }

          /* Accumulate regardless: the verdict must reflect anything that
           * happened in ANY window. */

          vop_flags |= (vop_int.bus_error ? 1u : 0u) |
                       (vop_int.post_buf_empty ? 2u : 0u);
        }

      now = (lines > 0) && health.video_mode;

      if (lines > vcnt_peak)
        {
          vcnt_peak = lines;
        }

      if (health.ipi_data_max > ipi_data_peak)
        {
          ipi_data_peak = health.ipi_data_max;
        }

      /* A single window can catch the FIFO while the packet builder happens to
       * be draining it, so count the windows in which it was clearly backed
       * up.  "Backed up in every window" is what makes the difference between
       * "the payload is flowing" and "the pixels are piling up" statement-
       * strength instead of anecdotal. */

      if (health.ipi_data_max >= KICKPI_K7_IPI_DATA_BACKED_UP)
        {
          ipi_data_hot_windows++;
        }

      if (health.phy_txhs_max > phy_txhs_peak)
        {
          phy_txhs_peak = health.phy_txhs_max;
        }

      txhs_full_hits += health.phy_txhs_full_hits;
      data_hs_hits += health.data_hs_hits;
      clk_hs_hits += health.clk_hs_hits;

      /* The fine-grained duty of the same window.  Its whole point is that
       * its sampling spread over the line (see RK3576_DSI_DUTY_SAMPLES), so
       * unlike data_hs_hits it can be compared against a predicted fraction
       * without worrying about phase. */

      duty_hits += health.duty_hits;
      duty_total += health.duty_total;

      /* Occupancy fractions, reported as a percentage of all samples.  These
       * separate "the pipeline is flowing" from "a FIFO is backed up" much
       * more directly than the peaks do: a peak at the FIFO depth is normal on
       * a healthy burst (the IPI deliberately runs ahead of the link), whereas
       * a HIGH non-empty fraction on the INPUT side means the source is
       * outrunning the packet builder. */

      ipi_data_nonempty_hits += health.ipi_data_nonempty_hits;
      phy_txhs_nonempty_hits += health.phy_txhs_nonempty_hits;

      /* The per-window MINIMUM matters as much as the sum: the sampling
       * window (16 samples ~36 us apart) is nearly commensurate with the
       * 13.1 us line period, so a mean near the maximum can still hide lines
       * on which nothing left LP-11 at all.  A window that never sees the
       * lanes leave LP-11 is a line with no transmission. */

      if (health.data_hs_hits < data_hs_min_window)
        {
          data_hs_min_window = health.data_hs_hits;
        }

      if (health.ipi_fifos_not_empty)
        {
          fne_seen++;
        }

      if (health.ipi_event_max > ipi_event_peak)
        {
          ipi_event_peak = health.ipi_event_max;
        }

      if (health.ipi_event_ever)
        {
          ipi_event_ever = true;
        }

      if (health.phy_txhs_ever)
        {
          phy_txhs_ever = true;
        }

      if (health.ipi_busy)
        {
          ipi_seen = true;
        }

      /* Kept short on purpose: these lines go out every 250 ms and a long
       * line is what gets mangled on a shared serial console. */

      syslog(LOG_INFO,
             "kickpi-k7: health[%s:%d] scan=%u ipi=%u fne=%u data=%u evt=%u "
             "txhs=%u lane=%u clk=%u cri=%u to=%04x mode=%u\n",
             tag, i, (unsigned)lines, (unsigned)health.ipi_busy,
             (unsigned)health.ipi_fifos_not_empty,
             (unsigned)health.ipi_data_max, (unsigned)health.ipi_event_max,
             (unsigned)health.phy_txhs_max,
             (unsigned)health.data_hs_hits, (unsigned)health.clk_hs_hits,
             (unsigned)((health.core_status >> 16) & 1u),
             (unsigned)(health.int_st_to & 0xffffu),
             (unsigned)health.mode_status);

      /* One compact line per FSM: current state, '*' when its cycle counter is
       * pinned at 0xffff and '!' when the hardware's own stuck flag is set.
       * The chain order is ipi_vid -> sys_main -> sys_pkt_build -> phy_tx_ready,
       * so the first entry that has stopped progressing names the dead stage. */

      syslog(LOG_INFO,
             "kickpi-k7: health[%s:%d] fsm iv=%02x%s%s main=%02x%s%s "
             "pkt=%02x%s%s txr=%02x%s%s raw iv=%08x txr=%08x\n",
             tag, i,
             (unsigned)health.fsm_state[0],
             health.fsm_saturated[0] ? "*" : "",
             health.fsm_stuck[0] ? "!" : "",
             (unsigned)health.fsm_state[1],
             health.fsm_saturated[1] ? "*" : "",
             health.fsm_stuck[1] ? "!" : "",
             (unsigned)health.fsm_state[2],
             health.fsm_saturated[2] ? "*" : "",
             health.fsm_stuck[2] ? "!" : "",
             (unsigned)health.fsm_state[3],
             health.fsm_saturated[3] ? "*" : "",
             health.fsm_stuck[3] ? "!" : "",
             health.fsm_raw[0], health.fsm_raw[3]);

      if (alive && !now)
        {
          syslog(LOG_WARNING,
                 "kickpi-k7: STREAM STOPPED at %s:%d scan=%u mode=%u "
                 "TO=%08x hstx=%u hstxrdy=%u\n",
                 tag, i, (unsigned)lines, (unsigned)health.mode_status,
                 health.int_st_to, (unsigned)(health.int_st_to & 1u),
                 (unsigned)((health.int_st_to >> 1) & 1u));
        }

      alive = now;
    }

  /* --- Two-sided discriminator: is the wedge owned by the VOP or the DSI?
   *
   * The lanes sit OUT of LP-11 continuously and the inter-packet low-power
   * window never appears -- which is also why the CRI command channel is
   * jammed.  The cheapest way to decide who is holding the transmitter is to
   * stop feeding it: gate the video port's output off while leaving the DSI in
   * video mode, then re-read the DSI.  Cutting the pixels cannot make a wedged
   * PHY recover, but it separates the two cases:
   *
   *   lanes return to LP-11 while the VOP output is gated
   *       -> the DSI was waiting on the pixel stream: the fault is the
   *          VOP -> IPI pixel handoff, not the PHY.
   *   lanes stay out of LP-11 with the VOP gated
   *       -> the DSI/PHY is wedged on its own, independent of the pixel feed.
   */

  if (with_vop_off)
  {
    struct rk3576_dsi_health_s before;
    struct rk3576_dsi_health_s gated;
    uint32_t gated_ipi_data;
    uint32_t gated_txhs;
    uint32_t dl[KICKPI_K7_GATE_LADDER_SAMPLES];
    uint32_t tl[KICKPI_K7_GATE_LADDER_SAMPLES];
    FAR const char *verdict;
    int k;

    rk3576_mipi_dsi_get_health(&before);

    if (rk3576_vop_set_output_enable(false) == OK)
      {
        /* Sample the two FIFO levels as a LADDER while the feed is cut.  The
         * tail of the ladder is the measurement: a healthy packet builder
         * empties the input FIFO once no more pixels arrive, and a blocked one
         * leaves its contents exactly where they were. */

        up_mdelay(20);

        for (k = 0; k < KICKPI_K7_GATE_LADDER_SAMPLES; k++)
          {
            uint32_t st;

            st = rk3576_mipi_dsi_obs_fifo(RK3576_DSI_OBS_FIFO_IPI_DATA);
            dl[k] = (st & RK3576_DSI_OBS_FIFO_WORD_CNT_MASK) >>
                    RK3576_DSI_OBS_FIFO_WORD_CNT_SHIFT;

            st = rk3576_mipi_dsi_obs_fifo(RK3576_DSI_OBS_FIFO_PHY_TXHS);
            tl[k] = (st & RK3576_DSI_OBS_FIFO_WORD_CNT_MASK) >>
                    RK3576_DSI_OBS_FIFO_WORD_CNT_SHIFT;

            up_mdelay(KICKPI_K7_GATE_LADDER_STEP_MS);
          }

        gated_ipi_data = dl[KICKPI_K7_GATE_LADDER_SAMPLES - 1];
        gated_txhs = tl[KICKPI_K7_GATE_LADDER_SAMPLES - 1];

        syslog(LOG_WARNING,
               "kickpi-k7: vop-off[%s] ipi_data %u %u %u %u %u %u\n",
               tag, (unsigned)dl[0], (unsigned)dl[1], (unsigned)dl[2],
               (unsigned)dl[3], (unsigned)dl[4], (unsigned)dl[5]);

        syslog(LOG_WARNING,
               "kickpi-k7: vop-off[%s] txhs %u %u %u %u %u %u\n",
               tag, (unsigned)tl[0], (unsigned)tl[1], (unsigned)tl[2],
               (unsigned)tl[3], (unsigned)tl[4], (unsigned)tl[5]);

        rk3576_mipi_dsi_get_health(&gated);
        rk3576_vop_set_output_enable(true);

        to_seen |= gated.int_st_to;

        syslog(LOG_INFO,
               "kickpi-k7: vop-off[%s] ipi_busy %u->%u fne %u->%u data_hs "
               "%u->%u/16 clk %u->%u\n",
               tag,
               (unsigned)before.ipi_busy, (unsigned)gated.ipi_busy,
               (unsigned)before.ipi_fifos_not_empty,
               (unsigned)gated.ipi_fifos_not_empty,
               (unsigned)before.data_hs_hits, (unsigned)gated.data_hs_hits,
               (unsigned)before.clk_hs_hits, (unsigned)gated.clk_hs_hits);

        syslog(LOG_INFO,
               "kickpi-k7: vop-off[%s] txhs %u->%u ipi_data %u->%u "
               "core_status=%08x->%08x TO=%08x\n",
               tag, (unsigned)before.phy_txhs_max, (unsigned)gated.phy_txhs_max,
               (unsigned)before.ipi_data_max, (unsigned)gated.ipi_data_max,
               before.core_status, gated.core_status, gated.int_st_to);

        /* ★ THE GATE IS CONFOUNDED, AND THE VERDICT BELOW SAYS SO.
         *
         * rk3576_vop_set_output_enable() clears MIPI0_INFACE_CTRL.{out_en,
         * clk_out_en}, and clk_out_en IS the IPI clock.  Cutting the feed
         * therefore also stops the IPI block that READS ipi_data, so a residual
         * word count freezes whether or not a packet builder would have
         * consumed it: a healthy pipeline and a blocked one give the same
         * ladder here.  An earlier round read this experiment as "the IPI is
         * genuinely packaging pixels" and was wrong.
         *
         * What it still proves, in the POSITIVE direction only: a FIFO that
         * EMPTIES while the feed is cut had a live consumer, so "the ladder
         * decays to zero" stays conclusive while "the ladder stays put" does
         * not.  The unconfounded measurement is the LIVE ladder in
         * kickpi_k7_mipi_dsi_payload_probe(), which touches nothing. */

        if (before.data_hs_hits == 0)
          {
            verdict = "NON-DISCRIMINATING -- the lanes were already in LP-11 "
                      "before the pixel feed was gated, so this test cannot "
                      "say whether the DSI was waiting on the VOP";
          }
        else if (gated.data_hs_hits != 0)
          {
            verdict = "lanes STAYED out of LP-11 while gated -> the DSI/PHY "
                      "is wedged on its own, independent of the pixel feed";
          }
        else if (gated_ipi_data == 0)
          {
            verdict = gated_txhs == 0
                          ? "CONSUMER ALIVE: the input FIFO emptied while the "
                            "feed was cut and the PHY FIFO is empty too -> "
                            "something WAS reading the pixels, so the IPI -> "
                            "SYS handoff is not the fault"
                          : "CONSUMER ALIVE, but the PHY FIFO still holds "
                            "words after the feed was cut -> the PHY may have "
                            "abandoned payload it had already been given";
          }
        else
          {
            verdict = "INCONCLUSIVE BY CONSTRUCTION: the input FIFO still "
                      "holds words, but cutting the feed also clears "
                      "clk_out_en, which IS the IPI clock, so this test stopped "
                      "the consumer itself.  The LIVE ladder in the payload "
                      "probe is what answers this question";
          }

        syslog(LOG_WARNING, "kickpi-k7: VOP-off test[%s] verdict: %s\n",
               tag, verdict);
      }
  }

  /* --- Per-state fingerprint.
   *
   * One short pair of lines per observation, deliberately structured so the
   * SAME boot's states can be compared against each other.  That matters
   * because the DSI's behaviour turned out to depend on WHEN it entered video
   * mode with byte-identical registers (see the bring-up sequence in
   * kickpi_k7_mipi_dsi_initialize()), and cross-boot comparisons have
   * repeatedly misled this project: the fault has been double-stable, so two
   * different boots can differ for reasons that have nothing to do with the
   * change being tested.
   *
   * The expected data-lane duty is derived from the board's own constants, so
   * the line states by itself whether the transmitter is sending payload:
   *
   *   duty = (hactive * bpp / lanes / hs_rate) / (htotal / pixel_clock)
   *
   * i.e. the fraction of a line the compressed burst should occupy at this
   * link margin.  Combined with the two FIFO readings this separates the
   * failure modes:
   *
   *   input FIFO saturated + duty far below expected
   *       -> the pixels pile up because the packet builder is not converting
   *          them into line packets (what does go out is not the payload).
   *   input FIFO drained + TX FIFO cycling + duty ~= expected
   *       -> the pipeline runs end to end and the payload IS on the lanes.
   *   TX FIFO full almost always
   *       -> the PHY/link is the bottleneck (payload is being packaged).
   */

  {
    uint64_t expected_permille =
        ((uint64_t)KICKPI_K7_MIPI_DSI_XRES * 24ull *
         KICKPI_K7_MIPI_DSI_PIXCLK * 1000ull) /
        ((uint64_t)KICKPI_K7_MIPI_DSI_LANES * KICKPI_K7_MIPI_DSI_HS_RATE *
         KICKPI_K7_HTOTAL);
    uint32_t samples_total = (uint32_t)samples * 16u;
    uint32_t measured_permille =
        samples_total != 0 ? (data_hs_hits * 1000u) / samples_total : 0;
    uint32_t txhs_full_pct =
        samples_total != 0 ? (txhs_full_hits * 100u) / samples_total : 0;
    uint32_t lane_pct =
        samples_total != 0 ? (data_hs_hits * 100u) / samples_total : 0;
    uint32_t clk_pct =
        samples_total != 0 ? (clk_hs_hits * 100u) / samples_total : 0;

    syslog(LOG_INFO,
           "kickpi-k7: state[%s] live mode=%u clk=%u lane=%u%%(exp %u%% "
           "min=%u/16) clkpct=%u ipi_data=%u/%u%% hot=%u/%d txhs=%u/%u%%\n",
           tag, (unsigned)(health.vid_tx_cfg & 0x3u),
           (unsigned)(health.phy_clk_cfg & 0x1u), (unsigned)lane_pct,
           (unsigned)expected_permille / 10u,
           (unsigned)(data_hs_min_window == 0xffffffffu
                          ? 0
                          : data_hs_min_window),
           (unsigned)clk_pct, (unsigned)ipi_data_peak,
           (unsigned)(samples_total != 0
                          ? (ipi_data_nonempty_hits * 100u) / samples_total
                          : 0),
           (unsigned)ipi_data_hot_windows, samples, (unsigned)phy_txhs_peak,
           (unsigned)(samples_total != 0
                          ? (phy_txhs_nonempty_hits * 100u) / samples_total
                          : 0));

    syslog(LOG_INFO,
           "kickpi-k7: state[%s] fsm iv=%08x main=%08x pkt=%08x txr=%08x "
           "busy=%u/%d fne=%u/%d to=%08x scan=%u\n",
           tag, health.fsm_raw[0], health.fsm_raw[1], health.fsm_raw[2],
           health.fsm_raw[3], (unsigned)ipi_seen, samples, (unsigned)fne_seen,
           samples, (unsigned)to_seen, (unsigned)vcnt_peak);

    syslog(LOG_INFO,
           "kickpi-k7: state[%s] extra txhsfull=%u%% ever=%u evt=%u/%u "
           "cri=%u/%d\n",
           tag, (unsigned)txhs_full_pct, (unsigned)phy_txhs_ever,
           (unsigned)ipi_event_peak, (unsigned)ipi_event_ever,
           (unsigned)cri_seen, samples);

    /* Lane busy fraction, measured with a spread sampling grid so the number
     * is comparable between windows.  It is a per-MODE signature, not a
     * packet-size readout: "out of LP-11" also counts the non-HS low-power
     * states, so both the scope and this probe read ~89% for burst while the
     * packet size was not what decided the picture.  Burst 88-93%, non-burst
     * 98% on this board. */

    {
      uint32_t duty_pm = duty_total != 0
                             ? (duty_hits * 1000u) / duty_total
                             : 0;

      syslog(LOG_INFO,
             "kickpi-k7: state[%s] duty lanes-busy=%u.%u%% of %u samples "
             "(mode signature: burst ~88-93, non-burst ~98)\n",
             tag, (unsigned)(duty_pm / 10u), (unsigned)(duty_pm % 10u),
             (unsigned)duty_total);

      /* --- Payload budget: how much data the wire carries per frame.
       *
       * The duty fraction says how much of a frame the data lanes spend in
       * high speed; multiplied by the link's byte rate and the frame period,
       * it becomes a BYTE COUNT per frame, and that can be compared with the
       * frame's own pixel payload.  This is the only quantitative check
       * available without a scope, and it answers a question no register
       * answers: is the link actually carrying our picture?
       *
       *   wire >= pixels  -> the transmitter is sending at least a frame's
       *                      worth of pixels per frame, so the payload reaches
       *                      the PHY and the remaining question is on the
       *                      panel's side (or in the packetisation);
       *   wire <  pixels  -> the link is NOT carrying the picture, and the
       *                      shortfall is a host-side fault.
       *
       * A small excess is expected and normal: packet headers, ECC, CRC,
       * sync/blanking packets and the HS<->LP transitions all consume link
       * time that carries no pixel bytes. */

      {
        uint64_t frame_bytes = (uint64_t)KICKPI_K7_MIPI_DSI_XRES *
                               KICKPI_K7_MIPI_DSI_YRES * 3u;
        uint64_t frame_ns = (uint64_t)KICKPI_K7_HTOTAL *
                            (KICKPI_K7_MIPI_DSI_YRES + KICKPI_K7_VSYNC_LEN +
                             KICKPI_K7_VBACK_PORCH + KICKPI_K7_VFRONT_PORCH) *
                            1000000000ull / KICKPI_K7_MIPI_DSI_PIXCLK;
        uint64_t wire_bps = (uint64_t)(KICKPI_K7_MIPI_DSI_HS_RATE / 8u) *
                            KICKPI_K7_MIPI_DSI_LANES;
        uint64_t wire = wire_bps * frame_ns / 1000000000ull;

        wire = wire * duty_pm / 1000ull;

        syslog(LOG_INFO,
               "kickpi-k7: state[%s] payload budget: wire ~%lu kB/frame vs "
               "frame pixels %lu kB (%lu%%) -> %s\n",
               tag, (unsigned long)(wire / 1024u),
               (unsigned long)(frame_bytes / 1024u),
               (unsigned long)(frame_bytes != 0
                                   ? (wire * 100u) / frame_bytes
                                   : 0),
               frame_bytes != 0 && wire * 100u >= frame_bytes * 80u
                   ? "the wire carries a whole frame of pixels per frame"
                   : "the wire carries LESS than the frame's pixels: the "
                     "payload is NOT getting out (host-side shortfall)");
      }
    }

    syslog(LOG_WARNING, "kickpi-k7: state[%s] verdict: %s\n", tag,
           vcnt_peak == 0
               ? "VOP SCAN STOPPED -- the panel cannot be refreshed"
           : (vop_flags & 1u
                  ? "VOP AXI READ ERROR: the framebuffer read failed, so no "
                    "pixels exist.  Fix AXI/DMA before anything else"
                  : (data_hs_hits == 0
                         ? "NO TRANSMISSION: the data lanes never left LP-11 "
                           "while the VOP scans"
                         : (ipi_data_hot_windows == samples
                                ? "PAYLOAD BLOCKED: the IPI input FIFO held "
                                  "more than a line in EVERY window"
                                : (measured_permille + 200u <
                                           (uint32_t)expected_permille
                                       ? "UNDER-SENDING: the lanes carry less "
                                         "than this mode's payload fraction"
                                       : "LINK RUNNING: both FIFOs cycling, "
                                         "lane duty matches this mode.  Judge "
                                         "the PICTURE next, not the link")))));

    syslog(LOG_INFO,
           "kickpi-k7: state[%s] vop vp=%08x (fs=%u fs_new=%u line=%u/%u "
           "BUF_EMPTY=%u field=%u hold=%u) sys0=%08x sys1=%08x "
           "(AXIbus_err=%u)\n",
           tag, (unsigned)vop_int_seen,
           (unsigned)(vop_int_seen & 1u) != 0u,
           (unsigned)((vop_int_seen >> 1) & 1u) != 0u,
           (unsigned)((vop_int_seen >> 2) & 1u) != 0u,
           (unsigned)((vop_int_seen >> 3) & 1u) != 0u,
           (unsigned)((vop_int_seen >> 4) & 1u) != 0u,
           (unsigned)((vop_int_seen >> 5) & 1u) != 0u,
           (unsigned)((vop_int_seen >> 6) & 1u) != 0u,
           (unsigned)vop_bus_err_seen, (unsigned)vop_int.sys1,
           (unsigned)((vop_bus_err_seen & RK3576_VOP_SYS_INT_BUS_ERROR) != 0u));

    /* The per-VP bit order is NOT the SYS bit order: bit1 here is FS_NEW, a
     * per-frame event, and the AXI bus error exists only in SYS0/SYS1.  An
     * earlier version of this line read "fs/line/win bits are per-line events;
     * only sys0/sys1 are faults" and by doing so silently discarded bit4 --
     * POST_BUF_EMPTY -- which says the pixels did not arrive in time.  That is
     * not an event; it is the display pipeline stating it was starved, and it
     * is the one mechanism found so far that explains a picture which follows
     * neither the framebuffer's content nor its address. */

    if ((vop_int_seen & RK3576_VOP_INT_POST_BUF_EMPTY) != 0u)
      {
        syslog(LOG_WARNING,
               "kickpi-k7: state[%s] *** POST output buffer UNDER-RAN: the "
               "pixels did not arrive in\n", tag);
        syslog(LOG_WARNING,
               "kickpi-k7:   time, so what was transmitted is not the "
               "framebuffer.  See the under-run\n");
        syslog(LOG_WARNING,
               "kickpi-k7:   probe, which measures whether this recurs ***\n");
      }

    if (to_seen & 1u)
      {
        syslog(LOG_WARNING,
               "kickpi-k7: state[%s] INT_ST_TO=%08x: err_hstx set = an HS "
               "transmission started and never finished\n",
               tag, (unsigned)to_seen);
      }
  }
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_payload_probe
 *
 * Description:
 *   Answer whether the video PAYLOAD is moving through the IPI, using only
 *   measurements that a LIVE stream cannot confound.
 *
 *   Why this replaces the gated experiment as the primary instrument: the
 *   VOP-off test clears MIPI0_INFACE_CTRL.{out_en,clk_out_en}, and clk_out_en
 *   IS the IPI clock.  Gating the feed therefore stops the IPI block that reads
 *   the FIFO, so ANY residual word count freezes -- a healthy pipeline and a
 *   blocked one look identical.  The gated ladder is kept (it still catches
 *   "the PHY FIFO never drains"), but it cannot decide this question, and a
 *   previous round's verdict claiming it did was wrong.
 *
 *   What can decide it, with the stream untouched:
 *
 *     1. A TIGHT burst of ipi_data readings (~2 us apart, so ~1.5 line periods
 *        of wall time).  A working input FIFO is emptied and refilled once per
 *        line, so a burst that spans a line MUST show different values; ten
 *        identical readings are a frozen level, no gate and no theory needed.
 *        This is the measurement the peak-based health field cannot make: a
 *        peak is the same number whether the level oscillates or is nailed.
 *     2. The ipi_vid FSM's STATE AND CYCLE COUNT.  The state alone is useless
 *        (state 8 is where it usually is) and the hardware's 'stuck' bit is
 *        latched, so it reads 1 even after the FSM has moved on.  The cycle
 *        count resets on every state change, so a varying count proves the FSM
 *        is cycling and a monotonically growing one proves it is frozen.
 *     3. THE DECISIVE ONE: how often the DATA LANES LEAVE LP-11.  Sampled in a
 *        tight loop and compared against the number of LINES the VOP scanned
 *        during that loop, this separates the two shapes a stream can have --
 *        one LP-11 episode per LINE (the transmitter emits a real burst per
 *        line, so a panel with no frame memory gets a per-line edge to
 *        re-synchronise on) from one LP-11 episode per FRAME (one endless HS
 *        burst, in which case such a panel has nothing to structure the stream
 *        into lines with).  No other probe in this project can tell those
 *        apart, and they point at completely different fixes.
 *     4. The six INT_ST_* groups in one read, because err_ipi_dtype means the
 *        controller silently substituted a different pixel format for ipi_format
 *        /ipi_depth -- a fault that leaves every link-health test passing.
 *     5. The HS-TX timeout RATE over ten 20 ms windows.  The health log's
 *        per-window boolean cannot tell "once per stream entry" from "once per
 *        line", and those are different faults.
 *
 *   The EVENT FIFO is read too, but as a WEAK datum and it is labelled as one:
 *   a FIFO whose consumer pops in the same cycle it is pushed reads empty at
 *   every instant, so "never non-empty" cannot by itself prove "no line
 *   boundary ever reached the builder" -- an earlier round drew that conclusion
 *   and was wrong to.  The lane-episode ratio is what carries the answer.
 *
 *   Read-only EXCEPT that reading the INT_ST_* groups consumes them (they are
 *   read-clear), which is why that read comes first and is reported separately.
 *
 ****************************************************************************/

static uint32_t kickpi_k7_mipi_dsi_fifo_words(uint32_t sel)
{
  uint32_t st = rk3576_mipi_dsi_obs_fifo(sel);

  return (st & RK3576_DSI_OBS_FIFO_WORD_CNT_MASK) >>
         RK3576_DSI_OBS_FIFO_WORD_CNT_SHIFT;
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_lane_episodes
 *
 * Description:
 *   Measure the transmitted LINE STRUCTURE: how often the data lanes leave
 *   LP-11, against how many lines the VOP scanned during the same interval.
 *
 *   The VOP's scan counter is the instrument's clock, so the comparison needs
 *   no timing assumption at all.  Two shapes are possible on the wire and the
 *   ratio names which one is present:
 *
 *     episodes ~= lines  -> one LP-11 episode per line: the transmitter emits
 *                           a real per-line burst, so a panel with no frame
 *                           memory gets a per-line edge to re-synchronise on
 *     episodes << lines  -> the transmitter starts one long HS burst and
 *                           stays in high speed for the whole frame: such a
 *                           panel has nothing to structure the stream into
 *                           lines with
 *
 *   Every other probe in this project reports a REGISTER or a PERCENTAGE, and
 *   both of those look identical under the two shapes above (a 97% duty is
 *   consistent with per-line bursts whose blanking is short, and with one
 *   endless burst plus a vertical blanking period).  A count does not.
 *
 * Input Parameters:
 *   tag       - Label for the log line.
 *   lines_out - Optional out: number of lines that elapsed during the sample.
 *
 * Returned Value:
 *   Number of LP-11 -> HS transitions seen on the data lanes.
 *
 ****************************************************************************/

static uint32_t kickpi_k7_mipi_dsi_lane_episodes(FAR const char *tag,
                                                 FAR uint32_t *lines_out)
{
  uint32_t scan_before;
  uint32_t scan_after;
  uint32_t lines;
  uint32_t hs = 0;
  uint32_t clk = 0;
  uint32_t edges;

  scan_before = rk3576_vop_get_scan_counter();
  edges = rk3576_mipi_dsi_count_lane_episodes(KICKPI_K7_EPISODE_SAMPLES,
                                             &hs, &clk);
  scan_after = rk3576_vop_get_scan_counter();
  lines = (scan_after - scan_before) & 0x1fffu;

  if (lines_out != NULL)
    {
      *lines_out = lines;
    }

  syslog(LOG_WARNING,
         "kickpi-k7: lanes[%s] %u LP->HS episodes over %u lines "
         "(hs=%u%% clk_hs=%u%%) -> %s\n",
         tag, (unsigned)edges, (unsigned)lines,
         (unsigned)((hs * 100u) / KICKPI_K7_EPISODE_SAMPLES),
         (unsigned)((clk * 100u) / KICKPI_K7_EPISODE_SAMPLES),
         edges >= lines && lines != 0
             ? "PER-LINE BURSTS (each line gets its own LP-11 edge)"
             : (edges * 4u < lines
                    ? "ONE LONG HS PER FRAME (no per-line LP-11 edge)"
                    : "MIXED / few episodes"));

  return edges;
}

static void kickpi_k7_mipi_dsi_payload_probe(void)
{
  struct rk3576_dsi_int_latches_s lat;
  uint32_t burst[10];
  uint32_t slow[6];
  uint32_t evt[6];
  uint32_t fsm[6];
  uint32_t fsmcnt[6];
  uint32_t to_hits = 0;
  uint32_t lines = 0;
  uint32_t edges;
  bool burst_moves = false;
  bool slow_moves = false;
  bool evt_ever = false;
  bool fsm_moves = false;
  int i;

  /* 0. FIRST, because these are read-clear: the six error groups.  Split over
   *    two short lines so a dropped byte in the console cannot hide the IPI
   *    group, whose err_ipi_dtype bit is the single most valuable bit here:
   *    the TRM says the controller then silently falls back to packed 24-bit,
   *    i.e. a wrong picture with a flawless-looking link. */

  if (rk3576_mipi_dsi_read_int_latches(&lat) == OK)
    {
      syslog(LOG_WARNING, "kickpi-k7: latches A main=%08x ipi=%08x\n",
             (unsigned)lat.main, (unsigned)lat.ipi);
      syslog(LOG_WARNING, "kickpi-k7: latches B fifo=%08x phy=%08x ack=%08x "
             "to=%08x\n",
             (unsigned)lat.fifo, (unsigned)lat.phy, (unsigned)lat.ack,
             (unsigned)lat.to);

      if ((lat.ipi & RK3576_DSI_INT_IPI_ERR_DTYPE) != 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: err_ipi_dtype SET -- ipi_format/ipi_depth has no "
                 "matching DSI-2 data type and the controller fell back to "
                 "packed 24-bit.  THE PIXEL FORMAT CONFIGURATION IS WRONG\n");
        }
    }

  /* 1. Tight burst: ~2 us per reading, so the whole burst covers rather more
   *    than one line period (13.1 us).  Anything that cycles per line MUST be
   *    caught varying here. */

  for (i = 0; i < 10; i++)
    {
      burst[i] = kickpi_k7_mipi_dsi_fifo_words(RK3576_DSI_OBS_FIFO_IPI_DATA);

      if (i != 0 && burst[i] != burst[0])
        {
          burst_moves = true;
        }
    }

  /* 2. Slow ladder across ~1.4 k lines (6 x 3 ms) for the input FIFO, the
   *    event FIFO and the IPI video FSM (state AND cycle count). */

  for (i = 0; i < 6; i++)
    {
      slow[i] = kickpi_k7_mipi_dsi_fifo_words(RK3576_DSI_OBS_FIFO_IPI_DATA);
      evt[i] = kickpi_k7_mipi_dsi_fifo_words(RK3576_DSI_OBS_FIFO_IPI_EVENT);

      fsm[i] = rk3576_mipi_dsi_obs_fsm(RK3576_DSI_OBS_FSM_IPI_VID);
      fsmcnt[i] = (fsm[i] & RK3576_DSI_OBS_FSM_CNT_MASK) >>
                  RK3576_DSI_OBS_FSM_CNT_SHIFT;

      if (i != 0)
        {
          if (slow[i] != slow[0])
            {
              slow_moves = true;
            }

          /* Any change in the raw word counts as movement, but the cycle
           * count is the sharp half: it alone proves the state changed. */

          if ((fsm[i] & RK3576_DSI_OBS_FSM_CUR_MASK) !=
              (fsm[0] & RK3576_DSI_OBS_FSM_CUR_MASK))
            {
              fsm_moves = true;
            }
        }

      if (evt[i] != 0)
        {
          evt_ever = true;
        }

      up_mdelay(3);
    }

  /* 3. THE DECISIVE MEASUREMENT: lane LP-11 episodes vs lines elapsed. */

  edges = kickpi_k7_mipi_dsi_lane_episodes("live", &lines);

  /* 4. The timeout rate (read-clear, so this counts its own window only). */

  for (i = 0; i < 10; i++)
    {
      if ((rk3576_mipi_dsi_read_to_status() & 1u) != 0)
        {
          to_hits++;
        }

      up_mdelay(20);
    }

  /* 5. Report.  The tight burst is printed in full because it is only ten
   *    small numbers and its variation IS the measurement. */

  syslog(LOG_WARNING,
         "kickpi-k7: payload ipi_data burst %u %u %u %u %u %u %u %u %u %u -> "
         "%s\n",
         (unsigned)burst[0], (unsigned)burst[1], (unsigned)burst[2],
         (unsigned)burst[3], (unsigned)burst[4], (unsigned)burst[5],
         (unsigned)burst[6], (unsigned)burst[7], (unsigned)burst[8],
         (unsigned)burst[9],
         burst_moves ? "CYCLES" : "FROZEN");

  syslog(LOG_WARNING,
         "kickpi-k7: payload slow data %u %u %u %u %u %u %s | evt %u %u %u %u "
         "%u %u %s (weak: a same-cycle consumer always reads empty)\n",
         (unsigned)slow[0], (unsigned)slow[1], (unsigned)slow[2],
         (unsigned)slow[3], (unsigned)slow[4], (unsigned)slow[5],
         slow_moves ? "moves" : "PINNED",
         (unsigned)evt[0], (unsigned)evt[1], (unsigned)evt[2],
         (unsigned)evt[3], (unsigned)evt[4], (unsigned)evt[5],
         evt_ever ? "seen" : "NEVER");

  syslog(LOG_WARNING,
         "kickpi-k7: payload ipi_vid %02x %02x %02x %02x %02x %02x -> %s | "
         "counts %u %u %u %u %u %u | to %u/10 win\n",
         (unsigned)(fsm[0] & RK3576_DSI_OBS_FSM_CUR_MASK),
         (unsigned)(fsm[1] & RK3576_DSI_OBS_FSM_CUR_MASK),
         (unsigned)(fsm[2] & RK3576_DSI_OBS_FSM_CUR_MASK),
         (unsigned)(fsm[3] & RK3576_DSI_OBS_FSM_CUR_MASK),
         (unsigned)(fsm[4] & RK3576_DSI_OBS_FSM_CUR_MASK),
         (unsigned)(fsm[5] & RK3576_DSI_OBS_FSM_CUR_MASK),
         fsm_moves ? "MOVING" : "SAME-STATE",
         (unsigned)fsmcnt[0], (unsigned)fsmcnt[1], (unsigned)fsmcnt[2],
         (unsigned)fsmcnt[3], (unsigned)fsmcnt[4], (unsigned)fsmcnt[5],
         (unsigned)to_hits);

  /* 6. The verdict, and only the part the data can carry.
   *
   * NOTE ON WHAT IS NO LONGER CLAIMED HERE.  An earlier version of this line
   * said that "one LP-11 episode per frame" was THE fault to chase, on the
   * reasoning that a panel with no frame memory needs a per-line edge.  That
   * reasoning does not survive contact with the arithmetic: in a NON-burst mode
   * at ~1x link margin the payload occupies the whole active window, so the
   * controller has no time to return to low power between lines and is EXPECTED
   * to stay in high speed across the frame.  The per-line LP-11 gaps a burst
   * stream shows come from its time compression, not from health.  Reading a
   * normal consequence of the configured margin as a defect sent two rounds of
   * this bring-up down the wrong path, so the claim is gone.
   *
   * What the measurement is still good for is the CONTRAST it can make: a burst
   * state at its proper x10/9 rate should show per-line episodes, and if it does
   * not, the burst is not being packetised per line.  Compare the lanes[] lines
   * between states rather than reading any single one as a verdict. */

  syslog(LOG_WARNING, "kickpi-k7: payload verdict: %s\n",
         !burst_moves && !slow_moves
             ? "the input FIFO level does NOT move on a live, un-gated stream: "
               "the pixels are not being consumed, so no line packet can be "
               "built (host-side: IPI -> SYS handoff)"
         : (edges * 4u < lines && lines != 0
                ? "pixels are flowing and being consumed, but the wire shows "
                  "FEWER LP-11 episodes than lines (see the lanes[] line).  "
                  "For a NON-burst mode at 1x margin that is expected; for a "
                  "BURST mode with its x10/9 headroom it means the per-line "
                  "packetisation is not happening -- compare this against the "
                  "burst states of the matrix before drawing a conclusion"
                : "pixels are flowing, being consumed, and the wire shows "
                  "per-line LP-11 episodes: the transmitted structure looks "
                  "like one packet per line, so the remaining question is what "
                  "the panel does with those packets")
         );
}

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_entry_probe
 *
 * Description:
 *   Paint one whole-screen colour and FORCE a stream re-entry, once per colour,
 *   announcing each step and holding long enough to look.  See
 *   KICKPI_K7_ENTRY_PROBE for the hypothesis and the three possible outcomes.
 *
 *   Order of operations inside a step matters and is the whole experiment:
 *   the colour is painted BEFORE the re-entry, so that the very first frames
 *   the panel can latch after the entry already carry that colour.  Painting
 *   after the entry would be the steady-stream case that has never worked.
 *
 *   The transmitted structure is measured in each state as well, because it
 *   costs one log line and says whether the re-entry also changed the wire
 *   (per-line bursts versus one long HS per frame).
 *
 *   Destructive by design: each step stops and restarts the stream.  It runs
 *   LAST in bring-up, after every read-only probe has already reported.
 *
 ****************************************************************************/

#if KICKPI_K7_ENTRY_PROBE

static void kickpi_k7_mipi_dsi_entry_probe(FAR struct mipi_dsi_host *host)
{
  static const struct
  {
    uint32_t rgb;
    FAR const char *name;
  } steps[] =
  {
    { 0xff0000, "RED" },
    { 0x00ff00, "GREEN" },
    { 0x0000ff, "BLUE" },
    { 0xffffff, "WHITE" },
    { 0x808000, "OLIVE (deliberately not a primary)" },
  };

  uint32_t lines = 0;
  int i;

  syslog(LOG_WARNING,
         "kickpi-k7: ENTRY PROBE: %d colours, each painted BEFORE a forced "
         "stream re-entry,\n", (int)nitems(steps));
  syslog(LOG_WARNING,
         "kickpi-k7:   %d ms each.  Watch for the colour that is ANNOUNCED "
         "next -- and whether\n", KICKPI_K7_ENTRY_DWELL_MS);
  syslog(LOG_WARNING,
         "kickpi-k7:   it appears at all, holds, or fades.  Report EVERY "
         "step, including 'nothing'\n");

  for (i = 0; i < (int)nitems(steps); i++)
    {
      int ret;

      /* 1. Paint first: the frames the panel latches on entry must already
       *    carry the colour under test. */

      if (rk3576_vop_fill(steps[i].rgb) != OK)
        {
          syslog(LOG_ERR, "kickpi-k7: ENTRY: cannot paint the framebuffer\n");
          return;
        }

      /* 2. Force the re-entry with the driver's own pair.  disable_video()
       *    cannot complete cleanly while the IPI is wedged, but it performs
       *    the full datapath rearm, which is the part that matters here. */

      ret = rk3576_mipi_dsi_disable_video(host);
      up_mdelay(30);

      ret = rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing);

      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: ENTRY %d/%d %s: re-entry FAILED (%d) -- "
                 "stopping\n", i + 1, (int)nitems(steps), steps[i].name, ret);
          return;
        }

      /* 3. Announce, then hold.  The announcement carries the colour in words
       *    so a console line mangled by the UART still identifies the step. */

      syslog(LOG_WARNING,
             "kickpi-k7: ENTRY %d/%d -> screen should be %s NOW "
             "(0x%06x)\n",
             i + 1, (int)nitems(steps), steps[i].name, (unsigned)steps[i].rgb);

      up_mdelay(KICKPI_K7_ENTRY_DWELL_MS);

      /* 4. One line of wire structure for this state. */

      (void)kickpi_k7_mipi_dsi_lane_episodes("entry", &lines);
    }

  syslog(LOG_WARNING,
         "kickpi-k7: ENTRY PROBE DONE -- report which colours appeared, "
         "whether they held, and what it ended as\n");
}

#endif /* KICKPI_K7_ENTRY_PROBE */

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_mode_eotp_matrix
 *
 * Description:
 *   Walk the (video mode x EoTp) combinations, giving each one a whole-screen
 *   colour, a forced stream re-entry and a structure measurement.  See
 *   KICKPI_K7_MODE_EOTP_MATRIX for why this is the remaining experiment and
 *   how to read it.
 *
 *   The re-entry is the only way to apply the change: the mode and the general
 *   config are both programmed by enable_video(), and set_link_mode() refuses
 *   to touch a live stream on purpose.
 *
 ****************************************************************************/

#if KICKPI_K7_MODE_EOTP_MATRIX

static void kickpi_k7_mipi_dsi_mode_eotp_matrix(FAR struct mipi_dsi_host *host)
{
  static const struct
  {
    uint8_t mode;
    bool eotp;
    bool cont;
    uint32_t rgb;
    FAR const char *name;
  } combos[] =
  {
    /* MOST LIKELY FIRST: the combination every Rockchip RK3576 reference
     * panel node uses (rk3576-tablet.dtsi, rk3576s-tablet-v10.dts):
     *   MIPI_DSI_MODE_VIDEO | VIDEO_BURST | LPM | NO_EOT_PACKET |
     *   MIPI_DSI_CLOCK_NON_CONTINUOUS
     * The clock-lane axis is here because BOTH previous matrices held it at
     * "continuous", so this half of the Rockchip reference combination had
     * never been measured on the current (correct) timing. */

    { KICKPI_K7_DSI_VID_MODE_BURST, false, false, 0xff0000, "RED" },
    { KICKPI_K7_DSI_VID_MODE_BURST, false, true, 0x00ff00, "GREEN" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS, false, false, 0x0000ff,
      "BLUE" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, false, false, 0xffffff,
      "WHITE" },
    { KICKPI_K7_DSI_VID_MODE_BURST, true, false, 0xffff00, "YELLOW" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS, true, false, 0x00ffff,
      "CYAN" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, true, false, 0xff00ff,
      "MAGENTA" },
    { KICKPI_K7_DSI_VID_MODE_BURST, true, true, 0xff8000, "ORANGE" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS, true, true, 0x8000ff,
      "VIOLET" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, true, true, 0x808000,
      "OLIVE" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_EVENTS, false, true, 0x008080,
      "TEAL" },
    { KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES, false, true, 0x808080,
      "GREY" },
  };

  uint32_t lines = 0;
  const bool boot_is_burst =
      (KICKPI_K7_DSI_VID_MODE == KICKPI_K7_DSI_VID_MODE_BURST);
  int i;

  syslog(LOG_WARNING,
         "kickpi-k7: MODE/EOTP MATRIX: %d states, %d ms each, one whole-screen "
         "colour per state\n", (int)nitems(combos),
         KICKPI_K7_MODE_EOTP_DWELL_MS);
  syslog(LOG_WARNING,
         "kickpi-k7:   state 1 is the combination every Rockchip RK3576 "
         "reference uses\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   (BURST + EoTp off + NON-continuous clock); each state "
         "re-enters the stream\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   REPORT which announced colour appears, and whether it "
         "is the right colour\n");

  /* *** THE RATE IS FIXED AT BOOT: A STATE CAN BE INVALID. ***
   *
   * KICKPI_K7_MIPI_DSI_HS_RATE is derived at COMPILE time from the compile-time
   * video mode, and it is baked into the DCPHY PLL when the PHY is powered on
   * (rk3576_mipi_dsi_initialize).  set_link_mode() changes the video mode at
   * runtime but CANNOT change the link rate, so a state whose mode needs a
   * different rate runs at the WRONG rate -- and "wrong rate" is exactly the
   * kind of thing that makes a test meaningless rather than negative.
   *
   * It matters most for BURST, which is the reason this matters at all: burst
   * time-compresses each line's payload into a packet sent as fast as possible,
   * so it needs x10/9 of the raw rate (the reference driver applies exactly
   * that factor, "take 1 / 0.9, since Mbps must big than bandwidth of RGB").
   * At 1x the payload of a line (720 px * 3 B = 2160 B) occupies the entire
   * active window (11.51 us of a 12.47 us line at 62.5 Mpx/s), so there is NO
   * time compression to make and the controller simply stays in high speed
   * across the frame: burst degenerates into a continuous stream with no
   * per-line packet, and it is indistinguishable from non-burst.
   *
   * That is precisely what happened to every earlier matrix: the log shows
   * every burst state at hs_rate=375160000, i.e. the non-burst rate.  So the
   * run below is the FIRST time a genuine burst (with its x10/9 headroom) can
   * be tested at boot -- which is why the board default is now BURST. */

  syslog(LOG_WARNING,
         "kickpi-k7:   link rate is FIXED AT BOOT (hs_rate=%u); states whose "
         "mode needs a\n", (unsigned)KICKPI_K7_MIPI_DSI_HS_RATE);
  syslog(LOG_WARNING,
         "kickpi-k7:   different rate are marked RATE-WRONG and cannot be "
         "read as test results\n");

  for (i = 0; i < (int)nitems(combos); i++)
    {
      int ret;

      /* 1. Stop the stream: set_link_mode() and the general config are only
       *    honoured outside Video mode, and the colour must already be in the
       *    framebuffer before the stream comes back. */

      (void)rk3576_mipi_dsi_disable_video(host);

      rk3576_mipi_dsi_set_link_mode(host, combos[i].mode, combos[i].cont);
      rk3576_mipi_dsi_set_eotp(host, combos[i].eotp);

      if (rk3576_vop_fill(combos[i].rgb) != OK)
        {
          syslog(LOG_ERR, "kickpi-k7: MATRIX: cannot paint the framebuffer\n");
          return;
        }

      up_mdelay(30);

      /* 2. Restart: enable_video programs VIDEO CFG and GENERAL_CFG from the
       *    staged settings, so this is the point at which the state changes. */

      ret = rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: MATRIX %d/%d: re-entry FAILED (%d) -- stopping\n",
                 i + 1, (int)nitems(combos), ret);
          return;
        }

      /* 3. Announce in words AND numbers, then hold long enough to look. */

      syslog(LOG_WARNING,
             "kickpi-k7: MATRIX %d/%d -> mode_type=%u (%s) eotp=%u clk=%s "
             "%s -- screen should be %s NOW (0x%06x)\n",
             i + 1, (int)nitems(combos), (unsigned)combos[i].mode,
             combos[i].mode == KICKPI_K7_DSI_VID_MODE_BURST
                 ? "BURST"
                 : (combos[i].mode ==
                            KICKPI_K7_DSI_VID_MODE_NON_BURST_SYNC_PULSES
                        ? "sync-pulses"
                        : "sync-events"),
             (unsigned)(combos[i].eotp ? 1 : 0),
             combos[i].cont ? "continuous" : "NON-cont",
             ((combos[i].mode == KICKPI_K7_DSI_VID_MODE_BURST) == boot_is_burst)
                 ? "RATE-OK"
                 : "RATE-WRONG(ignored)",
             combos[i].name, (unsigned)combos[i].rgb);

      up_mdelay(KICKPI_K7_MODE_EOTP_DWELL_MS);

      (void)kickpi_k7_mipi_dsi_lane_episodes("matrix", &lines);
    }

  /* Restore the board's configured default so the state the system keeps is the
   * one the source describes, not whichever entry happened to be last. */

  (void)rk3576_mipi_dsi_disable_video(host);
  rk3576_mipi_dsi_set_link_mode(host, KICKPI_K7_DSI_VID_MODE,
                                KICKPI_K7_DSI_CONTINUOUS_CLK);
  rk3576_mipi_dsi_set_eotp(host, KICKPI_K7_DSI_DEFAULT_EOTP);
  rk3576_vop_fill(0x808080);

  if (rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing) < 0)
    {
      syslog(LOG_ERR, "kickpi-k7: MATRIX: could not restore the default\n");
      return;
    }

  syslog(LOG_WARNING,
         "kickpi-k7: MODE/EOTP MATRIX DONE -- the configured default "
         "(mode_type=%u, eotp=%u, continuous=%u) is back.  Report every "
         "state's colour, including 'nothing'\n",
         (unsigned)KICKPI_K7_DSI_VID_MODE,
         (unsigned)(KICKPI_K7_DSI_DEFAULT_EOTP ? 1 : 0),
         (unsigned)(KICKPI_K7_DSI_CONTINUOUS_CLK ? 1 : 0));
}

#endif /* KICKPI_K7_MODE_EOTP_MATRIX */

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_strobe_test
 *
 * Description:
 *   Alternate the whole screen between BLACK and WHITE on an undisturbed stream
 *   and announce each flip.  See KICKPI_K7_STROBE_TEST for why this settles the
 *   "does content reach the glass" question and why the colours are byte-uniform
 *   on purpose.
 *
 ****************************************************************************/

#if KICKPI_K7_STROBE_TEST

static void kickpi_k7_mipi_dsi_strobe_test(void)
{
  int i;

  syslog(LOG_WARNING,
         "kickpi-k7: STROBE TEST: full-screen BLACK/WHITE every %d ms for %d "
         "steps, on\n", KICKPI_K7_STROBE_MS, KICKPI_K7_STROBE_STEPS);
  syslog(LOG_WARNING,
         "kickpi-k7:   an UNDISTURBED stream (nothing is stopped or "
         "re-programmed).\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   black and white are byte-uniform, so ANY byte-level "
         "fault would still\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   leave them black and white: if the glass is driven by "
         "our bytes at all,\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   THE SCREEN MUST FLASH.  Report plainly whether it "
         "flashes.\n");

  for (i = 0; i < KICKPI_K7_STROBE_STEPS; i++)
    {
      bool white = (i & 1) != 0;

      (void)rk3576_vop_fill(white ? 0xffffffu : 0x000000u);

      syslog(LOG_WARNING, "kickpi-k7: STROBE %d/%d -> %s\n", i + 1,
             KICKPI_K7_STROBE_STEPS, white ? "WHITE" : "BLACK");

      up_mdelay(KICKPI_K7_STROBE_MS);
    }

  syslog(LOG_WARNING,
         "kickpi-k7: STROBE TEST DONE -- did the screen flash black/white at "
         "all?\n");
}

#endif /* KICKPI_K7_STROBE_TEST */

/* Address echo test -- see KICKPI_K7_ADDR_ECHO_TEST for what the two possible
 * outcomes mean and why the register readback is not a witness for it. */

#if KICKPI_K7_ADDR_ECHO_TEST

static void kickpi_k7_mipi_dsi_address_echo_test(void)
{
  int i;

  syslog(LOG_WARNING,
         "kickpi-k7: ADDRESS ECHO TEST: alternating the ESMART layer's fetch "
         "between two\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   separate buffers, %d steps of %d ms.  This does NOT "
         "depend on the\n", KICKPI_K7_ADDR_ECHO_STEPS,
         KICKPI_K7_ADDR_ECHO_DWELL_MS);
  syslog(LOG_WARNING,
         "kickpi-k7:   panel decoding our stream -- it only asks whether the "
         "dump MOVED.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   If the screen alternates BLACK/WHITE, the fetch follows "
         "the address\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   register and memory mapping is NOT the fault.  If it "
         "never changes, the\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   layer is reading somewhere else entirely.\n");

  for (i = 0; i < KICKPI_K7_ADDR_ECHO_STEPS; i++)
    {
      bool second = (i & 1) != 0;
      uint32_t rgb = second ? 0xffffffu : 0x000000u;
      uint32_t pa = rk3576_vop_address_echo(second, rgb);

      syslog(LOG_WARNING,
             "kickpi-k7: ECHO %d/%d -> %s buffer, %s, pa=%08x\n", i + 1,
             KICKPI_K7_ADDR_ECHO_STEPS, second ? "SECOND" : "PRIMARY",
             second ? "WHITE" : "BLACK", (unsigned)pa);

      up_mdelay(KICKPI_K7_ADDR_ECHO_DWELL_MS);
    }

  syslog(LOG_WARNING,
         "kickpi-k7: ADDRESS ECHO DONE -- did the screen alternate "
         "black/white?\n");
}

#endif /* KICKPI_K7_ADDR_ECHO_TEST */

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_auto_probe
 *
 * Description:
 *   Toggle the IPI timing source at runtime (manual_mode_en -> 0), read the
 *   read-only AUTO registers back, measure the transmitted structure on both
 *   settings, and restore the known configuration.
 *
 *   *** WHAT THIS PROBE ACTUALLY ESTABLISHED -- read before trusting the
 *   *** values it prints.  An earlier version of this comment promised that
 *   *** the AUTO registers would hold "the controller's own MEASUREMENT of the
 *   *** incoming stream".  The measurement below disproves that reading:
 *
 *     - clear manual_mode_en is NOT auto-calculation.  Per TRM 18.3.1 the
 *       calibration procedure is the operating mode MODE_CTRL = AutoCalculation
 *       (3'b001), which "stops the data reception from the IPI, CRI and PRI
 *       interfaces" and ends in Idle mode.  It cannot be run while a video
 *       stream has to survive, so this probe never ran it.
 *     - the VERTICAL auto registers came back exactly equal to the values we
 *       program by hand (10/20/1280/10) while the HORIZONTAL ones stayed 0.
 *       A measurement landing on the programmed values to the last line would
 *       be a coincidence; "vertical values are reported, horizontal ones are
 *       computed by the calibration procedure we did not run" explains the
 *       asymmetry directly.  So H_AUTO == 0 says NOTHING about the stream, and
 *       must not be read as "the controller sees no line structure".
 *     - the one thing it DID measure: with manual_mode_en = 0 at runtime the
 *       controller stops driving the data lanes altogether (lanes[auto] reads
 *       0% high speed, against 100% in manual mode).  The configuration is not
 *       usable, which is why manual mode is restored.
 *
 *   Kept, because both of those facts are worth having in the log on every
 *   boot, and because the AUTO registers are the only place where a future
 *   question about the controller's own view could be answered -- with the
 *   real calibration procedure, run before Video mode, if it is ever wanted.
 *
 ****************************************************************************/

#if KICKPI_K7_AUTO_PROBE

static void kickpi_k7_mipi_dsi_auto_probe(void)
{
  struct rk3576_dsi_ipi_view_s view;
  uint32_t lines_man = 0;
  uint32_t lines_auto = 0;
  uint32_t edges_man;
  uint32_t edges_auto;
  bool repaired;

  edges_man = kickpi_k7_mipi_dsi_lane_episodes("man", &lines_man);

  if (rk3576_mipi_dsi_read_ipi_view(&view) != OK)
    {
      syslog(LOG_ERR, "kickpi-k7: AUTO PROBE: cannot read the IPI registers\n");
      return;
    }

  syslog(LOG_WARNING,
         "kickpi-k7: AUTO PROBE man HSA=%08x HBP=%08x HACT=%08x HLINE=%08x "
         "(integral %u/%u/%u/%u)\n",
         (unsigned)view.hsa_man, (unsigned)view.hbp_man,
         (unsigned)view.hact_man, (unsigned)view.hline_man,
         (unsigned)(view.hsa_man >> 16), (unsigned)(view.hbp_man >> 16),
         (unsigned)(view.hact_man >> 16), (unsigned)(view.hline_man >> 16));

  if (rk3576_mipi_dsi_set_manual_mode(false) != OK)
    {
      syslog(LOG_ERR, "kickpi-k7: AUTO PROBE: cannot select auto timing\n");
      return;
    }

  syslog(LOG_WARNING,
         "kickpi-k7: AUTO PROBE: manual_mode_en=0 (auto timing values).  "
         "Waiting 200 ms (~12 frames)\n");

  up_mdelay(200);

  if (rk3576_mipi_dsi_read_ipi_view(&view) != OK)
    {
      syslog(LOG_ERR, "kickpi-k7: AUTO PROBE: cannot read the IPI registers\n");
      return;
    }

  syslog(LOG_WARNING,
         "kickpi-k7: AUTO PROBE status manual_mode=%08x mode_status=%08x "
         "vid_tx_cfg=%08x ipi_ratio=%08x\n",
         (unsigned)view.manual_mode, (unsigned)view.mode_status,
         (unsigned)view.vid_tx_cfg, (unsigned)view.phy_ipi_ratio);

  /* Labelled "reported", not "measured", on purpose -- see the description. */

  syslog(LOG_WARNING,
         "kickpi-k7: AUTO PROBE reported HSA=%08x HBP=%08x HACT=%08x "
         "HLINE=%08x (H stays 0: computed only by the AutoCalculation "
         "operating mode, which this probe does not run)\n",
         (unsigned)view.hsa_auto, (unsigned)view.hbp_auto,
         (unsigned)view.hact_auto, (unsigned)view.hline_auto);

  syslog(LOG_WARNING,
         "kickpi-k7: AUTO PROBE reported VSA=%08x VBP=%08x VACT=%08x "
         "VFP=%08x (expected: a mirror of the programmed values)\n",
         (unsigned)view.vsa_auto, (unsigned)view.vbp_auto,
         (unsigned)view.vact_auto, (unsigned)view.vfp_auto);

  edges_auto = kickpi_k7_mipi_dsi_lane_episodes("auto", &lines_auto);

  repaired = (lines_auto != 0) && (edges_auto >= lines_auto);

  syslog(LOG_INFO,
         "kickpi-k7: AUTO PROBE numbers: man %u/%u cyc, reported %u/%u cyc, "
         "lines man=%u auto=%u, edges man=%u auto=%u\n",
         (unsigned)(view.hact_man >> 16), (unsigned)(view.hline_man >> 16),
         (unsigned)(view.hact_auto >> 16), (unsigned)(view.hline_auto >> 16),
         (unsigned)lines_man, (unsigned)lines_auto, (unsigned)edges_man,
         (unsigned)edges_auto);

  syslog(LOG_WARNING, "kickpi-k7: AUTO PROBE verdict: %s\n",
         repaired
             ? "with manual_mode_en=0 the stream changed to per-line bursts -- "
               "leave it cleared and re-run the payload probe to confirm"
             : "auto TIMING VALUES are not usable: with manual_mode_en=0 the "
               "controller stopped driving the data lanes (lanes[auto] reads "
               "0% high speed).  The reported V values are a mirror of the "
               "programmed ones and the H values are only filled by the "
               "AutoCalculation operating mode, so this probe says nothing "
               "about the incoming stream.  Manual timing restored");

  if (!repaired)
    {
      /* Put the known configuration back.  update_pixel_clock() rewrites the
       * manual H timing and PHY_IPI_RATIO together, which is exactly the
       * register set the toggle may have disturbed. */

      (void)rk3576_mipi_dsi_set_manual_mode(true);
      (void)rk3576_mipi_dsi_update_pixel_clock(KICKPI_K7_MIPI_DSI_PIXCLK);

      syslog(LOG_WARNING,
             "kickpi-k7: AUTO PROBE: manual timing and the manual registers "
             "restored\n");
    }
}

#endif /* KICKPI_K7_AUTO_PROBE */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_initialize
 ****************************************************************************/

int kickpi_k7_mipi_dsi_initialize(void)
{
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *dev;
  struct rk3576_dsi_config dsi_cfg;
  struct rk3576_vop_config vop_cfg;
  int ret;

  /* 1. Build the DSI link configuration.  rk3576_mipi_dsi_initialize()
   *    brings up the DCPHY and enters Command mode internally, so the
   *    panel DCS init sequence can be sent right after this call.
   */

  memset(&dsi_cfg, 0, sizeof(dsi_cfg));
  dsi_cfg.lanes = KICKPI_K7_MIPI_DSI_LANES;
  dsi_cfg.format = KICKPI_K7_MIPI_DSI_FORMAT;
  dsi_cfg.video_mode = KICKPI_K7_DSI_VID_MODE;
  dsi_cfg.hs_rate = KICKPI_K7_MIPI_DSI_HS_RATE;

  /* NON-CONTINUOUS clock lane, paired with a non-burst video mode.
   *
   * The clock lane must return to LP-11 after every line's HS burst so the
   * panel can re-sync its per-line SoT/HSDT handshake.  Three independent
   * sources agree:
   *
   *   - the panel spec itself: ILI9881D documents a THS-EXIT parameter
   *     (Table 46, "time to drive LP-11 after HS burst") and Figure 5 shows
   *     HSCM -> HS-0 -> LP-11, i.e. the clock lane DOES leave HS each burst;
   *   - the mainline peers with byte-identical timing to this panel
   *     (bananapi,lhr050h41, startek,kd050hdfia020) do not set
   *     MIPI_DSI_CLOCK_NON_CONTINUOUS, so Linux drives them with a returning
   *     clock under a non-burst mode;
   *   - measurement: with a continuous clock the IPI pixel FIFO stays empty
   *     (see the video-mode comment above), i.e. pixels never reach the link.
   *
   * The previous value (true) came from a BURST-mode analogy with
   * wanchanglong,w552946aba that is no longer applicable now that BURST has
   * been abandoned. */

  dsi_cfg.continuous_clk = KICKPI_K7_DSI_CONTINUOUS_CLK;
  dsi_cfg.eotp = KICKPI_K7_DSI_DEFAULT_EOTP;

  host = rk3576_mipi_dsi_initialize(&dsi_cfg);
  if (host == NULL)
    {
      syslog(LOG_ERR, "ERROR: rk3576_mipi_dsi_initialize failed\n");
      return -ENODEV;
    }

  /* 2. Register the host and bind a panel device. */

  ret = mipi_dsi_host_register(host);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_host_register failed: %d\n", ret);
      return ret;
    }

  dev = mipi_dsi_device_register(host, "kickpi-k7-lcd", KICKPI_K7_DSI_VC);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_device_register failed\n");
      return -ENODEV;
    }

  /* 3. Power-cycle the panel, release reset, send the DCS init and verify
   *    that the panel latched it (retrying with another power cycle if not). */

  kickpi_k7_mipi_dsi_configure_pins();

  ret = kickpi_k7_mipi_dsi_panel_bringup(dev);
  if (ret < 0)
    {
      return ret;
    }

  /* 3a'.  *** ENSURE THE PANEL IS NOT RUNNING ITS OWN BIST PATTERN. ***
   *
   *      This runs unconditionally.  The panel's BIST is free-running: while
   *      FRM_EN is set the panel generates its own image and has no use for
   *      the incoming video stream, so anything seen on the glass would be the
   *      panel's picture and not this driver's output.  The BIST probe further
   *      down used to enable it on every boot and turn it off with an
   *      unchecked write that also left the pattern selection armed at 0xFF.
   *      Clearing it here, and READING THE ENABLE BACK, is what makes the
   *      screen a trustworthy witness from this point on. */

  (void)kickpi_k7_mipi_dsi_bist_clear(dev);

#if KICKPI_K7_BIST_TEST
  /* 3b. Ask the PANEL to show its own test pattern, with no video stream
   *     running at all.  This is the only remaining measurement that can
   *     split "the panel is fine but cannot use our stream" from "the panel
   *     is not programmed correctly in the first place", and it must happen
   *     HERE -- in Command mode, before the pixel path exists -- because the
   *     pattern is generated by the panel itself and needs no input. */

  kickpi_k7_mipi_dsi_backlight_enable();
  (void)kickpi_k7_mipi_dsi_bist_test(dev);
#endif

#if KICKPI_K7_GRAM_TEST
  /* 3c. Then ask which display path is live: the panel's own frame memory, or
   *     our video stream.  Two standard DCS commands answer it; if the answer
   *     is "frame memory", the second half writes a solid colour into it and
   *     the project has a usable display path either way.  Must run HERE, in
   *     Command mode with no video stream, both because the commands need a
   *     reliable CRI and because a live video stream would mask the result. */

  (void)kickpi_k7_mipi_dsi_gram_test(dev);
#endif

  /* 4. Bring up the VOP framebuffer that feeds the DSI IPI on PORT0, BEFORE
   *    switching the DSI into video mode.  The order is the point of this
   *    step, not an accident:
   *
   *      Before: panel init -> DSI into VIDEO mode -> VOP up (pixel clock
   *              appears afterwards, plus a dclk reset pulse)
   *      Now:    panel init -> VOP up (pixel clock live and stable) -> DSI
   *              into VIDEO mode
   *
   *    Why this changed.  A bring-up log showed the data-lane duty going from
   *    1/16 to 15/16 -- i.e. from "the transmitter idles, the IPI input FIFO
   *    stays saturated at 768 words" to "the payload streams at the expected
   *    burst duty with the FIFO drained" -- across nothing but a
   *    disable_video()/enable_video() cycle, with byte-identical registers.
   *    The only difference between the two situations is WHEN video mode was
   *    entered: the boot path entered it while the pixel clock did not exist
   *    yet, the rearm entered it while the pixel stream was already running.
   *
   *    That matches the reference driver's ordering too: in Linux the CRTC
   *    (which sets and gates dclk, programs the MIPI interface and starts
   *    scan-out) is enabled BEFORE the DSI encoder's atomic_enable() switches
   *    the host into video mode (drm_atomic_helper_commit_modeset_enables()
   *    enables CRTCs first, then encoders).
   *
   *    The mechanism worth keeping in mind: entering video mode makes the IPI
   *    timing generator count lines out of ipi_clk (= pixel clock / 4).  Doing
   *    that with no pixel clock at all, and then having the clock appear and
   *    be interrupted once more by the VOP's dclk reset pulse, is a very good
   *    way to leave that generator waiting for a boundary that never comes --
   *    which is exactly the observed signature (pixels pile up in ipi_data,
   *    no line packet is ever built, the lanes stay idle).
   *
   *    Command mode is unaffected by the VOP running: the TRM states that
   *    Command mode ignores the IPI interface, so pixels arriving before the
   *    switch simply go nowhere.
   */

  memset(&vop_cfg, 0, sizeof(vop_cfg));
  vop_cfg.xres = KICKPI_K7_MIPI_DSI_XRES;
  vop_cfg.yres = KICKPI_K7_MIPI_DSI_YRES;
  vop_cfg.iface = RK3576_VOP_IFACE_MIPI_DSI;
  vop_cfg.port = RK3576_VOP_PORT0;
  vop_cfg.display = RK3576_VOP_DISPLAY_DEFAULT;
  vop_cfg.plane = 0;
  vop_cfg.hsync_len = KICKPI_K7_HSYNC_LEN;
  vop_cfg.hfront_porch = KICKPI_K7_HFRONT_PORCH;
  vop_cfg.hback_porch = KICKPI_K7_HBACK_PORCH;
  vop_cfg.vsync_len = KICKPI_K7_VSYNC_LEN;
  vop_cfg.vfront_porch = KICKPI_K7_VFRONT_PORCH;
  vop_cfg.vback_porch = KICKPI_K7_VBACK_PORCH;

  /* Same pixel clock the DSI is programmed with.  This MUST be the exactly
   * achievable value (see KICKPI_K7_MIPI_DSI_PIXCLK): the DSI derives its IPI
   * timing and PHY_IPI_RATIO from it, so a request the CRU only approximates
   * would leave the controller timing its pixel path against a clock the
   * stream does not run at.  rk3576_vop_enable_clocks() logs a warning if the
   * achieved rate still differs. */

  vop_cfg.pixel_clock = KICKPI_K7_MIPI_DSI_PIXCLK;

  ret = rk3576_vop_initialize(&vop_cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_vop_initialize failed: %d\n", ret);
      return ret;
    }

  /* Paint ONE SOLID COLOUR here, before the DSI is asked to send anything.
   *
   * Solid, and painted BEFORE video mode starts, on purpose: the first frames
   * the panel ever receives are then unmistakably a single colour, which is
   * what makes the capture test below readable.  A pattern (the eight bars
   * used previously) cannot distinguish "the panel is showing our first
   * frames, garbled" from "the panel is showing its own noise" -- both look
   * like stripes, and one of the earlier reports was literally "black and
   * white stripes" while the framebuffer held eight black-and-white-ish
   * bars.  See KICKPI_K7_CAPTURE_TEST. */

  rk3576_vop_fill(0xff00ff);

  syslog(LOG_WARNING,
         "kickpi-k7: pre-video frame = MAGENTA (0xff00ff) -- the panel's "
         "first frames should be exactly this colour\n");

  /* Prove the value really is in the framebuffer; without this, "the panel
   * does not show it" has two meanings and the second one is invisible. */

  {
    uint32_t a = 0;
    uint32_t b = 0;

    if (rk3576_vop_peek_pixel(10, 10, &a) == OK &&
        rk3576_vop_peek_pixel(700, 1200, &b) == OK)
      {
        syslog(LOG_INFO,
               "kickpi-k7: fb check %06x %06x (expect ff00ff ff00ff)\n",
               (unsigned)a, (unsigned)b);
      }
    else
      {
        syslog(LOG_ERR,
               "kickpi-k7: fb check FAILED -- the framebuffer is not "
               "readable; any 'no picture' result is meaningless\n");
      }
  }

  /* 5. Now program the DSI video timing and switch to Video mode -- with the
   *    pixel clock already running, per step 4. */

  ret = rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_mipi_dsi_enable_video failed: %d\n", ret);
      return ret;
    }

  /* 5a. *** THE REFERENCE DIFF. ***
   *
   *     Dump the whole VOP register space HERE -- after the video path is up,
   *     and before any probe has touched a register -- in exactly the format of
   *     the dump taken from this board running the Debian image with HDMI
   *     working.  That dump is a real, on-silicon configuration that fetches and
   *     displays; every register that differs from ours is then a candidate
   *     backed by evidence instead of by inference from the TRM.
   *
   *     Extract it from the console log with:
   *
   *       sed -n '/VOPDUMP START/,/VOPDUMP END/p' log.txt \
   *           | grep -E '^[0-9a-f]{4} ' > nuttx-vop.txt
   *
   *     and then:  diff debian-vop.txt nuttx-vop.txt */

  /* Let a couple of frames go by first.  Many of these registers are mirrors
   * whose values only reach the real register at a frame boundary, and a dump
   * taken too early would report the mirror's pre-load contents and invent
   * differences that do not exist in the steady state.  At ~62.5 MHz with
   * htotal 780 and vtotal 1320 a frame is ~16.5 ms, so 100 ms is six frames. */

  up_mdelay(100);

  rk3576_vop_full_dump();

  /* 4b. *** FIRST SETTLE THE ASSUMPTION EVERY OTHER TEST MAKES: DOES A REGISTER
   *     WRITE TO THE POST REACH THE GLASS AT ALL? ***
   *
   *     It stopped being safe to assume when it turned out that the probes'
   *     "solid blue background" was really programmed as SOLID GREEN -- this
   *     header had the background's blue and green fields swapped, and the
   *     value 0x3ff went through the blue shift into the hardware's green field
   *     -- while a BLUE screen was reported anyway.
   *
   *     So the layer is turned off and the background is stepped through five
   *     unmistakable colours, with the order revealed only afterwards, so the
   *     log cannot supply the answer.  See
   *     rk3576_vop_post_bg_sequence_probe(). */

  /* *** BOTH VISUAL PROBES MOVED TO STEP 6a, AFTER THE BACKLIGHT IS ON. ***
   *
   * They were called here and they showed nothing -- because the panel is dark
   * at this point.  The ONLY backlight enable in the whole boot path is the one
   * in step 6; the other apparent call site sits inside
   * `#if KICKPI_K7_BIST_TEST`, which is 0, so it does not exist.
   *
   * A probe that changes what the panel would be showing while the panel is
   * dark is not a weak test, it is NO test -- and worse, it is indistinguish-
   * able from a test that ran and failed: "nothing happened" and "nobody could
   * see" produce the same report.  Anything in this file that is meant to be
   * LOOKED AT belongs after step 6. */

  /* State the rate relationship on every boot.
   *
   * ONLY MEANINGFUL IN A NON-BURST MODE, and this has to be said explicitly
   * because the first version of this line was wrong.  In a NON-burst mode the
   * payload occupies the active window 1:1, so the link rate IS the pixel rate
   * times bpp/lanes -- hs_rate * lanes / bpp = 375.16 M * 4 / 24 = 62.5 Mpx/s,
   * exactly the clock the VOP delivers.  Any difference there is a real fault:
   * the panel would be told to expect a different rate from the one it gets.
   *
   * In BURST mode that identity does NOT hold and must not be "checked": the
   * link deliberately runs 10/9 FASTER than the pixel rate so the payload can
   * be time-compressed into a packet per line, so the same arithmetic yields
   * 69.5 Mpx/s against the delivered 62.5 and would report a permanent -9800
   * ppm "error" on a perfectly correct configuration.  A burst panel cannot
   * recover the pixel clock from the link at all; it takes line timing from the
   * sync packets, which is exactly why the mode exists.  So in burst mode the
   * line reports the numbers and says so, instead of raising a false alarm. */

  {
    uint32_t implied_mpx10 =
        (uint32_t)(((uint64_t)KICKPI_K7_MIPI_DSI_HS_RATE / 1000000ull) *
                   KICKPI_K7_MIPI_DSI_LANES * 10ull / 24ull);
    uint32_t ours_mpx10 =
        (uint32_t)(((uint64_t)KICKPI_K7_MIPI_DSI_PIXCLK / 100000ull));
    int32_t ppm = implied_mpx10 != 0
                      ? (int32_t)(((int64_t)ours_mpx10 -
                                   (int64_t)implied_mpx10) *
                                  100000 / (int64_t)implied_mpx10)
                      : 0;
    bool burst = (KICKPI_K7_DSI_VID_MODE == KICKPI_K7_DSI_VID_MODE_BURST);

    /* Units are 0.1 Mpx/s so that 62.5 does not truncate to 62. */

    syslog(LOG_WARNING,
           "kickpi-k7: rate context: link %u Mbps x %u lanes / 24 bpp gives "
           "%u.%u Mpx/s vs VOP %u.%u Mpx/s (%d ppm), mode=%s\n",
           (unsigned)(KICKPI_K7_MIPI_DSI_HS_RATE / 1000000u),
           (unsigned)KICKPI_K7_MIPI_DSI_LANES,
           (unsigned)(implied_mpx10 / 10u), (unsigned)(implied_mpx10 % 10u),
           (unsigned)(ours_mpx10 / 10u), (unsigned)(ours_mpx10 % 10u),
           (int)ppm, burst ? "burst" : "non-burst");

    if (burst)
      {
        syslog(LOG_WARNING,
               "kickpi-k7:   burst runs the link 10/9 faster on purpose, so "
               "this ratio is EXPECTED to differ; the panel takes line timing "
               "from the sync packets, not from the link rate.  No verdict is "
               "drawn from it here\n");
      }
    else if (ppm <= -10000 || ppm >= 10000)
      {
        syslog(LOG_WARNING,
               "kickpi-k7:   in a NON-burst mode this ratio must be 0: the link "
               "rate IS the pixel rate x bpp/lanes, so a difference means the "
               "panel and the host disagree about how long a line lasts\n");
      }
  }

  /* 6. Backlight on BEFORE the health watch, so the panel is visible while
   *    the stream is being observed -- otherwise a stream that dies during
   *    the watch window cannot be correlated with what the user sees. */

  kickpi_k7_mipi_dsi_backlight_enable();

  /* 6a. *** NOW THE PANEL IS VISIBLE, SO THE SCREEN CAN BE ASKED QUESTIONS. ***
   *
   *     Both of these were called back in step 4, before the backlight existed,
   *     and neither could be seen -- the panel was dark.  A register change the
   *     reader cannot see is not evidence.  They run here instead, and they run
   *     BEFORE the health watch so the watch's period is unaffected. */

#if KICKPI_K7_VOP_PROBES

  (void)rk3576_vop_post_bg_sequence_probe();
  (void)rk3576_vop_coarse_pattern_probe();

  /* 6b. And immediately, while those bars are still in the framebuffer and the
   *     oracle is known to work, toggle the driver's three unsupported AXI
   *     deviations one at a time.  If the fetch starts working at any of them,
   *     the bars appear and that rung has identified itself.  See
   *     rk3576_vop_layer_axi_probe(). */

  (void)rk3576_vop_layer_axi_probe();

#endif /* KICKPI_K7_VOP_PROBES */

  /* 7. Observe the state the BOOT path produced (step 4 entered video mode
   *    before the VOP existed, step 5 then started the pixel clock).
   *
   *    Then repeat the cycle "read the panel for the period that just ended,
   *    force a full rearm, observe again" twice.  This is deliberately not a
   *    one-shot timeline any more, because a previous boot produced two
   *    COMPLETELY different states with byte-identical registers: at boot the
   *    data lanes were out of LP-11 in 1/16 samples with the IPI input FIFO
   *    saturated, and after a rearm they were out of LP-11 in 15/16 with the
   *    FIFO drained.  Getting both states into one log, each with its own
   *    panel error tally, removes the cross-boot variance that has misled
   *    this project repeatedly:
   *
   *      - if the same state comes back every time, the difference is caused
   *        by WHEN video mode is entered (boot order), and the fix is an
   *        ordering change;
   *      - if the state keeps flipping, it is a race, and the pipeline state
   *        at entry is what has to be controlled.
   *
   *    Whichever it is, the panel read of each period says whether that
   *    period's traffic reached the panel intact.
   */

  kickpi_k7_mipi_dsi_observe("boot", KICKPI_K7_HEALTH_SAMPLES, true);

  /* 7b. Read the panel the boot phase left behind.  Everything streamed so
   *     far (the first stream entry, 3 s of running, the vop-off gate) is
   *     covered by this one read, so its error count is the BOOT PHASE's --
   *     and it is the reference the next read is compared against. */

  kickpi_k7_mipi_dsi_panel_probe(host, dev, "after-boot");

  /* 7b-bis. Two short lines that say whether the video payload is actually
   *         MOVING: the two ladders distinguish a running IPI video path from
   *         one that merely looks busy, and the timeout counter turns the
   *         health log's per-window boolean into a rate.  Read-only. */

  kickpi_k7_mipi_dsi_payload_probe();

#if KICKPI_K7_CAPTURE_TEST
  /* 7b. Change the whole screen three times and announce each, so the
   *     observer can say whether the FIRST frames (painted solid MAGENTA
   *     before video mode started) reached the glass and whether anything
   *     after them did.  See kickpi_k7_mipi_dsi_capture_test(). */

  kickpi_k7_mipi_dsi_capture_test();
#endif

  /* 7b-bis.  Before interpreting ANY content result, establish that the bytes
   *     we paint actually reach DRAM.  Every previous "fb check ... OK" line
   *     was answered by the CPU cache and therefore proved nothing about what
   *     the ESMART -- which reads DRAM with its MMU bypassed -- actually sees.
   *     A mismatch here invalidates every content measurement in this project,
   *     so it is printed before them rather than after.  See
   *     rk3576_vop_dram_witness(). */

  (void)rk3576_vop_dram_witness();

  /* 7b-quinquies.  And look at what we have NOT been looking at: every
   *     non-zero register in the ESMART0 and SYS_CTRL blocks.  This board boots
   *     through a vendor bootloader that drives the same VOP for a boot logo,
   *     so the hardware does not start from reset and anything left behind that
   *     this driver never writes is inherited silently.  Read-only. */

  (void)rk3576_vop_scan_regs();

  /* 7b-sexies.  Then settle the one question every measurement so far has
   *     guessed at: does the layer issue AXI reads AT ALL?  axi0_mmu_idle
   *     cannot say (this driver bypasses the MMU, so it may read idle either
   *     way).  Pointing the fetch at an undecodable address can: a read that is
   *     issued must come back as a bus error, and a layer that issues nothing
   *     cannot produce one.  See rk3576_vop_request_probe(). */

  /* *** THE STATE-CHANGING DIAGNOSTIC SEQUENCE, BEHIND THE SAME SWITCH. ***
   *
   * It used to run on every boot.  Now that the cause of the garbage is known
   * and corrected, leaving it on would only make the boot end with whatever the
   * last probe happened to put on the screen -- and cost seconds of boot time -
   * so it is off by default.  The read-only observations above stay on: they
   * cost nothing and they are the evidence that would show a regression. */

#if KICKPI_K7_VOP_PROBES

  (void)rk3576_vop_request_probe();

  /* 7b-septies.  And ask the same question a second, independent way: the
   *     poison test needs a bus error to be generated AND latched, and every
   *     read of it has been zero, which does not distinguish "the layer issues
   *     no reads" from "the error never reaches that register".  Turning the
   *     VOP MMU's paging on with no page table makes EVERY access fail and
   *     records which master did it, so it cannot be fooled the same way.  See
   *     rk3576_vop_mmu_fault_probe(). */

  (void)rk3576_vop_mmu_fault_probe();

  /* 7b-octies.  The one configuration that lives OUTSIDE the VOP register block
   *     and that no scan here has ever looked at: the SoC security setting.  The
   *     TRM's security chapter names esmart0 by hand -- "The security layer
   *     esmart0 is forced to vop_sec_port", and a security layer on a
   *     non-secure port is "forcibly closed".  If a boot stage left that on, the
   *     window would read back perfectly configured and still issue no accesses,
   *     which is exactly what two independent probes have now measured.
   *
   *     MEASURED, AND IT PANICKED THE KERNEL: a non-secure read of SYS_SGRF
   *     (0x26004000) takes a synchronous external abort, and the very first
   *     access is what faulted -- the previous round's log ends right here with
   *     "Assertion failed panic ... arm64_fatal.c:571".  The address is decoded
   *     (SYS_GRF next door at 0x2600A000 is written successfully by the WDT and
   *     audio drivers) and the page is mapped (DEVICE_REGION = 0x20000000 +
   *     256 MB), so this is a bus refusal, not a decode or translation problem.
   *     The security question is therefore UNTESTABLE from the non-secure world,
   *     by read or by write, and the probe is now a no-op that says so.  See
   *     rk3576_vop_security_probe(). */

  (void)rk3576_vop_security_probe();

  /* 7b-nonies.  Before trusting ANY of the above, verify the assumption every
   *     one of them rests on and none has ever tested: that the mirror -> real
   *     register load for the WINDOW group actually completes.  All layer
   *     configuration goes to mirror registers and only reaches the real ones
   *     at a frame boundary, and a readback returns the MIRROR either way.  If
   *     the window group never loads, ESMART0 is disabled in silicon while
   *     reading back enabled -- which would explain, all at once, "no memory
   *     accesses", "every register reads back correct", and the just-measured
   *     "changing ESMART0's configuration changes nothing on the glass".  See
   *     rk3576_vop_cfg_done_probe(). */

  (void)rk3576_vop_cfg_done_probe();

  /* 7b-decies.  Everything so far argues that the layer issues no reads, but
   *     only ever from the ABSENCE of a signal -- no bus error, no MMU fault.
   *     The SYS0/SYS1 interrupt latches allow the opposite, positive test, and
   *     they have been reporting dma_finish on BOTH AXI channels in every log.
   *     Clear them, wait two frames, and see whether they come back.  See
   *     rk3576_vop_dma_liveness_probe(). */

  (void)rk3576_vop_dma_liveness_probe();

  /* 7b-undecies.  The decisive, confound-free test of the window group.  The
   *     rung that looked like proof that the layer's own enable bit works also
   *     turned the POST background on, so it proved nothing; and "changing the
   *     ESMART block changes nothing" is consistent with the real window
   *     registers still holding the bootloader's values -- which would show a
   *     static, content-independent picture at a valid DRAM address, raising
   *     no bus error and no MMU fault.  Freeze the background blue, then change
   *     one window register per rung.  See rk3576_vop_winload_probe(). */

  (void)rk3576_vop_winload_probe();

  /* 7b-duodecies.  Every log so far shows the driver's write to
   *     ESMART_AXI_CTRL_IMD not surviving: mmu_bypass, outstanding_en and
   *     outstanding_num all read back zero.  Writing axi_sel was previously
   *     blamed, but the same loss happens with axi_sel clear, so the register's
   *     write format has simply never been established.  This matters more than
   *     anything else outstanding: the driver programs PHYSICAL addresses, and a
   *     window whose MMU bypass is not really set hands them to an MMU with no
   *     page tables -- a read that never returns, no data and no bus error,
   *     which is exactly how this board behaves.  Five candidate encodings are
   *     tried and read back, and the SYS-level bypass is verified too.  See
   *     rk3576_vop_axi_ctrl_probe(). */

  (void)rk3576_vop_axi_ctrl_probe();

  /* 7b-terdecies.  Every static register is now verified against the reference
   *     driver and the TRM, so stop setting values and try STARTING the layer:
   *     a 0->1 edge on the region enable, then the TRM's per-vsync internal-logic
   *     frame reset (ESMART_CTRL0 bit31), which is described as resetting the
   *     layer's internal logic while excluding the register logic -- exactly this
   *     board's symptom.  With the background frozen blue, that rung predicts a
   *     blue screen, so it is falsifiable rather than another guess.  See
   *     rk3576_vop_win_start_probe(). */

  (void)rk3576_vop_win_start_probe();

  /* 7b-quattuordecies.  The one mechanism that accounts for every observation at
   *     once, and the one that had never been measured: is the VOP's DATAPATH
   *     clock running?  The register file is clocked by hclk/pclk, so a stopped
   *     aclk answers every read perfectly, retains every write, and still never
   *     issues a memory access -- and raises no error, because nothing happens.
   *     POST_CLK_CNT counts aclk and dclk over a fixed 5000-hclk window, with
   *     dclk as a built-in control (the POST timing generator demonstrably runs,
   *     so dclk must count).  See rk3576_vop_clk_alive_probe(). */

  (void)rk3576_vop_clk_alive_probe();

  /* 7b-quindecies.  The clocks are measured healthy, so the last unexplained
   *     thing is a pair of per-port STATUS bits this driver has never read:
   *     dma_stop_valid and mmu_idle in SYS_STATUS0/1/2.  SYS_AXI0_CTRL_IMD holds
   *     a CONTROL that reads back clear; dma_stop_valid reports what the channel
   *     is ACTUALLY doing, and a genuinely stopped channel issues no read and
   *     raises no error -- exactly the combination measured, with the mixer
   *     still waiting for the layer.  The VP0 vertical counter is sampled at the
   *     same time to confirm the video port is scanning at all.  See
   *     rk3576_vop_scan_status_probe(). */

  (void)rk3576_vop_scan_status_probe();

  /* 7b-sexdecies.  The bisection.  Every property of the ESMART0 window is now
   *     verified against the TRM and the reference driver, the clock is measured
   *     alive, the video port is measured scanning, the channel is measured not
   *     stopped and the MMU test now runs with its preconditions established --
   *     and the window still issues no read.  CLUSTER0 reaches the SAME video
   *     port with its OWN register block and DMA but shares the overlay request
   *     path, the AXI ports and the clocks.  Putting it on layer0 therefore
   *     decides between "the ESMART block is broken" and "the shared request
   *     path is broken".  The ESMART configuration is restored afterwards.  See
   *     rk3576_vop_cluster_probe(). */

  (void)rk3576_vop_cluster_probe();

  /* 7b-septendecies.  CLUSTER0 -- a completely independent window block, DMA
   *     and AXI read-ids, correctly configured -- also fetched nothing, so the
   *     fault is in what all windows share.  SYS_PORT_CTRL_IMD has per-video-port
   *     DMA stop switches, and this driver writes that register through a mask
   *     that does NOT include bit8 (vfp0_dma_stop_en), so it has never been
   *     examined.  A stopped video port fetches from no window and raises no
   *     error, which is exactly the evidence.  See
   *     rk3576_vop_port_dma_stop_probe(). */

  (void)rk3576_vop_port_dma_stop_probe();

  /* 7b-octodecies.  Stop arguing from registers.  Every "the layer issues no
   *     reads" conclusion in this project rests on a chain of register
   *     assumptions, and this project has been misled by such chains repeatedly
   *     (a hiword write mask swallowing writes, a readback taken before the
   *     write landed, a status field mistaken for a counter).  This probe points
   *     the fetch at four wildly different addresses and asks only whether the
   *     PICTURE changes -- CHANGES means the window is reading memory and every
   *     "no read" conclusion is wrong; UNCHANGED proves on the glass that it
   *     reads nothing.  One rung paints the framebuffer solid red first.  See
   *     rk3576_vop_addr_screen_probe(). */

  (void)rk3576_vop_addr_screen_probe();

  /* 7b-novendecies.  THE DIFF.  A register dump was taken from this board
   *     running a Debian image with HDMI working -- a VOP configuration KNOWN to
   *     fetch and display.  Three things it has that this driver never writes
   *     are applied here one rung at a time, with the screen watched after each:
   *     the overlay's layer-select regdone-immediate bit (which the vendor
   *     documents as what makes the overlay's layer selection, and therefore the
   *     window's configuration, take effect at all), the scaler filter modes the
   *     vendor programs even at 1:1, and the overlay mixer coefficients.  See
   *     rk3576_vop_ref_align_probe(). */

  (void)rk3576_vop_ref_align_probe();

  /* 7b-ter.  Then ask where the pixels on the glass actually come from, which
   *     no previous probe ever measured.  This dumps the mixer / cfg_done
   *     state and then walks the source ladder, starting with pixels the POST
   *     generates internally -- no layer, no memory, no framebuffer.  See
   *     rk3576_vop_source_probe(). */

  (void)rk3576_vop_source_probe();

  /* 7b-quater.  The source probe surfaced POST_BUF_EMPTY (per-VP bit4): the
   *     POST's output buffer under-ran, i.e. the pixels did not arrive in
   *     time.  That is the only mechanism found in this whole bring-up that
   *     explains a picture following neither content nor address, and it was
   *     being dismissed as a per-line event.  Measure whether it recurs, and
   *     whether this driver's own AXI outstanding limits are what starve the
   *     layer.  See rk3576_vop_underrun_probe(). */

  (void)rk3576_vop_underrun_probe();

  /* 7b. And last, verify the one thing the diff actually FIXED.
   *
   *     The comparison against a working register dump found exactly three bits
   *     that differ in SYS_PORT_CTRL_IMD -- and all three are bits this driver
   *     had changed by reading the vendor source rather than by observing
   *     working silicon: dsp_vs_t_sel (cleared, because is_vop3() is true for
   *     RK3576, yet working silicon holds its reset value 1), auto_cs_mode (set,
   *     though RK3576's control table has no field for it), and reg_done_frm
   *     (never written at all, so it sat at its reset value 1; Linux writes it
   *     to 0).  configure_port() now writes only the one field that needs
   *     changing, which reproduces the working word 0x00070038 exactly.
   *
   *     Several probes that follow that point write this same register
   *     (port_dma_stop_probe) and none checked it afterwards, so the question
   *     worth asking at the END of the boot is whether the corrected value
   *     survived to the moment the display is live.  See
   *     rk3576_vop_port_ctrl_align_probe(). */

  (void)rk3576_vop_port_ctrl_align_probe();

  /* 7c. And now the scaler, which is the stage between the window's DMA and its
   *     output -- the stage that decides when to ask the DMA for data.  Its two
   *     registers were BOTH wrong in opposite directions: this driver wrote
   *     SCL_CTRL with only bit20 set (clobbering the filter modes the working
   *     configuration has), and never wrote SCL_FACTOR_YRGB at all (leaving the
   *     reset 1.0 where the vendor writes 0 for a 1:1 window).  That rung, plus
   *     the three remaining ESMART0 differences, are walked here.  See
   *     rk3576_vop_scl_align_probe(). */

  (void)rk3576_vop_scl_align_probe();

  /* 7d. And LAST, the experiment that was decisive before -- run again now
   *     that the commit asks only for VP0's register groups.  The earlier run
   *     shrank the layer to 200x200, no corner appeared, and the conclusion
   *     was that the window register group NEVER LOADS.  That run's commit
   *     also requested VP1's and VP2's groups, which nothing on this board can
   *     ever consume; the TRM says these bits gate the mirror->real copy on all
   *     the requested groups finishing.  See
   *     rk3576_vop_window_group_probe(). */

  (void)rk3576_vop_window_group_probe();

  /* 7e. And the A/B that the register diff made possible: the POST's OUTPUT
   *     scale factor held 0 -- a scale-by-zero -- where Linux writes 1.0 at
   *     1:1 and a working dump on this board reads 0x10001000.  That block is
   *     the last one before the DSI, and a uniform image is invariant under any
   *     scaling, which is why every solid-colour test passed while real content
   *     never did.  See rk3576_vop_post_scl_probe(). */

  (void)rk3576_vop_post_scl_probe();

#endif /* KICKPI_K7_VOP_PROBES */

#if KICKPI_K7_PIXEL_PROBE
  /* 7c. Then ask the one question the picture can still answer: do the BYTES
   *     of our framebuffer reach the glass at all?  Nothing is stopped,
   *     re-timed or re-programmed here -- the probe only writes the
   *     framebuffer -- precisely because an earlier round destroyed its own
   *     measurement by re-programming a panel that was already lit.  See
   *     kickpi_k7_mipi_dsi_pixel_probe(). */

  kickpi_k7_mipi_dsi_pixel_probe();

  /* The read that follows covers the whole probe.  It is only meaningful
   * because the probe itself does not disturb the stream. */

  kickpi_k7_mipi_dsi_panel_probe(host, dev, "after-pixel-probe");
#endif

#if KICKPI_K7_LANE_SWEEP
  /* 7d. Then try the one register class never touched: the panel's MIPI lane
   *     sequence / polarity / lane-count selection.  A mismatched lane order
   *     scrambles every 4-byte group, which is exactly the signature observed
   *     (clean link, zero errors, garble that ignores solid colours).  Each
   *     state is announced with its own label and paints whole-screen RED. */

  kickpi_k7_mipi_dsi_lane_sweep(host, dev);

  kickpi_k7_mipi_dsi_panel_probe(host, dev, "after-lane-sweep");
#endif
#if KICKPI_K7_DSI_LINK_MATRIX
  /* 8. Then sweep the (video mode x clock lane type) combinations, each with
   *    its own colour, fingerprint and panel error tally.  See
   *    kickpi_k7_mipi_dsi_link_matrix() for why this is rerun now and why the
   *    pixel clock is kept running throughout. */

  kickpi_k7_mipi_dsi_link_matrix(host, dev);
#else
  /* End on the eight-bar pattern: whatever the panel is showing, the operator
   * needs an image whose intended shape can be named (how many bars, vertical
   * or slanted, is the top band present) -- a solid colour cannot be described
   * wrongly or rightly in those terms. */

  rk3576_vop_paint_test_pattern(0xffffff);

  syslog(LOG_WARNING,
         "kickpi-k7: framebuffer now holds: 8 vertical bars (white/yellow/"
         "cyan/green/magenta/red/blue/black) with a white band on top\n");
  syslog(LOG_WARNING,
         "kickpi-k7: DESCRIBE IT: how many bars, vertical or slanted, is the "
         "top band there, is it static\n");
#endif

  /* Deliberately NOT "bring-up OK": the software sequence completing says
   * nothing about whether the panel displays our pixels, and a log line that
   * asserts success anyway has already sent one round of this analysis in the
   * wrong direction.  What the picture is, is what the pixel probe reported. */

  syslog(LOG_INFO,
         "kickpi-k7: DSI bring-up sequence complete -- judge the picture from "
         "the PIXEL PROBE observation, not from this line\n");

  /* 9. LAST, and destructive: ask whether the panel latches a frame on each
   *    stream ENTRY.  This is the only positive panel response this project has
   *    ever observed (a stream re-entry made it show a frame, which then
   *    decayed), so it gets its own controlled experiment.  See
   *    kickpi_k7_mipi_dsi_entry_probe().  Runs after every read-only probe has
   *    reported, so nothing that matters is lost if it leaves the stream dead.
   *
   *    The auto-timing probe that used to run here is disabled: it was measured
   *    to stop the data lanes outright and answers no question (see
   *    KICKPI_K7_AUTO_PROBE). */

#if KICKPI_K7_MODE_EOTP_MATRIX
  kickpi_k7_mipi_dsi_mode_eotp_matrix(host);
#endif

#if KICKPI_K7_STROBE_TEST
  kickpi_k7_mipi_dsi_strobe_test();
#endif

#if KICKPI_K7_ADDR_ECHO_TEST
  kickpi_k7_mipi_dsi_address_echo_test();
#endif

#if KICKPI_K7_ENTRY_PROBE
  kickpi_k7_mipi_dsi_entry_probe(host);
#endif

#if KICKPI_K7_AUTO_PROBE
  kickpi_k7_mipi_dsi_auto_probe();
#endif
  return OK;
}

#endif /* CONFIG_KICKPI_K7_MIPI_DSI */
