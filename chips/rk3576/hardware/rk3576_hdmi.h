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

#define RK3576_HDMI_CORE_ID    0x0000u
#define RK3576_HDMI_VER_NUMBER 0x0004u
#define RK3576_HDMI_VER_TYPE   0x0008u
#define RK3576_HDMI_CONFIG_REG 0x000cu

#define RK3576_HDMI_CONFIG_CEC (1u << 28)

/* ---------------------------------------------------------------------------
 * Reset manager
 * ---------------------------------------------------------------------------
 * GLOBAL_SWRESET_REQUEST is write-only and self-clearing: writing a 1 to a
 * field resets the corresponding functional unit *and its configuration
 * registers*, so a full reset also wipes everything programmed below -- which
 * is why it is pulsed before the programming pass rather than on the fly.
 *
 * MEASURED and corrected: the CRU soft-reset lines are NOT equivalent to this
 * register, and pulsing this one does not fix a cold controller either.
 * It was tried -- MASTER_SWINIT_P, below -- and REVERTED: it changed the
 * internal clock state for the worse (CMU_AUDQPCLK_FREQ fell from ~12000 Hz to
 * 0, LINKQP from ~37125 to 24000) and released no functional group at all
 * (STATUS0 stayed 0).  It did prove the reset machinery runs and that BIT(0)
 * is the right field, so the constant is kept for a future attempt, but it is
 * not the cure.
 *
 * GLOBAL_SWDISABLE is the inverse: 1 disables (holds in reset) a unit.  Only
 * CEC and the audio packet datapath are disabled by the mainline driver.
 *
 * The video datapath is nevertheless cleared explicitly below, because the
 * ROCKCHIP vendor driver does exactly that on every TMDS mode set:
 *
 *     hdmi_modb(hdmi, 0, AVP_DATAPATH_VIDEO_SWDISABLE, GLOBAL_SWDISABLE);
 *
 * and because MEASURED on this board the controller's own clock monitor
 * reports the IPI and video clock domains as gated OFF (see the CMU note
 * further down).  A unit held in software disable is a direct explanation
 * for its clock domains staying off, so the bit is cleared rather than
 * assumed.  Relying on the reset value is what the earlier note here did --
 * that is the difference between the two drivers, and mainline works only
 * because its bootloader has already produced video once.
 */

#define RK3576_HDMI_GLOBAL_SWRESET_REQUEST              0x0040u
#define RK3576_HDMI_GLOBAL_SWDISABLE                    0x0044u
#define RK3576_HDMI_RESET_MANAGER_CONFIG0               0x0048u
#define RK3576_HDMI_RESET_MANAGER_STATUS0               0x0050u
#define RK3576_HDMI_RESET_MANAGER_STATUS1               0x0054u
#define RK3576_HDMI_RESET_MANAGER_STATUS2               0x0058u

#define RK3576_HDMI_AVP_DATAPATH_PACKET_AUDIO_SWINIT_P  (1u << 10)
#define RK3576_HDMI_CEC_SWDISABLE                       (1u << 17)
#define RK3576_HDMI_AVP_DATAPATH_PACKET_AUDIO_SWDISABLE (1u << 10)
#define RK3576_HDMI_AVP_DATAPATH_VIDEO_SWDISABLE        (1u << 6)

/* GLOBAL_SWDISABLE[4] disables the whole AVP functional unit and, in the
 * TRM's words, "Also reset all contained configuration and status bits".  It
 * is a master switch sitting above the video bit above, and neither reference
 * driver ever writes it, so it is read back and reported.
 */

#define RK3576_HDMI_AVP_SWDISABLE (1u << 4)

/* ---------------------------------------------------------------------------
 * GLOBAL_SWRESET_REQUEST (0x0040) -- the controller's own reset request
 * ---------------------------------------------------------------------------
 * WRITE-ONLY and self-clearing: a field is written as 1 to START a reset and
 * reads back as nothing at all (reading the register aborts outright -- see
 * the note in rk3576_hdmi_report_status()).
 *
 * Each field is a *_SWINIT_P "init pulse" for one functional unit.  This is
 * the register TRM Part 2 24.6.1.3 refers to:
 *
 *     "Reset the controller using the imainrst_n signal."
 *
 * and it is the ONE part of the documented bring-up that this driver never
 * performed: the CRU soft-resets only gate the controller's clock domain, and
 * they do not run its internal reset/init state machine.
 *
 * Why it matters here: RESET_MANAGER_STATUS0 reports every AVP functional
 * group as still ASSERTED ("Init_n is asserted" = held in reset) on this
 * board, and forcing the reset manager's clock-domain override from CLK_OFF
 * to CLK_ON does NOT release them (measured).  That is the signature of a
 * controller whose initialisation was never kicked off -- on the reference
 * systems U-Boot had already done it.  A reset pulse is what clears this
 * state; the reset manager only decides *when* to release afterwards.
 *
 * Bit numbers below are taken from the vendor header
 * (dw-hdmi-qp.h: AVP_DATAPATH_PACKET_AUDIO_SWINIT_P BIT(10),
 * EARCRX_CMDC_SWINIT_P BIT(27)) and from the TRM's own bit column for
 * MASTER_SWINIT_P.  The per-unit bit for the VIDEO datapath is deliberately
 * NOT guessed: the TRM's text export scrambles that field's bit number and a
 * wrong bit in this register resets live configuration.  MASTER_SWINIT_P is
 * certain, covers the whole controller, and is used instead.
 *
 * Ordering, if it is ever used again: MASTER_SWINIT_P "resets all functional
 * units and all configuration registers", so it must run BEFORE any register
 * in this file is programmed.  It is currently NOT written anywhere -- see the
 * note further up.
 */

#define RK3576_HDMI_MASTER_SWINIT_P                    (1u << 0)
#define RK3576_HDMI_AVP_DATAPATH_PACKET_AUDIO_SWINIT_P (1u << 10)
#define RK3576_HDMI_EARCRX_CMDC_SWINIT_P               (1u << 27)

/* Settle time after the reset pulse, before the register file is trusted
 * again.  The pulse is self-clearing, so this only has to cover the internal
 * reset propagating through every clock domain.
 */

#define RK3576_HDMI_SWRESET_SETTLE_US 20000u

