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
#define RK3576_VOP_ESMART0_OFFSET        0x1800 /* Esmart 0 (512B) */
#define RK3576_VOP_ESMART1_OFFSET        0x1A00 /* Esmart 1 (512B) */
#define RK3576_VOP_ESMART2_OFFSET        0x1C00 /* Esmart 2 (512B) */
#define RK3576_VOP_ESMART3_OFFSET        0x1E00 /* Esmart 3 (512B) */

/* -----------------------------------------------------------------------
 * SYS_CTRL registers (base + RK3576_VOP_SYS_CTRL_OFFSET).
 * ----------------------------------------------------------------------- */

/* Register-configure-done (mirror -> real) trigger.  All layer/POST config
 * is written to mirror registers and only copied to the real registers at
 * the start of the next frame after this regdone pulse.  Uses the
 * hiword-mask write scheme (bits [31:16] = write-enable per bit).
 *   VP0 cfg_done      = bit 0
 *   VP0 sys_cfg_done  = bit 4
 */

#define RK3576_VOP_SYS_REG_CFG_DONE     0x0000 /* Register configure done */
#define RK3576_VOP_SYS_WIN_REG_CFG_DONE 0x000C /* Layer register cfg done */
#define RK3576_VOP_SYS_CORE_ID          0x0008 /* Core identification */

/* Trigger both VP0 global register loads (cfg_done + sys_cfg_done). */

#define RK3576_VOP_CFG_DONE_LOAD_CTRL   (0xffff << 16) /* hiword write-enable */
#define RK3576_VOP_CFG_DONE_VP0         (1 << 0)       /* VP0 cfg done */
#define RK3576_VOP_CFG_DONE_VP0_SYS     (1 << 4)       /* VP0 sys cfg done */
#define RK3576_VOP_CFG_DONE_TRIGGER                            \
  (RK3576_VOP_CFG_DONE_LOAD_CTRL | RK3576_VOP_CFG_DONE_VP0 |   \
   RK3576_VOP_CFG_DONE_VP0_SYS)

/* SYS_REG_CFG_DONE (0x0000) load-enable bits (mirror -> real).  Load every
 * group so the POST DSP/timing registers land whichever group they belong:
 *   bit 0 reg_load_global0_en   (VP0)
 *   bit 1 reg_load_global1_en   (VP1)
 *   bit 2 reg_load_global2_en   (VP2)
 *   bit 4 reg_load_sys0_en
 *   bit 5 reg_load_sys1_en
 *   bit 6 reg_load_sys2_en
 */

#define RK3576_VOP_CFG_DONE_ALL_GROUPS                          \
  ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 6))

/* SYS_REG_CFG_DONE (0x0000) bit 15 = sw_global_regdone_en.
 *
 * TRM: reset value 1, "Global regdone enable.  1'b0: Disable  1'b1: Enable".
 *
 * It lives inside the ordinary data (low 16 bits), so a write of
 *   CFG_DONE_LOAD_CTRL | CFG_DONE_ALL_GROUPS
 * (which enables the hiword write-mask for ALL 16 low bits) also overwrites
 * bit 15 with whatever the data word carries -- writing 0 there silently
 * DISABLES the global regdone.  Observed consequence: the VP0 *global*
 * group's mirror->real copy never latches, i.e. RAW readback of
 * SYS_REG_CFG_DONE shows bit0 (reg_load_global0_en) still pending while
 * bit4 (reg_load_sys0_en) has already been consumed by the frame boundary.
 * Linux keeps this bit set in vop2_cfg_done().
 *
 * Always OR this into the cfg_done data word.
 */

#define RK3576_VOP_CFG_DONE_GLOBAL_REGDONE_EN (1 << 15)

/* SYS_CTRL_SYS_WIN_REG_CFG_DONE (0x000C) layer mirror->real load enables.
 * The ESMART/CLUSTER layer registers are written into mirror registers and
 * only take effect when their per-layer load bit is pulsed here (hiword
 * mask in [31:16], enable bits in [7:0]):
 *   bit 0  reg_load_cluster0_en
 *   bit 1  reg_load_cluster1_en
 *   bit 4  reg_load_esmart0_en
 *   bit 5  reg_load_esmart1_en
 *   bit 6  reg_load_esmart2_en
 *   bit 7  reg_load_esmart3_en
 */

#define RK3576_VOP_WIN_CFG_DONE_LOAD_CTRL (0xffff << 16)
#define RK3576_VOP_WIN_CFG_DONE_CLUSTER0  (1 << 0)
#define RK3576_VOP_WIN_CFG_DONE_ESMART0   (1 << 4)

