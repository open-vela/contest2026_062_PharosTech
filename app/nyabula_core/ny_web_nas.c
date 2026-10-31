/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_web_nas.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
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

#include "ny_web_nas.h"
#include "ny_nas.h"
#include "ny_web.h"
#include "ny_web_auth.h"
#include "ny_websocket.h"
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#define NY_NAS_HTTP_CHUNK    16384
#define NY_NAS_HTTP_STALL_MS 15000

static int ny_web_nas_error(int fd, int error);
static int ny_web_nas_number(const char *value, size_t length,
                             uint64_t *number);
static int ny_web_nas_path(const char *target, size_t length, char *path);
static int ny_web_nas_range(const char *head, uint64_t size, uint64_t *first,
                            uint64_t *last);
static int ny_web_nas_upload(int fd, const char *head, const char *path,
                             const void *body, size_t body_length);
static int ny_web_nas_download(int fd, const char *head, const char *path,
                               bool headers_only);

/****************************************************************************
 * Name: ny_web_nas_error
 ****************************************************************************/

static int ny_web_nas_error(int fd, int error)
{
  int code = error == -EACCES                        ? 403
             : error == -ENOENT                      ? 404
             : error == -EROFS                       ? 403
             : error == -EEXIST                      ? 409
             : error == -EBUSY                       ? 423
             : error == -ENOSPC                      ? 507
             : error == -ENODEV                      ? 503
             : error == -EFBIG                       ? 413
             : error == -EINVAL || error == -EBADMSG ? 400
                                                     : 500;
  char body[64], header[256];
  int length = snprintf(body, sizeof(body), "{\"error\":%d}", error);
  int used = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d Error\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n",
                      code, length);
  int ret = ny_web_http_write(fd, header, used);
  return ret < 0 ? ret : ny_web_http_write(fd, body, length);
}

/****************************************************************************
 * Name: ny_web_nas_number
 ****************************************************************************/

static int ny_web_nas_number(const char *value, size_t length,
                             uint64_t *number)
{
  if (!value || !length)
    return -EINVAL;
  uint64_t result = 0;
  for (size_t i = 0; i < length; i++)
    {
      if (value[i] < '0' || value[i] > '9' ||
          result > (UINT64_MAX - (unsigned int)(value[i] - '0')) / 10)
        return -EINVAL;
      result = result * 10 + (unsigned int)(value[i] - '0');
    }
  *number = result;
  return 0;
}

/****************************************************************************
 * Name: ny_web_nas_path
 ****************************************************************************/

static int ny_web_nas_path(const char *target, size_t length, char *path)
{
  static const char prefix[] = "/nas/file?path=";
  if (length <= sizeof(prefix) - 1 ||
      memcmp(target, prefix, sizeof(prefix) - 1))
    return -EINVAL;
  size_t used = 0;
  for (size_t i = sizeof(prefix) - 1; i < length; i++)
    {
      unsigned char c = target[i];
      if (c == '&' || c == '#' || c == '+' || used >= NY_NAS_PATH_MAX)
        return -EINVAL;
      if (c == '%')
        {
          if (i + 2 >= length)
            return -EINVAL;
          char digits[3] = { target[i + 1], target[i + 2], 0 };
          if (strspn(digits, "0123456789abcdefABCDEF") != 2)
            return -EINVAL;
          c = (unsigned char)strtoul(digits, NULL, 16);
          i += 2;
        }
      if (!c || c < 32 || c == 127)
        return -EINVAL;
      path[used++] = c;
    }
  path[used] = '\0';
  return 0;
}

/****************************************************************************
 * Name: ny_web_nas_range
 ****************************************************************************/

