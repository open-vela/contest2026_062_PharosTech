/****************************************************************************
 * tools/amp/nyampd/nyampd_asr_test.cpp
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

/* The ASR service through the wire: encoded requests into Dispatch, event
 * frames out of the service queue, audio through a slot that is plain memory.
 * The recognizer is scripted, so every expected text is exact.
 *
 * Built with the sherpa-onnx runtime and given
 *   NYAMP_ASR_MODEL=<dir with encoder/decoder/joiner.onnx, tokens.txt>
 *   NYAMP_ASR_WAVS=<wav>[:<wav>...]
 * the last group also streams real recordings through the real model and
 * requires the transcript the whole-file backend (nyamp_asr_file_test's)
 * produces for the same audio.
 */

#include "nyampd_provision.h"
#include "nyampd_speech_test_support.h"
#include "nyampd_test_support.h"

#ifdef NYAMP_WITH_SHERPA
#include "c-api.h"
#include "nyamp_backends.h"
#endif

#include <cstdlib>

namespace
{

using namespace nyamp::testing;

constexpr std::uint16_t kAsr = NYAMP_SERVICE_ASR;

/* What a control domain keeps of a request: one string, edited by the
 * PARTIAL frames exactly as the protocol header says.
 */

struct Transcript
{
  std::string text;
  std::string final_text;
  unsigned int partial_frames = 0;
  unsigned int resyncs = 0;
  unsigned int endpoints = 0;
  unsigned int finals = 0;
  bool finished = false;
  bool in_order = true;
  std::int32_t status = 1;
  std::uint32_t finish_sequence = 0;
  std::uint32_t last_consumed = 0;
  std::uint32_t next_sequence = 0;

  void Take(const WireEvent &event, std::uint64_t request_id)
  {
    if (event.header.service != kAsr ||
        event.header.request_id != request_id ||
        (event.header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_EVENT)
      {
        return;
      }

    if (event.header.opcode == NYAMP_ASR_EVENT_PARTIAL)
      {
        std::uint32_t sequence = 0;
        std::uint32_t consumed = 0;
        std::uint16_t flags = 0;
        const char *bytes = nullptr;
        std::size_t length = 0;

        if (nyamp_asr_partial_decode(&sequence, &consumed, &flags, &bytes,
                                     &length, event.payload.data(),
                                     event.payload.size()) != NYAMP_OK ||
            finished || finals != 0)
          {
            in_order = false;
            return;
          }

        /* Frames may be shed, so the sequence can skip but never go back. */
        in_order = in_order && sequence >= next_sequence;
        next_sequence = sequence + 1;
        ++partial_frames;
        last_consumed = consumed;

        if ((flags & NYAMP_ASR_PARTIAL_RESYNC) != 0)
          {
            text.clear();
            ++resyncs;
          }

        text.append(bytes, length);
        endpoints += (flags & NYAMP_ASR_PARTIAL_ENDPOINT) != 0 ? 1 : 0;
        if ((flags & NYAMP_ASR_PARTIAL_FINAL) != 0)
          {
            ++finals;
            final_text = text;
          }
      }
    else if (event.header.opcode == NYAMP_ASR_EVENT_FINISH)
      {
        in_order = in_order && !finished &&
                   nyamp_asr_finish_decode(&status, &finish_sequence,
                                           event.payload.data(),
                                           event.payload.size()) == NYAMP_OK;
        finished = true;
      }
  }
};

struct Bench
{
  explicit Bench(bool with_backend = true)
      : capture(kCaptureOffset, kCaptureSize), mint(kSpeechGeneration),
        asr(kSpeechGeneration, [this] { return clock_ms.load(); },
            with_backend
                ? nyamp::AsrService::BackendFactory([this] {
                    return std::make_unique<ScriptedAsrBackend>(&script);
                  })
                : nyamp::AsrService::BackendFactory(),
            &capture, &gate, &mint),
        client(nyamp::SpeechServices{ &asr, nullptr, nullptr })
  {
  }

  bool Load(const std::string &path = "/models/asr")
  {
    std::int32_t status = 1;
    return client.Call(kAsr, NYAMP_ASR_LOAD, next_id++,
                       reinterpret_cast<const std::uint8_t *>(path.data()),
                       path.size(), &status) &&
           status == NYAMP_MODEL_OK;
  }

  /* BEGIN in its pushed form; returns the wire status. */

  std::int32_t Begin(std::uint64_t id, nyamp_buffer_s *grant,
                     std::uint16_t flags = 0, std::uint32_t max_samples = 0,
                     std::uint64_t deadline_ms = 0)
  {
    std::uint8_t payload[NYAMP_ASR_BEGIN_SIZE];
    std::size_t size = 0;
    std::int32_t status = 1;
    std::vector<std::uint8_t> body;

    if (nyamp_asr_begin_encode(payload, sizeof(payload), &size, 16000, 1,
                               flags, max_samples) != NYAMP_OK ||
        !client.Call(kAsr, NYAMP_ASR_BEGIN, id, payload, size, &status, &body,
                     nullptr, deadline_ms))
      {
        return 1;
      }

    if (status == NYAMP_MODEL_OK &&
        nyamp_buffer_decode(grant, body.data(), body.size()) != NYAMP_OK)
      {
        return 2;
      }

    return status;
  }

