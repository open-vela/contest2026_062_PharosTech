/****************************************************************************
 * tools/amp/nyampd/nyampd_provision.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_PROVISION_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_PROVISION_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "nyamp_protocol.h"
#include "nyampd_blob.h"
#include "nyampd_frame.h"

namespace nyamp
{

/****************************************************************************
 * Name: ModelProvisioner
 *
 * Description:
 *   Turns a logical model name into a local path the backends can open.
 *
 *   The compute domain has no storage: in the product layout the eMMC and
 *   /data belong to the control domain.  A backend still needs a file in this
 *   name space (rkllm_init takes a path and mmaps it), so the model is pulled
 *   into tmpfs first.
 *
 *   A logical name is RELATIVE ("llm/model.rkllm", or a directory such as
 *   "asr").  An absolute path is never provisioned and is handed to the
 *   backend untouched, which keeps the images whose models sit on a locally
 *   mounted SD card working exactly as before.
 *
 ****************************************************************************/

class ModelProvisioner
{
public:
  explicit ModelProvisioner(BlobClient *client);

  static bool IsLogicalName(const std::string &name);

  /* Blocking; never call from the transport loop.  kBusy when another
   * transfer owns the client.
   */
  BlobResult Provide(const std::string &name, std::string *path,
                     const BlobClient::Progress &progress, BlobStats *stats);

  /* Abort the Provide in flight, from any thread. */
  void Cancel();

  BlobClient *client() const { return client_; }

private:
  BlobResult ProvideDirectory(const std::string &name, unsigned int depth,
                              const BlobClient::Progress &progress,
                              BlobStats *stats);

  BlobClient *client_;
};

/****************************************************************************
 * Name: BlobService
 *
 * Description:
 *   The two control-originated BLOB opcodes: BENCH_RUN and PULL.  Both take
 *   far longer than the transport loop may block, so they run on a worker
 *   and answer through a frame queue the loop drains, the same arrangement
 *   the LLM service uses for its events.
 *
 ****************************************************************************/

class BlobService
{
public:
  BlobService(std::uint32_t generation, ModelProvisioner *provisioner);
  ~BlobService();

  BlobService(const BlobService &) = delete;
  BlobService &operator=(const BlobService &) = delete;

  /* Both return a wire status.  NYAMP_MODEL_OK means the request was
   * accepted and its response will arrive through Poll; anything else is the
   * status of an immediate refusal.
   */
  std::int32_t BeginBench(const nyamp_header_s &request, std::uint32_t rounds,
                          std::uint32_t window_bytes);
  std::int32_t BeginPull(const nyamp_header_s &request,
                         const std::string &name);

  /* A CANCEL-kind message naming the request that started the worker. */
  void Cancel(std::uint64_t request_id);

  bool Poll(Frame *frame);

private:
  std::int32_t Start(const nyamp_header_s &request,
                     std::function<void()> work);
  void Push(const Frame &frame, bool droppable);
  void RunBench(nyamp_header_s request, std::uint32_t rounds,
                std::uint32_t window_bytes);
  void RunPull(nyamp_header_s request, std::string name);

  std::uint32_t generation_;
  ModelProvisioner *provisioner_;

  std::thread worker_;
  std::atomic<bool> running_{ false };
  std::atomic<std::uint64_t> active_request_{ 0 };

  std::mutex queue_mutex_;
  std::deque<Frame> queue_;
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_PROVISION_H */
