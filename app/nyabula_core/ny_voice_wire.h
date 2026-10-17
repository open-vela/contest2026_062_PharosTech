/****************************************************************************
 * app/nyabula_core/ny_voice_wire.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
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

#ifndef __NYABULA_CORE_NY_VOICE_WIRE_H
#define __NYABULA_CORE_NY_VOICE_WIRE_H

/****************************************************************************
 * The control domain's client for the speech services of the compute
 * domain: ASR (3), TTS (4) and KWS (10) of tools/amp/protocol.
 *
 * One ny_voice_wire_s is one conversation with one service.  It sends
 * through callbacks rather than through a descriptor, because there are
 * three ways to reach the peer and the sequences must be the same on all of
 * them: a port of the Core compute service (ny_voice.c, and nyampctl while
 * that service runs), the RPMsg endpoint itself (nyampctl standalone), and a
 * socket pair to nyampd's real services on the host (the wire test).
 *
 * A port delivers only the frames of the request it last sent, and every
 * message of one speech request shares the BEGIN's request id, so one
 * conversation needs exactly one port.  Events that arrive while a response
 * is awaited -- a DETECTED between a PUSH and its acknowledgement -- are
 * kept and handed out by ny_voice_wire_event() in order.
 *
 * Nothing here includes a NuttX header.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "nyamp_protocol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Events kept while a response is awaited.  The services queue at most 64
 * partials and one round trip is short; what does not fit is shed the way
 * the service itself sheds (advisory frames first, never a FINISH).
 */

#define NY_VOICE_WIRE_STASH 16

/* How long one request/response round trip may take.  The services answer
 * from their transport loop, before any inference.
 */

#define NY_VOICE_WIRE_ACK_MS 5000

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_voice_wire_io_s
{
  int (*send)(void *arg, const uint8_t *wire, size_t size);
  ssize_t (*recv)(void *arg, uint8_t *wire, size_t capacity, int timeout_ms);
  uint64_t (*request_id)(void *arg);

  /* The compute generation as the link knows it, 0 when unknown.  It goes
   * into every header, so a daemon that restarted answers STALE_GENERATION
   * instead of NOT_READY, and a change ends a wait early.  May be NULL.
   */

  uint32_t (*generation)(void *arg);

  /* The caller wants a long wait (a model load) to end.  May be NULL. */

  bool (*interrupted)(void *arg);

  /* Pull progress of a LOAD by logical name.  May be NULL. */

  void (*progress)(void *arg, uint64_t done, uint64_t total,
                   uint32_t bytes_per_second);
  void *arg;

  uint8_t *arena;    /* The shared region, NULL when it is not mapped. */
  size_t arena_size;
  size_t arena_header; /* Leading bytes no window may touch. */
};

struct ny_voice_wire_frame_s
{
  uint16_t size;
  uint8_t data[NYAMP_RPMSG_MTU];
};

struct ny_voice_wire_s
{
  struct ny_voice_wire_io_s io;
  uint64_t request_id; /* The open BEGIN or SYNTH_TEXT, 0 when none. */
  uint32_t generation; /* The compute generation it was opened under. */
  unsigned int head;
  unsigned int count;
  uint32_t shed;       /* Events dropped because the stash was full. */
  struct ny_voice_wire_frame_s stash[NY_VOICE_WIRE_STASH];
};

struct ny_voice_wire_kws_s
{
  float threshold;              /* 0 selects the service default. */
  float score;
  uint16_t max_active_paths;
  uint16_t num_trailing_blanks;
};

/* One decoded event.  `text` points into the frame handed to
 * ny_voice_wire_event() and is not NUL terminated.
 */

enum ny_voice_wire_kind_e
{
  NY_VOICE_WIRE_NONE = 0,
  NY_VOICE_WIRE_DETECTED,
  NY_VOICE_WIRE_PARTIAL,
  NY_VOICE_WIRE_PCM,
  NY_VOICE_WIRE_FINISH
};

struct ny_voice_wire_event_s
{
  enum ny_voice_wire_kind_e kind;
  uint16_t service;
  uint64_t request_id;

  struct nyamp_kws_detected_s detected; /* DETECTED */

  uint16_t flags;                       /* PARTIAL: NYAMP_ASR_PARTIAL_* */
  uint32_t consumed_samples;
  const char *text;
  size_t text_length;

  struct nyamp_buffer_s buffer;         /* PCM */
  uint8_t descriptor[NYAMP_BUFFER_SIZE]; /* PCM: echoed by RELEASE as is */
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t valid_samples;

