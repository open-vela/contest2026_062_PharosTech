/****************************************************************************
 * tools/amp/nyampd/nyampd_kws.cpp
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

#include "nyampd_kws.h"

#include <algorithm>

namespace nyamp
{
namespace
{

constexpr int kFetchTimeoutMs = 200;
constexpr char kDefaultKeywords[] = "keywords.txt";

std::int32_t Wire(models::Status status)
{
  return -static_cast<std::int32_t>(status);
}

/* One path component: the keywords file sits inside the model directory and
 * nowhere else.
 */

bool ValidComponent(const std::string &name)
{
  if (name.empty() || name == "." || name == ".." || name.size() > 255)
    {
      return false;
    }

  for (const char byte : name)
    {
      const unsigned char value = static_cast<unsigned char>(byte);
      if (value < 0x20U || value == 0x7fU || byte == '/' || byte == '\\')
        {
          return false;
        }
    }

  return true;
}

} // namespace

KwsService::KwsService(std::uint32_t generation, BackendFactory factory,
                       SharedSlot *capture, SlotGate *gate, LeaseMint *mint)
    : generation_(generation), factory_(std::move(factory)),
      capture_(capture), gate_(gate), mint_(mint),
      queue_(kKwsEventQueueLimit), loader_(generation, &queue_)
{
}

KwsService::~KwsService()
{
  {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    cancel_.store(true);
    StopStreamLocked();
  }

  if (worker_.joinable())
    {
      worker_.join();
    }
}

void KwsService::SetProvisioner(ModelProvisioner *provisioner)
{
  loader_.SetProvisioner(provisioner);
}

void KwsService::SetWaker(std::function<void()> waker)
{
  queue_.SetWaker(std::move(waker));
}

std::string KwsService::Info() const
{
  const char *state = !factory_          ? "none"
                      : loader_.loading() ? "loading"
                      : running_.load()   ? "listening"
                      : loaded_.load()    ? "ready"
                                          : "off";
  return std::string(state) + ":" + std::to_string(Wire(loader_.last_status()));
}

models::Status KwsService::BeginLoad(const nyamp_kws_load_s &load,
                                     const nyamp_header_s &request,
                                     bool *deferred)
{
  if (deferred != nullptr)
    {
      *deferred = false;
    }

  if (!factory_)
    {
      return models::Status::kUnsupported;
    }

  const std::string directory(load.directory, load.directory_length);
  const std::string keywords =
      load.keywords_length == 0
          ? std::string(kDefaultKeywords)
          : std::string(load.keywords, load.keywords_length);

  if (directory.empty() || !ValidComponent(keywords))
    {
      return models::Status::kInvalid;
    }

  if (running_.load())
    {
      return models::Status::kBusy;
    }

  /* The parameters belong to the model as much as the files do: the same
   * directory with another threshold is another model.
   */
  const std::string identity =
      directory + "/" + keywords + "?" + std::to_string(load.threshold) + "," +
      std::to_string(load.score) + "," +
      std::to_string(load.max_active_paths) + "," +
      std::to_string(load.num_trailing_blanks);

  if (loaded_.load())
    {
      std::lock_guard<std::mutex> lock(labels_mutex_);
      return identity_ == identity ? models::Status::kOk
                                   : models::Status::kBusy;
    }

  KwsModel model;
  model.threshold = load.threshold;
  model.score = load.score;
  model.max_active_paths = load.max_active_paths;
  model.num_trailing_blanks = load.num_trailing_blanks;

  return loader_.Begin(
      directory, request, deferred,
      [this, model, keywords, identity](const std::string &path) {
        std::lock_guard<std::mutex> lock(backend_mutex_);
        std::unique_ptr<KwsBackend> backend = factory_();
        if (!backend)
          {
            return models::Status::kUnsupported;
          }

        KwsModel resolved = model;
        resolved.directory = path;
        resolved.keywords_file = path + "/" + keywords;

        const models::Status status = backend->Load(resolved);
        if (status == models::Status::kOk)
          {
            std::lock_guard<std::mutex> labels_lock(labels_mutex_);
            labels_ = backend->Labels();
            identity_ = identity;
            backend_ = std::move(backend);
            loaded_.store(true);
          }

        return status;
      });
}

