/****************************************************************************
 * tools/amp/nyampd/nyampd_speech_test_support.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_SPEECH_TEST_SUPPORT_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_SPEECH_TEST_SUPPORT_H

/* Shared by the ASR, TTS and KWS tests and by tools/amp/test_voice_flow.py:
 * a slot that is plain memory, a wire-level client that talks to the
 * services only through Dispatch and their event queues, and scripted
 * stand-ins for the three model backends.  Test code only; nothing here is
 * linked into the daemon.
 */

#include "nyamp_protocol.h"
#include "nyamp_streaming.h"
#include "nyampd_asr.h"
#include "nyampd_audio.h"
#include "nyampd_core.h"
#include "nyampd_kws.h"
#include "nyampd_tts.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef CHECK
#define CHECK(expression)                                                 \
  do                                                                      \
    {                                                                     \
      if (!(expression))                                                  \
        {                                                                 \
          std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
                       #expression);                                      \
          return 1;                                                       \
        }                                                                 \
    }                                                                     \
  while (0)
#endif

namespace nyamp::testing
{

inline constexpr std::uint32_t kSpeechGeneration = 0x5bee0c01U;

/* The arena offsets the daemon uses, so a descriptor in a test looks like
 * one on the board.
 */

inline constexpr std::uint32_t kSharedOffset = 0x00001000U;
inline constexpr std::uint32_t kSharedSize = 0x00100000U;
inline constexpr std::uint32_t kCaptureOffset = 0x00120000U;
inline constexpr std::uint32_t kCaptureSize = 0x00040000U;

/****************************************************************************
 * Name: MemorySlot
 ****************************************************************************/

class MemorySlot final : public nyamp::SharedSlot
{
public:
  MemorySlot(std::uint32_t offset, std::uint32_t capacity)
      : offset_(offset), bytes_(capacity)
  {
  }

  std::uint8_t *data() override
  {
    return mapped.load() ? bytes_.data() : nullptr;
  }

  std::uint32_t offset() const override { return offset_; }
  std::uint32_t capacity() const override
  {
    return static_cast<std::uint32_t>(bytes_.size());
  }

  /* Arena-absolute access, the way the control domain addresses it. */
  std::uint8_t *at(std::uint32_t arena_offset)
  {
    return bytes_.data() + (arena_offset - offset_);
  }

  std::atomic<bool> mapped{ true };

private:
  std::uint32_t offset_;
  std::vector<std::uint8_t> bytes_;
};

/****************************************************************************
 * Name: WireClient
 *
 * Description:
 *   The control domain, reduced to what the speech services see of it:
 *   encoded requests into Dispatch, decoded responses out, and the event
 *   frames the services queue.  It owns no service; it is handed them.
 *
 ****************************************************************************/

struct WireEvent
{
  nyamp_header_s header;
  std::vector<std::uint8_t> payload;
};

class WireClient
{
public:
  explicit WireClient(const nyamp::SpeechServices &services,
                      std::uint32_t generation = kSpeechGeneration)
      : services_(services), generation_(generation)
  {
  }

  /* One request, one response.  Returns false when Dispatch refused the
   * frame or the response is not the answer to it.  `*deferred` reports a
   * request whose response comes later through the event queue.
   */

  bool Call(std::uint16_t service, std::uint16_t opcode,
            std::uint64_t request_id, const std::uint8_t *payload,
            std::size_t payload_size, std::int32_t *status,
            std::vector<std::uint8_t> *body = nullptr,
            bool *deferred = nullptr, std::uint64_t deadline_ms = 0)
  {
    std::uint8_t wire[NYAMP_RPMSG_MTU];
    std::uint8_t response[NYAMP_RPMSG_MTU];
    std::size_t response_size = 0;
    nyamp_header_s header{};

    header.service = service;
    header.opcode = opcode;
    header.flags = NYAMP_FLAG_REQUEST;
    header.request_id = request_id;
    header.deadline_ms = deadline_ms;
    header.generation = generation_;
    header.payload_size = static_cast<std::uint32_t>(payload_size);
    if (payload_size > NYAMP_INLINE_MAX ||
        nyamp_header_encode(wire, sizeof(wire), &header) != NYAMP_OK)
      {
        return false;
      }

    if (payload_size != 0)
      {
        std::memcpy(wire + NYAMP_WIRE_HEADER_SIZE, payload, payload_size);
      }

    if (nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE + payload_size, now_ms,
                        generation_, response, sizeof(response),
                        &response_size, {}, nullptr, nullptr,
                        &services_) != NYAMP_OK)
      {
        return false;
      }

