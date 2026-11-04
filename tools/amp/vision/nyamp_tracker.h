/****************************************************************************
 * tools/amp/vision/nyamp_tracker.h
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

#ifndef __TOOLS_AMP_VISION_NYAMP_TRACKER_H
#define __TOOLS_AMP_VISION_NYAMP_TRACKER_H
#include "nyamp_vision.h"

namespace nyamp::vision
{
struct TrackBox
{
  float x = 0, y = 0, width = 0, height = 0;
};
struct TrackResult
{
  TrackBox box;
  float confidence = 0;
  unsigned int phase = 1, progress = 0, views = 0;
};
struct TrackPrediction
{
  TrackBox box;
  float confidence = 0, penalty = 0;
};
constexpr unsigned int kTrackGrid = 16;
constexpr std::size_t kTrackCells = kTrackGrid * kTrackGrid;
using TrackFeature = std::vector<float>;
using TrackHistogram = std::array<float, 24>;

bool ValidTrackBox(const TrackBox &box);
float TrackContext(const TrackBox &box);
bool CropTrackBgr(const std::vector<std::uint8_t> &bgr, float cx, float cy,
                  float side, unsigned int output_side,
                  std::vector<std::uint8_t> *crop);
bool DecodeTrack(const Tensor &scores, const Tensor &boxes,
                 const TrackBox &previous, float cx, float cy,
                 float search_side, bool searching, TrackPrediction *result);

class TrackerBackend
{
public:
  virtual ~TrackerBackend() = default;
  virtual bool Load(const std::string &directory) = 0;
  virtual bool Extract(const std::vector<std::uint8_t> &crop, bool exemplar,
                       TrackFeature *feature) = 0;
  virtual bool Match(const TrackFeature &exemplar, const TrackFeature &search,
                     std::vector<float> *scores,
                     std::vector<float> *boxes) = 0;
};
class Tracker
{
public:
  explicit Tracker(std::unique_ptr<TrackerBackend> backend);
  ~Tracker() { Reset(); }
  bool Load(const std::string &directory);
  void Reset();
  bool Start(const std::uint8_t *nv12, std::size_t bytes, const TrackBox &box,
             std::uint64_t timestamp, TrackResult *result);
  bool Update(const std::uint8_t *nv12, std::size_t bytes,
              std::uint64_t timestamp, TrackResult *result);

private:
  struct View
  {
    TrackFeature feature;
    TrackHistogram histogram{};
  };
  bool AddView(const std::vector<std::uint8_t> &bgr);
  void Result(float score, TrackResult *result) const;
  std::unique_ptr<TrackerBackend> backend_;
  std::vector<View> views_;
  TrackBox box_, candidate_;
  bool loaded_ = false, learned_ = false, lost_ = false;
  unsigned int confirmations_ = 0, misses_ = 0, scan_ = 0, good_frames_ = 0;
  std::uint64_t timestamp_ = 0, learning_ms_ = 0, last_view_ms_ = 0;
};
std::unique_ptr<Tracker> CreateRknnTracker();
} // namespace nyamp::vision
#endif
