/****************************************************************************
 * tools/amp/nyampd/nyampd_blob.cpp
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

#include "nyampd_blob.h"

#include "nyampd_sha256.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nyamp
{
namespace
{

/* How often a blocked exchange looks at the cancel flag.  Short enough that a
 * cancel feels immediate, long enough that an idle wait costs nothing.
 */

constexpr int kCancelPollMs = 20;

/* Bounds on a directory listing.  A model directory holds a handful of
 * files; a responder that keeps returning pages is broken, not large.
 */

constexpr std::size_t kMaxListPages = 256;
constexpr std::size_t kMaxListEntries = 1024;

constexpr std::uint32_t kBenchBlock = 4096;
constexpr std::uint32_t kBenchMaxRounds = 10000;
constexpr std::uint32_t kBenchMaxFills = 32;

using Clock = std::chrono::steady_clock;

std::uint64_t MicrosecondsSince(Clock::time_point start)
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                            start)
          .count());
}

/****************************************************************************
 * Name: MakeParents
 *
 * Description:
 *   Create every directory above `path`.  The name has already passed the
 *   blob name rules, so no component can climb out of the root.
 *
 ****************************************************************************/

bool MakeParents(const std::string &path)
{
  for (std::size_t index = 1; index < path.size(); ++index)
    {
      if (path[index] != '/')
        {
          continue;
        }

      const std::string parent = path.substr(0, index);
      if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST)
        {
          return false;
        }
    }

  return true;
}

bool WriteAll(int fd, const std::uint8_t *data, std::size_t size)
{
  while (size != 0)
    {
      const ssize_t written = write(fd, data, size);
      if (written < 0 && errno == EINTR)
        {
          continue;
        }

      if (written <= 0)
        {
          return false;
        }

      data += written;
      size -= static_cast<std::size_t>(written);
    }

  return true;
}

std::string ReadMarker(const std::string &path)
{
  char text[kSha256Size * 2 + 1] = {};
  std::FILE *file = std::fopen(path.c_str(), "r");

  if (file == nullptr)
    {
      return std::string();
    }

  const std::size_t got = std::fread(text, 1, kSha256Size * 2, file);
  std::fclose(file);
  return std::string(text, got);
}

bool WriteMarker(const std::string &path, const std::string &hex)
{
  std::FILE *file = std::fopen(path.c_str(), "w");

  if (file == nullptr)
    {
      return false;
    }

  const bool ok = std::fputs(hex.c_str(), file) >= 0 &&
                  std::fputc('\n', file) != EOF;
  return std::fclose(file) == 0 && ok;
}

BlobResult RemoteResult(std::int32_t status)
{
  switch (status)
    {
      case NYAMP_MODEL_OK:
        return BlobResult::kOk;
      case NYAMP_MODEL_INVALID:
        return BlobResult::kInvalid;
      case NYAMP_MODEL_NOT_READY:
        return BlobResult::kNotFound;
      case NYAMP_MODEL_UNSUPPORTED:
        return BlobResult::kDirectory;
      case NYAMP_MODEL_BUSY:
        return BlobResult::kBusy;
      case NYAMP_MODEL_STALE_GENERATION:
        return BlobResult::kStale;
      case NYAMP_MODEL_CANCELLED:
        return BlobResult::kCancelled;
      default:
        return BlobResult::kRemote;
    }
}

} // namespace

const char *BlobResultName(BlobResult result)
{
  switch (result)
    {
      case BlobResult::kOk:
        return "ok";
      case BlobResult::kInvalid:
        return "invalid";
      case BlobResult::kNotFound:
        return "not-found";
      case BlobResult::kDirectory:
        return "directory";
      case BlobResult::kBusy:
        return "busy";
      case BlobResult::kStale:
        return "stale-generation";
      case BlobResult::kCancelled:
        return "cancelled";
      case BlobResult::kTimeout:
        return "timeout";
      case BlobResult::kTransport:
        return "transport";
      case BlobResult::kProtocol:
        return "protocol";
      case BlobResult::kSize:
        return "size-mismatch";
      case BlobResult::kDigest:
        return "digest-mismatch";
      case BlobResult::kIo:
        return "local-io";
      case BlobResult::kRemote:
        return "remote-error";
    }

  return "unknown";
}

