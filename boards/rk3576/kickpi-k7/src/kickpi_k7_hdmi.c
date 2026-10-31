/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_hdmi.c
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
 * On-board HDMI output for the kickpi-k7 (RK3576).
 *
 * Bring-up order, and why it is this order:
 *
 *   1. rk3576_hdmi_initialize()
 *        Controller APB clock and resets, VO0_GRF routing, the controller's
 *        register file and its built-in DDC master, then the PHY's APB side.
 *        The PHY is left powered OFF, so no pixel clock exists yet.
 *
 *   2. kickpi_k7_hdmi_select_mode()
 *        Read the sink's EDID over DDC and adopt its preferred timing, falling
 *        back to the default 1080p60 mode on any failure.  This sits between
 *        steps 1 and 3 because it needs the DDC master from step 1 and must
 *        finish before step 3 programs the PHY with a pixel clock.
 *
 *   3. rk3576_hdmi_enable(pixel_clock, 8)
 *        Programs the PHY's TMDS PLL for the mode's pixel rate, switches the
 *        controller to DVI mode and releases the video datapath.  THIS is
 *        what puts a pixel clock on the wire -- on the HDMI path the PHY's
 *        PLL is the clock source, not the CRU.
 *
 *   4. kickpi_k7_video_initialize()
 *        Starts the VOP.  It must come after step 3 or the video port has no
 *        clock: rk3576_vop.c reparents dclk_vpN_sel onto clk_hdmiphy_pixel0_o
 *        for the HDMI interface instead of programming a CRU divider.
 *
 * Note the contrast with MIPI DSI, where the VOP comes BEFORE the interface
 * starts transmitting.  Neither ordering is free to change; see the banner in
 * kickpi_k7_video.h.
 *
 * Scope: DVI mode (no infoframes, no audio, no HDCP, no scrambling).
 *OPMODE_DVI is also the controller's reset state, which is what makes this the
 *cheapest path to a lit display.  The mode is chosen from the sink's EDID,
 *bounded by what this path can actually drive; see
 *kickpi_k7_hdmi_select_mode().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "kickpi_k7.h"
#include "kickpi_k7_video.h"
#include "rk3576_gpio.h"
#include "rk3576_hdmi.h"
#include "rk3576_hdptxphy.h"
#include "rk3576_vop.h"

#ifdef CONFIG_KICKPI_K7_HDMI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 1080p60 from CEA-861 (VIC 16): 1920x1080, 148.5 MHz pixel clock, 2200x1125
 * total.  The two totals are the arithmetic check worth doing before changing
 * anything here -- 1920 + 88 + 44 + 148 = 2200 and 1080 + 4 + 5 + 36 = 1125 --
 * because 148500000 / (2200 * 1125) = 60.000 Hz exactly, and any typo in a
 * porch shows up as a non-60 refresh rather than as an obvious error.
 *
 * 148.5 MHz is also one of the rates the PHY's TMDS PLL table synthesises
 * exactly (mdiv 123 -> 2970 MHz VCO, /4 /10 /8 -> 148.5 MHz), so the achieved
 * pixel clock matches the nominal one and the 500 ppm self-check in
 * rk3576_vop.c cannot fire.
 */

#define KICKPI_K7_HDMI_XRES         1920
#define KICKPI_K7_HDMI_YRES         1080
#define KICKPI_K7_HDMI_HSYNC_LEN    44
#define KICKPI_K7_HDMI_HFRONT_PORCH 88
#define KICKPI_K7_HDMI_HBACK_PORCH  148
#define KICKPI_K7_HDMI_VSYNC_LEN    5
#define KICKPI_K7_HDMI_VFRONT_PORCH 4
#define KICKPI_K7_HDMI_VBACK_PORCH  36
#define KICKPI_K7_HDMI_PIXCLK       148500000u

