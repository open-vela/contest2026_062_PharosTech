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
 * NOTE: this board code supports ONE panel model -- the WKS50HD072-WCT --
 *whose DCS init sequence and video-mode timing are hard-coded below.  A
 *different panel needs its data sheet consulted and the timing / init sequence
 *re-tuned; this is not a drop-in match for arbitrary DSI panels.
 *
 * WKS50HD072-WCT:
 *   - 720 x 1280 portrait, RGB888, 4 data lanes, 62 MHz pixel clock
 *   - Timing (vendor-supplied; identical to the mainline 5" ILI9881D modes
 *     bananapi,lhr050h41 and startek,kd050hdfia020):
 *             HFP=10 HSYNC=20 HBP=30 (htotal=780),
 *             VFP=10 VSYNC=10 VBP=20 (vtotal=1320)  -> ~60 Hz
 *   - 780 and 1320 are both 4-pixel aligned, which the RK3576 DSI-2 TRM 18.2.1
 *     requires for HTOTAL/HACTIVE/HSYNC/HBP/HFP.
 *
 * Bring-up sequence (the chip drivers own the DCPHY, so board code never
 * touches the PHY directly):
 *   1. rk3576_mipi_dsi_initialize()          -- DSI-2 host; brings up the
 *                                               DCPHY and enters Command mode
 *   2. mipi_dsi_host_register / device_register
 *   3. kickpi_k7_mipi_dsi_configure_pins() + panel_bringup()
 *                                            -- power/reset cycle, DCS init
 *                                               sequence, DISPON verification
 *   4. kickpi_k7_mipi_dsi_bist_clear()       -- the panel must not free-run
 *                                               its own test pattern
 *   5. rk3576_vop_initialize()               -- register /dev/fbN; the pixel
 *                                               clock must be live BEFORE
 *video mode is entered
 *   6. rk3576_mipi_dsi_enable_video()        -- program IPI timing, then
 *                                               transition to Video mode
 *   7. kickpi_k7_mipi_dsi_backlight_enable() -- backlight on
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

/* Video transmission mode.  BURST, because this VOP/IPI combination does not
 * deliver usable per-line sync to the IPI: with a non-burst mode the IPI
 * accepts pixels but its state machine never observes a line boundary, so no
 * packet is ever built.  BURST does not depend on those signals.
 *
 * mode_type is reported on the console by enable_video(), so every captured
 * log states which one was actually programmed. */

#define KICKPI_K7_DSI_VID_MODE KICKPI_K7_DSI_VID_MODE_BURST

/* Clock-lane behaviour.  The clock lane returns to LP-11 after each HS burst,
 * which is what the panel's own datasheet documents (THS-EXIT) and what the
 * mainline peers with identical timing require; a continuous clock lane also
 * leaves the IPI pixel FIFO empty on this board, i.e. pixels never reach the
 * link. */

#define KICKPI_K7_DSI_CONTINUOUS_CLK false

/* EoTp off, matching every reference for this panel IC and this SoC: Linux's
 * MIPI_DSI_MODE_EOT_PACKET means "disable EoT packets in HS mode", and
 * Rockchip's own ILI9881D DTS node sets it while every RK3576 reference panel
 * node sets MIPI_DSI_MODE_NO_EOT_PACKET. */

#define KICKPI_K7_DSI_DEFAULT_EOTP false

/* Panel geometry and link configuration. */
#define KICKPI_K7_MIPI_DSI_XRES   720
#define KICKPI_K7_MIPI_DSI_YRES   1280
#define KICKPI_K7_MIPI_DSI_LANES  4
#define KICKPI_K7_MIPI_DSI_FORMAT MIPI_DSI_FMT_RGB888

