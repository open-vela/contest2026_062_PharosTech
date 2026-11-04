/****************************************************************************
 * tools/amp/vision/nyamp_vision_test.cpp
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
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace nyamp::vision;
int main()
{
  std::vector<std::uint8_t> frame(kNv12Bytes, 128), bgr;
  std::fill(frame.begin(), frame.begin() + kWidth * kHeight, 16);
  assert(Nv12ToBgr(frame.data(), frame.size(), &bgr));
  for (auto value : bgr)
    assert(value == 0);
  std::fill(frame.begin(), frame.begin() + kWidth * kHeight, 235);
  assert(Nv12ToBgr(frame.data(), frame.size(), &bgr));
  assert(bgr[0] == 255);
  assert(bgr[kWidth * kHeight * 3] == 0);
  assert(!Nv12ToBgr(frame.data(), frame.size() - 1, &bgr));
  std::array<std::vector<float>, 12> data;
  std::array<Tensor, 12> tensors;
  for (unsigned int kind = 0; kind < 4; kind++)
    for (unsigned int level = 0; level < 3; level++)
      {
        unsigned int side = kModelSide / (8U << level),
                     channels = kind == 2   ? 4
                                : kind == 3 ? 10
                                            : 1,
                     index = kind * 3 + level;
        data[index].resize(side * side * channels);
        tensors[index] = { data[index].data(), data[index].size() };
      }
  const std::size_t cell = 20 * 80 + 30;
  data[0][cell] = .81f;
  data[3][cell] = 1;
  data[6][cell * 4 + 2] = std::log(10.f);
  data[6][cell * 4 + 3] = std::log(12.f);
  std::vector<Face> faces;
  assert(DecodeYunet(tensors, &faces) && faces.size() == 1);
  assert(std::fabs(faces[0].x - 200) < .01f &&
         std::fabs(faces[0].y - 112) < .01f);
  assert(std::fabs(faces[0].score - .9f) < .0001f);
  data[0][cell] = std::numeric_limits<float>::quiet_NaN();
  assert(DecodeYunet(tensors, &faces) && faces.empty());
  tensors[0].count--;
  assert(!DecodeYunet(tensors, &faces));
  Embedding embedding{};assert(!NormalizeEmbedding(&embedding));
  embedding[0]=3;embedding[1]=4;assert(NormalizeEmbedding(&embedding));
  assert(std::abs(embedding[0]-.6f)<1e-5f&&std::abs(embedding[1]-.8f)<1e-5f);
  Face aligned{};aligned.landmarks={38.2946f,51.6963f,73.5318f,51.5014f,
    56.0252f,71.7366f,41.5493f,92.3655f,70.7299f,92.2041f};
  std::vector<std::uint8_t> image(kModelSide*kModelSide*3),crop;
  for(unsigned int y=0;y<kHeight;y++)for(unsigned int x=0;x<kWidth;x++)
    {image[(y*kModelSide+x)*3]=x%256;image[(y*kModelSide+x)*3+1]=y%256;image[(y*kModelSide+x)*3+2]=31;}
  assert(AlignFaceRgb(image,aligned,&crop));
  assert(crop[(20*112+10)*3]==31&&crop[(20*112+10)*3+1]==20&&crop[(20*112+10)*3+2]==10);
  aligned.landmarks.fill(10);assert(!AlignFaceRgb(image,aligned,&crop));
  std::puts("VISION_MATH_PASS nv12_range padding geometry confidence "
            "malformed_tensor nonfinite");
}
