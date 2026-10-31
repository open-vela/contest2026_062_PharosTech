/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_boot.c
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

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdint.h>

#include <nuttx/cache.h>
#ifdef CONFIG_LEGACY_PAGING
#include <nuttx/page.h>
#endif

#include <nuttx/kmalloc.h>

#include <arch/chip/chip.h>

#ifdef CONFIG_SMP
#include "arm64_smp.h"
#endif

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "arm64_mmu.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_boot.h"
#include "rk3576_serial.h"
#ifdef CONFIG_RK3576_SHMEM
#include <arch/chip/rk3576_shmem.h>
#endif

#ifdef CONFIG_RK3576_DMA_ALLOC
#include "rk3576_dma_alloc.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_LITTLE_NCPUS 4

#if defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > RK3576_LITTLE_NCPUS
#error "RK3576 SMP currently supports only the four LITTLE CPUs"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct arm_mmu_region g_mmu_regions[] = {
  MMU_REGION_FLAT_ENTRY("DEVICE_REGION", CONFIG_DEVICEIO_BASEADDR,
                        CONFIG_DEVICEIO_SIZE,
                        MT_DEVICE_NGNRNE | MT_RW | MT_SECURE),

  MMU_REGION_FLAT_ENTRY("DRAM0_BANK1", CONFIG_RAMBANK1_ADDR,
                        CONFIG_RAMBANK1_SIZE, MT_NORMAL | MT_RW | MT_SECURE),

#ifdef CONFIG_RK3576_SHMEM

  /* The AMP shared region lies inside BANK1 and must be remapped
   * non-cacheable: the compute domain maps the same pages with plain ioremap,
   * and sharing one physical page under two cacheabilities is undefined
   * behaviour.  It is 2 MiB aligned and exactly two 2 MiB blocks long, so it
   * can be carved out without splitting the surrounding block mapping.
   */

  MMU_REGION_FLAT_ENTRY("AMP_SHMEM", RK3576_SHMEM_BASE, RK3576_SHMEM_SIZE,
                        MT_NORMAL_NC | MT_RW | MT_SECURE),
#endif

#ifdef CONFIG_RK3576_DMA_ALLOC
  MMU_REGION_FLAT_ENTRY("DMA_HEAP", RK3576_DMA_HEAP_ADDR, RK3576_DMA_HEAP_SIZE,
                        MT_NORMAL | MT_RW | MT_SECURE),
#endif

  MMU_REGION_FLAT_ENTRY("DRAM0_BANK2", CONFIG_RAMBANK2_ADDR,
                        CONFIG_RAMBANK2_SIZE, MT_NORMAL | MT_RW | MT_SECURE),

#if defined(CONFIG_RK3576_RAMBANK2_ADDR) && CONFIG_RK3576_RAMBANK2_ADDR != 0
  /* With the second bank confined elsewhere, nothing above covers the RAM
   * the image itself runs from when that lies outside bank 1 (AMP: the
   * carve-out at 0x4a400000).  Its primary heap and the idle stack live
   * there, past the sections the MMU code maps on its own.
   */

  MMU_REGION_FLAT_ENTRY("DRAM_KERNEL", CONFIG_RAM_START, CONFIG_RAM_SIZE,
                        MT_NORMAL | MT_RW | MT_SECURE),
#endif
};

const struct arm_mmu_config g_mmu_config = {
  .num_regions = nitems(g_mmu_regions),
  .mmu_regions = g_mmu_regions,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm64_el_init
 *
 * Description:
 *   The function called from arm64_head.S at very early stage for these
 * platform, it's use to:
 *   - Handling special hardware initialize routine which is need to
 *     run at high ELs
 *   - Initialize system software such as hypervisor or security firmware
 *     which is need to run at high ELs
 *
 ****************************************************************************/

void arm64_el_init(void)
{
  uint64_t el = arm64_current_el();

  /* If we are entered at EL3 (boot chain without BL31), cntfrq_el0 is
   * uninitialized and it is only writable at EL3.  The Rockchip generic
   * timer counts from the fixed 24 MHz oscillator.  With BL31 in the
   * chain (EL2/EL1 entry) the firmware has already programmed it and
   * this is a no-op.
   */

  if (el == 3)
    {
      write_sysreg(CONFIG_RK3576_OSC_FREQ, cntfrq_el0);
      UP_ISB();
    }
}

#ifdef CONFIG_SMP
/****************************************************************************
 * Name: arm64_get_mpid
 *
 * Description:
 *   Map logical CPUs to the RK3576 LITTLE cluster.  Cross-cluster SMP is
 *   not supported by this mapping.
 ****************************************************************************/

uint64_t arm64_get_mpid(int cpu)
{
  DEBUGASSERT(cpu >= 0 && cpu < CONFIG_SMP_NCPUS);
  return cpu >= 0 && cpu < CONFIG_SMP_NCPUS ? (uint64_t)cpu : UINT64_MAX;
}
#endif

/****************************************************************************
 * Name: arm64_chip_boot
 *
 * Description:
 *   Complete boot operations started in arm64_head.S
 *
 ****************************************************************************/

void arm64_chip_boot(void)
{
  /* MAP IO and DRAM, enable MMU. */

  arm64_mmu_init(true);

#if defined(CONFIG_ARM64_PSCI)
  arm64_psci_init("smc");

#endif

  /* Perform board-specific device initialization. This would include
   * configuration of board specific resources such as GPIOs, LEDs, etc.
   */

  rk3576_board_initialize();

#ifdef USE_EARLYSERIALINIT
  /* Perform early serial initialization if we are going to use the serial
   * driver.
   */

  arm64_earlyserialinit();
#endif
}

#if defined(CONFIG_NET) && !defined(CONFIG_NETDEV_LATEINIT)
void arm64_netinitialize(void)
{ /* TODO: Support net initialize */
}
#endif

/****************************************************************************
 * Name: arm64_addregion
 *
 * Description:
 *   Add the second DRAM bank (above OP-TEE) to the user heap.  This is
 *   called from up_initialize() when CONFIG_MM_REGIONS > 1.
 *
 *   Bank2 is split: the first 16MB (RK3576_DMA_HEAP) is reserved for DMA
 *   and managed by the granule allocator; the remainder is added to the
 *   user heap.
 *
 ****************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm64_addregion(void)
{
#ifdef CONFIG_RK3576_DMA_ALLOC
  {
    int ret;

    /* Initialise the DMA heap before adding Bank2 to the user heap.
     * If kumm_addregion ran first, the heap manager could allocate
     * from Bank2 and clobber the DMA region.
     */

    ret = rk3576_dma_alloc_init();
    if (ret < 0)
      {
        _err("arm64_addregion: DMA heap init failed: %d\n", ret);
      }
  }
#endif /* CONFIG_RK3576_DMA_ALLOC */

  kumm_addregion((void *)CONFIG_RAMBANK2_ADDR, CONFIG_RAMBANK2_SIZE);
}
#endif /* CONFIG_MM_REGIONS > 1 */