/* ---------------------------------------------------------------------------
 * Reset manager (RESET_MANAGER_CONFIG0, 0x0048)
 * ---------------------------------------------------------------------------
 * The controller's internal reset manager releases each FUNCTIONAL GROUP
 * (the AVP video datapath and so on) per clock domain, and it decides when to
 * do that from the live cmu_status.<clk>_off_st values -- unless software
 * overrides it, one domain at a time:
 *
 *   [12] ARC_BPCLK_OVR_VALUE   [11] AUDCLK_OVR_VALUE
 *   [10] LINKQPCLK_OVR_VALUE   [ 9] VIDQPCLK_OVR_VALUE
 *   [ 8] IPI_CLK_OVR_VALUE     [7:5] reserved (write 0)
 *   [ 4] ARC_BPCLK_OVR_EN      [ 3] AUDCLK_OVR_EN
 *   [ 2] LINKQPCLK_OVR_EN      [ 1] VIDQPCLK_OVR_EN
 *   [ 0] IPI_CLK_OVR_EN
 *
 * A _VALUE bit is 1'b1 for "CLK_ON -- the Reset Manager overlaps other
 * domains with this clock" and 1'b0 for "CLK_OFF -- it does not".
 *
 * RESET_MANAGER_STATUS0 (0x0050) reports the outcome per domain, and EVERY
 * field's documented "Value After Reset" is 0x0, which decodes as ASSERTED:
 *
 *   0x0 (ASSERTED):     Init_n is asserted  -- group held in reset
 *   0x1 (NOT_ASSERTED): Init_n is released
 *
 * There is a deadlock in that arrangement for a driver that starts from a
 * cold controller: the video group is held until the interface clock appears,
 * but the interface clock arrives from the VOP which the reset is not
 * blocking -- yet the manager never re-evaluates upward on its own.  The
 * override is the documented escape.
 */

#define RK3576_HDMI_RST_MGR_IPI_CLK_EN   (1u << 0)
#define RK3576_HDMI_RST_MGR_VIDQPCLK_EN  (1u << 1)
#define RK3576_HDMI_RST_MGR_LINKQPCLK_EN (1u << 2)
#define RK3576_HDMI_RST_MGR_AUDCLK_EN    (1u << 3)
#define RK3576_HDMI_RST_MGR_ARC_BPCLK_EN (1u << 4)

#define RK3576_HDMI_RST_MGR_IPI_CLK_ON   (1u << 8)
#define RK3576_HDMI_RST_MGR_VIDQPCLK_ON  (1u << 9)
#define RK3576_HDMI_RST_MGR_LINKQPCLK_ON (1u << 10)
#define RK3576_HDMI_RST_MGR_AUDCLK_ON    (1u << 11)
#define RK3576_HDMI_RST_MGR_ARC_BPCLK_ON (1u << 12)

#define RK3576_HDMI_RST_MGR_ALL_CLK_ON                                  \
  (RK3576_HDMI_RST_MGR_IPI_CLK_EN | RK3576_HDMI_RST_MGR_VIDQPCLK_EN |   \
   RK3576_HDMI_RST_MGR_LINKQPCLK_EN | RK3576_HDMI_RST_MGR_AUDCLK_EN |   \
   RK3576_HDMI_RST_MGR_ARC_BPCLK_EN | RK3576_HDMI_RST_MGR_IPI_CLK_ON |  \
   RK3576_HDMI_RST_MGR_VIDQPCLK_ON | RK3576_HDMI_RST_MGR_LINKQPCLK_ON | \
   RK3576_HDMI_RST_MGR_AUDCLK_ON | RK3576_HDMI_RST_MGR_ARC_BPCLK_ON)

/* ---------------------------------------------------------------------------
 * Packet scheduler
 * ---------------------------------------------------------------------------
 * The vendor's DVI branch in dw_hdmi_qp_setup() writes both of these right
 * after selecting OPMODE_DVI and before enabling the PHY, and its
 * atomic_enable() repeats them for every non-FRL mode:
 *
 *     hdmi_writel(hdmi, 2, PKTSCHED_PKT_CONTROL0);
 *     hdmi_modb(hdmi, PKTSCHED_GCP_TX_EN, PKTSCHED_GCP_TX_EN,
 *               PKTSCHED_PKT_EN);
 *
 * Neither register was written by this driver before.  LINK_CONFIG0 alone
 * selects the mode; it does not start the packet scheduler, and without a
 * started scheduler the controller has nothing to put on the link.
 * PKTSCHED_PKT_CONTROL0 = 2 is the vendor's "packets on" value, and the GCP
 * transmit enable is written in DVI mode as well as in HDMI mode, so it is
 * copied rather than reasoned about.
 *
 * ORDERING IS NOT COSMETIC HERE.  Both registers live in the 0x0800-0x0aff
 * block, which is clock-gated until the VOP delivers an interface clock, and
 * touching a gated block on this controller has consequences:
 *
 *   read  0x0aa8  -> synchronous external abort
 *   write 0x0aac  -> accepted, but the NEXT controller access then aborts
 *
 * So they are programmed from rk3576_hdmi_report_status(), and only once
 * CMU_STATUS proves the display clock domains are up.  Writing them earlier
 * is not merely useless, it takes the controller down.
 */

#define RK3576_HDMI_PKTSCHED_PKT_EN             0x0aa8u
#define RK3576_HDMI_PKTSCHED_GCP_TX_EN          (1u << 3)
#define RK3576_HDMI_PKTSCHED_PKT_CONTROL0       0x0aacu
#define RK3576_HDMI_PKTSCHED_PKT_CONTROL0_START 2u

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

#define RK3576_HDMI_TIMER_BASE_CONFIG0         0x0080u
#define RK3576_HDMI_TIMER_BASE_STATUS0         0x0084u

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

#define RK3576_HDMI_CMU_CONFIG0 0x00a0u
#define RK3576_HDMI_CMU_CONFIG1 0x00a4u
#define RK3576_HDMI_CMU_CONFIG2 0x00a8u
#define RK3576_HDMI_CMU_CONFIG3 0x00acu
#define RK3576_HDMI_CMU_STATUS  0x00b0u

/* The "controller clocks are up" test, taken verbatim from the vendor
 * driver's dw_hdmi_qp.c:
 *
 *     #define HDMI_CTRL_CLK_EN 0x15
 *     ... (hdmi_readl(hdmi, CMU_STATUS) & HDMI_CTRL_CLK_EN)
 *                                                == HDMI_CTRL_CLK_EN
 *
 * mainline's header carries the same number as DISPLAY_CLK_LOCKED (0x15,
 * next to DISPLAY_CLK_MONITOR = 0x3f) but never actually uses it.
 *
 * MEASURED on this board: CMU_STATUS = 0x0000020a, and 0x20a & 0x15 == 0 --
 * i.e. NOT locked.  In that state the 0x0800-0x08ff register block does not
 * respond and reading it aborts on the bus (measured twice: on 0x08e0 and on
 * 0x0814, the latter even after the VOP was running).  Whatever is tried
 * next, this is the pass/fail signal -- get CMU_STATUS to satisfy this test
 * before trusting any register in the upper block.
 */

