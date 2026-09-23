/****************************************************************************
 * tools/amp/nyampd/nyampd_kws_test.cpp
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

/* The wake word service through the wire, with a scripted spotter that
 * fires on a marker sample, so every reported offset is exact.  One group
 * runs it together with the ASR service: detection, then a recognition
 * request attached to the same stream from the detection's end offset.
 *
 * Built with the sherpa-onnx runtime and given
 *   NYAMP_KWS_MODEL=<dir with encoder/decoder/joiner.onnx, tokens.txt,
 *                    keywords.txt>
 *   NYAMP_KWS_POSITIVE=<wav>[:<wav>...]   NYAMP_KWS_NEGATIVE=<wav>[:...]
 * the last group streams real recordings through the real model: every
 * positive clip must be detected, no negative clip may be.
 */

#include "nyampd_speech_test_support.h"

#ifdef NYAMP_WITH_SHERPA
#include "c-api.h"
#endif

#include <cstdlib>

namespace
{

using namespace nyamp::testing;

constexpr std::uint16_t kKws = NYAMP_SERVICE_KWS;
constexpr std::uint16_t kAsr = NYAMP_SERVICE_ASR;

struct Detection
{
  nyamp_kws_detected_s wire;
  std::string label;
};

struct Listener
{
  std::vector<Detection> detections;
  bool finished = false;
  bool in_order = true;
  std::int32_t status = 1;
  std::uint32_t finish_sequence = 0;