  std::int32_t Attach(std::uint64_t id, std::uint64_t start,
                      std::uint32_t max_samples = 0)
  {
    std::uint8_t payload[NYAMP_ASR_ATTACH_SIZE];
    std::size_t size = 0;
    std::int32_t status = 1;
    std::vector<std::uint8_t> body;

    if (nyamp_asr_attach_encode(payload, sizeof(payload), &size, 16000, 1, 0,
                                max_samples, start) != NYAMP_OK ||
        !client.Call(kAsr, NYAMP_ASR_BEGIN, id, payload, size, &status,
                     &body) ||
        !body.empty()) /* An attached request is granted nothing. */
      {
        return 1;
      }

    return status;
  }

  std::int32_t PushWindow(std::uint64_t id, const nyamp_buffer_s &window,
                          std::uint32_t sequence)
  {
    std::uint8_t payload[NYAMP_ASR_PUSH_HEADER_SIZE];
    std::size_t size = 0;
    std::int32_t status = 1;

    if (nyamp_asr_push_encode(payload, sizeof(payload), &size, &window,
                              sequence, 0, 0, 0) != NYAMP_OK ||
        !client.Call(kAsr, NYAMP_ASR_PUSH, id, payload, size, &status))
      {
        return 1;
      }

    return status;
  }

  /* Write `samples` into the grant, ping-ponging between its first two
   * windows, and PUSH each one.  Returns the number of windows pushed, or
   * -1 when one was refused.
   */

  int PushAll(std::uint64_t id, const nyamp_buffer_s &grant,
              const std::vector<float> &samples, std::size_t window_samples,
              bool s16 = false, bool last_flag = false)
  {
    const std::size_t bytes = s16 ? 2 : 4;
    std::uint32_t sequence = 0;

    for (std::size_t offset = 0; offset < samples.size();
         offset += window_samples)
      {
        const std::size_t count =
            std::min(window_samples, samples.size() - offset);
        nyamp_buffer_s window = grant;

        window.offset = grant.offset + static_cast<std::uint32_t>(
                                           (sequence % 2) * window_samples *
                                           bytes);
        window.length = static_cast<std::uint32_t>(count * bytes);
        window.capacity = static_cast<std::uint32_t>(window_samples * bytes);
        if (last_flag && offset + count >= samples.size())
          {
            window.flags |= NYAMP_BUFFER_LAST;
          }

        std::uint8_t *dest = capture.at(window.offset);
        if (s16)
          {
            for (std::size_t index = 0; index < count; ++index)
              {
                const std::int16_t value = static_cast<std::int16_t>(
                    samples[offset + index] * 32768.0f);
                std::memcpy(dest + index * 2, &value, 2);
              }
          }
        else
          {
            std::memcpy(dest, samples.data() + offset, count * 4);
          }

        if (PushWindow(id, window, sequence) != NYAMP_MODEL_OK)
          {
            return -1;
          }

        /* The range may be rewritten as soon as the response is here:
         * prove the service no longer depends on it.
         */
        std::memset(dest, 0xa5, count * bytes);
        ++sequence;
      }

    return static_cast<int>(sequence);
  }

  std::int32_t End(std::uint64_t id,
                   std::uint64_t end_sample = NYAMP_STREAM_SAMPLE_NOW)
  {
    std::uint8_t payload[NYAMP_ASR_END_SIZE];
    std::size_t size = 0;
    std::int32_t status = 1;

    if (nyamp_asr_end_encode(payload, sizeof(payload), &size, end_sample) !=
            NYAMP_OK ||
        !client.Call(kAsr, NYAMP_ASR_END, id, payload, size, &status))
      {
        return 1;
      }

    return status;
  }

  std::int32_t Simple(std::uint16_t opcode, std::uint64_t id)
  {
    std::int32_t status = 1;
    return client.Call(kAsr, opcode, id, nullptr, 0, &status) ? status : 1;
  }

  bool Finish(std::uint64_t id, Transcript *transcript, int timeout_ms = 20000)
  {
    return client.Pump([&] { return transcript->finished; }, timeout_ms,
                       [&](const WireEvent &event) {
                         transcript->Take(event, id);
                       });
  }

