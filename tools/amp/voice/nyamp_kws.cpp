/****************************************************************************
 * tools/amp/voice/nyamp_kws.cpp
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

#include "nyamp_kws.h"

#include "c-api.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace nyamp::voice
{
namespace
{

// Decode after every 100 ms so the trigger position and the stop predicate
// are both resolved to 100 ms regardless of how large the caller's window
// is.  The encoder itself only advances every 320 ms of audio.
constexpr std::size_t kFeedSamples = kSampleRate / 10;

// One decoder frame: 10 ms hop times the zipformer subsampling factor of 4.
constexpr double kTokenFrameSeconds = 0.04;

// Collects the distinct "@label" values.  Returns false when a non-empty
// line has no label, so ids can never silently shift between two builds of
// the same keywords file.
bool ReadLabels(const std::string &path, std::vector<std::string> *labels)
{
  std::ifstream file(path);
  if (!file)
    {
      return false;
    }
  std::string line;
  while (std::getline(file, line))
    {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                               line.back() == '\t'))
        {
          line.pop_back();
        }
      if (line.empty())
        {
          continue;
        }
      const std::size_t at = line.rfind(" @");
      if (at == std::string::npos || at + 2 >= line.size() ||
          line.find(' ', at + 2) != std::string::npos)
        {
          return false;
        }
      const std::string label = line.substr(at + 2);
      if (std::find(labels->begin(), labels->end(), label) == labels->end())
        {
          labels->push_back(label);
        }
    }
  return !labels->empty() && labels->size() < kKwsUnknownKeyword;
}

} // namespace

struct KeywordSpotter::Impl
{
  std::vector<std::string> labels;
  const SherpaOnnxKeywordSpotter *spotter = nullptr;
  const SherpaOnnxOnlineStream *stream = nullptr;
  std::uint64_t accepted = 0;
  // Samples accepted when the current stream was created.  Token timestamps
  // are relative to stream creation, so this is what turns them into
  // offsets the owner can relate to its windows.
  std::uint64_t stream_base = 0;

  bool NewStream()
  {
    if (stream)
      {
        SherpaOnnxDestroyOnlineStream(stream);
      }
    stream = SherpaOnnxCreateKeywordStream(spotter);
    stream_base = accepted;
    return stream != nullptr;
  }
};

KeywordSpotter::KeywordSpotter() : impl_(std::make_unique<Impl>()) {}

KeywordSpotter::~KeywordSpotter() { Unload(); }

Status KeywordSpotter::Load(const KwsConfig &config)
{
  if (impl_->spotter)
    {
      return Status::kBusy;
    }
  if (config.model_directory.empty() || config.num_threads < 1 ||
      !(config.keywords_threshold > 0.0f && config.keywords_threshold < 1.0f))
    {
      return Status::kInvalid;
    }
  const std::string encoder = config.model_directory + "/encoder.onnx";
  const std::string decoder = config.model_directory + "/decoder.onnx";
  const std::string joiner = config.model_directory + "/joiner.onnx";
  const std::string tokens = config.model_directory + "/tokens.txt";
  const std::string keywords = config.keywords_file.empty()
                                   ? config.model_directory + "/keywords.txt"
                                   : config.keywords_file;
  std::vector<std::string> labels;
  if (!ReadLabels(keywords, &labels))
    {
      return Status::kInvalid;
    }
  SherpaOnnxKeywordSpotterConfig native{};
  native.feat_config.sample_rate = kSampleRate;
  native.feat_config.feature_dim = 80;
  native.model_config.transducer.encoder = encoder.c_str();
  native.model_config.transducer.decoder = decoder.c_str();
  native.model_config.transducer.joiner = joiner.c_str();
  native.model_config.tokens = tokens.c_str();
  native.model_config.num_threads = config.num_threads;
  native.model_config.provider = "cpu";
  native.max_active_paths = config.max_active_paths;
  native.num_trailing_blanks = config.num_trailing_blanks;
  native.keywords_score = config.keywords_score;
  native.keywords_threshold = config.keywords_threshold;
  native.keywords_file = keywords.c_str();
  impl_->spotter = SherpaOnnxCreateKeywordSpotter(&native);
  if (!impl_->spotter)
    {
      return Status::kBackendError;
    }
  impl_->labels = std::move(labels);
  impl_->accepted = 0;
  if (!impl_->NewStream())
    {
      Unload();
      return Status::kBackendError;
    }
  return Status::kOk;
}

void KeywordSpotter::Unload()
{
  if (impl_->stream)
    {
      SherpaOnnxDestroyOnlineStream(impl_->stream);
      impl_->stream = nullptr;
    }
  if (impl_->spotter)
    {
      SherpaOnnxDestroyKeywordSpotter(impl_->spotter);
      impl_->spotter = nullptr;
    }
  impl_->labels.clear();
}

Status KeywordSpotter::Reset()
{
  if (!impl_->spotter)
    {
      return Status::kNotReady;
    }
  impl_->accepted = 0;
  return impl_->NewStream() ? Status::kOk : Status::kBackendError;
}

std::uint64_t KeywordSpotter::accepted_samples() const
{
  return impl_->accepted;
}

const std::vector<std::string> &KeywordSpotter::labels() const
{
  return impl_->labels;
}

Status KeywordSpotter::AcceptWaveform(const float *samples, std::size_t count,
                                      const KwsSink &sink, const Stop &stop)
{
  if (!impl_->spotter || !impl_->stream)
    {
      return Status::kNotReady;
    }
  if (!samples || count == 0 || count > kKwsMaxWindowSamples || !sink)
    {
      return Status::kInvalid;
    }
  const auto stopped = [&stop] { return stop && stop(); };
  for (std::size_t offset = 0; offset < count; offset += kFeedSamples)
    {
      if (stopped())
        {
          return Status::kCancelled;
        }
      const std::size_t part = std::min(kFeedSamples, count - offset);
      SherpaOnnxOnlineStreamAcceptWaveform(impl_->stream, kSampleRate,
                                           samples + offset,
                                           static_cast<std::int32_t>(part));
      impl_->accepted += part;
      while (SherpaOnnxIsKeywordStreamReady(impl_->spotter, impl_->stream))
        {
          if (stopped())
            {
              return Status::kCancelled;
            }
          SherpaOnnxDecodeKeywordStream(impl_->spotter, impl_->stream);
          const SherpaOnnxKeywordResult *result =
              SherpaOnnxGetKeywordResult(impl_->spotter, impl_->stream);
          if (!result)
            {
              return Status::kBackendError;
            }
          if (!result->keyword || result->keyword[0] == '\0')
            {
              SherpaOnnxDestroyKeywordResult(result);
              continue;
            }
          KwsDetection detection;
          detection.keyword = result->keyword;
          detection.trigger_sample = impl_->accepted;
          const auto label = std::find(impl_->labels.begin(),
                                       impl_->labels.end(), detection.keyword);
          if (label != impl_->labels.end())
            {
              detection.keyword_id =
                  static_cast<std::uint16_t>(label - impl_->labels.begin());
            }
          for (std::int32_t i = 0; i < result->count; ++i)
            {
              detection.tokens.emplace_back(result->tokens_arr[i]);
            }
          if (result->count > 0 && result->timestamps)
            {
              const double first = result->timestamps[0];
              const double last =
                  result->timestamps[result->count - 1] + kTokenFrameSeconds;
              detection.has_offsets = true;
              detection.start_sample =
                  impl_->stream_base + static_cast<std::uint64_t>(
                                           std::llround(first * kSampleRate));
              detection.end_sample = std::min<std::uint64_t>(
                  impl_->accepted,
                  impl_->stream_base + static_cast<std::uint64_t>(
                                           std::llround(last * kSampleRate)));
            }
          SherpaOnnxDestroyKeywordResult(result);

          // A fresh stream, rather than SherpaOnnxResetKeywordStream, keeps
          // the timestamp origin under this class's control and also bounds
          // the float-seconds rounding error of a stream that has been
          // listening for days.  The few undecoded frames that are lost
          // belong to the command that follows the wake word, which is the
          // ASR service's business, not the spotter's.
          if (!impl_->NewStream())
            {
              return Status::kBackendError;
            }
          if (!sink(detection))
            {
              return Status::kConsumerStopped;
            }
        }
    }
  return Status::kOk;
}

} // namespace nyamp::voice
