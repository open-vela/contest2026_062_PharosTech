/****************************************************************************
 * tools/amp/vision/nyamp_vision.h
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

#ifndef __TOOLS_AMP_VISION_NYAMP_VISION_H
#define __TOOLS_AMP_VISION_NYAMP_VISION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nyamp::vision
{
constexpr std::uint32_t kWidth = 640;
constexpr std::uint32_t kHeight = 480;
constexpr std::uint32_t kModelSide = 640;
constexpr std::size_t kNv12Bytes = kWidth * kHeight * 3 / 2;
constexpr std::size_t kMaxFaces = 5;
constexpr std::size_t kEmbeddingSize = 128;
using Embedding = std::array<float, kEmbeddingSize>;

struct Face
{
  float x, y, width, height, score;
  std::array<float, 10> landmarks;
};

struct Tensor
{
  const float *data = nullptr;
  std::size_t count = 0;
};

/* Fixed VGA NV12, limited-range BT.601, unmirrored. Padding is black;
 * no resize or stretch changes the camera coordinate system.
 */
bool Nv12ToBgr(const std::uint8_t *nv12, std::size_t bytes,
               std::vector<std::uint8_t> *bgr);
bool DecodeYunet(const std::array<Tensor, 12> &outputs,
                 std::vector<Face> *faces, float threshold = 0.7f);

bool AlignFaceRgb(const std::vector<std::uint8_t> &bgr, const Face &face,
                  std::vector<std::uint8_t> *rgb);
bool NormalizeEmbedding(Embedding *embedding);

class Detector
{
public:
  virtual ~Detector() = default;
  virtual bool Load(const std::string &directory) = 0;
  virtual void ClearFrame() {}
  virtual bool SupportsIdentity() const { return false; }
  virtual bool LoadIdentity(const std::string &) { return false; }
  virtual bool CanEmbed() const { return false; }
  virtual bool Embed(const std::uint8_t *, std::size_t, const Face &,
                     Embedding *)
  {
    return false;
  }
  virtual bool Detect(const std::uint8_t *nv12, std::size_t bytes,
                      std::vector<Face> *faces) = 0;
};

std::unique_ptr<Detector> CreateRknnDetector();
} // namespace nyamp::vision
#endif
