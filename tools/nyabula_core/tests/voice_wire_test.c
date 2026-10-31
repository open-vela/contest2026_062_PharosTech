/****************************************************************************
 * tools/nyabula_core/tests/voice_wire_test.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The control domain's speech client (ny_voice_wire.c) against nyampd's
 * real ASR, TTS and KWS services, over a SOCK_SEQPACKET pair and a file
 * mapped as the shared arena.  The service side is the harness of
 * tools/amp/test_voice_flow.py; voice_wire_test.py starts both.
 *
 *   voice_wire_test <socket fd> <arena file> <generation>
 *                   <shared offset> <capture offset>
 *
 ****************************************************************************/

#include "ny_voice_dsp.h"
#include "ny_voice_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ARENA_SIZE (4U * 1024U * 1024U)
#define PORTS      3
#define DEPTH      64

#define CHECK(condition, ...)                                   \
  do                                                            \
    {                                                           \
      g_checks++;                                               \
      if (!(condition))                                         \
        {                                                       \
          g_failures++;                                         \
          fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);       \
          fprintf(stderr, __VA_ARGS__);                         \
          fprintf(stderr, "\n");                                \
        }                                                       \
    }                                                           \
  while (0)

/* What ny_compute.c's ports do on the board: a frame goes to the port that
 * last sent its request id.  Single threaded here, so a receive on one port
 * queues what it reads for the others.
 */

struct port_s
{
  uint64_t request_id;
  unsigned int head;
  unsigned int count;
  struct ny_voice_wire_frame_s queue[DEPTH];
};

static unsigned long g_checks;
static unsigned long g_failures;
static int g_socket;
static uint32_t g_generation;
static uint64_t g_next_id = 1000;
static struct port_s g_ports[PORTS];

static int port_send(void *arg, const uint8_t *wire, size_t size)
{
  struct port_s *port = arg;
  struct nyamp_header_s header;

  if (nyamp_header_decode(&header, wire, size) != NYAMP_OK)
    {
      return -EINVAL;
    }

  port->request_id = header.request_id;
  return write(g_socket, wire, size) == (ssize_t)size ? 0 : -EIO;
}

static ssize_t port_recv(void *arg, uint8_t *wire, size_t capacity,
                         int timeout_ms)
{
  struct port_s *port = arg;

  for (; ; )
    {
      struct ny_voice_wire_frame_s frame;
      struct nyamp_header_s header;
      struct pollfd pollfd = { g_socket, POLLIN, 0 };
      ssize_t size;
      int index;

      if (port->count != 0)
        {
          struct ny_voice_wire_frame_s *slot = &port->queue[port->head];

          size = slot->size;
          memcpy(wire, slot->data, (size_t)size);
          port->head = (port->head + 1) % DEPTH;
          port->count--;
          return size;
        }

      if (poll(&pollfd, 1, timeout_ms) <= 0)
        {
          return 0;
        }

      size = read(g_socket, frame.data, sizeof(frame.data));
      if (size <= 0 || (size_t)size > capacity)
        {
          return -EIO;
        }

      frame.size = (uint16_t)size;
      if (nyamp_header_decode(&header, frame.data, (size_t)size) != NYAMP_OK)
        {
          continue;
        }

      for (index = 0; index < PORTS; index++)
        {
          struct port_s *other = &g_ports[index];

          if (other->request_id == header.request_id && other->count < DEPTH)
            {
              other->queue[(other->head + other->count++) % DEPTH] = frame;
            }
        }
    }
}

static uint64_t port_request_id(void *arg)
{
  (void)arg;
  return ++g_next_id;
}

static uint32_t port_generation(void *arg)
{
  (void)arg;
  return g_generation;
}

static void fill(int16_t *samples, size_t count, int16_t level)
{
  size_t index;

  for (index = 0; index < count; index++)
    {
      samples[index] = level;
    }
}

/* Collect an ASR request to its FINISH; returns the status. */

