/****************************************************************************
 * app/nyabula_core/ny_web_ota.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* Receiving an image from the control panel:
 *
 *   POST   /ota/upload[?target=<id>]   body = the image; headers
 *                        Content-Length, X-Nya-Sha256 and
 *                        Authorization: Bearer <token>
 *   DELETE /ota/upload   forget a staged image
 *
 * The target says what the image is for, so that the size limit and the
 * magic that are applied are the ones of the place it is meant to go.
 * Without one it is the NuttX firmware, as it was before there were others.
 *
 * This only ever writes a temporary file.  The digest is computed on both
 * ends and compared before the file is kept, and writing the medium is a
 * separate, owner-confirmed step on the authenticated socket (update.apply).
 * A link that drops, a wrong file or a wrong digest therefore never reaches
 * the boot media.  Naming a target here grants nothing: update.apply checks
 * the file against the target again and decides on its own whether the
 * target may be written.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <crypto/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "nbootctl_part.h"
#include "ny_web.h"
#include "ny_web_auth.h"
#include "ny_web_ota.h"
#include "ny_websocket.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_OTA_TARGET        "/ota/upload"
#define NY_OTA_DIRECTORY     "/data/tmp"
#define NY_OTA_QUERY_KEY     "target"
#define NY_OTA_TARGET_ID_MAX 32

/* Kept free on the volume after the image is stored: the product database
 * and the logs share it, and a full volume breaks them first.
 */

#define NY_OTA_SPACE_MARGIN (4ul * 1024ul * 1024ul)

#define NY_OTA_CHUNK        16384
#define NY_OTA_STALL_MS     30000

/* What is still read from a request that has already been refused, so that
 * the refusal arrives as a response and not as a connection reset.
 */

#define NY_OTA_DRAIN_BYTES (4ul * 1024ul * 1024ul)
#define NY_OTA_DRAIN_MS    2000

/* The start of an image that is kept for the magic: up to the end of the
 * furthest one, the FAT signature that closes the first sector.
 */

#define NY_OTA_HEAD_SIZE 512

/* A FIT is a flattened device tree.  Its header says where the structure
 * block is; the image nodes are looked for in the first part of it, which
 * is all of it for a FIT that keeps its payloads outside the tree.
 */

#define NY_OTA_FDT_HEADER     40u
#define NY_OTA_FDT_VERSION    17u
#define NY_OTA_FDT_WINDOW     65536u
#define NY_OTA_FDT_BEGIN_NODE 1u

#define NY_OTA_LENGTH_DIGITS  12
#define NY_OTA_BEARER         "Bearer "
#define NY_OTA_BEARER_SIZE    7

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_ota_magic_s
{
  size_t offset;
  size_t size;
  const char *bytes;
};

