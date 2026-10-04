/****************************************************************************
 * chips/rk3576/hardware/rk3576_vepu.h
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
 * RK3576 video encoder (VEPU510) hardware register definitions.
 *
 * References:
 *   - Rockchip RK3576 TRM Part 1: CRU (ch. 5), PMU (ch. 6), storage map.
 *   - Rockchip RK3576 TRM Part 2: encoder.
 *   - Register *field* layout: the Apache-2.0 licensed Rockchip MPP
 *     userspace library, vendored at <workspace>/rk3576-mpp-ref/:
 *       mpp/hal/rkenc/common/vepu510_common.h
 *       mpp/hal/rkenc/h264e/hal_h264e_vepu510_reg.h
 *
 * TRM reading note: the Part 1 PDF-to-text dump emits the bit-number
 * column and the description column as *separate* blocks, and the
 * description block of one register can run past the next register's
 * header.  Attributing a description to the wrong register is therefore
 * very easy.  Every bit position quoted below was cross-checked against a
 * register whose value is already known to work in this tree:
 *
 *   source register / bit      TRM says            existing code says
 *   -------------------------  ------------------  ----------------------
 *   PWR_GATE_CON1 bit 1        pd_vi_dwn_ena       rk3576_vicap.c 1<<1
 *   PWR_GATE_STS bit 17        pd_vi_dwn_stat      rk3576_vicap.c 1<<17
 *   BISR_INITRST_SFTCON1 b1    pd_vi_initrst       rk3576_vicap.c 1<<1
 *   BIU_IDLE_SFTCON0 bit 9     idle_req_vi         rk3576_vicap.c 1<<9
 *   SOFTRST_CON53 bit 7        aresetn_vicap       rk3576_vicap.c bit 7
 *
 * Four independent matches confirm both the register-to-bit mapping and
 * the high-to-low bit ordering used by the PMU tables.  Coverage of the
 * CRU tables for VEPU0 is additionally confirmed by the TRM's own table of
 * contents, which shows CRU_CLKSEL_CON125..127 do *not* exist -- so the
 * VEPU0 core/root selector really is inside CLKSEL_CON124.
 *
 * Note on the existing driver: rk3576_vicap.c writes PD_VI_DWN_SFTENA to
 * PWR_GATE_SFTCON0 (0x20210) bit 1.  Per the TRM that bit is
 * pd_bus_dwn_sftena; PD_VI's software power-down bit lives in
 * PWR_GATE_SFTCON1 (0x20214).  The write in question is harmless (it
 * clears a bit whose reset value is already 0, and "0 = do not power
 * down"), which is why the VICAP driver works.  The VEPU0 code below uses
 * the register the TRM actually specifies.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VEPU_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VEPU_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* -----------------------------------------------------------------------
 * VEPU510 register block offsets (from the MPP HAL, which pokes the same
 * IP from userspace on Linux).
 *
 * A frame is programmed by writing whole register groups, not individual
 * registers: the encoder expects one write per block, in ascending offset
 * order, ending with the status block.
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU510_CTL_OFFSET    0x0000 /* reg0    - reg72   */
#define RK3576_VEPU510_FRAME_OFFSET  0x0270 /* reg156  - reg253  */
#define RK3576_VEPU510_RC_ROI_OFFSET 0x1000 /* reg1024 - reg1091 */
#define RK3576_VEPU510_PARAM_OFFSET  0x1700 /* reg1472 - reg1651 */
#define RK3576_VEPU510_SQI_OFFSET    0x2000 /* reg2048 - reg2123 */
#define RK3576_VEPU510_SCL_OFFSET    0x2200 /* reg2176 - reg2401 */
#define RK3576_VEPU510_STATUS_OFFSET 0x4000 /* reg4096 - reg4243 */
#define RK3576_VEPU510_DBG_OFFSET    0x5000 /* reg5120 - reg5260 */

/* Hardware status register, inside the CTL block.  Also the interrupt status
 * register, which is the name it is defined under further down; both names
 * are kept because the bring-up probe reads it for a different reason than
 * the job path does.
 */