static int ny_web_nas_range(const char *head, uint64_t size, uint64_t *first,
                            uint64_t *last)
{
  size_t length = 0;
  const char *range = ny_web_http_header(head, "Range", &length);
  *first = 0;
  *last = size ? size - 1 : 0;
  if (!range)
    return 0;
  if (!size || length < 7 || strncmp(range, "bytes=", 6))
    return -EINVAL;
  const char *dash = memchr(range + 6, '-', length - 6);
  if (!dash)
    return -EINVAL;
  size_t left = dash - range - 6, right = length - (dash - range) - 1;
  uint64_t a = 0, b = 0;
  if (!left)
    {
      if (ny_web_nas_number(dash + 1, right, &b) < 0 || !b)
        return -EINVAL;
      *first = b >= size ? 0 : size - b;
    }
  else
    {
      if (ny_web_nas_number(range + 6, left, &a) < 0 || a >= size ||
          (right && ny_web_nas_number(dash + 1, right, &b) < 0))
        return -EINVAL;
      *first = a;
      if (right && b < *last)
        *last = b;
      if (*last < *first)
        return -EINVAL;
    }
  return 1;
}

/****************************************************************************
 * Name: ny_web_nas_upload
 ****************************************************************************/

static int ny_web_nas_upload(int fd, const char *head, const char *path,
                             const void *body, size_t body_length)
{
  size_t length = 0;
  const char *field = ny_web_http_header(head, "Content-Length", &length);
  uint64_t expected, size;
  if (ny_web_nas_number(field, length, &expected) < 0 ||
      ny_web_http_header(head, "Transfer-Encoding", &length) ||
      body_length > expected)
    return ny_web_nas_error(fd, -EINVAL);
  struct ny_nas_file_s *file = NULL;
  int ret = ny_nas_file_open(path, true, expected, &file, &size);
  if (ret < 0)
    return ny_web_nas_error(fd, ret);
  unsigned char *buffer = malloc(NY_NAS_HTTP_CHUNK);
  if (!buffer)
    ret = -ENOMEM;
  uint64_t received = 0;
  uint64_t deadline = nyabula_eye_ws_now() + NY_NAS_HTTP_STALL_MS;
  const unsigned char *part = body;
  size_t count = body_length;
  while (ret == 0 && received < expected)
    {
      if (count)
        {
          size_t done = 0;
          while (done < count)
            {
              ssize_t written =
                  ny_nas_file_write(file, part + done, count - done);
              if (written == -EINTR)
                continue;
              if (written <= 0)
                {
                  ret = written < 0 ? (int)written : -EIO;
                  break;
                }
              done += written;
            }
          received += done;
          deadline = nyabula_eye_ws_now() + NY_NAS_HTTP_STALL_MS;
          count = 0;
          if (ret < 0 || received == expected)
            break;
        }
      uint64_t now = nyabula_eye_ws_now();
      if (now >= deadline)
        {
          ret = -ETIMEDOUT;
          break;
        }
      struct pollfd poller = { .fd = fd, .events = POLLIN };
      int ready = poll(&poller, 1, (int)(deadline - now));
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready <= 0)
        {
          ret = ready == 0 ? -ETIMEDOUT : -errno;
          break;
        }
      size_t wanted = expected - received < NY_NAS_HTTP_CHUNK
                          ? (size_t)(expected - received)
                          : NY_NAS_HTTP_CHUNK;
      ssize_t got = recv(fd, buffer, wanted, MSG_DONTWAIT);
      if (got < 0 && (errno == EAGAIN || errno == EINTR))
        continue;
      if (got <= 0)
        {
          ret = got == 0 ? -ECONNRESET : -errno;
          break;
        }
      part = buffer;
      count = got;
    }
  int closed = ny_nas_file_close(file, ret == 0 && received == expected);
  free(buffer);
  if (ret == 0)
    ret = closed;
  if (ret < 0)
    return ny_web_nas_error(fd, ret);
  const char *response =
      "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
  return ny_web_http_write(fd, response, strlen(response));
}

/****************************************************************************
 * Name: ny_web_nas_download
 ****************************************************************************/