/* MADCTL (36h) argument bits.  ILI9881D 5.3.20 defines the byte as
 * "0 0 0 0 BGR 0 SS GS" with a reset default of 00h, so the vendor table's
 * 0x03 sets only SS and GS and leaves BGR = 0.  Unlike a generic MIPI DCS
 * MADCTL there is no MY/MX/MV: D7..D4 are reserved on this panel, so SS and
 * GS are the ONLY orientation controls.
 *
 * BGR (bit3) is asserted because the panel shows red and blue exchanged
 * without it.  This bit is one end of a single R/B swap in the pipeline:
 * ESMART's REGION0_MST_CTL rb_swap (bit14, set in rk3576_vop.c) is the other.
 * Exactly ONE of the two may be active -- setting both cancels out and the
 * inversion returns.  The DSI link cannot absorb the difference either:
 * MIPI_DSI_FMT_RGB888 names the byte order of the pixel stream (R,G,B) and
 * the DSI-2 host has no colour-order field.
 *
 * SS and GS are scan-direction bits, not just sequencing flags: SS reverses
 * the source (column) scan order and GS reverses the gate (row) scan order,
 * so each one mirrors one axis of the picture on the glass.  The vendor's
 * 0x03 leaves both set; the board's enclosure mounts the panel the other way
 * up, so both are cleared below, which mirrors both axes and is therefore a
 * 180-degree rotation of the whole picture.  Note that this is a property of
 * the panel alone: it costs no scan-out bandwidth and needs no change to the
 * VOP timing or to the framebuffer geometry.
 *
 * If the result ever comes out mirrored on a single axis instead of rotated,
 * that means the reference orientation was the other one -- set exactly one of
 * the two bits (GS alone or SS alone) to mirror the remaining axis.
 */

#define KICKPI_K7_MADCTL_GS (1u << 0) /* Gate scan sequence: 1 = reversed */
#define KICKPI_K7_MADCTL_SS                                                  \
  (1u << 1)                            /* Source scan sequence: 1 = reversed \
                                        */
#define KICKPI_K7_MADCTL_BGR (1u << 3) /* 0: RGB, 1: BGR */

/* Pixel clock: the vendor device tree specifies 62000000 Hz, and 62526316 Hz
 * is the closest rate this board can actually produce --
 * clk_set_rate(dclk_vp0) keeps the largest divisor of gpll whose output is
 * still <= the request, and 1188/19 = 62.5263 MHz is above 62.000 MHz, so
 * requesting exactly 62 MHz silently lands on divisor 20 and loses 4.2%:
 *
 *   request 62000000 -> 1188/20 = 59.400000 (4.2% low, what this board ran)
 *   request 62526316 -> 1188/19 = 62.526316 (EXACT)      <- now
 *
 * A panel with no frame memory has no clock of its own: it recovers one from
 * the link and times its TCON from that.  The datasheet states the frame rate
 * in terms of the link bit rate, lanes and pixel format, i.e. the rate the
 * link implies IS the rate the panel believes it is receiving -- so a
 * link/host disagreement is a permanent framing error, not a refresh-rate nit.
 */

#define KICKPI_K7_MIPI_DSI_PIXCLK 62526316u

/* Link rate implied by the pixel stream, derived from the pixel clock rather
 * than chosen for roundness: 62526316 Hz * 24 bpp / 4 lanes = 375157896 bps
 * per lane.  Burst mode then takes 10/9 of it, exactly as the reference driver
 * does, so a burst build runs at ~416.8 Mbps. */

#define KICKPI_K7_MIPI_DSI_HS_RATE_RAW 375160000u

/* Burst headroom: burst transmission time-compresses a line's active pixels
 * into one packet sent as fast as possible, so the link must deliver pixels
 * faster than the raw pixel rate or the burst cannot fit inside the line and
 * every horizontal boundary shifts.  The reference driver applies exactly this
 * factor (dw_mipi_dsi2_get_lane_mbps(): "take 1 / 0.9, since Mbps must big
 * than bandwidth of RGB").  Set to 1/1 to run burst at the raw rate.
 *
 * A non-burst stream has no such margin by design -- its payload occupies the
 * active period 1:1 -- which is why the data lanes then stay in high speed
 * across the whole active frame.  That is a property of the mode, not a fault.
 */

#define KICKPI_K7_MIPI_DSI_BURST_HEADROOM_NUM 10u
#define KICKPI_K7_MIPI_DSI_BURST_HEADROOM_DEN 9u

/* The link rate actually handed to the DSI host, derived from the active
 * video mode: burst mode takes the headroom above, the non-burst modes use
 * the raw rate (they preserve the panel timing 1:1, so they need no
 * compression headroom). */

