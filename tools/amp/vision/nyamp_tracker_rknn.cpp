/****************************************************************************
 * tools/amp/vision/nyamp_tracker_rknn.cpp
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

#include "nyamp_tracker.h"
#include <algorithm>
#include <cstring>
#include <rknn_api.h>

namespace nyamp::vision
{
namespace
{
class RknnTrackerBackend final : public TrackerBackend
{
public:
  ~RknnTrackerBackend() override { Close(); }
  bool Load(const std::string &directory) override;
  bool Extract(const std::vector<std::uint8_t> &crop, bool exemplar,
               TrackFeature *feature) override;
  bool Match(const TrackFeature &exemplar, const TrackFeature &search,
             std::vector<float> *scores, std::vector<float> *boxes) override;

private:
  void Close();
  bool Outputs(unsigned int model, std::vector<float> *first,
               std::vector<float> *second = nullptr);
  std::array<rknn_context, 3> contexts_{};
  std::array<std::array<unsigned int, 2>, 3> input_indices_{};
  std::array<std::array<rknn_tensor_attr, 2>, 3> output_attrs_{};
};
void RknnTrackerBackend::Close()
{
  for (auto &context : contexts_)
    {
      if (context)
        rknn_destroy(context);
      context = 0;
    }
}
bool RknnTrackerBackend::Load(const std::string &directory)
{
  Close();
  const char *names[] = { "template.rknn", "search.rknn", "head.rknn" };
  const unsigned int inputs[][2] = { { 127 * 127 * 3, 0 },
                                     { 255 * 255 * 3, 0 },
                                     { 48 * 8 * 8, 48 * 16 * 16 } };
  const unsigned int outputs[][2] = { { 48 * 8 * 8, 0 },
                                      { 48 * 16 * 16, 0 },
                                      { 2 * 16 * 16, 4 * 16 * 16 } };
  for (unsigned int model = 0; model < 3; model++)
    {
      std::string path = directory + "/" + names[model];
      if (rknn_init(&contexts_[model], const_cast<char *>(path.c_str()), 0, 0,
                    nullptr))
        {
          Close();
          return false;
        }
      rknn_input_output_num counts{};
      unsigned int count = model == 2 ? 2 : 1;
      if (rknn_query(contexts_[model], RKNN_QUERY_IN_OUT_NUM, &counts,
                     sizeof(counts)) ||
          counts.n_input != count || counts.n_output != count)
        {
          Close();
          return false;
        }
      std::array<bool, 2> input_seen{}, output_seen{};
      for (unsigned int i = 0; i < count; i++)
        {
          rknn_tensor_attr input{}, output{};
          input.index = output.index = i;
          if (rknn_query(contexts_[model], RKNN_QUERY_INPUT_ATTR, &input,
                         sizeof(input)) ||
              rknn_query(contexts_[model], RKNN_QUERY_OUTPUT_ATTR, &output,
                         sizeof(output)))
            {
              Close();
              return false;
            }
          unsigned int in_slot = count, out_slot = count;
          for (unsigned int slot = 0; slot < count; slot++)
            {
              if (input.n_elems == inputs[model][slot])
                in_slot = slot;
              if (output.n_elems == outputs[model][slot])
                out_slot = slot;
            }
          if (in_slot == count || out_slot == count || input_seen[in_slot] ||
              output_seen[out_slot] || output.n_dims != 4 ||
              (output.fmt != RKNN_TENSOR_NCHW &&
               output.fmt != RKNN_TENSOR_NHWC))
            {
              Close();
              return false;
            }
          input_seen[in_slot] = output_seen[out_slot] = true;
          input_indices_[model][in_slot] = i;
          output_attrs_[model][out_slot] = output;
        }
    }
  return true;
}
bool RknnTrackerBackend::Outputs(unsigned int model, std::vector<float> *first,
                                 std::vector<float> *second)
{
  std::array<rknn_output, 2> outputs{};
  unsigned int count = second ? 2 : 1;
  for (unsigned int i = 0; i < count; i++)
    {
      outputs[i].index = output_attrs_[model][i].index;
      outputs[i].want_float = 1;
    }
  if (rknn_outputs_get(contexts_[model], count, outputs.data(), nullptr))
    return false;
  bool ok = true;
  for (unsigned int i = 0; i < count; i++)
    {
      if (!outputs[i].buf || outputs[i].size % sizeof(float))
        {
          ok = false;
          break;
        }
      auto *target = i ? second : first;
      const auto *data = static_cast<const float *>(outputs[i].buf);
      const auto &attr = output_attrs_[model][i];
      if (outputs[i].size != attr.n_elems * sizeof(float))
        {
          ok = false;
          break;
        }
      target->resize(attr.n_elems);
      if (attr.fmt == RKNN_TENSOR_NCHW)
        std::copy(data, data + attr.n_elems, target->begin());
      else
        {
          const unsigned int height = attr.dims[1], width = attr.dims[2],
                             channels = attr.dims[3];
          if (attr.dims[0] != 1 || height * width * channels != attr.n_elems)
            {
              ok = false;
              break;
            }
          for (unsigned int y = 0; y < height; y++)
            for (unsigned int x = 0; x < width; x++)
              for (unsigned int c = 0; c < channels; c++)
                (*target)[(c * height + y) * width + x] =
                    data[(y * width + x) * channels + c];
        }
    }
  rknn_outputs_release(contexts_[model], count, outputs.data());
  return ok;
}
bool RknnTrackerBackend::Extract(const std::vector<std::uint8_t> &crop,
                                 bool exemplar, TrackFeature *feature)
{
  unsigned int model = exemplar ? 0 : 1, side = exemplar ? 127 : 255;
  if (!contexts_[model] || !feature || crop.size() != side * side * 3)
    return false;
  rknn_input input{};
  input.index = input_indices_[model][0];
  input.type = RKNN_TENSOR_UINT8;
  input.fmt = RKNN_TENSOR_NHWC;
  input.buf = const_cast<std::uint8_t *>(crop.data());
  input.size = crop.size();
  return !rknn_inputs_set(contexts_[model], 1, &input) &&
         !rknn_run(contexts_[model], nullptr) && Outputs(model, feature);
}
bool RknnTrackerBackend::Match(const TrackFeature &exemplar,
                               const TrackFeature &search,
                               std::vector<float> *scores,
                               std::vector<float> *boxes)
{
  if (!contexts_[2] || exemplar.size() != 48 * 8 * 8 ||
      search.size() != 48 * 16 * 16 || !scores || !boxes)
    return false;
  std::array<rknn_input, 2> inputs{};
  for (unsigned int i = 0; i < 2; i++)
    {
      const auto &values = i ? search : exemplar;
      inputs[i].index = input_indices_[2][i];
      inputs[i].type = RKNN_TENSOR_FLOAT32;
      inputs[i].fmt = RKNN_TENSOR_NCHW;
      inputs[i].buf = const_cast<float *>(values.data());
      inputs[i].size = values.size() * sizeof(float);
    }
  return !rknn_inputs_set(contexts_[2], 2, inputs.data()) &&
         !rknn_run(contexts_[2], nullptr) && Outputs(2, scores, boxes);
}
} // namespace
std::unique_ptr<Tracker> CreateRknnTracker()
{
  return std::make_unique<Tracker>(std::make_unique<RknnTrackerBackend>());
}
} // namespace nyamp::vision
