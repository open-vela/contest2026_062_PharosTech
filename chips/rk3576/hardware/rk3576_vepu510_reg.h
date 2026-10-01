/****************************************************************************
 * chips/rk3576/hardware/rk3576_vepu510_reg.h
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
 * RK3576 VEPU510 register layout (the register model the encoder writes).
 *
 * GENERATED FILE -- do not edit by hand.
 *   Regenerate with:  rk3576-mpp-ref/port_regmodel.py
 *   Verify with:      rk3576-mpp-ref/port_regmodel.py --check
 *
 * These structs are ported from the Rockchip MPP userspace library, which is
 * what actually drives this IP on Linux:
 *
 *   Source : rockchip-linux/mpp
 *   Branch : develop
 *   Commit : 14729dd578e570e5f00fd1dd2113f5429012d64b
 *   Files  : mpp/hal/rkenc/common/vepu5xx_common.h
 *            mpp/hal/rkenc/common/vepu51x_common.h
 *            mpp/hal/rkenc/common/vepu510_common.h
 *            mpp/hal/rkenc/h264e/hal_h264e_vepu510_reg.h
 *   Licence: Apache-2.0 (the same licence as this file)
 *
 * A local copy of the unmodified upstream sources is kept in
 * <workspace>/rk3576-mpp-ref/ for reference and for the generated-file check.
 *
 * The register-address comments of the form "0x00000270 reg156" are the
 * upstream ones and are kept on purpose: they tie every struct to a register
 * address, which is what makes the layout checkable (see the --check and
 * --compare modes of the port script).
 *
 * The structs are written to the device as whole register blocks, so the
 * member order and the total size matter more than the names.
 *
 * NOT ported (deliberately): MPP host-side configuration structs
 * (VepuFmtCfg, Vepu5xxOsdCfg, VepuRgb2YuvCfg, Vepu510NpuOut).  Those are MPP
 * API surface containing pointers, not register layout.  The VepuFmt enum is
 * kept, because its enumerators are the source-format codes the encoder
 * writes into the format register.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VEPU510_REG_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VEPU510_REG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

typedef enum Vepu5xxOsdPltType_e
{
  VEPU5xx_OSD_PLT_TYPE_USERDEF = 0,
  VEPU5xx_OSD_PLT_TYPE_DEFAULT = 1,
} Vepu5xxOsdPltType;

typedef enum VepuFmt_e
{
  VEPU5xx_FMT_BGRA8888, // 0
  VEPU5xx_FMT_BGR888,   // 1
  VEPU5xx_FMT_BGR565,   // 2
  VEPU5xx_FMT_ARGB1555, // 3
  VEPU5xx_FMT_YUV422SP, // 4
  VEPU5xx_FMT_YUV422P,  // 5
  VEPU5xx_FMT_YUV420SP, // 6
  VEPU5xx_FMT_YUV420P,  // 7
  VEPU5xx_FMT_YUYV422,  // 8
  VEPU5xx_FMT_UYVY422,  // 9
  VEPU5xx_FMT_YUV400,   // 10
  VEPU5xx_FMT_AYUV2BPP, // 11
  VEPU5xx_FMT_YUV444SP, // 12
  VEPU5xx_FMT_YUV444P,  // 13
  VEPU5xx_FMT_ARGB4444, // 14
  VEPU5xx_FMT_AYUV1BPP, // 15
  VEPU5xx_FMT_BUTT,     // 16
} VepuFmt;

typedef struct Vepu5xxRoiH264BsCfg_t
{
  uint64_t force_inter : 42;
  uint64_t mode_mask : 9;
  uint64_t reserved : 10;
  uint64_t force_intra : 1;
  uint64_t qp_adj_en : 1;
  uint64_t amv_en : 1;
} Vepu5xxRoiH264BsCfg;

typedef struct VepuRgb2YuvCoeffs_t
{
  int16_t r_coeff;
  int16_t g_coeff;
  int16_t b_coeff;
  int16_t offset;
} VepuRgb2YuvCoeffs;

typedef enum qbias_ofst_e
{
  IFRAME_THD0 = 0,
  IFRAME_THD1,
  IFRAME_THD2,
  IFRAME_BIAS0,
  IFRAME_BIAS1,
  IFRAME_BIAS2,
  IFRAME_BIAS3,
  PFRAME_THD0,
  PFRAME_THD1,
  PFRAME_THD2,
  PFRAME_IBLK_BIAS0,
  PFRAME_IBLK_BIAS1,
  PFRAME_IBLK_BIAS2,
  PFRAME_IBLK_BIAS3,
  PFRAME_PBLK_BIAS0,
  PFRAME_PBLK_BIAS1,
  PFRAME_PBLK_BIAS2,
  PFRAME_PBLK_BIAS3
} QbiasOfst;

typedef struct Vepu51xH264Fbk_t
{
  uint32_t hw_status; /* 0:correct, 1:error */
  uint32_t frame_type;
  uint32_t qp_sum;
  uint32_t out_strm_size;
  uint32_t out_hw_strm_size;
  int64_t sse_sum;
  uint32_t st_lvl64_inter_num;
  uint32_t st_lvl32_inter_num;
  uint32_t st_lvl16_inter_num;
  uint32_t st_lvl8_inter_num;
  uint32_t st_lvl32_intra_num;
  uint32_t st_lvl16_intra_num;
  uint32_t st_lvl8_intra_num;
  uint32_t st_lvl4_intra_num;
  uint32_t st_cu_num_qp[52];
  uint32_t st_madp;
  uint32_t st_madi;
  uint32_t st_mb_num;
  uint32_t st_ctu_num;
  uint32_t st_smear_cnt[5];
} Vepu51xH264Fbk;

typedef struct Vepu51xH265Fbk_t
{
  uint32_t hw_status; /* 0:correct, 1:error */
  uint32_t frame_type;
  uint32_t qp_sum;
  uint32_t out_strm_size;
  uint32_t out_hw_strm_size;
  int64_t sse_sum;
  uint32_t st_lvl64_inter_num;
  uint32_t st_lvl32_inter_num;
  uint32_t st_lvl16_inter_num;
  uint32_t st_lvl8_inter_num;
  uint32_t st_lvl32_intra_num;
  uint32_t st_lvl16_intra_num;
  uint32_t st_lvl8_intra_num;
  uint32_t st_lvl4_intra_num;
  uint32_t st_cu_num_qp[52];
  uint32_t st_madp;
  uint32_t st_madi;
  uint32_t st_mb_num;
  uint32_t st_ctu_num;
  uint32_t st_smear_cnt[5];
  int32_t reg_idx;
  uint32_t acc_cover16_num;
  uint32_t acc_bndry16_num;
  uint32_t acc_zero_mv;
  int8_t tgt_sub_real_lvl[6];
} Vepu51xH265Fbk;

typedef struct Vepu510Online_t
{
  /* 0x00000270 reg156 */
  struct
  {
    uint32_t reserved : 4;
    uint32_t adr_vsy_t : 28;
  } adr_vsy_t;

  /* 0x00000274 reg157 */
  struct
  {
    uint32_t reserved : 4;
    uint32_t adr_vsc_t : 28;
  } adr_vsc_t;

  /* 0x00000278 reg158 */
  struct
  {
    uint32_t reserved : 4;
    uint32_t adr_vsy_b : 28;
  } adr_vsy_b;

  /* 0x0000027c reg159 */
  struct
  {
    uint32_t reserved : 4;
    uint32_t adr_vsc_b : 28;
  } adr_vsc_b;
} vepu510_online;

typedef struct Vepu510RdoSkipPar_t
{
  struct
  {
    uint32_t madp_thd0 : 12;
    uint32_t reserved : 4;
    uint32_t madp_thd1 : 12;
    uint32_t reserved1 : 4;
  } atf_thd0;

  /* 0x00002064 reg2073 */
  struct
  {
    uint32_t madp_thd2 : 12;
    uint32_t reserved : 4;
    uint32_t madp_thd3 : 12;
    uint32_t reserved1 : 4;
  } atf_thd1;

  /* 0x00002068 reg2074 */
  struct
  {
    uint32_t wgt0 : 8;
    uint32_t wgt1 : 8;
    uint32_t wgt2 : 8;
    uint32_t wgt3 : 8;
  } atf_wgt0;

  /* 0x0000206c reg2075 */
  struct
  {
    uint32_t wgt4 : 8;
    uint32_t reserved : 24;
  } atf_wgt1;
} rdo_skip_par;

typedef struct Vepu510RdoNoSkipPar_t
{
  /* 0x00002080 reg2080 */
  struct
  {
    uint32_t madp_thd0 : 12;
    uint32_t reserved : 4;
    uint32_t madp_thd1 : 12;
    uint32_t reserved1 : 4;
  } ratf_thd0;

  /* 0x00002084 reg2081 */
  struct
  {
    uint32_t madp_thd2 : 12;
    uint32_t reserved : 4;
    uint32_t atf_bypass_pri_flag : 1;
    uint32_t reserved1 : 15;
  } ratf_thd1;

  /* 0x00002088 reg2082 */
  struct
  {
    uint32_t wgt0 : 8;
    uint32_t wgt1 : 8;
    uint32_t wgt2 : 8;
    uint32_t wgt3 : 8;
  } atf_wgt;
} rdo_noskip_par;

typedef struct Vepu510RoiRegion_t
{
  struct
  {
    uint32_t roi_lt_x : 10;
    uint32_t reserved : 6;
    uint32_t roi_lt_y : 10;
    uint32_t reserved1 : 6;
  } roi_pos_lt;

  struct
  {
    uint32_t roi_rb_x : 10;
    uint32_t reserved : 6;
    uint32_t roi_rb_y : 10;
    uint32_t reserved1 : 6;
  } roi_pos_rb;

  struct
  {
    uint32_t roi_qp_value : 7;
    uint32_t roi_qp_adj_mode : 1;
    uint32_t roi_pri : 5;
    uint32_t roi_en : 1;
    uint32_t reserved : 18;
  } roi_base;

  struct
  {
    uint32_t roi_mdc_inter16 : 4;
    uint32_t roi_mdc_skip16 : 4;
    uint32_t roi_mdc_intra16 : 4;
    uint32_t roi0_mdc_inter32_hevc : 4;
    uint32_t roi0_mdc_skip32_hevc : 4;
    uint32_t roi0_mdc_intra32_hevc : 4;
    uint32_t roi0_mdc_dpth_hevc : 1;
    uint32_t reserved : 7;
  } roi_mdc;
} Vepu510RoiRegion;

typedef struct Vepu510RoiCfg_t
{
  /* 0x00001080 reg1056 */
  struct
  {
    uint32_t fmdc_adju_inter16 : 4;
    uint32_t fmdc_adju_skip16 : 4;
    uint32_t fmdc_adju_intra16 : 4;
    uint32_t fmdc_adju_inter32 : 4;
    uint32_t fmdc_adju_skip32 : 4;
    uint32_t fmdc_adju_intra32 : 4;
    uint32_t fmdc_adj_pri : 5;
    uint32_t reserved : 3;
  } fmdc_adj0;

  /* 0x00001084 reg1057 */
  struct
  {
    uint32_t fmdc_adju_inter8 : 4;
    uint32_t fmdc_adju_skip8 : 4;
    uint32_t fmdc_adju_intra8 : 4;
    uint32_t reserved : 20;
  } fmdc_adj1;

  uint32_t reserved_1058;

  /* 0x0000108c reg1059 */
  struct
  {
    uint32_t bmap_en : 1;
    uint32_t bmap_pri : 5;
    uint32_t bmap_qpmin : 6;
    uint32_t bmap_qpmax : 6;
    uint32_t bmap_mdc_dpth : 1;
    uint32_t reserved : 13;
  } bmap_cfg;

  /* 0x00001090 reg1060 - 0x0000110c reg1091 */
  Vepu510RoiRegion regions[8];
} Vepu510RoiCfg;

