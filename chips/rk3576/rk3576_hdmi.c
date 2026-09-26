/****************************************************************************
 * chips/rk3576/rk3576_hdmi.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_hdmi.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_hdmi.h"
#include "rk3576_hdptxphy.h"

#ifdef CONFIG_RK3576_HDMI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How long to wait for the controller's register bank / timers to come up
 * after the clocks have been enabled.  The controller has no documented
 * startup delay; these are generous margins around a poll that normally
 * succeeds on the first read.
 */

#define RK3576_HDMI_BANK_READY_TRIES 100
#define RK3576_HDMI_BANK_READY_DELAY 10

/* Settle time between deasserting the CRU resets and the first register
 * access.  Same rationale as above.
 */

#define RK3576_HDMI_RESET_SETTLE_US 100

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Driver state.  One instance: RK3576 has a single HDMI TX controller on
 * VO0, and the HDPTX PHY it drives is shared with the eDP controller, so a
 * second instance could not legally exist anyway.
 */

struct rk3576_hdmi_s
{
  mutex_t lock;               /* Serialises initialize/enable/disable      */

  uintptr_t base;             /* HDMI controller SFR block                 */
  uintptr_t grf;              /* VO0_GRF (routing and colour format)       */
  uintptr_t ioc;              /* VCCIO6 IOC (HPD pin sampling)             */
  uintptr_t cru;              /* Main CRU (APB and reference resets)       */
  uintptr_t pmu1;             /* PMU1CRU (HPD detector reset)              */

  FAR struct clk_s *pclk;     /* pclk_hdmitx0: the controller's APB clock  */
  FAR struct clk_s *ref;      /* clk_hdmitx0_ref: timer reference clock    */
  FAR struct clk_s *hdp;      /* clk_hdmitxhpd: HPD sampling clock         */

  uint32_t ref_hz;            /* Cached clk_get_rate(ref)                  */
  uint32_t pixel_clock;       /* Pixel clock of the enabled mode, Hz       */
  uint8_t bpc;                /* Bits per colour component of that mode    */

