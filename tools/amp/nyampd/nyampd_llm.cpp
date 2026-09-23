/****************************************************************************
 * tools/amp/nyampd/nyampd_llm.cpp
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

#include "nyampd_llm.h"

#include "nyampd_provision.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include <sys/stat.h>

namespace nyamp
{
namespace
{

constexpr char kTokenizerFile[] = "tokenizer.json";

bool IsRegularFile(const std::string &path)
{
  struct stat information;
  return stat(path.c_str(), &information) == 0 &&
         S_ISREG(information.st_mode);
}

/* The tokenizer sits beside the weights: next to a model file, or inside a
 * model directory.
 */

std::string SiblingTokenizer(const std::string &model)
{
  if (IsRegularFile(model))
    {
      const std::size_t slash = model.rfind('/');
      return slash == std::string::npos
                 ? std::string(kTokenizerFile)
                 : model.substr(0, slash + 1) + kTokenizerFile;
    }

  return model + "/" + kTokenizerFile;
}

bool IsChatStopId(std::int32_t id)
{
  return std::find(std::begin(kChatStopIds), std::end(kChatStopIds), id) !=
         std::end(kChatStopIds);
}

std::uint32_t MillisecondsBetween(std::chrono::steady_clock::time_point from,
                                  std::chrono::steady_clock::time_point to)
{
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(to - from)
          .count());
}

} // namespace

LlmService::LlmService(std::uint32_t generation, models::Clock clock,
                       BackendFactory factory)
    : generation_(generation), clock_(std::move(clock)),
      factory_(std::move(factory)), codec_factory_(&CreateTokenizerCodec)
{
}

LlmService::~LlmService()
{
  /* A worker owns the session until Run returns; never unload underneath it.
   */
  if (worker_.joinable())
    {
      worker_.join();
    }

  /* A pull in flight would otherwise keep this destructor waiting for as
   * long as the transfer takes.
   */
  if (loader_.joinable())
    {
      if (provisioner_ != nullptr)
        {
          provisioner_->Cancel();
        }

      loader_.join();
    }
}

void LlmService::SetProvisioner(ModelProvisioner *provisioner)
{
  provisioner_ = provisioner;
}

void LlmService::SetChatCodecFactory(ChatCodecFactory factory)
{
  codec_factory_ = std::move(factory);
}

bool LlmService::ChatSupported() const
{
  return static_cast<bool>(factory_) && static_cast<bool>(codec_factory_);
}

std::string LlmService::ChatState() const
{
  std::lock_guard<std::mutex> lock(report_mutex_);
  return chat_state_;
}

models::Status LlmService::Load(const std::string &directory)
{
  if (loading_.load())
    {
      return models::Status::kBusy;
    }

  /* A path the compute domain reaches by itself: the tokenizer is used when
   * it happens to be there, and its absence only leaves CHAT unsupported,
   * which is what this kind of load has always meant.
   */
  return LoadNow(directory, SiblingTokenizer(directory), false);
}

models::Status LlmService::BeginLoad(const std::string &name,
                                     const nyamp_header_s &request,
                                     bool *deferred)
{
  if (deferred == nullptr)
    {
      return models::Status::kInvalid;
    }

  *deferred = false;

  /* Only a relative, well-formed name is a logical model name.  Everything
   * else -- notably the absolute paths used while the models lived on a
   * locally mounted card -- takes the original synchronous path unchanged.
   */
  if (provisioner_ == nullptr || !factory_ ||
      !ModelProvisioner::IsLogicalName(name))
    {
      return Load(name);
    }

  if (running_.load())
    {
      return models::Status::kBusy;
    }

  /* Decide before pulling anything: loading what is already loaded is a
   * success, not a reason to move 875 MB and then be told the session is
   * busy, and another model needs an UNLOAD first.
   */
  if (model_loaded_.load())
    {
      std::lock_guard<std::mutex> report_lock(report_mutex_);
      return loaded_name_ == name ? models::Status::kOk
                                  : models::Status::kBusy;
    }

  bool expected = false;
  if (!loading_.compare_exchange_strong(expected, true))
    {
      return models::Status::kBusy;
    }

  try
    {
      if (loader_.joinable())
        {
          loader_.join();
        }

      load_request_.store(request.request_id);
      loader_ = std::thread(&LlmService::Loader, this, name, request);
    }
  catch (...)
    {
      load_request_.store(0);
      loading_.store(false);
      return models::Status::kBackendError;
    }

  *deferred = true;
  return models::Status::kOk;
}

