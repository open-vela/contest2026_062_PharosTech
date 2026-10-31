/****************************************************************************
 * chips/rk3576/vepu/vepu510_regs.h
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
 * RK3576 VEPU510 register *value* generation.
 *
 * The register *layout* lives in hardware/rk3576_vepu510_reg.h (generated).
 * This module decides what to put in it for one frame.
 *
 * Reference: Rockchip MPP, mpp/hal/rkenc/h264e/hal_h264e_vepu510.c
 * (Apache-2.0).  The functions here
 * mirror MPP's setup_vepu510_* chain, restricted to what this tree needs:
 * H.264, all-intra, fixed QP, no B frames, no ROI/OSD/SVC/dual-core.
 *
 * Scope note: this file covers the parts that can be checked exactly
 * against MPP's own code on the host:
 * the control block, the source-format block, the buffer addressing, the
 * codec (syntax) block and the fixed-QP rate-control block.  The remaining
 * blocks -- AQ, anti-artefact, scaling list, split, motion estimation -- are
 * the next increment.  Each function states which MPP function it mirrors,
 * so the remaining ones can be added the same way and verified the same way.
 *
 * All functions expect *regs to have been zeroed first, exactly as MPP's
 * hal_h264e_vepu510_gen_regs() does with memset(regs, 0, sizeof(*regs)).
 * They only assign the fields they own, so the order in
 * rk3576_vepu510_regs_build() does not matter and later blocks can add to
 * the same register set.
 *
 * ---------------------------------------------------------------------------
 * Derived from Rockchip MPP (Rockchip Media Process Platform),
 * https://github.com/rockchip-linux/mpp, branch develop, commit
 * 14729dd578e570e5f00fd1dd2113f5429012d64b, Apache-2.0:
 *
 *   mpp/hal/rkenc/h264e/hal_h264e_vepu510.c
 *   mpp/hal/rkenc/common/vepu510_common.h
 *
 * Copyright (c) 2024-2026 Rockchip Electronics Co., Ltd.
 *
 * This is a modified derivative, not a copy: the upstream code was reduced,
 * rewritten and reorganised for the NuttX kernel build, and the interfaces
 * here are this driver's rather than MPP's.  The register words it produces
 * have been byte-compared against the upstream functions on the host.
 * ---------------------------------------------------------------------------
 *
 ****************************************************************************/

#ifndef __CHIPS_RK3576_VEPU_VEPU510_REGS_H
#define __CHIPS_RK3576_VEPU_VEPU510_REGS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/* For FAR: this header names it in the prototypes below, so it must pull the
 * definition in itself rather than rely on the includer's order.
 */

#include <nuttx/compiler.h>

#include "hardware/rk3576_vepu510_reg.h"

/* The codec block is programmed from the same configuration the parameter
 * sets are generated from, so that the two cannot disagree about, for
 * example, whether entropy coding is CABAC.
 */

#include "h264_syntax.h"

/* The tuning tables, and the constants that bound them. */

#include "vepu510_tables.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The status block's raw words, which the encoder writes when a job finishes.
 *
 * These are offsets into a register file rather than members of a struct
 * because neither figure worth reading fits in one register: the bitstream
 * length's top eight bits share a register with the distortion figure's low
 * half, and the distortion figure's high half is the next register along.
 * The modelled Vepu510Status has all of them, but as bitfields -- and a
 * bitfield has no offset, so there is nothing for a header to point at.
 *
 * The low half of the length is the whole of it here: a buffer large enough
 * to need the other eight bits would be 4 GB, which this driver cannot be
 * given.  MPP reads it the same way.
 *
 * What the split means for a reader is spelled out at
 * rk3576_vepu510_status_decode(), which is the only thing that should be
 * assembling these.
 */

#define RK3576_VEPU510_ST_BS_LGTH_OFFSET 0x4000
#define RK3576_VEPU510_ST_SSE_LOW_OFFSET 0x4004 /* sse_l16 in bits 31:16 */

/* The smear counts the encoder reports for the picture it just encoded, as
 * four eight-bit fields in one word.  This is the half of the status block
 * that feeds back into the *next* picture's registers -- see
 * rk3576_vepu510_status_smear() -- and it is read like the rest of the status
 * block, from task context after the job has finished.
 */

#define RK3576_VEPU510_ST_SMEAR_CNT_OFFSET 0x40a4

/* What one step of a smear count is worth.  The encoder counts in units of
 * four macroblocks, so the status field is a quarter of the count the
 * anti-smear thresholds are expressed in and MPP multiplies it back.
 */

