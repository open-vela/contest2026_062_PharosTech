/****************************************************************************
 * tools/amp/voice/nyamp_kws_file_test.cpp
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

// File-driven keyword spotting test.  It drives the same KeywordSpotter
// class nyampd will use, in the same fixed-size windows the ASR service
// receives, so the numbers it prints are numbers about the product path and
// not about a sherpa-onnx example binary.

#include "c-api.h"
#include "nyamp_kws.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using nyamp::voice::KwsDetection;
using nyamp::voice::Status;

long ProcStatusKb(const char *key)
{
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line))
    {
      if (line.rfind(key, 0) == 0)
        {
          return std::atol(line.c_str() + std::strlen(key));
        }
    }
  return -1;
}

int Usage()
{
  std::cerr
      << "Usage: nyamp_kws_file_test MODEL_DIRECTORY [options] WAV...\n"
         "  --keywords FILE      replace MODEL_DIRECTORY/keywords.txt\n"
         "  --threshold X        keywords_threshold\n"
         "  --score X            keywords_score (boost)\n"
         "  --trailing-blanks N  num_trailing_blanks\n"
         "  --max-active-paths N beam width of the keyword search\n"
         "  --threads N          inference threads\n"
         "  --window N           samples per AcceptWaveform (default 8000)\n"
         "  --list FILE          read more wav paths, one per line\n"
         "  --continuous         one stream across all files (soak test)\n"
         "  --expect-all         fail unless every file triggers\n"
         "  --expect-none        fail if any file triggers\n"
         "  --quiet              print only the summary\n";
  return 2;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 3)
    {
      return Usage();
    }
  nyamp::voice::KwsConfig config;
  config.model_directory = argv[1];
  std::vector<std::string> paths;
  std::size_t window = 8000;
  bool continuous = false, expect_all = false, expect_none = false;
  bool quiet = false;
  for (int i = 2; i < argc; ++i)
    {
      const std::string arg = argv[i];
      const bool has_value = i + 1 < argc;
      if (arg == "--keywords" && has_value)
        config.keywords_file = argv[++i];
      else if (arg == "--threshold" && has_value)
        config.keywords_threshold = std::strtof(argv[++i], nullptr);
      else if (arg == "--score" && has_value)
        config.keywords_score = std::strtof(argv[++i], nullptr);
      else if (arg == "--trailing-blanks" && has_value)
        config.num_trailing_blanks = std::atoi(argv[++i]);
      else if (arg == "--max-active-paths" && has_value)
        config.max_active_paths = std::atoi(argv[++i]);
      else if (arg == "--threads" && has_value)
        config.num_threads = std::atoi(argv[++i]);
      else if (arg == "--window" && has_value)
        window = static_cast<std::size_t>(std::atol(argv[++i]));
      else if (arg == "--list" && has_value)
        {
          std::ifstream list(argv[++i]);
          std::string line;
          while (std::getline(list, line))
            {
              if (!line.empty())
                paths.push_back(line);
            }
        }
      else if (arg == "--continuous")
        continuous = true;
      else if (arg == "--expect-all")
        expect_all = true;
      else if (arg == "--expect-none")
        expect_none = true;
      else if (arg == "--quiet")
        quiet = true;
      else if (arg.rfind("--", 0) == 0)
        return Usage();
      else
        paths.push_back(arg);
    }
  if (paths.empty() || window == 0 ||
      window > nyamp::voice::kKwsMaxWindowSamples)
    {
      return Usage();
    }

  const long rss_before_kb = ProcStatusKb("VmRSS:");
  nyamp::voice::KeywordSpotter spotter;
  const auto load_begin = std::chrono::steady_clock::now();
  if (spotter.Load(config) != Status::kOk)
    {
      return 3;
    }
  const double load_s = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - load_begin)
                            .count();
  const long rss_loaded_kb = ProcStatusKb("VmRSS:");

  // Contract checks that need no particular audio: a second Load is refused,
  // oversized windows are refused, and a stop request wins before any audio
  // is consumed so a cancelled window can be replayed after Reset.
  std::vector<float> probe(window, 0.0f);
  const auto ignore = [](const KwsDetection &) { return true; };
  if (spotter.Load(config) != Status::kBusy ||
      spotter.AcceptWaveform(probe.data(),
                             nyamp::voice::kKwsMaxWindowSamples + 1, ignore,
                             nullptr) != Status::kInvalid ||
      spotter.AcceptWaveform(probe.data(), probe.size(), ignore,
                             [] { return true; }) != Status::kCancelled ||
      spotter.accepted_samples() != 0 || spotter.Reset() != Status::kOk)
    {
      return 4;
    }

  // Flushes the encoder's look-ahead after a file.  A live microphone never
  // needs this because more audio always follows.
  const std::vector<float> tail(nyamp::voice::kSampleRate * 4 / 5, 0.0f);

  unsigned int files = 0, files_detected = 0, detections = 0;
  double audio_s = 0.0, decode_s = 0.0;
  for (const std::string &path : paths)
    {
      const SherpaOnnxWave *wave = SherpaOnnxReadWave(path.c_str());
      if (!wave || wave->sample_rate != 16000 || wave->num_samples <= 0)
        {
          std::cerr << "KWS_BAD_WAVE " << path << "\n";
          if (wave)
            SherpaOnnxFreeWave(wave);
          return 5;
        }
      if (!continuous && spotter.Reset() != Status::kOk)
        {
          return 6;
        }
      // Signed seconds: in continuous mode a keyword may begin in the previous
      // file, which would wrap an unsigned difference.
      const double file_base = static_cast<double>(spotter.accepted_samples());
      const auto seconds_in_file = [file_base](std::uint64_t sample) {
        return (static_cast<double>(sample) - file_base) / 16000.0;
      };
      unsigned int here = 0;
      std::string first;
      const auto sink = [&](const KwsDetection &detection) {
        ++here;
        if (!quiet)
          {
            std::cout << "KWS_DETECTED file=" << path
                      << " keyword=" << detection.keyword
                      << " id=" << detection.keyword_id << " start_s="
                      << (detection.has_offsets
                              ? seconds_in_file(detection.start_sample)
                              : -1.0)
                      << " end_s="
                      << (detection.has_offsets
                              ? seconds_in_file(detection.end_sample)
                              : -1.0)
                      << " trigger_s="
                      << seconds_in_file(detection.trigger_sample)
                      << " score=" << detection.score << "\n";
          }
        if (first.empty())
          first = detection.keyword;
        return true;
      };
      const auto begin = std::chrono::steady_clock::now();
      const auto feed = [&](const float *samples, std::size_t count) {
        for (std::size_t offset = 0; offset < count; offset += window)
          {
            const std::size_t part = std::min(window, count - offset);
            if (spotter.AcceptWaveform(samples + offset, part, sink,
                                       nullptr) != Status::kOk)
              return false;
          }
        return true;
      };
      const bool ok =
          feed(wave->samples, static_cast<std::size_t>(wave->num_samples)) &&
          feed(tail.data(), tail.size());
      decode_s += std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - begin)
                      .count();
      const double seconds = wave->num_samples / 16000.0;
      SherpaOnnxFreeWave(wave);
      if (!ok)
        {
          return 7;
        }
      ++files;
      audio_s += seconds;
      detections += here;
      files_detected += here > 0 ? 1 : 0;
      if (!quiet)
        {
          std::cout << "KWS_FILE file=" << path << " seconds=" << seconds
                    << " detections=" << here << " first=" << first << "\n";
        }
    }

  std::cout << "KWS_SUMMARY files=" << files
            << " files_detected=" << files_detected
            << " detections=" << detections << " audio_s=" << audio_s
            << " decode_s=" << decode_s
            << " rtf=" << (audio_s > 0 ? decode_s / audio_s : 0.0)
            << " load_s=" << load_s << " threads=" << config.num_threads
            << " threshold=" << config.keywords_threshold
            << " score=" << config.keywords_score
            << " paths=" << config.max_active_paths
            << " blanks=" << config.num_trailing_blanks
            << " continuous=" << continuous
            << " rss_before_kb=" << rss_before_kb
            << " rss_loaded_kb=" << rss_loaded_kb
            << " rss_end_kb=" << ProcStatusKb("VmRSS:")
            << " rss_peak_kb=" << ProcStatusKb("VmHWM:") << "\n";
  if (expect_all && files_detected != files)
    {
      return 10;
    }
  if (expect_none && detections != 0)
    {
      return 11;
    }
  std::cout << "NYAMP_KWS_FILE_PASS real_inference=1 mic_capture=0\n";
  return 0;
}
