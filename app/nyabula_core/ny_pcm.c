/****************************************************************************
 * app/nyabula_core/ny_pcm.c
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

#include "ny_pcm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void ny_pcm_ring_copy_out(const struct ny_pcm_ring_s *ring,
                                 size_t offset, void *data, size_t size);
static void ny_pcm_put_le32(uint8_t *bytes, uint32_t value);
static uint32_t ny_pcm_isqrt(uint64_t value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_pcm_ring_copy_out
 ****************************************************************************/

static void ny_pcm_ring_copy_out(const struct ny_pcm_ring_s *ring,
                                 size_t offset, void *data, size_t size)
{
  size_t start = (ring->head + offset) % ring->capacity;
  size_t first = ring->capacity - start;

  if (first > size)
    {
      first = size;
    }

  memcpy(data, ring->data + start, first);
  memcpy((uint8_t *)data + first, ring->data, size - first);
}

/****************************************************************************
 * Name: ny_pcm_put_le32
 ****************************************************************************/

static void ny_pcm_put_le32(uint8_t *bytes, uint32_t value)
{
  bytes[0] = value & 0xff;
  bytes[1] = (value >> 8) & 0xff;
  bytes[2] = (value >> 16) & 0xff;
  bytes[3] = (value >> 24) & 0xff;
}

/****************************************************************************
 * Name: ny_pcm_isqrt
 ****************************************************************************/

