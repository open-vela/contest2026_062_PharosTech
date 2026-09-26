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
 * Scope: one ESMART REGION0 RGB888 layer scanned out through one video port
 * (POSTx) to one physical output interface.  No MMU, no compression, no alpha
 * blending, no vsync interrupt.
 *
 * The framebuffer is allocated from the RK3576 DMA heap (rk3576_dma_alloc):
 * the window's YRGB_MST register is a 32-bit physical address (MMU bypass), so
 * the buffer must be physically contiguous and below 4GB.
 *
 * Routing is expressed as an (iface, port) pair; each RK3576_VOP_IFACE_* id
 * maps to a SYS_CTRL_*_INFACE_CTRL offset in g_rk3576_vop_iface_regs and the
 * same field layout (port_sel/out_en) is applied uniformly.
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
#include <stdio.h>
#include <string.h>

#include <syslog.h>

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

/* The ESMART window this driver scans out through.  ESMART0 is the window
 * every Rockchip reference configuration uses for VP0's primary plane, and the
 * only one for which the axi_sel side effect below cannot fire.
 */

#define RK3576_VOP_ESMART_IDX 0

/* ESMART_CTRL0 esmart_scl_num ([13:12]) selects the scale engine for this
 * window.  The bootloader set it to the window's own index (ESMART1 = 1,
 * ESMART2 = 2, ESMART3 = 3, ESMART0 = 0), so it is derived from the index
 * rather than hard-coded, or the window would be pointed at another window's
 * scaler. */

#define RK3576_VOP_ESMART_SCL_NUM ((uint32_t)RK3576_VOP_ESMART_IDX << 12)

/****************************************************************************
 * RK3576 gives each ESMART its own AXI read IDs and its own AXI channel
 * (Rockchip's rk3576_vop_win_data[]):
 *
 *   Esmart0 : axi_id 0 (axi0)  yrgb 0x10  uv 0x11
 *   Esmart1 : axi_id 0 (axi0)  yrgb 0x12  uv 0x13
 *   Esmart2 : axi_id 1 (axi1)  yrgb 0x0a  uv 0x0b
 *   Esmart3 : axi_id 1 (axi1)  yrgb 0x0c  uv 0x0d
 *
 * A read ID travels with the transaction into the interconnect, where it
 * selects routing and QoS; an ID no slave expects yields a read that never
 * returns.  Whichever window RK3576_VOP_ESMART_IDX selects, its reads must
 * carry that window's own pair on that window's own channel.
 */

static const uint32_t g_rk3576_vop_esmart_yrgb_rid[4] = {
  0x10u, /* ESMART0 */
  0x12u, /* ESMART1 */
  0x0au, /* ESMART2 */
  0x0cu  /* ESMART3 */
};

static const uint32_t g_rk3576_vop_esmart_uv_rid[4] = {
  0x11u, /* ESMART0 */
  0x13u, /* ESMART1 */
  0x0bu, /* ESMART2 */
  0x0du  /* ESMART3 */
};

static const uint32_t g_rk3576_vop_esmart_axi_sel[4] = {
  0u, /* ESMART0 -> axi0 */
  0u, /* ESMART1 -> axi0 */
  1u, /* ESMART2 -> axi1 */
  1u  /* ESMART3 -> axi1 */
};

/* ESMART_AXI_CTRL_IMD bit1 esmart_axi_sel: 0 = axi0, 1 = axi1.
 *
 * Left at 0 (axi0), which the TRM recommends and the reference driver uses for
 * ESMART0.  MEASURED: asserting axi_sel clears bits [7:2] of the same register
 * -- mmu_bypass, outstanding_en and outstanding_num -- so it must never be set
 * without re-asserting those fields afterwards.
 */

#define RK3576_VOP_ESMART_USE_AXI1 0

/* VOP core resets (active-high "when high, reset", hiword-mask write
 * scheme).  TRM Part1 CRU_SOFTRST_CON61 (0x0AF4): write 1 = assert reset,
 * write 0 = release.  All reset bits reset to 0 (= released).
 *
 * SOFTRST_CON61: dresetn_vp0[13], aresetn_vop[9], hresetn_vop[8],
 *                presetn_vop_biu[7], hresetn_vop_biu[6], aresetn_vop_biu[4].
 */

#define RK3576_VOP_RST_CON         61
#define RK3576_VOP_RST_DRESETN_VP0 (1 << 13)
#define RK3576_VOP_RST_ARESETN_VOP (1 << 9)
#define RK3576_VOP_RST_HRESETN_VOP (1 << 8)
#define RK3576_VOP_RST_PRESETN_BIU (1 << 7)
#define RK3576_VOP_RST_HRESETN_BIU (1 << 6)
#define RK3576_VOP_RST_ARESETN_BIU (1 << 4)
#define RK3576_VOP_RST_MAIN_MASK                             \
  (RK3576_VOP_RST_DRESETN_VP0 | RK3576_VOP_RST_ARESETN_VOP | \
   RK3576_VOP_RST_HRESETN_VOP | RK3576_VOP_RST_PRESETN_BIU | \
   RK3576_VOP_RST_HRESETN_BIU | RK3576_VOP_RST_ARESETN_BIU)

/* Hiword-mask write: bits [31:16] are the write-enable mask. */

#define RK3576_VOP_HWM(bits) ((bits) << 16)

/* PMU power-domain control (PMU base + PWR_GATE_CON0 @ 0x20200).  The ESMART
 * layer lives in its own domain (PD_VOP_ESMART); there is no standalone
 * aclk_esmart gate in the CRU, so the power domain is what has to be up for
 * its registers to be readable.
 *
 *   bit 12  pd_vop_esmart_dwn_ena  ('0' = keep PD_VOP_ESMART powered)
 *   bit 11  pd_vop_dwn_ena         ('0' = keep PD_VOP powered)
 *
 * Writes use the hiword-mask scheme: bits [31:16] mask which low bits are
 * written.
 */

#define RK3576_VOP_PMU_PWR_GATE_CON0_OFF 0x20200
#define RK3576_VOP_PD_VO0_DWN_ENA        (1 << 15)
#define RK3576_VOP_PD_VO1_DWN_ENA        (1 << 14)
#define RK3576_VOP_PD_VOP_ESMART_DWN_ENA (1 << 12)
#define RK3576_VOP_PD_VOP_DWN_ENA        (1 << 11)

/* Power-state readback, same bit positions (PMU_PWR_GATE_STS @ 0x20230,
 * "1'b0: Power up  1'b1: Power down").  PD_VO0 owns the display output
 * interfaces and the VOP's channel into the VO0 interconnect, so its state is
 * worth reading explicitly rather than assuming. */

#define RK3576_VOP_PMU_PWR_GATE_STS_OFF 0x20230

/* Memory-repair initial reset (BISR initrst), PMU_BISR_INITRST_SFTCON0 @
 * 0x20540.  These bits gate the PD-level reset (aresetn_pd): the reset value
 * is 0 = HELD IN RESET, so they must be set to 1 or the ESMART layer clocks
 * never come up and every ESMART register reads back 0.
 *
 *   bit 13  pd_vop_cluster_initrst_sftena
 *   bit 12  pd_vop_esmart_initrst_sftena
 *   bit 11  pd_vop_initrst_sftena
 *
 * PD_VO0/PD_VO1 (bits 15:14) are deliberately NOT released: they own the
 * display output interfaces and the VOP's channel into the VO0 interconnect,
 * and releasing their initial reset once the DSI is up regressed a working
 * output path.  Their state is left exactly as found.
 */

#define RK3576_VOP_PMU_BISR_INITRST_SFTCON0_OFF 0x20540
#define RK3576_VOP_PD_VO0_INITRST               (1 << 15)
#define RK3576_VOP_PD_VO1_INITRST               (1 << 14)
#define RK3576_VOP_PD_VOP_CLUSTER_INITRST       (1 << 13)
#define RK3576_VOP_PD_VOP_ESMART_INITRST        (1 << 12)
#define RK3576_VOP_PD_VOP_INITRST               (1 << 11)

