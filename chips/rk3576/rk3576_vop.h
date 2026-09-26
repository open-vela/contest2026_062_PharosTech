/****************************************************************************
 * chips/rk3576/rk3576_vop.h
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
 * RK3576 Video Output Processor (VOP) framebuffer driver public interface.
 *
 * Wraps the NuttX generic framebuffer framework (struct fb_vtable_s) on top
 * of the RK3576 VOP.  rk3576_vop_initialize() allocates the framebuffer
 * (via the DMA heap), programs one WIN0 layer + one video port + one output
 * interface, then self-registers the framebuffer as /dev/fbN.
 *
 * The VOP is shared by multiple physical output interfaces (MIPI DSI,
 * HDMI, eDP, DP, RGB).  Routing is expressed as an (interface, port) pair
 * so that the driver is interface-agnostic and extensible: adding a new
 * output only requires a new RK3576_VOP_IFACE_* entry and a matching
 * interface-ctrl register offset in the internal map.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_VOP_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_VOP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#ifdef CONFIG_RK3576_VOP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Default display number used when config.display is left as
 * RK3576_VOP_DISPLAY_DEFAULT.  The framebuffer is registered as /dev/fbN.
 */

#define RK3576_VOP_DISPLAY_DEFAULT 0

/* Pixel clock requested when rk3576_vop_config.pixel_clock is left 0 (Hz). */

#define RK3576_VOP_DEFAULT_PCLK_HZ 64000000u

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Logical output interface.  Values map to SYS_CTRL_*_INFACE_CTRL offsets
 * through the driver's internal table; the caller never deals with raw
 * register offsets.
 */

enum rk3576_vop_iface_e
{
  RK3576_VOP_IFACE_MIPI_DSI = 0, /* MIPI DSI (SYS_CTRL_MIPI0_INFACE_CTRL) */
  RK3576_VOP_IFACE_HDMI,         /* HDMI (SYS_CTRL_HDMI0_INFACE_CTRL) */
  RK3576_VOP_IFACE_EDP,          /* eDP (SYS_CTRL_EDP0_INFACE_CTRL) */
  RK3576_VOP_IFACE_DP,           /* DP (SYS_CTRL_DP0_INFACE_CTRL) */
  RK3576_VOP_IFACE_RGB,          /* RGB (SYS_CTRL_RGB_INFACE_CTRL) */
  RK3576_VOP_IFACE_MAX
};

/* Video port.  Each port has its own pixel clock (dclk_vp0/1/2) and its
 * own POST timing block (POST0/1/2).  The DSI host's IPI is fed from the
 * port selected by mipi_port_sel (interfaces select a port independently).
 */

enum rk3576_vop_port_e
{
  RK3576_VOP_PORT0 = 0, /* dclk_vp0, up to 600MHz */
  RK3576_VOP_PORT1 = 1, /* dclk_vp1, up to 300MHz */
  RK3576_VOP_PORT2 = 2, /* dclk_vp2, up to 150MHz */
};

/* One-time VOP/framebuffer configuration, passed to
 * rk3576_vop_initialize().  Fixed for the life of the instance.
 */

struct rk3576_vop_config
{
  /* Framebuffer geometry (visible resolution). */

  uint16_t xres; /* Horizontal pixels */
  uint16_t yres; /* Vertical lines */

  /* Output routing: which interface and which video port. */

  enum rk3576_vop_iface_e iface; /* Output interface */
  enum rk3576_vop_port_e port;   /* Video port to route */

  /* Framebuffer device registration. */

  uint8_t display; /* /dev/fbN number */
  uint8_t plane;   /* Color plane (0 for RGB) */

  /* Output timing.  These describe the panel/interface scan-out timing
   * and must match what the output interface (e.g. the DSI host's IPI)
   * is programmed with.
   */

  uint16_t hsync_len;    /* HSYNC pulse width (pixels) */
  uint16_t hfront_porch; /* Horizontal front porch */
  uint16_t hback_porch;  /* Horizontal back porch */
  uint16_t vsync_len;    /* VSYNC pulse width (lines) */
  uint16_t vfront_porch; /* Vertical front porch */
  uint16_t vback_porch;  /* Vertical back porch */

