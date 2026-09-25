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
 * Name: rk3576_vop_get_scan_counter
 *
 * Description:
 *   Read the VP0 vertical scan counter (SYS_STATUS0.dsp_vcnt0), which
 *   increments while the timing generator scans.  Read it twice with a short
 *   delay to tell a live pixel stream from a stopped one -- the decisive
 *   check when a panel lights up and then fades, i.e. when the display is no
 *   longer being refreshed.
 *
 *   Sample the two reads a few ms apart: the counter is 13 bits and wraps
 *   every ~6 frames, so a long interval makes the delta meaningless.
 *
 * Returned Value:
 *   13-bit scan counter, or 0 if the VOP has not been initialised.
 *
 ****************************************************************************/

uint32_t rk3576_vop_get_scan_counter(void);

/****************************************************************************
 * Name: rk3576_vop_set_output_enable
 *
 * Description:
 *   Gate the selected interface's output (out_en + clk_out_en in
 *   SYS_CTRL_*_INFACE_CTRL) on or off WITHOUT leaving the DSI in video mode.
 *
 *   Bring-up diagnostic: the DSI's transmit FSM can park mid-packet, holding
 *   the lanes out of LP-11 forever.  Stopping the pixel feed cannot make a
 *   wedged PHY recover, but it DOES tell the two cases apart:
 *     - lanes return to LP-11 -> the DSI was waiting on the pixel stream
 *       (the fault is the VOP -> IPI handoff);
 *     - lanes stay out of LP-11 -> the DSI/PHY is wedged on its own.
 *
 * Input Parameters:
 *   enable - true to route the interface output, false to gate it off.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP has not been initialised.
 *
 ****************************************************************************/

int rk3576_vop_set_output_enable(bool enable);

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
 * Name: rk3576_vop_paint_test_pattern
 *
 * Description:
 *   Paint a test image that can be INTERPRETED, instead of one that can only
 *   be described.
 *
 *   The image is eight vertical colour bars (white / yellow / cyan / green /
 *   magenta / red / blue / black from left to right) with the top eighth of
 *   the screen filled with a solid marker colour.
 *
 *   Why this replaces the solid-colour fill once the panel shows something:
 *   a solid colour can only tell you THAT pixels arrived.  Vertical bars are
 *   read out against the display's own scan, so the way they come out names
 *   the fault directly:
 *
 *     eight clean bars              -> geometry, format and sync are all
 *                                      correct; stop looking at the pipeline
 *     bars but colours permuted or   -> pixel format / byte order mismatch
 *       the wrong band colour
 *     bars slanted, torn, doubled,   -> the panel is not locking onto the
 *       or rolled                       frame/line structure (timing, mode or
 *                                      clock-lane behaviour)
 *     noise rather than bars        -> packet structure, not timing
 *
 *   The marker band exists so the mode under test can be identified from the
 *   screen alone: each matrix entry paints its own marker colour, so "I saw
 *   stable bars with a red band" names the configuration that works without
 *   anyone having to correlate console timings.
 *
 * Input Parameters:
 *   marker_rgb - 0xRRGGBB for the top band.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP has not been initialised.
 *
 ****************************************************************************/

int rk3576_vop_paint_test_pattern(uint32_t marker_rgb);

/****************************************************************************
 * Name: rk3576_vop_paint_byte_probe
 *
 * Description:
 *   Paint eight full-height vertical bars in which EVERY byte of a bar is one
 *   constant value: 0x00, 0x24, 0x49, 0x6d, 0x92, 0xb6, 0xdb, 0xff from left
 *   to right -- a dark-to-bright ramp.
 *
 *   Why constant bytes: every fault known to garble a picture -- a wrong number
 *   of bytes per pixel, a lane swap, a lane-to-lane skew, a byte- or bit-order
 *   error -- only ever RE-ARRANGES the bytes.  A region whose bytes are all
 *   equal is therefore invariant under all of them, while a solid colour (whose
 *   byte stream repeats every three bytes) is not: a two- or four-byte packing
 *   turns it into a fixed multi-colour dither.  That is exactly how a screen
 *   can show garble that ignores which solid colour the framebuffer holds.
 *
 *   So if these bars come out as eight clean bands, the glass is being driven
 *   by our bytes and the fault is a byte-level misalignment; if the screen
 *   shows noise with no bands at all, our bytes are not driving the glass.
 *   Only the brightness of the bands can be wrong, which is itself informative:
 *   a clean dark-to-bright ramp means the 24-bit pixels were decoded as sent.
 *
 *   The whole frame is covered (no marker band), so no part of the image is
 *   outside the invariant.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP has not been initialised.
 *
 ****************************************************************************/

