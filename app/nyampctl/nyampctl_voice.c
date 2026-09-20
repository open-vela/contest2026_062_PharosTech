/****************************************************************************
 * app/nyampctl/nyampctl_voice.c
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
 * The voice chain, one link at a time.
 *
 *   nyampctl kws listen [seconds]      microphone -> wake word service
 *   nyampctl asr file <wav>            a 16 kHz mono recording -> text
 *   nyampctl asr mic [seconds]         microphone -> text
 *   nyampctl tts say <text> [out.wav]  text -> speaker, or -> a WAV file
 *
 * Each command loads the model it needs by its logical name, which makes
 * the compute domain pull it out of /data/models -- and that needs the Core
 * compute service on this side to answer the pull.  They use the very
 * client the voice service uses (ny_voice_wire.c), so a link that works here
 * works there.  The microphone commands need voice to be switched off
 * (voice.enable false): the microphone and the capture slot have one owner.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NYABULA_CORE_VOICE

#include <arch/chip/rk3576_shmem.h>
#include <arch/chip/rk3576_shmem_layout.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ny_voice.h"
#include "ny_voice_audio.h"
#include "ny_voice_dsp.h"
#include "ny_voice_wire.h"
#include "nyamp_protocol.h"
#include "nyampctl.h"
#include "nyampctl_io.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_NYABULA_CORE_VOICE_SPEAKER
#define CONFIG_NYABULA_CORE_VOICE_SPEAKER 1
#endif

#define NYAMPCTL_VOICE_WINDOW   3200U /* 200 ms at 16 kHz.            */
#define NYAMPCTL_VOICE_LOAD_MS  300000
#define NYAMPCTL_VOICE_TEXT_MAX 512
#define NYAMPCTL_VOICE_TTS_MAX  44100U /* The service's default window. */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t nyampctl_voice_now_ms(void);
static int nyampctl_voice_send(void *arg, const uint8_t *wire, size_t size);
static ssize_t nyampctl_voice_recv(void *arg, uint8_t *wire, size_t capacity,
                                   int timeout_ms);
static uint64_t nyampctl_voice_request_id(void *arg);
static uint32_t nyampctl_voice_generation(void *arg);
static void nyampctl_voice_progress(void *arg, uint64_t done, uint64_t total,
                                    uint32_t bytes_per_second);
static int nyampctl_voice_open(int *fd);
static int nyampctl_voice_load(uint16_t service, const char *name);
static int nyampctl_voice_microphone(struct ny_voice_audio_s **audio);
static int nyampctl_voice_transcribe(bool wait);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* A command is one conversation, and a 4 KiB task stack holds none of
 * this.
 */

static struct ny_voice_wire_s g_nyampctl_wire;
static struct ny_voice_wire_frame_s g_nyampctl_frame;
static int16_t g_nyampctl_samples[NYAMP_KWS_WINDOW_SAMPLES_MAX];
static int16_t g_nyampctl_pcm[NYAMPCTL_VOICE_TTS_MAX * 2];
static char g_nyampctl_text[NYAMPCTL_VOICE_TEXT_MAX];
static int g_nyampctl_fd = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t nyampctl_voice_now_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int nyampctl_voice_send(void *arg, const uint8_t *wire, size_t size)
{
  (void)arg;
  return nyampctl_send(g_nyampctl_fd, wire, size);
}

static ssize_t nyampctl_voice_recv(void *arg, uint8_t *wire, size_t capacity,
                                   int timeout_ms)
{
  (void)arg;
  return nyampctl_recv(g_nyampctl_fd, wire, capacity, timeout_ms);
}

static uint64_t nyampctl_voice_request_id(void *arg)
{
  (void)arg;
  return nyampctl_request_id();
}

static uint32_t nyampctl_voice_generation(void *arg)
{
  (void)arg;
  return ny_compute_running() ? ny_compute_generation() : 0;
}

static void nyampctl_voice_progress(void *arg, uint64_t done, uint64_t total,
                                    uint32_t bytes_per_second)
{
  (void)arg;
  printf("  pulled %" PRIu64 " of %" PRIu64 " bytes, %" PRIu32 " KiB/s\n",
         done, total, bytes_per_second / 1024);
}

