/****************************************************************************
 * app/nyabula_core/ny_websocket.c
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

/* Bounded RFC 6455 framing. No rendering, credentials, or Core state here. */

#include "ny_websocket.h"
#include "ny_utf8.h"
#include <crypto/sha1.h>
#include <errno.h>
#include <netutils/base64.h>
#include <nuttx/config.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

#define NYABULA_WS_IO_MS      2000
#define NYABULA_WS_HEADER_MAX 4096
#define NYABULA_WS_GUID       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static int nyabula_eye_ws_io(int fd, void *data, size_t length, bool writing,
                             uint64_t deadline);
static char *nyabula_eye_ws_header(char *headers, const char *name);
static bool nyabula_eye_ws_has_token(const char *value, const char *token);

/****************************************************************************
 * Name: nyabula_eye_ws_now
 ****************************************************************************/

uint64_t nyabula_eye_ws_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: nyabula_eye_ws_io
 ****************************************************************************/

static int nyabula_eye_ws_io(int fd, void *data, size_t length, bool writing,
                             uint64_t deadline)
{
  unsigned char *bytes = data;
  while (length > 0)
    {
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd = { fd, writing ? POLLOUT : POLLIN, 0 };
      ssize_t count;
      int ret;
      if (now >= deadline)
        {
          return -ETIMEDOUT;
        }

      ret = poll(&pfd, 1, (int)(deadline - now));
      if (ret < 0 && errno == EINTR)
        {
          continue;
        }

      if (ret <= 0)
        {
          return ret == 0 ? -ETIMEDOUT : -errno;
        }

      count = writing ? send(fd, bytes, length, MSG_DONTWAIT | MSG_NOSIGNAL)
                      : recv(fd, bytes, length, MSG_DONTWAIT);
      if (count < 0 && (errno == EAGAIN || errno == EINTR))
        {
          continue;
        }

      if (count <= 0)
        {
          return count == 0 ? -ECONNRESET : -errno;
        }

      bytes += count;
      length -= count;
    }

  return 0;
}

/****************************************************************************
 * Name: nyabula_eye_ws_header
 ****************************************************************************/

static char *nyabula_eye_ws_header(char *headers, const char *name)
{
  char *result = NULL;
  size_t size = strlen(name);
  for (char *line = headers; *line != '\0';)
    {
      char *next = line + strlen(line) + 1;
      if (strncasecmp(line, name, size) == 0 && line[size] == ':')
        {
          if (result != NULL)
            {
              return NULL;
            }

          result = line + size + 1;
          while (*result == ' ' || *result == '\t')
            {
              result++;
            }
        }

      line = next;
    }

  return result;
}

/****************************************************************************
 * Name: nyabula_eye_ws_has_token
 ****************************************************************************/

static bool nyabula_eye_ws_has_token(const char *value, const char *token)
{
  size_t size = strlen(token);
  if (value == NULL)
    {
      return false;
    }

  while (*value)
    {
      while (*value == ' ' || *value == '\t' || *value == ',')
        {
          value++;
        }

      const char *end = strchr(value, ',');
      size_t length = end == NULL ? strlen(value) : (size_t)(end - value);
      while (length > 0 &&
             (value[length - 1] == ' ' || value[length - 1] == '\t'))
        {
          length--;
        }

      if (length == size && strncasecmp(value, token, size) == 0)
        {
          return true;
        }

      if (end == NULL)
        {
          break;
        }

      value = end + 1;
    }

  return false;
}

/****************************************************************************
 * Name: nyabula_eye_ws_upgrade
 ****************************************************************************/

