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
#include "nyamp_streaming.h"

#include <algorithm>
#include <array>

namespace nyamp::models
{
namespace
{

constexpr std::size_t kChunkSamples = 320;
constexpr int kSampleRate = 16000;

// The padding the file replay was validated with; these are not VAD
// thresholds.  Half a second in front lets the encoder settle before the
// first word, 0.8 s behind pushes the last tokens through its look-ahead.
constexpr std::size_t kLeadingSilence = kSampleRate / 2;
constexpr std::size_t kTrailingSilence = kSampleRate * 4 / 5;

const SherpaOnnxOnlineRecognizer *CreateRecognizer(
    const std::string &directory)
{
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

  // Endpointing only answers IsEndpoint; nothing resets the stream on its
  // own, so the transcript is the same with or without it.  The rules are
  // the library's documented defaults, spelled out so an upgrade cannot
  // move them silently.
  config.enable_endpoint = 1;
  config.rule1_min_trailing_silence = 2.4f;
  config.rule2_min_trailing_silence = 1.2f;
  config.rule3_min_utterance_length = 20.0f;
  return SherpaOnnxCreateOnlineRecognizer(&config);
}

class SherpaStream final : public AsrStream
{
public:
  SherpaStream(const SherpaOnnxOnlineRecognizer *recognizer,
               const SherpaOnnxOnlineStream *stream)
      : recognizer_(recognizer), stream_(stream)
  {
    const std::array<float, kLeadingSilence> silence{};
    SherpaOnnxOnlineStreamAcceptWaveform(stream_, kSampleRate, silence.data(),
                                         silence.size());
  }

  ~SherpaStream() override { SherpaOnnxDestroyOnlineStream(stream_); }

  void Accept(const float *samples, std::size_t count) override
  {
    if (!finished_ && samples != nullptr && count != 0)
      {
        SherpaOnnxOnlineStreamAcceptWaveform(stream_, kSampleRate, samples,
                                             static_cast<int32_t>(count));
      }
  }

  bool Decode(const Stop &stop) override
  {
    while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_))
      {
        if (stop && stop())
          {
            return false;
          }

        SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
      }

    return !(stop && stop());
  }

  bool Text(std::string *text) override
  {
    const SherpaOnnxOnlineRecognizerResult *result =
        SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
    if (result == nullptr)
      {
        return false;
      }

    const bool ok = result->text != nullptr && text != nullptr;
    if (ok)
      {
        *text = result->text;
      }

    SherpaOnnxDestroyOnlineRecognizerResult(result);
    return ok;
  }

  bool Endpoint() override
  {
    return SherpaOnnxOnlineStreamIsEndpoint(recognizer_, stream_) != 0;
  }

  void InputFinished() override
  {
    if (!finished_)
      {
        const std::array<float, kTrailingSilence> silence{};
        SherpaOnnxOnlineStreamAcceptWaveform(stream_, kSampleRate,
                                             silence.data(), silence.size());
        SherpaOnnxOnlineStreamInputFinished(stream_);
        finished_ = true;
      }
  }

private:
  const SherpaOnnxOnlineRecognizer *recognizer_;
  const SherpaOnnxOnlineStream *stream_;
  bool finished_ = false;
};

class SherpaStreamBackend final : public AsrStreamBackend
{
public:
  ~SherpaStreamBackend() override { Unload(); }

  Status Load(const std::string &directory) override
  {
    if (recognizer_)
      {
        return Status::kBusy;
      }

    recognizer_ = CreateRecognizer(directory);
    return recognizer_ ? Status::kOk : Status::kBackendError;
  }

  void Unload() override
  {
    if (recognizer_)
      {
        SherpaOnnxDestroyOnlineRecognizer(recognizer_);
        recognizer_ = nullptr;
      }
  }

  std::unique_ptr<AsrStream> CreateStream() override
  {
    if (!recognizer_)
      {
        return nullptr;
      }

    const SherpaOnnxOnlineStream *stream =
        SherpaOnnxCreateOnlineStream(recognizer_);
    if (!stream)
      {
        return nullptr;
      }

    return std::make_unique<SherpaStream>(recognizer_, stream);
  }

private:
  const SherpaOnnxOnlineRecognizer *recognizer_ = nullptr;
};

// The whole-input backend is the streaming one fed from a buffer, so a file
// replay and a microphone exercise the same decoder calls.
class SherpaBackend final : public Backend
{
public:
  ~SherpaBackend() override { Unload(); }
  Kind kind() const override { return Kind::kAsr; }
  Status Load(const std::string &directory) override
  {
    return streams_.Load(directory);
  }
  void Unload() override { streams_.Unload(); }
  Status Run(const Input &input, const Emit &emit, const Stop &stop) override;

private:
  SherpaStreamBackend streams_;
};

Status SherpaBackend::Run(const Input &input, const Emit &emit,
                          const Stop &stop)
{
  const auto *audio = std::get_if<AsrInput>(&input);
  if (!audio)
    {
      return Status::kNotReady;
    }
  std::unique_ptr<AsrStream> stream = streams_.CreateStream();
  if (!stream)
    {
      return Status::kNotReady;
    }
  std::string previous;
  auto publish = [&](bool final, std::size_t consumed) {
    std::string text;
    if (!stream->Text(&text))
      {
        return false;
      }
    if (!final && text == previous)
      {
        return true;
      }
    previous = text;
    return emit(Transcript{ std::move(text), final, consumed });
  };
  for (std::size_t offset = 0; offset < audio->samples.size();
       offset += kChunkSamples)
    {
      if (stop())
        {
          return Status::kCancelled;
        }
      const auto count =
          std::min(kChunkSamples, audio->samples.size() - offset);
      stream->Accept(audio->samples.data() + offset, count);
      if (!stream->Decode(stop) || !publish(false, offset + count))
        {
          return stop() ? Status::kCancelled : Status::kBackendError;
        }
    }
  stream->InputFinished();
  if (!stream->Decode(stop) || !publish(true, audio->samples.size()))
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

std::unique_ptr<AsrStreamBackend> CreateSherpaStreamBackend()
{
  return std::make_unique<SherpaStreamBackend>();
}

} // namespace nyamp::models
