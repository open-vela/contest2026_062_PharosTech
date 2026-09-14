/****************************************************************************
 * tools/amp/models/nyamp_sherpa.cpp
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

#include <algorithm>
#include <array>

namespace nyamp::models
{
namespace
{

constexpr std::size_t kChunkSamples = 320;
constexpr int kSampleRate = 16000;

class SherpaBackend final : public Backend
{
public:
  ~SherpaBackend() override { Unload(); }
  Kind kind() const override { return Kind::kAsr; }
  Status Load(const std::string &directory) override;
  void Unload() override;
  Status Run(const Input &input, const Emit &emit, const Stop &stop) override;

private:
  const SherpaOnnxOnlineRecognizer *recognizer_ = nullptr;
};

Status SherpaBackend::Load(const std::string &directory)
{
  if (recognizer_)
    {
      return Status::kBusy;
    }
  const std::string encoder = directory + "/encoder.onnx";
  const std::string decoder = directory + "/decoder.onnx";
  const std::string joiner = directory + "/joiner.onnx";
  const std::string tokens = directory + "/tokens.txt";
  SherpaOnnxOnlineRecognizerConfig config{};
  config.feat_config.sample_rate = kSampleRate;
  config.feat_config.feature_dim = 80;
  config.model_config.transducer.encoder = encoder.c_str();
  config.model_config.transducer.decoder = decoder.c_str();
  config.model_config.transducer.joiner = joiner.c_str();
  config.model_config.tokens = tokens.c_str();
  config.model_config.num_threads = 1;
  config.model_config.provider = "cpu";
  config.decoding_method = "greedy_search";
  recognizer_ = SherpaOnnxCreateOnlineRecognizer(&config);
  return recognizer_ ? Status::kOk : Status::kBackendError;
}

void SherpaBackend::Unload()
{
  if (recognizer_)
    {
      SherpaOnnxDestroyOnlineRecognizer(recognizer_);
      recognizer_ = nullptr;
    }
}

Status SherpaBackend::Run(const Input &input, const Emit &emit,
                          const Stop &stop)
{
  const auto *audio = std::get_if<AsrInput>(&input);
  if (!audio || !recognizer_)
    {
      return Status::kNotReady;
    }
  const auto destroy_stream = [](const SherpaOnnxOnlineStream *stream) {
    SherpaOnnxDestroyOnlineStream(stream);
  };
  std::unique_ptr<const SherpaOnnxOnlineStream, decltype(destroy_stream)>
      stream(SherpaOnnxCreateOnlineStream(recognizer_), destroy_stream);
  if (!stream)
    {
      return Status::kBackendError;
    }
  std::string previous;
  auto decode = [&]() {
    while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream.get()))
      {
        if (stop())
          {
            return false;
          }
        SherpaOnnxDecodeOnlineStream(recognizer_, stream.get());
      }
    return !stop();
  };
  auto publish = [&](bool final, std::size_t consumed) {
    const auto destroy_result =
        [](const SherpaOnnxOnlineRecognizerResult *value) {
          SherpaOnnxDestroyOnlineRecognizerResult(value);
        };
    std::unique_ptr<const SherpaOnnxOnlineRecognizerResult,
                    decltype(destroy_result)>
        result(SherpaOnnxGetOnlineStreamResult(recognizer_, stream.get()),
               destroy_result);
    if (!result || !result->text)
      {
        return false;
      }
    std::string text(result->text);
    if (!final && text == previous)
      {
        return true;
      }
    previous = text;
    return emit(Transcript{ std::move(text), final, consumed });
  };
  // Match the validated file-replay padding; these are not VAD thresholds.
  std::array<float, kSampleRate * 4 / 5> silence{};
  SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), kSampleRate,
                                       silence.data(), kSampleRate / 2);
  for (std::size_t offset = 0; offset < audio->samples.size();
       offset += kChunkSamples)
    {
      if (stop())
        {
          return Status::kCancelled;
        }
      const auto count =
          std::min(kChunkSamples, audio->samples.size() - offset);
      SherpaOnnxOnlineStreamAcceptWaveform(
          stream.get(), kSampleRate, audio->samples.data() + offset, count);
      if (!decode() || !publish(false, offset + count))
        {
          return stop() ? Status::kCancelled : Status::kBackendError;
        }
    }
  SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), kSampleRate,
                                       silence.data(), silence.size());
  SherpaOnnxOnlineStreamInputFinished(stream.get());
  if (!decode() || !publish(true, audio->samples.size()))
    {
      return stop() ? Status::kCancelled : Status::kBackendError;
    }
  return Status::kOk;
}

} // namespace

std::unique_ptr<Backend> CreateSherpaBackend()
{
  return std::make_unique<SherpaBackend>();
}

} // namespace nyamp::models
