/****************************************************************************
 * chips/rk3576/rk3576_vop.c
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
 * RK3576 Video Output Processor (VOP) framebuffer driver.
 *
 * Implements the NuttX generic framebuffer framework (struct fb_vtable_s)
 * on top of the RK3576 VOP (TRM Part 2, Chapter 11 "VOP_LITE").
 *
 * Minimal scope: a single CLUSTER0_WIN0 RGB888 layer scanned out through
 * one video port (POSTx) to one physical output interface.  No MMU, no
 * ESMART, no compression, no alpha blending, no vsync interrupt.
 *
 * The framebuffer is allocated from the RK3576 DMA heap
 * (rk3576_dma_alloc): the WIN0 YRGB_MST register is a 32-bit physical
 * address (MMU bypass), so the buffer must be physically contiguous and
 * below 4GB.
 *
 * The VOP is shared by multiple output interfaces.  Routing is expressed
 * as an (iface, port) pair; each RK3576_VOP_IFACE_* id maps to a
 * SYS_CTRL_*_INFACE_CTRL register offset in g_rk3576_vop_iface_regs, and
 * the same field layout (port_sel/out_en) is applied uniformly.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/video/fb.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_vop.h"
#include "rk3576_addrenv.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_vop.h"

#ifdef CONFIG_RK3576_VOP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* VOP bus clocks (registered by rk3576_clk_register_vop()). */

#define RK3576_VOP_ACLK_NAME "aclk_vop"
#define RK3576_VOP_HCLK_NAME "hclk_vop"
#define RK3576_VOP_PCLK_NAME "pclk_vop_root"

/* VOP core resets (active-low, hiword-mask write scheme).
 * SOFTRST_CON61: aresetn_vop[9], hresetn_vop[8], presetn_vop_biu[7],
 *                hresetn_vop_biu[6], aresetn_vop_biu[4].
 */

#define RK3576_VOP_RST_CON         61
#define RK3576_VOP_RST_ARESETN_VOP (1 << 9)
#define RK3576_VOP_RST_HRESETN_VOP (1 << 8)
#define RK3576_VOP_RST_PRESETN_BIU (1 << 7)
#define RK3576_VOP_RST_HRESETN_BIU (1 << 6)
#define RK3576_VOP_RST_ARESETN_BIU (1 << 4)
#define RK3576_VOP_RST_MAIN_MASK                             \
  (RK3576_VOP_RST_ARESETN_VOP | RK3576_VOP_RST_HRESETN_VOP | \
   RK3576_VOP_RST_PRESETN_BIU | RK3576_VOP_RST_HRESETN_BIU | \
   RK3576_VOP_RST_ARESETN_BIU)

/* Hiword-mask write: bits [31:16] are the write-enable mask. */

#define RK3576_VOP_HWM(bits) ((bits) << 16)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Framebuffer driver instance.  The vtable is the first member so the
 * fb framework's struct fb_vtable_s * can be cast back via container_of.
 */