#if KICKPI_K7_DSI_VID_MODE == KICKPI_K7_DSI_VID_MODE_BURST
#define KICKPI_K7_MIPI_DSI_HS_RATE                                          \
  (KICKPI_K7_MIPI_DSI_HS_RATE_RAW * KICKPI_K7_MIPI_DSI_BURST_HEADROOM_NUM / \
   KICKPI_K7_MIPI_DSI_BURST_HEADROOM_DEN)
#else
#define KICKPI_K7_MIPI_DSI_HS_RATE KICKPI_K7_MIPI_DSI_HS_RATE_RAW
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

#define KICKPI_K7_DSI_VC       0 /* Virtual channel */

/* Panel control pins. */

#define KICKPI_K7_MIPI_DSI_RST   (GPIO_PORT0 | GPIO_PIN_A2)
#define KICKPI_K7_MIPI_DSI_PWREN (GPIO_PORT0 | GPIO_PIN_C6)

/* Panel power / reset sequencing timings (ms).  The panel is driven through a
 * real power cycle on every boot: PWREN low long enough for the rails to
 * discharge, then raised with RST still asserted.  Driving PWREN high without
 * ever lowering it gives the panel no power-on reset on a warm reboot, so the
 * panel IC can keep whatever state the previous session left it in and refuse
 * to latch the init sequence.
 *
 * Follows the vendor DT (power-delay-ms = <10>) and mainline
 * ili9881c_prepare(), extended on the release side because this panel's DCS
 * table needs more settle time before its first command.
 */

#define KICKPI_K7_POWER_OFF_MS    30 /* PWREN low: let the rails decay */
#define KICKPI_K7_PWREN_SETTLE_MS 20 /* rails good before reset release */
#define KICKPI_K7_RST_ASSERT_MS   20 /* reset held low */
#define KICKPI_K7_RST_RELEASE_MS             \
  120 /* after release, before first command \
       */

/* How many times to power-cycle the panel and re-send the init sequence when
 * the DCS power-mode read-back shows DISPON never latched. */

#define KICKPI_K7_INIT_RETRIES 3

/* Init-sequence settle times.
 *
 * The vendor's sequence carries `39 10 ...` on every line, i.e. the SAME 16 ms
 * after all 196 commands -- which alone accounts for ~3.1 s of boot time. That
 * is a blanket value, not a per-command requirement: the only command in the
 * table whose failure is SILENT is the manufacturer-page switch
 * (FF 98 81 <page>).  If the panel has not yet re-targeted its page register,
 * the following writes land in the previous page, and the panel then keeps its
 * unprogrammed timing while still answering DCS reads -- so nothing on the
 * host side notices.
 *
 * Everything else is a plain register write, and mipi_dsi_transfer() returns
 * only once the CRI has drained the packet, so there is no host-side reason to
 * pause after them.  Their own delays (the table's 120 ms sleep-out and 20 ms
 * display-on) come from the DCS spec and are applied as written.
 *
 * KICKPI_K7_PAGE_SETTLE_MS therefore applies to page switches only; the table
 * already carries 5 ms for each of them and this raises that to the vendor's
 * value.  Set KICKPI_K7_INIT_CMD_SETTLE_MS to 16 to restore the old
 * pause-after-every-command behaviour for bisection.
 */

#define KICKPI_K7_PAGE_SETTLE_MS     16
#define KICKPI_K7_INIT_CMD_SETTLE_MS 0

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
 * 0x39 is DCS long write: payload[0] is the command, the rest are parameters.
 * The vendor device tree uses it for the WHOLE sequence, including the 2-byte
 * register writes that this table used to send as 0x23 (generic short write,
 * 2 parameters).  Both forms carry the same bytes, but a page write that
 * silently does not latch is invisible from the host -- the panel still
 * answers DCS reads and displays its unprogrammed timing -- so the vendor
 * sequence is reproduced verbatim. */

