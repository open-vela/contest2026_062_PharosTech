/****************************************************************************
 * boards/rk3576/drivers/drivers/ws2812/ws2812.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
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
 * WS2812 addressable LED character driver -- SPI backend.
 *
 * One WS2812 bit is encoded as one SPI byte, so the SPI clock must run at
 * eight times the strip's bit rate.  A frame is bracketed by two runs of
 * zero bytes that hold the data line low for the reset/latch time: the
 * leading run separates the frame from whatever the line did before it,
 * the trailing run latches the frame before write() returns even if the
 * data line does not stay low afterwards.
 *
 * Unlike the in-tree NuttX driver this one never keeps a shadow buffer and
 * never holds the SPI bus between writes:
 *
 *   write()  pack the caller's pixels, lock the bus, notify the select
 *            callback, transmit, unselect, unlock.
 *
 * Consequently every write() is a complete frame and the bus can be shared
 * by several strips, each with its own select callback, that take turns.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "ws2812.h"

#ifdef CONFIG_WS2812_CHARDEV

/* This driver exports ws2812_register() with its own signature.  The
 * in-tree non-SPI model uses the same name, so refuse to build both.
 */

#ifdef CONFIG_WS2812_NON_SPI_DRIVER
#error \
    "CONFIG_WS2812_NON_SPI_DRIVER also defines ws2812_register(); disable one of the two"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* One WS2812 bit is encoded as one SPI byte.  The byte is a run of high
 * SPI bits starting at bit 6 (bit 7 stays 0, which also leaves MOSI low
 * after the last clock), so the run length sets the WS2812 high time:
 *
 *   T(high) = high_bits / SPI_clock
 *
 * The strip decides a bit from that high time alone; the remaining SPI
 * bits of the byte are the low time.  The run lengths are derived at run
 * time from the clock the controller actually produces, not from the
 * requested one: the SPI lower-half can only pick an even divider on a
 * fixed set of sources and may land well below the request.
 */

#define WS2812_T0H_TARGET_NS 250
#define WS2812_T1H_TARGET_NS 650

/* Reset/latch low time.  The part in use specifies RES >= 280 us; the
 * shorter 50 us some data sheets quote is not enough here.  The run has to
 * be driven, not merely held by the board: once the transfer ends the pin
 * is handed back to GPIO, and the strip must already have latched before
 * that switch, otherwise the tail of the frame can be disturbed.
 */

#define WS2812_RESET_US 300

/* Reset run for the fastest clock the driver accepts (9 MHz), in bytes.
 * The transmit buffer is sized for it; the run actually used is computed
 * from the achieved clock.
 */

#define WS2812_RST_MAX ((9ull * 1000000ull * WS2812_RESET_US) / 8000000ull)

/* One LED is 24 WS2812 bits, one SPI byte per bit. */

#define WS2812_BYTES_PER_LED (8 * 3)

/* Frame layout: leading reset run, up to MAX_PIXELS LEDs, trailing reset
 * run.  The leading run re-synchronizes the strip's bit decoder; the
 * trailing run guarantees the frame is latched before write() returns,
 * independently of how the data line behaves once the transfer is over.
 */

#define WS2812_TXBUFSIZE \
  (2 * WS2812_RST_MAX +  \
   CONFIG_WS2812_CHARDEV_MAX_PIXELS * WS2812_BYTES_PER_LED)

/* Clock calibration.  The SPI lower-half reports the rate its own model
 * derives, and that model and the controller do not always agree: on this
 * SoC the TRM's SPI source mux table and the vendor clock tree differ, so
 * the reported figure can be off by the ratio of the two sources.  Time a
 * burst of zero bytes against the system tick instead and derive the byte
 * rate the strip will actually see.  Zero bytes hold the data line low,
 * which is the idle state a WS2812 strip expects, so the burst is inert.
 */

