/****************************************************************************
 * chips/rk3576/rk3576_spi.h
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
 * RK3576 SPI (Serial Peripheral Interface) master driver public API.
 *
 * Implements the NuttX SPI lower-half interface (struct spi_dev_s /
 * struct spi_ops_s) for the Rockchip RK3576 SPI controller (a Synopsys
 * DesignWare SSI compatible IP).  Master mode only, polled FIFO transfers.
 *
 * Usage example:
 *
 *   FAR struct spi_dev_s *spi;
 *   spi = rk3576_spi_initialize(0);   // SPI0
 *   SPI_SELECT(spi, SPIDEV_FLASH(0), true);
 *   SPI_SETMODE(spi, SPIDEV_MODE0);
 *   SPI_SETBITS(spi, 8);
 *   SPI_SETFREQUENCY(spi, 10000000);
 *   SPI_EXCHANGE(spi, txbuf, rxbuf, nwords);
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/spi/spi.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_spi_initialize
 *
 * Description:
 *   Initialize one RK3576 SPI controller and return its spi_dev_s lower-half
 *   instance.  Master mode only.  The controller's PCLK and SCLK are brought
 *   up via the CRU/clock framework; SS_N, SCK, MOSI, MISO pin muxing is the
 *   board's responsibility (there is no pinctrl framework yet).
 *
 * Input Parameters:
 *   bus - The SPI controller number (0..4 for SPI0..SPI4).
 *
 * Returned Value:
 *   A pointer to the spi_dev_s lower-half on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct spi_dev_s *rk3576_spi_initialize(int bus);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H */