#define RK3576_VEPU510_ST_SMEAR_CNT_SCALE 4
#define RK3576_VEPU510_ST_SSE_HIGH_OFFSET 0x4008

/* How the picture was actually coded: the block counts the encoder reports
 * for the picture it just encoded, split by prediction mode and block size.
 *
 * This is the only place the choice between intra and inter coding of a
 * finished picture is visible.  A length says what the picture cost and a
 * distortion figure says how good it is, and neither distinguishes a picture
 * that predicted well from one that gave up and coded itself from the source
 * -- but the two have opposite causes, and a picture that costs an I
 * picture's worth of bits while claiming to be predicted is exactly the case
 * where that distinction is the whole question.  MPP reads the same fields
 * for its rate control (iblk4_prop is these counts over the macroblock
 * count).
 */

#define RK3576_VEPU510_ST_QP_SUM_OFFSET   0x400c
#define RK3576_VEPU510_ST_PNUM_P16_OFFSET 0x4088
#define RK3576_VEPU510_ST_PNUM_P8_OFFSET  0x408c
#define RK3576_VEPU510_ST_PNUM_I32_OFFSET 0x4090
#define RK3576_VEPU510_ST_PNUM_I16_OFFSET 0x4094
#define RK3576_VEPU510_ST_PNUM_I8_OFFSET  0x4098
#define RK3576_VEPU510_ST_PNUM_I4_OFFSET  0x409c
#define RK3576_VEPU510_ST_PNUM_MASK       0x1fffffu

#define RK3576_VEPU510_ST_SSE_LOW_SHIFT   16
#define RK3576_VEPU510_ST_SSE_LOW_MASK    0xffffu
#define RK3576_VEPU510_ST_SSE_HIGH_SHIFT  16

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Source pixel format codes, as the VEPU510 wants them in
 * reg_frm.src_fmt.src_cfmt.  These are VepuFmt enumerators from the MPP
 * register model -- note the numbering is *not* the V4L2 fourcc order and
 * not contiguous for the formats we care about.
 */

enum rk3576_vepu510_src_fmt_e
{
  RK3576_VEPU510_FMT_BGRA8888 = 0,
  RK3576_VEPU510_FMT_BGR888 = 1,
  RK3576_VEPU510_FMT_BGR565 = 2,
  RK3576_VEPU510_FMT_ARGB1555 = 3,
  RK3576_VEPU510_FMT_YUV422SP = 4,
  RK3576_VEPU510_FMT_YUV422P = 5,
  RK3576_VEPU510_FMT_YUV420SP = 6, /* NV12 / NV21 */
  RK3576_VEPU510_FMT_YUV420P = 7,
  RK3576_VEPU510_FMT_YUYV422 = 8,
  RK3576_VEPU510_FMT_UYVY422 = 9,
  RK3576_VEPU510_FMT_YUV400 = 10,
  RK3576_VEPU510_FMT_AYUV2BPP = 11,
  RK3576_VEPU510_FMT_YUV444SP = 12,
  RK3576_VEPU510_FMT_YUV444P = 13,
  RK3576_VEPU510_FMT_ARGB4444 = 14,
  RK3576_VEPU510_FMT_AYUV1BPP = 15,
};

/* The reconstruction working set.
 *
 * The encoder does not just emit a bitstream: it also reconstructs each
 * picture, because that reconstruction is what a later picture would use as
 * a reference.  The pixels it writes there are addressed by rfpw_h_addr and
 * rfpw_b_addr, and the IP writes them whether or not anything will ever read
 * them -- so the buffers have to exist even for an all-intra stream, and
 * leaving the registers at zero points the write at physical address 0.
 *
 * MPP allocates this set unconditionally, which is the only behaviour proven
 * to work, and it is what this driver does too.  It is deliberately not
 * optional, and regs_recn() rejects a frame that does not carry it.
 *
 * The pixel buffer is a frame buffer compression (FBC) buffer: a header
 * followed by the compressed body.  The encoder expects the header's address
 * in one register and the body's in the next, which is why the body offset
 * travels alongside the base rather than being recomputed by whoever sets
 * the registers.
 *
 * One struct describes one such set, and a frame names two of them: the set
 * it writes and, when it predicts from a decoded picture, the set it reads.
 * The two are separate registers, so a picture never reads the set it is
 * writing -- the reference has to be a set a previous picture left behind.
 */
