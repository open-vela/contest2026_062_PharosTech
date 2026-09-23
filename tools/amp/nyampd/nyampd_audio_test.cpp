/****************************************************************************
 * tools/amp/nyampd/nyampd_audio_test.cpp
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

/* The pieces the speech services share: the capture ring (ownership and
 * addressing are the reason the ASR-attaches-to-KWS design is acceptable, so
 * they are pinned down here, without any service around them), leases,
 * window validation, sample conversion and the text helpers.
 */

#include "nyampd_audio.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

#define CHECK(expression)                                                 \
  do                                                                      \
    {                                                                     \
      if (!(expression))                                                  \
        {                                                                 \
          std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
                       #expression);                                      \
          return 1;                                                       \
        }                                                                 \
    }                                                                     \
  while (0)

namespace
{

using nyamp::CaptureRing;

/* Sample k of the stream has the value k, so a read proves its position. */

std::vector<float> Ramp(std::uint64_t first, std::size_t count)
{
  std::vector<float> samples(count);
  for (std::size_t index = 0; index < count; ++index)
    {
      samples[index] = static_cast<float>(first + index);
    }

  return samples;
}

int TestRingAddressing()
{
  CaptureRing ring(100, 0);
  std::vector<float> out(64);
  std::uint64_t cursor = 0;
  std::uint32_t epoch = ring.epoch();

  /* Nothing yet: a read returns empty after its timeout. */
  CaptureRing::Read read = ring.Fetch(&cursor, &epoch, out.data(), out.size(),
                                      UINT64_MAX, 1);
  CHECK(read.count == 0 && !read.gap && !read.closed && cursor == 0);

  const std::vector<float> first = Ramp(0, 60);
  ring.Append(first.data(), first.size());
  CHECK(ring.begin() == 0 && ring.end() == 60);

  read = ring.Fetch(&cursor, &epoch, out.data(), 40, UINT64_MAX, 0);
  CHECK(read.count == 40 && cursor == 40 && out[0] == 0 && out[39] == 39);

  /* The limit is honoured even though more is there. */
  read = ring.Fetch(&cursor, &epoch, out.data(), 40, 50, 0);
  CHECK(read.count == 10 && cursor == 50 && out[9] == 49);
  read = ring.Fetch(&cursor, &epoch, out.data(), 40, 50, 0);
  CHECK(read.count == 0 && cursor == 50);

  /* Wrap: 60 more samples into a ring of 100. */
  const std::vector<float> second = Ramp(60, 60);
  ring.Append(second.data(), second.size());
  CHECK(ring.begin() == 20 && ring.end() == 120);

  read = ring.Fetch(&cursor, &epoch, out.data(), 64, UINT64_MAX, 0);
  CHECK(read.count == 64 && !read.gap && out[0] == 50 && out[63] == 113);

  /* A second reader, late: it lost samples 0..19 and is told so once. */
  std::uint64_t late = 0;
  std::uint32_t late_epoch = 0;
  read = ring.Fetch(&late, &late_epoch, out.data(), 8, UINT64_MAX, 0);
  CHECK(read.gap && read.count == 8 && out[0] == 20 && late == 28);
  read = ring.Fetch(&late, &late_epoch, out.data(), 8, UINT64_MAX, 0);
  CHECK(!read.gap && out[0] == 28);

  /* One append larger than the ring keeps the newest samples only. */
  const std::vector<float> big = Ramp(120, 250);
  ring.Append(big.data(), big.size());
  CHECK(ring.end() == 370 && ring.begin() == 270);
  read = ring.Fetch(&cursor, &epoch, out.data(), 4, UINT64_MAX, 0);
  CHECK(read.gap && out[0] == 270 && cursor == 274);
  return 0;
}

int TestRingRestartAndClose()
{
  CaptureRing ring(1000, 0);
  std::vector<float> out(100);
  std::uint64_t cursor = 0;
  std::uint32_t epoch = ring.epoch();

  const std::vector<float> first = Ramp(0, 100);
  ring.Append(first.data(), first.size());
  CHECK(ring.Fetch(&cursor, &epoch, out.data(), 100, UINT64_MAX, 0).count ==
        100);

  /* Capture was paused: 500 samples never arrived. */
  ring.Restart(600);
  CHECK(ring.begin() == 600 && ring.end() == 600);
  const std::vector<float> second = Ramp(600, 50);
  ring.Append(second.data(), second.size());

  CaptureRing::Read read =
      ring.Fetch(&cursor, &epoch, out.data(), 100, UINT64_MAX, 0);
  CHECK(read.gap && read.count == 50 && out[0] == 600 && cursor == 650);

  /* A restart at the very position the reader waits at is still a gap:
   * the samples either side of it are not contiguous in time.
   */
  ring.Restart(650);
  read = ring.Fetch(&cursor, &epoch, out.data(), 100, UINT64_MAX, 0);
  CHECK(read.gap && read.count == 0 && cursor == 650);

  /* Close: what is there can still be drained, then the reader is told. */
  const std::vector<float> third = Ramp(650, 10);
  ring.Append(third.data(), third.size());
  ring.Close();
  ring.Append(third.data(), third.size()); /* Ignored. */
  CHECK(ring.end() == 660);

  read = ring.Fetch(&cursor, &epoch, out.data(), 4, UINT64_MAX, 1000);
  CHECK(read.count == 4 && !read.closed);
  read = ring.Fetch(&cursor, &epoch, out.data(), 100, UINT64_MAX, 1000);
  CHECK(read.count == 6 && read.closed && cursor == 660);
  read = ring.Fetch(&cursor, &epoch, out.data(), 100, UINT64_MAX, 1000);
  CHECK(read.count == 0 && read.closed);
  return 0;
}

/* One writer, two readers at different speeds, all positions checked. */

int TestRingThreads()
{
  constexpr std::uint64_t kTotal = 200000;
  auto ring = std::make_shared<CaptureRing>(16000, 0);
  std::atomic<int> bad{ 0 };
  std::atomic<std::uint64_t> fast_seen{ 0 };
  std::atomic<std::uint64_t> slow_gaps{ 0 };

  auto reader = [&](std::size_t step, int delay_us,
                    std::atomic<std::uint64_t> *seen,
                    std::atomic<std::uint64_t> *gaps) {
    std::vector<float> out(step);
    std::uint64_t cursor = 0;
    std::uint32_t epoch = 0;

    for (;;)
      {
        const std::uint64_t before = cursor;
        const CaptureRing::Read read =
            ring->Fetch(&cursor, &epoch, out.data(), out.size(), UINT64_MAX,
                        50);
        if (read.gap && gaps != nullptr)
          {
            ++*gaps;
          }

        const std::uint64_t first = cursor - read.count;
        if (!read.gap && first != before)
          {
            ++bad;
          }

        for (std::size_t index = 0; index < read.count; ++index)
          {
            if (out[index] != static_cast<float>(first + index))
              {
                ++bad;
              }
          }

        if (seen != nullptr)
          {
            *seen += read.count;
          }

        if (read.closed)
          {
            return;
          }

        if (delay_us > 0)
          {
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
          }
      }
  };

  std::thread fast(reader, 1600, 0, &fast_seen, nullptr);
  std::thread slow(reader, 320, 300, nullptr, &slow_gaps);

  for (std::uint64_t position = 0; position < kTotal; position += 800)
    {
      const std::vector<float> window = Ramp(position, 800);
      ring->Append(window.data(), window.size());
      if (position % 8000 == 0)
        {
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

  ring->Close();
  fast.join();
  slow.join();

  CHECK(bad.load() == 0);
  CHECK(fast_seen.load() <= kTotal);
  std::printf("  ring threads: fast reader saw %llu of %llu, slow reader "
              "had %llu gaps\n",
              static_cast<unsigned long long>(fast_seen.load()),
              static_cast<unsigned long long>(kTotal),
              static_cast<unsigned long long>(slow_gaps.load()));
  return 0;
}

int TestNotifyWakesReader()
{
  CaptureRing ring(100, 0);
  std::atomic<bool> returned{ false };

  std::thread reader([&] {
    std::vector<float> out(10);
    std::uint64_t cursor = 0;
    std::uint32_t epoch = 0;
    const auto start = std::chrono::steady_clock::now();
    ring.Fetch(&cursor, &epoch, out.data(), out.size(), UINT64_MAX, 10000);
    returned.store(std::chrono::steady_clock::now() - start <
                   std::chrono::seconds(5));
  });

  /* Give the reader time to block, then wake it with nothing changed. */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ring.Notify();
  reader.join();
  CHECK(returned.load());
  return 0;
}

int TestLeasesAndWindows()
{
  nyamp::LeaseMint mint(0x1234abcdU);
  const std::uint64_t first = mint.Next();
  const std::uint64_t second = mint.Next();

  CHECK(first != second);
  CHECK((first >> 32) == 0x1234abcdU && (second >> 32) == 0x1234abcdU);

  /* Bit 31 keeps a speech lease apart from a blob lease (a bare counter). */
  CHECK((first & 0x80000000ULL) != 0);

  const nyamp_buffer_s grant =
      nyamp::MakeGrant(&mint, 0x120000, 0x40000, NYAMP_FORMAT_F32, 0);
  CHECK(grant.magic == NYAMP_BUFFER_MAGIC && grant.length == 0);
  CHECK((grant.flags & NYAMP_BUFFER_IN_SHMEM) != 0);
  CHECK(grant.generation == 0x1234abcdU && grant.lease != second);

  nyamp_buffer_s window = grant;
  window.offset = 0x120000 + 64000;
  window.length = 64000;
  CHECK(nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));

  /* The last legal byte, and one past it. */
  window.offset = 0x120000 + 0x40000 - 64000;
  CHECK(nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));
  window.offset += 4;
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));

  window = grant;
  window.offset = 0x120000 - 4; /* In front of the grant. */
  window.length = 8;
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));

  window = grant;
  window.offset = 0xfffffff0U; /* offset + length wraps in 32 bits. */
  window.length = 0x20;
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));

  window = grant;
  window.length = 6; /* Not a whole number of float32 samples. */
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_S16));

  window = grant;
  window.length = 64;
  window.lease ^= 1; /* Somebody else's grant. */
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));

  window = grant;
  window.length = 64;
  window.generation ^= 1; /* A previous daemon's. */
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));

  window = grant;
  window.length = 64;
  window.flags = NYAMP_BUFFER_IN_SHMEM | NYAMP_BUFFER_FROM_COMPUTE;
  CHECK(!nyamp::WindowInsideGrant(grant, window, NYAMP_FORMAT_F32));

  nyamp::AtomicGate gate;
  CHECK(gate.Acquire() && !gate.Acquire());
  gate.Release();
  CHECK(gate.Acquire());
  return 0;
}

