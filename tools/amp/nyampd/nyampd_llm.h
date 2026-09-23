/****************************************************************************
 * tools/amp/nyampd/nyampd_llm.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_LLM_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_LLM_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nyamp_models.h"
#include "nyamp_protocol.h"
#include "nyampd_chat.h"
#include "nyampd_frame.h"

namespace nyamp
{

class ModelProvisioner;

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Generous headroom over the RPMsg carveout (64 buffers of 512 bytes).  When
 * the queue is full the service stops the generating request instead of
 * dropping tokens, which is the backpressure contract the model layer
 * documents.
 */

constexpr std::size_t kLlmEventQueueLimit = 64;

/****************************************************************************
 * Public Types
 ****************************************************************************/

using LlmFrame = Frame;

/****************************************************************************
 * Name: LlmService
 *
 * Description:
 *   Owns the single LLM session behind the RPMsg endpoint.  Wire framing stays
 *   in the caller: this class accepts decoded chunks and hands back encoded
 *   event frames, so the transport loop never shares its file descriptor with
 *   a worker thread.
 *
 *   Generate uses one accumulated token array per endpoint.  Chunks must
 *   arrive in order; anything else discards the partial request.  Only one
 *   request may be in flight, matching Session's serial contract.
 *
 *   Chat is the text-level request: the body is an OpenAI chat-completions
 *   JSON, this class renders and tokenizes it, runs the same token-id path a
 *   generate uses, and parses what the model wrote back into a response JSON.
 *   It needs the model's tokenizer.json, which is loaded with the model.
 *
 ****************************************************************************/

class LlmService
{
public:
  using BackendFactory = std::function<std::unique_ptr<models::Backend>()>;

  LlmService(std::uint32_t generation, models::Clock clock,
             BackendFactory factory);
  ~LlmService();

  LlmService(const LlmService &) = delete;
  LlmService &operator=(const LlmService &) = delete;

  /* Load is synchronous and may take seconds; it runs on the caller's thread
   * so a load failure is reported as the response to that request.
   */

  models::Status Load(const std::string &directory);
  models::Status Unload();

  /* Attach the provisioner that turns a logical model name into a local
   * path.  Without one every LOAD behaves as it always has.
   */

  void SetProvisioner(ModelProvisioner *provisioner);

  /* LOAD as the transport loop sees it.  An absolute path -- or any path when
   * no provisioner is attached -- loads synchronously and the returned status
   * is the answer.  A logical name ("llm/model.rkllm") may need hundreds of
   * megabytes pulled from the control domain first, and the loop has to keep
   * running for that pull to make progress at all: it is what delivers the
   * blob responses.  So the load moves to a worker, `*deferred` is set, and
   * the response to `request` arrives later through Poll.
   */

  models::Status BeginLoad(const std::string &name,
                           const nyamp_header_s &request, bool *deferred);

  /* Accumulate one generate chunk.  On success `*started` reports whether the
   * chunk completed the request and the worker was launched; a chunk that only
   * extended the array leaves it false.
   */

  models::Status BeginGenerate(const nyamp_llm_chunk_s &chunk,
                               const std::int32_t *ids, std::size_t count,
                               std::uint64_t request_id,
                               std::uint64_t deadline_ms, bool *started);

  /* Accumulate one chat chunk; the chunk that completes the body starts the
   * run.  Returns a WIRE status (nyamp_model_status_e), because chat has an
   * outcome -- the prompt does not fit -- that the model layer has no name
   * for.  NOT_READY means no model is loaded (the caller may load one and
   * retry); UNSUPPORTED means a model is loaded without a tokenizer, which is
   * what an absolute-path load of a bare .rkllm file gives.
   */

  std::int32_t BeginChat(const nyamp_llm_chat_s &chunk,
                         const std::uint8_t *bytes, std::uint64_t request_id,
                         std::uint64_t deadline_ms, bool *started);

  /* Replace the codec factory.  Tests use this to run the whole chat path
   * without tokenizer.json, which is never in the repository.
   */

  void SetChatCodecFactory(ChatCodecFactory factory);

  /* Whether this build can serve CHAT at all (a backend exists). */
  bool ChatSupported() const;

  /* "ready", "off", or the reason the tokenizer was refused. */
  std::string ChatState() const;

  /* Cooperative cancel.  The terminal finish event still reports the outcome;
   * the response to the cancel request only confirms acceptance.
   */

  models::Status Cancel(std::uint64_t request_id);

  /* Pop one encoded event frame.  Returns false when nothing is queued. */
  bool Poll(LlmFrame *frame);

