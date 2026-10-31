/****************************************************************************
 * apps/graphics/nyabula_eye/tools/test_mirror_codec.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host test of the eye mirror codec.  Build and run where a C compiler is:
 *
 *   cc -O2 -Wall -Werror -Iinclude tools/test_mirror_codec.c \
 *      src/nyabula_eye_mirror_codec.c -o /tmp/test_mirror_codec
 *   /tmp/test_mirror_codec [WIDTH HEIGHT OUTDIR]
 *
 * Every page of a sequence is encoded against the one before it and decoded
 * again here, and the result has to be the page.  With OUTDIR the records
 * go to OUTDIR/stream.bin and the pages, as RGB565, to OUTDIR/pages.bin,
 * which is what the decoder of the control panel is checked against
 * (app/nyabula_web/apps/nyabula/test/eyeMirror.test.mjs).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nyabula_eye_mirror.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PAGES 9

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint32_t g_seed = 0x062;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t random_next(void)
{
  g_seed = g_seed * 1664525u + 1013904223u;
  return g_seed >> 8;
}

/* An eye of sorts: black, with a shaded disc that moves with `step`. */

static void draw_eye(uint16_t *page, int width, int height, int step)
{
  int cx = width / 2 + step * 3;
  int cy = height / 2 - step;
  int radius = height / 4;
  int x;
  int y;

  for (y = 0; y < height; y++)
    {
      for (x = 0; x < width; x++)
        {
          int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);

          page[y * width + x] =
              d < radius * radius
                  ? (uint16_t)(0x07e0 | ((d * 31 / (radius * radius)) << 11))
                  : 0;
        }
    }
}

static void draw_page(uint16_t *page, int width, int height, int index)
{
  int pixels = width * height;
  int i;

  switch (index)
    {
      case 0:                     /* The key page: all of it is new */
      case 1:                     /* Small motion */
      case 2:
        draw_eye(page, width, height, index);
        break;

      case 3:                     /* Nothing changed at all */
        draw_eye(page, width, height, 2);
        break;

      case 4:                     /* One pixel, the last one */
        draw_eye(page, width, height, 2);
        page[pixels - 1] = 0xffff;
        break;

      case 5:                     /* Noise: nothing to gain anywhere */
        for (i = 0; i < pixels; i++)
          {
            page[i] = (uint16_t)random_next();
          }
        break;

      case 6:                     /* One colour: a run longer than a count */
        for (i = 0; i < pixels; i++)
          {
            page[i] = 0x1234;
          }
        break;

      case 7:                     /* Runs and skips too short to be worth it */
        for (i = 0; i < pixels; i++)
          {
            page[i] = (i % 5) < 2 ? 0x1234 : (uint16_t)(i / 2);
          }
        break;

      default:                    /* Back to black */
        memset(page, 0, pixels * sizeof(uint16_t));
        break;
    }
}

/* The decoder the format implies, with every bound checked. */

static int decode(const uint8_t *data, size_t length, uint16_t *page,
                  size_t pixels)
{
  size_t at = 0;
  size_t pixel = 0;
  size_t i;

  while (at < length)
    {
      int kind = data[at] >> 6;
      size_t count = (data[at] & 0x3f) + 1;

      if ((data[at++] & 0x3f) == NYABULA_EYE_MIRROR_OP_LONG)
        {
          if (at + 2 > length)
            {
              return -1;
            }

          count = data[at] | (data[at + 1] << 8);
          at += 2;
        }

      if (count == 0 || pixel + count > pixels)
        {
          return -2;
        }

      if (kind == NYABULA_EYE_MIRROR_OP_RUN)
        {
          if (at + 2 > length)
            {
              return -3;
            }

          for (i = 0; i < count; i++)
            {
              page[pixel + i] = (uint16_t)(data[at] | (data[at + 1] << 8));
            }

          at += 2;
        }
      else if (kind == NYABULA_EYE_MIRROR_OP_LITERAL)
        {
          if (at + count * 2 > length)
            {
              return -4;
            }

          for (i = 0; i < count; i++, at += 2)
            {
              page[pixel + i] = (uint16_t)(data[at] | (data[at + 1] << 8));
            }
        }
      else if (kind != NYABULA_EYE_MIRROR_OP_SKIP)
        {
          return -5;
        }

      pixel += count;
    }

  return pixel == pixels ? 0 : -6;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char **argv)
{
  int width = argc > 2 ? atoi(argv[1]) : 360;
  int height = argc > 2 ? atoi(argv[2]) : 360;
  const char *outdir = argc > 3 ? argv[3] : NULL;
  size_t pixels = (size_t)width * height;
  size_t bound = nyabula_eye_mirror_bound(pixels);
  uint16_t *page = malloc(pixels * sizeof(uint16_t));
  uint16_t *previous = calloc(pixels, sizeof(uint16_t));
  uint16_t *decoded = calloc(pixels, sizeof(uint16_t));
  uint8_t *record = malloc(NYABULA_EYE_MIRROR_HEADER + bound);
  FILE *stream = NULL;
  FILE *pages = NULL;
  char path[512];
  int index;

  if (outdir != NULL)
    {
      snprintf(path, sizeof(path), "%s/stream.bin", outdir);
      stream = fopen(path, "wb");
      snprintf(path, sizeof(path), "%s/pages.bin", outdir);
      pages = fopen(path, "wb");
      if (stream == NULL || pages == NULL)
        {
          perror(outdir);
          return 1;
        }
    }

  if (nyabula_eye_mirror_encode(page, previous, pixels, record, bound - 1))
    {
      printf("FAIL: a buffer below the bound was accepted\n");
      return 1;
    }

  for (index = 0; index < PAGES; index++)
    {
      uint8_t *payload = record + NYABULA_EYE_MIRROR_HEADER;
      size_t size;
      int ret;

      draw_page(page, width, height, index);
      size = nyabula_eye_mirror_encode(page, previous, pixels, payload,
                                       bound);
      ret = size == 0 ? -100 : decode(payload, size, decoded, pixels);
      if (ret < 0 || size > bound ||
          memcmp(decoded, page, pixels * sizeof(uint16_t)) != 0 ||
          memcmp(previous, page, pixels * sizeof(uint16_t)) != 0)
        {
          printf("FAIL: page %d: decode %d, %zu bytes\n", index, ret, size);
          return 1;
        }

      printf("page %d: %zu bytes of %zu (%.1f%%)\n", index, size,
             pixels * 2, 100.0 * size / (pixels * 2));
      if (stream != NULL)
        {
          memcpy(record, "NEM1", 4);
          record[4] = NYABULA_EYE_MIRROR_FRAME;
          record[5] = 0;
          record[6] = index == 0 ? NYABULA_EYE_MIRROR_KEY : 0;
          record[7] = 1;
          record[8] = width & 0xff;
          record[9] = width >> 8;
          record[10] = height & 0xff;
          record[11] = height >> 8;
          record[12] = size & 0xff;
          record[13] = (size >> 8) & 0xff;
          record[14] = (size >> 16) & 0xff;
          record[15] = (size >> 24) & 0xff;
          fwrite(record, 1, NYABULA_EYE_MIRROR_HEADER + size, stream);
          fwrite(page, sizeof(uint16_t), pixels, pages);
        }
    }

  if (stream != NULL)
    {
      fclose(stream);
      fclose(pages);
    }

  free(page);
  free(previous);
  free(decoded);
  free(record);
  printf("MIRROR_CODEC_PASS %dx%d, %d pages\n", width, height, PAGES);
  return 0;
}
