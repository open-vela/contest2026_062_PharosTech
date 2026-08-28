/****************************************************************************
 * chips/rk3576/hardware/rk3576_vop.h
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
 * RK3576 Video Output Processor (VOP) hardware register definitions.
 *
 * Reference: Rockchip RK3576 TRM, Part 2, Chapter 11 "VOP_LITE".
 *
 * The RK3576 VOP is a 3-video-port + 3-POST display processor.  Pixel data
 * is fetched from DDR through the CLUSTER (layers), composed in the
 * OVERLAY blocks, timed by the POST blocks, and finally routed to one of
 * the physical output interfaces (MIPI DSI / HDMI / eDP / DP / RGB) through
 * the per-interface SYS_CTRL_*_INFACE_CTRL mux registers.
 *
 * This minimal driver uses only CLUSTER0_WIN0 (single RGB layer), one
 * video port (PORT0/1/2 -> POST0/1/2) and one output interface.  No MMU,
 * no ESMART, no compression, no blending - the bare minimum to scan out a
 * single 24-bit RGB framebuffer.
 *
 * All register offsets below are relative to RK3576_VOP_ADDR = 0x27D00000.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VOP_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VOP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* -----------------------------------------------------------------------
 * Internal address map (TRM 11.5.1, Table 11-6).
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_SYS_CTRL_OFFSET       0x0000 /* System control (512B) */
#define RK3576_VOP_OVERLAY_SYSTEM_OFFSET 0x0500 /* Overlay system */
#define RK3576_VOP_OVERLAY_PORT0_OFFSET  0x0600 /* Overlay port 0 */
#define RK3576_VOP_OVERLAY_PORT1_OFFSET  0x0700 /* Overlay port 1 */
#define RK3576_VOP_OVERLAY_PORT2_OFFSET  0x0800 /* Overlay port 2 */
#define RK3576_VOP_POST0_OFFSET          0x0C00 /* POST (video port 0) */
#define RK3576_VOP_POST1_OFFSET          0x0D00 /* POST (video port 1) */
#define RK3576_VOP_POST2_OFFSET          0x0E00 /* POST (video port 2) */
#define RK3576_VOP_CLUSTER0_OFFSET       0x1000 /* Cluster 0 (512B) */
#define RK3576_VOP_CLUSTER1_OFFSET       0x1200 /* Cluster 1 (512B) */

/* -----------------------------------------------------------------------
 * SYS_CTRL registers (base + RK3576_VOP_SYS_CTRL_OFFSET).
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_SYS_CFG_DONE     0x0000 /* Register configure done */
#define RK3576_VOP_SYS_WIN_CFG_DONE 0x0004 /* Layer register cfg done */
#define RK3576_VOP_SYS_CORE_ID      0x0008 /* Core identification */

/* Per-interface output control blocks.  Every interface exposes an
 * identical register layout: *_port_sel[3:2], *_out_en[0], ...
 * The driver maps a logical interface id to one of these offsets.
 */

#define RK3576_VOP_MIPI0_INFACE_CTRL 0x0180 /* MIPI DSI iface ctrl */
#define RK3576_VOP_HDMI0_INFACE_CTRL 0x0184 /* HDMI iface ctrl */
#define RK3576_VOP_EDP0_INFACE_CTRL  0x0188 /* eDP iface ctrl */
#define RK3576_VOP_DP0_INFACE_CTRL   0x018C /* DP iface ctrl */
#define RK3576_VOP_RGB_INFACE_CTRL   0x0194 /* RGB iface ctrl */
#define RK3576_VOP_DP1_INFACE_CTRL   0x0198 /* DP1 iface ctrl */
#define RK3576_VOP_DP2_INFACE_CTRL   0x019C /* DP2 iface ctrl */

/* Interface ctrl register field shifts/widths (common to all interfaces). */

