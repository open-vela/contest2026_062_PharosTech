/****************************************************************************
 * app/nyabula_core/ny_product_intercom.c
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

#include "ny_product_intercom.h"
#include "ny_audio_stream.h"
#include "ny_product_privacy.h"
#include "ny_voice.h"
#include <errno.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <nuttx/mutex.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NY_INTERCOM_RATE         16000
#define NY_INTERCOM_SAMPLES      1600
#define NY_INTERCOM_CAPTURE      8
#define NY_INTERCOM_PLAY         4
#define NY_INTERCOM_STACK        16384
#define NY_INTERCOM_IO_MS        250
#define NY_INTERCOM_DRAIN_MS     500
#define NY_INTERCOM_TALK_IDLE_MS 1000
#define NY_INTERCOM_MIC_IDLE_MS  2000
#define NY_INTERCOM_PLAY_CHUNK   640
#define NY_INTERCOM_PLAY_BUFFERS 12
#define NY_INTERCOM_PREFILL      8

struct ny_intercom_packet_s
{
  uint8_t bytes[NY_INTERCOM_SAMPLES * 2];
  size_t length;
};

static mutex_t g_intercom_lock = NXMUTEX_INITIALIZER;
static mutex_t g_intercom_request_lock = NXMUTEX_INITIALIZER;
static pthread_t g_intercom_thread;
static bool g_intercom_joinable;
static bool g_intercom_running;
static bool g_intercom_stop;
static bool g_intercom_talk;
static uint64_t g_intercom_owner;
static uint64_t g_intercom_next_owner;
static uint64_t g_intercom_revision;
static uint64_t g_intercom_capture_sequence;
static uint64_t g_intercom_input_sequence;
static uint64_t g_intercom_last_input;
static uint64_t g_intercom_captured;
static uint64_t g_intercom_played;
static unsigned int g_intercom_head;
static unsigned int g_intercom_count;
static unsigned int g_intercom_level;
static int g_intercom_error;
static const char *g_intercom_phase = "off";
static struct ny_intercom_packet_s g_intercom_capture[NY_INTERCOM_CAPTURE];
static struct ny_intercom_packet_s g_intercom_play[NY_INTERCOM_PLAY];

static void ny_intercom_phase(const char *phase);
static cJSON *ny_intercom_status(void);
static int ny_intercom_open(struct ny_audio_stream_s **stream, bool talk);
static void *ny_intercom_worker(void *arg);
static int ny_intercom_start(struct ny_intercom_client_s *client);
static int ny_intercom_push(const cJSON *data);

/****************************************************************************
 * Name: ny_intercom_phase
 ****************************************************************************/

static void ny_intercom_phase(const char *phase)
{
  if (strcmp(g_intercom_phase, phase))
    {
      g_intercom_phase = phase;
      g_intercom_revision++;
    }
}

/****************************************************************************
 * Name: ny_intercom_status
 ****************************************************************************/

static cJSON *ny_intercom_status(void)
{
  cJSON *out = cJSON_CreateObject();
  if (out == NULL)
    return NULL;
  bool ok =
      cJSON_AddStringToObject(out, "phase", g_intercom_phase) != NULL &&
      cJSON_AddStringToObject(out, "duplex", "push-to-talk") != NULL &&
      cJSON_AddStringToObject(out, "format", "s16le") != NULL &&
      cJSON_AddNumberToObject(out, "sampleRate", NY_INTERCOM_RATE) != NULL &&
      cJSON_AddNumberToObject(out, "channels", 1) != NULL &&
      cJSON_AddNumberToObject(out, "capturedSamples", g_intercom_captured) !=
          NULL &&
      cJSON_AddNumberToObject(out, "playedSamples", g_intercom_played) !=
          NULL &&
      cJSON_AddNumberToObject(out, "queuedPackets", g_intercom_count) !=
          NULL &&
      cJSON_AddNumberToObject(out, "level", g_intercom_level) != NULL &&
      cJSON_AddNumberToObject(out, "lastError", g_intercom_error) != NULL;
  if (!ok)
    {
      cJSON_Delete(out);
      return NULL;
    }
  return out;
}

/****************************************************************************
 * Name: ny_intercom_open
 ****************************************************************************/

