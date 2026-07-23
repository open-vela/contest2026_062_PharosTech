/****************************************************************************
 * chips/rk3576/rk3576_dma_alloc.c
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
 * RK3576 DMA alloc.
 *
 * Reserves 16MB from the head of Bank2 (0x49400000..0x4A400000),
 * physically contiguous and always within the PL330 DMA 32-bit SAR/DAR
 * range.  Managed by the NuttX granule allocator with 64B granules and
 * 64B alignment (ARMv8-A D-Cache line size).
 *
 * The MMU maps this region with a flat (identity) mapping, so
 * phys == virt.
 *
 * Note: gran_initialize builds its metadata in-place at the start of the
 * heap; the remaining space is available for gran_alloc().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdint.h>

#include <nuttx/mm/gran.h>

#include <arch/chip/chip.h>

#include "rk3576_dma_alloc.h"

#ifdef CONFIG_RK3576_DMA_ALLOC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DMA buffers must be aligned to the D-Cache line for cache maintenance. */

#define DMA_ALIGN       64
#define DMA_ALIGN_MASK  (DMA_ALIGN - 1)
#define DMA_ALIGN_UP(n) (((n) + DMA_ALIGN_MASK) & ~DMA_ALIGN_MASK)

/* Granule allocator alignment and granule size (log2).  64B = 2^6. */

#define DMA_GRAN_LOG2 6

/****************************************************************************
 * Private Data
 ****************************************************************************/

static GRAN_HANDLE g_dma_heap;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dma_alloc_init
 ****************************************************************************/

int rk3576_dma_alloc_init(void)
{
  /* Initialise the granule allocator at RK3576_DMA_HEAP_ADDR.
   * The MMU already maps this region as normal memory (phys == virt).
   */

  g_dma_heap =
      gran_initialize((void *)RK3576_DMA_HEAP_ADDR, RK3576_DMA_HEAP_SIZE,
                      DMA_GRAN_LOG2, DMA_GRAN_LOG2);

  if (g_dma_heap == NULL)
    {
      _err("rk3576_dma_alloc_init: gran_initialize(%p, %u) failed\n",
           (void *)RK3576_DMA_HEAP_ADDR, RK3576_DMA_HEAP_SIZE);
      return -ENOMEM;
    }

  _info("rk3576_dma_alloc_init: DMA heap %p..%p (%u bytes)\n",
        (void *)RK3576_DMA_HEAP_ADDR,
        (void *)(RK3576_DMA_HEAP_ADDR + RK3576_DMA_HEAP_SIZE),
        RK3576_DMA_HEAP_SIZE);

  return OK;
}

/****************************************************************************
 * Name: rk3576_dma_alloc
 ****************************************************************************/

void *rk3576_dma_alloc(size_t size)
{
  if (g_dma_heap == NULL)
    {
      return NULL;
    }

  return gran_alloc(g_dma_heap, size);
}

/****************************************************************************
 * Name: rk3576_dma_free
 ****************************************************************************/

void rk3576_dma_free(void *memory, size_t size)
{
  if (g_dma_heap != NULL && memory != NULL)
    {
      gran_free(g_dma_heap, memory, size);
    }
}

#endif /* CONFIG_RK3576_DMA_ALLOC */