typedef struct Vepu510ControlCfg_t
{
  /* 0x00000000 reg0 */
  struct
  {
    uint32_t sub_ver : 8;
    uint32_t h264_cap : 1;
    uint32_t hevc_cap : 1;
    uint32_t reserved : 2;
    uint32_t res_cap : 4;
    uint32_t osd_cap : 2;
    uint32_t filtr_cap : 2;
    uint32_t bfrm_cap : 1;
    uint32_t fbc_cap : 2;
    uint32_t reserved1 : 1;
    uint32_t ip_id : 8;
  } version;

  /* 0x00000004 - 0x0000000c */
  uint32_t reserved1_3[3];

  /* 0x00000010 reg4 */
  struct
  {
    uint32_t lkt_num : 8;
    uint32_t vepu_cmd : 3;
    uint32_t reserved : 21;
  } enc_strt;

  /* 0x00000014 reg5 */
  struct
  {
    uint32_t safe_clr : 1;
    uint32_t force_clr : 1;
    uint32_t reserved : 30;
  } enc_clr;

  /* 0x00000018 reg6 */
  struct
  {
    uint32_t vswm_lcnt_soft : 14;
    uint32_t vswm_fcnt_soft : 8;
    uint32_t reserved : 2;
    uint32_t dvbm_ack_soft : 1;
    uint32_t dvbm_ack_sel : 1;
    uint32_t dvbm_inf_sel : 1;
    uint32_t reserved1 : 5;
  } vs_ldly;

  /* 0x0000001c */
  uint32_t reserved_7;

  /* 0x00000020 reg8 */
  struct
  {
    uint32_t enc_done_en : 1;
    uint32_t lkt_node_done_en : 1;
    uint32_t sclr_done_en : 1;
    uint32_t vslc_done_en : 1;
    uint32_t vbsf_oflw_en : 1;
    uint32_t vbuf_lens_en : 1;
    uint32_t enc_err_en : 1;
    uint32_t vsrc_err_en : 1;
    uint32_t wdg_en : 1;
    uint32_t lkt_err_int_en : 1;
    uint32_t lkt_err_stop_en : 1;
    uint32_t lkt_force_stop_en : 1;
    uint32_t jslc_done_en : 1;
    uint32_t jbsf_oflw_en : 1;
    uint32_t jbuf_lens_en : 1;
    uint32_t dvbm_err_en : 1;
    uint32_t reserved : 16;
  } int_en;

  /* 0x00000024 reg9 */
  struct
  {
    uint32_t enc_done_msk : 1;
    uint32_t lkt_node_done_msk : 1;
    uint32_t sclr_done_msk : 1;
    uint32_t vslc_done_msk : 1;
    uint32_t vbsf_oflw_msk : 1;
    uint32_t vbuf_lens_msk : 1;
    uint32_t enc_err_msk : 1;
    uint32_t vsrc_err_msk : 1;
    uint32_t wdg_msk : 1;
    uint32_t lkt_err_int_msk : 1;
    uint32_t lkt_err_stop_msk : 1;
    uint32_t lkt_force_stop_msk : 1;
    uint32_t jslc_done_msk : 1;
    uint32_t jbsf_oflw_msk : 1;
    uint32_t jbuf_lens_msk : 1;
    uint32_t dvbm_err_msk : 1;
    uint32_t reserved : 16;
  } int_msk;

  /* 0x00000028 reg10 */
  struct
  {
    uint32_t enc_done_clr : 1;
    uint32_t lkt_node_done_clr : 1;
    uint32_t sclr_done_clr : 1;
    uint32_t vslc_done_clr : 1;
    uint32_t vbsf_oflw_clr : 1;
    uint32_t vbuf_lens_clr : 1;
    uint32_t enc_err_clr : 1;
    uint32_t vsrc_err_clr : 1;
    uint32_t wdg_clr : 1;
    uint32_t lkt_err_int_clr : 1;
    uint32_t lkt_err_stop_clr : 1;
    uint32_t lkt_force_stop_clr : 1;
    uint32_t jslc_done_clr : 1;
    uint32_t jbsf_oflw_clr : 1;
    uint32_t jbuf_lens_clr : 1;
    uint32_t dvbm_err_clr : 1;
    uint32_t reserved : 16;
  } int_clr;

  /* 0x0000002c reg11 */
  struct
  {
    uint32_t enc_done_sta : 1;
    uint32_t lkt_node_done_sta : 1;
    uint32_t sclr_done_sta : 1;
    uint32_t vslc_done_sta : 1;
    uint32_t vbsf_oflw_sta : 1;
    uint32_t vbuf_lens_sta : 1;
    uint32_t enc_err_sta : 1;
    uint32_t vsrc_err_sta : 1;
    uint32_t wdg_sta : 1;
    uint32_t lkt_err_int_sta : 1;
    uint32_t lkt_err_stop_sta : 1;
    uint32_t lkt_force_stop_sta : 1;
    uint32_t jslc_done_sta : 1;
    uint32_t jbsf_oflw_sta : 1;
    uint32_t jbuf_lens_sta : 1;
    uint32_t dvbm_err_sta : 1;
    uint32_t reserved : 16;
  } int_sta;

  /* 0x00000030 reg12 */
  struct
  {
    uint32_t jpeg_bus_edin : 4;
    uint32_t src_bus_edin : 4;
    uint32_t meiw_bus_edin : 4;
    uint32_t bsw_bus_edin : 4;
    uint32_t reserved : 8;
    uint32_t lktw_bus_edin : 4;
    uint32_t rec_nfbc_bus_edin : 4;
  } dtrns_map;

  /* 0x00000034 reg13 */
  struct
  {
    uint32_t reserved : 16;
    uint32_t axi_brsp_cke : 10;
    uint32_t reserved1 : 6;
  } dtrns_cfg;

  /* 0x00000038 reg14 */
  struct
  {
    uint32_t vs_load_thd : 24;
    uint32_t reserved : 8;
  } enc_wdg;

  /* 0x0000003c - 0x0000004c */
  uint32_t reserved15_19[5];

  /* 0x00000050 reg20 */
  struct
  {
    uint32_t idle_en_core : 1;
    uint32_t idle_en_axi : 1;
    uint32_t idle_en_ahb : 1;
    uint32_t reserved : 29;
  } enc_idle_en;

  /* 0x00000054 reg21 */
  struct
  {
    uint32_t cke : 1;
    uint32_t resetn_hw_en : 1;
    uint32_t rfpr_err_e : 1;
    uint32_t sram_ckg_en : 1;
    uint32_t link_err_stop : 1;
    uint32_t reserved : 27;
  } opt_strg;

  /* 0x00000058 reg22 */
  union
  {
    struct
    {
      uint32_t tq8_ckg : 1;
      uint32_t tq4_ckg : 1;
      uint32_t bits_ckg_8x8 : 1;
      uint32_t bits_ckg_4x4_1 : 1;
      uint32_t bits_ckg_4x4_0 : 1;
      uint32_t inter_mode_ckg : 1;
      uint32_t inter_ctrl_ckg : 1;
      uint32_t inter_pred_ckg : 1;
      uint32_t intra8_ckg : 1;
      uint32_t intra4_ckg : 1;
      uint32_t reserved : 22;
    } h264;
    struct
    {
      uint32_t recon32_ckg : 1;
      uint32_t iqit32_ckg : 1;
      uint32_t q32_ckg : 1;
      uint32_t t32_ckg : 1;
      uint32_t cabac32_ckg : 1;
      uint32_t recon16_ckg : 1;
      uint32_t iqit16_ckg : 1;
      uint32_t q16_ckg : 1;
      uint32_t t16_ckg : 1;
      uint32_t cabac16_ckg : 1;
      uint32_t recon8_ckg : 1;
      uint32_t iqit8_ckg : 1;
      uint32_t q8_ckg : 1;
      uint32_t t8_ckg : 1;
      uint32_t cabac8_ckg : 1;
      uint32_t recon4_ckg : 1;
      uint32_t iqit4_ckg : 1;
      uint32_t q4_ckg : 1;
      uint32_t t4_ckg : 1;
      uint32_t cabac4_ckg : 1;
      uint32_t intra32_ckg : 1;
      uint32_t intra16_ckg : 1;
      uint32_t intra8_ckg : 1;
      uint32_t intra4_ckg : 1;
      uint32_t inter_pred_ckg : 1;
      uint32_t reserved : 7;
    } hevc;
  } rdo_ckg;

  /* 0x0000005c reg23 */
  struct
  {
    uint32_t core_id : 2;
    uint32_t reserved : 30;
  } core_id;
} Vepu510ControlCfg;

typedef struct Vepu510FrmCommon_t
{
  /* 0x00000270 reg156 - 0x0000027c reg159 */
  vepu510_online online_addr;

  /* 0x00000280 reg160 */
  uint32_t adr_src0;

  /* 0x00000284 reg161 */
  uint32_t adr_src1;

  /* 0x00000288 reg162 */
  uint32_t adr_src2;

  /* 0x0000028c reg163 */
  uint32_t rfpw_h_addr;

  /* 0x00000290 reg164 */
  uint32_t rfpw_b_addr;

  /* 0x00000294 reg165 */
  uint32_t rfpr_h_addr;

  /* 0x00000298 reg166 */
  uint32_t rfpr_b_addr;

  /* 0x0000029c reg167 */
  uint32_t colmvw_addr;

  /* 0x000002a0 reg168 */
  uint32_t colmvr_addr;

  /* 0x000002a4 reg169 */
  uint32_t dspw_addr;

  /* 0x000002a8 reg170 */
  uint32_t dspr_addr;

  /* 0x000002ac reg171 */
  uint32_t meiw_addr;

  /* 0x000002b0 reg172 */
  uint32_t bsbt_addr;

  /* 0x000002b4 reg173 */
  uint32_t bsbb_addr;

  /* 0x000002b8 reg174 */
  uint32_t adr_bsbs;

  /* 0x000002bc reg175 */
  uint32_t bsbr_addr;

  /* 0x000002c0 reg176 */
  uint32_t lpfw_addr;

  /* 0x000002c4 reg177 */
  uint32_t lpfr_addr;

  /* 0x000002c8 reg178 */
  uint32_t ebuft_addr;

  /* 0x000002cc reg179 */
  uint32_t ebufb_addr;

  /* 0x000002d0 reg180 */
  uint32_t rfpt_h_addr;

  /* 0x000002d4 reg181 */
  uint32_t rfpb_h_addr;

  /* 0x000002d8 reg182 */
  uint32_t rfpt_b_addr;

  /* 0x000002dc reg183 */
  uint32_t adr_rfpb_b;

  /* 0x000002e0 reg184 */
  uint32_t adr_smear_rd;

  /* 0x000002e4 reg185 */
  uint32_t adr_smear_wr;

  /* 0x000002e8 reg186 */
  uint32_t adr_roir;

  /* 0x2ec - 0x2fc */
  uint32_t reserved187_191[5];

  /* 0x00000300 reg192 */
  struct
  {
    uint32_t enc_stnd : 2;
    uint32_t cur_frm_ref : 1;
    uint32_t mei_stor : 1;
    uint32_t bs_scp : 1;
    uint32_t reserved : 3;
    uint32_t pic_qp : 6;
    uint32_t num_pic_tot_cur_hevc : 5;
    uint32_t log2_ctu_num_hevc : 5;
    uint32_t reserved1 : 6;
    uint32_t slen_fifo : 1;
    uint32_t rec_fbc_dis : 1;
  } enc_pic;

  /* 0x00000304 reg193 */
  struct
  {
    uint32_t dchs_txid : 2;
    uint32_t dchs_rxid : 2;
    uint32_t dchs_txe : 1;
    uint32_t dchs_rxe : 1;
    uint32_t reserved : 2;
    uint32_t dchs_dly : 8;
    uint32_t dchs_ofst : 10;
    uint32_t reserved1 : 6;
  } dual_core;

  /* 0x00000308 reg194 */
  struct
  {
    uint32_t frame_id : 8;
    uint32_t frm_id_match : 1;
    uint32_t reserved : 7;
    uint32_t ch_id : 2;
    uint32_t vrsp_rtn_en : 1;
    uint32_t vinf_req_en : 1;
    uint32_t reserved1 : 12;
  } enc_id;

  /* 0x0000030c reg195 */
  uint32_t bsp_size;

  /* 0x00000310 reg196 */
  struct
  {
    uint32_t pic_wd8_m1 : 11;
    uint32_t reserved : 5;
    uint32_t pic_hd8_m1 : 11;
    uint32_t reserved1 : 5;
  } enc_rsl;

  /* 0x00000314 reg197 */
  struct
  {
    uint32_t pic_wfill : 6;
    uint32_t reserved : 10;
    uint32_t pic_hfill : 6;
    uint32_t reserved1 : 10;
  } src_fill;

  /* 0x00000318 reg198 */
  struct
  {
    uint32_t alpha_swap : 1;
    uint32_t rbuv_swap : 1;
    uint32_t src_cfmt : 4;
    uint32_t src_rcne : 1;
    uint32_t out_fmt : 1;
    uint32_t src_range_trns_en : 1;
    uint32_t src_range_trns_sel : 1;
    uint32_t chroma_ds_mode : 1;
    uint32_t reserved : 21;
  } src_fmt;

  /* 0x0000031c reg199 */
  struct
  {
    uint32_t csc_wgt_b2y : 9;
    uint32_t csc_wgt_g2y : 9;
    uint32_t csc_wgt_r2y : 9;
    uint32_t reserved : 5;
  } src_udfy;

  /* 0x00000320 reg200 */
  struct
  {
    uint32_t csc_wgt_b2u : 9;
    uint32_t csc_wgt_g2u : 9;
    uint32_t csc_wgt_r2u : 9;
    uint32_t reserved : 5;
  } src_udfu;

  /* 0x00000324 reg201 */
  struct
  {
    uint32_t csc_wgt_b2v : 9;
    uint32_t csc_wgt_g2v : 9;
    uint32_t csc_wgt_r2v : 9;
    uint32_t reserved : 5;
  } src_udfv;

  /* 0x00000328 reg202 */
  struct
  {
    uint32_t csc_ofst_v : 8;
    uint32_t csc_ofst_u : 8;
    uint32_t csc_ofst_y : 5;
    uint32_t reserved : 11;
  } src_udfo;

  /* 0x0000032c reg203 */
  struct
  {
    uint32_t cr_force_value : 8;
    uint32_t cb_force_value : 8;
    uint32_t chroma_force_en : 1;
    uint32_t reserved : 9;
    uint32_t src_mirr : 1;
    uint32_t src_rot : 2;
    uint32_t tile4x4_en : 1;
    uint32_t reserved1 : 2;
  } src_proc;

  /* 0x00000330 reg204 */
  struct
  {
    uint32_t pic_ofst_x : 14;
    uint32_t reserved : 2;
    uint32_t pic_ofst_y : 14;
    uint32_t reserved1 : 2;
  } pic_ofst;

  /* 0x00000334 reg205 */
  struct
  {
    uint32_t src_strd0 : 21;
    uint32_t reserved : 11;
  } src_strd0;

  /* 0x00000338 reg206 */
  struct
  {
    uint32_t src_strd1 : 16;
    uint32_t reserved : 16;
  } src_strd1;

  /* 0x33c - 0x34c */
  uint32_t reserved207_211[5];

  /* 0x00000350 reg212 */
  struct
  {
    uint32_t rc_en : 1;
    uint32_t aq_en : 1;
    uint32_t reserved : 10;
    uint32_t rc_ctu_num : 20;
  } rc_cfg;

  /* 0x00000354 reg213 */
  struct
  {
    uint32_t reserved : 16;
    uint32_t rc_qp_range : 4;
    uint32_t rc_max_qp : 6;
    uint32_t rc_min_qp : 6;
  } rc_qp;

  /* 0x00000358 reg214 */
  struct
  {
    uint32_t ctu_ebit : 20;
    uint32_t reserved : 12;
  } rc_tgt;

  /* 0x35c */
  uint32_t reserved_215;

  /* 0x00000360 reg216 */
  struct
  {
    uint32_t sli_splt : 1;
    uint32_t sli_splt_mode : 1;
    uint32_t sli_splt_cpst : 1;
    uint32_t reserved : 12;
    uint32_t sli_flsh : 1;
    uint32_t sli_max_num_m1 : 15;
    uint32_t reserved1 : 1;
  } sli_splt;

  /* 0x00000364 reg217 */
  struct
  {
    uint32_t sli_splt_byte : 20;
    uint32_t reserved : 12;
  } sli_byte;

  /* 0x00000368 reg218 */
  struct
  {
    uint32_t sli_splt_cnum_m1 : 20;
    uint32_t reserved : 12;
  } sli_cnum;

  /* 0x0000036c reg219 */
  struct
  {
    uint32_t uvc_partition0_len : 12;
    uint32_t uvc_partition_len : 12;
    uint32_t uvc_skip_len : 6;
    uint32_t reserved : 2;
  } vbs_pad;

  /* 0x00000370 reg220 */
  struct
  {
    uint32_t cime_srch_dwnh : 4;
    uint32_t cime_srch_uph : 4;
    uint32_t cime_srch_rgtw : 4;
    uint32_t cime_srch_lftw : 4;
    uint32_t dlt_frm_num : 16;
  } me_rnge;

  /* 0x00000374 reg221 */
  struct
  {
    uint32_t srgn_max_num : 7;
    uint32_t cime_dist_thre : 13;
    uint32_t rme_srch_h : 2;
    uint32_t rme_srch_v : 2;
    uint32_t rme_dis : 3;
    uint32_t reserved : 1;
    uint32_t fme_dis : 3;
    uint32_t reserved1 : 1;
  } me_cfg;

  /* 0x00000378 reg222 */
  struct
  {
    uint32_t cime_zero_thre : 13;
    uint32_t reserved : 15;
    uint32_t fme_prefsu_en : 2;
    uint32_t colmv_stor_hevc : 1;
    uint32_t colmv_load_hevc : 1;
  } me_cach;

  /* 0x37c - 0x39c */
  uint32_t reserved223_231[9];
} Vepu510FrmCommon;

