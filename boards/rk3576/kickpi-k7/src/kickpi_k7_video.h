/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_video.h
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
 * Board-level VOP / framebuffer ownership for the kickpi-k7 (RK3576).
 *
 * This file is the single place on this board that starts the RK3576 VOP and
 * registers the framebuffer.  The output drivers -- kickpi_k7_mipi_dsi.c and
 * kickpi_k7_hdmi.c -- own their interface, their PHY and their sequencing,
 * and call in here at the point their own protocol requires.
 *
 * ---------------------------------------------------------------------------
 * Why this is a helper and not a one-shot boot step
 * ---------------------------------------------------------------------------
 * The two outputs need the VOP started at OPPOSITE points in their bring-up,
 * and neither ordering is negotiable:
 *
 *   MIPI DSI:  panel ready -> VOP started -> DSI switched into video mode.
 *     Entering video mode makes the IPI timing generator count lines out of
 *     ipi_clk (= pixel clock / 4).  If that happens before a pixel clock
 *     exists, the generator waits for a line boundary that never comes and
 *     pixels pile up in ipi_data without a packet ever being built.
 *
 *   HDMI:      PHY powered on (pixel clock live) -> VOP started.
 *     On the HDMI path the HDPTX PHY's own PLL generates the pixel clock and
 *     the video port is reparented onto it (clk_hdmiphy_pixel0_o); the VOP
 *     has no clock at all until the PHY is running.
 *
 * So the VOP bring-up cannot simply be hoisted to the front of
 * board_late_initialize(): it has to stay inside each output's sequence.
 *
 * ---------------------------------------------------------------------------
 * One output at a time
 * ---------------------------------------------------------------------------
 * rk3576_vop.c currently supports a single output instance: it has one global
 * instance handle, it configures ESMART0 unconditionally
 *(RK3576_VOP_ESMART_IDX is a compile-time 0), it routes ESMART0 to video port
 *0 unconditionally, and it commits only the ESMART0 / VP0 groups.  Two outputs
 *would therefore fight over the same window, the same video port and the same
 *framebuffer handle.
 *
 * The board Kconfig expresses that as a choice (KICKPI_K7_MIPI_DSI or
 * KICKPI_K7_HDMI), and kickpi_k7_video_initialize() additionally refuses a
 * second call, so a misconfiguration fails loudly at boot instead of showing
 * whichever display happened to win.
 ****************************************************************************/

#ifndef __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_VIDEO_H
#define __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_VIDEO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "rk3576_vop.h"

#ifdef CONFIG_KICKPI_K7_VIDEO

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One display mode: the scan-out timing plus the routing that carries it to
 * an output interface.  Everything here is fixed for the life of the
 * framebuffer -- rk3576_vop_initialize() copies it into the driver instance
 * and there is no runtime mode-set path.
 *
 * Each output driver declares its own const instance of this struct, so the
 * timing that the interface is programmed with and the timing the VOP scans
 * out with come from one place and cannot drift apart.
 */

struct kickpi_k7_video_mode_s
{
  const char *name; /* Mode name, for the boot log */

  /* Visible geometry.  The framebuffer is allocated as xres * yres * 3
   * (RGB888), so this also fixes the memory footprint.
   */

  uint16_t xres;
  uint16_t yres;

  /* Scan-out timing, in pixels / lines.  For a panel that has no frame
   * memory these must be exactly what the panel was programmed with; for
   * HDMI they are the CEA-861 mode timing.
   */

  uint16_t hsync_len;
  uint16_t hfront_porch;
  uint16_t hback_porch;
  uint16_t vsync_len;
  uint16_t vfront_porch;
  uint16_t vback_porch;

  /* Nominal pixel clock of the mode, in Hz.  See the long note on
   * rk3576_vop_config.pixel_clock: this is the value the output interface
   * derives its own timing from, and on the DSI path it must be a rate the
   * CRU can divide exactly.
   */

  uint32_t pixel_clock;

  /* Routing: which SYS_CTRL_*_INFACE_CTRL register is enabled (iface) and
   * which video port feeds it (port).  The port also selects the POST block,
   * the OVERLAY port and dclk_vpN, so it must match what the output driver
   * assumed when it configured its own timing.
   */

  enum rk3576_vop_iface_e iface;
  enum rk3576_vop_port_e port;

  /* Colour the framebuffer is painted with before the stream starts, so the
   * first frames a sink receives are recognisably this driver's output rather
   * than uninitialised memory.  0xRRGGBB.
   */

  uint32_t fill_rgb;
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
 * Name: kickpi_k7_video_initialize
 *
 * Description:
 *   Bring up the VOP for one display mode and register the framebuffer as
 *   /dev/fbN.
 *
 *   Call this from the output driver at the point that output's protocol
 *   requires (see the file banner); it is not a standalone boot step.
 *
 * Input Parameters:
 *   mode - The mode to scan out.  Must not be NULL and must stay valid for
 *          the call's duration only (it is copied).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure:
 *     -EINVAL  mode is NULL or carries a zero pixel clock
 *     -EBUSY   the VOP has already been claimed by another output
 *     otherwise whatever rk3576_vop_initialize() returned
 *
 * Assumptions:
 *   The output interface's pixel clock source is already running: for MIPI
 *   DSI that means the CRU divider is set (the VOP programs it), for HDMI it
 *   means rk3576_hdmi_enable() has already powered the PHY's PLL.
 *
 ****************************************************************************/

int kickpi_k7_video_initialize(FAR const struct kickpi_k7_video_mode_s *mode);

/****************************************************************************
 * Name: kickpi_k7_video_is_initialized
 *
 * Description:
 *   Report whether kickpi_k7_video_initialize() has claimed the VOP.
 *
 *   Diagnostic only; worth logging because "the display came up but the
 *   framebuffer was never registered" and "the framebuffer exists but the
 *   interface is not transmitting" look identical from the console.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   true if the VOP has been brought up.
 *
 ****************************************************************************/

bool kickpi_k7_video_is_initialized(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* CONFIG_KICKPI_K7_VIDEO */
#endif /* __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_VIDEO_H */
