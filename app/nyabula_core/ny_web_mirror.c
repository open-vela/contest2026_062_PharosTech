/****************************************************************************
 * app/nyabula_core/ny_web_mirror.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include "nyabula_eye_mirror.h"
#include "ny_web.h"
#include "ny_web_auth.h"
#include "ny_web_mirror.h"

#ifdef CONFIG_NYABULA_CORE_WEB_EYEPROBE
#  include "ny_web_eyeprobe_page.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_MIRROR_PATH          "/eyes/stream"
#define NY_MIRROR_PROBE_PAGE    "/eyeprobe"
#define NY_MIRROR_PROBE_STREAM  "/eyeprobe/stream"
#define NY_MIRROR_BEARER        "Bearer "
#define NY_MIRROR_BEARER_SIZE   7

#define NY_MIRROR_FPS_DEFAULT   12
#define NY_MIRROR_FPS_MAX       20

/* How long one wait for a new page lasts, and after how many of them with
 * nothing to send the viewer is told that the stream is still alive.  The
 * end of a wait is also when a viewer that went away is noticed.
 */

#define NY_MIRROR_WAIT_MS       500
#define NY_MIRROR_IDLE_WAITS    4

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool ny_mirror_token_equal(const char *offered, const char *expected);
static bool ny_mirror_authorized(const char *head, const char *pair_token);
static int ny_mirror_query(const char *head, const char *name, int fallback);
static int ny_mirror_reply(int fd, int code, const char *reason,
                           const char *json);
static bool ny_mirror_peer_gone(int fd);
static bool ny_mirror_target_is(const char *head, const char *path);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_mirror_token_equal
 *
 * Description:
 *   Compare every byte whatever the outcome, so that the time taken says
 *   nothing about where the first difference is.
 *
 ****************************************************************************/

static bool ny_mirror_token_equal(const char *offered, const char *expected)
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
 * Name: ny_mirror_authorized
 *
 * Description:
 *   The pair token shown on the eyes, or the session token a correct
 *   password was exchanged for: what opens the socket opens this too.
 *   Authorization is not a header another site can make the owner's
 *   browser send without a preflight, and nothing here answers one.
 *
 ****************************************************************************/

static bool ny_mirror_authorized(const char *head, const char *pair_token)
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
      length != NY_MIRROR_BEARER_SIZE + NY_WEB_AUTH_TOKEN_SIZE ||
      strncasecmp(value, NY_MIRROR_BEARER, NY_MIRROR_BEARER_SIZE) != 0)
    {
      return false;
    }

  value += NY_MIRROR_BEARER_SIZE;
  valid = ny_mirror_token_equal(value, pair_token);
  if (ny_web_auth_session_token(pair_token, session) == 0)
    {
      bool match = ny_mirror_token_equal(value, session);
      valid = valid || match;
    }

  memset(session, 0, sizeof(session));
  return valid;
}

/****************************************************************************
 * Name: ny_mirror_query
 *
 * Description:
 *   A small non-negative number from the query of the request line.
 *
 ****************************************************************************/

static int ny_mirror_query(const char *head, const char *name, int fallback)
{
  const char *end = strstr(head, "\r\n");
  const char *at = strchr(head, '?');
  size_t size = strlen(name);

  while (at != NULL && (end == NULL || at < end))
    {
      at++;
      if (strncmp(at, name, size) == 0 && at[size] == '=')
        {
          int value = 0;

          at += size + 1;
          while (*at >= '0' && *at <= '9' && value < 1000)
            {
              value = value * 10 + (*at++ - '0');
            }

          return value;
        }

      at = strchr(at, '&');
    }

  return fallback;
}

static int ny_mirror_reply(int fd, int code, const char *reason,
                           const char *json)
{
  char buffer[256];
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
 * Name: ny_mirror_peer_gone
 *
 * Description:
 *   A viewer sends nothing after its request, so whatever can be read is
 *   either the end of the connection or of no interest.
 *
 ****************************************************************************/

static bool ny_mirror_peer_gone(int fd)
{
  char scratch[64];
  ssize_t count = recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT);

  return count == 0 ||
         (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR);
}

/****************************************************************************
 * Name: ny_mirror_target_is
 *
 * Description:
 *   Is the target of the request line `path`, with or without a query?
 *
 ****************************************************************************/

