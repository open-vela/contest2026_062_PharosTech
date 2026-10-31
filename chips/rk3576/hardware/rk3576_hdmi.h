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
 * VIDEO_INTERFACE_CONFIG0 has no field definitions here -- nothing in this
 * driver needs to write it.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_HDMI_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_HDMI_H

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
 * GLOBAL_SWDISABLE is the inverse: 1 disables (holds in reset) a unit.  CEC
 * and the audio packet datapath are not used here, so they are left as found.
 *
 * The video datapath is nevertheless cleared explicitly, because MEASURED on
 * this board the controller's own clock monitor reports the IPI and video
 * clock domains as gated OFF (see the CMU note further down).  A unit held in
 * software disable is a direct explanation for its clock domains staying off,
 * so the bit is cleared rather than assumed.
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

/* ---------------------------------------------------------------------------
 * Packet scheduler
 * ---------------------------------------------------------------------------
 * Both registers must be written: LINK_CONFIG0 alone selects the mode, it
 * does not start the packet scheduler, and without a started scheduler the
 * controller has nothing to put on the link.  PKTSCHED_PKT_CONTROL0 = 2 is
 * the "packets on" value, and the GCP transmit enable is written in DVI mode
 * as well as in HDMI mode.
 *
 * Neither register was written by this driver before.
 *
 * ORDERING IS NOT COSMETIC HERE.  Both registers live in the 0x0800-0x0aff
 * block, which is clock-gated until the VOP delivers an interface clock, and
 * touching a gated block on this controller has consequences:
 *
 *   read  0x0aa8  -> synchronous external abort
 *   write 0x0aac  -> accepted, but the NEXT controller access then aborts
 *
 * So they are programmed from rk3576_hdmi_start_video_path(), and only once
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

/* The "controller clocks are up" test: (CMU_STATUS & 0x15) == 0x15, i.e. the
 * IPI, video-datapath and TMDS-link domains are all up.
 *
 * rk3576_hdmi_start_video_path() gates its upper-block programming on this
 * test, because until it passes an access to 0x0800-0x0aff is accepted by the
 * bus and then breaks the NEXT controller access.
 */

#define RK3576_HDMI_CMU_CTRL_CLK_EN 0x15u

/* CMU_STATUS bit map.
 *
 * Each clock domain owns a PAIR of bits: the odd one reports the clock as
 * gated OFF, the even one below it reports the domain as up.  All three
 * display domains must be up for the 0x15 test to pass, and 0x3f is
 * the mask covering every domain:
 *
 *   bit 0  IPI_CLK_UP     bit 1  IPI_CLK_OFF     interface, VOP -> HDMITX
 *   bit 2  VIDQPCLK_UP    bit 3  VIDQPCLK_OFF    video datapath
 *   bit 4  LINKQPCLK_UP   bit 5  LINKQPCLK_OFF   TMDS link / serialiser
 *   bit 6  AUDCLK_UP      bit 7  AUDCLK_OFF      audio
 *   bit 8  EARC_BPCLK_UP  bit 9  EARC_BPCLK_OFF  eARC
 */

/* ---------------------------------------------------------------------------
 * I2C master (DDC)
 * ---------------------------------------------------------------------------
 * The controller embeds the DDC master that reads the sink's EDID EEPROM.  It
 * is the only master on the DDC pads (VO0_GRF_SOC_CON14 hands them over), and
 * it is the reference driver's choice too, because SCDC and HDCP -- both
 * hardware state machines sharing the same pads -- are unreachable from any
 * external I2C controller.
 *
 * The runtime interface is a window: I2CM_INTERFACE_CONTROL0 carries the slave
 * address and the command for one byte, the data register carries that byte,
 * and the transfer completes on the MAINUNIT_1 interrupt.  Control0 and the
 * command field are the tricky part -- I2CM_FM_READ, I2CM_FM_WRITE,
 * I2CM_SHORT_READ and I2CM_EXT_READ all live INSIDE I2CM_WR_MASK ([4:1]), so
 * the command is programmed by writing one of those values *through* that
 * mask, and cleared by writing 0 through it.  I2CM_FM_EN (bit 0) is the only
 * command bit outside the mask, and the reference driver leaves it clear.
 */