static int32_t collect(struct ny_voice_wire_s *wire, char *text, size_t size,
                       uint16_t *flags, uint32_t *consumed, bool to_endpoint)
{
  struct ny_voice_wire_frame_s frame;
  struct ny_voice_wire_event_s event;

  for (; ; )
    {
      int ret = ny_voice_wire_event(wire, &frame, &event, 10000);

      if (ret <= 0)
        {
          CHECK(false, "ASR event: %d", ret);
          return -1000;
        }

      if (event.kind == NY_VOICE_WIRE_FINISH)
        {
          return event.status;
        }

      if (event.kind == NY_VOICE_WIRE_PARTIAL)
        {
          ny_voice_wire_transcript(text, size, &event);
          *flags |= event.flags;
          *consumed = event.consumed_samples;
          if (to_endpoint && (event.flags & NYAMP_ASR_PARTIAL_ENDPOINT) != 0)
            {
              return 0;
            }
        }
    }
}

int main(int argc, char **argv)
{
  static int16_t samples[16000];
  static const char *const units[] =
  {
    "the first sentence is short.",
    "the second one is a little bit longer than that.",
    "and a third."
  };

  struct ny_voice_wire_io_s io;
  static struct ny_voice_wire_s asr;
  static struct ny_voice_wire_s kws;
  static struct ny_voice_wire_s tts;
  struct ny_voice_wire_frame_s frame;
  struct ny_voice_wire_event_s event;
  struct ny_voice_attach_s attach;
  struct nyamp_buffer_s grant;
  struct nyamp_buffer_s stream;
  uint32_t shared;
  uint32_t capture;
  uint64_t position = 0;
  uint64_t next = 0;
  uint64_t start;
  uint32_t sequence = 0;
  uint32_t consumed = 0;
  uint32_t windows = 0;
  uint32_t total = 0;
  uint32_t resyncs = 0;
  uint16_t flags = 0;
  char text[256] = "";
  char speech[1024] = "";
  char labels[128];
  uint8_t *arena;
  bool from_trigger;
  bool clamped;
  int arena_fd;
  int index;
  int ret;

  if (argc != 6)
    {
      return 2;
    }

  g_socket = atoi(argv[1]);
  arena_fd = open(argv[2], O_RDWR);
  g_generation = (uint32_t)strtoul(argv[3], NULL, 0);
  shared = (uint32_t)strtoul(argv[4], NULL, 0);
  capture = (uint32_t)strtoul(argv[5], NULL, 0);
  arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, arena_fd,
               0);
  if (arena_fd < 0 || arena == MAP_FAILED)
    {
      return 3;
    }

  memset(&io, 0, sizeof(io));
  io.send = port_send;
  io.recv = port_recv;
  io.request_id = port_request_id;
  io.generation = port_generation;
  io.arena = arena;
  io.arena_size = ARENA_SIZE;
  io.arena_header = 4096;
  io.arg = &g_ports[0];
  ny_voice_wire_init(&asr, &io);
  io.arg = &g_ports[1];
  ny_voice_wire_init(&kws, &io);
  io.arg = &g_ports[2];
  ny_voice_wire_init(&tts, &io);

  /* 1. ASR, pushed. */

  CHECK(ny_voice_wire_asr_begin(&asr, false, 0, &grant) == -ENOENT,
        "BEGIN before LOAD");
  CHECK(ny_voice_wire_load(&asr, NYAMP_SERVICE_ASR, "/models/asr", NULL,
                           10000) == 0, "ASR LOAD");
  ret = ny_voice_wire_asr_begin(&asr, false, 0, &grant);
  CHECK(ret == 0 && grant.offset == capture &&
        grant.format == NYAMP_FORMAT_F32 &&
        grant.lease >> 32 == g_generation, "ASR BEGIN: %d", ret);

  fill(samples, 16000, 4096);
  CHECK(ny_voice_wire_asr_push(&asr, &grant, samples, 16000, 0) == 0, "w0");
  CHECK(((float *)(void *)(arena + capture))[15999] == 0.125f,
        "the window holds float32");
  CHECK(ny_voice_wire_asr_push(&asr, &grant, samples, 16000, 1) == 0, "w1");
  CHECK(ny_voice_wire_asr_push(&asr, &grant, samples, 8000, 2) == 0, "w2");
  CHECK(ny_voice_wire_asr_push(&asr, &grant, samples, 8000, 4) == -EINVAL,
        "a skipped sequence number is refused");
  CHECK(ny_voice_wire_asr_push(&asr, &grant, samples, 70000, 3) == -EINVAL,
        "a window larger than the grant never leaves");
  CHECK(ny_voice_wire_asr_end(&asr, NYAMP_STREAM_SAMPLE_NOW) == 0, "END");
  CHECK(collect(&asr, text, sizeof(text), &flags, &consumed, false) == 0,
        "ASR FINISH");
  CHECK(strcmp(text, "打开客厅的灯。") == 0, "pushed text \"%s\"", text);
  CHECK((flags & NYAMP_ASR_PARTIAL_FINAL) != 0 &&
        (flags & NYAMP_ASR_PARTIAL_ENDPOINT) != 0 && consumed == 40000,
        "flags %#x consumed %u", flags, consumed);
  CHECK(asr.request_id == 0, "FINISH closes the request");

  /* 2. The wake word stream, and ASR attached to it. */

  CHECK(ny_voice_wire_load(&kws, NYAMP_SERVICE_KWS, "/models/kws", NULL,
                           10000) == 0, "KWS LOAD");
  CHECK(ny_voice_wire_kws_labels(&kws, labels, sizeof(labels)) == 2 &&
        strcmp(labels, "nihao_openvela hello_openvela") == 0, "labels \"%s\"",
        labels);
  ret = ny_voice_wire_kws_begin(&kws, false, 16000, &stream);
  CHECK(ret == 0 && stream.offset == capture && stream.lease != grant.lease,
        "KWS BEGIN: %d", ret);
  CHECK(ny_voice_wire_asr_begin(&asr, false, 0, &grant) == -EBUSY,
        "the capture slot has one owner");

  fill(samples, 16000, 328);
  ret = ny_voice_wire_kws_push(&kws, &stream, samples, 16000, sequence++,
                               false, position, &next);
  position += 16000;
  CHECK(ret == 0 && next == position, "push 0: %d", ret);
  samples[7999] = 24576; /* 0.75: the scripted wake phrase ends here. */
  ret = ny_voice_wire_kws_push(&kws, &stream, samples, 16000, sequence++,
                               false, position, &next);
  position += 16000;
  CHECK(ret == 0 && next == position, "push 1: %d", ret);

  ret = ny_voice_wire_event(&kws, &frame, &event, 10000);
  CHECK(ret == 1 && event.kind == NY_VOICE_WIRE_DETECTED &&
        event.detected.keyword_id == 0 &&
        event.detected.flags == NYAMP_KWS_DETECTED_HAS_OFFSETS &&
        event.detected.start_sample == 16000 &&
        event.detected.end_sample == 24000 &&
        event.detected.trigger_sample >= 24000 &&
        event.detected.trigger_sample <= 32000 &&
        event.detected.label_length == 14 &&
        memcmp(event.detected.label, "nihao_openvela", 14) == 0,
        "DETECTED: %d kind %d id %u flags %#x %llu..%llu trigger %llu", ret,
        (int)event.kind, event.detected.keyword_id, event.detected.flags,
        (unsigned long long)event.detected.start_sample,
        (unsigned long long)event.detected.end_sample,
        (unsigned long long)event.detected.trigger_sample);

  fill(samples, 16000, 4096);
  ret = ny_voice_wire_kws_push(&kws, &stream, samples, 16000, sequence++,
                               false, position, &next);
  position += 16000;

  memset(&attach, 0, sizeof(attach));
  attach.has_offsets =
      (event.detected.flags & NYAMP_KWS_DETECTED_HAS_OFFSETS) != 0;
  attach.start_sample = event.detected.start_sample;
  attach.end_sample = event.detected.end_sample;
  attach.trigger_sample = event.detected.trigger_sample;
  attach.stream_next = position;
  attach.ring_samples = NYAMP_KWS_RING_SECONDS * 16000;
  attach.backoff_samples = 8000;
  attach.margin_samples = 8000;
  start = ny_voice_attach_sample(&attach, &from_trigger, &clamped);
  CHECK(start == 24000 && !from_trigger && !clamped, "attach at %llu",
        (unsigned long long)start);

  text[0] = '\0';
  flags = 0;
  CHECK(ny_voice_wire_asr_attach(&asr, 0, start) == 0, "ASR attach");
  for (index = 0; index < 2; index++)
    {
      ret = ny_voice_wire_kws_push(&kws, &stream, samples, 16000, sequence++,
                                   false, position, &next);
      position += 16000;
      CHECK(ret == 0 && next == position, "command window %d: %d", index,
            ret);
    }

  CHECK(collect(&asr, text, sizeof(text), &flags, &consumed, true) == 0 &&
        (flags & NYAMP_ASR_PARTIAL_ENDPOINT) != 0, "the decoder's endpoint");
  CHECK(ny_voice_wire_asr_end(&asr, position) == 0, "END at the stream head");
  CHECK(collect(&asr, text, sizeof(text), &flags, &consumed, false) == 0,
        "attached FINISH");
  CHECK(strcmp(text, "打开客厅的灯。") == 0 && consumed == position - start,
        "attached text \"%s\", consumed %u", text, consumed);

  /* A pause (the robot spoke): the position jumps, which is no error. */

  position += 48000;
  ret = ny_voice_wire_kws_push(&kws, &stream, samples, 3200, sequence++, true,
                               position, &next);
  position += 3200;
  CHECK(ret == 0 && next == position, "discontinuity: %d", ret);

  /* Offsets the service does not believe: the trigger is what is left. */

  fill(samples, 16000, 328);
  for (index = 0; index < 3; index++)
    {
      samples[100] = index == 2 ? 16384 : 328; /* 0.5: the stale marker. */
      ret = ny_voice_wire_kws_push(&kws, &stream, samples, 16000, sequence++,
                                   false, position, &next);
      position += 16000;
      CHECK(ret == 0, "window before the stale hit: %d", ret);
    }

  ret = ny_voice_wire_event(&kws, &frame, &event, 10000);
  CHECK(ret == 1 && event.kind == NY_VOICE_WIRE_DETECTED &&
        event.detected.keyword_id == 1 && event.detected.flags == 0 &&
        event.detected.trigger_sample > position - 16000 &&
        event.detected.trigger_sample <= position,
        "stale DETECTED: %d kind %d id %u flags %#x trigger %llu of %llu", ret,
        (int)event.kind, event.detected.keyword_id, event.detected.flags,
        (unsigned long long)event.detected.trigger_sample,
        (unsigned long long)position);
  attach.has_offsets = false;
  attach.trigger_sample = event.detected.trigger_sample;
  attach.stream_next = position;
  attach.stream_base = position - 48000 - 3200;
  start = ny_voice_attach_sample(&attach, &from_trigger, &clamped);
  CHECK(start == event.detected.trigger_sample - 8000 && from_trigger &&
        !clamped,
        "trigger fallback at %llu", (unsigned long long)start);
  CHECK(ny_voice_wire_asr_attach(&asr, 0, start) == 0, "attach by trigger");
  CHECK(ny_voice_wire_cancel(&asr, NYAMP_SERVICE_ASR) == 0, "cancel frame");
  CHECK(collect(&asr, text, sizeof(text), &flags, &consumed, false) ==
        NYAMP_MODEL_CANCELLED, "a cancelled request says so");

  CHECK(ny_voice_wire_kws_push(&kws, &stream, samples, 1600, sequence, false,
                               0, &next) == -EINVAL, "a position going back");
  CHECK(ny_voice_wire_kws_end(&kws) == 0, "KWS END");
  ret = ny_voice_wire_event(&kws, &frame, &event, 10000);
  CHECK(ret == 1 && event.kind == NY_VOICE_WIRE_FINISH && event.status == 0 &&
        event.sequence == 2, "KWS FINISH: %d", ret);

  /* 3. TTS: more than one chunk of text, one window outstanding. */

  CHECK(ny_voice_wire_load(&tts, NYAMP_SERVICE_TTS, "/models/tts", NULL,
                           10000) == 0, "TTS LOAD");
  for (index = 0; index < 3; index++)
    {
      strcat(speech, units[index]);
      strcat(speech, " ");
    }

  for (index = 0; index < 9; index++)
    {
      strcat(speech, "pad pad pad pad pad pad pad pad pad pad pad. ");
    }

  speech[strlen(speech) - 1] = '\0';
  CHECK(strlen(speech) > NYAMP_TTS_TEXT_MAX_CHUNK, "one chunk would do");
  ret = ny_voice_wire_tts_say(&tts, speech, strlen(speech), 1, 1.0f, 0);
  CHECK(ret == 0, "SYNTH_TEXT: %d", ret);
  for (; ; )
    {
      uint8_t *window;

      ret = ny_voice_wire_event(&tts, &frame, &event, 10000);
      if (ret != 1 || event.kind != NY_VOICE_WIRE_PCM)
        {
          break;
        }

      CHECK(event.sequence == windows && event.sample_rate == 44100 &&
            event.channels == 1 && event.buffer.offset == shared &&
            event.buffer.format == NYAMP_FORMAT_F32 &&
            event.buffer.length == event.valid_samples * 4 &&
            (event.buffer.flags & NYAMP_BUFFER_FROM_COMPUTE) != 0 &&
            event.valid_samples <= 44100, "window %u", windows);
      CHECK(ny_voice_wire_window(&tts, &event.buffer, event.buffer.length,
                                 &window) == 0, "window %u address", windows);
      if ((event.buffer.flags & NYAMP_BUFFER_RESYNC) != 0)
        {
          const char *unit = resyncs < 3 ? units[resyncs] : "pad";

          CHECK(((float *)(void *)window)[0] ==
                (float)(unit[0] % 256) / 256.0f, "unit %u audio", resyncs);
          resyncs++;
        }

      windows++;
      total += event.valid_samples;
      CHECK(ny_voice_wire_tts_release(&tts, &event) == 0, "RELEASE %u",
            windows);
    }

  CHECK(ret == 1 && event.kind == NY_VOICE_WIRE_FINISH && event.status == 0 &&
        event.sequence == windows && event.total_samples == total &&
        resyncs == 12, "TTS FINISH: %d, %u windows, %u units", ret, windows,
        resyncs);
  CHECK(total == (strlen(speech) - 11) * 8 * 512, "%u samples", total);

  /* Barge-in: cancel while the first window is out. */

  CHECK(ny_voice_wire_tts_say(&tts, "this one is cancelled.", 22, 1, 1.0f,
                              0) == 0, "second SYNTH_TEXT");
  ret = ny_voice_wire_event(&tts, &frame, &event, 10000);
  CHECK(ret == 1 && event.kind == NY_VOICE_WIRE_PCM, "first window: %d", ret);
  CHECK(ny_voice_wire_cancel(&tts, NYAMP_SERVICE_TTS) == 0, "TTS cancel");
  ret = ny_voice_wire_event(&tts, &frame, &event, 10000);
  CHECK(ret == 1 && event.kind == NY_VOICE_WIRE_FINISH &&
        event.status == NYAMP_MODEL_CANCELLED && event.sequence == 1,
        "cancelled FINISH: %d", ret);
  CHECK(ny_voice_wire_tts_say(&tts, "\xff\xfe", 2, 1, 1.0f, 0) == -EINVAL,
        "malformed UTF-8 is refused by the service");

  /* 4. Unload, and a compute domain that restarts. */

  CHECK(ny_voice_wire_unload(&tts, NYAMP_SERVICE_TTS) == 0, "TTS UNLOAD");
  CHECK(ny_voice_wire_unload(&kws, NYAMP_SERVICE_KWS) == 0, "KWS UNLOAD");
  CHECK(ny_voice_wire_asr_begin(&asr, true, 0, &grant) == 0 &&
        grant.format == NYAMP_FORMAT_S16, "ASR BEGIN, S16 windows");
  CHECK(ny_voice_wire_asr_push(&asr, &grant, samples, 16000, 0) == 0 &&
        ((int16_t *)(void *)(arena + capture))[100] == 16384,
        "the window holds int16");
  g_generation++;
  do
    {
      ret = ny_voice_wire_event(&asr, &frame, &event, 300);
    }
  while (ret == 1);

  CHECK(ret == -ECONNRESET, "a new generation ends the wait: %d", ret);
  CHECK(ny_voice_wire_asr_end(&asr, NYAMP_STREAM_SAMPLE_NOW) == -ECONNRESET,
        "the old daemon's requests are stale");
  g_generation--;
  CHECK(ny_voice_wire_unload(&asr, NYAMP_SERVICE_ASR) == 0, "ASR UNLOAD");

  printf("voice_wire_test: %lu checks, %lu failures, %u tts windows, "
         "%u samples\n", g_checks, g_failures, windows, total);
  close(g_socket);
  return g_failures == 0 ? 0 : 1;
}
