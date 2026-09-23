/****************************************************************************
 * app/nyabula_core/ny_voice_dsp.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __NYABULA_CORE_NY_VOICE_DSP_H
#define __NYABULA_CORE_NY_VOICE_DSP_H

/****************************************************************************
 * The sample arithmetic of the voice chain.
 *
 * Everything here is pure: no NuttX header, no configuration symbol, no
 * allocation, no lock.  That is what lets tools/nyabula_core/tests build the
 * very same file on the host under ASan and UBSan; the threads in ny_voice.c
 * own the devices, the locks and the policy.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_VOICE_CAPTURE_RATE 16000U
#define NY_VOICE_TTS_RATE     44100U
#define NY_VOICE_WAV_HEADER   44U

/* 44100 -> 16000 is 160/441.  Forty-eight taps at the input rate put the
 * transition band between about 5.5 and 8.5 kHz, which keeps speech intact
 * and what folds back stays above 7 kHz.
 */

#define NY_VOICE_RESAMPLE_UP   160U
#define NY_VOICE_RESAMPLE_DOWN 441U
#define NY_VOICE_RESAMPLE_TAPS 48U

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The pre-roll ring: the last `capacity` captured samples, addressed by
 * their absolute position in the capture stream.  One writer; any number of
 * readers, each with a cursor of its own that consumes nothing.  The caller
 * serialises access.
 */

struct ny_voice_ring_s
{
  int16_t *data;
  size_t capacity;
  uint64_t total; /* Samples ever written = position of the next one. */
};

/* The idle gate.  It only decides whether silence is worth streaming to the
 * wake word service; it never decides whether a wake word was spoken.  So
 * it errs towards open: a low absolute threshold, a threshold relative to a
 * noise floor that follows the room, a long hang-over, and the caller
 * replays the pre-roll when it opens.
 */

struct ny_voice_gate_config_s
{
  uint32_t min_rms;   /* Never open below this level.                  */
  uint32_t ratio_x16; /* Open at floor * ratio_x16 / 16.               */
  uint32_t hang_ms;   /* Stay open this long after the last loud chunk. */
};

struct ny_voice_gate_s
{
  struct ny_voice_gate_config_s config;
  uint32_t floor_x16; /* Noise floor estimate, RMS * 16.               */
  uint32_t quiet_ms;  /* Since the last loud chunk.                    */
  bool primed;        /* The floor has seen its first chunk.           */
  bool open;
  bool loud; /* The last chunk was above the threshold.       */
};

struct ny_voice_resample_s
{
  float history[NY_VOICE_RESAMPLE_TAPS];
  uint32_t phase; /* Position of the next output, in 1/160 input   */
                  /* samples past the newest history sample + 1.   */
  bool ready;
  float taps[NY_VOICE_RESAMPLE_UP][NY_VOICE_RESAMPLE_TAPS];
};

/* What DETECTED said, and what the stream looks like right now. */

struct ny_voice_attach_s
{
  bool has_offsets; /* NYAMP_KWS_DETECTED_HAS_OFFSETS.            */
  uint64_t start_sample;
  uint64_t end_sample;
  uint64_t trigger_sample;
  uint64_t stream_next;     /* Position of the next sample to be pushed.  */
  uint64_t stream_base;     /* First sample after the last discontinuity. */
  uint32_t ring_samples;    /* What the compute domain keeps (10 s).      */
  uint32_t backoff_samples; /* Taken off trigger_sample without offsets. */
  uint32_t margin_samples;  /* Kept clear of the ring's oldest sample.    */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/* Sample formats.  Float to integer clips instead of wrapping and turns a
 * NaN into silence: the samples come from another domain.
 */

void ny_voice_s16_to_f32(float *out, const int16_t *in, size_t count);
void ny_voice_f32_to_s16(int16_t *out, const float *in, size_t count);
void ny_voice_f32_to_s16_stereo(int16_t *out, const float *in, size_t count);
void ny_voice_s16_to_stereo(int16_t *out, const int16_t *in, size_t count);
uint32_t ny_voice_rms_s16(const int16_t *in, size_t count);

/* The canonical 44-byte header the PCM decoder node insists on.  data_bytes
 * 0 announces a stream of unknown length.
 */

void ny_voice_wav_header(uint8_t *out, uint32_t rate, uint16_t channels,
                         uint32_t data_bytes);

/* Find the PCM in a RIFF/WAVE prefix.  Returns 0 and the format, or -1 when
 * the prefix is not 16 bit PCM or does not reach the data chunk.
 */

int ny_voice_wav_parse(const uint8_t *prefix, size_t size, uint32_t *rate,
                       uint16_t *channels, size_t *data_offset,
                       uint32_t *data_bytes);

void ny_voice_ring_init(struct ny_voice_ring_s *ring, int16_t *storage,
                        size_t capacity);
void ny_voice_ring_write(struct ny_voice_ring_s *ring, const int16_t *in,
                         size_t count);
uint64_t ny_voice_ring_oldest(const struct ny_voice_ring_s *ring);

/* Copy up to `count` samples from *cursor on and advance it.  A cursor that
 * fell out of the ring is moved to the oldest sample and *gap is set; one
 * ahead of the writer is pulled back to it.
 */

size_t ny_voice_ring_read(const struct ny_voice_ring_s *ring, uint64_t *cursor,
                          int16_t *out, size_t count, bool *gap);

void ny_voice_gate_init(struct ny_voice_gate_s *gate,
                        const struct ny_voice_gate_config_s *config);
bool ny_voice_gate_feed(struct ny_voice_gate_s *gate, uint32_t rms,
                        uint32_t chunk_ms);

/* Where an ASR request attaches to the wake word stream.  *from_trigger
 * tells a caller that the decoder's offsets were not used, *clamped that
 * the wanted position was no longer (or not yet) in the stream.
 */

uint64_t ny_voice_attach_sample(const struct ny_voice_attach_s *attach,
                                bool *from_trigger, bool *clamped);

void ny_voice_resample_init(struct ny_voice_resample_s *state);
size_t ny_voice_resample(struct ny_voice_resample_s *state, const float *in,
                         size_t count, float *out, size_t capacity);

/* The next piece of `text` worth one TTS request: a sentence, or a clause
 * or a run of whole UTF-8 characters when a sentence would not fit `size`
 * bytes (NUL included).  Markdown decoration is dropped.  Returns the
 * length of the piece, 0 when the text is exhausted.
 */

size_t ny_voice_sentence_next(const char *text, size_t *position, char *out,
                              size_t size);

#ifdef __cplusplus
}
#endif

#endif /* __NYABULA_CORE_NY_VOICE_DSP_H */
