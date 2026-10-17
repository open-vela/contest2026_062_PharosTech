/****************************************************************************
 * tools/amp/models/nyamp_asr_file_test.cpp
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

#include "c-api.h"
#include "nyamp_backends.h"

#include <chrono>
#include <iostream>

int main(int argc, char **argv)
{
  using namespace nyamp::models;
  if (argc != 3)
    {
      std::cerr << "Usage: nyamp_asr_file_test MODEL_DIRECTORY INPUT_WAV\n";
      return 2;
    }
  const auto free_wave = [](const SherpaOnnxWave *wave) {
    SherpaOnnxFreeWave(wave);
  };
  std::unique_ptr<const SherpaOnnxWave, decltype(free_wave)> wave(
      SherpaOnnxReadWave(argv[2]), free_wave);
  if (!wave || wave->sample_rate != 16000 || wave->num_samples <= 0)
    {
      return 3;
    }
  AsrInput input{
    std::vector<float>(wave->samples, wave->samples + wave->num_samples), 16000
  };
  auto clock = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  Session session(CreateSherpaBackend(), 1, clock);
  if (session.Load(argv[1]) != Status::kOk)
    return 4;
  std::string reference;
  for (std::uint64_t request = 1; request <= 3; ++request)
    {
      unsigned int terminals = 0, finals = 0, partials = 0;
      std::string text;
      bool requested_cancel = false;
      auto status =
          session.Run({ request, 1, 0 }, input, [&](const Event &event) {
            if (event.terminal)
              {
                ++terminals;
                return true;
              }
            const auto &transcript = std::get<Transcript>(*event.output);
            if (transcript.final)
              {
                ++finals;
                text = transcript.text;
              }
            else
              {
                ++partials;
              }
            if (request == 2 && !requested_cancel)
              {
                requested_cancel = session.Cancel(request, 1) == Status::kOk;
              }
            return true;
          });
      if (terminals != 1)
        return 5;
      if (request == 2)
        {
          if (!requested_cancel || status != Status::kCancelled || finals != 0)
            return 6;
        }
      else
        {
          if (status != Status::kOk || finals != 1 || partials == 0 ||
              text.empty())
            return 7;
          if (request == 1)
            reference = text;
          else if (text != reference)
            return 8;
        }
      std::cout << "ASR_FILE_REQUEST id=" << request
                << " partials=" << partials << " finals=" << finals
                << " terminals=" << terminals
                << " cancelled=" << requested_cancel << " text=" << text
                << "\n";
    }
  std::cout << "NYAMP_ASR_FILE_PASS real_inference=1 mic_capture=0\n";
  return session.Unload() == Status::kOk ? 0 : 9;
}