#define RK3576_VEPU510_HW_STATUS_OFFSET RK3576_VEPU510_INT_STA_OFFSET

/* Frame-start command register (ENC_STRT, reg4 = 0x0010).  Single-frame
 * ("register configuration") mode is lkt_num = 0, vepu_cmd = 1; the link
 * table mode the TRM describes is not used by this driver.
 */

#define RK3576_VEPU510_ENC_STRT_OFFSET   0x0010
#define RK3576_VEPU510_ENC_STRT_LKT_NUM  0x000000ff /* [7:0]   */
#define RK3576_VEPU510_ENC_STRT_VEPU_CMD 0x00000700 /* [10:8]  */

#define RK3576_VEPU510_ENC_CLR_OFFSET    0x0014
#define RK3576_VEPU510_ENC_CLR_SAFE_CLR  0x00000001 /* [0] */
#define RK3576_VEPU510_ENC_CLR_FORCE_CLR 0x00000002 /* [1] */

/* What the block reports in int_sta when it has finished the safe clear --
 * the same bit the vendor kernel polls after writing safe_clr, and the only
 * way to know whether the block accepted the request rather than ignoring it.
 */

#define RK3576_VEPU510_INT_SCLR_DONE     (1u << 2)

/* Interrupt registers, and the watchdog timeout threshold.  All four live
 * inside the CTL block and share one bit layout: bit n means the same event
 * in int_en, int_msk, int_clr and int_sta.
 *
 * These offsets are the hardware's, not an inference.  The vendor kernel
 * driver (rockchip-linux/kernel, drivers/video/rockchip/mpp/mpp_rkvenc2.c)
 * programs the same IP directly and publishes them as .int_en_base = 0x20,
 * .int_mask_base = 0x24, .int_clr_base = 0x28, .int_sta_base = 0x2c and
 * .enc_wdg_base = 0x38.  Note that the MPP HAL's own comments number these
 * registers differently (it calls ENC_STRT "reg001"); the offsets are what
 * agree with the generated struct layout, so the offsets win.
 */

#define RK3576_VEPU510_INT_EN_OFFSET  0x0020
#define RK3576_VEPU510_INT_MSK_OFFSET 0x0024
#define RK3576_VEPU510_INT_CLR_OFFSET 0x0028
#define RK3576_VEPU510_INT_STA_OFFSET 0x002c
#define RK3576_VEPU510_ENC_WDG_OFFSET 0x0038

/* int_sta / int_clr bit assignments.  Only the ones this driver acts on are
 * named; the rest are enabled and cleared as a group.
 */

#define RK3576_VEPU510_INT_ENC_DONE       (1u << 0)
#define RK3576_VEPU510_INT_LKT_NODE_DONE  (1u << 1)
#define RK3576_VEPU510_INT_SCLR_DONE      (1u << 2)
#define RK3576_VEPU510_INT_SLICE_DONE     (1u << 3)
#define RK3576_VEPU510_INT_ENC_ERR        (1u << 6)
#define RK3576_VEPU510_INT_VSRC_ERR       (1u << 7)
#define RK3576_VEPU510_INT_WDG            (1u << 8)
#define RK3576_VEPU510_INT_LKT_ERR_INT    (1u << 9)
#define RK3576_VEPU510_INT_LKT_ERR_STOP   (1u << 10)
#define RK3576_VEPU510_INT_LKT_FORCE_STOP (1u << 11)
#define RK3576_VEPU510_INT_DVBM_ERR       (1u << 15)

/* Every bit that means "the picture is wrong or the job did not complete",
 * as opposed to the benign completion notices.  A job ends when one of the
 * done bits appears; if any of these appears instead, the bitstream is not
 * to be trusted even if a length was latched.
 */

#define RK3576_VEPU510_INT_ERRORS                                        \
  (RK3576_VEPU510_INT_ENC_ERR | RK3576_VEPU510_INT_VSRC_ERR |            \
   RK3576_VEPU510_INT_WDG | RK3576_VEPU510_INT_LKT_ERR_INT |             \
   RK3576_VEPU510_INT_LKT_ERR_STOP | RK3576_VEPU510_INT_LKT_FORCE_STOP | \
   RK3576_VEPU510_INT_DVBM_ERR)

