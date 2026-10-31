/****************************************************************************
 * tools/amp/g2p/nyamp_g2p_main.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* nyamp-g2p: command line probe for the MeloTTS front end.  It exists so
 * that ids can be diffed against other front ends (sherpa-onnx debug log,
 * the Python fixture script) and fed to the ONNX model on a host without
 * writing any glue code.
 */

#include "nyamp_g2p.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

namespace
{

void Usage()
{
  std::fprintf(stderr,
               "usage: nyamp-g2p [options] <assets-dir> \"<text>\"\n"
               "       nyamp-g2p [options] <assets-dir> -   (one text per "
               "stdin line)\n"
               "  --frames N   latent frame capacity (default 512, 0 = no "
               "budget)\n"
               "  --fps F      frames per symbol used for the budget\n"
               "  --fmm        forward maximum matching (sherpa-onnx parity)\n"
               "  --no-merge   one utterance per sentence\n"
               "  --no-words   never split between words\n"
               "  --sandhi     apply yi/bu tone sandhi\n"
               "  --names      also print symbol names\n"
               "  --stats      print load time and memory\n");
}

/* VmRSS / VmHWM are the numbers the memory requirement is stated in, and
 * /proc is the only dependency-free way to read them on Linux.
 */

void PrintMemory()
{
  std::ifstream status("/proc/self/status");
  std::string line;

  while (std::getline(status, line))
    {
      if (line.compare(0, 6, "VmRSS:") == 0 ||
          line.compare(0, 6, "VmHWM:") == 0)
        {
          std::printf("%s\n", line.c_str());
        }
    }
}

std::map<long long, std::string> LoadNames(const std::string &dir)
{
  std::map<long long, std::string> names;
  std::ifstream in(dir + "/tokens.txt");
  std::string line;

  while (std::getline(in, line))
    {
      const std::size_t sp = line.rfind(' ');
      if (sp != std::string::npos && sp != 0)
        {
          names[std::atoll(line.c_str() + sp + 1)] = line.substr(0, sp);
        }
    }

  return names;
}

void PrintVector(const char *label, const std::vector<std::int64_t> &v)
{
  std::printf("%s:", label);
  for (std::int64_t x : v)
    {
      std::printf(" %lld", static_cast<long long>(x));
    }

  std::printf("\n");
}

} /* namespace */

int main(int argc, char **argv)
{
  nyamp::MeloFrontend::Options options;
  std::size_t frames = 512;
  bool names = false;
  bool stats = false;
  int arg = 1;

  for (; arg < argc && std::strncmp(argv[arg], "--", 2) == 0; arg++)
    {
      const std::string flag = argv[arg];

      if (flag == "--frames" && arg + 1 < argc)
        {
          frames = static_cast<std::size_t>(std::atoll(argv[++arg]));
        }
      else if (flag == "--fps" && arg + 1 < argc)
        {
          options.frames_per_symbol = std::atof(argv[++arg]);
        }
      else if (flag == "--fmm")
        {
          options.segmenter =
              nyamp::MeloFrontend::Segmenter::kForwardMaxMatch;
        }
      else if (flag == "--no-merge")
        {
          options.merge_sentences = false;
        }
      else if (flag == "--no-words")
        {
          options.split_at_words = false;
        }
      else if (flag == "--sandhi")
        {
          options.yi_bu_sandhi = true;
        }
      else if (flag == "--names")
        {
          names = true;
        }
      else if (flag == "--stats")
        {
          stats = true;
        }
      else
        {
          Usage();
          return 2;
        }
    }

  if (argc - arg != 2)
    {
      Usage();
      return 2;
    }

  const std::string dir = argv[arg];
  const std::string text = argv[arg + 1];
  std::string error;
  const auto frontend = nyamp::MeloFrontend::Load(dir, &error, options);

  if (!frontend)
    {
      std::fprintf(stderr, "nyamp-g2p: %s\n", error.c_str());
      return 1;
    }

  if (stats)
    {
      const auto &s = frontend->load_stats();
      std::printf("load_ms=%.1f tokens=%zu lexicon=%zu rejected=%zu "
                  "jieba=%zu segmenter=%s budget=%zu\n",
                  s.load_ms, s.tokens, s.lexicon_entries, s.lexicon_rejected,
                  s.jieba_entries,
                  frontend->active_segmenter() ==
                          nyamp::MeloFrontend::Segmenter::kFewestWords
                      ? "fewest-words" : "forward-max-match",
                  frontend->SymbolBudget(frames));
      PrintMemory();
    }

  const std::map<long long, std::string> name_of =
      names ? LoadNames(dir) : std::map<long long, std::string>();

  auto run = [&](const std::string &input)
    {
      const auto start = std::chrono::steady_clock::now();
      const auto utterances = frontend->Process(input, frames);
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count();

      std::printf("input: %s\n", input.c_str());
      std::printf("normalized: %s\n",
                  nyamp::MeloFrontend::Normalize(input).c_str());
      std::printf("utterances=%zu process_ms=%.3f\n", utterances.size(), ms);
      for (std::size_t k = 0; k < utterances.size(); k++)
        {
          const auto &u = utterances[k];

          std::printf("utt %zu symbols=%zu tokens=%zu dropped=%zu "
                      "over_budget=%d text=%s\n",
                      k, u.symbols, u.phonemes.size(), u.dropped,
                      u.over_budget ? 1 : 0, u.text.c_str());
          PrintVector("x", u.phonemes);
          PrintVector("tones", u.tones);
          if (names)
            {
              std::printf("names:");
              for (std::size_t i = 1; i < u.phonemes.size(); i += 2)
                {
                  const auto it = name_of.find(u.phonemes[i]);
                  std::printf(" %s%lld",
                              it == name_of.end() ? "?" : it->second.c_str(),
                              static_cast<long long>(u.tones[i]));
                }

              std::printf("\n");
            }
        }
    };

  if (text == "-")
    {
      std::string line;
      while (std::getline(std::cin, line))
        {
          if (!line.empty())
            {
              run(line);
            }
        }
    }
  else
    {
      run(text);
    }

  if (stats)
    {
      std::printf("dropped_total=%llu\n",
                  static_cast<unsigned long long>(frontend->dropped_total()));
      PrintMemory();
    }

  return 0;
}
