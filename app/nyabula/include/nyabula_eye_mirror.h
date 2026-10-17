/****************************************************************************
 * apps/graphics/nyabula_eye/include/nyabula_eye_mirror.h
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

#ifndef __APPS_GRAPHICS_NYABULA_EYE_INCLUDE_NYABULA_EYE_MIRROR_H
#define __APPS_GRAPHICS_NYABULA_EYE_INCLUDE_NYABULA_EYE_MIRROR_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The eye mirror shows the two eye pages to somebody who has no panels: a
 * viewer attaches, and from then on every page the renderer finishes is
 * kept as RGB565 and handed to the viewer as the difference to the page it
 * was given last.  With no viewer attached the renderer pays for one load
 * of an integer per page and nothing is allocated.
 *
 * A viewer receives a sequence of records.  Each starts with this header,
 * all fields little-endian:
 *
 *   0  "NEM1"
 *   4  type      NYABULA_EYE_MIRROR_FRAME or _KEEPALIVE
 *   5  eye       0 = left, 1 = right
 *   6  flags     NYABULA_EYE_MIRROR_KEY: decode against a black page
 *   7  scale     1 = every pixel, 2 = 2x2 pixels averaged into one
 *   8  width     in pixels, after scaling
 *   10 height
 *   12 length    of the payload that follows
 *
 * The payload is a run of operations over the pixels in raster order.  An
 * operation is one byte: the kind in the two high bits, the pixel count
 * minus one in the six low bits.  A low part of 63 means the count does
 * not fit and follows as two bytes (count itself, not minus one).
 *
 *   SKIP     the pixels stay what they were on the previous page
 *   RUN      the pixels all take the one RGB565 value that follows
 *   LITERAL  one RGB565 value follows for each pixel
 */

#define NYABULA_EYE_MIRROR_HEADER     16
#define NYABULA_EYE_MIRROR_FRAME      0
#define NYABULA_EYE_MIRROR_KEEPALIVE  1
#define NYABULA_EYE_MIRROR_KEY        0x01

#define NYABULA_EYE_MIRROR_OP_SKIP    0
#define NYABULA_EYE_MIRROR_OP_RUN     1
#define NYABULA_EYE_MIRROR_OP_LITERAL 2
#define NYABULA_EYE_MIRROR_OP_LONG    63

/* How many viewers may be attached at once.  Each costs the memory of the
 * pages it was last given plus one record.
 */

#define NYABULA_EYE_MIRROR_VIEWERS    2

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct nyabula_eye_mirror_viewer_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/* Codec: nyabula_eye_mirror_codec.c, free of any operating system. */

/* The most nyabula_eye_mirror_encode() can write for this many pixels. */

size_t nyabula_eye_mirror_bound(size_t pixels);

/* Write the operations that turn `previous` into `page`, and make
 * `previous` equal to `page` on the way.  Returns the number of bytes
 * written, or 0 when `capacity` is below nyabula_eye_mirror_bound().
 */

size_t nyabula_eye_mirror_encode(const uint16_t *page, uint16_t *previous,
                                 size_t pixels, uint8_t *out,
                                 size_t capacity);

/* Renderer side: nyabula_eye_mirror.c. */

/* Does this eye's page have to be rendered even though nothing changed?
 * True once per eye after the first viewer attached.
 */

bool nyabula_eye_mirror_wants(int eye);

/* A finished ARGB8888 page.  Does nothing unless a viewer is attached.
 * Each eye is rendered when its own display refreshes, so there is no
 * moment at which "both eyes are done": every page wakes the viewers, and
 * it is their pacing that puts the two eyes into one batch.
 */

void nyabula_eye_mirror_publish(int eye, const uint8_t *argb, uint32_t width,
                                uint32_t height, uint32_t stride);

/* Viewer side. */

/* Attach.  `scale` is 1 or 2.  -EBUSY with too many viewers, -ENOMEM. */

int nyabula_eye_mirror_open(int scale,
                            struct nyabula_eye_mirror_viewer_s **viewer);

/* Wait up to `timeout_ms` for pages this viewer has not been given, and
 * return them as records.  `*data` stays valid until the next call with
 * this viewer.  Returns 0 with `*length` 0 when nothing changed in time.
 */

int nyabula_eye_mirror_next(struct nyabula_eye_mirror_viewer_s *viewer,
                            int timeout_ms, const uint8_t **data,
                            size_t *length);

/* Detach.  The last viewer to leave frees everything. */

void nyabula_eye_mirror_close(struct nyabula_eye_mirror_viewer_s *viewer);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_GRAPHICS_NYABULA_EYE_INCLUDE_NYABULA_EYE_MIRROR_H */
