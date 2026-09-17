/****************************************************************************
 * tools/amp/nyampd/nyampd_llm.cpp
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

#include "nyampd_llm.h"

#include <cstring>
#include <utility>

namespace nyamp
{

LlmService::LlmService(std::uint32_t generation, models::Clock clock,
                       BackendFactory factory)
    : generation_(generation), clock_(std::move(clock)),
      factory_(std::move(factory))
{
}

LlmService::~LlmService()
{
  /* A worker owns the session until Run returns; never unload underneath it.
   */
  if (worker_.joinable())
    {
      worker_.join();
    }
}

models::Status LlmService::Load(const std::string &directory)
{
  if (directory.empty())
    {
      return models::Status::kInvalid;
    }

  if (!factory_)
    {
      return models::Status::kUnsupported;
    }

  if (running_.load())
    {
      return models::Status::kBusy;
    }

  std::lock_guard<std::mutex> lock(session_mutex_);
  if (!session_)
    {
      session_ =
          std::make_unique<models::Session>(factory_(), generation_, clock_);
    }

  return session_->Load(directory);
}

models::Status LlmService::Unload()
{
  if (running_.load())
    {
      return models::Status::kBusy;
    }

  std::lock_guard<std::mutex> lock(session_mutex_);
  if (!session_)
    {
      return models::Status::kInvalid;
    }

  return session_->Unload();
}

void LlmService::ResetPendingLocked()
{
  pending_.request_id = 0;
  pending_.deadline_ms = 0;
  pending_.total = 0;
  pending_.offset = 0;
  pending_.max_new_tokens = 0;
  pending_.ids.clear();
  pending_.active = false;
}

models::Status LlmService::BeginGenerate(
    const nyamp_llm_chunk_s &chunk, const std::int32_t *ids, std::size_t count,
    std::uint64_t request_id, std::uint64_t deadline_ms, bool *started)
{
  if (started == nullptr)
    {
      return models::Status::kInvalid;
    }

  *started = false;

  if (ids == nullptr || count == 0 || request_id == 0)
    {
      return models::Status::kInvalid;
    }

  if (chunk.total == 0 || chunk.total > models::kContextTokens)
    {
      return models::Status::kInvalid;
    }

  if (running_.load())
    {
      return models::Status::kBusy;
    }

  std::lock_guard<std::mutex> lock(pending_mutex_);

  if (!pending_.active)
    {
      if (chunk.offset != 0)
        {
          /* A continuation without its head can never be completed. */
          return models::Status::kInvalid;
        }

      pending_.active = true;
      pending_.request_id = request_id;
      pending_.deadline_ms = deadline_ms;
      pending_.total = chunk.total;
      pending_.offset = 0;
      pending_.max_new_tokens = chunk.max_new_tokens;
      pending_.ids.clear();
      pending_.ids.reserve(chunk.total);
    }
  else if (chunk.total != pending_.total || chunk.offset != pending_.offset)
    {
      /* Out-of-order or mismatched continuation: drop the partial request. */
      ResetPendingLocked();
      return models::Status::kInvalid;
    }

  if (chunk.count > pending_.total - pending_.offset)
    {
      ResetPendingLocked();
      return models::Status::kInvalid;
    }

  pending_.ids.insert(pending_.ids.end(), ids, ids + count);
  pending_.offset += chunk.count;

  if (pending_.offset < pending_.total)
    {
      return models::Status::kOk;
    }

  /* The array is complete.  Move it out and start the worker.  Session::Run
   * rejects a second request while one is active, so the busy check above plus
   * this handoff is the only admission control needed.
   */

  const std::uint64_t request = pending_.request_id;
  const std::uint64_t deadline = pending_.deadline_ms;
  auto input = std::make_shared<models::LlmInput>();
  input->token_ids = std::move(pending_.ids);
  input->max_new_tokens = pending_.max_new_tokens;
  ResetPendingLocked();

  {
    std::lock_guard<std::mutex> ready_lock(ready_mutex_);
    ready_ = std::move(input);
  }

  running_.store(true);
  try
    {
      /* The previous worker has cleared running_ but may not be joined yet;
       * assigning over a joinable thread terminates the process.
       */
      if (worker_.joinable())
        {
          worker_.join();
        }

      worker_ = std::thread(&LlmService::Worker, this, request, deadline);
    }
  catch (...)
    {
      running_.store(false);
      std::lock_guard<std::mutex> ready_lock(ready_mutex_);
      ready_.reset();
      return models::Status::kBackendError;
    }

  *started = true;
  return models::Status::kOk;
}

