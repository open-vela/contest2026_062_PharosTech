/****************************************************************************
 * tools/amp/nyampd/nyampd_chat_test.cpp
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

#include "nyampd_chat.h"
#include "nyampd_core.h"
#include "nyampd_llm.h"
#include "nyampd_provision.h"
#include "nyampd_test_support.h"

#include "nyamp_chat_template.h"
#include "nyamp_json.h"
#include "nyamp_protocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

using namespace nyamp::testing;

constexpr std::uint32_t kChatGeneration = 0x0c4a7001U;

/* A service with a loaded model and a tokenizer file beside it. */

struct Bench
{
  explicit Bench(bool with_tokenizer = true)
      : llm(kChatGeneration, [] { return 0; },
            [this] { return std::make_unique<ScriptedBackend>(&script); })
  {
    char pattern[] = "/tmp/nyamp-chat-test-XXXXXX";
    root = mkdtemp(pattern);
    std::ofstream(root + "/model.rkllm") << "weights";
    if (with_tokenizer)
      {
        std::ofstream(root + "/tokenizer.json") << "{}";
      }

    llm.SetChatCodecFactory([](const std::string &, std::string *) {
      return std::make_unique<ByteCodec>();
    });
  }

  ~Bench()
  {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  Script script;
  std::string root;
  nyamp::LlmService llm;
};

struct Outcome
{
  bool finished = false;
  nyamp_llm_chat_finish_s finish{};
  std::string json;
  std::string streamed;
  int tokens = 0;
  int results = 0;
  bool result_in_order = true;
  bool malformed = false;
};

/* Send one frame through Dispatch and return the response status. */

int Exchange(nyamp::LlmService &llm, std::uint16_t opcode,
             std::uint64_t request_id, const std::uint8_t *body,
             std::size_t body_size, std::int32_t *status)
{
  std::uint8_t wire[NYAMP_RPMSG_MTU] = {};
  std::uint8_t response[NYAMP_RPMSG_MTU] = {};
  std::size_t response_size = 0;
  nyamp_header_s header{};
  nyamp_header_s answer;
  const std::uint8_t *ignored = nullptr;
  std::size_t ignored_size = 0;

  header.service = NYAMP_SERVICE_LLM;
  header.opcode = opcode;
  header.flags = NYAMP_FLAG_REQUEST;
  header.request_id = request_id;
  header.generation = kChatGeneration;
  header.payload_size = static_cast<std::uint32_t>(body_size);
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  if (body_size != 0)
    {
      std::memcpy(wire + NYAMP_WIRE_HEADER_SIZE, body, body_size);
    }

  CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE + body_size, 0,
                        kChatGeneration, response, sizeof(response),
                        &response_size, {}, &llm) == NYAMP_OK);
  CHECK(nyamp_header_decode(&answer, response, response_size) == NYAMP_OK);
  CHECK(answer.request_id == request_id && answer.opcode == opcode);
  CHECK(nyamp_status_decode(status, &ignored, &ignored_size,
                            response + NYAMP_WIRE_HEADER_SIZE,
                            answer.payload_size) == NYAMP_OK);
  return 0;
}

/* Send a whole body the way a client must; stops at the first refusal. */

int SendChat(nyamp::LlmService &llm, std::uint64_t request_id,
             const std::string &body, std::uint32_t max_new_tokens,
             std::uint32_t flags, std::size_t chunk_size,
             std::int32_t *status)
{
  std::size_t offset = 0;

  *status = NYAMP_MODEL_INVALID;
  while (offset < body.size())
    {
      std::uint8_t payload[NYAMP_INLINE_MAX];
      std::size_t size = 0;
      nyamp_llm_chat_s chunk{};

      chunk.total = static_cast<std::uint32_t>(body.size());
      chunk.offset = static_cast<std::uint32_t>(offset);
      chunk.length = static_cast<std::uint32_t>(
          std::min(chunk_size, body.size() - offset));
      chunk.max_new_tokens = max_new_tokens;
      chunk.flags = flags;
      CHECK(nyamp_llm_chat_encode(
                payload, sizeof(payload), &size, &chunk,
                reinterpret_cast<const std::uint8_t *>(body.data()) +
                    offset) == NYAMP_OK);
      CHECK(Exchange(llm, NYAMP_LLM_CHAT, request_id, payload, size, status) ==
            0);
      if (*status != NYAMP_MODEL_OK)
        {
          return 0;
        }

      offset += chunk.length;
    }

  return 0;
}