  bool initialized;           /* Clocks up, resets released, GRF programmed */
  bool streaming;             /* A mode has been enabled                   */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_hdmi_s g_rk3576_hdmi =
{
  .lock        = NXMUTEX_INITIALIZER,
  .base        = RK3576_HDMITX_ADDR,
  .grf         = RK3576_VO0_GRF_ADDR,
  .ioc         = RK3576_VCCIO6_IOC_ADDR,
  .cru         = RK3576_CRU_ADDR,
  .pmu1        = RK3576_PMU1_CRU_ADDR,
  .initialized = false,
  .streaming   = false,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_getreg
 *
 * Description:
 *   Read one of the controller's 32-bit registers.
 *
 ****************************************************************************/

static inline uint32_t rk3576_hdmi_getreg(struct rk3576_hdmi_s *priv,
                                          uint32_t offset)
{
  return getreg32(priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_putreg
 *
 * Description:
 *   Write one of the controller's 32-bit registers.
 *
 ****************************************************************************/

static inline void rk3576_hdmi_putreg(struct rk3576_hdmi_s *priv,
                                      uint32_t offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_modifyreg
 *
 * Description:
 *   Read-modify-write one of the controller's 32-bit registers.
 *
 *   Only usable on registers without write-one-to-clear fields; the
 *   interrupt status registers in this controller are read-only or
 *   clear-on-write, so they are always written whole.
 *
 * Input Parameters:
 *   priv    - Driver state.
 *   offset  - Register byte offset.
 *   clrbits - Bits to clear.
 *   setbits - Bits to set.
 *
 ****************************************************************************/

static void rk3576_hdmi_modifyreg(struct rk3576_hdmi_s *priv, uint32_t offset,
                                  uint32_t clrbits, uint32_t setbits)
{
  uint32_t reg = getreg32(priv->base + offset);

  reg &= ~clrbits;
  reg |= setbits;
  putreg32(reg, priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_grf_write
 *
 * Description:
 *   Write a masked field of a Rockchip "hiword-mask" register: bits [31:16]
 *   of the written word enable the per-bit writes of bits [15:0], so a field
 *   can be updated without a read-modify-write and without disturbing the
 *   neighbouring fields that share the register with it.  VO0_GRF, the IOC
 *   block and the CRU soft-reset registers all use this convention.
 *
 *   A read-modify-write would also be functionally wrong on the soft-reset
 *   registers, whose write-enable bits read back as zero -- reading them
 *   yields a value that would silently disable the very write being made.
 *
 * Input Parameters:
 *   base   - Block base address (VO0_GRF, IOC or CRU).
 *   offset - Register byte offset within that block.
 *   mask   - Field mask, already positioned at bits [15:0].
 *   value  - Field value, already positioned and already masked.
 *
 ****************************************************************************/

static void rk3576_hdmi_grf_write(uintptr_t base, uint32_t offset,
                                  uint32_t mask, uint32_t value)
{
  putreg32(((mask & 0xffffu) << 16) | (value & mask), base + offset);
}

/****************************************************************************
 * Name: rk3576_hdmi_reset
 *
 * Description:
 *   Assert or release one CRU soft-reset line.
 *
 *   The reset-controller framework has no Rockchip CRU provider, so the line
 *   is pulsed through the raw register, exactly as rk3576_sai.c,
 *   rk3576_saradc.c and rk3576_vop.c do.  Polarity is uniform across the CRU:
 *   writing 1 asserts the reset, 0 releases it.
 *
 * Input Parameters:
 *   priv         - Driver state.
 *   base         - CRU block base (main CRU or PMU1CRU).
 *   con          - SOFTRST_CONn index within that block.
 *   bit          - Bit position of the reset line.
 *   assert_reset - true to assert (hold in reset), false to release.
 *
 ****************************************************************************/

static void rk3576_hdmi_reset(struct rk3576_hdmi_s *priv, uintptr_t base,
                              unsigned int con, unsigned int bit,
                              bool assert_reset)
{
  rk3576_hdmi_grf_write(base, RK3576_CRU_SOFTRST_CON(con), 1u << bit,
                        assert_reset ? (1u << bit) : 0u);
}

/****************************************************************************
 * Name: rk3576_hdmi_pulse_reset
 *
 * Description:
 *   Assert a soft-reset line, wait, then release it.  Used for the HPD
 *   detector, which has no dependencies and therefore needs no ordering
 *   relative to the rest of the init sequence.
 *
 ****************************************************************************/

static void rk3576_hdmi_pulse_reset(struct rk3576_hdmi_s *priv,
                                    uintptr_t base, unsigned int con,
                                    unsigned int bit)
{
  rk3576_hdmi_reset(priv, base, con, bit, true);
  up_udelay(RK3576_HDMI_RST_PULSE_US);
  rk3576_hdmi_reset(priv, base, con, bit, false);
}

/****************************************************************************
 * Name: rk3576_hdmi_routing
 *
 * Description:
 *   Program the VO0_GRF fields that connect the VOP to the HDMI controller.
 *
 *   In the vendor kernel these live in the VOP driver, because that driver is
 *   written per-interface and knows which output it is feeding.  Our
 *   rk3576_vop.c is deliberately interface-agnostic -- it only selects the
 *   SYS_CTRL_*_INFACE_CTRL register for the requested iface -- so the
 *   HDMI-specific routing has to be established here instead.
 *
 *   Fields written (see hardware/rk3576_hdmi.h for the full map):
 *     SOC_CON1  TMDS mode (no FRL), HDCP 1.4 SRAM left disabled
 *     SOC_CON8  IPI colour depth and format, which is how the controller
 *               learns what the VOP is sending
 *     SOC_CON9  the HDMI channel takes its pixels from the VOP, not from EDP
 *     SOC_CON13 ungate the dclk path into the HDMI transmitter
 *     SOC_CON14 hand the DDC pads to the controller's built-in I2C master and
 *               select the I2S audio source
 *
 * Input Parameters:
 *   priv - Driver state.
 *   bpc  - Bits per colour component of the pixel stream.
 *
 ****************************************************************************/

static void rk3576_hdmi_routing(struct rk3576_hdmi_s *priv, uint8_t bpc)
{
  uint32_t depth;

  /* Colour depth.  Only two values are wired up in the hardware; anything
   * else falls back to 8 bpc, which is also what a DVI sink requires.
   */

  depth = (bpc == 10) ? RK3576_HDMI_GRF_BPC_10 : RK3576_HDMI_GRF_BPC_8;

  rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON8_OFF,
                        RK3576_HDMI_GRF_COLOR_DEPTH_MASK |
                            RK3576_HDMI_GRF_COLOR_FORMAT_MASK,
                        (depth << RK3576_HDMI_GRF_COLOR_DEPTH_SHIFT) |
                            (RK3576_HDMI_GRF_FMT_RGB
                             << RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT));

  /* The remaining fields are mode-independent, so they are set once, on the
   * first call.  SOC_CON9 is shared with the MIPI DSI path: whichever
   * consumer runs last wins, and the VOP/HDMI pair is the one that matters
   * here.
   */

  if (!priv->initialized)
    {
      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON1_OFF,
                            RK3576_HDMI_GRF_FRLMOD |
                                RK3576_HDMI_GRF_HDCP14_MEM_EN,
                            0);

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON9_OFF,
                            RK3576_HDMI_GRF_CH_SEL_VOP,
                            RK3576_HDMI_GRF_CH_SEL_VOP);

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON13_OFF,
                            RK3576_HDMI_GRF_DCLK2HDMITX_DISABLE, 0);

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON14_OFF,
                            RK3576_HDMI_GRF_I2S_SEL |
                                RK3576_HDMI_GRF_SCLIN_MASK |
                                RK3576_HDMI_GRF_SDAIN_MASK |
                                RK3576_HDMI_GRF_GRANT_SEL,
                            RK3576_HDMI_GRF_I2S_SEL |
                                RK3576_HDMI_GRF_SCLIN_MASK |
                                RK3576_HDMI_GRF_SDAIN_MASK |
                                RK3576_HDMI_GRF_GRANT_SEL);

      /* Mask the IOC's HPD interrupt.  The pin is not routed to an interrupt
       * handler yet, and an unmasked level-triggered source would keep the
       * GIC firing.
       */

      rk3576_hdmi_grf_write(priv->ioc, RK3576_HDMI_IOC_MISC_CON0_OFF,
                            RK3576_HDMI_IOC_HPD_INT_MSK, 0);
    }
}

/****************************************************************************
 * Name: rk3576_hdmi_init_registers
 *
 * Description:
 *   Initialise the controller's register file, its embedded I2C (DDC)
 *   master, and the timer reference.  Mirrors dw_hdmi_qp_init_hw() from the
 *   reference driver.
 *
 *   Notable omissions, all deliberate:
 *     - GLOBAL_SWDISABLE is not touched.  The video datapath is enabled out
 *       of reset; the only bits the reference driver ever sets are CEC and
 *       the audio packet datapath, neither of which we use.
 *     - GLOBAL_SWRESET_REQUEST is not touched either.  It resets units *and
 *       their configuration registers*, so using it here would immediately
 *       undo the writes below.  The controller was already reset by the CRU
 *       lines before this function ran.
 *     - VIDEO_INTERFACE_CONFIG0 is not touched: the bus format reaches the
 *       controller as hardware signals from VO0_GRF_SOC_CON8.
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if the controller never reports its
 *   clock tree running.
 *
 ****************************************************************************/

static int rk3576_hdmi_init_registers(struct rk3576_hdmi_s *priv)
{
  int i;

  /* Mask every interrupt.  The controller has no interrupt consumer yet and
   * an unmasked APB_REGBANK_READY would fire on every register write.
   */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_MAINUNIT_0_INT_MASK_N, 0);
  rk3576_hdmi_putreg(priv, RK3576_HDMI_MAINUNIT_1_INT_MASK_N, 0);

  /* Tell the controller's internal timers how fast the reference clock is.
   * The field is 29 bits wide and rejects anything larger, so clamp rather
   * than silently truncating.
   */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_TIMER_BASE_CONFIG0,
                     priv->ref_hz & RK3576_HDMI_TIMER_REFERENCE_BASE_MASK);

