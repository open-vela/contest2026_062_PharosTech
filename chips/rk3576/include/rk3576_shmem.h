/****************************************************************************
 * arch/arm64/include/rk3576/rk3576_shmem.h
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

#ifndef __ARCH_ARM64_INCLUDE_RK3576_RK3576_SHMEM_H
#define __ARCH_ARM64_INCLUDE_RK3576_RK3576_SHMEM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_SHMEM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The shared region between the two AMP domains.  The address and size are a
 * contract with the Linux side's reserved-memory node; the firmware checks
 * the size it was built for against what the peer reports, because a mismatch
 * would corrupt data silently rather than fail loudly.
 */

#define RK3576_SHMEM_BASE 0x47c00000
#define RK3576_SHMEM_SIZE (4 * 1024 * 1024)

/* The region is mapped non-cacheable so both domains see the same bytes
 * without either side maintaining cache state.  It is 2 MiB aligned and two
 * whole 2 MiB blocks long, so it can be described as an MMU region without
 * splitting the surrounding block mapping.
 */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_shmem_initialize
 *
 * Description:
 *   Validate the shared region against the peer's view of it.  Called during
 *   board bring-up, after the compute domain has published its arena header.
 *   Returns 0 when the two sides agree, a negated errno otherwise; a mismatch
 *   is reported rather than papered over.
 *
 ****************************************************************************/

int rk3576_shmem_initialize(void);

/****************************************************************************
 * Name: rk3576_shmem_ready
 *
 * Description:
 *   Whether the arena header has been validated.  A caller that arrives
 *   before the compute domain has claimed the region can retry rather than
 *   treating the absence as a permanent failure.
 *
 ****************************************************************************/

bool rk3576_shmem_ready(void);

/****************************************************************************
 * Name: rk3576_shmem_trace
 *
 * Description:
 *   The compute domain's bring-up marker.  It has no console, so a probe
 *   that fails partway is otherwise invisible; this reports how far it got.
 *
 ****************************************************************************/

uint32_t rk3576_shmem_trace(void);

/****************************************************************************
 * Name: rk3576_shmem_reset
 *
 * Description:
 *   Clear the arena header so the compute domain claims the region again.
 *   Its driver refuses a header it does not recognise, which is correct but
 *   leaves no way back in once the header is wrong.
 *
 ****************************************************************************/

void rk3576_shmem_reset(void);

/****************************************************************************
 * Name: rk3576_shmem_base
 *
 * Description:
 *   Base address of the shared region.  Because the region is identity mapped
 *   and non-cacheable, this is directly dereferenceable.
 *
 ****************************************************************************/

void *rk3576_shmem_base(void);

/****************************************************************************
 * Name: rk3576_shmem_size
 *
 * Description:
 *   Size in bytes of the shared region.
 *
 ****************************************************************************/

size_t rk3576_shmem_size(void);

/****************************************************************************
 * Name: rk3576_shmem_generation
 *
 * Description:
 *   The compute domain's generation for the current arena contents, or zero
 *   when it has not published one.  A change means every previous grant is
 *   void.
 *
 ****************************************************************************/

uint32_t rk3576_shmem_generation(void);

#endif /* CONFIG_RK3576_SHMEM */

#endif /* __ARCH_ARM64_INCLUDE_RK3576_RK3576_SHMEM_H */
