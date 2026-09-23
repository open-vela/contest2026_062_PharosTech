/****************************************************************************
 * tools/amp/chat/nyamp_json.h
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

// Minimal JSON reader/writer for the chat front end.
//
// Why hand-written instead of a vendored library: two requirements rule the
// usual single-header parsers out.  (1) The chat template dumps the tool list
// with Python's json.dumps, so object key order and the int/float distinction
// of every number must survive a parse/serialize round trip bit-exactly.
// (2) tokenizer.json is ~10 MB with 130k vocab entries and 130k merges; a DOM
// of that costs tens of MB on a 4 GB shared board, so the tokenizer walks it
// with the pull cursor below and never materializes it.

#ifndef NYAMP_JSON_H
#define NYAMP_JSON_H

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nyamp
{

class Json
{
public:
  enum class Type
  {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
  };

  Json() = default;
  static Json null() { return Json(); }
  static Json boolean(bool v);
  static Json number_lexeme(std::string lexeme); // must be valid JSON number
  static Json integer(long long v);
  static Json string(std::string v);
  static Json array();
  static Json object();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool() const { return bool_; }
  // String value, or the original number lexeme for numbers.  Keeping the
  // lexeme (not a double) is what lets "1.0" and "1" stay distinguishable.
  const std::string &str() const { return str_; }
  // True when the number lexeme has no fraction/exponent (a Python int).
  bool number_is_integer() const;
  double number_as_double() const;

  const std::vector<Json> &items() const { return items_; }
  std::vector<Json> &items() { return items_; }
  const std::vector<std::pair<std::string, Json> > &members() const
  {
    return members_;
  }
  std::vector<std::pair<std::string, Json> > &members() { return members_; }

  // Object lookup; nullptr when absent or when this is not an object.
  const Json *find(std::string_view key) const;
  // Insert or overwrite in place.  Overwriting keeps the first position,
  // which is what a Python dict does with duplicate JSON keys.
  void set(std::string key, Json value);
  void push(Json value) { items_.push_back(std::move(value)); }

  // Python truthiness, used by the template's `{% if x %}` tests.
  bool truthy() const;

  bool operator==(const Json &other) const;
  bool operator!=(const Json &other) const { return !(*this == other); }

private:
  Type type_ = Type::Null;
  bool bool_ = false;
  std::string str_;
  std::vector<Json> items_;
  std::vector<std::pair<std::string, Json> > members_;
};

// Pull cursor over a JSON text.  All methods return false on malformed input
// and leave a message in error(); none of them throw or read out of bounds.
class JsonCursor
{
public:
  JsonCursor(const char *data, size_t size)
      : p_(data), end_(data + size), begin_(data)
  {
  }

  void skip_ws();
  // Next significant character without consuming it, 0 at end of input.
  char peek();
  bool consume(char expected);
  bool parse_string(std::string *out);
  bool parse_value(Json *out, int depth = 0);
  bool skip_value(int depth = 0);
  bool at_end();

  size_t offset() const { return static_cast<size_t>(p_ - begin_); }
  void seek(size_t offset) { p_ = begin_ + offset; }
  const std::string &error() const { return error_; }
  bool fail(const char *message);

private:
  bool parse_number(std::string *lexeme);
  bool parse_hex4(unsigned *out);

  const char *p_;
  const char *end_;
  const char *begin_;
  std::string error_;
};

// Whole-document parse; trailing garbage is an error.
bool json_parse(std::string_view text, Json *out, std::string *error);

// Byte-exact equivalent of Python json.dumps(v, ensure_ascii=False): ", " and
// ": " separators, floats through float.__repr__, ints canonical.
std::string json_dump_python(const Json &value);
// Compact form for responses we author ourselves.
std::string json_dump_compact(const Json &value);
// Appends the JSON string literal for `s` (quotes included).
void json_append_quoted(std::string_view s, std::string *out);

// Python repr() of a parsed JSON value, i.e. what Jinja prints for a
// non-string `{{ value }}`: True/False/None, float repr, single-quoted
// strings inside lists and dicts.
std::string python_repr(const Json &value);
// Python str(): same as repr except a top-level string is printed raw.
std::string python_str(const Json &value);
// repr(float(lexeme)) - shortest round-trip digits in Python's layout.
std::string python_float_repr(double value);

// Rust String::from_utf8_lossy equivalent: every maximal invalid subpart
// becomes one U+FFFD.  Returns true when the input was already valid.
bool utf8_sanitize(std::string_view in, std::string *out);
bool utf8_is_valid(std::string_view in);

} // namespace nyamp

#endif // NYAMP_JSON_H
