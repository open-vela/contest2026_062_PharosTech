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

#define RK3576_VOP_CFG_DONE_LOAD_CTRL                               \
  (0xffff << 16)                             /* hiword write-enable \
                                              */
#define RK3576_VOP_CFG_DONE_VP0     (1 << 0) /* VP0 cfg done */
#define RK3576_VOP_CFG_DONE_VP0_SYS (1 << 4) /* VP0 sys cfg done */
#define RK3576_VOP_CFG_DONE_TRIGGER                          \
  (RK3576_VOP_CFG_DONE_LOAD_CTRL | RK3576_VOP_CFG_DONE_VP0 | \
   RK3576_VOP_CFG_DONE_VP0_SYS)

/* SYS_REG_CFG_DONE (0x0000) load-enable bits (mirror -> real).
 *
 * *** THE NAMES BELOW ARE THE TRM'S OWN, AND THEY MAP ONE-FOR-ONE ONTO LINUX'S
 * RK3576 VIDEO-PORT TABLE.  THE OLD COMMENTS HERE HAD THEM WRONG. ***
 *
 *   TRM, SYS_CTRL_SYS_REG_CFG_DONE          Linux, rk3576 VP entries
 *   ------------------------------          -----------------------
 *   bit15 sw_global_regdone_en  (reset 1)   RK3568_VOP2_GLB_CFG_DONE_EN
 *   bit14 reg_load_wb_en                    RK3568_VOP2_WB_CFG_DONE
 *   bit 6 reg_load_sys2_en                  .sys_cfg_done shift 6  (VP2)
 *   bit 5 reg_load_sys1_en                  .sys_cfg_done shift 5  (VP1)
 *   bit 4 reg_load_sys0_en                  .sys_cfg_done shift 4  (VP0)
 *   bit 2 reg_load_vp2_en                   .cfg_done     bit 2    (VP2)
 *   bit 1 reg_load_vp1_en                   .cfg_done     bit 1    (VP1)
 *   bit 0 reg_load_vp0_en                   .cfg_done     bit 0    (VP0)
 *
 * LINUX'S RK3576 COMMIT WRITES ONLY THE BITS FOR THE VIDEO PORT IT IS
 * COMMITTING.  rk3588_vop2_cfg_done():
 *
 *   val = RK3568_VOP2_GLB_CFG_DONE_EN | RK3568_VOP2_WB_CFG_DONE |
 *         (RK3568_VOP2_WB_CFG_DONE << 16) | BIT(sys_cfg_done_shift) |
 *         (BIT(sys_cfg_done_shift) << 16);
 *   vop2_writel(vop2, 0, val);
 *
 * i.e. bit15 (unmasked) + bit14 + bit4, with hiword write-enables.  Nothing
 * else.  This driver instead wrote EVERY group, including bits 1/2/5/6 --
 * VP1's and VP2's load requests.  VP1 and VP2 are not running on this board,
 * so those four bits can never be consumed and STAY SET FOR EVER, which is
 * visible in this driver's own register dump:
 *
 *   SYS_REG_CFG_DONE here = 0x00008066   (bits 1,2,5,6 still pending)
 *   SYS_REG_CFG_DONE in a dump from a known-working configuration = 0x00000000
 *
 * and the TRM says of each of these bits that the mirror->real copy happens
 * when ALL the requested register configuration is finished.  A commit that
 * is permanently waiting on two video ports that will never scan is a commit
 * that may never complete -- and the one experiment that tested the window
 * group directly (shrink the layer to 200x200 and watch for the corner) found
 * that the WINDOW GROUP NEVER LOADS: readback perfect, glass unchanged.
 *
 * So the commit now asks for exactly what Linux asks for, and nothing more.
 */

#define RK3576_VOP_CFG_DONE_ALL_GROUPS \
  ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 6))

/* *** NOT USED FOR THE COMMIT ANY MORE. ***  Kept only so the mistake above
 * stays legible: writing these bits requests VP1's and VP2's loads too, and
 * on this board nothing will ever consume them.  See the block comment. */

#define RK3576_VOP_CFG_DONE_VP0_GROUPS ((1 << 0) | (1 << 4))

/* SYS_REG_CFG_DONE (0x0000) bit 14 = reg_load_wb_en.  Linux's
 * RK3568_VOP2_WB_CFG_DONE, which its RK3576 commit always sets. */

#define RK3576_VOP_CFG_DONE_WB_LOAD (1 << 14)

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

/* Every window-group load bit.  Bit names from the TRM's list, which runs
 * downwards: reg_load_esmart3_en (bit7), esmart2 (6), esmart1 (5),
 * esmart0 (4), cluster1 (1), cluster0 (0); bits 3:2 are RO reserved. */

#define RK3576_VOP_WIN_CFG_DONE_ALL \
  ((1 << 0) | (1 << 1) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7))

/* Every window-group load bit, for the case where a probe wants to be sure the
 * whole group is requested rather than betting on one bit:
 *   bit0 cluster0  bit1 cluster1  bit4 esmart0  bit5 esmart1  bit6 esmart2
 *   bit7 esmart3   (bits 3:2 are RO reserved)
 * Names taken from the TRM's bit list, which runs downwards through
 * reg_load_esmart3_en (bit7) ... reg_load_cluster0_en (bit0).
 */

#define RK3576_VOP_WIN_CFG_DONE_ALL \
  ((1 << 0) | (1 << 1) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7))

/* POST0_CTRL_POST_CFG_DONE  @  VOP + 0x0CFC  (POST0 base 0x0C00 + 0x0FC).
 *
 * *** THE RK3572+ FRAME COMMIT -- AND THIS DRIVER HAS NEVER WRITTEN IT. ***
 *
 * Rockchip's vop2 driver commits a frame differently by generation; see
 * rk3588_vop2_cfg_done():
 *
 *   if (version < VOP_VERSION_RK3572)        -> SYS_WIN_REG_CFG_DONE ...
 *   else if (version >= VOP_VERSION_RK3572)  -> VOP_MODULE_SET(vop2, vp,
 *                                                 cfg_done, 1);
 *
 * and for the RK3572/RK3576 video ports that module register is
 *
 *   .cfg_done = VOP_REG_MASK(RK3572_VP0_POST_CFG_DONE, 0x1, 0),
 *   #define RK3572_VP0_POST_CFG_DONE            0xCFC
 *
 * i.e. bit0 of POST0+0x0FC with a hiword write-enable, so the word to write is
 * 0x00010001 and NOT 0x00000001.  Note what is ABSENT from the RK3572+
 * branch: it does not write SYS_WIN_REG_CFG_DONE (0x000C) at all -- and that
 * is the only load trigger this driver has ever used for the layer group.  On
 * this generation the 0x000C register looks like a legacy one: it is writable
 * and its bits are consumed at a frame boundary, which is exactly what made it
 * look like a working load request, but Linux does not use it here.
 *
 * It fits the evidence better than anything else tested: every ESMART
 * register reads back in the MIRROR exactly as programmed, yet changing
 * anything inside the ESMART block changes nothing on screen while changing
 * the SYS block does -- and POST_BUF_EMPTY sets only with the layer routed on.
 * A window whose configuration never leaves the mirror is routed by the mixer
 * (so it visibly displaces the background) but never fetches.
 *
 * VP1/VP2 follow at 0x0DFC/0x0EFC by the POST1/POST2 block stride; this board
 * only uses POST0.
 */

#define RK3576_VOP_POST_CFG_DONE_OFF 0x0FC
#define RK3576_VOP_POST_CFG_DONE_EN  (1 << 0)
#define RK3576_VOP_POST_CFG_DONE_TRIGGER \
  (RK3576_VOP_POST_CFG_DONE_EN | (RK3576_VOP_POST_CFG_DONE_EN << 16))

/* POST0_CTRL_POST_CLK_CNT  @  POST0 + 0x0F4  (VOP + 0x0CF4).
 *
 * *** A HARDWARE CLOCK COUNTER -- THE ONLY WAY TO MEASURE WHETHER THE VOP'S
 * DATAPATH CLOCK IS ACTUALLY RUNNING. ***
 *
 * TRM:
 *
 *   bit15    calc_clk_en     RW  reset 0   1'b0: Disable  1'b1: Enable
 *   [31:16]  calc_aclk_cnt   RO  "the count number of aclk, when the count
 *                                 number of hclk is 5000"
 *   [14:0]   calc_dclk_cnt   RO  the count number of dclk over the same window
 *
 * So setting calc_clk_en makes the hardware count aclk and dclk cycles during
 * a FIXED 5000-hclk window and latch both, which turns "is the datapath clock
 * alive?" from an inference into a reading.  The ratio aclk/dclk is directly
 * comparable because both are counted over the same window, and dclk is a
 * built-in control: the POST's timing generator demonstrably runs (forced
 * black, forced zero and the solid background all reach the glass), so dclk
 * MUST count. If dclk counts and aclk does not, the datapath clock is stopped
 * -- which is the long-standing hypothesis in this file:
 *
 *   "the register file sits on the peripheral clock, so it answers reads and
 *    retains writes while the datapath's aclk is gated off; the request
 *    generator then simply never runs, and a clock that is gated produces no
 *    error because nothing happens."
 *
 * It fits every stubborn observation at once: every register reads back
 * correct (hclk/pclk alive), no bus error and no MMU fault are possible (no
 * read is ever issued), and the POST's input buffer under-runs the moment the
 * layer is enabled (the mixer waits forever for data that cannot arrive).
 *
 * The Linux driver exposes these as .calc_clk_en / .calc_aclk_cnt /
 * .calc_dclk_cnt on RK3576's video port 0, confirming the field layout.
 */

#define RK3576_VOP_POST_CLK_CNT_OFF    0x0F4
#define RK3576_VOP_POST_CLK_EN         (1u << 15)
#define RK3576_VOP_POST_ACLK_CNT_SHIFT 16
#define RK3576_VOP_POST_ACLK_CNT_MASK \
  (0xffffu << RK3576_VOP_POST_ACLK_CNT_SHIFT)
#define RK3576_VOP_POST_DCLK_CNT_MASK 0x7fffu

/* SYS_CTRL status / vsync registers (read-only diagnostics).
 *   SYS_CTRL_SYS_STATUS0 @ 0x0060: dsp_vcnt0[28:16] = video output0
 *     vertical counter.  Readbacks 0 when the VP0 scan state machine is
 *     not running; it increments every line when the frame is scanning.
 *   SYS_CTRL_VOP_IO_VSYNC_CTRL @ 0x004C: vsync-to-IO routing sel
 *     (vop_io_vp0_vsync_sel[1:0]).
 */

#define RK3576_VOP_SYS_STATUS0 0x0060

/* SYS_CTRL_SYS_STATUS0/1/2  @  0x0060 / 0x0064 / 0x0068.
 *
 * *** TWO STATUS BITS PER VIDEO PORT THAT THIS DRIVER HAS NEVER READ. ***
 *
 * TRM:
 *
 *   [28:16] RO  dsp_vcntN        "Read the video outputN vertical counter."
 *   1       RO  mmu_idle         "MMU idle status."
 *   0       RO  dma_stop_validN  "AXI dma stop status."
 *
 * dma_stop_validN IS A STATUS, NOT A CONTROL.  This driver has only ever
 * looked at the dma_stop bit inside SYS_AXI0_CTRL_IMD -- the bit it WRITES --
 * and a status field exists precisely because what the channel is actually
 * doing can differ from what the control register holds.  A channel that
 * really is stopped cannot issue a read, and a stopped channel raises no
 * error: that is exactly this board's signature -- the window configured
 * correctly, the mixer visibly waiting for it (the POST under-runs the moment
 * the layer is enabled), and yet no read, no bus error and no MMU fault.
 *
 * (The TRM's field names and bit numbers come out in separate blocks for this
 * register; the two remaining bits after [28:16] and [15:2] are 1 and 0, and
 * the names are listed in descending bit order, so mmu_idle = bit1 and
 * dma_stop_validN = bit0.  BOTH are printed regardless of that ordering, so a
 * mistake there cannot hide the answer.)
 *
 * dsp_vcntN is the VP's vertical counter.  Sampling it repeatedly is the check
 * on whether the video port's scan state machine is actually running -- it
 * increments every line while a frame is being scanned, and sits still if not.
 */

