/****************************************************************************
 * apps/graphics/nyabula_eye/src/nyabula_eye_mirror.c
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

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nyabula_eye_engine.h"
#include "nyabula_eye_mirror.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MIRROR_EYES  NYABULA_EYE_COUNT

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What the renderer last finished, shared by every viewer. */

struct mirror_s
{
  pthread_mutex_t lock;
  pthread_cond_t changed;
  volatile int viewers;          /* Read by the renderer without the lock */
  uint16_t *page[MIRROR_EYES];   /* RGB565, width * height */
  uint32_t serial[MIRROR_EYES];  /* 0: nothing published for this eye */
  uint32_t width;
  uint32_t height;
};

/* What one viewer has been given. */

struct nyabula_eye_mirror_viewer_s
{
  int scale;
  uint32_t width;                    /* After scaling; 0: nothing yet */
  uint32_t height;
  uint32_t seen[MIRROR_EYES];        /* Serial of the page last given */
  uint16_t *previous[MIRROR_EYES];   /* The page last given */
  uint16_t *scaled;                  /* Scratch for scale 2 */
  uint8_t *record;
  size_t capacity;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int mirror_viewer_prepare(struct nyabula_eye_mirror_viewer_s *viewer);
static void mirror_viewer_free(struct nyabula_eye_mirror_viewer_s *viewer);
static void mirror_halve(const uint16_t *page, uint32_t width,
                         uint32_t height, uint16_t *out);
static bool mirror_viewer_behind(
    const struct nyabula_eye_mirror_viewer_s *viewer);
static void mirror_put16(uint8_t *out, uint32_t value);
static void mirror_put32(uint8_t *out, uint32_t value);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct mirror_s g_mirror =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .changed = PTHREAD_COND_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void mirror_put16(uint8_t *out, uint32_t value)
{
  out[0] = (uint8_t)(value & 0xff);
  out[1] = (uint8_t)((value >> 8) & 0xff);
}

static void mirror_put32(uint8_t *out, uint32_t value)
{
  mirror_put16(out, value & 0xffff);
  mirror_put16(out + 2, value >> 16);
}

/****************************************************************************
 * Name: mirror_viewer_prepare
 *
 * Description:
 *   Size the viewer's buffers once the size of a page is known.  Called
 *   with the lock held.
 *
 ****************************************************************************/

static int mirror_viewer_prepare(struct nyabula_eye_mirror_viewer_s *viewer)
{
  size_t pixels;
  int eye;

  if (viewer->width != 0)
    {
      return 0;
    }

  viewer->width = g_mirror.width / viewer->scale;
  viewer->height = g_mirror.height / viewer->scale;
  pixels = (size_t)viewer->width * viewer->height;
  if (pixels == 0)
    {
      viewer->width = 0;
      return -EINVAL;
    }

  viewer->capacity = MIRROR_EYES * (NYABULA_EYE_MIRROR_HEADER +
                                    nyabula_eye_mirror_bound(pixels));
  viewer->record = malloc(viewer->capacity);
  if (viewer->scale > 1)
    {
      viewer->scaled = malloc(pixels * sizeof(uint16_t));
    }

  for (eye = 0; eye < MIRROR_EYES; eye++)
    {
      viewer->previous[eye] = calloc(pixels, sizeof(uint16_t));
    }

  if (viewer->record == NULL || (viewer->scale > 1 && viewer->scaled == NULL))
    {
      goto nomem;
    }

  for (eye = 0; eye < MIRROR_EYES; eye++)
    {
      if (viewer->previous[eye] == NULL)
        {
          goto nomem;
        }
    }

  return 0;

nomem:
  mirror_viewer_free(viewer);
  return -ENOMEM;
}

static void mirror_viewer_free(struct nyabula_eye_mirror_viewer_s *viewer)
{
  int eye;

  for (eye = 0; eye < MIRROR_EYES; eye++)
    {
      free(viewer->previous[eye]);
      viewer->previous[eye] = NULL;
    }

  free(viewer->scaled);
  free(viewer->record);
  viewer->scaled = NULL;
  viewer->record = NULL;
  viewer->width = 0;
}

/****************************************************************************
 * Name: mirror_halve
 *
 * Description:
 *   Average each 2x2 block into one pixel.
 *
 ****************************************************************************/

static void mirror_halve(const uint16_t *page, uint32_t width,
                         uint32_t height, uint16_t *out)
{
  uint32_t x;
  uint32_t y;

  for (y = 0; y + 1 < height; y += 2)
    {
      const uint16_t *upper = page + (size_t)y * width;
      const uint16_t *lower = upper + width;

      for (x = 0; x + 1 < width; x += 2)
        {
          uint32_t red = (upper[x] >> 11) + (upper[x + 1] >> 11) +
                         (lower[x] >> 11) + (lower[x + 1] >> 11);
          uint32_t green = ((upper[x] >> 5) & 0x3f) +
                           ((upper[x + 1] >> 5) & 0x3f) +
                           ((lower[x] >> 5) & 0x3f) +
                           ((lower[x + 1] >> 5) & 0x3f);
          uint32_t blue = (upper[x] & 0x1f) + (upper[x + 1] & 0x1f) +
                          (lower[x] & 0x1f) + (lower[x + 1] & 0x1f);

          *out++ = (uint16_t)(((red + 2) >> 2) << 11 |
                              ((green + 2) >> 2) << 5 | ((blue + 2) >> 2));
        }
    }
}

static bool mirror_viewer_behind(
    const struct nyabula_eye_mirror_viewer_s *viewer)
{
  int eye;

  for (eye = 0; eye < MIRROR_EYES; eye++)
    {
      if (g_mirror.serial[eye] != viewer->seen[eye])
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nyabula_eye_mirror_wants
 ****************************************************************************/

bool nyabula_eye_mirror_wants(int eye)
{
  return g_mirror.viewers > 0 && eye >= 0 && eye < MIRROR_EYES &&
         g_mirror.serial[eye] == 0;
}

/****************************************************************************
 * Name: nyabula_eye_mirror_publish
 ****************************************************************************/

void nyabula_eye_mirror_publish(int eye, const uint8_t *argb, uint32_t width,
                                uint32_t height, uint32_t stride)
{
  uint16_t *out;
  uint32_t x;
  uint32_t y;

  if (g_mirror.viewers <= 0 || eye < 0 || eye >= MIRROR_EYES ||
      argb == NULL || width == 0 || height == 0)
    {
      return;
    }

  pthread_mutex_lock(&g_mirror.lock);
  if (g_mirror.viewers <= 0 ||
      (g_mirror.width != 0 &&
       (g_mirror.width != width || g_mirror.height != height)))
    {
      pthread_mutex_unlock(&g_mirror.lock);
      return;
    }

  if (g_mirror.page[eye] == NULL)
    {
      g_mirror.page[eye] = malloc((size_t)width * height * sizeof(uint16_t));
      if (g_mirror.page[eye] == NULL)
        {
          pthread_mutex_unlock(&g_mirror.lock);
          return;
        }
    }

  g_mirror.width = width;
  g_mirror.height = height;
  out = g_mirror.page[eye];
  for (y = 0; y < height; y++)
    {
      const uint8_t *in = argb + (size_t)y * stride;

      /* ARGB8888 lies in memory as blue, green, red, alpha. */

      for (x = 0; x < width; x++, in += 4)
        {
          *out++ = (uint16_t)((in[2] >> 3) << 11 | (in[1] >> 2) << 5 |
                              (in[0] >> 3));
        }
    }

  if (++g_mirror.serial[eye] == 0)
    {
      g_mirror.serial[eye] = 1;
    }

  pthread_cond_broadcast(&g_mirror.changed);
  pthread_mutex_unlock(&g_mirror.lock);
}

/****************************************************************************
 * Name: nyabula_eye_mirror_open
 ****************************************************************************/

int nyabula_eye_mirror_open(int scale,
                            struct nyabula_eye_mirror_viewer_s **viewer)
{
  struct nyabula_eye_mirror_viewer_s *created;

  if (viewer == NULL || (scale != 1 && scale != 2))
    {
      return -EINVAL;
    }

  created = calloc(1, sizeof(*created));
  if (created == NULL)
    {
      return -ENOMEM;
    }

  created->scale = scale;
  pthread_mutex_lock(&g_mirror.lock);
  if (g_mirror.viewers >= NYABULA_EYE_MIRROR_VIEWERS)
    {
      pthread_mutex_unlock(&g_mirror.lock);
      free(created);
      return -EBUSY;
    }

  g_mirror.viewers++;
  pthread_mutex_unlock(&g_mirror.lock);
  *viewer = created;
  return 0;
}

/****************************************************************************
 * Name: nyabula_eye_mirror_next
 ****************************************************************************/

int nyabula_eye_mirror_next(struct nyabula_eye_mirror_viewer_s *viewer,
                            int timeout_ms, const uint8_t **data,
                            size_t *length)
{
  struct timespec until;
  uint8_t *out;
  int ret = 0;
  int eye;

  if (viewer == NULL || data == NULL || length == NULL)
    {
      return -EINVAL;
    }

  *data = NULL;
  *length = 0;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += timeout_ms / 1000;
  until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L)
    {
      until.tv_sec++;
      until.tv_nsec -= 1000000000L;
    }

  pthread_mutex_lock(&g_mirror.lock);
  while (!mirror_viewer_behind(viewer))
    {
      if (pthread_cond_timedwait(&g_mirror.changed, &g_mirror.lock,
                                 &until) != 0)
        {
          break;
        }
    }

  if (!mirror_viewer_behind(viewer))
    {
      goto out;
    }

  ret = mirror_viewer_prepare(viewer);
  if (ret < 0)
    {
      goto out;
    }

  out = viewer->record;
  for (eye = 0; eye < MIRROR_EYES; eye++)
    {
      size_t pixels = (size_t)viewer->width * viewer->height;
      const uint16_t *page = g_mirror.page[eye];
      size_t size;

      if (g_mirror.serial[eye] == viewer->seen[eye] || page == NULL)
        {
          continue;
        }

      if (viewer->scale > 1)
        {
          mirror_halve(page, g_mirror.width, g_mirror.height,
                       viewer->scaled);
          page = viewer->scaled;
        }

      size = nyabula_eye_mirror_encode(
          page, viewer->previous[eye], pixels,
          out + NYABULA_EYE_MIRROR_HEADER,
          viewer->capacity - (size_t)(out - viewer->record) -
          NYABULA_EYE_MIRROR_HEADER);
      if (size == 0)
        {
          ret = -ENOBUFS;
          goto out;
        }

      memcpy(out, "NEM1", 4);
      out[4] = NYABULA_EYE_MIRROR_FRAME;
      out[5] = (uint8_t)eye;
      out[6] = viewer->seen[eye] == 0 ? NYABULA_EYE_MIRROR_KEY : 0;
      out[7] = (uint8_t)viewer->scale;
      mirror_put16(out + 8, viewer->width);
      mirror_put16(out + 10, viewer->height);
      mirror_put32(out + 12, (uint32_t)size);
      out += NYABULA_EYE_MIRROR_HEADER + size;
      viewer->seen[eye] = g_mirror.serial[eye];
    }

  *data = viewer->record;
  *length = (size_t)(out - viewer->record);

out:
  pthread_mutex_unlock(&g_mirror.lock);
  return ret;
}

/****************************************************************************
 * Name: nyabula_eye_mirror_close
 ****************************************************************************/

void nyabula_eye_mirror_close(struct nyabula_eye_mirror_viewer_s *viewer)
{
  int eye;

  if (viewer == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_mirror.lock);
  if (--g_mirror.viewers <= 0)
    {
      /* Nobody is looking: give the memory back, and have the pages
       * rendered afresh for whoever comes next.
       */

      g_mirror.viewers = 0;
      g_mirror.width = 0;
      g_mirror.height = 0;
      for (eye = 0; eye < MIRROR_EYES; eye++)
        {
          free(g_mirror.page[eye]);
          g_mirror.page[eye] = NULL;
          g_mirror.serial[eye] = 0;
        }
    }

  pthread_mutex_unlock(&g_mirror.lock);
  mirror_viewer_free(viewer);
  free(viewer);
}