static int ny_intercom_open(struct ny_audio_stream_s **stream, bool talk)
{
  const struct ny_audio_stream_config_s config = {
    .path = talk ? CONFIG_NYABULA_CORE_AUDIO_OUTPUT_DEVICE
                 : CONFIG_NYABULA_CORE_AUDIO_INPUT_DEVICE,
    .capture = !talk,
    .wav = talk,
    .rate = NY_INTERCOM_RATE,
    .channels = 1,
    .chunk = talk ? NY_INTERCOM_PLAY_CHUNK : NY_INTERCOM_SAMPLES * 2,
    .prefill = talk ? NY_INTERCOM_PREFILL : 0,
    .buffers = talk ? NY_INTERCOM_PLAY_BUFFERS : NY_INTERCOM_PLAY
  };
  return ny_audio_stream_open(stream, &config);
}

/****************************************************************************
 * Name: ny_intercom_worker
 ****************************************************************************/

static void *ny_intercom_worker(void *arg)
{
  (void)arg;
  struct ny_audio_stream_s *stream = NULL;
  struct ny_intercom_packet_s packet;
  bool talking = false;
  uint64_t heard_at = ny_product_time_ms(true);
  /* Claim only gates the existing voice microphone. Audio RESERVE refuses
   * other players rather than stopping them or changing their settings.
   * claim increments its reference even on timeout, so always release it.
   */
  int ret = ny_voice_speaker_claim();
  while (ret == 0)
    {
      nxmutex_lock(&g_intercom_lock);
      bool stop = g_intercom_stop;
      if (g_intercom_talk && ny_product_time_ms(true) - g_intercom_last_input >
                                 NY_INTERCOM_TALK_IDLE_MS)
        g_intercom_talk = false;
      bool wanted = g_intercom_talk || g_intercom_count != 0;
      nxmutex_unlock(&g_intercom_lock);
      if (stop)
        break;
      if (stream == NULL || wanted != talking)
        {
          if (stream != NULL && talking)
            {
              ret = ny_audio_stream_drain(stream, NY_INTERCOM_DRAIN_MS);
              if (ret < 0)
                break;
            }
          ny_audio_stream_close(stream);
          stream = NULL;
          talking = wanted;
          ret = ny_intercom_open(&stream, talking);
          if (ret < 0)
            break;
          heard_at = ny_product_time_ms(true);
          nxmutex_lock(&g_intercom_lock);
          g_intercom_level = 0;
          ny_intercom_phase(talking ? "talking" : "listening");
          nxmutex_unlock(&g_intercom_lock);
        }
      if (talking)
        {
          nxmutex_lock(&g_intercom_lock);
          packet.length = 0;
          if (g_intercom_count != 0)
            {
              packet = g_intercom_play[g_intercom_head];
              g_intercom_head = (g_intercom_head + 1) % NY_INTERCOM_PLAY;
              g_intercom_count--;
            }
          nxmutex_unlock(&g_intercom_lock);
          if (packet.length == 0)
            {
              usleep(10000);
              continue;
            }
          ssize_t written = ny_audio_stream_write(
              stream, packet.bytes, packet.length, NY_INTERCOM_IO_MS);
          if (written != (ssize_t)packet.length)
            {
              ret = written < 0 ? (int)written : -ETIMEDOUT;
              break;
            }
          nxmutex_lock(&g_intercom_lock);
          g_intercom_played += packet.length / 2;
          g_intercom_revision++;
          nxmutex_unlock(&g_intercom_lock);
        }
      else
        {
          ssize_t got = ny_audio_stream_read(
              stream, packet.bytes, sizeof(packet.bytes), NY_INTERCOM_IO_MS);
          if (got < 0 || (got & 1))
            {
              ret = got < 0 ? (int)got : -EIO;
              break;
            }
          if (got == 0)
            {
              if (ny_product_time_ms(true) - heard_at >
                  NY_INTERCOM_MIC_IDLE_MS)
                ret = -ETIMEDOUT;
              continue;
            }
          heard_at = ny_product_time_ms(true);
          packet.length = got;
          uint64_t squares = 0;
          for (size_t i = 0; i < packet.length; i += 2)
            {
              int32_t sample = (int16_t)((uint16_t)packet.bytes[i] |
                                         (uint16_t)packet.bytes[i + 1] << 8);
              squares += (int64_t)sample * sample;
            }
          nxmutex_lock(&g_intercom_lock);
          /* A talk request arriving during read must not leak old room audio
           * back to a caller who has already switched to speaking.
           */
          if (!g_intercom_talk && !g_intercom_stop)
            {
              g_intercom_capture[g_intercom_capture_sequence++ %
                                 NY_INTERCOM_CAPTURE] = packet;
              g_intercom_captured += packet.length / 2;
              g_intercom_level = sqrt((double)squares / (packet.length / 2));
              g_intercom_revision++;
            }
          nxmutex_unlock(&g_intercom_lock);
        }
    }
  ny_audio_stream_close(stream);
  ny_voice_speaker_release();
  nxmutex_lock(&g_intercom_lock);
  g_intercom_running = false;
  g_intercom_talk = false;
  g_intercom_count = 0;
  g_intercom_level = 0;
  g_intercom_error = ret;
  memset(g_intercom_capture, 0, sizeof(g_intercom_capture));
  memset(g_intercom_play, 0, sizeof(g_intercom_play));
  ny_intercom_phase(ret < 0 ? "error" : "off");
  nxmutex_unlock(&g_intercom_lock);
  return NULL;
}

