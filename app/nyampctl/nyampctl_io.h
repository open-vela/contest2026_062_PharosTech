/****************************************************************************
 * app/nyampctl/nyampctl_io.h
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

#ifndef __APP_NYAMPCTL_NYAMPCTL_IO_H
#define __APP_NYAMPCTL_NYAMPCTL_IO_H

/****************************************************************************
 * How this diagnostic reaches the compute domain.
 *
 * Standalone -- the minimal AMP profile, or a product firmware whose compute
 * service is not running -- it owns the endpoint for the length of one
 * command and reads it directly, as it always has.
 *
 * When the Nyabula Core compute service is running, that service owns the
 * read side of the endpoint for good: the endpoint has one receive queue, so
 * a second reader would steal the service's frames (including the model
 * requests the compute domain sends it) and lose its own responses to it.
 * The command then goes through a port of that service instead.
 *
 * Both paths sit behind these two helpers so the command code is written
 * once.  They are inline on purpose: the host test harness builds this
 * diagnostic from an explicit list of sources and wraps read() with a macro.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_NYABULA_CORE_COMPUTE
#  include "ny_compute.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NYAMPCTL_SEND_RETRIES  50
#define NYAMPCTL_SEND_DELAY_US 100000

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef CONFIG_NYABULA_CORE_COMPUTE
/* Non-NULL while the command runs through the compute service. */

extern struct ny_compute_port_s *g_nyampctl_port;
#endif

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nyampctl_send
 *
 * Description:
 *   Send one frame.  EAGAIN is retried: until the Linux peer has sent its
 *   first message the endpoint has no destination address to send to.
 *
 ****************************************************************************/

static inline int nyampctl_send(int fd, const uint8_t *wire, size_t size)
{
  ssize_t written = -1;
  int attempt;

#ifdef CONFIG_NYABULA_CORE_COMPUTE
  if (g_nyampctl_port != NULL)
    {
      return ny_compute_port_send(g_nyampctl_port, wire, size);
    }
#endif

  for (attempt = 0; attempt < NYAMPCTL_SEND_RETRIES; attempt++)
    {
      written = write(fd, wire, size);
      if (written >= 0 || (errno != EAGAIN && errno != EINTR))
        {
          break;
        }

      usleep(NYAMPCTL_SEND_DELAY_US);
    }

  if (written < 0)
    {
      return -errno;
    }

  return written == (ssize_t)size ? 0 : -EIO;
}

/****************************************************************************
 * Name: nyampctl_recv
 *
 * Description:
 *   Receive one frame.  Returns its size, 0 on timeout, or a negated errno.
 *
 ****************************************************************************/

static inline ssize_t nyampctl_recv(int fd, uint8_t *wire, size_t capacity,
                                    int timeout_ms)
{
  struct pollfd pollfd;
  ssize_t size;
  int ret;

#ifdef CONFIG_NYABULA_CORE_COMPUTE
  if (g_nyampctl_port != NULL)
    {
      return ny_compute_port_recv(g_nyampctl_port, wire, capacity,
                                  timeout_ms);
    }
#endif

  pollfd.fd = fd;
  pollfd.events = POLLIN;
  pollfd.revents = 0;
  do
    {
      ret = poll(&pollfd, 1, timeout_ms);
    }
  while (ret < 0 && errno == EINTR);

  if (ret <= 0)
    {
      return ret == 0 ? 0 : -errno;
    }

  size = read(fd, wire, capacity);
  if (size < 0)
    {
      return errno == EAGAIN ? 0 : -errno;
    }

  return size == 0 ? -ENODATA : size;
}

/****************************************************************************
 * Name: nyampctl_request_id
 *
 * Description:
 *   A request id for this command.  It never sets the top bit, which marks
 *   the ids the compute domain originates.
 *
 ****************************************************************************/

static inline uint64_t nyampctl_request_id(void)
{
  struct timespec now;

#ifdef CONFIG_NYABULA_CORE_COMPUTE
  if (g_nyampctl_port != NULL)
    {
      return ny_compute_request_id();
    }
#endif

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return (uint64_t)getpid();
    }

  return (((uint64_t)(uint32_t)getpid() & 0x7fffffffU) << 32) |
         (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

#endif /* __APP_NYAMPCTL_NYAMPCTL_IO_H */
