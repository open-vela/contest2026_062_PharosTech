/****************************************************************************
 * tools/amp/nyampd/nyampd_main.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "nyampd_core.h"
#include "nyampd_llm.h"

#include "nyamp_backends.h"
#include "nyamp_protocol.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/random.h>
#include <unistd.h>

namespace
{

/* Milliseconds tick used to drain queued LLM events while idle.  The daemon
 * must stay responsive to the peer, so the loop never blocks indefinitely on
 * the transport when a generation is running.
 */

constexpr int kIdlePollMs = 20;

using BackendFactory = nyamp::LlmService::BackendFactory;

/****************************************************************************
 * Name: SelectBackendFactory
 *
 * Description:
 *   Pick the LLM backend this build can actually provide.  A build without the
 *   external RKLLM runtime returns an empty factory, which the service turns
 *   into an unsupported answer for every LLM request.  Never substitute a test
 *   backend here: a daemon that answers generate requests without inference is
 *   worse than one that refuses them.
 *
 ****************************************************************************/

BackendFactory SelectBackendFactory()
{
#ifdef NYAMP_WITH_RKLLM
  return &nyamp::models::CreateRkllmBackend;
#endif
  return BackendFactory();
}

std::uint64_t SharedCounterMilliseconds()
{
#if defined(__aarch64__)
  std::uint64_t counter;
  std::uint64_t frequency;

  asm volatile("mrs %0, cntvct_el0" : "=r"(counter));
  asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  return frequency == 0 ? 0 : counter / (frequency / 1000);
#else
  return 0;
#endif
}

std::uint32_t NewGeneration()
{
  std::uint32_t generation = 0;

  if (getrandom(&generation, sizeof(generation), GRND_NONBLOCK) !=
          sizeof(generation) ||
      generation == 0)
    {
      const std::uint64_t now = SharedCounterMilliseconds();
      generation = static_cast<std::uint32_t>(now) ^
                   static_cast<std::uint32_t>(now >> 32) ^
                   (static_cast<std::uint32_t>(getpid()) * 0x9e3779b9U);
      if (generation == 0)
        {
          generation = 1;
        }
    }

  return generation;
}

int FindDevice(char *path, std::size_t path_size)
{
  DIR *directory = opendir("/sys/class/rpmsg");

  if (directory == nullptr)
    {
      return -errno;
    }

  for (dirent *entry = readdir(directory); entry != nullptr;
       entry = readdir(directory))
    {
      char name_path[PATH_MAX];
      char name[64];
      std::FILE *file;

      if (std::strncmp(entry->d_name, "rpmsg", 5) != 0 ||
          std::strstr(entry->d_name, "ctrl") != nullptr)
        {
          continue;
        }

      std::snprintf(name_path, sizeof(name_path), "/sys/class/rpmsg/%s/name",
                    entry->d_name);
      file = std::fopen(name_path, "r");
      if (file == nullptr)
        {
          continue;
        }

      const char *read = std::fgets(name, sizeof(name), file);
      std::fclose(file);
      if (read == nullptr || std::strncmp(name, "rpmsg-raw", 9) != 0)
        {
          continue;
        }

      std::snprintf(path, path_size, "/dev/%s", entry->d_name);
      closedir(directory);
      return 0;
    }

  closedir(directory);
  return -ENOENT;
}

std::size_t CpuInfo(char *output, std::size_t capacity)
{
  int size = std::snprintf(output, capacity, "online=%ld\n",
                           sysconf(_SC_NPROCESSORS_ONLN));
  if (size < 0 || static_cast<std::size_t>(size) >= capacity)
    {
      return 0;
    }

  std::size_t used = static_cast<std::size_t>(size);
  std::FILE *file = std::fopen("/proc/cpuinfo", "r");
  if (file == nullptr)
    {
      return used;
    }

  char line[512];
  unsigned int cpu = 0;
  unsigned int part;
  while (std::fgets(line, sizeof(line), file) != nullptr)
    {
      if (std::sscanf(line, "processor : %u", &cpu) == 1)
        {
          continue;
        }

      if (std::sscanf(line, "CPU part : %x", &part) == 1)
        {
          size = std::snprintf(output + used, capacity - used,
                               "cpu%u part=0x%x\n", cpu, part);
          if (size < 0 || static_cast<std::size_t>(size) >= capacity - used)
            {
              std::fclose(file);
              return 0;
            }

          used += static_cast<std::size_t>(size);
        }
    }

  std::fclose(file);
  return used;
}

/****************************************************************************
 * Name: WriteAll
 *
 * Description:
 *   Write one complete frame.  RPMsg message boundaries are one write each, so
 *   a short write is a transport failure rather than a reason to retry a
 *   partial message.
 *
 ****************************************************************************/

bool WriteAll(int fd, const std::uint8_t *data, std::size_t size)
{
  ssize_t written;

  do
    {
      written = write(fd, data, size);
    }
  while (written < 0 && errno == EINTR);

  return written == static_cast<ssize_t>(size);
}

