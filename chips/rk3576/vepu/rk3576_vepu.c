/****************************************************************************
 * chips/rk3576/vepu/rk3576_vepu.c
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
 * RK3576 VEPU0 (VEPU510 hardware video encoder) -- bring-up.
 *
 * This file currently does power, reset and clock bring-up plus a version
 * probe.  It owns no capture/codec state yet; the encoder driver is layered
 * on top.
 *
 * PD_VEPU0 has two traps worth repeating:
 *
 *   - Its memory-repair initial reset resets to 0, i.e. HELD IN RESET.  Until
 *     it is released, every VEPU0 register reads back as zero.
 *   - Its bus interface unit can be held idle independently of the clocks.
 *     A module in that state reads and writes registers perfectly while
 *     being unable to issue a single AXI transaction.  (rk3576_vicap.c
 *     documents the same trap for PD_VI.)
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_RK3576_VEPU

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_vepu.h"
#include "hardware/rk3576_vepu510_reg.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_vepu.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_vepu_clks_s
{
  FAR struct clk_s *core;     /* clk_vepu0_core   -- encoder clock      */
  FAR struct clk_s *aclk;     /* aclk_vepu0      -- AXI register/DMA    */
  FAR struct clk_s *hclk;     /* hclk_vepu0      -- AHB register access */
  FAR struct clk_s *aclk_biu; /* aclk_vepu0_biu  -- AXI bus interface   */
  FAR struct clk_s *hclk_biu; /* hclk_vepu0_biu  -- AHB bus interface   */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_vepu_clks_s g_vepu_clks;
static bool g_vepu_powered;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_vepu_getreg(uint32_t offset)
{
  return getreg32(RK3576_VEPU0_ADDR + offset);
}

static void rk3576_vepu_putreg(uint32_t offset, uint32_t value)
{
  putreg32(value, RK3576_VEPU0_ADDR + offset);
}

/****************************************************************************
 * Name: rk3576_vepu_power_domain_on
 *
 * Description:
 *   Release PD_VEPU0's memory-repair initial reset, make sure the domain is
 *   not asked to power down, and leave the VEPU0 bus interface unit
 *   running.
 *
 ****************************************************************************/