std::int32_t BlobResultStatus(BlobResult result)
{
  switch (result)
    {
      case BlobResult::kOk:
        return NYAMP_MODEL_OK;
      case BlobResult::kInvalid:
        return NYAMP_MODEL_INVALID;
      case BlobResult::kNotFound:
      case BlobResult::kDirectory:
        return NYAMP_MODEL_NOT_READY;
      case BlobResult::kBusy:
        return NYAMP_MODEL_BUSY;
      case BlobResult::kStale:
        return NYAMP_MODEL_STALE_GENERATION;
      case BlobResult::kCancelled:
        return NYAMP_MODEL_CANCELLED;
      case BlobResult::kTimeout:
        return NYAMP_MODEL_DEADLINE;
      default:
        return NYAMP_MODEL_BACKEND_ERROR;
    }
}

BlobClient::BlobClient(std::uint32_t generation, BlobTransport *transport,
                       BlobWindow *window, Options options)
    : generation_(generation), transport_(transport), window_(window),
      options_(std::move(options))
{
}

bool BlobClient::Acquire()
{
  bool expected = false;
  return owned_.compare_exchange_strong(expected, true);
}

void BlobClient::Release() { owned_.store(false); }

void BlobClient::Cancel()
{
  cancel_.store(true);
  arrived_.notify_all();
}

void BlobClient::ClearCancel() { cancel_.store(false); }

void BlobClient::OnFrame(const nyamp_header_s &header,
                         const std::uint8_t *payload)
{
  std::lock_guard<std::mutex> lock(mutex_);

  /* Only the response to the one outstanding request is of interest.  A late
   * answer to a request that was abandoned is dropped here; answering it, or
   * treating it as the next response, are the two ways this could go wrong.
   */
  if ((header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_RESPONSE ||
      waiting_id_ == 0 || header.request_id != waiting_id_ || have_reply_)
    {
      return;
    }

  const std::uint8_t *body = nullptr;
  std::size_t body_size = 0;

  reply_.header = header;
  if (nyamp_status_decode(&reply_.status, &body, &body_size, payload,
                          header.payload_size) != NYAMP_OK)
    {
      /* A response with no status cannot be a success. */
      reply_.status = NYAMP_MODEL_BACKEND_ERROR;
      body_size = 0;
    }

  reply_.body_size = body_size;
  if (body_size != 0)
    {
      std::memcpy(reply_.body, body, body_size);
    }

  have_reply_ = true;
  arrived_.notify_all();
}

void BlobClient::SendCancel(std::uint16_t opcode, std::uint64_t request_id)
{
  Frame frame;
  nyamp_header_s header{};

  header.service = NYAMP_SERVICE_BLOB;
  header.opcode = opcode;
  header.flags = NYAMP_FLAG_CANCEL;
  header.request_id = request_id;
  header.generation = generation_;
  if (EncodeFrame(&frame, header, nullptr, 0))
    {
      transport_->Send(frame);
    }
}

BlobResult BlobClient::Exchange(std::uint16_t opcode,
                                const std::uint8_t *payload,
                                std::size_t payload_size, int timeout_ms,
                                Reply *reply, bool honour_cancel)
{
  Frame frame;
  nyamp_header_s header{};

  if (honour_cancel && cancel_.load())
    {
      return BlobResult::kCancelled;
    }

  header.service = NYAMP_SERVICE_BLOB;
  header.opcode = opcode;
  header.flags = NYAMP_FLAG_REQUEST;
  header.generation = generation_;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    header.request_id = NYAMP_REQUEST_ID_COMPUTE | ++next_request_;
    waiting_id_ = header.request_id;
    have_reply_ = false;
  }

  /* The slot is armed before the frame leaves: a loopback transport answers
   * from inside Send, and a real one may answer before this thread runs
   * again.
   */
  if (!EncodeFrame(&frame, header, payload, payload_size) ||
      !transport_->Send(frame))
    {
      std::lock_guard<std::mutex> lock(mutex_);
      waiting_id_ = 0;
      return BlobResult::kTransport;
    }

  BlobResult outcome = BlobResult::kOk;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    int waited = 0;

    while (!have_reply_)
      {
        if (honour_cancel && cancel_.load())
          {
            outcome = BlobResult::kCancelled;
            break;
          }

        if (waited >= timeout_ms)
          {
            outcome = BlobResult::kTimeout;
            break;
          }

        arrived_.wait_for(lock, std::chrono::milliseconds(kCancelPollMs));
        waited += kCancelPollMs;
      }

    waiting_id_ = 0;
    if (outcome == BlobResult::kOk)
      {
        *reply = reply_;
      }

    have_reply_ = false;
  }

  if (outcome != BlobResult::kOk)
    {
      /* Only OPEN runs long enough on the responder to be worth aborting,
       * and it is the one request whose late success would leak a blob.
       */
      if (opcode == NYAMP_BLOB_OPEN)
        {
          SendCancel(opcode, header.request_id);
        }

      return outcome;
    }

  if (reply->header.service != NYAMP_SERVICE_BLOB ||
      reply->header.opcode != opcode ||
      reply->header.generation != generation_)
    {
      return BlobResult::kProtocol;
    }

  return RemoteResult(reply->status);
}

