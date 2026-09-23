/****************************************************************************
 * tools/amp/nyampd/nyampd_tts.cpp
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

#include "nyampd_tts.h"

#include "nyamp_g2p.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>

namespace nyamp
{
namespace
{

constexpr float kSpeedMin = 0.5f;
constexpr float kSpeedMax = 2.0f;

/* A unit the encoder stretched past the bucket is split again with half the
 * budget; below this the text is one word that cannot be said in 5.9 s.
 */

constexpr std::size_t kSmallestBudgetFrames = 64;

std::int32_t Wire(models::Status status)
{
  return -static_cast<std::int32_t>(status);
}

/****************************************************************************
 * Name: MeloTextFrontend
 *
 * Description:
 *   tools/amp/g2p behind the service's interface.  Process is const and
 *   thread safe there; the unique_ptr only changes in Load.
 *
 ****************************************************************************/

class MeloTextFrontend final : public TtsFrontend
{
public:
  models::Status Load(const std::string &directory) override
  {
    std::string error;

    frontend_ = MeloFrontend::Load(directory, &error);
    if (!frontend_)
      {
        std::fprintf(stderr, "nyampd: tts front end: %s\n", error.c_str());
        return models::Status::kBackendError;
      }

    return models::Status::kOk;
  }

  std::vector<TtsUnit> Process(const std::string &text,
                               std::size_t max_frames) override
  {
    std::vector<TtsUnit> units;

    if (!frontend_)
      {
        return units;
      }

    for (MeloFrontend::Utterance &utterance :
         frontend_->Process(text, max_frames))
      {
        TtsUnit unit;
        unit.phonemes = std::move(utterance.phonemes);
        unit.tones = std::move(utterance.tones);
        unit.text = std::move(utterance.text);
        unit.over_budget = utterance.over_budget;
        units.push_back(std::move(unit));
      }

    return units;
  }

private:
  std::unique_ptr<MeloFrontend> frontend_;
};

} // namespace

std::unique_ptr<TtsFrontend> CreateMeloFrontend()
{
  return std::make_unique<MeloTextFrontend>();
}

TtsService::TtsService(std::uint32_t generation, models::Clock clock,
                       BackendFactory backend, FrontendFactory frontend,
                       SharedSlot *slot, SlotGate *gate, LeaseMint *mint)
    : generation_(generation), clock_(std::move(clock)),
      backend_factory_(std::move(backend)),
      frontend_factory_(std::move(frontend)), slot_(slot), gate_(gate),
      mint_(mint), queue_(kTtsEventQueueLimit), loader_(generation, &queue_)
{
}

TtsService::~TtsService()
{
  {
    std::lock_guard<std::mutex> lock(window_mutex_);
    cancel_.store(true);
  }

  released_.notify_all();
  if (worker_.joinable())
    {
      worker_.join();
    }
}

void TtsService::SetProvisioner(ModelProvisioner *provisioner)
{
  loader_.SetProvisioner(provisioner);
}

void TtsService::SetWaker(std::function<void()> waker)
{
  queue_.SetWaker(std::move(waker));
}

std::string TtsService::Info() const
{
  const char *state = !Supported()       ? "none"
                      : loader_.loading() ? "loading"
                      : running_.load()   ? "busy"
                      : loaded_.load()    ? "ready"
                                          : "off";
  return std::string(state) + ":" + std::to_string(Wire(loader_.last_status()));
}

models::Status TtsService::BeginLoad(const std::string &name,
                                     const nyamp_header_s &request,
                                     bool *deferred)
{
  if (deferred != nullptr)
    {
      *deferred = false;
    }

  if (!Supported())
    {
      return models::Status::kUnsupported;
    }

  if (running_.load())
    {
      return models::Status::kBusy;
    }

  if (loaded_.load())
    {
      return loader_.last_name() == name ? models::Status::kOk
                                         : models::Status::kBusy;
    }

  return loader_.Begin(name, request, deferred, [this](const std::string &path) {
    std::lock_guard<std::mutex> lock(backend_mutex_);
    std::unique_ptr<TtsFrontend> frontend = frontend_factory_();
    std::unique_ptr<models::Backend> backend = backend_factory_();
    if (!frontend || !backend || backend->kind() != models::Kind::kTts)
      {
        return models::Status::kUnsupported;
      }

    /* The front end first: it is the cheap half, so a directory without its
     * lexicon fails before the vocoder is handed to the NPU.
     */
    models::Status status = frontend->Load(path);
    if (status == models::Status::kOk)
      {
        status = backend->Load(path);
      }

    if (status == models::Status::kOk)
      {
        frontend_ = std::move(frontend);
        backend_ = std::move(backend);
        loaded_.store(true);
      }

    return status;
  });
}

