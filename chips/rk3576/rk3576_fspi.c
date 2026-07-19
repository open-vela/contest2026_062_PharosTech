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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
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
  struct clk_s *sclk_x2;  /* sclk_fspiX_x2_en gate (2x actual SCLK rate) */
  struct clk_s *hclk;     /* hclk_fspiX_en gate (CLK framework handle) */
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

/* Internal helpers */

static void fspi_hw_update_clock_div(struct rk3576_fspi_s *priv);

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

/* Controller-level hardware frequency cache.
 *
 * The SCLK_x2 divider is per-controller (shared by CS0 and CS1).  We
 * cache the last-written actual frequency here to avoid calling into the
 * CLK framework (clk_set_rate) when the requested frequency has not
 * changed.  A sentinel value of 0 means "never written / unknown".
 */

static uint32_t g_fspi_hw_freq[RK3576_FSPI_NUM_CONTROLLERS] = {
  0,
  0,
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
  char name[32];

  /* Get HCLK gate via CLK framework.
   * NuttX CLK framework uses platform_device-style name matching, so
   * clk_get() uses the exact name registered in clk_tree.
   */

  snprintf(name, sizeof(name), "hclk_fspi%u_en", priv->id);
  priv->hclk = clk_get(name);
  if (!priv->hclk)
    {
      spierr("FSPI%u: failed to get %s\n", priv->id, name);
      return -ENODEV;
    }

  snprintf(name, sizeof(name), "sclk_fspi%u_x2_en", priv->id);
  priv->sclk_x2 = clk_get(name);
  if (!priv->sclk_x2)
    {
      spierr("FSPI%u: failed to get %s\n", priv->id, name);
      return -ENODEV;
    }

  /* Enable both HCLK (AHB bus) and SCLK (functional) clocks */

  ret = clk_enable(priv->hclk);
  if (ret < 0)
    {
      spierr("FSPI%u: failed to enable hclk, ret=%d\n", priv->id, ret);
      return ret;
    }

  ret = clk_enable(priv->sclk_x2);
  if (ret < 0)
    {
      spierr("FSPI%u: failed to enable sclk, ret=%d\n", priv->id, ret);
      return ret;
    }

  /* TODO: make the clock source configurable.*/

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

  /* Mask all interrupts */

  fspi_putreg(priv, 0xffffffff, RK3576_FSPI_IMR);

  return OK;
}

/****************************************************************************
 * Private Functions — Internal Helpers
 ****************************************************************************/

/****************************************************************************
 * Name: fspi_hw_update_clock_div
 *
 * Description:
 *   Apply the per-CS frequency request to the SCLK_x2 divider hardware.
 *
 *   The SCLK_x2 divider is per-controller (shared by CS0 and CS1), so
 *   this function must only be called while the controller-level mutex
 *   is held.  We cache the last-written actual frequency at the
 *   controller level and skip the clk_set_rate() call (and the associated
 *   CLK framework lock overhead) when the frequency has not changed.
 ****************************************************************************/

static void fspi_hw_update_clock_div(struct rk3576_fspi_s *priv)
{
  int ret;

  if (priv->actual == g_fspi_hw_freq[priv->id])
    {
      return;
    }

  ret = clk_set_rate(priv->sclk_x2, priv->frequency * 2);
  if (ret >= 0)
    {
      priv->actual = clk_get_rate(priv->sclk_x2) / 2;
      g_fspi_hw_freq[priv->id] = priv->actual;
    }
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
 *
 *   On lock acquire, fspi_hw_update_clock_div() flushes the per-CS
 *   frequency request to the CRU hardware if the divider differs from the
 *   cached controller-level value.  This minimises CRU register access.
 ****************************************************************************/

static int fspi_lock(struct qspi_dev_s *dev, bool lock)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  int ret;

  if (lock)
    {
      ret = nxmutex_lock(priv->fspi_mutex);
      if (ret < 0)
        {
          return ret;
        }

      fspi_hw_update_clock_div(priv);
    }
  else
    {
      ret = nxmutex_unlock(priv->fspi_mutex);
    }

  return ret;
}

