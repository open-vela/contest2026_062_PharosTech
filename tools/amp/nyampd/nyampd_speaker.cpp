/****************************************************************************
 * tools/amp/nyampd/nyampd_speaker.cpp
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

#include "nyampd_speaker.h"
#include "nyamp_speaker_wire.h"
#include <algorithm>
#include <cstring>
namespace nyamp
{
SpeakerService::SpeakerService(std::uint32_t generation, Factory factory,
                               CaptureSource *source)
    : generation_(generation), factory_(std::move(factory)), source_(source),
      loader_(generation, &queue_)
{
}
SpeakerService::~SpeakerService()
{
  cancelled_.store(true);
  if (worker_.joinable())
    worker_.join();
}
void SpeakerService::SetProvisioner(ModelProvisioner *p)
{
  loader_.SetProvisioner(p);
}
void SpeakerService::SetWaker(std::function<void()> waker)
{
  queue_.SetWaker(std::move(waker));
}
void SpeakerService::Cancel(std::uint64_t id)
{
  if (request_id_.load() == id)
    cancelled_.store(true);
}
bool SpeakerService::Poll(Frame *frame) { return queue_.Poll(frame); }
std::int32_t SpeakerService::Handle(const nyamp_header_s &request,
                                    const std::uint8_t *payload,
                                    std::uint8_t *body, std::size_t *size,
                                    bool *deferred)
{
  *size = 0;
  *deferred = false;
  if (!factory_)
    return NYAMP_MODEL_UNSUPPORTED;
  if (request.opcode == NYAMP_SPK_DELETE && !loaded_.load())
    return request.payload_size ? NYAMP_MODEL_INVALID : NYAMP_MODEL_OK;
  if (running_.load() || loader_.loading())
    return NYAMP_MODEL_BUSY;
  if (request.opcode == NYAMP_SPK_LOAD)
    {
      if (!request.payload_size ||
          std::memchr(payload, 0, request.payload_size))
        return NYAMP_MODEL_INVALID;
      std::string name(reinterpret_cast<const char *>(payload),
                       request.payload_size);
      if (loaded_.load())
        return loader_.last_name() == name ? NYAMP_MODEL_OK : NYAMP_MODEL_BUSY;
      auto status = loader_.Begin(name, request, deferred,
                                  [this](const std::string &path) {
                                    auto backend = factory_();
                                    if (!backend)
                                      return models::Status::kUnsupported;
                                    auto result = backend->Load(path);
                                    if (result == models::Status::kOk)
                                      {
                                        backend_ = std::move(backend);
                                        loaded_.store(true);
                                      }
                                    return result;
                                  });
      return -static_cast<std::int32_t>(status);
    }
  if (!loaded_.load())
    return NYAMP_MODEL_NOT_READY;
  if (request.opcode == NYAMP_SPK_ENROL_ADD ||
      request.opcode == NYAMP_SPK_VERIFY)
    {
      if (request.payload_size != NYAMP_SPK_RANGE_SIZE || !source_)
        return NYAMP_MODEL_INVALID;
      auto ring = source_->StreamFor(nyamp_spk_get(payload, 8));
      std::uint64_t start = nyamp_spk_get(payload + 8, 8),
                    end = nyamp_spk_get(payload + 16, 8);
      if (end <= start || end - start < 8000 || end - start > 96000)
        return NYAMP_MODEL_INVALID;
      if (!ring || ring->closed() || start < ring->begin() ||
          end > ring->end())
        return NYAMP_MODEL_NOT_READY;
      if (request.opcode == NYAMP_SPK_ENROL_ADD)
        {
          std::uint64_t stream = nyamp_spk_get(payload, 8);
          if (stream == enrollment_stream_ && start < last_take_end_)
            return NYAMP_MODEL_DUPLICATE;
          enrollment_stream_ = stream;
          last_take_end_ = end;
        }
      if (worker_.joinable())
        worker_.join();
      request_id_.store(request.request_id);
      cancelled_.store(false);
      running_.store(true);
      try
        {
          worker_ = std::thread(&SpeakerService::Worker, this, request, ring,
                                start, end);
        }
      catch (...)
        {
          running_.store(false);
          return NYAMP_MODEL_BACKEND_ERROR;
        }
      *deferred = true;
      return NYAMP_MODEL_OK;
    }
  models::Status result = models::Status::kOk;
  if (request.opcode == NYAMP_SPK_TEMPLATE_PUT)
    {
      if (request.payload_size != NYAMP_SPK_TEMPLATE_SIZE)
        return NYAMP_MODEL_INVALID;
      result = backend_->Put(payload, request.payload_size);
    }
  else
    {
      if (request.payload_size)
        return NYAMP_MODEL_INVALID;
      switch (request.opcode)
        {
          case NYAMP_SPK_ENROL_BEGIN:
          case NYAMP_SPK_ENROL_ABORT:
            backend_->ResetEnrollment();
            enrollment_stream_ = last_take_end_ = 0;
            break;
          case NYAMP_SPK_DELETE:
            backend_->Forget();
            break;
          case NYAMP_SPK_UNLOAD:
            if (worker_.joinable())
              worker_.join();
            backend_.reset();
            loaded_.store(false);
            break;
          case NYAMP_SPK_ENROL_COMMIT:
            {
              std::vector<std::uint8_t> bytes;
              std::uint32_t kept = 0;
              float cohesion = 0;
              result = backend_->Commit(&bytes, &kept, &cohesion);
              if (result == models::Status::kOk)
                {
                  if (bytes.size() != NYAMP_SPK_TEMPLATE_SIZE)
                    return NYAMP_MODEL_BACKEND_ERROR;
                  nyamp_spk_put(body, kept, 4);
                  nyamp_spk_float_put(body + 4, cohesion);
                  std::memcpy(body + 8, bytes.data(), bytes.size());
                  *size = 8 + bytes.size();
                }
              break;
            }
          default:
            return NYAMP_MODEL_UNSUPPORTED;
        }
    }
  return -static_cast<std::int32_t>(result);
}
void SpeakerService::Worker(nyamp_header_s request,
                            std::shared_ptr<CaptureRing> ring,
                            std::uint64_t start, std::uint64_t end)
{
  std::vector<float> samples(end - start);
  std::uint32_t epoch = ring->epoch();
  std::uint64_t cursor = start;
  auto read =
      ring->Fetch(&cursor, &epoch, samples.data(), samples.size(), end, 0);
  models::Status status = models::Status::kOk;
  std::uint8_t body[8]{};
  std::size_t size = 0;
  auto stop = [this, &ring] { return cancelled_.load() || ring->closed(); };
  if (read.count != samples.size() || read.gap || read.closed)
    status = models::Status::kNotReady;
  else if (stop())
    status = models::Status::kCancelled;
  else if (request.opcode == NYAMP_SPK_ENROL_ADD)
    {
      std::uint32_t takes = 0;
      status = backend_->Add(samples.data(), samples.size(), stop, &takes);
      nyamp_spk_put(body, takes, 4);
      size = 4;
    }
  else
    {
      float score = -2;
      status = backend_->Verify(samples.data(), samples.size(), stop, &score);
      nyamp_spk_float_put(body, score);
      nyamp_spk_put(body + 4, samples.size(), 4);
      size = 8;
    }
  if (stop())
    status = models::Status::kCancelled;
  std::fill(samples.begin(), samples.end(), 0);
  Frame response{};
  running_.store(false);
  if (EncodeStatusResponse(&response, request, generation_,
                           -static_cast<std::int32_t>(status), body,
                           status == models::Status::kOk ? size : 0))
    queue_.Push(response, false);
}
}
