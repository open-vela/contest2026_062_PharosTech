/****************************************************************************
 * tools/amp/nyampd/nyampd_provision.cpp
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

#include "nyampd_provision.h"

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace nyamp
{
namespace
{

/* /data/models/<kind>/<file> is two levels; a little slack costs nothing and
 * a bound keeps a directory loop on the other side from recursing forever.
 */

constexpr unsigned int kMaxDirectoryDepth = 3;

/* Same headroom the LLM service keeps over the RPMsg carveout. */
constexpr std::size_t kQueueLimit = 64;

constexpr int kProgressIntervalMs = 500;

} // namespace

ModelProvisioner::ModelProvisioner(BlobClient *client) : client_(client) {}

bool ModelProvisioner::IsLogicalName(const std::string &name)
{
  return nyamp_blob_name_check(name.data(), name.size()) == NYAMP_OK;
}

bool ModelProvisioner::IsDirectory(const std::string &name)
{
  std::vector<BlobEntry> entries;

  if (client_ == nullptr || !IsLogicalName(name) || !client_->Acquire())
    {
      return false;
    }

  const BlobResult result = client_->List(name, &entries);
  client_->Release();
  return result == BlobResult::kOk;
}

void ModelProvisioner::Cancel()
{
  if (client_ != nullptr)
    {
      client_->Cancel();
    }
}

BlobResult ModelProvisioner::ProvideDirectory(
    const std::string &name, unsigned int depth,
    const BlobClient::Progress &progress, BlobStats *stats)
{
  std::vector<BlobEntry> entries;

  if (depth > kMaxDirectoryDepth)
    {
      return BlobResult::kProtocol;
    }

  BlobResult result = client_->List(name, &entries);
  if (result != BlobResult::kOk)
    {
      return result;
    }

  /* An empty directory is not a model.  Reporting success would hand the
   * backend a path with nothing in it and move the failure somewhere less
   * obvious.
   */
  if (entries.empty())
    {
      return BlobResult::kNotFound;
    }

  for (const BlobEntry &entry : entries)
    {
      const std::string child = name + "/" + entry.name;
      std::string ignored;

      result = entry.directory
                   ? ProvideDirectory(child, depth + 1, progress, stats)
                   : client_->Fetch(child, &ignored, progress, stats);
      if (result != BlobResult::kOk)
        {
          return result;
        }
    }

  return BlobResult::kOk;
}

BlobResult ModelProvisioner::Provide(const std::string &name,
                                     std::string *path,
                                     const BlobClient::Progress &progress,
                                     BlobStats *stats)
{
  if (client_ == nullptr || path == nullptr || !IsLogicalName(name))
    {
      return BlobResult::kInvalid;
    }

  if (!client_->Acquire())
    {
      return BlobResult::kBusy;
    }

  client_->ClearCancel();

  /* Try the name as a file first: that is one round trip for the common
   * case, and the responder says so explicitly when it is a directory.
   */
  BlobResult result = client_->Fetch(name, path, progress, stats);
  if (result == BlobResult::kDirectory)
    {
      result = ProvideDirectory(name, 1, progress, stats);
      if (result == BlobResult::kOk)
        {
          *path = client_->root() + "/" + name;
        }
    }

  client_->Release();
  return result;
}

BlobService::BlobService(std::uint32_t generation,
                         ModelProvisioner *provisioner)
    : generation_(generation), provisioner_(provisioner)
{
}

BlobService::~BlobService()
{
  if (worker_.joinable())
    {
      provisioner_->Cancel();
      worker_.join();
    }
}

void BlobService::Push(const Frame &frame, bool droppable)
{
  std::lock_guard<std::mutex> lock(queue_mutex_);

  /* Progress is advisory and may be shed under pressure; the response is
   * the only answer the requester will ever get, so it always goes in.
   */
  if (droppable && queue_.size() >= kQueueLimit)
    {
      return;
    }

  queue_.push_back(frame);
}

bool BlobService::Poll(Frame *frame)
{
  std::lock_guard<std::mutex> lock(queue_mutex_);

  if (frame == nullptr || queue_.empty())
    {
      return false;
    }

  *frame = queue_.front();
  queue_.pop_front();
  return true;
}

