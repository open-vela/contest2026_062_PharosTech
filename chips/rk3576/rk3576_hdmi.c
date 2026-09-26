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
  mutex_t lock; /* Serialises initialize/enable/disable      */

  uintptr_t base; /* HDMI controller SFR block                 */
  uintptr_t grf;  /* VO0_GRF (routing and colour format)       */
  uintptr_t ioc;  /* VCCIO6 IOC (HPD pin sampling)             */
  uintptr_t cru;  /* Main CRU (APB and reference resets)       */
  uintptr_t pmu1; /* PMU1CRU (HPD detector reset)              */

  FAR struct clk_s *pclk; /* pclk_hdmitx0: the controller's APB clock  */
  FAR struct clk_s *ref;  /* clk_hdmitx0_ref: timer reference clock    */
  FAR struct clk_s *hdp;  /* clk_hdmitxhpd: HPD sampling clock         */

  uint32_t ref_hz;      /* Cached clk_get_rate(ref)                  */
  uint32_t pixel_clock; /* Pixel clock of the enabled mode, Hz       */
  uint8_t bpc;          /* Bits per colour component of that mode    */

  bool initialized; /* Clocks up, resets released, GRF programmed */
  bool streaming;   /* A mode has been enabled                   */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_hdmi_s g_rk3576_hdmi = {
  .lock = NXMUTEX_INITIALIZER,
  .base = RK3576_HDMITX_ADDR,
  .grf = RK3576_VO0_GRF_ADDR,
  .ioc = RK3576_VCCIO6_IOC_ADDR,
  .cru = RK3576_CRU_ADDR,
  .pmu1 = RK3576_PMU1_CRU_ADDR,
  .initialized = false,
  .streaming = false,
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

/* Forward declaration: the CMU sampler is defined next to
 * rk3576_hdmi_report_status() further down, but rk3576_hdmi_initialize()
 * takes a baseline sample before the VOP exists and so needs it earlier.
 * Passing NULL for the output means the incomplete type is enough here.
 */

struct rk3576_hdmi_cmu_s;

static void rk3576_hdmi_sample_cmu(FAR struct rk3576_hdmi_s *priv,
                                   FAR const char *tag,
                                   FAR struct rk3576_hdmi_cmu_s *out);

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

static void rk3576_hdmi_pulse_reset(struct rk3576_hdmi_s *priv, uintptr_t base,
                                    unsigned int con, unsigned int bit)
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
 *               learns what the VOP is sending, plus the link-symbol clock
 *               gate forced ON (grf_vo0_hdmi_gate = 0)
 *     SOC_CON9  the HDMI channel takes its pixels from the VOP, not from EDP
 *     SOC_CON13 ungate the dclk path into the HDMI transmitter
 *     SOC_CON14 hand the DDC pads to the controller's built-in I2C master and
 *               select the I2S audio source
 *
 *   The GPIO4C sideband pads (HPD/DDC/CEC) are NOT muxed -- see the note below
 *   on why that is a separate concern from the video path.
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

  rk3576_hdmi_grf_write(
      priv->grf, RK3576_HDMI_GRF_SOC_CON8_OFF,
      RK3576_HDMI_GRF_COLOR_DEPTH_MASK | RK3576_HDMI_GRF_COLOR_FORMAT_MASK |
          RK3576_HDMI_GRF_HDMI_GATE,
      (depth << RK3576_HDMI_GRF_COLOR_DEPTH_SHIFT) |
          (RK3576_HDMI_GRF_FMT_RGB << RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT));

  /* The remaining fields are mode-independent, so they are set once, on the
   * first call.  SOC_CON9 is shared with the MIPI DSI path: whichever
   * consumer runs last wins, and the VOP/HDMI pair is the one that matters
   * here.
   */

  if (!priv->initialized)
    {
      /* TMDS, not FRL.  This is the reset value, so the write only serves to
       * undo the case where a bootloader left the controller in FRL mode.
       * The rest of SOC_CON1 is HDCP/EMP scratch that the TRM requires to be
       * 0 in normal operation and that nothing here enables, so it is left
       * alone -- mainline Linux's rk3576 glue does not write SOC_CON1 at all.
       */

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON1_OFF,
                            RK3576_HDMI_GRF_FRLMOD, 0);

      /* SOC_CON9[9] MUST BE 0 -- measured against the working firmware.
       *
       * This driver set it to 1 from its first version, on the strength of the
       * register description's "1'b1: Choose VOP path".  That is wrong.  A
       * register dump of the SAME BOARD running the vendor Debian image, which
       * does light the display, reads:
       *
       *     VO0_GRF SOC_CON9 (0x2601a024) = 0x00000000
       *
       * i.e. bit 9 clear, and so does the reset value.  Two independent
       * routes (regmap and direct MMIO) returned the same 0.
       *
       * The TRM's own application note (Part 2 12.6.3.4, "VOP_LITE -> HDMI
       * TX Controller") says:
       *
       *     Step1: GRF_VO0_CON13[4] = 1  // grf_sw_hdmi_1to4 with hdmi mode
       *     Step2: GRF_VO0_CON09[9] = 1  // hdmi controller data source
       *                                  //   comes from EBC
       *
       * so bit 9 = 1 selects the EBC/VOP_LITE source.  This board drives the
       * controller from the standard VOP2 path, so with bit 9 set the HDMI TX
       * is told to take its pixels from a source that is not running -- which
       * is exactly a permanently zero ipi_clk, and exactly what we measure.
       * The application note and the bit-field description contradict each
       * other; the working firmware settles it.
       */

      rk3576_hdmi_grf_write(
          priv->grf, RK3576_HDMI_GRF_SOC_CON9_OFF,
          RK3576_HDMI_GRF_CH_SEL_VOP | RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_MASK,
          0u | (depth == RK3576_HDMI_GRF_BPC_10
                    ? RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_10BPC
                    : RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_8BPC));

      /* Both halves of the interface's colour-depth agreement, read back so a
       * mismatch cannot hide: SOC_CON8 carries what the IPI sends, SOC_CON9
       * [7:4] carries what the far side is told to expect.  The TRM requires
       * them to agree ("align with HDMITX ipi color depth").
       *
       * The encoding is 0 for 8 bpc and 6 for 10 bpc, matching the working
       * vendor firmware (SOC_CON8 depth reads 0 there) and mainline Linux
       * (RK3576_8BPC = 0x0, RK3576_10BPC = 0x6).  See the note on
       * RK3576_HDMI_GRF_BPC_8 in hardware/rk3576_hdmi.h for why an earlier
       * version's 0x5 was wrong -- in short, the MIPI path's 0x5 lives in a
       * different field for a different consumer and proves nothing here.
       */

      {
        uint32_t con8 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_CON8_OFF);
        uint32_t con9 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_CON9_OFF);

        _err("RK3576 HDMI colour-depth agreement: SOC_CON8 %08" PRIx32
             " iipi_depth=%" PRIu32 " iipi_fmt=%" PRIu32
             "; SOC_CON9 %08" PRIx32 " hdmi_ch_sel=%" PRIu32
             " hdcp_tmds1_depth=%" PRIu32
             " (depths must match; hdmi_ch_sel must be 0 for the VOP2 path --"
             " the working Debian firmware reads SOC_CON9 = 0x00000000)\n",
             con8, (con8 >> RK3576_HDMI_GRF_COLOR_DEPTH_SHIFT) & 0xfu,
             (con8 >> RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT) & 0xfu, con9,
             (con9 >> 9) & 1u, (con9 >> 4) & 0xfu);
      }

      /* SOC_CON13[4] MUST BE 0 as well -- same measurement, same reasoning.
       *
       * This bit was added believing the TRM application note's
       * "Step1: GRF_VO0_CON13[4] = 1".  The working firmware reads
       *
       *     VO0_GRF SOC_CON13 (0x2601a034) = 0x00000000
       *
       * so bit 4 is clear there too.  bit 4 enables the 1to4 module, which the
       * application note introduces together with CON9[9] as the pair that
       * routes VOP_LITE/EBC into the HDMI TX.  With CON9[9] correctly cleared
       * the 1to4 module is not on the path at all, so enabling it can only
       * misroute the interface.  Only bit 8 (grf_ebc_dclk2hdmitx_disable) is
       * driven, and driven to 0, which is its reset value and what the working
       * firmware shows.
       */

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON13_OFF,
                            RK3576_HDMI_GRF_DCLK2HDMITX_DISABLE |
                                RK3576_HDMI_GRF_HDMI_1TO4_EN,
                            0u);

      {
        uint32_t c13 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_CON13_OFF);

        _err("RK3576 HDMI SOC_CON13 %08" PRIx32 " (hdmi_1to4_en(4)=%" PRIu32
             " ebc_dclk2hdmitx_disable(8)=%" PRIu32 ")\n",
             c13, (c13 >> 4) & 1u, (c13 >> 8) & 1u);
      }

      rk3576_hdmi_grf_write(
          priv->grf, RK3576_HDMI_GRF_SOC_CON14_OFF,
          RK3576_HDMI_GRF_I2S_SEL | RK3576_HDMI_GRF_SCLIN_MASK |
              RK3576_HDMI_GRF_SDAIN_MASK | RK3576_HDMI_GRF_GRANT_SEL,
          RK3576_HDMI_GRF_I2S_SEL | RK3576_HDMI_GRF_SCLIN_MASK |
              RK3576_HDMI_GRF_SDAIN_MASK | RK3576_HDMI_GRF_GRANT_SEL);

      /* Mask the IOC's HPD interrupt.  The pin is not routed to an interrupt
       * handler yet, and an unmasked level-triggered source would keep the
       * GIC firing.
       *
       * Mind the polarity: the TRM defines MISC_CON0[2] as
       * "hdmitx_hpd_int_mask
       * -- 1'b0: Interrupt enable, 1'b1: Interrupt disable", i.e. inverted
       * with respect to its own name.  The vendor writes 0 there and relies on
       * its hardirq to set it again later, which only works because it has a
       * handler.  With no handler the mask is written as 1, the vendor's end
       * state.  It gates the IRQ output only -- the HPD status bits read in
       * rk3576_hdmi_report_status() are unaffected by it.
       */

      rk3576_hdmi_grf_write(priv->ioc, RK3576_HDMI_IOC_MISC_CON0_OFF,
                            RK3576_HDMI_IOC_HPD_INT_MSK,
                            RK3576_HDMI_IOC_HPD_INT_MSK);

      /* HPD debounce, verbatim from the vendor's setup_hpd().  Without it the
       * internal HPD level (status bit 3) is the unfiltered pad and a single
       * glitch is enough to look like a hot-plug.  It is written for fidelity
       * with the vendor, not because it can light a display.
       */

      putreg32(RK3576_HDMI_IOC_MISC_CON1_DEBOUNCE,
               priv->ioc + RK3576_HDMI_IOC_MISC_CON1_OFF);

      /* Mux the GPIO4C pads to their HDMI functions.
       *
       * The distinction that matters here: the TMDS clock and data lanes are
       * DEDICATED pins and pass through no IOMUX -- that is why a clean
       * 148.5 MHz waveform appears at the connector no matter what this
       * register says.  But the four SIDEBAND signals are on GPIO4C, which
       * resets to 0 = GPIO, so HPD / DDC / CEC are disconnected from the
       * controller until this write is made.
       *
       * MEASURED, and the reason this is here: with the write absent,
       * VCCIO6_IOC_HDMITX_HPD_STATUS reads 0x00000080 -- bit 7 only, with
       * bit 3 clear.  The vendor driver's rk3576 read_hpd() is
       *
       *     return val & RK3576_HDMI_LEVEL_INT ? connected : disconnected;
       *     #define RK3576_HDMI_LEVEL_INT  BIT(3)
       *
       * so 0x80 decodes as "no sink attached" even with a monitor plugged in
       * and terminating the link.  The HPD input is simply not connected to
       * its pad.  Restoring this write makes that reading meaningful.
       *
       * Linux does the same thing through pinctrl ("hdmi_txm0_pins",
       * "hdmi_tx_scl", "hdmi_tx_sda"); there is no pinctrl framework here.
       */

      rk3576_hdmi_grf_write(priv->ioc, RK3576_HDMI_IOC_GPIO4C_IOMUX_SEL_L_OFF,
                            RK3576_HDMI_IOC_GPIO4C_SEL_MASK,
                            RK3576_HDMI_IOC_GPIO4C_HDMI_SEL);

      /* Read the two IOC registers straight back, and print the reference
       * values captured from the same board running the vendor Debian image
       * (which does light the display).  Both IOC reads matched exactly in
       * that dump, so these two lines are a quick way to notice a future
       * regression rather than a suspected fault.
       *
       * The interesting one is HPD_STATUS (VCCIO6_IOC + 0x0440), read in
       * report_status(): the working firmware shows 0x000000e9 there,
       * i.e. raw pad bit 5 = 1 and internal level bit 3 = 1, so the monitor
       * really is asserting hot-plug when it is connected and powered.
       */

      {
        uint32_t iomux =
            getreg32(priv->ioc + RK3576_HDMI_IOC_GPIO4C_IOMUX_SEL_L_OFF);
        uint32_t misc0 = getreg32(priv->ioc + RK3576_HDMI_IOC_MISC_CON0_OFF);

        _err("RK3576 HDMI IOC readback: GPIO4C_IOMUX_SEL_L %08" PRIx32
             " (working firmware: 00009999), MISC_CON0 %08" PRIx32
             " (working firmware: 00000003)\n",
             iomux, misc0);
      }
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

  /* Wait for the timer base to be accepted, then read the CMU back.
   *
   * TIMER_BASE_STATUS0[0] is the documented "the value was latched" flag and
   * is the cheapest proof that the reference clock is actually toggling -- the
   * field can only be accepted if irefclk is running, because the controller's
   * timers count it.
   *
   * This matters because the controller's register file is NOT uniformly
   * clocked.  Everything below 0x0400 (reset manager, CMU, I2CM, SCDC) sits in
   * the always-on APB domain and answers even with no reference clock at all,
   * while the blocks above it are clocked by the CMU's vidqp/linkqp domains,
   * which are generated FROM the reference clock.  With no reference clock
   * those domains are dead and an access is not decoded as a register access:
   * the slave returns an error and the CPU takes a synchronous external abort
   * rather than reading garbage.
   *
   * MEASURED, and the reason this check is here rather than a comment: the
   * very first access above 0x0400 -- HDCP2LOGIC_CONFIG0, 0x08e0, from
   * rk3576_hdmi_enable() -- aborted exactly this way, while every access below
   * 0x0400 in the same controller had already succeeded.
   *
   * NOTE: that abort has since turned out NOT to be a general property of this
   * region.  With the HDCP2 access removed the same binary reads and writes
   * 0x0968 (LINK_CONFIG0) and 0x3014/0x3024/0x3028 (main-unit interrupts)
   * without complaint -- so the region is live, and 0x08e0 fails for a reason
   * specific to the HDCP block.  See the long note on the HDCP2 access in
   * rk3576_hdmi_enable().  This probe is still worth keeping: it is cheap, it
   * is the documented precondition for the upper block, and it prints
   * CMU_STATUS for free.
   */

  for (i = 0; i < RK3576_HDMI_BANK_READY_TRIES; i++)
    {
      if (rk3576_hdmi_getreg(priv, RK3576_HDMI_TIMER_BASE_STATUS0) &
          RK3576_HDMI_TIMER_BASE_LOCKED_ST)
        {
          _info("RK3576 HDMI ref %" PRIu32
                " Hz, timer base locked, CMU_STATUS %08" PRIx32 "\n",
                priv->ref_hz,
                rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_STATUS));
          return OK;
        }

      up_udelay(RK3576_HDMI_BANK_READY_DELAY);
    }

  /* Deliberately NOT fatal.  Two reasons:
   *
   *   - The reference driver never polls this bit, so there is no evidence
   *     that every silicon revision implements it, and failing here would
   *     block a display that might otherwise work.
   *
   *   - This runs from rk3576_hdmi_initialize(), i.e. BEFORE the PHY is
   *     powered on.  If the vidqp/linkqp domains are fed from the PHY (which
   *     is what the reference driver's ordering implies -- see
   *     rk3576_hdmi_enable()), then a "not locked" result here is the
   *     expected state rather than a fault, and CMU_STATUS will look
   *     un-locked for the same reason.  Judge the reference clock by
   *     priv->ref_hz (a rate of 0 is the real failure) and re-check
   *     CMU_STATUS after the PHY is up.
   */

  _err("WARNING: RK3576 HDMI timer base never locked after %d tries "
       "(TIMER_BASE_STATUS0=%08" PRIx32 ", CMU_STATUS=%08" PRIx32
       ", ref %" PRIu32
       " Hz).  The controller's internal vidqp/linkqp clock domains are "
       "derived from the reference clock, so the register block above "
       "0x0400 may not answer and the next access to it can abort on the "
       "bus.\n",
       RK3576_HDMI_BANK_READY_TRIES,
       rk3576_hdmi_getreg(priv, RK3576_HDMI_TIMER_BASE_STATUS0),
       rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_STATUS), priv->ref_hz);

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
          _warn("WARNING: RK3576 HDMI optional clock '%s' is absent\n", name);
          return NULL;
        }

      _err("ERROR: RK3576 HDMI clock '%s' is not registered\n", name);
      return (FAR struct clk_s *)(uintptr_t)-ENOENT;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      _err("ERROR: RK3576 HDMI failed to enable clock '%s': %d\n", name, ret);
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

  /* Force the VO0 root mux onto clk_gpll before reading the reference rate.
   *
   * aclk_vo0_root_div is what feeds clk_hdmitx0_ref, and the reference clock
   * is what the controller's timers AND its internal CMU domains derive from
   * -- see rk3576_hdmi_init_registers() for why that decides whether the
   * 0x0800+ register block answers at all.
   *
   * The mux resets to 2'b00 = clk_gpll and the divider to /3 = 396 MHz, but a
   * bootloader is free to have moved either, and the other two inputs are
   * exactly the orphans the VOP driver already has to work around:
   * clk_lpll is reparented by the CPU-frequency helper, and clk_bpll is not
   * registered at all.  A mux left on either leaves the controller with a
   * reference rate that reads back fine and no clock on the wire -- the
   * failure mode this file now reports explicitly.
   *
   * clk_set_parent() is the right call here and cannot be done implicitly:
   * the mux is registered with CLK_MUX_SET_RATE_NO_REPARENT, so only an
   * explicit reparent can move it.
   */

  clk = clk_get("aclk_vo0_root_sel");
  if (clk != NULL)
    {
      FAR struct clk_s *gpll = clk_get("clk_gpll");

      if (gpll != NULL)
        {
          ret = clk_set_parent(clk, gpll);
          if (ret < 0)
            {
              _err("ERROR: RK3576 HDMI failed to put aclk_vo0_root on gpll: "
                   "%d\n",
                   ret);
            }
        }
    }

  /* The VO0 high-speed peripheral clock.
   *
   * Linux's device tree lists it among the HDMI node's required clocks (as
   * "hclk_vo1", matching the HCLK_VO0_ROOT dt-binding id), and the
   * controller's register file is NOT clocked by the APB gate alone: the
   * 0x0000-0x03ff block answers from pclk_hdmitx0, but the block above it --
   * video interface, HDCP, scrambler, packet scheduler and LINK_CONFIG0 --
   * needs this one as well.
   *
   * MEASURED: with only pclk_hdmitx0 and clk_hdmitx0_ref enabled, and after
   * every register below 0x0400 had been read and written successfully,
   * reading HDCP2LOGIC_CONFIG0 (0x08e0) aborted on the bus.
   *
   * That observation is what put this clock here, on the theory that the upper
   * block was unclocked.  The theory is now REFUTED: with the HDCP2 access
   * removed the driver reads and writes 0x0968 and 0x3014/0x3024/0x3028
   * happily, and 0x08e0 still aborts even now that this gate is enabled.  The
   * failure is specific to the HDCP block, not to the region.
   *
   * This clock is kept because the device tree lists it among the HDMI node's
   * required clocks, so enabling it is correct regardless -- but it is not,
   * and never was, the cure for the 0x08e0 abort.  The gate resets enabled, so
   * enabling it only matters if a vendor bootloader gated it.
   */

  (void)rk3576_hdmi_clock_get("hclk_vo0_root", true);

  /* The ARC clock.  Linux's DT names the same gate "earc" and the reference
   * driver enables every clock in the node's clock set; we enable ours
   * opportunistically because DVI mode has no audio return channel to carry.
   * It is intentionally left enabled for the life of the driver rather than
   * being tracked and torn down with the others: it is part of the
   * controller's clock set, and Linux keeps its bulk-enabled clocks on in the
   * same way.
   */

  (void)rk3576_hdmi_clock_get("clk_hdmitx0_arc", true);

  /* The audio clock, which the reference DT names "aud" and wires to
   * MCLK_SAI6_8CH.  It had been left off on the reasoning that DVI mode
   * carries no audio, and that reasoning is wrong for a structural reason:
   * the reset manager releases each functional group PER CLOCK DOMAIN, and
   * RESET_MANAGER_STATUS0 carries a dedicated
   * `AVP_DATAPATH_VIDEO_AUDCLK_STATUS` alongside the ipi/vidqp/linkqp/refclk
   * ones.  The VIDEO group therefore cannot come out of reset while audclk is
   * dead -- it is one of the five domains that group is gated on, audio or
   * not.
   *
   * MEASURED, which is what pointed here: RESET_MANAGER_STATUS0 reads
   * 0x00000000 (every group still ASSERTED) while CMU_STATUS reports the audio
   * domain as gated OFF and CMU_AUDQPCLK_FREQ measures 0 Hz.  Forcing the
   * manager's per-domain override to CLK_ON moved RESET_MANAGER_STATUS2 from
   * 0x0c to 0x1f but left STATUS0 at 0 -- consistent with the release having
   * to propagate into each domain on that domain's own clock, which cannot
   * happen if one of those clocks does not exist.
   *
   * The reference driver never faces this because it enables every clock in
   * its node's clock set, audclk included.
   */

  (void)rk3576_hdmi_clock_get("mclk_sai6", true);

  priv->ref_hz = clk_get_rate(priv->ref);

  if (priv->ref_hz == 0)
    {
      _err("ERROR: RK3576 HDMI reference clock reports rate 0\n");
      ret = -EIO;
      goto err_ref;
    }

  priv->hdp = rk3576_hdmi_clock_get("clk_hdmitxhpd", true);
  if (priv->hdp != NULL && (uintptr_t)priv->hdp > (uintptr_t)-4096)
    {
      priv->hdp = NULL;
    }

  /* Read-only check of the HDCP0 clock gates, CRU_GATE_CON63.
   *
   * MEASURED: this register already reads 0x00000000 out of reset, i.e. all
   * four gates (bit 14 pclk_hdcp0, 13 hclk_hdcp0, 12 aclk_hdcp0, 9
   * aclk_hdcp0_biu) are ALREADY OPEN -- Rockchip gates are "when high,
   * disable clock", so 0 means enabled.  Writing them open was tried and
   * changed nothing, which is expected when they were never closed.
   *
   * This matters because a tempting theory had to be retired:
   * VO0_GRF_SOC_CON13 carries a field called `grf_hdcp_ipiclk_disable` ("HDCP0
   * ipiclk gating"), which suggests the interface clock routes through the
   * HDCP0 block and would explain both the dead ipi_clk and the aborting
   * 0x08e0 register.  But that field also reads 0 (SOC_CON13 is 0x00000000),
   * so nothing is gated at HDCP0.  The theory is disproven; the readback is
   * kept because it is one more thing that does not need re-checking.
   */

  {
    uint32_t gate63 = getreg32(RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(63));

    _err("RK3576 HDMI HDCP0 clock gates: CRU_GATE_CON63 %08" PRIx32
         " (pclk_hdcp0(14)=%d hclk_hdcp0(13)=%d aclk_hdcp0(12)=%d "
         "aclk_hdcp0_biu(9)=%d, 0 = clock on)\n",
         gate63, (gate63 >> 14) & 1u, (gate63 >> 13) & 1u, (gate63 >> 12) & 1u,
         (gate63 >> 9) & 1u);
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
      _err("ERROR: RK3576 HDMI core id is 0; controller is not "
           "responding\n");
      ret = -ENODEV;
      goto err_hdp;
    }

  _info("RK3576 HDMI core id %08" PRIx32 ", ver %08" PRIx32
        ", config %08" PRIx32 ", ref %" PRIu32 " Hz\n",
        id, rk3576_hdmi_getreg(priv, RK3576_HDMI_VER_NUMBER),
        rk3576_hdmi_getreg(priv, RK3576_HDMI_CONFIG_REG), priv->ref_hz);

  /* Diagnostic (read-only): the controller's initialisation handshake.
   *
   * TRM Part 2 24.6.1.4: "Wait for the main interrupt line omain_int to be
   * triggered.  The apb_regbank_ready_irq is unmasked by default and notifies
   * the software that the reset operation is complete and APB interface is
   * online."  That is the controller's own statement that it finished coming
   * up, and MAINUNIT_0_INT_STATUS carries it as APB_REGBANK_READY [31] next to
   * TIMER_BASE_LOCKED [24].
   *
   * Reading this block is known-safe: both reference drivers read
   * MAINUNIT_1_INT_STATUS at runtime (dw-hdmi-qp.c:1227,
   * dw-hdmi-qp-vendor.c:4254).
   *
   * It is the right question to ask because everything else already checks
   * out and only the controller's internal state disagrees:
   *   - the VOP is scanning, dclk_vp0 is the PHY's pixel clock, its gate is
   *     open and SYS_CTRL_HDMI0_INFACE_CTRL reads back exactly 0x80000033;
   *   - the PHY is genuinely up: PLL_LOCK_DONE, PHY_CLK_RDY and PHY_RDY are
   *     all set, and post_enable_pll() polls PHY_CLK_RDY without erroring;
   *   - yet CMU_STATUS and RESET_MANAGER_STATUS0/STATUS2 all agree that
   *     ipi_clk and vidqpclk are dead and every AVP group is still ASSERTED.
   * If APB_REGBANK_READY is 0 the controller never completed its own reset,
   * and that -- not the clock routing -- is why the video group stays held.
   *
   * An earlier attempt pulsed MASTER_SWINIT_P (GLOBAL_SWRESET_REQUEST[0])
   * here and has been REVERTED: MEASURED, it changed the internal clock state
   * for the worse (CMU_AUDQPCLK_FREQ fell from ~12000 Hz to 0, LINKQP from
   * ~37125 to 24000) without releasing a single group -- STATUS0 stayed
   * 0x00000000.  It did prove the reset machinery runs and that BIT(0) is the
   * right field, but it is not the cure and it costs us the audio clock.
   */

  {
    uint32_t m0 = rk3576_hdmi_getreg(priv, RK3576_HDMI_MAINUNIT_0_INT_STATUS);
    uint32_t m1 = rk3576_hdmi_getreg(priv, RK3576_HDMI_MAINUNIT_1_INT_STATUS);

    _err("RK3576 HDMI init handshake: MAINUNIT_0_INT_STATUS %08" PRIx32
         " (APB_REGBANK_READY(31)=%" PRIu32 ", TIMER_BASE_LOCKED(24)=%" PRIu32
         "), MAINUNIT_1_INT_STATUS %08" PRIx32 "\n",
         m0, (m0 >> 31) & 1u, (m0 >> 24) & 1u, m1);

    _err("RK3576 HDMI clock-domain state at this point: "
         "RESET_MANAGER_STATUS0 %08" PRIx32 ", STATUS2 %08" PRIx32
         " (STATUS0 0 = every AVP group still ASSERTED)\n",
         getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_STATUS0),
         getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_STATUS2));
  }

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
      _err("ERROR: RK3576 HDPTX PHY initialisation failed: %d\n", ret);
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

