/****************************************************************************
 * app/nyabula_core/ny_web_models.c
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

/* Model files, brought to the device by its owner from the control panel:
 *
 *   PUT /models/upload?path=<kind>/<relative name>
 *                        body = one piece of the file; headers
 *                        Content-Length, Content-Range: bytes s-e/total,
 *                        X-Nya-Sha256 (of the WHOLE file) and
 *                        Authorization: Bearer <token>
 *   GET /models/upload?path=...
 *                        how much of an unfinished upload is here
 *
 *   models.list     any role   what is under /data/models and what is left
 *   models.status   any role   the upload and the verification in progress
 *   models.delete   owner      {"path": ...}; the file and what belongs to it
 *   models.verify   owner      {"path": ...}; hash a file in the background
 *
 * A language model is most of a gigabyte and the link carries about two
 * megabytes a second, so a transfer takes minutes and has to survive being
 * interrupted.  The client sends the file in pieces of at most 8 MiB and
 * says where each one goes.  They are appended to "<name>.part", and
 * "<name>.part.json" remembers which file that is (total, sha256) and how
 * much of it has been stored.  A piece is only taken when it starts exactly
 * where the last one ended; otherwise the answer is 409 with the offset the
 * device has, and the client goes on from there.  After a reload of the
 * page the GET says the same.
 *
 * What was stored of a piece that broke off is kept: it arrived in order
 * over TCP, and the digest of the whole file is what decides in the end.
 * When the last byte is in, the file is read back and hashed, and only a
 * match is renamed to its real name.  "<name>.sha256" then holds the
 * digest, so that listing the directory never has to hash gigabytes.
 *
 * Nothing here knows what a model is.  The kinds are directory names, the
 * rest of a path is letters, digits, dot, dash and underscore, and the
 * panel knows which names the inference services look for.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <crypto/sha2.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
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

#include "ny_product.h"
#include "ny_web.h"
#include "ny_web_auth.h"
#include "ny_web_models.h"
#include "ny_websocket.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_MODELS_TARGET        "/models/upload"
#define NY_MODELS_PREFIX        "/models"
#define NY_MODELS_QUERY_KEY     "path"

/* A path is "<kind>/<up to three more names>".  A name leaves room for the
 * longest suffix that is put behind it (".part.json") within the 64
 * characters the volume stores of a long file name.
 */

#define NY_MODELS_LEVELS        3
#define NY_MODELS_SEGMENT_MAX   48
#define NY_MODELS_SUFFIX_ROOM   10
#define NY_MODELS_FILE_MAX      (sizeof(NY_WEB_MODELS_ROOT) + 1 + \
                                 NY_WEB_MODELS_PATH_MAX + \
                                 NY_MODELS_SUFFIX_ROOM)

#define NY_MODELS_PART          ".part"
#define NY_MODELS_SIDECAR       ".part.json"
#define NY_MODELS_DIGEST        ".sha256"

/* How far below the root listing looks: the kind and three more names. */

#define NY_MODELS_WALK_DEPTH    4

/* A reply has to fit one 32 KiB message of the socket, envelope included. */

#define NY_MODELS_LIST_BUDGET   24000u
#define NY_MODELS_ITEM_COST     160u

/* The largest piece a client may send at once.  It is not buffered: this
 * bounds how much a broken piece can cost and how long one request holds a
 * client slot.
 */

#define NY_MODELS_PIECE_MAX     (8ull * 1024ull * 1024ull)

/* FAT stores the size of a file in 32 bits. */

#define NY_MODELS_FILE_LIMIT    0xffffffffull

/* Kept free on the volume once a model is stored: the product database, the
 * logs and a staged firmware image share it.
 */

#define NY_MODELS_SPACE_MARGIN  (64ull * 1024ull * 1024ull)

#define NY_MODELS_RECV          16384
#define NY_MODELS_HASH_READ     65536
#define NY_MODELS_STALL_MS      60000
#define NY_MODELS_WORKER_STACK  16384

/* What is still read from a request that has already been refused, so that
 * the refusal arrives as a response and not as a connection reset: one
 * whole piece at most.
 */

#define NY_MODELS_DRAIN_BYTES   (NY_MODELS_PIECE_MAX + 65536ull)
#define NY_MODELS_DRAIN_MS      5000

#define NY_MODELS_SHA256_HEX    64
#define NY_MODELS_NUMBER_DIGITS 15
#define NY_MODELS_SIDECAR_SIZE  256
#define NY_MODELS_BEARER        "Bearer "
#define NY_MODELS_BEARER_SIZE   7

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_models_paths_s
{
  char target[NY_MODELS_FILE_MAX];   /* the file under its real name */
  char part[NY_MODELS_FILE_MAX];     /* what has arrived of it so far */
  char sidecar[NY_MODELS_FILE_MAX];  /* which file the part belongs to */
  char digest[NY_MODELS_FILE_MAX];   /* sha256 of the finished file */
};

struct ny_models_sidecar_s
{
  uint64_t total;
  uint64_t received;
  char sha256[NY_MODELS_SHA256_HEX + 1];
};

/* One request to store a piece.  Allocated: four paths are more than the
 * stack of a client thread should carry next to everything else.
 */

struct ny_models_job_s
{
  struct ny_models_paths_s paths;
  struct ny_models_sidecar_s sidecar;
  char path[NY_WEB_MODELS_PATH_MAX + 1];
  char wanted[NY_MODELS_SHA256_HEX + 1];
  uint64_t start;
  uint64_t total;
  uint64_t length;                   /* Content-Length */
  uint64_t received;                 /* stored of the whole file */
  bool probe;                        /* "bytes * / total": no piece */
};

enum ny_models_verify_e
{
  NY_MODELS_VERIFY_IDLE = 0,
  NY_MODELS_VERIFY_RUNNING,
  NY_MODELS_VERIFY_DONE,
  NY_MODELS_VERIFY_FAILED,
};

/* One verification at a time, guarded by the claim, so the job is a single
 * static and not an allocation a failed thread start could leak.
 */

struct ny_models_verify_s
{
  enum ny_models_verify_e state;
  const char *reason;                /* short word for the panel, or "" */
  char path[NY_WEB_MODELS_PATH_MAX + 1];
  char sha256[NY_MODELS_SHA256_HEX + 1];
  char expected[NY_MODELS_SHA256_HEX + 1];  /* from the sidecar, or "" */
  uint64_t done;
  uint64_t total;
};

/* What models.status says about the upload that holds the claim. */

struct ny_models_progress_s
{
  bool active;
  bool hashing;
  char path[NY_WEB_MODELS_PATH_MAX + 1];
  uint64_t received;
  uint64_t total;
  uint64_t hashed;
};

struct ny_models_walk_s
{
  cJSON *items;
  size_t budget;
  bool truncated;

  /* The longest name an upload can leave behind is a part, and its record
   * is looked for by putting one more suffix behind that.
   */

