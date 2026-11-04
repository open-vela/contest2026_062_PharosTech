/****************************************************************************
 * tools/amp/vision/nyamp_tracker_test.cpp
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
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace nyamp::vision;
class FakeBackend : public TrackerBackend
{
public:
  bool visible = true;
  bool Load(const std::string &) override { return true; }
  bool Extract(const std::vector<std::uint8_t> &, bool,
               TrackFeature *feature) override
  {
    feature->assign(1, 1);
    return true;
  }
  bool Match(const TrackFeature &, const TrackFeature &,
             std::vector<float> *scores, std::vector<float> *boxes) override
  {
    scores->assign(kTrackCells * 2, 0);
    boxes->assign(kTrackCells * 4, 31.75f);
    for (std::size_t i = 0; i < kTrackCells; i++)
      (*scores)[i] = 10;
    if (visible)
      {
        (*scores)[8 * 16 + 8] = 0;
        (*scores)[kTrackCells + 8 * 16 + 8] = 10;
      }
    return true;
  }
};
int main()
{
  TrackBox box{ 240, 160, 160, 160 };
  assert(ValidTrackBox(box));
  assert(!ValidTrackBox({ -1, 0, 10, 10 }));
  std::vector<float> scores(kTrackCells * 2, 0),
      boxes(kTrackCells * 4, 31.75f);
  for (std::size_t i = 0; i < kTrackCells; i++)
    scores[i] = 10;
  scores[136] = 0;
  scores[kTrackCells + 136] = 10;
  TrackPrediction prediction;
  float side = 320 * 255.0f / 127;
  assert(DecodeTrack({ scores.data(), scores.size() },
                     { boxes.data(), boxes.size() }, box, 320, 240, side,
                     false, &prediction));
  assert(std::fabs(prediction.box.x - box.x) < .1f &&
         std::fabs(prediction.box.y - box.y) < .1f);
  assert(prediction.confidence > .99f);
  boxes.assign(kTrackCells * 4, std::numeric_limits<float>::quiet_NaN());
  assert(!DecodeTrack({ scores.data(), scores.size() },
                      { boxes.data(), boxes.size() }, box, 320, 240, side,
                      false, &prediction));
  std::vector<std::uint8_t> pixels(kNv12Bytes, 128), bgr, crop;
  assert(Nv12ToBgr(pixels.data(), pixels.size(), &bgr));
  assert(CropTrackBgr(bgr, 0, 0, 640, 127, &crop));
  assert(crop.size() == 127 * 127 * 3);
  auto backend = std::make_unique<FakeBackend>();
  auto *fake = backend.get();
  Tracker tracker(std::move(backend));
  assert(tracker.Load("fixture"));
  TrackResult result;
  assert(!tracker.Start(pixels.data(), pixels.size(), { 0, 0, 1, 1 }, 1,
                        &result));
  assert(tracker.Start(pixels.data(), pixels.size(), box, 100, &result));
  assert(result.phase == 1);
  for (unsigned int i = 1; i <= 31; i++)
    assert(
        tracker.Update(pixels.data(), pixels.size(), 100 + i * 100, &result));
  assert(result.phase == 2 && result.progress == 100 && result.views == 4);
  fake->visible = false;
  assert(tracker.Update(pixels.data(), pixels.size(), 3300, &result));
  assert(result.phase == 3);
  fake->visible = true;
  assert(tracker.Update(pixels.data(), pixels.size(), 3400, &result));
  assert(result.phase == 3);
  assert(tracker.Update(pixels.data(), pixels.size(), 3500, &result));
  assert(result.phase == 3);
  assert(tracker.Update(pixels.data(), pixels.size(), 3600, &result));
  assert(result.phase == 2);
  assert(!tracker.Update(pixels.data(), pixels.size(), 3600, &result));
  tracker.Reset();
  assert(!tracker.Update(pixels.data(), pixels.size(), 3700, &result));
  std::puts("TRACKER_PASS geometry finite_crop learning frozen_templates "
            "lost_three_frame_recovery reset");
}