static uint32_t ny_pcm_isqrt(uint64_t value)
{
  uint64_t result = 0;
  uint64_t bit = (uint64_t)1 << 62;

  while (bit > value)
    {
      bit >>= 2;
    }

  while (bit != 0)
    {
      if (value >= result + bit)
        {
          value -= result + bit;
          result = (result >> 1) + bit;
        }
      else
        {
          result >>= 1;
        }

      bit >>= 2;
    }

  return (uint32_t)result;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_pcm_ring_init
 ****************************************************************************/

int ny_pcm_ring_init(struct ny_pcm_ring_s *ring, size_t capacity)
{
  memset(ring, 0, sizeof(*ring));
  if (capacity == 0)
    {
      return -EINVAL;
    }

  ring->data = malloc(capacity);
  if (ring->data == NULL)
    {
      return -ENOMEM;
    }

  ring->capacity = capacity;
  return 0;
}

/****************************************************************************
 * Name: ny_pcm_ring_free
 ****************************************************************************/

void ny_pcm_ring_free(struct ny_pcm_ring_s *ring)
{
  free(ring->data);
  memset(ring, 0, sizeof(*ring));
}

/****************************************************************************
 * Name: ny_pcm_ring_clear
 ****************************************************************************/

void ny_pcm_ring_clear(struct ny_pcm_ring_s *ring)
{
  ring->head = 0;
  ring->used = 0;
}

/****************************************************************************
 * Name: ny_pcm_ring_space
 ****************************************************************************/

size_t ny_pcm_ring_space(const struct ny_pcm_ring_s *ring)
{
  return ring->capacity - ring->used;
}

/****************************************************************************
 * Name: ny_pcm_ring_write
 ****************************************************************************/

bool ny_pcm_ring_write(struct ny_pcm_ring_s *ring, const void *data,
                       size_t size)
{
  size_t start;
  size_t first;

  if (ring->data == NULL || size > ring->capacity - ring->used)
    {
      return false;
    }

  start = (ring->head + ring->used) % ring->capacity;
  first = ring->capacity - start;
  if (first > size)
    {
      first = size;
    }

  memcpy(ring->data + start, data, first);
  memcpy(ring->data, (const uint8_t *)data + first, size - first);
  ring->used += size;
  return true;
}

/****************************************************************************
 * Name: ny_pcm_ring_read
 ****************************************************************************/

size_t ny_pcm_ring_read(struct ny_pcm_ring_s *ring, void *data, size_t size)
{
  if (ring->data == NULL)
    {
      return 0;
    }

  if (size > ring->used)
    {
      size = ring->used;
    }

  if (data != NULL)
    {
      ny_pcm_ring_copy_out(ring, 0, data, size);
    }

  ring->head = (ring->head + size) % ring->capacity;
  ring->used -= size;
  return size;
}

/****************************************************************************
 * Name: ny_pcm_ring_put
 ****************************************************************************/

bool ny_pcm_ring_put(struct ny_pcm_ring_s *ring, const void *data,
                     uint16_t size)
{
  uint8_t length[2];

  if (ring->data == NULL ||
      (size_t)size + sizeof(length) > ring->capacity - ring->used)
    {
      return false;
    }

  length[0] = size & 0xff;
  length[1] = size >> 8;
  ny_pcm_ring_write(ring, length, sizeof(length));
  ny_pcm_ring_write(ring, data, size);
  return true;
}

/****************************************************************************
 * Name: ny_pcm_ring_get
 ****************************************************************************/

int ny_pcm_ring_get(struct ny_pcm_ring_s *ring, void *data, size_t capacity)
{
  uint8_t length[2];
  size_t size;

  if (ring->data == NULL || ring->used < sizeof(length))
    {
      return 0;
    }

  ny_pcm_ring_copy_out(ring, 0, length, sizeof(length));
  size = (size_t)length[0] | (size_t)length[1] << 8;
  if (size + sizeof(length) > ring->used)
    {
      /* Cannot happen with put as the only writer; never trust it. */

      ny_pcm_ring_clear(ring);
      return -EMSGSIZE;
    }

  ny_pcm_ring_read(ring, NULL, sizeof(length));
  if (size > capacity)
    {
      ny_pcm_ring_read(ring, NULL, size);
      return -EMSGSIZE;
    }

  ny_pcm_ring_read(ring, data, size);
  return (int)size;
}

/****************************************************************************
 * Name: ny_pcm_wav_header
 ****************************************************************************/

void ny_pcm_wav_header(uint8_t *header, uint32_t rate, uint8_t channels)
{
  memcpy(header, "RIFF", 4);
  ny_pcm_put_le32(header + 4, 0xffffffff);
  memcpy(header + 8, "WAVEfmt ", 8);
  ny_pcm_put_le32(header + 16, 16);
  header[20] = 1; /* PCM */
  header[21] = 0;
  header[22] = channels;
  header[23] = 0;
  ny_pcm_put_le32(header + 24, rate);
  ny_pcm_put_le32(header + 28, rate * channels * 2);
  header[32] = (uint8_t)(channels * 2);
  header[33] = 0;
  header[34] = 16;
  header[35] = 0;
  memcpy(header + 36, "data", 4);
  ny_pcm_put_le32(header + 40, 0xffffffff);
}

/****************************************************************************
 * Name: ny_pcm_mono_to_stereo
 ****************************************************************************/

void ny_pcm_mono_to_stereo(const int16_t *mono, int16_t *stereo, size_t frames)
{
  /* Backwards, so that the two may be the same buffer. */

  while (frames-- > 0)
    {
      int16_t sample = mono[frames];

      stereo[2 * frames] = sample;
      stereo[2 * frames + 1] = sample;
    }
}

/****************************************************************************
 * Name: ny_pcm_stereo_to_mono
 ****************************************************************************/

void ny_pcm_stereo_to_mono(const int16_t *stereo, int16_t *mono, size_t frames)
{
  size_t frame;

  for (frame = 0; frame < frames; frame++)
    {
      mono[frame] =
          (int16_t)(((int32_t)stereo[2 * frame] + stereo[2 * frame + 1]) / 2);
    }
}

/****************************************************************************
 * Name: ny_pcm_rms
 ****************************************************************************/

uint32_t ny_pcm_rms(const int16_t *pcm, size_t samples)
{
  uint64_t sum = 0;
  size_t index;

  if (samples == 0)
    {
      return 0;
    }

  for (index = 0; index < samples; index++)
    {
      sum += (uint64_t)((int32_t)pcm[index] * pcm[index]);
    }

  return ny_pcm_isqrt(sum / samples);
}

/****************************************************************************
 * Name: ny_pcm_gate_init
 ****************************************************************************/

void ny_pcm_gate_init(struct ny_pcm_gate_s *gate, uint32_t threshold,
                      unsigned int attenuation_db, unsigned int hang_ms,
                      unsigned int frame_ms_x10)
{
  uint32_t gain = NY_PCM_GAIN_UNITY;

  memset(gate, 0, sizeof(*gate));

  /* 6 dB is a halving; the odd 3 dB is close enough to 181/256. */

  while (attenuation_db >= 6)
    {
      gain >>= 1;
      attenuation_db -= 6;
    }

  if (attenuation_db >= 3)
    {
      gain = gain * 181 / 256;
    }

  if (frame_ms_x10 == 0)
    {
      frame_ms_x10 = 75;
    }

  gate->threshold = threshold;
  gate->closed_gain = gain;
  gate->gain = NY_PCM_GAIN_UNITY;
  gate->hang_frames =
      (uint16_t)((hang_ms * 10 + frame_ms_x10 - 1) / frame_ms_x10);
}

/****************************************************************************
 * Name: ny_pcm_gate_far
 ****************************************************************************/

void ny_pcm_gate_far(struct ny_pcm_gate_s *gate, const int16_t *pcm,
                     size_t samples)
{
  gate->far_rms = pcm == NULL ? 0 : ny_pcm_rms(pcm, samples);
  if (gate->far_rms >= gate->threshold)
    {
      if (gate->hang == 0)
        {
          gate->closures++;
        }

      gate->hang = gate->hang_frames;
    }
  else if (gate->hang > 0)
    {
      gate->hang--;
    }
}

/****************************************************************************
 * Name: ny_pcm_gate_near
 ****************************************************************************/

bool ny_pcm_gate_near(struct ny_pcm_gate_s *gate, int16_t *pcm, size_t samples)
{
  uint32_t target = gate->hang > 0 ? gate->closed_gain : NY_PCM_GAIN_UNITY;
  uint32_t from = gate->gain;
  size_t index;

  gate->gain = target;
  if (from == NY_PCM_GAIN_UNITY && target == NY_PCM_GAIN_UNITY)
    {
      return false;
    }

  for (index = 0; index < samples; index++)
    {
      int64_t gain = (int64_t)from + ((int64_t)target - (int64_t)from) *
                                         (int64_t)(index + 1) /
                                         (int64_t)samples;

      pcm[index] = (int16_t)((int64_t)pcm[index] * gain / NY_PCM_GAIN_UNITY);
    }

  return true;
}
