/****************************************************************************
 * tools/amp/models/nyamp_models_replay_test.cpp
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

#include "nyamp_models.h"

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

#define CHECK(expression)                                                    \
  do                                                                         \
    {                                                                        \
      if (!(expression))                                                     \
        {                                                                    \
          std::cerr << "check failed: " << __LINE__ << " " #expression "\n"; \
          std::exit(1);                                                      \
        }                                                                    \
    }                                                                        \
  while (false)

namespace
{

using namespace nyamp::models;

// This backend exists only in this test binary. It performs no inference and
// cannot be selected by production factory or daemon configuration.
class NyampReplayBackend final : public Backend
{
public:
  explicit NyampReplayBackend(Kind kind) : kind_(kind) {}
  Kind kind() const override { return kind_; }
  Status Load(const std::string &) override
  {
    ++loads;
    return load_status;
  }
  void Unload() override { ++unloads; }
  Status Run(const Input &, const Emit &emit, const Stop &stop) override
  {
    ++runs;
    if (action)
      {
        action();
      }
    for (const auto &item : outputs)
      {
        if (stop() || !emit(item))
          {
            break;
          }
      }
    return run_status;
  }

  Kind kind_;
  Status load_status = Status::kOk;
  Status run_status = Status::kOk;
  int loads = 0;
  int unloads = 0;
  int runs = 0;
  std::vector<Output> outputs;
  std::function<void()> action;
};

void NyampReplayLifecycle();
void NyampReplaySpeech();
void NyampReplayStop();
void NyampReplayFailures();
void NyampReplayConcurrentCancel();

void NyampReplayLifecycle()
{
  auto backend = std::make_unique<NyampReplayBackend>(Kind::kLlm);
  auto *probe = backend.get();
  probe->outputs = { TokenChunk{ 12, "hello" }, TokenChunk{ 13, " world" } };
  Session session(std::move(backend), 7, [] { return 100; });
  Input input = LlmInput{ { 1, 2 }, 10 };
  std::vector<Event> events;
  const auto sink = [&](const Event &event) {
    events.push_back(event);
    return true;
  };
  CHECK(session.Run({ 1, 7, 0 }, input, sink) == Status::kNotReady);
  CHECK(session.Load("verified-model-directory") == Status::kOk);
  CHECK(session.Load("other") == Status::kBusy);
  CHECK(session.Run({ 1, 8, 0 }, input, sink) == Status::kStaleGeneration);
  CHECK(session.Run({ 0, 7, 0 }, input, sink) == Status::kInvalid);
  CHECK(session.Run({ 1, 7, 0 }, input, sink) == Status::kOk);
  CHECK(events.size() == 3);
  CHECK(events[0].sequence == 0 && events[1].sequence == 1 &&
        events[2].sequence == 2);
  CHECK(events[2].terminal && !events[2].output);
  CHECK(events[0].context.request_id == 1 &&
        events[0].context.generation == 7);
  CHECK(std::get<TokenChunk>(*events[0].output).text == "hello");
  auto retained = events[0].output;
  events.clear();
  CHECK(session.Run({ 1, 7, 0 }, input, sink) == Status::kDuplicate);
  CHECK(events.empty());
  CHECK(session.Run({ 2, 7, 0 }, input, sink) == Status::kOk);
  CHECK(probe->loads == 1 && probe->runs == 2);
  CHECK(session.Unload() == Status::kOk);
  CHECK(probe->unloads == 1);
  CHECK(std::get<TokenChunk>(*retained).text == "hello");
}

void NyampReplaySpeech()
{
  auto asr = std::make_unique<NyampReplayBackend>(Kind::kAsr);
  auto *asr_probe = asr.get();
  asr->outputs = { Transcript{ "he", false, 160 },
                   Transcript{ "hello", true, 320 } };
  Session session(std::move(asr), 1, [] { return 1; });
  CHECK(session.Load("verified-asr") == Status::kOk);
  AsrInput audio{ std::vector<float>(320, 0.1f), 16000 };
  std::vector<Event> events;
  auto sink = [&](const Event &event) {
    events.push_back(event);
    return true;
  };
  CHECK(session.Run({ 1, 1, 0 }, audio, sink) == Status::kOk);
  CHECK(std::get<Transcript>(*events[1].output).final);
  audio.sample_rate = 48000;
  CHECK(session.Run({ 2, 1, 0 }, audio, sink) == Status::kInvalid);
  audio.sample_rate = 16000;
  audio.samples[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK(session.Run({ 2, 1, 0 }, audio, sink) == Status::kInvalid);
  audio.samples[0] = 0;
  asr_probe->outputs = { Transcript{ "partial", false, 160 } };
  CHECK(session.Run({ 2, 1, 0 }, audio, sink) == Status::kBackendError);

  auto tts = std::make_unique<NyampReplayBackend>(Kind::kTts);
  tts->outputs = { PcmChunk{ { 0.0f, 0.2f, -0.2f }, 44100, 1 } };
  Session speech(std::move(tts), 2, [] { return 1; });
  CHECK(speech.Load("verified-tts") == Status::kOk);
  events.clear();
  CHECK(speech.Run({ 1, 2, 0 }, TtsInput{ { 0, 1, 0 }, { 0, 1, 0 }, 1, 1 },
                   sink) == Status::kOk);
  auto samples = events[0].output;
  CHECK(speech.Unload() == Status::kOk);
  CHECK(std::get<PcmChunk>(*samples).samples[1] == 0.2f);
}

void NyampReplayStop()
{
  auto backend = std::make_unique<NyampReplayBackend>(Kind::kLlm);
  auto *probe = backend.get();
  probe->outputs = { TokenChunk{ 1, "a" }, TokenChunk{ 2, "b" } };
  std::uint64_t now = 100;
  Session session(std::move(backend), 1, [&] { return now; });
  CHECK(session.Load("llm") == Status::kOk);
  std::vector<Event> events;
  auto collect = [&](const Event &event) {
    events.push_back(event);
    return true;
  };
  CHECK(session.Run({ 1, 1, 100 }, LlmInput{ { 1 }, 2 }, collect) ==
        Status::kDeadline);
  CHECK(probe->runs == 0 && events.size() == 1 && events[0].terminal);
  events.clear();
  auto cancel = [&](const Event &event) {
    events.push_back(event);
    if (!event.terminal)
      {
        CHECK(session.Cancel(event.context.request_id, 1) == Status::kOk);
        CHECK(session.Unload() == Status::kBusy);
      }
    else
      {
        CHECK(session.Cancel(event.context.request_id, 1) == Status::kInvalid);
      }
    return true;
  };
  CHECK(session.Run({ 2, 1, 0 }, LlmInput{ { 1 }, 2 }, cancel) ==
        Status::kCancelled);
  CHECK(events.size() == 2 && events.back().status == Status::kCancelled);
  events.clear();
  CHECK(
      session.Run({ 3, 1, 0 }, LlmInput{ { 1 }, 2 }, [&](const Event &event) {
        events.push_back(event);
        return event.terminal;
      }) == Status::kConsumerStopped);
  CHECK(events.size() == 2 && events.back().terminal);
  CHECK(session.Run({ 4, 1, 0 }, LlmInput{ { 1 }, 2 }, collect) ==
        Status::kOk);
  events.clear();
  CHECK(
      session.Run({ 5, 1, 0 }, LlmInput{ { 1 }, 2 }, [&](const Event &event) {
        if (!event.terminal)
          throw std::runtime_error("consumer stopped");
        events.push_back(event);
        return true;
      }) == Status::kConsumerStopped);
  CHECK(events.size() == 1 && events[0].status == Status::kConsumerStopped);
  events.clear();
  CHECK(session.Run({ 6, 1, 101 }, LlmInput{ { 1 }, 2 },
                    [&](const Event &event) {
                      events.push_back(event);
                      now = 101;
                      return true;
                    }) == Status::kDeadline);
  CHECK(events.size() == 2 && events.back().status == Status::kDeadline);
}

void NyampReplayFailures()
{
  auto backend = std::make_unique<NyampReplayBackend>(Kind::kLlm);
  auto *probe = backend.get();
  probe->load_status = Status::kBackendError;
  Session session(std::move(backend), 1, [] { return 0; });
  CHECK(session.Load("bad") == Status::kBackendError);
  probe->load_status = Status::kOk;
  CHECK(session.Load("good") == Status::kOk);
  probe->outputs = { Transcript{ "wrong-output-type", true, 0 } };
  int terminal = 0;
  auto sink = [&](const Event &event) {
    terminal += event.terminal;
    return true;
  };
  CHECK(session.Run({ 1, 1, 0 }, LlmInput{ { 1 }, 2 }, sink) ==
        Status::kBackendError);
  CHECK(terminal == 1);
  probe->action = [] { throw std::runtime_error("injected failure"); };
  CHECK(session.Run({ 2, 1, 0 }, LlmInput{ { 1 }, 2 }, sink) ==
        Status::kBackendError);
  CHECK(terminal == 2);
  probe->action = {};
  probe->outputs = { TokenChunk{ 1, "ok" } };
  CHECK(session.Run({ 3, 1, 0 }, LlmInput{ { 1 }, 2 }, sink) == Status::kOk);
}

void NyampReplayConcurrentCancel()
{
  auto backend = std::make_unique<NyampReplayBackend>(Kind::kLlm);
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, released = false;
  backend->action = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    condition.notify_one();
    condition.wait(lock, [&] { return released; });
  };
  Session session(std::move(backend), 3, [] { return 0; });
  CHECK(session.Load("llm") == Status::kOk);
  Status result = Status::kInvalid;
  std::thread worker([&] {
    result = session.Run({ 1, 3, 0 }, LlmInput{ { 1 }, 2 },
                         [](const Event &) { return true; });
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return entered; });
  }
  CHECK(session.Cancel(1, 4) == Status::kStaleGeneration);
  CHECK(session.Cancel(1, 3) == Status::kOk);
  CHECK(session.Run({ 2, 3, 0 }, LlmInput{ { 1 }, 2 },
                    [](const Event &) { return true; }) == Status::kBusy);
  {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
  }
  condition.notify_one();
  worker.join();
  CHECK(result == Status::kCancelled);
}

} // namespace

int main()
{
  NyampReplayLifecycle();
  NyampReplaySpeech();
  NyampReplayStop();
  NyampReplayFailures();
  NyampReplayConcurrentCancel();
  std::cout << "NYAMP_MODELS_REPLAY_PASS backend=test-only no_inference=1\n";
  return 0;
}