/* SYS_CTRL status / vsync registers (read-only diagnostics).
 *   SYS_CTRL_SYS_STATUS0 @ 0x0060: dsp_vcnt0[28:16] = video output0
 *     vertical counter.  Readbacks 0 when the VP0 scan state machine is
 *     not running; it increments every line when the frame is scanning.
 *   SYS_CTRL_VOP_IO_VSYNC_CTRL @ 0x004C: vsync-to-IO routing sel
 *     (vop_io_vp0_vsync_sel[1:0]).
 */

#define RK3576_VOP_SYS_STATUS0         0x0060
#define RK3576_VOP_SYS_VSYNC_CTRL      0x004C
#define RK3576_VOP_DSP_VCNT0_SHIFT     16
#define RK3576_VOP_DSP_VCNT0_MASK      (0x1fff << RK3576_VOP_DSP_VCNT0_SHIFT)

/* SYS_CTRL AXI throughput control (IMD = immediate, applies without a
 * cfg_done pulse).  Each AXI channel caps the number of outstanding
 * transactions the VOP may issue, so its scan-out DMA cannot monopolise
 * the NoC and starve the CPU.
 *
 *   SYS_AXI0_CTRL_IMD @ 0x0010, SYS_AXI1_CTRL_IMD @ 0x001C:
 *   bit9:4 outstanding_num, bit1 dma_stop, bit0 outstanding_en.
 *   ESMART uses axi0 (reset esmart_axi_sel=0), so limit axi0.
 *
 *   As with the ESMART AXI control, outstanding_en=0 at reset means
 *   *unlimited*; it must be set to 1 to actually bound the bursts.
 */

#define RK3576_VOP_SYS_AXI0_CTRL_IMD    0x0010
#define RK3576_VOP_SYS_AXI1_CTRL_IMD    0x001C
#define RK3576_VOP_SYS_AXI_OUTSTANDING_EN (1 << 0)
#define RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT 4
#define RK3576_VOP_SYS_AXI_OUTSTANDING_MASK (0x3f << RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT)
#define RK3576_VOP_SYS_AXI_OUTSTANDING(n) \
  (((uint32_t)(n) << RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT) & \
   RK3576_VOP_SYS_AXI_OUTSTANDING_MASK)

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
#define RK3576_VOP_IFACE_HSYNC_POL         (1 << 4) /* HSYNC polarity (1=pos) */
#define RK3576_VOP_IFACE_VSYNC_POL         (1 << 5) /* VSYNC polarity (1=pos) */
#define RK3576_VOP_IFACE_SPLIT_EN          (1 << 8) /* Split mode enable */
#define RK3576_VOP_IFACE_DATA1_SEL         (1 << 9) /* Use data1 of video port */
#define RK3576_VOP_IFACE_CMD_MODE          (1 << 11) /* 1=Command, 0=Video */
#define RK3576_VOP_IFACE_PIX_CLK_SEL_SHIFT 20 /* Pixel clock divider sel */
#define RK3576_VOP_IFACE_PIX_CLK_SEL       (1 << 20) /* 0=div2, 1=div4 */
#define RK3576_VOP_IFACE_DCLK_SEL          (1 << 21) /* 0=dclk core, 1=dclk out */
#define RK3576_VOP_IFACE_REGDONE_IMD_EN    (1 << 31) /* regdone immediate en */

/* Reset-default polarity bits that must be preserved: the interface
 * control register reset to vsync_pol=1 (Positive) + hsync_pol=1
 * (Positive) + regdone_imd_en=1.  Writing the whole word resets these, so
 * config must read-modify-write. */

#define RK3576_VOP_IFACE_POL_MASK                                           \
  (RK3576_VOP_IFACE_VSYNC_POL | RK3576_VOP_IFACE_HSYNC_POL)

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
#define RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL 0x0020 /* Mixer0 src color ctrl */
#define RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL 0x0024 /* Mixer0 dst color ctrl */
#define RK3576_VOP_OVERLAY_MIX0_SRC_ALPHA_CTRL 0x0028 /* Mixer0 src alpha ctrl */
#define RK3576_VOP_OVERLAY_MIX0_DST_ALPHA_CTRL 0x002C /* Mixer0 dst alpha ctrl */
#define RK3576_VOP_OVERLAY_BG_MIX_CTRL 0x0070 /* Background mix ctrl */

