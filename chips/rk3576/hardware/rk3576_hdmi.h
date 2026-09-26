/****************************************************************************
 * chips/rk3576/hardware/rk3576_hdmi.h
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
 * RK3576 HDMI TX controller hardware register definitions.
 *
 * The controller is a Synopsys DesignWare HDMI QP (Quality-Path) transmitter
 * at RK3576_HDMITX_ADDR.  Reference: Rockchip RK3576 TRM Part 1, chapter
 * "HDMI TX Controller" (register descriptions).
 *
 * ---------------------------------------------------------------------------
 * Where the video format actually comes from
 * ---------------------------------------------------------------------------
 * Unlike older DesignWare HDMI cores, the QP transmitter has no "video
 * sampler" / CSC block that the driver feeds with timing parameters.  The
 * pixel data, the syncs and the bus format all arrive over the IPI (Internal
 * Pixel Interface) driven by the video output controller, and the colour
 * format + colour depth are conveyed out-of-band as two hardware signals from
 * VO0_GRF_SOC_CON8 (grf_hdmitx_iipi_format / grf_hdmitx_iipi_color_depth).
 *
 * Consequently the driver never programs a mode into the HDMI controller: it
 * only has to select the operating mode (TMDS vs FRL, HDMI vs DVI) in
 * LINK_CONFIG0 and let the PHY produce the matching pixel clock.  This is why
 * VIDEO_INTERFACE_CONFIG0 has no field definitions here -- the reference
 * driver never writes it either.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Core / configuration identification.
 *
 * CORE_ID          - controller version id (RK3576 reports 0x0000d0a1).
 * VER_NUMBER       - release number.
 * CONFIG_REG       - design-time feature bitmap; useful as a liveness probe
 *                    because it is read-only with a non-zero reset value.
 *                    CONFIG_CEC reports whether the CEC block was
 *                    instantiated.
 */

#define RK3576_HDMI_CORE_ID         0x0000u
#define RK3576_HDMI_VER_NUMBER      0x0004u
#define RK3576_HDMI_VER_TYPE        0x0008u
#define RK3576_HDMI_CONFIG_REG      0x000cu

#define RK3576_HDMI_CONFIG_CEC      (1u << 28)

/* ---------------------------------------------------------------------------
 * Reset manager
 * ---------------------------------------------------------------------------
 * GLOBAL_SWRESET_REQUEST is write-only and self-clearing: writing a 1 to a
 * field resets the corresponding functional unit *and its configuration
 * registers*, so a full reset also wipes everything programmed below.
 *
 * The reference driver only ever resets the audio datapath at runtime; the
 * whole controller is reset once, by the CRU lines, before the first
 * programming pass.  We follow that: rk3576_hdmi.c pulses the CRU resets and
 * never writes GLOBAL_SWRESET_REQUEST, which avoids having to re-run the
 * whole init sequence.
 *
 * GLOBAL_SWDISABLE is the inverse: 1 disables (holds in reset) a unit.  Only
 * CEC and the audio packet datapath are disabled by the reference driver --
 * the video datapath is enabled out of reset and must NOT be touched, so no
 * video disable bit is defined here.
 */

#define RK3576_HDMI_GLOBAL_SWRESET_REQUEST 0x0040u
#define RK3576_HDMI_GLOBAL_SWDISABLE       0x0044u
#define RK3576_HDMI_RESET_MANAGER_CONFIG0  0x0048u
#define RK3576_HDMI_RESET_MANAGER_STATUS0  0x0050u
#define RK3576_HDMI_RESET_MANAGER_STATUS1  0x0054u
#define RK3576_HDMI_RESET_MANAGER_STATUS2  0x0058u

#define RK3576_HDMI_AVP_DATAPATH_PACKET_AUDIO_SWINIT_P (1u << 10)
#define RK3576_HDMI_CEC_SWDISABLE                      (1u << 17)
#define RK3576_HDMI_AVP_DATAPATH_PACKET_AUDIO_SWDISABLE (1u << 10)

