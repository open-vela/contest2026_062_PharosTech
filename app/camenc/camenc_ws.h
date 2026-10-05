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

#include <poll.h>
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

/* The longest control message that will be read from a client.
 *
 * A message is a line of named values -- see camenc_ws_cmd_t -- and the
 * longest one this application is sent is under forty bytes.  The bound is
 * here rather than in the application because the buffer it is unmasked into
 * is the server's, and a message longer than it is dropped rather than
 * truncated: half a command is a different command.
 */

#define CAMENC_WS_CMD_MAX 96

/* The largest initialisation segment the server will hold for a client that
 * arrives after it.
 *
 * The initialisation segment is the one thing a late client cannot do
 * without: it carries the parameter sets and the description of the track,
 * and a decoder that has just started has no other way to learn what the
 * stream is.  Four kilobytes is room to be wrong about its size without
 * being room for anything that is not one.
 */

#define CAMENC_WS_INIT_MAX 4096

struct camenc_ws_client_s
{
  int fd;
  bool upgraded; /* the handshake has been answered           */
  bool closing;  /* close once the buffer has gone out        */

  /* Whether this client has been brought up to the stream yet.
   *
   * A client that has just connected has no decoder state, so there are
   * exactly two things it can be given: the initialisation segment, and a
   * segment a stream may start on.  Anything between them is a picture that
   * refers to pictures it never received.
   *
   * So it is given the first and waits for the second, and the segments
   * published while it waits are dropped for it rather than queued for it.
   * Queueing them is what used to break this.  The catch-up data and the
   * live data shared one transmit buffer, and the live frames alone filled
   * it in about three frames -- five hundredths of a second -- so a client
   * was dropped while it was still completing its handshake.  What a
   * browser reports for that is that it cannot connect at all, which is
   * what it looked like from the outside.
   *
   * Waiting costs the client one group of pictures before the picture
   * appears, which is fifteen frames here -- half a second at the mode's
   * rate.  It is the price of not replaying, and replaying cannot be made to
   * work: the client's window is bounded by how fast its socket drains, and a
   * replay is by definition larger than the frames arriving during it.
   */

  bool have_init;
  bool waiting;

  /* What this connection has managed, for the periodic report.
   *
   * `sent` is what the socket took, and `skipped` is what did not fit and
   * was thrown away.  They are the two numbers that say whether a client is
   * keeping up, which is otherwise invisible: a client that receives nothing
   * and a client that receives everything look the same from the capture
   * loop, because in both cases the loop's own work is identical.
   */

  uint64_t sent;
  uint32_t skipped;

  /* What `sent` was when it was last seen to change, and when that was.
   *
   * A socket that takes nothing for long enough has a peer that is not
   * reading, and that is the one signal in this server that does not have to
   * be asked of the stack: it is read from the server's own counter.  poll()
   * may not report the descriptor at all, and a peer that has gone can leave
   * nothing behind for recv() to find, so "nothing has moved" is what is
   * left.  See camenc_ws_expire().
   */

  uint64_t sent_seen;
  uint64_t sent_changed;

  uint8_t rx[CAMENC_WS_RX_SIZE];
  size_t rx_len;

  size_t tx_size;
  size_t tx_len;  /* bytes waiting in tx                       */
  size_t tx_sent; /* bytes of it already written               */
  uint8_t *tx;
};

/* Where a control message from a client is handed.
 *
 * The server carries bytes and knows nothing about what they mean, so the
 * text of a message is passed straight to the application -- the only part
 * that knows what a valid one is.  The callback returns nothing on purpose:
 * a message the application does not understand is not the server's to
 * complain about, and is certainly not a reason to drop a client that is
 * watching the stream perfectly well.
 *
 * The text is NUL-terminated and is not kept afterwards, so a callback that
 * wants to hold on to it has to copy it.
 */

typedef void (*camenc_ws_cmd_t)(FAR const char *text, FAR void *arg);

