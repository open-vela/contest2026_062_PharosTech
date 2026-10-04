/****************************************************************************
 * chips/rk3576/vepu/vepu510_regs.c
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
 * RK3576 VEPU510 register value generation.
 *
 * Mirrors the Rockchip MPP HAL (mpp/hal/rkenc/h264e/hal_h264e_vepu510.c,
 * Apache-2.0).  The parts implemented here are the ones that can be checked
 * exactly on the host against MPP's own code -- see rk3576-mpp-ref/
 * verify_regs.py, which extracts MPP's setup_vepu510_normal() and
 * setup_vepu510_prep() verbatim, calls them, and compares the resulting
 * register words against these functions for a range of geometries.
 *
 * Field names below are the upstream ones, so the correspondence with MPP
 * stays checkable.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_RK3576_VEPU

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* OK (== 0), NuttX's success value. */

#include <sys/types.h>

#include "vepu510_regs.h"

/* The tuning tables, and the constants that bound them. */

#include "vepu510_tables.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* VEPU510 works on a 16-pixel macroblock grid. */

#define RK3576_VEPU510_MB 16
#define RK3576_VEPU510_ALIGN_UP(v) \
  (((v) + RK3576_VEPU510_MB - 1) & ~(RK3576_VEPU510_MB - 1))
#define RK3576_VEPU510_ALIGN_TO(v, a) (((v) + (a)-1u) & ~((a)-1u))

/* Geometry of the reconstruction working set, from MPP setup_hal_bufs().
 *
 * The 16 extra rows, the 64th of the aligned area taken for the FBC header
 * and the 8 KB rounding are all the IP's own requirements; they are named
 * rather than left as literals so that the arithmetic below reads as the
 * formula it is.
 */

#define RK3576_VEPU510_RECN_ALIGN_W     64u
#define RK3576_VEPU510_RECN_ALIGN_H     16u
#define RK3576_VEPU510_RECN_EXTRA_ROWS  16u
#define RK3576_VEPU510_RECN_BUF_ALIGN   0x2000u /* 8 KB */
#define RK3576_VEPU510_RECN_SMALL_ALIGN 16u

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void rk3576_vepu510_regs_ctl(FAR HalVepu510RegSet *regs)
{
  /* Register numbers here are byte offset / 4, taken from the generated
   * struct layout.  MPP's own comments number these differently -- it calls
   * ENC_STRT "reg001" when the struct puts it at 0x0010, i.e. reg4 -- and
   * its numbering is simply wrong.  It matters: the offset is what decides
   * which register is written, and a reader chasing "reg001" through the
   * hardware would be looking at the wrong one entirely.
   */

  /* 0x0000 reg0 VERSION is read only */

  /* 0x0010 reg4 ENC_STRT: single-frame ("register configuration") mode.
   * lkt_num = 0 selects it; vepu_cmd = 1 tells the hardware to start.
   *
   * Writing this register is what starts the encoder, so the driver writes
   * it last rather than with the rest of the block -- see
   * rk3576_vepu_encode().  It is the image that carries the command, not
   * the write order, which is why the value is set here.
   */

  regs->reg_ctl.enc_strt.lkt_num = 0;
  regs->reg_ctl.enc_strt.vepu_cmd = 1;

  /* 0x0014 reg5 ENC_CLR: no clear requested this frame. */

  regs->reg_ctl.enc_clr.safe_clr = 0;
  regs->reg_ctl.enc_clr.force_clr = 0;

  /* 0x0020 reg8 INT_EN: everything that indicates a problem, plus the
   * completion interrupts we actually wait on.  MPP enables exactly this
   * set.
   */

  regs->reg_ctl.int_en.enc_done_en = 1;
  regs->reg_ctl.int_en.lkt_node_done_en = 1;
  regs->reg_ctl.int_en.sclr_done_en = 1;
  regs->reg_ctl.int_en.vslc_done_en = 0;
  regs->reg_ctl.int_en.vbsf_oflw_en = 1;
  regs->reg_ctl.int_en.vbuf_lens_en = 1;
  regs->reg_ctl.int_en.enc_err_en = 1;
  regs->reg_ctl.int_en.vsrc_err_en = 1;
  regs->reg_ctl.int_en.wdg_en = 1;
  regs->reg_ctl.int_en.lkt_err_int_en = 1;
  regs->reg_ctl.int_en.lkt_err_stop_en = 1;
  regs->reg_ctl.int_en.lkt_force_stop_en = 1;
  regs->reg_ctl.int_en.jslc_done_en = 1;
  regs->reg_ctl.int_en.jbsf_oflw_en = 1;
  regs->reg_ctl.int_en.jbuf_lens_en = 1;
  regs->reg_ctl.int_en.dvbm_err_en = 0;

  /* 0x0024 reg9 INT_MSK: unmask all of them. */

  regs->reg_ctl.int_msk.enc_done_msk = 0;
  regs->reg_ctl.int_msk.lkt_node_done_msk = 0;
  regs->reg_ctl.int_msk.sclr_done_msk = 0;
  regs->reg_ctl.int_msk.vslc_done_msk = 0;
  regs->reg_ctl.int_msk.vbsf_oflw_msk = 0;
  regs->reg_ctl.int_msk.vbuf_lens_msk = 0;
  regs->reg_ctl.int_msk.enc_err_msk = 0;
  regs->reg_ctl.int_msk.vsrc_err_msk = 0;
  regs->reg_ctl.int_msk.wdg_msk = 0;
  regs->reg_ctl.int_msk.lkt_err_int_msk = 0;
  regs->reg_ctl.int_msk.lkt_err_stop_msk = 0;
  regs->reg_ctl.int_msk.lkt_force_stop_msk = 0;
  regs->reg_ctl.int_msk.jslc_done_msk = 0;
  regs->reg_ctl.int_msk.jbsf_oflw_msk = 0;
  regs->reg_ctl.int_msk.jbuf_lens_msk = 0;
  regs->reg_ctl.int_msk.dvbm_err_msk = 0;

  /* 0x0038 reg14 ENC_WDG: hardware watchdog threshold.  MPP leaves it at 0
   * here (the watchdog is armed but never expires on a healthy stream).
   */

  regs->reg_ctl.enc_wdg.vs_load_thd = 0;

  /* 0x0054 reg21 OPT_STRG: keep the core clock enabled and let the
   * hardware drive its own reset.
   */

  regs->reg_ctl.opt_strg.cke = 1;
  regs->reg_ctl.opt_strg.resetn_hw_en = 1;

  /* 0x0030 reg12 DTRNS_MAP / 0x0034 reg13 DTRNS_CFG: byte order on the data
   * channels.  bsw_bus_edin = 7 is MPP's setting for the bitstream write
   * channel.
   */

  regs->reg_ctl.dtrns_map.jpeg_bus_edin = 0;
  regs->reg_ctl.dtrns_map.src_bus_edin = 0;
  regs->reg_ctl.dtrns_map.meiw_bus_edin = 0;
  regs->reg_ctl.dtrns_map.bsw_bus_edin = 7;
  regs->reg_ctl.dtrns_map.lktw_bus_edin = 0;
  regs->reg_ctl.dtrns_map.rec_nfbc_bus_edin = 0;

  regs->reg_ctl.dtrns_cfg.axi_brsp_cke = 0;
}

int rk3576_vepu510_regs_src(FAR HalVepu510RegSet *regs,
                            FAR const struct rk3576_vepu510_frame_s *frm)
{
  H264eVepu510Frame *reg_frm = &regs->reg_frm;
  uint32_t w_align = RK3576_VEPU510_ALIGN_UP(frm->width);
  uint32_t h_align = RK3576_VEPU510_ALIGN_UP(frm->height);
  uint32_t c_stride;

  /* This tree only ever feeds 4:2:0 from the camera path.  Anything that
   * would need the RGB-to-YUV conversion matrix (i.e. an RGB source format)
   * is rejected rather than silently mis-programmed.
   */

  switch (frm->src_fmt)
    {
      case RK3576_VEPU510_FMT_YUV420SP:
      case RK3576_VEPU510_FMT_YUV420P:
      case RK3576_VEPU510_FMT_YUV422SP:
      case RK3576_VEPU510_FMT_YUV422P:
      case RK3576_VEPU510_FMT_YUV444SP:
      case RK3576_VEPU510_FMT_YUV444P:
      case RK3576_VEPU510_FMT_YUV400:
      case RK3576_VEPU510_FMT_YUYV422:
      case RK3576_VEPU510_FMT_UYVY422:
        break;

      default:
        /* RGB input would need the RGB-to-YUV conversion matrix. */
        return -ENOSYS;
    }

  /* Picture size on the macroblock grid, and how much of the rounded size is
   * padding.  The hardware needs both: enc_rsl is the rounded geometry it
   * encodes, src_fill is how many pixels of the last macroblock row/column
   * are not real picture.
   */

  reg_frm->common.enc_rsl.pic_wd8_m1 = w_align / 8 - 1;
  reg_frm->common.src_fill.pic_wfill = w_align - frm->width;
  reg_frm->common.enc_rsl.pic_hd8_m1 = h_align / 8 - 1;
  reg_frm->common.src_fill.pic_hfill = h_align - frm->height;

  /* Source format.  For a YUV input the colour conversion weights and
   * offsets stay zero (MPP uses an all-zero table for every YUV format),
   * so nothing needs writing -- the register set is expected to be zeroed.
   */

  regs->reg_ctl.dtrns_map.src_bus_edin = 0;

  reg_frm->common.src_fmt.src_cfmt = frm->src_fmt;
  reg_frm->common.src_fmt.alpha_swap = 0;
  reg_frm->common.src_fmt.rbuv_swap = frm->rbuv_swap ? 1 : 0;
  reg_frm->common.src_fmt.out_fmt =
      (frm->src_fmt == RK3576_VEPU510_FMT_YUV400) ? 0 : 1;

  /* Plane strides.  MPP derives the chroma stride from the format; the
   * y_stride comes from the caller because it is a property of the buffer
   * the capture path produced, not of the pixel format.
   */

  switch (frm->src_fmt)
    {
      case RK3576_VEPU510_FMT_YUV444SP:
        c_stride = frm->y_stride * 2;
        break;

      case RK3576_VEPU510_FMT_YUV422SP:
      case RK3576_VEPU510_FMT_YUV420SP:
      case RK3576_VEPU510_FMT_YUV444P:
        c_stride = frm->y_stride;
        break;

      default: /* YUV420P, YUV422P and the packed formats */
        c_stride = frm->y_stride / 2;
        break;
    }

  reg_frm->common.src_strd0.src_strd0 = frm->y_stride;
  reg_frm->common.src_strd1.src_strd1 = c_stride;

  /* No mirroring, no rotation, no 4x4-tiled input. */

  reg_frm->common.src_proc.src_mirr = 0;
  reg_frm->common.src_proc.src_rot = 0;
  reg_frm->common.src_proc.tile4x4_en = 0;

  /* Motion-vector limits only apply to inter prediction; we emit I frames
   * from this module, and MPP reports the limits disabled here too.
   */

  reg_frm->sli_cfg.mv_v_lmt_thd = 0;
  reg_frm->sli_cfg.mv_v_lmt_en = 0;

  /* No sub-picture offset. */

  reg_frm->common.pic_ofst.pic_ofst_y = 0;
  reg_frm->common.pic_ofst.pic_ofst_x = 0;

  return OK;
}