/* OVERLAY_PORTx_LAYER_SEL field definitions.
 * Four logical layers (layer0..3), each a 4-bit select of the window that
 * feeds that mixer input.  Reset value = 0xffff (all layers Disable), so a
 * layer MUST be explicitly routed before any window data reaches the POST.
 */

/* OVERLAY_PORTx_MIX0_* blend register field definitions (TRM 11.5.3.4).
 *
 * Each mix stage blends one logical layer onto the accumulation buffer
 * using the standard Porter-Duff formula:
 *
 *   Cd = factor_src(Cs) OP factor_dst(Cd)
 *
 * where the alpha/factor modes are:
 *   factor_mode  3'b000=0, 3'b001=256, 3'b010=Ad0, 3'b011=256-Ad0,
 *                3'b100=As0, 3'b101=Ags
 *
 * CRITICAL: the hardware reset value of every *_FACTOR_MODE field is 0
 * (factor = 0) and GLB_ALPHA is 0x00, so an un-configured mixer multiplies
 * both source and destination by zero -> the layer is silently dropped and
 * only POST's background colour (also black at reset) reaches the output.
 * A passthrough layer MUST program the SOURCE factor to Ags(=Ags with
 * glb_alpha=0xff = fully opaque) and the DESTINATION factor to the inverse,
 * exactly matching Linux vop2_setup_alpha() / vop2_parse_alpha() for an
 * alpha-less (RGB888) bottom layer.
 */

/* src/dst COLOR_CTRL field bits (0x20 / 0x24). */

#define RK3576_VOP_MIX_CTRL_GLB_ALPHA_SHIFT 16 /* src: [31:24], dst: [23:16] */
#define RK3576_VOP_MIX_CTRL_ALPHA_EN       (1 << 8)  /* alpha blending enable */
#define RK3576_VOP_MIX_CTRL_FACTOR_SHIFT   5
#define RK3576_VOP_MIX_CTRL_FACTOR_MASK    (0x7 << RK3576_VOP_MIX_CTRL_FACTOR_SHIFT)
#define RK3576_VOP_MIX_CTRL_ALPHA_CAL_MODE (1 << 4)  /* 1=no saturation */
#define RK3576_VOP_MIX_CTRL_BLEND_SHIFT    2
#define RK3576_VOP_MIX_CTRL_BLEND_MASK     (0x3 << RK3576_VOP_MIX_CTRL_BLEND_SHIFT)
#define RK3576_VOP_MIX_CTRL_ALPHA_MODE     (1 << 1)  /* 1=255-As (inverse) */
#define RK3576_VOP_MIX_CTRL_COLOR_MODE     (1 << 0)  /* 1=Cs*As0 (pre-mul) */

/* src/dst ALPHA_CTRL field bits (0x28 / 0x2C). */

#define RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT  5
#define RK3576_VOP_MIX_ALPHA_FACTOR_MASK \
  (0x7 << RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT)
#define RK3576_VOP_MIX_ALPHA_CAL_MODE      (1 << 4)
#define RK3576_VOP_MIX_ALPHA_BLEND_SHIFT   2
#define RK3576_VOP_MIX_ALPHA_BLEND_MASK \
  (0x3 << RK3576_VOP_MIX_ALPHA_BLEND_SHIFT)
#define RK3576_VOP_MIX_ALPHA_MODE          (1 << 1)

/* factor_mode encodings (3-bit, matching TRM *_FACTOR_MODE). */

#define RK3576_VOP_FACTOR_ZERO         0      /* 3'b000 = 0 */
#define RK3576_VOP_FACTOR_ONE          1      /* 3'b001 = 256 */
#define RK3576_VOP_FACTOR_DST          2      /* 3'b010 = Ad0 */
#define RK3576_VOP_FACTOR_DST_INVERSE  3      /* 3'b011 = 256-Ad0 */
#define RK3576_VOP_FACTOR_SRC          4      /* 3'b100 = As0 */
#define RK3576_VOP_FACTOR_SRC_GLOBAL   5      /* 3'b101 = Ags */

/* blend_mode encodings (2-bit). */

#define RK3576_VOP_BLEND_GLOBAL         0     /* use global alpha only */
#define RK3576_VOP_BLEND_PER_PIX        1     /* use per-pixel alpha only */
#define RK3576_VOP_BLEND_PER_PIX_GLOBAL 2     /* per-pixel * global */

#define RK3576_VOP_LAYER_SEL_SHIFT0     0
#define RK3576_VOP_LAYER_SEL_SHIFT1     4
#define RK3576_VOP_LAYER_SEL_SHIFT2     8
#define RK3576_VOP_LAYER_SEL_SHIFT3     12
#define RK3576_VOP_LAYER_SEL_MASK       (0xf << RK3576_VOP_LAYER_SEL_SHIFT0)