#define RK3576_HDMI_I2CM_FM_SCL_CONFIG0     0x00e4u
#define RK3576_HDMI_I2CM_CONFIG0            0x00e8u
#define RK3576_HDMI_I2CM_CONTROL0           0x00ecu
#define RK3576_HDMI_I2CM_STATUS0            0x00f0u
#define RK3576_HDMI_I2CM_INTERFACE_CONTROL0 0x00f4u
#define RK3576_HDMI_I2CM_INTERFACE_CONTROL1 0x00f8u
#define RK3576_HDMI_I2CM_INTERFACE_WRDATA_0 0x00fcu /* WRDATA_0_3  */
#define RK3576_HDMI_I2CM_INTERFACE_RDDATA_0 0x010cu /* RDDATA_0_3  */

#define RK3576_HDMI_I2CM_CONTROL0_SWRESET   0x01u
#define RK3576_HDMI_I2CM_FM_SCL_CONFIG0_VAL 0x085c085cu

/* I2CM_INTERFACE_CONTROL0 fields. */

#define RK3576_HDMI_I2CM_ADDR          0x0ff000u /* [19:12] sub-address       */
#define RK3576_HDMI_I2CM_ADDR_SHIFT    12
#define RK3576_HDMI_I2CM_SLVADDR       0x000fe0u /* [11: 5] 7-bit slave address */
#define RK3576_HDMI_I2CM_SLVADDR_SHIFT 5
#define RK3576_HDMI_I2CM_WR_MASK       0x00001eu /* [ 4: 1] command field     */
#define RK3576_HDMI_I2CM_EXT_READ      (1u << 4) /* read, with segment pointer */
#define RK3576_HDMI_I2CM_SHORT_READ    (1u << 3) /* read, no sub-address */
#define RK3576_HDMI_I2CM_FM_READ       (1u << 2) /* read one byte              */
#define RK3576_HDMI_I2CM_FM_WRITE      (1u << 1) /* write one byte             */
#define RK3576_HDMI_I2CM_FM_EN         (1u << 0) /* fast-mode enable           */

/* I2CM_INTERFACE_CONTROL1 selects the EDID segment for extended reads.  The
 * base block lives in segment 0, which is the reset value, so a plain
 * base-block read never has to touch this register -- but it IS cleared at
 * init so a stale segment from a previous stage cannot redirect the read.
 */

#define RK3576_HDMI_I2CM_SEG_PTR  0x7f80u /* [14: 7]                    */
#define RK3576_HDMI_I2CM_SEG_ADDR 0x007fu /* [ 6: 0]                    */

/* The DDC master address block.  The EDID EEPROM answers at 0x50; 0x30 is the
 * segment-pointer pseudo-device, which this driver does not use.
 */

#define RK3576_HDMI_DDC_EDID_ADDR 0x50u

/* ---------------------------------------------------------------------------
 * Main unit 1 interrupt bits (the I2C master's completion signals)
 * ---------------------------------------------------------------------------
 * MAINUNIT_1_INT_STATUS, _MASK_N and _CLEAR all use the same bit positions;
 * _MASK_N is an active-low mask (1 = unmasked).  NuttX takes no interrupt from
 * this controller, so the driver polls _STATUS and unmasks the two bits for
 * the duration of a transfer, mirroring the reference driver's behaviour.
 */

#define RK3576_HDMI_I2CM_OP_DONE_IRQ   (1u << 0)
#define RK3576_HDMI_I2CM_NACK_RCVD_IRQ (1u << 2)

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
 * encryption).  It is only relevant to the HPD/EDID path.
 */

#define RK3576_HDMI_MAINUNIT_STATUS0 0x0180u