/* Drain the service queue until the terminal event of `request_id`. */

Outcome Collect(nyamp::LlmService &llm, std::uint64_t request_id)
{
  Outcome outcome;

  for (int waited = 0; waited < 10000 && !outcome.finished; waited += 2)
    {
      nyamp::Frame frame;
      while (!outcome.finished && llm.Poll(&frame))
        {
          nyamp_header_s header;
          const std::uint8_t *payload = frame.data + NYAMP_WIRE_HEADER_SIZE;

          if (nyamp_header_decode(&header, frame.data, frame.size) !=
                  NYAMP_OK ||
              header.flags != NYAMP_FLAG_EVENT ||
              header.service != NYAMP_SERVICE_LLM ||
              header.request_id != request_id ||
              header.generation != kChatGeneration)
            {
              outcome.malformed = true;
              continue;
            }

          if (header.opcode == NYAMP_LLM_EVENT_TOKEN)
            {
              std::uint32_t token_id;
              std::uint32_t sequence;
              const char *text;
              std::size_t length;

              if (nyamp_llm_token_decode(&token_id, &sequence, &text, &length,
                                         payload, header.payload_size) !=
                      NYAMP_OK ||
                  sequence != static_cast<std::uint32_t>(outcome.tokens))
                {
                  outcome.malformed = true;
                  continue;
                }

              outcome.streamed.append(text, length);
              ++outcome.tokens;
            }
          else if (header.opcode == NYAMP_LLM_EVENT_RESULT)
            {
              nyamp_llm_result_s chunk;
              const std::uint8_t *bytes;

              if (nyamp_llm_result_decode(&chunk, &bytes, payload,
                                          header.payload_size) != NYAMP_OK)
                {
                  outcome.malformed = true;
                  continue;
                }

              outcome.result_in_order = outcome.result_in_order &&
                                        chunk.offset == outcome.json.size();
              outcome.json.append(reinterpret_cast<const char *>(bytes),
                                  chunk.length);
              outcome.result_in_order = outcome.result_in_order &&
                                        outcome.json.size() <= chunk.total;
              ++outcome.results;
            }
          else if (header.opcode == NYAMP_LLM_EVENT_FINISH)
            {
              outcome.malformed =
                  outcome.malformed ||
                  nyamp_llm_chat_finish_decode(&outcome.finish, payload,
                                               header.payload_size) !=
                      NYAMP_OK;
              outcome.finished = true;
            }
          else
            {
              outcome.malformed = true;
            }
        }

      if (!outcome.finished)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

  return outcome;
}

const nyamp::Json *Path(const nyamp::Json &root,
                        std::initializer_list<const char *> keys)
{
  const nyamp::Json *node = &root;
  for (const char *key : keys)
    {
      if (node == nullptr)
        {
          return nullptr;
        }

      if (node->is_array())
        {
          const std::size_t index = static_cast<std::size_t>(std::atoi(key));
          node = index < node->items().size() ? &node->items()[index] : nullptr;
        }
      else
        {
          node = node->find(key);
        }
    }

  return node;
}

const char kPlainRequest[] =
    R"({"messages":[{"role":"system","content":"You are Nyabula."},)"
    R"({"role":"user","content":"你好，你是谁？"}]})";

const char kToolRequest[] =
    R"({"messages":[{"role":"user","content":"大连未来三天天气"}],)"
    R"("tools":[{"type":"function","function":{"name":"get_weather",)"
    R"("description":"Weather forecast","parameters":{"type":"object",)"
    R"("properties":{"city":{"type":"string"},"days":{"type":"integer"}}}}}]})";

int TestPlainAnswer()
{
  Bench bench;
  std::int32_t status = 1;
  std::string error;
  nyamp::Json response;

  CHECK(bench.llm.Load(bench.root + "/model.rkllm") ==
        nyamp::models::Status::kOk);
  CHECK(bench.llm.ChatState() == "ready");

  bench.script.output = "你好，我是星喵。";
  CHECK(SendChat(bench.llm, 0x2a00000010ULL, kPlainRequest, 64,
                 NYAMP_LLM_CHAT_GUARD_UNTRUSTED, NYAMP_LLM_CHAT_MAX_CHUNK,
                 &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);

  const Outcome outcome = Collect(bench.llm, 0x2a00000010ULL);
  CHECK(outcome.finished && !outcome.malformed && outcome.result_in_order);
  CHECK(outcome.finish.status == NYAMP_MODEL_OK);
  CHECK(outcome.tokens == 0); /* Not asked for, so not sent. */
  CHECK(outcome.results >= 1);

  /* The request really went through the chat template, with the guard. */
  CHECK(ByteCodec::last_guard);
  CHECK(ByteCodec::last_prompt.find("<|im_start|>user\n你好，你是谁？") !=
        std::string::npos);
  CHECK(ByteCodec::last_prompt.compare(0, 3, "<s>") == 0);
  CHECK(bench.script.seen_max_new_tokens == 64);
  CHECK(bench.script.seen_prompt.size() == outcome.finish.prompt_tokens);

  /* The stop token is not part of the answer or of its length. */
  CHECK(outcome.finish.completion_tokens == bench.script.output.size());
  CHECK(outcome.finish.context_limit == 2048);

  /* What an OpenAI client expects, field by field. */
  CHECK(nyamp::json_parse(outcome.json, &response, &error));
  CHECK(Path(response, { "object" })->str() == "chat.completion");
  CHECK(Path(response, { "choices", "0", "index" })->number_as_double() == 0);
  CHECK(Path(response, { "choices", "0", "message", "role" })->str() ==
        "assistant");
  CHECK(Path(response, { "choices", "0", "message", "content" })->str() ==
        "你好，我是星喵。");
  CHECK(Path(response, { "choices", "0", "finish_reason" })->str() == "stop");
  CHECK(Path(response, { "choices", "0", "message", "tool_calls" }) ==
        nullptr);
  CHECK(Path(response, { "usage", "prompt_tokens" })->number_as_double() ==
        outcome.finish.prompt_tokens);
  CHECK(Path(response, { "usage", "completion_tokens" })
            ->number_as_double() == outcome.finish.completion_tokens);
  CHECK(Path(response, { "usage", "total_tokens" })->number_as_double() ==
        outcome.finish.prompt_tokens + outcome.finish.completion_tokens);

  /* A runtime that swallows its stop token gives the same answer, and the
   * defensive third stop id ends a turn just as well.
   */
  bench.script.stop_id = 0;
  CHECK(SendChat(bench.llm, 0x2a00000011ULL, kPlainRequest, 64, 0, 100,
                 &status) == 0);
  Outcome again = Collect(bench.llm, 0x2a00000011ULL);
  CHECK(again.finish.status == NYAMP_MODEL_OK);
  CHECK(nyamp::json_parse(again.json, &response, &error));
  CHECK(Path(response, { "choices", "0", "finish_reason" })->str() == "stop");
  CHECK(!ByteCodec::last_guard);

  bench.script.stop_id = kStopImStart;
  bench.script.output = "好的。";
  CHECK(SendChat(bench.llm, 0x2a00000012ULL, kPlainRequest, 64, 0, 100,
                 &status) == 0);
  again = Collect(bench.llm, 0x2a00000012ULL);
  CHECK(again.finish.status == NYAMP_MODEL_OK);
  CHECK(nyamp::json_parse(again.json, &response, &error));
  CHECK(Path(response, { "choices", "0", "message", "content" })->str() ==
        "好的。");
  return 0;
}

int TestToolCall()
{
  Bench bench;
  std::int32_t status = 1;
  std::string error;
  nyamp::Json response;
  nyamp::Json arguments;

  CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);

  bench.script.output =
      "<function name=\"get_weather\"><param name=\"city\">Dalian</param>"
      "<param name=\"days\">3</param></function>";
  CHECK(SendChat(bench.llm, 21, kToolRequest, 128, 0,
                 NYAMP_LLM_CHAT_MAX_CHUNK, &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);

  const Outcome outcome = Collect(bench.llm, 21);
  CHECK(outcome.finished && !outcome.malformed && outcome.result_in_order);
  CHECK(outcome.finish.status == NYAMP_MODEL_OK);

  /* The tool list reached the template, which is where the model learns it. */
  CHECK(ByteCodec::last_prompt.find("get_weather") != std::string::npos);
  CHECK(ByteCodec::last_prompt.find("<tools>") != std::string::npos);

  CHECK(nyamp::json_parse(outcome.json, &response, &error));
  CHECK(Path(response, { "choices", "0", "finish_reason" })->str() ==
        "tool_calls");
  CHECK(Path(response, { "choices", "0", "message", "content" })->is_null());

  const nyamp::Json *call =
      Path(response, { "choices", "0", "message", "tool_calls", "0" });
  CHECK(call != nullptr);
  CHECK(call->find("type")->str() == "function");
  CHECK(call->find("id")->str().compare(0, 5, "call_") == 0);
  CHECK(Path(*call, { "function", "name" })->str() == "get_weather");

  /* arguments is a JSON STRING, and the schema typed `days` as a number. */
  const nyamp::Json *text = Path(*call, { "function", "arguments" });
  CHECK(text != nullptr && text->is_string());
  CHECK(nyamp::json_parse(text->str(), &arguments, &error));
  CHECK(arguments.find("city")->str() == "Dalian");
  CHECK(arguments.find("days")->is_number() &&
        arguments.find("days")->number_as_double() == 3);
  return 0;
}

int TestTruncatedToolCallStaysText()
{
  Bench bench;
  std::int32_t status = 1;
  std::string error;
  nyamp::Json response;

  CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);

  /* The budget runs out in the middle of a call.  Half a command must never
   * be executed: it comes back as text, marked as cut off.
   */
  bench.script.output =
      "<function name=\"get_weather\"><param name=\"city\">Dalian</param>"
      "<param name=\"days\">3</param></function>";
  CHECK(SendChat(bench.llm, 31, kToolRequest, 40, 0, NYAMP_LLM_CHAT_MAX_CHUNK,
                 &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);

  const Outcome outcome = Collect(bench.llm, 31);
  CHECK(outcome.finish.status == NYAMP_MODEL_OK);
  CHECK(outcome.finish.completion_tokens == 40);
  CHECK(bench.script.emitted.load() <= 40);

  CHECK(nyamp::json_parse(outcome.json, &response, &error));
  CHECK(Path(response, { "choices", "0", "finish_reason" })->str() ==
        "length");
  CHECK(Path(response, { "choices", "0", "message", "tool_calls" }) ==
        nullptr);
  CHECK(Path(response, { "choices", "0", "message", "content" })
            ->str()
            .find("<function name=\"get_weather\">") == 0);
  return 0;
}

int TestPromptOverflow()
{
  Bench bench;
  std::int32_t status = 1;
  std::string history(9000, 'x');
  const std::string request =
      R"({"messages":[{"role":"user","content":")" + history + R"("}]})";

  CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);

  /* The body is accepted -- it is a well-formed request -- and the refusal
   * arrives as the terminal event, with the numbers needed to trim.
   */
  CHECK(SendChat(bench.llm, 41, request, 256, 0, NYAMP_LLM_CHAT_MAX_CHUNK,
                 &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);

  Outcome outcome = Collect(bench.llm, 41);
  CHECK(outcome.finished && !outcome.malformed);
  CHECK(outcome.finish.status == NYAMP_MODEL_PROMPT_TOO_LONG);
  CHECK(outcome.finish.prompt_tokens > 2048 - 256);
  CHECK(outcome.finish.context_limit == 2048);
  CHECK(outcome.finish.completion_tokens == 0);
  CHECK(outcome.results == 0 && outcome.json.empty());
  CHECK(bench.script.runs == 0); /* The model was never bothered. */

  /* A prompt that fits by itself still overflows with a greedy budget. */
  CHECK(SendChat(bench.llm, 42, kPlainRequest, 2040, 0,
                 NYAMP_LLM_CHAT_MAX_CHUNK, &status) == 0);
  outcome = Collect(bench.llm, 42);
  CHECK(outcome.finish.status == NYAMP_MODEL_PROMPT_TOO_LONG);
  CHECK(outcome.finish.prompt_tokens < 100);

  /* The refusal leaves the service free for the trimmed retry. */
  bench.script.output = "ok";
  CHECK(SendChat(bench.llm, 43, kPlainRequest, 0, 0, NYAMP_LLM_CHAT_MAX_CHUNK,
                 &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);
  outcome = Collect(bench.llm, 43);
  CHECK(outcome.finish.status == NYAMP_MODEL_OK);

  /* Zero selected the daemon's default budget. */
  CHECK(bench.script.seen_max_new_tokens == nyamp::kChatDefaultNewTokens);
  return 0;
}

int TestCancelMidRun()
{
  Bench bench;
  std::int32_t status = 1;

  CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);

  bench.script.endless = true;
  bench.script.delay_ms = 5;
  CHECK(SendChat(bench.llm, 51, kPlainRequest, 1500,
                 NYAMP_LLM_CHAT_STREAM_TOKENS, NYAMP_LLM_CHAT_MAX_CHUNK,
                 &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);

  /* Busy while it runs, for chat and for generate alike. */
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(SendChat(bench.llm, 52, kPlainRequest, 16, 0,
                 NYAMP_LLM_CHAT_MAX_CHUNK, &status) == 0);
  CHECK(status == NYAMP_MODEL_BUSY);

  /* A cancel for somebody else's request changes nothing. */
  CHECK(Exchange(bench.llm, NYAMP_LLM_CANCEL, 999, nullptr, 0, &status) == 0);
  CHECK(status != NYAMP_MODEL_OK);

  /* The cancel is answered at once.  It used to wait for the session lock
   * the worker holds for the whole run -- 7.5 s here -- which froze the
   * transport loop for exactly as long as the run it was meant to stop.
   */
  const auto before = std::chrono::steady_clock::now();
  CHECK(Exchange(bench.llm, NYAMP_LLM_CANCEL, 51, nullptr, 0, &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(std::chrono::steady_clock::now() - before <
        std::chrono::milliseconds(1000));

  const Outcome outcome = Collect(bench.llm, 51);
  CHECK(outcome.finished && !outcome.malformed);
  CHECK(outcome.finish.status == NYAMP_MODEL_CANCELLED);
  CHECK(outcome.results == 0);
  CHECK(outcome.finish.completion_tokens > 0 &&
        outcome.finish.completion_tokens < 1500);
  CHECK(outcome.tokens > 0);

  /* And the service is usable again, with a SMALLER request id than the one
   * before: wire ids carry a pid and promise no order.
   */
  bench.script.endless = false;
  bench.script.delay_ms = 0;
  bench.script.output = "回来了";
  CHECK(SendChat(bench.llm, 7, kPlainRequest, 32, 0, NYAMP_LLM_CHAT_MAX_CHUNK,
                 &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(Collect(bench.llm, 7).finish.status == NYAMP_MODEL_OK);
  return 0;
}

int TestStreaming()
{
  Bench bench;
  std::int32_t status = 1;

  CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);

  /* Multi-byte characters arrive one byte-token at a time; every token
   * event must still be valid UTF-8, and together they are the answer.
   */
  bench.script.output = "星喵说：hi！";
  CHECK(SendChat(bench.llm, 61, kPlainRequest, 64,
                 NYAMP_LLM_CHAT_STREAM_TOKENS, 200, &status) == 0);
  const Outcome outcome = Collect(bench.llm, 61);
  CHECK(outcome.finish.status == NYAMP_MODEL_OK && !outcome.malformed);
  CHECK(outcome.streamed == bench.script.output);
  CHECK(outcome.tokens == 7); /* Characters, not bytes. */
  CHECK(outcome.finish.completion_tokens == bench.script.output.size());
  return 0;
}

int TestChunkReassembly()
{
  Bench bench;
  std::int32_t status = 1;
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t size = 0;
  const std::string body = kToolRequest;
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(body.data());
  nyamp_llm_chat_s chunk{};

  CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);
  bench.script.output = "ok";

  auto send = [&](std::uint64_t id, std::uint32_t offset, std::uint32_t length,
                  std::uint32_t total, std::uint32_t max_new,
                  std::uint32_t flags) {
    chunk.total = total;
    chunk.offset = offset;
    chunk.length = length;
    chunk.max_new_tokens = max_new;
    chunk.flags = flags;
    if (nyamp_llm_chat_encode(payload, sizeof(payload), &size, &chunk,
                              bytes + offset) != NYAMP_OK)
      {
        return 1;
      }

    return Exchange(bench.llm, NYAMP_LLM_CHAT, id, payload, size, &status);
  };

  const std::uint32_t total = static_cast<std::uint32_t>(body.size());
  CHECK(total > 120);

  /* A continuation without its head. */
  CHECK(send(71, 40, 40, total, 32, 0) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);

  /* A gap, which also discards the head that was accepted. */
  CHECK(send(71, 0, 40, total, 32, 0) == 0 && status == NYAMP_MODEL_OK);
  CHECK(send(71, 60, 40, total, 32, 0) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);
  CHECK(send(71, 40, 40, total, 32, 0) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);

  /* The same bytes again (a retransmission) is a wrong offset too. */
  CHECK(send(72, 0, 40, total, 32, 0) == 0 && status == NYAMP_MODEL_OK);
  CHECK(send(72, 0, 40, total, 32, 0) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);

  /* Parameters, total and owner may not change halfway. */
  CHECK(send(73, 0, 40, total, 32, 0) == 0 && status == NYAMP_MODEL_OK);
  CHECK(send(73, 40, 40, total, 64, 0) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);
  CHECK(send(74, 0, 40, total, 32, 0) == 0 && status == NYAMP_MODEL_OK);
  CHECK(send(74, 40, 40, total, 32, NYAMP_LLM_CHAT_STREAM_TOKENS) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);
  CHECK(send(75, 0, 40, total, 32, 0) == 0 && status == NYAMP_MODEL_OK);
  CHECK(send(75, 40, 40, total - 1, 32, 0) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);
  CHECK(send(76, 0, 40, total, 32, 0) == 0 && status == NYAMP_MODEL_OK);
  CHECK(send(77, 40, 40, total, 32, 0) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);

  /* None of that started a run or left anything queued. */
  nyamp::Frame frame;
  CHECK(bench.script.runs == 0 && !bench.llm.Poll(&frame));

  /* A frame the codec refuses never reaches the service. */
  std::memset(payload, 0xff, NYAMP_LLM_CHAT_HEADER_SIZE);
  CHECK(Exchange(bench.llm, NYAMP_LLM_CHAT, 78, payload,
                 NYAMP_LLM_CHAT_HEADER_SIZE + 4, &status) == 0);
  CHECK(status == NYAMP_MODEL_INVALID);

  /* After all that, an orderly body in small chunks still works. */
  CHECK(SendChat(bench.llm, 79, body, 32, 0, 97, &status) == 0);
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(Collect(bench.llm, 79).finish.status == NYAMP_MODEL_OK);
  CHECK(bench.script.runs == 1);

  /* A body that is not a chat request is reported by the terminal event. */
  CHECK(SendChat(bench.llm, 80, "{\"messages\":[]}", 32, 0, 100, &status) ==
        0);
  CHECK(status == NYAMP_MODEL_OK);
  Outcome outcome = Collect(bench.llm, 80);
  CHECK(outcome.finish.status == NYAMP_MODEL_INVALID && outcome.results == 0);

  CHECK(SendChat(bench.llm, 81, "not json at all", 32, 0, 100, &status) == 0);
  outcome = Collect(bench.llm, 81);
  CHECK(outcome.finish.status == NYAMP_MODEL_INVALID);
  CHECK(bench.script.runs == 1);
  return 0;
}

int TestAvailability()
{
  std::int32_t status = 1;
  std::uint8_t wire[NYAMP_RPMSG_MTU] = {};
  std::uint8_t response[NYAMP_RPMSG_MTU] = {};
  std::size_t response_size = 0;
  nyamp_header_s health{};

  health.service = NYAMP_SERVICE_HEALTH;
  health.opcode = nyamp::kHealthQuery;
  health.flags = NYAMP_FLAG_REQUEST;
  health.request_id = 5;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &health) == NYAMP_OK);

  /* No model yet: the caller is invited to load one. */
  {
    Bench bench;

    CHECK(SendChat(bench.llm, 91, kPlainRequest, 32, 0, 100, &status) == 0);
    CHECK(status == NYAMP_MODEL_NOT_READY);

    CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE, 0, kChatGeneration,
                          response, sizeof(response), &response_size, {},
                          &bench.llm) == NYAMP_OK);
    CHECK((response[NYAMP_WIRE_HEADER_SIZE + 8] & 0x8) != 0);

    CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);
    bench.script.output = "ok";
    CHECK(SendChat(bench.llm, 92, kPlainRequest, 32, 0, 100, &status) == 0);
    CHECK(status == NYAMP_MODEL_OK);
    CHECK(Collect(bench.llm, 92).finish.status == NYAMP_MODEL_OK);

    /* The tokenizer goes away with its model. */
    CHECK(bench.llm.Unload() == nyamp::models::Status::kOk);
    CHECK(bench.llm.ChatState() == "off");
    CHECK(SendChat(bench.llm, 93, kPlainRequest, 32, 0, 100, &status) == 0);
    CHECK(status == NYAMP_MODEL_NOT_READY);
  }

  /* A bare model file loaded by absolute path: generate works as before,
   * chat says it is not available rather than not ready.
   */
  {
    Bench bench(false);

    CHECK(bench.llm.Load(bench.root + "/model.rkllm") ==
          nyamp::models::Status::kOk);
    CHECK(bench.llm.ChatState() == "off");
    CHECK(SendChat(bench.llm, 94, kPlainRequest, 32, 0, 100, &status) == 0);
    CHECK(status == NYAMP_MODEL_UNSUPPORTED);
  }

  /* A tokenizer the codec refuses does not fail a legacy load either. */
  {
    Bench bench;

    bench.llm.SetChatCodecFactory(
        [](const std::string &, std::string *error) {
          *error = "unexpected pre-tokenizer";
          return std::unique_ptr<nyamp::ChatCodec>();
        });
    CHECK(bench.llm.Load(bench.root) == nyamp::models::Status::kOk);
    CHECK(bench.llm.ChatState().find("unexpected pre-tokenizer") !=
          std::string::npos);
    CHECK(SendChat(bench.llm, 95, kPlainRequest, 32, 0, 100, &status) == 0);
    CHECK(status == NYAMP_MODEL_UNSUPPORTED);
  }

  /* Without a backend the daemon cannot chat and must not advertise it. */
  {
    nyamp::LlmService bare(kChatGeneration, [] { return 0; },
                           nyamp::LlmService::BackendFactory());

    CHECK(nyamp::Dispatch(wire, NYAMP_WIRE_HEADER_SIZE, 0, kChatGeneration,
                          response, sizeof(response), &response_size, {},
                          &bare) == NYAMP_OK);
    CHECK((response[NYAMP_WIRE_HEADER_SIZE + 8] & 0x8) == 0);
    CHECK(SendChat(bare, 96, kPlainRequest, 32, 0, 100, &status) == 0);
    CHECK(status == NYAMP_MODEL_UNSUPPORTED);
  }

  return 0;
}

