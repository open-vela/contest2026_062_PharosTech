/****************************************************************************
 * app/nyabula_core/ny_product_camera.c
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

#include "ny_product_camera.h"
#include "camenc_capture.h"
#include "ny_camera_png.h"
#include "ny_product_privacy.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <nuttx/mutex.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NY_CAMERA_WIDTH               640
#define NY_CAMERA_HEIGHT              480
#define NY_CAMERA_MODE                6
#define NY_CAMERA_RAW_SIZE            (NY_CAMERA_WIDTH * NY_CAMERA_HEIGHT * 3 / 2)
#define NY_CAMERA_STACK               16384
#define NY_CAMERA_SEGMENTS            64
#define NY_CAMERA_SEGMENT_MAX         (512 * 1024)
#define NY_CAMERA_BUFFER_MAX          (2 * 1024 * 1024)
#define NY_CAMERA_INIT_MAX            4096
#define NY_CAMERA_CHUNK               12288
#define NY_CAMERA_PHOTO_WAIT_MS       8000
#define NY_CAMERA_PHOTO_WARMUP_FRAMES 32
#define NY_CAMERA_PHOTO_TTL_MS        600000

struct ny_camera_segment_s
{
  uint8_t *data;
  size_t length;
  bool key;
};

static mutex_t g_camera_request_lock = NXMUTEX_INITIALIZER;
static mutex_t g_camera_lock = NXMUTEX_INITIALIZER;
static bool g_camera_running;
static bool g_camera_stopping;
static unsigned int g_camera_readers;
static uint32_t g_camera_generation;
static int g_camera_error;
static uint64_t g_camera_first = 1;
static uint64_t g_camera_last;
static size_t g_camera_bytes;
static struct ny_camera_segment_s g_camera_segments[NY_CAMERA_SEGMENTS];
static uint8_t g_camera_init[NY_CAMERA_INIT_MAX];
static size_t g_camera_init_size;
static uint8_t *g_camera_raw;
static bool g_camera_raw_pending;
static uint64_t g_camera_raw_sequence;
static uint64_t g_camera_raw_timestamp;
static uint64_t g_camera_frames;
static uint64_t g_camera_last_frame;
static uint8_t *g_camera_photo;
static size_t g_camera_photo_size;
static uint64_t g_camera_photo_id;
static uint64_t g_camera_photo_at;

static void ny_camera_clear(void);
static void ny_camera_evict(void);
static bool ny_camera_stop(void *arg);
static int ny_camera_emit(void *arg, enum camenc_seg_e seg,
                          const uint8_t *data, size_t length, bool key);
static int ny_camera_frame(void *arg, const uint8_t *nv12, size_t size,
                           uint32_t width, uint32_t height, uint32_t stride);
static int ny_camera_worker(int argc, char **argv);
static int ny_camera_acquire(void);
static void ny_camera_release(void);
static cJSON *ny_camera_status(void);
static int ny_camera_chunk(const uint8_t *bytes, size_t total, size_t offset,
                           cJSON **result);
static int ny_camera_number(const cJSON *data, const char *key,
                            uint64_t maximum, uint64_t *value);

/****************************************************************************
 * Name: ny_camera_evict
 ****************************************************************************/

static void ny_camera_evict(void)
{
  struct ny_camera_segment_s *s =
      &g_camera_segments[(g_camera_first - 1) % NY_CAMERA_SEGMENTS];
  g_camera_bytes -= s->length;
  free(s->data);
  memset(s, 0, sizeof(*s));
  g_camera_first++;
}

/****************************************************************************
 * Name: ny_camera_clear
 ****************************************************************************/

static void ny_camera_clear(void)
{
  while (g_camera_first <= g_camera_last)
    ny_camera_evict();
  g_camera_first = 1;
  g_camera_last = 0;
  g_camera_init_size = 0;
  free(g_camera_raw);
  g_camera_raw = NULL;
  g_camera_raw_pending = false;
  g_camera_raw_sequence = 0;
}