  char path[NY_MODELS_FILE_MAX + NY_MODELS_SUFFIX_ROOM];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_models_reply(int fd, int code, const char *reason,
                           const char *json);
static int ny_models_refuse_json(int fd, int code, const char *reason,
                                 const char *json);
static int ny_models_refuse(int fd, int code, const char *reason,
                            const char *error);
static void ny_models_drain(int fd);
static void ny_models_hex(const uint8_t *digest, char *hex);
static bool ny_models_digest_ok(const char *text, size_t length);
static bool ny_models_token_equal(const char *offered, const char *expected);
static bool ny_models_authorized(const char *head, const char *pair_token);
static int ny_models_hex_value(char c);
static bool ny_models_ends_with(const char *name, size_t length,
                                const char *suffix);
static bool ny_models_path_ok(const char *path);
static int ny_models_query_path(const char *query, char *path, size_t size);
static void ny_models_paths(const char *path,
                            struct ny_models_paths_s *paths);
static int ny_models_parents(char *file);
static void ny_models_prune(char *file);
static int ny_models_number(const char **at, uint64_t *value);
static int ny_models_length(const char *head, uint64_t *length);
static int ny_models_range(const char *head, struct ny_models_job_s *job);
static int ny_models_sidecar_load(const char *file,
                                  struct ny_models_sidecar_s *sidecar);
static int ny_models_sidecar_save(const char *file,
                                  const struct ny_models_sidecar_s *sidecar);
static int ny_models_text_save(const char *file, const char *text);
static int ny_models_digest_load(const char *file, char *hex);
static uint64_t ny_models_file_size(const char *file);
static uint64_t ny_models_space(void);
static uint64_t ny_models_file_limit(void);
static int ny_models_claim(void);
static void ny_models_release(void);
static int ny_models_digest(const char *file, char *hex, uint64_t *size,
                            uint64_t *progress);
static int ny_models_store(int file, const uint8_t *data, size_t length);
static int ny_models_receive(int fd, int file, uint64_t expected,
                             const void *body, size_t body_length,
                             uint64_t *stored);
static int ny_models_finish(int fd, struct ny_models_job_s *job);
static int ny_models_upload(int fd, const char *head, const char *query,
                            const void *body, size_t body_length);
static int ny_models_state(int fd, const char *query);
static void ny_models_walk_file(struct ny_models_walk_s *walk,
                                size_t length, const struct stat *status);
static void ny_models_walk(struct ny_models_walk_s *walk, size_t length,
                           int depth);
static cJSON *ny_models_list(void);
static cJSON *ny_models_status(void);
static int ny_models_remove(const cJSON *data, cJSON **result);
static void *ny_models_verify_worker(void *argument);
static int ny_models_verify(const cJSON *data, cJSON **result);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The directories a model may go to.  A kind that is not listed here cannot
 * be uploaded, listed or deleted, whatever a client sends.
 */

static const char *const g_models_kinds[] =
{
  "llm", "asr", "tts", "kws", "speaker", "face",
};

/* One lock for the claim and for the two progress records.  The claim has
 * one holder at a time: an upload, a removal, or the verification worker.
 */

static mutex_t g_models_lock = NXMUTEX_INITIALIZER;
static bool g_models_claimed;
static struct ny_models_progress_s g_models_progress;
static struct ny_models_verify_s g_models_verify =
{
  .reason = "",
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_models_reply
 ****************************************************************************/

static int ny_models_reply(int fd, int code, const char *reason,
                           const char *json)
{
  char buffer[640];
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
 * Name: ny_models_refuse_json
 *
 * Description:
 *   Answer while the peer may still be sending, and make sure the answer
 *   can be read.  Closing a socket with unread data resets the connection,
 *   which a browser reports as a network failure and not as the status it
 *   was just sent; the offset in a 409 is exactly what must not be lost.
 *
 ****************************************************************************/

static int ny_models_refuse_json(int fd, int code, const char *reason,
                                 const char *json)
{
  int ret = ny_models_reply(fd, code, reason, json);
  if (ret == 0)
    {
      ny_models_drain(fd);
    }

  return ret;
}

/****************************************************************************
 * Name: ny_models_refuse
 ****************************************************************************/

static int ny_models_refuse(int fd, int code, const char *reason,
                            const char *error)
{
  char json[96];

  snprintf(json, sizeof(json), "{\"error\":\"%s\"}", error);
  return ny_models_refuse_json(fd, code, reason, json);
}

/****************************************************************************
 * Name: ny_models_drain
 ****************************************************************************/

static void ny_models_drain(int fd)
{
  uint64_t deadline = nyabula_eye_ws_now() + NY_MODELS_DRAIN_MS;
  uint64_t drained = 0;
  char scratch[512];

  /* The response carries its own length, so the peer knows it is complete
   * without the connection being half-closed first.  Bounded both ways: a
   * peer that keeps sending must not hold a client slot for it.
   */

  while (drained < NY_MODELS_DRAIN_BYTES)
    {
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd =
      {
        fd, POLLIN, 0
      };

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
          drained += (uint64_t)count;
        }
    }
}

/****************************************************************************
 * Name: ny_models_hex
 ****************************************************************************/

static void ny_models_hex(const uint8_t *digest, char *hex)
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
 * Name: ny_models_digest_ok
 *
 * Description:
 *   Whether text is exactly 64 hex digits.  The firmware upload has the
 *   same test, but it is only built with NYABULA_CORE_OTA and this module
 *   has to stand without it.
 *
 ****************************************************************************/

static bool ny_models_digest_ok(const char *text, size_t length)
{
  size_t i;

  if (text == NULL || length != NY_MODELS_SHA256_HEX)
    {
      return false;
    }

  for (i = 0; i < length; i++)
    {
      if (ny_models_hex_value(text[i]) < 0)
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: ny_models_token_equal
 *
 * Description:
 *   Compare two tokens of NY_WEB_AUTH_TOKEN_SIZE characters in a time that
 *   does not depend on where they first differ.
 *
 ****************************************************************************/

static bool ny_models_token_equal(const char *offered, const char *expected)
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
 * Name: ny_models_authorized
 *
 * Description:
 *   The rule of the firmware upload: the pair token shown on the eyes, or
 *   the session token a correct password was exchanged for.  Either one
 *   makes the socket an owner's, so either one is the owner here.
 *
 *   A page from another site cannot forge this request from the owner's
 *   browser.  Authorization is not a header a cross-origin request may set
 *   without a preflight, and nothing here answers one.
 *
 ****************************************************************************/

static bool ny_models_authorized(const char *head, const char *pair_token)
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
      length != NY_MODELS_BEARER_SIZE + NY_WEB_AUTH_TOKEN_SIZE ||
      strncasecmp(value, NY_MODELS_BEARER, NY_MODELS_BEARER_SIZE) != 0)
    {
      return false;
    }

  value += NY_MODELS_BEARER_SIZE;
  valid = ny_models_token_equal(value, pair_token);
  if (ny_web_auth_session_token(pair_token, session) == 0)
    {
      /* Both comparisons always run, so the time taken does not say which
       * kind of token was offered.
       */

      bool match = ny_models_token_equal(value, session);
      valid = valid || match;
    }

  memset(session, 0, sizeof(session));
  return valid;
}

/****************************************************************************
 * Name: ny_models_hex_value
 ****************************************************************************/

static int ny_models_hex_value(char c)
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
 * Name: ny_models_ends_with
 *
 * Description:
 *   Without regard to case: the volume does not tell "X.PART" from
 *   "x.part" either.
 *
 ****************************************************************************/

static bool ny_models_ends_with(const char *name, size_t length,
                                const char *suffix)
{
  size_t size = strlen(suffix);

  return length > size &&
         strncasecmp(name + length - size, suffix, size) == 0;
}

/****************************************************************************
 * Name: ny_models_path_ok
 *
 * Description:
 *   Accept only "<kind>/<one to three names>".
 *
 *   The list is of what is allowed rather than of what is dangerous.  A
 *   name is letters, digits, dot, dash and underscore, and neither starts
 *   nor ends with a dot: that leaves no second spelling of "..", no hidden
 *   file, and nothing the volume would silently store under another name.
 *   The names this module puts beside a model are not models.
 *
 ****************************************************************************/

static bool ny_models_path_ok(const char *path)
{
  const char *segment = path;
  size_t length = strlen(path);
  size_t span = 0;
  unsigned int segments = 0;
  bool known = false;
  size_t i;

  if (length == 0 || length > NY_WEB_MODELS_PATH_MAX)
    {
      return false;
    }

  for (; ; )
    {
      span = strcspn(segment, "/");
      if (span == 0 || span > NY_MODELS_SEGMENT_MAX || segment[0] == '.' ||
          segment[span - 1] == '.')
        {
          return false;
        }

      for (i = 0; i < span; i++)
        {
          char c = segment[i];
          bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                       c == '_';
          if (!plain)
            {
              return false;
            }
        }

      if (segments == 0)
        {
          for (i = 0; i < sizeof(g_models_kinds) / sizeof(g_models_kinds[0]);
               i++)
            {
              if (strlen(g_models_kinds[i]) == span &&
                  memcmp(g_models_kinds[i], segment, span) == 0)
                {
                  known = true;
                }
            }
        }

      segments++;
      if (segment[span] == '\0')
        {
          break;
        }

      segment += span + 1;
    }

  if (!known || segments < 2 || segments > 1 + NY_MODELS_LEVELS)
    {
      return false;
    }

  /* ".part.json" ends with the second of these; a model's own "*.json" is
   * an ordinary file.
   */

  return !ny_models_ends_with(segment, span, NY_MODELS_PART) &&
         !ny_models_ends_with(segment, span, NY_MODELS_SIDECAR) &&
         !ny_models_ends_with(segment, span, NY_MODELS_DIGEST);
}

/****************************************************************************
 * Name: ny_models_query_path
 *
 * Description:
 *   The value of "path" in the query of the request target, decoded and
 *   checked.  query points at the '?', or at whatever ended the target when
 *   there is no query, and the request line ends at the next space.
 *
 *   A browser escapes the '/' of a path it was told to encode, so the
 *   escapes have to be understood; what comes out still has to pass
 *   ny_models_path_ok() before it is used for anything.
 *
 ****************************************************************************/

static int ny_models_query_path(const char *query, char *path, size_t size)
{
  static const char key[] = NY_MODELS_QUERY_KEY "=";
  const char *at = query;
  size_t used = 0;

  if (*at != '?')
    {
      return -EINVAL;
    }

  at++;
  while (strncmp(at, key, sizeof(key) - 1) != 0)
    {
      /* Not this parameter: on to the one after the next '&'. */

      at += strcspn(at, "& #\r\n");
      if (*at != '&')
        {
          return -EINVAL;
        }

      at++;
    }

  at += sizeof(key) - 1;
  while (*at != '\0' && strchr("& #\r\n", *at) == NULL)
    {
      int value = (unsigned char)*at;

      if (*at == '%')
        {
          int high = ny_models_hex_value(at[1]);
          int low = high < 0 ? -1 : ny_models_hex_value(at[2]);

          if (low < 0)
            {
              return -EINVAL;
            }

          value = (high << 4) | low;
          at += 2;
        }

      if (value <= ' ' || value >= 0x7f || used + 1 >= size)
        {
          return -EINVAL;
        }

      path[used++] = (char)value;
      at++;
    }

  path[used] = '\0';
  return ny_models_path_ok(path) ? 0 : -EINVAL;
}

/****************************************************************************
 * Name: ny_models_paths
 *
 * Description:
 *   The four files of one model.  path has passed ny_models_path_ok(), so
 *   every one of them fits.
 *
 ****************************************************************************/

static void ny_models_paths(const char *path, struct ny_models_paths_s *paths)
{
  snprintf(paths->target, sizeof(paths->target), "%s/%s",
           NY_WEB_MODELS_ROOT, path);
  snprintf(paths->part, sizeof(paths->part), "%s/%s%s",
           NY_WEB_MODELS_ROOT, path, NY_MODELS_PART);
  snprintf(paths->sidecar, sizeof(paths->sidecar), "%s/%s%s",
           NY_WEB_MODELS_ROOT, path, NY_MODELS_SIDECAR);
  snprintf(paths->digest, sizeof(paths->digest), "%s/%s%s",
           NY_WEB_MODELS_ROOT, path, NY_MODELS_DIGEST);
}

/****************************************************************************
 * Name: ny_models_parents
 *
 * Description:
 *   Make the directories a file is in.  file is changed while this runs and
 *   is whole again when it returns.  -ENOTDIR when one of them exists as
 *   something else.
 *
 ****************************************************************************/

static int ny_models_parents(char *file)
{
  char *slash = file + sizeof(NY_WEB_MODELS_ROOT) - 1;
  int ret = 0;

  while (ret == 0 && slash != NULL)
    {
      struct stat status;

      *slash = '\0';
      if (mkdir(file, 0755) < 0 && errno != EEXIST)
        {
          ret = -errno;
        }
      else if (stat(file, &status) < 0 || !S_ISDIR(status.st_mode))
        {
          ret = -ENOTDIR;
        }

      *slash = '/';
      slash = strchr(slash + 1, '/');
    }

  return ret;
}

/****************************************************************************
 * Name: ny_models_prune
 *
 * Description:
 *   Remove the directories a deleted file has left empty, up to but not
 *   including the root.  rmdir() refuses one that still holds something,
 *   which is the whole test.  file is whole again when this returns.
 *
 ****************************************************************************/

static void ny_models_prune(char *file)
{
  size_t root = sizeof(NY_WEB_MODELS_ROOT) - 1;
  size_t length = strlen(file);
  bool emptied = true;
  size_t at = length;

  while (emptied)
    {
      while (at > root && file[at] != '/')
        {
          at--;
        }

      if (at <= root)
        {
          break;
        }

      file[at] = '\0';
      emptied = rmdir(file) == 0;
      at--;
    }

  /* Every terminator that was put in stands where a '/' was. */

  for (at = root; at < length; at++)
    {
      if (file[at] == '\0')
        {
          file[at] = '/';
        }
    }
}

/****************************************************************************
 * Name: ny_models_number
 *
 * Description:
 *   Parse a run of decimal digits and move past it.  The limit on their
 *   number keeps the sum inside 64 bits, and far inside what JSON carries
 *   exactly.
 *
 ****************************************************************************/

static int ny_models_number(const char **at, uint64_t *value)
{
  const char *c = *at;
  uint64_t sum = 0;
  size_t digits = 0;

  while (*c >= '0' && *c <= '9')
    {
      if (++digits > NY_MODELS_NUMBER_DIGITS)
        {
          return -EFBIG;
        }

      sum = sum * 10 + (uint64_t)(*c - '0');
      c++;
    }

  if (digits == 0)
    {
      return -EINVAL;
    }

  *at = c;
  *value = sum;
  return 0;
}

/****************************************************************************
 * Name: ny_models_length
 *
 * Description:
 *   Parse Content-Length.  -ENOENT when it is absent, -EINVAL when it is
 *   not a number, -EFBIG when it cannot possibly fit.
 *
 ****************************************************************************/

static int ny_models_length(const char *head, uint64_t *length)
{
  char text[NY_MODELS_NUMBER_DIGITS + 2];
  size_t size = 0;
  const char *value = ny_web_http_header(head, "Content-Length", &size);
  const char *at = text;
  int ret;

  if (value == NULL)
    {
      return -ENOENT;
    }

  while (size > 0 && (value[size - 1] == ' ' || value[size - 1] == '\t'))
    {
      size--;
    }

  if (size >= sizeof(text))
    {
      return -EFBIG;
    }

  memcpy(text, value, size);
  text[size] = '\0';
  ret = ny_models_number(&at, length);
  return ret == 0 && *at != '\0' ? -EINVAL : ret;
}

/****************************************************************************
 * Name: ny_models_range
 *
 * Description:
 *   Parse Content-Range into the job: "bytes <start>-<end>/<total>" for a
 *   piece, or "bytes * / <total>" (without the spaces) for a request that
 *   carries none and only asks for a finished part to be checked and put
 *   in place.  -ENOENT when the header is absent, -EINVAL when it does not
 *   add up with Content-Length.
 *
 ****************************************************************************/

static int ny_models_range(const char *head, struct ny_models_job_s *job)
{
  char text[64];
  size_t size = 0;
  const char *value = ny_web_http_header(head, "Content-Range", &size);
  const char *at = text;
  uint64_t end = 0;

  if (value == NULL)
    {
      return -ENOENT;
    }

  while (size > 0 && (value[size - 1] == ' ' || value[size - 1] == '\t'))
    {
      size--;
    }

  if (size >= sizeof(text))
    {
      return -EINVAL;
    }

  memcpy(text, value, size);
  text[size] = '\0';
  if (strncasecmp(at, "bytes", 5) != 0 || (at[5] != ' ' && at[5] != '\t'))
    {
      return -EINVAL;
    }

  at += 5;
  while (*at == ' ' || *at == '\t')
    {
      at++;
    }

  job->probe = *at == '*';
  if (job->probe)
    {
      at++;
      job->start = 0;
    }
  else if (ny_models_number(&at, &job->start) < 0 || *at++ != '-' ||
           ny_models_number(&at, &end) < 0)
    {
      return -EINVAL;
    }

  if (*at++ != '/' || ny_models_number(&at, &job->total) < 0 || *at != '\0')
    {
      return -EINVAL;
    }

  if (job->total == 0)
    {
      return -EINVAL;
    }

  if (job->probe)
    {
      return job->length == 0 ? 0 : -EINVAL;
    }

  return end >= job->start && end < job->total &&
         end - job->start + 1 == job->length ? 0 : -EINVAL;
}

/****************************************************************************
 * Name: ny_models_sidecar_load
 *
 * Description:
 *   Read "<name>.part.json".  Anything but a complete, plausible record is
 *   no record: the part it describes then starts again from nothing, which
 *   costs time and never stores a wrong file.
 *
 ****************************************************************************/

static int ny_models_sidecar_load(const char *file,
                                  struct ny_models_sidecar_s *sidecar)
{
  char text[NY_MODELS_SIDECAR_SIZE];
  const cJSON *total;
  const cJSON *received;
  const cJSON *digest;
  cJSON *root;
  ssize_t count;
  int ret = -EINVAL;
  int descriptor;

