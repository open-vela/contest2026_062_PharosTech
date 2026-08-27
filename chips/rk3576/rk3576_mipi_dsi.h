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
 * The primary operating mode is Video mode: the host is configured to
 * stream pixel data received from the VOP through the IPI (Image Pixel
 * Interface) with the panel timing programmed via
 * rk3576_mipi_dsi_enable_video().  Command-mode transfer (DCS init /
 * generic read-write) is handled through the DSI-2 Command Interface (CRI)
 * register bank and remains available as a secondary, "future support"
 * path (it is also used at panel bring-up to send the DCS init sequence).
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

#endif /* CONFIG_RK3576_MIPI_DSI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H */
