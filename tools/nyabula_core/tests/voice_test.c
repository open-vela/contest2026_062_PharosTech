/****************************************************************************
 * tools/nyabula_core/tests/voice_test.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the pure half of the voice chain: ny_voice_dsp.c (sample
 * formats, pre-roll ring, attach arithmetic, idle gate, resampler, sentence
 * chunking) and ny_voice_sm.c (the turn state machine driven by scripted
 * event lists).  Built by voice_test.py; not part of the firmware.
 *
 ****************************************************************************/

#include "ny_voice_dsp.h"
#include "ny_voice_sm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(list) (sizeof(list) / sizeof((list)[0]))

#define CHECK(condition, ...)                                   \
  do                                                            \
    {                                                           \
      g_checks++;                                               \
      if (!(condition))                                         \
        {                                                       \
          g_failures++;                                         \
          fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);       \
          fprintf(stderr, __VA_ARGS__);                         \
          fprintf(stderr, "\n");                                \
        }                                                       \
    }                                                           \
  while (0)

static unsigned long g_checks;
static unsigned long g_failures;

/****************************************************************************
 * Sample formats
 ****************************************************************************/

static void test_formats(void)
{
  static int16_t all[65536];
  static float floats[65536];
  static int16_t back[65536];
  static int16_t stereo[2 * 65536];
  float odd[6];
  int16_t clipped[6];
  uint8_t header[NY_VOICE_WAV_HEADER];
  uint8_t listed[128];
  uint32_t rate = 0;
  uint32_t bytes = 0;
  uint16_t channels = 0;
  size_t offset = 0;
  int index;

  for (index = 0; index < 65536; index++)
    {
      all[index] = (int16_t)(index - 32768);
    }

  ny_voice_s16_to_f32(floats, all, 65536);
  ny_voice_f32_to_s16(back, floats, 65536);
  for (index = 0; index < 65536; index++)
    {
      if (back[index] != all[index] || floats[index] < -1.0f ||
          floats[index] >= 1.0f)
        {
          CHECK(false, "s16 %d -> %f -> %d", all[index],
                (double)floats[index], back[index]);
          break;
        }
    }

  CHECK(index == 65536, "round trip stopped at %d", index);

  odd[0] = 2.5f;
  odd[1] = -7.0f;
  odd[2] = NAN;
  odd[3] = INFINITY;
  odd[4] = -INFINITY;
  odd[5] = 0.5f;
  ny_voice_f32_to_s16(clipped, odd, 6);
  CHECK(clipped[0] == INT16_MAX && clipped[1] == INT16_MIN, "clip");
  CHECK(clipped[2] == 0, "NaN became %d", clipped[2]);
  CHECK(clipped[3] == INT16_MAX && clipped[4] == INT16_MIN, "infinities");
  CHECK(clipped[5] == 16384, "0.5 became %d", clipped[5]);

  ny_voice_f32_to_s16_stereo(stereo, odd, 6);
  for (index = 0; index < 6; index++)
    {
      CHECK(stereo[2 * index] == clipped[index] &&
            stereo[2 * index + 1] == clipped[index], "stereo %d", index);
    }

  /* In place: the widened copy overlaps its own source. */

  memcpy(stereo, all, 1000 * sizeof(int16_t));
  ny_voice_s16_to_stereo(stereo, stereo, 1000);
  for (index = 0; index < 1000; index++)
    {
      if (stereo[2 * index] != all[index] ||
          stereo[2 * index + 1] != all[index])
        {
          break;
        }
    }

  CHECK(index == 1000, "in-place stereo broke at %d", index);

  CHECK(ny_voice_rms_s16(all, 0) == 0, "rms of nothing");
  for (index = 0; index < 1000; index++)
    {
      back[index] = (int16_t)(index % 2 == 0 ? 1000 : -1000);
    }

  CHECK(ny_voice_rms_s16(back, 1000) == 1000, "rms square wave");
  for (index = 0; index < 1000; index++)
    {
      back[index] = INT16_MIN;
    }

  CHECK(ny_voice_rms_s16(back, 1000) == 32768, "rms full scale");

  ny_voice_wav_header(header, 44100, 2, 176400);
  CHECK(ny_voice_wav_parse(header, sizeof(header), &rate, &channels, &offset,
                           &bytes) == 0 &&
        rate == 44100 && channels == 2 && offset == 44 && bytes == 176400,
        "canonical header");
  CHECK(header[28] == 0x10 && header[29] == 0xb1 && header[30] == 0x02,
        "byte rate");
  CHECK(header[32] == 4 && header[34] == 16, "block align and width");

  ny_voice_wav_header(header, 16000, 1, 0);
  CHECK(ny_voice_wav_parse(header, sizeof(header), &rate, &channels, &offset,
                           &bytes) == 0 && rate == 16000 && channels == 1 &&
        bytes == 0xffffffffU - 36U, "streaming header");

  /* fmt, an odd-sized LIST chunk, then data. */

  ny_voice_wav_header(header, 16000, 1, 3200);
  memcpy(listed, header, 36);
  memcpy(listed + 36, "LIST\x05\x00\x00\x00" "abcde\x00", 14);
  memcpy(listed + 50, header + 36, 8);
  CHECK(ny_voice_wav_parse(listed, 58, &rate, &channels, &offset, &bytes) ==
        0 && offset == 58 && bytes == 3200, "header with a LIST chunk");
  CHECK(ny_voice_wav_parse(listed, 40, &rate, &channels, &offset, &bytes) < 0,
        "truncated before data");
  header[20] = 3; /* IEEE float */
  CHECK(ny_voice_wav_parse(header, sizeof(header), &rate, &channels, &offset,
                           &bytes) < 0, "float wav refused");
  header[20] = 1;
  header[34] = 24;
  CHECK(ny_voice_wav_parse(header, sizeof(header), &rate, &channels, &offset,
                           &bytes) < 0, "24 bit wav refused");
  memset(listed + 40, 0xff, 4); /* A chunk length that would wrap. */
  CHECK(ny_voice_wav_parse(listed, 58, &rate, &channels, &offset, &bytes) < 0,
        "absurd chunk length");
}

