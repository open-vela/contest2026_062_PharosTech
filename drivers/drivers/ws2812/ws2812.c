/****************************************************************************
 * drivers/drivers/ws2812/ws2812.c
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
 * eight times the strip's bit rate.  The strip is a single daisy chain: the
 * bytes go out on one wire, and each LED takes the first 24 bits that reach
 * it, re-times them, and passes the rest down the chain.  One frame is
 * therefore the whole chain, first LED to last, and has to be written out
 * in one piece.
 *
 * The bit encoding is derived at run time from the rate the SPI lower-half
 * reports for the transfer, not from the rate the board asked for (see
 * ws2812_bit_code()).  That report comes from the clock tree and the
 * divider the lower-half programmed, so it is the rate the strip sees, and
 * it is the only clock this driver has: if it disagrees with the wire, the
 * clock tree or the SPI driver is wrong, and a second guess here would only
 * move the error into the waveform.
 *
 * Unlike the in-tree NuttX driver this one never keeps a shadow buffer and
 * never holds the SPI bus between writes:
 *
 *   write()  pack the caller's pixels, lock the bus, notify the transfer
 *            callback, transmit, un-notify, unlock.
 *
 * Every write() is a self-contained frame: nothing is carried over between
 * calls and the device has no f_pos-addressed partial update, which is why
 * lseek() is rejected.
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
 * time from the rate the SPI lower-half reports for the transfer, not from
 * the requested one: the lower-half can only pick an even divider on a
 * fixed set of sources and may land below the request, and it reports what
 * it reached.
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

/* The SCLK range in which one byte per bit still encodes usable WS2812
 * high times: below it the high pulses are too long to be codes, above it
 * they are too short to be seen.  write() rejects anything outside it.
 */

#define WS2812_FREQ_MIN 3600000
#define WS2812_FREQ_MAX 9000000

/* Reset run for the fastest clock the driver accepts, in bytes.  This cap
 * and the range check in write() share WS2812_FREQ_MAX: reset_bytes()
 * clamps to the cap, so a frame can never outgrow WS2812_TXBUFSIZE.  Change
 * them together.
 */

#define WS2812_RST_MAX \
  (((uint64_t)WS2812_FREQ_MAX * WS2812_RESET_US) / 8000000ull)

/* One LED is 24 WS2812 bits, one SPI byte per bit. */

#define WS2812_BYTES_PER_LED (8 * 3)

/* Frame layout: leading reset run, up to MAX_PIXELS LEDs, trailing reset
 * run.  The trailing run is the load-bearing one: it latches the frame
 * before write() returns, independently of how the data line behaves once
 * the transfer is over.
 *
 * The leading run is belt and braces.  The board parks the data line low
 * between frames, so the strip should already be in reset when a frame
 * starts; the run stays because 300 us is cheap next to being wrong about
 * that, and it costs the same whether the line needed it or not.
 */

