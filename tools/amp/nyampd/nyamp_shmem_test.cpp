/****************************************************************************
 * tools/amp/nyampd/nyamp_shmem_test.cpp
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

/****************************************************************************
 * Shared-memory region self-test for the compute domain.
 *
 * The two domains must agree on this region before any audio can move, and
 * the failure they produce when they disagree is silent corruption rather
 * than an error.  Run with no mode to write a deterministic pattern and read
 * it back; run with "check" to verify a pattern the control domain left
 * behind.  Together those prove both directions reach the same memory.
 *
 ****************************************************************************/

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nyamp_shmem_uapi.h"

namespace
{

constexpr std::uint32_t kPatternBase = 0x5a5a0000U;

/* The arena header occupies the first words and belongs to whichever side
 * claims the region, so the data pattern starts after it.
 */
constexpr std::uint32_t kFirstDataWord = 4;

std::uint32_t Expected(std::uint32_t index) { return index ^ kPatternBase; }

int Check(volatile std::uint32_t *region, std::uint32_t count)
{
  std::uint32_t errors = 0;

  for (std::uint32_t index = kFirstDataWord; index < count; ++index)
    {
      const std::uint32_t actual = region[index];
      if (actual != Expected(index))
        {
          if (errors < 4)
            {
              std::printf("mismatch at word %u: %08x != %08x\n", index, actual,
                          Expected(index));
            }

          errors++;
        }
    }

  if (errors != 0)
    {
      std::printf("nyamp shmem: FAILED, %u words differ\n", errors);
      return 1;
    }

  std::printf("nyamp shmem: ok, %u words match\n", count - kFirstDataWord);
  return 0;
}

int Run(const char *device, bool check_only)
{
  const int fd = open(device, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      std::perror("open");
      return 1;
    }

  nyamp_shmem_info info{};
  if (ioctl(fd, NYAMP_SHMEM_IOC_INFO, &info) < 0)
    {
      std::perror("ioctl");
      close(fd);
      return 1;
    }

  if (info.magic != NYAMP_SHMEM_MAGIC || info.version != NYAMP_SHMEM_VERSION)
    {
      std::fprintf(stderr, "unexpected driver identity %#x v%u\n", info.magic,
                   info.version);
      close(fd);
      return 1;
    }

  std::printf("nyamp shmem: size=%u base=%#llx\n", info.size,
              static_cast<unsigned long long>(info.base_phys));

  void *mapping =
      mmap(nullptr, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED)
    {
      std::perror("mmap");
      close(fd);
      return 1;
    }

  volatile std::uint32_t *region =
      static_cast<volatile std::uint32_t *>(mapping);
  const std::uint32_t count = info.size / sizeof(std::uint32_t);

  if (!check_only)
    {
      for (std::uint32_t index = kFirstDataWord; index < count; ++index)
        {
          region[index] = Expected(index);
        }
    }

  const int result = Check(region, count);

  munmap(mapping, info.size);
  close(fd);
  return result;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 2 || argc > 3)
    {
      std::fprintf(stderr, "usage: %s DEVICE [check]\n", argv[0]);
      return 2;
    }

  return Run(argv[1], argc == 3 && std::strcmp(argv[2], "check") == 0);
}
