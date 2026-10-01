/****************************************************************************
 * chips/rk3576/vepu/vepu510_tables.c
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
 * RK3576 VEPU510 tuning tables.
 *
 * Source: Rockchip MPP, mpp/hal/rkenc/common/vepu51x_common.c (Apache-2.0).
 *
 * These are transcribed, not linked, so that the firmware builds from its
 * own tree.  A mistyped entry would degrade the picture with no error, so
 * every table is diffed element by element against MPP's definition by
 * rk3576-mpp-ref/verify_regs.py.  Values are reproduced verbatim, including
 * one that looks like an upstream typo -- see the note at the end.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "vepu510_tables.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Anti-flicker: thresholds and weights for skip, inter and intra blocks,
 * indexed [threshold level][strength].
 */

const uint8_t rk3576_vepu510_pskip_atf_thd[2][4] = {
  { 0, 0, 0, 1 },
  { 7, 7, 7, 10 },
};

const uint8_t rk3576_vepu510_pskip_atf_wgt[2][4] = {
  { 16, 16, 16, 20 },
  { 16, 16, 14, 16 },
};

const uint8_t rk3576_vepu510_intra_atf_thd[3][4] = {
  { 8, 16, 20, 20 },
  { 16, 32, 40, 40 },
  { 32, 56, 72, 72 },
};

const uint8_t rk3576_vepu510_intra_atf_wgt[3][4] = {
  { 16, 24, 27, 27 },
  { 16, 22, 25, 25 },
  { 16, 19, 20, 20 },
};

/* RDO lambda tables, selected by scene mode.  Each is a geometric curve over
 * QP; the encoder copies RK3576_VEPU510_LAMBDA_COPY_LEN entries from the
 * configured index onwards.
 */

const uint32_t rk3576_vepu510_lambda_default_60[60] = {
  0x00000005, 0x00000006, 0x00000007, 0x00000009, 0x0000000b, 0x0000000e,
  0x00000012, 0x00000016, 0x0000001c, 0x00000024, 0x0000002d, 0x00000039,
  0x00000048, 0x0000005b, 0x00000073, 0x00000091, 0x000000b6, 0x000000e6,
  0x00000122, 0x0000016d, 0x000001cc, 0x00000244, 0x000002db, 0x00000399,
  0x00000489, 0x000005b6, 0x00000733, 0x00000912, 0x00000b6d, 0x00000e66,
  0x00001224, 0x000016db, 0x00001ccc, 0x00002449, 0x00002db7, 0x00003999,
  0x00004892, 0x00005b6f, 0x00007333, 0x00009124, 0x0000b6de, 0x0000e666,
  0x00012249, 0x00016dbc, 0x0001cccc, 0x00024492, 0x0002db79, 0x00039999,
  0x00048924, 0x0005b6f2, 0x00073333, 0x00091249, 0x000b6de5, 0x000e6666,
  0x00122492, 0x0016dbcb, 0x001ccccc, 0x00244924, 0x002db796, 0x00399998,
};

const uint32_t rk3576_vepu510_lambda_cvr_60[60] = {
  0x00000009, 0x0000000b, 0x0000000e, 0x00000011, 0x00000016, 0x0000001b,
  0x00000022, 0x0000002b, 0x00000036, 0x00000045, 0x00000056, 0x0000006d,
  0x00000089, 0x000000ad, 0x000000da, 0x00000112, 0x00000159, 0x000001b3,
  0x00000224, 0x000002b3, 0x00000366, 0x00000449, 0x00000566, 0x000006cd,
  0x00000891, 0x00000acb, 0x00000d9a, 0x000013c1, 0x000018e4, 0x00001f5c,
  0x00002783, 0x000031c8, 0x00003eb8, 0x00004f06, 0x00006390, 0x00008e14,
  0x0000b302, 0x0000e18a, 0x00011c29, 0x00016605, 0x0001c313, 0x00027ae1,
  0x00031fe6, 0x0003efcf, 0x0004f5c3, 0x0006e785, 0x0008b2ef, 0x000af5c3,
  0x000f1e7a, 0x00130c7f, 0x00180000, 0x001e3cf4, 0x002618fe, 0x00300000,
  0x003c79e8, 0x004c31fc, 0x00600000, 0x0078f3d0, 0x009863f8, 0x0c000000,
};

/* Note on the last entry of the lambda tables above: every other entry follows
 * a curve that roughly doubles every four steps, which would put entry 59 near
 * 0x00c00000.  MPP actually has 0x0c000000 -- an extra zero, a factor of
 * sixteen out.  It is reproduced verbatim, because this driver's contract is
 * to program what MPP programs, and "fixing" it here would make our register
 * image disagree with the reference the whole module is verified against.
 *
 * It is reachable -- at lambda index 8 the copy takes entries 8 through 59 --
 * but only if a caller selects that index, and the H.264 default is 6.  It is
 * recorded here so that a future reader who does reach it knows the value is
 * deliberate and not a transcription slip of ours.
 */

/* Adaptive-quantisation defaults.  See the header for why the two step
 * tables are kept separate even though they currently hold the same values.
 */

const int32_t rk3576_vepu510_aq_tthd_default[16] = {
  0, 0, 0, 0, 3, 3, 5, 5, 8, 8, 8, 15, 15, 20, 25, 25,
};

const int32_t rk3576_vepu510_aq_step_i_default[16] = {
  -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 8,
};

const int32_t rk3576_vepu510_aq_step_p_default[16] = {
  -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 8,
};
