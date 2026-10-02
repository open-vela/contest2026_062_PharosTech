/****************************************************************************
 * chips/rk3576/vepu/h264_syntax.h
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
 * RK3576 VEPU510 H.264 parameter set generation.
 *
 * The VEPU510 *hardware* synthesizes the slice header from the synt_sli0/1/2
 * register fields.  Software therefore only has to supply the two parameter
 * sets that the hardware cannot produce on its own:
 *
 *   SPS   seq_parameter_set_rbsp()
 *   PPS   pic_parameter_set_rbsp()
 *
 * Both are emitted here with a start code prefix and RBSP trailing bits, so
 * the caller can concatenate
 *
 *   SPS | PPS | (hardware produced IDR slice)
 *
 * and get a decodable access unit.  Emulation prevention is applied to the
 * RBSP payload only, exactly as the H.264 spec requires and as Rockchip MPP
 * does it (mpp/base/mpp_bitwrite.c).
 *
 * --------------------------------------------------------------------------
 * Scope / supported configuration
 * --------------------------------------------------------------------------
 *
 *   - chroma_format_idc = 1 (4:2:0); 8-bit samples
 *   - frame_mbs_only_flag = 1 (progressive, no field coding)
 *   - num_slice_groups_minus1 = 0 (one slice group)
 *   - scaling lists are never transmitted
 *   - weighted prediction is off
 *
 * Profiles 66 (baseline), 77 (main) and 100 (high) are supported.  For
 * profile < 100 the PPS extension block (transform_8x8_mode_flag and
 * second_chroma_qp_index_offset) is omitted, matching MPP.
 *
 * --------------------------------------------------------------------------
 * How this is verified
 * --------------------------------------------------------------------------
 *
 * rk3576-mpp-ref/verify_sps_pps.py extracts MPP's h264e_sps_to_packet() and
 * h264e_pps_to_packet() verbatim, calls them, and compares the resulting
 * bytes against these functions over a matrix of geometries, levels and VUI
 * settings.  A single wrong bit anywhere (including in the emulation
 * prevention logic) shows up as a byte difference, so the whole writer is
 * covered, not just the parts that happen to be easy to reason about.
 *
 * The *policy* decisions that MPP makes in h264e_sps_update() -- level_idc
 * selection, log2_max_frame_num_minus4, cropping offsets -- are reproduced
 * here from that function's rules.  They are not byte-compared, because
 * h264e_sps_update() needs MPP's reference-configuration objects to run; the
 * rules are commented at each site instead.
 ****************************************************************************/

#ifndef __CHIPS_RK3576_VEPU_H264_SYNTAX_H
#define __CHIPS_RK3576_VEPU_H264_SYNTAX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/compiler.h>

/* For RK3576_VEPU510_LAMBDA_IDX_MAX: the lambda index range is a property of
 * the tuning tables, so it is defined with them rather than duplicated here.
 */

#include "vepu510_tables.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* NAL unit types we emit ourselves.  H264_NALU_TYPE_IDR_SLICE and
 * H264_NALU_TYPE_NONIDR_SLICE are used by the capture side to recognise
 * hardware produced NAL units in the output bitstream.
 */

#define RK3576_H264_NAL_TYPE_NONIDR_SLICE 1
#define RK3576_H264_NAL_TYPE_IDR_SLICE    5
#define RK3576_H264_NAL_TYPE_SPS          7
#define RK3576_H264_NAL_TYPE_PPS          8

/* nal_ref_idc for parameter sets and for reference slices.
 *
 * An IDR carries the highest and every other reference picture the one
 * below it, which is how a decoder tells a stream's restarting points from
 * the pictures that merely continue it.
 */

#define RK3576_H264_NALU_PRIORITY_HIGH    2
#define RK3576_H264_NALU_PRIORITY_HIGHEST 3

/* profile_idc */

#define RK3576_H264_PROFILE_BASELINE 66
#define RK3576_H264_PROFILE_MAIN     77
#define RK3576_H264_PROFILE_HIGH     100

/* A level_idc of zero asks the writer to pick the smallest level that can
 * carry the frame size, using the same table MPP uses.
 */

#define RK3576_H264_LEVEL_AUTO 0

/* The H.264 quantisation parameter range.  26 is the neutral value MPP uses
 * for pic_init_qp.
 */

#define RK3576_H264_QP_MIN     0
#define RK3576_H264_QP_MAX     51
#define RK3576_H264_QP_DEFAULT 26