static bool ny_mirror_target_is(const char *head, const char *path)
{
  const char *target = strchr(head, ' ');
  size_t size = strlen(path);

  if (target == NULL || strncmp(target + 1, path, size) != 0)
    {
      return false;
    }

  target += 1 + size;
  return *target == ' ' || *target == '?';
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_web_mirror_claims
 ****************************************************************************/

bool ny_web_mirror_claims(const char *head)
{
#ifdef CONFIG_NYABULA_CORE_WEB_EYEPROBE
  if (ny_mirror_target_is(head, NY_MIRROR_PROBE_PAGE) ||
      ny_mirror_target_is(head, NY_MIRROR_PROBE_STREAM))
    {
      return true;
    }
#endif

  return ny_mirror_target_is(head, NY_MIRROR_PATH);
}

/****************************************************************************
 * Name: ny_web_mirror_serve
 ****************************************************************************/

int ny_web_mirror_serve(int fd, const char *head, const char *pair_token)
{
  static const char ok[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Cache-Control: no-store\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Connection: close\r\n\r\n";
  static const uint8_t keepalive[NYABULA_EYE_MIRROR_HEADER] =
  {
    'N', 'E', 'M', '1', NYABULA_EYE_MIRROR_KEEPALIVE
  };

  struct nyabula_eye_mirror_viewer_s *viewer;
  int scale;
  int fps;
  int idle = 0;
  int ret;

  if (strncmp(head, "GET ", 4) != 0)
    {
      return ny_mirror_reply(fd, 405, "Method Not Allowed",
                             "{\"error\":\"method\"}");
    }

#ifdef CONFIG_NYABULA_CORE_WEB_EYEPROBE
  if (ny_mirror_target_is(head, NY_MIRROR_PROBE_PAGE))
    {
      static const char page[] = NY_WEB_EYEPROBE_PAGE;
      char header[192];
      int count = snprintf(header, sizeof(header),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: %zu\r\n"
                           "Cache-Control: no-store\r\n"
                           "X-Content-Type-Options: nosniff\r\n"
                           "Connection: close\r\n\r\n",
                           sizeof(page) - 1);

      ret = ny_web_http_write(fd, header, count);
      return ret < 0 ? ret : ny_web_http_write(fd, page, sizeof(page) - 1);
    }

  /* What the panels show is for whoever stands in front of the device, and
   * this build says that whoever is on its network stands in front of it.
   */

  if (!ny_mirror_target_is(head, NY_MIRROR_PROBE_STREAM))
#endif
  if (!ny_mirror_authorized(head, pair_token))
    {
      return ny_mirror_reply(fd, 401, "Unauthorized",
                             "{\"error\":\"unauthorized\"}");
    }

  scale = ny_mirror_query(head, "scale", 1) == 2 ? 2 : 1;
  fps = ny_mirror_query(head, "fps", NY_MIRROR_FPS_DEFAULT);
  if (fps < 1 || fps > NY_MIRROR_FPS_MAX)
    {
      fps = NY_MIRROR_FPS_DEFAULT;
    }

  ret = nyabula_eye_mirror_open(scale, &viewer);
  if (ret < 0)
    {
      return ny_mirror_reply(fd, 503, "Service Unavailable",
                             ret == -EBUSY ? "{\"error\":\"busy\"}"
                                           : "{\"error\":\"memory\"}");
    }

  syslog(LOG_INFO, "nymirror: viewer attached, scale %d, %d fps\n", scale,
         fps);
  ret = ny_web_http_write(fd, ok, sizeof(ok) - 1);
  while (ret >= 0)
    {
      const uint8_t *data;
      size_t length;

      ret = nyabula_eye_mirror_next(viewer, NY_MIRROR_WAIT_MS, &data,
                                    &length);
      if (ret < 0)
        {
          break;
        }

      if (length > 0)
        {
          /* A send that blocks is the pacing of a slow link: whatever was
           * rendered meanwhile collapses into the one next difference.
           */

          idle = 0;
          ret = ny_web_http_write(fd, data, length);
          usleep(1000000 / fps);
        }
      else if (ny_mirror_peer_gone(fd))
        {
          break;
        }
      else if (++idle >= NY_MIRROR_IDLE_WAITS)
        {
          idle = 0;
          ret = ny_web_http_write(fd, keepalive, sizeof(keepalive));
        }
    }

  nyabula_eye_mirror_close(viewer);
  syslog(LOG_INFO, "nymirror: viewer detached: %d\n", ret);
  return ret;
}
