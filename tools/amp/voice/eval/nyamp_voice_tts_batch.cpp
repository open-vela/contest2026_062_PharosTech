/****************************************************************************
 * tools/amp/voice/eval/nyamp_voice_tts_batch.cpp
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

// Host-only evidence generator.  There are no human recordings of the wake
// phrase yet, so the evaluation synthesizes it.  The stock sherpa-onnx TTS
// command reloads the model for every sentence (about eight seconds for
// Kokoro), which makes a few hundred utterances impractical; this tool
// loads once and reads a job list instead.  It is never linked into nyampd.
//
// Job list: one "SID<TAB>SPEED<TAB>OUTPUT.wav<TAB>TEXT" per line.

#include "c-api.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

std::string Join(const std::string &directory, const char *name)
{
  return directory + "/" + name;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 4)
    {
      std::cerr << "Usage: nyamp_voice_tts_batch kokoro|vits|vits-fst "
                   "MODEL_DIRECTORY JOBS.tsv\n";
      return 2;
    }
  const std::string engine = argv[1];
  const std::string directory = argv[2];
  const std::string model =
      Join(directory, engine == "kokoro" ? "model.int8.onnx" : "model.onnx");
  const std::string tokens = Join(directory, "tokens.txt");
  const std::string dict = Join(directory, "dict");
  const std::string voices = Join(directory, "voices.bin");
  const std::string espeak = Join(directory, "espeak-ng-data");
  const std::string kokoro_lexicon = Join(directory, "lexicon-us-en.txt") +
                                     "," + Join(directory, "lexicon-zh.txt");
  const std::string vits_lexicon = Join(directory, "lexicon.txt");
  const std::string fsts = Join(directory, "phone.fst") + "," +
                           Join(directory, "date.fst") + "," +
                           Join(directory, "number.fst");

  SherpaOnnxOfflineTtsConfig config;
  std::memset(&config, 0, sizeof(config));
  config.model.num_threads = 2;
  config.model.provider = "cpu";
  config.max_num_sentences = 1;
  if (engine == "kokoro")
    {
      config.model.kokoro.model = model.c_str();
      config.model.kokoro.voices = voices.c_str();
      config.model.kokoro.tokens = tokens.c_str();
      config.model.kokoro.data_dir = espeak.c_str();
      config.model.kokoro.dict_dir = dict.c_str();
      config.model.kokoro.lexicon = kokoro_lexicon.c_str();
      config.model.kokoro.length_scale = 1.0f;
    }
  else
    {
      config.model.vits.model = model.c_str();
      config.model.vits.lexicon = vits_lexicon.c_str();
      config.model.vits.tokens = tokens.c_str();
      config.model.vits.noise_scale = 0.667f;
      config.model.vits.noise_scale_w = 0.8f;
      config.model.vits.length_scale = 1.0f;
      if (engine == "vits-fst")
        {
          config.rule_fsts = fsts.c_str();
        }
      else
        {
          config.model.vits.dict_dir = dict.c_str();
        }
    }
  const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&config);
  if (!tts)
    {
      return 3;
    }
  std::ifstream jobs(argv[3]);
  std::string line;
  unsigned int done = 0, failed = 0;
  while (std::getline(jobs, line))
    {
      std::istringstream fields(line);
      std::string sid, speed, output, text;
      if (!std::getline(fields, sid, '\t') ||
          !std::getline(fields, speed, '\t') ||
          !std::getline(fields, output, '\t') || !std::getline(fields, text))
        {
          continue;
        }
      const SherpaOnnxGeneratedAudio *audio = SherpaOnnxOfflineTtsGenerate(
          tts, text.c_str(), std::atoi(sid.c_str()),
          static_cast<float>(std::atof(speed.c_str())));
      if (audio && audio->n > 0 &&
          SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate,
                              output.c_str()))
        {
          ++done;
        }
      else
        {
          ++failed;
          std::cerr << "TTS_FAILED " << output << "\n";
        }
      if (audio)
        {
          SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        }
    }
  SherpaOnnxDestroyOfflineTts(tts);
  std::cout << "TTS_BATCH engine=" << engine << " done=" << done
            << " failed=" << failed << " synthetic=1\n";
  return failed == 0 ? 0 : 4;
}