int rk3576_vepu510_regs_addr(FAR HalVepu510RegSet *regs,
                             FAR const struct rk3576_vepu510_frame_s *frm)
{
  H264eVepu510Frame *reg_frm = &regs->reg_frm;
  uint32_t y_size = frm->y_stride * frm->v_stride;
  uint32_t off_u;
  uint32_t off_v;

  /* Per-plane offsets, following MPP setup_vepu510_io_buf().  It sizes the
   * luma plane as hor_stride * ver_stride, i.e. the *allocated* geometry,
   * not the visible one -- so v_stride is a separate field rather than being
   * assumed equal to height.  A caller whose buffer is exactly the picture
   * height passes the same value for both; one whose capture path pads the
   * height does not, and using height there would put the chroma planes at
   * the wrong address.
   */

  switch (frm->src_fmt)
    {
      case RK3576_VEPU510_FMT_YUV420SP:
      case RK3576_VEPU510_FMT_YUV422SP:
      case RK3576_VEPU510_FMT_YUV444SP:
        /* Two interleaved chroma planes share one offset. */
        off_u = y_size;
        off_v = y_size;
        break;

      case RK3576_VEPU510_FMT_YUV420P:
        off_u = y_size;
        off_v = y_size * 5 / 4;
        break;

      case RK3576_VEPU510_FMT_YUV422P:
        off_u = y_size;
        off_v = y_size * 3 / 2;
        break;

      case RK3576_VEPU510_FMT_YUV444P:
        off_u = y_size;
        off_v = y_size * 2;
        break;

      case RK3576_VEPU510_FMT_YUV400:
      case RK3576_VEPU510_FMT_YUYV422:
      case RK3576_VEPU510_FMT_UYVY422:
        /* Packed formats and luma-only carry no separate chroma plane. */
        off_u = 0;
        off_v = 0;
        break;

      default:
        return -ENOSYS;
    }

  /* Source planes.  MPP points all three registers at the same dma-buf and
   * adds the plane offsets through its multi-offset mechanism; with the MMU
   * bypassed we can just add them here.
   */

  reg_frm->common.adr_src0 = frm->src_phys;
  reg_frm->common.adr_src1 = frm->src_phys + off_u;
  reg_frm->common.adr_src2 = frm->src_phys + off_v;

  /* Bitstream buffer.  All four registers are addresses.
   *
   * MPP points every one of them at the same dma-buf and then adjusts two of
   * them through its multi-offset mechanism:
   *
   *   reg172 (bsbt_addr) += siz_out - 1   -> the buffer's last byte
   *   reg174 (adr_bsbs)  += off_out       -> where writing starts
   *
   * and since that mechanism adds to the register's existing value rather
   * than replacing it, those are the addresses the hardware ends up with.
   * With the MMU bypassed the additions are made here, as they are for the
   * source planes.
   *
   * Getting this wrong is not subtle and not loud.  Writing the bare numbers
   * -- the size and the offset instead of the addresses derived from them --
   * points the encoder's buffer top and write start at addresses near zero,
   * which the first frame can survive and later ones cannot.
   *
   * bsbb_addr and bsbr_addr are left at the buffer's first byte: the bottom
   * of the buffer is its start, and nothing has been read back yet.
   */

  reg_frm->common.bsbt_addr = frm->dst_phys + frm->dst_size - 1u;
  reg_frm->common.bsbb_addr = frm->dst_phys;
  reg_frm->common.adr_bsbs = frm->dst_phys + frm->dst_offset;
  reg_frm->common.bsbr_addr = frm->dst_phys;

  /* No reference frame on an all-intra frame.  0xffffffff / 0 are MPP's
   * "no reference" markers -- not arbitrary, so they are kept verbatim.
   */

  reg_frm->common.rfpt_h_addr = 0xffffffff;
  reg_frm->common.rfpb_h_addr = 0;
  reg_frm->common.rfpt_b_addr = 0xffffffff;
  reg_frm->common.adr_rfpb_b = 0;

  /* No motion-estimation info buffer. */

  reg_frm->common.meiw_addr = 0;
  reg_frm->common.enc_pic.mei_stor = 0;

  return OK;
}

void rk3576_vepu510_recn_size(uint32_t width, uint32_t height,
                              FAR struct rk3576_vepu510_recn_size_s *size)
{
  uint32_t aw = RK3576_VEPU510_ALIGN_TO(width, RK3576_VEPU510_RECN_ALIGN_W);
  uint32_t ah = RK3576_VEPU510_ALIGN_TO(height, RK3576_VEPU510_RECN_ALIGN_H) +
                RK3576_VEPU510_RECN_EXTRA_ROWS;

  /* Two cell counts, and the height is deliberately not the same in both.
   *
   *   cols64 -- 64-pixel columns, used by both smaller buffers
   *   rows16 -- 16-pixel rows, used by the anti-smear area
   *
   * The thumbnail does not get a count of its own: it is computed below in
   * the same form the reference uses, and that form is not
   * cols64 * (ah / 64).
   */

  uint32_t cols64 = aw / RK3576_VEPU510_RECN_ALIGN_W;
  uint32_t rows16 = ah / RK3576_VEPU510_RECN_ALIGN_H;

  /* The header describes the FBC layout of the body that follows it, and is
   * sized from the aligned area rather than from the visible picture.
   */

  size->header = RK3576_VEPU510_ALIGN_TO(aw * ah / RK3576_VEPU510_RECN_ALIGN_W,
                                         RK3576_VEPU510_RECN_BUF_ALIGN);

  /* The body holds a 4:2:0 picture at the aligned geometry, plus the rows
   * the 16-row allowance above added.
   */

  size->pixel = size->header + aw * ah * 3u / 2u;

  /* Downscaled copy: one 256-byte cell per 64x64 block of the aligned
   * picture.  It can come out as zero for a picture only a few cells wide,
   * which is the same answer MPP arrives at.
   *
   * Written as one left-to-right chain, and it has to stay that way.  C
   * evaluates it as ((aw / 64) * ah) / 64 * 256, which is *not* the same as
   * (aw / 64) * (ah / 64) * 256: the intermediate product is not always a
   * multiple of 64, so dividing it separately loses the fractional cells
   * twice instead of once.  At 720 lines the grouped form is a sixteenth
   * short of what the encoder expects.
   *
   * The reference written this way is not an oversight to be tidied up.
   */

  size->thumb =
      RK3576_VEPU510_ALIGN_TO((aw / RK3576_VEPU510_RECN_ALIGN_W) * ah /
                                  RK3576_VEPU510_RECN_ALIGN_W * 256u,
                              RK3576_VEPU510_RECN_BUF_ALIGN);

  /* Anti-smear area, counted in whole 16-pixel cells on both axes.
   *
   * Note this one is aligned to 16 rather than to 8 KB, and that it is a
   * product of two separately rounded counts -- rounding the product instead
   * gives a different, too-small buffer.
   */

  size->smear =
      RK3576_VEPU510_ALIGN_TO(cols64, RK3576_VEPU510_RECN_SMALL_ALIGN) *
      RK3576_VEPU510_ALIGN_TO(rows16, RK3576_VEPU510_RECN_SMALL_ALIGN);
}

