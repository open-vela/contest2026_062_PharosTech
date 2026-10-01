/****************************************************************************
 * chips/rk3576/vepu/h264_syntax.c
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
 * The bit writer below is a reimplementation of the one Rockchip MPP uses
 * (mpp/base/mpp_bitwrite.c, Apache-2.0).  It is reproduced rather than
 * linked so that the driver does not drag an MPP internal into the kernel
 * build, and it is verified against MPP by byte comparison -- see
 * rk3576-mpp-ref/verify_sps_pps.py.
 *
 * Two behaviours in there are worth calling out because they are easy to get
 * wrong and impossible to notice by reading the output:
 *
 *   1. emulation prevention.  When two consecutive zero bytes have already
 *      been emitted and the next byte is below four, a 0x03 is inserted.
 *      The start code and the NAL header are written with the *raw* writer
 *      so that the leading 00 00 00 01 cannot itself be escaped, and so that
 *      the zero-byte run counter still reads zero when the RBSP begins.
 *
 *   2. mpp_writer_bytes() returns byte_cnt plus a pending partial byte, so a
 *      writer that has just emitted trailing bits reports a whole number.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "h264_syntax.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Bit writer state.  Field-for-field the same as MPP's MppWriteCtx, minus
 * the SEI-specific emulation counter which parameter sets never need.
 */

struct rk3576_h264_bw_s
{
  uint8_t *buffer;        /* first byte of the destination */
  uint8_t *stream;        /* next byte to fill             */
  uint32_t size;          /* destination capacity          */
  uint32_t byte_cnt;      /* whole bytes emitted           */
  uint32_t byte_buffer;   /* partially filled byte, left aligned */
  uint32_t buffered_bits; /* valid bits in byte_buffer, 0..7 */
  uint32_t zero_bytes;    /* trailing run of zero bytes    */
  bool overflow;
};

/* One row of MPP's level table (h264e_sps.c).  max_mbs is the largest frame
 * the level can carry, in macroblocks.
 */

struct rk3576_h264_level_s
{
  uint32_t level;
  uint32_t max_mbs;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Kept in MPP's order, which is ascending by max_mbs.  Level 1b (level_idc
 * 9) sits between 1.0 and 1.1 and is never selected, matching MPP.
 */

static const struct rk3576_h264_level_s g_level_infos[] = {
  { 10, 99 },     /* 1.0   */
  { 9, 99 },      /* 1.b   */
  { 11, 396 },    /* 1.1   */
  { 12, 396 },    /* 1.2   */
  { 13, 396 },    /* 1.3   */
  { 20, 396 },    /* 2.0   */
  { 21, 792 },    /* 2.1   */
  { 22, 1620 },   /* 2.2   */
  { 30, 1620 },   /* 3.0   */
  { 31, 3600 },   /* 3.1   */
  { 32, 5120 },   /* 3.2   */
  { 40, 8192 },   /* 4.0   */
  { 41, 8192 },   /* 4.1   */
  { 42, 8704 },   /* 4.2   */
  { 50, 22080 },  /* 5.0   */
  { 51, 36864 },  /* 5.1   */
  { 52, 36864 },  /* 5.2   */
  { 60, 139264 }, /* 6.0   */
  { 61, 139264 }, /* 6.1   */
  { 62, 139264 }, /* 6.2   */
};

/* MPP_FRAME_VIDEO_FMT_UNSPECIFIED */

#define RK3576_H264_VIDEO_FMT_UNSPECIFIED 5

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_h264_bw_init
 *
 * Description:
 *   Point a bit writer at a destination buffer.
 *
 ****************************************************************************/

static void rk3576_h264_bw_init(FAR struct rk3576_h264_bw_s *bw,
                                FAR uint8_t *buffer, uint32_t size)
{
  memset(bw, 0, sizeof(*bw));
  bw->buffer = buffer;
  bw->stream = buffer;
  bw->size = size;
}

