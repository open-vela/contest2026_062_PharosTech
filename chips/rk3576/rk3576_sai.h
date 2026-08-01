/****************************************************************************
 * chips/rk3576/rk3576_sai.h
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

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_SAI_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_SAI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/audio/i2s.h>

#ifdef CONFIG_RK3576_SAI

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_sai_initialize
 *
 * Description:
 *   Initialize a SAI controller and return its lower-half I2S interface for
 *   the board to bind to a codec via audio_i2s_initialize().  The board is
 *   responsible for muxing the SAI's MCLK/SCLK/LRCK/SDO/SDI pins before the
 *   returned device is used.
 *
 *   Any of the ten SAI0..SAI9 controllers may be selected.  Note that some
 *   controllers are inherently single-direction in hardware (SAI5 is RX-only
 *   and SAI7/SAI8/SAI9 are TX-only); the returned device rejects the
 *   unsupported direction with -ENODEV.
 *
 * Input Parameters:
 *   busno - SAI controller index (0..9).
 *
 * Returned Value:
 *   A non-NULL i2s_dev_s on success; NULL on failure.
 *
 ****************************************************************************/

struct i2s_dev_s *rk3576_sai_initialize(int busno);

#endif /* CONFIG_RK3576_SAI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_SAI_H */