bool BlobClient::Grant(std::uint32_t bytes, nyamp_buffer_s *buffer)
{
  if (window_ == nullptr || window_->data() == nullptr || bytes == 0 ||
      bytes > window_->capacity())
    {
      return false;
    }

  /* The lease names this grant and nothing else: the generation keeps it
   * from surviving a daemon restart, the counter keeps a delayed response to
   * an earlier window from being taken for the current one.
   */
  std::memset(buffer, 0, sizeof(*buffer));
  buffer->magic = NYAMP_BUFFER_MAGIC;
  buffer->version = NYAMP_BUFFER_VERSION;
  buffer->flags = NYAMP_BUFFER_IN_SHMEM;
  buffer->offset = window_->offset();
  buffer->length = 0;
  buffer->capacity = bytes;
  buffer->format = NYAMP_FORMAT_BYTES;
  buffer->lease = (static_cast<std::uint64_t>(generation_) << 32) |
                  (++next_lease_ & 0xffffffffULL);
  buffer->generation = generation_;
  return true;
}

void BlobClient::Close(std::uint32_t blob_id)
{
  std::uint8_t payload[NYAMP_BLOB_CLOSE_SIZE];
  std::size_t size = 0;
  Reply reply;

  if (nyamp_blob_close_encode(payload, sizeof(payload), &size, blob_id) ==
      NYAMP_OK)
    {
      /* Best effort, and it must go out even when the transfer was
       * cancelled: a blob left open holds a descriptor on the control domain
       * until the next generation.
       */
      Exchange(NYAMP_BLOB_CLOSE, payload, size, options_.io_timeout_ms, &reply,
               false);
    }
}