/* The bits that mean "the picture is finished".  A job is only complete when
 * one of these is set; anything else arriving is a job that has not finished,
 * and treating it as one would hand back a half-written bitstream.
 *
 * Both bits are listed because which one arrives is not something the driver
 * gets to choose.  ENC_DONE is the obvious one, but the vendor kernel is
 * explicit that VEPU510 cannot be relied on for it:
 *
 *   "There is a hw bug, the encoder has probabilistically encodes one frame
 *    repeatedly and does not return enc done in time.  So use slice info to
 *    check if the frame is encoded."
 *
 * which is why the kernel enables the slice-done interrupt for VEPU510 even
 * though the userspace HAL's register image leaves it clear.  On this IP
 * slice-done is the signal that actually arrives -- a measurement on the
 * first working job saw 0x8 and no enc-done at all -- so a driver waiting
 * only on ENC_DONE would time out on a picture that encoded perfectly.
 */

#define RK3576_VEPU510_INT_DONE \
  (RK3576_VEPU510_INT_ENC_DONE | RK3576_VEPU510_INT_SLICE_DONE)

/* -----------------------------------------------------------------------
 * Registers the vendor kernel patches in for VEPU510 specifically, on top
 * of the register table the userspace HAL supplies.
 *
 * These are not in the userspace HAL at all, so "our register image matches
 * MPP's" does not cover them: MPP's userspace cannot reach them, because its
 * modelled control block stops at 96 bytes (register 23) and 0x74 is outside
 * it.  They are applied by the kernel, in rkvenc2_start(), between the table
 * write and the start command.  Since this driver replaces both halves, it
 * has to do them itself.
 *
 * 0x300 (enc_pic) is the exception that is *both*: the HAL models it and the
 * kernel ors bits into it afterwards.  Its two bits are written from the
 * value the HAL produced, which is why the offset is named here rather than
 * the whole register being re-derived.
 * -----------------------------------------------------------------------
 */

/* 0x74 = reg29: the kernel calls this a dvbm special register and writes a
 * constant, to make the IP hold the expected state when encoding finishes.
 */

#define RK3576_VEPU510_DVBM_HOLD_OFFSET 0x0074
#define RK3576_VEPU510_DVBM_HOLD_VALUE  0x00000023

/* 0x308 = reg194: also a constant, written together with the above. */

#define RK3576_VEPU510_DVBM_STATE_OFFSET 0x0308
#define RK3576_VEPU510_DVBM_STATE_VALUE  0x00050000 /* BIT(18) | BIT(16) */

/* 0x5300: the kernel writes 2 here before every job and calls it a hardware
 * counter clear.  It is the one register the kernel writes that this driver
 * did not, and it is written on every job here for that reason.
 *
 * It is above every register class the HAL models -- the status block ends at
 * 0x40a4 and the debug class at 0x5230 -- and it is in none of MPP's tables
 * either, so nothing in the register image can supply it and no diff of the
 * image can see it missing.  That is why it survived the register-by-register
 * comparison that cleared everything else.
 *
 * Nothing here needs a counter cleared, and a run with and a run without it
 * produced the same stream to within the measurement: it is a counter clear.
 * It is written anyway, because it is what the platform does before a job and
 * because being outside the register image means agreeing with the kernel
 * about it costs one write and disagreeing is invisible until it is not.
 */

#define RK3576_VEPU510_CLR_COUNTER_OFFSET 0x5300
#define RK3576_VEPU510_CLR_COUNTER_VALUE  0x00000002

/* 0x300 = reg192 (enc_pic).  Bit 30 is set for every VEPU510 job.  Bit 31
 * selects the reconstruction-frame compression behaviour and is set only
 * when FBC is disabled -- and only with the core clock stopped, because
 * writing it while the clock runs makes the DMA module trigger a write of
 * its own.  That silicon workaround is why this is not a plain read-modify-
 * write.
 */

#define RK3576_VEPU510_ENC_PIC_OFFSET 0x0300
#define RK3576_VEPU510_ENC_PIC_BIT30  (1u << 30)
#define RK3576_VEPU510_ENC_PIC_BIT31  (1u << 31)