/****************************************************************************
 * Name: ny_intercom_start
 ****************************************************************************/

static int ny_intercom_start(struct ny_intercom_client_s *client)
{
  int ret = 0;
  nxmutex_lock(&g_intercom_request_lock);
  if (!ny_product_privacy_mic_allowed())
    {
      nxmutex_unlock(&g_intercom_request_lock);
      return -EACCES;
    }
  nxmutex_lock(&g_intercom_lock);
  if (g_intercom_running)
    {
      ret = client->id == g_intercom_owner && !g_intercom_stop ? 0 : -EBUSY;
      nxmutex_unlock(&g_intercom_lock);
      nxmutex_unlock(&g_intercom_request_lock);
      return ret;
    }
  nxmutex_unlock(&g_intercom_lock);
  if (g_intercom_joinable)
    {
      pthread_join(g_intercom_thread, NULL);
      g_intercom_joinable = false;
    }
  nxmutex_lock(&g_intercom_lock);
  g_intercom_owner = ++g_intercom_next_owner;
  memset(client, 0, sizeof(*client));
  client->id = g_intercom_owner;
  g_intercom_stop = g_intercom_talk = false;
  g_intercom_running = true;
  g_intercom_capture_sequence = g_intercom_input_sequence = 0;
  g_intercom_captured = g_intercom_played = 0;
  g_intercom_head = g_intercom_count = g_intercom_level = 0;
  g_intercom_error = 0;
  ny_intercom_phase("starting");
  pthread_attr_t attr;
  struct sched_param scheduling = {
    .sched_priority = CONFIG_NYABULA_CORE_VOICE_PRIORITY + 10
  };
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, NY_INTERCOM_STACK);
  pthread_attr_setschedparam(&attr, &scheduling);
  ret = pthread_create(&g_intercom_thread, &attr, ny_intercom_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      g_intercom_running = false;
      g_intercom_error = -ret;
      client->id = 0;
      ny_intercom_phase("error");
      ret = -ret;
    }
  else
    g_intercom_joinable = true;
  nxmutex_unlock(&g_intercom_lock);
  nxmutex_unlock(&g_intercom_request_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_intercom_close
 ****************************************************************************/

void ny_product_intercom_close(struct ny_intercom_client_s *client)
{
  nxmutex_lock(&g_intercom_lock);
  if (client->id != 0 && client->id == g_intercom_owner)
    g_intercom_stop = true;
  memset(client, 0, sizeof(*client));
  nxmutex_unlock(&g_intercom_lock);
}

/****************************************************************************
 * Name: ny_intercom_push
 ****************************************************************************/

static int ny_intercom_push(const cJSON *data)
{
  const cJSON *number = cJSON_GetObjectItemCaseSensitive(data, "sequence");
  const char *encoded =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "data"));
  if (!cJSON_IsNumber(number) || !isfinite(number->valuedouble) ||
      number->valuedouble < 0 || number->valuedouble > 4294967295.0 ||
      floor(number->valuedouble) != number->valuedouble || encoded == NULL ||
      strlen(encoded) > ((NY_INTERCOM_SAMPLES * 2 + 2) / 3) * 4)
    return -EINVAL;
  if (!g_intercom_talk)
    return -EACCES;
  if (number->valuedouble != g_intercom_input_sequence)
    return -ESTALE;
  if (g_intercom_count == NY_INTERCOM_PLAY)
    return -EBUSY;
  struct ny_intercom_packet_s *packet =
      &g_intercom_play[(g_intercom_head + g_intercom_count) %
                       NY_INTERCOM_PLAY];
  int ret = mbedtls_base64_decode(
      packet->bytes, sizeof(packet->bytes), &packet->length,
      (const unsigned char *)encoded, strlen(encoded));
  if (ret != 0 || packet->length == 0 || (packet->length & 1))
    return -EINVAL;
  g_intercom_count++;
  g_intercom_input_sequence++;
  g_intercom_last_input = ny_product_time_ms(true);
  return 0;
}

/****************************************************************************
 * Name: ny_product_intercom_request
 ****************************************************************************/

