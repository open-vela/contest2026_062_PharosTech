/****************************************************************************
 * tools/amp/vision/nyamp_identity_math.cpp
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
bool NormalizeEmbedding(Embedding *embedding)
{
  if (!embedding)
    return false;
  double norm = 0;
  for (float value : *embedding)
    {
      if (!std::isfinite(value))
        return false;
      norm += static_cast<double>(value) * value;
    }
  if (norm < 1e-12)
    return false;
  norm = std::sqrt(norm);
  for (float &value : *embedding)
    value /= norm;
  return true;
}

bool AlignFaceRgb(const std::vector<std::uint8_t> &bgr, const Face &face,
                  std::vector<std::uint8_t> *rgb)
{
  if (!rgb || bgr.size() != kModelSide * kModelSide * 3)
    return false;
  constexpr double target[10] = {
    38.2946, 51.6963, 73.5318, 51.5014, 56.0252,
    71.7366, 41.5493, 92.3655, 70.7299, 92.2041
  };
  double sx = 0, sy = 0, dx = 0, dy = 0;
  for (unsigned int i = 0; i < 5; i++)
    {
      double x = face.landmarks[i * 2], y = face.landmarks[i * 2 + 1];
      if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x >= kWidth ||
          y < 0 || y >= kHeight)
        return false;
      sx += x;
      sy += y;
      dx += target[i * 2];
      dy += target[i * 2 + 1];
    }
  sx /= 5;
  sy /= 5;
  dx /= 5;
  dy /= 5;
  double a = 0, b = 0, energy = 0;
  for (unsigned int i = 0; i < 5; i++)
    {
      double x = face.landmarks[i * 2] - sx,
             y = face.landmarks[i * 2 + 1] - sy;
      double u = target[i * 2] - dx, v = target[i * 2 + 1] - dy;
      a += x * u + y * v;
      b += x * v - y * u;
      energy += x * x + y * y;
    }
  if (energy < 1)
    return false;
  a /= energy;
  b /= energy;
  double determinant = a * a + b * b;
  if (determinant < 1e-9)
    return false;
  double tx = dx - a * sx + b * sy, ty = dy - b * sx - a * sy;
  rgb->assign(112 * 112 * 3, 0);
  for (unsigned int y = 0; y < 112; y++)
    for (unsigned int x = 0; x < 112; x++)
      {
        double u = (a * (x - tx) + b * (y - ty)) / determinant;
        double v = (-b * (x - tx) + a * (y - ty)) / determinant;
        if (u < 0 || v < 0 || u > kWidth - 1 || v > kHeight - 1)
          continue;
        unsigned int x0 = static_cast<unsigned int>(u),
                     y0 = static_cast<unsigned int>(v);
        unsigned int x1 = std::min(x0 + 1, kWidth - 1),
                     y1 = std::min(y0 + 1, kHeight - 1);
        double fx = u - x0, fy = v - y0;
        for (unsigned int channel = 0; channel < 3; channel++)
          {
            auto at = [&](unsigned int xx, unsigned int yy) {
              return bgr[(yy * kModelSide + xx) * 3 + 2 - channel];
            };
            double value =
                (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) +
                (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
            (*rgb)[(y * 112 + x) * 3 + channel] =
                static_cast<std::uint8_t>(std::clamp(value + .5, 0.0, 255.0));
          }
      }
  return true;
}
} // namespace nyamp::vision
