/****************************************************************************
 * app/ws2812_test/ws2812_test_main.c
 *
 * WS2812 strip smoke test for /dev/ws2812 (SPI3 MOSI on the KICKPI-K7).
 *
 * Sequence, all on the first 16 LEDs only:
 *   1. red for 1 s
 *   2. green for 1 s
 *   3. blue for 1 s
 *   4. four red, four green, four blue, four white, for 3 s
 *   5. all black, then return
 *
 * Every step is one write() of a complete 16-pixel frame: the character
 * driver keeps no state between writes, so the whole frame has to be
 * supplied each time.  LEDs beyond the 16th are left untouched (they never
 * receive the 24 bits that would update them).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Device registered by kickpi_k7_ws2812.c. */

#define WS2812_TEST_DEVPATH "/dev/ws2812"

/* Only the head of the chain is exercised. */

#define WS2812_TEST_NLEDS 16

/* Pixel format is the driver's: 0x00RRGGBB in host byte ordering. */

#define WS2812_TEST_RED   0x00ff0000u
#define WS2812_TEST_GREEN 0x0000ff00u
#define WS2812_TEST_BLUE  0x000000ffu
#define WS2812_TEST_WHITE 0x00ffffffu
#define WS2812_TEST_BLACK 0x00000000u

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ws2812_test_write_frame
 *
 * Description:
 *   Send one complete frame and report any failure.  The driver transmits
 *   before returning, so the delay that follows a call is the time the
 *   pattern is actually visible.
 *
 ****************************************************************************/

static int ws2812_test_write_frame(int fd, const uint32_t *frame)
{
  size_t frame_len = WS2812_TEST_NLEDS * sizeof(uint32_t);
  ssize_t nwritten;

  nwritten = write(fd, frame, frame_len);
  if (nwritten < 0)
    {
      fprintf(stderr, "ws2812_test: write failed: %d\n", errno);
      return -1;
    }

  if ((size_t)nwritten != frame_len)
    {
      fprintf(stderr, "ws2812_test: short write: %ld of %zu\n", (long)nwritten,
              frame_len);
      return -1;
    }

  return 0;
}

/****************************************************************************
 * Name: ws2812_test_write_solid
 *
 * Description:
 *   Light all 16 LEDs with the same color.
 *
 ****************************************************************************/

static int ws2812_test_write_solid(int fd, uint32_t color)
{
  uint32_t frame[WS2812_TEST_NLEDS];
  size_t i;

  for (i = 0; i < WS2812_TEST_NLEDS; i++)
    {
      frame[i] = color;
    }

  return ws2812_test_write_frame(fd, frame);
}

/****************************************************************************
 * Name: ws2812_test_dump_status
 *
 * Description:
 *   Print the clock and the bit codes the driver used for the last frame.
 *   The driver measures its own SCLK, so this is the figure to compare with
 *   a logic analyzer's reading.
 *
 ****************************************************************************/

static void ws2812_test_dump_status(void)
{
  char buffer[64];
  ssize_t n;
  int fd;

  fd = open(WS2812_TEST_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      return;
    }

  n = read(fd, buffer, sizeof(buffer) - 1);
  if (n > 0)
    {
      buffer[n] = '\0';
      printf("ws2812_test: %s", buffer);
    }

  close(fd);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 *
 * Description:
 *   Run the fixed sequence described in the file header and return.
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  int ret = EXIT_FAILURE;
  size_t i;

  fd = open(WS2812_TEST_DEVPATH, O_WRONLY);
  if (fd < 0)
    {
      fprintf(stderr, "ws2812_test: open %s failed: %d\n", WS2812_TEST_DEVPATH,
              errno);
      return EXIT_FAILURE;
    }

  /* Red, green, blue: one second each. */

  if (ws2812_test_write_solid(fd, WS2812_TEST_RED) < 0)
    {
      goto out;
    }

  sleep(1);

  if (ws2812_test_write_solid(fd, WS2812_TEST_GREEN) < 0)
    {
      goto out;
    }

  sleep(1);

  if (ws2812_test_write_solid(fd, WS2812_TEST_BLUE) < 0)
    {
      goto out;
    }

  sleep(1);

  /* Four red, four green, four blue, four white: three seconds. */

  {
    uint32_t frame[WS2812_TEST_NLEDS];

    for (i = 0; i < 4; i++)
      {
        frame[i] = WS2812_TEST_RED;
        frame[4 + i] = WS2812_TEST_GREEN;
        frame[8 + i] = WS2812_TEST_BLUE;
        frame[12 + i] = WS2812_TEST_WHITE;
      }

    if (ws2812_test_write_frame(fd, frame) < 0)
      {
        goto out;
      }
  }

  sleep(3);

  /* Blank the 16 LEDs and return. */

  if (ws2812_test_write_solid(fd, WS2812_TEST_BLACK) < 0)
    {
      goto out;
    }

  ret = EXIT_SUCCESS;

out:
  ws2812_test_dump_status();
  close(fd);
  return ret;
}