struct ny_ota_upload_s
{
  int file;
  SHA2_CTX hash;
  const struct ny_ota_magic_s *magic;
  uint64_t expected; /* Content-Length */
  uint64_t received;
  uint8_t first[NY_OTA_HEAD_SIZE]; /* start of the image, for the magic */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_ota_reply(int fd, int code, const char *reason,
                        const char *json);
static int ny_ota_refuse(int fd, int code, const char *reason,
                         const char *error);
static void ny_ota_drain(int fd);
static void ny_ota_hex(const uint8_t *digest, char *hex);
static bool ny_ota_token_equal(const char *offered, const char *expected);
static bool ny_ota_authorized(const char *head, const char *pair_token);
static int ny_ota_length(const char *head, uint64_t *length);
static int ny_ota_hex_value(char c);
static int ny_ota_query_target(const char *query, char *id, size_t size);
static int ny_ota_read_full(int file, uint8_t *data, size_t length);
static uint32_t ny_ota_be32(const uint8_t *value);
static int ny_ota_fit_check(int file, uint64_t size, const char *const *nodes);
static int ny_ota_consume(struct ny_ota_upload_s *upload, const uint8_t *data,
                          size_t length);
static int ny_ota_receive(int fd, struct ny_ota_upload_s *upload,
                          const void *body, size_t body_length);
static int ny_ota_upload(int fd, const char *head, const char *query,
                         const void *body, size_t body_length);
static int ny_ota_remove(int fd);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_ota_lock = NXMUTEX_INITIALIZER;
static bool g_ota_claimed;

/* Indexed by enum ny_web_ota_magic_e.  None of them proves an image good;
 * each is the one cheap test that tells it from a photograph, or from the
 * image of another target, picked by mistake.
 */

static const struct ny_ota_magic_s g_ota_magics[] = {
  { 0, 0, "" },
  { 56, 4, "ARMd" },            /* arm64 Image header, "ARM\x64" */
  { 0, 4, "\xd0\x0d\xfe\xed" }, /* flattened device tree */
  { 0, 8, "K7ABCTRL" },         /* first bootctrl record */
  { 510, 2, "\x55\xaa" },       /* boot sector signature */
};

/* What N-Boot requires of the two kinds of FIT, as far as it can be seen
 * without parsing the tree: the image nodes.  They also tell the two apart,
 * which the magic they share cannot.
 */

static const char *const g_ota_nodes_amp[] = {
  "linux", "fdt", "ramdisk", "openvela", NULL,
};

static const char *const g_ota_nodes_nboot[] = {
  "atf-1", "atf-2", "atf-3", "optee", "fdt", "uboot", NULL,
};

/* The uboot partition is absent on purpose: it is the N-Boot region, and
 * the nboot target is the way to it that looks at the image first.
 */

static const struct ny_web_ota_target_s g_ota_targets[] = {
  {
      .id = "nuttx",
      .label = "openvela 固件",
      .description = "openvela（NuttX）主域固件 nuttx.bin。写入没有在运行的"
                     "槽位，从存储回读校验通过后设为下次启动的槽位。",
      .kind = NY_WEB_OTA_KIND_SLOT,
      .magic = NY_WEB_OTA_MAGIC_ARM64,
      .domain = "nuttx",
      .partition = "nuttx_a",
  },
  {
      .id = "amp",
      .label = "AMP 计算域镜像",
      .description = "AMP FIT 镜像（Linux、设备树、initramfs 与 A53 侧 "
                     "openvela）。写入非活动的 AMP 槽位，回读校验通过后激活；"
                     "AMP 槽位处于激活状态时，N-Boot 下次启动会先尝试它。",
      .kind = NY_WEB_OTA_KIND_SLOT,
      .magic = NY_WEB_OTA_MAGIC_FIT,
      .domain = "amp",
      .partition = "amp_a",
      .nodes = g_ota_nodes_amp,
  },
  {
      .id = "nboot",
      .label = "N-Boot 引导程序",
      .description = "4 MiB 的 N-Boot FIT。原位覆盖，没有备份区，不具备断电"
                     "安全：写入中途断电，或镜像本身有问题，设备将无法启动，"
                     "只能经 USB（MaskROM）恢复。",
      .kind = NY_WEB_OTA_KIND_NBOOT,
      .magic = NY_WEB_OTA_MAGIC_FIT,
      .partition = "uboot",
      .nodes = g_ota_nodes_nboot,
      .advanced = true,
  },
  {
      .id = "partition:trust",
      .label = "trust 分区",
      .description = "原样写入 trust 分区。设备无法判断内容是否正确，写错"
                     "可能导致无法启动。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_NONE,
      .partition = "trust",
      .advanced = true,
  },
  {
      .id = "partition:bootctrl",
      .label = "bootctrl 分区",
      .description = "原样覆盖 A/B 启动记录。记录与槽位内容不符时，N-Boot "
                     "会把对应槽位判为不可启动。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_BOOTCTRL,
      .partition = "bootctrl",
      .advanced = true,
  },
  {
      .id = "partition:nuttx_a",
      .label = "nuttx_a 分区（原样）",
      .description = "原样写入，不更新 bootctrl 里记录的大小与摘要：N-Boot "
                     "校验会失败并把该槽位标为不可启动。正常升级请用"
                     "「openvela 固件」。正在运行的槽位不可写。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_ARM64,
      .partition = "nuttx_a",
      .advanced = true,
  },
  {
      .id = "partition:nuttx_b",
      .label = "nuttx_b 分区（原样）",
      .description = "原样写入，不更新 bootctrl 里记录的大小与摘要：N-Boot "
                     "校验会失败并把该槽位标为不可启动。正常升级请用"
                     "「openvela 固件」。正在运行的槽位不可写。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_ARM64,
      .partition = "nuttx_b",
      .advanced = true,
  },
  {
      .id = "partition:amp_a",
      .label = "amp_a 分区（原样）",
      .description = "原样写入，不更新 bootctrl 里记录的大小与摘要：N-Boot "
                     "校验会失败并把该槽位标为不可启动。正常升级请用"
                     "「AMP 计算域镜像」。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_FIT,
      .partition = "amp_a",
      .nodes = g_ota_nodes_amp,
      .advanced = true,
  },
  {
      .id = "partition:amp_b",
      .label = "amp_b 分区（原样）",
      .description = "原样写入，不更新 bootctrl 里记录的大小与摘要：N-Boot "
                     "校验会失败并把该槽位标为不可启动。正常升级请用"
                     "「AMP 计算域镜像」。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_FIT,
      .partition = "amp_b",
      .nodes = g_ota_nodes_amp,
      .advanced = true,
  },
  {
      .id = "partition:config",
      .label = "config 分区",
      .description = "用一个 FAT 镜像原样覆盖配置分区，其中的配网与设备身份"
                     "会被替换。分区已挂载时只能强制写入，写完必须立即重启。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_FAT,
      .partition = "config",
      .mount = "/config",
      .advanced = true,
  },
  {
      .id = "partition:data",
      .label = "data 分区",
      .description = "上传的文件就暂存在这个分区上，不能用它覆盖自己。"
                     "请经 USB 刷写。",
      .kind = NY_WEB_OTA_KIND_PARTITION,
      .magic = NY_WEB_OTA_MAGIC_FAT,
      .partition = "data",
      .mount = NY_WEB_OTA_VOLUME,
      .advanced = true,
      .blocked = true,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_ota_reply
 ****************************************************************************/

static int ny_ota_reply(int fd, int code, const char *reason, const char *json)
{
  char buffer[512];
  int count = snprintf(buffer, sizeof(buffer),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %zu\r\n"
                       "Cache-Control: no-store\r\n"
                       "X-Content-Type-Options: nosniff\r\n"
                       "Connection: close\r\n\r\n%s",
                       code, reason, strlen(json), json);
  if (count < 0 || (size_t)count >= sizeof(buffer))
    {
      return -ENOBUFS;
    }

  return ny_web_http_write(fd, buffer, count);
}

/****************************************************************************
 * Name: ny_ota_refuse
 *
 * Description:
 *   Answer with an error and make sure it can be read.
 *
 *   The panel expects JSON from this prefix whatever happens, and a refusal
 *   usually comes while the browser is still sending the body.  Closing a
 *   socket with unread data resets the connection, which a browser reports
 *   as a network failure and not as the status it was just sent.
 *
 ****************************************************************************/

static int ny_ota_refuse(int fd, int code, const char *reason,
                         const char *error)
{
  char json[96];
  int ret;

  snprintf(json, sizeof(json), "{\"error\":\"%s\"}", error);
  ret = ny_ota_reply(fd, code, reason, json);
  if (ret == 0)
    {
      ny_ota_drain(fd);
    }

  return ret;
}

/****************************************************************************
 * Name: ny_ota_drain
 ****************************************************************************/

static void ny_ota_drain(int fd)
{
  uint64_t deadline = nyabula_eye_ws_now() + NY_OTA_DRAIN_MS;
  size_t drained = 0;
  char scratch[512];

  /* The response carries its own length, so the peer knows it is complete
   * without the connection being half-closed first.  Bounded both ways: a
   * peer that keeps sending must not hold a client slot for it.
   */

  while (drained < NY_OTA_DRAIN_BYTES)
    {
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd = { fd, POLLIN, 0 };
      ssize_t count;

      if (now >= deadline || poll(&pfd, 1, (int)(deadline - now)) <= 0)
        {
          break;
        }

      count = recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT);
      if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR))
        {
          break;
        }

      if (count > 0)
        {
          drained += (size_t)count;
        }
    }
}