void LlmService::Loader(std::string name, nyamp_header_s request)
{
  using Clock = std::chrono::steady_clock;

  Clock::time_point reported = Clock::now();
  BlobStats stats;
  std::string path;

  /* Progress rides on the LOAD's request_id as BLOB events, so a client that
   * cares can show it and one that does not simply skips non-response frames,
   * which every existing client already does.
   */
  auto progress = [&](const BlobProgress &update) {
    const Clock::time_point now = Clock::now();
    if (update.done != update.total &&
        now - reported < std::chrono::milliseconds(500))
      {
        return;
      }

    reported = now;

    nyamp_blob_progress_s wire{ update.done, update.total,
                                update.bytes_per_second };
    std::uint8_t payload[NYAMP_BLOB_PROGRESS_SIZE];
    std::size_t size = 0;
    nyamp_header_s header{};
    Frame event;

    header.service = NYAMP_SERVICE_BLOB;
    header.opcode = NYAMP_BLOB_EVENT_PROGRESS;
    header.flags = NYAMP_FLAG_EVENT;
    header.request_id = request.request_id;
    header.generation = generation_;
    if (nyamp_blob_progress_encode(payload, sizeof(payload), &size, &wire) ==
            NYAMP_OK &&
        EncodeFrame(&event, header, payload, size))
      {
        PushFrame(event, true);
      }
  };

  /* The tokenizer goes first.  It is 10 MB against the model's 875, so a
   * deployment that forgot it fails in a second instead of after a minute.
   * NOT_FOUND is not final yet: `name` may be a model directory, and then the
   * tokenizer arrives inside it.
   */
  const std::size_t slash = name.rfind('/');
  const std::string tokenizer_name =
      (slash == std::string::npos ? std::string() : name.substr(0, slash + 1)) +
      kTokenizerFile;
  const bool want_chat = static_cast<bool>(codec_factory_);
  std::string tokenizer;
  BlobResult pulled =
      want_chat
          ? provisioner_->Provide(tokenizer_name, &tokenizer, progress, &stats)
          : BlobResult::kOk;
  bool tokenizer_missing = false;
  if (pulled == BlobResult::kNotFound || pulled == BlobResult::kDirectory)
    {
      tokenizer.clear();
      pulled = BlobResult::kOk;
      tokenizer_missing = !provisioner_->IsDirectory(name);
    }

  if (pulled == BlobResult::kOk && !tokenizer_missing)
    {
      pulled = provisioner_->Provide(name, &path, progress, &stats);
    }

  models::Status status;

  std::fprintf(stderr, "nyampd: provision %s: %s files=%u reused=%u\n",
               name.c_str(), BlobResultName(pulled), stats.files,
               stats.reused);

  if (tokenizer_missing)
    {
      /* A model file without its tokenizer can only ever serve GENERATE, and
       * a caller that names a model by logical name is the chat path.  Say so
       * now, before the weights move.
       */
      std::fprintf(stderr, "nyampd: %s is not on the control domain\n",
                   tokenizer_name.c_str());
      status = models::Status::kNotReady;

      std::lock_guard<std::mutex> report_lock(report_mutex_);
      last_load_ = status;
      last_directory_ = name;
      chat_state_ = "tokenizer.json missing";
    }
  else if (pulled == BlobResult::kOk)
    {
      if (tokenizer.empty())
        {
          tokenizer = SiblingTokenizer(path);
        }

      status = LoadNow(path, tokenizer, want_chat);
      if (status == models::Status::kOk)
        {
          std::lock_guard<std::mutex> report_lock(report_mutex_);
          loaded_name_ = name;
        }
    }
  else
    {
      /* The wire status is the negated model status, see ModelStatus(). */
      status = static_cast<models::Status>(-BlobResultStatus(pulled));

      std::lock_guard<std::mutex> report_lock(report_mutex_);
      last_load_ = status;
      last_directory_ = name;
    }

  load_request_.store(0);
  loading_.store(false, std::memory_order_release);

  Frame frame;
  if (EncodeStatusResponse(&frame, request, generation_,
                           -static_cast<std::int32_t>(status), nullptr, 0))
    {
      PushFrame(frame, false);
    }
}