/* ---------------------------------------------------------------------------
 * Video interface
 * ---------------------------------------------------------------------------
 * DO NOT READ ANY REGISTER IN THIS BLOCK.  It is retained only so that the
 * addresses are named somewhere and cannot be reintroduced by accident.
 *
 * MEASURED: reading 0x0804 takes a synchronous external
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
 * HDCP2_BYPASS routes the datapath around the HDCP2 cipher.  It must be set
 * on *every* mode set, before LINK_CONFIG0.  The TRM states the consequence of
 * leaving it clear, and it is about the VIDEO datapath rather than the output
 * framing (see the DVI note below): "0x0 (BYPASS_OFF): The video datapath goes
 * through the External HDCP2 Module", which is the reset value.
 *
 * WRITE IT, AND WRITE IT FROM rk3576_hdmi_start_video_path() -- NOT FROM
 * initialize() OR enable().  Everything in 0x0800-0x0aff is clock-gated until
 * the CMU's video clock domains come up, which happens once the VOP is
 * delivering an interface clock, and MEASURED, an access made while it is
 * gated is accepted by the bus and then breaks the NEXT controller access:
 * writing 0x0aac and 0x08e0 each did it, and reading 0x08e0, 0x0804 or 0x0814
 * aborts outright.  The packet scheduler in the same region is programmed from
 * the same clk_locked branch, for the same reason.
 *
 * DVI MODE NEEDS THE BYPASS TOO, which was this driver's most expensive
 * mistake, so it is spelled out rather than implied.  With the bit clear the
 * video datapath passes through the external HDCP2 module, and that is true
 * whether the link carries DVI or HDMI framing -- the TRM sentence is about
 * the datapath, not the mode, so DVI mode sets it as well.
 */

#define RK3576_HDMI_HDCP2LOGIC_CONFIG0 0x08e0u
#define RK3576_HDMI_HDCP2_BYPASS       (1u << 0)

/* HDCP0 clock gates live in CRU_GATE_CON63 (0x272008FC): bit 14 pclk_hdcp0_en,
 * bit 13 hclk_hdcp0_en, bit 12 aclk_hdcp0_en, bit 9 aclk_hdcp0_biu_en -- all
 * "When high, disable clock", reset 0.  The driver never writes any GATE_CON:
 * the register reads 0 out of reset, i.e. all four HDCP0 clocks are already
 * running, and DVI mode has no HDCP cipher for them to feed.  Recorded here so
 * the next reader does not re-derive them.
 */

/* ---------------------------------------------------------------------------
 * Video configuration / link mode
 * ---------------------------------------------------------------------------
 * LINK_CONFIG0 selects the link operating mode.  OPMODE_DVI = 1 puts the
 * controller in DVI mode, which is the mode used here; the bit's reset value
 * is 1.
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
 * Nothing in the datapath depends on this flag, so treating it as optional
 * (poll with a timeout, warn on expiry) is safe.
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
 * EDID base block
 * ---------------------------------------------------------------------------
 * Only the base block is needed: a sink that offers anything else as its
 * preferred timing still advertises "I can be driven like this" first.  The
 * extension block count is read only to report whether the sink has more.
 *
 * The magic and checksum rules are the ones every EDID reader applies, and the
 * detailed timing descriptor below is the standard 18-byte layout.  Note that
 * the horizontal active/blanking high bits share one byte with the HIGH nibble
 * carrying the ACTIVE pixels -- byte 4 bits [7:4] = hactive[11:8], bits [3:0]
 * = hblank[11:8]; the vertical word (byte 7) is arranged the same way, and the
 * sync-offset/width high bits live in byte 11.
 */