  /* Nominal pixel clock to request for the video port, in Hz.
   *
   * This is the SINGLE SOURCE OF TRUTH for the pixel rate and must be the
   * same number the output interface was programmed with.  It is a REQUEST:
   * the CRU can only divide the parent PLL by an integer, so the achieved
   * dclk will usually differ slightly (asking for 62 MHz on this board lands
   * on gpll/19 = 62.526 MHz).
   *
   * That residual error is deliberately NOT corrected afterwards.  The
   * reference driver also derives all of its IPI timing and PHY ratios from
   * the nominal mode clock, never from the achieved rate, so a ~1% mismatch
   * is the normal, working condition -- and re-writing the IPI timing
   * registers while the video datapath is live re-times a running state
   * machine mid-packet, which is a race (see the comment in
   * rk3576_vop_enable_clocks()).
   */

  uint32_t pixel_clock; /* Hz; 0 = keep the driver default */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop_initialize
 *
 * Description:
 *   Initialize the RK3576 VOP, allocate the framebuffer, program a single
 *   WIN0 RGB888 layer routed through the requested video port to the
 *   requested output interface, and self-register the framebuffer as
 *   /dev/fbN (N = config->display).
 *
 *   The framebuffer is allocated from the RK3576 DMA heap
 *   (rk3576_dma_alloc) because the VOP WIN0 YRGB_MST register is a 32-bit
 *   physical address (MMU bypass), therefore the buffer must be physically
 *   contiguous and below 4GB.
 *
 *   The caller must have already brought up the output interface (e.g.
 *   rk3576_mipi_dsi_initialize()) and programmed its timing to match
 *   config->xres/yres and the porch/sync timings.
 *
 * Input Parameters:
 *   config - Framebuffer/routing configuration.  Must not be NULL.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vop_initialize(FAR const struct rk3576_vop_config *config);

/****************************************************************************
 * Name: rk3576_vop_fill
 *
 * Description:
 *   Fill the framebuffer with a single solid colour.
 *
 *   Two uses, both about making a bring-up experiment legible:
 *
 *     - A solid colour is the only pattern whose appearance on the panel is
 *       unambiguous.  The default checkerboard is fine as an "is the source
 *       black?" probe, but a panel that lights up showing OUR colour proves
 *       pixel data traversed the whole path (VOP -> IPI -> PHY -> panel) with
 *       the right content, while stripes or noise proves the packets arrive
 *       but are misparsed.  A checkerboard cannot tell those apart.
 *
 *     - Giving each configuration under test its OWN colour lets a human
 *       observer report which one lit the panel without needing to read the
 *       console for timing: "it showed green" identifies the third entry of
 *       the matrix directly.
 *
 *   It also gets the source out of the way as an explanation for a dark
 *   screen: an all-zero framebuffer would make every one of the above
 *   unanswerable.
 *
 * Input Parameters:
 *   rgb - 0xRRGGBB (the framebuffer is R,G,B byte order, matching RGB888).
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP has not been initialised.
 *
 ****************************************************************************/

int rk3576_vop_fill(uint32_t rgb);

/****************************************************************************
 * Name: rk3576_vop_fill_bars
 *
 * Description:
 *   Fill the framebuffer with alternating vertical bars of two colours.
 *
 *   Unlike rk3576_vop_fill(), this makes the serialiser emit a REPEATING
 *   SYMBOL SEQUENCE instead of one constant symbol, so the dominant tone on
 *   each TMDS lane moves from the pixel rate (148.5 MHz) down to roughly
 *   148.5/bar_px MHz.  That is what makes it possible to tell a live video
 *   datapath from a dead one with a scope whose bandwidth cannot reach the
 *   pixel rate -- see the implementation for the full reasoning.
 *
 * Input Parameters:
 *   rgb_a  - Colour of the even bars, 0xRRGGBB.
 *   rgb_b  - Colour of the odd bars, 0xRRGGBB.
 *   bar_px - Width of one bar in pixels; must be non-zero.
 *
 * Returned Value:
 *   OK, -ENODEV if the VOP has not been initialised, -EINVAL if bar_px is 0.
 *
 ****************************************************************************/

int rk3576_vop_fill_bars(uint32_t rgb_a, uint32_t rgb_b, uint32_t bar_px);

/****************************************************************************
 * Name: rk3576_vop_fill_bands
 *
 * Description:
 *   Paint horizontal bands of solid colour into the framebuffer and clean the
 *   D-cache so the ESMART layer's MMU-bypassed fetch sees them.
 *
 *   Preferred over rk3576_vop_fill_bars() for bring-up.  The bar colours it
 *   shipped with (magenta 0xff00ff and green 0x00ff00) are both invariant
 *   under bit and byte order reversal, so a swapped interface could not have
 *   shown up as anything but "still no pattern"; white/red/green/blue are not
 *   (red and blue exchange places), and each primary exercises one component.
 *
 * Input Parameters:
 *   colors - Array of RGB888 values, first band first.
 *   nbands - Number of entries in colors.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENODEV if no display is up; -EINVAL on a bad
 *   argument.
 *
 ****************************************************************************/

int rk3576_vop_fill_bands(FAR const uint32_t *colors, uint32_t nbands);

/****************************************************************************
 * Name: rk3576_vop_background_test
 *
 * Description:
 *   Bisect the pixel path AFTER the POST: disable the window, drive the POST's
 *   own background colour generator through red, green and blue, then restore
 *   the window.
 *
 *   Everything up to and including the POST has been verified by register
 *   read-back (window enabled, correct format and swap, YRGB_MST equal to the
 *   framebuffer's physical address, opaque-bottom-layer mixer factors,
 *   background disabled, full-frame active/display sizes) and the pixels are
 *   demonstrably present in DRAM, yet the display stays black.
 *
 *   The window and the POST background are two independent sources feeding the
 *   same output, so switching between them isolates the last untested link:
 *
 *     the screen shows colour -> the POST-to-interface path works and the
 *                                fault is in the window's fetch or mixing;
 *     the screen stays black -> nothing from the POST reaches the interface,
 *                                so no window setting can help.
 *
 *   The window is disabled during the test so the mixer cannot cover the
 *   background, and the colours are saturated so a photograph is unambiguous.
 *
 * Input Parameters:
 *   hold_ms - Time to hold each colour, in milliseconds.  0 means 2000.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENODEV if the VOP has not been initialised.
 *
 ****************************************************************************/

int rk3576_vop_background_test(uint32_t hold_ms);

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Live VOP scan-out state, for bring-up diagnostics.  All fields are read
 * from hardware counters rather than inferred from register read-backs,
 * because every VOP register reads back correctly even when the datapath is
 * stopped -- the register file sits on the peripheral clock, which stays
 * alive on its own.
 */

struct rk3576_vop_status_s
{
  uint32_t dclk_hz;      /* clk_get_rate(dclk_vpN) -- the requested clock    */
  uint32_t dclk_cnt;     /* POST_CLK_CNT dclk cycles over a fixed window     */
  uint32_t aclk_cnt;     /* POST_CLK_CNT aclk cycles over the same window    */
  uint32_t dsp_vcnt0;    /* SYS_STATUS0 vertical counter, per-line increment */
  uint32_t vp_int_raw;   /* VP_INT_RAW_STATUS(port): bit4 POST_BUF_EMPTY  */
  uint32_t sys0_int_raw; /* SYS0_INT_RAW: bit1 BUS_ERROR                  */
  uint32_t sys1_int_raw; /* SYS1_INT_RAW                                  */
};

/****************************************************************************
 * Name: rk3576_vop_get_status
 *
 * Description:
 *   Sample the VOP's hardware counters to answer "is the scan-out path
 *   actually running?".
 *
 *   Three independent readings, because each failure mode looks different:
 *
 *     dclk_cnt == 0   the pixel clock is not reaching the video port at all.
 *                     On the HDMI path that means the PHY-output mux
 *                     (dclk_vpN_sel) did not take, or the PHY's pixel clock
 *                     output is not running -- the CRU divider chain is not
 *                     involved.
 *     aclk_cnt == 0   the datapath (AXI/master) clock is gated: the request
 *                     generator never runs, so the layer's reads never leave
 *                     the chip and the POST buffer under-runs.
 *     dsp_vcnt0 == 0  the vertical counter is not advancing, i.e. the POST
 *                     scan state machine is not running.
 *
 *   dclk_cnt and aclk_cnt are counted over the SAME fixed window, so their
 *   ratio is directly comparable and dclk doubles as a control: the POST
 *   timing generator demonstrably runs (forced black and the solid background
 *   both reach the glass on the DSI path), so dclk must count.
 *
 *   dsp_vcnt0 is a free-running line counter and is sampled, not latched, so
 *   two consecutive calls returning the same value is the meaningful test.
 *
 * Input Parameters:
 *   status - Receives the sample.  Must not be NULL.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENODEV if the VOP has not been initialised.
 *
 ****************************************************************************/

int rk3576_vop_get_status(FAR struct rk3576_vop_status_s *status);

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Live layer-path state, for bring-up diagnostics.
 *
 * rk3576_vop_get_status() answers "is the scan-out engine running"; this
 * answers the next question along the pipe, "is the window's data reaching
 * the POST at all".  Those are independent: the POST can scan perfectly while
 * the OVERLAY mixers substitute their own background, which is how a fully
 * configured framebuffer ends up as a black screen.
 *
 * Everything needed to tell those apart is a register read, so it is all
 * reported here rather than in the driver, because the VOP's own logging is
 * gerr/ginfo and gated off in this build.
 *
 * The framebuffer's first bytes matter as much as the registers: if the
 * pixels are non-zero in memory but the display is black, the fault is on the
 * scan-out side; if they are zero in memory, no register is at fault.
 */

struct rk3576_vop_layer_state_s
{
  uint32_t esmart_idx;  /* ESMART window index this port fetches through */
  uint32_t port;        /* Video port / POST index                      */
  uint32_t fb_pa;       /* Framebuffer physical address (MMU bypass)     */
  uint32_t fb_len;      /* Framebuffer length in bytes                   */
  uint32_t stride;      /* Line stride in bytes                          */
  uint32_t fb_words[4]; /* fb_words[0..3] = first 16 bytes of the buffer */

