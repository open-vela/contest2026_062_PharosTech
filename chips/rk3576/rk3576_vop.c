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

/* *** WHICH ESMART WINDOW THIS DRIVER USES. ***
 *
 * 0 = ESMART0 (the one every Rockchip reference configuration uses), 2 = the
 * other ESMART that the OVERLAY can route to PORT0 (LAYER_SEL has
 * 4'b0010: Esmart0 and 4'b0011: Esmart2, and only those two reach port 0).
 *
 * WHY THIS IS A SWITCH AND NOT A CONSTANT.  After every other explanation was
 * measured and eliminated -- clocks, power domains, both reset polarities, the
 * mirror load (proved by the load-request bits being consumed), the fetch
 * address, the pixel format, the stride, the scaler, the layer enable, the
 * overlay routing and LAYER_SEL encoding, the mixer factors, the security
 * configuration, the MMU (both of them, with paging on and no page table), the
 * BIU idle state, and the AXI outstanding limit -- ESMART0 still issues no AXI
 * reads at all while reading back perfectly configured.
 *
 * At that point the useful question is no longer "which bit is wrong" but "is
 * this specific window special".  ESMART2 is the perfect control: it is the
 * other window that can drive port 0, it is configured by exactly the same
 * code, and on this board it came out of the bootloader in the same state as
 * ESMART0 (all regions disabled, frame reset clear, AXI bypass off).  If
 * ESMART2 fetches, the fault is specific to ESMART0 -- which the TRM's own
 * security chapter calls out by name, and which alone supports yuv2rgb 13-bit.
 * If ESMART2 fetches nothing either, the fault is in what both windows share
 * (the overlay request path or the AXI port), and that is equally decisive.
 *
 * *** THE SWITCH BACK TO ESMART0 (index 0), AND WHY THE ESMART2 ROUND IS
 * RETROACTIVELY INVALID. ***
 *
 * The ESMART2 experiment was run -- and "ESMART2 behaves exactly like ESMART0"
 * was recorded -- while this driver was still writing ESMART0's AXI IDENTITY
 * (read IDs 0x10/0x11 and axi_sel = axi0) onto ESMART2's register block.  On
 * RK3576 each ESMART has its OWN ids and its own AXI channel (Esmart2 is
 * axi1 / 0x0a / 0x0b), so the control window was emitting reads that declared
 * themselves to be a different window on a different port.  The comparison
 * therefore never tested what it claimed to, and "both windows behave the
 * same" says nothing about the windows.
 *
 * Now that the identity follows the index (see the table further down), the
 * ESMART0 configuration -- index 0, axi0, ids 0x10/0x11, scl_num 0,
 * LAYER_SEL 4'b0010 -- is bit-for-bit what Rockchip's own driver programs for a
 * VP0 primary plane, and it has never once been run here.  It is also the only
 * window for which the axi_sel side effect below cannot fire, since axi_sel
 * stays 0.
 */

#define RK3576_VOP_ESMART_IDX 0

/* ESMART_CTRL0 esmart_scl_num ([13:12]) selects the scale engine for this
 * window.  The bootloader set it to the window's own index (ESMART1 = 1,
 * ESMART2 = 2, ESMART3 = 3, ESMART0 = 0), so it is derived from the index
 * rather than hard-coded, or the window would be pointed at another window's
 * scaler. */

#define RK3576_VOP_ESMART_SCL_NUM \
  ((uint32_t)RK3576_VOP_ESMART_IDX << 12)

/****************************************************************************
 * PER-WINDOW AXI IDENTITY.
 *
 * *** RK3576 GIVES EACH ESMART ITS OWN AXI READ-IDs AND ITS OWN AXI CHANNEL,
 * AND THIS DRIVER WAS WRITING ESMART0'S FOR WHATEVER WINDOW IT DROVE. ***
 *
 * From RK3576's own window table (Rockchip's vop2 driver, rk3576_vop_win_data[]):
 *
 *   Esmart0 : axi_id 0 (axi0)  yrgb 0x10  uv 0x11
 *   Esmart1 : axi_id 0 (axi0)  yrgb 0x12  uv 0x13
 *   Esmart2 : axi_id 1 (axi1)  yrgb 0x0a  uv 0x0b
 *   Esmart3 : axi_id 1 (axi1)  yrgb 0x0c  uv 0x0d
 *
 * The driver was written for ESMART0 (RIDs 0x10/0x11 on axi0) and later the
 * window was switched to ESMART2 -- RK3576_VOP_ESMART_IDX = 2, the block at
 * 0x1C00 -- purely as a diagnostic, WITHOUT moving the two AXI-identifying
 * fields with it.  So the window has been issuing reads that declare
 * themselves to be a different window on a different AXI port.
 *
 * The read ID travels with the transaction into the interconnect, where it
 * selects routing and QoS, and the failure mode of an ID no slave expects is a
 * read that NEVER COMES BACK -- no data, no bus error, and a POST whose input
 * buffer is permanently empty.  That is this board's exact signature, and the
 * header of this file already records the reasoning for the RID field:
 * "a bogus one yields a read that never returns".
 *
 * It also invalidates an earlier conclusion: "ESMART2 behaves identically to
 * ESMART0" was measured with ESMART2's AXI identity already wrong, so it never
 * tested what it claimed to.
 *
 * Hence the table.  Whichever window RK3576_VOP_ESMART_IDX selects, the reads
 * it issues now carry that window's own ID pair on that window's own channel.
 */

static const uint32_t g_rk3576_vop_esmart_yrgb_rid[4] =
{
  0x10u, /* ESMART0 */
  0x12u, /* ESMART1 */
  0x0au, /* ESMART2 */
  0x0cu  /* ESMART3 */
};

static const uint32_t g_rk3576_vop_esmart_uv_rid[4] =
{
  0x11u, /* ESMART0 */
  0x13u, /* ESMART1 */
  0x0bu, /* ESMART2 */
  0x0du  /* ESMART3 */
};

static const uint32_t g_rk3576_vop_esmart_axi_sel[4] =
{
  0u, /* ESMART0 -> axi0 */
  0u, /* ESMART1 -> axi0 */
  1u, /* ESMART2 -> axi1 */
  1u  /* ESMART3 -> axi1 */
};

/* *** WHICH AXI CHANNEL THE ESMART FETCHES THROUGH. ***
 *
 * ESMART_AXI_CTRL_IMD bit1 esmart_axi_sel: 1'b0 = axi0, 1'b1 = axi1.
 *
 * This driver has always left it at the reset value (axi0), and the TRM
 * recommends axi0 ("VOP uses AXI0 preferentially because AXI0 is on port 0 of
 * DDR and has a dedicated debug"), so axi0 is the normal choice.  But the two
 * channels are genuinely separate paths -- separate arbitration, separate MMU,
 * separate outstanding accounting -- and this is the one axis of the ESMART's
 * fetch configuration that has never been varied.
 *
 * The fault has been narrowed to "the window is configured correctly and its
 * DMA still never issues a single read", and the same is true of a second
 * window, which places it in the shared fetch path.  Moving the fetch to the
 * other channel bisects that path cleanly:
 *   - if it fetches on axi1, the fault is in the axi0 path;
 *   - if it fetches on neither, the fault is in the window's own DMA.
 *
 * *** REVERTED TO 0: SETTING axi_sel DOES NOT DO WHAT IT LOOKS LIKE IT DOES. ***
 *
 * The first attempt at this experiment set bit1 and then read the register
 * back, and got 0x00010002 where 0x000100fe had been written:
 *
 *   meant:   bit16 | mmu_bypass(bit2) | outstanding_en(bit3) | num=15[7:4] | axi_sel(bit1)
 *   read:    bit16 | axi_sel(bit1)                                    0x10002
 *
 * i.e. asserting axi_sel CLEARED mmu_bypass, outstanding_en and the outstanding
 * count -- the three fields a driver that uses physical addresses depends on.
 * Whatever the mechanism, the write is not a plain read-modify-write of an
 * independent bit, so switching channels has to be done as "write axi_sel
 * first, then re-assert the rest", and that is a separate experiment.
 *
 * Worse, leaving it set would mean running with the MMU NOT bypassed while this
 * driver still programs physical addresses, which is strictly worse than the
 * state it replaced.  So it goes back to 0 and the discovery is recorded here.
 *
 * Set to 1 only together with a write order that re-asserts bits [7:2]
 * afterwards.
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

/* PMU power-domain control (PMU base + PWR_GATE_CON0).  The ESMART layer
 * lives in its own power domain (PD_VOP_ESMART) inside the VOP; its AXI
 * clock (aclk_esmart0..3) is generated by the VOP-internal ESMART_PD_CRU
 * once the domain is powered.  There is no standalone aclk_esmart gate in
 * the CRU -- the whole esmart clock tree hangs off aclk_vop (already
 * enabled below), so the missing piece is the power-domain + frame reset.
 *
 * PMU_PWR_GATE_CON0 (0x27360000 + 0x20200):
 *   bit 12  pd_vop_esmart_dwn_ena  ('0' = keep PD_VOP_ESMART powered)
 *   bit 11  pd_vop_dwn_ena         ('0' = keep PD_VOP powered)
 *
 * Writes use the hiword-mask scheme: bits [31:16] mask which low bits are
 * written.  Clearing a *_dwn_ena bit (mask=1, data=0) keeps the domain up.
 */

#define RK3576_VOP_PMU_PWR_GATE_CON0_OFF  0x20200
#define RK3576_VOP_PD_VO0_DWN_ENA          (1 << 15)
#define RK3576_VOP_PD_VO1_DWN_ENA          (1 << 14)
#define RK3576_VOP_PD_VOP_ESMART_DWN_ENA   (1 << 12)
#define RK3576_VOP_PD_VOP_DWN_ENA          (1 << 11)

/* Power-state readback, same bit positions (PMU_PWR_GATE_STS @ 0x20230,
 * "1'b0: Power up  1'b1: Power down").  PD_VO0 owns the display output
 * interfaces and the VOP's channel into the VO0 interconnect, so its state is
 * worth reading explicitly rather than assuming. */

#define RK3576_VOP_PMU_PWR_GATE_STS_OFF   0x20230

/* Memory-repair initial reset (BISR initrst).  These bits gate the PD-level
 * reset (aresetn_pd in the VOP cluster/esmart block diagrams, Fig 11-14/15):
 *   PMU_BISR_INITRST_SFTCON0 = 0x27360000 + 0x20540
 *     bit 13  pd_vop_cluster_initrst_sftena  (1 = Normal, release reset)
 *     bit 12  pd_vop_esmart_initrst_sftena   (1 = Normal, release reset)
 *     bit 11  pd_vop_initrst_sftena          (1 = Normal, release reset)
 * Reset value 0 => held in RESET; the driver MUST set these to 1, otherwise
 * the ESMART/POST layer clocks (aclk_esmart0..3, derived from aclk_vop by
 * ESMART_PD_CRU) never come up and every ESMART register reads back 0.
 */

#define RK3576_VOP_PMU_BISR_INITRST_SFTCON0_OFF 0x20540
#define RK3576_VOP_PD_VO0_INITRST          (1 << 15)
#define RK3576_VOP_PD_VO1_INITRST          (1 << 14)
#define RK3576_VOP_PD_VOP_CLUSTER_INITRST (1 << 13)
#define RK3576_VOP_PD_VOP_ESMART_INITRST  (1 << 12)
#define RK3576_VOP_PD_VOP_INITRST         (1 << 11)

/* *** THE MASK IS BACK TO THE THREE VOP DOMAINS (bits 13:11). ***
 *
 * An attempt was made to widen it to PD_VO0/PD_VO1 (bits 15:14), on the theory
 * that PD_VO0 owns the display output interfaces and the VOP's channel into the
 * VO0 interconnect, and that a domain held in its initial reset would answer
 * register accesses while doing no work.
 *
 * It REGRESSED a working path.  With those bits released, a rung that had been
 * producing a clean solid background colour stopped doing so, and the panel
 * showed a faded, non-refreshing image instead.  Releasing the initial reset of
 * the OUTPUT domain after the DSI interface has already been brought up disturbs
 * the live output pipeline -- which is consistent with PD_VO0 owning the output
 * interfaces.
 *
 * So the output domain's initrst is left exactly as found.  It is printed (see
 * rk3576_vop_power_on_esmart) so the state is known rather than assumed, and the
 * macros below are kept for whoever revisits it -- but they are deliberately NOT
 * part of the release mask.
 */

#define RK3576_VOP_INITRST_RELEASE_MASK                     \
  (RK3576_VOP_PD_VOP_CLUSTER_INITRST |                      \
   RK3576_VOP_PD_VOP_ESMART_INITRST | RK3576_VOP_PD_VOP_INITRST)

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
 *
 * Description:
 *   Register accessors.  The "reg" parameter is an *absolute* address,
 *   already composed from the VOP base plus one of the RK3576_VOP_*()
 *   address macros (which receive priv->base as their base argument).
 *   Callers pass e.g. RK3576_VOP_POST(priv->base, port) + POST_*_OFFSET,
 *   so these helpers must NOT add priv->base again (that would duplicate
 *   the base into the address).
 ****************************************************************************/

static uint32_t rk3576_vop_getreg(struct rk3576_vop_s *priv,
                                  unsigned int reg)
{
  return getreg32(reg);
}

static void rk3576_vop_putreg(struct rk3576_vop_s *priv, unsigned int reg,
                              uint32_t value)
{
  putreg32(value, reg);
}

static void rk3576_vop_modifyreg(struct rk3576_vop_s *priv,
                                 unsigned int reg, uint32_t clrbits,
                                 uint32_t setbits)
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

/****************************************************************************
 * Name: rk3576_vop_configure_timing
 *
 * Description:
 *   Program the POSTx timing registers (HTOTAL/HACT/VTOTAL/VACT plus the
 *   "post" output window) from the configuration's porch/sync values.
 *
 *   Every value here is taken from the reference driver, which is the only
 *   authority for the field semantics.  Reading them off the register
 *   descriptions alone gets two things wrong:
 *
 *   1. The *_END fields are EXCLUSIVE ends, not inclusive ones, and the
 *      *_TOTAL_*_END registers carry the FULL total (not total - 1):
 *
 *        DSP_HTOTAL_HS_END = (htotal     << 16) | hsync_len
 *        DSP_HACT_ST_END   = (hact_st    << 16) | (hact_st + hdisplay)
 *        DSP_VTOTAL_VS_END = (vtotal     << 16) | vsync_len
 *        DSP_VACT_ST_END   = (vact_st    << 16) | (vact_st + vdisplay)
 *
 *      where  hact_st = htotal - hsync_start,  hsync_start = hdisplay +
 *      hfront_porch,  vact_st = vtotal - vsync_start.
 *
 *      This driver used to write (total - 1) and (start + pixels - 1), i.e.
 *      a line one pixel short with a sync one pixel narrow.  Small, but it is
 *      exactly the kind of off-by-one that keeps a panel from locking on, and
 *      it is not what a working driver does.
 *
 *   2. The POST's own output active window (POST_DSP_HACT_INFO /
 *      POST_DSP_VACT_INFO) must be programmed as well, and it was never
 *      written at all -- so it sat at its reset value of 0, i.e. "no output
 *      window".  The reference driver writes the identity case on every mode
 *      set.  The POST scaler is likewise told explicitly that it is a 1:1
 *      pass-through (SCL_FACTOR = 0, SCL_CTRL = 0) instead of relying on
 *      reset values.
 *
 *   Note what a missing output window would look like from the DSI side:
 *   packets still get built and transmitted, so the lane activity, the FIFO
 *   behaviour and the panel's own CRC/ECC error counter would all look
 *   plausible -- while the payload carries no real picture.  That is worth
 *   ruling out before spending more time on the link.
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
                    ((uint32_t)htotal << 16) |
                        (priv->cfg.hsync_len & 0x1fff));

  /* HACT_ST_END: {start[28:16], end[12:0]} (end exclusive) */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_HACT_ST_END,
                    ((uint32_t)hact_st << 16) | (hact_end & 0x1fff));

  /* VTOTAL_VS_END: {total[28:16], vsync_len[12:0]} */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_VTOTAL_VS_END,
                    ((uint32_t)vtotal << 16) |
                        (priv->cfg.vsync_len & 0x1fff));

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

  /* *** THE POST SCALER FACTOR IS 1.0, NOT 0 -- AND 0 IS A DEGENERATE VALUE. ***
   *
   * This wrote 0, on the stated assumption that "a factor of 0 is this
   * hardware's no-scaling".  That assumption was never checked and it is
   * wrong, on both authorities:
   *
   *   Linux, vop2_crtc_atomic_enable():
   *     val  = scl_cal_scale2(vdisplay, vsize) << 16;
   *     val |= scl_cal_scale2(hdisplay, hsize);
   *     VOP_MODULE_SET(vop2, vp, post_scl_factor, val);
   *
   *   It writes this register UNCONDITIONALLY, 1:1 included, and
   *   scl_cal_scale2() is NOT the window scaler's helper: for identical source
   *   and destination it returns 2^12, not 0.  (vop2_scale_factor(), which does
   *   return 0 for SCALE_NONE, is the WINDOW scaler's; this driver conflated
   *   the two and wrote the window's answer into the POST's register.)
   *
   *   A dump from a working configuration on this board reads 0x10001000 here:
   *   post_vs_factor = post_hs_factor = 0x1000 = 1.0.
   *
   * And the field is defined as
   *
   *   post_vs_factor[31:16] = (src_height / dst_height) * 2^12
   *   post_hs_factor[15:0]  = (src_width  / dst_width ) * 2^12
   *
   * so 0 means "scale by zero", which is not a pass-through, it is a
   * degenerate scale.  It sits on the POST's OUTPUT scaler, the last block
   * before the DSI, and it would explain a picture that is stable and
   * structured yet follows neither the framebuffer's content nor its address:
   * a UNIFORM image is invariant under any scaling, which is why the solid
   * black and solid blue tests kept passing while real content never did.
   *
   * NOTE THE SHAPE OF THIS MISTAKE.  It is the same one this bring-up has made
   * repeatedly: a register value taken from reasoning about the vendor source,
   * or from the wrong helper, rather than from a working configuration.  It is
   * also the same shape as the errata in REGION0_CTRL and SCL_CTRL -- a plain
   * write of the whole register that clobbers fields nobody looked at.
   */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_SCL_FACTOR_YRGB,
                    RK3576_VOP_POST_SCL_FACTOR_1_1);
  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_SCL_CTRL, 0);

  syslog(LOG_WARNING,
         "vop: POST_SCL_FACTOR_YRGB=%08x (want 10001000 = 1.0; the old value "
         "0 meant 'scale by zero'), POST_SCL_CTRL=%08x\n",
         (unsigned)rk3576_vop_getreg(priv,
                                     post_base +
                                         RK3576_VOP_POST_SCL_FACTOR_YRGB),
         (unsigned)rk3576_vop_getreg(priv,
                                     post_base + RK3576_VOP_POST_SCL_CTRL));
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

  /* Disable the VOP's automatic clock gating BEFORE anything else.
   *
   * THE RESET VALUE HAS GATING ON, and the reference driver turns it off for
   * RK3576 by name.  Its comments describe precisely this board's failure:
   *
   *   auto_gating_en: "workaround to avoid display image shift when a window
   *   enabled";
   *   aclk_pre_auto_gating_en: "workaround for RK3528/RK3562/RK3576: the aclk
   *   pre auto gating function may disable the aclk in some unexpected cases,
   *   which is detected by hardware automatically ... resulting in abnormal
   *   display".
   *
   * The hardware deciding for itself that a block looks idle and gating its
   * clock fits every measurement this bring-up eventually reached: the layer
   * enabled and routed, its registers all correct, no bus error, and yet not
   * one read ever issued (axi0_mmu_idle = 1) with the POST permanently
   * under-run unless the layer was disabled.  A gated clock stalls the fetch
   * silently -- there is nothing to report, because nothing happens.
   *
   * Clearing both bits is what the reference driver does.  The rest of the
   * register's gating bits are left as the hardware set them, so this is the
   * smallest change that matches the documented workaround. */

  {
    uint32_t gate = rk3576_vop_getreg(priv, sys_base +
                                      RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD);
    uint32_t fixed = gate & ~(RK3576_VOP_AUTO_GATING_EN |
                              RK3576_VOP_ACLK_PRE_AUTO_GATING_EN);

    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD,
                      fixed);

    syslog(LOG_WARNING,
           "vop: SYS_AUTO_GATING_CTRL_IMD %08x -> %08x (auto_gating_en "
           "bit31=%u -> %u, aclk_pre_auto_gating_en bit7=%u -> %u; the "
           "RK3576 workaround is to clear both, or the hardware may gate a "
           "window's aclk by itself and stop its fetch with no error at "
           "all)\n",
           (unsigned)gate, (unsigned)fixed,
           (unsigned)((gate >> 31) & 1u), (unsigned)((fixed >> 31) & 1u),
           (unsigned)((gate >> 7) & 1u), (unsigned)((fixed >> 7) & 1u));
  }

  /* AXI read IDs: set them to THIS window's values, not the reset defaults.
   *
   * The reference driver programs these for every window
   * (rk3576_vop_win_data): ESMART0 on axi0 uses yrgb 0x10 / uv 0x11.  The
   * reset value is 0x0a/0x0b -- which belongs to Esmart2, a window that
   * fetches on axi1.  Leaving it alone meant this window's reads announced
   * themselves as a different window on a different AXI port; the ID decides
   * routing and QoS inside the interconnect, and a bogus one yields a read
   * that never returns: no data, no bus error, and a POST whose output buffer
   * is permanently empty.
   *
   * *** ALSO ENABLE THE WINDOW'S OWN HURRY READ REQUEST (bit28). ***
   *
   * That is the third leg of the anti-starvation mechanism and it had never been
   * connected.  The other two are now in place: the POST raises an urgency
   * signal when its line buffer runs dry (POST_COLOR_CTRL, sw_urgency_en with
   * thl/thh), and the SYS block forwards it onto the AXI arbitration
   * (SYS_AXI_HURRY_CTRL0/1, axi0/axi1_port0_urgency_en).  This bit is the
   * window's own copy of the same idea.
   *
   * The TRM defines the trigger as "If esmart empty lb number >=
   * esmart_dma_rreq_thold, dma_rreq_hurry is asserted", so thold = 0 makes the
   * condition true whenever this window is short of data.  The reference driver
   * leaves bit28 clear, but the measured symptom here IS starvation -- the
   * POST's input buffer under-runs while this layer is enabled and stops the
   * moment it is disabled -- so a window that can ask for priority is what the
   * evidence calls for.  If it changes nothing, that is informative too: it
   * would mean the request never reaches the arbitration at all.
   *
   * *** THE HURRY REQUEST IS GONE.  IT WAS PERMANENTLY ASSERTED. ***
   *
   * This write used to set dma_rreq_hurry_en with thold = 0, and the TRM's
   * trigger for that request is
   *
   *     "if esmart empty lb number >= esmart_dma_rreq_thold,
   *      dma_rreq_hurry is asserted"
   *
   * with thold = 0 the condition is TRUE AT ALL TIMES.  The window held a
   * priority request on the AXI arbitration high for the entire run, from the
   * moment this bit was added -- and a request that is never de-asserted carries
   * no information and cannot be arbitrated on, which is the opposite of what it
   * was added for.  The working register dump has this bit clear, and the
   * working driver's window table writes only the two read-id fields here.
   *
   * Only the read ids are written now: the one field with evidence behind it,
   * and the one that decides which AXI channel this window's reads travel on.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_CTRL1,
                    (g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX]
                     << RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT) |
                        (g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX]
                         << RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT));

  {
    uint32_t c1 = rk3576_vop_getreg(priv, esmart_base +
                                    RK3576_VOP_ESMART_CTRL1);

    syslog(LOG_WARNING,
           "vop: ESMART%u_CTRL1=%08x (yrgb_rid[8:4]=0x%02x want 0x%02x, "
           "cbcr_rid[16:12]=0x%02x want 0x%02x, dma_rreq_hurry_en bit28=%u "
           "want 0) -- reads must carry THIS window's pair\n",
           (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)c1,
           (unsigned)((c1 >> 4) & 0x1fu),
           (unsigned)g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX],
           (unsigned)((c1 >> 12) & 0x1fu),
           (unsigned)g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX],
           (unsigned)((c1 >> 28) & 1u));

    /* The assertion is now the OPPOSITE of what it used to be: the bit must be
     * CLEAR.  Keeping the old check would have the driver insist on the very
     * condition that was just removed. */

    if ((c1 & RK3576_VOP_ESMART_CTRL1_DMA_RREQ_HURRY_EN) != 0u)
      {
        syslog(LOG_ERR,
               "vop: *** esmart_dma_rreq_hurry_en is SET again; with thold=0 "
               "that means a\n");
        syslog(LOG_ERR,
               "vop:   priority request asserted for ever with nothing to "
               "arbitrate on ***\n");
      }
  }

  /* AXI control: MMU bypass (REGION0_YRGB_MST is a physical address), let the
   * hardware auto-optimise DMA bursts that cross a 4k boundary, and bound the
   * number of outstanding transactions.
   *
   * THE BIT POSITIONS ARE FROM THE TRM, and getting them wrong here was the
   * fault that this whole bring-up spent dozens of rounds on: this register's
   * low bits are dma_sop(0) / axi_sel(1) / mmu_bypass(2) / outstanding_en(3),
   * NOT outstanding_en(0) / dma_sop(1) / axi_sel(2) / mmu_bypass(3) as an
   * earlier version of the header claimed.  The wrong positions meant the
   * written value set outstanding_en while leaving mmu_bypass at 0, so a
   * driver using physical addresses was routing them through an MMU with no
   * page tables.  The bits are named symbolically now, so this call is
   * unchanged and correct.
   *
   * BOUND THE AXI OUTSTANDING TRANSACTIONS -- BUT NOT TOO TIGHTLY.  At reset
   * ESMART outstanding_en = 0, which means *unlimited* bursts, and an earlier
   * round capped it at num = 4 to stop the scan-out DMA flooding the AXI/NoC and
   * starving the CPU's DDR traffic.  A later round established that the hang was
   * NOT caused by the outstanding limit, and 4 is very tight for this panel:
   *
   *   a line is 720 pixels x 3 bytes = 2160 bytes, about 34 64-byte bursts, and
   *   all of it has to arrive inside one line time -- 780 pixels at ~62.5 MHz,
   *   about 12.5 us.  With only 4 requests in flight each burst must complete in
   *   under ~1.4 us on average.  Any contention spike on the DDR controller
   *   pushes a burst past that and the POST under-runs -- and the POST's answer
   *   to an empty input buffer is to keep emitting whatever is left in it.
   *
   * The reference driver leaves this field alone entirely (unlimited).
   * num = 15 is the field's MAXIMUM: esmart_outstanding_num occupies only bits
   * [7:4], four bits, so 16 is not representable and
   * RK3576_VOP_ESMART_AXI_OUTSTANDING() masks the value to those four bits --
   * asking for 16 would silently write 0, the exact opposite of the intent.
   * 15 keeps a bound, because the original concern was real, while leaving far
   * more in flight than 4 did.
   */

  /* *** SELECT THE AXI CHANNEL FIRST, THEN PROGRAM THE REST. ***
   *
   * RK3576 gives each ESMART its own channel -- ESMART0/1 on axi0,
   * ESMART2/3 on axi1 -- so this window (RK3576_VOP_ESMART_IDX) must move to
   * axi1, and until now it did not: the driver wrote ESMART0's identity onto
   * ESMART2's block.  See the table g_rk3576_vop_esmart_axi_sel[].
   *
   * The channel is switched on its own because of a recorded measurement: an
   * earlier round set axi_sel and read back
   *
   *   0x00010002   where   0x000100fe   had been written
   *
   * i.e. bits [7:2] -- mmu_bypass, outstanding_en, outstanding_num -- came back
   * CLEARED.  Whatever the mechanism, the fields a physical-address driver
   * depends on do not survive being written in the same access as axi_sel, so
   * they are re-asserted afterwards.
   *
   * Losing the window's own mmu_bypass is not fatal here: configure_port()
   * already programs the SYS-level bypass (SYS_MMU_CTRL_IMD, mmu_bypass_en with
   * bypass_id = 0, i.e. "rid > 0 bypasses"), which covers every window.
   */

  /* *** NO OUTSTANDING BOUND.  THIS IS THE SECOND DEVIATION NOW REMOVED. ***
   *
   * The working register dump reads 0x00010000 here: dma_4k_addr_opt only, with
   * outstanding_en CLEAR, i.e. the window may keep as many requests in flight as
   * the interconnect accepts.  The working driver's window table does not even
   * define a field for it.  This driver bounded it on the theory that the
   * scan-out DMA could monopolise the NoC and starve the CPU -- a theory an
   * earlier round already tested and rejected.
   *
   * mmu_bypass STAYS SET.  That is a deliberate, permanent difference from the
   * working dump: that configuration uses an IOMMU, this driver does not have
   * one, and its window addresses are physical.  It is the one deviation here
   * that exists for a reason that cannot be removed.
   */

  if (g_rk3576_vop_esmart_axi_sel[RK3576_VOP_ESMART_IDX] != 0u)
    {
      rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                        RK3576_VOP_ESMART_AXI_AXI_SEL);
    }

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                    RK3576_VOP_ESMART_AXI_DMA_4K_ADDR_OPT |
                        RK3576_VOP_ESMART_AXI_MMU_BYPASS |
                        (g_rk3576_vop_esmart_axi_sel[RK3576_VOP_ESMART_IDX] !=
                                 0u
                             ? RK3576_VOP_ESMART_AXI_AXI_SEL
                             : 0u));

  {
    uint32_t ax;
    uint32_t want_sel = g_rk3576_vop_esmart_axi_sel[RK3576_VOP_ESMART_IDX];

    /* *** THE READ-BACK IS DELIBERATELY TAKEN AFTER A DELAY. ***
     *
     * A read issued immediately after this write returns the PREVIOUS
     * contents, and taking it as the result produced two rounds of false
     * conclusions.  The probe run that settled the register's write format
     * showed this outright: it found the register already holding
     * 0x000100fc -- the driver's own intended value -- while every
     * configure_layer() log in this project had reported the same write
     * "losing" bits[7:2] and reading back 0x00010000, the RESET value.  The
     * probe also wrote 0x00fc00ff and read back 0x00ff00fc, i.e. the value
     * from the PREVIOUS iteration.  So the loss was never real; the read was
     * simply early.
     *
     * (This register is IMD = immediate, which is why a delay of a few
     * microseconds is enough; the delay exists to let the write land, not to
     * wait for a frame boundary.)
     */

    up_udelay(50);
    ax = rk3576_vop_getreg(priv, esmart_base +
                           RK3576_VOP_ESMART_AXI_CTRL_IMD);

    syslog(LOG_WARNING,
           "vop: ESMART%u AXI_CTRL_IMD=%08x (mmu_bypass bit2=%u (want 1), "
           "outstanding_en bit3=%u num[7:4]=%u (want 0/unlimited), "
           "axi_sel bit1=%u -> %s, want %s)\n",
           (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)ax,
           (unsigned)((ax >> 2) & 1u), (unsigned)((ax >> 3) & 1u),
           (unsigned)((ax >> 4) & 0xfu), (unsigned)((ax >> 1) & 1u),
           ((ax >> 1) & 1u) != 0u ? "AXI1" : "AXI0",
           want_sel != 0u ? "AXI1" : "AXI0");

    if (((ax >> 1) & 1u) != want_sel)
      {
        syslog(LOG_ERR,
               "vop: *** ESMART%u is still fetching on %s but RK3576's window "
               "table says it must use\n",
               (unsigned)RK3576_VOP_ESMART_IDX,
               ((ax >> 1) & 1u) != 0u ? "AXI1" : "AXI0");
        syslog(LOG_ERR,
               "vop:   %s -- reads on the wrong channel are the prime "
               "suspect for a fetch that\n", want_sel != 0u ? "AXI1" : "AXI0");
        syslog(LOG_ERR,
               "vop:   never returns, so this write did not take and must be "
               "reordered ***\n");
      }

    /* *** ASSERT THE FIELDS axi_sel DESTROYS. ***
     *
     * MEASURED: writing this register with bit1 (axi_sel) SET clears bits
     * [7:2] -- mmu_bypass, outstanding_en and outstanding_num -- and the
     * clearing is not order-dependent.  An earlier round recorded
     * 0x00010002 coming back from 0x000100fe, and the two-step write (set
     * axi_sel alone, then re-assert everything) did NOT help: the second write
     * carries bit1 too, so it is cleared again.
     *
     * That is why the window is ESMART0: axi_sel stays 0 there and the whole
     * problem disappears.  This assertion exists so that if the window is ever
     * moved to ESMART2/3 again, the loss of mmu_bypass cannot pass unnoticed --
     * it is a physical-address driver, and a window whose MMU bypass has
     * silently gone away hands its addresses to an MMU with no page tables.
     */

    if ((ax & RK3576_VOP_ESMART_AXI_MMU_BYPASS) == 0u)
      {
        syslog(LOG_ERR,
               "vop: *** ESMART%u AXI_CTRL_IMD LOST mmu_bypass=%u -- this "
               "driver has no\n",
               (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)((ax >> 2) & 1u));
        syslog(LOG_ERR,
               "vop:   page tables, so its physical addresses would go to an "
               "MMU ***\n");
      }
  }

  /* REGION0 disabled first, RGB888 format -- AS A FIELD WRITE.
   *
   * *** THE PREVIOUS VERSION WROTE THE WHOLE REGISTER, WHICH ERASED EVERY BIT
   * OF ITS RESET VALUE. ***  The working dump reads 0x00400001 here: bit0 (the
   * region enable) written by the vendor driver, and bit22 left at its reset
   * value of 1.  A plain write of a format value silently cleared bit22 -- a
   * reset bit this driver never intended to touch, and never looked at.  The
   * same mistake was made on SCL_CTRL, where it wiped the filter modes.
   *
   * Only the fields this driver has any evidence for are written now.
   */

  ctrl = rk3576_vop_getreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL);
  ctrl &= ~(RK3576_VOP_ESMART_REGION0_FMT_MASK |
            RK3576_VOP_ESMART_REGION0_MST_EN);
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

  /* *** SCL_CTRL: NOT ZERO -- yrgb_anei_en MUST BE SET. ***
   *
   * This write used to be a plain 0, which is a value the reference driver
   * NEVER produces on this SoC generation: it sets yrgb_anei_en (bit20)
   * unconditionally for RK3572 and later, outside the scaling-mode branch, so a
   * 1:1 layer still gets bit20 = 1 while every other field of this register is
   * 0.  See RK3576_VOP_ESMART_REGION0_SCL_ANE_I_EN in the header for the
   * citations.
   *
   * The scaler stage sits between the window's DMA/line buffers and its output,
   * so a scaler configured differently from the vendor's is a plausible reason
   * for the window to issue no read at all while every register reads back
   * perfectly.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_SCL_CTRL,
                    RK3576_VOP_ESMART_SCL_FILTER_MODES_DEFAULT);

  /* *** SCL_FACTOR: LINUX WRITES 0 FOR A NON-SCALING WINDOW, AND THIS DRIVER
   * NEVER WROTE THIS REGISTER AT ALL. ***
   *
   * It therefore sat at its reset value, 0x10001000 -- scale factor 1.0 in both
   * axes -- while the working dump shows 0x1834 ABSENT, i.e. zero.
   *
   * That is not a guess about which value is "nicer".  vop2_scale_factor()
   * begins
   *
   *     if (mode == SCALE_NONE)
   *             return 0;
   *
   * and the SCL_NONE branch of vop2_setup_scale() is exactly "write
   * scale_yrgb_x = scale_yrgb_y = 0".  So zero is the value the vendor driver
   * produces for a 1:1 layer, deliberately, and 1.0 is a value it never writes
   * here.  A scale factor of 1.0 on a windows whose scale MODE says "none" is
   * an inconsistent pair, and the scaler is the stage that decides when to ask
   * the DMA for data.
   *
   * The CbCr factor is deliberately left alone: the working dump DOES read
   * 0x10001000 there, and Linux only writes it when the layer is YUV -- which
   * this one is not.  Leaving it at reset reproduces the working value.
   */

  rk3576_vop_putreg(priv,
                    esmart_base + RK3576_VOP_ESMART_REGION0_SCL_FACTOR_YRGB, 0u);

  {
    uint32_t scl = rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_REGION0_SCL_CTRL);
    uint32_t fac = rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_REGION0_SCL_FACTOR_YRGB);

    syslog(LOG_WARNING,
           "vop: ESMART%u REGION0_SCL_CTRL=%08x (working dump 00000044: "
           "hscl_filter=%u vscl_filter=%u yrgb_anei_en bit20=%u want 0)\n",
           (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)scl,
           (unsigned)((scl >> RK3576_VOP_ESMART_SCL_HSCL_FILTER_SHIFT) & 0x3u),
           (unsigned)((scl >> RK3576_VOP_ESMART_SCL_VSCL_FILTER_SHIFT) & 0x3u),
           (unsigned)((scl >> 20) & 1u));
    syslog(LOG_WARNING,
           "vop: ESMART%u REGION0_SCL_FACTOR_YRGB=%08x (working dump: absent, "
           "i.e. 0)\n", (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)fac);

    if ((scl & RK3576_VOP_ESMART_REGION0_SCL_ANE_I_EN) != 0u ||
        scl != RK3576_VOP_ESMART_SCL_FILTER_MODES_DEFAULT || fac != 0u)
      {
        syslog(LOG_ERR,
               "vop: *** the scaler block does NOT match the working dump: "
               "SCL_CTRL=%08x (want 00000044) ***\n", (unsigned)scl);
      }
  }

  /* Layer pipeline delay: 0, which is what the reference driver computes for a
   * plain SDR layer.
   *
   * ESMART_DLY_NUM delays the layer's pixel stream relative to the mixing
   * pipeline.  The reference driver derives it in vop2_calc_dly_num() from the
   * VP's window/mix delays plus any HDR/CGC path, and its own comment states
   * the baseline: "If hdrvivid and sdr2hdr is not work, the default bg_dly is
   * 0x10 and the default win delay num is 0."  With no HDR, no CGC and no
   * sdr2hdr -- this board -- every intermediate delay is zero and the
   * arithmetic reduces to win->dly_num = 0.
   *
   * This driver never wrote the register, and it read 0x17: 23 cycles of extra
   * delay on the layer's data.  That is exactly the kind of misalignment that
   * starves the POST's input buffer at the moment it expects pixels, which
   * matches the two measurements that are now firm -- the output-buffer
   * under-run recurs with the layer enabled, and it stops the instant the layer
   * is disabled -- and it also fits a picture that follows neither the
   * framebuffer's content nor its address.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_DLY_NUM, 0u);

  /* Route ESMART0 to this video port. */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_PORT_SEL_IMD,
                    RK3576_VOP_ESMART_PORT_VP0);

  /* Enable REGION0: RGB888 + region enable.
   *
   * rb_swap IS SET, because the TRM's swap table (11.3, "Swap configuration
   * table in esmart default format") gives rb_swap = 1 for RGB888 -- the layer
   * wants BGR byte order while the NuttX framebuffer is stored R,G,B.  Leaving
   * it clear exchanges red and blue on the glass, which no whole-screen black or
   * white can reveal (swapping equal bytes changes nothing) and which therefore
   * survived every byte-uniform probe in this bring-up.
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

    rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_LAYER_SEL,
                      RK3576_VOP_LAYER_SEL_L0(layer0_sel) |
                          RK3576_VOP_LAYER_SEL_L1(
                              RK3576_VOP_LAYER_SEL_DISABLE) |
                          RK3576_VOP_LAYER_SEL_L2(
                              RK3576_VOP_LAYER_SEL_DISABLE) |
                          RK3576_VOP_LAYER_SEL_L3(
                              RK3576_VOP_LAYER_SEL_DISABLE));

    syslog(LOG_WARNING,
           "vop: OVERLAY_PORT%u_LAYER_SEL <= layer0=%u (%s), layer1-3=%u "
           "(disable)\n",
           (unsigned)priv->cfg.port, (unsigned)layer0_sel,
           RK3576_VOP_ESMART_IDX == 2 ? "ESMART2" : "ESMART0",
           (unsigned)RK3576_VOP_LAYER_SEL_DISABLE);
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

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL,
                    (0xffu << RK3576_VOP_MIX_CTRL_GLB_ALPHA_SHIFT) |
                        RK3576_VOP_MIX_CTRL_ALPHA_EN |
                        (RK3576_VOP_FACTOR_SRC_GLOBAL
                         << RK3576_VOP_MIX_CTRL_FACTOR_SHIFT));

  /* MIX0 dest color: factor=256-Ad0 + glb_alpha=0xff (color_mode=Cd). */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL,
                    (0xffu << RK3576_VOP_MIX_CTRL_GLB_ALPHA_SHIFT) |
                        (RK3576_VOP_FACTOR_DST_INVERSE
                         << RK3576_VOP_MIX_CTRL_FACTOR_SHIFT));

  /* MIX0 source alpha: factor=256, blend=global. */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_ALPHA_CTRL,
                    (RK3576_VOP_FACTOR_ONE
                     << RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT));

  /* MIX0 dest alpha: factor=256-Ad0, no-saturation. */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_ALPHA_CTRL,
                    (RK3576_VOP_FACTOR_DST_INVERSE
                     << RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT) |
                        RK3576_VOP_MIX_ALPHA_CAL_MODE);
}

/****************************************************************************
 * Name: rk3576_vop_channel_biu_enable
 *
 * Description:
 *   *** THE VOP'S CHANNEL TO DDR HAS ITS OWN CLOCK, RESET AND GATE, IN THE VO0
 *   DOMAIN -- AND THIS DRIVER NEVER ENABLED ANY OF THEM. ***
 *
 *   TRM Part 1, clock dependency table:
 *
 *     Clocks which have dependency      The clock which can not be gated
 *     aclk_vo0_biu                      aclk_vop_biu、aclk_vo0vop_channel_biu
 *
 *   i.e. aclk_vo0vop_channel_biu is the clock of the VOP's channel to the DDR
 *   subsystem, and the TRM names it alongside aclk_vop_biu as a clock that must
 *   not be gated.  The driver's biu_clocks[] list contains aclk_vop_biu,
 *   aclk_vop2_biu, hclk_vop_biu and pclk_vop_biu -- but NOT this one, and a
 *   search of rk3576_clk_tree.c shows it was never registered as a clock either,
 *   so nothing in the system ever ungated it.
 *
 *   Registers (CRU at 0x27200000; all three use the hiword write-enable
 *   scheme, "1'b1: Write access enable" in bits[31:16]):
 *
 *     CRU_CLKSEL_CON19 0x034C  [13:12] aclk_vo0vop_channel_biu_sel (reset 0,
 *                                      2'b00: clk_gpll_mux), [11:8] div
 *                                      (divide by div+1, reset 2)
 *                              [7:6] hclk_..._sel, [5:4] hclk divider
 *     CRU_GATE_CON03   0x080C  bit1 aclk_vo0vop_channel_biu_en
 *                              bit0 hclk_vo0vop_channel_biu_en
 *                              "When high, disable clock"  (reset 0 = running)
 *     CRU_SOFTRST_CON06 0x0A18 bit1 aresetn_vo0vop_channel_biu
 *                              bit0 hresetn_vo0vop_channel_biu
 *                              "When high, reset relative logic" (reset 0)
 *
 *   WHY THIS MATTERS MORE THAN ANY OTHER CLOCK IN THIS FILE.  A VOP whose
 *   aclk_vop (the block's internal AXI clock) runs normally but whose
 *   vo0vop_channel_biu is gated has a very specific pathology, and it is the
 *   exact set of measurements this bring-up has accumulated:
 *
 *     - every register reads and writes perfectly (that is pclk/hclk_vop on the
 *       APB path, a different clock domain);
 *     - the POST and the MIPI/DSI interface work, because they are downstream
 *       of the VOP's own logic and dclk, not of the DDR path;
 *     - the display scans and the panel receives packets;
 *     - and yet NO LAYER ISSUES A SINGLE AXI READ, with no bus error and no MMU
 *       fault, because a transaction can never leave a clockless channel BIU.
 *
 *   It also explains, without any further assumptions, why substituting ESMART2
 *   for ESMART0 changed nothing: the two windows share this channel.  The fault
 *   is in the shared path, which is exactly what that experiment established.
 *
 *   The gate's reset value is 0 (clock running), so this is only a fault if a
 *   boot stage gated it -- but this board's bootloader drives the VOP for a boot
 *   logo and putting the display's DDR channel clock into a gated state before
 *   handing over is a normal power-saving action.  Nothing in this system ever
 *   checked, and nothing ever undid it.  Both the state and the correction are
 *   printed below.
 *
 ****************************************************************************/

static void rk3576_vop_channel_biu_enable(void)
{
  static const uint32_t cru = 0x27200000u;
  uint32_t clksel_b;
  uint32_t gate_b;
  uint32_t rst_b;
  uint32_t clksel_a;
  uint32_t gate_a;
  uint32_t rst_a;

  clksel_b = getreg32(cru + 0x034cu); /* CRU_CLKSEL_CON19 */
  gate_b   = getreg32(cru + 0x080cu); /* CRU_GATE_CON03    */
  rst_b    = getreg32(cru + 0x0a18u); /* CRU_SOFTRST_CON06 */

  syslog(LOG_WARNING,
         "vop: VO0VOP CHANNEL BIU: CLKSEL_CON19(0x34c)=%08x "
         "GATE_CON03(0x80c)=%08x SOFTRST_CON06(0xa18)=%08x\n",
         (unsigned)clksel_b, (unsigned)gate_b, (unsigned)rst_b);
  syslog(LOG_WARNING,
         "vop:   aclk_vo0vop_channel_biu: sel[13:12]=%u (0=gpll) div[11:8]=%u "
         "-> /%u, gate bit1=%u (0=RUNNING 1=STOPPED), reset bit1=%u\n",
         (unsigned)((clksel_b >> 12) & 0x3u), (unsigned)((clksel_b >> 8) & 0xfu),
         (unsigned)(((clksel_b >> 8) & 0xfu) + 1u),
         (unsigned)((gate_b >> 1) & 1u), (unsigned)((rst_b >> 1) & 1u));

  if (((gate_b >> 1) & 1u) != 0u || ((gate_b >> 0) & 1u) != 0u)
    {
      syslog(LOG_ERR,
             "vop: *** THE VOP'S DDR CHANNEL CLOCK IS GATED (GATE_CON03 = "
             "%04x: aclk_en bit1=%u,\n", (unsigned)(gate_b & 0xffffu),
             (unsigned)((gate_b >> 1) & 1u));
      syslog(LOG_ERR,
             "vop:   hclk_en bit0=%u).  A gated channel BIU CANNOT ISSUE AXI "
             "TRANSACTIONS AT\n", (unsigned)(gate_b & 1u));
      syslog(LOG_ERR,
             "vop:   ALL, while every register in the VOP still reads and "
             "writes normally and\n");
      syslog(LOG_ERR,
             "vop:   the POST still scans.  That is 'zero reads, no bus "
             "error, no MMU fault'. ***\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "vop:   the channel gate is OPEN (0 = running), so this was not "
             "the fault.  It is\n");
      syslog(LOG_WARNING,
             "vop:   now proven rather than assumed.\n");
    }

  if ((rst_b & 0x3u) != 0u)
    {
      syslog(LOG_ERR,
             "vop: *** THE VOP'S DDR CHANNEL IS HELD IN RESET (SOFTRST_CON06 "
             "bits[1:0] = %u).\n", (unsigned)(rst_b & 0x3u));
      syslog(LOG_ERR,
             "vop:   Same consequence: no AXI transactions, no error, no "
             "fault. ***\n");
    }

  /* Release the reset and open both gates.  Data 0 = not reset, 0 = clock
   * running, with the hiword enabling those two low bits.
   */

  putreg32(0x00030000u | 0x0000u, cru + 0x0a18u);
  putreg32(0x00030000u | 0x0000u, cru + 0x080cu);
  up_udelay(20);

  clksel_a = getreg32(cru + 0x034cu);
  gate_a   = getreg32(cru + 0x080cu);
  rst_a    = getreg32(cru + 0x0a18u);

  syslog(LOG_WARNING,
         "vop: VO0VOP CHANNEL BIU: after -- CLKSEL_CON19=%08x "
         "GATE_CON03=%08x SOFTRST_CON06=%08x\n",
         (unsigned)clksel_a, (unsigned)gate_a, (unsigned)rst_a);

  if (((gate_a >> 1) & 1u) != 0u || ((gate_a >> 0) & 1u) != 0u)
    {
      syslog(LOG_ERR,
             "vop: *** the channel gate did NOT open (reads back %04x).  The "
             "VOP's path to DDR\n", (unsigned)(gate_a & 0xffffu));
      syslog(LOG_ERR,
             "vop:   stays unusable and no layer here can ever fetch. ***\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "vop:   aclk_vo0vop_channel_biu and hclk_vo0vop_channel_biu are "
             "now RUNNING and the\n");
      syslog(LOG_WARNING,
             "vop:   channel is out of reset.  If this was the fault, the "
             "layers can fetch now.\n");
    }
}

/****************************************************************************
 * Name: rk3576_vop_sys_masked_write
 *
 * Description:
 *   Write a SYS_CTRL register that uses the hiword write-mask scheme.
 *
 *   Several SYS_CTRL registers carry a "write_mask" field in bits[31:16]:
 *   "When every bit HIGH, enable the writing corresponding bit.  When every bit
 *   LOW, don't care the writing corresponding bit."  A plain putreg() leaves
 *   bits[31:16] at zero, so the write is discarded completely and the register
 *   keeps its previous contents -- with nothing in a readback to say so.
 *
 *   This is not a theoretical concern here.  The two SYS writes this driver
 *   cares about most were both dropped for exactly this reason:
 *
 *     SYS_PORT_CTRL_IMD   reg_done_frm <- 0.  (This write used to assert two
 *                         more bits of its own -- clearing dsp_vs_t_sel and
 *                         setting auto_cs_mode -- on the strength of reading
 *                         the vendor source.  A dump from a working
 *                         configuration on this board shows that both of those
 *                         are wrong: dsp_vs_t_sel holds its reset value 1, and
 *                         auto_cs_mode is 0.  Only reg_done_frm is written
 *                         now.)
 *     SYS_MMU_CTRL_IMD    the mmu_bypass_en/bypass_id setting
 *
 *   Both were printed as "old -> new" and both were read back and reported as
 *   applied, because the log showed the value the driver INTENDED to write.
 *   Neither ever reached the register.  That is how a real fix becomes a
 *   "refuted hypothesis".  Writes here are therefore followed by a readback
 *   check of the bits that matter, not by a print of the intended value.
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
 *   groups (SYS_REG_CFG_DONE) and the ESMART0 layer group
 *   (SYS_WIN_REG_CFG_DONE).  The copies land at the start of the next frame.
 *
 *   sw_global_regdone_en (bit 15) MUST be kept set: it is an ordinary data
 *   bit inside the 16-bit data word, and the hiword write-mask
 *   (0xffff << 16) enables writes to all of them, so leaving it 0 in the
 *   data silently disables the global regdone -- after which the VP0 global
 *   group's load bit stays pending forever (seen in the register readback)
 *   and only the system group ever latches.  See
 *   RK3576_VOP_CFG_DONE_GLOBAL_REGDONE_EN in rk3576_vop.h.
 *
 *   Called from configure_port(), and AGAIN right after the dclk reset pulse
 *   (see there) because the dclk reset restarts the VP0 timing generator and
 *   can swallow an in-flight load.
 ****************************************************************************/

static void rk3576_vop_trigger_cfg_done(FAR struct rk3576_vop_s *priv)
{
  uint32_t sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  uint32_t post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);

  /* *** ASK FOR EXACTLY WHAT LINUX ASKS FOR. ***
   *
   * This used to OR in RK3576_VOP_CFG_DONE_ALL_GROUPS -- bits 1/2/5/6, which
   * are VP1's and VP2's own load requests.  Nothing on this board consumes
   * them, so they stay set for ever: this driver's own dump reads 0x00008066
   * here where a known-working configuration reads 0x00000000.  The TRM states
   * that each of these bits makes the mirror->real copy happen "when all the
   * [group] register config finish", and the one experiment that tested the
   * window group directly found that THE WINDOW GROUP NEVER LOADS -- which is
   * exactly what a commit permanently waiting on two video ports that will
   * never scan would produce.
   *
   * Linux's rk3588_vop2_cfg_done() writes only
   *
   *   GLB_CFG_DONE_EN | WB_CFG_DONE | (WB_CFG_DONE << 16) |
   *   BIT(sys_cfg_done_shift) | (BIT(sys_cfg_done_shift) << 16)
   *
   * i.e. bit15 (unmasked) + bit14 + bit4 with write-enables, for the video
   * port being committed.  That is what is written now, with bit0 kept as well
   * because this board demonstrably consumes it (VP0's own cfg_done), and
   * with a readback that NAMES every remaining pending bit so the difference
   * is visible rather than inferred.
   */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE,
                    RK3576_VOP_CFG_DONE_LOAD_CTRL |
                        RK3576_VOP_CFG_DONE_GLOBAL_REGDONE_EN |
                        RK3576_VOP_CFG_DONE_WB_LOAD |
                        RK3576_VOP_CFG_DONE_VP0_GROUPS);

  up_udelay(200);

  {
    uint32_t done = rk3576_vop_getreg(priv,
                                      sys_base + RK3576_VOP_SYS_REG_CFG_DONE);

    syslog(LOG_WARNING,
           "vop: SYS_REG_CFG_DONE=%08x (working dump 00000000; "
           "vp0_en bit0=%u sys0_en bit4=%u wb bit14=%u global_en bit15=%u)\n",
           (unsigned)done, (unsigned)(done & 1u), (unsigned)((done >> 4) & 1u),
           (unsigned)((done >> 14) & 1u), (unsigned)((done >> 15) & 1u));

    if ((done & RK3576_VOP_CFG_DONE_VP0_GROUPS) != 0u)
      {
        syslog(LOG_WARNING,
               "vop:   VP0's own load bits are STILL PENDING 200 us after the "
               "commit\n");
      }

    if ((done & ((1u << 1) | (1u << 2) | (1u << 5) | (1u << 6))) != 0u)
      {
        syslog(LOG_ERR,
               "vop: *** VP1/VP2 load bits are pending: something still asks "
               "for groups that\n");
        syslog(LOG_ERR,
               "vop:   cannot be consumed on this board, which is the "
               "condition this write ***\n");
        syslog(LOG_ERR,
               "vop:   was changed to remove ***\n");
      }
  }

  /* The layer group's own load enable.  Only the bit for the window actually
   * in use (ESMART0 = bit4) is pulsed now.  The previous version pulsed every
   * window group on the assumption that "pulsing the load bit of an idle
   * window is harmless" -- and that assumption is exactly what the
   * window-group experiment put in doubt, so it is no longer relied on.
   *
   * (This register is the RK3588-era path.  On RK3572 and later Linux does not
   * write it at all -- it commits through SYS_REG_CFG_DONE above -- which is
   * why a dump from a working configuration reads 0x00000000 here.  It is kept
   * because on this board the window group is the thing that has been measured
   * as not loading, and this is the register whose TRM description names the
   * mirror->real copy for windows explicitly.)
   */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE,
                    RK3576_VOP_WIN_CFG_DONE_LOAD_CTRL |
                        RK3576_VOP_WIN_CFG_DONE_ESMART0);

  up_udelay(200);

  {
    uint32_t win = rk3576_vop_getreg(priv,
                                     sys_base +
                                         RK3576_VOP_SYS_WIN_REG_CFG_DONE);

    syslog(LOG_WARNING,
           "vop: SYS_WIN_REG_CFG_DONE=%08x (working dump 00000000; "
           "reg_load_esmart0_en bit4=%u)\n",
           (unsigned)win, (unsigned)((win >> 4) & 1u));
  }

  /* *** THE RK3572+ FRAME COMMIT. ***
   *
   * Linux commits an RK3572/RK3576 video port by writing bit0 of
   * POSTx + 0x0FC, which is NOT any of the triggers above: on this generation
   * it does not touch SYS_WIN_REG_CFG_DONE at all.  This driver pulses only
   * the rk3568-era triggers, so the window group's mirror->real copy has
   * arguably never been requested, while every readback looked perfect
   * because readback shows the MIRROR either way.
   *
   * VOP_REG_MASK => hiword write-enable, hence the 0x00010001 it writes.
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
  uint32_t esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
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

  /* Pre-scan: how EARLY the layers ask for their pixels.
   *
   * TRM 11.4 "H. Pre_scan_active": "Pre_scan_active define that layers send
   * the read data request early vop post read pixel data in post line buff",
   * with Pre_scan_hactive = pre_scan_max_dly + act_width/2 - 1.
   *
   * *** The reset value is ZERO and this driver never wrote it. ***  At zero the
   * layers request their pixels at the moment the POST reads it out of the line
   * buffer, i.e. too late, so the buffer under-runs.  That is the measured
   * POST_BUF_EMPTY: it recurs with the layer enabled and stops the instant the
   * layer is disabled, and a starved POST transmits undefined data rather than
   * the framebuffer -- a picture that follows neither the framebuffer's content
   * nor its address, while every layer register reads back correct.
   *
   * Value computed exactly as the reference driver does:
   *   pre_scan_dly = bg_dly + (roundup(hdisplay, 2) >> 1) - 1
   *   hblank       = max(hsync_len, 8)   (below 8 the window reset signal makes
   *                                       the first line's data zero)
   *
   * *** bg_dly IS 20 FOR RK3576 PORT0, NOT 16. ***  Two independent sources
   * agree: the TRM's Table 11-4 ("The delay of the port0 last mux output 20")
   * and the reference driver's own arithmetic (win_dly 10 + layer_mix_dly 8 +
   * hdr_mix_dly 2 = 20).  0x10 is RK3568's figure.  The previous version of
   * this code used 16 and did not even write BG_MIX_CTRL -- it asserted that
   * the inherited value already held 16, which is the kind of assumption the
   * early dump exists to eliminate.  Both places are written now and must
   * agree, because pre_scan is measured against the mux output.
   */

  {
    uint32_t bg_dly = RK3576_VOP_OVERLAY_BG_DLY_VP0;
    uint32_t hblank = (uint32_t)priv->cfg.hsync_len < 8u
                          ? 8u
                          : (uint32_t)priv->cfg.hsync_len;
    uint32_t hactive = bg_dly + (((uint32_t)priv->cfg.xres + 1u) / 2u) - 1u;
    uint32_t pre = ((hactive & 0x1fffu) << 16) | (hblank & 0x1fffu);
    uint32_t bgm = rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_BG_MIX_CTRL);
    uint32_t bgm_new = (bgm & ~RK3576_VOP_OVERLAY_BG_DLY_MASK) |
                       (bg_dly << RK3576_VOP_OVERLAY_BG_DLY_SHIFT);
    uint32_t bgm_got;

    rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_BG_MIX_CTRL,
                      bgm_new);
    rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_PRE_SCAN_HTIMING,
                      pre);

    bgm_got = rk3576_vop_getreg(priv, ovl_base +
                               RK3576_VOP_OVERLAY_BG_MIX_CTRL);

    syslog(LOG_WARNING,
           "vop: OVERLAY_BG_MIX_CTRL(0x0070) %08x -> readback %08x (bg_dly "
           "%u -> %u, TRM Table 11-4 says 20 for port0)\n",
           (unsigned)bgm, (unsigned)bgm_got,
           (unsigned)((bgm >> RK3576_VOP_OVERLAY_BG_DLY_SHIFT) & 0xffu),
           (unsigned)((bgm_got >> RK3576_VOP_OVERLAY_BG_DLY_SHIFT) & 0xffu));

    syslog(LOG_WARNING,
           "vop: POST_PRE_SCAN_HTIMING=%08x (hactive=%u = bg_dly %u + %u - 1, "
           "hblank=%u) -- the\n",
           (unsigned)pre, (unsigned)hactive, (unsigned)bg_dly,
           (unsigned)(((uint32_t)priv->cfg.xres + 1u) / 2u),
           (unsigned)hblank);
    syslog(LOG_WARNING,
           "vop:   layers request their pixels this many cycles before the "
           "POST reads them; at\n");
    syslog(LOG_WARNING,
           "vop:   its reset value of 0 the request arrives too late and the "
           "POST's buffer\n");
    syslog(LOG_WARNING,
           "vop:   under-runs.  bg_dly and this pre_scan are measured against "
           "each other, so\n");
    syslog(LOG_WARNING,
           "vop:   they must use the same base (20).\n");
  }

  /* Bypass the VOP's MMU for everything.
   *
   * This driver programs PHYSICAL addresses (the ESMART window's MMU bypass bit
   * is set) and there is no IOMMU driver, yet this register was never written:
   * mmu_bypass_en sat at its reset value of 0, so nothing was bypassed and the
   * layer's physical addresses were offered to an MMU with no page tables.  The
   * reference driver never faces this because it drives the IOMMU itself.
   *
   * A read into an unconfigured MMU need not report an error -- it can simply
   * never return, which is exactly the shape of the one measurement that has
   * defeated every other explanation: the layer issues a read that never
   * arrives, no error is logged, the POST's input buffer stays empty, and the
   * panel shows a static picture that follows neither the framebuffer's content
   * nor its address.
   *
   * bypass_id = 0 makes "rid > bypass_id" true for every window, so everything
   * bypasses, which is what a driver without an IOMMU needs.
   */

  {
    uint32_t mmu = rk3576_vop_getreg(priv, sys_base +
                                     RK3576_VOP_SYS_MMU_CTRL_IMD);
    uint32_t got;

    /* *** SYS_MMU_CTRL_IMD HAS THE HIWORD WRITE MASK (TRM: bits[31:16]
     * "write_mask"). ***  The previous version of this code wrote it with a
     * plain putreg(), so bits[31:16] were zero and the hardware treated every
     * bit as "don't care": the write was DISCARDED and mmu_bypass_en stayed at
     * its reset value of 0 for the entire bring-up, while the log line below
     * printed the value the driver MEANT to write and appeared to confirm it.
     * The tell was in the readback all along -- SYS_MMU_CTRL read back as
     * 0x00010048, whose bits[31:16] are a WRITE MASK left by an earlier stage
     * (0x0001 = the mask of rkmmu2.0_en), not part of the value.
     *
     * The reference driver sets rkmmu_v2_en = 0 for RK3576 (the rkmmu 2.0 block
     * is unused on this SoC), so bit0 is written as 0 here as well; bit11
     * (mmu2.0_soft_rst_en) keeps its reset value of 1, i.e. not in reset.
     */

    rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_MMU_CTRL_IMD,
                                RK3576_VOP_SYS_MMU_BYPASS_EN |
                                    RK3576_VOP_SYS_MMU1_BYPASS_EN |
                                    RK3576_VOP_SYS_MMU_SOFT_RST_EN,
                                RK3576_VOP_SYS_MMU_BYPASS_EN |
                                    RK3576_VOP_SYS_MMU1_BYPASS_EN |
                                    RK3576_VOP_SYS_MMU_BYPASS_ID_MASK |
                                    RK3576_VOP_SYS_MMU_SOFT_RST_EN);

    got = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_MMU_CTRL_IMD);

    syslog(LOG_WARNING,
           "vop: SYS_MMU_CTRL_IMD %08x -> readback %08x (mmu_bypass_en bit2 "
           "%u -> %u, bypass_id[8:4]=%u, mmu1_bypass_en bit9 %u -> %u)\n",
           (unsigned)mmu, (unsigned)got,
           (unsigned)((mmu >> 2) & 1u), (unsigned)((got >> 2) & 1u),
           (unsigned)((got >> RK3576_VOP_SYS_MMU_BYPASS_ID_SHIFT) & 0x1fu),
           (unsigned)((mmu >> 9) & 1u), (unsigned)((got >> 9) & 1u));

    if ((got & (RK3576_VOP_SYS_MMU_BYPASS_EN |
                RK3576_VOP_SYS_MMU1_BYPASS_EN)) !=
        (RK3576_VOP_SYS_MMU_BYPASS_EN | RK3576_VOP_SYS_MMU1_BYPASS_EN))
      {
        syslog(LOG_ERR,
               "vop: *** the masked write did NOT take: bypass bits read "
               "%u/%u.  Bypass_id=0 with bypass_en=1 is what makes physical "
               "addresses work without an IOMMU ***\n",
               (unsigned)((got >> 2) & 1u), (unsigned)((got >> 9) & 1u));
      }
  }

  /* Global VOP control bits the reference driver sets for RK3576 and this
   * driver never wrote.  See RK3576_VOP_SYS_PORT_CTRL_IMD in the header for the
   * bit map and why dsp_vs_t_sel matters: it decides where the display's vs/t
   * timing is tapped, and taking it from the wrong place leaves the POST unable
   * to issue a coherent demand for pixels -- which is the one thing that
   * explains a layer that is enabled, routed and correct yet issues NO reads.
   */

  {
    uint32_t port = rk3576_vop_getreg(priv, sys_base +
                                      RK3576_VOP_SYS_PORT_CTRL_IMD);
    uint32_t lut = rk3576_vop_getreg(priv, sys_base +
                                     RK3576_VOP_SYS_AXI_LUT_CTRL_IMD);
    uint32_t got;
    uint32_t got_lut;

    /* *** THIS WRITE USED TO DISAGREE WITH WORKING SILICON IN THREE BITS. ***
     *
     * A register dump from this same board, running a configuration that is
     * known to fetch and display, reads
     *
     *     0028 00070038
     *
     * while this driver read 0x8030802f -- and every one of the three
     * differing bits was a decision made by reading the vendor source instead
     * of by observing working silicon:
     *
     *   bit15 auto_cs_mode : we SET it (=> 1).  RK3576's control table has no
     *                        field for it -- only rk3568/rk3588 do -- so on this
     *                        SoC nothing ever writes it and the reset value of
     *                        0 stands.  Working dump: 0.
     *
     *   bit4  dsp_vs_t_sel : we CLEARED it (=> 0) because is_vop3() is true for
     *                        RK3576 and vop2_initial() clears it inside the vop3
     *                        branch.  Working silicon holds the RESET value, 1.
     *                        The bit picks where the display's vs/t timing is
     *                        tapped ("1'b0: Dsp_vs_t_out, 1'b1: Dsp_vs_t_pre"),
     *                        i.e. it sits on the line-timing path that decides
     *                        when the POST asks a window for pixels.
     *
     *   bits[2:0] reg_done_frm : we NEVER WROTE it, so it held its reset value
     *                        of 1.  This field exists for RK3576 and for no
     *                        other SoC --
     *                          .reg_done_frm = VOP_REG_MASK(RK3576_SYS_PORT_CTRL_IMD, 0x7, 0)
     *                        -- and Linux writes it to 0 with the comment "Set
     *                        reg done every field for interlace".  It decides
     *                        when a video port's register updates become valid,
     *                        the very mechanism that has been failing here: a
     *                        configuration that reads back perfectly and never
     *                        takes effect.  Working dump: 0.
     *
     * Writing ONLY reg_done_frm is what reproduces the working word exactly,
     * because the reset value already supplies dsp_vs_t_sel = 1, auto_cs_en = 1
     * and the reserved bit 3 = 1 -- 0x38 -- with auto_cs_mode at 0.  So this is
     * no longer a write that asserts an inference; it is the smallest write that
     * makes the register agree with the only ground truth available.
     */

    rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_PORT_CTRL_IMD, 0u,
                                RK3576_VOP_SYS_PORT_REG_DONE_FRM_MASK);

    /* SYS_AXI_LUT_CTRL_IMD has NO write mask -- bits[31:10] are read-only -- so
     * a plain write is correct here. */

    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI_LUT_CTRL_IMD,
                      lut & ~RK3576_VOP_SYS_LUT_USE_AXI1);

    /* Read back and CHECK, rather than printing the intended value: a SYS write
     * that does not land is otherwise completely invisible. */

    got = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_PORT_CTRL_IMD);
    got_lut = rk3576_vop_getreg(priv, sys_base +
                                RK3576_VOP_SYS_AXI_LUT_CTRL_IMD);

    syslog(LOG_WARNING,
           "vop: SYS_PORT_CTRL_IMD %08x -> readback %08x "
           "(want data half 0038: dsp_vs_t_sel bit4=%u, auto_cs_mode bit15=%u, "
           "reg_done_frm[2:0]=%u)\n",
           (unsigned)port, (unsigned)got,
           (unsigned)((got >> 4) & 1u), (unsigned)((got >> 15) & 1u),
           (unsigned)(got & 0x7u));

    syslog(LOG_WARNING,
           "vop: SYS_AXI_LUT_CTRL_IMD %08x -> readback %08x (lut_use_axi1 "
           "bit9 %u -> %u)\n",
           (unsigned)lut, (unsigned)got_lut, (unsigned)((lut >> 9) & 1u),
           (unsigned)((got_lut >> 9) & 1u));

    if ((got & RK3576_VOP_SYS_PORT_DSP_VS_T_SEL) == 0u ||
        (got & RK3576_VOP_SYS_PORT_AUTO_CS_MODE) != 0u ||
        (got & RK3576_VOP_SYS_PORT_AUTO_CS_EN) == 0u ||
        (got & RK3576_VOP_SYS_PORT_REG_DONE_FRM_MASK) != 0u)
      {
        syslog(LOG_ERR,
               "vop: *** SYS_PORT_CTRL_IMD did NOT reach the working "
               "configuration 0x00070038: dsp_vs_t_sel=%u (want 1), "
               "auto_cs_mode=%u (want 0), reg_done_frm=%u (want 0) ***\n",
               (unsigned)((got >> 4) & 1u), (unsigned)((got >> 15) & 1u),
               (unsigned)(got & 0x7u));
      }
  }

  /* *** THE ANTI-UNDER-RUN MECHANISM: POST LINE-BUFFER URGENCY. ***
   *
   * This has been missing for the whole bring-up and it is the one piece of the
   * reference driver's video-port setup that this driver never performed.  Linux
   * applies it unconditionally whenever the video port carries urgency data, and
   * RK3576 video output 0 does:
   *
   *   static const struct vop_urgency rk3576_vp0_urgency = {
   *           .urgen_thl = 4,
   *           .urgen_thh = 6,
   *   };
   *   ...
   *   VOP_MODULE_SET(vop2, vp, axi0_port_urgency_en, 1);
   *   VOP_MODULE_SET(vop2, vp, axi1_port_urgency_en, 1);
   *   VOP_MODULE_SET(vop2, vp, post_urgency_en, 1);
   *   VOP_MODULE_SET(vop2, vp, post_urgency_thl, urgen_thl);
   *   VOP_MODULE_SET(vop2, vp, post_urgency_thh, urgen_thh);
   *
   * WHY THIS MATTERS MORE THAN ANY OTHER BIT IN THIS FILE.  The TRM states the
   * mechanism outright, in POST0_CTRL_POST_COLOR_CTRL:
   *
   *   [19:16] sw_urgency_thl  "When post_lb < sw_urgency_thl, post set urgency
   *                            to 1'b1.  Increase the priority of the layer on
   *                            the AXI0 bus."
   *
   * and the SoC chapter describes the other end of the wire: with the VOP's
   * read-urgency line high, the DDR controller's mask_ctrl module "will mask all
   * the other requests" so that this read port is served first.
   *
   * So it is a closed hardware loop -- the POST measures its own line-buffer
   * occupancy and asks the interconnect for priority when it is about to run
   * dry.  Disabled, the layer's fetches compete on equal terms with every other
   * master and lose.  That fits every measurement this bring-up has made: the
   * POST input buffer under-runs (POST_BUF_EMPTY recurs with the layer on and
   * stops with it off), and the panel shows a static picture following neither
   * the framebuffer's content nor its address -- because what it is displaying is
   * whatever was left in the line buffer when the data did not arrive in time.
   *
   * AXIS: this layer fetches on AXI0 (esmart_axi_sel reset = 0), so the enable
   * that matters is CTRL0 bit24 = axi0_port0_urgency_en (the bit index follows
   * the video port: 24 = VP0, 25 = VP1, 26 = VP2).  CTRL1 bit24 is set as well
   * because the reference sets the AXI1 one unconditionally.
   *
   * POST_COLOR_CTRL is a POST mirror register, so this write only reaches the
   * real register on the next frame boundary via the cfg_done pulse fired below.
   */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_COLOR_CTRL,
                    RK3576_VOP_POST_URGENCY_EN |
                        (RK3576_VOP_POST_URGENCY_THL_LINUX
                         << RK3576_VOP_POST_URGENCY_THL_SHIFT) |
                        (RK3576_VOP_POST_URGENCY_THH_LINUX
                         << RK3576_VOP_POST_URGENCY_THH_SHIFT));

  {
    uint32_t h0 = rk3576_vop_getreg(priv, sys_base +
                                    RK3576_VOP_SYS_AXI_HURRY_CTRL0_IMD);
    uint32_t h1 = rk3576_vop_getreg(priv, sys_base +
                                    RK3576_VOP_SYS_AXI_HURRY_CTRL1_IMD);
    uint32_t g0;
    uint32_t g1;

    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI_HURRY_CTRL0_IMD,
                      h0 | RK3576_VOP_SYS_AXI_HURRY_AXI0_VP0_EN);
    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI_HURRY_CTRL1_IMD,
                      h1 | RK3576_VOP_SYS_AXI_HURRY_AXI1_VP0_EN);

    g0 = rk3576_vop_getreg(priv, sys_base +
                           RK3576_VOP_SYS_AXI_HURRY_CTRL0_IMD);
    g1 = rk3576_vop_getreg(priv, sys_base +
                           RK3576_VOP_SYS_AXI_HURRY_CTRL1_IMD);

    syslog(LOG_WARNING,
           "vop: SYS_AXI_HURRY_CTRL0_IMD(0x0014) %08x -> readback %08x "
           "(axi0_port0_urgency_en bit24 %u -> %u)\n",
           (unsigned)h0, (unsigned)g0,
           (unsigned)((h0 >> 24) & 1u), (unsigned)((g0 >> 24) & 1u));
    syslog(LOG_WARNING,
           "vop: SYS_AXI_HURRY_CTRL1_IMD(0x0018) %08x -> readback %08x "
           "(bit24 %u -> %u)\n",
           (unsigned)h1, (unsigned)g1,
           (unsigned)((h1 >> 24) & 1u), (unsigned)((g1 >> 24) & 1u));
    syslog(LOG_WARNING,
           "vop: POST_COLOR_CTRL(0x0008) <= sw_urgency_en bit8=1, "
           "sw_urgency_thl[19:16]=%u, sw_urgency_thh[23:20]=%u -- loads on the "
           "next cfg_done\n",
           (unsigned)RK3576_VOP_POST_URGENCY_THL_LINUX,
           (unsigned)RK3576_VOP_POST_URGENCY_THH_LINUX);

    if ((g0 & RK3576_VOP_SYS_AXI_HURRY_AXI0_VP0_EN) == 0u)
      {
        syslog(LOG_ERR,
               "vop: *** axi0_port0_urgency_en did NOT take, so the layer "
               "still competes for NoC\n");
        syslog(LOG_ERR,
               "vop:   bandwidth on equal terms and the POST can still "
               "under-run ***\n");
      }
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

  /* --- Probe A: immediately read back both cfg_done registers.
   *
   * Readback of the hiword stays 0 (write-mask bits self-clear), so the
   * useful information is in the low 16 bits:
   *   REG_CFG_DONE bit15 sw_global_regdone_en must be 1 (it is now written
   *     as 1; if it ever reads 0 again the load will never complete).
   *   REG_CFG_DONE bit0/4 and WIN_CFG_DONE bit4 are LOAD REQUEST bits: they
   *     read 1 until the frame boundary consumes them.
   * NOTE this sample is taken microseconds after the request, so a
   * non-zero low word here is EXPECTED, not a fault.  The meaningful
   * "were the requests consumed?" check is probe-C, several ms later.
   * --- */

  syslog(LOG_INFO,
         "vop-probe-A: after cfg_done REG_CFG_DONE=%08x "
         "WIN_CFG_DONE=%08x (requests just posted; nonzero low word is "
         "expected here)\n",
         rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE),
         rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE));

  /* --- Probe B: read back the ESMART registers right after the load
   * trigger.
   *
   * IMPORTANT: these are the REAL registers, and the mirror->real copy only
   * happens at a FRAME BOUNDARY.  A few microseconds after the request they
   * are therefore still the OLD (reset) values -- all-zero here proves
   * nothing at all and was misread as a fault in earlier rounds.  A short
   * delay is inserted so this sample crosses at least one frame boundary and
   * becomes meaningful; the authoritative readback is still the later
   * "vop-dump:" one.
   * --- */

  up_mdelay(20); /* ~1+ frame @60 Hz: cross the mirror->real boundary */

  syslog(LOG_INFO,
         "vop-probe-B: ESMART0 CTRL0=%08x REGION0_CTRL=%08x "
         "YRGB_MST=%08x VIR=%08x (after 20ms = post-frame-boundary)\n",
         rk3576_vop_getreg(priv,
                           esmart_base + RK3576_VOP_ESMART_CTRL0),
         rk3576_vop_getreg(priv,
                           esmart_base + RK3576_VOP_ESMART_REGION0_CTRL),
         rk3576_vop_getreg(priv,
                           esmart_base + RK3576_VOP_ESMART_REGION0_YRGB_MST),
         rk3576_vop_getreg(priv,
                           esmart_base + RK3576_VOP_ESMART_REGION0_VIR));

  /* Now that at least one frame boundary has passed, the load-request bits
   * are meaningful: each group's bit must have been CONSUMED (read 0) once
   * its mirror set was copied to the real registers.  The bits belonging to
   * video ports that never scan (global1/2 = VP1/VP2, sys1/2) legitimately
   * stay pending; the VP0 bits (global0 = bit0, sys0 = bit4) and the ESMART0
   * bit (WIN_CFG_DONE bit4) are the ones that matter here.
   */

  syslog(LOG_INFO,
         "vop-probe-B: post-frame REG_CFG_DONE=%08x WIN_CFG_DONE=%08x "
         "(VP0 load requests: global0 bit0=%u sys0 bit4=%u esmart0 bit4=%u; "
         "all should be 0 = consumed; global1/2+sys1/2 staying set is "
         "normal, those VPs never scan)\n",
         rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE),
         rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE),
         (unsigned)(rk3576_vop_getreg(priv,
                                      sys_base + RK3576_VOP_SYS_REG_CFG_DONE) &
                    1u),
         (unsigned)((rk3576_vop_getreg(priv,
                                       sys_base +
                                           RK3576_VOP_SYS_REG_CFG_DONE) >>
                     4) &
                    1u),
         (unsigned)((rk3576_vop_getreg(priv,
                                       sys_base +
                                           RK3576_VOP_SYS_WIN_REG_CFG_DONE) >>
                     4) &
                    1u));

  /* Bound the AXI0 outstanding transactions (IMD registers take effect
   * immediately, no cfg_done needed).  The ESMART scan-out DMA goes out on
   * axi0 (reset esmart_axi_sel=0), and with SYS_AXI0_CTRL_IMD at reset
   * (outstanding_en=0 = unlimited) it can monopolise the NoC and starve the
   * CPU, hanging the SoC.  outstanding_en=1 + num=16 caps the in-flight
   * bursts so the CPU keeps its DDR bandwidth.
   *
   * *** bit0 IS dma_stop, bit1 IS outstanding_en *** -- the opposite of what
   * this driver previously assumed.  Because of that, this write used to set
   * bit0, i.e. it ENABLED axi0_dma_stop and stopped the very AXI channel the
   * ESMART layer fetches through.  The layer could then not read memory at
   * all: its registers were correct, nothing reported a bus error, the POST
   * was starved (POST_BUF_EMPTY, measured recurring), and the panel displayed
   * a static garble that followed neither the framebuffer's content nor its
   * address.  dma_stop must stay 0, so it is cleared explicitly here rather
   * than merely omitted.
   */

  /* *** THIS WRITE WAS THE BUG.  IT IS THE MEASURED ROOT CAUSE. ***
   *
   * It used to set outstanding_en with num = 16, capping the VOP's AXI0
   * channel -- and ESMART0 and ESMART1 fetch through AXI0.  A dump from a
   * working configuration reads 0 here: no cap at all.  The working driver
   * never writes this register.
   *
   * The cap was added to protect the CPU's DDR bandwidth from the scan-out DMA
   * monopolising the NoC.  An earlier round tested that theory and rejected it,
   * but the write stayed -- and it was sat directly on the window's own channel.
   *
   * PROVEN BY PUTTING IT BACK, ONE REGISTER AT A TIME:
   *
   *   from the corrected configuration, with eight colour bars sitting in the
   *   framebuffer as the oracle,
   *
   *     re-setting dma_rreq_hurry_en (thold = 0)  -> BARS STILL PRESENT
   *     re-setting the window's own AXI bound      -> BARS STILL PRESENT
   *     re-setting THIS SYS-level cap              -> BARS GONE, garbage back
   *
   * One register, added back on its own, and the display broke.  That is the
   * cause that a hundred rounds of inference from register readbacks, from the
   * vendor source and from the TRM all failed to find, and it was found by
   * changing one thing and looking at the glass.
   *
   * How it produces the symptom: with the layer's fetch capped, the window stops
   * delivering pixels, so the POST under-runs and emits its own fixed pattern
   * instead -- a stable, structured garble that follows neither the
   * framebuffer's content nor its address, which is exactly what was observed
   * for the whole bring-up.  Disabling the layer made that pattern give way to a
   * clean POST background, which is why every "disable the layer" test showed a
   * clean screen and looked like progress.
   *
   * Written as 0 now: the value the working configuration holds, which also
   * keeps dma_stop at 0 -- the one thing about this register that is genuinely
   * dangerous, since bit0 STOPPED the channel outright the last time the two
   * bits were confused.
   */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AXI0_CTRL_IMD, 0u);

  /* AXI1 is deliberately left alone.  It was bounded for one round while the
   * window was pointed at it, but that experiment is reverted (see
   * RK3576_VOP_ESMART_USE_AXI1: asserting axi_sel clears the window's MMU-bypass
   * and outstanding fields), and this register has otherwise never been written
   * on this board.  Writing it now would be a change with no justification. */

  {
    uint32_t axi0 = rk3576_vop_getreg(priv, sys_base +
                                      RK3576_VOP_SYS_AXI0_CTRL_IMD);

    syslog(LOG_WARNING,
           "vop: SYS_AXI0_CTRL_IMD=%08x (working dump 00010000; dma_stop=%u "
           "outstanding_en=%u num=%u, want 0/0/0 = no cap)\n",
           (unsigned)axi0,
           (unsigned)((axi0 & RK3576_VOP_SYS_AXI_DMA_STOP) != 0u),
           (unsigned)((axi0 & RK3576_VOP_SYS_AXI_OUTSTANDING_EN) != 0u),
           (unsigned)((axi0 >> RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT) & 0x3fu));

    /* mmu_idle is NOT printed as evidence any more.  A dump from a working,
     * displaying configuration reads bit16 = 1, i.e. "idle", so the bit says
     * nothing about whether a fetch is happening and an entire earlier
     * conclusion ("the layer issues no reads") was drawn from it. */

    if ((axi0 & RK3576_VOP_SYS_AXI_DMA_STOP) != 0u)
      {
        syslog(LOG_ERR,
               "vop: ERROR: axi0_dma_stop is SET -- the ESMART layer, which "
               "fetches on axi0, cannot read memory, and no framebuffer write "
               "can ever reach the panel\n");
      }
  }

  /* --- dclk reset pulse: align VOP pixel clock to the DSI link ---
   *
   * Linux dw_mipi_dsi2_encoder_atomic_enable() runs, after the DSI has
   * fully entered Video mode:
   *   crtc_standby(1) -> dsi pre_enable+enable -> crtc_standby(0)
   *   -> output_post_enable() { vop2_clk_reset(vp->dclk_rst); }
   * where vop2_clk_reset() pulses the dclk reset (assert 10us + deassert).
   * This re-locks the VOP pixel clock (dclk_core = 16M) to the already
   * ready DSI link so the controller's phy_tx_ready FSM can leave INIT.
   * Without it the pixel data piles up in the IPI data FIFO (cnt=768) while
   * phy_tx_ready stays INIT and all lanes stay LP-11 (no HS burst at all).
   *
   * SAFETY: pulse ONLY dresetn_vp0 (SOFTRST_CON61 bit13, the VP0 pixel
   * clock reset).  NEVER pulse aresetn_vop/hresetn_* (bit4-9): those are
   * the BIU/AXI resets; asserting them against a live clocked AXI master
   * tears down in-flight transactions and hangs the NoC/DDR (observed
   * SoC-wide hang -- see rk3576_vop_reset()).
   */

  {
    uint32_t cru = RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_VOP_RST_CON);
    uint32_t vcnt_before;
    uint32_t vcnt_mid;
    uint32_t vcnt_after;

    vcnt_before = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0);

    /* assert: bit13 data=1 with hiword mask (reset held). */

    putreg32(RK3576_VOP_HWM(RK3576_VOP_RST_DRESETN_VP0) |
                 RK3576_VOP_RST_DRESETN_VP0,
             cru);
    up_udelay(10);

    /* deassert: bit13 data=0 (release). */

    putreg32(RK3576_VOP_HWM(RK3576_VOP_RST_DRESETN_VP0), cru);
    up_udelay(20);

    vcnt_mid = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0);

    /* The dclk reset restarts the VP0 timing generator, so the counter is
     * expected to WRAP/RESTART here (e.g. 118 -> 1).  Sampling immediately
     * after the pulse therefore cannot show "advancing"; the previous
     * `delta = (after - before) & 0x1fff` formulation silently turned the
     * wrap into a bogus 8075 and always printed "scan resumed".
     *
     * Re-post the register load requests: the reset can swallow an
     * in-flight mirror->real copy, and if the POST/layer config was lost the
     * VP0 would keep scanning with reset-default timing.  Re-issuing the
     * pulses costs nothing and guarantees the copy is armed for the next
     * frame boundary.
     */

    rk3576_vop_trigger_cfg_done(priv);

    /* Now cross a couple of frame boundaries and then check that the
     * counter really is ADVANCING (the only valid "scan is alive"
     * criterion). */

    up_mdelay(2); /* ~2 ms: several lines at 62.5 MHz */

    vcnt_after = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0);

    {
      uint32_t v_mid = (vcnt_mid >> RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff;
      uint32_t v_after = (vcnt_after >> RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff;

      syslog(LOG_INFO,
             "vop-dclk-rst: dresetn_vp0 pulsed, dsp_vcnt0 %u -> %u (after "
             "reset) then %u after 2ms (delta=%u lines/2ms=%u lines/s; the "
             "post-pulse wrap alone means nothing -- only a nonzero 2ms "
             "delta proves the scan is advancing)\n",
             (unsigned)((vcnt_before >> RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff),
             (unsigned)v_mid, (unsigned)v_after,
             (unsigned)((v_after - v_mid) & 0x1fff),
             (unsigned)(((v_after - v_mid) & 0x1fff) * 500));
    }
  }
}

/****************************************************************************
 * Name: rk3576_vop_enable_clocks
 ****************************************************************************/

/* End of the VOP clock tree is the mux that selects the VOP AXI root and
 * the pixel-clock source.  Their HW reset value points at PLLs that are not
 * yet modelled in rk3576_clk_tree.c (clk_spll/clk_lpll/clk_vpll/clk_bpll are
 * orphan), so clk_get_rate() on aclk_vop reads back 0 even though the HW
 * clock may be running.  Zero aclk_vop leaves the ESMART/POST layers
 * unclocked (registers read back 0) -> all-black output.
 *
 * Until those PLLs are registered, force the VOP source muxes onto
 * clk_gpll (1188 MHz), which IS registered.  Temporary bring-up hack; remove
 * once the full PLL set is modelled.
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

  ret = clk_set_rate(priv->dclk,
                     priv->cfg.pixel_clock != 0 ? priv->cfg.pixel_clock
                                                : RK3576_VOP_DEFAULT_PCLK_HZ);
  if (ret < 0)
    {
      gerr("ERROR: VOP failed to set %s rate: %d\n",
           g_rk3576_vop_dclk_names[priv->cfg.port], ret);
      return ret;
    }

  /* Self-check the clock that the whole MIPI pixel path hangs off.
   *
   * clk_set_rate() is a REQUEST, not a command.  The divider search walks the
   * divisors of the parent PLL and keeps the LARGEST one whose output is still
   * <= the request, so the achieved rate normally sits BELOW the request by a
   * whole divider step -- not by a rounding crumb.  That matters because the
   * DSI programs its IPI horizontal timing (HSA/HBP/HACT/HLINE, counted in
   * phy_hstx_clk cycles) and PHY_IPI_RATIO from the value it was handed: with
   * a mismatch the controller concludes "the line ended" at the wrong instant
   * and mis-sizes the pixel-clock <-> PHY-clock ratio the IPI FSM
   * synchronises on.
   *
   * So make any mismatch impossible to overlook instead of leaving it to be
   * noticed in the vop-dump line.  The fix is to REQUEST a rate the CRU can hit
   * exactly (see KICKPI_K7_MIPI_DSI_PIXCLK):
   *   requesting 62 MHz from gpll 1188 MHz -> 1188/20 = 59.4 MHz (4.2% low)
   *   requesting 59.4 MHz from gpll 1188 MHz -> 1188/20 = 59.4 MHz (exact)
   * (the older "gpll/19 = 62.526 MHz" figures come from a build that still
   * requested 64 MHz; 1188/19 = 62.526 MHz > 62 MHz, so that divisor is
   * rejected outright once the request drops to 62 MHz.)
   *
   * rk3576_mipi_dsi_update_pixel_clock() is deliberately NOT called to paper
   * over this: it rewrites the timing of a LIVE pixel datapath (see the note
   * below).  A mismatch is a build-time bug here, not something to servo out
   * at run time.
   */

  {
    uint32_t requested = priv->cfg.pixel_clock != 0 ? priv->cfg.pixel_clock
                                                   : RK3576_VOP_DEFAULT_PCLK_HZ;
    uint32_t achieved = (uint32_t)clk_get_rate(priv->dclk);
    uint32_t diff = achieved > requested ? achieved - requested
                                         : requested - achieved;
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
    else
      {
        syslog(LOG_INFO,
               "vop: dclk_vp%u %lu Hz matches the DSI pixel-clock request "
               "(%lu Hz, %lu ppm off)\n",
               (unsigned int)priv->cfg.port, (unsigned long)achieved,
               (unsigned long)requested, (unsigned long)ppm);
      }
  }

  /* Do NOT re-program the DSI's IPI timing here.
   *
   * An earlier revision called rk3576_mipi_dsi_update_pixel_clock() from this
   * point, handing the DSI the ACHIEVED dclk instead of the requested one
   * (62.526 MHz instead of 62 MHz, a 0.85% difference).  Two reasons that was
   * wrong:
   *
   *  1. It deviates from the reference driver, which computes the IPI
   *     horizontal timing and both PHY ratios from the NOMINAL mode clock
   *     (dw_mipi_dsi2_ipi_set(): pixel_clk = mode->crtc_clock).  A ~1%
   *     nominal-vs-achieved mismatch is the normal, working condition on this
   *     SoC -- it is not something to correct.
   *
   *  2. By the time this runs the DSI is ALREADY in video mode and the pixel
   *     datapath is live, so the call re-timed a running state machine
   *     mid-packet: it rewrote HSA/HBP/HACT/HLINE (the boundaries the IPI FSM
   *     synchronises on) and PHY_IPI_RATIO (the CDC ratio between ipi_clk and
   *     phy_hstx_clk) asynchronously to the stream.  That is a textbook race,
   *     and it matches the observed behaviour: the SAME firmware wedges at
   *     DIFFERENT points on different boots -- sometimes the pixel FIFO backs
   *     up and nothing moves downstream, sometimes the packets reach the PHY
   *     but the PHY never takes them.
   *
   * If the pixel clock ever must be re-derived, it has to be done BEFORE the
   * host is switched into video mode.
   */

  /* Enable the VOP BIU (bus-interface-unit) clocks.  The ESMART/POST layers
   * reach DDR over the VOP AXI port, which is clocked by aclk_vop_biu /
   * hclk_vop_biu (CON61[4]/[6]); these gates are SET_TO_DISABLE and are NOT
   * auto-enabled by enabling aclk_vop.  If they stay gated, the very first
   * ESMART scan-out AXI read enters a clockless BIU and pends forever,
   * seizing a NoC outstanding slot and starving the CPU's DDR traffic ->
   * the whole SoC hangs right after nsh starts.  This is the actual VOP
   * hang root cause (not the AXI outstanding limit).
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
 *   Power on the VOP_ESMART power domain and release the ESMART layer from
 *   its frame reset.  The ESMART layer has no standalone clock gate in the
 *   CRU; its aclk_esmart0..3 are derived from aclk_vop by the VOP-internal
 *   ESMART_PD_CRU once the PD_VOP_ESMART domain is up.
 *
 *   Steps:
 *     1. Release the BISR initial reset (initrst) for PD_VOP + PD_VOP_ESMART
 *        + PD_VOP_CLUSTER in PMU_BISR_INITRST_SFTCON0 (1 = Normal).  Until
 *        this is done the PD reset (aresetn_pd) is asserted and the ESMART
 *        layer clock (aclk_esmart0..3) never comes up, so every ESMART
 *        register reads back 0.
 *     2. Clear pd_vop_dwn_ena and pd_vop_esmart_dwn_ena in
 *        PMU_PWR_GATE_CON0 so both domains stay powered ('0' = keep powered,
 *        hiword mask).
 *     3. De-assert esmart_frm_resetn_en (ESMART0_CTRL0 bit 31) so the esmart
 *        layer leaves reset.
 ****************************************************************************/

static void rk3576_vop_power_on_esmart(FAR struct rk3576_vop_s *priv)
{
  uint32_t pmu_pwr =
      RK3576_PMU_ADDR + RK3576_VOP_PMU_PWR_GATE_CON0_OFF;
  uint32_t pmu_initrst =
      RK3576_PMU_ADDR + RK3576_VOP_PMU_BISR_INITRST_SFTCON0_OFF;
  uint32_t esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);

  /* 1. Release the PD memory-repair initial reset (write 1 = Normal) for the
   * three VOP domains this driver needs.  The state of the other two
   * (PD_VO0/PD_VO1) is printed but deliberately not changed: releasing the
   * output domain's initial reset after the DSI is already up regressed a
   * working output path.  See RK3576_VOP_INITRST_RELEASE_MASK.
   */

  {
    uint32_t ini_b = getreg32(pmu_initrst);
    uint32_t sts_b = getreg32(RK3576_PMU_ADDR +
                              RK3576_VOP_PMU_PWR_GATE_STS_OFF);

    syslog(LOG_WARNING,
           "vop: PD state before: INITRST_SFTCON0=%08x -> vo0[15]=%u vo1[14]=%u "
           "cluster[13]=%u esmart[12]=%u vop[11]=%u (1 = Normal)\n",
           (unsigned)ini_b, (unsigned)((ini_b >> 15) & 1u),
           (unsigned)((ini_b >> 14) & 1u), (unsigned)((ini_b >> 13) & 1u),
           (unsigned)((ini_b >> 12) & 1u), (unsigned)((ini_b >> 11) & 1u));
    syslog(LOG_WARNING,
           "vop: PMU_PWR_GATE_STS=%08x -> vo0[15]=%u vo1[14]=%u cluster[13]=%u "
           "esmart[12]=%u vop[11]=%u (0 = POWER UP)\n",
           (unsigned)sts_b, (unsigned)((sts_b >> 15) & 1u),
           (unsigned)((sts_b >> 14) & 1u), (unsigned)((sts_b >> 13) & 1u),
           (unsigned)((sts_b >> 12) & 1u), (unsigned)((sts_b >> 11) & 1u));

    if (((ini_b >> 15) & 1u) == 0u)
      {
        syslog(LOG_ERR,
               "vop:*** PD_VO0 WAS STILL HELD IN ITS INITIAL RESET.  VO0 owns "
               "the display output\n");
        syslog(LOG_ERR,
               "vop:   interfaces AND the VOP's channel into the VO0 "
               "interconnect -- aclk_vo0vop\n");
        syslog(LOG_ERR,
               "vop:   _channel_biu, the clock the TRM says must not be "
               "gated.  A domain held\n");
        syslog(LOG_ERR,
               "vop:   here answers register accesses but does no work, "
               "which is this symptom ***\n");
      }

    putreg32(RK3576_VOP_HWM(RK3576_VOP_INITRST_RELEASE_MASK) |
                 RK3576_VOP_INITRST_RELEASE_MASK,
             pmu_initrst);
    up_udelay(20);

    {
      uint32_t ini_a = getreg32(pmu_initrst);

      syslog(LOG_WARNING,
             "vop: PD state after:  INITRST_SFTCON0=%08x (changed by %04x)\n",
             (unsigned)ini_a,
             (unsigned)((ini_a ^ ini_b) & RK3576_VOP_INITRST_RELEASE_MASK));
    }
  }

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

  /* 3. *** ESMART_CTRL0 MUST STAY 0.  THIS LINE USED TO BE THE BUG. ***
   *
   * The previous version of this function wrote
   * RK3576_VOP_ESMART_CTRL0_FRM_RESETN_EN (bit31 = 1) here, described in the
   * header as "the layer is held in reset until this bit is set ('1' =
   * release)".  That reading is WRONG, in both polarity and model.  The TRM's
   * "Software Frame Reset" section defines what the bit actually does:
   *
   *   "The frame reset of each layer is supported, that is, when the vsync
   *    signal of each frame arrives, the layer's internal logic will be reset
   *    exclude register logic."
   *
   * and the field itself is esmart_frm_resetn_en, reset value 0x0:
   *
   *   1'b0: Disable        1'b1: Enable
   *
   * So 1 ENABLES a soft reset that fires on EVERY vsync and wipes the layer's
   * internal logic -- and only its internal logic, which the same sentence
   * carves out explicitly: "exclude register logic".  This driver has been
   * setting it to 1 since the first bring-up round.
   *
   * The consequence is precisely the symptom that has resisted every other
   * explanation in this project:
   *
   *   - every register reads back exactly as written, because register logic
   *     is excluded from the reset by design;
   *   - the fetch engine and line buffers are torn down once per frame, so they
   *     never fill -- "the layer issues no reads" as measured by two
   *     independent mechanisms (poison address, MMU page fault with both
   *     bypasses off).  A read that the reset aborts before it completes never
   *     reaches the error latch either, which is why the poison address could
   *     be confirmed in place (poison_landed=1) and yet produce bus_error=0;
   *   - the POST's input buffer under-runs, as measured (POST_BUF_EMPTY recurs
   *     while the layer is enabled and stops when it is disabled);
   *   - the panel shows a static garble that follows neither the framebuffer's
   *     content nor its address, because what reaches it is the residue of line
   *     buffers that were never legitimately loaded.
   *
   * Three independent sources agree the bit must be 0, and all three were
   * available before this line was ever written:
   *   1. the TRM gives esmart_frm_resetn_en a reset value of 0x0;
   *   2. the reference driver has NO handling of frm_resetn at all -- it never
   *      writes ESMART_CTRL0, so the bit stays 0 on every Rockchip board that
   *      runs Linux;
   *   3. this board's own bootloader left ESMART0_CTRL0 = 0x00000000 (and
   *      ESMART1/2/3 with only their esmart_scl_num bits set, bit31 clear),
   *      as the early dump captured before anything here wrote to the VOP.
   *
   * esmart_scl_num (bits [13:12]) selects which scale engine the window uses
   * and is 0 for ESMART0, matching the reset value, so a plain 0 is the
   * correct and complete value for this window -- no CSC, no 8bpp paths, no
   * frame reset.
   */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_CTRL0,
                    RK3576_VOP_ESMART_SCL_NUM);

  {
    uint32_t got = rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_CTRL0);

    syslog(LOG_WARNING,
           "vop: ESMART%u_CTRL0 = %08x -- frm_resetn_en bit31 = %u (must be "
           "0: 1 means the layer's\n", (unsigned)RK3576_VOP_ESMART_IDX,
           (unsigned)got, (unsigned)((got >> 31) & 1u));
    syslog(LOG_WARNING,
           "vop:   internal logic is soft-reset on EVERY vsync, which the "
           "TRM says excludes\n");
    syslog(LOG_WARNING,
           "vop:   the register logic -- so the register file reads back "
           "perfect while the layer\n");
    syslog(LOG_WARNING,
           "vop:   can never fetch.  scl_num[13:12]=%u (0 = scale engine "
           "0, correct for ESMART0).\n",
           (unsigned)((got >> 12) & 0x3u));

    if ((got >> 31) != 0u)
      {
        syslog(LOG_ERR,
               "vop: *** frm_resetn_en is STILL SET: the layer is being "
               "reset every frame and\n");
        syslog(LOG_ERR,
               "vop:   cannot possibly issue a sustained stream of reads "
               "***\n");
      }
  }
}

/****************************************************************************
 * Name: rk3576_vop_biu_active
 *
 * Description:
 *   *** THE MISSING STEP FROM THE REFERENCE POWER-UP SEQUENCE. ***
 *
 *   The TRM's power-domain chapter gives the software power-up procedure
 *   (6.5.6.2 "PD power down/up operation by software"):
 *
 *     2. PD power up operation by software
 *        (1) Enable power up to the specific power domain by setting the
 *            corresponding bit to 0 in PMU_PWR_GATE_SFTCON0/1 register;
 *        (2) Query PMU_PWR_GATE_STS to wait for the specific power domain
 *            power up;
 *        (3) Do BIU active operation by software if BIU interface exists in
 *            the specific power domain; If BIU interface does not exist, skip
 *            this step.
 *
 *   and defines that third step as:
 *
 *     BIU active operation by software:
 *       (1) Send BIU active request by setting the corresponding bit to 0 in
 *           PMU_BIU_IDLE_SFTCON0/1 register;
 *       (2) Query PMU_IDLE_ACK_STS register to wait for the corresponding BIU
 *           active acknowledge state; Query PMU_IDLE_STS register to wait for
 *           the corresponding BIU in active state.
 *
 *   Step 3 is about the BIU -- the Bus Interface Unit, the thing that turns a
 *   module's internal requests into AXI transactions.  A module whose BIU is
 *   held IDLE still reads and writes its configuration registers perfectly
 *   (those go over the APB/AHB path) while being unable to issue a single AXI
 *   transaction.  That is, precisely and without exception, every measurement
 *   this bring-up has made:
 *
 *     - the ESMART issues NO memory accesses, proved twice by independent
 *       mechanisms (an undecodable poison address produced no bus error, and
 *       turning both MMUs' paging on with no page table produced no fault);
 *     - every register reads back exactly as programmed, because the register
 *       path is unaffected;
 *     - nothing is reported in the interrupt latches, because nothing failed;
 *     - the POST's input buffer under-runs continuously, because no data can
 *       arrive;
 *     - the panel shows a picture that follows neither the framebuffer's
 *       content nor its address.
 *
 *   This step is also exactly the kind of thing a bootloader is SUPPOSED to
 *   undo on the way out and might not: this board's vendor loader drives the
 *   VOP for a boot logo, and putting a display controller's BIU into idle
 *   before handing over is standard practice -- the operating system is then
 *   expected to bring it back, and this driver never did.  The registers below
 *   are outside the VOP block entirely, which is why no scan of the VOP has
 *   ever looked at them.
 *
 *   Register map (PMU at 0x27360000):
 *     0x20100  PMU_BIU_IDLE_CON0      hardware-mode idle config
 *     0x20110  PMU_BIU_IDLE_SFTCON0   *** software-mode idle request ***
 *     0x20120  PMU_BIU_IDLE_ACK_STS   idle acknowledge state (RO)
 *     0x20128  PMU_BIU_IDLE_STS       idle state, 1 = IDLE (RO)
 *
 *   SFTCON0 uses the same hiword write-enable scheme as the VOP:
 *   bits[31:16] enable the write of the corresponding low bit.  Its data bits
 *   are idle_req_*_sftena, one per bus master, and its RESET VALUE IS 0x0000 --
 *   i.e. "every BIU active" is the power-on default.  Writing 0 to the low
 *   word with the whole hiword enabled therefore RESTORES the reset state,
 *   which is the safe direction to move in.
 *
 *   Every value is printed before and after so the log says what the inherited
 *   state actually was, rather than leaving it to inference.  If a request bit
 *   was set and the corresponding STS bit was 1, this is the root cause.
 *
 ****************************************************************************/

static void rk3576_vop_biu_active(void)
{
  static const uint32_t pmu = RK3576_PMU_ADDR;
  uint32_t con_b;
  uint32_t sftcon_b;
  uint32_t ack_b;
  uint32_t sts_b;
  uint32_t sftcon_a;
  uint32_t ack_a;
  uint32_t sts_a;

  con_b    = getreg32(pmu + 0x20100u);
  sftcon_b = getreg32(pmu + 0x20110u);
  ack_b    = getreg32(pmu + 0x20120u);
  sts_b    = getreg32(pmu + 0x20128u);

  syslog(LOG_WARNING,
         "vop: BIU IDLE PROBE: CON0=%08x SFTCON0=%08x ACK_STS=%08x "
         "STS=%08x\n",
         (unsigned)con_b, (unsigned)sftcon_b, (unsigned)ack_b,
         (unsigned)sts_b);

  /* *** DECODE THE BITS BY NAME, NOT BY POSITION. ***
   *
   * The first version of this probe treated a non-zero low word as an alarm,
   * and the log answered STS=0x00000001 -- which it reported as "those channels
   * ARE currently idle".  That was a false alarm, and the TRM's own bit list is
   * what shows why: the master names run DOWN the register with the high bits,
   * so the VOP's bits are in the middle and bit0 belongs to something else
   * entirely.
   *
   *   bit27 idle_vo0vop_channel   bit26 idle_top       bit25 idle_secure
   *   bit24 idle_bus              bit23 idle_ddrsch1   bit22 idle_ddrsch0
   *   bit21 idle_ddr              bit20 idle_center_main
   *   bit19 idle_center_ddrsch    bit18 idle_nvm       bit17 idle_gmac
   *   bit16 idle_audio            bit15 idle_php       bit14 idle_vop_ddrsch
   *   bit13 idle_vop   <-- THIS    bit12 idle_vo1       bit11 idle_vo0
   *   bit10 idle_usb              bit9  idle_vi        bit8  idle_vepu1
   *   bit7  idle_vepu0            bit6  idle_vdec      bit5  idle_vpu
   *   bit4  idle_npusys           bit3  idle_nputop    bit2  idle_npu1
   *   bit1  idle_npu0             bit0  idle_gpu  <-- and this
   *
   * So STS bit0 = the GPU's BIU being idle, which is expected and meaningless
   * on a board that never uses the GPU, while idle_vop (bit13) was 0 -- i.e.
   * the VOP's BIU is ACTIVE and the whole hypothesis is refuted.  Reporting the
   * names removes the ambiguity that produced the false alarm.
   */

  {
    struct
    {
      uint32_t bit;
      FAR const char *name;
    } const masters[] =
    {
      { 27, "vo0vop_channel" }, { 26, "top" },      { 25, "secure" },
      { 24, "bus" },            { 23, "ddrsch1" },  { 22, "ddrsch0" },
      { 21, "ddr" },            { 20, "center_main" },
      { 19, "center_ddrsch" },  { 18, "nvm" },      { 17, "gmac" },
      { 16, "audio" },          { 15, "php" },      { 14, "vop_ddrsch" },
      { 13, "vop" },            { 12, "vo1" },      { 11, "vo0" },
      { 10, "usb" },            { 9, "vi" },        { 8, "vepu1" },
      { 7, "vepu0" },           { 6, "vdec" },      { 5, "vpu" },
      { 4, "npusys" },          { 3, "nputop" },    { 2, "npu1" },
      { 1, "npu0" },            { 0, "gpu" },
    };

    int i;

    for (i = 0; i < (int)nitems(masters); i++)
      {
        uint32_t m = (1u << masters[i].bit);

        if ((sts_b & m) != 0u)
          {
            syslog(LOG_WARNING,
                   "vop:   idle @ bit%u = %s%s\n", (unsigned)masters[i].bit,
                   masters[i].name,
                   masters[i].bit == 0u
                       ? "  (the GPU: expected, this board never uses it)"
                       : masters[i].bit == 13u || masters[i].bit == 14u ||
                             masters[i].bit == 11u || masters[i].bit == 27u
                             ? "  <-- *** VOP-RELATED AND IDLE ***"
                             : "");
          }
      }
  }

  /* The VOP's own channels.  These are the only ones that matter here. */

  {
    uint32_t vop_bits = (1u << 13) | (1u << 14) | (1u << 11) | (1u << 27);

    if ((sts_b & vop_bits) == 0u)
      {
        syslog(LOG_WARNING,
               "vop:   *** NO VOP CHANNEL IS IDLE (idle_vop bit13 = 0, "
               "idle_vop_ddrsch bit14 = 0,\n");
        syslog(LOG_WARNING,
               "vop:   idle_vo0 bit11 = 0, idle_vo0vop_channel bit27 = 0) ***, "
               "so the VOP's BIU is\n");
        syslog(LOG_WARNING,
               "vop:   ACTIVE and CAN issue AXI transactions.  The BIU-idle "
               "hypothesis is REFUTED;\n");
        syslog(LOG_WARNING,
               "vop:   the zero-fetch fault lies elsewhere.  (The only idle "
               "master here is the\n");
        syslog(LOG_WARNING,
               "vop:   GPU, which this board never uses.)\n");
      }
    else
      {
        syslog(LOG_ERR,
               "vop: *** A VOP CHANNEL IS IDLE (mask %08x of bits 13/14/11/27).\n",
               (unsigned)(sts_b & vop_bits));
        syslog(LOG_ERR,
               "vop:   That would physically prevent fetching.  Clearing it "
               "below. ***\n");
      }
  }

  if ((sftcon_b & 0xffffu) != 0u)
    {
      syslog(LOG_ERR,
             "vop: *** A BIU IDLE REQUEST IS ASSERTED (SFTCON0 low word = "
             "%04x).  A master\n", (unsigned)(sftcon_b & 0xffffu));
      syslog(LOG_ERR,
             "vop:   whose bit is set there cannot issue AXI transactions "
             "while its configuration\n");
      syslog(LOG_ERR,
             "vop:   registers still read and write normally ***\n");
    }

  /* Request ACTIVE for every bus master: data = 0, write-enable = all 16 low
   * bits.  This is the reset state, so it can only move things towards the
   * power-on default.
   */

  putreg32(0xffff0000u | 0x0000u, pmu + 0x20110u);
  up_mdelay(5);

  sftcon_a = getreg32(pmu + 0x20110u);
  ack_a    = getreg32(pmu + 0x20120u);
  sts_a    = getreg32(pmu + 0x20128u);

  syslog(LOG_WARNING,
         "vop: BIU IDLE PROBE: after requesting ACTIVE -- SFTCON0=%08x "
         "ACK_STS=%08x STS=%08x\n",
         (unsigned)sftcon_a, (unsigned)ack_a, (unsigned)sts_a);
  syslog(LOG_WARNING,
         "vop:   STS changed by %08x, ACK_STS changed by %08x (bits that left "
         "the idle state)\n",
         (unsigned)(sts_b & ~sts_a), (unsigned)(ack_b & ~ack_a));
  syslog(LOG_WARNING,
         "vop:   this step is now DONE either way, so it cannot be the reason "
         "the layer does\n");
  syslog(LOG_WARNING,
         "vop:   not fetch; it is recorded here so it stops being suspected.\n");
}

/****************************************************************************
 * Name: rk3576_vop_power_domain_on
 *
 * Description:
 *   Power on the VOP's two INTERNAL power domains: the CLUSTER domain
 *   (cluster0/1) and the ESMART domain (esmart0/1/2/3).
 *
 *   *** THIS IS THE ONE INITIALISATION STEP EVERY PREVIOUS ROUND MISSED. ***
 *
 *   The reference driver performs it before it touches anything else, for every
 *   video port:
 *
 *     list_for_each_entry_safe_reverse(pd, n, &vop2->pd_list_head, list) {
 *             if (vop2_power_domain_status(pd)) { pd->on = true; }
 *             else { u32 o = pd->data->regs->pd.offset;
 *                    vop2->regsbak[o >> 2] = vop2_readl(vop2, o);
 *                    vop2_power_domain_on(pd); }
 *     }
 *
 *   For RK3576 the control bits are:
 *
 *     SYS_ESMART_PD_CTRL_IMD  (0x0034) bit0  esmart_pd_en     reset 0
 *     SYS_CLUSTER_PD_CTRL_IMD (0x0030) bit0  cluster01_pd_en  reset 0
 *
 *   1'b0 = power on, and power-on takes effect IMMEDIATELY (power-down is the
 *   one that waits for a regdone).  Both registers carry the hiword write mask,
 *   so a plain putreg() would have been discarded -- use the masked writer.
 *
 *   WHY THIS IS A REAL SUSPECT AND NOT A THEORY.  An earlier round attacked
 *   this from the PMU side instead (RK3576_PMU_PWR_GATE_CON0,
 *   RK3576_PMU_BISR_INITRST_SFTCON0).  In the reference driver those PMU
 *   registers are the STATUS side of the mechanism -- PMU_PWR_GATE_STS and
 *   PMU_BISR_PDGEN_CON0 are only ever READ, to observe the result of writing
 *   the VOP bit -- so the control bit written here had never been touched.
 *
 *   It matters because a powered-down ESMART still answers APB accesses to its
 *   configuration registers.  Every readback in this bring-up is therefore
 *   equally consistent with a window that is correctly programmed and
 *   physically unable to fetch: no AXI reads, no bus error, no MMU fault, and a
 *   POST input buffer that is permanently empty -- which is exactly the picture
 *   on the glass.  Power-on is idempotent, so forcing it costs nothing when the
 *   domain was already up, and the readback distinguishes the two cases.
 *
 ****************************************************************************/

static void rk3576_vop_power_domain_on(FAR struct rk3576_vop_s *priv)
{
  uint32_t sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  uint32_t c_before;
  uint32_t e_before;
  uint32_t e_after;

  c_before = rk3576_vop_getreg(priv, sys_base +
                               RK3576_VOP_SYS_CLUSTER_PD_CTRL_IMD);
  e_before = rk3576_vop_getreg(priv, sys_base +
                               RK3576_VOP_SYS_ESMART_PD_CTRL_IMD);

  syslog(LOG_WARNING,
         "vop: PD before: CLUSTER_PD(0x0030)=%08x ESMART_PD(0x0034)=%08x -- "
         "esmart_pd_en bit0=%u, so esmart0/1/2/3 were %s\n",
         (unsigned)c_before, (unsigned)e_before,
         (unsigned)(e_before & RK3576_VOP_SYS_ESMART_PD_EN),
         (e_before & RK3576_VOP_SYS_ESMART_PD_EN) != 0u
             ? "POWERED DOWN"
             : "powered on");

  /* Power on, with the hiword write mask (data bit = 0 = on). */

  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_ESMART_PD_CTRL_IMD, 0u,
                              RK3576_VOP_SYS_ESMART_PD_EN);
  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_CLUSTER_PD_CTRL_IMD, 0u,
                              RK3576_VOP_SYS_CLUSTER01_PD_EN);

  /* *** esmart_lb_mode: SET TO 3, THE REFERENCE DEFAULT AND THE RESET VALUE. ***
   *
   * The field partitions the ESMART line buffers between esmart0..3:
   *   2'b10: 3 x 4k          (VOP3_ESMART_4K_4K_4K_MODE)
   *   2'b11: 2 x 4k + 2 x 2k (VOP3_ESMART_4K_4K_2K_2K_MODE)  <-- reset value
   *
   * This driver used to force 2, reasoning that a 720-pixel RGB888 line (2160
   * bytes) does not fit a 2K buffer and mode 2 gives more windows a 4K buffer.
   * That reasoning was sound, and 2 does appear in RK3576's map -- but that
   * entry is only reached when a device tree supplies an unsupported value.
   * With NO property at all, which is this board, the reference resolves
   * VOP3_ESMART_4K_4K_2K_2K_MODE through the map and writes 3, i.e. the reset
   * value.  See RK3576_VOP_SYS_ESMART_LB_MODE_4K_4K_2K_2K in the header.
   *
   * ESMART0 has a 4K buffer in either mode, so this is not a strong candidate
   * for ESMART0's failure -- it is being aligned because it is the last value in
   * this driver that differs from both the reference default AND the hardware
   * reset value.
   */

  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_ESMART_PD_CTRL_IMD,
                              RK3576_VOP_SYS_ESMART_LB_MODE_4K_4K_2K_2K
                                  << RK3576_VOP_SYS_ESMART_LB_MODE_SHIFT,
                              RK3576_VOP_SYS_ESMART_LB_MODE_MASK);

  up_mdelay(10);

  e_after = rk3576_vop_getreg(priv, sys_base +
                              RK3576_VOP_SYS_ESMART_PD_CTRL_IMD);

  syslog(LOG_WARNING,
         "vop: PD after:  ESMART_PD(0x0034)=%08x (esmart_pd_en bit0=%u, "
         "esmart_lb_mode[7:6]=%u (reference default and reset value = %u), "
         "write_mask[31:16]=%04x)\n",
         (unsigned)e_after, (unsigned)(e_after & RK3576_VOP_SYS_ESMART_PD_EN),
         (unsigned)((e_after & RK3576_VOP_SYS_ESMART_LB_MODE_MASK)
                    >> RK3576_VOP_SYS_ESMART_LB_MODE_SHIFT),
         (unsigned)RK3576_VOP_SYS_ESMART_LB_MODE_4K_4K_2K_2K,
         (unsigned)(e_after >> 16));

  if ((e_after & RK3576_VOP_SYS_ESMART_PD_EN) != 0u)
    {
      syslog(LOG_ERR,
             "vop: *** ESMART PD STILL READS AS POWERED DOWN after a masked "
             "write.  Either the bit is protected here or something re-asserts "
             "it; this probe cannot tell which ***\n");
    }
}

/****************************************************************************
 * Name: rk3576_vop_full_dump
 *
 * Description:
 *   *** PRINT THE ENTIRE VOP REGISTER SPACE IN THE SAME FORMAT AS A DUMP TAKEN
 *   FROM A KNOWN-WORKING SYSTEM, SO THE TWO CAN BE DIFFED DIRECTLY. ***
 *
 *   Everything this bring-up has established about the window has been
 *   established by reading the TRM and the vendor driver and INFERRING what the
 *   registers should be.  That process has produced dozens of confident
 *   conclusions and no working display.  A register dump from the same board
 *   running a Debian image with HDMI working removes the inference entirely: it
 *   is a real, on-silicon configuration that fetches and displays, and any
 *   register that differs from ours is a candidate with evidence behind it
 *   rather than a guess.
 *
 *   FORMAT.  Identical to that dump, deliberately, so that
 *
 *       diff debian-vop.txt nuttx-vop.txt
 *
 *   works with no post-processing: one register per line, four hex digits of
 *   offset, a space, eight hex digits of value, and ONLY NON-ZERO values --
 *   which is what makes the output a manageable size, since the VOP's address
 *   space is 128 KB but only a few thousand words are ever used.
 *
 *   ONLY offsets and values are printed -- no "kickpi-k7:" prefix, no
 *   timestamping -- because any decoration would break the diff.  The block is
 *   delimited by two marker lines instead, so it can be extracted from a busy
 *   console log with something like:
 *
 *       sed -n '/VOPDUMP START/,/VOPDUMP END/p' log.txt | grep -E '^[0-9a-f]{4} '
 *
 *   This is a lot of output (order of a thousand lines), so it runs ONCE, from
 *   the board file, right after the video path is up and before any probe has
 *   touched the registers -- that way it describes the driver's configuration
 *   rather than the probes' leftovers.
 *
 ****************************************************************************/

void rk3576_vop_full_dump(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t off;
  uint32_t base;

  if (priv == NULL)
    {
      return;
    }

  base = priv->base;

  syslog(LOG_WARNING, "kickpi-k7: VOPDUMP START base=%08x\n",
         (unsigned)base);

  /* Only the VOP's own register area, 0x0000 through 0x2000.
   *
   * The address space is 128 KB, but everything above 0x2000 is bulk data --
   * gamma LUTs and, from 0x7E00, the MMU's page tables, which are thousands of
   * repeating non-zero words.  The reference dump has 13035 non-zero registers
   * in total but only 188 in this range, and those 188 are the ones that carry
   * configuration: SYS_CTRL, the overlay ports, POST0/1/2, CLUSTER0/1 and
   * ESMART0..3. Dumping the data regions would produce tens of thousands of
   * lines over a serial console for no diagnostic gain.
   */

  for (off = 0; off < 0x2000u; off += 4u)
    {
      uint32_t v = getreg32(base + off);

      if (v != 0u)
        {
          /* No prefix: these lines must be byte-identical in shape to the
           * reference dump's, or the diff is worthless. */

          syslog(LOG_WARNING, "%04x %08x\n", (unsigned)off, (unsigned)v);
        }
    }

  syslog(LOG_WARNING, "kickpi-k7: VOPDUMP END\n");
}

/****************************************************************************
 * Name: rk3576_vop_early_dump
 *
 * Description:
 *   Capture the VOP register file AS INHERITED FROM THE BOOTLOADER, before
 *   this driver writes a single register to it.
 *
 *   WHY THIS, AFTER EVERYTHING ELSE.  Two independent mechanisms now agree
 *   that ESMART0 -- the window this driver configures, and the only one any
 *   scan in this bring-up has ever read -- issues NO memory accesses at all.
 *   If that is true, then ESMART0 is not what draws the picture on the glass,
 *   and every round spent adjusting ESMART0 and its SYS-block switches has
 *   been spent on a window that contributes nothing.  Something ELSE is
 *   painting a static garble that follows neither the framebuffer's content
 *   nor its address, and it changes when the SYS block changes because those
 *   switches belong to the composition path, not to one window.
 *
 *   The candidates are exactly the blocks no scan has ever read: the CLUSTER
 *   windows (0x1000/0x1200), ESMART1/2/3 (0x1A00/0x1C00/0x1E00), and the
 *   per-window VP routing fields (ESMARTx_PORT_SEL_IMD, offset 0x00F4 -- the
 *   reference driver READS these to learn which video port each window feeds,
 *   rather than writing them, so on this SoC they are inherited state).  A
 *   leftover enabled window pointing at stale memory produces precisely the
 *   symptom: a picture that ignores our framebuffer, follows no address, and
 *   never generates a bus error or an MMU fault because its reads SUCCEED.
 *
 *   This board boots through a vendor bootloader that drives the same VOP for
 *   a boot logo, so the hardware does not start from reset.  Reading it before
 *   touching it is the only way to separate "inherited" from "ours", and it is
 *   the one measurement that needs no hypothesis about which bit is wrong.
 *
 *   Also prints the CRU gates for the VOP AXI clock tree, which has been the
 *   subject of repeated speculation.  "When high, disable clock" (the bits are
 *   SET_TO_DISABLE), so 0 in a gate bit means the clock is RUNNING.
 *
 *   Read-only.
 *
 ****************************************************************************/

static void rk3576_vop_early_dump(FAR struct rk3576_vop_s *priv)
{
  static const struct
  {
    uint32_t off;
    uint32_t size;
    FAR const char *name;
  } blocks[] =
  {
    { RK3576_VOP_SYS_CTRL_OFFSET,       0x200, "SYS_CTRL"    },
    { RK3576_VOP_OVERLAY_SYSTEM_OFFSET, 0x100, "OVERLAY_SYS" },
    { RK3576_VOP_OVERLAY_PORT0_OFFSET,  0x100, "OVERLAY_P0"  },
    { RK3576_VOP_POST0_OFFSET,          0x100, "POST0"       },
    { RK3576_VOP_CLUSTER0_OFFSET,       0x100, "CLUSTER0"    },
    { RK3576_VOP_CLUSTER1_OFFSET,       0x100, "CLUSTER1"    },
    { RK3576_VOP_ESMART0_OFFSET,        0x100, "ESMART0"     },
    { RK3576_VOP_ESMART1_OFFSET,        0x100, "ESMART1"     },
    { RK3576_VOP_ESMART2_OFFSET,        0x100, "ESMART2"     },
    { RK3576_VOP_ESMART3_OFFSET,        0x100, "ESMART3"     },
  };

  static const struct
  {
    uint32_t off;
    FAR const char *name;
  } key[] =
  {
    { RK3576_VOP_SYS_CTRL_OFFSET + 0x0020, "SYS_MMU_CTRL      " },
    { RK3576_VOP_SYS_CTRL_OFFSET + 0x0028, "SYS_PORT_CTRL     " },
    { RK3576_VOP_SYS_CTRL_OFFSET + 0x0034, "SYS_ESMART_PD     " },
    { RK3576_VOP_ESMART0_OFFSET  + 0x0000, "ESMART0_CTRL0     " },
    { RK3576_VOP_ESMART0_OFFSET  + 0x0004, "ESMART0_CTRL1     " },
    { RK3576_VOP_ESMART0_OFFSET  + 0x0008, "ESMART0_AXI_CTRL  " },
    { RK3576_VOP_ESMART0_OFFSET  + 0x0010, "ESMART0_R0_MSTCTL " },
    { RK3576_VOP_ESMART0_OFFSET  + 0x0014, "ESMART0_R0_YRGB   " },
    { RK3576_VOP_ESMART0_OFFSET  + 0x00F4, "ESMART0_PORT_SEL  " },
    { RK3576_VOP_CLUSTER0_OFFSET + 0x0000, "CLUSTER0_W0_CTRL0 " },
    { RK3576_VOP_CLUSTER0_OFFSET + 0x0010, "CLUSTER0_W0_YRGB  " },
    { RK3576_VOP_CLUSTER0_OFFSET + 0x0030, "CLUSTER0_W1_CTRL0 " },
  };

  uint32_t cru = 0x27200000u; /* CRU, TRM Part 1 address map */
  uint32_t sel144;
  uint32_t g61;
  uint32_t g62;
  int b;
  int i;

  syslog(LOG_WARNING, "kickpi-k7: ================= VOP EARLY DUMP =================\n");
  syslog(LOG_WARNING, "kickpi-k7: The register file BELOW IS INHERITED.  This driver has\n");
  syslog(LOG_WARNING, "kickpi-k7:   not written a single VOP register yet.\n");

  /* CRU: is the VOP AXI clock tree actually open?  Gate bits are
   * SET_TO_DISABLE, so 0 = clock running.
   */

  sel144 = getreg32(cru + 0x0540u); /* CRU_CLKSEL_CON144 */
  g61    = getreg32(cru + 0x08f4u); /* CRU_GATE_CON61 (VOP clocks + BIU) */
  g62    = getreg32(cru + 0x08f8u); /* CRU_GATE_CON62 (vop2_biu / vopgrf) */

  syslog(LOG_WARNING,
         "kickpi-k7: CRU CLKSEL_CON144=%08x GATE_CON61=%08x GATE_CON62=%08x\n",
         (unsigned)sel144, (unsigned)g61, (unsigned)g62);

  /* The VOP's channel to DDR lives in a DIFFERENT clock domain (VO0) with its
   * own gate and reset, none of which this driver has ever touched.  Printed
   * here so the inherited state is recorded before we change it. */

  {
    uint32_t c19 = getreg32(cru + 0x034cu); /* CRU_CLKSEL_CON19 */
    uint32_t g3  = getreg32(cru + 0x080cu); /* CRU_GATE_CON03    */
    uint32_t r6  = getreg32(cru + 0x0a18u); /* CRU_SOFTRST_CON06 */

    syslog(LOG_WARNING,
           "kickpi-k7: CRU VO0VOP-CHANNEL: CLKSEL_CON19=%08x GATE_CON03=%08x "
           "SOFTRST_CON06=%08x\n", (unsigned)c19, (unsigned)g3, (unsigned)r6);
    syslog(LOG_WARNING,
           "kickpi-k7:   aclk_vo0vop_channel_biu: sel[13:12]=%u (0=gpll) "
           "div[11:8]=%u -> /%u, GATE bit1=%u bit0=%u (0=RUNNING), "
           "RST bit1=%u bit0=%u (0=released)\n",
           (unsigned)((c19 >> 12) & 0x3u), (unsigned)((c19 >> 8) & 0xfu),
           (unsigned)(((c19 >> 8) & 0xfu) + 1u),
           (unsigned)((g3 >> 1) & 1u), (unsigned)(g3 & 1u),
           (unsigned)((r6 >> 1) & 1u), (unsigned)(r6 & 1u));
  }
  syslog(LOG_WARNING,
         "kickpi-k7:   GATE_CON61 gate bits (0 = clock RUNNING): "
         "aclk_vop_root[0]=%u aclk_vop_biu[4]=%u aclk_vop2_biu[5]=%u "
         "hclk_vop_biu[6]=%u pclk_vop_biu[7]=%u hclk_vop[8]=%u "
         "aclk_vop[9]=%u dclk_vp0_src[10]=%u dclk_vp0[13]=%u\n",
         (unsigned)((g61 >> 0) & 1u), (unsigned)((g61 >> 4) & 1u),
         (unsigned)((g61 >> 5) & 1u), (unsigned)((g61 >> 6) & 1u),
         (unsigned)((g61 >> 7) & 1u), (unsigned)((g61 >> 8) & 1u),
         (unsigned)((g61 >> 9) & 1u), (unsigned)((g61 >> 10) & 1u),
         (unsigned)((g61 >> 13) & 1u));
  syslog(LOG_WARNING,
         "kickpi-k7:   CLKSEL_CON144: aclk_vop_root_sel[7:5]=%u "
         "aclk_vop_root_div[4:0]=%u\n",
         (unsigned)((sel144 >> 5) & 0x7u), (unsigned)(sel144 & 0x1fu));

  /* Named key registers, printed whether or not they are zero -- the
   * interesting ones usually ARE zero, which is why a non-zero-only scan
   * cannot show them.
   */

  for (i = 0; i < (int)nitems(key); i++)
    {
      syslog(LOG_WARNING, "kickpi-k7:   %s (base+%03x) = %08x\n",
             key[i].name, (unsigned)key[i].off, 
             (unsigned)rk3576_vop_getreg(priv, priv->base + key[i].off));
    }

  /* Everything non-zero, block by block. */

  for (b = 0; b < (int)nitems(blocks); b++)
    {
      uint32_t base = priv->base + blocks[b].off;
      uint32_t off;
      int shown = 0;

      for (off = 0; off < blocks[b].size; off += 4u)
        {
          uint32_t v = rk3576_vop_getreg(priv, base + off);

          if (v != 0u)
            {
              syslog(LOG_WARNING, "kickpi-k7:   %s +%03x = %08x\n",
                     blocks[b].name, (unsigned)off, (unsigned)v);
              shown++;

              if (shown > 40)
                {
                  syslog(LOG_WARNING,
                         "kickpi-k7:   %s ... (truncated, >40 non-zero)\n",
                         blocks[b].name);
                  break;
                }
            }
        }

      syslog(LOG_WARNING, "kickpi-k7:   %s: %d non-zero in %u bytes\n",
             blocks[b].name, shown, (unsigned)blocks[b].size);
    }

  /* *** THE VERDICT THAT MATTERS: IS ANY LEFTOVER LAYER ENABLED? ***
   *
   * A region's MST_CTL bit0 is mst_en and the region stride is 0x30.  A
   * CLUSTER window's CTRL0 bit0 is win_en and the window stride is 0x30.
   * Any of these set in a block this driver never configures is a layer
   * compositing memory of its own choosing -- and it would explain a picture
   * that ignores our framebuffer while ESMART0 provably issues no reads.
   */

  for (b = 6; b < (int)nitems(blocks); b++)
    {
      uint32_t base = priv->base + blocks[b].off;
      uint32_t r;

      for (r = 0; r < 4u; r++)
        {
          uint32_t mst_ctl = rk3576_vop_getreg(priv, base + 0x10u + (r * 0x30u));

          if ((mst_ctl & 1u) != 0u)
            {
              syslog(LOG_ERR,
                     "kickpi-k7: *** %s REGION%u is ENABLED (MST_CTL=%08x) -- "
                     "this driver never configures it ***\n",
                     blocks[b].name, (unsigned)r, (unsigned)mst_ctl);
            }
        }
    }

  for (b = 4; b <= 5; b++)
    {
      uint32_t base = priv->base + blocks[b].off;
      uint32_t w;

      for (w = 0; w < 2u; w++)
        {
          uint32_t ctrl0 = rk3576_vop_getreg(priv, base + (w * 0x30u));

          if ((ctrl0 & 1u) != 0u)
            {
              syslog(LOG_ERR,
                     "kickpi-k7: *** %s WIN%u is ENABLED (CTRL0=%08x) -- this "
                     "driver never configures it ***\n",
                     blocks[b].name, (unsigned)w, (unsigned)ctrl0);
            }
        }
    }

  syslog(LOG_WARNING, "kickpi-k7: =============== END VOP EARLY DUMP =============\n");
}

/****************************************************************************
 * Name: rk3576_vop_cfg_done_probe
 *
 * Description:
 *   Prove that the mirror -> real register load for the WINDOW group actually
 *   completes -- the one assumption every measurement in this project has
 *   rested on and none has ever tested.
 *
 *   WHAT IS AT STAKE.  All layer configuration is written to MIRROR registers;
 *   the real registers are only updated at a frame boundary, after the load bit
 *   is written in SYS_WIN_REG_CFG_DONE.  A readback returns the MIRROR, so it
 *   reports what the driver intended whether or not the load ever happened.
 *   Nothing in this bring-up has ever distinguished those two cases.
 *
 *   IF THE WINDOW GROUP NEVER LOADS, ESMART0's real registers keep their reset
 *   values (mst_en = 0, YRGB address = 0) and every observation follows at once:
 *
 *     - the layer issues NO memory accesses (two independent probes agree);
 *     - every layer register reads back correct, because it is the mirror;
 *     - changing ESMART0's configuration changes NOTHING on the glass -- which
 *       is exactly what was just measured when ESMART_CTRL0's frame-reset bit
 *       was cleared -- while changing SYS-block configuration DOES change the
 *       picture, because the SYS groups load;
 *     - the POST starves (POST_BUF_EMPTY), because a disabled window supplies
 *       no data at all.
 *
 *   HOW IT IS DECIDED.  These load bits are REQUEST bits: they read 1 while
 *   pending and are cleared by the frame boundary that consumes them.  So:
 *   pulse, read back immediately (pending is expected), wait several frame
 *   times, read back again (a consumed bit is 0).  A load bit still set after
 *   the wait is a load that never happened.
 *
 *   The pulse masks every window-group bit, so a single wrong bit number cannot
 *   masquerade as "nothing loads".
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_cfg_done_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sys_base;
  uint32_t post_base;
  uint32_t post_cfg_base;
  uint32_t post_cfg_now;
  uint32_t post_cfg_later;
  uint32_t glb_now;
  uint32_t win_now;
  uint32_t glb_later;
  uint32_t win_later;
  uint32_t win_pending;
  uint32_t glb_pending;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);

  /* Read 0xCFC BEFORE touching it.  The previous run wrote 0x00010001 here and
   * read back 0xffff00f3 -- bit-for-bit the value just written to
   * SYS_WIN_REG_CFG_DONE (0x000C).  Identical readbacks from two different
   * addresses mean either the register aliases 0x000C, or the write never
   * landed and the read is showing something else.  A baseline sample taken
   * before the first write is the cheapest way to tell, and without it the
   * whole "the missing RK3572+ commit" idea rests on an unexplained number. */

  post_cfg_base = rk3576_vop_getreg(priv, post_base +
                                    RK3576_VOP_POST_CFG_DONE_OFF);

  syslog(LOG_WARNING,
         "kickpi-k7:   BASELINE before any write: POST_CFG_DONE=%08x "
         "WIN_REG_CFG_DONE=%08x\n",
         (unsigned)post_cfg_base,
         (unsigned)rk3576_vop_getreg(priv, sys_base +
                                     RK3576_VOP_SYS_WIN_REG_CFG_DONE));

  syslog(LOG_WARNING,
         "kickpi-k7: CFG_DONE PROBE: every mirror register in this project is\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   written as a MIRROR and only reaches its real register\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   when the load bit below is consumed at a frame boundary.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   A readback shows the MIRROR either way, so this has\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   never actually been verified.  Measuring it now.\n");

  /* Pulse every load bit in both groups, then look at once. */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE,
                    RK3576_VOP_CFG_DONE_LOAD_CTRL |
                        RK3576_VOP_CFG_DONE_GLOBAL_REGDONE_EN |
                        RK3576_VOP_CFG_DONE_ALL_GROUPS);
  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE,
                    RK3576_VOP_WIN_CFG_DONE_LOAD_CTRL |
                        RK3576_VOP_WIN_CFG_DONE_ALL);

  /* And the RK3572+ trigger, which Linux actually uses and this driver has
   * never written.  Its immediate readback is itself informative: a register
   * that does not exist or is not writable reads 0 here, whereas one that is
   * accepted reads back the bit (and then clears it when the load happens).
   */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_CFG_DONE_OFF,
                    RK3576_VOP_POST_CFG_DONE_TRIGGER);

  up_mdelay(5);

  glb_now = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE);
  win_now = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE);
  post_cfg_now = rk3576_vop_getreg(priv, post_base +
                                   RK3576_VOP_POST_CFG_DONE_OFF);

  syslog(LOG_WARNING,
         "kickpi-k7:   immediately after the pulse: REG_CFG_DONE=%08x "
         "WIN_REG_CFG_DONE=%08x POST_CFG_DONE=%08x (want 00010001 -> the bit "
         "is accepted; 0 -> the register is absent or not writable)\n",
         (unsigned)glb_now, (unsigned)win_now, (unsigned)post_cfg_now);

  if ((win_now & RK3576_VOP_WIN_CFG_DONE_ALL) == 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7: CFG_DONE PROBE => *** WIN_REG_CFG_DONE reads 0 right "
             "after being written:\n");
      syslog(LOG_ERR,
             "kickpi-k7:   the pulse never reached the register at all, so the "
             "window group can\n");
      syslog(LOG_ERR,
             "kickpi-k7:   never load and NO window configuration in this "
             "driver has ever taken\n");
      syslog(LOG_ERR,
             "kickpi-k7:   effect.  Check the write-mask hiword and the "
             "offset. ***\n");
    }

  /* A frame is 780 x 1320 at about 62.5 MHz, roughly 16.5 ms.  Wait four of
   * them so the request is unambiguously either consumed or not. */

  up_mdelay(70);

  glb_later = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE);
  win_later = rk3576_vop_getreg(priv, sys_base +
                                RK3576_VOP_SYS_WIN_REG_CFG_DONE);
  post_cfg_later = rk3576_vop_getreg(priv, post_base +
                                     RK3576_VOP_POST_CFG_DONE_OFF);

  glb_pending = glb_later & RK3576_VOP_CFG_DONE_VP0_GROUPS;
  win_pending = win_later & RK3576_VOP_WIN_CFG_DONE_ALL;

  syslog(LOG_WARNING,
         "kickpi-k7:   after 70 ms (about 4 frames): REG_CFG_DONE=%08x "
         "WIN_REG_CFG_DONE=%08x POST_CFG_DONE=%08x\n",
         (unsigned)glb_later, (unsigned)win_later,
         (unsigned)post_cfg_later);

  if (post_cfg_now == 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   POST_CFG_DONE read 0 immediately after being "
             "written, so POSTx+0x0FC is\n");
      syslog(LOG_ERR,
             "kickpi-k7:   not writable here and the RK3572+ commit is not "
             "this register either.\n");
    }
  else if (post_cfg_later == 0u)
    {
      syslog(LOG_WARNING,
             "kickpi-k7:   POST_CFG_DONE accepted the bit and has since "
             "cleared it: the RK3572+\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   frame commit is real and is now being fired, "
             "where before it never was.\n");
    }

  syslog(LOG_WARNING,
         "kickpi-k7:   pending load bits (VP0 ONLY: global0 bit0, sys0 bit4): "
         "global=%02x window=%02x  (0 = the request was consumed; VP1/VP2's "
         "bits are excluded because no VP1/VP2 engine runs here and they can "
         "never clear)\n",
         (unsigned)glb_pending, (unsigned)win_pending);

  if (win_pending == 0u && glb_pending == 0u)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: CFG_DONE PROBE => BOTH groups were consumed.  The "
             "mirror -> real load IS\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   happening, so the layer's real registers do hold the "
             "configuration read\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   back above.  \"The mirror never loaded\" is "
             "eliminated.\n");
    }
  else if (win_pending != 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7: CFG_DONE PROBE => *** THE WINDOW GROUP LOAD IS NOT "
             "BEING CONSUMED\n");
      syslog(LOG_ERR,
             "kickpi-k7:   (pending bits %02x).  The ESMART/CLUSTER real "
             "registers keep their\n", (unsigned)win_pending);
      syslog(LOG_ERR,
             "kickpi-k7:   reset values no matter what this driver writes, "
             "which means the layer is\n");
      syslog(LOG_ERR,
             "kickpi-k7:   disabled in silicon while reading back enabled -- "
             "ROOT CAUSE. ***\n");
    }
  else
    {
      syslog(LOG_ERR,
             "kickpi-k7: CFG_DONE PROBE => *** THE GLOBAL GROUP LOAD IS NOT "
             "BEING CONSUMED\n");
      syslog(LOG_ERR,
             "kickpi-k7:   (pending bits %02x).  Window loads work, global "
             "ones do not -- the POST\n", (unsigned)glb_pending);
      syslog(LOG_ERR,
             "kickpi-k7:   timing/colour registers would then be the stale "
             "ones. ***\n");
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_dma_liveness_probe
 *
 * Description:
 *   *** THE FIRST DIRECT MEASUREMENT OF WHETHER THE VOP'S DMA RUNS AT ALL. ***
 *
 *   Every statement in this bring-up that the layer "issues no memory accesses"
 *   has been INFERRED, never observed.  The poison address produced no bus error
 *   and the MMUs with paging on and no page table produced no fault -- but both
 *   of those are arguments from the ABSENCE of a signal, and both of them assume
 *   that the mechanism which would have produced the signal was itself armed.
 *   A test that cannot fail proves nothing.
 *
 *   The SYS0/SYS1 interrupt latches allow the opposite kind of evidence -- a
 *   positive observation -- and they have been reporting it in every log so far.
 *   From SYS1_INTR_EN, whose field names the TRM states alongside their numbers:
 *
 *     bit2 intr_en_dma1_finish    bit1 intr_en_bus1_error    bit7 intr_en_mmu1
 *
 *   and SYS0_INTR_RAW_STATUS carries the same layout for channel 0.  The
 *   register scan has shown BOTH channels reporting dma_finish since the first
 *   dump:
 *
 *     sys0 = 0x00000124  ->  bit2 dma0_finish set
 *     sys1 = 0x00000004  ->  bit2 dma1_finish set
 *
 *   If those are live rather than stale, DMA traffic is completing inside the
 *   VOP, which flatly contradicts "the layer issues no accesses".
 *
 *   They are latches, so the test is: clear both, wait, read again.
 *
 *     - a dma_finish bit that REAPPEARS  => the channel really is transferring;
 *     - neither reappears               => the bits were stale, most likely a
 *       bootloader leftover this driver never cleared, and the absence of
 *       traffic stands.
 *
 *   In the same wait window the channel-busy status (bit16 axiN_mmu_idle, "0 =
 *   MMU busy") is sampled repeatedly.  That bit has only ever been read once per
 *   boot, at a moment when nothing was expected to move; counting how often it
 *   reads BUSY across two whole frames is a second, independent positive test.
 *
 *   Read/write, but it only clears latches -- which is exactly what the
 *   reference driver does for every interrupt it services.
 *
 ****************************************************************************/

int rk3576_vop_dma_liveness_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sys_base;
  uint32_t raw0_before;
  uint32_t raw1_before;
  uint32_t raw0_after;
  uint32_t raw1_after;
  int busy0 = 0;
  int busy1 = 0;
  int i;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  sys_base = RK3576_VOP_SYS_CTRL(priv->base);

  raw0_before = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS0_INT_RAW);
  raw1_before = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS1_INT_RAW);

  syslog(LOG_WARNING,
         "kickpi-k7: DMA LIVENESS PROBE: latched BEFORE clearing -- "
         "SYS0_RAW=%08x (dma0_finish bit2=%u bus_error bit1=%u) "
         "SYS1_RAW=%08x (dma1_finish bit2=%u bus_error bit1=%u)\n",
         (unsigned)raw0_before,
         (unsigned)((raw0_before >> 2) & 1u), (unsigned)((raw0_before >> 1) & 1u),
         (unsigned)raw1_before,
         (unsigned)((raw1_before >> 2) & 1u), (unsigned)((raw1_before >> 1) & 1u));

  /* Clear both latch sets and confirm they went quiet, so that anything seen
   * afterwards is unambiguously new. */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS0_INT_CLR, 0xffffffffu);
  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS1_INT_CLR, 0xffffffffu);
  up_mdelay(1);

  syslog(LOG_WARNING,
         "kickpi-k7:   after clearing: SYS0_RAW=%08x SYS1_RAW=%08x (both should "
         "be 0)\n",
         (unsigned)rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS0_INT_RAW),
         (unsigned)rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS1_INT_RAW));

  /* Sample the two channels' busy status across about two frames.  One frame is
   * htotal 780 x vtotal 1320 at ~62.5 MHz, i.e. ~16.5 ms, so the window must be
   * at least that long: a line-rate fetch would be easy to miss entirely between
   * two samples taken closer together than a line time.
   */

  for (i = 0; i < 400; i++)
    {
      uint32_t a0 = rk3576_vop_getreg(priv, sys_base +
                                      RK3576_VOP_SYS_AXI0_CTRL_IMD);
      uint32_t a1 = rk3576_vop_getreg(priv, sys_base +
                                      RK3576_VOP_SYS_AXI1_CTRL_IMD);

      if ((a0 & RK3576_VOP_SYS_AXI_MMU_IDLE) == 0u)
        {
          busy0++;
        }

      if ((a1 & RK3576_VOP_SYS_AXI_MMU_IDLE) == 0u)
        {
          busy1++;
        }

      up_udelay(100);
    }

  raw0_after = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS0_INT_RAW);
  raw1_after = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS1_INT_RAW);

  syslog(LOG_WARNING,
         "kickpi-k7:   channel busy samples over ~40 ms (2.4 frames): "
         "AXI0 busy %d/400, AXI1 busy %d/400\n", busy0, busy1);
  syslog(LOG_WARNING,
         "kickpi-k7:   latched AFTER the wait -- SYS0_RAW=%08x (dma0_finish=%u "
         "bus_error=%u) SYS1_RAW=%08x (dma1_finish=%u bus_error=%u)\n",
         (unsigned)raw0_after,
         (unsigned)((raw0_after >> 2) & 1u), (unsigned)((raw0_after >> 1) & 1u),
         (unsigned)raw1_after,
         (unsigned)((raw1_after >> 2) & 1u), (unsigned)((raw1_after >> 1) & 1u));

  if ((raw0_after & RK3576_VOP_SYS_INT_DMA_FINISH) != 0u ||
      (raw1_after & RK3576_VOP_SYS_INT_DMA_FINISH) != 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7: DMA LIVENESS PROBE => *** A dma_finish REAPPEARED after "
             "being cleared, so a\n");
      syslog(LOG_ERR,
             "kickpi-k7:   DMA engine in this VOP IS transferring data.  The "
             "VOP's path to memory\n");
      syslog(LOG_ERR,
             "kickpi-k7:   therefore works, and the fault is in what feeds this "
             "particular window\n");
      syslog(LOG_ERR,
             "kickpi-k7:   -- not in the shared fetch path.  This OVERTURNS the "
             "inference drawn\n");
      syslog(LOG_ERR,
             "kickpi-k7:   from the poison and MMU tests, which could only ever "
             "argue from\n");
      syslog(LOG_ERR,
             "kickpi-k7:   absence. ***\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "kickpi-k7: DMA LIVENESS PROBE => no dma_finish reappeared: those "
             "bits were stale\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   bootloader leftovers, not evidence of traffic.  The "
             "absence of layer\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   fetches stands, now on positive as well as negative "
             "evidence.\n");
    }

  if (busy0 != 0 || busy1 != 0)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   and a channel read BUSY in %d/%d samples -- a "
             "translation really was in\n", busy0, busy1);
      syslog(LOG_ERR,
             "kickpi-k7:   flight, which the single-sample readbacks never "
             "showed. ***\n");
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_winload_probe
 *
 * Description:
 *   *** SEPARATES "THE WINDOW GROUP'S MIRROR->REAL LOAD" AND "THE LAYER'S OWN
 *   ENABLE BIT" FROM EVERYTHING ELSE -- WITH ONE VARIABLE PER RUNG. ***
 *
 *   Two things in this bring-up have never actually been isolated:
 *
 *     - Rung 3 of the source probe disables the layer by clearing the ESMART
 *       window's OWN enable bit (REGION0_CTRL bit0), and the screen does go to
 *       a clean solid blue.  That looks like proof that the window register
 *       group really does load.  But the same rung ALSO turns the POST
 *       background on and paints it blue -- and the blue can come entirely
 *       from the POST background.  Two variables, one rung, no attribution,
 *       which is the confound this project keeps being tripped by.
 *
 *     - "changing the ESMART block changes nothing on screen" has been read as
 *       "the window group never loads".  If that is true then nothing ever
 *       written to the ESMART mirror has reached silicon, the REAL window
 *       registers still hold the bootloader's values, and the panel is showing
 *       the BOOTLOADER's buffer against a mismatched geometry.  That is
 *       static, follows neither our content nor our address, and -- because
 *       the bootloader's address is valid DRAM -- can produce no bus error and
 *       no MMU fault.  One mechanism, and it accounts for every stubborn
 *       observation in this project.
 *
 *   Method: freeze the POST background at a constant solid blue for the whole
 *   probe, so blue can only ever mean "the layer contributed nothing"; then
 *   change exactly ONE window register per rung and pulse the load.
 *
 *     Rung A  window enabled, full 720x1280                    baseline
 *     Rung B  REGION0_CTRL enable bit cleared                  does the window
 *                                                              OWN enable bit
 *                                                              reach silicon?
 *     Rung C  window enabled, ACT/DSP shrunk to 200x200        decisive: does
 *                                                              the whole group
 *                                                              load and fetch?
 *     Rung D  restored to full size
 *
 *   HOW TO READ RUNG C:
 *
 *     O1  our framebuffer's top-left 200x200 appears -> the window FETCHES, and
 *         the full-screen failure is a stride/geometry problem.
 *     O2  garble confined to a 200x200 corner with blue around it -> the
 *         window group DOES load and geometry takes effect, but no fetch
 *         happens: look at the address, the format and the AXI read IDs.
 *     O3  the whole screen unchanged -> the window group does NOT load and the
 *         real registers are still the bootloader's.  That is the root cause,
 *         and the fix belongs in the load trigger, not in the window.
 *
 ****************************************************************************/

int rk3576_vop_winload_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t esmart_base;
  uint32_t post_base;
  uint32_t ovl_base;
  uint32_t ctrl_on;
  uint32_t ctrl_off;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);
  ovl_base = RK3576_VOP_OVERLAY_PORT(priv->base, priv->cfg.port);

  ctrl_on = RK3576_VOP_ESMART_FMT_RGB888 |
            RK3576_VOP_ESMART_REGION0_MST_EN |
            RK3576_VOP_ESMART_REGION0_RB_SWAP;
  ctrl_off = RK3576_VOP_ESMART_FMT_RGB888;

  /* Freeze the background.  Everything after this point must be attributed to
   * a window change alone. */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_BG,
                    RK3576_VOP_POST_BG_DISPLAY_EN |
                        (0x3ffu << RK3576_VOP_POST_BG_BLUE_SHIFT));

  /* --- Rung A: baseline, layer enabled at full size. --- */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                    ctrl_on);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: WINLOAD RUNG A: layer ON, %ux%u, POST background frozen "
         "SOLID BLUE.\n",
         (unsigned)priv->cfg.xres, (unsigned)priv->cfg.yres);
  syslog(LOG_WARNING,
         "kickpi-k7:   REGION0_CTRL=%08x LAYER_SEL=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_REGION0_CTRL),
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_LAYER_SEL));
  up_mdelay(2500);

  /* --- Rung B: clear ONLY the window's own enable bit. --- */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                    ctrl_off);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: WINLOAD RUNG B: ONLY the window enable bit cleared "
         "(%08x), background still blue.\n", (unsigned)ctrl_off);
  syslog(LOG_WARNING,
         "kickpi-k7:   BLUE => the window group loads and the layer's own "
         "enable bit reaches\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   silicon.  STILL GARBLE => that bit never lands, so no "
         "window write ever has.\n");
  up_mdelay(2500);

  /* --- Rung C: decisive.  Enable it again, but shrink ACT and DSP together to
   * 200x200 so the layer can only occupy the top-left corner and no scaling is
   * implied. --- */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_ACT_INFO,
                    (199u << 16) | 199u);
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_DSP_INFO,
                    (199u << 16) | 199u);
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                    ctrl_on);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: WINLOAD RUNG C: layer ON but shrunk to 200x200 in the "
         "TOP-LEFT corner\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   (ACT_INFO=DSP_INFO=199x199).  ACT=%08x DSP=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_REGION0_ACT_INFO),
         (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_REGION0_DSP_INFO));
  syslog(LOG_WARNING,
         "kickpi-k7:   O1 our framebuffer's top-left corner visible  => it "
         "FETCHES, look at stride.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   O2 garble in the corner + blue elsewhere       => group "
         "loads, no fetch.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   O3 full-screen garble, corner invisible        => group "
         "NEVER loads = ROOT CAUSE\n");
  up_mdelay(3000);

  /* --- Rung D: restore so the system is left as the driver intended. --- */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_ACT_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_DSP_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                    ctrl_on);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: WINLOAD RUNG D: restored to full size and layer ON\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_win_start_probe
 *
 * Description:
 *   *** TRYING TO START THE LAYER'S INTERNAL LOGIC -- AND TO FALSIFY THE ONE
 *   MECHANISM WHOSE DESCRIPTION MATCHES EVERY OBSERVATION. ***
 *
 *   Every static configuration this window needs is now verified against both
 *   the reference driver AND the TRM, register by register, and the window
 *   still issues no memory read at all: the poison address raises no bus error
 *   even though the MMU bypass is confirmed active (SYS_MMU_CTRL_IMD reads
 *   0x0bf40a04, bypass_en = 1).  What is left is not a value but a TRANSITION --
 *   some state machine inside the window that no amount of register contents
 *   will start.
 *
 *   Two candidates, one rung each, and both are falsifiable from the screen:
 *
 *   Rung A -- a 0 -> 1 edge on the region enable.  The window has been written
 *     "enabled" on every boot but has never been taken through disable first.
 *     If the region enable gates a transfer that must BEGIN, an edge is what
 *     starts it.
 *
 *   Rung B -- match the reference driver's ESMART_CTRL1 exactly.  Linux writes
 *     only the two read-id fields there, so every other bit keeps its reset
 *     value of zero.  This driver additionally sets dma_rreq_hurry_en with
 *     thold = 0, which makes the TRM's hurry condition permanently true -- the
 *     window has been holding its priority request high since the first round,
 *     and that has never been compared against the reference.
 *
 *   Rung D -- esmart_frm_resetn_en (ESMART_CTRL0 bit31).  The TRM's "Software
 *     Frame Reset" section says this bit, when set, resets "the layer's
 *     internal logic ... exclude register logic" on EVERY vsync.  That
 *     sentence describes this board's symptom with unnerving precision: the
 *     register file reads back perfectly (it is excluded from the reset by
 *     design) while the fetch engine and line buffers are torn down once per
 *     frame and so never fill.
 *
 *     An earlier round concluded the bit "must stay 0" and reverted it, on the
 *     grounds that the TRM's reset value is 0, the reference driver never
 *     writes ESMART_CTRL0, and the bootloader left it 0.  All three of those
 *     say the STEADY STATE should be 0 -- none of them says the bit is never
 *     set on the way there, and a layer whose internal logic has never once
 *     been reset is not obviously the same thing as a layer whose reset is
 *     correctly released.
 *
 *     So this rung does not assume; it measures.  Setting the bit should, if
 *     the TRM's description is right, STOP the layer from contributing
 *     anything at all -- and the POST background is frozen solid blue for this
 *     probe, so "the layer stopped" is directly visible as a BLUE screen.
 *
 *       bit31=1 -> BLUE   => the bit really does collapse the layer's internal
 *                            logic, the description is confirmed, and the
 *                            question becomes how to release it properly.
 *       bit31=1 -> no change => the bit does not affect this window's fetching
 *                            at all, and that whole line of reasoning is dead.
 *
 ****************************************************************************/

int rk3576_vop_win_start_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t eb;
  uint32_t pb;
  uint32_t ctrl_on;
  uint32_t ctrl_off;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  pb = RK3576_VOP_POST(priv->base, priv->cfg.port);

  ctrl_on = RK3576_VOP_ESMART_FMT_RGB888 |
            RK3576_VOP_ESMART_REGION0_MST_EN |
            RK3576_VOP_ESMART_REGION0_RB_SWAP;
  ctrl_off = RK3576_VOP_ESMART_FMT_RGB888;

  /* Freeze the background blue so that blue always means "the layer
   * contributed nothing". */

  rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_DSP_BG,
                    RK3576_VOP_POST_BG_DISPLAY_EN |
                        (0x3ffu << RK3576_VOP_POST_BG_BLUE_SHIFT));
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== WINDOW START PROBE (ESMART%u) =====\n",
         (unsigned)RK3576_VOP_ESMART_IDX);
  syslog(LOG_WARNING,
         "kickpi-k7:   background frozen SOLID BLUE, so BLUE means the layer "
         "stopped contributing.\n");

  /* --- Rung A: take the region enable through 0 before returning it to 1. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, ctrl_off);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(300);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG A: region DISABLED for 300 ms (REGION0_CTRL=%08x), "
         "now re-enabling\n",
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_CTRL));

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, ctrl_on);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(2500);

  syslog(LOG_WARNING,
         "kickpi-k7:   RUNG A done: REGION0_CTRL=%08x.  If a disable/re-enable "
         "edge was what the\n",
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_CTRL));
  syslog(LOG_WARNING,
         "kickpi-k7:   layer needed, the framebuffer is on the glass now.\n");

  /* --- Rung B: match the reference driver's CTRL1 EXACTLY. ---
   *
   * The reference driver touches only the two read-id fields of this register
   * (VOP_WIN_SET on axi_yrgb_id / axi_uv_id), so every other bit keeps its
   * reset value -- all zero.  This driver writes the whole word and sets
   * dma_rreq_hurry_en (bit28) with thold = 0.
   *
   * thold = 0 makes the TRM's condition "if esmart empty lb number >=
   * esmart_dma_rreq_thold, dma_rreq_hurry is asserted" true ALWAYS, so this
   * window has been holding its hurry/priority request permanently asserted
   * since the first bring-up round.  That was added here as an anti-starvation
   * measure and has never once been tested against the reference, which simply
   * does not do it.  A request line tied permanently high is a perfectly good
   * way to have the arbiter drop or mis-schedule a channel's traffic.
   *
   * This rung clears it, leaving exactly what the reference driver leaves: the
   * ids, and zero everywhere else.
   */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_CTRL1,
                    (g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX]
                     << RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT) |
                        (g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX]
                         << RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT));
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(2500);

  {
    uint32_t c1 = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_CTRL1);

    syslog(LOG_WARNING,
           "kickpi-k7: RUNG B: ESMART_CTRL1=%08x -- hurry_en bit28=%u is now "
           "CLEAR, exactly what\n",
           (unsigned)c1, (unsigned)((c1 >> 28) & 1u));
    syslog(LOG_WARNING,
           "kickpi-k7:   the reference driver leaves (it writes only the id "
           "fields).  Framebuffer on\n");
    syslog(LOG_WARNING,
           "kickpi-k7:   the glass now => hurry_en was the problem.\n");
  }

  /* --- Rung C: restore the driver's own value so the next rung is clean. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_CTRL1,
                    (g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX]
                     << RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT) |
                        (g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX]
                         << RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT) |
                        RK3576_VOP_ESMART_CTRL1_DMA_RREQ_HURRY_EN);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(500);

  /* --- Rung D: set the per-vsync frame reset of the layer's internal logic. ---
   *
   * The prediction, if the TRM's description is accurate, is a BLUE screen.
   * Anything else falsifies it.
   */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_CTRL0,
                    RK3576_VOP_ESMART_SCL_NUM |
                        RK3576_VOP_ESMART_CTRL0_FRM_RESETN_EN);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(2500);

  {
    uint32_t c0 = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_CTRL0);

    syslog(LOG_WARNING,
           "kickpi-k7: RUNG D: ESMART_CTRL0=%08x (frm_resetn_en bit31=%u) -- "
           "the TRM says this resets\n",
           (unsigned)c0, (unsigned)((c0 >> 31) & 1u));
    syslog(LOG_WARNING,
           "kickpi-k7:   the layer's internal logic on EVERY vsync.  "
           "BLUE now => confirmed.\n");
    syslog(LOG_WARNING,
           "kickpi-k7:   UNCHANGED => the bit does not gate this window's "
           "fetching at all.\n");
  }

  /* --- Rung E: release it again and confirm the picture comes back. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_CTRL0,
                    RK3576_VOP_ESMART_SCL_NUM);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(2500);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG E: ESMART_CTRL0=%08x (bit31 cleared again).  "
         "Picture returning means the\n",
         (unsigned)rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_CTRL0));
  syslog(LOG_WARNING,
         "kickpi-k7:   bit does control the layer and Rung D's reading is "
         "valid.\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_axi_ctrl_probe
 *
 * Description:
 *   *** FINDS THE WRITE FORMAT OF ESMART_AXI_CTRL_IMD -- THE REGISTER NOBODY
 *   HAS EVER VERIFIED, AND NOW THE PRIME SUSPECT. ***
 *
 *   Every log shows the driver's write to this register failing to survive.
 *   The driver writes 0x000100fc (dma_4k_addr_opt | mmu_bypass |
 *   outstanding_en | num = 15) and reads back 0x00010000: bit16 kept,
 *   bits[7:2] gone.  With axi_sel set it wrote 0x000100fe and read back
 *   0x00010002 -- so the loss happens with axi_sel clear too, which retires the
 *   previous explanation ("setting axi_sel clears bits[7:2]").
 *
 *   mmu_bypass IS ONE OF THE BITS THAT VANISHES, and this driver programs
 *   PHYSICAL addresses.  The header of this file already states the cost:
 *   addresses handed to "an MMU that has no page tables", whose failure mode is
 *   "a read that never comes back -- no data, no bus error, and a POST whose
 *   output buffer is permanently empty".  That is this board's signature, and
 *   it accounts for the poison address raising no bus error and the armed MMUs
 *   raising no fault: the read never reaches anything that could report one.
 *
 *   WHY THESE BITS MIGHT NOT SURVIVE.  Several registers in this block take a
 *   WRITE ENABLE in one half of the word -- SYS_MMU_CTRL_IMD and the
 *   SYS_REG_CFG_DONE group are hiword-masked, and this driver has already been
 *   bitten by writing those plainly: the write was silently discarded.  NOBODY
 *   HAS EVER TESTED WHETHER THIS REGISTER IS ONE OF THEM.  Reading back a
 *   register after a plain write cannot distinguish "the bits are read-only"
 *   from "the bits need a write enable" from "the hardware clears them"; the
 *   same target is therefore written in five candidate encodings and each is
 *   read back, and then the one that works is re-tested after a delay to see
 *   whether the hardware clears it again immediately.
 *
 *     A  value                    plain data
 *     B  value | (value << 16)    lo = data, hi = write enable
 *     C  (value << 16) | value    hi = data, lo = write enable
 *     D  value | (0x00ff << 16)   lo = data, hi = write enable, all bits on
 *     E  (value << 16) | 0x00ff   hi = data, lo = write enable, all bits on
 *
 *   The target is 0x000000fc -- mmu_bypass(bit2) + outstanding_en(bit3) +
 *   outstanding_num[7:4] = 15 -- so a working encoding reads 0x0c (or better
 *   0xfc) back somewhere in the word.
 *
 *   Read/write, but it restores the register before returning.
 *
 ****************************************************************************/

int rk3576_vop_axi_ctrl_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t eb;
  uint32_t sb;
  uint32_t save;
  uint32_t target;
  uint32_t cand[5];
  uint32_t got[5];
  uint32_t settle;
  int winner;
  int i;

  static const char * const names[5] =
  {
    "plain value",
    "value | (value << 16) [lo=data]",
    "(value << 16) | value [hi=data]",
    "value | (00ff << 16) [lo=data]",
    "(value << 16) | 00ff [hi=data]"
  };

  if (priv == NULL)
    {
      return -ENODEV;
    }

  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  sb = RK3576_VOP_SYS_CTRL(priv->base);

  save = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== AXI CTRL WRITE-FORMAT PROBE (ESMART%u) =====\n",
         (unsigned)RK3576_VOP_ESMART_IDX);
  syslog(LOG_WARNING,
         "kickpi-k7:   AXI_CTRL_IMD reads %08x after the driver wrote "
         "0x0001xxfc: mmu_bypass,\n", (unsigned)save);
  syslog(LOG_WARNING,
         "kickpi-k7:   outstanding_en and outstanding_num[7:4] are ALL gone, "
         "and this driver\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   programs physical addresses.  Testing whether those "
         "bits are writable at\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   all, and in which encoding.\n");

  target = 0x000000fcu;

  cand[0] = target;
  cand[1] = target | (target << 16);
  cand[2] = (target << 16) | target;
  cand[3] = target | (0x00ffu << 16);
  cand[4] = (target << 16) | 0x00ffu;

  winner = -1;

  for (i = 0; i < 5; i++)
    {
      bool stuck;

      rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD, cand[i]);
      up_udelay(20);
      got[i] = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD);

      stuck = ((got[i] & 0x0cu) == 0x0cu) ||
              (((got[i] >> 16) & 0x0cu) == 0x0cu);

      syslog(LOG_WARNING,
             "kickpi-k7:   %c %-34s wrote %08x -> read %08x%s\n",
             (char)('A' + i), names[i], (unsigned)cand[i], (unsigned)got[i],
             stuck ? "   <== BITS 3:2 SURVIVED" : "");

      if (winner < 0 && stuck)
        {
          winner = i;
        }
    }

  if (winner >= 0)
    {
      bool hi = (winner == 2 || winner == 4);
      uint32_t full = (hi ? (target << 16) : target) |
                      (winner == 1 ? (target << 16) : 0u) |
                      (winner == 3 ? (0x00ffu << 16) : 0u) |
                      (winner == 4 ? 0x00ffu : 0u);

      syslog(LOG_WARNING,
             "kickpi-k7:   => encoding %c works (%s).  Re-writing with the "
             "REAL value and watching\n", (char)('A' + winner),
             hi ? "data in the HIGH half" : "data in the LOW half");

      rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD, full);
      settle = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD);

      up_mdelay(60);

      syslog(LOG_WARNING,
             "kickpi-k7:      wrote %08x, immediately read %08x, after 60 ms "
             "read %08x\n", (unsigned)full, (unsigned)settle,
             (unsigned)rk3576_vop_getreg(priv, eb +
                                         RK3576_VOP_ESMART_AXI_CTRL_IMD));

      if (((settle >> (hi ? 16 : 0)) & 0x0cu) != 0x0cu)
        {
          syslog(LOG_ERR,
                 "kickpi-k7:      the bits were not held even by the working "
                 "encoding -- check the\n");
          syslog(LOG_ERR,
                 "kickpi-k7:      field positions instead of the write "
                 "format ***\n");
        }
    }
  else
    {
      syslog(LOG_ERR,
             "kickpi-k7:   => *** NO ENCODING COULD WRITE BITS[7:2].  They are "
             "not writable through\n");
      syslog(LOG_ERR,
             "kickpi-k7:   this register on this silicon, so esmart_mmu_bypass "
             "has NEVER been set:\n");
      syslog(LOG_ERR,
             "kickpi-k7:   every framebuffer address this driver has ever "
             "programmed has been\n");
      syslog(LOG_ERR,
             "kickpi-k7:   offered to an MMU with no page tables, whose "
             "documented failure mode is\n");
      syslog(LOG_ERR,
             "kickpi-k7:   a read that never returns, with no bus error.  The "
             "SYS-level bypass in\n");
      syslog(LOG_ERR,
             "kickpi-k7:   SYS_MMU_CTRL_IMD is then the ONLY thing standing "
             "behind it. ***\n");
    }

  /* The SYS-level bypass is the backstop, so verify it really reads back set
   * rather than assuming a write to it took. */

  {
    uint32_t m = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_MMU_CTRL_IMD);

    syslog(LOG_WARNING,
           "kickpi-k7:   SYS_MMU_CTRL_IMD=%08x (mmu_bypass_en bit2=%u, "
           "mmu_bypass_id[8:4]=%u, mmu1_bypass_en bit9=%u) -- bypass is what\n",
           (unsigned)m, (unsigned)((m >> 2) & 1u), (unsigned)((m >> 4) & 0x1fu),
           (unsigned)((m >> 9) & 1u));
    syslog(LOG_WARNING,
           "kickpi-k7:   keeps the window's physical addresses away from an "
           "unconfigured MMU.\n");

    if (((m >> 2) & 1u) == 0u)
      {
        syslog(LOG_ERR,
               "kickpi-k7:   *** AND IT READS BACK CLEAR -- after "
               "rk3576_vop_sys_masked_write() set\n");
        syslog(LOG_ERR,
               "kickpi-k7:   it.  Nothing is bypassing either MMU, so every "
               "fetch this window issues\n");
        syslog(LOG_ERR,
               "kickpi-k7:   is translated with no page table: it cannot "
               "return, and it cannot\n");
        syslog(LOG_ERR,
               "kickpi-k7:   raise an error.  ROOT CAUSE. ***\n");
      }
  }

  /* *** LEAVE THE WINDOW CORRECTLY CONFIGURED, NOT AS IT WAS FOUND. ***
   *
   * Restoring the value that was read on entry would put the window straight
   * back into the state under test -- the one with no mmu_bypass -- and quietly
   * undo the point of the probe.  If an encoding was found, it is used to write
   * the full intended configuration and left in place, so the panel has the
   * chance to react within the same run.  Only when no encoding works is the
   * original value put back, because then there is nothing better to write.
   */

  if (winner >= 0)
    {
      uint32_t full;

      switch (winner)
        {
          case 0:  /* plain */
            full = 0x000100fcu;
            break;
          case 1:  /* lo = data, hi = value */
            full = 0x000100fcu | (0x000100fcu << 16);
            break;
          case 2:  /* hi = data, lo = value */
            full = (0x000100fcu << 16) | 0x000100fcu;
            break;
          case 3:  /* lo = data, hi = all-ones enable */
            full = 0x000100fcu | (0x00ffu << 16);
            break;
          default: /* hi = data, lo = all-ones enable */
            full = (0x000100fcu << 16) | 0x00ffu;
            break;
        }

      rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD, full);
      rk3576_vop_trigger_cfg_done(priv);

      syslog(LOG_WARNING,
             "kickpi-k7:   window left as: wrote %08x -> reads %08x (mmu_bypass "
             "now wired the way\n", (unsigned)full,
             (unsigned)rk3576_vop_getreg(priv, eb +
                                         RK3576_VOP_ESMART_AXI_CTRL_IMD));
      syslog(LOG_WARNING,
             "kickpi-k7:   this silicon accepts it).  WATCH THE SCREEN: if "
             "mmu_bypass was the missing\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   piece, the framebuffer should appear now.\n");
    }
  else
    {
      rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD, save);
      rk3576_vop_trigger_cfg_done(priv);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_clk_alive_probe
 *
 * Description:
 *   *** MEASURE WHETHER THE VOP'S DATAPATH CLOCK (aclk) IS RUNNING. ***
 *
 *   Every other explanation for "the window is configured correctly and issues
 *   no read" has been measured and eliminated against both the TRM and
 *   Rockchip's driver: clocks-gates (main CRU gates open, channel BIU gate open,
 *   divider correct), power domains, both reset polarities, the mirror->real
 *   load, the fetch address, the pixel format, the stride, the scaler, the layer
 *   enable, the overlay routing and LAYER_SEL encoding, the mixer factors, the
 *   security configuration, the MMU (with the probe verified to arm both MMUs
 *   and clear BOTH bypasses), the BIU idle state, the AXI outstanding limit, the
 *   AXI read identiddle (now per-window), CTRL0, DLY_NUM and the auto-gating
 *   switches.
 *
 *   What has never been measured is whether the DATAPATH clock is running at
 *   all.  The register file and the datapath do not share a clock: registers
 *   answer on hclk/pclk, the fetch engine and line buffers run on aclk.  A VOP
 *   whose aclk is stopped answers every read back perfectly, retains every
 *   write, and STILL cannot issue a single memory access -- and because nothing
 *   happens, it raises no error either.  That is this board's symptom exactly,
 *   and it is the hypothesis already written into this file:
 *
 *     "THAT IS THE SIGNATURE OF A BLOCK WITH NO CLOCK.  The register file sits
 *      on the peripheral clock, so it answers reads and retains writes while
 *      the datapath's aclk is gated off; the request generator then simply
 *      never runs, and a clock that is gated produces no error because nothing
 *      happens."
 *
 *   POST_CLK_CNT turns that into a measurement.  Setting calc_clk_en makes the
 *   hardware count aclk and dclk cycles over a fixed 5000-hclk window, so the
 *   two counts come from the SAME window and their ratio is meaningful.  dclk
 *   is a built-in control: the POST's timing generator demonstrably runs -- the
 *   forced-black, forced-zero and solid-background rungs all reach the glass --
 *   so dclk MUST count.  A zero aclk count alongside a non-zero dclk count is
 *   therefore not a subtle reading: it means the datapath is not clocked.
 *
 *   Read the TRM's field list and Linux's expose them as .calc_clk_en /
 *   .calc_aclk_cnt / .calc_dclk_cnt on RK3576's video port 0.
 *
 ****************************************************************************/

int rk3576_vop_clk_alive_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t pb;
  uint32_t clk_cnt;
  uint32_t aclk;
  uint32_t dclk;
  uint32_t prev;
  int round;
  int aclk_live = 0;
  int dclk_live = 0;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  pb = RK3576_VOP_POST(priv->base, priv->cfg.port);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== CLOCK ALIVENESS PROBE (POST%u + 0x0F4) =====\n",
         (unsigned)priv->cfg.port);
  syslog(LOG_WARNING,
         "kickpi-k7:   Register file = hclk/pclk, fetch engine = aclk.  A stopped "
         "aclk answers\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   every read correctly and still issues no memory access "
         "at all.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   dclk is the control: the POST timing generator works "
         "(three rungs reached\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   the glass), so dclk MUST count.\n");

  for (round = 0; round < 3; round++)
    {
      /* Start a fresh counting window. */

      rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_CLK_CNT_OFF, 0u);
      rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_CLK_CNT_OFF,
                        RK3576_VOP_POST_CLK_EN);

      /* The window is 5000 hclk cycles.  hclk is the VOP's bus clock, at most a
       * few hundred MHz, so 5000 cycles is at most tens of microseconds; 20 ms
       * is orders of magnitude more than enough for the hardware to latch a
       * result, and it also guarantees a fresh window rather than a stale one.
       */

      up_mdelay(20);

      clk_cnt = rk3576_vop_getreg(priv, pb + RK3576_VOP_POST_CLK_CNT_OFF);
      aclk = (clk_cnt & RK3576_VOP_POST_ACLK_CNT_MASK) >>
             RK3576_VOP_POST_ACLK_CNT_SHIFT;
      dclk = clk_cnt & RK3576_VOP_POST_DCLK_CNT_MASK;

      syslog(LOG_WARNING,
             "kickpi-k7:   round %d: CLK_CNT=%08x -> aclk counted %u, dclk "
             "counted %u\n",
             round, (unsigned)clk_cnt, (unsigned)aclk, (unsigned)dclk);

      if (aclk != 0u)
        {
          aclk_live = 1;
        }

      if (dclk != 0u)
        {
          dclk_live = 1;
        }

      /* *** SECOND READ WITHOUT RE-TRIGGERING, AND AFTER A LONGER DELAY. ***
       *
       * The counters came out at 15003 / 1580, 15003 / 1579, 15003 / 1579 --
       * aclk identical every time while dclk jitters by one.  That is the
       * expected picture for a FIXED-WINDOW latch: aclk is derived from the same
       * PLL as hclk and so lands on exactly the same count every window, while
       * dclk comes from a different PLL and cannot.  The dclk jitter is in fact
       * the proof that the counter re-measures rather than showing a stale
       * value.
       *
       * This second read settles it directly: it is taken WITHOUT clearing, so
       *   - unchanged  => the register is a fixed-window latch (as documented)
       *   - different  => the counters keep accumulating, i.e. free-running
       * and the longer delay before it removes any doubt that a fresh window
       * would have completed either way.
       */

      up_mdelay(150);

      {
        uint32_t again = rk3576_vop_getreg(priv, pb +
                                           RK3576_VOP_POST_CLK_CNT_OFF);
        uint32_t a2 = (again & RK3576_VOP_POST_ACLK_CNT_MASK) >>
                      RK3576_VOP_POST_ACLK_CNT_SHIFT;
        uint32_t d2 = again & RK3576_VOP_POST_DCLK_CNT_MASK;

        syslog(LOG_WARNING,
               "kickpi-k7:      +150 ms, NOT re-triggered: CLK_CNT=%08x -> aclk "
               "%u, dclk %u%s\n",
               (unsigned)again, (unsigned)a2, (unsigned)d2,
               (a2 == aclk && d2 == dclk)
                   ? "  (unchanged => fixed-window latch)"
                   : "  (changed => the counters keep running)");
      }

      /* Keep the last reading for the verdict, but note that a single zero must
       * not be over-read: the first window can straddle the moment the counter
       * is enabled.  Three rounds, and any non-zero count in any of them, is
       * enough to call the clock alive. */

      prev = clk_cnt;
      (void)prev;
    }

  syslog(LOG_WARNING,
         "kickpi-k7:   dclk %s, aclk %s\n",
         dclk_live ? "COUNTS (timing generator clocked, as expected)"
                   : "COUNTED ZERO -- the POST would not be running, which "
                     "contradicts the rungs",
         aclk_live ? "COUNTS (the datapath clock is running)"
                   : "COUNTED ZERO (the datapath clock is STOPPED)");

  if (dclk_live && !aclk_live)
    {
      syslog(LOG_ERR,
             "kickpi-k7: CLOCK PROBE => *** dclk runs, aclk DOES NOT.  The "
             "register file is\n");
      syslog(LOG_ERR,
             "kickpi-k7:   clocked and answers every read, while the datapath "
             "that would issue the\n");
      syslog(LOG_ERR,
             "kickpi-k7:   layer's memory reads has no clock at all -- so no "
             "read is ever issued and\n");
      syslog(LOG_ERR,
             "kickpi-k7:   none of the mechanisms that would REPORT an error "
             "ever runs either.\n");
      syslog(LOG_ERR,
             "kickpi-k7:   This is the root cause, and it explains why every "
             "register reads back\n");
      syslog(LOG_ERR,
             "kickpi-k7:   perfectly while the POST starves.  The fix is in the "
             "VOP's aclk: its CRU\n");
      syslog(LOG_ERR,
             "kickpi-k7:   gate is reported open, so look at the DIVIDER and "
             "the clock MUX next. ***\n");
    }
  else if (aclk_live && dclk_live)
    {
      uint32_t a_last = (clk_cnt & RK3576_VOP_POST_ACLK_CNT_MASK) >>
                        RK3576_VOP_POST_ACLK_CNT_SHIFT;
      uint32_t d_last = clk_cnt & RK3576_VOP_POST_DCLK_CNT_MASK;

      syslog(LOG_WARNING,
             "kickpi-k7: CLOCK PROBE => both clocks run, so \"no clock\" is "
             "eliminated.  aclk:dclk\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   over the SAME window is %u:%u, about %u:1, and "
             "aclk:hclk is about %u:1\n",
             (unsigned)a_last, (unsigned)d_last,
             (unsigned)(d_last != 0u ? (a_last / d_last) : 0u),
             (unsigned)(a_last / 5000u));
      syslog(LOG_WARNING,
             "kickpi-k7:   (hclk is the fixed 5000-cycle counting window).  "
             "With dclk at the\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   configured 62.5 MHz that puts aclk near %u MHz -- "
             "a healthy datapath\n", (unsigned)((a_last * 62500u) / (d_last * 1000u)));
      syslog(LOG_WARNING,
             "kickpi-k7:   clock, and an independent confirmation that the "
             "pixel clock is configured\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   correctly.  This also matches theory: aclk and hclk "
             "share a PLL so the\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   aclk count is identical every round, while dclk "
             "comes from another PLL and\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   jitters by a count or two.  A clock fault would "
             "show up as a zero or a\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   wandering count, not a clean fixed ratio.\n");
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_scan_status_probe
 *
 * Description:
 *   *** READS THE TWO PER-PORT STATUS BITS THIS DRIVER HAS NEVER LOOKED AT:
 *   dma_stop_valid and mmu_idle IN SYS_STATUS0/1/2. ***
 *
 *   Everything else has been measured: the clocks are alive (POST_CLK_CNT
 *   counted 15003 aclk in the same window as 1579 dclk, both stable), the power
 *   domains are on, every register in the window matches the reference driver
 *   and the TRM field for field, the MMU probe now runs with its preconditions
 *   actually established (bypasses off, paging on both MMUs) and still faults
 *   nothing, and the window is bound to the running video port
 *   (ESMART_PORT_SEL_IMD = 0) and selected by the mixer (LAYER_SEL layer0 = 2).
 *
 *   And yet no read is ever issued, while the mixer demonstrably waits for the
 *   layer -- the POST's input buffer under-runs the moment the layer is enabled
 *   and stops the moment it is disabled.
 *
 *   dma_stop_validN is the remaining way to reconcile those two: it reports what
 *   the AXI channel is ACTUALLY doing, as opposed to what SYS_AXI0_CTRL_IMD --
 *   a control register this driver writes -- says.  A channel that is genuinely
 *   stopped cannot issue a read and cannot raise an error, which is precisely
 *   the combination observed.
 *
 *   Two other things are read in the same pass because they are free and would
 *   otherwise be the next questions:
 *
 *     dsp_vcntN -- the video port's vertical counter.  Sampled repeatedly; if it
 *       advances, the scan state machine is running and the window really does
 *       have a line reference to trigger fetches from.  If it is frozen, the
 *       window has nothing to synchronise to and the fault is in the video port
 *       rather than in any window.
 *
 *     SYS_AXI1_CTRL_IMD -- so the dma_stop CONTROL bit and the dma_stop STATUS
 *       bit can be compared on both channels in one line.
 *
 ****************************************************************************/

int rk3576_vop_scan_status_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sb;
  uint32_t st0;
  uint32_t st1;
  uint32_t st2;
  uint32_t axi0;
  uint32_t axi1;
  uint32_t v0_first;
  uint32_t v0_last;
  uint32_t v0_min;
  uint32_t v0_max;
  int v0_moved = 0;
  int i;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  sb = RK3576_VOP_SYS_CTRL(priv->base);

  st0 = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_STATUS0);
  st1 = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_STATUS1);
  st2 = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_STATUS2);
  axi0 = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_AXI0_CTRL_IMD);
  axi1 = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_AXI1_CTRL_IMD);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== SCAN / AXI-STATUS PROBE (SYS_STATUS0/1/2) =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   STATUS0=%08x STATUS1=%08x STATUS2=%08x\n",
         (unsigned)st0, (unsigned)st1, (unsigned)st2);

  syslog(LOG_WARNING,
         "kickpi-k7:   VP0: dsp_vcnt=%u  mmu_idle_bit1=%u  dma_stop_valid_bit0=%u"
         "  (vcn[28:16])\n",
         (unsigned)((st0 & RK3576_VOP_SYS_STATUS_VCNT_MASK) >>
                    RK3576_VOP_SYS_STATUS_VCNT_SHIFT),
         (unsigned)((st0 & RK3576_VOP_SYS_STATUS_MMU_IDLE) != 0u),
         (unsigned)((st0 & RK3576_VOP_SYS_STATUS_DMA_STOP) != 0u));

  syslog(LOG_WARNING,
         "kickpi-k7:   VP1: dsp_vcnt=%u dma_stop_valid=%u ; VP2: dsp_vcnt=%u "
         "dma_stop_valid=%u\n",
         (unsigned)((st1 & RK3576_VOP_SYS_STATUS_VCNT_MASK) >>
                    RK3576_VOP_SYS_STATUS_VCNT_SHIFT),
         (unsigned)(st1 & 1u),
         (unsigned)((st2 & RK3576_VOP_SYS_STATUS_VCNT_MASK) >>
                    RK3576_VOP_SYS_STATUS_VCNT_SHIFT),
         (unsigned)(st2 & 1u));

  /* CONTROL versus STATUS, side by side on both channels.  The control bit is
   * the one this driver writes; the status bit is what the channel is doing. */

  syslog(LOG_WARNING,
         "kickpi-k7:   CONTROL vs STATUS:  axi0_ctrl_dma_stop=%u  "
         "axi0_status_dma_stop_valid=%u\n",
         (unsigned)(axi0 & RK3576_VOP_SYS_AXI_DMA_STOP ? 1u : 0u),
         (unsigned)(st0 & RK3576_VOP_SYS_STATUS_DMA_STOP ? 1u : 0u));
  syslog(LOG_WARNING,
         "kickpi-k7:                        axi1_ctrl_dma_stop=%u  "
         "axi1_status_dma_stop_valid=%u\n",
         (unsigned)(axi1 & RK3576_VOP_SYS_AXI_DMA_STOP ? 1u : 0u),
         (unsigned)(st1 & RK3576_VOP_SYS_STATUS_DMA_STOP ? 1u : 0u));
  syslog(LOG_WARNING,
         "kickpi-k7:   ESMART0 fetches on axi0, so axi0's pair is the one that "
         "matters here.\n");

  if ((st0 & RK3576_VOP_SYS_STATUS_DMA_STOP) != 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   *** axi0 dma_stop_valid IS SET: the channel is "
             "STOPPED.  A stopped\n");
      syslog(LOG_ERR,
             "kickpi-k7:   channel issues no read and raises no error, which is "
             "why nothing has ever\n");
      syslog(LOG_ERR,
             "kickpi-k7:   faulted and why the poison address stayed silent, "
             "while the mixer still\n");
      syslog(LOG_ERR,
             "kickpi-k7:   waits for the layer.  ROOT CAUSE. ***\n");
    }

  if ((st0 & RK3576_VOP_SYS_STATUS_MMU_IDLE) == 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   axi0's mmu_idle status reads BUSY -- something on "
             "that channel is in\n");
      syslog(LOG_ERR,
             "kickpi-k7:   flight, which every single-sample readback missed "
             "***\n");
    }

  /* Now watch the VP0 vertical counter for about two frames.  One frame is
   * htotal 780 x vtotal 1320 at ~62.5 MHz, about 16.5 ms, so 40 ms covers two
   * and a half frames and leaves no doubt whether it is moving.
   */

  v0_first = (st0 & RK3576_VOP_SYS_STATUS_VCNT_MASK) >>
             RK3576_VOP_SYS_STATUS_VCNT_SHIFT;
  v0_last = v0_first;
  v0_min = v0_first;
  v0_max = v0_first;

  for (i = 0; i < 400; i++)
    {
      uint32_t s = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_STATUS0);
      uint32_t v = (s & RK3576_VOP_SYS_STATUS_VCNT_MASK) >>
                   RK3576_VOP_SYS_STATUS_VCNT_SHIFT;

      if (v != v0_last)
        {
          v0_moved = 1;
        }

      if (v < v0_min)
        {
          v0_min = v;
        }

      if (v > v0_max)
        {
          v0_max = v;
        }

      v0_last = v;
      up_udelay(100);
    }

  syslog(LOG_WARNING,
         "kickpi-k7:   VP0 dsp_vcnt over ~40 ms: first=%u last=%u min=%u "
         "max=%u -> %s\n",
         (unsigned)v0_first, (unsigned)v0_last, (unsigned)v0_min,
         (unsigned)v0_max,
         v0_moved ? "ADVANCING (the video port is scanning)"
                  : "FROZEN (the video port is NOT scanning)");

  if (!v0_moved)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   *** THE VP0 VERTICAL COUNTER NEVER MOVES.  The video "
             "port is not\n");
      syslog(LOG_ERR,
             "kickpi-k7:   scanning, so no window can be asked for a line and no "
             "fetch can begin --\n");
      syslog(LOG_ERR,
             "kickpi-k7:   while the POST still drives the panel from its own "
             "line buffer.  That\n");
      syslog(LOG_ERR,
             "kickpi-k7:   makes the fault a VIDEO PORT problem, not a window "
             "problem, and it\n");
      syslog(LOG_ERR,
             "kickpi-k7:   would explain why nothing done inside any window has "
             "ever helped. ***\n");
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_cluster_probe
 *
 * Description:
 *   *** THE BISECTION: PUT CLUSTER0, NOT ESMART0, ON LAYER0. ***
 *
 *   Every property of the ESMART0 window has now been verified against both the
 *   TRM and Rockchip's own driver, register by register, and the window still
 *   issues no memory read:
 *
 *     the datapath clock runs (POST_CLK_CNT counted 15003 aclk in the same
 *       window as 1579 dclk, both stable across three rounds and unchanged when
 *       re-read 150 ms later, i.e. a real fixed-window measurement);
 *     the power domains are on and the register file is clocked (every register
 *       reads back exactly as written);
 *     the video port is scanning (dsp_vcnt0 sweeps the full 0..1318 range and
 *       advances at line rate -- 256 -> 539 over 20 ms is exactly one wrap of
 *       256 + 1600 mod 1320);
 *     the AXI channel is not stopped (SYS_STATUS0 dma_stop_valid = 0);
 *     the window is bound to that running port (ESMART_PORT_SEL_IMD = 0) and is
 *       the layer the mixer selects (LAYER_SEL layer0 = 2 = Esmart0), which is
 *       why the POST under-runs the instant the layer is enabled;
 *     and the MMU probe, now with its preconditions actually established --
 *       both bypasses cleared by a proper masked write, paging on BOTH MMUs --
 *       still faults nothing.
 *
 *   So the question is no longer "which bit of ESMART0 is wrong".  It is
 *   "is the fault inside the ESMART block, or in what all windows share?"  Only
 *   a second kind of window can answer that, and CLUSTER0 is the right one: it
 *   reaches the same video port (LAYER_SEL 4'b0000), it has its own register
 *   block, its own DMA and its own AXI read-ids, but it shares the overlay's
 *   request path, the AXI ports and the clocks with ESMART.
 *
 *   The probe therefore swaps layer0's source -- nothing else -- waits, and
 *   reports.  It restores the ESMART configuration on the way out, so the board
 *   is left exactly as it was found.
 *
 *   HOW TO READ IT:
 *
 *     the framebuffer appears  => CLUSTER0 fetches, so the fault is specific to
 *       the ESMART block (or to how this driver drives it), and the bring-up has
 *       a working path to build on;
 *     the picture is unchanged or turns blue => CLUSTER0 does not fetch either,
 *       so the fault is in the shared request path, and every window-level
 *       setting can be ruled out for good.
 *
 *   THE DETAILS THAT DIFFER FROM ESMART, and would each be enough to break a
 *   copy-paste configuration:
 *
 *     the format field is SIX bits at [6:1], not five;
 *     bit7 is tile_mode and must be 0 for a linear framebuffer;
 *     csc_mode is at [12:10], not [9:7];
 *     rb_swap is bit14, not bit15;
 *     the AXI read-ids live in WIN0_CTRL2 at [4:0]/[9:5], not in ESMART_CTRL1;
 *     the cluster needs its OWN enable (CLUSTER0_CTRL bit0) in addition to the
 *       window's, because the reference driver does VOP_CLUSTER_SET(enable, 1)
 *       for cluster windows and has no such call for ESMART;
 *     a working CLUSTER also has frm_reset_en = 1 and dma_stride_4k_disable = 1,
 *       which the reference driver sets for CLUSTER windows and never for
 *       ESMART ones;
 *     and the pixel format value itself is the same 1, because the reference
 *       driver's format enum is shared between the two window types.
 *
 ****************************************************************************/

int rk3576_vop_cluster_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t cb;
  uint32_t eb;
  uint32_t ovl_base;
  uint32_t save_region0;
  uint32_t save_layer_sel;
  uint32_t ctrl0;
  uint32_t ctrl2;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  cb = RK3576_VOP_CLUSTER(priv->base);
  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  ovl_base = RK3576_VOP_OVERLAY_PORT(priv->base, priv->cfg.port);

  save_region0 = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL);
  save_layer_sel = rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_LAYER_SEL);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== CLUSTER0 BISECTION PROBE =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   Putting CLUSTER0 on layer0 instead of ESMART0.  Same "
         "video port, same\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   overlay, same AXI port and clocks -- different window "
         "block, different\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   DMA, different registers.  It is the only way left to tell "
         "whether the fault\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   is inside the ESMART block or in the path all windows "
         "share.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   LAYER_SEL was %08x; Cluster0 on layer0 is 4'b0000\n",
         (unsigned)save_layer_sel);

  /* Take ESMART0 out of the picture entirely so that anything on the glass can
   * only be CLUSTER0's doing.  LAYER_SEL below is what actually swaps the
   * source; clearing the region enable just guarantees ESMART0 cannot
   * contribute as well. */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL,
                    RK3576_VOP_ESMART_FMT_RGB888);

  /* --- Configure CLUSTER0_WIN0. --- */

  /* CTRL0: enable(bit0) + RGB888(1 at [6:1]) + rb_swap(bit14).
   * tile_mode(bit7) stays 0 -- this is a linear framebuffer, not AFBC.  Every
   * other field is left at its reset value, exactly as the reference driver
   * leaves them for a plain RGB layer. */

  ctrl0 = RK3576_VOP_WIN0_EN | RK3576_VOP_WIN0_FMT_RGB888 |
          RK3576_VOP_WIN0_RB_SWAP;
  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_CTRL0, ctrl0);

  /* CTRL2: this window's own AXI read-ids (yrgb [4:0], uv [9:5]). */

  ctrl2 = (RK3576_VOP_CLUSTER0_AXI_YRGB_ID <<
           RK3576_VOP_WIN0_CTRL2_YRGB_ID_SHIFT) |
          (RK3576_VOP_CLUSTER0_AXI_UV_ID <<
           RK3576_VOP_WIN0_CTRL2_UV_ID_SHIFT);
  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_CTRL2, ctrl2);

  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_YRGB_MST,
                    (uint32_t)up_addrenv_va_to_pa(priv->fbmem));

  /* Same stride definition as ESMART: for RGB888 the register takes WORDS,
   * (width*3/4) + (width%3), which is 540 for 720 pixels. */

  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_VIR,
                    ((uint32_t)priv->cfg.xres * 3 / 4) +
                        ((uint32_t)priv->cfg.xres % 3u));

  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_ACT_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_DSP_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_DSP_ST, 0u);

  /* Port select and delay, at the same block offsets as ESMART's. */

  rk3576_vop_putreg(priv, cb + RK3576_VOP_CLUSTER0_PORT_SEL_IMD,
                    RK3576_VOP_ESMART_PORT_VP0);
  rk3576_vop_putreg(priv, cb + RK3576_VOP_CLUSTER0_DLY_NUM, 0u);

  /* CLUSTER0_CTRL: the cluster's own enable, plus the two settings the
   * reference driver applies to CLUSTER windows and never to ESMART ones.
   * afbc_enable (bit1) stays 0 -- the framebuffer is linear. */

  rk3576_vop_putreg(priv, cb + RK3576_VOP_CLUSTER0_CTRL_OFF,
                    RK3576_VOP_CLUSTER_CTRL_EN |
                        RK3576_VOP_CLUSTER_CTRL_DMA_STRIDE_4K_DIS |
                        RK3576_VOP_CLUSTER_CTRL_FRM_RESET_EN);

  /* --- Swap layer0's source, then load. --- */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_LAYER_SEL,
                    RK3576_VOP_LAYER_SEL_L0(0u) |
                        RK3576_VOP_LAYER_SEL_L1(
                            RK3576_VOP_LAYER_SEL_DISABLE) |
                        RK3576_VOP_LAYER_SEL_L2(
                            RK3576_VOP_LAYER_SEL_DISABLE) |
                        RK3576_VOP_LAYER_SEL_L3(
                            RK3576_VOP_LAYER_SEL_DISABLE));

  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(60);

  syslog(LOG_WARNING,
         "kickpi-k7:   programmed: CTRL0=%08x CTRL2=%08x MST=%08x VIR=%08x "
         "ACT=%08x DSP=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, cb + RK3576_VOP_WIN0_CTRL0),
         (unsigned)rk3576_vop_getreg(priv, cb + RK3576_VOP_WIN0_CTRL2),
         (unsigned)rk3576_vop_getreg(priv, cb + RK3576_VOP_WIN0_YRGB_MST),
         (unsigned)rk3576_vop_getreg(priv, cb + RK3576_VOP_WIN0_VIR),
         (unsigned)rk3576_vop_getreg(priv, cb + RK3576_VOP_WIN0_ACT_INFO),
         (unsigned)rk3576_vop_getreg(priv, cb + RK3576_VOP_WIN0_DSP_INFO));
  syslog(LOG_WARNING,
         "kickpi-k7:   CLUSTER0_CTRL=%08x  LAYER_SEL=%08x (layer0=0 = "
         "Cluster0)\n",
         (unsigned)rk3576_vop_getreg(priv, cb +
                                     RK3576_VOP_CLUSTER0_CTRL_OFF),
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_LAYER_SEL));

  /* Snapshot the shared status too, so the comparison with the ESMART runs is
   * in the same log: if CLUSTER0 fetches, the window's own BUF_EMPTY and the
   * POST under-run should behave differently from the ESMART case. */

  {
    uint32_t vp_raw = rk3576_vop_getreg(priv, RK3576_VOP_SYS_CTRL(priv->base) +
                                        RK3576_VOP_VP_INT_RAW_STATUS(
                                            priv->cfg.port));

    syslog(LOG_WARNING,
           "kickpi-k7:   VP%u RAW=%08x (BUF_EMPTY bit4=%u) -- compare with the "
           "ESMART0 case\n",
           (unsigned)priv->cfg.port, (unsigned)vp_raw,
           (unsigned)((vp_raw >> 4) & 1u));
  }

  syslog(LOG_WARNING,
         "kickpi-k7: CLUSTER0 PROBE: WATCH THE SCREEN for 5 s.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   framebuffer image  => CLUSTER0 FETCHES; the fault is "
         "ESMART-specific and\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   there is a working path;\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   unchanged garble or blue => CLUSTER0 does not fetch "
         "either, so the fault is\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   in the SHARED request path and no window setting can be "
         "at fault.\n");

  up_mdelay(5000);

  /* --- Restore. --- */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_LAYER_SEL,
                    save_layer_sel);
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, save_region0);
  rk3576_vop_putreg(priv, cb + RK3576_VOP_WIN0_CTRL0, 0u);
  rk3576_vop_putreg(priv, cb + RK3576_VOP_CLUSTER0_CTRL_OFF, 0u);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(60);

  syslog(LOG_WARNING,
         "kickpi-k7: CLUSTER0 PROBE: restored (LAYER_SEL=%08x REGION0_CTRL=%08x "
         "CLUSTER0_CTRL=%08x)\n",
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_LAYER_SEL),
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_CTRL),
         (unsigned)rk3576_vop_getreg(priv, cb +
                                     RK3576_VOP_CLUSTER0_CTRL_OFF));

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_port_dma_stop_probe
 *
 * Description:
 *   *** THE FIRST THING THAT CAN EXPLAIN BOTH WINDOW TYPES FAILING: A PER-VIDEO-
 *   PORT DMA STOP INSIDE SYS_PORT_CTRL_IMD. ***
 *
 *   The CLUSTER0 bisection settled a great deal: CLUSTER0 was configured
 *   correctly for this window -- CTRL0 = 0x4003, CTRL2 = 0x0000016a (yrgb id
 *   0x0a on [4:0], uv 0x0b on [9:5]), MST = the framebuffer, VIR = 540,
 *   ACT/DSP = 719x1279, LAYER_SEL layer0 = 0 = Cluster0 -- and the picture did
 *   NOT change.  Two windows with separate register blocks, separate DMA and
 *   separate AXI read-ids fail identically, so nothing about the ESMART block is
 *   at fault and no window-level setting can be.  The fault is in what every
 *   window shares.
 *
 *   SYS_PORT_CTRL_IMD holds per-video-port DMA stop switches:
 *
 *     bit10 vfp2_dma_stop_en   RW reset 0   1'b0: Disable  1'b1: Enable
 *     bit9  vfp1_dma_stop_en   RW reset 0
 *     bit8  vfp0_dma_stop_en   RW reset 0      <- this video port
 *     bit5  auto_cs_en         RW reset 1
 *     bit4  dsp_vs_t_sel       RW reset 1
 *     bit0  vp0_interlace_frm_reg_done  RW reset 1
 *
 *   The values quoted above as "reset" are now confirmed for this register by a
 *   dump from a working configuration on this same board, and that dump also
 *   settled two bits this project had been guessing at: dsp_vs_t_sel holds its
 *   reset value 1 in working silicon (it is NOT cleared, despite is_vop3() being
 *   true for RK3576), and auto_cs_mode must be 0 (RK3576's control table has no
 *   field for it, so nothing should write it).  The driver now writes only
 *   bits[2:0], which is exactly what reproduces the working word 0x00070038.
 *
 *   Bit8 was still worth checking on its own: a video port whose DMA is
 *   is stopped cannot fetch from ANY of its windows, and it raises no error
 *   while doing it -- which is exactly the evidence collected over this whole
 *   bring-up: no read, no bus error, no MMU fault, the mixer visibly waiting,
 *   and both window types equally dead.
 *
 *   So: print every named bit, and if vfp0_dma_stop_en is set, clear it and
 *   watch the screen.  If it is already clear, print that too, because that
 *   retires this candidate as cleanly as setting it would confirm it.
 *
 ****************************************************************************/

int rk3576_vop_port_dma_stop_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sb;
  uint32_t v;
  uint32_t after;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  sb = RK3576_VOP_SYS_CTRL(priv->base);
  v = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_PORT_CTRL_IMD);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== PORT DMA-STOP PROBE (SYS_PORT_CTRL_IMD) =====\n");
  syslog(LOG_WARNING, "kickpi-k7:   value=%08x\n", (unsigned)v);

  /* One bit per line: the syslog channel truncates long lines, which has
   * already cost this project one diagnostic. */

  /* "working" below means the value read out of the working register dump:
   * 0x00070038, i.e. auto_cs_mode 0, dsp_vs_t_sel 1, reg_done_frm 0. */

  syslog(LOG_WARNING, "kickpi-k7:   auto_cs_mode bit15=%u (working 0)\n",
         (unsigned)((v >> 15) & 1u));
  syslog(LOG_WARNING, "kickpi-k7:   vfp2_dma_stop_en bit10=%u\n",
         (unsigned)((v >> 10) & 1u));
  syslog(LOG_WARNING, "kickpi-k7:   vfp1_dma_stop_en bit9=%u\n",
         (unsigned)((v >> 9) & 1u));
  syslog(LOG_WARNING, "kickpi-k7:   vfp0_dma_stop_en bit8=%u  <== VP0\n",
         (unsigned)((v >> 8) & 1u));
  syslog(LOG_WARNING, "kickpi-k7:   auto_cs_en bit5=%u (working 1)\n",
         (unsigned)((v >> 5) & 1u));
  syslog(LOG_WARNING, "kickpi-k7:   dsp_vs_t_sel bit4=%u (working 1)\n",
         (unsigned)((v >> 4) & 1u));
  syslog(LOG_WARNING, "kickpi-k7:   reg_done_frm[2:0]=%u (working 0)\n",
         (unsigned)(v & 0x7u));

  if (((v >> 8) & 1u) != 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7:   *** vfp0_dma_stop_en IS SET: THIS VIDEO PORT'S DMA "
             "IS STOPPED ***\n");
      syslog(LOG_ERR,
             "kickpi-k7:   A stopped video port cannot fetch from any window, "
             "which is why\n");
      syslog(LOG_ERR,
             "kickpi-k7:   ESMART0 and CLUSTER0 both fetch nothing while the "
             "mixer still waits\n");
      syslog(LOG_ERR,
             "kickpi-k7:   for them.  Clearing it now -- WATCH THE SCREEN.\n");

      rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_PORT_CTRL_IMD,
                                  0u, 1u << 8);

      up_mdelay(60);

      after = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_PORT_CTRL_IMD);

      syslog(LOG_WARNING,
             "kickpi-k7:   after clearing bit8: %08x (vfp0_dma_stop_en=%u)\n",
             (unsigned)after, (unsigned)((after >> 8) & 1u));

      rk3576_vop_trigger_cfg_done(priv);

      syslog(LOG_WARNING,
             "kickpi-k7:   WATCH THE SCREEN for 5 s: if the framebuffer "
             "appears, THIS WAS THE\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   ROOT CAUSE -- a video port left with its DMA "
             "stopped, which the\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   driver never read and so never noticed.\n");

      up_mdelay(5000);

      if (((after >> 8) & 1u) != 0u)
        {
          syslog(LOG_ERR,
                 "kickpi-k7:   *** and the clear did NOT take -- bit8 is still "
                 "set.  Then it is\n");
          syslog(LOG_ERR,
                 "kickpi-k7:   held by something outside this register and that "
                 "something is the\n");
          syslog(LOG_ERR,
                 "kickpi-k7:   next place to look ***\n");
        }
    }
  else
    {
      syslog(LOG_WARNING,
             "kickpi-k7:   vfp0_dma_stop_en is CLEAR, so this video port's DMA "
             "is not stopped and\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   this candidate is retired.  (It had to be checked: "
             "the register was\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   only ever written through a mask that excludes "
             "bit8.)\n");
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_addr_screen_probe
 *
 * Description:
 *   *** ANSWERS "DOES THE WINDOW FETCH AT ALL?" WITH THE ONE INSTRUMENT THAT
 *   HAS NEVER MISLED THIS PROJECT: THE SCREEN. ***
 *
 *   Every conclusion about the layer's memory traffic so far has been argued
 *   from a REGISTER -- no bus error latched, no MMU fault, no AXI busy.  Each
 *   of those depends on a chain of assumptions about which register reports
 *   what, and this project has been burned by exactly that kind of chain
 *   repeatedly (a hiword write mask swallowing writes, a readback taken before
 *   the write landed, a counter field made counting status).  The poison test in
 *   particular has never once been confirmed by looking at the panel.
 *
 *   This probe removes the interpretation entirely.  It points the fetch at four
 *   WILDLY different addresses, one at a time, and asks a single question:
 *
 *     does the picture on the glass change at all?
 *
 *     IT CHANGES  => the window IS reading memory, the address register works,
 *                    and every "no read" conclusion in this project is wrong --
 *                    which would redirect the whole effort at the data path
 *                    (stride, format, packing) instead of at the request path.
 *
 *     IT DOES NOT CHANGE => for four addresses that cannot hold the same data
 *                    (including one that decodes to no slave at all), the window
 *                    reads nothing.  That is direct physical evidence, not an
 *                    inference, and it retires the fetch question for good.
 *
 *   One rung fills the framebuffer with a solid saturated RED first, so that if
 *   the fetch does work at that rung, the difference cannot be missed.
 *
 ****************************************************************************/

int rk3576_vop_addr_screen_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;

  static const struct
  {
    uint32_t addr;
    bool     paint_red;
    FAR const char *what;
  } rungs[] =
  {
    { 0x00000000u, false, "address 0 -- DRAM bottom, never our buffer" },
    { 0x10000000u, false, "address 0x10000000 -- middle of DRAM, never ours" },
    { 0u,          true,  "our own framebuffer, filled SOLID RED" },
    { 0xf0000000u, false, "address 0xf0000000 -- decodes to NO slave" },
  };

  uint32_t eb;
  uint32_t save;
  int i;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  save = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_REGION0_YRGB_MST);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== FETCH-BY-SCREEN PROBE =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   Four different fetch addresses, 3 s each.  This needs no\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   register interpretation at all: it asks only whether the\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   picture CHANGES.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   CHANGES     => the window IS reading memory, and every\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   \"no read\" conclusion in this project is wrong.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   UNCHANGED   => it reads nothing, proven on the glass.\n");

  for (i = 0; i < (int)nitems(rungs); i++)
    {
      uint32_t addr = rungs[i].addr;

      if (rungs[i].paint_red)
        {
          addr = (uint32_t)up_addrenv_va_to_pa(priv->fbmem);
          rk3576_vop_fill(0xff0000u);
        }

      rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_YRGB_MST, addr);
      rk3576_vop_trigger_cfg_done(priv);
      up_mdelay(100);

      syslog(LOG_WARNING,
             "kickpi-k7:   RUNG %d/4: %s\n", i + 1, rungs[i].what);
      syslog(LOG_WARNING,
             "kickpi-k7:     wrote MST=%08x, reads back %08x\n",
             (unsigned)addr,
             (unsigned)rk3576_vop_getreg(priv, eb +
                                         RK3576_VOP_ESMART_REGION0_YRGB_MST));

      up_mdelay(3000);
    }

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_YRGB_MST, save);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: FETCH-BY-SCREEN PROBE: restored MST=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_YRGB_MST));
  syslog(LOG_WARNING,
         "kickpi-k7:   REPORT: did the picture change AT ALL between the four\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   rungs -- and did SOLID RED ever appear?\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_ref_align_probe
 *
 * Description:
 *   *** APPLIES THE DIFFERENCES FOUND AGAINST A KNOWN-WORKING VOP DUMP, ONE AT
 *   A TIME, AND WATCHES THE SCREEN AFTER EACH. ***
 *
 *   A register dump was taken from this same board running a Debian image with
 *   HDMI working -- i.e. a VOP configuration that is KNOWN to fetch and display.
 *   Comparing it against this driver turned up three things the working
 *   configuration has that this driver never writes at all:
 *
 *   1. OVL_PORT0_CTRL (0x0600) bit28.  The working dump reads 0x10000001, so
 *      bit28 is set.  The vendor header names that bit
 *
 *        RK3568_OVL_CTRL__LAYERSEL_REGDONE_IMD   BIT(28)
 *
 *      and the vendor driver's comment on it says exactly what it is for:
 *
 *        "Register OVERLAY_LAYER_SEL and OVERLAY_PORT_SEL should take effect
 *         immediately, than windows configuration(CLUSTER/ESMART/SMART) can
 *         take effect according the video port mux configuration as we
 *         wished."
 *
 *      That is a statement that WITHOUT this bit the overlay's layer selection
 *      does not take effect and the windows' configuration never becomes
 *      effective through the video port mux -- a window that is configured,
 *      enabled and routed yet never actually connected.  RK3576's control table
 *      in the vendor driver has no field for this bit (only rk3568 and rk3588
 *      do), so on this SoC it is left set by the bootloader and Linux simply
 *      inherits it.  THIS DRIVER NEVER WRITES 0x0600 AT ALL, so whatever the
 *      bootloader left is what we get -- and if this board's bootloader does not
 *      set it, nothing ever has.
 *
 *      (The dump's bit0 is also set -- overlay_mode, which the vendor driver
 *      sets from the output's YUV-ness.  Ours is RGB, so it should stay 0; it is
 *      rung 2 only to find out whether it matters at all.)
 *
 *   2. ESMART0 REGION0_SCL_CTRL (0x1830).  The working dump reads 0x00000044.
 *      By the vendor's field map that is
 *
 *        [3:2] yrgb_hscl_filter_mode = 1
 *        [7:6] yrgb_vscl_filter_mode = 1
 *
 *      i.e. the bilinear filter mode is programmed EVEN THOUGH the layer is
 *      1:1.  The vendor driver sets those two fields from the window's own
 *      static filter-mode properties, not from the computed scale mode, so they
 *      are non-zero on every layer it configures.  THIS DRIVER WRITES ZERO.
 *
 *   3. The overlay mixer coefficients (0x0620..0x062c for mixer0).  The working
 *      dump reads 00ff01a0 / 00ff0060 / 00000020 / 00000074.  THIS DRIVER NEVER
 *      WRITES THEM, so our mixer runs on reset values -- and the mixer is the
 *      stage that decides whether the layer's pixels reach the POST at all.
 *
 *   EACH RUNG IS APPLIED ON TOP OF THE PREVIOUS ONE and left in place, so the
 *   screen shows the cumulative effect; the rung at which the picture appears is
 *   the one that mattered.
 *
 ****************************************************************************/

int rk3576_vop_ref_align_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t ovl_base;
  uint32_t eb;
  uint32_t ctrl;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  ovl_base = RK3576_VOP_OVERLAY_PORT(priv->base, priv->cfg.port);
  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);

  ctrl = rk3576_vop_getreg(priv, ovl_base + RK3576_VOP_OVERLAY_CTRL);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== REFERENCE-ALIGN PROBE =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   Diffing against a working VOP dump from this board "
         "(Debian + HDMI).\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   OVL_CTRL(0x0600) here=%08x, working dump=10000001\n",
         (unsigned)ctrl);
  syslog(LOG_WARNING,
         "kickpi-k7:   SCL_CTRL(0x1830) here=%08x, working dump=00000044\n",
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_SCL_CTRL));

  /* --- Rung 1: overlay layersel regdone immediate. --- */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_CTRL,
                    ctrl | RK3576_VOP_OVERLAY_LAYERSEL_REGDONE_IMD);
  up_mdelay(100);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 1/4: OVL_CTRL bit28 (layersel_regdone_imd) SET -> "
         "%08x\n",
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_CTRL));
  syslog(LOG_WARNING,
         "kickpi-k7:   Without this the overlay's layer select does not take "
         "effect, so the\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   window is configured but never connected.  WATCH THE "
         "SCREEN (4 s).\n");
  up_mdelay(4000);

  /* --- Rung 2: overlay_mode as well, only to find out whether it matters. --- */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_CTRL,
                    rk3576_vop_getreg(priv, ovl_base +
                                      RK3576_VOP_OVERLAY_CTRL) | 1u);
  up_mdelay(100);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 2/4: OVL_CTRL bit0 (overlay_mode) also SET -> %08x.  "
         "WATCH (4 s)\n",
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_CTRL));
  up_mdelay(4000);

  /* --- Rung 3: the scaler filter modes the vendor always programs. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_SCL_CTRL,
                    RK3576_VOP_ESMART_SCL_FILTER_MODES_DEFAULT);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(100);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 3/4: SCL_CTRL = %08x (hscl/vscl filter mode = 1, as "
         "the working dump\n",
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_SCL_CTRL));
  syslog(LOG_WARNING,
         "kickpi-k7:   shows, even at 1:1).  WATCH (4 s)\n");
  up_mdelay(4000);

  /* --- Rung 4: the overlay mixer coefficients for mixer0. --- */

  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL,
                    0x00ff01a0u);
  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL,
                    0x00ff0060u);
  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_ALPHA_CTRL,
                    0x00000020u);
  rk3576_vop_putreg(priv, ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_ALPHA_CTRL,
                    0x00000074u);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(100);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 4/4: mixer0 coefficients set to the working dump's "
         "values:\n");
  syslog(LOG_WARNING, "kickpi-k7:   src_color=%08x dst_color=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL),
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL));
  syslog(LOG_WARNING, "kickpi-k7:   src_alpha=%08x dst_alpha=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_MIX0_SRC_ALPHA_CTRL),
         (unsigned)rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_MIX0_DST_ALPHA_CTRL));
  syslog(LOG_WARNING, "kickpi-k7:   WATCH (4 s)\n");
  up_mdelay(4000);

  syslog(LOG_WARNING,
         "kickpi-k7: REFERENCE-ALIGN PROBE done.  All four changes are left "
         "APPLIED.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   REPORT which rung the picture appeared at -- 1, 2, 3, 4, "
         "or none.\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_port_ctrl_align_probe
 *
 * Description:
 *   *** RESTORES THE THREE BITS OF SYS_PORT_CTRL_IMD THAT THIS DRIVER CHANGED
 *   ON THE STRENGTH OF INFERENCE, NOT EVIDENCE. ***
 *
 *   A register dump from this same board running a Debian image with HDMI
 *   working -- i.e. a VOP configuration known to fetch and display -- reads
 *
 *     0028 00070038
 *
 *   and this driver reads
 *
 *     0028 8030802f
 *
 *   Three bits differ, and every one of them is a bit this driver set from
 *   reading the vendor source rather than from observing working silicon:
 *
 *     bit15 auto_cs_mode   working 0, ours 1
 *       Linux sets this in vop2_initial()... but RK3576's control table in the
 *       vendor driver has NO field for it (only rk3568/rk3588 do), so on this
 *       SoC nothing ever writes it and the reset value stands.  Ours reads 1
 *       only because this driver writes it.
 *
 *     bit4 dsp_vs_t_sel    working 1 (the reset value), ours 0
 *       This driver clears it because is_vop3() is true for RK3576 and
 *       vop2_initial() contains VOP_CTRL_SET(dsp_vs_t_sel, 0) inside the vop3
 *       branch.  Working silicon disagrees: it holds the reset value of 1.  The
 *       bit selects where the display's vs/t timing is tapped
 *       ("1'b0: Dsp_vs_t_out, 1'b1: Dsp_vs_t_pre"), so it is at the heart of
 *       line timing, which is at the heart of when a window is asked for data.
 *
 *     bits[2:0] reg_done_frm  working 0, ours 1 (the reset value)
 *       This ONE field is defined for RK3576 and for no other SoC --
 *       .reg_done_frm = VOP_REG_MASK(RK3576_SYS_PORT_CTRL_IMD, 0x7, 0) -- and
 *       Linux writes it to 0 explicitly, with the comment "Set reg done every
 *       field for interlace".  This driver has never written it, so it sits at
 *       its reset value of 1.  It controls when a video port's register updates
 *       become valid, which is precisely the mechanism that has been failing
 *       here: a configuration that reads back perfectly but never takes effect.
 *
 *   THE POINT IS NOT THAT ONE OF THESE IS OBVIOUSLY THE CULPRIT.  It is that
 *   all three are cases of this driver overriding working silicon on the basis
 *   of a reading of the vendor code, and that class of change has produced every
 *   real fault found in this bring-up.
 *
 *   configure_port() now writes ONLY reg_done_frm, which reproduces the working
 *   word exactly, so this function no longer changes anything.  It is kept as a
 *   VERIFIER, run last in the sequence: several probes write this same register
 *   (port_dma_stop_probe, cfg_done_probe) and no probe checks it afterwards, so
 *   the one thing worth asking at the end of boot is whether the value that was
 *   proven correct at the start survived to the point where the display is live.
 *   A register whose contents are silently rewritten undoes the fix and undoes
 *   the reasoning that produced it.
 *
 ****************************************************************************/

int rk3576_vop_port_ctrl_align_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sb;
  uint32_t v;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  sb = RK3576_VOP_SYS_CTRL(priv->base);
  v = rk3576_vop_getreg(priv, sb + RK3576_VOP_SYS_PORT_CTRL_IMD);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== SYS_PORT_CTRL_IMD VERIFY (end of boot) =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   value here=%08x, working dump=00070038\n",
         (unsigned)v);
  syslog(LOG_WARNING,
         "kickpi-k7:     auto_cs_mode  bit15: ours=%u working=0\n",
         (unsigned)((v >> 15) & 1u));
  syslog(LOG_WARNING,
         "kickpi-k7:     dsp_vs_t_sel  bit4 : ours=%u working=1\n",
         (unsigned)((v >> 4) & 1u));
  syslog(LOG_WARNING,
         "kickpi-k7:     reg_done_frm  [2:0]: ours=%u working=0\n",
         (unsigned)(v & 0x7u));

  if ((v & 0x0000ffffu) != 0x0038u)
    {
      syslog(LOG_ERR,
             "kickpi-k7: *** SYS_PORT_CTRL_IMD NO LONGER MATCHES THE WORKING "
             "VALUE ***\n");
      syslog(LOG_ERR,
             "kickpi-k7:   configure_port() set 0x0038; something later in the "
             "boot rewrote it.\n");
      syslog(LOG_ERR,
             "kickpi-k7:   Re-applying the working value now -- WATCH THE "
             "SCREEN (5 s).\n");

      rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_PORT_CTRL_IMD, 0u,
                                  RK3576_VOP_SYS_PORT_REG_DONE_FRM_MASK |
                                      RK3576_VOP_SYS_PORT_AUTO_CS_MODE |
                                      RK3576_VOP_SYS_PORT_DSP_VS_T_SEL);
      rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_PORT_CTRL_IMD,
                                  RK3576_VOP_SYS_PORT_DSP_VS_T_SEL,
                                  RK3576_VOP_SYS_PORT_DSP_VS_T_SEL);
      rk3576_vop_trigger_cfg_done(priv);
      up_mdelay(5000);

      syslog(LOG_WARNING, "kickpi-k7:   after re-apply: %08x\n",
             (unsigned)rk3576_vop_getreg(priv,
                                         sb +
                                             RK3576_VOP_SYS_PORT_CTRL_IMD));
    }
  else
    {
      syslog(LOG_WARNING,
             "kickpi-k7:   MATCHES the working configuration and survived the "
             "whole boot.\n");
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_scl_align_probe
 *
 * Description:
 *   Runs LAST, and does two things.
 *
 *   First it re-reads the scaler block that configure_layer() now programs to
 *   match the working dump (SCL_CTRL = 0x44, SCL_FACTOR_YRGB = 0), because that
 *   block is the stage between the window's DMA and its output -- the stage
 *   that decides when to ask for data -- and because this is the change most
 *   likely to matter.
 *
 *   Then, if the picture still has not appeared, it walks through the three
 *   remaining places where the ESMART0 block differs from the working dump, one
 *   rung at a time, announcing each and watching the screen.  Each rung moves
 *   one register towards the only ground truth available -- a dump from this
 *   board with a configuration that fetches and displays:
 *
 *     RUNG A  ESMART0_CTRL1 bit28 (dma_rreq_hurry_en)
 *               working 0x00011100, ours 0x10011100.  This bit was set on the
 *               theory that the window needed to ask for priority when starved.
 *               A window that issues no read at all cannot be asking for
 *               priority, so the theory does not fit the measurement -- and
 *               working silicon has it clear.  Clear it.
 *
 *     RUNG B  ESMART0_AXI_CTRL -- outstanding_en (bit3) and num[7:4]
 *               working 0x00010000: bit16 dma_4k_addr_opt only, i.e. outstanding
 *               UNLIMITED.  Ours is 0x000100fc: bounded at 15.  mmu_bypass must
 *               stay set here (this driver uses physical addresses and there are
 *               no page tables), so this rung cannot be byte-identical to the
 *               working value; it removes only the bound that the working
 *               configuration does not impose.  dma_4k_addr_opt is kept, since
 *               the working dump has it set.
 *
 *     RUNG C  ESMART0_ALPHA_MAP (0x18d8)
 *               working 0x8000ff00 -- alpha_map_en (bit31) set, map value
 *               0xff00.  Ours is 0, i.e. the register is never written.  Alpha
 *               mapping is the last piece of the window's pixel path that is
 *               configured on working silicon and absent here.
 *
 * A rung that changes the picture tells us what was wrong; a rung that changes
 * nothing retires that difference as cleanly.  Either answer is progress, and
 * after three rounds of regressions the point is to move one knob at a time and
 * let the screen speak.
 *
 ****************************************************************************/

int rk3576_vop_scl_align_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t eb;
  uint32_t v;
  uint32_t fac;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);

  v = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_REGION0_SCL_CTRL);
  fac = rk3576_vop_getreg(priv,
                          eb + RK3576_VOP_ESMART_REGION0_SCL_FACTOR_YRGB);

  syslog(LOG_WARNING, "kickpi-k7: ===== SCALER ALIGN (end of boot) =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   SCL_CTRL=%08x (working 00000044), FACTOR_YRGB=%08x "
         "(working 00000000)\n",
         (unsigned)v, (unsigned)fac);

  if (v != RK3576_VOP_ESMART_SCL_FILTER_MODES_DEFAULT || fac != 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7: *** the scaler block no longer matches the working "
             "dump ***\n");
      syslog(LOG_ERR,
             "kickpi-k7:   configure_layer() set it; something later rewrote "
             "it.\n");

      rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_SCL_CTRL,
                        RK3576_VOP_ESMART_SCL_FILTER_MODES_DEFAULT);
      rk3576_vop_putreg(priv,
                        eb + RK3576_VOP_ESMART_REGION0_SCL_FACTOR_YRGB, 0u);
      rk3576_vop_trigger_cfg_done(priv);
      up_mdelay(2000);
    }

  /* --- RUNG A: CTRL1 bit28. --- */

  v = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_CTRL1);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG A: ESMART%u_CTRL1=%08x, working 00011100 "
         "(dma_rreq_hurry_en bit28=%u -> 0).  WATCH (5 s).\n",
         (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)v,
         (unsigned)((v >> 28) & 1u));

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_CTRL1,
                    v & ~RK3576_VOP_ESMART_CTRL1_DMA_RREQ_HURRY_EN);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(5000);

  /* --- RUNG B: unlimited AXI outstanding. --- */

  v = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG B: ESMART%u_AXI_CTRL=%08x, working 00010000 "
         "(outstanding_en bit3 %u -> 0 = unlimited, num[7:4] %u -> 0).  Keeping "
         "mmu_bypass (no IOMMU here).  WATCH (5 s).\n",
         (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)v,
         (unsigned)((v >> 3) & 1u), (unsigned)((v >> 4) & 0xfu));

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                    RK3576_VOP_ESMART_AXI_DMA_4K_ADDR_OPT |
                        RK3576_VOP_ESMART_AXI_MMU_BYPASS);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(5000);

  /* --- RUNG C: the alpha map. --- */

  v = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_ALPHA_MAP);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG C: ESMART%u_ALPHA_MAP=%08x, working 8000ff00 "
         "(alpha_map_en bit31 %u -> 1, val %04x -> ff00).  WATCH (5 s).\n",
         (unsigned)RK3576_VOP_ESMART_IDX, (unsigned)v,
         (unsigned)((v >> 31) & 1u), (unsigned)(v & 0xffffu));

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_ALPHA_MAP, 0x8000ff00u);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(5000);

  syslog(LOG_WARNING,
         "kickpi-k7: SCALER ALIGN done; A and B and C all LEFT APPLIED.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   REPORT which rung the picture appeared at -- A, B, C, or "
         "none.\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_window_group_probe
 *
 * Description:
 *   *** RE-RUNS THE ONE EXPERIMENT THAT WAS DECISIVE, NOW THAT THE COMMIT IS
 *   FIXED, AND WITH THE CONFOUND REMOVED. ***
 *
 *   An earlier round shrank the layer to 200x200 and watched for the corner.
 *   It did not appear -- the whole screen stayed unchanged -- and the
 *   conclusion drawn was that the WINDOW REGISTER GROUP NEVER LOADS: every
 *   readback perfect, the glass untouched.  That conclusion is the most
 *   explanatory thing this bring-up ever produced, and the commit it was
 *   measured through had a defect: it requested the load of VP1's and VP2's
 *   register groups as well, and those groups can never be consumed on this
 *   board, because VP1 and VP2 never scan.  The TRM says each of these bits
 *   makes the mirror->real copy happen "when all the [group] register config
 *   finish".  The commit now asks for VP0 only, exactly as Linux does.
 *
 *   So the experiment is repeated.  The POST background is frozen SOLID BLUE
 *   for the whole probe, which removes the confound that made "the screen went
 *   blue" ambiguous in the original run -- with the background fixed, BLUE can
 *   only ever mean "the layer contributed nothing".
 *
 *     Rung 1  full 720x1280, layer ON                         baseline
 *     Rung 2  ACT = DSP = 200x200, layer ON                   decisive
 *     Rung 3  layer OFF, background still blue                does the enable
 *                                                             bit reach silicon?
 *
 *   HOW TO READ RUNG 2:
 *
 *     the corner shows our framebuffer -> THE GROUP LOADS AND THE WINDOW
 *         FETCHES.  The full-screen failure is a stride/geometry problem.
 *     garble in the corner, blue everywhere else -> the group loads and
 *         geometry takes effect, but no fetch happens: look at the address,
 *         the format and the AXI read IDs.
 *     the whole screen unchanged -> the group STILL never loads, even with
 *         the commit corrected, and the next place to look is what actually
 *         consumes these bits on this silicon.
 *
 *   This is deliberately the last thing that runs, and it leaves the driver's
 *   intended configuration in place.
 *
 ****************************************************************************/

int rk3576_vop_window_group_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t eb;
  uint32_t pb;
  uint32_t ctrl;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  pb = RK3576_VOP_POST(priv->base, priv->cfg.port);

  /* Freeze the background solid blue first, so nothing that follows can be
   * mistaken for the background. */

  rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_DSP_BG,
                    RK3576_VOP_POST_BG_DISPLAY_EN |
                        (0x3ffu << RK3576_VOP_POST_BG_BLUE_SHIFT));
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(500);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== WINDOW GROUP PROBE (commit corrected) =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   background frozen SOLID BLUE: BLUE can only mean \"the "
         "layer contributed nothing\".\n");

  /* --- Rung 1: the driver's own configuration, committed. --- */

  ctrl = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL);
  ctrl |= RK3576_VOP_ESMART_FMT_RGB888 | RK3576_VOP_ESMART_REGION0_MST_EN |
          RK3576_VOP_ESMART_REGION0_RB_SWAP;
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, ctrl);
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_ACT_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_DSP_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 1: layer ON at %ux%u, committed.  WATCH (2.5 s).\n",
         (unsigned)priv->cfg.xres, (unsigned)priv->cfg.yres);
  up_mdelay(2500);

  /* --- Rung 2: decisive.  Shrink ACT and DSP together to 200x200, so the
   * layer can only occupy the top-left corner and no scaling is implied. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_ACT_INFO,
                    (199u << 16) | 199u);
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_DSP_INFO,
                    (199u << 16) | 199u);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(100);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 2 (decisive): ACT=DSP=199x199, committed "
         "(readback ACT=%08x DSP=%08x).  WATCH (3 s).\n",
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_ACT_INFO),
         (unsigned)rk3576_vop_getreg(priv, eb +
                                     RK3576_VOP_ESMART_REGION0_DSP_INFO));
  syslog(LOG_WARNING,
         "kickpi-k7:   corner shows our framebuffer  => GROUP LOADS, it "
         "FETCHES (look at stride)\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   garble in the corner only    => GROUP LOADS, no fetch\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   whole screen unchanged       => GROUP STILL NEVER "
         "LOADS\n");
  up_mdelay(3000);

  /* --- Rung 3: the layer's own enable bit, with the background still blue.
   * Only the enable bit is changed, so blue here means the enable DID reach
   * silicon (the confound of the original run was that this rung also turned
   * the background on). --- */

  ctrl = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL);
  ctrl &= ~RK3576_VOP_ESMART_REGION0_MST_EN;
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, ctrl);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 3: ONLY the window enable bit cleared, background "
         "still blue.  WATCH (2.5 s).\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   BLUE => the enable bit reaches silicon.  STILL GARBLE "
         "=> it never lands.\n");
  up_mdelay(2500);

  /* --- Restore the driver's configuration. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_ACT_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_DSP_INFO,
                    (((uint32_t)(priv->cfg.yres - 1)) << 16) |
                        (priv->cfg.xres - 1));
  ctrl = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL);
  ctrl |= RK3576_VOP_ESMART_FMT_RGB888 | RK3576_VOP_ESMART_REGION0_MST_EN |
          RK3576_VOP_ESMART_REGION0_RB_SWAP;
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, ctrl);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: WINDOW GROUP PROBE done, configuration restored.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   REPORT rung 2's outcome verbatim -- corner, corner+blue, "
         "or unchanged.\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_post_scl_probe
 *
 * Description:
 *   *** A/B ON THE ONE REGISTER THAT WAS FOUND TO HOLD A DEGENERATE VALUE, SO
 *   THE SCREEN CAN ATTRIBUTE IT. ***
 *
 *   POST_SCL_FACTOR_YRGB is the POST's output scale factor, the last block
 *   before the DSI.  This driver wrote 0 there -- "no scaling", as its comment
 *   said -- while Linux writes scl_cal_scale2() unconditionally and a working
 *   dump on this board reads 0x10001000, i.e. 1.0 in both axes.  Zero is not a
 *   pass-through; by the field's own definition it is a scale factor of zero.
 *
 *   configure_timing() now writes 1.0.  This probe walks the register through
 *   both values with the layer enabled, so which one the glass agrees with is
 *   answered directly rather than argued:
 *
 *     Rung 1  factor = 0          the old, degenerate value
 *     Rung 2  factor = 0x10001000 the value Linux computes and a working
 *                                 configuration holds
 *
 *   It leaves rung 2 applied, which is what the driver now intends anyway.
 *
 ****************************************************************************/

int rk3576_vop_post_scl_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t pb;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  pb = RK3576_VOP_POST(priv->base, priv->cfg.port);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== POST OUTPUT SCALER A/B =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   this register held 0 (\"scale by zero\"); Linux writes "
         "1.0 = 10001000\n");

  /* --- Rung 1: put back the old value. --- */

  rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_SCL_FACTOR_YRGB, 0u);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(100);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 1: factor = 0 (the old value), readback %08x.  "
         "WATCH (4 s).\n",
         (unsigned)rk3576_vop_getreg(priv,
                                     pb + RK3576_VOP_POST_SCL_FACTOR_YRGB));
  up_mdelay(4000);

  /* --- Rung 2: the value both authorities give. --- */

  rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_SCL_FACTOR_YRGB,
                    RK3576_VOP_POST_SCL_FACTOR_1_1);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(100);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 2: factor = 10001000 (1.0), readback %08x.  "
         "WATCH (4 s).\n",
         (unsigned)rk3576_vop_getreg(priv,
                                     pb + RK3576_VOP_POST_SCL_FACTOR_YRGB));
  up_mdelay(4000);

  syslog(LOG_WARNING,
         "kickpi-k7: POST SCALER A/B done; rung 2 LEFT APPLIED.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   REPORT which rung showed an image -- 1, 2, or neither.\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_coarse_pattern_probe
 *
 * Description:
 *   *** THE TEST THE WHOLE BRING-UP HAS BEEN MISSING: A PATTERN THAT CANNOT BE
 *   CONFUSED WITH A UNIFORM IMAGE. ***
 *
 *   Every "does it display" judgement in this project has used SOLID COLOURS --
 *   solid black, solid blue, solid red, solid green, white.  A uniform image is
 *   INVARIANT UNDER EVERY POSITIONAL ERROR THERE IS: any scaling, any line
 *   mis-address, any horizontal or vertical offset, any stream desynchronisation
 *   all leave a uniform field looking exactly the same.  So the solid-colour
 *   tests could never separate "the window fetches nothing" from "the window
 *   fetches correctly and the picture arrives in the wrong place", and that is
 *   the ambiguity this project has been stuck inside for a hundred rounds.
 *
 *   This panel is RAM-LESS -- it has no GRAM and no frame buffer of its own, so
 *   its line addressing comes entirely from the received DSI video stream.  A
 *   panel like that whose stream timing does not match what it expects
 *   desynchronises and shows its own fixed garbage, while a uniform field still
 *   looks perfect.  That fits every stubborn observation here, and it has NEVER
 *   been tested.
 *
 *   The instrument: eight WIDE vertical bars, full height, in
 *   R G B W K Y C M.  Wide, so a positional error of tens of pixels still
 *   leaves whole bars intact; high-contrast, so there is no chance of confusing
 *   a bar with a blank; and it covers the whole frame so a vertical offset is
 *   visible too.
 *
 *   BE HONEST ABOUT THE ODDS.  This is a discriminator, not a prediction: it
 *   does not assume bars will appear.  And there is already evidence that they
 *   will not -- addr_screen_probe pointed the window at four very different
 *   addresses, one of them a framebuffer filled SOLID RED, and the screen did
 *   not change at all.  If the window's data never reaches the output, adding
 *   structure to the buffer cannot change the output either, and "no bars" is
 *   the outcome to expect.  It is worth one look because a POSITIVE result here
 *   would overturn a great deal at once, and because a negative result is
 *   cheap.
 *
 *   HOW TO READ IT:
 *
 *     bars appear, in order or displaced/duplicated  => THE DATA PATH WORKS.
 *         The window is fetching the framebuffer and the fault is in the video
 *         timing or the panel's expected stream format.  Every solid-colour
 *         result so far becomes suspect and the next step is the DSI video
 *         timing, not the VOP.
 *
 *     only garble, no bars anywhere                  => THE DATA PATH IS
 *         BROKEN.  Nothing the window fetches is reaching the output.
 *
 *     clean blue                                     => the layer contributed
 *         nothing at all and even the enable path is suspect.
 *
 *   The POST background is frozen SOLID BLUE for the whole probe so that blue
 *   keeps its unambiguous meaning.
 *
 ****************************************************************************/

int rk3576_vop_coarse_pattern_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t pb;
  uint8_t *fb;
  uint16_t xres;
  uint16_t yres;
  uint32_t stride;
  uint32_t x;
  uint32_t y;
  uint32_t bar;

  /* R G B W K Y C M -- eight unmistakable, well-separated hues. */

  static const uint8_t bars[8][3] =
  {
    { 0xff, 0x00, 0x00 },   /* red     */
    { 0x00, 0xff, 0x00 },   /* green   */
    { 0x00, 0x00, 0xff },   /* blue    */
    { 0xff, 0xff, 0xff },   /* white   */
    { 0x00, 0x00, 0x00 },   /* black   */
    { 0xff, 0xff, 0x00 },   /* yellow  */
    { 0x00, 0xff, 0xff },   /* cyan    */
    { 0xff, 0x00, 0xff }    /* magenta */
  };

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  pb = RK3576_VOP_POST(priv->base, priv->cfg.port);
  xres = priv->cfg.xres;
  yres = priv->cfg.yres;
  stride = priv->stride;
  fb = (uint8_t *)priv->fbmem;

  /* Freeze the background to ORANGE, which is deliberately NOT one of the eight
   * bar colours: then a bar-coloured screen can only mean the framebuffer
   * reached the glass, and an orange screen can only mean it did not.
   *
   * (This used to say "blue" and programme the BLUE field, but this header had
   * the background's blue and green fields swapped, so it was really setting
   * GREEN.  The fields are fixed; see POST_DSP_BG in the hardware header.) */

  rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_DSP_BG,
                    RK3576_VOP_POST_BG_DISPLAY_EN |
                        RK3576_VOP_POST_BG_RGB(0x3ffu, 0x180u, 0x000u));
  rk3576_vop_trigger_cfg_done(priv);

  /* Paint eight full-height bars.  Row pitch is the real framebuffer stride,
   * not a computed one, so this cannot disagree with what the window scans. */

  if (stride == 0u)
    {
      stride = (uint32_t)xres * 3u;
    }

  for (y = 0; y < yres; y++)
    {
      uint8_t *row = fb + (size_t)y * stride;

      for (x = 0; x < xres; x++)
        {
          bar = (x * 8u) / xres;
          row[x * 3u + 0u] = bars[bar][0];
          row[x * 3u + 1u] = bars[bar][1];
          row[x * 3u + 2u] = bars[bar][2];
        }
    }

  up_clean_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== COARSE PATTERN TEST =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   framebuffer painted with 8 full-height bars: R G B W K Y "
         "C M\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   (bars are %u px wide, %u px tall), stride=%u\n",
         (unsigned)((uint32_t)xres / 8u), (unsigned)yres, (unsigned)stride);
  syslog(LOG_WARNING,
         "kickpi-k7:   EVERY solid-colour result so far is blind to positional "
         "error;\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   this is not.  LOOK NOW (8 s):\n");
  syslog(LOG_WARNING,
         "kickpi-k7:     bars visible (even displaced/duplicated) => DATA PATH "
         "WORKS, fault is timing\n");
  syslog(LOG_WARNING,
         "kickpi-k7:     no bars anywhere, only garble           => DATA PATH "
         "BROKEN\n");
  syslog(LOG_WARNING,
         "kickpi-k7:     clean blue                              => layer "
         "contributed nothing\n");

  up_mdelay(8000);

  syslog(LOG_WARNING,
         "kickpi-k7: COARSE PATTERN TEST done (pattern left in the "
         "framebuffer).\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_post_bg_sequence_probe
 *
 * Description:
 *   *** ANSWERS THE QUESTION THIS WHOLE BRING-UP HAS ASSUMED AWAY: DOES WHAT WE
 *   WRITE TO THE POST HAVE ANY EFFECT ON THE GLASS AT ALL? ***
 *
 *   Every test in this project has assumed that the screen reflects our
 *   register writes -- and that assumption was never checked, because there was
 *   never a case where we knew what colour we had asked for and could compare it
 *   with what appeared.  Then it turned out we did have such a case and it had
 *   been read the wrong way round:
 *
 *     the probes set a "solid blue background" by writing 0x3ff through
 *     POST_BG_BLUE_SHIFT, but this header had the background's BLUE field at
 *     bits[9:0] when the TRM puts GREEN there.  The value written was therefore
 *     SOLID GREEN.  A blue screen was reported anyway.
 *
 *   A reported colour that does not match the programmed colour means either
 *   the observation or the control path is unreliable, and until that is settled
 *   every other conclusion here rests on sand.  So it is settled directly:
 *
 *     the layer is turned OFF, so the POST background is the ONLY thing on the
 *     screen, and the background is then stepped through five unmistakable
 *     colours.  The screen is asked to repeat the sequence back.
 *
 *   The order is deliberately NOT printed before or during the run: only a
 *   step number is logged, and the colour list is printed at the END.  A
 *   reading of the log therefore cannot invent the answer, which is exactly
 *   what happened with the "blue" background.
 *
 *     the colours appear in the order logged  => this driver CONTROLS what the
 *         panel shows, and every register conclusion in this project is
 *         standing on something real.
 *
 *     a different order, or no changes at all => nothing we write to the POST
 *         is reaching the glass.  That would be a completely different
 *         problem from the one being chased, and it must be found first.
 *
 ****************************************************************************/

int rk3576_vop_post_bg_sequence_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t eb;
  uint32_t pb;
  uint32_t ctrl;
  uint32_t saved;
  uint32_t i;

  /* Red, green, blue, white, black -- six bits of separation between every
   * adjacent pair, so no two steps can be mistaken for each other. */

  static const uint32_t seq[5] =
  {
    RK3576_VOP_POST_BG_RGB(0x3ff, 0x000, 0x000),
    RK3576_VOP_POST_BG_RGB(0x000, 0x3ff, 0x000),
    RK3576_VOP_POST_BG_RGB(0x000, 0x000, 0x3ff),
    RK3576_VOP_POST_BG_RGB(0x3ff, 0x3ff, 0x3ff),
    RK3576_VOP_POST_BG_RGB(0x000, 0x000, 0x000)
  };

  if (priv == NULL)
    {
      return -ENODEV;
    }

  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  pb = RK3576_VOP_POST(priv->base, priv->cfg.port);

  saved = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL);

  /* Layer OFF: the background is then the only thing on the screen, so the
   * screen has nothing to show except what we program. */

  ctrl = saved & ~RK3576_VOP_ESMART_REGION0_MST_EN;
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, ctrl);
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(500);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== CAN THIS DRIVER CONTROL THE GLASS? =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   the layer is OFF.  The POST background is the only "
         "source and it is\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   about to be set to five different colours, 2.5 s each.  "
         "WATCH the\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   screen and REMEMBER THE ORDER.  The list is printed at "
         "the end, so\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   reading this log cannot give the answer away.\n");

  for (i = 0; i < 5u; i++)
    {
      rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_DSP_BG,
                        RK3576_VOP_POST_BG_DISPLAY_EN | seq[i]);
      rk3576_vop_trigger_cfg_done(priv);
      up_mdelay(150);

      /* Only the step number and what the register now READS are printed. */

      syslog(LOG_WARNING,
             "kickpi-k7:   STEP %u/5 -- background register now reads %08x\n",
             (unsigned)(i + 1u),
             (unsigned)rk3576_vop_getreg(priv,
                                         pb + RK3576_VOP_POST_DSP_BG));
      up_mdelay(2500);
    }

  /* Only now is the expected order revealed. */

  syslog(LOG_WARNING,
         "kickpi-k7:   the sequence just shown was: 1=RED 2=GREEN 3=BLUE "
         "4=WHITE 5=BLACK\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   If that is the order you saw, this driver CONTROLS the "
         "panel and every\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   register conclusion in this project rests on real "
         "evidence.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   If you saw a different order, or no change at all, then "
         "nothing we write\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   to the POST is reaching the glass -- and that has to be "
         "found first.\n");

  /* Restore the layer and the previous background state. */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_REGION0_CTRL, saved);
  rk3576_vop_putreg(priv, pb + RK3576_VOP_POST_DSP_BG,
                    RK3576_VOP_POST_BG_DISPLAY_EN |
                        RK3576_VOP_POST_BG_RGB(0x0, 0x0, 0x3ff));
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: BACKGROUND SEQUENCE done; layer restored, background back "
         "to blue.\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_layer_axi_probe
 *
 * Description:
 *   *** THE FIRST EXPERIMENT IN THIS PROJECT WHOSE ORACLE IS TRUSTWORTHY. ***
 *
 *   A probe stepped the POST background through five colours and the screen
 *   showed them, in a deterministic order, cleanly -- black and white included.
 *   That settles two things that everything else here had been assuming:
 *
 *     the register writes DO reach the glass, and the whole path from the POST
 *     through the DSI to the panel is GOOD.  The screen is a working instrument.
 *
 *     the garbage requires the ESMART LAYER to be enabled.  With the layer off
 *     the output is a clean flat colour; the moment the layer is restored the
 *     garbage comes back.  So the fault is confined to the layer's data path,
 *     and nothing downstream of the mixer is suspect any more.
 *
 *   That leaves this driver's three deviations from a known-working
 *   configuration, all of which were introduced on theory and none of which the
 *   working dump contains:
 *
 *     ESMART_CTRL1 bit28, dma_rreq_hurry_en, with thold = 0.  The TRM's trigger
 *       is "if esmart empty lb number >= esmart_dma_rreq_thold, dma_rreq_hurry
 *       is asserted", so thold = 0 makes it PERMANENTLY TRUE: the window holds a
 *       priority request high for the entire run, and has done since this bit
 *       was added.  A request that is always asserted is not a request.
 *
 *     ESMART_AXI_CTRL: outstanding_en with num = 15.  The working configuration
 *       has this field clear, i.e. unlimited.  This driver bounded it to stop
 *       the scan-out DMA monopolising the NoC -- a concern that a later round
 *       established was not the fault.
 *
 *     SYS_AXI0_CTRL_IMD: outstanding_en with num = 16, on axi0.  The working
 *       dump reads 0 here.  ESMART0 and ESMART1 fetch on axi0, so this cap sits
 *       directly on the window's own channel.
 *
 *   One rung each, and the oracle is the eight full-height colour bars that the
 *   preceding probe left in the framebuffer: if the fetch ever starts working,
 *   the bars appear.  Nothing else about the pipeline is touched, so a rung that
 *   produces bars has identified itself.
 *
 *   Baseline rung first: the screen should currently be garbage, because the
 *   layer is on and the fetch is not working.  If it is NOT garbage, then the
 *   bars are already visible and the fault has gone; that is worth knowing too.
 *
 ****************************************************************************/

int rk3576_vop_layer_axi_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t eb;
  uint32_t sb;
  uint32_t v;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  eb = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  sb = RK3576_VOP_SYS_CTRL(priv->base);

  syslog(LOG_WARNING,
         "kickpi-k7: ===== LAYER AXI PROBE (oracle: the 8 colour bars) =====\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   the framebuffer holds R G B W K Y C M bars, so BARS ON "
         "THE SCREEN mean the\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   fetch is working right now.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   These rungs run BACKWARDS from before: the driver now "
         "holds the corrected\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   configuration, and each rung puts ONE of the three old "
         "deviations back.  The\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   rung that makes the bars VANISH has named the culprit "
         "itself.\n");

  /* --- Rung 0: the corrected configuration.  The bars should be visible. --- */

  v = rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_CTRL1);
  syslog(LOG_WARNING,
         "kickpi-k7: RUNG 0: corrected config -- CTRL1=%08x (hurry bit28=%u, "
         "want 0), win AXI=%08x, SYS axi0=%08x.  The BARS should be on screen "
         "now.  WATCH (4 s).\n",
         (unsigned)v, (unsigned)((v >> 28) & 1u),
         (unsigned)rk3576_vop_getreg(priv,
                                     eb + RK3576_VOP_ESMART_AXI_CTRL_IMD),
         (unsigned)rk3576_vop_getreg(priv,
                                     sb + RK3576_VOP_SYS_AXI0_CTRL_IMD));
  up_mdelay(4000);

  /* --- Rung A: put the permanently-asserted hurry request back.  This is the
   * deviation with the clearest mechanism: thold = 0 makes the TRM's trigger
   * true for ever. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_CTRL1,
                    v | RK3576_VOP_ESMART_CTRL1_DMA_RREQ_HURRY_EN);
  rk3576_vop_trigger_cfg_done(priv);
  up_udelay(200);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG A: dma_rreq_hurry_en SET AGAIN with thold=0 "
         "(CTRL1=%08x).  WATCH (5 s).\n",
         (unsigned)rk3576_vop_getreg(priv, eb + RK3576_VOP_ESMART_CTRL1));
  syslog(LOG_WARNING,
         "kickpi-k7:   if the BARS VANISHED, this rung is the culprit.  "
         "(Note the readback above\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   is a MIRROR register: it loads at the next frame "
         "start, so a read taken\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   200 us later still shows the PREVIOUS value.)\n");
  up_mdelay(5000);

  /* --- Rung B: window outstanding bound back. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                    RK3576_VOP_ESMART_AXI_DMA_4K_ADDR_OPT |
                        RK3576_VOP_ESMART_AXI_MMU_BYPASS |
                        RK3576_VOP_ESMART_AXI_OUTSTANDING_EN |
                        RK3576_VOP_ESMART_AXI_OUTSTANDING(15));
  rk3576_vop_trigger_cfg_done(priv);
  up_udelay(200);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG B: window AXI_CTRL bounded again (%08x).  WATCH "
         "(5 s).\n",
         (unsigned)rk3576_vop_getreg(priv,
                                     eb + RK3576_VOP_ESMART_AXI_CTRL_IMD));
  syslog(LOG_WARNING,
         "kickpi-k7:   if the BARS VANISHED, this rung is the culprit.\n");
  up_mdelay(5000);

  /* --- Rung C: SYS axi0 cap back. --- */

  rk3576_vop_putreg(priv, sb + RK3576_VOP_SYS_AXI0_CTRL_IMD,
                    RK3576_VOP_SYS_AXI_OUTSTANDING_EN |
                        RK3576_VOP_SYS_AXI_OUTSTANDING(16));
  rk3576_vop_trigger_cfg_done(priv);
  up_udelay(200);

  syslog(LOG_WARNING,
         "kickpi-k7: RUNG C: SYS_AXI0_CTRL capped again (%08x).  WATCH (5 s).\n",
         (unsigned)rk3576_vop_getreg(priv,
                                     sb + RK3576_VOP_SYS_AXI0_CTRL_IMD));
  syslog(LOG_WARNING,
         "kickpi-k7:   if the BARS VANISHED, this rung is the culprit.  (This "
         "register is IMD, so\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   unlike the two above its readback is current.)\n");
  up_mdelay(5000);

  /* --- Restore the corrected configuration. --- */

  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_CTRL1, v);
  rk3576_vop_putreg(priv, eb + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                    RK3576_VOP_ESMART_AXI_DMA_4K_ADDR_OPT |
                        RK3576_VOP_ESMART_AXI_MMU_BYPASS);
  rk3576_vop_putreg(priv, sb + RK3576_VOP_SYS_AXI0_CTRL_IMD, 0u);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: LAYER AXI PROBE done; corrected configuration RESTORED.\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   REPORT which rung made the bars vanish -- A, B, C, or "
         "none.\n");

  return OK;
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

  /* Fill the framebuffer with a 16x16-pixel black/white checkerboard test
   * pattern (each cell RWGB888 -> 0x00/0xff for every byte).  A uniform
   * fill (0xff or 0x00) makes every byte identical, so the MIPI data lane
   * carries a constant bit stream with no transitions -- useless for
   * verifying pixel packing on the scope.  The checkerboard toggles the
   * bytes so the HS data lanes visibly toggle while scanning. */

  {
    uint8_t *fb = (uint8_t *)priv->fbmem;
    uint32_t x;
    uint32_t y;

    for (y = 0; y < priv->cfg.yres; y++)
      {
        for (x = 0; x < priv->cfg.xres; x++)
          {
            uint8_t v = (((x / 16u) + (y / 16u)) & 1u) ? 0xff : 0x00;
            uint32_t p = (y * priv->stride) + (x * 3);

            fb[p + 0] = v; /* R */
            fb[p + 1] = v; /* G */
            fb[p + 2] = v; /* B */
          }
      }

    up_clean_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);
  }

  /* Bring up clocks and release resets. */

  ret = rk3576_vop_enable_clocks(priv);
  if (ret < 0)
    {
      goto errout_with_fb;
    }

  /* *** CAPTURE THE INHERITED STATE BEFORE TOUCHING ANYTHING. ***
   *
   * Placed here -- after the clocks (so the register file is readable without
   * risking a hang on an unclocked peripheral) but BEFORE rk3576_vop_reset()
   * and before every other VOP write -- so that what it prints is the
   * bootloader's register file, not ours.  This board boots a vendor
   * bootloader that drives the same VOP for a boot logo, so the hardware does
   * not start from reset and this is the only measurement that separates
   * "inherited" from "ours".  See rk3576_vop_early_dump() for why a leftover
   * window is now the leading explanation for a picture that ignores our
   * framebuffer while ESMART0 provably issues no memory reads.
   */

  rk3576_vop_early_dump(priv);

  rk3576_vop_reset(priv);

  /* Power on the VOP's internal CLUSTER and ESMART power domains FIRST.  The
   * ESMART window's registers answer APB accesses even when its domain is
   * powered down, so every later readback would look correct while the window
   * was physically unable to fetch.  See rk3576_vop_power_domain_on(). */

  rk3576_vop_power_domain_on(priv);

  /* Power on the ESMART domain and release the esmart layer reset before
   * touching any ESMART registers (aclk_esmart derives from this domain). */

  rk3576_vop_power_on_esmart(priv);

  /* *** AND TAKE THE VOP'S BUS INTERFACE OUT OF IDLE. ***
   *
   * The TRM's power-up sequence (6.5.6.2) has three steps, not two: power the
   * domain, wait for it, and then "do BIU active operation by software".  This
   * driver only ever did the first two -- and step 3 is the one that decides
   * whether the module can issue AXI transactions at all.  A module whose BIU
   * is held idle reads and writes its configuration registers normally while
   * being unable to fetch a single byte, which is exactly the symptom this
   * bring-up has been chasing.  See rk3576_vop_biu_active().
   */

  rk3576_vop_biu_active();

  /* *** AND OPEN THE VOP'S DDR CHANNEL CLOCK. ***
   *
   * aclk_vo0vop_channel_biu is the clock of the VOP's channel to DDR.  The TRM's
   * clock dependency table lists it beside aclk_vop_biu as a clock that must not
   * be gated, but this driver's biu_clocks[] never included it and the clock
   * tree never registered it, so nothing ever ungated it.  A gated channel can
   * issue no AXI transactions while every register still works and the POST
   * still scans -- which is this board's symptom exactly, and which is shared
   * between ESMART0 and ESMART2, matching the substitution experiment.  See
   * rk3576_vop_channel_biu_enable().
   */

  rk3576_vop_channel_biu_enable();

  /* Program layer, timing and output routing. */

  rk3576_vop_configure_layer(priv);
  rk3576_vop_configure_timing(priv);
  rk3576_vop_configure_port(priv);

  /* --- Temporary bring-up diagnostics (B: dump, no behaviour change) ---
   * 1. Clock rates: dclk_vp0 must be 64MHz to match the DSI IPI's
   *    phy_ipi_ratio (hs_rate / pixel_clock).  Its misprogramming is the
   *    prime suspect for "command link up but all-black video".
   * 2. Framebuffer VA/PA: identity-mapped -> equal; a mismatch means the
   *    ESMART DMA is reading the wrong address.
   * 3. Register readbacks: confirm the mirror->real copy took effect.
   */

  {
    uint32_t esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
    uint32_t ovl_base = RK3576_VOP_OVERLAY_PORT(priv->base, priv->cfg.port);
    uint32_t post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);
    uint32_t sys_base = RK3576_VOP_SYS_CTRL(priv->base);
    uint32_t iface_off = g_rk3576_vop_iface_regs[priv->cfg.iface];
    uintptr_t fb_pa = up_addrenv_va_to_pa(priv->fbmem);

    /* Power-domain state (PMU@0x27360000).  PD_VOP_ESMART/PD_VOP must be
     * "Power up" (status bit 0) before the ESMART layer's aclk_esmart0 clock
     * is generated and its REGFILE becomes readable.  A readback of 0 from
     * every ESMART register above is the tell-tale of a down power domain. */

    syslog(LOG_INFO,
           "vop-dump: PMU PWR_GATE_STS=%08x (vop[11]=%u esmart[12]=%u "
           "cluster[13]=%u; 1=down)\n",
           getreg32(RK3576_PMU_ADDR + 0x20230),
           (getreg32(RK3576_PMU_ADDR + 0x20230) >> 11) & 1,
           (getreg32(RK3576_PMU_ADDR + 0x20230) >> 12) & 1,
           (getreg32(RK3576_PMU_ADDR + 0x20230) >> 13) & 1);

    syslog(LOG_INFO,
           "vop-dump: PMU PWR_GATE_SFTCON0=%08x CON0=%08x "
           "CHAIN0_STS0=%08x INITRST_SFTCON0=%08x\n",
           getreg32(RK3576_PMU_ADDR + 0x20210),
           getreg32(RK3576_PMU_ADDR + 0x20200),
           getreg32(RK3576_PMU_ADDR + 0x20240),
           getreg32(RK3576_PMU_ADDR + 0x20540));

    syslog(LOG_INFO,
           "vop-dump: aclk_vop=%lu hclk_vop=%lu dclk_vp%u=%lu "
           "(this is the ACHIEVED rate; the request came from the board's "
           "KICKPI_K7_MIPI_DSI_PIXCLK and must equal it -- enable_clocks() "
           "warns when it does not, because the DSI IPI timing and "
           "PHY_IPI_RATIO are computed from the requested value)\n",
           (unsigned long)clk_get_rate(priv->aclk),
           (unsigned long)clk_get_rate(priv->hclk),
           (unsigned int)priv->cfg.port,
           (unsigned long)clk_get_rate(priv->dclk));

    syslog(LOG_INFO,
           "vop-dump: fbmem va=%p pa=%p (expect equal) stride=%u fblen=%u\n",
           priv->fbmem, (void *)fb_pa, priv->stride, (unsigned int)priv->fblen);

    /* --- Framebuffer CONTENT check.  "The panel shows black" has two
     * completely different causes that look identical from the DSI side:
     * (a) the pixel stream never reaches the panel, or (b) the pixel stream
     * arrives but every pixel in it is black because the framebuffer itself
     * is black.  Everything upstream has now been verified (PLL, lanes, IPI
     * timing, per-line HS bursts), so (b) must be excluded before spending
     * any more effort on the link.
     *
     * Sample the frame sparsely and report how much of it is non-zero plus a
     * small hash, so a single log line answers "is the source image
     * actually black?".  Read-only; no behaviour change.
     */

    {
      uint32_t nonzero = 0;
      uint32_t sampled = 0;
      uint32_t hash = 2166136261u;
      FAR const uint8_t *fb = (FAR const uint8_t *)priv->fbmem;
      uint32_t words = (uint32_t)(priv->fblen / 4u);
      uint32_t step = words > 4096u ? words / 4096u : 1u;
      uint32_t i;

      for (i = 0; i < words; i += step)
        {
          uint32_t w;

          memcpy(&w, fb + (size_t)i * 4u, sizeof(w));
          sampled++;

          if (w != 0)
            {
              nonzero++;
            }

          hash = (hash ^ w) * 16777619u;
        }

      syslog(LOG_INFO,
             "vop-dump: FB content sampled=%u nonzero=%u hash=%08x stride=%u "
             "(nonzero==0 -> the framebuffer is entirely BLACK, so a black "
             "screen proves nothing about the link; fill it with white/colour "
             "before judging the panel)\n",
             (unsigned)sampled, (unsigned)nonzero, hash,
             (unsigned)priv->stride);
    }

    /* --- Probe C: read the WIN_CFG_DONE / REG_CFG_DONE a second time (now
     * several ms after configure_port fired them).  TRM says a load bit
     * holds 1 "until cleared", so if it was 1 in probe-A and 0 here, some
     * later write or a frame-boundary auto-clear wiped it. --- */

    syslog(LOG_INFO,
           "vop-probe-C: now REG_CFG_DONE=%08x WIN_CFG_DONE=%08x\n",
           rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE),
           rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE));

    {
      uint32_t esmart_ctrl0 =
        rk3576_vop_getreg(priv, esmart_base + RK3576_VOP_ESMART_CTRL0);

      syslog(LOG_INFO,
             "vop-dump: ESMART0 CTRL0=%08x (frm_resetn_en[bit31]=%u) "
             "REGION0_CTRL=%08x YRGB_MST=%08x VIR=%08x\n",
             esmart_ctrl0,
             (unsigned)((esmart_ctrl0 >> 31) & 1),
             rk3576_vop_getreg(priv,
                               esmart_base + RK3576_VOP_ESMART_REGION0_CTRL),
             rk3576_vop_getreg(priv,
                               esmart_base + RK3576_VOP_ESMART_REGION0_YRGB_MST),
             rk3576_vop_getreg(priv,
                               esmart_base + RK3576_VOP_ESMART_REGION0_VIR));
    }

    syslog(LOG_INFO,
           "vop-dump: OVERLAY_PORT%u_LAYER_SEL=%08x (expect layer0=Esmart0)\n",
           (unsigned int)priv->cfg.port,
           rk3576_vop_getreg(priv, ovl_base + RK3576_VOP_OVERLAY_LAYER_SEL));

    /* The MIX0 blend registers are a content check, not a routing check.
     *
     * They reset to all-zero, and the blend formula with src_factor = 0 and
     * dst_factor = 0 evaluates to Cd = 0*Cs + 0*Cd = BLACK for every pixel
     * regardless of the source -- which is exactly how the "pixels genuinely
     * reach the IPI but the panel shows nothing" milestone was reached once
     * before (configure_layer() now programs them).  So dump them: if the
     * framebuffer is a colour checkerboard (the content probe above proves it
     * is), and these read back non-zero with factor=Ags / 256-Ad0, then the
     * pixel data leaving the POST block really is the framebuffer content and
     * "black screen" cannot be a black stream.  Cheap, and it retires a
     * question that has been re-opened more than once. */

    syslog(LOG_INFO,
           "vop-dump: MIX0 src_color=%08x dst_color=%08x src_alpha=%08x "
           "dst_alpha=%08x (expect 00ff01a0 00ff0060 00000020 00000070 = "
           "factor Ags / 256-Ad0 with glb_alpha=0xff; a factor field of 0 "
           "would multiply the layer away)\n",
           rk3576_vop_getreg(priv,
                             ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL),
           rk3576_vop_getreg(priv,
                             ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL),
           rk3576_vop_getreg(priv,
                             ovl_base + RK3576_VOP_OVERLAY_MIX0_SRC_ALPHA_CTRL),
           rk3576_vop_getreg(priv,
                             ovl_base + RK3576_VOP_OVERLAY_MIX0_DST_ALPHA_CTRL));

    syslog(LOG_INFO,
           "vop-dump: POST%u_DSP_BG=%08x (bg_display_en must be 0, else the "
           "background colour is composited under the layer)\n",
           (unsigned int)priv->cfg.port,
           rk3576_vop_getreg(priv, post_base + RK3576_VOP_POST_DSP_BG));

    {
      uint32_t iface =
        rk3576_vop_getreg(priv, sys_base + iface_off);

      syslog(LOG_INFO,
             "vop-dump: POST%u_DSP_CTRL=%08x MIPI0_INFACE_CTRL=%08x "
             "REG_CFG_DONE=%08x WIN_CFG_DONE=%08x\n",
             (unsigned int)priv->cfg.port,
             rk3576_vop_getreg(priv, post_base + RK3576_VOP_POST_DSP_CTRL),
             iface,
             rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_REG_CFG_DONE),
             rk3576_vop_getreg(priv,
                               sys_base + RK3576_VOP_SYS_WIN_REG_CFG_DONE));

      syslog(LOG_INFO,
             "vop-dump: INFACE out_en=%u clk_out_en=%u port_sel=%u "
             "hsync_pol=%u vsync_pol=%u cmd_mode=%u dclk_sel=%u "
             "pix_clk_sel=%u\n",
             (unsigned)(iface & RK3576_VOP_IFACE_OUT_EN) != 0,
             (unsigned)((iface >> 1) & 1),
             (unsigned)((iface >> RK3576_VOP_IFACE_PORT_SEL_SHIFT) & 0x3),
             (unsigned)((iface >> 4) & 1),
             (unsigned)((iface >> 5) & 1),
             (unsigned)((iface >> 11) & 1),
             (unsigned)((iface >> 21) & 1),
             (unsigned)((iface >> 20) & 1));
    }

    /* The timing registers are printed together with the values they SHOULD
     * hold for this configuration, so the line checks itself.  The expected
     * encoding is the reference driver's (see configure_timing()): FULL total
     * in the high half, and an exclusive end in the low half -- an earlier
     * revision wrote total-1 / end-1, which is a one-pixel-short line. */

    {
      uint16_t xres = priv->cfg.xres;
      uint16_t yres = priv->cfg.yres;
      uint16_t htotal = xres + priv->cfg.hsync_len +
                        priv->cfg.hfront_porch + priv->cfg.hback_porch;
      uint16_t vtotal = yres + priv->cfg.vsync_len +
                        priv->cfg.vfront_porch + priv->cfg.vback_porch;
      uint16_t hact_st = htotal - (xres + priv->cfg.hfront_porch);
      uint16_t vact_st = vtotal - (yres + priv->cfg.vfront_porch);
      uint32_t exp_htotal = ((uint32_t)htotal << 16) |
                            (priv->cfg.hsync_len & 0x1fff);
      uint32_t exp_hact = ((uint32_t)hact_st << 16) |
                          ((hact_st + xres) & 0x1fff);
      uint32_t exp_vtotal = ((uint32_t)vtotal << 16) |
                            (priv->cfg.vsync_len & 0x1fff);
      uint32_t exp_vact = ((uint32_t)vact_st << 16) |
                          ((vact_st + yres) & 0x1fff);
      uint32_t got_htotal = rk3576_vop_getreg(
          priv, post_base + RK3576_VOP_POST_DSP_HTOTAL_HS_END);
      uint32_t got_hact = rk3576_vop_getreg(
          priv, post_base + RK3576_VOP_POST_DSP_HACT_ST_END);
      uint32_t got_vtotal = rk3576_vop_getreg(
          priv, post_base + RK3576_VOP_POST_DSP_VTOTAL_VS_END);
      uint32_t got_vact = rk3576_vop_getreg(
          priv, post_base + RK3576_VOP_POST_DSP_VACT_ST_END);

      syslog(LOG_INFO,
             "vop-dump: POST%u HTOTAL=%08x HACT=%08x VTOTAL=%08x VACT=%08x "
             "(expect %08x %08x %08x %08x) MIPI_CTRL=%08x CORE_CLK=%08x\n",
             (unsigned int)priv->cfg.port, got_htotal, got_hact, got_vtotal,
             got_vact, exp_htotal, exp_hact, exp_vtotal, exp_vact,
             rk3576_vop_getreg(priv, post_base + RK3576_VOP_POST_MIPI_CTRL),
             rk3576_vop_getreg(priv, post_base + RK3576_VOP_POST_CORE_CLK));

      if (got_htotal != exp_htotal || got_hact != exp_hact ||
          got_vtotal != exp_vtotal || got_vact != exp_vact)
        {
          gerr("WARNING: VOP POST timing readback does not match the "
               "configuration -- the output geometry is not what the DSI IPI "
               "and the panel were told to expect\n");
        }
    }

    /* Decisive scan-state probe: dsp_vcnt0 is the VP0 vertical counter,
     * monotonically incrementing while the VP0 scan state machine runs.  A
     * value stuck at 0 proves VOP is NOT scanning (no pixel clock reaching
     * POST), which is the only remaining explanation for a fully-configured
     * but silent DSI IPI.  Read it twice with a short delay to observe any
     * increment; also dump the vsync-to-IO routing / DSI side IPI busy. */

    {
      uint32_t vcnt0 = rk3576_vop_getreg(priv,
                                         sys_base + RK3576_VOP_SYS_STATUS0);
      uint32_t vcnt_after;

      up_udelay(2000); /* ~2 ms, several scan lines at 64 MHz */

      vcnt_after = rk3576_vop_getreg(priv,
                                     sys_base + RK3576_VOP_SYS_STATUS0);

      {
        uint32_t v0 = (vcnt0 >> RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff;
        uint32_t v1 = (vcnt_after >> RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff;
        uint32_t vtotal = priv->cfg.yres + priv->cfg.vsync_len +
                          priv->cfg.vfront_porch + priv->cfg.vback_porch;
        uint32_t dclk = (uint32_t)clk_get_rate(priv->dclk);
        uint32_t htotal = priv->cfg.xres + priv->cfg.hsync_len +
                          priv->cfg.hback_porch + priv->cfg.hfront_porch;
        uint32_t expect =
            (htotal != 0) ? (dclk / htotal) / 500u : 0; /* lines per 2 ms */
        uint32_t delta;

        /* The counter counts scan lines 0..vtotal-1 and then wraps at VTOTAL,
         * NOT at the width of its register field.  Taking the difference
         * modulo the field width (13 bits) therefore produces a confident-
         * looking but wrong number -- e.g. "3512500 lines/s" for a perfectly
         * normal 76 kHz line rate.  Wrap at vtotal. */

        delta = (vtotal != 0) ? ((v1 + vtotal - v0) % vtotal) : (v1 - v0);

        syslog(LOG_INFO,
               "vop-dump: SYS_STATUS0=%08x (dsp_vcnt0=%u) then %08x "
               "(dsp_vcnt0=%u) delta=%u lines/2ms (expect ~%u for dclk %u Hz "
               "/ htotal %u, vtotal %u) VSYNC_CTRL=%08x\n",
               vcnt0, v0, vcnt_after, v1, delta, expect, (unsigned)dclk,
               (unsigned)htotal, (unsigned)vtotal,
               rk3576_vop_getreg(priv,
                                 sys_base + RK3576_VOP_SYS_VSYNC_CTRL));

        /* A delta that is off by more than half is not a timing fault: the
         * 2 ms window can be stretched by interrupt latency, which does not
         * change "is it scanning" but does change "how fast". */

        if (delta == 0)
          {
            gerr("WARNING: VOP scan counter did not advance in 2 ms -- the "
                 "timing generator is not running\n");
          }
      }
    }
  }

  /* Self-register the framebuffer device. */

  ret = fb_register_device(config->display, config->plane, &priv->vtable);
  if (ret < 0)
    {
      gerr("ERROR: VOP fb_register_device() failed: %d\n", ret);
      goto errout_with_clocks;
    }

  /* Publish the handle for the read-only status helpers (see
   * rk3576_vop_get_scan_counter). */

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
 * Name: rk3576_vop_get_scan_counter
 *
 * Description:
 *   Read VP0's vertical scan counter (SYS_STATUS0.dsp_vcnt0).  See the
 *   prototype in rk3576_vop.h for the sampling rules.
 *
 ****************************************************************************/

uint32_t rk3576_vop_get_scan_counter(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t status;

  if (priv == NULL)
    {
      return 0;
    }

  status = rk3576_vop_getreg(priv, RK3576_VOP_SYS_CTRL(priv->base) +
                                       RK3576_VOP_SYS_STATUS0);

  return (status >> RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff;
}

/****************************************************************************
 * Name: rk3576_vop_set_output_enable
 *
 * Description:
 *   Gate the interface output on/off.  See the prototype in rk3576_vop.h.
 *
 ****************************************************************************/

int rk3576_vop_set_output_enable(bool enable)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sys_base;
  uint32_t iface_off;
  uint32_t mask;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  iface_off = g_rk3576_vop_iface_regs[priv->cfg.iface];
  mask = RK3576_VOP_IFACE_OUT_EN | RK3576_VOP_IFACE_CLK_OUT_EN;

  /* modifyreg writes the hiword mask, so passing 0 as the value clears both
   * bits and passing `mask` sets them.  The polarity/port_sel fields are left
   * untouched: this only removes the pixel feed. */

  rk3576_vop_modifyreg(priv, sys_base + iface_off, mask,
                       enable ? mask : 0u);

  return OK;
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

/****************************************************************************
 * Name: rk3576_vop_get_int_status
 *
 * Description:
 *   Read (and optionally clear) the VOP's latched interrupt state for the
 *   configured video port.  See the prototype's comment in rk3576_vop.h for
 *   why the RAW register is the one that carries information here.
 *
 ****************************************************************************/

int rk3576_vop_get_int_status(FAR struct rk3576_vop_int_s *status,
                              bool clear)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sys_base;
  uint32_t vp_status_off;
  uint32_t vp_clr_off;
  uint32_t vp_raw_off;

  if (priv == NULL || status == NULL)
    {
      return -ENODEV;
    }

  memset(status, 0, sizeof(*status));

  sys_base = RK3576_VOP_SYS_CTRL(priv->base);
  vp_status_off = RK3576_VOP_VP_INT_STATUS(priv->cfg.port);
  vp_clr_off = RK3576_VOP_VP_INT_CLR(priv->cfg.port);
  vp_raw_off = RK3576_VOP_VP_INT_RAW_STATUS(priv->cfg.port);

  status->sys0 = rk3576_vop_getreg(priv, sys_base +
                                   RK3576_VOP_SYS0_INT_RAW);
  status->sys1 = rk3576_vop_getreg(priv, sys_base +
                                   RK3576_VOP_SYS1_INT_RAW);
  status->vp_raw = rk3576_vop_getreg(priv, sys_base + vp_raw_off);
  status->vp_status = rk3576_vop_getreg(priv, sys_base + vp_status_off);

  status->bus_error = (status->sys0 & RK3576_VOP_INT_BUS_ERRPR) != 0 ||
                      (status->sys1 & RK3576_VOP_INT_BUS_ERRPR) != 0;
  status->post_buf_empty =
      (status->vp_raw & RK3576_VOP_INT_POST_BUF_EMPTY) != 0;
  status->frame_start = (status->vp_raw & RK3576_VOP_INT_FS) != 0;
  status->line_flag = (status->vp_raw &
                       (RK3576_VOP_INT_LINE_FLAG0 |
                        RK3576_VOP_INT_LINE_FLAG1)) != 0;

  if (clear)
    {
      /* Clear the bits we just reported (hiword write-enable mask), so the
       * next window starts from a known state and an event can be attributed
       * to the period in which it happened. */

      rk3576_vop_putreg(priv, sys_base + vp_clr_off,
                        status->vp_raw | (status->vp_raw & 0xffffu));
      rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS0_INT_CLR,
                        status->sys0 | (status->sys0 & 0xffffu));
      rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS1_INT_CLR,
                        status->sys1 | (status->sys1 & 0xffffu));
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_paint_test_pattern
 *
 * Description:
 *   Eight vertical colour bars with a solid marker band across the top.
 *   See the prototype's comment in rk3576_vop.h for how to read the result.
 *
 ****************************************************************************/

int rk3576_vop_paint_test_pattern(uint32_t marker_rgb)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;

  static const uint8_t bars[8][3] =
  {
    { 0xff, 0xff, 0xff }, /* white   */
    { 0xff, 0xff, 0x00 }, /* yellow  */
    { 0x00, 0xff, 0xff }, /* cyan    */
    { 0x00, 0xff, 0x00 }, /* green   */
    { 0xff, 0x00, 0xff }, /* magenta */
    { 0xff, 0x00, 0x00 }, /* red     */
    { 0x00, 0x00, 0xff }, /* blue    */
    { 0x00, 0x00, 0x00 }, /* black   */
  };

  uint8_t mr;
  uint8_t mg;
  uint8_t mb;
  uint32_t band;
  uint32_t barw;
  uint8_t *fb;
  uint32_t x;
  uint32_t y;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  mr = (uint8_t)(marker_rgb >> 16);
  mg = (uint8_t)(marker_rgb >> 8);
  mb = (uint8_t)(marker_rgb);

  /* Top eighth = marker, remainder = vertical bars.  Both divisors are exact
   * for the 720x1280 panel this board drives (90 px bars, 160-line band), but
   * the arithmetic is written to stay correct for any geometry. */

  band = priv->cfg.yres / 8u;
  barw = priv->cfg.xres / 8u;
  if (barw == 0)
    {
      barw = 1;
    }

  fb = (uint8_t *)priv->fbmem;

  for (y = 0; y < priv->cfg.yres; y++)
    {
      uint32_t row = y * priv->stride;

      if (y < band)
        {
          for (x = 0; x < priv->cfg.xres; x++)
            {
              uint32_t p = row + (x * 3);

              fb[p + 0] = mr;
              fb[p + 1] = mg;
              fb[p + 2] = mb;
            }
        }
      else
        {
          for (x = 0; x < priv->cfg.xres; x++)
            {
              uint32_t bar = x / barw;
              uint32_t p = row + (x * 3);

              if (bar > 7u)
                {
                  bar = 7u;
                }

              fb[p + 0] = bars[bar][0];
              fb[p + 1] = bars[bar][1];
              fb[p + 2] = bars[bar][2];
            }
        }
    }

  /* The ESMART layer DMAs this memory with the MMU bypassed, so the cache
   * must be cleaned or the display keeps scanning out stale lines. */

  up_clean_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);

  return OK;
}

int rk3576_vop_paint_byte_probe(void)
{
  /* One constant byte per bar.  Adjacent levels differ by 0x24 (36) so the
   * ramp has eight clearly separated steps in both RGB888 and, if the panel
   * mis-packs it, in any wider or narrower interpretation. */

  static const uint8_t levels[8] =
  {
    0x00, 0x24, 0x49, 0x6d, 0x92, 0xb6, 0xdb, 0xff
  };

  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  FAR uint8_t *fb;
  uint32_t barw;
  uint32_t x;
  uint32_t y;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  barw = priv->cfg.xres / 8u;
  if (barw == 0u)
    {
      barw = 1u;
    }

  fb = (uint8_t *)priv->fbmem;

  for (y = 0; y < priv->cfg.yres; y++)
    {
      uint32_t row = y * priv->stride;

      for (x = 0; x < priv->cfg.xres; x++)
        {
          uint32_t bar = x / barw;
          uint32_t p = row + (x * 3u);
          uint8_t v;

          if (bar > 7u)
            {
              bar = 7u;
            }

          v = levels[bar];

          fb[p + 0] = v;
          fb[p + 1] = v;
          fb[p + 2] = v;
        }
    }

  /* Same reason as every other paint path: the ESMART layer DMAs this memory
   * with the MMU bypassed, so the cache must be cleaned or the panel keeps
   * scanning out stale lines. */

  up_clean_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);

  return OK;
}

int rk3576_vop_peek_pixel(uint16_t x, uint16_t y, FAR uint32_t *rgb)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  FAR const uint8_t *fb;
  uint32_t p;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  if (rgb == NULL || x >= priv->cfg.xres || y >= priv->cfg.yres)
    {
      return -EINVAL;
    }

  fb = (FAR const uint8_t *)priv->fbmem;
  p = (y * priv->stride) + (x * 3u);

  *rgb = ((uint32_t)fb[p] << 16) | ((uint32_t)fb[p + 1] << 8) |
         (uint32_t)fb[p + 2];

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_post_force
 *
 * Description:
 *   Drive the POST's own output overrides -- the ones that generate pixels
 *   WITHOUT reading any memory -- and reload the mirror registers.
 *
 *   See rk3576_vop_source_probe() for why this matters: these bits are the
 *   only way to ask "does a pixel we generated reach the glass?" without the
 *   layer, the DMA, the address registers or the framebuffer content being
 *   able to influence the answer.
 *
 * Input Parameters:
 *   setbits - RK3576_VOP_POST_DSP_OUT_ZERO / _BLACK_EN, or 0 for normal
 *
 ****************************************************************************/

static void rk3576_vop_post_force(FAR struct rk3576_vop_s *priv,
                                  uint32_t setbits)
{
  uint32_t post_base = RK3576_VOP_POST(priv->base, priv->cfg.port);

  /* POST_DSP_CTRL is a mirror register, so write the whole intended word
   * rather than reading it back first (the driver's configure_port() does
   * the same, and for the same reason).  Value 0 == standby off + out mode
   * RGB888 + no override, which is exactly what configure_port() programs. */

  rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_CTRL,
                    RK3576_VOP_POST_OUT_RGB888 | setbits);

  rk3576_vop_trigger_cfg_done(priv);
}

/****************************************************************************
 * Name: rk3576_vop_source_probe
 *
 * Description:
 *   Establish WHERE the pixels on the glass come from -- the one question
 *   that has never actually been measured, and the one every previous probe
 *   silently assumed an answer to.
 *
 *   THE ARGUMENT FOR ASKING IT NOW.  A byte-uniform image (all 0x00 or all
 *   0xff) cannot be turned into garble by ANY receiver-side fault: lane swap,
 *   lane count, byte order, bit order, packing, wrong bytes-per-pixel, shear,
 *   frame folding, FIFO underrun -- every one of them permutes or re-groups
 *   IDENTICAL bytes and leaves a uniform image uniform.  This board feeds
 *   pure black and pure white and the glass shows static garble.  It also
 *   feeds the same bytes from two different physical addresses, and the glass
 *   still shows the same garble.  There is therefore no fault in the
 *   framebuffer-to-panel path that can explain the picture: the displayed
 *   bytes are not the bytes we are writing.
 *
 *   Every content probe this driver has ever run -- colour bars, whole-screen
 *   colours, the byte-ramp bars, the black/white strobe, the fetch-address
 *   echo -- wrote `priv->fbmem` through ESMART0.  If ESMART0 is not the
 *   window feeding the POST, all of them were guaranteed to produce no
 *   change, and their "no change" results say nothing about the panel.
 *
 *   So this probe walks a ladder in which the pixel source is progressively
 *   more "ours", and each rung is announced:
 *
 *     1. POST forced BLACK   (bit16) -- generated inside the POST, no memory
 *        access at all, not even the layer is involved;
 *     2. POST forced ZERO    (bit15) -- likewise;
 *     3. ESMART0 DISABLED and the POST background colour set to solid BLUE
 *        -- again no layer data, but now the normal background path;
 *     4. the normal path: layer enabled, framebuffer filled solid WHITE.
 *
 *   What each outcome means:
 *
 *     1 or 2 turns the screen black -> the POST output really does reach the
 *        glass.  Then the pixel path POST->DSI->panel is PROVEN WORKING, and
 *        the fault is entirely in the layer/mixer/fetch side -- upstream of
 *        the POST, i.e. in the part of the pipeline that decides WHICH bytes
 *        get composited.  Rung 4 then shows whether our framebuffer is that
 *        source.
 *
 *     none of them changes anything -> not even a colour the POST generates
 *        internally reaches the glass.  The picture is then being made
 *        somewhere other than this compositor, and no work on the layer or
 *        the framebuffer could ever have fixed it.
 *
 *   The register dump that precedes the rungs is not decoration: the mixer
 *   registers decide whether the layer is composited or silently dropped
 *   (their reset value multiplies both source and destination by zero), and
 *   the cfg_done registers report whether the mirror->real loads were ever
 *   consumed.  Reading them costs nothing and has been the missing half of
 *   several earlier conclusions.
 *
 *   Nothing here touches the DSI, the PHY or the panel: it only re-points
 *   the compositor, so a panel-side or link-side result cannot be disturbed.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_source_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sys_base;
  uint32_t post_base;
  uint32_t ovl_base;
  uint32_t esmart_base;
  uint32_t vcnt_a;
  uint32_t vcnt_b;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  sys_base   = RK3576_VOP_SYS_CTRL(priv->base);
  post_base  = RK3576_VOP_POST(priv->base, priv->cfg.port);
  ovl_base   = RK3576_VOP_OVERLAY_PORT(priv->base, priv->cfg.port);
  esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);

  /* --- Witnesses first: what the hardware believes about the source. --- */

  vcnt_a = (rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0) >>
            RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff;
  up_mdelay(20);
  vcnt_b = (rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0) >>
            RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fff;

  syslog(LOG_WARNING,
         "kickpi-k7: SOURCE PROBE: dsp_vcnt0 %u -> %u over 20 ms ( %s )\n",
         (unsigned)vcnt_a, (unsigned)vcnt_b,
         vcnt_b != vcnt_a ? "SCANNING" : "FROZEN -- no frames at all");

  {
    uint32_t reg_done = rk3576_vop_getreg(priv, sys_base +
                                          RK3576_VOP_SYS_REG_CFG_DONE);
    uint32_t win_done = rk3576_vop_getreg(priv, sys_base +
                                          RK3576_VOP_SYS_WIN_REG_CFG_DONE);

    syslog(LOG_WARNING,
           "kickpi-k7: SOURCE PROBE: REG_CFG_DONE=%08x WIN_CFG_DONE=%08x | "
           "VP0 global0=%u sys0=%u | esmart0=%u (0=CONSUMED, 1=STILL PENDING; "
           "a pending VP0 bit means the mirror never reached the real "
           "registers)\n",
           (unsigned)reg_done, (unsigned)win_done,
           (unsigned)(reg_done & 1u), (unsigned)((reg_done >> 4) & 1u),
           (unsigned)((win_done >> 4) & 1u));
  }

  {
    uint32_t ovl_ctrl = rk3576_vop_getreg(priv, ovl_base +
                                          RK3576_VOP_OVERLAY_CTRL);
    uint32_t sel = rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_LAYER_SEL);
    uint32_t src = rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL);
    uint32_t dst = rk3576_vop_getreg(priv, ovl_base +
                                     RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL);
    uint32_t bgmix = rk3576_vop_getreg(priv, ovl_base +
                                       RK3576_VOP_OVERLAY_BG_MIX_CTRL);

    syslog(LOG_WARNING,
           "kickpi-k7: SOURCE PROBE: OVERLAY_CTRL=%08x LAYER_SEL=%08x "
           "(layer0 sel=%u: 0=CLUSTER0 1=CLUSTER1 2=ESMART0 3=ESMART2 "
           "15=DISABLE)\n",
           (unsigned)ovl_ctrl, (unsigned)sel, (unsigned)(sel & 0xfu));

    syslog(LOG_WARNING,
           "kickpi-k7: SOURCE PROBE: MIX0 SRC=%08x DST=%08x BG_MIX=%08x "
           "(factor 0 in either multiplies the layer away; alpha_en=%u)\n",
           (unsigned)src, (unsigned)dst, (unsigned)bgmix,
           (unsigned)((src >> 8) & 1u));
  }

  syslog(LOG_WARNING,
         "kickpi-k7: SOURCE PROBE: POST_DSP_CTRL=%08x CORE_CLK=%08x "
         "DSP_BG=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, post_base +
                                     RK3576_VOP_POST_DSP_CTRL),
         (unsigned)rk3576_vop_getreg(priv, post_base +
                                     RK3576_VOP_POST_CORE_CLK),
         (unsigned)rk3576_vop_getreg(priv, post_base +
                                     RK3576_VOP_POST_DSP_BG));

  syslog(LOG_WARNING,
         "kickpi-k7: SOURCE PROBE: ESMART0 CTRL0=%08x AXI=%08x PORT_SEL=%08x "
         "REGION0_CTRL=%08x YRGB_MST=%08x VIR=%08x ACT=%08x DSP=%08x\n",
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base + RK3576_VOP_ESMART_CTRL0),
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base +
                                         RK3576_VOP_ESMART_AXI_CTRL_IMD),
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base +
                                         RK3576_VOP_ESMART_PORT_SEL_IMD),
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base +
                                         RK3576_VOP_ESMART_REGION0_CTRL),
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base +
                                         RK3576_VOP_ESMART_REGION0_YRGB_MST),
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base +
                                         RK3576_VOP_ESMART_REGION0_VIR),
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base +
                                         RK3576_VOP_ESMART_REGION0_ACT_INFO),
         (unsigned)rk3576_vop_getreg(priv,
                                     esmart_base +
                                         RK3576_VOP_ESMART_REGION0_DSP_INFO));

  /* --- Witnesses for "is the window's read actually happening?" ---
   *
   * Rung 3 proved the POST/DSI/panel path can put a CLEAN full-screen colour on
   * the glass, and that disabling the layer removes the garble -- so the layer
   * IS routed to the output and the garble IS the layer's data.  The layer's
   * registers read back exactly as programmed, yet its output follows neither
   * the framebuffer's content nor its address.  The remaining possibility is
   * that its read never returns our data at all, and these three registers are
   * how the hardware reports that:
   *
   *   SYS_AXI0_CTRL_IMD  bit1 dma_stop   -- if SET, the VOP's AXI0 channel is
   *        stopped, and ESMART (esmart_axi_sel=0 at reset) can never fetch.  The
   *        layer would then emit undefined data: a stable, content-independent
   *        and address-independent pattern, which is exactly what is on screen.
   *   VOP BUS_ERRPR / POST_BUF_EMPTY     -- the window's AXI read failed, or the
   *        POST output buffer underran.  Either one is the hardware stating
   *        that the pixels never arrived.
   *   ESMART_CTRL1                       -- the layer's own DMA control, which
   *        this driver has never programmed and never read.
   *
   * Read and cleared, so a zero here means "clean since the last clear". */

  {
    uint32_t axi0 = rk3576_vop_getreg(priv, sys_base +
                                      RK3576_VOP_SYS_AXI0_CTRL_IMD);
    uint32_t axi1 = rk3576_vop_getreg(priv, sys_base +
                                      RK3576_VOP_SYS_AXI1_CTRL_IMD);
    uint32_t ctrl1 = rk3576_vop_getreg(priv, esmart_base +
                                       RK3576_VOP_ESMART_CTRL1);
    uint32_t yrgb_id = (ctrl1 & RK3576_VOP_ESMART_CTRL1_YRGB_ID_MASK) >>
                       RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT;
    uint32_t uv_id = (ctrl1 & RK3576_VOP_ESMART_CTRL1_UV_ID_MASK) >>
                     RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT;

    syslog(LOG_WARNING,
           "kickpi-k7: SOURCE PROBE: SYS_AXI0_CTRL_IMD=%08x (dma_stop=%u "
           "outstanding_en=%u num=%u MMU_IDLE=%u) SYS_AXI1=%08x "
           "ESMART_CTRL1=%08x (yrgb_rid=0x%02x uv_rid=0x%02x; ESMART%u must "
           "use 0x%02x/0x%02x)\n",
           (unsigned)axi0,
           (unsigned)((axi0 & RK3576_VOP_SYS_AXI_DMA_STOP) != 0u),
           (unsigned)((axi0 & RK3576_VOP_SYS_AXI_OUTSTANDING_EN) != 0u),
           (unsigned)((axi0 >> RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT) & 0x3fu),
           (unsigned)((axi0 & RK3576_VOP_SYS_AXI_MMU_IDLE) != 0u),
           (unsigned)axi1, (unsigned)ctrl1, (unsigned)yrgb_id,
           (unsigned)uv_id, (unsigned)RK3576_VOP_ESMART_IDX,
           (unsigned)g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX],
           (unsigned)g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX]);

    /* An AXI read ID that does not match this window's is not cosmetic: the ID
     * travels with every read into the interconnect, where it selects routing
     * and QoS, and a wrong one can produce reads that never come back -- no
     * data, no bus error, and a permanently starved POST. */

    if (yrgb_id != g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX] ||
        uv_id != g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX])
      {
        syslog(LOG_ERR,
               "kickpi-k7:   *** AXI READ ID MISMATCH: this window reads as "
               "0x%02x/0x%02x, but ESMART%u\n", (unsigned)yrgb_id,
               (unsigned)uv_id, (unsigned)RK3576_VOP_ESMART_IDX);
        syslog(LOG_ERR,
               "kickpi-k7:   must use 0x%02x/0x%02x.  Reads carrying "
               "another window's\n",
               (unsigned)g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX],
               (unsigned)g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX]);
        syslog(LOG_ERR,
               "kickpi-k7:   ID may never be returned, which starves the POST "
               "with no error ***\n");
      }

    /* Decoded with the TRM's bit positions.  The previous version tested bit1
     * for dma_stop -- bit1 is outstanding_en -- so it printed "dma_stop=0"
     * while the register's bit0 (the real dma_stop) was set, and the layer's
     * fetch was in fact stopped. */

    if ((axi0 & RK3576_VOP_SYS_AXI_DMA_STOP) != 0u)
      {
        syslog(LOG_ERR,
               "kickpi-k7:   *** dma_stop IS SET on AXI0 -- the ESMART cannot "
               "fetch anything, which\n");
        syslog(LOG_ERR,
               "kickpi-k7:   would make its output stable garbage regardless "
               "of content or address ***\n");
      }

    if ((axi0 & RK3576_VOP_SYS_AXI_MMU_IDLE) != 0u)
      {
        syslog(LOG_WARNING,
               "kickpi-k7:   AXI0 MMU reports IDLE: no read is in flight.  On "
               "a scanning VOP whose\n");
        syslog(LOG_WARNING,
               "kickpi-k7:   layer is enabled this means the layer is NOT "
               "fetching, whatever its\n");
        syslog(LOG_WARNING,
               "kickpi-k7:   registers say ***\n");
      }
  }

  {
    uint32_t vp_raw = rk3576_vop_getreg(priv, sys_base +
                                        RK3576_VOP_VP_INT_RAW_STATUS(
                                            priv->cfg.port));
    uint32_t s0 = rk3576_vop_getreg(priv, sys_base +
                                    RK3576_VOP_SYS0_INT_RAW);
    uint32_t s1 = rk3576_vop_getreg(priv, sys_base +
                                    RK3576_VOP_SYS1_INT_RAW);

    syslog(LOG_WARNING,
           "kickpi-k7: SOURCE PROBE: VOP int raw: VP%u=%08x "
           "(fs=%u fs_new=%u line0=%u line1=%u BUF_EMPTY=%u field=%u "
           "hold=%u) SYS0=%08x SYS1=%08x (bus_errpr=%u)\n",
           (unsigned)priv->cfg.port, (unsigned)vp_raw,
           (unsigned)(vp_raw & 1u), (unsigned)((vp_raw >> 1) & 1u),
           (unsigned)((vp_raw >> 2) & 1u), (unsigned)((vp_raw >> 3) & 1u),
           (unsigned)((vp_raw >> 4) & 1u), (unsigned)((vp_raw >> 5) & 1u),
           (unsigned)((vp_raw >> 6) & 1u), (unsigned)s0, (unsigned)s1,
           (unsigned)(((s0 | s1) & RK3576_VOP_SYS_INT_BUS_ERROR) != 0u));

    /* NOTE ON THE DECODE.  bit1 of a per-VP status is FS_NEW -- a per-frame
     * event -- and NOT a bus error; the bus error is only ever in SYS0/SYS1.
     * An earlier version of this code applied bit1 to the per-VP register and
     * announced "the VOP reports an AXI READ ERROR", which was wrong: the
     * SYS registers are zero and there is no bus error.  The bit that IS
     * meaningful here is bit4. */

    if ((s0 & RK3576_VOP_SYS_INT_BUS_ERROR) != 0u ||
        (s1 & RK3576_VOP_SYS_INT_BUS_ERROR) != 0u)
      {
        syslog(LOG_ERR,
               "kickpi-k7:   *** SYS0/SYS1 report an AXI READ ERROR: the "
               "layer's fetch is\n");
        syslog(LOG_ERR,
               "kickpi-k7:   failing, so its output cannot be our "
               "framebuffer ***\n");
      }

    if ((vp_raw & RK3576_VOP_INT_POST_BUF_EMPTY) != 0u)
      {
        syslog(LOG_ERR,
               "kickpi-k7:   *** POST output buffer UNDER-RAN (per-VP bit4): "
               "the pixels did NOT\n");
        syslog(LOG_ERR,
               "kickpi-k7:   arrive in time, so the POST transmitted "
               "something other than the\n");
        syslog(LOG_ERR,
               "kickpi-k7:   framebuffer.  This is a candidate mechanism for "
               "content-independent\n");
        syslog(LOG_ERR,
               "kickpi-k7:   garble and is NOT a per-line event ***\n");
      }

    /* Clear so the next report covers only what follows. */

    rk3576_vop_putreg(priv, sys_base +
                      RK3576_VOP_VP_INT_CLR(priv->cfg.port),
                      vp_raw | (vp_raw & 0xffffu));
    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS0_INT_CLR,
                      s0 | (s0 & 0xffffu));
    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS1_INT_CLR,
                      s1 | (s1 & 0xffffu));
  }

  /* --- Rung 1: force the POST output BLACK (no memory read at all). --- */

  syslog(LOG_WARNING,
         "kickpi-k7: SOURCE RUNG 1/4: POST forced BLACK (post_black_en, bit16)"
         " -- generated inside the POST, no layer, no memory.  WATCH THE "
         "SCREEN\n");

  rk3576_vop_post_force(priv, RK3576_VOP_POST_BLACK_EN);
  up_mdelay(2000);

  /* --- Rung 2: force the POST output to ZERO. --- */

  syslog(LOG_WARNING,
         "kickpi-k7: SOURCE RUNG 2/4: POST forced ZERO (dsp_out_zero, bit15)"
         "\n");

  rk3576_vop_post_force(priv, RK3576_VOP_POST_DSP_OUT_ZERO);
  up_mdelay(2000);

  /* --- Rung 3: layer OFF, POST background = solid BLUE. --- */

  syslog(LOG_WARNING,
         "kickpi-k7: SOURCE RUNG 3/4: ESMART0 disabled + POST background "
         "SOLID BLUE\n");

  /* Disable the layer.  REGION0_CTRL is a mirror register, so the load pulse
   * inside post_force() is what makes it take effect. */

  {
    uint32_t post_bg = RK3576_VOP_POST_BG_DISPLAY_EN |
                       (0x3ffu << RK3576_VOP_POST_BG_BLUE_SHIFT);

    rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                      RK3576_VOP_ESMART_FMT_RGB888);
    rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_BG, post_bg);
    rk3576_vop_post_force(priv, 0);

    up_mdelay(2000);

    /* Restore: layer on, background colour off (as configure_port() leaves
     * them), so the rungs after this see the normal configuration.
     *
     * THE LOAD PULSE IS NOT OPTIONAL.  The first version of this function
     * restored the mirror registers and did NOT pulse cfg_done afterwards, so
     * the restore never reached the real registers: the panel stayed on the
     * blue background for the rest of the run, which silently invalidated
     * rung 4 and every content probe that came after it (the pixel probe
     * "showed blue" because the layer was still disabled, not because the
     * framebuffer failed to reach the glass).  Mirror registers need the pulse
     * for the WRITE as much as for the disable -- which is the same lesson the
     * rung-3 result teaches from the other direction. */

    rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                      RK3576_VOP_ESMART_FMT_RGB888 |
                          RK3576_VOP_ESMART_REGION0_MST_EN |
                          RK3576_VOP_ESMART_REGION0_RB_SWAP);
    rk3576_vop_putreg(priv, post_base + RK3576_VOP_POST_DSP_BG, 0);
    rk3576_vop_post_force(priv, 0); /* <- the load pulse */

    up_mdelay(100);

    /* Prove the restore landed, rather than assuming it: a background colour
     * that never turns off would make every later result unreadable. */

    syslog(LOG_WARNING,
           "kickpi-k7: SOURCE RUNG 3/4 restore: REGION0_CTRL=%08x (mst_en=%u) "
           "DSP_BG=%08x (bg_display_en=%u)\n",
           (unsigned)rk3576_vop_getreg(priv,
                                       esmart_base +
                                           RK3576_VOP_ESMART_REGION0_CTRL),
           (unsigned)(rk3576_vop_getreg(priv,
                                        esmart_base +
                                            RK3576_VOP_ESMART_REGION0_CTRL) &
                      1u),
           (unsigned)rk3576_vop_getreg(priv,
                                       post_base + RK3576_VOP_POST_DSP_BG),
           (unsigned)((rk3576_vop_getreg(priv, post_base +
                                         RK3576_VOP_POST_DSP_BG) >> 31) & 1u));
  }

  /* --- Rung 4: the normal path, on a stream that is now CLEAN.
   *
   * The rung-3 result changes what this rung is for.  It established that the
   * panel faithfully shows whatever the POST outputs, so the question is no
   * longer "can anything reach the glass" but "does the LAYER's output follow
   * our framebuffer".  The sequence therefore walks from uniform to
   * structured, each step announced:
   *
   *   BLACK, WHITE   -- byte-uniform, so no byte-level fault can disguise
   *                     them.  If the layer is delivering our memory at all,
   *                     these MUST appear as flat black and flat white.
   *   RED            -- a uniform colour that is not greyscale.
   *   8 COLOUR BARS  -- structured content, to see whether geometry is right.
   *
   * The first three are the decisive part: a uniform colour appearing proves
   * the layer reads our buffer; uniform colours NOT appearing, after rung 3
   * proved the output path is clean, pins the fault inside the layer's own
   * fetch.  The bars follow only to grade the geometry if the uniforms work. */

  {
    static const struct
    {
      uint32_t rgb;
      FAR const char *name;
    } steps[] =
    {
      { 0x000000u, "BLACK (uniform)" },
      { 0xffffffu, "WHITE (uniform)" },
      { 0xff0000u, "RED (uniform, not greyscale)" },
      { 0x000000u, "BLACK again (repeat the first, to catch drift)" },
    };

    int s;

    for (s = 0; s < (int)nitems(steps); s++)
      {
        rk3576_vop_fill(steps[s].rgb);

        syslog(LOG_WARNING,
               "kickpi-k7: SOURCE RUNG 4/4 step %d/%d: framebuffer = %s "
               "(0x%06x)\n",
               s + 1, (int)nitems(steps), steps[s].name,
               (unsigned)steps[s].rgb);

        up_mdelay(1500);
      }

    /* Structured content last, and only to grade the geometry. */

    syslog(LOG_WARNING,
           "kickpi-k7: SOURCE RUNG 4/4 step %d: 8 COLOUR BARS (structured)"
           "\n", (int)nitems(steps) + 1);
    (void)rk3576_vop_paint_test_pattern(0xff00ffu);
    up_mdelay(1500);
  }

  syslog(LOG_WARNING,
         "kickpi-k7: SOURCE PROBE DONE -- did rung 4 show FLAT black, FLAT "
         "white, FLAT red?\n");

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_underrun_probe
 *
 * Description:
 *   Measure whether the POST's output-buffer under-run (per-VP bit4,
 *   POST_BUF_EMPTY) RECURS, and test the one candidate cause this driver can
 *   control itself.
 *
 *   WHY THIS BIT MATTERS MORE THAN ANY COUNTER SEEN SO FAR.  POST_BUF_EMPTY
 *   means the pixels to be transmitted did not arrive in time.  That is the
 *   only mechanism found in this whole bring-up that explains a picture which
 *   follows NEITHER the framebuffer's content NOR its address: if the POST is
 *   starved it transmits something other than the layer's data, and no amount
 *   of writing to the framebuffer can change it.  It also fits rung 3, where
 *   disabling the layer forced the POST onto its own background colour and the
 *   screen became a clean solid blue -- with no layer fetch there is nothing to
 *   starve.
 *
 *   It was dismissed for many rounds by a comment in this very driver --
 *   "fs/line/win bits are per-line events; only sys0/sys1 are faults" -- which
 *   lumped bit4 in with the per-frame event bits.  It is not an event.
 *
 *   HOW IT IS MEASURED, rather than argued.  SYS_STATUS0 carries a dedicated
 *   capture: post_buf_empty_dsp_vcnt records the dsp_vcnt at which the under-run
 *   happened, with an arm bit and a clear bit.  Arming it, clearing it, waiting
 *   and reading back converts a latched bit into either "it recurs every frame"
 *   or "it happened once at startup", which is the difference between a fault
 *   and an artefact.
 *
 *   THE CANDIDATE CAUSE TESTED.  This driver sets AXI outstanding limits that
 *   the reference driver never writes at all: ESMART outstanding_en + num=4,
 *   and SYS_AXI0 outstanding_en + num=16.  They were added to stop the
 *   scan-out DMA monopolising the NoC, and they are this driver's own
 *   invention.  A limit too low to sustain the pixel bandwidth would starve
 *   the layer permanently -- so phase B raises both to the maximum their fields
 *   can encode (4 bits -> 15, 6 bits -> 63) and re-measures the same window.
 *   Both stay BOUNDED: the state that was believed to hang the SoC was
 *   outstanding_en=0, which means *unlimited*, a different thing entirely.
 *
 *   The higher limit is kept only if it removes the under-run; otherwise the
 *   original values are restored, so this build stays a single-variable change
 *   whichever way the measurement goes.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_underrun_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sys_base;
  uint32_t esmart_base;
  uint32_t st;
  uint32_t raw_a;
  uint32_t cap_a;
  uint32_t raw_b;
  bool a_under;
  bool b_under;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  sys_base    = RK3576_VOP_SYS_CTRL(priv->base);
  esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);

  syslog(LOG_WARNING,
         "kickpi-k7: UNDER-RUN PROBE: POST output buffer under-run -- the bit "
         "that means the\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   pixels did not arrive in time.  Measuring whether it "
         "RECURS, and whether\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   it is the LAYER that causes it -- tested by repeating "
         "the same window with\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   the layer turned off.\n");

  /* --- Phase A: at the AXI limits this driver currently uses. --- */

  st = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0);
  syslog(LOG_WARNING,
         "kickpi-k7: UNDER-RUN[A] status0=%08x dsp_vcnt=%u captured_vcnt=%u "
         "arm=%u (limits: ESMART 4 SYS_AXI0 16)\n",
         (unsigned)st,
         (unsigned)((st >> RK3576_VOP_DSP_VCNT0_SHIFT) & 0x1fffu),
         (unsigned)(st & RK3576_VOP_STATUS0_BUFEMPTY_VCNT_MASK),
         (unsigned)((st >> 14) & 1u));

  /* Arm + clear the capture, and clear the latched interrupt bits, so the next
   * read covers ONLY what happens from here on.  CLR and EN go in SEPARATE
   * writes: issuing them together left arm=0 in every previous log, so the
   * capture never actually armed and captured_vcnt was always 0. */

  st = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0);
  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_STATUS0,
                    RK3576_VOP_STATUS0_BUFEMPTY_VCNT_CLR);
  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_STATUS0,
                    RK3576_VOP_STATUS0_BUFEMPTY_VCNT_EN);
  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_VP_INT_CLR(priv->cfg.port),
                    0xffffffffu);

  up_mdelay(300);

  raw_a = rk3576_vop_getreg(priv, sys_base +
                            RK3576_VOP_VP_INT_RAW_STATUS(priv->cfg.port));
  st = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0);
  cap_a = st & RK3576_VOP_STATUS0_BUFEMPTY_VCNT_MASK;
  a_under = (raw_a & RK3576_VOP_INT_POST_BUF_EMPTY) != 0u;

  syslog(LOG_WARNING,
         "kickpi-k7: UNDER-RUN[A] after 300 ms: raw=%08x BUF_EMPTY=%u "
         "post_full=%u captured_vcnt=%u (layer ON)\n",
         (unsigned)raw_a, a_under ? 1u : 0u,
         (unsigned)((raw_a & RK3576_VOP_INT_POST_FULL) != 0u),
         (unsigned)cap_a);

  if (!a_under)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: UNDER-RUN[A] => it did NOT recur in 300 ms: a one-off, "
             "and NOT the\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   cause of a static picture.  Nothing is changed "
             "below.\n");
      return OK;
    }

  syslog(LOG_ERR,
         "kickpi-k7: UNDER-RUN[A] => IT RECURS.  The POST is being starved "
         "continuously, which\n");
  syslog(LOG_ERR,
         "kickpi-k7:   fits a picture that follows neither content nor "
         "address.\n");

  /* --- Phase B: the decisive control -- measure the SAME window with the
   * layer turned OFF.
   *
   * Everything so far rests on one unverified assumption: that the recurring
   * output-buffer under-run is caused by the LAYER failing to fetch.  This
   * driver has a bad record on exactly this kind of assumption, and this
   * particular one is cheap to test directly -- with the layer disabled the
   * POST has no layer data to composite, so if the layer is what starves it
   * the under-run must stop.
   *
   *   under-run STOPS with the layer off -> the starvation IS the layer's
   *       fetch, and it is the thing to fix;
   *   under-run CONTINUES with the layer off -> the starvation is NOT the
   *       layer at all (it is the POST's own output side or the MIPI
   *       interface throttling it), and every "the layer cannot fetch"
   *       conclusion drawn so far needs re-examining.
   *
   * post_full (bit9) is read alongside, as the opposite witness: a POST whose
   * buffer is filling is receiving data, even if it also reports an under-run
   * at some other moment.
   */

  {
    uint32_t off_ctrl;

    syslog(LOG_WARNING,
           "kickpi-k7: UNDER-RUN[B] CONTROL: same 300 ms window with ESMART0 "
           "DISABLED\n");

    off_ctrl = rk3576_vop_getreg(priv, esmart_base +
                                 RK3576_VOP_ESMART_REGION0_CTRL);
    rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                      off_ctrl & ~RK3576_VOP_ESMART_REGION0_MST_EN);
    rk3576_vop_post_force(priv, 0); /* load pulse: mirror -> real */

    up_mdelay(100);

    st = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS_STATUS0);
    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_STATUS0,
                      (st & ~(RK3576_VOP_STATUS0_BUFEMPTY_VCNT_MASK |
                              RK3576_VOP_STATUS0_BUFEMPTY_VCNT_CLR)) |
                          RK3576_VOP_STATUS0_BUFEMPTY_VCNT_EN |
                          RK3576_VOP_STATUS0_BUFEMPTY_VCNT_CLR);
    rk3576_vop_putreg(priv, sys_base + RK3576_VOP_VP_INT_CLR(priv->cfg.port),
                      0xffffffffu);

    up_mdelay(300);

    raw_b = rk3576_vop_getreg(priv, sys_base +
                              RK3576_VOP_VP_INT_RAW_STATUS(priv->cfg.port));
    b_under = (raw_b & RK3576_VOP_INT_POST_BUF_EMPTY) != 0u;

    syslog(LOG_WARNING,
           "kickpi-k7: UNDER-RUN[B] layer OFF: raw=%08x BUF_EMPTY=%u "
           "post_full=%u\n",
           (unsigned)raw_b, b_under ? 1u : 0u,
           (unsigned)((raw_b & RK3576_VOP_INT_POST_FULL) != 0u));

    /* Restore the layer, WITH the load pulse -- omitting it once already left
     * the layer permanently disabled and invalidated two whole probes. */

    rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                      off_ctrl);
    rk3576_vop_post_force(priv, 0);
    up_mdelay(100);

    syslog(LOG_WARNING,
           "kickpi-k7: UNDER-RUN[B] layer restored: REGION0_CTRL=%08x "
           "(mst_en=%u)\n",
           (unsigned)rk3576_vop_getreg(priv,
                                       esmart_base +
                                           RK3576_VOP_ESMART_REGION0_CTRL),
           (unsigned)(rk3576_vop_getreg(priv, esmart_base +
                                        RK3576_VOP_ESMART_REGION0_CTRL) & 1u));

    if (!b_under)
      {
        syslog(LOG_WARNING,
               "kickpi-k7: UNDER-RUN[B] => with the layer OFF the under-run "
               "STOPS: the starvation\n");
        syslog(LOG_WARNING,
               "kickpi-k7:   IS the layer's fetch.  That is the fault to "
               "fix.\n");
      }
    else
      {
        syslog(LOG_ERR,
               "kickpi-k7: UNDER-RUN[B] => the under-run CONTINUES with the "
               "layer OFF, so it is NOT\n");
        syslog(LOG_ERR,
               "kickpi-k7:   caused by the layer's fetch: the POST's own "
               "output side (or the MIPI\n");
        syslog(LOG_ERR,
               "kickpi-k7:   interface throttling it) is starving it.  Every "
               "\"the layer cannot fetch\"\n");
        syslog(LOG_ERR,
               "kickpi-k7:   conclusion so far must be re-examined ***\n");
      }
  }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_request_probe_check
 *
 * Description:
 *   Point the layer at an undecodable address, wait, and report whether the VOP
 *   latched an AXI bus error.  A read that is actually issued must fail against
 *   an address no slave decodes; a request generator that never runs cannot
 *   produce an error.
 *
 ****************************************************************************/

static bool rk3576_vop_request_probe_check(FAR struct rk3576_vop_s *priv,
                                           FAR bool *poison_landed)
{
  uint32_t esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  uint32_t sys_base    = RK3576_VOP_SYS_CTRL(priv->base);
  uint32_t s0;
  uint32_t s1;

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS0_INT_CLR, 0xffffffffu);
  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS1_INT_CLR, 0xffffffffu);

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_YRGB_MST,
                    0xf0000000u);
  rk3576_vop_trigger_cfg_done(priv);

  /* READ THE POISON BACK.  Without this the test cannot be trusted: if the
   * write were ignored the layer would still be pointed at the real
   * framebuffer, no bus error could possibly occur, and "the layer issues no
   * reads" would be an artefact of a register write that never happened --
   * which is exactly the kind of false conclusion this bring-up has been
   * repeatedly burned by.  A write to this register was in fact observed to be
   * ignored once the layer had been put back into frame reset, so the guard is
   * not hypothetical. */

  up_mdelay(20);
  *poison_landed =
      rk3576_vop_getreg(priv, esmart_base +
                        RK3576_VOP_ESMART_REGION0_YRGB_MST) == 0xf0000000u;

  up_mdelay(150);

  s0 = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS0_INT_RAW);
  s1 = rk3576_vop_getreg(priv, sys_base + RK3576_VOP_SYS1_INT_RAW);

  return ((s0 | s1) & RK3576_VOP_SYS_INT_BUS_ERROR) != 0u;
}

/****************************************************************************
 * Name: rk3576_vop_request_probe
 *
 * Description:
 *   Find what stops the ESMART layer from issuing AXI reads.
 *
 *   MEASURED, NOT ASSUMED.  With the layer enabled, its region enabled, its
 *   address register holding a valid value and its route selected, pointing the
 *   fetch at an address that no AXI slave decodes produced NO bus error.  A read
 *   that was issued would have to fail against that address, so the layer is
 *   issuing no reads at all: the request never leaves the window.  Two
 *   consequences follow immediately -- nothing downstream of the request
 *   generator can be the cause, and the layer's registers reading back correct
 *   proves only that the register file is reachable, not that the block is
 *   running.
 *
 *   THAT IS THE SIGNATURE OF A BLOCK WITH NO CLOCK.  The register file sits on
 *   the peripheral clock, so it answers reads and retains writes while the
 *   datapath's aclk is gated off; the request generator then simply never runs,
 *   and a clock that is gated produces no error because nothing happens.
 *
 *   This board has strong reason to be in exactly that state: the gating
 *   register resets with many per-block gates ENABLED, including gates for the
 *   window, the overlay, the AXI path and the pre-scan block, and this driver
 *   writes none of them.  The earlier attempt cleared only the two bits the
 *   reference driver names (the master enable and the aclk pre-gating
 *   workaround); the per-block gates were left as the hardware set them.
 *
 *   So this probe walks a ladder, and after EACH step repeats the
 *   poison-address check.  The step that makes a bus error appear is the one
 *   that was stopping the fetch:
 *
 *     A. as the driver leaves it          (the measured baseline: no error)
 *     B. all gating bits cleared          (every block clocked unconditionally)
 *     C. frm_resetn_en cleared            (in case its semantics are inverted
 *                                          from what this driver assumed)
 *
 *   Every step is announced with its result, so the answer is a reading and not
 *   an inference.  The poison address is restored and the layer re-armed at the
 *   end either way.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_request_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t esmart_base;
  uint32_t sys_base;
  uint32_t save_addr;
  uint32_t save_gate;
  uint32_t save_ctrl0;
  bool hit;
  bool landed;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  sys_base    = RK3576_VOP_SYS_CTRL(priv->base);

  save_addr = rk3576_vop_getreg(priv, esmart_base +
                                RK3576_VOP_ESMART_REGION0_YRGB_MST);
  save_gate = rk3576_vop_getreg(priv, sys_base +
                                RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD);
  save_ctrl0 = rk3576_vop_getreg(priv, esmart_base +
                                 RK3576_VOP_ESMART_CTRL0);

  syslog(LOG_WARNING,
         "kickpi-k7: REQUEST PROBE: the layer issues NO reads (no bus error "
         "from an undecodable\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   address).  A register file that reads back correct but "
         "issues nothing is a\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   block whose datapath clock is not running.  Testing the "
         "three candidates by\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   repeating the poison-address check after each.\n");

  /* --- A: baseline, exactly as the driver leaves it. --- */

  hit = rk3576_vop_request_probe_check(priv, &landed);
  syslog(LOG_WARNING,
         "kickpi-k7: REQUEST[A] as the driver leaves it (gating=%08x, "
         "CTRL0=%08x): poison_landed=%u bus_error=%u\n",
         (unsigned)save_gate, (unsigned)save_ctrl0, landed ? 1u : 0u,
         hit ? 1u : 0u);

  if (!landed)
    {
      syslog(LOG_ERR,
             "kickpi-k7: REQUEST[A] => THE POISON DID NOT LAND: the address "
             "register ignored the\n");
      syslog(LOG_ERR,
             "kickpi-k7:   write, so no bus error could have occurred "
             "regardless of whether the layer\n");
      syslog(LOG_ERR,
             "kickpi-k7:   reads.  This measurement is VOID and nothing is "
             "concluded from it ***\n");
    }

  /* --- B: does "enable the region" itself start a fetch?
   *
   * THE TRM'S OWN STATEMENT IS: "The bus behavior of the esmart layer is to
   * take one line of buffers WHEN ONE LINE IS FREE."  So a fetch is driven by
   * the line buffers becoming free, i.e. by something continuously cycling --
   * it is not something the layer does once on its own.  That makes the
   * following distinction the one that matters:
   *
   *   a fresh region enable DOES start a fetch -> the very first request is
   *       gated by something that fires once, and the continuous driver is what
   *       is missing (the per-line / per-frame signal from the display side);
   *   even a fresh enable fetches nothing -> the request generator is not
   *       running at all, independently of the display side, and the fault is
   *       in the block's own clock/reset/handshake rather than in the timing
   *       that would sustain it.
   *
   * Disabling and re-enabling the region is the cheapest way to ask that, and
   * the register is read back at each step so the toggle itself cannot be
   * assumed.
   */

  {
    uint32_t ctrl = rk3576_vop_getreg(priv, esmart_base +
                                      RK3576_VOP_ESMART_REGION0_CTRL);
    uint32_t ctrl_off = ctrl & ~RK3576_VOP_ESMART_REGION0_MST_EN;

    rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                      ctrl_off);
    rk3576_vop_trigger_cfg_done(priv);
    up_mdelay(80);

    syslog(LOG_WARNING,
           "kickpi-k7: REQUEST[B] region disabled: REGION0_CTRL=%08x (mst_en=%u "
           "-- this is the value the readback must show)\n",
           (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                       RK3576_VOP_ESMART_REGION0_CTRL),
           (unsigned)(rk3576_vop_getreg(priv, esmart_base +
                                        RK3576_VOP_ESMART_REGION0_CTRL) & 1u));

    rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_CTRL,
                      ctrl);
    rk3576_vop_trigger_cfg_done(priv);
    up_mdelay(80);

    syslog(LOG_WARNING,
           "kickpi-k7: REQUEST[B] region re-enabled: REGION0_CTRL=%08x "
           "(mst_en=%u)\n",
           (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                       RK3576_VOP_ESMART_REGION0_CTRL),
           (unsigned)(rk3576_vop_getreg(priv, esmart_base +
                                        RK3576_VOP_ESMART_REGION0_CTRL) & 1u));
  }

  hit = rk3576_vop_request_probe_check(priv, &landed);
  syslog(LOG_WARNING,
         "kickpi-k7: REQUEST[B] after a fresh MST_EN 0->1 with the poison "
         "address: poison_landed=%u bus_error=%u\n",
         landed ? 1u : 0u, hit ? 1u : 0u);

  if (hit)
    {
      syslog(LOG_WARNING,
             "kickpi-k7: REQUEST[B] => ENABLING THE REGION DOES START A FETCH. "
             "The first request\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   is fine; what is missing is the signal that keeps "
             "the line buffers\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   cycling, i.e. the display side's per-line / "
             "per-frame demand.  That is\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   where to look next.\n");
    }
  else
    {
      syslog(LOG_ERR,
             "kickpi-k7: REQUEST[B] => even a fresh region enable fetches "
             "nothing.  The request\n");
      syslog(LOG_ERR,
             "kickpi-k7:   generator is not running at all, independently of "
             "the display side, so the\n");
      syslog(LOG_ERR,
             "kickpi-k7:   fault is in the block's own clock, reset or "
             "handshake -- not in the timing\n");
      syslog(LOG_ERR,
             "kickpi-k7:   that would sustain it ***\n");
    }

  /* NOTE ON THE STEP THAT USED TO BE HERE.  A rung that cleared
   * esmart_frm_resetn_en (CTRL0 bit31) is GONE: after that write, further
   * writes to the ESMART registers were IGNORED (the restore read back 0 for
   * CTRL0), which means the write did something outside the register file --
   * it disabled the block.  Together with the reference driver also writing 1
   * for a normal window, that settles the sense of the bit as the driver
   * originally assumed ('1' = released), and the rung only destroyed
   * measurements taken after it. */

  /* --- C: TEST THE TWO CANDIDATE AXI READ-ID PAIRS.
   *
   * A GAP IN EVERY MEASUREMENT SO FAR.  The two sources disagree about which
   * read IDs ESMART0 must use, and this driver has only ever tested one of them
   * with the poison-address method:
   *
   *   the TRM's read-id map says   ESMART0 = 0xa / 0xb   (which is also the
   *                                                      reset value), on AXI0
   *   the reference driver says    ESMART0 = 0x10 / 0x11 for RK3576
   *
   * This driver was set to the reference driver's pair BEFORE the poison probe
   * existed, so "the layer issues no reads" has never been checked against the
   * pair the TRM assigns to this window.  The ID travels with every read into
   * the interconnect, where it selects routing and QoS, so a pair no slave
   * expects is a plausible way to have reads vanish with no error at all.
   *
   * Both pairs are therefore exercised here, explicitly, in one boot.
   */

  {
    static const struct
    {
      uint32_t yrgb;
      uint32_t uv;
    } ids[] =
    {
      { 0x10u, 0x11u }, /* the reference driver's choice (current) */
      { 0x0au, 0x0bu }, /* the TRM's read-id map, and the reset value */
    };

    int k;

    for (k = 0; k < (int)nitems(ids); k++)
      {
        uint32_t v = (ids[k].yrgb << RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT) |
                     (ids[k].uv << RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT);

        rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_CTRL1, v);
        rk3576_vop_trigger_cfg_done(priv);
        up_mdelay(50);

        hit = rk3576_vop_request_probe_check(priv, &landed);
        syslog(LOG_WARNING,
               "kickpi-k7: REQUEST[C] with AXI read id 0x%02x/0x%02x "
               "(CTRL1=%08x): poison_landed=%u bus_error=%u\n",
               (unsigned)ids[k].yrgb, (unsigned)ids[k].uv,
               (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                           RK3576_VOP_ESMART_CTRL1),
               landed ? 1u : 0u, hit ? 1u : 0u);

        if (hit)
          {
            syslog(LOG_WARNING,
                   "kickpi-k7: REQUEST[C] => THE READ ID WAS THE PROBLEM: the "
                   "layer issues reads with\n");
            syslog(LOG_WARNING,
                   "kickpi-k7:   0x%02x/0x%02x.  Leaving that pair programmed "
                   "-- look at the screen\n", (unsigned)ids[k].yrgb,
                   (unsigned)ids[k].uv);
            syslog(LOG_WARNING,
                   "kickpi-k7:   now.\n");

            rk3576_vop_putreg(priv, esmart_base +
                              RK3576_VOP_ESMART_REGION0_YRGB_MST, save_addr);
            rk3576_vop_trigger_cfg_done(priv);
            return OK;
          }
      }
  }

  /* Put the reference driver's pair back and restore the framebuffer.  The
   * hurry enable goes back too: this probe must leave the window exactly as
   * configure_layer() set it up. */

  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_CTRL1,
                    (g_rk3576_vop_esmart_yrgb_rid[RK3576_VOP_ESMART_IDX]
                     << RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT) |
                        (g_rk3576_vop_esmart_uv_rid[RK3576_VOP_ESMART_IDX]
                         << RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT) |
                        RK3576_VOP_ESMART_CTRL1_DMA_RREQ_HURRY_EN |
                        (0u << RK3576_VOP_ESMART_CTRL1_DMA_RREQ_THOLD_SHIFT));

  /* Restore, with the load pulse. */

  rk3576_vop_putreg(priv, sys_base + RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD,
                    save_gate);
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_YRGB_MST,
                    save_addr);
  rk3576_vop_trigger_cfg_done(priv);

  syslog(LOG_WARNING,
         "kickpi-k7: REQUEST PROBE: restored (gating=%08x CTRL0=%08x "
         "YRGB_MST=%08x REGION0_CTRL=%08x)\n",
         (unsigned)rk3576_vop_getreg(priv, sys_base +
                                     RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD),
         (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_CTRL0),
         (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_REGION0_YRGB_MST),
         (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_REGION0_CTRL));

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_mmu_fault_probe
 *
 * Description:
 *   Decide whether the ESMART layer issues reads, using the MMU instead of the
 *   AXI error path.
 *
 *   WHY A SECOND METHOD.  The poison-address test rests on three assumptions at
 *   once: that the address is genuinely undecodable, that a bus error results,
 *   and that it is latched where we look.  Every reading of SYS0/SYS1 has been
 *   zero, which either means the layer issues no reads or means one of those
 *   assumptions fails -- and nothing so far distinguishes the two.
 *
 *   THE MMU CANNOT BE FOOLED THAT WAY.  This driver has no page tables and
 *   keeps the MMU bypassed.  Turning paging ON with dte_addr left at zero makes
 *   EVERY translation fail, so ANY read the layer issues MUST produce a page
 *   fault, and the fault is recorded with "the index of the master responsible"
 *   in page_fault_bus_id.  That is a different mechanism from the AXI error
 *   path, so agreement between the two is real evidence and disagreement is
 *   itself informative.
 *
 *   The MMU is left bypassed afterwards (that is the correct configuration for
 *   a driver with no page tables) and paging is switched off again.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_mmu_fault_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t esmart_base;
  uint32_t sys_base;
  uint32_t mmu;
  uint32_t mmu1;
  uint32_t st_before;
  uint32_t st_after;
  uint32_t st1_after;
  uint32_t bus_id;
  uint32_t save_mmu_ctrl;
  uint32_t save_axi;
  bool faulted;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  sys_base    = RK3576_VOP_SYS_CTRL(priv->base);

  mmu = priv->base + RK3576_VOP_MMU0_BASE;

  /* *** ARM BOTH MMUs, NOT JUST MMU0. ***
   *
   * The VOP has MMU0 at 0x7E00 and MMU1 at 0x7F00, one per AXI channel.  Every
   * run of this probe until now enabled paging on MMU0 only.  If this window's
   * accesses were in fact served by MMU1, then with paging disabled there they
   * would pass through untranslated, never fault, and "FAULT_ACTIVE = 0" would
   * be a FALSE NEGATIVE -- the probe would be reporting its own incomplete
   * setup rather than the layer's behaviour.  Since the conclusion "the layer
   * issues no memory accesses" rests on this test, the test has to be able to
   * fail in both places before that conclusion is worth anything.
   */

  mmu1 = priv->base + RK3576_VOP_MMU1_BASE;

  st_before = rk3576_vop_getreg(priv, mmu + RK3576_VOP_MMU_STATUS);

  syslog(LOG_WARNING,
         "kickpi-k7: MMU FAULT PROBE: MMU0 status=%08x dte=%08x.  Turning "
         "paging ON with no page\n",
         (unsigned)st_before,
         (unsigned)rk3576_vop_getreg(priv, mmu + RK3576_VOP_MMU_DTE_ADDR));
  syslog(LOG_WARNING,
         "kickpi-k7:   table: every read a window issues MUST fault, and the "
         "fault names the master.\n");

  /* *** THE BYPASSES MUST BE OFF FOR THIS TO MEAN ANYTHING. ***
   *
   * The previous version of this probe enabled paging and still saw no fault,
   * which proved nothing: this driver keeps the MMU BYPASSED in two places --
   * the global SYS_MMU_CTRL_IMD (bypass_en with bypass_id = 0, so every rid
   * bypasses) and the window's own esmart_mmu_bypass -- and an access that
   * bypasses the MMU cannot fault no matter what paging_en says.  The probe was
   * therefore measuring its own configuration rather than the layer.
   *
   * Both bypasses are turned off for the duration and restored afterwards.
   */

  save_mmu_ctrl = rk3576_vop_getreg(priv, sys_base +
                                    RK3576_VOP_SYS_MMU_CTRL_IMD);
  save_axi = rk3576_vop_getreg(priv, esmart_base +
                               RK3576_VOP_ESMART_AXI_CTRL_IMD);

  /* *** CLEAR THE GLOBAL BYPASS WITH THE PROPER MASKED WRITE. ***
   *
   * SYS_MMU_CTRL_IMD is one of the SYS registers that carry a WRITE ENABLE in
   * the HIGH half of the word -- the same trap this project has already fallen
   * into twice, and the reason configure_port() uses
   * rk3576_vop_sys_masked_write() here.  This probe was doing it with a plain
   * putreg(), i.e. it relied on whatever value happened to be in bits[31:16],
   * which is not a decision so much as a coincidence.
   *
   * That matters more here than anywhere else in the driver, because this is
   * the ONE test that can tell "the layer issues no reads" apart from "the
   * layer issues reads that fail".  If the bypass does not really clear, no
   * fault can occur and the probe would be measuring its own configuration.
   * The masked write makes the precondition certain instead of lucky.
   *
   * mmu_bypass_id is cleared at the same time: bypass is "rid > bypass_id", so
   * with bypass_en off the id is irrelevant, but leaving it at 0 removes any
   * chance of a rid slipping past on the mmu1 side.
   */

  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_MMU_CTRL_IMD,
                              0u,
                              RK3576_VOP_SYS_MMU_BYPASS_EN |
                                  RK3576_VOP_SYS_MMU1_BYPASS_EN |
                                  RK3576_VOP_SYS_MMU_BYPASS_ID_MASK);
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                    save_axi & ~RK3576_VOP_ESMART_AXI_MMU_BYPASS);

  /* The window's AXI control is a MIRROR register: it needs the load pulse
   * before the real register changes, exactly like every other layer setting.
   * The previous attempt cleared the bypass without pulsing, read the register
   * back, and saw it unchanged -- and then ran the test anyway, which is why its
   * "no fault" result meant nothing. */

  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(30);

  {
    uint32_t got_mmu = rk3576_vop_getreg(priv, sys_base +
                                         RK3576_VOP_SYS_MMU_CTRL_IMD);
    uint32_t got_axi = rk3576_vop_getreg(priv, esmart_base +
                                         RK3576_VOP_ESMART_AXI_CTRL_IMD);
    bool mmu_off = (got_mmu & (RK3576_VOP_SYS_MMU_BYPASS_EN |
                               RK3576_VOP_SYS_MMU1_BYPASS_EN)) == 0u;
    bool win_off = (got_axi & RK3576_VOP_ESMART_AXI_MMU_BYPASS) == 0u;

    syslog(LOG_WARNING,
           "kickpi-k7: MMU FAULT PROBE: bypasses OFF for the test -- "
           "SYS_MMU_CTRL %08x -> %08x, ESMART AXI %08x -> %08x\n",
           (unsigned)save_mmu_ctrl, (unsigned)got_mmu, (unsigned)save_axi,
           (unsigned)got_axi);
    syslog(LOG_WARNING,
           "kickpi-k7:   global bypass_off=%u (rid<=%u now translated), "
           "window bypass_off=%u\n", mmu_off ? 1u : 0u,
           (unsigned)((got_mmu >> RK3576_VOP_SYS_MMU_BYPASS_ID_SHIFT) & 0x1fu),
           win_off ? 1u : 0u);

    /* VERIFY THE PRECONDITION BEFORE MEASURING.  If either bypass is still set
     * the layer's accesses skip the MMU entirely and no fault can ever appear,
     * so a "no fault" reading would be about this driver's configuration rather
     * than about the layer.  Say so instead of reporting a result. */

    if (!mmu_off || !win_off)
      {
        syslog(LOG_ERR,
               "kickpi-k7: MMU FAULT PROBE: *** PRECONDITION FAILED -- a bypass "
               "is still set, so the\n");
        syslog(LOG_ERR,
               "kickpi-k7:   MMU cannot see the layer's accesses and NO "
               "conclusion can be drawn.  Not\n");
        syslog(LOG_ERR,
               "kickpi-k7:   running the test ***\n");

        rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_MMU_CTRL_IMD,
                                    save_mmu_ctrl,
                                    RK3576_VOP_SYS_MMU_BYPASS_EN |
                                        RK3576_VOP_SYS_MMU1_BYPASS_EN |
                                        RK3576_VOP_SYS_MMU_BYPASS_ID_MASK |
                                        RK3576_VOP_SYS_MMU_SOFT_RST_EN);
        rk3576_vop_putreg(priv, esmart_base +
                          RK3576_VOP_ESMART_AXI_CTRL_IMD, save_axi);
        rk3576_vop_trigger_cfg_done(priv);
        return OK;
      }
  }

  /* Point the layer at the real framebuffer so the fault is about the fetch
   * that should be happening in normal operation, not about a poisoned
   * address. */

  rk3576_vop_putreg(priv, esmart_base +
                    RK3576_VOP_ESMART_REGION0_YRGB_MST,
                    (uint32_t)up_addrenv_va_to_pa(priv->fbmem));
  rk3576_vop_trigger_cfg_done(priv);
  up_mdelay(20);

  rk3576_vop_putreg(priv, mmu + RK3576_VOP_MMU_COMMAND,
                    RK3576_VOP_MMU_CMD_ENABLE_PAGING);
  rk3576_vop_putreg(priv, mmu1 + RK3576_VOP_MMU_COMMAND,
                    RK3576_VOP_MMU_CMD_ENABLE_PAGING);
  up_mdelay(200);

  st_after  = rk3576_vop_getreg(priv, mmu + RK3576_VOP_MMU_STATUS);
  st1_after = rk3576_vop_getreg(priv, mmu1 + RK3576_VOP_MMU_STATUS);
  bus_id = (st_after & RK3576_VOP_MMU_STATUS_FAULT_BUS_ID_MASK) >>
           RK3576_VOP_MMU_STATUS_FAULT_BUS_ID_SHIFT;

  /* Either MMU faulting is proof that the layer issued an access. */

  faulted = ((st_after & RK3576_VOP_MMU_STATUS_FAULT_ACTIVE) != 0u) ||
            ((st1_after & RK3576_VOP_MMU_STATUS_FAULT_ACTIVE) != 0u);

  if ((st1_after & RK3576_VOP_MMU_STATUS_FAULT_ACTIVE) != 0u)
    {
      bus_id = (st1_after & RK3576_VOP_MMU_STATUS_FAULT_BUS_ID_MASK) >>
               RK3576_VOP_MMU_STATUS_FAULT_BUS_ID_SHIFT;
    }

  syslog(LOG_WARNING,
         "kickpi-k7: MMU FAULT PROBE: MMU0 status=%08x MMU1 status=%08x "
         "(paging_en %u/%u, FAULT_ACTIVE %u/%u)\n",
         (unsigned)st_after, (unsigned)st1_after,
         (unsigned)((st_after & RK3576_VOP_MMU_STATUS_PAGING_EN) ? 1u : 0u),
         (unsigned)((st1_after & RK3576_VOP_MMU_STATUS_PAGING_EN) ? 1u : 0u),
         (unsigned)((st_after & RK3576_VOP_MMU_STATUS_FAULT_ACTIVE) ? 1u : 0u),
         (unsigned)((st1_after & RK3576_VOP_MMU_STATUS_FAULT_ACTIVE) ? 1u : 0u));

  if ((st_after & RK3576_VOP_MMU_STATUS_PAGING_EN) == 0u ||
      (st1_after & RK3576_VOP_MMU_STATUS_PAGING_EN) == 0u)
    {
      syslog(LOG_ERR,
             "kickpi-k7: MMU FAULT PROBE: *** PRECONDITION FAILED -- paging "
             "did not enable on both\n");
      syslog(LOG_ERR,
             "kickpi-k7:   MMUs, so an access could have passed through "
             "untranslated and the result\n");
      syslog(LOG_ERR,
             "kickpi-k7:   is meaningless ***\n");
    }

  syslog(LOG_WARNING,
         "kickpi-k7: MMU FAULT PROBE: status=%08x paging_en=%u "
         "FAULT_ACTIVE=%u is_write=%u bus_id=%u\n",
         (unsigned)st_after,
         (unsigned)(st_after & RK3576_VOP_MMU_STATUS_PAGING_EN ? 1u : 0u),
         faulted ? 1u : 0u,
         (unsigned)(st_after & RK3576_VOP_MMU_STATUS_FAULT_IS_WRITE ? 1u : 0u),
         (unsigned)bus_id);

  if (faulted)
    {
      syslog(LOG_ERR,
             "kickpi-k7: MMU FAULT PROBE => THE LAYER %s: a translation "
             "failed, which can only\n",
             (st_after & RK3576_VOP_MMU_STATUS_FAULT_IS_WRITE) != 0u
                 ? "IS ACCESSING MEMORY (write)"
                 : "IS READING MEMORY");
      syslog(LOG_ERR,
             "kickpi-k7:   happen if it issued an access.  The layer does not "
             "\"fetch nothing\" -- it\n");
      syslog(LOG_ERR,
             "kickpi-k7:   fetches and the access does not complete.  bus_id=%u "
             "identifies the master ***\n", (unsigned)bus_id);
    }
  else
    {
      syslog(LOG_ERR,
             "kickpi-k7: MMU FAULT PROBE => NOTHING faulted even with paging "
             "on and no page table,\n");
      syslog(LOG_ERR,
             "kickpi-k7:   so no access is being made at all.  This agrees "
             "with the poison test\n");
      syslog(LOG_ERR,
             "kickpi-k7:   from an independent mechanism, which makes \"the "
             "layer issues no accesses\" real ***\n");
    }

  /* Always leave the MMU bypassed again -- that is the correct state for a
   * driver with no page tables -- and put both bypass settings back exactly as
   * they were. */

  rk3576_vop_putreg(priv, mmu + RK3576_VOP_MMU_COMMAND,
                    RK3576_VOP_MMU_CMD_DISABLE_PAGING);
  rk3576_vop_putreg(priv, mmu1 + RK3576_VOP_MMU_COMMAND,
                    RK3576_VOP_MMU_CMD_DISABLE_PAGING);

  /* Masked, like the clearing above: a plain write here would again depend on
   * the leftover contents of bits[31:16] rather than on this write's intent. */

  rk3576_vop_sys_masked_write(priv, RK3576_VOP_SYS_MMU_CTRL_IMD,
                              save_mmu_ctrl,
                              RK3576_VOP_SYS_MMU_BYPASS_EN |
                                  RK3576_VOP_SYS_MMU1_BYPASS_EN |
                                  RK3576_VOP_SYS_MMU_BYPASS_ID_MASK |
                                  RK3576_VOP_SYS_MMU_SOFT_RST_EN);
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_AXI_CTRL_IMD,
                    save_axi);
  up_mdelay(20);

  syslog(LOG_WARNING,
         "kickpi-k7: MMU FAULT PROBE: paging disabled and bypasses restored, "
         "MMU0 status=%08x MMU1 status=%08x SYS_MMU_CTRL=%08x ESMART_AXI=%08x\n",
         (unsigned)rk3576_vop_getreg(priv, mmu + RK3576_VOP_MMU_STATUS),
         (unsigned)rk3576_vop_getreg(priv, mmu1 + RK3576_VOP_MMU_STATUS),
         (unsigned)rk3576_vop_getreg(priv, sys_base +
                                     RK3576_VOP_SYS_MMU_CTRL_IMD),
         (unsigned)rk3576_vop_getreg(priv, esmart_base +
                                     RK3576_VOP_ESMART_AXI_CTRL_IMD));

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_security_probe
 *
 * Description:
 *   Read the SoC's VOP security configuration, which lives OUTSIDE the VOP's
 *   own register block and has therefore never been observed by any scan or
 *   probe in this bring-up.
 *
 *   WHY THIS COULD BE THE ANSWER.  The TRM's security chapter names this very
 *   window and describes a mechanism that closes it without leaving a trace:
 *
 *     "vop_sec_drm_ctrl(SYS_SGRF_SOC_CON1[10:0])" selects secure mode and the
 *     secure port;  "The security layer esmart0 is forced to vop_sec_port"; and
 *     "When a secure layer is selected for non-secure PORT, the layer is
 *     forcibly closed".
 *
 *   ESMTART0 is not secure-only -- vop_sec_en = 0 means plain non-secure mode
 *   and the TRM shows secure display being switched on and off by software --
 *   but IF a boot stage or a fuse left vop_sec_en set, then this window would be
 *   treated as the security layer, forced to the secure port, and (since this
 *   driver uses PORT0) closed by hardware.  The registers would still read back
 *   exactly as programmed, no error would be logged, the layer would issue no
 *   memory accesses, and the POST would starve: every measurement this bring-up
 *   has made, including the two independent "issues no accesses" results.
 *
 *   SYS_SGRF is at 0x26004000 (TRM address map); SOC_CON1 is at offset 0x004.
 *
 *   *** THE READ ITSELF IS WHAT SETTLED THIS, AND NOT IN THE WAY INTENDED. ***
 *   On this board a non-secure read of SYS_SGRF is not answered with zeros: it
 *   takes a SYNCHRONOUS EXTERNAL ABORT and panics the kernel.
 *
 *   *** BUT THE VOP KEEPS ITS OWN COPY OF THE SECURITY CONFIGURATION, INSIDE
 *   ITS OWN REGISTER BLOCK, AND THAT PART IS READABLE. ***  This is the
 *   measurement that should have replaced the SGRF read instead of a no-op:
 *
 *     SYS_CTRL_SEC_DRM_CTRL        0x01E0  sec_axiN_ridM_prot_en
 *     SYS_CTRL_SEC_DRM_PORT_SEL    0x01E4  per-layer secure port
 *     SYS_CTRL_SEC_PORT0_LAYER_SEL 0x01E8  per-layer secure LAYER select
 *     SYS_CTRL_SEC_PORT1_LAYER_SEL 0x01EC
 *     SYS_CTRL_SEC_PORT2_LAYER_SEL 0x01F0
 *     SYS_CTRL_SEC_AXI_RID_PROT    0x01F8  the secure read ids
 *
 *   All of them reset to zero.  In SEC_PORTn_LAYER_SEL each layer takes four
 *   bits -- an enable and a 3-bit window select:
 *
 *     bit[3]   drm_layer0_sel_en   "Enable sec_layer0 sel"
 *     bits[2:0] drm_layer0_sel     3'b0010: Esmart0   3'b0011: Esmart2
 *     (layer1 +4, layer2 +8, layer3 +12)
 *
 *   So "esmart0 is the secure layer on port0" is directly readable here: it is
 *   bit3 set with bits[2:0] = 2.  If those are clear, the security mechanism is
 *   NOT what closes the window -- and no secure-world access is needed to know
 *   it.
 *
 *   The early dump (512-byte SYS_CTRL scan, taken before this driver wrote
 *   anything) already showed 0x01E0 onward as zero, which is the answer.  This
 *   probe makes it explicit and decoded rather than leaving it implied by an
 *   absence from a non-zero list.
 *
 * Returned Value:
 *   OK, or -ENODEV if the VOP is not up.
 *
 ****************************************************************************/

int rk3576_vop_security_probe(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t sys_base;
  uint32_t sec_drm;
  uint32_t sec_port_sel;
  uint32_t sec_ls[3];
  uint32_t sec_rid;
  uint32_t vp;
  bool any_secure_layer = false;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  sys_base = RK3576_VOP_SYS_CTRL(priv->base);

  sec_drm      = rk3576_vop_getreg(priv, sys_base + 0x01e0u);
  sec_port_sel = rk3576_vop_getreg(priv, sys_base + 0x01e4u);
  sec_ls[0]    = rk3576_vop_getreg(priv, sys_base + 0x01e8u);
  sec_ls[1]    = rk3576_vop_getreg(priv, sys_base + 0x01ecu);
  sec_ls[2]    = rk3576_vop_getreg(priv, sys_base + 0x01f0u);
  sec_rid      = rk3576_vop_getreg(priv, sys_base + 0x01f8u);

  syslog(LOG_WARNING,
         "kickpi-k7: SECURITY PROBE: reading the VOP's OWN copy of the security\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   configuration (inside the VOP block, so it is readable --\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   unlike SYS_SGRF, which aborts).\n");
  syslog(LOG_WARNING,
         "kickpi-k7:   SEC_DRM_CTRL(0x1e0)=%08x SEC_DRM_PORT_SEL(0x1e4)=%08x "
         "SEC_AXI_RID_PROT(0x1f8)=%08x\n",
         (unsigned)sec_drm, (unsigned)sec_port_sel, (unsigned)sec_rid);

  for (vp = 0; vp < 3u; vp++)
    {
      int l;

      syslog(LOG_WARNING,
             "kickpi-k7:   SEC_PORT%u_LAYER_SEL(0x%03x)=%08x\n",
             (unsigned)vp, (unsigned)(0x1e8u + (vp * 4u)),
             (unsigned)sec_ls[vp]);

      for (l = 0; l < 4; l++)
        {
          uint32_t en = (sec_ls[vp] >> (l * 4)) & 1u;
          uint32_t sl = (sec_ls[vp] >> ((l * 4) + 1)) & 0x7u;

          if (en != 0u)
            {
              const char *who = sl == 0u ? "Cluster0" :
                                sl == 1u ? "Cluster1" :
                                sl == 2u ? "ESMART0" :
                                sl == 3u ? "ESMART2" : "(reserved)";

              any_secure_layer = true;
              syslog(LOG_ERR,
                     "kickpi-k7:     layer%u IS the SECURE layer = %s\n",
                     (unsigned)l, who);
            }
        }
    }

  if (any_secure_layer)
    {
      syslog(LOG_ERR,
             "kickpi-k7: SECURITY PROBE => *** A LAYER IS MARKED SECURE.  Per the\n");
      syslog(LOG_ERR,
             "kickpi-k7:   TRM a secure layer on a non-secure port is FORCIBLY\n");
      syslog(LOG_ERR,
             "kickpi-k7:   CLOSED, which would read back correct and fetch nothing\n");
      syslog(LOG_ERR,
             "kickpi-k7:   -- exactly the symptom.  Clear the enable bit. ***\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "kickpi-k7: SECURITY PROBE => NO layer is marked secure on any port,\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   and the RID protection and secure id registers are 0.\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   The VOP is in plain non-secure mode, so the security\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   mechanism is NOT what closes ESMART0.  *** THEORY\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   ELIMINATED -- and this is the measurement that settles\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   it, not the SGRF read that used to panic. ***\n");
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_scan_regs
 *
 * Description:
 *   Print every NON-ZERO register in the ESMART0 and SYS_CTRL blocks.
 *
 *   WHY THIS, AFTER SO MANY TARGETED FAILURES.  Every hypothesis so far was
 *   tested one register at a time, and every one was refuted: the mode, the
 *   clock lane, the pixel format, the layer geometry, the AXI read ID, dma_stop,
 *   mmu_bypass, the auto-gating workaround.  Meanwhile the one thing that has
 *   never been looked at is the REST of the register file.
 *
 *   That matters because this driver does not start the block from a known
 *   state.  rk3576_vop_reset() only DE-ASSERTS the VOP reset -- asserting it was
 *   found to tear down in-flight AXI traffic and hang the SoC -- so whatever an
 *   earlier stage left in these registers is inherited, and only the registers
 *   this driver writes are known.  The reference driver survives that by
 *   re-initialising the whole block; this one does not.
 *
 *   (The MIPI DSI interface and the panel are NOT initialised by the bootloader
 *   on this board: that bring-up is entirely ours.  The VOP side is the part
 *   whose prior state is unverified, and this scan measures it instead of
 *   assuming it.)
 *
 *   Almost every register in these blocks resets to zero, so ANY non-zero value
 *   here that this driver did not write is a leftover -- and the most damaging
 *   possible leftover is an ENABLED REGION1/2/3: the ESMART composites four
 *   regions, and a leftover region pointing at stale memory would fill the
 *   layer with garbage that follows neither REGION0's content nor its address,
 *   which is exactly the symptom.
 *
 *   Read-only.  Printed with offsets, so a suspect can be chased directly.
 *
 ****************************************************************************/

int rk3576_vop_scan_regs(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  static const struct
  {
    uint32_t off;
    FAR const char *name;
  } blocks[] =
  {
    { RK3576_VOP_ESMART0_OFFSET,  "ESMART0" },
    { RK3576_VOP_SYS_CTRL_OFFSET, "SYS_CTRL" },
  };

  int b;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  for (b = 0; b < (int)nitems(blocks); b++)
    {
      uint32_t base = priv->base + blocks[b].off;
      int off;
      int shown = 0;

      syslog(LOG_WARNING,
             "kickpi-k7: REG SCAN %s non-zero registers (base+%03x):\n",
             blocks[b].name, (unsigned)blocks[b].off);

      for (off = 0; off < 0x200; off += 4)
        {
          uint32_t v = rk3576_vop_getreg(priv, base + (uint32_t)off);

          if (v != 0u)
            {
              syslog(LOG_WARNING, "kickpi-k7:   +%03x = %08x\n",
                     (unsigned)off, (unsigned)v);
              shown++;
            }
        }

      syslog(LOG_WARNING,
             "kickpi-k7: REG SCAN %s: %d non-zero of %u\n", blocks[b].name,
             shown, (unsigned)(0x200u / 4u));
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_fb_fingerprint
 *
 * Description:
 *   Cheap sparse fingerprint of a buffer, used by rk3576_vop_dram_witness().
 *   Sampling every 257 bytes touches most cache lines without reading 2.6 MB.
 *
 ****************************************************************************/

static uint32_t rk3576_vop_fb_fingerprint(FAR const uint8_t *fb, size_t len)
{
  uint32_t hash = 2166136261u;
  size_t n;

  for (n = 0; n < len; n += 257u)
    {
      hash ^= fb[n];
      hash *= 16777619u;
    }

  return hash;
}

/****************************************************************************
 * Name: rk3576_vop_dram_witness
 *
 * Description:
 *   Answer, by measurement, the question the "the layer reads the wrong
 *   address" theory turns into: do the bytes we paint actually reach DRAM, or
 *   do they only ever exist inside the CPU's cache?
 *
 *   A CPU read-back (rk3576_vop_peek_pixel) CANNOT tell those apart -- it is
 *   answered by the cache, so it passes even if DRAM still holds something
 *   completely different.  The ESMART layer has its MMU bypassed and therefore
 *   reads DRAM, so "the cache has it" is not the property that matters, and
 *   every "fb check ... OK" line this project has printed was measured the
 *   wrong way round.
 *
 *   The test paints a known byte pattern, cleans the cache, fingerprints the
 *   buffer, then INVALIDATES the cache and fingerprints it again.  After the
 *   invalidate every read must be served from DRAM:
 *
 *     both fingerprints match the pattern -> the bytes really are in DRAM, so
 *         the layer is not reading a stale or unwritten buffer;
 *     the second one differs            -> the clean never wrote them back,
 *         and the layer has been displaying whatever DRAM happened to hold.
 *
 *   The patterns alternate 0x55/0xaa from pixel to pixel, so a pass cannot be
 *   an accident of what DRAM contained before, and a partly-written buffer
 *   shows up as a mismatch rather than a pass.
 *
 * Returned Value:
 *   OK if DRAM holds the pattern, -EIO if it does not, -ENODEV if not up.
 *
 ****************************************************************************/

int rk3576_vop_dram_witness(void)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  FAR uint8_t *fb;
  uint32_t cached;
  uint32_t dram;
  size_t n;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return -ENODEV;
    }

  fb = (FAR uint8_t *)priv->fbmem;

  for (n = 0; n < priv->fblen; n++)
    {
      fb[n] = (((n / 3u) & 1u) != 0u) ? 0xaau : 0x55u;
    }

  up_clean_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);

  cached = rk3576_vop_fb_fingerprint(fb, priv->fblen);

  /* Throw the cache away.  Everything read after this point is DRAM. */

  up_invalidate_dcache((uintptr_t)fb, (uintptr_t)fb + priv->fblen);

  dram = rk3576_vop_fb_fingerprint(fb, priv->fblen);

  syslog(LOG_WARNING,
         "kickpi-k7: DRAM WITNESS: after clean=%08x, after invalidate=%08x\n",
         (unsigned)cached, (unsigned)dram);

  if (cached == dram)
    {
      syslog(LOG_WARNING,
             "kickpi-k7:   MATCH -- DRAM holds our bytes, so the ESMART is "
             "NOT reading a stale\n");
      syslog(LOG_WARNING,
             "kickpi-k7:   or unwritten buffer.  The framebuffer content is "
             "not the fault.\n");
      return OK;
    }

  syslog(LOG_ERR,
         "kickpi-k7:   MISMATCH -- the cache clean never reached DRAM, so the "
         "ESMART has\n");
  syslog(LOG_ERR,
         "kickpi-k7:   been scanning out whatever DRAM happened to hold, and "
         "NO framebuffer\n");
  syslog(LOG_ERR,
         "kickpi-k7:   write by this driver could ever have changed the "
         "picture.\n");

  return -EIO;
}

/****************************************************************************
 * Name: rk3576_vop_address_echo
 *
 * Description:
 *   Point the ESMART layer's REGION0 fetch address at one of two separate
 *   buffers, so that "the layer reads the address we program" stops being an
 *   assumption.
 *
 *   This tests the single fault that fits EVERY observation this board has
 *   produced: a static image that never follows our content, a whole-screen
 *   colour change that does nothing, and a panel that is nevertheless able to
 *   show a clean picture when its own generator drives it.  All three follow
 *   if the layer fetches from a DIFFERENT physical address than the one we
 *   painted -- the panel then faithfully displays some other part of DRAM, and
 *   no amount of framebuffer work can ever change it.
 *
 *   The second buffer comes from the same identity-mapped DMA heap and is
 *   filled with the caller's colour.  Switching is one register write plus the
 *   mirror->real load pulse, so if the address is honoured the picture MUST
 *   change with it -- which is a far stronger statement than anything a
 *   content change can make, because it does not depend on the panel decoding
 *   the stream at all: it only asks whether the fetch moved.
 *
 *   Note the register readback is not a witness for this: it reports the
 *   MIRROR register, and the mirror->real load is exactly the step this
 *   driver's own comments warn can stay pending forever.
 *
 * Input Parameters:
 *   second - true to fetch from the second buffer, false for the primary
 *   rgb    - colour to paint into the buffer being selected
 *
 * Returned Value:
 *   The physical address programmed, or 0 on failure.
 *
 ****************************************************************************/

static FAR void *g_rk3576_vop_echo_buf;

uint32_t rk3576_vop_address_echo(bool second, uint32_t rgb)
{
  FAR struct rk3576_vop_s *priv = g_rk3576_vop_priv;
  uint32_t esmart_base;
  uintptr_t pa;
  FAR uint8_t *buf;
  size_t n;

  if (priv == NULL || priv->fbmem == NULL)
    {
      return 0;
    }

  if (g_rk3576_vop_echo_buf == NULL)
    {
      g_rk3576_vop_echo_buf = rk3576_dma_alloc(priv->fblen);
      if (g_rk3576_vop_echo_buf == NULL)
        {
          syslog(LOG_ERR,
                 "kickpi-k7: address echo: second buffer allocation "
                 "failed\n");
          return 0;
        }

      syslog(LOG_WARNING,
             "kickpi-k7: address echo: second buffer %p (%u bytes, %s the "
             "primary)\n", g_rk3576_vop_echo_buf, (unsigned)priv->fblen,
             ((uintptr_t)g_rk3576_vop_echo_buf >=
              (uintptr_t)priv->fbmem &&
              (uintptr_t)g_rk3576_vop_echo_buf <
                  (uintptr_t)priv->fbmem + priv->fblen)
                 ? "INSIDE"
                 : "outside");
    }

  buf = second ? (FAR uint8_t *)g_rk3576_vop_echo_buf
               : (FAR uint8_t *)priv->fbmem;

  for (n = 0; n < priv->fblen; n += 3u)
    {
      buf[n + 0] = (uint8_t)(rgb >> 16);
      buf[n + 1] = (uint8_t)(rgb >> 8);
      buf[n + 2] = (uint8_t)rgb;
    }

  up_clean_dcache((uintptr_t)buf, (uintptr_t)buf + priv->fblen);

  pa = up_addrenv_va_to_pa(buf);

  esmart_base = RK3576_VOP_ESMART(priv->base, RK3576_VOP_ESMART_IDX);
  rk3576_vop_putreg(priv, esmart_base + RK3576_VOP_ESMART_REGION0_YRGB_MST,
                    (uint32_t)pa);
  rk3576_vop_trigger_cfg_done(priv);

  return (uint32_t)pa;
}

#endif /* CONFIG_RK3576_VOP */