#define RK3576_VOP_INITRST_RELEASE_MASK                                   \
  (RK3576_VOP_PD_VOP_CLUSTER_INITRST | RK3576_VOP_PD_VOP_ESMART_INITRST | \
   RK3576_VOP_PD_VOP_INITRST)

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

/* Retained handle so the read-only status helper below can be called after
 * rk3576_vop_initialize() has returned (fb_register_device() hands out the
 * vtable, not a pointer to the private state). */

static FAR struct rk3576_vop_s *g_rk3576_vop_priv;

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
#ifdef CONFIG_FB_UPDATE
static int rk3576_vop_updatearea(FAR struct fb_vtable_s *vtable,
                                 FAR const struct fb_area_s *area);
#endif

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
#ifdef CONFIG_FB_UPDATE
  .updatearea = rk3576_vop_updatearea,
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop_getreg / putreg / modifyreg
 *
 * Description:
 *   Register accessors.  The "reg" parameter is an *absolute* address,
 *   already composed from the VOP base plus one of the RK3576_VOP_*()
 *   address macros (which receive priv->base as their base argument).
 *   Callers pass e.g. RK3576_VOP_POST(priv->base, port) + POST_*_OFFSET,
 *   so these helpers must NOT add priv->base again (that would duplicate
 *   the base into the address).
 ****************************************************************************/

static uint32_t rk3576_vop_getreg(struct rk3576_vop_s *priv, unsigned int reg)
{
  return getreg32(reg);
}

static void rk3576_vop_putreg(struct rk3576_vop_s *priv, unsigned int reg,
                              uint32_t value)
{
  putreg32(value, reg);
}

static void rk3576_vop_modifyreg(struct rk3576_vop_s *priv, unsigned int reg,
                                 uint32_t clrbits, uint32_t setbits)
{
  uint32_t regval = rk3576_vop_getreg(priv, reg);

  regval &= ~clrbits;
  regval |= setbits;
  rk3576_vop_putreg(priv, reg, regval);
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

#ifdef CONFIG_FB_UPDATE
/****************************************************************************
 * Name: rk3576_vop_updatearea
 *
 * Description:
 *   Make a region of the framebuffer written by the CPU visible to the
 *   scan-out path.
 *
 *   The ESMART layer reads the framebuffer with the MMU bypassed (YRGB_MST
 *   is a physical address), so it sees DDR, not the CPU's D-cache.  A CPU
 *   write - an application drawing through mmap(), or the fb character
 *   driver's fb_write() - stays in the D-cache and the display keeps
 *   scanning out stale pixels until the cache is cleaned.
 *
 *   The fb framework forwards FBIO_UPDATE here, which is what LVGL's fbdev
 *   backend issues after every refresh with the area it just dirtied.
 ****************************************************************************/

static int rk3576_vop_updatearea(FAR struct fb_vtable_s *vtable,
                                 FAR const struct fb_area_s *area)
{
  FAR struct rk3576_vop_s *priv = (FAR struct rk3576_vop_s *)vtable;
  FAR uint8_t *fb;
  uint32_t y;
  uint32_t h;
  uintptr_t start;
  uintptr_t end;

  DEBUGASSERT(vtable != NULL);

  if (priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  fb = (FAR uint8_t *)priv->fbmem;

  /* No area information: flush everything, the always-correct choice. */

  if (area == NULL)
    {
      up_clean_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);
      return OK;
    }

  if (area->w == 0 || area->h == 0 || area->y >= priv->cfg.yres)
    {
      return OK;
    }

  /* Clean whole scan lines: cache maintenance works on 64-byte lines, so
   * the rows are the smallest unit that can be flushed without leaving a
   * half-written line behind.
   */

  y = area->y;
  h = area->h;

  if (y + h > priv->cfg.yres)
    {
      h = priv->cfg.yres - y;
    }

  start = (uintptr_t)fb + (size_t)y * priv->stride;
  end = start + (size_t)h * priv->stride;

  up_clean_dcache(start, end);

  return OK;
}
#endif /* CONFIG_FB_UPDATE */

/****************************************************************************
 * Name: rk3576_vop_configure_timing
 *
 * Description:
 *   Program the POSTx timing registers (HTOTAL/HACT/VTOTAL/VACT plus the POST
 *   output window) from the configuration's porch/sync values.
 *
 *   The *_END fields are EXCLUSIVE ends, and the *_TOTAL_*_END registers carry
 *   the FULL total (not total - 1):
 *
 *     DSP_HTOTAL_HS_END = (htotal     << 16) | hsync_len
 *     DSP_HACT_ST_END   = (hact_st    << 16) | (hact_st + hdisplay)
 *     DSP_VTOTAL_VS_END = (vtotal     << 16) | vsync_len
 *     DSP_VACT_ST_END   = (vact_st    << 16) | (vact_st + vdisplay)
 *
 *   where hact_st = htotal - hsync_start, hsync_start = hdisplay +
 *   hfront_porch, vact_st = vtotal - vsync_start.
 *
 *   The POST's own output active window (POST_DSP_HACT_INFO /
 *   POST_DSP_VACT_INFO) must be programmed too: left at reset it is 0, i.e.
 *"no output window", and the payload carries no real picture while the DSI
 *side still looks plausible.
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

  uint16_t hsync_start = xres + priv->cfg.hfront_porch;
  uint16_t vsync_start = yres + priv->cfg.vfront_porch;

  uint16_t hact_st = htotal - hsync_start;
  uint16_t hact_end = hact_st + xres;
  uint16_t vact_st = vtotal - vsync_start;
  uint16_t vact_end = vact_st + yres;

  /* HTOTAL_HS_END: {total[28:16], hsync_len[12:0]} */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_HTOTAL_HS_END,
                    ((uint32_t)htotal << 16) | (priv->cfg.hsync_len & 0x1fff));

  /* HACT_ST_END: {start[28:16], end[12:0]} (end exclusive) */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_HACT_ST_END,
                    ((uint32_t)hact_st << 16) | (hact_end & 0x1fff));

  /* VTOTAL_VS_END: {total[28:16], vsync_len[12:0]} */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_VTOTAL_VS_END,
                    ((uint32_t)vtotal << 16) | (priv->cfg.vsync_len & 0x1fff));

  /* VACT_ST_END: {start[28:16], end[12:0]} (end exclusive) */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_VACT_ST_END,
                    ((uint32_t)vact_st << 16) | (vact_end & 0x1fff));

  /* The POST's output active window.  The reference driver's margins split
   * 100/100, which makes the scaled size equal the panel size and the window
   * identical to the ACT one above; written explicitly rather than assumed. */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_HACT_INFO,
                    ((uint32_t)hact_st << 16) | (hact_end & 0x1fff));
  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_VACT_INFO,
                    ((uint32_t)vact_st << 16) | (vact_end & 0x1fff));

  /* The POST scaler is a 1:1 pass-through.  The factor is (src/dst) * 2^12, so
   * 1.0 is 0x1000 per axis; 0 means "scale by zero", a degenerate scale that
   * corrupts a real picture while leaving a uniform fill unchanged.  The
   * reference driver writes scl_cal_scale2() here unconditionally, 1:1
   * included, and a working dump on this board reads 0x10001000.
   */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_SCL_FACTOR_YRGB,
                    RK3576_VOP_POST_SCL_FACTOR_1_1);
  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_SCL_CTRL, 0);
}

/****************************************************************************
 * Name: rk3576_vop_configure_layer
 *
 * Description:
 *   Configure a single ESMART REGION0 to scan out an RGB888 framebuffer,
 *   route it into the selected video port's OVERLAY layer0, and fire the
 *   register-configure-done pulse so all mirror registers take effect at
 *   the start of the next frame.
 *
 *   The ESMART layer (not CLUSTER) is used because it is the plain
 *   uncompressed raster window -- the natural fit for a regular RGB
 *   framebuffer (no FBCD compression state machine to bring up).
 ****************************************************************************/