#define KICKPI_K7_PKT_GEN_LONG 0x39 /* DCS long write (4-byte page select) */
#define KICKPI_K7_PKT_DCS_LONG \
  0x39 /* DCS long write (2-byte register write) */

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
 * MUST settle after the page register is re-targeted before the following
 * register writes can be latched.  With delay_ms = 0 the sequence only worked
 * by accident, because the debug probe that used to follow the first
 * page-switch burned a few ms of UART time; removing that probe made DISPON
 * stop sticking (GET_POWER_MODE read back 0x0C instead of 0x9C).  The table
 * carries an explicit delay on every page switch below AND the code enforces
 * a floor from the payload, so neither can be lost by editing a row. */

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

  /* MADCTL: BGR plus a 180-degree rotation (SS and GS both cleared, where
   * the vendor's 0x03 leaves them set).  See the bit notes above. */

  { KICKPI_K7_PKT_DCS_LONG, 0, _PANEL_INIT(0x36, KICKPI_K7_MADCTL_BGR) },

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

  /* Idempotent: the guards keep a repeated call from registering the same PWM
   * device twice, which would fail and log a line that reads like a fault
   * while the backlight is in fact already running. */

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
      bl_pwm =
          rk3576_pwm_initialize(KICKPI_K7_BL_PWM_CTRL, KICKPI_K7_BL_PWM_CH);
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
 *   with its original packet type via mipi_dsi_transfer(), then the entry's
 *   own delay is applied with up_mdelay() (raised to the page-switch floor
 *   where that applies).
 *
 ****************************************************************************/

/* The panel's manufacturer-page switch is the 4-byte sequence FF 98 81 <page>.
 * Identified from the payload rather than from the entry's delay so that a
 * page switch cannot lose its settle by being written with delay_ms = 0.
 */

static bool kickpi_k7_mipi_dsi_is_page_switch(
    FAR const struct kickpi_k7_mipi_dsi_cmd_s *cmd)
{
  return cmd->len == 4 && cmd->data[0] == 0xff && cmd->data[1] == 0x98 &&
         cmd->data[2] == 0x81;
}

static int
kickpi_k7_mipi_dsi_send_init_sequence(FAR struct mipi_dsi_device *device)
{
  int i;
  int ncmds = nitems(g_kickpi_k7_mipi_dsi_init);

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

      /* Settle after the command: the entry's own delay, raised to the
       * page-switch floor where that applies.  See
       * KICKPI_K7_PAGE_SETTLE_MS for why only page switches get one.
       */

      {
        uint32_t settle = cmd->delay_ms;

        if (kickpi_k7_mipi_dsi_is_page_switch(cmd) &&
            settle < KICKPI_K7_PAGE_SETTLE_MS)
          {
            settle = KICKPI_K7_PAGE_SETTLE_MS;
          }

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

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_panel_bringup
 *
 * Description:
 *   Power-cycle the panel, send the DCS init sequence, then verify that the
 *   panel latched it by reading MIPI_DCS_GET_POWER_MODE (0x0a) back over the
 *   link; a bus-turnaround read, so it only succeeds if the panel is genuinely
 *   alive and answering.  DISPON (0x80) is what the trailing DCS 0x29 in the
 *   init table sets.  On failure, power-cycle the panel and re-send the whole
 *   table -- no host-side state change is needed because the DSI host is still
 *   in Command mode at this point.
 *
 ****************************************************************************/

static int kickpi_k7_mipi_dsi_panel_bringup(FAR struct mipi_dsi_device *dev)
{
  int attempt;

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

          return OK;
        }

      syslog(LOG_WARNING,
             "kickpi-k7: init attempt %d: power mode read %d, value 0x%02x "
             "(DISPON not set) -- power-cycling the panel and retrying\n",
             attempt + 1, (int)n, pwrmode);
    }

  syslog(LOG_ERR, "kickpi-k7: panel never reported DISPON after %d attempts\n",
         KICKPI_K7_INIT_RETRIES + 1);
  return -EIO;
}

/* Defined further down; declared here because bist_clear() needs it. */

static int kickpi_k7_mipi_dsi_write2(FAR struct mipi_dsi_device *dev,
                                     uint8_t reg, uint8_t value);

