/****************************************************************************
 * chips/rk3576/vepu/vepu510_tables.h
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
 * Data, separated from the logic that uses it.
 *
 * Every table here is transcribed from Rockchip MPP
 * (mpp/hal/rkenc/common/vepu51x_common.c, Apache-2.0) and is compared
 * element by element against MPP's own definition on the host.  That
 * comparison is the point of keeping them in one file:
 *
 *   - Transcribing is unavoidable.  Linking MPP's object would make the
 *     firmware depend on a file outside its own tree, and the dependency
 *     would be invisible: these are data referenced by code that the linker
 *     pulls in only once something calls it, so a missing symbol can stay
 *     unreported until a later change reaches it.
 *
 *   - Transcription is also exactly how a digit gets mistyped, and a wrong
 *     weight or lambda does not fail -- it quietly costs quality.  So the
 *     values are never trusted by inspection.  The harness compiles MPP's
 *     definition and diffs ours against it, which turns a typo into a
 *     failed verification instead of a picture that is subtly worse.
 *
 * Nothing else may be added here.  A table that is not directly comparable
 * against an upstream definition belongs with the code that uses it, because
 * the comparison above is the only reason this file exists.
 *
 * ---------------------------------------------------------------------------
 * Transcribed from Rockchip MPP (Rockchip Media Process Platform),
 * https://github.com/rockchip-linux/mpp, branch develop, commit
 * 14729dd578e570e5f00fd1dd2113f5429012d64b, Apache-2.0:
 *
 *   mpp/hal/rkenc/common/vepu51x_common.c
 *
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 *
 * This is a modified derivative: the tables are reproduced here, renamed to
 * this driver's namespace and reduced to the entries the H.264 fixed-QP path
 * uses.
 * ---------------------------------------------------------------------------
 *
 ****************************************************************************/

#ifndef __CHIPS_RK3576_VEPU_VEPU510_TABLES_H
#define __CHIPS_RK3576_VEPU_VEPU510_TABLES_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of 32-bit entries the encoder copies out of a lambda table, the
 * index width of the tables, and the largest index the copy can start at.
 *
 * The tables carry 60 entries but only 52 are ever copied, so the index must
 * not exceed 8: 8 + 52 is exactly 60.  MPP enforces the same bound, which is
 * what keeps the copy from reading past the end of the table.
 */

#define RK3576_VEPU510_LAMBDA_TAB_LEN  60
#define RK3576_VEPU510_LAMBDA_COPY_LEN 52
#define RK3576_VEPU510_LAMBDA_IDX_MAX  8

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Anti-flicker thresholds and weights, indexed [threshold level][strength],
 * with strength 0..3.
 */

extern const uint8_t rk3576_vepu510_pskip_atf_thd[2][4];
extern const uint8_t rk3576_vepu510_pskip_atf_wgt[2][4];
extern const uint8_t rk3576_vepu510_intra_atf_thd[3][4];
extern const uint8_t rk3576_vepu510_intra_atf_wgt[3][4];

/* RDO lambda tables.  MPP selects between them on scene mode, and the names
 * are the wrong way round from what they suggest: "default" is used *in* IPC
 * mode and "cvr" outside it.  They are genuinely different curves rather than
 * one being a strength of the other, so getting the mapping backwards is not
 * a scaling error -- it substitutes a different rate-distortion trade-off.
 */

extern const uint32_t rk3576_vepu510_lambda_default_60[60];
extern const uint32_t rk3576_vepu510_lambda_cvr_60[60];

/* Adaptive-quantisation defaults: one threshold table and a step table per
 * slice type, each 16 entries.
 *
 * MPP keeps these as defaults for a settable hardware configuration and
 * copies them in at context init.  This driver does not expose the settable
 * form -- there is no V4L2 control for it -- so the defaults are what it
 * always programs, and they are kept here so that they can be diffed against
 * MPP's own copies.
 *
 * The two step tables are currently identical in MPP.  They are kept as two
 * tables rather than one because that is how the upstream code is arranged,
 * and because a future MPP revision changing one of them should show up as a
 * table diff rather than as a silent behaviour change.
 */

extern const int32_t rk3576_vepu510_aq_tthd_default[16];
extern const int32_t rk3576_vepu510_aq_step_i_default[16];
extern const int32_t rk3576_vepu510_aq_step_p_default[16];

#endif /* __CHIPS_RK3576_VEPU_VEPU510_TABLES_H */
