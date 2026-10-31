/****************************************************************************
 * tools/amp/nyampd/nyampd_tts_test.cpp
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

/* The TTS service through the wire.  The vocoder is scripted -- the real
 * one needs the NPU -- and so, in most groups, is the text front end, which
 * keeps the expected unit boundaries exact.  What is real is everything the
 * control domain can observe: chunked text in, PCM windows through a slot,
 * one window outstanding at a time, leases, cancel, and the rule that audio
 * is never truncated.
 *
 * Given NYAMP_G2P_ASSETS=<dir with tokens.txt, lexicon.txt, dict/> the last
 * group runs the real front end (tools/amp/g2p) in front of the scripted
 * vocoder; NYAMP_G2P_GOLDEN=<dir with X.BIN> additionally requires the ids
 * of the reference sentence to match the fixture bit for bit.
 */

#include "nyampd_provision.h"
#include "nyampd_speech_test_support.h"
#include "nyampd_test_support.h"

#include <cstdlib>
#include <fstream>

namespace
{

using namespace nyamp::testing;

constexpr std::uint16_t kTts = NYAMP_SERVICE_TTS;

struct Window
{
  nyamp_buffer_s buffer;
  std::uint32_t sequence;
  std::uint32_t valid_samples;
  float first_sample;
  bool content_ok;
};

struct Bench
{
  explicit Bench(bool with_backend = true, bool real_frontend = false)
      : slot(kSharedOffset, kSharedSize), mint(kSpeechGeneration),
        tts(kSpeechGeneration, [this] { return clock_ms.load(); },
            with_backend
                ? nyamp::TtsService::BackendFactory([this] {
                    return std::make_unique<ScriptedVocoder>(&script);
                  })
                : nyamp::TtsService::BackendFactory(),
            real_frontend
                ? nyamp::TtsService::FrontendFactory(&nyamp::CreateMeloFrontend)
                : nyamp::TtsService::FrontendFactory([this] {
                    return std::make_unique<ScriptedFrontend>(&script);
                  }),
            &slot, &gate, &mint),
        client(nyamp::SpeechServices{ nullptr, &tts, nullptr })
  {
  }

  std::int32_t Load(const std::string &path = "/models/tts")
  {
    std::int32_t status = 1;
    return client.Call(kTts, NYAMP_TTS_LOAD, next_id++,
                       reinterpret_cast<const std::uint8_t *>(path.data()),
                       path.size(), &status)
               ? status
               : 1;
  }

  std::int32_t Chunk(std::uint64_t id, const std::string &text,
                     std::size_t offset, std::size_t length, float speed,
                     std::uint32_t window_samples)
  {
    nyamp_tts_text_s chunk{};
    std::uint8_t payload[NYAMP_INLINE_MAX];
    std::size_t size = 0;
    std::int32_t status = 1;

    chunk.total = static_cast<std::uint32_t>(text.size());
    chunk.offset = static_cast<std::uint32_t>(offset);
    chunk.length = static_cast<std::uint32_t>(length);
    chunk.speaker_id = 1;
    chunk.speed = speed;
    chunk.window_samples = window_samples;
    if (nyamp_tts_text_encode(
            payload, sizeof(payload), &size, &chunk,
            reinterpret_cast<const std::uint8_t *>(text.data()) + offset) !=
            NYAMP_OK ||
        !client.Call(kTts, NYAMP_TTS_SYNTH_TEXT, id, payload, size, &status))
      {
        return 1;
      }

    return status;
  }

  /* The whole text, in chunks of at most `step` bytes; the status of the
   * last chunk, or of the first one that was refused.
   */

  std::int32_t Say(std::uint64_t id, const std::string &text,
                   float speed = 1.0f, std::uint32_t window_samples = 0,
                   std::size_t step = NYAMP_TTS_TEXT_MAX_CHUNK)
  {
    std::int32_t status = NYAMP_MODEL_INVALID;

    for (std::size_t offset = 0; offset < text.size(); offset += step)
      {
        status = Chunk(id, text, offset, std::min(step, text.size() - offset),
                       speed, window_samples);
        if (status != NYAMP_MODEL_OK)
          {
            break;
          }
      }

    return status;
  }

  std::int32_t Release(std::uint64_t id, const nyamp_buffer_s &window)
  {
    std::uint8_t payload[NYAMP_BUFFER_SIZE];
    std::size_t size = 0;
    std::int32_t status = 1;

    if (nyamp_buffer_encode(payload, sizeof(payload), &size, &window) !=
            NYAMP_OK ||
        !client.Call(kTts, NYAMP_TTS_RELEASE, id, payload, size, &status))
      {
        return 1;
      }

    return status;
  }

