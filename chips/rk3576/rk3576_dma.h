/****************************************************************************
 * chips/rk3576/rk3576_dma.h
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
 * RK3576 ARM PL330 (DMA-330) driver public interface.
 *
 * The controller is exposed through the generic NuttX DMA framework
 * (include/nuttx/dma/dma.h): rk3576_dma_initialize() returns a
 * struct dma_dev_s * and clients drive it with the framework's
 * DMA_GET_CHAN / DMA_CONFIG / DMA_START / DMA_STOP / DMA_PUT_CHAN macros.
 * This lets any generic consumer (e.g. audio_dma, uart_16550) reuse the
 * controller without a controller-specific API.  All three PL330 controllers
 * (dmac0/1/2) are exposed; select one with the rk3576_dma_initialize()
 * 'ctrl' argument.  The private micro-code assembler / debug-interface
 * launch stays internal to rk3576_dma.c.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_DMA_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_DMA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/dma/dma.h>

#ifdef CONFIG_RK3576_DMA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Peripheral DMA request line used as the 'ident' argument to
 * DMA_GET_CHAN() (dev->get_chan).  For a memory-to-memory channel that is
 * not bound to any peripheral request line, pass RK3576_DMA_DRQ_NONE.
 * Peripheral transfers pass the controller peripheral-request line index
 * (0..RK3576_DMA_NPERIPH-1, e.g. the SAI1 TX/RX request), which is also
 * mirrored into struct dma_config_s dst_drq (M2P) / src_drq (P2M).
 */

#define RK3576_DMA_DRQ_NONE 0xff

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dma_initialize
 *
 * Description:
 *   Return the selected RK3576 PL330 DMA controller (index 0/1/2 ->
 *   dmac0/dmac1/dmac2) as a generic struct dma_dev_s.  The controller
 *   hardware (clock/reset ungating, interrupt attach) is brought up lazily
 *   on the first DMA_GET_CHAN() against that controller.
 *
 * Input Parameters:
 *   ctrl - Controller index.
 *
 * Returned Value:
 *   A pointer to the DMA controller device, or NULL on failure.
 *
 ****************************************************************************/

struct dma_dev_s *rk3576_dma_initialize(unsigned int ctrl);

#endif /* CONFIG_RK3576_DMA */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_DMA_H */
