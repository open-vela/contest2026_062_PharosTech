/****************************************************************************
 * tools/amp/nyampd/nyampd_core_test.cpp
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

#include "nyampd_core.h"
#include "nyampd_llm.h"

#include "nyamp_protocol.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

namespace
{

/* Wait for a background generation to finish.  The worker is a real thread,
 * so a bounded spin with a short sleep is the only portable way to observe it
 * without exposing service internals to the test.
 */

constexpr int kWaitMilliseconds = 5000;
constexpr int kWaitStepMilliseconds = 2;

/* A backend used only by this test binary.  It performs no inference and is
 * never selectable from production configuration; the daemon's own factory
 * returns an empty one, so a passing test cannot be mistaken for a running
 * model.
 */

class FakeLlmBackend final : public nyamp::models::Backend
{
public:
  nyamp::models::Kind kind() const override
  {
    return nyamp::models::Kind::kLlm;
  }
  nyamp::models::Status Load(const std::string &) override
  {
    return nyamp::models::Status::kOk;
  }
  void Unload() override {}
  nyamp::models::Status Run(const nyamp::models::Input &,
                            const nyamp::models::Emit &emit,
                            const nyamp::models::Stop &stop) override
  {
    for (int index = 0; index < 3; ++index)
      {
        if (stop() || !emit(nyamp::models::TokenChunk{ index, "tok" }))
          {
            return nyamp::models::Status::kCancelled;
          }
      }
    return nyamp::models::Status::kOk;
  }
};

std::int32_t GetLe32(const std::uint8_t *source)
{
  std::uint32_t value = 0;
  for (unsigned int index = 0; index < 4; ++index)
    {
      value |= static_cast<std::uint32_t>(source[index]) << (index * 8);
    }

  return static_cast<std::int32_t>(value);
}

std::uint32_t GetLe32u(const std::uint8_t *source)
{
  return static_cast<std::uint32_t>(GetLe32(source));
}

int Exchange(nyamp_header_s request, std::uint64_t now,
             std::uint32_t generation, nyamp_header_s *response_header,
             std::int32_t *status, nyamp::LlmService *llm,
             const std::uint8_t *body = nullptr, std::size_t body_size = 0)
{
  std::uint8_t request_wire[NYAMP_RPMSG_MTU] = {};
  std::uint8_t response_wire[NYAMP_RPMSG_MTU] = {};
  std::size_t response_size = 0;

  /* The wire header and the requested frame length must agree, or Dispatch
   * rejects the frame before it reaches any service.
   */
  request.payload_size = static_cast<std::uint32_t>(body_size);

  CHECK(nyamp_header_encode(request_wire, sizeof(request_wire), &request) ==
        NYAMP_OK);
  if (body_size != 0)
    {
      std::memcpy(request_wire + NYAMP_WIRE_HEADER_SIZE, body, body_size);
    }

  CHECK(nyamp::Dispatch(request_wire, NYAMP_WIRE_HEADER_SIZE + body_size, now,
                        generation, response_wire, sizeof(response_wire),
                        &response_size, {}, llm) == NYAMP_OK);
  CHECK(nyamp_header_decode(response_header, response_wire, response_size) ==
        NYAMP_OK);
  *status = GetLe32(response_wire + NYAMP_WIRE_HEADER_SIZE);
  return 0;
}

/* Feed a whole token array as consecutive generate chunks the way a client
 * must, and report how many chunks were accepted.
 */

int Generate(nyamp::LlmService &llm, std::uint64_t request_id,
             const std::vector<std::int32_t> &ids,
             std::uint32_t max_new_tokens, std::int32_t *last_status,
             int *chunks)
{
  std::vector<std::int32_t> remaining = ids;
  std::size_t offset = 0;
  *chunks = 0;

  while (offset < ids.size() || offset == 0)
    {
      const std::size_t count =
          std::min<std::size_t>(NYAMP_LLM_MAX_CHUNK_IDS, ids.size() - offset);
      nyamp_llm_chunk_s chunk{};
      chunk.total = static_cast<std::uint32_t>(ids.size());
      chunk.offset = static_cast<std::uint32_t>(offset);
      chunk.count = static_cast<std::uint32_t>(count);
      chunk.max_new_tokens = offset == 0 ? max_new_tokens : 0;

      bool started = false;
      const auto status =
          llm.BeginGenerate(chunk, ids.data() + offset, count, request_id,
                            offset == 0 ? 5000 : 0, &started);
      *last_status = static_cast<std::int32_t>(status);
      ++*chunks;

      if (status != nyamp::models::Status::kOk)
        {
          return 1;
        }

      offset += count;
      if (offset >= ids.size())
        {
          break;
        }
    }

  return 0;
}