  void Take(const WireEvent &event, std::uint64_t request_id)
  {
    if (event.header.service != kKws ||
        event.header.request_id != request_id ||
        (event.header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_EVENT)
      {
        return;
      }

    if (event.header.opcode == NYAMP_KWS_EVENT_DETECTED)
      {
        Detection detection{};
        if (nyamp_kws_detected_decode(&detection.wire, event.payload.data(),
                                      event.payload.size()) != NYAMP_OK ||
            finished || detection.wire.sequence != detections.size())
          {
            in_order = false;
            return;
          }

        detection.label.assign(detection.wire.label,
                               detection.wire.label_length);
        detection.wire.label = nullptr;
        detections.push_back(detection);
      }
    else if (event.header.opcode == NYAMP_KWS_EVENT_FINISH)
      {
        in_order = in_order && !finished &&
                   nyamp_kws_finish_decode(&status, &finish_sequence,
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
        kws(kSpeechGeneration,
            with_backend
                ? nyamp::KwsService::BackendFactory([this] {
                    return std::make_unique<ScriptedKwsBackend>(&script);
                  })
                : nyamp::KwsService::BackendFactory(),
            &capture, &gate, &mint),
        asr(kSpeechGeneration, [] { return std::uint64_t(0); },
            [this] { return std::make_unique<ScriptedAsrBackend>(&asr_script); },
            &capture, &gate, &mint),
        client(nyamp::SpeechServices{ &asr, nullptr, &kws })
  {
    asr.SetCaptureSource(&kws);
  }

  std::int32_t Load(const std::string &directory = "/models/kws",
                    const std::string &keywords = "", float threshold = 0,
                    std::uint16_t paths = 0)
  {
    nyamp_kws_load_s load{};
    std::uint8_t payload[NYAMP_INLINE_MAX];
    std::size_t size = 0;
    std::int32_t status = 1;

    load.threshold = threshold;
    load.max_active_paths = paths;
    load.directory = directory.data();
    load.directory_length = static_cast<std::uint16_t>(directory.size());
    load.keywords = keywords.data();
    load.keywords_length = static_cast<std::uint16_t>(keywords.size());
    if (nyamp_kws_load_encode(payload, sizeof(payload), &size, &load) !=
            NYAMP_OK ||
        !client.Call(kKws, NYAMP_KWS_LOAD, next_id++, payload, size, &status))
      {
        return 1;
      }

    return status;
  }

  std::int32_t Begin(std::uint64_t id, nyamp_buffer_s *grant,
                     std::uint16_t flags = 0,
                     std::uint32_t window_samples = 16000)
  {
    std::uint8_t payload[NYAMP_KWS_BEGIN_SIZE];
    std::size_t size = 0;
    std::int32_t status = 1;
    std::vector<std::uint8_t> body;

    if (nyamp_kws_begin_encode(payload, sizeof(payload), &size, 16000, 1,
                               flags, window_samples) != NYAMP_OK ||
        !client.Call(kKws, NYAMP_KWS_BEGIN, id, payload, size, &status, &body))
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

  /* Write one window into the grant (ping-pong over two ranges) and PUSH
   * it.  `*next` receives the position the service expects next.
   */

  std::int32_t Push(std::uint64_t id, const nyamp_buffer_s &grant,
                    const std::vector<float> &samples,
                    std::uint64_t stream_sample, std::uint16_t flags = 0,
                    std::uint64_t *next = nullptr)
  {
    nyamp_kws_push_s push{};
    std::uint8_t payload[NYAMP_KWS_PUSH_SIZE];
    std::size_t size = 0;
    std::int32_t status = 1;
    std::vector<std::uint8_t> body;

    push.buffer = grant;
    push.buffer.offset = grant.offset + (sequence % 2) * 64000;
    push.buffer.length = static_cast<std::uint32_t>(samples.size() * 4);
    push.sequence = sequence;
    push.flags = flags;
    push.stream_sample = stream_sample;
    /* A test that pushes a range outside the slot has nowhere to put the
     * samples, and the service must refuse it without looking at them.
     */
    const bool writable =
        push.buffer.offset >= kCaptureOffset &&
        push.buffer.offset + push.buffer.length <= kCaptureOffset + kCaptureSize;

    if (!samples.empty() && writable)
      {
        std::memcpy(capture.at(push.buffer.offset), samples.data(),
                    samples.size() * 4);
      }

    if (nyamp_kws_push_encode(payload, sizeof(payload), &size, &push) !=
            NYAMP_OK ||
        !client.Call(kKws, NYAMP_KWS_PUSH, id, payload, size, &status, &body))
      {
        return 1;
      }

    if (status == NYAMP_MODEL_OK)
      {
        std::uint64_t expected = 0;
        if (nyamp_kws_push_ack_decode(&expected, body.data(), body.size()) !=
            NYAMP_OK)
          {
            return 2;
          }

        if (next != nullptr)
          {
            *next = expected;
          }

        /* The range is the producer's again once the response is here. */
        std::memset(capture.at(push.buffer.offset), 0x5a, samples.size() * 4);
        ++sequence;
      }

    return status;
  }

  std::int32_t Simple(std::uint16_t service, std::uint16_t opcode,
                      std::uint64_t id)
  {
    std::int32_t status = 1;
    return client.Call(service, opcode, id, nullptr, 0, &status) ? status : 1;
  }

  bool Wait(std::uint64_t id, Listener *listener,
            const std::function<bool()> &done, int timeout_ms = 20000)
  {
    return client.Pump(done, timeout_ms, [&](const WireEvent &event) {
      listener->Take(event, id);
    });
  }

  MemorySlot capture;
  nyamp::AtomicGate gate;
  nyamp::LeaseMint mint;
  KwsScript script;
  AsrScript asr_script;
  nyamp::KwsService kws;
  nyamp::AsrService asr;
  WireClient client;
  std::uint64_t next_id = 0x8000;
  std::uint32_t sequence = 0;
};

std::vector<float> Quiet(std::size_t count)
{
  return std::vector<float>(count, 0.01f);
}

/****************************************************************************
 * Tests
 ****************************************************************************/

int TestAvailabilityAndLoad()
{
  Bench bare(false);
  std::int32_t status = 1;
  std::vector<std::uint8_t> body;
  nyamp_buffer_s grant{};

  CHECK(bare.client.Call(NYAMP_SERVICE_HEALTH, 1, 1, nullptr, 0, &status,
                         &body));
  CHECK(body.size() == 8 && (body[4] & (1U << 6)) == 0);
  CHECK((body[4] & (1U << 4)) != 0); /* This bench does have an ASR. */
  CHECK(bare.Load() == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bare.Begin(5, &grant) == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bare.Simple(kKws, NYAMP_KWS_LIST, 6) == NYAMP_MODEL_UNSUPPORTED);
  CHECK(bare.kws.Info() == "none:-2");

  Bench bench;
  CHECK(bench.client.Call(NYAMP_SERVICE_HEALTH, 1, 2, nullptr, 0, &status,
                          &body));
  CHECK((body[4] & (1U << 6)) != 0);

  CHECK(bench.Begin(10, &grant) == NYAMP_MODEL_NOT_READY);
  CHECK(bench.Simple(kKws, NYAMP_KWS_LIST, 11) == NYAMP_MODEL_NOT_READY);
  CHECK(bench.Simple(kKws, NYAMP_KWS_END, 12) == NYAMP_MODEL_NOT_READY);

  /* The keywords file is one component inside the model directory. */
  CHECK(bench.Load("/models/kws", "../secrets") == NYAMP_MODEL_INVALID);
  CHECK(bench.Load("/models/kws", "sub/keywords.txt") == NYAMP_MODEL_INVALID);
  CHECK(bench.Load("/models/kws", "..") == NYAMP_MODEL_INVALID);

  CHECK(bench.Load("/models/kws", "", 0.25f, 8) == NYAMP_MODEL_OK);
  CHECK(bench.script.loaded.directory == "/models/kws");
  CHECK(bench.script.loaded.keywords_file == "/models/kws/keywords.txt");
  CHECK(bench.script.loaded.threshold == 0.25f);
  CHECK(bench.script.loaded.max_active_paths == 8);
  CHECK(bench.script.loaded.score == 0.0f); /* Zero = evaluated default. */
  CHECK(bench.kws.Info() == "ready:0");

  /* The same model again is a success; other parameters are another model
   * and need an UNLOAD first.
   */
  CHECK(bench.Load("/models/kws", "", 0.25f, 8) == NYAMP_MODEL_OK);
  CHECK(bench.Load("/models/kws", "", 0.10f, 8) == NYAMP_MODEL_BUSY);
  CHECK(bench.Load("/models/kws", "setB.txt", 0.25f, 8) == NYAMP_MODEL_BUSY);

  /* LIST: labels in keyword-id order. */
  CHECK(bench.client.Call(kKws, NYAMP_KWS_LIST, 13, nullptr, 0, &status,
                          &body));
  CHECK(status == NYAMP_MODEL_OK);
  std::uint16_t count = 0;
  CHECK(nyamp_kws_labels_decode(&count, body.data(), body.size()) == NYAMP_OK);
  CHECK(count == 2);
  const char *label = nullptr;
  std::uint16_t length = 0;
  std::size_t position = 0;
  CHECK(nyamp_kws_labels_next(&label, &length, &position, body.data(),
                              body.size()) == NYAMP_OK);
  CHECK(std::string(label, length) == "nihao_openvela");
  CHECK(nyamp_kws_labels_next(&label, &length, &position, body.data(),
                              body.size()) == NYAMP_OK);
  CHECK(std::string(label, length) == "hello_openvela");

  CHECK(bench.Simple(kKws, NYAMP_KWS_UNLOAD, 14) == NYAMP_MODEL_OK);
  CHECK(bench.Load("/models/kws", "setB.txt") == NYAMP_MODEL_OK);
  CHECK(bench.script.loaded.keywords_file == "/models/kws/setB.txt");
  return 0;
}

int TestStreamAndOffsets()
{
  Bench bench;
  Listener listener;
  nyamp_buffer_s grant{};
  const std::uint64_t id = 0xabc0;
  std::uint64_t next = 0;

  CHECK(bench.Load() == NYAMP_MODEL_OK);
  CHECK(bench.Begin(id, &grant) == NYAMP_MODEL_OK);
  CHECK(grant.offset == kCaptureOffset && grant.capacity == kCaptureSize);
  CHECK(bench.gate.held());
  CHECK(bench.kws.Info() == "listening:0");
  CHECK(bench.Begin(id + 1, &grant) == NYAMP_MODEL_BUSY);

  /* Three quiet seconds, then the phrase ends at stream sample 60000. */
  for (std::uint64_t second = 0; second < 3; ++second)
    {
      CHECK(bench.Push(id, grant, Quiet(16000), second * 16000, 0, &next) ==
            NYAMP_MODEL_OK);
      CHECK(next == (second + 1) * 16000);
    }

  std::vector<float> wake = Quiet(16000);
  wake[60000 - 48000 - 1] = kKwsMarker;
  CHECK(bench.Push(id, grant, wake, 48000, 0, &next) == NYAMP_MODEL_OK);
  CHECK(next == 64000);

  CHECK(bench.Wait(id, &listener, [&] { return !listener.detections.empty(); }));
  const Detection &hit = listener.detections[0];
  CHECK(hit.label == "nihao_openvela" && hit.wire.keyword_id == 0);
  CHECK((hit.wire.flags & NYAMP_KWS_DETECTED_HAS_OFFSETS) != 0);
  CHECK((hit.wire.flags & NYAMP_KWS_DETECTED_HAS_SCORE) == 0);
  CHECK(hit.wire.end_sample == 60000);
  CHECK(hit.wire.start_sample == 60000 - kKwsPhraseSamples);
  CHECK(hit.wire.trigger_sample > hit.wire.end_sample &&
        hit.wire.trigger_sample <= 64000);

  /* Capture paused while the robot spoke: 5 s never sent.  Not an error;
   * the decoder is reset and positions stay absolute.
   */
  const unsigned int resets = bench.script.resets;
  wake = Quiet(16000);
  wake[999] = kKwsMarker;
  CHECK(bench.Push(id, grant, wake, 144000, NYAMP_KWS_PUSH_DISCONTINUITY,
                   &next) == NYAMP_MODEL_OK);
  CHECK(next == 160000);
  CHECK(bench.Wait(id, &listener,
                   [&] { return listener.detections.size() == 2; }));
  CHECK(bench.script.resets == resets + 1);
  CHECK(listener.detections[1].wire.end_sample == 145000);

  /* A phrase cannot reach back across the hole. */
  CHECK(listener.detections[1].wire.start_sample == 144000);

  /* An unexpected position without the flag is handled the same way... */
  CHECK(bench.Push(id, grant, Quiet(1600), 170000, 0, &next) ==
        NYAMP_MODEL_OK);
  CHECK(next == 171600);
  CHECK(bench.client.Pump([&] { return bench.script.resets == resets + 2; }));

  /* ...but a position that goes back is refused: every offset reported so
   * far would become ambiguous.
   */
  CHECK(bench.Push(id, grant, Quiet(1600), 100000) == NYAMP_MODEL_INVALID);

  /* Window checks: foreign lease, outside the grant, longer than the
   * window size of BEGIN, empty.
   */
  nyamp_buffer_s foreign = grant;
  foreign.lease ^= 4;
  CHECK(bench.Push(id, foreign, Quiet(1600), 171600) == NYAMP_MODEL_INVALID);
  nyamp_buffer_s outside = grant;
  outside.offset = kCaptureOffset + kCaptureSize;
  CHECK(bench.Push(id, outside, Quiet(16), 171600) == NYAMP_MODEL_INVALID);
  CHECK(bench.Push(id, grant, Quiet(16001), 171600) == NYAMP_MODEL_INVALID);
  CHECK(bench.Push(id, grant, Quiet(0), 171600) == NYAMP_MODEL_INVALID);

  /* A trigger whose phrase offsets cannot be right -- the real decoder
   * does this in a long stream -- is still a trigger, but it is sent
   * without offsets, so nobody attaches a recognizer to a minute ago.
   */
  std::uint64_t position = 171600;
  for (int second = 0; second < 3; ++second)
    {
      CHECK(bench.Push(id, grant, Quiet(16000), position) == NYAMP_MODEL_OK);
      position += 16000;
    }

  wake = Quiet(16000);
  wake[8000] = kKwsStaleMarker;
  CHECK(bench.Push(id, grant, wake, position) == NYAMP_MODEL_OK);
  CHECK(bench.Wait(id, &listener,
                   [&] { return listener.detections.size() == 3; }));
  CHECK(listener.detections[2].label == "hello_openvela");
  CHECK((listener.detections[2].wire.flags &
         NYAMP_KWS_DETECTED_HAS_OFFSETS) == 0);
  CHECK(listener.detections[2].wire.start_sample == 0 &&
        listener.detections[2].wire.end_sample == 0);
  CHECK(listener.detections[2].wire.trigger_sample > position &&
        listener.detections[2].wire.trigger_sample <= position + 16000);
  position += 16000;

  /* END: the rest is drained, the slot is returned, FINISH counts events. */
  CHECK(bench.Simple(kKws, NYAMP_KWS_END, 0x1) == NYAMP_MODEL_OK);
  CHECK(bench.Wait(id, &listener, [&] { return listener.finished; }));
  CHECK(listener.status == NYAMP_MODEL_OK && listener.in_order);
  CHECK(listener.finish_sequence == 3 && listener.detections.size() == 3);
  CHECK(!bench.gate.held());
  CHECK(bench.Push(id, grant, Quiet(1600), 171600) == NYAMP_MODEL_NOT_READY);
  CHECK(bench.kws.Info() == "ready:0");

  /* A new stream counts from zero again, with a new lease, S16 this time. */
  nyamp_buffer_s second{};
  CHECK(bench.Begin(id + 2, &second, NYAMP_AUDIO_BEGIN_S16, 1600) ==
        NYAMP_MODEL_OK);
  CHECK(second.lease != grant.lease && second.format == NYAMP_FORMAT_S16);
  CHECK(bench.Simple(kKws, NYAMP_KWS_END, 0x2) == NYAMP_MODEL_OK);
  Listener empty;
  CHECK(bench.Wait(id + 2, &empty, [&] { return empty.finished; }));
  CHECK(empty.status == NYAMP_MODEL_OK && empty.finish_sequence == 0);
  return 0;
}

int TestCancelAndUnload()
{
  Bench bench;
  nyamp_buffer_s grant{};
  const std::uint64_t id = 0xabd0;

  CHECK(bench.Load() == NYAMP_MODEL_OK);

  /* A CANCEL-kind frame naming the BEGIN. */
  Listener first;
  CHECK(bench.Begin(id, &grant) == NYAMP_MODEL_OK);
  CHECK(bench.client.CancelFrame(kKws, id + 9)); /* Not this stream. */
  CHECK(bench.Push(id, grant, Quiet(1600), 0) == NYAMP_MODEL_OK);
  CHECK(bench.client.CancelFrame(kKws, id));
  CHECK(bench.Wait(id, &first, [&] { return first.finished; }));
  CHECK(first.status == NYAMP_MODEL_CANCELLED && !bench.gate.held());

  /* UNLOAD ends the stream: BUSY now, OK after the FINISH. */
  Listener second;
  bench.sequence = 0;
  CHECK(bench.Begin(id + 1, &grant) == NYAMP_MODEL_OK);
  CHECK(bench.Simple(kKws, NYAMP_KWS_UNLOAD, 0x10) == NYAMP_MODEL_BUSY);
  CHECK(bench.Wait(id + 1, &second, [&] { return second.finished; }));
  CHECK(second.status == NYAMP_MODEL_OK);
  CHECK(bench.Simple(kKws, NYAMP_KWS_UNLOAD, 0x11) == NYAMP_MODEL_OK);
  CHECK(bench.Begin(id + 2, &grant) == NYAMP_MODEL_NOT_READY);

  /* Without the shared region there is no stream. */
  CHECK(bench.Load() == NYAMP_MODEL_OK);
  bench.capture.mapped.store(false);
  CHECK(bench.Begin(id + 3, &grant) == NYAMP_MODEL_UNSUPPORTED);
  CHECK(!bench.gate.held());
  return 0;
}

/* Wake word, then the command, over one stream: the recognizer attaches at
 * the detection's end offset, so the command is neither lost nor sent twice.
 */

int TestWakeThenAttachedAsr()
{
  Bench bench;
  Listener listener;
  nyamp_buffer_s grant{};
  const std::uint64_t stream = 0xabe0;
  const std::uint64_t asr_id = 0xabe1;
  std::int32_t status = 1;

  bench.asr_script.steps = { { 16000, "现在", false },
                             { 28000, "现在几点", true } };

  CHECK(bench.Load() == NYAMP_MODEL_OK);
  const std::string asr_model = "/models/asr";
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_LOAD, 1,
                          reinterpret_cast<const std::uint8_t *>(
                              asr_model.data()),
                          asr_model.size(), &status));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(bench.Begin(stream, &grant) == NYAMP_MODEL_OK);

  /* While the stream owns the capture slot a pushed ASR request is BUSY:
   * the slot has one owner, and the lease says who.
   */
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t size = 0;
  CHECK(nyamp_asr_begin_encode(payload, sizeof(payload), &size, 16000, 1, 0,
                               0) == NYAMP_OK);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_BEGIN, asr_id + 50, payload, size,
                          &status));
  CHECK(status == NYAMP_MODEL_BUSY);

  /* "你好 openvela" ends at 20000; the user keeps talking. */
  std::vector<float> window = Quiet(16000);
  CHECK(bench.Push(stream, grant, window, 0) == NYAMP_MODEL_OK);
  window[3999] = kKwsMarker;
  CHECK(bench.Push(stream, grant, window, 16000) == NYAMP_MODEL_OK);
  CHECK(bench.Wait(stream, &listener,
                   [&] { return !listener.detections.empty(); }));
  const std::uint64_t wake_end = listener.detections[0].wire.end_sample;
  CHECK(wake_end == 20000);

  /* More of the command arrives before the control domain has reacted. */
  CHECK(bench.Push(stream, grant, Quiet(16000), 32000) == NYAMP_MODEL_OK);

  CHECK(nyamp_asr_attach_encode(payload, sizeof(payload), &size, 16000, 1, 0,
                                0, wake_end) == NYAMP_OK);
  std::vector<std::uint8_t> body;
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_BEGIN, asr_id, payload, size,
                          &status, &body));
  CHECK(status == NYAMP_MODEL_OK && body.empty());