  uint32_t region0_ctrl;     /* ESMART REGION0_CTRL: format + MST_EN      */
  uint32_t region0_yrgb_mst; /* ESMART REGION0_YRGB_MST: read address     */
  uint32_t axi_ctrl_imd;     /* ESMART AXI_CTRL_IMD: MMU bypass + axi sel */

  uint32_t layer_sel;      /* OVERLAY LAYER_SEL: which window feeds L0  */
  uint32_t mix0_src_alpha; /* OVERLAY MIX0_SRC_ALPHA_CTRL               */
  uint32_t mix0_dst_alpha; /* OVERLAY MIX0_DST_ALPHA_CTRL               */
  uint32_t bg_mix_ctrl;    /* OVERLAY PORTx_BG_MIX_CTRL                 */

  uint32_t dsp_ctrl; /* POST DSP_CTRL: output format              */
  uint32_t dsp_bg;   /* POST DSP_BG: bit31 = background displayed */

  /* The same registers, decoded.  The bit positions deliberately live in ONE
   * place -- here, driven by the definitions in hardware/rk3576_vop.h -- and
   * not at the call site.  The first version of this diagnostic decoded them
   * in the board file, from memory, and got REGION0_CTRL's enable, format and
   * swap bits (and AXI_CTRL_IMD's bypass bit) all wrong, producing a log that
   * accused a correctly programmed layer of being disabled.  A bring-up
   * diagnostic that misleads is worse than having none.
   */