#define RK3576_HDMI_EDID_LENGTH       128u
#define RK3576_HDMI_EDID_MAGIC_SIZE   8u
#define RK3576_HDMI_EDID_MANUFACTURER 8u  /* 2 bytes, big endian  */
#define RK3576_HDMI_EDID_PRODUCTCODE  10u /* 2 bytes, little endian */
#define RK3576_HDMI_EDID_SERIALNO     12u /* 4 bytes, little endian */
#define RK3576_HDMI_EDID_WEEK         16u
#define RK3576_HDMI_EDID_YEAR         17u /* minus 1990 */
#define RK3576_HDMI_EDID_VERSION      18u
#define RK3576_HDMI_EDID_REVISION     19u
#define RK3576_HDMI_EDID_VIDEO_INPUT  20u
#define RK3576_HDMI_EDID_HSIZE_CM     21u
#define RK3576_HDMI_EDID_VSIZE_CM     22u
#define RK3576_HDMI_EDID_GAMMA        23u
#define RK3576_HDMI_EDID_FEATURES     24u
#define RK3576_HDMI_EDID_DESCRIPTOR   54u /* first descriptor, 18 bytes */
#define RK3576_HDMI_EDID_DESC_NUMBER  4u
#define RK3576_HDMI_EDID_DESC_SIZE    18u
#define RK3576_HDMI_EDID_EXT_COUNT    126u
#define RK3576_HDMI_EDID_CHECKSUM     127u

/* Video input bitmap (byte 20). */

#define RK3576_HDMI_EDID_INPUT_DIGITAL            (1u << 7)
#define RK3576_HDMI_EDID_INPUT_VIDIF_MASK         0x0fu
#define RK3576_HDMI_EDID_INPUT_VIDIF_HDMIA        1u
#define RK3576_HDMI_EDID_INPUT_VIDIF_HDMIB        2u
#define RK3576_HDMI_EDID_INPUT_BITDEPTH_SHIFT     4u
#define RK3576_HDMI_EDID_INPUT_BITDEPTH_MASK      0x70u
#define RK3576_HDMI_EDID_INPUT_BITDEPTH_UNDEFINED 0u

/* Features bitmap (byte 24).  Bit 1 is the one that matters here: it is the
 * sink PROMISING that descriptor 1 holds its preferred timing.  When it is
 * clear the sink is free to put something else there, which is why the reader
 * scans all four descriptor slots rather than trusting slot 1.
 */

#define RK3576_HDMI_EDID_FEATURE_DPMS_STANDBY     (1u << 7)
#define RK3576_HDMI_EDID_FEATURE_DPMS_SUSPEND     (1u << 6)
#define RK3576_HDMI_EDID_FEATURE_DPMS_OFF         (1u << 5)
#define RK3576_HDMI_EDID_FEATURE_PREFERRED_TIMING (1u << 1)
#define RK3576_HDMI_EDID_FEATURE_SRGB             (1u << 2)

/* Display descriptor classification.  A descriptor whose first two bytes are
 * zero is not a timing: byte 3 then names what it actually is.
 */

#define RK3576_HDMI_DESC_TYPE            3u
#define RK3576_HDMI_DESC_TEXT            5u /* text/range fields start here */

#define RK3576_HDMI_DESC_TYPE_DUMMY      0x10u
#define RK3576_HDMI_DESC_TYPE_STDTIMING  0xf7u
#define RK3576_HDMI_DESC_TYPE_CVT        0xf8u
#define RK3576_HDMI_DESC_TYPE_DCM        0xf9u
#define RK3576_HDMI_DESC_TYPE_STDID      0xfau
#define RK3576_HDMI_DESC_TYPE_WHITEPOINT 0xfbu
#define RK3576_HDMI_DESC_TYPE_NAME       0xfcu
#define RK3576_HDMI_DESC_TYPE_LIMITS     0xfdu
#define RK3576_HDMI_DESC_TYPE_TEXT       0xfeu
#define RK3576_HDMI_DESC_TYPE_SERIAL     0xffu

#define RK3576_HDMI_DESC_TEXT_LEN        13u /* bytes 5..17 */

/* Display range limits (descriptor type 0xfd).  The maximum pixel clock is
 * stored in 10 MHz units, which is why it needs a *10 to become MHz -- a
 * detail worth naming, because reading it as MHz understates a sink's
 * capability by 10x and would make a perfectly good mode look unsupported.
 */