bool KwsService::StopStreamLocked()
{
  if (!running_.load() || !stream_.ring)
    {
      return false;
    }

  stream_.ring->Close();
  return true;
}

models::Status KwsService::Unload()
{
  if (!factory_)
    {
      return models::Status::kUnsupported;
    }

  if (loader_.loading())
    {
      return models::Status::kBusy;
    }

  if (running_.load())
    {
      /* UNLOAD ends the stream; the model goes once the worker has let go
       * of it, which the caller learns from EVENT_FINISH.
       */
      std::lock_guard<std::mutex> lock(stream_mutex_);
      StopStreamLocked();
      return models::Status::kBusy;
    }

  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (!backend_)
    {
      return models::Status::kInvalid;
    }

  backend_->Unload();
  backend_.reset();
  loaded_.store(false);

  std::lock_guard<std::mutex> labels_lock(labels_mutex_);
  labels_.clear();
  identity_.clear();
  return models::Status::kOk;
}

std::int32_t KwsService::Begin(const nyamp_header_s &request,
                               std::uint32_t sample_rate,
                               std::uint16_t channels, std::uint16_t flags,
                               std::uint32_t window_samples,
                               nyamp_buffer_s *grant)
{
  if (grant == nullptr)
    {
      return NYAMP_MODEL_INVALID;
    }

  if (!factory_)
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  if (window_samples == 0)
    {
      window_samples = NYAMP_KWS_WINDOW_SAMPLES_MAX;
    }

  if (sample_rate != models::kAsrSampleRate || channels != 1 ||
      (flags & ~NYAMP_AUDIO_BEGIN_S16) != 0 ||
      window_samples < NYAMP_KWS_WINDOW_SAMPLES_MIN ||
      window_samples > NYAMP_KWS_WINDOW_SAMPLES_MAX)
    {
      return NYAMP_MODEL_INVALID;
    }

  if (!loaded_.load() || loader_.loading())
    {
      return NYAMP_MODEL_NOT_READY;
    }

  if (running_.load())
    {
      return NYAMP_MODEL_BUSY;
    }

  if (capture_ == nullptr || gate_ == nullptr || capture_->data() == nullptr)
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  if (!gate_->Acquire())
    {
      return NYAMP_MODEL_BUSY;
    }

  StreamState next;
  next.id = request.request_id;
  next.format = (flags & NYAMP_AUDIO_BEGIN_S16) != 0 ? NYAMP_FORMAT_S16
                                                     : NYAMP_FORMAT_F32;
  next.window_samples = window_samples;
  next.ring = std::make_shared<CaptureRing>(
      static_cast<std::size_t>(NYAMP_KWS_RING_SECONDS) *
          models::kAsrSampleRate,
      0);
  next.grant = MakeGrant(mint_, capture_->offset(), capture_->capacity(),
                         next.format, 0);

  cancel_.store(false);
  running_.store(true);

  {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    stream_ = next;
  }

  try
    {
      if (worker_.joinable())
        {
          worker_.join();
        }

      worker_ = std::thread(&KwsService::Worker, this, next.id, next.ring);
    }
  catch (...)
    {
      std::lock_guard<std::mutex> lock(stream_mutex_);
      stream_ = StreamState();
      gate_->Release();
      running_.store(false);
      return NYAMP_MODEL_BACKEND_ERROR;
    }

  *grant = next.grant;
  return NYAMP_MODEL_OK;
}