  std::int32_t Simple(std::uint16_t opcode, std::uint64_t id)
  {
    std::int32_t status = 1;
    return client.Call(kTts, opcode, id, nullptr, 0, &status) ? status : 1;
  }

  /* Play the control domain's part until FINISH: read every window out of
   * the slot, check it, release it.  `on_window` may return false to stop
   * releasing (the caller then owns the rest of the conversation).
   */

  struct Outcome
  {
    std::vector<Window> windows;
    bool finished = false;
    bool malformed = false;
    std::int32_t status = 1;
    std::uint32_t finish_sequence = 0;
    std::uint32_t total_samples = 0;
    std::uint64_t pcm_samples = 0;
  };

  bool Listen(std::uint64_t id, Outcome *outcome, int timeout_ms = 20000,
              const std::function<bool(const Window &)> &on_window = {})
  {
    return client.Pump(
        [&] { return outcome->finished; }, timeout_ms,
        [&](const WireEvent &event) {
          if (event.header.service != kTts || event.header.request_id != id ||
              (event.header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_EVENT)
            {
              return;
            }

          if (event.header.opcode == NYAMP_TTS_EVENT_FINISH)
            {
              outcome->malformed =
                  outcome->malformed || outcome->finished ||
                  nyamp_tts_finish_decode(&outcome->status,
                                          &outcome->finish_sequence,
                                          &outcome->total_samples,
                                          event.payload.data(),
                                          event.payload.size()) != NYAMP_OK;
              outcome->finished = true;
              return;
            }

          if (event.header.opcode != NYAMP_TTS_EVENT_PCM)
            {
              return;
            }

          Window window{};
          std::uint32_t rate = 0;
          std::uint32_t channels = 0;

          if (nyamp_tts_pcm_decode(&window.buffer, &window.sequence, &rate,
                                   &channels, &window.valid_samples,
                                   event.payload.data(),
                                   event.payload.size()) != NYAMP_OK ||
              outcome->finished || rate != 44100 || channels != 1 ||
              window.sequence != outcome->windows.size() ||
              window.buffer.format != NYAMP_FORMAT_F32 ||
              window.buffer.length != window.valid_samples * 4 ||
              window.buffer.offset != kSharedOffset ||
              window.buffer.length > kSharedSize ||
              window.buffer.generation != kSpeechGeneration ||
              (window.buffer.flags &
               (NYAMP_BUFFER_IN_SHMEM | NYAMP_BUFFER_FROM_COMPUTE)) !=
                  (NYAMP_BUFFER_IN_SHMEM | NYAMP_BUFFER_FROM_COMPUTE))
            {
              outcome->malformed = true;
              return;
            }

          /* Read the samples the way the control domain would, and check
           * they are this unit's, at this offset into it.
           */
          const float *samples =
              reinterpret_cast<const float *>(slot.at(window.buffer.offset));
          if ((window.buffer.flags & NYAMP_BUFFER_RESYNC) != 0)
            {
              unit_offset = 0;
            }

          window.first_sample = samples[0];
          window.content_ok = true;
          for (std::uint32_t index = unit_offset == 0 ? 1 : 0;
               index < window.valid_samples; ++index)
            {
              if (samples[index] != UnitSample(0, unit_offset + index))
                {
                  window.content_ok = false;
                }
            }

          unit_offset += window.valid_samples;
          outcome->pcm_samples += window.valid_samples;
          outcome->windows.push_back(window);

          if (!on_window || on_window(window))
            {
              if (Release(id, window.buffer) != NYAMP_MODEL_OK)
                {
                  outcome->malformed = true;
                }
            }
        });
  }

  MemorySlot slot;
  nyamp::AtomicGate gate;
  nyamp::LeaseMint mint;
  TtsScript script;
  std::atomic<std::uint64_t> clock_ms{ 1000 };
  nyamp::TtsService tts;
  WireClient client;
  std::uint64_t next_id = 0x9000;
  std::size_t unit_offset = 0;
};

std::size_t UnitSamples(const std::string &unit, double frames_per_phoneme = 8,
                        double speed = 1.0)
{
  return static_cast<std::size_t>(static_cast<double>(unit.size()) *
                                      frames_per_phoneme / speed +
                                  1e-6) *
         512;
}

/****************************************************************************
 * Tests
 ****************************************************************************/

int TestAvailability()
{
  Bench bare(false);
  std::int32_t status = 1;
  std::vector<std::uint8_t> body;

  CHECK(bare.client.Call(NYAMP_SERVICE_HEALTH, 1, 1, nullptr, 0, &status,
                         &body));
  CHECK(body.size() == 8 && (body[4] & (1U << 5)) == 0);
  CHECK(bare.Load() == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bare.Say(5, "hello.") == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bare.tts.Info() == "none:-2");

  Bench bench;
  CHECK(bench.client.Call(NYAMP_SERVICE_HEALTH, 1, 2, nullptr, 0, &status,
                          &body));
  CHECK((body[4] & (1U << 5)) != 0);
  CHECK(bench.Say(10, "hello.") == NYAMP_MODEL_NOT_READY);

  bench.script.fail_load = true;
  CHECK(bench.Load() == NYAMP_MODEL_BACKEND_ERROR);
  CHECK(bench.tts.Info() == "off:-8");
  bench.script.fail_load = false;
  CHECK(bench.Load() == NYAMP_MODEL_OK);
  CHECK(bench.script.loaded_directory == "/models/tts");
  CHECK(bench.tts.Info() == "ready:0");

  /* SYNTH takes ids from the control domain, which has no front end to
   * make them; it is refused, not half served.
   */
  std::uint8_t payload[NYAMP_TTS_SYNTH_SIZE];
  std::size_t size = 0;
  CHECK(nyamp_tts_synth_encode(payload, sizeof(payload), &size, 95, 1, 1.0f,
                               512) == NYAMP_OK);
  CHECK(bench.client.Call(kTts, NYAMP_TTS_SYNTH, 11, payload, size, &status));
  CHECK(status == NYAMP_MODEL_UNSUPPORTED);

  CHECK(bench.Simple(NYAMP_TTS_UNLOAD, 12) == NYAMP_MODEL_OK);
  CHECK(bench.Simple(NYAMP_TTS_UNLOAD, 13) == NYAMP_MODEL_INVALID);
  return 0;
}

int TestSentencesAndWindows()
{
  Bench bench;
  Bench::Outcome outcome;
  const std::uint64_t id = 0x1000000000000123ULL;

  /* Three sentences; the text is longer than one 428-byte chunk. */
  const std::string one = "the first sentence is a short one.";
  const std::string two = " and the second one follows it closely.";
  std::string three = " third";
  while (three.size() < 60)
    {
      three += " x";
    }

  three += ".";
  std::string text = one + two + three;
  std::string filler;
  while (text.size() + filler.size() < 500)
    {
      filler += " pad pad pad pad pad pad pad pad pad pad pad pad pad.";
    }

  text += filler;
  CHECK(text.size() > NYAMP_TTS_TEXT_MAX_CHUNK);

  CHECK(bench.Load() == NYAMP_MODEL_OK);
  CHECK(bench.Say(id, text) == NYAMP_MODEL_OK);
  CHECK(bench.gate.held());
  CHECK(bench.Listen(id, &outcome));

  CHECK(!outcome.malformed && outcome.status == NYAMP_MODEL_OK);
  CHECK(outcome.finish_sequence == outcome.windows.size());
  CHECK(outcome.total_samples == outcome.pcm_samples);
  CHECK(!bench.gate.held());

  /* One unit per sentence, in order, nothing dropped. */
  CHECK(bench.script.units.size() >= 4);
  CHECK(bench.script.units[0] == one);
  CHECK(bench.script.units[1] == two.substr(1));
  CHECK(bench.script.units[2] == three.substr(1));

  std::size_t expected = 0;
  for (const std::string &unit : bench.script.units)
    {
      const std::size_t samples = UnitSamples(unit);

      /* No unit is longer than the vocoder's bucket. */
      CHECK(samples <= 512 * 512);
      expected += samples;
    }

  CHECK(outcome.total_samples == expected);

  /* Windows: one second each except the tail of a unit; RESYNC opens every
   * unit, LAST closes the request and nothing else; the samples are the
   * unit's own, at the right offset.
   */
  std::size_t unit = 0;
  std::size_t in_unit = 0;
  for (std::size_t index = 0; index < outcome.windows.size(); ++index)
    {
      const Window &window = outcome.windows[index];
      const bool resync = (window.buffer.flags & NYAMP_BUFFER_RESYNC) != 0;
      const bool last = (window.buffer.flags & NYAMP_BUFFER_LAST) != 0;

      CHECK(window.content_ok);
      CHECK(resync == (in_unit == 0));
      if (resync)
        {
          CHECK(window.first_sample ==
                UnitSample(bench.script.units[unit][0], 0));
        }

      const std::size_t left = UnitSamples(bench.script.units[unit]) - in_unit;
      CHECK(window.valid_samples == std::min<std::size_t>(44100, left));
      in_unit += window.valid_samples;
      if (in_unit == UnitSamples(bench.script.units[unit]))
        {
          ++unit;
          in_unit = 0;
        }

      CHECK(last == (index + 1 == outcome.windows.size()));
    }

  CHECK(unit == bench.script.units.size());

  /* Every window had a lease of its own. */
  for (std::size_t index = 1; index < outcome.windows.size(); ++index)
    {
      CHECK(outcome.windows[index].buffer.lease !=
            outcome.windows[index - 1].buffer.lease);
    }

  return 0;
}

int TestHeldWindowAndLeases()
{
  Bench bench;
  Bench::Outcome outcome;
  const std::uint64_t id = 0x2002;
  bool checked = false;
  bool ok = true;

  CHECK(bench.Load() == NYAMP_MODEL_OK);
  CHECK(bench.Say(id, "a sentence of some length to fill a few windows.") ==
        NYAMP_MODEL_OK);

  CHECK(bench.Listen(id, &outcome, 20000, [&](const Window &window) {
    if (window.sequence != 1)
      {
        return true;
      }

    /* Window 1 is out and unreleased: the service must not write the slot
     * or publish window 2, however long the control domain takes.
     */
    std::vector<std::uint8_t> before(
        bench.slot.at(kSharedOffset),
        bench.slot.at(kSharedOffset) + window.buffer.length);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    bench.client.Drain();
    for (const WireEvent &event : bench.client.events)
      {
        std::uint32_t sequence = 0;
        std::uint32_t rate = 0;
        std::uint32_t channels = 0;
        std::uint32_t valid = 0;
        nyamp_buffer_s buffer{};
        if (event.header.opcode == NYAMP_TTS_EVENT_PCM &&
            nyamp_tts_pcm_decode(&buffer, &sequence, &rate, &channels, &valid,
                                 event.payload.data(),
                                 event.payload.size()) == NYAMP_OK &&
            sequence > 1)
          {
            ok = false;
          }
      }

    ok = ok && std::memcmp(before.data(), bench.slot.at(kSharedOffset),
                           before.size()) == 0;

    /* A late echo of window 0, a forged lease, another request id: none of
     * them frees window 1.
     */
    ok = ok && bench.Release(id, outcome.windows[0].buffer) ==
                   NYAMP_MODEL_INVALID;
    nyamp_buffer_s forged = window.buffer;
    forged.lease += 1;
    ok = ok && bench.Release(id, forged) == NYAMP_MODEL_INVALID;
    ok = ok && bench.Release(id + 1, window.buffer) == NYAMP_MODEL_INVALID;
    checked = true;
    return true; /* Now release it properly. */
  }));

  CHECK(checked && ok);
  CHECK(!outcome.malformed && outcome.status == NYAMP_MODEL_OK);
  CHECK(outcome.windows.size() >= 3);

  /* After FINISH nothing is outstanding. */
  CHECK(bench.Release(id, outcome.windows.back().buffer) ==
        NYAMP_MODEL_INVALID);
  return 0;
}

int TestNeverTruncated()
{
  Bench bench;
  Bench::Outcome outcome;
  const std::uint64_t id = 0x3001;

  /* The front end believes a phoneme costs 4 frames; it costs 8.  Its
   * units come out up to twice the bucket, the vocoder refuses them, and
   * the service splits them again instead of clipping them.
   */
  bench.script.believed_frames_per_phoneme = 4;

  std::string text;
  for (int word = 0; word < 40; ++word)
    {
      text += word == 0 ? "w00" : " w" + std::to_string(10 + word);
    }

  text += "."; /* One 160-byte sentence: 1280 frames if said in one go. */

  CHECK(bench.Load() == NYAMP_MODEL_OK);
  CHECK(bench.Say(id, text) == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id, &outcome));
  CHECK(!outcome.malformed && outcome.status == NYAMP_MODEL_OK);
  CHECK(bench.script.refused > 0);

