/****************************************************************************
 * app/nyampctl/nyampctl_main.c
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

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/rpmsg/rpmsg.h>

#include "nyamp_protocol.h"
#include "nyampctl.h"

#define NYAMPCTL_CTRL_PATH       "/dev/rpmsg/linux"
#define NYAMPCTL_ENDPOINT_NAME   "rpmsg-raw"
#define NYAMPCTL_ENDPOINT_PATH   "/dev/rpmsg-rpmsg-raw"
#define NYAMPCTL_OPEN_RETRIES    50
#define NYAMPCTL_OPEN_DELAY_US   100000
#define NYAMPCTL_RESPONSE_MS     5000
#define NYAMPCTL_HEALTH_OPCODE   1
#define NYAMPCTL_INFO_OPCODE     2
#define NYAMPCTL_HEALTH_RESPONSE 12

static uint32_t nyampctl_get_le32(const uint8_t *source);
static int nyampctl_open_endpoint(void);

static uint32_t nyampctl_get_le32(const uint8_t *source)
{
  uint32_t value = 0;
  unsigned int index;

  for (index = 0; index < 4; index++)
    {
      value |= (uint32_t)source[index] << (index * 8);
    }

  return value;
}

static int nyampctl_open_endpoint(void)
{
  int retry;
  int fd;

  for (retry = 0; retry < NYAMPCTL_OPEN_RETRIES; retry++)
    {
      fd = open(NYAMPCTL_ENDPOINT_PATH, O_RDWR | O_NONBLOCK);
      if (fd >= 0)
        {
          return fd;
        }

      usleep(NYAMPCTL_OPEN_DELAY_US);
    }

  return -errno;
}

int nyampctl_query(int fd, uint16_t opcode)
{
  struct nyamp_header_s request = {
    .service = NYAMP_SERVICE_HEALTH,
    .opcode = opcode,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = 0,
    .deadline_ms = 0,
    .generation = 0,
    .payload_size = 0,
  };
  struct nyamp_header_s response;
  struct pollfd pollfd;
  uint8_t wire[NYAMP_RPMSG_MTU];
  ssize_t size;
  int attempt;
  int ret;
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return -errno;
    }

  request.request_id =
      ((uint64_t)(uint32_t)getpid() << 32) |
      (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);

  ret = nyamp_header_encode(wire, sizeof(wire), &request);
  if (ret != NYAMP_OK)
    {
      return ret;
    }

  /* The Linux peer sends a ready event to publish its dynamic address. */

  for (attempt = 0; attempt < NYAMPCTL_OPEN_RETRIES; attempt++)
    {
      size = write(fd, wire, NYAMP_WIRE_HEADER_SIZE);
      if (size >= 0 || errno != EAGAIN)
        {
          break;
        }

      usleep(NYAMPCTL_OPEN_DELAY_US);
    }

  if (size != (ssize_t)NYAMP_WIRE_HEADER_SIZE)
    {
      return size < 0 ? -errno : -EIO;
    }

  pollfd.fd = fd;
  pollfd.events = POLLIN;
  for (attempt = 0; attempt < 2; attempt++)
    {
      pollfd.revents = 0;
      ret = poll(&pollfd, 1, NYAMPCTL_RESPONSE_MS);
      if (ret <= 0)
        {
          return ret == 0 ? -ETIMEDOUT : -errno;
        }

      size = read(fd, wire, sizeof(wire));
      if (size < 0)
        {
          return -errno;
        }

      ret = nyamp_header_decode(&response, wire, (size_t)size);
      if (ret != NYAMP_OK || response.flags != NYAMP_FLAG_EVENT ||
          response.service != NYAMP_SERVICE_HEALTH ||
          response.opcode != NYAMP_HEALTH_READY || response.request_id != 1 ||
          response.generation == 0 || response.payload_size != 0 ||
          size != NYAMP_WIRE_HEADER_SIZE)
        {
          break;
        }
    }

  if (ret != NYAMP_OK || response.request_id != request.request_id ||
      response.service != request.service || response.opcode != opcode ||
      (response.flags & ~NYAMP_FLAG_ERROR) != NYAMP_FLAG_RESPONSE ||
      response.payload_size < 4 || response.generation == 0 ||
      size != (ssize_t)(NYAMP_WIRE_HEADER_SIZE + response.payload_size))
    {
      return -EPROTO;
    }

  if (nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE) != 0)
    {
      return -EREMOTEIO;
    }

  if (response.flags != NYAMP_FLAG_RESPONSE)
    {
      return -EPROTO;
    }

  if (opcode == NYAMPCTL_INFO_OPCODE)
    {
      size_t index;

      if (response.payload_size <= 4)
        {
          return -EPROTO;
        }

      for (index = NYAMP_WIRE_HEADER_SIZE + 4; index < (size_t)size; index++)
        {
          if (wire[index] != '\n' &&
              (wire[index] < 0x20 || wire[index] > 0x7e))
            {
              return -EPROTO;
            }
        }

      printf("nyamp Linux info: generation=%" PRIu32 "\n",
             response.generation);
      fwrite(wire + NYAMP_WIRE_HEADER_SIZE + 4, 1, response.payload_size - 4,
             stdout);
      return 0;
    }

  if (response.payload_size != NYAMPCTL_HEALTH_RESPONSE ||
      nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE + 4) !=
          response.generation ||
      (nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE + 8) & 1) == 0)
    {
      return -EPROTO;
    }

  printf("nyamp health ok: generation=%" PRIu32 " capabilities=0x%08" PRIx32
         "\n",
         nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE + 4),
         nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE + 8));
  return 0;
}

