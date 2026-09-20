/****************************************************************************
 * tools/amp/nyampd/nyampd_sha256.h
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

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_SHA256_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace nyamp
{

constexpr std::size_t kSha256Size = 32;

/* Incremental SHA-256 (FIPS 180-4).
 *
 * The daemon is linked statically into an initramfs that carries no crypto
 * library, and the digest is only an integrity check against the control
 * domain's copy of the same file, so a small portable implementation is the
 * whole requirement.  It is incremental because a model is pulled through a
 * 1 MiB window and never exists in memory as one buffer.
 */

class Sha256
{
public:
  Sha256();

  void Update(const void *data, std::size_t size);
  void Final(std::uint8_t digest[kSha256Size]);

private:
  void Block(const std::uint8_t *block);

  std::uint32_t state_[8];
  std::uint64_t length_;
  std::uint8_t buffer_[64];
  std::size_t buffered_;
};

std::string Sha256Hex(const std::uint8_t digest[kSha256Size]);

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_SHA256_H */