  /* Every word was spoken exactly once, in order, and every sample of
   * every unit was delivered.
   */
  std::string spoken;
  std::size_t expected = 0;
  for (const std::string &unit : bench.script.units)
    {
      spoken += (spoken.empty() ? "" : " ") + unit;
      expected += UnitSamples(unit);
      CHECK(UnitSamples(unit) <= 512 * 512);
    }

  CHECK(spoken == text);
  CHECK(outcome.total_samples == expected);
  CHECK(outcome.total_samples == text.size() * 8 * 512 -
                                     (bench.script.units.size() - 1) * 8 * 512);

  /* Text that cannot be split ends the request with UNSUPPORTED -- after
   * the part that could be said, and without a clipped window.
   */
  Bench::Outcome refused;
  const std::string word(100, 'z');
  bench.script.units.clear();
  bench.script.believed_frames_per_phoneme = 8;
  CHECK(bench.Say(id + 1, "fine. " + word + ". never reached.") ==
        NYAMP_MODEL_OK);
  CHECK(bench.Listen(id + 1, &refused));
  CHECK(!refused.malformed && refused.status == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bench.script.units.size() == 1 && bench.script.units[0] == "fine.");
  CHECK(refused.total_samples == UnitSamples("fine."));
  CHECK(!bench.gate.held());