/* ---------------------------------------------------------------------------
 * Timer base
 * ---------------------------------------------------------------------------
 * TIMER_BASE_CONFIG0 holds the reference clock frequency in Hz (field
 * TIMER_REFERENCE_BASE, [28:0]) that the controller's internal timers count
 * against.  It is the *ref* clock of the HDMI node, which on RK3576 is
 * CLK_HDMITX0_REF: see rk3576_clk_register_hdmi() in rk3576_clk_tree.c, where
 * it is parented to aclk_vo0_root.  The reset value 0x179a7b00 is exactly
 * 396000000, i.e. GPLL (1188 MHz) divided by the aclk_vo0_root divider's
 * reset value of 3 -- read the live rate, never hard-code it.
 *
 * TIMER_BASE_STATUS0[0] latches high once the controller has accepted the
 * value; poll it instead of guessing a delay.
 */

#define RK3576_HDMI_TIMER_BASE_CONFIG0  0x0080u
#define RK3576_HDMI_TIMER_BASE_STATUS0  0x0084u

#define RK3576_HDMI_TIMER_REFERENCE_BASE_SHIFT 0
#define RK3576_HDMI_TIMER_REFERENCE_BASE_MASK  0x1fffffffu
#define RK3576_HDMI_TIMER_BASE_LOCKED_ST       (1u << 0)

/* ---------------------------------------------------------------------------
 * CMU (internal clock monitor)
 * ---------------------------------------------------------------------------
 * CMU_STATUS is a 6-bit per-domain "clock locked/running" bitmap whose value
 * is 0x3f when every internal clock is up and 0x15 while the pixel clock is
 * off.  Handy as a readiness check that does not depend on the PHY.
 */

#define RK3576_HDMI_CMU_CONFIG0  0x00a0u
#define RK3576_HDMI_CMU_CONFIG1  0x00a4u
#define RK3576_HDMI_CMU_CONFIG2  0x00a8u
#define RK3576_HDMI_CMU_CONFIG3  0x00acu
#define RK3576_HDMI_CMU_STATUS   0x00b0u

#define RK3576_HDMI_CMU_DISPLAY_CLK_MONITOR 0x3fu
#define RK3576_HDMI_CMU_DISPLAY_CLK_LOCKED  0x15u

/* ---------------------------------------------------------------------------
 * I2C master (DDC)
 * ---------------------------------------------------------------------------
 * The controller embeds the DDC master used to read the sink EDID.  The
 * reference driver only initialises it; actual transfers go through the
 * I2CM_INTERFACE_* windowed register file.  EDID reading is not part of the
 * DVI bring-up, so only the init constants are defined.
 */

#define RK3576_HDMI_I2CM_FM_SCL_CONFIG0    0x00e4u
#define RK3576_HDMI_I2CM_CONFIG0           0x00e8u
#define RK3576_HDMI_I2CM_CONTROL0          0x00ecu
#define RK3576_HDMI_I2CM_STATUS0           0x00f0u
#define RK3576_HDMI_I2CM_INTERFACE_CONTROL0 0x00f4u

#define RK3576_HDMI_I2CM_CONTROL0_SWRESET  0x01u
#define RK3576_HDMI_I2CM_FM_SCL_CONFIG0_VAL 0x085c085cu
#define RK3576_HDMI_I2CM_FM_EN             (1u << 0)

/* SCDC (Status and Data Channel) -- HDMI 1.4+ scrambling control.  Not needed
 * for DVI mode but the registers must exist so that future HDMI 1.4 modes can
 * enable scrambling without another research pass.  SCDC_STATUS0
 * FRL_START/FLT_UPDATE are only meaningful in FRL mode.
 */

#define RK3576_HDMI_SCDC_CONFIG0   0x0140u
#define RK3576_HDMI_SCDC_CONTROL0  0x0148u
#define RK3576_HDMI_SCDC_STATUS0   0x0150u

#define RK3576_HDMI_SCDC_STATUS_UPDATE (1u << 0)
#define RK3576_HDMI_SCDC_FRL_START     (1u << 4)
#define RK3576_HDMI_SCDC_FLT_UPDATE    (1u << 5)

/* ---------------------------------------------------------------------------
 * Main unit status
 * ---------------------------------------------------------------------------
 * MAINUNIT_STATUS0 is the aggregate state register (FRL vs TMDS, scrambling,
 * encryption).  The reference driver reads it in the HPD/EDID path only.
 */

