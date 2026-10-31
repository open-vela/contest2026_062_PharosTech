/****************************************************************************
 * chips/rk3576/rk3576_shmem.c
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
 * RK3576 AMP shared-memory region (openvela side).
 *
 * The region sits inside DRAM0_BANK1, which rk3576_boot.c maps as normal
 * cacheable memory, so the pages are already reachable.  They are remapped
 * non-cacheable here because the peer maps them with plain ioremap: two
 * domains sharing one physical page under different cacheability is
 * architecturally undefined, and it is the kind of fault that shows up as an
 * occasional stale value rather than a clean failure.
 *
 * Making both sides non-cacheable also removes the need for either side to
 * clean or invalidate anything, which is where the subtle bugs would live.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdint.h>

#include <arch/chip/rk3576_shmem_layout.h>

#include <arch/chip/rk3576_shmem.h>

#ifdef CONFIG_RK3576_SHMEM

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct nyamp_arena_s *g_arena =
    (struct nyamp_arena_s *)(uintptr_t)RK3576_SHMEM_BASE;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_shmem_initialize(void)
{
  struct nyamp_arena_s *arena = g_arena;

  /* The compute domain owns the arena and publishes the header before this
   * domain starts: the boot loader brings up Linux first and waits for its
   * first transport notification, then enters openvela on CPU0.  A header
   * that is still absent therefore means the peer has not claimed the region,
   * and a foreign header means the two sides disagree about it.  Neither is
   * something to paper over -- guessing here would corrupt audio silently.
   */

  if (arena->magic != NYAMP_ARENA_MAGIC)
    {
      _err("rk3576_shmem: no arena header at %p (magic %08lx)\n", arena,
           (unsigned long)arena->magic);
      return -ENODATA;
    }

  if (arena->version != NYAMP_ARENA_VERSION)
    {
      _err("rk3576_shmem: arena version %lu, firmware expects %u\n",
           (unsigned long)arena->version, NYAMP_ARENA_VERSION);
      return -EPROTO;
    }

  /* A size disagreement shifts every slot, so it is a hard failure and not
   * something to adapt to at run time.
   */

  if (arena->size != NYAMP_SHMEM_SIZE)
    {
      _err("rk3576_shmem: arena size %lu, firmware built for %lu\n",
           (unsigned long)arena->size, (unsigned long)NYAMP_SHMEM_SIZE);
      return -EINVAL;
    }

  ninfo("rk3576_shmem: arena ready, generation %lu\n",
        (unsigned long)arena->generation);
  return OK;
}

/****************************************************************************
 * Name: rk3576_shmem_ready
 *
 * Description:
 *   Whether the arena has been validated.  A caller that arrives before the
 *   compute domain has claimed the region can retry rather than treating the
 *   absence as a permanent failure.
 *
 ****************************************************************************/

bool rk3576_shmem_ready(void)
{
  const struct nyamp_arena_s *arena = g_arena;

  return arena->magic == NYAMP_ARENA_MAGIC &&
         arena->version == NYAMP_ARENA_VERSION &&
         arena->size == NYAMP_SHMEM_SIZE;
}

uint32_t rk3576_shmem_trace(void)
{
  return *(volatile uint32_t *)(uintptr_t)(RK3576_SHMEM_BASE +
                                           NYAMP_ARENA_TRACE_OFFSET);
}

/****************************************************************************
 * Name: rk3576_shmem_reset
 *
 * Description:
 *   Clear the arena header so the compute domain will claim the region again.
 *
 *   Its driver refuses to claim a region whose header it does not recognise,
 *   which is the right call but leaves no way back in once the header is
 *   wrong.  This is that way back, and it exists for the diagnostic command
 *   whose own pattern writing is what corrupts the header.
 *
 ****************************************************************************/

void rk3576_shmem_reset(void)
{
  volatile uint32_t *header =
      (volatile uint32_t *)(uintptr_t)RK3576_SHMEM_BASE;

  /* Clear every word the compute domain checks before it will claim the
   * region: a partially cleared header would be claimed and then fail its
   * size check.
   */
  header[NYAMP_ARENA_MAGIC_OFFSET / 4] = 0;
  header[NYAMP_ARENA_VERSION_OFFSET / 4] = 0;
  header[NYAMP_ARENA_SIZE_OFFSET / 4] = 0;
  header[NYAMP_ARENA_TRACE_OFFSET / 4] = 0;
}

void *rk3576_shmem_base(void) { return (void *)g_arena; }

size_t rk3576_shmem_size(void) { return NYAMP_SHMEM_SIZE; }

uint32_t rk3576_shmem_generation(void) { return g_arena->generation; }

#endif /* CONFIG_RK3576_SHMEM */