models::Status TtsService::Unload()
{
  if (!Supported())
    {
      return models::Status::kUnsupported;
    }

  if (loader_.loading())
    {
      return models::Status::kBusy;
    }

  if (running_.load())
    {
      /* UNLOAD ends the request, but the transport loop does not wait for
       * a synthesis step; the caller retries after EVENT_FINISH.
       */
      {
        std::lock_guard<std::mutex> lock(window_mutex_);
        cancel_.store(true);
      }

      released_.notify_all();
      return models::Status::kBusy;
    }

  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_ = Pending();
  }

  std::lock_guard<std::mutex> lock(backend_mutex_);
  if (!backend_)
    {
      return models::Status::kInvalid;
    }

  backend_->Unload();
  backend_.reset();
  frontend_.reset();
  loaded_.store(false);
  return models::Status::kOk;
}

std::int32_t TtsService::BeginText(const nyamp_tts_text_s &chunk,
                                   const std::uint8_t *bytes,
                                   std::uint64_t request_id,
                                   std::uint64_t deadline_ms, bool *started)
{
  if (started == nullptr || (bytes == nullptr && chunk.length != 0))
    {
      return NYAMP_MODEL_INVALID;
    }

  *started = false;

  if (!Supported())
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  std::lock_guard<std::mutex> lock(pending_mutex_);

  if (chunk.offset == 0)
    {
      /* A first chunk always starts over; whatever was half assembled
       * belonged to a requester that gave up.
       */
      pending_ = Pending();

      if (chunk.total == 0 || chunk.total > NYAMP_TTS_TEXT_MAX_BODY ||
          chunk.speed < kSpeedMin || chunk.speed > kSpeedMax)
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

      pending_.request_id = request_id;
      pending_.deadline_ms = deadline_ms;
      pending_.first = chunk;
      pending_.text.reserve(chunk.total);
      pending_.active = true;
    }
  else if (!pending_.active || pending_.request_id != request_id ||
           pending_.first.total != chunk.total ||
           pending_.text.size() != chunk.offset ||
           pending_.first.speaker_id != chunk.speaker_id ||
           pending_.first.speed != chunk.speed ||
           pending_.first.window_samples != chunk.window_samples)
    {
      pending_ = Pending();
      return NYAMP_MODEL_INVALID;
    }

  if (chunk.length == 0 ||
      pending_.text.size() + chunk.length > pending_.first.total)
    {
      pending_ = Pending();
      return NYAMP_MODEL_INVALID;
    }

  pending_.text.append(reinterpret_cast<const char *>(bytes), chunk.length);
  if (pending_.text.size() < pending_.first.total)
    {
      return NYAMP_MODEL_OK;
    }

  /* The text is whole. */

  Job job{ pending_.request_id,
           pending_.deadline_ms,
           std::move(pending_.text),
           pending_.first.speaker_id,
           pending_.first.speed,
           pending_.first.window_samples };
  pending_ = Pending();

  if (!ValidUtf8(job.text))
    {
      return NYAMP_MODEL_INVALID;
    }

  if (running_.load())
    {
      return NYAMP_MODEL_BUSY;
    }

  if (slot_ == nullptr || gate_ == nullptr || slot_->data() == nullptr)
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  /* One second unless asked otherwise, and never more than the slot. */
  const std::uint32_t slot_samples =
      slot_->capacity() / static_cast<std::uint32_t>(sizeof(float));
  if (job.window_samples == 0)
    {
      job.window_samples = NYAMP_TTS_SAMPLE_RATE;
    }

  job.window_samples = std::min(job.window_samples, slot_samples);
  if (job.window_samples == 0)
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  /* A model pull owns the slot right now. */
  if (!gate_->Acquire())
    {
      return NYAMP_MODEL_BUSY;
    }

  {
    std::lock_guard<std::mutex> window_lock(window_mutex_);
    cancel_.store(false);
    outstanding_lease_ = 0;
  }

  active_id_.store(job.request_id);
  running_.store(true);

  try
    {
      if (worker_.joinable())
        {
          worker_.join();
        }

      worker_ = std::thread(&TtsService::Worker, this, std::move(job));
    }
  catch (...)
    {
      active_id_.store(0);
      running_.store(false);
      gate_->Release();
      return NYAMP_MODEL_BACKEND_ERROR;
    }

  *started = true;
  return NYAMP_MODEL_OK;
}