int rk3576_vepu510_regs_recn(FAR HalVepu510RegSet *regs,
                             FAR const struct rk3576_vepu510_frame_s *frm)
{
  H264eVepu510Frame *reg_frm = &regs->reg_frm;

  /* Reject rather than leave the registers at zero.  Zero is not "off" for
   * these: it is physical address zero, which is where the reconstruction
   * write would then land.  A frame that reaches here without the working
   * set is a bug in the caller, and the only safe thing to do with it is
   * refuse to start the encoder.
   */

  if (frm->recn.pixel_phys == 0 || frm->recn.thumb_phys == 0 ||
      frm->recn.smear_phys == 0)
    {
      return -EINVAL;
    }

  reg_frm->common.rfpw_h_addr = frm->recn.pixel_phys;
  reg_frm->common.rfpw_b_addr = frm->recn.pixel_phys + frm->recn.body_offset;
  reg_frm->common.dspw_addr = frm->recn.thumb_phys;
  reg_frm->common.adr_smear_wr = frm->recn.smear_phys;

  /* The read side.  These are the addresses the encoder fetches the
   * reference picture's reconstructed pixels and downscale from.
   *
   * They are programmed whenever the caller supplies them, which now includes
   * the pictures that predict from nothing.  The vendor stack points those at
   * the very buffer it is writing rather than leaving the read side at zero
   * (its h264e_dpb.c resolves an empty reference slot to the current frame),
   * and this driver follows it -- that is the caller's decision, made in
   * rk3576_vepu.c, and this layer just writes what it is given.
   *
   * A caller that supplies nothing still gets nothing written.  That is the
   * older behaviour and it is what the self-tests rely on, where the register
   * image is built for pictures with no reference at all.
   *
   * Zero is not "off" for these registers: it is physical address zero.  A
   * partially-filled set is therefore still refused rather than programmed.
   */

  if (frm->ref.pixel_phys == 0 && frm->ref.thumb_phys == 0 &&
      frm->ref.smear_phys == 0)
    {
      return OK;
    }

  if (frm->ref.pixel_phys == 0 || frm->ref.thumb_phys == 0 ||
      frm->ref.smear_phys == 0)
    {
      return -EINVAL;
    }

  reg_frm->common.rfpr_h_addr = frm->ref.pixel_phys;
  reg_frm->common.rfpr_b_addr = frm->ref.pixel_phys + frm->ref.body_offset;
  reg_frm->common.dspr_addr = frm->ref.thumb_phys;
  reg_frm->common.adr_smear_rd = frm->ref.smear_phys;

  return OK;
}

/****************************************************************************
 * Name: rk3576_vepu510_frame_num_mask, rk3576_vepu510_poc_lsb_mask
 *
 * Description:
 *   See the header.  Both ranges are a power of two, so the driver reduces
 *   the counters with a mask where MPP compares and resets -- which agree
 *   because frame_num advances by one and the picture order count by two
 *   from an even start, so neither can step over its range.
 *
 ****************************************************************************/

uint32_t rk3576_vepu510_frame_num_mask(FAR const struct rk3576_h264_cfg_s *cfg)
{
  return (1u << (cfg->log2_max_frame_num_minus4 + 4u)) - 1u;
}

uint32_t rk3576_vepu510_poc_lsb_mask(FAR const struct rk3576_h264_cfg_s *cfg)
{
  return (1u << (cfg->log2_max_poc_lsb_minus4 + 4u)) - 1u;
}

/****************************************************************************
 * Name: rk3576_vepu510_regs_codec
 *
 * Description:
 *   Fill the codec (CODEC) block for one picture: which kind of picture it
 *   is, the syntax values its slice header carries, and the reference
 *   marking and reordering commands that go with it.
 *
 *   Mirrors MPP setup_vepu510_codec().
 *
 ****************************************************************************/

int rk3576_vepu510_regs_codec(FAR HalVepu510RegSet *regs,
                              FAR const struct rk3576_h264_cfg_s *cfg,
                              FAR const struct rk3576_vepu510_slice_s *slice)
{
  H264eVepu510Frame *reg_frm = &regs->reg_frm;
  struct rk3576_h264_pps_resolved_s pps;
  int32_t cabac_init_idc;

  if (cfg == NULL || slice == NULL)
    {
      return -EINVAL;
    }

  /* The PPS-level values come from the same resolver the PPS bitstream
   * writer uses, so the registers cannot disagree with the parameter set
   * that was emitted alongside them.
   */

  rk3576_h264_pps_resolve(cfg, &pps);

  /* reg192 ENC_PIC: enc_stnd 0 selects H.264 (2 would select JPEG, which
   * shares this register block).  Every picture this driver encodes is a
   * reference picture -- nothing is produced that later pictures do not
   * predict from -- and bs_scp asks for a start code prefix ahead of the
   * slice.
   */

  reg_frm->common.enc_pic.enc_stnd = 0;
  reg_frm->common.enc_pic.cur_frm_ref = 1;
  reg_frm->common.enc_pic.bs_scp = 1;

  /* reg236 SYNT_NAL: the NAL header of the synthesized slice.  An IDR and
   * an ordinary slice differ in both fields, and an IDR carries the highest
   * reference indicator while every other reference picture carries the one
   * below it -- which is MPP's h264e_slice_update() reading of the two, and
   * what a decoder expects of a stream whose IDRs reset it.
   */

  reg_frm->synt_nal.nal_ref_idc = slice->idr
                                      ? RK3576_H264_NALU_PRIORITY_HIGHEST
                                      : RK3576_H264_NALU_PRIORITY_HIGH;
  reg_frm->synt_nal.nal_unit_type = slice->idr
                                        ? RK3576_H264_NAL_TYPE_IDR_SLICE
                                        : RK3576_H264_NAL_TYPE_NONIDR_SLICE;

  /* reg237 SYNT_SPS: echoed from the sequence parameter set. */

  reg_frm->synt_sps.max_fnum = cfg->log2_max_frame_num_minus4;
  reg_frm->synt_sps.drct_8x8 = cfg->direct8x8_inference ? 1 : 0;
  reg_frm->synt_sps.mpoc_lm4 = cfg->log2_max_poc_lsb_minus4;
  reg_frm->synt_sps.poc_type = cfg->poc_type;

  /* reg238 SYNT_PPS: echoed from the picture parameter set.  num_ref0_idx
   * and num_ref1_idx are the PPS defaults minus one, and the PPS writer
   * always emits 1, so both are zero.
   */

  reg_frm->synt_pps.etpy_mode = pps.entropy_coding_mode;
  reg_frm->synt_pps.trns_8x8 = pps.transform8x8_mode;
  reg_frm->synt_pps.csip_flag = cfg->constrained_intra_pred ? 1 : 0;
  reg_frm->synt_pps.num_ref0_idx = 0;
  reg_frm->synt_pps.num_ref1_idx = 0;
  reg_frm->synt_pps.pic_init_qp = cfg->pic_init_qp;
  reg_frm->synt_pps.cb_ofst = cfg->chroma_cb_qp_offset;
  reg_frm->synt_pps.cr_ofst = pps.second_chroma_qp_offset;
  reg_frm->synt_pps.dbf_cp_flg = cfg->deblocking_filter_control ? 1 : 0;

  /* reg239 SYNT_SLI0: the first part of the slice header.  sli_type 2 is an
   * I slice and 0 is a P slice; the hardware only distinguishes those two,
   * there being no B pictures on this part.  frame_num is the picture's
   * place in its sequence, which an IDR restarts and which the caller
   * therefore supplies.
   *
   * num_ref_ovrd is zero: MPP sets it from slice->num_ref_idx_override, and
   * h264e_slice_update() assigns that zero unconditionally.
   */

  reg_frm->synt_sli0.sli_type = slice->idr ? 2u : 0u;
  reg_frm->synt_sli0.pps_id = 0;
  reg_frm->synt_sli0.drct_smvp = 0;
  reg_frm->synt_sli0.num_ref_ovrd = 0;
  reg_frm->synt_sli0.frm_num = slice->frame_num;

  /* MPP computes this as -1 whenever entropy coding is CAVLC, and the field
   * is two bits wide, so -1 lands as 3.  It is a don't-care for an I slice
   * (cabac_init_idc is not present in an I slice header at all) but it is
   * reproduced unchanged because the register write has to match MPP's.
   *
   * Note which entropy mode decides it: the *raw* configured one, not the
   * resolved one.  MPP's h264e_slice_update() reads h264->entropy_coding_mode
   * here, while the value that got downgraded for baseline is a different
   * field (pps->entropy_coding_mode).  So a baseline configuration that asks
   * for CABAC writes etpy_mode = 0 but cbc_init_idc = 0 rather than 3.  Using
   * the resolved value would look more "consistent" and would be wrong.
   */

  cabac_init_idc =
      (cfg->entropy_coding_mode != 0) ? (int32_t)slice->cabac_init_idc : -1;

  reg_frm->synt_sli0.cbc_init_idc = (uint32_t)cabac_init_idc;

  /* reg240 SYNT_SLI1: the IDR picture id and the picture order count LSB.
   *
   * The picture id is present in an IDR's slice header and absent from
   * every other one, and MPP writes all ones for the pictures that do not
   * have it -- (RK_U32)(-1) into a sixteen-bit field.  The hardware
   * presumably ignores the field when the NAL type says there is no id in
   * it; it is written the way MPP writes it either way, because matching
   * MPP is what this module is verified against.
   */

  reg_frm->synt_sli1.idr_pid = slice->idr ? slice->idr_pic_id : 0xffffu;
  reg_frm->synt_sli1.poc_lsb = slice->poc_lsb;

  /* reg241 SYNT_SLI2: deblocking controls and reference list reordering.
   *
   * A reorder command is what a stream emits when its reference list0 is not
   * in the default order -- the most recent reference picture first.  This
   * driver produces one reference picture per picture and never reorders,
   * so the queue is empty and the fields are zero.  For an I slice the
   * question does not arise at all.
   *
   * sli_beta_ofst is deliberately left alone: MPP never programs it
   * anywhere (the field occurs only in its own definition in the register
   * model), so a configured slice_beta_offset_div2 is silently dropped
   * upstream.  There is no way to configure it here either, for the same
   * reason -- matching MPP is the contract this module is verified against.
   */

  reg_frm->synt_sli2.dis_dblk_idc = slice->deblock_disable;
  reg_frm->synt_sli2.sli_alph_ofst = slice->deblock_offset_alpha;
  reg_frm->synt_sli2.ref_list0_rodr = 0;
  reg_frm->synt_sli2.rodr_pic_idx = 0;
  reg_frm->synt_sli2.rodr_pic_num = 0;

  /* reg242..reg244 SYNT_REFM0/1/2: reference marking.  The whole MMCO area
   * is cleared, and then an I slice sets only these two flags while a P
   * slice sets nothing at all.
   *
   * That asymmetry is MPP's: the adaptive marking machinery past this point
   * is reached only when the marking queue is not empty, and a stream with
   * one reference picture and no reordering has nothing to mark -- the
   * sliding window drops the previous reference by itself when the new one
   * arrives.  nopp_flg is always 0 because MPP assigns
   * slice->no_output_of_prior_pics = 0 unconditionally, and ltrf_flg is
   * zero for a P slice because h264e_slice_update() clears it for anything
   * that is not an IDR.
   */

  reg_frm->synt_refm0.nopp_flg = 0;
  reg_frm->synt_refm0.ltrf_flg =
      (slice->idr && slice->long_term_reference_flag) ? 1 : 0;

  return OK;
}