int rk3576_vop_paint_byte_probe(void);

/****************************************************************************
 * Name: rk3576_vop_peek_pixel
 *
 * Description:
 *   Read one pixel back out of the framebuffer, in 0xRRGGBB form.
 *
 *   This exists to make a negative result falsifiable.  "The panel does not
 *   show the bars" has two very different causes -- the panel is not decoding
 *   the stream, or the pattern was never written -- and the second one leaves
 *   no trace in any other line of the log.  Checking a few known pixels of a
 *   pattern whose expected values are known by construction removes it.
 *
 * Input Parameters:
 *   x   - Column, 0 .. xres-1
 *   y   - Row, 0 .. yres-1
 *   rgb - Location to return 0xRRGGBB
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not initialised, -EINVAL if out of range.
 *
 ****************************************************************************/

int rk3576_vop_peek_pixel(uint16_t x, uint16_t y, FAR uint32_t *rgb);

/****************************************************************************
 * Name: rk3576_vop_dram_witness
 *
 * Description:
 *   Prove that the framebuffer bytes reach DRAM rather than only ever living
 *   in the CPU's D-cache.  A CPU read-back cannot show this: it is answered by
 *   the cache, so it passes even when DRAM holds something else -- and the
 *   ESMART layer, with its MMU bypassed, reads DRAM.  The test paints a known
 *   pattern, cleans the cache, fingerprints it, invalidates the cache and
 *   fingerprints it again; a mismatch means the layer has been scanning out
 *   whatever DRAM happened to contain, which no framebuffer write could ever
 *   change.
 *
 * Returned Value:
 *   OK if DRAM holds the pattern, -EIO if it does not.
 *
 ****************************************************************************/

int rk3576_vop_dram_witness(void);

/****************************************************************************
 * Name: rk3576_vop_address_echo
 *
 * Description:
 *   Point the ESMART layer's fetch at one of two physically separate buffers
 *   and paint it, so "the layer reads the address we program" becomes a
 *   measurement instead of an assumption.  If the fetch follows the register
 *   the picture must change with it, which tests the one fault that explains a
 *   static, content-independent image without depending on the panel decoding
 *   anything.  The register readback is not a witness here: it reports the
 *   mirror, and the mirror->real load is the very step that can stay pending.
 *
 * Input Parameters:
 *   second - true to fetch from the second buffer, false for the primary
 *   rgb    - colour to paint into the selected buffer
 *
 * Returned Value:
 *   The physical address programmed, or 0 on failure.
 *
 ****************************************************************************/

uint32_t rk3576_vop_address_echo(bool second, uint32_t rgb);