static void rk3576_vop_configure_layer(FAR struct rk3576_vop_s *priv)
{
  uint32_t esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  uint32_t ovl_base = RK3576_VOP_OVERLAY_PORT(priv->base, priv->cfg.port);
  uint32_t sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  uint16_t xres = priv->cfg.xres;
  uint16_t yres = priv->cfg.yres;
  uint32_t vir_stride; /* stride in words (4 bytes) */
  uint32_t ctrl;

  /* Disable the VOP's automatic clock gating.  The reset value has gating ON,
   * and the reference driver clears both bits for RK3576; its own comment
   * describes this board's failure mode -- the hardware may decide a block
   * looks idle, gate its aclk, and stop its fetch with no error reported at
   * all.  The remaining gating bits are left as the hardware set them. */

  {
    uint32_t gate = rk3576_vop_getreg(
        priv, sys_base + RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD);
    uint32_t fixed = gate & ~(RK3576_VOP_AUTO_GATING_EN |
                              RK3576_VOP_ACLK_PRE_AUTO_GATING_EN);

    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD,
                      fixed);
  }

  /* AXI read IDs: this window's own pair, not the reset defaults.  0x0a/0x0b
   * belongs to Esmart2, which fetches on axi1.  Only the two ID fields are
   * written, which is all the reference driver's window table writes here:
   * dma_rreq_hurry_en with thold = 0 would hold a priority request asserted
   * for ever, which carries no information.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_CTRL1,
                    (g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX]
                     << RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT) |
                        (g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX]
                         << RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT));

  /* AXI control: MMU bypass (REGION0_YRGB_MST is a physical address) plus the
   * 4k-boundary burst optimisation.  The low bits are dma_sop(0) / axi_sel(1)
   * / mmu_bypass(2) / outstanding_en(3), named symbolically in the header.
   *
   * No outstanding bound: a working dump reads outstanding_en = 0, i.e. "as
   * many as the interconnect accepts", and the reference driver's window table
   * does not define the field at all.  mmu_bypass STAYS SET -- a deliberate,
   * permanent difference from that dump, which uses an IOMMU this driver does
   * not have.
   */

  /* Program the AXI channel first, then the rest.  RK3576 gives each ESMART
   * its own channel (ESMART0/1 on axi0, ESMART2/3 on axi1); MEASURED, a write
   * that sets axi_sel also clears bits [7:2] -- mmu_bypass, outstanding_en and
   * the count -- and a later write carrying axi_sel clears them again.  That
   * is why the window is ESMART0: axi_sel stays 0 there.
   */

  if (g_rk3576_vop_esmart_axi_sel[RK3576_VOP_ESMART_IDX] != 0u)
    {
      rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                        RK3576_VOP_ESMART_AXI_AXI_SEL);
    }

  rk3576_vop_putreg(
      priv, esmart_base + RK3576_VOP_ESMART_AXI_CTRL_IMD,
      RK3576_VOP_ESMART_AXI_DMA_4K_ADDR_OPT |
          RK3576_VOP_ESMART_AXI_MMU_BYPASS |
          (g_rk3576_vop_esmart_axi_sel[RK3576_VOP_ESMART_IDX] != 0u
               ? RK3576_VOP_ESMART_AXI_AXI_SEL
               : 0u));

  /* REGION0 disabled first, RGB888 format -- as a FIELD write.  A working dump
   * reads 0x00400001 here: bit0 written by the driver and bit22 left at its
   * reset value of 1.  A plain whole-register write silently clears bit22, and
   * the same mistake wiped the SCL_CTRL filter modes, so only the fields this
   * driver has evidence for are written.
   */

  ctrl = rk3576_vop_getreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL);
  ctrl &=
      ~(RK3576_VOP_ESMART_REGION0_FMT_MASK | RK3576_VOP_ESMART_REGION0_MST_EN);
  ctrl |= RK3576_VOP_ESMART_FMT_RGB888;
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL, ctrl);

  /* REGION0 framebuffer physical start address (MMU bypass -> 32-bit pa). */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_YRGB_MST,
                    (uint32_t)up_addrenv_va_to_pa(priv->fbmem));

  /* Virtual stride in words: RGB888 -> (w*3/4) + (w%3). */

  vir_stride = ((uint32_t)xres * 3 / 4) + ((uint32_t)xres % 3);
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_VIR,
                    vir_stride);

  /* Active region (w-1, h-1) and display region (same, no scaling),
   * display offset (0,0), scaling engine off.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_ACT_INFO,
                    (((uint32_t)(yres - 1)) << 16) | (xres - 1));
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_DSP_INFO,
                    (((uint32_t)(yres - 1)) << 16) | (xres - 1));
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_DSP_OFF, 0);

  /* SCL_CTRL: the default filter modes (0x44), which is the value the working
   * configuration holds.  The scaler sits between the window's line buffers
   * and its output, so it is not left at a value the vendor never writes.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_SCL_CTRL,
                    RK3576_VOP_ESMART_SCL_FILTER_MODES_DEFAULT);

  /* SCL_FACTOR = 0 for a non-scaling window: vop2_scale_factor() returns 0 for
   * SCALE_NONE and the SCL_NONE branch writes scale_yrgb_x/y = 0.  The reset
   * value 0x10001000 (1.0) is not a value the vendor produces for a 1:1 layer.
   * The CbCr factor is deliberately left alone -- the working dump does read
   * 0x10001000 there and Linux only writes it for a YUV layer.
   */

  rk3576_vop_putreg(
      priv, esmart_base + RK3576_VOP_ESMART_REGION0_SCL_FACTOR_YRGB, 0u);

  /* Layer pipeline delay: 0, which is what the reference driver's
   * vop2_calc_dly_num() reduces to with no HDR, no CGC and no sdr2hdr.  The
   * register is not written by the reference driver and read 0x17 here; it
   * delays the layer's pixel stream against the mixing pipeline.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_DLY_NUM, 0u);

  /* Route ESMART0 to this video port. */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_PORT_SEL_IMD,
                    RK3576_VOP_ESMART_PORT_VP0);

  /* Enable REGION0: RGB888 + region enable.
   *
   * rb_swap is set because the TRM's swap table (11.3) gives rb_swap = 1 for
   * RGB888 -- the layer wants BGR byte order while the framebuffer is stored
   * R,G,B.  Clearing it exchanges red and blue on the glass.
   */

  ctrl = rk3576_vop_getreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL);
  ctrl |= RK3576_VOP_ESMART_FMT_RGB888 | RK3576_VOP_ESMART_REGION0_MST_EN |
          RK3576_VOP_ESMART_REGION0_RB_SWAP;
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL, ctrl);

  /* OVERLAY: connect layer0 to the window selected by RK3576_VOP_ESMART_IDX
   * (0 = ESMART0 = 4'b0010, 2 = ESMART2 = 4'b0011; those are the only two the
   * TRM lists as reaching port 0).  The reset default for every layer slot is
   * Disable, so this write is what makes any window data reach the POST.
   */

  {
    uint32_t layer0_sel = RK3576_VOP_ESMART_IDX == 2
                              ? RK3576_VOP_LAYER_SEL_ESMART2
                              : RK3576_VOP_LAYER_SEL_ESMART0;

    rk3576_vop_putreg(
        priv, ovl_base + RK3576_VOP_OVERLAY_LAYER_SEL,
        RK3576_VOP_LAYER_SEL_L0(layer0_sel) |
            RK3576_VOP_LAYER_SEL_L1(RK3576_VOP_LAYER_SEL_DISABLE) |
            RK3576_VOP_LAYER_SEL_L2(RK3576_VOP_LAYER_SEL_DISABLE) |
            RK3576_VOP_LAYER_SEL_L3(RK3576_VOP_LAYER_SEL_DISABLE));
  }

  /* OVERLAY MIX0 blend: passthrough the layer0 (ESMART0) framebuffer.
   *
   * The mixer's *_FACTOR_MODE and GLB_ALPHA fields reset to 0, which makes
   * the blend formula Cd = 0*Cs + 0*Cd = black regardless of the source.
   * This is the precise counterpart of Linux vop2_setup_alpha() for an
   * alpha-less (RGB888, no global-alpha) bottom layer:
   *   src: factor=Ags(3'b101), glb_alpha=0xff, color_mode=no-pre-mul,
   *        alpha_en=1, blend=global
   *   dst: factor=256-Ad0(3'b011), glb_alpha=0xff, blend=global
   * which yields the opaque copy Cd = Cs.
   */

  /* MIX0 source color: alpha_en + factor=Ags + glb_alpha=0xff,
   * color_mode=Cs (no pre-multiply), blend=global, straight alpha. */

  rk3576_vop_putreg(
      priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL,
      (0xffu << RK3576_VOP_MIX_CTRL_GLB_ALPHA_SHIFT) |
          RK3576_VOP_MIX_CTRL_ALPHA_EN |
          (RK3576_VOP_FACTOR_SRC_GLOBAL << RK3576_VOP_MIX_CTRL_FACTOR_SHIFT));

  /* MIX0 dest color: factor=256-Ad0 + glb_alpha=0xff (color_mode=Cd). */

  rk3576_vop_putreg(
      priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL,
      (0xffu << RK3576_VOP_MIX_CTRL_GLB_ALPHA_SHIFT) |
          (RK3576_VOP_FACTOR_DST_INVERSE << RK3576_VOP_MIX_CTRL_FACTOR_SHIFT));

  /* MIX0 source alpha: factor=256, blend=global. */

  rk3576_vop_putreg(
      priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_ALPHA_CTRL,
      (RK3576_VOP_FACTOR_ONE << RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT));

  /* MIX0 dest alpha: factor=256-Ad0, no-saturation. */

  rk3576_vop_putreg(
      priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_ALPHA_CTRL,
      (RK3576_VOP_FACTOR_DST_INVERSE << RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT) |
          RK3576_VOP_MIX_ALPHA_CAL_MODE);
}