models::Status LlmService::LoadNow(const std::string &directory,
                                   const std::string &tokenizer,
                                   bool tokenizer_required)
{
  if (directory.empty())
    {
      return models::Status::kInvalid;
    }

  if (!factory_)
    {
      return models::Status::kUnsupported;
    }

  if (running_.load())
    {
      return models::Status::kBusy;
    }

  std::lock_guard<std::mutex> lock(session_mutex_);
  if (!session_)
    {
      session_ =
          std::make_unique<models::Session>(factory_(), generation_, clock_);
      session_view_.store(session_.get());
    }

  /* Build the codec before touching the runtime: parsing the vocabulary
   * takes a fraction of a second, rkllm_init takes many, and a logical-name
   * load without a usable tokenizer is a failed load either way.
   */
  std::unique_ptr<ChatCodec> codec;
  std::string chat_state = "off";
  models::Status status = models::Status::kOk;

  if (codec_factory_ && IsRegularFile(tokenizer))
    {
      std::string reason;

      codec = codec_factory_(tokenizer, &reason);
      chat_state = codec ? "ready" : "tokenizer refused: " + reason;
      if (!codec)
        {
          std::fprintf(stderr, "nyampd: %s: %s\n", tokenizer.c_str(),
                       reason.c_str());
          status = models::Status::kBackendError;
        }
    }
  else if (tokenizer_required)
    {
      chat_state = "tokenizer.json missing";
      std::fprintf(stderr, "nyampd: no tokenizer at %s\n", tokenizer.c_str());
      status = models::Status::kNotReady;
    }

  if (status != models::Status::kOk && !tokenizer_required)
    {
      /* A legacy load keeps working without chat. */
      status = models::Status::kOk;
    }

  if (status == models::Status::kOk)
    {
      status = session_->Load(directory);
    }

  if (status == models::Status::kOk)
    {
      codec_ = std::move(codec);
      chat_ready_.store(codec_ != nullptr);
      model_loaded_.store(true);
    }

  /* Record the attempt so the info query can report why a load was refused. */
  {
    std::lock_guard<std::mutex> report_lock(report_mutex_);
    last_load_ = status;
    last_directory_ = directory;
    chat_state_ = chat_state.substr(0, 60);
    if (status == models::Status::kOk)
      {
        loaded_name_ = directory;
      }
  }

  return status;
}

models::Status LlmService::LastLoadStatus() const
{
  std::lock_guard<std::mutex> lock(report_mutex_);
  return last_load_;
}

std::string LlmService::LastLoadDirectory() const
{
  std::lock_guard<std::mutex> lock(report_mutex_);
  return last_directory_;
}

models::Status LlmService::Unload()
{
  if (running_.load() || loading_.load())
    {
      return models::Status::kBusy;
    }

  std::lock_guard<std::mutex> lock(session_mutex_);
  if (!session_)
    {
      return models::Status::kInvalid;
    }

  const models::Status status = session_->Unload();
  if (status == models::Status::kOk)
    {
      /* The tokenizer belongs to the model that just went away. */
      codec_.reset();
      chat_ready_.store(false);
      model_loaded_.store(false);

      std::lock_guard<std::mutex> report_lock(report_mutex_);
      loaded_name_.clear();
      chat_state_ = "off";
    }

  return status;
}

/****************************************************************************
 * Name: StartWorker / BeginRun / EndRun
 *
 * Description:
 *   Admission for the one run that may be in flight.  StartWorker is called
 *   on the transport thread with the wire id already published, so a cancel
 *   that arrives before the worker has reached the session is not lost.
 *
 ****************************************************************************/

bool LlmService::StartWorker(std::function<void()> work)
{
  running_.store(true);
  try
    {
      /* The previous worker has cleared running_ but may not be joined yet;
       * assigning over a joinable thread terminates the process.
       */
      if (worker_.joinable())
        {
          worker_.join();
        }

      worker_ = std::thread(std::move(work));
    }
  catch (...)
    {
      active_wire_id_.store(0);
      running_.store(false);
      return false;
    }

  return true;
}