/*
 * Inline the packet-scheduler and video-datapath programming here, in the
 * vendor's order, but with WRITE-ONLY accesses.
 *
 * MEASURED: reading 0x0aa8 (PKTSCHED_PKT_EN) aborts.  The faulting trail was
 *
 *   lr = 0x4023aa7c   (the instruction after `bl rk3576_hdmi_modifyreg`)
 *   x0 = 0x27da0aa8   x4 = 0x00000aa8   x5 = 0x27da0000
 *   esr = 0x96000010   (Data Abort, DFSC 0b010000 = synchronous external
 *                       abort, WnR = 0 -- it was a READ)
 *
 * while the `str` immediately before it, the plain write of 2 to 0x0aac,
 * went through.  So on this controller a WRITE to a block whose clock is
 * stopped is accepted and discarded, whereas a READ has to complete and takes
 * an external abort.  That asymmetry is why every earlier abort was also a
 * read (0x08e0, 0x0814) and why 0x0968 is fine -- the link clock domain is
 * running and the video one is not.
 *
 * Consequence: nothing in the 0x0800-0x0aff range may be read here, and a
 * successful write proves nothing about the block being alive.  Hence either
 * putreg32() or an explicit mask.
 */

/* One CMU sample: the raw status word plus the four per-domain frequency
 * read-backs, kept together so a caller can act on the values without
 * re-reading registers that may not answer any more.
 */