/****************************************************************************
 * Name: rk3576_vop_channel_biu_enable
 *
 * Description:
 *   Open the VOP's DDR channel clock and take it out of reset.
 *
 *   TRM Part 1's clock dependency table lists aclk_vo0vop_channel_biu beside
 *   aclk_vop_biu as a clock that must not be gated: it is the clock of the
 *VOP's channel to the DDR subsystem.  This driver's biu_clocks[] never
 *included it and the clock tree never registered it, so nothing ever ungated
 *it -- and a gated channel BIU cannot issue an AXI transaction while every VOP
 *register still reads and writes normally and the POST still scans.
 *
 *   CRU_CLKSEL_CON19 (0x034C) selects and divides it; CRU_GATE_CON03 (0x080C)
 *   bits 1:0 gate aclk/hclk ("when high, disable clock", reset 0 = running);
 *   CRU_SOFTRST_CON06 (0x0A18) bits 1:0 are its resets ("when high, reset").
 ****************************************************************************/

static void rk3576_vop_channel_biu_enable(void)
{
  static const uint32_t cru = 0x27200000u;

  /* Release the reset and open both gates: data 0 = not reset, 0 = clock
   * running, with the hiword enabling those two low bits.
   */

  putreg32(0x00030000u | 0x0000u, cru + 0x0a18u);
  putreg32(0x00030000u | 0x0000u, cru + 0x080cu);
  up_udelay(20);
}

/****************************************************************************
 * Name: rk3576_vop_sys_masked_write
 *
 * Description:
 *   Write a SYS_CTRL register that uses the hiword write-mask scheme.  Several
 *   SYS_CTRL registers carry a "write_mask" field in bits[31:16], and a plain
 *   putreg() leaves that field at zero, so the write is discarded completely
 *and the register keeps its previous contents with nothing in a readback to
 *say so.  Two writes this driver depends on (SYS_PORT_CTRL_IMD and
 *   SYS_MMU_CTRL_IMD) were silently dropped for exactly this reason.
 *
 *   Registers WITHOUT the write mask -- SYS_AUTO_GATING_CTRL_IMD (0x0008),
 *   SYS_AXI0_CTRL_IMD (0x0010), SYS_AXI1_CTRL_IMD (0x001C) and
 *   SYS_AXI_LUT_CTRL_IMD (0x0024) -- keep using plain writes; for those,
 *   bits[31:16] hold ordinary data (auto_gating_en) or read-only status
 *   (axi0_mmu_idle), and writing a mask there would clobber them.
 *
 * Input Parameters:
 *   priv - VOP state
 *   off  - register offset within SYS_CTRL
 *   data - desired value of the masked bits
 *   mask - which bits to write
 *
 ****************************************************************************/

static void rk3576_vop_sys_masked_write(FAR struct rk3576_vop_s *priv,
                                        uint32_t off, uint32_t data,
                                        uint32_t mask)
{
  rk3576_vop_putreg(priv, RK3576_VOP_SYS_CTRL(priv->base) + off,
                    (data & mask) | (mask << 16));
}

/****************************************************************************
 * Name: rk3576_vop_trigger_cfg_done
 *
 * Description:
 *   Fire the mirror->real register load pulses for VP0: the global/system
 *groups (SYS_REG_CFG_DONE) and the ESMART0 layer group (SYS_WIN_REG_CFG_DONE).
 *The copies land at the start of the next frame.
 *
 *   sw_global_regdone_en (bit 15) must be kept set: it is an ordinary data bit
 *   inside the 16-bit data word, and leaving it 0 silently disables the global
 *   regdone, after which the VP0 global group's load bit stays pending for
 *ever and only the system group ever latches.
 *
 *   Called from configure_port(), and again right after the dclk reset pulse:
 *the reset restarts the VP0 timing generator and can swallow an in-flight
 *load.
 ****************************************************************************/

