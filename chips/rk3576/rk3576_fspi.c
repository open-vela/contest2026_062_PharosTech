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
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/qspi.h>
#include <nuttx/wdog.h>
#include <semaphore.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_fspi.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_fspi.h"

#ifdef CONFIG_RK3576_FSPI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of register retries for polling loops */

#define FSPI_POLL_MAX 100000

/* Base timeout allowance (in microseconds) added on top of the estimated
 * data transfer time to form the DMA transfer timeout threshold.
 */

#define FSPI_INT_TIMEOUT_US 1000000

/* Minimum transfer length (in bytes) that qualifies for the internal DMA
 * path.  Shorter transfers go through the FIFO polling path instead, since
 * the DMA setup/teardown and interrupt overhead would outweigh the benefit.
 */

#define FSPI_DMA_LEN_THRES 64

/* DMA payload cache-line alignment.
 *
 * rk3576_dma_alloc() hands out 64B-aligned blocks so that per-transfer
 * D-cache maintenance (clean/invalidate) never touches neighbouring data.
 * fspi_alloc() prepends a size_t metadata header to recover the block size
 * on free (QSPI_FREE() carries no length).  If the header were placed
 * directly in front of the payload (payload = base + 8), it would drag the
 * payload down to 8-byte alignment and would force the metadata and the DMA
 * payload to share a cache line.  Instead we park the header alone in the
 * first cache line and return the payload at the start of the next line,
 * keeping the payload 64B-aligned and cache-line isolated from the header.
 */

#define FSPI_DMA_CACHELINE  64
#define FSPI_DMA_ALIGN_MASK (FSPI_DMA_CACHELINE - 1)
#define FSPI_DMA_ALIGN_UP(n) \
  (((n) + FSPI_DMA_ALIGN_MASK) & ~FSPI_DMA_ALIGN_MASK)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Forward declaration */

struct rk3576_fspi_ctrl_s;

struct rk3576_fspi_s
{
  struct qspi_dev_s qspi;          /* Externally visible QSPI interface */
  struct rk3576_fspi_ctrl_s *ctrl; /* Back-pointer to owning controller */
  uint8_t cs;                      /* Chip select: 0 or 1 */
  bool initialized;                /* TRUE after hw_init succeeds */
  uint32_t freq_req;               /* Requested clock frequency (raw) */
  uint32_t freq;                   /* Expected achievable SCLK for this CS */
  uint8_t mode;                    /* CPOL/CPHA mode (0-3) */
  int8_t nbits;                    /* Bits per word (typically 8) */
};

/* Per-controller state.
 * All controller-level resources (register base, mutex, frequency cache,
 * IRQ state, CS devices) are grouped into one struct so the ISR receives a
 * single self-contained pointer via irq_attach arg.
 */

struct rk3576_fspi_ctrl_s
{
  uint32_t regbase; /* FSPI controller register base */
  uint8_t id;       /* Controller index: 0 or 1 */
  mutex_t lock;     /* Controller-level mutex */
  struct clk_s
      *sclk_x2;     /* sclk_fspiX_x2 clock (2x actual SCLK), per-controller */
  uint32_t hw_freq; /* Actual SCLK frequency currently programmed in HW */
  bool hw_initialized; /* One-time hardware init done */
  bool irq_registered; /* IRQ handler registered */

  struct rk3576_fspi_s *volatile active; /* Currently locked CS device (ISR) */
  struct rk3576_fspi_s devs[RK3576_FSPI_NUM_CHIPSELECTS]; /* CS0, CS1 */

  /* Transfer state (shared by CS0/CS1 on this controller) */

