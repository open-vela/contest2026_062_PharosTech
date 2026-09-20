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
#include "nyampctl_io.h"

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
static int nyampctl_llm_command(int fd, int argc, char *argv[]);
static int nyampctl_blob_command(int fd, int argc, char *argv[]);
static int nyampctl_attach(int *fd);
static void nyampctl_detach(int fd);

#ifdef CONFIG_NYABULA_CORE_COMPUTE
struct ny_compute_port_s *g_nyampctl_port;
#endif

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

/****************************************************************************
 * Name: nyampctl_open_endpoint
 *
 * Description:
 *   Create the endpoint if needed and open it.  With the Core compute
 *   library in the image this is that library's code; the copy below only
 *   exists so the minimal AMP profile can build this diagnostic alone.
 *
 ****************************************************************************/

static int nyampctl_open_endpoint(void)
{
#ifdef CONFIG_NYABULA_CORE_COMPUTE
  return ny_compute_endpoint_open(O_RDWR | O_NONBLOCK);
#else
  struct rpmsg_endpoint_info info;
  int retry;
  int ctrl;
  int fd = -1;
  int ret;

  ctrl = open(NYAMPCTL_CTRL_PATH, O_RDWR);
  if (ctrl < 0)
    {
      fprintf(stderr, "nyampctl: open %s failed: %d\n", NYAMPCTL_CTRL_PATH,
              errno);
      return -errno;
    }

  memset(&info, 0, sizeof(info));
  strlcpy(info.name, NYAMPCTL_ENDPOINT_NAME, sizeof(info.name));
  info.src = RPMSG_ADDR_ANY;
  info.dst = RPMSG_ADDR_ANY;

  ret = ioctl(ctrl, RPMSG_CREATE_DEV_IOCTL, (unsigned long)&info);
  if (ret < 0 && errno != EEXIST)
    {
      ret = -errno;
      fprintf(stderr, "nyampctl: create endpoint failed: %d\n", -ret);
      close(ctrl);
      return ret;
    }

  for (retry = 0; retry < NYAMPCTL_OPEN_RETRIES; retry++)
    {
      fd = open(NYAMPCTL_ENDPOINT_PATH, O_RDWR | O_NONBLOCK);
      if (fd >= 0)
        {
          break;
        }

      usleep(NYAMPCTL_OPEN_DELAY_US);
    }

  ret = fd < 0 ? -errno : fd;
  close(ctrl);
  return ret;
#endif
}

/****************************************************************************
 * Name: nyampctl_attach / nyampctl_detach
 *
 * Description:
 *   Reach the compute domain for one command.  While the Core compute
 *   service runs it owns the read side of the endpoint, so the command goes
 *   through one of its ports and *fd stays -1.  Otherwise the endpoint is
 *   opened directly.  Returns 0 or a negated errno.
 *
 ****************************************************************************/

static int nyampctl_attach(int *fd)
{
  *fd = -1;

#ifdef CONFIG_NYABULA_CORE_COMPUTE
  if (ny_compute_running())
    {
      int ret = ny_compute_port_open(&g_nyampctl_port);

      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: compute service port failed: %d\n", -ret);
          return ret;
        }

      return 0;
    }
#endif

  *fd = nyampctl_open_endpoint();
  return *fd < 0 ? *fd : 0;
}