static void rk3576_vop_trigger_cfg_done(FAR struct rk3576_vop_s *priv)
{
  uint32_t sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  uint32_t post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);

  /* Commit exactly what Linux commits: bit15 (global enable) + bit14 (WB) +
   * bit4 (the VP's own system group).  ORing in CFG_DONE_ALL_GROUPS would also
   * set VP1's and VP2's load requests, which nothing on this board consumes,
   * so they stay pending for ever -- the TRM makes each bit a commit that
   * waits for its group to finish.
   */

  rk3576_vop_putreg(
      priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE,
      RK3576_VOP_CFG_DONE_LOAD_CTRL | RK3576_VOP_CFG_DONE_GLOBAL_REGDONE_EN |
          RK3576_VOP_CFG_DONE_WB_LOAD | RK3576_VOP_CFG_DONE_VP0_GROUPS);

  /* The layer group's own load enable: only the bit for the window actually in
   * use (ESMART0 = bit4) is pulsed.  Linux does not write this register on
   * RK3572 and later -- it commits through SYS_REG_CFG_DONE above -- but the
   * TRM's description of this register names the window mirror->real copy
   * explicitly, so it is kept.
   */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE,
                    RK3576_VOP_WIN_CFG_DONE_LOAD_CTRL |
                        RK3576_VOP_WIN_CFG_DONE_ESMART0);

  /* The RK3572+ frame commit: Linux commits a video port by writing bit0 of
   * POSTx + 0x0FC, which is none of the triggers above -- on this generation
   * it does not touch SYS_WIN_REG_CFG_DONE.  VOP_REG_MASK means hiword
   * write-enable, hence the 0x00010001.
   */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_CFG_DONE_OFF,
                    RK3576_VOP_POST_CFG_DONE_TRIGGER);
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
  uint32_t ovl_base = RK3576_VOP_OVERLAY_PORT(priv->base, priv->cfg.port);
  uint32_t regval;

  /* POST: RGB888 output mode, clear standby.  POST_DSP_CTRL is a mirror
   * register (reset value 0x8000000f: bit31 vop_standby_en=1, dsp_out_mode
   * =0xf).  Write the whole word with putreg -- do NOT use modifyreg, which
   * reads back the unloaded mirror (0) and can mis-handle the immediate
   * standby bit.  Value 0 = standby off + out_mode RGB888 + no black/blank.
   */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_CTRL,
                    RK3576_VOP_POST_OUT_RGB888);

  /* Disable the POST background colour (bg_display_en reset = 1, colours
   * reset = 0 = black).  With the OVERLAY mixers reset to factor 0, POST
   * otherwise substitutes its own black background in place of the layer
   * data, so even a correctly-routed framebuffer shows as black.  The
   * layer0 mixer (configured above) is now the sole source.
   */

  rk3576_vop_modifyreg(priv, post_base + RK3576_VOP_POST_DSP_BG,
                       RK3576_VOP_POST_BG_DISPLAY_EN, 0);

  /* Interface ctrl: select video port + enable output (and clock), while
   * preserving the reset-default sync polarities (Positive) and
   * regdone_imd_en.  MIPI0_INFACE_CTRL resets to vsync_pol=1/hsync_pol=1/
   * regdone_imd_en=1; writing the whole word would clear those and drive
   * inverted sync into the DSI IPI (which then never locks onto a frame).
   *
   * Set out_en + clk_out_en + port_sel, and keep hsync/vsync Positive +
   * regdone_imd_en (mirror -> real immediately).  cmd_mode stays 0 (Video).
   *
   * CRITICAL pixel-clock configuration (Linux rk3576_calc_cru_cfg()):
   * RK3576 VP0 is dual-pixel (pixel_rate=2), so the POST scan clock must be
   * HALF the panel pixel clock.  With dclk=64M the VOP-internal dclk_core
   * must be 32M (dclk/2), selected by POST_CORE_CLK.dclk_core_sel=1.  The
   * MIPI interface then takes dclk_core directly (mipi0_dclk_sel=0) and
   * divides by 2 (mipi0_pixclk_div=0) to feed the DSI IPI 16M
   * (= crtc_clock/4, matching PHY_IPI_RATIO=1.5).  Leaving dclk_core_sel=0
   * doubles the scan rate: dsp_vcnt0 runs at 2x and the pixel stream never
   * aligns with the DSI IPI clock domain -> all-black.
   */

  /* Pre-scan: how early the layers ask for their pixels (TRM 11.4 "H.
   * Pre_scan_active").  The reset value is 0, at which the layers request
   * their pixels at the moment the POST reads them out of the line buffer --
   * too late, so the buffer under-runs and the POST transmits undefined data.
   *
   *   pre_scan_dly = bg_dly + (roundup(hdisplay, 2) >> 1) - 1
   *   hblank       = max(hsync_len, 8)   (below 8 the window reset signal
   * makes the first line's data zero)
   *
   * bg_dly is 20 for RK3576 port0 (TRM Table 11-4: win_dly 10 + layer_mix_dly
   * 8
   * + hdr_mix_dly 2), not RK3568's 16.  OVERLAY_BG_MIX_CTRL and this register
   * are measured against the same mux output, so both are written.
   */

  {
    uint32_t bg_dly = RK3576_VOP_OVERLAY_BG_DLY_VP0;
    uint32_t hblank = (uint32_t)priv->cfg.hsync_len < 8u
                          ? 8u
                          : (uint32_t)priv->cfg.hsync_len;
    uint32_t hactive = bg_dly + (((uint32_t)priv->cfg.xres + 1u) / 2u) - 1u;
    uint32_t pre = ((hactive & 0x1fffu) << 16) | (hblank & 0x1fffu);
    uint32_t bgm =
        rk3576_vop_getreg(priv, ovl_base + RK3576_VOP_OVERLAY_BG_MIX_CTRL);
    uint32_t bgm_new = (bgm & ~RK3576_VOP_OVERLAY_BG_DLY_MASK) |
                       (bg_dly << RK3576_VOP_OVERLAY_BG_DLY_SHIFT);

    rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_BG_MIX_CTRL,
                      bgm_new);
    rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_PRE_SCAN_HTIMING, pre);
  }

  /* Bypass the VOP's MMU for everything.  This driver programs PHYSICAL
   * addresses and there is no IOMMU driver, yet the register was never
   * written: mmu_bypass_en sat at its reset value of 0, so the layer's
   * physical addresses were offered to an MMU with no page tables.  A read
   * into an unconfigured MMU need not report an error -- it can simply never
   * return.  bypass_id = 0 makes "rid > bypass_id" true for every window.
   */

  {
    /* SYS_MMU_CTRL_IMD carries the hiword write mask, so a plain putreg()
     * would be discarded and mmu_bypass_en would stay at its reset value of
     * 0 -- the layer's physical addresses would then be offered to an MMU
     * with no page tables.  bypass_id = 0 makes "rid > bypass_id" true for
     * every window, which is what a driver without an IOMMU needs.
     */

    rk3576_vop_sys_masked_write(
        priv, RK3576_VOP_SYS_MMU_CTRL_IMD,
        RK3576_VOP_SYS_MMU_BYPASS_EN | RK3576_VOP_SYS_MMU1_BYPASS_EN |
            RK3576_VOP_SYS_MMU_SOFT_RST_EN,
        RK3576_VOP_SYS_MMU_BYPASS_EN | RK3576_VOP_SYS_MMU1_BYPASS_EN |
            RK3576_VOP_SYS_MMU_BYPASS_ID_MASK |
            RK3576_VOP_SYS_MMU_SOFT_RST_EN);
  }

  /* Global VOP control the reference driver sets for RK3576 and this driver
   * never wrote.  See RK3576_VOP_SYS_PORT_CTRL_IMD in the header for the bit
   * map: dsp_vs_t_sel decides where the display's vs/t timing is tapped, and a
   * window that is enabled, routed and correct yet issues no reads is what a
   * POST unable to issue a coherent pixel demand looks like.
   */

  {
    uint32_t lut =
        rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_AXI_LUT_CTRL_IMD);

    /* reg_done_frm is written to 0 (Linux: "Set reg done every field for
     * interlace"); it decides when a video port's register updates become
     * valid.  dsp_vs_t_sel and auto_cs_en keep their reset value of 1 and
     * auto_cs_mode stays 0, which is what the working configuration holds.
     *
     * SYS_AXI_LUT_CTRL_IMD has NO write mask, so a plain write is correct.
     */

    rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_PORT_CTRL_IMD, 0u,
                                RK3576_VOP_SYS_PORT_REG_DONE_FRM_MASK);

    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI_LUT_CTRL_IMD,
                      lut & ~RK3576_VOP_SYS_LUT_USE_AXI1);
  }

  /* The anti-under-run mechanism: POST line-buffer urgency.  The reference
   * driver applies it unconditionally for RK3576 video port 0 (urgen_thl = 4,
   * urgen_thh = 6), enabling post_urgency_en, both axiN_port_urgency_en bits
   * and the two thresholds.
   *
   * The TRM states the mechanism in POST0_CTRL_POST_COLOR_CTRL: "When post_lb
   * < sw_urgency_thl, post set urgency to 1'b1.  Increase the priority of the
   * layer on the AXI0 bus."  With the VOP's read-urgency line high, the DDR
   * controller masks all other requests so this read port is served first.  It
   * is a closed hardware loop: the POST measures its own line-buffer occupancy
   * and asks the interconnect for priority when it is about to run dry.
   * Disabled, the layer's fetches compete on equal terms and the POST
   * under-runs.
   *
   * This layer fetches on AXI0 (esmart_axi_sel reset = 0), so the enable that
   * matters is CTRL0 bit24 = axi0_port0_urgency_en (the bit index follows the
   * video port: 24 = VP0, 25 = VP1, 26 = VP2); CTRL1 bit24 is set as well
   * because the reference sets the AXI1 one unconditionally.
   *
   * POST_COLOR_CTRL is a POST mirror register, so this write reaches the real
   * register only on the next frame boundary, via the cfg_done pulse below.
   */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_COLOR_CTRL,
                    RK3576_VOP_POST_URGENCY_EN |
                        (RK3576_VOP_POST_URGENCY_THL_LINUX
                         << RK3576_VOP_POST_URGENCY_THL_SHIFT) |
                        (RK3576_VOP_POST_URGENCY_THH_LINUX
                         << RK3576_VOP_POST_URGENCY_THH_SHIFT));

  {
    uint32_t h0 =
        rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_AXI_HURRY_CTRL0_IMD);
    uint32_t h1 =
        rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_AXI_HURRY_CTRL1_IMD);

    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI_HURRY_CTRL0_IMD,
                      h0 | RK3576_VOP_SYS_AXI_HURRY_AXI0_VP0_EN);
    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI_HURRY_CTRL1_IMD,
                      h1 | RK3576_VOP_SYS_AXI_HURRY_AXI1_VP0_EN);
  }

  /* dclk_core_sel = 1 (dclk_core = dclk/2 = 32M), dclk_out_sel = 0. */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_CORE_CLK,
                    RK3576_VOP_POST_CORE_CLK_DCLK_CORE_SEL);

  regval = RK3576_VOP_IFACE_CLK_OUT_EN | RK3576_VOP_IFACE_OUT_EN |
           RK3576_VOP_IFACE_VSYNC_POL | RK3576_VOP_IFACE_HSYNC_POL |
           RK3576_VOP_IFACE_REGDONE_IMD_EN |
           ((uint32_t)priv->cfg.port << RK3576_VOP_IFACE_PORT_SEL_SHIFT);
  rk3576_vop_modifyreg(priv, sys_base + iface_off,
                       RK3576_VOP_IFACE_OUT_EN | RK3576_VOP_IFACE_CLK_OUT_EN |
                           RK3576_VOP_IFACE_PORT_SEL_MASK |
                           RK3576_VOP_IFACE_POL_MASK |
                           RK3576_VOP_IFACE_REGDONE_IMD_EN,
                       regval);

  /* Fire the register-configure-done sequence (hiword write-mask scheme) to
   * copy all mirror registers (layer, overlay, POST timing/control) to
   * their real registers at the start of the next frame, and load the
   * ESMART0 layer mirror set.  See rk3576_vop_trigger_cfg_done().
   */

  rk3576_vop_trigger_cfg_done(priv);

  /* SYS_AXI0_CTRL_IMD is written as 0: no outstanding cap.  ESMART0 and
   * ESMART1 fetch through AXI0, and a dump from a working configuration reads
   * 0 here -- the working driver never writes this register.  Capping the
   * channel starves the window's scan-out DMA, so the POST under-runs and
   * emits its own fixed pattern instead of the framebuffer.
   *
   * bit0 is dma_stop and bit1 is outstanding_en.  dma_stop must stay 0: with
   * the two confused, this write once stopped the very channel the layer
   * fetches through.
   */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI0_CTRL_IMD, 0u);

  /* AXI1 is deliberately left alone: this window does not fetch through it and
   * the register has never been written on this board. */

  /* --- dclk reset pulse: align the VOP pixel clock to the DSI link ---
   *
   * Linux's dw_mipi_dsi2 encoder pulses the dclk reset (assert 10us +
   * deassert) after the DSI has fully entered Video mode; this re-locks the
   * VOP pixel clock to the ready DSI link so the controller's phy_tx_ready FSM
   * can leave INIT.
   *
   * SAFETY: pulse ONLY dresetn_vp0 (SOFTRST_CON61 bit13, the VP0 pixel clock
   * reset).  Never pulse aresetn_vop/hresetn_* (bits 4-9): those are the
   * BIU/AXI resets, and asserting them against a live clocked AXI master tears
   * down in-flight transactions and hangs the NoC/DDR.
   */

  {
    uint32_t cru =
        RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_VOP_RST_CON);

    /* assert: bit13 data=1 with hiword mask (reset held). */

    putreg32(RK3576_VOP_HWM(RK3576_VOP_RST_DRESETN_VP0) |
                 RK3576_VOP_RST_DRESETN_VP0,
             cru);
    up_udelay(10);

    /* deassert: bit13 data=0 (release). */

    putreg32(RK3576_VOP_HWM(RK3576_VOP_RST_DRESETN_VP0), cru);
    up_udelay(20);

    /* The reset restarts the VP0 timing generator and can swallow an in-flight
     * mirror->real copy, so re-post the load requests.
     */

    rk3576_vop_trigger_cfg_done(priv);
  }
}