#define RK3576_HDMI_CMU_CTRL_CLK_EN 0x15u

/* CMU_STATUS bit map.
 *
 * Each clock domain owns a PAIR of bits: the odd one reports the clock as
 * gated OFF, the even one below it reports the domain as up.  All three
 * display domains must be up for the vendor's 0x15 test to pass, and 0x3f is
 * the mask covering every domain:
 *
 *   bit 0  IPI_CLK_UP     bit 1  IPI_CLK_OFF     interface, VOP -> HDMITX
 *   bit 2  VIDQPCLK_UP    bit 3  VIDQPCLK_OFF    video datapath
 *   bit 4  LINKQPCLK_UP   bit 5  LINKQPCLK_OFF   TMDS link / serialiser
 *   bit 6  AUDCLK_UP      bit 7  AUDCLK_OFF      audio
 *   bit 8  EARC_BPCLK_UP  bit 9  EARC_BPCLK_OFF  eARC
 *
 * MEASURED: 0x0000020a before the VOP starts (IPI off, VIDQP off, eARC off)
 * and 0x0000025a after it (LINKQP and AUD come up).  The two OFF bits that
 * never clear are the video ones -- the pair a stalled video datapath leaves
 * behind.  A serialiser clocked by LINKQP but fed no pixels repeats one
 * constant symbol, which is precisely the clean, unvarying 148.5 MHz tone
 * the scope shows on every data lane.
 */

#define RK3576_HDMI_CMU_IPI_CLK_OFF    (1u << 1)
#define RK3576_HDMI_CMU_VIDQPCLK_OFF   (1u << 3)
#define RK3576_HDMI_CMU_LINKQPCLK_OFF  (1u << 5)
#define RK3576_HDMI_CMU_AUDCLK_OFF     (1u << 7)
#define RK3576_HDMI_CMU_EARC_BPCLK_OFF (1u << 9)

/* Per-domain frequency read-back: what the clock monitor actually measures
 * on each internal clock.  These live in the same always-on block as
 * CMU_STATUS (0x00b0, proven readable) and answer "is the VOP feeding this
 * controller at all" without touching anything in 0x0800-0x08ff.
 *
 * A zero IPI reading means the VOP is not delivering a video clock to this
 * controller and no amount of programming here can fix it; a live IPI with a
 * gated VIDQP points inside the controller instead.
 */

#define RK3576_HDMI_CMU_IPI_CLK_FREQ    0x00b4u
#define RK3576_HDMI_CMU_VIDQPCLK_FREQ   0x00b8u
#define RK3576_HDMI_CMU_LINKQPCLK_FREQ  0x00bcu
#define RK3576_HDMI_CMU_AUDQPCLK_FREQ   0x00c0u
#define RK3576_HDMI_CMU_EARC_BPCLK_FREQ 0x00c4u

/* ---------------------------------------------------------------------------
 * I2C master (DDC)
 * ---------------------------------------------------------------------------
 * The controller embeds the DDC master used to read the sink EDID.  The
 * reference driver only initialises it; actual transfers go through the
 * I2CM_INTERFACE_* windowed register file.  EDID reading is not part of the
 * DVI bring-up, so only the init constants are defined.
 */

#define RK3576_HDMI_I2CM_FM_SCL_CONFIG0     0x00e4u
#define RK3576_HDMI_I2CM_CONFIG0            0x00e8u
#define RK3576_HDMI_I2CM_CONTROL0           0x00ecu
#define RK3576_HDMI_I2CM_STATUS0            0x00f0u
#define RK3576_HDMI_I2CM_INTERFACE_CONTROL0 0x00f4u

#define RK3576_HDMI_I2CM_CONTROL0_SWRESET   0x01u
#define RK3576_HDMI_I2CM_FM_SCL_CONFIG0_VAL 0x085c085cu
#define RK3576_HDMI_I2CM_FM_EN              (1u << 0)

/* SCDC (Status and Data Channel) -- HDMI 1.4+ scrambling control.  Not needed
 * for DVI mode but the registers must exist so that future HDMI 1.4 modes can
 * enable scrambling without another research pass.  SCDC_STATUS0
 * FRL_START/FLT_UPDATE are only meaningful in FRL mode.
 */

#define RK3576_HDMI_SCDC_CONFIG0       0x0140u
#define RK3576_HDMI_SCDC_CONTROL0      0x0148u
#define RK3576_HDMI_SCDC_STATUS0       0x0150u

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
 * DO NOT READ ANY REGISTER IN THIS BLOCK.  It is retained only so that the
 * addresses are named somewhere and cannot be reintroduced by accident.
 *
 * MEASURED: reading 0x0804 panics the kernel with a synchronous external
 * abort (ESR_EL1 = 0x96000210, EC 0x25 / DFSC 0x10), and it does so even when
 * the CMU proves ipi_clk, vidqpclk and linkqpclk all running at 37125 Hz.  A
 * probe of these registers was added on the assumption that live clocks would
 * make them answer; they do not.  The disassembly-pinned faulting instruction
 * was `ldr w21, [x21]` on base + 0x804, and the PKTSCHED WRITEs immediately
 * before it had succeeded, so this is not a poisoned-bus artefact -- the AVP
 * register window simply does not respond.
 *
 * That matches MAINUNIT_0_INT_STATUS bit31, described as "all NON-POWERED OFF
 * units between AVPUNIT, PKTFIFO, and CEC are ready": the AVP block's register
 * interface is powered down even while the pixel pipeline it drives is live.
 * As a consequence the controller's own view of the interface is not
 * inspectable on this SoC.  Every diagnostic must come from VO0_GRF, CRU, VOP
 * or IOC, which are plain syscon blocks and safe to read.
 *
 * The same applies to PKTSCHED_PKT_EN (0x0aa8): writes are accepted, reads
 * abort.  Never add a read-back of either.
 */

#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG0                         0x0800u
#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG1                         0x0804u /* read aborts */
#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG2                         0x0808u /* read aborts */
#define RK3576_HDMI_VIDEO_INTERFACE_CONTROL0                        0x080cu /* read aborts */
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0                         0x0814u /* read aborts */

