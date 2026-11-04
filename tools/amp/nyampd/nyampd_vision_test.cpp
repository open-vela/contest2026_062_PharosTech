/****************************************************************************
 * tools/amp/nyampd/nyampd_vision_test.cpp
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

#include "nyamp_vision_wire.h"
#include "nyampd_core.h"
#include "nyampd_vision.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace nyamp;
namespace
{
std::atomic<bool> hold{ false };
std::atomic<int> pixel{ -1 };
class Slot : public SharedSlot
{
public:
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(1024 * 1024, 19);
  std::uint8_t *data() override { return bytes.data(); }
  std::uint32_t offset() const override { return 0x160000; }
  std::uint32_t capacity() const override { return bytes.size(); }
};
class TrackerBackend:public vision::TrackerBackend
{
public:
 bool Load(const std::string &path) override {return path=="/tracker";}
 bool Extract(const std::vector<std::uint8_t>&,bool,vision::TrackFeature *feature)override {feature->assign(1,1);return true;}
 bool Match(const vision::TrackFeature &,const vision::TrackFeature &,std::vector<float>*scores,std::vector<float>*boxes)override
 {
  scores->assign(512,0);boxes->assign(1024,31.75f);
  for(unsigned int i=0;i<256;i++)(*scores)[i]=10;
  (*scores)[136]=0;(*scores)[256+136]=10;return true;
 }
};
class Detector : public vision::Detector
{
public:
  bool SupportsIdentity() const override { return true; }
  bool LoadIdentity(const std::string &) override { return true; }
  bool CanEmbed() const override { return true; }
  bool Embed(const std::uint8_t *,std::size_t,const vision::Face &,vision::Embedding *e) override { e->fill(0);(*e)[0]=1;return true; }
  bool Load(const std::string &path) override { return path == "/fixture"; }
  bool Detect(const std::uint8_t *data, std::size_t bytes,
              std::vector<vision::Face> *faces) override
  {
    while (hold.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(bytes == vision::kNv12Bytes);
    pixel.store(data[0]);
    faces->push_back({ 64, 48, 128, 96, .9f, {} });
    return true;
  }
};
}
int main()
{
  Slot slot;
  AtomicGate gate;
  LeaseMint mint(7);
  VisionService service(
      7, [] { return 100U; }, [] { return std::make_unique<Detector>(); },
      &slot, &gate, &mint, []{return std::make_unique<vision::Tracker>(std::make_unique<TrackerBackend>());});
  std::uint8_t response[NYAMP_RPMSG_MTU];
  std::size_t size = 0;
  std::uint64_t id = 0;
  auto call = [&](unsigned int opcode, const std::uint8_t *body,
                  std::size_t bytes) {
    std::uint8_t request[NYAMP_RPMSG_MTU];
    nyamp_header_s header{ NYAMP_SERVICE_VISION,
                           static_cast<std::uint16_t>(opcode),
                           NYAMP_FLAG_REQUEST,
                           ++id,
                           900,
                           7,
                           static_cast<std::uint32_t>(bytes) };
    assert(nyamp_header_encode(request, sizeof(request), &header) == 0);
    if (bytes)
      std::memcpy(request + NYAMP_WIRE_HEADER_SIZE, body, bytes);
    assert(Dispatch(request, NYAMP_WIRE_HEADER_SIZE + bytes, 100, 7, response,
                    sizeof(response), &size, {}, nullptr, nullptr, nullptr,
                    &service) == 0);
    return size ? static_cast<std::int32_t>(
                      nyamp_vision_get(response + NYAMP_WIRE_HEADER_SIZE, 4))
                : 0;
  };
  assert(call(NYAMP_VISION_BEGIN, nullptr, 0) == NYAMP_MODEL_NOT_READY);
  const std::string path = "/fixture";
  assert(call(NYAMP_VISION_LOAD,
              reinterpret_cast<const std::uint8_t *>(path.data()),
              path.size()) == 0);
  assert(call(NYAMP_VISION_IDENTITY_LOAD,
              reinterpret_cast<const std::uint8_t *>(path.data()), path.size()) == 0);
  assert(call(NYAMP_VISION_BEGIN, nullptr, 0) == 0 && gate.held());
  nyamp_buffer_s grant{};
  assert(nyamp_buffer_decode(&grant, response + NYAMP_WIRE_HEADER_SIZE + 4,
                             size - NYAMP_WIRE_HEADER_SIZE - 4) == 0);
  grant.length = NYAMP_VISION_BYTES;
  std::uint8_t frame[NYAMP_VISION_FRAME_SIZE];
  std::size_t encoded;
  assert(nyamp_buffer_encode(frame, sizeof(frame), &encoded, &grant) == 0);
  nyamp_vision_put(frame + NYAMP_BUFFER_SIZE, 1, 8);
  nyamp_vision_put(frame + NYAMP_BUFFER_SIZE + 8, 1000, 8);
  assert(call(NYAMP_VISION_FRAME, frame, sizeof(frame) - 1) ==
         NYAMP_MODEL_INVALID);
  hold.store(true);
  assert(call(NYAMP_VISION_FRAME, frame, sizeof(frame)) == 0 && size == 0);
  assert(call(NYAMP_VISION_BEGIN, nullptr, 0) == NYAMP_MODEL_BUSY);
  assert(call(NYAMP_VISION_END, nullptr, 0) == 0 && !gate.held());
  hold.store(false);
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
  wait();
  assert(static_cast<std::int32_t>(nyamp_vision_get(
             out.data + NYAMP_WIRE_HEADER_SIZE, 4)) == NYAMP_MODEL_CANCELLED);
  for (int n = 0; n < 100; n++)
    {
      if (call(NYAMP_VISION_BEGIN, nullptr, 0) == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  nyamp_buffer_s next{};
  assert(nyamp_buffer_decode(&next, response + NYAMP_WIRE_HEADER_SIZE + 4,
                             size - NYAMP_WIRE_HEADER_SIZE - 4) == 0);
  assert(next.lease != grant.lease);
  assert(call(NYAMP_VISION_FRAME, frame, sizeof(frame)) ==
         NYAMP_MODEL_STALE_GENERATION);
  next.length = NYAMP_VISION_BYTES;
  assert(nyamp_buffer_encode(frame, sizeof(frame), &encoded, &next) == 0);
  hold.store(true);
  assert(call(NYAMP_VISION_FRAME, frame, sizeof(frame)) == 0 && size == 0);
  slot.bytes[0] = 77;
  hold.store(false);
  wait();
  assert(pixel.load() == 19);
  auto *result = out.data + NYAMP_WIRE_HEADER_SIZE;
  assert(nyamp_vision_get(result, 4) == 0);
  assert(nyamp_vision_get(result + 4 + 16, 4) == 1);
  assert(std::abs(nyamp_vision_get_float(result + 4 + 20) - .1f) < .0001f);
  for (int n = 0; n < 100; n++)
    {
      if (call(NYAMP_VISION_FRAME, frame, sizeof(frame)) ==
          NYAMP_MODEL_DUPLICATE)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  std::uint8_t embedded[NYAMP_VISION_EMBED_SIZE];std::memcpy(embedded,frame,sizeof(frame));
  nyamp_vision_put(embedded+NYAMP_BUFFER_SIZE,2,8);
  nyamp_vision_put(embedded+NYAMP_BUFFER_SIZE+8,2000,8);
  nyamp_vision_put(embedded+NYAMP_VISION_FRAME_SIZE,5,4);
  assert(call(NYAMP_VISION_EMBED,embedded,sizeof(embedded))==NYAMP_MODEL_INVALID);
  nyamp_vision_put(embedded+NYAMP_VISION_FRAME_SIZE,0,4);
  assert(call(NYAMP_VISION_EMBED,embedded,sizeof(embedded))==0&&size==0);wait();
  result=out.data+NYAMP_WIRE_HEADER_SIZE;
  assert(nyamp_vision_get(result,4)==0);
  assert(out.size==NYAMP_WIRE_HEADER_SIZE+4+NYAMP_VISION_EMBED_HEADER+NYAMP_VISION_FACE_SIZE+128*2);
  assert(nyamp_vision_get(result+4+NYAMP_VISION_EMBED_HEADER+NYAMP_VISION_FACE_SIZE,2)==32767);
  assert(call(NYAMP_VISION_END, nullptr, 0) == 0 && !gate.held());
  assert(call(NYAMP_VISION_UNLOAD,nullptr,0)==0);
  const std::string tracker_path="/tracker";
  assert(call(NYAMP_VISION_TRACK_LOAD,reinterpret_cast<const std::uint8_t*>(tracker_path.data()),tracker_path.size())==0);
  assert(call(NYAMP_VISION_BEGIN,nullptr,0)==0);
  assert(nyamp_buffer_decode(&next,response+NYAMP_WIRE_HEADER_SIZE+4,size-NYAMP_WIRE_HEADER_SIZE-4)==0);
  next.length=NYAMP_VISION_BYTES;
  std::uint8_t tracked[NYAMP_VISION_TRACK_INIT_SIZE]{};
  assert(nyamp_buffer_encode(tracked,sizeof(tracked),&encoded,&next)==0);
  nyamp_vision_put(tracked+NYAMP_BUFFER_SIZE,1,8);
  nyamp_vision_put(tracked+NYAMP_BUFFER_SIZE+8,1000,8);
  const float roi[]={.375f,1.0f/3,.25f,1.0f/3};
  for(unsigned int i=0;i<4;i++)nyamp_vision_put_float(tracked+NYAMP_VISION_FRAME_SIZE+i*4,roi[i]);
  assert(call(NYAMP_VISION_TRACK_INIT,tracked,sizeof(tracked))==0&&size==0);wait();
  assert(out.size==NYAMP_WIRE_HEADER_SIZE+4+NYAMP_VISION_TRACK_RESULT_SIZE);
  result=out.data+NYAMP_WIRE_HEADER_SIZE;assert(nyamp_vision_get(result,4)==0&&nyamp_vision_get(result+4+16,4)==1);
  for(unsigned int i=1;i<=31;i++)
    {
      nyamp_vision_put(tracked+NYAMP_BUFFER_SIZE,i+1,8);
      nyamp_vision_put(tracked+NYAMP_BUFFER_SIZE+8,1000+i*100,8);
      std::int32_t status=NYAMP_MODEL_BUSY;
      for(unsigned int n=0;n<100&&status==NYAMP_MODEL_BUSY;n++)
        {status=call(NYAMP_VISION_TRACK_FRAME,tracked,NYAMP_VISION_FRAME_SIZE);if(status==NYAMP_MODEL_BUSY)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
      assert(status==0&&size==0);wait();
    }
  result=out.data+NYAMP_WIRE_HEADER_SIZE;assert(nyamp_vision_get(result,4)==0&&nyamp_vision_get(result+4+16,4)==2);
  assert(call(NYAMP_VISION_END,nullptr,0)==0);
  std::puts("TRACKER_SERVICE_PASS independent_model shared_lease async_init learning_result");
  std::puts("VISION_SERVICE_PASS grant_bounds stale_lease async_copy "
            "cancellation geometry replay");
}