    if (deferred != nullptr)
      {
        *deferred = response_size == 0;
      }

    if (response_size == 0)
      {
        return deferred != nullptr;
      }

    nyamp_header_s answer{};
    const std::uint8_t *answer_body = nullptr;
    std::size_t answer_size = 0;

    if (nyamp_header_decode(&answer, response, response_size) != NYAMP_OK ||
        answer.request_id != request_id || answer.opcode != opcode ||
        answer.service != service ||
        (answer.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_RESPONSE ||
        nyamp_status_decode(status, &answer_body, &answer_size,
                            response + NYAMP_WIRE_HEADER_SIZE,
                            answer.payload_size) != NYAMP_OK)
      {
        return false;
      }

    if (body != nullptr)
      {
        body->assign(answer_body, answer_body + answer_size);
      }

    return true;
  }

  /* A CANCEL-kind frame: no payload, no response. */

  bool CancelFrame(std::uint16_t service, std::uint64_t request_id)
  {
    std::uint8_t wire[NYAMP_RPMSG_MTU];
    std::uint8_t response[NYAMP_RPMSG_MTU];
    std::size_t response_size = 1;
    nyamp_header_s header{};

    header.service = service;
    header.opcode = 1;
    header.flags = NYAMP_FLAG_CANCEL;
    header.request_id = request_id;
    header.generation = generation_;
    return nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK &&
           nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE, now_ms, generation_,
                           response, sizeof(response), &response_size, {},
                           nullptr, nullptr, &services_) == NYAMP_OK &&
           response_size == 0;
  }

  /* Drain the three queues once. */

  void Drain()
  {
    nyamp::Frame frame;

    for (;;)
      {
        const bool got =
            (services_.asr != nullptr && services_.asr->Poll(&frame)) ||
            (services_.tts != nullptr && services_.tts->Poll(&frame)) ||
            (services_.kws != nullptr && services_.kws->Poll(&frame));
        if (!got)
          {
            return;
          }

        WireEvent event{};
        if (nyamp_header_decode(&event.header, frame.data, frame.size) ==
                NYAMP_OK &&
            frame.size == NYAMP_WIRE_HEADER_SIZE + event.header.payload_size)
          {
            event.payload.assign(frame.data + NYAMP_WIRE_HEADER_SIZE,
                                 frame.data + frame.size);
            events.push_back(std::move(event));
          }
        else
          {
            ++malformed;
          }
      }
  }

  /* Drain until `done` says so.  `on_event` sees every new event once, in
   * order, and may issue requests of its own (a RELEASE, say).
   */

  bool Pump(const std::function<bool()> &done, int timeout_ms = 20000,
            const std::function<void(const WireEvent &)> &on_event = {})
  {
    const auto give_up = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout_ms);

    for (;;)
      {
        Drain();
        while (seen_ < events.size())
          {
            /* By value: on_event may issue a request that grows `events`. */
            const WireEvent event = events[seen_++];
            if (on_event)
              {
                on_event(event);
              }
          }

        if (done())
          {
            return true;
          }

        if (std::chrono::steady_clock::now() >= give_up)
          {
            return false;
          }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
  }

  const WireEvent *Find(std::uint16_t service, std::uint16_t opcode,
                        std::uint64_t request_id,
                        std::uint32_t kind = NYAMP_FLAG_EVENT) const
  {
    for (const WireEvent &event : events)
      {
        if (event.header.service == service && event.header.opcode == opcode &&
            event.header.request_id == request_id &&
            (event.header.flags & NYAMP_FLAG_KIND_MASK) == kind)
          {
            return &event;
          }
      }

    return nullptr;
  }