/****************************************************************************
 * Name: DrainEvents
 *
 * Description:
 *   Flush every queued LLM event to the peer.  Returns false when the
 *   transport failed, which the caller treats as a fatal error so PID1 can
 *   restart the daemon with a fresh generation.
 *
 ****************************************************************************/

bool DrainEvents(int fd, nyamp::LlmService &llm)
{
  for (;;)
    {
      nyamp::LlmFrame frame;
      if (!llm.Poll(&frame))
        {
          return true;
        }

      if (!WriteAll(fd, frame.data, frame.size))
        {
          std::fprintf(stderr, "nyampd: event write failed: %s\n",
                       std::strerror(errno));
          return false;
        }
    }
}

int Run(const char *requested_device)
{
  std::uint8_t request[NYAMP_RPMSG_MTU];
  std::uint8_t response[NYAMP_RPMSG_MTU];
  const std::uint32_t generation = NewGeneration();
  char discovered[PATH_MAX];
  const char *device = requested_device;
  int fd;

  if (device == nullptr)
    {
      const int result = FindDevice(discovered, sizeof(discovered));
      if (result < 0)
        {
          std::fprintf(stderr, "nyampd: rpmsg-raw endpoint not found: %d\n",
                       -result);
          return 1;
        }

      device = discovered;
    }

  fd = open(device, O_RDWR | O_CLOEXEC | O_NONBLOCK);

  if (fd < 0)
    {
      std::fprintf(stderr, "nyampd: open %s failed: %s\n", device,
                   std::strerror(errno));
      return 1;
    }

  std::fprintf(stderr, "nyampd: generation=%u device=%s\n", generation,
               device);

  /* The service is always constructed so a client can learn from the health
   * capability mask whether an LLM backend was compiled in.  Without the
   * external RKLLM runtime the factory is absent and every LLM opcode is
   * answered unsupported rather than reported as working.
   */

  nyamp::LlmService llm(generation, SharedCounterMilliseconds,
                        SelectBackendFactory());

  // Standard Linux RPMsg does not announce dynamically assigned addresses.
  // The first message lets the remote learn our endpoint address.
  nyamp_header_s ready{};
  ready.service = NYAMP_SERVICE_HEALTH;
  ready.opcode = NYAMP_HEALTH_READY;
  ready.flags = NYAMP_FLAG_EVENT;
  ready.request_id = 1;
  ready.generation = generation;
  nyamp_header_encode(response, sizeof(response), &ready);
  if (!WriteAll(fd, response, NYAMP_WIRE_HEADER_SIZE))
    {
      std::fprintf(stderr, "nyampd: endpoint announcement failed\n");
      close(fd);
      return 1;
    }

  for (;;)
    {
      struct pollfd pollfd = { fd, POLLIN, 0 };
      const int ready_count = poll(&pollfd, 1, kIdlePollMs);

      if (ready_count < 0 && errno != EINTR)
        {
          std::fprintf(stderr, "nyampd: transport poll failed: %s\n",
                       std::strerror(errno));
          close(fd);
          return 1;
        }

      if (ready_count > 0 && (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        {
          std::fprintf(stderr, "nyampd: transport disconnected\n");
          close(fd);
          return 1;
        }

      if (ready_count > 0 && (pollfd.revents & POLLIN) != 0)
        {
          const ssize_t received = read(fd, request, sizeof(request));

          if (received < 0 && (errno == EINTR || errno == EAGAIN))
            {
              if (!DrainEvents(fd, llm))
                {
                  close(fd);
                  return 1;
                }

              continue;
            }

          if (received <= 0)
            {
              std::fprintf(stderr, "nyampd: transport disconnected: %s\n",
                           received == 0 ? "end of file"
                                         : std::strerror(errno));
              close(fd);
              return 1;
            }

          char info[NYAMP_INLINE_MAX - 4];
          std::size_t info_size = 0;
          std::size_t response_size = 0;
          nyamp_header_s header;
          if (nyamp_header_decode(&header, request,
                                  static_cast<std::size_t>(received)) ==
                  NYAMP_OK &&
              header.service == NYAMP_SERVICE_HEALTH &&
              header.opcode == nyamp::kInfoQuery)
            {
              info_size = CpuInfo(info, sizeof(info));
            }

          const int result =
              nyamp::Dispatch(request, static_cast<std::size_t>(received),
                              SharedCounterMilliseconds(), generation,
                              response, sizeof(response), &response_size,
                              std::string_view(info, info_size), &llm);
          if (result == NYAMP_OK && !WriteAll(fd, response, response_size))
            {
              std::fprintf(stderr, "nyampd: transport write failed: %s\n",
                           std::strerror(errno));
              close(fd);
              return 1;
            }

          if (result != NYAMP_OK)
            {
              std::fprintf(stderr, "nyampd: malformed request: %d\n", result);
            }
        }

      if (!DrainEvents(fd, llm))
        {
          close(fd);
          return 1;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
  if (argc > 2)
    {
      std::fprintf(stderr, "usage: %s [rpmsg-device]\n", argv[0]);
      return 2;
    }

  return Run(argc == 2 ? argv[1] : nullptr);
}