/****************************************************************************
 * Name: rk3576_vop_enable_clocks
 ****************************************************************************/

/* The VOP AXI-root and pixel-clock muxes reset to PLLs that are not modelled
 * in rk3576_clk_tree.c (clk_spll/clk_lpll/clk_vpll/clk_bpll are orphan), so
 * clk_get_rate() on aclk_vop reads back 0 and the ESMART/POST layers are left
 * unclocked.  Force the source muxes onto clk_gpll (1188 MHz), which IS
 * registered.  Remove once the full PLL set is modelled.
 */

static void rk3576_vop_reparent_clocks(FAR struct rk3576_vop_s *priv)
{
  struct clk_s *gpll = clk_get("clk_gpll");
  struct clk_s *mux;
  int ret;

  if (gpll == NULL)
    {
      gerr("ERROR: VOP failed to get clk_gpll\n");
      return;
    }

  /* aclk_vop_root_sel: the AXI clock source mux (reset -> orphan PLL). */

  mux = clk_get("aclk_vop_root_sel");
  if (mux != NULL)
    {
      ret = clk_set_parent(mux, gpll);
      if (ret < 0)
        {
          gerr("ERROR: VOP failed to reparent aclk_vop_root_sel: %d\n", ret);
        }
    }

  /* Clamp aclk_vop to <= 700 MHz (TRM Part 2, 11.3.1: VOP ACLK max op
   * frequency is 700 MHz).  The orphan-PLL workaround above left
   * aclk_vop_root_div at its reset value (div-by-1), feeding clk_gpll's
   * raw 1188 MHz straight into the AXI domain -- well over the 700 MHz
   * design limit, which can lock the VOP BIU/AXI bus and hang the SoC.
   * Setting the rate propagates up and programs aclk_vop_root_div.
   */

  if (priv->aclk != NULL)
    {
      ret = clk_set_rate(priv->aclk, 594000000);
      if (ret < 0)
        {
          gerr("ERROR: VOP failed to set aclk_vop rate below 700MHz: %d\n",
               ret);
        }
    }

  /* dclk_vpN_sel: final pixel-clock select (0 = *_src, 1 = hdmiphy pixel).
   * Force onto the src path so the divider chain below can drive the rate.
   */

  mux = clk_get(g_rk3576_vop_dclk_names[priv->cfg.port]);
  if (mux != NULL)
    {
      /* Reparent the *_src_sel mux (the 3-bit PLL selector) onto gpll. */
      char src_sel_name[24];

      snprintf(src_sel_name, sizeof(src_sel_name), "%s_src_sel",
               g_rk3576_vop_dclk_names[priv->cfg.port]);
      mux = clk_get(src_sel_name);
      if (mux != NULL)
        {
          ret = clk_set_parent(mux, gpll);
          if (ret < 0)
            {
              gerr("ERROR: VOP failed to reparent %s: %d\n", src_sel_name,
                   ret);
            }
        }
    }
}

