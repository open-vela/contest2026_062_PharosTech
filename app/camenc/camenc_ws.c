/****************************************************************************
 * app/camenc/camenc_ws.c
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
 * A WebSocket server, in as much of the protocol as a one-way stream needs.
 *
 * The protocol has two halves.  The opening handshake is an HTTP request with
 * a few headers and a reply that proves the server understood them; after
 * that the connection stops being HTTP and becomes a sequence of frames.
 *
 * Only what a video stream uses is implemented:
 *
 *   - the handshake, with the key and the digest it is answered with;
 *   - text and binary frames, outbound, unmasked (a server never masks);
 *   - close and ping frames, inbound, because a browser sends both;
 *   - per-frame payload lengths of 7, 16 and 64 bits, so a large frame is
 *     not silently truncated.
 *
 * What is not implemented is the parts a one-way stream never reaches:
 * fragmentation of outbound frames, extensions, subprotocols, and masking of
 * anything the server sends.  Each of those is refused rather than ignored
 * where refusing is what the protocol says to do.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <crypto/sha1.h>

#include "camenc_ws.h"

/* A connection is taken when the caller's poll() says one is waiting, and the
 * backlog is what makes that possible.  On NuttX a connection is handed to
 * accept() only if some task is blocked in accept() at that moment; the branch
 * that parks it in a backlog instead -- so that a task which is busy, as a
 * video encoder is busy, can pick it up later -- is inside
 * #ifdef CONFIG_NET_TCPBACKLOG.  And the branch in accept() that returns
 * EAGAIN instead of waiting is inside the same #ifdef, so without the option
 * there is no backlog for a connection to wait in and no way to ask whether
 * one is waiting.
 *
 * Which is worth a build error rather than a log that stops in the middle of
 * a line: a connection that arrives while no task is blocked in accept() is
 * dropped and the peer sees a refused or timed-out connection, which looks
 * like a network fault and is not one.
 */

#if defined(__NuttX__) && !defined(CONFIG_NET_TCPBACKLOG)
#error "camenc_ws polls for connections; enable CONFIG_NET_TCPBACKLOG"
#endif

/* The same assumption about the send side, and the one that is harder to
 * notice when it is wrong.
 *
 * This server queues a frame and expects send() to take what it can and give
 * back EAGAIN for the rest; ws_flush() is written around that and resends
 * from where it stopped.  NuttX only behaves that way when TCP write
 * buffering is on.  With it off, sends go inline and wait for the
 * acknowledgement -- and, unlike the receive path, the send path does not
 * consult the non-blocking flag at all.  tcp_send_unbuffered.c contains no
 * reference to O_NONBLOCK or to _SF_NONBLOCK anywhere, so every send() in
 * ws_flush() blocks until the data is acknowledged however the socket is
 * set.  The wait has no bound either: the default send timeout is zero,
 * which _SO_TIMEOUT() turns into UINT_MAX.
 *
 * The cost of getting this wrong is not a hang, which would be obvious.  It
 * is a frame rate that sags when the picture moves: bigger frames mean more
 * round trips, and the round trips are inside the loop that takes frames from
 * the camera.  With write buffering off, this server took the camera from
 * 62.5 fps to under 40 on a scene with motion in it, while the encoder itself
 * never used more than 1.5 ms of the 16 ms frame period.
 *
 * So it is a build error rather than a measurement: the failure mode is a
 * plausible-looking performance problem that no amount of reading the
 * application's own code explains.
 */

#if defined(__NuttX__) && defined(CONFIG_NET_TCP) && \
    !defined(CONFIG_NET_TCP_WRITE_BUFFERS)
#error \
    "camenc_ws needs non-blocking sends; enable CONFIG_NET_TCP_WRITE_BUFFERS"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The magic string from RFC 6455 section 4.2.2.  The accept key is the
 * SHA-1 of the client's key followed by this, base64-encoded; it exists so
 * that a reply cannot be forged by something that merely echoes a header.
 */

#define CAMENC_WS_GUID     "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define CAMENC_WS_OP_CONT  0x0
#define CAMENC_WS_OP_TEXT  0x1
#define CAMENC_WS_OP_BIN   0x2
#define CAMENC_WS_OP_CLOSE 0x8
#define CAMENC_WS_OP_PING  0x9
#define CAMENC_WS_OP_PONG  0xa

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The page a browser is given, and the whole of the client side.
 *
 * It is a MediaSource fed from a WebSocket, which is the shortest way to
 * play a fragmented MP4 in a browser: append what arrives, and remove what
 * has been played so the buffer does not grow without bound.  A live stream
 * has no duration, so nothing here seeks or ends.
 *
 * The codec string is filled in from the stream rather than written here,
 * because a MediaSource refuses data whose codec string does not match it.
 */

/* The page is no longer a C string in this file.  It lives in web/page.html
 * and is embedded verbatim from there by the .incbin assembler directive
 * below, which defines the two symbols delimiting its bytes.  Editing the
 * page is now editing an HTML file, not splicing a string across C source.
 *
 * The page still has to end up in one reply as a single body, and it still
 * has to have the stream's codec string (something like "avc1.42c016",
 * eleven bytes) written into it rather than baked in, because the codec is
 * only known once the encoder's first parameter set has been seen.  The page
 * therefore carries an eleven-byte placeholder where the codec belongs --
 * the same length as a codec string -- which this file overwrites at request
 * time.  The placeholder being exactly codec-sized means filling it moves
 * nothing and the page keeps its length, so no buffer has to hold the whole
 * page and the reply's Content-Length stays exact.  The placeholder is found
 * by a search rather than a hard-coded offset, so it can move with the page.
 */

