/****************************************************************************
 * tools/amp/models/nyamp_model_file.cpp
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

#include "nyamp_backends.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <utility>

namespace
{

template <typename Integer>
bool NyampReadIds(const char *path, std::vector<Integer> &ids)
{
  std::ifstream input(path);
  if (!input)
    return false;
  std::int64_t value;
  while (input >> value)
    {
      if (value < 0 || value > std::numeric_limits<Integer>::max() ||
          ids.size() >= 4096)
        return false;
      ids.push_back(static_cast<Integer>(value));
    }
  return input.eof() && !ids.empty();
}

} // namespace

int main(int argc, char **argv)
{
  using namespace nyamp::models;
  Input input;
  std::unique_ptr<Backend> backend;
#ifdef NYAMP_FILE_LLM
  if (argc != 3)
    {
      std::cerr << "Usage: nyamp_llm_file MODEL_DIRECTORY TOKEN_IDS_TXT\n";
      return 2;
    }
  LlmInput tokens;
  if (!NyampReadIds(argv[2], tokens.token_ids))
    return 3;
  input = std::move(tokens);
  backend = CreateRkllmBackend();
#else
  if (argc != 4)
    {
      std::cerr << "Usage: nyamp_tts_file MODEL_DIRECTORY PHONEME_IDS_TXT "
                   "TONE_IDS_TXT\n";
      return 2;
    }
  TtsInput text;
  if (!NyampReadIds(argv[2], text.phoneme_ids) ||
      !NyampReadIds(argv[3], text.tone_ids))
    return 3;
  input = std::move(text);
  backend = CreateMeloBackend();
#endif
  auto clock = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  Session session(std::move(backend), 1, clock);
  if (session.Load(argv[1]) != Status::kOk)
    return 4;
  const auto result = session.Run({ 1, 1, 0 }, input, [](const Event &event) {
    if (event.terminal)
      {
        std::cout << "NYAMP_FILE_TERMINAL status="
                  << static_cast<int>(event.status) << "\n";
      }
    else if (const auto *token = std::get_if<TokenChunk>(event.output.get()))
      {
        std::cout << token->text << std::flush;
      }
    else if (const auto *pcm = std::get_if<PcmChunk>(event.output.get()))
      {
        std::cout << "NYAMP_FILE_PCM samples=" << pcm->samples.size()
                  << " rate=" << pcm->sample_rate
                  << " channels=" << pcm->channels << "\n";
      }
    return true;
  });
  return result == Status::kOk ? 0 : 5;
}