/****************************************************************************
 * Name: ny_camera_stop
 ****************************************************************************/

static bool ny_camera_stop(void *arg)
{
  (void)arg;
  nxmutex_lock(&g_camera_lock);
  bool stop = g_camera_stopping || g_camera_readers == 0;
  nxmutex_unlock(&g_camera_lock);
  return stop;
}

/****************************************************************************
 * Name: ny_camera_emit
 ****************************************************************************/

static int ny_camera_emit(void *arg, enum camenc_seg_e seg,
                          const uint8_t *data, size_t length, bool key)
{
  (void)arg;
  if (length == 0 || length > NY_CAMERA_SEGMENT_MAX ||
      (seg == CAMENC_SEG_INIT && length > NY_CAMERA_INIT_MAX))
    return -EFBIG;
  uint8_t *copy = NULL;
  if (seg == CAMENC_SEG_FRAGMENT)
    {
      copy = malloc(length);
      if (copy == NULL)
        return -ENOMEM;
      memcpy(copy, data, length);
    }
  nxmutex_lock(&g_camera_lock);
  if (seg == CAMENC_SEG_INIT)
    {
      memcpy(g_camera_init, data, length);
      g_camera_init_size = length;
    }
  else
    {
      while (g_camera_first <= g_camera_last &&
             (g_camera_last - g_camera_first + 1 >= NY_CAMERA_SEGMENTS ||
              g_camera_bytes + length > NY_CAMERA_BUFFER_MAX))
        ny_camera_evict();
      struct ny_camera_segment_s *s =
          &g_camera_segments[g_camera_last++ % NY_CAMERA_SEGMENTS];
      s->data = copy;
      s->length = length;
      s->key = key;
      g_camera_bytes += length;
    }
  nxmutex_unlock(&g_camera_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_camera_frame
 ****************************************************************************/

static int ny_camera_frame(void *arg, const uint8_t *nv12, size_t size,
                           uint32_t width, uint32_t height, uint32_t stride)
{
  (void)arg;
  if (width != NY_CAMERA_WIDTH || height != NY_CAMERA_HEIGHT ||
      stride != NY_CAMERA_WIDTH || size < NY_CAMERA_RAW_SIZE)
    return -EINVAL;
  nxmutex_lock(&g_camera_lock);
  g_camera_frames++;
  g_camera_last_frame = ny_product_time_ms(true);
  /* Stream startup precedes sensor exposure convergence. Keep preview live,
   * but take a still only after the measured startup transient has passed.
   */
  if (g_camera_raw_pending && g_camera_frames >= NY_CAMERA_PHOTO_WARMUP_FRAMES)
    {
      memcpy(g_camera_raw, nv12, NY_CAMERA_RAW_SIZE);
      g_camera_raw_sequence++;
      g_camera_raw_timestamp = g_camera_last_frame;
      g_camera_raw_pending = false;
    }
  nxmutex_unlock(&g_camera_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_camera_worker
 ****************************************************************************/

static int ny_camera_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  const struct camenc_capture_s capture = { .mode = NY_CAMERA_MODE,
                                            .frames = INT_MAX,
                                            .qp = 28,
                                            .gop = 30,
                                            .emit = ny_camera_emit,
                                            .frame = ny_camera_frame,
                                            .stop = ny_camera_stop };
  int ret = camenc_capture_run(&capture);
  nxmutex_lock(&g_camera_lock);
  g_camera_running = false;
  g_camera_error = ret;
  ny_camera_clear();
  nxmutex_unlock(&g_camera_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_camera_acquire
 ****************************************************************************/

static int ny_camera_acquire(void)
{
  int ret = 0;
  nxmutex_lock(&g_camera_request_lock);
  if (!ny_product_privacy_camera_allowed())
    {
      nxmutex_unlock(&g_camera_request_lock);
      return -EACCES;
    }
  nxmutex_lock(&g_camera_lock);
  if (g_camera_running)
    {
      if (g_camera_stopping)
        ret = -EBUSY;
      else
        g_camera_readers++;
      nxmutex_unlock(&g_camera_lock);
      nxmutex_unlock(&g_camera_request_lock);
      return ret;
    }
  nxmutex_unlock(&g_camera_lock);
  nxmutex_lock(&g_camera_lock);
  ny_camera_clear();
  g_camera_raw = malloc(NY_CAMERA_RAW_SIZE);
  if (g_camera_raw == NULL)
    ret = -ENOMEM;
  else
    {
      g_camera_stopping = false;
      g_camera_running = true;
      g_camera_error = 0;
      g_camera_frames = 0;
      g_camera_last_frame = 0;
      g_camera_generation++;
      g_camera_readers++;
      /* The service outlives a CLI request. A separate task group owns the
       * capture descriptors and cleanup even after that caller exits.
       */
      ret = task_create("ny_camera", CONFIG_EXAMPLES_CAMENC_PRIORITY,
                        NY_CAMERA_STACK, ny_camera_worker, NULL);
      if (ret < 0)
        {
          ret = -errno;
          g_camera_running = false;
          g_camera_readers--;
          ny_camera_clear();
          g_camera_error = ret;
        }
      else
        ret = 0;
    }
  nxmutex_unlock(&g_camera_lock);
  nxmutex_unlock(&g_camera_request_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_camera_release
 ****************************************************************************/

static void ny_camera_release(void)
{
  nxmutex_lock(&g_camera_lock);
  if (g_camera_readers > 0)
    g_camera_readers--;
  if (g_camera_readers == 0)
    g_camera_stopping = true;
  nxmutex_unlock(&g_camera_lock);
}

/****************************************************************************
 * Name: ny_product_camera_subscribe
 ****************************************************************************/

int ny_product_camera_subscribe(struct ny_camera_client_s *client)
{
  if (client->active)
    return 0;
  int ret = ny_camera_acquire();
  if (ret == 0)
    {
      memset(client, 0, sizeof(*client));
      client->active = true;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_product_camera_close
 ****************************************************************************/

void ny_product_camera_close(struct ny_camera_client_s *client)
{
  if (client->active)
    {
      ny_camera_release();
      memset(client, 0, sizeof(*client));
    }
}

/****************************************************************************
 * Name: ny_camera_chunk
 ****************************************************************************/

static int ny_camera_chunk(const uint8_t *bytes, size_t total, size_t offset,
                           cJSON **result)
{
  if (offset >= total)
    return -ERANGE;
  size_t count = total - offset;
  if (count > NY_CAMERA_CHUNK)
    count = NY_CAMERA_CHUNK;
  size_t size = (count + 2) / 3 * 4 + 1;
  unsigned char *encoded = malloc(size);
  size_t length;
  if (encoded == NULL)
    return -ENOMEM;
  int ret =
      mbedtls_base64_encode(encoded, size, &length, bytes + offset, count);
  cJSON *out = ret == 0 ? cJSON_CreateObject() : NULL;
  bool ok = out != NULL &&
            cJSON_AddNumberToObject(out, "offset", offset) != NULL &&
            cJSON_AddNumberToObject(out, "total", total) != NULL &&
            cJSON_AddStringToObject(out, "data", (char *)encoded) != NULL;
  free(encoded);
  if (!ok)
    {
      cJSON_Delete(out);
      return -ENOMEM;
    }
  *result = out;
  return (int)count;
}

/****************************************************************************
 * Name: ny_product_camera_next
 ****************************************************************************/

int ny_product_camera_next(struct ny_camera_client_s *client, cJSON **chunk)
{
  *chunk = NULL;
  if (!ny_product_privacy_camera_allowed())
    return -EACCES;
  if (!client->active)
    return -ENODATA;
  int ret = -EAGAIN;
  nxmutex_lock(&g_camera_lock);
  if (!g_camera_running || g_camera_stopping)
    {
      ret = g_camera_error != 0 ? g_camera_error : -EPIPE;
      goto out;
    }
  if (g_camera_init_size == 0)
    goto out;
  if (client->generation != g_camera_generation ||
      client->sequence < g_camera_first)
    {
      uint64_t key = 0;
      for (uint64_t seq = g_camera_last; seq >= g_camera_first; seq--)
        if (g_camera_segments[(seq - 1) % NY_CAMERA_SEGMENTS].key)
          {
            key = seq;
            break;
          }
      if (key == 0)
        goto out;
      ret = ny_camera_chunk(g_camera_init, g_camera_init_size, 0, chunk);
      if (ret > 0)
        {
          if (cJSON_AddStringToObject(*chunk, "kind", "init") == NULL ||
              cJSON_AddNumberToObject(*chunk, "generation",
                                      g_camera_generation) == NULL ||
              cJSON_AddNumberToObject(*chunk, "sequence", 0) == NULL)
            ret = -ENOMEM;
          else
            {
              client->generation = g_camera_generation;
              client->sequence = key;
              client->offset = 0;
              ret = 0;
            }
        }
      goto out;
    }
  if (client->sequence > g_camera_last)
    goto out;
  struct ny_camera_segment_s *s =
      &g_camera_segments[(client->sequence - 1) % NY_CAMERA_SEGMENTS];
  ret = ny_camera_chunk(s->data, s->length, client->offset, chunk);
  if (ret > 0)
    {
      if (cJSON_AddStringToObject(*chunk, "kind", "fragment") == NULL ||
          cJSON_AddNumberToObject(*chunk, "generation", g_camera_generation) ==
              NULL ||
          cJSON_AddNumberToObject(*chunk, "sequence", client->sequence) ==
              NULL ||
          cJSON_AddBoolToObject(*chunk, "key", s->key) == NULL)
        ret = -ENOMEM;
      else
        {
          client->offset += ret;
          if (client->offset == s->length)
            {
              client->sequence++;
              client->offset = 0;
            }
          ret = 0;
        }
    }
out:
  nxmutex_unlock(&g_camera_lock);
  if (ret < 0)
    {
      cJSON_Delete(*chunk);
      *chunk = NULL;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_product_camera_photo
 ****************************************************************************/

int ny_product_camera_photo(unsigned int factor, uint8_t **png, size_t *length)
{
  *png = NULL;
  *length = 0;
  uint8_t *raw = malloc(NY_CAMERA_RAW_SIZE);
  if (raw == NULL)
    return -ENOMEM;
  int ret = ny_camera_acquire();
  if (ret < 0)
    {
      free(raw);
      return ret;
    }
  nxmutex_lock(&g_camera_lock);
  uint64_t before = g_camera_raw_sequence;
  uint32_t generation = g_camera_generation;
  g_camera_raw_pending = true;
  nxmutex_unlock(&g_camera_lock);
  uint64_t deadline = ny_product_time_ms(true) + NY_CAMERA_PHOTO_WAIT_MS;
  for (;;)
    {
      nxmutex_lock(&g_camera_lock);
      if (!g_camera_running || g_camera_stopping ||
          generation != g_camera_generation)
        ret = g_camera_error != 0 ? g_camera_error : -ECANCELED;
      else if (g_camera_raw_sequence > before)
        {
          memcpy(raw, g_camera_raw, NY_CAMERA_RAW_SIZE);
          ret = 0;
        }
      else
        ret = -EAGAIN;
      nxmutex_unlock(&g_camera_lock);
      if (ret != -EAGAIN)
        break;
      if (ny_product_time_ms(true) >= deadline)
        {
          ret = -ETIMEDOUT;
          break;
        }
      usleep(10000);
    }
  ny_camera_release();
  if (ret == 0)
    ret =
        ny_camera_png(raw, NY_CAMERA_RAW_SIZE, NY_CAMERA_WIDTH,
                      NY_CAMERA_HEIGHT, NY_CAMERA_WIDTH, factor, png, length);
  free(raw);
  if (ret == 0 && !ny_product_privacy_camera_allowed())
    {
      free(*png);
      *png = NULL;
      *length = 0;
      ret = -EACCES;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_camera_status
 ****************************************************************************/

static cJSON *ny_camera_status(void)
{
  cJSON *out = cJSON_CreateObject();
  if (out == NULL)
    return NULL;
  nxmutex_lock(&g_camera_lock);
  bool ok =
      cJSON_AddBoolToObject(out, "available",
                            access("/dev/video0", F_OK) == 0 &&
                                access("/dev/video1", F_OK) == 0) != NULL;
  ok &= cJSON_AddBoolToObject(out, "running", g_camera_running) != NULL;
  ok &= cJSON_AddStringToObject(
            out, "phase",
            g_camera_stopping && g_camera_running ? "stopping"
            : g_camera_running ? (g_camera_frames ? "streaming" : "starting")
            : g_camera_error   ? "error"
                               : "idle") != NULL;
  ok &= cJSON_AddNumberToObject(out, "width", NY_CAMERA_WIDTH) != NULL;
  ok &= cJSON_AddNumberToObject(out, "height", NY_CAMERA_HEIGHT) != NULL;
  ok &= cJSON_AddNumberToObject(out, "frames", g_camera_frames) != NULL;
  ok &=
      cJSON_AddNumberToObject(out, "lastFrameMs", g_camera_last_frame) != NULL;
  ok &= cJSON_AddNumberToObject(out, "readers", g_camera_readers) != NULL;
  ok &= cJSON_AddNumberToObject(out, "bufferBytes", g_camera_bytes) != NULL;
  ok &= cJSON_AddNumberToObject(out, "lastError", g_camera_error) != NULL;
  nxmutex_unlock(&g_camera_lock);
  if (!ok)
    {
      cJSON_Delete(out);
      return NULL;
    }
  return out;
}

/****************************************************************************
 * Name: ny_camera_number
 ****************************************************************************/

static int ny_camera_number(const cJSON *data, const char *key,
                            uint64_t maximum, uint64_t *value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
      item->valuedouble < 0 || item->valuedouble > maximum ||
      floor(item->valuedouble) != item->valuedouble)
    return -EINVAL;
  *value = item->valuedouble;
  return 0;
}

/****************************************************************************
 * Name: ny_product_camera_request
 ****************************************************************************/

int ny_product_camera_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result)
{
  if (strcmp(topic, "camera.status") && strcmp(topic, "camera.stop") &&
      strcmp(topic, "camera.photo.capture") &&
      strcmp(topic, "camera.photo.read") &&
      strcmp(topic, "camera.photo.clear"))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  if (!strcmp(topic, "camera.stop"))
    {
      nxmutex_lock(&g_camera_lock);
      g_camera_stopping = true;
      nxmutex_unlock(&g_camera_lock);
    }
  if (!strcmp(topic, "camera.status") || !strcmp(topic, "camera.stop"))
    {
      *result = ny_camera_status();
      return *result == NULL ? -ENOMEM : 0;
    }
  if (strcmp(topic, "camera.photo.clear") &&
      !ny_product_privacy_camera_allowed())
    return -EACCES;
  if (!strcmp(topic, "camera.photo.capture"))
    {
      uint8_t *png;
      size_t length;
      int ret = ny_product_camera_photo(1, &png, &length);
      if (ret < 0)
        return ret;
      cJSON *out = cJSON_CreateObject();
      nxmutex_lock(&g_camera_lock);
      if (!ny_product_privacy_camera_allowed())
        {
          nxmutex_unlock(&g_camera_lock);
          free(png);
          cJSON_Delete(out);
          return -EACCES;
        }
      free(g_camera_photo);
      g_camera_photo = png;
      g_camera_photo_size = length;
      g_camera_photo_id++;
      g_camera_photo_at = ny_product_time_ms(true);
      bool ok =
          out != NULL &&
          cJSON_AddNumberToObject(out, "photoId", g_camera_photo_id) != NULL &&
          cJSON_AddNumberToObject(out, "bytes", length) != NULL &&
          cJSON_AddNumberToObject(out, "width", NY_CAMERA_WIDTH) != NULL &&
          cJSON_AddNumberToObject(out, "height", NY_CAMERA_HEIGHT) != NULL &&
          cJSON_AddStringToObject(out, "mime", "image/png") != NULL;
      nxmutex_unlock(&g_camera_lock);
      if (!ok)
        {
          cJSON_Delete(out);
          return -ENOMEM;
        }
      *result = out;
      return 0;
    }
  nxmutex_lock(&g_camera_lock);
  if (g_camera_photo != NULL &&
      ny_product_time_ms(true) - g_camera_photo_at > NY_CAMERA_PHOTO_TTL_MS)
    {
      free(g_camera_photo);
      g_camera_photo = NULL;
      g_camera_photo_size = 0;
    }
  int ret;
  if (!strcmp(topic, "camera.photo.clear"))
    {
      free(g_camera_photo);
      g_camera_photo = NULL;
      g_camera_photo_size = 0;
      *result = cJSON_CreateObject();
      ret = *result == NULL ? -ENOMEM : 0;
    }
  else
    {
      uint64_t id;
      uint64_t offset;
      ret = ny_camera_number(data, "photoId", 9007199254740991ULL, &id);
      if (ret == 0)
        ret = ny_camera_number(data, "offset", NY_CAMERA_SEGMENT_MAX * 2,
                               &offset);
      if (ret == 0 && (g_camera_photo == NULL || id != g_camera_photo_id))
        ret = -ENOENT;
      if (ret == 0)
        {
          ret = ny_camera_chunk(g_camera_photo, g_camera_photo_size, offset,
                                result);
          if (ret > 0)
            ret = 0;
        }
    }
  nxmutex_unlock(&g_camera_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_camera_shutdown
 ****************************************************************************/

void ny_product_camera_shutdown(void)
{
  nxmutex_lock(&g_camera_request_lock);
  nxmutex_lock(&g_camera_lock);
  g_camera_stopping = true;
  nxmutex_unlock(&g_camera_lock);
  for (;;)
    {
      nxmutex_lock(&g_camera_lock);
      bool running = g_camera_running;
      nxmutex_unlock(&g_camera_lock);
      if (!running)
        break;
      usleep(10000);
    }
  nxmutex_lock(&g_camera_lock);
  ny_camera_clear();
  free(g_camera_photo);
  g_camera_photo = NULL;
  g_camera_photo_size = 0;
  nxmutex_unlock(&g_camera_lock);
  nxmutex_unlock(&g_camera_request_lock);
}

/****************************************************************************
 * Name: ny_product_camera_raw_next
 ****************************************************************************/

int ny_product_camera_raw_next(struct ny_camera_client_s *client,
                               uint8_t *pixels, size_t capacity,
                               uint64_t *sequence, uint64_t *timestamp)
{
  if (client == NULL || pixels == NULL || sequence == NULL ||
      timestamp == NULL || capacity < NY_CAMERA_RAW_SIZE)
    return -EINVAL;
  if (!ny_product_privacy_camera_allowed())
    return -EACCES;
  nxmutex_lock(&g_camera_lock);
  int ret = -EAGAIN;
  if (!client->active || !g_camera_running || g_camera_stopping ||
      (client->generation && client->generation != g_camera_generation))
    ret = -EPIPE;
  else
    {
      bool first = client->generation == 0;
      client->generation = g_camera_generation;
      g_camera_raw_pending = true;
      if (first)
        *sequence = g_camera_raw_sequence;
      else if (g_camera_raw_sequence > *sequence)
        {
          memcpy(pixels, g_camera_raw, NY_CAMERA_RAW_SIZE);
          *sequence = g_camera_raw_sequence;
          *timestamp = g_camera_raw_timestamp;
          ret = 0;
        }
    }
  nxmutex_unlock(&g_camera_lock);
  return ret;
}