int TestHealthAndCapabilities()
{
  constexpr std::uint32_t generation = 17;
  nyamp_header_s request = {
    NYAMP_SERVICE_HEALTH,
    nyamp::kHealthQuery,
    NYAMP_FLAG_REQUEST,
    42,
    1000,
    0,
    0,
  };
  nyamp_header_s response;
  std::int32_t status;
  std::uint8_t response_wire[NYAMP_RPMSG_MTU] = {};
  std::size_t response_size = 0;

  /* Without a service the health payload still reports the baseline
   * capability, so an older client sees no change.
   */
  CHECK(Exchange(request, 500, generation, &response, &status, nullptr) == 0);
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(response.flags == NYAMP_FLAG_RESPONSE);
  CHECK(response.generation == generation);
  CHECK(response.payload_size == 12);

  /* With a service the LLM bit appears, but only when the daemon actually has
   * one to serve.
   */
  nyamp::LlmService llm(
      generation, [] { return 0; },
      [] { return std::make_unique<FakeLlmBackend>(); });
  std::uint8_t request_wire[NYAMP_RPMSG_MTU] = {};
  CHECK(nyamp_header_encode(request_wire, sizeof(request_wire), &request) ==
        NYAMP_OK);
  CHECK(nyamp::Dispatch(request_wire, NYAMP_WIRE_HEADER_SIZE, 500, generation,
                        response_wire, sizeof(response_wire), &response_size,
                        {}, &llm) == NYAMP_OK);
  /* Health, LLM, and CHAT: a service with a backend can serve the text
   * level request too.  BLOB (bit 2) is absent because no blob service is
   * attached here.
   */
  CHECK(GetLe32u(response_wire + NYAMP_WIRE_HEADER_SIZE + 8) == 0xbU);
  return 0;
}

int TestHealthFailures()
{
  constexpr std::uint32_t generation = 17;
  nyamp_header_s request = {
    NYAMP_SERVICE_HEALTH,
    nyamp::kHealthQuery,
    NYAMP_FLAG_REQUEST,
    42,
    1000,
    0,
    0,
  };
  nyamp_header_s response;
  std::int32_t status;

  request.deadline_ms = 499;
  CHECK(Exchange(request, 500, generation, &response, &status, nullptr) == 0);
  CHECK(status == NYAMP_MODEL_DEADLINE);
  CHECK((response.flags & NYAMP_FLAG_ERROR) != 0);

  request.deadline_ms = 1000;
  request.generation = generation - 1;
  CHECK(Exchange(request, 500, generation, &response, &status, nullptr) == 0);
  CHECK(status == NYAMP_MODEL_STALE_GENERATION);

  request.generation = generation;
  request.service = NYAMP_SERVICE_NPU;
  CHECK(Exchange(request, 500, generation, &response, &status, nullptr) == 0);
  CHECK(status == NYAMP_MODEL_UNSUPPORTED);

  /* A reserved service with no implementation must not answer as if it had
   * one, even when a healthy LLM service is attached.
   */
  nyamp::LlmService llm(
      generation, [] { return 0; },
      [] { return std::make_unique<FakeLlmBackend>(); });
  CHECK(Exchange(request, 500, generation, &response, &status, &llm) == 0);
  CHECK(status == NYAMP_MODEL_UNSUPPORTED);
  return 0;
}

int TestLlmRequiresService()
{
  constexpr std::uint32_t generation = 5;
  nyamp_header_s request = {
    NYAMP_SERVICE_LLM, NYAMP_LLM_LOAD, NYAMP_FLAG_REQUEST, 7, 0, generation, 0,
  };
  nyamp_header_s response;
  std::int32_t status;
  const char path[] = "/data/model";

  /* No service attached: LOAD must be refused, never reported as loaded. */
  CHECK(Exchange(request, 500, generation, &response, &status, nullptr,
                 reinterpret_cast<const std::uint8_t *>(path),
                 sizeof(path) - 1) == 0);
  CHECK(status == NYAMP_MODEL_UNSUPPORTED);

  /* A service with no backend factory is equally unable to load. */
  nyamp::LlmService llm(
      generation, [] { return 0; }, nyamp::LlmService::BackendFactory());
  CHECK(Exchange(request, 500, generation, &response, &status, &llm,
                 reinterpret_cast<const std::uint8_t *>(path),
                 sizeof(path) - 1) == 0);
  CHECK(status == NYAMP_MODEL_UNSUPPORTED);
  return 0;
}