std::int32_t TtsService::Release(std::uint64_t request_id,
                                 const nyamp_buffer_s &window)
{
  {
    std::lock_guard<std::mutex> lock(window_mutex_);

    /* Only the echo of the window that is out right now counts: a late
     * echo of an earlier window must not free the current one while the
     * control domain is still reading it.
     */
    if (!running_.load() || active_id_.load() != request_id ||
        outstanding_lease_ == 0 || window.lease != outstanding_lease_ ||
        window.generation != generation_)
      {
        return NYAMP_MODEL_INVALID;
      }

    outstanding_lease_ = 0;
  }

  released_.notify_all();
  return NYAMP_MODEL_OK;
}

std::int32_t TtsService::Cancel(std::uint64_t request_id)
{
  {
    std::lock_guard<std::mutex> pending_lock(pending_mutex_);
    if (pending_.active && pending_.request_id == request_id)
      {
        pending_ = Pending();
        return NYAMP_MODEL_OK;
      }
  }

  {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (!running_.load() || active_id_.load() != request_id)
      {
        return NYAMP_MODEL_NOT_READY;
      }

    cancel_.store(true);
  }

  released_.notify_all();
  return NYAMP_MODEL_OK;
}

models::Status TtsService::StopReason(const Job &job) const
{
  if (cancel_.load())
    {
      return models::Status::kCancelled;
    }

  if (job.deadline_ms != 0 && clock_ && clock_() > job.deadline_ms)
    {
      return models::Status::kDeadline;
    }

  return models::Status::kOk;
}

models::Status TtsService::Synthesize(const TtsUnit &unit, const Job &job,
                                      std::vector<float> *pcm)
{
  models::TtsInput input;
  input.phoneme_ids = unit.phonemes;
  input.tone_ids = unit.tones;
  input.speaker_id = job.speaker_id;
  input.speed = job.speed;

  bool format_ok = true;
  pcm->clear();

  const models::Status status = backend_->Run(
      models::Input(std::move(input)),
      [&](models::Output output) {
        const auto *chunk = std::get_if<models::PcmChunk>(&output);
        if (chunk == nullptr || chunk->sample_rate != NYAMP_TTS_SAMPLE_RATE ||
            chunk->channels != 1)
          {
            format_ok = false;
            return false;
          }

        pcm->insert(pcm->end(), chunk->samples.begin(), chunk->samples.end());
        return true;
      },
      [&] { return StopReason(job) != models::Status::kOk; });

  if (!format_ok)
    {
      return models::Status::kBackendError;
    }

  /* More than the bucket holds cannot have come out of it whole. */
  if (status == models::Status::kOk &&
      (pcm->empty() || pcm->size() > kTtsBucketFrames * kTtsSamplesPerFrame))
    {
      return models::Status::kBackendError;
    }

  return status;
}

models::Status TtsService::Deliver(const Job &job,
                                   const std::vector<float> &pcm,
                                   bool last_unit, std::uint32_t *sequence,
                                   std::uint32_t *total_samples)
{
  for (std::size_t offset = 0; offset < pcm.size();
       offset += job.window_samples)
    {
      const std::size_t count =
          std::min<std::size_t>(job.window_samples, pcm.size() - offset);
      const bool last = last_unit && offset + count >= pcm.size();
      std::uint8_t *base = slot_->data();

      if (base == nullptr)
        {
          return models::Status::kBackendError;
        }

      /* Nothing is outstanding here, so the slot is ours to write. */
      std::memcpy(base, pcm.data() + offset, count * sizeof(float));

      nyamp_buffer_s window = MakeGrant(
          mint_, slot_->offset(), slot_->capacity(), NYAMP_FORMAT_F32,
          static_cast<std::uint16_t>(NYAMP_BUFFER_FROM_COMPUTE |
                                     (offset == 0 ? NYAMP_BUFFER_RESYNC : 0U) |
                                     (last ? NYAMP_BUFFER_LAST : 0U)));
      window.length = static_cast<std::uint32_t>(count * sizeof(float));

      std::uint8_t payload[NYAMP_TTS_PCM_HEADER_SIZE];
      std::size_t size = 0;
      Frame frame;

      if (nyamp_tts_pcm_encode(payload, sizeof(payload), &size, &window,
                               *sequence, NYAMP_TTS_SAMPLE_RATE, 1,
                               static_cast<std::uint32_t>(count)) != NYAMP_OK ||
          !EncodeEvent(&frame, NYAMP_SERVICE_TTS, NYAMP_TTS_EVENT_PCM,
                       job.request_id, generation_, payload, size))
        {
          return models::Status::kBackendError;
        }

      std::unique_lock<std::mutex> lock(window_mutex_);

      /* Published before the event can be seen, so the release that answers
       * it always finds the lease it echoes.
       */
      outstanding_lease_ = window.lease;
      queue_.Push(frame, false);
      ++*sequence;
      *total_samples += static_cast<std::uint32_t>(count);

      const auto give_up = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(release_timeout_ms_);
      models::Status stop = models::Status::kOk;

      while (outstanding_lease_ != 0)
        {
          stop = StopReason(job);
          if (stop != models::Status::kOk)
            {
              break;
            }

          /* Short slices: the deadline clock is the caller's, not one this
           * wait can sleep on.
           */
          if (released_.wait_for(lock, std::chrono::milliseconds(50)) ==
                  std::cv_status::timeout &&
              std::chrono::steady_clock::now() >= give_up)
            {
              stop = models::Status::kDeadline;
              break;
            }
        }

      if (stop != models::Status::kOk)
        {
          /* The window is void from here on; see the protocol header. */
          outstanding_lease_ = 0;
          return stop;
        }
    }

  return models::Status::kOk;
}