/****************************************************************************
 * Name: rk3576_vop_source_probe
 *
 * Description:
 *   Establish WHERE the pixels on the glass come from, by walking a ladder in
 *   which the pixel source is progressively more "ours": POST forced black,
 *   POST forced zero, ESMART0 disabled with the POST background set to solid
 *   blue, and finally the normal layer path with the framebuffer filled
 *   white.  The first two rungs generate pixels inside the POST and read no
 *   memory at all, so they answer "can anything we control reach the glass?"
 *   without the layer, the address registers or the framebuffer being able to
 *   influence the result.
 *
 *   It exists because a byte-uniform image cannot be turned into garble by
 *   any receiver-side fault, so a garble that survives both pure black and
 *   pure white cannot be showing our bytes -- and every content probe this
 *   driver has run wrote the framebuffer through ESMART0.  A register dump of
 *   the mixer and cfg_done state precedes the rungs.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_source_probe(void);

/****************************************************************************
 * Name: rk3576_vop_underrun_probe
 *
 * Description:
 *   Measure whether the POST's output-buffer under-run (POST_BUF_EMPTY, the
 *   per-VP bit that means "the pixels did not arrive in time") RECURS, using
 *   the dedicated vcnt capture in SYS_STATUS0, and test whether the AXI
 *   outstanding limits this driver invented are what starve the layer.  The
 *   limits are raised to the maximum their fields can encode and the same
 *   window is re-measured; the higher value is kept only if it removes the
 *   under-run.
 *
 *   This matters because a starved POST transmits something other than the
 *   framebuffer, which is the only mechanism found so far that explains a
 *   picture following neither content nor address.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_underrun_probe(void);

/****************************************************************************
 * Name: rk3576_vop_scan_regs
 *
 * Description:
 *   Print every non-zero register in the ESMART0 and SYS_CTRL blocks.  This
 *   board does not start from reset -- it boots through a vendor bootloader
 *   that drives the same VOP for a boot logo -- so a bring-up that writes only
 *   the registers it knows about inherits whatever else is left.  Almost every
 *   register in these blocks resets to zero, so any non-zero value this driver
 *   did not write is a leftover, and an enabled REGION1/2/3 would composite
 *   stale memory over the framebuffer.  Read-only.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_scan_regs(void);

/****************************************************************************
 * Name: rk3576_vop_request_probe
 *
 * Description:
 *   Decide by experiment whether the ESMART layer issues AXI read requests at
 *   all: point its fetch at an undecodable address and see whether the VOP
 *   latches a bus error.  A read that is issued must fail against an address no
 *   AXI slave decodes; a layer that issues nothing cannot produce an error.
 *
 *   axi0_mmu_idle cannot answer this -- with esmart_mmu_bypass set it may read
 *   "idle" either way -- so the request path has never actually been observed.
 *   The fork matters: a bus error means the fetch works and only the return
 *   path or pipeline behind it is at fault, while no error means the request
 *   never leaves the window and nothing downstream can be the cause.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_request_probe(void);

/****************************************************************************
 * Name: rk3576_vop_mmu_fault_probe
 *
 * Description:
 *   A second, independent way to ask whether the ESMART layer issues memory
 *   accesses.  The poison-address test relies on a bus error being generated
 *   AND latched; this one turns the MMU's paging on with dte_addr left at zero,
 *   so every translation must fail and any access the layer makes is recorded
 *   along with the index of the master responsible (page_fault_bus_id).  The
 *   MMU is left bypassed again afterwards.
 *
 *   Agreement between the two methods is real evidence; disagreement is itself
 *   informative, which is the point of having a second mechanism.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_mmu_fault_probe(void);

/****************************************************************************
 * Name: rk3576_vop_security_probe
 *
 * Description:
 *   Read the SoC's VOP security configuration (SYS_SGRF_SOC_CON1[10:0]), which
 *   sits outside the VOP register block and has never been observed by any scan
 *   in this bring-up.  If secure mode were left enabled, the TRM says esmart0 is
 *   treated as the security layer, forced to the secure port, and FORCIBLY
 *   CLOSED when it lands on a non-secure port -- which would leave a layer that
 *   reads back correct and yet issues no memory accesses at all.  Read-only.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_security_probe(void);

/****************************************************************************
 * Name: rk3576_vop_cfg_done_probe
 *
 * Description:
 *   Prove that the mirror -> real register load for the WINDOW group actually
 *   completes, instead of assuming it from a readback of the mirror.
 *
 *   This is the one assumption every measurement in this project has rested on
 *   and none has ever tested.  All layer configuration goes to MIRROR
 *   registers; the real registers are only updated at a frame boundary after
 *   the load bit in SYS_WIN_REG_CFG_DONE is written.  Readbacks return the
 *   MIRROR, so they report the intended value whether or not the load happened.
 *
 *   If the window group's load never completes, ESMART0's real registers keep
 *   their reset values (mst_en = 0, YRGB address = 0), and that single failure
 *   accounts for all three of the hardest measurements at once:
 *     - the layer issues NO memory accesses (two independent probes agree);
 *     - every layer register reads back correct, because it is the mirror;
 *     - changing ESMART0's configuration changes NOTHING on the glass (just
 *       measured: clearing the frame-reset bit had no effect), while changing
 *       SYS-block configuration DOES change the picture -- the SYS groups load.
 *
 *   The load bits are request bits: 1 while pending, cleared by the frame
 *   boundary that consumes them.  Pulse, read (expect pending), wait several
 *   frames, read again (expect consumed).  Still set after the wait means the
 *   load never happened.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_cfg_done_probe(void);

/****************************************************************************
 * Name: rk3576_vop_dma_liveness_probe
 *
 * Description:
 *   The first DIRECT test of whether any DMA inside the VOP is transferring
 *   data -- as opposed to inferring it from the absence of a bus error or an MMU
 *   fault, which can only ever argue from a missing signal.
 *
 *   Clears the SYS0/SYS1 interrupt latches, waits two frames while sampling the
 *   per-channel busy status, then reads the latches back.  A dma_finish bit that
 *   returns after being cleared means a DMA engine really is completing
 *   transfers; one that never returns means the bit was a stale bootloader
 *   leftover and the absence of layer fetches stands on positive evidence too.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_dma_liveness_probe(void);

/****************************************************************************
 * Name: rk3576_vop_winload_probe
 *
 * Description:
 *   Isolates whether the ESMART window group's mirror->real load happens at
 *   all, and whether the layer's own enable bit reaches silicon, by freezing
 *   the POST background at a constant blue and changing exactly one window
 *   register per rung (enable, then geometry).
 *
 *   This exists because the one rung that appeared to show the layer's own
 *   enable bit working changed two things at once (it also turned the POST
 *   background on), so it proved nothing, and because "changing the ESMART
 *   block changes nothing" admits an explanation that fits every stubborn
 *   observation: the real window registers are still the bootloader's, so the
 *   panel shows that buffer at a valid DRAM address -- static, content-
 *   independent, and incapable of raising a bus error or an MMU fault.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_winload_probe(void);

/****************************************************************************
 * Name: rk3576_vop_axi_ctrl_probe
 *
 * Description:
 *   Determines the write format of ESMART_AXI_CTRL_IMD by writing the same
 *   value in five candidate encodings and reading each back, then re-tests the
 *   working one after a delay to see whether the hardware clears it again.
 *
 *   This exists because every log shows the driver's write to that register
 *   not surviving -- mmu_bypass, outstanding_en and outstanding_num[7:4] all
 *   read back zero -- and this driver programs physical addresses, for which a
 *   missing MMU bypass is the documented way to get a read that never returns
 *   with no bus error.  It also verifies that the SYS-level bypass in
 *   SYS_MMU_CTRL_IMD, the backstop behind it, really reads back set.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_axi_ctrl_probe(void);

/****************************************************************************
 * Name: rk3576_vop_win_start_probe
 *
 * Description:
 *   Tries to START the window's internal logic rather than set another value in
 *   it, now that every static register is verified against the reference driver
 *   and the TRM.
 *
 *   Rung A takes the region enable through 0 before returning it to 1, in case
 *   the gated transfer has to BEGIN rather than merely be permitted.
 *
 *   Rung B sets esmart_frm_resetn_en (ESMART_CTRL0 bit31), which the TRM says
 *   resets the layer's internal logic on every vsync while deliberately
 *   excluding the register logic -- a mechanism that matches this board's
 *   symptom exactly.  With the POST background frozen blue, that rung predicts
 *   a BLUE screen if the description is accurate, so it either confirms the
 *   mechanism or falsifies it.
 *
 *   Rung C releases the bit again to confirm the layer responds at all.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_win_start_probe(void);

/****************************************************************************
 * Name: rk3576_vop_clk_alive_probe
 *
 * Description:
 *   Uses POST_CLK_CNT to measure whether the VOP's datapath clock (aclk) is
 *   running, with dclk as a built-in control.
 *
 *   The register file is clocked by hclk/pclk and the fetch engine by aclk, and
 *   a stopped aclk answers every register read perfectly while issuing no
 *   memory access at all -- and raising no error, because nothing happens.  This
 *   is the only mechanism that accounts for every observation on this board at
 *   once, and it had never been measured.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_clk_alive_probe(void);

/****************************************************************************
 * Name: rk3576_vop_scan_status_probe
 *
 * Description:
 *   Reads the two per-video-port status bits this driver has never examined --
 *   dma_stop_valid and mmu_idle in SYS_STATUS0/1/2 -- and watches the VP0
 *   vertical counter to establish whether the video port is actually scanning.
 *
 *   dma_stop_valid is a STATUS where SYS_AXI0_CTRL_IMD holds a CONTROL, and a
 *   channel that is genuinely stopped issues no read and raises no error: that
 *   is exactly the combination measured here, with the mixer still waiting for
 *   the layer.  A frozen vertical counter would instead put the fault in the
 *   video port rather than in any window.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_scan_status_probe(void);

/****************************************************************************
 * Name: rk3576_vop_cluster_probe
 *
 * Description:
 *   Puts CLUSTER0 on layer0 in place of ESMART0 and watches the screen, to
 *   decide whether the window that never fetches is broken inside the ESMART
 *   block or whether the fault lies in the request path every window shares.
 *
 *   CLUSTER0 reaches the same video port, is configured with its own register
 *   block and has its own DMA, but shares the overlay request path, the AXI
 *   ports and the clocks with ESMART.  A framebuffer image during the probe
 *   means the fault is ESMART-specific; no change means it is shared, which
 *   would retire every window-level setting as a suspect.
 *
 *   The ESMART configuration is restored on the way out.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP or framebuffer is not available.
 *
 ****************************************************************************/

