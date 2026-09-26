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
 *   2. rk3576_hdmi_enable(pixel_clock, 8)
 *        Programs the PHY's TMDS PLL for the mode's pixel rate, switches the
 *        controller to DVI mode and releases the video datapath.  THIS is
 *        what puts a pixel clock on the wire -- on the HDMI path the PHY's
 *        PLL is the clock source, not the CRU.
 *
 *   3. kickpi_k7_video_initialize()
 *        Starts the VOP.  It must come after step 2 or the video port has no
 *        clock: rk3576_vop.c reparents dclk_vpN_sel onto clk_hdmiphy_pixel0_o
 *        for the HDMI interface instead of programming a CRU divider.
 *
 * Note the contrast with MIPI DSI, where the VOP comes BEFORE the interface
 * starts transmitting.  Neither ordering is free to change; see the banner in
 * kickpi_k7_video.h.
 *
 * Scope: DVI mode (no infoframes, no audio, no HDCP, no scrambling) at
 * 1080p60.  OPMODE_DVI is also the controller's reset state, which is what
 * makes this the cheapest path to a lit display.
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

static const struct kickpi_k7_video_mode_s g_kickpi_k7_hdmi_mode = {
  .name = "HDMI 1080p60 DVI",
  .xres = KICKPI_K7_HDMI_XRES,
  .yres = KICKPI_K7_HDMI_YRES,
  .hsync_len = KICKPI_K7_HDMI_HSYNC_LEN,
  .hfront_porch = KICKPI_K7_HDMI_HFRONT_PORCH,
  .hback_porch = KICKPI_K7_HDMI_HBACK_PORCH,
  .vsync_len = KICKPI_K7_HDMI_VSYNC_LEN,
  .vfront_porch = KICKPI_K7_HDMI_VFRONT_PORCH,
  .vback_porch = KICKPI_K7_HDMI_VBACK_PORCH,
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
  struct rk3576_vop_status_s vop_status;
  struct rk3576_vop_layer_state_s layer_state;
  int ret;

  /* 0. Enable the TMDS DC level-shift network before anything transmits. */

  ret = kickpi_k7_hdmi_tx_on_enable();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: kickpi-k7 HDMI_TX_ON_H (GPIO2_B0) failed: %d\n",
             ret);
      return ret;
    }

  syslog(LOG_INFO, "kickpi-k7: HDMI_TX_ON_H (GPIO2_B0) driven high: TMDS DC "
                   "level-shift network engaged\n");

  /* 1. Controller: clocks, resets, VO0_GRF routing, register file, PHY APB. */

  ret = rk3576_hdmi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: rk3576_hdmi_initialize failed: %d\n", ret);
      return ret;
    }

  /* 2. PHY TMDS PLL on, DVI mode selected.  After this the pixel clock is
   *    live, which is the precondition for step 3.
   */

  ret = rk3576_hdmi_enable(KICKPI_K7_HDMI_PIXCLK, KICKPI_K7_HDMI_BPC);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: rk3576_hdmi_enable failed at %lu Hz / %u bpc: %d\n",
             (unsigned long)KICKPI_K7_HDMI_PIXCLK,
             (unsigned)KICKPI_K7_HDMI_BPC, ret);
      return ret;
    }

  /* 3. VOP: allocate the framebuffer, program the scan-out and route it to
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

  /* 4. Now that the VOP is running, the video-interface block of the
   *    controller is clocked and can be read.  Doing this before step 3
   *    aborts on the bus -- see rk3576_hdmi_report_status().
   *
   *    This is the measurement that separates "the controller is not being
   *    fed" from "the controller is fed but the sink still says no signal".
   */

  (void)rk3576_hdmi_report_status();

  /* 5. DIAGNOSTIC: paint four horizontal bands -- white, red, green, blue.
   *
   *    This replaces the magenta/green bars.  Those colours were a bad test:
   *
   *      0xff00ff reversed bit-wise = 0xff00ff
   *      0x00ff00 reversed bit-wise = 0x00ff00
   *
   *    Both are invariant under bit and byte order reversal, so a swapped
   *    interface could never have shown up as anything but "no pattern", and
   *    two colours exercise at most two components.
   *
   *    White / red / green / blue is informative in one look at the screen:
   *
   *      all four bands     the pixel path is alive and correctly ordered;
   *      white only         chroma is lost, luma survives;
   *      red and blue
   *        exchanged        bit or byte order is reversed;
   *      one band missing   that component's lane is stuck or misrouted;
   *      all black          no framebuffer pixel is reaching the link at all,
   *                         which is the state this bring-up is chasing.
   *
   *    For the scope, the band edges still break the otherwise constant
   *    symbol stream, so the data lanes must show structure that a solid,
   *    never-changing fill cannot produce.
   */

  {
    static const uint32_t bands[4] = {
      0xffffffu, /* top    - all three components high                  */
      0xff0000u, /* red    - swaps with blue under any reversal        */
      0x00ff00u, /* green  - the middle component                       */
      0x0000ffu  /* blue   - swaps with red under any reversal          */
    };

    if (rk3576_vop_fill_bands(bands, 4u) == OK)
      {
        syslog(LOG_INFO,
               "kickpi-k7: HDMI framebuffer set to 4 horizontal bands "
               "(white / red / green / blue, top to bottom).  Reading the "
               "screen: all four bands = pixel path alive and correctly "
               "ordered; white only = chroma lost; red and blue exchanged = "
               "bit/byte order reversed; all black = no framebuffer pixel "
               "reaches the link.\n");

        /* 5b. WATCH THE SCREEN for the next ~8 seconds.
         *
         *    The IPI colour FORMAT is the last known difference from the
         *    firmware that lights a display, and the equivalent reasoning
         *    about the colour DEPTH was wrong once already, so this steps the
         *    format through all four values the TRM names and dwells on each.
         *
         *    It runs here, after the bands are painted, because a format test
         *    is meaningless while the framebuffer is still zero-filled.
         *
         *    The log names the value in effect at each moment, so a screen
         *    that changes at any point identifies it: "it went to the bands
         *    when the log said YCbCr 4:4:4" is a complete answer.  A screen
         *    that stays black for all four rules the IPI format out.
         */

        syslog(LOG_INFO,
               "kickpi-k7: starting IPI format sweep -- WATCH THE SCREEN for "
               "about 8 seconds and note whether it ever stops being black\n");

        (void)rk3576_hdmi_sweep_ipi_format(2000u);

        /* 5c. Then bisect the pixel path after the POST.
         *
         *     Every register up to and including the POST has been read back
         *     and is correct, the pixels are provably in DRAM, and sweeping
         *     the IPI format changed nothing -- so the one link never tested
         *     is whether ANY pixel leaves the POST.  The window and the POST's
         *     background generator are independent sources into the same
         *     output, so this switches to the background and shows red, green
         *     and blue.
         *
         *     Colour on screen proves the POST-to-interface path is alive and
         *     narrows the fault to the window's fetch/mix; staying black means
         *     nothing from the POST reaches the interface at all.
         */

        syslog(LOG_INFO,
               "kickpi-k7: starting POST background test -- WATCH THE SCREEN "
               "for about 6 seconds (red, then green, then blue)\n");

        (void)rk3576_vop_background_test(2000u);
      }
  }

  if (rk3576_vop_get_status(&vop_status) == OK)
    {
      uint32_t vcnt_first;
      uint32_t vcnt_second;

      /* The vertical counter is free-running, so a single sample means
       * nothing; take two and report both.
       */

      vcnt_first = vop_status.dsp_vcnt0;
      up_mdelay(20);
      (void)rk3576_vop_get_status(&vop_status);
      vcnt_second = vop_status.dsp_vcnt0;

      syslog(LOG_INFO,
             "kickpi-k7: VOP scan-out: dclk %lu Hz, dclk_cnt %lu, aclk_cnt "
             "%lu, dsp_vcnt0 %lu -> %lu %s\n",
             (unsigned long)vop_status.dclk_hz,
             (unsigned long)vop_status.dclk_cnt,
             (unsigned long)vop_status.aclk_cnt, (unsigned long)vcnt_first,
             (unsigned long)vcnt_second,
             vop_status.dclk_cnt == 0
                 ? "(PIXEL CLOCK NOT REACHING THE VIDEO PORT)"
                 : (vcnt_first == vcnt_second ? "(SCAN NOT ADVANCING)"
                                              : "(scan running)"));

      /* The interrupt RAW registers, which are the only ones that can report
       * anything at all (the STATUS forms are masked by enable bits this
       * driver never sets).  These two flags decide whether the POST is being
       * starved, which is the one failure mode that matches "the TMDS lanes
       * carry a constant symbol and no framebuffer content ever appears".
       */

      syslog(LOG_INFO,
             "kickpi-k7: VOP interrupts (raw): VP_INT %08lx "
             "[POST_BUF_EMPTY(4)=%lu under-ran, POST_FULL(9)=%lu], "
             "SYS0_INT %08lx [BUS_ERROR(1)=%lu], SYS1_INT %08lx\n",
             (unsigned long)vop_status.vp_int_raw,
             (unsigned long)((vop_status.vp_int_raw >> 4) & 1u),
             (unsigned long)((vop_status.vp_int_raw >> 9) & 1u),
             (unsigned long)vop_status.sys0_int_raw,
             (unsigned long)((vop_status.sys0_int_raw >> 1) & 1u),
             (unsigned long)vop_status.sys1_int_raw);
    }

  /* Whether the window's pixels are actually reaching the glass.
   *
   * The framebuffer sample is the decisive one.  The ESMART layer reads DRAM
   * with the MMU bypassed, so it does not see the D-cache; if the pixels are
   * non-zero here but the screen is black, the writer never reached DRAM (a
   * missing cache clean).  If they are zero here, no scan-out register can be
   * at fault and the fault is in whatever should have filled the buffer.
   */

  if (rk3576_vop_get_layer_state(&layer_state) == OK)
    {
      syslog(
          LOG_INFO,
          "kickpi-k7: VOP layer path:\n"
          "  fb pa %08lx len %lu stride %lu, first 16 bytes "
          "%08lx %08lx %08lx %08lx\n"
          "  ESMART%lu REGION0_CTRL %08lx [mst_en(0)=%lu fmt(5:1)=%lu "
          "rb_swap(14)=%lu]\n"
          "  ESMART%lu REGION0_YRGB_MST %08lx %s\n"
          "  ESMART%lu AXI_CTRL_IMD %08lx [mmu_bypass(2)=%lu]\n"
          "  OVERLAY%lu LAYER_SEL %08lx [layer0(3:0)=%lu]\n"
          "  OVERLAY%lu MIX0_SRC_ALPHA %08lx MIX0_DST_ALPHA %08lx "
          "[src_factor(7:5)=%lu dst_factor(7:5)=%lu]\n"
          "  OVERLAY%lu PORT_BG_MIX_CTRL %08lx\n"
          "  POST%lu DSP_CTRL %08lx DSP_BG %08lx [bg_display_en(31)=%lu]\n"
          "  => %s\n",
          (unsigned long)layer_state.fb_pa, (unsigned long)layer_state.fb_len,
          (unsigned long)layer_state.stride,
          (unsigned long)layer_state.fb_words[0],
          (unsigned long)layer_state.fb_words[1],
          (unsigned long)layer_state.fb_words[2],
          (unsigned long)layer_state.fb_words[3],

          (unsigned long)layer_state.esmart_idx,
          (unsigned long)layer_state.region0_ctrl,
          (unsigned long)layer_state.mst_en, (unsigned long)layer_state.fmt,
          (unsigned long)layer_state.rb_swap,

          (unsigned long)layer_state.esmart_idx,
          (unsigned long)layer_state.region0_yrgb_mst,
          layer_state.region0_yrgb_mst == layer_state.fb_pa
              ? "(MATCHES the framebuffer)"
              : "(MISMATCH: the layer reads a different address)",

          (unsigned long)layer_state.esmart_idx,
          (unsigned long)layer_state.axi_ctrl_imd,
          (unsigned long)layer_state.mmu_bypass,

          (unsigned long)layer_state.port,
          (unsigned long)layer_state.layer_sel,
          (unsigned long)layer_state.layer0_sel,

          (unsigned long)layer_state.port,
          (unsigned long)layer_state.mix0_src_alpha,
          (unsigned long)layer_state.mix0_dst_alpha,
          (unsigned long)layer_state.src_factor,
          (unsigned long)layer_state.dst_factor,

          (unsigned long)layer_state.port,
          (unsigned long)layer_state.bg_mix_ctrl,
          (unsigned long)layer_state.port, (unsigned long)layer_state.dsp_ctrl,
          (unsigned long)layer_state.dsp_bg,
          (unsigned long)layer_state.bg_display_en,

          (layer_state.fb_words[0] == 0u && layer_state.fb_words[1] == 0u &&
           layer_state.fb_words[2] == 0u && layer_state.fb_words[3] == 0u)
              ? "framebuffer reads as ZERO in memory -- the writer never "
                "reached DRAM, so no scan-out register can fix this"
              : "framebuffer holds non-zero pixels -- the fault is on the "
                "scan-out side, not in the writer");
    }

  syslog(LOG_INFO,
         "kickpi-k7: HDMI bring-up complete: DVI/TMDS, pixel clock %lu Hz, "
         "ref clock %lu Hz, PHY locked and ready\n",
         (unsigned long)rk3576_hdmi_pixel_clock_hz(),
         (unsigned long)rk3576_hdmi_ref_clock_hz());

  return OK;
}

#endif /* CONFIG_KICKPI_K7_HDMI */