/****************************************************************************
 * Pre-roll ring and sample offsets
 ****************************************************************************/

static int16_t sample_at(uint64_t position)
{
  return (int16_t)(position * 7919U % 65521U - 32760);
}

static void test_ring(void)
{
  int16_t storage[1000];
  int16_t chunk[2500];
  int16_t out[1200];
  struct ny_voice_ring_s ring;
  uint64_t written = 0;
  uint64_t cursor;
  uint64_t reader = 0;
  uint64_t verified = 0;
  size_t got;
  bool gap;
  int round;
  size_t index;

  ny_voice_ring_init(&ring, storage, COUNT(storage));
  cursor = 0;
  CHECK(ny_voice_ring_read(&ring, &cursor, out, 10, &gap) == 0 && !gap,
        "empty ring");

  /* Uneven writes that wrap many times, a reader that keeps up. */

  for (round = 0; round < 400; round++)
    {
      size_t count = (size_t)(round * 37 % 331) + 1;

      for (index = 0; index < count; index++)
        {
          chunk[index] = sample_at(written + index);
        }

      ny_voice_ring_write(&ring, chunk, count);
      written += count;
      CHECK(ring.total == written, "position");

      got = ny_voice_ring_read(&ring, &reader, out, COUNT(out), &gap);
      CHECK(!gap, "a reader that keeps up saw a gap");
      for (index = 0; index < got; index++)
        {
          if (out[index] != sample_at(verified + index))
            {
              CHECK(false, "sample %llu wrong",
                    (unsigned long long)(verified + index));
              break;
            }
        }

      verified += got;
    }

  CHECK(verified == written && reader == written, "reader position");
  CHECK(written > 50 * COUNT(storage), "ring wrapped %llu samples",
        (unsigned long long)written);

  /* A second cursor consumes nothing from the first. */

  cursor = written - 300;
  got = ny_voice_ring_read(&ring, &cursor, out, 100, &gap);
  CHECK(got == 100 && !gap && out[0] == sample_at(written - 300) &&
        out[99] == sample_at(written - 201), "second reader");
  CHECK(cursor == written - 200, "second reader cursor");

  /* Older than the ring: moved to the oldest sample, and told. */

  cursor = written - 5000;
  got = ny_voice_ring_read(&ring, &cursor, out, 50, &gap);
  CHECK(got == 50 && gap && out[0] == sample_at(written - COUNT(storage)),
        "attach older than the ring");
  CHECK(ny_voice_ring_oldest(&ring) == written - COUNT(storage), "oldest");

  /* Ahead of the writer: nothing yet, no gap. */

  cursor = written + 77;
  got = ny_voice_ring_read(&ring, &cursor, out, 50, &gap);
  CHECK(got == 0 && !gap && cursor == written, "reader ahead");

  /* A write larger than the ring keeps its tail and its count. */

  for (index = 0; index < COUNT(chunk); index++)
    {
      chunk[index] = sample_at(written + index);
    }

  ny_voice_ring_write(&ring, chunk, COUNT(chunk));
  written += COUNT(chunk);
  cursor = 0;
  got = ny_voice_ring_read(&ring, &cursor, out, COUNT(storage), &gap);
  CHECK(ring.total == written && got == COUNT(storage) && gap &&
        out[0] == sample_at(written - COUNT(storage)) &&
        out[999] == sample_at(written - 1), "oversized write");

  /* The read that straddles the physical end of the storage. */

  cursor = written - 10;
  got = ny_voice_ring_read(&ring, &cursor, out, 100, &gap);
  CHECK(got == 10 && out[9] == sample_at(written - 1), "short tail read");
}

