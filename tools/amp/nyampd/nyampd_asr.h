/****************************************************************************
 * tools/amp/nyampd/nyampd_asr.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_ASR_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_ASR_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "nyamp_models.h"
#include "nyamp_protocol.h"
#include "nyamp_streaming.h"
#include "nyampd_audio.h"
#include "nyampd_loader.h"

namespace nyamp
{

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

constexpr std::size_t kAsrEventQueueLimit = 64;

/* Samples handed to the decoder per step.  0.2 s bounds both the partial
 * rate (five a second) and how long a cancel can wait behind a decode.
 */

constexpr std::size_t kAsrStepSamples = 3200;

/****************************************************************************
 * Name: CaptureSource
 *
 * Description:
 *   Where an attached request finds the stream it reads: the KWS service.
 *   Null when no stream is running.
 *
 ****************************************************************************/

class CaptureSource
{
public:
  virtual ~CaptureSource() = default;
  virtual std::shared_ptr<CaptureRing> Stream() = 0;
};

/****************************************************************************
 * Name: AsrService
 *
 * Description:
 *   Streaming speech recognition behind the RPMsg endpoint: one model, one
 *   request at a time, decoded incrementally on a worker while the audio is
 *   still arriving.
 *
 *   Audio reaches a request in one of two ways, and the worker cannot tell
 *   them apart: it reads a CaptureRing by absolute sample position either
 *   way.  A pushed request owns a private ring that PUSH appends to; an
 *   attached request (NYAMP_ASR_BEGIN_ATTACH_KWS) reads the ring of the
 *   wake word stream from the offset it names.
 *
 *   Every message of a request -- BEGIN, PUSH, END, RELEASE, CANCEL --
 *   carries the request_id of its BEGIN, and so do the events.
 *
 *   All entry points run on the transport thread and none of them blocks on
 *   inference: a PUSH is answered once its samples were copied out of the
 *   shared window, before they are decoded.
 *
 ****************************************************************************/

class AsrService
{
public:
  using BackendFactory =
      std::function<std::unique_ptr<models::AsrStreamBackend>()>;

  /* `capture` and `gate` are the capture slot and its ownership flag, both
   * shared with the KWS service.  Either may be null on a build that has no
   * shared region; pushed requests are then refused.
   */

  AsrService(std::uint32_t generation, models::Clock clock,
             BackendFactory factory, SharedSlot *capture, SlotGate *gate,
             LeaseMint *mint);
  ~AsrService();

  AsrService(const AsrService &) = delete;
  AsrService &operator=(const AsrService &) = delete;

  void SetProvisioner(ModelProvisioner *provisioner);
  void SetCaptureSource(CaptureSource *source);
  void SetWaker(std::function<void()> waker);

  /* Whether this build has a recognizer at all (HEALTH capability). */
  bool Supported() const { return static_cast<bool>(factory_); }

  /* "off", "loading", "ready" or "busy", then the last load's status. */
  std::string Info() const;

  models::Status BeginLoad(const std::string &name,
                           const nyamp_header_s &request, bool *deferred);

  /* Ends the request in flight, if any.  While it is still winding down the
   * answer is kBusy and the caller retries after its EVENT_FINISH.
   */
  models::Status Unload();

  /* All of these return a wire status (nyamp_model_status_e).
   *
   * Begin: `attach` selects the attached form and `start_sample` is then the
   * stream position recognition starts at.  A pushed request gets `*grant`
   * (the whole capture slot) and `*has_grant` set.
   */

  std::int32_t Begin(const nyamp_header_s &request, std::uint32_t sample_rate,
                     std::uint16_t channels, std::uint16_t flags,
                     std::uint32_t max_samples, bool attach,
                     std::uint64_t start_sample, nyamp_buffer_s *grant,
                     bool *has_grant);
  std::int32_t Push(std::uint64_t request_id, const nyamp_buffer_s &window,
                    std::uint32_t sequence);
  std::int32_t End(std::uint64_t request_id, std::uint64_t end_sample);
  std::int32_t Release(std::uint64_t request_id, const nyamp_buffer_s &grant);
  std::int32_t Cancel(std::uint64_t request_id);

  bool Poll(Frame *frame);

private:
  struct Request
  {
    std::uint64_t id = 0;
    std::uint64_t deadline_ms = 0;
    std::uint64_t start = 0;  /* Stream position of the first sample.    */
    std::uint64_t budget = 0; /* Most samples this request may consume.  */
    std::uint32_t format = NYAMP_FORMAT_F32;
    std::uint32_t next_sequence = 0;
    bool attached = false;
    bool has_grant = false;
    nyamp_buffer_s grant{};
    std::shared_ptr<CaptureRing> ring;
  };

  void Worker(Request request);
  void Finish(const Request &request, models::Status status,
              std::uint32_t sequence);
  void ReleaseGrantLocked();

  std::uint32_t generation_;
  models::Clock clock_;
  BackendFactory factory_;
  SharedSlot *capture_;
  SlotGate *gate_;
  LeaseMint *mint_;
  CaptureSource *source_ = nullptr;

  FrameQueue queue_;
  ModelLoader loader_;

  /* The backend and "is it loaded".  The worker holds backend_mutex_ for the
   * whole of a request, so nothing on the transport thread may take it while
   * running_ is set; loaded_ is what that thread reads instead.
   */
  std::mutex backend_mutex_;
  std::unique_ptr<models::AsrStreamBackend> backend_;
  std::atomic<bool> loaded_{ false };

  /* The request as the transport thread sees it.  request_mutex_ is never
   * held across anything slower than a window copy.
   */
  mutable std::mutex request_mutex_;
  Request request_;
  std::atomic<bool> running_{ false };
  std::atomic<bool> cancel_{ false };
  std::atomic<std::uint64_t> end_sample_{ NYAMP_STREAM_SAMPLE_NOW };

  std::thread worker_;
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_ASR_H */