#define WS2812_CAL_BYTES 2048 /* bytes per calibration burst      */
#define WS2812_CAL_MS    90   /* wall time to measure over, in ms */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ws2812_dev_s
{
  FAR struct spi_dev_s *spi;    /* SPI lower-half owned by the board   */
  ws2812_select_cb_t select_cb; /* Optional select notification        */
  FAR uint8_t *txbuf;           /* Transmit scratch (not a cache)      */
  mutex_t lock;                 /* Serializes write() on this device   */
  uint32_t freq;                /* Measured SCLK, 0 until calibrated   */
  uint8_t zero;                 /* High-run bit count of the last '0'  */
  uint8_t one;                  /* High-run bit count of the last '1'  */
  uint16_t rst;                 /* Reset bytes of the last frame       */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static ssize_t ws2812_read(FAR struct file *filep, FAR char *buffer,
                           size_t buflen);
static ssize_t ws2812_write(FAR struct file *filep, FAR const char *buffer,
                            size_t buflen);
static off_t ws2812_seek(FAR struct file *filep, off_t offset, int whence);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_ws2812_fops = {
  .read = ws2812_read,
  .write = ws2812_write,
  .seek = ws2812_seek,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ws2812_lock
 *
 * Description:
 *   Acquire the SPI bus and notify the select callback (if any) that this
 *   strip is now the active device.  This is the single place where the
 *   driver couples to the bus mutex, so every frame is preceded by the
 *   callback while the bus is guaranteed to stay with this device.
 *
 ****************************************************************************/

static void ws2812_lock(FAR struct ws2812_dev_s *priv)
{
  SPI_LOCK(priv->spi, true);

  if (priv->select_cb != NULL)
    {
      priv->select_cb(true);
    }
}

/****************************************************************************
 * Name: ws2812_unlock
 *
 * Description:
 *   Notify the select callback (if any) that this strip is about to be
 *   deselected, then release the SPI bus.  This is the counterpart of
 *   ws2812_lock().
 *
 ****************************************************************************/

static void ws2812_unlock(FAR struct ws2812_dev_s *priv)
{
  if (priv->select_cb != NULL)
    {
      priv->select_cb(false);
    }

  SPI_LOCK(priv->spi, false);
}

/****************************************************************************
 * Name: ws2812_measure_clock
 *
 * Description:
 *   Measure the SCLK the controller really produces, by timing a burst of
 *   zero bytes against the system tick while the bus is locked and the
 *   strip is selected.  The caller must have programmed the mode, word
 *   size and requested clock already.
 *
 *   The result is only as precise as the tick (about one percent over the
 *   90 ms window), which is more than enough to pick the bit codes: the
 *   datasheet windows they must land in are tens of percent wide.
 *
 * Returned Value:
 *   Measured SCLK in Hz, or 0 if the measurement is unusable.
 *
 ****************************************************************************/

static uint32_t ws2812_measure_clock(FAR struct ws2812_dev_s *priv)
{
  clock_t start;
  clock_t elapsed;
  uint64_t bytes = 0;
  uint32_t freq;

  memset(priv->txbuf, 0, WS2812_CAL_BYTES);

  start = clock_systime_ticks();

  do
    {
      SPI_SNDBLOCK(priv->spi, priv->txbuf, WS2812_CAL_BYTES);
      bytes += WS2812_CAL_BYTES;
      elapsed = clock_systime_ticks() - start;
    }
  while (elapsed < MSEC2TICK(WS2812_CAL_MS));

  if (elapsed <= 0)
    {
      return 0;
    }

  /* One SPI byte is eight SCLK periods, and a tick is 1/CLK_TCK seconds. */

  freq = (uint32_t)((bytes * 8 * CLK_TCK) / (uint64_t)elapsed);
  return freq;
}

/****************************************************************************
 * Name: ws2812_bit_code
 *
 * Description:
 *   Build the SPI byte that encodes one WS2812 bit: a run of high bits
 *   starting at bit 6, long enough to represent 'target_ns' at the given
 *   SPI clock.  Bit 7 is always 0 so MOSI is low after the last clock and
 *   the run never touches the byte boundary.
 *
 * Input Parameters:
 *   freq      - SPI clock actually produced by the controller, in Hz.
 *   target_ns - Desired high time, in nanoseconds.
 *
 * Returned Value:
 *   The SPI byte to shift out for that WS2812 bit.
 *
 ****************************************************************************/

static uint8_t ws2812_bit_code(uint32_t freq, uint32_t target_ns)
{
  uint32_t bits =
      (uint32_t)(((uint64_t)freq * target_ns + 500000000u) / 1000000000u);

  if (bits < 1)
    {
      bits = 1;
    }
  else if (bits > 7)
    {
      bits = 7;
    }

  return (uint8_t)(((1u << bits) - 1u) << (7 - bits));
}

/****************************************************************************
 * Name: ws2812_reset_bytes
 *
 * Description:
 *   Number of zero bytes that hold the data line low for WS2812_RESET_US
 *   at the given SPI clock.  One byte is eight SPI bit times.
 *
 ****************************************************************************/

static size_t ws2812_reset_bytes(uint32_t freq)
{
  uint32_t bytes = (uint32_t)(((uint64_t)freq * WS2812_RESET_US) / 8000000u);

  if (bytes < 1)
    {
      bytes = 1;
    }
  else if (bytes > WS2812_RST_MAX)
    {
      bytes = WS2812_RST_MAX;
    }

  return bytes;
}

/****************************************************************************
 * Name: ws2812_pack
 *
 * Description:
 *   Expand one 24-bit RGB value into the SPI bytes that shift it out as a
 *   WS2812 GRB sequence (the strips expect green first).
 *
 * Input Parameters:
 *   buf  - Location to write WS2812_BYTES_PER_LED bytes.
 *   rgb  - Color in 0x00RRGGBB host byte ordering.
 *   zero - SPI byte for a WS2812 0 bit.
 *   one  - SPI byte for a WS2812 1 bit.
 *
 ****************************************************************************/

static void ws2812_pack(FAR uint8_t *buf, uint32_t rgb, uint8_t zero,
                        uint8_t one)
{
  uint8_t bit_idx;
  uint8_t byte_idx;
  uint8_t offset = 0;
  uint8_t color;
  uint32_t grb;

  grb = (rgb & 0x00ff00) << 8;
  grb |= (rgb & 0xff0000) >> 8;
  grb |= rgb & 0x0000ff;

  for (byte_idx = 0; byte_idx < 3; byte_idx++)
    {
      color = (uint8_t)(grb >> (8 * (2 - byte_idx)));
      for (bit_idx = 0; bit_idx < 8; bit_idx++)
        {
          buf[offset++] = (color & (1 << (7 - bit_idx))) ? one : zero;
        }
    }
}

/****************************************************************************
 * Name: ws2812_read
 *
 * Description:
 *   Report the clock and the bit codes the last frame was built from, as
 *   text, so the timing can be checked on the board without a debugger.
 *   There is no pixel state to read back: this is all the device has to
 *   say.
 *
 ****************************************************************************/

static ssize_t ws2812_read(FAR struct file *filep, FAR char *buffer,
                           size_t buflen)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct ws2812_dev_s *priv;
  char text[64];
  ssize_t len;

  DEBUGASSERT(inode != NULL && inode->i_private != NULL);
  priv = inode->i_private;

  nxmutex_lock(&priv->lock);

  len = snprintf(text, sizeof(text), "freq=%lu zero=%u one=%u rst=%u\n",
                 (unsigned long)priv->freq, priv->zero, priv->one, priv->rst);

  nxmutex_unlock(&priv->lock);

  if (len <= 0)
    {
      return -EIO;
    }

  if (filep->f_pos >= len)
    {
      return 0;
    }

  if (buflen > (size_t)(len - filep->f_pos))
    {
      buflen = len - filep->f_pos;
    }

  memcpy(buffer, text + filep->f_pos, buflen);
  filep->f_pos += buflen;

  return (ssize_t)buflen;
}

/****************************************************************************
 * Name: ws2812_write
 *
 * Description:
 *   Pack the supplied pixels into one frame and transmit it before
 *   returning.  Nothing is retained for a later call: the frame always
 *   starts at LED 0, and each write() carries the whole chain.
 *
 *   The buffer is an array of 32-bit pixels, 0x00RRGGBB in host byte
 *   ordering, and its length must be a non-zero multiple of
 *   WS2812_PIXEL_SIZE.
 *
 ****************************************************************************/

static ssize_t ws2812_write(FAR struct file *filep, FAR const char *buffer,
                            size_t buflen)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct ws2812_dev_s *priv = inode->i_private;
  FAR uint8_t *tx;
  uint32_t freq;
  uint8_t zero;
  uint8_t one;
  size_t npixels;
  size_t rst;
  size_t frame_len;
  size_t i;

  if (buffer == NULL || buflen == 0 || (buflen % WS2812_PIXEL_SIZE) != 0)
    {
      lederr("ERROR: write length %zu is not a non-zero multiple of %d\n",
             buflen, WS2812_PIXEL_SIZE);
      return -EINVAL;
    }

  npixels = buflen / WS2812_PIXEL_SIZE;

  if (npixels > CONFIG_WS2812_CHARDEV_MAX_PIXELS)
    {
      lederr("ERROR: %zu pixels exceed the %d pixel limit\n", npixels,
             CONFIG_WS2812_CHARDEV_MAX_PIXELS);
      return -EFBIG;
    }

  nxmutex_lock(&priv->lock);

  ws2812_lock(priv);

  /* The bus may have been used by another strip since the last frame (or by
   * a non-WS2812 device on the same controller), so program our own mode,
   * word size and clock every time.  Use the clock the controller reports
   * back, not the requested one: the encoding below depends on it.
   */

  SPI_SETMODE(priv->spi, SPIDEV_MODE3);
  SPI_SETBITS(priv->spi, 8);

  freq = SPI_SETFREQUENCY(priv->spi, CONFIG_WS2812_CHARDEV_FREQUENCY);
  if (freq == 0)
    {
      freq = CONFIG_WS2812_CHARDEV_FREQUENCY;
    }

  /* The figure SPI_SETFREQUENCY() returns is the SPI lower-half's own
   * model; prefer the rate measured at registration, which is what the
   * strip actually sees.
   */

  if (priv->freq != 0)
    {
      freq = priv->freq;
    }

  /* One SPI byte per WS2812 bit only gives usable high times in this range:
   * below it the high pulses are too long to be codes, above it they are
   * too short to be seen.
   */

  if (freq < 3600000 || freq > 9000000)
    {
      lederr("ERROR: SPI clock %lu Hz outside the 3.6-9 MHz WS2812 range\n",
             (unsigned long)freq);
      ws2812_unlock(priv);
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  zero = ws2812_bit_code(freq, WS2812_T0H_TARGET_NS);
  one = ws2812_bit_code(freq, WS2812_T1H_TARGET_NS);
  rst = ws2812_reset_bytes(freq);

  priv->zero = zero;
  priv->one = one;
  priv->rst = (uint16_t)rst;

  memset(priv->txbuf, 0, rst);
  tx = priv->txbuf + rst;

  for (i = 0; i < npixels; i++)
    {
      uint32_t rgb;

      memcpy(&rgb, buffer + i * WS2812_PIXEL_SIZE, sizeof(rgb));
      ws2812_pack(tx, rgb & 0xffffff, zero, one);
      tx += WS2812_BYTES_PER_LED;
    }

  memset(tx, 0, rst);

  frame_len = rst + npixels * WS2812_BYTES_PER_LED + rst;

  SPI_SNDBLOCK(priv->spi, priv->txbuf, frame_len);

  ws2812_unlock(priv);

  nxmutex_unlock(&priv->lock);

  return buflen;
}

/****************************************************************************
 * Name: ws2812_seek
 *
 * Description:
 *   Seeking is meaningless for a device that transmits every frame from
 *   LED 0, so it is rejected rather than silently accepted (an absent seek
 *   method would let the VFS update f_pos, which the driver ignores).
 *
 ****************************************************************************/

static off_t ws2812_seek(FAR struct file *filep, off_t offset, int whence)
{
  return -ESPIPE;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ws2812_register
 *
 * Description:
 *   Register a WS2812 character device at 'path'.  See ws2812.h for the
 *   full contract.
 *
 ****************************************************************************/

int ws2812_register(FAR const char *path, FAR struct spi_dev_s *spi,
                    ws2812_select_cb_t select_cb)
{
  FAR struct ws2812_dev_s *priv;
  int ret;

  if (path == NULL || spi == NULL)
    {
      return -EINVAL;
    }

  priv = kmm_zalloc(sizeof(struct ws2812_dev_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->txbuf = kmm_zalloc(WS2812_TXBUFSIZE);
  if (priv->txbuf == NULL)
    {
      kmm_free(priv);
      return -ENOMEM;
    }

  priv->spi = spi;
  priv->select_cb = select_cb;
  nxmutex_init(&priv->lock);

  /* Measure the clock once, while nothing else is using the bus.  The
   * burst is zero bytes, so the strip sees a long low, which is exactly a
   * reset; no LED state can be disturbed.
   */

  nxmutex_lock(&priv->lock);
  ws2812_lock(priv);

  SPI_SETMODE(priv->spi, SPIDEV_MODE3);
  SPI_SETBITS(priv->spi, 8);
  SPI_SETFREQUENCY(priv->spi, CONFIG_WS2812_CHARDEV_FREQUENCY);
  priv->freq = ws2812_measure_clock(priv);

  ws2812_unlock(priv);
  nxmutex_unlock(&priv->lock);

  if (priv->freq == 0)
    {
      lederr("ERROR: could not measure the SPI clock; "
             "falling back to the reported rate\n");
    }

  ret = register_driver(path, &g_ws2812_fops, 0666, priv);
  if (ret < 0)
    {
      lederr("ERROR: register_driver(%s) failed: %d\n", path, ret);
      nxmutex_destroy(&priv->lock);
      kmm_free(priv->txbuf);
      kmm_free(priv);
      return ret;
    }

  return OK;
}

#endif /* CONFIG_WS2812_CHARDEV */