int TestSamplesAndText()
{
  const std::int16_t pcm[] = { 0, 16384, -16384, 32767, -32768 };
  std::uint8_t bytes[sizeof(pcm)];
  float out[5];

  std::memcpy(bytes, pcm, sizeof(pcm));
  nyamp::CopySamples(out, bytes, 5, NYAMP_FORMAT_S16);
  CHECK(out[0] == 0.0f && out[1] == 0.5f && out[2] == -0.5f);
  CHECK(out[3] > 0.9999f && out[3] < 1.0f && out[4] == -1.0f);

  const float floats[] = { 0.25f, -1.0f };
  nyamp::CopySamples(out, reinterpret_cast<const std::uint8_t *>(floats), 2,
                     NYAMP_FORMAT_F32);
  CHECK(out[0] == 0.25f && out[1] == -1.0f);

  const std::string text = "ab\xe4\xbd\xa0\xe5\xa5\xbd"; /* ab你好 */
  CHECK(nyamp::Utf8Prefix(text, 0, 100) == 8);
  CHECK(nyamp::Utf8Prefix(text, 0, 3) == 2); /* Not inside 你. */
  CHECK(nyamp::Utf8Prefix(text, 0, 5) == 5);
  CHECK(nyamp::Utf8Prefix(text, 0, 7) == 5);
  CHECK(nyamp::Utf8Prefix(text, 5, 3) == 3);
  CHECK(nyamp::Utf8Prefix(text, 8, 3) == 0);

  CHECK(nyamp::ValidUtf8(text));
  CHECK(nyamp::ValidUtf8(""));
  CHECK(nyamp::ValidUtf8("\xf0\x9f\x98\x80"));          /* U+1F600 */
  CHECK(!nyamp::ValidUtf8("\xe4\xbd"));                 /* Cut short. */
  CHECK(!nyamp::ValidUtf8("\xc0\xaf"));                 /* Overlong. */
  CHECK(!nyamp::ValidUtf8("\xed\xa0\x80"));             /* Surrogate. */
  CHECK(!nyamp::ValidUtf8("\xf4\x90\x80\x80"));         /* > U+10FFFF. */
  CHECK(!nyamp::ValidUtf8("\x80"));                     /* Stray tail. */
  CHECK(!nyamp::ValidUtf8(std::string("a\0b", 3)));     /* NUL. */
  return 0;
}