int ny_product_intercom_request(const struct ny_product_caller_s *caller,
                                struct ny_intercom_client_s *client,
                                const char *topic, const cJSON *data,
                                cJSON **result)
{
  bool start = !strcmp(topic, "intercom.start");
  bool stop = !strcmp(topic, "intercom.stop");
  bool talk = !strcmp(topic, "intercom.talk.start");
  bool listen = !strcmp(topic, "intercom.talk.stop");
  bool audio = !strcmp(topic, "intercom.audio");
  bool status = !strcmp(topic, "intercom.status");
  if (!start && !stop && !talk && !listen && !audio && !status)
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER || client == NULL)
    return -EACCES;
  int ret = 0;
  if (start)
    ret = ny_intercom_start(client);
  if (stop)
    ny_product_intercom_close(client);
  nxmutex_lock(&g_intercom_lock);
  if (ret == 0 && (talk || listen || audio) &&
      (client->id == 0 || client->id != g_intercom_owner ||
       !g_intercom_running || g_intercom_stop))
    ret = -ENODEV;
  if (ret == 0 && talk && !g_intercom_talk)
    {
      g_intercom_talk = true;
      g_intercom_input_sequence = 0;
      g_intercom_last_input = ny_product_time_ms(true);
      client->sequence = g_intercom_capture_sequence;
      ny_intercom_phase("switching");
    }
  if (ret == 0 && listen)
    {
      g_intercom_talk = false;
      client->sequence = g_intercom_capture_sequence;
    }
  if (ret == 0 && audio)
    ret = ny_intercom_push(data);
  if (ret == 0)
    {
      *result = ny_intercom_status();
      if (*result == NULL)
        ret = -ENOMEM;
    }
  nxmutex_unlock(&g_intercom_lock);
  if (ret == -ENOMEM && start)
    ny_product_intercom_close(client);
  return ret;
}

/****************************************************************************
 * Name: ny_product_intercom_next
 ****************************************************************************/

int ny_product_intercom_next(struct ny_intercom_client_s *client,
                             const char **topic, cJSON **result)
{
  *result = NULL;
  if (!ny_product_privacy_mic_allowed())
    return -EACCES;
  int ret = -EAGAIN;
  nxmutex_lock(&g_intercom_lock);
  if (client->id == 0 || client->id != g_intercom_owner)
    goto out;
  if (client->revision != g_intercom_revision)
    {
      *result = ny_intercom_status();
      if (*result == NULL)
        ret = -ENOMEM;
      else
        {
          *topic = "intercom.state";
          client->revision = g_intercom_revision;
          ret = 0;
          if (!g_intercom_running)
            client->id = 0;
        }
      goto out;
    }
  if (!g_intercom_running || g_intercom_talk ||
      strcmp(g_intercom_phase, "listening") ||
      client->sequence >= g_intercom_capture_sequence)
    goto out;
  /* Stay near live audio; never play a growing backlog after a stalled link.
   */
  if (g_intercom_capture_sequence - client->sequence > 2)
    client->sequence = g_intercom_capture_sequence - 1;
  struct ny_intercom_packet_s *packet =
      &g_intercom_capture[client->sequence % NY_INTERCOM_CAPTURE];
  size_t size = (packet->length + 2) / 3 * 4 + 1;
  unsigned char *encoded = malloc(size);
  size_t written;
  if (encoded == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }
  ret = mbedtls_base64_encode(encoded, size, &written, packet->bytes,
                              packet->length);
  cJSON *out = ret == 0 ? cJSON_CreateObject() : NULL;
  bool valid =
      out != NULL &&
      cJSON_AddNumberToObject(out, "sequence", client->sequence) != NULL &&
      cJSON_AddNumberToObject(out, "samples", packet->length / 2) != NULL &&
      cJSON_AddStringToObject(out, "data", (char *)encoded) != NULL;
  free(encoded);
  if (!valid)
    {
      cJSON_Delete(out);
      ret = -ENOMEM;
    }
  else
    {
      client->sequence++;
      *result = out;
      *topic = "intercom.audio";
      ret = 0;
    }
out:
  nxmutex_unlock(&g_intercom_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_intercom_shutdown
 ****************************************************************************/

void ny_product_intercom_shutdown(void)
{
  nxmutex_lock(&g_intercom_request_lock);
  nxmutex_lock(&g_intercom_lock);
  g_intercom_stop = true;
  nxmutex_unlock(&g_intercom_lock);
  if (g_intercom_joinable)
    {
      pthread_join(g_intercom_thread, NULL);
      g_intercom_joinable = false;
    }
  nxmutex_unlock(&g_intercom_request_lock);
}