#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_FORMAT_SHIFT        0
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_FORMAT_MASK         0x0fu
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_COLOR_DEPTH_SHIFT   4
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_IPI_COLOR_DEPTH_MASK    0xf0u
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_COLOR_DEPTH_SHIFT  8
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_COLOR_DEPTH_MASK   0xf00u
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_VIDEO_FORMAT_SHIFT 12
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HDMI_VIDEO_FORMAT_MASK  0x7000u
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_VSYNC_POLARITY          (1u << 17)
#define RK3576_HDMI_VIDEO_INTERFACE_STATUS0_HSYNC_POLARITY          (1u << 18)

/* ---------------------------------------------------------------------------
 * HDCP2 logic bypass
 * ---------------------------------------------------------------------------
 * HDCP2_BYPASS routes the datapath around the HDCP2 cipher.  The reference
 * driver sets it on *every* mode set, unconditionally and before LINK_CONFIG0.
 * The TRM states the consequence of leaving it clear, and it is about the
 * VIDEO datapath rather than the output framing (see the DVI note below):
 * "0x0 (BYPASS_OFF): The video datapath goes through the External HDCP2
 * Module", which is the reset value.
 *
 * WRITE IT, BUT ONLY ONCE THE BLOCK'S CLOCK IS UP.
 *
 * MEASURED: a read of 0x08e0 (which is what modifyreg() starts with) produces
 * a synchronous external abort, twice, in both access orders -- before and
 * after the PHY is powered on.  Reading and writing 0x0968, in the same
 * register block, works fine in the same binary.
 *
 * That last sentence is the whole clue, and the answer is timing rather than
 * anything specific to HDCP: everything in 0x0800-0x0aff is clock-gated until
 * the CMU's video clock domains come up, which happens once the VOP is
 * delivering an interface clock.  0x0968 was reached late in the bring-up
 * (clk_locked true, so it worked) while every 0x08e0 attempt came from
 * initialize() or enable(), long before the VOP had started.  The writes that
 * did run with the clocks up -- PKTSCHED_PKT_CONTROL0 (0x0aac) and
 * PKTSCHED_PKT_EN (0x0aa8), in the same region -- succeeded, which is the
 * control that closes the question.
 *
 * So HDCP2_BYPASS is written from rk3576_hdmi_report_status(), in the same
 * clk_locked branch as the packet scheduler, and nowhere else.
 *
 * That "something" is most likely its own clocks.  The HDCP block is gated
 * separately in CRU_GATE_CON63 (all "when high, disable clock", reset 0):
 *
 *   [14] pclk_hdcp0        [13] hclk_hdcp0
 *   [12] aclk_hdcp0        [ 9] aclk_hdcp0_biu
 *
 * (The same register holds the VO0 roots at [0] aclk, [1] hclk and [3] pclk,
 * and aclk_hdc0_biu next to pclk_vo0_grf at [10] -- all verified against the
 * TRM.  rk3576_clk_register_vo0() registers the three VO0 roots; none of the
 * HDCP ones is registered, and nothing enables them.)
 *
 * THE HDCP0 GATES ARE NOT THE ANSWER TO 0x08e0, but the note that used to sit
 * here claimed they were and concluded that the bypass register could not be
 * written at all.  That conclusion is retracted: the driver now writes it from
 * the clk_locked branch of rk3576_hdmi_report_status(), on the same footing as
 * the packet-scheduler writes in the same 0x0800-0x0aff region.
 */

#define RK3576_HDMI_HDCP2LOGIC_CONFIG0 0x08e0u
#define RK3576_HDMI_HDCP2_BYPASS       (1u << 0)

/* HDCP block clock gates, CRU_GATE_CON63 (0x272008FC).  Verified against the
 * TRM: bit 14 pclk_hdcp0_en, bit 13 hclk_hdcp0_en, bit 12 aclk_hdcp0_en,
 * bit 9 aclk_hdcp0_biu_en -- all "When high, disable clock", reset 0.
 *
 * THESE ARE NOT HDCP-ONLY.  They have to be enabled for the VIDEO path too,
 * because the interface clock passes THROUGH the HDCP0 block.  The evidence
 * is a VO0_GRF field in SOC_CON13 called `grf_hdcp_ipiclk_disable` --
 * "HDCP0 ipiclk gating.  When high, disable clock" -- i.e. the IPI clock has
 * an explicit gate at the HDCP0 boundary, which only makes sense if it routes
 * through there.
 *
 * SUPERSEDED.  This "the interface clock passes through HDCP0" reading rested
 * on VO0_GRF SOC_CON13's grf_hdcp_ipiclk_disable field, and that field reads 0
 * -- nothing is gated at the HDCP0 boundary -- so the theory does not hold.
 * The ipi_clk fault had a different cause (SOC_CON9[9], since fixed).
 *
 * The gates themselves are benign and cost nothing to leave alone:
 * CRU_GATE_CON63 reads 0 out of reset, i.e. all four HDCP0 clocks are already
 * running, and the driver never writes any GATE_CON at all (it only reads this
 * one, as a diagnostic).  They are recorded here so the next reader does not
 * re-derive them.
 *
 * DVI MODE NEEDS THE BYPASS TOO, which was this driver's most expensive
 * mistake, so it is spelled out rather than implied.  With the bit clear the
 * video datapath passes through the external HDCP2 module, and that is true
 * whether the link carries DVI or HDMI framing -- the TRM sentence is about
 * the datapath, not the mode.  U-Boot sets the bit UNCONDITIONALLY, ahead of
 * its DVI/HDMI branch in dw_hdmi_setup(), so its DVI path sets it as well.
 */

#define RK3576_HDMI_HDCP_CLK_GATE_MASK      \
  ((1u << RK3576_HDMI_HDCP_PCLK_GATE_BIT) | \
   (1u << RK3576_HDMI_HDCP_HCLK_GATE_BIT) | \
   (1u << RK3576_HDMI_HDCP_ACLK_GATE_BIT) | \
   (1u << RK3576_HDMI_HDCP_ACLK_BIU_GATE_BIT))

#define RK3576_HDMI_HDCP_PCLK_GATE_BIT     14
#define RK3576_HDMI_HDCP_HCLK_GATE_BIT     13
#define RK3576_HDMI_HDCP_ACLK_GATE_BIT     12
#define RK3576_HDMI_HDCP_ACLK_BIU_GATE_BIT 9

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

