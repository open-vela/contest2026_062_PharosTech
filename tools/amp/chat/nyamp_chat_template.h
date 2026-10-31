/****************************************************************************
 * tools/amp/chat/nyamp_chat_template.h
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

// Hand-written equivalent of MiniCPM5-1B's Jinja chat_template.
//
// A Jinja interpreter would be the generic answer, but the template is fixed
// for the life of the converted .rkllm model, the daemon must stay small and
// static, and a line-by-line transcription can be proven byte-identical with
// golden files - which is the property that actually matters.  The
// transcription keeps the template's quirks (documented in the .cpp) instead
// of "fixing" them, because the model was trained on the quirky output.

#ifndef NYAMP_CHAT_TEMPLATE_H
#define NYAMP_CHAT_TEMPLATE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nyamp_json.h"
#include "nyamp_tokenizer.h"

namespace nyamp
{

struct ChatTemplateOptions
{
  bool add_generation_prompt = true;

  // Mirrors the template's three-way `enable_thinking`: kUnset leaves the
  // assistant header bare, kOff appends the empty think block that tells the
  // model to answer directly (what the board was validated with), kOn opens
  // a think block.
  enum Thinking
  {
    kUnset,
    kOff,
    kOn
  };
  Thinking thinking = kOff;

  // OpenAI clients may send content as [{"type":"text","text":...}].  The
  // reference template renders any non-string content as "" (the question
  // silently disappears), so by default text parts are joined with "\n"
  // before rendering.  Set to false for strict reference behaviour.
  bool flatten_text_parts = true;

  // The reference prints object/array tool-call arguments with Python
  // repr() ({'a': True}).  Leave false to stay bit-exact; true prints JSON
  // instead, for experiments on which form the model follows better.
  bool json_for_nested_arguments = false;
};

struct RenderedPrompt
{
  std::string text;
  // Byte ranges of `text` that were copied from request data rather than
  // written by the template: user and tool message content, tool-call names
  // and argument values, and the tool definitions.  Feed them to
  // Tokenizer::encode_guarded() so a typed "<|im_end|>" cannot end a turn.
  // System and assistant content is deliberately NOT listed: the first is
  // authored by us, the second is the model's own earlier output whose
  // structural tokens must survive a round trip through the history.
  std::vector<ByteRange> untrusted;
};

// Accepts the tool list in OpenAI shape ({type:"function",function:{...}})
// or in the Anthropic-like shape our agent authors ({name, description,
// input_schema}) and returns the OpenAI shape the template serializes.
// Entries already in OpenAI shape are passed through untouched, key order
// included, because every byte of them ends up in the prompt.
Json normalize_tools(const Json &tools);

// Renders an OpenAI chat-completions request ({"messages":[...],
// "tools":[...]}).  Returns false with a reason for requests the reference
// template would also reject (no messages) or that are not objects.
bool render_chat_prompt(const Json &request,
                        const ChatTemplateOptions &options,
                        RenderedPrompt *out, std::string *error);

struct EncodedPrompt
{
  RenderedPrompt prompt;
  std::vector<int32_t> ids;
};

// Request JSON text -> prompt string + ids, the one call the daemon needs.
// guard_untrusted=false reproduces the reference tokenization verbatim;
// true (recommended in production) differs only when request data contains
// a special-token look-alike.
bool encode_chat_request(const Tokenizer &tokenizer, std::string_view request,
                         const ChatTemplateOptions &options,
                         bool guard_untrusted, EncodedPrompt *out,
                         std::string *error);

} // namespace nyamp

#endif // NYAMP_CHAT_TEMPLATE_H
