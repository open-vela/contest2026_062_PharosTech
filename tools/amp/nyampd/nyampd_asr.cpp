/****************************************************************************
 * tools/amp/nyampd/nyampd_asr.cpp
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

#include "nyampd_asr.h"

#include <algorithm>
#include <vector>

namespace nyamp
{
namespace
{

/* How long the worker sleeps in the ring when no audio arrives.  It only
 * bounds how late a deadline is noticed; END and CANCEL wake it at once.
 */

constexpr int kFetchTimeoutMs = 100;

std::int32_t Wire(models::Status status)
{
  return -static_cast<std::int32_t>(status);
}

/****************************************************************************
 * Name: TextPublisher
 *
 * Description:
 *   Turns successive whole hypotheses into EVENT_PARTIAL frames.  The
 *   receiver keeps one string: RESYNC replaces it, anything else appends.
 *   A hypothesis that merely grew is sent as its new suffix; one that
 *   rewrote earlier tokens -- a transducer may -- is sent whole.  A shed
 *   frame leaves the receiver behind by an unknown amount, so the next one
 *   is always a RESYNC.
 *
 ****************************************************************************/

class TextPublisher
{
public:
  TextPublisher(FrameQueue *queue, std::uint32_t generation,
                std::uint64_t request_id)
      : queue_(queue), generation_(generation), request_id_(request_id)
  {
  }

  void Publish(const std::string &text, std::uint32_t consumed,
               bool endpoint, bool final)
  {
    const bool grew = !resync_ && text.size() >= published_.size() &&
                      text.compare(0, published_.size(), published_) == 0;

    if (grew && text.size() == published_.size() && !endpoint && !final)
      {
        return;
      }

    /* The final text is always a replacement: it is the one frame sequence
     * a receiver that ignored every partial can rely on.
     */
    const bool replace = final || !grew;
    std::size_t from = replace ? 0 : published_.size();
    bool first = true;
    bool sent = true;

    do
      {
        const std::size_t length =
            Utf8Prefix(text, from, NYAMP_ASR_MAX_TEXT);
        const bool last = from + length >= text.size();
        std::uint16_t flags = 0;

        if (first && replace)
          {
            flags |= NYAMP_ASR_PARTIAL_RESYNC;
          }

        if (last && endpoint)
          {
            flags |= NYAMP_ASR_PARTIAL_ENDPOINT;
          }

        if (last && final)
          {
            flags |= NYAMP_ASR_PARTIAL_FINAL;
          }

        std::uint8_t payload[NYAMP_INLINE_MAX];
        std::size_t size = 0;
        Frame frame;

        sent = nyamp_asr_partial_encode(payload, sizeof(payload), &size,
                                        sequence_, consumed, flags,
                                        text.data() + from,
                                        length) == NYAMP_OK &&
               EncodeEvent(&frame, NYAMP_SERVICE_ASR, NYAMP_ASR_EVENT_PARTIAL,
                           request_id_, generation_, payload, size) &&
               queue_->Push(frame, !final);
        if (!sent)
          {
            break;
          }

        ++sequence_;
        from += length;
        first = false;
      }
    while (from < text.size());

    published_ = text;
    resync_ = !sent;
  }

  std::uint32_t sequence() const { return sequence_; }

private:
  FrameQueue *queue_;
  std::uint32_t generation_;
  std::uint64_t request_id_;
  std::string published_;
  std::uint32_t sequence_ = 0;
  bool resync_ = false;
};

} // namespace

AsrService::AsrService(std::uint32_t generation, models::Clock clock,
                       BackendFactory factory, SharedSlot *capture,
                       SlotGate *gate, LeaseMint *mint)
    : generation_(generation), clock_(std::move(clock)),
      factory_(std::move(factory)), capture_(capture), gate_(gate),
      mint_(mint), queue_(kAsrEventQueueLimit), loader_(generation, &queue_)
{
}

AsrService::~AsrService()
{
  cancel_.store(true);

  {
    std::lock_guard<std::mutex> lock(request_mutex_);
    if (request_.ring)
      {
        request_.ring->Notify();
      }
  }

  if (worker_.joinable())
    {
      worker_.join();
    }
}