std::uint64_t LlmService::BeginRun(std::uint64_t request_id)
{
  (void)request_id;
  const std::uint64_t run_id = ++next_run_id_;
  active_run_id_.store(run_id);
  return run_id;
}

void LlmService::EndRun()
{
  active_run_id_.store(0);
  active_wire_id_.store(0);
}

void LlmService::ResetPendingLocked()
{
  pending_.request_id = 0;
  pending_.deadline_ms = 0;
  pending_.total = 0;
  pending_.offset = 0;
  pending_.max_new_tokens = 0;
  pending_.ids.clear();
  pending_.active = false;
}

models::Status LlmService::BeginGenerate(
    const nyamp_llm_chunk_s &chunk, const std::int32_t *ids, std::size_t count,
    std::uint64_t request_id, std::uint64_t deadline_ms, bool *started)
{
  if (started == nullptr)
    {
      return models::Status::kInvalid;
    }

  *started = false;

  if (ids == nullptr || count == 0 || request_id == 0)
    {
      return models::Status::kInvalid;
    }

  if (chunk.total == 0 || chunk.total > models::kContextTokens)
    {
      return models::Status::kInvalid;
    }

  if (running_.load() || loading_.load())
    {
      return models::Status::kBusy;
    }

  std::lock_guard<std::mutex> lock(pending_mutex_);

  /* One endpoint, one request in flight: a generate abandons a chat body
   * that was still arriving.
   */
  pending_chat_ = PendingChat();

  if (!pending_.active)
    {
      if (chunk.offset != 0)
        {
          /* A continuation without its head can never be completed. */
          return models::Status::kInvalid;
        }

      pending_.active = true;
      pending_.request_id = request_id;
      pending_.deadline_ms = deadline_ms;
      pending_.total = chunk.total;
      pending_.offset = 0;
      pending_.max_new_tokens = chunk.max_new_tokens;
      pending_.ids.clear();
      pending_.ids.reserve(chunk.total);
    }
  else if (chunk.total != pending_.total || chunk.offset != pending_.offset)
    {
      /* Out-of-order or mismatched continuation: drop the partial request. */
      ResetPendingLocked();
      return models::Status::kInvalid;
    }

  if (chunk.count > pending_.total - pending_.offset)
    {
      ResetPendingLocked();
      return models::Status::kInvalid;
    }

  pending_.ids.insert(pending_.ids.end(), ids, ids + count);
  pending_.offset += chunk.count;

  if (pending_.offset < pending_.total)
    {
      return models::Status::kOk;
    }

  /* The array is complete.  Move it out and start the worker.  Session::Run
   * rejects a second request while one is active, so the busy check above plus
   * this handoff is the only admission control needed.
   */

  const std::uint64_t request = pending_.request_id;
  const std::uint64_t deadline = pending_.deadline_ms;
  auto input = std::make_shared<models::LlmInput>();
  input->token_ids = std::move(pending_.ids);
  input->max_new_tokens = pending_.max_new_tokens;
  ResetPendingLocked();

  {
    std::lock_guard<std::mutex> ready_lock(ready_mutex_);
    ready_ = std::move(input);
  }

  cancel_pending_.store(false);
  active_wire_id_.store(request);
  if (!StartWorker([this, request, deadline] { Worker(request, deadline); }))
    {
      std::lock_guard<std::mutex> ready_lock(ready_mutex_);
      ready_.reset();
      return models::Status::kBackendError;
    }

  *started = true;
  return models::Status::kOk;
}

