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

namespace nyamp
{

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

struct LlmFrame
{
  std::uint8_t data[NYAMP_RPMSG_MTU];
  std::size_t size;
};

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

  /* Accumulate one generate chunk.  On success `*started` reports whether the
   * chunk completed the request and the worker was launched; a chunk that only
   * extended the array leaves it false.
   */

  models::Status BeginGenerate(const nyamp_llm_chunk_s &chunk,
                               const std::int32_t *ids, std::size_t count,
                               std::uint64_t request_id,
                               std::uint64_t deadline_ms, bool *started);

  /* Cooperative cancel.  The terminal finish event still reports the outcome;
   * the response to the cancel request only confirms acceptance.
   */

  models::Status Cancel(std::uint64_t request_id);

  /* Pop one encoded event frame.  Returns false when nothing is queued. */
  bool Poll(LlmFrame *frame);

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

  void Worker(std::uint64_t request_id, std::uint64_t deadline_ms);
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
   * its calls share one mutex.  Cancel takes the same lock from the transport
   * thread; Session::Cancel is documented safe from another thread.
   */
  std::mutex session_mutex_;
  std::unique_ptr<models::Session> session_;

  std::thread worker_;
  std::atomic<bool> running_{ false };

  /* Move-only transfer of the assembled token array to the worker. */
  std::mutex ready_mutex_;
  std::shared_ptr<models::LlmInput> ready_;

  std::mutex queue_mutex_;
  std::deque<LlmFrame> queue_;

  std::mutex pending_mutex_;
  PendingChunks pending_;
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_LLM_H */