int rk3576_vop_cluster_probe(void);

/****************************************************************************
 * Name: rk3576_vop_port_dma_stop_probe
 *
 * Description:
 *   Decodes every named bit of SYS_PORT_CTRL_IMD and, if vfp0_dma_stop_en
 *   (bit8) is set, clears it and watches the screen.
 *
 *   The CLUSTER0 bisection showed that two windows with independent register
 *   blocks, DMA engines and AXI read-ids fail identically, so the fault lies in
 *   what all windows share.  A per-video-port DMA stop is precisely such a
 *   thing, it is inside a register this driver only ever writes through a mask
 *   that EXCLUDES bit8, and a stopped port fetches nothing while raising no
 *   error -- the exact pattern measured here.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_port_dma_stop_probe(void);

/****************************************************************************
 * Name: rk3576_vop_addr_screen_probe
 *
 * Description:
 *   Points the window's fetch at four wildly different addresses, one at a
 *   time, and asks the only question that needs no register interpretation:
 *   does the picture on the glass change?
 *
 *   Every "the layer issues no reads" conclusion so far rests on a chain of
 *   register assumptions -- no bus error latched, no MMU fault, no AXI busy --
 *   and this project has repeatedly been misled by exactly that kind of chain.
 *   A change in the picture when the address changes proves the window is
 *   reading memory; an identical picture for four addresses that cannot hold
 *   the same data proves it is not.
 *
 *   One rung fills the framebuffer with solid red first, so a working fetch
 *   cannot be missed.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP or framebuffer is not available.
 *
 ****************************************************************************/