#define RK3576_HDMI_LINK_CONFIG0      0x0968u

#define RK3576_HDMI_OPMODE_FRL        (1u << 0)
#define RK3576_HDMI_OPMODE_DVI        (1u << 4)
#define RK3576_HDMI_OPMODE_FRL_4LANES (1u << 8)

/* TMDS output FIFO.  Reset state is correct for the pixel-rate-equals-TMDS-
 * rate relationship the PHY produces; exposed only so a future debug pass can
 * dump it.
 */

#define RK3576_HDMI_TMDS_FIFO_CONFIG0  0x0970u
#define RK3576_HDMI_TMDS_FIFO_CONTROL0 0x0974u

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

#define RK3576_HDMI_MAINUNIT_0_INT_STATUS            0x3010u
#define RK3576_HDMI_MAINUNIT_0_INT_MASK_N            0x3014u
#define RK3576_HDMI_MAINUNIT_0_INT_CLEAR             0x3018u

#define RK3576_HDMI_MAINUNIT_0_INT_APB_REGBANK_READY (1u << 31)
#define RK3576_HDMI_MAINUNIT_0_INT_TIMER_BASE_LOCKED (1u << 24)

#define RK3576_HDMI_MAINUNIT_1_INT_STATUS            0x3020u
#define RK3576_HDMI_MAINUNIT_1_INT_MASK_N            0x3024u
#define RK3576_HDMI_MAINUNIT_1_INT_CLEAR             0x3028u

#define RK3576_HDMI_I2CM_OP_DONE_CLEAR               (1u << 0)
#define RK3576_HDMI_I2CM_NACK_RCVD_CLEAR             (1u << 2)

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
 *           [15] grf_con_hdmtx_revapb_sel   HDCP 1.4 SRAM CPU access;
 *                                          "in the func mode this must be 0"
 *   SOC_CON8 [11:8] grf_hdmitx_iipi_color_depth  0x5 = 8 bpc, 0x6 = 10 bpc
 *            [7:4]  grf_hdmitx_iipi_format       0x0 = RGB, 0x1 = YCbCr422,
 *                                                0x2 = YCbCr444, 0x3 =
 * YCbCr420 [3]    grf_hdmitx_cec_in_disable    1 = disable CEC [2]
 * grf_vo0_hdmi_gate         linksym clock gate, ACTIVE LOW (0 = clock on, 1 =
 * clock off) SOC_CON9 [9]  grf_hdmi_ch_sel      0 = HDMI fed by EDP, 1 = fed
 * by VOP SOC_CON13 [8] grf_ebc_dclk2hdmitx_disable  0 = dclk reaches the HDMI
 * TX SOC_CON14 [0] grf_hdmitx_i2s_sel   1 = HDMI I2S audio source is I2S [4]
 * grf_con_hdmitx_sclin_msk [5]  grf_con_hdmitx_sdain_msk [6]
 * grf_hdmi_grant_sel
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

#define RK3576_HDMI_GRF_SOC_CON1_OFF  0x0004u
#define RK3576_HDMI_GRF_SOC_CON8_OFF  0x0020u
#define RK3576_HDMI_GRF_SOC_CON9_OFF  0x0024u
#define RK3576_HDMI_GRF_SOC_CON13_OFF 0x0034u
#define RK3576_HDMI_GRF_SOC_CON14_OFF 0x0038u

/* VO0_GRF read-only status registers (TRM Part 1, 5.16.1 Registers Summary).
 *
 * These are the only window this SoC gives software into the HDMI TX's
 * link-layer activity, and they are the reason the HDMI bring-up can be
 * debugged at all without the controller's own protected register file.
 *
 * SOC_ST0 (0x00C0, grf_hdmitx_status0) -- live observation of the HDMI TX ->
 * HDPTXPHY parallel video/control bus:
 *
 *   [3:0]   int_ophy_txffe0     TMDS transmit feed-forward equaliser tap 0
 *   [7:4]   int_ophy_txffe1     tap 1
 *   [11:8]  int_ophy_txffe2     tap 2
 *   [15:12] int_ophy_txffe3     tap 3
 *   [23:16] link2phy_status     link layer -> PHY handshake state
 *   [24]    ii2cm_scl           DDC input SCL level
 *   [25]    ii2cm_sda           DDC input SDA level
 *   [26]    oi2cm_scl           DDC output SCL level
 *   [27]    oi2cm_sda           DDC output SDA level
 *   [30:28] grf_st_hdcp14_rmemaccess_ff2
 *
 * The TRM warns that these inputs cannot be de-meta-stated, so a value is
 * only trustworthy when three consecutive reads agree.
 *
 * SOC_ST3 (0x00CC) -- HDPTXPHY ready flags as seen from the VO0 side:
 *
 *   [3] hdptx_o_pll_lock_done
 *   [2] hdptx_o_phy_rdy
 *   [1] hdptx_o_phy_clk_rdy
 *   [0] hdptx_dtb                HDPTXPHY monitoring digital signal level
 */

#define RK3576_HDMI_GRF_SOC_ST0_OFF           0x00c0u
#define RK3576_HDMI_GRF_SOC_ST3_OFF           0x00ccu

#define RK3576_HDMI_GRF_LINK2PHY_STATUS_SHIFT 16
#define RK3576_HDMI_GRF_LINK2PHY_STATUS_MASK  0xffu
#define RK3576_HDMI_GRF_TXFFE0_MASK           0x0000000fu
#define RK3576_HDMI_GRF_TXFFE1_SHIFT          4
#define RK3576_HDMI_GRF_TXFFE2_SHIFT          8
#define RK3576_HDMI_GRF_TXFFE3_SHIFT          12
#define RK3576_HDMI_GRF_II2CM_SCL             (1u << 24)
#define RK3576_HDMI_GRF_II2CM_SDA             (1u << 25)
#define RK3576_HDMI_GRF_OI2CM_SCL             (1u << 26)
#define RK3576_HDMI_GRF_OI2CM_SDA             (1u << 27)

#define RK3576_HDMI_GRF_ST3_PLL_LOCK_DONE     (1u << 3)
#define RK3576_HDMI_GRF_ST3_PHY_RDY           (1u << 2)
#define RK3576_HDMI_GRF_ST3_PHY_CLK_RDY       (1u << 1)
#define RK3576_HDMI_GRF_ST3_DTB               (1u << 0)

#define RK3576_HDMI_GRF_FRLMOD                (1u << 0)