/* Encoder tuning.  These mirror MPP's MppEncFineTuneCfg for H.264, and the
 * defaults are the ones h264e_api_v2.c sets up (atl_str / atr_str_i /
 * atf_str all 1, scene mode default).
 *
 * Scene mode matters more than it looks: MPP disables anti-stripe entirely
 * outside IPC mode, and switches several anti-artefact weight tables to a
 * stronger set in IPC mode.  The default is the non-IPC set.
 */

#define RK3576_H264_SCENE_MODE_DEFAULT 0
#define RK3576_H264_SCENE_MODE_IPC     1

#define RK3576_H264_STR_DEFAULT        1
#define RK3576_H264_STR_MAX            3

/* Slice types, with MPP's numbering.  Only two are produced by this driver,
 * but the anti-smear block compares the previous picture's type against the I
 * value, so the numbers have to be the ones MPP feeds it rather than a local
 * enum that happens to be ordered differently.
 */

#define RK3576_H264_SLICE_P 0
#define RK3576_H264_SLICE_I 2

/* The deblur tuning, MPP's defaults from h264e_api_v2.c: 0 and 3.
 *
 * Neither does what its name suggests here.  deblur_en is what MPP calls
 * qpmap_en by the time the register layer sees it -- the two names are the
 * same flag -- and deblur_str is a strength, 0..7, where 6 and 7 mean
 * something else entirely rather than "stronger".  Both feed the anti-smear
 * block: the strength selects which threshold set is used and appears in the
 * quantiser delta, and the flag selects between two of those sets.
 *
 * They are configuration rather than constants because the register layer's
 * contract is to reproduce MPP for the configuration it is given, and a
 * caller that changed either would otherwise get the defaults silently.
 */

#define RK3576_H264_DEBLUR_EN_DEFAULT  0
#define RK3576_H264_DEBLUR_STR_DEFAULT 3
#define RK3576_H264_DEBLUR_STR_MAX     7

struct rk3576_h264_tune_s
{
  uint32_t scene_mode; /* RK3576_H264_SCENE_MODE_*                */
  uint32_t atl_str;    /* anti-stripe strength, 0..3              */
  uint32_t atr_str_i;  /* anti-ringing strength, I frames, 0..3   */
  uint32_t atf_str;    /* anti-flicker strength, 0..3             */

  /* RDO lambda table index, 0..RK3576_VEPU510_LAMBDA_IDX_MAX.  This selects
   * the rate-distortion trade-off curve, so it is a quality control rather
   * than a speed control, despite the name.  MPP defaults it to 6.
   */

  uint32_t lambda_idx_i;

  /* The deblur tuning the anti-smear thresholds are derived from.  See
   * RK3576_H264_DEBLUR_EN_DEFAULT above for why they are here at all.
   */

  uint32_t deblur_en;  /* RK3576_H264_DEBLUR_EN_DEFAULT  */
  uint32_t deblur_str; /* 0..RK3576_H264_DEBLUR_STR_MAX  */
};

/* QP bias.  A small additive bias on the quantiser, split by frame type.
 *
 * MPP keeps these with the hardware configuration rather than with the
 * tuning parameters, and this struct mirrors that split so the
 * correspondence stays visible.  Leaving it disabled is not the same as
 * leaving it at zero: MPP substitutes the fixed pair below when the enable
 * flag is clear, so the register image differs either way.
 */

#define RK3576_H264_QBIAS_EN_DEFAULT 0
#define RK3576_H264_QBIAS_I_DEFAULT  683
#define RK3576_H264_QBIAS_P_DEFAULT  341

struct rk3576_h264_qbias_s
{
  uint32_t en;
  uint32_t i; /* significant only when en is set */
  uint32_t p;
};

/* Largest parameter set we will ever produce.  The SPS is the bigger of the
 * two and comes out well under 64 bytes even with a full VUI, but callers
 * should size their buffer with room for emulation prevention bytes too.
 */

#define RK3576_H264_SPS_MAX 96
#define RK3576_H264_PPS_MAX 32

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Encoder syntax configuration.
 *
 * This is deliberately a plain value struct with no pointers, so it can be
 * copied, stored and compared.  The register-programming layer takes the
 * same struct, which keeps the parameter sets and the codec registers from
 * disagreeing about, for example, whether 8x8 transform is enabled.
 */

