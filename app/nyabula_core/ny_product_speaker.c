/****************************************************************************
 * app/nyabula_core/ny_product_speaker.c
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

#include "ny_product_speaker.h"
#include "ny_compute.h"
#include "ny_product_privacy.h"
#include "ny_product_store.h"
#include "ny_voice.h"
#include "ny_voice_wire.h"
#include "nyamp_speaker_wire.h"
#include <errno.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <nuttx/mutex.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NY_SPEAKER_TAKES     4U
#define NY_SPEAKER_STACK     16384
#define NY_SPEAKER_ENCODED   537
#define NY_SPEAKER_THRESHOLD .55

enum ny_speaker_job_e
{
  NY_SPK_LOAD,
  NY_SPK_PREPARE,
  NY_SPK_TAKE,
  NY_SPK_TEST_PREPARE,
  NY_SPK_TEST,
  NY_SPK_GATE,
  NY_SPK_FORGET
};
struct ny_speaker_job_s
{
  enum ny_speaker_job_e kind;
  uint32_t epoch;
  uint64_t stream;
  struct nyamp_kws_detected_s wake;
  char label[NYAMP_INLINE_MAX];
};
struct ny_speaker_link_s
{
  struct ny_compute_port_s *port;
  struct ny_voice_wire_s wire;
  uint32_t epoch;
  bool requires_audio;
};
static mutex_t g_speaker_lock = NXMUTEX_INITIALIZER;
static bool g_speaker_loaded, g_speaker_enabled, g_speaker_busy,
    g_speaker_stop;
static bool g_speaker_enrolling, g_speaker_testing, g_speaker_ready,
    g_speaker_matched;
static uint8_t g_speaker_template[NYAMP_SPK_TEMPLATE_SIZE];
static size_t g_speaker_template_size;
static uint64_t g_speaker_revision, g_speaker_until, g_speaker_min_sample;
static uint32_t g_speaker_epoch, g_speaker_probe, g_speaker_generation,
    g_speaker_takes, g_speaker_kept;
static float g_speaker_cohesion, g_speaker_score = -2;
static const char *g_speaker_phase = "idle";
static int g_speaker_error;
static struct ny_speaker_job_s g_speaker_job;

static bool ny_speaker_template_valid(const uint8_t *bytes, size_t size);
static int ny_speaker_save(const uint8_t *bytes, size_t size, bool enabled);
static cJSON *ny_speaker_status(void);
static int ny_speaker_probe_start(uint32_t *id);
static void ny_speaker_probe_stop(uint32_t id);
static int ny_speaker_send(void *arg, const uint8_t *wire, size_t size);
static ssize_t ny_speaker_recv(void *arg, uint8_t *wire, size_t size,
                               int timeout);
static uint64_t ny_speaker_id(void *arg);
static uint32_t ny_speaker_generation(void *arg);
static bool ny_speaker_cancelled(void *arg);
static int ny_speaker_rpc(struct ny_speaker_link_s *link, uint16_t opcode,
                          const uint8_t *body, size_t size, uint8_t *reply,
                          size_t *reply_size);
static int ny_speaker_spawn(enum ny_speaker_job_e kind, uint64_t stream,
                            const struct nyamp_kws_detected_s *wake);
static int ny_speaker_worker(int argc, char **argv);

/****************************************************************************
 * Name: ny_speaker_template_valid
 ****************************************************************************/
static bool ny_speaker_template_valid(const uint8_t *bytes, size_t size)
{
  return bytes && size == NYAMP_SPK_TEMPLATE_SIZE &&
         nyamp_spk_get(bytes, 4) == 0x5056594eU &&
         nyamp_spk_get(bytes + 4, 2) == 1 &&
         nyamp_spk_get(bytes + 6, 2) == 192 &&
         nyamp_spk_get(bytes + 12, 2) >= 3 &&
         nyamp_spk_get(bytes + 12, 2) <= 8 &&
         nyamp_spk_get(bytes + 14, 2) == 0;
}

/****************************************************************************
 * Name: ny_product_speaker_initialize
 ****************************************************************************/