  MemorySlot capture;
  nyamp::AtomicGate gate;
  nyamp::LeaseMint mint;
  AsrScript script;
  std::atomic<std::uint64_t> clock_ms{ 1000 };
  nyamp::AsrService asr;
  WireClient client;
  std::uint64_t next_id = 0x7000;
};

std::vector<float> Tone(std::size_t count, float level = 0.25f)
{
  return std::vector<float>(count, level);
}

/****************************************************************************
 * Tests
 ****************************************************************************/

int TestAvailability()
{
  /* No recognizer in this build: everything is refused, LLM-style, and the
   * capability bit says so up front.
   */
  Bench bare(false);
  std::int32_t status = 1;
  std::vector<std::uint8_t> body;
  nyamp_buffer_s grant{};

  CHECK(bare.client.Call(NYAMP_SERVICE_HEALTH, 1, 1, nullptr, 0, &status,
                         &body));
  CHECK(status == NYAMP_MODEL_OK && body.size() == 8);
  CHECK((body[4] & (1U << 4)) == 0);
  CHECK(!bare.Load());
  CHECK(bare.Begin(10, &grant) == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bare.Simple(NYAMP_ASR_UNLOAD, 11) == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bare.asr.Info() == "none:-2");

  /* No service object at all (a daemon predating it). */
  WireClient nobody(nyamp::SpeechServices{});
  CHECK(nobody.Call(kAsr, NYAMP_ASR_UNLOAD, 12, nullptr, 0, &status));
  CHECK(status == NYAMP_MODEL_UNSUPPORTED);

  Bench bench;
  CHECK(bench.client.Call(NYAMP_SERVICE_HEALTH, 1, 2, nullptr, 0, &status,
                          &body));
  CHECK((body[4] & (1U << 4)) != 0);
  CHECK((body[4] & ((1U << 5) | (1U << 6))) == 0); /* Not TTS, not KWS. */

  /* Before LOAD. */
  CHECK(bench.asr.Info() == "off:-2");
  CHECK(bench.Begin(20, &grant) == NYAMP_MODEL_NOT_READY);
  CHECK(bench.Simple(NYAMP_ASR_UNLOAD, 21) == NYAMP_MODEL_INVALID);

  /* A load the backend refuses is reported, and leaves nothing loaded. */
  bench.script.fail_load = true;
  CHECK(!bench.Load());
  CHECK(bench.asr.Info() == "off:-8");
  bench.script.fail_load = false;
  CHECK(bench.Load());
  CHECK(bench.asr.Info() == "ready:0");
  CHECK(bench.script.loaded_directory == "/models/asr");

  /* Loading what is loaded moves nothing; another model needs UNLOAD. */
  const unsigned int loads = bench.script.loads;
  CHECK(bench.Load());
  CHECK(bench.script.loads == loads);
  CHECK(!bench.Load("/models/other"));

  /* Unknown opcode, malformed BEGIN. */
  CHECK(bench.Simple(0x55, 22) == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bench.Simple(NYAMP_ASR_BEGIN, 23) == NYAMP_MODEL_INVALID);

  std::uint8_t payload[NYAMP_ASR_BEGIN_SIZE];
  std::size_t size = 0;
  CHECK(nyamp_asr_begin_encode(payload, sizeof(payload), &size, 8000, 1, 0,
                               0) == NYAMP_OK);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_BEGIN, 24, payload, size, &status));
  CHECK(status == NYAMP_MODEL_INVALID); /* 8 kHz. */

  CHECK(bench.Simple(NYAMP_ASR_UNLOAD, 25) == NYAMP_MODEL_OK);
  CHECK(bench.script.unloads == 1);
  return 0;
}

int TestPushedRequest()
{
  Bench bench;
  nyamp_buffer_s grant{};
  Transcript transcript;
  const std::uint64_t id = 0x1111000000000001ULL;

  bench.script.steps = { { 8000, "你好", false },
                         { 24000, "你好世界", false },
                         { 40000, "你好世界今天", false } };
  bench.script.final_suffix = "天气";

  CHECK(bench.Load());
  CHECK(bench.Begin(id, &grant) == NYAMP_MODEL_OK);
  CHECK(grant.offset == kCaptureOffset && grant.capacity == kCaptureSize);
  CHECK(grant.format == NYAMP_FORMAT_F32 && grant.length == 0);
  CHECK(grant.generation == kSpeechGeneration);
  CHECK((grant.flags & NYAMP_BUFFER_IN_SHMEM) != 0);
  CHECK(bench.gate.held());
  CHECK(bench.asr.Info() == "busy:0");

  /* 3.1 s in one-second windows; the last one is short. */
  const std::vector<float> audio = Tone(49600);
  CHECK(bench.PushAll(id, grant, audio, 16000) == 4);
  CHECK(bench.End(id) == NYAMP_MODEL_OK);
  CHECK(bench.End(id) == NYAMP_MODEL_OK); /* Twice is harmless. */
  CHECK(bench.Finish(id, &transcript));

  CHECK(transcript.status == NYAMP_MODEL_OK && transcript.in_order);
  CHECK(transcript.finals == 1);
  CHECK(transcript.final_text == "你好世界今天天气");
  CHECK(transcript.last_consumed == 49600);
  CHECK(transcript.finish_sequence == transcript.partial_frames);
  CHECK(bench.client.malformed == 0);

  /* Every sample arrived, with its value: 49600 * 0.25. */
  CHECK(bench.script.accepted == 49600);
  CHECK(std::fabs(bench.script.energy.load() - 12400.0) < 0.01);

  /* The request gave the slot back by ending. */
  CHECK(!bench.gate.held());
  CHECK(bench.script.live_streams == 0);

  /* RELEASE after the end is an idempotent success; a foreign lease never. */
  std::uint8_t payload[NYAMP_BUFFER_SIZE];
  std::size_t size = 0;
  std::int32_t status = 1;
  CHECK(nyamp_buffer_encode(payload, sizeof(payload), &size, &grant) ==
        NYAMP_OK);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_RELEASE, id, payload, size,
                          &status));
  CHECK(status == NYAMP_MODEL_OK);
  nyamp_buffer_s foreign = grant;
  foreign.lease ^= 1;
  CHECK(nyamp_buffer_encode(payload, sizeof(payload), &size, &foreign) ==
        NYAMP_OK);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_RELEASE, id, payload, size,
                          &status));
  CHECK(status == NYAMP_MODEL_INVALID);

  /* A second request on the same model: S16 windows, ended by the LAST
   * flag of its final window instead of ASR_END.  A new grant, a new lease.
   */
  nyamp_buffer_s second{};
  Transcript again;
  const std::uint64_t id2 = id + 1;

  bench.script.energy.store(0);
  CHECK(bench.Begin(id2, &second, NYAMP_AUDIO_BEGIN_S16) == NYAMP_MODEL_OK);
  CHECK(second.lease != grant.lease && second.format == NYAMP_FORMAT_S16);
  CHECK(bench.PushAll(id2, second, Tone(40000, 0.5f), 8000, true, true) == 5);
  CHECK(bench.Finish(id2, &again));
  CHECK(again.status == NYAMP_MODEL_OK && again.final_text == "你好世界今天天气");
  CHECK(std::fabs(bench.script.energy.load() - 20000.0) < 0.01);
  CHECK(bench.script.streams == 2);
  return 0;
}