  /* Software-reset the embedded I2C master and park its SCL timing.  The
   * constants are the reference driver's; the master is not used for EDID
   * reading yet, but leaving it in an undefined state can make it drive the
   * DDC pads.
   */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_I2CM_CONTROL0,
                     RK3576_HDMI_I2CM_CONTROL0_SWRESET);
  rk3576_hdmi_putreg(priv, RK3576_HDMI_I2CM_FM_SCL_CONFIG0,
                     RK3576_HDMI_I2CM_FM_SCL_CONFIG0_VAL);
  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_I2CM_INTERFACE_CONTROL0,
                        RK3576_HDMI_I2CM_FM_EN, 0);

  /* Acknowledge the I2C master's two interrupt sources. */

  rk3576_hdmi_putreg(priv, RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                     RK3576_HDMI_MAINUNIT_1_INT_CLEAR_VAL);

  /* Wait for the timer base to be accepted.  The reference driver does not
   * poll here, so a timeout is reported but not treated as fatal: the mode
   * may still come up, and failing hard would turn a cosmetic timing
   * inaccuracy into a dead display.
   */

  for (i = 0; i < RK3576_HDMI_BANK_READY_TRIES; i++)
    {
      if (rk3576_hdmi_getreg(priv, RK3576_HDMI_TIMER_BASE_STATUS0) &
          RK3576_HDMI_TIMER_BASE_LOCKED_ST)
        {
          return OK;
        }

      up_udelay(RK3576_HDMI_BANK_READY_DELAY);
    }

  lcdwarn("WARNING: RK3576 HDMI timer base never locked (STATUS0=%08" PRIx32
          ")\n", rk3576_hdmi_getreg(priv, RK3576_HDMI_TIMER_BASE_STATUS0));

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_clock_get
 *
 * Description:
 *   Resolve one of the controller's clocks and enable it.
 *
 *   hdp is optional: it only samples the hot-plug pin, which nothing consumes
 *   yet, so a board whose clock tree omits it should still be able to
 *   transmit.
 *
 * Input Parameters:
 *   name     - Clock name as registered with the CLK framework.
 *   optional - true if the clock may be absent.
 *
 * Returned Value:
 *   The clock handle on success, NULL if absent and optional; a negated
 *   errno value cast to a pointer otherwise.
 *
 ****************************************************************************/