void AsrService::SetProvisioner(ModelProvisioner *provisioner)
{
  loader_.SetProvisioner(provisioner);
}

void AsrService::SetCaptureSource(CaptureSource *source) { source_ = source; }

void AsrService::SetWaker(std::function<void()> waker)
{
  queue_.SetWaker(std::move(waker));
}

std::string AsrService::Info() const
{
  const char *state = !factory_          ? "none"
                      : loader_.loading() ? "loading"
                      : running_.load()   ? "busy"
                      : loaded_.load()    ? "ready"
                                          : "off";
  return std::string(state) + ":" + std::to_string(Wire(loader_.last_status()));
}

models::Status AsrService::BeginLoad(const std::string &name,
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

  if (running_.load())
    {
      return models::Status::kBusy;
    }

  /* Loading what is already loaded is a success, not a reason to pull the
   * model again; another model needs an UNLOAD first.
   */
  if (loaded_.load())
    {
      return loader_.last_name() == name ? models::Status::kOk
                                         : models::Status::kBusy;
    }

  return loader_.Begin(name, request, deferred, [this](const std::string &path) {
    std::lock_guard<std::mutex> lock(backend_mutex_);
    std::unique_ptr<models::AsrStreamBackend> backend = factory_();
    if (!backend)
      {
        return models::Status::kUnsupported;
      }

    const models::Status status = backend->Load(path);
    if (status == models::Status::kOk)
      {
        backend_ = std::move(backend);
        loaded_.store(true);
      }

    return status;
  });
}

models::Status AsrService::Unload()
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
      /* UNLOAD ends the request, but never by making the transport loop
       * wait for a decode step.
       */
      std::lock_guard<std::mutex> lock(request_mutex_);
      cancel_.store(true);
      if (request_.ring)
        {
          request_.ring->Notify();
        }

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
  return models::Status::kOk;
}

void AsrService::ReleaseGrantLocked()
{
  if (request_.has_grant)
    {
      request_.has_grant = false;
      if (gate_ != nullptr)
        {
          gate_->Release();
        }
    }
}

std::int32_t AsrService::Begin(const nyamp_header_s &request,
                               std::uint32_t sample_rate,
                               std::uint16_t channels, std::uint16_t flags,
                               std::uint32_t max_samples, bool attach,
                               std::uint64_t start_sample,
                               nyamp_buffer_s *grant, bool *has_grant)
{
  if (grant == nullptr || has_grant == nullptr)
    {
      return NYAMP_MODEL_INVALID;
    }

  *has_grant = false;

  if (!factory_)
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  const bool attach_flag = (flags & NYAMP_ASR_BEGIN_ATTACH_KWS) != 0;
  const bool s16 = (flags & NYAMP_AUDIO_BEGIN_S16) != 0;

  if (sample_rate != models::kAsrSampleRate || channels != 1 ||
      (flags & ~NYAMP_AUDIO_BEGIN_ALL) != 0 || attach_flag != attach ||
      (attach && s16) || max_samples > models::kAsrMaxSamples)
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

  Request next;
  next.id = request.request_id;
  next.deadline_ms = request.deadline_ms;
  next.budget = max_samples == 0 ? models::kAsrMaxSamples : max_samples;
  next.format = s16 ? NYAMP_FORMAT_S16 : NYAMP_FORMAT_F32;
  next.attached = attach;

  if (attach)
    {
      next.ring = source_ != nullptr ? source_->Stream() : nullptr;
      if (!next.ring || next.ring->closed())
        {
          return NYAMP_MODEL_NOT_READY;
        }

      /* Older than the ring still holds: the words are gone, and silently
       * starting later would transcribe a different sentence.
       */
      if (start_sample < next.ring->begin())
        {
          return NYAMP_MODEL_INVALID;
        }

      next.start = start_sample;
    }
  else
    {
      if (capture_ == nullptr || gate_ == nullptr ||
          capture_->data() == nullptr)
        {
          return NYAMP_MODEL_UNSUPPORTED;
        }

      /* The capture slot has one owner; while the wake word stream holds
       * it, recognition attaches to that stream instead.
       */
      if (!gate_->Acquire())
        {
          return NYAMP_MODEL_BUSY;
        }

      next.ring = std::make_shared<CaptureRing>(next.budget, 0);
      next.grant = MakeGrant(mint_, capture_->offset(), capture_->capacity(),
                             next.format, 0);
      next.has_grant = true;
    }

  cancel_.store(false);
  end_sample_.store(NYAMP_STREAM_SAMPLE_NOW);
  running_.store(true);

  {
    std::lock_guard<std::mutex> lock(request_mutex_);
    request_ = next;
  }

  try
    {
      if (worker_.joinable())
        {
          worker_.join();
        }

      worker_ = std::thread(&AsrService::Worker, this, next);
    }
  catch (...)
    {
      std::lock_guard<std::mutex> lock(request_mutex_);
      ReleaseGrantLocked();
      request_ = Request();
      running_.store(false);
      return NYAMP_MODEL_BACKEND_ERROR;
    }

  if (next.has_grant)
    {
      *grant = next.grant;
      *has_grant = true;
    }

  return NYAMP_MODEL_OK;
}

