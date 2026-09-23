/****************************************************************************
 * tools/amp/chat/nyamp_tokenizer.h
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

// Runtime tokenizer for MiniCPM5-1B, loaded from the model's own
// tokenizer.json (HuggingFace `tokenizers` format).
//
// RKLLM's built-in prompt path produces garbage for this model, so the daemon
// must hand the NPU runtime ready-made token ids.  They have to be the ids
// the reference tokenizer would produce: a 1B model has no slack to absorb a
// slightly different segmentation of its own chat scaffolding.
//
// What tokenizer.json specifies for this model, and therefore what is
// implemented here (load() refuses anything else instead of guessing):
//   normalizer      none
//   added tokens    510 literal strings cut out of the text first
//   pre-tokenizer   Split(\p{N}{1,3}) -> Split(GPT-4 style regex) -> ByteLevel
//   model           byte-level BPE, 130072 vocab entries, 129794 merges,
//                   no byte_fallback, no dropout, ignore_merges=false
//   decoder         ByteLevel

#ifndef NYAMP_TOKENIZER_H
#define NYAMP_TOKENIZER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nyamp
{

// Half-open byte range [begin, end) inside a prompt string.
struct ByteRange
{
  size_t begin;
  size_t end;
};

class Tokenizer
{
public:
  Tokenizer();
  ~Tokenizer();
  Tokenizer(const Tokenizer &) = delete;
  Tokenizer &operator=(const Tokenizer &) = delete;

  // Loads tokenizer.json.  Returns false with a reason when the file is
  // unreadable, malformed, or describes a pipeline other than the one above.
  bool load(const std::string &path, std::string *error);
  bool load_from_memory(const char *data, size_t size, std::string *error);

  // text -> ids, never adds BOS (the chat template carries its own "<s>").
  //
  // allow_special=true is the reference behaviour of
  // tokenizer(text, add_special_tokens=False): a literal "<|im_end|>" in the
  // text becomes the control token, whoever typed it.  allow_special=false
  // matches split_special_tokens=True: special tokens are spelled out as
  // ordinary text, non-special added tokens (<think>, <unused_token_N>) are
  // still recognized.  Invalid UTF-8 is replaced by U+FFFD first, since the
  // reference can only ever see valid strings.
  std::vector<int32_t> encode(std::string_view text,
                              bool allow_special = true) const;

  // Same as encode(text, true), except that a special token overlapping any
  // of the `untrusted` ranges is spelled out as text.  This is how a rendered
  // prompt keeps its template scaffolding as control tokens while user-typed
  // look-alikes stay inert.  Ranges must be sorted and non-overlapping.
  // Identical to the reference whenever the untrusted text holds no
  // look-alike, because the text is still tokenized as one piece.
  void encode_guarded(std::string_view text,
                      const std::vector<ByteRange> &untrusted,
                      std::vector<int32_t> *out) const;

  // ids -> text.  Unknown ids are skipped like the reference does.  Bytes
  // that do not form valid UTF-8 become U+FFFD (String::from_utf8_lossy).
  std::string decode(const std::vector<int32_t> &ids,
                     bool skip_special = false) const;

  // Raw bytes of one token (added tokens: their literal content).  Empty for
  // ids outside the vocabulary.
  std::string_view token_bytes(int32_t id) const;
  bool is_special(int32_t id) const;
  // Exact lookup of an added token or a vocab entry given as raw bytes.
  int32_t token_to_id(std::string_view token) const; // -1 when absent

  // Size of the id space (largest id + 1), not the embedding size.
  size_t vocab_size() const;
  // Bytes held after load(), for the README's memory figure.
  size_t memory_bytes() const;

private:
  struct Impl;
  Impl *impl_;
};

// Incremental decoder for token streaming.  A multi-byte character is often
// split across tokens (always for rare CJK and emoji), so bytes are held back
// until the character is complete; the concatenation of everything returned
// equals Tokenizer::decode() of the whole id list.
class StreamDecoder
{
public:
  explicit StreamDecoder(const Tokenizer &tokenizer, bool skip_special = false)
      : tokenizer_(tokenizer), skip_special_(skip_special)
  {
  }

  // Returns the text that became complete with this token (may be empty).
  std::string push(int32_t id);
  // Call once at end of generation: a dangling partial character is reported
  // as U+FFFD exactly like the one-shot decoder would.
  std::string flush();

private:
  const Tokenizer &tokenizer_;
  bool skip_special_;
  std::string pending_;
};

} // namespace nyamp

#endif // NYAMP_TOKENIZER_H