struct rk3576_vepu510_recn_s
{
  uint32_t pixel_phys;  /* reconstruction pixel buffer, FBC header   */
  uint32_t body_offset; /* where the FBC body starts inside it      */
  uint32_t thumb_phys;  /* downscaled copy of the reconstruction    */
  uint32_t smear_phys;  /* anti-smear working area                  */
};

/* The sizes that set needs, derived from the picture geometry.
 *
 * Exposed because the driver has to allocate the buffers before it can
 * describe them, and the arithmetic belongs with the rest of the IP's
 * geometry rules rather than in the allocator.
 */

struct rk3576_vepu510_recn_size_s
{
  uint32_t header; /* size of the FBC header, and the body offset  */
  uint32_t pixel;  /* header plus body                             */
  uint32_t thumb;  /* thumbnail buffer                             */
  uint32_t smear;  /* anti-smear buffer                            */
};

/* What one encoded picture reported about itself, which the *next* picture's
 * anti-smear thresholds are derived from.
 *
 * The numbers are the encoder's own account of how much of the picture it had
 * to refresh, so they are only available once the picture has been encoded:
 * this is the one part of the register image that comes from a status read
 * rather than from configuration.
 *
 * frame_type uses H.264's slice-type numbering (0 for P, 2 for I) because
 * that is the value MPP compares, and the comparison is against the constant
 * RK3576_H264_SLICE_I.  A local enum with a different order would compare
 * equal to P pictures whenever it happened to line up numerically.
 */

struct rk3576_vepu510_feedback_s
{
  uint32_t frame_type;   /* RK3576_H264_SLICE_*                   */
  uint32_t mb_num;       /* macroblocks in that picture         */
  uint32_t smear_cnt[5]; /* four counts, and their sum in [4]   */
};

struct rk3576_vepu510_frame_s
{
  /* Source (input) frame. */
  uint32_t src_fmt;   /* enum rk3576_vepu510_src_fmt_e              */
  uint32_t rbuv_swap; /* 1 to swap the chroma order of a semi-planar
                       * format: NV21 needs 1, NV12 needs 0.  The
                       * hardware format code is the same for both, so
                       * this bit is what distinguishes them.
                       */
  uint32_t width;     /* picture width in pixels                    */
  uint32_t height;    /* picture height in pixels                   */
  uint32_t y_stride;  /* luma stride in bytes                       */
  uint32_t v_stride;  /* allocated height in rows                   */
  uint32_t src_phys;  /* physical address of the luma plane         */
  uint32_t src_size;  /* bytes in the source buffer, all planes
                       * together.
                       *
                       * The encoder does not read this: the chroma
                       * plane offsets come from the strides, as MPP
                       * computes them.  The driver needs it for
                       * something the register layer has no business
                       * knowing about -- flushing the CPU's cache over
                       * the whole picture before the DMA reads it.  It
                       * is therefore part of describing a buffer rather
                       * than part of programming the encoder, and is
                       * deliberately not used by regs_addr().        */

  /* Destination (bitstream) buffer. */

  uint32_t dst_phys;   /* physical address of the bitstream buffer   */
  uint32_t dst_size;   /* bitstream buffer size in bytes             */
  uint32_t dst_offset; /* bytes of the buffer already consumed       */

  /* Reconstruction working set.  Required: see struct
   * rk3576_vepu510_recn_s.  Size it with rk3576_vepu510_recn_size().
   */

  struct rk3576_vepu510_recn_s recn;

  /* Reconstruction working set of the picture this one predicts from, as a
   * set of the same shape.  Consulted only when ref_valid is set.
   *
   * The caller owns the ordering rule: the set written here must be one
   * that a previous call left behind, that nothing is currently writing to,
   * and that the encoder is not still reading.  The register layer cannot
   * check any of that -- it sees one frame at a time -- so a mismatch shows
   * up as a corrupted picture rather than as an error.
   */

  struct rk3576_vepu510_recn_s ref;

  bool ref_valid; /* 1 when ref describes a decoded picture to
                   * predict from.  Zero for the first picture of
                   * a stream and for every IDR: an IDR is decoded
                   * without reference to anything, and the
                   * hardware must not be pointed at a stale set
                   * for it.
                   */

  /* What the picture before this one reported about itself.
   *
   * Nothing is derived from it unless the anti-smear thresholds are
   * programmed, and those are programmed for every picture -- so this has to
   * be supplied even for an IDR, and even for the first picture of a stream,
   * where it is a zeroed struct.  That is not an invented "no previous
   * picture" state: it is what MPP's own context holds on its first task,
   * because it copies its feedback into the previous slot before every job
   * and never special-cases the first.
   */