/****************************************************************************
 * Name: fspi_setfrequency
 *
 * Description:
 *   Record the requested frequency for this CS instance.  The divider
 *   computation and CRU register write are deferred to fspi_lock() when
 *   this CS gains exclusive access to the shared controller hardware.
 ****************************************************************************/

static uint32_t fspi_setfrequency(struct qspi_dev_s *dev, uint32_t frequency)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;

  /* Ask the CLK framework what rate the divider can actually achieve
   * for the requested frequency.  clk_round_rate() walks the clock
   * tree and applies the hardware divider constraints without writing
   * any registers, giving us the nearest achievable rate.
   */

  DEBUGASSERT(priv->sclk_x2);
  uint32_t achievable = clk_round_rate(priv->sclk_x2, frequency * 2) / 2;

  priv->frequency = frequency;
  priv->actual = achievable;

  return achievable;
}

/****************************************************************************
 * Name: fspi_setmode
 ****************************************************************************/

static void fspi_setmode(struct qspi_dev_s *dev, enum qspi_mode_e mode)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  unsigned int ctrl_reg = RK3576_FSPI_CTRL(priv->cs);
  uint32_t ctrl;

  priv->mode = mode;

  ctrl = fspi_getreg(priv, ctrl_reg);
  ctrl &= ~(FSPI_CTRL_SPIM_MASK | FSPI_CTRL_SHIFTPHASE_MASK);

  switch (mode)
    {
      case QSPIDEV_MODE0:
        ctrl |= FSPI_CTRL_SPIM_MODE_0;
        ctrl |= FSPI_CTRL_SHIFTPHASE_POSEDGE;
        break;
      case QSPIDEV_MODE3:
        ctrl |= FSPI_CTRL_SPIM_MODE_3;
        ctrl |= FSPI_CTRL_SHIFTPHASE_NEGEDGE;
        break;
      case QSPIDEV_MODE1:
      case QSPIDEV_MODE2:
        spiwarn("rk3576 fspi peripheral does not support mode %d", mode);
        return;
      default:
        spiwarn("unknown spi mode: %d", mode);
        return;
    }

  fspi_putreg(priv, ctrl, ctrl_reg);
}

/****************************************************************************
 * Name: fspi_setbits
 *
 * Description:
 *   Set the number of bits per SPI word (frame width).
 *
 *   NOTE: This is a legacy API inherited from the traditional SPI subsystem.
 *   In the traditional SPI model, nbits determines how the lower-half
 *   driver interprets the tx/rx buffer in spi_exchange() — e.g. nbits > 8
 *   means 16-bit words and the buffer is accessed as uint16_t*.
 *
 *   In the QSPI subsystem, data transfers use the command()/memory()
 *   descriptor-based model instead of exchange().  The SFC controller
 *   operates on a byte-stream basis with no per-word bit-width register;
 *   the instruction/data line count is controlled per-transfer via
 *   QSPICMD_IDUAL/IQUAD and QSPIMEM_DUALIO/QUADIO flags in cmdinfo/meminfo.
 *
 *   Therefore this function exists only for API compatibility.  It stores
 *   the value but it is never consumed by any transfer logic.  All QSPI
 *   Flash/lower-half upper-half drivers today pass nbits=8, which is the
 *   only value accepted here.
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *   nbits - The number of bits per word.  Only 8 is supported.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static void fspi_setbits(struct qspi_dev_s *dev, int nbits)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;

  if (nbits != 8)
    {
      spiinfo("unsupported nbits: %d\n", nbits);
      DEBUGASSERT(FALSE);
    }

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
 * Name: fspi_wait_tx_fifo_ready
 *
 * Description:
 *   Poll FSR (FIFO Status Register) until TX FIFO has at least one free
 *   slot, then return the number of available slots.
 *
 *   TXLV (TX FIFO Level) — FSR bits[12:8] indicates how many 32-bit words
 *   the TX FIFO can still accept.
 *
 *   Returns:
 *     > 0 — number of free TX FIFO slots
 *     0 — timeout
 ****************************************************************************/