typedef struct Vepu510RcRoi_t
{
  /* 0x00001000 reg1024 */
  struct
  {
    uint32_t qp_adj0 : 5;
    uint32_t qp_adj1 : 5;
    uint32_t qp_adj2 : 5;
    uint32_t qp_adj3 : 5;
    uint32_t qp_adj4 : 5;
    uint32_t reserved : 7;
  } rc_adj0;

  /* 0x00001004 reg1025 */
  struct
  {
    uint32_t qp_adj5 : 5;
    uint32_t qp_adj6 : 5;
    uint32_t qp_adj7 : 5;
    uint32_t qp_adj8 : 5;
    uint32_t reserved : 12;
  } rc_adj1;

  /* 0x00001008 reg1026 - 0x00001028 reg1034 */
  uint32_t rc_dthd_0_8[9];

  /* 0x102c */
  uint32_t reserved_1035;

  /* 0x00001030 reg1036 */
  struct
  {
    uint32_t qpmin_area0 : 6;
    uint32_t qpmax_area0 : 6;
    uint32_t qpmin_area1 : 6;
    uint32_t qpmax_area1 : 6;
    uint32_t qpmin_area2 : 6;
    uint32_t reserved : 2;
  } roi_qthd0;

  /* 0x00001034 reg1037 */
  struct
  {
    uint32_t qpmax_area2 : 6;
    uint32_t qpmin_area3 : 6;
    uint32_t qpmax_area3 : 6;
    uint32_t qpmin_area4 : 6;
    uint32_t qpmax_area4 : 6;
    uint32_t reserved : 2;
  } roi_qthd1;

  /* 0x00001038 reg1038 */
  struct
  {
    uint32_t qpmin_area5 : 6;
    uint32_t qpmax_area5 : 6;
    uint32_t qpmin_area6 : 6;
    uint32_t qpmax_area6 : 6;
    uint32_t qpmin_area7 : 6;
    uint32_t reserved : 2;
  } roi_qthd2;

  /* 0x0000103c reg1039 */
  struct
  {
    uint32_t qpmax_area7 : 6;
    uint32_t reserved : 24;
    uint32_t qpmap_mode : 2;
  } roi_qthd3;

  /* 0x00001040 reg1040 */
  uint32_t reserved_1040;

  /* 0x00001044 reg1041 - 0x00001050 reg1044 */
  uint8_t aq_tthd[16];

  /* 0x00001054 reg1045 */
  struct
  {
    int32_t aq_stp_s0 : 5;
    int32_t aq_stp_0t1 : 5;
    int32_t aq_stp_1t2 : 5;
    int32_t aq_stp_2t3 : 5;
    int32_t aq_stp_3t4 : 5;
    int32_t aq_stp_4t5 : 5;
    int32_t reserved : 2;
  } aq_stp0;

  /* 0x00001058 reg1046 */
  struct
  {
    int32_t aq_stp_5t6 : 5;
    int32_t aq_stp_6t7 : 5;
    int32_t aq_stp_7t8 : 5;
    int32_t aq_stp_8t9 : 5;
    int32_t aq_stp_9t10 : 5;
    int32_t aq_stp_10t11 : 5;
    int32_t reserved : 2;
  } aq_stp1;

  /* 0x0000105c reg1047 */
  struct
  {
    int32_t aq_stp_11t12 : 5;
    int32_t aq_stp_12t13 : 5;
    int32_t aq_stp_13t14 : 5;
    int32_t aq_stp_14t15 : 5;
    int32_t aq_stp_b15 : 5;
    uint32_t reserved : 7;
  } aq_stp2;

  /* 0x00001060 reg1048 */
  struct
  {
    uint32_t aq16_rnge : 4;
    uint32_t aq32_rnge : 4;
    uint32_t aq8_rnge : 5;
    uint32_t aq16_dif0 : 5;
    uint32_t aq16_dif1 : 5;
    uint32_t reserved : 1;
    uint32_t aq_cme_en : 1;
    uint32_t aq_subj_cme_en : 1;
    uint32_t aq_rme_en : 1;
    uint32_t aq_subj_rme_en : 1;
    uint32_t reserved1 : 4;
  } aq_clip;

  /* 0x00001064 reg1049 */
  struct
  {
    uint32_t madi_th0 : 8;
    uint32_t madi_th1 : 8;
    uint32_t madi_th2 : 8;
    uint32_t reserved : 8;
  } madi_st_thd;

  /* 0x00001068 reg1050 */
  struct
  {
    uint32_t madp_th0 : 12;
    uint32_t reserved : 4;
    uint32_t madp_th1 : 12;
    uint32_t reserved1 : 4;
  } madp_st_thd0;

  /* 0x0000106c reg1051 */
  struct
  {
    uint32_t madp_th2 : 12;
    uint32_t reserved : 20;
  } madp_st_thd1;

  /* 0x1070 - 0x1078 */
  uint32_t reserved1052_1054[3];

  /* 0x0000107c reg1055 */
  struct
  {
    uint32_t chrm_klut_ofst : 4;
    uint32_t reserved : 4;
    uint32_t inter_chrm_dist_multi : 6;
    uint32_t reserved1 : 18;
  } klut_ofst;

  /*0x00001080 reg1056 - 0x0000110c reg1091 */
  Vepu510RoiCfg roi_cfg;
} Vepu510RcRoi;

typedef struct Vepu510WgtCommon_t
{
  /* 0x00001760 reg1496 */
  struct
  {
    uint32_t cime_pmv_num : 1;
    uint32_t cime_fuse : 1;
    uint32_t itp_mode : 1;
    uint32_t reserved : 1;
    uint32_t move_lambda : 4;
    uint32_t rime_lvl_mrg : 2;
    uint32_t rime_prelvl_en : 2;
    uint32_t rime_prersu_en : 3;
    uint32_t reserved1 : 17;
  } me_sqi_comb;

  /* 0x00001764 reg1497 */
  struct
  {
    uint32_t cime_mvd_th0 : 9;
    uint32_t reserved : 1;
    uint32_t cime_mvd_th1 : 9;
    uint32_t reserved1 : 1;
    uint32_t cime_mvd_th2 : 9;
    uint32_t reserved2 : 3;
  } cime_mvd_th_comb;

  /* 0x00001768 reg1498 */
  struct
  {
    uint32_t cime_madp_th : 12;
    uint32_t reserved : 20;
  } cime_madp_th_comb;

  /* 0x0000176c reg1499 */
  struct
  {
    uint32_t cime_multi0 : 8;
    uint32_t cime_multi1 : 8;
    uint32_t cime_multi2 : 8;
    uint32_t cime_multi3 : 8;
  } cime_multi_comb;

  /* 0x00001770 reg1500 */
  struct
  {
    uint32_t rime_mvd_th0 : 3;
    uint32_t reserved : 1;
    uint32_t rime_mvd_th1 : 3;
    uint32_t reserved1 : 9;
    uint32_t fme_madp_th : 12;
    uint32_t reserved2 : 4;
  } rime_mvd_th_comb;

  /* 0x00001774 reg1501 */
  struct
  {
    uint32_t rime_madp_th0 : 12;
    uint32_t reserved : 4;
    uint32_t rime_madp_th1 : 12;
    uint32_t reserved1 : 4;
  } rime_madp_th_comb;

  /* 0x00001778 reg1502 */
  struct
  {
    uint32_t rime_multi0 : 10;
    uint32_t rime_multi1 : 10;
    uint32_t rime_multi2 : 10;
    uint32_t reserved : 2;
  } rime_multi_comb;

  /* 0x0000177c reg1503 */
  struct
  {
    uint32_t cmv_th0 : 8;
    uint32_t cmv_th1 : 8;
    uint32_t cmv_th2 : 8;
    uint32_t reserved : 8;
  } cmv_st_th_comb;

  /* 0x1780 - 0x17fc */
  uint32_t reserved1504_1535[32];

  /* 0x00001800 reg1536 - 0x000018cc reg1587 */
  uint32_t pprd_lamb_satd_0_51[52];

  /* 0x000018d0 reg1588 */
  struct
  {
    uint32_t lambda_satd_offset : 5;
    uint32_t reserved : 27;
  } iprd_lamb_satd_ofst;

  /* 0x18d4 - 0x18fc */
  uint32_t reserved1589_1599[11];

  /* 0x00001900 reg1600 - 0x000019cc reg1651 */
  uint32_t rdo_wgta_qp_grpa_0_51[52];
} Vepu510WgtCommon;

typedef struct Vepu510SclCfg_t
{
  /* 0x2200 - 0x221F, valid for h.264/h.h265, jpeg no use */
  uint32_t tu8_intra_y[16];
  uint32_t tu8_intra_u[16]; /* tu8_inter_y[16] for h.264 */

  /* 0x2220 - 0x2584, valid for h.265 only */
  uint32_t tu8_intra_v[16];
  uint32_t tu8_inter_y[16];
  uint32_t tu8_inter_u[16];
  uint32_t tu8_inter_v[16];
  uint32_t tu16_intra_y_ac[16];
  uint32_t tu16_intra_u_ac[16];
  uint32_t tu16_intra_v_ac[16];
  uint32_t tu16_inter_y_ac[16];
  uint32_t tu16_inter_u_ac[16];
  uint32_t tu16_inter_v_ac[16];
  uint32_t tu32_intra_y_ac[16];
  uint32_t tu32_inter_y_ac[16];

  /* 0x2580 */
  struct
  {
    uint32_t tu16_intra_y_dc : 8;
    uint32_t tu16_intra_u_dc : 8;
    uint32_t tu16_intra_v_dc : 8;
    uint32_t tu16_inter_y_dc : 8;
  } tu_dc0;

  /* 0x2584 */
  struct
  {
    uint32_t tu16_inter_u_dc : 8;
    uint32_t tu16_inter_v_dc : 8;
    uint32_t tu32_intra_y_dc : 8;
    uint32_t tu32_inter_y_dc : 8;
  } tu_dc1;
} Vepu510SclCfg;