/* Bit 3 of int_en (slice-done) is enabled by the kernel for VEPU510 even
 * though the userspace HAL leaves it clear.  It is the same bit the HAL
 * calls int_en.vslc_done_en.
 */

#define RK3576_VEPU510_INT_SLICE_DONE_EN RK3576_VEPU510_INT_SLICE_DONE

/* -----------------------------------------------------------------------
 * The status block's raw word offsets live in vepu/vepu510_regs.h, with the
 * code that decodes them.  They are words rather than modelled fields --
 * both figures in question straddle two registers -- so they belong with the
 * part of the tree that knows what the values mean, not here with the
 * addresses and block offsets.
 * -----------------------------------------------------------------------
 */

/* -----------------------------------------------------------------------
 * Slice FIFO, at the end of the status block.
 *
 * The encoder does not merely write a bitstream and stop: it pushes one
 * length record per slice into a FIFO and raises an interrupt.  Reading the
 * FIFO is how the driver learns how long the frame is, and the record's top
 * bit is what says the frame is finished.
 *
 * This is not an optional detail.  The vendor kernel is explicit that
 * VEPU510's own enc-done status cannot be relied on -- it can re-encode a
 * frame and never raise it -- and uses these records instead:
 *
 *   "There is a hw bug, the encoder has probabilistically encodes one frame
 *    repeatedly and does not return enc done in time.  So use slice info to
 *    check if the frame is encoded."
 *
 * A reading of the FIFO is therefore what completion means, and the lengths
 * in it are the frame's bitstream length without qualification.  The status
 * block's own length word is a convenience that is not always populated.
 *
 * The kernel also warns that the interrupt is raised by the FIFO being
 * non-empty rather than by a slice being added, so the interrupt must not be
 * cleared before the FIFO has been drained -- an entry left behind raises a
 * second interrupt for a frame that has already been dealt with.
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU510_SLICE_NUM_OFFSET 0x4034
#define RK3576_VEPU510_SLICE_LEN_OFFSET 0x4038

/* How many length records the FIFO can hold.  Only meaningful as an upper
 * bound: a record read from an empty FIFO would be a fabricated length, so
 * draining stops at whichever of this and the reported count is smaller.
 */

#define RK3576_VEPU510_SLICE_FIFO_LEN 8

#define RK3576_VEPU510_SLICE_NUM_MASK 0x3fu
#define RK3576_VEPU510_SLICE_LEN_MASK 0x7fffffffu
#define RK3576_VEPU510_SLICE_LAST     (1u << 31)

/* -----------------------------------------------------------------------
 * Version register (VEPU510 CTL block, offset 0x0000, read-only).
 *
 * This is the cheapest bring-up probe on the whole IP: it reads back as 0
 * while the power domain is still held in its initial reset, and as
 * 0xffffffff if the APB/AXI clock is not running.  A sane value with
 * h264_cap set means power, clocks and resets are all correct.
 *
 * Bit layout mirrors Vepu510ControlCfg.version in vepu510_common.h.
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU510_VERSION_OFFSET     0x0000

#define RK3576_VEPU510_VER_SUB_VER_MASK   0x000000ff /* [7:0]   */
#define RK3576_VEPU510_VER_SUB_VER_SHFT   0
#define RK3576_VEPU510_VER_H264_CAP       (1u << 8)  /* [8]     */
#define RK3576_VEPU510_VER_HEVC_CAP       (1u << 9)  /* [9]     */
#define RK3576_VEPU510_VER_RES_CAP_MASK   0x0000f000 /* [15:12] */
#define RK3576_VEPU510_VER_RES_CAP_SHFT   12
#define RK3576_VEPU510_VER_OSD_CAP_MASK   0x00030000 /* [17:16] */
#define RK3576_VEPU510_VER_OSD_CAP_SHFT   16
#define RK3576_VEPU510_VER_FILTR_CAP_MASK 0x000c0000 /* [19:18] */
#define RK3576_VEPU510_VER_FILTR_CAP_SHFT 18
#define RK3576_VEPU510_VER_BFRM_CAP       (1u << 20) /* [20]    */
#define RK3576_VEPU510_VER_FBC_CAP_MASK   0x00600000 /* [22:21] */
#define RK3576_VEPU510_VER_FBC_CAP_SHFT   21
#define RK3576_VEPU510_VER_IP_ID_MASK     0xff000000 /* [31:24] */
#define RK3576_VEPU510_VER_IP_ID_SHFT     24