struct rk3576_hdmi_cmu_s
{
  uint32_t status;
  uint32_t ipi_hz;
  uint32_t vidqp_hz;
  uint32_t linkqp_hz;
  uint32_t aud_hz;
};

/****************************************************************************
 * Name: rk3576_hdmi_sample_cmu
 *
 * Description:
 *   Read and log the controller's clock monitor.
 *
 *   Every read here lives in the always-on 0x0000-0x03ff block -- the only
 *   part of the controller that answers regardless of its internal clock
 *   state.  That is deliberate: the 0x0800-0x0aff block aborts on read until
 *   the video clocks are up, and MEASURED, writing to it while it is gated
 *   leaves the next controller access aborting as well.
 *
 *   In the logged line a SET bit means that clock is gated OFF.  The
 *   "measured" figures are what the clock monitor sees on each internal
 *   clock, and that is the pair that separates "the VOP never delivered an
 *   interface clock" (out->ipi_hz == 0) from "the controller is holding its
 *   own video domain down" (ipi_hz live, vidqp_hz zero).
 *
 * Input Parameters:
 *   priv - Driver state.
 *   tag  - Short label distinguishing this sample in the log.
 *   out  - Optional; receives the sample.  May be NULL.
 *
 ****************************************************************************/

static void rk3576_hdmi_sample_cmu(FAR struct rk3576_hdmi_s *priv,
                                   FAR const char *tag,
                                   FAR struct rk3576_hdmi_cmu_s *out)
{
  struct rk3576_hdmi_cmu_s s;

  s.status = rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_STATUS);
  s.ipi_hz = rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_IPI_CLK_FREQ);
  s.vidqp_hz = rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_VIDQPCLK_FREQ);
  s.linkqp_hz = rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_LINKQPCLK_FREQ);
  s.aud_hz = rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_AUDQPCLK_FREQ);

  _err("RK3576 HDMI CMU[%s]: gated OFF IPI=%d VIDQP=%d LINKQP=%d AUD=%d "
       "eARC=%d (0x15 test %s) | measured IPI=%" PRIu32 " VIDQP=%" PRIu32
       " LINKQP=%" PRIu32 " AUD=%" PRIu32 "\n",
       tag, (s.status & RK3576_HDMI_CMU_IPI_CLK_OFF) != 0,
       (s.status & RK3576_HDMI_CMU_VIDQPCLK_OFF) != 0,
       (s.status & RK3576_HDMI_CMU_LINKQPCLK_OFF) != 0,
       (s.status & RK3576_HDMI_CMU_AUDCLK_OFF) != 0,
       (s.status & RK3576_HDMI_CMU_EARC_BPCLK_OFF) != 0,
       (s.status & RK3576_HDMI_CMU_CTRL_CLK_EN) == RK3576_HDMI_CMU_CTRL_CLK_EN
           ? "PASS"
           : "FAIL",
       s.ipi_hz, s.vidqp_hz, s.linkqp_hz, s.aud_hz);

  if (out != NULL)
    {
      *out = s;
    }
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
      _err("ERROR: RK3576 HDMI unsupported colour depth %u bpc\n", bpc);
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
      _err("ERROR: RK3576 HDMI enable before initialize\n");
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  /* Update the IPI colour depth first: the controller latches it together
   * with the first pixels, so it must be correct before the PHY starts
   * producing the pixel clock.
   */

  rk3576_hdmi_routing(priv, bpc);

  /* Bring the PHY up FIRST -- before touching anything above 0x0400 in the
   * controller.  The order is not stylistic; it is what the reference driver
   * does, in this exact sequence:
   *
   *     hdmi->phy.ops->init(hdmi, hdmi->phy.data);          // PHY first
   *     dw_hdmi_qp_mod(hdmi, HDCP2_BYPASS, ..., HDCP2LOGIC_CONFIG0);
   *     dw_hdmi_qp_mod(hdmi, op_mode, OPMODE_DVI, LINK_CONFIG0);
   *
   * Linux's atomic_pre_enable() powers the PHY before it programs either of
   * those two registers, and the reason is that the controller's upper
   * register block does not answer until the PHY is live (see the long note
   * in rk3576_hdmi_init_registers()).
   *
   * The order here used to be HDCP2 -> PHY -> LINK_CONFIG, and the HDCP2
   * access aborted.  0x08e0 is simply the lowest address above 0x0400, so it
   * was the first one reached while that block was still unclocked -- which
   * makes this a one-line ordering bug, not a fault in the HDCP2 register.
   *
   * power_on() takes the *pixel* clock and the colour depth; the TMDS
   * character rate (pixel_clock * bpc / 8) is derived internally.
   */

  ret = rk3576_hdptxphy_power_on(pixel_clock_hz, bpc);
  if (ret < 0)
    {
      _err("ERROR: RK3576 HDPTX PHY power on failed at %" PRIu32
           " Hz / %u bpc: %d\n",
           pixel_clock_hz, bpc, ret);
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  /* HDCP2_BYPASS is NOT written here, and the reason is timing rather than
   * permissions -- an earlier version of this comment got that wrong and
   * concluded the register was unreachable, so the correction is recorded.
   *
   * HDCP2LOGIC_CONFIG0 (0x08e0) sits in the 0x0800-0x0aff block, which is
   * clock-gated until the CMU's video clock domains come up, i.e. until the
   * VOP is actually delivering an interface clock.  At this point in the
   * sequence the PHY is live but the VOP is not scanning yet, so CLK_LOCKED is
   * still false and the block does not answer.  MEASURED, twice:
   *
   *   1. modifyreg (read-modify-write) before power_on(): abort on the read.
   *   2. A plain store after power_on(): the store was accepted, and the NEXT
   *      controller access then aborted -- the panic dumped x4 = 0x0968 and
   *      x5 = 0x27da0000, i.e. the LINK_CONFIG0 write immediately below,
   *      already computing its address.
   *
   * Attempt 2 is what made the register look permanently dead, because the two
   * PKTSCHED writes in that same region had just succeeded.  They succeeded
   * because report_status() issues them after clk_locked turns true; every
   * 0x08e0 attempt was made here, before it.  Same block, same access width,
   * opposite outcome -- the variable was never the register.
   *
   * So HDCP2_BYPASS is written from rk3576_hdmi_report_status(), in the
   * clk_locked branch, immediately ahead of the packet scheduler.  Order
   * relative to the reference driver is preserved there: bypass first, then
   * packets.
   */

  /* Select DVI mode: no infoframes, no audio, no HDCP, no scrambling.  FRL
   * must be explicitly cleared -- it is a separate field, not the complement
   * of OPMODE_DVI.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0,
                        RK3576_HDMI_OPMODE_FRL | RK3576_HDMI_OPMODE_FRL_4LANES,
                        RK3576_HDMI_OPMODE_DVI);

  /* Take the video datapath out of software disable.  0x0044 is in the
   * always-on block, so a read-modify-write is safe here.
   *
   * The packet scheduler is deliberately NOT programmed here.  It lives in
   * the 0x0800-0x0aff block, which is clock-gated until the VOP delivers an
   * interface clock -- and MEASURED, touching that block while it is gated
   * is what produces the synchronous external aborts: writing 0x0aac and
   * 0x0aa8 is accepted, but the very next controller access then aborts,
   * and reading 0x0aa8 aborts outright.  rk3576_hdmi_report_status()
   * programs the scheduler once CMU_STATUS proves the clocks are up.
   */

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_GLOBAL_SWDISABLE,
                        RK3576_HDMI_AVP_DATAPATH_VIDEO_SWDISABLE, 0);

  priv->pixel_clock = pixel_clock_hz;
  priv->bpc = bpc;
  priv->streaming = true;

  /* Pre-VOP baseline.  The VOP has not started yet, so the interface clock
   * is EXPECTED to read zero here.  What matters is the same line printed
   * from rk3576_hdmi_report_status() once the VOP is scanning: comparing the
   * two is what localises the fault to the VOP side or the controller side.
   */

  rk3576_hdmi_sample_cmu(priv, "pre-vop", NULL);

  /* CMU_STATUS is the only status register that may be read at this point --
   * and it turns out to be the one that matters.
   *
   * MEASURED: CMU_STATUS = 0x0000020a at this point, and the vendor driver's
   * readiness test (CMU_STATUS & 0x15) == 0x15 FAILS -- the VOP is not
   * scanning yet, so that is the expected reading rather than a fault.
   *
   * The upper block is clock-gated while clk_locked is false, and MEASURED,
   * touching it then accepts a write and breaks the NEXT controller access:
   * 0x0aac and 0x08e0 have both done it.
   *
   * 0x0968 (LINK_CONFIG0) and the whole 0x0000-0x03ff block do answer here,
   * which is why this function can set DVI mode at all -- their clocks are
   * already up.
   *
   * So anything in 0x0800-0x0aff waits for clk_locked, which only becomes true
   * once the VOP delivers the interface clock, and that is why HDCP2_BYPASS
   * and the packet scheduler are both programmed from
   * rk3576_hdmi_report_status() rather than from here.
   *
   * READS are a separate matter and are not done anywhere in this driver: the
   * reads of 0x08e0 and of 0x0814/0x0804 that were measured aborted in BOTH
   * clock states, so a read is suspect in a way a write is not.
   */

  _err("RK3576 HDMI DVI mode enabled: %" PRIu32
       " Hz, %u bpc, CMU_STATUS %08" PRIx32 " (%s), HPD %08" PRIx32 " (%s)\n",
       pixel_clock_hz, bpc, rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_STATUS),
       ((rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_STATUS) &
         RK3576_HDMI_CMU_CTRL_CLK_EN) == RK3576_HDMI_CMU_CTRL_CLK_EN)
           ? "clocks locked"
           : "CLOCKS NOT LOCKED",
       getreg32(priv->ioc + RK3576_HDMI_IOC_HDMI_HPD_STATUS_OFF),
       (getreg32(priv->ioc + RK3576_HDMI_IOC_HDMI_HPD_STATUS_OFF) &
        RK3576_HDMI_IOC_HPD_LEVEL)
           ? "sink attached"
           : "NO SINK (bit 3 clear)");

  nxmutex_unlock(&priv->lock);

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_report_status
 *
 * Description:
 *   Log the controller's clock state.
 *
 *   This function MUST NOT READ anything in the 0x0800-0x0aff block.  Two
 *   measured panics came from reading HDCP2LOGIC_CONFIG0 (0x08e0) and then
 *   VIDEO_INTERFACE_STATUS0 (0x0814), and the second one still aborted when
 *   called AFTER the VOP was up, so live display clocks do NOT make the block
 *   readable.  Writes behave differently: the PKTSCHED ones below succeed once
 *   clk_locked is true, and the HDCP2_BYPASS write immediately above them
 *   relies on exactly that.  Do not reintroduce those reads.
 *
 *   What is safe, and measured: CMU_STATUS (0x00b0), MAINUNIT_STATUS0
 *   (0x0180, same sub-0x0400 sub-block as the proven-working 0x00f4) and the
 *   IOC HPD register (0x0440).
 *
 *   The important value is CMU_STATUS, and there IS a documented expected
 *   value for it: the vendor driver's dw_hdmi_qp.c defines
 *
 *       #define HDMI_CTRL_CLK_EN 0x15
 *       ... (hdmi_readl(hdmi, CMU_STATUS) & HDMI_CTRL_CLK_EN)
 *                                                      == HDMI_CTRL_CLK_EN
 *
 *   as its "the controller's internal clocks are up" test, and mainline's
 *   header carries the same number as DISPLAY_CLK_LOCKED (0x15, alongside
 *   DISPLAY_CLK_MONITOR = 0x3f).  MEASURED here: CMU_STATUS = 0x0000020a,
 *   and 0x20a & 0x15 == 0 -- so the controller's clock tree is NOT locked.
 *   That is consistent with whole register blocks failing to respond, and it
 *   is the state to fix before anything else is worth chasing.
 *
 * Returned Value:
 *   Zero (OK) on success; -EIO if no stream is enabled.
 *
 ****************************************************************************/

