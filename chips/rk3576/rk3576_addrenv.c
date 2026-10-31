/****************************************************************************
 * chips/rk3576/rk3576_addrenv.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <sys/types.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "arm64_mmu.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* arm64 4K-granule, 4-level page table constants (ARMv8-A).
 *
 * Each level covers 9 bits of the VA (512 entries per table).  Level 3
 * maps 4 KB pages; level 2 maps 2 MB blocks; level 1 maps 1 GB blocks.
 *
 *   Level | Block size         | VA shift
 *   ------+--------------------+---------
 *     0   | 512 GB (2^39)      |   39
 *     1   |   1 GB (2^30)      |   30
 *     2   |   2 MB (2^21)      |   21
 *     3   |   4 KB (2^12)      |   12
 *
 * LEVEL_SHIFT(l) = (3 - l) * 9 + 12
 */

#define RK3576_MMU_LEVEL_SHIFT(l)  (((3U - (l)) * 9U) + 12U)
#define RK3576_MMU_LEVEL_SIZE(l)   (UINT64_C(1) << RK3576_MMU_LEVEL_SHIFT(l))
#define RK3576_MMU_LEVEL_OFFSET(l) (RK3576_MMU_LEVEL_SIZE(l) - 1U)
#define RK3576_MMU_OA_MASK         UINT64_C(0x0000fffffffff000)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_addrenv_pa_to_va
 *
 * Description:
 *   Convert an identity-mapped DMA physical address back to a CPU pointer.
 *   RK3576 flat builds keep DMA buffers identity mapped.
 *
 ****************************************************************************/

FAR void *up_addrenv_pa_to_va(uintptr_t paddr) { return (FAR void *)paddr; }

/****************************************************************************
 * Name: up_addrenv_va_to_pa
 *
 * Description:
 *   Translate a kernel virtual address to the corresponding physical
 *   address by walking the current MMU page tables.
 *
 *   In flat address space (CONFIG_BUILD_FLAT, the default for rk3576) the
 *   MMU maps every kernel VA identically to PA, so this is a no-op.  When
 *   CONFIG_BUILD_KERNEL is enabled the kernel runs with a non-trivial VA
 *   space and DMA hardware (which operates on PAs) must receive translated
 *   addresses.
 *
 * Input Parameters:
 *   vaddr - Kernel virtual address to translate.
 *
 * Returned Value:
 *   The physical address corresponding to vaddr, or vaddr itself if no
 *   mapping is found (e.g., already a physical address or unmapped region).
 *
 ****************************************************************************/

uintptr_t up_addrenv_va_to_pa(FAR void *vaddr)
{
#ifndef CONFIG_BUILD_KERNEL

  /* Flat address space: VA == PA everywhere. */

  return (uintptr_t)vaddr;

#else /* CONFIG_BUILD_KERNEL */

  uintptr_t va = (uintptr_t)vaddr;
  uintptr_t table; /* VA of the current-level page table */
  uint32_t level;
  uintptr_t entry;
  uintptr_t type;
  uintptr_t shift;

  /* g_kernel_pgt_pbase is the physical base of the kernel page table set.
   * In the kernel virtual address space the page tables themselves reside
   * in an identity-mapped region, so their PA can be used directly as a
   * VA to read through the C pointer.
   */

  table = g_kernel_pgt_pbase;

  for (level = mmu_get_base_pgt_level(); level <= MMU_PGT_LEVEL_MAX; level++)
    {
      entry = mmu_ln_getentry(level, table, va);
      type = entry & PTE_DESC_TYPE_MASK;

      if (type == 0)
        {
          /* Invalid (unmapped) — return the original address unchanged. */

          return va;
        }

      shift = RK3576_MMU_LEVEL_SHIFT(level);

      if (type == PTE_BLOCK_DESC ||
          (type == PTE_PAGE_DESC && level == MMU_PGT_LEVEL_MAX))
        {
          /* Block descriptor (L0–L2) or page descriptor (L3):
           * Output address = entry[47:shift], offset = VA[shift-1:0].
           */

          uintptr_t oa_aligned_mask =
              (uintptr_t)(~((UINT64_C(1) << shift) - 1U)) &
              (uintptr_t)RK3576_MMU_OA_MASK;

          return (entry & oa_aligned_mask) |
                 (va & RK3576_MMU_LEVEL_OFFSET(level));
        }

      /* Table descriptor: descend to the next level.
       * The next-level table physical address is in bits [47:12].
       * Treat it as a VA (valid in the identity-mapped page-table region).
       */

      table = entry & (uintptr_t)RK3576_MMU_OA_MASK;
    }

  /* Walked past level 3 without a terminal descriptor — should not happen
   * with a well-formed page table.  Return vaddr unchanged.
   */

  return va;

#endif /* CONFIG_BUILD_KERNEL */
}