std::int32_t AsrService::Push(std::uint64_t request_id,
                              const nyamp_buffer_s &window,
                              std::uint32_t sequence)
{
  std::shared_ptr<CaptureRing> ring;
  std::uint32_t format;

  {
    std::lock_guard<std::mutex> lock(request_mutex_);

    if (!running_.load() || request_.id != request_id)
      {
        return NYAMP_MODEL_NOT_READY;
      }

    /* An attached request, a returned grant, a request already ended, a
     * foreign or stale lease, a range outside the grant, a lost window.
     */
    if (request_.attached || !request_.has_grant ||
        end_sample_.load() != NYAMP_STREAM_SAMPLE_NOW ||
        !WindowInsideGrant(request_.grant, window, request_.format) ||
        sequence != request_.next_sequence)
      {
        return NYAMP_MODEL_INVALID;
      }

    const std::size_t count = window.length / SampleBytes(request_.format);
    if (request_.ring->end() + count > request_.budget)
      {
        return NYAMP_MODEL_INVALID;
      }

    ++request_.next_sequence;
    ring = request_.ring;
    format = request_.format;
  }

  const std::size_t count = window.length / SampleBytes(format);
  if (count != 0)
    {
      std::uint8_t *base = capture_->data();
      if (base == nullptr)
        {
          return NYAMP_MODEL_BACKEND_ERROR;
        }

      std::vector<float> samples(count);
      CopySamples(samples.data(), base + (window.offset - capture_->offset()),
                  count, format);
      ring->Append(samples.data(), count);
    }

  if ((window.flags & NYAMP_BUFFER_LAST) != 0)
    {
      end_sample_.store(ring->end());
      ring->Notify();
    }

  return NYAMP_MODEL_OK;
}

std::int32_t AsrService::End(std::uint64_t request_id,
                             std::uint64_t end_sample)
{
  std::lock_guard<std::mutex> lock(request_mutex_);

  if (!running_.load() || request_.id != request_id)
    {
      return NYAMP_MODEL_NOT_READY;
    }

  /* Ending twice is harmless; moving the end is not. */
  if (end_sample_.load() != NYAMP_STREAM_SAMPLE_NOW)
    {
      return NYAMP_MODEL_OK;
    }

  if (end_sample == NYAMP_STREAM_SAMPLE_NOW)
    {
      end_sample = std::max(request_.ring->end(), request_.start);
    }
  else if (!request_.attached || end_sample < request_.start)
    {
      /* A pushed request has no position but "everything so far". */
      return NYAMP_MODEL_INVALID;
    }

  end_sample_.store(end_sample);
  request_.ring->Notify();
  return NYAMP_MODEL_OK;
}

std::int32_t AsrService::Release(std::uint64_t request_id,
                                 const nyamp_buffer_s &grant)
{
  std::lock_guard<std::mutex> lock(request_mutex_);

  if (request_.id != request_id || request_.attached ||
      grant.lease != request_.grant.lease ||
      grant.generation != request_.grant.generation)
    {
      return NYAMP_MODEL_INVALID;
    }

  /* The end of the request already returned the grant; saying so again is
   * not an error.
   */
  ReleaseGrantLocked();
  return NYAMP_MODEL_OK;
}

