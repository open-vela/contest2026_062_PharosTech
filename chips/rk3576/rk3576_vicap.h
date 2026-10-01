/****************************************************************************
 * chips/rk3576/rk3576_vicap.h
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
 * RK3576 VICAP capture driver -- public interface.
 *
 * Implements the NuttX capture framework's SoC side (struct imgdata_s) for
 * the RK3576 VICAP block, fed by a CSI HOST:
 *
 *   D-PHY RX  ->  CSI HOST  ->  VICAP  ->  DDR  ->  CPU debayer  ->  NV12
 *
 * The sensor end is a separate driver implementing struct imgsensor_s; the
 * framework binds one available sensor per capture device.
 *
 * Why the CPU does the demosaicing: the capture framework's pixel format
 * table has no RAW entry at all (see include/nuttx/video/imgdata.h), and an
 * unrecognised V4L2 fourcc is silently mapped to JPEG_WITH_SUBIMG with a
 * default 2-byte-per-pixel buffer size -- a trap for a RAW path but an exact
 * match for nothing.  Rather than extend the shared tables, this driver
 * keeps the RAW data private: VICAP DMAs RAW into its own ping-pong buffers,
 * the CPU demosaics into the buffer the framework handed over, and the
 * framework only ever sees NV12.
 *
 * The RK3576's hardware ISP would do the demosaicing for free, but it is a
 * different block, it has no register documentation in the SoC TRM, and it
 * shares the VI power domain and clock tree with VICAP.  Leaving it out
 * keeps this driver self-contained.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_VICAP_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_VICAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/video/imgdata.h>

#ifdef CONFIG_RK3576_VICAP

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Which VICAP MIPI input to capture from.  The value is the VICAP input
 * index, not the connector number: VICAP's MIPIn is hard-wired to CSIHOSTn,
 * and it is the board that knows which connector reaches which host.
 */

enum rk3576_vicap_input_e
{
  RK3576_VICAP_INPUT_MIPI0 = 0,
  RK3576_VICAP_INPUT_MIPI1 = 1,
  RK3576_VICAP_INPUT_MIPI2 = 2,
  RK3576_VICAP_INPUT_MIPI3 = 3,
  RK3576_VICAP_INPUT_MIPI4 = 4,
};

/* Raw Bayer order, i.e. the colour of the first two pixels of the first
 * line.  This is what the demosaicer needs; it is not a CSI-2 concept.
 */

enum rk3576_vicap_bayer_e
{
  RK3576_VICAP_BAYER_BGGR = 0, /* SBGGR: row0 = B G.., row1 = G R.. */
  RK3576_VICAP_BAYER_GBRG = 1, /* row0 = G B.., row1 = R G.. */
  RK3576_VICAP_BAYER_GRBG = 2, /* row0 = G R.., row1 = B G.. */
  RK3576_VICAP_BAYER_RGGB = 3, /* row0 = R G.., row1 = G B.. */
};

struct rk3576_vicap_config
{
  uint8_t input;    /* enum rk3576_vicap_input_e */
  uint8_t id;       /* ID slot within the input, 0..3 (0 is the usual one) */
  uint8_t bayer;    /* enum rk3576_vicap_bayer_e */
  uint8_t raw_bits; /* RAW sample width: 8, 10, 12 or 14 */

  uint16_t width;  /* Active width in pixels; must be 4-pixel aligned */
  uint16_t height; /* Active height in lines */

  uint8_t vc; /* CSI-2 virtual channel to accept */
  uint8_t dt; /* CSI-2 data type to accept (0x2b = RAW10) */

  /* true: raw samples are written uncompacted, one sample per 16-bit DDR
   * word, positioned by `align_high`.  false: compacted as the MIPI
   * packing has them.
   *
   * The uncompact path is the one this driver wants: it makes both the
   * stride arithmetic and the demosaicer's loads trivially simple, at the
   * cost of 2 bytes per pixel instead of 1.25.
   */

  bool uncompact;

  /* Only meaningful when `uncompact`.  true puts a 10-bit sample in
   * [15:6] and a 12-bit one in [15:4]; false puts them in [9:0] / [11:0].
   * The high position lets a plain 16-bit load be shifted down to 8 bits
   * with a single >> 8.
   */

  bool align_high;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vicap_initialize
 *
 * Description:
 *   Bring up VICAP for MIPI RAW capture and register the capture device.
 *
 *   On success the caller receives the imgdata handle to pass to
 *   capture_register() together with its image sensor:
 *
 *     rk3576_vicap_initialize(&cfg, &data);
 *     capture_register("/dev/video0", data, sensors, nsensors);
 *
 *   Exactly one instance may exist, because there is one VICAP and this
 *   driver drives its single MIPI path.
 *
 * Input Parameters:
 *   config - Capture geometry, Bayer order and VC/DT filter.
 *   data   - Receives the imgdata handle on success.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vicap_initialize(FAR const struct rk3576_vicap_config *config,
                            FAR struct imgdata_s **data);

/****************************************************************************
 * Name: rk3576_vicap_uninitialize
 *
 * Description:
 *   Stop capture, release the frame buffers and disable the block.  Safe to
 *   call when not initialised.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vicap_uninitialize(void);

/****************************************************************************
 * Name: rk3576_vicap_dump
 *
 * Description:
 *   Log the capture state and the per-frame diagnostics: whether any frame
 *   has been seen, the frame-start/end counters, the measured line and pixel
 *   counts VICAP saw, and the MMU status.
 *
 *   The line/pixel counters are the fastest way to tell "the sensor is
 *   sending the wrong geometry" from "the sensor is not sending anything at
 *   all", which are otherwise indistinguishable from a black frame.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_vicap_dump(void);

#endif /* CONFIG_RK3576_VICAP */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_VICAP_H */