static int rk3576_vepu_power_domain_on(void)
{
  uintptr_t pmu = RK3576_PMU_ADDR;
  int loops = RK3576_VEPU_PMU_POLL_LOOPS;

  /* 1. Release the memory-repair initial reset (write 1 = Normal).
   *
   *    This is the step that makes the domain readable.  Skip it and every
   *    VEPU0 register -- including the version register -- reads back 0.
   */

  putreg32(RK3576_VEPU_PMU_HWM(RK3576_VEPU_PD0_INITRST_BIT) |
               RK3576_VEPU_PD0_INITRST_BIT,
           pmu + RK3576_VEPU_PMU_BISR_INITRST_SFTCON1_OFF);
  up_udelay(20);

  /* 2. Keep the domain powered: clear both the hardware and the software
   *    power-down request bits.  Note that the software bit lives in
   *    PWR_GATE_SFTCON1 (0x20214), not in SFTCON0 -- SFTCON0 holds the
   *    VO0/VO1/VOP/PHP/audio/GMAC/NVM/CCI/DDR/center/secure/bus/NPU
   *    domains instead.
   */

  putreg32(RK3576_VEPU_PMU_HWM(RK3576_VEPU_PD0_DWN_ENA_BIT),
           pmu + RK3576_VEPU_PMU_PWR_GATE_CON1_OFF);
  putreg32(RK3576_VEPU_PMU_HWM(RK3576_VEPU_PD0_DWN_SFTENA_BIT),
           pmu + RK3576_VEPU_PMU_PWR_GATE_SFTCON1_OFF);

  /* 3. Do not hold the bus interface unit idle.  A module whose BIU is held
   *    idle programs and reads its registers perfectly while being unable
   *    to issue a single memory transaction.
   */

  putreg32(RK3576_VEPU_PMU_HWM(RK3576_VEPU_BIU0_IDLE_REQ_BIT),
           pmu + RK3576_VEPU_PMU_BIU_IDLE_SFTCON0_OFF);
  up_udelay(20);

  /* 4. Wait for the domain to report powered up.  The status bit is 0 while
   *    the domain is up, so this waits for it to clear.
   */

  while (loops-- > 0)
    {
      if ((getreg32(pmu + RK3576_VEPU_PMU_PWR_GATE_STS_OFF) &
           RK3576_VEPU_PD0_DWN_STAT_BIT) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  _err("ERROR: VEPU0 PD_VEPU0 never reported powered up\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_vepu_release_resets
 *
 * Description:
 *   Release the VEPU0 CRU software resets.  All of them are "when high,
 *   reset", so the hiword mask selects the bits and the low word carries 0
 *   to release them.  Only the release side is written: nothing is running
 *   yet, and the assert side has no defined purpose here.
 *
 ****************************************************************************/

static void rk3576_vepu_release_resets(void)
{
  uintptr_t cru = RK3576_CRU_ADDR;

  putreg32(RK3576_VEPU_RESET_BITS << 16,
           cru + RK3576_CRU_SOFTRST_CON(RK3576_VEPU_CRU_SOFTRST_CON));
  up_udelay(20);
}

/****************************************************************************
 * Name: rk3576_vepu_assert_resets
 ****************************************************************************/

static void rk3576_vepu_assert_resets(void)
{
  uintptr_t cru = RK3576_CRU_ADDR;

  putreg32(RK3576_VEPU_RESET_BITS | (RK3576_VEPU_RESET_BITS << 16),
           cru + RK3576_CRU_SOFTRST_CON(RK3576_VEPU_CRU_SOFTRST_CON));
  up_udelay(20);
}

/****************************************************************************
 * Name: rk3576_vepu_enable_clocks
 *
 * Description:
 *   Look up and enable the VEPU0 clocks.  clk_enable() walks the parent
 *   chain, so enabling a leaf also opens the root gates it hangs from; the
 *   bus-interface gates are enabled explicitly because they are not in any
 *   parent chain that the leaves traverse.
 *
 *   All of these gates reset to 0, i.e. enabled, so out of reset this is
 *   bookkeeping rather than a fix.  It is still done so the clock tree's
 *   accounting matches the hardware and nothing else can gate VEPU0 off
 *   behind the driver's back.
 *
 ****************************************************************************/

static int rk3576_vepu_enable_clocks(void)
{
  g_vepu_clks.core = clk_get("clk_vepu0_core");
  g_vepu_clks.aclk = clk_get("aclk_vepu0");
  g_vepu_clks.hclk = clk_get("hclk_vepu0");
  g_vepu_clks.aclk_biu = clk_get("aclk_vepu0_biu");
  g_vepu_clks.hclk_biu = clk_get("hclk_vepu0_biu");

  if (g_vepu_clks.core == NULL || g_vepu_clks.aclk == NULL ||
      g_vepu_clks.hclk == NULL || g_vepu_clks.aclk_biu == NULL ||
      g_vepu_clks.hclk_biu == NULL)
    {
      _err("ERROR: VEPU0 failed to look up its clocks\n");
      return -ENODEV;
    }

  if (clk_enable(g_vepu_clks.core) < 0 || clk_enable(g_vepu_clks.aclk) < 0 ||
      clk_enable(g_vepu_clks.hclk) < 0)
    {
      _err("ERROR: VEPU0 failed to enable its clocks\n");
      return -EIO;
    }

  if (clk_enable(g_vepu_clks.aclk_biu) < 0 ||
      clk_enable(g_vepu_clks.hclk_biu) < 0)
    {
      _err("ERROR: VEPU0 failed to enable its bus interface clocks\n");
      return -EIO;
    }

  /* Report what the clock tree can resolve.  clk_vepu0_core's selector
   * resets to SPLL, which this tree does not model, so its rate reads back
   * as 0 until the encoder driver pins a source with clk_set_parent().
   */

  _info("VEPU0: aclk=%" PRIu32 " hclk=%" PRIu32 " core=%" PRIu32
        " (core unresolvable while its mux selects the unmodelled SPLL)\n",
        clk_get_rate(g_vepu_clks.aclk), clk_get_rate(g_vepu_clks.hclk),
        clk_get_rate(g_vepu_clks.core));

  return OK;
}

/****************************************************************************
 * Name: rk3576_vepu_disable_clocks
 ****************************************************************************/

static void rk3576_vepu_disable_clocks(void)
{
  if (g_vepu_clks.hclk_biu != NULL)
    {
      clk_disable(g_vepu_clks.hclk_biu);
      g_vepu_clks.hclk_biu = NULL;
    }

  if (g_vepu_clks.aclk_biu != NULL)
    {
      clk_disable(g_vepu_clks.aclk_biu);
      g_vepu_clks.aclk_biu = NULL;
    }

  if (g_vepu_clks.hclk != NULL)
    {
      clk_disable(g_vepu_clks.hclk);
      g_vepu_clks.hclk = NULL;
    }

  if (g_vepu_clks.aclk != NULL)
    {
      clk_disable(g_vepu_clks.aclk);
      g_vepu_clks.aclk = NULL;
    }

  if (g_vepu_clks.core != NULL)
    {
      clk_disable(g_vepu_clks.core);
      g_vepu_clks.core = NULL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_vepu_power_on(void)
{
  int ret;

  if (g_vepu_powered)
    {
      return OK;
    }

  ret = rk3576_vepu_power_domain_on();
  if (ret < 0)
    {
      return ret;
    }

  rk3576_vepu_release_resets();

  ret = rk3576_vepu_enable_clocks();
  if (ret < 0)
    {
      rk3576_vepu_assert_resets();
      return ret;
    }

  g_vepu_powered = true;
  return OK;
}

int rk3576_vepu_power_off(void)
{
  uintptr_t pmu = RK3576_PMU_ADDR;

  if (!g_vepu_powered)
    {
      return OK;
    }

  rk3576_vepu_disable_clocks();

  /* Put the domain back the way it was found: resets asserted, initial
   * reset back to Reset, and the initial reset request re-armed.
   */

  rk3576_vepu_assert_resets();

  putreg32(RK3576_VEPU_PMU_HWM(RK3576_VEPU_PD0_INITRST_BIT),
           pmu + RK3576_VEPU_PMU_BISR_INITRST_SFTCON1_OFF);
  putreg32(RK3576_VEPU_PMU_HWM(RK3576_VEPU_PD0_DWN_ENA_BIT) |
               RK3576_VEPU_PD0_DWN_ENA_BIT,
           pmu + RK3576_VEPU_PMU_PWR_GATE_CON1_OFF);
  putreg32(RK3576_VEPU_PMU_HWM(RK3576_VEPU_PD0_DWN_SFTENA_BIT) |
               RK3576_VEPU_PD0_DWN_SFTENA_BIT,
           pmu + RK3576_VEPU_PMU_PWR_GATE_SFTCON1_OFF);

  g_vepu_powered = false;
  return OK;
}

uint32_t rk3576_vepu_read_version(void)
{
  return rk3576_vepu_getreg(RK3576_VEPU510_VERSION_OFFSET);
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* One job at a time.  The IP holds a single encoder, so a second caller
 * waits rather than interleaving its register writes with the first's.
 */

static mutex_t g_vepu_lock = NXMUTEX_INITIALIZER;

/* Completion signalling.
 *
 * The ISR records the status it saw and posts; the waiter reads the status
 * rather than trusting the count.  A completion that arrives after its own
 * waiter has given up must not be taken for the next job's, and a bare
 * count cannot tell the two apart.
 */

static sem_t g_vepu_done;
static bool g_vepu_sem_ready;
static volatile uint32_t g_vepu_intsta;

static bool g_vepu_irq_attached;

/* The register image.
 *
 * Nearly 4 KB, which is more than this driver's callers should have to find
 * on their stacks.  Used only under g_vepu_lock, so one instance serves
 * every job; regs_build() starts it from a clean slate each time.
 */

static HalVepu510RegSet g_vepu_regs;

/* The reconstruction working set.
 *
 * One slot, not MPP's four: the encoder reconstructs exactly one picture per
 * job, this driver runs one job at a time, and an all-intra stream never
 * reads a reconstruction back as a reference.  The size is remembered so a
 * change of picture geometry can be noticed and the old buffers released.
 */

static FAR void *g_vepu_recn_pixel;
static FAR void *g_vepu_recn_thumb;
static FAR void *g_vepu_recn_smear;
static struct rk3576_vepu510_recn_size_s g_vepu_recn_size;
static uint32_t g_vepu_recn_width;
static uint32_t g_vepu_recn_height;

/* How long to wait for a job before deciding the hardware is not going to
 * answer.  1080p is a few milliseconds on this IP, so a second is not a
 * deadline, it is the point at which waiting has stopped being useful.
 */

#define RK3576_VEPU_JOB_TIMEOUT_MS 1000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vepu_write_block
 *
 * Description:
 *   Write one register group.  The encoder is programmed a block at a time
 *   rather than register by register, which is what the block offsets in
 *   hardware/rk3576_vepu.h are: the units the hardware is written in.
 *
 ****************************************************************************/

static void rk3576_vepu_write_block(uintptr_t base, uint32_t offset,
                                    FAR const void *block, size_t size)
{
  FAR const uint32_t *src = (FAR const uint32_t *)block;
  size_t count = size / sizeof(uint32_t);
  size_t i;

  for (i = 0; i < count; i++)
    {
      putreg32(src[i], base + offset + (uint32_t)i * sizeof(uint32_t));
    }
}

/****************************************************************************
 * Name: rk3576_vepu_collect_status
 *
 * Description:
 *   Ask the encoder for the rest of its completion status and return
 *   everything that is now latched.
 *
 *   The vendor kernel does this after every VEPU510 job, and both halves are
 *   load-bearing.  Clearing the encoder-id register is what prompts the IP to
 *   update its done status -- the kernel's comment is "after config the
 *   register, the encoder will update enc done int status" -- so a driver
 *   that skipped it would never see a late ENC_DONE at all.  The short delay
 *   is the same one the kernel uses for that update to land.
 *
 *   Called from task context rather than from the interrupt: it sleeps, and
 *   the status is not needed until something is waiting for it.
 *
 ****************************************************************************/

static bool rk3576_vepu_drain_slice_fifo(FAR uint32_t *length)
{
  uint32_t count;
  uint32_t total = 0;
  bool last = false;
  uint32_t i;

  count = rk3576_vepu_getreg(RK3576_VEPU510_SLICE_NUM_OFFSET) &
          RK3576_VEPU510_SLICE_NUM_MASK;

  /* The count comes from the hardware, so it is bounded before it is used as
   * a loop limit.  A record read past the end of the FIFO is not a repeat of
   * the last one, it is a fabricated length, and it would be added to the
   * frame's total with nothing to show for it.
   */

  if (count > RK3576_VEPU510_SLICE_FIFO_LEN)
    {
      count = RK3576_VEPU510_SLICE_FIFO_LEN;
    }

  for (i = 0; i < count; i++)
    {
      uint32_t record = rk3576_vepu_getreg(RK3576_VEPU510_SLICE_LEN_OFFSET);

      total += record & RK3576_VEPU510_SLICE_LEN_MASK;

      if ((record & RK3576_VEPU510_SLICE_LAST) != 0)
        {
          last = true;
        }
    }

  *length = total;
  return last;
}

static uint32_t rk3576_vepu_collect_status(uint32_t intsta)
{
  uintptr_t base = RK3576_VEPU0_ADDR;

  putreg32(0, base + RK3576_VEPU510_DVBM_STATE_OFFSET);
  up_udelay(5);

  intsta |= rk3576_vepu_getreg(RK3576_VEPU510_INT_STA_OFFSET);

  /* Clear whatever appeared, so it cannot be taken for the next job's
   * completion.  The job path masks the interrupt and drains the semaphore
   * before starting, so this is belt and braces -- but a latched done bit is
   * exactly the thing that would be misread as an instant completion.
   *
   * This happens after the slice FIFO has been read, not before: the
   * interrupt is raised by the FIFO being non-empty, so clearing it while a
   * record is still sitting there asks for the same interrupt a second time.
   */

  putreg32(intsta, base + RK3576_VEPU510_INT_CLR_OFFSET);

  return intsta;
}

/****************************************************************************
 * Name: rk3576_vepu_mmu_passthrough
 *
 * Description:
 *   Leave the MMU in front of the encoder translating nothing.
 *
 *   Every address this driver hands the encoder is already physical and
 *   contiguous, so there is nothing to translate and a page table would only
 *   be a way to fault.  The registers are written rather than left alone:
 *   "paging happens to be off after reset" is not something a driver should
 *   depend on.  rk3576_vicap.c treats its own MMU the same way.
 *
 ****************************************************************************/

static void rk3576_vepu_mmu_passthrough(void)
{
  uintptr_t mmu = RK3576_VEPU0_MMU_ADDR;

  putreg32(0, mmu + RK3576_VEPU_MMU_DTE_ADDR);
  putreg32(RK3576_VEPU_MMU_INT_ALL, mmu + RK3576_VEPU_MMU_INT_CLEAR);
  putreg32(RK3576_VEPU_MMU_INT_ALL, mmu + RK3576_VEPU_MMU_INT_MASK);
}

/****************************************************************************
 * Name: rk3576_vepu_recn_free
 *
 * Description:
 *   Release the reconstruction working set.  The caller must hold the driver
 *   mutex.
 *
 ****************************************************************************/

static void rk3576_vepu_recn_free(void)
{
  if (g_vepu_recn_pixel != NULL)
    {
      rk3576_dma_free(g_vepu_recn_pixel, g_vepu_recn_size.pixel);
      g_vepu_recn_pixel = NULL;
    }

  if (g_vepu_recn_thumb != NULL)
    {
      rk3576_dma_free(g_vepu_recn_thumb, g_vepu_recn_size.thumb);
      g_vepu_recn_thumb = NULL;
    }

  if (g_vepu_recn_smear != NULL)
    {
      rk3576_dma_free(g_vepu_recn_smear, g_vepu_recn_size.smear);
      g_vepu_recn_smear = NULL;
    }

  g_vepu_recn_width = 0;
  g_vepu_recn_height = 0;
}

/****************************************************************************
 * Name: rk3576_vepu_recn_alloc
 *
 * Description:
 *   Make sure the reconstruction working set matches this picture, and
 *   describe it in frm.  The caller must hold the driver mutex.
 *
 *   The buffers come from the DMA heap because the encoder writes them
 *   directly: they have to be physically contiguous and below 4 GB, which
 *   the MMU's pass-through route makes the whole requirement.
 *
 ****************************************************************************/

static int rk3576_vepu_recn_alloc(FAR struct rk3576_vepu510_frame_s *frm)
{
  struct rk3576_vepu510_recn_size_s size;

  if (g_vepu_recn_pixel != NULL && g_vepu_recn_width == frm->width &&
      g_vepu_recn_height == frm->height)
    {
      goto describe;
    }

  /* A different geometry: the old set is the wrong size, so it is released
   * before the new one is taken.  Holding both would peak at twice the
   * footprint for no reason.
   */

  rk3576_vepu_recn_free();

  rk3576_vepu510_recn_size(frm->width, frm->height, &size);
  g_vepu_recn_size = size;

  g_vepu_recn_pixel = rk3576_dma_alloc(size.pixel);
  if (g_vepu_recn_pixel == NULL)
    {
      goto errout_nomem;
    }

  g_vepu_recn_thumb = rk3576_dma_alloc(size.thumb);
  if (g_vepu_recn_thumb == NULL)
    {
      goto errout_nomem;
    }

  g_vepu_recn_smear = rk3576_dma_alloc(size.smear);
  if (g_vepu_recn_smear == NULL)
    {
      goto errout_nomem;
    }

  g_vepu_recn_width = frm->width;
  g_vepu_recn_height = frm->height;

  _info("VEPU0: reconstruction set for %ux%u: pixel %" PRIu32
        " (header %" PRIu32 "), thumb %" PRIu32 ", smear %" PRIu32 "\n",
        (unsigned int)frm->width, (unsigned int)frm->height, size.pixel,
        size.header, size.thumb, size.smear);

describe:
  /* The published pointer is the virtual address, which for this heap is
   * also the physical one.  Stating that in one place beats scattering
   * casts through the register layer.
   */

  frm->recn.pixel_phys = (uint32_t)(uintptr_t)g_vepu_recn_pixel;
  frm->recn.body_offset = g_vepu_recn_size.header;
  frm->recn.thumb_phys = (uint32_t)(uintptr_t)g_vepu_recn_thumb;
  frm->recn.smear_phys = (uint32_t)(uintptr_t)g_vepu_recn_smear;

  return OK;

errout_nomem:
  _err("ERROR: VEPU0 out of DMA heap for the reconstruction set "
       "(pixel %" PRIu32 ", thumb %" PRIu32 ", smear %" PRIu32 ")\n",
       size.pixel, size.thumb, size.smear);
  rk3576_vepu_recn_free();
  return -ENOMEM;
}

/****************************************************************************
 * Name: rk3576_vepu_isr
 *
 * Description:
 *   Completion interrupt: acknowledge the block, record what it said and
 *   wake the job waiting for it.
 *
 ****************************************************************************/

static int rk3576_vepu_isr(int irq, FAR void *context, FAR void *arg)
{
  uintptr_t base = RK3576_VEPU0_ADDR;
  uint32_t intsta;

  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  intsta = rk3576_vepu_getreg(RK3576_VEPU510_INT_STA_OFFSET);
  if (intsta == 0)
    {
      /* Nothing set: not this block's interrupt, or already acknowledged. */

      return OK;
    }

  /* Acknowledge by writing the bits back, and mask the block so that a
   * second edge cannot arrive before the waiter has dealt with this one.
   *
   * int_en is deliberately left alone.  It is an ordinary enable register --
   * enc_strt is the one that starts the encoder -- but rewriting an enable
   * register from the completion path would make the interrupt setup depend
   * on how the previous job ended.
   */

  putreg32(intsta, base + RK3576_VEPU510_INT_CLR_OFFSET);
  putreg32(0xffffffffu, base + RK3576_VEPU510_INT_MSK_OFFSET);

  g_vepu_intsta = intsta;
  nxsem_post(&g_vepu_done);

  return OK;
}

int rk3576_vepu_probe(void)
{
  uint32_t ver;
  int ret;

  ret = rk3576_vepu_power_on();
  if (ret < 0)
    {
      _err("ERROR: VEPU0 power-on failed: %d\n", ret);
      return ret;
    }

  ver = rk3576_vepu_read_version();

  if (ver == 0)
    {
      _err("ERROR: VEPU0 version register reads 0 -- PD_VEPU0 is still held "
           "in its initial reset\n");
      return -ENODEV;
    }

  if (ver == 0xffffffffu)
    {
      _err("ERROR: VEPU0 version register reads all-ones -- the register bus "
           "is not responding\n");
      return -ENODEV;
    }

  if ((ver & RK3576_VEPU510_VER_H264_CAP) == 0)
    {
      _err("ERROR: VEPU0 reports no H.264 encode capability (version "
           "0x%08" PRIx32 ")\n",
           ver);
      return -ENODEV;
    }

  _info("VEPU0: VEPU510 rev %" PRIu32 ", ip_id 0x%02" PRIx32
        ", h264=%d hevc=%d bframe=%d fbc=%d, res=%" PRIu32 ", osd=%" PRIu32
        ", filter=%" PRIu32 "\n",
        (ver & RK3576_VEPU510_VER_SUB_VER_MASK) >>
            RK3576_VEPU510_VER_SUB_VER_SHFT,
        (ver & RK3576_VEPU510_VER_IP_ID_MASK) >> RK3576_VEPU510_VER_IP_ID_SHFT,
        (ver & RK3576_VEPU510_VER_H264_CAP) ? 1 : 0,
        (ver & RK3576_VEPU510_VER_HEVC_CAP) ? 1 : 0,
        (ver & RK3576_VEPU510_VER_BFRM_CAP) ? 1 : 0,
        (ver & RK3576_VEPU510_VER_FBC_CAP_MASK) >>
            RK3576_VEPU510_VER_FBC_CAP_SHFT,
        (ver & RK3576_VEPU510_VER_RES_CAP_MASK) >>
            RK3576_VEPU510_VER_RES_CAP_SHFT,
        (ver & RK3576_VEPU510_VER_OSD_CAP_MASK) >>
            RK3576_VEPU510_VER_OSD_CAP_SHFT,
        (ver & RK3576_VEPU510_VER_FILTR_CAP_MASK) >>
            RK3576_VEPU510_VER_FILTR_CAP_SHFT);

  return OK;
}

int rk3576_vepu_initialize(void)
{
  int ret;

  ret = nxmutex_lock(&g_vepu_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_vepu_irq_attached)
    {
      nxmutex_unlock(&g_vepu_lock);
      return OK;
    }

  ret = rk3576_vepu_power_on();
  if (ret < 0)
    {
      nxmutex_unlock(&g_vepu_lock);
      return ret;
    }

  rk3576_vepu_mmu_passthrough();

  nxsem_init(&g_vepu_done, 0, 0);
  g_vepu_sem_ready = true;

  ret = irq_attach(RK3576_IRQ_VEPU0, rk3576_vepu_isr, NULL);
  if (ret < 0)
    {
      _err("ERROR: VEPU0 failed to attach IRQ %d: %d\n", RK3576_IRQ_VEPU0,
           ret);
      nxsem_destroy(&g_vepu_done);
      g_vepu_sem_ready = false;
      nxmutex_unlock(&g_vepu_lock);
      return ret;
    }

  g_vepu_irq_attached = true;
  up_enable_irq(RK3576_IRQ_VEPU0);

  /* Start with the block masked.  The encoder is idle, so nothing should
   * signal, but a status bit left set from the version probe would otherwise
   * be delivered as if a job had completed.
   */

  rk3576_vepu_putreg(RK3576_VEPU510_INT_MSK_OFFSET, 0xffffffffu);
  rk3576_vepu_putreg(RK3576_VEPU510_INT_CLR_OFFSET, 0xffffffffu);

  nxmutex_unlock(&g_vepu_lock);
  return OK;
}

int rk3576_vepu_uninitialize(void)
{
  int ret;

  ret = nxmutex_lock(&g_vepu_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_vepu_irq_attached)
    {
      nxmutex_unlock(&g_vepu_lock);
      return OK;
    }

  up_disable_irq(RK3576_IRQ_VEPU0);
  irq_detach(RK3576_IRQ_VEPU0);
  g_vepu_irq_attached = false;

  nxsem_destroy(&g_vepu_done);
  g_vepu_sem_ready = false;

  rk3576_vepu_recn_free();

  ret = rk3576_vepu_power_off();

  nxmutex_unlock(&g_vepu_lock);
  return ret;
}

int rk3576_vepu_encode(FAR const struct rk3576_vepu510_frame_s *frm,
                       FAR const struct rk3576_h264_cfg_s *cfg,
                       FAR const struct rk3576_vepu510_idr_s *idr,
                       FAR struct rk3576_vepu_result_s *result)
{
  uintptr_t base = RK3576_VEPU0_ADDR;
  struct rk3576_vepu510_frame_s job;
  uint32_t start;
  uint32_t intsta;
  uint32_t status_length;
  uint32_t bs_length = 0;
  bool complete;
  int ret;

  if (frm == NULL || cfg == NULL || idr == NULL)
    {
      return -EINVAL;
    }

  if (frm->src_phys == 0 || frm->src_size == 0)
    {
      return -EINVAL;
    }

  /* dst_size is programmed as a limit of dst_size - 1, so a zero here would
   * be written as "no limit at all" -- the one value that lets the encoder
   * run off the end of the buffer instead of stopping at it.
   */

  if (frm->dst_phys == 0 || frm->dst_size == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_vepu_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_vepu_powered || !g_vepu_irq_attached || !g_vepu_sem_ready)
    {
      ret = -ENODEV;
      goto errout_unlock;
    }

  /* The caller describes the picture and its own two buffers; the working
   * set the encoder also needs is this driver's to provide, so it is added
   * to a local copy rather than required from every caller.
   */

  job = *frm;

  ret = rk3576_vepu_recn_alloc(&job);
  if (ret < 0)
    {
      goto errout_unlock;
    }

  /* Everything the encoder is told comes from here: the picture geometry and
   * both buffers, the coding tools, the syntax parameters and the slice
   * values.  This is the whole of the register knowledge; nothing below
   * needs to know what a block is called.
   */

  ret = rk3576_vepu510_regs_build(&g_vepu_regs, &job, cfg, idr);
  if (ret < 0)
    {
      goto errout_unlock;
    }

  /* The CPU produced the source picture, so its dirty cache lines have to
   * reach memory before the DMA reads them.
   *
   * The destination is the other way round.  Only the encoder writes it, so
   * whatever the cache still holds for that range must be dropped rather
   * than written back -- both before the job, so a stale line cannot be
   * evicted over the bitstream, and after it, so the CPU reads what the
   * encoder wrote instead of what happened to be cached.
   */

  up_clean_dcache((uintptr_t)frm->src_phys,
                  (uintptr_t)frm->src_phys + frm->src_size);
  up_invalidate_dcache((uintptr_t)frm->dst_phys,
                       (uintptr_t)frm->dst_phys + frm->dst_size);

  /* Program the picture.  The control block is written in two pieces with
   * enc_strt left out, because writing that register is what starts the
   * encoder and it therefore has to come after everything else.
   *
   * MPP writes its control block first, start command included, and gets
   * away with it because its register writes are staged and only reach the
   * hardware when the task is committed.  This driver writes MMIO directly
   * and has to order the writes itself: starting here would run the encoder
   * against frame parameters that had not been written yet.
   */

#if RK3576_VEPU510_CTL_OFFSET != 0
#error "the split control-block write assumes the block starts at zero"
#endif

  rk3576_vepu_write_block(base, RK3576_VEPU510_CTL_OFFSET,
                          &g_vepu_regs.reg_ctl,
                          RK3576_VEPU510_ENC_STRT_OFFSET);

  rk3576_vepu_write_block(
      base,
      RK3576_VEPU510_CTL_OFFSET + RK3576_VEPU510_ENC_STRT_OFFSET +
          sizeof(uint32_t),
      (FAR const uint8_t *)&g_vepu_regs.reg_ctl +
          RK3576_VEPU510_ENC_STRT_OFFSET + sizeof(uint32_t),
      sizeof(g_vepu_regs.reg_ctl) - RK3576_VEPU510_ENC_STRT_OFFSET -
          sizeof(uint32_t));

  rk3576_vepu_write_block(base, RK3576_VEPU510_FRAME_OFFSET,
                          &g_vepu_regs.reg_frm, sizeof(g_vepu_regs.reg_frm));
  rk3576_vepu_write_block(base, RK3576_VEPU510_RC_ROI_OFFSET,
                          &g_vepu_regs.reg_rc_roi,
                          sizeof(g_vepu_regs.reg_rc_roi));
  rk3576_vepu_write_block(base, RK3576_VEPU510_PARAM_OFFSET,
                          &g_vepu_regs.reg_param,
                          sizeof(g_vepu_regs.reg_param));
  rk3576_vepu_write_block(base, RK3576_VEPU510_SQI_OFFSET,
                          &g_vepu_regs.reg_sqi, sizeof(g_vepu_regs.reg_sqi));
  rk3576_vepu_write_block(base, RK3576_VEPU510_SCL_OFFSET,
                          &g_vepu_regs.reg_scl, sizeof(g_vepu_regs.reg_scl));

  /* Three more writes that are not part of the register image above, and are
   * not in MPP's image either.
   *
   * The vendor kernel applies them when it commits the register table, which
   * is why a driver replacing both halves has to do them itself.  They are
   * either past the end of what MPP models -- 0x74 is beyond the 96-byte
   * control block MPP generates -- or they modify a register the image
   * already set, so the register layer cannot supply them.
   */

  /* The encoder-identity register: ch_id 1 with response-return enabled,
   * which is what the vendor kernel puts here before a job.  It is cleared
   * again when the job completes -- see rk3576_vepu_collect_status() -- and
   * that round trip is what makes the IP publish its done status.
   */

  rk3576_vepu_putreg(RK3576_VEPU510_DVBM_HOLD_OFFSET,
                     RK3576_VEPU510_DVBM_HOLD_VALUE);
  rk3576_vepu_putreg(RK3576_VEPU510_DVBM_STATE_OFFSET,
                     RK3576_VEPU510_DVBM_STATE_VALUE);

  /* Enable the slice-done interrupt, which the register image leaves clear
   * and the kernel sets for VEPU510.
   *
   * This is not cosmetic and must not be tidied away.  VEPU510 is documented
   * -- by the kernel, in the comment quoted at RK3576_VEPU510_INT_DONE -- as
   * being unreliable about enc-done: it can re-encode a frame and never raise
   * it.  Slice-done is the signal that does arrive, so a job without this
   * enabled has no completion to wait for. */

  rk3576_vepu_putreg(RK3576_VEPU510_INT_EN_OFFSET,
                     rk3576_vepu_getreg(RK3576_VEPU510_INT_EN_OFFSET) |
                         RK3576_VEPU510_INT_SLICE_DONE);

  /* Bit 30 of enc_pic, taken from the value the register image produced.
   *
   * The kernel also sets bit 31, but only when it has disabled FBC for the
   * reconstruction frames, and only with the core clock stopped: writing it
   * while the clock runs makes the DMA module issue a write of its own.  FBC
   * stays enabled here as it does in MPP's main path, so this takes the
   * branch that sets bit 30 alone and never has to stop the clock.
   */

  rk3576_vepu_putreg(RK3576_VEPU510_ENC_PIC_OFFSET,
                     rk3576_vepu_getreg(RK3576_VEPU510_ENC_PIC_OFFSET) |
                         RK3576_VEPU510_ENC_PIC_BIT30);

  /* Close the window between the last register write and the start.
   *
   * Mask, clear the status, then drop any completion the semaphore is still
   * holding.  A job that timed out leaves its own post behind, and without
   * this the next job would take it for an immediate completion and read a
   * bitstream that does not exist yet.
   */

  rk3576_vepu_putreg(RK3576_VEPU510_INT_MSK_OFFSET, 0xffffffffu);
  rk3576_vepu_putreg(RK3576_VEPU510_INT_CLR_OFFSET, 0xffffffffu);

  while (nxsem_trywait(&g_vepu_done) == OK)
    {
    }

  g_vepu_intsta = 0;

  rk3576_vepu_putreg(RK3576_VEPU510_INT_MSK_OFFSET, 0);

  /* Start.  The command comes out of the register image rather than being
   * assembled here, so it cannot drift away from what the register layer
   * decided lkt_num and vepu_cmd should be. */

  memcpy(&start, &g_vepu_regs.reg_ctl.enc_strt, sizeof(start));
  rk3576_vepu_putreg(RK3576_VEPU510_ENC_STRT_OFFSET, start);

  ret = nxsem_tickwait(&g_vepu_done, MSEC2TICK(RK3576_VEPU_JOB_TIMEOUT_MS));
  if (ret < 0)
    {
      /* Nothing is going to clear this job.  Mask the interrupt so a late
       * completion cannot arrive after the wait has been abandoned. */

      rk3576_vepu_putreg(RK3576_VEPU510_INT_MSK_OFFSET, 0xffffffffu);
      _err("ERROR: VEPU0 encode timed out after %u ms\n",
           (unsigned int)RK3576_VEPU_JOB_TIMEOUT_MS);
      rk3576_vepu_dump();
      ret = -ETIMEDOUT;
      goto errout_unlock;
    }

  intsta = rk3576_vepu_collect_status(g_vepu_intsta);

  up_invalidate_dcache((uintptr_t)frm->dst_phys,
                       (uintptr_t)frm->dst_phys + frm->dst_size);

  /* The bitstream length comes from the slice FIFO, not from the status
   * block.  The status block has a length word of its own, but it is not
   * always populated -- a job that encoded its picture and reported success
   * has been seen leaving it at zero -- whereas the FIFO records are what
   * the encoder produces unconditionally, one per slice.
   */

  complete = rk3576_vepu_drain_slice_fifo(&bs_length);

  /* The status block keeps a length word of its own, and over an appended
   * buffer it is not obvious what that word holds: this frame's length, or
   * everything written into the buffer so far.  The reference does not settle
   * it, because MPP always hands the encoder an offset of zero, so its
   * behaviour says nothing about the case where frames are appended.
   *
   * Rather than pick one and design around it, both are reported when they
   * disagree.  One log line answers the question the first time a second
   * frame is encoded, which is cheaper than being wrong quietly.
   */

  status_length = rk3576_vepu_getreg(RK3576_VEPU510_ST_BS_LGTH_OFFSET);

  if (status_length != 0 && status_length != bs_length)
    {
      _warn("WARNING: VEPU0 status length %" PRIu32
            " but the slice records total %" PRIu32
            " (frame starts at offset %" PRIu32 ")\n",
            status_length, bs_length, frm->dst_offset);
    }

  if (result != NULL)
    {
      result->bs_length = bs_length;

      /* The distortion figure is reassembled from two registers rather than
       * read, and the register layer owns that mapping -- see
       * rk3576_vepu510_status_decode(), which is host-checked.
       */

      rk3576_vepu510_status_decode(
          status_length, rk3576_vepu_getreg(RK3576_VEPU510_ST_SSE_LOW_OFFSET),
          rk3576_vepu_getreg(RK3576_VEPU510_ST_SSE_HIGH_OFFSET), NULL,
          &result->sse);

      result->intsta = intsta;
    }

  if (intsta == 0)
    {
      /* Woken without a status: whatever posted was not this job's
       * completion, so there is no reason to believe the bitstream. */

      _err("ERROR: VEPU0 job woke with no interrupt status\n");
      ret = -EIO;
      goto errout_unlock;
    }

  if ((intsta & RK3576_VEPU510_INT_ERRORS) != 0)
    {
      _err("ERROR: VEPU0 encode failed, int_sta 0x%08" PRIx32 "\n", intsta);
      rk3576_vepu_dump();
      ret = -EIO;
      goto errout_unlock;
    }

  /* Completion is the slice FIFO's last record, not the interrupt's done
   * bits, and the two are not interchangeable.
   *
   * The interrupt is raised by the FIFO being non-empty, so it stands for "a
   * slice went in", not "the picture is finished" -- on this IP a frame can
   * arrive in pieces, and it can also be re-encoded, which is the hardware
   * bug the vendor kernel documents.  The last record is the encoder saying
   * the picture is complete, which is the thing worth waiting for.
   *
   * Accepting an interrupt on its own returns however much of the bitstream
   * had been written at that moment, which reads as a valid length and a
   * valid-looking stream.
   */

  if (!complete)
    {
      _err(
          "ERROR: VEPU0 job ended without its last slice, int_sta 0x%08" PRIx32
          ", %" PRIu32 " bytes so far\n",
          intsta, bs_length);
      rk3576_vepu_dump();
      ret = -EIO;
      goto errout_unlock;
    }

  if ((intsta & RK3576_VEPU510_INT_DONE) == 0)
    {
      _err("ERROR: VEPU0 job ended without a completion bit, int_sta "
           "0x%08" PRIx32 "\n",
           intsta);
      rk3576_vepu_dump();
      ret = -EIO;
      goto errout_unlock;
    }

  if (bs_length == 0)
    {
      /* A clean status with an empty bitstream is still a failure: the
       * encoder was handed a picture and wrote nothing for it. */

      _err("ERROR: VEPU0 encode produced no bitstream (int_sta 0x%08" PRIx32
           ")\n",
           intsta);
      rk3576_vepu_dump();
      ret = -EIO;
      goto errout_unlock;
    }

  ret = OK;

errout_unlock:
  nxmutex_unlock(&g_vepu_lock);
  return ret;
}

#ifdef CONFIG_RK3576_VEPU_SELFTEST

/* The three pictures, chosen so that what the encoder does with them is
 * predictable from first principles.  They are listed in increasing order of
 * how much there is to encode, and that order is the expectation the test
 * checks, so it is stated once here rather than restated at the checks.
 *
 *   solid     nothing to predict badly and nothing to spend bits on: the
 *             floor, and a picture the encoder should describe in a few
 *             hundred bytes whatever else it does
 *   gradient  smooth in both directions, so entirely predictable from its
 *             neighbours: cheap, but not free
 *   noise     no spatial correlation at all: incompressible, so the ceiling
 *
 * A test that only ever encoded one picture could not tell an encoder from a
 * very short wire, since a fixed-size or empty output looks the same as a
 * correct one.  These three make the size itself a measurement.
 */

enum rk3576_vepu_pattern_e
{
  RK3576_VEPU_PATTERN_SOLID,
  RK3576_VEPU_PATTERN_GRADIENT,
  RK3576_VEPU_PATTERN_NOISE,
  RK3576_VEPU_PATTERN_COUNT
};

static const char *const g_vepu_pattern_name[RK3576_VEPU_PATTERN_COUNT] = {
  "solid   ", "gradient", "noise   "
};

/****************************************************************************
 * Name: rk3576_vepu_fill_pattern
 *
 * Description:
 *   Write one luma plane of a synthetic picture.  Chroma is left neutral by
 *   the caller, so what the test compares is luma complexity and the claim it
 *   makes is about luma only.
 *
 ****************************************************************************/

static void rk3576_vepu_fill_pattern(FAR uint8_t *y, uint32_t width,
                                     uint32_t height, uint32_t pattern,
                                     uint32_t seed)
{
  uint32_t lcg = seed;
  uint32_t row;
  uint32_t col;

  for (row = 0; row < height; row++)
    {
      for (col = 0; col < width; col++)
        {
          switch (pattern)
            {
              case RK3576_VEPU_PATTERN_GRADIENT:
                y[row * width + col] =
                    (uint8_t)((row * 255u) / height + (col * 255u) / width);
                break;

              case RK3576_VEPU_PATTERN_NOISE:
              default:
                /* An LCG rather than anything from the C library, so that the
                 * picture is the same on every build and a difference in the
                 * numbers is a difference in the encoder.
                 *
                 * The high bits are taken, not the low ones: this generator's
                 * low bits have short periods, which would produce a picture
                 * with visible structure and make it more compressible than
                 * the test assumes.
                 */

                lcg = lcg * 1664525u + 1013904223u;
                y[row * width + col] = (uint8_t)((lcg >> 16) & 0xffu);
                break;

              case RK3576_VEPU_PATTERN_SOLID:
                y[row * width + col] = 128;
                break;
            }
        }
    }
}

/* Geometry and layout of the test.  Small on purpose: this asks whether the
 * encoder is doing the right thing, not how fast it does it.
 *
 * The destination carries a guard region past the end.  It is not a buffer
 * the driver is told about -- frm.dst_size covers the real part only -- so
 * anything the encoder writes there is an overrun.  Without it, an encoder
 * that ignored its own size limit would corrupt whatever the DMA heap handed
 * out next, and nothing would say so until much later.
 */

#define RK3576_VEPU_ST_W          320u
#define RK3576_VEPU_ST_H          240u
#define RK3576_VEPU_ST_FRAMES     6u
#define RK3576_VEPU_ST_DST        (1024u * 1024u)
#define RK3576_VEPU_ST_GUARD      (64u * 1024u)

#define RK3576_VEPU_ST_GUARD_BYTE 0xa5u

int rk3576_vepu_selftest(void)
{
  struct rk3576_vepu510_frame_s frm;
  struct rk3576_h264_cfg_s cfg;
  struct rk3576_vepu510_idr_s idr;
  struct rk3576_vepu_result_s result;
  FAR uint8_t *src;
  FAR uint8_t *dst;
  FAR uint8_t *guard;
  uint32_t offset[RK3576_VEPU_ST_FRAMES];
  uint32_t length[RK3576_VEPU_ST_FRAMES];
  uint32_t sse[RK3576_VEPU_ST_FRAMES];
  uint32_t y_size = RK3576_VEPU_ST_W * RK3576_VEPU_ST_H;
  uint32_t src_size = y_size * 3u / 2u;
  uint32_t total = 0;
  uint32_t i;
  int bad = 0;
  int ret;

  /* Every buffer the encoder touches comes from the DMA heap: physically
   * contiguous, below 4 GB and 64-byte aligned, which is what the
   * pass-through MMU route requires.  Source and destination are separate
   * allocations because the source is read and the destination written, and
   * a single buffer would make it impossible to tell a misdirected write from
   * a misdirected read.
   */

  src = rk3576_dma_alloc(src_size);
  dst = rk3576_dma_alloc(RK3576_VEPU_ST_DST + RK3576_VEPU_ST_GUARD);

  if (src == NULL || dst == NULL)
    {
      _err("ERROR: VEPU0 self-test out of DMA heap (%" PRIu32 " + %" PRIu32
           " bytes)\n",
           src_size, RK3576_VEPU_ST_DST + RK3576_VEPU_ST_GUARD);
      ret = -ENOMEM;
      goto errout;
    }

  guard = dst + RK3576_VEPU_ST_DST;
  memset(dst, 0, RK3576_VEPU_ST_DST);

  /* The guard has to be pushed out to memory before the test and read back
   * past the cache afterwards, because the driver never flushes it: its
   * range is frm.dst_size, which stops where the guard begins.
   *
   * Both halves matter.  Without the clean, the CPU's copy would never reach
   * memory and the check would pass no matter what the hardware did.  Without
   * the invalidate, the check would read that same stale copy back and pass
   * for the same reason.
   */

  memset(guard, RK3576_VEPU_ST_GUARD_BYTE, RK3576_VEPU_ST_GUARD);
  up_clean_dcache((uintptr_t)guard, (uintptr_t)guard + RK3576_VEPU_ST_GUARD);

  memset(&cfg, 0, sizeof(cfg));
  cfg.width = RK3576_VEPU_ST_W;
  cfg.height = RK3576_VEPU_ST_H;
  cfg.profile_idc = RK3576_H264_PROFILE_BASELINE;
  cfg.level_idc = RK3576_H264_LEVEL_AUTO;
  cfg.log2_max_frame_num_minus4 = 12;
  cfg.poc_type = 0;
  cfg.log2_max_poc_lsb_minus4 = 12;
  cfg.num_ref_frames = 1;
  cfg.gaps_allowed = 0;
  cfg.direct8x8_inference = 1;
  cfg.entropy_coding_mode = 0;
  cfg.transform8x8_mode = 0;
  cfg.constrained_intra_pred = 0;
  cfg.deblocking_filter_control = 1;
  cfg.pic_init_qp = 26;
  cfg.frame_qp = 26;
  cfg.chroma_cb_qp_offset = 0;
  cfg.chroma_cr_qp_offset = 0;
  cfg.vui_en = 1;
  cfg.full_range = 0;
  cfg.fps_num = 30;
  cfg.fps_den = 1;

  /* MPP's own tuning defaults, so the test exercises the same rate
   * distortion setup as the reference. */

  cfg.tune.scene_mode = 0; /* MPP_ENC_SCENE_MODE_DEFAULT, i.e. not IPC */
  cfg.tune.atl_str = 1;
  cfg.tune.atr_str_i = 1;
  cfg.tune.atf_str = 1;
  cfg.tune.lambda_idx_i = 6;

  memset(&frm, 0, sizeof(frm));
  frm.src_fmt = RK3576_VEPU510_FMT_YUV420SP;
  frm.rbuv_swap = 0; /* NV12, not NV21 */
  frm.width = RK3576_VEPU_ST_W;
  frm.height = RK3576_VEPU_ST_H;
  frm.y_stride = RK3576_VEPU_ST_W;
  frm.v_stride = RK3576_VEPU_ST_H;
  frm.src_phys = (uint32_t)(uintptr_t)src;
  frm.src_size = src_size;
  frm.dst_phys = (uint32_t)(uintptr_t)dst;
  frm.dst_size = RK3576_VEPU_ST_DST;

  /* Neutral chroma for every picture, so the comparison is about luma. */

  memset(src + y_size, 128, src_size - y_size);

  for (i = 0; i < RK3576_VEPU_ST_FRAMES; i++)
    {
      uint32_t pattern = i % RK3576_VEPU_PATTERN_COUNT;

      rk3576_vepu_fill_pattern(src, RK3576_VEPU_ST_W, RK3576_VEPU_ST_H,
                               pattern, i + 1u);

      /* Frames are appended to one buffer, each starting where the previous
       * one ended.  This is the mechanism the V4L2 layer will need in order
       * to hand several frames to a single output buffer, so it is exercised
       * here on its own rather than only incidentally.
       */

      offset[i] = total;
      frm.dst_offset = total;

      /* Consecutive IDR pictures are required to carry different
       * idr_pic_id values (H.264 7.4.3), and this is the only place in the
       * driver where more than one IDR is encoded in a row -- so it is the
       * only place the requirement can be met or missed. */

      memset(&idr, 0, sizeof(idr));
      idr.idr_pic_id = i & 1u;

      ret = rk3576_vepu_encode(&frm, &cfg, &idr, &result);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 self-test frame %" PRIu32 " (%s) failed: %d\n", i,
               g_vepu_pattern_name[pattern], ret);
          goto errout;
        }

      length[i] = result.bs_length;
      sse[i] = result.sse;
      total += result.bs_length;

      if (total > RK3576_VEPU_ST_DST)
        {
          _err("ERROR: VEPU0 self-test overflowed its description of the"
               " buffer at frame %" PRIu32 "\n",
               i);
          ret = -EIO;
          goto errout;
        }

      _info("VEPU0 self-test: frame %" PRIu32 " %s offset %6" PRIu32
            " length %6" PRIu32 " sse %10" PRIu32 "\n",
            i, g_vepu_pattern_name[pattern], offset[i], length[i], sse[i]);

      /* Where each frame begins is known, so the start code is looked for
       * there rather than searched for.  A search would find a start code
       * wherever it happened to be; this finds it only where it was asked
       * for, which is what makes the offset a checked value.
       */

      if (!(dst[offset[i]] == 0 && dst[offset[i] + 1] == 0 &&
            dst[offset[i] + 2] == 0 && dst[offset[i] + 3] == 1))
        {
          _err("ERROR: VEPU0 self-test frame %" PRIu32 " has no start code at"
               " its offset\n",
               i);
          bad++;
        }

      if ((dst[offset[i] + 4] & 0x1fu) != 5)
        {
          _err("ERROR: VEPU0 self-test frame %" PRIu32 " NAL type %" PRIu32
               ", expected an IDR\n",
               i, (uint32_t)(dst[offset[i] + 4] & 0x1fu));
          bad++;
        }

      /* Both signals are checked, and they are independent: the length is
       * what the rate controller spent, the distortion is how close the
       * reconstruction came.  An encoder that produced the right sizes by
       * emitting filler would pass the first and fail the second.
       */
    }

  /* With one frame of each pattern per round, the expectation is simply that
   * each round is ordered.  Checked per round rather than across all six, so
   * that drift between rounds shows up instead of averaging out.
   *
   * The two orderings are held to different standards, which is deliberate.
   *
   * Length increases strictly: a flat picture codes to hundreds of bytes and
   * a gradient to thousands, so the gaps are wide and there is no legitimate
   * reason for them to be equal.
   *
   * Distortion is only required to be non-decreasing from solid to gradient,
   * and strictly increasing from gradient to noise.  A flat picture can be
   * reproduced exactly, so requiring it to be strictly worse than the
   * gradient would fail on a perfectly good encoder.  The gradient-to-noise
   * step is where the real check is: noise has nothing predictable in it, so
   * its distortion has to be far larger -- and a distortion figure that was
   * truncated to its low half would wrap to something small and fail here,
   * which is the mistake this pair of numbers was added to catch.
   */

  for (i = 0; i + 2u < RK3576_VEPU_ST_FRAMES; i += RK3576_VEPU_PATTERN_COUNT)
    {
      if (!(length[i] < length[i + 1] && length[i + 1] < length[i + 2]))
        {
          _err("ERROR: VEPU0 self-test lengths not ordered: solid %" PRIu32
               ", gradient %" PRIu32 ", noise %" PRIu32 "\n",
               length[i], length[i + 1], length[i + 2]);
          bad++;
        }

      if (!(sse[i] <= sse[i + 1] && sse[i + 1] < sse[i + 2]))
        {
          _err("ERROR: VEPU0 self-test distortion not ordered: solid %" PRIu32
               ", gradient %" PRIu32 ", noise %" PRIu32 "\n",
               sse[i], sse[i + 1], sse[i + 2]);
          bad++;
        }
    }

  /* The guard, read back from memory rather than from the cache. */

  up_invalidate_dcache((uintptr_t)guard,
                       (uintptr_t)guard + RK3576_VEPU_ST_GUARD);

  for (i = 0; i < RK3576_VEPU_ST_GUARD; i++)
    {
      if (guard[i] != RK3576_VEPU_ST_GUARD_BYTE)
        {
          _err("ERROR: VEPU0 self-test wrote %" PRIu32
               " bytes past the end of the destination buffer\n",
               RK3576_VEPU_ST_GUARD - i);
          bad++;
          break;
        }
    }

  _info("VEPU0 self-test: %" PRIu32 " frames, %" PRIu32
        " bytes of bitstream, solid %" PRIu32 "/%" PRIu32 ", gradient %" PRIu32
        "/%" PRIu32 ", noise %" PRIu32 "/%" PRIu32 " (length/sse)\n",
        RK3576_VEPU_ST_FRAMES, total, length[0], sse[0], length[1], sse[1],
        length[2], sse[2]);

  /* Report the parameter sets alongside.  They come from the syntax layer
   * rather than from the hardware, so they are not evidence about the IP --
   * but they are the other half of a playable stream, and having both in the
   * log means the whole thing can be reassembled from it.
   */

  {
    uint8_t sps[RK3576_H264_SPS_MAX];
    uint8_t pps[RK3576_H264_PPS_MAX];
    uint32_t sps_len = 0;
    uint32_t pps_len = 0;

    if (rk3576_h264_sps_write(sps, sizeof(sps), &cfg, &sps_len) == 0 &&
        rk3576_h264_pps_write(pps, sizeof(pps), &cfg, &pps_len) == 0)
      {
        _info("VEPU0 self-test: SPS %" PRIu32 " bytes, PPS %" PRIu32
              " bytes, so the six frames are %" PRIu32 " bytes total\n",
              sps_len, pps_len, sps_len + pps_len + total);
      }
  }

  ret = bad == 0 ? OK : -EIO;

errout:
  if (src != NULL)
    {
      rk3576_dma_free(src, src_size);
    }

  if (dst != NULL)
    {
      rk3576_dma_free(dst, RK3576_VEPU_ST_DST + RK3576_VEPU_ST_GUARD);
    }

  return ret;
}

#endif /* CONFIG_RK3576_VEPU_SELFTEST */

void rk3576_vepu_dump(void)
{
  _info("VEPU0: int_en 0x%08" PRIx32 ", int_msk 0x%08" PRIx32
        ", int_sta 0x%08" PRIx32 "\n",
        rk3576_vepu_getreg(RK3576_VEPU510_INT_EN_OFFSET),
        rk3576_vepu_getreg(RK3576_VEPU510_INT_MSK_OFFSET),
        rk3576_vepu_getreg(RK3576_VEPU510_INT_STA_OFFSET));

  /* enc_strt is the register whose write starts a job, so how it reads back
   * is the difference between "the command was never issued" and "it was
   * issued and the hardware did not act on it".
   */

  _info("VEPU0: enc_strt 0x%08" PRIx32 ", enc_clr 0x%08" PRIx32
        ", enc_pic 0x%08" PRIx32 ", wdg 0x%08" PRIx32 "\n",
        rk3576_vepu_getreg(RK3576_VEPU510_ENC_STRT_OFFSET),
        rk3576_vepu_getreg(RK3576_VEPU510_ENC_CLR_OFFSET),
        rk3576_vepu_getreg(RK3576_VEPU510_ENC_PIC_OFFSET),
        rk3576_vepu_getreg(RK3576_VEPU510_ENC_WDG_OFFSET));

  _info("VEPU0: version 0x%08" PRIx32 ", mmu status 0x%08" PRIx32 "\n",
        rk3576_vepu_getreg(RK3576_VEPU510_VERSION_OFFSET),
        getreg32(RK3576_VEPU0_MMU_ADDR + RK3576_VEPU_MMU_STATUS));
}

#endif /* CONFIG_RK3576_VEPU */