static int ny_web_nas_download(int fd, const char *head, const char *path,
                               bool headers_only)
{
  struct ny_nas_file_s *file = NULL;
  uint64_t size, first, last;
  int ret = ny_nas_file_open(path, false, 0, &file, &size);
  if (ret < 0)
    return ny_web_nas_error(fd, ret);
  int ranged = ny_web_nas_range(head, size, &first, &last);
  char header[512];
  if (ranged < 0)
    {
      ny_nas_file_close(file, false);
      int count = snprintf(
          header, sizeof(header),
          "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes "
          "*/%" PRIu64 "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
          size);
      return ny_web_http_write(fd, header, count);
    }
  uint64_t remaining = size ? last - first + 1 : 0;
  char range[128] = "";
  if (ranged)
    snprintf(range, sizeof(range),
             "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n",
             first, last, size);
  int count =
      snprintf(header, sizeof(header),
               "HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\n"
               "Content-Disposition: attachment\r\nContent-Length: %" PRIu64
               "\r\nAccept-Ranges: bytes\r\n%sCache-Control: no-store\r\n"
               "Connection: close\r\n\r\n",
               ranged ? "206 Partial Content" : "200 OK", remaining, range);
  ret = ny_nas_file_seek(file, first);
  if (ret == 0)
    ret = ny_web_http_write(fd, header, count);
  unsigned char *buffer =
      !headers_only && ret == 0 ? malloc(NY_NAS_HTTP_CHUNK) : NULL;
  if (!headers_only && ret == 0 && !buffer)
    ret = -ENOMEM;
  while (ret == 0 && !headers_only && remaining)
    {
      size_t wanted =
          remaining < NY_NAS_HTTP_CHUNK ? remaining : NY_NAS_HTTP_CHUNK;
      ssize_t got = ny_nas_file_read(file, buffer, wanted);
      if (got == -EINTR)
        continue;
      if (got <= 0)
        {
          ret = got < 0 ? (int)got : -EIO;
          break;
        }
      ret = ny_web_http_write(fd, buffer, got);
      remaining -= got;
    }
  free(buffer);
  int closed = ny_nas_file_close(file, false);
  return ret < 0 ? ret : closed;
}

/****************************************************************************
 * Name: ny_web_nas_claims
 ****************************************************************************/

bool ny_web_nas_claims(const char *head)
{
  const char *target = strchr(head, ' ');
  return target && !strncmp(target + 1, "/nas/", 5);
}

/****************************************************************************
 * Name: ny_web_nas_serve
 ****************************************************************************/

int ny_web_nas_serve(int fd, const char *head, const void *body,
                     size_t body_length, const char *pair_token)
{
  size_t length = 0;
  const char *authorization =
      ny_web_http_header(head, "Authorization", &length);
  bool authorized =
      authorization && length == 7 + NY_WEB_AUTH_TOKEN_SIZE &&
      !strncasecmp(authorization, "Bearer ", 7) &&
      ny_web_auth_accept_token(pair_token, authorization + 7, length - 7);
  const char *start = strchr(head, ' ');
  const char *end = start ? strchr(start + 1, ' ') : NULL;
  const char *query = end ? memchr(start + 1, '&', end - start - 1) : NULL;
  char path[NY_NAS_PATH_MAX + 1];
  int parsed =
      start && end
          ? ny_web_nas_path(start + 1, (query ? query : end) - start - 1, path)
          : -EINVAL;
  bool reading = !strncmp(head, "GET ", 4) || !strncmp(head, "HEAD ", 5);
  if (!authorized && reading && parsed == 0 && query &&
      end - query == 8 + 64 && !memcmp(query, "&ticket=", 8))
    {
      char ticket[65];
      memcpy(ticket, query + 8, 64);
      ticket[64] = '\0';
      authorized = ny_nas_ticket_authorized(path, ticket);
      memset(ticket, 0, sizeof(ticket));
    }
  if (!authorized)
    {
      const char *response =
          "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n"
          "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
      return ny_web_http_write(fd, response, strlen(response));
    }
  if (parsed < 0 ||
      (query && (end - query != 8 + 64 || memcmp(query, "&ticket=", 8))))
    return ny_web_nas_error(fd, -EINVAL);
  if (!strncmp(head, "GET ", 4) || !strncmp(head, "HEAD ", 5))
    return ny_web_nas_download(fd, head, path, !strncmp(head, "HEAD ", 5));
  if (!strncmp(head, "PUT ", 4))
    return ny_web_nas_upload(fd, head, path, body, body_length);
  const char *response = "HTTP/1.1 405 Method Not Allowed\r\n"
                         "Allow: GET, HEAD, PUT\r\nContent-Length: 0\r\n"
                         "Connection: close\r\n\r\n";
  return ny_web_http_write(fd, response, strlen(response));
}