struct rk3576_vop_s
{
  struct fb_vtable_s vtable;    /* Must be first */
  uintptr_t base;               /* VOP base (0x27D00000) */
  struct clk_s *aclk;           /* aclk_vop */
  struct clk_s *hclk;           /* hclk_vop */
  struct clk_s *dclk;           /* dclk_vpN (pixel clock) */
  struct rk3576_vop_config cfg; /* Routing/geometry configuration */
  void *fbmem;                  /* Framebuffer (DMA heap) */
  size_t fblen;                 /* Framebuffer length in bytes */
  uint32_t stride;              /* Line stride in bytes */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Map logical interface id -> SYS_CTRL_*_INFACE_CTRL register offset.
 * Indexed by enum rk3576_vop_iface_e.
 */

static const uint32_t g_rk3576_vop_iface_regs[RK3576_VOP_IFACE_MAX] = {
  RK3576_VOP_MIPI0_INFACE_CTRL, /* RK3576_VOP_IFACE_MIPI_DSI */
  RK3576_VOP_HDMI0_INFACE_CTRL, /* RK3576_VOP_IFACE_HDMI */
  RK3576_VOP_EDP0_INFACE_CTRL,  /* RK3576_VOP_IFACE_EDP */
  RK3576_VOP_DP0_INFACE_CTRL,   /* RK3576_VOP_IFACE_DP */
  RK3576_VOP_RGB_INFACE_CTRL,   /* RK3576_VOP_IFACE_RGB */
};

/* Pixel clock name per video port (registered by rk3576_clk_register_vop). */

static const char *g_rk3576_vop_dclk_names[3] = {
  "dclk_vp0", /* RK3576_VOP_PORT0 */
  "dclk_vp1", /* RK3576_VOP_PORT1 */
  "dclk_vp2", /* RK3576_VOP_PORT2 */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_vop_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo);
static int rk3576_vop_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                                   FAR struct fb_planeinfo_s *pinfo);
static int rk3576_vop_open(FAR struct fb_vtable_s *vtable);
static int rk3576_vop_close(FAR struct fb_vtable_s *vtable);
static int rk3576_vop_setpower(FAR struct fb_vtable_s *vtable, int power);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Framebuffer vtable (only the callbacks exercised by the fb framework's
 * char driver are wired here; advanced features are intentionally left
 * NULL so the framework reports -ENOTTY where appropriate).
 */