/* Highest pixel clock this path may run at.
 *
 * 165 MHz is DVI 1.0's single-link limit.  Above it a DVI link needs the
 * scrambling HDMI 1.4 introduced, and this driver implements neither
 * scrambling nor HDMI mode at all, so the PHY's own (much higher) capability
 * is not the relevant bound -- the link protocol is.  A sink whose preferred
 * timing exceeds this falls back to 1080p60 rather than being driven outside
 * spec.
 */

#define KICKPI_K7_HDMI_DVI_MAX_PIXCLK 165000000u

/* DVI is 8 bits per colour component, always.  10 bpc only exists on an HDMI
 * sink, which this driver does not support yet. */

#define KICKPI_K7_HDMI_BPC 8

/* Magenta, matching the MIPI DSI path: an obviously synthetic colour makes
 * "the pipeline is running" distinguishable at a glance from "the sink is
 * showing noise / its own test pattern / nothing". */

#define KICKPI_K7_HDMI_FILL_RGB 0xff00ffu

/* HDMI_TX_ON_H -- the TMDS DC level-shift / bias network enable.
 *
 * From the board schematic: this net drives the gates of four MOSFETs
 * (Q5600-Q5603, WNM6002-3/TR) whose drains pull each TMDS line to ground
 * through 590R 1% (R5600-R5607).  The schematic's own note says:
 *
 *   "The controller only support AC coupled link.  In order to backward
 *    compatibility or to meet HDMI2.0 (1.4b) DC common mode spec and Voff,
 *    need do R based level-shift.
 *    Switch on in HDMI2.0(TMDS) mode.  Switch off in HDMI2.1(FRL) mode."
 *
 * This link is AC coupled, so the resistors are what establish the DC common
 * mode and Voff the receiver needs.  With the MOSFETs off the transmitter
 * still drives a perfectly good TMDS clock and data -- the coupling caps pass
 * the AC content -- but the levels sit outside the HDMI 1.4b DC spec and the
 * sink rejects the signal.  That is exactly "clean eye diagram, monitor says
 * no signal".
 *
 * Nothing else in the tree drives this net, so it sat at its reset state.
 * It must be driven HIGH for TMDS/DVI, which is what this driver does.
 * GPIO2_B0 defaults to GPIO function, so no IOMUX change is needed.
 */

#define KICKPI_K7_HDMI_TX_ON_H (GPIO_PORT2 | GPIO_PIN_B0)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* GPIO handle for HDMI_TX_ON_H, claimed once and cached.  The RK3576 GPIO
 * driver enforces single occupancy, so it is deliberately not re-acquired.
 */

static FAR struct gpio_dev_s *g_kickpi_k7_hdmi_tx_on;

/* The mode actually programmed.  It starts as the CEA-861 1080p60 timing
 * above, which is both the fallback when the sink's EDID cannot be used and
 * the only timing this driver knew before EDID support; step 2 of the bring-up
 * order overwrites it in place when the sink offers something this path can
 * drive.  A mutable static rather than a const default plus a runtime copy,
 * because kickpi_k7_video_initialize() keeps the pointer for the life of the
 * framebuffer.
 */

