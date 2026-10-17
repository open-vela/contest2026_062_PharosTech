/****************************************************************************
 * app/nyabula_core/ny_pcm.h
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

#ifndef __NYABULA_CORE_NY_PCM_H
#define __NYABULA_CORE_NY_PCM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Plain sample plumbing for the Bluetooth audio paths: a byte ring, the WAV
 * header the PCM front-end of the codec wants, channel conversion and the
 * half-duplex gate that stands in for echo cancellation.  No NuttX in here,
 * so all of it runs in the host unit test.
 */

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_PCM_WAV_HEADER_BYTES 44
#define NY_PCM_GAIN_UNITY       32768

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Not locked: one lock of the owner covers both ends. */

struct ny_pcm_ring_s
{
  uint8_t *data;
  size_t   capacity;
  size_t   head; /* Next byte to read */
  size_t   used;
};

/* While the far end is heard on the speaker the microphone is turned down,
 * and for a moment longer: the room keeps ringing and the output path holds
 * a few tens of milliseconds that have not been played yet.
 */

struct ny_pcm_gate_s
{
  uint32_t threshold;   /* Far end RMS that closes the gate */
  uint32_t closed_gain; /* Microphone gain while closed, Q15 */
  uint32_t gain;        /* Gain reached at the end of the last frame, Q15 */
  uint16_t hang_frames; /* Far end frames the gate stays closed after speech */
  uint16_t hang;        /* Frames left */
  uint32_t far_rms;     /* Last far end level, for status */
  uint32_t closures;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_pcm_ring_init(struct ny_pcm_ring_s *ring, size_t capacity);
void ny_pcm_ring_free(struct ny_pcm_ring_s *ring);
void ny_pcm_ring_clear(struct ny_pcm_ring_s *ring);
size_t ny_pcm_ring_space(const struct ny_pcm_ring_s *ring);

/****************************************************************************
 * Name: ny_pcm_ring_write / ny_pcm_ring_read
 *
 * Description:
 *   Write is all or nothing and says which with its return value; read
 *   returns what there is, up to size.
 *
 ****************************************************************************/

bool ny_pcm_ring_write(struct ny_pcm_ring_s *ring, const void *data,
                       size_t size);
size_t ny_pcm_ring_read(struct ny_pcm_ring_s *ring, void *data, size_t size);

/****************************************************************************
 * Name: ny_pcm_ring_put / ny_pcm_ring_get
 *
 * Description:
 *   The same ring carrying whole records (a media packet each) behind a two
 *   byte length.
 *
 * Returned Value:
 *   put: whether the record went in.  get: the record length, 0 when the
 *   ring is empty, -EMSGSIZE when the next record does not fit (it is
 *   dropped).
 *
 ****************************************************************************/

bool ny_pcm_ring_put(struct ny_pcm_ring_s *ring, const void *data,
                     uint16_t size);
int ny_pcm_ring_get(struct ny_pcm_ring_s *ring, void *data, size_t capacity);

/****************************************************************************
 * Name: ny_pcm_wav_header
 *
 * Description:
 *   The canonical 44 byte header of a 16 bit PCM stream of unknown length.
 *
 ****************************************************************************/

void ny_pcm_wav_header(uint8_t *header, uint32_t rate, uint8_t channels);

/****************************************************************************
 * Name: ny_pcm_mono_to_stereo / ny_pcm_stereo_to_mono
 *
 * Description:
 *   frames counts sample frames.  mono_to_stereo may run in place when both
 *   point at the same buffer of 2 * frames samples.
 *
 ****************************************************************************/

void ny_pcm_mono_to_stereo(const int16_t *mono, int16_t *stereo,
                           size_t frames);
void ny_pcm_stereo_to_mono(const int16_t *stereo, int16_t *mono,
                           size_t frames);
uint32_t ny_pcm_rms(const int16_t *pcm, size_t samples);

/****************************************************************************
 * Name: ny_pcm_gate_init / ny_pcm_gate_far / ny_pcm_gate_near
 *
 * Description:
 *   far is given every frame that goes to the speaker, near turns the
 *   microphone frame of the same moment down while the gate is closed and
 *   ramps across the frame so that the gate never clicks.
 *
 * Returned Value:
 *   ny_pcm_gate_near: whether the frame was attenuated at all.
 *
 ****************************************************************************/

void ny_pcm_gate_init(struct ny_pcm_gate_s *gate, uint32_t threshold,
                      unsigned int attenuation_db, unsigned int hang_ms,
                      unsigned int frame_ms_x10);
void ny_pcm_gate_far(struct ny_pcm_gate_s *gate, const int16_t *pcm,
                     size_t samples);
bool ny_pcm_gate_near(struct ny_pcm_gate_s *gate, int16_t *pcm,
                      size_t samples);

#endif /* __NYABULA_CORE_NY_PCM_H */