static int rk3576_vop_enable_clocks(FAR struct rk3576_vop_s *priv)
{
  static const char *const biu_clocks[] = {
    "aclk_vop_biu",
    "aclk_vop2_biu",
    "hclk_vop_biu",
    "pclk_vop_biu",
  };
  struct clk_s *biu;
  int ret;
  int i;

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

  /* Force orphan PLL muxes onto clk_gpll before enabling (temporary). */

  rk3576_vop_reparent_clocks(priv);

  /* Set the pixel clock to the rate the caller asked for (it must equal what
   * the output interface was programmed with).  The rate propagates up through
   * dclk_vpN_sel/_src/_src_div/_src_sel.
   */

  ret = clk_set_rate(priv->dclk, priv->cfg.pixel_clock != 0
                                     ? priv->cfg.pixel_clock
                                     : RK3576_VOP_DEFAULT_PCLK_HZ);
  if (ret < 0)
    {
      gerr("ERROR: VOP failed to set %s rate: %d\n",
           g_rk3576_vop_dclk_names[priv->cfg.port], ret);
      return ret;
    }

  /* Self-check the clock the whole MIPI pixel path hangs off.  clk_set_rate()
   * is a request: the divider search keeps the largest divisor whose output is
   * still
   * <= the request, so the achieved rate normally sits below it by a whole
   * divider step.  The DSI computes its IPI timing and PHY_IPI_RATIO from the
   * value it was handed, so the board must request a rate the parent PLL
   * divides exactly (1188/20 = 59.4 MHz, not a nominal 62 MHz).
   *
   * rk3576_mipi_dsi_update_pixel_clock() is deliberately NOT called to paper
   * over this: it rewrites the timing of a live pixel datapath.
   */

  {
    uint32_t requested = priv->cfg.pixel_clock != 0
                             ? priv->cfg.pixel_clock
                             : RK3576_VOP_DEFAULT_PCLK_HZ;
    uint32_t achieved = (uint32_t)clk_get_rate(priv->dclk);
    uint32_t diff =
        achieved > requested ? achieved - requested : requested - achieved;
    uint32_t ppm = requested != 0
                       ? (uint32_t)(((uint64_t)diff * 1000000u) / requested)
                       : 0;

    /* 500 ppm is far below a divider step (4.2% = 42000 ppm) and far above
     * what integer arithmetic alone can produce, so it cannot false-alarm. */

    if (requested != 0 && ppm > 500)
      {
        gerr("WARNING: VOP dclk_vp%u achieved %lu Hz but %lu Hz was "
             "requested (%lu ppm off) -- the DSI IPI timing and "
             "PHY_IPI_RATIO were computed from the REQUESTED value, so the "
             "pixel timing the controller expects does not match the stream "
             "it receives.  Request a rate the parent PLL divides into "
             "exactly.\n",
             (unsigned int)priv->cfg.port, (unsigned long)achieved,
             (unsigned long)requested, (unsigned long)ppm);
      }
  }

  /* Do NOT re-program the DSI's IPI timing here: by this point the DSI is
   * already in video mode, so rewriting HSA/HBP/HACT/HLINE and PHY_IPI_RATIO
   * re-times a running state machine asynchronously to the stream.  The
   * reference driver computes both from the nominal mode clock, and a ~1%
   * nominal-vs-achieved mismatch is the normal condition on this SoC.
   */

  /* Enable the VOP BIU (bus-interface-unit) clocks.  The ESMART/POST layers
   * reach DDR over the VOP AXI port, clocked by aclk_vop_biu / hclk_vop_biu;
   * these gates are set-to-disable and are not auto-enabled by enabling
   * aclk_vop.  Left gated, the first ESMART scan-out read enters a clockless
   * BIU and pends for ever, seizing a NoC outstanding slot and hanging the
   * SoC.
   */

  for (i = 0; i < nitems(biu_clocks); i++)
    {
      biu = clk_get(biu_clocks[i]);
      if (biu == NULL)
        {
          continue; /* Not registered: skip (older clock trees). */
        }

      ret = clk_enable(biu);
      if (ret < 0)
        {
          gerr("ERROR: VOP failed to enable %s: %d\n", biu_clocks[i], ret);
        }
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
 *   Release (de-assert) the VOP core resets.  The CRU reset bits are
 *   "When high, reset relative logic", so write 0 (with hiword write-mask)
 *   to release.  Only the de-assert side is written: pulsing the assert
 *   side tears down the already-clocked BIU's AXI transactions and hangs
 *   the NoC/DDR (SoC-wide hang).
 ****************************************************************************/

static void rk3576_vop_reset(FAR struct rk3576_vop_s *priv)
{
  uint32_t reg = RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_VOP_RST_CON);

  /* De-assert (release) the VOP core only.  Do NOT pulse the assert side:
   * asserting aresetn_biu/hresetn_biu while the VOP AXI master is already
   * clocked (clk_enable above) tears down in-flight AXI transactions and
   * leaves a hung outstanding slot on the NoC/DDR that then starves the
   * CPU's DDR traffic -> SoC hang (this was the observed root cause).
   *
   * The VOP is already out of reset from the boot stage, so a deassert-only
   * write (data 0) is sufficient; the reset bits are "When high, reset" so
   * writing 0 releases them without disturbing a live bus interface.
   */

  putreg32(RK3576_VOP_HWM(RK3576_VOP_RST_MAIN_MASK), reg);
  up_udelay(20);
}

/****************************************************************************
 * Name: rk3576_vop_power_on_esmart
 *
 * Description:
 *   Power on the VOP_ESMART power domain and bring the ESMART layer out of
 *reset. The ESMART layer has no standalone clock gate in the CRU: its
 *aclk_esmart0..3 come from aclk_vop via the VOP-internal ESMART_PD_CRU once
 *PD_VOP_ESMART is up.
 *
 *   1. Release the BISR initial reset (initrst) for PD_VOP + PD_VOP_ESMART +
 *      PD_VOP_CLUSTER; until then the PD reset (aresetn_pd) is asserted and
 *every ESMART register reads back 0.
 *   2. Clear pd_vop_dwn_ena and pd_vop_esmart_dwn_ena so both domains stay
 *      powered ('0' = keep powered, hiword mask).
 *   3. Write ESMART_CTRL0 = 0 (see the note at the call site).
 ****************************************************************************/

static void rk3576_vop_power_on_esmart(FAR struct rk3576_vop_s *priv)
{
  uint32_t pmu_pwr = RK3576_PMU_ADDR + RK3576_VOP_PMU_PWR_GATE_CON0_OFF;
  uint32_t pmu_initrst =
      RK3576_PMU_ADDR + RK3576_VOP_PMU_BISR_INITRST_SFTCON0_OFF;
  uint32_t esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);

  /* 1. Release the PD memory-repair initial reset (write 1 = Normal) for the
   * three VOP domains this driver needs.  The state of the other two
   * (PD_VO0/PD_VO1) is printed but deliberately not changed: releasing the
   * output domain's initial reset after the DSI is already up regressed a
   * working output path.  See RK3576_VOP_INITRST_RELEASE_MASK.
   */

  /* Release the PD memory-repair initial reset (write 1 = Normal) for the
   * three VOP domains this driver needs.  See
   * RK3576_VOP_INITRST_RELEASE_MASK.
   */

  putreg32(RK3576_VOP_HWM(RK3576_VOP_INITRST_RELEASE_MASK) |
               RK3576_VOP_INITRST_RELEASE_MASK,
           pmu_initrst);
  up_udelay(20);

  /* 2. Keep the display power domains powered: write-enable both this driver
   * needs (mask) with data 0 (so *_dwn_ena = 0 -> not powered down).
   *
   * PD_VO0/PD_VO1 (bits 15/14) are deliberately NOT written: touching the
   * output domain's power/reset state after the DSI has been brought up was
   * tried and it regressed a working output path.  See
   * RK3576_VOP_INITRST_RELEASE_MASK.  Their state is printed, not changed.
   */

  putreg32(RK3576_VOP_HWM(RK3576_VOP_PD_VOP_ESMART_DWN_ENA |
                          RK3576_VOP_PD_VOP_DWN_ENA),
           pmu_pwr);
  up_udelay(20);

  /* ESMART_CTRL0 = 0 for this window.  esmart_frm_resetn_en (bit31) resets to
   * 0 and the reference driver never writes this register, so 0 is what every
   * Rockchip board runs.  The TRM defines the bit as "1'b0: Disable 1'b1:
   * Enable" for a soft reset that fires on every vsync and resets the layer's
   * internal logic "exclude register logic" -- setting it would leave the
   * register file reading back perfectly while the fetch engine and line
   * buffers are torn down once per frame.  esmart_scl_num (bits 13:12) is 0
   * for ESMART0, matching the reset value.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_CTRL0,
                    RK3576_VOP_ESMART_SCL_NUM);
}

/****************************************************************************
 * Name: rk3576_vop_biu_active
 *
 * Description:
 *   Take the VOP's Bus Interface Units out of idle.
 *
 *   The TRM's software power-up procedure (6.5.6.2) has three steps: power the
 *   domain, wait for it, and then "do BIU active operation by software" --
 *send the BIU active request by clearing the bit in PMU_BIU_IDLE_SFTCON0/1,
 *then wait on PMU_IDLE_ACK_STS / PMU_IDLE_STS.  A module whose BIU is held
 *idle reads and writes its configuration registers normally while being unable
 *to issue a single AXI transaction.
 *
 *   PMU is at 0x27360000: 0x20110 PMU_BIU_IDLE_SFTCON0 (software-mode request,
 *   hiword write-enable, reset value 0 = every BIU active), 0x20120
 *   PMU_BIU_IDLE_ACK_STS, 0x20128 PMU_BIU_IDLE_STS.  Writing 0 with the whole
 *   hiword enabled restores the reset state.
 ****************************************************************************/

static void rk3576_vop_biu_active(void)
{
  static const uint32_t pmu = RK3576_PMU_ADDR;

  /* Request ACTIVE for every bus master: data = 0, write-enable = all 16 low
   * bits.  This is the reset state, so it can only move things towards the
   * power-on default.  A module whose BIU is held idle still reads and writes
   * its configuration registers normally while being unable to issue a single
   * AXI transaction -- see the TRM's power-up sequence 6.5.6.2, step 3.
   */

  putreg32(0xffff0000u | 0x0000u, pmu + 0x20110u);
  up_mdelay(5);
}

/****************************************************************************
 * Name: rk3576_vop_power_domain_on
 *
 * Description:
 *   Power on the VOP's two internal power domains: CLUSTER (cluster0/1) and
 *   ESMART (esmart0/1/2/3).
 *
 *   The reference driver performs this before anything else, for every video
 *   port.  For RK3576 the control bits are
 *
 *     SYS_ESMART_PD_CTRL_IMD  (0x0034) bit0  esmart_pd_en     reset 0
 *     SYS_CLUSTER_PD_CTRL_IMD (0x0030) bit0  cluster01_pd_en  reset 0
 *
 *   where 1'b0 = power on (power-down is the one that waits for a regdone).
 *Both registers carry the hiword write mask, so a plain putreg() would be
 *   discarded -- use rk3576_vop_sys_masked_write().
 *
 *   This matters because a powered-down ESMART still answers APB accesses to
 *its configuration registers, so every readback is equally consistent with a
 *   window that is correctly programmed and physically unable to fetch.
 ****************************************************************************/

static void rk3576_vop_power_domain_on(FAR struct rk3576_vop_s *priv)
{

  /* Power on the CLUSTER and ESMART domains (data bit = 0 = on, with the
   * hiword write mask).  Power-on takes effect immediately; power-down is the
   * one that waits for a regdone.
   */

  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_ESMART_PD_CTRL_IMD, 0u,
                              RK3576_VOP_SYS_ESMART_PD_EN);
  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_CLUSTER_PD_CTRL_IMD, 0u,
                              RK3576_VOP_SYS_CLUSTER01_PD_EN);

  /* esmart_lb_mode = 3 (2 x 4k + 2 x 2k), the reference default and the reset
   * value.  See RK3576_VOP_SYS_ESMART_LB_MODE_4K_4K_2K_2K.
   */

  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_ESMART_PD_CTRL_IMD,
                              RK3576_VOP_SYS_ESMART_LB_MODE_4K_4K_2K_2K
                                  << RK3576_VOP_SYS_ESMART_LB_MODE_SHIFT,
                              RK3576_VOP_SYS_ESMART_LB_MODE_MASK);

  up_mdelay(10);
}

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

  /* Start from a known, cache-clean state; the caller paints the real content
   * through rk3576_vop_fill().  The ESMART layer reads this memory with the
   * MMU bypassed, so it must be cleaned to memory before scan-out starts.
   */

  memset(priv->fbmem, 0, priv->fblen);
  up_clean_dcache((uintptr_t)priv->fbmem,
                  (uintptr_t)priv->fbmem + priv->fblen);

  /* Bring up clocks and release resets. */

  ret = rk3576_vop_enable_clocks(priv);
  if (ret < 0)
    {
      goto errout_with_fb;
    }

  rk3576_vop_reset(priv);

  /* Power on the VOP's internal CLUSTER and ESMART domains FIRST: a
   * powered-down ESMART still answers APB accesses, so every later readback
   * would look correct while the window was unable to fetch.  See
   * rk3576_vop_power_domain_on(). */

  rk3576_vop_power_domain_on(priv);

  /* Power on the ESMART domain and release the esmart layer reset before
   * touching any ESMART registers (aclk_esmart derives from this domain). */

  rk3576_vop_power_on_esmart(priv);

  /* Take the VOP's bus interface out of idle -- the third step of the TRM's
   * power-up sequence (6.5.6.2), which decides whether the module can issue
   * AXI transactions at all.  See rk3576_vop_biu_active(). */

  rk3576_vop_biu_active();

  /* Open the VOP's DDR channel clock (aclk_vo0vop_channel_biu), which the
   * clock tree never registered and nothing ever ungated.  See
   * rk3576_vop_channel_biu_enable(). */

  rk3576_vop_channel_biu_enable();

  /* Program layer, timing and output routing. */

  rk3576_vop_configure_layer(priv);
  rk3576_vop_configure_timing(priv);
  rk3576_vop_configure_port(priv);

  /* Self-register the framebuffer device. */

  ret = fb_register_device(config->display, config->plane, &priv->vtable);
  if (ret < 0)
    {
      gerr("ERROR: VOP fb_register_device() failed: %d\n", ret);
      goto errout_with_clocks;
    }

  /* Publish the handle used by the driver's internal helpers. */

  g_rk3576_vop_priv = priv;

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

/****************************************************************************
 * Name: rk3576_vop_fill
 *
 * Description:
 *   Fill the whole framebuffer with one solid colour and make it visible to
 *   the scan-out path.  See the prototype in rk3576_vop.h.
 *
 ****************************************************************************/

int rk3576_vop_fill(uint32_t rgb)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint8_t r = (uint8_t)(rgb >> 16);
  uint8_t g = (uint8_t)(rgb >> 8);
  uint8_t b = (uint8_t)(rgb);
  uint8_t *fb;
  size_t n;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  fb = (uint8_t *)priv->fbmem;

  for (n = 0; n < priv->fblen; n += 3)
    {
      fb[n + 0] = r;
      fb[n + 1] = g;
      fb[n + 2] = b;
    }

  /* The ESMART layer DMAs this memory with the MMU bypassed, so the cache
   * must be cleaned or the display keeps scanning out stale lines. */

  up_clean_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);

  return OK;
}

#endif /* CONFIG_RK3576_VOP */