int nyabula_eye_ws_upgrade(int fd, const char *origin, char *scratch,
                           size_t capacity)
{
  size_t length = 0;
  size_t decoded = 0;
  uint64_t deadline = nyabula_eye_ws_now() + NYABULA_WS_IO_MS;
  unsigned char digest[SHA1_DIGEST_LENGTH];
  unsigned char keybytes[32];
  char accept[64];
  char *key;
  char *version;
  char *actual_origin;
  char *host;
  bool same_host = false;
  SHA1_CTX sha;
  if (capacity < NYABULA_WS_HEADER_MAX)
    {
      return -ENOBUFS;
    }

  while (length + 1 < NYABULA_WS_HEADER_MAX)
    {
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd = { fd, POLLIN, 0 };
      ssize_t count;
      int ready;
      if (now >= deadline)
        {
          return -ETIMEDOUT;
        }

      ready = poll(&pfd, 1, (int)(deadline - now));
      if (ready < 0 && errno == EINTR)
        {
          continue;
        }

      if (ready <= 0)
        {
          return ready == 0 ? -ETIMEDOUT : -errno;
        }

      count = recv(fd, scratch + length, NYABULA_WS_HEADER_MAX - length - 1,
                   MSG_DONTWAIT);
      if (count < 0 && (errno == EAGAIN || errno == EINTR))
        {
          continue;
        }

      if (count <= 0)
        {
          return count == 0 ? -ECONNRESET : -errno;
        }

      if (memchr(scratch + length, '\0', count) != NULL)
        {
          return -EPROTO;
        }

      length += count;
      scratch[length] = '\0';
      char *end = strstr(scratch, "\r\n\r\n");
      if (end != NULL)
        {
          /* A client must await the upgrade response before sending data. */

          if (end + 4 != scratch + length)
            {
              return -EPROTO;
            }

          break;
        }
    }

  if (length < 4 || memcmp(scratch + length - 4, "\r\n\r\n", 4) != 0)
    {
      return -EPROTO;
    }

  /* A well-formed request for anything else is not an error, just not
   * ours.  The head is handed back untouched so the caller can serve it as
   * plain HTTP on the same port.
   */

  if (strncmp(scratch, "GET /nyalink HTTP/1.1\r\n", 23) != 0)
    {
      return NYABULA_WS_NOT_UPGRADE;
    }

  /* Replace CRLF with one NUL each, preserving a double-NUL terminator. */

  char *readp = scratch;
  char *writep = scratch;
  while (*readp)
    {
      if (readp[0] == '\r' && readp[1] == '\n')
        {
          while (writep > scratch && (writep[-1] == ' ' || writep[-1] == '\t'))
            {
              writep--;
            }

          *writep++ = '\0';
          readp += 2;
        }
      else
        {
          *writep++ = *readp++;
        }
    }

  *writep = '\0';
  key = nyabula_eye_ws_header(scratch, "Sec-WebSocket-Key");
  version = nyabula_eye_ws_header(scratch, "Sec-WebSocket-Version");
  actual_origin = nyabula_eye_ws_header(scratch, "Origin");
  host = nyabula_eye_ws_header(scratch, "Host");

  /* With no origin configured the rule is that the page came from this
   * device: its Origin, less the scheme, is the very Host it is calling.
   * That holds on whichever address the device happens to have, and still
   * refuses a page from anywhere else -- which is the whole point of the
   * check, since the token alone would let any site the owner visits drive
   * the socket from their browser.
   */

  if (origin == NULL && actual_origin != NULL && host != NULL)
    {
      same_host = strncmp(actual_origin, "http://", 7) == 0 &&
                  strcmp(actual_origin + 7, host) == 0;
    }

  if (key == NULL || strlen(key) != 24 || version == NULL ||
      strcmp(version, "13") != 0 || actual_origin == NULL ||
      (origin != NULL ? strcmp(actual_origin, origin) != 0 : !same_host) ||
      host == NULL ||
      !nyabula_eye_ws_has_token(nyabula_eye_ws_header(scratch, "Upgrade"),
                                "websocket") ||
      !nyabula_eye_ws_has_token(nyabula_eye_ws_header(scratch, "Connection"),
                                "upgrade") ||
      base64_decode(key, 24, keybytes, &decoded) == NULL || decoded != 16)
    {
      return -EACCES;
    }

  base64_encode(keybytes, decoded, accept, &decoded);
  accept[decoded] = '\0';
  if (strcmp(key, accept) != 0)
    {
      return -EPROTO;
    }

  sha1init(&sha);
  sha1update(&sha, key, 24);
  sha1update(&sha, NYABULA_WS_GUID, sizeof(NYABULA_WS_GUID) - 1);
  sha1final(digest, &sha);
  base64_encode(digest, sizeof(digest), accept, &decoded);
  accept[decoded] = '\0';
  int count = snprintf(scratch, capacity,
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n\r\n",
                       accept);
  return nyabula_eye_ws_io(fd, scratch, count, true, deadline);
}

/****************************************************************************
 * Name: nyabula_eye_ws_send
 ****************************************************************************/

int nyabula_eye_ws_send(int fd, uint8_t opcode, const void *data,
                        size_t length)
{
  unsigned char header[4] = { 0x80 | opcode, 0, 0, 0 };
  size_t hlen = 2;
  uint64_t deadline = nyabula_eye_ws_now() + NYABULA_WS_IO_MS;
  int ret;
  if (length > NYABULA_WS_MESSAGE_MAX)
    {
      return -EMSGSIZE;
    }

  if (length < 126)
    {
      header[1] = length;
    }
  else
    {
      header[1] = 126;
      header[2] = length >> 8;
      header[3] = length;
      hlen = 4;
    }

  ret = nyabula_eye_ws_io(fd, header, hlen, true, deadline);
  return ret < 0 ? ret
                 : nyabula_eye_ws_io(fd, (void *)data, length, true, deadline);
}

/****************************************************************************
 * Name: nyabula_eye_ws_receive
 ****************************************************************************/

int nyabula_eye_ws_receive(int fd, unsigned char *data, size_t capacity,
                           uint8_t *opcode, bool *final)
{
  unsigned char header[2];
  unsigned char mask[4];
  uint64_t deadline = nyabula_eye_ws_now() + NYABULA_WS_IO_MS;
  size_t length;
  int ret = nyabula_eye_ws_io(fd, header, 2, false, deadline);
  if (ret < 0)
    {
      return ret;
    }

  *opcode = header[0] & 15;
  *final = (header[0] & 0x80) != 0;
  if ((header[0] & 0x70) != 0 || (header[1] & 0x80) == 0 ||
      (*opcode != 0 && *opcode != 1 && *opcode != 8 && *opcode != 9 &&
       *opcode != 10))
    {
      return -EPROTO;
    }

  length = header[1] & 127;
  if (*opcode >= 8 && (!*final || length > 125))
    {
      return -EPROTO;
    }

  if (length == 127)
    {
      return -EMSGSIZE;
    }

  if (length == 126)
    {
      ret = nyabula_eye_ws_io(fd, header, 2, false, deadline);
      if (ret < 0)
        {
          return ret;
        }

      length = ((size_t)header[0] << 8) | header[1];
      if (length < 126)
        {
          return -EPROTO;
        }
    }

  if (length > capacity)
    {
      return -EMSGSIZE;
    }

  ret = nyabula_eye_ws_io(fd, mask, 4, false, deadline);
  if (ret == 0)
    {
      ret = nyabula_eye_ws_io(fd, data, length, false, deadline);
    }

  if (ret < 0)
    {
      return ret;
    }

  for (size_t i = 0; i < length; i++)
    {
      data[i] ^= mask[i % 4];
    }

  return length;
}

/****************************************************************************
 * Name: nyabula_eye_ws_utf8
 ****************************************************************************/

bool nyabula_eye_ws_utf8(const unsigned char *data, size_t length)
{
  return ny_utf8_valid(data, length);
}