#define RK3576_HDMI_DESC_LIMITS_MIN_VFREQ      5u
#define RK3576_HDMI_DESC_LIMITS_MAX_VFREQ      6u
#define RK3576_HDMI_DESC_LIMITS_MIN_HFREQ      7u
#define RK3576_HDMI_DESC_LIMITS_MAX_HFREQ      8u
#define RK3576_HDMI_DESC_LIMITS_MAX_CLOCK      9u /* 10 MHz units */
#define RK3576_HDMI_DESC_LIMITS_MAX_CLOCK_UNIT 10000000u

/* Detailed timing descriptor (relative to RK3576_HDMI_EDID_DESCRIPTOR + i*18).
 *
 * NOTE: the whole RK3576_HDMI_DTD_* namespace belongs to these field
 * constants. They are macros, so an enum in a .c file that could also see them
 * must not reuse a name from it -- the preprocessor expands the enumerator and
 * the declaration turns into garbage.  The DTD classification enum in
 * rk3576_hdmi.c is therefore spelt RK3576_HDMI_DTD_STATUS_* for that reason.
 */

#define RK3576_HDMI_DTD_PIXCLOCK_LO 0u /* 10 kHz units, little endian */
#define RK3576_HDMI_DTD_PIXCLOCK_HI 1u
#define RK3576_HDMI_DTD_HACTIVE_LO  2u
#define RK3576_HDMI_DTD_HBLANK_LO   3u
#define RK3576_HDMI_DTD_H_MSBITS \
  4u /* [7:4] hactive[11:8], [3:0] hblank[11:8] */
#define RK3576_HDMI_DTD_VACTIVE_LO 5u
#define RK3576_HDMI_DTD_VBLANK_LO  6u
#define RK3576_HDMI_DTD_V_MSBITS \
  7u /* [7:4] vactive[11:8], [3:0] vblank[11:8] */
#define RK3576_HDMI_DTD_HSYNC_OFF_LO 8u
#define RK3576_HDMI_DTD_HSYNC_WID_LO 9u
#define RK3576_HDMI_DTD_VSYNC_LO \
  10u /* [7:4] vsync offset[3:0], [3:0] vsync width[3:0] */
#define RK3576_HDMI_DTD_SYNC_MSBITS        \
  11u /* [7:6] hoff[9:8], [5:4] hwid[5:4], \
       * [3:2] voff[5:4], [1:0] vwid[5:4] */
#define RK3576_HDMI_DTD_HACTIVE_MSB_SHIFT   4u /* 0xf0 -> bits [11:8] */
#define RK3576_HDMI_DTD_HBLANK_MSB_SHIFT    8u /* 0x0f -> bits [11:8] */
#define RK3576_HDMI_DTD_HACTIVE_MSB_MASK    0xf0u
#define RK3576_HDMI_DTD_HBLANK_MSB_MASK     0x0fu
#define RK3576_HDMI_DTD_VACTIVE_MSB_SHIFT   4u /* 0xf0 -> bits [11:8] */
#define RK3576_HDMI_DTD_VBLANK_MSB_SHIFT    8u /* 0x0f -> bits [11:8] */
#define RK3576_HDMI_DTD_VACTIVE_MSB_MASK    0xf0u
#define RK3576_HDMI_DTD_VBLANK_MSB_MASK     0x0fu
#define RK3576_HDMI_DTD_HSYNC_OFF_MSB_MASK  0xc0u
#define RK3576_HDMI_DTD_HSYNC_OFF_MSB_SHIFT 2u
#define RK3576_HDMI_DTD_HSYNC_WID_MSB_MASK  0x30u
#define RK3576_HDMI_DTD_HSYNC_WID_MSB_SHIFT 4u
#define RK3576_HDMI_DTD_VSYNC_OFF_MSB_MASK  0x0cu
#define RK3576_HDMI_DTD_VSYNC_OFF_MSB_SHIFT 2u
#define RK3576_HDMI_DTD_VSYNC_WID_MSB_MASK  0x03u
#define RK3576_HDMI_DTD_VSYNC_WID_MSB_SHIFT 4u
#define RK3576_HDMI_DTD_VSYNC_OFF_LO_SHIFT  4u
#define RK3576_HDMI_DTD_VSYNC_OFF_LO_MASK   0x0fu
#define RK3576_HDMI_DTD_VSYNC_WID_LO_MASK   0x0fu
#define RK3576_HDMI_DTD_FLAGS               17u
#define RK3576_HDMI_DTD_INTERLACED          (1u << 7)
#define RK3576_HDMI_DTD_SYNCTYPE_MASK       (3u << 3)
#define RK3576_HDMI_DTD_SYNCTYPE_SEPARATE   (3u << 3)
#define RK3576_HDMI_DTD_VSYNC_POLARITY      (1u << 2)
#define RK3576_HDMI_DTD_HSYNC_POLARITY      (1u << 1)

