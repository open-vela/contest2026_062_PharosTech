/****************************************************************************
 * tools/amp/nyampd/nyampd_audio.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_AUDIO_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_AUDIO_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nyamp_protocol.h"
#include "nyampd_frame.h"

namespace nyamp
{

class BlobClient;

/****************************************************************************
 * Name: SharedSlot
 *
 * Description:
 *   One slot of the shared arena as this process sees it.  offset() is
 *   arena-absolute because that is what travels in a descriptor; data() is
 *   where the same bytes are mapped here, or null while the region is not
 *   reachable (an image without the shared-memory driver), in which case
 *   every request that needs the slot is refused instead of failing later.
 *
 ****************************************************************************/

class SharedSlot
{
public:
  virtual ~SharedSlot() = default;
  virtual std::uint8_t *data() = 0;
  virtual std::uint32_t offset() const = 0;
  virtual std::uint32_t capacity() const = 0;
};

/****************************************************************************
 * Name: SlotGate
 *
 * Description:
 *   Exclusive use of a slot.  Acquire never blocks: the second taker is
 *   told BUSY at once instead of queueing behind a transfer of unknown
 *   length.  The gate is what turns "these two never overlap" from a
 *   comment into something a request can be refused by.
 *
 ****************************************************************************/

class SlotGate
{
public:
  virtual ~SlotGate() = default;
  virtual bool Acquire() = 0;
  virtual void Release() = 0;
};

/* A gate of its own: the capture slot, and every slot in the unit tests. */

class AtomicGate final : public SlotGate
{
public:
  bool Acquire() override;
  void Release() override;
  bool held() const { return held_.load(); }

private:
  std::atomic<bool> held_{ false };
};

/* NYAMP_SLOT_SHARED already has an owner flag: the blob client's, because
 * model pulls were its first user.  Synthesized speech takes the same flag,
 * so a pull and a synthesis exclude each other without a second lock that
 * could disagree with the first.
 */

class BlobGate final : public SlotGate
{
public:
  explicit BlobGate(BlobClient *client) : client_(client) {}
  bool Acquire() override;
  void Release() override;

private:
  BlobClient *client_;
};

/****************************************************************************
 * Name: LeaseMint
 *
 * Description:
 *   Leases for the speech services.  generation << 32 | bit 31 | counter:
 *   the generation keeps a lease from surviving a daemon restart, bit 31
 *   keeps it apart from the blob client's leases (a bare counter), and the
 *   counter keeps a late echo of an earlier window from passing for the
 *   current one.  One mint serves all three services so no two grants of a
 *   generation ever share a lease.
 *
 ****************************************************************************/

class LeaseMint
{
public:
  explicit LeaseMint(std::uint32_t generation) : generation_(generation) {}
  std::uint64_t Next();
  std::uint32_t generation() const { return generation_; }

private:
  std::uint32_t generation_;
  std::atomic<std::uint32_t> counter_{ 0 };
};

/* A grant over `capacity` bytes at `offset`; length starts at zero. */

nyamp_buffer_s MakeGrant(LeaseMint *mint, std::uint32_t offset,
                         std::uint32_t capacity, std::uint32_t format,
                         std::uint16_t flags);

/* Whether `window` is a legal sub-range of `grant`: same lease and
 * generation, in shared memory, inside the granted bytes, carrying the
 * sample format the stream was opened with and a whole number of samples.
 */

bool WindowInsideGrant(const nyamp_buffer_s &grant,
                       const nyamp_buffer_s &window, std::uint32_t format);

inline std::size_t SampleBytes(std::uint32_t format)
{
  return format == NYAMP_FORMAT_S16 ? 2U : 4U;
}

/* Copy `count` samples out of a shared window as normalized float32.  The
 * source is uncached device-like memory, so it is read exactly once.
 */

void CopySamples(float *dest, const std::uint8_t *source, std::size_t count,
                 std::uint32_t format);