  struct rk3576_vepu510_feedback_s prev;
};

/* One coded picture, as the hardware needs it described.
 *
 * This was an IDR descriptor while every picture was one.  It is a slice
 * descriptor now, because the two differ in three fields and only the caller
 * knows which it is making: the NAL unit type and reference indicator, the
 * slice type, and whether the per-picture syntax starts over.
 *
 * The values the caller supplies are the ones the hardware writes into the
 * slice header it synthesises.  They are not checked against each other
 * here: an IDR with a non-zero frame_num is a contradiction, and it is the
 * caller's business not to make one, because only the caller knows the
 * picture's place in its group.
 */

struct rk3576_vepu510_slice_s
{
  bool idr; /* 1 for an IDR, 0 for a P picture */

  /* The picture's own syntax.  An IDR resets both, so they are zero for it
   * and only the caller can supply the sequence.
   */

  uint32_t frame_num; /* frame_num, 0..max_frame_num-1  */
  uint32_t poc_lsb;   /* pic_order_cnt_lsb              */

  uint32_t idr_pic_id; /* only written for an IDR, and
                        * only meaningful there          */

  uint32_t deblock_disable;          /* disable_deblocking_filter_idc */
  uint32_t deblock_offset_alpha;     /* slice_alpha_c0_offset_div2  */
  uint32_t cabac_init_idc;           /* used only when entropy coding
                                      * is CABAC */
  uint32_t long_term_reference_flag; /* an IDR is the only picture
                                      * that can say this        */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vepu510_frame_num_mask, rk3576_vepu510_poc_lsb_mask
 *
 * Description:
 *   The masks that reduce frame_num and pic_order_cnt_lsb to the ranges the
 *   configured syntax declares.
 *
 *   These are derived here, from the same cfg the slice header is built from,
 *   because the SPS tells the decoder what the ranges are: a driver that
 *   wrapped somewhere else would be signalling one range and using another.
 *   The disagreement would first appear as a decoding failure after a wrap,
 *   which at sixteen bits is hours into a stream and so is never in a test --
 *   which is why the arithmetic lives where it can be checked against the
 *   reference rather than next to its only caller.
 *
 *   Both ranges are a power of two wider than the four bits the SPS field
 *   is short by, and both fit the sixteen-bit register fields they are
 *   written into.
 *
 ****************************************************************************/

uint32_t
rk3576_vepu510_frame_num_mask(FAR const struct rk3576_h264_cfg_s *cfg);
uint32_t rk3576_vepu510_poc_lsb_mask(FAR const struct rk3576_h264_cfg_s *cfg);

/****************************************************************************
 * Name: rk3576_vepu510_regs_ctl
 *
 * Description:
 *   Fill the control (CTL) block: start command, clock/reset enables,
 *   interrupt enables and masks, watchdog and the data transfer map.
 *
 *   Mirrors MPP setup_vepu510_normal().
 *
 ****************************************************************************/

void rk3576_vepu510_regs_ctl(FAR HalVepu510RegSet *regs);

/****************************************************************************
 * Name: rk3576_vepu510_regs_src
 *
 * Description:
 *   Fill the source-format fields of the frame (FRAME) block: picture size
 *   rounded up to the 16-pixel macroblock grid, the fill counts that tell
 *   the hardware how much of the rounded picture is padding, source format,
 *   plane strides and the source processing flags.
 *
 *   Mirrors the non-RGB path of MPP setup_vepu510_prep().  RGB input (which
 *   would need the RGB-to-YUV conversion matrix) is rejected: this tree
 *   feeds the encoder NV12 straight from the camera path.
 *
 *   The caller sets frm->rbuv_swap -- for a semi-planar format the hardware
 *   format code is the same for NV12 and NV21, and that bit is the only
 *   thing that distinguishes them.
 *
 * Returned Value:
 *   OK on success, -ENOSYS for an unsupported source format.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_src(FAR HalVepu510RegSet *regs,
                            FAR const struct rk3576_vepu510_frame_s *frm);

/****************************************************************************
 * Name: rk3576_vepu510_regs_addr
 *
 * Description:
 *   Fill the source and destination buffer addresses of the FRAME block.
 *
 *   MPP passes dma-buf file descriptors and then patches the per-plane
 *   offsets in through its "multi offset" ioctl, because the kernel adds
 *   them to the value in the register.  This tree bypasses the VEPU0 MMU
 *   (like rk3576_vicap.c does) and programs physical addresses, so the
 *   plane offsets are added here and the final addresses written directly.
 *
 *   Calculated from the same plane-offset rules as MPP
 *   setup_vepu510_io_buf().
 *
 * Returned Value:
 *   OK on success, -ENOSYS for an unsupported source format.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_addr(FAR HalVepu510RegSet *regs,
                             FAR const struct rk3576_vepu510_frame_s *frm);

/****************************************************************************
 * Name: rk3576_vepu510_recn_size
 *
 * Description:
 *   Work out how much memory the reconstruction working set needs for one
 *   picture of this size.
 *
 *   Mirrors MPP setup_hal_bufs(), including the extra 16 rows it adds to the
 *   aligned height and the fact that every size is rounded up to 8 KB.  The
 *   numbers look arbitrary -- 16 rows, a 64th of the aligned area for the
 *   FBC header -- but they are the IP's, so they are reproduced rather than
 *   derived.
 *
 ****************************************************************************/

void rk3576_vepu510_recn_size(uint32_t width, uint32_t height,
                              FAR struct rk3576_vepu510_recn_size_s *size);

/****************************************************************************
 * Name: rk3576_vepu510_regs_recn
 *
 * Description:
 *   Point the encoder at the reconstruction working set.
 *
 *   Mandatory rather than best-effort: a frame without it is rejected, and
 *   that is the point.  The registers have no "disabled" setting -- zero
 *   means physical address zero -- so a frame that quietly omitted the
 *   buffers would send the reconstruction write to the start of memory
 *   instead of failing.
 *
 *   Mirrors MPP setup_vepu510_recn_refr(), with its multi-offset additions
 *   folded in: MPP points both registers at the FBC header and then adds the
 *   header size to the body register, and since the kernel's offset
 *   mechanism adds rather than assigns, the result is the header address and
 *   the body address.
 *
 * Returned Value:
 *   OK on success, -EINVAL if the working set is not described.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_recn(FAR HalVepu510RegSet *regs,
                             FAR const struct rk3576_vepu510_frame_s *frm);

/****************************************************************************
 * Name: rk3576_vepu510_regs_codec
 *
 * Description:
 *   Fill the codec (syntax) fields: the NAL header, the SPS and PPS values
 *   the hardware echoes into the slice header it synthesizes, and the slice
 *   header fields themselves.
 *
 *   Mirrors MPP setup_vepu510_codec(), restricted to the IDR I-slice path.
 *   MPP's reorder and reference-marking machinery runs in that function too,
 *   but a single-frame all-intra stream has an empty reorder queue and an
 *   empty marking list, so both collapse to the constant zeroes written
 *   here -- see the comments in the implementation.
 *
 *   Not programmed here: enc_pic.pic_qp, the per-frame quantiser.  MPP sets
 *   it in setup_vepu510_rc_base(), so it belongs with the rate-control
 *   block; leaving it at zero would be a real quality problem, which is why
 *   this is spelled out rather than left implicit.
 *
 *   The caller must have zeroed *regs first, as rk3576_vepu510_regs_build()
 *   does.
 *
 * Input Parameters:
 *   regs - register set to fill
 *   cfg  - encoder syntax configuration, the same struct the SPS and PPS
 *          writers take
 *   slice - slice-level values for this picture
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_codec(FAR HalVepu510RegSet *regs,
                              FAR const struct rk3576_h264_cfg_s *cfg,
                              FAR const struct rk3576_vepu510_slice_s *slice);

/****************************************************************************
 * Name: rk3576_vepu510_regs_rc_fixqp
 *
 * Description:
 *   Fill the rate-control block for fixed-QP operation: the ROI quantiser
 *   thresholds and the per-frame QP, with rate control left disabled.
 *
 *   Mirrors the FIXQP path of MPP setup_vepu510_rc_base().  That function
 *   programs a rate-control configuration and then returns early when
 *   rc_mode is FIXQP; everything past the early return -- the rate-control
 *   enable, the per-CTU bit target, the QP-adaptation step tables -- belongs
 *   to the rate-controlled modes, which this driver does not implement.
 *   Returning early is what "fixed QP" means: nothing adapts the QP, so
 *   enabling the rate-control machinery would only add a target that cannot
 *   be met.
 *
 *   All eight ROI areas get the same min/max pair.  In FIXQP MPP defaults
 *   quality_min and quality_max to the target QP (rc_model_v2.c, RC_FIXQP
 *   branch), giving a degenerate range, and that is what is programmed here.
 *   A wider range would only matter if regions could have different QPs,
 *   which needs the ROI configuration this driver does not expose.
 *
 *   The caller must have zeroed *regs first, as rk3576_vepu510_regs_build()
 *   does.
 *
 * Input Parameters:
 *   regs - register set to fill
 *   qp   - quantisation parameter, 0..RK3576_H264_QP_MAX
 *
 * Returned Value:
 *   OK on success, -EINVAL if qp is out of range.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_rc_fixqp(FAR HalVepu510RegSet *regs, uint32_t qp);

/****************************************************************************
 * Name: rk3576_vepu510_regs_rdo_pred
 *
 * Description:
 *   Fill the RDO (rate-distortion optimisation) configuration: which coding
 *   tools the search may use, and the chroma lambda offset.
 *
 *   These are not optional decoration.  Every rdo_cfg field defaults to
 *   zero, which turns the corresponding tool *off*: leaving this block
 *   unprogrammed silently produces a much worse picture, with no error and
 *   no obvious symptom beyond a larger bitstream at the same QP.
 *
 *   Mirrors MPP setup_vepu510_rdo_pred().
 *
 * Input Parameters:
 *   regs     - register set to fill
 *   cfg      - encoder syntax configuration
 *   slice_is_i - true for an I slice.  This driver encodes all-intra, so it
 *              is always true here; it is a parameter so the harness can
 *              verify the other branch, and so adding inter support later
 *              does not silently need new code.
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_rdo_pred(FAR HalVepu510RegSet *regs,
                                 FAR const struct rk3576_h264_cfg_s *cfg,
                                 bool slice_is_i);

/****************************************************************************
 * Name: rk3576_vepu510_regs_anti_stripe
 *
 * Description:
 *   Fill the intra-prediction deblocking thresholds and weights that
 *   suppress stripe artefacts along block edges.
 *
 *   Note that in the default (non-IPC) scene mode MPP sets iprd_tthd_ul to
 *   4095, whose comment reads "disable anti-stripe": the thresholds are
 *   still programmed, but the feature is switched off.  This mirrors that
 *   rather than deciding differently, so the registers match MPP in both
 *   scene modes.
 *
 *   Mirrors MPP setup_vepu510_anti_stripe().
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_anti_stripe(FAR HalVepu510RegSet *regs,
                                    FAR const struct rk3576_h264_cfg_s *cfg);

/****************************************************************************
 * Name: rk3576_vepu510_regs_anti_ringing
 *
 * Description:
 *   Fill the anti-ringing thresholds and weights.
 *
 *   Only the scene mode and the slice type select values here -- the
 *   configured anti-ringing strength (atr_str_i) does *not* appear, in this
 *   function or anywhere else in the register programming.  MPP uses it only
 *   as an on/off flag in rdo_cfg.atr_e, so the "strength" is really a switch
 *   and the weights are fixed tables.  That is an upstream quirk, reproduced
 *   deliberately: inventing a strength-dependent table here would produce a
 *   configuration MPP never generates and that nothing has been tuned for.
 *
 *   Mirrors MPP setup_vepu510_anti_ringing().
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_anti_ringing(FAR HalVepu510RegSet *regs,
                                     FAR const struct rk3576_h264_cfg_s *cfg,
                                     bool slice_is_i);

/****************************************************************************
 * Name: rk3576_vepu510_regs_anti_flicker
 *
 * Description:
 *   Fill the anti-flicker thresholds and weights for the three block types
 *   (skip, inter, intra).
 *
 *   All three are programmed even for an all-intra stream, because MPP
 *   programs them unconditionally and only the intra set can ever be used.
 *   Programming just the intra set would leave the other two at zero, which
 *   is not what MPP produces; matching MPP keeps the register image
 *   comparable.
 *
 *   This is the anti-flicker part of the SQI block.  The anti-smear part --
 *   smear_opt_cfg, smear_madp_thd*, smear_cnt_thd*, smear_resi_thd* and
 *   smear_st_thd -- is a separate block with a separate entry point, because
 *   it is derived from what the previous picture reported rather than from
 *   configuration; see rk3576_vepu510_regs_anti_smear().  The two share the
 *   block and write disjoint fields.
 *
 *   Mirrors MPP setup_vepu510_anti_flicker(), and only that.
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable (an out-of-range strength
 *   would read past the end of MPP's tuning tables).
 *
 ****************************************************************************/

int rk3576_vepu510_regs_anti_flicker(FAR HalVepu510RegSet *regs,
                                     FAR const struct rk3576_h264_cfg_s *cfg);

/****************************************************************************
 * Name: rk3576_vepu510_regs_anti_smear
 *
 * Description:
 *   Fill the SQI block's anti-smear thresholds, weights and quantiser delta.
 *
 *   Most of this is mirrored from MPP's setup_vepu510_anti_smear() -- every
 *   threshold, every weight, and the branch that the deblur tuning selects
 *   between two threshold sets.  Three fields are deliberately not:
 *   stated_mode, rdo_smear_dlt_qp and rdo_smear_lvl16_multi are written with
 *   the constants MPP 1.0.6 used, not the values develop derives from the
 *   previous picture's status word.
 *
 *   Why is measured rather than reasoned.  A P picture came back differing
 *   from its source by exactly the reference's own local mean, in every
 *   plane, on every picture that read a reference -- a whole-reference error
 *   from a mechanism that acts on the reference as a whole.  Porting
 *   develop's block and then writing 1.0.6's constants for those three
 *   fields is what removed it: the leak fell from 38.2 to 0.14 in luma and
 *   from 59.5 to 0.02 in chroma, against 0.17 and 0.012 for MPP itself on
 *   the same source, while the I pictures stayed bit-identical.  Two other
 *   switches that moved in the same build were excluded afterwards by a run
 *   that already held them fixed.
 *
 *   The three fields are constant in 1.0.6 because its setup_vepu510_sqi()
 *   takes the register block and nothing else -- it has no state to be in.
 *   develop gave the block the previous picture's status and made these
 *   derive from it, and on this silicon that derivation describes the
 *   reference wrongly.  It is worth being explicit about the consequence:
 *   the values 1.0.6 programs cannot be reached from the code below under
 *   any configuration, so this is a difference between two MPP versions
 *   rather than a setting this driver got wrong.  The board runs 1.0.6.
 *
 *   Nothing else in the block is state dependent.  The thresholds turn on
 *   the deblur tuning alone, and the counts that select between threshold
 *   sets reach the hardware, in this configuration, through the quantiser
 *   delta alone -- which is why that one field is the whole of the
 *   behavioural change and the other two are consistency.
 *
 *   The smear counts themselves are read correctly: the status word is at
 *   0x40a4, which is where MPP's own Vepu510Status puts st_smear_cnt.  What
 *   was wrong was what develop does with it here.
 *
 * Input Parameters:
 *   regs        - register set to fill
 *   cfg         - encoder syntax configuration, for the deblur tuning
 *   slice_is_i  - true when the picture being programmed is an I slice
 *   prev        - what the previous picture reported; zeroed on the first
 *                 picture of a stream, as MPP's own context is.  Still read,
 *                 because the thresholds that do not depend on it are
 *                 computed alongside the ones that do.
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable or deblur_str is out of
 *   range.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_anti_smear(
    FAR HalVepu510RegSet *regs, FAR const struct rk3576_h264_cfg_s *cfg,
    bool slice_is_i, FAR const struct rk3576_vepu510_feedback_s *prev);

/****************************************************************************
 * Name: rk3576_vepu510_regs_l2
 *
 * Description:
 *   Fill the rate-distortion lambda table and the quantiser QP bias.
 *
 *   This block matters more than its size suggests.  The lambda table is the
 *   trade-off curve the encoder uses to choose between bits and distortion,
 *   so leaving it zeroed does not disable an optional feature -- it encodes
 *   with a lambda of zero, which pushes every decision towards distortion and
 *   visibly degrades the picture, with no error reported.
 *
 *   The table is selected by scene mode and offset by the lambda index, and
 *   52 of the table's 60 entries are copied from that offset.  The index is
 *   range checked because the copy would otherwise read past the end.
 *
 *   Mirrors MPP setup_vepu510_l2().
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_l2(FAR HalVepu510RegSet *regs,
                           FAR const struct rk3576_h264_cfg_s *cfg);

/****************************************************************************
 * Name: rk3576_vepu510_regs_aq
 *
 * Description:
 *   Fill the adaptive-quantisation thresholds and per-block-type QP steps.
 *
 *   MPP calls this unconditionally, so it is implemented even though the
 *   fixed-QP path leaves rate control off: the contract for this module is
 *   to produce the register image MPP produces, and MPP always writes these
 *   32 values.  Whether the hardware acts on them with rate control disabled
 *   is a separate question that only measurement can answer -- and it is not
 *   a reason to leave the registers at zero, which is a state MPP never
 *   produces.
 *
 *   MPP takes the values from a settable hardware configuration and copies
 *   defaults in at context init.  This driver has no control surface for
 *   them, so it always programs MPP's defaults; they live in
 *   vepu510_tables.c and are diffed against MPP's copies.
 *
 *   Mirrors MPP setup_vepu510_aq().
 *
 * Input Parameters:
 *   regs       - register set to fill
 *   slice_is_i - true for an I slice, which selects the step table
 *
 * Returned Value:
 *   OK.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_aq(FAR HalVepu510RegSet *regs, bool slice_is_i);

/****************************************************************************
 * Name: rk3576_vepu510_regs_me
 *
 * Description:
 *   Fill the motion-estimation search ranges, thresholds and weights.
 *
 *   Like the AQ block this is inter-prediction machinery that an all-intra
 *   stream does not use, and like it MPP programs it unconditionally.  It is
 *   implemented for the same reason: the goal is the register image MPP
 *   produces, not a hand-picked subset of it.
 *
 *   Only the scene mode varies anything here, and the non-IPC branch is not
 *   merely a lower setting -- it zeroes several thresholds and flattens the
 *   multi tables, in a block MPP labels "disable subjective optimization".
 *
 *   Mirrors MPP setup_vepu510_me().
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfg is not usable.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_me(FAR HalVepu510RegSet *regs,
                           FAR const struct rk3576_h264_cfg_s *cfg);

/****************************************************************************
 * Name: rk3576_vepu510_regs_build
 *
 * Description:
 *   Convenience: zero the register set and fill every block this module
 *   currently implements.
 *
 * Returned Value:
 *   OK on success, a negated errno otherwise.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_build(FAR HalVepu510RegSet *regs,
                              FAR const struct rk3576_vepu510_frame_s *frm,
                              FAR const struct rk3576_h264_cfg_s *cfg,
                              FAR const struct rk3576_vepu510_slice_s *slice);

/****************************************************************************
 * Name: rk3576_vepu510_status_decode
 *
 * Description:
 *   Turn the status block's raw words into the two numbers worth having: the
 *   bitstream length and the distortion figure.
 *
 *   This is the register layer's business for the same reason the encode
 *   direction is: it is a fixed mapping between the hardware's layout and a
 *   value, with no state and no I/O, so it can be checked on the host.  It
 *   is also the inverse of what the rest of this file does, which is where
 *   the one bug found by inspection so far lived -- the distortion figure is
 *   split across two registers and reading half of it yields a value that
 *   wraps rather than rounds, so "a bit wrong" is not among the possible
 *   outcomes.
 *
 *   The three raw words are passed separately rather than as a register set,
 *   because on the read side that is all there is: nothing else in the
 *   status block is used, and taking a whole HalVepu510RegSet here would
 *   invite reading fields that were never latched.
 *
 * Input Parameters:
 *   bs_lgth_l32 - status word at RK3576_VEPU510_ST_BS_LGTH_OFFSET
 *   sse_bsl     - status word at RK3576_VEPU510_ST_SSE_LOW_OFFSET
 *   sse_h32     - status word at RK3576_VEPU510_ST_SSE_HIGH_OFFSET
 *   bs_length   - receives the bitstream length in bytes; may be NULL
 *   sse         - receives the distortion figure; may be NULL
 *
 ****************************************************************************/

void rk3576_vepu510_status_decode(uint32_t bs_lgth_l32, uint32_t sse_bsl,
                                  uint32_t sse_h32, FAR uint32_t *bs_length,
                                  FAR uint32_t *sse);

/****************************************************************************
 * Name: rk3576_vepu510_status_smear
 *
 * Description:
 *   Turn the status block's smear-count register into the five counts the
 *   anti-smear block is programmed from, which is what the driver has to
 *   carry from one job to the next.
 *
 *   The register holds four eight-bit counts and the fifth entry is their
 *   sum, which is how MPP's own feedback arranges them.  The scaling by four
 *   is MPP's too, and it is here rather than at the call site because it is
 *   part of what the hardware's number means: the encoder counts in units of
 *   four macroblocks.
 *
 *   This is separate from rk3576_vepu510_status_decode() because it is read
 *   at a different time for a different purpose: the length and distortion
 *   are read once, to report the frame that just finished, while the counts
 *   are read so that the *next* frame can be programmed.
 *
 * Input Parameters:
 *   smear_cnt_reg - status word at RK3576_VEPU510_ST_SMEAR_CNT_OFFSET
 *   out           - receives five counts; must not be NULL
 *
 ****************************************************************************/

void rk3576_vepu510_status_smear(uint32_t smear_cnt_reg, FAR uint32_t *out);

#endif /* __CHIPS_RK3576_VEPU_VEPU510_REGS_H */