std::int32_t AsrService::Cancel(std::uint64_t request_id)
{
  std::lock_guard<std::mutex> lock(request_mutex_);

  if (!running_.load() || request_.id != request_id)
    {
      return NYAMP_MODEL_NOT_READY;
    }

  cancel_.store(true);
  request_.ring->Notify();
  return NYAMP_MODEL_OK;
}

void AsrService::Finish(const Request &request, models::Status status,
                        std::uint32_t sequence)
{
  /* The slot and the session are given back before the terminal event, so a
   * client that starts its next request the moment it sees FINISH is not
   * told BUSY by a worker that has not quite returned yet.
   */
  {
    std::lock_guard<std::mutex> lock(request_mutex_);
    ReleaseGrantLocked();
    request_.ring.reset();
  }

  running_.store(false);

  std::uint8_t payload[NYAMP_ASR_FINISH_SIZE];
  std::size_t size = 0;
  Frame frame;

  if (nyamp_asr_finish_encode(payload, sizeof(payload), &size, Wire(status),
                              sequence) == NYAMP_OK &&
      EncodeEvent(&frame, NYAMP_SERVICE_ASR, NYAMP_ASR_EVENT_FINISH,
                  request.id, generation_, payload, size))
    {
      queue_.Push(frame, false);
    }
}

void AsrService::Worker(Request request)
{
  TextPublisher publisher(&queue_, generation_, request.id);
  models::Status status = models::Status::kOk;

  {
    std::lock_guard<std::mutex> lock(backend_mutex_);
    std::unique_ptr<models::AsrStream> stream =
        backend_ ? backend_->CreateStream() : nullptr;

    if (!stream)
      {
        status = models::Status::kBackendError;
      }

    const models::Stop stop = [this] { return cancel_.load(); };
    const std::uint64_t hard_limit = request.start + request.budget;
    std::vector<float> samples(kAsrStepSamples);
    std::uint64_t cursor = request.start;
    std::uint32_t epoch = request.ring->epoch();
    std::string text;
    bool endpoint = false;

    while (status == models::Status::kOk)
      {
        if (cancel_.load())
          {
            status = models::Status::kCancelled;
            break;
          }

        if (request.deadline_ms != 0 && clock_ &&
            clock_() > request.deadline_ms)
          {
            status = models::Status::kDeadline;
            break;
          }

        const std::uint64_t limit = std::min(end_sample_.load(), hard_limit);
        const CaptureRing::Read read =
            request.ring->Fetch(&cursor, &epoch, samples.data(),
                                samples.size(), limit, kFetchTimeoutMs);

        if (read.count != 0)
          {
            stream->Accept(samples.data(), read.count);
            if (!stream->Decode(stop))
              {
                status = cancel_.load() ? models::Status::kCancelled
                                        : models::Status::kBackendError;
                break;
              }

            if (!stream->Text(&text))
              {
                status = models::Status::kBackendError;
                break;
              }

            /* Only the moment the decoder changes its mind is news. */
            const bool now = stream->Endpoint();
            publisher.Publish(text,
                              static_cast<std::uint32_t>(cursor - request.start),
                              now && !endpoint, false);
            endpoint = now;
          }

        /* Re-read the end: it may have been set while this step decoded. */
        if (cursor >= std::min(end_sample_.load(), hard_limit) ||
            (read.closed && read.count == 0))
          {
            break;
          }
      }

    if (status == models::Status::kOk)
      {
        stream->InputFinished();
        if (!stream->Decode(stop) || !stream->Text(&text))
          {
            status = cancel_.load() ? models::Status::kCancelled
                                    : models::Status::kBackendError;
          }
        else
          {
            publisher.Publish(
                text, static_cast<std::uint32_t>(cursor - request.start),
                false, true);
          }
      }

    /* The stream goes before the lock does: it must not outlive a backend
     * that the next UNLOAD is free to destroy.
     */
    stream.reset();
  }

  Finish(request, status, publisher.sequence());
}

bool AsrService::Poll(Frame *frame) { return queue_.Poll(frame); }

} // namespace nyamp