  /* The same when only the vocoder finds out. */
  Bench::Outcome late;
  bench.script.units.clear();
  bench.script.believed_frames_per_phoneme = 1;
  CHECK(bench.Say(id + 2, word + ".") == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id + 2, &late));
  CHECK(late.status == NYAMP_MODEL_UNSUPPORTED && late.windows.empty());
  CHECK(late.total_samples == 0 && bench.script.units.empty());

  /* Nothing pronounceable is not an error: no audio, a clean finish. */
  Bench::Outcome empty;
  bench.script.believed_frames_per_phoneme = 8;
  CHECK(bench.Say(id + 3, "   ") == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id + 3, &empty));
  CHECK(empty.status == NYAMP_MODEL_OK && empty.windows.empty());
  return 0;
}

int TestSpeedAndWindowSize()
{
  Bench bench;
  const std::uint64_t id = 0x4001;
  const std::string text = "w01 w02 w03 w04 w05 w06 w07 w08 w09 w10 w11 w12.";

  CHECK(bench.Load() == NYAMP_MODEL_OK);

  /* 48 bytes at 8 frames is 384 frames: one unit at normal speed... */
  Bench::Outcome normal;
  CHECK(bench.Say(id, text, 1.0f, 262144) == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id, &normal));
  CHECK(normal.status == NYAMP_MODEL_OK && bench.script.units.size() == 1);

  /* ...delivered as ONE window when the control domain asks for the whole
   * slot: 196608 samples, 786432 bytes.
   */
  CHECK(normal.windows.size() == 1);
  CHECK(normal.windows[0].valid_samples == 384 * 512);
  CHECK((normal.windows[0].buffer.flags &
         (NYAMP_BUFFER_RESYNC | NYAMP_BUFFER_LAST)) ==
        (NYAMP_BUFFER_RESYNC | NYAMP_BUFFER_LAST));

  /* At half speed the same text needs 768 frames, so the budget halves and
   * it becomes two units -- planned, not discovered by a refusal.
   */
  Bench::Outcome slow;
  bench.script.units.clear();
  const unsigned int refused = bench.script.refused;
  CHECK(bench.Say(id + 1, text, 0.5f, 4410) == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id + 1, &slow));
  CHECK(slow.status == NYAMP_MODEL_OK && !slow.malformed);
  CHECK(bench.script.units.size() == 2 && bench.script.refused == refused);
  CHECK(slow.total_samples == 2 * normal.total_samples - 2 * 8 * 512);

  /* 0.1 s windows. */
  for (const Window &window : slow.windows)
    {
      CHECK(window.valid_samples <= 4410 && window.content_ok);
    }

  /* Speeds outside 0.5 .. 2.0 are refused. */
  CHECK(bench.Say(id + 2, text, 0.25f) == NYAMP_MODEL_INVALID);
  CHECK(bench.Say(id + 3, text, 4.0f) == NYAMP_MODEL_INVALID);
  return 0;
}