/* SOC_CON1 bit 15.  The TRM calls it grf_con_hdmtx_revapb_sel ("1'b1: the rev
 * ram can be accessed by the cpu with the apb bus.  Attention: in the func
 * mode, this must be 0"); mainline Linux names the same bit after its HDCP
 * 1.4 role (RK3576_HDMI_HDCP14_MEM_EN).  It resets to 0, which is what the
 * TRM requires of it in normal operation, so the driver deliberately does not
 * write it -- mainline's rk3576 glue does not write SOC_CON1 at all.
 */

#define RK3576_HDMI_GRF_REVAPB_SEL         (1u << 15)
#define RK3576_HDMI_GRF_COLOR_DEPTH_SHIFT  8
#define RK3576_HDMI_GRF_COLOR_DEPTH_MASK   0xf00u
#define RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT 4
#define RK3576_HDMI_GRF_COLOR_FORMAT_MASK  0xf0u
#define RK3576_HDMI_GRF_CEC_IN_DISABLE     (1u << 3)
#define RK3576_HDMI_GRF_HDMI_GATE          (1u << 2)

/* Colour-depth encoding for VO0_GRF_SOC_CON8 [11:8] and SOC_CON9 [7:4].
 *
 * MEASURED against the working vendor firmware on this board: it writes 0 for
 * 8 bpc.  Mainline Linux agrees -- dw_hdmi_qp-rockchip.c defines
 * RK3576_8BPC as 0x0 and writes it into this exact field from
 * dw_hdmi_qp_rk3576_enc_init().  Both agree on 0x6 for 10 bpc.
 *
 * An earlier version of this file used 0x5, reasoning from the TRM table
 * ("4'h5: 8 bits per component") and from the MIPI DSI side of the same GRF
 * block, which does use 0x5 for 8 bpc.  That reasoning was wrong on two
 * counts and it cost a debugging cycle:
 *
 *   1. SOC_CON10 (dsihost_ipi_color_depth, what the DSI path uses) and
 *      SOC_CON8 (grf_hdmitx_iipi_color_depth, what the HDMI TX uses) are
 *      DIFFERENT fields feeding DIFFERENT consumers.  A value proven good on
 *      one says nothing about the other.  The MIPI path never writes
 *      SOC_CON8 at all.
 *   2. The TRM table for this field is demonstrably unreliable: it renders
 *      the 8 bpc entry as "4'h5: 0 bits per component", so its "Reserved"
 *      verdict on the other encodings cannot be trusted either.
 *
 * The measured value from a firmware that actually lights a display wins.
 */

#define RK3576_HDMI_GRF_BPC_8      0x0u
#define RK3576_HDMI_GRF_BPC_10     0x6u

#define RK3576_HDMI_GRF_FMT_RGB    0x0u
#define RK3576_HDMI_GRF_FMT_YUV422 0x1u
#define RK3576_HDMI_GRF_FMT_YUV444 0x2u
#define RK3576_HDMI_GRF_FMT_YUV420 0x3u

/* SOC_CON9 [9] grf_hdmi_ch_sel -- MUST BE WRITTEN TO 0.
 *
 * The register description reads "Select path between VOP and EDP.  1'b0:
 * Choose EDP path; 1'b1: Choose VOP path", which is why this driver originally
 * set it.  A dump of the same board running the vendor Debian image -- which
 * does drive a display -- reads
 *
 *     VO0_GRF SOC_CON9 (0x2601a024) = 0x00000000
 *
 * via two independent routes, and that is also the reset value.  The TRM's own
 * application note agrees with the hardware rather than with the bit-field
 * text: Part 2 12.6.3.4 says "Step2: GRF_VO0_CON09[9] = 1 ; // it enable the
 * hdmi controller data source come from ebc", pairing it with CON13[4] as the
 * two bits that route the EBC/VOP_LITE source into the HDMI TX.
 *
 * This board drives the controller from the standard VOP2 path, so 1 selects a
 * dead source and leaves the interface clock at zero for ever.
 */

#define RK3576_HDMI_GRF_CH_SEL_VOP (1u << 9)

/* SOC_CON9 [7:4] grf_hdcp_i_tmds1_color_depth.
 *
 * The TRM's requirement is explicit -- "HDCP0 color depth, align with HDMITX
 * ipi color depth" -- so this field must always carry the SAME value as
 * SOC_CON8 [11:8].  The two encodings are therefore defined in terms of one
 * another below rather than written out separately.  They were separate once,
 * and when RK3576_HDMI_GRF_BPC_8 was corrected from 0x5 to 0x0 to match the
 * working vendor firmware, this field silently stayed at 0x5 -- leaving the
 * IPI depth and the far side's expectation disagreeing, which is precisely
 * the condition the TRM warns about.  Tying them together makes that class of
 * mistake impossible.
 *
 * MEASURED on the working vendor firmware: SOC_CON8 = 0x00000020 and
 * SOC_CON9 = 0x00000000, i.e. 0 in BOTH fields.  That is what is reproduced.
 *
 * The name says "HDCP0" but the description ties it to the interface, and DVI
 * mode has no HDCP cipher for it to be about -- this is IPI plumbing that
 * happens to live next to the HDCP0 instance.
 */

#define RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_MASK  (0xfu << 4)
#define RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_8BPC  (RK3576_HDMI_GRF_BPC_8 << 4)
#define RK3576_HDMI_GRF_HDCP_TMDS1_DEPTH_10BPC (RK3576_HDMI_GRF_BPC_10 << 4)

#define RK3576_HDMI_GRF_DCLK2HDMITX_DISABLE    (1u << 8)

