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

#endif /* CONFIG_RK3576_VOP */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_VOP_H */