/****************************************************************************
 * Name: kickpi_k7_mipi_dsi_bist_clear
 *
 * Description:
 *   Make sure the panel is not running its own test pattern.  While BIST is
 *   enabled the panel free-runs a pattern generated from its own registers, so
 *   it ignores the incoming video stream and anything seen on the glass
 *   describes the panel, not this driver.
 *
 *   Clears the pattern selection (0x2c/0x2d/0x2e) and then the enable (0x2f),
 *   and reads the enable back, because an unchecked write is not evidence that
 *   the panel left the mode.
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
  int ret;

  memset(&msg, 0, sizeof(msg));
  msg.channel = KICKPI_K7_DSI_VC;
  msg.type = KICKPI_K7_PKT_GEN_LONG;
  msg.tx_buf = page4;
  msg.tx_len = sizeof(page4);

  if (mipi_dsi_transfer(dev, &msg) < 0)
    {
      syslog(LOG_ERR, "ERROR: panel BIST: page 4 select failed\n");
      return -EIO;
    }

  /* Clear the pattern selection first, then the enable. */

  (void)kickpi_k7_mipi_dsi_write2(dev, 0x2c, 0x00);
  (void)kickpi_k7_mipi_dsi_write2(dev, 0x2d, 0x00);
  (void)kickpi_k7_mipi_dsi_write2(dev, 0x2e, 0x00);

  if (kickpi_k7_mipi_dsi_write2(dev, 0x2f, 0x00) < 0)
    {
      syslog(LOG_ERR, "ERROR: panel BIST enable write failed\n");
    }

  up_udelay(200);

  rb = 0xff;
  ret = mipi_dsi_dcs_read(dev, 0x2f, &rb, 1);

  if (ret != 1 || rb != 0x00)
    {
      syslog(LOG_ERR,
             "ERROR: panel BIST is still enabled (FRM_EN reads 0x%02x): the "
             "panel free-runs\n",
             (unsigned)rb);
      syslog(LOG_ERR,
             "ERROR:   its own test pattern and ignores the video stream\n");
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

  /* Non-continuous clock lane; see KICKPI_K7_DSI_CONTINUOUS_CLK. */

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
   *    that the panel latched it (retrying with another power cycle if not).
   */

  kickpi_k7_mipi_dsi_configure_pins();

  ret = kickpi_k7_mipi_dsi_panel_bringup(dev);
  if (ret < 0)
    {
      return ret;
    }

  /* 3a. Make sure the panel is not free-running its own test pattern: while
   *     BIST is enabled the panel ignores the video stream entirely. */

  (void)kickpi_k7_mipi_dsi_bist_clear(dev);

  /* 4. Bring up the VOP framebuffer that feeds the DSI IPI on PORT0, BEFORE
   *    switching the DSI into video mode -- the order matters.
   *
   *    Entering video mode makes the IPI timing generator count lines out of
   *    ipi_clk (= pixel clock / 4).  Doing that while no pixel clock exists,
   *    and then having the clock appear and be interrupted once more by the
   *    VOP's dclk reset pulse, leaves that generator waiting for a boundary
   *    that never comes: pixels pile up in ipi_data, no line packet is built
   *    and the lanes stay idle.  Linux has the same order -- the CRTC (dclk,
   *    MIPI interface, scan-out) is enabled before the DSI encoder's
   *    atomic_enable() switches the host into video mode.
   *
   *    Command mode is unaffected by the VOP running: the TRM states that
   *    Command mode ignores the IPI interface.
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

  /* Paint a solid colour before video mode starts, so the first frames the
   * panel receives are recognisably this driver's output rather than the
   * panel's own noise.  The application paints the real content later.
   */

  rk3576_vop_fill(0xff00ff);

  /* 5. Now program the DSI video timing and switch to Video mode -- with the
   *    pixel clock already running, per step 4. */

  ret = rk3576_mipi_dsi_enable_video(host, &g_kickpi_k7_mipi_dsi_timing);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_mipi_dsi_enable_video failed: %d\n", ret);
      return ret;
    }

  /* 6. Backlight on.  It must come after the stream is running, or the panel
   * is lit while nothing is being transmitted. */

  kickpi_k7_mipi_dsi_backlight_enable();

  /* Completing the software sequence says nothing about whether the panel
   * displays anything; the framebuffer is registered as /dev/fbN from here on.
   */

  syslog(LOG_INFO, "kickpi-k7: DSI bring-up complete, /dev/fb registered\n");

  return OK;
}

#endif /* CONFIG_KICKPI_K7_MIPI_DSI */
