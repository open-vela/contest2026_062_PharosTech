/****************************************************************************
 * tools/amp/chat/nyamp_chat_parse.h
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

// Turns MiniCPM5-1B output text into an OpenAI chat-completions response.
//
// The model announces tool calls as
//   <function name="get_weather"><param name="city">Dalian</param></function>
// where <function, </function>, <param and </param> are single special
// tokens.  The daemon must therefore decode with special tokens KEPT
// (skip_special=false), otherwise the structure is gone before it gets here.
//
// Design rule: output of a 1B model under a 2K window is routinely truncated
// or slightly malformed.  Nothing in here throws, asserts, or drops text: a
// call that does not parse completely is handed back as plain content, so
// the agent shows it (or retries) instead of executing half a command.

#ifndef NYAMP_CHAT_PARSE_H
#define NYAMP_CHAT_PARSE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nyamp_json.h"

namespace nyamp
{

struct ToolCall
{
  std::string id;
  std::string name;
  std::string arguments; // JSON object text, as OpenAI specifies
};

struct ParsedCompletion
{
  std::string content;           // may be empty
  std::string reasoning_content; // <think> block, when the model emitted one
  std::vector<ToolCall> tool_calls;
  std::string finish_reason; // "stop" | "length" | "tool_calls"
};

struct ParseOptions
{
  // Why generation ended, as seen by the daemon: true when the token budget
  // or the context window ran out before a stop token appeared.
  bool hit_length_limit = false;
  // True when the prompt ended in an open <think> (thinking enabled), so
  // the output starts inside the reasoning without an opening tag.
  bool prompt_opened_think = false;
  // Mixed into the generated call ids so they are unique per completion yet
  // reproducible in tests.  The request id is a good value.
  std::string id_seed;
};

// `tools` is the request's tool list (either accepted shape, may be null).
// It supplies parameter types: the wire format is untyped text, and without
// the schema "42" for an integer parameter would reach the agent as a string.
ParsedCompletion parse_completion(std::string_view text, const Json *tools,
                                  const ParseOptions &options);

struct ResponseMeta
{
  std::string id;    // "chatcmpl-..."
  std::string model; // echoed back, e.g. "minicpm5-1b"
  int64_t created = 0;
  int64_t prompt_tokens = 0;
  int64_t completion_tokens = 0;
};

// OpenAI chat.completion object.  content is null when there is none (tool
// call only), as clients expect.  Strings are forced to valid UTF-8.
std::string completion_to_json(const ParsedCompletion &completion,
                               const ResponseMeta &meta);

// Ids that must stop generation for this model, in the order
// generation_config.json lists them: </s>=1 and <|im_end|>=130073.
// Resolved through the tokenizer by the daemon at start-up; the constants
// are here so a mismatch can be detected.
constexpr int32_t kStopTokenEos = 1;
constexpr int32_t kStopTokenImEnd = 130073;

} // namespace nyamp

#endif // NYAMP_CHAT_PARSE_H