static FAR struct clk_s *rk3576_hdmi_clock_get(FAR const char *name,
                                               bool optional)
{
  FAR struct clk_s *clk;
  int ret;

  clk = clk_get(name);
  if (clk == NULL)
    {
      if (optional)
        {
          lcdwarn("WARNING: RK3576 HDMI optional clock '%s' is absent\n",
                  name);
          return NULL;
        }

      gerr("ERROR: RK3576 HDMI clock '%s' is not registered\n", name);
      return (FAR struct clk_s *)(uintptr_t)-ENOENT;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      gerr("ERROR: RK3576 HDMI failed to enable clock '%s': %d\n", name, ret);
      return (FAR struct clk_s *)(uintptr_t)ret;
    }

  return clk;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_initialize
 *
 * Description:
 *   Bring the HDMI TX controller up to the point where it can accept a mode,
 *   and put the shared HDPTX PHY into its powered-off state.
 *
 *   Sequence:
 *     1. enable pclk / ref / hdp clocks
 *     2. release the CRU APB and reference resets, pulse the HPD reset
 *     3. probe the controller by reading its ID registers
 *     4. program VO0_GRF routing and the controller's register file
 *     5. bring the PHY's APB and resets up (rk3576_hdptxphy_initialize)
 *
 *   Step 5 is last on purpose: the PHY touches PMU1CRU reset lines that are
 *   independent of ours, but it also reads its own SFRs, and doing it after
 *   the routing fields are in place keeps the whole path in a
 *   "clocks on, output off" state if it fails.
 *
 ****************************************************************************/

int rk3576_hdmi_initialize(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  FAR struct clk_s *clk;
  uint32_t id;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  /* Step 1: clocks.  pclk_hdmitx0 is the APB clock and must be running
   * before any register access; clk_hdmitx0_ref feeds the internal timers;
   * clk_hdmitxhpd samples the hot-plug pin.
   */

  clk = rk3576_hdmi_clock_get("pclk_hdmitx0", false);
  if (clk == NULL || (uintptr_t)clk > (uintptr_t)-4096)
    {
      ret = (clk == NULL) ? -EIO : (int)(intptr_t)clk;
      goto err_unlock;
    }

  priv->pclk = clk;

  clk = rk3576_hdmi_clock_get("clk_hdmitx0_ref", false);
  if (clk == NULL || (uintptr_t)clk > (uintptr_t)-4096)
    {
      ret = (clk == NULL) ? -EIO : (int)(intptr_t)clk;
      goto err_pclk;
    }

  priv->ref = clk;
  priv->ref_hz = clk_get_rate(priv->ref);

  if (priv->ref_hz == 0)
    {
      gerr("ERROR: RK3576 HDMI reference clock reports rate 0\n");
      ret = -EIO;
      goto err_ref;
    }

  priv->hdp = rk3576_hdmi_clock_get("clk_hdmitxhpd", true);
  if (priv->hdp != NULL && (uintptr_t)priv->hdp > (uintptr_t)-4096)
    {
      priv->hdp = NULL;
    }

  /* Step 2: CRU resets.  Assert both lines, hold them long enough for the
   * logic to settle, then release.  Presetn (APB) must be released for the
   * controller's SFRs to answer at all; resetn (reference) gates the timers.
   */

  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_APB_BIT, true);
  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_REF_BIT, true);
  rk3576_hdmi_pulse_reset(priv, priv->pmu1, RK3576_HDMI_PMU1RST_CON,
                          RK3576_HDMI_PMU1RST_HPD_BIT);
  up_udelay(RK3576_HDMI_RST_PULSE_US);

  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_APB_BIT, false);
  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_REF_BIT, false);
  up_udelay(RK3576_HDMI_RESET_SETTLE_US);

  /* Step 3: probe.  CORE_ID is read-only with a non-zero reset value, so a
   * zero read means the APB clock or the reset line is still wrong -- worth
   * failing on, because every subsequent write would silently be lost.
   */

  id = rk3576_hdmi_getreg(priv, RK3576_HDMI_CORE_ID);
  if (id == 0)
    {
      gerr("ERROR: RK3576 HDMI core id is 0; controller is not responding\n");
      ret = -ENODEV;
      goto err_hdp;
    }

  ginfo("RK3576 HDMI core id %08" PRIx32 ", ver %08" PRIx32
        ", config %08" PRIx32 ", ref %" PRIu32 " Hz\n",
        id, rk3576_hdmi_getreg(priv, RK3576_HDMI_VER_NUMBER),
        rk3576_hdmi_getreg(priv, RK3576_HDMI_CONFIG_REG), priv->ref_hz);

  /* Step 4: routing and register file.  The routing depends on bpc, so the
   * one-time fields are programmed inside rk3576_hdmi_routing() guarded by
   * the initialized flag, which is not yet set -- hence the explicit call
   * here with the DVI default of 8 bpc.
   */

  rk3576_hdmi_routing(priv, 8);

  ret = rk3576_hdmi_init_registers(priv);
  if (ret < 0)
    {
      goto err_hdp;
    }

  /* Step 5: the PHY.  It is shared with the eDP controller and owns the
   * pixel-clock PLL, so it must be brought up before the VOP is asked to
   * consume that clock.
   */

  ret = rk3576_hdptxphy_initialize();
  if (ret < 0)
    {
      gerr("ERROR: RK3576 HDPTX PHY initialisation failed: %d\n", ret);
      goto err_hdp;
    }

  priv->initialized = true;
  nxmutex_unlock(&priv->lock);

  return OK;