  sem_t dma_sem;         /* DMA transfer complete semaphore */
  volatile bool dma_err; /* TRUE if the last DMA transfer ended in an error */
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
static inline uint32_t
rk3576_fspi_calc_timeout_thres(struct rk3576_fspi_s *priv, uint32_t buflen,
                               uint32_t line_bits);

/* Interrupt handler */

static int fspi_interrupt(int irq, void *context, void *arg);

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

/* Per-controller state array.
 * Each controller has its own register base, mutex, frequency cache,
 * IRQ state, and two CS device instances (CS0, CS1).  The ISR receives
 * a pointer to the owning g_fspi_ctrl[id] via irq_attach arg, so it can
 * directly access regbase and active device without pointer arithmetic.
 */

static struct rk3576_fspi_ctrl_s g_fspi_ctrl[RK3576_FSPI_NUM_CONTROLLERS] =
{
  /* FSPI0 */
  {
    .regbase = RK3576_FSPI0_ADDR,
    .id      = 0,
    .lock    = NXMUTEX_INITIALIZER,
    .devs    =
    {
      /* CS0 */
      {
        .qspi  = { .ops = &g_fspi_qspi_ops },
        .cs    = 0,
      },
      /* CS1 */
      {
        .qspi  = { .ops = &g_fspi_qspi_ops },
        .cs    = 1,
      },
    },
  },
  /* FSPI1 */
  {
    .regbase = RK3576_FSPI1_ADDR,
    .id      = 1,
    .lock    = NXMUTEX_INITIALIZER,
    .devs    =
    {
      /* CS0 */
      {
        .qspi  = { .ops = &g_fspi_qspi_ops },
        .cs    = 0,
      },
      /* CS1 */
      {
        .qspi  = { .ops = &g_fspi_qspi_ops },
        .cs    = 1,
      },
    },
  },
};

/****************************************************************************
 * Private Functions — Register Access Helpers
 ****************************************************************************/

static inline uint32_t fspi_getreg(struct rk3576_fspi_s *priv,
                                   unsigned int offset)
{
  return getreg32(priv->ctrl->regbase + offset);
}

static inline void fspi_putreg(struct rk3576_fspi_s *priv, uint32_t value,
                               unsigned int offset)
{
  putreg32(value, priv->ctrl->regbase + offset);
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
      spierr("FSPI%u reset timeout\n", priv->ctrl->id);
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
  struct rk3576_fspi_ctrl_s *ctrl = priv->ctrl;
  struct clk_s *hclk;
  int ret;
  char name[32];

  /* Get and enable HCLK (AHB bus clock gate) */

  snprintf(name, sizeof(name), "hclk_fspi%u", ctrl->id);
  hclk = clk_get(name);
  if (!hclk)
    {
      spierr("FSPI%u: failed to get %s\n", ctrl->id, name);
      return -ENODEV;
    }

  ret = clk_enable(hclk);
  if (ret < 0)
    {
      spierr("FSPI%u: failed to enable hclk, ret=%d\n", ctrl->id, ret);
      return ret;
    }

  /* Get and enable SCLK x2 (per-controller clock, shared by CS0/CS1) */

  snprintf(name, sizeof(name), "sclk_fspi%u_x2", ctrl->id);
  ctrl->sclk_x2 = clk_get(name);
  if (!ctrl->sclk_x2)
    {
      spierr("FSPI%u: failed to get %s\n", ctrl->id, name);
      ret = -ENODEV;
      goto err_disable_hclk;
    }

  ret = clk_enable(ctrl->sclk_x2);
  if (ret < 0)
    {
      spierr("FSPI%u: failed to enable sclk_x2, ret=%d\n", ctrl->id, ret);
      goto err_disable_hclk;
    }

  /* Reset the SFC FSM and FIFOs */

  ret = fspi_hw_reset(priv);
  if (ret < 0)
    {
      goto err_disable_sclk;
    }

  /* Initialize both CS CTRL registers: single-line mode as safe default.
   * Each CS can later be independently reconfigured via fspi_setmode /
   * fspi_command.
   */

  fspi_putreg(priv, 0, RK3576_FSPI_CTRL(0));
  fspi_putreg(priv, 0, RK3576_FSPI_CTRL(1));

  /* Mask all interrupts */

  fspi_putreg(priv, 0xffffffff, RK3576_FSPI_IMR);

  /* Register interrupt handler for this controller */

  if (!ctrl->irq_registered)
    {
      int irq = (ctrl->id == 0) ? RK3576_IRQ_FSPI0 : RK3576_IRQ_FSPI1;

      ret = irq_attach(irq, fspi_interrupt, ctrl);
      if (ret < 0)
        {
          spierr("FSPI%u: failed to attach IRQ, ret=%d\n", ctrl->id, ret);
          goto err_disable_sclk;
        }

      up_enable_irq(irq);
      ctrl->irq_registered = true;
    }

  /* One-time transfer semaphore init (shared by CS0/CS1).
   * Done after IRQ registration succeeds so a failed init leaves the
   * semaphore untouched and a later retry initialises it exactly once.
   */

  ret = nxsem_init(&ctrl->dma_sem, 0, 0);
  DEBUGASSERT(ret == OK);

  ctrl->dma_err = false;

  return OK;

err_disable_sclk:
  clk_disable(ctrl->sclk_x2);
err_disable_hclk:
  clk_disable(hclk);
  return ret;
}

/****************************************************************************
 * Private Functions — Internal Helpers
 ****************************************************************************/

/****************************************************************************
 * Name: fspi_hw_update_clock_div
 *
 * Description:
 *   Refresh the SCLK_x2 divider hardware to match this CS's expected
 *   rate (priv->freq), but only when that differs from the rate currently
 *   programmed into the hardware (ctrl->hw_freq).
 *
 *   The SCLK_x2 divider is per-controller (shared by CS0 and CS1), so
 *   this function must only be called while the controller-level mutex
 *   is held (from fspi_lock()).  Comparing priv->freq (expected) against
 *   ctrl->hw_freq (actual in HW) lets us skip the clk_set_rate() call
 *   and the associated CLK framework lock overhead when a CS requests a
 *   rate the hardware already provides.
 ****************************************************************************/

static void fspi_hw_update_clock_div(struct rk3576_fspi_s *priv)
{
  struct rk3576_fspi_ctrl_s *ctrl = priv->ctrl;
  int ret;

  /* No change required when this CS's expected rate already matches the
   * rate currently programmed into the hardware.
   */

  if (priv->freq == ctrl->hw_freq)
    {
      return;
    }

  ret = clk_set_rate(ctrl->sclk_x2, priv->freq * 2);
  if (ret < 0)
    {
      /* Divider update failed: re-read the actual rate and keep the
       * cached hw_freq consistent with hardware so the next lock()
       * call sees priv->freq != hw_freq and retries the change instead
       * of silently skipping it.
       */

      spiwarn("FSPI%u: clk_set_rate(%lu) failed, using %lu\n", ctrl->id,
              (unsigned long)priv->freq * 2,
              (unsigned long)clk_get_rate(ctrl->sclk_x2));
      ctrl->hw_freq = clk_get_rate(ctrl->sclk_x2) / 2;
      return;
    }

  ctrl->hw_freq = clk_get_rate(ctrl->sclk_x2) / 2;
}

/****************************************************************************
 * Name: fspi_interrupt
 *
 * Description:
 *   FSPI controller interrupt handler.
 *
 *   Since CS0 and CS1 share the same IRQ line, the mutex guarantees only
 *   one CS is active at a time.  We dispatch all events to the device
 *   recorded in ctrl->active.
 *
 ****************************************************************************/

static int fspi_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_fspi_ctrl_s *ctrl = (struct rk3576_fspi_ctrl_s *)arg;
  struct rk3576_fspi_s *priv = ctrl->active;
  uint32_t isr;