int TestRewriteEndpointAndLongText()
{
  Bench bench;
  nyamp_buffer_s grant{};
  Transcript transcript;
  const std::uint64_t id = 0x2222;

  /* The third step rewrites the second character -- a transducer may --
   * so it cannot be sent as a suffix.  The fourth holds the text still
   * while the decoder reports an endpoint, the fifth outgrows one frame.
   */
  std::string longer = "你号世界";
  for (int index = 0; index < 200; ++index)
    {
      longer += "啊";
    }

  bench.script.steps = { { 3200, "你好", false },
                         { 6400, "你好世", false },
                         { 9600, "你号世界", false },
                         { 12800, "你号世界", true },
                         { 16000, "你号世界", true },
                         { 19200, longer, false } };

  CHECK(bench.Load());
  CHECK(bench.Begin(id, &grant) == NYAMP_MODEL_OK);

  /* One decoder step per window, and wait for it, so every script step is
   * observed: this test is about the frames, not about throughput.
   */
  std::vector<std::string> seen;
  for (std::uint32_t window = 0; window < 6; ++window)
    {
      nyamp_buffer_s range = grant;
      range.length = 3200 * 4;
      std::memset(bench.capture.at(range.offset), 0, range.length);
      CHECK(bench.PushWindow(id, range, window) == NYAMP_MODEL_OK);
      const auto take = [&](const WireEvent &event) {
        transcript.Take(event, id);
      };
      CHECK(bench.client.Pump(
          [&] { return bench.script.accepted == (window + 1) * 3200ULL; },
          20000, take));

      /* Let the publish that follows the decode land. */
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      bench.client.Pump([] { return true; }, 0, take);
      seen.push_back(transcript.text);
    }

  CHECK(seen[0] == "你好" && seen[1] == "你好世" && seen[2] == "你号世界");
  CHECK(seen[5] == longer);
  CHECK(transcript.resyncs == 1); /* Only the rewrite. */
  CHECK(transcript.endpoints == 1); /* The rising edge, not every step. */

  CHECK(bench.End(id) == NYAMP_MODEL_OK);
  CHECK(bench.Finish(id, &transcript));
  CHECK(transcript.status == NYAMP_MODEL_OK && transcript.in_order);
  CHECK(transcript.final_text == longer && transcript.finals == 1);

  /* 612 bytes of text do not fit one 444-byte frame. */
  CHECK(longer.size() > NYAMP_ASR_MAX_TEXT);
  CHECK(transcript.resyncs == 2);
  return 0;
}

int TestLeaseAndRangeChecks()
{
  Bench bench;
  nyamp_buffer_s grant{};
  const std::uint64_t id = 0x3333;

  CHECK(bench.Load());
  CHECK(bench.Begin(id, &grant, 0, 32000) == NYAMP_MODEL_OK);

  nyamp_buffer_s window = grant;
  window.length = 64000;

  /* Not this request. */
  CHECK(bench.PushWindow(id + 1, window, 0) == NYAMP_MODEL_NOT_READY);

  /* A stale or foreign lease, another generation, outside the grant, in
   * front of it, half a sample, the wrong sample format.
   */
  nyamp_buffer_s bad = window;
  bad.lease ^= 0x10;
  CHECK(bench.PushWindow(id, bad, 0) == NYAMP_MODEL_INVALID);
  bad = window;
  bad.generation ^= 1;
  CHECK(bench.PushWindow(id, bad, 0) == NYAMP_MODEL_INVALID);
  bad = window;
  bad.offset = kCaptureOffset + kCaptureSize - 4;
  bad.capacity = bad.length = 8;
  CHECK(bench.PushWindow(id, bad, 0) == NYAMP_MODEL_INVALID);
  bad = window;
  bad.offset = kSharedOffset;
  CHECK(bench.PushWindow(id, bad, 0) == NYAMP_MODEL_INVALID);
  bad = window;
  bad.length = 6;
  CHECK(bench.PushWindow(id, bad, 0) == NYAMP_MODEL_INVALID);
  bad = window;
  bad.format = NYAMP_FORMAT_S16;
  CHECK(bench.PushWindow(id, bad, 0) == NYAMP_MODEL_INVALID);

  /* A window that skips a sequence number was lost on the way. */
  CHECK(bench.PushWindow(id, window, 1) == NYAMP_MODEL_INVALID);
  CHECK(bench.script.accepted == 0); /* None of the above got through. */

  /* The request said 32000 samples at most. */
  CHECK(bench.PushWindow(id, window, 0) == NYAMP_MODEL_OK);
  CHECK(bench.PushWindow(id, window, 1) == NYAMP_MODEL_OK);
  window.length = 4;
  CHECK(bench.PushWindow(id, window, 2) == NYAMP_MODEL_INVALID);

  /* A pushed request has no position to end at but "now". */
  CHECK(bench.End(id, 16000) == NYAMP_MODEL_INVALID);
  CHECK(bench.End(id + 1) == NYAMP_MODEL_NOT_READY);
  CHECK(bench.End(id) == NYAMP_MODEL_OK);
  CHECK(bench.PushWindow(id, window, 2) == NYAMP_MODEL_INVALID ||
        bench.PushWindow(id, window, 2) == NYAMP_MODEL_NOT_READY);

  Transcript transcript;
  CHECK(bench.Finish(id, &transcript));
  CHECK(transcript.status == NYAMP_MODEL_OK && transcript.last_consumed == 32000);

  /* max_samples above a minute is refused outright. */
  CHECK(bench.Begin(id + 2, &grant, 0, 16000 * 60 + 1) == NYAMP_MODEL_INVALID);
  return 0;
}