/****************************************************************************
 * Name: rk3576_h264_bw_reserve
 *
 * Description:
 *   Claim one more output byte, refusing to write past the end of the
 *   destination buffer.
 *
 *   MPP tests its counter only after writing the byte, so a full buffer
 *   corrupts one byte of memory.  Refusing up front can only differ from MPP
 *   when MPP would itself be writing out of bounds, so byte-for-byte
 *   agreement on valid inputs is preserved.
 *
 * Returned Value:
 *   True if the byte was claimed.
 *
 ****************************************************************************/

static bool rk3576_h264_bw_reserve(FAR struct rk3576_h264_bw_s *bw)
{
  if (bw->overflow)
    {
      return false;
    }

  if (bw->byte_cnt >= bw->size)
    {
      bw->overflow = true;
      return false;
    }

  return true;
}

/****************************************************************************
 * Name: rk3576_h264_bw_put_raw
 *
 * Description:
 *   Append len bits of val without emulation prevention.  Used for the start
 *   code prefix and the NAL header.
 *
 ****************************************************************************/

static void rk3576_h264_bw_put_raw(FAR struct rk3576_h264_bw_s *bw,
                                   uint32_t val, uint32_t len)
{
  uint32_t bits;
  uint32_t byte_buffer = bw->byte_buffer;

  if (bw->overflow)
    {
      return;
    }

  bits = len + bw->buffered_bits;
  byte_buffer |= (uint32_t)(val << (32 - bits));

  while (bits > 7)
    {
      if (!rk3576_h264_bw_reserve(bw))
        {
          return;
        }

      *bw->stream++ = (uint8_t)(byte_buffer >> 24);
      bw->byte_cnt++;
      bits -= 8;
      byte_buffer <<= 8;
    }

  bw->byte_buffer = byte_buffer;
  bw->buffered_bits = bits;
}

/****************************************************************************
 * Name: rk3576_h264_bw_put
 *
 * Description:
 *   Append len bits of val, inserting an emulation prevention byte whenever
 *   a third byte below four would follow two zero bytes.
 *
 ****************************************************************************/

static void rk3576_h264_bw_put(FAR struct rk3576_h264_bw_s *bw, uint32_t val,
                               uint32_t len)
{
  uint32_t bits = len + bw->buffered_bits;
  uint32_t byte_buffer = bw->byte_buffer;

  byte_buffer |= (uint32_t)(val << (32 - bits));

  while (bits > 7)
    {
      uint32_t zero_bytes = bw->zero_bytes;
      uint8_t byte;

      if (!rk3576_h264_bw_reserve(bw))
        {
          return;
        }

      byte = (uint8_t)(byte_buffer >> 24);

      if (zero_bytes == 2 && byte < 4)
        {
          /* The 0x03 counts against the buffer too. */

          if (!rk3576_h264_bw_reserve(bw))
            {
              return;
            }

          *bw->stream++ = 3;
          bw->byte_cnt++;
          zero_bytes = 0;
        }

      *bw->stream++ = byte;
      bw->byte_cnt++;

      bw->zero_bytes = (byte == 0) ? zero_bytes + 1 : 0;

      bits -= 8;
      byte_buffer <<= 8;
    }

  bw->byte_buffer = byte_buffer;
  bw->buffered_bits = bits;
}

/****************************************************************************
 * Name: rk3576_h264_bw_put_ue / rk3576_h264_bw_put_se
 *
 * Description:
 *   Exp-Golomb codes.  The unsigned form splits values wider than twelve
 *   bits into several writes so that no single call exceeds the writer's
 *   twenty-four bit limit, exactly as MPP does.
 *
 ****************************************************************************/

static void rk3576_h264_bw_put_ue(FAR struct rk3576_h264_bw_s *bw,
                                  uint32_t val)
{
  uint32_t num_bits = 0;

  val++;
  while (val >> ++num_bits)
    {
    }

  if (num_bits > 12)
    {
      uint32_t shim = num_bits - 1;

      if (shim > 24)
        {
          shim -= 24;
          rk3576_h264_bw_put(bw, 0, 24);
        }

      rk3576_h264_bw_put(bw, 0, shim);

      if (num_bits > 24)
        {
          num_bits -= 24;
          rk3576_h264_bw_put(bw, val >> num_bits, 24);
          val >>= num_bits;
        }

      rk3576_h264_bw_put(bw, val, num_bits);
    }
  else
    {
      rk3576_h264_bw_put(bw, val, 2 * num_bits - 1);
    }
}

