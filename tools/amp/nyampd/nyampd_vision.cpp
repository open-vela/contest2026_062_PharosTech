/****************************************************************************
 * tools/amp/nyampd/nyampd_vision.cpp
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

#include "nyampd_vision.h"
#include "nyamp_vision_wire.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace nyamp
{
VisionService::VisionService(std::uint32_t generation, models::Clock clock,
                             Factory factory, SharedSlot *slot, SlotGate *gate,
                             LeaseMint *mint, TrackerFactory tracker_factory)
    : generation_(generation), clock_(std::move(clock)),
      factory_(std::move(factory)),
      tracker_factory_(std::move(tracker_factory)), slot_(slot), gate_(gate),
      mint_(mint), loader_(generation, &queue_),
      identity_loader_(generation, &queue_),
      tracker_loader_(generation, &queue_)
{
}
VisionService::~VisionService()
{
  End();
  if (worker_.joinable())
    worker_.join();
}
void VisionService::SetProvisioner(ModelProvisioner *p)
{
  loader_.SetProvisioner(p);
  identity_loader_.SetProvisioner(p);
  tracker_loader_.SetProvisioner(p);
}
void VisionService::SetWaker(std::function<void()> waker)
{
  queue_.SetWaker(std::move(waker));
}
models::Status VisionService::Load(const std::string &name,
                                   const nyamp_header_s &request,
                                   bool *deferred)
{
  *deferred = false;
  if (!factory_)
    return models::Status::kUnsupported;
  if (running_.load() || active_)
    return models::Status::kBusy;
  if (loaded_.load())
    return loader_.last_name() == name ? models::Status::kOk
                                       : models::Status::kBusy;
  return loader_.Begin(name, request, deferred,
                       [this](const std::string &path) {
                         auto candidate = factory_();
                         if (!candidate || !candidate->Load(path))
                           return models::Status::kBackendError;
                         detector_ = std::move(candidate);
                         loaded_.store(true);
                         return models::Status::kOk;
                       });
}
models::Status VisionService::LoadIdentity(const std::string &name,
                                           const nyamp_header_s &request,
                                           bool *deferred)
{
  *deferred = false;
  if (!loaded_.load() || loader_.loading())
    return models::Status::kNotReady;
  if (running_.load() || active_)
    return models::Status::kBusy;
  if (!detector_->SupportsIdentity())
    return models::Status::kUnsupported;
  if (detector_->CanEmbed() && identity_loader_.last_name() == name)
    return models::Status::kOk;
  return identity_loader_.Begin(
      name, request, deferred, [this](const std::string &path) {
        return detector_->LoadIdentity(path) ? models::Status::kOk
                                             : models::Status::kBackendError;
      });
}
models::Status VisionService::LoadTracker(const std::string &name,
                                          const nyamp_header_s &request,
                                          bool *deferred)
{
  *deferred = false;
  if (!tracker_factory_)
    return models::Status::kUnsupported;
  if (running_.load() || active_)
    return models::Status::kBusy;
  if (tracker_loaded_.load())
    return tracker_loader_.last_name() == name ? models::Status::kOk
                                               : models::Status::kBusy;
  return tracker_loader_.Begin(name, request, deferred,
                               [this](const std::string &path) {
                                 auto candidate = tracker_factory_();
                                 if (!candidate || !candidate->Load(path))
                                   return models::Status::kBackendError;
                                 tracker_ = std::move(candidate);
                                 tracker_loaded_.store(true);
                                 return models::Status::kOk;
                               });
}
std::int32_t VisionService::Begin(nyamp_buffer_s *grant)
{
  if (!factory_ && !tracker_factory_)
    return NYAMP_MODEL_UNSUPPORTED;
  if (loader_.loading() || identity_loader_.loading() ||
      tracker_loader_.loading() || running_.load() || active_)
    return NYAMP_MODEL_BUSY;
  if ((!loaded_.load() && !tracker_loaded_.load()) || !slot_ ||
      !slot_->data() || slot_->capacity() < NYAMP_VISION_BYTES)
    return NYAMP_MODEL_NOT_READY;
  if (!gate_->Acquire())
    return NYAMP_MODEL_BUSY;
  grant_ = MakeGrant(mint_, slot_->offset(), slot_->capacity(),
                     NYAMP_FORMAT_BYTES, 0);
  if (tracker_)
    tracker_->Reset();
  clear_tracker_ = false;
  active_ = true;
  sequence_ = timestamp_ = 0;
  *grant = grant_;
  return NYAMP_MODEL_OK;
}
std::int32_t VisionService::Analyze(const std::uint8_t *payload,
                                    std::size_t size,
                                    const nyamp_header_s &request)
{
  if (!active_)
    return NYAMP_MODEL_NOT_READY;
  if (running_.load() || queue_.size() != 0)
    return NYAMP_MODEL_BUSY;
  nyamp_buffer_s buffer{};
  bool embed = request.opcode == NYAMP_VISION_EMBED;
  bool track = request.opcode == NYAMP_VISION_TRACK_INIT ||
               request.opcode == NYAMP_VISION_TRACK_FRAME;
  bool initialize = request.opcode == NYAMP_VISION_TRACK_INIT;
  if (track ? !tracker_loaded_.load() : !loaded_.load())
    return NYAMP_MODEL_NOT_READY;
  if (embed && !detector_->CanEmbed())
    return NYAMP_MODEL_UNSUPPORTED;
  if (size != (initialize ? NYAMP_VISION_TRACK_INIT_SIZE
               : embed    ? NYAMP_VISION_EMBED_SIZE
                          : NYAMP_VISION_FRAME_SIZE) ||
      nyamp_buffer_decode(&buffer, payload, NYAMP_BUFFER_SIZE) != NYAMP_OK)
    return NYAMP_MODEL_INVALID;
  if (buffer.lease != grant_.lease || buffer.generation != grant_.generation)
    return NYAMP_MODEL_STALE_GENERATION;
  if (buffer.offset != grant_.offset || buffer.capacity != grant_.capacity ||
      buffer.length != NYAMP_VISION_BYTES ||
      buffer.format != NYAMP_FORMAT_BYTES ||
      buffer.flags != NYAMP_BUFFER_IN_SHMEM)
    return NYAMP_MODEL_INVALID;
  std::uint64_t sequence = nyamp_vision_get(payload + NYAMP_BUFFER_SIZE, 8);
  std::uint64_t timestamp =
      nyamp_vision_get(payload + NYAMP_BUFFER_SIZE + 8, 8);
  if (sequence <= sequence_ || timestamp <= timestamp_)
    return NYAMP_MODEL_DUPLICATE;
  std::uint32_t face_index =
      embed ? nyamp_vision_get(payload + NYAMP_VISION_FRAME_SIZE, 4) : 0;
  if (face_index >= NYAMP_VISION_MAX_FACES)
    return NYAMP_MODEL_INVALID;
  vision::TrackBox box;
  if (initialize)
    {
      const auto *p = payload + NYAMP_VISION_FRAME_SIZE;
      box = { nyamp_vision_get_float(p) * vision::kWidth,
              nyamp_vision_get_float(p + 4) * vision::kHeight,
              nyamp_vision_get_float(p + 8) * vision::kWidth,
              nyamp_vision_get_float(p + 12) * vision::kHeight };
      if (!vision::ValidTrackBox(box))
        return NYAMP_MODEL_INVALID;
    }
  auto *source = slot_->data();
  if (!source)
    return NYAMP_MODEL_NOT_READY;
  if (worker_.joinable())
    worker_.join();
  std::vector<std::uint8_t> pixels(source, source + NYAMP_VISION_BYTES);
  sequence_ = sequence;
  timestamp_ = timestamp;
  cancelled_.store(false);
  running_.store(true);
  try
    {
      worker_ = std::thread(&VisionService::Worker, this, std::move(pixels),
                            request, sequence, timestamp, face_index, box);
    }
  catch (...)
    {
      running_.store(false);
      return NYAMP_MODEL_BACKEND_ERROR;
    }
  return NYAMP_MODEL_OK;
}
void VisionService::Worker(std::vector<std::uint8_t> pixels,
                           nyamp_header_s request, std::uint64_t sequence,
                           std::uint64_t timestamp, std::uint32_t face_index,
                           vision::TrackBox box)
{
  if (request.opcode == NYAMP_VISION_TRACK_INIT ||
      request.opcode == NYAMP_VISION_TRACK_FRAME)
    {
      TrackWorker(std::move(pixels), request, sequence, timestamp, box);
      return;
    }
  std::vector<vision::Face> faces;
  vision::Embedding embedding{};
  bool embed = request.opcode == NYAMP_VISION_EMBED;
  bool has_embedding = false;
  std::int32_t status = NYAMP_MODEL_OK;
  if (cancelled_.load())
    status = NYAMP_MODEL_CANCELLED;
  else if (!detector_->Detect(pixels.data(), pixels.size(), &faces))
    status = NYAMP_MODEL_BACKEND_ERROR;
  if (status == NYAMP_MODEL_OK && embed && face_index < faces.size() &&
      !cancelled_.load())
    {
      const auto &face = faces[face_index];
      bool inside = true;
      for (unsigned int i = 0; i < 10; i++)
        inside &=
            std::isfinite(face.landmarks[i]) && face.landmarks[i] >= 0 &&
            face.landmarks[i] < (i % 2 ? vision::kHeight : vision::kWidth);
      if (face.width >= 64 && face.height >= 64 && face.score >= .8f && inside)
        {
          if (!detector_->Embed(pixels.data(), pixels.size(), face,
                                &embedding))
            status = NYAMP_MODEL_BACKEND_ERROR;
          else
            has_embedding = true;
        }
    }
  if (cancelled_.load())
    status = NYAMP_MODEL_CANCELLED;
  else if (request.deadline_ms && clock_() > request.deadline_ms)
    status = NYAMP_MODEL_DEADLINE;
  detector_->ClearFrame();
  std::fill(pixels.begin(), pixels.end(), 0);
  std::uint8_t body[NYAMP_INLINE_MAX - 4]{};
  std::size_t size = 0;
  if (status == NYAMP_MODEL_OK)
    {
      if (faces.size() > NYAMP_VISION_MAX_FACES)
        faces.resize(NYAMP_VISION_MAX_FACES);
      nyamp_vision_put(body, sequence, 8);
      nyamp_vision_put(body + 8, timestamp, 8);
      nyamp_vision_put(body + 16, faces.size(), 4);
      size = embed ? NYAMP_VISION_EMBED_HEADER : NYAMP_VISION_RESULT_HEADER;
      auto append_face = [&](const vision::Face &face) {
        const float geometry[] = { face.x / vision::kWidth,
                                   face.y / vision::kHeight,
                                   face.width / vision::kWidth,
                                   face.height / vision::kHeight, face.score };
        for (unsigned int i = 0; i < 5; i++)
          nyamp_vision_put_float(body + size + i * 4, geometry[i]);
        for (unsigned int i = 0; i < 10; i++)
          nyamp_vision_put_float(
              body + size + 20 + i * 4,
              face.landmarks[i] / (i % 2 ? vision::kHeight : vision::kWidth));
        size += NYAMP_VISION_FACE_SIZE;
      };
      if (embed)
        {
          nyamp_vision_put(body + 20, has_embedding ? face_index : UINT32_MAX,
                           4);
          if (has_embedding)
            {
              append_face(faces[face_index]);
              for (float value : embedding)
                {
                  auto q = static_cast<std::int16_t>(
                      std::lround(std::clamp(value, -1.0f, 1.0f) * 32767));
                  nyamp_vision_put(body + size, static_cast<std::uint16_t>(q),
                                   2);
                  size += 2;
                }
            }
        }
      else
        for (const auto &face : faces)
          append_face(face);
    }
  Frame response{};
  if (EncodeStatusResponse(&response, request, generation_, status, body,
                           size))
    queue_.Push(response, false);
  running_.store(false);
}
void VisionService::TrackWorker(std::vector<std::uint8_t> pixels,
                                nyamp_header_s request, std::uint64_t sequence,
                                std::uint64_t timestamp, vision::TrackBox box)
{
  vision::TrackResult result;
  std::int32_t status = NYAMP_MODEL_OK;
  bool ok = false;
  if (!cancelled_.load())
    ok = request.opcode == NYAMP_VISION_TRACK_INIT
             ? tracker_->Start(pixels.data(), pixels.size(), box, timestamp,
                               &result)
             : tracker_->Update(pixels.data(), pixels.size(), timestamp,
                                &result);
  if (!ok)
    status = NYAMP_MODEL_BACKEND_ERROR;
  if (cancelled_.load())
    status = NYAMP_MODEL_CANCELLED;
  else if (request.deadline_ms && clock_() > request.deadline_ms)
    status = NYAMP_MODEL_DEADLINE;
  std::fill(pixels.begin(), pixels.end(), 0);
  std::uint8_t body[NYAMP_VISION_TRACK_RESULT_SIZE]{};
  std::size_t size = 0;
  if (status == NYAMP_MODEL_OK)
    {
      nyamp_vision_put(body, sequence, 8);
      nyamp_vision_put(body + 8, timestamp, 8);
      nyamp_vision_put(body + 16, result.phase, 4);
      nyamp_vision_put(body + 20, result.progress, 4);
      nyamp_vision_put(body + 24, result.views, 4);
      float values[] = { result.box.x / vision::kWidth,
                         result.box.y / vision::kHeight,
                         result.box.width / vision::kWidth,
                         result.box.height / vision::kHeight,
                         result.confidence };
      for (unsigned int i = 0; i < 5; i++)
        nyamp_vision_put_float(body + 28 + i * 4, values[i]);
      size = sizeof(body);
    }
  Frame response{};
  if (EncodeStatusResponse(&response, request, generation_, status, body,
                           size))
    queue_.Push(response, false);
  running_.store(false);
}
std::int32_t VisionService::End()
{
  cancelled_.store(true);
  clear_tracker_ = true;
  if (active_)
    {
      active_ = false;
      grant_ = {};
      gate_->Release();
    }
  return NYAMP_MODEL_OK;
}
std::int32_t VisionService::Unload()
{
  if (loader_.loading() || identity_loader_.loading() ||
      tracker_loader_.loading())
    return NYAMP_MODEL_BUSY;
  End();
  if (running_.load())
    return NYAMP_MODEL_BUSY;
  if (worker_.joinable())
    worker_.join();
  tracker_.reset();
  tracker_loaded_.store(false);
  detector_.reset();
  loaded_.store(false);
  return NYAMP_MODEL_OK;
}
bool VisionService::Poll(Frame *frame)
{
  if (clear_tracker_ && !active_ && !running_.load() &&
      !tracker_loader_.loading())
    {
      if (tracker_)
        tracker_->Reset();
      clear_tracker_ = false;
    }
  return queue_.Poll(frame);
}
} // namespace nyamp
