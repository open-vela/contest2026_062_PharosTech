/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_dmaprobe.c
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
 * Bring-up self-test for the ARM PL330 DMA controller
 * (dma-controller@2ab90000).  Reserves a memory-to-memory channel, copies a
 * seeded source buffer to a cleared destination buffer, waits for the
 * completion interrupt and verifies the copy byte-for-byte.  Validates the
 * micro-code assembler, DMAGO launch and DMASEV completion interrupt on real
 * hardware before DMA is wired to the SAI/I2S audio driver.  Memory-to-memory
 * only -- no peripheral request lines are exercised here.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/semaphore.h>
#include <nuttx/cache.h>

#include "rk3576_dma.h"
#include "kickpi_k7.h"

#ifdef CONFIG_KICKPI_K7_DMA_PROBE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DMAPROBE_NBYTES   512

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Cache-line aligned so the dcache clean/invalidate around the DMA hit whole
 * lines only (no false sharing with neighbours).
 */

static uint8_t g_dmaprobe_src[DMAPROBE_NBYTES]
  aligned_data(64);
static uint8_t g_dmaprobe_dst[DMAPROBE_NBYTES]
  aligned_data(64);

static sem_t   g_dmaprobe_done = SEM_INITIALIZER(0);
static int     g_dmaprobe_result;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void kickpi_k7_dmaprobe_cb(struct rk3576_dmach_s *ch, int result,
                                  void *arg)
{
  UNUSED(ch);
  UNUSED(arg);

  g_dmaprobe_result = result;
  nxsem_post(&g_dmaprobe_done);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_dma_probe
 *
 * Description:
 *   PL330 memory-to-memory bring-up self-test (see file banner).
 *
 ****************************************************************************/

void kickpi_k7_dma_probe(void)
{
  struct rk3576_dma_config_s cfg;
  struct rk3576_dmach_s *ch;
  unsigned int i;
  int ret;

  syslog(LOG_INFO, "DMAPROBE: PL330 memory-to-memory self-test\n");

  for (i = 0; i < DMAPROBE_NBYTES; i++)
    {
      g_dmaprobe_src[i] = (uint8_t)(i ^ 0xa5);
    }

  memset(g_dmaprobe_dst, 0, DMAPROBE_NBYTES);

  /* Publish the source and cleared destination to main memory before the DMA
   * reads/overwrites them (the PL330 is not cache-coherent with the CPU).
   */

  up_clean_dcache((uintptr_t)g_dmaprobe_src,
                  (uintptr_t)g_dmaprobe_src + DMAPROBE_NBYTES);
  up_clean_dcache((uintptr_t)g_dmaprobe_dst,
                  (uintptr_t)g_dmaprobe_dst + DMAPROBE_NBYTES);

  ch = rk3576_dma_get_channel(RK3576_DMA_PERIPH_NONE);
  if (ch == NULL)
    {
      syslog(LOG_ERR, "DMAPROBE: no free channel\n");
      return;
    }

  memset(&cfg, 0, sizeof(cfg));
  cfg.src       = (uintptr_t)g_dmaprobe_src;
  cfg.dst       = (uintptr_t)g_dmaprobe_dst;
  cfg.nbytes    = DMAPROBE_NBYTES;
  cfg.direction = RK3576_DMA_M2M;
  cfg.src_width = 4;
  cfg.dst_width = 4;
  cfg.burst_len = 16;
  cfg.callback  = kickpi_k7_dmaprobe_cb;
  cfg.arg       = NULL;

  ret = rk3576_dma_setup(ch, &cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: setup failed %d\n", ret);
      rk3576_dma_free_channel(ch);
      return;
    }

  ret = rk3576_dma_start(ch);
  if (ret < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: start failed %d\n", ret);
      rk3576_dma_free_channel(ch);
      return;
    }

  /* Wait for the completion callback (bounded so a dead controller cannot
   * hang the boot).
   */

  ret = nxsem_tickwait_uninterruptible(&g_dmaprobe_done, SEC2TICK(2));
  if (ret < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: timeout waiting for completion %d\n", ret);
      rk3576_dma_stop(ch);
      rk3576_dma_free_channel(ch);
      return;
    }

  if (g_dmaprobe_result < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: transfer faulted %d\n", g_dmaprobe_result);
      rk3576_dma_free_channel(ch);
      return;
    }

  /* The PL330 wrote the destination behind the CPU's cache; invalidate it so
   * the CPU reads what the DMA actually stored.
   */

  up_invalidate_dcache((uintptr_t)g_dmaprobe_dst,
                       (uintptr_t)g_dmaprobe_dst + DMAPROBE_NBYTES);

  if (memcmp(g_dmaprobe_src, g_dmaprobe_dst, DMAPROBE_NBYTES) == 0)
    {
      syslog(LOG_INFO, "DMAPROBE: PASS - %u bytes copied and verified\n",
             DMAPROBE_NBYTES);
    }
  else
    {
      syslog(LOG_ERR, "DMAPROBE: FAIL - destination mismatch\n");
    }

  rk3576_dma_free_channel(ch);
}

#endif /* CONFIG_KICKPI_K7_DMA_PROBE */
