/****************************************************************************
 * tools/amp/chat/nyamp_json.cpp
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

#include "nyamp_json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nyamp_unicode.h"

namespace nyamp
{

namespace
{

// Requests come from a local agent, but the daemon must survive a hostile or
// corrupted one: recursion is bounded so nesting cannot exhaust the stack.
constexpr int kMaxDepth = 96;

void append_utf8(unsigned cp, std::string *out)
{
  if (cp < 0x80)
    {
      out->push_back(static_cast<char>(cp));
    }
  else if (cp < 0x800)
    {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  else if (cp < 0x10000)
    {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  else
    {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Json
// ---------------------------------------------------------------------------

Json Json::boolean(bool v)
{
  Json j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}

Json Json::number_lexeme(std::string lexeme)
{
  Json j;
  j.type_ = Type::Number;
  j.str_ = std::move(lexeme);
  return j;
}

Json Json::integer(long long v) { return number_lexeme(std::to_string(v)); }

Json Json::string(std::string v)
{
  Json j;
  j.type_ = Type::String;
  j.str_ = std::move(v);
  return j;
}

Json Json::array()
{
  Json j;
  j.type_ = Type::Array;
  return j;
}

Json Json::object()
{
  Json j;
  j.type_ = Type::Object;
  return j;
}

bool Json::number_is_integer() const
{
  return type_ == Type::Number &&
         str_.find_first_of(".eE") == std::string::npos;
}

double Json::number_as_double() const
{
  return type_ == Type::Number ? std::strtod(str_.c_str(), nullptr) : 0.0;
}

const Json *Json::find(std::string_view key) const
{
  if (type_ != Type::Object)
    return nullptr;
  for (const auto &member : members_)
    {
      if (member.first == key)
        return &member.second;
    }
  return nullptr;
}

void Json::set(std::string key, Json value)
{
  for (auto &member : members_)
    {
      if (member.first == key)
        {
          member.second = std::move(value);
          return;
        }
    }
  members_.emplace_back(std::move(key), std::move(value));
}

bool Json::truthy() const
{
  switch (type_)
    {
      case Type::Null:
        return false;
      case Type::Bool:
        return bool_;
      case Type::Number:
        return number_as_double() != 0.0;
      case Type::String:
        return !str_.empty();
      case Type::Array:
        return !items_.empty();
      case Type::Object:
        return !members_.empty();
    }
  return false;
}

bool Json::operator==(const Json &other) const
{
  if (type_ != other.type_)
    return false;
  switch (type_)
    {
      case Type::Null:
        return true;
      case Type::Bool:
        return bool_ == other.bool_;
      case Type::Number:
        // Lexeme first so huge integers compare exactly; fall back to value so
        // 1.0 == 1.00 still holds.
        return str_ == other.str_ ||
               (number_is_integer() == other.number_is_integer() &&
                number_as_double() == other.number_as_double());
      case Type::String:
        return str_ == other.str_;
      case Type::Array:
        return items_ == other.items_;
      case Type::Object:
        if (members_.size() != other.members_.size())
          return false;
        for (const auto &member : members_)
          {
            const Json *peer = other.find(member.first);
            if (peer == nullptr || *peer != member.second)
              return false;
          }
        return true;
    }
  return false;
}

// ---------------------------------------------------------------------------
// JsonCursor
// ---------------------------------------------------------------------------

bool JsonCursor::fail(const char *message)
{
  if (error_.empty())
    {
      error_ = message;
      error_ += " at byte ";
      error_ += std::to_string(offset());
    }
  return false;
}

void JsonCursor::skip_ws()
{
  while (p_ < end_ &&
         (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t'))
    {
      ++p_;
    }
}

char JsonCursor::peek()
{
  skip_ws();
  return p_ < end_ ? *p_ : '\0';
}

bool JsonCursor::consume(char expected)
{
  skip_ws();
  if (p_ < end_ && *p_ == expected)
    {
      ++p_;
      return true;
    }
  return false;
}

bool JsonCursor::at_end()
{
  skip_ws();
  return p_ == end_;
}

bool JsonCursor::parse_hex4(unsigned *out)
{
  if (end_ - p_ < 4)
    return fail("truncated \\u escape");
  unsigned v = 0;
  for (int i = 0; i < 4; ++i)
    {
      char c = p_[i];
      v <<= 4;
      if (c >= '0' && c <= '9')
        {
          v |= static_cast<unsigned>(c - '0');
        }
      else if (c >= 'a' && c <= 'f')
        {
          v |= static_cast<unsigned>(c - 'a' + 10);
        }
      else if (c >= 'A' && c <= 'F')
        {
          v |= static_cast<unsigned>(c - 'A' + 10);
        }
      else
        {
          return fail("bad \\u escape");
        }
    }
  p_ += 4;
  *out = v;
  return true;
}

bool JsonCursor::parse_string(std::string *out)
{
  skip_ws();
  if (p_ >= end_ || *p_ != '"')
    return fail("expected string");
  ++p_;
  out->clear();
  for (;;)
    {
      // Copy the run of ordinary bytes in one go; vocab strings are almost
      // entirely such runs and this loop dominates tokenizer load time.
      const char *run = p_;
      while (p_ < end_ && *p_ != '"' && *p_ != '\\' &&
             static_cast<unsigned char>(*p_) >= 0x20)
        {
          ++p_;
        }
      out->append(run, static_cast<size_t>(p_ - run));
      if (p_ >= end_)
        return fail("unterminated string");
      char c = *p_++;
      if (c == '"')
        return true;
      if (c != '\\')
        return fail("control character in string");
      if (p_ >= end_)
        return fail("unterminated escape");
      char e = *p_++;
      switch (e)
        {
          case '"':
          case '\\':
          case '/':
            out->push_back(e);
            break;
          case 'b':
            out->push_back('\b');
            break;
          case 'f':
            out->push_back('\f');
            break;
          case 'n':
            out->push_back('\n');
            break;
          case 'r':
            out->push_back('\r');
            break;
          case 't':
            out->push_back('\t');
            break;
          case 'u':
            {
              unsigned cp = 0;
              if (!parse_hex4(&cp))
                return false;
              if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 &&
                  p_[0] == '\\' && p_[1] == 'u')
                {
                  const char *save = p_;
                  p_ += 2;
                  unsigned low = 0;
                  if (!parse_hex4(&low))
                    return false;
                  if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                      cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                  else
                    {
                      p_ = save;
                    }
                }
              // A lone surrogate cannot be represented in UTF-8.  Substituting
              // U+FFFD keeps every string in the DOM valid UTF-8, which the
              // tokenizer and the response serializer both rely on.
              if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0xFFFD;
              append_utf8(cp, out);
              break;
            }
          default:
            return fail("bad escape");
        }
    }
}

bool JsonCursor::parse_number(std::string *lexeme)
{
  const char *start = p_;
  if (p_ < end_ && *p_ == '-')
    ++p_;
  if (p_ >= end_)
    return fail("bad number");
  if (*p_ == '0')
    {
      ++p_;
    }
  else if (*p_ >= '1' && *p_ <= '9')
    {
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
  else
    {
      return fail("bad number");
    }
  if (p_ < end_ && *p_ == '.')
    {
      ++p_;
      if (p_ >= end_ || *p_ < '0' || *p_ > '9')
        return fail("bad fraction");
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
  if (p_ < end_ && (*p_ == 'e' || *p_ == 'E'))
    {
      ++p_;
      if (p_ < end_ && (*p_ == '+' || *p_ == '-'))
        ++p_;
      if (p_ >= end_ || *p_ < '0' || *p_ > '9')
        return fail("bad exponent");
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
  lexeme->assign(start, static_cast<size_t>(p_ - start));
  return true;
}

bool JsonCursor::parse_value(Json *out, int depth)
{
  if (depth > kMaxDepth)
    return fail("nesting too deep");
  char c = peek();
  if (c == '{')
    {
      ++p_;
      *out = Json::object();
      if (consume('}'))
        return true;
      for (;;)
        {
          std::string key;
          if (!parse_string(&key))
            return false;
          if (!consume(':'))
            return fail("expected ':'");
          Json value;
          if (!parse_value(&value, depth + 1))
            return false;
          out->set(std::move(key), std::move(value));
          if (consume(','))
            continue;
          if (consume('}'))
            return true;
          return fail("expected ',' or '}'");
        }
    }
  if (c == '[')
    {
      ++p_;
      *out = Json::array();
      if (consume(']'))
        return true;
      for (;;)
        {
          Json value;
          if (!parse_value(&value, depth + 1))
            return false;
          out->push(std::move(value));
          if (consume(','))
            continue;
          if (consume(']'))
            return true;
          return fail("expected ',' or ']'");
        }
    }
  if (c == '"')
    {
      std::string s;
      if (!parse_string(&s))
        return false;
      *out = Json::string(std::move(s));
      return true;
    }
  if (c == '-' || (c >= '0' && c <= '9'))
    {
      std::string lexeme;
      if (!parse_number(&lexeme))
        return false;
      *out = Json::number_lexeme(std::move(lexeme));
      return true;
    }
  size_t left = static_cast<size_t>(end_ - p_);
  if (left >= 4 && std::memcmp(p_, "true", 4) == 0)
    {
      p_ += 4;
      *out = Json::boolean(true);
      return true;
    }
  if (left >= 5 && std::memcmp(p_, "false", 5) == 0)
    {
      p_ += 5;
      *out = Json::boolean(false);
      return true;
    }
  if (left >= 4 && std::memcmp(p_, "null", 4) == 0)
    {
      p_ += 4;
      *out = Json::null();
      return true;
    }
  return fail("unexpected token");
}

bool JsonCursor::skip_value(int depth)
{
  if (depth > kMaxDepth)
    return fail("nesting too deep");
  char c = peek();
  if (c == '{' || c == '[')
    {
      char close = c == '{' ? '}' : ']';
      ++p_;
      if (consume(close))
        return true;
      std::string scratch;
      for (;;)
        {
          if (c == '{')
            {
              if (!parse_string(&scratch))
                return false;
              if (!consume(':'))
                return fail("expected ':'");
            }
          if (!skip_value(depth + 1))
            return false;
          if (consume(','))
            continue;
          if (consume(close))
            return true;
          return fail("expected ',' or close");
        }
    }
  Json scratch;
  return parse_value(&scratch, depth);
}

bool json_parse(std::string_view text, Json *out, std::string *error)
{
  JsonCursor cursor(text.data(), text.size());
  if (!cursor.parse_value(out) ||
      (!cursor.at_end() && !cursor.fail("trailing characters")))
    {
      if (error != nullptr)
        *error = cursor.error();
      return false;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void json_append_quoted(std::string_view s, std::string *out)
{
  out->push_back('"');
  for (char ch : s)
    {
      unsigned char c = static_cast<unsigned char>(ch);
      switch (c)
        {
          case '"':
            *out += "\\\"";
            break;
          case '\\':
            *out += "\\\\";
            break;
          case '\n':
            *out += "\\n";
            break;
          case '\r':
            *out += "\\r";
            break;
          case '\t':
            *out += "\\t";
            break;
          case '\b':
            *out += "\\b";
            break;
          case '\f':
            *out += "\\f";
            break;
          default:
            if (c < 0x20)
              {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                *out += buf;
              }
            else
              {
                out->push_back(ch);
              }
        }
    }
  out->push_back('"');
}

std::string python_float_repr(double value)
{
  if (std::isnan(value))
    return "nan";
  if (std::isinf(value))
    return value < 0 ? "-inf" : "inf";

  // to_chars(scientific) without a precision yields the shortest digit
  // string that round-trips, which is exactly the digit set float.__repr__
  // uses; only the layout rules below are Python specific.
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), value,
                           std::chars_format::scientific);
  std::string sci(buf, static_cast<size_t>(res.ptr - buf));

  std::string out;
  size_t i = 0;
  if (sci[0] == '-')
    {
      out.push_back('-');
      i = 1;
    }
  size_t epos = sci.find('e', i);
  std::string digits;
  for (size_t k = i; k < epos; ++k)
    {
      if (sci[k] != '.')
        digits.push_back(sci[k]);
    }
  int exp10 = std::atoi(sci.c_str() + epos + 1);

  // float_repr_style 'r': fixed notation for -4 <= exp10 < 16.
  if (exp10 >= -4 && exp10 < 16)
    {
      if (exp10 < 0)
        {
          out += "0.";
          out.append(static_cast<size_t>(-exp10 - 1), '0');
          out += digits;
        }
      else if (static_cast<size_t>(exp10) + 1 >= digits.size())
        {
          out += digits;
          out.append(static_cast<size_t>(exp10) + 1 - digits.size(), '0');
          out += ".0";
        }
      else
        {
          out.append(digits, 0, static_cast<size_t>(exp10) + 1);
          out.push_back('.');
          out.append(digits, static_cast<size_t>(exp10) + 1,
                     std::string::npos);
        }
      return out;
    }
  out.push_back(digits[0]);
  if (digits.size() > 1)
    {
      out.push_back('.');
      out.append(digits, 1, std::string::npos);
    }
  char ebuf[16];
  std::snprintf(ebuf, sizeof(ebuf), "e%c%02d", exp10 < 0 ? '-' : '+',
                exp10 < 0 ? -exp10 : exp10);
  out += ebuf;
  return out;
}

namespace
{

// Python parses a JSON integer into an int and prints it canonically, so the
// only lexeme that changes is "-0".  Floats go through float.__repr__.
std::string python_number(const Json &value, bool json_flavour)
{
  if (value.number_is_integer())
    {
      return value.str() == "-0" ? "0" : value.str();
    }
  double d = value.number_as_double();
  if (std::isinf(d))
    {
      if (json_flavour)
        return d < 0 ? "-Infinity" : "Infinity";
      return d < 0 ? "-inf" : "inf";
    }
  return python_float_repr(d);
}

void dump(const Json &value, bool python_style, std::string *out)
{
  switch (value.type())
    {
      case Json::Type::Null:
        *out += "null";
        break;
      case Json::Type::Bool:
        *out += value.as_bool() ? "true" : "false";
        break;
      case Json::Type::Number:
        *out += python_style ? python_number(value, true) : value.str();
        break;
      case Json::Type::String:
        json_append_quoted(value.str(), out);
        break;
      case Json::Type::Array:
        {
          out->push_back('[');
          bool first = true;
          for (const Json &item : value.items())
            {
              if (!first)
                *out += python_style ? ", " : ",";
              first = false;
              dump(item, python_style, out);
            }
          out->push_back(']');
          break;
        }
      case Json::Type::Object:
        {
          out->push_back('{');
          bool first = true;
          for (const auto &member : value.members())
            {
              if (!first)
                *out += python_style ? ", " : ",";
              first = false;
              json_append_quoted(member.first, out);
              *out += python_style ? ": " : ":";
              dump(member.second, python_style, out);
            }
          out->push_back('}');
          break;
        }
    }
}

size_t utf8_decode_one(const unsigned char *s, size_t n, unsigned *cp)
{
  // Returns the sequence length, or 0 when s[0] does not start a valid one.
  unsigned char c = s[0];
  if (c < 0x80)
    {
      *cp = c;
      return 1;
    }
  size_t len;
  unsigned min;
  if (c >= 0xC2 && c <= 0xDF)
    {
      len = 2;
      min = 0x80;
      *cp = c & 0x1F;
    }
  else if (c >= 0xE0 && c <= 0xEF)
    {
      len = 3;
      min = 0x800;
      *cp = c & 0x0F;
    }
  else if (c >= 0xF0 && c <= 0xF4)
    {
      len = 4;
      min = 0x10000;
      *cp = c & 0x07;
    }
  else
    {
      return 0;
    }
  if (n < len)
    return 0;
  for (size_t i = 1; i < len; ++i)
    {
      if ((s[i] & 0xC0) != 0x80)
        return 0;
      *cp = (*cp << 6) | (s[i] & 0x3F);
    }
  if (*cp < min || *cp > 0x10FFFF || (*cp >= 0xD800 && *cp <= 0xDFFF))
    {
      return 0;
    }
  return len;
}

void python_repr_string(const std::string &s, std::string *out)
{
  // Same quote choice as CPython: prefer ', switch to " only when the text
  // holds a ' and no ".
  bool has_single = s.find('\'') != std::string::npos;
  bool has_double = s.find('"') != std::string::npos;
  char quote = (has_single && !has_double) ? '"' : '\'';
  out->push_back(quote);
  const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
  size_t n = s.size();
  size_t i = 0;
  char buf[16];
  while (i < n)
    {
      unsigned cp = 0;
      size_t len = utf8_decode_one(p + i, n - i, &cp);
      if (len == 0)
        {
          // Cannot happen for strings that came through the parser; keep the
          // byte so the defect stays visible instead of being silently
          // dropped.
          out->push_back(static_cast<char>(p[i]));
          ++i;
          continue;
        }
      if (cp == static_cast<unsigned>(quote) || cp == '\\')
        {
          out->push_back('\\');
          out->push_back(static_cast<char>(cp));
        }
      else if (cp == '\n')
        {
          *out += "\\n";
        }
      else if (cp == '\r')
        {
          *out += "\\r";
        }
      else if (cp == '\t')
        {
          *out += "\\t";
        }
      else if (cp < 0x20 || cp == 0x7F)
        {
          std::snprintf(buf, sizeof(buf), "\\x%02x", cp);
          *out += buf;
        }
      else if (cp < 0x7F)
        {
          out->push_back(static_cast<char>(cp));
        }
      else if (!unicode_is_python_printable(cp))
        {
          if (cp < 0x100)
            {
              std::snprintf(buf, sizeof(buf), "\\x%02x", cp);
            }
          else if (cp < 0x10000)
            {
              std::snprintf(buf, sizeof(buf), "\\u%04x", cp);
            }
          else
            {
              std::snprintf(buf, sizeof(buf), "\\U%08x", cp);
            }
          *out += buf;
        }
      else
        {
          out->append(s, i, len);
        }
      i += len;
    }
  out->push_back(quote);
}

void python_repr_into(const Json &value, std::string *out)
{
  switch (value.type())
    {
      case Json::Type::Null:
        *out += "None";
        break;
      case Json::Type::Bool:
        *out += value.as_bool() ? "True" : "False";
        break;
      case Json::Type::Number:
        *out += python_number(value, false);
        break;
      case Json::Type::String:
        python_repr_string(value.str(), out);
        break;
      case Json::Type::Array:
        {
          out->push_back('[');
          bool first = true;
          for (const Json &item : value.items())
            {
              if (!first)
                *out += ", ";
              first = false;
              python_repr_into(item, out);
            }
          out->push_back(']');
          break;
        }
      case Json::Type::Object:
        {
          out->push_back('{');
          bool first = true;
          for (const auto &member : value.members())
            {
              if (!first)
                *out += ", ";
              first = false;
              python_repr_string(member.first, out);
              *out += ": ";
              python_repr_into(member.second, out);
            }
          out->push_back('}');
          break;
        }
    }
}

} // namespace

std::string json_dump_python(const Json &value)
{
  std::string out;
  dump(value, true, &out);
  return out;
}

std::string json_dump_compact(const Json &value)
{
  std::string out;
  dump(value, false, &out);
  return out;
}

std::string python_repr(const Json &value)
{
  std::string out;
  python_repr_into(value, &out);
  return out;
}

std::string python_str(const Json &value)
{
  if (value.is_string())
    return value.str();
  return python_repr(value);
}

bool utf8_is_valid(std::string_view in)
{
  const unsigned char *p = reinterpret_cast<const unsigned char *>(in.data());
  size_t n = in.size();
  size_t i = 0;
  while (i < n)
    {
      if (p[i] < 0x80)
        {
          ++i;
          continue;
        }
      unsigned cp = 0;
      size_t len = utf8_decode_one(p + i, n - i, &cp);
      if (len == 0)
        return false;
      i += len;
    }
  return true;
}

bool utf8_sanitize(std::string_view in, std::string *out)
{
  out->clear();
  out->reserve(in.size());
  const unsigned char *p = reinterpret_cast<const unsigned char *>(in.data());
  size_t n = in.size();
  size_t i = 0;
  bool valid = true;
  while (i < n)
    {
      unsigned cp = 0;
      size_t len = utf8_decode_one(p + i, n - i, &cp);
      if (len != 0)
        {
          out->append(in.substr(i, len));
          i += len;
          continue;
        }
      valid = false;
      // "Maximal subpart" rule (Unicode 3.9, also what Rust implements): the
      // longest prefix that could still begin a valid sequence is replaced by
      // a single U+FFFD, so streaming and one-shot decoding agree.
      unsigned char c = p[i];
      size_t skip = 1;
      if (c >= 0xC2 && c <= 0xF4 && i + 1 < n)
        {
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
          size_t need = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
          if (p[i + 1] >= lo && p[i + 1] <= hi)
            {
              skip = 2;
              while (skip < need && i + skip < n &&
                     (p[i + skip] & 0xC0) == 0x80)
                {
                  ++skip;
                }
            }
        }
      *out += "\xEF\xBF\xBD";
      i += skip;
    }
  return valid;
}

} // namespace nyamp
