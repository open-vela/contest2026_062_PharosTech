/****************************************************************************
 * tools/amp/nyampd/nyampd_speaker_sherpa.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#include "nyamp_speaker.h"
#include "nyampd_speaker.h"
#include <algorithm>
#include <cctype>
#include <fstream>
namespace nyamp
{
namespace
{
class SherpaSpeaker final : public SpeakerBackend
{
public:
  models::Status Load(const std::string &path) override;
  void ResetEnrollment() override { enrollment_.Clear(); }
  models::Status Add(const float *samples, std::size_t count,
                     const models::Stop &stop, std::uint32_t *takes) override;
  models::Status Commit(std::vector<std::uint8_t> *bytes, std::uint32_t *kept,
                        float *cohesion) override;
  models::Status Put(const std::uint8_t *bytes, std::size_t size) override;
  void Forget() override
  {
    std::fill(reference_.begin(), reference_.end(), 0);
    reference_.clear();
    enrollment_.Clear();
  }
  models::Status Verify(const float *samples, std::size_t count,
                        const models::Stop &stop, float *score) override;

private:
  voice::SpeakerEmbedder embedder_;
  voice::Enrolment enrollment_;
  voice::EnrolPolicy policy_;
  voice::Embedding reference_;
  std::uint32_t tag_ = 0;
};
models::Status SherpaSpeaker::Load(const std::string &path)
{
  std::ifstream marker(path + "/model.onnx.sha256");
  std::string digest;
  marker >> digest;
  if (digest.size() != 64 ||
      !std::all_of(digest.begin(), digest.end(),
                   [](unsigned char c) { return std::isxdigit(c); }))
    return models::Status::kNotReady;
  tag_ =
      static_cast<std::uint32_t>(std::stoul(digest.substr(0, 8), nullptr, 16));
  voice::SpeakerConfig config;
  config.model_directory = path;
  config.model_filename = "model.onnx";
  config.num_threads = 1;
  auto status = embedder_.Load(config);
  if (status == models::Status::kOk && embedder_.dim() != 192)
    {
      embedder_.Unload();
      return models::Status::kUnsupported;
    }
  return status;
}
models::Status SherpaSpeaker::Add(const float *samples, std::size_t count,
                                  const models::Stop &stop,
                                  std::uint32_t *takes)
{
  voice::Embedding embedding;
  auto status = embedder_.Embed(samples, count, &embedding, stop);
  if (status == models::Status::kOk)
    status = enrollment_.Add(embedding, policy_);
  *takes = enrollment_.size();
  return status;
}
models::Status SherpaSpeaker::Commit(std::vector<std::uint8_t> *bytes,
                                     std::uint32_t *kept, float *cohesion)
{
  auto result = enrollment_.Commit(policy_);
  if (result.status != models::Status::kOk)
    return result.status;
  *kept = std::count(result.kept.begin(), result.kept.end(), true);
  *cohesion = result.cohesion;
  *bytes = voice::PackTemplate(result.voiceprint,
                               { tag_, static_cast<std::uint16_t>(*kept) });
  return bytes->empty() ? models::Status::kBackendError : models::Status::kOk;
}
models::Status SherpaSpeaker::Put(const std::uint8_t *bytes, std::size_t size)
{
  voice::Embedding next;
  voice::TemplateInfo info;
  auto status = voice::UnpackTemplate(bytes, size, &next, &info);
  if (status != models::Status::kOk)
    return status;
  if (info.model_tag != tag_ || next.size() != 192)
    return models::Status::kUnsupported;
  reference_ = std::move(next);
  return models::Status::kOk;
}
models::Status SherpaSpeaker::Verify(const float *samples, std::size_t count,
                                     const models::Stop &stop, float *score)
{
  if (reference_.empty())
    return models::Status::kNotReady;
  voice::Embedding embedding;
  auto status = embedder_.Embed(samples, count, &embedding, stop);
  if (status == models::Status::kOk)
    *score = voice::Verify(reference_, embedding);
  return status;
}
}
std::unique_ptr<SpeakerBackend> CreateSherpaSpeakerBackend()
{
  return std::make_unique<SherpaSpeaker>();
}
}