#define RK3576_VOP_SYS_STATUS0           0x0060
#define RK3576_VOP_SYS_STATUS1           0x0064
#define RK3576_VOP_SYS_STATUS2           0x0068
#define RK3576_VOP_SYS_STATUS_VCNT_SHIFT 16
#define RK3576_VOP_SYS_STATUS_VCNT_MASK \
  (0x1fffu << RK3576_VOP_SYS_STATUS_VCNT_SHIFT)
#define RK3576_VOP_SYS_STATUS_MMU_IDLE (1u << 1)
#define RK3576_VOP_SYS_STATUS_DMA_STOP (1u << 0)

/* SYS_CTRL interrupt registers (offsets relative to SYS_CTRL).
 *
 * These are the VOP's own error/event reporting and the driver never looked at
 * them before.  The two worth having:
 *
 *   VOP2_INT_BUS_ERRPR -- the window's AXI read failed.  A framebuffer read
 *        that errors out would leave the POST compositing nothing, i.e. a
 *        perfectly valid but BLACK pixel stream.  Nothing else in the driver
 *        would notice: the DSI still transmits, the panel sees clean packets
 *        and reports no errors, and the screen stays dark.
 *
 *   VP_INT_POST_BUF_EMPTY -- the POST's output buffer underran.  That is the
 *        hardware saying "the pixels to be transmitted did not arrive in
 *        time", which is the missing positive evidence for "the stream is
 *        being sent but carries no real pixel data".
 *
 * The RAW_STATUS register is used for the per-VP events because it is NOT
 * masked by the interrupt-enable bits, so it reports what happened even though
 * this driver registers no IRQ handler.
 */

#define RK3576_VOP_SYS0_INT_EN  0x0080 /* AXI bus 0 interrupt enable */
#define RK3576_VOP_SYS0_INT_CLR 0x0084 /* AXI bus 0 interrupt clear */
#define RK3576_VOP_SYS0_INT_STATUS                                           \
  0x0088                                  /* AXI bus 0 status (MASKED by EN) \
                                           */
#define RK3576_VOP_SYS0_INT_RAW    0x008C /* AXI bus 0 raw status (unmasked) */
#define RK3576_VOP_SYS1_INT_EN     0x0090
#define RK3576_VOP_SYS1_INT_CLR    0x0094
#define RK3576_VOP_SYS1_INT_STATUS 0x0098 /* MASKED by EN */
#define RK3576_VOP_SYS1_INT_RAW    0x009C /* unmasked */

/* *** READ THE RAW REGISTER, NOT THE STATUS REGISTER. ***
 *
 * The TRM gives four separate registers per AXI channel (SYS_CTRL offsets
 * 0x0080/0x0084/0x0088/0x008C for SYS0, 0x0090/0x0094/0x0098/0x009C for SYS1):
 * enable, clear, STATUS and RAW_STATUS.  STATUS is masked by the enable bits,
 * and this driver never sets them, so STATUS reads 0 no matter what happens on
 * the bus -- while RAW_STATUS reports the same events unmasked.
 *
 * Every earlier "no bus error" reading in this bring-up came from the MASKED
 * register, which cannot show anything at all.  In particular the
 * poison-address test -- which was built to answer "does the layer issue
 * reads?" and answered "no" -- was measuring a register that could not have
 * said otherwise.  The per-VP interrupts were already read from their RAW
 * register for exactly this reason (see RK3576_VOP_VP_INT_RAW_STATUS); the SYS
 * pair was overlooked. */
#define RK3576_VOP_VP_INT_CLR(vp)        (0x00A4 + (vp)*0x10)
#define RK3576_VOP_VP_INT_STATUS(vp)     (0x00A8 + (vp)*0x10)
#define RK3576_VOP_VP_INT_RAW_STATUS(vp) (0x00AC + (vp)*0x10)

/* Interrupt bits -- TWO DIFFERENT TABLES THAT MUST NOT BE CONFLATED.
 *
 * Getting this wrong produced a false alarm in this project ("the VOP reports
 * an AXI read error") and, worse, made a real output-buffer under-run look
 * like a harmless per-line event and get dismissed for many rounds.
 *
 * The bit ORDER comes from the reference driver's tables in
 * rockchip_vop2_reg.c:
 *
 *   per-VP (POST) status           SYS0/SYS1 (AXI) status
 *     bit0 FS                        bit0 - (unused)
 *     bit1 FS_NEW   <- NOT a bus err bit1 BUS_ERROR
 *     bit2 LINE_FLAG                 bit2 - (unused)
 *     bit3 LINE_FLAG1                bit3 WB_UV_FIFO_FULL
 *     bit4 POST_BUF_EMPTY            bit4 WB_YRGB_FIFO_FULL
 *     bit5 FS_FIELD                  bit5 WB_COMPLETE
 *     bit6 DSP_HOLD_VALID            bit6 - (unused)
 *                                    bit7 MMU_EN
 *
 * i.e. the bus error lives ONLY in the SYS registers, and the under-run ONLY
 * in the per-VP register, at the same numeric position in two different
 * registers. */

/* Per-VP (POST) interrupt bits. */

#define RK3576_VOP_INT_FS             (1 << 0) /* frame start */
#define RK3576_VOP_VP_INT_FS_NEW      (1 << 1) /* frame start (new) */
#define RK3576_VOP_INT_LINE_FLAG0     (1 << 2)
#define RK3576_VOP_INT_LINE_FLAG1     (1 << 3)
#define RK3576_VOP_INT_POST_BUF_EMPTY (1 << 4) /* output buffer under-ran */
#define RK3576_VOP_INT_FS_FIELD       (1 << 5)
#define RK3576_VOP_INT_DSP_HOLD_VALID (1 << 6) /* standby is complete */
#define RK3576_VOP_INT_VFP            (1 << 7)
#define RK3576_VOP_INT_POST_FULL      (1 << 9) /* output buffer nearly full */

/* SYS0/SYS1 (AXI) interrupt bits -- the other table. */

#define RK3576_VOP_SYS_INT_BUS_ERROR    (1 << 1) /* AXI read error */
#define RK3576_VOP_SYS_INT_WB_UV_FIFO   (1 << 3)
#define RK3576_VOP_SYS_INT_WB_YRGB_FIFO (1 << 4)
#define RK3576_VOP_SYS_INT_WB_COMPLETE  (1 << 5)
#define RK3576_VOP_SYS_INT_MMU_EN       (1 << 7)

/* Old name, kept because it was only ever applied to SYS0/SYS1, where bit1
 * really is the bus error. */

#define RK3576_VOP_INT_BUS_ERRPR RK3576_VOP_SYS_INT_BUS_ERROR

/* SYS_CTRL_SYS_STATUS0 @ 0x0060 is the reference driver's RK3572_VP0_STATUS:
 *   [12:0]  post_buf_empty_dsp_vcnt     the dsp_vcnt at which the output
 *                                       buffer under-ran (a CAPTURE of one
 *                                       event, not a counter)
 *   [14]    post_buf_empty_dsp_vcnt_en  arm the capture
 *   [15]    post_buf_empty_dsp_vcnt_clr clear it (self-clearing)
 *   [28:16] dsp_vcnt                    the current line counter
 *
 * Arming, clearing, waiting and reading back is what turns the latched
 * POST_BUF_EMPTY bit from "something happened at some point" into "it recurs
 * every frame" or "it was a one-off startup transient" -- which is the
 * difference between a fault and an artefact, and is exactly the distinction
 * that has been missing. */

#define RK3576_VOP_STATUS0_BUFEMPTY_VCNT_MASK (0x1fffu)
#define RK3576_VOP_STATUS0_BUFEMPTY_VCNT_EN   (1u << 14)
#define RK3576_VOP_STATUS0_BUFEMPTY_VCNT_CLR  (1u << 15)
#define RK3576_VOP_SYS_VSYNC_CTRL             0x004C
#define RK3576_VOP_DSP_VCNT0_SHIFT            16
#define RK3576_VOP_DSP_VCNT0_MASK             (0x1fff << RK3576_VOP_DSP_VCNT0_SHIFT)

/* SYS_CTRL_SYS_AUTO_GATING_CTRL_IMD (0x27D00000 + 0x0008), reset 0x8155E953.
 *
 * THE CLOCK-GATING SWITCHES, AND RK3576'S DEFAULT IS ON.  Bit positions from
 * the TRM ("SYS_CTRL_SYS_AUTO_GATING_CTRL_IMD") and cross-checked against the
 * reference driver's tables, which read:
 *
 *   .auto_gating_en          = VOP_REG(RK3568_SYS_AUTO_GATING_CTRL, 0x1, 31)
 *   .aclk_pre_auto_gating_en = VOP_REG(RK3568_SYS_AUTO_GATING_CTRL, 0x1, 7)
 *
 *   bit31 auto_gating_en               reset 1  "default auto gating enable"
 *   bit29 axi0_aclk_static_gating_en   reset 0
 *   bit30 axi1_aclk_static_gating_en   reset 0
 *   bit7  aclk_pre_auto_gating_en      reset 0
 *   bit24 rgb_clk_gating_en, and further per-block gating bits below it
 *
 * *** THE REFERENCE DRIVER TURNS THIS OFF, FOR RK3576 BY NAME. ***
 *
 *   auto_gating_en: "Disable auto gating, workaround to avoid display image
 *   shift when a window is enabled."
 *
 *   aclk_pre_auto_gating_en: "Workaround for RK3528/RK3562/RK3576: the aclk
 *   pre auto gating function may disable the aclk in some unexpected cases,
 *   which is detected by hardware automatically.  For example, if enabled, the
 *   post scale function will be affected, resulting in abnormal display."
 *
 * "The aclk may be disabled, detected by hardware automatically" is exactly
 * the state this board was in: the layer was enabled and routed with every
 * register correct and no bus error, yet NOT ONE READ was ever issued
 * (axi0_mmu_idle read 1) and the POST reported a permanent output-buffer
 * under-run that vanished the moment the layer was disabled.  A window whose
 * clock is gated cannot issue a fetch, and gating it produces no error.
 *
 * This driver never wrote the register, so the reset value applied and gating
 * stayed ENABLED for the whole bring-up.
 */

#define RK3576_VOP_SYS_AUTO_GATING_CTRL_IMD 0x0008
#define RK3576_VOP_AUTO_GATING_EN           (1u << 31) /* reset 1 */
#define RK3576_VOP_ACLK_PRE_AUTO_GATING_EN  (1u << 7)

/* VOP MMU0 (base 0x27D07E00).  Per-channel MMU the display windows fetch
 * through; this driver has no page tables and keeps it bypassed.
 *
 *   MMU0_MMU_DTE_ADDR  0x0000  current page table address
 *   MMU0_MMU_STATUS    0x0004  bit0 paging_en
 *                              bit1 page_fault_active
 *                              bit2 stall_active
 *                              bit4 replay_buffer_empty
 *                              bit5 page_fault_is_write
 *                              [10:6] page_fault_bus_id
 *   MMU0_MMU_COMMAND   0x0008  mmu_cmd: 0=ENABLE_PAGING 1=DISABLE_PAGING
 *
 * page_fault_bus_id is "the index of the master responsible for the last page
 * fault", which makes this block a way to observe a window's fetch that does
 * not depend on the AXI bus-error path at all: turn paging on with no page
 * table programmed and EVERY read a window issues must fault. */

#define RK3576_VOP_MMU0_BASE 0x7e00 /* offset from the VOP base */

/* *** THERE ARE TWO MMUs, AND EVERY PROBE IN THIS PROJECT ONLY EVER USED MMU0.
 * ***
 *
 * MMU0 @ 0x27D07E00 and MMU1 @ 0x27D07F00 (TRM 11.5.3.18 / 11.5.3.19).  The
 * MMU page-fault probe turns paging on with dte_addr = 0 so that EVERY
 * translation must fail, which is a stronger test than the poison address
 * because it does not depend on the target address being undecoded.  But it
 * only ever enabled paging on MMU0.  If this window's accesses were served by
 * MMU1 -- and with paging disabled there, an access would simply pass through
 * untranslated and never fault -- then "FAULT_ACTIVE = 0" would be a FALSE
 * NEGATIVE and the conclusion "the layer issues no accesses" would be wrong.
 *
 * A probe that cannot produce the failure it looks for proves nothing, so the
 * fault test now arms BOTH MMUs.
 */

