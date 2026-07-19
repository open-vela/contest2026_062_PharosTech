/****************************************************************************
 * chips/rk3576/rk3576_fspi.h
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
 * RK3576 FSPI (Flexible Serial Peripheral Interface) QSPI driver public API.
 *
 * Implements the NuttX QSPI lower-half interface (struct qspi_dev_s /
 * struct qspi_ops_s) for the Rockchip RK3576 SFC/FSPI controller.
 *
 * The FSPI IP supports single/dual/quad data lanes and two chip-select lines
 * (CS0 / CS1) per controller.  Each CS has independent configuration
 * registers (CTRL, AX, ABIT, DLL, DEVSIZE, TME) but they share the same
 * FSM / FIFO / DMA engine, so access is serialized via a controller-level
 * mutex.
 *
 * Usage example:
 *
 *   // Get FSPI0 CS0 for a QSPI LCD
 *   FAR struct qspi_dev_s *qspi_lcd;
 *   qspi_lcd = rk3576_fspi_initialize(0, 0);
 *
 *   // Get FSPI0 CS1 for a NOR flash
 *   FAR struct qspi_dev_s *qspi_flash;
 *   qspi_flash = rk3576_fspi_initialize(0, 1);
 *
 *   // qspi_lcd and qspi_flash are independent qspi_dev_s instances
 *   // but are serialized when accessing the shared FSPI0 hardware.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_FSPI_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_FSPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/spi/qspi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_fspi_initialize
 *
 * Description:
 *   Initialize the selected FSPI controller / chip-select as a QSPI
 *   lower-half device.
 *
 *   RK3576 has two FSPI controllers (0 and 1), each supporting two chip
 *   selects (CS0 and CS1).  This function returns an independent
 *   qspi_dev_s for each {controller, cs} pair.  CS0 and CS1 on the same
 *   controller share the underlying FSM/FIFO/DMA and are serialized via a
 *   controller-level mutex.
 *
 * Input Parameters:
 *   fspi_id - FSPI controller index: 0 = FSPI0, 1 = FSPI1.
 *   cs      - Chip select index: 0 = CS0, 1 = CS1.
 *
 * Returned Value:
 *   Pointer to the QSPI lower-half device on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct qspi_dev_s *rk3576_fspi_initialize(int fspi_id, int cs);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_FSPI_H */
