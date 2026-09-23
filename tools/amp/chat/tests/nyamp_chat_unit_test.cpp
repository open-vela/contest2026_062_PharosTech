/****************************************************************************
 * tools/amp/chat/tests/nyamp_chat_unit_test.cpp
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

// Asset-free tests: everything here runs without tokenizer.json, so it is
// the part of the suite that CI on a bare checkout can always execute.
// Focus: the output parser's tolerance for what a small model really emits,
// and the Python-compatibility helpers the template depends on.

#include <cstdint>
#include <cstdio>
#include <string>

#include "nyamp_chat_parse.h"
#include "nyamp_chat_template.h"
#include "nyamp_json.h"
#include "nyamp_unicode.h"

namespace
{

using nyamp::Json;

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char *expr, int line)
{
  ++g_checks;
  if (ok)
    return;
  ++g_failures;
  std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
}

void check_eq(const std::string &got, const std::string &want, int line)
{
  ++g_checks;
  if (got == want)
    return;
  ++g_failures;
  std::fprintf(stderr, "FAIL line %d:\n  want %s\n  got  %s\n", line,
               want.c_str(), got.c_str());
}

#define CHECK(expr)         check((expr), #expr, __LINE__)
#define CHECK_EQ(got, want) check_eq((got), (want), __LINE__)

Json parse(const char *text)
{
  Json value;
  std::string error;
  if (!nyamp::json_parse(text, &value, &error))
    {
      std::fprintf(stderr, "test JSON is broken: %s\n", error.c_str());
      ++g_failures;
    }
  return value;
}

// JSON fixtures live here, outside any macro argument and with formatting
// switched off: the repository's clang-format profile predates raw string
// literals and rewrites their contents.
// clang-format off
const char *kTools = R"([
  {"type":"function","function":{"name":"get_weather","parameters":{"type":"object",
    "properties":{"city":{"type":"string"},"days":{"type":"integer"},
                  "metric":{"type":"boolean"},"ratio":{"type":"number"},
                  "opts":{"type":"object"},"maybe":{"anyOf":[{"type":"integer"},{"type":"null"}]}}}}},
  {"name":"nyabula_music","description":"d","input_schema":{"type":"object",
    "properties":{"topic":{"type":"string"},"arguments":{"type":"object"}}}}
])";
const char *kNumbersJson = R"({"a":[1,2.50,-0,1E3,true,null],"b":"é\"\n\u0001\u007f"})";
const char *kNumbersDump = "{\"a\": [1, 2.5, 0, 1000.0, true, null], \"b\": \"é\\\"\\n\\u0001\x7f\"}";
const char *kQuotesJson = R"({"a":[1,2.0,"x'y","p\"q","both'\""],"b":{"t":true,"n":null}})";
const char *kQuotesRepr = "{'a': [1, 2.0, \"x'y\", 'p\"q', 'both\\'\"'], 'b': {'t': True, 'n': None}}";
const char *kEscapesJson = R"(["\u0001\u00a0\u200b\\ é🐱\n"])";
const char *kEscapesRepr = "['\\x01\\xa0\\u200b\\\\ é🐱\\n']";
const char *kDuplicateJson = R"({"a":1,"b":2,"a":3})";
const char *kPartsRequest = R"({"messages":[{"role":"user","content":[{"type":"text","text":"a"},
  {"type":"image_url","image_url":{}},{"type":"text","text":"b"}]}]})";
// clang-format on

void test_python_numbers()
{
  CHECK_EQ(nyamp::python_float_repr(0.1), "0.1");
  CHECK_EQ(nyamp::python_float_repr(1.0), "1.0");
  CHECK_EQ(nyamp::python_float_repr(100.0), "100.0");
  CHECK_EQ(nyamp::python_float_repr(1e15), "1000000000000000.0");
  CHECK_EQ(nyamp::python_float_repr(1e16), "1e+16");
  CHECK_EQ(nyamp::python_float_repr(1.5e-5), "1.5e-05");
  CHECK_EQ(nyamp::python_float_repr(0.0001), "0.0001");
  CHECK_EQ(nyamp::python_float_repr(0.00001), "1e-05");
  CHECK_EQ(nyamp::python_float_repr(-0.0), "-0.0");
  CHECK_EQ(nyamp::python_float_repr(123456789.125), "123456789.125");
  CHECK_EQ(nyamp::python_float_repr(5e-324), "5e-324");
  CHECK_EQ(nyamp::python_float_repr(0.30000000000000004),
           "0.30000000000000004");
  CHECK_EQ(nyamp::json_dump_python(parse(kNumbersJson)), kNumbersDump);
  CHECK_EQ(nyamp::python_repr(parse(kQuotesJson)), kQuotesRepr);
  CHECK_EQ(nyamp::python_repr(parse(kEscapesJson)), kEscapesRepr);
  // Duplicate keys: last value, first position - what a Python dict does.
  CHECK_EQ(nyamp::json_dump_compact(parse(kDuplicateJson)),
           "{\"a\":3,\"b\":2}");
}

void test_json_rejects_garbage()
{
  Json value;
  for (const char *bad :
       { "", "{", "[1,", "{\"a\"}", "01", "1.", "\"\\x\"", "\"unterminated",
         "nul", "[1] x", "{\"a\":}", "\"\t\"" })
    {
      CHECK(!nyamp::json_parse(bad, &value, nullptr));
    }
  std::string deep(5000, '[');
  CHECK(!nyamp::json_parse(deep, &value, nullptr)); // bounded recursion
  CHECK(nyamp::json_parse("\"\\ud83d\\ude00 \\ud800\"", &value, nullptr));
  CHECK_EQ(value.str(), "\xF0\x9F\x98\x80 \xEF\xBF\xBD");
}

void test_utf8()
{
  std::string out;
  CHECK(nyamp::utf8_sanitize("ok \xE4\xBD\xA0", &out));
  CHECK(!nyamp::utf8_sanitize("a\xE4\xBD", &out));
  CHECK_EQ(out, "a\xEF\xBF\xBD"); // one U+FFFD per maximal subpart
  CHECK(!nyamp::utf8_sanitize("\xC0\xAF\xED\xA0\x80"
                              "b\xF0\x9F\x98",
                              &out));
  CHECK_EQ(out, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
                "b\xEF\xBF\xBD");
  // The CJK fast path must agree with the generated table at its edges.
  CHECK(nyamp::unicode_classify(0x4E00) == nyamp::kClassLetter);
  CHECK(nyamp::unicode_classify(0x9FFF) == nyamp::kClassLetter);
  CHECK(nyamp::unicode_classify(0x3000) == nyamp::kClassSpace);
  CHECK(nyamp::unicode_classify(0x00BD) == nyamp::kClassNumber);
  CHECK(nyamp::unicode_classify(0x1F431) == 0);
  CHECK(nyamp::unicode_classify('\n') ==
        (nyamp::kClassSpace | nyamp::kClassNewline));
}

void test_parse_well_formed()
{
  Json tools = parse(kTools);
  nyamp::ParseOptions options;
  options.id_seed = "req-1";

  // The exact shape seen on the board for the weather smoke prompt.
  auto r = nyamp::parse_completion(
      "<function name=\"get_weather\"><param "
      "name=\"city\">Dalian</param></function><|im_end|>",
      &tools, options);
  CHECK_EQ(r.finish_reason, "tool_calls");
  CHECK(r.tool_calls.size() == 1);
  CHECK_EQ(r.tool_calls[0].name, "get_weather");
  CHECK_EQ(r.tool_calls[0].arguments, "{\"city\":\"Dalian\"}");
  CHECK_EQ(r.content, "");
  CHECK(r.tool_calls[0].id.rfind("call_", 0) == 0 &&
        r.tool_calls[0].id.size() == 29);

  r = nyamp::parse_completion(
      "我查一下喵\n<function name=\"get_weather\">\n  <param "
      "name=\"city\">\n大连\n</param>\n"
      "  <param name=\"days\"> 3 </param><param name=\"metric\">True</param>"
      "<param name=\"ratio\">0.5</param><param name=\"opts\">{'a': [1, "
      "True, None, 'x']}</param>"
      "<param name=\"maybe\">None</param>\n</function>\n"
      "<function name='nyabula_music'><param "
      "name=\"topic\">music.play</param>"
      "<param name=\"arguments\">{\"name\": "
      "\"晴天.mp3\"}</param></function>",
      &tools, options);
  CHECK(r.tool_calls.size() == 2);
  CHECK_EQ(r.content, "我查一下喵");
  CHECK_EQ(r.tool_calls[0].arguments,
           "{\"city\":\"大连\",\"days\":3,\"metric\":true,\"ratio\":0.5,"
           "\"opts\":{\"a\":[1,true,null,\"x\"]},\"maybe\":null}");
  CHECK_EQ(r.tool_calls[1].arguments,
           "{\"topic\":\"music.play\",\"arguments\":{\"name\":\"晴天.mp3\"}}");
  CHECK(r.tool_calls[0].id != r.tool_calls[1].id);

  // Values that do not fit the declared type stay strings instead of being
  // coerced or dropped: the agent's validator gives the better error.
  r = nyamp::parse_completion(
      "<function name=\"get_weather\"><param name=\"days\">three</param>"
      "<param name=\"metric\">yes</param><param "
      "name=\"opts\">{broken</param>"
      "<param name=\"days2\">7</param></function>",
      &tools, options);
  CHECK_EQ(r.tool_calls[0].arguments,
           "{\"days\":\"three\",\"metric\":\"yes\",\"opts\":\"{broken\","
           "\"days2\":\"7\"}");

  r = nyamp::parse_completion("<function name=\"get_weather\"><param "
                              "name=\"days\">3.0</param></function>",
                              &tools, options);
  CHECK_EQ(r.tool_calls[0].arguments, "{\"days\":3}");

  // CDATA is literal, including a fake terminator inside it.
  r = nyamp::parse_completion(
      "<function name=\"w\"><param name=\"c\"><![CDATA[\n<p>a</param> ]]> "
      "b\n]]></param></function>",
      &tools, options);
  CHECK_EQ(r.tool_calls[0].arguments, "{\"c\":\"\\n<p>a</param> ]]> b\\n\"}");

  // No tool list at all: strings stay strings, structures are recognized.
  r = nyamp::parse_completion(
      "<function name=\"x\"><param name=\"n\">42</param><param "
      "name=\"o\">[1, 2]</param></function>",
      nullptr, options);
  CHECK_EQ(r.tool_calls[0].arguments, "{\"n\":\"42\",\"o\":[1,2]}");
}

void test_parse_malformed()
{
  Json tools = parse(kTools);
  nyamp::ParseOptions options;
  options.hit_length_limit = true;

  const char *broken[] = {
    "<function",
    "<function name=",
    "<function name=\"get_weather",
    "<function name=\"get_weather\">",
    "<function name=\"get_weather\"><param",
    "<function name=\"get_weather\"><param name=\"city\">Dal",
    "<function name=\"get_weather\"><param name=\"city\">Dalian</param>",
    "<function name=\"get_weather\"><param "
    "name=\"city\"><![CDATA[Dalian</param></function>",
    "<function name=\"get_weather\"><param name=\"city\">Dalian</function>",
    "<function name=\"get_weather\">stray text</function>",
    "<function name=\"\"></function>",
    "<functionname=\"x\"></function>",
    "<function ... </function>",
    "</function></param>]]>",
  };
  for (const char *text : broken)
    {
      auto r = nyamp::parse_completion(text, &tools, options);
      CHECK(r.tool_calls.empty());
      CHECK_EQ(r.content, text); // nothing lost, nothing executed
      CHECK_EQ(r.finish_reason, "length");
    }

  // A broken call followed by a complete one: the complete one still runs,
  // the fragment is shown.
  auto r = nyamp::parse_completion(
      "<function name=\"a\"><param name=\"p\">x <function "
      "name=\"get_weather\">"
      "<param name=\"city\">Dalian</param></function>",
      &tools, options);
  CHECK(r.tool_calls.size() == 1);
  CHECK_EQ(r.tool_calls[0].name, "get_weather");
  CHECK_EQ(r.content, "<function name=\"a\"><param name=\"p\">x");

  // Every truncation point of a valid completion must be survivable.
  std::string full =
      "ok\n<function name=\"get_weather\"><param "
      "name=\"city\"><![CDATA[a<b]]></param>"
      "<param name=\"days\">3</param></function>\n<function "
      "name=\"nyabula_music\">"
      "<param name=\"arguments\">{'k': 'v'}</param></function><|im_end|>";
  for (size_t cut = 0; cut <= full.size(); ++cut)
    {
      auto part = nyamp::parse_completion(
          std::string_view(full).substr(0, cut), &tools, options);
      CHECK(part.tool_calls.size() <= 2);
      nyamp::ResponseMeta meta;
      Json response;
      CHECK(nyamp::json_parse(nyamp::completion_to_json(part, meta), &response,
                              nullptr));
    }
  CHECK(nyamp::parse_completion(full, &tools, options).tool_calls.size() == 2);

  // Deterministic mutation fuzz.  The assertions are weak on purpose; the
  // value is in running this under ASan/UBSan (see README) where any
  // out-of-bounds read in the scanner becomes a hard failure.
  const char *splices[] = { "<function", "</function>", "<param",
                            "</param>",  "<![CDATA[",   "]]>",
                            " name=\"",  "\">",         "<think>",
                            "</think>",  "<|im_end|>",  "{'a': [1, ",
                            "\xF0\x9F",  "\n",          "'",
                            "\"",        "\\" };
  uint32_t state = 20260919u;
  auto next = [&state]() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  };
  for (int round = 0; round < 20000; ++round)
    {
      std::string mutated = full;
      for (uint32_t edits = 1 + next() % 4; edits > 0; --edits)
        {
          size_t at = next() % (mutated.size() + 1);
          switch (next() % 3)
            {
              case 0:
                mutated.insert(
                    at,
                    splices[next() % (sizeof(splices) / sizeof(splices[0]))]);
                break;
              case 1:
                mutated.erase(at, next() % 12);
                break;
              default:
                mutated.resize(at);
            }
        }
      auto part = nyamp::parse_completion(mutated, &tools, options);
      nyamp::ResponseMeta meta;
      std::string response = nyamp::completion_to_json(part, meta);
      if (!nyamp::utf8_is_valid(response) || part.tool_calls.size() > 8)
        {
          CHECK(false && "mutation produced an invalid response");
        }
    }
  ++g_checks;
}

void test_parse_text_and_think()
{
  nyamp::ParseOptions options;
  auto r = nyamp::parse_completion("你好喵～<|im_end|>", nullptr, options);
  CHECK_EQ(r.content, "你好喵～");
  CHECK_EQ(r.finish_reason, "stop");
  CHECK(r.tool_calls.empty());

  r = nyamp::parse_completion("<think>\nplan\n</think>\n\nanswer</s>trailing",
                              nullptr, options);
  CHECK_EQ(r.reasoning_content, "plan");
  CHECK_EQ(r.content, "answer");

  options.prompt_opened_think = true;
  r = nyamp::parse_completion("plan\n</think>\n\nanswer", nullptr, options);
  CHECK_EQ(r.reasoning_content, "plan");
  CHECK_EQ(r.content, "answer");
  r = nyamp::parse_completion("still thinking", nullptr, options);
  CHECK_EQ(r.reasoning_content, "still thinking");
  CHECK_EQ(r.content, "");

  options.prompt_opened_think = false;
  r = nyamp::parse_completion("the tag </think> is just text here", nullptr,
                              options);
  CHECK_EQ(r.content, "the tag </think> is just text here");
  CHECK_EQ(r.reasoning_content, "");
}

void test_response_json()
{
  nyamp::ParseOptions options;
  options.id_seed = "seed";
  Json tools = parse(kTools);
  auto r = nyamp::parse_completion("<function name=\"get_weather\"><param "
                                   "name=\"city\">Dalian</param></function>",
                                   &tools, options);
  nyamp::ResponseMeta meta;
  meta.id = "chatcmpl-1";
  meta.model = "minicpm5-1b";
  meta.created = 1789000000;
  meta.prompt_tokens = 321;
  meta.completion_tokens = 17;
  Json response = parse(nyamp::completion_to_json(r, meta).c_str());
  CHECK_EQ(response.find("object")->str(), "chat.completion");
  const Json &choice = response.find("choices")->items()[0];
  CHECK_EQ(choice.find("finish_reason")->str(), "tool_calls");
  const Json &message = *choice.find("message");
  CHECK(message.find("content")->is_null());
  const Json &call = message.find("tool_calls")->items()[0];
  CHECK_EQ(call.find("type")->str(), "function");
  CHECK_EQ(call.find("function")->find("arguments")->str(),
           "{\"city\":\"Dalian\"}");
  CHECK_EQ(response.find("usage")->find("total_tokens")->str(), "338");

  // Broken UTF-8 from a cut-off stream must not produce an invalid response.
  nyamp::ParsedCompletion raw;
  raw.content = "caf\xC3";
  raw.finish_reason = "length";
  std::string text = nyamp::completion_to_json(raw, meta);
  CHECK(nyamp::utf8_is_valid(text));
  CHECK(nyamp::json_parse(text, &response, nullptr));
}

void test_template_without_assets()
{
  // Anthropic-shaped tools are wrapped; OpenAI-shaped ones pass untouched.
  Json tools = nyamp::normalize_tools(parse(kTools));
  CHECK_EQ(nyamp::json_dump_compact(tools.items()[1]),
           "{\"type\":\"function\",\"function\":{\"name\":\"nyabula_music\","
           "\"description\":\"d\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{\"topic\":{"
           "\"type\":\"string\"},"
           "\"arguments\":{\"type\":\"object\"}}}}}");

  nyamp::RenderedPrompt prompt;
  std::string error;
  CHECK(!nyamp::render_chat_prompt(parse("{\"messages\":[]}"), {}, &prompt,
                                   &error));
  CHECK(!nyamp::render_chat_prompt(parse("{}"), {}, &prompt, &error));

  CHECK(nyamp::render_chat_prompt(parse(kPartsRequest), {}, &prompt, &error));
  CHECK_EQ(prompt.text, "<s><|im_start|>user\na\nb<|im_end|>\n<|im_start|>"
                        "assistant\n<think>\n\n</think>\n\n");
  CHECK(prompt.untrusted.size() == 1);
  CHECK_EQ(
      prompt.text.substr(prompt.untrusted[0].begin,
                         prompt.untrusted[0].end - prompt.untrusted[0].begin),
      "a\nb");
}

} // namespace

int main()
{
  test_python_numbers();
  test_json_rejects_garbage();
  test_utf8();
  test_parse_well_formed();
  test_parse_malformed();
  test_parse_text_and_think();
  test_response_json();
  test_template_without_assets();
  std::printf("unit: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
