/****************************************************************************
 * app/nyabula_core/ny_voice_wire.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <string.h>
#include <time.h>

#include "ny_voice_dsp.h"
#include "ny_voice_wire.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* One slice of a wait: how late an interruption or a new compute generation
 * is noticed.
 */

#define NY_VOICE_WIRE_SLICE_MS   200

/* UNLOAD answers BUSY until the request it just ended has finished. */

#define NY_VOICE_WIRE_UNLOAD_TRIES 20

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t ny_voice_wire_now_ms(void);
static uint32_t ny_voice_wire_generation(const struct ny_voice_wire_s *wire);
static void ny_voice_wire_keep(struct ny_voice_wire_s *wire,
                               const struct nyamp_header_s *header,
                               const uint8_t *frame, size_t size);
static int ny_voice_wire_call(struct ny_voice_wire_s *wire, uint16_t service,
                              uint16_t opcode, uint64_t request_id,
                              const uint8_t *body, size_t body_size,
                              int timeout_ms, bool interruptible,
                              uint8_t *reply, size_t *reply_size);
static int ny_voice_wire_begin(struct ny_voice_wire_s *wire, uint16_t service,
                               uint16_t opcode, const uint8_t *body,
                               size_t body_size,
                               struct nyamp_buffer_s *grant);
static int ny_voice_wire_fill(const struct ny_voice_wire_s *wire,
                              const struct nyamp_buffer_s *grant,
                              const int16_t *samples, size_t count,
                              struct nyamp_buffer_s *window);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t ny_voice_wire_now_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static uint32_t ny_voice_wire_generation(const struct ny_voice_wire_s *wire)
{
  return wire->io.generation != NULL ? wire->io.generation(wire->io.arg) : 0;
}

/****************************************************************************
 * Name: ny_voice_wire_keep
 *
 * Description:
 *   Keep an event of the open request for ny_voice_wire_event().  A full
 *   stash sheds the oldest advisory frame (a PARTIAL, a DETECTED) rather
 *   than refuse a FINISH: the service guarantees the frame after a shed
 *   PARTIAL is a RESYNC only for what IT shed, so losing one here can cost
 *   a partial transcript, never the final one, which is always a RESYNC.
 *
 ****************************************************************************/

static void ny_voice_wire_keep(struct ny_voice_wire_s *wire,
                               const struct nyamp_header_s *header,
                               const uint8_t *frame, size_t size)
{
  struct ny_voice_wire_frame_s *slot;

  if (wire->request_id == 0 || header->request_id != wire->request_id ||
      size > NYAMP_RPMSG_MTU)
    {
      return; /* An event of a request this conversation already closed. */
    }

  if (wire->count >= NY_VOICE_WIRE_STASH)
    {
      wire->head = (wire->head + 1) % NY_VOICE_WIRE_STASH;
      wire->count--;
      wire->shed++;
    }

  slot = &wire->stash[(wire->head + wire->count) % NY_VOICE_WIRE_STASH];
  slot->size = (uint16_t)size;
  memcpy(slot->data, frame, size);
  wire->count++;
}

/****************************************************************************
 * Name: ny_voice_wire_call
 *
 * Description:
 *   One request and its response.  `reply` receives the body that follows
 *   the status (at most *reply_size bytes on entry).  Returns 0 with the
 *   status OK, the status as a negated errno, or a transport error.
 *
 ****************************************************************************/

