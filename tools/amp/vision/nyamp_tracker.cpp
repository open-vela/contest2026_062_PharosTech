/****************************************************************************
 * tools/amp/vision/nyamp_tracker.cpp
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
#include <cmath>
#include <limits>
#include <utility>

namespace nyamp::vision
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinimumScore = .72f;
constexpr float kRecoveryScore = .85f;
constexpr float kAppearanceScore = .65f;
constexpr std::uint64_t kLearningMs = 3000;
float RatioPenalty(float ratio);
TrackHistogram Histogram(const std::vector<std::uint8_t> &bgr,
                         const TrackBox &box);
float Similarity(const TrackHistogram &a, const TrackHistogram &b);
float Intersection(const TrackBox &a, const TrackBox &b);
TrackBox Clip(TrackBox box);

float RatioPenalty(float ratio) { return std::max(ratio, 1.0f / ratio); }
float Intersection(const TrackBox &a, const TrackBox &b)
{
  float w = std::max(0.0f, std::min(a.x + a.width, b.x + b.width) -
                               std::max(a.x, b.x));
  float h = std::max(0.0f, std::min(a.y + a.height, b.y + b.height) -
                               std::max(a.y, b.y));
  return w * h / (a.width * a.height + b.width * b.height - w * h);
}
TrackBox Clip(TrackBox box)
{
  box.width = std::clamp(box.width, 12.0f, float(kWidth));
  box.height = std::clamp(box.height, 12.0f, float(kHeight));
  box.x = std::clamp(box.x, 0.0f, kWidth - box.width);
  box.y = std::clamp(box.y, 0.0f, kHeight - box.height);
  return box;
}
TrackHistogram Histogram(const std::vector<std::uint8_t> &bgr,
                         const TrackBox &box)
{
  TrackHistogram histogram{};
  unsigned int count = 0;
  for (int y = int(box.y); y < int(box.y + box.height); y += 3)
    for (int x = int(box.x); x < int(box.x + box.width); x += 3)
      {
        for (unsigned int c = 0; c < 3; c++)
          histogram[c * 8 + bgr[(y * kModelSide + x) * 3 + c] / 32]++;
        count++;
      }
  if (count)
    for (auto &value : histogram)
      value /= count * 3.0f;
  return histogram;
}
float Similarity(const TrackHistogram &a, const TrackHistogram &b)
{
  float score = 0;
  for (std::size_t i = 0; i < a.size(); i++)
    score += std::min(a[i], b[i]);
  return score;
}
} // namespace
bool ValidTrackBox(const TrackBox &b)
{
  return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.width) &&
         std::isfinite(b.height) && b.x >= 0 && b.y >= 0 && b.width >= 12 &&
         b.height >= 12 && b.x + b.width <= kWidth &&
         b.y + b.height <= kHeight;
}
float TrackContext(const TrackBox &b)
{
  float pad = (b.width + b.height) * .5f;
  return std::sqrt((b.width + pad) * (b.height + pad));
}
bool CropTrackBgr(const std::vector<std::uint8_t> &bgr, float cx, float cy,
                  float side, unsigned int output_side,
                  std::vector<std::uint8_t> *crop)
{
  if (!crop || bgr.size() != kModelSide * kModelSide * 3 ||
      !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(side) ||
      side < 1 || side > 4096 || (output_side != 127 && output_side != 255))
    return false;
  std::array<float, 3> average{};
  for (unsigned int y = 0; y < kHeight; y += 8)
    for (unsigned int x = 0; x < kWidth; x += 8)
      for (unsigned int c = 0; c < 3; c++)
        average[c] += bgr[(y * kModelSide + x) * 3 + c];
  for (auto &v : average)
    v /= float((kHeight / 8) * (kWidth / 8));
  crop->resize(output_side * output_side * 3);
  auto pixel = [&](int x, int y, unsigned int c) {
    return x < 0 || y < 0 || x >= int(kWidth) || y >= int(kHeight)
               ? average[c]
               : float(bgr[(y * kModelSide + x) * 3 + c]);
  };
  for (unsigned int y = 0; y < output_side; y++)
    for (unsigned int x = 0; x < output_side; x++)
      {
        float sx =
            cx + (float(x) + .5f - output_side * .5f) * side / output_side;
        float sy =
            cy + (float(y) + .5f - output_side * .5f) * side / output_side;
        int ix = int(std::floor(sx)), iy = int(std::floor(sy));
        float fx = sx - ix, fy = sy - iy;
        for (unsigned int c = 0; c < 3; c++)
          (*crop)[(y * output_side + x) * 3 + c] = std::uint8_t(std::clamp(
              (pixel(ix, iy, c) * (1 - fx) + pixel(ix + 1, iy, c) * fx) *
                      (1 - fy) +
                  (pixel(ix, iy + 1, c) * (1 - fx) +
                   pixel(ix + 1, iy + 1, c) * fx) *
                      fy,
              0.0f, 255.0f));
      }
  return true;
}
bool DecodeTrack(const Tensor &scores, const Tensor &boxes,
                 const TrackBox &previous, float cx, float cy,
                 float search_side, bool searching, TrackPrediction *result)
{
  if (!result || !ValidTrackBox(previous) || !scores.data || !boxes.data ||
      scores.count != 2 * kTrackCells || boxes.count != 4 * kTrackCells ||
      !std::isfinite(search_side) || search_side <= 0)
    return false;
  const float scale = 255.0f / search_side;
  const float prior_size = TrackContext(previous) * scale;
  float best = -1;
  for (unsigned int i = 0; i < kTrackCells; i++)
    {
      float negative = scores.data[i], positive = scores.data[kTrackCells + i];
      float left = boxes.data[i], top = boxes.data[kTrackCells + i],
            right = boxes.data[2 * kTrackCells + i],
            bottom = boxes.data[3 * kTrackCells + i];
      if (!std::isfinite(negative) || !std::isfinite(positive) ||
          !std::isfinite(left) || !std::isfinite(top) ||
          !std::isfinite(right) || !std::isfinite(bottom) || left < 0 ||
          top < 0 || right < 0 || bottom < 0)
        continue;
      float w = left + right, h = top + bottom;
      if (w < 1 || h < 1)
        continue;
      float score =
          1.0f /
          (1.0f + std::exp(std::clamp(negative - positive, -80.0f, 80.0f)));
      float pad = (w + h) * .5f;
      float size = std::sqrt((w + pad) * (h + pad));
      float penalty = std::exp(
          -(RatioPenalty(size / prior_size) *
                RatioPenalty(previous.width * h / (previous.height * w)) -
            1) *
          .150f);
      unsigned int row = i / kTrackGrid, column = i % kTrackGrid;
      float window = (1 - std::cos(2 * kPi * row / (kTrackGrid - 1))) *
                     (1 - std::cos(2 * kPi * column / (kTrackGrid - 1))) *
                     .25f;
      float rank = penalty * score * (searching ? 1.0f : .510f) +
                   (searching ? 0 : .490f * window);
      if (rank <= best)
        continue;
      float mx = cx + ((int(column) - 8) * 16 + (right - left) * .5f) / scale;
      float my = cy + ((int(row) - 8) * 16 + (bottom - top) * .5f) / scale;
      float rate = searching ? 1.0f : penalty * score * .385f;
      float width = previous.width * (1 - rate) + w / scale * rate;
      float height = previous.height * (1 - rate) + h / scale * rate;
      result->box =
          Clip({ mx - width * .5f, my - height * .5f, width, height });
      result->confidence = score;
      result->penalty = penalty;
      best = rank;
    }
  return best >= 0;
}
Tracker::Tracker(std::unique_ptr<TrackerBackend> backend)
    : backend_(std::move(backend))
{
}
bool Tracker::Load(const std::string &directory)
{
  Reset();
  loaded_ = backend_ && backend_->Load(directory);
  return loaded_;
}
void Tracker::Reset()
{
  for (auto &view : views_)
    std::fill(view.feature.begin(), view.feature.end(), 0);
  views_.clear();
  learned_ = lost_ = false;
  confirmations_ = misses_ = scan_ = good_frames_ = 0;
  timestamp_ = learning_ms_ = last_view_ms_ = 0;
  box_ = candidate_ = {};
}
bool Tracker::AddView(const std::vector<std::uint8_t> &bgr)
{
  std::vector<std::uint8_t> crop;
  View view;
  if (!CropTrackBgr(bgr, box_.x + box_.width * .5f, box_.y + box_.height * .5f,
                    TrackContext(box_), 127, &crop) ||
      !backend_->Extract(crop, true, &view.feature))
    return false;
  view.histogram = Histogram(bgr, box_);
  views_.push_back(std::move(view));
  return true;
}
void Tracker::Result(float score, TrackResult *result) const
{
  result->box = box_;
  result->confidence = score;
  result->phase = lost_ ? 3 : learned_ ? 2 : 1;
  result->progress = learned_ ? 100
                              : unsigned(std::min<std::uint64_t>(
                                    99, learning_ms_ * 100 / kLearningMs));
  result->views = views_.size();
}
bool Tracker::Start(const std::uint8_t *nv12, std::size_t bytes,
                    const TrackBox &box, std::uint64_t timestamp,
                    TrackResult *result)
{
  if (!loaded_ || !result || !timestamp || !ValidTrackBox(box))
    return false;
  std::vector<std::uint8_t> bgr;
  if (!Nv12ToBgr(nv12, bytes, &bgr))
    return false;
  Reset();
  box_ = box;
  timestamp_ = last_view_ms_ = timestamp;
  if (!AddView(bgr))
    {
      Reset();
      return false;
    }
  good_frames_ = 1;
  Result(1, result);
  return true;
}
bool Tracker::Update(const std::uint8_t *nv12, std::size_t bytes,
                     std::uint64_t timestamp, TrackResult *result)
{
  if (!result || views_.empty() || timestamp <= timestamp_)
    return false;
  std::vector<std::uint8_t> bgr, crop;
  if (!Nv12ToBgr(nv12, bytes, &bgr))
    return false;
  std::uint64_t elapsed = std::min<std::uint64_t>(timestamp - timestamp_, 500);
  timestamp_ = timestamp;
  float side = TrackContext(box_) * 255.0f / 127.0f;
  float cx = box_.x + box_.width * .5f, cy = box_.y + box_.height * .5f;
  if (lost_ && confirmations_)
    {
      cx = candidate_.x + candidate_.width * .5f;
      cy = candidate_.y + candidate_.height * .5f;
    }
  else if (lost_ && misses_ > 3)
    {
      const unsigned int position = scan_++ % 9;
      cx = (position % 3 + .5f) * kWidth / 3;
      cy = (position / 3 + .5f) * kHeight / 3;
      side = std::max(side, 320.0f);
    }
  if (!CropTrackBgr(bgr, cx, cy, side, 255, &crop))
    return false;
  TrackFeature search;
  if (!backend_->Extract(crop, false, &search))
    return false;
  TrackPrediction best;
  float best_score = -1;
  for (const auto &view : views_)
    {
      std::vector<float> scores, boxes;
      TrackPrediction prediction;
      if (!backend_->Match(view.feature, search, &scores, &boxes) ||
          !DecodeTrack({ scores.data(), scores.size() },
                       { boxes.data(), boxes.size() }, box_, cx, cy, side,
                       lost_, &prediction))
        return false;
      float appearance =
          Similarity(view.histogram, Histogram(bgr, prediction.box));
      float threshold = lost_ ? kRecoveryScore : kMinimumScore;
      if (prediction.confidence >= threshold &&
          appearance >= kAppearanceScore && prediction.penalty > .25f &&
          prediction.confidence > best_score)
        {
          best = prediction;
          best_score = prediction.confidence;
        }
    }
  if (best_score < 0)
    {
      lost_ = true;
      misses_++;
      confirmations_ = 0;
      Result(0, result);
      return true;
    }
  if (lost_)
    {
      confirmations_ =
          confirmations_ && Intersection(candidate_, best.box) > .45f
              ? confirmations_ + 1
              : 1;
      candidate_ = best.box;
      if (confirmations_ < 3)
        {
          Result(best_score, result);
          return true;
        }
      lost_ = false;
      misses_ = confirmations_ = 0;
    }
  box_ = best.box;
  if (!learned_)
    {
      learning_ms_ += elapsed;
      good_frames_++;
      if (views_.size() < 4 && timestamp - last_view_ms_ >= 750 &&
          best_score >= .9f)
        {
          if (!AddView(bgr))
            return false;
          last_view_ms_ = timestamp;
        }
      learned_ = learning_ms_ >= kLearningMs && good_frames_ >= 8;
    }
  Result(best_score, result);
  return true;
}
} // namespace nyamp::vision