  /* Outcome of the most recent Load, and the directory it was asked for.
   * Exposed so the control domain can diagnose a refused load through the
   * info query rather than needing a console on this side.
   */

  models::Status LastLoadStatus() const;
  std::string LastLoadDirectory() const;

private:
  struct PendingChunks
  {
    std::uint64_t request_id = 0;
    std::uint64_t deadline_ms = 0;
    std::uint32_t total = 0;
    std::uint32_t offset = 0;
    std::uint32_t max_new_tokens = 0;
    std::vector<std::int32_t> ids;
    bool active = false;
  };

  struct PendingChat
  {
    std::uint64_t request_id = 0;
    std::uint64_t deadline_ms = 0;
    std::uint32_t total = 0;
    std::uint32_t max_new_tokens = 0;
    std::uint32_t flags = 0;
    std::string body;
    bool active = false;
  };

  void Worker(std::uint64_t request_id, std::uint64_t deadline_ms);
  void ChatWorker(std::uint64_t request_id, std::uint64_t deadline_ms,
                  std::string body, std::uint32_t max_new_tokens,
                  std::uint32_t flags);
  void Loader(std::string name, nyamp_header_s request);
  models::Status LoadNow(const std::string &directory,
                         const std::string &tokenizer,
                         bool tokenizer_required);
  bool StartWorker(std::function<void()> work);
  std::uint64_t BeginRun(std::uint64_t request_id);
  void EndRun();
  bool PushChatFinish(std::uint64_t request_id,
                      const nyamp_llm_chat_finish_s &finish);
  bool PushFrame(const Frame &frame, bool droppable);
  void ResetPendingLocked();
  bool Push(const std::uint8_t *payload, std::size_t payload_size,
            std::uint16_t opcode, std::uint64_t request_id);
  bool PushToken(std::uint64_t request_id, std::uint32_t sequence,
                 const models::TokenChunk &token);
  bool PushFinish(std::uint64_t request_id, models::Status status);

  std::uint32_t generation_;
  models::Clock clock_;
  BackendFactory factory_;

  /* Load/Unload/Run are serialized on the session, so the session itself and
   * its calls share one mutex.  A worker holds it for the whole of a run, so
   * Cancel must NOT take it -- it would block the transport loop until the
   * very run it wants to stop has ended.  Cancel reaches the session through
   * session_view_ instead: the session is created once and lives as long as
   * this object, and Session::Cancel is safe from another thread.
   */
  std::mutex session_mutex_;
  std::unique_ptr<models::Session> session_;
  std::atomic<models::Session *> session_view_{ nullptr };

  /* The session refuses a request id that is not larger than the last one.
   * Wire ids cannot promise that: they embed the pid of whichever control
   * domain task sent the request.  Runs therefore get an id of this
   * service's own, and the wire id is only used to address events and
   * cancels.
   */
  std::uint64_t next_run_id_ = 0;
  std::atomic<std::uint64_t> active_run_id_{ 0 };
  std::atomic<std::uint64_t> active_wire_id_{ 0 };

  /* Set by Cancel, read by the running worker's sink.  It closes the window
   * between a request being admitted and the session accepting cancels for
   * it (a chat spends that window tokenizing), and it is what lets a worker
   * report "cancelled" when the stop reached it through its own sink.
   */
  std::atomic<bool> cancel_pending_{ false };

  /* Chat.  The codec is swapped under session_mutex_ together with the model
   * it belongs to; the flags let the transport thread answer without waiting
   * for that mutex.
   */
  ChatCodecFactory codec_factory_;
  std::unique_ptr<ChatCodec> codec_;
  std::atomic<bool> model_loaded_{ false };
  std::atomic<bool> chat_ready_{ false };
  PendingChat pending_chat_;

  std::thread worker_;
  std::atomic<bool> running_{ false };

  /* A deferred load.  It excludes generate, unload and a second load for as
   * long as it runs, exactly as a synchronous load did by blocking the loop.
   */
  ModelProvisioner *provisioner_ = nullptr;
  std::thread loader_;
  std::atomic<bool> loading_{ false };
  std::atomic<std::uint64_t> load_request_{ 0 };

  /* Move-only transfer of the assembled token array to the worker. */
  std::mutex ready_mutex_;
  std::shared_ptr<models::LlmInput> ready_;

  std::mutex queue_mutex_;
  std::deque<LlmFrame> queue_;

  std::mutex pending_mutex_;
  PendingChunks pending_;

  mutable std::mutex report_mutex_;
  models::Status last_load_ = models::Status::kInvalid;
  std::string last_directory_;
  std::string loaded_name_; /* What LOAD was asked for, not where it landed. */
  std::string chat_state_ = "off";
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_LLM_H */