int rk3576_hdmi_report_status(void)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  struct rk3576_hdmi_cmu_s clk;
  bool clk_locked;
  uint32_t mainunit;
  uint32_t hpd;

  if (!priv->streaming)
    {
      _err("ERROR: RK3576 HDMI status report before enable\n");
      return -EIO;
    }

  rk3576_hdmi_sample_cmu(priv, "final", &clk);

  /* SWDISABLE FIRST, in its own very short line, because it is the single
   * most diagnostic word in the controller and it kept being lost to log
   * truncation.
   *
   * The AVP functional unit owns the video datapath, and CMU reports its
   * domain (vidqpclk, "Video datapath clock") as dead while the link domain
   * runs.  GLOBAL_SWDISABLE has a bit that disables exactly that unit:
   *
   *   [6] AVP_DATAPATH_VIDEO_SWDISABLE  the video data path
   *   [4] AVP_SWDISABLE                 the WHOLE AVP unit, and the TRM adds
   *                                     "Also reset all contained
   *                                     configuration and status bits"
   *
   * If [4] is set, nothing in the video path can run and every symptom
   * follows: no pixels accepted, vidqpclk dead, ipi_clk never coming up,
   * while the link domain (which is not in the AVP) runs happily.
   *
   * Both must read 0.
   */

  {
    uint32_t swd = getreg32(priv->base + RK3576_HDMI_GLOBAL_SWDISABLE);

    _err("RK3576 HDMI SWDISABLE %08" PRIx32 " video=%" PRIu32 " avp=%" PRIu32
         " (both 0 = enabled)\n",
         swd, (swd >> 6) & 1u, (swd >> 4) & 1u);
  }

  clk_locked = (clk.status & RK3576_HDMI_CMU_CTRL_CLK_EN) ==
               RK3576_HDMI_CMU_CTRL_CLK_EN;

  mainunit = rk3576_hdmi_getreg(priv, RK3576_HDMI_MAINUNIT_STATUS0);
  hpd = getreg32(priv->ioc + RK3576_HDMI_IOC_HDMI_HPD_STATUS_OFF);

  /* Decode HPD, not just print it.
   *
   * The register is a bitmap, and the ONLY bit that means anything to the
   * driver is bit 3: the vendor's rk3576 read_hpd() is
   *
   *     return val & RK3576_HDMI_LEVEL_INT ? connected : disconnected;
   *     #define RK3576_HDMI_LEVEL_INT  BIT(3)
   *
   * MEASURED here: 0x00000080 -- bit 7 set, bit 3 CLEAR, i.e. "no sink" with
   * a monitor plugged in and terminating the link.  Printing the raw word hid
   * that, so it is spelled out.
   */

  _err("RK3576 HDMI status: MAINUNIT_STATUS0 %08" PRIx32 ", HPD %08" PRIx32
       ", phy_ready %d\n",
       mainunit, hpd, (int)rk3576_hdptxphy_is_ready());

  /* Turn the clock readings into a verdict.  This is the fork in the road
   * for the whole bring-up, and the raw numbers do not say which side of it
   * we are on: an interface clock that never arrives and a controller that
   * gates its own video domain look identical in the status word alone.
   */

  if (clk.ipi_hz == 0)
    {
      _err(
          "RK3576 HDMI DIAGNOSIS: the interface clock (IPI) measures zero, so "
          "the VOP is not delivering a video clock into this controller.\n"
          "      That is an UPSTREAM fault -- no register in the HDMI "
          "controller can fix it.  Check, on the VOP side: dclk_vp0 "
          "reparented to the PHY pixel clock, the POST dclk_core running, "
          "and VO0_GRF SOC_CON13[8] (grf_ebc_dclk2hdmitx_disable) cleared.\n");

      /* Rule the routing gate out here rather than asking for another boot:
       * it is a plain VO0_GRF read, and it is the one upstream condition this
       * driver controls itself.
       */

      {
        uint32_t con13 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_CON13_OFF);

        _err("      VO0_GRF SOC_CON13 readback %08" PRIx32
             ": grf_ebc_dclk2hdmitx_disable=%" PRIu32 " (must be 0)\n",
             con13, (con13 & RK3576_HDMI_GRF_DCLK2HDMITX_DISABLE) ? 1u : 0u);
      }

      /* The one place the SoC lets us SEE the HDMI TX's link layer working.
       *
       * VO0_GRF SOC_ST0 latches the HDMI TX -> PHY parallel bus, so it answers
       * the question the controller's own register file cannot (its video and
       * packet blocks are clock-gated, and every access to them aborts):  is
       * the link layer actually transmitting?
       *
       *   link2phy_status == 0 while the PHY is locked means the link layer is
       *   NOT driving the parallel bus -- i.e. no TMDS is being produced at
       *   all, which is exactly what a persistence-scope test with no waveform
       *   change looks like from the physical side.
       *
       * This register is also readable under the vendor firmware, so the same
       * read there gives a reference value for a board that does light up.
       */

      {
        uint32_t st0a;
        uint32_t st0b;
        uint32_t st0;
        uint32_t st3;

        /* The TRM states these inputs are not de-meta-stated, so only a value
         * that repeats is meaningful.  Two agreeing reads is the minimum this
         * driver can afford here; the third is only taken when the first two
         * disagree, which is the case that actually needs settling.
         */

        st0a = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST0_OFF);
        st0b = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST0_OFF);
        if (st0a == st0b)
          {
            st0 = st0a;
          }
        else
          {
            st0 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST0_OFF);
            _err("      VO0_GRF SOC_ST0 unstable across reads: %08" PRIx32
                 " %08" PRIx32 " %08" PRIx32 " (using the last)\n",
                 st0a, st0b, st0);
          }

        st3 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST3_OFF);

        _err("      VO0_GRF SOC_ST0 (grf_hdmitx_status0) %08" PRIx32
             " -> link2phy_status[23:16]=%02" PRIx32 " txffe0-3=%" PRIx32
             "/%" PRIx32 "/%" PRIx32 "/%" PRIx32 " ddc in(scl/sda)=%" PRIu32
             "/%" PRIu32 " ddc out(scl/sda)=%" PRIu32 "/%" PRIu32 "\n",
             st0,
             (st0 >> RK3576_HDMI_GRF_LINK2PHY_STATUS_SHIFT) &
                 RK3576_HDMI_GRF_LINK2PHY_STATUS_MASK,
             (st0 >> 0) & RK3576_HDMI_GRF_TXFFE0_MASK,
             (st0 >> RK3576_HDMI_GRF_TXFFE1_SHIFT) &
                 RK3576_HDMI_GRF_TXFFE0_MASK,
             (st0 >> RK3576_HDMI_GRF_TXFFE2_SHIFT) &
                 RK3576_HDMI_GRF_TXFFE0_MASK,
             (st0 >> RK3576_HDMI_GRF_TXFFE3_SHIFT) &
                 RK3576_HDMI_GRF_TXFFE0_MASK,
             (st0 & RK3576_HDMI_GRF_II2CM_SCL) ? 1u : 0u,
             (st0 & RK3576_HDMI_GRF_II2CM_SDA) ? 1u : 0u,
             (st0 & RK3576_HDMI_GRF_OI2CM_SCL) ? 1u : 0u,
             (st0 & RK3576_HDMI_GRF_OI2CM_SDA) ? 1u : 0u);

        _err("      VO0_GRF SOC_ST3 (hdptx flags) %08" PRIx32
             " -> pll_lock_done(3)=%" PRIu32 " phy_rdy(2)=%" PRIu32
             " phy_clk_rdy(1)=%" PRIu32 " hdptx_dtb(0)=%" PRIu32 "\n",
             st3, (st3 & RK3576_HDMI_GRF_ST3_PLL_LOCK_DONE) ? 1u : 0u,
             (st3 & RK3576_HDMI_GRF_ST3_PHY_RDY) ? 1u : 0u,
             (st3 & RK3576_HDMI_GRF_ST3_PHY_CLK_RDY) ? 1u : 0u,
             (st3 & RK3576_HDMI_GRF_ST3_DTB) ? 1u : 0u);

        if (((st0 >> RK3576_HDMI_GRF_LINK2PHY_STATUS_SHIFT) &
             RK3576_HDMI_GRF_LINK2PHY_STATUS_MASK) == 0)
          {
            _err("      => link2phy_status is ZERO while the PHY is locked:"
                 " the link layer is not driving the parallel bus, so no "
                 "TMDS is being generated at all.  The fault is upstream of "
                 "the PHY and inside the HDMI TX's video path -- it cannot "
                 "be the PHY configuration.\n");
          }
      }

      /* ...and which clock dclk_vp0 is actually sourced from, which is the
       * half of the question the VOP driver CANNOT answer.
       *
       * Every failure path in its reparenting code logs with gerr(), and gerr
       * is gated by CONFIG_DEBUG_GRAPHICS_ERROR -- which is off.  So if the
       * reparenting onto the PHY silently failed, nothing would ever say so,
       * and the VOP log would still print a healthy "dclk 148500000 Hz",
       * because that number is read from the fixed-rate placeholder node
       * rather than measured from the hardware.
       *
       * The two sources cannot be told apart by rate, and that is the whole
       * danger: the CRU chain hanging off GPLL divides to exactly 148.5 MHz
       * (1188/8) -- the same nominal figure as the PHY output, while being a
       * completely independent clock.  A pixel stream whose clock bears no
       * phase relationship to the TMDS bit clock gives the controller's
       * interface receiver nothing to lock to, and that is exactly the state
       * CMU reports: link domain up, interface and video domains gated, IPI
       * measuring zero.
       *
       *   CLKSEL_CON147[11] dclk_vp0_sel  0 = dclk_vp0_src, 1 = PHY pixel clk
       *   CLKSEL_CON145     dclk_vp0_src_sel [10:8], divider [7:0]
       *   GATE_CON61[10]    dclk_vp0_src_en, [13] dclk_vp0_en
       *                     (Rockchip gates: "When high, disable clock")
       */

      {
        uint32_t con147 =
            getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(147));
        uint32_t con145 =
            getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(145));
        uint32_t gate61 = getreg32(RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(61));
        uint32_t vp0_sel = (con147 >> 11) & 1u;

        _err("      CRU readback: CLKSEL_CON147 %08" PRIx32
             " -> dclk_vp0_sel=%" PRIu32 " = %s\n",
             con147, vp0_sel,
             vp0_sel ? "clk_hdmiphy_pixel0_o (the PHY) -- correct for HDMI"
                     : "dclk_vp0_src (CRU/GPLL chain) -- WRONG for HDMI");
        _err("      CRU readback: CLKSEL_CON145 %08" PRIx32
             " -> dclk_vp0_src_sel=%" PRIu32 " div=%" PRIu32
             "; GATE_CON61 %08" PRIx32 " -> dclk_vp0_src_en=%" PRIu32
             " dclk_vp0_en=%" PRIu32 " (gates: 0 = clock on)\n",
             con145, (con145 >> 8) & 0x7u, con145 & 0xffu, gate61,
             (gate61 >> 10) & 1u, (gate61 >> 13) & 1u);
      }

      /* Snapshot the always-on bootstrap block, 0x0000-0x03ff.  Every
       * register here answers regardless of the controller's internal clock
       * state, so these reads are safe even in this gated condition.
       *
       * This is worth doing because ON THIS BOARD THE BOOTLOADER DOES NOT
       * INITIALISE THE HDMI CONTROLLER AT ALL -- measured, not assumed.  Both
       * reference drivers (mainline dw-hdmi-qp.c and the Rockchip vendor
       * tree) were written for systems where U-Boot had already brought the
       * controller up, and NEITHER of them ever programs any of the registers
       * below.  That premise does not hold here, so whatever they leave to
       * the bootloader has to be inspected rather than presumed:
       *
       *   0x0044 GLOBAL_SWDISABLE    bit 6 = video datapath software disable.
       *                             A 1 here holds the video datapath in reset
       *                             and no amount of clocking will bring the
       *                             interface domain up.
       *   0x0048 RESET_MANAGER_CONFIG0 + 0x0050-0x0058 STATUS0/1/2
       *                             the controller's internal reset manager.
       *   0x00a0-0x00ac CMU_CONFIG0-3  the clock monitor's own configuration.
       */

      {
        uint32_t swdisable =
            getreg32(priv->base + RK3576_HDMI_GLOBAL_SWDISABLE);

        _err("      always-on: GLOBAL_SWDISABLE %08" PRIx32
             " (video-disable bit6 = %" PRIu32
             ", AVP master disable bit4 = %" PRIu32 ", both must be 0)\n",
             swdisable,
             (swdisable & RK3576_HDMI_AVP_DATAPATH_VIDEO_SWDISABLE) ? 1u : 0u,
             (swdisable & RK3576_HDMI_AVP_SWDISABLE) ? 1u : 0u);

        /* GLOBAL_SWRESET_REQUEST (0x0040) is deliberately NOT read.
         * MEASURED: reading it takes a synchronous external abort --
         * "Synchronous Abort handler, esr 0x96000210, far 0x27da0010", with
         * the faulting instruction being `ldr w3, [x3]` on base+0x40.  The
         * ESR says WnR = 0, so it is the READ that is illegal: this register
         * is WRITE-ONLY and self-clearing, exactly as this driver's own
         * header documents it.  Being inside the 0x0000-0x03ff region does
         * not make a register readable -- write-only registers live there
         * too.
         */
      }

      /* Prove, rather than assume, that the CRU soft-resets were actually
       * RELEASED.  The TRM documents CRU_SOFTRST_CON64 as "When high, reset
       * relative logic", so bit = 1 asserts and bit = 0 releases, and that is
       * what rk3576_hdmi_reset() writes -- but a polarity mistake here is
       * exactly the failure mode that once left the MIPI DCPHY completely
       * unresponsive on this board, and the symptom (peripheral dead while
       * status registers still look plausible) matches what we are chasing.
       *
       * The low 16 bits are readable; only the 31:16 write-enable field is
       * write-only, so this read is safe and settles the question outright.
       *   bit 9 resetn_hdmitx0_ref   bit 8 reserved(RO)   bit 7
       * presetn_hdmitx0 Both must read 0 after initialize() has run.
       */

      {
        uint32_t con64 =
            getreg32(RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(64));

        _err("      CRU readback: SOFTRST_CON64 %08" PRIx32
             " -> resetn_hdmitx0_ref(9)=%" PRIu32
             ", presetn_hdmitx0(7)=%" PRIu32 " (both must be 0 = released)\n",
             con64, (con64 >> 9) & 1u, (con64 >> 7) & 1u);
      }

      _err("      always-on: RESET_MANAGER_CONFIG0 %08" PRIx32
           ", STATUS0 %08" PRIx32 ", STATUS1 %08" PRIx32 ", STATUS2 %08" PRIx32
           "\n",
           getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_CONFIG0),
           getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_STATUS0),
           getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_STATUS1),
           getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_STATUS2));

      _err("      always-on: CMU_CONFIG0-3 %08" PRIx32 " %08" PRIx32
           " %08" PRIx32 " %08" PRIx32 " (never written by this driver)\n",
           getreg32(priv->base + RK3576_HDMI_CMU_CONFIG0),
           getreg32(priv->base + RK3576_HDMI_CMU_CONFIG1),
           getreg32(priv->base + RK3576_HDMI_CMU_CONFIG2),
           getreg32(priv->base + RK3576_HDMI_CMU_CONFIG3));

      /* Confirm the VOP actually kept the interface programming, rather than
       * having it reverted by its own cfg_done / mirror-register mechanism:
       * SYS_CTRL_HDMI0_INFACE_CTRL is a VOP register, not a controller one,
       * so it is readable from here.
       */

      _err("      VOP SYS_CTRL_HDMI0_INFACE_CTRL (0x27d00184) reads %08" PRIx32
           " (expect 0x80000033 = regdone_imd_en + vsync/hsync positive + "
           "clk_out_en + out_en + port_sel=0)\n",
           getreg32(RK3576_VOP_ADDR + 0x0184u));

      /* Two VOP values that decide whether the interface can produce a pixel
       * clock at all, and that this driver had never read back.
       *
       * POST0_CTRL_POST_CORE_CLK (VOP+0x0c0c):
       *   [0] dclk_core_sel   0 = dclk,    1 = dclk/2
       *   [2] dclk_out_sel    0 = dclk,    1 = dclk/2
       *
       *   dclk_core is the clock the POST timing generator AND the output
       *   interface both run on, and INFACE_CTRL[21] selects it ("Dclk
       *   core").  RK3576's VP0 is dual-pixel, so dclk_core must be dclk/2
       *   -- 74.25 MHz for a 148.5 MHz dclk -- and INFACE_CTRL[20] = div2
       *   then makes the pixel-interface clock 37.125 MHz, which is exactly
       *   the figure LINKQP measures.
       *
       *   A dropped write here would leave dclk_core = dclk, i.e. the
       *   interface running at the wrong RATE -- it would still run, so this
       *   cannot explain a zero ipi_clk.  The readback closes the gap rather
       *   than explaining the fault.
       *
       * POST0_CTRL_POST_CLK_CNT (VOP+0x0cf4) counts aclk in [31:16] and dclk
       * in [14:0] over one fixed 5000-hclk window once [15] is set.  Both are
       * real hardware counts, so they settle "is the VOP datapath clock
       * alive" with a measurement instead of an inference.  The VOP driver
       * prints its own copy of these; repeating them here keeps every
       * measurement that matters in one place.
       */

      {
        uint32_t core_clk = getreg32(RK3576_VOP_ADDR + 0x0c0cu);
        uint32_t clk_cnt = getreg32(RK3576_VOP_ADDR + 0x0cf4u);

        _err("      VOP POST0_CTRL_POST_CORE_CLK (0x27d00c0c) reads %08" PRIx32
             " -> dclk_core_sel(0)=%" PRIu32
             " (1 = dclk/2, required: VP0 is dual-pixel), "
             "dclk_out_sel(2)=" PRIu32 "\n",
             core_clk, core_clk & 1u, (core_clk >> 2) & 1u);

        _err("      VOP POST0_CTRL_POST_CLK_CNT (0x27d00cf4) reads %08" PRIx32
             " -> calc_clk_en(15)=%" PRIu32 " calc_aclk_cnt=%" PRIu32
             " calc_dclk_cnt=%" PRIu32
             " (both counts must be non-zero; counted over the same window)\n",
             clk_cnt, (clk_cnt >> 15) & 1u, (clk_cnt >> 16) & 0xffffu,
             clk_cnt & 0x7fffu);
      }

      /* CRU_SOFTRST_CON75 -- the PHY's link-symbol clock reset, the only
       * reset on the HDMI path that this driver does not write.
       */

      {
        uint32_t sr75 =
            getreg32(RK3576_CRU_ADDR +
                     RK3576_CRU_SOFTRST_CON(RK3576_HDMI_LINKSYM_RST_CON));

        _err("      CRU readback: SOFTRST_CON75 %08" PRIx32
             " -> resetn_linksym_hdmitxphy0(1)=%" PRIu32
             " (must be 0 = released; the PHY driver writes it)\n",
             sr75, (sr75 >> RK3576_HDMI_LINKSYM_RST_BIT) & 1u);
      }

      /* CRU_SOFTRST_CON63 -- the HDCP0 and VO0 resets.
       *
       * The clock half of HDCP0 was checked long ago and dismissed the whole
       * block (GATE_CON63 reads 0, so every HDCP0 clock gate is open).  The
       * reset half was never read.  Since the pixel-interface clock genuinely
       * passes through that block -- the block diagram puts HDCP2 Logic inline
       * with the video datapath, and VO0_GRF has a dedicated
       * `grf_hdcp_ipiclk_disable` gate for it -- an asserted HDCP0 reset stops
       * ipi_clk just as effectively as a closed gate, while leaving every
       * register this driver had been checking completely correct.
       *
       * Reset value is 0, so all seven must read 0.
       */

      {
        uint32_t sr63 = getreg32(
            RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_HDMI_VO0RST_CON));

        _err("      CRU readback: SOFTRST_CON63 %08" PRIx32
             " -> resetn_hdcp0(10)=%" PRIu32 " hresetn_hdcp0(9)=%" PRIu32
             " aresetn_hdcp0(8)=%" PRIu32 " aresetn_hdcp0_biu(5)=%" PRIu32
             " presetn_vo0_grf(6)=%" PRIu32 " presetn_vo0_biu(3)=%" PRIu32
             " hresetn_vo0_biu(1)=%" PRIu32 " (all must be 0 = released)\n",
             sr63, (sr63 >> RK3576_HDMI_VO0RST_HDCP0_BIT) & 1u,
             (sr63 >> RK3576_HDMI_VO0RST_HDCP0_H_BIT) & 1u,
             (sr63 >> RK3576_HDMI_VO0RST_HDCP0_A_BIT) & 1u,
             (sr63 >> RK3576_HDMI_VO0RST_HDCP0_BIU_BIT) & 1u,
             (sr63 >> RK3576_HDMI_VO0RST_VO0GRF_P_BIT) & 1u,
             (sr63 >> RK3576_HDMI_VO0RST_VO0BIU_P_BIT) & 1u,
             (sr63 >> RK3576_HDMI_VO0RST_VO0BIU_H_BIT) & 1u);
      }

      /* VOP SYS_CTRL_SYS_AUTO_GATING_CTRL_IMD -- the actual value in the
       * hardware, not the value the VOP driver believes it wrote.
       *
       * The VOP driver clears the master bit and aclk_pre_auto_gating_en and
       * leaves the per-block bits alone, on the assumption that the master bit
       * overrides them.  The reference driver treats aclk_pre_auto_gating_en
       * as an independent workaround rather than a master-bit consequence,
       * which is evidence that the per-block bits are NOT shadowed.  Those
       * bits reset to 1, and two of them -- port_dclk_gating_en and
       * hdmi_pix_clk_gating_en -- gate exactly the output-interface clock this
       * bring-up is missing.
       */

      {
        uint32_t ag =
            getreg32(RK3576_VOP_ADDR + RK3576_HDMI_VOP_AUTO_GATING_OFF);

        _err("      VOP SYS_AUTO_GATING_CTRL_IMD (0x27d00008) reads %08" PRIx32
             " -> master(31)=%" PRIu32 " hdmi_pix_clk(18)=%" PRIu32
             " port_dclk(14)=%" PRIu32 " prescan_aclk(13)=%" PRIu32
             " axi_aclk(15)=%" PRIu32 " aclk_pre(7)=%" PRIu32
             " win_aclk(6)=%" PRIu32
             " (1 = that block may self-gate; all must be 0)\n",
             ag, (ag & RK3576_HDMI_VOP_AG_MASTER) ? 1u : 0u,
             (ag & RK3576_HDMI_VOP_AG_HDMI_PIX_CLK) ? 1u : 0u,
             (ag & RK3576_HDMI_VOP_AG_PORT_DCLK) ? 1u : 0u,
             (ag & RK3576_HDMI_VOP_AG_PRESCAN_ACLK) ? 1u : 0u,
             (ag & RK3576_HDMI_VOP_AG_AXI_ACLK) ? 1u : 0u,
             (ag & RK3576_HDMI_VOP_AG_ACLK_PRE) ? 1u : 0u,
             (ag & RK3576_HDMI_VOP_AG_WIN_ACLK) ? 1u : 0u);
      }

      /* SETTLED: the working vendor firmware reads SOC_CON9 = 0x00000000, so
       * bit 9 belongs at 0 and routing() now clears it.  This block no longer
       * flips the bit -- it only confirms the programmed value, because a
       * sweep that ended by writing 1 would silently undo the fix above.
       *
       * If a future bring-up ever needs the experiment back, note the ordering
       * trap: the last write in a sweep wins, so a "restore" that writes the
       * old value is part of the experiment, not cleanup.
       */

      {
        uint32_t con9 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_CON9_OFF);

        _err("      SOC_CON9 readback %08" PRIx32 " -> hdmi_ch_sel(9)=%" PRIu32
             " (must be 0: 1 selects the EBC/VOP_LITE source, and the working"
             " Debian firmware has this register at 0x00000000)\n",
             con9, (con9 >> 9) & 1u);
      }

      /* BOUNDED EXPERIMENT: sweep the interface's two clock-select bits.
       *
       * Everything else is now verified by measurement, so the remaining
       * doubt is not "is something switched off" but "is INFACE_CTRL[21:20]
       * programmed correctly".
       *
       * Established: the VOP scans on a live 148.5 MHz dclk_vp0 (dsp_vcnt0
       * advances, POST_CLK_CNT ratio 1:4 against a 594 MHz aclk); dclk_vp0
       * really is selected from clk_hdmiphy_pixel0_o (CLKSEL_CON147 bit11 =
       * 1), so the PHY pixel clock reaches the VOP; and it reaches the
       * controller too, because LINKQP measures 37125 kHz == pixel_clock/4,
       * i.e. the link domain is derived from that clock and is running.
       * MAINUNIT_0_INT_STATUS confirms the interface side never came up: bit1
       * IPI_CLK_OFF_CHG_IRQ and bit0 IPI_CLK_LOCKED_CHG_IRQ are both clear,
       * so ipi_clk never changed state at all.
       *
       *   [21] hdmi_dclk_sel    0 = Dclk core, 1 = Dclk out
       *   [20] hdmi_pix_clk_sel 0 = Div 2,     1 = Div 4
       *
       * Their correct values come from the SoC data table entry
       * `vp[].pixel_rate`, which is not present in the reference material
       * available here, so what is programmed now (both 0) rests on an
       * unverified assumption.  Four combinations is a bounded search with a
       * quantitative readout after each -- a better use of a flash cycle
       * than another guess.
       */

      {
        uint32_t iface = RK3576_VOP_ADDR + 0x0184u;
        uint32_t saved = getreg32(iface);
        int sel;

        _err("      VOP interface sweep: INFACE_CTRL starts %08" PRIx32
             " (dclk_sel=%" PRIu32 " pix_clk_sel=%" PRIu32 ")\n",
             saved, (saved >> 21) & 1u, (saved >> 20) & 1u);

        for (sel = 0; sel < 4; sel++)
          {
            putreg32((saved & ~(0x3u << 20)) | ((uint32_t)sel << 20), iface);
            up_udelay(10000);

            _err("      VOP interface sweep: dclk_sel=%d pix_clk_sel=%d -> "
                 "IPI %" PRIu32 " kHz, VIDQP %" PRIu32 " kHz\n",
                 (sel >> 1) & 1, sel & 1,
                 rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_IPI_CLK_FREQ),
                 rk3576_hdmi_getreg(priv, RK3576_HDMI_CMU_VIDQPCLK_FREQ));
          }

        putreg32(saved, iface);
        _err("      VOP interface sweep: INFACE_CTRL restored to %08" PRIx32
             "\n",
             getreg32(iface));
      }
    }
  else if (!clk_locked)
    {
      _err("RK3576 HDMI DIAGNOSIS: IPI is live (%" PRIu32
           " Hz) so the VOP IS feeding this controller, but CMU_STATUS still "
           "reports its video domain as gated.\n"
           "      The fault is INSIDE the controller.  VIDQP measures %" PRIu32
           " Hz (%s).\n",
           clk.ipi_hz, clk.vidqp_hz, clk.vidqp_hz == 0 ? "gated" : "running");
    }
  else
    {
      _err("RK3576 HDMI DIAGNOSIS: all three display clock domains are up, "
           "so the controller is clocked and the fault is past it -- at the "
           "PHY's analog half or on the sink side.\n");
    }

  /* EXPERIMENT: release the video functional group through the reset
   * manager's override.
   *
   * MEASURED: RESET_MANAGER_CONFIG0 reads 0x00000000 and
   * RESET_MANAGER_STATUS0 reads 0x00000000.  Every field of STATUS0
   * documents "Value After Reset: 0x0" as (ASSERTED) -- "Init_n is
   * asserted", i.e. the AVP functional groups are HELD IN RESET -- and the
   * manager releases a group per clock domain, deciding from the live
   * cmu_status.<clk>_off_st bits whenever the override is disabled.
   *
   * Both of the domains that matter here report "off" (IPI and VIDQP), so
   * the manager has no reason to release the video group and never gets
   * one: the clock it waits for is the interface clock, and the reset it is
   * holding back is what gates that clock.  This driver walked into that
   * deadlock by releasing the controller resets early, in
   * rk3576_hdmi_initialize(), when no pixel clock existed yet; a
   * bootloader-initialised system releases them while the display pipeline
   * is already running.
   *
   * The override is the documented escape, so force every domain to CLK_ON
   * and then look at whether the groups come out of reset and whether the
   * interface clock appears.  Both readings are logged either way, so the
   * experiment informs the next step whether or not it fixes the problem.
   */

  if (!clk_locked)
    {
      putreg32(RK3576_HDMI_RST_MGR_ALL_CLK_ON,
               priv->base + RK3576_HDMI_RESET_MANAGER_CONFIG0);

      _err("RK3576 HDMI reset-manager override applied: RESET_MANAGER_CONFIG0 "
           "now %08" PRIx32 "\n",
           getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_CONFIG0));

      rk3576_hdmi_sample_cmu(priv, "post-rstmgr", &clk);

      clk_locked = (clk.status & RK3576_HDMI_CMU_CTRL_CLK_EN) ==
                   RK3576_HDMI_CMU_CTRL_CLK_EN;

      _err("RK3576 HDMI RESET_MANAGER_STATUS0 -> %08" PRIx32
           " (per-domain: 0 = group still ASSERTED, 1 = released), "
           "STATUS2 %08" PRIx32 "\n",
           getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_STATUS0),
           getreg32(priv->base + RK3576_HDMI_RESET_MANAGER_STATUS2));
    }

  /* EXPERIMENT: re-apply the mode programming now that the pixel clock is
   * actually running.
   *
   * This tests the one ordering violation left.  TRM Part 2 24.6.1 gives the
   * order as: enable all clocks -> configure the PHY -> reset the controller
   * -> configure link/packets/video.  This driver cannot follow that literally
   * because the pixel clock is produced by the PHY and only reaches the
   * controller once the VOP is scanning, so initialize() runs long before any
   * pixel clock exists -- the exact inverse of "clocks first".
   *
   * The controller now reports that it is fully initialised:
   *
   *   MAINUNIT_0_INT_STATUS = 0xa1000020 ->
   *     bit31 APB_REGBANK_READY_IRQ      "all ... units ... are ready"
   *     bit29 RESET_MANAGER_STATUS_END_IRQ "Reset Manager Init_n/Disable end"
   *     bit24 TIMER_BASE_LOCKED
   *
   * so its internal reset/init has completed and the link and audio clock
   * domains are running (CMU measures LINKQP ~27070 Hz and AUD ~12055 Hz).
   * Only the two domains that depend on the incoming video -- ipi_clk and
   * vidqpclk -- still measure zero.  If those latch when the mode is (re)set
   * while the clock is live, the ordering was the whole problem.
   *
   * Both writes are re-issued through the same helpers the first pass used:
   * SOC_CON8/SOC_CON9/SOC_CON13/SOC_CON14 in VO0_GRF and LINK_CONFIG0 in the
   * controller.  All are in always-on or already-proven blocks.
   *
   * NOTE: this supersedes an earlier false lead.  RESET_MANAGER_STATUS0
   * reading 0x00000000 was taken to mean every AVP group was held in reset,
   * which is why a reset-manager override and a MASTER_SWINIT_P pulse were
   * tried -- both did nothing, because RESET_MANAGER_STATUS_END_IRQ above
   * shows the reset manager had already finished.  0x00000000 is simply the
   * normal post-init value of that status word.
   */

  rk3576_hdmi_routing(priv, priv->bpc);

  rk3576_hdmi_modifyreg(priv, RK3576_HDMI_LINK_CONFIG0,
                        RK3576_HDMI_OPMODE_FRL | RK3576_HDMI_OPMODE_FRL_4LANES,
                        RK3576_HDMI_OPMODE_DVI);

  rk3576_hdmi_sample_cmu(priv, "post-reprogram", &clk);

  clk_locked = (clk.status & RK3576_HDMI_CMU_CTRL_CLK_EN) ==
               RK3576_HDMI_CMU_CTRL_CLK_EN;

  /* Everything in 0x0800-0x0aff is clock-gated until the CMU's video clock
   * domains are up, and MEASURED, programming it while gated is accepted by
   * the bus but breaks the NEXT controller access -- writing 0x0aac and 0x08e0
   * and reading 0x0aa8 have each done it.  So this whole block is programmed
   * here, and only once the clocks have provably come up.
   */

  if (clk_locked)
    {
      /* HDCP2_BYPASS first: of everything in this block it is the one write
       * that changes what leaves the connector rather than how it is framed,
       * and it is the current best explanation for the black picture.
       *
       * TRM Part 2, HDCP2LOGIC_CONFIG0 (0x08e0) bit0:
       *
       *   0x0 (BYPASS_OFF): The video datapath goes through the External
       *                     HDCP2 Module.        <-- THIS IS THE RESET VALUE
       *   0x1 (BYPASS_ON):  The video datapath bypasses the External HDCP2
       *                     Module.
       *
       * So out of reset the pixel stream is routed THROUGH a cipher block that
       * nothing on this board brings up.  Timing is generated before that
       * point, so the sink still locks, counts a full frame and shows black --
       * exactly the symptom.  Both reference drivers set this bit and U-Boot
       * sets it UNCONDITIONALLY, in its DVI branch as well as its HDMI branch,
       * ahead of LINK_CONFIG0 and the packet scheduler.  Debian corroborates
       * it from the other side: its CRU_GATE_CON63 reads 0x00007000, i.e.
       * pclk/hclk/aclk_hdcp0 GATED OFF, which only makes sense once the
       * datapath no longer passes through HDCP2.
       *
       * Plain store, never a read-modify-write: reads of this register have
       * taken a synchronous external abort twice on this board.  Writing the
       * whole word also clears the four override fields at bits [4:1], and 0
       * is what they must be -- an override ENABLE of 0 leaves that indication
       * "defined internally", which is the normal state.
       */

      putreg32(RK3576_HDMI_HDCP2_BYPASS,
               priv->base + RK3576_HDMI_HDCP2LOGIC_CONFIG0);

      _err("RK3576 HDMI HDCP2 bypass set (0x08e0 <- 0x%08" PRIx32
           "): the video datapath no longer routes through the external "
           "HDCP2 module\n",
           (uint32_t)RK3576_HDMI_HDCP2_BYPASS);

      putreg32(RK3576_HDMI_PKTSCHED_PKT_CONTROL0_START,
               priv->base + RK3576_HDMI_PKTSCHED_PKT_CONTROL0);
      putreg32(RK3576_HDMI_PKTSCHED_GCP_TX_EN,
               priv->base + RK3576_HDMI_PKTSCHED_PKT_EN);
      _err("RK3576 HDMI packet scheduler started\n");
    }
  else
    {
      _err("RK3576 HDMI HDCP2 bypass NOT set and packet scheduler NOT "
           "started: their register block is clock-gated, so the programming "
           "would be discarded and has been measured to break the next "
           "controller access.\n");
    }

  /* SUPERSEDED CONCLUSION, kept because the measurements are still true and
   * were expensive: an earlier version of this comment concluded from them
   * that the AVP register window is POWERED OFF and untouchable.  That is
   * wrong, and the two PKTSCHED writes directly above disprove it -- they land
   * in the same 0x0a00-0x0aff region and succeed whenever clk_locked is true.
   *
   * What the measurements actually show is a CLOCK GATE, and the difference
   * between success and failure is WHEN the access happens:
   *
   *   gated   -> write accepted, NEXT access aborts; read aborts directly
   *   clocked -> both work
   *
   *   read  0x0814 -> abort   (interface clocks dead, before the SOC_CON9 fix)
   *   read  0x0804 -> abort   (after the SOC_CON9 fix, in report_status)
   *   read  0x0aa8 -> abort
   *   write 0x0aac -> accepted, next access aborts
   *   write 0x08e0 -> accepted, next access aborts (both attempts, see the
   * note in rk3576_hdmi_enable())
   *
   * The last one is the important one, because at the time of that abort the
   * two PKTSCHED writes had ALREADY succeeded, which is what made the window
   * look untouchable.  The resolution is that those two writes happen after
   * clk_locked is true while the 0x08e0 attempts happened long before it --
   * so 0x08e0 is gated by the same condition and not by anything specific to
   * HDCP.  HDCP2_BYPASS is therefore written in the clk_locked branch above,
   * next to the packet scheduler, and not from initialize() or enable().
   *
   * A read of 0x0800-0x0aff is still not attempted anywhere in this driver:
   * the reads that aborted did so in BOTH states, so reads are suspect in a
   * way writes are not, and nothing here needs one.  Every diagnostic comes
   * from VO0_GRF / CRU / VOP, which are plain syscon blocks and always safe.
   */

  /* VO0_GRF's read-only status window, moved out of the "IPI is zero" branch
   * so it runs on a WORKING bring-up too.
   *
   * It was written when the only reachable state was the broken one, and the
   * moment the clock domains came up the branch stopped being taken -- losing
   * the single most useful measurement on the board.  link2phy_status is the
   * link layer's handshake with the PHY: on the working vendor firmware it is
   * non-zero and changes between reads, so a steady zero here means the link
   * layer is not driving the parallel bus even though the PHY is locked.
   */

  {
    uint32_t st0a = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST0_OFF);
    uint32_t st0b = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST0_OFF);
    uint32_t st0 = st0a;
    uint32_t st3 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST3_OFF);
    uint32_t link2phy;

    if (st0a != st0b)
      {
        st0 = getreg32(priv->grf + RK3576_HDMI_GRF_SOC_ST0_OFF);
        _err("  VO0_GRF SOC_ST0 unstable across reads: %08" PRIx32
             " %08" PRIx32 " %08" PRIx32 " (using the last)\n",
             st0a, st0b, st0);
      }

    link2phy = (st0 >> RK3576_HDMI_GRF_LINK2PHY_STATUS_SHIFT) &
               RK3576_HDMI_GRF_LINK2PHY_STATUS_MASK;

    _err("  VO0_GRF SOC_ST0 %08" PRIx32 " -> link2phy_status[23:16]=%02" PRIx32
         " txffe0-3=%" PRIx32 "/%" PRIx32 "/%" PRIx32 "/%" PRIx32
         " ddc in(scl/sda)=%" PRIu32 "/%" PRIu32 "\n",
         st0, link2phy, (st0 >> 0) & RK3576_HDMI_GRF_TXFFE0_MASK,
         (st0 >> RK3576_HDMI_GRF_TXFFE1_SHIFT) & RK3576_HDMI_GRF_TXFFE0_MASK,
         (st0 >> RK3576_HDMI_GRF_TXFFE2_SHIFT) & RK3576_HDMI_GRF_TXFFE0_MASK,
         (st0 >> RK3576_HDMI_GRF_TXFFE3_SHIFT) & RK3576_HDMI_GRF_TXFFE0_MASK,
         (st0 & RK3576_HDMI_GRF_II2CM_SCL) ? 1u : 0u,
         (st0 & RK3576_HDMI_GRF_II2CM_SDA) ? 1u : 0u);

    _err("  VO0_GRF SOC_ST3 %08" PRIx32 " -> pll_lock_done=%" PRIu32
         " phy_rdy=%" PRIu32 " phy_clk_rdy=%" PRIu32 " hdptx_dtb=%" PRIu32
         "\n",
         st3, (st3 & RK3576_HDMI_GRF_ST3_PLL_LOCK_DONE) ? 1u : 0u,
         (st3 & RK3576_HDMI_GRF_ST3_PHY_RDY) ? 1u : 0u,
         (st3 & RK3576_HDMI_GRF_ST3_PHY_CLK_RDY) ? 1u : 0u,
         (st3 & RK3576_HDMI_GRF_ST3_DTB) ? 1u : 0u);

    if (link2phy == 0)
      {
        _err("      => link2phy_status is ZERO while the PHY is locked: the "
             "link layer is not driving the parallel bus, so the pixels the "
             "VOP is delivering are not reaching TMDS.  The fault is between "
             "the interface and the link layer, inside the controller.\n");
      }
  }

  /* Full HPD decode.  Only bit 3 answers "is a sink there", and bit 5 is the
   * raw pad underneath the debounce.  The edge counter is the field that
   * separates "the monitor is not asserting HPD" from "this register is not
   * wired to that pad at all": a count that never moves across a plug and
   * unplug means the wiring, not the monitor.
   */

  _err("  HPD bits: level(3)=%d, raw pad(5)=%d, internal port(6)=%d, "
       "irq active(4)=%d, hold-time ok(7)=%d, edge count(2:0)=%d\n",
       (hpd & RK3576_HDMI_IOC_HPD_LEVEL) != 0,
       (hpd & RK3576_HDMI_IOC_HPD_RAW_PAD) != 0,
       (hpd & RK3576_HDMI_IOC_HPD_INT_PORT) != 0,
       (hpd & RK3576_HDMI_IOC_HPD_IRQ_ACTIVE) != 0,
       (hpd & RK3576_HDMI_IOC_HPD_LOWKEEP_OK) != 0,
       (int)(hpd & RK3576_HDMI_IOC_HPD_EDGE_COUNT));

  if ((hpd & RK3576_HDMI_IOC_HPD_LEVEL) == 0)
    {
      _err("NOTE: HDMI HPD reads as not attached, and MEASURED the raw pad "
           "bit is low as well -- so this is not a debounce artefact: the pad "
           "itself is not being driven high.  With the GPIO4C pad mux in "
           "place, that leaves the board wiring between the HDMI connector's "
           "HPD (pin 19) and this pad -- check the schematic for a level "
           "shifter, a pull-down, or a series element on that net.\n"
           "      The mode is force-enabled rather than waiting for HPD, so a "
           "low HPD cannot by itself explain a blank display: what is missing "
           "is the EDID/hot-plug path, not the video path.\n");
    }

  /* Informational only -- this is NOT treated as a failure.
   *
   * In the vendor driver the 0x15 test appears once, in dw_hdmi_qp_bind(), as
   * a heuristic for "the bootloader already left the controller running":
   *
   *     if (read_hpd() == connected && I2CM_INTERFACE_CONTROL0 &&
   *         (CMU_STATUS & HDMI_CTRL_CLK_EN) == HDMI_CTRL_CLK_EN)
   *             hdmi->initialized = true;
   *
   * Nothing in the enable path requires it, so failing it does not by itself
   * prove the controller is unusable -- but it does predict, exactly, which
   * registers answer.  Every access that has aborted on this board sits in a
   * domain CMU_STATUS reports as gated:
   *
   *   read  0x08e0 (HDCP2LOGIC_CONFIG0)      -> abort
   *   read  0x0814 (VIDEO_INTERFACE_STATUS0) -> abort
   *   read  0x0aa8 (PKTSCHED_PKT_EN)         -> abort
   *   write 0x0aac / 0x0aa8                  -> accepted, next access aborts
   *   read  0x0968 (link domain, up)         -> fine
   *   read  0x0000-0x03ff (always-on block)  -> fine
   *
   * MEASURED pre-VOP on this board: status 0x20a -- IPI and VIDQP gated,
   * LINKQP and AUD up -- with CMU_IPI_CLK_FREQ reading exactly 0 while
   * LINKQP and AUD measure non-zero.  So the frequency registers do work and
   * a zero is meaningful.  The decisive number is CMU_IPI_CLK_FREQ, logged
   * by rk3576_hdmi_sample_cmu(): zero there means the VOP is not clocking
   * this controller at all, and no register here can change that.
   */

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

