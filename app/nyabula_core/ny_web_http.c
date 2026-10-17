/****************************************************************************
 * app/nyabula_core/ny_web_http.c
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

/* The control panel is a static single-page site, and the device is the
 * only thing a phone on the provisioning access point can reach.  So the
 * device serves it, from the same port as the WebSocket: the page's origin
 * is then the host it calls, which is the rule the WebSocket enforces.
 *
 * This is a file server and nothing more.  It reads files under one root,
 * answers GET and HEAD, and closes the connection.  Nothing here acts on
 * the device; everything that does goes through the authenticated socket.
 *
 * The one exception is handed on before any of that: a firmware image is
 * too large for a socket that carries 32 KiB text messages, so requests
 * under /ota/ go to ny_web_ota.c, which checks the same credentials itself.
 */

#include "ny_web.h"
#include "ny_web_models.h"
#include "ny_web_ota.h"
#include "ny_websocket.h"
#include <errno.h>
#include <fcntl.h>
#include <nuttx/config.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_HTTP_ROOT     CONFIG_NYABULA_CORE_WEB_ROOT
#define NY_HTTP_PATH_MAX 192
#define NY_HTTP_CHUNK    8192
#define NY_HTTP_SEND_MS  15000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_http_type_s
{
  const char *extension;
  const char *type;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_http_status(int fd, int code, const char *reason,
                          const char *extra);
static const char *ny_http_type(const char *path);
static bool ny_http_path_ok(const char *path);
static bool ny_http_accepts_gzip(const char *head);
static bool ny_http_is_probe(const char *path);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct ny_http_type_s g_http_types[] = {
  { ".html", "text/html; charset=utf-8" },
  { ".js", "text/javascript; charset=utf-8" },
  { ".css", "text/css; charset=utf-8" },
  { ".json", "application/json" },
  { ".svg", "image/svg+xml" },
  { ".png", "image/png" },
  { ".ico", "image/x-icon" },
  { ".wasm", "application/wasm" },
  { ".txt", "text/plain; charset=utf-8" },
};

/* What phones and laptops fetch to decide whether a network has a sign-in
 * page.  Redirecting them to the panel is what turns "connected, no
 * internet" into a prompt to open it.
 */

static const char *const g_http_probes[] = {
  "/generate_204",        "/gen_204",
  "/hotspot-detect.html", "/ncsi.txt",
  "/connecttest.txt",     "/canonical.html",
  "/success.txt",         "/library/test/success.html",
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_http_status
 ****************************************************************************/

static int ny_http_status(int fd, int code, const char *reason,
                          const char *extra)
{
  char buffer[384];
  int count = snprintf(buffer, sizeof(buffer),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Length: 0\r\n"
                       "Cache-Control: no-store\r\n"
                       "Connection: close\r\n"
                       "%s\r\n",
                       code, reason, extra != NULL ? extra : "");
  if (count < 0 || (size_t)count >= sizeof(buffer))
    {
      return -ENOBUFS;
    }

  return ny_web_http_write(fd, buffer, count);
}

/****************************************************************************
 * Name: ny_http_type
 ****************************************************************************/

static const char *ny_http_type(const char *path)
{
  const char *dot = strrchr(path, '.');
  if (dot != NULL)
    {
      for (size_t i = 0; i < sizeof(g_http_types) / sizeof(g_http_types[0]);
           i++)
        {
          if (strcasecmp(dot, g_http_types[i].extension) == 0)
            {
              return g_http_types[i].type;
            }
        }
    }

  return "application/octet-stream";
}

/****************************************************************************
 * Name: ny_http_path_ok
 *
 * Description:
 *   Accept only the paths a build of the panel can produce.
 *
 *   The list is of what is allowed rather than of what is dangerous.  Site
 *   assets are letters, digits, dot, dash and underscore between slashes,
 *   so that is all that passes -- which leaves no percent-encoding to
 *   decode and no second spelling of "..", and makes "is this inside the
 *   root" a question about the string rather than about the filesystem.
 *
 ****************************************************************************/

static bool ny_http_path_ok(const char *path)
{
  if (path[0] != '/')
    {
      return false;
    }

  for (const char *c = path; *c != '\0'; c++)
    {
      bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                   (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
      if (plain)
        {
          continue;
        }

      /* A dot may not start a segment, which covers "." and ".." and any
       * hidden file; a slash may not follow a slash.
       */

      if (*c == '.' && c[-1] != '/')
        {
          continue;
        }

      if (*c == '/' && (c == path || c[-1] != '/'))
        {
          continue;
        }

      return false;
    }

  return true;
}

/****************************************************************************
 * Name: ny_http_accepts_gzip
 ****************************************************************************/

static bool ny_http_accepts_gzip(const char *head)
{
  size_t length;
  const char *value = ny_web_http_header(head, "Accept-Encoding", &length);
  for (size_t i = 0; value != NULL && i + 4 <= length; i++)
    {
      if (strncasecmp(value + i, "gzip", 4) == 0)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: ny_http_is_probe
 ****************************************************************************/

static bool ny_http_is_probe(const char *path)
{
  for (size_t i = 0; i < sizeof(g_http_probes) / sizeof(g_http_probes[0]); i++)
    {
      if (strcmp(path, g_http_probes[i]) == 0)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_web_http_write
 *
 * Description:
 *   Send a whole buffer, giving up if the peer stops reading.  A phone that
 *   walks out of range mid-transfer must not hold one of the few client
 *   slots for ever.
 *
 ****************************************************************************/

int ny_web_http_write(int fd, const void *data, size_t length)
{
  const char *cursor = data;
  uint64_t deadline = nyabula_eye_ws_now() + NY_HTTP_SEND_MS;
  while (length > 0)
    {
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd = { fd, POLLOUT, 0 };
      if (now >= deadline)
        {
          return -ETIMEDOUT;
        }

      int ready = poll(&pfd, 1, (int)(deadline - now));
      if (ready < 0 && errno == EINTR)
        {
          continue;
        }

      if (ready <= 0)
        {
          return ready == 0 ? -ETIMEDOUT : -errno;
        }

      ssize_t count = send(fd, cursor, length, MSG_DONTWAIT);
      if (count < 0 && (errno == EAGAIN || errno == EINTR))
        {
          continue;
        }

      if (count <= 0)
        {
          return count == 0 ? -ECONNRESET : -errno;
        }

      cursor += count;
      length -= count;

      /* Progress buys more time: the limit is on a stalled peer, not on a
       * large file over a slow link.
       */

      deadline = nyabula_eye_ws_now() + NY_HTTP_SEND_MS;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_web_http_header
 *
 * Description:
 *   Find a header in a raw request head.  Returns a pointer to its value
 *   and the value's length, or NULL.
 *
 ****************************************************************************/

const char *ny_web_http_header(const char *head, const char *name,
                               size_t *length)
{
  size_t name_length = strlen(name);
  const char *line = strstr(head, "\r\n");
  while (line != NULL && line[2] != '\r' && line[2] != '\0')
    {
      line += 2;
      if (strncasecmp(line, name, name_length) == 0 &&
          line[name_length] == ':')
        {
          const char *value = line + name_length + 1;
          const char *end = strstr(value, "\r\n");
          while (*value == ' ' || *value == '\t')
            {
              value++;
            }

          *length = end != NULL ? (size_t)(end - value) : strlen(value);
          return value;
        }

      line = strstr(line, "\r\n");
    }

  return NULL;
}

/****************************************************************************
 * Name: ny_web_http_serve
 ****************************************************************************/

int ny_web_http_serve(int fd, const char *head, const void *body,
                      size_t body_length, const char *pair_token)
{
  char target[NY_HTTP_PATH_MAX];
  char path[sizeof(NY_HTTP_ROOT) + NY_HTTP_PATH_MAX + 16];
  char header[384];
  struct stat info;
  const char *space;
  const char *type;
  bool head_only;
  bool gzip = false;
  int file;
  int ret;

#ifdef CONFIG_NYABULA_CORE_MODELS
  /* Model files are larger still and arrive in pieces.  Like the firmware
   * upload below, ny_web_models.c checks the credentials itself.
   */

  if (ny_web_models_claims(head))
    {
      return ny_web_models_serve(fd, head, body, body_length, pair_token);
    }
#endif

#ifdef CONFIG_NYABULA_CORE_OTA
  /* The firmware upload is the one request here that takes a body and acts
   * on the device, so it is decided before anything below can treat its
   * path as a file name or as a route of the page.
   */

  if (ny_web_ota_claims(head))
    {
      return ny_web_ota_serve(fd, head, body, body_length, pair_token);
    }
#else
  /* Nothing else reads a body: what came with the head is dropped, and the
   * rest goes with the connection.
   */

  (void)body;
  (void)body_length;
  (void)pair_token;
#endif

  if (strncmp(head, "GET ", 4) == 0)
    {
      head_only = false;
      head += 4;
    }
  else if (strncmp(head, "HEAD ", 5) == 0)
    {
      head_only = true;
      head += 5;
    }
  else
    {
      return ny_http_status(fd, 405, "Method Not Allowed",
                            "Allow: GET, HEAD\r\n");
    }

  /* The target runs to the next space; a query string is not part of the
   * file name.
   */

  space = strchr(head, ' ');
  if (space == NULL || (size_t)(space - head) >= sizeof(target))
    {
      return ny_http_status(fd, 414, "URI Too Long", NULL);
    }

  memcpy(target, head, space - head);
  target[space - head] = '\0';
  target[strcspn(target, "?#")] = '\0';

  if (!ny_http_path_ok(target))
    {
      return ny_http_status(fd, 400, "Bad Request", NULL);
    }

  if (ny_http_is_probe(target))
    {
      /* Relative, so it is right on whichever address was called. */

      return ny_http_status(fd, 302, "Found", "Location: /\r\n");
    }

  /* The panel also talks to a cloud API under this prefix.  There is none
   * here, and saying so plainly matters: the fallback below would answer
   * with the page itself, 200 OK, to a client that expects JSON.
   */

  if (strncmp(target, "/api/", 5) == 0)
    {
      return ny_http_status(fd, 404, "Not Found", NULL);
    }

  if (strcmp(target, "/") == 0)
    {
      strlcpy(target, "/index.html", sizeof(target));
    }

  type = ny_http_type(target);
  snprintf(path, sizeof(path), "%s%s", NY_HTTP_ROOT, target);

  /* A build step may leave a compressed twin beside each text asset.  It is
   * preferred when the client can take it: the store is slow and the link
   * is slower, and a desk pet has better uses for its one core than
   * deflating the same script for every visitor.
   */

  if (ny_http_accepts_gzip(space))
    {
      size_t end = strlen(path);
      strlcat(path, ".gz", sizeof(path));
      if (stat(path, &info) == 0 && S_ISREG(info.st_mode))
        {
          gzip = true;
        }
      else
        {
          path[end] = '\0';
        }
    }

  if (!gzip && (stat(path, &info) < 0 || !S_ISREG(info.st_mode)))
    {
      /* A path with no extension is a route inside the page, not a file,
       * and gets the page.  A missing asset is simply missing.
       */

      if (strchr(strrchr(target, '/'), '.') != NULL)
        {
          return ny_http_status(fd, 404, "Not Found", NULL);
        }

      type = "text/html; charset=utf-8";
      snprintf(path, sizeof(path), "%s/index.html", NY_HTTP_ROOT);
      if (stat(path, &info) < 0 || !S_ISREG(info.st_mode))
        {
          return ny_http_status(fd, 503, "Service Unavailable",
                                "Retry-After: 5\r\n");
        }
    }

  file = open(path, O_RDONLY | O_CLOEXEC);
  if (file < 0)
    {
      return ny_http_status(fd, 404, "Not Found", NULL);
    }

  /* Built assets carry a content hash in their name and never change; the
   * entry page names them and must always be fetched again.
   */

  ret = snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %ld\r\n"
                 "%s"
                 "Cache-Control: %s\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "Connection: close\r\n\r\n",
                 type, (long)info.st_size,
                 gzip ? "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"
                      : "",
                 strncmp(target, "/assets/", 8) == 0
                     ? "public, max-age=31536000, immutable"
                     : "no-cache");
  if (ret < 0 || (size_t)ret >= sizeof(header))
    {
      close(file);
      return -ENOBUFS;
    }

  ret = ny_web_http_write(fd, header, ret);
  if (ret == 0 && !head_only)
    {
      char *chunk = malloc(NY_HTTP_CHUNK);
      if (chunk == NULL)
        {
          ret = -ENOMEM;
        }

      while (ret == 0)
        {
          ssize_t count = read(file, chunk, NY_HTTP_CHUNK);
          if (count < 0 && errno == EINTR)
            {
              continue;
            }

          if (count <= 0)
            {
              ret = count < 0 ? -errno : 0;
              break;
            }

          ret = ny_web_http_write(fd, chunk, count);
        }

      free(chunk);
    }

  close(file);
  return ret;
}