  /* The pushes keep going to KWS only; both consumers see them. */
  CHECK(bench.Push(stream, grant, Quiet(16000), 48000) == NYAMP_MODEL_OK);

  /* The recognizer reports its endpoint; the control domain ends the
   * request at the position it chose.
   */
  bool endpoint = false;
  std::string text;
  bool asr_finished = false;
  std::int32_t asr_status = 1;
  std::uint32_t consumed = 0;
  const auto take = [&](const WireEvent &event) {
    listener.Take(event, stream);
    if (event.header.service != kAsr || event.header.request_id != asr_id)
      {
        return;
      }

    if (event.header.opcode == NYAMP_ASR_EVENT_PARTIAL)
      {
        std::uint32_t sequence = 0;
        std::uint16_t flags = 0;
        const char *bytes = nullptr;
        std::size_t length = 0;
        if (nyamp_asr_partial_decode(&sequence, &consumed, &flags, &bytes,
                                     &length, event.payload.data(),
                                     event.payload.size()) == NYAMP_OK)
          {
            if ((flags & NYAMP_ASR_PARTIAL_RESYNC) != 0)
              {
                text.clear();
              }

            text.append(bytes, length);
            endpoint = endpoint || (flags & NYAMP_ASR_PARTIAL_ENDPOINT) != 0;
          }
      }
    else if (event.header.opcode == NYAMP_ASR_EVENT_FINISH)
      {
        std::uint32_t sequence = 0;
        asr_finished = nyamp_asr_finish_decode(&asr_status, &sequence,
                                               event.payload.data(),
                                               event.payload.size()) ==
                       NYAMP_OK;
      }
  };