void LlmService::Worker(std::uint64_t request_id, std::uint64_t deadline_ms)
{
  std::shared_ptr<models::LlmInput> input;
  {
    std::lock_guard<std::mutex> lock(ready_mutex_);
    input = std::move(ready_);
  }

  models::Status result = models::Status::kBackendError;

  if (input)
    {
      const std::uint64_t sequence_base = request_id;
      std::uint32_t sequence = 0;

      /* One token event per emitted output.  Returning false from the sink
       * stops the request; the model layer then reports kConsumerStopped, so a
       * full queue surfaces as an explicit failure instead of silent loss.
       */

      auto sink = [&](const models::Event &event) -> bool {
        if (event.terminal)
          {
            return true;
          }

        const auto *token =
            std::get_if<models::TokenChunk>(event.output.get());
        if (token == nullptr)
          {
            return false;
          }

        (void)sequence_base;
        return !cancel_pending_.load() &&
               PushToken(request_id, sequence++, *token);
      };

      std::lock_guard<std::mutex> lock(session_mutex_);
      if (session_)
        {
          result = session_->Run(
              { BeginRun(request_id), generation_, deadline_ms }, *input,
              sink);
          if (cancel_pending_.load() &&
              result == models::Status::kConsumerStopped)
            {
              result = models::Status::kCancelled;
            }
        }
    }

  EndRun();

  /* Release admission before publishing the terminal event.  An observer that
   * has just seen the finish of this request must be able to start the next
   * one immediately; doing it the other way round leaves a window where the
   * terminal is visible but every new request is still refused as busy.
   */

  running_.store(false, std::memory_order_release);
  PushFinish(request_id, result);
}

models::Status LlmService::Cancel(std::uint64_t request_id)
{
  if (request_id == 0)
    {
      return models::Status::kInvalid;
    }

  /* A load that is still pulling its model is cancelled at the provisioner;
   * the session is not involved yet.  The LOAD's own response reports the
   * outcome, as a generate's finish event does for a generate.
   */
  if (loading_.load() && load_request_.load() == request_id &&
      provisioner_ != nullptr)
    {
      provisioner_->Cancel();
      return models::Status::kOk;
    }

  /* No session_mutex_ here: the worker holds it for the whole run, and this
   * is the transport thread.
   */
  models::Session *session = session_view_.load();
  if (session == nullptr || active_wire_id_.load() != request_id)
    {
      return models::Status::kInvalid;
    }

  cancel_pending_.store(true);

  /* The session only accepts a cancel once its run has begun; before that
   * the flag alone does the job, so its refusal is not an error.
   */
  const std::uint64_t run_id = active_run_id_.load();
  if (run_id != 0)
    {
      session->Cancel(run_id, generation_);
    }

  return models::Status::kOk;
}

/****************************************************************************
 * Name: BeginChat
 *
 * Description:
 *   Reassemble a chat body.  Every refusal drops the partial body: a client
 *   that lost its place has to start over, because resuming a JSON document
 *   from a guessed offset would produce a different request, not an error.
 *
 ****************************************************************************/

std::int32_t LlmService::BeginChat(const nyamp_llm_chat_s &chunk,
                                   const std::uint8_t *bytes,
                                   std::uint64_t request_id,
                                   std::uint64_t deadline_ms, bool *started)
{
  if (started == nullptr)
    {
      return NYAMP_MODEL_INVALID;
    }

  *started = false;

  if (bytes == nullptr || request_id == 0 || chunk.length == 0 ||
      chunk.total == 0 || chunk.total > NYAMP_LLM_CHAT_MAX_BODY)
    {
      return NYAMP_MODEL_INVALID;
    }

  if (!ChatSupported())
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  if (running_.load() || loading_.load())
    {
      return NYAMP_MODEL_BUSY;
    }

  /* NOT_READY invites the caller to load a model and try again; UNSUPPORTED
   * tells it that the model that IS loaded came without a tokenizer.
   */
  if (!model_loaded_.load())
    {
      return NYAMP_MODEL_NOT_READY;
    }

  if (!chat_ready_.load())
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  std::lock_guard<std::mutex> lock(pending_mutex_);
  ResetPendingLocked();

  if (!pending_chat_.active)
    {
      if (chunk.offset != 0)
        {
          return NYAMP_MODEL_INVALID;
        }

      pending_chat_.active = true;
      pending_chat_.request_id = request_id;
      pending_chat_.deadline_ms = deadline_ms;
      pending_chat_.total = chunk.total;
      pending_chat_.max_new_tokens = chunk.max_new_tokens;
      pending_chat_.flags = chunk.flags;
      pending_chat_.body.clear();
      pending_chat_.body.reserve(chunk.total);
    }
  else if (request_id != pending_chat_.request_id ||
           chunk.total != pending_chat_.total ||
           chunk.offset != pending_chat_.body.size() ||
           chunk.max_new_tokens != pending_chat_.max_new_tokens ||
           chunk.flags != pending_chat_.flags)
    {
      pending_chat_ = PendingChat();
      return NYAMP_MODEL_INVALID;
    }

  if (chunk.length > pending_chat_.total - pending_chat_.body.size())
    {
      pending_chat_ = PendingChat();
      return NYAMP_MODEL_INVALID;
    }

  pending_chat_.body.append(reinterpret_cast<const char *>(bytes),
                            chunk.length);
  if (pending_chat_.body.size() < pending_chat_.total)
    {
      return NYAMP_MODEL_OK;
    }

  PendingChat ready = std::move(pending_chat_);
  pending_chat_ = PendingChat();

  const std::uint32_t max_new_tokens =
      ready.max_new_tokens == 0 ? kChatDefaultNewTokens : ready.max_new_tokens;

  cancel_pending_.store(false);
  active_wire_id_.store(ready.request_id);
  if (!StartWorker([this, id = ready.request_id, deadline = ready.deadline_ms,
                    body = std::move(ready.body), max_new_tokens,
                    flags = ready.flags]() mutable {
        ChatWorker(id, deadline, std::move(body), max_new_tokens, flags);
      }))
    {
      return NYAMP_MODEL_BACKEND_ERROR;
    }

  *started = true;
  return NYAMP_MODEL_OK;
}