  int32_t status;                       /* FINISH */
  uint32_t sequence;                    /* Every kind */
  uint32_t total_samples;               /* FINISH of a TTS request */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

void ny_voice_wire_init(struct ny_voice_wire_s *wire,
                        const struct ny_voice_wire_io_s *io);

/* A nyamp_model_status_e as the negated errno this module returns. */

int ny_voice_wire_errno(int32_t status);

/****************************************************************************
 * Name: ny_voice_wire_load / ny_voice_wire_unload
 *
 * Description:
 *   LOAD a model directory ("asr", "tts", "kws", or an absolute path the
 *   compute domain can reach) and wait for the delayed response; pull
 *   progress goes to io.progress.  `kws` is only used for NYAMP_SERVICE_KWS
 *   and may be NULL for the defaults.  UNLOAD retries while the service
 *   reports BUSY, which it does until a request it just ended has finished.
 *
 *   -ENOENT the directory is not under /data/models, -EREMOTEIO the backend
 *   could not open it, -ENOTSUP the daemon has no such backend, -EBUSY the
 *   shared slot is taken or another model is loaded, -ECANCELED interrupted,
 *   -ECONNRESET the compute domain restarted, -ETIMEDOUT.
 *
 ****************************************************************************/

int ny_voice_wire_load(struct ny_voice_wire_s *wire, uint16_t service,
                       const char *directory,
                       const struct ny_voice_wire_kws_s *kws,
                       int timeout_ms);
int ny_voice_wire_unload(struct ny_voice_wire_s *wire, uint16_t service);

/* KWS: the always-on stream.  `samples` are 16 kHz mono; they cross the
 * link as float32 unless `s16` was given to begin.
 */

int ny_voice_wire_kws_labels(struct ny_voice_wire_s *wire, char *out,
                             size_t size);
int ny_voice_wire_kws_begin(struct ny_voice_wire_s *wire, bool s16,
                            uint32_t window_samples,
                            struct nyamp_buffer_s *grant);
int ny_voice_wire_kws_push(struct ny_voice_wire_s *wire,
                           const struct nyamp_buffer_s *grant,
                           const int16_t *samples, size_t count,
                           uint32_t sequence, bool discontinuity,
                           uint64_t stream_sample, uint64_t *next_sample);
int ny_voice_wire_kws_end(struct ny_voice_wire_s *wire);

/* ASR: pushed (begin returns the grant) or attached to the KWS stream. */

int ny_voice_wire_asr_begin(struct ny_voice_wire_s *wire, bool s16,
                            uint32_t max_samples,
                            struct nyamp_buffer_s *grant);
int ny_voice_wire_asr_attach(struct ny_voice_wire_s *wire,
                             uint32_t max_samples, uint64_t start_sample);
int ny_voice_wire_asr_push(struct ny_voice_wire_s *wire,
                           const struct nyamp_buffer_s *grant,
                           const int16_t *samples, size_t count,
                           uint32_t sequence);
int ny_voice_wire_asr_end(struct ny_voice_wire_s *wire, uint64_t end_sample);

/* TTS: send the text, then take PCM events and release each window. */

int ny_voice_wire_tts_say(struct ny_voice_wire_s *wire, const char *text,
                          size_t length, uint32_t speaker_id, float speed,
                          uint32_t window_samples);
int ny_voice_wire_tts_release(struct ny_voice_wire_s *wire,
                              const struct ny_voice_wire_event_s *event);

/****************************************************************************
 * Name: ny_voice_wire_cancel
 *
 * Description:
 *   Give up the open request with a CANCEL-kind frame, which has no
 *   response: it is what a thread other than the one reading the port can
 *   send without taking that thread's frames.  The FINISH event still
 *   arrives and closes the request.
 *
 ****************************************************************************/

int ny_voice_wire_cancel(struct ny_voice_wire_s *wire, uint16_t service);

/****************************************************************************
 * Name: ny_voice_wire_event
 *
 * Description:
 *   The next event of the open request, decoded; `frame` keeps the bytes
 *   event->text points into.  Returns 1, 0 on timeout, or a negated errno:
 *   -ECONNRESET when the compute generation changed while waiting.  A
 *   FINISH closes the request.
 *
 ****************************************************************************/

int ny_voice_wire_event(struct ny_voice_wire_s *wire,
                        struct ny_voice_wire_frame_s *frame,
                        struct ny_voice_wire_event_s *event, int timeout_ms);

/****************************************************************************
 * Name: ny_voice_wire_window
 *
 * Description:
 *   The address of a window the compute domain granted or published, after
 *   the two checks that protect the link itself: it must not touch the arena
 *   header and must not leave the arena.  WHERE it sits is the compute
 *   domain's business, as for the blob windows.
 *
 ****************************************************************************/

int ny_voice_wire_window(const struct ny_voice_wire_s *wire,
                         const struct nyamp_buffer_s *buffer, size_t bytes,
                         uint8_t **window);

/****************************************************************************
 * Name: ny_voice_wire_transcript
 *
 * Description:
 *   Apply one PARTIAL frame to the transcript: RESYNC replaces it, anything
 *   else appends.  What does not fit is cut between two characters.
 *
 ****************************************************************************/

void ny_voice_wire_transcript(char *transcript, size_t size,
                              const struct ny_voice_wire_event_s *event);

#ifdef __cplusplus
}
#endif

#endif /* __NYABULA_CORE_NY_VOICE_WIRE_H */