std::int32_t BlobService::Start(const nyamp_header_s &request,
                                std::function<void()> work)
{
  bool expected = false;

  if (provisioner_ == nullptr || provisioner_->client() == nullptr)
    {
      return NYAMP_MODEL_UNSUPPORTED;
    }

  if (!running_.compare_exchange_strong(expected, true))
    {
      return NYAMP_MODEL_BUSY;
    }

  try
    {
      /* The previous worker cleared running_ but may not be joined yet;
       * assigning over a joinable thread terminates the process.
       */
      if (worker_.joinable())
        {
          worker_.join();
        }

      active_request_.store(request.request_id);
      worker_ = std::thread(std::move(work));
    }
  catch (...)
    {
      active_request_.store(0);
      running_.store(false);
      return NYAMP_MODEL_BACKEND_ERROR;
    }

  return NYAMP_MODEL_OK;
}

std::int32_t BlobService::BeginBench(const nyamp_header_s &request,
                                     std::uint32_t rounds,
                                     std::uint32_t window_bytes)
{
  return Start(request, [this, request, rounds, window_bytes] {
    RunBench(request, rounds, window_bytes);
  });
}

std::int32_t BlobService::BeginPull(const nyamp_header_s &request,
                                    const std::string &name)
{
  if (!ModelProvisioner::IsLogicalName(name))
    {
      return NYAMP_MODEL_INVALID;
    }

  return Start(request, [this, request, name] { RunPull(request, name); });
}

void BlobService::Cancel(std::uint64_t request_id)
{
  if (request_id != 0 && active_request_.load() == request_id)
    {
      provisioner_->Cancel();
    }
}

void BlobService::RunBench(nyamp_header_s request, std::uint32_t rounds,
                           std::uint32_t window_bytes)
{
  BlobClient *client = provisioner_->client();
  nyamp_blob_bench_report_s report{};
  std::uint8_t body[NYAMP_BLOB_BENCH_REPORT_SIZE];
  std::size_t body_size = 0;
  BlobResult result = BlobResult::kBusy;
  Frame frame;

  if (client->Acquire())
    {
      client->ClearCancel();
      result = client->Bench(rounds, window_bytes, &report);
      client->Release();
    }

  std::fprintf(stderr,
               "nyampd: bench %s rounds=%u rtt_us=%u/%u/%u window=%u "
               "fill=%uKiB/s copy=%uKiB/s errors=%u\n",
               BlobResultName(result), report.rounds, report.rtt_min_us,
               report.rtt_avg_us, report.rtt_max_us, report.window_bytes,
               report.fill_kib_per_s, report.copy_kib_per_s,
               report.pattern_errors);

  if (result == BlobResult::kOk)
    {
      nyamp_blob_bench_report_encode(body, sizeof(body), &body_size, &report);
    }

  /* Release admission before publishing, for the reason the LLM worker
   * gives: whoever sees this response must be able to start the next run.
   */
  active_request_.store(0);
  running_.store(false, std::memory_order_release);
  if (EncodeStatusResponse(&frame, request, generation_,
                           BlobResultStatus(result), body, body_size))
    {
      Push(frame, false);
    }
}

void BlobService::RunPull(nyamp_header_s request, std::string name)
{
  using Clock = std::chrono::steady_clock;

  const Clock::time_point started = Clock::now();
  Clock::time_point reported = started;
  BlobStats stats;
  std::string path;
  Frame frame;

  auto progress = [&](const BlobProgress &update) {
    const Clock::time_point now = Clock::now();
    if (update.done != update.total &&
        now - reported < std::chrono::milliseconds(kProgressIntervalMs))
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
        Push(event, true);
      }
  };

  const BlobResult result =
      provisioner_->Provide(name, &path, progress, &stats);

  nyamp_blob_pull_report_s report{};
  std::uint8_t body[NYAMP_BLOB_PULL_REPORT_SIZE];
  std::size_t body_size = 0;

  report.bytes = stats.bytes;
  report.elapsed_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            started)
          .count());
  report.files = stats.files;
  report.reused = stats.reused;

  std::fprintf(stderr,
               "nyampd: pull %s -> %s: %s bytes=%llu files=%u reused=%u "
               "ms=%llu\n",
               name.c_str(), path.c_str(), BlobResultName(result),
               static_cast<unsigned long long>(report.bytes), report.files,
               report.reused,
               static_cast<unsigned long long>(report.elapsed_ms));

  if (result == BlobResult::kOk)
    {
      nyamp_blob_pull_report_encode(body, sizeof(body), &body_size, &report);
    }

  active_request_.store(0);
  running_.store(false, std::memory_order_release);
  if (EncodeStatusResponse(&frame, request, generation_,
                           BlobResultStatus(result), body, body_size))
    {
      Push(frame, false);
    }
}

} // namespace nyamp
