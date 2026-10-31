/****************************************************************************
 * tools/amp/nyampd/nyampd_kws.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_KWS_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_KWS_H

#include <atomic>
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
#include "nyampd_asr.h"
#include "nyampd_audio.h"
#include "nyampd_loader.h"

namespace nyamp
{

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

constexpr std::size_t kKwsEventQueueLimit = 32;

/* Samples handed to the spotter per step: 0.1 s, so a detection is reported
 * at most that long after the decoder could have known.
 */

constexpr std::size_t kKwsStepSamples = 1600;

/* What a reported phrase may look like for its offsets to be believed: at
 * most six seconds long (the evaluation measured 1.0 .. 3.4 s) and ended at
 * most two seconds before the trigger (measured 0.24 .. 0.61 s).
 */

constexpr std::uint64_t kKwsLongestPhraseSamples = 6 * 16000;
constexpr std::uint64_t kKwsLongestLatencySamples = 2 * 16000;

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct KwsModel
{
  std::string directory;     /* encoder/decoder/joiner.onnx, tokens.txt. */
  std::string keywords_file; /* Full path.                               */
  float threshold = 0.0f;    /* Zero selects the evaluated default.      */
  float score = 0.0f;
  std::uint16_t max_active_paths = 0;
  std::uint16_t num_trailing_blanks = 0;
};

struct KwsHit
{
  std::string label;
  std::uint16_t keyword_id = NYAMP_KWS_KEYWORD_UNKNOWN;
  bool has_offsets = false;
  bool has_score = false;
  float score = 0.0f;

  /* Samples since Load or the last Reset. */
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  std::uint64_t trigger = 0;
};

/****************************************************************************
 * Name: KwsBackend
 *
 * Description:
 *   The keyword spotter as the service needs it; tools/amp/voice provides
 *   the real one (CreateSherpaKwsBackend) and the unit tests a scripted one.
 *   One thread calls it at a time.
 *
 ****************************************************************************/

class KwsBackend
{
public:
  using Sink = std::function<bool(const KwsHit &)>;

  virtual ~KwsBackend() = default;
  virtual models::Status Load(const KwsModel &model) = 0;
  virtual void Unload() = 0;
  virtual models::Status Accept(const float *samples, std::size_t count,
                                const Sink &sink,
                                const models::Stop &stop) = 0;
  virtual models::Status Reset() = 0;
  virtual std::vector<std::string> Labels() const = 0;
};

/* Link nyampd_kws_sherpa and the external runtime to get this one. */
std::unique_ptr<KwsBackend> CreateSherpaKwsBackend();

/****************************************************************************
 * Name: KwsService
 *
 * Description:
 *   The always-on wake word listener, and the owner of the capture stream.
 *
 *   BEGIN takes the capture slot and returns a grant over all of it; PUSH
 *   submits windows of it, each with the absolute position of its first
 *   sample.  The transport thread copies a window into a CaptureRing and
 *   answers; a worker follows the ring and runs the spotter.  The ring is
 *   also what an attached ASR request reads (CaptureSource), which is why it
 *   lives here: the stream outlasts every utterance.
 *
 *   One stream exists at a time and one control domain feeds it, so END and
 *   UNLOAD end it whatever request_id they carry: a control-domain task that
 *   was restarted has no other way to get the slot back.
 *
 ****************************************************************************/

class KwsService final : public CaptureSource
{
public:
  using BackendFactory = std::function<std::unique_ptr<KwsBackend>()>;

  KwsService(std::uint32_t generation, BackendFactory factory,
             SharedSlot *capture, SlotGate *gate, LeaseMint *mint);
  ~KwsService() override;

  KwsService(const KwsService &) = delete;
  KwsService &operator=(const KwsService &) = delete;

  void SetProvisioner(ModelProvisioner *provisioner);
  void SetWaker(std::function<void()> waker);

  bool Supported() const { return static_cast<bool>(factory_); }
  std::string Info() const;

  /* `load.directory` is an absolute path or the logical name ("kws"); the
   * keywords name is one path component inside it.
   */
  models::Status BeginLoad(const nyamp_kws_load_s &load,
                           const nyamp_header_s &request, bool *deferred);
  models::Status Unload();

  /* Wire statuses. */

  std::int32_t Begin(const nyamp_header_s &request, std::uint32_t sample_rate,
                     std::uint16_t channels, std::uint16_t flags,
                     std::uint32_t window_samples, nyamp_buffer_s *grant);
  std::int32_t Push(const nyamp_kws_push_s &push, std::uint64_t *next_sample);
  std::int32_t End();
  void Cancel(std::uint64_t request_id);

  /* Appends the labels to a LIST response body. */
  std::int32_t List(std::uint8_t *body, std::size_t capacity,
                    std::size_t *size);

  std::shared_ptr<CaptureRing> Stream() override;

  bool Poll(Frame *frame);

private:
  struct StreamState
  {
    std::uint64_t id = 0;
    std::uint32_t format = NYAMP_FORMAT_F32;
    std::uint32_t window_samples = 0;
    std::uint32_t next_sequence = 0;
    std::uint64_t next_sample = 0;
    nyamp_buffer_s grant{};
    std::shared_ptr<CaptureRing> ring;
  };

  void Worker(std::uint64_t request_id, std::shared_ptr<CaptureRing> ring);
  bool StopStreamLocked();

  std::uint32_t generation_;
  BackendFactory factory_;
  SharedSlot *capture_;
  SlotGate *gate_;
  LeaseMint *mint_;

  FrameQueue queue_;
  ModelLoader loader_;

  /* Held by the worker for the life of a stream; see AsrService. */
  std::mutex backend_mutex_;
  std::unique_ptr<KwsBackend> backend_;
  std::atomic<bool> loaded_{ false };

  mutable std::mutex labels_mutex_;
  std::vector<std::string> labels_;
  std::string identity_; /* Directory, keywords file and parameters. */

  mutable std::mutex stream_mutex_;
  StreamState stream_;
  std::atomic<bool> running_{ false };
  std::atomic<bool> cancel_{ false };

  std::thread worker_;
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_KWS_H */
