/****************************************************************************
 * app/nyabula_core/ny_sbc.c
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
 * Included Files
 ****************************************************************************/

#include "ny_sbc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <oi_codec_sbc.h>
#include <sbc_encoder.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_MSBC_SYNCWORD 0xad
#define NY_MSBC_H2_SYNC  0x01
#define NY_MSBC_BITPOOL  26
#define NY_MSBC_BLOCKS   15
#define NY_MSBC_SUBBANDS 8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_sbc_decoder_s
{
  OI_CODEC_SBC_DECODER_CONTEXT context;
  uint32_t data[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
  bool msbc;
};

struct ny_msbc_encoder_s
{
  SBC_ENC_PARAMS params;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The H2 header codes its two bit sequence number with each bit doubled. */

static const uint8_t g_msbc_h2_sequence[4] =
{
  0x08, 0x38, 0xc8, 0xf8
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_sbc_decoder_create
 ****************************************************************************/

struct ny_sbc_decoder_s *ny_sbc_decoder_create(bool msbc)
{
  struct ny_sbc_decoder_s *decoder = calloc(1, sizeof(*decoder));

  if (decoder == NULL)
    {
      return NULL;
    }

  decoder->msbc = msbc;
  if (ny_sbc_decoder_reset(decoder) < 0)
    {
      free(decoder);
      return NULL;
    }

  return decoder;
}

/****************************************************************************
 * Name: ny_sbc_decoder_destroy
 ****************************************************************************/

void ny_sbc_decoder_destroy(struct ny_sbc_decoder_s *decoder)
{
  free(decoder);
}

/****************************************************************************
 * Name: ny_sbc_decoder_reset
 ****************************************************************************/

int ny_sbc_decoder_reset(struct ny_sbc_decoder_s *decoder)
{
  OI_STATUS status;

  if (decoder == NULL)
    {
      return -EINVAL;
    }

  /* A2DP output is always two interleaved channels: with a stride of two
   * the library copies a mono stream to both.
   */

  status = OI_CODEC_SBC_DecoderReset(&decoder->context, decoder->data,
                                     sizeof(decoder->data),
                                     decoder->msbc ? 1 : 2,
                                     decoder->msbc ? 1 : 2, FALSE);
  if (status == OI_OK && decoder->msbc)
    {
      status = OI_CODEC_SBC_DecoderConfigureMSbc(&decoder->context);
    }

  return status == OI_OK ? 0 : -EINVAL;
}

/****************************************************************************
 * Name: ny_sbc_decode
 ****************************************************************************/

int ny_sbc_decode(struct ny_sbc_decoder_s *decoder, const uint8_t *data,
                  size_t size, size_t *used, int16_t *pcm, size_t capacity,
                  struct ny_sbc_format_s *format)
{
  const OI_BYTE *cursor = data;
  uint32_t remaining = (uint32_t)size;
  uint32_t bytes = (uint32_t)(capacity * sizeof(int16_t));
  unsigned int stride;
  OI_STATUS status;

  if (used != NULL)
    {
      *used = 0;
    }

  if (decoder == NULL || data == NULL || used == NULL || pcm == NULL)
    {
      return -EINVAL;
    }

  stride = decoder->msbc ? 1 : 2;
  if (capacity < NY_SBC_FRAME_SAMPLES_MAX * stride)
    {
      return -ENOSPC;
    }

  status = OI_CODEC_SBC_DecodeFrame(&decoder->context, &cursor, &remaining,
                                    pcm, &bytes);
  if (status == OI_CODEC_SBC_NOT_ENOUGH_HEADER_DATA ||
      status == OI_CODEC_SBC_NOT_ENOUGH_BODY_DATA)
    {
      return -EAGAIN;
    }

  if (status != OI_OK)
    {
      /* Step over the damaged frame when the header at the start of data
       * was read and says how long the frame is (the library's own skip
       * refuses a frame whose CRC fails, which is the case that matters);
       * give up on the rest of the input otherwise, so that the caller can
       * never spin on the same bytes.
       */

      size_t length = size;

      if (decoder->msbc)
        {
          length = NY_MSBC_FRAME_BYTES;
        }
      else if (status == OI_CODEC_SBC_CHECKSUM_MISMATCH &&
               data[0] == OI_SBC_SYNCWORD)
        {
          OI_CODEC_SBC_FRAME_INFO *info = &decoder->context.common.frameInfo;

          length = OI_CODEC_SBC_CalculateFramelen(info);
        }

      *used = length == 0 || length > size ? size : length;
      return -EBADMSG;
    }

  *used = (size_t)(cursor - data);
  if (format != NULL)
    {
      const OI_CODEC_SBC_FRAME_INFO *info = &decoder->context.common.frameInfo;

      format->rate = info->frequency;
      format->channels = info->nrof_channels;
      format->blocks = info->nrof_blocks;
      format->subbands = info->nrof_subbands;
      format->bitpool = info->bitpool;
      format->samples = (uint16_t)(bytes / sizeof(int16_t) / stride);
    }

  return (int)(bytes / sizeof(int16_t) / stride);
}

/****************************************************************************
 * Name: ny_msbc_encoder_create
 ****************************************************************************/

struct ny_msbc_encoder_s *ny_msbc_encoder_create(void)
{
  struct ny_msbc_encoder_s *encoder = calloc(1, sizeof(*encoder));

  if (encoder == NULL)
    {
      return NULL;
    }

  encoder->params.s16SamplingFreq = SBC_sf16000;
  encoder->params.s16ChannelMode = SBC_MONO;
  encoder->params.s16NumOfSubBands = NY_MSBC_SUBBANDS;
  encoder->params.s16NumOfChannels = 1;
  encoder->params.s16NumOfBlocks = NY_MSBC_BLOCKS;
  encoder->params.s16AllocationMethod = SBC_LOUDNESS;
  encoder->params.Format = SBC_FORMAT_MSBC;
  SBC_Encoder_Init(&encoder->params);

  /* The library derives a bitpool from a bit rate, which mSBC does not
   * have: the profile fixes the bitpool itself.
   */

  encoder->params.s16BitPool = NY_MSBC_BITPOOL;
  return encoder;
}

/****************************************************************************
 * Name: ny_msbc_encoder_destroy
 ****************************************************************************/

void ny_msbc_encoder_destroy(struct ny_msbc_encoder_s *encoder)
{
  free(encoder);
}

/****************************************************************************
 * Name: ny_msbc_encode
 ****************************************************************************/

int ny_msbc_encode(struct ny_msbc_encoder_s *encoder, const int16_t *pcm,
                   uint8_t *frame)
{
  int16_t input[NY_MSBC_FRAME_SAMPLES];
  uint32_t size;

  if (encoder == NULL || pcm == NULL || frame == NULL)
    {
      return -EINVAL;
    }

  /* The library takes a writable buffer. */

  memcpy(input, pcm, sizeof(input));
  size = SBC_Encode(&encoder->params, input, frame);
  return size == NY_MSBC_FRAME_BYTES ? NY_MSBC_FRAME_BYTES : -EIO;
}

/****************************************************************************
 * Name: ny_msbc_h2_pack
 ****************************************************************************/

void ny_msbc_h2_pack(unsigned int sequence, const uint8_t *frame,
                     uint8_t *out)
{
  out[0] = NY_MSBC_H2_SYNC;
  out[1] = g_msbc_h2_sequence[sequence & 3];
  memcpy(out + 2, frame, NY_MSBC_FRAME_BYTES);
  out[NY_MSBC_H2_BYTES - 1] = 0;
}

/****************************************************************************
 * Name: ny_msbc_deframer_reset
 ****************************************************************************/

void ny_msbc_deframer_reset(struct ny_msbc_deframer_s *deframer)
{
  memset(deframer, 0, sizeof(*deframer));
  deframer->sequence = -1;
}

/****************************************************************************
 * Name: ny_msbc_deframer_push
 ****************************************************************************/

int ny_msbc_deframer_push(struct ny_msbc_deframer_s *deframer,
                          const uint8_t *data, size_t size, size_t *used,
                          uint8_t *frame, unsigned int *lost)
{
  size_t index;

  *used = 0;
  *lost = 0;
  for (index = 0; index < size; index++)
    {
      uint8_t byte = data[index];

      if (deframer->filled == 0)
        {
          if (byte == NY_MSBC_H2_SYNC)
            {
              deframer->frame[deframer->filled++] = byte;
            }
          else if (byte != 0)
            {
              /* Zero is the padding that follows every frame. */

              deframer->skipped++;
            }

          continue;
        }

      if (deframer->filled == 1)
        {
          unsigned int sequence;

          for (sequence = 0; sequence < 4; sequence++)
            {
              if (byte == g_msbc_h2_sequence[sequence])
                {
                  break;
                }
            }

          if (sequence == 4)
            {
              deframer->skipped++;
              deframer->filled = byte == NY_MSBC_H2_SYNC ? 1 : 0;
              continue;
            }

          deframer->frame[deframer->filled++] = byte;
          continue;
        }

      if (deframer->filled == 2 && byte != NY_MSBC_SYNCWORD)
        {
          deframer->skipped += 2;
          deframer->filled = byte == NY_MSBC_H2_SYNC ? 1 : 0;
          continue;
        }

      deframer->frame[deframer->filled++] = byte;
      if (deframer->filled == 2 + NY_MSBC_FRAME_BYTES)
        {
          int sequence = 0;

          while (deframer->frame[1] != g_msbc_h2_sequence[sequence])
            {
              sequence++;
            }

          if (deframer->sequence >= 0)
            {
              *lost = (unsigned int)(sequence - deframer->sequence - 1) & 3;
            }

          deframer->sequence = (int8_t)sequence;
          deframer->lost += *lost;
          deframer->frames++;
          deframer->filled = 0;
          memcpy(frame, deframer->frame + 2, NY_MSBC_FRAME_BYTES);
          *used = index + 1;
          return 1;
        }
    }

  *used = size;
  return 0;
}
