/****************************************************************************
 * chips/rk3576/rk3576_fspi.c
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

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/qspi.h>
#include <nuttx/wdog.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_fspi.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_fspi.h"

#ifdef CONFIG_RK3576_FSPI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clock / reset CRU indices — TODO: verify CRU register offsets */

#define RK3576_FSPI_FREQUENCY 100000000 /* Default 100 MHz */

/* CRU clock gate index: hclk_fspi gate bit (shared for both FSPI0/FSPI1
 * in NVM domain).  TODO: resolve exact GATE_CON / SOFTRST_CON offsets
 * from TRM Chapter 5.
 *
 * Current placeholder: GATE_CON(28) hclk_fspi / sclk_fspi_x2.
 */

/* TODO — enable once CRU gate/reset indices are confirmed */

/* Number of register retries for polling loops */

#define FSPI_POLL_MAX 100000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_fspi_s
{
  struct qspi_dev_s qspi; /* Externally visible QSPI interface */
  uint32_t regbase;       /* FSPI controller register base address */
  uint8_t id;             /* Controller instance: 0 or 1 */
  uint8_t cs;             /* Chip select: 0 or 1 */
  bool initialized;       /* TRUE after hw_init succeeds */
  uint32_t frequency;     /* Requested clock frequency */
  uint32_t actual;        /* Actual clock frequency */
  uint8_t mode;           /* CPOL/CPHA mode (0-3) */
  int8_t nbits;           /* Bits per word (typically 8) */
  mutex_t *fspi_mutex;    /* Pointer to shared controller-level mutex */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register access helpers */

static inline uint32_t fspi_getreg(struct rk3576_fspi_s *priv,
                                   unsigned int offset);
static inline void fspi_putreg(struct rk3576_fspi_s *priv, uint32_t value,
                               unsigned int offset);

/* Hardware operations */

static int fspi_hw_reset(struct rk3576_fspi_s *priv);
static int fspi_hw_init(struct rk3576_fspi_s *priv);

/* QSPI vtable methods */

static int fspi_lock(struct qspi_dev_s *dev, bool lock);
static uint32_t fspi_setfrequency(struct qspi_dev_s *dev, uint32_t frequency);
static void fspi_setmode(struct qspi_dev_s *dev, enum qspi_mode_e mode);
static void fspi_setbits(struct qspi_dev_s *dev, int nbits);
static int fspi_command(struct qspi_dev_s *dev,
                        struct qspi_cmdinfo_s *cmdinfo);
static int fspi_memory(struct qspi_dev_s *dev, struct qspi_meminfo_s *meminfo);
static void *fspi_alloc(struct qspi_dev_s *dev, size_t buflen);
static void fspi_free(struct qspi_dev_s *dev, void *buffer);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct qspi_ops_s g_fspi_qspi_ops = {
  .lock = fspi_lock,
  .setfrequency = fspi_setfrequency,
  .setmode = fspi_setmode,
  .setbits = fspi_setbits,
#ifdef CONFIG_QSPI_HWFEATURES
  .hwfeatures = NULL, /* Not implemented yet */
#endif
  .command = fspi_command,
  .memory = fspi_memory,
  .alloc = fspi_alloc,
  .free = fspi_free,
};

/* Controller-level shared mutexes.
 * CS0 and CS1 on the same FSPI controller share the same FSM / FIFO / DMA
 * engine, so they must be serialized at the controller level even though
 * they appear as independent qspi_dev_s instances to upper-half drivers.
 */

static mutex_t g_fspi_mutex[RK3576_FSPI_NUM_CONTROLLERS] = {
  NXMUTEX_INITIALIZER,
  NXMUTEX_INITIALIZER,
};

/* FSPI instance array: [controller_id][cs_index] */

static struct rk3576_fspi_s g_fspi_devs[RK3576_FSPI_NUM_CONTROLLERS]
                                       [RK3576_FSPI_NUM_CHIPSELECTS] =
{
  /* FSPI0 */
  {
    /* CS0 */
    {
      .qspi =
      {
        .ops = &g_fspi_qspi_ops,
      },
      .regbase = RK3576_FSPI0_ADDR,
      .id      = 0,
      .cs      = 0,
    },
    /* CS1 */
    {
      .qspi =
      {
        .ops = &g_fspi_qspi_ops,
      },
      .regbase = RK3576_FSPI0_ADDR,
      .id      = 0,
      .cs      = 1,
    },
  },
  /* FSPI1 */
  {
    /* CS0 */
    {
      .qspi =
      {
        .ops = &g_fspi_qspi_ops,
      },
      .regbase = RK3576_FSPI1_ADDR,
      .id      = 1,
      .cs      = 0,
    },
    /* CS1 */
    {
      .qspi =
      {
        .ops = &g_fspi_qspi_ops,
      },
      .regbase = RK3576_FSPI1_ADDR,
      .id      = 1,
      .cs      = 1,
    },
  },
};

/****************************************************************************
 * Private Functions — Register Access Helpers
 ****************************************************************************/

static inline uint32_t fspi_getreg(struct rk3576_fspi_s *priv,
                                   unsigned int offset)
{
  return getreg32(priv->regbase + offset);
}

static inline void fspi_putreg(struct rk3576_fspi_s *priv, uint32_t value,
                               unsigned int offset)
{
  putreg32(value, priv->regbase + offset);
}

/****************************************************************************
 * Private Functions — Hardware Operations
 ****************************************************************************/

/****************************************************************************
 * Name: fspi_hw_reset
 ****************************************************************************/

static int fspi_hw_reset(struct rk3576_fspi_s *priv)
{
  uint32_t status;
  int timeout;

  /* Trigger SFC FSM + FIFO reset */

  fspi_putreg(priv, FSPI_RCVR_RESET, RK3576_FSPI_RCVR);

  timeout = FSPI_POLL_MAX;
  do
    {
      status = fspi_getreg(priv, RK3576_FSPI_RCVR);
      if (!(status & FSPI_RCVR_RESET))
        {
          break;
        }
    }
  while (--timeout > 0);

  if (timeout == 0)
    {
      spierr("FSPI%u reset timeout\n", priv->id);
      return -ETIMEDOUT;
    }

  /* Clear all pending interrupts */

  fspi_putreg(priv, 0xffffffff, RK3576_FSPI_ICLR);

  return OK;
}

/****************************************************************************
 * Name: fspi_hw_init
 ****************************************************************************/

static int fspi_hw_init(struct rk3576_fspi_s *priv)
{
  int ret;

  /* TODO: Enable clocks via CRU
   *
   * 1. Enable hclk_fspi gate   (hclk_fspi_en)  — APB bus clock
   * 2. Enable sclk_fspi_x2 gate (sclk_fspi_x2_en) — core clock
   *
   * These are in the NVM clock domain.  Exact GATE_CON register offsets
   * need to be confirmed from TRM Chapter 5.
   *
   * Pseudocode:
   *   rk3576_cru_gate_enable(RK3576_CRU_GATE_HCLK_FSPI, true);
   *   rk3576_cru_gate_enable(RK3576_CRU_GATE_SCLK_FSPI_X2, true);
   *
   * TODO: De-assert resets via CRU
   *
   * Pseudocode:
   *   rk3576_cru_softreset_deassert(RK3576_CRU_SOFTRST_FSPI);
   */

  /* Reset the SFC FSM and FIFOs */

  ret = fspi_hw_reset(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Initialize both CS CTRL registers: single-line mode as safe default.
   * Each CS can later be independently reconfigured via fspi_setmode /
   * fspi_command.
   */

  fspi_putreg(priv, 0, RK3576_FSPI_CTRL(0));
  fspi_putreg(priv, 0, RK3576_FSPI_CTRL(1));

  /* Reset all interrupts */

  fspi_putreg(priv, 0xffffffff, RK3576_FSPI_IMR);

  /* TODO: VER >= 4: set SFC_LEN_CTRL_TRB_SEL */

  return OK;
}

/****************************************************************************
 * Private Functions — QSPI Vtable Methods
 ****************************************************************************/

/****************************************************************************
 * Name: fspi_lock
 *
 * Description:
 *   Lock / unlock the QSPI bus for exclusive access.
 *
 *   Since CS0 and CS1 on the same FSPI controller share the FSM, FIFO and
 *   DMA engine, the lock is controller-wide.  Both CS instances on the same
 *   controller share a common mutex (g_fspi_mutex[id]).
 ****************************************************************************/

static int fspi_lock(struct qspi_dev_s *dev, bool lock)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  int ret;

  if (lock)
    {
      ret = nxmutex_lock(priv->fspi_mutex);
    }
  else
    {
      ret = nxmutex_unlock(priv->fspi_mutex);
    }

  return ret;
}

/****************************************************************************
 * Name: fspi_setfrequency
 ****************************************************************************/

static uint32_t fspi_setfrequency(struct qspi_dev_s *dev, uint32_t frequency)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;

  /* TODO: Program SFC clock divider through CRU
   *
   * Frequency is set via CRU sclk_fspi_x2 clock:
   *   target_rate = frequency * 2;
   *   clk_set_rate(sfc->clk, target_rate);
   *   actual = clk_get_rate(sfc->clk) / 2;
   *
   * For now just store and return the requested value.
   */

  priv->frequency = frequency;
  priv->actual = frequency;

  return priv->actual;
}