#define RK3576_VOP_IFACE_OUT_EN            (1 << 0) /* Interface output enable */
#define RK3576_VOP_IFACE_CLK_OUT_EN        (1 << 1) /* Pixel clock output enable */
#define RK3576_VOP_IFACE_PORT_SEL_SHIFT    2        /* Video port select */
#define RK3576_VOP_IFACE_PORT_SEL_MASK     (0x3 << RK3576_VOP_IFACE_PORT_SEL_SHIFT)
#define RK3576_VOP_IFACE_VSYNC_POL         (1 << 4) /* VSYNC polarity (1=pos) */
#define RK3576_VOP_IFACE_HSYNC_POL         (1 << 5) /* HSYNC polarity (1=pos) */
#define RK3576_VOP_IFACE_SPLIT_EN          (1 << 8) /* Split mode enable */
#define RK3576_VOP_IFACE_DATA1_SEL         (1 << 9) /* Use data1 of video port */
#define RK3576_VOP_IFACE_PIX_CLK_SEL_SHIFT 20 /* Pixel clock divider sel */
#define RK3576_VOP_IFACE_DCLK_SEL          (1 << 21) /* 0=dclk core, 1=dclk out */

/* -----------------------------------------------------------------------
 * CLUSTER0_WIN0 registers (base + RK3576_VOP_CLUSTER0_OFFSET).
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_WIN0_CTRL0    0x0000 /* Window config */
#define RK3576_VOP_WIN0_CTRL1    0x0004 /* Window config 1 */
#define RK3576_VOP_WIN0_CTRL2    0x0008 /* Window config 2 */
#define RK3576_VOP_WIN0_YRGB_MST 0x0010 /* YRGB fb start address */
#define RK3576_VOP_WIN0_CBCR_MST 0x0014 /* CbCr fb start address */
#define RK3576_VOP_WIN0_VIR      0x0018 /* Virtual stride */
#define RK3576_VOP_WIN0_ACT_INFO 0x0020 /* Active region (w-1,h-1) */
#define RK3576_VOP_WIN0_DSP_INFO 0x0024 /* Display region (w-1,h-1) */
#define RK3576_VOP_WIN0_DSP_ST   0x0028 /* Display start (x,y) */

/* WIN0_CTRL0 field definitions. */

#define RK3576_VOP_WIN0_EN             (1 << 0) /* Layer enable */
#define RK3576_VOP_WIN0_DATA_FMT_SHIFT 1        /* Pixel format */
#define RK3576_VOP_WIN0_DATA_FMT_MASK  (0x3f << RK3576_VOP_WIN0_DATA_FMT_SHIFT)
#define RK3576_VOP_WIN0_CSC_Y2R_EN     (1 << 8)  /* CSC Y2R enable */
#define RK3576_VOP_WIN0_CSC_MODE_SHIFT 9         /* CSC mode (3-bit) */
#define RK3576_VOP_WIN0_RB_SWAP        (1 << 15) /* Swap R and B */
#define RK3576_VOP_WIN0_RG_SWAP        (1 << 16) /* Swap R and G */
#define RK3576_VOP_WIN0_UV_SWAP        (1 << 17) /* Swap U and V */
#define RK3576_VOP_WIN0_DITHER_UP_EN   (1 << 18) /* Dither up */

/* win0_data_fmt values (CTRL0[6:1]). */

#define RK3576_VOP_WIN0_FMT_ARGB8888 (0x00 << RK3576_VOP_WIN0_DATA_FMT_SHIFT)
#define RK3576_VOP_WIN0_FMT_RGB888   (0x01 << RK3576_VOP_WIN0_DATA_FMT_SHIFT)
#define RK3576_VOP_WIN0_FMT_RGB565   (0x02 << RK3576_VOP_WIN0_DATA_FMT_SHIFT)