int TestBusyCancelUnload()
{
  Bench bench;
  nyamp_buffer_s grant{};
  nyamp_buffer_s other{};
  const std::uint64_t id = 0x4444;

  bench.script.steps = { { 3200, "正在", false } };
  bench.script.decode_delay_ms = 20;

  CHECK(bench.Load());

  /* The wake word stream (or anyone else) owns the capture slot. */
  CHECK(bench.gate.Acquire());
  CHECK(bench.Begin(id, &grant) == NYAMP_MODEL_BUSY);
  bench.gate.Release();

  CHECK(bench.Begin(id, &grant) == NYAMP_MODEL_OK);
  CHECK(bench.Begin(id + 1, &other) == NYAMP_MODEL_BUSY);
  CHECK(!bench.Load("/models/other"));

  /* Cancel while the decoder is busy with a backlog of windows. */
  CHECK(bench.PushAll(id, grant, Tone(48000), 16000) == 3);
  CHECK(bench.Simple(NYAMP_ASR_CANCEL, id + 1) == NYAMP_MODEL_NOT_READY);
  CHECK(bench.Simple(NYAMP_ASR_CANCEL, id) == NYAMP_MODEL_OK);

  Transcript transcript;
  CHECK(bench.Finish(id, &transcript));
  CHECK(transcript.status == NYAMP_MODEL_CANCELLED);
  CHECK(transcript.finals == 0); /* No final text for a cancelled request. */
  CHECK(bench.script.accepted < 48000);
  CHECK(!bench.gate.held() && bench.script.live_streams == 0);

  /* The CANCEL-kind frame does the same and is never answered. */
  Transcript second;
  CHECK(bench.Begin(id + 2, &grant) == NYAMP_MODEL_OK);
  CHECK(bench.PushAll(id + 2, grant, Tone(48000), 16000) == 3);
  CHECK(bench.client.CancelFrame(kAsr, id + 2));
  CHECK(bench.Finish(id + 2, &second));
  CHECK(second.status == NYAMP_MODEL_CANCELLED);

  /* UNLOAD ends the request too, without making the loop wait for it:
   * BUSY now, OK once the FINISH has been seen.
   */
  Transcript third;
  CHECK(bench.Begin(id + 3, &grant) == NYAMP_MODEL_OK);
  CHECK(bench.PushAll(id + 3, grant, Tone(48000), 16000) == 3);
  CHECK(bench.Simple(NYAMP_ASR_UNLOAD, 90) == NYAMP_MODEL_BUSY);
  CHECK(bench.Finish(id + 3, &third));
  CHECK(third.status == NYAMP_MODEL_CANCELLED);
  CHECK(bench.Simple(NYAMP_ASR_UNLOAD, 91) == NYAMP_MODEL_OK);
  CHECK(bench.Begin(id + 4, &grant) == NYAMP_MODEL_NOT_READY);
  return 0;
}

int TestDeadlineAndEarlyRelease()
{
  Bench bench;
  nyamp_buffer_s grant{};
  const std::uint64_t id = 0x5555;

  CHECK(bench.Load());

  /* A request nobody ends is ended by its deadline. */
  bench.client.now_ms = 1000;
  CHECK(bench.Begin(id, &grant, 0, 0, 5000) == NYAMP_MODEL_OK);
  bench.clock_ms.store(6000);

  Transcript transcript;
  CHECK(bench.Finish(id, &transcript));
  CHECK(transcript.status == NYAMP_MODEL_DEADLINE && transcript.finals == 0);
  bench.clock_ms.store(1000);

  /* RELEASE hands the slot back while the request is still decoding; no
   * further PUSH is accepted, END still works.
   */
  std::uint8_t payload[NYAMP_BUFFER_SIZE];
  std::size_t size = 0;
  std::int32_t status = 1;
  Transcript second;

  bench.script.steps = { { 16000, "一秒", false } };
  CHECK(bench.Begin(id + 1, &grant) == NYAMP_MODEL_OK);
  CHECK(bench.PushAll(id + 1, grant, Tone(16000), 16000) == 1);
  CHECK(nyamp_buffer_encode(payload, sizeof(payload), &size, &grant) ==
        NYAMP_OK);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_RELEASE, id + 1, payload, size,
                          &status));
  CHECK(status == NYAMP_MODEL_OK && !bench.gate.held());

  nyamp_buffer_s window = grant;
  window.length = 64;
  CHECK(bench.PushWindow(id + 1, window, 1) == NYAMP_MODEL_INVALID);
  CHECK(bench.End(id + 1) == NYAMP_MODEL_OK);
  CHECK(bench.Finish(id + 1, &second));
  CHECK(second.status == NYAMP_MODEL_OK && second.final_text == "一秒");

  /* Without the shared region a pushed request cannot exist. */
  bench.capture.mapped.store(false);
  CHECK(bench.Begin(id + 2, &grant) == NYAMP_MODEL_UNSUPPORTED);
  CHECK(!bench.gate.held());
  return 0;
}

