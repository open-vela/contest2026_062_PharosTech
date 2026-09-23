/****************************************************************************
 * tools/amp/chat/nyamp_tokenizer.cpp
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

#include "nyamp_tokenizer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "nyamp_json.h"
#include "nyamp_unicode.h"

namespace nyamp
{

namespace
{

constexpr uint32_t kNone = 0xFFFFFFFFu;

// The two Split patterns this implementation hard-codes.  load() compares
// them verbatim: if a future model revision changes the regex, a loud load
// failure is far better than a tokenizer that is quietly wrong on some inputs.
constexpr const char *kDigitPattern = "\\p{N}{1,3}";
constexpr const char *kWordPattern =
    "(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\\r\\n\\p{L}\\p{N}]?\\p{L}+|\\p{N}+| "
    "?[^\\s\\p{L}\\p{N}]+[\\r\\n]*|\\s*[\\r\\n]+|\\s+(?!\\S)|\\s+";

enum TokenFlag : uint8_t
{
  kFlagPresent = 1,
  kFlagAdded = 2,
  kFlagSpecial = 4,
};

uint64_t hash_bytes(const char *data, size_t size)
{
  uint64_t h = 1469598103934665603ull; // FNV-1a: tiny, good enough for 130k
  for (size_t i = 0; i < size; ++i)
    {
      h ^= static_cast<unsigned char>(data[i]);
      h *= 1099511628211ull;
    }
  return h;
}

uint64_t hash_pair(uint64_t key)
{
  // splitmix64 finalizer; pair keys are two small ids and would otherwise
  // cluster badly in an open-addressed table.
  key ^= key >> 30;
  key *= 0xbf58476d1ce4e5b9ull;
  key ^= key >> 27;
  key *= 0x94d049bb133111ebull;
  key ^= key >> 31;
  return key;
}

size_t table_size_for(size_t entries)
{
  size_t size = 1024;
  while (size < entries * 2)
    size <<= 1; // load factor <= 0.5
  return size;
}

// GPT-2 "bytes to unicode": printable Latin-1 bytes map to themselves, the
// remaining 68 bytes to U+0100.. in ascending order.  Returns the inverse.
struct ByteLevelMap
{
  int16_t cp_to_byte[0x144];
  ByteLevelMap()
  {
    for (auto &v : cp_to_byte)
      v = -1;
    int extra = 0;
    for (int b = 0; b < 256; ++b)
      {
        bool direct =
            (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174);
        int cp = direct ? b : 256 + extra++;
        cp_to_byte[cp] = static_cast<int16_t>(b);
      }
  }
};

// Converts a byte-level vocab string to raw bytes.  Returns false when a
// character is outside the 256-symbol alphabet; such a token can never be
// produced by BPE and is decoded literally, as the reference decoder does.
bool byte_level_to_raw(const std::string &in, std::string *out)
{
  static const ByteLevelMap map;
  out->clear();
  const unsigned char *p = reinterpret_cast<const unsigned char *>(in.data());
  size_t n = in.size();
  for (size_t i = 0; i < n;)
    {
      unsigned cp;
      if (p[i] < 0x80)
        {
          cp = p[i];
          i += 1;
        }
      else if ((p[i] & 0xE0) == 0xC0 && i + 1 < n)
        {
          cp = ((p[i] & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu);
          i += 2;
        }
      else
        {
          return false;
        }
      if (cp >= 0x144 || map.cp_to_byte[cp] < 0)
        return false;
      out->push_back(static_cast<char>(map.cp_to_byte[cp]));
    }
  return true;
}

struct Merge
{
  uint32_t a;
  uint32_t b;
  uint32_t id;
};

struct AddedToken
{
  std::string content;
  int32_t id;
  bool special;
  bool normalized;
};

struct Symbol
{
  int32_t prev;
  int32_t next;
  uint32_t id;
  bool alive;
};

struct HeapItem
{
  uint32_t rank;
  int32_t pos;
};

// std heap is a max-heap, so "less" means "merge later".
struct HeapLater
{
  bool operator()(const HeapItem &x, const HeapItem &y) const
  {
    if (x.rank != y.rank)
      return x.rank > y.rank;
    return x.pos > y.pos;
  }
};

bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

} // namespace

struct Tokenizer::Impl
{
  std::string arena;               // raw bytes of every token, back to back
  std::vector<uint32_t> tok_off;   // by id
  std::vector<uint32_t> tok_len;   // by id
  std::vector<uint8_t> tok_flags;  // by id
  std::vector<uint32_t> str_slots; // open-addressed: raw bytes -> id
  std::vector<Merge> merges;       // index == rank
  std::vector<uint32_t> merge_slots;
  uint32_t byte_id[256];
  std::vector<AddedToken> added;
  // Candidate added tokens per first byte, longest first, so the first hit at
  // a position is the leftmost-longest match the reference's Aho-Corasick
  // automaton reports.  [0] = non-normalized pass, [1] = normalized pass.
  std::vector<uint16_t> added_index[2][256];

  void set_token(uint32_t id, const std::string &raw, uint8_t flags)
  {
    if (id >= tok_off.size())
      {
        tok_off.resize(id + 1, 0);
        tok_len.resize(id + 1, 0);
        tok_flags.resize(id + 1, 0);
      }
    tok_off[id] = static_cast<uint32_t>(arena.size());
    tok_len[id] = static_cast<uint32_t>(raw.size());
    tok_flags[id] |= flags | kFlagPresent;
    arena += raw;
  }

  std::string_view bytes(uint32_t id) const
  {
    if (id >= tok_off.size() || !(tok_flags[id] & kFlagPresent))
      return {};
    return std::string_view(arena.data() + tok_off[id], tok_len[id]);
  }

  uint32_t lookup(const char *data, size_t size) const
  {
    if (str_slots.empty())
      return kNone;
    size_t mask = str_slots.size() - 1;
    for (size_t i = hash_bytes(data, size) & mask;; i = (i + 1) & mask)
      {
        uint32_t id = str_slots[i];
        if (id == kNone)
          return kNone;
        if (tok_len[id] == size &&
            std::memcmp(arena.data() + tok_off[id], data, size) == 0)
          {
            return id;
          }
      }
  }

  void build_string_index(uint32_t vocab_ids)
  {
    str_slots.assign(table_size_for(vocab_ids), kNone);
    size_t mask = str_slots.size() - 1;
    for (uint32_t id = 0; id < vocab_ids; ++id)
      {
        if (!(tok_flags[id] & kFlagPresent))
          continue;
        size_t i = hash_bytes(arena.data() + tok_off[id], tok_len[id]) & mask;
        while (str_slots[i] != kNone)
          i = (i + 1) & mask;
        str_slots[i] = id;
      }
  }

  uint32_t find_merge(uint32_t a, uint32_t b) const
  {
    uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
    size_t mask = merge_slots.size() - 1;
    for (size_t i = hash_pair(key) & mask;; i = (i + 1) & mask)
      {
        uint32_t rank = merge_slots[i];
        if (rank == kNone)
          return kNone;
        if (merges[rank].a == a && merges[rank].b == b)
          return rank;
      }
  }

  void build_merge_index()
  {
    merge_slots.assign(table_size_for(merges.size()), kNone);
    size_t mask = merge_slots.size() - 1;
    for (uint32_t rank = 0; rank < merges.size(); ++rank)
      {
        uint64_t key =
            (static_cast<uint64_t>(merges[rank].a) << 32) | merges[rank].b;
        size_t i = hash_pair(key) & mask;
        while (merge_slots[i] != kNone)
          {
            const Merge &other = merges[merge_slots[i]];
            // The reference keeps its merges in a HashMap, so a repeated pair
            // ends up with the later rank.  Mirror that instead of chaining.
            if (other.a == merges[rank].a && other.b == merges[rank].b)
              break;
            i = (i + 1) & mask;
          }
        merge_slots[i] = rank;
      }
  }

  bool load_vocab(JsonCursor *cur, uint32_t *vocab_ids, std::string *error);
  bool load_merges(JsonCursor *cur, std::string *error);
  bool load_added(const Json &list, std::string *error);
  bool check_pipeline(const Json &root, const Json &model,
                      std::string *error) const;

  void bpe(const unsigned char *s, size_t n, std::vector<Symbol> *symbols,
           std::vector<HeapItem> *heap, std::vector<int32_t> *out) const;
  void encode_plain(const char *s, size_t n, std::vector<int32_t> *out) const;
  void encode_split(std::string_view text, int pass, size_t base,
                    bool allow_special, const std::vector<ByteRange> *guard,
                    std::vector<int32_t> *out) const;
};

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool Tokenizer::Impl::load_vocab(JsonCursor *cur, uint32_t *vocab_ids,
                                 std::string *error)
{
  if (!cur->consume('{'))
    {
      *error = "model.vocab is not an object";
      return false;
    }
  // Reserving up front avoids doubling a multi-megabyte buffer while the
  // 10 MB source file is also resident.
  arena.reserve(1200 * 1024);
  std::string key;
  std::string raw;
  uint32_t max_id = 0;
  if (!cur->consume('}'))
    {
      for (;;)
        {
          Json value;
          if (!cur->parse_string(&key) || !cur->consume(':') ||
              !cur->parse_value(&value) || !value.number_is_integer())
            {
              *error = "bad vocab entry: " + cur->error();
              return false;
            }
          unsigned long id = std::strtoul(value.str().c_str(), nullptr, 10);
          if (value.str()[0] == '-' || id > 0xFFFFFF)
            {
              *error = "vocab id out of range";
              return false;
            }
          if (!byte_level_to_raw(key, &raw))
            raw = key;
          set_token(static_cast<uint32_t>(id), raw, 0);
          max_id = std::max<uint32_t>(max_id, static_cast<uint32_t>(id));
          if (cur->consume(','))
            continue;
          if (cur->consume('}'))
            break;
          *error = "malformed vocab object";
          return false;
        }
    }
  *vocab_ids = max_id + 1;
  build_string_index(*vocab_ids);
  for (int b = 0; b < 256; ++b)
    {
      char c = static_cast<char>(b);
      byte_id[b] = lookup(&c, 1);
      if (byte_id[b] == kNone)
        {
          // Without byte_fallback/unk handling every byte must be a token, or
          // some input would be unencodable.  True for this model; checked so
          // a different vocab fails at load instead of at 3 am.
          *error =
              "vocab lacks a single-byte token for byte " + std::to_string(b);
          return false;
        }
    }
  return true;
}

bool Tokenizer::Impl::load_merges(JsonCursor *cur, std::string *error)
{
  if (!cur->consume('['))
    {
      *error = "model.merges is not an array";
      return false;
    }
  merges.reserve(131072);
  std::string left;
  std::string right;
  std::string raw_left;
  std::string raw_right;
  if (!cur->consume(']'))
    {
      for (;;)
        {
          if (cur->peek() == '[')
            {
              // tokenizers >= 0.20 layout: ["left", "right"]
              if (!cur->consume('[') || !cur->parse_string(&left) ||
                  !cur->consume(',') || !cur->parse_string(&right) ||
                  !cur->consume(']'))
                {
                  *error = "bad merge entry: " + cur->error();
                  return false;
                }
            }
          else
            {
              // Legacy layout: "left right".  A byte-level token never
              // contains a real space, so the first space is the separator.
              if (!cur->parse_string(&left))
                {
                  *error = "bad merge entry: " + cur->error();
                  return false;
                }
              size_t space = left.find(' ');
              if (space == std::string::npos)
                {
                  *error = "legacy merge entry without separator";
                  return false;
                }
              right = left.substr(space + 1);
              left.resize(space);
            }
          if (!byte_level_to_raw(left, &raw_left))
            raw_left = left;
          if (!byte_level_to_raw(right, &raw_right))
            raw_right = right;
          uint32_t a = lookup(raw_left.data(), raw_left.size());
          uint32_t b = lookup(raw_right.data(), raw_right.size());
          raw_left += raw_right;
          uint32_t id = lookup(raw_left.data(), raw_left.size());
          if (a == kNone || b == kNone || id == kNone)
            {
              *error = "merge refers to a token outside the vocabulary";
              return false;
            }
          merges.push_back(Merge{ a, b, id });
          if (cur->consume(','))
            continue;
          if (cur->consume(']'))
            break;
          *error = "malformed merges array";
          return false;
        }
    }
  merges.shrink_to_fit();
  build_merge_index();
  return true;
}

bool Tokenizer::Impl::load_added(const Json &list, std::string *error)
{
  if (!list.is_array())
    {
      *error = "added_tokens is not an array";
      return false;
    }
  for (const Json &entry : list.items())
    {
      const Json *id = entry.find("id");
      const Json *content = entry.find("content");
      if (id == nullptr || !id->number_is_integer() || content == nullptr ||
          !content->is_string() || content->str().empty())
        {
          *error = "bad added_tokens entry";
          return false;
        }
      for (const char *flag : { "single_word", "lstrip", "rstrip" })
        {
          const Json *v = entry.find(flag);
          if (v != nullptr && v->truthy())
            {
              // None of the 510 tokens use these; implementing them untested
              // would only pretend to be compatible.
              *error =
                  std::string("added token option not supported: ") + flag;
              return false;
            }
        }
      AddedToken token;
      token.content = content->str();
      token.id =
          static_cast<int32_t>(std::strtol(id->str().c_str(), nullptr, 10));
      if (token.id < 0 || token.id > 0xFFFFFF)
        {
          *error = "added token id out of range";
          return false;
        }
      const Json *special = entry.find("special");
      const Json *normalized = entry.find("normalized");
      token.special = special != nullptr && special->truthy();
      token.normalized = normalized != nullptr && normalized->truthy();
      uint8_t flags = kFlagAdded | (token.special ? kFlagSpecial : 0);
      uint32_t uid = static_cast<uint32_t>(token.id);
      if (uid < tok_flags.size() && (tok_flags[uid] & kFlagPresent))
        {
          tok_flags[uid] |= flags; // ids 0..21 also live in the BPE vocab
        }
      else
        {
          set_token(uid, token.content, flags);
        }
      added.push_back(std::move(token));
    }
  if (added.size() > 0xFFFF)
    {
      *error = "too many added tokens";
      return false;
    }
  for (size_t i = 0; i < added.size(); ++i)
    {
      int pass = added[i].normalized ? 1 : 0;
      unsigned char first = static_cast<unsigned char>(added[i].content[0]);
      added_index[pass][first].push_back(static_cast<uint16_t>(i));
    }
  for (auto &pass : added_index)
    {
      for (auto &bucket : pass)
        {
          std::stable_sort(
              bucket.begin(), bucket.end(), [this](uint16_t x, uint16_t y) {
                return added[x].content.size() > added[y].content.size();
              });
        }
    }
  return true;
}

bool Tokenizer::Impl::check_pipeline(const Json &root, const Json &model,
                                     std::string *error) const
{
  auto str_is = [](const Json *v, const char *want) {
    return v != nullptr && v->is_string() && v->str() == want;
  };
  auto absent = [](const Json *v) { return v == nullptr || !v->truthy(); };

  if (!str_is(model.find("type"), "BPE"))
    {
      *error = "model.type is not BPE";
      return false;
    }
  for (const char *key :
       { "dropout", "continuing_subword_prefix", "end_of_word_suffix",
         "byte_fallback", "ignore_merges", "fuse_unk" })
    {
      if (!absent(model.find(key)))
        {
          *error = std::string("unsupported BPE option: ") + key;
          return false;
        }
    }
  if (!absent(root.find("normalizer")))
    {
      *error = "a normalizer is configured; this implementation has none";
      return false;
    }
  const Json *pre = root.find("pre_tokenizer");
  const Json *steps = pre != nullptr ? pre->find("pretokenizers") : nullptr;
  if (pre == nullptr || !str_is(pre->find("type"), "Sequence") ||
      steps == nullptr || !steps->is_array() || steps->items().size() != 3)
    {
      *error = "pre_tokenizer is not the expected 3-step Sequence";
      return false;
    }
  const char *patterns[2] = { kDigitPattern, kWordPattern };
  for (int i = 0; i < 2; ++i)
    {
      const Json &step = steps->items()[static_cast<size_t>(i)];
      const Json *pattern = step.find("pattern");
      if (!str_is(step.find("type"), "Split") ||
          !str_is(step.find("behavior"), "Isolated") ||
          !absent(step.find("invert")) || pattern == nullptr ||
          !str_is(pattern->find("Regex"), patterns[i]))
        {
          *error =
              "pre_tokenizer Split step differs from the implemented regex";
          return false;
        }
    }
  const Json &byte_level = steps->items()[2];
  if (!str_is(byte_level.find("type"), "ByteLevel") ||
      !absent(byte_level.find("add_prefix_space")) ||
      !absent(byte_level.find("use_regex")))
    {
      *error = "pre_tokenizer ByteLevel step has unsupported options";
      return false;
    }
  const Json *decoder = root.find("decoder");
  if (decoder == nullptr || !str_is(decoder->find("type"), "ByteLevel"))
    {
      *error = "decoder is not ByteLevel";
      return false;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

void Tokenizer::Impl::bpe(const unsigned char *s, size_t n,
                          std::vector<Symbol> *symbols,
                          std::vector<HeapItem> *heap,
                          std::vector<int32_t> *out) const
{
  if (n == 1)
    {
      out->push_back(static_cast<int32_t>(byte_id[s[0]]));
      return;
    }
  // Same procedure as tokenizers' Word::merge_all: a priority queue of
  // candidate merges ordered by (rank, position), lazily invalidated.  The
  // queue keeps a 10k-character run of one letter at O(n log n); the naive
  // rescan would make such an input a denial of service on an A72.
  symbols->clear();
  heap->clear();
  for (size_t i = 0; i < n; ++i)
    {
      symbols->push_back(Symbol{ static_cast<int32_t>(i) - 1,
                                 i + 1 < n ? static_cast<int32_t>(i) + 1 : -1,
                                 byte_id[s[i]], true });
    }
  std::vector<Symbol> &sym = *symbols;
  for (size_t i = 0; i + 1 < n; ++i)
    {
      uint32_t rank = find_merge(sym[i].id, sym[i + 1].id);
      if (rank != kNone)
        heap->push_back(HeapItem{ rank, static_cast<int32_t>(i) });
    }
  std::make_heap(heap->begin(), heap->end(), HeapLater());

  while (!heap->empty())
    {
      std::pop_heap(heap->begin(), heap->end(), HeapLater());
      HeapItem top = heap->back();
      heap->pop_back();
      Symbol &left = sym[static_cast<size_t>(top.pos)];
      if (!left.alive || left.next < 0)
        continue;
      Symbol &right = sym[static_cast<size_t>(left.next)];
      const Merge &merge = merges[top.rank];
      // Stale entry: one side was merged away since it was queued.
      if (left.id != merge.a || right.id != merge.b)
        continue;

      left.id = merge.id;
      right.alive = false;
      left.next = right.next;
      if (right.next >= 0)
        sym[static_cast<size_t>(right.next)].prev = top.pos;

      if (left.prev >= 0)
        {
          uint32_t rank =
              find_merge(sym[static_cast<size_t>(left.prev)].id, left.id);
          if (rank != kNone)
            {
              heap->push_back(HeapItem{ rank, left.prev });
              std::push_heap(heap->begin(), heap->end(), HeapLater());
            }
        }
      if (left.next >= 0)
        {
          uint32_t rank =
              find_merge(left.id, sym[static_cast<size_t>(left.next)].id);
          if (rank != kNone)
            {
              heap->push_back(HeapItem{ rank, top.pos });
              std::push_heap(heap->begin(), heap->end(), HeapLater());
            }
        }
    }
  for (int32_t i = 0; i >= 0; i = sym[static_cast<size_t>(i)].next)
    {
      out->push_back(static_cast<int32_t>(sym[static_cast<size_t>(i)].id));
    }
}

// Pre-tokenizes one stretch of ordinary text and runs BPE on every piece.
//
// The regex is evaluated by hand.  That is safe here because every
// alternative is a fixed sequence of character-class runs; the comments name
// the alternative each branch implements, in the regex's own priority order.
void Tokenizer::Impl::encode_plain(const char *s, size_t n,
                                   std::vector<int32_t> *out) const
{
  if (n == 0)
    return;
  std::vector<uint32_t> cp;
  std::vector<uint32_t> off;
  std::vector<uint8_t> cls;
  cp.reserve(n);
  off.reserve(n + 1);
  cls.reserve(n);
  const unsigned char *u = reinterpret_cast<const unsigned char *>(s);
  for (size_t i = 0; i < n;)
    {
      unsigned char c = u[i];
      uint32_t value;
      size_t len;
      // Input is valid UTF-8 by now (sanitized at the API boundary), so the
      // lead byte alone decides the length.
      if (c < 0x80)
        {
          value = c;
          len = 1;
        }
      else if (c < 0xE0)
        {
          value = ((c & 0x1Fu) << 6) | (u[i + 1] & 0x3Fu);
          len = 2;
        }
      else if (c < 0xF0)
        {
          value = ((c & 0x0Fu) << 12) | ((u[i + 1] & 0x3Fu) << 6) |
                  (u[i + 2] & 0x3Fu);
          len = 3;
        }
      else
        {
          value = ((c & 0x07u) << 18) | ((u[i + 1] & 0x3Fu) << 12) |
                  ((u[i + 2] & 0x3Fu) << 6) | (u[i + 3] & 0x3Fu);
          len = 4;
        }
      cp.push_back(value);
      off.push_back(static_cast<uint32_t>(i));
      cls.push_back(unicode_classify(value));
      i += len;
    }
  off.push_back(static_cast<uint32_t>(n));

  std::vector<Symbol> symbols;
  std::vector<HeapItem> heap;
  auto emit = [&](size_t from, size_t to) {
    bpe(u + off[from], off[to] - off[from], &symbols, &heap, out);
  };
  auto lower = [&](size_t k) -> uint32_t {
    uint32_t c = cp[k];
    if (c >= 'A' && c <= 'Z')
      return c + 32;
    // Oniguruma's (?i) folds U+017F LATIN SMALL LETTER LONG S onto 's'.
    // Measured against the reference over all of Unicode: it is the only
    // non-ASCII character that matches any of the contraction suffixes.
    if (c == 0x17F)
      return 's';
    return c;
  };

  const size_t count = cp.size();
  size_t i = 0;
  while (i < count)
    {
      // First Split: \p{N}{1,3}, Isolated.  Digit runs are cut into groups of
      // three from the left; everything else goes to the second Split.
      if (cls[i] & kClassNumber)
        {
          size_t j = i;
          while (j < count && j - i < 3 && (cls[j] & kClassNumber))
            ++j;
          emit(i, j);
          i = j;
          continue;
        }
      // The second Split only ever sees one digit-free piece at a time, so
      // `end` is a hard end-of-text for its look-ahead.  (This is why
      // "a  1" yields "a", "  ", "1" here and not GPT-4's "a", " ", " 1".)
      size_t end = i;
      while (end < count && !(cls[end] & kClassNumber))
        ++end;

      while (i < end)
        {
          uint8_t c = cls[i];
          // 1. (?i:'s|'t|'re|'ve|'m|'ll|'d)
          if (cp[i] == '\'' && i + 1 < end)
            {
              uint32_t a = lower(i + 1);
              uint32_t b = i + 2 < end ? lower(i + 2) : 0;
              size_t len = 0;
              if (a == 's' || a == 't' || a == 'm' || a == 'd')
                {
                  len = 2;
                }
              else if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') ||
                       (a == 'l' && b == 'l'))
                {
                  len = 3;
                }
              if (len != 0)
                {
                  emit(i, i + len);
                  i += len;
                  continue;
                }
            }
          // 2. [^\r\n\p{L}\p{N}]?\p{L}+
          {
            size_t start = i;
            size_t j = i;
            if (!(c & (kClassLetter | kClassNumber | kClassNewline)) &&
                i + 1 < end && (cls[i + 1] & kClassLetter))
              {
                j = i + 1;
              }
            if (cls[j] & kClassLetter)
              {
                while (j < end && (cls[j] & kClassLetter))
                  ++j;
                emit(start, j);
                i = j;
                continue;
              }
          }
          // 3. \p{N}+ cannot match: the piece holds no digit by construction.
          // 4.  ?[^\s\p{L}\p{N}]+[\r\n]*
          {
            size_t j = i;
            if (cp[j] == ' ' && j + 1 < end)
              ++j;
            if (!(cls[j] & (kClassSpace | kClassLetter | kClassNumber)))
              {
                while (j < end &&
                       !(cls[j] & (kClassSpace | kClassLetter | kClassNumber)))
                  {
                    ++j;
                  }
                while (j < end && (cls[j] & kClassNewline))
                  ++j;
                emit(i, j);
                i = j;
                continue;
              }
          }
          // Only whitespace can reach this point.
          size_t run = i;
          while (run < end && (cls[run] & kClassSpace))
            ++run;
          // 5. \s*[\r\n]+  - greedy \s* backs off to the last newline of the
          // run.
          size_t last_newline = run;
          while (last_newline > i && !(cls[last_newline - 1] & kClassNewline))
            {
              --last_newline;
            }
          if (last_newline > i)
            {
              emit(i, last_newline);
              i = last_newline;
              continue;
            }
          // 6. \s+(?!\S) - leave the last blank to prefix the following word,
          //    unless the run reaches the end of the piece.
          if (run == end || run - i == 1)
            {
              // run - i == 1 with a following non-space falls through to 7.
              // \s+
              emit(i, run);
              i = run;
            }
          else
            {
              emit(i, run - 1);
              i = run - 1;
            }
        }
    }
}

// Cuts added tokens out of `text`, then recurses for the second pass and
// finally hands the remaining plain stretches to encode_plain().
//
// Two passes because the reference keeps two automata: tokens flagged
// normalized=false are matched against the raw text first, the rest against
// the normalized text.  There is no normalizer, so both see the same bytes,
// but the ordering still decides who wins when candidates of both kinds
// overlap.
void Tokenizer::Impl::encode_split(std::string_view text, int pass,
                                   size_t base, bool allow_special,
                                   const std::vector<ByteRange> *guard,
                                   std::vector<int32_t> *out) const
{
  if (pass == 2)
    {
      encode_plain(text.data(), text.size(), out);
      return;
    }
  size_t plain_start = 0;
  size_t guard_cursor = 0;
  size_t p = 0;
  while (p < text.size())
    {
      const auto &bucket =
          added_index[pass][static_cast<unsigned char>(text[p])];
      const AddedToken *hit = nullptr;
      for (uint16_t index : bucket)
        {
          const AddedToken &token = added[index];
          if (token.content.size() <= text.size() - p &&
              std::memcmp(text.data() + p, token.content.data(),
                          token.content.size()) == 0)
            {
              hit = &token;
              break;
            }
        }
      if (hit == nullptr)
        {
          ++p;
          continue;
        }
      size_t match_end = p + hit->content.size();
      bool spell_out = false;
      if (hit->special)
        {
          if (!allow_special)
            {
              spell_out = true;
            }
          else if (guard != nullptr)
            {
              size_t abs_begin = base + p;
              size_t abs_end = base + match_end;
              while (guard_cursor < guard->size() &&
                     (*guard)[guard_cursor].end <= abs_begin)
                {
                  ++guard_cursor;
                }
              spell_out = guard_cursor < guard->size() &&
                          (*guard)[guard_cursor].begin < abs_end;
            }
        }
      if (spell_out)
        {
          // The reference skips the whole match and resumes after it, so a
          // shorter token hiding inside a rejected one is not reconsidered.
          p = match_end;
          continue;
        }
      if (p > plain_start)
        {
          encode_split(text.substr(plain_start, p - plain_start), pass + 1,
                       base + plain_start, allow_special, guard, out);
        }
      out->push_back(hit->id);
      p = match_end;
      plain_start = match_end;
    }
  if (text.size() > plain_start)
    {
      encode_split(text.substr(plain_start), pass + 1, base + plain_start,
                   allow_special, guard, out);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Tokenizer::Tokenizer() : impl_(new Impl()) {}
Tokenizer::~Tokenizer() { delete impl_; }

bool Tokenizer::load(const std::string &path, std::string *error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    {
      *error = "cannot open " + path;
      return false;
    }
  std::string data;
  if (std::fseek(file, 0, SEEK_END) == 0)
    {
      long size = std::ftell(file);
      if (size > 0)
        data.reserve(static_cast<size_t>(size));
      std::rewind(file);
    }
  char buf[1 << 16];
  size_t got;
  while ((got = std::fread(buf, 1, sizeof(buf), file)) > 0)
    {
      data.append(buf, got);
    }
  bool read_error = std::ferror(file) != 0;
  std::fclose(file);
  if (read_error)
    {
      *error = "read error on " + path;
      return false;
    }
  return load_from_memory(data.data(), data.size(), error);
}

bool Tokenizer::load_from_memory(const char *data, size_t size,
                                 std::string *error)
{
  std::string scratch;
  if (error == nullptr)
    error = &scratch;
  std::unique_ptr<Impl> fresh(new Impl());

  JsonCursor cur(data, size);
  Json root = Json::object(); // everything except the two big tables
  Json model = Json::object();
  uint32_t vocab_ids = 0;
  bool have_vocab = false;
  bool have_merges = false;
  size_t deferred_merges = static_cast<size_t>(-1);

  if (!cur.consume('{'))
    {
      *error = "tokenizer.json is not a JSON object";
      return false;
    }
  std::string key;
  for (bool first = true;; first = false)
    {
      if (first && cur.consume('}'))
        break;
      if (!cur.parse_string(&key) || !cur.consume(':'))
        {
          *error = "malformed tokenizer.json: " + cur.error();
          return false;
        }
      if (key == "model")
        {
          if (!cur.consume('{'))
            {
              *error = "model is not an object";
              return false;
            }
          std::string model_key;
          for (bool model_first = true;; model_first = false)
            {
              if (model_first && cur.consume('}'))
                break;
              if (!cur.parse_string(&model_key) || !cur.consume(':'))
                {
                  *error = "malformed model object: " + cur.error();
                  return false;
                }
              if (model_key == "vocab")
                {
                  if (!fresh->load_vocab(&cur, &vocab_ids, error))
                    return false;
                  have_vocab = true;
                }
              else if (model_key == "merges")
                {
                  if (have_vocab)
                    {
                      if (!fresh->load_merges(&cur, error))
                        return false;
                      have_merges = true;
                    }
                  else
                    {
                      // Merges are resolved to ids through the vocab; if a
                      // writer ever emits them first, come back once the vocab
                      // is known.
                      cur.skip_ws();
                      deferred_merges = cur.offset();
                      if (!cur.skip_value())
                        {
                          *error = "malformed merges: " + cur.error();
                          return false;
                        }
                    }
                }
              else
                {
                  Json value;
                  if (!cur.parse_value(&value))
                    {
                      *error = "malformed model object: " + cur.error();
                      return false;
                    }
                  model.set(model_key, std::move(value));
                }
              if (cur.consume(','))
                continue;
              if (cur.consume('}'))
                break;
              *error = "malformed model object";
              return false;
            }
        }
      else
        {
          Json value;
          if (!cur.parse_value(&value))
            {
              *error = "malformed tokenizer.json: " + cur.error();
              return false;
            }
          root.set(key, std::move(value));
        }
      if (cur.consume(','))
        continue;
      if (cur.consume('}'))
        break;
      *error = "malformed tokenizer.json";
      return false;
    }
  if (have_vocab && !have_merges && deferred_merges != static_cast<size_t>(-1))
    {
      cur.seek(deferred_merges);
      if (!fresh->load_merges(&cur, error))
        return false;
      have_merges = true;
    }
  if (!have_vocab || !have_merges)
    {
      *error = "tokenizer.json lacks model.vocab or model.merges";
      return false;
    }
  if (!fresh->check_pipeline(root, model, error))
    return false;
  const Json *added = root.find("added_tokens");
  if (added != nullptr && !fresh->load_added(*added, error))
    return false;

  fresh->arena.shrink_to_fit();
  delete impl_;
  impl_ = fresh.release();
  return true;
}

std::vector<int32_t> Tokenizer::encode(std::string_view text,
                                       bool allow_special) const
{
  std::vector<int32_t> out;
  std::string clean;
  if (!utf8_is_valid(text))
    {
      utf8_sanitize(text, &clean);
      text = clean;
    }
  out.reserve(text.size() / 3 + 8);
  impl_->encode_split(text, 0, 0, allow_special, nullptr, &out);
  return out;
}

void Tokenizer::encode_guarded(std::string_view text,
                               const std::vector<ByteRange> &untrusted,
                               std::vector<int32_t> *out) const
{
  if (!utf8_is_valid(text))
    {
      // Sanitizing shifts offsets and would detach the guard ranges from the
      // text they protect.  Failing safe: nothing in a corrupt prompt is
      // allowed to become a control token.
      std::string clean;
      utf8_sanitize(text, &clean);
      impl_->encode_split(clean, 0, 0, false, nullptr, out);
      return;
    }
  out->reserve(out->size() + text.size() / 3 + 8);
  impl_->encode_split(text, 0, 0, true, &untrusted, out);
}

std::string_view Tokenizer::token_bytes(int32_t id) const
{
  if (id < 0)
    return {};
  return impl_->bytes(static_cast<uint32_t>(id));
}

bool Tokenizer::is_special(int32_t id) const
{
  return id >= 0 && static_cast<size_t>(id) < impl_->tok_flags.size() &&
         (impl_->tok_flags[static_cast<size_t>(id)] & kFlagSpecial) != 0;
}

int32_t Tokenizer::token_to_id(std::string_view token) const
{
  for (const AddedToken &added : impl_->added)
    {
      if (added.content == token)
        return added.id;
    }
  uint32_t id = impl_->lookup(token.data(), token.size());
  return id == kNone ? -1 : static_cast<int32_t>(id);
}

size_t Tokenizer::vocab_size() const { return impl_->tok_off.size(); }

size_t Tokenizer::memory_bytes() const
{
  size_t total = sizeof(Impl) + impl_->arena.capacity();
  total += impl_->tok_off.capacity() * sizeof(uint32_t) * 2;
  total += impl_->tok_flags.capacity();
  total += impl_->str_slots.capacity() * sizeof(uint32_t);
  total += impl_->merges.capacity() * sizeof(Merge);
  total += impl_->merge_slots.capacity() * sizeof(uint32_t);
  for (const AddedToken &added : impl_->added)
    {
      total += sizeof(AddedToken) + added.content.capacity();
    }
  return total;
}

std::string Tokenizer::decode(const std::vector<int32_t> &ids,
                              bool skip_special) const
{
  std::string raw;
  for (int32_t id : ids)
    {
      if (skip_special && is_special(id))
        continue;
      raw.append(token_bytes(id));
    }
  std::string out;
  if (utf8_sanitize(raw, &out))
    return raw;
  return out;
}

// ---------------------------------------------------------------------------
// StreamDecoder
// ---------------------------------------------------------------------------

namespace
{

// Length of the trailing bytes that are a still-completable prefix of a
// multi-byte character.  Anything that can no longer become valid is NOT
// held back: it will be U+FFFD regardless of what the next token brings.
size_t incomplete_tail(const std::string &s)
{
  size_t n = s.size();
  for (size_t back = 1; back <= 3 && back <= n; ++back)
    {
      unsigned char c = static_cast<unsigned char>(s[n - back]);
      if (is_continuation(c))
        continue;
      size_t need = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC2 ? 2 : 1));
      if (c > 0xF4 || need <= back)
        return 0;
      if (back >= 2)
        {
          unsigned char second = static_cast<unsigned char>(s[n - back + 1]);
          unsigned char lo = 0x80;
          unsigned char hi = 0xBF;
          if (c == 0xE0)
            lo = 0xA0;
          if (c == 0xED)
            hi = 0x9F;
          if (c == 0xF0)
            lo = 0x90;
          if (c == 0xF4)
            hi = 0x8F;
          if (second < lo || second > hi)
            return 0;
        }
      return back;
    }
  return 0;
}

} // namespace

std::string StreamDecoder::push(int32_t id)
{
  if (skip_special_ && tokenizer_.is_special(id))
    return std::string();
  pending_.append(tokenizer_.token_bytes(id));
  size_t hold = incomplete_tail(pending_);
  std::string out;
  utf8_sanitize(std::string_view(pending_).substr(0, pending_.size() - hold),
                &out);
  pending_.erase(0, pending_.size() - hold);
  return out;
}

std::string StreamDecoder::flush()
{
  std::string out;
  utf8_sanitize(pending_, &out);
  pending_.clear();
  return out;
}

} // namespace nyamp