/* -----------------------------------------------------------------------
 * CRU clock selectors for VEPU0 -- CLKSEL_CON124 (CRU + 0x04F0).
 *
 * VEPU1 does NOT live next door: its selectors are in CLKSEL_CON178
 * (hclk root) and CLKSEL_CON180 (core + aclk root), so they cannot be
 * derived by adding one to the VEPU0 register number.
 *
 *   [15:13] clk_vepu0_core_sel     0 gpll / 1 cpll / 2 spll / 3 lpll /
 *                                  4 bpll        (reset: 2 = spll)
 *   [12:8]  clk_vepu0_core_div     divide by (div_con + 1)
 *   [7]     aclk_vepu0_root_sel    0 gpll / 1 cpll   (reset: 0)
 *   [6:2]   aclk_vepu0_root_div    divide by (div_con + 1)  (reset: 2)
 *   [1:0]   hclk_vepu0_root_sel    0 gpll_div6 / 1 cpll_div10 /
 *                                  2 cpll_div20 / 3 xin_osc0  (reset: 0)
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU_CRU_CLKSEL_CON      124

#define RK3576_VEPU_CORE_SEL_SHIFT      13
#define RK3576_VEPU_CORE_SEL_MASK       0x7

#define RK3576_VEPU_CORE_DIV_SHIFT      8
#define RK3576_VEPU_CORE_DIV_WIDTH      5

#define RK3576_VEPU_ACLK_ROOT_SEL_SHIFT 7
#define RK3576_VEPU_ACLK_ROOT_SEL_MASK  0x1

#define RK3576_VEPU_ACLK_ROOT_DIV_SHIFT 2
#define RK3576_VEPU_ACLK_ROOT_DIV_WIDTH 5

#define RK3576_VEPU_HCLK_ROOT_SEL_SHIFT 0
#define RK3576_VEPU_HCLK_ROOT_SEL_MASK  0x3

/* Parent selector encodings (for the two documented selectors). */

#define RK3576_VEPU_ROOT_SEL_GPLL       0
#define RK3576_VEPU_ROOT_SEL_CPLL       1

#define RK3576_VEPU_HCLK_SEL_GPLL_DIV6  0
#define RK3576_VEPU_HCLK_SEL_CPLL_DIV10 1
#define RK3576_VEPU_HCLK_SEL_CPLL_DIV20 2
#define RK3576_VEPU_HCLK_SEL_XIN_OSC0   3

