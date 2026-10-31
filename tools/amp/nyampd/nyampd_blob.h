/****************************************************************************
 * tools/amp/nyampd/nyampd_blob.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_BLOB_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_BLOB_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nyamp_protocol.h"
#include "nyampd_frame.h"

namespace nyamp
{

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Why a blob operation ended.  Finer than the wire status on purpose: "the
 * digest did not match" and "the peer went away" both reach the control
 * domain as a backend error, but they need different fixes and the daemon's
 * log is the only place that can tell them apart.
 */

enum class BlobResult
{
  kOk,
  kInvalid,   /* Illegal name or argument; nothing was sent.            */
  kNotFound,  /* The control domain has no such blob.                   */
  kDirectory, /* The name is a directory; list it instead.              */
  kBusy,      /* Another transfer owns the client or the responder.     */
  kStale,     /* The responder refused our generation.                  */
  kCancelled,
  kTimeout,
  kTransport, /* The frame could not be queued to the peer.             */
  kProtocol,  /* The response contradicts the request.                  */
  kSize,      /* The bytes received disagree with the announced size.   */
  kDigest,    /* SHA-256 of the received bytes disagrees with OPEN.     */
  kIo,        /* Local tmpfs or shared-window failure.                  */
  kRemote,    /* Any other error status from the responder.             */
};

const char *BlobResultName(BlobResult result);

/* The wire status the control domain should see for a given outcome. */
std::int32_t BlobResultStatus(BlobResult result);

struct BlobProgress
{
  std::string name;
  std::uint64_t done;
  std::uint64_t total;
  std::uint32_t bytes_per_second;
};

struct BlobEntry
{
  std::string name; /* One path component. */
  std::uint64_t size;
  bool directory;
};

struct BlobStats
{
  std::uint64_t bytes = 0; /* Bytes that actually crossed the window. */
  std::uint32_t files = 0;
  std::uint32_t reused = 0;
};

/****************************************************************************
 * Name: BlobTransport
 *
 * Description:
 *   Where the client puts outbound frames.  The production implementation
 *   queues them for the transport loop; the unit tests loop them straight
 *   into an in-process responder.  Send must be callable from any thread.
 *
 ****************************************************************************/

class BlobTransport
{
public:
  virtual ~BlobTransport() = default;
  virtual bool Send(const Frame &frame) = 0;
};

/****************************************************************************
 * Name: BlobWindow
 *
 * Description:
 *   The compute domain's view of the slot it grants for a transfer.  offset()
 *   is arena-absolute because that is what travels in the descriptor; data()
 *   is where those same bytes are mapped in this process.
 *
 ****************************************************************************/

class BlobWindow
{
public:
  virtual ~BlobWindow() = default;
  virtual const std::uint8_t *data() const = 0;
  virtual std::uint32_t offset() const = 0;
  virtual std::uint32_t capacity() const = 0;
};

/****************************************************************************
 * Name: BlobClient
 *
 * Description:
 *   Pulls named blobs from the control domain into a local directory.
 *
 *   The client is strictly serial: one request is outstanding at a time and
 *   one caller owns the client at a time (Acquire/Release).  That is not a
 *   shortcut.  There is one shared window, the responder reads one file from
 *   one eMMC, and the requester is about to mmap the result, so pipelining
 *   would add reordering hazards and buy nothing.
 *
 *   Operations block the calling thread, so they must never run on the
 *   transport loop: the loop is what delivers the responses (OnFrame).
 *
 ****************************************************************************/

class BlobClient
{
public:
  struct Options
  {
    std::string root = "/tmp/models";
    std::uint32_t window_bytes = 0; /* 0 = the whole granted slot. */

    /* OPEN may have to hash the file on the control domain first: 875 MB at
     * about 75 MiB/s is twelve seconds before the first byte can be answered.
     */
    int open_timeout_ms = 180000;
    int io_timeout_ms = 15000;
  };

  using Progress = std::function<void(const BlobProgress &)>;

  BlobClient(std::uint32_t generation, BlobTransport *transport,
             BlobWindow *window, Options options);

  BlobClient(const BlobClient &) = delete;
  BlobClient &operator=(const BlobClient &) = delete;

  /* Transport loop entry: hand over every BLOB response frame.  Frames that
   * answer nothing outstanding are dropped, never answered.
   */
  void OnFrame(const nyamp_header_s &header, const std::uint8_t *payload);

  /* Ownership.  Acquire fails instead of blocking, so a second requester
   * reports busy rather than queueing behind a multi-minute transfer.
   */
  bool Acquire();
  void Release();

  /* Fetch `name` into <root>/<name>.  On success `*path` names the verified
   * file.  An existing file whose `.sha256` marker matches the digest OPEN
   * reports is reused without moving a byte.
   */
  BlobResult Fetch(const std::string &name, std::string *path,
                   const Progress &progress, BlobStats *stats);

  BlobResult List(const std::string &prefix, std::vector<BlobEntry> *entries);

  BlobResult Bench(std::uint32_t rounds, std::uint32_t window_bytes,
                   nyamp_blob_bench_report_s *report);

  /* Abort the operation in flight, from any thread.  The flag is sticky
   * until the owner clears it, so a cancel that lands between two files of a
   * directory pull still stops the pull.
   */
  void Cancel();
  void ClearCancel();
  bool cancelled() const { return cancel_.load(); }

  const std::string &root() const { return options_.root; }

private:
  struct Reply
  {
    nyamp_header_s header;
    std::int32_t status;
    std::uint8_t body[NYAMP_INLINE_MAX];
    std::size_t body_size;
  };

  BlobResult Exchange(std::uint16_t opcode, const std::uint8_t *payload,
                      std::size_t payload_size, int timeout_ms, Reply *reply,
                      bool honour_cancel = true);
  BlobResult Transfer(const std::string &name, const nyamp_blob_info_s &info,
                      const std::string &part, const Progress &progress);
  bool Grant(std::uint32_t bytes, nyamp_buffer_s *buffer);
  void Close(std::uint32_t blob_id);
  void SendCancel(std::uint16_t opcode, std::uint64_t request_id);

  std::uint32_t generation_;
  BlobTransport *transport_;
  BlobWindow *window_;
  Options options_;

  std::atomic<bool> cancel_{ false };
  std::atomic<bool> owned_{ false };

  std::mutex mutex_;
  std::condition_variable arrived_;
  std::uint64_t waiting_id_ = 0;
  bool have_reply_ = false;
  Reply reply_{};

  std::uint64_t next_request_ = 0;
  std::uint64_t next_lease_ = 0;
};

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_BLOB_H */