static int nyampctl_voice_open(int *fd)
{
  struct ny_voice_wire_io_s io;

  if (!rk3576_shmem_ready() && rk3576_shmem_initialize() < 0)
    {
      fprintf(stderr, "nyampctl: the shared arena is not claimed yet\n");
      return -ENODEV;
    }

  g_nyampctl_fd = *fd;
  memset(&io, 0, sizeof(io));
  io.send = nyampctl_voice_send;
  io.recv = nyampctl_voice_recv;
  io.request_id = nyampctl_voice_request_id;
  io.generation = nyampctl_voice_generation;
  io.progress = nyampctl_voice_progress;
  io.arena = rk3576_shmem_base();
  io.arena_size = NYAMP_SHMEM_SIZE;
  io.arena_header = NYAMP_SLOT_HEADER + NYAMP_SLOT_HEADER_SIZE;
  ny_voice_wire_init(&g_nyampctl_wire, &io);
  return 0;
}

static int nyampctl_voice_load(uint16_t service, const char *name)
{
  uint64_t begin = nyampctl_voice_now_ms();
  int ret;

  ret = ny_voice_wire_load(&g_nyampctl_wire, service, name, NULL,
                           NYAMPCTL_VOICE_LOAD_MS);
  if (ret < 0)
    {
      fprintf(stderr, "nyampctl: load %s failed: %d%s\n", name, ret,
              ret == -ENOENT    ? " (not under /data/models, or the Core "
                                  "compute service is not running)"
              : ret == -ENOTSUP ? " (nyampd was built without this backend)"
              : ret == -EBUSY   ? " (the shared slot is busy, or the model "
                                  "is loaded with other parameters)"
                                : "");
      return ret;
    }

  printf("%s loaded in %" PRIu64 " ms\n", name,
         nyampctl_voice_now_ms() - begin);
  return 0;
}

static int nyampctl_voice_microphone(struct ny_voice_audio_s **audio)
{
  struct ny_voice_audio_config_s config;
  int ret;

  memset(&config, 0, sizeof(config));
  config.path = CONFIG_NYABULA_CORE_AUDIO_INPUT_DEVICE;
  config.capture = true;
  config.rate = NY_VOICE_CAPTURE_RATE;
  config.channels = 1;
  config.chunk = NYAMPCTL_VOICE_WINDOW;
  ret = ny_voice_audio_open(audio, &config);
  if (ret < 0)
    {
      fprintf(stderr, "nyampctl: %s: %d%s\n", config.path, ret,
              ret == -EBUSY ? " (taken: voice.enable false first, and "
                              "nothing may be playing)"
                            : "");
    }

  return ret;
}

/****************************************************************************
 * Name: nyampctl_voice_transcribe
 *
 * Description:
 *   Print the partials that are there; with `wait`, all of them up to the
 *   FINISH.  Returns 1 once the request has finished.
 *
 ****************************************************************************/

