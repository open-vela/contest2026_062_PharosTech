/****************************************************************************
 * tools/amp/nyampd/nyampd_loader.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_LOADER_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_LOADER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "nyamp_models.h"
#include "nyamp_protocol.h"
#include "nyampd_audio.h"

namespace nyamp
{

class ModelProvisioner;

/****************************************************************************
 * Name: ModelLoader
 *
 * Description:
 *   LOAD as the three speech services share it, the arrangement the LLM
 *   service introduced.  An absolute path -- or any path when no provisioner
 *   is attached -- loads on the caller's thread and the status is the
 *   answer.  A logical directory name ("asr", "tts", "kws") has to be pulled
 *   from the control domain first, and the transport loop must keep running
 *   for that pull to make progress, so the load moves to a worker and the
 *   response to the LOAD arrives later through the service's frame queue,
 *   preceded by BLOB PROGRESS events on the same request_id.
 *
 *   `load` receives the local path and does the service-specific part.  It
 *   runs on the worker for a deferred load, so it must take the service's
 *   own lock.
 *
 ****************************************************************************/

class ModelLoader
{
public:
  using Load = std::function<models::Status(const std::string &path)>;

  ModelLoader(std::uint32_t generation, FrameQueue *queue);
  ~ModelLoader();

  ModelLoader(const ModelLoader &) = delete;
  ModelLoader &operator=(const ModelLoader &) = delete;

  void SetProvisioner(ModelProvisioner *provisioner);

  models::Status Begin(const std::string &name, const nyamp_header_s &request,
                       bool *deferred, Load load);

  bool loading() const { return loading_.load(); }

  /* What the last LOAD was asked for and how it ended, for `info`. */

  models::Status last_status() const;
  std::string last_name() const;

private:
  void Run(std::string name, nyamp_header_s request, Load load);
  void Report(const std::string &name, models::Status status);

  std::uint32_t generation_;
  FrameQueue *queue_;
  ModelProvisioner *provisioner_ = nullptr;
  std::thread worker_;
  std::atomic<bool> loading_{ false };

  mutable std::mutex report_mutex_;
  models::Status last_status_ = models::Status::kNotReady;
  std::string last_name_;
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_LOADER_H */