/****************************************************************************
 * Name: ChatWorker
 *
 * Description:
 *   One chat completion: request JSON -> prompt ids -> the ordinary token-id
 *   run -> output ids -> text with the structural tokens kept -> response
 *   JSON.  The ids are collected rather than the backend's text, because the
 *   backend decodes token by token and cannot put a character back together
 *   that the vocabulary split across two tokens.
 *
 ****************************************************************************/

void LlmService::ChatWorker(std::uint64_t request_id,
                            std::uint64_t deadline_ms, std::string body,
                            std::uint32_t max_new_tokens, std::uint32_t flags)
{
  using Clock = std::chrono::steady_clock;

  const Clock::time_point accepted = Clock::now();
  const bool stream = (flags & NYAMP_LLM_CHAT_STREAM_TOKENS) != 0;
  nyamp_llm_chat_finish_s finish{};
  std::string response;

  finish.status = NYAMP_MODEL_BACKEND_ERROR;
  finish.context_limit = static_cast<std::uint32_t>(models::kContextTokens);

  {
    std::lock_guard<std::mutex> lock(session_mutex_);
    std::vector<std::int32_t> prompt;
    std::string error;

    if (!session_ || !codec_)
      {
        finish.status = NYAMP_MODEL_NOT_READY;
      }
    else if (!codec_->Encode(body,
                             (flags & NYAMP_LLM_CHAT_GUARD_UNTRUSTED) != 0,
                             &prompt, &error) ||
             prompt.empty())
      {
        std::fprintf(stderr, "nyampd: chat request refused: %s\n",
                     error.c_str());
        finish.status = NYAMP_MODEL_INVALID;
      }
    else if (prompt.size() > models::kContextTokens ||
             max_new_tokens > models::kContextTokens - prompt.size())
      {
        /* The one failure the caller is expected to fix by itself: it gets
         * the two numbers it needs to decide how much history to drop.
         */
        finish.prompt_tokens = static_cast<std::uint32_t>(prompt.size());
        finish.status = NYAMP_MODEL_PROMPT_TOO_LONG;
      }
    else if (cancel_pending_.load())
      {
        finish.prompt_tokens = static_cast<std::uint32_t>(prompt.size());
        finish.status = NYAMP_MODEL_CANCELLED;
      }
    else
      {
        std::vector<std::int32_t> generated;
        Clock::time_point first_token = accepted;
        Clock::time_point last_token = accepted;
        std::uint32_t sequence = 0;
        std::int32_t last_id = 0;
        bool saw_stop = false;
        bool hit_limit = false;
        bool queue_full = false;

        finish.prompt_tokens = static_cast<std::uint32_t>(prompt.size());
        generated.reserve(max_new_tokens);
        if (stream)
          {
            codec_->StreamReset();
          }

        auto sink = [&](const models::Event &event) -> bool {
          if (event.terminal)
            {
              return true;
            }

          const auto *token =
              std::get_if<models::TokenChunk>(event.output.get());
          if (token == nullptr || cancel_pending_.load())
            {
              return false;
            }

          last_token = Clock::now();
          if (generated.empty() && !saw_stop)
            {
              first_token = last_token;
            }

          /* Whether the runtime reports its stop token or not is its own
           * business; the turn ends here either way and the stop token is
           * never part of the answer.
           */
          if (IsChatStopId(token->token_id))
            {
              saw_stop = true;
              return false;
            }

          generated.push_back(token->token_id);
          last_id = token->token_id;
          if (stream)
            {
              const std::string text = codec_->StreamPush(token->token_id);
              if (!text.empty() &&
                  !PushToken(request_id, sequence++,
                             models::TokenChunk{ token->token_id, text }))
                {
                  queue_full = true;
                  return false;
                }
            }

          if (generated.size() >= max_new_tokens)
            {
              hit_limit = true;
              return false;
            }

          return true;
        };

        models::LlmInput input;
        input.token_ids = std::move(prompt);
        input.max_new_tokens = max_new_tokens;

        const models::Status result = session_->Run(
            { BeginRun(request_id), generation_, deadline_ms }, input, sink);

        finish.completion_tokens =
            static_cast<std::uint32_t>(generated.size());
        finish.prefill_ms = MillisecondsBetween(accepted, first_token);
        finish.decode_ms = MillisecondsBetween(first_token, last_token);

        /* The sink ending the run on purpose reaches here as "consumer
         * stopped"; only a stop the sink did not ask for is a failure.
         */
        const bool ended_by_sink =
            result == models::Status::kConsumerStopped &&
            (saw_stop || hit_limit) && !queue_full;

        if (cancel_pending_.load() || result == models::Status::kCancelled)
          {
            finish.status = NYAMP_MODEL_CANCELLED;
          }
        else if (result == models::Status::kOk || ended_by_sink)
          {
            if (stream)
              {
                const std::string tail = codec_->StreamFlush();
                if (!tail.empty())
                  {
                    PushToken(request_id, sequence++,
                              models::TokenChunk{ last_id, tail });
                  }
              }

            ChatUsage usage;
            usage.prompt_tokens = finish.prompt_tokens;
            usage.completion_tokens = finish.completion_tokens;

            /* "length" only when the budget ran out before the model ended
             * its turn; a run the runtime cut short counts the same way.
             */
            const bool truncated =
                !saw_stop && generated.size() >= max_new_tokens;
            response = BuildChatResponse(codec_->Decode(generated), body,
                                         truncated, request_id, usage);
            finish.status = response.size() <= NYAMP_LLM_RESULT_MAX_BODY
                                ? NYAMP_MODEL_OK
                                : NYAMP_MODEL_BACKEND_ERROR;
          }
        else
          {
            finish.status = -static_cast<std::int32_t>(result);
          }
      }
  }

  EndRun();

  if (finish.status == NYAMP_MODEL_OK)
    {
      /* The result is the answer itself, so it is queued past the limit the
       * advisory token events obey.
       */
      nyamp_llm_result_s chunk{};
      chunk.total = static_cast<std::uint32_t>(response.size());
      while (chunk.offset < chunk.total)
        {
          std::uint8_t payload[NYAMP_INLINE_MAX];
          std::size_t size = 0;
          nyamp_header_s header{};
          Frame frame;

          chunk.length = std::min<std::uint32_t>(chunk.total - chunk.offset,
                                                 NYAMP_LLM_RESULT_MAX_CHUNK);
          header.service = NYAMP_SERVICE_LLM;
          header.opcode = NYAMP_LLM_EVENT_RESULT;
          header.flags = NYAMP_FLAG_EVENT;
          header.request_id = request_id;
          header.generation = generation_;
          if (nyamp_llm_result_encode(
                  payload, sizeof(payload), &size, &chunk,
                  reinterpret_cast<const std::uint8_t *>(response.data()) +
                      chunk.offset) != NYAMP_OK ||
              !EncodeFrame(&frame, header, payload, size))
            {
              finish.status = NYAMP_MODEL_BACKEND_ERROR;
              break;
            }

          PushFrame(frame, false);
          chunk.offset += chunk.length;
        }
    }

  /* Release admission before publishing the terminal event, for the reason
   * the generate worker gives.
   */
  running_.store(false, std::memory_order_release);
  PushChatFinish(request_id, finish);
}