BlobResult BlobClient::Transfer(const std::string &name,
                                const nyamp_blob_info_s &info,
                                const std::string &part,
                                const Progress &progress)
{
  const std::uint32_t window =
      options_.window_bytes != 0 && window_ != nullptr
          ? std::min(options_.window_bytes, window_->capacity())
          : (window_ != nullptr ? window_->capacity() : 0);

  if (window == 0 && info.size != 0)
    {
      return BlobResult::kIo;
    }

  const int fd =
      open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    {
      return BlobResult::kIo;
    }

  /* The window is uncached device-style memory shared with another OS.  One
   * bulk copy into ordinary memory is the cheapest thing that can be done
   * with it; hashing and writing then run on the private copy, and the
   * window is free for the next grant the moment the copy returns.
   */
  std::vector<std::uint8_t> bounce(window);
  Sha256 hash;
  std::uint64_t offset = 0;
  BlobResult result = BlobResult::kOk;
  const Clock::time_point started = Clock::now();

  while (offset < info.size)
    {
      std::uint8_t payload[NYAMP_BLOB_READ_SIZE];
      std::size_t size = 0;
      nyamp_blob_read_s request{};
      nyamp_blob_read_s answer{};
      Reply reply;

      const std::uint64_t remaining = info.size - offset;
      const std::uint32_t want = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(window, remaining));

      request.blob_id = info.blob_id;
      request.file_offset = offset;
      if (!Grant(want, &request.buffer) ||
          nyamp_blob_read_encode(payload, sizeof(payload), &size, &request) !=
              NYAMP_OK)
        {
          result = BlobResult::kIo;
          break;
        }

      result = Exchange(NYAMP_BLOB_READ, payload, size, options_.io_timeout_ms,
                        &reply);
      if (result != BlobResult::kOk)
        {
          break;
        }

      /* The response must describe exactly the window that was granted.
       * Anything else means the bytes in the window are not the bytes that
       * were asked for, whatever they look like.
       */
      if (nyamp_blob_read_decode(&answer, reply.body, reply.body_size) !=
              NYAMP_OK ||
          answer.blob_id != request.blob_id ||
          answer.file_offset != request.file_offset ||
          answer.buffer.offset != request.buffer.offset ||
          answer.buffer.capacity != request.buffer.capacity ||
          answer.buffer.lease != request.buffer.lease ||
          answer.buffer.generation != request.buffer.generation ||
          answer.buffer.length == 0)
        {
          result = BlobResult::kProtocol;
          break;
        }

      const std::uint32_t got = answer.buffer.length;
      const bool at_end = offset + got == info.size;

      /* A short read is legal and simply advances less.  End of file before
       * the announced size is not: the file changed under the transfer.
       */
      if (got > remaining ||
          ((answer.flags & NYAMP_BLOB_READ_EOF) != 0 && !at_end))
        {
          result = BlobResult::kSize;
          break;
        }

      std::memcpy(bounce.data(), window_->data(), got);
      hash.Update(bounce.data(), got);
      if (!WriteAll(fd, bounce.data(), got))
        {
          result = BlobResult::kIo;
          break;
        }

      offset += got;
      if (progress)
        {
          const std::uint64_t micros = MicrosecondsSince(started);
          BlobProgress report;

          report.name = name;
          report.done = offset;
          report.total = info.size;
          report.bytes_per_second = static_cast<std::uint32_t>(
              micros == 0 ? 0 : offset * 1000000ULL / micros);
          progress(report);
        }
    }

  if (close(fd) != 0 && result == BlobResult::kOk)
    {
      result = BlobResult::kIo;
    }

  if (result == BlobResult::kOk)
    {
      std::uint8_t digest[kSha256Size];

      hash.Final(digest);
      if (std::memcmp(digest, info.sha256, kSha256Size) != 0)
        {
          std::fprintf(stderr, "nyampd: blob %s digest %s, expected %s\n",
                       name.c_str(), Sha256Hex(digest).c_str(),
                       Sha256Hex(info.sha256).c_str());
          result = BlobResult::kDigest;
        }
    }

  return result;
}

BlobResult BlobClient::Fetch(const std::string &name, std::string *path,
                             const Progress &progress, BlobStats *stats)
{
  std::uint8_t payload[NYAMP_INLINE_MAX];
  std::size_t size = 0;
  nyamp_blob_info_s info{};
  Reply reply;

  if (path == nullptr ||
      nyamp_blob_open_encode(payload, sizeof(payload), &size, 0, name.data(),
                             name.size()) != NYAMP_OK)
    {
      return BlobResult::kInvalid;
    }

  BlobResult result = Exchange(NYAMP_BLOB_OPEN, payload, size,
                               options_.open_timeout_ms, &reply);
  if (result != BlobResult::kOk)
    {
      return result;
    }

  if (nyamp_blob_info_decode(&info, reply.body, reply.body_size) != NYAMP_OK)
    {
      return BlobResult::kProtocol;
    }

  const std::string target = options_.root + "/" + name;
  const std::string marker = target + ".sha256";
  const std::string part = target + ".part";
  const std::string expected = Sha256Hex(info.sha256);
  struct stat existing;

  /* The marker is only ever written after a verified transfer, and the
   * directory is private to this daemon, so marker + size is enough to trust
   * the file without reading 875 MB back just to hash it again.
   */
  if (stat(target.c_str(), &existing) == 0 && S_ISREG(existing.st_mode) &&
      static_cast<std::uint64_t>(existing.st_size) == info.size &&
      ReadMarker(marker) == expected)
    {
      Close(info.blob_id);
      *path = target;
      if (stats != nullptr)
        {
          ++stats->files;
          ++stats->reused;
        }

      return BlobResult::kOk;
    }

  /* Drop the marker before touching the file it vouches for, so an
   * interrupted refresh can never leave an old digest beside new bytes.
   */
  unlink(marker.c_str());

  if (!MakeParents(target))
    {
      Close(info.blob_id);
      return BlobResult::kIo;
    }

  result = Transfer(name, info, part, progress);

  /* A stale generation means the responder has already dropped the blob, and
   * a dead transport cannot carry the close.
   */
  if (result != BlobResult::kStale && result != BlobResult::kTransport)
    {
      Close(info.blob_id);
    }

  if (result == BlobResult::kOk &&
      (rename(part.c_str(), target.c_str()) != 0 ||
       !WriteMarker(marker, expected)))
    {
      result = BlobResult::kIo;
    }

  if (result != BlobResult::kOk)
    {
      unlink(part.c_str());
      return result;
    }

  *path = target;
  if (stats != nullptr)
    {
      ++stats->files;
      stats->bytes += info.size;
    }

  return BlobResult::kOk;
}