  /* No active device — spurious interrupt.
   * Read ISR and clear all pending bits so level-triggered IRQs
   * won't retrigger endlessly.  ctrl->regbase is always valid.
   */

  if (!priv)
    {
      isr = getreg32(ctrl->regbase + RK3576_FSPI_ISR);
      putreg32(isr, ctrl->regbase + RK3576_FSPI_ICLR);
      return OK;
    }

  /* Atomically read ISR and clear all pending interrupt bits.
   * This prevents re-entry of the same interrupt condition while
   * we dispatch the events below.  The FIFO status (FSR) is also
   * read once so both TX and RX level extraction use a consistent
   * hardware snapshot.
   */

  isr = fspi_getreg(priv, RK3576_FSPI_ISR);
  fspi_putreg(priv, isr, RK3576_FSPI_ICLR);

  if (isr & (FSPI_INT_BUS_ERR | FSPI_INT_NSPI_ERR))
    {
      ctrl->dma_err = true;
      nxsem_post(&ctrl->dma_sem);
      spierr("FSPI%u error: ISR=0x%08x\n", ctrl->id, isr);

      return OK;
    }

  if (isr & FSPI_INT_DMA)
    {
      nxsem_post(&ctrl->dma_sem);
    }

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
 *   controller share a common mutex (ctrl->lock).
 *
 *   On lock acquire, fspi_hw_update_clock_div() flushes the per-CS
 *   frequency request to the CRU hardware if the divider differs from the
 *   cached controller-level value.  This minimises CRU register access.
 ****************************************************************************/

static int fspi_lock(struct qspi_dev_s *dev, bool lock)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  struct rk3576_fspi_ctrl_s *ctrl = priv->ctrl;
  int ret;

  if (lock)
    {
      ret = nxmutex_lock(&ctrl->lock);
      if (ret < 0)
        {
          return ret;
        }

      fspi_hw_update_clock_div(priv);

      /* Record this device as active for IRQ dispatch.
       * The mutex guarantees only one CS is active at a time,
       * so the ISR can safely route interrupts to this device.
       */

      ctrl->active = priv;
    }
  else
    {
      ctrl->active = NULL;
      ret = nxmutex_unlock(&ctrl->lock);
    }

  return ret;
}

/****************************************************************************
 * Name: fspi_setfrequency
 *
 * Description:
 *   Record the requested frequency for this CS instance.  Only the
 *   expected (achievable) rate is cached in priv->freq; the actual CRU
 *   register write is deferred to fspi_lock(), which calls
 *   fspi_hw_update_clock_div() to refresh the hardware when the expected
 *   rate differs from the currently programmed one.
 ****************************************************************************/

static uint32_t fspi_setfrequency(struct qspi_dev_s *dev, uint32_t frequency)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  struct rk3576_fspi_ctrl_s *ctrl = priv->ctrl;
  uint32_t achievable;

  /* Ask the CLK framework what rate the divider can actually achieve
   * for the requested frequency.  clk_round_rate() walks the clock
   * tree and applies the hardware divider constraints without writing
   * any registers, giving us the nearest achievable rate.
   */

  DEBUGASSERT(ctrl->sclk_x2);
  achievable = clk_round_rate(ctrl->sclk_x2, frequency * 2) / 2;

  /* Record the raw request and cache the expected achievable rate.  No
   * CRU register is touched here; the actual clk_set_rate() write is
   * deferred to fspi_lock() -> fspi_hw_update_clock_div().
   */

  priv->freq_req = frequency;
  priv->freq = achievable;

  return achievable;
}

/****************************************************************************
 * Name: fspi_setmode
 ****************************************************************************/