  CHECK(bench.client.Pump([&] { return endpoint; }, 20000, take));
  CHECK(text == "现在几点");

  CHECK(nyamp_asr_end_encode(payload, sizeof(payload), &size, 64000) ==
        NYAMP_OK);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_END, asr_id, payload, size,
                          &status));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(bench.client.Pump([&] { return asr_finished; }, 20000, take));
  CHECK(asr_status == NYAMP_MODEL_OK && text == "现在几点");

  /* Exactly the samples from the wake word's end to the named end: nothing
   * lost, nothing twice.
   */
  CHECK(consumed == 64000 - wake_end);
  CHECK(bench.asr_script.accepted == 64000 - wake_end);

  /* The wake word stream never noticed. */
  CHECK(bench.client.Pump([&] { return bench.script.accepted == 64000; },
                          20000, take));
  CHECK(bench.gate.held() && !listener.finished);

  /* An attached request outlives neither the stream nor its END. */
  CHECK(nyamp_asr_attach_encode(payload, sizeof(payload), &size, 16000, 1, 0,
                                0, 64000) == NYAMP_OK);
  CHECK(bench.client.Call(kAsr, NYAMP_ASR_BEGIN, asr_id + 1, payload, size,
                          &status, &body));
  CHECK(status == NYAMP_MODEL_OK);
  asr_finished = false;
  CHECK(bench.Simple(kKws, NYAMP_KWS_END, 0x20) == NYAMP_MODEL_OK);
  bool second_finished = false;
  CHECK(bench.client.Pump(
      [&] { return second_finished && listener.finished; }, 20000,
      [&](const WireEvent &event) {
        listener.Take(event, stream);
        if (event.header.service == kAsr &&
            event.header.request_id == asr_id + 1 &&
            event.header.opcode == NYAMP_ASR_EVENT_FINISH)
          {
            second_finished = true;
          }
      }));
  CHECK(!bench.gate.held());
  return 0;
}