/* The wake word stream, reduced to its ring. */

class RingSource final : public nyamp::CaptureSource
{
public:
  std::shared_ptr<nyamp::CaptureRing> Stream() override { return ring; }
  std::shared_ptr<nyamp::CaptureRing> ring;
};

int TestAttachedRequest()
{
  Bench bench;
  RingSource source;
  const std::uint64_t id = 0x6666;

  bench.script.steps = { { 16000, "打开", false }, { 32000, "打开灯", false } };
  bench.asr.SetCaptureSource(&source);
  CHECK(bench.Load());

  /* No stream to attach to. */
  CHECK(bench.Attach(id, 0) == NYAMP_MODEL_NOT_READY);

  /* A stream that has been running for a while: 10 s ring, 25 s in. */
  source.ring = std::make_shared<nyamp::CaptureRing>(160000, 0);
  const std::vector<float> second_of_audio = Tone(16000, 0.5f);
  for (int second = 0; second < 25; ++second)
    {
      source.ring->Append(second_of_audio.data(), second_of_audio.size());
    }

  CHECK(source.ring->begin() == 240000 && source.ring->end() == 400000);

  /* Older than the ring holds: refused, not silently started later. */
  CHECK(bench.Attach(id, 100000) == NYAMP_MODEL_INVALID);

  /* An attached request may not be pushed to, and holds no slot. */
  CHECK(bench.Attach(id, 384000) == NYAMP_MODEL_OK);
  CHECK(!bench.gate.held());
  nyamp_buffer_s window =
      nyamp::MakeGrant(&bench.mint, kCaptureOffset, 64000, NYAMP_FORMAT_F32, 0);
  window.length = 64000;
  CHECK(bench.PushWindow(id, window, 0) == NYAMP_MODEL_INVALID);

  /* It starts with the second already in the ring, then follows the
   * stream live.
   */
  Transcript transcript;
  const auto take = [&](const WireEvent &event) { transcript.Take(event, id); };
  CHECK(bench.client.Pump([&] { return bench.script.accepted == 16000; },
                          20000, take));

  /* End at an explicit position that the stream has not reached yet, half
   * a second short of what is about to arrive.
   */
  CHECK(bench.End(id, 383999) == NYAMP_MODEL_INVALID); /* Before the start. */
  CHECK(bench.End(id, 424000) == NYAMP_MODEL_OK);
  source.ring->Append(second_of_audio.data(), second_of_audio.size());
  source.ring->Append(second_of_audio.data(), second_of_audio.size());

  CHECK(bench.Finish(id, &transcript));
  CHECK(transcript.status == NYAMP_MODEL_OK);
  CHECK(transcript.final_text == "打开灯");
  CHECK(transcript.last_consumed == 40000);
  CHECK(bench.script.accepted == 40000);
  CHECK(std::fabs(bench.script.energy.load() - 20000.0) < 0.01);

  /* A start in the future waits for the stream to get there; max_samples
   * ends the request by itself when nobody else does.
   */
  Transcript bounded;
  bench.script.energy.store(0);
  CHECK(bench.Attach(id + 1, 440000, 8000) == NYAMP_MODEL_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(bench.script.accepted == 0);
  source.ring->Append(second_of_audio.data(), second_of_audio.size());
  source.ring->Append(second_of_audio.data(), second_of_audio.size());
  CHECK(bench.Finish(id + 1, &bounded));
  CHECK(bounded.status == NYAMP_MODEL_OK && bounded.last_consumed == 8000);
  CHECK(bench.script.accepted == 8000);

  /* The stream ends underneath an attached request: it finishes with the
   * audio it got, as if ASR_END had named that point.
   */
  Transcript orphan;
  CHECK(bench.Attach(id + 2, source.ring->end()) == NYAMP_MODEL_OK);
  source.ring->Append(second_of_audio.data(), second_of_audio.size());
  source.ring->Close();
  CHECK(bench.Finish(id + 2, &orphan));
  CHECK(orphan.status == NYAMP_MODEL_OK && orphan.final_text == "打开");
  CHECK(orphan.last_consumed == 16000);

  /* And a closed stream cannot be attached to. */
  CHECK(bench.Attach(id + 3, source.ring->end()) == NYAMP_MODEL_NOT_READY);
  return 0;
}

int TestBackPressure()
{
  Bench bench;
  nyamp_buffer_s grant{};
  const std::uint64_t id = 0x7777;
  std::string expected;

  /* 200 steps, each a longer text, and nobody reads the queue. */
  for (std::uint64_t step = 1; step <= 200; ++step)
    {
      expected += "字";
      bench.script.steps.push_back({ step * 3200, expected, false });
    }

  CHECK(bench.Load());
  CHECK(bench.Begin(id, &grant, 0, 0) == NYAMP_MODEL_OK);
  CHECK(bench.PushAll(id, grant, Tone(200 * 3200), 16000) == 40);
  CHECK(bench.End(id) == NYAMP_MODEL_OK);

  /* Wait for the worker without draining. */
  for (int wait = 0; wait < 5000 && bench.gate.held(); ++wait)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

  CHECK(!bench.gate.held());

  /* Partials were shed; the final text and the FINISH never are, and the
   * receiver still ends up with the exact text.
   */
  Transcript transcript;
  CHECK(bench.Finish(id, &transcript));
  CHECK(transcript.status == NYAMP_MODEL_OK && transcript.in_order);
  CHECK(transcript.final_text == expected);
  CHECK(transcript.partial_frames < 200);
  CHECK(bench.client.events.size() <= nyamp::kAsrEventQueueLimit + 4);
  std::printf("  back-pressure: %u of 200 partial frames delivered\n",
              transcript.partial_frames);
  return 0;
}

int TestLogicalNameLoad()
{
  Rig rig(4096);
  nyamp::ModelProvisioner provisioner(rig.client.get());
  Bench bench;
  std::int32_t status = 1;
  bool deferred = false;
  const std::string name = "asr";

  bench.asr.SetProvisioner(&provisioner);

  /* Nothing on the control domain yet: the deferred response says so. */
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_LOAD, 500,
                          reinterpret_cast<const std::uint8_t *>(name.data()),
                          name.size(), &status, nullptr, &deferred));
  CHECK(deferred);
  CHECK(bench.client.Pump([&] {
    return bench.client.Find(kAsr, NYAMP_ASR_LOAD, 500, NYAMP_FLAG_RESPONSE) !=
           nullptr;
  }));
  const WireEvent *answer =
      bench.client.Find(kAsr, NYAMP_ASR_LOAD, 500, NYAMP_FLAG_RESPONSE);
  const std::uint8_t *body = nullptr;
  std::size_t body_size = 0;
  CHECK(nyamp_status_decode(&status, &body, &body_size, answer->payload.data(),
                            answer->payload.size()) == NYAMP_OK);
  CHECK(status == NYAMP_MODEL_NOT_READY && bench.script.loads == 0);

  /* The directory is pulled file by file into the blob root, then loaded
   * from there.
   */
  rig.responder.files["asr/encoder.onnx"] = Noise(9000, 1);
  rig.responder.files["asr/decoder.onnx"] = Noise(5000, 2);
  rig.responder.files["asr/joiner.onnx"] = Noise(3000, 3);
  rig.responder.files["asr/tokens.txt"] = Noise(700, 4);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_LOAD, 501,
                          reinterpret_cast<const std::uint8_t *>(name.data()),
                          name.size(), &status, nullptr, &deferred));
  CHECK(deferred);
  CHECK(bench.client.Pump([&] {
    return bench.client.Find(kAsr, NYAMP_ASR_LOAD, 501, NYAMP_FLAG_RESPONSE) !=
           nullptr;
  }));
  answer = bench.client.Find(kAsr, NYAMP_ASR_LOAD, 501, NYAMP_FLAG_RESPONSE);
  CHECK(nyamp_status_decode(&status, &body, &body_size, answer->payload.data(),
                            answer->payload.size()) == NYAMP_OK);
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(bench.script.loaded_directory == rig.root + "/asr");
  CHECK(rig.Read("asr/encoder.onnx") == rig.responder.files["asr/encoder.onnx"]);
  CHECK(rig.Exists("asr/tokens.txt"));

  /* Progress rode on the LOAD's request id as BLOB events. */
  CHECK(bench.client.Find(NYAMP_SERVICE_BLOB, NYAMP_BLOB_EVENT_PROGRESS, 501) !=
        nullptr);
  CHECK(bench.asr.Info() == "ready:0");
  return 0;
}

