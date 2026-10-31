/****************************************************************************
 * tools/amp/nyampd/nyampd_chat.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_CHAT_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_CHAT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nyamp
{

/* Ids that end an assistant turn for MiniCPM5: </s>, <|im_end|>, and -- as a
 * defence -- <|im_start|>: a model that opens a new turn without closing the
 * current one has nothing more to say that anyone should read.
 */

constexpr std::int32_t kChatStopIds[] = { 1, 130073, 130072 };

/* Used when the request leaves max_new_tokens at zero. */

constexpr std::uint32_t kChatDefaultNewTokens = 256;

/****************************************************************************
 * Name: ChatCodec
 *
 * Description:
 *   The part of a chat completion that depends on the model's vocabulary:
 *   request JSON to prompt ids, and generated ids back to text.
 *
 *   It is an interface so the service can be tested without tokenizer.json,
 *   which is a third-party asset that is never in this repository.  The test
 *   codec still goes through the real chat template and the real output
 *   parser -- neither needs the vocabulary -- so only the id mapping itself
 *   is substituted.
 *
 *   A codec is used by one request at a time.
 *
 ****************************************************************************/

class ChatCodec
{
public:
  virtual ~ChatCodec() = default;

  /* Render the request through the chat template and tokenize it.  BOS comes
   * from the template; nothing may add another.
   */
  virtual bool Encode(std::string_view request_json, bool guard_untrusted,
                      std::vector<std::int32_t> *ids, std::string *error) = 0;

  /* Special tokens are KEPT: "<function" and "<param" are single special
   * tokens, and they are the whole structure of a tool call.
   */
  virtual std::string Decode(const std::vector<std::int32_t> &ids) = 0;

  /* Incremental decoding for token events: returns the text that became
   * complete with this id, so a multi-byte character split across two tokens
   * is never emitted as two invalid halves.
   */
  virtual void StreamReset() = 0;
  virtual std::string StreamPush(std::int32_t id) = 0;
  virtual std::string StreamFlush() = 0;
};

/* Build a codec from the tokenizer.json at `path`.  Returns null with a
 * reason when the file is missing or describes another tokenizer pipeline.
 */

using ChatCodecFactory = std::function<std::unique_ptr<ChatCodec>(
    const std::string &path, std::string *error)>;

std::unique_ptr<ChatCodec> CreateTokenizerCodec(const std::string &path,
                                                std::string *error);

struct ChatUsage
{
  std::uint32_t prompt_tokens = 0;
  std::uint32_t completion_tokens = 0;
};

/****************************************************************************
 * Name: BuildChatResponse
 *
 * Description:
 *   Model output text to an OpenAI chat.completion object.  The request is
 *   needed again because its tool schemas type the arguments: the model's
 *   wire format is untyped text.  A call that does not parse completely stays
 *   in `content`; it is never executed by halves.
 *
 ****************************************************************************/

std::string BuildChatResponse(std::string_view output_text,
                              std::string_view request_json,
                              bool hit_length_limit, std::uint64_t request_id,
                              const ChatUsage &usage);

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_CHAT_H */