  descriptor = open(file, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    {
      return -errno;
    }

  count = read(descriptor, text, sizeof(text) - 1);
  close(descriptor);
  if (count <= 0)
    {
      return -EINVAL;
    }

  text[count] = '\0';
  root = cJSON_Parse(text);
  total = cJSON_GetObjectItemCaseSensitive(root, "total");
  received = cJSON_GetObjectItemCaseSensitive(root, "received");
  digest = cJSON_GetObjectItemCaseSensitive(root, "sha256");
  if (cJSON_IsNumber(total) && cJSON_IsNumber(received) &&
      cJSON_IsString(digest) && total->valuedouble >= 1 &&
      total->valuedouble <= (double)NY_MODELS_FILE_LIMIT &&
      received->valuedouble >= 0 &&
      received->valuedouble <= total->valuedouble &&
      ny_models_digest_ok(digest->valuestring, strlen(digest->valuestring)))
    {
      sidecar->total = (uint64_t)total->valuedouble;
      sidecar->received = (uint64_t)received->valuedouble;
      strlcpy(sidecar->sha256, digest->valuestring, sizeof(sidecar->sha256));
      ret = 0;
    }

  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_models_sidecar_save
 ****************************************************************************/

static int ny_models_sidecar_save(const char *file,
                                  const struct ny_models_sidecar_s *sidecar)
{
  char text[NY_MODELS_SIDECAR_SIZE];

  /* The digest is 64 hex digits: nothing in it needs escaping. */

  snprintf(text, sizeof(text),
           "{\"total\":%llu,\"sha256\":\"%s\",\"received\":%llu}\n",
           (unsigned long long)sidecar->total, sidecar->sha256,
           (unsigned long long)sidecar->received);
  return ny_models_text_save(file, text);
}

/****************************************************************************
 * Name: ny_models_text_save
 ****************************************************************************/

static int ny_models_text_save(const char *file, const char *text)
{
  size_t length = strlen(text);
  size_t written = 0;
  int ret = 0;
  int descriptor;

  descriptor = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0)
    {
      return -errno;
    }

  while (ret == 0 && written < length)
    {
      ssize_t count = write(descriptor, text + written, length - written);
      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          ret = count < 0 ? -errno : -EIO;
          break;
        }

      written += (size_t)count;
    }