static void fspi_setmode(struct qspi_dev_s *dev, enum qspi_mode_e mode)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;

  switch (mode)
    {
      case QSPIDEV_MODE0:
      case QSPIDEV_MODE3:
        break;
      case QSPIDEV_MODE1:
      case QSPIDEV_MODE2:
        spiwarn("rk3576 fspi peripheral does not support mode %d", mode);
        return;
      default:
        spiwarn("unknown spi mode: %d", mode);
        return;
    }

  priv->mode = mode;
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
 *
 * Description:
 *   Allocate a DMA-safe buffer for QSPI data transfer.
 *
 *   Prefers the RK3576 DMA-dedicated heap (rk3576_dma_alloc), which
 *   provides physically-contiguous memory inside the 32-bit DMA
 *   addressable range.  When CONFIG_RK3576_DMA_ALLOC is disabled we fall
 *   back to the kernel heap (kmm_malloc) which tracks the block size
 *   internally, so that path is passed through directly.
 *
 *   The size metadata is only used on the DMA path: because the QSPI_FREE()
 *   API does not carry the buffer length, but rk3576_dma_free() requires
 *   the exact size, we persist the total size in a single size_t stored at
 *   the head of the allocation.
 *
 *   To keep the returned DMA payload cache-line aligned (64B) and isolated
 *   from the metadata, the size_t header is parked alone in the first cache
 *   line (offset 0) and the payload is returned at the start of the next
 *   cache line.  This preserves the alignment rk3576_dma_alloc() provides
 *   so per-transfer D-cache maintenance never touches neighbouring data,
 *   and so the metadata is never in the same line as DMA payload.
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *   buflen - Buffer length to allocate, in bytes
 *
 * Returned Value:
 *   Pointer to the allocated buffer, or NULL on failure.
 *
 ****************************************************************************/

static void *fspi_alloc(struct qspi_dev_s *dev, size_t buflen)
{
#ifndef CONFIG_RK3576_DMA_ALLOC
#warning \
    "CONFIG_RK3576_DMA_ALLOC is disabled: FSPI buffers fall back to kmm_malloc"
  return kmm_malloc(buflen);
#else
  size_t *size;
  size_t total;
  uint8_t *base;

  /* Total block: size_t header (first cache line) + payload, rounded up to
   * a whole number of cache lines so no padding is ever needed.
   */

  total = FSPI_DMA_ALIGN_UP(sizeof(*size) + buflen);

  base = rk3576_dma_alloc(total);
  if (base == NULL)
    {
      return NULL;
    }

  /* Store the total block size in the metadata header. */

  size = (size_t *)base;
  *size = total;

  /* Return the payload at the start of the second cache line.
   * base is 64B-aligned (granule allocator), so payload = base + 64 is
   * again 64B-aligned and cache-line isolated from the metadata header.
   */

  return base + FSPI_DMA_CACHELINE;
#endif
}

/****************************************************************************
 * Name: fspi_free
 *
 * Description:
 *   Free a buffer previously allocated by fspi_alloc(), dispatching to the
 *   matching allocator.  On the DMA path the total size is recovered from
 *   the size_t header stored at the head of the allocation.
 *
 * Input Parameters:
 *   dev    - Device-specific state data
 *   buffer - Buffer previously returned by fspi_alloc()
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static void fspi_free(struct qspi_dev_s *dev, void *buffer)
{
#ifndef CONFIG_RK3576_DMA_ALLOC
  kmm_free(buffer);
#else
  size_t *size;

  DEBUGASSERT(buffer != NULL);

  /* Recover the total size from the header in the first cache line,
   * i.e. one cache line before the returned payload.
   */

  size = (size_t *)((uintptr_t)buffer - FSPI_DMA_CACHELINE);

  rk3576_dma_free(size, *size);
#endif /* CONFIG_RK3576_DMA_ALLOC */
}

/****************************************************************************
 * Name: rk3576_fspi_calc_timeout_thres
 *
 * Description:
 *   Estimate a DMA transfer timeout threshold based on the requested data
 *   length and the data line width, using the actual SCLK frequency
 *   currently programmed in the hardware (ctrl->hw_freq).
 *
 *   The pure data transfer time is approximated as:
 *
 *     clocks       = buflen * (8 / line_bits)
 *     time_us      = clocks * 1000000 / fspi_freq
 *
 *   where "8 / line_bits" is the number of SCLK cycles needed per byte:
 *     1 line (X1)  -> 8 cycles/byte  (i.e. buflen divided by 1/8)
 *     2 lines (X2) -> 4 cycles/byte  (i.e. buflen divided by 1/4)
 *     4 lines (X4) -> 2 cycles/byte  (i.e. buflen divided by 1/2)
 *
 *   Command/address/dummy cycles are negligible and ignored.  The final
 *   threshold adds FSPI_INT_TIMEOUT_US as a fixed base allowance, so short
 *   transfers always keep a sane floor while long transfers scale up with
 *   the actual amount of data being moved.
 *
 *   The timeout is computed from ctrl->hw_freq, which the owning lock()
 *   sequence refreshes to this CS's expected rate before any transfer
 *   starts.
 *
 * Input Parameters:
 *   priv      - FSPI device instance (controller holds the HW rate)
 *   buflen    - Data transfer length, in bytes
 *   line_bits - Number of data lines: 1 (X1), 2 (X2) or 4 (X4)
 *
 * Returned Value:
 *   Timeout threshold, in microseconds.
 *
 ****************************************************************************/