BlobResult BlobClient::List(const std::string &prefix,
                            std::vector<BlobEntry> *entries)
{
  std::uint32_t cursor = 0;

  if (entries == nullptr)
    {
      return BlobResult::kInvalid;
    }

  entries->clear();
  for (std::size_t page = 0; page < kMaxListPages; ++page)
    {
      std::uint8_t payload[NYAMP_INLINE_MAX];
      std::size_t size = 0;
      std::uint32_t next = 0;
      std::uint32_t count = 0;
      std::size_t position = 0;
      Reply reply;

      if (nyamp_blob_list_encode(payload, sizeof(payload), &size, cursor,
                                 prefix.data(), prefix.size()) != NYAMP_OK)
        {
          return BlobResult::kInvalid;
        }

      const BlobResult result = Exchange(NYAMP_BLOB_LIST, payload, size,
                                         options_.io_timeout_ms, &reply);
      if (result != BlobResult::kOk)
        {
          /* UNSUPPORTED means "a directory" only as an answer to OPEN. */
          return result == BlobResult::kDirectory ? BlobResult::kRemote
                                                  : result;
        }

      if (nyamp_blob_list_body_decode(&next, &count, reply.body,
                                      reply.body_size) != NYAMP_OK)
        {
          return BlobResult::kProtocol;
        }

      for (std::uint32_t index = 0; index < count; ++index)
        {
          nyamp_blob_entry_s entry;

          if (nyamp_blob_list_body_next(&entry, &position, reply.body,
                                        reply.body_size) != NYAMP_OK ||
              entries->size() >= kMaxListEntries)
            {
              return BlobResult::kProtocol;
            }

          entries->push_back(
              { std::string(entry.name, entry.name_length), entry.size,
                (entry.flags & NYAMP_BLOB_ENTRY_DIRECTORY) != 0 });
        }

      if (next == 0)
        {
          return BlobResult::kOk;
        }

      /* A cursor that does not move would loop forever. */
      if (next == cursor)
        {
          return BlobResult::kProtocol;
        }

      cursor = next;
    }

  return BlobResult::kProtocol;
}

