/****************************************************************************
 * tools/amp/nyampd/nyampd_speaker.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_SPEAKER_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_SPEAKER_H
#include "nyampd_asr.h"
#include "nyampd_loader.h"
#include <thread>
namespace nyamp
{
class SpeakerBackend
{
public:
  virtual ~SpeakerBackend() = default;
  virtual models::Status Load(const std::string &path) = 0;
  virtual void ResetEnrollment() = 0;
  virtual models::Status Add(const float *samples, std::size_t count,
                             const models::Stop &stop,
                             std::uint32_t *takes) = 0;
  virtual models::Status Commit(std::vector<std::uint8_t> *bytes,
                                std::uint32_t *kept, float *cohesion) = 0;
  virtual models::Status Put(const std::uint8_t *bytes, std::size_t size) = 0;
  virtual void Forget() = 0;
  virtual models::Status Verify(const float *samples, std::size_t count,
                                const models::Stop &stop, float *score) = 0;
};
std::unique_ptr<SpeakerBackend> CreateSherpaSpeakerBackend();
class SpeakerService
{
public:
  using Factory = std::function<std::unique_ptr<SpeakerBackend>()>;
  SpeakerService(std::uint32_t generation, Factory factory,
                 CaptureSource *source);
  ~SpeakerService();
  bool Supported() const { return static_cast<bool>(factory_); }
  void SetProvisioner(ModelProvisioner *p);
  void SetWaker(std::function<void()> waker);
  std::int32_t Handle(const nyamp_header_s &request,
                      const std::uint8_t *payload, std::uint8_t *body,
                      std::size_t *size, bool *deferred);
  void Cancel(std::uint64_t id);
  bool Poll(Frame *frame);

private:
  void Worker(nyamp_header_s request, std::shared_ptr<CaptureRing> ring,
              std::uint64_t start, std::uint64_t end);
  std::uint32_t generation_;
  Factory factory_;
  CaptureSource *source_;
  std::unique_ptr<SpeakerBackend> backend_;
  std::atomic<bool> running_{ false }, cancelled_{ false }, loaded_{ false };
  std::atomic<std::uint64_t> request_id_{ 0 };
  std::thread worker_;
  std::uint64_t enrollment_stream_ = 0, last_take_end_ = 0;
  FrameQueue queue_{ 8 };
  ModelLoader loader_;
};
}
#endif