#define WS2812_TXBUFSIZE \
  (2 * WS2812_RST_MAX +  \
   CONFIG_WS2812_CHARDEV_MAX_PIXELS * WS2812_BYTES_PER_LED)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ws2812_dev_s
{
  FAR struct spi_dev_s *spi; /* SPI lower-half owned by the board */
  ws2812_xfer_cb_t xfer_cb;  /* Optional transfer notification */
  FAR uint8_t *txbuf;        /* Transmit scratch (not a cache) */
  mutex_t lock;              /* Serializes write() on this device */
  uint32_t freq;             /* SCLK the last frame was built from */
  uint8_t zero;              /* High-run bit count of the last '0' */
  uint8_t one;               /* High-run bit count of the last '1' */
  uint16_t rst;              /* Reset bytes of the last frame */
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
 * Name: ws2812_bus_acquire
 *
 * Description:
 *   Take the SPI bus and let the board prepare the data line for a frame.
 *   This is the single place where the driver couples to the bus mutex, so
 *   the callback always runs with the bus guaranteed to stay with this
 *   device until ws2812_bus_release().
 *
 ****************************************************************************/

static void ws2812_bus_acquire(FAR struct ws2812_dev_s *priv)
{
  SPI_LOCK(priv->spi, true);

  if (priv->xfer_cb != NULL)
    {
      priv->xfer_cb(true);
    }
}

/****************************************************************************
 * Name: ws2812_bus_release
 *
 * Description:
 *   Let the board settle the data line, then give the SPI bus back.  This
 *   is the counterpart of ws2812_bus_acquire().
 *
 ****************************************************************************/

static void ws2812_bus_release(FAR struct ws2812_dev_s *priv)
{
  if (priv->xfer_cb != NULL)
    {
      priv->xfer_cb(false);
    }

  SPI_LOCK(priv->spi, false);
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
 *   The clock is the rate the SPI lower-half reported for that frame.  All
 *   four fields read 0 until the first write(), since nothing has been
 *   built yet.  There is no pixel state to read back: this is all the
 *   device has to say.
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
 *   starts at LED 0.  A buffer shorter than the chain updates only its
 *   head -- the LEDs behind it receive no bits and keep the colors they
 *   latched last time -- so a caller that wants the whole strip to change
 *   supplies the whole chain.
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

  ws2812_bus_acquire(priv);

  /* Program mode, word size and clock before every frame: the SPI
   * lower-half makes no promise that any of them survive between
   * transactions.  Keep this per frame -- rk3576_spi_setfrequency() has to
   * drop SSIENR around the divider write to be obeyed at all, and this
   * call is the only thing that exercises that path.
   */

  SPI_SETMODE(priv->spi, SPIDEV_MODE3);
  SPI_SETBITS(priv->spi, 8);

  freq = SPI_SETFREQUENCY(priv->spi, CONFIG_WS2812_CHARDEV_FREQUENCY);
  if (freq == 0)
    {
      /* Not even the rate is known, so neither the bit codes nor the reset
       * run can be built.  This is a lower-half fault, and guessing the
       * request in its place would hide it behind a wrong waveform.
       */

      lederr("ERROR: SPI clock for %d Hz is unknown\n",
             CONFIG_WS2812_CHARDEV_FREQUENCY);
      ws2812_bus_release(priv);
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  /* That figure is the rate the controller produces: the lower-half derives
   * it from its clock tree and the divider it programmed, so the bit codes
   * and the reset run are built from it and nothing else.  There is
   * deliberately no second opinion here, because a wrong report is an SPI
   * or clock-tree bug, and compensating for it in this driver would only
   * move the error into the waveform.
   *
   * One SPI byte per WS2812 bit only gives usable high times in the range
   * WS2812_FREQ_MIN..WS2812_FREQ_MAX.
   */

  if (freq < WS2812_FREQ_MIN || freq > WS2812_FREQ_MAX)
    {
      lederr("ERROR: SPI clock %lu Hz outside the %lu-%lu Hz WS2812 range\n",
             (unsigned long)freq, (unsigned long)WS2812_FREQ_MIN,
             (unsigned long)WS2812_FREQ_MAX);
      ws2812_bus_release(priv);
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  zero = ws2812_bit_code(freq, WS2812_T0H_TARGET_NS);
  one = ws2812_bit_code(freq, WS2812_T1H_TARGET_NS);
  rst = ws2812_reset_bytes(freq);

  priv->freq = freq;
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

  ws2812_bus_release(priv);

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
                    ws2812_xfer_cb_t xfer_cb)
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
  priv->xfer_cb = xfer_cb;
  nxmutex_init(&priv->lock);

  /* Nothing is programmed here and the bus is not touched: write() sets the
   * mode, the word size and the clock before every frame, because the
   * lower-half promises none of them survive between transactions.  The
   * frame buffer is sized for the largest frame write() accepts, so no
   * separate scratch is needed.
   */

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