static int nyampctl_voice_transcribe(bool wait)
{
  struct ny_voice_wire_event_s event;
  int ret;

  for (;;)
    {
      ret = ny_voice_wire_event(&g_nyampctl_wire, &g_nyampctl_frame, &event,
                                wait ? 20000 : 0);
      if (ret <= 0)
        {
          return wait && ret == 0 ? -ETIMEDOUT : ret;
        }

      if (event.kind == NY_VOICE_WIRE_PARTIAL)
        {
          ny_voice_wire_transcript(g_nyampctl_text, sizeof(g_nyampctl_text),
                                   &event);
          printf("%s%s%s @%" PRIu32 ": %s\n",
                 (event.flags & NYAMP_ASR_PARTIAL_FINAL) != 0 ? "final"
                                                              : "partial",
                 (event.flags & NYAMP_ASR_PARTIAL_RESYNC) != 0 ? " resync"
                                                               : "",
                 (event.flags & NYAMP_ASR_PARTIAL_ENDPOINT) != 0 ? " endpoint"
                                                                 : "",
                 event.consumed_samples, g_nyampctl_text);
        }
      else if (event.kind == NY_VOICE_WIRE_FINISH)
        {
          printf("finish: status %" PRId32 ", %" PRIu32 " text frames\n",
                 event.status, event.sequence);
          return event.status == NYAMP_MODEL_OK
                     ? 1
                     : ny_voice_wire_errno(event.status);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int nyampctl_kws_listen(int fd, unsigned int seconds)
{
  struct ny_voice_audio_s *audio = NULL;
  struct ny_voice_wire_event_s event;
  struct nyamp_buffer_s grant;
  uint64_t position = 0;
  uint64_t next = 0;
  uint32_t sequence = 0;
  uint32_t hits = 0;
  int ret;

  ret = nyampctl_voice_open(&fd);
  if (ret == 0)
    {
      ret = nyampctl_voice_load(NYAMP_SERVICE_KWS, "kws");
    }

  if (ret == 0 && ny_voice_wire_kws_labels(&g_nyampctl_wire, g_nyampctl_text,
                                           sizeof(g_nyampctl_text)) > 0)
    {
      printf("keywords: %s\n", g_nyampctl_text);
    }

  if (ret == 0)
    {
      ret = ny_voice_wire_kws_begin(&g_nyampctl_wire, false,
                                    NYAMPCTL_VOICE_WINDOW, &grant);
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: KWS BEGIN failed: %d%s\n", ret,
                  ret == -EBUSY ? " (a stream is open: voice.enable false)"
                                : "");
        }
    }

  if (ret == 0)
    {
      printf("grant: offset 0x%" PRIx32 " capacity %" PRIu32
             " lease 0x%" PRIx64 "\n",
             grant.offset, grant.capacity, grant.lease);
      ret = nyampctl_voice_microphone(&audio);
    }

  while (ret == 0 && position < (uint64_t)seconds * NY_VOICE_CAPTURE_RATE)
    {
      ssize_t got = ny_voice_audio_read(audio, g_nyampctl_samples,
                                        NYAMPCTL_VOICE_WINDOW * 2, 2000);

      if (got != (ssize_t)(NYAMPCTL_VOICE_WINDOW * 2))
        {
          fprintf(stderr, "nyampctl: microphone read: %zd\n", got);
          ret = got < 0 ? (int)got : -ETIMEDOUT;
          break;
        }

      ret = ny_voice_wire_kws_push(&g_nyampctl_wire, &grant,
                                   g_nyampctl_samples, NYAMPCTL_VOICE_WINDOW,
                                   sequence++, false, position, &next);
      position += NYAMPCTL_VOICE_WINDOW;
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: KWS PUSH failed: %d\n", ret);
          break;
        }

      if (next != position)
        {
          printf("service expects sample %" PRIu64 ", sent up to %" PRIu64
                 "\n",
                 next, position);
        }

      if (sequence % 5 == 0)
        {
          printf("  %" PRIu64 " s, level %" PRIu32 "\r",
                 position / NY_VOICE_CAPTURE_RATE,
                 ny_voice_rms_s16(g_nyampctl_samples, NYAMPCTL_VOICE_WINDOW));
          fflush(stdout);
        }

      while (ny_voice_wire_event(&g_nyampctl_wire, &g_nyampctl_frame, &event,
                                 0) == 1)
        {
          if (event.kind == NY_VOICE_WIRE_DETECTED)
            {
              hits++;
              printf("DETECTED %.*s id %u flags 0x%x start %" PRIu64
                     " end %" PRIu64 " trigger %" PRIu64 " (%.2f s)\n",
                     (int)event.detected.label_length, event.detected.label,
                     event.detected.keyword_id, event.detected.flags,
                     event.detected.start_sample, event.detected.end_sample,
                     event.detected.trigger_sample,
                     (double)event.detected.trigger_sample /
                         NY_VOICE_CAPTURE_RATE);
            }
        }
    }

  ny_voice_audio_close(audio);
  if (g_nyampctl_wire.request_id != 0)
    {
      ny_voice_wire_kws_end(&g_nyampctl_wire);
      if (ny_voice_wire_event(&g_nyampctl_wire, &g_nyampctl_frame, &event,
                              3000) == 1 &&
          event.kind == NY_VOICE_WIRE_FINISH)
        {
          printf("\nstream finished: status %" PRId32 ", %" PRIu32
                 " detections\n",
                 event.status, event.sequence);
        }
    }

  printf("%" PRIu32 " detections in %" PRIu64 " s\n", hits,
         position / NY_VOICE_CAPTURE_RATE);
  return ret;
}