BlobResult BlobClient::Bench(std::uint32_t rounds, std::uint32_t window_bytes,
                             nyamp_blob_bench_report_s *report)
{
  if (report == nullptr)
    {
      return BlobResult::kInvalid;
    }

  std::memset(report, 0, sizeof(*report));
  rounds = std::max<std::uint32_t>(1, std::min(rounds, kBenchMaxRounds));

  /* Round trips first: an 8-byte echo measures the message path alone, which
   * is the fixed cost every window pays once.
   */
  std::uint64_t total_us = 0;
  std::uint64_t min_us = UINT64_MAX;
  std::uint64_t max_us = 0;

  for (std::uint32_t index = 0; index < rounds; ++index)
    {
      std::uint8_t payload[NYAMP_BLOB_BENCH_FILL_SIZE];
      std::size_t size = 0;
      nyamp_blob_bench_s bench{};
      nyamp_blob_bench_s echo{};
      Reply reply;

      bench.mode = NYAMP_BLOB_BENCH_ECHO;
      bench.seed = 0x6e796200U + index;
      if (nyamp_blob_bench_encode(payload, sizeof(payload), &size, &bench) !=
          NYAMP_OK)
        {
          return BlobResult::kInvalid;
        }

      const Clock::time_point start = Clock::now();
      const BlobResult result = Exchange(NYAMP_BLOB_BENCH, payload, size,
                                         options_.io_timeout_ms, &reply);
      const std::uint64_t elapsed = MicrosecondsSince(start);

      if (result != BlobResult::kOk)
        {
          return result == BlobResult::kDirectory ? BlobResult::kRemote
                                                  : result;
        }

      if (nyamp_blob_bench_decode(&echo, reply.body, reply.body_size) !=
              NYAMP_OK ||
          echo.mode != bench.mode || echo.seed != bench.seed)
        {
          return BlobResult::kProtocol;
        }

      total_us += elapsed;
      min_us = std::min(min_us, elapsed);
      max_us = std::max(max_us, elapsed);
    }

  report->rounds = rounds;
  report->rtt_min_us = static_cast<std::uint32_t>(min_us);
  report->rtt_avg_us = static_cast<std::uint32_t>(total_us / rounds);
  report->rtt_max_us = static_cast<std::uint32_t>(max_us);

  if (window_ == nullptr || window_->data() == nullptr)
    {
      /* No shared region: the round-trip numbers are still worth having. */
      return BlobResult::kOk;
    }

  std::uint32_t bytes = window_bytes == 0 ? window_->capacity() : window_bytes;
  bytes = std::min(bytes, window_->capacity()) / kBenchBlock * kBenchBlock;
  if (bytes == 0)
    {
      return BlobResult::kInvalid;
    }

  const std::uint32_t fills = std::min(rounds, kBenchMaxFills);
  std::vector<std::uint8_t> bounce(bytes);
  std::uint8_t expected[kBenchBlock];
  std::uint64_t fill_us = 0;
  std::uint64_t copy_us = 0;
  std::uint32_t errors = 0;

  for (std::uint32_t index = 0; index < fills; ++index)
    {
      std::uint8_t payload[NYAMP_BLOB_BENCH_FILL_SIZE];
      std::size_t size = 0;
      nyamp_blob_bench_s bench{};
      nyamp_blob_bench_s echo{};
      Reply reply;

      /* A different seed per fill at the same address is deliberate: a
       * mapping that serves stale bytes passes a single fill and fails here.
       */
      bench.mode = NYAMP_BLOB_BENCH_FILL;
      bench.seed = 0x5eed0000U + index * 0x01010101U;
      if (!Grant(bytes, &bench.buffer) ||
          nyamp_blob_bench_encode(payload, sizeof(payload), &size, &bench) !=
              NYAMP_OK)
        {
          return BlobResult::kIo;
        }

      Clock::time_point start = Clock::now();
      const BlobResult result = Exchange(NYAMP_BLOB_BENCH, payload, size,
                                         options_.io_timeout_ms, &reply);
      fill_us += MicrosecondsSince(start);
      if (result != BlobResult::kOk)
        {
          return result == BlobResult::kDirectory ? BlobResult::kRemote
                                                  : result;
        }

      if (nyamp_blob_bench_decode(&echo, reply.body, reply.body_size) !=
              NYAMP_OK ||
          echo.mode != bench.mode || echo.seed != bench.seed ||
          echo.buffer.lease != bench.buffer.lease ||
          echo.buffer.length != bytes)
        {
          return BlobResult::kProtocol;
        }

      start = Clock::now();
      std::memcpy(bounce.data(), window_->data(), bytes);
      copy_us += MicrosecondsSince(start);

      for (std::uint32_t block = 0; block < bytes / kBenchBlock; ++block)
        {
          nyamp_blob_bench_pattern(expected, kBenchBlock / 4, bench.seed,
                                   block * (kBenchBlock / 4));
          const std::uint8_t *actual = bounce.data() + block * kBenchBlock;
          if (std::memcmp(actual, expected, kBenchBlock) == 0)
            {
              continue;
            }

          for (std::uint32_t word = 0; word < kBenchBlock / 4; ++word)
            {
              if (std::memcmp(actual + word * 4, expected + word * 4, 4) != 0)
                {
                  ++errors;
                }
            }
        }
    }

  const std::uint64_t kib = static_cast<std::uint64_t>(bytes) * fills / 1024;
  report->window_bytes = bytes;

  /* The fill figure includes one round trip per window, exactly as a real
   * READ does, so it is the number to compare window sizes with.
   */
  report->fill_kib_per_s = static_cast<std::uint32_t>(
      fill_us == 0 ? 0 : kib * 1000000ULL / fill_us);
  report->copy_kib_per_s = static_cast<std::uint32_t>(
      copy_us == 0 ? 0 : kib * 1000000ULL / copy_us);
  report->pattern_errors = errors;
  return BlobResult::kOk;
}

} // namespace nyamp