static void test_attach(void)
{
  struct ny_voice_attach_s attach;
  bool trigger;
  bool clamped;

  memset(&attach, 0, sizeof(attach));
  attach.ring_samples = 160000;
  attach.backoff_samples = 8000;
  attach.margin_samples = 8000;

  /* The normal case: right after the phrase. */

  attach.has_offsets = true;
  attach.start_sample = 100000;
  attach.end_sample = 127000;
  attach.trigger_sample = 133000;
  attach.stream_next = 136000;
  CHECK(ny_voice_attach_sample(&attach, &trigger, &clamped) == 127000 &&
        !trigger && !clamped, "trusted offsets");

  /* No offsets: half a second before the trigger. */

  attach.has_offsets = false;
  CHECK(ny_voice_attach_sample(&attach, &trigger, &clamped) == 125000 &&
        trigger && !clamped, "trigger fallback");

  /* Offsets that contradict each other are not used even if flagged. */

  attach.has_offsets = true;
  attach.end_sample = 140000;
  CHECK(ny_voice_attach_sample(&attach, &trigger, NULL) == 125000 && trigger,
        "end after trigger");
  attach.end_sample = 90000;
  CHECK(ny_voice_attach_sample(&attach, &trigger, NULL) == 125000 && trigger,
        "end before start");

  /* A trigger in the first half second of the stream. */

  attach.has_offsets = false;
  attach.trigger_sample = 3000;
  attach.stream_next = 6000;
  CHECK(ny_voice_attach_sample(&attach, &trigger, &clamped) == 0 && !clamped,
        "backoff floors at zero");

  /* The ASR model took so long to load that the phrase left the ring. */

  attach.has_offsets = true;
  attach.start_sample = 100000;
  attach.end_sample = 127000;
  attach.trigger_sample = 133000;
  attach.stream_next = 400000;
  CHECK(ny_voice_attach_sample(&attach, &trigger, &clamped) == 248000 &&
        clamped && !trigger, "older than the compute ring");

  /* Capture paused (the robot spoke): the ring restarts at stream_base. */

  attach.stream_next = 140000;
  attach.stream_base = 130000;
  CHECK(ny_voice_attach_sample(&attach, NULL, &clamped) == 130000 && clamped,
        "before the last discontinuity");

  /* An end offset ahead of what was pushed waits at the stream head. */

  attach.stream_base = 0;
  attach.end_sample = 150000;
  attach.trigger_sample = 150000;
  CHECK(ny_voice_attach_sample(&attach, NULL, &clamped) == 140000 && clamped,
        "ahead of the stream");

  /* Positions past 2^32: more than three days of capture. */

  attach.start_sample = 5000000000ULL;
  attach.end_sample = 5000020000ULL;
  attach.trigger_sample = 5000026000ULL;
  attach.stream_next = 5000030000ULL;
  CHECK(ny_voice_attach_sample(&attach, &trigger, &clamped) ==
        5000020000ULL && !clamped && !trigger, "64 bit positions");
}

/****************************************************************************
 * Idle gate
 ****************************************************************************/

static void test_gate(void)
{
  struct ny_voice_gate_config_s config = { 150, 40, 2000 };
  struct ny_voice_gate_s gate;
  int chunk;
  int closed_at = -1;

  ny_voice_gate_init(&gate, &config);
  for (chunk = 0; chunk < 50; chunk++)
    {
      CHECK(!ny_voice_gate_feed(&gate, 40, 100), "silence opened the gate");
    }

  CHECK(ny_voice_gate_feed(&gate, 3000, 100) && gate.loud, "speech opens");
  for (chunk = 0; chunk < 19; chunk++)
    {
      CHECK(ny_voice_gate_feed(&gate, 40, 100), "hang-over %d", chunk);
    }

  CHECK(!ny_voice_gate_feed(&gate, 40, 100), "closes after the hang-over");

  /* A fan is switched on: open at first, then the floor follows it. */

  for (chunk = 0; chunk < 600; chunk++)
    {
      if (!ny_voice_gate_feed(&gate, 800, 100) && closed_at < 0)
        {
          closed_at = chunk;
        }
    }

  CHECK(closed_at > 20 && closed_at < 300, "steady noise closed at %d",
        closed_at);
  CHECK(!gate.open, "steady noise keeps it closed");
  CHECK(ny_voice_gate_feed(&gate, 4000, 100), "speech over the fan");

  /* Speech must not drag the floor up within a sentence. */

  for (chunk = 0; chunk < 50; chunk++)
    {
      ny_voice_gate_feed(&gate, 4000, 100);
    }

  CHECK(gate.loud, "five seconds of speech still count as speech");

  /* The fan goes off: the floor comes down fast enough to hear a whisper. */

  for (chunk = 0; chunk < 40; chunk++)
    {
      ny_voice_gate_feed(&gate, 30, 100);
    }

  CHECK(ny_voice_gate_feed(&gate, 300, 100), "quiet speech in a quiet room");
  CHECK(ny_voice_gate_feed(&gate, 0xffffffffU, 100), "absurd level");
}

/****************************************************************************
 * Resampler
 ****************************************************************************/

static double tone_gain(struct ny_voice_resample_s *state, double hz)
{
  static float in[44100];
  static float out[17000];
  double sum = 0.0;
  size_t count;
  size_t index;

  for (index = 0; index < COUNT(in); index++)
    {
      in[index] = (float)(0.5 * sin(2.0 * 3.14159265358979 * hz *
                                    (double)index / 44100.0));
    }

  ny_voice_resample_init(state);
  count = ny_voice_resample(state, in, COUNT(in), out, COUNT(out));
  CHECK(count >= 15990 && count <= 16001, "%zu samples out of one second",
        count);
  for (index = 1000; index < count; index++)
    {
      sum += (double)out[index] * out[index];
    }

  return sqrt(sum / (double)(count - 1000)) / (0.5 / sqrt(2.0));
}