/* -----------------------------------------------------------------------
 * CRU clock gates for VEPU0 -- GATE_CON51 (CRU + 0x08CC), hiword-masked.
 *
 * "When high, disable clock", so enabling means writing 0.  All of these
 * reset to 0, i.e. the clocks are ungated out of reset; the driver still
 * enables them through the CLK framework so the tree's accounting matches
 * the hardware.
 *
 * Beware: GATE_CON50 is JPEG and GATE_CON53 is VPSS/ISP0 (that is where
 * VICAP's gates live).  GATE_CON51 is VEPU0 -- reaching for CON53 here
 * would silently leave VEPU0 gated while appearing to succeed.
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU_CRU_GATE_CON      51

#define RK3576_VEPU_HCLK0_ROOT_EN_BIT 0
#define RK3576_VEPU_ACLK0_ROOT_EN_BIT 1
#define RK3576_VEPU_HCLK0_BIU_EN_BIT  2
#define RK3576_VEPU_ACLK0_BIU_EN_BIT  3
#define RK3576_VEPU_HCLK0_EN_BIT      4
#define RK3576_VEPU_ACLK0_EN_BIT      5
#define RK3576_VEPU_CORE_CLK_EN_BIT   6

#define RK3576_VEPU_GATE_BITS                                                \
  ((1u << RK3576_VEPU_CORE_CLK_EN_BIT) | (1u << RK3576_VEPU_ACLK0_EN_BIT) |  \
   (1u << RK3576_VEPU_HCLK0_EN_BIT) | (1u << RK3576_VEPU_ACLK0_BIU_EN_BIT) | \
   (1u << RK3576_VEPU_HCLK0_BIU_EN_BIT) |                                    \
   (1u << RK3576_VEPU_ACLK0_ROOT_EN_BIT) |                                   \
   (1u << RK3576_VEPU_HCLK0_ROOT_EN_BIT))

/* -----------------------------------------------------------------------
 * CRU software resets for VEPU0 -- SOFTRST_CON51 (CRU + 0x0ACC).
 *
 * "When high, reset relative logic", so releasing means writing 0 through
 * the hiword mask.  Same shape as the capture path: only the release side
 * is written, because nothing here is running yet and the assert side has
 * no purpose in this driver.
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU_CRU_SOFTRST_CON  51

#define RK3576_VEPU_H0RESETN_BIU_BIT 2
#define RK3576_VEPU_A0RESETN_BIU_BIT 3
#define RK3576_VEPU_HRESETN_BIT      4
#define RK3576_VEPU_ARESETN_BIT      5
#define RK3576_VEPU_CORE_RESETN_BIT  6

#define RK3576_VEPU_RESET_BITS                                              \
  ((1u << RK3576_VEPU_CORE_RESETN_BIT) | (1u << RK3576_VEPU_ARESETN_BIT) |  \
   (1u << RK3576_VEPU_HRESETN_BIT) | (1u << RK3576_VEPU_A0RESETN_BIU_BIT) | \
   (1u << RK3576_VEPU_H0RESETN_BIU_BIT))

/* -----------------------------------------------------------------------
 * PMU control for the VEPU power domains (PMU at RK3576_PMU_ADDR).
 *
 * PD_VEPU0 and PD_VEPU1 are separate domains.  Each has
 *
 *   - a hardware power-down request      PWR_GATE_CON1
 *   - a software power-down request      PWR_GATE_SFTCON1
 *   - a power state readback             PWR_GATE_STS
 *   - a memory-repair initial reset      BISR_INITRST_SFTCON1
 *   - a bus-interface "go idle" request  BIU_IDLE_SFTCON0
 *
 * The initial reset is the one that bites: it resets to 0, i.e. HELD IN
 * RESET, and until it is released every register in the domain reads back
 * as zero -- indistinguishable from a clock that never came up.  The
 * capture driver documents the same trap for PD_VI.
 *
 * The same layout is mirrored across the CON/SFTCON/STS/BISR/BIU tables,
 * with VEPU1 one bit above VEPU0 everywhere:
 *
 *         [9]gpu [8]npu1 [7]npu0 [6]nputop [5]vpu [4]vdec
 *         [3]vepu1 [2]vepu0 [1]vi [0]usb
 *
 * Writes use the hiword write-enable scheme: bits [31:16] select which of
 * the low 16 bits are written.
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU_PMU_PWR_GATE_CON1_OFF        0x20204
#define RK3576_VEPU_PMU_PWR_GATE_SFTCON1_OFF     0x20214
#define RK3576_VEPU_PMU_PWR_GATE_STS_OFF         0x20230
#define RK3576_VEPU_PMU_BISR_INITRST_SFTCON1_OFF 0x20544
#define RK3576_VEPU_PMU_BIU_IDLE_SFTCON0_OFF     0x20110

#define RK3576_VEPU_PD0_DWN_ENA_BIT              (1u << 2) /* 0 = powered */
#define RK3576_VEPU_PD1_DWN_ENA_BIT              (1u << 3)
#define RK3576_VEPU_PD0_DWN_SFTENA_BIT           (1u << 2) /* 0 = powered */
#define RK3576_VEPU_PD1_DWN_SFTENA_BIT           (1u << 3)
#define RK3576_VEPU_PD0_DWN_STAT_BIT             (1u << 18) /* 0 = up      */
#define RK3576_VEPU_PD1_DWN_STAT_BIT             (1u << 19)
#define RK3576_VEPU_PD0_INITRST_BIT              (1u << 2) /* 1 = normal  */
#define RK3576_VEPU_PD1_INITRST_BIT              (1u << 3)
#define RK3576_VEPU_BIU0_IDLE_REQ_BIT            (1u << 7) /* 0 = active  */
#define RK3576_VEPU_BIU1_IDLE_REQ_BIT            (1u << 8)