/* Window select values (common to all layers). */

#define RK3576_VOP_LAYER_SEL_CLUSTER0   0x0 /* Cluster0 */
#define RK3576_VOP_LAYER_SEL_CLUSTER1   0x1 /* Cluster1 */
#define RK3576_VOP_LAYER_SEL_ESMART0    0x2 /* Esmart0 */
#define RK3576_VOP_LAYER_SEL_ESMART2    0x3 /* Esmart2 */
#define RK3576_VOP_LAYER_SEL_DISABLE    0xf /* Unused/disable */
#define RK3576_VOP_LAYER_SEL_L0(x)      ((x) << RK3576_VOP_LAYER_SEL_SHIFT0)
#define RK3576_VOP_LAYER_SEL_L1(x)      ((x) << RK3576_VOP_LAYER_SEL_SHIFT1)
#define RK3576_VOP_LAYER_SEL_L2(x)      ((x) << RK3576_VOP_LAYER_SEL_SHIFT2)
#define RK3576_VOP_LAYER_SEL_L3(x)      ((x) << RK3576_VOP_LAYER_SEL_SHIFT3)

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

/* POST_CORE_CLK (0x000C) field definitions — the VOP-internal pixel clock
 * dividers.  RK3576 VP0 is dual-pixel (pixel_rate = 2), so Linux's
 * rk3576_calc_cru_cfg() programs:
 *   core_dclk_div (dclk_core_sel, bit0) = 1  -> dclk_core = dclk / 2
 *   dclk_div2     (dclk_out_sel,  bit2) = 0  -> dclk_out  = dclk_core
 * With dclk = 64 M the scan is driven by dclk_core = 32 M, which matches
 * the panel's 64 M pixel rate (dual-pixel: 2 pixels per dclk_core cycle).
 * Leaving bit0 = 0 (reset default) doubles dclk_core to 64 M -> the VOP
 * scans at 2x the panel frame rate and the DSI IPI never locks (INT_ST_IPI
 * stays 0, all-black panel).
 */

#define RK3576_VOP_POST_CORE_CLK_DCLK_CORE_SEL (1u << 0) /* 0=dclk, 1=dclk/2 */
#define RK3576_VOP_POST_CORE_CLK_DCLK_OUT_SEL  (1u << 2) /* 0=dclk, 1=dclk/2 */

/* POST_DSP_BG field definitions. */

#define RK3576_VOP_POST_BG_DISPLAY_EN  (1 << 31) /* BG display en */
#define RK3576_VOP_POST_BG_RED_SHIFT   20        /* 10-bit red */
#define RK3576_VOP_POST_BG_GREEN_SHIFT 10        /* 10-bit green */
#define RK3576_VOP_POST_BG_BLUE_SHIFT  0         /* 10-bit blue */

/* -----------------------------------------------------------------------
 * ESMARTx registers (base + RK3576_VOP_ESMARTx_OFFSET).
 *
 * The ESMART layer is the plain, uncompressed raster layer -- the natural
 * choice for a regular RGB framebuffer (no FBCD/compression, no 4k line
 * buffer).  Each window (REGION0..3) is independently addressable; this
 * driver uses only REGION0.  The data path is:
 *   REGION0 (DMA from DDR) --> ESMART out --> OVERLAY layer --> mix --> POST
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_ESMART_CTRL0            0x0000 /* CSC / global config */
#define RK3576_VOP_ESMART_CTRL1            0x0004 /* DMA control */
#define RK3576_VOP_ESMART_AXI_CTRL_IMD     0x0008 /* AXI / MMU control */
#define RK3576_VOP_ESMART_REGION0_CTRL     0x0010 /* Region0 config/enable */
#define RK3576_VOP_ESMART_REGION0_YRGB_MST 0x0014 /* Region0 fb start addr */
#define RK3576_VOP_ESMART_REGION0_CBCR_MST 0x0018 /* Region0 CbCr addr */
#define RK3576_VOP_ESMART_REGION0_VIR      0x001C /* Region0 virtual stride */
#define RK3576_VOP_ESMART_REGION0_ACT_INFO 0x0020 /* Region0 active (w-1,h-1) */
#define RK3576_VOP_ESMART_REGION0_DSP_INFO 0x0024 /* Region0 display (w-1,h-1) */
#define RK3576_VOP_ESMART_REGION0_DSP_OFF  0x0028 /* Region0 display offset */
#define RK3576_VOP_ESMART_REGION0_SCL_CTRL 0x0030 /* Region0 scale (0=off) */
#define RK3576_VOP_ESMART_PORT_SEL_IMD     0x00F4 /* Output video port sel */