/* Wait for the deferred response to a LOAD. */

bool WaitLoad(nyamp::LlmService &llm, std::uint64_t request_id,
              std::int32_t *status)
{
  for (int waited = 0; waited < 5000; waited += 2)
    {
      nyamp::Frame frame;
      while (llm.Poll(&frame))
        {
          nyamp_header_s header;
          const std::uint8_t *body = nullptr;
          std::size_t body_size = 0;

          if (nyamp_header_decode(&header, frame.data, frame.size) !=
                  NYAMP_OK ||
              header.request_id != request_id)
            {
              return false;
            }

          if ((header.flags & NYAMP_FLAG_KIND_MASK) == NYAMP_FLAG_EVENT)
            {
              continue;
            }

          return nyamp_status_decode(status, &body, &body_size,
                                     frame.data + NYAMP_WIRE_HEADER_SIZE,
                                     header.payload_size) == NYAMP_OK;
        }

      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

  return false;
}

int TestLogicalLoadBringsTokenizer()
{
  Rig rig(4096);
  nyamp::ModelProvisioner provisioner(rig.client.get());
  Script script;
  std::string tokenizer_path;
  nyamp::LlmService llm(
      kGeneration, [] { return 0; },
      [&] { return std::make_unique<ScriptedBackend>(&script); });
  nyamp_header_s request{};
  std::int32_t status = 1;
  bool deferred = false;

  llm.SetProvisioner(&provisioner);
  llm.SetChatCodecFactory([&](const std::string &path, std::string *) {
    tokenizer_path = path;
    return std::make_unique<ByteCodec>();
  });

  request.service = NYAMP_SERVICE_LLM;
  request.opcode = NYAMP_LLM_LOAD;
  request.flags = NYAMP_FLAG_REQUEST;
  request.request_id = 100;
  request.generation = kGeneration;

  /* Without the tokenizer a logical-name load fails, and it fails before
   * the model -- 875 MB on the board -- has moved at all.
   */
  rig.responder.files["llm/model.rkllm"] = Noise(4096 * 3, 5);
  CHECK(llm.BeginLoad("llm/model.rkllm", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(deferred && WaitLoad(llm, 100, &status));
  CHECK(status == NYAMP_MODEL_NOT_READY);
  CHECK(llm.ChatState() == "tokenizer.json missing");
  CHECK(rig.responder.reads == 0); /* Not one window of the model moved. */
  CHECK(script.runs == 0);

  /* With it, both files arrive verified and chat is ready. */
  rig.responder.files["llm/tokenizer.json"] = Noise(9000, 6);
  request.request_id = 101;
  CHECK(llm.BeginLoad("llm/model.rkllm", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(deferred && WaitLoad(llm, 101, &status));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(tokenizer_path == rig.root + "/llm/tokenizer.json");
  CHECK(rig.Read("llm/tokenizer.json") ==
        rig.responder.files["llm/tokenizer.json"]);
  CHECK(llm.ChatState() == "ready");

  /* Loading what is loaded is a success that moves nothing; another model
   * needs an unload first, and says so before pulling it.
   */
  const int opens = rig.responder.opens;
  request.request_id = 102;
  CHECK(llm.BeginLoad("llm/model.rkllm", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(!deferred && rig.responder.opens == opens);
  CHECK(llm.BeginLoad("llm/other.rkllm", request, &deferred) ==
        nyamp::models::Status::kBusy);
  CHECK(!deferred && rig.responder.opens == opens);

  /* A model directory carries its tokenizer inside. */
  CHECK(llm.Unload() == nyamp::models::Status::kOk);
  rig.responder.files["minicpm/model.rkllm"] = Noise(5000, 7);
  rig.responder.files["minicpm/tokenizer.json"] = Noise(700, 8);
  request.request_id = 103;
  CHECK(llm.BeginLoad("minicpm", request, &deferred) ==
        nyamp::models::Status::kOk);
  CHECK(deferred && WaitLoad(llm, 103, &status));
  CHECK(status == NYAMP_MODEL_OK);
  CHECK(tokenizer_path == rig.root + "/minicpm/tokenizer.json");
  return 0;
}

/* Runs only where the model's tokenizer.json is available. */

int TestRealTokenizer()
{
  const char *path = std::getenv("NYAMP_TOKENIZER_JSON");
  if (path == nullptr || path[0] == '\0')
    {
      std::printf("skipped: real tokenizer (NYAMP_TOKENIZER_JSON not set)\n");
      return 0;
    }

  std::string error;
  auto codec = nyamp::CreateTokenizerCodec(path, &error);
  if (!codec)
    {
      std::fprintf(stderr, "tokenizer refused: %s\n", error.c_str());
      return 1;
    }

  /* BOS comes from the template exactly once; nothing may add another. */
  std::vector<std::int32_t> prompt;
  CHECK(codec->Encode(kToolRequest, true, &prompt, &error));
  CHECK(prompt.size() > 100 && prompt.size() < 400);
  CHECK(prompt[0] == 0 && std::count(prompt.begin(), prompt.end(), 0) == 1);

  /* The ids the board produced for the weather smoke test. */
  const std::vector<std::int32_t> smoke = { 18,   2546, 943,   1135, 84,
                                            29996, 1822, 20,   2546, 943,
                                            35605, 1822, 57,   9305, 21,
                                            19 };
  nyamp::ChatUsage usage;
  usage.prompt_tokens = static_cast<std::uint32_t>(prompt.size());
  usage.completion_tokens = static_cast<std::uint32_t>(smoke.size());

  nyamp::Json response;
  nyamp::Json arguments;
  CHECK(nyamp::json_parse(nyamp::BuildChatResponse(codec->Decode(smoke),
                                                   kToolRequest, false, 1,
                                                   usage),
                          &response, &error));
  CHECK(Path(response, { "choices", "0", "finish_reason" })->str() ==
        "tool_calls");
  CHECK(Path(response,
             { "choices", "0", "message", "tool_calls", "0", "function",
               "name" })
            ->str() == "get_weather");
  CHECK(nyamp::json_parse(Path(response, { "choices", "0", "message",
                                           "tool_calls", "0", "function",
                                           "arguments" })
                              ->str(),
                          &arguments, &error));
  CHECK(arguments.find("city")->str() == "Dalian");
  std::printf("ok: real tokenizer, prompt=%zu tokens\n", prompt.size());
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
    { "plain answer", TestPlainAnswer },
    { "tool call", TestToolCall },
    { "truncated tool call stays text", TestTruncatedToolCallStaysText },
    { "prompt overflow", TestPromptOverflow },
    { "cancel mid-run", TestCancelMidRun },
    { "streamed tokens", TestStreaming },
    { "chunk reassembly", TestChunkReassembly },
    { "availability and capability", TestAvailability },
    { "logical load brings the tokenizer", TestLogicalLoadBringsTokenizer },
    { "real tokenizer", TestRealTokenizer },
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

  std::printf("nyampd chat tests passed (%d groups)\n", passed);
  return 0;
}