std::int32_t KwsService::Push(const nyamp_kws_push_s &push,
                              std::uint64_t *next_sample)
{
  std::shared_ptr<CaptureRing> ring;
  std::uint32_t format;
  bool restart;

  {
    std::lock_guard<std::mutex> lock(stream_mutex_);

    if (!running_.load() || !stream_.ring || stream_.ring->closed())
      {
        return NYAMP_MODEL_NOT_READY;
      }

    const std::size_t bytes = SampleBytes(stream_.format);
    const std::size_t count = push.buffer.length / bytes;

    if ((push.flags & ~NYAMP_KWS_PUSH_DISCONTINUITY) != 0 ||
        !WindowInsideGrant(stream_.grant, push.buffer, stream_.format) ||
        count == 0 || count > stream_.window_samples)
      {
        return NYAMP_MODEL_INVALID;
      }

    /* A position other than the expected one is a discontinuity, not an
     * error: a window was lost, or capture was paused while the robot
     * spoke.  Going BACK is an error, because every offset ever reported
     * for this stream would become ambiguous.
     */
    if (push.stream_sample < stream_.next_sample)
      {
        return NYAMP_MODEL_INVALID;
      }

    restart = (push.flags & NYAMP_KWS_PUSH_DISCONTINUITY) != 0 ||
              push.stream_sample != stream_.next_sample ||
              push.sequence != stream_.next_sequence;

    stream_.next_sequence = push.sequence + 1;
    stream_.next_sample = push.stream_sample + count;
    ring = stream_.ring;
    format = stream_.format;
  }

  const std::size_t count = push.buffer.length / SampleBytes(format);
  std::uint8_t *base = capture_->data();
  if (base == nullptr)
    {
      return NYAMP_MODEL_BACKEND_ERROR;
    }

  std::vector<float> samples(count);
  CopySamples(samples.data(),
              base + (push.buffer.offset - capture_->offset()), count, format);

  if (restart)
    {
      ring->Restart(push.stream_sample);
    }

  ring->Append(samples.data(), count);

  if (next_sample != nullptr)
    {
      *next_sample = push.stream_sample + count;
    }

  return NYAMP_MODEL_OK;
}

std::int32_t KwsService::End()
{
  std::lock_guard<std::mutex> lock(stream_mutex_);
  return StopStreamLocked() ? NYAMP_MODEL_OK : NYAMP_MODEL_NOT_READY;
}

void KwsService::Cancel(std::uint64_t request_id)
{
  std::lock_guard<std::mutex> lock(stream_mutex_);

  if (running_.load() && stream_.id == request_id)
    {
      cancel_.store(true);
      StopStreamLocked();
    }
}

std::int32_t KwsService::List(std::uint8_t *body, std::size_t capacity,
                              std::size_t *size)
{
  if (!factory_)
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  if (!loaded_.load())
    {
      return NYAMP_MODEL_NOT_READY;
    }

  std::lock_guard<std::mutex> lock(labels_mutex_);

  if (nyamp_kws_labels_begin(body, capacity, size) != NYAMP_OK)
    {
      return NYAMP_MODEL_BACKEND_ERROR;
    }

  for (const std::string &label : labels_)
    {
      if (nyamp_kws_labels_append(body, capacity, size, label.data(),
                                  label.size()) != NYAMP_OK)
        {
          /* A keyword file with more labels than one frame holds. */
          return NYAMP_MODEL_BACKEND_ERROR;
        }
    }

  return NYAMP_MODEL_OK;
}

std::shared_ptr<CaptureRing> KwsService::Stream()
{
  std::lock_guard<std::mutex> lock(stream_mutex_);
  return running_.load() ? stream_.ring : nullptr;
}

