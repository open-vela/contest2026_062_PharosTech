/****************************************************************************
 * tools/amp/chat/nyamp_chat_cli.cpp
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

// Bench and inspection tool.  It exists so that the numbers quoted in the
// README (load time, memory, tokens per tool list) can be re-measured on the
// board with one static binary instead of being taken on faith.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nyamp_chat_parse.h"
#include "nyamp_chat_template.h"
#include "nyamp_json.h"
#include "nyamp_tokenizer.h"

namespace
{

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

bool read_file(const char *path, std::string *out)
{
  if (std::strcmp(path, "-") == 0)
    {
      std::stringstream buffer;
      buffer << std::cin.rdbuf();
      *out = buffer.str();
      return true;
    }
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  std::stringstream buffer;
  buffer << file.rdbuf();
  *out = buffer.str();
  return true;
}

// Linux only; prints 0 elsewhere.  VmHWM is the peak, which is what decides
// whether loading fits next to the model on a 4 GB board.
long proc_status_kb(const char *key)
{
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line))
    {
      if (line.compare(0, std::strlen(key), key) == 0)
        {
          return std::atol(line.c_str() + std::strlen(key) + 1);
        }
    }
  return 0;
}

void print_ids(const std::vector<int32_t> &ids)
{
  for (size_t i = 0; i < ids.size(); ++i)
    {
      std::printf(i ? " %d" : "%d", ids[i]);
    }
  std::printf("\n");
}

int usage()
{
  std::fprintf(
      stderr,
      "usage:\n"
      "  nyamp_chat_cli info   <tokenizer.json>\n"
      "  nyamp_chat_cli encode <tokenizer.json> <text-file|-> [--no-special]\n"
      "  nyamp_chat_cli prompt <tokenizer.json> <request.json|-> "
      "[--reference] [--quiet]\n"
      "  nyamp_chat_cli bench  <tokenizer.json> <text-file> [iterations]\n"
      "  nyamp_chat_cli parse  <request.json|none> <model-output-file|->\n");
  return 2;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 3)
    return usage();
  std::string command = argv[1];
  std::string error;

  if (command == "parse")
    {
      if (argc < 4)
        return usage();
      nyamp::Json request;
      const nyamp::Json *tools = nullptr;
      std::string text;
      if (std::strcmp(argv[2], "none") != 0)
        {
          if (!read_file(argv[2], &text) ||
              !nyamp::json_parse(text, &request, &error))
            {
              std::fprintf(stderr, "bad request file: %s\n", error.c_str());
              return 1;
            }
          tools = request.find("tools");
        }
      std::string output;
      if (!read_file(argv[3], &output))
        return 1;
      nyamp::ParseOptions options;
      options.id_seed = "cli";
      nyamp::ResponseMeta meta;
      meta.id = "chatcmpl-cli";
      meta.model = "minicpm5-1b";
      std::printf("%s\n",
                  nyamp::completion_to_json(
                      nyamp::parse_completion(output, tools, options), meta)
                      .c_str());
      return 0;
    }

  long rss_before = proc_status_kb("VmRSS:");
  auto start = Clock::now();
  nyamp::Tokenizer tokenizer;
  if (!tokenizer.load(argv[2], &error))
    {
      std::fprintf(stderr, "load failed: %s\n", error.c_str());
      return 1;
    }
  double load_ms = ms_since(start);

  if (command == "info")
    {
      std::printf("load_ms=%.1f id_space=%zu resident_tables_kb=%zu\n",
                  load_ms, tokenizer.vocab_size(),
                  tokenizer.memory_bytes() / 1024);
      std::printf("rss_before_kb=%ld rss_after_kb=%ld peak_rss_kb=%ld\n",
                  rss_before, proc_status_kb("VmRSS:"),
                  proc_status_kb("VmHWM:"));
      for (const char *token :
           { "<s>", "</s>", "<|im_start|>", "<|im_end|>", "<think>",
             "</think>", "<function", "</function>", "<param", "</param>",
             "<tool_response>", "</tool_response>", "<tools>", "</tools>",
             "/think", "/no_think" })
        {
          int32_t id = tokenizer.token_to_id(token);
          std::printf("%-18s %6d %s\n", token, id,
                      tokenizer.is_special(id) ? "special"
                                               : "added, not special");
        }
      return 0;
    }

  if (argc < 4)
    return usage();
  std::string input;
  if (!read_file(argv[3], &input))
    {
      std::fprintf(stderr, "cannot read %s\n", argv[3]);
      return 1;
    }

  if (command == "encode")
    {
      bool allow_special =
          !(argc > 4 && std::strcmp(argv[4], "--no-special") == 0);
      print_ids(tokenizer.encode(input, allow_special));
      return 0;
    }

  if (command == "prompt")
    {
      bool guard = true;
      bool quiet = false;
      for (int i = 4; i < argc; ++i)
        {
          if (std::strcmp(argv[i], "--reference") == 0)
            guard = false;
          if (std::strcmp(argv[i], "--quiet") == 0)
            quiet = true;
        }
      nyamp::EncodedPrompt encoded;
      nyamp::ChatTemplateOptions options;
      start = Clock::now();
      if (!nyamp::encode_chat_request(tokenizer, input, options, guard,
                                      &encoded, &error))
        {
          std::fprintf(stderr, "request rejected: %s\n", error.c_str());
          return 1;
        }
      double encode_ms = ms_since(start);
      if (!quiet)
        {
          std::printf("%s\n----\n", encoded.prompt.text.c_str());
          print_ids(encoded.ids);
        }
      std::printf(
          "prompt_bytes=%zu prompt_tokens=%zu render_and_encode_ms=%.2f\n",
          encoded.prompt.text.size(), encoded.ids.size(), encode_ms);
      return 0;
    }

  if (command == "bench")
    {
      int iterations = argc > 4 ? std::atoi(argv[4]) : 20;
      if (iterations < 1)
        iterations = 1;
      size_t tokens = 0;
      start = Clock::now();
      for (int i = 0; i < iterations; ++i)
        tokens = tokenizer.encode(input, true).size();
      double encode_ms = ms_since(start) / iterations;
      std::vector<int32_t> ids = tokenizer.encode(input, true);
      start = Clock::now();
      size_t bytes = 0;
      for (int i = 0; i < iterations; ++i)
        bytes = tokenizer.decode(ids).size();
      double decode_ms = ms_since(start) / iterations;
      std::printf("load_ms=%.1f peak_rss_kb=%ld\n", load_ms,
                  proc_status_kb("VmHWM:"));
      std::printf("encode: %zu bytes -> %zu tokens in %.3f ms  (%.0f "
                  "tokens/s, %.2f MB/s)\n",
                  input.size(), tokens, encode_ms,
                  tokens / (encode_ms / 1000.0),
                  input.size() / 1048576.0 / (encode_ms / 1000.0));
      std::printf(
          "decode: %zu tokens -> %zu bytes in %.3f ms  (%.0f tokens/s)\n",
          ids.size(), bytes, decode_ms, ids.size() / (decode_ms / 1000.0));
      return 0;
    }
  return usage();
}