  uint32_t mst_en;        /* REGION0_CTRL bit0: 1 = region enabled      */
  uint32_t fmt;           /* REGION0_CTRL [5:1]: 1 = RGB888             */
  uint32_t rb_swap;       /* REGION0_CTRL bit14: 1 ONLY for a BGR buffer */
  uint32_t mmu_bypass;    /* AXI_CTRL_IMD bit2: 1 = YRGB_MST is a PA    */
  uint32_t layer0_sel;    /* LAYER_SEL [3:0]: 2 = ESMART0              */
  uint32_t src_factor;    /* MIX0_SRC_ALPHA [7:5]: 1 = ONE             */
  uint32_t dst_factor;    /* MIX0_DST_ALPHA [7:5]: 3 = DST_INVERSE     */
  uint32_t bg_display_en; /* POST DSP_BG bit31: must be 0              */
};

/****************************************************************************
 * Name: rk3576_vop_get_layer_state
 *
 * Description:
 *   Read back the window/mixer/post registers that decide whether the
 *   framebuffer's pixels reach the glass, plus the first bytes of the
 *   framebuffer itself.
 *
 * Input Parameters:
 *   state - Receives the sample.  Must not be NULL.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENODEV if the VOP has not been initialised;
 *   -EINVAL if state is NULL.
 *
 ****************************************************************************/

int rk3576_vop_get_layer_state(FAR struct rk3576_vop_layer_state_s *state);

#endif /* CONFIG_RK3576_VOP */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_VOP_H */
