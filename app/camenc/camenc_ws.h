/****************************************************************************
 * app/camenc/camenc_ws.h
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
 * A WebSocket server that carries a video stream, and the page that plays it.
 *
 * One port serves both: a browser gets the page with a plain GET, and the
 * page's WebSocket then upgrades the connection and is fed the stream.  That
 * is why there is one server rather than an HTTP server plus a separate
 * stream socket -- the second would need its own port, its own address to
 * tell the page, and its own notion of when a client has gone away.
 *
 * Why WebSocket and not HLS: with HLS the server writes segment files, keeps
 * a playlist, serves byte ranges and varies its latency with the segment
 * length.  Here a client connects and is handed segments on one connection.
 * No file system, no playlist, sub-second latency, and the browser side is
 * the page below.  The price is that a client that falls behind has to be
 * dropped rather than throttled, which the buffer bound below decides.
 *
 * This module knows nothing about cameras or encoders.  It is handed
 * segments and a flag saying whether a stream may start on one, exactly as
 * the muxer's own callback is; the two shapes match so that connecting them
 * is one function.
 *
 ****************************************************************************/

#ifndef __APP_CAMENC_CAMENC_WS_H
#define __APP_CAMENC_CAMENC_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camenc_stream.h"

/* How many clients may be connected at once.  Each one costs a socket and a
 * transmit buffer, so this is the knob that bounds the whole server's memory.
 */

#define CAMENC_WS_MAX_CLIENTS 4

/* Per-client transmit buffer, chosen by the caller.
 *
 * A segment is copied in whole and then written out over however many calls
 * it takes, which is what bounds the server's memory: a segment that does not
 * fit is not queued, and the client that could not take it is dropped.
 *
 * Dropping is the honest answer for a live stream.  The alternative is to
 * stop taking frames from the camera until the slowest viewer catches up,
 * which turns one slow viewer into a stalled camera.  A dropped client
 * reconnects and is caught up again, which costs it a moment of video and
 * costs the capture loop nothing.
 *
 * The size is the caller's rather than a constant here because only the
 * caller knows how large a segment can be -- it is the same bound it gives
 * the muxer -- and only the caller knows how much memory there is for it.
 * Whatever is chosen, it has to be at least one segment plus a frame header,
 * or no client can ever be served.
 */

#define CAMENC_WS_TX_HEADER 16

/* Per-client receive buffer.
 *
 * Nothing is expected from a client after the handshake -- the stream only
 * goes one way -- but its data still has to be read, because that is how its
 * going away is noticed, and because a close frame has to be answered.  A
 * browser's request is well under a kilobyte and its later frames are two
 * bytes or none.
 */

#define CAMENC_WS_RX_SIZE 2048

/* How far back a late client can be caught up.
 *
 * A client that connects mid-stream cannot be given the stream from the
 * beginning, and must not be given it from the middle either: it has to start
 * where its decoder can.  So the segments needed to start a stream are kept
 * -- the initialisation segment, and then every segment since the last one a
 * stream may start on -- and a client that arrives is given those before it
 * is given anything live.
 *
 * Today every frame is a key frame, so this holds one segment plus the
 * initialisation segment and the bound below is never approached.  It is
 * sized for the day that stops being true: a second of pictures at a
 * reasonable bit rate.
 */

#define CAMENC_WS_PRIMER_SIZE (256 * 1024)

struct camenc_ws_client_s
{
  int fd;
  bool upgraded; /* the handshake has been answered           */
  bool closing;  /* close once the buffer has gone out        */

  uint8_t rx[CAMENC_WS_RX_SIZE];
  size_t rx_len;

  size_t tx_size;
  size_t tx_len;  /* bytes waiting in tx                       */
  size_t tx_sent; /* bytes of it already written               */
  uint8_t *tx;
};

/* The whole of the server's state, and it is 8464 bytes: the receive buffer
 * of every client it may have lives in it.  That is larger than the stack
 * the application task is given, so it belongs in .bss or on the heap and
 * never in a frame -- a frame of that size does not fit, and overflowing it
 * corrupts whatever the heap put next.  camenc_main.c keeps it in .bss,
 * which is where that was learned.
 */

struct camenc_ws_s
{
  int listen_fd;
  uint16_t port;

  struct camenc_ws_client_s clients[CAMENC_WS_MAX_CLIENTS];

  /* The segment a new client has to be given first, and how long it is. */

  uint8_t *primer;
  size_t primer_len;
  size_t primer_init_len; /* where the initialisation segment ends   */
  size_t primer_size;

  size_t tx_size; /* per-client transmit buffer size         */

  /* Whether the segments since the last key segment fit in the primer.
   * If one alone did not, there is no point admitting a client until the
   * next key segment resets this, so it is refused until then.
   *
   * A segment too large to be sent is treated the same way: admitting a
   * client on one would only drop it again on the send, so the primer has
   * to refuse what the transmit buffer cannot carry, or the two limits
   * disagree and a client is told to start somewhere it cannot be sent.
   */

  bool primable;

  /* Said once, however many segments are too large, because if the encoder
   * produces them at the configured size it produces them every time.
   */

  bool warned_oversize;

  /* What to tell a player the stream is, for the page to put in its
   * MediaSource.  It has to be the stream's own profile, compatibility and
   * level; see camenc_stream_codec_string(), which builds it.
   */

  char codec[16];

  uint32_t clients_served;
  uint32_t clients_dropped;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Open the port and start listening.
 *
 * `tx_size` is the largest segment this server will carry; one that does not
 * fit drops the client it was meant for.  Returns 0 or a negative errno.
 */

int camenc_ws_start(FAR struct camenc_ws_s *ws, uint16_t port, size_t tx_size);

/* Name the stream to the page, as an MP4 codec string such as
 * "avc1.42c016".  A browser will refuse a MediaSource whose codec string
 * does not match the stream it is fed, so this has to come from the stream's
 * own parameter set rather than be guessed.
 */

void camenc_ws_set_codec(FAR struct camenc_ws_s *ws, FAR const char *codec);

void camenc_ws_stop(FAR struct camenc_ws_s *ws);

/* Service the server: accept new connections, read what clients have sent
 * (which is how their going away is noticed), and push out what is queued.
 *
 * Called from the capture loop between frames, so it must not block.
 */

void camenc_ws_poll(FAR struct camenc_ws_s *ws);

/* Hand one segment to every client.
 *
 * `key` says a stream may be started on this segment, which is what makes the
 * primer start here rather than earlier.  Shaped like camenc_stream's own
 * callback so that the two connect with one line.
 */

int camenc_ws_publish(FAR struct camenc_ws_s *ws, enum camenc_seg_e seg,
                      const uint8_t *data, size_t len, bool key);

#endif /* __APP_CAMENC_CAMENC_WS_H */