/****************************************************************************
 * Name: ny_ota_hex
 ****************************************************************************/

static void ny_ota_hex(const uint8_t *digest, char *hex)
{
  static const char digits[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
      hex[i * 2] = digits[digest[i] >> 4];
      hex[i * 2 + 1] = digits[digest[i] & 0xf];
    }

  hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}

/****************************************************************************
 * Name: ny_ota_token_equal
 *
 * Description:
 *   Compare two tokens of NY_WEB_AUTH_TOKEN_SIZE characters in a time that
 *   does not depend on where they first differ.
 *
 ****************************************************************************/

static bool ny_ota_token_equal(const char *offered, const char *expected)
{
  unsigned int different = 0;
  size_t i;

  for (i = 0; i < NY_WEB_AUTH_TOKEN_SIZE; i++)
    {
      different |= (unsigned char)offered[i] ^ (unsigned char)expected[i];
    }

  return different == 0;
}

/****************************************************************************
 * Name: ny_ota_authorized
 *
 * Description:
 *   The same two credentials that open the socket: the pair token shown on
 *   the eyes, or the session token a correct password was exchanged for.
 *
 *   A page from another site cannot forge this request from the owner's
 *   browser.  Authorization is not a header a cross-origin request may set
 *   without a preflight, and nothing here answers one.
 *
 ****************************************************************************/

static bool ny_ota_authorized(const char *head, const char *pair_token)
{
  char session[NY_WEB_AUTH_TOKEN_SIZE + 1];
  size_t length = 0;
  const char *value = ny_web_http_header(head, "Authorization", &length);
  bool valid;

  while (value != NULL && length > 0 &&
         (value[length - 1] == ' ' || value[length - 1] == '\t'))
    {
      length--;
    }

  if (value == NULL || pair_token == NULL ||
      strlen(pair_token) != NY_WEB_AUTH_TOKEN_SIZE ||
      length != NY_OTA_BEARER_SIZE + NY_WEB_AUTH_TOKEN_SIZE ||
      strncasecmp(value, NY_OTA_BEARER, NY_OTA_BEARER_SIZE) != 0)
    {
      return false;
    }

  value += NY_OTA_BEARER_SIZE;
  valid = ny_ota_token_equal(value, pair_token);
  if (ny_web_auth_session_token(pair_token, session) == 0)
    {
      /* Both comparisons always run, so the time taken does not say which
       * kind of token was offered.
       */

      bool match = ny_ota_token_equal(value, session);
      valid = valid || match;
    }

  memset(session, 0, sizeof(session));
  return valid;
}

/****************************************************************************
 * Name: ny_ota_length
 *
 * Description:
 *   Parse Content-Length.  -ENOENT when it is absent, -EINVAL when it is
 *   not a number, -EFBIG when it cannot possibly fit.
 *
 ****************************************************************************/