  std::vector<WireEvent> events;
  unsigned int malformed = 0;
  std::uint64_t now_ms = 0;

private:
  nyamp::SpeechServices services_;
  std::uint32_t generation_;
  std::size_t seen_ = 0;
};

/****************************************************************************
 * Name: ScriptedAsr
 *
 * Description:
 *   A recognizer whose hypothesis is a function of how many samples it has
 *   been given, so a test knows the exact text after any window.  A step
 *   with `replace` models the transducer rewriting earlier tokens.
 *
 ****************************************************************************/

struct AsrScript
{
  struct Step
  {
    std::uint64_t samples; /* Reached when this many were accepted. */
    std::string text;      /* The whole hypothesis from then on.    */
    bool endpoint;
  };

  std::vector<Step> steps;
  std::string final_suffix; /* Appended by the flush.               */
  int decode_delay_ms = 0;
  bool fail_load = false;

  std::atomic<std::uint64_t> accepted{ 0 };
  std::atomic<unsigned int> streams{ 0 };
  std::atomic<unsigned int> live_streams{ 0 };
  std::atomic<unsigned int> loads{ 0 };
  std::atomic<unsigned int> unloads{ 0 };
  std::atomic<double> energy{ 0 }; /* Sum of |sample|, to prove content. */
  std::mutex mutex;
  std::string loaded_directory;
};

class ScriptedAsrStream final : public nyamp::models::AsrStream
{
public:
  explicit ScriptedAsrStream(AsrScript *script) : script_(script)
  {
    ++script_->streams;
    ++script_->live_streams;
    script_->accepted.store(0);
  }

  ~ScriptedAsrStream() override { --script_->live_streams; }

  void Accept(const float *samples, std::size_t count) override
  {
    double sum = 0;
    for (std::size_t index = 0; index < count; ++index)
      {
        sum += std::fabs(samples[index]);
      }

    script_->energy.store(script_->energy.load() + sum);
    accepted_ += count;
    script_->accepted.store(accepted_);
  }

  bool Decode(const nyamp::models::Stop &stop) override
  {
    if (script_->decode_delay_ms > 0)
      {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(script_->decode_delay_ms));
      }

    return !(stop && stop());
  }

  bool Text(std::string *text) override
  {
    text->clear();
    for (const AsrScript::Step &step : script_->steps)
      {
        if (accepted_ >= step.samples)
          {
            *text = step.text;
          }
      }

    if (finished_)
      {
        *text += script_->final_suffix;
      }

    return true;
  }

  bool Endpoint() override
  {
    bool endpoint = false;
    for (const AsrScript::Step &step : script_->steps)
      {
        if (accepted_ >= step.samples)
          {
            endpoint = step.endpoint;
          }
      }

    return endpoint;
  }

  void InputFinished() override { finished_ = true; }

private:
  AsrScript *script_;
  std::uint64_t accepted_ = 0;
  bool finished_ = false;
};

class ScriptedAsrBackend final : public nyamp::models::AsrStreamBackend
{
public:
  explicit ScriptedAsrBackend(AsrScript *script) : script_(script) {}

  nyamp::models::Status Load(const std::string &directory) override
  {
    ++script_->loads;
    std::lock_guard<std::mutex> lock(script_->mutex);
    script_->loaded_directory = directory;
    return script_->fail_load ? nyamp::models::Status::kBackendError
                              : nyamp::models::Status::kOk;
  }

  void Unload() override { ++script_->unloads; }

  std::unique_ptr<nyamp::models::AsrStream> CreateStream() override
  {
    return std::make_unique<ScriptedAsrStream>(script_);
  }

private:
  AsrScript *script_;
};

/****************************************************************************
 * Name: ScriptedKws
 *
 * Description:
 *   A spotter that fires on a marker: a sample of exactly kKwsMarker is "the
 *   last sample of the wake phrase", which is said to have started
 *   kKwsPhraseSamples earlier.  The trigger position is the end of the
 *   window that carried it, as with the real decoder.
 *
 ****************************************************************************/

inline constexpr float kKwsMarker = 0.75f;
inline constexpr float kKwsStaleMarker = 0.5f;
inline constexpr std::uint64_t kKwsPhraseSamples = 8000;

