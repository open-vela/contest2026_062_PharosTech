/****************************************************************************
 * app/nyabula_core/ny_sbc.h
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

#ifndef __NYABULA_CORE_NY_SBC_H
#define __NYABULA_CORE_NY_SBC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The only door between the Bluetooth audio paths and the SBC library, so
 * the library behind it (external/libfluoride-sbc today) can be swapped
 * without touching a caller.  Nothing here depends on NuttX: the same file
 * builds for the host unit test.
 */

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The largest SBC frame: 16 blocks of 8 subbands. */

#define NY_SBC_FRAME_SAMPLES_MAX 128

/* mSBC (HFP wide band speech): 16 kHz mono, 15 blocks, 8 subbands, loudness,
 * bitpool 26.  One frame is 120 samples = 7.5 ms and encodes to 57 bytes.
 */

#define NY_MSBC_RATE             16000
#define NY_MSBC_FRAME_SAMPLES    120
#define NY_MSBC_FRAME_BYTES      57

/* What one frame occupies in a SCO stream: a two byte H2 header, the mSBC
 * frame and one byte of padding.
 */

#define NY_MSBC_H2_BYTES         60

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_sbc_format_s
{
  uint32_t rate;     /* Samples per second */
  uint8_t  channels; /* Channels in the encoded stream: 1 or 2 */
  uint8_t  blocks;
  uint8_t  subbands;
  uint8_t  bitpool;
  uint16_t samples;  /* Samples per channel in the frame just decoded */
};

struct ny_sbc_decoder_s; /* Opaque */
struct ny_msbc_encoder_s; /* Opaque */

/* Reassembles mSBC frames from SCO payloads of any size, and tells which
 * frames the link lost from the H2 sequence numbers.
 */

struct ny_msbc_deframer_s
{
  uint8_t  frame[NY_MSBC_H2_BYTES];
  uint8_t  filled;
  int8_t   sequence; /* Last sequence number seen: 0..3, or -1 */
  uint32_t frames;   /* Frames handed out */
  uint32_t lost;     /* Frames the sequence numbers say are missing */
  uint32_t skipped;  /* Bytes thrown away while looking for a header */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ny_sbc_decoder_create
 *
 * Description:
 *   A decoder for A2DP SBC (msbc false: any SBC stream, always decoded to
 *   two interleaved channels, a mono stream being copied to both) or for
 *   HFP mSBC (msbc true: one channel).
 *
 * Returned Value:
 *   The decoder, or NULL when out of memory.
 *
 ****************************************************************************/

struct ny_sbc_decoder_s *ny_sbc_decoder_create(bool msbc);
void ny_sbc_decoder_destroy(struct ny_sbc_decoder_s *decoder);

/****************************************************************************
 * Name: ny_sbc_decoder_reset
 *
 * Description:
 *   Forget the filter history.  Needed when the stream changes or restarts.
 *
 ****************************************************************************/

int ny_sbc_decoder_reset(struct ny_sbc_decoder_s *decoder);

/****************************************************************************
 * Name: ny_sbc_decode
 *
 * Description:
 *   Decode one frame from the start of data.
 *
 * Input Parameters:
 *   data, size - Encoded bytes; may hold more than one frame.
 *   used       - Bytes consumed.  Set on failure too, so that a caller
 *                walking a packet always makes progress: a frame that is
 *                damaged is skipped whole when its length is known.
 *   pcm        - Receives interleaved 16 bit samples.
 *   capacity   - Room in pcm, in int16_t units.  Two channels of
 *                NY_SBC_FRAME_SAMPLES_MAX always fit any frame.
 *   format     - Optional.  Receives what the frame header said.
 *
 * Returned Value:
 *   Samples per channel written (the output holds twice as many values for
 *   an A2DP decoder), -EAGAIN when data ends inside the frame, -EBADMSG for
 *   a frame that cannot be decoded, -ENOSPC when pcm is too small.
 *
 ****************************************************************************/

int ny_sbc_decode(struct ny_sbc_decoder_s *decoder, const uint8_t *data,
                  size_t size, size_t *used, int16_t *pcm, size_t capacity,
                  struct ny_sbc_format_s *format);

/****************************************************************************
 * Name: ny_msbc_encoder_create / ny_msbc_encode
 *
 * Description:
 *   Encode NY_MSBC_FRAME_SAMPLES mono samples into NY_MSBC_FRAME_BYTES.
 *
 * Returned Value:
 *   ny_msbc_encode returns NY_MSBC_FRAME_BYTES or a negated errno value.
 *
 ****************************************************************************/

struct ny_msbc_encoder_s *ny_msbc_encoder_create(void);
void ny_msbc_encoder_destroy(struct ny_msbc_encoder_s *encoder);
int ny_msbc_encode(struct ny_msbc_encoder_s *encoder, const int16_t *pcm,
                   uint8_t *frame);

/****************************************************************************
 * Name: ny_msbc_h2_pack
 *
 * Description:
 *   Wrap one mSBC frame for a SCO link: H2 header carrying the two bit
 *   sequence number, the frame, one padding byte.
 *
 * Input Parameters:
 *   sequence - Frame counter; only its two low bits are sent.
 *   frame    - NY_MSBC_FRAME_BYTES of mSBC.
 *   out      - Receives NY_MSBC_H2_BYTES.
 *
 ****************************************************************************/

void ny_msbc_h2_pack(unsigned int sequence, const uint8_t *frame,
                     uint8_t *out);

/****************************************************************************
 * Name: ny_msbc_deframer_reset / ny_msbc_deframer_push
 *
 * Description:
 *   Feed SCO payload bytes and collect whole mSBC frames.  Push consumes
 *   input up to and including the byte that completes a frame, so it is
 *   called in a loop until it has used everything.
 *
 * Input Parameters:
 *   data, size - SCO payload.
 *   used       - Bytes consumed by this call.
 *   frame      - Receives NY_MSBC_FRAME_BYTES when a frame completes.
 *   lost       - Frames missing before this one, from the sequence numbers
 *                (0..3); the caller conceals that many.
 *
 * Returned Value:
 *   1 when frame was filled, 0 when more input is needed.
 *
 ****************************************************************************/

void ny_msbc_deframer_reset(struct ny_msbc_deframer_s *deframer);
int ny_msbc_deframer_push(struct ny_msbc_deframer_s *deframer,
                          const uint8_t *data, size_t size, size_t *used,
                          uint8_t *frame, unsigned int *lost);

#endif /* __NYABULA_CORE_NY_SBC_H */