int main(int argc, char *argv[])
{
  struct rpmsg_endpoint_info info;
  int ctrl;
  int fd;
  int ret;
  uint16_t opcode;

  if (argc < 2)
    {
      fprintf(stderr,
              "usage: %s health|info\n"
              "       %s llm load <model-directory-or-file>\n"
              "       %s llm unload\n"
              "       %s llm generate <token-ids-file> [max-new-tokens]\n"
              "       %s llm generate --inline <id,id,...> [max-new-tokens]\n",
              argv[0], argv[0], argv[0], argv[0], argv[0]);
      return 2;
    }

  if (strcmp(argv[1], "health") != 0 && strcmp(argv[1], "info") != 0 &&
      strcmp(argv[1], "llm") != 0)
    {
      fprintf(stderr, "nyampctl: unknown command: %s\n", argv[1]);
      return 2;
    }

  if (strcmp(argv[1], "llm") == 0 && argc < 3)
    {
      fprintf(stderr, "nyampctl: llm needs load|unload|generate\n");
      return 2;
    }

  opcode = strcmp(argv[1], "info") == 0 ? NYAMPCTL_INFO_OPCODE
                                        : NYAMPCTL_HEALTH_OPCODE;

  ctrl = open(NYAMPCTL_CTRL_PATH, O_RDWR);
  if (ctrl < 0)
    {
      fprintf(stderr, "nyampctl: open %s failed: %d\n", NYAMPCTL_CTRL_PATH,
              errno);
      return 1;
    }

  memset(&info, 0, sizeof(info));
  strlcpy(info.name, NYAMPCTL_ENDPOINT_NAME, sizeof(info.name));
  info.src = RPMSG_ADDR_ANY;
  info.dst = RPMSG_ADDR_ANY;

  ret = ioctl(ctrl, RPMSG_CREATE_DEV_IOCTL, (unsigned long)&info);
  if (ret < 0 && errno != EEXIST)
    {
      fprintf(stderr, "nyampctl: create endpoint failed: %d\n", errno);
      close(ctrl);
      return 1;
    }

  fd = nyampctl_open_endpoint();
  if (fd < 0)
    {
      fprintf(stderr, "nyampctl: endpoint did not bind: %d\n", -fd);
      ret = fd;
    }
  else
    {
      if (strcmp(argv[1], "llm") == 0)
        {
          if (strcmp(argv[2], "load") == 0 && argc == 4)
            {
              ret = nyampctl_llm_load(fd, argv[3]);
            }
          else if (strcmp(argv[2], "unload") == 0 && argc == 3)
            {
              ret = nyampctl_llm_unload(fd);
            }
          else if (strcmp(argv[2], "generate") == 0 && argc >= 4)
            {
              bool inline_ids = strcmp(argv[3], "--inline") == 0;
              const char *source = inline_ids ? argv[4] : argv[3];
              int value_index = inline_ids ? 5 : 4;
              uint32_t max_new_tokens =
                  argc > value_index
                      ? (uint32_t)strtoul(argv[value_index], NULL, 10)
                      : 128;

              if (inline_ids && argc < 5)
                {
                  fprintf(stderr, "nyampctl: --inline needs token ids\n");
                  ret = -EINVAL;
                }
              else
                {
                  ret = nyampctl_llm_generate(fd, source, max_new_tokens,
                                              inline_ids);
                }
            }
          else
            {
              fprintf(stderr, "nyampctl: bad llm arguments\n");
              ret = -EINVAL;
            }
        }
      else
        {
          ret = nyampctl_query(fd, opcode);
        }

      close(fd);
    }

  close(ctrl);

  if (ret < 0)
    {
      fprintf(stderr, "nyampctl: command failed: %d\n", ret);
      return 1;
    }

  return 0;
}
