/****************************************************************************
 * app/nyabula_core/ny_voice_dsp.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <math.h>
#include <string.h>

#include "ny_voice_dsp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_VOICE_PI 3.14159265358979323846

/* The resampler's pass band ends here; see NY_VOICE_RESAMPLE_TAPS. */

#define NY_VOICE_RESAMPLE_EDGE 7000.0

/* Samples converted per pass of the resampler's scratch buffer. */

#define NY_VOICE_RESAMPLE_BLOCK 256U

/* How fast the noise floor moves towards the level of a chunk, as shifts;
 * see ny_voice_gate_feed().
 */

#define NY_VOICE_FLOOR_FALL      2
#define NY_VOICE_FLOOR_RISE      7
#define NY_VOICE_FLOOR_RISE_LOUD 9

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int16_t ny_voice_clip(float sample);
static uint32_t ny_voice_le32(const uint8_t *bytes);
static void ny_voice_put_le32(uint8_t *bytes, uint32_t value);
static void ny_voice_put_le16(uint8_t *bytes, uint16_t value);
static size_t ny_voice_utf8_length(const char *text);
static bool ny_voice_is_mark(const char *text, size_t length,
                             const char *const *marks);
static bool ny_voice_is_dropped(const char *text, size_t length);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* A piece ends after one of these ... */

static const char *const g_voice_terminators[] = {
  "\xe3\x80\x82", /* Ideographic full stop */
  "\xef\xbc\x81", /* Fullwidth exclamation mark */
  "\xef\xbc\x9f", /* Fullwidth question mark */
  "\xef\xbc\x9b", /* Fullwidth semicolon */
  "\xe2\x80\xa6", /* Horizontal ellipsis */
  "!",
  "?",
  ";",
  "\n",
  NULL
};

/* ... and an overlong one is cut after one of these. */

static const char *const g_voice_breaks[] = {
  "\xef\xbc\x8c", /* Fullwidth comma */
  "\xe3\x80\x81", /* Ideographic comma */
  "\xef\xbc\x9a", /* Fullwidth colon */
  ",",
  ":",
  " ",
  NULL
};

/* Punctuation that alone is not worth a synthesis request. */