#define RK3576_VOP_MMU1_BASE                     0x7f00 /* offset from the VOP base */
#define RK3576_VOP_MMU_DTE_ADDR                  0x0000
#define RK3576_VOP_MMU_STATUS                    0x0004
#define RK3576_VOP_MMU_COMMAND                   0x0008
#define RK3576_VOP_MMU_STATUS_PAGING_EN          (1u << 0)
#define RK3576_VOP_MMU_STATUS_FAULT_ACTIVE       (1u << 1)
#define RK3576_VOP_MMU_STATUS_FAULT_IS_WRITE     (1u << 5)
#define RK3576_VOP_MMU_STATUS_FAULT_BUS_ID_SHIFT 6
#define RK3576_VOP_MMU_STATUS_FAULT_BUS_ID_MASK \
  (0x1fu << RK3576_VOP_MMU_STATUS_FAULT_BUS_ID_SHIFT)
#define RK3576_VOP_MMU_CMD_ENABLE_PAGING  0u
#define RK3576_VOP_MMU_CMD_DISABLE_PAGING 1u

/* SYS_CTRL_SYS_MMU_CTRL_IMD (0x27D00000 + 0x0020).
 *
 *   bit9    mmu1_bypass_en   "If axi rid > mmu_bypass_id, bypass mmu"  reset 0
 *   [8:4]   mmu_bypass_id                                              reset 0
 *   bit2    mmu_bypass_en    "If axi rid > mmu_bypass_id, bypass mmu"  reset 0
 *   bit1    rkmmu2.0_sel     1'b0: AXI0   1'b1: AXI1                   reset 0
 *   bit11   mmu2.0_soft_rst_en                                         reset 1
 *
 * *** THIS DRIVER USES PHYSICAL ADDRESSES AND HAS NO IOMMU DRIVER, YET IT
 * NEVER WROTE THIS REGISTER. ***  With mmu_bypass_en at its reset value of 0
 * nothing is bypassed, so the layer's physical addresses are offered to an MMU
 * that has no page tables -- and the reference driver never has to deal with
 * that case because it drives the IOMMU itself.
 *
 * A read that lands on an unconfigured MMU need not produce an error: it can
 * simply never come back.  That matches the measurement that has resisted
 * every other explanation -- the layer is enabled, routed, clocked (its
 * register file answers reads), configured to match the reference, and yet
 * issues a read that never arrives, while the POST's input buffer stays empty
 * and the panel shows a static picture that follows neither the framebuffer's
 * content nor its address.
 *
 * Setting bypass_en with bypass_id = 0 makes "rid > 0" true for every window,
 * i.e. everything bypasses, which is the state a driver without an IOMMU
 * needs.
 */

#define RK3576_VOP_SYS_MMU_CTRL_IMD        0x0020
#define RK3576_VOP_SYS_MMU1_BYPASS_EN      (1u << 9)
#define RK3576_VOP_SYS_MMU_BYPASS_ID_SHIFT 4
#define RK3576_VOP_SYS_MMU_BYPASS_ID_MASK \
  (0x1fu << RK3576_VOP_SYS_MMU_BYPASS_ID_SHIFT)
#define RK3576_VOP_SYS_MMU_BYPASS_EN   (1u << 2)
#define RK3576_VOP_SYS_MMU_SOFT_RST_EN (1u << 11) /* reset 1 (released) */

/* SYS_CTRL_SYS_PORT_CTRL_IMD (0x27D00000 + 0x0028).
 *
 *   bit15 auto_cs_mode                 reset 0   (Linux sets 1 for vop3)
 *   bit14 vp_intr_merge_en             reset 0
 *   bit10 vfp2_dma_stop_en             reset 0
 *   bit9  vfp1_dma_stop_en             reset 0
 *   bit8  vfp0_dma_stop_en             reset 0
 *   bit5  auto_cs_en                   reset 1   (Linux sets 1)
 *   bit4  dsp_vs_t_sel                 reset 1   (Linux CLEARS it for vop3)
 *   bit2  vp2_interlace_frm_reg_done   reset 1
 *   bit1  vp1_interlace_frm_reg_done   reset 1
 *   bit0  vp0_interlace_frm_reg_done   reset 1
 *
 * *** TWO OF THESE DIFFER FROM WHAT THE REFERENCE DRIVER PROGRAMS FOR RK3576,
 * AND THIS DRIVER NEVER WROTE THE REGISTER AT ALL. ***  Linux, in the
 * is_vop3() branch of its CRTC enable:
 *
 *     VOP_CTRL_SET(vop2, dsp_vs_t_sel, 0);   // vop3: take vs/t from OUT
 *     VOP_CTRL_SET(vop2, auto_cs_en, 1);
 *     VOP_CTRL_SET(vop2, auto_cs_mode, 1);
 *
 * dsp_vs_t_sel selects where the display's vertical sync and timing are
 * tapped: "1'b0: Dsp_vs_t_out" versus "1'b1: Dsp_vs_t_pre".  Its reset value
 * is 1 and this board reads 1, so the POST has been deriving its timing from
 * the wrong tap in the pipeline.  A POST whose timing is taken from the wrong
 * point does not issue a coherent "I need pixels now" demand, which is
 * consistent with the one measurement that is now firm and hard to explain
 * otherwise: the layer is enabled and routed with every register correct, and
 * yet it ISSUES NO AXI READS AT ALL -- no reads, no errors, and a permanently
 * starved output buffer.
 */

#define RK3576_VOP_SYS_PORT_CTRL_IMD     0x0028
#define RK3576_VOP_SYS_PORT_AUTO_CS_MODE (1u << 15)
#define RK3576_VOP_SYS_PORT_DSP_VS_T_SEL (1u << 4) /* 1 = "pre", clear it */
#define RK3576_VOP_SYS_PORT_AUTO_CS_EN   (1u << 5)

/* SYS_PORT_CTRL_IMD bits[2:0] = vp2/vp1/vp0_interlace_frm_reg_done.
 *
 * *** THIS ONE FIELD IS DEFINED FOR RK3576 AND FOR NO OTHER SoC. ***
 *
 *   .reg_done_frm = VOP_REG_MASK(RK3576_SYS_PORT_CTRL_IMD, 0x7, 0)
 *
 * and Linux writes it to 0 explicitly, with the comment "Set reg done every
 * field for interlace".  The TRM gives the bit's reset value as 1 and reads
 *
 *   1'b0: Reg done every field for interlace.
 *   1'b1: Reg done every frame for interlace.
 *
 * A register dump from this board running a working configuration reads
 * 0028 00070038, i.e. these three bits are 0 -- and this driver has never
 * written them, so they sit at their reset value of 1.
 *
 * It controls WHEN a video port's register updates become valid, which is
 * exactly the mechanism that has been failing here: a configuration that reads
 * back perfectly and never takes effect.  That makes it worth matching the
 * working value rather than leaving it at reset.
 */

#define RK3576_VOP_SYS_PORT_REG_DONE_FRM_MASK 0x7u

/* SYS_CTRL_SYS_AXI_LUT_CTRL_IMD (0x27D00000 + 0x0024): bit9 lut_use_axi1.
 * Linux clears it for RK3576 (VOP_CTRL_SET(vop2, lut_use_axi1, 0)); the reset
 * value is 1 and this board reads 1.  Only the display LUT's own DMA uses it,
 * and this driver never enables that LUT, but it is listed and set here so the
 * global AXI configuration matches the reference rather than the reset. */

#define RK3576_VOP_SYS_AXI_LUT_CTRL_IMD 0x0024
#define RK3576_VOP_SYS_LUT_USE_AXI1     (1u << 9)

/* *** THE SYS_CTRL BLOCK MIXES TWO REGISTER FORMATS. ***
 *
 * Some SYS_CTRL registers carry a "write_mask" field in bits[31:16] that the
 * TRM describes as:
 *
 *   "When every bit HIGH, enable the writing corresponding bit.
 *    When every bit LOW, don't care the writing corresponding bit."
 *
 * A plain putreg() has bits[31:16] == 0, so such a write is DISCARDED --
 * silently, completely, and invisibly: the register keeps its old contents and
 * nothing in a readback says so.  The reference driver implements the scheme
 * explicitly in its mask write helper:
 *
 *   v = ((v & mask) << shift) | (mask << (shift + 16));
 *
 *   HAS the write mask:  SYS_REG_CFG_DONE (0x0000), SYS_WIN_REG_CFG_DONE
 *                        (0x000C), SYS_MMU_CTRL_IMD (0x0020),
 *                        SYS_PORT_CTRL_IMD (0x0028),
 *                        SYS_CLUSTER_PD_CTRL_IMD (0x0030),
 *                        SYS_ESMART_PD_CTRL_IMD (0x0034)
 *   has NO write mask:   SYS_AUTO_GATING_CTRL_IMD (0x0008), SYS_AXI0_CTRL_IMD
 *                        (0x0010), SYS_AXI1_CTRL_IMD (0x001C),
 *                        SYS_AXI_LUT_CTRL_IMD (0x0024)   -- bit31/bit16 there
 *                        are ordinary data or read-only status bits
 *
 * Use rk3576_vop_sys_masked_write() for the first group.  This distinction is
 * not academic: it is why a series of SYS-bit fixes in this bring-up produced
 * no change in behaviour and were then recorded as refuted hypotheses.
 */

/* SYS_CTRL_SYS_ESMART_PD_CTRL_IMD (0x27D00000 + 0x0034).
 * *** HAS THE HIWORD WRITE MASK. ***
 *
 *   bit0   esmart_pd_en     "Power Control to whole Esmart Power Domain,
 *                            include the esmart0/1/2/3.
 *                            1'b0: Power on (immediate effect)
 *                            1'b1: Power down (set corresponding regdone bit
 *                            and it will be vaild on the next frame start)"
 *                                                                     reset 0
 *   [7:6]  esmart_lb_mode   2'b10: 3 x 4k   2'b11: 2 x 4k + 2 x 2k    reset
 * 0x3 bit8   bpp_lut_en       "1'b0: Disable  1'b1: Enable"             reset
 * 0 [11:10] bpp_win_sel     2'b00: Esmart0 ... 2'b11: Esmart3         reset 0
 *
 * The reference driver manages exactly this bit as the ESMART power domain and
 * turns it on before it touches anything else:
 *
 *   rk3576_esmart_pd_regs.pd = VOP_REG_MASK(RK3568_SYS_PD_CTRL, 0x1, 0);
 *
 * and its own timeout message names the offset outright: vop2_readl(vop2,
 * 0x34).  A powered-down ESMART still answers APB accesses to its
 * configuration registers, which is why every readback in this bring-up has
 * been consistent with a window that is correctly programmed and physically
 * unable to fetch.
 */

#define RK3576_VOP_SYS_ESMART_PD_CTRL_IMD 0x0034
#define RK3576_VOP_SYS_ESMART_PD_EN       (1u << 0)

/* SYS_ESMART_PD_CTRL_IMD [7:6] esmart_lb_mode -- how the ESMART line buffers
 * (lb) are partitioned, which decides how many of them are 4K:
 *
 *   2'b10: 3 x 4k          (VOP3_ESMART_4K_4K_4K_MODE)
 *   2'b11: 2 x 4k + 2 x 2k (VOP3_ESMART_4K_4K_2K_2K_MODE)   reset value
 *
 * THE RESET VALUE AND THE REFERENCE DRIVER'S DEFAULT ARE BOTH 3, AND THIS
 * DRIVER WAS WRITING 2.  The reference computes it in
 * vop3_get_esmart_lb_mode():
 *
 *   vop2->esmart_lb_mode = vop2->data->esmart_lb_mode;      // no DT property
 *   ... rk3576_vop.esmart_lb_mode = VOP3_ESMART_4K_4K_2K_2K_MODE
 *   ... looked up in rk3576_esmart_lb_mode_map[]:
 *         {{VOP3_ESMART_4K_4K_4K_MODE,   2},
 *          {VOP3_ESMART_4K_4K_2K_2K_MODE, 3}}
 *   => 3
 *
 * A note here used to claim the lookup fails and falls back to map[0] = 2.  It
 * does not: 4K_4K_2K_2K_MODE IS in RK3576's map, so the lookup succeeds and
 * yields 3.  (A fallback to 2 would need a device-tree value that is absent
 * from the map, which is a different situation from having no property at
 * all.)
 *
 * WHY IT MIGHT MATTER.  The mode names list the buffer sizes per ESMART, so
 * 4K_4K_2K_2K gives ESMART0/1 4K buffers and ESMART2/3 only 2K -- and a
 * 720-pixel RGB888 line is 720*3 = 2160 bytes, which does NOT fit in a 2K
 * buffer.  That is a complete explanation for why the earlier ESMART2-era
 * experiments could never fetch, and it is why this driver's author chose 2 in
 * the first place.
 *
 * It does NOT explain ESMART0's failure, because ESMART0 receives a 4K buffer
 * in either mode.  It is being set to 3 anyway because 3 is the only remaining
 * value in this driver that differs from BOTH the reference driver's default
 * and the hardware's own reset value, and that class of difference is exactly
 * what has produced every real bug found in this bring-up.  If it changes
 * nothing, line-buffer partitioning is eliminated too.
 */

