/****************************************************************************
 * boards/rk3576/drivers/include/ws2812.h
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
 *   - the SPI bus is locked only for the duration of one frame, so several
 *     strips can share one SPI controller and take turns on it;
 *   - an optional select callback is invoked around each frame so the
 *     caller can switch an external multiplexer (or do any other work) while
 *     the strip is guaranteed to own the bus.
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
 * A write() must carry a whole number of pixels.  It is one complete frame:
 * pixel 0 drives the first LED of the chain, pixel 1 the second, and so on.
 * Writing a single pixel therefore only programs the first LED, which is
 * the protocol's behavior, not a driver limitation.
 */

#define WS2812_PIXEL_SIZE 4

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Select notification callback.
 *
 * The driver invokes this callback every time it acquires ownership of the
 * SPI bus (the "selected" state), so the caller can perform custom work --
 * for example drive the select lines of an external multiplexer that routes
 * the shared SPI data line to this strip -- while the strip is guaranteed
 * to be the active device.  It is called with select == true right after
 * the bus is locked, before any byte of the frame is transmitted, and with
 * select == false right before the bus is unlocked.
 *
 * The callback runs with the SPI bus lock held, so no other WS2812 device
 * on the same bus can interleave a frame between the select change and the
 * transfer.  It must not call back into the driver or block for long.
 *
 * It may be NULL to disable the notification (for example while the
 * multiplexer hardware is not fitted yet).
 */

typedef void (*ws2812_select_cb_t)(bool select);

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
 *   The SCLK the controller really produces is measured once, at
 *   registration, and the bit codes are derived from that measurement
 *   rather than from the rate the SPI lower-half reports.  read() returns
 *   one text line with that measurement and the codes the last frame used
 *   ("freq=<Hz> zero=<bits> one=<bits> rst=<bytes>"), which is how the
 *   timing can be checked on a board with no debugger.
 *
 * Input Parameters:
 *   path      - Device node path, e.g. "/dev/ws2812".
 *   spi       - SPI lower-half obtained by board code, e.g. from
 *               rk3576_spi_initialize(3).  The bus must be dedicated to
 *               WS2812 traffic (or multiplexed externally and arbitrated by
 *               the select callback): the driver changes its mode, word
 *               size and frequency around every frame.
 *   select_cb - Optional select notification callback (see
 *               ws2812_select_cb_t).  It is invoked with true after the bus
 *               is locked and before the frame is transmitted, and with
 *               false before the bus is released.  May be NULL.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int ws2812_register(FAR const char *path, FAR struct spi_dev_s *spi,
                    ws2812_select_cb_t select_cb);

#endif /* CONFIG_WS2812_CHARDEV */

#endif /* __BOARDS_RK3576_DRIVERS_INCLUDE_WS2812_H */