static int ny_ota_length(const char *head, uint64_t *length)
{
  size_t size = 0;
  const char *value = ny_web_http_header(head, "Content-Length", &size);
  uint64_t total = 0;
  size_t i;

  if (value == NULL)
    {
      return -ENOENT;
    }

  while (size > 0 && (value[size - 1] == ' ' || value[size - 1] == '\t'))
    {
      size--;
    }

  if (size == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < size; i++)
    {
      if (value[i] < '0' || value[i] > '9')
        {
          return -EINVAL;
        }

      /* Stop while the sum still fits; the caller's limit is far below. */

      if (i >= NY_OTA_LENGTH_DIGITS)
        {
          return -EFBIG;
        }

      total = total * 10 + (uint64_t)(value[i] - '0');
    }

  *length = total;
  return 0;
}

/****************************************************************************
 * Name: ny_ota_hex_value
 ****************************************************************************/

static int ny_ota_hex_value(char c)
{
  if (c >= '0' && c <= '9')
    {
      return c - '0';
    }

  if (c >= 'a' && c <= 'f')
    {
      return c - 'a' + 10;
    }

  if (c >= 'A' && c <= 'F')
    {
      return c - 'A' + 10;
    }

  return -1;
}

/****************************************************************************
 * Name: ny_ota_query_target
 *
 * Description:
 *   The value of "target" in the query of the request target, decoded.
 *   query points at the '?', or at whatever ended the path when there is no
 *   query, and the request line ends at the next space.
 *
 *   The default when the parameter is absent; -EINVAL when it is there and
 *   cannot be a target id.  A browser escapes the ':' of "partition:trust",
 *   so the escapes have to be understood, but what comes out is only ever
 *   compared with the ids in the table.
 *
 ****************************************************************************/

static int ny_ota_query_target(const char *query, char *id, size_t size)
{
  static const char key[] = NY_OTA_QUERY_KEY "=";
  const char *at = query;
  size_t used = 0;

  strlcpy(id, NY_WEB_OTA_TARGET_DEFAULT, size);
  if (*at != '?')
    {
      return 0;
    }

  at++;
  while (strncmp(at, key, sizeof(key) - 1) != 0)
    {
      /* Not this parameter: on to the one after the next '&'. */

      at += strcspn(at, "& #\r\n");
      if (*at != '&')
        {
          return 0;
        }

      at++;
    }

  at += sizeof(key) - 1;
  while (*at != '\0' && strchr("& #\r\n", *at) == NULL)
    {
      int value = (unsigned char)*at;

      if (*at == '%')
        {
          int high = ny_ota_hex_value(at[1]);
          int low = high < 0 ? -1 : ny_ota_hex_value(at[2]);

          if (low < 0)
            {
              return -EINVAL;
            }

          value = (high << 4) | low;
          at += 2;
        }

      /* Ids are plain ASCII words; nothing else needs to get any further. */

      if (value <= ' ' || value >= 0x7f || used + 1 >= size)
        {
          return -EINVAL;
        }

      id[used++] = (char)value;
      at++;
    }

  id[used] = '\0';
  return used > 0 ? 0 : -EINVAL;
}

/****************************************************************************
 * Name: ny_ota_read_full
 *
 * Description:
 *   Read exactly length bytes from the current position.  -ENOEXEC when the
 *   file ends first: whatever announced that many bytes was not true.
 *
 ****************************************************************************/