#define RK3576_VOP_SYS_ESMART_LB_MODE_SHIFT 6
#define RK3576_VOP_SYS_ESMART_LB_MODE_MASK \
  (0x3u << RK3576_VOP_SYS_ESMART_LB_MODE_SHIFT)
#define RK3576_VOP_SYS_ESMART_LB_MODE_4K_4K_2K_2K \
  3u /* reference default + reset */

/* SYS_CTRL_SYS_CLUSTER_PD_CTRL_IMD (0x27D00000 + 0x0030).
 * *** HAS THE HIWORD WRITE MASK. ***
 *
 *   bit0  cluster01_pd_en  "Power Control to whole Cluster Power Domain,
 *                           include the cluster0/1.
 *                           1'b0: Power on (immediate effect)
 *                           1'b1: Power down (... next frame start)"  reset 0
 */

#define RK3576_VOP_SYS_CLUSTER_PD_CTRL_IMD 0x0030
#define RK3576_VOP_SYS_CLUSTER01_PD_EN     (1u << 0)

/* SYS_CTRL_SYS_AXI_HURRY_CTRL0_IMD (0x0014) and _CTRL1_IMD (0x0018).
 *
 * Neither has a hiword write mask (CTRL0 has [31:27] RO, CTRL1 [31:15] RO), so
 * plain writes are correct here.
 *
 *   CTRL0 bit24  axi0_port0_urgency_en   1'b0: Disable  1'b1: Enable
 *   CTRL0 bit25  axi0_port1_urgency_en
 *   CTRL0 bit26  axi0_port2_urgency_en
 *   CTRL1        axi1_port0/1/2_urgency_en, same numbering
 *
 * These gate the POST's urgency signal (see POST_COLOR_CTRL) onto the AXI bus
 * arbitration.  The TRM's SoC chapter is explicit about what they buy: when
 * the VOP's read-urgency line is high, the DDR controller's mask_ctrl module
 * "will mask all the other requests" so that this read port is served first.
 *
 * The reference driver sets them for every video port it brings up --
 *
 *   .axi0_port_urgency_en = VOP_REG(RK3576_SYS_AXI_HURRY_CTRL0_IMD, 0x1, 24),
 *   .axi1_port_urgency_en = VOP_REG(RK3576_SYS_AXI_HURRY_CTRL1_IMD, 0x1, 24),
 *
 * with the bit number following the video port (24 = VP0, 25 = VP1, 26 = VP2,
 * per the TRM's port0/port1/port2 naming).
 *
 * *** AND THE REGISTER SCAN SHOWED BOTH REGISTERS AS 0x00000000 ***, i.e. the
 * whole mechanism was off, which is consistent with the one measurement that
 * has never been explained away: POST_BUF_EMPTY recurring while the layer is
 * on and stopping while it is off.
 */

#define RK3576_VOP_SYS_AXI_HURRY_CTRL0_IMD   0x0014
#define RK3576_VOP_SYS_AXI_HURRY_CTRL1_IMD   0x0018
#define RK3576_VOP_SYS_AXI_HURRY_AXI0_VP0_EN (1u << 24)
#define RK3576_VOP_SYS_AXI_HURRY_AXI1_VP0_EN (1u << 24)

/* SYS_CTRL AXI throughput control (IMD = immediate, applies without a
 * cfg_done pulse).  Each AXI channel caps the number of outstanding
 * transactions the VOP may issue, so its scan-out DMA cannot monopolise
 * the NoC and starve the CPU.
 *
 * BIT ORDER TAKEN FROM THE TRM (Part 2, "SYS_CTRL_SYS_AXI0_CTRL_IMD",
 * 0x27D00000 + 0x0010):
 *
 *   [9:4]  axiN_outstanding_num   RW  reset 0x00
 *   [3:2]  reserved               RO
 *   bit1   axiN_outstanding_en    RW  reset 0    1 = limit bursts
 *   bit0   axiN_dma_stop          RW  reset 0    1 = STOP this AXI channel
 *   bit16  axiN_mmu_idle          RO
 *
 * *** bit0 IS dma_stop, NOT outstanding_en. ***  An earlier version of this
 * header had the two the other way round, and the consequence was not
 * cosmetic.  The driver wrote outstanding_en into bit0, which ENABLES
 * axi0_dma_stop -- and the ESMART layer fetches through axi0
 * (esmart_axi_sel reset = axi0).  So the layer's memory access was switched
 * OFF for the entire bring-up: the layer could not read the framebuffer at
 * all, the POST was starved (measured: POST_BUF_EMPTY recurring), and the
 * panel showed a static garble that followed neither the framebuffer's
 * content nor its address -- while every layer register read back exactly as
 * programmed, because the registers were fine; the fetch was stopped.
 *
 * The readback carried the proof and it was printed and ignored:
 * SYS_AXI0_CTRL_IMD read 0x00010101, whose bit16 (mmu_idle = 1) means
 * "nothing is being fetched".
 */

#define RK3576_VOP_SYS_AXI0_CTRL_IMD         0x0010
#define RK3576_VOP_SYS_AXI1_CTRL_IMD         0x001C
#define RK3576_VOP_SYS_AXI_DMA_STOP          (1u << 0) /* 1 = STOP the channel */
#define RK3576_VOP_SYS_AXI_OUTSTANDING_EN    (1u << 1)
#define RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT 4
#define RK3576_VOP_SYS_AXI_OUTSTANDING_MASK \
  (0x3f << RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT)
#define RK3576_VOP_SYS_AXI_OUTSTANDING(n)                    \
  (((uint32_t)(n) << RK3576_VOP_SYS_AXI_OUTSTANDING_SHIFT) & \
   RK3576_VOP_SYS_AXI_OUTSTANDING_MASK)

/* bit16 axiN_mmu_idle -- READ-ONLY channel status ("1'b0: MMU is busy status,
 * 1'b1: MMU is idle status"), i.e. 0 means a translation is in flight on that
 * channel.  It has only ever been sampled once per boot in this bring-up, at a
 * moment when nothing was expected to be moving; sampling it repeatedly across
 * whole frames is a positive test for whether the channel is transferring at
 * all, which is the opposite kind of evidence to everything gathered so far.
 */

#define RK3576_VOP_SYS_AXI_MMU_IDLE (1u << 16)

/* AXI channel interrupt bits, from SYS1_INTR_EN (whose field names the TRM
 * states alongside their numbers) and the matching *_INTR_RAW_STATUS of both
 * channels:
 *
 *   bit7 intr_*_mmu           the channel's MMU raised an interrupt
 *   bit2 intr_*_dma_finish    the channel's DMA finished a transfer
 *   bit1 intr_*_bus_error     the channel saw a bus error
 *
 * (SYS0's own bit list is badly extracted in the TRM text -- the numbers and
 * the names come out in separate blocks -- but the layout matches SYS1's,
 * whose EN register pairs each name with its number explicitly.  The names
 * already used here for bits 1/3/4/5/7 are consistent with that.)
 *
 * These are LATCHES, which is what makes them usable as positive evidence:
 * unlike a status bit that reports only the present instant, a latched
 * dma_finish that REAPPEARS after being cleared proves traffic actually
 * happened.
 */

#define RK3576_VOP_SYS_INT_DMA_FINISH (1 << 2)   /* channel DMA finished */
#define RK3576_VOP_SYS_AXI_MMU_IDLE   (1u << 16) /* RO: 1 = nothing fetched */

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

#define RK3576_VOP_IFACE_POL_MASK \
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

/* WIN0_CTRL0 field definitions.
 *
 * *** TWO OF THESE WERE WRONG, AND THEY ARE THE KIND OF WRONG THAT MAKES A
 * WINDOW NEVER FETCH. ***  Verified field by field against RK3576's own window
 * table (Rockchip's vop2 driver, rk3576_cluster0_win_data):
 *
 *   .enable         = CTRL0, 0x1,  0
 *   .format         = CTRL0, 0x3f, 1      <- SIX bits, unlike ESMART's five
 *   .tile_mode      = CTRL0, 0x1,  7
 *   .y2r_en         = CTRL0, 0x1,  8
 *   .r2y_en         = CTRL0, 0x1,  9
 *   .csc_mode       = CTRL0, 0x7, 10      <- was written as shift 9 here
 *   .rb_swap        = CTRL0, 0x1, 14      <- was written as bit 15 here
 *   .uv_swap        = CTRL0, 0x1, 17
 *   .dither_up      = CTRL0, 0x1, 18
 *   .yuv_clip       = CTRL0, 0x1, 19
 *   .ymirror        = CTRL0, 0x1, 21
 *   .csc_y2r_path_sel = CTRL0, 0x1, 24
 *
 * There is no rg_swap on this window (the reference data defines one only for
 * ESMART), so the old RG_SWAP macro is gone rather than left as a landmine.
 */

#define RK3576_VOP_WIN0_EN               (1 << 0) /* Layer enable */
#define RK3576_VOP_WIN0_DATA_FMT_SHIFT   1        /* Pixel format, 6 bits */
#define RK3576_VOP_WIN0_DATA_FMT_MASK    (0x3f << RK3576_VOP_WIN0_DATA_FMT_SHIFT)
#define RK3576_VOP_WIN0_TILE_MODE        (1 << 7) /* 1 = AFBC tiled input */
#define RK3576_VOP_WIN0_CSC_Y2R_EN       (1 << 8) /* CSC Y2R enable */
#define RK3576_VOP_WIN0_CSC_R2Y_EN       (1 << 9) /* CSC R2Y enable */
#define RK3576_VOP_WIN0_CSC_MODE_SHIFT   10       /* CSC mode, 3 bits */
#define RK3576_VOP_WIN0_CSC_MODE_MASK    (0x7 << RK3576_VOP_WIN0_CSC_MODE_SHIFT)
#define RK3576_VOP_WIN0_RB_SWAP          (1 << 14) /* Swap R and B */
#define RK3576_VOP_WIN0_UV_SWAP          (1 << 17) /* Swap U and V */
#define RK3576_VOP_WIN0_DITHER_UP_EN     (1 << 18) /* Dither up */
#define RK3576_VOP_WIN0_YUV_CLIP         (1 << 19) /* YCbCr clip */
#define RK3576_VOP_WIN0_YMIRROR          (1 << 21) /* Vertical mirror */
#define RK3576_VOP_WIN0_CSC_Y2R_PATH_SEL (1 << 24)

/* win0_data_fmt values (CTRL0[6:1]).  The SAME numbers as ESMART's: the format
 * enum in the reference driver is shared, so RGB888 is 1 for both. */

#define RK3576_VOP_WIN0_FMT_ARGB8888 (0x00 << RK3576_VOP_WIN0_DATA_FMT_SHIFT)
#define RK3576_VOP_WIN0_FMT_RGB888   (0x01 << RK3576_VOP_WIN0_DATA_FMT_SHIFT)
#define RK3576_VOP_WIN0_FMT_RGB565   (0x02 << RK3576_VOP_WIN0_DATA_FMT_SHIFT)

/* WIN0_CTRL2 -- the CLUSTER's own AXI read-id pair, where ESMART keeps its in
 * ESMART_CTRL1.  Different register, different field positions: yrgb [4:0],
 * uv [9:5], not [8:4] and [16:12]. */