static const struct fb_vtable_s g_rk3576_vop_vtable = {
  .getvideoinfo = rk3576_vop_getvideoinfo,
  .getplaneinfo = rk3576_vop_getplaneinfo,
  .open = rk3576_vop_open,
  .close = rk3576_vop_close,
  .setpower = rk3576_vop_setpower,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop_getreg / putreg / modifyreg
 ****************************************************************************/

static uint32_t rk3576_vop_getreg(struct rk3576_vop_s *priv,
                                  unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static void rk3576_vop_putreg(struct rk3576_vop_s *priv, unsigned int offset,
                              uint32_t value)
{
  putreg32(value, priv->base + offset);
}

static void rk3576_vop_modifyreg(struct rk3576_vop_s *priv,
                                 unsigned int offset, uint32_t clrbits,
                                 uint32_t setbits)
{
  uint32_t regval = rk3576_vop_getreg(priv, offset);

  regval &= ~clrbits;
  regval |= setbits;
  rk3576_vop_putreg(priv, offset, regval);
}

/****************************************************************************
 * Name: rk3576_vop_getvideoinfo
 ****************************************************************************/

static int rk3576_vop_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct rk3576_vop_s *priv = (FAR struct rk3576_vop_s *)vtable;

  DEBUGASSERT(vtable != NULL);
  DEBUGASSERT(vinfo != NULL);

  vinfo->fmt = FB_FMT_RGB24;
  vinfo->xres = priv->cfg.xres;
  vinfo->yres = priv->cfg.yres;
  vinfo->nplanes = 1;

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_getplaneinfo
 ****************************************************************************/

static int rk3576_vop_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                                   FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct rk3576_vop_s *priv = (FAR struct rk3576_vop_s *)vtable;

  DEBUGASSERT(vtable != NULL);
  DEBUGASSERT(pinfo != NULL);

  if (planeno != 0)
    {
      return -EINVAL;
    }

  pinfo->fbmem = priv->fbmem;
  pinfo->fblen = priv->fblen;
  pinfo->stride = priv->stride;
  pinfo->display = priv->cfg.display;
  pinfo->bpp = 24;
  pinfo->xres_virtual = priv->cfg.xres;
  pinfo->yres_virtual = priv->cfg.yres;
  pinfo->xoffset = 0;
  pinfo->yoffset = 0;

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_open / close
 ****************************************************************************/

static int rk3576_vop_open(FAR struct fb_vtable_s *vtable) { return OK; }

static int rk3576_vop_close(FAR struct fb_vtable_s *vtable) { return OK; }

/****************************************************************************
 * Name: rk3576_vop_setpower
 ****************************************************************************/

static int rk3576_vop_setpower(FAR struct fb_vtable_s *vtable, int power)
{
  FAR struct rk3576_vop_s *priv = (FAR struct rk3576_vop_s *)vtable;
  uint32_t post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);

  DEBUGASSERT(vtable != NULL);

  /* power == 0 -> black display (dsp_black_en); power != 0 -> normal. */

  if (power == 0)
    {
      rk3576_vop_modifyreg(priv, post_base + RK3576_VOP_POST_DSP_CTRL, 0,
                           RK3576_VOP_POST_BLACK_EN);
    }
  else
    {
      rk3576_vop_modifyreg(priv, post_base + RK3576_VOP_POST_DSP_CTRL,
                           RK3576_VOP_POST_BLACK_EN, 0);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_configure_timing
 *
 * Description:
 *   Program the POSTx timing registers (HTOTAL/HACT/VTOTAL/VACT) from the
 *   configuration's porch/sync values.  Layout follows the Rockchip
 *   convention where each *_TOTAL_*_END register packs {total[28:16],
 *   sync_end[12:0]} and each *_ACT_ST_END packs {start[28:16], end[12:0]}.
 ****************************************************************************/

static void rk3576_vop_configure_timing(FAR struct rk3576_vop_s *priv)
{
  uint32_t post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);
  uint16_t xres = priv->cfg.xres;
  uint16_t yres = priv->cfg.yres;

  uint16_t htotal = xres + priv->cfg.hsync_len + priv->cfg.hfront_porch +
                    priv->cfg.hback_porch;
  uint16_t vtotal = yres + priv->cfg.vsync_len + priv->cfg.vfront_porch +
                    priv->cfg.vback_porch;

  uint16_t hs_end = priv->cfg.hsync_len - 1;
  uint16_t hact_st = priv->cfg.hsync_len + priv->cfg.hback_porch;
  uint16_t hact_end = hact_st + xres - 1;

  uint16_t vs_end = priv->cfg.vsync_len - 1;
  uint16_t vact_st = priv->cfg.vsync_len + priv->cfg.vback_porch;
  uint16_t vact_end = vact_st + yres - 1;

  /* HTOTAL_HS_END: {total[28:16], hs_end[12:0]} */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_HTOTAL_HS_END,
                    ((uint32_t)(htotal - 1) << 16) | (hs_end & 0x1fff));

  /* HACT_ST_END: {start[28:16], end[12:0]} */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_HACT_ST_END,
                    ((uint32_t)hact_st << 16) | (hact_end & 0x1fff));

  /* VTOTAL_VS_END: {total[28:16], vs_end[12:0]} */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_VTOTAL_VS_END,
                    ((uint32_t)(vtotal - 1) << 16) | (vs_end & 0x1fff));

  /* VACT_ST_END: {start[28:16], end[12:0]} */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_VACT_ST_END,
                    ((uint32_t)vact_st << 16) | (vact_end & 0x1fff));
}

/****************************************************************************
 * Name: rk3576_vop_configure_win
 *
 * Description:
 *   Program CLUSTER0_WIN0 to scan out a single RGB888 layer:
 *   data format, framebuffer address, virtual stride, active/display
 *   region, and layer enable.
 ****************************************************************************/