static void test_resample(void)
{
  static struct ny_voice_resample_s state;
  static struct ny_voice_resample_s blocks;
  static float in[30000];
  static float whole[12000];
  static float pieces[12000];
  size_t whole_count;
  size_t pieces_count = 0;
  size_t position = 0;
  size_t index;
  double gain;
  uint32_t seed = 12345;

  gain = tone_gain(&state, 1000.0);
  CHECK(fabs(20.0 * log10(gain)) < 0.2, "1 kHz gain %.3f dB",
        20.0 * log10(gain));
  gain = tone_gain(&state, 5000.0);
  CHECK(fabs(20.0 * log10(gain)) < 1.0, "5 kHz gain %.3f dB",
        20.0 * log10(gain));
  gain = tone_gain(&state, 12000.0);
  CHECK(20.0 * log10(gain) < -40.0, "12 kHz leaks %.1f dB",
        20.0 * log10(gain));
  gain = tone_gain(&state, 20000.0);
  CHECK(20.0 * log10(gain) < -40.0, "20 kHz leaks %.1f dB",
        20.0 * log10(gain));

  /* DC stays DC: no ripple at the phase rate. */

  for (index = 0; index < COUNT(in); index++)
    {
      in[index] = 0.25f;
    }

  ny_voice_resample_init(&state);
  whole_count = ny_voice_resample(&state, in, COUNT(in), whole, COUNT(whole));
  for (index = 200; index < whole_count; index++)
    {
      if (fabs(whole[index] - 0.25f) > 1e-4)
        {
          CHECK(false, "DC ripple %f at %zu", (double)whole[index], index);
          break;
        }
    }

  /* Any split of the input gives the very same output. */

  for (index = 0; index < COUNT(in); index++)
    {
      seed = seed * 1664525U + 1013904223U;
      in[index] = (float)((int32_t)(seed >> 8) % 20000) / 32768.0f;
    }

  ny_voice_resample_init(&state);
  whole_count = ny_voice_resample(&state, in, COUNT(in), whole, COUNT(whole));
  ny_voice_resample_init(&blocks);
  while (position < COUNT(in))
    {
      size_t take;

      seed = seed * 1664525U + 1013904223U;
      take = seed % 1500 + 1;
      if (take > COUNT(in) - position)
        {
          take = COUNT(in) - position;
        }

      pieces_count += ny_voice_resample(&blocks, in + position, take,
                                        pieces + pieces_count,
                                        COUNT(pieces) - pieces_count);
      position += take;
    }

  CHECK(pieces_count == whole_count, "%zu samples in pieces, %zu whole",
        pieces_count, whole_count);
  CHECK(memcmp(whole, pieces, whole_count * sizeof(float)) == 0,
        "block boundaries change the output");

  /* An output buffer that is too small loses samples, not alignment. */

  ny_voice_resample_init(&blocks);
  position = ny_voice_resample(&blocks, in, 10000, pieces, COUNT(pieces));
  ny_voice_resample_init(&blocks);
  pieces_count = ny_voice_resample(&blocks, in, 10000, pieces, 100);
  CHECK(pieces_count == 100 && position > 3600, "capacity respected");
  pieces_count = ny_voice_resample(&blocks, in + 10000, 10000, pieces,
                                   COUNT(pieces));
  CHECK(pieces_count > 3600 &&
        memcmp(pieces, whole + position, pieces_count * sizeof(float)) == 0,
        "stream stays aligned after an overflow");
}

/****************************************************************************
 * Sentence chunking
 ****************************************************************************/

static bool utf8_valid(const char *text)
{
  const unsigned char *bytes = (const unsigned char *)text;

  while (*bytes != 0)
    {
      int more = *bytes < 0x80 ? 0 : *bytes >= 0xf0 ? 3 : *bytes >= 0xe0 ? 2
                 : *bytes >= 0xc2 ? 1 : -1;

      if (more < 0)
        {
          return false;
        }

      bytes++;
      while (more-- > 0)
        {
          if ((*bytes++ & 0xc0) != 0x80)
            {
              return false;
            }
        }
    }

  return true;
}

static void expect_pieces(const char *text, size_t size,
                          const char *const *expected, size_t count)
{
  char piece[512];
  size_t position = 0;
  size_t index;

  for (index = 0; index < count; index++)
    {
      size_t length = ny_voice_sentence_next(text, &position, piece, size);

      CHECK(length == strlen(expected[index]) &&
            strcmp(piece, expected[index]) == 0,
            "piece %zu of \"%s\": got \"%s\", want \"%s\"", index, text,
            length != 0 ? piece : "", expected[index]);
    }

  CHECK(ny_voice_sentence_next(text, &position, piece, size) == 0,
        "\"%s\" has more than %zu pieces", text, count);
}

