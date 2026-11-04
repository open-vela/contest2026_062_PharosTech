/****************************************************************************
 * tools/amp/vision/nyamp_vision_rknn.cpp
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

#include "nyamp_vision.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <rknn_api.h>

namespace nyamp::vision
{
namespace
{
class RknnDetector final : public Detector
{
public:
  ~RknnDetector() override { Close(); }
  bool Load(const std::string &directory) override;
  void ClearFrame() override { std::fill(input_.begin(), input_.end(), 0); }
  bool SupportsIdentity() const override { return true; }
  bool LoadIdentity(const std::string &directory) override;
  bool CanEmbed() const override { return identity_ != 0; }
  bool Embed(const std::uint8_t *nv12, std::size_t bytes, const Face &face,
             Embedding *embedding) override;
  bool Detect(const std::uint8_t *nv12, std::size_t bytes,
              std::vector<Face> *faces) override;

private:
  void Close();
  rknn_context context_ = 0;
  rknn_context identity_ = 0;
  std::array<rknn_tensor_attr, 12> attrs_{};
  std::array<unsigned int, 12> indices_{};
  std::vector<std::uint8_t> input_;
};

void RknnDetector::Close()
{
  if (context_)
    rknn_destroy(context_);
  context_ = 0;
  if (identity_)
    rknn_destroy(identity_);
  identity_ = 0;
}

bool RknnDetector::Load(const std::string &directory)
{
  Close();
  const std::string path = directory + "/yunet.rknn";
  if (rknn_init(&context_, const_cast<char *>(path.c_str()), 0, 0, nullptr))
    {
      context_ = 0;
      return false;
    }
  rknn_input_output_num io{};
  rknn_tensor_attr input{};
  if (rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) ||
      io.n_input != 1 || io.n_output != attrs_.size() ||
      rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input)) ||
      input.n_elems != kModelSide * kModelSide * 3)
    {
      Close();
      return false;
    }
  std::array<bool, 12> seen{};
  for (unsigned int i = 0; i < attrs_.size(); ++i)
    {
      attrs_[i] = {};
      attrs_[i].index = i;
      if (rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &attrs_[i],
                     sizeof(attrs_[i])))
        {
          Close();
          return false;
        }
      unsigned int found = 12;
      const char *prefix[] = { "cls", "obj", "bbox", "kps" };
      for (unsigned int kind = 0; kind < 4; ++kind)
        for (unsigned int level = 0; level < 3; ++level)
          {
            char name[24];
            std::snprintf(name, sizeof(name), "%s_%u", prefix[kind],
                          8U << level);
            if (!std::strcmp(name, attrs_[i].name))
              found = kind * 3 + level;
          }
      if (found == 12 || seen[found])
        {
          Close();
          return false;
        }
      seen[found] = true;
      indices_[found] = i;
    }
  return true;
}

bool RknnDetector::Detect(const std::uint8_t *nv12, std::size_t bytes,
                          std::vector<Face> *faces)
{
  if (!context_ || !Nv12ToBgr(nv12, bytes, &input_))
    return false;
  rknn_input input{};
  input.index = 0;
  input.type = RKNN_TENSOR_UINT8;
  input.fmt = RKNN_TENSOR_NHWC;
  input.buf = input_.data();
  input.size = input_.size();
  if (rknn_inputs_set(context_, 1, &input) || rknn_run(context_, nullptr))
    return false;
  std::array<rknn_output, 12> outputs{};
  for (unsigned int i = 0; i < outputs.size(); ++i)
    {
      outputs[i].index = i;
      outputs[i].want_float = 1;
    }
  if (rknn_outputs_get(context_, outputs.size(), outputs.data(), nullptr))
    return false;
  std::array<Tensor, 12> tensors{};
  for (unsigned int i = 0; i < tensors.size(); ++i)
    tensors[i] = { static_cast<const float *>(outputs[indices_[i]].buf),
                   outputs[indices_[i]].size / sizeof(float) };
  bool ok = DecodeYunet(tensors, faces);
  rknn_outputs_release(context_, outputs.size(), outputs.data());
  return ok;
}
bool RknnDetector::LoadIdentity(const std::string &directory)
{
  if (identity_)
    rknn_destroy(identity_);
  identity_ = 0;
  const std::string path = directory + "/sface.rknn";
  if (rknn_init(&identity_, const_cast<char *>(path.c_str()), 0, 0, nullptr))
    {
      identity_ = 0;
      return false;
    }
  rknn_input_output_num io{};
  rknn_tensor_attr input{}, output{};
  bool ok =
      !rknn_query(identity_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) &&
      io.n_input == 1 && io.n_output == 1 &&
      !rknn_query(identity_, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input)) &&
      !rknn_query(identity_, RKNN_QUERY_OUTPUT_ATTR, &output,
                  sizeof(output)) &&
      input.n_elems == 112 * 112 * 3 && output.n_elems == kEmbeddingSize;
  if (!ok)
    {
      rknn_destroy(identity_);
      identity_ = 0;
    }
  return ok;
}

bool RknnDetector::Embed(const std::uint8_t *nv12, std::size_t bytes,
                         const Face &face, Embedding *embedding)
{
  if (!identity_ || !embedding || !Nv12ToBgr(nv12, bytes, &input_))
    return false;
  std::vector<std::uint8_t> rgb;
  if (!AlignFaceRgb(input_, face, &rgb))
    return false;
  rknn_input input{};
  input.type = RKNN_TENSOR_UINT8;
  input.fmt = RKNN_TENSOR_NHWC;
  input.buf = rgb.data();
  input.size = rgb.size();
  if (rknn_inputs_set(identity_, 1, &input) || rknn_run(identity_, nullptr))
    return false;
  rknn_output output{};
  output.want_float = 1;
  if (rknn_outputs_get(identity_, 1, &output, nullptr))
    return false;
  bool ok = output.buf && output.size == kEmbeddingSize * sizeof(float);
  if (ok)
    std::memcpy(embedding->data(), output.buf, output.size);
  rknn_outputs_release(identity_, 1, &output);
  return ok && NormalizeEmbedding(embedding);
}

} // namespace

std::unique_ptr<Detector> CreateRknnDetector()
{
  return std::make_unique<RknnDetector>();
}
} // namespace nyamp::vision
