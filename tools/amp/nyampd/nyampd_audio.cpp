/****************************************************************************
 * tools/amp/nyampd/nyampd_audio.cpp
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

#include "nyampd_audio.h"

#include "nyampd_blob.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace nyamp
{

/****************************************************************************
 * Gates and leases
 ****************************************************************************/

bool AtomicGate::Acquire()
{
  bool expected = false;
  return held_.compare_exchange_strong(expected, true);
}

void AtomicGate::Release() { held_.store(false); }

bool BlobGate::Acquire() { return client_ != nullptr && client_->Acquire(); }

void BlobGate::Release()
{
  if (client_ != nullptr)
    {
      client_->Release();
    }
}

std::uint64_t LeaseMint::Next()
{
  const std::uint32_t count = ++counter_;
  return (static_cast<std::uint64_t>(generation_) << 32) | 0x80000000ULL |
         (count & 0x7fffffffU);
}

nyamp_buffer_s MakeGrant(LeaseMint *mint, std::uint32_t offset,
                         std::uint32_t capacity, std::uint32_t format,
                         std::uint16_t flags)
{
  nyamp_buffer_s buffer{};

  buffer.magic = NYAMP_BUFFER_MAGIC;
  buffer.version = NYAMP_BUFFER_VERSION;
  buffer.flags = static_cast<std::uint16_t>(NYAMP_BUFFER_IN_SHMEM | flags);
  buffer.offset = offset;
  buffer.length = 0;
  buffer.capacity = capacity;
  buffer.format = format;
  buffer.lease = mint->Next();
  buffer.generation = mint->generation();
  return buffer;
}

bool WindowInsideGrant(const nyamp_buffer_s &grant,
                       const nyamp_buffer_s &window, std::uint32_t format)
{
  if (window.magic != NYAMP_BUFFER_MAGIC ||
      window.version != NYAMP_BUFFER_VERSION ||
      (window.flags & NYAMP_BUFFER_IN_SHMEM) == 0 ||
      (window.flags & NYAMP_BUFFER_FROM_COMPUTE) != 0 ||
      window.lease != grant.lease || window.generation != grant.generation ||
      window.format != format)
    {
      return false;
    }

  /* 64-bit sums: offset + length of a hostile descriptor must not wrap
   * back inside the grant.
   */
  const std::uint64_t first = window.offset;
  const std::uint64_t last = first + window.length;
  const std::uint64_t grant_first = grant.offset;
  const std::uint64_t grant_last = grant_first + grant.capacity;

  return first >= grant_first && last <= grant_last &&
         window.length % SampleBytes(format) == 0;
}

void CopySamples(float *dest, const std::uint8_t *source, std::size_t count,
                 std::uint32_t format)
{
  if (format != NYAMP_FORMAT_S16)
    {
      std::memcpy(dest, source, count * sizeof(float));
      return;
    }

  for (std::size_t index = 0; index < count; ++index)
    {
      std::int16_t sample;
      std::memcpy(&sample, source + index * 2, sizeof(sample));
      dest[index] = static_cast<float>(sample) / 32768.0f;
    }
}

/****************************************************************************
 * CaptureRing
 ****************************************************************************/

CaptureRing::CaptureRing(std::size_t capacity, std::uint64_t first_position)
    : samples_(capacity == 0 ? 1 : capacity), begin_(first_position),
      end_(first_position)
{
}

void CaptureRing::Append(const float *samples, std::size_t count)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t capacity = samples_.size();

    if (closed_ || samples == nullptr || count == 0)
      {
        return;
      }

    /* More than the ring holds: only the tail can survive anyway. */
    if (count > capacity)
      {
        end_ += count - capacity;
        samples += count - capacity;
        count = capacity;
      }

    std::size_t position = static_cast<std::size_t>(end_ % capacity);
    const std::size_t head = std::min(count, capacity - position);
    std::memcpy(samples_.data() + position, samples, head * sizeof(float));
    std::memcpy(samples_.data(), samples + head,
                (count - head) * sizeof(float));
    end_ += count;
    if (end_ - begin_ > capacity)
      {
        begin_ = end_ - capacity;
      }
  }

  changed_.notify_all();
}

void CaptureRing::Restart(std::uint64_t position)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
      {
        return;
      }

    begin_ = position;
    end_ = position;
    ++epoch_;
  }

  changed_.notify_all();
}

void CaptureRing::Close()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }

  changed_.notify_all();
}

void CaptureRing::Notify()
{
  /* Taking the mutex orders this after a reader's predicate check, so the
   * wake-up cannot fall between that check and the wait.
   */
  {
    std::lock_guard<std::mutex> lock(mutex_);
  }

  changed_.notify_all();
}