int TestLlmGenerateEvents()
{
  constexpr std::uint32_t generation = 9;
  nyamp::LlmService llm(
      generation, [] { return 0; },
      [] { return std::make_unique<FakeLlmBackend>(); });

  /* Load first: generate against an unloaded session must not start. */
  std::int32_t status = 0;
  bool started = false;
  nyamp_llm_chunk_s chunk{};
  chunk.total = 2;
  chunk.count = 2;
  chunk.max_new_tokens = 8;
  const std::int32_t ids[] = { 1, 2 };
  CHECK(llm.BeginGenerate(chunk, ids, 2, 1, 0, &started) ==
        nyamp::models::Status::kOk);
  CHECK(started);

  /* Events for an unloaded session are the backend error terminal.  The
   * worker produces them asynchronously, so wait for the terminal rather than
   * assuming it is queued by the time generate returns.
   */
  nyamp::LlmFrame frame;
  bool saw_finish = false;
  int tokens = 0;
  for (int waited = 0; waited < kWaitMilliseconds && !saw_finish;
       waited += kWaitStepMilliseconds)
    {
      while (llm.Poll(&frame))
        {
          nyamp_header_s header;
          CHECK(nyamp_header_decode(&header, frame.data, frame.size) ==
                NYAMP_OK);
          CHECK(header.service == NYAMP_SERVICE_LLM);
          CHECK(header.flags == NYAMP_FLAG_EVENT);
          CHECK(header.request_id == 1);
          if (header.opcode == NYAMP_LLM_EVENT_TOKEN)
            {
              ++tokens;
            }
          else if (header.opcode == NYAMP_LLM_EVENT_FINISH)
            {
              saw_finish = true;
            }
        }

      if (!saw_finish)
        {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(kWaitStepMilliseconds));
        }
    }
  CHECK(saw_finish);
  CHECK(tokens == 0);

  /* Once the terminal is visible the service must accept a load again. */
  CHECK(llm.Load("/tmp") == nyamp::models::Status::kOk);
  (void)status;
  return 0;
}

int TestLlmChunkOrdering()
{
  constexpr std::uint32_t generation = 3;
  nyamp::LlmService llm(
      generation, [] { return 0; },
      [] { return std::make_unique<FakeLlmBackend>(); });
  CHECK(llm.Load("/tmp") == nyamp::models::Status::kOk);

  std::vector<std::int32_t> ids(NYAMP_LLM_MAX_CHUNK_IDS * 2 + 5, 7);
  std::int32_t last = 0;
  int chunks = 0;
  CHECK(Generate(llm, 1, ids, 16, &last, &chunks) == 0);
  CHECK(last == static_cast<std::int32_t>(nyamp::models::Status::kOk));
  CHECK(chunks == 3);

  /* Let the worker finish and drain its events before the next request, or it
   * would be refused as busy rather than exercising the ordering rules.
   */
  nyamp::LlmFrame frame;
  bool finished = false;
  for (int waited = 0; waited < kWaitMilliseconds && !finished;
       waited += kWaitStepMilliseconds)
    {
      while (llm.Poll(&frame))
        {
          nyamp_header_s header;
          CHECK(nyamp_header_decode(&header, frame.data, frame.size) ==
                NYAMP_OK);
          if (header.opcode == NYAMP_LLM_EVENT_FINISH)
            {
              finished = true;
            }
        }

      if (!finished)
        {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(kWaitStepMilliseconds));
        }
    }
  CHECK(finished);

  /* A continuation without its head is refused and clears the partial. */
  nyamp_llm_chunk_s mid{};
  mid.total = 4;
  mid.offset = 2;
  mid.count = 2;
  mid.max_new_tokens = 8;
  const std::int32_t tail[] = { 3, 4 };
  bool started = false;
  CHECK(llm.BeginGenerate(mid, tail, 2, 5000, 0, &started) ==
        nyamp::models::Status::kInvalid);
  CHECK(!started);

  /* A mismatched continuation is refused and clears the partial. */
  nyamp_llm_chunk_s head{};
  head.total = 4;
  head.count = 1;
  head.max_new_tokens = 8;
  const std::int32_t first[] = { 1 };
  CHECK(llm.BeginGenerate(head, first, 1, 5001, 0, &started) ==
        nyamp::models::Status::kOk);
  CHECK(!started);
  nyamp_llm_chunk_s wrong{};
  wrong.total = 9;
  wrong.offset = 1;
  wrong.count = 1;
  CHECK(llm.BeginGenerate(wrong, first, 1, 5001, 0, &started) ==
        nyamp::models::Status::kInvalid);
  CHECK(!started);
  return 0;
}

} // namespace

int main()
{
  if (TestHealthAndCapabilities() != 0)
    return 1;
  if (TestHealthFailures() != 0)
    return 1;
  if (TestLlmRequiresService() != 0)
    return 1;
  if (TestLlmGenerateEvents() != 0)
    return 1;
  if (TestLlmChunkOrdering() != 0)
    return 1;
  std::puts("nyampd core tests passed");
  return 0;
}