static void test_sentences(void)
{
  static const char *const plain[] =
  {
    "你好，我是星喵。", "忙了一天，辛苦啦。", "要不要休息一会儿？"
  };

  static const char *const mixed[] =
  {
    "Room is 23.5 degrees.", "Pi is 3.14!", "OK?!", "好的……", "再见"
  };

  static const char *const markdown[] =
  {
    "今日天气", "晴，最高 26 度。", "记得带伞"
  };

  static const char *const clauses[] =
  {
    "第一段比较长的话，", "第二段也不短的话，", "第三段。"
  };

  static const char *const uncut[] =
  {
    "一二三四五六七八九", "十一二三四五六七八", "九十。"
  };

  static const char *const dirty[] =
  {
    "坏字节被丢掉。", "后面照常。"
  };

  char piece[64];
  char noise[300];
  size_t position;
  uint32_t seed = 99;
  int round;

  expect_pieces("你好，我是星喵。忙了一天，辛苦啦。要不要休息一会儿？", 200,
                plain, COUNT(plain));
  expect_pieces("Room is 23.5 degrees. Pi is 3.14!  OK?! 好的…… 再见", 200,
                mixed, COUNT(mixed));
  expect_pieces("## 今日天气\n\n**晴**，最高 `26` 度。\r\n- * -\n记得带伞\n",
                200, markdown, COUNT(markdown));

  /* 30 bytes fit nine three-byte characters plus a mark, not more. */

  expect_pieces("第一段比较长的话，第二段也不短的话，第三段。", 30, clauses,
                COUNT(clauses));
  expect_pieces("一二三四五六七八九十一二三四五六七八九十。", 30, uncut,
                COUNT(uncut));
  expect_pieces("坏\xff字节\xe4\xbd被丢掉。\xc0\x80后面照常。", 200, dirty,
                COUNT(dirty));
  expect_pieces("", 200, NULL, 0);
  expect_pieces("。。。 ！？ …… \n\n --- ***", 200, NULL, 0);

  position = 0;
  CHECK(ny_voice_sentence_next("太小的缓冲区", &position, piece, 4) == 0,
        "a buffer nothing fits in");

  /* Whatever comes in, every piece is whole UTF-8, fits, and the walk
   * ends.
   */

  for (round = 0; round < 3000; round++)
    {
      static const char *const alphabet[] =
      {
        "好", "。", "，", "a", " ", ".", "5", "\n", "*", "\xff", "\xe4",
        "！", "😀", "\x01", "?", "、"
      };

      size_t length = 0;
      size_t size;
      size_t pieces = 0;

      noise[0] = '\0';
      while (length < sizeof(noise) - 8)
        {
          const char *part;

          seed = seed * 1664525U + 1013904223U;
          part = alphabet[(seed >> 16) % COUNT(alphabet)];
          strcpy(noise + length, part);
          length += strlen(part);
          if ((seed >> 8) % 97 == 0)
            {
              break;
            }
        }

      seed = seed * 1664525U + 1013904223U;
      size = 8 + (seed >> 16) % 56;
      position = 0;
      for (; ; )
        {
          size_t got = ny_voice_sentence_next(noise, &position, piece, size);

          if (got == 0)
            {
              break;
            }

          pieces++;
          if (got >= size || piece[got] != '\0' || !utf8_valid(piece) ||
              position > length || pieces > 400)
            {
              CHECK(false, "fuzz round %d: piece %zu bytes of %zu", round,
                    got, size);
              break;
            }
        }

      CHECK(position == length, "fuzz round %d stopped at %zu of %zu", round,
            position, length);
    }
}

/****************************************************************************
 * State machine
 ****************************************************************************/

struct step_s
{
  enum ny_voice_event_e type;
  uint32_t after_ms;         /* Time added before the event. */
  bool ok;
  bool empty;
  bool heard;
  uint32_t silence_ms;
  enum ny_voice_state_e state; /* Expected afterwards. */
  const char *actions;         /* Expected, as letters; see action_letter. */
};

static char action_letter(const struct ny_voice_sm_action_s *action)
{
  switch (action->type)
    {
      case NY_VOICE_ACT_ASR_START:
        return action->attach ? 'A' : 'a';

      case NY_VOICE_ACT_ASR_END:
        return 'E';

      case NY_VOICE_ACT_ASR_CANCEL:
        return 'X';

      case NY_VOICE_ACT_AGENT_SUBMIT:
        return 'G';

      case NY_VOICE_ACT_AGENT_CANCEL:
        return 'g';

      case NY_VOICE_ACT_AGENT_FORGET:
        return 'f';

      case NY_VOICE_ACT_SPEAK:
        return "RTNPBe"[action->speech];

      case NY_VOICE_ACT_SPEAK_CANCEL:
        return 'x';

      case NY_VOICE_ACT_EYES:
        return "0123"[action->eyes];

      default:
        return '?';
    }
}

