/****************************************************************************
 * app/nyabula_core/ny_product_vision.c
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

#include "ny_product_vision.h"
#include "ny_compute.h"
#include "ny_product_camera.h"
#include "ny_product_camera_gaze.h"
#include "ny_product_identity.h"
#include "ny_product_privacy.h"
#include "nyamp_vision_wire.h"
#ifdef CONFIG_NYABULA_CORE_VOICE
#include "ny_voice.h"
#endif
#include <arch/chip/rk3576_shmem.h>
#include <arch/chip/rk3576_shmem_layout.h>
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NY_VISION_STACK    16384
#define NY_VISION_MODEL    "face/detector"
#define NY_VISION_LOAD_MS  30000
#define NY_VISION_FRAME_MS 5000

static mutex_t g_vision_lock = NXMUTEX_INITIALIZER;
static bool g_vision_running;
static bool g_vision_stop;
static bool g_vision_follow;
static bool g_vision_identify;
static bool g_vision_tracking;
static float g_vision_box[4];
static int g_vision_error;
static int g_vision_follow_error;
static const char *g_vision_phase = "idle";
static uint64_t g_vision_frames;
static cJSON *g_vision_result;

static bool ny_vision_stopping(void);
static int ny_vision_call(struct ny_compute_port_s *port, uint16_t opcode,
                          const uint8_t *body, size_t size, uint8_t *reply,
                          size_t *reply_size, int timeout, bool cleanup);
static cJSON *ny_vision_decode(const uint8_t *reply, size_t size,
                               uint64_t sequence, uint64_t timestamp);
static void ny_vision_follow(cJSON *result);
static cJSON *ny_vision_track_decode(const uint8_t *reply, size_t size,
                                     uint64_t sequence, uint64_t timestamp);
static int ny_vision_identity_reply(const uint8_t *reply, size_t size,
                                    uint64_t sequence, uint64_t timestamp);
static int ny_vision_worker(int argc, char **argv);
static cJSON *ny_vision_status(void);

/****************************************************************************
 * Name: ny_vision_stopping
 ****************************************************************************/

static bool ny_vision_stopping(void)
{
  nxmutex_lock(&g_vision_lock);
  bool stop = g_vision_stop;
  nxmutex_unlock(&g_vision_lock);
  return stop || !ny_product_privacy_camera_allowed();
}

/****************************************************************************
 * Name: ny_vision_call
 ****************************************************************************/

