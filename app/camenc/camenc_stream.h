/****************************************************************************
 * app/camenc/camenc_stream.h
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
 * Turn the encoder's output into a stream something can play.
 *
 * The encoder hands over an H.264 elementary stream in annex-B form: NAL
 * units, each preceded by a start code.  Nothing consumes that directly over
 * a network -- the browser plays fragmented MP4 through Media Source
 * Extensions, which wants NAL units of known length instead, and the
 * parameter sets kept out of band.
 *
 * This module is that conversion, and nothing else.  It has no sockets, no
 * files and no configuration; it is handed access units and calls back with
 * segments.  Where the segments go is the caller's business, which is what
 * makes the same code usable for a file on disk, for one network client, and
 * for several at once.
 *
 * The two differences that matter, and why:
 *
 *   Framing.  Annex-B marks the end of a NAL unit with the next start code,
 *   so a parser cannot skip ahead.  MP4 prefixes each unit with its length.
 *   The bytes of the unit itself do not change: this is a change of framing,
 *   not of content, including the emulation-prevention bytes, which are part
 *   of the unit as it appears in the stream.  Removing them would make a
 *   round trip lossy and would have to be undone on the way back in.
 *
 *   Parameter sets.  The sequence and picture parameter sets are carried
 *   in-band, in front of the first access unit.  MP4 puts them in the
 *   initialisation segment's decoder configuration record instead, so they
 *   are captured from the first access unit and taken out of the samples.
 *   This is why an initialisation segment cannot be produced until the first
 *   access unit has been seen, and why the segments come out in the order
 *   they do.
 *
 * The module is written to be copied.  It depends on nothing but the C
 * library, so the same file builds on a host and on the target.
 *
 ****************************************************************************/

#ifndef __APP_CAMENC_CAMENC_STREAM_H
#define __APP_CAMENC_CAMENC_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Room for the parameter sets, start codes stripped.  They are tens of bytes
 * in every configuration this encoder offers; the bound exists so that the
 * structure has one, not because anything is expected to approach it.
 */

#define CAMENC_SPS_MAX 128
#define CAMENC_PPS_MAX 64

/* What the module is handing over.
 *
 * INIT is the initialisation segment, sent once before anything else and
 * again to any client that arrives later.  FRAGMENT is one access unit.
 */

enum camenc_seg_e
{
  CAMENC_SEG_INIT = 0,
  CAMENC_SEG_FRAGMENT = 1,
};

/* Called once per segment.
 *
 * `key` is true for a fragment that a stream may be started on -- one whose
 * pictures do not refer to anything before it.  A client that arrives
 * mid-stream needs exactly two things, and this is the second of them: the
 * initialisation segment, which carries the parameter sets, and a fragment
 * whose pictures refer to nothing before them.  Without the second its
 * decoder starts with references it never received.
 *
 * Which key fragment a caller uses is the caller's business.  Keeping the
 * one most recently seen, and the fragments since, gives a client a stream
 * it can start on immediately; waiting for the next one costs that client up
 * to a group of pictures of blank picture, and is what this tree does --
 * camenc_ws.c has the reason, which is that the immediate version needs more
 * room in a client's transmit buffer than the live frames leave.
 *
 * `data` is only valid for the duration of the call.
 *
 * Return 0 for success, or a negative value to abandon the write.
 */

typedef int (*camenc_emit_fn)(void *arg, enum camenc_seg_e seg,
                              const uint8_t *data, size_t len, bool key);

struct camenc_stream_s
{
  camenc_emit_fn emit;
  void *arg;

  uint32_t width;
  uint32_t height;

  /* Parameter sets, as they appeared in the stream's first access unit. */

  uint8_t sps[CAMENC_SPS_MAX];
  uint8_t pps[CAMENC_PPS_MAX];
  size_t sps_len;
  size_t pps_len;

  bool params_known; /* the initialisation segment can be built */
  bool init_sent;    /* and has been handed to the caller         */

  uint32_t seq;      /* fragment sequence number                  */
  uint64_t last_pts; /* previous access unit's presentation time  */
  bool have_last_pts;

  /* The caller's scratch space, used to build one fragment's boxes.  Kept
   * out of this structure so its size does not live on the stack, and kept
   * the caller's so that where it goes is visible.
   */

  uint8_t *scratch;
  size_t scratch_size;
  uint32_t max_au; /* the largest access unit the caller will pass */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Prepare to mux.  `scratch` must hold one fragment: the boxes, the mdat
 * header and the access unit itself, with its start codes widened to lengths.
 * `CAMENC_STREAM_SLACK` over the largest access unit is enough.
 */

#define CAMENC_STREAM_SLACK 4096

int camenc_stream_init(struct camenc_stream_s *st, uint32_t width,
                       uint32_t height, uint32_t max_au, uint8_t *scratch,
                       size_t scratch_size, camenc_emit_fn emit, void *arg);

/* Mux one access unit, in annex-B form, as the encoder produced it.
 *
 * `pts_us` is the presentation time the caller measured, in microseconds.
 * It is written as the fragment's decode time, so the timeline the player
 * reconstructs is the one the frames actually arrived on rather than a
 * nominal frame rate; a camera that stutters then produces a stream that
 * stutters in the same places instead of one that runs at the wrong speed.
 *
 * The first access unit is expected to carry the parameter sets.  Ones that
 * do not are still muxed, and the parameters learned so far are used.
 *
 * Return 0, or a negative errno.  -ENOSPC means the access unit or its
 * fragment did not fit the scratch space, and nothing was emitted.
 */

int camenc_stream_write(struct camenc_stream_s *st, const uint8_t *au,
                        size_t len, uint64_t pts_us, bool key);

/* Hand the initialisation segment to the caller again, for a client that has
 * just arrived.  Fails if no parameter sets have been seen yet.
 */

int camenc_stream_repeat_init(struct camenc_stream_s *st);
/* The stream's codec, as an MP4 codec string such as "avc1.42c016".
 *
 * A player has to be told this before it is fed anything, and it has to be
 * the stream's own profile, compatibility flags and level -- a browser will
 * refuse a MediaSource whose codec string does not match the data, and a
 * string that is merely plausible is the kind of mismatch that fails on
 * someone else's machine rather than here.  The three bytes are read out of
 * the sequence parameter set, which is why this cannot answer until one has
 * been seen.
 *
 * Return the length written, or 0 if the parameter sets are not known yet.
 */

size_t camenc_stream_codec_string(FAR const struct camenc_stream_s *st,
                                  FAR char *buf, size_t len);
#endif /* __APP_CAMENC_CAMENC_STREAM_H */