/****************************************************************************
 * Name: fspi_setmode
 ****************************************************************************/

static void fspi_setmode(struct qspi_dev_s *dev, enum qspi_mode_e mode)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  unsigned int ctrl_off = RK3576_FSPI_CTRL(priv->cs);
  uint32_t ctrl;

  priv->mode = mode;

  /* TODO: Configure CPOL/CPHA in SFC CTRL register
   *
   * FSPI CTRL phase selection:
   *   FSPI_CTRL_PHASE_SEL_NEGETIVE selects sampling edge.
   *
   * Current SFC IP may not support all SPI modes natively — verify with TRM.
   * For QSPI LCD displays, mode 0 or mode 3 is typical.
   */

  ctrl = fspi_getreg(priv, ctrl_off);

  switch (mode)
    {
      case QSPIDEV_MODE0: /* CPOL=0 CPHA=0 */
        /* TODO: configure for MODE0 */
        break;
      case QSPIDEV_MODE1: /* CPOL=0 CPHA=1 */
        ctrl |= FSPI_CTRL_PHASE_SEL_NEGETIVE;
        break;
      case QSPIDEV_MODE2: /* CPOL=1 CPHA=0 */
        /* TODO: configure for MODE2 */
        break;
      case QSPIDEV_MODE3: /* CPOL=1 CPHA=1 */
        /* TODO: configure for MODE3 */
        break;
      default:
        break;
    }

  fspi_putreg(priv, ctrl, ctrl_off);
}

