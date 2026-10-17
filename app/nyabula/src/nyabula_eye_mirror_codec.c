/****************************************************************************
 * apps/graphics/nyabula_eye/src/nyabula_eye_mirror_codec.c
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

#include "nyabula_eye_mirror.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A SKIP or RUN shorter than this costs more than the literal it would
 * interrupt: two operation bytes against at most two pixel values saved.
 */

#define MIRROR_WORTH      3
#define MIRROR_COUNT_MAX  65535

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static size_t mirror_same(const uint16_t *page, const uint16_t *previous,
                          size_t at, size_t pixels);
static size_t mirror_flat(const uint16_t *page, size_t at, size_t pixels);
static uint8_t *mirror_operation(uint8_t *out, int kind, size_t count);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mirror_same
 *
 * Description:
 *   How many pixels from `at` on are what the previous page had.
 *
 ****************************************************************************/

static size_t mirror_same(const uint16_t *page, const uint16_t *previous,
                          size_t at, size_t pixels)
{
  size_t end = at;

  while (end < pixels && end - at < MIRROR_COUNT_MAX &&
         page[end] == previous[end])
    {
      end++;
    }

  return end - at;
}

/****************************************************************************
 * Name: mirror_flat
 *
 * Description:
 *   How many pixels from `at` on have the value of the one at `at`.
 *
 ****************************************************************************/

static size_t mirror_flat(const uint16_t *page, size_t at, size_t pixels)
{
  size_t end = at + 1;

  while (end < pixels && end - at < MIRROR_COUNT_MAX &&
         page[end] == page[at])
    {
      end++;
    }

  return end - at;
}

/****************************************************************************
 * Name: mirror_operation
 ****************************************************************************/

static uint8_t *mirror_operation(uint8_t *out, int kind, size_t count)
{
  if (count < NYABULA_EYE_MIRROR_OP_LONG + 1)
    {
      *out++ = (uint8_t)((kind << 6) | (count - 1));
    }
  else
    {
      *out++ = (uint8_t)((kind << 6) | NYABULA_EYE_MIRROR_OP_LONG);
      *out++ = (uint8_t)(count & 0xff);
      *out++ = (uint8_t)(count >> 8);
    }

  return out;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nyabula_eye_mirror_bound
 *
 * Description:
 *   The worst page is one literal after another, each as long as a count
 *   can say, and each with the long form of the operation in front.
 *
 ****************************************************************************/

size_t nyabula_eye_mirror_bound(size_t pixels)
{
  return pixels * 2 + (pixels / MIRROR_COUNT_MAX + 1) * 3;
}

/****************************************************************************
 * Name: nyabula_eye_mirror_encode
 ****************************************************************************/

size_t nyabula_eye_mirror_encode(const uint16_t *page, uint16_t *previous,
                                 size_t pixels, uint8_t *out,
                                 size_t capacity)
{
  uint8_t *start = out;
  size_t at = 0;

  if (capacity < nyabula_eye_mirror_bound(pixels))
    {
      return 0;
    }

  while (at < pixels)
    {
      size_t count = mirror_same(page, previous, at, pixels);
      size_t end;
      size_t i;

      if (count >= MIRROR_WORTH || (count > 0 && at + count == pixels))
        {
          out = mirror_operation(out, NYABULA_EYE_MIRROR_OP_SKIP, count);
          at += count;
          continue;
        }

      count = mirror_flat(page, at, pixels);
      if (count >= MIRROR_WORTH)
        {
          out = mirror_operation(out, NYABULA_EYE_MIRROR_OP_RUN, count);
          *out++ = (uint8_t)(page[at] & 0xff);
          *out++ = (uint8_t)(page[at] >> 8);
          for (i = 0; i < count; i++)
            {
              previous[at + i] = page[at];
            }

          at += count;
          continue;
        }

      /* A literal runs until something cheaper starts. */

      end = at + 1;
      while (end < pixels && end - at < MIRROR_COUNT_MAX &&
             mirror_same(page, previous, end, pixels) < MIRROR_WORTH &&
             mirror_flat(page, end, pixels) < MIRROR_WORTH)
        {
          end++;
        }

      out = mirror_operation(out, NYABULA_EYE_MIRROR_OP_LITERAL, end - at);
      for (; at < end; at++)
        {
          *out++ = (uint8_t)(page[at] & 0xff);
          *out++ = (uint8_t)(page[at] >> 8);
          previous[at] = page[at];
        }
    }

  return (size_t)(out - start);
}
