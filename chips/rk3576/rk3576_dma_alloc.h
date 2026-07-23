/****************************************************************************
 * chips/rk3576/rk3576_dma_alloc.h
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
 * RK3576 DMA-dedicated heap allocator: public API.
 *
 * Reserves 16MB of physically-contiguous memory from the head of Bank2
 * (RK3576_DMA_HEAP_BASE = 0x49400000), managed by the NuttX granule
 * allocator.  All allocations stay within the PL330 DMA 32-bit SAR/DAR
 * addressable range (<4GB) and honour D-cache line alignment (64B).
 *
 * Usage:
 *   rk3576_dma_alloc_init()   -- one-time init at boot
 *   rk3576_dma_alloc(size)    -- allocate DMA-safe memory
 *   rk3576_dma_free(ptr,size) -- free
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_DMA_ALLOC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_DMA_ALLOC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>

#ifdef CONFIG_RK3576_DMA_ALLOC

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dma_alloc_init
 *
 * Description:
 *   Initialize the DMA-dedicated heap (granule allocator) at
 *   RK3576_DMA_HEAP_BASE with 64B granules.  Must be called from
 *   arm64_addregion() before kumm_addregion(Bank2).
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_dma_alloc_init(void);

/****************************************************************************
 * Name: rk3576_dma_alloc
 *
 * Description:
 *   Allocate size bytes of physically-contiguous memory from the DMA heap.
 *
 * Input Parameters:
 *   size - Number of bytes to allocate
 *
 * Returned Value:
 *   Pointer to the allocated memory (phys == virt), or NULL if exhausted.
 *
 ****************************************************************************/

void *rk3576_dma_alloc(size_t size);

/****************************************************************************
 * Name: rk3576_dma_free
 *
 * Description:
 *   Free memory previously allocated by rk3576_dma_alloc().
 *
 * Input Parameters:
 *   memory - Pointer returned by rk3576_dma_alloc()
 *   size   - Size passed to the matching rk3576_dma_alloc() call
 *
 ****************************************************************************/

void rk3576_dma_free(void *memory, size_t size);

#endif /* CONFIG_RK3576_DMA_ALLOC */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_DMA_ALLOC_H */