  if (close(descriptor) < 0 && ret == 0)
    {
      ret = -errno;
    }

  return ret;
}

/****************************************************************************
 * Name: ny_models_digest_load
 *
 * Description:
 *   Read "<name>.sha256" as lowercase hex.  hex must hold
 *   NY_MODELS_SHA256_HEX + 1 bytes.  -ENOENT when there is none, -EINVAL
 *   when what is there is not a digest.
 *
 ****************************************************************************/

static int ny_models_digest_load(const char *file, char *hex)
{
  char text[NY_MODELS_SHA256_HEX];
  ssize_t count;
  size_t i;
  int descriptor;

  descriptor = open(file, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    {
      return -ENOENT;
    }

  count = read(descriptor, text, sizeof(text));
  close(descriptor);
  if (count != NY_MODELS_SHA256_HEX ||
      !ny_models_digest_ok(text, NY_MODELS_SHA256_HEX))
    {
      return -EINVAL;
    }

  for (i = 0; i < NY_MODELS_SHA256_HEX; i++)
    {
      hex[i] = text[i] >= 'A' && text[i] <= 'F' ? (char)(text[i] - 'A' + 'a')
                                                : text[i];
    }

  hex[NY_MODELS_SHA256_HEX] = '\0';
  return 0;
}

/****************************************************************************
 * Name: ny_models_file_size
 *
 * Description:
 *   The size of a regular file, 0 when there is none.
 *
 ****************************************************************************/

static uint64_t ny_models_file_size(const char *file)
{
  struct stat status;

  if (stat(file, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0)
    {
      return 0;
    }

  return (uint64_t)status.st_size;
}

/****************************************************************************
 * Name: ny_models_space
 *
 * Description:
 *   What the volume has left, in bytes.  0 when that cannot be found out,
 *   which is never an invitation to write.
 *
 ****************************************************************************/

static uint64_t ny_models_space(void)
{
  struct statfs volume;

  if (statfs(NY_WEB_MODELS_VOLUME, &volume) < 0)
    {
      return 0;
    }

  return (uint64_t)volume.f_bavail * (uint64_t)volume.f_bsize;
}

/****************************************************************************
 * Name: ny_models_file_limit
 *
 * Description:
 *   The largest file that can be stored.  The volume keeps the size of a
 *   file in 32 bits, and a build without FS_LARGEFILE cannot even seek that
 *   far: its file offsets are 32 bits wide and signed.
 *
 ****************************************************************************/

static uint64_t ny_models_file_limit(void)
{
  return sizeof(off_t) < sizeof(uint64_t) ? (uint64_t)INT32_MAX
                                          : NY_MODELS_FILE_LIMIT;
}

/****************************************************************************
 * Name: ny_models_claim
 ****************************************************************************/

static int ny_models_claim(void)
{
  int ret = nxmutex_lock(&g_models_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_models_claimed)
    {
      ret = -EBUSY;
    }
  else
    {
      g_models_claimed = true;
    }

  nxmutex_unlock(&g_models_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_models_release
 ****************************************************************************/

static void ny_models_release(void)
{
  nxmutex_lock(&g_models_lock);
  g_models_claimed = false;
  g_models_progress.active = false;
  g_models_progress.hashing = false;
  nxmutex_unlock(&g_models_lock);
}

/****************************************************************************
 * Name: ny_models_digest
 *
 * Description:
 *   SHA-256 of a file as lowercase hex, read in pieces of 64 KiB so that
 *   the size of the file never becomes the size of a buffer.  progress,
 *   when it is given, is one of the counters models.status reports and is
 *   updated under their lock.
 *
 ****************************************************************************/

static int ny_models_digest(const char *file, char *hex, uint64_t *size,
                            uint64_t *progress)
{
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA2_CTX hash;
  uint64_t total = 0;
  uint8_t *chunk;
  int descriptor;
  int ret = 0;

  descriptor = open(file, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    {
      return -errno;
    }

  chunk = malloc(NY_MODELS_HASH_READ);
  if (chunk == NULL)
    {
      close(descriptor);
      return -ENOMEM;
    }

  sha256init(&hash);
  for (; ; )
    {
      ssize_t count = read(descriptor, chunk, NY_MODELS_HASH_READ);
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
      if (progress != NULL)
        {
          nxmutex_lock(&g_models_lock);
          *progress = total;
          nxmutex_unlock(&g_models_lock);
        }
    }

  free(chunk);
  close(descriptor);
  if (ret < 0)
    {
      return ret;
    }

  sha256final(digest, &hash);
  ny_models_hex(digest, hex);
  *size = total;
  return 0;
}

/****************************************************************************
 * Name: ny_models_store
 ****************************************************************************/

static int ny_models_store(int file, const uint8_t *data, size_t length)
{
  size_t written = 0;

  while (written < length)
    {
      ssize_t count = write(file, data + written, length - written);
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

  return 0;
}

/****************************************************************************
 * Name: ny_models_receive
 *
 * Description:
 *   Read exactly the announced number of body bytes into the file, at its
 *   current position.  *stored counts what has been written, also when this
 *   fails half way: the caller keeps that much.
 *
 *   The first bytes may not be on the socket any more: the read that
 *   completed the request head takes whatever had arrived, and a client is
 *   free to send head and body together.  Those bytes are handed in as
 *   body/body_length and are stored first, in order.
 *
 ****************************************************************************/

static int ny_models_receive(int fd, int file, uint64_t expected,
                             const void *body, size_t body_length,
                             uint64_t *stored)
{
  uint64_t deadline;
  uint8_t *chunk;
  int ret = 0;

  /* More than was announced is not part of this request. */

  if ((uint64_t)body_length > expected)
    {
      body_length = (size_t)expected;
    }

  if (body_length > 0)
    {
      ret = ny_models_store(file, body, body_length);
      if (ret < 0)
        {
          return ret;
        }

      *stored += body_length;
      nxmutex_lock(&g_models_lock);
      g_models_progress.received += body_length;
      nxmutex_unlock(&g_models_lock);
    }

  chunk = malloc(NY_MODELS_RECV);
  if (chunk == NULL)
    {
      return -ENOMEM;
    }

  deadline = nyabula_eye_ws_now() + NY_MODELS_STALL_MS;
  while (ret == 0 && *stored < expected)
    {
      uint64_t left = expected - *stored;
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd =
      {
        fd, POLLIN, 0
      };

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

      want = left < NY_MODELS_RECV ? (size_t)left : NY_MODELS_RECV;
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

      ret = ny_models_store(file, chunk, (size_t)count);
      if (ret == 0)
        {
          *stored += (uint64_t)count;
          nxmutex_lock(&g_models_lock);
          g_models_progress.received += (uint64_t)count;
          nxmutex_unlock(&g_models_lock);
        }

      /* The limit is on a link that has gone quiet, not on a large piece
       * over a slow one: progress buys more time.
       */

      deadline = nyabula_eye_ws_now() + NY_MODELS_STALL_MS;
    }

  free(chunk);
  return ret;
}

/****************************************************************************
 * Name: ny_models_finish
 *
 * Description:
 *   Every byte of the part is in.  Read it back, and give it its real name
 *   only if it is the file the client said it was sending.
 *
 *   This takes as long as reading the whole file takes, and the client is
 *   waiting for the answer to its last piece meanwhile.  models.status
 *   says how far it is.  The body of the request has been read to its end
 *   by now, so there is nothing to drain before the connection closes.
 *
 ****************************************************************************/

static int ny_models_finish(int fd, struct ny_models_job_s *job)
{
  char actual[NY_MODELS_SHA256_HEX + 1];
  char json[320];
  uint64_t size = 0;
  int code = 200;
  const char *reason = "OK";
  int ret;

  nxmutex_lock(&g_models_lock);
  g_models_progress.hashing = true;
  g_models_progress.hashed = 0;
  nxmutex_unlock(&g_models_lock);

  ret = ny_models_digest(job->paths.part, actual, &size,
                         &g_models_progress.hashed);
  if (ret < 0)
    {
      /* The part could not be read, which says nothing about what is in
       * it.  It stays, and the next request tries again.
       */

      snprintf(json, sizeof(json), "{\"error\":\"ESTORAGE\"}");
      code = 500;
      reason = "Internal Server Error";
    }
  else if (size != job->total ||
           memcmp(actual, job->wanted, NY_MODELS_SHA256_HEX) != 0)
    {
      /* The bytes that are here are not the bytes of that file.  Which of
       * them are wrong cannot be known, so all of them go.
       */

      unlink(job->paths.part);
      unlink(job->paths.sidecar);
      snprintf(json, sizeof(json),
               "{\"error\":\"EDIGEST\",\"received\":%llu,\"sha256\":\"%s\"}",
               (unsigned long long)size, actual);
      code = 422;
      reason = "Unprocessable Content";
    }
  else
    {
      /* rename() does not replace a file on this volume.  Between the two
       * calls the model has no name at all, but its bytes are complete and
       * stay where a later request finds and finishes them.
       */

      unlink(job->paths.target);
      if (rename(job->paths.part, job->paths.target) < 0)
        {
          snprintf(json, sizeof(json), "{\"error\":\"ESTORAGE\"}");
          code = 500;
          reason = "Internal Server Error";
        }
      else
        {
          /* A digest that cannot be stored costs a verification later and
           * nothing else.
           */

          ny_models_text_save(job->paths.digest, actual);
          unlink(job->paths.sidecar);
          fprintf(stderr, "nyabula_web: model %s stored, %llu bytes\n",
                  job->path, (unsigned long long)size);

          /* The path has passed ny_models_path_ok(): nothing in it needs
           * escaping.
           */

          snprintf(json, sizeof(json),
                   "{\"path\":\"%s\",\"received\":%llu,\"total\":%llu,"
                   "\"complete\":true,\"sha256\":\"%s\"}",
                   job->path, (unsigned long long)size,
                   (unsigned long long)size, actual);
        }
    }

  return ny_models_reply(fd, code, reason, json);
}

/****************************************************************************
 * Name: ny_models_upload
 ****************************************************************************/

static int ny_models_upload(int fd, const char *head, const char *query,
                            const void *body, size_t body_length)
{
  struct ny_models_job_s *job;
  char json[256];
  const char *value;
  uint64_t stored = 0;
  uint64_t held;
  uint64_t space;
  size_t length = 0;
  size_t i;
  bool fresh;
  int file;
  int ret;

  job = calloc(1, sizeof(*job));
  if (job == NULL)
    {
      return ny_models_refuse(fd, 500, "Internal Server Error", "ENOMEM");
    }

  if (ny_models_query_path(query, job->path, sizeof(job->path)) < 0)
    {
      ret = ny_models_refuse(fd, 400, "Bad Request", "EPATH");
      goto done;
    }

  ret = ny_models_length(head, &job->length);
  if (ret == -ENOENT)
    {
      ret = ny_models_refuse(fd, 411, "Length Required", "ELENGTH");
      goto done;
    }

  if (ret == -EFBIG || (ret == 0 && job->length > NY_MODELS_PIECE_MAX))
    {
      snprintf(json, sizeof(json),
               "{\"error\":\"ECHUNK\",\"reason\":\"piece-too-large\","
               "\"limit\":%llu}",
               (unsigned long long)NY_MODELS_PIECE_MAX);
      ret = ny_models_refuse_json(fd, 413, "Content Too Large", json);
      goto done;
    }

  if (ret < 0)
    {
      ret = ny_models_refuse(fd, 400, "Bad Request", "EINVAL");
      goto done;
    }

  if (ny_models_range(head, job) < 0)
    {
      ret = ny_models_refuse(fd, 400, "Bad Request", "ERANGE");
      goto done;
    }

  /* The volume cannot hold a larger file whatever it has left. */

  if (job->total > ny_models_file_limit())
    {
      snprintf(json, sizeof(json),
               "{\"error\":\"ETOOLARGE\",\"reason\":\"%s\",\"limit\":%llu}",
               job->total > NY_MODELS_FILE_LIMIT ? "fat32-file-limit"
                                                 : "offset-width",
               (unsigned long long)ny_models_file_limit());
      ret = ny_models_refuse_json(fd, 413, "Content Too Large", json);
      goto done;
    }

  value = ny_web_http_header(head, "X-Nya-Sha256", &length);
  while (value != NULL && length > 0 &&
         (value[length - 1] == ' ' || value[length - 1] == '\t'))
    {
      length--;
    }

  if (value == NULL || !ny_models_digest_ok(value, length))
    {
      ret = ny_models_refuse(fd, 400, "Bad Request", "EDIGEST");
      goto done;
    }

  for (i = 0; i < NY_MODELS_SHA256_HEX; i++)
    {
      job->wanted[i] = value[i] >= 'A' && value[i] <= 'F'
                           ? (char)(value[i] - 'A' + 'a')
                           : value[i];
    }

  job->wanted[NY_MODELS_SHA256_HEX] = '\0';

  if (ny_models_claim() < 0)
    {
      ret = ny_models_refuse(fd, 409, "Conflict", "EBUSY");
      goto done;
    }

  /* From here the claim is held; every path below releases it. */

  ny_models_paths(job->path, &job->paths);
  ret = ny_models_parents(job->paths.target);
  if (ret == 0)
    {
      struct stat status;
      if (stat(job->paths.target, &status) == 0 && !S_ISREG(status.st_mode))
        {
          ret = -ENOTDIR;
        }
    }

  if (ret < 0)
    {
      /* A name that is taken by a directory, or a directory that is taken
       * by a file: no retry changes that.
       */

      ret = ret == -ENOTDIR
                ? ny_models_refuse(fd, 409, "Conflict", "EPATHTYPE")
                : ny_models_refuse(fd, 500, "Internal Server Error",
                                   "ESTORAGE");
      goto out;
    }

  /* What is already here counts only if it is a part of this very file.
   * The record may be ahead of the part after a loss of power, never the
   * other way round in a way that matters: bytes beyond the record are
   * simply written again.
   */

  held = ny_models_file_size(job->paths.part);
  fresh = ny_models_sidecar_load(job->paths.sidecar, &job->sidecar) < 0 ||
          job->sidecar.total != job->total ||
          memcmp(job->sidecar.sha256, job->wanted,
                 NY_MODELS_SHA256_HEX) != 0;
  job->received = fresh ? 0
                  : job->sidecar.received < held ? job->sidecar.received
                                                 : held;

  if (job->probe ? job->received != job->total
                 : job->start != job->received)
    {
      /* Not where this file stands.  The answer says where that is, and
       * the client goes on from there; a different file that was left
       * unfinished under this name is given up for the new one as soon as
       * a piece arrives that starts at zero.
       */

      snprintf(json, sizeof(json),
               "{\"error\":\"EOFFSET\",\"received\":%llu,\"total\":%llu}",
               (unsigned long long)job->received,
               (unsigned long long)job->total);
      ret = ny_models_refuse_json(fd, 409, "Conflict", json);
      goto out;
    }

  nxmutex_lock(&g_models_lock);
  g_models_progress.active = true;
  g_models_progress.hashing = false;
  g_models_progress.received = job->received;
  g_models_progress.total = job->total;
  g_models_progress.hashed = 0;
  strlcpy(g_models_progress.path, job->path, sizeof(g_models_progress.path));
  nxmutex_unlock(&g_models_lock);

  if (job->probe)
    {
      /* A part that was complete when something came between it and its
       * name: a lost connection during the hash, or a loss of power.
       */

      ret = ny_models_finish(fd, job);
      goto out;
    }

  if (fresh)
    {
      /* Whatever was left under this name belongs to another file, and its
       * blocks may be exactly the ones this one needs.
       */

      unlink(job->paths.part);
      unlink(job->paths.sidecar);
    }

  space = ny_models_space();
  if (space < NY_MODELS_SPACE_MARGIN ||
      space - NY_MODELS_SPACE_MARGIN < job->total - job->received)
    {
      snprintf(json, sizeof(json),
               "{\"error\":\"ENOSPACE\",\"reason\":\"volume-full\","
               "\"free\":%llu,\"needed\":%llu,\"margin\":%llu}",
               (unsigned long long)space,
               (unsigned long long)(job->total - job->received),
               (unsigned long long)NY_MODELS_SPACE_MARGIN);
      ret = ny_models_refuse_json(fd, 413, "Content Too Large", json);
      goto out;
    }

  if (fresh)
    {
      job->sidecar.total = job->total;
      job->sidecar.received = 0;
      strlcpy(job->sidecar.sha256, job->wanted, sizeof(job->sidecar.sha256));
      if (ny_models_sidecar_save(job->paths.sidecar, &job->sidecar) < 0)
        {
          ret = ny_models_refuse(fd, 500, "Internal Server Error",
                                 "ESTORAGE");
          goto out;
        }
    }

  file = open(job->paths.part,
              O_WRONLY | O_CREAT | O_CLOEXEC | (fresh ? O_TRUNC : 0), 0600);
  if (file < 0 || lseek(file, (off_t)job->received, SEEK_SET) < 0)
    {
      if (file >= 0)
        {
          close(file);
        }

      ret = ny_models_refuse(fd, 500, "Internal Server Error", "ESTORAGE");
      goto out;
    }

  /* A client that asked may be holding the body back until it is told to
   * go on.  Browsers never ask; command line tools do for large bodies.
   */

  ret = 0;
  value = ny_web_http_header(head, "Expect", &length);
  if (value != NULL && length >= 12 &&
      strncasecmp(value, "100-continue", 12) == 0)
    {
      static const char go_on[] = "HTTP/1.1 100 Continue\r\n\r\n";
      ret = ny_web_http_write(fd, go_on, sizeof(go_on) - 1);
    }

  if (ret == 0)
    {
      ret = ny_models_receive(fd, file, job->length, body, body_length,
                              &stored);
    }

  /* What is kept is what reached the medium.  If that cannot be said, the
   * record stays where it was and the bytes are sent again.
   */

  if (fsync(file) < 0)
    {
      stored = 0;
      ret = ret == 0 ? -errno : ret;
    }

  if (close(file) < 0)
    {
      stored = 0;
      ret = ret == 0 ? -errno : ret;
    }

  if (stored > 0)
    {
      job->received += stored;
      job->sidecar.received = job->received;
      if (ny_models_sidecar_save(job->paths.sidecar, &job->sidecar) < 0 &&
          ret == 0)
        {
          ret = -EIO;
        }
    }

  if (ret < 0)
    {
      const char *error = "ESTORAGE";
      const char *reason = "Internal Server Error";
      int code = 500;

      if (ret == -ECONNRESET || ret == -EPIPE || ret == -ENOTCONN)
        {
          /* Nobody is left to tell. */

          goto out;
        }

      if (ret == -ENOSPC)
        {
          error = "ENOSPACE";
          reason = "Insufficient Storage";
          code = 507;
        }
      else if (ret == -ETIMEDOUT)
        {
          error = "ETIMEDOUT";
          reason = "Request Timeout";
          code = 408;
        }

      snprintf(json, sizeof(json),
               "{\"error\":\"%s\",\"received\":%llu,\"total\":%llu}", error,
               (unsigned long long)job->received,
               (unsigned long long)job->total);
      ret = ny_models_refuse_json(fd, code, reason, json);
      goto out;
    }

  if (job->received == job->total)
    {
      ret = ny_models_finish(fd, job);
      goto out;
    }

  snprintf(json, sizeof(json),
           "{\"path\":\"%s\",\"received\":%llu,\"total\":%llu,"
           "\"complete\":false}",
           job->path, (unsigned long long)job->received,
           (unsigned long long)job->total);
  ret = ny_models_reply(fd, 200, "OK", json);

out:
  ny_models_release();

done:
  free(job);
  return ret;
}

/****************************************************************************
 * Name: ny_models_state
 *
 * Description:
 *   GET: what is here of one file, so that a page that was reloaded knows
 *   where to go on once the owner has picked the same file again.  Nothing
 *   is claimed: this only reads, and the record of an upload that is
 *   running is simply one piece behind.
 *
 ****************************************************************************/

static int ny_models_state(int fd, const char *query)
{
  struct ny_models_paths_s *paths;
  struct ny_models_sidecar_s sidecar;
  char path[NY_WEB_MODELS_PATH_MAX + 1];
  char present[NY_MODELS_SHA256_HEX + 1];
  char json[448];
  uint64_t held;
  uint64_t bytes;

  if (ny_models_query_path(query, path, sizeof(path)) < 0)
    {
      return ny_models_refuse(fd, 400, "Bad Request", "EPATH");
    }

  paths = malloc(sizeof(*paths));
  if (paths == NULL)
    {
      return ny_models_refuse(fd, 500, "Internal Server Error", "ENOMEM");
    }

  ny_models_paths(path, paths);
  held = ny_models_file_size(paths->part);
  if (ny_models_sidecar_load(paths->sidecar, &sidecar) < 0)
    {
      memset(&sidecar, 0, sizeof(sidecar));
    }
  else if (sidecar.received > held)
    {
      sidecar.received = held;
    }

  bytes = ny_models_file_size(paths->target);
  if (bytes == 0 || ny_models_digest_load(paths->digest, present) < 0)
    {
      present[0] = '\0';
    }

  free(paths);
  snprintf(json, sizeof(json),
           "{\"path\":\"%s\",\"received\":%llu,\"total\":%llu,"
           "\"sha256\":\"%s\",\"exists\":%s,\"bytes\":%llu,"
           "\"fileSha256\":\"%s\"}",
           path, (unsigned long long)sidecar.received,
           (unsigned long long)sidecar.total, sidecar.sha256,
           bytes > 0 ? "true" : "false", (unsigned long long)bytes, present);
  return ny_models_reply(fd, 200, "OK", json);
}

/****************************************************************************
 * Name: ny_models_walk_file
 *
 * Description:
 *   One regular file of the listing.  walk->path holds its full name,
 *   length characters long, with room behind it for a suffix.
 *
 ****************************************************************************/

static void ny_models_walk_file(struct ny_models_walk_s *walk, size_t length,
                                const struct stat *status)
{
  const char *relative = walk->path + sizeof(NY_WEB_MODELS_ROOT);
  struct ny_models_sidecar_s sidecar;
  char digest[NY_MODELS_SHA256_HEX + 1];
  char kind[NY_MODELS_SEGMENT_MAX + 1];
  bool partial;
  bool known = false;
  size_t cost;
  size_t span;
  cJSON *row;

  /* What this module keeps beside a model is not a model. */

  if (ny_models_ends_with(walk->path, length, NY_MODELS_SIDECAR) ||
      ny_models_ends_with(walk->path, length, NY_MODELS_DIGEST))
    {
      return;
    }

  partial = ny_models_ends_with(walk->path, length, NY_MODELS_PART);
  if (partial)
    {
      /* Its record is "<name>.part" + ".json"; the row is for "<name>". */

      strlcat(walk->path, ".json", sizeof(walk->path));
      known = ny_models_sidecar_load(walk->path, &sidecar) == 0;
      length -= sizeof(NY_MODELS_PART) - 1;
      walk->path[length] = '\0';
    }
  else
    {
      strlcat(walk->path, NY_MODELS_DIGEST, sizeof(walk->path));
      known = ny_models_digest_load(walk->path, digest) == 0;
      walk->path[length] = '\0';
    }

  cost = NY_MODELS_ITEM_COST + strlen(relative) +
         (known ? NY_MODELS_SHA256_HEX : 0);
  if (cost > walk->budget)
    {
      walk->truncated = true;
      return;
    }

  span = strcspn(relative, "/");
  if (span >= sizeof(kind))
    {
      return;
    }

  memcpy(kind, relative, span);
  kind[span] = '\0';

  row = cJSON_CreateObject();
  if (row == NULL)
    {
      walk->truncated = true;
      return;
    }

  cJSON_AddStringToObject(row, "path", relative);
  cJSON_AddStringToObject(row, "kind", kind);
  cJSON_AddNumberToObject(row, "bytes",
                          status->st_size > 0 ? (double)status->st_size : 0);
  cJSON_AddNumberToObject(row, "mtime", (double)status->st_mtime * 1000.0);
  cJSON_AddBoolToObject(row, "partial", partial);
  if (partial && known)
    {
      uint64_t held = status->st_size > 0 ? (uint64_t)status->st_size : 0;
      cJSON_AddNumberToObject(row, "received",
                              (double)(sidecar.received < held
                                           ? sidecar.received
                                           : held));
      cJSON_AddNumberToObject(row, "total", (double)sidecar.total);
      cJSON_AddStringToObject(row, "sha256", sidecar.sha256);
    }
  else if (known)
    {
      cJSON_AddStringToObject(row, "sha256", digest);
    }

  cJSON_AddItemToArray(walk->items, row);
  walk->budget -= cost;
}

/****************************************************************************
 * Name: ny_models_walk
 *
 * Description:
 *   List the files below a directory.  walk->path holds the directory,
 *   length characters long; names are put behind it in place, so the depth
 *   of the tree costs no stack.  Bounded in depth: the tree is ours and
 *   shallow, and a loop in it must not take the caller's stack with it.
 *
 ****************************************************************************/

static void ny_models_walk(struct ny_models_walk_s *walk, size_t length,
                           int depth)
{
  struct dirent *entry;
  DIR *dir = opendir(walk->path);

  if (dir == NULL)
    {
      return;
    }

  while (!walk->truncated && (entry = readdir(dir)) != NULL)
    {
      size_t name = strlen(entry->d_name);
      struct stat status;

      /* A name that is too long for the buffer cannot have come from an
       * upload.  It is left out, not cut short into another name.
       */

      if (entry->d_name[0] == '.' ||
          length + 1 + name + NY_MODELS_SUFFIX_ROOM >= sizeof(walk->path))
        {
          continue;
        }

      walk->path[length] = '/';
      memcpy(walk->path + length + 1, entry->d_name, name + 1);
      if (stat(walk->path, &status) == 0)
        {
          if (S_ISDIR(status.st_mode))
            {
              if (depth < NY_MODELS_WALK_DEPTH)
                {
                  ny_models_walk(walk, length + 1 + name, depth + 1);
                }
            }
          else if (S_ISREG(status.st_mode))
            {
              ny_models_walk_file(walk, length + 1 + name, &status);
            }
        }

      walk->path[length] = '\0';
    }

  closedir(dir);
}

/****************************************************************************
 * Name: ny_models_list
 ****************************************************************************/

static cJSON *ny_models_list(void)
{
  struct ny_models_walk_s *walk = calloc(1, sizeof(*walk));
  cJSON *root = cJSON_CreateObject();
  cJSON *kinds = cJSON_AddArrayToObject(root, "kinds");
  struct statfs volume;
  size_t i;

  if (walk != NULL)
    {
      walk->items = cJSON_AddArrayToObject(root, "items");
    }

  if (walk == NULL || root == NULL || kinds == NULL || walk->items == NULL)
    {
      free(walk);
      cJSON_Delete(root);
      return NULL;
    }

  cJSON_AddStringToObject(root, "root", NY_WEB_MODELS_ROOT);
  cJSON_AddNumberToObject(root, "free", (double)ny_models_space());
  if (statfs(NY_WEB_MODELS_VOLUME, &volume) == 0)
    {
      cJSON_AddNumberToObject(root, "volume",
                              (double)volume.f_blocks *
                              (double)volume.f_bsize);
    }

  /* What an upload will be held to, so that the panel can say no before a
   * transfer and not after it.
   */

  cJSON_AddNumberToObject(root, "margin", (double)NY_MODELS_SPACE_MARGIN);
  cJSON_AddNumberToObject(root, "fileLimit", (double)ny_models_file_limit());
  cJSON_AddNumberToObject(root, "pieceLimit", (double)NY_MODELS_PIECE_MAX);

  walk->budget = NY_MODELS_LIST_BUDGET;
  for (i = 0; i < sizeof(g_models_kinds) / sizeof(g_models_kinds[0]); i++)
    {
      int length = snprintf(walk->path, sizeof(walk->path), "%s/%s",
                            NY_WEB_MODELS_ROOT, g_models_kinds[i]);

      cJSON_AddItemToArray(kinds, cJSON_CreateString(g_models_kinds[i]));
      if (length > 0 && (size_t)length < sizeof(walk->path))
        {
          /* The kind is the first of the four names. */

          ny_models_walk(walk, (size_t)length, 1);
        }
    }

  cJSON_AddBoolToObject(root, "truncated", walk->truncated);
  free(walk);
  return root;
}

/****************************************************************************
 * Name: ny_models_status
 ****************************************************************************/

static cJSON *ny_models_status(void)
{
  static const char *const states[] =
  {
    "idle", "running", "done", "failed",
  };

  cJSON *root = cJSON_CreateObject();
  cJSON *verify = cJSON_AddObjectToObject(root, "verify");
  cJSON *upload = cJSON_AddObjectToObject(root, "upload");

  if (root == NULL || verify == NULL || upload == NULL)
    {
      cJSON_Delete(root);
      return NULL;
    }

  nxmutex_lock(&g_models_lock);
  cJSON_AddBoolToObject(root, "busy", g_models_claimed);
  cJSON_AddStringToObject(verify, "path", g_models_verify.path);
  cJSON_AddStringToObject(verify, "state", states[g_models_verify.state]);
  cJSON_AddNumberToObject(verify, "done", (double)g_models_verify.done);
  cJSON_AddNumberToObject(verify, "total", (double)g_models_verify.total);
  cJSON_AddStringToObject(verify, "sha256", g_models_verify.sha256);
  cJSON_AddStringToObject(verify, "expected", g_models_verify.expected);
  cJSON_AddStringToObject(verify, "reason", g_models_verify.reason);
  cJSON_AddBoolToObject(upload, "active", g_models_progress.active);
  if (g_models_progress.active)
    {
      cJSON_AddStringToObject(upload, "path", g_models_progress.path);
      cJSON_AddStringToObject(upload, "phase",
                              g_models_progress.hashing ? "hashing"
                                                        : "receiving");
      cJSON_AddNumberToObject(upload, "received",
                              (double)g_models_progress.received);
      cJSON_AddNumberToObject(upload, "total",
                              (double)g_models_progress.total);
      cJSON_AddNumberToObject(upload, "hashed",
                              (double)g_models_progress.hashed);
    }

  nxmutex_unlock(&g_models_lock);
  return root;
}

/****************************************************************************
 * Name: ny_models_remove
 *
 * Description:
 *   models.delete: the file, an unfinished upload of it, and the records
 *   of both.  A path that names nothing is not an error: the answer says
 *   that nothing was removed, and an unknown topic is what ENOTFOUND is
 *   kept for on this socket.
 *
 ****************************************************************************/

static int ny_models_remove(const cJSON *data, cJSON **result)
{
  const cJSON *path = cJSON_GetObjectItemCaseSensitive(data, "path");
  struct ny_models_paths_s *paths;
  bool removed;
  int ret;

  if (!cJSON_IsString(path) || !ny_models_path_ok(path->valuestring))
    {
      return -EINVAL;
    }

  paths = malloc(sizeof(*paths));
  if (paths == NULL)
    {
      return -ENOMEM;
    }

  /* An upload or a verification may have this very file open. */

  ret = ny_models_claim();
  if (ret < 0)
    {
      free(paths);
      return ret;
    }

  ny_models_paths(path->valuestring, paths);
  removed = unlink(paths->target) == 0;
  removed = unlink(paths->part) == 0 || removed;
  unlink(paths->sidecar);
  unlink(paths->digest);
  ny_models_prune(paths->target);
  ny_models_release();
  free(paths);

  *result = ny_models_list();
  if (*result == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(*result, "removed", removed);
  return 0;
}

/****************************************************************************
 * Name: ny_models_verify_worker
 *
 * Description:
 *   Hash one model.  Reading most of a gigabyte takes longer than a panel
 *   waits for an answer, so this runs on its own thread and models.status
 *   reports how it is going.  The caller took the claim; it is given back
 *   here.
 *
 ****************************************************************************/

static void *ny_models_verify_worker(void *argument)
{
  struct ny_models_verify_s *job = argument;
  struct ny_models_paths_s *paths = malloc(sizeof(*paths));
  char actual[NY_MODELS_SHA256_HEX + 1];
  char expected[NY_MODELS_SHA256_HEX + 1];
  const char *reason = "";
  uint64_t size = 0;
  bool recorded = false;
  int ret = -ENOMEM;

  actual[0] = '\0';
  expected[0] = '\0';
  if (paths != NULL)
    {
      /* job->path is only written under the claim, which this holds. */

      ny_models_paths(job->path, paths);
      ret = ny_models_digest(paths->target, actual, &size, &job->done);
      if (ret == 0)
        {
          recorded = ny_models_digest_load(paths->digest, expected) == 0;
          if (!recorded)
            {
              /* A file that came some other way has no record yet.  This
               * is its first digest, and what it is compared with later.
               */

              ny_models_text_save(paths->digest, actual);
            }
          else if (memcmp(actual, expected, NY_MODELS_SHA256_HEX) != 0)
            {
              reason = "mismatch";
              ret = -EBADMSG;
            }
        }

      free(paths);
    }

  if (ret < 0 && reason[0] == '\0')
    {
      reason = ret == -ENOMEM ? "memory" : "io";
    }

  nxmutex_lock(&g_models_lock);
  job->state = ret == 0 ? NY_MODELS_VERIFY_DONE : NY_MODELS_VERIFY_FAILED;
  job->reason = reason;
  job->total = size > 0 ? size : job->total;
  strlcpy(job->sha256, actual, sizeof(job->sha256));
  strlcpy(job->expected, recorded ? expected : "", sizeof(job->expected));
  nxmutex_unlock(&g_models_lock);
  ny_models_release();
  return NULL;
}

/****************************************************************************
 * Name: ny_models_verify
 ****************************************************************************/

static int ny_models_verify(const cJSON *data, cJSON **result)
{
  const cJSON *path = cJSON_GetObjectItemCaseSensitive(data, "path");
  char file[NY_MODELS_FILE_MAX];
  pthread_attr_t attributes;
  pthread_t thread;
  uint64_t size;
  int ret;

  if (!cJSON_IsString(path) || !ny_models_path_ok(path->valuestring))
    {
      return -EINVAL;
    }

  ret = ny_models_claim();
  if (ret < 0)
    {
      return ret;
    }

  snprintf(file, sizeof(file), "%s/%s", NY_WEB_MODELS_ROOT,
           path->valuestring);
  size = ny_models_file_size(file);

  nxmutex_lock(&g_models_lock);
  memset(&g_models_verify, 0, sizeof(g_models_verify));
  strlcpy(g_models_verify.path, path->valuestring,
          sizeof(g_models_verify.path));
  g_models_verify.total = size;
  g_models_verify.reason = size > 0 ? "" : "missing";
  g_models_verify.state = size > 0 ? NY_MODELS_VERIFY_RUNNING
                                   : NY_MODELS_VERIFY_FAILED;
  nxmutex_unlock(&g_models_lock);

  if (size > 0)
    {
      pthread_attr_init(&attributes);
      pthread_attr_setstacksize(&attributes, NY_MODELS_WORKER_STACK);
      pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
      ret = pthread_create(&thread, &attributes, ny_models_verify_worker,
                           &g_models_verify);
      pthread_attr_destroy(&attributes);
      if (ret != 0)
        {
          nxmutex_lock(&g_models_lock);
          g_models_verify.state = NY_MODELS_VERIFY_FAILED;
          g_models_verify.reason = "memory";
          nxmutex_unlock(&g_models_lock);
        }
    }

  if (size == 0 || ret != 0)
    {
      /* No worker will give the claim back. */

      ny_models_release();
    }

  *result = ny_models_status();
  return *result == NULL ? -ENOMEM : 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_web_models_claims
 ****************************************************************************/

bool ny_web_models_claims(const char *head)
{
  size_t prefix = sizeof(NY_MODELS_PREFIX) - 1;
  const char *target = strchr(head, ' ');

  if (target == NULL)
    {
      return false;
    }

  target++;
  if (strncmp(target, NY_MODELS_PREFIX, prefix) != 0)
    {
      return false;
    }

  /* Everything below "/models", but neither "/modelszoo" nor "/models"
   * itself: that one is a route of the page, and a reload of it has to get
   * the page.
   */

  return target[prefix] == '/';
}

/****************************************************************************
 * Name: ny_web_models_serve
 ****************************************************************************/

int ny_web_models_serve(int fd, const char *head, const void *body,
                        size_t body_length, const char *pair_token)
{
  const char *target = strchr(head, ' ');
  size_t method_length;
  size_t target_length;

  if (target == NULL)
    {
      return ny_models_refuse(fd, 400, "Bad Request", "EINVAL");
    }

  method_length = (size_t)(target - head);
  target++;
  target_length = strcspn(target, " ?#");

  /* Anything else under the prefix does not exist, and says so in the form
   * the caller parses.  The page fallback of the file server would answer
   * 200 with HTML, which a client waiting for JSON cannot tell from success.
   */

  if (target_length != sizeof(NY_MODELS_TARGET) - 1 ||
      strncmp(target, NY_MODELS_TARGET, target_length) != 0)
    {
      return ny_models_refuse(fd, 404, "Not Found", "ENOTFOUND");
    }

  /* Who is asking comes before everything about the file, so that a
   * stranger learns nothing from the order of the refusals.
   */

  if (!ny_models_authorized(head, pair_token))
    {
      return ny_models_refuse(fd, 401, "Unauthorized", "EAUTH");
    }

  if (method_length == 3 && strncmp(head, "PUT", 3) == 0)
    {
      return ny_models_upload(fd, head, target + target_length, body,
                              body_length);
    }

  if (method_length == 3 && strncmp(head, "GET", 3) == 0)
    {
      return ny_models_state(fd, target + target_length);
    }

  return ny_models_refuse(fd, 405, "Method Not Allowed", "EMETHOD");
}

/****************************************************************************
 * Name: ny_web_models_request
 ****************************************************************************/

int ny_web_models_request(const struct ny_product_caller_s *caller,
                          const char *topic, const struct cJSON *data,
                          struct cJSON **result)
{
  bool owner = caller->role == NY_PRODUCT_OWNER;

  if (strcmp(topic, "models.list") == 0)
    {
      *result = ny_models_list();
      return *result == NULL ? -ENOMEM : 0;
    }

  if (strcmp(topic, "models.status") == 0)
    {
      *result = ny_models_status();
      return *result == NULL ? -ENOMEM : 0;
    }

  if (strcmp(topic, "models.delete") == 0)
    {
      return owner ? ny_models_remove(data, result) : -EACCES;
    }

  if (strcmp(topic, "models.verify") == 0)
    {
      return owner ? ny_models_verify(data, result) : -EACCES;
    }

  return -ENOSYS;
}