static void rk3576_vop_configure_win(FAR struct rk3576_vop_s *priv)
{
  uint32_t cluster_base = RK3576_VOP_CLUSTER(priv->base);
  uint16_t xres = priv->cfg.xres;
  uint16_t yres = priv->cfg.yres;
  uint32_t vir_stride; /* stride in words (4 bytes) */
  uint32_t ctrl;

  /* CTRL0: layer disabled first, RGB888 format. */

  rk3576_vop_putreg(priv, cluster_base + RK3576_VOP_WIN0_CTRL0,
                    RK3576_VOP_WIN0_FMT_RGB888);

  /* Framebuffer physical start address.  The WIN0 YRGB_MST register takes
   * a 32-bit physical address (MMU bypass), so translate the kernel virtual
   * address to its physical address first (identity under CONFIG_BUILD_FLAT,
   * walked through the page tables under CONFIG_BUILD_KERNEL).
   */

  rk3576_vop_putreg(priv, cluster_base + RK3576_VOP_WIN0_YRGB_MST,
                    (uint32_t)up_addrenv_va_to_pa(priv->fbmem));

  /* Virtual stride in words: RGB888 -> (w*3/4) + (w%3). */

  vir_stride = ((uint32_t)xres * 3 / 4) + ((uint32_t)xres % 3);
  rk3576_vop_putreg(priv, cluster_base + RK3576_VOP_WIN0_VIR, vir_stride);

  /* Active region (w-1, h-1) and display region (same, no scaling). */

  rk3576_vop_putreg(priv, cluster_base + RK3576_VOP_WIN0_ACT_INFO,
                    (((uint32_t)(yres - 1)) << 16) | (xres - 1));
  rk3576_vop_putreg(priv, cluster_base + RK3576_VOP_WIN0_DSP_INFO,
                    (((uint32_t)(yres - 1)) << 16) | (xres - 1));
  rk3576_vop_putreg(priv, cluster_base + RK3576_VOP_WIN0_DSP_ST, 0);

  /* Enable the window. */

  ctrl = RK3576_VOP_WIN0_FMT_RGB888 | RK3576_VOP_WIN0_EN;
  rk3576_vop_putreg(priv, cluster_base + RK3576_VOP_WIN0_CTRL0, ctrl);
}

/****************************************************************************
 * Name: rk3576_vop_configure_port
 *
 * Description:
 *   Program POSTx output mode (dsp_out_mode = RGB888), take the device out
 *   of standby, and route the selected interface to the selected port
 *   through SYS_CTRL_*_INFACE_CTRL.
 ****************************************************************************/

static void rk3576_vop_configure_port(FAR struct rk3576_vop_s *priv)
{
  uint32_t post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);
  uint32_t sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  uint32_t iface_off = g_rk3576_vop_iface_regs[priv->cfg.iface];
  uint32_t regval;

  /* POST: RGB888 output mode, clear standby. */

  rk3576_vop_modifyreg(priv, post_base + RK3576_VOP_POST_DSP_CTRL,
                       RK3576_VOP_POST_STANDBY_EN |
                           RK3576_VOP_POST_OUT_MODE_MASK,
                       RK3576_VOP_POST_OUT_RGB888);

  /* Interface ctrl: select video port + enable output (and clock). */

  regval = RK3576_VOP_IFACE_CLK_OUT_EN | RK3576_VOP_IFACE_OUT_EN |
           ((uint32_t)priv->cfg.port << RK3576_VOP_IFACE_PORT_SEL_SHIFT);
  rk3576_vop_putreg(priv, sys_base + iface_off, regval);
}

/****************************************************************************
 * Name: rk3576_vop_enable_clocks
 ****************************************************************************/