/* Sanity bounds applied by the decoder.  An EDID is untrusted input: a
 * malformed or dishonest one must not be able to size a framebuffer
 * allocation or wrap a porch calculation.
 */

#define RK3576_HDMI_DTD_MAX_ACTIVE 4096u

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

#define RK3576_HDMI_GRF_FRLMOD        (1u << 0)

/* SOC_CON1 bit 15.  The TRM calls it grf_con_hdmtx_revapb_sel ("1'b1: the rev
 * ram can be accessed by the cpu with the apb bus.  Attention: in the func
 * mode, this must be 0").  It resets to 0, which is what the TRM requires of
 * it in normal operation, so the driver deliberately does not write it.
 */

#define RK3576_HDMI_GRF_REVAPB_SEL         (1u << 15)
#define RK3576_HDMI_GRF_COLOR_DEPTH_SHIFT  8
#define RK3576_HDMI_GRF_COLOR_DEPTH_MASK   0xf00u
#define RK3576_HDMI_GRF_COLOR_FORMAT_SHIFT 4
#define RK3576_HDMI_GRF_COLOR_FORMAT_MASK  0xf0u
#define RK3576_HDMI_GRF_CEC_IN_DISABLE     (1u << 3)
#define RK3576_HDMI_GRF_HDMI_GATE          (1u << 2)

/* Colour-depth encoding for VO0_GRF_SOC_CON8 [11:8] and SOC_CON9 [7:4]:
 * 0x0 = 8 bpc, 0x6 = 10 bpc.
 *
 * The TRM table for this field is unreliable: it renders the 8 bpc entry as
 * "4'h5: 0 bits per component", so its "Reserved" verdict on the other
 * encodings cannot be trusted either.  An earlier version of this file used
 * 0x5, reasoning from that table and from the MIPI DSI side of the same GRF
 * block, which does use 0x5 for 8 bpc.  That reasoning was wrong on two
 * counts and it cost a debugging cycle:
 *
 *   1. SOC_CON10 (dsihost_ipi_color_depth, what the DSI path uses) and
 *      SOC_CON8 (grf_hdmitx_iipi_color_depth, what the HDMI TX uses) are
 *      DIFFERENT fields feeding DIFFERENT consumers.  A value that works on
 *      one says nothing about the other.  The MIPI path never writes
 *      SOC_CON8 at all.
 *   2. For the same reason, the TRM's "Reserved" verdicts for this field
 *      carry no weight either.
 *
 * 0x0 is the encoding the HDMI datapath on this SoC accepts for 8 bpc.
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
 * set it, and that is the wrong reading of the field.  The TRM's own
 * application note (Part 2 12.6.3.4) says "Step2: GRF_VO0_CON09[9] = 1 ; // it
 * enable the hdmi controller data source come from ebc", pairing it with
 * CON13[4] as the two bits that route the EBC/VOP_LITE source into the HDMI
 * TX.
 *
 * This board drives the controller from the standard VOP2 path, so 1 selects a
 * dead source and leaves the interface clock at zero for ever.  The reset
 * value of the bit is 0, and 0 is what this driver writes.
 */

#define RK3576_HDMI_GRF_CH_SEL_VOP (1u << 9)