/* What the server did over one report window, and how long it took.
 *
 * Counted inside camenc_ws.c, where each event is known, and copied out by
 * camenc_ws_take_phases() once per report, so that what the report says is a
 * window rather than a total running since the server started.  The counters
 * the report accumulates are these, added up.
 *
 * The read and write paths each keep their own view of the sockets:
 * `read_eagain` beside `reads` says how often a client was asked and had
 * nothing to give, and `send_eagain` beside `sends` says the same for the
 * far more interesting case, a socket that would not take what was queued
 * for it.
 *
 * `flushes` is deliberately only the flushes a POLLOUT produced, so that a
 * zero beside a non-zero `pending` in the server's own line says the stack
 * never reported the socket writable.  The retry that does not wait for an
 * event is not counted here -- see camenc_ws_flush_all() -- because telling
 * the two apart is the point of the distinction.
 *
 * `conn_err` counts POLLERR reports and `conn_lost` the connections actually
 * let go of, so the two differ by the reports the socket did not confirm:
 * see the note on SO_ERROR in camenc_ws_ready().  `replaced` and `expired`
 * are the two ways a client is let go of without the client asking -- a new
 * stream request taking the one seat, and a socket that stopped taking what
 * was queued for it -- and `refused` is a connection the listening socket
 * handed over while every slot was taken.
 */

struct camenc_ws_phases_s
{
  uint64_t accept_us;  /* time taking connections          */
  uint64_t accept_max; /* the slowest of them              */
  uint64_t read_us;    /* time reading from clients        */
  uint64_t read_max;
  uint64_t flush_us; /* time writing to clients          */
  uint64_t flush_max;

  uint32_t accepted;    /* connections taken                */
  uint32_t refused;     /* ... and turned away, no slot     */
  uint32_t reads;       /* reads from clients               */
  uint32_t read_eagain; /* ... that had nothing to give     */
  uint32_t read_closed; /* clients that closed, either way  */
  uint64_t read_bytes;  /* bytes read from clients          */
  uint32_t conn_err;    /* POLLERR reports                  */
  uint32_t conn_lost;   /* ... that the socket confirmed    */
  uint32_t replaced;    /* seats taken by a newer request   */
  uint32_t expired;     /* clients that stopped taking      */
  uint32_t flushes;     /* writes a POLLOUT produced        */
  uint32_t sends;       /* send() calls from any flush      */
  uint32_t send_eagain; /* ... that the socket refused      */
  uint64_t sent;        /* bytes handed to the sockets      */
};

/* The whole of the server's state, and most of it is the receive buffer of
 * every client it may have: about eight kilobytes, which is more than the
 * stack the application task is given.  So it belongs in .bss or on the heap
 * and never in a frame -- a frame of that size does not fit, and overflowing
 * it corrupts whatever the heap put next.  camenc_main.c keeps it in .bss,
 * which is where that was learned.
 */

struct camenc_ws_s
{
  int listen_fd;
  uint16_t port;

  struct camenc_ws_client_s clients[CAMENC_WS_MAX_CLIENTS];

  /* The initialisation segment, held for clients that arrive after it. */

  uint8_t *init_seg;
  size_t init_len;

  size_t tx_size; /* per-client transmit buffer size         */

  /* Said once, however many segments are too large, because if the encoder
   * produces them at the configured size it produces them every time.
   */

  bool warned_oversize;

  /* Said once for the same reason: a client whose buffer is full has it
   * full again on the next frame.
   */

  bool warned_full;

  /* What to tell a player the stream is, for the page to put in its
   * MediaSource.  It has to be the stream's own profile, compatibility and
   * level; see camenc_stream_codec_string(), which builds it.
   */

  char codec[16];

  uint32_t clients_served;
  uint32_t clients_dropped;

  /* The server's account of the report window just ended, which
   * camenc_ws_take_phases() empties as it reads it.  See the structure.
   */

  struct camenc_ws_phases_s phases;

  /* Where a text frame from a client goes.  Null until one is set, in which
   * case text frames are read and discarded.
   */

