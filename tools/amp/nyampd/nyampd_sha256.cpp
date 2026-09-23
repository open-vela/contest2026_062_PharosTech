/****************************************************************************
 * tools/amp/nyampd/nyampd_sha256.cpp
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

#include "nyampd_sha256.h"

#include <cstring>

namespace nyamp
{
namespace
{

constexpr std::uint32_t kRound[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t Rotate(std::uint32_t value, unsigned int bits)
{
  return (value >> bits) | (value << (32 - bits));
}

} // namespace

Sha256::Sha256()
    : state_{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 },
      length_(0), buffer_{}, buffered_(0)
{
}

void Sha256::Block(const std::uint8_t *block)
{
  std::uint32_t schedule[64];
  std::uint32_t work[8];

  for (unsigned int index = 0; index < 16; ++index)
    {
      schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                        (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                        (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                        static_cast<std::uint32_t>(block[index * 4 + 3]);
    }

  for (unsigned int index = 16; index < 64; ++index)
    {
      const std::uint32_t s0 = Rotate(schedule[index - 15], 7) ^
                               Rotate(schedule[index - 15], 18) ^
                               (schedule[index - 15] >> 3);
      const std::uint32_t s1 = Rotate(schedule[index - 2], 17) ^
                               Rotate(schedule[index - 2], 19) ^
                               (schedule[index - 2] >> 10);
      schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

  std::memcpy(work, state_, sizeof(work));
  for (unsigned int index = 0; index < 64; ++index)
    {
      const std::uint32_t s1 =
          Rotate(work[4], 6) ^ Rotate(work[4], 11) ^ Rotate(work[4], 25);
      const std::uint32_t choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
      const std::uint32_t t1 =
          work[7] + s1 + choose + kRound[index] + schedule[index];
      const std::uint32_t s0 =
          Rotate(work[0], 2) ^ Rotate(work[0], 13) ^ Rotate(work[0], 22);
      const std::uint32_t majority =
          (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
      const std::uint32_t t2 = s0 + majority;

      work[7] = work[6];
      work[6] = work[5];
      work[5] = work[4];
      work[4] = work[3] + t1;
      work[3] = work[2];
      work[2] = work[1];
      work[1] = work[0];
      work[0] = t1 + t2;
    }

  for (unsigned int index = 0; index < 8; ++index)
    {
      state_[index] += work[index];
    }
}

void Sha256::Update(const void *data, std::size_t size)
{
  const auto *bytes = static_cast<const std::uint8_t *>(data);

  length_ += size;
  if (buffered_ != 0)
    {
      const std::size_t take =
          size < sizeof(buffer_) - buffered_ ? size : sizeof(buffer_) - buffered_;
      std::memcpy(buffer_ + buffered_, bytes, take);
      buffered_ += take;
      bytes += take;
      size -= take;
      if (buffered_ < sizeof(buffer_))
        {
          return;
        }

      Block(buffer_);
      buffered_ = 0;
    }

  while (size >= sizeof(buffer_))
    {
      Block(bytes);
      bytes += sizeof(buffer_);
      size -= sizeof(buffer_);
    }

  if (size != 0)
    {
      std::memcpy(buffer_, bytes, size);
      buffered_ = size;
    }
}

void Sha256::Final(std::uint8_t digest[kSha256Size])
{
  const std::uint64_t bits = length_ * 8;
  std::uint8_t pad[72] = { 0x80 };
  std::uint8_t tail[8];

  /* Pad to 56 mod 64, then the 64-bit big-endian bit count. */
  const std::size_t pad_size =
      (buffered_ < 56 ? 56 : 120) - buffered_;
  for (unsigned int index = 0; index < 8; ++index)
    {
      tail[index] = static_cast<std::uint8_t>(bits >> (56 - index * 8));
    }

  Update(pad, pad_size);
  Update(tail, sizeof(tail));

  for (unsigned int index = 0; index < 8; ++index)
    {
      digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
      digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
      digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
      digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
    }
}

std::string Sha256Hex(const std::uint8_t digest[kSha256Size])
{
  static const char kDigits[] = "0123456789abcdef";
  std::string text(kSha256Size * 2, '0');

  for (std::size_t index = 0; index < kSha256Size; ++index)
    {
      text[index * 2] = kDigits[digest[index] >> 4];
      text[index * 2 + 1] = kDigits[digest[index] & 0x0f];
    }

  return text;
}

} // namespace nyamp