static void nyampctl_detach(int fd)
{
#ifdef CONFIG_NYABULA_CORE_COMPUTE
  if (g_nyampctl_port != NULL)
    {
      ny_compute_port_close(g_nyampctl_port);
      g_nyampctl_port = NULL;
      return;
    }
#endif

  if (fd >= 0)
    {
      close(fd);
    }
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
  uint8_t wire[NYAMP_RPMSG_MTU];
  ssize_t size;
  int attempt;
  int ret;

  request.request_id = nyampctl_request_id();

  ret = nyamp_header_encode(wire, sizeof(wire), &request);
  if (ret != NYAMP_OK)
    {
      return ret;
    }

  /* The Linux peer sends a ready event to publish its dynamic address; until
   * it has, the send is retried.
   */

  ret = nyampctl_send(fd, wire, NYAMP_WIRE_HEADER_SIZE);
  if (ret < 0)
    {
      return ret;
    }

  /* Standalone, the first frame read may be that ready event rather than the
   * response.  Through the compute service it never is: the service consumes
   * the event itself, and the second pass is simply not taken.
   */

  for (attempt = 0; attempt < 2; attempt++)
    {
      size = nyampctl_recv(fd, wire, sizeof(wire), NYAMPCTL_RESPONSE_MS);
      if (size <= 0)
        {
          return size == 0 ? -ETIMEDOUT : (int)size;
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

/****************************************************************************
 * Name: nyampctl_llm_command / nyampctl_blob_command
 *
 * Description:
 *   Argument handling for the two command families that have sub-verbs.
 *
 ****************************************************************************/

static int nyampctl_llm_command(int fd, int argc, char *argv[])
{
  if (strcmp(argv[2], "load") == 0 && argc == 4)
    {
      return nyampctl_llm_load(fd, argv[3]);
    }

  if (strcmp(argv[2], "unload") == 0 && argc == 3)
    {
      return nyampctl_llm_unload(fd);
    }

  if (strcmp(argv[2], "generate") == 0 && argc >= 4)
    {
      bool inline_ids = strcmp(argv[3], "--inline") == 0;
      const char *source = inline_ids ? argv[4] : argv[3];
      int value_index = inline_ids ? 5 : 4;
      uint32_t max_new_tokens =
          argc > value_index ? (uint32_t)strtoul(argv[value_index], NULL, 10)
                             : 128;

      if (inline_ids && argc < 5)
        {
          fprintf(stderr, "nyampctl: --inline needs token ids\n");
          return -EINVAL;
        }

      return nyampctl_llm_generate(fd, source, max_new_tokens, inline_ids);
    }

  if (strcmp(argv[2], "chat") == 0 && (argc == 4 || argc == 5))
    {
      return nyampctl_llm_chat(
          fd, argv[3], argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 0);
    }

  fprintf(stderr, "nyampctl: bad llm arguments\n");
  return -EINVAL;
}

static int nyampctl_blob_command(int fd, int argc, char *argv[])
{
  if (strcmp(argv[2], "bench") == 0 && argc <= 5)
    {
      return nyampctl_blob_bench(
          fd, argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 200,
          argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 0);
    }

  if (strcmp(argv[2], "pull") == 0 && argc == 4)
    {
      return nyampctl_blob_pull(fd, argv[3]);
    }

  fprintf(stderr, "nyampctl: blob needs bench [rounds] [window-bytes] or "
                  "pull <name>\n");
  return -EINVAL;
}

int main(int argc, char *argv[])
{
  int fd = -1;
  int ret;

  if (argc < 2)
    {
      fprintf(stderr,
              "usage: %s health|info|status\n"
              "       %s llm load <model-name-or-absolute-path>\n"
              "       %s llm unload\n"
              "       %s llm generate <token-ids-file> [max-new-tokens]\n"
              "       %s llm generate --inline <id,id,...> [max-new-tokens]\n"
              "       %s llm chat <request.json> [max-new-tokens]\n"
              "       %s blob bench [rounds] [window-bytes]\n"
              "       %s blob pull <name-under-/data/models>\n"
              "       %s shmem test [keep]\n",
              argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
              argv[0], argv[0]);
      return 2;
    }

  if (strcmp(argv[1], "health") != 0 && strcmp(argv[1], "info") != 0 &&
      strcmp(argv[1], "status") != 0 && strcmp(argv[1], "llm") != 0 &&
      strcmp(argv[1], "blob") != 0 && strcmp(argv[1], "shmem") != 0)
    {
      fprintf(stderr, "nyampctl: unknown command: %s\n", argv[1]);
      return 2;
    }

  if ((strcmp(argv[1], "llm") == 0 || strcmp(argv[1], "blob") == 0) &&
      argc < 3)
    {
      fprintf(stderr, "nyampctl: %s needs a sub-command\n", argv[1]);
      return 2;
    }

  /* The shared region is reached directly, not through the RPMsg endpoint,
   * so it needs no control channel and no peer to be up.
   */

  if (strcmp(argv[1], "shmem") == 0)
    {
      if (argc < 3 || strcmp(argv[2], "test") != 0)
        {
          fprintf(stderr, "nyampctl: shmem needs test [keep]\n");
          return 2;
        }

      return nyampctl_shmem_test(argc > 3 && strcmp(argv[3], "keep") == 0) < 0
                 ? 1
                 : 0;
    }

  /* The control domain's own view of the link; nothing is sent. */

  if (strcmp(argv[1], "status") == 0)
    {
      return nyampctl_status() < 0 ? 1 : 0;
    }

  ret = nyampctl_attach(&fd);
  if (ret < 0)
    {
      fprintf(stderr, "nyampctl: endpoint did not bind: %d\n", -ret);
    }
  else
    {
      if (strcmp(argv[1], "llm") == 0)
        {
          ret = nyampctl_llm_command(fd, argc, argv);
        }
      else if (strcmp(argv[1], "blob") == 0)
        {
          ret = nyampctl_blob_command(fd, argc, argv);
        }
      else
        {
          ret = nyampctl_query(fd, strcmp(argv[1], "info") == 0
                                       ? NYAMPCTL_INFO_OPCODE
                                       : NYAMPCTL_HEALTH_OPCODE);
        }

      nyampctl_detach(fd);
    }

  if (ret < 0)
    {
      fprintf(stderr, "nyampctl: command failed: %d\n", ret);
      return 1;
    }

  return 0;
}
