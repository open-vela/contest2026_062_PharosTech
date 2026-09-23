/****************************************************************************
 * tools/amp/voice/nyamp_speaker_file_test.cpp
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

// File-driven owner-voiceprint test.  It replays the product flow -- a few
// short takes are enrolled, averaged into one voiceprint, and short probes
// are scored against it -- through the same classes nyampd will use.  The
// build machine has no Python, and the device has none either, so the
// statistics (score spread, equal error rate) are computed here as well.
//
// Manifest: one "enrol|probe<TAB>LABEL<TAB>WAV" per line.  A probe labelled
// "-" belongs to nobody and is an impostor for every enrolled label.

#include "c-api.h"
#include "nyamp_speaker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{

using nyamp::voice::Embedding;
using nyamp::voice::Status;

constexpr std::size_t kRate = nyamp::voice::kSampleRate;

// Test-only tag ("CMP1").  The product derives the tag from the verified
// model manifest so a voiceprint can never be scored by another model.
constexpr std::uint32_t kTestModelTag = 0x31504d43U;

struct Entry
{
  std::string label;
  std::string path;
};

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

bool ReadWave(const std::string &path, std::vector<float> *samples)
{
  const SherpaOnnxWave *wave = SherpaOnnxReadWave(path.c_str());
  if (!wave || wave->sample_rate != 16000 || wave->num_samples <= 0)
    {
      if (wave)
        SherpaOnnxFreeWave(wave);
      return false;
    }
  samples->assign(wave->samples, wave->samples + wave->num_samples);
  SherpaOnnxFreeWave(wave);
  return true;
}

// Removes 20 ms frames more than 26 dB below the loud frames.  In the
// product the segment comes from the keyword spotter's start/end offsets and
// is dense speech; public test recordings have leading silence and pauses
// that would otherwise turn a "1.5 s probe" into half a second of voice.
std::vector<float> GateSilence(const std::vector<float> &samples)
{
  constexpr std::size_t frame = kRate / 50;
  const std::size_t frames = samples.size() / frame;
  if (frames < 5)
    {
      return samples;
    }
  std::vector<float> rms(frames);
  for (std::size_t f = 0; f < frames; ++f)
    {
      double energy = 0.0;
      for (std::size_t i = 0; i < frame; ++i)
        {
          const double value = samples[f * frame + i];
          energy += value * value;
        }
      rms[f] = static_cast<float>(std::sqrt(energy / frame));
    }
  std::vector<float> sorted = rms;
  std::sort(sorted.begin(), sorted.end());
  const float floor = 0.05f * sorted[frames * 95 / 100];
  std::vector<float> voiced;
  voiced.reserve(samples.size());
  for (std::size_t f = 0; f < frames; ++f)
    {
      if (rms[f] >= floor)
        {
          voiced.insert(voiced.end(), samples.begin() + f * frame,
                        samples.begin() + (f + 1) * frame);
        }
    }
  return voiced;
}

struct Stats
{
  std::vector<float> values;
  void Print(const char *name) const
  {
    if (values.empty())
      {
        std::cout << "SPK_SCORES kind=" << name << " n=0\n";
        return;
      }
    std::vector<float> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (float value : sorted)
      sum += value;
    const auto at = [&](double q) {
      return sorted[static_cast<std::size_t>(q * (sorted.size() - 1))];
    };
    std::cout << "SPK_SCORES kind=" << name << " n=" << sorted.size()
              << " min=" << sorted.front() << " p05=" << at(0.05)
              << " p50=" << at(0.5) << " p95=" << at(0.95)
              << " max=" << sorted.back() << " mean=" << sum / sorted.size()
              << "\n";
  }
};

int Usage()
{
  std::cerr
      << "Usage: nyamp_speaker_file_test MODEL_DIRECTORY --manifest FILE\n"
         "  --enrol-count N       takes per enrolment (default 4)\n"
         "  --whole-enrol         one enrol file is one take (wake phrase)\n"
         "  --probe-seconds X     cut probes to X s, hop X/2 (0 = whole)\n"
         "  --min-mean-cosine X   enrolment outlier bar (absolute)\n"
         "  --max-below-median X  enrolment outlier bar (relative, 0 = off)\n"
         "  --no-gate             keep silence\n"
         "  --matrix              also print the whole-file cosine matrix\n"
         "  --threads N\n"
         "  --quiet               no per-trial lines\n";
  return 2;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 4)
    {
      return Usage();
    }
  nyamp::voice::SpeakerConfig config;
  config.model_directory = argv[1];
  nyamp::voice::EnrolPolicy policy;
  std::string manifest;
  std::size_t enrol_count = 4;
  double probe_seconds = 0.0;
  bool whole_enrol = false, gate = true, matrix = false, quiet = false;
  for (int i = 2; i < argc; ++i)
    {
      const std::string arg = argv[i];
      const bool has_value = i + 1 < argc;
      if (arg == "--manifest" && has_value)
        manifest = argv[++i];
      else if (arg == "--enrol-count" && has_value)
        enrol_count = static_cast<std::size_t>(std::atol(argv[++i]));
      else if (arg == "--probe-seconds" && has_value)
        probe_seconds = std::atof(argv[++i]);
      else if (arg == "--min-mean-cosine" && has_value)
        policy.min_mean_cosine = std::strtof(argv[++i], nullptr);
      else if (arg == "--max-below-median" && has_value)
        policy.max_below_median = std::strtof(argv[++i], nullptr);
      else if (arg == "--threads" && has_value)
        config.num_threads = std::atoi(argv[++i]);
      else if (arg == "--whole-enrol")
        whole_enrol = true;
      else if (arg == "--no-gate")
        gate = false;
      else if (arg == "--matrix")
        matrix = true;
      else if (arg == "--quiet")
        quiet = true;
      else
        return Usage();
    }
  std::vector<Entry> enrol_files, probe_files;
  {
    std::ifstream in(manifest);
    std::string line;
    while (std::getline(in, line))
      {
        std::istringstream fields(line);
        std::string role, label, path;
        if (std::getline(fields, role, '\t') &&
            std::getline(fields, label, '\t') && std::getline(fields, path))
          {
            (role == "enrol" ? enrol_files : probe_files)
                .push_back({ label, path });
          }
      }
  }
  if (enrol_files.empty() || probe_files.empty() ||
      enrol_count < policy.min_kept || enrol_count > policy.max_segments)
    {
      return Usage();
    }

  const long rss_before_kb = ProcStatusKb("VmRSS:");
  nyamp::voice::SpeakerEmbedder embedder;
  const auto load_begin = std::chrono::steady_clock::now();
  if (embedder.Load(config) != Status::kOk)
    {
      return 3;
    }
  const double load_s = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - load_begin)
                            .count();
  const long rss_loaded_kb = ProcStatusKb("VmRSS:");

  // Contract checks that need no particular audio.
  {
    std::vector<float> quiet_second(kRate, 0.0f);
    Embedding unused;
    if (embedder.Load(config) != Status::kBusy ||
        embedder.Embed(quiet_second.data(),
                       nyamp::voice::kSpeakerMinSamples - 1, &unused,
                       nullptr) != Status::kInvalid ||
        embedder.Embed(quiet_second.data(), quiet_second.size(), &unused,
                       [] { return true; }) != Status::kCancelled ||
        !unused.empty())
      {
        return 4;
      }
  }

  double embed_s = 0.0, embed_audio_s = 0.0;
  unsigned int embeds = 0;
  const auto embed = [&](const float *samples, std::size_t count,
                         Embedding *out) {
    const auto begin = std::chrono::steady_clock::now();
    const Status status = embedder.Embed(samples, count, out, nullptr);
    embed_s +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
            .count();
    embed_audio_s += static_cast<double>(count) / kRate;
    ++embeds;
    return status == Status::kOk;
  };
  const auto load_audio = [&](const std::string &path,
                              std::vector<float> *samples) {
    std::vector<float> raw;
    if (!ReadWave(path, &raw))
      {
        std::cerr << "SPK_BAD_WAVE " << path << "\n";
        return false;
      }
    *samples = gate ? GateSilence(raw) : raw;
    return samples->size() >= nyamp::voice::kSpeakerMinSamples;
  };

  if (matrix)
    {
      std::vector<Entry> all = enrol_files;
      all.insert(all.end(), probe_files.begin(), probe_files.end());
      std::vector<Embedding> vectors(all.size());
      for (std::size_t i = 0; i < all.size(); ++i)
        {
          std::vector<float> samples;
          if (!load_audio(all[i].path, &samples) ||
              !embed(
                  samples.data(),
                  std::min(samples.size(), nyamp::voice::kSpeakerMaxSamples),
                  &vectors[i]))
            return 5;
        }
      std::cout.precision(3);
      for (std::size_t i = 0; i < all.size(); ++i)
        {
          std::cout << "SPK_MATRIX " << i << " " << all[i].label << " "
                    << all[i].path.substr(all[i].path.rfind('/') + 1);
          for (std::size_t j = 0; j < all.size(); ++j)
            std::cout << " " << std::fixed
                      << nyamp::voice::Cosine(vectors[i], vectors[j]);
          std::cout << "\n";
        }
      std::cout.unsetf(std::ios::fixed);
      std::cout.precision(6);
    }

  // Enrolment.  Takes are either whole files (synthetic wake phrases) or
  // 1.2-2.0 s cuts of longer speech, the duration range of the wake phrase.
  std::map<std::string, std::vector<Embedding> > takes;
  {
    std::map<std::string, std::vector<float> > pooled;
    for (const Entry &entry : enrol_files)
      {
        std::vector<float> samples;
        if (!load_audio(entry.path, &samples))
          return 5;
        if (whole_enrol)
          {
            if (takes[entry.label].size() < enrol_count)
              {
                Embedding vector;
                if (!embed(samples.data(), samples.size(), &vector))
                  return 6;
                takes[entry.label].push_back(std::move(vector));
              }
          }
        else
          {
            auto &pool = pooled[entry.label];
            pool.insert(pool.end(), samples.begin(), samples.end());
          }
      }
    constexpr double kCuts[] = { 1.2, 1.6, 2.0, 1.4, 1.8 };
    for (const auto &[label, pool] : pooled)
      {
        std::size_t offset = 0;
        for (std::size_t n = 0; n < enrol_count; ++n)
          {
            const auto length = static_cast<std::size_t>(kCuts[n % 5] * kRate);
            if (offset + length > pool.size())
              break;
            Embedding vector;
            if (!embed(pool.data() + offset, length, &vector))
              return 6;
            takes[label].push_back(std::move(vector));
            offset += length;
          }
      }
  }

  std::map<std::string, Embedding> voiceprints;
  float worst_round_trip = 1.0f;
  for (const auto &[label, vectors] : takes)
    {
      nyamp::voice::Enrolment enrolment;
      for (const Embedding &vector : vectors)
        {
          if (enrolment.Add(vector, policy) != Status::kOk)
            return 7;
        }
      const nyamp::voice::EnrolResult result = enrolment.Commit(policy);
      std::size_t kept = 0;
      for (bool flag : result.kept)
        kept += flag ? 1 : 0;
      std::cout << "SPK_ENROL label=" << label << " takes=" << vectors.size()
                << " kept=" << kept
                << " status=" << nyamp::voice::WireStatus(result.status)
                << " cohesion=" << result.cohesion << "\n";
      if (result.status != Status::kOk)
        continue;

      // What the control domain stores is the packed form, so score with
      // the voiceprint as it comes back from storage, not the float one.
      const auto packed = nyamp::voice::PackTemplate(
          result.voiceprint,
          { kTestModelTag, static_cast<std::uint16_t>(kept) });
      Embedding restored;
      nyamp::voice::TemplateInfo info;
      if (packed.size() !=
              nyamp::voice::kTemplateHeaderSize + embedder.dim() * 2 ||
          nyamp::voice::UnpackTemplate(packed.data(), packed.size(), &restored,
                                       &info) != Status::kOk ||
          info.segments != kept || info.model_tag != kTestModelTag ||
          nyamp::voice::UnpackTemplate(packed.data(), packed.size() - 1,
                                       &restored, &info) != Status::kInvalid)
        {
          return 8;
        }
      worst_round_trip = std::min(
          worst_round_trip, nyamp::voice::Cosine(result.voiceprint, restored));
      voiceprints[label] = std::move(restored);
    }
  if (voiceprints.empty() || worst_round_trip < 0.9999f)
    {
      return 9;
    }

  // Outlier rejection: slip one take of every other speaker into each
  // enrolment and see whether exactly that take is refused.
  unsigned int outlier_cases = 0, foreign_rejected = 0, genuine_dropped = 0;
  unsigned int enrolment_failed = 0;
  for (const auto &[owner, own_takes] : takes)
    {
      if (own_takes.size() < policy.min_kept + 1)
        continue;
      for (const auto &[other, other_takes] : takes)
        {
          if (other == owner || other_takes.empty())
            continue;
          nyamp::voice::Enrolment enrolment;
          enrolment.Add(own_takes[0], policy);
          enrolment.Add(other_takes[0], policy);
          for (std::size_t n = 1; n < own_takes.size(); ++n)
            enrolment.Add(own_takes[n], policy);
          const auto result = enrolment.Commit(policy);
          ++outlier_cases;
          if (result.status != Status::kOk)
            {
              ++enrolment_failed;
              continue;
            }
          foreign_rejected += result.kept[1] ? 0 : 1;
          for (std::size_t n = 0; n < result.kept.size(); ++n)
            genuine_dropped += (n != 1 && !result.kept[n]) ? 1 : 0;
        }
    }
  std::cout << "SPK_OUTLIER cases=" << outlier_cases
            << " foreign_rejected=" << foreign_rejected
            << " genuine_dropped=" << genuine_dropped
            << " enrolment_failed=" << enrolment_failed
            << " min_mean_cosine=" << policy.min_mean_cosine
            << " max_below_median=" << policy.max_below_median << "\n";

  // Verification trials.
  Stats target, impostor;
  for (const Entry &entry : probe_files)
    {
      std::vector<float> samples;
      if (!load_audio(entry.path, &samples))
        return 5;
      const std::size_t length =
          probe_seconds > 0.0
              ? static_cast<std::size_t>(probe_seconds * kRate)
              : std::min(samples.size(), nyamp::voice::kSpeakerMaxSamples);
      const std::size_t hop = probe_seconds > 0.0 ? length / 2 : length;
      for (std::size_t offset = 0; offset + length <= samples.size();
           offset += hop)
        {
          Embedding probe;
          if (!embed(samples.data() + offset, length, &probe))
            return 6;
          for (const auto &[label, voiceprint] : voiceprints)
            {
              const float score = nyamp::voice::Verify(voiceprint, probe);
              const bool same = label == entry.label;
              (same ? target : impostor).values.push_back(score);
              if (!quiet)
                {
                  std::cout
                      << "SPK_TRIAL kind=" << (same ? "target" : "impostor")
                      << " owner=" << label << " probe="
                      << entry.path.substr(entry.path.rfind('/') + 1) << "@"
                      << offset / static_cast<double>(kRate)
                      << " score=" << score << "\n";
                }
            }
        }
    }
  target.Print("target");
  impostor.Print("impostor");
  if (target.values.empty() || impostor.values.empty())
    {
      return 10;
    }

  const auto rates = [&](float threshold, double *frr, double *far) {
    std::size_t rejected = 0, accepted = 0;
    for (float score : target.values)
      rejected += score < threshold ? 1 : 0;
    for (float score : impostor.values)
      accepted += score >= threshold ? 1 : 0;
    *frr = static_cast<double>(rejected) / target.values.size();
    *far = static_cast<double>(accepted) / impostor.values.size();
  };
  double best_gap = 2.0, eer = 1.0;
  float eer_threshold = 0.0f;
  for (int step = -200; step <= 1000; ++step)
    {
      const float threshold = step / 1000.0f;
      double frr, far;
      rates(threshold, &frr, &far);
      if (std::fabs(frr - far) < best_gap)
        {
          best_gap = std::fabs(frr - far);
          eer = (frr + far) / 2;
          eer_threshold = threshold;
        }
    }
  std::cout << "SPK_EER eer=" << eer << " threshold=" << eer_threshold
            << " small_sample=1\n";
  for (int step = 30; step <= 75; step += 5)
    {
      double frr, far;
      rates(step / 100.0f, &frr, &far);
      std::cout << "SPK_OPERATING threshold=" << step / 100.0
                << " false_reject=" << frr << " false_accept=" << far << "\n";
    }

  std::cout << "SPK_SUMMARY dim=" << embedder.dim()
            << " owners=" << voiceprints.size()
            << " probe_seconds=" << probe_seconds
            << " enrol_count=" << enrol_count << " whole_enrol=" << whole_enrol
            << " gate=" << gate << " embeds=" << embeds
            << " embed_ms_mean=" << 1000.0 * embed_s / embeds
            << " rtf=" << embed_s / embed_audio_s << " load_s=" << load_s
            << " threads=" << config.num_threads << " template_bytes="
            << nyamp::voice::kTemplateHeaderSize + embedder.dim() * 2
            << " half_round_trip_cosine_min=" << worst_round_trip
            << " rss_before_kb=" << rss_before_kb
            << " rss_loaded_kb=" << rss_loaded_kb
            << " rss_peak_kb=" << ProcStatusKb("VmHWM:") << "\n";
  double target_mean = 0.0, impostor_mean = 0.0;
  for (float score : target.values)
    target_mean += score / target.values.size();
  for (float score : impostor.values)
    impostor_mean += score / impostor.values.size();
  if (!(target_mean > impostor_mean))
    {
      return 11;
    }
  std::cout << "NYAMP_SPEAKER_FILE_PASS real_inference=1 mic_capture=0\n";
  return 0;
}