static inline uint32_t
rk3576_fspi_calc_timeout_thres(struct rk3576_fspi_s *priv, uint32_t buflen,
                               uint32_t line_bits)
{
  uint64_t clocks;          /* SCLK cycles needed for the data payload */
  uint64_t time_us;         /* Pure data transfer time, in us */
  uint32_t clocks_per_byte; /* SCLK cycles consumed per byte */
  uint32_t hw_freq;         /* Actual SCLK currently programmed in HW */

  DEBUGASSERT(line_bits == 1 || line_bits == 2 || line_bits == 4);

  hw_freq = priv->ctrl->hw_freq;

  /* If the hardware SCLK is unknown, the internal state is broken.
   * Fail fast: return the shortest possible timeout so the transfer
   * errors out immediately instead of stalling.
   */
  if (!hw_freq)
    {
      return 1;
    }

  /* Convert the byte count into SCLK cycles (8 / line_bits per byte). */

  clocks_per_byte = 8 / line_bits;
  clocks = (uint64_t)buflen * clocks_per_byte;

  /* Convert cycles to microseconds using the hardware SCLK. */

  time_us = (clocks * 1000000) / hw_freq;

  /* Add a fixed base allowance, guarding against 32-bit overflow. */

  if (time_us > UINT32_MAX - FSPI_INT_TIMEOUT_US)
    {
      return UINT32_MAX;
    }

  return (uint32_t)time_us + FSPI_INT_TIMEOUT_US;
}

/****************************************************************************
 * Name: fspi_wait_irq
 *
 * Description:
 *   Wait for a specific interrupt event with timeout.
 *
 *   This is a generic helper that enables the requested interrupt, waits
 *   on the corresponding semaphore, then disables the interrupt.  It is
 *   used for TX FIFO ready, RX FIFO ready, transfer complete, and (in
 *   the future) DMA completion.
 *
 * Input Parameters:
 *   priv       - FSPI device instance
 *   irq_mask   - Interrupt bit(s) to enable (FSPI_INT_xxx)
 *   sem        - Semaphore to wait on
 *   timeout_us - Timeout in microseconds
 *
 * Returned Value:
 *   OK on success, or a negative errno on failure (e.g. -ETIMEDOUT).
 ****************************************************************************/

static int fspi_wait_irq(struct rk3576_fspi_s *priv, uint32_t irq_mask,
                         sem_t *sem, uint32_t timeout_us)
{
  clock_t delay;
  uint32_t imr;
  int ret;

  /* Drain any stale semaphore count left over from a previous transfer
   * (e.g. the ISR posted before we managed to mask the interrupt).
   */

  while (nxsem_trywait(sem) >= 0)
    {
    }

  /* Enable requested interrupt */

  imr = fspi_getreg(priv, RK3576_FSPI_IMR);
  imr &= ~irq_mask;
  fspi_putreg(priv, imr, RK3576_FSPI_IMR);

  /* Calculate delay in ticks and wait */

  delay = USEC2TICK(timeout_us);
  ret = nxsem_tickwait(sem, delay);

  /* Disable requested interrupt */

  imr |= irq_mask;
  fspi_putreg(priv, imr, RK3576_FSPI_IMR);

  return ret;
}

/****************************************************************************
 * Name: fspi_wait_tx_fifo_ready
 *
 * Description:
 *   Poll until TX FIFO has at least one free slot.
 *
 *   TXLV (TX FIFO Level) — FSR bits[12:8] indicates how many 32-bit words
 *   the TX FIFO can still accept.
 *
 * Returned Value:
 *   >= 0 — number of free TX FIFO slots
 *   < 0  — negated errno on failure (e.g. -ETIMEDOUT)
 ****************************************************************************/

