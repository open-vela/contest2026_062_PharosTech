/****************************************************************************
 * app/nyabula_core/ny_product_bt_audio.c
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
 * Included Files
 ****************************************************************************/

#include "ny_product_bt_audio.h"

#ifdef CONFIG_NYABULA_CORE_BT

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <nuttx/mutex.h>

#include "ny_audio_stream.h"
#include "ny_pcm.h"
#include "ny_sbc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* About 2 s of the best SBC an A2DP source sends (328 kbit/s). */

#define NY_BT_MEDIA_RING_BYTES  (96 * 1024)
#define NY_BT_MEDIA_PACKET_MAX  1024
#define NY_BT_MEDIA_CHUNK_BYTES 4096 /* 23 ms of 44.1 kHz stereo */
#define NY_BT_MEDIA_BUFFERS     4
#define NY_BT_MEDIA_WRITE_MS    200
#define NY_BT_MEDIA_BURST       8 /* Packets decoded per look at flags */
#define NY_BT_DEVICE_RETRY_MS   300

#define NY_BT_SCO_RING_BYTES    4096
#define NY_BT_SCO_PACKET_MAX    256
#define NY_BT_CALL_CHUNK_MS     20
#define NY_BT_CALL_PLAY_BUFFERS 6 /* At most 120 ms queued to the codec */
#define NY_BT_CALL_MIC_BUFFERS  4
#define NY_BT_CALL_MIC_MAX_MS   60 /* Older microphone audio is dropped */
#define NY_BT_CALL_SILENT_RX_MS 60 /* Then transmit is paced by the clock */
#define NY_BT_CALL_CREDIT_MAX   4  /* Packets sent in one go at most */
#define NY_BT_CALL_CVSD_PACKET  48

#define NY_BT_AUDIO_STACK       16384

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_bt_audio_s
{
  mutex_t lock;
  sem_t wake;
  pthread_t thread;
  bool running;
  bool stop;

  /* Wishes, written by the stack side under lock. */

  bool want_media;
  bool want_call;
  bool held;
  bool call_msbc;
  uint32_t media_rate;
  uint8_t media_channels;
  uint16_t media_frame_samples;
  ny_bt_audio_sco_send_t send;
  uint32_t call_generation; /* Bumped per call so a new one is never missed */

  /* Queues filled in stack context. */

  struct ny_pcm_ring_s media_ring;
  uint32_t media_buffered_frames;
  struct ny_pcm_ring_s sco_ring;

  /* What the audio thread reports, under lock as well. */

  struct ny_bt_audio_status_s status;
  bool device_closed; /* The speaker is not open: hold may return */
};

/* Only the audio thread touches this. */