#define RK3576_HDMI_MAINUNIT_STATUS0 0x0180u

/* ---------------------------------------------------------------------------
 * Video interface
 * ---------------------------------------------------------------------------
 * See the file banner: no field of VIDEO_INTERFACE_CONFIG0 is programmed by
 * the driver, because the IPI bus format is driven by VO0_GRF_SOC_CON8.  The
 * status register is still useful -- it reports back the polarities and format
 * the controller actually latched from the incoming IPI stream, which is the
 * cheapest way to prove that the VOP is really feeding the controller.
 */

#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG0   0x0800u
#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG1   0x0804u
#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG2   0x0808u
#define RK3576_HDMI_VIDEO_INTERFACE_CONTROL0  0x080cu
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0   0x0814u

#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_FORMAT_SHIFT      0
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_FORMAT_MASK       0x0fu
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_COLOR_DEPTH_SHIFT 4
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_COLOR_DEPTH_MASK  0xf0u
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_COLOR_DEPTH_SHIFT 8
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_COLOR_DEPTH_MASK  0xf00u
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_VIDEO_FORMAT_SHIFT 12
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_VIDEO_FORMAT_MASK  0x7000u
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_VSYNC_POLARITY          (1u << 17)
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HSYNC_POLARITY          (1u << 18)

/* ---------------------------------------------------------------------------
 * HDCP2 logic bypass
 * ---------------------------------------------------------------------------
 * HDCP2_BYPASS routes the datapath around the HDCP2 cipher.  The reference
 * driver sets it on *every* mode set, unconditionally and before LINK_CONFIG0;
 * with the bit clear the AVP holds the video datapath in the HDCP handshake
 * state and no pixels leave the controller.  This is easy to miss because the
 * register is named after an unrelated feature.
 */

#define RK3576_HDMI_HDCP2LOGIC_CONFIG0 0x08e0u
#define RK3576_HDMI_HDCP2_BYPASS       (1u << 0)

/* ---------------------------------------------------------------------------
 * Video configuration / link mode
 * ---------------------------------------------------------------------------
 * LINK_CONFIG0 selects the link operating mode.  The reference driver sets
 * OPMODE_DVI = 1 exactly when the attached sink did not report itself as an
 * HDMI sink (connector->display_info.is_hdmi == false), and clears it for
 * every HDMI sink; the bit's reset value is 1.
 *
 * For the DVI bring-up this is precisely what we want, and it is also the
 * simplest setting: with OPMODE_DVI = 1 the controller sends no AVI/VSI/GCP
 * infoframes, requires no audio, no HDCP and no SCDC scrambling, so the only
 * remaining prerequisite for a lit display is a valid TMDS clock and data
 * pair out of the PHY.
 */

#define RK3576_HDMI_LINK_CONFIG0 0x0968u

#define RK3576_HDMI_OPMODE_FRL        (1u << 0)
#define RK3576_HDMI_OPMODE_DVI        (1u << 4)
#define RK3576_HDMI_OPMODE_FRL_4LANES (1u << 8)

/* TMDS output FIFO.  Reset state is correct for the pixel-rate-equals-TMDS-
 * rate relationship the PHY produces; exposed only so a future debug pass can
 * dump it.
 */

#define RK3576_HDMI_TMDS_FIFO_CONFIG0   0x0970u
#define RK3576_HDMI_TMDS_FIFO_CONTROL0  0x0974u

/* ---------------------------------------------------------------------------
 * Interrupt registers
 * ---------------------------------------------------------------------------
 * Register-bank readiness is reported through MAINUNIT_0, the I2C master
 * completion through MAINUNIT_1.  The controller's register file is shadowed;
 * APB_REGBANK_READY_IRQ is the documented "the APB writes have landed" signal.
 *
 * The reference driver never polls it -- it simply masks all interrupts and
 * trusts that a subsequent read is ordered after the write -- so treating it
 * as optional (poll with a timeout, warn on expiry) is safe.
 */

#define RK3576_HDMI_MAINUNIT_0_INT_STATUS  0x3010u
#define RK3576_HDMI_MAINUNIT_0_INT_MASK_N  0x3014u
#define RK3576_HDMI_MAINUNIT_0_INT_CLEAR   0x3018u