static int ny_ota_read_full(int file, uint8_t *data, size_t length)
{
  size_t done = 0;

  while (done < length)
    {
      ssize_t count = read(file, data + done, length - done);
      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          return count < 0 ? -errno : -ENOEXEC;
        }

      done += (size_t)count;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_ota_be32
 ****************************************************************************/

static uint32_t ny_ota_be32(const uint8_t *value)
{
  return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
         (uint32_t)value[2] << 8 | value[3];
}

/****************************************************************************
 * Name: ny_ota_fit_check
 *
 * Description:
 *   Whether a FIT has every image node a target requires.
 *
 *   This is not the validation N-Boot does before it writes itself over
 *   USB: hashes, load addresses and the configuration are not looked at.
 *   It is the part of it that fits here, and it catches the mistake that
 *   the shared magic lets through: the FIT of one target offered to the
 *   other.  A node starts with the begin-node token, on a four byte
 *   boundary, followed by its name and a terminator.
 *
 ****************************************************************************/

static int ny_ota_fit_check(int file, uint64_t size, const char *const *nodes)
{
  uint8_t header[NY_OTA_FDT_HEADER];
  uint8_t *block;
  uint32_t total;
  uint32_t offset;
  uint32_t length;
  size_t i;
  int ret;

  if (lseek(file, 0, SEEK_SET) < 0)
    {
      return -errno;
    }

  ret = ny_ota_read_full(file, header, sizeof(header));
  if (ret < 0)
    {
      return ret;
    }

  total = ny_ota_be32(header + 4);
  offset = ny_ota_be32(header + 8);
  length = ny_ota_be32(header + 36);
  if (ny_ota_be32(header + 20) < NY_OTA_FDT_VERSION ||
      total < NY_OTA_FDT_HEADER || total > size ||
      offset < NY_OTA_FDT_HEADER || offset >= total || length > total - offset)
    {
      return -ENOEXEC;
    }

  if (length > NY_OTA_FDT_WINDOW)
    {
      length = NY_OTA_FDT_WINDOW;
    }

  block = malloc(length > 0 ? length : 1);
  if (block == NULL)
    {
      return -ENOMEM;
    }

  if (lseek(file, (off_t)offset, SEEK_SET) < 0)
    {
      ret = -errno;
    }
  else
    {
      ret = ny_ota_read_full(file, block, length);
    }

  for (i = 0; ret == 0 && nodes[i] != NULL; i++)
    {
      size_t name = strlen(nodes[i]) + 1; /* with its terminator */
      bool found = false;
      uint32_t at;

      for (at = 0; !found && at + 4 + name <= length; at += 4)
        {
          found = ny_ota_be32(block + at) == NY_OTA_FDT_BEGIN_NODE &&
                  memcmp(block + at + 4, nodes[i], name) == 0;
        }

      if (!found)
        {
          ret = -ENOEXEC;
        }
    }

  free(block);
  return ret;
}

/****************************************************************************
 * Name: ny_ota_consume
 *
 * Description:
 *   Take the next bytes of the body: check the magic of the target as soon
 *   as the bytes that hold it are in, then store and hash.
 *
 ****************************************************************************/

static int ny_ota_consume(struct ny_ota_upload_s *upload, const uint8_t *data,
                          size_t length)
{
  size_t magic_end = upload->magic->offset + upload->magic->size;
  size_t written = 0;

  if (upload->received < magic_end)
    {
      size_t have = (size_t)upload->received;
      size_t take = magic_end - have;
      if (take > length)
        {
          take = length;
        }

      memcpy(upload->first + have, data, take);
      if (have + take == magic_end &&
          memcmp(upload->first + upload->magic->offset, upload->magic->bytes,
                 upload->magic->size) != 0)
        {
          return -ENOEXEC;
        }
    }

  while (written < length)
    {
      ssize_t count = write(upload->file, data + written, length - written);
      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          return count < 0 ? -errno : -EIO;
        }

      written += (size_t)count;
    }

  sha256update(&upload->hash, data, length);
  upload->received += length;
  return 0;
}

/****************************************************************************
 * Name: ny_ota_receive
 *
 * Description:
 *   Read exactly the announced number of body bytes.
 *
 *   The first of them may not be on the socket any more: the read that
 *   completed the request head takes whatever had arrived, and a client is
 *   free to send head and body together.  Those bytes are handed in as
 *   body/body_length and are consumed first, in order.
 *
 ****************************************************************************/

static int ny_ota_receive(int fd, struct ny_ota_upload_s *upload,
                          const void *body, size_t body_length)
{
  uint64_t deadline;
  uint8_t *chunk;
  int ret = 0;

  /* More than was announced is not part of this request. */

  if ((uint64_t)body_length > upload->expected)
    {
      body_length = (size_t)upload->expected;
    }

  if (body_length > 0)
    {
      ret = ny_ota_consume(upload, body, body_length);
      if (ret < 0)
        {
          return ret;
        }
    }

  chunk = malloc(NY_OTA_CHUNK);
  if (chunk == NULL)
    {
      return -ENOMEM;
    }

  deadline = nyabula_eye_ws_now() + NY_OTA_STALL_MS;
  while (ret == 0 && upload->received < upload->expected)
    {
      uint64_t left = upload->expected - upload->received;
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd = { fd, POLLIN, 0 };
      ssize_t count;
      size_t want;
      int ready;

      if (now >= deadline)
        {
          ret = -ETIMEDOUT;
          break;
        }

      ready = poll(&pfd, 1, (int)(deadline - now));
      if (ready < 0 && errno == EINTR)
        {
          continue;
        }

      if (ready <= 0)
        {
          ret = ready == 0 ? -ETIMEDOUT : -errno;
          break;
        }

      want = left < NY_OTA_CHUNK ? (size_t)left : NY_OTA_CHUNK;
      count = recv(fd, chunk, want, MSG_DONTWAIT);
      if (count < 0 && (errno == EAGAIN || errno == EINTR))
        {
          continue;
        }

      if (count <= 0)
        {
          ret = count == 0 ? -ECONNRESET : -errno;
          break;
        }

      ret = ny_ota_consume(upload, chunk, (size_t)count);

      /* The limit is on a link that has gone quiet, not on a large image
       * over a slow one: progress buys more time.
       */

      deadline = nyabula_eye_ws_now() + NY_OTA_STALL_MS;
    }

  free(chunk);
  return ret;
}

/****************************************************************************
 * Name: ny_ota_upload
 ****************************************************************************/