static struct kickpi_k7_video_mode_s g_kickpi_k7_hdmi_mode = {
  .name = "HDMI DVI 1080p60 (default)",
  .xres = KICKPI_K7_HDMI_XRES,
  .yres = KICKPI_K7_HDMI_YRES,
  .hsync_len = KICKPI_K7_HDMI_HSYNC_LEN,
  .hfront_porch = KICKPI_K7_HDMI_HFRONT_PORCH,
  .hback_porch = KICKPI_K7_HDMI_HBACK_PORCH,
  .vsync_len = KICKPI_K7_HDMI_VSYNC_LEN,
  .vfront_porch = KICKPI_K7_HDMI_VFRONT_PORCH,
  .vback_porch = KICKPI_K7_HDMI_VBACK_PORCH,
  .hsync_positive = true,
  .vsync_positive = true,
  .pixel_clock = KICKPI_K7_HDMI_PIXCLK,
  .iface = RK3576_VOP_IFACE_HDMI,
  .port = RK3576_VOP_PORT0,
  .fill_rgb = KICKPI_K7_HDMI_FILL_RGB,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_hdmi_tx_on_enable
 *
 * Description:
 *   Claim HDMI_TX_ON_H and drive it high, engaging the 590R-to-ground DC
 *   level-shift network on the TMDS lines.  See the long note on
 *   KICKPI_K7_HDMI_TX_ON_H for why this is required for TMDS mode.
 *
 *   Called before the PHY starts transmitting: the bias network has to be in
 *   place when the first symbols leave the serialisers, or the sink evaluates
 *   the link during the window in which the levels are out of spec.
 *
 * Returned Value:
 *   Zero (OK) on success, or a negated errno value on failure.
 *
 ****************************************************************************/

static int kickpi_k7_hdmi_tx_on_enable(void)
{
  int ret;

  if (g_kickpi_k7_hdmi_tx_on == NULL)
    {
      ret = rk3576_gpio_get(KICKPI_K7_HDMI_TX_ON_H, &g_kickpi_k7_hdmi_tx_on);
      if (ret < 0)
        {
          return ret;
        }
    }

  rk3576_gpio_set_mode(g_kickpi_k7_hdmi_tx_on, RK3576_GPIO_OUTPUT);
  rk3576_gpio_write_bit(g_kickpi_k7_hdmi_tx_on, true);

  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_hdmi_select_mode
 *
 * Description:
 *   Replace the default 1080p60 mode with the sink's preferred EDID timing,
 *   when the sink offers one this path can actually drive.
 *
 *   EVERY failure leaves the default mode in place and only logs: the display
 *   still comes up, on a timing that is known to work.  That is the reason the
 *   policy lives here rather than in the controller driver -- an EDID is
 *   untrusted data arriving over a connector, and a failed read, a dishonest
 *   block, a rate the PLL cannot synthesise and a mode beyond the DVI limit
 *   are all ordinary outcomes rather than reasons to lose the display.
 *
 *   The three checks between "what the sink asked for" and "what is
 *   programmed" each rule out a different class of unusable mode:
 *
 *     - the pixel clock must be one the TMDS PLL can synthesise.  The PLL is
 *       table-driven, so the sink's clock is snapped to the nearest entry; a
 *       request that is not within 1% of any entry means this PHY cannot carry
 *       the mode at all (see rk3576_hdptxphy_snap_pixel_clock()).
 *     - the rate must stay inside DVI's single-link limit, because there is no
 *       scrambling here to go above it.
 *     - the descriptor itself must be sane, which the decoder has already
 *       enforced before returning.
 *
 *   Must be called after rk3576_hdmi_initialize() (which configures the DDC
 *   master and hands it the pads) and before rk3576_hdmi_enable() (which
 *   programs the PHY with the resulting pixel clock).
 *
 ****************************************************************************/

static void kickpi_k7_hdmi_select_mode(void)
{
  struct rk3576_hdmi_timing_s timing;
  uint32_t pixel_clock;
  int ret;

  ret = rk3576_hdmi_read_sink_timing(&timing);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: HDMI no usable EDID timing (%d); keeping %s "
             "(%ux%u @ %lu Hz)\n",
             ret, g_kickpi_k7_hdmi_mode.name,
             (unsigned int)g_kickpi_k7_hdmi_mode.xres,
             (unsigned int)g_kickpi_k7_hdmi_mode.yres,
             (unsigned long)g_kickpi_k7_hdmi_mode.pixel_clock);
      return;
    }

