/****************************************************************************
 * tools/amp/voice/nyamp_speaker.cpp
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

#include "nyamp_speaker.h"

#include "c-api.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nyamp::voice
{
namespace
{

bool Normalize(Embedding *embedding)
{
  double energy = 0.0;
  for (float value : *embedding)
    {
      if (!std::isfinite(value))
        {
          return false;
        }
      energy += static_cast<double>(value) * value;
    }
  if (!(energy > 0.0))
    {
      return false;
    }
  const float scale = static_cast<float>(1.0 / std::sqrt(energy));
  for (float &value : *embedding)
    {
      value *= scale;
    }
  return true;
}

// Software half-float conversion: the compute domain is AArch64 and the
// host tests run on x86_64, and a voiceprint written by one must be read
// bit-exactly by the other, so no compiler-specific _Float16 is used.

std::uint16_t ToHalf(float value)
{
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const std::uint32_t sign = (bits >> 16) & 0x8000U;
  const std::int32_t exponent =
      static_cast<std::int32_t>((bits >> 23) & 0xffU) - 127 + 15;
  std::uint32_t mantissa = bits & 0x7fffffU;
  if (exponent >= 31)
    {
      return static_cast<std::uint16_t>(sign | 0x7bffU); // Clamp, never inf.
    }
  if (exponent <= 0)
    {
      if (exponent < -10)
        {
          return static_cast<std::uint16_t>(sign);
        }
      mantissa |= 0x800000U;
      const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
      const std::uint32_t rounded = (mantissa + (1U << (shift - 1))) >> shift;
      return static_cast<std::uint16_t>(sign | rounded);
    }
  const std::uint32_t rounded =
      (static_cast<std::uint32_t>(exponent) << 10) +
      ((mantissa + 0x1000U) >> 13); // A mantissa carry bumps the exponent.
  return static_cast<std::uint16_t>(sign | std::min(rounded, 0x7bffU));
}

float FromHalf(std::uint16_t half)
{
  const std::uint32_t sign = (half & 0x8000U) << 16;
  std::uint32_t exponent = (half >> 10) & 0x1fU;
  std::uint32_t mantissa = half & 0x3ffU;
  std::uint32_t bits;
  if (exponent == 0)
    {
      if (mantissa == 0)
        {
          bits = sign;
        }
      else
        {
          exponent = 127 - 15 + 1;
          while ((mantissa & 0x400U) == 0)
            {
              mantissa <<= 1;
              --exponent;
            }
          bits = sign | (exponent << 23) | ((mantissa & 0x3ffU) << 13);
        }
    }
  else
    {
      bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void PutU16(std::vector<std::uint8_t> *out, std::uint16_t value)
{
  out->push_back(static_cast<std::uint8_t>(value & 0xffU));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void PutU32(std::vector<std::uint8_t> *out, std::uint32_t value)
{
  PutU16(out, static_cast<std::uint16_t>(value & 0xffffU));
  PutU16(out, static_cast<std::uint16_t>(value >> 16));
}

std::uint16_t GetU16(const std::uint8_t *bytes)
{
  return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t GetU32(const std::uint8_t *bytes)
{
  return static_cast<std::uint32_t>(GetU16(bytes)) |
         (static_cast<std::uint32_t>(GetU16(bytes + 2)) << 16);
}

} // namespace

struct SpeakerEmbedder::Impl
{
  const SherpaOnnxSpeakerEmbeddingExtractor *extractor = nullptr;
  std::size_t dim = 0;
};

SpeakerEmbedder::SpeakerEmbedder() : impl_(std::make_unique<Impl>()) {}

SpeakerEmbedder::~SpeakerEmbedder() { Unload(); }

Status SpeakerEmbedder::Load(const SpeakerConfig &config)
{
  if (impl_->extractor)
    {
      return Status::kBusy;
    }
  if (config.model_directory.empty() || config.num_threads < 1)
    {
      return Status::kInvalid;
    }
  const std::string model = config.model_directory + "/speaker.onnx";
  SherpaOnnxSpeakerEmbeddingExtractorConfig native{};
  native.model = model.c_str();
  native.num_threads = config.num_threads;
  native.provider = "cpu";
  impl_->extractor = SherpaOnnxCreateSpeakerEmbeddingExtractor(&native);
  if (!impl_->extractor)
    {
      return Status::kBackendError;
    }
  const std::int32_t dim =
      SherpaOnnxSpeakerEmbeddingExtractorDim(impl_->extractor);
  if (dim <= 0 || dim > 0xffff)
    {
      Unload();
      return Status::kBackendError;
    }
  impl_->dim = static_cast<std::size_t>(dim);
  return Status::kOk;
}

void SpeakerEmbedder::Unload()
{
  if (impl_->extractor)
    {
      SherpaOnnxDestroySpeakerEmbeddingExtractor(impl_->extractor);
      impl_->extractor = nullptr;
    }
  impl_->dim = 0;
}

std::size_t SpeakerEmbedder::dim() const { return impl_->dim; }

Status SpeakerEmbedder::Embed(const float *samples, std::size_t count,
                              Embedding *embedding, const Stop &stop)
{
  if (!impl_->extractor)
    {
      return Status::kNotReady;
    }
  if (!samples || !embedding || count < kSpeakerMinSamples ||
      count > kSpeakerMaxSamples)
    {
      return Status::kInvalid;
    }
  if (stop && stop())
    {
      return Status::kCancelled;
    }
  const auto destroy_stream = [](const SherpaOnnxOnlineStream *stream) {
    SherpaOnnxDestroyOnlineStream(stream);
  };
  std::unique_ptr<const SherpaOnnxOnlineStream, decltype(destroy_stream)>
      stream(SherpaOnnxSpeakerEmbeddingExtractorCreateStream(impl_->extractor),
             destroy_stream);
  if (!stream)
    {
      return Status::kBackendError;
    }
  SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), kSampleRate, samples,
                                       static_cast<std::int32_t>(count));
  SherpaOnnxOnlineStreamInputFinished(stream.get());
  if (!SherpaOnnxSpeakerEmbeddingExtractorIsReady(impl_->extractor,
                                                  stream.get()))
    {
      return Status::kInvalid;
    }
  const float *raw = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(
      impl_->extractor, stream.get());
  if (!raw)
    {
      return Status::kBackendError;
    }
  Embedding result(raw, raw + impl_->dim);
  SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(raw);
  if (stop && stop())
    {
      return Status::kCancelled;
    }
  if (!Normalize(&result))
    {
      return Status::kBackendError;
    }
  *embedding = std::move(result);
  return Status::kOk;
}

float Cosine(const Embedding &a, const Embedding &b)
{
  if (a.empty() || a.size() != b.size())
    {
      return -2.0f;
    }
  double dot = 0.0, aa = 0.0, bb = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i)
    {
      dot += static_cast<double>(a[i]) * b[i];
      aa += static_cast<double>(a[i]) * a[i];
      bb += static_cast<double>(b[i]) * b[i];
    }
  if (!(aa > 0.0) || !(bb > 0.0) || !std::isfinite(dot))
    {
      return -2.0f;
    }
  return static_cast<float>(dot / std::sqrt(aa * bb));
}

float Verify(const Embedding &voiceprint, const Embedding &probe)
{
  return Cosine(voiceprint, probe);
}

Status Enrolment::Add(const Embedding &embedding, const EnrolPolicy &policy)
{
  if (segments_.size() >= policy.max_segments)
    {
      return Status::kBusy;
    }
  if (embedding.empty() ||
      (!segments_.empty() && segments_.front().size() != embedding.size()))
    {
      return Status::kInvalid;
    }
  Embedding unit = embedding;
  if (!Normalize(&unit))
    {
      return Status::kInvalid;
    }
  segments_.push_back(std::move(unit));
  return Status::kOk;
}

void Enrolment::Clear() { segments_.clear(); }

std::size_t Enrolment::size() const { return segments_.size(); }

EnrolResult Enrolment::Commit(const EnrolPolicy &policy) const
{
  EnrolResult result;
  const std::size_t count = segments_.size();
  result.kept.assign(count, true);
  if (count < policy.min_kept || policy.min_kept < 2)
    {
      // Consistency is a pairwise property, so a policy that accepts a
      // single take is a caller bug, while too few takes so far is simply
      // an unfinished dialogue.
      result.kept.assign(count, false);
      result.status =
          policy.min_kept < 2 ? Status::kInvalid : Status::kNotReady;
      return result;
    }
  std::vector<float> pair(count * count, 1.0f);
  for (std::size_t i = 0; i < count; ++i)
    {
      for (std::size_t j = i + 1; j < count; ++j)
        {
          pair[i * count + j] = pair[j * count + i] =
              Cosine(segments_[i], segments_[j]);
        }
    }

  // Drop the single least consistent take and re-evaluate, instead of
  // cutting everything under the bar at once: one foreign take drags down
  // the mean of every genuine take, and removing it first lets the genuine
  // ones recover above the threshold.
  std::size_t active = count;
  for (;;)
    {
      std::size_t worst = count;
      float worst_mean = 2.0f;
      std::vector<float> means;
      for (std::size_t i = 0; i < count; ++i)
        {
          if (!result.kept[i])
            {
              continue;
            }
          float sum = 0.0f;
          for (std::size_t j = 0; j < count; ++j)
            {
              if (j != i && result.kept[j])
                {
                  sum += pair[i * count + j];
                }
            }
          const float mean = sum / static_cast<float>(active - 1);
          means.push_back(mean);
          if (mean < worst_mean)
            {
              worst_mean = mean;
              worst = i;
            }
        }
      std::sort(means.begin(), means.end());
      const float median = means[means.size() / 2];
      const bool below_bar = worst_mean < policy.min_mean_cosine;
      const bool straggler = policy.max_below_median > 0.0f &&
                             worst_mean < median - policy.max_below_median;
      if (!below_bar && !straggler)
        {
          break;
        }
      if (active <= policy.min_kept)
        {
          if (!below_bar)
            {
              break;
            }

          // Too little agreement left to tell who the owner is; the dialogue
          // must ask for more takes rather than store a blurred voiceprint.
          result.status = Status::kInvalid;
          return result;
        }
      result.kept[worst] = false;
      --active;
    }

  Embedding mean(segments_.front().size(), 0.0f);
  float cohesion = 0.0f;
  std::size_t pairs = 0;
  for (std::size_t i = 0; i < count; ++i)
    {
      if (!result.kept[i])
        {
          continue;
        }
      for (std::size_t k = 0; k < mean.size(); ++k)
        {
          mean[k] += segments_[i][k];
        }
      for (std::size_t j = i + 1; j < count; ++j)
        {
          if (result.kept[j])
            {
              cohesion += pair[i * count + j];
              ++pairs;
            }
        }
    }
  if (!Normalize(&mean))
    {
      result.status = Status::kBackendError;
      return result;
    }
  result.voiceprint = std::move(mean);
  result.cohesion = pairs ? cohesion / static_cast<float>(pairs) : 0.0f;
  result.status = Status::kOk;
  return result;
}

std::vector<std::uint8_t> PackTemplate(const Embedding &voiceprint,
                                       const TemplateInfo &info)
{
  std::vector<std::uint8_t> out;
  if (voiceprint.empty() || voiceprint.size() > 0xffffU)
    {
      return out;
    }
  out.reserve(kTemplateHeaderSize + voiceprint.size() * 2);
  PutU32(&out, kTemplateMagic);
  PutU16(&out, kTemplateVersion);
  PutU16(&out, static_cast<std::uint16_t>(voiceprint.size()));
  PutU32(&out, info.model_tag);
  PutU16(&out, info.segments);
  PutU16(&out, 0);
  for (float value : voiceprint)
    {
      PutU16(&out, ToHalf(value));
    }
  return out;
}

Status UnpackTemplate(const std::uint8_t *bytes, std::size_t size,
                      Embedding *voiceprint, TemplateInfo *info)
{
  if (!bytes || !voiceprint || !info || size < kTemplateHeaderSize ||
      GetU32(bytes) != kTemplateMagic)
    {
      return Status::kInvalid;
    }
  if (GetU16(bytes + 4) != kTemplateVersion)
    {
      return Status::kUnsupported;
    }
  const std::size_t dim = GetU16(bytes + 6);
  if (dim == 0 || size != kTemplateHeaderSize + dim * 2 ||
      GetU16(bytes + 14) != 0)
    {
      return Status::kInvalid;
    }
  Embedding result(dim);
  for (std::size_t i = 0; i < dim; ++i)
    {
      result[i] = FromHalf(GetU16(bytes + kTemplateHeaderSize + i * 2));
    }

  // Re-normalize: rounding to half precision moves the norm slightly off
  // one, and an all-zero payload must be refused rather than scored.
  if (!Normalize(&result))
    {
      return Status::kInvalid;
    }
  info->model_tag = GetU32(bytes + 8);
  info->segments = GetU16(bytes + 12);
  *voiceprint = std::move(result);
  return Status::kOk;
}

} // namespace nyamp::voice