static int ny_ota_upload(int fd, const char *head, const char *query,
                         const void *body, size_t body_length)
{
  const struct ny_web_ota_target_s *target;
  const struct ny_ota_magic_s *magic;
  struct ny_ota_upload_s *upload;
  uint8_t digest[SHA256_DIGEST_LENGTH];
  char wanted[NY_WEB_OTA_SHA256_HEX + 1];
  char actual[NY_WEB_OTA_SHA256_HEX + 1];
  char id[NY_OTA_TARGET_ID_MAX];
  char json[224];
  const char *value;
  uint64_t expected = 0;
  uint64_t capacity;
  size_t length = 0;
  size_t i;
  int ret;

  /* What the image is for decides every limit below. */

  target = ny_ota_query_target(query, id, sizeof(id)) < 0
               ? NULL
               : ny_web_ota_target_find(id, strlen(id));
  if (target == NULL)
    {
      return ny_ota_refuse(fd, 400, "Bad Request", "ETARGET");
    }

  capacity = ny_web_ota_target_capacity(target);
  if (target->blocked || capacity == 0)
    {
      /* Refused now and not after the transfer: the answer will not be
       * any different then.
       */

      return ny_ota_refuse(fd, 403, "Forbidden", "EBLOCKED");
    }

  magic = &g_ota_magics[target->magic];
  ret = ny_ota_length(head, &expected);
  if (ret == -ENOENT)
    {
      return ny_ota_refuse(fd, 411, "Length Required", "ELENGTH");
    }

  if (ret == -EFBIG || (ret == 0 && expected > capacity))
    {
      return ny_ota_refuse(fd, 413, "Content Too Large", "ETOOLARGE");
    }

  if (ret < 0)
    {
      return ny_ota_refuse(fd, 400, "Bad Request", "EINVAL");
    }

  if (expected == 0 || expected < magic->offset + magic->size)
    {
      return ny_ota_refuse(fd, 415, "Unsupported Media Type", "ENOTIMAGE");
    }

  value = ny_web_http_header(head, "X-Nya-Sha256", &length);
  while (value != NULL && length > 0 &&
         (value[length - 1] == ' ' || value[length - 1] == '\t'))
    {
      length--;
    }

  if (value == NULL || !ny_web_ota_digest_ok(value, length))
    {
      return ny_ota_refuse(fd, 400, "Bad Request", "EDIGEST");
    }

  for (i = 0; i < NY_WEB_OTA_SHA256_HEX; i++)
    {
      wanted[i] =
          value[i] >= 'A' && value[i] <= 'F' ? value[i] - 'A' + 'a' : value[i];
    }

  wanted[NY_WEB_OTA_SHA256_HEX] = '\0';

  if (ny_web_ota_claim() < 0)
    {
      return ny_ota_refuse(fd, 409, "Conflict", "EBUSY");
    }

  /* From here the claim is held; every path below releases it. */

  upload = calloc(1, sizeof(*upload));
  if (upload == NULL)
    {
      ny_web_ota_release();
      return ny_ota_refuse(fd, 500, "Internal Server Error", "ENOMEM");
    }

  upload->file = -1;
  upload->magic = magic;
  upload->expected = expected;

  /* An earlier image is of no further use, and its blocks may be exactly
   * the ones this image needs.  The partition is not the only bound: an
   * AMP slot is larger than what the volume usually has left.
   */

  mkdir(NY_OTA_DIRECTORY, 0755);
  unlink(NY_WEB_OTA_FILE);
  if (ny_web_ota_staging_space() < expected)
    {
      ret = ny_ota_refuse(fd, 413, "Content Too Large", "ENOSPACE");
      goto out;
    }

  upload->file =
      open(NY_WEB_OTA_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (upload->file < 0)
    {
      ret = ny_ota_refuse(fd, 500, "Internal Server Error", "ESTORAGE");
      goto out;
    }

  /* A client that asked may be holding the body back until it is told to
   * go on.  Browsers never ask; command line tools do for large bodies.
   */

  value = ny_web_http_header(head, "Expect", &length);
  if (value != NULL && length >= 12 &&
      strncasecmp(value, "100-continue", 12) == 0)
    {
      static const char go_on[] = "HTTP/1.1 100 Continue\r\n\r\n";
      ret = ny_web_http_write(fd, go_on, sizeof(go_on) - 1);
      if (ret < 0)
        {
          goto discard;
        }
    }

  sha256init(&upload->hash);
  ret = ny_ota_receive(fd, upload, body, body_length);
  if (ret == 0 && fsync(upload->file) < 0)
    {
      ret = -errno;
    }

  if (close(upload->file) < 0 && ret == 0)
    {
      ret = -errno;
    }

  upload->file = -1;
  if (ret < 0)
    {
      int status = ret;
      unlink(NY_WEB_OTA_FILE);
      if (status == -ENOEXEC)
        {
          ret = ny_ota_refuse(fd, 415, "Unsupported Media Type", "ENOTIMAGE");
        }
      else if (status == -ENOSPC)
        {
          ret = ny_ota_refuse(fd, 507, "Insufficient Storage", "ENOSPACE");
        }
      else if (status == -ETIMEDOUT)
        {
          ret = ny_ota_refuse(fd, 408, "Request Timeout", "ETIMEDOUT");
        }
      else if (status == -ECONNRESET || status == -EPIPE ||
               status == -ENOTCONN)
        {
          /* Nobody is left to tell. */

          ret = status;
        }
      else
        {
          ret = ny_ota_refuse(fd, 500, "Internal Server Error", "ESTORAGE");
        }

      goto out;
    }

  sha256final(digest, &upload->hash);
  ny_ota_hex(digest, actual);
  if (memcmp(actual, wanted, NY_WEB_OTA_SHA256_HEX) != 0)
    {
      /* The bytes that arrived are not the bytes that were sent.  Nothing
       * has been written outside the temporary file, and it goes too.
       */

      unlink(NY_WEB_OTA_FILE);
      snprintf(json, sizeof(json),
               "{\"error\":\"EDIGEST\",\"received\":%llu,\"sha256\":\"%s\"}",
               (unsigned long long)upload->received, actual);
      ret = ny_ota_reply(fd, 422, "Unprocessable Content", json);
      goto out;
    }

  /* The magic was seen on the way in.  What needs the whole file, the image
   * nodes of a FIT, is looked at now, so that the wrong FIT is refused here
   * and not after the owner has confirmed writing it.
   */

  ret = ny_web_ota_file_check(target, NY_WEB_OTA_FILE);
  if (ret < 0)
    {
      /* The body has been read to its end: there is nothing to drain. */

      unlink(NY_WEB_OTA_FILE);
      ret = ret == -ENOEXEC ? ny_ota_reply(fd, 415, "Unsupported Media Type",
                                           "{\"error\":\"ENOTIMAGE\"}")
                            : ny_ota_reply(fd, 500, "Internal Server Error",
                                           "{\"error\":\"ESTORAGE\"}");
      goto out;
    }

  /* The id comes from the table, not from the request: nothing in it needs
   * escaping.
   */

  fprintf(stderr, "nyabula_web: image for %s staged, %llu bytes\n", target->id,
          (unsigned long long)upload->received);
  snprintf(json, sizeof(json),
           "{\"received\":%llu,\"sha256\":\"%s\",\"target\":\"%s\"}",
           (unsigned long long)upload->received, actual, target->id);
  ret = ny_ota_reply(fd, 200, "OK", json);
  goto out;

discard:
  close(upload->file);
  upload->file = -1;
  unlink(NY_WEB_OTA_FILE);

out:
  if (upload->file >= 0)
    {
      close(upload->file);
    }

  free(upload);
  ny_web_ota_release();
  return ret;
}

/****************************************************************************
 * Name: ny_ota_remove
 ****************************************************************************/

static int ny_ota_remove(int fd)
{
  bool removed;

  if (ny_web_ota_claim() < 0)
    {
      return ny_ota_refuse(fd, 409, "Conflict", "EBUSY");
    }

  removed = unlink(NY_WEB_OTA_FILE) == 0;
  ny_web_ota_release();
  return ny_ota_reply(fd, 200, "OK",
                      removed ? "{\"removed\":true}" : "{\"removed\":false}");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_web_ota_claims
 ****************************************************************************/

bool ny_web_ota_claims(const char *head)
{
  const char *target = strchr(head, ' ');
  if (target == NULL)
    {
      return false;
    }

  target++;
  if (strncmp(target, "/ota", 4) != 0)
    {
      return false;
    }

  /* "/ota" itself and everything below it, but not "/otazoo". */

  return target[4] == '/' || target[4] == ' ' || target[4] == '?';
}

/****************************************************************************
 * Name: ny_web_ota_serve
 ****************************************************************************/

int ny_web_ota_serve(int fd, const char *head, const void *body,
                     size_t body_length, const char *pair_token)
{
  const char *target = strchr(head, ' ');
  size_t method_length;
  size_t target_length;

  if (target == NULL)
    {
      return ny_ota_refuse(fd, 400, "Bad Request", "EINVAL");
    }

  method_length = (size_t)(target - head);
  target++;
  target_length = strcspn(target, " ?#");

  /* Anything else under the prefix does not exist, and says so in the form
   * the caller parses.  The page fallback of the file server would answer
   * 200 with HTML, which a client waiting for JSON cannot tell from success.
   */

  if (target_length != sizeof(NY_OTA_TARGET) - 1 ||
      strncmp(target, NY_OTA_TARGET, target_length) != 0)
    {
      return ny_ota_refuse(fd, 404, "Not Found", "ENOTFOUND");
    }

  /* Who is asking comes before everything about the image, so that a
   * stranger learns nothing from the order of the refusals.
   */

  if (!ny_ota_authorized(head, pair_token))
    {
      return ny_ota_refuse(fd, 401, "Unauthorized", "EAUTH");
    }

  if (method_length == 4 && strncmp(head, "POST", 4) == 0)
    {
      return ny_ota_upload(fd, head, target + target_length, body,
                           body_length);
    }

  if (method_length == 6 && strncmp(head, "DELETE", 6) == 0)
    {
      return ny_ota_remove(fd);
    }

  return ny_ota_refuse(fd, 405, "Method Not Allowed", "EMETHOD");
}

/****************************************************************************
 * Name: ny_web_ota_claim
 ****************************************************************************/

int ny_web_ota_claim(void)
{
  int ret = nxmutex_lock(&g_ota_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_ota_claimed)
    {
      ret = -EBUSY;
    }
  else
    {
      g_ota_claimed = true;
    }

  nxmutex_unlock(&g_ota_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_web_ota_release
 ****************************************************************************/

void ny_web_ota_release(void)
{
  nxmutex_lock(&g_ota_lock);
  g_ota_claimed = false;
  nxmutex_unlock(&g_ota_lock);
}

/****************************************************************************
 * Name: ny_web_ota_target_at
 ****************************************************************************/

const struct ny_web_ota_target_s *ny_web_ota_target_at(size_t position)
{
  return position < sizeof(g_ota_targets) / sizeof(g_ota_targets[0])
             ? &g_ota_targets[position]
             : NULL;
}

/****************************************************************************
 * Name: ny_web_ota_target_find
 ****************************************************************************/

const struct ny_web_ota_target_s *ny_web_ota_target_find(const char *id,
                                                         size_t length)
{
  size_t i;

  if (id == NULL)
    {
      return NULL;
    }

  for (i = 0; i < sizeof(g_ota_targets) / sizeof(g_ota_targets[0]); i++)
    {
      if (strlen(g_ota_targets[i].id) == length &&
          memcmp(g_ota_targets[i].id, id, length) == 0)
        {
          return &g_ota_targets[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: ny_web_ota_target_capacity
 ****************************************************************************/

uint64_t ny_web_ota_target_capacity(const struct ny_web_ota_target_s *target)
{
  uint64_t bytes = 0;

  /* The layout table is nbootctl's; a second copy here would drift. */

  if (target == NULL || nbootctl_part_size(target->partition, &bytes) < 0)
    {
      return 0;
    }

  return bytes;
}

/****************************************************************************
 * Name: ny_web_ota_staging_space
 ****************************************************************************/

uint64_t ny_web_ota_staging_space(void)
{
  struct statfs volume;
  uint64_t space;

  if (statfs(NY_WEB_OTA_VOLUME, &volume) < 0)
    {
      return 0;
    }

  space = (uint64_t)volume.f_bavail * (uint64_t)volume.f_bsize;
  return space > NY_OTA_SPACE_MARGIN ? space - NY_OTA_SPACE_MARGIN : 0;
}

/****************************************************************************
 * Name: ny_web_ota_file_check
 ****************************************************************************/

int ny_web_ota_file_check(const struct ny_web_ota_target_s *target,
                          const char *path)
{
  const struct ny_ota_magic_s *magic = &g_ota_magics[target->magic];
  uint64_t capacity = ny_web_ota_target_capacity(target);
  uint8_t *first;
  struct stat status;
  uint64_t size;
  int file;
  int ret = 0;

  file = open(path, O_RDONLY | O_CLOEXEC);
  if (file < 0)
    {
      return -errno;
    }

  if (fstat(file, &status) < 0)
    {
      ret = -errno;
      close(file);
      return ret;
    }

  size = status.st_size > 0 ? (uint64_t)status.st_size : 0;
  if (capacity == 0 || size > capacity)
    {
      close(file);
      return -EFBIG;
    }

  if (size == 0 || size < magic->offset + magic->size)
    {
      close(file);
      return -ENOEXEC;
    }

  if (magic->size > 0)
    {
      /* Not on the stack: the worker that calls this has a small one. */

      first = malloc(NY_OTA_HEAD_SIZE);
      if (first == NULL)
        {
          close(file);
          return -ENOMEM;
        }

      ret = ny_ota_read_full(file, first, magic->offset + magic->size);
      if (ret == 0 &&
          memcmp(first + magic->offset, magic->bytes, magic->size) != 0)
        {
          ret = -ENOEXEC;
        }

      free(first);
    }

  if (ret == 0 && target->nodes != NULL)
    {
      ret = ny_ota_fit_check(file, size, target->nodes);
    }

  close(file);
  return ret;
}

/****************************************************************************
 * Name: ny_web_ota_digest_ok
 ****************************************************************************/

bool ny_web_ota_digest_ok(const char *text, size_t length)
{
  size_t i;

  if (text == NULL || length != NY_WEB_OTA_SHA256_HEX)
    {
      return false;
    }

  for (i = 0; i < length; i++)
    {
      bool hex = (text[i] >= '0' && text[i] <= '9') ||
                 (text[i] >= 'a' && text[i] <= 'f') ||
                 (text[i] >= 'A' && text[i] <= 'F');
      if (!hex)
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: ny_web_ota_file_digest
 ****************************************************************************/

int ny_web_ota_file_digest(const char *path, char *hex, uint64_t *size)
{
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA2_CTX hash;
  uint64_t total = 0;
  uint8_t *chunk;
  int file;
  int ret = 0;

  file = open(path, O_RDONLY | O_CLOEXEC);
  if (file < 0)
    {
      return -errno;
    }

  chunk = malloc(NY_OTA_CHUNK);
  if (chunk == NULL)
    {
      close(file);
      return -ENOMEM;
    }

  sha256init(&hash);
  for (;;)
    {
      ssize_t count = read(file, chunk, NY_OTA_CHUNK);
      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          ret = count < 0 ? -errno : 0;
          break;
        }

      sha256update(&hash, chunk, (size_t)count);
      total += (uint64_t)count;
    }

  free(chunk);
  close(file);
  if (ret < 0)
    {
      return ret;
    }

  sha256final(digest, &hash);
  ny_ota_hex(digest, hex);
  if (size != NULL)
    {
      *size = total;
    }

  return 0;
}
