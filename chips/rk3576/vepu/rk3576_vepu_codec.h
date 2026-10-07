/****************************************************************************
 * chips/rk3576/vepu/rk3576_vepu_codec.h
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
 * RK3576 VEPU510 as a V4L2 memory-to-memory video encoder.
 *
 * The layer above the hardware driver: it presents the encoder as a
 * /dev/videoN node that takes NV12 frames on its output side and returns an
 * H.264 elementary stream on its capture side, using the buffer and
 * streaming model the v4l2_m2m framework already implements.
 *
 * What it deliberately is not:
 *
 *   - It is not a decoder.  The hardware can decode, and nothing here does.
 *   - It is not rate-controlled.  The encoder runs at a fixed QP because
 *     that is all the register layer was ported and verified for.  So there
 *     is no bitrate or frame-rate control to set, and the frame rate is
 *     carried for the VUI only.
 *   - It predicts, and the group length is the caller's to choose.  The
 *     hardware reports bframe=0, so a group is an IDR followed by P pictures
 *     that predict from it and from each other; see RK3576_VEPU_CID_GOP and
 *     RK3576_VEPU_DEFAULT_GOP.  A group of one is all-intra, which is what
 *     this driver encoded before it could predict.
 *
 *     What that costs anything passing the stream on is worth stating where
 *     the choice is made: a picture that is not the first of its group
 *     cannot be dropped on its own.  The pictures after it predict from it,
 *     and a decoder that never received it reconstructs them from the wrong
 *     reference -- which shows as a burst of wrong colour rather than as one
 *     missing picture.  Anything that discards pictures has to tell its
 *     client to start again at the next group instead; see the note on
 *     `waiting` in app/camenc/camenc_ws.h.
 ****************************************************************************/

#ifndef __CHIPS_RK3576_VEPU_RK3576_VEPU_CODEC_H
#define __CHIPS_RK3576_VEPU_RK3576_VEPU_CODEC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Driver-specific controls.
 *
 * There is no choice about these being private.  v4l2_m2m.c's
 * struct v4l2_ext_control declares its id as uint16_t, where the standard
 * uses uint32_t, so every V4L2_CID_MPEG_* value -- all of them at
 * 0x009809xx or above -- is unreachable.  The same reason rules out
 * V4L2_CID_PRIVATE_BASE, which videoio.h defines as 0x08000000 and then
 * stores in that same 16-bit field.
 *
 * So the controls are numbered from here, clear of the standard range (the
 * framework's own V4L2_CID_MAX_CTRLS is 1024) and inside the width the
 * struct can actually carry.  An application has to be told these numbers;
 * it cannot enumerate them, because the framework's query_ext_ctrl slot is
 * not wired up.
 */

#define RK3576_VEPU_CID_QP                    \
  0x1000 /* 0..51, the QP every frame is      \
          * encoded at.  Fixed-QP is the only \
          * mode the register layer implements. */
#define RK3576_VEPU_CID_PROFILE                  \
  0x1001 /* 66, 77 or 100.  Lowering it below    \
          * 100 also drops the 8x8 transform and \
          * the second chroma offset, because    \
          * those are what the profile gates. */
#define RK3576_VEPU_CID_LEVEL            \
  0x1002 /* 0 lets the writer choose the \
          * smallest level the geometry fits. */
#define RK3576_VEPU_CID_DEBLOCK          \
  0x1003 /* 1 to carry deblocking filter \
          * control in the PPS. */
#define RK3576_VEPU_CID_GOP                              \
  0x1004 /* Pictures per group of pictures, 1..1000.     \
          * 1 makes every picture an IDR, which is what  \
          * the driver did before it could predict.  A   \
          * larger value makes the first picture of each \
          * group an IDR and the rest P pictures, which  \
          * is what makes the stream's bit rate depend   \
          * on the scene instead of only on the size of  \
          * the pictures. */

/* Frame rates are not controls: V4L2 already has a place for them, and
 * VIDIOC_S_PARM is where an application expects to set one.  The value
 * reaches the bitstream as the VUI timing block, so it tells a player how to
 * present the stream.  It does not govern how fast the encoder runs, because
 * nothing here is rate-controlled.
 */

#define RK3576_VEPU_DEFAULT_QP      26
#define RK3576_VEPU_DEFAULT_PROFILE 66 /* baseline */
#define RK3576_VEPU_DEFAULT_LEVEL   0

/* Two pictures per group: one IDR and one P that predicts from it.
 *
 * This is the shortest group that predicts at all, and it is the driver's
 * default because the driver does not know how its stream is consumed.  A
 * group is also how long a decoder that arrived late has to wait before it can
 * start, so its length is a property of how a stream is watched rather than of
 * the encoding, and the caller is the one that can answer it.  The streaming
 * application asks for fifteen, which is half a second at its frame rate.
 */

#define RK3576_VEPU_DEFAULT_GOP 2

/* The longest group a caller may ask for.  Not a hardware limit -- the limits
 * that matter are the widths of frame_num and pic_order_cnt_lsb in the slice
 * header, and those are far larger.  It is a limit on how long a stream can
 * go before a decoder that joined late has nothing to start from.
 */

#define RK3576_VEPU_GOP_MAX 1000

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vepu_codec_register
 *
 * Description:
 *   Register the encoder as a V4L2 M2M device.
 *
 *   The hardware must already be prepared with rk3576_vepu_initialize():
 *   this layer assumes the block is powered and its interrupt is attached.
 *
 * Input Parameters:
 *   devpath - node to create, e.g. "/dev/video0"
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_vepu_codec_register(FAR const char *devpath);

/****************************************************************************
 * Name: rk3576_vepu_codec_unregister
 *
 * Description:
 *   Remove the device node.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_vepu_codec_unregister(FAR const char *devpath);

#ifdef CONFIG_RK3576_VEPU_CODEC_SELFTEST

/****************************************************************************
 * Name: rk3576_vepu_codec_selftest
 *
 * Description:
 *   Acceptance test for the V4L2 layer: run the whole buffer and streaming
 *   sequence an application would, over the device node, and check what
 *   comes back.
 *
 *   This is the counterpart of rk3576_vepu_selftest(), one layer up.  That
 *   one proves the hardware encodes a frame; this one proves an application
 *   can get a frame to it and a bitstream back, which is a different claim
 *   -- the format negotiation, the buffer queues and the streaming state
 *   machine all sit between the two.
 *
 *   It uses MMAP, because that is the only memory the device accepts.
 *
 * Returned Value:
 *   OK if every frame produced a well-formed annex-B stream; a negated errno
 *   otherwise.  What it found is logged either way.
 *
 ****************************************************************************/

int rk3576_vepu_codec_selftest(FAR const char *devpath);

#endif /* CONFIG_RK3576_VEPU_CODEC_SELFTEST */

#endif /* __CHIPS_RK3576_VEPU_RK3576_VEPU_CODEC_H */
