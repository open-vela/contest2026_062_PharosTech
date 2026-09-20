/****************************************************************************
 * tools/amp/nyampd/nyampd_chat.cpp
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

#include "nyamp_chat_parse.h"
#include "nyamp_chat_template.h"
#include "nyamp_json.h"
#include "nyamp_tokenizer.h"

#include <cstdio>
#include <ctime>

namespace nyamp
{
namespace
{

/* The model name echoed in responses.  It identifies the converted model this
 * daemon was validated with, not whatever file happens to be loaded.
 */

constexpr char kModelName[] = "minicpm5-1b";

class TokenizerCodec final : public ChatCodec
{
public:
  bool Load(const std::string &path, std::string *error)
  {
    if (!tokenizer_.load(path, error))
      {
        return false;
      }

    /* The stop ids are constants of the protocol with the control domain and
     * of the backend configuration.  A vocabulary that disagrees belongs to
     * another model, and running it would stop at the wrong places.
     */
    if (tokenizer_.token_to_id("</s>") != kChatStopIds[0] ||
        tokenizer_.token_to_id("<|im_end|>") != kChatStopIds[1] ||
        tokenizer_.token_to_id("<|im_start|>") != kChatStopIds[2])
      {
        *error = "tokenizer.json stop tokens do not match MiniCPM5";
        return false;
      }

    return true;
  }

  bool Encode(std::string_view request_json, bool guard_untrusted,
              std::vector<std::int32_t> *ids, std::string *error) override
  {
    EncodedPrompt prompt;

    if (!encode_chat_request(tokenizer_, request_json, ChatTemplateOptions(),
                             guard_untrusted, &prompt, error))
      {
        return false;
      }

    *ids = std::move(prompt.ids);
    return true;
  }

  std::string Decode(const std::vector<std::int32_t> &ids) override
  {
    return tokenizer_.decode(ids, false);
  }

  void StreamReset() override
  {
    stream_ = std::make_unique<StreamDecoder>(tokenizer_, false);
  }

  std::string StreamPush(std::int32_t id) override
  {
    return stream_ ? stream_->push(id) : std::string();
  }

  std::string StreamFlush() override
  {
    return stream_ ? stream_->flush() : std::string();
  }

private:
  Tokenizer tokenizer_;
  std::unique_ptr<StreamDecoder> stream_;
};

} // namespace

std::unique_ptr<ChatCodec> CreateTokenizerCodec(const std::string &path,
                                                std::string *error)
{
  auto codec = std::make_unique<TokenizerCodec>();
  std::string reason;

  if (!codec->Load(path, &reason))
    {
      if (error != nullptr)
        {
          *error = reason;
        }

      return nullptr;
    }

  return codec;
}

std::string BuildChatResponse(std::string_view output_text,
                              std::string_view request_json,
                              bool hit_length_limit, std::uint64_t request_id,
                              const ChatUsage &usage)
{
  Json request;
  std::string ignored;
  const Json *tools = nullptr;
  ParseOptions options;
  ResponseMeta meta;
  char id[40];

  /* The request already rendered, so it parses; a failure here only costs
   * the argument typing, never the response.
   */
  if (json_parse(request_json, &request, &ignored) && request.is_object())
    {
      tools = request.find("tools");

      /* With thinking enabled the prompt ends inside an open <think>, so the
       * output starts in the middle of the reasoning with no opening tag.
       */
      if (const Json *kwargs = request.find("chat_template_kwargs"))
        {
          const Json *thinking =
              kwargs->is_object() ? kwargs->find("enable_thinking") : nullptr;
          options.prompt_opened_think = thinking != nullptr &&
                                        thinking->truthy();
        }
    }

  std::snprintf(id, sizeof(id), "chatcmpl-%016llx",
                static_cast<unsigned long long>(request_id));
  options.hit_length_limit = hit_length_limit;
  options.id_seed = id;

  meta.id = id;
  meta.model = kModelName;
  meta.created = static_cast<std::int64_t>(std::time(nullptr));
  meta.prompt_tokens = usage.prompt_tokens;
  meta.completion_tokens = usage.completion_tokens;

  return completion_to_json(parse_completion(output_text, tools, options),
                            meta);
}

} // namespace nyamp