struct rk3576_h264_cfg_s
{
  /* Frame geometry, in luma samples.  The coded size is rounded up to a
   * multiple of 16 and the difference is signalled as cropping.
   */

  uint32_t width;
  uint32_t height;

  /* profile_idc, one of RK3576_H264_PROFILE_*, and the level.  A level of
   * RK3576_H264_LEVEL_AUTO lets the writer choose.
   */

  uint32_t profile_idc;
  uint32_t level_idc;

  /* log2_max_frame_num_minus4 and log2_max_pic_order_cnt_lsb_minus4.
   *
   * Both are written as 12, the widest the syntax allows and wider than any
   * group this driver will be asked for needs: 16-bit frame_num and
   * pic_order_cnt_lsb, against a group length capped at RK3576_VEPU_GOP_MAX.
   * The encoder does not read them back, so what they have to satisfy is a
   * decoder, and a decoder is satisfied by a width that cannot wrap rather
   * than by one that exactly fits.  Picture order count type 0 is the only
   * type the hardware supports.
   */

  uint32_t log2_max_frame_num_minus4;
  uint32_t poc_type;
  uint32_t log2_max_poc_lsb_minus4;

  /* max_num_ref_frames.  One, because the group is short enough that the
   * picture being predicted from is always the one immediately before: there
   * is never more than a single reference, so there is nothing for a larger
   * number to describe.
   */

  uint32_t num_ref_frames;

  /* gaps_in_frame_num_value_allowed_flag.  Zero (gaps not allowed) is what
   * an encoder that never skips frames should signal.
   */

  uint32_t gaps_allowed;

  /* direct_8x8_inference_flag.  Must be 1 for slices that may use direct
   * motion inference; harmless to set for all-intra, and MPP ties it to the
   * 8x8 transform configuration.
   */

  uint32_t direct8x8_inference;

  /* PPS controls. */

  uint32_t entropy_coding_mode; /* 0 = CAVLC, 1 = CABAC */
  uint32_t transform8x8_mode;   /* only meaningful for profile 100 */
  uint32_t constrained_intra_pred;
  uint32_t deblocking_filter_control; /* 1 = PPS carries disable_deblocking */

  /* Quantisation.
   *
   * Two independent values, and confusing them produces a decoder that
   * dequantises with the wrong QP -- i.e. visible garbage rather than an
   * error:
   *
   *   pic_init_qp  the reference QP advertised by the picture parameter
   *                set (pic_init_qp_minus26).
   *   frame_qp     the QP actually used to encode each frame.  The
   *                hardware signals the difference in the slice header as
   *                slice_qp_delta, so the coded QP works out to frame_qp
   *                either way.
   *
   * MPP always leaves pic_init_qp at 26 -- it is a literal in
   * h264e_pps_update() -- and varies frame_qp.  This driver defaults to the
   * same split for the same reason: 26 is the value every field report is
   * based on, so a non-default pic_init_qp is a path with no evidence
   * behind it.  Changing frame_qp instead is supported and exercised.
   */

  uint32_t pic_init_qp;
  uint32_t frame_qp;
  uint32_t chroma_cb_qp_offset;
  uint32_t chroma_cr_qp_offset;

  /* VUI.  The timing block is what tells a player the frame rate, so it is
   * worth emitting even for a file-only milestone.
   */

  uint32_t vui_en;
  uint32_t full_range; /* studio (0) or full (1) range */
  uint32_t fps_num;
  uint32_t fps_den;

  /* Encoder tuning.  The register layer consumes this; the parameter sets
   * do not, so a change here cannot invalidate an already-emitted SPS. */

  struct rk3576_h264_tune_s tune;
  struct rk3576_h264_qbias_s qbias;
};

/* PPS-level values after MPP's profile-dependent downgrades have been
 * applied.
 *
 * Both the PPS bitstream and the codec registers need these, and they have
 * to agree.  A PPS that advertises CABAC while the registers are programmed
 * for CAVLC produces a stream that decodes into noise rather than one that
 * reports an error, so the resolution is done once, here, and both users
 * call it.
 */

