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
 * Public API for the RK3576 ARM PL330 (DMA-330) driver.  The primary
 * consumer is the SAI/I2S audio driver, which moves audio samples between a
 * memory buffer and a peripheral FIFO register at a fixed address (M2P for
 * playback, P2M for capture).  A generic memory-to-memory mode is also
 * exposed.  Only single-shot (one buffer) transfers are implemented; cyclic
 * and scatter/gather transfers are deferred (see rk3576_dma.c).
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_DMA_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_DMA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stddef.h>

#ifdef CONFIG_RK3576_DMA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Transfer direction. */

#define RK3576_DMA_M2M             0     /* Memory to memory              */
#define RK3576_DMA_M2P             1     /* Memory to peripheral (TX)     */
#define RK3576_DMA_P2M             2     /* Peripheral to memory (RX)     */

/* Passed as periph_req to rk3576_dma_get_channel() for a memory-to-memory
 * channel that is not bound to any peripheral request line.
 */

#define RK3576_DMA_PERIPH_NONE     0xff

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Opaque per-channel handle. */

struct rk3576_dmach_s;

/* Completion callback.  Invoked from interrupt context when the transfer
 * finishes (result == OK) or aborts on a channel fault (result < 0, a
 * negated errno).  Keep the callback short.
 */

typedef void (*rk3576_dma_callback_t)(struct rk3576_dmach_s *ch,
                                      int result, void *arg);

/* Transfer description passed to rk3576_dma_setup().
 *
 * For a peripheral (M2P/P2M) transfer the peripheral side (the FIFO
 * register) is kept at a fixed address (no increment) and the memory side
 * increments.  src_width/dst_width are the transfer widths in bytes (1, 2,
 * 4 or 8) and must currently be equal; burst_len is the number of beats per
 * burst (1..16).  nbytes must be a whole multiple of (src_width*burst_len).
 * All addresses must lie in the low 4GB (PL330 SAR/DAR are 32-bit).
 */

struct rk3576_dma_config_s
{
  uintptr_t             src;       /* Source address                     */
  uintptr_t             dst;       /* Destination address                */
  size_t                nbytes;    /* Total byte count                   */
  uint8_t               direction; /* RK3576_DMA_M2M / M2P / P2M          */
  uint8_t               src_width; /* Source beat width in bytes          */
  uint8_t               dst_width; /* Destination beat width in bytes     */
  uint8_t               burst_len; /* Beats per burst (1..16)             */
  rk3576_dma_callback_t callback;  /* Completion callback (may be NULL)   */
  void                 *arg;       /* Callback argument                   */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dma_get_channel
 *
 * Description:
 *   Reserve a free DMA channel.  For a peripheral transfer pass the
 *   controller peripheral-request line (e.g. SAI1 TX/RX); for a plain
 *   memory-to-memory channel pass RK3576_DMA_PERIPH_NONE.
 *
 * Returned Value:
 *   A channel handle on success, NULL if none are free.
 *
 ****************************************************************************/

struct rk3576_dmach_s *rk3576_dma_get_channel(unsigned int periph_req);

/****************************************************************************
 * Name: rk3576_dma_free_channel
 *
 * Description:
 *   Stop and release a channel previously returned by
 *   rk3576_dma_get_channel().
 *
 ****************************************************************************/

void rk3576_dma_free_channel(struct rk3576_dmach_s *ch);

/****************************************************************************
 * Name: rk3576_dma_setup
 *
 * Description:
 *   Assemble the PL330 micro-code program for the requested transfer.  Does
 *   not start it; call rk3576_dma_start() afterwards.
 *
 * Returned Value:
 *   OK on success, a negated errno on error.
 *
 ****************************************************************************/

int rk3576_dma_setup(struct rk3576_dmach_s *ch,
                     const struct rk3576_dma_config_s *config);

/****************************************************************************
 * Name: rk3576_dma_start
 *
 * Description:
 *   Launch the channel programmed by the last rk3576_dma_setup().
 *
 * Returned Value:
 *   OK on success, a negated errno on error.
 *
 ****************************************************************************/

int rk3576_dma_start(struct rk3576_dmach_s *ch);

/****************************************************************************
 * Name: rk3576_dma_stop
 *
 * Description:
 *   Abort an in-flight transfer (issues DMAKILL to the channel).  No
 *   completion callback is delivered for a stopped transfer.
 *
 * Returned Value:
 *   OK on success, a negated errno on error.
 *
 ****************************************************************************/

int rk3576_dma_stop(struct rk3576_dmach_s *ch);

#endif /* CONFIG_RK3576_DMA */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_DMA_H */
