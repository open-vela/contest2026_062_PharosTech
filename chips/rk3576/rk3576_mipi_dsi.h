/****************************************************************************
 * chips/rk3576/rk3576_mipi_dsi.h
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
 * RK3576 MIPI DSI-2 host controller driver public interface.
 *
 * Implements the NuttX generic MIPI DSI framework (struct mipi_dsi_host /
 * struct mipi_dsi_host_ops) on top of the RK3576 DSI-2 controller.  The
 * board layer calls rk3576_mipi_dsi_initialize() to obtain the host, then
 * optionally registers it with mipi_dsi_host_register() so that LCD panel
 * drivers can bind to it via mipi_dsi_device.
 *
 * The DSI driver owns the whole link lifecycle, including the DCPHY: board
 * code never talks to the PHY directly.  rk3576_mipi_dsi_initialize()
 * brings up the DCPHY and enters Command mode, so the panel DCS init
 * sequence can be transmitted over the (live) D-PHY lanes immediately.
 * rk3576_mipi_dsi_enable_video() then programs the IPI (Image Pixel
 * Interface) timing and transitions the host to Video mode, where pixel
 * data from the VOP is streamed to the panel.
 *
 * Command-mode transfer (DCS init / generic read-write) is handled through
 * the DSI-2 Command Interface (CRI) register bank and is usable in both
 * Command and Video operating states.
 *
 * The VOP (video processor) that feeds the IPI is outside the scope of
 * this driver and is wired up by a separate framebuffer/VOP driver.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/video/mipi_dsi.h>

#ifdef CONFIG_RK3576_MIPI_DSI

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* DSI-2 host one-time configuration (passed to
 * rk3576_mipi_dsi_initialize()).  These describe the PHY/link and are
 * fixed for the life of the driver instance.
 */

struct rk3576_dsi_config
{
  uint8_t lanes;      /* D-PHY data lanes in use (1..4). */
  uint8_t format;     /* MIPI_DSI_FMT_* pixel format (RGB888/666/565). */
  uint8_t video_mode; /* DSI2_VID_MODE_* video-mode packet type. */
  bool continuous_clk; /* true = clock lane stays in HS (continuous) for
                         * panels that keep HSCM across the whole frame.
                         * ILI9881D is NON-continuous: its clock lane
                         * returns to LP-11 after every HS burst (Table 46
                         * THS-EXIT), so it must be false for that panel. */
  uint32_t hs_rate;   /* Requested lane high-speed data rate in Hz
                       * (80 Mbps .. 2.5 Gbps). */
};

/* Video-mode timing, programmed into the DSI-2 IPI timing registers by
 * rk3576_mipi_dsi_enable_video().  Naming follows drm_display_mode for
 * consistency with the VOP integration.
 */

struct rk3576_dsi_video_timing
{
  uint32_t hactive;      /* Horizontal active pixels. */
  uint32_t hfront_porch; /* Horizontal front porch (HFP). */
  uint32_t hback_porch;  /* Horizontal back porch (HBP). */
  uint32_t hsync_len;    /* Horizontal sync width (HSA). */
  uint32_t vactive;      /* Vertical active lines. */
  uint32_t vfront_porch; /* Vertical front porch (VFP). */
  uint32_t vback_porch;  /* Vertical back porch (VBP). */
  uint32_t vsync_len;    /* Vertical sync width (VSA). */
  uint32_t pixel_clock;  /* Pixel clock in Hz. */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mipi_dsi_initialize
 *
 * Description:
 *   Initialize the RK3576 DSI-2 host controller and return the host to be
 *   registered with the NuttX MIPI DSI framework.
 *
 *   The RK3576 has a single DSI-2 host, so the driver is a singleton.
 *   The DCPHY must have been initialized beforehand (rk3576_dcphy_init()).
 *
 * Input Parameters:
 *   config - Link/PHY configuration (lanes, format, video mode, hs_rate).
 *            Must not be NULL.
 *
 * Returned Value:
 *   A non-NULL struct mipi_dsi_host * on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct mipi_dsi_host *
rk3576_mipi_dsi_initialize(FAR const struct rk3576_dsi_config *config);

/****************************************************************************
 * Name: rk3576_mipi_dsi_enable_video
 *
 * Description:
 *   Program the DSI-2 controller for Video mode with the given panel
 *   timing, then power on the DCPHY and switch the host into Video mode.
 *   This must be called after the DCS init sequence (if any) has been
 *   sent and the panel is ready to receive pixel data.
 *
 * Input Parameters:
 *   host   - DSI host returned by rk3576_mipi_dsi_initialize().
 *   timing - Panel video timing.  Must not be NULL.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mipi_dsi_enable_video(
    FAR struct mipi_dsi_host *host,
    FAR const struct rk3576_dsi_video_timing *timing);

/****************************************************************************
 * Name: rk3576_mipi_dsi_disable_video
 *
 * Description:
 *   Put the DSI-2 host back into Idle mode and power off the DCPHY.
 *
 * Input Parameters:
 *   host - DSI host returned by rk3576_mipi_dsi_initialize().
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mipi_dsi_disable_video(FAR struct mipi_dsi_host *host);

/****************************************************************************
 * Name: rk3576_mipi_dsi_update_pixel_clock
 *
 * Description:
 *   Re-derive the IPI horizontal timing (HSA/HBP/HACT/HLINE) and
 *   PHY_IPI_RATIO from the VOP's REAL pixel clock, once the CRU divider
 *   chain has settled.
 *
 *   The board passes a NOMINAL pixel clock (64 MHz for kickpi-k7) to
 *   enable_video(), but the CRU can only select an integer divider from the
 *   parent PLL, so the real dclk_vp0 usually differs slightly (gpll
 *   1188 MHz / 19 = 62.526 MHz here).  Programming the DSI from the nominal
 *   value leaves the controller comparing the incoming pixel stream against
 *   a timing/ratio that does not match the real clock, leaving the
 *   IPI<->PHY CDC handshake at the edge of its tolerance (intermittent:
 *   the same firmware behaves differently run to run).
 *
 *   Called by rk3576_vop_enable_clocks() right after clk_set_rate(dclk).
 *   Safe to call at any time once the host is in Video mode: the IPI timing
 *   registers are plain RW in manual mode and take effect without leaving
 *   Video mode.
 *
 * Input Parameters:
 *   pixel_clock_hz - Actual VOP pixel clock (crtc clock) in Hz.
 *
 * Returned Value:
 *   OK on success; -EPERM if the host is not in Video mode; -EINVAL on a
 *   bad clock or timing overflow.
 *
 ****************************************************************************/

int rk3576_mipi_dsi_update_pixel_clock(uint32_t pixel_clock_hz);

/****************************************************************************
 * Name: rk3576_mipi_dsi_dump_video_status
 *
 * Description:
 *   Bring-up diagnostic (read-only): sample the DSI IPI receive path while
 *   the VOP is scanning, to answer whether pixels physically reach the DSI
 *   IPI.  Must be called AFTER rk3576_vop_initialize() has started the
 *   pixel stream (the enable_video() dump runs before the VOP is up, so its
 *   empty ipi_data FIFO / INIT ipi_vid_fsm are expected, not diagnostic).
 *
 *   A positive ipi_data FIFO count or ipi_busy=1 proves the pixel stream
 *   reached the IPI (break is downstream in the PHY send path); all-zero
 *   again proves the break is upstream (VOP -> DSI physical link).
 *
 ****************************************************************************/

void rk3576_mipi_dsi_dump_video_status(void);

#endif /* CONFIG_RK3576_MIPI_DSI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H */