struct rk3576_h264_pps_resolved_s
{
  uint32_t entropy_coding_mode;     /* 0 for baseline, whatever was asked
                                     * for otherwise */
  uint32_t transform8x8_mode;       /* forced 0 below profile 100 */
  uint32_t second_chroma_present;   /* 1 only at profile 100 and above */
  uint32_t second_chroma_qp_offset; /* cb offset below 100, cr at/above */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_h264_sps_write
 *
 * Description:
 *   Serialise a sequence parameter set, including its start code prefix and
 *   RBSP trailing bits, into the caller's buffer.
 *
 * Input Parameters:
 *   buf     - destination buffer
 *   bufsize - capacity of buf in bytes; must be at least
 *             RK3576_H264_SPS_MAX to be safe
 *   cfg     - encoder syntax configuration
 *   len     - receives the number of bytes written
 *
 * Returned Value:
 *   Zero on success.  A negated errno on failure:
 *
 *     -EINVAL  cfg is not representable (bad profile, odd geometry)
 *     -ENOSPC  the buffer is too small
 *
 ****************************************************************************/

int rk3576_h264_sps_write(FAR uint8_t *buf, uint32_t bufsize,
                          FAR const struct rk3576_h264_cfg_s *cfg,
                          FAR uint32_t *len);

/****************************************************************************
 * Name: rk3576_h264_pps_write
 *
 * Description:
 *   Serialise a picture parameter set, including its start code prefix and
 *   RBSP trailing bits, into the caller's buffer.
 *
 *   The returned PPS is a matching companion to the SPS produced from the
 *   same cfg, so the two must always be generated from one configuration.
 *
 * Input Parameters:
 *   buf     - destination buffer
 *   bufsize - capacity of buf in bytes; must be at least
 *             RK3576_H264_PPS_MAX to be safe
 *   cfg     - encoder syntax configuration
 *   len     - receives the number of bytes written
 *
 * Returned Value:
 *   Zero on success, or a negated errno as for rk3576_h264_sps_write().
 *
 ****************************************************************************/

int rk3576_h264_pps_write(FAR uint8_t *buf, uint32_t bufsize,
                          FAR const struct rk3576_h264_cfg_s *cfg,
                          FAR uint32_t *len);

/****************************************************************************
 * Name: rk3576_h264_pps_resolve
 *
 * Description:
 *   Resolve the PPS-level values for a configuration, applying the same
 *   profile-dependent downgrades MPP applies in h264e_pps_update():
 *
 *     - baseline (66)   entropy coding is forced to CAVLC
 *     - below high (100) the 8x8 transform and the second chroma offset are
 *       dropped, and the *first* chroma offset is reused in the register
 *       block's cr_ofst field.
 *
 *   Doing this in one place is what keeps the PPS bitstream and the codec
 *   registers consistent; see the struct comment above.
 *
 * Input Parameters:
 *   cfg - encoder syntax configuration
 *   out - receives the resolved values
 *
 ****************************************************************************/

void rk3576_h264_pps_resolve(FAR const struct rk3576_h264_cfg_s *cfg,
                             FAR struct rk3576_h264_pps_resolved_s *out);

/****************************************************************************
 * Name: rk3576_h264_level_auto
 *
 * Description:
 *   Pick the smallest level that can carry a frame of the given size, using
 *   MPP's max_MBs table.  Exposed so the register layer and callers can
 *   report the level that rk3576_h264_sps_write() would have chosen when
 *   cfg->level_idc is RK3576_H264_LEVEL_AUTO.
 *
 * Input Parameters:
 *   width  - frame width in luma samples
 *   height - frame height in luma samples
 *
 * Returned Value:
 *   A level_idc such as 40 for level 4.0.
 *
 ****************************************************************************/

uint32_t rk3576_h264_level_auto(uint32_t width, uint32_t height);

/****************************************************************************
 * Name: rk3576_h264_level_resolve
 *
 * Description:
 *   The level_idc that rk3576_h264_sps_write() will actually signal: the
 *   configured level, raised to the smallest level that can carry the frame
 *   if the configuration asks for something lower (or for automatic).
 *
 *   Exposed because the codec registers also depend on the level -- the
 *   rectangle-size RDO shortcut is only enabled at baseline up to level 3.0
 *   -- and deriving it twice is how the parameter set and the registers end
 *   up disagreeing.
 *
 * Input Parameters:
 *   cfg - encoder syntax configuration
 *
 * Returned Value:
 *   level_idc, e.g. 40 for level 4.0.
 *
 ****************************************************************************/

uint32_t rk3576_h264_level_resolve(FAR const struct rk3576_h264_cfg_s *cfg);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __CHIPS_RK3576_VEPU_H264_SYNTAX_H */