#define RK3576_VOP_WIN0_CTRL2_YRGB_ID_SHIFT 0
#define RK3576_VOP_WIN0_CTRL2_YRGB_ID_MASK \
  (0x1fu << RK3576_VOP_WIN0_CTRL2_YRGB_ID_SHIFT)
#define RK3576_VOP_WIN0_CTRL2_UV_ID_SHIFT 5
#define RK3576_VOP_WIN0_CTRL2_UV_ID_MASK \
  (0x1fu << RK3576_VOP_WIN0_CTRL2_UV_ID_SHIFT)

/* CLUSTER0_CTRL @ VOP + 0x1100 (cluster block base 0x1000, so offset 0x0100).
 *
 * Cluster-level control, with no ESMART equivalent:
 *
 *   bit0   cluster_enable
 *   bit1   afbc_enable          0 for a linear framebuffer
 *   [7:4]  lb_mode              line-buffer split
 *   bit29  dma_stride_4k_disable
 *   bit31  frm_reset_en
 *
 * The reference driver sets frm_reset_en = 1 and dma_stride_4k_disable = 1 for
 * CLUSTER windows and NEVER sets either for an ESMART window, so a CLUSTER
 * configured without them is not a faithful copy of a working configuration.
 */

#define RK3576_VOP_CLUSTER0_CTRL_OFF          0x0100
#define RK3576_VOP_CLUSTER_CTRL_EN            (1u << 0)
#define RK3576_VOP_CLUSTER_CTRL_AFBC_EN       (1u << 1)
#define RK3576_VOP_CLUSTER_CTRL_LB_MODE_SHIFT 4
#define RK3576_VOP_CLUSTER_CTRL_LB_MODE_MASK \
  (0xfu << RK3576_VOP_CLUSTER_CTRL_LB_MODE_SHIFT)
#define RK3576_VOP_CLUSTER_CTRL_DMA_STRIDE_4K_DIS (1u << 29)
#define RK3576_VOP_CLUSTER_CTRL_FRM_RESET_EN      (1u << 31)

/* CLUSTER0's port select and window delay live at the same block offsets as
 * ESMART's (0x00F4 / 0x00F8) -- the reference header puts them at 0x11F4 and
 * 0x11F8 on a 0x1000 base.  DLY_NUM is 16 bits here, not 8. */

#define RK3576_VOP_CLUSTER0_PORT_SEL_IMD 0x00F4
#define RK3576_VOP_CLUSTER0_DLY_NUM      0x00F8
#define RK3576_VOP_CLUSTER0_DLY_NUM_MASK 0xffffu

/* RK3576 gives CLUSTER0 its own AXI read IDs, like every other window:
 * yrgb 0x0a / uv 0x0b on the primary video port (Rockchip's window table). */

#define RK3576_VOP_CLUSTER0_AXI_YRGB_ID 0x0au
#define RK3576_VOP_CLUSTER0_AXI_UV_ID   0x0bu

/* -----------------------------------------------------------------------
 * OVERLAY_PORTx registers (base + RK3576_VOP_OVERLAY_PORTx_OFFSET).
 * ----------------------------------------------------------------------- */

/* OVERLAY_PORTx_CTRL @ 0x0600.
 *
 * *** bit28 IS THE ONE THAT MAKES THE OVERLAY'S LAYER SELECT TAKE EFFECT, AND
 * THIS DRIVER NEVER WROTE THIS REGISTER AT ALL. ***
 *
 * A register dump taken from this same board running a Debian image with HDMI
 * working -- i.e. a configuration KNOWN to fetch and display -- reads
 *
 *   0600 10000001
 *
 * so bit28 is set.  The vendor header names it:
 *
 *   #define RK3568_OVL_CTRL__LAYERSEL_REGDONE_IMD   BIT(28)
 *
 * and the vendor driver's comment where it asserts it states its purpose:
 *
 *   "Register OVERLAY_LAYER_SEL and OVERLAY_PORT_SEL should take effect
 *    immediately, than windows configuration(CLUSTER/ESMART/SMART) can take
 *    effect according the video port mux configuration as we wished."
 *
 * Read that as a statement of what happens WITHOUT the bit: the overlay's
 * layer selection does not take effect, and a window's configuration never
 * becomes effective through the video port mux -- a window that is configured,
 * enabled and selected by the mixer, yet never actually connected to the video
 * port. That is precisely the state this bring-up has been stuck in.
 *
 * Note that RK3576's control table in the vendor driver has NO field for this
 * bit (only rk3568 and rk3588 do), so it is not something Linux re-asserts on
 * this SoC -- it is left set by the bootloader and inherited.  Whether this
 * board's bootloader sets it has never been checked, because this driver does
 * not write 0x0600 at all.
 *
 * bit0 is overlay_mode, which the vendor driver sets from whether the output
 * is YUV.  Ours is RGB, so it should remain 0.
 */

#define RK3576_VOP_OVERLAY_LAYERSEL_REGDONE_IMD (1u << 28)
#define RK3576_VOP_OVERLAY_MODE                 (1u << 0)

#define RK3576_VOP_OVERLAY_CTRL                 0x0000 /* Overlay ctrl config */
#define RK3576_VOP_OVERLAY_LAYER_SEL            0x0004 /* Overlay layer select */
#define RK3576_VOP_OVERLAY_MIX0_SRC_COLOR_CTRL \
  0x0020 /* Mixer0 src color ctrl */
#define RK3576_VOP_OVERLAY_MIX0_DST_COLOR_CTRL \
  0x0024 /* Mixer0 dst color ctrl */
#define RK3576_VOP_OVERLAY_MIX0_SRC_ALPHA_CTRL \
  0x0028 /* Mixer0 src alpha ctrl */
#define RK3576_VOP_OVERLAY_MIX0_DST_ALPHA_CTRL \
  0x002C                                      /* Mixer0 dst alpha ctrl */
#define RK3576_VOP_OVERLAY_BG_MIX_CTRL 0x0070 /* Background mix ctrl */

/* OVERLAY_PORTx_BG_MIX_CTRL [31:24] bg_dly = the port's LAST-MUX OUTPUT DELAY.
 *
 * *** THIS DRIVER NEVER WROTE THIS REGISTER, AND THE VALUE MATTERS. ***
 *
 * The TRM states it outright (Part 2, Table 11-4 "Default table of layer delay
 * number"):
 *
 *   Block                                     Delay_num
 *   Esmart layer                              0
 *   Cluster layer                             0
 *   The delay of the port0 last mux output    20
 *
 * and the reference driver's arithmetic agrees.  For RK3576 video output 0 its
 * per-VP constants are win_dly = 10, layer_mix_dly = 8, hdr_mix_dly = 2, and
 * with no HDR, no CGC and no sdr2hdr the SDR branch wins:
 *
 *   max_sdr_dly = win_dly + layer_mix_dly + sdr2hdr_dly + hdr_mix_dly
 *               = 10 + 8 + 0 + 2 = 20 = bg_dly
 *
 * An earlier version of this driver used 0x10 (16), justified with the
 * reference driver's comment "the default bg_dly is 0x10" -- but that constant
 * is RK3568's (8 + 6 + 2 = 16), not RK3576's.  It also did not write the
 * register: it merely asserted that BG_MIX_CTRL[31:24] "already holds" 0x10,
 * i.e. it trusted an inherited bootloader value that happened to match a
 * guess. The reason the early dump exists is that inherited state has to be
 * written, not assumed.
 */

#define RK3576_VOP_OVERLAY_BG_DLY_SHIFT 24
#define RK3576_VOP_OVERLAY_BG_DLY_MASK \
  (0xffu << RK3576_VOP_OVERLAY_BG_DLY_SHIFT)
#define RK3576_VOP_OVERLAY_BG_DLY_VP0 20u /* TRM Table 11-4 */

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

#define RK3576_VOP_MIX_CTRL_GLB_ALPHA_SHIFT \
  16 /* src: [31:24], dst: [23:16]          \
      */
#define RK3576_VOP_MIX_CTRL_ALPHA_EN     (1 << 8) /* alpha blending enable */
#define RK3576_VOP_MIX_CTRL_FACTOR_SHIFT 5
#define RK3576_VOP_MIX_CTRL_FACTOR_MASK \
  (0x7 << RK3576_VOP_MIX_CTRL_FACTOR_SHIFT)
#define RK3576_VOP_MIX_CTRL_ALPHA_CAL_MODE (1 << 4) /* 1=no saturation */
#define RK3576_VOP_MIX_CTRL_BLEND_SHIFT    2
#define RK3576_VOP_MIX_CTRL_BLEND_MASK     (0x3 << RK3576_VOP_MIX_CTRL_BLEND_SHIFT)
#define RK3576_VOP_MIX_CTRL_ALPHA_MODE     (1 << 1) /* 1=255-As (inverse) */
#define RK3576_VOP_MIX_CTRL_COLOR_MODE     (1 << 0) /* 1=Cs*As0 (pre-mul) */

/* src/dst ALPHA_CTRL field bits (0x28 / 0x2C). */

#define RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT 5
#define RK3576_VOP_MIX_ALPHA_FACTOR_MASK \
  (0x7 << RK3576_VOP_MIX_ALPHA_FACTOR_SHIFT)
#define RK3576_VOP_MIX_ALPHA_CAL_MODE    (1 << 4)
#define RK3576_VOP_MIX_ALPHA_BLEND_SHIFT 2
#define RK3576_VOP_MIX_ALPHA_BLEND_MASK \
  (0x3 << RK3576_VOP_MIX_ALPHA_BLEND_SHIFT)
#define RK3576_VOP_MIX_ALPHA_MODE (1 << 1)

/* factor_mode encodings (3-bit, matching TRM *_FACTOR_MODE). */

#define RK3576_VOP_FACTOR_ZERO        0 /* 3'b000 = 0 */
#define RK3576_VOP_FACTOR_ONE         1 /* 3'b001 = 256 */
#define RK3576_VOP_FACTOR_DST         2 /* 3'b010 = Ad0 */
#define RK3576_VOP_FACTOR_DST_INVERSE 3 /* 3'b011 = 256-Ad0 */
#define RK3576_VOP_FACTOR_SRC         4 /* 3'b100 = As0 */
#define RK3576_VOP_FACTOR_SRC_GLOBAL  5 /* 3'b101 = Ags */

/* blend_mode encodings (2-bit). */

#define RK3576_VOP_BLEND_GLOBAL         0 /* use global alpha only */
#define RK3576_VOP_BLEND_PER_PIX        1 /* use per-pixel alpha only */
#define RK3576_VOP_BLEND_PER_PIX_GLOBAL 2 /* per-pixel * global */

#define RK3576_VOP_LAYER_SEL_SHIFT0     0
#define RK3576_VOP_LAYER_SEL_SHIFT1     4
#define RK3576_VOP_LAYER_SEL_SHIFT2     8
#define RK3576_VOP_LAYER_SEL_SHIFT3     12
#define RK3576_VOP_LAYER_SEL_MASK       (0xf << RK3576_VOP_LAYER_SEL_SHIFT0)

/* Window select values (common to all layers). */

#define RK3576_VOP_LAYER_SEL_CLUSTER0 0x0 /* Cluster0 */
#define RK3576_VOP_LAYER_SEL_CLUSTER1 0x1 /* Cluster1 */
#define RK3576_VOP_LAYER_SEL_ESMART0  0x2 /* Esmart0 */
#define RK3576_VOP_LAYER_SEL_ESMART2  0x3 /* Esmart2 (also reaches port 0) */
#define RK3576_VOP_LAYER_SEL_DISABLE  0xf /* Unused/disable */
#define RK3576_VOP_LAYER_SEL_L0(x)    ((x) << RK3576_VOP_LAYER_SEL_SHIFT0)
#define RK3576_VOP_LAYER_SEL_L1(x)    ((x) << RK3576_VOP_LAYER_SEL_SHIFT1)
#define RK3576_VOP_LAYER_SEL_L2(x)    ((x) << RK3576_VOP_LAYER_SEL_SHIFT2)
#define RK3576_VOP_LAYER_SEL_L3(x)    ((x) << RK3576_VOP_LAYER_SEL_SHIFT3)

/* -----------------------------------------------------------------------
 * POSTx_CTRL registers (base + RK3576_VOP_POSTx_OFFSET).
 * ----------------------------------------------------------------------- */