struct KwsScript
{
  std::vector<std::string> labels{ "nihao_openvela", "hello_openvela" };
  bool fail_load = false;
  std::atomic<unsigned int> resets{ 0 };
  std::atomic<std::uint64_t> accepted{ 0 };
  std::mutex mutex;
  nyamp::KwsModel loaded;
};

class ScriptedKwsBackend final : public nyamp::KwsBackend
{
public:
  explicit ScriptedKwsBackend(KwsScript *script) : script_(script) {}

  nyamp::models::Status Load(const nyamp::KwsModel &model) override
  {
    std::lock_guard<std::mutex> lock(script_->mutex);
    script_->loaded = model;
    return script_->fail_load ? nyamp::models::Status::kBackendError
                              : nyamp::models::Status::kOk;
  }

  void Unload() override {}

  nyamp::models::Status Accept(const float *samples, std::size_t count,
                               const Sink &sink,
                               const nyamp::models::Stop &stop) override
  {
    if (stop && stop())
      {
        return nyamp::models::Status::kCancelled;
      }

    for (std::size_t index = 0; index < count; ++index)
      {
        if (samples[index] == kKwsStaleMarker)
          {
            /* What the real decoder has been seen to do in a long stream:
             * a genuine trigger whose "phrase" is tokens from long ago.
             */
            nyamp::KwsHit hit;

            hit.label = script_->labels[1];
            hit.keyword_id = 1;
            hit.has_offsets = true;
            hit.start = 0;
            hit.end = 160;
            hit.trigger = accepted_ + count;
            sink(hit);
          }

        if (samples[index] == kKwsMarker)
          {
            nyamp::KwsHit hit;
            const std::uint64_t end = accepted_ + index + 1;

            hit.label = script_->labels[0];
            hit.keyword_id = 0;
            hit.has_offsets = true;
            hit.start = end > kKwsPhraseSamples ? end - kKwsPhraseSamples : 0;
            hit.end = end;
            hit.trigger = accepted_ + count;
            sink(hit);
          }
      }

    accepted_ += count;
    script_->accepted.store(script_->accepted.load() + count);
    return nyamp::models::Status::kOk;
  }

  nyamp::models::Status Reset() override
  {
    accepted_ = 0;
    ++script_->resets;
    return nyamp::models::Status::kOk;
  }

  std::vector<std::string> Labels() const override { return script_->labels; }

private:
  KwsScript *script_;
  std::uint64_t accepted_ = 0;
};

/****************************************************************************
 * Name: ScriptedFrontend / ScriptedVocoder
 *
 * Description:
 *   The front end makes one unit per sentence ('.' ends one) and one
 *   phoneme per byte; a sentence over the budget is cut at spaces, and one
 *   without a space to cut at comes back over_budget, as the real front end
 *   does.  The vocoder turns N phonemes into N * frames_per_phoneme / speed
 *   latent frames of 512 samples and refuses more than the bucket, as the
 *   real one does.  UnitSample gives sample k of a unit, so a test can tell
 *   from a window which unit and which offset it holds.
 *
 ****************************************************************************/

/* Exact in float32: the first sample names the unit, the rest the offset. */

inline float UnitSample(std::int64_t first_phoneme, std::size_t index)
{
  return index == 0 ? static_cast<float>(first_phoneme % 256) / 256.0f
                    : static_cast<float>(index % 1000) / 1000.0f;
}

struct TtsScript
{
  double frames_per_phoneme = 8;

  /* What the front end believes a phoneme costs.  Lower than the truth
   * makes it under-split, which is how the re-split path is reached.
   */
  std::size_t believed_frames_per_phoneme = 8;
  int synth_delay_ms = 0;
  bool fail_load = false;
  std::atomic<unsigned int> runs{ 0 };
  std::atomic<unsigned int> refused{ 0 };
  std::mutex mutex;
  std::vector<std::string> units; /* Text of every unit synthesized. */
  std::vector<std::vector<std::int64_t> > unit_ids;
  std::vector<std::size_t> unit_frames;
  std::string loaded_directory;
};

class ScriptedFrontend final : public nyamp::TtsFrontend
{
public:
  explicit ScriptedFrontend(TtsScript *script) : script_(script) {}

