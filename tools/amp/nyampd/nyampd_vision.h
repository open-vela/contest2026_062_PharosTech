/****************************************************************************
 * tools/amp/nyampd/nyampd_vision.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_VISION_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_VISION_H
#include "nyamp_tracker.h"
#include "nyamp_vision.h"
#include "nyampd_audio.h"
#include "nyampd_loader.h"
#include <atomic>
#include <thread>

namespace nyamp
{
class VisionService
{
public:
  using Factory = std::function<std::unique_ptr<vision::Detector>()>;
  using TrackerFactory = std::function<std::unique_ptr<vision::Tracker>()>;
  VisionService(std::uint32_t generation, models::Clock clock, Factory factory,
                SharedSlot *slot, SlotGate *gate, LeaseMint *mint,
                TrackerFactory tracker_factory = {});
  ~VisionService();
  bool Supported() const { return static_cast<bool>(factory_); }
  void SetProvisioner(ModelProvisioner *provisioner);
  void SetWaker(std::function<void()> waker);
  models::Status Load(const std::string &name, const nyamp_header_s &request,
                      bool *deferred);
  models::Status LoadIdentity(const std::string &name,
                              const nyamp_header_s &request, bool *deferred);
  models::Status LoadTracker(const std::string &name,
                             const nyamp_header_s &request, bool *deferred);
  std::int32_t Begin(nyamp_buffer_s *grant);
  std::int32_t Analyze(const std::uint8_t *payload, std::size_t size,
                       const nyamp_header_s &request);
  std::int32_t End();
  std::int32_t Unload();
  bool Poll(Frame *frame);

private:
  void Worker(std::vector<std::uint8_t> pixels, nyamp_header_s request,
              std::uint64_t sequence, std::uint64_t timestamp,
              std::uint32_t face_index, vision::TrackBox box);
  void TrackWorker(std::vector<std::uint8_t> pixels, nyamp_header_s request,
                   std::uint64_t sequence, std::uint64_t timestamp,
                   vision::TrackBox box);
  std::uint32_t generation_;
  models::Clock clock_;
  Factory factory_;
  TrackerFactory tracker_factory_;
  std::unique_ptr<vision::Tracker> tracker_;
  std::atomic<bool> tracker_loaded_{ false };
  SharedSlot *slot_;
  SlotGate *gate_;
  LeaseMint *mint_;
  nyamp_buffer_s grant_{};
  bool active_ = false;
  bool clear_tracker_ = false;
  std::uint64_t sequence_ = 0, timestamp_ = 0;
  std::atomic<bool> loaded_{ false }, running_{ false }, cancelled_{ false };
  std::thread worker_;
  std::unique_ptr<vision::Detector> detector_;
  FrameQueue queue_{ 8 };
  /* Destroy the loader before the backend/queue its callback uses. */
  ModelLoader loader_;
  ModelLoader identity_loader_;
  ModelLoader tracker_loader_;
};
} // namespace nyamp
#endif