/****************************************************************************
 * Name: fspi_setbits
 ****************************************************************************/

static void fspi_setbits(struct qspi_dev_s *dev, int nbits)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;

  /* For QSPI LCD, bits-per-word is typically 8.
   * The SFC controller does not have a dedicated "bits per word" register;
   * it operates on bytes.  For 16-bit QSPI LCD transfers, the upper layer
   * packs pixel data into the buffer, and we transmit as byte stream.
   */

  priv->nbits = nbits;
}

/****************************************************************************
 * Name: fspi_alloc
 ****************************************************************************/

static void *fspi_alloc(struct qspi_dev_s *dev, size_t buflen)
{
  /* Allocate memory suitable for DMA (cache-aligned).
   * TODO: use a DMA-safe allocator if available.
   */

  return kmm_malloc(buflen);
}

/****************************************************************************
 * Name: fspi_free
 ****************************************************************************/

static void fspi_free(struct qspi_dev_s *dev, void *buffer)
{
  kmm_free(buffer);
}

/****************************************************************************
 * Name: fspi_command
 *
 * Description:
 *   Perform one QSPI command transfer.
 *
 *   This is the CORE method for QSPI LCD driving.  It supports:
 *     - Command-only transfers (flags == 0 or QSPICMD_IQUAD)
 *     - Command + write data  (QSPICMD_WRITEDATA)
 *     - Command + read data   (QSPICMD_READDATA)
 *     - Command + address     (QSPICMD_ADDRESS)
 *     - 4-line instruction    (QSPICMD_IQUAD)
 *     - 2-line instruction    (QSPICMD_IDUAL)
 *
 * Input Parameters:
 *   dev     - Device-specific state data
 *   cmdinfo - Describes the command transfer to be performed.
 *
 ****************************************************************************/