  pixel_clock =
      rk3576_hdptxphy_snap_pixel_clock(timing.pixel_clock, KICKPI_K7_HDMI_BPC);
  if (pixel_clock == 0)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: HDMI sink asks for a %lu Hz pixel clock and the "
             "TMDS PLL has no setting within 1%% of it; keeping %s\n",
             (unsigned long)timing.pixel_clock, g_kickpi_k7_hdmi_mode.name);
      return;
    }

  if (pixel_clock > KICKPI_K7_HDMI_DVI_MAX_PIXCLK)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: HDMI sink's preferred timing needs %lu Hz, above "
             "the %lu Hz DVI single-link limit (no scrambling support); "
             "keeping %s\n",
             (unsigned long)pixel_clock,
             (unsigned long)KICKPI_K7_HDMI_DVI_MAX_PIXCLK,
             g_kickpi_k7_hdmi_mode.name);
      return;
    }

  g_kickpi_k7_hdmi_mode.name = "HDMI DVI (EDID preferred timing)";
  g_kickpi_k7_hdmi_mode.xres = timing.xres;
  g_kickpi_k7_hdmi_mode.yres = timing.yres;
  g_kickpi_k7_hdmi_mode.hsync_len = timing.hsync_len;
  g_kickpi_k7_hdmi_mode.hfront_porch = timing.hfront_porch;
  g_kickpi_k7_hdmi_mode.hback_porch = timing.hback_porch;
  g_kickpi_k7_hdmi_mode.vsync_len = timing.vsync_len;
  g_kickpi_k7_hdmi_mode.vfront_porch = timing.vfront_porch;
  g_kickpi_k7_hdmi_mode.vback_porch = timing.vback_porch;
  g_kickpi_k7_hdmi_mode.hsync_positive = timing.hsync_positive;
  g_kickpi_k7_hdmi_mode.vsync_positive = timing.vsync_positive;
  g_kickpi_k7_hdmi_mode.pixel_clock = pixel_clock;

  /* Report the adopted mode, mentioning the snap only when there was one.  The
   * common case is that the sink's preferred pixel clock is already in the PLL
   * table and nothing was changed, so saying "snapped to the nearest setting"
   * there would describe work that did not happen.
   */

  if (pixel_clock == timing.pixel_clock)
    {
      syslog(
          LOG_INFO,
          "kickpi-k7: HDMI mode from sink EDID: %ux%u @ %lu Hz, sync %c%c\n",
          (unsigned int)timing.xres, (unsigned int)timing.yres,
          (unsigned long)pixel_clock, timing.hsync_positive ? '+' : '-',
          timing.vsync_positive ? '+' : '-');
    }
  else
    {
      syslog(LOG_INFO,
             "kickpi-k7: HDMI mode from sink EDID: %ux%u @ %lu Hz (sink asked "
             "%lu Hz; nearest TMDS PLL setting), sync %c%c\n",
             (unsigned int)timing.xres, (unsigned int)timing.yres,
             (unsigned long)pixel_clock, (unsigned long)timing.pixel_clock,
             timing.hsync_positive ? '+' : '-',
             timing.vsync_positive ? '+' : '-');
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_hdmi_initialize
 *
 * Description:
 *   Bring up the on-board HDMI output and the VOP framebuffer behind it.
 *
 ****************************************************************************/

int kickpi_k7_hdmi_initialize(void)
{
  int ret;

  /* 0. Enable the TMDS DC level-shift network before anything transmits. */

  ret = kickpi_k7_hdmi_tx_on_enable();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: kickpi-k7 HDMI_TX_ON_H (GPIO2_B0) failed: %d\n",
             ret);
      return ret;
    }

  syslog(LOG_INFO, "kickpi-k7: HDMI_TX_ON_H (GPIO2_B0) high: TMDS DC "
                   "level-shift network engaged\n");

  /* 1. Controller: clocks, resets, VO0_GRF routing, register file, PHY APB.
   *    This is also what configures the DDC master and hands it the pads, so
   *    it is the precondition for reading the sink's EDID.
   */

  ret = rk3576_hdmi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_hdmi_initialize failed: %d\n", ret);
      return ret;
    }

  /* 2. Ask the sink what it can display.  On any failure this leaves the
   *    default 1080p60 mode in place, so the display still comes up.
   */

  kickpi_k7_hdmi_select_mode();

  /* 3. PHY TMDS PLL on, DVI mode selected, for the chosen mode's pixel clock.
   *    After this the pixel clock is live, which is the precondition for
   *    step 4.
   */

  ret = rk3576_hdmi_enable(g_kickpi_k7_hdmi_mode.pixel_clock,
                           KICKPI_K7_HDMI_BPC);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: rk3576_hdmi_enable failed at %lu Hz / %u bpc: %d\n",
             (unsigned long)g_kickpi_k7_hdmi_mode.pixel_clock,
             (unsigned)KICKPI_K7_HDMI_BPC, ret);
      return ret;
    }

  /* 4. VOP: allocate the framebuffer, program the scan-out and route it to
   *    the HDMI interface.  This also reparents the video port's pixel clock
   *    onto the PHY output, so it is the step that closes the loop.
   */

  ret = kickpi_k7_video_initialize(&g_kickpi_k7_hdmi_mode);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: kickpi_k7_video_initialize failed: %d\n", ret);
      return ret;
    }

  /* Report the PHY state rather than assuming it.  is_ready() is the strongest
   * claim the driver can honestly make: it means a mode is enabled AND the
   * PHY reports its PLL locked AND its parallel output ready.  Whether a
   * picture appears additionally depends on the VOP actually driving the IPI
   * and on the sink accepting the signal, neither of which the controller can
   * report -- so a false here is a real failure, and a true with a dark screen
   * points at the sink, the cable or the connectors.
   */

  if (!rk3576_hdmi_is_ready())
    {
      syslog(LOG_ERR,
             "ERROR: kickpi-k7 HDMI PHY reports not ready after enable "
             "(pixel clock %lu Hz, ref %lu Hz)\n",
             (unsigned long)rk3576_hdmi_pixel_clock_hz(),
             (unsigned long)rk3576_hdmi_ref_clock_hz());
      return -ETIMEDOUT;
    }

  /* 5. Complete the video path now that the VOP is scanning.
   *
   *    The controller's video-interface and packet blocks are clock-gated
   *    until the VOP delivers the interface clock, so they cannot be
   *    programmed any earlier -- see rk3576_hdmi_start_video_path().
   */

  ret = rk3576_hdmi_start_video_path();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_hdmi_start_video_path failed: %d\n", ret);
      return ret;
    }

  /* 6. Paint the framebuffer: four horizontal bands, white / red / green /
   *    blue top to bottom.
   *
   *    The colours are what makes the picture self-checking.  White proves the
   *    path is alive end to end; red and blue exercise a single component
   *    each and exchange places under bit or byte order reversal, which a
   *    solid white or black fill could never reveal; green separates the
   *    middle component from the other two.  Nothing is logged: the bands are
   *    the report, and they are visible on the screen.
   */

  {
    static const uint32_t bands[4] = {
      0xffffffu, /* top    - all three components high           */
      0xff0000u, /* red    - swaps with blue under any reversal */
      0x00ff00u, /* green  - the middle component                */
      0x0000ffu  /* blue   - swaps with red under any reversal   */
    };

    (void)rk3576_vop_fill_bands(bands, 4u);
  }

  syslog(LOG_INFO,
         "kickpi-k7: HDMI bring-up complete: %s, %ux%u, DVI/TMDS, pixel clock "
         "%lu Hz, PHY locked and ready\n",
         g_kickpi_k7_hdmi_mode.name, (unsigned int)g_kickpi_k7_hdmi_mode.xres,
         (unsigned int)g_kickpi_k7_hdmi_mode.yres,
         (unsigned long)rk3576_hdmi_pixel_clock_hz());

  return OK;
}

#endif /* CONFIG_KICKPI_K7_HDMI */