static const char *const g_voice_silent[] = {
  "\xe2\x80\x9c",
  "\xe2\x80\x9d", /* Double quotation marks */
  "\xe2\x80\x98",
  "\xe2\x80\x99", /* Single quotation marks */
  "\xef\xbc\x88",
  "\xef\xbc\x89", /* Fullwidth parentheses */
  "\xe3\x80\x8a",
  "\xe3\x80\x8b", /* Double angle brackets */
  "\xe2\x80\x94",
  "\xef\xbd\x9e", /* Em dash, fullwidth tilde */
  NULL
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int16_t ny_voice_clip(float sample)
{
  float scaled;

  /* One scale in both directions (32768), so that a sample which crosses
   * the link and comes back is the sample that left.  The comparison is
   * false for a NaN, which therefore becomes silence.
   */

  if (!(sample >= -1.0f && sample < 1.0f))
    {
      return sample >= 1.0f ? INT16_MAX : sample < -1.0f ? INT16_MIN : 0;
    }

  scaled = sample * 32768.0f;
  scaled = scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f;
  return scaled >= 32767.0f ? INT16_MAX : (int16_t)scaled;
}

static uint32_t ny_voice_le32(const uint8_t *bytes)
{
  return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
         (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void ny_voice_put_le32(uint8_t *bytes, uint32_t value)
{
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

static void ny_voice_put_le16(uint8_t *bytes, uint16_t value)
{
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
}

/****************************************************************************
 * Name: ny_voice_utf8_length
 *
 * Description:
 *   Bytes of the well-formed UTF-8 character at `text`, or 0 when the byte
 *   there does not start one.  The compute domain refuses a text with a
 *   single malformed byte, so such bytes are left out rather than sent.
 *
 ****************************************************************************/

static size_t ny_voice_utf8_length(const char *text)
{
  const unsigned char *bytes = (const unsigned char *)text;
  size_t length;
  size_t index;

  if (bytes[0] < 0x80)
    {
      return 1;
    }

  length = bytes[0] >= 0xf0 && bytes[0] <= 0xf4   ? 4
           : bytes[0] >= 0xe0 && bytes[0] <= 0xef ? 3
           : bytes[0] >= 0xc2 && bytes[0] <= 0xdf ? 2
                                                  : 0;
  for (index = 1; index < length; index++)
    {
      if ((bytes[index] & 0xc0) != 0x80)
        {
          return 0;
        }
    }

  return length;
}

static bool ny_voice_is_mark(const char *text, size_t length,
                             const char *const *marks)
{
  size_t index;

  for (index = 0; marks[index] != NULL; index++)
    {
      if (strlen(marks[index]) == length &&
          memcmp(marks[index], text, length) == 0)
        {
          return true;
        }
    }

  return false;
}

static bool ny_voice_is_dropped(const char *text, size_t length)
{
  /* Markdown emphasis, headings and code marks: a language model writes
   * them, a speaker should not read them.
   */

  return length == 1 &&
         (text[0] == '*' || text[0] == '#' || text[0] == '`' ||
          text[0] == '\r' || text[0] == '\t' ||
          (unsigned char)text[0] < 0x20) &&
         text[0] != '\n';
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ny_voice_s16_to_f32(float *out, const int16_t *in, size_t count)
{
  size_t index;

  for (index = 0; index < count; index++)
    {
      out[index] = (float)in[index] / 32768.0f;
    }
}

void ny_voice_f32_to_s16(int16_t *out, const float *in, size_t count)
{
  size_t index;

  for (index = 0; index < count; index++)
    {
      out[index] = ny_voice_clip(in[index]);
    }
}

void ny_voice_f32_to_s16_stereo(int16_t *out, const float *in, size_t count)
{
  size_t index;

  for (index = 0; index < count; index++)
    {
      int16_t sample = ny_voice_clip(in[index]);

      out[2 * index] = sample;
      out[2 * index + 1] = sample;
    }
}

void ny_voice_s16_to_stereo(int16_t *out, const int16_t *in, size_t count)
{
  size_t index = count;

  /* Backwards, so that a buffer may be widened in place. */

  while (index-- > 0)
    {
      int16_t sample = in[index];

      out[2 * index] = sample;
      out[2 * index + 1] = sample;
    }
}

uint32_t ny_voice_rms_s16(const int16_t *in, size_t count)
{
  uint64_t sum = 0;
  uint64_t mean;
  uint64_t root;
  uint64_t bit;
  size_t index;

  if (count == 0)
    {
      return 0;
    }

  for (index = 0; index < count; index++)
    {
      int32_t sample = in[index];

      sum += (uint64_t)(sample * sample);
    }

  /* Integer square root: the capture thread has no use for a float. */

  mean = sum / count;
  root = 0;
  for (bit = 1ULL << 30; bit != 0; bit >>= 2)
    {
      if (mean >= root + bit)
        {
          mean -= root + bit;
          root = (root >> 1) + bit;
        }
      else
        {
          root >>= 1;
        }
    }

  return (uint32_t)root;
}

void ny_voice_wav_header(uint8_t *out, uint32_t rate, uint16_t channels,
                         uint32_t data_bytes)
{
  /* A live stream has no length to announce; the decoder node plays until
   * the final buffer whatever the header says, and a file reader is better
   * off with "as much as there is" than with zero.
   */

  uint32_t size = data_bytes != 0 ? data_bytes : 0xffffffffU - 36U;

  memcpy(out, "RIFF", 4);
  ny_voice_put_le32(out + 4, size + 36U);
  memcpy(out + 8, "WAVEfmt ", 8);
  ny_voice_put_le32(out + 16, 16);
  ny_voice_put_le16(out + 20, 1);
  ny_voice_put_le16(out + 22, channels);
  ny_voice_put_le32(out + 24, rate);
  ny_voice_put_le32(out + 28, rate * channels * 2U);
  ny_voice_put_le16(out + 32, (uint16_t)(channels * 2U));
  ny_voice_put_le16(out + 34, 16);
  memcpy(out + 36, "data", 4);
  ny_voice_put_le32(out + 40, size);
}

int ny_voice_wav_parse(const uint8_t *prefix, size_t size, uint32_t *rate,
                       uint16_t *channels, size_t *data_offset,
                       uint32_t *data_bytes)
{
  size_t position = 12;
  bool format = false;

  if (size < 12 || memcmp(prefix, "RIFF", 4) != 0 ||
      memcmp(prefix + 8, "WAVE", 4) != 0)
    {
      return -1;
    }

  while (position + 8 <= size)
    {
      uint32_t length = ny_voice_le32(prefix + position + 4);

      if (memcmp(prefix + position, "fmt ", 4) == 0)
        {
          if (length < 16 || position + 8 + 16 > size ||
              (prefix[position + 8] | prefix[position + 9] << 8) != 1 ||
              (prefix[position + 22] | prefix[position + 23] << 8) != 16)
            {
              return -1;
            }

          *channels =
              (uint16_t)(prefix[position + 10] | prefix[position + 11] << 8);
          *rate = ny_voice_le32(prefix + position + 12);
          format = *channels >= 1 && *channels <= 2 && *rate != 0;
          if (!format)
            {
              return -1;
            }
        }
      else if (memcmp(prefix + position, "data", 4) == 0)
        {
          if (!format)
            {
              return -1;
            }

          *data_offset = position + 8;
          *data_bytes = length;
          return 0;
        }

      /* Chunks are word aligned.  A length that would wrap ends the walk. */

      if (length > size)
        {
          return -1;
        }

      position += 8 + (size_t)length + (length & 1U);
    }

  return -1;
}

void ny_voice_ring_init(struct ny_voice_ring_s *ring, int16_t *storage,
                        size_t capacity)
{
  ring->data = storage;
  ring->capacity = capacity;
  ring->total = 0;
}

void ny_voice_ring_write(struct ny_voice_ring_s *ring, const int16_t *in,
                         size_t count)
{
  size_t at;
  size_t first;

  if (ring->capacity == 0)
    {
      return;
    }

  /* Only the tail of an oversized write can still be in the ring, but every
   * sample of it counts towards the position.
   */

  if (count > ring->capacity)
    {
      ring->total += count - ring->capacity;
      in += count - ring->capacity;
      count = ring->capacity;
    }

  at = (size_t)(ring->total % ring->capacity);
  first = ring->capacity - at < count ? ring->capacity - at : count;
  memcpy(ring->data + at, in, first * sizeof(int16_t));
  memcpy(ring->data, in + first, (count - first) * sizeof(int16_t));
  ring->total += count;
}

uint64_t ny_voice_ring_oldest(const struct ny_voice_ring_s *ring)
{
  return ring->total > ring->capacity ? ring->total - ring->capacity : 0;
}

size_t ny_voice_ring_read(const struct ny_voice_ring_s *ring, uint64_t *cursor,
                          int16_t *out, size_t count, bool *gap)
{
  uint64_t oldest = ny_voice_ring_oldest(ring);
  size_t at;
  size_t first;

  if (gap != NULL)
    {
      *gap = false;
    }

  if (ring->capacity == 0)
    {
      return 0;
    }

  if (*cursor < oldest)
    {
      *cursor = oldest;
      if (gap != NULL)
        {
          *gap = true;
        }
    }
  else if (*cursor > ring->total)
    {
      *cursor = ring->total;
    }

  if (ring->total - *cursor < count)
    {
      count = (size_t)(ring->total - *cursor);
    }

  at = (size_t)(*cursor % ring->capacity);
  first = ring->capacity - at < count ? ring->capacity - at : count;
  memcpy(out, ring->data + at, first * sizeof(int16_t));
  memcpy(out + first, ring->data, (count - first) * sizeof(int16_t));
  *cursor += count;
  return count;
}

void ny_voice_gate_init(struct ny_voice_gate_s *gate,
                        const struct ny_voice_gate_config_s *config)
{
  memset(gate, 0, sizeof(*gate));
  gate->config = *config;
  gate->quiet_ms = config->hang_ms;
}

bool ny_voice_gate_feed(struct ny_voice_gate_s *gate, uint32_t rms,
                        uint32_t chunk_ms)
{
  uint32_t level = rms > 0x0fffffffU ? 0xfffffff0U : rms * 16U;
  uint64_t threshold;

  if (!gate->primed)
    {
      gate->floor_x16 = level;
      gate->primed = true;
    }

  threshold = (uint64_t)gate->floor_x16 * gate->config.ratio_x16 / 256U;
  if (threshold < gate->config.min_rms)
    {
      threshold = gate->config.min_rms;
    }

  /* The floor follows a quieter room at once.  It follows a louder one
   * slowly, and slower still while the level counts as speech: a sentence
   * must not raise the floor under itself, a fan that was switched on must
   * within half a minute.
   */

  if (level < gate->floor_x16)
    {
      gate->floor_x16 -= (gate->floor_x16 - level) >> NY_VOICE_FLOOR_FALL;
    }
  else
    {
      gate->floor_x16 +=
          (level - gate->floor_x16) >>
          (rms >= threshold ? NY_VOICE_FLOOR_RISE_LOUD : NY_VOICE_FLOOR_RISE);
    }

  gate->loud = rms >= threshold;
  if (gate->loud)
    {
      gate->quiet_ms = 0;
    }
  else if (gate->quiet_ms < gate->config.hang_ms)
    {
      gate->quiet_ms += chunk_ms;
    }

  gate->open = gate->quiet_ms < gate->config.hang_ms;
  return gate->open;
}

uint64_t ny_voice_attach_sample(const struct ny_voice_attach_s *attach,
                                bool *from_trigger, bool *clamped)
{
  uint64_t oldest;
  uint64_t wanted;
  bool trusted;

  /* The service already withholds HAS_OFFSETS from offsets it does not
   * believe; the order is checked again because a wrong attach point costs
   * the whole command.
   */

  trusted = attach->has_offsets && attach->start_sample < attach->end_sample &&
            attach->end_sample <= attach->trigger_sample;
  if (trusted)
    {
      wanted = attach->end_sample;
    }
  else
    {
      wanted = attach->trigger_sample > attach->backoff_samples
                   ? attach->trigger_sample - attach->backoff_samples
                   : 0;
    }

  /* What the compute domain can still serve: its ring, which a
   * discontinuity empties.  An attach point older than that is refused by
   * the service rather than moved, so it is moved here, past a margin the
   * ring will not overwrite before the request arrives.
   */

  oldest = attach->stream_next > attach->ring_samples
               ? attach->stream_next - attach->ring_samples
               : 0;
  if (oldest != 0)
    {
      oldest += attach->margin_samples;
    }

  if (oldest < attach->stream_base)
    {
      oldest = attach->stream_base;
    }

  if (oldest > attach->stream_next)
    {
      oldest = attach->stream_next;
    }

  if (from_trigger != NULL)
    {
      *from_trigger = !trusted;
    }

  if (clamped != NULL)
    {
      *clamped = wanted < oldest || wanted > attach->stream_next;
    }

  return wanted < oldest                ? oldest
         : wanted > attach->stream_next ? attach->stream_next
                                        : wanted;
}

void ny_voice_resample_init(struct ny_voice_resample_s *state)
{
  double cutoff = NY_VOICE_RESAMPLE_EDGE / (double)NY_VOICE_TTS_RATE;
  unsigned int phase;
  unsigned int tap;

  memset(state->history, 0, sizeof(state->history));
  state->phase = NY_VOICE_RESAMPLE_UP * NY_VOICE_RESAMPLE_TAPS;
  if (state->ready)
    {
      return;
    }

  for (phase = 0; phase < NY_VOICE_RESAMPLE_UP; phase++)
    {
      double sum = 0.0;

      for (tap = 0; tap < NY_VOICE_RESAMPLE_TAPS; tap++)
        {
          double at = (double)phase / NY_VOICE_RESAMPLE_UP +
                      NY_VOICE_RESAMPLE_TAPS / 2.0 - 1.0 - (double)tap;
          double angle = 2.0 * NY_VOICE_PI * cutoff * at;
          double sinc = fabs(at) < 1e-9 ? 1.0 : sin(angle) / angle;
          double window = 0.5 * (1.0 + cos(2.0 * NY_VOICE_PI * at /
                                           (double)NY_VOICE_RESAMPLE_TAPS));
          double value =
              fabs(at) >= NY_VOICE_RESAMPLE_TAPS / 2.0 ? 0.0 : sinc * window;

          state->taps[phase][tap] = (float)value;
          sum += value;
        }

      /* Unity gain at DC for every phase, or a steady level would ripple
       * at the phase rate.
       */

      for (tap = 0; tap < NY_VOICE_RESAMPLE_TAPS; tap++)
        {
          state->taps[phase][tap] = (float)(state->taps[phase][tap] / sum);
        }
    }

  state->ready = true;
}

size_t ny_voice_resample(struct ny_voice_resample_s *state, const float *in,
                         size_t count, float *out, size_t capacity)
{
  float block[NY_VOICE_RESAMPLE_TAPS + NY_VOICE_RESAMPLE_BLOCK];
  size_t produced = 0;

  while (count > 0)
    {
      size_t take =
          count < NY_VOICE_RESAMPLE_BLOCK ? count : NY_VOICE_RESAMPLE_BLOCK;
      size_t length = NY_VOICE_RESAMPLE_TAPS + take;

      /* block[b] is the input sample TAPS - b positions before the first
       * new one; state->phase counts from block[0] in 1/160 samples.
       */

      memcpy(block, state->history, sizeof(state->history));
      memcpy(block + NY_VOICE_RESAMPLE_TAPS, in, take * sizeof(float));

      for (;;)
        {
          size_t centre = state->phase / NY_VOICE_RESAMPLE_UP;
          const float *taps = state->taps[state->phase % NY_VOICE_RESAMPLE_UP];
          const float *window;
          float sum = 0.0f;
          unsigned int tap;

          if (centre + NY_VOICE_RESAMPLE_TAPS / 2 > length - 1)
            {
              break;
            }

          if (produced >= capacity)
            {
              /* The caller sized `out` too small; the rest of the input is
               * still run through the history so the stream stays aligned.
               */

              state->phase += NY_VOICE_RESAMPLE_DOWN;
              continue;
            }

          window = block + centre - NY_VOICE_RESAMPLE_TAPS / 2 + 1;
          for (tap = 0; tap < NY_VOICE_RESAMPLE_TAPS; tap++)
            {
              sum += window[tap] * taps[tap];
            }

          out[produced++] = sum;
          state->phase += NY_VOICE_RESAMPLE_DOWN;
        }

      memcpy(state->history, block + take, sizeof(state->history));
      state->phase -= (uint32_t)(take * NY_VOICE_RESAMPLE_UP);
      in += take;
      count -= take;
    }

  return produced;
}

size_t ny_voice_sentence_next(const char *text, size_t *position, char *out,
                              size_t size)
{
  size_t at = *position;

  if (size < 8)
    {
      return 0;
    }

  for (;;)
    {
      size_t length = 0;
      size_t break_in = 0;  /* Input position just past the last break. */
      size_t break_out = 0; /* Output length up to that break.          */
      bool speakable = false;
      bool ended = false;

      while (text[at] != '\0')
        {
          size_t bytes = ny_voice_utf8_length(text + at);
          bool terminator;

          if (bytes == 0 || ny_voice_is_dropped(text + at, bytes))
            {
              at += bytes == 0 ? 1 : bytes;
              continue;
            }

          terminator = ny_voice_is_mark(text + at, bytes, g_voice_terminators);

          /* A full stop ends a sentence unless a digit follows ("3.5"). */

          if (bytes == 1 && text[at] == '.' &&
              !(text[at + 1] >= '0' && text[at + 1] <= '9'))
            {
              terminator = true;
            }

          if (ended && !terminator)
            {
              break;
            }

          if (length == 0 && (text[at] == ' ' || text[at] == '\n'))
            {
              at += bytes;
              continue;
            }

          if (length + bytes > size - 1)
            {
              /* Too long for one request: back to the last clause break, or
               * cut between two characters when there was none.
               */

              if (break_out != 0)
                {
                  length = break_out;
                  at = break_in;
                }

              break;
            }

          if (text[at] != '\n')
            {
              memcpy(out + length, text + at, bytes);
              length += bytes;
            }

          at += bytes;
          if (terminator)
            {
              ended = true;
            }
          else if (ny_voice_is_mark(text + at - bytes, bytes, g_voice_breaks))
            {
              break_in = at;
              break_out = length;
            }
          else if (!ny_voice_is_mark(text + at - bytes, bytes,
                                     g_voice_silent) &&
                   !(bytes == 1 &&
                     strchr(".-\"'()[]<>/\\|~=+", text[at - 1]) != NULL))
            {
              speakable = true;
            }
        }

      *position = at;
      if (length == 0 && text[at] == '\0')
        {
          return 0;
        }

      if (speakable)
        {
          out[length] = '\0';
          return length;
        }
    }
}
