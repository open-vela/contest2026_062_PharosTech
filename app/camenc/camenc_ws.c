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

/* This server polls: it calls accept() between frames and expects to be told
 * EAGAIN when nobody is trying to connect.  On NuttX that only happens with
 * CONFIG_NET_TCPBACKLOG, and with it off accept() does not merely behave
 * differently -- it waits, forever, on the first call, and the application
 * stops where it stands.
 *
 * The reason is in the two halves of the stack that look after a connection
 * arriving at a listening socket.  tcp_accept_connection() hands the
 * connection to accept() only if some task is blocked in accept() at that
 * moment; the branch that puts it on a backlog instead -- so that a task
 * which is busy, as a video encoder is busy, can pick it up later -- is
 * inside #ifdef CONFIG_NET_TCPBACKLOG.  And the branch in accept() that
 * returns EAGAIN instead of waiting is inside the same #ifdef.  Without the
 * option there is no backlog for a connection to wait in and no way to ask
 * whether one is waiting, so the two cannot be combined into a server.
 *
 * Which is worth a build error rather than a log that stops in the middle of
 * a line: a connection that arrives while no task is blocked in accept() is
 * dropped and the peer sees a refused or timed-out connection, which looks
 * like a network fault and is not one.
 */

#if defined(__NuttX__) && !defined(CONFIG_NET_TCPBACKLOG)
#error "camenc_ws polls for connections; enable CONFIG_NET_TCPBACKLOG"
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

/* The page is a fixed text with one hole in it, for the codec string.  The
 * buffer it is composed into is therefore sized from the page itself plus
 * the longest that hole can be -- rather than from a number that has to be
 * kept in step with the page by hand.  It was such a number once, and the
 * page outgrew it: snprintf then refused to write anything, and the reply
 * was no page at all, which looks like a server that does not work rather
 * than one that is one addition too small.
 */

#define CAMENC_PAGE_SLACK 64