static int fspi_wait_tx_fifo_ready(struct rk3576_fspi_s *priv)
{
  uint32_t status;
  int poll = FSPI_POLL_MAX;

  while (poll-- > 0)
    {
      status = fspi_getreg(priv, RK3576_FSPI_FSR);
      if (status & FSPI_FSR_TXLV_MASK)
        {
          return (int)((status & FSPI_FSR_TXLV_MASK) >> FSPI_FSR_TXLV_SHIFT);
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: fspi_wait_rx_fifo_ready
 *
 * Description:
 *   Poll RX FIFO until at least one word is available.
 *
 *   RXLV (RX FIFO Level) — FSR bits[20:16] indicates how many 32-bit words
 *   are available in the RX FIFO.
 *
 * Returned Value:
 *   >= 0 — number of available RX FIFO words
 *   < 0  — negated errno on failure (e.g. -ETIMEDOUT)
 ****************************************************************************/

static int fspi_wait_rx_fifo_ready(struct rk3576_fspi_s *priv)
{
  uint32_t status;
  int poll = FSPI_POLL_MAX;

  while (poll-- > 0)
    {
      status = fspi_getreg(priv, RK3576_FSPI_FSR);
      if (status & FSPI_FSR_RXLV_MASK)
        {
          return (int)((status & FSPI_FSR_RXLV_MASK) >> FSPI_FSR_RXLV_SHIFT);
        }
    }

  return -ETIMEDOUT;
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
      tx_level = fspi_wait_tx_fifo_ready(priv);
      if (tx_level < 0)
        {
          spierr("FSPI TX FIFO wait failed: %d\n", tx_level);
          return tx_level;
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
      tx_level = fspi_wait_tx_fifo_ready(priv);
      if (tx_level < 0)
        {
          spierr("FSPI TX FIFO tail wait failed: %d\n", tx_level);
          return tx_level;
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
      rx_level = fspi_wait_rx_fifo_ready(priv);
      if (rx_level < 0)
        {
          spierr("FSPI RX FIFO wait failed: %d\n", rx_level);
          return rx_level;
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
      rx_level = fspi_wait_rx_fifo_ready(priv);
      if (rx_level < 0)
        {
          spierr("FSPI RX FIFO tail wait failed: %d\n", rx_level);
          return rx_level;
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
 * Name: fspi_busy
 *
 * Description:
 *   Poll FSPI_SR until the controller is idle (IS_BUSY == 0).
 *
 ****************************************************************************/

static int fspi_wait_busy(struct rk3576_fspi_s *priv)
{
  unsigned int poll_cnt = FSPI_POLL_MAX;

  while (fspi_getreg(priv, RK3576_FSPI_SR) & FSPI_SR_IS_BUSY)
    {
      if (poll_cnt-- == 0)
        {
          spierr("FSPI%u busy timeout\n", priv->ctrl->id);
          return -ETIMEDOUT;
        }
    }

  return OK;
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
  uint32_t ctrl_rge_bits, cmd_reg_bits;
  int ret;

  DEBUGASSERT(cmdinfo);
  DEBUGASSERT(!cmdinfo->buflen || cmdinfo->buffer);

  /* CTRL config begin */

  ctrl_rge_bits = FSPI_CTRL_SHIFTPHASE_NEGEDGE;

  if (priv->mode == QSPIDEV_MODE3)
    {
      ctrl_rge_bits |= FSPI_CTRL_SPIM_MODE_3;
    }
  else if (priv->mode == QSPIDEV_MODE0)
    {
      ctrl_rge_bits |= FSPI_CTRL_SPIM_MODE_0;
    }
  else
    {
      spierr("Unsupported spi mode %u", priv->mode);
      return -EINVAL;
    }

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

  /* CTRL config end */

  /* CMD config begin */

  /* setup cs */
  cmd_reg_bits = FSPI_CMD_CS(priv->cs);

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
      spierr("8-bit cmd expected, got 0x%X", cmdinfo->cmd);
      return -EINVAL;
    }
  cmd_reg_bits |= (cmdinfo->cmd << FSPI_CMD_OPCODE_SHIFT);

  /* CMD config end */

  /* wait for SR */
  ret = fspi_wait_busy(priv);
  if (ret < 0)
    {
      goto transmission_failure;
    }

  /* setup data length */
  fspi_putreg(priv, cmdinfo->buflen, RK3576_FSPI_LEN_EXT);
  fspi_putreg(priv, FSPI_LEN_CTRL_TRB_SEL, RK3576_FSPI_LEN_CTRL);

  /* setup CTRL and CMD register */
  fspi_putreg(priv, ctrl_rge_bits, RK3576_FSPI_CTRL(priv->cs));
  fspi_putreg(priv, cmd_reg_bits, RK3576_FSPI_CMD);

  /* send addr*/
  if ((cmdinfo->flags & QSPICMD_ADDRESS) && (cmdinfo->addrlen))
    {
      fspi_putreg(priv, cmdinfo->addr, RK3576_FSPI_ADDR);
    }

  /* skip data if len==0 */
  if (!cmdinfo->buflen)
    {
      return OK;
    }

  /* transfer data via FIFO */

  if (cmdinfo->flags & QSPICMD_WRITEDATA)
    {
      ret = fspi_write_fifo(priv, cmdinfo->buffer, cmdinfo->buflen);
      if (ret < 0)
        {
          goto transmission_failure;
        }
    }
  else if (cmdinfo->flags & QSPICMD_READDATA)
    {
      ret = fspi_read_fifo(priv, cmdinfo->buffer, cmdinfo->buflen);
      if (ret < 0)
        {
          goto transmission_failure;
        }
    }

  return OK;

transmission_failure:
  fspi_hw_reset(priv);
  return ret;
}

/****************************************************************************
 * Name: fspi_memory
 *
 * Description:
 *   Perform one QSPI memory transfer
 ****************************************************************************/

static int fspi_memory(struct qspi_dev_s *dev, struct qspi_meminfo_s *meminfo)
{
  struct rk3576_fspi_s *priv = (struct rk3576_fspi_s *)dev;
  uint32_t ctrl_rge_bits, cmd_reg_bits;
  int ret;

  DEBUGASSERT(meminfo);
  DEBUGASSERT(!meminfo->buflen || meminfo->buffer);

  /* CTRL config begin */

  ctrl_rge_bits = FSPI_CTRL_SHIFTPHASE_NEGEDGE;

  if (priv->mode == QSPIDEV_MODE3)
    {
      ctrl_rge_bits |= FSPI_CTRL_SPIM_MODE_3;
    }
  else if (priv->mode == QSPIDEV_MODE0)
    {
      ctrl_rge_bits |= FSPI_CTRL_SPIM_MODE_0;
    }
  else
    {
      spierr("Unsupported spi mode %u", priv->mode);
      return -EINVAL;
    }

  if (meminfo->flags & QSPIMEM_IDUAL)
    {
      ctrl_rge_bits |= FSPI_CTRL_CMDB_X2;
    }
  else if (meminfo->flags & QSPIMEM_IQUAD)
    {
      ctrl_rge_bits |= FSPI_CTRL_CMDB_X4;
    }
  else
    {
      ctrl_rge_bits |= FSPI_CTRL_CMDB_X1;
    }

  if (meminfo->flags & QSPIMEM_DUALIO)
    {
      ctrl_rge_bits |= FSPI_CTRL_ADDRB_X2;
      ctrl_rge_bits |= FSPI_CTRL_DATAB_X2;
    }
  else if (meminfo->flags & QSPIMEM_QUADIO)
    {
      ctrl_rge_bits |= FSPI_CTRL_ADDRB_X4;
      ctrl_rge_bits |= FSPI_CTRL_DATAB_X4;
    }
  else
    {
      ctrl_rge_bits |= FSPI_CTRL_ADDRB_X1;
      ctrl_rge_bits |= FSPI_CTRL_DATAB_X1;
    }

  /* CTRL config end */

  /* CMD config begin */

  /* setup cs */
  cmd_reg_bits = FSPI_CMD_CS(priv->cs);

  /* setup addr length */
  switch (meminfo->addrlen)
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
               meminfo->addrlen);
        return -EINVAL;
    }

  /* setup WR */
  if (meminfo->flags & QSPIMEM_WRITE)
    {
      cmd_reg_bits |= FSPI_CMD_DIR_WR;
    }
  else
    {
      cmd_reg_bits |= FSPI_CMD_DIR_RD;
    }

  /* setup dummy cycles (only in read mode) */
  if (meminfo->buflen && !(meminfo->flags & QSPIMEM_WRITE))
    {
      if (meminfo->dummies > 15)
        {
          spierr("dummies out of range [0, 15], got %u", meminfo->dummies);
          return -EINVAL;
        }
      cmd_reg_bits |= (meminfo->dummies << FSPI_CMD_DUMMY_SHIFT);
    }
  else
    {
      /* 0 dummy cycles */
      cmd_reg_bits |= (0 << FSPI_CMD_DUMMY_SHIFT);
    }

  /* non-continuous mode */
  cmd_reg_bits |= FSPI_CMD_CONT_DISABLE;

  /* setup cmd */
  if (meminfo->cmd > 0xff)
    {
      spierr("8-bit cmd expected, got 0x%X", meminfo->cmd);
      return -EINVAL;
    }
  cmd_reg_bits |= (meminfo->cmd << FSPI_CMD_OPCODE_SHIFT);

  /* CMD config end */

  /* wait for SR */
  ret = fspi_wait_busy(priv);
  if (ret < 0)
    {
      goto transmission_failure;
    }

  /* setup data length */
  fspi_putreg(priv, meminfo->buflen, RK3576_FSPI_LEN_EXT);
  fspi_putreg(priv, FSPI_LEN_CTRL_TRB_SEL, RK3576_FSPI_LEN_CTRL);

  /* setup CTRL and CMD register */
  fspi_putreg(priv, ctrl_rge_bits, RK3576_FSPI_CTRL(priv->cs));
  fspi_putreg(priv, cmd_reg_bits, RK3576_FSPI_CMD);

  /* send addr*/
  if (meminfo->addrlen)
    {
      fspi_putreg(priv, meminfo->addr, RK3576_FSPI_ADDR);
    }

  /* skip data if len==0 */
  if (!meminfo->buflen)
    {
      return OK;
    }

  uintptr_t physical_addr = up_addrenv_va_to_pa(meminfo->buffer);

  /* Transfer data via the FSPI internal DMA engine.
   *
   * The DMA path is only used when ALL of the following hold:
   *
   *   1) The transfer is large enough (>= FSPI_DMA_LEN_THRES).  For small
   *      transfers the DMA setup/teardown and interrupt overhead is not
   *      worth it, so they always go through the FIFO polling path.
   *
   *   2) The length is a multiple of 4.  The built-in DMA engine moves
   *      data in 32-bit word units, and the total transfer byte count
   *      (TRB / LEN_EXT) must match what the FSM actually shifts out.
   *      If len is 2-byte but not 4-byte aligned (e.g. 0x82), the DMA
   *      engine only shifts whole words (0x80 bytes): the FSM is left
   *      waiting for the missing 1-3 bytes, busy stays asserted, CS stays
   *      low, and wait_busy() below times out.  The data buffer length
   *      must therefore be a multiple of 4 bytes to use the DMA path.
   *
   *   3) The whole buffer (physical address PLUS its length) fits in
   *      32 bits.  RK3576_FSPI_DMAADDR is a 32-bit register, so the
   *      source/destination buffer must reside entirely within the lower
   *      4 GiB of the physical address space for the built-in DMA engine
   *      to address it.  A buffer that merely STARTS below 4 GiB but
   *      crosses the 4 GiB boundary would make the DMA engine wrap around
   *      and corrupt data, so both the base and the end address must be
   *      checked.  Buffers above 4 GiB (e.g. high-DRAM) fall back to the
   *      FIFO polling path instead.
   *
   * Anything else falls through to the FIFO polling path below, which
   * handles the trailing 1-3 bytes correctly via the SFC_DATA register.
   */

  if (meminfo->buflen >= FSPI_DMA_LEN_THRES && (meminfo->buflen % 4) == 0 &&
      physical_addr <= 0xffffffffu &&
      physical_addr + meminfo->buflen - 1 <= 0xffffffffu)
    {
      uint32_t line_bits;
      uint32_t timeout_us;

      /* Determine the data line width.  The DATA phase dominates the
       * transfer time, so only the DUALIO/QUADIO flags matter here; they
       * match the DATAB_X1/X2/X4 bits programmed into the CTRL register
       * above.  Used to scale the DMA timeout with the data volume.
       */

      if (meminfo->flags & QSPIMEM_QUADIO)
        {
          line_bits = 4;
        }
      else if (meminfo->flags & QSPIMEM_DUALIO)
        {
          line_bits = 2;
        }
      else
        {
          line_bits = 1;
        }

      timeout_us =
          rk3576_fspi_calc_timeout_thres(priv, meminfo->buflen, line_bits);

      fspi_putreg(priv, (unsigned int)physical_addr, RK3576_FSPI_DMAADDR);

      if (meminfo->flags & QSPIMEM_WRITE)
        {
          up_clean_dcache((uintptr_t)meminfo->buffer,
                          (uintptr_t)meminfo->buffer + meminfo->buflen);
        }

      fspi_putreg(priv, FSPI_DMA_TRIGGER_START, RK3576_FSPI_DMATR);

      /* Clear any stale error flag from a previous transfer, then wait
       * for DMA completion OR an error.  The error bits must be included
       * in the mask so the ISR's error branch is actually reachable;
       * otherwise a BUS_ERR/NSPI_ERR is silently masked and we would only
       * discover it via timeout.
       */

      priv->ctrl->dma_err = false;
      ret = fspi_wait_irq(
          priv, (FSPI_INT_DMA | FSPI_INT_BUS_ERR | FSPI_INT_NSPI_ERR),
          &(priv->ctrl->dma_sem), timeout_us);
      if (ret < 0)
        {
          goto transmission_failure;
        }

      if (priv->ctrl->dma_err)
        {
          ret = -EIO;
          goto transmission_failure;
        }

      /* Read path: DMA has written the buffer into memory.  Invalidate
       * the D-cache so a subsequent CPU read misses and fetches the fresh
       * DMA data, instead of hitting a stale line held since before the
       * transfer.  Write path needs no post-DMA maintenance.
       *
       * As above, this operates by virtual address meminfo->buffer.
       */

      if (!(meminfo->flags & QSPIMEM_WRITE))
        {
          up_invalidate_dcache((uintptr_t)meminfo->buffer,
                               (uintptr_t)meminfo->buffer + meminfo->buflen);
        }

      return OK;
    }

  /* Fall back to FIFO polling. */

  if (meminfo->flags & QSPIMEM_WRITE)
    {
      ret = fspi_write_fifo(priv, meminfo->buffer, meminfo->buflen);
      if (ret < 0)
        {
          goto transmission_failure;
        }
    }
  else
    {
      ret = fspi_read_fifo(priv, meminfo->buffer, meminfo->buflen);
      if (ret < 0)
        {
          goto transmission_failure;
        }
    }

  return OK;

transmission_failure:
  fspi_hw_reset(priv);
  return ret;
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
  struct rk3576_fspi_ctrl_s *ctrl;
  struct rk3576_fspi_s *priv;

  /* Validate arguments */

  if (fspi_id < 0 || fspi_id >= RK3576_FSPI_NUM_CONTROLLERS || cs < 0 ||
      cs >= RK3576_FSPI_NUM_CHIPSELECTS)
    {
      spierr("Invalid FSPI instance: id=%d cs=%d\n", fspi_id, cs);
      return NULL;
    }

  ctrl = &g_fspi_ctrl[fspi_id];
  priv = &ctrl->devs[cs];

  /* Wire the back-pointer (idempotent, safe to do on every call) */

  priv->ctrl = ctrl;

  /* Lock the controller-level mutex to serialize hardware init */

  nxmutex_lock(&ctrl->lock);

  /* CS instance already initialized? */

  if (priv->initialized)
    {
      nxmutex_unlock(&ctrl->lock);
      return &priv->qspi;
    }

  /* Hardware initialization — done only once per controller.
   * The first call (for either CS0 or CS1) performs the one-time
   * hardware init.  Subsequent calls for the other CS skip it.
   */

  if (!ctrl->hw_initialized)
    {
      int ret;

      ret = fspi_hw_init(priv);
      if (ret < 0)
        {
          nxmutex_unlock(&ctrl->lock);
          return NULL;
        }

      ctrl->hw_initialized = true;
    }

  /* Set default frequency and mode for this CS instance */

  fspi_setfrequency(&priv->qspi, 10000000); /* 10MHz */
  fspi_setmode(&priv->qspi, QSPIDEV_MODE0);

  priv->initialized = true;

  nxmutex_unlock(&ctrl->lock);

  return &priv->qspi;
}

#endif /* CONFIG_RK3576_FSPI */