int ny_product_speaker_initialize(void)
{
  nxmutex_lock(&g_speaker_lock);
  if (g_speaker_loaded)
    {
      nxmutex_unlock(&g_speaker_lock);
      return 0;
    }
  cJSON *record = NULL;
  uint64_t revision = 0;
  int ret = ny_product_store_read("speaker", &record, &revision);
  bool enabled = false;
  size_t length = 0;
  uint8_t value[NYAMP_SPK_TEMPLATE_SIZE];
  if (ret == 0 && record)
    {
      const cJSON *flag = cJSON_GetObjectItemCaseSensitive(record, "enabled");
      const cJSON *blob = cJSON_GetObjectItemCaseSensitive(record, "template");
      if (!cJSON_IsBool(flag) || !cJSON_IsString(blob) ||
          strlen(blob->valuestring) >= NY_SPEAKER_ENCODED)
        ret = -EINVAL;
      else
        {
          enabled = cJSON_IsTrue(flag);
          if (blob->valuestring[0])
            {
              if (mbedtls_base64_decode(
                      value, sizeof(value), &length,
                      (const unsigned char *)blob->valuestring,
                      strlen(blob->valuestring)) ||
                  !ny_speaker_template_valid(value, length))
                ret = -EINVAL;
            }
          if (enabled && !length)
            ret = -EINVAL;
        }
    }
  if (ret == 0)
    {
      g_speaker_enabled = enabled;
      g_speaker_revision = revision;
      g_speaker_template_size = length;
      memset(g_speaker_template, 0, sizeof(g_speaker_template));
      if (length)
        memcpy(g_speaker_template, value, length);
      g_speaker_loaded = true;
    }
  cJSON_Delete(record);
  nxmutex_unlock(&g_speaker_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_speaker_save
 ****************************************************************************/
static int ny_speaker_save(const uint8_t *bytes, size_t size, bool enabled)
{
  unsigned char encoded[NY_SPEAKER_ENCODED] = { 0 };
  size_t length = 0;
  if (size &&
      (!ny_speaker_template_valid(bytes, size) ||
       mbedtls_base64_encode(encoded, sizeof(encoded), &length, bytes, size)))
    return -EINVAL;
  cJSON *record = cJSON_CreateObject();
  uint64_t revision = 0;
  if (!record || !cJSON_AddBoolToObject(record, "enabled", enabled) ||
      !cJSON_AddStringToObject(record, "template", (char *)encoded))
    {
      cJSON_Delete(record);
      return -ENOMEM;
    }
  int ret =
      ny_product_store_write("speaker", record, g_speaker_revision, &revision);
  cJSON_Delete(record);
  if (ret == 0)
    {
      g_speaker_enabled = enabled;
      g_speaker_revision = revision;
      if (bytes != g_speaker_template)
        {
          memset(g_speaker_template, 0, sizeof(g_speaker_template));
          if (size)
            memcpy(g_speaker_template, bytes, size);
        }
      g_speaker_template_size = size;
      if (!size)
        {
          g_speaker_takes = g_speaker_kept = 0;
          g_speaker_cohesion = 0;
          g_speaker_score = -2;
          g_speaker_matched = false;
        }
    }
  return ret;
}

/****************************************************************************
 * Name: ny_speaker_status
 ****************************************************************************/
static cJSON *ny_speaker_status(void)
{
  cJSON *out = cJSON_CreateObject();
  if (!out)
    return NULL;
  nxmutex_lock(&g_speaker_lock);
  bool ok =
      cJSON_AddBoolToObject(out, "installed",
                            access("/data/models/speaker/model.onnx", F_OK) ==
                                0) &&
      cJSON_AddBoolToObject(out, "ready",
                            g_speaker_ready && g_speaker_generation ==
                                                   ny_compute_generation()) &&
      cJSON_AddBoolToObject(out, "registered", g_speaker_template_size != 0) &&
      cJSON_AddBoolToObject(out, "enabled", g_speaker_enabled) &&
      cJSON_AddBoolToObject(out, "busy", g_speaker_busy) &&
      cJSON_AddBoolToObject(out, "enrolling", g_speaker_enrolling) &&
      cJSON_AddBoolToObject(out, "testing", g_speaker_testing) &&
      cJSON_AddStringToObject(out, "phase", g_speaker_phase) &&
      cJSON_AddNumberToObject(out, "takes", g_speaker_takes) &&
      cJSON_AddNumberToObject(out, "required", NY_SPEAKER_TAKES) &&
      cJSON_AddNumberToObject(out, "kept", g_speaker_kept) &&
      cJSON_AddNumberToObject(out, "cohesion", g_speaker_cohesion) &&
      cJSON_AddNumberToObject(out, "score", g_speaker_score) &&
      cJSON_AddNumberToObject(out, "threshold", NY_SPEAKER_THRESHOLD) &&
      cJSON_AddBoolToObject(out, "matched", g_speaker_matched) &&
      cJSON_AddNumberToObject(out, "lastError", g_speaker_error) &&
      cJSON_AddNumberToObject(out, "revision", g_speaker_revision);
  nxmutex_unlock(&g_speaker_lock);
  if (!ok)
    {
      cJSON_Delete(out);
      return NULL;
    }
  return out;
}

/****************************************************************************
 * Name: ny_speaker_probe_start
 ****************************************************************************/
static int ny_speaker_probe_start(uint32_t *id)
{
  const struct ny_product_caller_s caller = { "speaker", NY_PRODUCT_OWNER,
                                              true };
  cJSON *input = cJSON_CreateObject(), *out = NULL;
  if (!input)
    return -ENOMEM;
  cJSON_AddStringToObject(input, "mode", "wake");
  cJSON_AddNumberToObject(input, "durationMs", 30000);
  int ret = ny_voice_request(&caller, "voice.test.start", input, &out);
  cJSON_Delete(input);
  if (ret == 0)
    {
      const cJSON *probe = cJSON_GetObjectItemCaseSensitive(out, "test");
      const cJSON *number = cJSON_GetObjectItemCaseSensitive(probe, "id");
      if (!cJSON_IsNumber(number) || number->valuedouble < 1)
        ret = -EPROTO;
      else
        *id = (uint32_t)number->valuedouble;
    }
  cJSON_Delete(out);
  return ret;
}

/****************************************************************************
 * Name: ny_speaker_probe_stop
 ****************************************************************************/
static void ny_speaker_probe_stop(uint32_t id)
{
  if (!id)
    return;
  const struct ny_product_caller_s caller = { "speaker", NY_PRODUCT_OWNER,
                                              true };
  cJSON *input = cJSON_CreateObject(), *out = NULL;
  if (!input)
    return;
  cJSON_AddNumberToObject(input, "id", id);
  ny_voice_request(&caller, "voice.test.stop", input, &out);
  cJSON_Delete(input);
  cJSON_Delete(out);
}

/****************************************************************************
 * Name: ny_speaker_send
 ****************************************************************************/
static int ny_speaker_send(void *arg, const uint8_t *wire, size_t size)
{
  return ny_compute_port_send(((struct ny_speaker_link_s *)arg)->port, wire,
                              size);
}
/****************************************************************************
 * Name: ny_speaker_recv
 ****************************************************************************/
static ssize_t ny_speaker_recv(void *arg, uint8_t *wire, size_t size,
                               int timeout)
{
  return ny_compute_port_recv(((struct ny_speaker_link_s *)arg)->port, wire,
                              size, timeout);
}
/****************************************************************************
 * Name: ny_speaker_id
 ****************************************************************************/
static uint64_t ny_speaker_id(void *arg)
{
  (void)arg;
  return ny_compute_request_id();
}
/****************************************************************************
 * Name: ny_speaker_generation
 ****************************************************************************/
static uint32_t ny_speaker_generation(void *arg)
{
  (void)arg;
  return ny_compute_generation();
}
/****************************************************************************
 * Name: ny_speaker_cancelled
 ****************************************************************************/
static bool ny_speaker_cancelled(void *arg)
{
  struct ny_speaker_link_s *link = arg;
  nxmutex_lock(&g_speaker_lock);
  bool stop = g_speaker_stop || link->epoch != g_speaker_epoch;
  nxmutex_unlock(&g_speaker_lock);
  return stop || (link->requires_audio && !ny_product_privacy_mic_allowed());
}
/****************************************************************************
 * Name: ny_speaker_rpc
 ****************************************************************************/
static int ny_speaker_rpc(struct ny_speaker_link_s *link, uint16_t opcode,
                          const uint8_t *body, size_t size, uint8_t *reply,
                          size_t *reply_size)
{
  return ny_voice_wire_request(
      &link->wire, NYAMP_SERVICE_SPEAKER, opcode, body, size,
      opcode == NYAMP_SPK_LOAD ? 90000 : 5000, reply, reply_size);
}

/****************************************************************************
 * Name: ny_speaker_spawn
 * Called with the speaker lock held. No audio is copied by the pump.
 ****************************************************************************/
static int ny_speaker_spawn(enum ny_speaker_job_e kind, uint64_t stream,
                            const struct nyamp_kws_detected_s *wake)
{
  if (g_speaker_busy)
    return -EBUSY;
  memset(&g_speaker_job, 0, sizeof(g_speaker_job));
  g_speaker_job.kind = kind;
  g_speaker_job.epoch = g_speaker_epoch;
  g_speaker_job.stream = stream;
  if (wake)
    {
      g_speaker_job.wake = *wake;
      g_speaker_job.wake.label = NULL;
      if (wake->label && wake->label_length < sizeof(g_speaker_job.label))
        memcpy(g_speaker_job.label, wake->label, wake->label_length);
      else
        g_speaker_job.wake.label_length = 0;
    }
  g_speaker_busy = true;
  g_speaker_stop = false;
  g_speaker_error = 0;
  if (kind == NY_SPK_GATE || kind == NY_SPK_TEST)
    {
      g_speaker_score = -2;
      g_speaker_matched = false;
    }
  g_speaker_phase = kind == NY_SPK_LOAD || kind == NY_SPK_PREPARE ||
                            kind == NY_SPK_TEST_PREPARE
                        ? "loading"
                        : "processing";
  int pid = task_create("ny_speaker", 100, NY_SPEAKER_STACK, ny_speaker_worker,
                        NULL);
  if (pid < 0)
    {
      g_speaker_busy = false;
      g_speaker_error = -errno;
      g_speaker_phase = "error";
      return -errno;
    }
  return 0;
}

/****************************************************************************
 * Name: ny_speaker_worker
 ****************************************************************************/
static int ny_speaker_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  nxmutex_lock(&g_speaker_lock);
  struct ny_speaker_job_s job = g_speaker_job;
  nxmutex_unlock(&g_speaker_lock);
  job.wake.label = job.label;
  struct ny_speaker_link_s *link = calloc(1, sizeof(*link));
  uint8_t reply[NYAMP_INLINE_MAX], reference[NYAMP_SPK_TEMPLATE_SIZE];
  size_t bytes = sizeof(reply), reference_size = 0;
  uint32_t probe = 0;
  bool finish = false, deliver = false, committing = false;
  int ret = link ? ny_compute_port_open(&link->port) : -ENOMEM;
  if (ret < 0)
    goto done;
  link->epoch = job.epoch;
  link->requires_audio = job.kind != NY_SPK_LOAD && job.kind != NY_SPK_FORGET;
  struct ny_voice_wire_io_s io = { .send = ny_speaker_send,
                                   .recv = ny_speaker_recv,
                                   .request_id = ny_speaker_id,
                                   .generation = ny_speaker_generation,
                                   .interrupted = ny_speaker_cancelled,
                                   .arg = link };
  ny_voice_wire_init(&link->wire, &io);
  if (job.kind == NY_SPK_FORGET)
    {
      for (int n = 0; n < 20; n++)
        {
          ret = ny_speaker_rpc(link, NYAMP_SPK_DELETE, NULL, 0, NULL, NULL);
          if (ret != -EBUSY)
            break;
          usleep(50000);
        }
      if (ret == -ENOENT)
        ret = 0;
      goto done;
    }
  ret = ny_speaker_rpc(link, NYAMP_SPK_LOAD, (const uint8_t *)"speaker", 7,
                       NULL, NULL);
  if (ret < 0)
    goto done;
  nxmutex_lock(&g_speaker_lock);
  g_speaker_ready = true;
  g_speaker_generation = ny_compute_generation();
  nxmutex_unlock(&g_speaker_lock);
  if (job.kind == NY_SPK_LOAD)
    goto done;
  if (job.kind == NY_SPK_TEST_PREPARE || job.kind == NY_SPK_TEST ||
      job.kind == NY_SPK_GATE)
    {
      nxmutex_lock(&g_speaker_lock);
      reference_size = g_speaker_template_size;
      memcpy(reference, g_speaker_template, reference_size);
      nxmutex_unlock(&g_speaker_lock);
      if (!reference_size)
        {
          ret = -ENOENT;
          goto done;
        }
      ret = ny_speaker_rpc(link, NYAMP_SPK_TEMPLATE_PUT, reference,
                           reference_size, NULL, NULL);
      if (ret < 0)
        goto done;
    }
  if (job.kind == NY_SPK_PREPARE || job.kind == NY_SPK_TEST_PREPARE)
    {
      if (job.kind == NY_SPK_PREPARE)
        {
          ret =
              ny_speaker_rpc(link, NYAMP_SPK_ENROL_BEGIN, NULL, 0, NULL, NULL);
          if (ret < 0)
            goto done;
        }
      ret = ny_speaker_probe_start(&probe);
      if (ret < 0)
        goto done;
      uint64_t position = ny_voice_capture_position();
      nxmutex_lock(&g_speaker_lock);
      if (job.epoch == g_speaker_epoch && !g_speaker_stop)
        {
          g_speaker_probe = probe;
          probe = 0;
          g_speaker_min_sample = position;
          g_speaker_until = ny_product_time_ms(true) + 30000;
          g_speaker_phase = "listening";
        }
      else
        ret = -ECANCELED;
      nxmutex_unlock(&g_speaker_lock);
      goto done;
    }
  uint8_t range[NYAMP_SPK_RANGE_SIZE];
  nyamp_spk_put(range, job.stream, 8);
  nyamp_spk_put(range + 8, job.wake.start_sample, 8);
  nyamp_spk_put(range + 16, job.wake.end_sample, 8);
  bytes = sizeof(reply);
  ret = ny_speaker_rpc(
      link, job.kind == NY_SPK_TAKE ? NYAMP_SPK_ENROL_ADD : NYAMP_SPK_VERIFY,
      range, sizeof(range), reply, &bytes);
  if (ret < 0)
    goto done;
  if (job.kind == NY_SPK_TAKE)
    {
      if (bytes != 4 || nyamp_spk_get(reply, 4) > 8)
        {
          ret = -EPROTO;
          goto done;
        }
      uint32_t takes = nyamp_spk_get(reply, 4);
      nxmutex_lock(&g_speaker_lock);
      if (job.epoch == g_speaker_epoch)
        g_speaker_takes = takes;
      nxmutex_unlock(&g_speaker_lock);
      if (takes < NY_SPEAKER_TAKES)
        goto done;
      committing = true;
      bytes = sizeof(reply);
      ret =
          ny_speaker_rpc(link, NYAMP_SPK_ENROL_COMMIT, NULL, 0, reply, &bytes);
      if (ret < 0)
        goto done;
      if (bytes != 8 + NYAMP_SPK_TEMPLATE_SIZE ||
          !ny_speaker_template_valid(reply + 8, bytes - 8) ||
          !isfinite(nyamp_spk_float_get(reply + 4)))
        {
          ret = -EPROTO;
          goto done;
        }
      if (ny_speaker_cancelled(link))
        {
          ret = -ECANCELED;
          goto done;
        }
      nxmutex_lock(&g_speaker_lock);
      if (job.epoch != g_speaker_epoch || g_speaker_stop)
        ret = -ECANCELED;
      else
        {
          ret = ny_speaker_save(reply + 8, NYAMP_SPK_TEMPLATE_SIZE,
                                g_speaker_enabled);
          if (ret == 0)
            {
              g_speaker_kept = nyamp_spk_get(reply, 4);
              g_speaker_cohesion = nyamp_spk_float_get(reply + 4);
              g_speaker_phase = "saved";
            }
        }
      nxmutex_unlock(&g_speaker_lock);
      finish = true;
    }
  else
    {
      if (bytes != 8 || !isfinite(nyamp_spk_float_get(reply)) ||
          nyamp_spk_float_get(reply) < -1 || nyamp_spk_float_get(reply) > 1)
        {
          ret = -EPROTO;
          goto done;
        }
      nxmutex_lock(&g_speaker_lock);
      if (job.epoch != g_speaker_epoch || g_speaker_stop)
        ret = -ECANCELED;
      else
        {
          g_speaker_score = nyamp_spk_float_get(reply);
          g_speaker_matched = g_speaker_score >= NY_SPEAKER_THRESHOLD;
          g_speaker_phase = g_speaker_matched ? "matched" : "unknown";
          deliver = job.kind == NY_SPK_GATE && g_speaker_matched &&
                    g_speaker_enabled;
        }
      nxmutex_unlock(&g_speaker_lock);
      finish = job.kind == NY_SPK_TEST;
    }
done:
  if (probe)
    ny_speaker_probe_stop(probe);
  bool repeat = job.kind == NY_SPK_TAKE && !committing &&
                (ret == -EINVAL || ret == -ENOENT);
  uint32_t stop_probe = 0;
  nxmutex_lock(&g_speaker_lock);
  if (job.epoch == g_speaker_epoch)
    {
      g_speaker_error = ret;
      if (ret < 0 && !repeat)
        {
          g_speaker_phase = "error";
          finish = true;
        }
      else if (repeat || (ret == 0 && job.kind == NY_SPK_TAKE && !finish))
        g_speaker_phase = "listening";
      else if (ret == 0 &&
               (job.kind == NY_SPK_LOAD || job.kind == NY_SPK_FORGET))
        g_speaker_phase = "ready";
      if (finish)
        {
          g_speaker_enrolling = false;
          g_speaker_testing = false;
          stop_probe = g_speaker_probe;
          g_speaker_probe = 0;
        }
    }
  else
    deliver = false;
  nxmutex_unlock(&g_speaker_lock);
  if (stop_probe)
    ny_speaker_probe_stop(stop_probe);
  if (link)
    {
      if (link->port)
        ny_compute_port_close(link->port);
      free(link);
    }
  memset(reference, 0, sizeof(reference));
  nxmutex_lock(&g_speaker_lock);
  if (deliver && job.epoch == g_speaker_epoch && !g_speaker_stop &&
      ny_product_privacy_mic_allowed())
    ny_voice_accept_speaker_wake(&job.wake);
  g_speaker_busy = false;
  nxmutex_unlock(&g_speaker_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_product_speaker_on_wake
 ****************************************************************************/
bool ny_product_speaker_on_wake(uint64_t stream_id,
                                const struct nyamp_kws_detected_s *wake,
                                uint32_t probe_id)
{
  nxmutex_lock(&g_speaker_lock);
  if (probe_id && probe_id != g_speaker_probe)
    {
      nxmutex_unlock(&g_speaker_lock);
      return false;
    }
  if ((g_speaker_enrolling || g_speaker_testing) && g_speaker_probe &&
      probe_id != g_speaker_probe)
    {
      g_speaker_enrolling = g_speaker_testing = false;
      g_speaker_phase = "idle";
      g_speaker_probe = 0;
    }
  bool consume = g_speaker_enrolling || g_speaker_testing || g_speaker_enabled;
  if (!consume)
    {
      nxmutex_unlock(&g_speaker_lock);
      return false;
    }
  if (!g_speaker_busy && ny_product_privacy_mic_allowed())
    {
      if (!(wake->flags & NYAMP_KWS_DETECTED_HAS_OFFSETS) ||
          wake->end_sample <= wake->start_sample ||
          wake->end_sample - wake->start_sample < 8000)
        {
          g_speaker_error = -ENODATA;
          g_speaker_score = -2;
          g_speaker_matched = false;
          if (!g_speaker_enrolling && !g_speaker_testing)
            g_speaker_phase = "unknown";
        }
      else if ((!g_speaker_enrolling && !g_speaker_testing) ||
               (g_speaker_probe && wake->start_sample >= g_speaker_min_sample))
        ny_speaker_spawn(g_speaker_enrolling ? NY_SPK_TAKE
                         : g_speaker_testing ? NY_SPK_TEST
                                             : NY_SPK_GATE,
                         stream_id, wake);
    }
  nxmutex_unlock(&g_speaker_lock);
  return true;
}

/****************************************************************************
 * Name: ny_product_speaker_shutdown
 ****************************************************************************/
void ny_product_speaker_shutdown(void)
{
  nxmutex_lock(&g_speaker_lock);
  g_speaker_epoch++;
  g_speaker_stop = true;
  g_speaker_enrolling = g_speaker_testing = false;
  uint32_t id = g_speaker_probe;
  g_speaker_probe = 0;
  g_speaker_phase = "idle";
  nxmutex_unlock(&g_speaker_lock);
  ny_speaker_probe_stop(id);
  for (int i = 0; i < 500; i++)
    {
      nxmutex_lock(&g_speaker_lock);
      bool busy = g_speaker_busy;
      nxmutex_unlock(&g_speaker_lock);
      if (!busy)
        return;
      usleep(10000);
    }
}

/****************************************************************************
 * Name: ny_product_speaker_tick
 ****************************************************************************/
void ny_product_speaker_tick(void)
{
  uint32_t probe = 0;
  nxmutex_lock(&g_speaker_lock);
  if ((g_speaker_enrolling || g_speaker_testing) && g_speaker_until &&
      ny_product_time_ms(true) >= g_speaker_until)
    {
      g_speaker_epoch++;
      g_speaker_stop = true;
      g_speaker_enrolling = g_speaker_testing = false;
      probe = g_speaker_probe;
      g_speaker_probe = 0;
      g_speaker_phase = "error";
      g_speaker_error = -ETIMEDOUT;
    }
  nxmutex_unlock(&g_speaker_lock);
  ny_speaker_probe_stop(probe);
}

/****************************************************************************
 * Name: ny_product_speaker_request
 ****************************************************************************/
int ny_product_speaker_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result)
{
  if (strncmp(topic, "speaker.", 8))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  int ret = ny_product_speaker_initialize();
  if (ret < 0)
    return ret;
  bool status = !strcmp(topic, "speaker.status"),
       load = !strcmp(topic, "speaker.prepare");
  bool enroll = !strcmp(topic, "speaker.enroll.start"),
       test = !strcmp(topic, "speaker.test.start");
  bool cancel = !strcmp(topic, "speaker.cancel"),
       remove = !strcmp(topic, "speaker.delete"),
       configure = !strcmp(topic, "speaker.configure");
  if (!status && !load && !enroll && !test && !cancel && !remove && !configure)
    return -ENOSYS;
  if (cancel || remove)
    ny_product_speaker_shutdown();
  if ((enroll || test) && !ny_product_privacy_mic_allowed())
    return -EACCES;
  nxmutex_lock(&g_speaker_lock);
  if (!status && !cancel && g_speaker_busy)
    {
      ret = -EBUSY;
      goto out;
    }
  if (configure || remove)
    {
      const cJSON *revision =
          cJSON_GetObjectItemCaseSensitive(data, "revision");
      if (!cJSON_IsNumber(revision) ||
          revision->valuedouble != (double)g_speaker_revision)
        {
          ret = -ESTALE;
          goto out;
        }
      if (configure)
        {
          const cJSON *enabled =
              cJSON_GetObjectItemCaseSensitive(data, "enabled");
          if (!cJSON_IsBool(enabled))
            {
              ret = -EINVAL;
              goto out;
            }
          if (cJSON_IsTrue(enabled) && !g_speaker_template_size)
            {
              ret = -ENOENT;
              goto out;
            }
          if (g_speaker_enrolling || g_speaker_testing)
            {
              ret = -EBUSY;
              goto out;
            }
          ret = ny_speaker_save(g_speaker_template, g_speaker_template_size,
                                cJSON_IsTrue(enabled));
        }
      else
        {
          ret = ny_speaker_save(NULL, 0, false);
          if (ret == 0)
            ret = ny_speaker_spawn(NY_SPK_FORGET, 0, NULL);
        }
    }
  else if (load || enroll || test)
    {
      if (g_speaker_enrolling || g_speaker_testing)
        {
          ret = -EBUSY;
          goto out;
        }
      if (test && !g_speaker_template_size)
        {
          ret = -ENOENT;
          goto out;
        }
      g_speaker_epoch++;
      g_speaker_until = 0;
      g_speaker_takes = 0;
      g_speaker_score = -2;
      g_speaker_matched = false;
      g_speaker_enrolling = enroll;
      g_speaker_testing = test;
      ret = ny_speaker_spawn(enroll ? NY_SPK_PREPARE
                             : test ? NY_SPK_TEST_PREPARE
                                    : NY_SPK_LOAD,
                             0, NULL);
      if (ret < 0)
        g_speaker_enrolling = g_speaker_testing = false;
    }
out:
  nxmutex_unlock(&g_speaker_lock);
  if (ret < 0)
    return ret;
  *result = ny_speaker_status();
  return *result ? 0 : -ENOMEM;
}