/****************************************************************************
 * Real model, real recordings.
 ****************************************************************************/

#ifdef NYAMP_WITH_SHERPA
int StreamOneFile(nyamp::AsrService &asr, WireClient &client,
                  MemorySlot &capture, const std::string &path,
                  const std::string &model, std::uint64_t id)
{
  const SherpaOnnxWave *wave = SherpaOnnxReadWave(path.c_str());
  CHECK(wave != nullptr && wave->sample_rate == 16000 &&
        wave->num_samples > 0);
  const std::vector<float> audio(wave->samples,
                                 wave->samples + wave->num_samples);
  SherpaOnnxFreeWave(wave);

  /* The reference: the whole-file backend nyamp_asr_file_test runs. */
  std::string reference;
  {
    nyamp::models::Session session(nyamp::models::CreateSherpaBackend(), 1,
                                   [] { return std::uint64_t(0); });
    CHECK(session.Load(model) == nyamp::models::Status::kOk);
    nyamp::models::AsrInput input{ audio, 16000 };
    CHECK(session.Run({ 1, 1, 0 }, input,
                      [&](const nyamp::models::Event &event) {
                        if (!event.terminal)
                          {
                            const auto &text =
                                std::get<nyamp::models::Transcript>(
                                    *event.output);
                            if (text.final)
                              {
                                reference = text.text;
                              }
                          }

                        return true;
                      }) == nyamp::models::Status::kOk);
    CHECK(session.Unload() == nyamp::models::Status::kOk);
  }

  CHECK(!reference.empty());

  /* The wire: BEGIN, one-second windows ping-ponged over two ranges at
   * real-time pace / 20, END.
   */
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t size = 0;
  std::int32_t status = 1;
  std::vector<std::uint8_t> body;
  nyamp_buffer_s grant{};

  CHECK(nyamp_asr_begin_encode(payload, sizeof(payload), &size, 16000, 1, 0,
                               0) == NYAMP_OK);
  CHECK(client.Call(NYAMP_SERVICE_ASR, NYAMP_ASR_BEGIN, id, payload, size,
                    &status, &body));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(nyamp_buffer_decode(&grant, body.data(), body.size()) == NYAMP_OK);

  const auto started = std::chrono::steady_clock::now();
  Transcript transcript;
  std::uint32_t sequence = 0;
  double first_partial_ms = -1;

  auto take = [&](const WireEvent &event) {
    transcript.Take(event, id);
    if (first_partial_ms < 0 && !transcript.text.empty())
      {
        first_partial_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
      }
  };

  for (std::size_t offset = 0; offset < audio.size(); offset += 16000)
    {
      const std::size_t count =
          std::min<std::size_t>(16000, audio.size() - offset);
      nyamp_buffer_s window = grant;

      window.offset = grant.offset + (sequence % 2) * 64000;
      window.length = static_cast<std::uint32_t>(count * 4);
      std::memcpy(capture.at(window.offset), audio.data() + offset, count * 4);
      CHECK(nyamp_asr_push_encode(payload, sizeof(payload), &size, &window,
                                  sequence, 0, 0, 0) == NYAMP_OK);
      CHECK(client.Call(NYAMP_SERVICE_ASR, NYAMP_ASR_PUSH, id, payload, size,
                        &status));
      CHECK(status == NYAMP_MODEL_OK);
      ++sequence;
      client.Pump([] { return true; }, 0, take);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

  CHECK(nyamp_asr_end_encode(payload, sizeof(payload), &size,
                             NYAMP_STREAM_SAMPLE_NOW) == NYAMP_OK);
  CHECK(client.Call(NYAMP_SERVICE_ASR, NYAMP_ASR_END, id, payload, size,
                    &status));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(client.Pump([&] { return transcript.finished; }, 120000, take));

  const double total_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count();

  CHECK(transcript.status == NYAMP_MODEL_OK && transcript.in_order);
  CHECK(transcript.finals == 1);
  CHECK(transcript.partial_frames >= 3); /* It really was incremental. */
  CHECK(first_partial_ms >= 0 && first_partial_ms < total_ms);

  std::printf("  %s\n    audio %.2f s, %u partial frames, first text after "
              "%.0f ms, done after %.0f ms\n    streamed : %s\n    reference: "
              "%s\n",
              path.c_str(), static_cast<double>(audio.size()) / 16000.0,
              transcript.partial_frames, first_partial_ms, total_ms,
              transcript.final_text.c_str(), reference.c_str());
  CHECK(transcript.final_text == reference);
  (void)asr;
  return 0;
}
#endif

int TestRealModel()
{
#ifdef NYAMP_WITH_SHERPA
  const char *model = std::getenv("NYAMP_ASR_MODEL");
  const char *wavs = std::getenv("NYAMP_ASR_WAVS");

  if (model == nullptr || wavs == nullptr)
    {
      std::printf("  skipped: set NYAMP_ASR_MODEL and NYAMP_ASR_WAVS\n");
      return 0;
    }

  MemorySlot capture(kCaptureOffset, kCaptureSize);
  nyamp::AtomicGate gate;
  nyamp::LeaseMint mint(kSpeechGeneration);
  nyamp::AsrService asr(
      kSpeechGeneration, [] { return std::uint64_t(0); },
      &nyamp::models::CreateSherpaStreamBackend, &capture, &gate, &mint);
  WireClient client(nyamp::SpeechServices{ &asr, nullptr, nullptr });
  std::int32_t status = 1;
  const std::string directory = model;

  const auto load_started = std::chrono::steady_clock::now();
  CHECK(client.Call(NYAMP_SERVICE_ASR, NYAMP_ASR_LOAD, 1,
                    reinterpret_cast<const std::uint8_t *>(directory.data()),
                    directory.size(), &status));
  CHECK(status == NYAMP_MODEL_OK);
  std::printf("  model loaded in %.0f ms\n",
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - load_started)
                  .count());

  std::string list = wavs;
  std::uint64_t id = 0x9000;
  while (!list.empty())
    {
      const std::size_t colon = list.find(':');
      const std::string path = list.substr(0, colon);
      list = colon == std::string::npos ? "" : list.substr(colon + 1);
      if (!path.empty() &&
          StreamOneFile(asr, client, capture, path, directory, id++) != 0)
        {
          return 1;
        }
    }

  CHECK(client.Call(NYAMP_SERVICE_ASR, NYAMP_ASR_UNLOAD, 2, nullptr, 0,
                    &status));
  CHECK(status == NYAMP_MODEL_OK);
  std::printf("  NYAMPD_ASR_REAL_PASS real_inference=1 mic_capture=0\n");
#else
  std::printf("  skipped: built without the sherpa-onnx runtime\n");
#endif
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
    { "pushed request, float32 and S16", TestPushedRequest },
    { "rewrite, endpoint and long text", TestRewriteEndpointAndLongText },
    { "lease and range checks", TestLeaseAndRangeChecks },
    { "busy, cancel and unload", TestBusyCancelUnload },
    { "deadline and early release", TestDeadlineAndEarlyRelease },
    { "attached to the wake word stream", TestAttachedRequest },
    { "back-pressure sheds partials only", TestBackPressure },
    { "logical model name through BLOB", TestLogicalNameLoad },
    { "real model and recordings", TestRealModel },
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

  std::printf("nyampd asr tests passed (%d groups)\n", passed);
  return 0;
}