/****************************************************************************
 * Real model, real recordings.
 ****************************************************************************/

#ifdef NYAMP_WITH_SHERPA
std::vector<std::string> Split(const char *list)
{
  std::vector<std::string> parts;
  std::string rest = list == nullptr ? "" : list;

  while (!rest.empty())
    {
      const std::size_t colon = rest.find(':');
      if (colon != 0)
        {
          parts.push_back(rest.substr(0, colon));
        }

      rest = colon == std::string::npos ? "" : rest.substr(colon + 1);
    }

  return parts;
}
#endif

int TestRealModel()
{
#ifdef NYAMP_WITH_SHERPA
  const char *model = std::getenv("NYAMP_KWS_MODEL");
  const std::vector<std::string> positives =
      Split(std::getenv("NYAMP_KWS_POSITIVE"));
  const std::vector<std::string> negatives =
      Split(std::getenv("NYAMP_KWS_NEGATIVE"));

  if (model == nullptr || positives.empty() || negatives.empty())
    {
      std::printf("  skipped: set NYAMP_KWS_MODEL, NYAMP_KWS_POSITIVE and "
                  "NYAMP_KWS_NEGATIVE\n");
      return 0;
    }

  MemorySlot capture(kCaptureOffset, kCaptureSize);
  nyamp::AtomicGate gate;
  nyamp::LeaseMint mint(kSpeechGeneration);
  nyamp::KwsService kws(kSpeechGeneration, &nyamp::CreateSherpaKwsBackend,
                        &capture, &gate, &mint);
  WireClient client(nyamp::SpeechServices{ nullptr, nullptr, &kws });
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t size = 0;
  std::int32_t status = 1;
  std::vector<std::uint8_t> body;
  const std::string directory = model;
  const std::uint64_t id = 0xc000;

  nyamp_kws_load_s load{};
  load.directory = directory.data();
  load.directory_length = static_cast<std::uint16_t>(directory.size());
  CHECK(nyamp_kws_load_encode(payload, sizeof(payload), &size, &load) ==
        NYAMP_OK);
  const auto load_started = std::chrono::steady_clock::now();
  CHECK(client.Call(kKws, NYAMP_KWS_LOAD, 1, payload, size, &status));
  CHECK(status == NYAMP_MODEL_OK);
  const double load_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - load_started)
                             .count();

  CHECK(client.Call(kKws, NYAMP_KWS_LIST, 2, nullptr, 0, &status, &body));
  std::uint16_t labels = 0;
  CHECK(status == NYAMP_MODEL_OK &&
        nyamp_kws_labels_decode(&labels, body.data(), body.size()) ==
            NYAMP_OK);
  CHECK(labels >= 1);

  nyamp_buffer_s grant{};
  CHECK(nyamp_kws_begin_encode(payload, sizeof(payload), &size, 16000, 1, 0,
                               16000) == NYAMP_OK);
  CHECK(client.Call(kKws, NYAMP_KWS_BEGIN, id, payload, size, &status, &body));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(nyamp_buffer_decode(&grant, body.data(), body.size()) == NYAMP_OK);

  /* One always-on stream carries every clip, each followed by a second of
   * silence (the encoder needs about 0.4 s of what follows the phrase).
   * A clip's detections are those whose trigger falls in its span.
   */
  struct Span
  {
    std::string path;
    bool positive;
    std::uint64_t first;
    std::uint64_t last;
  };

  std::vector<Span> spans;
  Listener listener;
  std::uint64_t position = 0;
  std::uint32_t sequence = 0;
  double audio_seconds = 0;
  const auto take = [&](const WireEvent &event) { listener.Take(event, id); };
  const auto started = std::chrono::steady_clock::now();

  bool fresh_decoder = false;
  auto push = [&](const float *samples, std::size_t count) {
    nyamp_kws_push_s window{};
    window.buffer = grant;
    window.buffer.offset = grant.offset + (sequence % 2) * 64000;
    window.buffer.length = static_cast<std::uint32_t>(count * 4);
    window.sequence = sequence++;
    window.flags = fresh_decoder ? NYAMP_KWS_PUSH_DISCONTINUITY : 0;
    fresh_decoder = false;
    window.stream_sample = position;
    std::memcpy(capture.at(window.buffer.offset), samples, count * 4);
    position += count;
    return nyamp_kws_push_encode(payload, sizeof(payload), &size, &window) ==
               NYAMP_OK &&
           client.Call(kKws, NYAMP_KWS_PUSH, id, payload, size, &status) &&
           status == NYAMP_MODEL_OK;
  };

  auto stream_file = [&](const std::string &path, bool positive,
                         bool isolate) {
    /* Isolated: the clip starts with a DISCONTINUITY, the way capture
     * resumes after the robot has spoken, so the decoder starts clean and
     * the result can be compared with the per-file evaluation.  Otherwise
     * the clip simply follows the previous one in an always-on stream.
     *
     * Back-to-back positive clips are NOT streamed without the reset: the
     * evaluation tool shows the same thing in its --continuous mode -- a
     * phrase the model missed can complete seconds later on the next
     * clip's audio, with offsets that point back at the first one.
     */
    fresh_decoder = isolate;
    if (isolate)
      {
        /* A discontinuity drops what the listener has not read yet, as it
         * must; a real pause is seconds long, so give it a moment here.
         */
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }

    const SherpaOnnxWave *wave = SherpaOnnxReadWave(path.c_str());
    if (wave == nullptr || wave->sample_rate != 16000)
      {
        std::fprintf(stderr, "cannot read %s as 16 kHz\n", path.c_str());
        return false;
      }

    std::vector<float> audio(wave->samples, wave->samples + wave->num_samples);
    SherpaOnnxFreeWave(wave);
    audio.resize(audio.size() + 16000, 0.0f);
    audio_seconds += static_cast<double>(audio.size()) / 16000.0;

    Span span{ path, positive, position, 0 };
    for (std::size_t offset = 0; offset < audio.size(); offset += 16000)
      {
        const std::size_t count =
            std::min<std::size_t>(16000, audio.size() - offset);
        if (!push(audio.data() + offset, count))
          {
            return false;
          }

        /* The ring holds ten seconds; stay inside it. */
        client.Pump([&] { return true; }, 0, take);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }

    span.last = position;
    spans.push_back(span);
    return true;
  };

  for (const std::string &path : positives)
    {
      CHECK(stream_file(path, true, true));
    }

  /* The negatives run as one uninterrupted stream: nothing may fire, and
   * nothing may be left over from the positives either.
   */
  bool first_negative = true;
  for (const std::string &path : negatives)
    {
      CHECK(stream_file(path, false, first_negative));
      first_negative = false;
    }

  CHECK(client.Call(kKws, NYAMP_KWS_END, 3, nullptr, 0, &status));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(client.Pump([&] { return listener.finished; }, 300000, take));
  const double wall = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  CHECK(listener.status == NYAMP_MODEL_OK && listener.in_order);

  unsigned int missed = 0;
  unsigned int false_alarms = 0;
  for (const Span &span : spans)
    {
      unsigned int hits = 0;
      for (const Detection &detection : listener.detections)
        {
          if (detection.wire.trigger_sample > span.first &&
              detection.wire.trigger_sample <= span.last)
            {
              ++hits;
              std::printf("    %s  id=%u start=%.2fs end=%.2fs trigger=%.2fs "
                          "(in clip)\n",
                          detection.label.c_str(), detection.wire.keyword_id,
                          (static_cast<double>(detection.wire.start_sample) -
                           static_cast<double>(span.first)) /
                              16000.0,
                          (static_cast<double>(detection.wire.end_sample) -
                           static_cast<double>(span.first)) /
                              16000.0,
                          (static_cast<double>(detection.wire.trigger_sample) -
                           static_cast<double>(span.first)) /
                              16000.0);
              /* A clip that started on a clean decoder has its phrase
               * inside the clip.
               */
              if (span.positive)
                {
                  CHECK((detection.wire.flags &
                         NYAMP_KWS_DETECTED_HAS_OFFSETS) != 0);
                  CHECK(detection.wire.start_sample >= span.first &&
                        detection.wire.start_sample <
                            detection.wire.end_sample &&
                        detection.wire.end_sample <=
                            detection.wire.trigger_sample);
                }
            }
        }

      std::printf("  %s %s: %u detection(s)\n",
                  span.positive ? "positive" : "negative", span.path.c_str(),
                  hits);
      missed += span.positive && hits == 0 ? 1 : 0;
      false_alarms += span.positive ? 0 : hits;
    }

  std::printf("  load %.0f ms; %.1f s of audio streamed in %.1f s wall "
              "(paced, not a speed measurement); %zu positive, %zu negative "
              "clips\n",
              load_ms, audio_seconds, wall, positives.size(),
              negatives.size());
  CHECK(missed == 0);
  CHECK(false_alarms == 0);
  std::printf("  NYAMPD_KWS_REAL_PASS real_inference=1 mic_capture=0\n");
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
    { "availability, load parameters and list", TestAvailabilityAndLoad },
    { "stream, offsets and discontinuities", TestStreamAndOffsets },
    { "cancel and unload", TestCancelAndUnload },
    { "wake word, then ASR attached to the stream", TestWakeThenAttachedAsr },
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

  std::printf("nyampd kws tests passed (%d groups)\n", passed);
  return 0;
}
