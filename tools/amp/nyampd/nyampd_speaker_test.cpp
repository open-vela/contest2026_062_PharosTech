/****************************************************************************
 * tools/amp/nyampd/nyampd_speaker_test.cpp
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

#include "nyamp_speaker_wire.h"
#include "nyampd_core.h"
#include "nyampd_speaker.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace nyamp;
namespace
{
class Source : public CaptureSource
{
public:
  std::shared_ptr<CaptureRing> ring = std::make_shared<CaptureRing>(64000, 0);
  std::shared_ptr<CaptureRing> Stream() override { return ring; }
  std::shared_ptr<CaptureRing> StreamFor(std::uint64_t id) override
  {
    return id == 55 ? ring : nullptr;
  }
};
class Backend : public SpeakerBackend
{
  unsigned int takes = 0;
  bool stored = false;

public:
  models::Status Load(const std::string &path) override
  {
    return path == "/fixture" ? models::Status::kOk : models::Status::kInvalid;
  }
  void ResetEnrollment() override { takes = 0; }
  models::Status Add(const float *s, std::size_t count,
                     const models::Stop &stop, std::uint32_t *n) override
  {
    assert(count == 16000 && std::abs(s[0] - .1f) < 1e-5);
    if (stop())
      return models::Status::kCancelled;
    *n = ++takes;
    return models::Status::kOk;
  }
  models::Status Commit(std::vector<std::uint8_t> *b, std::uint32_t *kept,
                        float *cohesion) override
  {
    if (takes < 3)
      return models::Status::kInvalid;
    b->assign(400, 0);
    (*b)[0] = 7;
    *kept = takes;
    *cohesion = .8f;
    return models::Status::kOk;
  }
  models::Status Put(const std::uint8_t *b, std::size_t size) override
  {
    stored = size == 400 && b[0] == 7;
    return stored ? models::Status::kOk : models::Status::kInvalid;
  }
  void Forget() override { stored = false; }
  models::Status Verify(const float *s, std::size_t count,
                        const models::Stop &stop, float *score) override
  {
    assert(count == 16000 && s);
    if (stop())
      return models::Status::kCancelled;
    if (!stored)
      return models::Status::kNotReady;
    *score = .8f;
    return models::Status::kOk;
  }
};
}
int main()
{
  Source source;
  std::vector<float> audio(64000, .1f);
  source.ring->Append(audio.data(), audio.size());
  SpeakerService service(
      9, [] { return std::make_unique<Backend>(); }, &source);
  SpeechServices speech{ nullptr, nullptr, nullptr, &service };
  std::uint8_t response[NYAMP_RPMSG_MTU];
  std::size_t size = 0;
  std::uint64_t id = 0;
  auto call = [&](std::uint16_t opcode, const std::uint8_t *body,
                  std::size_t count) {
    std::uint8_t request[NYAMP_RPMSG_MTU];
    nyamp_header_s h{ NYAMP_SERVICE_SPEAKER,
                      opcode,
                      NYAMP_FLAG_REQUEST,
                      ++id,
                      0,
                      9,
                      static_cast<std::uint32_t>(count) };
    assert(nyamp_header_encode(request, sizeof(request), &h) == 0);
    if (count)
      std::memcpy(request + NYAMP_WIRE_HEADER_SIZE, body, count);
    assert(Dispatch(request, NYAMP_WIRE_HEADER_SIZE + count, 1, 9, response,
                    sizeof(response), &size, {}, nullptr, nullptr,
                    &speech) == 0);
    return size ? static_cast<std::int32_t>(
                      nyamp_spk_get(response + NYAMP_WIRE_HEADER_SIZE, 4))
                : 0;
  };
  Frame out{};
  auto wait = [&] {
    for (int n = 0; n < 1000; n++)
      {
        if (service.Poll(&out))
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    assert(false);
  };
  assert(call(NYAMP_SPK_ENROL_BEGIN, nullptr, 0) == NYAMP_MODEL_NOT_READY);
  assert(call(NYAMP_SPK_LOAD,
              reinterpret_cast<const std::uint8_t *>("/fixture"), 8) == 0);
  assert(call(NYAMP_SPK_ENROL_BEGIN, nullptr, 0) == 0);
  std::uint8_t range[24];
  nyamp_spk_put(range, 54, 8);
  nyamp_spk_put(range + 8, 0, 8);
  nyamp_spk_put(range + 16, 16000, 8);
  assert(call(NYAMP_SPK_ENROL_ADD, range, 24) == NYAMP_MODEL_NOT_READY);
  nyamp_spk_put(range, 55, 8);
  for (int i = 0; i < 3; i++)
    {
      nyamp_spk_put(range + 8, i * 16000, 8);
      nyamp_spk_put(range + 16, (i + 1) * 16000, 8);
      assert(call(NYAMP_SPK_ENROL_ADD, range, 24) == 0 && size == 0);
      wait();
      assert(nyamp_spk_get(out.data + NYAMP_WIRE_HEADER_SIZE, 4) == 0);
      assert(nyamp_spk_get(out.data + NYAMP_WIRE_HEADER_SIZE + 4, 4) ==
             static_cast<unsigned int>(i + 1));
      assert(call(NYAMP_SPK_ENROL_ADD, range, 24) == NYAMP_MODEL_DUPLICATE);
    }
  assert(call(NYAMP_SPK_ENROL_COMMIT, nullptr, 0) == 0);
  assert(size == NYAMP_WIRE_HEADER_SIZE + 4 + 8 + 400);
  std::uint8_t reference[400];
  std::memcpy(reference, response + NYAMP_WIRE_HEADER_SIZE + 12, 400);
  assert(call(NYAMP_SPK_TEMPLATE_PUT, reference, 400) == 0);
  nyamp_spk_put(range + 8, 48000, 8);
  nyamp_spk_put(range + 16, 64000, 8);
  assert(call(NYAMP_SPK_VERIFY, range, 24) == 0 && size == 0);
  wait();
  assert(nyamp_spk_get(out.data + NYAMP_WIRE_HEADER_SIZE, 4) == 0);
  assert(std::abs(nyamp_spk_float_get(out.data + NYAMP_WIRE_HEADER_SIZE + 4) -
                  .8f) < 1e-5);
  assert(call(NYAMP_SPK_DELETE, nullptr, 0) == 0);
  source.ring->Close();
  assert(call(NYAMP_SPK_VERIFY, range, 24) == NYAMP_MODEL_NOT_READY);
  std::puts("SPEAKER_SERVICE_PASS stream_identity bounds distinct_takes "
            "enrollment template verify delete closed_capture");
}