/****************************************************************************
 * Name: CaptureRing
 *
 * Description:
 *   The last `capacity` samples of a stream, addressed by absolute sample
 *   position.  One writer (the transport thread, as windows arrive), any
 *   number of readers, each with a cursor of its own; a reader never
 *   consumes anything, so a wake word listener and a recognizer can follow
 *   the same stream without knowing about each other.
 *
 *   Ownership is the writer's alone: whoever created the ring appends to it
 *   and closes it.  Readers hold a shared_ptr, so a ring outlives the
 *   stream that fed it for exactly as long as somebody is still draining it,
 *   and "the stream ended" reaches them as closed(), not as a dangling
 *   pointer.
 *
 *   A reader that falls more than the capacity behind loses the overwritten
 *   samples: its cursor is moved to the oldest sample still held and the
 *   read reports a gap.  Restart() is the writer's way to say the same thing
 *   about the source (capture was paused, a window was lost): the content is
 *   dropped, counting continues from the new position and every reader sees
 *   one gap.
 *
 ****************************************************************************/

class CaptureRing
{
public:
  struct Read
  {
    std::size_t count; /* Samples copied; the cursor advanced by as many. */
    bool gap;          /* Samples were skipped before the ones returned.  */
    bool closed;       /* No sample will ever follow the ones returned.   */
  };

  CaptureRing(std::size_t capacity, std::uint64_t first_position);

  /* Writer. */

  void Append(const float *samples, std::size_t count);
  void Restart(std::uint64_t position);
  void Close();

  /* Wake every reader without changing anything, so a reader blocked in
   * Fetch re-reads whatever its owner just changed (a limit, a cancel).
   */

  void Notify();

  /* Reader.  Copies up to `max` samples starting at *cursor, never past
   * `limit`, waiting at most `timeout_ms` for the first one.  `*epoch` is
   * the reader's memory of the last Restart it has seen; start it from
   * epoch().
   */

  Read Fetch(std::uint64_t *cursor, std::uint32_t *epoch, float *out,
             std::size_t max, std::uint64_t limit, int timeout_ms);

  std::uint64_t begin() const;
  std::uint64_t end() const;
  std::uint32_t epoch() const;
  bool closed() const;
  std::size_t capacity() const { return samples_.size(); }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<float> samples_;
  std::uint64_t begin_;
  std::uint64_t end_;
  std::uint32_t epoch_ = 0;
  bool closed_ = false;
};

/****************************************************************************
 * Name: FrameQueue
 *
 * Description:
 *   The bounded event queue every service keeps between its worker and the
 *   transport loop.  A droppable frame is shed when the queue is full; a
 *   frame that is the only answer its requester will get (a deferred
 *   response, a terminal finish) is queued regardless.
 *
 ****************************************************************************/

class FrameQueue
{
public:
  explicit FrameQueue(std::size_t limit) : limit_(limit) {}

  /* Called after every successful push, from the pushing thread.  The
   * daemon rings its eventfd here so a PCM window does not wait out the
   * loop's idle tick.
   */

  void SetWaker(std::function<void()> waker);

  bool Push(const Frame &frame, bool droppable);
  bool Poll(Frame *frame);
  std::size_t size() const;

private:
  std::size_t limit_;
  mutable std::mutex mutex_;
  std::deque<Frame> queue_;
  std::function<void()> waker_;
};

/* An EVENT frame for `service`/`opcode` tied to `request_id`. */

bool EncodeEvent(Frame *frame, std::uint16_t service, std::uint16_t opcode,
                 std::uint64_t request_id, std::uint32_t generation,
                 const std::uint8_t *payload, std::size_t payload_size);

/* The longest prefix of `text` that is at most `max` bytes and does not end
 * inside a UTF-8 sequence.  Never zero for a non-empty text and max >= 4.
 */

std::size_t Utf8Prefix(const std::string &text, std::size_t from,
                       std::size_t max);

/* Well-formed UTF-8 without NUL. */

bool ValidUtf8(const std::string &text);

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_AUDIO_H */