static void rk3576_h264_bw_put_se(FAR struct rk3576_h264_bw_s *bw, int32_t val)
{
  uint32_t tmp;

  if (val > 0)
    {
      tmp = (uint32_t)(2 * val - 1);
    }
  else
    {
      tmp = (uint32_t)(-2 * val);
    }

  rk3576_h264_bw_put_ue(bw, tmp);
}

/****************************************************************************
 * Name: rk3576_h264_bw_trailing
 *
 * Description:
 *   Append the rbsp_trailing_bits: a stop bit then zero fill to the next
 *   byte boundary.
 *
 ****************************************************************************/

static void rk3576_h264_bw_trailing(FAR struct rk3576_h264_bw_s *bw)
{
  rk3576_h264_bw_put(bw, 1, 1);

  if (bw->buffered_bits)
    {
      rk3576_h264_bw_put(bw, 0, 8 - bw->buffered_bits);
    }
}

/****************************************************************************
 * Name: rk3576_h264_bw_bytes
 *
 * Description:
 *   Number of whole bytes in the finished stream.
 *
 ****************************************************************************/

static uint32_t rk3576_h264_bw_bytes(FAR const struct rk3576_h264_bw_s *bw)
{
  return bw->byte_cnt + (bw->buffered_bits > 0 ? 1 : 0);
}

/****************************************************************************
 * Name: rk3576_h264_bw_nal_header
 *
 * Description:
 *   Write the start code prefix and the one byte NAL header.  Raw writes, so
 *   the leading zero run is not escaped.
 *
 ****************************************************************************/

static void rk3576_h264_bw_nal_header(FAR struct rk3576_h264_bw_s *bw,
                                      uint32_t nal_ref_idc, uint32_t nal_type)
{
  /* start_code_prefix_one_3bytes is 00 00 01; MPP emits a leading zero byte
   * as well, making the four byte start code that decoders expect to find at
   * the head of a parameter set.
   */

  rk3576_h264_bw_put_raw(bw, 0, 24);
  rk3576_h264_bw_put_raw(bw, 1, 8);

  rk3576_h264_bw_put_raw(bw, 0, 1); /* forbidden_zero_bit */
  rk3576_h264_bw_put_raw(bw, nal_ref_idc, 2);
  rk3576_h264_bw_put_raw(bw, nal_type, 5);
}

/****************************************************************************
 * Name: rk3576_h264_validate
 *
 * Description:
 *   Reject configurations the writers below cannot represent.  Everything
 *   checked here would otherwise produce a bitstream that a decoder silently
 *   misinterprets rather than one it rejects.
 *
 ****************************************************************************/