void KwsService::Worker(std::uint64_t request_id,
                        std::shared_ptr<CaptureRing> ring)
{
  models::Status status = models::Status::kOk;
  std::uint32_t sequence = 0;

  {
    std::lock_guard<std::mutex> lock(backend_mutex_);
    const models::Stop stop = [this] { return cancel_.load(); };
    std::vector<float> samples(kKwsStepSamples);
    std::uint64_t cursor = ring->begin();
    std::uint32_t epoch = ring->epoch();

    /* The spotter counts samples from its last Reset; `base` is where that
     * count started in the stream.
     */
    std::uint64_t base = cursor;

    if (!backend_ || backend_->Reset() != models::Status::kOk)
      {
        status = models::Status::kBackendError;
      }

    const KwsBackend::Sink sink = [&](const KwsHit &hit) {
      nyamp_kws_detected_s detected{};
      std::uint8_t payload[NYAMP_INLINE_MAX];
      std::size_t size = 0;
      Frame frame;

      /* The phrase offsets come from the decoder's token timestamps, and in
       * a stream that has been running for a while those have been seen to
       * name tokens from long before the phrase (measured on the host: a
       * trigger whose "phrase" lay a minute in the past).  An offset is
       * only passed on when it can be one: a phrase of plausible length
       * that ended shortly before the trigger.  Without the flag the
       * control domain falls back on trigger_sample, which is always this
       * service's own count.
       */
      const bool offsets =
          hit.has_offsets && hit.start < hit.end && hit.end <= hit.trigger &&
          hit.end - hit.start <= kKwsLongestPhraseSamples &&
          hit.trigger - hit.end <= kKwsLongestLatencySamples;

      detected.sequence = sequence;
      detected.keyword_id = hit.keyword_id;
      detected.flags = static_cast<std::uint16_t>(
          (offsets ? NYAMP_KWS_DETECTED_HAS_OFFSETS : 0U) |
          (hit.has_score ? NYAMP_KWS_DETECTED_HAS_SCORE : 0U));
      detected.score = hit.has_score ? hit.score : 0.0f;
      detected.start_sample = offsets ? base + hit.start : 0;
      detected.end_sample = offsets ? base + hit.end : 0;
      detected.trigger_sample = base + hit.trigger;
      detected.label = hit.label.data();
      detected.label_length = static_cast<std::uint32_t>(
          std::min<std::size_t>(hit.label.size(), NYAMP_KWS_MAX_LABEL));

      /* A control domain that cannot take an event loses that event and
       * nothing else; the stream keeps running.
       */
      if (nyamp_kws_detected_encode(payload, sizeof(payload), &size,
                                    &detected) == NYAMP_OK &&
          EncodeEvent(&frame, NYAMP_SERVICE_KWS, NYAMP_KWS_EVENT_DETECTED,
                      request_id, generation_, payload, size) &&
          queue_.Push(frame, true))
        {
          ++sequence;
        }

      return true;
    };

    while (status == models::Status::kOk)
      {
        if (cancel_.load())
          {
            status = models::Status::kCancelled;
            break;
          }

        const CaptureRing::Read read =
            ring->Fetch(&cursor, &epoch, samples.data(), samples.size(),
                        NYAMP_STREAM_SAMPLE_NOW, kFetchTimeoutMs);

        if (read.gap)
          {
            /* Stale context must not complete a keyword across the hole. */
            if (backend_->Reset() != models::Status::kOk)
              {
                status = models::Status::kBackendError;
                break;
              }

            base = cursor - read.count;
          }

        if (read.count != 0)
          {
            const models::Status accepted =
                backend_->Accept(samples.data(), read.count, sink, stop);
            if (accepted != models::Status::kOk)
              {
                status = cancel_.load() ? models::Status::kCancelled
                                        : accepted;
                break;
              }
          }
        else if (read.closed)
          {
            break;
          }
      }

    /* A cancel closes the ring too, and the worker may notice the closed
     * ring first; it was still a cancel.
     */
    if (status == models::Status::kOk && cancel_.load())
      {
        status = models::Status::kCancelled;
      }
  }

  /* Whatever ended the stream, nobody may append to or wait on it again. */
  ring->Close();

  {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    stream_.ring.reset();
    gate_->Release();
  }

  running_.store(false);

  std::uint8_t payload[NYAMP_ASR_FINISH_SIZE];
  std::size_t size = 0;
  Frame frame;

  if (nyamp_kws_finish_encode(payload, sizeof(payload), &size, Wire(status),
                              sequence) == NYAMP_OK &&
      EncodeEvent(&frame, NYAMP_SERVICE_KWS, NYAMP_KWS_EVENT_FINISH,
                  request_id, generation_, payload, size))
    {
      queue_.Push(frame, false);
    }
}

bool KwsService::Poll(Frame *frame) { return queue_.Poll(frame); }

} // namespace nyamp
