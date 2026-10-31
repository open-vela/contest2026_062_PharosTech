/****************************************************************************
 * tools/amp/chat/nyamp_chat_parse.cpp
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

#include "nyamp_chat_parse.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "nyamp_chat_template.h"

namespace nyamp
{

namespace
{

constexpr std::string_view kFunctionOpen = "<function";
constexpr std::string_view kFunctionClose = "</function>";
constexpr std::string_view kParamOpen = "<param";
constexpr std::string_view kParamClose = "</param>";
constexpr std::string_view kCdataOpen = "<![CDATA[";
constexpr std::string_view kCdataClose = "]]>";
constexpr std::string_view kThinkOpen = "<think>";
constexpr std::string_view kThinkClose = "</think>";

bool is_space(char c)
{
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

std::string_view trim(std::string_view s)
{
  while (!s.empty() && is_space(s.front()))
    s.remove_prefix(1);
  while (!s.empty() && is_space(s.back()))
    s.remove_suffix(1);
  return s;
}

std::string_view trim_newlines(std::string_view s)
{
  while (!s.empty() && (s.front() == '\n' || s.front() == '\r'))
    {
      s.remove_prefix(1);
    }
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    {
      s.remove_suffix(1);
    }
  return s;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Bounds-checked reader over the model text.  Every helper reports failure
// instead of advancing past the end, which is what makes truncated output a
// non-event.
struct Scanner
{
  std::string_view s;
  size_t p = 0;

  void skip_space()
  {
    while (p < s.size() && is_space(s[p]))
      ++p;
  }
  bool eat(std::string_view token)
  {
    if (!starts_with(s.substr(p), token))
      return false;
    p += token.size();
    return true;
  }
  // name = "value"  (the model always uses double quotes; single quotes are
  // accepted because they cost nothing and small models drift)
  bool name_attribute(std::string *value)
  {
    size_t before = p;
    skip_space();
    if (p == before)
      return false; // "<functionname" is not a tag
    if (!eat("name"))
      return false;
    skip_space();
    if (!eat("="))
      return false;
    skip_space();
    if (p >= s.size() || (s[p] != '"' && s[p] != '\''))
      return false;
    char quote = s[p++];
    size_t close = s.find(quote, p);
    if (close == std::string_view::npos)
      return false;
    value->assign(s.substr(p, close - p));
    p = close + 1;
    skip_space();
    return eat(">");
  }
};

// ---- Python literal fallback ----------------------------------------------
//
// The chat template prints nested arguments with Python repr(), so the
// history the model conditions on contains {'k': True}.  A model that copies
// that style must still yield valid JSON arguments.

struct PyLiteral
{
  std::string_view s;
  size_t p = 0;

  void skip_space()
  {
    while (p < s.size() && is_space(s[p]))
      ++p;
  }

  static void append_utf8(unsigned cp, std::string *out)
  {
    if (cp >= 0xD800 && cp <= 0xDFFF)
      cp = 0xFFFD;
    if (cp > 0x10FFFF)
      cp = 0xFFFD;
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

  bool hex(size_t digits, unsigned *out)
  {
    if (s.size() - p < digits)
      return false;
    unsigned v = 0;
    for (size_t i = 0; i < digits; ++i)
      {
        char c = s[p + i];
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
            return false;
          }
      }
    p += digits;
    *out = v;
    return true;
  }

  bool string(std::string *out)
  {
    char quote = s[p++];
    out->clear();
    while (p < s.size())
      {
        char c = s[p++];
        if (c == quote)
          return true;
        if (c != '\\')
          {
            out->push_back(c);
            continue;
          }
        if (p >= s.size())
          return false;
        char e = s[p++];
        unsigned cp = 0;
        switch (e)
          {
            case 'n':
              out->push_back('\n');
              break;
            case 'r':
              out->push_back('\r');
              break;
            case 't':
              out->push_back('\t');
              break;
            case 'b':
              out->push_back('\b');
              break;
            case 'f':
              out->push_back('\f');
              break;
            case 'x':
              if (!hex(2, &cp))
                return false;
              append_utf8(cp, out);
              break;
            case 'u':
              if (!hex(4, &cp))
                return false;
              append_utf8(cp, out);
              break;
            case 'U':
              if (!hex(8, &cp))
                return false;
              append_utf8(cp, out);
              break;
            case '\\':
            case '\'':
            case '"':
              out->push_back(e);
              break;
            default: // Python keeps unknown escapes verbatim
              out->push_back('\\');
              out->push_back(e);
          }
      }
    return false;
  }

  bool word(std::string_view w)
  {
    if (!starts_with(s.substr(p), w))
      return false;
    p += w.size();
    return true;
  }

  bool value(Json *out, int depth)
  {
    if (depth > 32)
      return false;
    skip_space();
    if (p >= s.size())
      return false;
    char c = s[p];
    if (c == '\'' || c == '"')
      {
        std::string text;
        if (!string(&text))
          return false;
        *out = Json::string(std::move(text));
        return true;
      }
    if (c == '{' || c == '[')
      {
        char close = c == '{' ? '}' : ']';
        *out = c == '{' ? Json::object() : Json::array();
        ++p;
        for (;;)
          {
            skip_space();
            if (p < s.size() && s[p] == close)
              {
                ++p;
                return true;
              }
            Json item;
            if (c == '{')
              {
                Json key;
                if (!value(&key, depth + 1) || !key.is_string())
                  return false;
                skip_space();
                if (p >= s.size() || s[p] != ':')
                  return false;
                ++p;
                if (!value(&item, depth + 1))
                  return false;
                out->set(key.str(), std::move(item));
              }
            else
              {
                if (!value(&item, depth + 1))
                  return false;
                out->push(std::move(item));
              }
            skip_space();
            if (p < s.size() && s[p] == ',')
              {
                ++p;
                continue;
              }
            if (p < s.size() && s[p] == close)
              {
                ++p;
                return true;
              }
            return false;
          }
      }
    if (word("True") || word("true"))
      {
        *out = Json::boolean(true);
        return true;
      }
    if (word("False") || word("false"))
      {
        *out = Json::boolean(false);
        return true;
      }
    if (word("None") || word("null"))
      {
        *out = Json::null();
        return true;
      }
    size_t start = p;
    while (p < s.size() &&
           ((s[p] >= '0' && s[p] <= '9') || s[p] == '-' || s[p] == '+' ||
            s[p] == '.' || s[p] == 'e' || s[p] == 'E'))
      {
        ++p;
      }
    Json number;
    if (p == start ||
        !json_parse(s.substr(start, p - start), &number, nullptr) ||
        !number.is_number())
      {
        return false;
      }
    *out = std::move(number);
    return true;
  }
};

bool parse_structured(std::string_view raw, Json *out)
{
  raw = trim(raw);
  if (json_parse(raw, out, nullptr))
    return true;
  PyLiteral literal{ raw };
  if (!literal.value(out, 0))
    return false;
  literal.skip_space();
  return literal.p == raw.size();
}

// ---- Schema driven typing
// ---------------------------------------------------

void collect_types(const Json &schema, std::vector<std::string> *types)
{
  if (const Json *type = schema.find("type"))
    {
      if (type->is_string())
        {
          types->push_back(type->str());
        }
      else if (type->is_array())
        {
          for (const Json &t : type->items())
            {
              if (t.is_string())
                types->push_back(t.str());
            }
        }
    }
  for (const char *key : { "anyOf", "oneOf" })
    {
      const Json *list = schema.find(key);
      if (list == nullptr || !list->is_array())
        continue;
      for (const Json &option : list->items())
        {
          if (option.is_object())
            collect_types(option, types);
        }
    }
}

bool convert(const std::string &type, std::string_view raw, Json *out)
{
  std::string_view t = trim(raw);
  if (type == "integer" || type == "number")
    {
      Json number;
      if (!json_parse(t, &number, nullptr) || !number.is_number())
        return false;
      if (type == "integer" && !number.number_is_integer())
        {
          // "3.0" for an integer parameter: accept the value, fix the
          // spelling.
          double d = number.number_as_double();
          if (!(std::fabs(d) < 9007199254740992.0) || d != std::floor(d))
            {
              return false;
            }
          number = Json::integer(static_cast<long long>(d));
        }
      *out = std::move(number);
      return true;
    }
  if (type == "boolean")
    {
      // The template prints Python booleans, so history teaches the model
      // "True"; JSON-minded models say "true".  Both are unambiguous.
      if (t == "true" || t == "True")
        {
          *out = Json::boolean(true);
          return true;
        }
      if (t == "false" || t == "False")
        {
          *out = Json::boolean(false);
          return true;
        }
      return false;
    }
  if (type == "null")
    {
      if (t == "null" || t == "None")
        {
          *out = Json::null();
          return true;
        }
      return false;
    }
  if (type == "object" || type == "array")
    {
      Json value;
      if (!parse_structured(t, &value))
        return false;
      if (type == "object" ? !value.is_object() : !value.is_array())
        return false;
      *out = std::move(value);
      return true;
    }
  return false;
}

Json typed_value(std::string_view raw, bool cdata, const Json *param_schema)
{
  std::vector<std::string> types;
  if (param_schema != nullptr && param_schema->is_object())
    {
      collect_types(*param_schema, &types);
    }
  // A CDATA wrapper is the model saying "this is literal text"; only an
  // explicit non-string schema type overrides that.
  for (const std::string &type : types)
    {
      Json value;
      if (type != "string" && convert(type, raw, &value))
        return value;
    }
  if (types.empty() && !cdata)
    {
      // Untyped parameter: only structures are recognized.  Guessing scalars
      // would turn a postcode or a version string into a number.
      std::string_view t = trim(raw);
      if (!t.empty() && (t.front() == '{' || t.front() == '['))
        {
          Json value;
          if (parse_structured(t, &value))
            return value;
        }
    }
  return Json::string(std::string(cdata ? raw : trim_newlines(raw)));
}

const Json *find_parameters(const Json &tools, const std::string &name)
{
  for (const Json &tool : tools.items())
    {
      const Json *function = tool.find("function");
      if (function == nullptr)
        continue;
      const Json *tool_name = function->find("name");
      if (tool_name == nullptr || !tool_name->is_string() ||
          tool_name->str() != name)
        {
          continue;
        }
      const Json *parameters = function->find("parameters");
      return parameters != nullptr ? parameters->find("properties") : nullptr;
    }
  return nullptr;
}

// Parses one call starting right after "<function".  On failure the scanner
// position is meaningless and the caller falls back to plain text.
bool parse_call(Scanner *sc, const Json &tools, ToolCall *call)
{
  if (!sc->name_attribute(&call->name) || call->name.empty())
    return false;
  const Json *properties = find_parameters(tools, call->name);
  Json arguments = Json::object();
  for (;;)
    {
      sc->skip_space();
      if (sc->eat(kFunctionClose))
        break;
      if (!sc->eat(kParamOpen))
        return false;
      std::string param;
      if (!sc->name_attribute(&param))
        return false;

      std::string_view rest = sc->s.substr(sc->p);
      size_t lead = 0;
      while (lead < rest.size() && is_space(rest[lead]))
        ++lead;
      std::string_view value;
      bool cdata = false;
      if (starts_with(rest.substr(lead), kCdataOpen))
        {
          // The payload may itself contain "</param>" or even "]]>", so the
          // terminator is the first "]]>" that is directly followed by the
          // closing tag.
          size_t begin = lead + kCdataOpen.size();
          size_t search = begin;
          for (;;)
            {
              size_t close = rest.find(kCdataClose, search);
              if (close == std::string_view::npos)
                return false;
              size_t after = close + kCdataClose.size();
              while (after < rest.size() && is_space(rest[after]))
                ++after;
              if (starts_with(rest.substr(after), kParamClose))
                {
                  value = rest.substr(begin, close - begin);
                  sc->p += after + kParamClose.size();
                  cdata = true;
                  break;
                }
              search = close + 1;
            }
        }
      else
        {
          size_t close = rest.find(kParamClose);
          if (close == std::string_view::npos)
            return false;
          value = rest.substr(0, close);
          // An opening tag inside a plain value means the real terminator was
          // never generated and this "</param>" belongs to a later call.
          // Swallowing that call as an argument would execute the wrong thing.
          if (value.find(kFunctionOpen) != std::string_view::npos ||
              value.find(kParamOpen) != std::string_view::npos)
            {
              return false;
            }
          sc->p += close + kParamClose.size();
        }
      const Json *schema =
          properties != nullptr ? properties->find(param) : nullptr;
      arguments.set(std::move(param), typed_value(value, cdata, schema));
    }
  call->arguments = json_dump_compact(arguments);
  return true;
}

std::string make_call_id(const std::string &seed, size_t index,
                         const ToolCall &call)
{
  // OpenAI ids are opaque; clients only echo them back in the tool message.
  // A hash keeps them unique per completion without needing an entropy
  // source in a static daemon, and stable for golden tests.
  uint64_t h1 = 1469598103934665603ull;
  uint64_t h2 = 0x9E3779B97F4A7C15ull;
  auto mix = [&](std::string_view part) {
    for (char c : part)
      {
        h1 = (h1 ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        h2 = (h2 + static_cast<unsigned char>(c)) * 0xff51afd7ed558ccdull;
        h2 ^= h2 >> 29;
      }
    h1 = (h1 ^ 0xFF) * 1099511628211ull;
  };
  mix(seed);
  mix(std::to_string(index));
  mix(call.name);
  mix(call.arguments);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "call_%016llx%08llx",
                static_cast<unsigned long long>(h1),
                static_cast<unsigned long long>(h2 & 0xFFFFFFFFull));
  return buf;
}

} // namespace

ParsedCompletion parse_completion(std::string_view text, const Json *tools_in,
                                  const ParseOptions &options)
{
  ParsedCompletion result;
  Json tools =
      tools_in != nullptr ? normalize_tools(*tools_in) : Json::array();

  // Anything after an end-of-turn marker is not part of this message.  The
  // daemon normally stops on the token id already; this covers a runtime
  // that delivers the stop token's text, or fails to stop at all.
  for (std::string_view stop :
       { std::string_view("<|im_end|>"), std::string_view("</s>"),
         std::string_view("<|im_start|>") })
    {
      size_t at = text.find(stop);
      if (at != std::string_view::npos)
        text = text.substr(0, at);
    }

  // A think block is only honoured where the template puts one: at the very
  // start of the turn.  "</think>" appearing later is ordinary text (the
  // user may well be asking about these tags).
  bool opens_with_think = starts_with(trim(text), kThinkOpen);
  if (opens_with_think || options.prompt_opened_think)
    {
      std::string_view body = trim(text);
      if (opens_with_think)
        body.remove_prefix(kThinkOpen.size());
      size_t think_close = body.find(kThinkClose);
      if (think_close != std::string_view::npos)
        {
          result.reasoning_content =
              std::string(trim(body.substr(0, think_close)));
          text = body.substr(think_close + kThinkClose.size());
          while (!text.empty() && text.front() == '\n')
            text.remove_prefix(1);
        }
      else
        {
          // Thinking was cut off by the length limit: there is no answer yet.
          result.reasoning_content = std::string(trim(body));
          text = std::string_view();
        }
    }

  std::string content;
  size_t copied = 0;
  size_t search = 0;
  while (search < text.size())
    {
      size_t open = text.find(kFunctionOpen, search);
      if (open == std::string_view::npos)
        break;
      Scanner sc{ text, open + kFunctionOpen.size() };
      ToolCall call;
      if (parse_call(&sc, tools, &call))
        {
          content.append(text.substr(copied, open - copied));
          call.id =
              make_call_id(options.id_seed, result.tool_calls.size(), call);
          result.tool_calls.push_back(std::move(call));
          copied = sc.p;
          search = sc.p;
        }
      else
        {
          // Leave the broken fragment in the content and keep looking: a later
          // complete call is still worth executing.
          search = open + kFunctionOpen.size();
        }
    }
  content.append(text.substr(copied));

  if (!result.tool_calls.empty())
    {
      // The template joins content and calls with "\n"; those separators are
      // framing, not something the user should read.
      result.content = std::string(trim(content));
      result.finish_reason = "tool_calls";
    }
  else
    {
      result.content = std::move(content);
      result.finish_reason = options.hit_length_limit ? "length" : "stop";
    }
  return result;
}

std::string completion_to_json(const ParsedCompletion &completion,
                               const ResponseMeta &meta)
{
  auto clean = [](const std::string &s) {
    std::string out;
    return utf8_sanitize(s, &out) ? s : out;
  };

  Json message = Json::object();
  message.set("role", Json::string("assistant"));
  message.set("content", completion.content.empty()
                             ? Json::null()
                             : Json::string(clean(completion.content)));
  if (!completion.reasoning_content.empty())
    {
      message.set("reasoning_content",
                  Json::string(clean(completion.reasoning_content)));
    }
  if (!completion.tool_calls.empty())
    {
      Json calls = Json::array();
      for (const ToolCall &call : completion.tool_calls)
        {
          Json function = Json::object();
          function.set("name", Json::string(clean(call.name)));
          function.set("arguments", Json::string(clean(call.arguments)));
          Json entry = Json::object();
          entry.set("id", Json::string(call.id));
          entry.set("type", Json::string("function"));
          entry.set("function", std::move(function));
          calls.push(std::move(entry));
        }
      message.set("tool_calls", std::move(calls));
    }

  Json choice = Json::object();
  choice.set("index", Json::integer(0));
  choice.set("message", std::move(message));
  choice.set("finish_reason", Json::string(completion.finish_reason));
  Json choices = Json::array();
  choices.push(std::move(choice));

  Json usage = Json::object();
  usage.set("prompt_tokens", Json::integer(meta.prompt_tokens));
  usage.set("completion_tokens", Json::integer(meta.completion_tokens));
  usage.set("total_tokens",
            Json::integer(meta.prompt_tokens + meta.completion_tokens));

  Json root = Json::object();
  root.set("id", Json::string(clean(meta.id)));
  root.set("object", Json::string("chat.completion"));
  root.set("created", Json::integer(meta.created));
  root.set("model", Json::string(clean(meta.model)));
  root.set("choices", std::move(choices));
  root.set("usage", std::move(usage));
  return json_dump_compact(root);
}

} // namespace nyamp