int nyampctl_asr(int fd, const char *wav, unsigned int seconds)
{
  struct ny_voice_audio_s *audio = NULL;
  struct nyamp_buffer_s grant;
  uint8_t header[256];
  uint64_t begin;
  uint32_t sequence = 0;
  uint32_t rate = 0;
  uint32_t bytes = 0;
  uint32_t pushed = 0;
  uint16_t channels = 0;
  size_t offset = 0;
  int file = -1;
  int done = 0;
  int ret;

  if (wav != NULL)
    {
      ssize_t got;

      file = open(wav, O_RDONLY | O_CLOEXEC);
      got = file < 0 ? -1 : read(file, header, sizeof(header));
      if (got <= 0 ||
          ny_voice_wav_parse(header, (size_t)got, &rate, &channels, &offset,
                             &bytes) < 0 ||
          rate != NY_VOICE_CAPTURE_RATE || channels != 1 ||
          lseek(file, (off_t)offset, SEEK_SET) < 0)
        {
          fprintf(stderr, "nyampctl: %s is not a 16 kHz mono 16 bit WAV\n",
                  wav);
          if (file >= 0)
            {
              close(file);
            }

          return -EINVAL;
        }
    }

  ret = nyampctl_voice_open(&fd);
  if (ret == 0)
    {
      ret = nyampctl_voice_load(NYAMP_SERVICE_ASR, "asr");
    }

  if (ret == 0)
    {
      ret = ny_voice_wire_asr_begin(&g_nyampctl_wire, false, 0, &grant);
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: ASR BEGIN failed: %d%s\n", ret,
                  ret == -EBUSY ? " (the capture slot is the wake word "
                                  "stream's: voice.enable false)"
                                : "");
        }
    }

  if (ret == 0 && wav == NULL)
    {
      ret = nyampctl_voice_microphone(&audio);
      if (ret == 0)
        {
          printf("speak now, %u s\n", seconds);
        }
    }

  g_nyampctl_text[0] = '\0';
  begin = nyampctl_voice_now_ms();
  while (ret == 0 && done == 0)
    {
      size_t want =
          wav != NULL ? NY_VOICE_CAPTURE_RATE : NYAMPCTL_VOICE_WINDOW;
      ssize_t got;

      if (wav != NULL)
        {
          got = read(file, g_nyampctl_samples, want * 2);
        }
      else if (pushed >= seconds * NY_VOICE_CAPTURE_RATE)
        {
          got = 0;
        }
      else
        {
          got = ny_voice_audio_read(audio, g_nyampctl_samples, want * 2, 2000);
        }

      if (got < 2)
        {
          break; /* End of the recording, or of the time. */
        }

      ret =
          ny_voice_wire_asr_push(&g_nyampctl_wire, &grant, g_nyampctl_samples,
                                 (size_t)got / 2, sequence++);
      pushed += (uint32_t)got / 2;
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: ASR PUSH failed: %d\n", ret);
          break;
        }

      done = nyampctl_voice_transcribe(false);
      ret = done < 0 ? done : 0;
    }

  ny_voice_audio_close(audio);
  if (file >= 0)
    {
      close(file);
    }

  if (ret == 0 && done == 0)
    {
      ret = ny_voice_wire_asr_end(&g_nyampctl_wire, NYAMP_STREAM_SAMPLE_NOW);
      if (ret == 0)
        {
          done = nyampctl_voice_transcribe(true);
          ret = done < 0 ? done : 0;
        }
    }
  else if (g_nyampctl_wire.request_id != 0)
    {
      ny_voice_wire_cancel(&g_nyampctl_wire, NYAMP_SERVICE_ASR);
    }

  if (ret == 0)
    {
      printf("%" PRIu32 " samples (%.2f s) in %" PRIu64 " ms: %s\n", pushed,
             (double)pushed / NY_VOICE_CAPTURE_RATE,
             nyampctl_voice_now_ms() - begin, g_nyampctl_text);
    }

  return ret;
}