/* SOC_CON13[4] grf_sw_hdmi_1to4_en -- "HDMI 1to4 enable" (0 = Disable,
 * 1 = Enable), reset 0.  Note this is SOC_CON13, a different register from the
 * SOC_CON9 [7:4] colour-depth field above that happens to share bit 4.
 *
 * The TRM describes the module it controls: "If the output interface is
 * MIPI/HDMI, it will also pass through a 1to4 module, which will convert the
 * data out of [the source] into the input required by MIPI/HDMI."
 *
 * Bit position and requirement come from TRM Part 2 12.6.3.4, which states the
 * sequence outright:
 *
 *     b. VOP_LITE -> HDMI TX Controller
 *        Step1: GRF_VO0_CON13 [4] = 1 ; // it enable the grf_sw_hdmi_1to4
 *                                       //   with hdmi mode.
 *        Step2: GRF_VO0_CON9 [9] = 1 ;  // hdmi controller data source
 *
 * The field order in TRM Part 1's SOC_CON13 table confirms bit 4: counting up
 * from grf_sw_mipi_1to4_en at bit 0 gives mipi_vsync_pol 1, mipi_hsync_pol 2,
 * mipi_mode 3, **hdmi_1to4_en 4**, hdmi_vsync_pol 5, hdmi_hsync_pol 6,
 * ebc_dclk2edp_disable 7, ebc_dclk2hdmitx_disable 8.  That independently
 * confirms the position of the field above, and it agrees with mainline's VOP
 * grf_ctrl (.grf_hdmi_1to4_en = VOP_REG(RK3576_VO0_GRF_SOC_CON13, 0x1, 4) and
 * .grf_hdmi_pin_pol = VOP_REG(..., 0x3, 5)) -- two independent sources.
 *
 * CAVEAT, recorded so this is not mistaken for a confirmed fix: the TRM
 * section concerns the VOP_LITE/EBC source, and in mainline the field appears
 * only in the rk3576_lit_* (LITE) GRF control structure.  Our DSI path works
 * without setting the matching grf_sw_mipi_1to4_en at bit 0, which argues the
 * main-VOP path may not need it either.  It remains the only HDMI-specific
 * enable the TRM names and that this driver never wrote, it resets disabled,
 * and it costs one bit -- so it is set and the effect measured, not assumed.
 */

#define RK3576_HDMI_GRF_HDMI_1TO4_EN (1u << 4)

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
 *
 * The pads themselves also have to be switched away from GPIO before any of
 * this works -- see the IOMUX_SEL_L note below.
 */

#define RK3576_HDMI_IOC_MISC_CON0_OFF       0x0400u
#define RK3576_HDMI_IOC_MISC_CON1_OFF       0x0404u
#define RK3576_HDMI_IOC_HDMI_HPD_STATUS_OFF 0x0440u

/* MISC_CON0.  Watch the polarity: the TRM documents bit 2 as
 * "hdmitx_hpd_int_mask -- 1'b0: Interrupt enable, 1'b1: Interrupt disable",
 * so writing a 1 DISABLES.  The vendor writes 0 there and lets its own
 * hardirq set it afterwards, which only works because it has a handler.
 */

#define RK3576_HDMI_IOC_HPD_INT_CLR (1u << 1)
#define RK3576_HDMI_IOC_HPD_INT_MSK (1u << 2)

/* MISC_CON1[15:0] hdmitx_hpd_con, the HPD debounce.  Written verbatim from
 * the vendor's setup_hpd() ("regmap_write(hdmi->regmap, 0xa404, 0xffff0102)")
 * and decoded against TRM Part 2 24.6.4.1:
 *
 *   [15:14] = 00    internal HPD taken from the glitch-free port (best)
 *   [13: 8] != 0    enable the low-level-keep filter
 *   [ 7: 0] = 0x02  low level must persist 2 ms to be accepted
 */

#define RK3576_HDMI_IOC_MISC_CON1_DEBOUNCE 0xffff0102u

/* HDMITX_HPD_STATUS (VCCIO6_IOC + 0x0440), documented in TRM Part 2 24.6.4.1
 * bit by bit.  It is NOT a single flag:
 *
 *   [3]  internal HPD level -- the ONLY "is a sink attached" bit, and the
 *        one the vendor's rk3576 read_hpd() tests
 *   [5]  raw external iopad level, i.e. the pad underneath the debounce
 *   [6]  internal hpd port level
 *   [4]  HPD interrupt active
 *   [7]  1 = the pad has NOT been held low for the configured hold time
 *   [2:0] count of HPD edges
 *
 * MEASURED with a monitor connected and +5 V present: 0x00000080.  That is
 * bit 7 only -- bit 3 clear AND bit 5 clear, so the raw pad itself is low,
 * not merely the debounced level.  The edge counter is the field that tells
 * "the monitor is not asserting HPD" apart from "this register is not
 * connected to that pad at all": a counter that never moves across a
 * plug/unplug means wiring.
 */

#define RK3576_HDMI_IOC_HPD_LEVEL      (1u << 3)
#define RK3576_HDMI_IOC_HPD_IRQ_ACTIVE (1u << 4)
#define RK3576_HDMI_IOC_HPD_RAW_PAD    (1u << 5)
#define RK3576_HDMI_IOC_HPD_INT_PORT   (1u << 6)
#define RK3576_HDMI_IOC_HPD_LOWKEEP_OK (1u << 7)
#define RK3576_HDMI_IOC_HPD_EDGE_COUNT 0x7u

/* ---------------------------------------------------------------------------
 * GPIO4C pad function select (VCCIO6_IOC_GPIO4C_IOMUX_SEL_L)
 * ---------------------------------------------------------------------------
 * Reference only -- NOT written by the driver today (see the note in
 * rk3576_hdmi_routing()).
 *
 * The four HDMI SIDEBAND signals live on GPIO4C pads, whose function select
 * resets to 0 = GPIO.  Function 9 is the HDMI one for all four:
 *
 *   gpio4c0_sel [3:0]   -> HDMI_TX_CEC_M0
 *   gpio4c1_sel [7:4]   -> HDMI_TX_HPDIN_M0
 *   gpio4c2_sel [11:8]  -> HDMI_TX_SCL
 *   gpio4c3_sel [15:12] -> HDMI_TX_SDA
 *
 * These pads really are shared with GPIO, so HPD / DDC / CEC genuinely need
 * this muxing before they can work -- Linux does it through pinctrl
 * ("hdmi_txm0_pins", "hdmi_tx_scl", "hdmi_tx_sda").
 *
 * BUT the TMDS clock and data lanes are NOT here.  They are dedicated HDMI
 * pins and pass through no IOMUX, so this register has no bearing on whether
 * a picture appears -- only on whether hot-plug and EDID can be read.  It is
 * a prerequisite for adding HPD/EDID support, not for the video path, and
 * forcing four pads to a fixed function without checking the board schematic
 * can break whatever else they drive.  Wire it up when HPD/EDID is actually
 * implemented and testable.
 */

#define RK3576_HDMI_IOC_GPIO4C_IOMUX_SEL_L_OFF 0x0390u
#define RK3576_HDMI_IOC_GPIO4C_SEL_MASK        0xffffu