int rk3576_vop_addr_screen_probe(void);

/****************************************************************************
 * Name: rk3576_vop_ref_align_probe
 *
 * Description:
 *   Applies, one rung at a time and with the screen watched after each, the
 *   differences found between this driver and a register dump taken from this
 *   same board running a known-good configuration (Debian + HDMI):
 *
 *     1. OVL_PORT0_CTRL bit28, the layer-select regdone-immediate bit the
 *        vendor documents as being what makes the overlay's layer selection --
 *        and therefore the window's configuration -- take effect;
 *     2. OVL_PORT0_CTRL bit0, overlay_mode, only to see whether it matters;
 *     3. ESMART REGION0_SCL_CTRL = 0x44, the filter modes the vendor programs
 *        even at 1:1;
 *     4. the overlay mixer coefficients this driver never writes at all.
 *
 *   All four are left applied; the rung at which the picture appears is the one
 *   that mattered.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_ref_align_probe(void);

/****************************************************************************
 * Name: rk3576_vop_ref_align_probe
 *
 * Description:
 *   Applies, one rung at a time and with the screen watched after each, the
 *   differences found between this driver and a register dump taken from this
 *   same board running a known-good configuration (Debian + HDMI):
 *
 *     1. OVL_PORT0_CTRL bit28, the layer-select regdone-immediate bit the
 *        vendor documents as being what makes the overlay's layer selection --
 *        and therefore the window's configuration -- take effect;
 *     2. OVL_PORT0_CTRL bit0, overlay_mode, only to see whether it matters;
 *     3. ESMART REGION0_SCL_CTRL = 0x44, the filter modes the vendor programs
 *        even at 1:1;
 *     4. the overlay mixer coefficients this driver never writes at all.
 *
 *   All four are left applied; the rung at which the picture appears is the one
 *   that mattered.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_ref_align_probe(void);

/****************************************************************************
 * Name: rk3576_vop_full_dump
 *
 * Description:
 *   Prints every non-zero word of the VOP's 128 KB register space in exactly
 *   the format of a dump taken from this board running a known-working
 *   configuration, so the two can be compared with a plain diff.
 *
 *   One line per non-zero register: four hex digits of offset, a space, eight
 *   hex digits of value, with no prefix or timestamp, delimited by the markers
 *   "VOPDUMP START" and "VOPDUMP END".  This replaces inference from the TRM
 *   and the vendor driver with an actual on-silicon reference.
 *
 ****************************************************************************/