struct ny_bt_audio_worker_s
{
  int mode;
  struct ny_audio_stream_s *speaker;
  struct ny_audio_stream_s *microphone;
  struct ny_sbc_decoder_s *sbc;
  struct ny_sbc_decoder_s *msbc;
  struct ny_msbc_encoder_s *encoder;
  struct ny_msbc_deframer_s deframer;
  struct ny_pcm_gate_s gate;
  struct ny_pcm_ring_s mic_ring;
  struct ny_pcm_ring_s tx_ring;
  uint64_t retry_at;
  uint64_t last_rx;
  uint64_t last_credit;
  uint32_t call_generation;
  uint32_t rate;
  uint8_t channels;
  bool msbc_call;
  bool playing; /* Media: past the prebuffer */
  unsigned int tx_sequence;
  size_t tx_credit;
  size_t tx_packet;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t ny_bt_audio_now(void);
static void ny_bt_audio_error(int error);
static void ny_bt_audio_leave(struct ny_bt_audio_worker_s *worker);
static void ny_bt_audio_media_service(struct ny_bt_audio_worker_s *worker);
static int ny_bt_audio_call_open(struct ny_bt_audio_worker_s *worker);
static void ny_bt_audio_call_play(struct ny_bt_audio_worker_s *worker,
                                  int16_t *pcm, size_t samples);
static void ny_bt_audio_call_capture(struct ny_bt_audio_worker_s *worker);
static void ny_bt_audio_call_transmit(struct ny_bt_audio_worker_s *worker,
                                      ny_bt_audio_sco_send_t send);
static void ny_bt_audio_call_service(struct ny_bt_audio_worker_s *worker);
static void *ny_bt_audio_thread(void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ny_bt_audio_s g_bt_audio = {
  .lock = NXMUTEX_INITIALIZER,
  .device_closed = true,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_bt_audio_now
 ****************************************************************************/

static uint64_t ny_bt_audio_now(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: ny_bt_audio_error
 ****************************************************************************/

static void ny_bt_audio_error(int error)
{
  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.status.error = error;
  nxmutex_unlock(&g_bt_audio.lock);
}

/****************************************************************************
 * Name: ny_bt_audio_leave
 *
 * Description:
 *   Let go of both devices.  The flash player may be waiting for exactly
 *   this in ny_bt_audio_hold().
 *
 ****************************************************************************/

static void ny_bt_audio_leave(struct ny_bt_audio_worker_s *worker)
{
  ny_audio_stream_close(worker->microphone);
  ny_audio_stream_close(worker->speaker);
  worker->microphone = NULL;
  worker->speaker = NULL;
  worker->playing = false;
  worker->mode = NY_BT_AUDIO_OWNER_NONE;
  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.status.owner = NY_BT_AUDIO_OWNER_NONE;
  g_bt_audio.status.rate = 0;
  g_bt_audio.status.channels = 0;
  g_bt_audio.device_closed = true;
  nxmutex_unlock(&g_bt_audio.lock);
}

/****************************************************************************
 * Name: ny_bt_audio_media_service
 ****************************************************************************/

static void ny_bt_audio_media_service(struct ny_bt_audio_worker_s *worker)
{
  static uint8_t packet[NY_BT_MEDIA_PACKET_MAX];
  static int16_t pcm[2 * NY_SBC_FRAME_SAMPLES_MAX];
  uint32_t buffered_ms;
  uint32_t rate;
  uint16_t frame_samples;
  int burst;

  nxmutex_lock(&g_bt_audio.lock);
  rate = g_bt_audio.media_rate;
  frame_samples = g_bt_audio.media_frame_samples;
  buffered_ms = rate == 0
                    ? 0
                    : (uint32_t)((uint64_t)g_bt_audio.media_buffered_frames *
                                 frame_samples * 1000 / rate);
  g_bt_audio.status.media_buffer_ms = buffered_ms;
  nxmutex_unlock(&g_bt_audio.lock);
  if (rate == 0)
    {
      return;
    }

  if (!worker->playing)
    {
      /* Start (and start again after running dry) with a cushion against
       * the jitter of the radio link, which Wi-Fi traffic on the same chip
       * makes worse.
       */

      if (buffered_ms < CONFIG_NYABULA_CORE_BT_A2DP_PREBUFFER_MS)
        {
          return;
        }

      if (worker->speaker == NULL)
        {
          struct ny_audio_stream_config_s config = {
            .path = CONFIG_NYABULA_CORE_BT_OUTPUT_DEVICE,
            .capture = false,
            .wav = true,
            .rate = rate,
            .channels = 2,
            .chunk = NY_BT_MEDIA_CHUNK_BYTES,
            .buffers = NY_BT_MEDIA_BUFFERS,
          };

          int ret;

          if (ny_bt_audio_now() < worker->retry_at)
            {
              return;
            }

          /* Announce the open under the lock that ny_bt_audio_hold() takes
           * to set its flag: either it sees the device in use and waits, or
           * this sees the hold and leaves the device alone.
           */

          nxmutex_lock(&g_bt_audio.lock);
          if (g_bt_audio.held)
            {
              nxmutex_unlock(&g_bt_audio.lock);
              return;
            }

          g_bt_audio.device_closed = false;
          nxmutex_unlock(&g_bt_audio.lock);
          ret = ny_audio_stream_open(&worker->speaker, &config);
          if (ret < 0)
            {
              /* Usually the flash player finishing a track or a chime. */

              worker->retry_at = ny_bt_audio_now() + NY_BT_DEVICE_RETRY_MS;
              nxmutex_lock(&g_bt_audio.lock);
              g_bt_audio.device_closed = true;
              g_bt_audio.status.error = ret;
              nxmutex_unlock(&g_bt_audio.lock);
              return;
            }

          ny_sbc_decoder_reset(worker->sbc);
          worker->rate = rate;
          nxmutex_lock(&g_bt_audio.lock);
          g_bt_audio.status.owner = NY_BT_AUDIO_OWNER_MEDIA;
          g_bt_audio.status.rate = rate;
          g_bt_audio.status.channels = 2;
          g_bt_audio.status.error = 0;
          nxmutex_unlock(&g_bt_audio.lock);
        }

      worker->playing = true;
    }

  for (burst = 0; burst < NY_BT_MEDIA_BURST; burst++)
    {
      unsigned int frames;
      size_t offset = 1;
      int size;

      nxmutex_lock(&g_bt_audio.lock);
      size = ny_pcm_ring_get(&g_bt_audio.media_ring, packet, sizeof(packet));
      if (size > 0)
        {
          frames = packet[0] & 0x0f;
          g_bt_audio.media_buffered_frames =
              g_bt_audio.media_buffered_frames > frames
                  ? g_bt_audio.media_buffered_frames - frames
                  : 0;
        }

      nxmutex_unlock(&g_bt_audio.lock);
      if (size <= 0)
        {
          /* Dry.  Once the codec has played what it was given, build the
           * cushion again rather than limp from packet to packet.
           */

          if (ny_audio_stream_queued(worker->speaker) == 0)
            {
              worker->playing = false;
              nxmutex_lock(&g_bt_audio.lock);
              g_bt_audio.status.media_underruns++;
              nxmutex_unlock(&g_bt_audio.lock);
            }

          return;
        }

      while (offset < (size_t)size)
        {
          struct ny_sbc_format_s format;
          size_t used;
          int samples = ny_sbc_decode(worker->sbc, packet + offset,
                                      (size_t)size - offset, &used, pcm,
                                      2 * NY_SBC_FRAME_SAMPLES_MAX, &format);

          if (samples == -EAGAIN)
            {
              break;
            }

          offset += used;
          nxmutex_lock(&g_bt_audio.lock);
          if (samples < 0)
            {
              g_bt_audio.status.media_bad++;
            }
          else
            {
              g_bt_audio.status.media_frames++;
            }

          nxmutex_unlock(&g_bt_audio.lock);
          if (samples > 0)
            {
              ssize_t ret = ny_audio_stream_write(
                  worker->speaker, pcm, (size_t)samples * 2 * sizeof(int16_t),
                  NY_BT_MEDIA_WRITE_MS);

              if (ret < 0)
                {
                  /* The driver ended the stream: open it again. */

                  ny_bt_audio_error((int)ret);
                  ny_audio_stream_close(worker->speaker);
                  worker->speaker = NULL;
                  worker->playing = false;
                  worker->retry_at = ny_bt_audio_now() + NY_BT_DEVICE_RETRY_MS;
                  nxmutex_lock(&g_bt_audio.lock);
                  g_bt_audio.device_closed = true;
                  g_bt_audio.status.owner = NY_BT_AUDIO_OWNER_NONE;
                  nxmutex_unlock(&g_bt_audio.lock);
                  return;
                }
            }
        }
    }
}

/****************************************************************************
 * Name: ny_bt_audio_call_open
 *
 * Description:
 *   The codec runs both directions from one clock: the two nodes have to be
 *   opened with the same rate and channel count, so they are opened
 *   together.  Mono first; a board whose I2S only does stereo gets the
 *   second try.
 *
 ****************************************************************************/

static int ny_bt_audio_call_open(struct ny_bt_audio_worker_s *worker)
{
  uint32_t rate = worker->msbc_call ? NY_MSBC_RATE : 8000;
  uint8_t channels;
  int ret = -ENODEV;

  for (channels = 1; channels <= 2; channels++)
    {
      struct ny_audio_stream_config_s config = {
        .path = CONFIG_NYABULA_CORE_BT_OUTPUT_DEVICE,
        .capture = false,
        .wav = true,
        .rate = rate,
        .channels = channels,
        .chunk = (size_t)rate * channels * 2 * NY_BT_CALL_CHUNK_MS / 1000,
        .buffers = NY_BT_CALL_PLAY_BUFFERS,
      };

      ret = ny_audio_stream_open(&worker->speaker, &config);
      if (ret < 0)
        {
          continue;
        }

      config.path = CONFIG_NYABULA_CORE_BT_INPUT_DEVICE;
      config.capture = true;
      config.wav = false;
      config.buffers = NY_BT_CALL_MIC_BUFFERS;
      ret = ny_audio_stream_open(&worker->microphone, &config);
      if (ret == 0)
        {
          worker->rate = rate;
          worker->channels = channels;
          return 0;
        }

      ny_audio_stream_close(worker->speaker);
      worker->speaker = NULL;
    }

  return ret;
}

/****************************************************************************
 * Name: ny_bt_audio_call_play
 *
 * Description:
 *   One block of far end speech to the speaker.  Never waits: a block that
 *   finds the codec queue full is late already and is dropped.
 *
 ****************************************************************************/

static void ny_bt_audio_call_play(struct ny_bt_audio_worker_s *worker,
                                  int16_t *pcm, size_t samples)
{
  static int16_t stereo[2 * NY_BT_SCO_PACKET_MAX];

  ny_pcm_gate_far(&worker->gate, pcm, samples);
  if (worker->speaker == NULL)
    {
      return;
    }

  if (worker->channels == 2)
    {
      ny_pcm_mono_to_stereo(pcm, stereo, samples);
      ny_audio_stream_write(worker->speaker, stereo,
                            samples * 2 * sizeof(int16_t), 0);
    }
  else
    {
      ny_audio_stream_write(worker->speaker, pcm, samples * sizeof(int16_t),
                            0);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_call_capture
 *
 * Description:
 *   Move what the microphone has into the mono ring, keeping only the newest
 *   NY_BT_CALL_MIC_MAX_MS so that a stall never turns into standing delay.
 *
 ****************************************************************************/

static void ny_bt_audio_call_capture(struct ny_bt_audio_worker_s *worker)
{
  static int16_t block[2 * 320];
  size_t limit = (size_t)worker->rate * 2 * NY_BT_CALL_MIC_MAX_MS / 1000;

  if (worker->microphone == NULL)
    {
      return;
    }

  for (;;)
    {
      size_t want = 320 * sizeof(int16_t) * worker->channels;
      ssize_t got = ny_audio_stream_read(worker->microphone, block, want, 0);
      size_t samples;

      if (got <= 0)
        {
          return;
        }

      samples = (size_t)got / sizeof(int16_t) / worker->channels;
      if (worker->channels == 2)
        {
          ny_pcm_stereo_to_mono(block, block, samples);
        }

      while (worker->mic_ring.used + samples * sizeof(int16_t) > limit &&
             worker->mic_ring.used > 0)
        {
          ny_pcm_ring_read(&worker->mic_ring, NULL,
                           NY_MSBC_FRAME_SAMPLES * sizeof(int16_t));
        }

      ny_pcm_ring_write(&worker->mic_ring, block, samples * sizeof(int16_t));
    }
}

/****************************************************************************
 * Name: ny_bt_audio_call_transmit
 *
 * Description:
 *   Spend the transmit credit: one SCO packet out for each one in, of the
 *   same size, which keeps the two directions on the controller's clock.
 *
 ****************************************************************************/

static void ny_bt_audio_call_transmit(struct ny_bt_audio_worker_s *worker,
                                      ny_bt_audio_sco_send_t send)
{
  if (worker->tx_credit > NY_BT_CALL_CREDIT_MAX * worker->tx_packet)
    {
      worker->tx_credit = NY_BT_CALL_CREDIT_MAX * worker->tx_packet;
    }

  while (worker->tx_packet > 0 && worker->tx_credit >= worker->tx_packet)
    {
      uint8_t out[NY_BT_SCO_PACKET_MAX];
      int16_t pcm[NY_MSBC_FRAME_SAMPLES];
      int ret;

      while (worker->tx_ring.used < worker->tx_packet)
        {
          size_t bytes = worker->msbc_call ? sizeof(pcm)
                                           : (worker->tx_packet & ~(size_t)1);
          size_t got = 0;

          if (bytes > sizeof(pcm))
            {
              bytes = sizeof(pcm);
            }

          /* Whole blocks only; silence when the microphone is behind. */

          if (worker->mic_ring.used >= bytes)
            {
              got = ny_pcm_ring_read(&worker->mic_ring, pcm, bytes);
            }

          memset((uint8_t *)pcm + got, 0, sizeof(pcm) - got);
          ny_pcm_gate_near(&worker->gate, pcm, bytes / sizeof(int16_t));
          if (worker->msbc_call)
            {
              uint8_t frame[NY_MSBC_FRAME_BYTES];
              uint8_t packed[NY_MSBC_H2_BYTES];

              if (ny_msbc_encode(worker->encoder, pcm, frame) < 0)
                {
                  memset(frame, 0, sizeof(frame));
                }

              ny_msbc_h2_pack(worker->tx_sequence++, frame, packed);
              ny_pcm_ring_write(&worker->tx_ring, packed, sizeof(packed));
            }
          else
            {
              ny_pcm_ring_write(&worker->tx_ring, pcm, bytes);
            }
        }

      ny_pcm_ring_read(&worker->tx_ring, out, worker->tx_packet);
      ret = send == NULL ? -ENOTCONN : send(out, worker->tx_packet);
      worker->tx_credit -= worker->tx_packet;
      nxmutex_lock(&g_bt_audio.lock);
      if (ret < 0)
        {
          g_bt_audio.status.sco_tx_failed++;
        }
      else
        {
          g_bt_audio.status.sco_tx_packets++;
        }

      nxmutex_unlock(&g_bt_audio.lock);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_call_service
 ****************************************************************************/

static void ny_bt_audio_call_service(struct ny_bt_audio_worker_s *worker)
{
  static uint8_t packet[NY_BT_SCO_PACKET_MAX + 1];
  static int16_t pcm[NY_SBC_FRAME_SAMPLES_MAX];
  ny_bt_audio_sco_send_t send;
  uint64_t now = ny_bt_audio_now();

  if (worker->speaker == NULL && now >= worker->retry_at)
    {
      int ret;

      nxmutex_lock(&g_bt_audio.lock);
      g_bt_audio.device_closed = false;
      nxmutex_unlock(&g_bt_audio.lock);
      ret = ny_bt_audio_call_open(worker);
      nxmutex_lock(&g_bt_audio.lock);
      g_bt_audio.status.error = ret;
      if (ret < 0)
        {
          g_bt_audio.device_closed = true;
        }
      else
        {
          g_bt_audio.status.owner = NY_BT_AUDIO_OWNER_CALL;
          g_bt_audio.status.rate = worker->rate;
          g_bt_audio.status.channels = worker->channels;
        }

      nxmutex_unlock(&g_bt_audio.lock);
      if (ret < 0)
        {
          /* The call goes on without sound here; keep trying, the flash
           * player may only be finishing a chime.
           */

          worker->retry_at = now + NY_BT_DEVICE_RETRY_MS;
        }
    }

  for (;;)
    {
      size_t offset = 0;
      int size;

      nxmutex_lock(&g_bt_audio.lock);
      size = ny_pcm_ring_get(&g_bt_audio.sco_ring, packet, sizeof(packet));
      nxmutex_unlock(&g_bt_audio.lock);
      if (size <= 1)
        {
          break;
        }

      /* packet[0] is the controller's verdict on the payload. */

      size--;
      worker->last_rx = now;
      worker->tx_packet = (size_t)size;
      worker->tx_credit += (size_t)size;
      if (!worker->msbc_call)
        {
          size_t samples = (size_t)size / sizeof(int16_t);

          if (samples > NY_SBC_FRAME_SAMPLES_MAX)
            {
              samples = NY_SBC_FRAME_SAMPLES_MAX;
            }

          memcpy(pcm, packet + 1, samples * sizeof(int16_t));
          ny_bt_audio_call_play(worker, pcm, samples);
          continue;
        }

      while (offset < (size_t)size)
        {
          uint8_t frame[NY_MSBC_FRAME_BYTES];
          unsigned int lost;
          size_t used;
          int samples;

          if (!ny_msbc_deframer_push(&worker->deframer, packet + 1 + offset,
                                     (size_t)size - offset, &used, frame,
                                     &lost))
            {
              break;
            }

          offset += used;

          /* The library conceals nothing: a lost frame is 7.5 ms of
           * silence, which is also what keeps the speaker queue in step.
           */

          memset(pcm, 0, NY_MSBC_FRAME_SAMPLES * sizeof(int16_t));
          if (lost > 0)
            {
              nxmutex_lock(&g_bt_audio.lock);
              g_bt_audio.status.sco_rx_bad += lost;
              nxmutex_unlock(&g_bt_audio.lock);
            }

          while (lost-- > 0)
            {
              ny_bt_audio_call_play(worker, pcm, NY_MSBC_FRAME_SAMPLES);
            }

          samples = ny_sbc_decode(worker->msbc, frame, sizeof(frame), &used,
                                  pcm, NY_SBC_FRAME_SAMPLES_MAX, NULL);
          if (samples != NY_MSBC_FRAME_SAMPLES)
            {
              memset(pcm, 0, NY_MSBC_FRAME_SAMPLES * sizeof(int16_t));
              nxmutex_lock(&g_bt_audio.lock);
              g_bt_audio.status.sco_rx_bad++;
              nxmutex_unlock(&g_bt_audio.lock);
            }

          ny_bt_audio_call_play(worker, pcm, NY_MSBC_FRAME_SAMPLES);
        }
    }

  if (now - worker->last_rx > NY_BT_CALL_SILENT_RX_MS)
    {
      /* Nothing is coming in.  The far end must still hear us, and an
       * outgoing stream is also what some controllers need before they
       * deliver anything: pace transmit by our own clock meanwhile.
       */

      size_t per_second =
          worker->msbc_call ? NY_MSBC_H2_BYTES * 1000 * 10 / 75 : 16000;

      if (worker->tx_packet == 0)
        {
          worker->tx_packet =
              worker->msbc_call ? NY_MSBC_H2_BYTES : NY_BT_CALL_CVSD_PACKET;
        }

      if (worker->last_credit != 0 && now > worker->last_credit)
        {
          worker->tx_credit +=
              (size_t)(now - worker->last_credit) * per_second / 1000;
        }
    }

  worker->last_credit = now;
  ny_bt_audio_call_capture(worker);
  nxmutex_lock(&g_bt_audio.lock);
  send = g_bt_audio.send;
  g_bt_audio.status.gate_closed = worker->gate.hang > 0;
  g_bt_audio.status.gate_closures = worker->gate.closures;
  nxmutex_unlock(&g_bt_audio.lock);
  ny_bt_audio_call_transmit(worker, send);
}

/****************************************************************************
 * Name: ny_bt_audio_thread
 ****************************************************************************/

static void *ny_bt_audio_thread(void *arg)
{
  static struct ny_bt_audio_worker_s worker;

  (void)arg;
  memset(&worker, 0, sizeof(worker));
  worker.sbc = ny_sbc_decoder_create(false);
  worker.msbc = ny_sbc_decoder_create(true);
  worker.encoder = ny_msbc_encoder_create();
  if (worker.sbc == NULL || worker.msbc == NULL || worker.encoder == NULL ||
      ny_pcm_ring_init(&worker.mic_ring, NY_MSBC_RATE * 2 / 4) < 0 ||
      ny_pcm_ring_init(&worker.tx_ring, 2 * NY_BT_SCO_PACKET_MAX) < 0)
    {
      ny_bt_audio_error(-ENOMEM);
      goto out;
    }

  for (;;)
    {
      struct timespec until;
      uint32_t generation;
      bool want_media;
      bool want_call;
      bool msbc;
      bool stop;
      int wait_ms;
      int desired;

      wait_ms = worker.mode == NY_BT_AUDIO_OWNER_CALL    ? 5
                : worker.mode == NY_BT_AUDIO_OWNER_MEDIA ? 20
                                                         : 500;
      clock_gettime(CLOCK_MONOTONIC, &until);
      until.tv_nsec += (long)wait_ms * 1000000;
      if (until.tv_nsec >= 1000000000)
        {
          until.tv_sec++;
          until.tv_nsec -= 1000000000;
        }

      /* Not the wall clock: the product sets that while running. */

      sem_clockwait(&g_bt_audio.wake, CLOCK_MONOTONIC, &until);
      nxmutex_lock(&g_bt_audio.lock);
      stop = g_bt_audio.stop;
      want_call = g_bt_audio.want_call;
      want_media = g_bt_audio.want_media && !g_bt_audio.held;
      msbc = g_bt_audio.call_msbc;
      generation = g_bt_audio.call_generation;
      nxmutex_unlock(&g_bt_audio.lock);
      if (stop)
        {
          break;
        }

      desired = want_call    ? NY_BT_AUDIO_OWNER_CALL
                : want_media ? NY_BT_AUDIO_OWNER_MEDIA
                             : NY_BT_AUDIO_OWNER_NONE;
      if (desired != worker.mode || (desired == NY_BT_AUDIO_OWNER_CALL &&
                                     generation != worker.call_generation))
        {
          ny_bt_audio_leave(&worker);
          worker.mode = desired;
          worker.retry_at = 0;
          if (desired == NY_BT_AUDIO_OWNER_CALL)
            {
              worker.call_generation = generation;
              worker.msbc_call = msbc;
              worker.tx_credit = 0;
              worker.tx_packet = 0;
              worker.tx_sequence = 0;
              worker.last_credit = 0;
              worker.last_rx = ny_bt_audio_now();
              ny_pcm_ring_clear(&worker.mic_ring);
              ny_pcm_ring_clear(&worker.tx_ring);
              ny_msbc_deframer_reset(&worker.deframer);
              ny_sbc_decoder_reset(worker.msbc);
              ny_pcm_gate_init(
                  &worker.gate, CONFIG_NYABULA_CORE_BT_GATE_THRESHOLD,
                  CONFIG_NYABULA_CORE_BT_GATE_ATTENUATION_DB,
                  CONFIG_NYABULA_CORE_BT_GATE_HANG_MS, msbc ? 75 : 30);
            }
        }

      if (worker.mode == NY_BT_AUDIO_OWNER_MEDIA)
        {
          ny_bt_audio_media_service(&worker);
        }
      else if (worker.mode == NY_BT_AUDIO_OWNER_CALL)
        {
          ny_bt_audio_call_service(&worker);
        }
    }

out:
  ny_bt_audio_leave(&worker);
  ny_pcm_ring_free(&worker.mic_ring);
  ny_pcm_ring_free(&worker.tx_ring);
  ny_sbc_decoder_destroy(worker.sbc);
  ny_sbc_decoder_destroy(worker.msbc);
  ny_msbc_encoder_destroy(worker.encoder);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_bt_audio_start
 ****************************************************************************/

int ny_bt_audio_start(void)
{
  struct sched_param param;
  pthread_attr_t attr;
  int ret;

  nxmutex_lock(&g_bt_audio.lock);
  if (g_bt_audio.running)
    {
      nxmutex_unlock(&g_bt_audio.lock);
      return 0;
    }

  ret = ny_pcm_ring_init(&g_bt_audio.media_ring, NY_BT_MEDIA_RING_BYTES);
  if (ret == 0)
    {
      ret = ny_pcm_ring_init(&g_bt_audio.sco_ring, NY_BT_SCO_RING_BYTES);
    }

  if (ret == 0 && sem_init(&g_bt_audio.wake, 0, 0) < 0)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      ny_pcm_ring_free(&g_bt_audio.media_ring);
      ny_pcm_ring_free(&g_bt_audio.sco_ring);
      nxmutex_unlock(&g_bt_audio.lock);
      return ret;
    }

  g_bt_audio.stop = false;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, NY_BT_AUDIO_STACK);
  param.sched_priority = CONFIG_NYABULA_CORE_BT_AUDIO_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  ret = pthread_create(&g_bt_audio.thread, &attr, ny_bt_audio_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      sem_destroy(&g_bt_audio.wake);
      ny_pcm_ring_free(&g_bt_audio.media_ring);
      ny_pcm_ring_free(&g_bt_audio.sco_ring);
      nxmutex_unlock(&g_bt_audio.lock);
      return -ret;
    }

  pthread_setname_np(g_bt_audio.thread, "nybtaudio");
  g_bt_audio.running = true;
  nxmutex_unlock(&g_bt_audio.lock);
  return 0;
}

/****************************************************************************
 * Name: ny_bt_audio_shutdown
 ****************************************************************************/

void ny_bt_audio_shutdown(void)
{
  bool running;

  nxmutex_lock(&g_bt_audio.lock);
  running = g_bt_audio.running;
  g_bt_audio.stop = true;
  nxmutex_unlock(&g_bt_audio.lock);
  if (!running)
    {
      return;
    }

  sem_post(&g_bt_audio.wake);
  pthread_join(g_bt_audio.thread, NULL);
  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.running = false;
  g_bt_audio.want_media = false;
  g_bt_audio.want_call = false;
  g_bt_audio.send = NULL;
  ny_pcm_ring_free(&g_bt_audio.media_ring);
  ny_pcm_ring_free(&g_bt_audio.sco_ring);
  sem_destroy(&g_bt_audio.wake);
  nxmutex_unlock(&g_bt_audio.lock);
}

/****************************************************************************
 * Name: ny_bt_audio_media_configure
 ****************************************************************************/

void ny_bt_audio_media_configure(uint32_t rate, uint8_t channels,
                                 uint16_t frame_samples)
{
  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.media_rate = rate;
  g_bt_audio.media_channels = channels;
  g_bt_audio.media_frame_samples = frame_samples;
  nxmutex_unlock(&g_bt_audio.lock);
}

/****************************************************************************
 * Name: ny_bt_audio_media_start
 ****************************************************************************/

void ny_bt_audio_media_start(void)
{
  nxmutex_lock(&g_bt_audio.lock);
  ny_pcm_ring_clear(&g_bt_audio.media_ring);
  g_bt_audio.media_buffered_frames = 0;
  g_bt_audio.want_media = true;
  g_bt_audio.status.streaming = true;
  nxmutex_unlock(&g_bt_audio.lock);
  if (g_bt_audio.running)
    {
      sem_post(&g_bt_audio.wake);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_media_stop
 ****************************************************************************/

void ny_bt_audio_media_stop(void)
{
  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.want_media = false;
  g_bt_audio.status.streaming = false;
  g_bt_audio.status.media_buffer_ms = 0;
  ny_pcm_ring_clear(&g_bt_audio.media_ring);
  g_bt_audio.media_buffered_frames = 0;
  nxmutex_unlock(&g_bt_audio.lock);
  if (g_bt_audio.running)
    {
      sem_post(&g_bt_audio.wake);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_media_packet
 ****************************************************************************/

void ny_bt_audio_media_packet(const uint8_t *data, size_t size)
{
  bool queued = false;

  if (data == NULL || size < 2 || size > NY_BT_MEDIA_PACKET_MAX)
    {
      return;
    }

  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.status.media_packets++;
  if (g_bt_audio.want_media && !g_bt_audio.held && !g_bt_audio.want_call &&
      ny_pcm_ring_put(&g_bt_audio.media_ring, data, (uint16_t)size))
    {
      g_bt_audio.media_buffered_frames += data[0] & 0x0f;
      queued = true;
    }
  else
    {
      g_bt_audio.status.media_dropped++;
    }

  nxmutex_unlock(&g_bt_audio.lock);
  if (queued)
    {
      sem_post(&g_bt_audio.wake);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_call_start
 ****************************************************************************/

void ny_bt_audio_call_start(bool msbc, ny_bt_audio_sco_send_t send)
{
  nxmutex_lock(&g_bt_audio.lock);
  ny_pcm_ring_clear(&g_bt_audio.sco_ring);
  g_bt_audio.call_msbc = msbc;
  g_bt_audio.send = send;
  g_bt_audio.want_call = true;
  g_bt_audio.call_generation++;
  g_bt_audio.status.msbc = msbc;
  g_bt_audio.status.sco_rx_packets = 0;
  g_bt_audio.status.sco_rx_bad = 0;
  g_bt_audio.status.sco_tx_packets = 0;
  g_bt_audio.status.sco_tx_failed = 0;
  nxmutex_unlock(&g_bt_audio.lock);
  if (g_bt_audio.running)
    {
      sem_post(&g_bt_audio.wake);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_call_stop
 ****************************************************************************/

void ny_bt_audio_call_stop(void)
{
  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.want_call = false;
  g_bt_audio.send = NULL;
  g_bt_audio.status.gate_closed = false;
  ny_pcm_ring_clear(&g_bt_audio.sco_ring);
  nxmutex_unlock(&g_bt_audio.lock);
  if (g_bt_audio.running)
    {
      sem_post(&g_bt_audio.wake);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_call_packet
 ****************************************************************************/

void ny_bt_audio_call_packet(const uint8_t *data, size_t size,
                             uint8_t packet_status)
{
  uint8_t record[NY_BT_SCO_PACKET_MAX + 1];
  bool queued = false;

  if (data == NULL || size == 0 || size > NY_BT_SCO_PACKET_MAX)
    {
      return;
    }

  record[0] = packet_status;
  memcpy(record + 1, data, size);
  nxmutex_lock(&g_bt_audio.lock);
  g_bt_audio.status.sco_rx_packets++;
  if (packet_status != 0)
    {
      g_bt_audio.status.sco_rx_bad++;
    }

  if (g_bt_audio.want_call)
    {
      queued =
          ny_pcm_ring_put(&g_bt_audio.sco_ring, record, (uint16_t)(size + 1));
    }

  nxmutex_unlock(&g_bt_audio.lock);
  if (queued)
    {
      sem_post(&g_bt_audio.wake);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_hold
 ****************************************************************************/

int ny_bt_audio_hold(int timeout_ms)
{
  uint64_t deadline = ny_bt_audio_now() + (timeout_ms > 0 ? timeout_ms : 0);

  nxmutex_lock(&g_bt_audio.lock);
  if (g_bt_audio.want_call)
    {
      nxmutex_unlock(&g_bt_audio.lock);
      return -EBUSY;
    }

  g_bt_audio.held = true;
  g_bt_audio.status.held = true;
  ny_pcm_ring_clear(&g_bt_audio.media_ring);
  g_bt_audio.media_buffered_frames = 0;
  nxmutex_unlock(&g_bt_audio.lock);
  if (g_bt_audio.running)
    {
      sem_post(&g_bt_audio.wake);
    }

  for (;;)
    {
      struct timespec nap = {
        .tv_sec = 0,
        .tv_nsec = 5000000,
      };

      bool closed;

      nxmutex_lock(&g_bt_audio.lock);
      closed = g_bt_audio.device_closed || !g_bt_audio.running;
      nxmutex_unlock(&g_bt_audio.lock);
      if (closed)
        {
          return 0;
        }

      if (ny_bt_audio_now() >= deadline)
        {
          return -ETIMEDOUT;
        }

      nanosleep(&nap, NULL);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_release
 ****************************************************************************/

void ny_bt_audio_release(void)
{
  bool held;

  nxmutex_lock(&g_bt_audio.lock);
  held = g_bt_audio.held;
  g_bt_audio.held = false;
  g_bt_audio.status.held = false;
  nxmutex_unlock(&g_bt_audio.lock);
  if (held && g_bt_audio.running)
    {
      sem_post(&g_bt_audio.wake);
    }
}

/****************************************************************************
 * Name: ny_bt_audio_status
 ****************************************************************************/

void ny_bt_audio_status(struct ny_bt_audio_status_s *status)
{
  nxmutex_lock(&g_bt_audio.lock);
  *status = g_bt_audio.status;
  nxmutex_unlock(&g_bt_audio.lock);
}

#endif /* CONFIG_NYABULA_CORE_BT */