static int ny_vision_call(struct ny_compute_port_s *port, uint16_t opcode,
                          const uint8_t *body, size_t size, uint8_t *reply,
                          size_t *reply_size, int timeout, bool cleanup)
{
  uint8_t frame[NYAMP_RPMSG_MTU];
  uint64_t id = ny_compute_request_id();
  uint32_t generation = ny_compute_generation();
  struct nyamp_header_s header = {
    .service = NYAMP_SERVICE_VISION,
    .opcode = opcode,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = id,
    .generation = generation,
    .payload_size = size,
  };
  size_t capacity = reply_size ? *reply_size : 0;
  if (reply_size)
    *reply_size = 0;
  if (size > NYAMP_INLINE_MAX ||
      nyamp_header_encode(frame, sizeof(frame), &header) != NYAMP_OK)
    return -EINVAL;
  if (size)
    memcpy(frame + NYAMP_WIRE_HEADER_SIZE, body, size);
  if (!cleanup && ny_vision_stopping())
    return -ECANCELED;
  int ret = ny_compute_port_send(port, frame, NYAMP_WIRE_HEADER_SIZE + size);
  if (ret < 0)
    return ret;
  uint64_t deadline = ny_product_time_ms(true) + timeout;
  while (ny_product_time_ms(true) < deadline)
    {
      if (!cleanup && ny_vision_stopping())
        return -ECANCELED;
      if (ny_compute_generation() != generation)
        return -ECONNRESET;
      ssize_t count = ny_compute_port_recv(port, frame, sizeof(frame), 100);
      if (count < 0)
        return (int)count;
      if (!count)
        continue;
      if (nyamp_header_decode(&header, frame, count) != NYAMP_OK ||
          count != NYAMP_WIRE_HEADER_SIZE + header.payload_size)
        return -EPROTO;
      if ((header.flags & NYAMP_FLAG_KIND_MASK) == NYAMP_FLAG_EVENT)
        continue;
      if (header.request_id != id || header.service != NYAMP_SERVICE_VISION ||
          header.opcode != opcode || header.generation != generation ||
          (header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_RESPONSE ||
          header.payload_size < 4)
        return -EPROTO;
      int32_t status =
          (int32_t)nyamp_vision_get(frame + NYAMP_WIRE_HEADER_SIZE, 4);
      if (status != NYAMP_MODEL_OK)
        return status == NYAMP_MODEL_BUSY               ? -EBUSY
               : status == NYAMP_MODEL_UNSUPPORTED      ? -ENOTSUP
               : status == NYAMP_MODEL_NOT_READY        ? -ENODEV
               : status == NYAMP_MODEL_INVALID          ? -EINVAL
               : status == NYAMP_MODEL_STALE_GENERATION ? -ECONNRESET
                                                        : -EIO;
      size_t bytes = header.payload_size - 4;
      if (bytes > capacity)
        return -EMSGSIZE;
      if (bytes && reply)
        memcpy(reply, frame + NYAMP_WIRE_HEADER_SIZE + 4, bytes);
      if (reply_size)
        *reply_size = bytes;
      return 0;
    }
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: ny_vision_decode
 ****************************************************************************/

static cJSON *ny_vision_decode(const uint8_t *reply, size_t size,
                               uint64_t sequence, uint64_t timestamp)
{
  if (size < NYAMP_VISION_RESULT_HEADER ||
      nyamp_vision_get(reply, 8) != sequence ||
      nyamp_vision_get(reply + 8, 8) != timestamp)
    return NULL;
  uint32_t count = nyamp_vision_get(reply + 16, 4);
  if (count > NYAMP_VISION_MAX_FACES ||
      size != NYAMP_VISION_RESULT_HEADER + count * NYAMP_VISION_FACE_SIZE)
    return NULL;
  cJSON *out = cJSON_CreateObject();
  cJSON *faces = out ? cJSON_AddArrayToObject(out, "faces") : NULL;
  if (!faces || !cJSON_AddNumberToObject(out, "sequence", sequence) ||
      !cJSON_AddNumberToObject(out, "capturedMs", timestamp))
    {
      cJSON_Delete(out);
      return NULL;
    }
  const char *names[] = { "x", "y", "width", "height", "confidence" };
  for (uint32_t i = 0; i < count; ++i)
    {
      const uint8_t *p =
          reply + NYAMP_VISION_RESULT_HEADER + i * NYAMP_VISION_FACE_SIZE;
      cJSON *face = cJSON_CreateObject();
      if (!face || !cJSON_AddItemToArray(faces, face))
        {
          cJSON_Delete(face);
          cJSON_Delete(out);
          return NULL;
        }
      for (unsigned int n = 0; n < 5; ++n)
        {
          float value = nyamp_vision_get_float(p + n * 4);
          if (!isfinite(value) || value < 0 || value > 1 ||
              !cJSON_AddNumberToObject(face, names[n], value))
            {
              cJSON_Delete(out);
              return NULL;
            }
        }
      cJSON *points = cJSON_AddArrayToObject(face, "landmarks");
      if (!points)
        {
          cJSON_Delete(out);
          return NULL;
        }
      for (unsigned int n = 0; n < 10; ++n)
        {
          float value = nyamp_vision_get_float(p + 20 + n * 4);
          cJSON *number = isfinite(value) ? cJSON_CreateNumber(value) : NULL;
          if (!number || !cJSON_AddItemToArray(points, number))
            {
              cJSON_Delete(number);
              cJSON_Delete(out);
              return NULL;
            }
        }
    }
  return out;
}

/****************************************************************************
 * Name: ny_vision_track_decode
 ****************************************************************************/

static cJSON *ny_vision_track_decode(const uint8_t *reply, size_t size,
                                     uint64_t sequence, uint64_t timestamp)
{
  if (size != NYAMP_VISION_TRACK_RESULT_SIZE ||
      nyamp_vision_get(reply, 8) != sequence ||
      nyamp_vision_get(reply + 8, 8) != timestamp)
    return NULL;
  unsigned int phase = nyamp_vision_get(reply + 16, 4),
               progress = nyamp_vision_get(reply + 20, 4),
               views = nyamp_vision_get(reply + 24, 4);
  if (phase < 1 || phase > 3 || progress > 100 || views < 1 || views > 4)
    return NULL;
  float geometry[5];
  for (unsigned int i = 0; i < 5; i++)
    {
      geometry[i] = nyamp_vision_get_float(reply + 28 + i * 4);
      if (!isfinite(geometry[i]) || geometry[i] < 0 || geometry[i] > 1)
        return NULL;
    }
  if (geometry[2] <= 0 || geometry[3] <= 0 ||
      geometry[0] + geometry[2] > 1.001f || geometry[1] + geometry[3] > 1.001f)
    return NULL;
  cJSON *out = cJSON_CreateObject();
  cJSON *target = out ? cJSON_AddObjectToObject(out, "target") : NULL;
  const char *names[] = { "x", "y", "width", "height", "confidence" };
  bool ok = target && cJSON_AddArrayToObject(out, "faces") &&
            cJSON_AddNumberToObject(out, "sequence", sequence) &&
            cJSON_AddNumberToObject(out, "capturedMs", timestamp) &&
            cJSON_AddNumberToObject(out, "progress", progress) &&
            cJSON_AddNumberToObject(out, "views", views) &&
            cJSON_AddStringToObject(out, "phase",
                                    phase == 1   ? "learning"
                                    : phase == 2 ? "tracking"
                                                 : "lost");
  for (unsigned int i = 0; ok && i < 5; i++)
    ok = cJSON_AddNumberToObject(target, names[i], geometry[i]) != NULL;
  if (!ok)
    {
      cJSON_Delete(out);
      return NULL;
    }
  return out;
}

/****************************************************************************
 * Name: ny_vision_follow
 ****************************************************************************/

static void ny_vision_follow(cJSON *result)
{
  nxmutex_lock(&g_vision_lock);
  bool follow = g_vision_follow;
  nxmutex_unlock(&g_vision_lock);
  if (!follow || ny_vision_stopping())
    return;
  const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(result, "capturedMs");
  if (!cJSON_IsNumber(stamp) ||
      ny_product_time_ms(true) - (uint64_t)stamp->valuedouble >= 700)
    return;
  cJSON *faces = cJSON_GetObjectItemCaseSensitive(result, "faces");
  cJSON *face = cJSON_GetArrayItem(faces, 0);
  cJSON *phase = cJSON_GetObjectItemCaseSensitive(result, "phase");
  if (cJSON_IsString(phase))
    {
      if (strcmp(phase->valuestring, "tracking"))
        return;
      face = cJSON_GetObjectItemCaseSensitive(result, "target");
    }
  if (!face)
    return;
  const struct ny_product_caller_s caller = { "vision", NY_PRODUCT_OWNER,
                                              true };
  cJSON *point = cJSON_CreateObject(), *out = NULL;
  if (!point)
    return;
  double x = cJSON_GetObjectItemCaseSensitive(face, "x")->valuedouble;
  double y = cJSON_GetObjectItemCaseSensitive(face, "y")->valuedouble;
  double w = cJSON_GetObjectItemCaseSensitive(face, "width")->valuedouble;
  double h = cJSON_GetObjectItemCaseSensitive(face, "height")->valuedouble;
  bool ok = cJSON_AddNumberToObject(point, "u", x + w / 2) &&
            cJSON_AddNumberToObject(point, "v", y + h / 2) &&
            cJSON_AddNumberToObject(point, "width", 640) &&
            cJSON_AddNumberToObject(point, "height", 480) &&
            cJSON_AddNumberToObject(point, "mode", 6);
  int ret =
      ok ? ny_product_camera_gaze_request(&caller, "camera.point", point, &out)
         : -ENOMEM;
  cJSON_Delete(point);
  cJSON_Delete(out);
  nxmutex_lock(&g_vision_lock);
  g_vision_follow_error = ret;
  nxmutex_unlock(&g_vision_lock);
}

/****************************************************************************
 * Name: ny_vision_identity_reply
 ****************************************************************************/
static int ny_vision_identity_reply(const uint8_t *reply, size_t size,
                                    uint64_t sequence, uint64_t timestamp)
{
  if (size < NYAMP_VISION_EMBED_HEADER ||
      nyamp_vision_get(reply, 8) != sequence ||
      nyamp_vision_get(reply + 8, 8) != timestamp)
    return -EPROTO;
  uint32_t faces = nyamp_vision_get(reply + 16, 4),
           index = nyamp_vision_get(reply + 20, 4);
  if (faces > NYAMP_VISION_MAX_FACES)
    return -EPROTO;
  if (index == UINT32_MAX)
    {
      if (size != NYAMP_VISION_EMBED_HEADER)
        return -EPROTO;
      ny_product_identity_observe(NULL, faces, timestamp);
      return 0;
    }
  if (index >= faces || size != NYAMP_VISION_EMBED_HEADER +
                                    NYAMP_VISION_FACE_SIZE +
                                    NY_IDENTITY_DIM * 2)
    return -EPROTO;
  for (unsigned int i = 0; i < 15; i++)
    {
      float value =
          nyamp_vision_get_float(reply + NYAMP_VISION_EMBED_HEADER + i * 4);
      if (!isfinite(value) || value < 0 || value > 1)
        return -EPROTO;
    }
  int16_t feature[NY_IDENTITY_DIM];
  double energy = 0;
  for (int i = 0; i < NY_IDENTITY_DIM; i++)
    {
      feature[i] = (int16_t)nyamp_vision_get(
          reply + NYAMP_VISION_EMBED_HEADER + NYAMP_VISION_FACE_SIZE + i * 2,
          2);
      energy += (double)feature[i] * feature[i];
    }
  if (energy < 800000000.0 || energy > 1300000000.0)
    return -EPROTO;
  ny_product_identity_observe(feature, faces, timestamp);
  memset(feature, 0, sizeof(feature));
  return 0;
}

/****************************************************************************
 * Name: ny_vision_worker
 ****************************************************************************/
static int ny_vision_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  struct ny_compute_port_s *port = NULL;
  struct ny_camera_client_s camera = { 0 };
  uint8_t *pixels = NULL, reply[NYAMP_INLINE_MAX];
  size_t bytes = sizeof(reply);
  struct nyamp_buffer_s grant;
  nxmutex_lock(&g_vision_lock);
  bool identify = g_vision_identify;
  bool tracking = g_vision_tracking;
  float box[4];
  memcpy(box, g_vision_box, sizeof(box));
  nxmutex_unlock(&g_vision_lock);
  int ret = ny_compute_port_open(&port);
  if (ret < 0)
    goto done;
  ret =
      ny_vision_call(port, NYAMP_VISION_END, NULL, 0, NULL, NULL, 1000, true);
  if (ret < 0)
    goto done;
  const char *model = tracking ? "vision/tracker" : NY_VISION_MODEL;
  ret = ny_vision_call(port,
                       tracking ? NYAMP_VISION_TRACK_LOAD : NYAMP_VISION_LOAD,
                       (const uint8_t *)model, strlen(model), NULL, NULL,
                       NY_VISION_LOAD_MS, false);
  if (ret < 0)
    goto done;
  if (identify)
    {
      const char *recognizer = "face/recognizer";
      ret = ny_vision_call(port, NYAMP_VISION_IDENTITY_LOAD,
                           (const uint8_t *)recognizer, strlen(recognizer),
                           NULL, NULL, 60000, false);
      if (ret < 0)
        goto done;
    }
  ret = ny_vision_call(port, NYAMP_VISION_BEGIN, NULL, 0, reply, &bytes, 3000,
                       false);
  if (ret < 0)
    goto done;
  if (nyamp_buffer_decode(&grant, reply, bytes) != NYAMP_OK ||
      grant.offset != NYAMP_SLOT_VISION ||
      grant.capacity != NYAMP_SLOT_VISION_SIZE ||
      grant.generation != ny_compute_generation() ||
      grant.format != NYAMP_FORMAT_BYTES ||
      grant.flags != NYAMP_BUFFER_IN_SHMEM ||
      (!rk3576_shmem_ready() && rk3576_shmem_initialize() < 0))
    {
      ret = -EPROTO;
      goto done;
    }
  pixels = malloc(NYAMP_VISION_BYTES);
  if (!pixels)
    {
      ret = -ENOMEM;
      goto done;
    }
  ret = ny_product_camera_subscribe(&camera);
  if (ret < 0)
    goto done;
  uint64_t sequence = 0, timestamp = 0, next_identity = 0;
  unsigned int faces = 0, face_index = 0;
  bool initialized = false;
  bool announced = false;
  nxmutex_lock(&g_vision_lock);
  g_vision_phase = tracking ? "learning" : "detecting";
  nxmutex_unlock(&g_vision_lock);
  while (!ny_vision_stopping())
    {
      ret = ny_product_camera_raw_next(&camera, pixels, NYAMP_VISION_BYTES,
                                       &sequence, &timestamp);
      if (ret == -EAGAIN)
        {
          usleep(10000);
          continue;
        }
      if (ret < 0)
        break;
      if (tracking && !initialized && sequence < 32)
        continue;
      uint8_t body[NYAMP_VISION_TRACK_INIT_SIZE];
      grant.length = NYAMP_VISION_BYTES;
      bytes = 0;
      if (nyamp_buffer_encode(body, sizeof(body), &bytes, &grant) != NYAMP_OK)
        {
          ret = -EPROTO;
          break;
        }
      memcpy((uint8_t *)rk3576_shmem_base() + grant.offset, pixels,
             NYAMP_VISION_BYTES);
      __sync_synchronize();
      nyamp_vision_put(body + NYAMP_BUFFER_SIZE, sequence, 8);
      nyamp_vision_put(body + NYAMP_BUFFER_SIZE + 8, timestamp, 8);
      bool embed = identify && faces && timestamp >= next_identity;
      size_t request_size = NYAMP_VISION_FRAME_SIZE;
      if (embed)
        {
          nyamp_vision_put(body + NYAMP_VISION_FRAME_SIZE,
                           face_index++ % faces, 4);
          request_size = NYAMP_VISION_EMBED_SIZE;
        }
      uint16_t opcode = embed ? NYAMP_VISION_EMBED : NYAMP_VISION_FRAME;
      if (tracking)
        {
          opcode =
              initialized ? NYAMP_VISION_TRACK_FRAME : NYAMP_VISION_TRACK_INIT;
          if (!initialized)
            {
              for (unsigned int i = 0; i < 4; i++)
                nyamp_vision_put_float(body + NYAMP_VISION_FRAME_SIZE + i * 4,
                                       box[i]);
              request_size = NYAMP_VISION_TRACK_INIT_SIZE;
            }
        }
      bytes = sizeof(reply);
      ret = ny_vision_call(port, opcode, body, request_size, reply, &bytes,
                           NY_VISION_FRAME_MS, false);
      if (ret < 0 || ny_vision_stopping())
        break;
      if (embed)
        {
          ret = ny_vision_identity_reply(reply, bytes, sequence, timestamp);
          if (ret < 0)
            break;
          next_identity = timestamp + 400;
        }
      else
        {
          cJSON *result =
              tracking
                  ? ny_vision_track_decode(reply, bytes, sequence, timestamp)
                  : ny_vision_decode(reply, bytes, sequence, timestamp);
          if (!result)
            {
              ret = -EPROTO;
              break;
            }
          faces = cJSON_GetArraySize(
              cJSON_GetObjectItemCaseSensitive(result, "faces"));
          if (identify)
            ny_product_identity_faces(faces, timestamp);
          ny_vision_follow(result);
#ifdef CONFIG_NYABULA_CORE_VOICE
          const cJSON *track_phase =
              cJSON_GetObjectItemCaseSensitive(result, "phase");
          if (tracking && !announced && cJSON_IsString(track_phase) &&
              !strcmp(track_phase->valuestring, "tracking"))
            {
              const struct ny_product_caller_s caller = { "vision",
                                                          NY_PRODUCT_OWNER,
                                                          true };
              cJSON *say = cJSON_CreateObject(), *spoken = NULL;
              if (say && cJSON_AddStringToObject(
                             say, "text", "记住啦，可以移动这个物品了。"))
                {
                  int said =
                      ny_voice_request(&caller, "voice.say", say, &spoken);
                  announced = said != -EBUSY;
                }
              cJSON_Delete(say);
              cJSON_Delete(spoken);
            }
#else
          (void)announced;
#endif
          nxmutex_lock(&g_vision_lock);
          cJSON_Delete(g_vision_result);
          g_vision_result = result;
          if (tracking)
            {
              const char *phase =
                  cJSON_GetObjectItemCaseSensitive(result, "phase")
                      ->valuestring;
              g_vision_phase = !strcmp(phase, "tracking") ? "tracking"
                               : !strcmp(phase, "lost")   ? "lost"
                                                          : "learning";
              initialized = true;
            }
          nxmutex_unlock(&g_vision_lock);
        }
      nxmutex_lock(&g_vision_lock);
      g_vision_frames++;
      nxmutex_unlock(&g_vision_lock);
      usleep(50000);
    }
done:
  ny_product_camera_close(&camera);
  free(pixels);
  if (port)
    {
      ny_vision_call(port, NYAMP_VISION_END, NULL, 0, NULL, NULL, 1000, true);
      ny_compute_port_close(port);
    }
  ny_product_identity_clear_live();
  nxmutex_lock(&g_vision_lock);
  if (g_vision_stop || !ny_product_privacy_camera_allowed())
    ret = 0;
  g_vision_error = ret;
  g_vision_phase = ret < 0 ? "error" : "idle";
  cJSON_Delete(g_vision_result);
  g_vision_result = NULL;
  g_vision_running = false;
  nxmutex_unlock(&g_vision_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_vision_status
 ****************************************************************************/

static cJSON *ny_vision_status(void)
{
  cJSON *out = cJSON_CreateObject();
  if (!out)
    return NULL;
  nxmutex_lock(&g_vision_lock);
  bool ok = cJSON_AddBoolToObject(out, "running", g_vision_running) &&
            cJSON_AddStringToObject(out, "phase", g_vision_phase) &&
            cJSON_AddBoolToObject(out, "following", g_vision_follow) &&
            cJSON_AddBoolToObject(out, "identifying", g_vision_identify) &&
            cJSON_AddBoolToObject(out, "tracking", g_vision_tracking) &&
            cJSON_AddNumberToObject(out, "frames", g_vision_frames) &&
            cJSON_AddNumberToObject(out, "lastError", g_vision_error) &&
            cJSON_AddNumberToObject(out, "followError", g_vision_follow_error);
  if (ok && g_vision_result)
    {
      cJSON *result = cJSON_Duplicate(g_vision_result, true);
      if (!result)
        ok = false;
      else if (!cJSON_AddItemToObject(out, "result", result))
        {
          cJSON_Delete(result);
          ok = false;
        }
      if (ok)
        {
          const cJSON *stamp =
              cJSON_GetObjectItemCaseSensitive(g_vision_result, "capturedMs");
          ok = cJSON_AddNumberToObject(out, "ageMs",
                                       ny_product_time_ms(true) -
                                           (uint64_t)stamp->valuedouble) !=
               NULL;
        }
    }
  nxmutex_unlock(&g_vision_lock);
  if (!ok)
    {
      cJSON_Delete(out);
      return NULL;
    }
  return out;
}

/****************************************************************************
 * Name: ny_product_vision_shutdown
 ****************************************************************************/

void ny_product_vision_shutdown(void)
{
  nxmutex_lock(&g_vision_lock);
  g_vision_stop = true;
  nxmutex_unlock(&g_vision_lock);
  for (int n = 0; n < 500; n++)
    {
      nxmutex_lock(&g_vision_lock);
      bool running = g_vision_running;
      nxmutex_unlock(&g_vision_lock);
      if (!running)
        return;
      usleep(10000);
    }
}

/****************************************************************************
 * Name: ny_product_vision_request
 ****************************************************************************/

int ny_product_vision_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result)
{
  bool learn = !strcmp(topic, "vision.learn");
  bool start = !strcmp(topic, "vision.start") || learn,
       stop = !strcmp(topic, "vision.stop");
  if (!start && !stop && strcmp(topic, "vision.status"))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
#ifndef CONFIG_NYABULA_CORE_OBJECT_TRACKING
  if (learn)
    return -ENOTSUP;
#endif
  if (stop)
    ny_product_vision_shutdown();
  if (start)
    {
      if (!ny_product_privacy_camera_allowed())
        return -EACCES;
      const cJSON *follow = cJSON_GetObjectItemCaseSensitive(data, "follow");
      const cJSON *identify =
          cJSON_GetObjectItemCaseSensitive(data, "identify");
      if ((follow && !cJSON_IsBool(follow)) ||
          (identify && !cJSON_IsBool(identify)))
        return -EINVAL;
      float box[4] = { .375f, 1.0f / 3, .25f, 1.0f / 3 };
      if (learn)
        {
          const char *names[] = { "x", "y", "width", "height" };
          for (unsigned int i = 0; i < 4; i++)
            {
              const cJSON *v =
                  cJSON_GetObjectItemCaseSensitive(data, names[i]);
              if (v)
                {
                  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
                      v->valuedouble < 0 || v->valuedouble > 1)
                    return -EINVAL;
                  box[i] = v->valuedouble;
                }
            }
          if (box[2] * 640 < 12 || box[3] * 480 < 12 || box[0] + box[2] > 1 ||
              box[1] + box[3] > 1)
            return -EINVAL;
        }
      nxmutex_lock(&g_vision_lock);
      if (g_vision_running)
        {
          nxmutex_unlock(&g_vision_lock);
          return -EBUSY;
        }
      g_vision_stop = false;
      g_vision_follow = follow ? cJSON_IsTrue(follow) : learn;
      g_vision_identify = !learn && cJSON_IsTrue(identify);
      g_vision_tracking = learn;
      memcpy(g_vision_box, box, sizeof(box));
      g_vision_error = g_vision_follow_error = 0;
      g_vision_frames = 0;
      g_vision_running = true;
      g_vision_phase = "loading";
      int ret = task_create("ny_vision", 100, NY_VISION_STACK,
                            ny_vision_worker, NULL);
      if (ret < 0)
        {
          g_vision_running = false;
          g_vision_error = -errno;
          g_vision_phase = "error";
        }
      nxmutex_unlock(&g_vision_lock);
      if (ret < 0)
        return -errno;
    }
  *result = ny_vision_status();
  return *result ? 0 : -ENOMEM;
}

/****************************************************************************
 * Name: ny_product_vision_identifying
 ****************************************************************************/
bool ny_product_vision_identifying(void)
{
  nxmutex_lock(&g_vision_lock);
  bool active = g_vision_running && !g_vision_stop && g_vision_identify &&
                !strcmp(g_vision_phase, "detecting");
  nxmutex_unlock(&g_vision_lock);
  return active;
}