void rk3576_vop_full_dump(void);

/****************************************************************************
 * Name: rk3576_vop_port_ctrl_align_probe
 *
 * Description:
 *   Verifies, at the END of the boot, that SYS_PORT_CTRL_IMD still holds the
 *   value taken from a working register dump on this same board.
 *
 *   configure_port() now writes only bits[2:0] (reg_done_frm = 0), which
 *   reproduces that working word 0x00070038 exactly, because the reset value
 *   already supplies dsp_vs_t_sel = 1, auto_cs_en = 1 and the reserved bit 3.
 *   This function exists because several later probes write the same register
 *   -- port_dma_stop_probe among them -- and nothing checked it afterwards.
 *
 *   The three bits at stake are the ones this driver used to get wrong, all of
 *   them by reading the vendor source instead of observing working silicon:
 *   dsp_vs_t_sel (we cleared it; working silicon holds its reset value 1),
 *   auto_cs_mode (we set it; RK3576's control table has no field for it, so it
 *   must stay 0), and reg_done_frm (we never wrote it, leaving the reset value
 *   1; Linux writes 0).  A register dump from this board with a known-working
 *   configuration reads 0x00070038 while this driver used to read 0x8030802f.
 *
 *   If the value has been disturbed, the working value is re-applied and
 *   announced, so a regression here cannot pass unnoticed.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_port_ctrl_align_probe(void);

/****************************************************************************
 * Name: rk3576_vop_scl_align_probe
 *
 * Description:
 *   Re-reads the scaler block (SCL_CTRL = 0x44, SCL_FACTOR_YRGB = 0, both taken
 *   from a working dump on this board), then walks one rung at a time through
 *   the last three places the ESMART0 block differs from that dump:
 *   ESMART0_CTRL1 bit28, the AXI outstanding bound, and the alpha map.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_scl_align_probe(void);

/****************************************************************************
 * Name: rk3576_vop_window_group_probe
 *
 * Description:
 *   Re-runs the decisive window-group experiment -- shrink ACT/DSP to 200x200
 *   and see whether the corner appears -- now that the commit asks for VP0's
 *   register groups only, exactly as Linux does, instead of also requesting
 *   VP1's and VP2's, which can never be consumed on this board.  The POST
 *   background is frozen solid blue so that "blue" can only mean "the layer
 *   contributed nothing".
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_window_group_probe(void);

/****************************************************************************
 * Name: rk3576_vop_post_scl_probe
 *
 * Description:
 *   A/B on POST_SCL_FACTOR_YRGB -- the POST's output scale factor, the last
 *   block before the DSI -- between the degenerate 0 this driver used to write
 *   and the 1.0 (0x10001000) that Linux computes via scl_cal_scale2() and that
 *   a working dump on this board holds.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_post_scl_probe(void);

/****************************************************************************
 * Name: rk3576_vop_coarse_pattern_probe
 *
 * Description:
 *   Paints the framebuffer with eight wide full-height colour bars and holds it,
 *   with the POST background frozen solid blue.
 *
 *   Every display judgement in this bring-up has used solid colours, and a
 *   uniform image is invariant under every positional error there is -- any
 *   scaling, line mis-address, h/v offset or stream desynchronisation leaves it
 *   looking identical.  So the solid-colour tests could never separate "the
 *   window fetches nothing" from "the window fetches correctly and the picture
 *   lands in the wrong place", which is the ambiguity this bring-up has been
 *   stuck in.  This panel is RAM-LESS, so its line addressing comes entirely
 *   from the received stream, and that second possibility has never been tested.
 *
 *   Bars visible, even displaced or duplicated, means the data path works and
 *   the fault is in the video timing.  No bars at all means the data path is
 *   broken.  Clean blue means the layer contributed nothing.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP or the framebuffer is not up.
 *
 ****************************************************************************/

