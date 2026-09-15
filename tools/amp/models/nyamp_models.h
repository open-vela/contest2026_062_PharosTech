/****************************************************************************
 * tools/amp/models/nyamp_models.h
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

#ifndef __TOOLS_AMP_MODELS_NYAMP_MODELS_H
#define __TOOLS_AMP_MODELS_NYAMP_MODELS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace nyamp::models
{

constexpr std::uint32_t kInterfaceVersion = 1;
constexpr std::size_t kContextTokens = 2048;
constexpr std::uint32_t kAsrSampleRate = 16000;
constexpr std::size_t kAsrMaxSamples = kAsrSampleRate * 60;
constexpr std::size_t kTtsMaxPhonemes = 4096;

enum class Kind
{
  kLlm,
  kAsr,
  kTts
};
enum class Status
{
  kOk,
  kInvalid,
  kNotReady,
  kBusy,
  kStaleGeneration,
  kDuplicate,
  kCancelled,
  kDeadline,
  kBackendError,
  kUnsupported,
  kConsumerStopped
};

struct RequestContext
{
  std::uint64_t request_id;
  std::uint32_t generation;
  std::uint64_t
      deadline_ms; // Zero means no deadline; clock supplied by owner.
};

struct LlmInput
{
  std::vector<std::int32_t> token_ids;
  std::uint32_t max_new_tokens = 128;
};

struct AsrInput
{
  std::vector<float>
      samples; // Normalized mono samples, owned by this request.
  std::uint32_t sample_rate = 16000;
};

struct TtsInput
{
  std::vector<std::int64_t> phoneme_ids;
  std::vector<std::int64_t> tone_ids;
  std::uint32_t speaker_id = 1;
  float speed = 1.0f;
};

using Input = std::variant<LlmInput, AsrInput, TtsInput>;

struct TokenChunk
{
  std::int32_t token_id;
  std::string text;
};

struct Transcript
{
  std::string text;
  bool final;
  std::uint64_t consumed_samples;
};

struct PcmChunk
{
  std::vector<float> samples;
  std::uint32_t sample_rate;
  std::uint32_t channels;
};

using Output = std::variant<TokenChunk, Transcript, PcmChunk>;

struct Event
{
  RequestContext context;
  std::uint64_t sequence;
  bool terminal;
  Status status;
  std::shared_ptr<const Output> output;
};

// Returning false applies backpressure by stopping this request, not dropping
// data.
using EventSink = std::function<bool(const Event &)>;
using Emit = std::function<bool(Output)>;
using Stop = std::function<bool()>;
using Clock = std::function<std::uint64_t()>;

class Backend
{
public:
  // Emit is serialized; no callback may outlive Run. Unload must not throw.
  virtual ~Backend() = default;
  virtual Kind kind() const = 0;
  virtual Status Load(const std::string &model_directory) = 0;
  virtual void Unload() = 0;
  virtual Status Run(const Input &input, const Emit &emit,
                     const Stop &stop) = 0;
};

// Load/Run/Unload have one owning thread. Cancel may be called from another
// thread or a sink. Destroy only after Run returns. No worker thread is
// hidden. Clock must be monotonic, nonblocking and nonthrowing.
class Session
{
public:
  Session(std::unique_ptr<Backend> backend, std::uint32_t generation,
          Clock clock);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  Status Load(const std::string &model_directory);
  Status Unload();
  Status Run(const RequestContext &context, const Input &input,
             const EventSink &sink);
  Status Cancel(std::uint64_t request_id, std::uint32_t generation);

private:
  bool ValidInput(const Input &input) const;
  bool ValidOutput(const Input &input, const Output &output) const;
  Status StopReason(const RequestContext &context) const;

  std::unique_ptr<Backend> backend_;
  std::uint32_t generation_;
  Clock clock_;
  mutable std::mutex mutex_;
  bool loaded_ = false;
  bool busy_ = false;
  std::uint64_t active_id_ = 0;
  std::uint64_t last_id_ = 0;
  std::atomic<bool> cancelled_{ false };
};

} // namespace nyamp::models

#endif // __TOOLS_AMP_MODELS_NYAMP_MODELS_H