bool LlmService::PushChatFinish(std::uint64_t request_id,
                                const nyamp_llm_chat_finish_s &finish)
{
  std::uint8_t payload[NYAMP_LLM_CHAT_FINISH_SIZE];
  std::size_t size = 0;
  nyamp_header_s header{};
  Frame frame;

  header.service = NYAMP_SERVICE_LLM;
  header.opcode = NYAMP_LLM_EVENT_FINISH;
  header.flags = NYAMP_FLAG_EVENT;
  header.request_id = request_id;
  header.generation = generation_;
  if (nyamp_llm_chat_finish_encode(payload, sizeof(payload), &size, &finish) !=
          NYAMP_OK ||
      !EncodeFrame(&frame, header, payload, size))
    {
      return false;
    }

  /* The terminal event is the only way the caller learns the run is over. */
  return PushFrame(frame, false);
}

bool LlmService::Push(const std::uint8_t *payload, std::size_t payload_size,
                      std::uint16_t opcode, std::uint64_t request_id)
{
  if (payload_size > NYAMP_INLINE_MAX)
    {
      return false;
    }

  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (queue_.size() >= kLlmEventQueueLimit)
    {
      return false;
    }

  LlmFrame frame{};
  nyamp_header_s header{};

  header.service = NYAMP_SERVICE_LLM;
  header.opcode = opcode;
  header.flags = NYAMP_FLAG_EVENT;
  header.request_id = request_id;
  header.deadline_ms = 0;
  header.generation = generation_;
  header.payload_size = static_cast<std::uint32_t>(payload_size);

  if (nyamp_header_encode(frame.data, sizeof(frame.data), &header) != NYAMP_OK)
    {
      return false;
    }

  if (payload_size != 0)
    {
      std::memcpy(frame.data + NYAMP_WIRE_HEADER_SIZE, payload, payload_size);
    }

  frame.size = NYAMP_WIRE_HEADER_SIZE + payload_size;
  queue_.push_back(frame);
  return true;
}