int TestCancel()
{
  Bench bench;
  const std::uint64_t id = 0x5001;
  const std::string text =
      "sentence number one is here. sentence number two is here. and three.";

  CHECK(bench.Load() == NYAMP_MODEL_OK);

  /* At window granularity: cancel while window 0 is out.  The window is
   * void from then on and its late release is refused.
   */
  Bench::Outcome first;
  nyamp_buffer_s kept{};
  CHECK(bench.Say(id, text) == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id, &first, 20000, [&](const Window &window) {
    kept = window.buffer;
    return bench.Simple(NYAMP_TTS_CANCEL, id) != NYAMP_MODEL_OK;
  }));
  CHECK(first.status == NYAMP_MODEL_CANCELLED && first.windows.size() == 1);
  CHECK(first.finish_sequence == 1);
  CHECK(bench.script.units.size() == 1); /* Sentence two was never made. */
  CHECK(!bench.gate.held());
  CHECK(bench.Release(id, kept) == NYAMP_MODEL_INVALID);
  CHECK(bench.Simple(NYAMP_TTS_CANCEL, id) == NYAMP_MODEL_NOT_READY);

  /* Between sentences: the cancel lands while a synthesis is running; its
   * audio is never published.
   */
  Bench::Outcome second;
  bench.script.units.clear();
  bench.script.synth_delay_ms = 150;
  CHECK(bench.Say(id + 1, text) == NYAMP_MODEL_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  CHECK(bench.client.CancelFrame(kTts, id + 1));
  CHECK(bench.Listen(id + 1, &second));
  CHECK(second.status == NYAMP_MODEL_CANCELLED && second.windows.empty());
  bench.script.synth_delay_ms = 0;

  /* A half-assembled text is dropped by a cancel, and by a new first
   * chunk.
   */
  const std::string longer(600, 'a');
  CHECK(bench.Chunk(id + 2, longer, 0, 400, 1.0f, 0) == NYAMP_MODEL_OK);
  CHECK(bench.Simple(NYAMP_TTS_CANCEL, id + 2) == NYAMP_MODEL_OK);
  CHECK(bench.Chunk(id + 2, longer, 400, 200, 1.0f, 0) == NYAMP_MODEL_INVALID);

  /* UNLOAD ends a running request: BUSY now, OK after its FINISH. */
  Bench::Outcome third;
  bool unloaded_early = false;
  CHECK(bench.Say(id + 3, text) == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id + 3, &third, 20000, [&](const Window &) {
    unloaded_early = bench.Simple(NYAMP_TTS_UNLOAD, 0x77) != NYAMP_MODEL_BUSY;
    return false;
  }));
  CHECK(!unloaded_early && third.status == NYAMP_MODEL_CANCELLED);
  CHECK(bench.Simple(NYAMP_TTS_UNLOAD, 0x78) == NYAMP_MODEL_OK);
  return 0;
}