#define CAMENC_WS_CODEC_SLOT_SIZE 11

/* .incbin takes its path relative to the assembler's working directory,
 * which in the Makefile build is this application's own directory
 * (Config.mk runs `make -C system/camenc`), so "web/page.html" resolves
 * here. */

__asm__(".section .rodata\n\t"
        ".align 2\n\t"
        ".global _g_page_html_start\n\t"
        ".type _g_page_html_start, %object\n\t"
        "_g_page_html_start:\n\t"
        ".incbin \"web/page.html\"\n\t"
        ".global _g_page_html_end\n\t"
        ".type _g_page_html_end, %object\n\t"
        "_g_page_html_end:\n\t");

extern const char _g_page_html_start[];
extern const char _g_page_html_end[];

/* The offset of the codec slot from the start of the page, found at request
 * time by looking for the placeholder.  A fixed offset would quietly point
 * past the slot the first time a line is added above it.  Returns
 * (size_t)-1 if the placeholder is not there, which cannot be a valid slot.
 */

static size_t page_codec_slot(void)
{
  static const char marker[] = "@@@@@@@@@@@";

  FAR const char *p = memmem(_g_page_html_start,
                             (size_t)(_g_page_html_end - _g_page_html_start),
                             marker, CAMENC_WS_CODEC_SLOT_SIZE);

  return p != NULL ? (size_t)(p - _g_page_html_start) : (size_t)-1;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ws_base64
 *
 * Description:
 *   Encode a digest for the handshake reply.
 *
 *   The C library's base64 support writes to a stream for logging rather
 *   than to a buffer, so the twenty lines are written out here instead of
 *   pulling in a stream to write into a string.
 *
 ****************************************************************************/

static size_t ws_base64(FAR const uint8_t *in, size_t len, FAR char *out)
{
  static const char tab[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  size_t i;
  size_t n = 0;

  for (i = 0; i + 2 < len; i += 3)
    {
      uint32_t v =
          ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];

      out[n++] = tab[(v >> 18) & 0x3f];
      out[n++] = tab[(v >> 12) & 0x3f];
      out[n++] = tab[(v >> 6) & 0x3f];
      out[n++] = tab[v & 0x3f];
    }

  if (i < len)
    {
      uint32_t v = (uint32_t)in[i] << 16;

      out[n++] = tab[(v >> 18) & 0x3f];

      if (i + 1 < len)
        {
          v |= (uint32_t)in[i + 1] << 8;
          out[n++] = tab[(v >> 12) & 0x3f];
          out[n++] = tab[(v >> 6) & 0x3f];
        }
      else
        {
          out[n++] = tab[(v >> 12) & 0x3f];
          out[n++] = '=';
        }

      out[n++] = '=';
    }

  out[n] = '\0';
  return n;
}

/****************************************************************************
 * Name: ws_head_len
 *
 * Description:
 *   Where a request's headers end: the offset just past the blank line that
 *   terminates them, or zero if it is not all there yet.
 *
 *   The two uses of this differ.  Before dispatching, zero means "wait for
 *   more".  After a request has been answered, the same offset says how much
 *   of the buffer was that request, so that what follows it -- frames, on a
 *   connection that has just been upgraded -- is not read as though the
 *   request were still in front of them.
 *
 ****************************************************************************/

static size_t ws_head_len(FAR const uint8_t *buf, size_t len)
{
  static const char end[] = "\r\n\r\n";
  size_t i;

  for (i = 0; i + sizeof(end) - 1 <= len; i++)
    {
      if (memcmp(buf + i, end, sizeof(end) - 1) == 0)
        {
          return i + sizeof(end) - 1;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: ws_header
 *
 * Description:
 *   Find a header in a request, case-insensitively, and return its value
 *   with leading spaces removed.
 *
 *   The request is NUL-terminated by the caller, at the end of what has
 *   arrived, which is what makes walking it line by line safe.  Without
 *   that the walk runs on past the request into whatever the buffer held
 *   before, and a header that is not in this request can be found in the
 *   last one.
 *
 ****************************************************************************/

static FAR const char *ws_header(FAR const char *req, FAR const char *name)
{
  size_t n = strlen(name);
  FAR const char *p = req;

  while ((p = strchr(p, '\n')) != NULL)
    {
      p++;

      if (strncasecmp(p, name, n) == 0 && p[n] == ':')
        {
          p += n + 1;
          while (*p == ' ' || *p == '\t')
            {
              p++;
            }

          return p;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: ws_close_client
 *
 * Description:
 *   Release a client slot: close its socket and forget the connection, but
 *   keep the transmit buffer, which belongs to the slot rather than to any
 *   one connection.
 *
 ****************************************************************************/

static void ws_close_client(FAR struct camenc_ws_s *ws,
                            FAR struct camenc_ws_client_s *c);

/****************************************************************************
 * Name: ws_queue
 *
 * Description:
 *   Append bytes to a client's transmit buffer, dropping the client if they
 *   do not fit.
 *
 *   Dropping rather than waiting is the decision this whole module turns on.
 *   Waiting would mean the capture loop stops taking frames whenever one
 *   viewer is slow, so a slow viewer would stall the camera for everyone
 *   else; dropping costs that viewer a reconnect and costs the stream
 *   nothing.  It is only defensible because a client that comes back can be
 *   brought up to the stream again from the initialisation segment.
 *
 ****************************************************************************/

/* Whether a client's buffer has room for `len` more bytes.
 *
 * Asked before anything is queued rather than after, because a frame is two
 * writes -- a header and a payload -- and a header that was queued while its
 * payload was refused leaves the client reading the next frame's bytes as
 * this frame's contents.  Nothing recovers from that, so the all-or-nothing
 * question has to be asked first.
 */

static bool ws_room(FAR const struct camenc_ws_client_s *c, size_t len)
{
  return c->tx_len + len <= c->tx_size;
}

/* Say once, however many times it happens, that a client is not keeping up.
 * A line per frame for the rest of the run buries whatever else the log was
 * going to say, and the condition does not change frame to frame.
 */

static void ws_warn_full(FAR struct camenc_ws_s *ws,
                         FAR const struct camenc_ws_client_s *c, size_t len)
{
  if (!ws->warned_full)
    {
      _warn("CAMENC WS: a %zu-byte frame does not fit %zu of %zu bytes; that "
            "client is behind and some frames are being thrown away\n",
            len, c->tx_len, c->tx_size);
      ws->warned_full = true;
    }
}

static bool ws_queue(FAR struct camenc_ws_s *ws,
                     FAR struct camenc_ws_client_s *c, const uint8_t *data,
                     size_t len)
{
  if (!ws_room(c, len))
    {
      ws_warn_full(ws, c, len);
      return false;
    }

  memcpy(c->tx + c->tx_len, data, len);
  c->tx_len += len;

  /* Compact once everything queued has gone out, so that a long-lived
   * connection does not walk its buffer to the end and stop.
   */

  if (c->tx_sent == c->tx_len)
    {
      c->tx_sent = 0;
      c->tx_len = 0;
    }

  return true;
}

/****************************************************************************
 * Name: ws_frame
 *
 * Description:
 *   Queue one WebSocket frame: a two-byte header, an extended length if
 *the payload needs one, and the payload.
 *
 *   The mask bit is clear, as it must be for anything a server sends.
 *The opcode is the final one, because a frame that fits is never
 *fragmented.
 *
 ****************************************************************************/

static bool ws_frame(FAR struct camenc_ws_s *ws,
                     FAR struct camenc_ws_client_s *c, uint8_t opcode,
                     const uint8_t *data, size_t len)
{
  uint8_t hdr[10];
  size_t hlen;

  hdr[0] = (uint8_t)(0x80u | opcode); /* FIN, and this opcode */

  if (len < 126)
    {
      hdr[1] = (uint8_t)len;
      hlen = 2;
    }
  else if (len <= 0xffff)
    {
      hdr[1] = 126;
      hdr[2] = (uint8_t)(len >> 8);
      hdr[3] = (uint8_t)len;
      hlen = 4;
    }
  else
    {
      int i;

      hdr[1] = 127;

      for (i = 0; i < 8; i++)
        {
          hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
        }

      hlen = 10;
    }

  if (!ws_room(c, hlen + len))
    {
      /* Refused before the header, so the client's stream is still whole.
       * See ws_room.  Whether this costs it a frame or its connection is
       * the caller's decision.
       */

      ws_warn_full(ws, c, hlen + len);
      return false;
    }

  if (!ws_queue(ws, c, hdr, hlen))
    {
      return false;
    }

  return len == 0 || ws_queue(ws, c, data, len);
}

/****************************************************************************
 * Name: ws_flush
 *
 * Description:
 *   Write out as much of a client's buffer as the socket will take.
 *
 *   The socket is non-blocking, so a partial write is normal and the
 *rest waits for the next poll.
 *
 ****************************************************************************/

static void ws_flush(FAR struct camenc_ws_s *ws,
                     FAR struct camenc_ws_client_s *c)
{
  while (c->tx_sent < c->tx_len)
    {
      ssize_t n = send(c->fd, c->tx + c->tx_sent, c->tx_len - c->tx_sent, 0);

      if (n > 0)
        {
          c->tx_sent += (size_t)n;
          c->sent += (uint64_t)n;
          continue;
        }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
          return; /* the socket is full; the rest goes next time */
        }

      _warn("CAMENC WS: send failed: %d\n", errno);
      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }

  c->tx_sent = 0;
  c->tx_len = 0;

  /* A reply that is the whole of a connection is closed once it has
   * actually gone out, not when it was queued -- closing on the queue
   * would cut the reply off partway for anyone whose socket was full at
   * that moment.
   */

  if (c->closing)
    {
      ws_close_client(ws, c);
    }
}

/****************************************************************************
 * Name: ws_serve_*
 *
 * Description:
 *   The two things a plain HTTP request can be: a request for the page,
 *   or a request to upgrade.  Anything else gets a short answer saying so.
 *
 ****************************************************************************/

static void ws_serve_page(FAR struct camenc_ws_s *ws,
                          FAR struct camenc_ws_client_s *c)
{
  FAR const char *codec = ws->codec[0] != '\0' ? ws->codec : "avc1.42c016";
  size_t page_len = (size_t)(_g_page_html_end - _g_page_html_start);
  size_t slot = page_codec_slot();
  size_t clen = strlen(codec);
  size_t blen;
  char head[256];
  int hlen;

  /* The reply is the page with the codec slot's bytes replaced by the codec
   * string, so its length is the page's with the slot swapped for the codec
   * -- equal in practice, kept exact by the subtraction.
   */

  if (slot == (size_t)-1 || slot + CAMENC_WS_CODEC_SLOT_SIZE > page_len)
    {
      /* The placeholder is missing or mis-sized: serve the page as-is rather
       * than write the codec over some unrelated byte.  A player will refuse
       * the MediaSource, but the page itself still loads.
       */

      blen = page_len;
    }
  else
    {
      blen = page_len - CAMENC_WS_CODEC_SLOT_SIZE + clen;
    }

  hlen = snprintf(head, sizeof(head),
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Content-Length: %zu\r\n"
                  "Cache-Control: no-store\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  blen);
  if (hlen < 0 || (size_t)hlen >= sizeof(head))
    {
      return;
    }

  if (!ws_room(c, (size_t)hlen + blen))
    {
      /* The page is all this connection was for, so there is nothing to be
       * done for it but to let it go -- unlike a stream, which can be
       * degraded and still be a stream.
       */

      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }

  /* The page's bytes are queued in pieces -- before the slot, the codec,
   * after the slot -- rather than composed into one buffer, for the same
   * reason the page used to be two strings: nothing anywhere holds the
   * whole page, and a reply larger than one write is not a special case.
   * The page lives in read-only memory, so the slot is not overwritten in
   * place; it is skipped and the codec queued in its stead, and the reply's
   * length stays exact because the slot and the codec swap one-for-one.
   */

  if (!ws_queue(ws, c, (FAR const uint8_t *)head, (size_t)hlen))
    {
      return;
    }

  if (slot == (size_t)-1 || slot + CAMENC_WS_CODEC_SLOT_SIZE > page_len)
    {
      if (!ws_queue(ws, c, (FAR const uint8_t *)_g_page_html_start, page_len))
        {
          return;
        }
    }
  else if (!ws_queue(ws, c, (FAR const uint8_t *)_g_page_html_start, slot) ||
           !ws_queue(ws, c, (FAR const uint8_t *)codec, clen) ||
           !ws_queue(ws, c,
                     (FAR const uint8_t *)(_g_page_html_start + slot +
                                           CAMENC_WS_CODEC_SLOT_SIZE),
                     page_len - slot - CAMENC_WS_CODEC_SLOT_SIZE))
    {
      return;
    }

  /* The page is all this connection was for.  It is written out by the next
   * flush and then closed, because a fresh connection is what the page's
   * WebSocket will make anyway.
   */

  c->closing = true;
  ws_flush(ws, c);
}

static void ws_serve_upgrade(FAR struct camenc_ws_s *ws,
                             FAR struct camenc_ws_client_s *c,
                             FAR const char *req)
{
  FAR const char *key = ws_header(req, "Sec-WebSocket-Key");
  char concat[128];
  char accept[64];
  char reply[256];
  SHA1_CTX sha;
  uint8_t digest[SHA1_DIGEST_LENGTH];
  int n;

  if (key == NULL)
    {
      /* A browser's upgrade always carries the key, so its absence means
       * the request was not what the server took it for.  What arrived
       * is then the whole of the question, so it is printed -- the
       * request line alone would not say whether the key was missing
       * from the request or lost between here and there.  Line breaks
       * are shown rather than obeyed, so that it stays one line in the
       * log.
       */

      char seen[320];
      size_t used;
      size_t total = strlen(req);

      for (used = 0; used < sizeof(seen) - 1 && used < total; used++)
        {
          char ch = req[used];

          seen[used] = (ch == '\r' || ch == '\n') ? '|' : ch;
        }

      seen[used] = '\0';

      _warn("CAMENC WS: upgrade without a key, %zu bytes: %s\n", total, seen);
      ws_close_client(ws, c);
      return;
    }

  /* The digest covers the client's key and the fixed string,
   * concatenated. The key is printed up to the line's end, so it has to
   * be copied out rather than pointed at.
   */

  {
    size_t i = 0;
    size_t k = 0;

    while (k < sizeof(concat) - sizeof(CAMENC_WS_GUID) - 1 && key[k] != '\0' &&
           key[k] != '\r' && key[k] != '\n')
      {
        concat[i++] = key[k++];
      }

    memcpy(concat + i, CAMENC_WS_GUID, sizeof(CAMENC_WS_GUID));
    i += sizeof(CAMENC_WS_GUID) - 1;

    sha1init(&sha);
    sha1update(&sha, concat, (unsigned int)i);
    sha1final(digest, &sha);
  }

  ws_base64(digest, sizeof(digest), accept);

  n = snprintf(reply, sizeof(reply),
               "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n"
               "\r\n",
               accept);
  if (n < 0 || (size_t)n >= sizeof(reply))
    {
      ws_close_client(ws, c);
      return;
    }

  if (!ws_queue(ws, c, (FAR const uint8_t *)reply, (size_t)n))
    {
      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }

  c->upgraded = true;
  c->have_init = false;
  c->waiting = true;
  ws->clients_served++;

  /* Give it the stream's own description if that is known, and nothing
   * else.
   *
   * The rest of what a decoder needs to start is a segment it may start on,
   * and the next one of those is at most one group of pictures away.  Waiting
   * for it rather than replaying the group in progress is the fix for a
   * client that could not connect at all; see the note on `waiting` in
   * camenc_ws.h.
   */

  if (ws->init_len > 0)
    {
      _info("CAMENC WS: new client, given the %zu-byte initialisation "
            "segment\n",
            ws->init_len);

      /* If even this cannot be queued, the client cannot be brought up at
       * all: it has no way to learn what the stream is, and waiting for an
       * initialisation segment that has already been published means
       * waiting for ever.
       */

      if (!ws_frame(ws, c, CAMENC_WS_OP_BIN, ws->init_seg, ws->init_len))
        {
          ws_close_client(ws, c);
          ws->clients_dropped++;
          return;
        }

      c->have_init = true;
    }
  else
    {
      _info("CAMENC WS: new client, waiting for the initialisation "
            "segment\n");
    }

  ws_flush(ws, c);
}

/****************************************************************************
 * Name: ws_handle_request
 ****************************************************************************/

static void ws_handle_request(FAR struct camenc_ws_s *ws,
                              FAR struct camenc_ws_client_s *c)
{
  FAR const char *upgrade = ws_header((FAR const char *)c->rx, "Upgrade");

  if (upgrade != NULL && strncasecmp(upgrade, "websocket", 9) == 0)
    {
      size_t used = ws_head_len(c->rx, c->rx_len);

      ws_serve_upgrade(ws, c, (FAR const char *)c->rx);

      /* The request has been answered, so it is consumed.  Anything
       * behind it on this connection is a frame, and reading from the
       * front of the buffer again would read the request as one.
       */

      if (used <= c->rx_len)
        {
          memmove(c->rx, c->rx + used, c->rx_len - used);
          c->rx_len -= used;
        }

      return;
    }

  if (strncmp((FAR const char *)c->rx, "GET ", 4) == 0)
    {
      ws_serve_page(ws, c);
      return;
    }

  {
    static const char notfound[] = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n"
                                   "\r\n";

    if (ws_queue(ws, c, (FAR const uint8_t *)notfound, sizeof(notfound) - 1))
      {
        c->closing = true;
        ws_flush(ws, c);
      }
    else
      {
        ws_close_client(ws, c);
      }
  }
}

/****************************************************************************
 * Name: ws_close_client
 ****************************************************************************/

static void ws_close_client(FAR struct camenc_ws_s *ws,
                            FAR struct camenc_ws_client_s *c)
{
  if (c->fd >= 0)
    {
      close(c->fd);
      c->fd = -1;
    }

  c->upgraded = false;
  c->closing = false;
  c->have_init = false;
  c->waiting = false;
  c->rx_len = 0;
  c->tx_len = 0;
  c->tx_sent = 0;

  (void)ws;
}

/****************************************************************************
 * Name: ws_read_client
 *
 * Description:
 *   Consume whatever a client has sent.
 *
 *   Before the handshake that means collecting a request; afterwards it
 *   means answering a close and ignoring a ping, and noticing when the
 *peer has gone.  Reading at all is what makes that last part work: a
 *socket that is never read reports a disconnection only when something
 *is finally written to it, which for a stream that has stopped for any
 *   reason is never.
 *
 ****************************************************************************/

static void ws_read_client(FAR struct camenc_ws_s *ws,
                           FAR struct camenc_ws_client_s *c)
{
  for (;;)
    {
      ssize_t n;

      /* The receive buffer keeps a byte back for the terminator, so that
       * what has arrived is always a string: see ws_header.
       */

      if (c->rx_len >= sizeof(c->rx) - 1)
        {
          /* A request this long is not a request.  A frame cannot reach
           * here because frames are consumed as they are read.
           */

          _warn("CAMENC WS: request too long, dropping the connection\n");
          ws_close_client(ws, c);
          ws->clients_dropped++;
          return;
        }

      n = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - 1 - c->rx_len, 0);

      if (n > 0)
        {
          c->rx_len += (size_t)n;
          c->rx[c->rx_len] = '\0';

          if (!c->upgraded)
            {
              if (ws_head_len(c->rx, c->rx_len) != 0)
                {
                  ws_handle_request(ws, c);
                  return;
                }

              continue;
            }

          /* Upgraded: everything received is a frame, and the only one
           * worth acting on is close.
           */

          {
            size_t i = 0;

            while (i + 2 <= c->rx_len)
              {
                uint8_t opcode = c->rx[i] & 0x0fu;
                uint8_t masked = c->rx[i + 1] & 0x80u;
                uint64_t len = c->rx[i + 1] & 0x7fu;
                size_t hdr = 2;

                if (len == 126)
                  {
                    if (i + 4 > c->rx_len)
                      {
                        break;
                      }

                    len = ((uint64_t)c->rx[i + 2] << 8) | c->rx[i + 3];
                    hdr = 4;
                  }
                else if (len == 127)
                  {
                    int k;

                    if (i + 10 > c->rx_len)
                      {
                        break;
                      }

                    len = 0;
                    for (k = 0; k < 8; k++)
                      {
                        len = (len << 8) | c->rx[i + 2 + k];
                      }

                    hdr = 10;
                  }

                if (masked)
                  {
                    hdr += 4;
                  }

                if (i + hdr + len > c->rx_len)
                  {
                    break;
                  }

                if (opcode == CAMENC_WS_OP_CLOSE)
                  {
                    _info("CAMENC WS: client closed\n");
                    ws_close_client(ws, c);
                    return;
                  }

                if (opcode == CAMENC_WS_OP_PING)
                  {
                    ws_frame(ws, c, CAMENC_WS_OP_PONG, c->rx + i + hdr,
                             (size_t)len);
                  }

                if (opcode == CAMENC_WS_OP_TEXT)
                  {
                    /* A text frame is a control message, and the server
                     * hands it on without reading it -- see
                     * camenc_ws_cmd_t.
                     *
                     * It arrives masked, as everything from a client must,
                     * so it is unmasked into a buffer of its own rather
                     * than in place: what follows this frame in the receive
                     * buffer belongs to the next one, and unmasking in place
                     * would leave it unreadable.
                     *
                     * A frame longer than the buffer is dropped rather than
                     * truncated.  Half a command is a different command, and
                     * running the first half of one would be worse than
                     * ignoring it.
                     */

                    if (len > CAMENC_WS_CMD_MAX)
                      {
                        _warn("CAMENC WS: ignoring a %zu-byte control "
                              "message\n",
                              (size_t)len);
                      }
                    else if (masked)
                      {
                        uint8_t text[CAMENC_WS_CMD_MAX + 1];
                        size_t k;

                        for (k = 0; k < (size_t)len; k++)
                          {
                            text[k] = c->rx[i + hdr + k] ^
                                      c->rx[i + hdr - 4 + (k & 3u)];
                          }

                        text[(size_t)len] = '\0';

                        if (ws->on_command != NULL)
                          {
                            ws->on_command((FAR const char *)text,
                                           ws->command_arg);
                          }
                      }
                  }

                i += hdr + (size_t)len;
              }

            if (i > 0)
              {
                memmove(c->rx, c->rx + i, c->rx_len - i);
                c->rx_len -= i;
              }

            return;
          }
        }

      if (n == 0)
        {
          _info("CAMENC WS: client went away\n");
          ws_close_client(ws, c);
          return;
        }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return;
        }

      _warn("CAMENC WS: recv failed: %d\n", errno);
      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camenc_ws_start(FAR struct camenc_ws_s *ws, uint16_t port, size_t tx_size)
{
  struct sockaddr_in addr;
  int one = 1;
  int i;

  if (ws == NULL || tx_size <= CAMENC_WS_TX_HEADER)
    {
      return -EINVAL;
    }

  memset(ws, 0, sizeof(*ws));
  ws->listen_fd = -1;
  ws->tx_size = tx_size;

  /* Every descriptor says "none" before anything is allocated, because a
   * failure part-way through runs the teardown below, and zero is a
   * descriptor it would close -- the console.
   */

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      ws->clients[i].fd = -1;
      ws->clients[i].tx_size = tx_size;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      ws->clients[i].tx = malloc(tx_size);
      if (ws->clients[i].tx == NULL)
        {
          camenc_ws_stop(ws);
          return -ENOMEM;
        }
    }

  ws->init_seg = malloc(CAMENC_WS_INIT_MAX);
  if (ws->init_seg == NULL)
    {
      camenc_ws_stop(ws);
      return -ENOMEM;
    }

  ws->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (ws->listen_fd < 0)
    {
      int err = -errno;

      camenc_ws_stop(ws);
      return err;
    }

  setsockopt(ws->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(ws->listen_fd, (FAR struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      int err = -errno;

      _err("CAMENC WS: cannot bind port %u: %d\n", port, errno);
      camenc_ws_stop(ws);
      return err;
    }

  if (listen(ws->listen_fd, CAMENC_WS_MAX_CLIENTS) < 0)
    {
      int err = -errno;

      camenc_ws_stop(ws);
      return err;
    }

  if (fcntl(ws->listen_fd, F_SETFL,
            fcntl(ws->listen_fd, F_GETFL, 0) | O_NONBLOCK) < 0)
    {
      int err = -errno;

      _err("CAMENC WS: the listening socket will not go non-blocking: %d\n",
           errno);
      camenc_ws_stop(ws);
      return err;
    }

  ws->port = port;
  return 0;
}

void camenc_ws_set_codec(FAR struct camenc_ws_s *ws, FAR const char *codec)
{
  if (ws != NULL && codec != NULL)
    {
      strncpy(ws->codec, codec, sizeof(ws->codec) - 1);
      ws->codec[sizeof(ws->codec) - 1] = '\0';
    }
}

void camenc_ws_drop_clients(FAR struct camenc_ws_s *ws)
{
  int i;

  if (ws == NULL)
    {
      return;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd >= 0)
        {
          close(c->fd);
          c->fd = -1;
        }

      /* Reset field by field rather than with memset, for the reason the
       * accept path gives: the slot owns its transmit buffer, which outlives
       * any one connection.
       */

      c->upgraded = false;
      c->closing = false;
      c->have_init = false;
      c->waiting = false;
      c->sent = 0;
      c->skipped = 0;
      c->rx_len = 0;
      c->tx_len = 0;
      c->tx_sent = 0;
    }

  /* The initialisation segment describes the stream that has just ended.  The
   * buffer stays: it is reused by the next stream, which is about to write its
   * own over it.
   */

  ws->init_len = 0;
}

void camenc_ws_stop(FAR struct camenc_ws_s *ws)
{
  int i;

  if (ws == NULL)
    {
      return;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      if (ws->clients[i].fd >= 0)
        {
          close(ws->clients[i].fd);
          ws->clients[i].fd = -1;
        }

      free(ws->clients[i].tx);
      ws->clients[i].tx = NULL;
    }

  if (ws->listen_fd >= 0)
    {
      close(ws->listen_fd);
      ws->listen_fd = -1;
    }

  free(ws->init_seg);
  ws->init_seg = NULL;
  ws->init_len = 0;
}

/****************************************************************************
 * Name: ws_accept
 *
 * Description:
 *   Take every connection waiting on the listening socket.
 *
 *   Called only once poll() has said there is at least one, and looping
 *   because more than one may have arrived.  A connection that arrived while
 *   the loop was busy was held in the backlog by the stack rather than
 *   waited for by this server, which is the arrangement the file header
 *   describes.
 *
 ****************************************************************************/

static void ws_accept(FAR struct camenc_ws_s *ws)
{
  struct sockaddr_in addr;
  socklen_t alen;

  for (;;)
    {
      int fd;
      int slot;

      alen = sizeof(addr);
      fd = accept(ws->listen_fd, (FAR struct sockaddr *)&addr, &alen);

      if (fd < 0)
        {
          break;
        }

      for (slot = 0; slot < CAMENC_WS_MAX_CLIENTS; slot++)
        {
          if (ws->clients[slot].fd < 0)
            {
              break;
            }
        }

      if (slot == CAMENC_WS_MAX_CLIENTS)
        {
          _warn("CAMENC WS: no room for another client\n");
          close(fd);
          continue;
        }

      if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0)
        {
          _err("CAMENC WS: the connection will not go non-blocking: %d\n",
               errno);
          close(fd);
          continue;
        }

      /* Reset the connection's state, field by field.
       *
       * Not with memset: the slot owns its transmit buffer, which was
       * allocated when the server started and is meant to outlive any
       * one connection.  Clearing the whole struct would throw that
       * pointer away and leave the next write going to address zero.
       */

      {
        FAR struct camenc_ws_client_s *c = &ws->clients[slot];

        c->fd = fd;
        c->upgraded = false;
        c->closing = false;
        c->have_init = false;
        c->waiting = false;
        c->sent = 0;
        c->skipped = 0;
        c->rx_len = 0;
        c->tx_len = 0;
        c->tx_sent = 0;

        /* The receive buffer too, not just its length.  A request is
         * parsed as a string, so a byte left from the last connection is
         * a byte this one did not send -- and a header it does not have
         * can be found in it.
         */

        memset(c->rx, 0, sizeof(c->rx));
      }

      _info("CAMENC WS: client %d connected\n", slot);
    }
}

/****************************************************************************
 * Name: ws_client_for
 *
 * Description:
 *   The client a descriptor belongs to, or NULL if it is not one of ours.
 *
 *   Looked up by descriptor rather than remembered by position, because the
 *   caller's array is built before the wait and consumed after it and a
 *   descriptor is what the two ends of that have in common.  There are at
 *   most CAMENC_WS_MAX_CLIENTS of them.
 *
 ****************************************************************************/

static FAR struct camenc_ws_client_s *ws_client_for(FAR struct camenc_ws_s *ws,
                                                    int fd)
{
  int i;

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      if (ws->clients[i].fd == fd)
        {
          return &ws->clients[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: camenc_ws_fds
 *
 * Description:
 *   Say which sockets the caller should wait on.
 *
 *   The listening socket always is, and so is every client's: a client is
 *   read for because that is where its handshake and its controls arrive, and
 *   also because a client that has gone away is noticed there.  Writability
 *   is asked for only when there is something queued for that client, since a
 *   socket that is writable with nothing to write would wake the loop for
 *   nothing.
 *
 ****************************************************************************/

int camenc_ws_fds(FAR struct camenc_ws_s *ws, FAR struct pollfd *fds, int max)
{
  int n = 0;
  int i;

  if (ws == NULL || ws->listen_fd < 0)
    {
      return 0;
    }

  if (n < max)
    {
      fds[n].fd = ws->listen_fd;
      fds[n].events = POLLIN;
      fds[n].revents = 0;
      n++;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS && n < max; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0)
        {
          continue;
        }

      fds[n].fd = c->fd;
      fds[n].events = POLLIN;
      if (c->tx_len > c->tx_sent)
        {
          fds[n].events |= POLLOUT;
        }

      fds[n].revents = 0;
      n++;
    }

  return n;
}

/****************************************************************************
 * Name: camenc_ws_ready
 *
 * Description:
 *   Act on the sockets that became ready.
 *
 *   The array is the one camenc_ws_fds() filled, with poll()'s answer in it.
 *   Everything here is still non-blocking; what has changed is that the work
 *   is done because poll() said there was something to do, rather than once
 *   per frame whether or not there was.
 *
 ****************************************************************************/

void camenc_ws_ready(FAR struct camenc_ws_s *ws, FAR struct pollfd *fds, int n)
{
  int i;

  if (ws == NULL || ws->listen_fd < 0)
    {
      return;
    }

  for (i = 0; i < n; i++)
    {
      FAR struct camenc_ws_client_s *c;

      if (fds[i].revents == 0)
        {
          continue;
        }

      if (fds[i].fd == ws->listen_fd)
        {
          ws_accept(ws);
          continue;
        }

      c = ws_client_for(ws, fds[i].fd);
      if (c == NULL)
        {
          continue;
        }

      /* A hangup is read for rather than acted on directly: the read is what
       * finds the end of the stream, and it is also where a client that has
       * gone away is let go of -- see ws_read_client().
       */

      if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        {
          ws_read_client(ws, c);

          if (c->fd < 0)
            {
              continue;
            }
        }

      if ((fds[i].revents & POLLOUT) != 0)
        {
          ws_flush(ws, c);
        }
    }
}

int camenc_ws_publish(FAR struct camenc_ws_s *ws, enum camenc_seg_e seg,
                      const uint8_t *data, size_t len, bool key)
{
  int i;

  if (ws == NULL || data == NULL)
    {
      return -EINVAL;
    }

  /* Keep the initialisation segment, for clients that arrive after it.
   *
   * It is the one thing a late client cannot do without.  What is *not* kept
   * is the group of pictures in progress, and that is deliberate: a client
   * used to be given the last segment a stream may start on and everything
   * since, so that it could start immediately, and that cannot be made to
   * work.  The catch-up and the live frames share one transmit buffer, and
   * the live frames alone filled it in about three frames, so the client was
   * dropped while it was still completing its handshake and a browser
   * reported that it could not connect at all.
   *
   * A client waits for the next segment a stream may start on instead, which
   * is at most one group of pictures away.  See camenc_ws.h.
   */

  if (len + CAMENC_WS_TX_HEADER > ws->tx_size)
    {
      /* A segment no client can be sent.  Said once, because if the encoder
       * produces segments this size it produces them every time.
       */

      if (!ws->warned_oversize)
        {
          _warn("CAMENC WS: %zu-byte segments do not fit a %zu-byte "
                "buffer; no client can be served at this size\n",
                len, ws->tx_size);
          ws->warned_oversize = true;
        }
    }
  else if (seg == CAMENC_SEG_INIT && len <= CAMENC_WS_INIT_MAX)
    {
      memcpy(ws->init_seg, data, len);
      ws->init_len = len;
    }

  /* Then to everyone connected. */

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0 || !c->upgraded)
        {
          continue;
        }

      /* A client that has not been brought up to the stream yet is given
       * only the two things that can bring it up: the initialisation
       * segment, and then the first segment a stream may start on.  What
       * falls between them is dropped for that client rather than queued
       * for it -- queueing it is what used to fill the buffer.
       */

      if (!c->have_init)
        {
          if (seg != CAMENC_SEG_INIT)
            {
              continue;
            }

          c->have_init = true;
        }
      else if (c->waiting)
        {
          if (seg == CAMENC_SEG_INIT || !key)
            {
              continue;
            }

          c->waiting = false;
        }

      /* A frame that does not fit is thrown away for this client, which
       * keeps its connection.
       *
       * For a live stream that is the honest degradation.  The client is
       * behind, so what it needs is less data rather than all of it late:
       * dropping the frame loses one picture, and closing the connection
       * loses the viewer until it reconnects -- which for a moment of
       * backlog at the start of a stream is what used to happen, every
       * time, and looked like a server that refused connections.
       *
       * The caveat is P pictures, and it is worth stating even though
       * nothing here acts on it.  A fragment whose pictures refer to the one
       * before it cannot be dropped on its own, because the fragments that
       * follow refer to a picture the client never received -- which a
       * decoder shows as a frame's worth of garbage rather than as one
       * missing frame.
       *
       * This was briefly made to set `waiting` here, so that a client that
       * had dropped one would resume at the next picture a stream may start
       * on.  It was put back because it made things worse in a way that is
       * not hard to see in hindsight: a client whose socket stays full drops
       * every frame including the key ones, so `waiting` never clears and
       * the client receives nothing at all until it reconnects.  A stream
       * that is degraded is still a stream; one that has stopped is not.
       *
       * So the drop is left as it was, and the concern stands as a thing
       * this does not do.  What it needs is a way for the client itself to
       * be told to start again -- a decoder refresh on the encoder, asked
       * for when the drop happens -- rather than the server going quiet.
       */

      if (!ws_frame(ws, c, CAMENC_WS_OP_BIN, data, len))
        {
          c->skipped++;
          continue;
        }

      ws_flush(ws, c);
    }

  return 0;
}

/****************************************************************************
 * Name: camenc_ws_report
 *
 * Description:
 *   Print one line about how the clients are doing.
 *
 *   `pending` is the interesting number.  It is data the server has accepted
 *   and the socket has not, so it says both whether the link is draining and
 *   how far behind the client is.  `sent` says how much has gone out, and
 *   `skipped` how much was thrown away for not fitting, which together
 *   answer the question the capture loop's own timings cannot: a browser
 *   that receives the stream and one that receives nothing cost the loop
 *   exactly the same.
 *
 ****************************************************************************/

void camenc_ws_report(FAR struct camenc_ws_s *ws)
{
  uint64_t pending = 0;
  uint64_t sent = 0;
  uint64_t skipped = 0;
  unsigned int live = 0;
  unsigned int i;

  if (ws == NULL)
    {
      return;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR const struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0)
        {
          continue;
        }

      live++;
      pending += (uint64_t)(c->tx_len - c->tx_sent);
      sent += c->sent;
      skipped += c->skipped;
    }

  _info("CAMENC WS: %u client(s), pending %llu, sent %llu, skipped %llu, "
        "served %" PRIu32 ", dropped %" PRIu32 "\n",
        live, pending, sent, skipped, ws->clients_served, ws->clients_dropped);
}

/****************************************************************************
 * Name: camenc_ws_status
 *
 * Description:
 *   Send one text frame to every client, as far as it fits.
 *
 *   The difference between this and camenc_ws_publish is the whole of the
 *   design.  A segment that does not fit its client drops that client,
 *   because a viewer that cannot keep up with a live stream is not being
 *   served by delaying the camera for it.  A status message is not the
 *   stream: it says what the loop is doing, and a viewer whose buffer happens
 *   to be full at that moment loses one readout and keeps its picture.
 *
 *   So the room is checked before the header is queued rather than by letting
 *   ws_queue fail on the payload.  A header that was queued and a payload
 *   that was not is not a dropped message but a corrupt stream: the client
 *   reads the next frame's bytes as this one's contents and never recovers.
 *   And ws_queue cannot simply be asked first, because by then the header is
 *   already in the buffer.
 *
 ****************************************************************************/

void camenc_ws_status(FAR struct camenc_ws_s *ws, FAR const char *text,
                      size_t len)
{
  size_t hlen;
  unsigned int i;

  if (ws == NULL || len > 0xffffu)
    {
      return;
    }

  hlen = len < 126u ? 2u : 4u;

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0 || !c->upgraded)
        {
          continue;
        }

      if (c->tx_len + hlen + len > c->tx_size)
        {
          continue;
        }

      ws_frame(ws, c, CAMENC_WS_OP_TEXT, (FAR const uint8_t *)text, len);
      ws_flush(ws, c);
    }
}

/****************************************************************************
 * Name: camenc_ws_set_command
 *
 * Description:
 *   Say where a control message from a client should be handed.
 *
 ****************************************************************************/

void camenc_ws_set_command(FAR struct camenc_ws_s *ws, camenc_ws_cmd_t fn,
                           FAR void *arg)
{
  if (ws != NULL)
    {
      ws->on_command = fn;
      ws->command_arg = arg;
    }
}