/* SOC_CON9 [7:4] grf_hdcp_i_tmds1_color_depth.
 *
 * The TRM's requirement is explicit -- "HDCP0 color depth, align with HDMITX
 * ipi color depth" -- so this field must always carry the SAME value as
 * SOC_CON8 [11:8].  The two encodings are therefore defined in terms of one
 * another below rather than written out separately.  They were separate once,
 * and when RK3576_HDMI_GRF_BPC_8 was corrected from 0x5 to 0x0, this field
 * silently stayed at 0x5 -- leaving the IPI depth and the far side's
 * expectation disagreeing, which is precisely the condition the TRM warns
 * about.  Tying them together makes that class of mistake impossible.
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
 * confirms the position of the field above.
 *
 * CAVEAT, recorded so this is not mistaken for a confirmed fix: the TRM
 * section concerns the VOP_LITE/EBC source.  Our DSI path works
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
 * so writing a 1 DISABLES.  This driver registers no handler, so the interrupt
 * is masked.
 */

#define RK3576_HDMI_IOC_HPD_INT_CLR (1u << 1)
#define RK3576_HDMI_IOC_HPD_INT_MSK (1u << 2)

/* MISC_CON1[15:0] hdmitx_hpd_con, the HPD debounce, decoded against TRM Part 2
 * 24.6.4.1:
 *
 *   [15:14] = 00    internal HPD taken from the glitch-free port (best)
 *   [13: 8] != 0    enable the low-level-keep filter
 *   [ 7: 0] = 0x02  low level must persist 2 ms to be accepted
 */

#define RK3576_HDMI_IOC_MISC_CON1_DEBOUNCE 0xffff0102u

/* HDMITX_HPD_STATUS (VCCIO6_IOC + 0x0440), documented in TRM Part 2 24.6.4.1
 * bit by bit.  It is NOT a single flag, but only one bit matters to this
 * driver:
 *
 *   [3]  internal HPD level -- the ONLY "is a sink attached" bit
 *
 * The remaining fields are a raw pad level [5], an internal port level [6], an
 * interrupt-active flag [4], a hold-time flag [7] and an edge counter [2:0].
 */

#define RK3576_HDMI_IOC_HPD_LEVEL (1u << 3)

/* ---------------------------------------------------------------------------
 * GPIO4C pad function select (VCCIO6_IOC_GPIO4C_IOMUX_SEL_L)
 * ---------------------------------------------------------------------------
 * The four HDMI SIDEBAND signals live on GPIO4C pads, whose function select
 * resets to 0 = GPIO.  Function 9 is the HDMI one for all four:
 *
 *   gpio4c0_sel [3:0]   -> HDMI_TX_CEC_M0
 *   gpio4c1_sel [7:4]   -> HDMI_TX_HPDIN_M0
 *   gpio4c2_sel [11:8]  -> HDMI_TX_SCL
 *   gpio4c3_sel [15:12] -> HDMI_TX_SDA
 *
 * The driver writes all four fields together from rk3576_hdmi_routing(), and
 * the value (0x9999) is the one the vendor's own Debian image leaves in this
 * register, so it is known-good for this board rather than a guess.
 *
 * MEASURED, and why the write is not optional: with it absent,
 * VCCIO6_IOC_HDMITX_HPD_STATUS reads 0x00000080 -- bit 7 only, bit 3 clear --
 * i.e. "no sink attached" with a monitor plugged in and terminating the link,
 * because the HPD input is not connected to its pad.
 *
 * The TMDS clock and data lanes are NOT here.  They are dedicated HDMI pins
 * and pass through no IOMUX, so this register has no bearing on whether a
 * picture appears -- only on whether HPD, DDC and CEC can be used.  It is a
 * prerequisite for HPD/EDID, not for the video path.
 *
 * NOTE: gpio4c0 (CEC) is muxed along with the rest even though nothing here
 * speaks CEC, because the whole 16-bit field is written as one known-good
 * value.  A board whose schematic puts something else on GPIO4_C0 would need
 * this narrowed to bits [15:4].
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

/* Time allowed between asserting and releasing a soft reset, and the settle
 * time after the last release.  A display that
 * fails to come up should be debugged in the PHY/GRF path first, not here.
 */

#define RK3576_HDMI_RST_PULSE_US 10

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_HDMI_H */
