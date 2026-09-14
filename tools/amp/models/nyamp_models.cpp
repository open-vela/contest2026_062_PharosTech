/****************************************************************************
 * tools/amp/models/nyamp_models.cpp
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

#include <algorithm>
#include <cmath>
#include <utility>

namespace nyamp::models
{

Session::Session(std::unique_ptr<Backend> backend, std::uint32_t generation,
                 Clock clock)
    : backend_(std::move(backend)), generation_(generation),
      clock_(std::move(clock))
{
}

Session::~Session()
{
  if (loaded_)
    {
      backend_->Unload();
    }
}

Status Session::Load(const std::string &model_directory)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!backend_ || !clock_ || generation_ == 0 || model_directory.empty())
    {
      return Status::kInvalid;
    }
  if (busy_ || loaded_)
    {
      return Status::kBusy;
    }
  const Status status = backend_->Load(model_directory);
  loaded_ = status == Status::kOk;
  return status;
}

Status Session::Unload()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (busy_)
    {
      return Status::kBusy;
    }
  if (loaded_)
    {
      backend_->Unload();
      loaded_ = false;
    }
  return Status::kOk;
}

bool Session::ValidInput(const Input &input) const
{
  if (backend_->kind() == Kind::kLlm)
    {
      const auto *value = std::get_if<LlmInput>(&input);
      return value && !value->token_ids.empty() && value->max_new_tokens > 0 &&
             value->token_ids.size() +
                     static_cast<std::size_t>(value->max_new_tokens) <=
                 kContextTokens &&
             std::all_of(value->token_ids.begin(), value->token_ids.end(),
                         [](std::int32_t id) { return id >= 0; });
    }
  if (backend_->kind() == Kind::kAsr)
    {
      const auto *value = std::get_if<AsrInput>(&input);
      return value && value->sample_rate == kAsrSampleRate &&
             !value->samples.empty() &&
             value->samples.size() <= kAsrMaxSamples &&
             std::all_of(value->samples.begin(), value->samples.end(),
                         [](float sample) {
                           return std::isfinite(sample) &&
                                  std::abs(sample) <= 1;
                         });
    }
  const auto *value = std::get_if<TtsInput>(&input);
  return value && !value->phoneme_ids.empty() &&
         value->phoneme_ids.size() <= kTtsMaxPhonemes &&
         value->phoneme_ids.size() == value->tone_ids.size() &&
         std::isfinite(value->speed) && value->speed > 0 &&
         std::all_of(value->phoneme_ids.begin(), value->phoneme_ids.end(),
                     [](std::int64_t id) { return id >= 0; }) &&
         std::all_of(value->tone_ids.begin(), value->tone_ids.end(),
                     [](std::int64_t id) { return id >= 0; });
}

bool Session::ValidOutput(const Input &input, const Output &output) const
{
  if (std::holds_alternative<LlmInput>(input))
    {
      const auto *token = std::get_if<TokenChunk>(&output);
      return token && token->token_id >= 0;
    }
  if (const auto *audio = std::get_if<AsrInput>(&input))
    {
      const auto *text = std::get_if<Transcript>(&output);
      return text && text->consumed_samples <= audio->samples.size();
    }
  const auto *pcm = std::get_if<PcmChunk>(&output);
  return pcm && pcm->sample_rate == 44100 && pcm->channels == 1 &&
         !pcm->samples.empty() &&
         std::all_of(pcm->samples.begin(), pcm->samples.end(),
                     [](float sample) { return std::isfinite(sample); });
}

Status Session::StopReason(const RequestContext &context) const
{
  if (cancelled_.load())
    {
      return Status::kCancelled;
    }
  if (context.deadline_ms && clock_() >= context.deadline_ms)
    {
      return Status::kDeadline;
    }
  return Status::kOk;
}

Status Session::Cancel(std::uint64_t request_id, std::uint32_t generation)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != generation_)
    {
      return Status::kStaleGeneration;
    }
  if (!busy_ || request_id == 0 || active_id_ != request_id)
    {
      return Status::kInvalid;
    }
  cancelled_.store(true);
  return Status::kOk;
}

Status Session::Run(const RequestContext &context, const Input &input,
                    const EventSink &sink)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_)
      {
        return Status::kNotReady;
      }
    if (context.generation != generation_)
      {
        return Status::kStaleGeneration;
      }
    if (!sink || context.request_id == 0 || !ValidInput(input))
      {
        return Status::kInvalid;
      }
    if (busy_)
      {
        return Status::kBusy;
      }
    if (context.request_id <= last_id_)
      {
        return Status::kDuplicate;
      }
    busy_ = true;
    active_id_ = last_id_ = context.request_id;
    cancelled_.store(false);
  }

  std::uint64_t sequence = 0;
  Status observed = Status::kOk;
  bool final_transcript = false;
  auto stop = [&]() {
    if (observed == Status::kOk)
      {
        observed = StopReason(context);
      }
    return observed != Status::kOk;
  };
  auto emit = [&](Output output) {
    if (stop())
      {
        return false;
      }
    if (!ValidOutput(input, output) || final_transcript)
      {
        observed = Status::kBackendError;
        return false;
      }
    if (const auto *text = std::get_if<Transcript>(&output))
      {
        final_transcript = text->final;
      }
    Event event{ context, sequence++, false, Status::kOk,
                 std::make_shared<const Output>(std::move(output)) };
    try
      {
        if (!sink(event))
          {
            observed = Status::kConsumerStopped;
            return false;
          }
      }
    catch (...)
      {
        observed = Status::kConsumerStopped;
        return false;
      }
    return !stop();
  };

  Status result = Status::kBackendError;
  try
    {
      if (!stop())
        {
          result = backend_->Run(input, emit, stop);
        }
      stop();
      if (observed != Status::kOk)
        {
          result = observed;
        }
      if (result == Status::kOk && std::holds_alternative<AsrInput>(input) &&
          !final_transcript)
        {
          result = Status::kBackendError;
        }
    }
  catch (...)
    {
      result = Status::kBackendError;
    }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Status reason = StopReason(context);
    if (reason != Status::kOk)
      {
        result = reason;
      }
    active_id_ = 0; // Cancellation can no longer be accepted for this result.
  }

  // A terminal event is emitted even after consumer backpressure. Its return
  // value is ignored. Callback exceptions cannot strand a loaded session.
  try
    {
      sink(Event{ context, sequence, true, result, nullptr });
    }
  catch (...)
    {
      result = Status::kConsumerStopped;
    }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
  }
  return result;
}

} // namespace nyamp::models