int TestSlotSharing()
{
  /* The real arrangement: NYAMP_SLOT_SHARED belongs to whoever holds the
   * blob client, a model pull or a synthesis, never both.
   */
  Rig rig(4096);
  nyamp::BlobGate gate(rig.client.get());
  MemorySlot slot(kSharedOffset, kSharedSize);
  nyamp::LeaseMint mint(kSpeechGeneration);
  TtsScript script;
  nyamp::TtsService tts(
      kSpeechGeneration, [] { return std::uint64_t(0); },
      [&] { return std::make_unique<ScriptedVocoder>(&script); },
      [&] { return std::make_unique<ScriptedFrontend>(&script); }, &slot,
      &gate, &mint);
  WireClient client(nyamp::SpeechServices{ nullptr, &tts, nullptr });
  std::int32_t status = 1;
  const std::string path = "/models/tts";
  const std::string text = "one sentence that is long enough for two windows.";
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t size = 0;
  nyamp_tts_text_s chunk{};

  chunk.total = chunk.length = static_cast<std::uint32_t>(text.size());
  chunk.speed = 1.0f;
  CHECK(nyamp_tts_text_encode(
            payload, sizeof(payload), &size, &chunk,
            reinterpret_cast<const std::uint8_t *>(text.data())) == NYAMP_OK);

  CHECK(client.Call(kTts, NYAMP_TTS_LOAD, 1,
                    reinterpret_cast<const std::uint8_t *>(path.data()),
                    path.size(), &status));
  CHECK(status == NYAMP_MODEL_OK);

  /* A pull is in flight: the synthesis is told BUSY and nothing is lost. */
  CHECK(rig.client->Acquire());
  CHECK(client.Call(kTts, NYAMP_TTS_SYNTH_TEXT, 2, payload, size, &status));
  CHECK(status == NYAMP_MODEL_BUSY);
  rig.client->Release();

  /* A synthesis is in flight: a pull is told busy for as long as it runs,
   * window waits included.
   */
  CHECK(client.Call(kTts, NYAMP_TTS_SYNTH_TEXT, 3, payload, size, &status));
  CHECK(status == NYAMP_MODEL_OK);

  bool pull_refused = true;
  bool finished = false;
  std::int32_t finish_status = 1;
  CHECK(client.Pump([&] { return finished; }, 20000,
                    [&](const WireEvent &event) {
                      if (event.header.opcode == NYAMP_TTS_EVENT_PCM)
                        {
                          nyamp_buffer_s buffer{};
                          std::uint32_t a = 0, b = 0, c = 0, d = 0;
                          std::string ignored;
                          nyamp::BlobStats stats;

                          pull_refused = pull_refused && !rig.client->Acquire();
                          nyamp::ModelProvisioner provisioner(rig.client.get());
                          pull_refused =
                              pull_refused &&
                              provisioner.Provide("llm/model.rkllm", &ignored,
                                                  {}, &stats) ==
                                  nyamp::BlobResult::kBusy;
                          nyamp_tts_pcm_decode(&buffer, &a, &b, &c, &d,
                                               event.payload.data(),
                                               event.payload.size());
                          std::uint8_t release[NYAMP_BUFFER_SIZE];
                          std::size_t release_size = 0;
                          std::int32_t released = 1;
                          nyamp_buffer_encode(release, sizeof(release),
                                              &release_size, &buffer);
                          client.Call(kTts, NYAMP_TTS_RELEASE, 3, release,
                                      release_size, &released);
                        }
                      else if (event.header.opcode == NYAMP_TTS_EVENT_FINISH)
                        {
                          std::uint32_t sequence = 0, total = 0;
                          nyamp_tts_finish_decode(&finish_status, &sequence,
                                                  &total, event.payload.data(),
                                                  event.payload.size());
                          finished = true;
                        }
                    }));
  CHECK(pull_refused && finish_status == NYAMP_MODEL_OK);

  /* And the slot is free again afterwards. */
  CHECK(rig.client->Acquire());
  rig.client->Release();
  return 0;
}