void LlmService::Worker(std::uint64_t request_id, std::uint64_t deadline_ms)
{
  std::shared_ptr<models::LlmInput> input;
  {
    std::lock_guard<std::mutex> lock(ready_mutex_);
    input = std::move(ready_);
  }

  models::Status result = models::Status::kBackendError;

  if (input)
    {
      const std::uint64_t sequence_base = request_id;
      std::uint32_t sequence = 0;

      /* One token event per emitted output.  Returning false from the sink
       * stops the request; the model layer then reports kConsumerStopped, so a
       * full queue surfaces as an explicit failure instead of silent loss.
       */

      auto sink = [&](const models::Event &event) -> bool {
        if (event.terminal)
          {
            return true;
          }

        const auto *token =
            std::get_if<models::TokenChunk>(event.output.get());
        if (token == nullptr)
          {
            return false;
          }

        (void)sequence_base;
        return PushToken(request_id, sequence++, *token);
      };

      std::lock_guard<std::mutex> lock(session_mutex_);
      if (session_)
        {
          result = session_->Run({ request_id, generation_, deadline_ms },
                                 *input, sink);
        }
    }

  /* Release admission before publishing the terminal event.  An observer that
   * has just seen the finish of this request must be able to start the next
   * one immediately; doing it the other way round leaves a window where the
   * terminal is visible but every new request is still refused as busy.
   */

  running_.store(false, std::memory_order_release);
  PushFinish(request_id, result);
}

models::Status LlmService::Cancel(std::uint64_t request_id)
{
  if (request_id == 0)
    {
      return models::Status::kInvalid;
    }

  std::lock_guard<std::mutex> lock(session_mutex_);
  if (!session_)
    {
      return models::Status::kInvalid;
    }

  return session_->Cancel(request_id, generation_);
}

bool LlmService::Push(const std::uint8_t *payload, std::size_t payload_size,
                      std::uint16_t opcode, std::uint64_t request_id)
{
  if (payload_size > NYAMP_INLINE_MAX)
    {
      return false;
    }

  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (queue_.size() >= kLlmEventQueueLimit)
    {
      return false;
    }

  LlmFrame frame{};
  nyamp_header_s header{};

  header.service = NYAMP_SERVICE_LLM;
  header.opcode = opcode;
  header.flags = NYAMP_FLAG_EVENT;
  header.request_id = request_id;
  header.deadline_ms = 0;
  header.generation = generation_;
  header.payload_size = static_cast<std::uint32_t>(payload_size);

  if (nyamp_header_encode(frame.data, sizeof(frame.data), &header) != NYAMP_OK)
    {
      return false;
    }

  if (payload_size != 0)
    {
      std::memcpy(frame.data + NYAMP_WIRE_HEADER_SIZE, payload, payload_size);
    }

  frame.size = NYAMP_WIRE_HEADER_SIZE + payload_size;
  queue_.push_back(frame);
  return true;
}

bool LlmService::PushToken(std::uint64_t request_id, std::uint32_t sequence,
                           const models::TokenChunk &token)
{
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t payload_size = 0;
  const std::size_t length = token.text.size();

  if (nyamp_llm_token_encode(payload, sizeof(payload), &payload_size,
                             static_cast<std::uint32_t>(token.token_id),
                             sequence, token.text.c_str(), length) != NYAMP_OK)
    {
      return false;
    }

  return Push(payload, payload_size, NYAMP_LLM_EVENT_TOKEN, request_id);
}

bool LlmService::PushFinish(std::uint64_t request_id, models::Status status)
{
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t payload_size = 0;

  if (nyamp_llm_finish_encode(payload, sizeof(payload), &payload_size,
                              static_cast<std::int32_t>(status),
                              0) != NYAMP_OK)
    {
      return false;
    }

  return Push(payload, payload_size, NYAMP_LLM_EVENT_FINISH, request_id);
}

bool LlmService::Poll(LlmFrame *frame)
{
  if (frame == nullptr)
    {
      return false;
    }

  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (queue_.empty())
    {
      return false;
    }

  *frame = queue_.front();
  queue_.pop_front();
  return true;
}

} // namespace nyamp
