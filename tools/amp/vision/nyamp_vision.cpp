/****************************************************************************
 * tools/amp/vision/nyamp_vision.cpp
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

namespace nyamp::vision
{
namespace
{
std::uint8_t Byte(int value);
float Overlap(const Face &a, const Face &b);

std::uint8_t Byte(int value)
{
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

float Overlap(const Face &a, const Face &b)
{
  float w = std::max(0.0f, std::min(a.x + a.width, b.x + b.width) -
                               std::max(a.x, b.x));
  float h = std::max(0.0f, std::min(a.y + a.height, b.y + b.height) -
                               std::max(a.y, b.y));
  float intersection = w * h;
  float total = a.width * a.height + b.width * b.height - intersection;
  return total > 0 ? intersection / total : 0;
}
} // namespace

bool Nv12ToBgr(const std::uint8_t *nv12, std::size_t bytes,
               std::vector<std::uint8_t> *bgr)
{
  if (!nv12 || !bgr || bytes != kNv12Bytes)
    return false;
  bgr->assign(kModelSide * kModelSide * 3, 0);
  for (std::uint32_t y = 0; y < kHeight; ++y)
    for (std::uint32_t x = 0; x < kWidth; ++x)
      {
        int yy = std::max(0, static_cast<int>(nv12[y * kWidth + x]) - 16);
        const std::size_t uv = kWidth * kHeight + (y / 2) * kWidth + (x & ~1U);
        int u = static_cast<int>(nv12[uv]) - 128;
        int v = static_cast<int>(nv12[uv + 1]) - 128;
        std::size_t at = (y * kModelSide + x) * 3;
        (*bgr)[at] = Byte((298 * yy + 516 * u + 128) >> 8);
        (*bgr)[at + 1] = Byte((298 * yy - 100 * u - 208 * v + 128) >> 8);
        (*bgr)[at + 2] = Byte((298 * yy + 409 * v + 128) >> 8);
      }
  return true;
}

bool DecodeYunet(const std::array<Tensor, 12> &outputs,
                 std::vector<Face> *faces, float threshold)
{
  if (!faces || !std::isfinite(threshold) || threshold <= 0 || threshold > 1)
    return false;
  faces->clear();
  std::vector<Face> candidates;
  for (unsigned int level = 0; level < 3; ++level)
    {
      const unsigned int stride = 8U << level;
      const unsigned int columns = kModelSide / stride;
      const std::size_t cells = columns * columns;
      const Tensor &cls = outputs[level], &obj = outputs[level + 3];
      const Tensor &box = outputs[level + 6], &points = outputs[level + 9];
      if (!cls.data || !obj.data || !box.data || !points.data ||
          cls.count != cells || obj.count != cells || box.count != cells * 4 ||
          points.count != cells * 10)
        return false;
      for (std::size_t index = 0; index < cells; ++index)
        {
          if (!std::isfinite(cls.data[index]) ||
              !std::isfinite(obj.data[index]))
            continue;
          float score = std::sqrt(std::clamp(cls.data[index], 0.0f, 1.0f) *
                                  std::clamp(obj.data[index], 0.0f, 1.0f));
          if (score < threshold)
            continue;
          const float *b = box.data + index * 4;
          const float *p = points.data + index * 10;
          if (!std::all_of(b, b + 4,
                           [](float n) { return std::isfinite(n); }) ||
              !std::all_of(p, p + 10,
                           [](float n) { return std::isfinite(n); }))
            continue;
          float col = index % columns, row = index / columns;
          float w = std::exp(b[2]) * stride, h = std::exp(b[3]) * stride;
          float cx = (col + b[0]) * stride, cy = (row + b[1]) * stride;
          if (!std::isfinite(w) || !std::isfinite(h))
            continue;
          float x = std::clamp(cx - w / 2, 0.0f, static_cast<float>(kWidth));
          float y = std::clamp(cy - h / 2, 0.0f, static_cast<float>(kHeight));
          float right =
              std::clamp(cx + w / 2, 0.0f, static_cast<float>(kWidth));
          float bottom =
              std::clamp(cy + h / 2, 0.0f, static_cast<float>(kHeight));
          if (right <= x || bottom <= y)
            continue;
          Face face{ x, y, right - x, bottom - y, score, {} };
          for (unsigned int point = 0; point < 5; ++point)
            {
              face.landmarks[point * 2] = (p[point * 2] + col) * stride;
              face.landmarks[point * 2 + 1] =
                  (p[point * 2 + 1] + row) * stride;
            }
          candidates.push_back(face);
        }
    }
  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const Face &a, const Face &b) { return a.score > b.score; });
  for (const Face &face : candidates)
    {
      if (std::none_of(faces->begin(), faces->end(),
                       [&face](const Face &prior) {
                         return Overlap(face, prior) > 0.3f;
                       }))
        faces->push_back(face);
      if (faces->size() == kMaxFaces)
        break;
    }
  return true;
}
} // namespace nyamp::vision
