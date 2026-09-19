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

/* Receiving a firmware image from the control panel:
 *
 *   POST   /ota/upload   body = the image; headers Content-Length,
 *                        X-Nya-Sha256 and Authorization: Bearer <token>
 *   DELETE /ota/upload   forget a staged image
 *
 * This only ever writes a temporary file.  The digest is computed on both
 * ends and compared before the file is kept, and writing a slot is a
 * separate, owner-confirmed step on the authenticated socket (update.apply).
 * A link that drops, a wrong file or a wrong digest therefore never reaches
 * the boot media.
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

#include "ny_web.h"
#include "ny_web_auth.h"
#include "ny_web_ota.h"
#include "ny_websocket.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_OTA_TARGET    "/ota/upload"
#define NY_OTA_DIRECTORY "/data/tmp"
#define NY_OTA_VOLUME    "/data"

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

/* An arm64 Image header carries "ARM\x64" at offset 56.  It is the one cheap
 * test that tells a NuttX image from a photograph picked by mistake.
 */

#define NY_OTA_MAGIC_OFFSET  56
#define NY_OTA_MAGIC         "ARMd"
#define NY_OTA_MAGIC_SIZE    4
#define NY_OTA_MAGIC_END     (NY_OTA_MAGIC_OFFSET + NY_OTA_MAGIC_SIZE)

#define NY_OTA_LENGTH_DIGITS 12
#define NY_OTA_BEARER        "Bearer "
#define NY_OTA_BEARER_SIZE   7

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_ota_upload_s
{
  int file;
  SHA2_CTX hash;
  uint64_t expected; /* Content-Length */
  uint64_t received;
  uint8_t first[NY_OTA_MAGIC_END]; /* start of the image, for the magic */
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
static int ny_ota_consume(struct ny_ota_upload_s *upload, const uint8_t *data,
                          size_t length);
static int ny_ota_receive(int fd, struct ny_ota_upload_s *upload,
                          const void *body, size_t body_length);
static int ny_ota_upload(int fd, const char *head, const void *body,
                         size_t body_length);
static int ny_ota_remove(int fd);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_ota_lock = NXMUTEX_INITIALIZER;
static bool g_ota_claimed;

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
 * Name: ny_ota_consume
 *
 * Description:
 *   Take the next bytes of the body: check the image magic as soon as the
 *   bytes that hold it are in, then store and hash.
 *
 ****************************************************************************/

static int ny_ota_consume(struct ny_ota_upload_s *upload, const uint8_t *data,
                          size_t length)
{
  size_t written = 0;

  if (upload->received < NY_OTA_MAGIC_END)
    {
      size_t have = (size_t)upload->received;
      size_t take = NY_OTA_MAGIC_END - have;
      if (take > length)
        {
          take = length;
        }

      memcpy(upload->first + have, data, take);
      if (have + take == NY_OTA_MAGIC_END &&
          memcmp(upload->first + NY_OTA_MAGIC_OFFSET, NY_OTA_MAGIC,
                 NY_OTA_MAGIC_SIZE) != 0)
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

static int ny_ota_upload(int fd, const char *head, const void *body,
                         size_t body_length)
{
  struct ny_ota_upload_s *upload;
  struct statfs volume;
  uint8_t digest[SHA256_DIGEST_LENGTH];
  char wanted[NY_WEB_OTA_SHA256_HEX + 1];
  char actual[NY_WEB_OTA_SHA256_HEX + 1];
  char json[160];
  const char *value;
  uint64_t expected = 0;
  uint64_t space;
  size_t length = 0;
  size_t i;
  int ret;

  ret = ny_ota_length(head, &expected);
  if (ret == -ENOENT)
    {
      return ny_ota_refuse(fd, 411, "Length Required", "ELENGTH");
    }

  if (ret == -EFBIG || (ret == 0 && expected > NY_WEB_OTA_MAX_BYTES))
    {
      return ny_ota_refuse(fd, 413, "Content Too Large", "ETOOLARGE");
    }

  if (ret < 0)
    {
      return ny_ota_refuse(fd, 400, "Bad Request", "EINVAL");
    }

  if (expected < NY_OTA_MAGIC_END)
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
  upload->expected = expected;

  /* An earlier image is of no further use, and its blocks may be exactly
   * the ones this image needs.
   */

  mkdir(NY_OTA_DIRECTORY, 0755);
  unlink(NY_WEB_OTA_FILE);
  if (statfs(NY_OTA_VOLUME, &volume) < 0)
    {
      ret = ny_ota_refuse(fd, 500, "Internal Server Error", "ESTORAGE");
      goto out;
    }

  space = (uint64_t)volume.f_bavail * (uint64_t)volume.f_bsize;
  if (space < NY_OTA_SPACE_MARGIN || space - NY_OTA_SPACE_MARGIN < expected)
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

  fprintf(stderr, "nyabula_web: firmware image staged, %llu bytes\n",
          (unsigned long long)upload->received);
  snprintf(json, sizeof(json), "{\"received\":%llu,\"sha256\":\"%s\"}",
           (unsigned long long)upload->received, actual);
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
      return ny_ota_upload(fd, head, body, body_length);
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