  nyamp::models::Status Load(const std::string &) override
  {
    return nyamp::models::Status::kOk;
  }

  std::vector<nyamp::TtsUnit> Process(const std::string &text,
                                      std::size_t max_frames) override
  {
    const std::size_t budget = std::max<std::size_t>(
        1, max_frames / script_->believed_frames_per_phoneme);
    std::vector<nyamp::TtsUnit> units;
    std::size_t from = 0;

    while (from < text.size())
      {
        std::size_t to = text.find('.', from);
        to = to == std::string::npos ? text.size() : to + 1;
        std::string sentence = text.substr(from, to - from);
        from = to;
        sentence.erase(0, sentence.find_first_not_of(' '));

        while (!sentence.empty())
          {
            std::size_t cut = sentence.size();
            bool over = false;

            if (cut > budget)
              {
                cut = sentence.rfind(' ', budget);
                if (cut == std::string::npos || cut == 0)
                  {
                    cut = sentence.size();
                    over = true;
                  }
              }

            nyamp::TtsUnit unit;
            unit.text = sentence.substr(0, cut);
            unit.over_budget = over;
            for (const char byte : unit.text)
              {
                unit.phonemes.push_back(static_cast<unsigned char>(byte));
                unit.tones.push_back(0);
              }

            units.push_back(std::move(unit));
            sentence.erase(0, cut);
            while (!sentence.empty() && sentence[0] == ' ')
              {
                sentence.erase(0, 1);
              }
          }
      }

    return units;
  }

private:
  TtsScript *script_;
};

class ScriptedVocoder final : public nyamp::models::Backend
{
public:
  explicit ScriptedVocoder(TtsScript *script) : script_(script) {}

  nyamp::models::Kind kind() const override
  {
    return nyamp::models::Kind::kTts;
  }

  nyamp::models::Status Load(const std::string &directory) override
  {
    std::lock_guard<std::mutex> lock(script_->mutex);
    script_->loaded_directory = directory;
    return script_->fail_load ? nyamp::models::Status::kBackendError
                              : nyamp::models::Status::kOk;
  }

  void Unload() override {}

  nyamp::models::Status Run(const nyamp::models::Input &input,
                            const nyamp::models::Emit &emit,
                            const nyamp::models::Stop &stop) override
  {
    const auto *text = std::get_if<nyamp::models::TtsInput>(&input);
    if (text == nullptr || text->phoneme_ids.empty())
      {
        return nyamp::models::Status::kInvalid;
      }

    ++script_->runs;
    if (script_->synth_delay_ms > 0)
      {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(script_->synth_delay_ms));
      }

    if (stop())
      {
        return nyamp::models::Status::kCancelled;
      }

    const std::size_t frames = static_cast<std::size_t>(
        static_cast<double>(text->phoneme_ids.size()) *
            script_->frames_per_phoneme / static_cast<double>(text->speed) +
        1e-6);
    if (frames > nyamp::kTtsBucketFrames)
      {
        ++script_->refused;
        return nyamp::models::Status::kUnsupported;
      }

    {
      std::lock_guard<std::mutex> lock(script_->mutex);
      std::string unit;
      for (const std::int64_t id : text->phoneme_ids)
        {
          unit.push_back(static_cast<char>(id));
        }

      script_->units.push_back(unit);
      script_->unit_ids.push_back(text->phoneme_ids);
      script_->unit_frames.push_back(frames);
    }

    nyamp::models::PcmChunk pcm;
    pcm.sample_rate = NYAMP_TTS_SAMPLE_RATE;
    pcm.channels = 1;
    pcm.samples.resize(frames * nyamp::kTtsSamplesPerFrame);
    for (std::size_t index = 0; index < pcm.samples.size(); ++index)
      {
        pcm.samples[index] = UnitSample(text->phoneme_ids[0], index);
      }

    return emit(std::move(pcm)) ? nyamp::models::Status::kOk
                                : nyamp::models::Status::kCancelled;
  }

private:
  TtsScript *script_;
};

} // namespace nyamp::testing

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_SPEECH_TEST_SUPPORT_H */