/* ESMART_CTRL0 field definitions.  Bit 31 is the esmart frame reset enable:
 * the esmart layer is held in reset until this bit is set ('1' = release).
 * RGB sources leave the color-space conversion fields at their defaults. */

#define RK3576_VOP_ESMART_CTRL0_FRM_RESETN_EN (1 << 31)

/* ESMART_AXI_CTRL_IMD field definitions.
 *   bit16 dma_4k_addr_opt, bit8 auto_gating, bit7:4 outstanding_num,
 *   bit3 mmu_bypass, bit2 axi_sel, bit1 dma_sop, bit0 outstanding_en.
 *
 *   CRITICAL: outstanding_en reset = 0 means *unlimited* AXI bursts, NOT
 *   "DMA off".  An unbounded ESMART scan-out DMA floods the AXI/NoC and
 *   starves the CPU's DDR traffic -> the whole SoC hangs once the VOP
 *   starts scanning (see repo memory rk3576-vop-axi-outstanding).
 */

#define RK3576_VOP_ESMART_AXI_DMA_4K_ADDR_OPT (1 << 16) /* 4k crossing opt */
#define RK3576_VOP_ESMART_AXI_MMU_BYPASS      (1 << 3)  /* physical addr */
#define RK3576_VOP_ESMART_AXI_OUTSTANDING_EN  (1 << 0)  /* limit bursts */
#define RK3576_VOP_ESMART_AXI_OUTSTANDING_SHIFT 4
#define RK3576_VOP_ESMART_AXI_OUTSTANDING_MASK (0xf << RK3576_VOP_ESMART_AXI_OUTSTANDING_SHIFT)
#define RK3576_VOP_ESMART_AXI_OUTSTANDING(n) \
  (((uint32_t)(n) << RK3576_VOP_ESMART_AXI_OUTSTANDING_SHIFT) & \
   RK3576_VOP_ESMART_AXI_OUTSTANDING_MASK)

/* REGION0_CTRL field definitions. */

#define RK3576_VOP_ESMART_REGION0_MST_EN    (1 << 0) /* Region enable */
#define RK3576_VOP_ESMART_REGION0_FMT_SHIFT 1        /* Pixel format */
#define RK3576_VOP_ESMART_REGION0_FMT_MASK  \
  (0x1f << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)
#define RK3576_VOP_ESMART_REGION0_RB_SWAP   (1 << 10) /* Swap R/B */

/* region0_data_fmt values (REGION0_CTRL[5:1]). */

#define RK3576_VOP_ESMART_FMT_ARGB8888 (0x00 << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)
#define RK3576_VOP_ESMART_FMT_RGB888   (0x01 << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)
#define RK3576_VOP_ESMART_FMT_RGB565   (0x02 << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)

/* REGION0_VIR: line stride in words.
 *   ARGB8888: vir_width; RGB888: (w*3/4)+(w%3); RGB565: ceil(w/2)
 */

/* ESMART_PORT_SEL_IMD: 2-bit video-port select. */

#define RK3576_VOP_ESMART_PORT_SEL_SHIFT 0
#define RK3576_VOP_ESMART_PORT_SEL_MASK  (0x3 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)
#define RK3576_VOP_ESMART_PORT_VP0       (0x0 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)
#define RK3576_VOP_ESMART_PORT_VP1       (0x1 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)
#define RK3576_VOP_ESMART_PORT_VP2       (0x2 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)

/* -----------------------------------------------------------------------
 * Macros to build full register addresses from a VOP base address.
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_SYS_CTRL(base) ((base) + RK3576_VOP_SYS_CTRL_OFFSET)
#define RK3576_VOP_OVERLAY_PORT(base, p) \
  ((base) + RK3576_VOP_OVERLAY_PORT0_OFFSET + ((p) << 8))
#define RK3576_VOP_POST(base, p) \
  ((base) + RK3576_VOP_POST0_OFFSET + ((p) << 8))
#define RK3576_VOP_CLUSTER(base) ((base) + RK3576_VOP_CLUSTER0_OFFSET)
#define RK3576_VOP_ESMART(base, n) \
  ((base) + RK3576_VOP_ESMART0_OFFSET + ((n) << 9))

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VOP_H */