static void run_script(const char *name, const struct step_s *steps,
                       size_t count)
{
  struct ny_voice_sm_config_s config;
  struct ny_voice_sm_s sm;
  uint64_t now = 1000;
  size_t index;

  ny_voice_sm_defaults(&config);
  ny_voice_sm_init(&sm, &config);
  for (index = 0; index < count; index++)
    {
      struct ny_voice_sm_event_s event;
      struct ny_voice_sm_result_s result;
      char got[NY_VOICE_SM_MAX_ACTIONS + 1];
      unsigned int action;

      now += steps[index].after_ms;
      memset(&event, 0, sizeof(event));
      event.type = steps[index].type;
      event.now_ms = now;
      event.ok = steps[index].ok;
      event.empty = steps[index].empty;
      event.heard_speech = steps[index].heard;
      event.silence_ms = steps[index].silence_ms;
      ny_voice_sm_step(&sm, &event, &result);
      for (action = 0; action < result.count; action++)
        {
          got[action] = action_letter(&result.actions[action]);
        }

      got[result.count] = '\0';
      CHECK(sm.state == steps[index].state &&
            strcmp(got, steps[index].actions) == 0,
            "%s step %zu: state %s actions \"%s\", want %s \"%s\"", name,
            index, ny_voice_sm_state_name(sm.state), got,
            ny_voice_sm_state_name(steps[index].state),
            steps[index].actions);
    }
}

#define EV(type, after, state, actions) \
  { type, after, true, false, false, 0, state, actions }
#define TICK(after, heard, silence, state, actions) \
  { NY_VOICE_EV_TICK, after, true, false, heard, silence, state, actions }
#define DONE(type, after, ok, empty, state, actions) \
  { type, after, ok, empty, false, 0, state, actions }