static int ny_voice_wire_call(struct ny_voice_wire_s *wire, uint16_t service,
                              uint16_t opcode, uint64_t request_id,
                              const uint8_t *body, size_t body_size,
                              int timeout_ms, bool interruptible,
                              uint8_t *reply, size_t *reply_size)
{
  uint8_t frame[NYAMP_RPMSG_MTU];
  struct nyamp_header_s header;
  uint32_t generation = ny_voice_wire_generation(wire);
  uint64_t deadline = ny_voice_wire_now_ms() + (uint64_t)timeout_ms;
  size_t capacity = reply_size != NULL ? *reply_size : 0;
  int ret;

  if (reply_size != NULL)
    {
      *reply_size = 0;
    }

  memset(&header, 0, sizeof(header));
  header.service = service;
  header.opcode = opcode;
  header.flags = NYAMP_FLAG_REQUEST;
  header.request_id = request_id;
  header.generation = generation;
  header.payload_size = (uint32_t)body_size;
  if (body_size > NYAMP_INLINE_MAX ||
      nyamp_header_encode(frame, sizeof(frame), &header) != NYAMP_OK)
    {
      return -EINVAL;
    }

  if (body_size != 0)
    {
      memcpy(frame + NYAMP_WIRE_HEADER_SIZE, body, body_size);
    }

  ret = wire->io.send(wire->io.arg, frame, NYAMP_WIRE_HEADER_SIZE + body_size);
  if (ret < 0)
    {
      return ret;
    }

  for (; ; )
    {
      const uint8_t *data;
      size_t data_size;
      int32_t status;
      uint64_t now = ny_voice_wire_now_ms();
      ssize_t size;

      if (now >= deadline)
        {
          return -ETIMEDOUT;
        }

      size = wire->io.recv(wire->io.arg, frame, sizeof(frame),
                           deadline - now < NY_VOICE_WIRE_SLICE_MS
                               ? (int)(deadline - now)
                               : NY_VOICE_WIRE_SLICE_MS);
      if (size < 0)
        {
          return (int)size;
        }

      if (size == 0)
        {
          /* The daemon that would have answered is gone. */

          if (generation != 0 && ny_voice_wire_generation(wire) != generation)
            {
              return -ECONNRESET;
            }

          if (interruptible && wire->io.interrupted != NULL &&
              wire->io.interrupted(wire->io.arg))
            {
              return -ECANCELED;
            }

          continue;
        }

      if (nyamp_header_decode(&header, frame, (size_t)size) != NYAMP_OK ||
          (size_t)size != NYAMP_WIRE_HEADER_SIZE + header.payload_size)
        {
          continue;
        }

      if ((header.flags & NYAMP_FLAG_KIND_MASK) == NYAMP_FLAG_EVENT)
        {
          struct nyamp_blob_progress_s progress;

          if (header.service == NYAMP_SERVICE_BLOB &&
              header.opcode == NYAMP_BLOB_EVENT_PROGRESS)
            {
              if (wire->io.progress != NULL &&
                  nyamp_blob_progress_decode(
                      &progress, frame + NYAMP_WIRE_HEADER_SIZE,
                      header.payload_size) == NYAMP_OK)
                {
                  wire->io.progress(wire->io.arg, progress.done,
                                    progress.total,
                                    progress.bytes_per_second);
                }
            }
          else
            {
              ny_voice_wire_keep(wire, &header, frame, (size_t)size);
            }

          continue;
        }

      if ((header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_RESPONSE ||
          header.service != service || header.opcode != opcode ||
          header.request_id != request_id)
        {
          continue;
        }

      if (nyamp_status_decode(&status, &data, &data_size,
                              frame + NYAMP_WIRE_HEADER_SIZE,
                              header.payload_size) != NYAMP_OK)
        {
          return -EPROTO;
        }

      if (status != NYAMP_MODEL_OK)
        {
          return ny_voice_wire_errno(status);
        }

      if (reply != NULL && reply_size != NULL)
        {
          if (data_size > capacity)
            {
              return -EMSGSIZE;
            }

          memcpy(reply, data, data_size);
          *reply_size = data_size;
        }

      return 0;
    }
}

/****************************************************************************
 * Name: ny_voice_wire_begin
 *
 * Description:
 *   Open a request: every later message and every event of it carries the
 *   id chosen here.  `grant` is NULL for the forms that return none.
 *
 ****************************************************************************/

static int ny_voice_wire_begin(struct ny_voice_wire_s *wire, uint16_t service,
                               uint16_t opcode, const uint8_t *body,
                               size_t body_size,
                               struct nyamp_buffer_s *grant)
{
  uint8_t reply[NYAMP_BUFFER_SIZE];
  size_t reply_size = sizeof(reply);
  uint64_t request_id = wire->io.request_id(wire->io.arg);
  int ret;

  /* Events may overtake the response, so the request is open before it. */

  wire->head = 0;
  wire->count = 0;
  wire->request_id = request_id;
  wire->generation = ny_voice_wire_generation(wire);
  ret = ny_voice_wire_call(wire, service, opcode, request_id, body, body_size,
                           NY_VOICE_WIRE_ACK_MS, false, reply, &reply_size);
  if (ret == 0 && grant != NULL &&
      (nyamp_buffer_decode(grant, reply, reply_size) != NYAMP_OK ||
       (grant->format != NYAMP_FORMAT_F32 &&
        grant->format != NYAMP_FORMAT_S16)))
    {
      ret = -EPROTO;
    }

  if (ret == 0 && grant != NULL)
    {
      uint8_t *window;

      ret = ny_voice_wire_window(wire, grant, grant->capacity, &window);
    }

  if (ret < 0)
    {
      wire->request_id = 0;
    }

  return ret;
}

/****************************************************************************
 * Name: ny_voice_wire_fill
 *
 * Description:
 *   Put samples into the granted slot in the format the grant names, and
 *   describe them as a window under the grant's lease.  One range is enough:
 *   the service has copied a window out by the time it acknowledges it.
 *
 ****************************************************************************/

static int ny_voice_wire_fill(const struct ny_voice_wire_s *wire,
                              const struct nyamp_buffer_s *grant,
                              const int16_t *samples, size_t count,
                              struct nyamp_buffer_s *window)
{
  size_t width = grant->format == NYAMP_FORMAT_S16 ? 2 : 4;
  uint8_t *target;
  int ret;

  if (count == 0 || count > grant->capacity / width)
    {
      return -EINVAL;
    }

  ret = ny_voice_wire_window(wire, grant, count * width, &target);
  if (ret < 0)
    {
      return ret;
    }

  if (width == 2)
    {
      memcpy(target, samples, count * 2);
    }
  else
    {
      ny_voice_s16_to_f32((float *)(void *)target, samples, count);
    }

  *window = *grant;
  window->flags = NYAMP_BUFFER_IN_SHMEM;
  window->length = (uint32_t)(count * width);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ny_voice_wire_init(struct ny_voice_wire_s *wire,
                        const struct ny_voice_wire_io_s *io)
{
  memset(wire, 0, sizeof(*wire));
  wire->io = *io;
}

int ny_voice_wire_errno(int32_t status)
{
  switch (status)
    {
      case NYAMP_MODEL_OK:
        return 0;

      case NYAMP_MODEL_INVALID:
        return -EINVAL;

      case NYAMP_MODEL_NOT_READY:
        return -ENOENT;

      case NYAMP_MODEL_BUSY:
        return -EBUSY;

      case NYAMP_MODEL_STALE_GENERATION:
        return -ECONNRESET;

      case NYAMP_MODEL_DUPLICATE:
        return -EALREADY;

      case NYAMP_MODEL_CANCELLED:
        return -ECANCELED;

      case NYAMP_MODEL_DEADLINE:
        return -ETIMEDOUT;

      case NYAMP_MODEL_UNSUPPORTED:
        return -ENOTSUP;

      default:
        return -EREMOTEIO;
    }
}

int ny_voice_wire_load(struct ny_voice_wire_s *wire, uint16_t service,
                       const char *directory,
                       const struct ny_voice_wire_kws_s *kws,
                       int timeout_ms)
{
  uint8_t body[NYAMP_INLINE_MAX];
  size_t size = strlen(directory);

  if (size == 0 || size > NYAMP_BLOB_MAX_NAME)
    {
      return -EINVAL;
    }

  if (service == NYAMP_SERVICE_KWS)
    {
      struct nyamp_kws_load_s load;

      memset(&load, 0, sizeof(load));
      if (kws != NULL)
        {
          load.threshold = kws->threshold;
          load.score = kws->score;
          load.max_active_paths = kws->max_active_paths;
          load.num_trailing_blanks = kws->num_trailing_blanks;
        }

      load.directory = directory;
      load.directory_length = (uint16_t)size;
      load.keywords = "";
      if (nyamp_kws_load_encode(body, sizeof(body), &size, &load) != NYAMP_OK)
        {
          return -EINVAL;
        }
    }
  else
    {
      memcpy(body, directory, size);
    }

  /* All three services use opcode 1 for LOAD. */

  return ny_voice_wire_call(wire, service, NYAMP_ASR_LOAD,
                            wire->io.request_id(wire->io.arg), body, size,
                            timeout_ms, true, NULL, NULL);
}

int ny_voice_wire_unload(struct ny_voice_wire_s *wire, uint16_t service)
{
  struct timespec pause = { 0, 100000000 };
  int attempt;
  int ret = -EBUSY;

  for (attempt = 0; attempt < NY_VOICE_WIRE_UNLOAD_TRIES && ret == -EBUSY;
       attempt++)
    {
      if (attempt != 0)
        {
          nanosleep(&pause, NULL);
        }

      ret = ny_voice_wire_call(wire, service, NYAMP_ASR_UNLOAD,
                               wire->io.request_id(wire->io.arg), NULL, 0,
                               NY_VOICE_WIRE_ACK_MS, false, NULL, NULL);
    }

  wire->request_id = 0;
  return ret;
}

int ny_voice_wire_kws_labels(struct ny_voice_wire_s *wire, char *out,
                             size_t size)
{
  uint8_t reply[NYAMP_INLINE_MAX];
  size_t reply_size = sizeof(reply);
  size_t position = 0;
  size_t used = 0;
  uint16_t count;
  uint16_t index;
  int ret;

  if (size == 0)
    {
      return -EINVAL;
    }

  out[0] = '\0';
  ret = ny_voice_wire_call(wire, NYAMP_SERVICE_KWS, NYAMP_KWS_LIST,
                           wire->io.request_id(wire->io.arg), NULL, 0,
                           NY_VOICE_WIRE_ACK_MS, false, reply, &reply_size);
  if (ret < 0)
    {
      return ret;
    }

  if (nyamp_kws_labels_decode(&count, reply, reply_size) != NYAMP_OK)
    {
      return -EPROTO;
    }

  for (index = 0; index < count; index++)
    {
      const char *label;
      uint16_t length;

      if (nyamp_kws_labels_next(&label, &length, &position, reply,
                                reply_size) != NYAMP_OK)
        {
          return -EPROTO;
        }

      if (used + length + 2 > size)
        {
          break;
        }

      if (used != 0)
        {
          out[used++] = ' ';
        }

      memcpy(out + used, label, length);
      used += length;
      out[used] = '\0';
    }

  return (int)count;
}

int ny_voice_wire_kws_begin(struct ny_voice_wire_s *wire, bool s16,
                            uint32_t window_samples,
                            struct nyamp_buffer_s *grant)
{
  uint8_t body[NYAMP_KWS_BEGIN_SIZE];
  size_t size;

  if (nyamp_kws_begin_encode(body, sizeof(body), &size, NY_VOICE_CAPTURE_RATE,
                             1, s16 ? NYAMP_AUDIO_BEGIN_S16 : 0,
                             window_samples) != NYAMP_OK)
    {
      return -EINVAL;
    }

  return ny_voice_wire_begin(wire, NYAMP_SERVICE_KWS, NYAMP_KWS_BEGIN, body,
                             size, grant);
}

int ny_voice_wire_kws_push(struct ny_voice_wire_s *wire,
                           const struct nyamp_buffer_s *grant,
                           const int16_t *samples, size_t count,
                           uint32_t sequence, bool discontinuity,
                           uint64_t stream_sample, uint64_t *next_sample)
{
  uint8_t body[NYAMP_KWS_PUSH_SIZE];
  uint8_t reply[NYAMP_KWS_PUSH_ACK_SIZE];
  size_t reply_size = sizeof(reply);
  struct nyamp_kws_push_s push;
  size_t size;
  int ret;

  if (wire->request_id == 0)
    {
      return -ENOTCONN;
    }

  memset(&push, 0, sizeof(push));
  ret = ny_voice_wire_fill(wire, grant, samples, count, &push.buffer);
  if (ret < 0)
    {
      return ret;
    }

  push.sequence = sequence;
  push.flags = discontinuity ? NYAMP_KWS_PUSH_DISCONTINUITY : 0;
  push.stream_sample = stream_sample;
  if (nyamp_kws_push_encode(body, sizeof(body), &size, &push) != NYAMP_OK)
    {
      return -EINVAL;
    }

  ret = ny_voice_wire_call(wire, NYAMP_SERVICE_KWS, NYAMP_KWS_PUSH,
                           wire->request_id, body, size, NY_VOICE_WIRE_ACK_MS,
                           false, reply, &reply_size);
  if (ret == 0 && next_sample != NULL &&
      nyamp_kws_push_ack_decode(next_sample, reply, reply_size) != NYAMP_OK)
    {
      ret = -EPROTO;
    }

  return ret;
}

int ny_voice_wire_kws_end(struct ny_voice_wire_s *wire)
{
  /* END ends the current stream whatever id it carries; the stream's own
   * keeps the FINISH routed to this conversation's port.
   */

  return ny_voice_wire_call(wire, NYAMP_SERVICE_KWS, NYAMP_KWS_END,
                            wire->request_id != 0
                                ? wire->request_id
                                : wire->io.request_id(wire->io.arg),
                            NULL, 0, NY_VOICE_WIRE_ACK_MS, false, NULL, NULL);
}

int ny_voice_wire_asr_begin(struct ny_voice_wire_s *wire, bool s16,
                            uint32_t max_samples,
                            struct nyamp_buffer_s *grant)
{
  uint8_t body[NYAMP_ASR_BEGIN_SIZE];
  size_t size;

  if (nyamp_asr_begin_encode(body, sizeof(body), &size, NY_VOICE_CAPTURE_RATE,
                             1, s16 ? NYAMP_AUDIO_BEGIN_S16 : 0,
                             max_samples) != NYAMP_OK)
    {
      return -EINVAL;
    }

  return ny_voice_wire_begin(wire, NYAMP_SERVICE_ASR, NYAMP_ASR_BEGIN, body,
                             size, grant);
}

int ny_voice_wire_asr_attach(struct ny_voice_wire_s *wire,
                             uint32_t max_samples, uint64_t start_sample)
{
  uint8_t body[NYAMP_ASR_ATTACH_SIZE];
  size_t size;

  if (nyamp_asr_attach_encode(body, sizeof(body), &size,
                              NY_VOICE_CAPTURE_RATE, 1, 0, max_samples,
                              start_sample) != NYAMP_OK)
    {
      return -EINVAL;
    }

  return ny_voice_wire_begin(wire, NYAMP_SERVICE_ASR, NYAMP_ASR_BEGIN, body,
                             size, NULL);
}

int ny_voice_wire_asr_push(struct ny_voice_wire_s *wire,
                           const struct nyamp_buffer_s *grant,
                           const int16_t *samples, size_t count,
                           uint32_t sequence)
{
  uint8_t body[NYAMP_ASR_PUSH_HEADER_SIZE];
  struct nyamp_buffer_s window;
  size_t size;
  int ret;

  if (wire->request_id == 0)
    {
      return -ENOTCONN;
    }

  ret = ny_voice_wire_fill(wire, grant, samples, count, &window);
  if (ret < 0)
    {
      return ret;
    }

  if (nyamp_asr_push_encode(body, sizeof(body), &size, &window, sequence, 0,
                            0, 0) != NYAMP_OK)
    {
      return -EINVAL;
    }

  return ny_voice_wire_call(wire, NYAMP_SERVICE_ASR, NYAMP_ASR_PUSH,
                            wire->request_id, body, size,
                            NY_VOICE_WIRE_ACK_MS, false, NULL, NULL);
}

int ny_voice_wire_asr_end(struct ny_voice_wire_s *wire, uint64_t end_sample)
{
  uint8_t body[NYAMP_ASR_END_SIZE];
  size_t size;

  if (wire->request_id == 0)
    {
      return -ENOTCONN;
    }

  if (nyamp_asr_end_encode(body, sizeof(body), &size, end_sample) != NYAMP_OK)
    {
      return -EINVAL;
    }

  return ny_voice_wire_call(wire, NYAMP_SERVICE_ASR, NYAMP_ASR_END,
                            wire->request_id, body, size,
                            NY_VOICE_WIRE_ACK_MS, false, NULL, NULL);
}

int ny_voice_wire_tts_say(struct ny_voice_wire_s *wire, const char *text,
                          size_t length, uint32_t speaker_id, float speed,
                          uint32_t window_samples)
{
  uint8_t body[NYAMP_INLINE_MAX];
  struct nyamp_tts_text_s chunk;
  uint64_t request_id;
  size_t offset = 0;
  int ret = 0;

  if (length == 0 || length > NYAMP_TTS_TEXT_MAX_BODY)
    {
      return -EINVAL;
    }

  request_id = wire->io.request_id(wire->io.arg);
  wire->head = 0;
  wire->count = 0;
  wire->request_id = request_id;
  wire->generation = ny_voice_wire_generation(wire);

  memset(&chunk, 0, sizeof(chunk));
  chunk.total = (uint32_t)length;
  chunk.speaker_id = speaker_id;
  chunk.speed = speed;
  chunk.window_samples = window_samples;
  while (offset < length && ret == 0)
    {
      size_t size;

      /* A chunk may end inside a character: the text is validated whole. */

      chunk.offset = (uint32_t)offset;
      chunk.length = (uint32_t)(length - offset < NYAMP_TTS_TEXT_MAX_CHUNK
                                    ? length - offset
                                    : NYAMP_TTS_TEXT_MAX_CHUNK);
      if (nyamp_tts_text_encode(body, sizeof(body), &size, &chunk,
                                (const uint8_t *)text + offset) != NYAMP_OK)
        {
          ret = -EINVAL;
          break;
        }

      ret = ny_voice_wire_call(wire, NYAMP_SERVICE_TTS, NYAMP_TTS_SYNTH_TEXT,
                               request_id, body, size, NY_VOICE_WIRE_ACK_MS,
                               false, NULL, NULL);
      offset += chunk.length;
    }

  if (ret < 0)
    {
      wire->request_id = 0;
    }

  return ret;
}

int ny_voice_wire_tts_release(struct ny_voice_wire_s *wire,
                              const struct ny_voice_wire_event_s *event)
{
  if (wire->request_id == 0 || event->kind != NY_VOICE_WIRE_PCM)
    {
      return -EINVAL;
    }

  /* The descriptor goes back byte for byte: the lease is the service's. */

  return ny_voice_wire_call(wire, NYAMP_SERVICE_TTS, NYAMP_TTS_RELEASE,
                            wire->request_id, event->descriptor,
                            NYAMP_BUFFER_SIZE, NY_VOICE_WIRE_ACK_MS, false,
                            NULL, NULL);
}

int ny_voice_wire_cancel(struct ny_voice_wire_s *wire, uint16_t service)
{
  uint8_t frame[NYAMP_WIRE_HEADER_SIZE];
  struct nyamp_header_s header;
  uint64_t request_id = wire->request_id;

  if (request_id == 0)
    {
      return -ENOENT;
    }

  memset(&header, 0, sizeof(header));
  header.service = service;
  header.opcode = service == NYAMP_SERVICE_TTS   ? NYAMP_TTS_CANCEL
                  : service == NYAMP_SERVICE_ASR ? NYAMP_ASR_CANCEL
                                                 : NYAMP_KWS_END;
  header.flags = NYAMP_FLAG_CANCEL;
  header.request_id = request_id;
  header.generation = wire->generation;
  if (nyamp_header_encode(frame, sizeof(frame), &header) != NYAMP_OK)
    {
      return -EINVAL;
    }

  return wire->io.send(wire->io.arg, frame, sizeof(frame));
}

int ny_voice_wire_event(struct ny_voice_wire_s *wire,
                        struct ny_voice_wire_frame_s *frame,
                        struct ny_voice_wire_event_s *event, int timeout_ms)
{
  uint64_t deadline = ny_voice_wire_now_ms() + (uint64_t)timeout_ms;
  struct nyamp_header_s header;
  const uint8_t *payload = frame->data + NYAMP_WIRE_HEADER_SIZE;

  memset(event, 0, sizeof(*event));
  if (wire->request_id == 0)
    {
      return -ENOTCONN;
    }

  for (; ; )
    {
      if (wire->count != 0)
        {
          *frame = wire->stash[wire->head];
          wire->head = (wire->head + 1) % NY_VOICE_WIRE_STASH;
          wire->count--;
        }
      else
        {
          uint64_t now = ny_voice_wire_now_ms();
          ssize_t size;

          size = wire->io.recv(wire->io.arg, frame->data, sizeof(frame->data),
                               now >= deadline ? 0 : (int)(deadline - now));
          if (size < 0)
            {
              return (int)size;
            }

          if (size == 0)
            {
              return wire->generation != 0 &&
                             ny_voice_wire_generation(wire) != wire->generation
                         ? -ECONNRESET
                         : 0;
            }

          frame->size = (uint16_t)size;
        }

      if (nyamp_header_decode(&header, frame->data, frame->size) != NYAMP_OK ||
          frame->size != NYAMP_WIRE_HEADER_SIZE + header.payload_size ||
          (header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_EVENT ||
          header.request_id != wire->request_id)
        {
          /* A late response (to a cancel another thread sent, say) or an
           * event of a closed request.
           */

          if (ny_voice_wire_now_ms() >= deadline && wire->count == 0)
            {
              return 0;
            }

          continue;
        }

      event->service = header.service;
      event->request_id = header.request_id;
      if (header.service == NYAMP_SERVICE_KWS &&
          header.opcode == NYAMP_KWS_EVENT_DETECTED &&
          nyamp_kws_detected_decode(&event->detected, payload,
                                    header.payload_size) == NYAMP_OK)
        {
          event->kind = NY_VOICE_WIRE_DETECTED;
          event->sequence = event->detected.sequence;
          return 1;
        }

      if (header.service == NYAMP_SERVICE_ASR &&
          header.opcode == NYAMP_ASR_EVENT_PARTIAL &&
          nyamp_asr_partial_decode(&event->sequence,
                                   &event->consumed_samples, &event->flags,
                                   &event->text, &event->text_length, payload,
                                   header.payload_size) == NYAMP_OK)
        {
          event->kind = NY_VOICE_WIRE_PARTIAL;
          return 1;
        }

      if (header.service == NYAMP_SERVICE_TTS &&
          header.opcode == NYAMP_TTS_EVENT_PCM &&
          nyamp_tts_pcm_decode(&event->buffer, &event->sequence,
                               &event->sample_rate, &event->channels,
                               &event->valid_samples, payload,
                               header.payload_size) == NYAMP_OK)
        {
          memcpy(event->descriptor, payload, NYAMP_BUFFER_SIZE);
          event->kind = NY_VOICE_WIRE_PCM;
          return 1;
        }

      if (header.opcode == NYAMP_ASR_EVENT_FINISH &&
          (header.service == NYAMP_SERVICE_TTS
               ? nyamp_tts_finish_decode(&event->status, &event->sequence,
                                         &event->total_samples, payload,
                                         header.payload_size)
               : nyamp_asr_finish_decode(&event->status, &event->sequence,
                                         payload, header.payload_size)) ==
              NYAMP_OK)
        {
          event->kind = NY_VOICE_WIRE_FINISH;
          wire->request_id = 0;
          return 1;
        }

      /* An event this client does not know: skip it, keep waiting. */
    }
}

int ny_voice_wire_window(const struct ny_voice_wire_s *wire,
                         const struct nyamp_buffer_s *buffer, size_t bytes,
                         uint8_t **window)
{
  if ((buffer->flags & NYAMP_BUFFER_IN_SHMEM) == 0 || buffer->lease == 0 ||
      bytes > buffer->capacity)
    {
      return -EINVAL;
    }

  if (wire->io.arena == NULL)
    {
      return -ENODEV;
    }

  /* Written so that nothing can wrap. */

  if (buffer->offset < wire->io.arena_header ||
      buffer->offset >= wire->io.arena_size ||
      buffer->capacity > wire->io.arena_size - buffer->offset)
    {
      return -ERANGE;
    }

  *window = wire->io.arena + buffer->offset;
  return 0;
}

void ny_voice_wire_transcript(char *transcript, size_t size,
                              const struct ny_voice_wire_event_s *event)
{
  size_t used;
  size_t take;

  if (size == 0 || event->kind != NY_VOICE_WIRE_PARTIAL)
    {
      return;
    }

  if ((event->flags & NYAMP_ASR_PARTIAL_RESYNC) != 0)
    {
      transcript[0] = '\0';
    }

  used = strlen(transcript);
  take = event->text_length;
  if (take > size - 1 - used)
    {
      take = size - 1 - used;

      /* Do not end on half a character. */

      while (take > 0 && ((unsigned char)event->text[take] & 0xc0) == 0x80)
        {
          take--;
        }
    }

  memcpy(transcript + used, event->text, take);
  transcript[used + take] = '\0';
}