#define RK3576_HDMI_MAINUNIT_0_INT_APB_REGBANK_READY (1u << 31)
#define RK3576_HDMI_MAINUNIT_0_INT_TIMER_BASE_LOCKED (1u << 24)

#define RK3576_HDMI_MAINUNIT_1_INT_STATUS  0x3020u
#define RK3576_HDMI_MAINUNIT_1_INT_MASK_N  0x3024u
#define RK3576_HDMI_MAINUNIT_1_INT_CLEAR   0x3028u

#define RK3576_HDMI_I2CM_OP_DONE_CLEAR   (1u << 0)
#define RK3576_HDMI_I2CM_NACK_RCVD_CLEAR (1u << 2)

/* Written to MAINUNIT_1_INT_CLEAR to acknowledge the I2C master's two
 * interrupt sources.  Named here rather than in the driver so that the
 * register and its bits stay together.
 */

#define RK3576_HDMI_MAINUNIT_1_INT_CLEAR_VAL \
  (RK3576_HDMI_I2CM_OP_DONE_CLEAR | RK3576_HDMI_I2CM_NACK_RCVD_CLEAR)

/* ---------------------------------------------------------------------------
 * VO0_GRF: routing, colour format and pad ownership
 * ---------------------------------------------------------------------------
 * VO0_GRF sits between the VOP and the HDMI controller and carries the
 * signals the controller itself cannot be told about over its register
 * interface.  Written with the standard Rockchip hiword-mask convention
 * (bits [31:16] enable the write of bits [15:0]).
 *
 *   SOC_CON1 [0]  grf_con_hdmitx_frlmod     0 = TMDS, 1 = FRL
 *           [15] grf_hdmitx_hdcp14_mem_en   HDCP 1.4 SRAM; leave 0 for DVI
 *   SOC_CON8 [11:8] grf_hdmitx_iipi_color_depth  0x0 = 8 bpc, 0x6 = 10 bpc
 *            [7:4]  grf_hdmitx_iipi_format       0x0 = RGB, 0x1 = YUV422,
 *                                                0x2 = YUV444, 0x3 = YUV420
 *            [3]    grf_vo0_hdmi_gate       linksym clock gate, ACTIVE LOW
 *   SOC_CON9 [9]  grf_hdmi_ch_sel      0 = HDMI fed by EDP, 1 = fed by VOP
 *   SOC_CON13 [8] grf_ebc_dclk2hdmitx_disable  0 = dclk reaches the HDMI TX
 *   SOC_CON14 [0] grf_hdmitx_i2s_sel   1 = HDMI I2S audio source is I2S
 *            [4]  grf_con_hdmitx_sclin_msk
 *            [5]  grf_con_hdmitx_sdain_msk
 *            [6]  grf_hdmi_grant_sel
 *
 * The SOC_CON14 bits hand the DDC pads to the controller's built-in I2C
 * master instead of letting them be GPIOs.  The colour depth / format in
 * SOC_CON8 is a *routing* signal into the controller's IPI receiver, which is
 * why the same information must not also be written to
 * VIDEO_INTERFACE_CONFIG0.
 *
 * The SOC_CON9 offset is shared with the MIPI DSI path (see
 * RK3576_VO0_GRF_SOC_CON9_OFF in hardware/rk3576_mipi_dsi.h).  It is
 * re-declared here under an HDMI-specific name rather than cross-including a
 * sibling driver's header, so that the two drivers stay independent; the two
 * macros are guaranteed to agree because both transcribe the same TRM table.
 */

#define RK3576_HDMI_GRF_SOC_CON1_OFF   0x0004u
#define RK3576_HDMI_GRF_SOC_CON8_OFF   0x0020u
#define RK3576_HDMI_GRF_SOC_CON9_OFF   0x0024u
#define RK3576_HDMI_GRF_SOC_CON13_OFF  0x0034u
#define RK3576_HDMI_GRF_SOC_CON14_OFF  0x0038u

#define RK3576_HDMI_GRF_FRLMOD          (1u << 0)
#define RK3576_HDMI_GRF_HDCP14_MEM_EN   (1u << 15)