int rk3576_vop_coarse_pattern_probe(void);

/****************************************************************************
 * Name: rk3576_vop_post_bg_sequence_probe
 *
 * Description:
 *   With the layer turned off, steps the POST background through five
 *   unmistakable colours and asks the screen to repeat the order back.
 *
 *   This settles an assumption every other test here has been making: that a
 *   register write to the POST reaches the glass.  It stopped being safe to
 *   assume when it was found that the probes' "solid blue background" was
 *   actually programmed as SOLID GREEN -- this header had the background's
 *   blue and green fields swapped -- while a blue screen was reported anyway.
 *
 *   The order is logged only after the run, so reading the log cannot supply
 *   the answer.  If the colours appear in the logged order, this driver
 *   controls the panel.  If not, nothing written to the POST is reaching the
 *   glass and that has to be found first.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_post_bg_sequence_probe(void);

/****************************************************************************
 * Name: rk3576_vop_layer_axi_probe
 *
 * Description:
 *   Toggles this driver's three unsupported AXI deviations -- the window's
 *   hurry request, the window's outstanding bound, and the SYS-level axi0
 *   outstanding bound -- one per rung, with the eight colour bars sitting in the
 *   framebuffer as the oracle.
 *
 *   A working register dump has NONE of those three, and every one of them was
 *   added here on theory alone.  hurry_en with thold = 0 makes the TRM's hurry
 *   condition permanently true, so the window holds a priority request high for
 *   ever; the two outstanding bounds are caps the working configuration does not
 *   impose.  Any rung that makes the bars appear identifies its own culprit.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_layer_axi_probe(void);

/* The VOP's own error/event reporting, as latched by the hardware.
 *
 * This driver registers no IRQ handler, so these bits are the only way to
 * learn that the display pipeline itself is unhappy.  That matters because
 * the failure this board is chasing (a dark panel with a clean DSI link) has
 * two candidates that look identical from the DSI side:
 *
 *   bus_errpr      - a window's AXI read failed, so the compositor had no
 *                    pixel data.  The DSI still transmits, the panel sees
 *                    well-formed packets and reports no errors, and the
 *                    screen is black.
 *   post_buf_empty - the POST's output buffer underran, i.e. the hardware's
 *                    own statement that pixels did not arrive in time.
 *
 * Reading either of those as set changes the investigation completely, so
 * they are sampled during the live observation rather than assumed absent.
 */

struct rk3576_vop_int_s
{
  uint32_t sys0;      /* SYS0_INT_STATUS (AXI bus 0). */
  uint32_t sys1;      /* SYS1_INT_STATUS (AXI bus 1). */
  uint32_t vp_raw;    /* VP_INT_RAW_STATUS of the configured port. */
  uint32_t vp_status; /* VP_INT_STATUS of the configured port (masked). */

  /* The same bits, already decoded, so callers do not need the register
   * layout to interpret them. */

  bool bus_error;      /* A window's AXI read failed. */
  bool post_buf_empty; /* The POST output buffer underran. */
  bool frame_start;    /* The timing generator produced frame starts. */
  bool line_flag;      /* A line-flag position was reached. */
};

/****************************************************************************
 * Name: rk3576_vop_get_int_status
 *
 * Description:
 *   Read the VOP's latched interrupt state for the configured port.
 *
 *   RAW_STATUS is reported alongside STATUS on purpose: the raw register is
 *   not filtered by the interrupt-enable mask, and since this driver enables
 *   no VOP interrupts the masked register would read 0 whether or not
 *   anything happened.
 *
 * Input Parameters:
 *   status - Destination snapshot (must not be NULL).
 *   clear  - Also clear the latched bits, so the next call reports only what
 *            happened since this one (attribution per observation window).
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP has not been initialised.
 *
 ****************************************************************************/

int rk3576_vop_get_int_status(FAR struct rk3576_vop_int_s *status,
                              bool clear);

#endif /* CONFIG_RK3576_VOP */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_VOP_H */