#define RK3576_VEPU_PMU_HWM(bits)                ((bits) << 16)

/* Bounded poll for the domain to report powered up.  The PMU status bit
 * settles in a few microseconds; the bound only exists so a mis-clocked or
 * unpowered part fails loudly instead of hanging the boot.
 */

#define RK3576_VEPU_PMU_POLL_LOOPS 1000000

/* -----------------------------------------------------------------------
 * Clock rate targets, from the vendor device tree
 * (rockchip-linux/kernel, develop-6.1, arch/arm64/boot/dts/rockchip/
 * rk3576.dtsi, node rkvenc0@27a00000):
 *
 *   rockchip,normal-rates  = <400000000>, <0>, <702000000>;
 *   assigned-clock-rates   = <400000000>, <702000000>;
 *
 * i.e. ACLK_VEPU0 = 400 MHz and CLK_VEPU0_CORE = 702 MHz.  The same node
 * also pins the register window to 0x6000, places the MMU at 0x27A0F000
 * and lists resets "video_a"/"video_h"/"video_core" -- and its
 * GIC_SPI 310/311 map to hwirq 342/343, which is exactly what irq.h
 * already defines for VEPU0/VEPU0_MMU.
 *
 * These are the targets the driver programs: aclk at 400 MHz and the core
 * clock at 702 MHz.  Neither is a ceiling the part imposes -- they are the
 * vendor's chosen operating point -- so the driver takes the closest rate
 * each source can reach without going over, and reports what it got rather
 * than insisting on these two figures.  The rates are measured from the
 * PLLs at run time instead of assumed here, because the bootloader owns the
 * PLL configuration; see rk3576_vepu_select_source().
 * -----------------------------------------------------------------------
 */

#define RK3576_VEPU_ACLK_HZ 400000000
#define RK3576_VEPU_CORE_HZ 702000000

/* Register window actually decoded by the VEPU0 instance (the vendor node
 * maps 0x6000 bytes, not the whole 64 KB slot).
 */

#define RK3576_VEPU0_WINDOW 0x6000

/* MMU (IOMMU v2) in front of VEPU0's AXI masters.
 *
 * This driver takes the pass-through route, exactly as rk3576_vicap.c does:
 * every buffer it hands the encoder is already physically contiguous, so
 * there is nothing to translate and paging is left disabled.  The MMU is
 * touched only to make that state explicit -- DTE_ADDR cleared, interrupts
 * masked -- rather than to be relied on.
 *
 * The block sits at its own address rather than inside the encoder's window,
 * and its registers use the standard IOMMU v2 layout.  rk3576_vicap.h
 * publishes the same four registers for VI's MMU at 0x0800/0x0804/0x0818/
 * 0x081c, which is the same 0x00/0x04/0x18/0x1c pattern one base higher.
 */

#define RK3576_VEPU0_MMU_OFFSET   0x0000f000

#define RK3576_VEPU0_MMU_ADDR     (RK3576_VEPU0_ADDR + RK3576_VEPU0_MMU_OFFSET)

#define RK3576_VEPU_MMU_DTE_ADDR  0x0000
#define RK3576_VEPU_MMU_STATUS    0x0004
#define RK3576_VEPU_MMU_INT_CLEAR 0x0018
#define RK3576_VEPU_MMU_INT_MASK  0x001c

/* status[0] tells whether translation is on.  It must read clear: the
 * encoder's addresses are physical, so a translation unit that believed it
 * had a page table would fault on every access. */

#define RK3576_VEPU_MMU_STATUS_PAGING_ENABLED (1u << 0)

/* Only two events exist; masking both is the whole job. */

#define RK3576_VEPU_MMU_INT_ALL 0x3

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VEPU_H */