static int rk3576_h264_validate(FAR const struct rk3576_h264_cfg_s *cfg)
{
  if (cfg == NULL || cfg->width == 0 || cfg->height == 0)
    {
      return -EINVAL;
    }

  if (cfg->profile_idc != RK3576_H264_PROFILE_BASELINE &&
      cfg->profile_idc != RK3576_H264_PROFILE_MAIN &&
      cfg->profile_idc != RK3576_H264_PROFILE_HIGH)
    {
      return -EINVAL;
    }

  /* Only picture order count type 0 is supported: it is the only type the
   * hardware can generate, and type 1 and 2 would need different SPS fields.
   */

  if (cfg->poc_type != 0)
    {
      return -EINVAL;
    }

  /* Both of these are coded as ue(v) minus four, so they must be at least
   * four.  MPP uses 12 for an all-intra stream.
   */

  if (cfg->log2_max_frame_num_minus4 > 12 || cfg->log2_max_poc_lsb_minus4 > 12)
    {
      return -EINVAL;
    }

  if (cfg->chroma_cb_qp_offset > 12 || cfg->chroma_cr_qp_offset > 12)
    {
      return -EINVAL;
    }

  /* The tuning strengths index fixed tables, so a value past the end would
   * read out of bounds in the register layer.  The scene mode selects
   * between two known table sets. */

  if (cfg->tune.scene_mode != RK3576_H264_SCENE_MODE_DEFAULT &&
      cfg->tune.scene_mode != RK3576_H264_SCENE_MODE_IPC)
    {
      return -EINVAL;
    }

  if (cfg->tune.atl_str > RK3576_H264_STR_MAX ||
      cfg->tune.atr_str_i > RK3576_H264_STR_MAX ||
      cfg->tune.atf_str > RK3576_H264_STR_MAX)
    {
      return -EINVAL;
    }

  /* The lambda index bounds the table copy, so an index past the end would
   * read out of bounds.  MPP applies the same bound (0..8).
   */

  if (cfg->tune.lambda_idx_i > RK3576_VEPU510_LAMBDA_IDX_MAX)
    {
      return -EINVAL;
    }

  /* Both quantisers are six bit fields in the codec registers and both are
   * outside the H.264 range beyond 51, so reject rather than truncate.
   */

  if (cfg->pic_init_qp > RK3576_H264_QP_MAX ||
      cfg->frame_qp > RK3576_H264_QP_MAX)
    {
      return -EINVAL;
    }

  /* A single slice group and 4:2:0 are the only shapes this writer emits. */

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uint32_t rk3576_h264_level_auto(uint32_t width, uint32_t height)
{
  uint32_t aligned_w = (width + 15) & ~15u;
  uint32_t aligned_h = (height + 15) & ~15u;
  uint32_t mbs = (aligned_w * aligned_h) >> 8;
  uint32_t i;

  for (i = 0; i < sizeof(g_level_infos) / sizeof(g_level_infos[0]); i++)
    {
      if (g_level_infos[i].max_mbs >= mbs)
        {
          /* MPP never selects 1b, even though its row comes first for small
           * frames.
           */

          if (g_level_infos[i].level == 9)
            {
              continue;
            }

          return g_level_infos[i].level;
        }
    }

  /* Larger than level 6.2.  Return the top of the table rather than zero, so
   * the caller still gets a legal level_idc.
   */

  return 62;
}

uint32_t rk3576_h264_level_resolve(FAR const struct rk3576_h264_cfg_s *cfg)
{
  uint32_t needed = rk3576_h264_level_auto(cfg->width, cfg->height);

  /* MPP never signals a level below what the frame size needs.  For an
   * automatic level that is the whole answer; for an explicit one it is a
   * floor. */

  if (cfg->level_idc == RK3576_H264_LEVEL_AUTO || needed > cfg->level_idc)
    {
      return needed;
    }

  return cfg->level_idc;
}

int rk3576_h264_sps_write(FAR uint8_t *buf, uint32_t bufsize,
                          FAR const struct rk3576_h264_cfg_s *cfg,
                          FAR uint32_t *len)
{
  struct rk3576_h264_bw_s bw;
  uint32_t aligned_w;
  uint32_t aligned_h;
  uint32_t pic_width_in_mbs;
  uint32_t pic_height_in_mbs;
  uint32_t crop_right;
  uint32_t crop_bottom;
  uint32_t cropping;
  uint32_t level_idc;
  uint32_t signal_type_present;
  int ret;

  ret = rk3576_h264_validate(cfg);
  if (ret < 0)
    {
      return ret;
    }

  if (len != NULL)
    {
      *len = 0;
    }

  aligned_w = (cfg->width + 15) & ~15u;
  aligned_h = (cfg->height + 15) & ~15u;

  /* Coded size is the 16-aligned size; whatever that adds is signalled as
   * cropping.  The crop offsets are divided by SubWidthC and SubHeightC,
   * both of which are two for 4:2:0.
   */

  pic_width_in_mbs = aligned_w >> 4;
  pic_height_in_mbs = aligned_h >> 4;
  crop_right = aligned_w - cfg->width;
  crop_bottom = aligned_h - cfg->height;
  cropping = (crop_right != 0 || crop_bottom != 0) ? 1 : 0;

  level_idc = rk3576_h264_level_resolve(cfg);

  rk3576_h264_bw_init(&bw, buf, bufsize);
  rk3576_h264_bw_nal_header(&bw, RK3576_H264_NALU_PRIORITY_HIGHEST,
                            RK3576_H264_NAL_TYPE_SPS);

  /* profile_idc plus the constraint flags MPP derives from the profile.
   * Baseline sets constraint_set0 and constraint_set1, main sets only
   * constraint_set1, and high sets none of them.
   */

  rk3576_h264_bw_put(&bw, cfg->profile_idc, 8);
  rk3576_h264_bw_put(&bw,
                     cfg->profile_idc == RK3576_H264_PROFILE_BASELINE ? 1 : 0,
                     1); /* set0 */
  rk3576_h264_bw_put(&bw, cfg->profile_idc == RK3576_H264_PROFILE_HIGH ? 0 : 1,
                     1);         /* set1 */
  rk3576_h264_bw_put(&bw, 0, 1); /* set2 */
  rk3576_h264_bw_put(&bw, 0, 1); /* set3 */
  rk3576_h264_bw_put(&bw, 0, 1); /* set4 */
  rk3576_h264_bw_put(&bw, 0, 1); /* set5 */
  rk3576_h264_bw_put(&bw, 0, 2); /* reserved */

  rk3576_h264_bw_put(&bw, level_idc, 8);
  rk3576_h264_bw_put_ue(&bw, 0); /* sps_id */

  if (cfg->profile_idc >= RK3576_H264_PROFILE_HIGH)
    {
      /* Only reachable for profile 100, which is 8 bit 4:2:0, so these are
       * the fixed values 1, 0, 0.
       */

      rk3576_h264_bw_put_ue(&bw, 1); /* chroma_format_idc    */
      rk3576_h264_bw_put_ue(&bw, 0); /* bit_depth_luma_minus8 */
      rk3576_h264_bw_put_ue(&bw, 0); /* bit_depth_chroma_minus8 */
      rk3576_h264_bw_put(&bw, 0, 1); /* qpprime_y_zero_bypass */
      rk3576_h264_bw_put(&bw, 0, 1); /* seq_scaling_matrix_present */
    }

  rk3576_h264_bw_put_ue(&bw, cfg->log2_max_frame_num_minus4);
  rk3576_h264_bw_put_ue(&bw, cfg->poc_type);

  if (cfg->poc_type == 0)
    {
      rk3576_h264_bw_put_ue(&bw, cfg->log2_max_poc_lsb_minus4);
    }

  rk3576_h264_bw_put_ue(&bw, cfg->num_ref_frames);
  rk3576_h264_bw_put(&bw, cfg->gaps_allowed ? 1 : 0, 1);

  rk3576_h264_bw_put_ue(&bw, pic_width_in_mbs - 1);
  rk3576_h264_bw_put_ue(&bw, pic_height_in_mbs - 1);

  rk3576_h264_bw_put(&bw, 1, 1); /* frame_mbs_only_flag    */
  rk3576_h264_bw_put(&bw, cfg->direct8x8_inference ? 1 : 0, 1);
  rk3576_h264_bw_put(&bw, cropping, 1);

  if (cropping)
    {
      rk3576_h264_bw_put_ue(&bw, 0); /* frame_crop_left_offset  */
      rk3576_h264_bw_put_ue(&bw, crop_right / 2);
      rk3576_h264_bw_put_ue(&bw, 0); /* frame_crop_top_offset   */
      rk3576_h264_bw_put_ue(&bw, crop_bottom / 2);
    }

  rk3576_h264_bw_put(&bw, cfg->vui_en ? 1 : 0, 1);

  if (cfg->vui_en)
    {
      /* MPP derives video_signal_type_present_flag from the colour
       * description: it is set when the range is JPEG/full, or when any of
       * colour primaries, transfer characteristics or matrix coefficients
       * has been specified.  This driver carries no colour description, so
       * only the full range case sets it.
       */

      signal_type_present = cfg->full_range ? 1 : 0;

      rk3576_h264_bw_put(&bw, 0, 1); /* aspect_ratio_info_present */
      rk3576_h264_bw_put(&bw, 0, 1); /* overscan_info_present     */
      rk3576_h264_bw_put(&bw, signal_type_present, 1);

      if (signal_type_present)
        {
          rk3576_h264_bw_put(&bw, RK3576_H264_VIDEO_FMT_UNSPECIFIED, 3);
          rk3576_h264_bw_put(&bw, cfg->full_range ? 1 : 0, 1);
          rk3576_h264_bw_put(&bw, 0, 1); /* color_description_present */
        }

      rk3576_h264_bw_put(&bw, 0, 1); /* chroma_loc_info_present   */

      /* timing_info.  num_units_in_tick is the denominator and time_scale is
       * twice the numerator, which is how a decoder arrives at the frame
       * rate.  Both are written as two 16 bit halves, high half first.
       */

      rk3576_h264_bw_put(&bw, 1, 1); /* timing_info_present       */
      rk3576_h264_bw_put(&bw, cfg->fps_den >> 16, 16);
      rk3576_h264_bw_put(&bw, cfg->fps_den & 0xffff, 16);
      rk3576_h264_bw_put(&bw, (cfg->fps_num * 2) >> 16, 16);
      rk3576_h264_bw_put(&bw, (cfg->fps_num * 2) & 0xffff, 16);
      rk3576_h264_bw_put(&bw, 1, 1); /* fixed_frame_rate_flag     */

      rk3576_h264_bw_put(&bw, 0, 1); /* nal_hrd_parameters_present */
      rk3576_h264_bw_put(&bw, 0, 1); /* vcl_hrd_parameters_present */
      rk3576_h264_bw_put(&bw, 0, 1); /* pic_struct_present         */

      rk3576_h264_bw_put(&bw, 1, 1);  /* bitstream_restriction      */
      rk3576_h264_bw_put(&bw, 1, 1);  /* motion_vectors_over_pic_boundaries */
      rk3576_h264_bw_put_ue(&bw, 0);  /* max_bytes_per_pic_denom   */
      rk3576_h264_bw_put_ue(&bw, 0);  /* max_bits_per_mb_denom     */
      rk3576_h264_bw_put_ue(&bw, 15); /* log2_max_mv_length_horizontal */
      rk3576_h264_bw_put_ue(&bw, 15); /* log2_max_mv_length_vertical   */
      rk3576_h264_bw_put_ue(&bw, 0);  /* num_reorder_frames        */
      rk3576_h264_bw_put_ue(&bw, cfg->num_ref_frames);
    }

  rk3576_h264_bw_trailing(&bw);

  if (bw.overflow)
    {
      return -ENOSPC;
    }

  if (len != NULL)
    {
      *len = rk3576_h264_bw_bytes(&bw);
    }

  return OK;
}

void rk3576_h264_pps_resolve(FAR const struct rk3576_h264_cfg_s *cfg,
                             FAR struct rk3576_h264_pps_resolved_s *out)
{
  /* MPP silently downgrades CABAC on baseline, and drops the 8x8 transform
   * and the second chroma offset for every profile below high.  Reproduce
   * that here so a mismatched configuration produces a stream that decodes
   * rather than one that does not.
   *
   * Note the interaction with cr_ofst in the register block: with the
   * extension block absent, a decoder uses chroma_qp_index_offset for *both*
   * chroma components, so the codec registers have to be given the same
   * value in their cr_ofst field.  That is what carrying cb through to
   * second_chroma_qp_offset below achieves.
   */

  out->entropy_coding_mode = cfg->entropy_coding_mode;
  if (cfg->profile_idc == RK3576_H264_PROFILE_BASELINE)
    {
      out->entropy_coding_mode = 0;
    }

  if (cfg->profile_idc < RK3576_H264_PROFILE_HIGH)
    {
      out->transform8x8_mode = 0;
      out->second_chroma_present = 0;
      out->second_chroma_qp_offset = cfg->chroma_cb_qp_offset;
    }
  else
    {
      out->transform8x8_mode = cfg->transform8x8_mode;
      out->second_chroma_present = 1;
      out->second_chroma_qp_offset = cfg->chroma_cr_qp_offset;
    }
}

int rk3576_h264_pps_write(FAR uint8_t *buf, uint32_t bufsize,
                          FAR const struct rk3576_h264_cfg_s *cfg,
                          FAR uint32_t *len)
{
  struct rk3576_h264_bw_s bw;
  struct rk3576_h264_pps_resolved_s pps;
  int ret;

  ret = rk3576_h264_validate(cfg);
  if (ret < 0)
    {
      return ret;
    }

  if (len != NULL)
    {
      *len = 0;
    }

  rk3576_h264_pps_resolve(cfg, &pps);

  rk3576_h264_bw_init(&bw, buf, bufsize);
  rk3576_h264_bw_nal_header(&bw, RK3576_H264_NALU_PRIORITY_HIGHEST,
                            RK3576_H264_NAL_TYPE_PPS);

  rk3576_h264_bw_put_ue(&bw, 0); /* pic_parameter_set_id      */
  rk3576_h264_bw_put_ue(&bw, 0); /* seq_parameter_set_id      */
  rk3576_h264_bw_put(&bw, pps.entropy_coding_mode, 1);
  rk3576_h264_bw_put(&bw, 0, 1); /* bottom_field_pic_order_in_frame_present */
  rk3576_h264_bw_put_ue(&bw, 0); /* num_slice_groups_minus1   */
  rk3576_h264_bw_put_ue(&bw, 0); /* num_ref_idx_l0_active_minus1 */
  rk3576_h264_bw_put_ue(&bw, 0); /* num_ref_idx_l1_active_minus1 */
  rk3576_h264_bw_put(&bw, 0, 1); /* weighted_pred_flag        */
  rk3576_h264_bw_put(&bw, 0, 2); /* weighted_bipred_idc       */

  /* pic_init_qp_minus26 and pic_init_qs_minus26 are both the same signed
   * value: MPP sets pic_init_qs to the slice QP before subtracting.
   */

  rk3576_h264_bw_put_se(&bw, (int32_t)cfg->pic_init_qp - 26);
  rk3576_h264_bw_put_se(&bw, (int32_t)cfg->pic_init_qp - 26);
  rk3576_h264_bw_put_se(&bw, (int32_t)cfg->chroma_cb_qp_offset);

  rk3576_h264_bw_put(&bw, cfg->deblocking_filter_control ? 1 : 0, 1);
  rk3576_h264_bw_put(&bw, cfg->constrained_intra_pred ? 1 : 0, 1);
  rk3576_h264_bw_put(&bw, 0, 1); /* redundant_pic_cnt_present */

  /* The extension block is present only if one of its three flags is set.
   * Scaling lists are never transmitted by this driver, so the scaling
   * matrix flag is always zero and the six-entry list is never written.
   */

  if (pps.transform8x8_mode || pps.second_chroma_present)
    {
      rk3576_h264_bw_put(&bw, pps.transform8x8_mode ? 1 : 0, 1);
      rk3576_h264_bw_put(&bw, 0, 1); /* pic_scaling_matrix_present */
      rk3576_h264_bw_put_se(&bw, (int32_t)pps.second_chroma_qp_offset);
    }

  rk3576_h264_bw_trailing(&bw);

  if (bw.overflow)
    {
      return -ENOSPC;
    }

  if (len != NULL)
    {
      *len = rk3576_h264_bw_bytes(&bw);
    }

  return OK;
}
