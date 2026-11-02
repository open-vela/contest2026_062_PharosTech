/****************************************************************************
 * drivers/include/ws2812.h
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
 * WS2812 addressable LED character driver public interface.
 *
 * This driver is the kernel-side "upper half" for WS2812-family strips that
 * are driven over SPI: each WS2812 bit is encoded as one SPI byte, exactly
 * like the in-tree NuttX driver.  Unlike the in-tree driver it keeps no
 * pixel shadow buffer and holds no permanent claim on the SPI bus:
 *
 *   - every write() carries a complete frame and is transmitted
 *     immediately, in write() order, with nothing carried over between
 *     calls;
 *   - the SPI bus is locked only for the duration of one frame;
 *   - a transfer callback is invoked around each frame so the board can
 *     put the data pin on the SPI function and take it back afterwards.
 ****************************************************************************/

#ifndef __BOARDS_RK3576_DRIVERS_INCLUDE_WS2812_H
#define __BOARDS_RK3576_DRIVERS_INCLUDE_WS2812_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/spi/spi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Pixel representation: one 32-bit word per pixel holding 24 bits of color
 * in host byte ordering, 0x00RRGGBB.  This is the same layout the in-tree
 * NuttX WS2812 driver uses, so application code can be shared between the
 * two.
 *
 * A write() must carry a whole number of pixels.  It is one frame of a
 * daisy chain: pixel 0 drives the first LED, pixel 1 the second, and so on
 * down the wire.  A short write therefore updates only the head of the
 * chain; the LEDs behind it receive no bits and keep the colors they
 * latched last time.  That is the protocol, not a driver limitation.
 */

#define WS2812_PIXEL_SIZE 4

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Transfer notification callback.
 *
 * The driver invokes this callback around every frame, with the SPI bus
 * locked, so the board can get the data line ready before the first bit
 * leaves and settle it after the last one.  It is called with xfer == true
 * right after the bus is locked, before any byte of the frame is
 * transmitted, and with xfer == false right before the bus is unlocked.
 *
 * On this SoC the callback is what keeps the line low between frames: the
 * SPI controller releases MOSI when a transfer ends, so the pad is switched
 * to the SPI function for the transfer and back to a GPIO output driving 0
 * afterwards.  A strip sees a data line left high as a never-ending run of
 * 1 bits and latches white at the next reset.
 *
 * The callback runs with the SPI bus lock held and must not call back into
 * the driver or block for long.
 *
 * It may be NULL, but a board that parks its data line from this hook --
 * this one does -- then leaves the pin on the SPI function between frames
 * and depends on the pad pull to keep the line from floating high.
 */

typedef void (*ws2812_xfer_cb_t)(bool xfer);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_WS2812_CHARDEV

/****************************************************************************
 * Name: ws2812_register
 *
 * Description:
 *   Register a WS2812 character device at 'path'.  The caller supplies the
 *   already-initialized SPI lower-half this strip is wired to; the driver
 *   does not own the bus and does not configure any pin multiplexing.
 *
 *   Every write() packs the supplied pixels into a single frame and shifts
 *   it out before returning.  There is no buffering: the frame is not
 *   retained, and lseek() is not supported (-ESPIPE).
 *
 *   The bit codes and the reset run are derived from the rate the SPI
 *   lower-half reports for the transfer, which it computes from its clock
 *   tree and the divider it programmed.  The driver does not measure the
 *   clock itself: a report that disagrees with the wire is an SPI or
 *   clock-tree bug to fix there, not something a second estimate here
 *   could correct.  read() returns one text line with that rate and the
 *   codes the last frame used ("freq=<Hz> zero=<bits> one=<bits>
 *   rst=<bytes>"), which is how the timing can be checked on a board with
 *   no debugger.
 *
 * Input Parameters:
 *   path      - Device node path, e.g. "/dev/ws2812".
 *   spi       - SPI lower-half obtained by board code, e.g. from
 *               rk3576_spi_initialize(3).  The bus must carry WS2812
 *               traffic only: the driver reprograms its mode, word size and
 *               frequency before every frame.
 *   xfer_cb   - Optional transfer notification callback (see
 *               ws2812_xfer_cb_t).  It is invoked with true after the bus
 *               is locked and before the frame is transmitted, and with
 *               false before the bus is released.  May be NULL.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int ws2812_register(FAR const char *path, FAR struct spi_dev_s *spi,
                    ws2812_xfer_cb_t xfer_cb);

#endif /* CONFIG_WS2812_CHARDEV */

#endif /* __BOARDS_RK3576_DRIVERS_INCLUDE_WS2812_H */
