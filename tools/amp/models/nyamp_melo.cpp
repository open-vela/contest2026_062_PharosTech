/****************************************************************************
 * tools/amp/models/nyamp_melo.cpp
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
#include "onnxruntime_c_api.h"
#include "rknn_api.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace nyamp::models
{
namespace
{

constexpr std::size_t kFrames = 512;
constexpr std::size_t kChannels = 192;
constexpr std::size_t kSpeakerValues = 256;
constexpr std::size_t kSamplesPerFrame = 512;

struct MeloValues
{
  const OrtApi *api;
  std::array<OrtValue *, 7> inputs{};
  std::array<OrtValue *, 2> latent{};
  ~MeloValues()
  {
    for (auto *value : inputs)
      if (value)
        api->ReleaseValue(value);
    for (auto *value : latent)
      if (value)
        api->ReleaseValue(value);
  }
};

struct MeloNpuOutput
{
  rknn_context context;
  rknn_output output{};
  bool live = false;
  ~MeloNpuOutput()
  {
    if (live)
      rknn_outputs_release(context, 1, &output);
  }
};

class MeloBackend final : public Backend
{
public:
  ~MeloBackend() override { Unload(); }
  Kind kind() const override { return Kind::kTts; }
  Status Load(const std::string &directory) override;
  void Unload() override;
  Status Run(const Input &input, const Emit &emit, const Stop &stop) override;

private:
  void Check(OrtStatus *status) const;
  const OrtApi *api_ = nullptr;
  OrtEnv *environment_ = nullptr;
  OrtSessionOptions *options_ = nullptr;
  OrtSession *prefix_ = nullptr;
  OrtMemoryInfo *memory_ = nullptr;
  rknn_context context_ = 0;
  std::array<rknn_input, 3> npu_inputs_{};
  int latent_index_ = -1;
  int speaker_index_ = -1;
  int mask_index_ = -1;
};

void MeloBackend::Check(OrtStatus *status) const
{
  if (status)
    {
      std::string message = api_->GetErrorMessage(status);
      api_->ReleaseStatus(status);
      throw std::runtime_error(message);
    }
}

Status MeloBackend::Load(const std::string &directory)
{
  if (prefix_ || context_)
    {
      return Status::kBusy;
    }
  api_ = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  if (!api_)
    {
      return Status::kUnsupported;
    }
  try
    {
      Check(api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "nyamp-melo",
                            &environment_));
      Check(api_->CreateSessionOptions(&options_));
      Check(api_->SetIntraOpNumThreads(options_, 4));
      Check(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                      &memory_));
      const auto prefix_path = directory + "/prefix.onnx";
      Check(api_->CreateSession(environment_, prefix_path.c_str(), options_,
                                &prefix_));
      const auto vocoder_path = directory + "/vocoder.rknn";
      if (rknn_init(&context_, const_cast<char *>(vocoder_path.c_str()), 0, 0,
                    nullptr))
        {
          throw std::runtime_error("NPU model initialization failed");
        }
      rknn_input_output_num io{};
      if (rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) ||
          io.n_input != 3 || io.n_output != 1)
        {
          throw std::runtime_error("Unexpected vocoder input/output count");
        }
      for (std::uint32_t index = 0; index < 3; ++index)
        {
          rknn_tensor_attr attribute{};
          attribute.index = index;
          if (rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &attribute,
                         sizeof(attribute)))
            {
              throw std::runtime_error("NPU input query failed");
            }
          auto &input = npu_inputs_[index];
          input = {};
          input.index = index;
          input.type = RKNN_TENSOR_FLOAT32;
          input.fmt = RKNN_TENSOR_NHWC;
          input.size = attribute.n_elems * sizeof(float);
          if (!std::strcmp(attribute.name, "latent") &&
              attribute.n_elems == kFrames * kChannels)
            latent_index_ = index;
          else if (!std::strcmp(attribute.name, "speaker") &&
                   attribute.n_elems == kSpeakerValues)
            speaker_index_ = index;
          else if (!std::strcmp(attribute.name, "valid_mask") &&
                   attribute.n_elems == kFrames)
            mask_index_ = index;
          else
            throw std::runtime_error("Unexpected NPU input contract");
        }
      if (latent_index_ < 0 || speaker_index_ < 0 || mask_index_ < 0)
        {
          throw std::runtime_error("Missing NPU input");
        }
    }
  catch (...)
    {
      Unload();
      return Status::kBackendError;
    }
  return Status::kOk;
}

void MeloBackend::Unload()
{
  if (context_)
    rknn_destroy(context_);
  if (prefix_)
    api_->ReleaseSession(prefix_);
  if (memory_)
    api_->ReleaseMemoryInfo(memory_);
  if (options_)
    api_->ReleaseSessionOptions(options_);
  if (environment_)
    api_->ReleaseEnv(environment_);
  context_ = 0;
  prefix_ = nullptr;
  memory_ = nullptr;
  options_ = nullptr;
  environment_ = nullptr;
  latent_index_ = speaker_index_ = mask_index_ = -1;
}

Status MeloBackend::Run(const Input &input, const Emit &emit, const Stop &stop)
{
  const auto *text = std::get_if<TtsInput>(&input);
  if (!text || !prefix_ || !context_)
    {
      return Status::kNotReady;
    }
  if (stop())
    return Status::kCancelled;
  auto phones = text->phoneme_ids;
  auto tones = text->tone_ids;
  std::int64_t length = phones.size(), speaker_id = text->speaker_id;
  std::int64_t dimensions[] = { 1, length }, one[] = { 1 };
  float noise = 0.667f, length_scale = 1.0f / text->speed, noise_w = 0.8f;
  void *buffers[] = { phones.data(), &length,       tones.data(), &speaker_id,
                      &noise,        &length_scale, &noise_w };
  const std::size_t bytes[] = { phones.size() * sizeof(std::int64_t),
                                sizeof(length),
                                tones.size() * sizeof(std::int64_t),
                                sizeof(speaker_id),
                                sizeof(noise),
                                sizeof(length_scale),
                                sizeof(noise_w) };
  const char *names[] = { "x",           "x_lengths",    "tones",        "sid",
                          "noise_scale", "length_scale", "noise_scale_w" };
  const char *outputs[] = { "/Mul_10_output_0", "/Unsqueeze_6_output_0" };
  MeloValues values{ api_ };
  for (std::size_t index = 0; index < values.inputs.size(); ++index)
    {
      const bool sequence = index == 0 || index == 2;
      Check(api_->CreateTensorWithDataAsOrtValue(
          memory_, buffers[index], bytes[index], sequence ? dimensions : one,
          sequence ? 2 : 1,
          index < 4 ? ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64
                    : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
          &values.inputs[index]));
    }
  const OrtValue *input_values[7];
  for (std::size_t index = 0; index < 7; ++index)
    input_values[index] = values.inputs[index];
  Check(api_->Run(prefix_, nullptr, names, input_values, 7, outputs, 2,
                  values.latent.data()));
  if (stop())
    return Status::kCancelled;

  OrtTensorTypeAndShapeInfo *raw_shape = nullptr;
  Check(api_->GetTensorTypeAndShape(values.latent[0], &raw_shape));
  const auto release_shape = [&](OrtTensorTypeAndShapeInfo *shape) {
    api_->ReleaseTensorTypeAndShapeInfo(shape);
  };
  std::unique_ptr<OrtTensorTypeAndShapeInfo, decltype(release_shape)> shape(
      raw_shape, release_shape);
  std::size_t rank = 0;
  Check(api_->GetDimensionsCount(shape.get(), &rank));
  if (rank != 3)
    return Status::kBackendError;
  std::int64_t latent_shape[3];
  Check(api_->GetDimensions(shape.get(), latent_shape, 3));
  if (latent_shape[0] != 1 || latent_shape[1] != kChannels ||
      latent_shape[2] < 1)
    return Status::kBackendError;
  if (latent_shape[2] > static_cast<std::int64_t>(kFrames))
    return Status::kUnsupported;
  const auto frames = static_cast<std::size_t>(latent_shape[2]);
  raw_shape = nullptr;
  Check(api_->GetTensorTypeAndShape(values.latent[1], &raw_shape));
  std::unique_ptr<OrtTensorTypeAndShapeInfo, decltype(release_shape)>
      speaker_shape(raw_shape, release_shape);
  std::size_t speaker_count = 0;
  Check(api_->GetTensorShapeElementCount(speaker_shape.get(), &speaker_count));
  if (speaker_count != kSpeakerValues)
    return Status::kBackendError;
  float *latent = nullptr, *speaker = nullptr;
  Check(api_->GetTensorMutableData(values.latent[0],
                                   reinterpret_cast<void **>(&latent)));
  Check(api_->GetTensorMutableData(values.latent[1],
                                   reinterpret_cast<void **>(&speaker)));
  std::vector<float> packed(kFrames * kChannels, 0), mask(kFrames, 0);
  for (std::size_t frame = 0; frame < frames; ++frame)
    {
      mask[frame] = 1;
      for (std::size_t channel = 0; channel < kChannels; ++channel)
        packed[frame * kChannels + channel] = latent[channel * frames + frame];
    }
  npu_inputs_[latent_index_].buf = packed.data();
  npu_inputs_[speaker_index_].buf = speaker;
  npu_inputs_[mask_index_].buf = mask.data();
  if (stop())
    return Status::kCancelled;
  if (rknn_inputs_set(context_, 3, npu_inputs_.data()) ||
      rknn_run(context_, nullptr))
    return Status::kBackendError;
  MeloNpuOutput result{ context_ };
  result.output.want_float = 1;
  if (rknn_outputs_get(context_, 1, &result.output, nullptr))
    return Status::kBackendError;
  result.live = true;
  if (stop())
    return Status::kCancelled;
  if (!result.output.buf ||
      result.output.size != kFrames * kSamplesPerFrame * sizeof(float))
    return Status::kBackendError;
  const auto *samples = static_cast<const float *>(result.output.buf);
  PcmChunk pcm{
    std::vector<float>(samples, samples + frames * kSamplesPerFrame), 44100, 1
  };
  return emit(std::move(pcm)) ? Status::kOk : Status::kCancelled;
}

} // namespace

std::unique_ptr<Backend> CreateMeloBackend()
{
  return std::make_unique<MeloBackend>();
}

} // namespace nyamp::models
