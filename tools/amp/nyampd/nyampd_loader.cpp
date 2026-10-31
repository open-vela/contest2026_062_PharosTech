/****************************************************************************
 * tools/amp/nyampd/nyampd_loader.cpp
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

#include "nyampd_loader.h"

#include "nyampd_provision.h"

#include <chrono>
#include <cstdio>

namespace nyamp
{

ModelLoader::ModelLoader(std::uint32_t generation, FrameQueue *queue)
    : generation_(generation), queue_(queue)
{
}

ModelLoader::~ModelLoader()
{
  if (worker_.joinable())
    {
      /* A pull in flight would otherwise keep the destructor waiting for as
       * long as the transfer takes.
       */
      if (provisioner_ != nullptr && loading_.load())
        {
          provisioner_->Cancel();
        }

      worker_.join();
    }
}

void ModelLoader::SetProvisioner(ModelProvisioner *provisioner)
{
  provisioner_ = provisioner;
}

models::Status ModelLoader::last_status() const
{
  std::lock_guard<std::mutex> lock(report_mutex_);
  return last_status_;
}

std::string ModelLoader::last_name() const
{
  std::lock_guard<std::mutex> lock(report_mutex_);
  return last_name_;
}

void ModelLoader::Report(const std::string &name, models::Status status)
{
  std::lock_guard<std::mutex> lock(report_mutex_);
  last_status_ = status;
  last_name_ = name;
}

models::Status ModelLoader::Begin(const std::string &name,
                                  const nyamp_header_s &request,
                                  bool *deferred, Load load)
{
  if (deferred == nullptr || !load)
    {
      return models::Status::kInvalid;
    }

  *deferred = false;

  bool expected = false;
  if (!loading_.compare_exchange_strong(expected, true))
    {
      return models::Status::kBusy;
    }

  if (provisioner_ == nullptr || !ModelProvisioner::IsLogicalName(name))
    {
      const models::Status status = load(name);
      Report(name, status);
      loading_.store(false);
      return status;
    }

  try
    {
      /* The previous worker cleared loading_ but may not be joined yet;
       * assigning over a joinable thread terminates the process.
       */
      if (worker_.joinable())
        {
          worker_.join();
        }

      worker_ = std::thread(&ModelLoader::Run, this, name, request,
                            std::move(load));
    }
  catch (...)
    {
      loading_.store(false);
      return models::Status::kBackendError;
    }

  *deferred = true;
  return models::Status::kOk;
}

void ModelLoader::Run(std::string name, nyamp_header_s request, Load load)
{
  using Clock = std::chrono::steady_clock;

  Clock::time_point reported = Clock::now();
  BlobStats stats;
  std::string path;

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
    Frame event;

    if (nyamp_blob_progress_encode(payload, sizeof(payload), &size, &wire) ==
            NYAMP_OK &&
        EncodeEvent(&event, NYAMP_SERVICE_BLOB, NYAMP_BLOB_EVENT_PROGRESS,
                    request.request_id, generation_, payload, size))
      {
        queue_->Push(event, true);
      }
  };

  const BlobResult pulled = provisioner_->Provide(name, &path, progress, &stats);
  std::fprintf(stderr, "nyampd: provision %s: %s files=%u reused=%u\n",
               name.c_str(), BlobResultName(pulled), stats.files,
               stats.reused);

  /* The wire status is the negated model status. */
  const models::Status status =
      pulled == BlobResult::kOk
          ? load(path)
          : static_cast<models::Status>(-BlobResultStatus(pulled));

  Report(name, status);
  loading_.store(false);

  Frame frame;
  if (EncodeStatusResponse(&frame, request, generation_,
                           -static_cast<std::int32_t>(status), nullptr, 0))
    {
      queue_->Push(frame, false);
    }
}

} // namespace nyamp