#define RK3576_VOP_POST_DSP_CTRL   0x0000 /* DSP ctrl (standby/out mode) */
#define RK3576_VOP_POST_MIPI_CTRL  0x0004 /* MIPI te/double channel */
#define RK3576_VOP_POST_COLOR_CTRL 0x0008 /* DSP colour / urgency ctrl */
#define RK3576_VOP_POST_CORE_CLK   0x000C /* Core clock select */
#define RK3576_VOP_POST_DSP_BG     0x002C /* Background color */
#define RK3576_VOP_POST_PRE_SCAN_HTIMING \
  0x0030 /* Layer read-request lead time */
#define RK3576_VOP_POST_DSP_HACT_INFO   \
  0x0034 /* POST output H active st/end \
          */
#define RK3576_VOP_POST_DSP_VACT_INFO                                         \
  0x0038                                       /* POST output V active st/end \
                                                */
#define RK3576_VOP_POST_SCL_FACTOR_YRGB 0x003C /* POST scaler factors */

/* POST_SCL_FACTOR_YRGB: the POST's OUTPUT scaler, the last block before the
 * DSI.  (TRM 11.5.3.x, POST0_CTRL + 0x003C)
 *
 *   post_vs_factor[31:16] = (src_height / dst_height) * 2^12
 *   post_hs_factor[15:0]  = (src_width  / dst_width ) * 2^12
 *
 * 1.0 is therefore 0x1000, and 1:1 pass-through is 0x10001000.  That is what
 * Linux writes for an unscaled video port --
 *
 *   val  = scl_cal_scale2(vdisplay, vsize) << 16;
 *   val |= scl_cal_scale2(hdisplay, hsize);
 *   VOP_MODULE_SET(vop2, vp, post_scl_factor, val);
 *
 * -- unconditionally, 1:1 included, because scl_cal_scale2() returns 2^12 for
 * equal source and destination.  It is NOT the same helper as the window
 * scaler's vop2_scale_factor(), which returns 0 for SCALE_NONE; this driver
 * conflated the two and wrote 0 here, which is a degenerate "scale by zero"
 * rather than a pass-through.  A dump from a working configuration on this
 * board reads 0x10001000, confirming the Linux arithmetic.
 */

#define RK3576_VOP_POST_SCL_FACTOR_1_1    0x10001000u
#define RK3576_VOP_POST_SCL_CTRL          0x0040 /* POST scaler enables */
#define RK3576_VOP_POST_DSP_HTOTAL_HS_END 0x0048 /* H total + hsync end */
#define RK3576_VOP_POST_DSP_HACT_ST_END   0x004C /* H active start/end */
#define RK3576_VOP_POST_DSP_VTOTAL_VS_END 0x0050 /* V total + vsync end */
#define RK3576_VOP_POST_DSP_VACT_ST_END   0x0054 /* V active start/end */

/* POST_SCL_CTRL (0x0040): the POST scaler is a straight 1:1 pass-through in
 * this driver, so both bits stay 0.  They exist here so the register is
 * programmed EXPLICITLY rather than left at its reset value by accident. */

#define RK3576_VOP_POST_SCL_CTRL_HSCALEDOWN (1 << 0)
#define RK3576_VOP_POST_SCL_CTRL_VSCALEDOWN (1 << 1)

/* POST_DSP_CTRL field definitions.
 *
 * BIT POSITIONS FROM THE TRM (Part 2, "POST0_CTRL_POST_DSP_CTRL",
 * 0x27D00C00 + 0x0000; cross-checked against the reference driver, whose
 * rk35xx_vop_ctrl tables put dsp_lut_en at bit 28):
 *
 *   bit31 vop_standby_en_imd     reset 1  (standby on at reset)
 *   bit30 vop_fp_standby_en_imd  reset 0
 *   bit28 dsp_lut_en             reset 0
 *   bit27 dsp_black_en           "the pixel data output is all black"
 *   bit26 dsp_out_zero           "all output '0'"
 *   bit24 dsp_blank_en           blanks hsync/vsync/den
 *   bit23 post_lb_mode           reset 0
 *   bit20 dither_down_mode
 *   [19:18] dither_down_sel
 *   bit17 dither_down_en
 *
 * *** The force-output bits are at 27/26, NOT at 16/15. ***  This header had
 * dsp_out_zero at bit15, dsp_black_en at bit16 and dsp_blank_en at bit17, so
 * the "POST forced BLACK" and "POST forced ZERO" rungs of the source probe
 * wrote reserved bits and changed nothing -- and the observation "forcing the
 * POST output black did not blank the screen" was used as evidence when in
 * fact no such thing had been requested.  That conclusion is retracted.
 *
 * These are probe-only bits; the normal path writes 0, which is why a wrong
 * position here could not corrupt normal operation -- only the probe's
 * meaning.
 */

#define RK3576_VOP_POST_STANDBY_EN     (1u << 31) /* Standby mode */
#define RK3576_VOP_POST_LUT_EN         (1u << 28) /* display LUT enable */
#define RK3576_VOP_POST_BLACK_EN       (1u << 27) /* output all black */
#define RK3576_VOP_POST_DSP_OUT_ZERO   (1u << 26) /* output all zero */
#define RK3576_VOP_POST_BLANK_EN       (1u << 24) /* blank hs/vs/den */
#define RK3576_VOP_POST_DITHER_DOWN_EN (1u << 17) /* Dither down */

/* dsp_out_mode values (POST_DSP_CTRL[3:0]). */

#define RK3576_VOP_POST_OUT_MODE_SHIFT 0
#define RK3576_VOP_POST_OUT_MODE_MASK  (0xf << RK3576_VOP_POST_OUT_MODE_SHIFT)
#define RK3576_VOP_POST_OUT_RGB888     (0x0 << RK3576_VOP_POST_OUT_MODE_SHIFT)
#define RK3576_VOP_POST_OUT_RGB666     (0x1 << RK3576_VOP_POST_OUT_MODE_SHIFT)
#define RK3576_VOP_POST_OUT_RGB565     (0x2 << RK3576_VOP_POST_OUT_MODE_SHIFT)

/* POST_PRE_SCAN_HTIMING (0x0030) -- the layers' read-request lead time.
 *
 * TRM 11.4 "H. Pre_scan_active": "Pre_scan_active define that layers send the
 * read data request early vop post read pixel data in post line buff.", with
 *
 *     Pre_scan_hactive = pre_scan_max_dly + act_width / 2 - 1
 *
 * *** RESET VALUE IS ZERO, AND ZERO IS WRONG. ***  With it at zero the layers
 * ask for their pixels at the very moment the POST wants to read them out of
 * its line buffer -- too late -- so the buffer under-runs (POST_BUF_EMPTY).  A
 * starved POST transmits undefined data instead of the framebuffer, which is a
 * picture that follows neither the framebuffer's content nor its address,
 * while every layer register reads back exactly as programmed.
 *
 * The reference driver computes it in vop2_calc_dly_num():
 *
 *     pre_scan_dly = bg_dly + (roundup(hdisplay, 2) >> 1) - 1;
 *     pre_scan_dly = (pre_scan_dly << 16) | (hsync_len < 8 ? 8 : hsync_len);
 *
 * Its comment adds why the hblank half must not be small: "pre_scan_hblank
 * minimum value is 8, otherwise the win reset signal will lead to first line
 * data be zero".
 *
 * *** bg_dly IS 20, NOT 0x10. ***  The reference driver's comment "the default
 * bg_dly is 0x10" describes RK3568 (win_dly 8 + layer_mix_dly 6 + hdr_mix_dly
 * 2).  RK3576 video output 0 uses 10 + 8 + 2 = 20, and the TRM's Table 11-4
 * states the same thing directly ("The delay of the port0 last mux output
 * 20"). An earlier version of this header claimed BG_MIX_CTRL[31:24] "already
 * holds 0x10 on this board, so the two sources agree" -- which was a guess
 * matching an inherited bootloader value, not agreement.  See
 * RK3576_VOP_OVERLAY_BG_DLY_VP0: the register is now written explicitly, and
 * pre_scan is computed from the same 20 so the two remain consistent.
 */

#define RK3576_VOP_POST_PRE_SCAN_HACTIVE_SHIFT 16
#define RK3576_VOP_POST_PRE_SCAN_HACTIVE_MASK \
  (0x1fffu << RK3576_VOP_POST_PRE_SCAN_HACTIVE_SHIFT)
#define RK3576_VOP_POST_PRE_SCAN_HBLANK_MASK 0x1fffu

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

#define RK3576_VOP_POST_CORE_CLK_DCLK_CORE_SEL \
  (1u << 0) /* 0=dclk, 1=dclk/2                \
             */
#define RK3576_VOP_POST_CORE_CLK_DCLK_OUT_SEL \
  (1u << 2) /* 0=dclk, 1=dclk/2               \
             */

/* POST_DSP_BG field definitions.  (POST0_CTRL_POST_DSP_BG, + 0x002C)
 *
 * *** THE BIT MAPPING BELOW IS SET BY MEASUREMENT, NOT BY READING THE TRM. ***
 *
 * A probe stepped this register through five colours and the screen was asked
 * to repeat the order back.  It wrote, in order, 0x3ff into the field named
 * red, then into the field named green, then blue, white, black; the screen
 * showed RED, BLUE, GREEN, WHITE, BLACK.  So on this hardware:
 *
 *     0x3ff at bits[29:20]  ->  RED    on the glass
 *     0x3ff at bits[19:10]  ->  GREEN  on the glass
 *     0x3ff at bits[ 9:0]   ->  BLUE   on the glass
 *
 * i.e. the plain RGB order, and a value written through the BLUE field appears
 * green while one written through the GREEN field appears blue.  Either the
 * TRM's field naming for this register is the other way round, or something
 * downstream exchanges green and blue; the observation cannot tell the two
 * apart, and for the purpose of controlling the screen it does not need to.
 * What matters is that the mapping above is the one that produces the intended
 * colour, and it is the one encoded here.
 *
 * (Worth noting for later, since it cannot be distinguished yet: if the
 * exchange is downstream of the mixer rather than a register naming quirk,
 * then the LAYER's colours are exchanged too and a framebuffer will show blue
 * where it should show green.  That is a colour error, not a cause of garbage,
 * so it is recorded and set aside.)
 *
 * THE LESSON: a probe that drove this register with the TRIED-AND-WRONG value
 * reported "blue" and that report was believed for a whole round, which is
 * what prompted swapping these two macros in the first place.  The colour
 * actually programmed at the time was green.  A marker whose colour is not
 * verified is not a marker.
 */

#define RK3576_VOP_POST_BG_DISPLAY_EN  (1u << 31) /* BG display en */
#define RK3576_VOP_POST_BG_RED_SHIFT   20         /* 10-bit red   */
#define RK3576_VOP_POST_BG_GREEN_SHIFT 10         /* 10-bit green */
#define RK3576_VOP_POST_BG_BLUE_SHIFT  0          /* 10-bit blue  */

/* Build a POST background word from 10-bit components, in the plain order a
 * caller would expect regardless of how the hardware packs them. */

#define RK3576_VOP_POST_BG_RGB(r, g, b)                         \
  ((((uint32_t)(r)&0x3ffu) << RK3576_VOP_POST_BG_RED_SHIFT) |   \
   (((uint32_t)(g)&0x3ffu) << RK3576_VOP_POST_BG_GREEN_SHIFT) | \
   (((uint32_t)(b)&0x3ffu) << RK3576_VOP_POST_BG_BLUE_SHIFT))