static void test_machine(void)
{
  /* Letters: A/a ASR start (attached / from now), E end, X cancel, G agent
   * submit, g agent cancel, f forget the pending run, x stop speaking,
   * R reply, T voice.say text, N not heard, P approval, B busy, e error,
   * 0..3 eyes restore / curious / processing / happy.
   */

  static const struct step_s turn[] =
  {
    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_OFF, ""),
    EV(NY_VOICE_EV_ENABLE, 0, NY_VOICE_IDLE, ""),
    EV(NY_VOICE_EV_WAKE, 100, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 50, NY_VOICE_LISTENING, ""),
    TICK(1000, true, 0, NY_VOICE_LISTENING, ""),
    TICK(1000, true, 900, NY_VOICE_LISTENING, ""),
    TICK(300, true, 1200, NY_VOICE_LISTENING, "E"),
    TICK(100, true, 1300, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 200, true, false, NY_VOICE_THINKING,
         "2G"),
    TICK(20000, false, 0, NY_VOICE_THINKING, ""),
    DONE(NY_VOICE_EV_AGENT_REPLY, 5000, true, false, NY_VOICE_SPEAKING,
         "3R"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 6000, true, false, NY_VOICE_IDLE, "0"),
  };

  static const struct step_s endpoint[] =
  {
    EV(NY_VOICE_EV_ENABLE, 0, NY_VOICE_IDLE, ""),
    EV(NY_VOICE_EV_LISTEN, 0, NY_VOICE_LISTENING, "1a"),

    /* The decoder's opinion arrives before the request is even confirmed:
     * END waits for the request, and is sent once.
     */

    EV(NY_VOICE_EV_ASR_ENDPOINT, 10, NY_VOICE_LISTENING, ""),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, "E"),
    EV(NY_VOICE_EV_ASR_ENDPOINT, 10, NY_VOICE_LISTENING, ""),
    TICK(100, true, 5000, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 100, true, true, NY_VOICE_SPEAKING, "N"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, true, false, NY_VOICE_IDLE, "0"),
  };

  static const struct step_s limits[] =
  {
    EV(NY_VOICE_EV_ENABLE, 0, NY_VOICE_IDLE, ""),

    /* Nothing is said at all. */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 2000, NY_VOICE_LISTENING, ""),
    TICK(4900, false, 4900, NY_VOICE_LISTENING, ""),
    TICK(100, false, 5000, NY_VOICE_LISTENING, "E"),
    DONE(NY_VOICE_EV_ASR_FINISHED, 300, true, true, NY_VOICE_SPEAKING, "N"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, true, false, NY_VOICE_IDLE, "0"),

    /* Someone who never stops talking: eight seconds from ASR start, not
     * from the wake word, because the model load came in between.
     */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    TICK(7000, false, 0, NY_VOICE_LISTENING, ""),
    EV(NY_VOICE_EV_ASR_STARTED, 1000, NY_VOICE_LISTENING, ""),
    TICK(7900, true, 0, NY_VOICE_LISTENING, ""),
    TICK(100, true, 0, NY_VOICE_LISTENING, "E"),

    /* ... and FINISH never comes. */

    TICK(5900, true, 0, NY_VOICE_LISTENING, ""),
    TICK(100, true, 0, NY_VOICE_SPEAKING, "XN"),
    DONE(NY_VOICE_EV_ASR_FINISHED, 10, true, false, NY_VOICE_SPEAKING, ""),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, true, false, NY_VOICE_IDLE, "0"),

    /* The ASR model never loads. */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    TICK(89999, false, 0, NY_VOICE_LISTENING, ""),
    TICK(1, false, 0, NY_VOICE_SPEAKING, "Xe"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, false, false, NY_VOICE_IDLE, "0"),

    /* ... or says so itself. */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_FAILED, 500, NY_VOICE_SPEAKING, "e"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, true, false, NY_VOICE_IDLE, "0"),

    /* The agent takes too long; playback that never ends. */

    EV(NY_VOICE_EV_LISTEN, 0, NY_VOICE_LISTENING, "1a"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    TICK(119999, false, 0, NY_VOICE_THINKING, ""),
    TICK(1, false, 0, NY_VOICE_SPEAKING, "ge"),
    TICK(179999, false, 0, NY_VOICE_SPEAKING, ""),
    TICK(1, false, 0, NY_VOICE_IDLE, "x0"),
  };

  static const struct step_s approval[] =
  {
    EV(NY_VOICE_EV_ENABLE, 0, NY_VOICE_IDLE, ""),
    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_AGENT_PENDING, 2000, NY_VOICE_SPEAKING, "3P"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 4000, true, false, NY_VOICE_IDLE, "0"),

    /* The owner approves on the panel a minute later: the outcome is
     * spoken although no turn is open.
     */

    TICK(60000, false, 0, NY_VOICE_IDLE, ""),
    DONE(NY_VOICE_EV_AGENT_REPLY, 100, true, false, NY_VOICE_SPEAKING, "3R"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 3000, true, false, NY_VOICE_IDLE, "0"),

    /* A reply nobody waits for means nothing. */

    DONE(NY_VOICE_EV_AGENT_REPLY, 100, true, false, NY_VOICE_IDLE, ""),

    /* Pending again, but the owner never decides ... */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_AGENT_PENDING, 2000, NY_VOICE_SPEAKING, "3P"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 4000, true, false, NY_VOICE_IDLE, "0"),
    TICK(595999, false, 0, NY_VOICE_IDLE, ""),
    TICK(1, false, 0, NY_VOICE_IDLE, "f"),
    TICK(1000, false, 0, NY_VOICE_IDLE, ""),

    /* ... or asks something else first: the old run is no longer followed,
     * and the agent, still held by it, says it is busy.
     */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_AGENT_PENDING, 2000, NY_VOICE_SPEAKING, "3P"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 4000, true, false, NY_VOICE_IDLE, "0"),
    EV(NY_VOICE_EV_WAKE, 1000, NY_VOICE_LISTENING, "f1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_AGENT_BUSY, 100, NY_VOICE_SPEAKING, "B"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, true, false, NY_VOICE_IDLE, "0"),

    /* The followed run ends while its own approval sentence plays. */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_AGENT_PENDING, 2000, NY_VOICE_SPEAKING, "3P"),
    DONE(NY_VOICE_EV_AGENT_REPLY, 500, true, false, NY_VOICE_SPEAKING, ""),
    DONE(NY_VOICE_EV_SPEAK_DONE, 4000, true, false, NY_VOICE_IDLE, "0"),
    TICK(700000, false, 0, NY_VOICE_IDLE, ""),
  };

  static const struct step_s barge[] =
  {
    EV(NY_VOICE_EV_ENABLE, 0, NY_VOICE_IDLE, ""),
    EV(NY_VOICE_EV_SAY, 0, NY_VOICE_SPEAKING, "3T"),
    EV(NY_VOICE_EV_SAY, 10, NY_VOICE_SPEAKING, ""),

    /* A wake word on top of the reply. */

    EV(NY_VOICE_EV_WAKE, 2000, NY_VOICE_LISTENING, "x1A"),

    /* The cancelled playback reports in late; it is not this state's. */

    DONE(NY_VOICE_EV_SPEAK_DONE, 50, false, false, NY_VOICE_LISTENING, ""),
    EV(NY_VOICE_EV_WAKE, 10, NY_VOICE_LISTENING, ""),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),

    /* Asking again instead of waiting for the answer. */

    EV(NY_VOICE_EV_LISTEN, 1000, NY_VOICE_LISTENING, "g1a"),
    EV(NY_VOICE_EV_CANCEL, 500, NY_VOICE_IDLE, "X0"),
    EV(NY_VOICE_EV_CANCEL, 10, NY_VOICE_IDLE, ""),

    /* voice.cancel in every other state. */

    EV(NY_VOICE_EV_LISTEN, 0, NY_VOICE_LISTENING, "1a"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_CANCEL, 500, NY_VOICE_IDLE, "g0"),
    EV(NY_VOICE_EV_SAY, 0, NY_VOICE_SPEAKING, "3T"),
    EV(NY_VOICE_EV_CANCEL, 500, NY_VOICE_IDLE, "x0"),

    /* voice.enable false in the middle of a reply, with a run pending. */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_AGENT_PENDING, 2000, NY_VOICE_SPEAKING, "3P"),
    EV(NY_VOICE_EV_DISABLE, 100, NY_VOICE_OFF, "xf0"),
    EV(NY_VOICE_EV_DISABLE, 100, NY_VOICE_OFF, ""),
    EV(NY_VOICE_EV_SAY, 100, NY_VOICE_OFF, ""),
    TICK(100, false, 0, NY_VOICE_OFF, ""),
    EV(NY_VOICE_EV_ENABLE, 100, NY_VOICE_IDLE, ""),
    EV(NY_VOICE_EV_DISABLE, 100, NY_VOICE_OFF, ""),
  };

  static const struct step_s restart[] =
  {
    EV(NY_VOICE_EV_ENABLE, 0, NY_VOICE_IDLE, ""),
    EV(NY_VOICE_EV_LINK_LOST, 10, NY_VOICE_IDLE, ""),

    /* The compute domain restarts while the command is being heard ... */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    EV(NY_VOICE_EV_LINK_LOST, 1500, NY_VOICE_IDLE, "0"),
    DONE(NY_VOICE_EV_ASR_FINISHED, 10, false, true, NY_VOICE_IDLE, ""),

    /* ... while the agent thinks: a cloud model is not affected, and the
     * on-device one reports its own failure as the run's outcome ...
     */

    EV(NY_VOICE_EV_WAKE, 0, NY_VOICE_LISTENING, "1A"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_LINK_LOST, 1000, NY_VOICE_THINKING, ""),
    DONE(NY_VOICE_EV_AGENT_REPLY, 1000, false, false, NY_VOICE_SPEAKING,
         "e"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, true, false, NY_VOICE_IDLE, "0"),

    /* ... and while the reply is spoken: the window in the shared arena is
     * void, playback stops at once.
     */

    EV(NY_VOICE_EV_SAY, 0, NY_VOICE_SPEAKING, "3T"),
    EV(NY_VOICE_EV_LINK_LOST, 700, NY_VOICE_IDLE, "x0"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 50, false, false, NY_VOICE_IDLE, ""),

    /* An empty reply is an error cue, not silence. */

    EV(NY_VOICE_EV_LISTEN, 0, NY_VOICE_LISTENING, "1a"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    DONE(NY_VOICE_EV_AGENT_REPLY, 1000, true, true, NY_VOICE_SPEAKING, "e"),
    DONE(NY_VOICE_EV_SPEAK_DONE, 900, true, false, NY_VOICE_IDLE, "0"),
    EV(NY_VOICE_EV_LISTEN, 0, NY_VOICE_LISTENING, "1a"),
    EV(NY_VOICE_EV_ASR_STARTED, 10, NY_VOICE_LISTENING, ""),
    DONE(NY_VOICE_EV_ASR_FINISHED, 3000, true, false, NY_VOICE_THINKING,
         "2G"),
    EV(NY_VOICE_EV_AGENT_FAILED, 10, NY_VOICE_SPEAKING, "e"),
  };

  struct ny_voice_sm_config_s config;
  struct ny_voice_sm_s sm;
  struct ny_voice_sm_event_s event;
  struct ny_voice_sm_result_s result;
  uint32_t seed = 7;
  int round;

  run_script("turn", turn, COUNT(turn));
  run_script("endpoint", endpoint, COUNT(endpoint));
  run_script("limits", limits, COUNT(limits));
  run_script("approval", approval, COUNT(approval));
  run_script("barge-in", barge, COUNT(barge));
  run_script("restart", restart, COUNT(restart));

  /* Microphone policy per state. */

  ny_voice_sm_defaults(&config);
  ny_voice_sm_init(&sm, &config);
  CHECK(!ny_voice_sm_streaming(&sm, true, true), "off streams nothing");
  sm.state = NY_VOICE_IDLE;
  CHECK(ny_voice_sm_streaming(&sm, false, false), "idle streams");
  sm.state = NY_VOICE_THINKING;
  CHECK(ny_voice_sm_streaming(&sm, false, false), "thinking streams");
  sm.state = NY_VOICE_SPEAKING;
  CHECK(!ny_voice_sm_streaming(&sm, false, true), "half duplex is deaf");
  CHECK(!ny_voice_sm_streaming(&sm, true, false), "no barge-in, no stream");
  CHECK(ny_voice_sm_streaming(&sm, true, true), "barge-in streams");

  /* Random event storms: the machine stays inside its states and never
   * emits more actions than the result can hold.
   */

  ny_voice_sm_init(&sm, &config);
  memset(&event, 0, sizeof(event));
  for (round = 0; round < 200000; round++)
    {
      seed = seed * 1664525U + 1013904223U;
      event.type = (enum ny_voice_event_e)((seed >> 16) % 17);
      event.ok = (seed >> 8 & 1) != 0;
      event.empty = (seed >> 9 & 1) != 0;
      event.heard_speech = (seed >> 10 & 1) != 0;
      event.silence_ms = (seed >> 11) % 3000;
      event.now_ms += (seed >> 20) % 4000;
      ny_voice_sm_step(&sm, &event, &result);
      if (sm.state > NY_VOICE_SPEAKING ||
          result.count > NY_VOICE_SM_MAX_ACTIONS ||
          (sm.state == NY_VOICE_OFF && sm.pending))
        {
          CHECK(false, "storm round %d: state %d, %u actions", round,
                (int)sm.state, result.count);
          break;
        }
    }

  CHECK(round == 200000, "storm ended early");
}

int main(void)
{
  test_formats();
  test_ring();
  test_attach();
  test_gate();
  test_resample();
  test_sentences();
  test_machine();
  printf("voice_test: %lu checks, %lu failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