static int rk3576_vop_enable_clocks(FAR struct rk3576_vop_s *priv)
{
  int ret;

  priv->aclk = clk_get(RK3576_VOP_ACLK_NAME);
  if (priv->aclk == NULL)
    {
      gerr("ERROR: VOP failed to get %s\n", RK3576_VOP_ACLK_NAME);
      return -ENODEV;
    }

  priv->hclk = clk_get(RK3576_VOP_HCLK_NAME);
  if (priv->hclk == NULL)
    {
      gerr("ERROR: VOP failed to get %s\n", RK3576_VOP_HCLK_NAME);
      return -ENODEV;
    }

  priv->dclk = clk_get(g_rk3576_vop_dclk_names[priv->cfg.port]);
  if (priv->dclk == NULL)
    {
      gerr("ERROR: VOP failed to get %s\n",
           g_rk3576_vop_dclk_names[priv->cfg.port]);
      return -ENODEV;
    }

  ret = clk_enable(priv->aclk);
  if (ret < 0)
    {
      gerr("ERROR: VOP failed to enable aclk_vop: %d\n", ret);
      return ret;
    }

  ret = clk_enable(priv->hclk);
  if (ret < 0)
    {
      gerr("ERROR: VOP failed to enable hclk_vop: %d\n", ret);
      clk_disable(priv->aclk);
      return ret;
    }

  ret = clk_enable(priv->dclk);
  if (ret < 0)
    {
      gerr("ERROR: VOP failed to enable %s: %d\n",
           g_rk3576_vop_dclk_names[priv->cfg.port], ret);
      clk_disable(priv->hclk);
      clk_disable(priv->aclk);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_reset
 *
 * Description:
 *   De-assert (release) the VOP core resets.  Resets are active-low and use
 *   the hiword-mask write scheme: assert then de-assert with a short delay.
 ****************************************************************************/

static void rk3576_vop_reset(FAR struct rk3576_vop_s *priv)
{
  uint32_t reg = RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_VOP_RST_CON);

  /* Assert (hold in reset, write 1 = assert since active-low). */

  putreg32(RK3576_VOP_HWM(RK3576_VOP_RST_MAIN_MASK) | RK3576_VOP_RST_MAIN_MASK,
           reg);
  up_udelay(20);

  /* De-assert (release, write 0). */

  putreg32(RK3576_VOP_HWM(RK3576_VOP_RST_MAIN_MASK), reg);
  up_udelay(20);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop_initialize
 ****************************************************************************/

int rk3576_vop_initialize(FAR const struct rk3576_vop_config *config)
{
  FAR struct rk3576_vop_s *priv;
  int ret;

  DEBUGASSERT(config != NULL);

  if (config->iface >= RK3576_VOP_IFACE_MAX || config->port > 2)
    {
      gerr("ERROR: VOP invalid routing (iface=%d port=%d)\n", config->iface,
           config->port);
      return -EINVAL;
    }

  priv = kmm_zalloc(sizeof(struct rk3576_vop_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  memcpy(&priv->vtable, &g_rk3576_vop_vtable, sizeof(struct fb_vtable_s));
  memcpy(&priv->cfg, config, sizeof(struct rk3576_vop_config));
  priv->base = RK3576_VOP_ADDR;

  /* Framebuffer geometry (RGB888, 3 bytes/pixel). */

  priv->stride = (uint32_t)config->xres * 3;
  priv->fblen = (size_t)priv->stride * config->yres;

  /* Allocate the framebuffer from the DMA heap (<4GB, physically
   * contiguous, identity mapped).
   */

  priv->fbmem = rk3576_dma_alloc(priv->fblen);
  if (priv->fbmem == NULL)
    {
      gerr("ERROR: VOP failed to allocate %zu-byte framebuffer\n",
           priv->fblen);
      ret = -ENOMEM;
      goto errout_with_priv;
    }

  /* Bring up clocks and release resets. */

  ret = rk3576_vop_enable_clocks(priv);
  if (ret < 0)
    {
      goto errout_with_fb;
    }

  rk3576_vop_reset(priv);

  /* Program layer, timing and output routing. */

  rk3576_vop_configure_win(priv);
  rk3576_vop_configure_timing(priv);
  rk3576_vop_configure_port(priv);

  /* Self-register the framebuffer device. */

  ret = fb_register_device(config->display, config->plane, &priv->vtable);
  if (ret < 0)
    {
      gerr("ERROR: VOP fb_register_device() failed: %d\n", ret);
      goto errout_with_clocks;
    }

  return OK;

errout_with_clocks:
  clk_disable(priv->dclk);
  clk_disable(priv->hclk);
  clk_disable(priv->aclk);

errout_with_fb:
  rk3576_dma_free(priv->fbmem, priv->fblen);

errout_with_priv:
  kmm_free(priv);
  return ret;
}

#endif /* CONFIG_RK3576_VOP */