CaptureRing::Read CaptureRing::Fetch(std::uint64_t *cursor,
                                     std::uint32_t *epoch, float *out,
                                     std::size_t max, std::uint64_t limit,
                                     int timeout_ms)
{
  std::unique_lock<std::mutex> lock(mutex_);
  Read result{ 0, false, false };

  auto readable = [&] {
    return closed_ || *epoch != epoch_ || *cursor < begin_ ||
           (*cursor < end_ && *cursor < limit);
  };

  if (!readable() && timeout_ms > 0)
    {
      /* One wait, not a loop: a Notify has to get the caller back to its
       * own flags even though nothing here changed.
       */
      changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    }

  if (*epoch != epoch_)
    {
      *epoch = epoch_;
      result.gap = true;
      if (*cursor < begin_)
        {
          *cursor = begin_;
        }
    }
  else if (*cursor < begin_)
    {
      *cursor = begin_;
      result.gap = true;
    }

  if (*cursor < end_ && *cursor < limit && out != nullptr)
    {
      const std::size_t capacity = samples_.size();
      const std::uint64_t stop = std::min(end_, limit);
      const std::size_t count = static_cast<std::size_t>(
          std::min<std::uint64_t>(max, stop - *cursor));
      const std::size_t position = static_cast<std::size_t>(*cursor % capacity);
      const std::size_t head = std::min(count, capacity - position);

      std::memcpy(out, samples_.data() + position, head * sizeof(float));
      std::memcpy(out + head, samples_.data(), (count - head) * sizeof(float));
      *cursor += count;
      result.count = count;
    }

  result.closed = closed_ && *cursor >= end_;
  return result;
}

std::uint64_t CaptureRing::begin() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return begin_;
}

std::uint64_t CaptureRing::end() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return end_;
}

std::uint32_t CaptureRing::epoch() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return epoch_;
}

bool CaptureRing::closed() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

/****************************************************************************
 * FrameQueue
 ****************************************************************************/

void FrameQueue::SetWaker(std::function<void()> waker)
{
  std::lock_guard<std::mutex> lock(mutex_);
  waker_ = std::move(waker);
}

bool FrameQueue::Push(const Frame &frame, bool droppable)
{
  std::function<void()> waker;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (droppable && queue_.size() >= limit_)
      {
        return false;
      }

    queue_.push_back(frame);
    waker = waker_;
  }

  if (waker)
    {
      waker();
    }

  return true;
}

bool FrameQueue::Poll(Frame *frame)
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (frame == nullptr || queue_.empty())
    {
      return false;
    }

  *frame = queue_.front();
  queue_.pop_front();
  return true;
}

std::size_t FrameQueue::size() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

bool EncodeEvent(Frame *frame, std::uint16_t service, std::uint16_t opcode,
                 std::uint64_t request_id, std::uint32_t generation,
                 const std::uint8_t *payload, std::size_t payload_size)
{
  nyamp_header_s header{};

  header.service = service;
  header.opcode = opcode;
  header.flags = NYAMP_FLAG_EVENT;
  header.request_id = request_id;
  header.generation = generation;
  return EncodeFrame(frame, header, payload, payload_size);
}

/****************************************************************************
 * UTF-8
 ****************************************************************************/

std::size_t Utf8Prefix(const std::string &text, std::size_t from,
                       std::size_t max)
{
  std::size_t length = std::min(max, text.size() - from);

  /* Back off while the byte after the cut is a continuation byte. */
  while (length > 0 && from + length < text.size() &&
         (static_cast<unsigned char>(text[from + length]) & 0xc0U) == 0x80U)
    {
      --length;
    }

  return length;
}

bool ValidUtf8(const std::string &text)
{
  std::size_t index = 0;

  while (index < text.size())
    {
      const unsigned char lead = static_cast<unsigned char>(text[index]);
      std::size_t extra;
      std::uint32_t code;

      if (lead == 0)
        {
          return false;
        }
      else if (lead < 0x80U)
        {
          ++index;
          continue;
        }
      else if ((lead & 0xe0U) == 0xc0U)
        {
          extra = 1;
          code = lead & 0x1fU;
        }
      else if ((lead & 0xf0U) == 0xe0U)
        {
          extra = 2;
          code = lead & 0x0fU;
        }
      else if ((lead & 0xf8U) == 0xf0U)
        {
          extra = 3;
          code = lead & 0x07U;
        }
      else
        {
          return false;
        }

      if (index + extra >= text.size())
        {
          return false;
        }

      for (std::size_t step = 1; step <= extra; ++step)
        {
          const unsigned char next =
              static_cast<unsigned char>(text[index + step]);
          if ((next & 0xc0U) != 0x80U)
            {
              return false;
            }

          code = (code << 6) | (next & 0x3fU);
        }

      /* Overlong forms, surrogates and values past U+10FFFF. */
      static constexpr std::uint32_t kMinimum[] = { 0, 0x80U, 0x800U,
                                                    0x10000U };
      if (code < kMinimum[extra] || (code >= 0xd800U && code <= 0xdfffU) ||
          code > 0x10ffffU)
        {
          return false;
        }

      index += extra + 1;
    }

  return true;
}

} // namespace nyamp