/* -----------------------------------------------------------------------
 * OVERLAY_PORTx registers (base + RK3576_VOP_OVERLAY_PORTx_OFFSET).
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_OVERLAY_CTRL        0x0000 /* Overlay ctrl config */
#define RK3576_VOP_OVERLAY_LAYER_SEL   0x0004 /* Overlay layer select */
#define RK3576_VOP_OVERLAY_BG_MIX_CTRL 0x0070 /* Background mix ctrl */

/* -----------------------------------------------------------------------
 * POSTx_CTRL registers (base + RK3576_VOP_POSTx_OFFSET).
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_POST_DSP_CTRL          0x0000 /* DSP ctrl (standby/out mode) */
#define RK3576_VOP_POST_MIPI_CTRL         0x0004 /* MIPI te/double channel */
#define RK3576_VOP_POST_CORE_CLK          0x000C /* Core clock select */
#define RK3576_VOP_POST_DSP_BG            0x002C /* Background color */
#define RK3576_VOP_POST_DSP_HACT_INFO     0x0034 /* H active start/end (post) */
#define RK3576_VOP_POST_DSP_VACT_INFO     0x0038 /* V active start/end (post) */
#define RK3576_VOP_POST_DSP_HTOTAL_HS_END 0x0048 /* H total + hsync end */
#define RK3576_VOP_POST_DSP_HACT_ST_END   0x004C /* H active start/end */
#define RK3576_VOP_POST_DSP_VTOTAL_VS_END 0x0050 /* V total + vsync end */
#define RK3576_VOP_POST_DSP_VACT_ST_END   0x0054 /* V active start/end */

/* POST_DSP_CTRL field definitions. */

#define RK3576_VOP_POST_STANDBY_EN     (1 << 31) /* Standby mode */
#define RK3576_VOP_POST_DSP_OUT_ZERO   (1 << 15) /* Output zero */
#define RK3576_VOP_POST_BLACK_EN       (1 << 16) /* Output black */
#define RK3576_VOP_POST_BLANK_EN       (1 << 17) /* Blank hs/vs/den */
#define RK3576_VOP_POST_DITHER_DOWN_EN (1 << 20) /* Dither down */

/* dsp_out_mode values (POST_DSP_CTRL[3:0]). */

#define RK3576_VOP_POST_OUT_MODE_SHIFT 0
#define RK3576_VOP_POST_OUT_MODE_MASK  (0xf << RK3576_VOP_POST_OUT_MODE_SHIFT)
#define RK3576_VOP_POST_OUT_RGB888     (0x0 << RK3576_VOP_POST_OUT_MODE_SHIFT)
#define RK3576_VOP_POST_OUT_RGB666     (0x1 << RK3576_VOP_POST_OUT_MODE_SHIFT)
#define RK3576_VOP_POST_OUT_RGB565     (0x2 << RK3576_VOP_POST_OUT_MODE_SHIFT)

/* POST_DSP_BG field definitions. */

#define RK3576_VOP_POST_BG_DISPLAY_EN  (1 << 31) /* BG display en */
#define RK3576_VOP_POST_BG_RED_SHIFT   20        /* 10-bit red */
#define RK3576_VOP_POST_BG_GREEN_SHIFT 10        /* 10-bit green */
#define RK3576_VOP_POST_BG_BLUE_SHIFT  0         /* 10-bit blue */

/* -----------------------------------------------------------------------
 * Macros to build full register addresses from a VOP base address.
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_SYS_CTRL(base) ((base) + RK3576_VOP_SYS_CTRL_OFFSET)
#define RK3576_VOP_OVERLAY_PORT(base, p) \
  ((base) + RK3576_VOP_OVERLAY_PORT0_OFFSET + ((p) << 8))
#define RK3576_VOP_POST(base, p) \
  ((base) + RK3576_VOP_POST0_OFFSET + ((p) << 8))
#define RK3576_VOP_CLUSTER(base) ((base) + RK3576_VOP_CLUSTER0_OFFSET)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VOP_H */
