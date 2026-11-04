/****************************************************************************
 * app/nyabula_core/ny_camera_png.c
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

#include "ny_camera_png.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define NY_CAMERA_PNG_MAX_WIDTH  640
#define NY_CAMERA_PNG_MAX_HEIGHT 480
#define NY_CAMERA_PNG_OVERHEAD   57

static uint8_t ny_camera_png_byte(int value);
static void ny_camera_png_u32(uint8_t *p, uint32_t value);
static void ny_camera_png_chunk(uint8_t *p, const char *type, size_t size);

/****************************************************************************
 * Name: ny_camera_png_byte
 ****************************************************************************/

static uint8_t ny_camera_png_byte(int value)
{
  return value < 0 ? 0 : value > 255 ? 255 : value;
}

/****************************************************************************
 * Name: ny_camera_png_u32
 ****************************************************************************/

static void ny_camera_png_u32(uint8_t *p, uint32_t value)
{
  p[0] = value >> 24;
  p[1] = value >> 16;
  p[2] = value >> 8;
  p[3] = value;
}

/****************************************************************************
 * Name: ny_camera_png_chunk
 ****************************************************************************/

static void ny_camera_png_chunk(uint8_t *p, const char *type, size_t size)
{
  ny_camera_png_u32(p, size);
  memcpy(p + 4, type, 4);
  ny_camera_png_u32(p + 8 + size, crc32(0, p + 4, size + 4));
}

/****************************************************************************
 * Name: ny_camera_png
 ****************************************************************************/

int ny_camera_png(const uint8_t *nv12, size_t size, uint32_t width,
                  uint32_t height, uint32_t stride, unsigned int factor,
                  uint8_t **output, size_t *length)
{
  if (output == NULL || length == NULL)
    return -EINVAL;
  *output = NULL;
  *length = 0;
  if (nv12 == NULL || width == 0 || height == 0 ||
      width > NY_CAMERA_PNG_MAX_WIDTH || height > NY_CAMERA_PNG_MAX_HEIGHT ||
      stride < width || stride > NY_CAMERA_PNG_MAX_WIDTH * 2 ||
      (width | height | stride) & 1 || (factor != 1 && factor != 2) ||
      size < (size_t)stride * height * 3 / 2)
    return -EINVAL;

  uint32_t w = width / factor;
  uint32_t h = height / factor;
  size_t row_size = (size_t)w * 3 + 1;
  size_t raw_size = row_size * h;
  uint8_t *raw = malloc(raw_size);
  uLongf compressed = compressBound(raw_size);
  uint8_t *png = malloc(compressed + NY_CAMERA_PNG_OVERHEAD);
  if (raw == NULL || png == NULL)
    {
      free(raw);
      free(png);
      return -ENOMEM;
    }
  const uint8_t *uv = nv12 + (size_t)stride * height;
  for (uint32_t y = 0; y < h; y++)
    {
      uint32_t sy = y * factor;
      uint8_t *row = raw + y * row_size;
      *row++ = 0; /* PNG filter: none. */
      for (uint32_t x = 0; x < w; x++)
        {
          uint32_t sx = x * factor;
          int luma = nv12[sy * stride + sx] - 16;
          int u = uv[(sy / 2) * stride + (sx & ~1u)] - 128;
          int v = uv[(sy / 2) * stride + (sx & ~1u) + 1] - 128;
          if (luma < 0)
            luma = 0;
          *row++ = ny_camera_png_byte((298 * luma + 409 * v + 128) >> 8);
          *row++ =
              ny_camera_png_byte((298 * luma - 100 * u - 208 * v + 128) >> 8);
          *row++ = ny_camera_png_byte((298 * luma + 516 * u + 128) >> 8);
        }
    }
  int ret = compress2(png + 41, &compressed, raw, raw_size, 3);
  free(raw);
  if (ret != Z_OK)
    {
      free(png);
      return ret == Z_MEM_ERROR ? -ENOMEM : -EIO;
    }
  memcpy(png, "\x89PNG\r\n\x1a\n", 8);
  ny_camera_png_u32(png + 16, w);
  ny_camera_png_u32(png + 20, h);
  png[24] = 8; /* Eight bits per RGB channel. */
  png[25] = 2;
  png[26] = png[27] = png[28] = 0;
  ny_camera_png_chunk(png + 8, "IHDR", 13);
  ny_camera_png_chunk(png + 33, "IDAT", compressed);
  ny_camera_png_chunk(png + 45 + compressed, "IEND", 0);
  *output = png;
  *length = compressed + NY_CAMERA_PNG_OVERHEAD;
  return 0;
}