int TestRefusals()
{
  Bench bench;
  const std::uint64_t id = 0x6001;
  const std::string text(900, 'b');

  CHECK(bench.Load() == NYAMP_MODEL_OK);

  /* A continuation that does not continue drops the whole text. */
  CHECK(bench.Chunk(id, text, 0, 400, 1.0f, 0) == NYAMP_MODEL_OK);
  CHECK(bench.Chunk(id, text, 500, 100, 1.0f, 0) == NYAMP_MODEL_INVALID);
  CHECK(bench.Chunk(id, text, 400, 400, 1.0f, 0) == NYAMP_MODEL_INVALID);

  /* So does one whose parameters changed half way, or whose id did. */
  CHECK(bench.Chunk(id, text, 0, 400, 1.0f, 0) == NYAMP_MODEL_OK);
  CHECK(bench.Chunk(id, text, 400, 400, 1.5f, 0) == NYAMP_MODEL_INVALID);
  CHECK(bench.Chunk(id, text, 0, 400, 1.0f, 0) == NYAMP_MODEL_OK);
  CHECK(bench.Chunk(id + 1, text, 400, 400, 1.0f, 0) == NYAMP_MODEL_INVALID);
  CHECK(bench.script.runs == 0 && !bench.gate.held());

  /* Bytes that are not UTF-8 are refused when the text is whole. */
  CHECK(bench.Say(id + 2, std::string("ok \xe4\xbd") + ".") ==
        NYAMP_MODEL_INVALID);
  CHECK(!bench.gate.held());

  /* A window nobody releases ends the request instead of holding the slot
   * for ever.
   */
  Bench::Outcome stuck;
  bench.tts.SetReleaseTimeout(100);
  CHECK(bench.Say(id + 3, "nobody is listening to this one.") ==
        NYAMP_MODEL_OK);
  CHECK(bench.Say(id + 4, "second request.") == NYAMP_MODEL_BUSY);
  CHECK(bench.Listen(id + 3, &stuck, 20000,
                     [](const Window &) { return false; }));
  CHECK(stuck.status == NYAMP_MODEL_DEADLINE && stuck.windows.size() == 1);
  CHECK(!bench.gate.held());
  bench.tts.SetReleaseTimeout(nyamp::kTtsReleaseTimeoutMs);

  /* The request's own deadline does the same. */
  Bench::Outcome late;
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t size = 0;
  std::int32_t status = 1;
  nyamp_tts_text_s chunk{};
  const std::string words = "this one has a deadline.";

  chunk.total = chunk.length = static_cast<std::uint32_t>(words.size());
  chunk.speed = 1.0f;
  CHECK(nyamp_tts_text_encode(
            payload, sizeof(payload), &size, &chunk,
            reinterpret_cast<const std::uint8_t *>(words.data())) == NYAMP_OK);
  bench.client.now_ms = 1000;
  CHECK(bench.client.Call(kTts, NYAMP_TTS_SYNTH_TEXT, id + 5, payload, size,
                          &status, nullptr, nullptr, 5000));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(bench.Listen(id + 5, &late, 20000, [&](const Window &) {
    bench.clock_ms.store(6000);
    return false;
  }));
  CHECK(late.status == NYAMP_MODEL_DEADLINE);
  bench.clock_ms.store(1000);

  /* Without the shared region there is nowhere to put the audio. */
  bench.slot.mapped.store(false);
  CHECK(bench.Say(id + 6, "no slot.") == NYAMP_MODEL_UNSUPPORTED);
  CHECK(!bench.gate.held());
  return 0;
}

/****************************************************************************
 * The real text front end in front of the scripted vocoder.
 ****************************************************************************/