typedef struct Vepu510Status_t
{
  /* 0x00004000 reg4096 */
  uint32_t bs_lgth_l32;

  /* 0x00004004 reg4097 */
  struct
  {
    uint32_t bs_lgth_h8 : 8;
    uint32_t reserved : 8;
    uint32_t sse_l16 : 16;
  } st_sse_bsl;

  /* 0x00004008 reg4098 */
  uint32_t sse_h32;

  /* 0x0000400c reg4099 */
  uint32_t qp_sum;

  /* 0x00004010 reg4100 */
  struct
  {
    uint32_t sao_cnum : 16;
    uint32_t sao_ynum : 16;
  } st_sao;

  /* 0x00004014 reg4101 */
  uint32_t rdo_head_bits;

  /* 0x00004018 reg4102 */
  struct
  {
    uint32_t rdo_head_bits_h8 : 8;
    uint32_t reserved : 8;
    uint32_t rdo_res_bits_l16 : 16;
  } st_head_res_bl;

  /* 0x0000401c reg4103 */
  uint32_t rdo_res_bits_h24;

  /* 0x00004020 reg4104 */
  struct
  {
    uint32_t st_enc : 2;
    uint32_t st_sclr : 1;
    uint32_t isp_src_oflw : 1;
    uint32_t vepu_src_oflw : 1;
    uint32_t vepu_fcnt_nmch : 1;
    uint32_t vepu_fbd_err : 5;
    uint32_t reserved : 5;
    uint32_t dvbm_finf_wful : 1;
    uint32_t dvbm_linf_wful : 1;
    uint32_t dvbm_fcnt_late : 1;
    uint32_t dvbm_fcnt_early : 1;
    uint32_t dvbm_isp_oflw : 1;
    uint32_t dvbm_vepu_oflw : 1;
    uint32_t isp_time_out : 1;
    uint32_t dvbm_vsrc_fcnt : 1;
    uint32_t reserved1 : 8;
  } st_enc;

  /* 0x00004024 reg4105 */
  struct
  {
    uint32_t fnum_cfg_done : 8;
    uint32_t fnum_cfg : 8;
    uint32_t fnum_int : 8;
    uint32_t fnum_enc_done : 8;
  } st_lkt;

  /* 0x00004028 reg4106 */
  struct
  {
    uint32_t reserved : 4;
    uint32_t node_addr : 28;
  } st_nadr;

  /* 0x0000402c reg4107 */
  struct
  {
    uint32_t bsbw_ovfl : 1;
    uint32_t reserved : 2;
    uint32_t bsbw_addr : 28;
    uint32_t reserved1 : 1;
  } st_bsb;

  /* 0x00004030 reg4108 */
  struct
  {
    uint32_t axib_idl : 8;
    uint32_t axib_ovfl : 8;
    uint32_t axib_err : 8;
    uint32_t axir_err : 8;
  } st_bus;

  /* 0x00004034 reg4109 */
  struct
  {
    uint32_t sli_num_video : 6;
    uint32_t sli_num_jpeg : 6;
    uint32_t reserved : 4;
    uint32_t bpkt_num_video : 7;
    uint32_t bpkt_lst_video : 1;
    uint32_t bpkt_num_jpeg : 7;
    uint32_t bpkt_lst_jpeg : 1;
  } st_snum;

  /* 0x00004038 reg4110 */
  struct
  {
    uint32_t sli_len : 31;
    uint32_t sli_lst : 1;
  } st_slen;

  /* 0x403c - reg4111 */
  struct
  {
    uint32_t task_id_proc : 12;
    uint32_t task_id_done : 12;
    uint32_t task_done : 1;
    uint32_t task_lkt_err : 3;
    uint32_t reserved : 4;
  } st_link_task;

  /* 0x4040 - 0x405c */
  uint32_t reserved4111_4119[8];

  /* 0x00004060 reg4120 */
  struct
  {
    uint32_t sli_len_jpeg : 31;
    uint32_t sli_lst_jpeg : 1;
  } st_slen_jpeg;

  /* 0x00004064 reg4121 */
  uint32_t jpeg_head_bits_l32;

  /* 0x00004068 reg4122 */
  struct
  {
    uint32_t jpeg_head_bits_h8 : 1;
    uint32_t reserved : 31;
  } st_bsl_h8_jpeg;

  /* 0x0000406c reg4123 */
  struct
  {
    uint32_t jbsbw_ovfl : 1;
    uint32_t reserved : 2;
    uint32_t jbsbw_addr : 28;
    uint32_t reserved1 : 1;
  } st_jbsb;

  /* 0x4070 - 0x407c */
  uint32_t reserved4124_4127[4];

  /* 0x00004080 reg4128 */
  struct
  {
    uint32_t pnum_p64 : 17;
    uint32_t reserved : 15;
  } st_pnum_p64;

  /* 0x00004084 reg4129 */
  struct
  {
    uint32_t pnum_p32 : 19;
    uint32_t reserved : 13;
  } st_pnum_p32;

  /* 0x00004088 reg4130 */
  struct
  {
    uint32_t pnum_p16 : 21;
    uint32_t reserved : 11;
  } st_pnum_p16;

  /* 0x0000408c reg4131 */
  struct
  {
    uint32_t pnum_p8 : 23;
    uint32_t reserved : 9;
  } st_pnum_p8;

  /* 0x00004090 reg4132 */
  struct
  {
    uint32_t pnum_i32 : 19;
    uint32_t reserved : 13;
  } st_pnum_i32;

  /* 0x00004094 reg4133 */
  struct
  {
    uint32_t pnum_i16 : 21;
    uint32_t reserved : 11;
  } st_pnum_i16;

  /* 0x00004098 reg4134 */
  struct
  {
    uint32_t pnum_i8 : 23;
    uint32_t reserved : 9;
  } st_pnum_i8;

  /* 0x0000409c reg4135 */
  struct
  {
    uint32_t pnum_i4 : 23;
    uint32_t reserved : 9;
  } st_pnum_i4;

  /* 0x000040a0 reg4136 */
  struct
  {
    uint32_t num_b16 : 23;
    uint32_t reserved : 9;
  } st_bnum_b16;

  /* 0x000040a4 reg4137 */
  struct
  {
    uint32_t rdo_smear_cnt0 : 8;
    uint32_t rdo_smear_cnt1 : 8;
    uint32_t rdo_smear_cnt2 : 8;
    uint32_t rdo_smear_cnt3 : 8;
  } st_smear_cnt;

  /* 0x000040a8 reg4138 */
  uint32_t madi16_sum;

  /* 0x000040ac reg4139 */
  uint32_t madi32_sum;

  /* 0x000040b0 reg4140 */
  uint32_t madp_sum;

  /* 0x40b4 - 0x40bc */
  uint32_t reserved4141_4143[3];

  /* 0x000040c0 reg4144 */
  struct
  {
    uint32_t madi_th_lt_cnt0 : 16;
    uint32_t madi_th_lt_cnt1 : 16;
  } st_madi_lt_num0;

  /* 0x000040c4 reg4145 */
  struct
  {
    uint32_t madi_th_lt_cnt2 : 16;
    uint32_t madi_th_lt_cnt3 : 16;
  } st_madi_lt_num1;

  /* 0x000040c8 reg4146 */
  struct
  {
    uint32_t madi_th_rt_cnt0 : 16;
    uint32_t madi_th_rt_cnt1 : 16;
  } st_madi_rt_num0;

  /* 0x000040cc reg4147 */
  struct
  {
    uint32_t madi_th_rt_cnt2 : 16;
    uint32_t madi_th_rt_cnt3 : 16;
  } st_madi_rt_num1;

  /* 0x000040d0 reg4148 */
  struct
  {
    uint32_t madi_th_lb_cnt0 : 16;
    uint32_t madi_th_lb_cnt1 : 16;
  } st_madi_lb_num0;

  /* 0x000040d4 reg4149 */
  struct
  {
    uint32_t madi_th_lb_cnt2 : 16;
    uint32_t madi_th_lb_cnt3 : 16;
  } st_madi_lb_num1;

  /* 0x000040d8 reg4150 */
  struct
  {
    uint32_t madi_th_rb_cnt0 : 16;
    uint32_t madi_th_rb_cnt1 : 16;
  } st_madi_rb_num0;

  /* 0x000040dc reg4151 */
  struct
  {
    uint32_t madi_th_rb_cnt2 : 16;
    uint32_t madi_th_rb_cnt3 : 16;
  } st_madi_rb_num1;

  /* 0x000040e0 reg4152 */
  struct
  {
    uint32_t madp_th_lt_cnt0 : 16;
    uint32_t madp_th_lt_cnt1 : 16;
  } st_madp_lt_num0;

  /* 0x000040e4 reg4153 */
  struct
  {
    uint32_t madp_th_lt_cnt2 : 16;
    uint32_t madp_th_lt_cnt3 : 16;
  } st_madp_lt_num1;

  /* 0x000040e8 reg4154 */
  struct
  {
    uint32_t madp_th_rt_cnt0 : 16;
    uint32_t madp_th_rt_cnt1 : 16;
  } st_madp_rt_num0;

  /* 0x000040ec reg4155 */
  struct
  {
    uint32_t madp_th_rt_cnt2 : 16;
    uint32_t madp_th_rt_cnt3 : 16;
  } st_madp_rt_num1;

  /* 0x000040f0 reg4156 */
  struct
  {
    uint32_t madp_th_lb_cnt0 : 16;
    uint32_t madp_th_lb_cnt1 : 16;
  } st_madp_lb_num0;

  /* 0x000040f4 reg4157 */
  struct
  {
    uint32_t madp_th_lb_cnt2 : 16;
    uint32_t madp_th_lb_cnt3 : 16;
  } st_madp_lb_num1;

  /* 0x000040f8 reg4158 */
  struct
  {
    uint32_t madp_th_rb_cnt0 : 16;
    uint32_t madp_th_rb_cnt1 : 16;
  } st_madp_rb_num0;

  /* 0x000040fc reg4159 */
  struct
  {
    uint32_t madp_th_rb_cnt2 : 16;
    uint32_t madp_th_rb_cnt3 : 16;
  } st_madp_rb_num1;

  /* 0x00004100 reg4160 */
  struct
  {
    uint32_t cmv_th_lt_cnt0 : 16;
    uint32_t cmv_th_lt_cnt1 : 16;
  } st_cmv_lt_num0;

  /* 0x00004104 reg4161 */
  struct
  {
    uint32_t cmv_th_lt_cnt2 : 16;
    uint32_t cmv_th_lt_cnt3 : 16;
  } st_cmv_lt_num1;

  /* 0x00004108 reg4162 */
  struct
  {
    uint32_t cmv_th_rt_cnt0 : 16;
    uint32_t cmv_th_rt_cnt1 : 16;
  } st_cmv_rt_num0;

  /* 0x0000410c reg4163 */
  struct
  {
    uint32_t cmv_th_rt_cnt2 : 16;
    uint32_t cmv_th_rt_cnt3 : 16;
  } st_cmv_rt_num1;

  /* 0x00004110 reg4164 */
  struct
  {
    uint32_t cmv_th_lb_cnt0 : 16;
    uint32_t cmv_th_lb_cnt1 : 16;
  } st_cmv_lb_num0;

  /* 0x00004114 reg4165 */
  struct
  {
    uint32_t cmv_th_lb_cnt2 : 16;
    uint32_t cmv_th_lb_cnt3 : 16;
  } st_cmv_lb_num1;

  /* 0x00004118 reg4166 */
  struct
  {
    uint32_t cmv_th_rb_cnt0 : 16;
    uint32_t cmv_th_rb_cnt1 : 16;
  } st_cmv_rb_num0;

  /* 0x0000411c reg4167 */
  struct
  {
    uint32_t cmv_th_rb_cnt2 : 16;
    uint32_t cmv_th_rb_cnt3 : 16;
  } st_cmv_rb_num1;

  /* 0x00004120 reg4168 */
  struct
  {
    uint32_t org_y_r_max_value : 8;
    uint32_t org_y_r_min_value : 8;
    uint32_t org_u_g_max_value : 8;
    uint32_t org_u_g_min_value : 8;
  } st_vsp_org_value0;

  /* 0x00004124 reg4169 */
  struct
  {
    uint32_t org_v_b_max_value : 8;
    uint32_t org_v_b_min_value : 8;
    uint32_t reserved : 16;
  } st_vsp_org_value1;

  /* 0x4128 - 0x412c */
  uint32_t reserved4170_4171[2];

  /* 0x00004130 reg4172 */
  uint32_t dsp_y_sum;

  /* 0x00004134 reg4173 */
  uint32_t acc_zero_mv;

  /* 0x00004138 reg4174 */
  uint32_t acc_dist0;

  /* 0x0000413c reg4175 */
  uint32_t acc_block_num;

  /* 0x00004140 reg4176 */
  struct
  {
    uint32_t num0_point_skin : 15;
    uint32_t acc_cmplx_num : 17;
  } st_skin_sum0;

  /* 0x00004144 reg4177 */
  struct
  {
    uint32_t num1_point_skin : 15;
    uint32_t acc_cover16_num : 17;
  } st_skin_sum1;

  /* 0x00004148 reg4178 */
  struct
  {
    uint32_t num2_point_skin : 15;
    uint32_t acc_bndry16_num : 17;
  } st_skin_sum2;

  /* 0x0000414c reg4179 */
  uint32_t num0_grdnt_point_dep0;

  /* 0x00004150 reg4180 */
  uint32_t num1_grdnt_point_dep0;

  /* 0x00004154 reg4181 */
  uint32_t num2_grdnt_point_dep0;

  /* 0x4158 - 0x417c */
  uint32_t reserved4182_4191[10];

  /* 0x00004180 reg4192 - 0x0000424c reg4243*/
  uint32_t st_b8_qp[52];
} Vepu510Status;