void TtsService::Worker(Job job)
{
  struct Work
  {
    TtsUnit unit;
    std::size_t budget;
  };

  models::Status status = models::Status::kOk;
  std::uint32_t sequence = 0;
  std::uint32_t total_samples = 0;

  {
    std::lock_guard<std::mutex> lock(backend_mutex_);

    /* Slower speech stretches every unit, so the budget shrinks with it. */
    const std::size_t budget = static_cast<std::size_t>(
        static_cast<float>(kTtsBucketFrames) * std::min(job.speed, 1.0f));
    std::deque<Work> work;

    if (!backend_ || !frontend_)
      {
        status = models::Status::kNotReady;
      }
    else
      {
        for (TtsUnit &unit : frontend_->Process(job.text, budget))
          {
            work.push_back(Work{ std::move(unit), budget });
          }
      }

    std::vector<float> pcm;

    while (status == models::Status::kOk && !work.empty())
      {
        status = StopReason(job);
        if (status != models::Status::kOk)
          {
            break;
          }

        Work current = std::move(work.front());
        work.pop_front();

        status = current.unit.over_budget
                     ? models::Status::kUnsupported
                     : Synthesize(current.unit, job, &pcm);

        if (status == models::Status::kUnsupported)
          {
            /* Too long for the bucket.  Never clip it: split it again with
             * half the budget, and give up only when it does not split.
             */
            const std::size_t smaller = current.budget / 2;
            std::vector<TtsUnit> parts;

            if (smaller >= kSmallestBudgetFrames)
              {
                parts = frontend_->Process(current.unit.text, smaller);
              }

            if (parts.size() < 2)
              {
                break;
              }

            for (auto part = parts.rbegin(); part != parts.rend(); ++part)
              {
                work.push_front(Work{ std::move(*part), smaller });
              }

            status = models::Status::kOk;
            continue;
          }

        if (status == models::Status::kOk)
          {
            status = Deliver(job, pcm, work.empty(), &sequence,
                             &total_samples);
          }
      }

    /* A stop that arrived inside the backend comes back as "cancelled";
     * say which kind it was.
     */
    if (status == models::Status::kCancelled)
      {
        const models::Status reason = StopReason(job);
        status = reason == models::Status::kOk ? status : reason;
      }
  }

  /* Give the slot and the session back before the terminal event; see
   * AsrService::Finish.
   */
  gate_->Release();
  active_id_.store(0);
  running_.store(false);

  std::uint8_t payload[NYAMP_TTS_FINISH_SIZE];
  std::size_t size = 0;
  Frame frame;

  if (nyamp_tts_finish_encode(payload, sizeof(payload), &size, Wire(status),
                              sequence, total_samples) == NYAMP_OK &&
      EncodeEvent(&frame, NYAMP_SERVICE_TTS, NYAMP_TTS_EVENT_FINISH,
                  job.request_id, generation_, payload, size))
    {
      queue_.Push(frame, false);
    }
}

bool TtsService::Poll(Frame *frame) { return queue_.Poll(frame); }

} // namespace nyamp