/* POST_COLOR_CTRL (POST base + 0x0008) field definitions -- *** THE POST
 * LINE-BUFFER URGENCY MECHANISM, WHICH THIS DRIVER NEVER ENABLED ***.
 *
 *   bit8    sw_urgency_en    "1'b0: Disable  1'b1: Enable"
 *   [19:16] sw_urgency_thl   "When post_lb < sw_urgency_thl, post set urgency
 *                             to 1'b1.  Increase the priority of the layer on
 *                             the AXI0 bus."
 *   [23:20] sw_urgency_thh   "When post_lb > sw_urgency_thh, post set urgency
 *                             to 1'b0.  Realse [sic] the priority of the layer
 *                             on the AXI0 bus."
 *
 * [31:24] is RO reserved, so there is NO hiword write mask here and a plain
 * write is correct -- unlike SYS_PORT_CTRL_IMD and friends.
 *
 * post_lb is the POST's line-buffer occupancy, so this is the hardware's own
 * anti-under-run loop: as the line buffer drains below thl the POST raises its
 * urgency line, and the SoC's DDR controller answers it by masking other
 * masters' requests for that port.  Without it the layer's fetches queue up
 * behind everything else and the buffer does not refill in time.
 *
 * The thresholds are the reference driver's for RK3576 video output 0:
 *
 *   static const struct vop_urgency rk3576_vp0_urgency = {
 *           .urgen_thl = 4,
 *           .urgen_thh = 6,
 *   };
 */

#define RK3576_VOP_POST_URGENCY_EN        (1u << 8)
#define RK3576_VOP_POST_URGENCY_THL_SHIFT 16
#define RK3576_VOP_POST_URGENCY_THL_MASK \
  (0xfu << RK3576_VOP_POST_URGENCY_THL_SHIFT)
#define RK3576_VOP_POST_URGENCY_THH_SHIFT 20
#define RK3576_VOP_POST_URGENCY_THH_MASK \
  (0xfu << RK3576_VOP_POST_URGENCY_THH_SHIFT)
#define RK3576_VOP_POST_URGENCY_THL_LINUX 4u
#define RK3576_VOP_POST_URGENCY_THH_LINUX 6u

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
#define RK3576_VOP_ESMART_CTRL1            0x0004 /* DMA control, AXI read IDs */
#define RK3576_VOP_ESMART_AXI_CTRL_IMD     0x0008 /* AXI / MMU control */
#define RK3576_VOP_ESMART_REGION0_CTRL     0x0010 /* Region0 config/enable */
#define RK3576_VOP_ESMART_REGION0_YRGB_MST 0x0014 /* Region0 fb start addr */
#define RK3576_VOP_ESMART_REGION0_CBCR_MST 0x0018 /* Region0 CbCr addr */
#define RK3576_VOP_ESMART_REGION0_VIR      0x001C /* Region0 virtual stride */
#define RK3576_VOP_ESMART_REGION0_ACT_INFO \
  0x0020 /* Region0 active (w-1,h-1) */
#define RK3576_VOP_ESMART_REGION0_DSP_INFO \
  0x0024 /* Region0 display (w-1,h-1) */
#define RK3576_VOP_ESMART_REGION0_DSP_OFF  0x0028 /* Region0 display offset */
#define RK3576_VOP_ESMART_REGION0_SCL_CTRL 0x0030 /* Region0 scale (0=off) */
#define RK3576_VOP_ESMART_REGION0_SCL_FACTOR_YRGB                          \
  0x0034                                                /* Scl factor YRGB \
                                                         */
#define RK3576_VOP_ESMART_REGION0_SCL_FACTOR_CBR 0x0038 /* Scl factor CbCr */
#define RK3576_VOP_ESMART_ALPHA_MAP              0x00D8 /* Region0 alpha map */

/* REGION0_SCL_CTRL: the filter modes the vendor always programs.
 *
 * The working dump reads 0x00000044, which by the vendor's field map is
 *
 *   [3:2] yrgb_hscl_filter_mode = 1
 *   [7:6] yrgb_vscl_filter_mode = 1
 *
 * The vendor driver sets those from the window's static filter-mode properties
 * rather than from the computed scale mode, so they are non-zero even on a 1:1
 * layer.  Writing SCL_CTRL = 0, as this driver did, is a value the vendor
 * driver never produces.
 *
 * (bit20, yrgb_anei_en, is 0 in the working dump as well -- which
 * independently confirms that the earlier attempt to set it was chasing a bit
 * that is not programmable on this silicon.)
 */

#define RK3576_VOP_ESMART_SCL_HSCL_FILTER_SHIFT 2
#define RK3576_VOP_ESMART_SCL_VSCL_FILTER_SHIFT 6
#define RK3576_VOP_ESMART_SCL_FILTER_MODES_DEFAULT   \
  ((1u << RK3576_VOP_ESMART_SCL_HSCL_FILTER_SHIFT) | \
   (1u << RK3576_VOP_ESMART_SCL_VSCL_FILTER_SHIFT))

/* REGION0_SCL_CTRL bit20 = region0_yrgb_anei_en.
 *
 * *** THE REFERENCE DRIVER SETS THIS UNCONDITIONALLY ON RK3572 AND LATER, AND
 * THIS DRIVER WROTE THE WHOLE REGISTER AS ZERO. ***
 *
 *   rockchip_drm_vop2.c, in the window's scaler setup:
 *
 *     if (vop2->version >= VOP_VERSION_RK3572)
 *             VOP_SCL_SET(vop2, win, yrgb_anei_en, 1);
 *
 * and the field is declared in RK3576's own window table as
 *
 *     .yrgb_anei_en = VOP_REG(RK3568_ESMART0_REGION0_SCL_CTRL, 0x1, 20)
 *         (the vendor comment there reads: supported from rk3572)
 *
 * *** RETRACTED CLAIM, KEPT ON RECORD BECAUSE IT IS THIS PROJECT'S MOST
 * COMMON FAILURE: INFERRING A REGISTER VALUE FROM SOURCE INSTEAD OF FROM
 * WORKING SILICON. ***
 *
 * Note where that assignment sits: it is NOT inside the scaling-mode branch.
 * Every other SCL_CTRL field there is written from the computed scale modes,
 * so for a 1:1 layer they all come out 0 -- but yrgb_anei_en is set on its
 * own, for the whole SoC generation, whatever the scaling.  Writing SCL_CTRL =
 * 0, as this driver did, therefore produces a value the reference driver NEVER
 * produces on this silicon.
 *
 * The TRM lists bit20 inside "31:18 RO reserved" for this register, so it is
 * undocumented there -- but the vendor driver writes it as an ordinary RW bit
 * on exactly this generation, which is the stronger evidence.
 *
 * *** THE WORKING DUMP SAYS ALL OF THAT REASONING WAS WRONG.  IT WINS. ***
 *
 * A register dump from this same board, running a configuration that is known
 * to fetch and display, reads 0x00000044 for SCL_CTRL: filters 1, and bit20
 * ZERO. A working 1:1 window does NOT have yrgb_anei_en set.  Whatever version
 * of vop2_setup_scale() the copy being read here came from, this SoC's working
 * configuration is the ground truth, and it says 0.
 *
 * This driver wrote SCL_CTRL = 0x00100000, so it did two things wrong at once:
 * it SET bit20, and because it wrote the whole register it silently CLEARED
 * the two filter-mode fields that the working configuration has set.  The
 * second is the more dangerous kind of mistake -- the
 * plain-write-erases-reset-bits class
 * -- and it is the same mistake this driver made on REGION0_CTRL, where a
 * format write erased a reset bit22.  Both are now field writes.
 *
 * It matters because the scaler stage sits between the window's DMA/line
 * buffers and its output: it is the stage that decides when to ask the DMA for
 * data, and a scaler configured differently from the vendor's is a plausible
 * reason for a window that is enabled, routed and correct to issue no read at
 * all.
 *
 * "anei" names the scaler's anti-noise edge interpolation path, i.e. it sits
 * in the window's pixel pipeline between the DMA/line buffers and the output.
 * A window whose scaler stage is not configured the way the vendor configures
 * it is a window whose data path may never run, which is the failure this
 * whole bring-up has been chasing: the registers all read back correctly and
 * no read is ever issued.
 */

#define RK3576_VOP_ESMART_REGION0_SCL_ANE_I_EN (1u << 20)
#define RK3576_VOP_ESMART_PORT_SEL_IMD         0x00F4 /* Output video port sel */
#define RK3576_VOP_ESMART_DLY_NUM              0x00F8 /* Layer pipeline delay */

/* ESMART_DLY_NUM: [7:0] esmart_dly_num -- "Esmart delay cycle number" (TRM
 * 11.5.3.11 "ESMART0_ESMART_DLY_NUM", base 0x27D01800 + 0x00F8).
 *
 * It delays the layer's pixel stream relative to the mixing pipeline so the
 * three paths (window, background, overlay mix) meet at the right cycle.  The
 * reference driver computes it in vop2_calc_dly_num() and writes it per
 * window; this driver never wrote it at all.
 *
 * WHAT IT MUST BE, from that function's own arithmetic.  For a plain SDR layer
 * with no HDR, no CGC and no sdr2hdr -- which is this board -- every
 * intermediate delay is 0 and the branch that wins sets its own window delay
 * to 0, so:
 *
 *     sdr_win_dly = 0;  dly = sdr_win_dly;  win->dly_num = 0;
 *
 * which is exactly what the function's comment states: "If hdrvivid and
 * sdr2hdr is not work, the default bg_dly is 0x10.  and the default win delay
 * num is 0."
 *
 * THIS BOARD READ 0x17 (23) there -- and 23 extra cycles of layer delay is
 * precisely the kind of misalignment that starves the POST's input buffer at
 * the moment it expects pixels, which fits the two hard measurements: the
 * output-buffer under-run recurs with the layer enabled and stops the instant
 * the layer is disabled. */

#define RK3576_VOP_ESMART_DLY_NUM_SHIFT 0
#define RK3576_VOP_ESMART_DLY_NUM_MASK \
  (0xffu << RK3576_VOP_ESMART_DLY_NUM_SHIFT)

/* ESMART_CTRL0 field definitions.
 *
 * *** BIT31 IS A FRAME-RESET ENABLE, NOT A RESET RELEASE, AND MUST BE 0. ***
 *
 *   bit31  esmart_frm_resetn_en   reset 0x0   1'b0: Disable  1'b1: Enable
 *
 * An earlier version of this header described this bit as "the esmart layer is
 * held in reset until this bit is set ('1' = release)", and the driver duly
 * set it on every boot.  That is backwards, and it is the reason this bring-up
 * spent so long on a layer that read back perfectly and never fetched:
 *
 *   TRM 11.3.1, "Software Frame Reset":
 *     "The frame reset of each layer is supported, that is, when the vsync
 *      signal of each frame arrives, the layer's internal logic will be reset
 *      exclude register logic."
 *
 * So setting the bit ENABLES a per-vsync soft reset of the layer's internal
 * logic -- and the reset deliberately EXCLUDES the register logic, which is
 * exactly why every readback in this project has been correct while the layer
 * issued no memory accesses at all.
 *
 * Keep it 0.  The reference driver never writes ESMART_CTRL0 (no frm_resetn
 * handling exists in it), this bit's reset value is 0, and this board's own
 * bootloader left it 0 for all four ESMARTs.  esmart_scl_num ([13:12]) is 0
 * for ESMART0, so the whole register should simply be 0 for this window.
 *
 * RGB sources leave the color-space conversion fields at their defaults.
 */

#define RK3576_VOP_ESMART_CTRL0_FRM_RESETN_EN (1u << 31) /* MUST STAY 0 */

/* ESMART_CTRL1 (offset 0x0004) -- this window's read-request plumbing.
 *
 *   bit31    esmart_ymir_en              vertical mirror
 *   [30:29]  esmart_dma_rreq_thold       "If esmart empty lb number >=
 *                                        esmart_dma_rreq_thold, dma_rreq_hurry
 *                                        is asserted"
 *   bit28    esmart_dma_rreq_hurry_en    "Enable esmart dma hurry read
 * request" [27:24]  esmart_cbcr_gather_num      RO [23:20]
 * esmart_yrgb_gather_num      RO [16:12]  esmart_cbcr_rid             reset
 * 0x0b [8:4]    esmart_yrgb_rid             reset 0x0a bit3
 * esmart_cbcr_gather_en bit2     esmart_yrgb_gather_en [1:0]
 * esmart_esmart_axi_rlen      2'b00: Burst16  2'b01: Burst8 2'b10: Burst4
 * 2'b11: Reserved
 *
 * *** bit28 IS THE LAYER-SIDE HALF OF THE ANTI-STARVATION MECHANISM, AND THIS
 * DRIVER NEVER SET IT. ***  The POST raises an urgency signal when its line
 * buffer runs dry (POST_COLOR_CTRL), and the SYS block forwards that onto the
 * AXI arbitration (SYS_AXI_HURRY_CTRL0/1); both of those are now enabled. This
 * bit is the third leg, on the window itself: the ESMART asserts its own hurry
 * read request when its line buffers are empty.
 *
 * The reference driver leaves bit28 alone, but its own device-tree
 * documentation treats the ESMART line-buffer starvation path as something to
 * tune, and the measured symptom here IS starvation (POST_BUF_EMPTY recurs
 * with the layer enabled and stops when it is disabled).  With thold = 0 the
 * condition "empty lb number >= 0" holds whenever the window wants data, i.e.
 * a starving window asks for it at high priority instead of waiting its turn.
 */