bool LlmService::PushFrame(const Frame &frame, bool droppable)
{
  std::lock_guard<std::mutex> lock(queue_mutex_);

  /* A response is the only answer its requester will get, so it is queued
   * even when the limit is reached; progress is advisory and is shed.
   */
  if (droppable && queue_.size() >= kLlmEventQueueLimit)
    {
      return false;
    }

  queue_.push_back(frame);
  return true;
}

bool LlmService::PushToken(std::uint64_t request_id, std::uint32_t sequence,
                           const models::TokenChunk &token)
{
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t payload_size = 0;
  const std::size_t length = token.text.size();

  if (nyamp_llm_token_encode(payload, sizeof(payload), &payload_size,
                             static_cast<std::uint32_t>(token.token_id),
                             sequence, token.text.c_str(), length) != NYAMP_OK)
    {
      return false;
    }

  return Push(payload, payload_size, NYAMP_LLM_EVENT_TOKEN, request_id);
}

bool LlmService::PushFinish(std::uint64_t request_id, models::Status status)
{
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t payload_size = 0;

  if (nyamp_llm_finish_encode(payload, sizeof(payload), &payload_size,
                              static_cast<std::int32_t>(status),
                              0) != NYAMP_OK)
    {
      return false;
    }

  return Push(payload, payload_size, NYAMP_LLM_EVENT_FINISH, request_id);
}

bool LlmService::Poll(LlmFrame *frame)
{
  if (frame == nullptr)
    {
      return false;
    }

  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (queue_.empty())
    {
      return false;
    }

  *frame = queue_.front();
  queue_.pop_front();
  return true;
}

} // namespace nyamp