static const char g_page[] =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\"><title>camenc</title></head>\n"
    "<body style=\"margin:0;background:#111;color:#ddd;"
    "font:12px monospace\">\n"
    "<div id=s style=\"position:fixed;top:0;left:0;right:0;z-index:9;"
    "background:#000d;padding:4px 6px;white-space:pre-wrap\">"
    "connecting...</div>\n"
    "<video id=v autoplay muted playsinline style=\"display:block;"
    "margin:70px auto 0;max-width:100vw;"
    "max-height:calc(100vh - 76px)\"></video>\n"
    "<script>\n"
    "const v=document.getElementById('v'),s=document.getElementById('s');\n"
    "let sb=null,q=[],rx=0,msgs=0,err='',note='';\n"
    "const ms=new MediaSource();\n"
    "v.src=URL.createObjectURL(ms);\n"
    "/* How far behind the newest data the picture is allowed to fall before\n"
    "   the playhead is brought forward. Half a second keeps the picture\n"
    "   close to live while leaving enough in hand not to stutter. */\n"
    "const MAXLAG=0.6;\n"
    "function say(){\n"
    "  const b=sb?sb.buffered:null;\n"
    "  let br='-',lag='-';\n"
    "  if(b&&b.length){\n"
    "    const e=b.end(b.length-1);\n"
    "    br=b.start(0).toFixed(2)+'..'+e.toFixed(2)+' ('+b.length+')';\n"
    "    lag=(e-v.currentTime).toFixed(2)+'s'+\n"
    "      (e-v.currentTime>MAXLAG?' LATE':'');\n"
    "  }\n"
    "  s.textContent='msgs='+msgs+' rx='+rx+'B q='+q.length+\n"
    "    ' lag='+lag+'\\n'+\n"
    "    'sb='+(sb?sb.readyState:'none')+' upd='+(sb?sb.updating:'-')+\n"
    "    ' ms='+ms.readyState+' buffered='+br+'\\n'+\n"
    "    't='+v.currentTime.toFixed(2)+' vw='+v.videoWidth+'x'+\n"
    "    v.videoHeight+' rs='+v.readyState+' paused='+v.paused+'\\n'+\n"
    "    'verr='+(v.error?v.error.code+':'+v.error.message:'-')+\n"
    "    ' err='+(err||'-')+(note?' note='+note:'');\n"
    "}\n"
    "function pump(){\n"
    "  if(!sb||sb.updating||!q.length)return;\n"
    "  try{\n"
    "    const b=sb.buffered;\n"
    "    if(b.length){\n"
    "      const end=b.end(b.length-1);\n"
    "      /* A live stream does not start at zero for a client that joined\n"
    "         late: the first fragment it was given carries the timestamp of\n"
    "         the frame it was, which is however long the stream had been\n"
    "         running. The player, meanwhile, sits at zero -- where there is\n"
    "         no data and so nothing to show, which is a black picture that\n"
    "         looks like a broken stream and is not one. Put it where the\n"
    "         data is, which is what any live player has to do. */\n"
    "      if(v.currentTime<b.start(0)){\n"
    "        v.currentTime=b.start(0);\n"
    "        note='seek to '+b.start(0).toFixed(2);\n"
    "      }\n"
    "      /* And it does not wait. A browser plays at exactly real time, so\n"
    "         anything that makes it stall -- a slow append, a busy machine,\n"
    "         a burst of frames -- is never made up: the picture simply runs\n"
    "         later and later, and stays there. Bringing the playhead "
    "forward\n"
    "         when the backlog grows is the difference between a stream that\n"
    "         ends up a second late for ever and one that is about as late "
    "as\n"
    "         it was when it started. */\n"
    "      else if(end-v.currentTime>MAXLAG){\n"
    "        const behind=end-v.currentTime;\n"
    "        v.currentTime=end-MAXLAG/2;\n"
    "        note='caught up from '+behind.toFixed(2)+'s';\n"
    "      }\n"
    "      v.play().catch(e=>{note='play:'+e.name;});\n"
    "      if(v.currentTime-b.start(0)>4){\n"
    "        note='removing';\n"
    "        sb.remove(0,v.currentTime-1);\n"
    "        return;\n"
    "      }\n"
    "    }\n"
    "    const f=q.shift();\n"
    "    sb.appendBuffer(f);\n"
    "    note='appended '+f.length+'B';\n"
    "  }catch(e){err=e.name+': '+e.message;}\n"
    "  say();\n"
    "}\n"
    "ms.addEventListener('sourceopen',()=>{\n"
    "  try{\n"
    "    sb=ms.addSourceBuffer('video/mp4; codecs=\"%s\"');\n"
    "  }catch(e){err='addSourceBuffer: '+e.name+': "
    "'+e.message;say();return;}\n"
    "  sb.mode='segments';\n"
    "  sb.addEventListener('updateend',()=>{note='updateend';pump();});\n"
    "  sb.addEventListener('error',()=>{err='sourcebuffer error';say();});\n"
    "  sb.addEventListener('abort',()=>{err='sourcebuffer abort';say();});\n"
    "  const ws=new WebSocket('ws://'+location.host+'/stream');\n"
    "  ws.binaryType='arraybuffer';\n"
    "  ws.onopen=()=>{note='ws open';say();};\n"
    "  ws.onmessage=e=>{q.push(new Uint8Array(e.data));\n"
    "    rx+=e.data.byteLength;msgs++;say();pump();};\n"
    "  ws.onerror=()=>{err='ws error';say();};\n"
    "  ws.onclose=e=>{err='ws closed code='+e.code;say();};\n"
    "});\n"
    "ms.addEventListener('sourceclose',()=>{err='media source "
    "closed';say();});\n"
    "setInterval(say,500);\n"
    "</script></body></html>\n";

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
 *   nothing.  It is only defensible because a client can be brought up to
 *   date from the primer when it comes back.
 *
 ****************************************************************************/