int rk3576_vepu510_regs_rc_fixqp(FAR HalVepu510RegSet *regs, uint32_t qp)
{
  if (qp > RK3576_H264_QP_MAX)
    {
      return -EINVAL;
    }

  /* reg1036..reg1039 ROI_QTHD0..3: the QP range allowed in each of the eight
   * ROI areas.  All areas get the same pair, so the map is flat.  The fields
   * are 6 bit and unsigned.
   */

  regs->reg_rc_roi.roi_qthd0.qpmin_area0 = qp;
  regs->reg_rc_roi.roi_qthd0.qpmax_area0 = qp;
  regs->reg_rc_roi.roi_qthd0.qpmin_area1 = qp;
  regs->reg_rc_roi.roi_qthd0.qpmax_area1 = qp;
  regs->reg_rc_roi.roi_qthd0.qpmin_area2 = qp;

  regs->reg_rc_roi.roi_qthd1.qpmax_area2 = qp;
  regs->reg_rc_roi.roi_qthd1.qpmin_area3 = qp;
  regs->reg_rc_roi.roi_qthd1.qpmax_area3 = qp;
  regs->reg_rc_roi.roi_qthd1.qpmin_area4 = qp;
  regs->reg_rc_roi.roi_qthd1.qpmax_area4 = qp;

  regs->reg_rc_roi.roi_qthd2.qpmin_area5 = qp;
  regs->reg_rc_roi.roi_qthd2.qpmax_area5 = qp;
  regs->reg_rc_roi.roi_qthd2.qpmin_area6 = qp;
  regs->reg_rc_roi.roi_qthd2.qpmax_area6 = qp;
  regs->reg_rc_roi.roi_qthd2.qpmin_area7 = qp;

  regs->reg_rc_roi.roi_qthd3.qpmax_area7 = qp;

  /* qpmap_mode 1 enables the quantiser map.  MPP sets this before its FIXQP
   * early return, so it applies in fixed-QP mode too; with a flat map it is
   * a no-op in practice.
   */

  regs->reg_rc_roi.roi_qthd3.qpmap_mode = 1;

  /* The per-frame QP, and the min/max pair that bounds it.  rc_cfg.rc_en is
   * deliberately left zero: that is the early return, and it is also the
   * whole point of fixed QP.
   */

  regs->reg_frm.common.enc_pic.pic_qp = qp;
  regs->reg_frm.common.rc_qp.rc_max_qp = qp;
  regs->reg_frm.common.rc_qp.rc_min_qp = qp;

  return OK;
}

int rk3576_vepu510_regs_rdo_pred(FAR HalVepu510RegSet *regs,
                                 FAR const struct rk3576_h264_cfg_s *cfg,
                                 bool slice_is_i)
{
  H264eVepu510Frame *reg_frm = &regs->reg_frm;
  struct rk3576_h264_pps_resolved_s pps;
  uint32_t level_idc;

  if (cfg == NULL)
    {
      return -EINVAL;
    }

  rk3576_h264_pps_resolve(cfg, &pps);
  level_idc = rk3576_h264_level_resolve(cfg);

  /* Chroma lambda offset.  An I slice has a fixed value; a P slice varies
   * with the scene mode.  An all-intra stream only ever takes the first
   * branch, but the second is written out because it is what MPP produces
   * and because leaving it implicit would look like an oversight.
   */

  if (slice_is_i)
    {
      regs->reg_rc_roi.klut_ofst.chrm_klut_ofst = 6;
    }
  else
    {
      regs->reg_rc_roi.klut_ofst.chrm_klut_ofst =
          (cfg->tune.scene_mode == RK3576_H264_SCENE_MODE_IPC) ? 9 : 6;
    }

  /* Rectangle-size RDO shortcut: only allowed at baseline up to level 3.0,
   * where the hardware's restriction is lifted.  Note this reads the
   * *resolved* level, so a configuration asking for level 1.0 with a 1080p
   * frame resolves to level 4.0 and correctly gets the shortcut disabled.
   */

  reg_frm->rdo_cfg.rect_size =
      (cfg->profile_idc == RK3576_H264_PROFILE_BASELINE && level_idc <= 30)
          ? 1
          : 0;

  /* VLC limit: baseline and constrained-baseline profiles with CAVLC only.
   * Reads the resolved entropy mode: a baseline configuration asking for
   * CABAC has it silently downgraded to CAVLC, and the register side must
   * follow the downgrade, or it would program a limit the stream does not
   * need.  This is the opposite of cabac_init_idc in the codec block, which
   * deliberately reads the raw value -- the two are different fields for
   * different purposes, and MPP is inconsistent about it.
   */

  reg_frm->rdo_cfg.vlc_lmt =
      (cfg->profile_idc < RK3576_H264_PROFILE_MAIN && !pps.entropy_coding_mode)
          ? 1
          : 0;

  reg_frm->rdo_cfg.chrm_spcl = 1;
  reg_frm->rdo_cfg.ccwa_e = 1;

  /* Scaling-list selection.  This driver never transmits a scaling list, so
   * the PPS flag is always zero and this stays zero.  It is written out
   * rather than left implicit so the link to the PPS is visible.
   */

  reg_frm->rdo_cfg.scl_lst_sel = 0;

  reg_frm->rdo_cfg.atf_e = (cfg->tune.atf_str > 0) ? 1 : 0;
  reg_frm->rdo_cfg.atr_e = (cfg->tune.atr_str_i > 0) ? 1 : 0;
  reg_frm->rdo_cfg.atr_mult_sel_e = 1;

  reg_frm->iprd_csts.rdo_mark_mode = 0;

  return OK;
}

int rk3576_vepu510_regs_anti_stripe(FAR HalVepu510RegSet *regs,
                                    FAR const struct rk3576_h264_cfg_s *cfg)
{
  H264eVepu510Param *s = &regs->reg_param;
  uint32_t str;
  bool ipc;

  if (cfg == NULL)
    {
      return -EINVAL;
    }

  str = cfg->tune.atl_str;
  ipc = (cfg->tune.scene_mode == RK3576_H264_SCENE_MODE_IPC);

  s->iprd_tthdy4_0.iprd_tthdy4_0 = 1;
  s->iprd_tthdy4_0.iprd_tthdy4_1 = 3;
  s->iprd_tthdy4_1.iprd_tthdy4_2 = 6;
  s->iprd_tthdy4_1.iprd_tthdy4_3 = 8;

  s->iprd_tthdc8_0.iprd_tthdc8_0 = 1;
  s->iprd_tthdc8_0.iprd_tthdc8_1 = 3;
  s->iprd_tthdc8_1.iprd_tthdc8_2 = 6;
  s->iprd_tthdc8_1.iprd_tthdc8_3 = 8;

  s->iprd_tthdy8_0.iprd_tthdy8_0 = 1;
  s->iprd_tthdy8_0.iprd_tthdy8_1 = 3;
  s->iprd_tthdy8_1.iprd_tthdy8_2 = 6;
  s->iprd_tthdy8_1.iprd_tthdy8_3 = 8;

  /* 4095 is MPP's "disable anti-stripe" marker, used outside IPC mode. */

  s->iprd_tthd_ul.iprd_tthd_ul = ipc ? ((str != 0) ? 4 : 255) : 4095;

  s->iprd_wgty8.iprd_wgty8_0 = (str != 0) ? 22 : 16;
  s->iprd_wgty8.iprd_wgty8_1 = (str != 0) ? 23 : 16;
  s->iprd_wgty8.iprd_wgty8_2 = (str != 0) ? 20 : 16;
  s->iprd_wgty8.iprd_wgty8_3 = (str != 0) ? 22 : 16;

  s->iprd_wgty4.iprd_wgty4_0 = (str != 0) ? 22 : 16;
  s->iprd_wgty4.iprd_wgty4_1 = (str != 0) ? 26 : 16;
  s->iprd_wgty4.iprd_wgty4_2 = (str != 0) ? 20 : 16;
  s->iprd_wgty4.iprd_wgty4_3 = (str != 0) ? 22 : 16;

  /* The 16x16 and chroma weight sets ignore the strength entirely. */

  s->iprd_wgty16.iprd_wgty16_0 = 22;
  s->iprd_wgty16.iprd_wgty16_1 = 26;
  s->iprd_wgty16.iprd_wgty16_2 = 20;
  s->iprd_wgty16.iprd_wgty16_3 = 22;

  s->iprd_wgtc8.iprd_wgtc8_0 = 18;
  s->iprd_wgtc8.iprd_wgtc8_1 = 21;
  s->iprd_wgtc8.iprd_wgtc8_2 = 20;
  s->iprd_wgtc8.iprd_wgtc8_3 = 19;

  return OK;
}