uint32_t rk3576_hdmi_pixel_clock_hz(void) { return g_rk3576_hdmi.pixel_clock; }

/****************************************************************************
 * Name: rk3576_hdmi_ref_clock_hz
 *
 * Description:
 *   Return the controller's reference clock frequency, or 0.
 *
 ****************************************************************************/

uint32_t rk3576_hdmi_ref_clock_hz(void) { return g_rk3576_hdmi.ref_hz; }

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

/****************************************************************************
 * Name: rk3576_hdmi_sweep_ipi_format
 *
 * Description:
 *   Step the IPI format through every value the TRM names and dwell on each,
 *   so an observer watching the screen can say whether the format matters.
 *   See the prototype in rk3576_hdmi.h for why this is worth a flash.
 *
 ****************************************************************************/

int rk3576_hdmi_sweep_ipi_format(uint32_t dwell_ms)
{
  FAR struct rk3576_hdmi_s *priv = &g_rk3576_hdmi;
  static const char *const names[4] = {
    "RGB (4'h0)",         /* what this driver programs                  */
    "YCbCr 4:2:2 (4'h1)", /*                                            */
    "YCbCr 4:4:4 (4'h2)", /* what the working vendor firmware uses      */
    "YCbCr 4:2:0 (4'h3)"  /*                                            */
  };
  uint32_t fmt;

  if (!priv->initialized || priv->grf == 0u)
    {
      return -EIO;
    }

  if (dwell_ms == 0u)
    {
      dwell_ms = 2000u;
    }

  _err("RK3576 HDMI IPI format sweep starting: watch the screen.  Each value "
       "is held for %" PRIu32 " ms; any change from black identifies the "
       "format.  The normal value (RGB) is restored at the end.\n",
       dwell_ms);

  for (fmt = 0; fmt < 4u; fmt++)
    {
      _err("  IPI format sweep: now %s\n", names[fmt]);

      rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON8_OFF,
                            RK3576_HDMI_GRF_COLOR_FORMAT_MASK,
                            fmt << RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT);

      up_mdelay(dwell_ms);
    }

  /* Restore.  The pointer walk in names[] mirrors the loop above, so fmt == 4
   * here; writing 0 explicitly rather than names[4] keeps that obvious.
   */

  _err("  IPI format sweep: restoring RGB (4'h0)\n");

  rk3576_hdmi_grf_write(priv->grf, RK3576_HDMI_GRF_SOC_CON8_OFF,
                        RK3576_HDMI_GRF_COLOR_FORMAT_MASK,
                        RK3576_HDMI_GRF_FMT_RGB
                            << RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT);

  up_mdelay(200);

  _err("  IPI format sweep done; SOC_CON8 reads %08" PRIx32 "\n",
       getreg32(priv->grf + RK3576_HDMI_GRF_SOC_CON8_OFF));

  return OK;
}

#endif /* CONFIG_RK3576_HDMI */