int nyampctl_tts_say(int fd, const char *text, const char *out)
{
  struct ny_voice_audio_config_s config;
  struct ny_voice_audio_s *audio = NULL;
  struct ny_voice_wire_event_s event;
  uint8_t header[NY_VOICE_WAV_HEADER];
  uint64_t begin;
  uint64_t first = 0;
  uint32_t total = 0;
  int file = -1;
  int ret;

  ret = nyampctl_voice_open(&fd);
  if (ret == 0)
    {
      ret = nyampctl_voice_load(NYAMP_SERVICE_TTS, "tts");
    }

  if (ret == 0 && out != NULL)
    {
      /* A place holder; the real one follows once the length is known. */

      ny_voice_wav_header(header, NY_VOICE_TTS_RATE, 1, 0);
      file = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      if (file < 0 ||
          write(file, header, sizeof(header)) != (ssize_t)sizeof(header))
        {
          fprintf(stderr, "nyampctl: %s: %d\n", out, errno);
          ret = -errno;
        }
    }
  else if (ret == 0)
    {
      memset(&config, 0, sizeof(config));
      config.path = CONFIG_NYABULA_CORE_AUDIO_OUTPUT_DEVICE;
      config.wav = true;
      config.rate = NY_VOICE_TTS_RATE;
      config.channels = 2;
      config.chunk = 8192;
      ret = ny_voice_audio_open(&audio, &config);
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: %s: %d%s\n", config.path, ret,
                  ret == -EBUSY ? " (taken, or the microphone is open in "
                                  "another format: voice.enable false)"
                                : "");
        }
    }

  begin = nyampctl_voice_now_ms();
  if (ret == 0)
    {
      ret = ny_voice_wire_tts_say(&g_nyampctl_wire, text, strlen(text),
                                  CONFIG_NYABULA_CORE_VOICE_SPEAKER, 1.0f, 0);
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: SYNTH_TEXT failed: %d%s\n", ret,
                  ret == -EBUSY ? " (a model pull or another request owns "
                                  "the shared slot)"
                                : "");
        }
    }

  while (ret == 0)
    {
      uint8_t *window;
      size_t bytes;

      ret = ny_voice_wire_event(&g_nyampctl_wire, &g_nyampctl_frame, &event,
                                30000);
      if (ret <= 0)
        {
          ret = ret == 0 ? -ETIMEDOUT : ret;
          break;
        }

      ret = 0;
      if (event.kind == NY_VOICE_WIRE_FINISH)
        {
          printf("finish: status %" PRId32 ", %" PRIu32 " windows, %" PRIu32
                 " samples (%.2f s) in %" PRIu64
                 " ms, first window after %" PRIu64 " ms\n",
                 event.status, event.sequence, event.total_samples,
                 (double)event.total_samples / NY_VOICE_TTS_RATE,
                 nyampctl_voice_now_ms() - begin, first);
          ret = ny_voice_wire_errno(event.status);
          break;
        }

      if (event.kind != NY_VOICE_WIRE_PCM)
        {
          continue;
        }

      if (first == 0)
        {
          first = nyampctl_voice_now_ms() - begin;
        }

      if (event.valid_samples > NYAMPCTL_VOICE_TTS_MAX ||
          event.buffer.format != NYAMP_FORMAT_F32 ||
          ny_voice_wire_window(&g_nyampctl_wire, &event.buffer,
                               event.buffer.length, &window) < 0)
        {
          fprintf(stderr, "nyampctl: unusable PCM window\n");
          ny_voice_wire_cancel(&g_nyampctl_wire, NYAMP_SERVICE_TTS);
          ret = -EPROTO;
          break;
        }

      printf("window %" PRIu32 ": %" PRIu32 " samples at 0x%" PRIx32 "%s%s\n",
             event.sequence, event.valid_samples, event.buffer.offset,
             (event.buffer.flags & NYAMP_BUFFER_RESYNC) != 0 ? " unit" : "",
             (event.buffer.flags & NYAMP_BUFFER_LAST) != 0 ? " last" : "");

      /* Copy out, release, then spend time on the copy: the service
       * synthesizes the next window meanwhile.
       */

      if (file >= 0)
        {
          ny_voice_f32_to_s16(g_nyampctl_pcm, (const float *)(void *)window,
                              event.valid_samples);
          bytes = event.valid_samples * 2;
        }
      else
        {
          ny_voice_f32_to_s16_stereo(g_nyampctl_pcm,
                                     (const float *)(void *)window,
                                     event.valid_samples);
          bytes = event.valid_samples * 4;
        }

      total += event.valid_samples;
      ret = ny_voice_wire_tts_release(&g_nyampctl_wire, &event);
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: RELEASE failed: %d\n", ret);
          break;
        }

      if (file >= 0)
        {
          if (write(file, g_nyampctl_pcm, bytes) != (ssize_t)bytes)
            {
              ret = -errno;
            }
        }
      else
        {
          size_t done = 0;

          while (done < bytes && ret == 0)
            {
              ssize_t written = ny_voice_audio_write(
                  audio, (const uint8_t *)g_nyampctl_pcm + done, bytes - done,
                  2000);

              ret = written < 0 ? (int)written : written == 0 ? -ETIMEDOUT : 0;
              done += written > 0 ? (size_t)written : 0;
            }
        }
    }

  if (audio != NULL)
    {
      if (ret == 0)
        {
          ny_voice_audio_drain(audio, 5000);
        }

      ny_voice_audio_close(audio);
    }

  if (file >= 0)
    {
      /* The header is written last, once the length is known. */

      ny_voice_wav_header(header, NY_VOICE_TTS_RATE, 1, total * 2);
      if (lseek(file, 0, SEEK_SET) < 0 ||
          write(file, header, sizeof(header)) != (ssize_t)sizeof(header))
        {
          ret = ret < 0 ? ret : -errno;
        }

      close(file);
      if (ret == 0)
        {
          printf("%s: %" PRIu32 " samples, 44100 Hz mono\n", out, total);
        }
    }

  return ret;
}

#endif /* CONFIG_NYABULA_CORE_VOICE */