int rk3576_vepu510_regs_anti_ringing(FAR HalVepu510RegSet *regs,
                                     FAR const struct rk3576_h264_cfg_s *cfg,
                                     bool slice_is_i)
{
  H264eVepu510Param *s = &regs->reg_param;
  bool ipc;

  if (cfg == NULL)
    {
      return -EINVAL;
    }

  ipc = (cfg->tune.scene_mode == RK3576_H264_SCENE_MODE_IPC);

  s->atr_thd1.thdqp = ipc ? 32 : 45;

  if (slice_is_i)
    {
      s->atr_thd0.thd0 = 1;
      s->atr_thd0.thd1 = 2;
      s->atr_thd1.thd2 = 6;

      s->atr_wgt16.atr_lv16_wgt0 = 16;
      s->atr_wgt16.atr_lv16_wgt1 = 16;
      s->atr_wgt16.atr_lv16_wgt2 = 16;

      if (ipc)
        {
          s->atr_wgt8.atr_lv8_wgt0 = 22;
          s->atr_wgt8.atr_lv8_wgt1 = 21;
          s->atr_wgt8.atr_lv8_wgt2 = 20;

          s->atr_wgt4.atr_lv4_wgt0 = 20;
          s->atr_wgt4.atr_lv4_wgt1 = 18;
          s->atr_wgt4.atr_lv4_wgt2 = 16;
        }
      else
        {
          s->atr_wgt8.atr_lv8_wgt0 = 18;
          s->atr_wgt8.atr_lv8_wgt1 = 17;
          s->atr_wgt8.atr_lv8_wgt2 = 18;

          s->atr_wgt4.atr_lv4_wgt0 = 16;
          s->atr_wgt4.atr_lv4_wgt1 = 16;
          s->atr_wgt4.atr_lv4_wgt2 = 16;
        }
    }
  else if (ipc)
    {
      s->atr_thd0.thd0 = 2;
      s->atr_thd0.thd1 = 4;
      s->atr_thd1.thd2 = 9;

      s->atr_wgt16.atr_lv16_wgt0 = 25;
      s->atr_wgt16.atr_lv16_wgt1 = 20;
      s->atr_wgt16.atr_lv16_wgt2 = 16;

      s->atr_wgt8.atr_lv8_wgt0 = 25;
      s->atr_wgt8.atr_lv8_wgt1 = 20;
      s->atr_wgt8.atr_lv8_wgt2 = 18;

      s->atr_wgt4.atr_lv4_wgt0 = 25;
      s->atr_wgt4.atr_lv4_wgt1 = 20;
      s->atr_wgt4.atr_lv4_wgt2 = 16;
    }
  else
    {
      s->atr_thd0.thd0 = 1;
      s->atr_thd0.thd1 = 2;
      s->atr_thd1.thd2 = 7;

      s->atr_wgt16.atr_lv16_wgt0 = 23;
      s->atr_wgt16.atr_lv16_wgt1 = 22;
      s->atr_wgt16.atr_lv16_wgt2 = 20;

      s->atr_wgt8.atr_lv8_wgt0 = 24;
      s->atr_wgt8.atr_lv8_wgt1 = 24;
      s->atr_wgt8.atr_lv8_wgt2 = 24;

      s->atr_wgt4.atr_lv4_wgt0 = 23;
      s->atr_wgt4.atr_lv4_wgt1 = 22;
      s->atr_wgt4.atr_lv4_wgt2 = 20;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vepu510_regs_anti_smear
 *
 * Description:
 *   See the header.  Ported from MPP setup_vepu510_anti_smear() with its
 *   branch structure intact.
 *
 *   Two things about the arithmetic are worth knowing before reading it,
 *   because both look like mistakes and neither is.
 *
 *   The first is that a negative quantiser delta is stored in a four-bit
 *   unsigned field.  MPP assigns the signed value to that field and lets the
 *   conversion wrap, so -8 is programmed as 8.  Reproducing the expression
 *   rather than the wrapped value is what keeps the two in step: the field
 *   widths are the same on both sides, so the same assignment truncates the
 *   same way.
 *
 *   The second is that the counts are compared against fractions of the
 *   picture in units of four macroblocks, so they are only meaningful next to
 *   the macroblock count of the picture they came from -- which is why that
 *   count travels with them rather than being recomputed here.
 *
 ****************************************************************************/

int rk3576_vepu510_regs_anti_smear(
    FAR HalVepu510RegSet *regs, FAR const struct rk3576_h264_cfg_s *cfg,
    bool slice_is_i, FAR const struct rk3576_vepu510_feedback_s *prev)
{
  H264eVepu510Sqi *reg = &regs->reg_sqi;
  FAR const uint32_t *smear_cnt;
  uint32_t mb_cnt;
  uint32_t deblur_str;
  bool ipc;
  bool qpmap;
  int32_t delta_qp = 0;
  int32_t flg0;
  int32_t flg1 = 1;
  int32_t flg2 = 0;
  int32_t flg3 = 0;
  int32_t smear_multi[4] = { 9, 12, 16, 16 };
  uint32_t max012;

  if (cfg == NULL || prev == NULL)
    {
      return -EINVAL;
    }

  if (cfg->tune.deblur_str > RK3576_H264_DEBLUR_STR_MAX)
    {
      return -EINVAL;
    }

  mb_cnt = prev->mb_num;
  smear_cnt = prev->smear_cnt;
  deblur_str = cfg->tune.deblur_str;
  ipc = (cfg->tune.scene_mode == RK3576_H264_SCENE_MODE_IPC);
  qpmap = (cfg->tune.deblur_en != 0);

  flg0 = smear_cnt[4] < (mb_cnt >> 6);

  /* The three-way maximum is written out rather than taken from a macro, so
   * that the comparison the flags rest on is visible where it is made.
   */

  max012 = smear_cnt[0] > smear_cnt[1] ? smear_cnt[0] : smear_cnt[1];
  if (smear_cnt[2] > max012)
    {
      max012 = smear_cnt[2];
    }

  if (smear_cnt[3] < ((5u * mb_cnt) >> 10) ||
      smear_cnt[3] < ((1126u * max012) >> 10) || deblur_str == 6 ||
      deblur_str == 7)
    {
      flg1 = 0;
    }

  flg3 = flg1                                       ? 3
         : (smear_cnt[4] > ((102u * mb_cnt) >> 10)) ? 2
         : (smear_cnt[4] > ((66u * mb_cnt) >> 10))  ? 1
                                                    : 0;

  if (ipc)
    {
      reg->smear_opt_cfg.rdo_smear_en = qpmap ? 1u : 0u;

      if (qpmap && deblur_str > 3)
        {
          reg->smear_opt_cfg.rdo_smear_lvl16_multi =
              (uint32_t)smear_multi[flg3];
        }
      else
        {
          reg->smear_opt_cfg.rdo_smear_lvl16_multi = (flg0 != 0) ? 9u : 12u;
        }
    }
  else
    {
      reg->smear_opt_cfg.rdo_smear_en = 0;
      reg->smear_opt_cfg.rdo_smear_lvl16_multi = 16;
    }

  if (qpmap && deblur_str > 3)
    {
      flg2 = 1;

      if (smear_cnt[2] + smear_cnt[3] > (3u * smear_cnt[4] / 4u))
        {
          delta_qp = 1;
        }

      if (smear_cnt[4] < (mb_cnt >> 4))
        {
          delta_qp -= 8;
        }
      else if (smear_cnt[4] < ((3u * mb_cnt) >> 5))
        {
          delta_qp -= 7;
        }
      else
        {
          delta_qp -= 6;
        }

      if (flg3 == 2)
        {
          delta_qp = 0;
        }
      else if (flg3 == 1)
        {
          delta_qp = -2;
        }
    }
  else
    {
      if (smear_cnt[2] + smear_cnt[3] > smear_cnt[4] / 2u)
        {
          delta_qp = 1;
        }

      if (smear_cnt[4] < (mb_cnt >> 8))
        {
          delta_qp -= (deblur_str < 2) ? 6 : 8;
        }
      else if (smear_cnt[4] < (mb_cnt >> 7))
        {
          delta_qp -= (deblur_str < 2) ? 5 : 6;
        }
      else if (smear_cnt[4] < (mb_cnt >> 6))
        {
          delta_qp -= (deblur_str < 2) ? 3 : 4;
        }
      else
        {
          delta_qp -= 1;
        }
    }

  reg->smear_opt_cfg.rdo_smear_dlt_qp = (uint32_t)delta_qp;

  /* The one field here that is not a threshold but a mode, and the reason
   * this block cannot be left zeroed: 1 is the state that means "this picture
   * or the one before it stands on its own", and MPP uses it for the first
   * picture of a group as well as for the IDR itself.  A driver that wrote 0
   * would be telling the encoder something MPP never says.
   */

  if (slice_is_i || prev->frame_type == RK3576_H264_SLICE_I)
    {
      reg->smear_opt_cfg.stated_mode = 1;
    }
  else
    {
      reg->smear_opt_cfg.stated_mode = 2;
    }

  reg->smear_madp_thd0.madp_cur_thd0 = 0;
  reg->smear_madp_thd0.madp_cur_thd1 = (flg2 != 0) ? 48u : 24u;
  reg->smear_madp_thd1.madp_cur_thd2 = (flg2 != 0) ? 64u : 48u;
  reg->smear_madp_thd1.madp_cur_thd3 = (flg2 != 0) ? 72u : 64u;
  reg->smear_madp_thd2.madp_around_thd0 = (flg2 != 0) ? 4095u : 16u;
  reg->smear_madp_thd2.madp_around_thd1 = 32;
  reg->smear_madp_thd3.madp_around_thd2 = 48;
  reg->smear_madp_thd3.madp_around_thd3 = (flg2 != 0) ? 0u : 96u;
  reg->smear_madp_thd4.madp_around_thd4 = 48;
  reg->smear_madp_thd4.madp_around_thd5 = 24;
  reg->smear_madp_thd5.madp_ref_thd0 = (flg2 != 0) ? 64u : 96u;
  reg->smear_madp_thd5.madp_ref_thd1 = 48;

  reg->smear_cnt_thd0.cnt_cur_thd0 = (flg2 != 0) ? 2u : 1u;
  reg->smear_cnt_thd0.cnt_cur_thd1 = (flg2 != 0) ? 5u : 3u;
  reg->smear_cnt_thd0.cnt_cur_thd2 = 1;
  reg->smear_cnt_thd0.cnt_cur_thd3 = 3;
  reg->smear_cnt_thd1.cnt_around_thd0 = 1;
  reg->smear_cnt_thd1.cnt_around_thd1 = 4;
  reg->smear_cnt_thd1.cnt_around_thd2 = 1;
  reg->smear_cnt_thd1.cnt_around_thd3 = 4;
  reg->smear_cnt_thd2.cnt_around_thd4 = 0;
  reg->smear_cnt_thd2.cnt_around_thd5 = 3;
  reg->smear_cnt_thd2.cnt_around_thd6 = 0;
  reg->smear_cnt_thd2.cnt_around_thd7 = 3;
  reg->smear_cnt_thd3.cnt_ref_thd0 = 1;
  reg->smear_cnt_thd3.cnt_ref_thd1 = 3;

  reg->smear_resi_thd0.resi_small_cur_th0 = 6;
  reg->smear_resi_thd0.resi_big_cur_th0 = 9;
  reg->smear_resi_thd0.resi_small_cur_th1 = 6;
  reg->smear_resi_thd0.resi_big_cur_th1 = 9;
  reg->smear_resi_thd1.resi_small_around_th0 = 6;
  reg->smear_resi_thd1.resi_big_around_th0 = 11;
  reg->smear_resi_thd1.resi_small_around_th1 = 6;
  reg->smear_resi_thd1.resi_big_around_th1 = 8;
  reg->smear_resi_thd2.resi_small_around_th2 = 9;
  reg->smear_resi_thd2.resi_big_around_th2 = 20;
  reg->smear_resi_thd2.resi_small_around_th3 = 6;
  reg->smear_resi_thd2.resi_big_around_th3 = 20;
  reg->smear_resi_thd3.resi_small_ref_th0 = 7;
  reg->smear_resi_thd3.resi_big_ref_th0 = 16;
  reg->smear_resi_thd4.resi_th0 = (flg2 != 0) ? 0u : 10u;
  reg->smear_resi_thd4.resi_th1 = (flg2 != 0) ? 0u : 6u;

  reg->smear_st_thd.madp_cnt_th0 = (flg2 != 0) ? 0u : 1u;
  reg->smear_st_thd.madp_cnt_th1 = (flg2 != 0) ? 0u : 5u;
  reg->smear_st_thd.madp_cnt_th2 = (flg2 != 0) ? 0u : 1u;
  reg->smear_st_thd.madp_cnt_th3 = (flg2 != 0) ? 0u : 3u;

  /* The three fields the block above derives from the previous picture's
   * status are written with constants instead -- MPP 1.0.6's.
   *
   * This is not a tuning preference.  A P picture used to come back differing
   * from its source by exactly the reference's own local mean, in every
   * plane, on every picture that read a reference.  With these three fields
   * at 1.0.6's values that stopped: the leak fell from 38.2 to 0.14 in luma
   * and from 59.5 to 0.02 in chroma, against 0.17 and 0.012 for MPP itself on
   * the same source, with the I pictures coming out bit-identical.  Two other
   * switches that moved in the same build were excluded afterwards by a run
   * that held them fixed, and the status word this reads was confirmed to be
   * at the offset MPP's own Vepu510Status puts it at.
   *
   * Why 1.0.6 rather than develop.  The board runs MPP 1.0.6, and 1.0.6's
   * setup_vepu510_sqi() takes the register block and nothing else -- it has
   * no state, so all three of these are constants there.  develop gave the
   * block the previous picture's status and made them derive from it, and on
   * this silicon that derivation describes the reference wrongly.  The values
   * 1.0.6 writes cannot be reached from the arithmetic above under any
   * configuration, so this is a difference between two MPP versions rather
   * than a setting this driver got wrong.
   *
   * The arithmetic above is left in place rather than deleted, because it is
   * the record of what the difference is: deleting these four lines restores
   * develop's behaviour and the fault, which is how the two were compared.
   *
   * The thresholds are deliberately untouched.  At this driver's tuning they
   * already agree with 1.0.6 field for field, so changing them would put a
   * second variable into the same experiment.  One difference is known to
   * remain unexamined for that same reason: the anti-flicker tables are
   * indexed by a strength this driver defaults to 1, where 1.0.6's constants
   * correspond to 3.  It is left alone because the fault is already gone
   * without it.
   */

  reg->smear_opt_cfg.rdo_smear_en = 0;
  reg->smear_opt_cfg.rdo_smear_lvl16_multi = 9;
  reg->smear_opt_cfg.rdo_smear_dlt_qp = 0;
  reg->smear_opt_cfg.stated_mode = 0;

  return OK;
}

int rk3576_vepu510_regs_anti_flicker(FAR HalVepu510RegSet *regs,
                                     FAR const struct rk3576_h264_cfg_s *cfg)
{
  H264eVepu510Sqi *reg = &regs->reg_sqi;
  rdo_skip_par *p_skip;
  rdo_noskip_par *p_no_skip;
  uint32_t str;

  if (cfg == NULL)
    {
      return -EINVAL;
    }

  str = cfg->tune.atf_str;

  /* Belt and braces: the strength indexes a four-column table, and reading
   * past the end would be a silent out-of-bounds read in the firmware.  The
   * syntax layer checks this too, but this function is public.
   */

  if (str > RK3576_H264_STR_MAX)
    {
      return -EINVAL;
    }

  /* Skip-block (P frame) thresholds and weights. */

  p_skip = &reg->rdo_b16_skip;

  p_skip->atf_thd0.madp_thd0 = rk3576_vepu510_pskip_atf_thd[0][str];
  p_skip->atf_thd0.madp_thd1 = rk3576_vepu510_pskip_atf_thd[1][str];
  p_skip->atf_thd1.madp_thd2 = 15;
  p_skip->atf_thd1.madp_thd3 = 25;

  p_skip->atf_wgt0.wgt0 = rk3576_vepu510_pskip_atf_wgt[0][str];
  p_skip->atf_wgt0.wgt1 = rk3576_vepu510_pskip_atf_wgt[1][str];
  p_skip->atf_wgt0.wgt2 = 16;
  p_skip->atf_wgt0.wgt3 = 16;
  p_skip->atf_wgt1.wgt4 = 16;

  /* Inter blocks (P frame), fixed values. */

  p_no_skip = &reg->rdo_b16_inter;

  p_no_skip->ratf_thd0.madp_thd0 = 20;
  p_no_skip->ratf_thd0.madp_thd1 = 40;
  p_no_skip->ratf_thd1.madp_thd2 = 72;

  p_no_skip->atf_wgt.wgt0 = 16;
  p_no_skip->atf_wgt.wgt1 = 16;
  p_no_skip->atf_wgt.wgt2 = 16;
  p_no_skip->atf_wgt.wgt3 = 16;

  /* Intra blocks: the set that matters for an all-intra stream, and the
   * only one of the three that is strength dependent. */

  p_no_skip = &reg->rdo_b16_intra;

  p_no_skip->ratf_thd0.madp_thd0 = rk3576_vepu510_intra_atf_thd[0][str];
  p_no_skip->ratf_thd0.madp_thd1 = rk3576_vepu510_intra_atf_thd[1][str];
  p_no_skip->ratf_thd1.madp_thd2 = rk3576_vepu510_intra_atf_thd[2][str];

  p_no_skip->atf_wgt.wgt0 = rk3576_vepu510_intra_atf_wgt[0][str];
  p_no_skip->atf_wgt.wgt1 = rk3576_vepu510_intra_atf_wgt[1][str];
  p_no_skip->atf_wgt.wgt2 = rk3576_vepu510_intra_atf_wgt[2][str];
  p_no_skip->atf_wgt.wgt3 = 16;

  reg->rdo_b16_intra_atf_cnt_thd.thd0 = 1;
  reg->rdo_b16_intra_atf_cnt_thd.thd1 = 4;
  reg->rdo_b16_intra_atf_cnt_thd.thd2 = 1;
  reg->rdo_b16_intra_atf_cnt_thd.thd3 = 4;

  reg->rdo_atf_resi_thd.big_th0 = 16;
  reg->rdo_atf_resi_thd.big_th1 = 16;
  reg->rdo_atf_resi_thd.small_th0 = 8;
  reg->rdo_atf_resi_thd.small_th1 = 8;

  return OK;
}

int rk3576_vepu510_regs_l2(FAR HalVepu510RegSet *regs,
                           FAR const struct rk3576_h264_cfg_s *cfg)
{
  const uint32_t *lambda;
  uint32_t idx;

  if (cfg == NULL)
    {
      return -EINVAL;
    }

  idx = cfg->tune.lambda_idx_i;

  /* The copy below takes RK3576_VEPU510_LAMBDA_COPY_LEN entries from idx, so
   * idx has to leave that many in the table.  This is the same bound MPP
   * applies.
   */

  if (idx > RK3576_VEPU510_LAMBDA_IDX_MAX)
    {
      return -EINVAL;
    }

  /* MPP picks between the two tables on scene mode, and the names do not say
   * which way round: honouring its actual source, the table called "default"
   * is the one used *in* IPC mode and the one called "cvr" is used outside
   * it.  Reading the names and assuming the obvious mapping is wrong, and
   * quietly so -- both tables are plausible, so the encoder still runs, just
   * with the wrong rate-distortion curve.
   */

  lambda = (cfg->tune.scene_mode == RK3576_H264_SCENE_MODE_IPC)
               ? rk3576_vepu510_lambda_default_60
               : rk3576_vepu510_lambda_cvr_60;

  memcpy(&regs->reg_param.common.rdo_wgta_qp_grpa_0_51[0], &lambda[idx],
         RK3576_VEPU510_LAMBDA_COPY_LEN * sizeof(uint32_t));

  /* Quantiser QP bias.  With the enable flag clear MPP substitutes the fixed
   * pair below rather than leaving the fields zero, so "disabled" still
   * writes something -- which is why this is not simply skipped.
   */

  if (cfg->qbias.en)
    {
      regs->reg_param.qnt_bias_comb.qnt_f_bias_i = cfg->qbias.i;
      regs->reg_param.qnt_bias_comb.qnt_f_bias_p = cfg->qbias.p;
    }
  else
    {
      regs->reg_param.qnt_bias_comb.qnt_f_bias_i = RK3576_H264_QBIAS_I_DEFAULT;
      regs->reg_param.qnt_bias_comb.qnt_f_bias_p = RK3576_H264_QBIAS_P_DEFAULT;
    }

  return OK;
}

int rk3576_vepu510_regs_aq(FAR HalVepu510RegSet *regs, bool slice_is_i)
{
  Vepu510RcRoi *s = &regs->reg_rc_roi;
  const int32_t *step;
  uint8_t i;

  /* Thresholds.  The register field is eight bits wide and MPP floors each
   * value with 0x1f, which looks like a mask but only strips anything above
   * 31; the defaults never reach it.  Applied as MPP applies it, so a future
   * default above 31 would be floored identically.
   */

  for (i = 0; i < 16; i++)
    {
      s->aq_tthd[i] = (uint8_t)(rk3576_vepu510_aq_tthd_default[i] & 0x1f);
    }

  /* Per-block-type QP steps.  The register fields are signed and five bits
   * wide, so the values run -16..15; MPP's defaults use -8..8.  MPP masks
   * with 0x1f before assigning, which is a no-op for its defaults -- the
   * signed field does the sign extension back.  Reproduced as written.
   */

  step = slice_is_i ? rk3576_vepu510_aq_step_i_default
                    : rk3576_vepu510_aq_step_p_default;

  s->aq_stp0.aq_stp_s0 = step[0] & 0x1f;
  s->aq_stp0.aq_stp_0t1 = step[1] & 0x1f;
  s->aq_stp0.aq_stp_1t2 = step[2] & 0x1f;
  s->aq_stp0.aq_stp_2t3 = step[3] & 0x1f;
  s->aq_stp0.aq_stp_3t4 = step[4] & 0x1f;
  s->aq_stp0.aq_stp_4t5 = step[5] & 0x1f;

  s->aq_stp1.aq_stp_5t6 = step[6] & 0x1f;
  s->aq_stp1.aq_stp_6t7 = step[7] & 0x1f;

  /* Note the anomaly: MPP assigns a literal 0 to this one field rather than a
   * table entry, so the table's 16 entries drive 16 of the 17 step fields and
   * `7t8` is fixed at zero.  It is reproduced as written -- the field is not
   * simply the sixteenth entry, and reading the sequence of assignments as a
   * straight table copy would get the tail wrong.
   */

  s->aq_stp1.aq_stp_7t8 = 0;
  s->aq_stp1.aq_stp_8t9 = step[8] & 0x1f;
  s->aq_stp1.aq_stp_9t10 = step[9] & 0x1f;
  s->aq_stp1.aq_stp_10t11 = step[10] & 0x1f;

  s->aq_stp2.aq_stp_11t12 = step[11] & 0x1f;
  s->aq_stp2.aq_stp_12t13 = step[12] & 0x1f;
  s->aq_stp2.aq_stp_13t14 = step[13] & 0x1f;
  s->aq_stp2.aq_stp_14t15 = step[14] & 0x1f;
  s->aq_stp2.aq_stp_b15 = step[15] & 0x1f;

  return OK;
}

int rk3576_vepu510_regs_me(FAR HalVepu510RegSet *regs,
                           FAR const struct rk3576_h264_cfg_s *cfg)
{
  H264eVepu510Frame *reg_frm = &regs->reg_frm;
  H264eVepu510Param *reg_param = &regs->reg_param;
  bool ipc;

  if (cfg == NULL)
    {
      return -EINVAL;
    }

  ipc = (cfg->tune.scene_mode == RK3576_H264_SCENE_MODE_IPC);

  /* Search ranges and cost configuration. */

  reg_frm->common.me_rnge.cime_srch_dwnh = 15;
  reg_frm->common.me_rnge.cime_srch_uph = 15;
  reg_frm->common.me_rnge.cime_srch_rgtw = 12;
  reg_frm->common.me_rnge.cime_srch_lftw = 12;
  reg_frm->common.me_rnge.dlt_frm_num = 0x0;

  reg_frm->common.me_cfg.rme_srch_h = 3;
  reg_frm->common.me_cfg.rme_srch_v = 3;
  reg_frm->common.me_cfg.srgn_max_num = 54;
  reg_frm->common.me_cfg.cime_dist_thre = 1024;
  reg_frm->common.me_cfg.rme_dis = 0;
  reg_frm->common.me_cfg.fme_dis = 0;

  reg_frm->common.me_cach.cime_zero_thre = 64;

  /* Integer-pel motion estimation. */

  reg_param->common.me_sqi_comb.cime_pmv_num = 1;
  reg_param->common.me_sqi_comb.cime_fuse = 1;
  reg_param->common.me_sqi_comb.itp_mode = 0;
  reg_param->common.me_sqi_comb.move_lambda = 0;
  reg_param->common.me_sqi_comb.rime_lvl_mrg = 1;
  reg_param->common.me_sqi_comb.rime_prelvl_en = 0;
  reg_param->common.me_sqi_comb.rime_prersu_en = 0;

  reg_param->common.cime_mvd_th_comb.cime_mvd_th0 = 16;
  reg_param->common.cime_mvd_th_comb.cime_mvd_th1 = 48;
  reg_param->common.cime_mvd_th_comb.cime_mvd_th2 = 80;

  reg_param->common.cime_madp_th_comb.cime_madp_th = 16;

  reg_param->common.cime_multi_comb.cime_multi0 = 8;
  reg_param->common.cime_multi_comb.cime_multi1 = 12;
  reg_param->common.cime_multi_comb.cime_multi2 = 16;
  reg_param->common.cime_multi_comb.cime_multi3 = 20;

  /* Reference-frame motion estimation. */

  reg_param->common.rime_mvd_th_comb.rime_mvd_th0 = 1;
  reg_param->common.rime_mvd_th_comb.rime_mvd_th1 = 2;
  reg_param->common.rime_mvd_th_comb.fme_madp_th = 0;

  reg_param->common.rime_madp_th_comb.rime_madp_th0 = 8;
  reg_param->common.rime_madp_th_comb.rime_madp_th1 = 16;

  reg_param->common.rime_multi_comb.rime_multi0 = 4;
  reg_param->common.rime_multi_comb.rime_multi1 = 8;
  reg_param->common.rime_multi_comb.rime_multi2 = 12;

  reg_param->common.cmv_st_th_comb.cmv_th0 = 64;
  reg_param->common.cmv_st_th_comb.cmv_th1 = 96;
  reg_param->common.cmv_st_th_comb.cmv_th2 = 128;

  /* Outside IPC mode MPP disables what it calls subjective optimisation: the
   * MADP thresholds go to zero and the multi tables flatten to 4.  This is
   * not a weaker setting but a different one, so it is a branch rather than a
   * scaled constant.
   */

  if (!ipc)
    {
      reg_param->common.cime_madp_th_comb.cime_madp_th = 0;
      reg_param->common.rime_madp_th_comb.rime_madp_th0 = 0;
      reg_param->common.rime_madp_th_comb.rime_madp_th1 = 0;

      reg_param->common.cime_multi_comb.cime_multi0 = 4;
      reg_param->common.cime_multi_comb.cime_multi1 = 4;
      reg_param->common.cime_multi_comb.cime_multi2 = 4;
      reg_param->common.cime_multi_comb.cime_multi3 = 4;

      reg_param->common.rime_multi_comb.rime_multi0 = 4;
      reg_param->common.rime_multi_comb.rime_multi1 = 4;
      reg_param->common.rime_multi_comb.rime_multi2 = 4;
    }

  /* Static-detection thresholds, which live in the rate-control block. */

  regs->reg_rc_roi.madi_st_thd.madi_th0 = 5;
  regs->reg_rc_roi.madi_st_thd.madi_th1 = 12;
  regs->reg_rc_roi.madi_st_thd.madi_th2 = 20;

  regs->reg_rc_roi.madp_st_thd0.madp_th0 = 4 << 4;
  regs->reg_rc_roi.madp_st_thd0.madp_th1 = 9 << 4;

  regs->reg_rc_roi.madp_st_thd1.madp_th2 = 15 << 4;

  return OK;
}

/* Register image completeness.
 *
 * MPP builds the image in hal_h264e_vepu510_gen_regs().  Every call it makes
 * unconditionally is either implemented above, or proven a no-op by
 * check_omissions() in the reference harness (slice split, dual core, scaling
 * lists).  Four further calls in that function sit behind predicates that no
 * other check covers, because they depend on input objects rather than on
 * configuration.  They are recorded here so that the accounting is complete
 * and so that anything which later makes one of them reachable is recognised
 * as a change to this driver, not just to MPP.
 *
 *   vepu510_h264e_tune_reg_patch (line 2096)
 *       gated on  ctx->qpmap_en && task->md_info != NULL
 *       ctx->qpmap_en is cfg->tune.deblur_en, which this driver leaves at its
 *       default of zero.  md_info is the motion/deblur map the application
 *       supplies with the input frame; this driver produces no such map.
 *       Either gate alone is sufficient to skip it.
 *
 *   vepu510_h264e_save_pass1_patch (line 2100), _use_pass1_patch (line 2103)
 *       gated on  frm->save_pass1, frm->use_pass1
 *       Both are set only by the two-pass rate controller.  This driver is
 *       fixed-QP, so neither is ever raised.
 *
 *   vepu510_set_roi (line 2093)
 *       gated on  ctx->roi_data != NULL
 *       External ROI data, an application-supplied object of the same kind as
 *       md_info.  Not produced by this driver.
 *
 *   setup_vepu510_intra_refresh (line 2088)
 *       gated on  frm_status->is_i_refresh
 *       Intra refresh needs a P-slice schedule, which an all-intra stream does
 *       not have.
 *
 * The call that is neither one of those: setup_vepu510_recn_refr (line 2076)
 * programs the reconstructed-frame write and reference read addresses from
 * driver-allocated buffers.  It is implemented as rk3576_vepu510_regs_recn()
 * above, called from rk3576_vepu510_regs_build(), and the working sets it
 * needs come from rk3576_vepu_recn_alloc() in rk3576_vepu.c.  The read side it
 * programs is exercised only by a picture that predicts, so an all-intra
 * stream leaves those registers at zero and never says whether the fetch
 * works. */

int rk3576_vepu510_regs_build(FAR HalVepu510RegSet *regs,
                              FAR const struct rk3576_vepu510_frame_s *frm,
                              FAR const struct rk3576_h264_cfg_s *cfg,
                              FAR const struct rk3576_vepu510_slice_s *slice)
{
  int ret;

  memset(regs, 0, sizeof(*regs));

  rk3576_vepu510_regs_ctl(regs);

  ret = rk3576_vepu510_regs_src(regs, frm);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_vepu510_regs_addr(regs, frm);
  if (ret < 0)
    {
      return ret;
    }

  /* The reconstruction working set is pointed at right after the buffers, as
   * MPP does: setup_vepu510_io_buf() then setup_vepu510_recn_refr().
   */

  ret = rk3576_vepu510_regs_recn(regs, frm);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_vepu510_regs_codec(regs, cfg, slice);
  if (ret < 0)
    {
      return ret;
    }

  /* The frame QP, not pic_init_qp: the latter is the PPS reference and is
   * echoed into the codec block separately above.
   */

  ret = rk3576_vepu510_regs_rc_fixqp(regs, cfg->frame_qp);
  if (ret < 0)
    {
      return ret;
    }

  /* Coding-tool selection.  Three of the blocks below are chosen by slice
   * type in MPP rather than being fixed: the RDO and anti-ringing parameters
   * and the adaptive-quantisation tables all have an I and a P set.  Leaving
   * any of them at its I-slice values would encode every P picture with the
   * settings chosen for a picture that is nothing but intra prediction --
   * which the encoder would accept without complaint, and which no test that
   * only looked at whether the picture decoded could tell from the right
   * ones.
   */

  ret = rk3576_vepu510_regs_rdo_pred(regs, cfg, slice->idr);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_vepu510_regs_anti_stripe(regs, cfg);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_vepu510_regs_anti_ringing(regs, cfg, slice->idr);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_vepu510_regs_anti_flicker(regs, cfg);
  if (ret < 0)
    {
      return ret;
    }

  /* Anti-smear, which is the only block here programmed from state the
   * hardware reported rather than from configuration: its thresholds are
   * derived from what the previous picture said about how much of itself it
   * had to refresh.  It is built for every picture, including the first,
   * whose "previous" is the zeroed feedback the caller supplies.
   */

  ret = rk3576_vepu510_regs_anti_smear(regs, cfg, slice->idr, &frm->prev);
  if (ret < 0)
    {
      return ret;
    }

  /* The lambda table last: it is the largest block either way, and keeping it
   * at the end leaves the per-picture fields above grouped together.
   */

  ret = rk3576_vepu510_regs_l2(regs, cfg);
  if (ret < 0)
    {
      return ret;
    }

  /* Adaptive quantisation and motion estimation.  Neither does anything
   * worth having for a still picture, and MPP programs both unconditionally;
   * they are here so the register image matches MPP's rather than being a
   * hand-picked subset of it.  Adaptive quantisation is the third of the
   * slice-type-dependent blocks.
   */

  ret = rk3576_vepu510_regs_aq(regs, slice->idr);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_vepu510_regs_me(regs, cfg);
}

void rk3576_vepu510_status_decode(uint32_t bs_lgth_l32, uint32_t sse_bsl,
                                  uint32_t sse_h32, FAR uint32_t *bs_length,
                                  FAR uint32_t *sse)
{
  if (bs_length != NULL)
    {
      /* The length is wider than 32 bits on paper -- its top eight bits share
       * the register the distortion figure starts in -- but no buffer this
       * driver can be handed holds 4 GB, so the low word is the whole value.
       * MPP reads it the same way.
       */

      *bs_length = bs_lgth_l32;
    }

  if (sse != NULL)
    {
      /* Reassembled, and it has to be: the two halves are not in a place-value
       * relationship, so a truncated read is not a rounded answer but an
       * unrelated one.  The low half sits in the top half of its register,
       * sharing it with the length's high bits.
       *
       * MPP writes this as ((sse_h32 << 16) + (sse_l16 & 0xffff)); the mask
       * there is redundant on a 16-bit field, so an OR says the same thing
       * without implying the field might be wider than it is.
       */

      *sse = (sse_h32 << RK3576_VEPU510_ST_SSE_HIGH_SHIFT) |
             ((sse_bsl >> RK3576_VEPU510_ST_SSE_LOW_SHIFT) &
              RK3576_VEPU510_ST_SSE_LOW_MASK);
    }
}

/****************************************************************************
 * Name: rk3576_vepu510_status_smear
 *
 * Description:
 *   See the header.  The four counts are eight-bit fields of the status
 *   word's four bytes, and the fifth entry is their sum -- MPP's own
 *   arrangement, kept because the anti-smear block compares that sum against
 *   fractions of the picture.
 *
 ****************************************************************************/

void rk3576_vepu510_status_smear(uint32_t smear_cnt_reg, FAR uint32_t *out)
{
  uint32_t i;

  for (i = 0; i < 4; i++)
    {
      /* Scaled by four as MPP scales it: the encoder counts in units of four
       * macroblocks, so the raw field is a quarter of the count that the
       * thresholds are expressed in.  Writing the shift as a multiply keeps
       * the correspondence with the reference visible.
       */

      out[i] = ((smear_cnt_reg >> (8u * i)) & 0xffu) *
               RK3576_VEPU510_ST_SMEAR_CNT_SCALE;
    }

  out[4] = out[0] + out[1] + out[2] + out[3];
}

#endif /* CONFIG_RK3576_VEPU */