#define RK3576_HDMI_GRF_COLOR_DEPTH_SHIFT  8
#define RK3576_HDMI_GRF_COLOR_DEPTH_MASK   0xf00u
#define RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT 4
#define RK3576_HDMI_GRF_COLOR_FORMAT_MASK  0xf0u
#define RK3576_HDMI_GRF_HDMI_GATE          (1u << 3)

#define RK3576_HDMI_GRF_BPC_8   0x0u
#define RK3576_HDMI_GRF_BPC_10  0x6u

#define RK3576_HDMI_GRF_FMT_RGB    0x0u
#define RK3576_HDMI_GRF_FMT_YUV422 0x1u
#define RK3576_HDMI_GRF_FMT_YUV444 0x2u
#define RK3576_HDMI_GRF_FMT_YUV420 0x3u

#define RK3576_HDMI_GRF_CH_SEL_VOP  (1u << 9)

#define RK3576_HDMI_GRF_DCLK2HDMITX_DISABLE (1u << 8)

#define RK3576_HDMI_GRF_I2S_SEL      (1u << 0)
#define RK3576_HDMI_GRF_SCLIN_MASK   (1u << 4)
#define RK3576_HDMI_GRF_SDAIN_MASK   (1u << 5)
#define RK3576_HDMI_GRF_GRANT_SEL    (1u << 6)

/* ---------------------------------------------------------------------------
 * Hot-plug detect (VCCIO6 IOC)
 * ---------------------------------------------------------------------------
 * The HPD pin is sampled by the IOC block, not by the HDMI controller.  The
 * status bit is read-only and level-based; MISC_CON0 must have its interrupt
 * mask set, otherwise the raw level keeps re-triggering the GIC.
 */

#define RK3576_HDMI_IOC_MISC_CON0_OFF       0x0400u
#define RK3576_HDMI_IOC_HDMI_HPD_STATUS_OFF 0x0440u

#define RK3576_HDMI_IOC_HPD_INT_CLR  (1u << 1)
#define RK3576_HDMI_IOC_HPD_INT_MSK  (1u << 2)
#define RK3576_HDMI_IOC_HPD_LEVEL    (1u << 3)

/* ---------------------------------------------------------------------------
 * CRU resets
 * ---------------------------------------------------------------------------
 * The reset-controller framework has no Rockchip CRU provider, so drivers
 * pulse CRU soft-reset lines directly -- the same scheme rk3576_sai.c,
 * rk3576_saradc.c and rk3576_vop.c use.
 *
 *   CRU_SOFTRST_CON64 (0x0b00)  [9] resetn_hdmitx0_ref
 *                               [7] presetn_hdmitx0
 *                               [5] resetn_dsihost0   (MIPI DSI, not ours)
 *                               [4] presetn_dsihost0  (MIPI DSI, not ours)
 *   PMU1CRU_SOFTRST_CON1 (0x0a04) [13] resetn_hdmitxhpd
 *                                 [11] resetn_hdptx_lane  (PHY)
 *                                 [10] resetn_hdptx_cmn   (PHY)
 *                                  [9] resetn_hdptx_init  (PHY)
 *
 * The PHY-owned bits are deliberately NOT listed as defines here: they belong
 * to the PHY driver (hardware/rk3576_hdptxphy.h) so that only one translation
 * unit writes them.  Only resetn_hdmitxhpd is pulsed by this driver, because
 * the HPD detector is part of the HDMI glue logic rather than the PHY.
 *
 * The link-symbol clock reset (CRU_SOFTRST_CON75[1]) belongs to the PHY as
 * well and is released by rk3576_hdptxphy.c.
 *
 * Polarity is the standard CRU convention: 1 asserts the reset, 0 releases it.
 */

#define RK3576_HDMI_RST_CON             64
#define RK3576_HDMI_RST_REF_BIT         9
#define RK3576_HDMI_RST_APB_BIT         7

#define RK3576_HDMI_PMU1RST_CON           1
#define RK3576_HDMI_PMU1RST_HPD_BIT       13

/* Time allowed between asserting and releasing a soft reset, and the settle
 * time after the last release.  The reference driver's reset-y toggles are
 * instantaneous, so these are only generous safety margins; a display that
 * fails to come up should be debugged in the PHY/GRF path first, not here.
 */

#define RK3576_HDMI_RST_PULSE_US  10

#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H */
