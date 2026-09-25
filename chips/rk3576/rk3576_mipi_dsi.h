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
  uint8_t lanes;       /* D-PHY data lanes in use (1..4). */
  uint8_t format;      /* MIPI_DSI_FMT_* pixel format (RGB888/666/565). */
  uint8_t video_mode;  /* DSI2_VID_MODE_* video-mode packet type. */
  bool continuous_clk; /* true = clock lane stays in HS (continuous) for
                        * panels that keep HSCM across the whole frame.
                        * ILI9881D is NON-continuous: its clock lane
                        * returns to LP-11 after every HS burst (Table 46
                        * THS-EXIT), so it must be false for that panel. */
  uint32_t hs_rate;    /* Requested lane high-speed data rate in Hz
                        * (80 Mbps .. 2.5 Gbps). */
  bool eotp;           /* true = transmit EoTp at the end of each HS burst
                        * (DSI2_DSI_GENERAL_CFG.eotp_tx_en).
                        *
                        * Either value is valid: EoTp is optional in D-PHY
                        * and changes what a receiver sees at the end of every
                        * HS burst, so the board decides it per panel.  A
                        * configuration that sets MIPI_DSI_MODE_EOT_PACKET
                        * disables EoTp in HS mode; boards that do not set it
                        * get EoTp enabled.
                        *
                        * Zero-initialised rk3576_dsi_config therefore means
                        * "no EoTp". */
};

/* Video-mode timing, programmed into the DSI-2 IPI timing registers by
 * rk3576_mipi_dsi_enable_video().  The field names follow the usual display
 * mode naming (hactive / hfront_porch / ...) for consistency with the VOP
 * integration.
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
 *   The board passes a pixel clock to enable_video(), but the CRU can only
 *   select an integer divider from the parent PLL, and the divider search
 *   picks the largest divisor whose output is still <= the request.  So the
 *   achieved dclk_vp0 can be several percent BELOW the request, and then the
 *   IPI horizontal timing and PHY_IPI_RATIO are computed against a clock the
 *   pixel stream never runs at (the controller would judge "one line ended"
 *   at the wrong instant and mis-size its CDC ratio).
 *
 *   The board therefore requests a value the CRU hits EXACTLY (see
 *   KICKPI_K7_MIPI_DSI_PIXCLK), and rk3576_vop_enable_clocks() logs a
 *   warning if the achieved rate still differs from the request.  This
 *   function remains the escape hatch for the case where an exact request is
 *   impossible.
 *
 *   NOT called from rk3576_vop_enable_clocks() any more: doing so rewrote the
 *   horizontal timing and the CDC ratio of the pixel datapath asynchronously
 *   WHILE the stream was live, which is a race (see the long note in that
 *   function).  If it must be called, call it before the host enters Video
 *   mode.
 *
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

/* DSI2_INT_ST_IPI bits.  err_ipi_dtype is the one worth naming: the TRM says
 * it fires when "no DSI-2 data type is a direct match for the choice made by
 * the pair ipi_format and ipi_depth", after which the controller SILENTLY
 * falls back to Packed Pixel Stream 24-bit.  That is a pixel-format fault that
 * produces perfectly legal-looking traffic, so it can only be seen by reading
 * this latch -- and the driver never did. */

#define RK3576_DSI_INT_IPI_ERR_DTYPE    (1u << 0)
#define RK3576_DSI_INT_IPI_ERR_CMD_TIME (1u << 1)
#define RK3576_DSI_INT_IPI_ERR_CMD_OVFL (1u << 2)

/* DSI2_INT_ST_MAIN summary bits (bit n = "group n latched something"). */

#define RK3576_DSI_INT_MAIN_PHY  (1u << 0)
#define RK3576_DSI_INT_MAIN_TO   (1u << 1)
#define RK3576_DSI_INT_MAIN_ACK  (1u << 2)
#define RK3576_DSI_INT_MAIN_IPI  (1u << 3)
#define RK3576_DSI_INT_MAIN_FIFO (1u << 4)
#define RK3576_DSI_INT_MAIN_PRI  (1u << 5)
#define RK3576_DSI_INT_MAIN_CRI  (1u << 6)

#endif /* CONFIG_RK3576_MIPI_DSI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H */