  camenc_ws_cmd_t on_command;
  FAR void *command_arg;
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

/* Say where a control message from a client should be handed. */

void camenc_ws_set_command(FAR struct camenc_ws_s *ws, camenc_ws_cmd_t fn,
                           FAR void *arg);

void camenc_ws_stop(FAR struct camenc_ws_s *ws);

/* Close every connection but leave the server listening.
 *
 * Used when the stream changes shape underneath the clients.  A new frame size
 * means a new initialisation segment and a different sample description, and a
 * browser that keeps appending to the one SourceBuffer it built from the old
 * codec configuration ends up with a buffer that cannot describe both.  There
 * is no message that tells it to start again -- the remedy is the one every
 * page already has for a server that went away, which is to reconnect -- so
 * the connections are closed and the page rebuilds.
 *
 * The cached initialisation segment goes with them, because it describes the
 * stream that has ended; a client that reconnects before the next one has
 * produced its own would otherwise be started on the wrong picture size.
 *
 * The transmit buffers are kept: they belong to the slot rather than to the
 * connection, and a client that comes back gets one that is already there.
 */

void camenc_ws_drop_clients(FAR struct camenc_ws_s *ws);

/* Say which of the server's sockets to wait on, and act on the ones that
 * became ready.
 *
 * The server has no thread of its own and does not wait: it asks to be put
 * into the caller's poll() and is handed the ready set back afterwards.  That
 * is what lets one wait cover the camera and every client together, so a
 * connection or a command is answered when it arrives rather than whenever
 * the frame in progress happens to finish.
 *
 * camenc_ws_fds() writes at most `max` entries and returns how many it wrote,
 * so a caller with a fixed array can give the server whatever is left after
 * its own descriptors.  camenc_ws_ready() is then handed that same array with
 * poll()'s ready flags in it.  Neither one blocks.
 */

int camenc_ws_fds(FAR struct camenc_ws_s *ws, FAR struct pollfd *fds, int max);
void camenc_ws_ready(FAR struct camenc_ws_s *ws, FAR struct pollfd *fds,
                     int n);

/* Hand one segment to every client.
 *
 * `key` says a stream may be started on this segment, which is what a client
 * that has just connected is waiting for.  Shaped like camenc_stream's own
 * callback so that the two connect with one line.
 */

int camenc_ws_publish(FAR struct camenc_ws_s *ws, enum camenc_seg_e seg,
                      const uint8_t *data, size_t len, bool key);

/* Send one text frame to every client, as far as it fits.
 *
 * This is not a segment and is not allowed to cost a client: one whose
 * transmit buffer is full when the exposure changes misses that readout and
 * keeps its picture.  `text` need not be NUL-terminated; `len` is the whole
 * of it.
 */

void camenc_ws_status(FAR struct camenc_ws_s *ws, FAR const char *text,
                      size_t len);

/* Print one line about how the clients are doing: how many are connected,
 * how many bytes are waiting for a socket that will not take them, how many
 * have gone out, and how many frames have been thrown away for not fitting.
 *
 * Worth a line in the report rather than a debug-only facility, because a
 * stream that is not reaching a browser looks exactly like a stream that is:
 * the capture loop's own timings are the same either way, and the only place
 * the difference shows is here.
 */

void camenc_ws_report(FAR struct camenc_ws_s *ws);

/* Hand over the report window's counts and begin a new one.  Called once per
 * report by the loop, which accumulates what it is given into its own totals;
 * what is left behind is a window and not a total.
 */

void camenc_ws_take_phases(FAR struct camenc_ws_s *ws,
                           FAR struct camenc_ws_phases_s *phases);

/* Write out whatever is still queued for anyone, once a round.
 *
 * Not an optimisation and not a workaround: with a send buffer that has a
 * limit, a reply larger than that limit cannot be finished in one call, and
 * something has to call again.  The events that could -- the socket becoming
 * writable again -- are not a thing to rely on here, and the publish path
 * only ever looks at upgraded clients, so a page reply has nothing else to
 * drive it.  Costs a loop over the clients and a send() only when there is
 * something to send.  Returns the number of clients it found with something
 * queued.
 */

unsigned int camenc_ws_flush_all(FAR struct camenc_ws_s *ws);

/* Let go of clients that have stopped taking what is queued for them.
 *
 * Called once a round, and the only mechanism here that does not depend on
 * the stack reporting anything: a socket that has accepted no bytes for
 * CAMENC_WS_STALL_US while bytes are waiting has a peer that is not reading,
 * whether it has gone or is merely stuck, and a live stream has nothing to
 * send such a client.  With one viewer that is not a loss -- the page
 * reconnects -- and it is the difference between a slot and a share of the
 * buffer pool being held indefinitely and being given back.  Returns the
 * number of clients let go of.
 */

unsigned int camenc_ws_expire(FAR struct camenc_ws_s *ws);

#endif /* __APP_CAMENC_CAMENC_WS_H */