typedef struct Vepu510Dbg_t
{
  /* 0x00005000 reg5120 */
  struct
  {
    uint32_t vsp0_pos_x : 16;
    uint32_t vsp0_pos_y : 16;
  } st_ppl_pos_vsp0;

  /* 0x00005004 reg5121 */
  struct
  {
    uint32_t vsp1_pos_x : 16;
    uint32_t vsp1_pos_y : 16;
  } st_ppl_pos_vsp1;

  /* 0x00005008 reg5122 */
  struct
  {
    uint32_t cme_pos_x : 16;
    uint32_t cme_pos_y : 16;
  } st_ppl_pos_cme;

  /* 0x0000500c reg5123 */
  struct
  {
    uint32_t swin_cmd_x : 16;
    uint32_t swin_cmd_y : 16;
  } st_ppl_cmd_swin;

  /* 0x00005010 reg5124 */
  struct
  {
    uint32_t swin_pos_x : 16;
    uint32_t swin_pos_y : 16;
  } st_ppl_pos_swin;

  /* 0x00005014 reg5125 */
  struct
  {
    uint32_t pren_pos_x : 16;
    uint32_t pren_pos_y : 16;
  } st_ppl_pos_pren;

  /* 0x00005018 reg5126 */
  struct
  {
    uint32_t rfme_pos_x : 16;
    uint32_t rfme_pos_y : 16;
  } st_ppl_pos_rfme;

  /* 0x0000501c reg5127 */
  struct
  {
    uint32_t rdo_pos_x : 16;
    uint32_t rdo_pos_y : 16;
  } st_ppl_pos_rdo;

  /* 0x00005020 reg5128 */
  struct
  {
    uint32_t lpf_pos_x : 16;
    uint32_t lpf_pos_y : 16;
  } st_ppl_pos_lpf;

  /* 0x00005024 reg5129 */
  struct
  {
    uint32_t etpy_pos_x : 16;
    uint32_t etpy_pos_y : 16;
  } st_ppl_pos_etpy;

  /* 0x00005028 reg5130 */
  struct
  {
    uint32_t vsp0_pos_x : 16;
    uint32_t vsp0_pos_y : 16;
  } st_ppl_pos_jsp0;

  /* 0x0000502c reg5131 */
  struct
  {
    uint32_t vsp1_pos_x : 16;
    uint32_t vsp1_pos_y : 16;
  } st_ppl_pos_jsp1;

  /* 0x00005030 reg5132 */
  struct
  {
    uint32_t jpeg_pos_x : 16;
    uint32_t jpeg_pos_y : 16;
  } st_ppl_pos_jpeg;

  /* 0x5034 - 0x503c */
  uint32_t reserved5133_5135[3];
  /* 0x00005040 reg5136 */
  struct
  {
    uint32_t vsp0_org_err : 1;
    uint32_t vsp0_vsld_err : 1;
    uint32_t pp0_pp1_err : 1;
    uint32_t vsp0_cmd_err : 1;
    uint32_t reserved : 24;
    uint32_t vsp0_wrk : 1;
    uint32_t vsp0_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_vsp0;

  /* 0x00005044 reg5137 */
  struct
  {
    uint32_t vsp1_org_err : 1;
    uint32_t vsp1_rdo_err : 1;
    uint32_t reserved : 26;
    uint32_t vsp1_wrk : 1;
    uint32_t vsp1_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_vsp1;

  /* 0x00005048 reg5138 */
  struct
  {
    uint32_t cme_org_err : 1;
    uint32_t cme_roi_err : 1;
    uint32_t cme_win_err : 1;
    uint32_t cme_cmmv_err : 1;
    uint32_t cme_smvp_err : 1;
    uint32_t cme_meiw_err : 1;
    uint32_t cme_dist_err : 1;
    uint32_t cme_rdo_err : 1;
    uint32_t cme_madp_err : 1;
    uint32_t cme_mv_err : 1;
    uint32_t reserved : 18;
    uint32_t cme_wrk : 1;
    uint32_t cme_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_cme;

  /* 0x0000504c reg5139 */
  struct
  {
    uint32_t swin_org_err : 1;
    uint32_t swin_ref_err : 1;
    uint32_t swin_cmd_err : 1;
    uint32_t reserved : 25;
    uint32_t swin_wrk : 1;
    uint32_t swin_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_swin;

  /* 0x00005050 reg5140 */
  struct
  {
    uint32_t swin_buff_ptr : 2;
    uint32_t swin_buff_num0 : 2;
    uint32_t swin_buff_num1 : 2;
    uint32_t swin_buff_num2 : 2;
    uint32_t reserved : 24;
  } dbg_ppl_swin;

  /* 0x00005054 reg5141 */
  struct
  {
    uint32_t pnra_org_err : 1;
    uint32_t pnra_dist_err : 1;
    uint32_t pnra_olm_err : 1;
    uint32_t reserved : 25;
    uint32_t pnra_wrk : 1;
    uint32_t pnra_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_pren;

  /* 0x00005058 reg5142 */
  struct
  {
    uint32_t rfme_org_err : 1;
    uint32_t rfme_ref_err : 1;
    uint32_t rfme_cmmv_err : 1;
    uint32_t rfme_rfmv_err : 1;
    uint32_t rfme_tmvp_err : 1;
    uint32_t reserved : 23;
    uint32_t rfme_wrk : 1;
    uint32_t rfme_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_rfme;

  /* 0x0000505c reg5143 */
  struct
  {
    uint32_t rdo_org_err : 1;
    uint32_t rdo_ref_err : 1;
    uint32_t rdo_inf_err : 1;
    uint32_t rdo_roi_err : 1;
    uint32_t rdo_rfmv_err : 1;
    uint32_t rdo_lbfr_err : 1;
    uint32_t rdo_lbfw_err : 1;
    uint32_t rdo_tmvp_rd_err : 1;
    uint32_t rdo_tmvp_wr_err : 1;
    uint32_t rdo_st_err : 1;
    uint32_t rdo_pnra_err : 1;
    uint32_t rdo_lpf_err : 1;
    uint32_t rdo_ent_err : 1;
    uint32_t reserved : 15;
    uint32_t rdo_wrk : 1;
    uint32_t rdo_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_rdo;

  /* 0x00005060 reg5144 */
  struct
  {
    uint32_t lpf_org_err : 1;
    uint32_t lpf_lbfr_err : 1;
    uint32_t lpf_lbfw_err : 1;
    uint32_t lpf_rcol_err : 1;
    uint32_t reserved : 24;
    uint32_t lpf_wrk : 1;
    uint32_t lpf_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_lpf;

  /* 0x00005064 reg5145 */
  struct
  {
    uint32_t etpy_bsw_err : 1;
    uint32_t reserved : 27;
    uint32_t etpy_wrk : 1;
    uint32_t etpy_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_etpy;

  /* 0x00005068 reg5146 */
  struct
  {
    uint32_t jsp0_org_err : 1;
    uint32_t jsp0_vsld_err : 1;
    uint32_t pp0_pp1_err : 1;
    uint32_t jsp0_cmd_err : 1;
    uint32_t reserved : 24;
    uint32_t jsp0_wrk : 1;
    uint32_t jsp0_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_jsp0;

  /* 0x0000506c reg5147 */
  struct
  {
    uint32_t jsp1_org_err : 1;
    uint32_t jsp1_madi_err : 1;
    uint32_t reserved : 26;
    uint32_t jsp1_wrk : 1;
    uint32_t jsp1_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_jsp1;

  /* 0x00005070 reg5148 */
  struct
  {
    uint32_t jpeg_org_err : 1;
    uint32_t reserved : 27;
    uint32_t jpeg_wrk : 1;
    uint32_t jpeg_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_ctrl_jpeg;

  /* 0x00005074 reg5149 */
  struct
  {
    uint32_t dma_brsp_idle : 1;
    uint32_t jpeg_frm_done : 1;
    uint32_t rdo_frm_done : 1;
    uint32_t lpf_frm_done : 1;
    uint32_t ent_frm_done : 1;
    uint32_t ppl_ctrl_done : 1;
    uint32_t criw_frm_done : 1;
    uint32_t meiw_frm_done : 1;
    uint32_t smiw_frm_done : 1;
    uint32_t strg_rsrc_done : 1;
    uint32_t reserved : 18;
    uint32_t frm_wrk : 1;
    uint32_t frm_tout : 1;
    uint32_t reserved1 : 2;
  } dbg_tctrl0;

  /* 0x00005078 reg5150 */
  struct
  {
    uint32_t pp0_cmd_vld : 1;
    uint32_t pp0_cmd_rdy : 1;
    uint32_t pp0_cmd_eid : 1;
    uint32_t cme_madp_vld : 1;
    uint32_t cme_madp_rdy0 : 1;
    uint32_t cmd_madp_rdy1 : 1;
    uint32_t cme_mv16_vld : 1;
    uint32_t cmd_mv16_rdy : 1;
    uint32_t swin_cmd_vld : 1;
    uint32_t swin_cmd_rdy : 1;
    uint32_t pnra_olm_vld : 1;
    uint32_t pnra_olm_rdy : 1;
    uint32_t lpf_rcol_vld : 1;
    uint32_t lpf_rcol_rdy : 1;
    uint32_t bsw_dat_vld : 1;
    uint32_t bsw_dat_rdy : 1;
    uint32_t slc_fifo_full : 1;
    uint32_t reserved : 15;
  } dbg_tctrl1;

  /* 0x507c */
  uint32_t reserved_5151;

  /* 0x00005080 reg5152 */
  struct
  {
    uint32_t sli_num : 15;
    uint32_t reserved : 17;
  } st_sli_num;

  /* 0x5084 - 0x50fc */
  uint32_t reserved5153_5183[31];

  /* 0x00005100 reg5184 */
  struct
  {
    uint32_t empty_oafifo : 1;
    uint32_t full_cmd_oafifo : 1;
    uint32_t full_data_oafifo : 1;
    uint32_t empty_iafifo : 1;

    uint32_t full_cmd_iafifo : 1;
    uint32_t full_info_iafifo : 1;
    uint32_t fbd_brq_st : 4;
    uint32_t fbd_hdr_vld : 1;
    uint32_t fbd_bmng_end : 1;

    uint32_t nfbd_req_st : 4;
    uint32_t acc_axi_cmd : 8;
    uint32_t reserved : 8;
  } dbg_pp_st;

  /* 0x00005104 reg5185 */
  struct
  {
    uint32_t r_ena_lambd : 1;
    uint32_t r_fst_swinw_end : 1;
    uint32_t r_swinw_end : 1;
    uint32_t r_cnt_swinw : 1;

    uint32_t r_dspw_end : 1;
    uint32_t r_dspw_cnt : 1;
    uint32_t i_sjgen_work : 1;
    uint32_t r_end_rspgen : 1;

    uint32_t r_cost_gate : 1;
    uint32_t r_ds_gate : 1;
    uint32_t r_mvp_gate : 1;
    uint32_t i_smvp_arrdy : 1;

    uint32_t i_smvp_arvld : 1;
    uint32_t i_stptr_wrdy : 1;
    uint32_t i_stptr_wvld : 1;
    uint32_t i_rdy_atf : 1;

    uint32_t i_vld_atf : 1;
    uint32_t i_rdy_bmv16 : 1;
    uint32_t i_vld_bmv16 : 1;
    uint32_t i_wr_dsp : 1;

    uint32_t i_rdy_dsp : 1;
    uint32_t i_vld_dsp : 1;
    uint32_t r_rdy_org : 1;
    uint32_t i_vld_org : 1;

    uint32_t i_rdy_state : 1;
    uint32_t i_vld_state : 1;
    uint32_t i_rdy_madp : 1;
    uint32_t i_vld_madp : 1;

    uint32_t i_rdy_diff : 1;
    uint32_t i_vld_diff : 1;
    uint32_t reserved : 2;
  } dbg_cime_st;

  /* 0x00005108 reg5186 */
  uint32_t swin_dbg_inf;

  /* 0x0000510c reg5187 */
  struct
  {
    uint32_t bbrq_cmps_left_len2 : 1;
    uint32_t bbrq_cmps_left_len1 : 1;
    uint32_t cmps_left_len0 : 1;
    uint32_t bbrq_rdy2 : 1;
    uint32_t dcps_vld2 : 1;
    uint32_t bbrq_rdy1 : 1;
    uint32_t dcps_vld1 : 1;
    uint32_t bbrq_rdy0 : 1;
    uint32_t dcps_vld0 : 1;
    uint32_t hb_rdy2 : 1;
    uint32_t bbrq_vld2 : 1;
    uint32_t hb_rdy1 : 1;
    uint32_t bbrq_vld1 : 1;
    uint32_t hb_rdy0 : 1;
    uint32_t bbrq_vld0 : 1;
    uint32_t idle_msb2 : 1;
    uint32_t idle_msb1 : 1;
    uint32_t idle_msb0 : 1;
    uint32_t cur_state_dcps : 1;
    uint32_t cur_state_bbrq : 1;
    uint32_t cur_state_hb : 1;
    uint32_t cke_bbrq_dcps : 1;
    uint32_t cke_dcps : 1;
    uint32_t cke_bbrq : 1;
    uint32_t rdy_lwcd_rsp : 1;
    uint32_t vld_lwcd_rsp : 1;
    uint32_t rdy_lwcd_req : 1;
    uint32_t vld_lwcd_req : 1;
    uint32_t rdy_lwrsp : 1;
    uint32_t vld_lwrsp : 1;
    uint32_t rdy_lwreq : 1;
    uint32_t vld_lwreq : 1;
  } dbg_fbd_hhit0;

  /* 0x00005110 reg5188 */
  uint32_t rfme_dbg_inf;

  /* 0x00005114 reg5189 */
  struct
  {
    uint32_t mscnt_clr : 1;
    uint32_t reserved : 31;
  } dbg_cach_clr;

  /* 0x00005118 reg5190 */
  uint32_t l1_mis;

  /* 0x0000511c reg5191 */
  uint32_t l2_mis;

  /* 0x00005120 reg5192 */
  uint32_t rdo_dbg0;

  /* 0x00005124 reg5193 */
  uint32_t rdo_dbg1;

  /* 0x00005128 reg5194 */
  struct
  {
    uint32_t h264_sh_st_cs : 4;
    uint32_t rsd_st_cs : 4;
    uint32_t h264_sd_st_cs : 5;
    uint32_t etpy_rdy : 1;
    uint32_t reserved : 18;
  } dbg_etpy;

  /* 0x0000512c reg5195 */
  struct
  {
    uint32_t chl_aw_vld : 10;
    uint32_t chl_aw_rdy : 10;
    uint32_t aw_vld_arb : 1;
    uint32_t aw_rdy_arb : 1;
    uint32_t aw_vld_crosclk : 1;
    uint32_t aw_rdy_crosclk : 1;
    uint32_t aw_rdy_mmu : 1;
    uint32_t aw_vld_mmu : 1;
    uint32_t aw_rdy_axi : 1;
    uint32_t aw_vld_axi : 1;
    uint32_t reserved : 4;
  } dbg_dma_aw;

  /* 0x00005130 reg5196 */
  struct
  {
    uint32_t chl_w_vld : 10;
    uint32_t chl_w_rdy : 10;
    uint32_t w_vld_arb : 1;
    uint32_t w_rdy_arb : 1;
    uint32_t w_vld_crosclk : 1;
    uint32_t w_rdy_crosclk : 1;
    uint32_t w_rdy_mmu : 1;
    uint32_t w_vld_mmu : 1;
    uint32_t w_rdy_axi : 1;
    uint32_t w_vld_axi : 1;
    uint32_t reserved : 4;
  } dbg_dma_w;

  /* 0x00005134 reg5197 */
  struct
  {
    uint32_t chl_ar_vld : 9;
    uint32_t chl_ar_rdy : 9;
    uint32_t reserved : 2;
    uint32_t ar_vld_arb : 1;
    uint32_t ar_rdy_arb : 1;
    uint32_t ar_vld_crosclk : 1;
    uint32_t ar_rdy_crosclk : 1;
    uint32_t ar_rdy_mmu : 1;
    uint32_t ar_vld_mmu : 1;
    uint32_t ar_rdy_axi : 1;
    uint32_t ar_vld_axi : 1;
    uint32_t reserved1 : 4;
  } dbg_dma_ar;

  /* 0x00005138 reg5198 */
  struct
  {
    uint32_t chl_r_vld : 9;
    uint32_t chl_r_rdy : 9;
    uint32_t reserved : 2;
    uint32_t r_vld_arb : 1;
    uint32_t r_rdy_arb : 1;
    uint32_t r_vld_crosclk : 1;
    uint32_t r_rdy_crosclk : 1;
    uint32_t r_rdy_mmu : 1;
    uint32_t r_vld_mmu : 1;
    uint32_t r_rdy_axi : 1;
    uint32_t r_vld_axi : 1;
    uint32_t b_rdy_mmu : 1;
    uint32_t b_vld_mmu : 1;
    uint32_t b_rdy_axi : 1;
    uint32_t b_vld_axi : 1;
  } dbg_dma_r;

  /* 0x513c */
  uint32_t reserved_5199;

  /* 0x00005140 reg5200 */
  struct
  {
    uint32_t bsw_fsm_stus : 4;
    uint32_t bsw_aw_full : 1;
    uint32_t bsw_rdy_ent : 1;
    uint32_t bsw_vld_ent : 1;
    uint32_t jpg_bsw_stus : 4;
    uint32_t jpg_aw_full : 1;
    uint32_t jpg_bsw_rdy : 1;
    uint32_t jpg_bsw_vld : 1;
    uint32_t crpw_fsm_stus : 3;
    uint32_t hdwr_rdy : 1;
    uint32_t hdwr_vld : 1;
    uint32_t bdwr_rdy : 1;
    uint32_t bdwr_vld : 1;
    uint32_t nfbc_rdy : 1;
    uint32_t nfbc_vld : 1;
    uint32_t dsp_fsm_stus : 2;
    uint32_t dsp_wr_flg : 1;
    uint32_t dsp_rsy : 1;
    uint32_t dsp_vld : 1;
    uint32_t lpfw_fsm_stus : 3;
    uint32_t reserved : 1;
  } dbg_dma_dbg1;

  /* 0x5144 */
  uint32_t reserved_5201;

  /* 0x00005148 reg5202 */
  struct
  {
    uint32_t rdo_st : 20;
    uint32_t reserved : 12;
  } dbg_rdo_st;

  /* 0x0000514c reg5203 */
  struct
  {
    uint32_t lpf_work : 1;
    uint32_t rdo_par_nrdy : 1;
    uint32_t rdo_rcn_nrdy : 1;
    uint32_t lpf_rcn_rdy : 1;
    uint32_t dblk_work : 1;
    uint32_t sao_work : 1;
    uint32_t reserved : 18;
    uint32_t tile_bdry_read : 1;
    uint32_t tile_bdry_write : 1;
    uint32_t tile_bdry_rrdy : 1;
    uint32_t rdo_read_tile_bdry : 1;
    uint32_t rdo_write_tile_bdry : 1;
    uint32_t reserved1 : 3;
  } dbg_lpf;

  /* 0x5150 */
  uint32_t reserved_5204;

  /* 0x00005154 reg5205 */
  uint32_t dbg0_cache;

  /* 0x00005158 reg5206 */
  uint32_t dbg1_cache;

  /* 0x0000515c reg5207 */
  uint32_t dbg2_cache;

  /* 0x00005160 reg5208 */
  struct
  {
    uint32_t ebuf_diff_cmd : 8;
    uint32_t lbuf_lpf_ncnt : 7;
    uint32_t lbuf_lpf_cien : 1;
    uint32_t lbuf_rdo_ncnt : 7;
    uint32_t lbuf_rdo_cien : 1;
    uint32_t reserved : 8;
  } dbg_lbuf0;

  /* 0x00005164 reg5209 */
  struct
  {
    uint32_t rvld_ebfr : 1;
    uint32_t rrdy_ebfr : 1;
    uint32_t arvld_ebfr : 1;
    uint32_t arrdy_ebfr : 1;
    uint32_t wvld_ebfw : 1;
    uint32_t wrdy_ebfw : 1;
    uint32_t awvld_ebfw : 1;
    uint32_t awrdy_ebfw : 1;
    uint32_t lpf_lbuf_rvld : 1;
    uint32_t lpf_lbuf_rrdy : 1;
    uint32_t lpf_lbuf_wvld : 1;
    uint32_t lpf_lbuf_wrdy : 1;
    uint32_t rdo_lbuf_rvld : 1;
    uint32_t rdo_lbuf_rrdy : 1;
    uint32_t rdo_lbuf_wvld : 1;
    uint32_t rdo_lbuf_wrdy : 1;
    uint32_t fme_lbuf_rvld : 1;
    uint32_t fme_lbuf_rrdy : 1;
    uint32_t cme_lbuf_rvld : 1;
    uint32_t cme_lbuf_rrdy : 1;
    uint32_t smear_lbuf_rvld : 1;
    uint32_t smear_lbuf_rrdy : 1;
    uint32_t smear_lbuf_wvld : 1;
    uint32_t smear_lbuf_wrdy : 1;
    uint32_t rdo_lbufw_flag : 1;
    uint32_t rdo_lbufr_flag : 1;
    uint32_t cme_lbufr_flag : 1;
    uint32_t reserved : 5;
  } dbg_lbuf1;

  /* 0x00005168 reg5210 */
  struct
  {
    uint32_t dbg_isp_fcnt : 8;
    uint32_t dbg_isp_fcyc : 24;
  } dbg_dvbm_isp0;

  /* 0x0000516c reg5211 */
  struct
  {
    uint32_t dbg_isp_lcnt : 14;
    uint32_t reserved : 1;
    uint32_t dbg_isp_ltgl : 1;
    uint32_t dbg_isp_fcnt : 8;
    uint32_t dbg_isp_oflw : 1;
    uint32_t dbg_isp_ftgl : 1;
    uint32_t dbg_isp_full : 1;
    uint32_t dbg_isp_work : 1;
    uint32_t dbg_isp_lvld : 1;
    uint32_t dbg_isp_lrdy : 1;
    uint32_t dbg_isp_fvld : 1;
    uint32_t dbg_isp_frdy : 1;
  } dbg_dvbm_isp1;

  /* 0x00005170 reg5212 */
  struct
  {
    uint32_t dbg_bf0_isp_lcnt : 14;
    uint32_t dbg_bf0_isp_llst : 1;
    uint32_t dbg_bf0_isp_sofw : 1;
    uint32_t dbg_bf0_isp_fcnt : 8;
    uint32_t dbg_bf0_isp_pnt : 1;
    uint32_t reserved : 3;
    uint32_t dbg_bf0_vpu_pnt : 1;
    uint32_t reserved1 : 3;
  } dbg_dvbm_buf0_inf0;

  /* 0x00005174 reg5213 */
  struct
  {
    uint32_t dbg_bf0_src_lcnt : 14;
    uint32_t dbg_bf0_src_llst : 1;
    uint32_t reserved : 1;
    uint32_t dbg_bf0_vpu_lcnt : 14;
    uint32_t dbg_bf0_vpu_llst : 1;
    uint32_t dbg_bf0_vpu_vofw : 1;
  } dbg_dvbm_buf0_inf1;

  /* 0x00005178 reg5214 */
  struct
  {
    uint32_t dbg_bf1_isp_lcnt : 14;
    uint32_t dbg_bf1_isp_llst : 1;
    uint32_t dbg_bf1_isp_sofw : 1;
    uint32_t dbg_bf1_isp_fcnt : 1;
    uint32_t reserved : 7;
    uint32_t dbg_bf1_isp_pnt : 1;
    uint32_t reserved1 : 3;
    uint32_t dbg_bf1_vpu_pnt : 1;
    uint32_t reserved2 : 3;
  } dbg_dvbm_buf1_inf0;

  /* 0x0000517c reg5215 */
  struct
  {
    uint32_t dbg_bf1_src_lcnt : 14;
    uint32_t dbg_bf1_src_llst : 1;
    uint32_t reserved : 1;
    uint32_t dbg_bf1_vpu_lcnt : 14;
    uint32_t dbg_bf1_vpu_llst : 1;
    uint32_t dbg_bf1_vpu_vofw : 1;
  } dbg_dvbm_buf1_inf1;

  /* 0x00005180 reg5216 */
  struct
  {
    uint32_t dbg_bf2_isp_lcnt : 14;
    uint32_t dbg_bf2_isp_llst : 1;
    uint32_t dbg_bf2_isp_sofw : 1;
    uint32_t dbg_bf2_isp_fcnt : 1;
    uint32_t reserved : 7;
    uint32_t dbg_bf2_isp_pnt : 1;
    uint32_t reserved1 : 3;
    uint32_t dbg_bf2_vpu_pnt : 1;
    uint32_t reserved2 : 3;
  } dbg_dvbm_buf2_inf0;

  /* 0x00005184 reg5217 */
  struct
  {
    uint32_t dbg_bf2_src_lcnt : 14;
    uint32_t dbg_bf2_src_llst : 1;
    uint32_t reserved : 1;
    uint32_t dbg_bf2_vpu_lcnt : 14;
    uint32_t dbg_bf2_vpu_llst : 1;
    uint32_t dbg_bf2_vpu_vofw : 1;
  } dbg_dvbm_buf2_inf1;

  /* 0x00005188 reg5218 */
  struct
  {
    uint32_t dbg_bf3_isp_lcnt : 14;
    uint32_t dbg_bf3_isp_llst : 1;
    uint32_t dbg_bf3_isp_sofw : 1;
    uint32_t dbg_bf3_isp_fcnt : 1;
    uint32_t reserved : 7;
    uint32_t dbg_bf3_isp_pnt : 1;
    uint32_t reserved1 : 3;
    uint32_t dbg_bf3_vpu_pnt : 1;
    uint32_t reserved2 : 3;
  } dbg_dvbm_buf3_inf0;

  /* 0x0000518c reg5219 */
  struct
  {
    uint32_t dbg_bf3_src_lcnt : 14;
    uint32_t dbg_bf3_src_llst : 1;
    uint32_t reserved : 1;
    uint32_t dbg_bf3_vpu_lcnt : 14;
    uint32_t dbg_bf3_vpu_llst : 1;
    uint32_t dbg_bf3_vpu_vofw : 1;
  } dbg_dvbm_buf3_inf1;

  /* 0x00005190 reg5220 */
  struct
  {
    uint32_t dbg_isp_fptr : 3;
    uint32_t dbg_isp_full : 1;
    uint32_t dbg_src_fptr : 3;
    uint32_t reserved : 1;
    uint32_t dbg_vpu_fptr : 3;
    uint32_t dbg_vpu_empt : 1;
    uint32_t dbg_vpu_lvld : 1;
    uint32_t dbg_vpu_lrdy : 1;
    uint32_t dbg_vpu_fvld : 1;
    uint32_t dbg_vpu_frdy : 1;
    uint32_t dbg_fcnt_misp : 4;
    uint32_t dbg_fcnt_mvpu : 4;
    uint32_t dbg_fcnt_sofw : 4;
    uint32_t dbg_fcnt_vofw : 4;
  } dbg_dvbm_ctrl;

  /* 0x5194 - 0x519c */
  uint32_t reserved5221_5223[3];

  /* 0x000051a0 reg5224 */
  uint32_t dbg_dvbm_buf0_yadr;

  /* 0x000051a4 reg5225 */
  uint32_t dbg_dvbm_buf0_cadr;

  /* 0x000051a8 reg5226 */
  uint32_t dbg_dvbm_buf1_yadr;

  /* 0x000051ac reg5227 */
  uint32_t dbg_dvbm_buf1_cadr;

  /* 0x000051b0 reg5228 */
  uint32_t dbg_dvbm_buf2_yadr;

  /* 0x000051b4 reg5229 */
  uint32_t dbg_dvbm_buf2_cadr;

  /* 0x000051b8 reg5230 */
  uint32_t dbg_dvbm_buf3_yadr;

  /* 0x000051bc reg5231 */
  uint32_t dbg_dvbm_buf3_cadr;

  /* 0x000051c0 reg5232 */
  struct
  {
    uint32_t dchs_rx_cnt : 11;
    uint32_t dchs_rx_id : 2;
    uint32_t dchs_rx_en : 1;
    uint32_t dchs_rx_ack : 1;
    uint32_t dchs_rx_req : 1;
    uint32_t dchs_tx_cnt : 11;
    uint32_t dchs_tx_id : 2;
    uint32_t dchs_tx_en : 1;
    uint32_t dchs_tx_ack : 1;
    uint32_t dchs_tx_req : 1;
  } dbg_dchs_intfc;

  /* 0x000051c4 reg5233 */
  struct
  {
    uint32_t lpfw_tx_cnt : 11;
    uint32_t lpfw_tx_en : 1;
    uint32_t crpw_tx_cnt : 11;
    uint32_t crpw_tx_en : 1;
    uint32_t dual_err_updt : 1;
    uint32_t dlyc_fifo_oflw : 1;
    uint32_t dlyc_tx_vld : 1;
    uint32_t dlyc_tx_rdy : 1;
    uint32_t dlyc_tx_empty : 1;
    uint32_t dchs_tx_idle : 1;
    uint32_t dchs_tx_asy : 1;
    uint32_t dchs_tx_syn : 1;
  } dbg_dchs_tx_inf0;

  /* 0x000051c8 reg5234 */
  struct
  {
    uint32_t criw_tx_cnt : 11;
    uint32_t criw_tx_en : 1;
    uint32_t smrw_tx_cnt : 11;
    uint32_t smrw_tx_en : 1;
    uint32_t reserved : 8;
  } dbg_dchs_tx_inf1;

  /* 0x000051cc reg5235 */
  struct
  {
    uint32_t dual_rx_cnt : 11;
    uint32_t dual_rx_id : 2;
    uint32_t dual_rx_en : 1;
    uint32_t dual_rx_syn : 1;
    uint32_t dual_rx_lock : 1;
    uint32_t dual_lpfr_dule : 1;
    uint32_t dual_cime_dule : 1;
    uint32_t dual_clomv_dule : 1;
    uint32_t dual_smear_dule : 1;
    uint32_t reserved : 12;
  } dbg_dchs_rx_inf0;

  /* 0x51d0 - 0x51fc */
  uint32_t reserved5236_5247[12];

  /* 0x00005200 reg5248 */
  uint32_t frame_cyc;

  /* 0x00005204 reg5249 */
  uint32_t vsp0_fcyc;

  /* 0x00005208 reg5250 */
  uint32_t vsp1_fcyc;

  /* 0x0000520c reg5251 */
  uint32_t cme_fcyc;

  /* 0x00005210 reg5252 */
  uint32_t ldr_fcyc;

  /* 0x00005214 reg5253 */
  uint32_t rfme_fcyc;

  /* 0x00005218 reg5254 */
  uint32_t fme_fcyc;

  /* 0x0000521c reg5255 */
  uint32_t rdo_fcyc;

  /* 0x00005220 reg5256 */
  uint32_t lpf_fcyc;

  /* 0x00005224 reg5257 */
  uint32_t etpy_fcyc;

  /* 0x00005228 reg5258 */
  uint32_t jsp0_fcyc;

  /* 0x0000522c reg5259 */
  uint32_t jsp1_fcyc;

  /* 0x00005230 reg5260 */
  uint32_t jpeg_fcyc;
} Vepu510Dbg;

typedef struct Vepu510H264RoiBlkCfg
{
  uint32_t qp_adju : 8;
  uint32_t mdc_adju_inter : 4;
  uint32_t mdc_adju_skip : 4;
  uint32_t mdc_adju_intra : 4;
  uint32_t reserved : 12;
} Vepu510H264RoiBlkCfg;

typedef struct Vepu510H265RoiBlkCfg
{
  uint32_t qp_adju : 8;
  uint32_t reserved : 12;
  uint32_t mdc_adju_inter : 4;
  uint32_t mdc_adju_skip : 4;
  uint32_t mdc_adju_intra : 4;
} Vepu510H265RoiBlkCfg;

typedef struct H264eVepu510Frame_t
{

  Vepu510FrmCommon common;

  /* 0x000003a0 reg232 */
  struct
  {
    uint32_t rect_size : 1;
    uint32_t reserved : 2;
    uint32_t vlc_lmt : 1;
    uint32_t chrm_spcl : 1;
    uint32_t reserved1 : 8;
    uint32_t ccwa_e : 1;
    uint32_t reserved2 : 1;
    uint32_t atr_e : 1;
    uint32_t reserved3 : 4;
    uint32_t scl_lst_sel : 2;
    uint32_t reserved4 : 6;
    uint32_t atf_e : 1;
    uint32_t atr_mult_sel_e : 1;
    uint32_t reserved5 : 2;
  } rdo_cfg;

  /* 0x000003a4 reg233 */
  struct
  {
    uint32_t rdo_mark_mode : 9;
    uint32_t reserved : 23;
  } iprd_csts;

  /* 0x3a8 - 0x3ac */
  uint32_t reserved234_235[2];

  /* 0x000003b0 reg236 */
  struct
  {
    uint32_t nal_ref_idc : 2;
    uint32_t nal_unit_type : 5;
    uint32_t reserved : 25;
  } synt_nal;

  /* 0x000003b4 reg237 */
  struct
  {
    uint32_t max_fnum : 4;
    uint32_t drct_8x8 : 1;
    uint32_t mpoc_lm4 : 4;
    uint32_t poc_type : 2;
    uint32_t reserved : 21;
  } synt_sps;

  /* 0x000003b8 reg238 */
  struct
  {
    uint32_t etpy_mode : 1;
    uint32_t trns_8x8 : 1;
    uint32_t csip_flag : 1;
    uint32_t num_ref0_idx : 2;
    uint32_t num_ref1_idx : 2;
    uint32_t pic_init_qp : 6;
    uint32_t cb_ofst : 5;
    uint32_t cr_ofst : 5;
    uint32_t reserved : 1;
    uint32_t dbf_cp_flg : 1;
    uint32_t reserved1 : 7;
  } synt_pps;

  /* 0x000003bc reg239 */
  struct
  {
    uint32_t sli_type : 2;
    uint32_t pps_id : 8;
    uint32_t drct_smvp : 1;
    uint32_t num_ref_ovrd : 1;
    uint32_t cbc_init_idc : 2;
    uint32_t reserved : 2;
    uint32_t frm_num : 16;
  } synt_sli0;

  /* 0x000003c0 reg240 */
  struct
  {
    uint32_t idr_pid : 16;
    uint32_t poc_lsb : 16;
  } synt_sli1;

  /* 0x000003c4 reg241 */
  struct
  {
    uint32_t rodr_pic_idx : 2;
    uint32_t ref_list0_rodr : 1;
    uint32_t sli_beta_ofst : 4;
    uint32_t sli_alph_ofst : 4;
    uint32_t dis_dblk_idc : 2;
    uint32_t reserved : 3;
    uint32_t rodr_pic_num : 16;
  } synt_sli2;

  /* 0x000003c8 reg242 */
  struct
  {
    uint32_t nopp_flg : 1;
    uint32_t ltrf_flg : 1;
    uint32_t arpm_flg : 1;
    uint32_t mmco4_pre : 1;
    uint32_t mmco_type0 : 3;
    uint32_t mmco_parm0 : 16;
    uint32_t mmco_type1 : 3;
    uint32_t mmco_type2 : 3;
    uint32_t reserved : 3;
  } synt_refm0;

  /* 0x000003cc reg243 */
  struct
  {
    uint32_t mmco_parm1 : 16;
    uint32_t mmco_parm2 : 16;
  } synt_refm1;

  /* 0x000003d0 reg244 */
  struct
  {
    uint32_t long_term_frame_idx0 : 4;
    uint32_t long_term_frame_idx1 : 4;
    uint32_t long_term_frame_idx2 : 4;
    uint32_t reserved : 20;
  } synt_refm2;

  /* 0x000003d4 reg245 */
  struct
  {
    uint32_t dlt_poc_s0_m12 : 16;
    uint32_t dlt_poc_s0_m13 : 16;
  } synt_refm3_hevc;

  /* 0x000003d8 reg246 */
  struct
  {
    uint32_t poc_lsb_lt1 : 16;
    uint32_t poc_lsb_lt2 : 16;
  } synt_long_refm0_hevc;

  /* 0x000003dc reg247 */
  struct
  {
    uint32_t dlt_poc_msb_cycl1 : 16;
    uint32_t dlt_poc_msb_cycl2 : 16;
  } synt_long_refm1_hevc;

  /* 0x000003e0 reg248 */
  struct
  {
    uint32_t sao_lambda_multi : 3;
    uint32_t reserved : 29;
  } sao_cfg_hevc;

  /* 0x3e4 - 0x3ec */
  uint32_t reserved249_251[3];

  /* 0x000003f0 reg252 */
  struct
  {
    uint32_t mv_v_lmt_thd : 14;
    uint32_t reserved : 1;
    uint32_t mv_v_lmt_en : 1;
    uint32_t reserved1 : 16;
  } sli_cfg;

  /* 0x000003f4 reg253 */
  struct
  {
    uint32_t tile_x : 9;
    uint32_t reserved : 7;
    uint32_t tile_y : 9;
    uint32_t reserved1 : 7;
  } tile_pos_hevc;
} H264eVepu510Frame;

typedef struct H264eVepu510Param_t
{
  /* 0x00001700 reg1472 */
  struct
  {
    uint32_t iprd_tthdy4_0 : 12;
    uint32_t reserved : 4;
    uint32_t iprd_tthdy4_1 : 12;
    uint32_t reserved1 : 4;
  } iprd_tthdy4_0;

  /* 0x00001704 reg1473 */
  struct
  {
    uint32_t iprd_tthdy4_2 : 12;
    uint32_t reserved : 4;
    uint32_t iprd_tthdy4_3 : 12;
    uint32_t reserved1 : 4;
  } iprd_tthdy4_1;

  /* 0x00001708 reg1474 */
  struct
  {
    uint32_t iprd_tthdc8_0 : 12;
    uint32_t reserved : 4;
    uint32_t iprd_tthdc8_1 : 12;
    uint32_t reserved1 : 4;
  } iprd_tthdc8_0;

  /* 0x0000170c reg1475 */
  struct
  {
    uint32_t iprd_tthdc8_2 : 12;
    uint32_t reserved : 4;
    uint32_t iprd_tthdc8_3 : 12;
    uint32_t reserved1 : 4;
  } iprd_tthdc8_1;

  /* 0x00001710 reg1476 */
  struct
  {
    uint32_t iprd_tthdy8_0 : 12;
    uint32_t reserved : 4;
    uint32_t iprd_tthdy8_1 : 12;
    uint32_t reserved1 : 4;
  } iprd_tthdy8_0;

  /* 0x00001714 reg1477 */
  struct
  {
    uint32_t iprd_tthdy8_2 : 12;
    uint32_t reserved : 4;
    uint32_t iprd_tthdy8_3 : 12;
    uint32_t reserved1 : 4;
  } iprd_tthdy8_1;

  /* 0x00001718 reg1478 */
  struct
  {
    uint32_t iprd_tthd_ul : 12;
    uint32_t reserved : 20;
  } iprd_tthd_ul;

  /* 0x0000171c reg1479 */
  struct
  {
    uint32_t iprd_wgty8_0 : 8;
    uint32_t iprd_wgty8_1 : 8;
    uint32_t iprd_wgty8_2 : 8;
    uint32_t iprd_wgty8_3 : 8;
  } iprd_wgty8;

  /* 0x00001720 reg1480 */
  struct
  {
    uint32_t iprd_wgty4_0 : 8;
    uint32_t iprd_wgty4_1 : 8;
    uint32_t iprd_wgty4_2 : 8;
    uint32_t iprd_wgty4_3 : 8;
  } iprd_wgty4;

  /* 0x00001724 reg1481 */
  struct
  {
    uint32_t iprd_wgty16_0 : 8;
    uint32_t iprd_wgty16_1 : 8;
    uint32_t iprd_wgty16_2 : 8;
    uint32_t iprd_wgty16_3 : 8;
  } iprd_wgty16;

  /* 0x00001728 reg1482 */
  struct
  {
    uint32_t iprd_wgtc8_0 : 8;
    uint32_t iprd_wgtc8_1 : 8;
    uint32_t iprd_wgtc8_2 : 8;
    uint32_t iprd_wgtc8_3 : 8;
  } iprd_wgtc8;

  /* 0x172c */
  uint32_t reserved_1483;

  /* 0x00001730 reg1484 */
  struct
  {
    uint32_t qnt_f_bias_i : 10;
    uint32_t qnt_f_bias_p : 10;
    uint32_t reserve : 12;
  } qnt_bias_comb;

  /* 0x1734 - 0x173c */
  uint32_t reserved1485_1487[3];

  /* 0x00001740 reg1488 */
  struct
  {
    uint32_t thd0 : 8;
    uint32_t reserve0 : 8;
    uint32_t thd1 : 8;
    uint32_t reserve1 : 8;
  } atr_thd0;

  /* 0x00001744 reg1489 */
  struct
  {
    uint32_t thd2 : 8;
    uint32_t reserve0 : 8;
    uint32_t thdqp : 6;
    uint32_t reserve1 : 10;
  } atr_thd1;

  /* 0x1748 - 0x174c */
  uint32_t reserved1490_1491[2];

  /* 0x00001750 reg1492 */
  struct
  {
    uint32_t atr_lv16_wgt0 : 8;
    uint32_t atr_lv16_wgt1 : 8;
    uint32_t atr_lv16_wgt2 : 8;
    uint32_t reserved : 8;
  } atr_wgt16;

  /* 0x00001754  reg1493*/
  struct
  {
    uint32_t atr_lv8_wgt0 : 8;
    uint32_t atr_lv8_wgt1 : 8;
    uint32_t atr_lv8_wgt2 : 8;
    uint32_t reserved : 8;
  } atr_wgt8;

  /* 0x00001758 reg1494 */
  struct
  {
    uint32_t atr_lv4_wgt0 : 8;
    uint32_t atr_lv4_wgt1 : 8;
    uint32_t atr_lv4_wgt2 : 8;
    uint32_t reserved : 8;
  } atr_wgt4;

  /* 0x175c */
  uint32_t reserved_1495;

  /* 0x00001760 reg1496 - 0x000019cc reg1651 */
  Vepu510WgtCommon common;
} H264eVepu510Param;

typedef struct H264eVepu510SqiCfg_t
{
  /* 0x00002000 reg2048 - 0x00002010 reg2052*/
  uint32_t reserved_2048_2052[5];

  /* 0x00002014 reg2053 */
  struct
  {
    uint32_t rdo_smear_lvl16_multi : 8;
    uint32_t rdo_smear_dlt_qp : 4;
    uint32_t reserved : 1;
    uint32_t stated_mode : 2;
    uint32_t rdo_smear_en : 1;
    uint32_t reserved1 : 16;
  } smear_opt_cfg;

  /* 0x00002018 reg2054 */
  struct
  {
    uint32_t madp_cur_thd0 : 12;
    uint32_t reserved : 4;
    uint32_t madp_cur_thd1 : 12;
    uint32_t reserved1 : 4;
  } smear_madp_thd0;

  /* 0x0000201c reg2055 */
  struct
  {
    uint32_t madp_cur_thd2 : 12;
    uint32_t reserved : 4;
    uint32_t madp_cur_thd3 : 12;
    uint32_t reserved1 : 4;
  } smear_madp_thd1;

  /* 0x00002020 reg2056 */
  struct
  {
    uint32_t madp_around_thd0 : 12;
    uint32_t reserved : 4;
    uint32_t madp_around_thd1 : 12;
    uint32_t reserved1 : 4;
  } smear_madp_thd2;

  /* 0x00002024 reg2057 */
  struct
  {
    uint32_t madp_around_thd2 : 12;
    uint32_t reserved : 4;
    uint32_t madp_around_thd3 : 12;
    uint32_t reserved1 : 4;
  } smear_madp_thd3;

  /* 0x00002028 reg2058 */
  struct
  {
    uint32_t madp_around_thd4 : 12;
    uint32_t reserved : 4;
    uint32_t madp_around_thd5 : 12;
    uint32_t reserved1 : 4;
  } smear_madp_thd4;

  /* 0x0000202c reg2059 */
  struct
  {
    uint32_t madp_ref_thd0 : 12;
    uint32_t reserved : 4;
    uint32_t madp_ref_thd1 : 12;
    uint32_t reserved1 : 4;
  } smear_madp_thd5;

  /* 0x00002030 reg2060 */
  struct
  {
    uint32_t cnt_cur_thd0 : 4;
    uint32_t reserved : 4;
    uint32_t cnt_cur_thd1 : 4;
    uint32_t reserved1 : 4;
    uint32_t cnt_cur_thd2 : 4;
    uint32_t reserved2 : 4;
    uint32_t cnt_cur_thd3 : 4;
    uint32_t reserved3 : 4;
  } smear_cnt_thd0;

  /* 0x00002034 reg2061 */
  struct
  {
    uint32_t cnt_around_thd0 : 4;
    uint32_t reserved : 4;
    uint32_t cnt_around_thd1 : 4;
    uint32_t reserved1 : 4;
    uint32_t cnt_around_thd2 : 4;
    uint32_t reserved2 : 4;
    uint32_t cnt_around_thd3 : 4;
    uint32_t reserved3 : 4;
  } smear_cnt_thd1;

  /* 0x00002038 reg2062 */
  struct
  {
    uint32_t cnt_around_thd4 : 4;
    uint32_t reserved : 4;
    uint32_t cnt_around_thd5 : 4;
    uint32_t reserved1 : 4;
    uint32_t cnt_around_thd6 : 4;
    uint32_t reserved2 : 4;
    uint32_t cnt_around_thd7 : 4;
    uint32_t reserved3 : 4;
  } smear_cnt_thd2;

  /* 0x0000203c reg2063 */
  struct
  {
    uint32_t cnt_ref_thd0 : 4;
    uint32_t reserved : 4;
    uint32_t cnt_ref_thd1 : 4;
    uint32_t reserved1 : 20;
  } smear_cnt_thd3;

  /* 0x00002040 reg2064 */
  struct
  {
    uint32_t resi_small_cur_th0 : 6;
    uint32_t reserved : 2;
    uint32_t resi_big_cur_th0 : 6;
    uint32_t reserved1 : 2;
    uint32_t resi_small_cur_th1 : 6;
    uint32_t reserved2 : 2;
    uint32_t resi_big_cur_th1 : 6;
    uint32_t reserved3 : 2;
  } smear_resi_thd0;

  /* 0x00002044 reg2065 */
  struct
  {
    uint32_t resi_small_around_th0 : 6;
    uint32_t reserved : 2;
    uint32_t resi_big_around_th0 : 6;
    uint32_t reserved1 : 2;
    uint32_t resi_small_around_th1 : 6;
    uint32_t reserved2 : 2;
    uint32_t resi_big_around_th1 : 6;
    uint32_t reserved3 : 2;
  } smear_resi_thd1;

  /* 0x00002048 reg2066 */
  struct
  {
    uint32_t resi_small_around_th2 : 6;
    uint32_t reserved : 2;
    uint32_t resi_big_around_th2 : 6;
    uint32_t reserved1 : 2;
    uint32_t resi_small_around_th3 : 6;
    uint32_t reserved2 : 2;
    uint32_t resi_big_around_th3 : 6;
    uint32_t reserved3 : 2;
  } smear_resi_thd2;

  /* 0x0000204c reg2067 */
  struct
  {
    uint32_t resi_small_ref_th0 : 6;
    uint32_t reserved : 2;
    uint32_t resi_big_ref_th0 : 6;
    uint32_t reserved1 : 18;
  } smear_resi_thd3;

  /* 0x00002050 reg2068 */
  struct
  {
    uint32_t resi_th0 : 8;
    uint32_t reserved : 8;
    uint32_t resi_th1 : 8;
    uint32_t reserved1 : 8;
  } smear_resi_thd4;

  /* 0x00002054 reg2069 */
  struct
  {
    uint32_t madp_cnt_th0 : 4;
    uint32_t madp_cnt_th1 : 4;
    uint32_t madp_cnt_th2 : 4;
    uint32_t madp_cnt_th3 : 4;
    uint32_t reserved : 16;
  } smear_st_thd;

  /* 0x2058 - 0x206c */
  uint32_t reserved2070_2075[6];

  /* 0x00002070 reg2076 - 0x0000207c reg2079*/
  rdo_skip_par rdo_b16_skip;

  /* 0x00002080 reg2080 - 0x00002088 reg2082 */
  uint32_t reserved2080_2082[3];

  /* 0x0000208c reg2083 - 0x00002094 reg2085 */
  rdo_noskip_par rdo_b16_inter;

  /* 0x00002098 reg2086 - 0x000020a4 reg2088 */
  uint32_t reserved2086_2088[3];

  /* 0x000020a8 reg2089 - 0x000020ac reg2091 */
  rdo_noskip_par rdo_b16_intra;

  /* 0x000020b0 reg2092 */
  uint32_t reserved2092;

  /* 0x000020b4 reg2093 */
  struct
  {
    uint32_t thd0 : 4;
    uint32_t reserved : 4;
    uint32_t thd1 : 4;
    uint32_t reserved1 : 4;
    uint32_t thd2 : 4;
    uint32_t reserved2 : 4;
    uint32_t thd3 : 4;
    uint32_t reserved3 : 4;
  } rdo_b16_intra_atf_cnt_thd;

  /* 0x000020b8 reg2094 */
  struct
  {
    uint32_t big_th0 : 6;
    uint32_t reserved : 2;
    uint32_t big_th1 : 6;
    uint32_t reserved1 : 2;
    uint32_t small_th0 : 6;
    uint32_t reserved2 : 2;
    uint32_t small_th1 : 6;
    uint32_t reserved3 : 2;
  } rdo_atf_resi_thd;
} H264eVepu510Sqi;

typedef struct HalVepu510Reg_t
{
  Vepu510ControlCfg reg_ctl;
  H264eVepu510Frame reg_frm;
  Vepu510RcRoi reg_rc_roi;
  H264eVepu510Param reg_param;
  H264eVepu510Sqi reg_sqi;
  Vepu510SclCfg reg_scl;
  Vepu510Status reg_st;
  Vepu510Dbg reg_dbg;
} HalVepu510RegSet;

/****************************************************************************
 * Layout assertions
 *
 * These sizes were taken from the generated structs and verified equal to the
 * upstream MPP structs by `port_regmodel.py --compare`.  They are asserted at
 * compile time so that an edit to this header -- or a regeneration against a
 * different upstream revision -- cannot silently change the register layout
 * that gets written to the encoder.
 ****************************************************************************/

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(H264eVepu510Frame) == 392,
               "H264eVepu510Frame layout changed");
_Static_assert(sizeof(H264eVepu510Param) == 720,
               "H264eVepu510Param layout changed");
_Static_assert(sizeof(H264eVepu510Sqi) == 188,
               "H264eVepu510Sqi layout changed");
_Static_assert(sizeof(HalVepu510RegSet) == 3728,
               "HalVepu510RegSet layout changed");
_Static_assert(sizeof(Vepu510ControlCfg) == 96,
               "Vepu510ControlCfg layout changed");
_Static_assert(sizeof(Vepu510Dbg) == 564, "Vepu510Dbg layout changed");
_Static_assert(sizeof(Vepu510FrmCommon) == 304,
               "Vepu510FrmCommon layout changed");
_Static_assert(sizeof(Vepu510H264RoiBlkCfg) == 4,
               "Vepu510H264RoiBlkCfg layout changed");
_Static_assert(sizeof(Vepu510H265RoiBlkCfg) == 4,
               "Vepu510H265RoiBlkCfg layout changed");
_Static_assert(sizeof(Vepu510RcRoi) == 272, "Vepu510RcRoi layout changed");
_Static_assert(sizeof(Vepu510RoiCfg) == 144, "Vepu510RoiCfg layout changed");
_Static_assert(sizeof(Vepu510RoiRegion) == 16,
               "Vepu510RoiRegion layout changed");
_Static_assert(sizeof(Vepu510SclCfg) == 904, "Vepu510SclCfg layout changed");
_Static_assert(sizeof(Vepu510Status) == 592, "Vepu510Status layout changed");
_Static_assert(sizeof(Vepu510WgtCommon) == 624,
               "Vepu510WgtCommon layout changed");
_Static_assert(sizeof(Vepu51xH264Fbk) == 312, "Vepu51xH264Fbk layout changed");
_Static_assert(sizeof(Vepu51xH265Fbk) == 336, "Vepu51xH265Fbk layout changed");
_Static_assert(sizeof(Vepu5xxRoiH264BsCfg) == 8,
               "Vepu5xxRoiH264BsCfg layout changed");
_Static_assert(sizeof(VepuRgb2YuvCoeffs) == 8,
               "VepuRgb2YuvCoeffs layout changed");
_Static_assert(sizeof(rdo_noskip_par) == 12, "rdo_noskip_par layout changed");
_Static_assert(sizeof(rdo_skip_par) == 16, "rdo_skip_par layout changed");
_Static_assert(sizeof(vepu510_online) == 16, "vepu510_online layout changed");
#endif /* C11 */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VEPU510_REG_H */