int TestRealFrontend()
{
  const char *assets = std::getenv("NYAMP_G2P_ASSETS");
  const char *golden = std::getenv("NYAMP_G2P_GOLDEN");

  if (assets == nullptr)
    {
      std::printf("  skipped: set NYAMP_G2P_ASSETS\n");
      return 0;
    }

  Bench bench(true, true);

  /* The fixture measured 342 latent frames for its 95 ids; make the
   * scripted vocoder agree so the window arithmetic is the board's.
   */
  bench.script.frames_per_phoneme = 342.0 / 95.0;

  const auto load_started = std::chrono::steady_clock::now();
  CHECK(bench.Load(assets) == NYAMP_MODEL_OK);
  const double load_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - load_started)
                             .count();

  /* The reference sentence of docs/voice-chain-plan.md. */
  const std::string reference =
      "你好，我是星喵。忙了一天，辛苦啦。要不要休息一会儿？";
  Bench::Outcome outcome;
  CHECK(bench.Say(0xa001, reference) == NYAMP_MODEL_OK);
  CHECK(bench.Listen(0xa001, &outcome));
  CHECK(outcome.status == NYAMP_MODEL_OK && !outcome.malformed);
  CHECK(bench.script.unit_ids.size() == 1);
  std::printf("  reference sentence: %zu ids, %zu frames, %u samples, %zu "
              "windows (front end + model stub loaded in %.0f ms)\n",
              bench.script.unit_ids[0].size(), bench.script.unit_frames[0],
              outcome.total_samples, outcome.windows.size(), load_ms);

  if (golden != nullptr)
    {
      std::ifstream file(std::string(golden) + "/X.BIN", std::ios::binary);
      std::vector<std::int64_t> ids;
      std::int64_t id = 0;
      while (file.read(reinterpret_cast<char *>(&id), sizeof(id)))
        {
          ids.push_back(id);
        }

      CHECK(ids.size() == 95);
      CHECK(bench.script.unit_ids[0] == ids);
      CHECK(bench.script.unit_frames[0] == 342);

      /* valid_samples = frames * 512, exactly: 175104 over four windows. */
      CHECK(outcome.total_samples == 175104);
      CHECK(outcome.windows.size() == 4);
      CHECK(outcome.windows[3].valid_samples == 175104 - 3 * 44100);
      std::printf("  ids match X.BIN bit for bit; valid_samples == 175104\n");
    }

  /* A reply of LLM length: several units, none over the bucket, none
   * refused by the vocoder, every unit ending where a sentence or clause
   * does.
   */
  const std::string reply =
      "现在是下午3点25分，室外温度26.5℃。今天的日程有三项：上午十点和产品团队"
      "开会，讨论openvela在RK3576上的移植进度；下午两点去医院复查；晚上七点和"
      "家人一起吃饭。另外，你昨天让我提醒你给猫咪买猫粮，别忘了哦！如果需要，我"
      "可以帮你设置一个下午五点的闹钟。By the way, the weather tomorrow "
      "will be sunny with a high of 28 degrees.";
  Bench::Outcome longer;
  bench.script.unit_ids.clear();
  bench.script.unit_frames.clear();
  const unsigned int refused = bench.script.refused;
  const auto started = std::chrono::steady_clock::now();
  CHECK(bench.Say(0xa002, reply) == NYAMP_MODEL_OK);
  CHECK(bench.Listen(0xa002, &longer));
  const double reply_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  CHECK(longer.status == NYAMP_MODEL_OK && !longer.malformed);
  CHECK(bench.script.unit_ids.size() >= 4);
  CHECK(bench.script.refused == refused);

  std::size_t frames = 0;
  for (std::size_t index = 0; index < bench.script.unit_frames.size(); ++index)
    {
      CHECK(bench.script.unit_frames[index] <= 512);
      frames += bench.script.unit_frames[index];
    }

  CHECK(longer.total_samples == frames * 512);
  std::printf("  %zu-byte reply: %zu units, %zu frames (%.1f s of speech), "
              "%zu windows, %.0f ms with the scripted vocoder\n",
              reply.size(), bench.script.unit_ids.size(), frames,
              static_cast<double>(frames) * 512.0 / 44100.0,
              longer.windows.size(), reply_ms);
  std::printf("  NYAMPD_TTS_FRONTEND_PASS real_g2p=1 real_vocoder=0\n");
  return 0;
}

} // namespace

int main()
{
  struct
  {
    const char *name;
    int (*run)();
  } const tests[] = {
    { "availability, capability and load", TestAvailability },
    { "sentences, chunks and windows", TestSentencesAndWindows },
    { "one window outstanding, leases", TestHeldWindowAndLeases },
    { "audio is never truncated", TestNeverTruncated },
    { "speed and window size", TestSpeedAndWindowSize },
    { "cancel and unload", TestCancel },
    { "the shared slot: synthesis versus model pull", TestSlotSharing },
    { "refusals, timeouts and deadlines", TestRefusals },
    { "real text front end", TestRealFrontend },
  };

  int passed = 0;
  for (const auto &test : tests)
    {
      if (test.run() != 0)
        {
          std::fprintf(stderr, "FAILED: %s\n", test.name);
          return 1;
        }

      std::printf("ok: %s\n", test.name);
      ++passed;
    }

  std::printf("nyampd tts tests passed (%d groups)\n", passed);
  return 0;
}