#define RK3576_VOP_ESMART_CTRL1_DMA_RREQ_HURRY_EN    (1u << 28)
#define RK3576_VOP_ESMART_CTRL1_DMA_RREQ_THOLD_SHIFT 29
#define RK3576_VOP_ESMART_CTRL1_DMA_RREQ_THOLD_MASK \
  (0x3u << RK3576_VOP_ESMART_CTRL1_DMA_RREQ_THOLD_SHIFT)

/* ESMART_CTRL1 field definitions.
 *
 * BIT ORDER FROM THE TRM (Part 2, "ESMART0_ESMART_CTRL1", base 0x27D01800 +
 * 0x0004), corroborated by the reference driver's register table:
 *
 *   [23:20] esmart_yrgb_gather_num      RO
 *   [19:17] reserved                    RO
 *   [16:12] esmart_cbcr_rid             RW  reset 0x0b
 *   [11:9]  reserved                    RO
 *   [8:4]   esmart_yrgb_rid             RW  reset 0x0a
 *   bit3    esmart_cbcr_gather_en       RW
 *   bit2    esmart_yrgb_gather_en       RW
 *   [1:0]   esmart_esmart_axi_rlen      RW
 *
 * *** THESE IDs ARE NOT OPTIONAL AND THE RESET VALUES ARE WRONG FOR THIS
 * WINDOW. ***  The reference driver programs them per window
 * (rockchip_vop2_reg.c, rk3576_vop_win_data):
 *
 *   Esmart0 : axi_id 0 (axi0)  yrgb 0x10  uv 0x11   <- the window used here
 *   Esmart1 : axi_id 0 (axi0)  yrgb 0x12  uv 0x13
 *   Esmart2 : axi_id 1 (axi1)  yrgb 0x0a  uv 0x0b
 *   Esmart3 : axi_id 1 (axi1)  yrgb 0x0c  uv 0x0d
 *
 * The reset values read 0x0a/0x0b, which is Esmart2's pair -- and Esmart2
 * fetches on axi1, while Esmart0 fetches on axi0.  So by leaving this register
 * untouched, the window was issuing reads that declared themselves as a
 * different window on a different AXI port.  The ID travels with the read all
 * the way into the interconnect, where it decides routing and QoS, and the
 * failure mode of a bogus ID is a read that never comes back -- no data, no
 * bus error, and a POST whose output buffer is permanently empty.  That is
 * precisely this board's signature.
 */

#define RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT 4
#define RK3576_VOP_ESMART_CTRL1_YRGB_ID_MASK \
  (0x1fu << RK3576_VOP_ESMART_CTRL1_YRGB_ID_SHIFT)
#define RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT 12
#define RK3576_VOP_ESMART_CTRL1_UV_ID_MASK \
  (0x1fu << RK3576_VOP_ESMART_CTRL1_UV_ID_SHIFT)

/* ESMART_CTRL1 AXI read IDs.  *** THESE ARE PER-WINDOW ON RK3576. ***
 *
 *   Esmart0 : axi_id 0 (axi0)  yrgb 0x10  uv 0x11
 *   Esmart1 : axi_id 0 (axi0)  yrgb 0x12  uv 0x13
 *   Esmart2 : axi_id 1 (axi1)  yrgb 0x0a  uv 0x0b
 *   Esmart3 : axi_id 1 (axi1)  yrgb 0x0c  uv 0x0d
 *
 * Values from RK3576's own window table (Rockchip's vop2 driver,
 * rk3576_vop_win_data[]).  The reset values read 0x0a/0x0b, which is Esmart2's
 * pair, and Esmart2 fetches on axi1 while Esmart0 fetches on axi0.
 *
 * The ID travels with the read all the way into the interconnect, where it
 * decides routing and QoS, and the failure mode of a bogus ID is a read that
 * never comes back -- no data, no bus error, and a POST whose output buffer is
 * permanently empty.  That is precisely this board's signature.
 *
 * *** SO THE PAIR MUST FOLLOW THE WINDOW BEING DRIVEN. ***  The driver used to
 * hard-code ESMART0's pair while the window was selectable by
 * RK3576_VOP_ESMART_IDX, so selecting any other window issued reads declaring
 * themselves to be ESMART0 -- on ESMART0's channel.  The four pairs live in
 * g_rk3576_vop_esmart_yrgb_rid[] / _uv_rid[] / _axi_sel[] in rk3576_vop.c and
 * are indexed by that same constant; these macros remain only as the ESMART0
 * entry so the table stays readable.
 */

#define RK3576_VOP_ESMART0_AXI_YRGB_ID 0x10u
#define RK3576_VOP_ESMART0_AXI_UV_ID   0x11u

/* ESMART_AXI_CTRL_IMD field definitions.
 *
 * BIT ORDER TAKEN FROM THE TRM (Part 2, "ESMART0_ESMART_AXI_CTRL_IMD",
 * base 0x27D01800 + 0x0008):
 *
 *   bit16  esmart_dma_4k_addr_opt   RW  reset 1
 *   [15:9] reserved                 RO
 *   bit8   esmart_auto_gating_en    RW  reset 0
 *   [7:4]  esmart_outstanding_num   RW  reset 0
 *   bit3   esmart_outstanding_en    RW  reset 0
 *   bit2   esmart_mmu_bypass        RW  reset 0
 *   bit1   esmart_axi_sel           RW  reset 0   0 = axi0
 *   bit0   esmart_dma_sop           RW  reset 0
 *
 * *** The low bits are NOT where this header used to put them. ***  It had
 * outstanding_en at bit0, dma_sop at bit1, axi_sel at bit2 and mmu_bypass at
 * bit3 -- every field one position too low, which came from the same mistaken
 * reading of the SYS_AXI0_CTRL_IMD layout above.  Writing mmu_bypass therefore
 * actually set outstanding_en, and mmu_bypass was left at 0: a driver that
 * programs PHYSICAL addresses was letting them go through an MMU that has no
 * page tables.  Both are fixed here; the writes in rk3576_vop.c are unchanged
 * because they name the bits symbolically.
 *
 * Note bit0 (dma_sop) resets to 0 and the reference driver never writes it, so
 * it is left alone rather than guessed at.
 */

#define RK3576_VOP_ESMART_AXI_DMA_4K_ADDR_OPT \
  (1u << 16) /* 4k crossing opt               \
              */
#define RK3576_VOP_ESMART_AXI_AUTO_GATING_EN    (1u << 8)
#define RK3576_VOP_ESMART_AXI_OUTSTANDING_EN    (1u << 3)
#define RK3576_VOP_ESMART_AXI_MMU_BYPASS        (1u << 2) /* physical addr */
#define RK3576_VOP_ESMART_AXI_AXI_SEL           (1u << 1) /* 0=axi0 1=axi1 */
#define RK3576_VOP_ESMART_AXI_DMA_SOP           (1u << 0)
#define RK3576_VOP_ESMART_AXI_OUTSTANDING_SHIFT 4

/* NOTE the field is only FOUR BITS ([7:4], per the TRM), so the largest legal
 * value is 15 -- the macro below masks to the field, so passing 16 silently
 * writes 0 ("none outstanding"), the exact opposite of what such a call looks
 * like it means. */
#define RK3576_VOP_ESMART_AXI_OUTSTANDING_MASK \
  (0xf << RK3576_VOP_ESMART_AXI_OUTSTANDING_SHIFT)
#define RK3576_VOP_ESMART_AXI_OUTSTANDING(n)                    \
  (((uint32_t)(n) << RK3576_VOP_ESMART_AXI_OUTSTANDING_SHIFT) & \
   RK3576_VOP_ESMART_AXI_OUTSTANDING_MASK)

/* REGION0_CTRL (a.k.a. REGION0_MST_CTL, offset 0x0010) field definitions.
 *
 * Bit map from the TRM's detailed section ("ESMART0_REGION0_MST_CTL") and
 * cross-checked against the reference driver's table, which reads
 *   .rb_swap = VOP_REG(RK3568_ESMART0_REGION0_CTRL, 0x1, 14)
 *   .uv_swap = VOP_REG(RK3568_ESMART0_REGION0_CTRL, 0x1, 16)
 *   .rg_swap = VOP_REG(RK3568_ESMART0_REGION0_CTRL, 0x1, 18)
 *   .dither_up = VOP_REG(RK3568_ESMART0_REGION0_CTRL, 0x1, 12)
 *   .format_argb1555 = VOP_REG(RK3568_ESMART0_REGION0_CTRL, 0x1, 7)
 *   .format = VOP_REG(RK3568_ESMART0_REGION0_CTRL, 0x1f, 1)
 *
 *   bit0    region0_mst_en      Region enable
 *   [5:1]   region0_data_fmt    1 = RGB888
 *   bit7    region0_argb5551_en
 *   [10:8]  yrgb/cbcr 2gt/4gt prescale
 *   bit12   region0_dither_up_en
 *   bit14   region0_rb_swap      1'b0: RGB   1'b1: BGR
 *   bit16   region0_uv_swap
 *   bit18   region0_rg_swap
 *
 * *** rb_swap IS bit14, NOT bit10. ***  This header had it at bit10, which the
 * TRM assigns to region0_cbcr_2gt, so setting it would have mis-programmed a
 * prescale flag instead of doing anything to the colour order.
 *
 * AND THE TRM'S OWN SWAP TABLE (11.3, "Swap configuration table in esmart
 * default format") REQUIRES IT SET FOR RGB888:
 *
 *   Format    rb_swap  uv_swap
 *   ARGB         1        0
 *   RGB888       1        0
 *   RGB565       0        0
 *
 * i.e. with RGB888 the layer expects BGR byte order and leaves rb_swap at 0
 * when the framebuffer is R,G,B.  Red and blue are then exchanged on the glass
 * -- which a whole-screen white or black cannot reveal, because swapping equal
 * bytes changes nothing.
 */

#define RK3576_VOP_ESMART_REGION0_MST_EN    (1u << 0) /* Region enable */
#define RK3576_VOP_ESMART_REGION0_FMT_SHIFT 1         /* Pixel format */
#define RK3576_VOP_ESMART_REGION0_FMT_MASK \
  (0x1fu << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)
#define RK3576_VOP_ESMART_REGION0_DITHER_UP (1u << 12)
#define RK3576_VOP_ESMART_REGION0_RB_SWAP   (1u << 14) /* 0=RGB 1=BGR */
#define RK3576_VOP_ESMART_REGION0_UV_SWAP   (1u << 16)
#define RK3576_VOP_ESMART_REGION0_RG_SWAP   (1u << 18)

/* region0_data_fmt values (REGION0_CTRL[5:1]). */

#define RK3576_VOP_ESMART_FMT_ARGB8888 \
  (0x00 << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)
#define RK3576_VOP_ESMART_FMT_RGB888 \
  (0x01 << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)
#define RK3576_VOP_ESMART_FMT_RGB565 \
  (0x02 << RK3576_VOP_ESMART_REGION0_FMT_SHIFT)

/* REGION0_VIR: line stride in words.
 *   ARGB8888: vir_width; RGB888: (w*3/4)+(w%3); RGB565: ceil(w/2)
 */

/* ESMART_PORT_SEL_IMD: 2-bit video-port select. */

#define RK3576_VOP_ESMART_PORT_SEL_SHIFT 0
#define RK3576_VOP_ESMART_PORT_SEL_MASK \
  (0x3 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)
#define RK3576_VOP_ESMART_PORT_VP0 (0x0 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)
#define RK3576_VOP_ESMART_PORT_VP1 (0x1 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)
#define RK3576_VOP_ESMART_PORT_VP2 (0x2 << RK3576_VOP_ESMART_PORT_SEL_SHIFT)

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