static int fspi_command(struct qspi_dev_s *dev, struct qspi_cmdinfo_s *cmdinfo)
{
  UNUSED(dev);
  UNUSED(cmdinfo);

  return -ENOSYS;
}

/****************************************************************************
 * Name: fspi_memory
 *
 * Description:
 *   Perform one QSPI memory transfer (for SPI NOR flash).
 *
 *   TODO: Implement when flash support is needed.
 ****************************************************************************/

static int fspi_memory(struct qspi_dev_s *dev, struct qspi_meminfo_s *meminfo)
{
  UNUSED(dev);
  UNUSED(meminfo);

  return -ENOSYS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

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

struct qspi_dev_s *rk3576_fspi_initialize(int fspi_id, int cs)
{
  struct rk3576_fspi_s *priv = NULL;
  static bool g_fspi_hw_initialized[RK3576_FSPI_NUM_CONTROLLERS] = { false,
                                                                     false };

  /* Validate arguments */

  if (fspi_id < 0 || fspi_id >= RK3576_FSPI_NUM_CONTROLLERS || cs < 0 ||
      cs >= RK3576_FSPI_NUM_CHIPSELECTS)
    {
      spierr("Invalid FSPI instance: id=%d cs=%d\n", fspi_id, cs);
      return NULL;
    }

  /* Select the pre-allocated instance */

  priv = &g_fspi_devs[fspi_id][cs];

  /* Ensure the shared mutex pointer is set (only needed once) */

  priv->fspi_mutex = &g_fspi_mutex[fspi_id];

  /* Lock the controller-level mutex to serialize hardware init */

  nxmutex_lock(priv->fspi_mutex);

  /* CS instance already initialized? */

  if (priv->initialized)
    {
      nxmutex_unlock(priv->fspi_mutex);
      return &priv->qspi;
    }

  /* Hardware initialization — done only once per controller.
   * The first call (for either CS0 or CS1) performs the one-time
   * hardware init.  Subsequent calls for the other CS skip it.
   */

  if (!g_fspi_hw_initialized[fspi_id])
    {
      int ret;

      ret = fspi_hw_init(priv);
      if (ret < 0)
        {
          nxmutex_unlock(priv->fspi_mutex);
          return NULL;
        }

      g_fspi_hw_initialized[fspi_id] = true;
    }

  /* Set default frequency and mode for this CS instance */

  fspi_setfrequency(&priv->qspi, RK3576_FSPI_FREQUENCY);
  fspi_setmode(&priv->qspi, QSPIDEV_MODE0);
  fspi_setbits(&priv->qspi, 8);

  priv->initialized = true;

  nxmutex_unlock(priv->fspi_mutex);

  return &priv->qspi;
}

#endif /* CONFIG_RK3576_FSPI */