int TestFrameQueue()
{
  nyamp::FrameQueue queue(2);
  nyamp::Frame frame{};
  int wakes = 0;

  queue.SetWaker([&] { ++wakes; });
  frame.size = 1;
  CHECK(queue.Push(frame, true) && queue.Push(frame, true));
  CHECK(!queue.Push(frame, true)); /* Full: advisory frames are shed. */
  frame.size = 2;
  CHECK(queue.Push(frame, false)); /* A response never is. */
  CHECK(wakes == 3 && queue.size() == 3);

  nyamp::Frame out{};
  CHECK(queue.Poll(&out) && out.size == 1);
  CHECK(queue.Poll(&out) && out.size == 1);
  CHECK(queue.Poll(&out) && out.size == 2);
  CHECK(!queue.Poll(&out));
  return 0;
}

} // namespace

int main()
{
  struct
  {
    const char *name;
    int (*run)();
  } const tests[] = {
    { "ring addressing, wrap and overrun", TestRingAddressing },
    { "ring restart and close", TestRingRestartAndClose },
    { "ring with one writer and two readers", TestRingThreads },
    { "notify wakes a blocked reader", TestNotifyWakesReader },
    { "leases, grants and windows", TestLeasesAndWindows },
    { "sample conversion and text helpers", TestSamplesAndText },
    { "frame queue", TestFrameQueue },
  };

  int passed = 0;
  for (const auto &test : tests)
    {
      if (test.run() != 0)
        {
          std::fprintf(stderr, "FAILED: %s\n", test.name);
          return 1;
        }

      std::printf("ok: %s\n", test.name);
      ++passed;
    }

  std::printf("nyampd audio tests passed (%d groups)\n", passed);
  return 0;
}