static int fspi_wait_tx_fifo_ready(struct rk3576_fspi_s *priv,
                                   uint32_t timeout_us)
{
  uint32_t timeout = timeout_us * 10;
  uint32_t status;

  while (--timeout)
    {
      status = fspi_getreg(priv, RK3576_FSPI_FSR);
      if (status & FSPI_FSR_TXLV_MASK)
        {
          return (status & FSPI_FSR_TXLV_MASK) >> FSPI_FSR_TXLV_SHIFT;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: fspi_wait_rx_fifo_ready
 *
 * Description:
 *   Poll FSR until RX FIFO has at least one word available, then return
 *   the number of available words.
 *
 *   RXLV (RX FIFO Level) — FSR bits[20:16] indicates how many 32-bit words
 *   are available in the RX FIFO.
 ****************************************************************************/

static int fspi_wait_rx_fifo_ready(struct rk3576_fspi_s *priv,
                                   uint32_t timeout_us)
{
  uint32_t timeout = timeout_us * 10;
  uint32_t status;

  while (--timeout)
    {
      status = fspi_getreg(priv, RK3576_FSPI_FSR);
      if (status & FSPI_FSR_RXLV_MASK)
        {
          return (status & FSPI_FSR_RXLV_MASK) >> FSPI_FSR_RXLV_SHIFT;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: fspi_write_fifo
 *
 * Description:
 *   Write len bytes from buf to the SFC_DATA register (TX FIFO).
 *
 *   The SFC controller requires 32-bit word-aligned access to SFC_DATA.
 *   This function writes in 4-byte chunks via iowrite32-like loop, then
 *   handles leftover 1–3 bytes by padding into a temporary 32-bit word.
 *
 *   Before each write, fspi_wait_tx_fifo_ready() is called to ensure the
 *   TX FIFO has room.
 ****************************************************************************/

static int fspi_write_fifo(struct rk3576_fspi_s *priv, const uint8_t *buf,
                           int len)
{
  uint8_t bytes = len & 0x3;
  uint32_t dwords = len >> 2;
  int tx_level;
  uint32_t write_words;
  uint32_t tmp = 0;

  /* Write 4-byte aligned portion */

  while (dwords)
    {
      tx_level = fspi_wait_tx_fifo_ready(priv, 1000);
      if (tx_level <= 0)
        {
          spierr("FSPI TX FIFO wait timeout\n");
          return -ETIMEDOUT;
        }

      write_words = dwords;
      if ((uint32_t)tx_level < write_words)
        {
          write_words = (uint32_t)tx_level;
        }

      while (write_words--)
        {
          tmp = ((uint32_t)buf[0] << 0) | ((uint32_t)buf[1] << 8) |
                ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
          fspi_putreg(priv, tmp, RK3576_FSPI_DATA);
          buf += 4;
          dwords--;
        }
    }

  /* Write leftover 1–3 bytes */

  if (bytes)
    {
      tx_level = fspi_wait_tx_fifo_ready(priv, 1000);
      if (tx_level <= 0)
        {
          spierr("FSPI TX FIFO tail wait timeout\n");
          return -ETIMEDOUT;
        }

      tmp = 0;
      if (bytes > 0)
        tmp |= (uint32_t)buf[0] << 0;
      if (bytes > 1)
        tmp |= (uint32_t)buf[1] << 8;
      if (bytes > 2)
        tmp |= (uint32_t)buf[2] << 16;
      fspi_putreg(priv, tmp, RK3576_FSPI_DATA);
    }

  return len;
}

/****************************************************************************
 * Name: fspi_read_fifo
 *
 * Description:
 *   Read len bytes from the SFC_DATA register (RX FIFO) into buf.
 *
 *   Same 32-bit alignment rules as fspi_write_fifo: bulk reads in 4-byte
 *   chunks, then handle the trailing 1–3 bytes with a temp variable.
 ****************************************************************************/

static int fspi_read_fifo(struct rk3576_fspi_s *priv, uint8_t *buf, int len)
{
  uint8_t bytes = len & 0x3;
  uint32_t dwords = len >> 2;
  int rx_level;
  uint32_t read_words;
  uint32_t tmp;

  /* Read 4-byte aligned portion */

  while (dwords)
    {
      rx_level = fspi_wait_rx_fifo_ready(priv, 1000);
      if (rx_level <= 0)
        {
          spierr("FSPI RX FIFO wait timeout\n");
          return -ETIMEDOUT;
        }

      read_words = dwords;
      if ((uint32_t)rx_level < read_words)
        {
          read_words = (uint32_t)rx_level;
        }

      while (read_words--)
        {
          tmp = fspi_getreg(priv, RK3576_FSPI_DATA);
          buf[0] = (uint8_t)(tmp >> 0);
          buf[1] = (uint8_t)(tmp >> 8);
          buf[2] = (uint8_t)(tmp >> 16);
          buf[3] = (uint8_t)(tmp >> 24);
          buf += 4;
          dwords--;
        }
    }

  /* Read leftover 1–3 bytes */

  if (bytes)
    {
      rx_level = fspi_wait_rx_fifo_ready(priv, 1000);
      if (rx_level <= 0)
        {
          spierr("FSPI RX FIFO tail wait timeout\n");
          return -ETIMEDOUT;
        }

      tmp = fspi_getreg(priv, RK3576_FSPI_DATA);
      if (bytes > 0)
        buf[0] = (uint8_t)(tmp >> 0);
      if (bytes > 1)
        buf[1] = (uint8_t)(tmp >> 8);
      if (bytes > 2)
        buf[2] = (uint8_t)(tmp >> 16);
    }

  return len;
}

/****************************************************************************
 * Name: fspi_xfer_done
 *
 * Description:
 *   Poll the FSM Status Register (SR) until the controller goes idle.
 ****************************************************************************/

static int fspi_xfer_done(struct rk3576_fspi_s *priv)
{
  uint32_t timeout = FSPI_POLL_MAX;
  uint32_t status;

  while (--timeout)
    {
      status = fspi_getreg(priv, RK3576_FSPI_SR);
      if (!(status & FSPI_SR_IS_BUSY))
        {
          return OK;
        }
    }

  spierr("FSPI wait idle timeout\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: fspi_command
 *
 * Description:
 *   Perform one QSPI command transfer.
 *
 * Input Parameters:
 *   dev     - Device-specific state data
 *   cmdinfo - Describes the command transfer to be performed.
 *
 ****************************************************************************/

static int fspi_command(struct qspi_dev_s *dev, struct qspi_cmdinfo_s *cmdinfo)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  unsigned int ctrl_reg = RK3576_FSPI_CTRL(priv->cs);
  uint32_t ctrl_rge_bits, cmd_reg_bits;
  int ret;

  ret = fspi_xfer_done(priv);
  if (ret < 0)
    {
      return ret;
    }

  uint32_t timeout = FSPI_POLL_MAX;
  while (--timeout)
    {
      if (getreg32(RK3576_FSPI_SR) & FSPI_SR_IS_BUSY)
        {
          break;
        }
    }
  if (timeout == 0)
    {
      return -ETIMEDOUT;
    }

  /* CTRL config begin */

  ctrl_rge_bits = fspi_getreg(priv, ctrl_reg);
  ctrl_rge_bits &=
      ~(FSPI_CTRL_CMDB_MASK | FSPI_CTRL_ADDRB_MASK | FSPI_CTRL_DATAB_MASK);

  /* The QSPICMD_IDUAL/IQUAD flags are ambiguous (upstream drivers read
   * them as widening only the instruction phase, while others apply them
   * to the whole transaction), so they are rejected outright.  Nearly all
   * QSPI devices only accept a single-line command phase, so the command
   * path always runs in the most compatible 1-1-1 mode: command, address
   * and data all on a single line.  Callers that need dual/quad bulk
   * transfers must use fspi_memory(), which exposes finer-grained control
   * of the line widths through QSPIMEM_* flags.
   */

  if (cmdinfo->flags & (QSPICMD_IDUAL | QSPICMD_IQUAD))
    {
      spierr("QSPICMD_IDUAL/IQUAD unsupported in command()");
      return -EINVAL;
    }

  ctrl_rge_bits |= FSPI_CTRL_CMDB_X1;
  ctrl_rge_bits |= FSPI_CTRL_ADDRB_X1;
  ctrl_rge_bits |= FSPI_CTRL_DATAB_X1;

  fspi_putreg(priv, ctrl_rge_bits, ctrl_reg);

  /* CTRL config end */

  /* CMD config begin */

  /* setup cs */
  cmd_reg_bits = FSPI_CMD_CS(priv->cs);

  /* setup data length */
  if (cmdinfo->flags & (QSPICMD_READDATA | QSPICMD_WRITEDATA))
    {
      cmd_reg_bits |= (cmdinfo->buflen << FSPI_CMD_TRB_SHIFT);
    }
  else /* 0 data bytes will be transfered */
    {
      cmd_reg_bits |= (0 << FSPI_CMD_TRB_SHIFT);
    }

  /* setup addr length */
  if (cmdinfo->flags & QSPICMD_ADDRESS)
    {
      switch (cmdinfo->addrlen)
        {
          case 0:
            cmd_reg_bits |= FSPI_CMD_ADDR_0BITS;
            break;
          case 3:
            cmd_reg_bits |= FSPI_CMD_ADDR_24BITS;
            break;
          case 4:
            cmd_reg_bits |= FSPI_CMD_ADDR_32BITS;
            break;
          default:
            spierr("Invalid qspi addr len (0, 3 or 4 bytes expected, got %u)",
                   cmdinfo->addrlen);
            return -EINVAL;
        }
    }
  else
    {
      cmd_reg_bits |= FSPI_CMD_ADDR_0BITS;
    }

  /* setup WR */
  if (cmdinfo->flags & QSPICMD_WRITEDATA)
    {
      cmd_reg_bits |= FSPI_CMD_DIR_WR;
    }
  else
    {
      cmd_reg_bits |= FSPI_CMD_DIR_RD;
    }

  /* 0 dummy cycles */
  cmd_reg_bits |= (0 << FSPI_CMD_DUMMY_SHIFT);

  /* non-continuous mode */
  cmd_reg_bits |= FSPI_CMD_CONT_DISABLE;

  /* setup cmd */
  if (cmdinfo->cmd > 0xff)
    {
      /* TODO: support 16-bit cmd */
      spierr("8-bit cmd expected, got 0x%X", cmdinfo->cmd);
      return -EINVAL;
    }
  cmd_reg_bits |= (cmdinfo->cmd << FSPI_CMD_OPCODE_SHIFT);

  fspi_putreg(priv, cmd_reg_bits, RK3576_FSPI_CMD);

  /* CMD config end */

  /* send addr*/
  if (cmdinfo->flags & QSPICMD_ADDRESS)
    {
      fspi_putreg(priv, cmdinfo->addr, RK3576_FSPI_ADDR);
    }

  /* transfer data via FIFO */

  if (cmdinfo->flags & QSPICMD_WRITEDATA)
    {
      ret = fspi_write_fifo(priv, cmdinfo->buffer, cmdinfo->buflen);
      if (ret < 0)
        {
          return ret;
        }
    }
  else if (cmdinfo->flags & QSPICMD_READDATA)
    {
      ret = fspi_read_fifo(priv, cmdinfo->buffer, cmdinfo->buflen);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* wait for transfer to complete */

  ret = fspi_xfer_done(priv);
  if (ret < 0)
    {
      fspi_hw_reset(priv);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: fspi_memory
 *
 * Description:
 *   Perform one QSPI memory transfer
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

  fspi_setfrequency(&priv->qspi, 10000000); /* 10MHz */
  fspi_setmode(&priv->qspi, QSPIDEV_MODE0);

  priv->initialized = true;

  nxmutex_unlock(priv->fspi_mutex);

  return &priv->qspi;
}

#endif /* CONFIG_RK3576_FSPI */