static bool ws_queue(FAR struct camenc_ws_s *ws,
                     FAR struct camenc_ws_client_s *c, const uint8_t *data,
                     size_t len)
{
  if (c->tx_len + len > c->tx_size)
    {
      _warn("CAMENC WS: %zu bytes do not fit a %zu-byte buffer, "
            "dropping\n",
            c->tx_len + len, c->tx_size);
      ws_close_client(ws, c);
      ws->clients_dropped++;
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
  /* SIZE_MAX is a compile-time constant here, so the frame is too: the
   * page at its largest, plus the codec string in place of the two
   * characters that stand for it.
   */

  char body[sizeof(g_page) + CAMENC_PAGE_SLACK];
  char head[256];
  int blen;
  int hlen;

  /* snprintf rather than a fixed reply so a codec string longer than
   * expected cannot run past the end of the buffer it is put in.
   */

  blen = snprintf(body, sizeof(body), g_page,
                  ws->codec[0] != '\0' ? ws->codec : "avc1.42c016");
  if (blen < 0 || (size_t)blen >= sizeof(body))
    {
      return;
    }

  hlen = snprintf(head, sizeof(head),
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Content-Length: %d\r\n"
                  "Cache-Control: no-store\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  blen);
  if (hlen < 0 || (size_t)hlen >= sizeof(head))
    {
      return;
    }

  if (ws_queue(ws, c, (FAR const uint8_t *)head, (size_t)hlen) &&
      ws_queue(ws, c, (FAR const uint8_t *)body, (size_t)blen))
    {
      /* The page is all this connection was for.  It is written out by
       * the next flush and then closed, because a fresh connection is
       * what the page's WebSocket will make anyway.
       */

      c->closing = true;
      ws_flush(ws, c);
    }
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
      return;
    }

  c->upgraded = true;
  ws->clients_served++;

  /* Catch this client up before it is given anything live.
   *
   * A client that has just connected has no decoder state, so it cannot
   * be given the middle of a stream: it is given the segments that start
   * one, which are the initialisation segment and everything since the
   * last segment a stream may start on.  Then the live segments follow.
   * Without this the first thing a late viewer sees is a picture that
   * refers to pictures it never received.
   */

  if (ws->primable && ws->primer_len > 0)
    {
      _info("CAMENC WS: new client primed with %zu bytes\n", ws->primer_len);
      ws_frame(ws, c, CAMENC_WS_OP_BIN, ws->primer, ws->primer_len);
    }
  else
    {
      _info("CAMENC WS: new client, nothing to prime with yet\n");
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

  ws->primer = malloc(CAMENC_WS_PRIMER_SIZE);
  if (ws->primer == NULL)
    {
      camenc_ws_stop(ws);
      return -ENOMEM;
    }

  ws->primer_size = CAMENC_WS_PRIMER_SIZE;
  ws->primable = true;

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

  fcntl(ws->listen_fd, F_SETFL, fcntl(ws->listen_fd, F_GETFL, 0) | O_NONBLOCK);

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

  free(ws->primer);
  ws->primer = NULL;
  ws->primer_len = 0;
  ws->primer_init_len = 0;
}

void camenc_ws_poll(FAR struct camenc_ws_s *ws)
{
  struct sockaddr_in addr;
  socklen_t alen;
  int i;

  if (ws == NULL || ws->listen_fd < 0)
    {
      return;
    }

  /* Take whatever is waiting.  Non-blocking, so this is one accept per
   * connection and never a wait.
   */

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

      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

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

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0)
        {
          continue;
        }

      ws_read_client(ws, c);

      if (c->fd < 0)
        {
          continue;
        }

      ws_flush(ws, c);
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

  /* Keep what a later client will need, before sending anything.
   *
   * The initialisation segment opens the primer.  After that a segment
   * that a stream may start on *replaces* it: the new sequence is that
   * segment and what follows it, not that one appended to the previous
   * group.  A primer that accumulated every key segment would replay the
   * stream from the beginning to each new client -- or rather from
   * wherever it first filled up, which is worse, because it would look
   * deliberate.
   */

  if (len + CAMENC_WS_TX_HEADER > ws->tx_size)
    {
      /* A segment no client can be sent must not start a primer either:
       * admitting a client on one would take it only to drop it straight
       * away again, and it would be sent back here for the next one, and
       * the next.  So the primer is given up until a segment fits, and
       * the send below drops whoever is connected rather than passing
       * them an incomplete stream.
       */

      if (!ws->warned_oversize)
        {
          _warn("CAMENC WS: %zu-byte segments do not fit a %zu-byte "
                "buffer;"
                " no client can be served at this size\n",
                len, ws->tx_size);
          ws->warned_oversize = true;
        }

      ws->primer_len = 0;
      ws->primable = false;
    }
  else if (seg == CAMENC_SEG_INIT)
    {
      if (len <= ws->primer_size)
        {
          memcpy(ws->primer, data, len);
          ws->primer_len = len;
          ws->primer_init_len = len;
          ws->primable = true;
        }
      else
        {
          ws->primable = false;
        }
    }
  else if (key)
    {
      if (ws->primer_init_len + len <= ws->primer_size)
        {
          memcpy(ws->primer + ws->primer_init_len, data, len);
          ws->primer_len = ws->primer_init_len + len;
          ws->primable = true;
        }
      else
        {
          /* A single group of pictures that does not fit means no client
           * can be brought up to date from here, so none is admitted
           * until the next segment a stream may start on begins a fresh
           * primer. Refusing is the honest outcome: the alternative is
           * admitting a client and feeding it a stream it cannot decode.
           */

          _warn("CAMENC WS: primer full (%zu bytes), refusing clients\n",
                ws->primer_len);
          ws->primer_len = 0;
          ws->primable = false;
        }
    }
  else if (ws->primable)
    {
      if (ws->primer_len + len <= ws->primer_size)
        {
          memcpy(ws->primer + ws->primer_len, data, len);
          ws->primer_len += len;
        }
      else
        {
          ws->primer_len = 0;
          ws->primable = false;
        }
    }

  /* Then to everyone connected. */

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0 || !c->upgraded)
        {
          continue;
        }

      ws_frame(ws, c, CAMENC_WS_OP_BIN, data, len);

      if (c->fd >= 0)
        {
          ws_flush(ws, c);
        }
    }

  return 0;
}
