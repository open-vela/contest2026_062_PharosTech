/****************************************************************************
 * tools/amp/nyampd/nyampd_tts.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_TTS_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_TTS_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nyamp_models.h"
#include "nyamp_protocol.h"
#include "nyampd_audio.h"
#include "nyampd_loader.h"

namespace nyamp
{

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

constexpr std::size_t kTtsEventQueueLimit = 16;

/* The vocoder's one fixed bucket, in latent frames and in samples. */

constexpr std::size_t kTtsBucketFrames = 512;
constexpr std::size_t kTtsSamplesPerFrame = 512;

/* How long a published window may stay unreleased before the request is
 * given up.  A consumer that never releases can only stall its own service,
 * but it must not stall it -- and hold the shared slot -- forever.
 */

constexpr int kTtsReleaseTimeoutMs = 30000;

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct TtsUnit
{
  std::vector<std::int64_t> phonemes;
  std::vector<std::int64_t> tones;
  std::string text; /* Normalised text the ids came from. */
  bool over_budget = false;
};

/****************************************************************************
 * Name: TtsFrontend
 *
 * Description:
 *   Text to model inputs: normalisation, segmentation, grapheme-to-phoneme
 *   and the split into units that fit `max_frames`.  The real one is
 *   tools/amp/g2p (CreateMeloFrontend); the tests also use a scripted one so
 *   they run without the 7 MB lexicon.
 *
 ****************************************************************************/

class TtsFrontend
{
public:
  virtual ~TtsFrontend() = default;
  virtual models::Status Load(const std::string &directory) = 0;
  virtual std::vector<TtsUnit> Process(const std::string &text,
                                       std::size_t max_frames) = 0;
};

std::unique_ptr<TtsFrontend> CreateMeloFrontend();

/****************************************************************************
 * Name: TtsService
 *
 * Description:
 *   Text to speech behind the RPMsg endpoint.  SYNTH_TEXT chunks are
 *   assembled on the transport thread; the last one starts a worker that
 *   splits the text into units, synthesizes them one after the other and
 *   hands the PCM over window by window through NYAMP_SLOT_SHARED.
 *
 *   One window is outstanding at a time.  The worker writes it, sends
 *   EVENT_PCM and waits for the RELEASE that echoes its lease before it
 *   touches the slot again, so the control domain never reads a window that
 *   is being rewritten.  A cancel is honoured between units, between the two
 *   halves of a synthesis, and while waiting for a release.
 *
 *   The slot is shared with model pulls; the gate is held for the life of a
 *   request and a request that finds it taken is answered BUSY.
 *
 *   Audio is never truncated.  A unit the encoder stretches past the bucket
 *   is split again with a smaller budget, and text that cannot be split any
 *   further ends the request with UNSUPPORTED.
 *
 ****************************************************************************/

class TtsService
{
public:
  using BackendFactory = std::function<std::unique_ptr<models::Backend>()>;
  using FrontendFactory = std::function<std::unique_ptr<TtsFrontend>()>;

  TtsService(std::uint32_t generation, models::Clock clock,
             BackendFactory backend, FrontendFactory frontend,
             SharedSlot *slot, SlotGate *gate, LeaseMint *mint);
  ~TtsService();

  TtsService(const TtsService &) = delete;
  TtsService &operator=(const TtsService &) = delete;

  void SetProvisioner(ModelProvisioner *provisioner);
  void SetWaker(std::function<void()> waker);

  /* Tests shorten this to see the unreleased-window path. */
  void SetReleaseTimeout(int timeout_ms) { release_timeout_ms_ = timeout_ms; }

  bool Supported() const
  {
    return static_cast<bool>(backend_factory_) &&
           static_cast<bool>(frontend_factory_);
  }

  std::string Info() const;

  models::Status BeginLoad(const std::string &name,
                           const nyamp_header_s &request, bool *deferred);
  models::Status Unload();

  /* Wire statuses.  BeginText: `*started` says the chunk completed the text
   * and the worker was launched.
   */

  std::int32_t BeginText(const nyamp_tts_text_s &chunk,
                         const std::uint8_t *bytes, std::uint64_t request_id,
                         std::uint64_t deadline_ms, bool *started);
  std::int32_t Release(std::uint64_t request_id, const nyamp_buffer_s &window);
  std::int32_t Cancel(std::uint64_t request_id);

  bool Poll(Frame *frame);

private:
  struct Pending
  {
    std::uint64_t request_id = 0;
    std::uint64_t deadline_ms = 0;
    nyamp_tts_text_s first{};
    std::string text;
    bool active = false;
  };

  struct Job
  {
    std::uint64_t request_id;
    std::uint64_t deadline_ms;
    std::string text;
    std::uint32_t speaker_id;
    float speed;
    std::uint32_t window_samples;
  };

  void Worker(Job job);
  models::Status Synthesize(const TtsUnit &unit, const Job &job,
                            std::vector<float> *pcm);
  models::Status Deliver(const Job &job, const std::vector<float> &pcm,
                         bool last_unit, std::uint32_t *sequence,
                         std::uint32_t *total_samples);
  models::Status StopReason(const Job &job) const;

  std::uint32_t generation_;
  models::Clock clock_;
  BackendFactory backend_factory_;
  FrontendFactory frontend_factory_;
  SharedSlot *slot_;
  SlotGate *gate_;
  LeaseMint *mint_;
  int release_timeout_ms_ = kTtsReleaseTimeoutMs;

  FrameQueue queue_;
  ModelLoader loader_;

  /* Held by the worker for the whole of a request; see AsrService. */
  std::mutex backend_mutex_;
  std::unique_ptr<models::Backend> backend_;
  std::unique_ptr<TtsFrontend> frontend_;
  std::atomic<bool> loaded_{ false };

  std::mutex pending_mutex_;
  Pending pending_;

  std::atomic<bool> running_{ false };
  std::atomic<bool> cancel_{ false };
  std::atomic<std::uint64_t> active_id_{ 0 };

  /* The window the control domain currently holds. */
  std::mutex window_mutex_;
  std::condition_variable released_;
  std::uint64_t outstanding_lease_ = 0;

  std::thread worker_;
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_TTS_H */