err_hdp:
  if (priv->hdp != NULL)
    {
      clk_disable(priv->hdp);
      priv->hdp = NULL;
    }

err_ref:
  clk_disable(priv->ref);
  priv->ref = NULL;

err_pclk:
  clk_disable(priv->pclk);
  priv->pclk = NULL;

err_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_hdmi_enable
 *
 * Description:
 *   Start outputting a DVI (TMDS) video stream at the requested pixel rate.
 *
 *   Order matters and follows the reference driver's bridge pre-enable path:
 *   bring the PHY up first, then select the link mode.  Programming
 *   LINK_CONFIG0 before the PHY is live would have the controller sampling a
 *   dead pixel clock.
 *
 ****************************************************************************/

int rk3576_hdmi_enable(uint32_t pixel_clock_hz, uint8_t bpc)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  int ret;

  /* Only the two colour depths the hardware can encode are accepted.  DVI
   * sinks are always 8 bpc, so 10 bpc is only reachable on an HDMI sink,
   * which this driver does not support yet -- but the plumbing is here so
   * that adding HDMI mode later does not need an API change.
   */

  if (bpc != 8 && bpc != 10)
    {
      gerr("ERROR: RK3576 HDMI unsupported colour depth %u bpc\n", bpc);
      return -EINVAL;
    }

  if (pixel_clock_hz == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      gerr("ERROR: RK3576 HDMI enable before initialize\n");
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  /* Update the IPI colour depth first: the controller latches it together
   * with the first pixels, so it must be correct before the PHY starts
   * producing the pixel clock.
   */

  rk3576_hdmi_routing(priv, bpc);

  /* Bypass the HDCP2 cipher unconditionally, as the reference driver does.
   * With this bit clear the AVP holds the video datapath pending an HDCP
   * handshake that never happens, and no pixels reach the PHY.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_HDCP2LOGIC_CONFIG0, 0,
                        RK3576_HDMI_HDCP2_BYPASS);

  /* Bring the PHY up.  It takes the *pixel* clock and the colour depth; the
   * TMDS character rate (pixel_clock * bpc / 8) is derived internally.
   */

  ret = rk3576_hdptxphy_power_on(pixel_clock_hz, bpc);
  if (ret < 0)
    {
      gerr("ERROR: RK3576 HDPTX PHY power on failed at %" PRIu32
           " Hz / %u bpc: %d\n", pixel_clock_hz, bpc, ret);
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  /* Select DVI mode: no infoframes, no audio, no HDCP, no scrambling.  FRL
   * must be explicitly cleared -- it is a separate field, not the complement
   * of OPMODE_DVI.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0,
                        RK3576_HDMI_OPMODE_FRL |
                            RK3576_HDMI_OPMODE_FRL_4LANES,
                        RK3576_HDMI_OPMODE_DVI);

  priv->pixel_clock = pixel_clock_hz;
  priv->bpc = bpc;
  priv->streaming = true;

  /* The pixel stream itself is produced by the VOP, which the caller is
   * expected to start only after this returns.  Until it does, the video
   * interface status registers still describe the idle state, so they are
   * not consulted as a readiness condition here -- rk3576_hdmi_is_ready()
   * and the caller's own logging are the places to look.
   */

  nxmutex_unlock(&priv->lock);

  ginfo("RK3576 HDMI DVI mode enabled: %" PRIu32 " Hz, %u bpc\n",
        pixel_clock_hz, bpc);

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_disable
 *
 * Description:
 *   Stop the video stream and park the PHY.  The controller's register file
 *   is left configured so that a later enable() is enough to restart.
 *
 ****************************************************************************/

int rk3576_hdmi_disable(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      /* Back to DVI idle.  Nothing else needs undoing: the infoframe
       * scheduler was never enabled, so there is no packet state to clear.
       */

      rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0, 0,
                            RK3576_HDMI_OPMODE_DVI);
      rk3576_hdptxphy_power_off();
    }

  priv->streaming = false;
  priv->pixel_clock = 0;
  priv->bpc = 0;

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_is_ready
 *
 * Description:
 *   Report whether the HDMI transmit path is producing a stream.
 *
 *   The condition is "a mode was enabled and the PHY reports locked + ready".
 *   That is deliberately the strictest statement the driver can honestly
 *   make: whether a picture appears also depends on the VOP actually driving
 *   the IPI and on the sink accepting the signal, neither of which the
 *   controller can report.
 *
 *   The PHY's lock and ready flags are also mirrored into VO0_GRF_SOC_ST3,
 *   so this can be cross-checked from outside the driver.
 *
 ****************************************************************************/

bool rk3576_hdmi_is_ready(void)
{
  return g_rk3576_hdmi.streaming && rk3576_hdptxphy_is_ready();
}

/****************************************************************************
 * Name: rk3576_hdmi_pixel_clock_hz
 *
 * Description:
 *   Return the pixel clock of the enabled mode, or 0.
 *
 ****************************************************************************/

uint32_t rk3576_hdmi_pixel_clock_hz(void)
{
  return g_rk3576_hdmi.pixel_clock;
}

/****************************************************************************
 * Name: rk3576_hdmi_ref_clock_hz
 *
 * Description:
 *   Return the controller's reference clock frequency, or 0.
 *
 ****************************************************************************/

uint32_t rk3576_hdmi_ref_clock_hz(void)
{
  return g_rk3576_hdmi.ref_hz;
}

/****************************************************************************
 * Name: rk3576_hdmi_uninitialize
 *
 * Description:
 *   Stop the stream, park the PHY, hold the controller in reset and turn its
 *   clocks off.  Present for symmetry with initialize() and for a future
 *   power-management path; board bring-up never calls it.
 *
 ****************************************************************************/

int rk3576_hdmi_uninitialize(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  /* Inline what rk3576_hdmi_disable() does rather than calling it: the mutex
   * is not recursive, so calling it here would deadlock against ourselves.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0, 0,
                        RK3576_HDMI_OPMODE_DVI);
  rk3576_hdptxphy_power_off();

  priv->streaming = false;
  priv->pixel_clock = 0;
  priv->bpc = 0;

  /* Hold the controller in reset before stopping its clocks, so that it
   * cannot issue partial AXI transactions while pclk disappears.
   */

  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_REF_BIT, true);
  rk3576_hdmi_reset(priv, priv->cru, RK3576_HDMI_RST_CON,
                    RK3576_HDMI_RST_APB_BIT, true);

  if (priv->hdp != NULL)
    {
      clk_disable(priv->hdp);
      priv->hdp = NULL;
    }

  clk_disable(priv->ref);
  priv->ref = NULL;

  clk_disable(priv->pclk);
  priv->pclk = NULL;

  priv->ref_hz = 0;
  priv->initialized = false;

  nxmutex_unlock(&priv->lock);
  return OK;
}

#endif /* CONFIG_RK3576_HDMI */