#define RK3576_HDMI_IOC_GPIO4C_CEC_SEL         (0x9u << 0) /* HDMI_TX_CEC_M0    */
#define RK3576_HDMI_IOC_GPIO4C_HPDIN_SEL       (0x9u << 4) /* HDMI_TX_HPDIN_M0  */
#define RK3576_HDMI_IOC_GPIO4C_SCL_SEL         (0x9u << 8) /* HDMI_TX_SCL       */
#define RK3576_HDMI_IOC_GPIO4C_SDA_SEL         (0x9u << 12) /* HDMI_TX_SDA       */

#define RK3576_HDMI_IOC_GPIO4C_HDMI_SEL                                \
  (RK3576_HDMI_IOC_GPIO4C_CEC_SEL | RK3576_HDMI_IOC_GPIO4C_HPDIN_SEL | \
   RK3576_HDMI_IOC_GPIO4C_SCL_SEL | RK3576_HDMI_IOC_GPIO4C_SDA_SEL)

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

#define RK3576_HDMI_RST_CON         64
#define RK3576_HDMI_RST_REF_BIT     9
#define RK3576_HDMI_RST_APB_BIT     7

#define RK3576_HDMI_PMU1RST_CON     1
#define RK3576_HDMI_PMU1RST_HPD_BIT 13

/* CRU_SOFTRST_CON63 (0x0afc) -- the HDCP0 and VO0_GRF/VO0_BIU resets.
 *
 * This is the reset half of the HDCP0 block, and it is a real gap in the
 * earlier investigation: the clock half was checked long ago (CRU_GATE_CON63
 * reads 0, so pclk/hclk/aclk/aclk_biu are all ungated) and that was taken as
 * "HDCP0 is fine, so it cannot be the blocker".  A block with running clocks
 * but an asserted reset is just as dead, and the gate register says nothing
 * about the reset line.
 *
 * It matters because the HDMI TX block diagram puts HDCP2 Logic INLINE with
 * the video datapath ("HDCP 2.x Encoder (External)" feeding the Image Pixel
 * Interface), and VO0_GRF carries `grf_hdcp_ipiclk_disable` -- "HDCP0 ipiclk
 * gating" -- which is the interface clock's own gate at that boundary.  So
 * the pixel-interface clock really does pass through HDCP0, which makes an
 * asserted HDCP0 reset a way to stop ipi_clk while every other checked
 * register still looks correct.
 *
 * Bit assignment cross-checked against the TRM's RO/RW column: the three
 * reserved rows are the only RO rows, and they land exactly where the
 * reserved entries sit in the description column.
 *
 * Polarity is the standard CRU convention: 1 asserts the reset, 0 releases.
 * Reset value is 0, so every one of these must read 0.
 */

#define RK3576_HDMI_VO0RST_CON           63
#define RK3576_HDMI_VO0RST_HDCP0_BIT     10 /* resetn_hdcp0       */
#define RK3576_HDMI_VO0RST_HDCP0_H_BIT   9  /* hresetn_hdcp0      */
#define RK3576_HDMI_VO0RST_HDCP0_A_BIT   8  /* aresetn_hdcp0      */
#define RK3576_HDMI_VO0RST_VO0GRF_P_BIT  6  /* presetn_vo0_grf    */
#define RK3576_HDMI_VO0RST_HDCP0_BIU_BIT 5  /* aresetn_hdcp0_biu  */
#define RK3576_HDMI_VO0RST_VO0BIU_P_BIT  3  /* presetn_vo0_biu    */
#define RK3576_HDMI_VO0RST_VO0BIU_H_BIT  1  /* hresetn_vo0_biu    */

/* VOP SYS_CTRL_SYS_AUTO_GATING_CTRL_IMD (VOP + 0x0008), read-only here.
 *
 * The VOP driver owns this register; it is read back only because the value
 * decides whether the hardware may gate the output-interface clock on its
 * own.  The reference driver treats this as a live failure mode on RK3576 --
 * its comment says the auto-gating "may disable the aclk in some unexpected
 * cases, which detected by hardware automatically", which is precisely the
 * shape of a fault that leaves every register correct and every counter
 * running while the interface clock disappears.
 *
 * Bit numbers verified against the register's reset value 0x8155E953: every
 * one of the 18 bit slots matches, which pins the assignment.
 *
 * 0 = gating disabled for that block, 1 = auto-gating enabled.
 */

#define RK3576_HDMI_VOP_AUTO_GATING_OFF 0x0008u
#define RK3576_HDMI_VOP_AG_MASTER       (1u << 31)
#define RK3576_HDMI_VOP_AG_RGB_CLK      (1u << 24)
#define RK3576_HDMI_VOP_AG_EDP_PIX_CLK  (1u << 22)
#define RK3576_HDMI_VOP_AG_MIPI_PIX_CLK (1u << 20)
#define RK3576_HDMI_VOP_AG_HDMI_PIX_CLK (1u << 18)
#define RK3576_HDMI_VOP_AG_DP_PIX_CLK   (1u << 16)
#define RK3576_HDMI_VOP_AG_AXI_ACLK     (1u << 15)
#define RK3576_HDMI_VOP_AG_PORT_DCLK    (1u << 14)
#define RK3576_HDMI_VOP_AG_PRESCAN_ACLK (1u << 13)
#define RK3576_HDMI_VOP_AG_WB_ACLK      (1u << 11)
#define RK3576_HDMI_VOP_AG_OVERLAY_ACLK (1u << 8)
#define RK3576_HDMI_VOP_AG_ACLK_PRE     (1u << 7)
#define RK3576_HDMI_VOP_AG_WIN_ACLK     (1u << 6)

/* CRU_SOFTRST_CON75 (0x0b2c) [1] resetn_linksym_hdmitxphy0 -- the HDMI TX
 * PHY's link-symbol clock reset.
 *
 * The PHY driver owns and releases this one, but it is read back here because
 * it is the only reset on the whole HDMI path that this driver does not
 * write, and a link-symbol clock held in reset is one of the few remaining
 * ways to end up with a live reference clock, a locked PHY and a video
 * datapath that never clocks.
 *
 * Polarity is the standard CRU convention: 1 asserts the reset, 0 releases.
 */

#define RK3576_HDMI_LINKSYM_RST_CON 75
#define RK3576_HDMI_LINKSYM_RST_BIT 1

/* Time allowed between asserting and releasing a soft reset, and the settle
 * time after the last release.  The reference driver's reset-y toggles are
 * instantaneous, so these are only generous safety margins; a display that
 * fails to come up should be debugged in the PHY/GRF path first, not here.
 */

#define RK3576_HDMI_RST_PULSE_US 10

#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_HDMI_H */
