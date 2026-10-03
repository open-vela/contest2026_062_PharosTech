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
#include <sys/param.h>
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
 * Pre-processor Definitions
 ****************************************************************************/

/* The driver prints two lines at boot, and everything else it has to say goes
 * through vinfo().
 *
 * Those two are the ones a reader needs: which IP answered the probe, and
 * whether the encode path works.  The rest -- which clock source each branch
 * settled on, what the dividers hold, the size of the reconstruction sets, a
 * frame-by-frame account of the self-test -- is a diagnosis of something
 * already known to work, and printing it every boot buries the two lines that
 * are not.
 *
 * vinfo() is NuttX's own video informational channel, enabled with
 * CONFIG_DEBUG_VIDEO_INFO.  That is the right switch rather than one of this
 * driver's own: a video driver's diagnostics belong to the video subsystem's
 * debug control, so turning video debugging on shows this driver's detail
 * along with every other video driver's.
 *
 * Note which switches that takes.  CONFIG_DEBUG_VIDEO_INFO is one of three
 * children of CONFIG_DEBUG_VIDEO, which defaults to off, so a configuration
 * that names only the child has that line dropped by Kconfig without a
 * warning and builds a driver that appears to have nothing to say -- which is
 * how this driver's clock selection, its divider readback and the size of its
 * reconstruction sets were all invisible in the configuration the driver
 * ships with.  The camera configuration turns on all of them, so the detail
 * is there when a boot log is what is being read.
 *
 * Errors are unaffected.  They are reported whatever the debug options say,
 * and rk3576_vepu_dump() prints the registers when an encode fails, which is
 * where the clock and status detail is actually wanted.
 */

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
 * Name: rk3576_vepu_select_source
 *
 * Description:
 *   Point a selector at the source that runs its clock closest to the
 *   target without going over, and program the divider to match.
 *
 *   Both selectors are pointed at a source from a candidate list, and the
 *   one that runs the clock closest to the target without going over is
 *   chosen, with its divider programmed to match.
 *
 *   SPLL is the first candidate for the core because it is the exact figure
 *   the core is assigned: 702 MHz is SPLL/1, and nothing else reaches it,
 *   because the divider is an integer -- GPLL is 1188 MHz, whose integer
 *   divisions are 1188, 594, 396, ..., none of which is 702.  That is why
 *   the core ran at 594 before: the next-best source was GPLL divided by
 *   two.
 *
 *   BPLL is left out (not modelled).  LPLL is left out despite being
 *   modelled: it is the CPU's PLL, and pointing a peripheral at it is the
 *   hazard spelled out at length in rk3576_clk_register_vop().  The rates
 *   the other candidates offer belong to the bootloader and are read back
 *   from the PLL registers rather than assumed; SPLL's is a constant this
 *   tree states, for the reason given at RK3576_SPLL_HZ.
 *
 *   A tie goes to the earlier candidate.
 *
 *   The candidates are the caller's, because the two selectors this serves
 *   do not have the same parents.  The core selector offers SPLL, GPLL and
 *   CPLL; the AXI root selector offers only GPLL and CPLL.  Offering SPLL to
 *   the AXI root is rejected by the framework -- SPLL is not one of its
 *   parents -- and reporting that rejection as a failure to select a source
 *   is a false alarm, which is what a list shared between the two callers
 *   used to produce.
 *
 * Input Parameters:
 *   mux      - the selector to point at the chosen source
 *   clk      - the clock whose rate is wanted, a descendant of the mux
 *   target   - the rate wanted
 *   sources  - the candidate source names, in preference order
 *   nsources - how many candidates there are
 *
 * Returned Value:
 *   the rate the clock ends up at, or 0 if no source could be used
 *
 ****************************************************************************/

static uint32_t rk3576_vepu_select_source(FAR struct clk_s *mux,
                                          FAR struct clk_s *clk,
                                          uint32_t target,
                                          FAR const char *const *sources,
                                          size_t nsources)
{
  FAR struct clk_s *best = NULL;
  uint32_t best_rate = 0;
  size_t i;

  if (mux == NULL || clk == NULL)
    {
      return 0;
    }

  for (i = 0; i < nsources; i++)
    {
      FAR struct clk_s *src = clk_get(sources[i]);
      uint32_t rate;

      if (src == NULL)
        {
          continue;
        }

      if (clk_set_parent(mux, src) < 0)
        {
          _err("ERROR: VEPU0 could not select %s\n", sources[i]);
          continue;
        }

      /* Check that the selector actually moved.  A rate measured after a
       * selection that did not happen is the rate of the previous source,
       * which makes the comparison below look like a tie and turns the
       * choice into a coin toss on the order of this array.  Comparing the
       * tree's view catches that here rather than in the resulting numbers.
       */

      if (clk_get_parent(mux) != src)
        {
          _err("ERROR: VEPU0 selected %s but %s is still the parent\n",
               sources[i],
               clk_get_parent(mux) == NULL ? "nothing" : "another");
          continue;
        }

      /* What the clock would run at from this source.  Asking rather than
       * working it out keeps the divisor arithmetic in one place.
       */

      rate = clk_round_rate(clk, target);
      vinfo("VEPU0: %s gives %" PRIu32 " Hz for a %" PRIu32 " Hz target\n",
            sources[i], rate, target);

      if (rate > best_rate)
        {
          best_rate = rate;
          best = src;
        }
    }

  if (best == NULL)
    {
      return 0;
    }

  if (clk_set_parent(mux, best) < 0)
    {
      return 0;
    }

  if (clk_set_rate(clk, target) < 0)
    {
      return 0;
    }

  return clk_get_rate(clk);
}

/****************************************************************************
 * Name: rk3576_vepu_prepare_clocks
 *
 * Description:
 *   Bring up the VEPU0 clock branches: choose a source for each, program the
 *   rates the hardware is specified to run at, and open the gates.
 *
 *   clk_enable() walks the parent chain, so enabling a leaf also opens the
 *   root gates it hangs from; the bus-interface gates are enabled explicitly
 *   because they are not in any parent chain that the leaves traverse.
 *
 *   The gates all reset to 0, i.e. enabled, so out of reset that part is
 *   bookkeeping rather than a fix.  It is still done so the clock tree's
 *   accounting matches the hardware and nothing else can gate VEPU0 off
 *   behind the driver's back.  The rates are another matter: they reset to a
 *   source the tree cannot resolve, so they are programmed here rather than
 *   left as found.
 *
 *   The AXI rate is set on aclk_vepu0_root rather than on aclk_vepu0 below
 *   it, because the consumer gate carries no CLK_SET_RATE_PARENT and would
 *   swallow the request instead of passing it up to the divider.
 *
 ****************************************************************************/

static int rk3576_vepu_prepare_clocks(void)
{
  uint32_t core_rate;
  uint32_t aclk_rate;

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

  /* Rates first, so that nothing is ever reported as running at a frequency
   * it has not been set to.
   */

  /* The candidate sources differ between the two selectors, so each states
   * its own.
   *
   * SPLL comes first for the core because it is the rate the core is
   * assigned: 702 MHz is SPLL/1, and no division of the other two reaches it
   * -- GPLL is 1188 MHz whose integer divisions are 1188, 594, 396..., and
   * the core used to run at the 594.  The AXI root selector has no SPLL
   * among its parents and so does not offer it.
   */

  static const char *const core_sources[] = { "clk_spll", "clk_gpll",
                                              "clk_cpll" };
  static const char *const aclk_sources[] = { "clk_gpll", "clk_cpll" };

  core_rate = rk3576_vepu_select_source(clk_get("clk_vepu0_core_sel"),
                                        g_vepu_clks.core, RK3576_VEPU_CORE_HZ,
                                        core_sources, nitems(core_sources));
  if (core_rate == 0)
    {
      _err("ERROR: VEPU0 could not give its core clock a source\n");
    }

  aclk_rate = rk3576_vepu_select_source(
      clk_get("aclk_vepu0_root_sel"), clk_get("aclk_vepu0_root"),
      RK3576_VEPU_ACLK_HZ, aclk_sources, nitems(aclk_sources));
  if (aclk_rate == 0)
    {
      _err("ERROR: VEPU0 could not give its AXI clock a source\n");
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

  _info("VEPU0: aclk=%" PRIu32 " hclk=%" PRIu32 " core=%" PRIu32
        " (targets %u and %u)\n",
        clk_get_rate(g_vepu_clks.aclk), clk_get_rate(g_vepu_clks.hclk),
        clk_get_rate(g_vepu_clks.core), (unsigned int)RK3576_VEPU_ACLK_HZ,
        (unsigned int)RK3576_VEPU_CORE_HZ);

  /* What the hardware actually holds, next to what the tree reports above.
   *
   * The selectors and dividers are one register, so it settles in one read
   * which source each branch is really on and what it is divided by -- as
   * opposed to which source the tree believes it chose.  The PLL
   * configuration comes along because a PLL's rate is derived from these
   * three registers at run time and cannot be inferred from anything else
   * here; FOUT = ((m + k/65536) * 24 MHz) / (p * 2^s).
   */

  {
    const uintptr_t cru = RK3576_CRU_ADDR;
    uint32_t sel =
        getreg32(cru + RK3576_CRU_CLKSEL_CON(RK3576_VEPU_CRU_CLKSEL_CON));

    vinfo("VEPU0: sel 0x%08" PRIx32 " core_sel %" PRIu32 " core_div %" PRIu32
          " aclk_sel %" PRIu32 " aclk_div %" PRIu32 " hclk_sel %" PRIu32 "\n",
          sel, (sel >> RK3576_VEPU_CORE_SEL_SHIFT) & RK3576_VEPU_CORE_SEL_MASK,
          (sel >> RK3576_VEPU_CORE_DIV_SHIFT) &
              ((1u << RK3576_VEPU_CORE_DIV_WIDTH) - 1u),
          (sel >> RK3576_VEPU_ACLK_ROOT_SEL_SHIFT) &
              RK3576_VEPU_ACLK_ROOT_SEL_MASK,
          (sel >> RK3576_VEPU_ACLK_ROOT_DIV_SHIFT) &
              ((1u << RK3576_VEPU_ACLK_ROOT_DIV_WIDTH) - 1u),
          (sel >> RK3576_VEPU_HCLK_ROOT_SEL_SHIFT) &
              RK3576_VEPU_HCLK_ROOT_SEL_MASK);

    vinfo("VEPU0: gpll %08" PRIx32 " %08" PRIx32 " %08" PRIx32
          " cpll %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
          getreg32(cru + RK3576_CRU_GPLL_CON(0)),
          getreg32(cru + RK3576_CRU_GPLL_CON(1)),
          getreg32(cru + RK3576_CRU_GPLL_CON(2)),
          getreg32(cru + RK3576_CRU_CPLL_CON(0)),
          getreg32(cru + RK3576_CRU_CPLL_CON(1)),
          getreg32(cru + RK3576_CRU_CPLL_CON(2)));
  }

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

  ret = rk3576_vepu_prepare_clocks();
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

/* The reconstruction working sets.
 *
 * Two of them, and the reason is structural rather than a matter of
 * throughput: an encoder that predicts from a previous picture reconstructs
 * its own picture while reading the one it predicts from, and the read and
 * write sides are different registers.  They therefore cannot be the same
 * buffer.  Two is exactly enough for a stream that keeps one reference and
 * does not reorder -- picture N reads what N-1 wrote and writes the other
 * slot, so the pair alternates.
 *
 * The size is remembered so a change of picture geometry can be noticed and
 * the old buffers released.
 */

struct rk3576_vepu_recn_slot_s
{
  FAR void *pixel;
  FAR void *thumb;
  FAR void *smear;
};

/* How many reconstruction working sets the encoder is given.
 *
 * Two is enough for a stream that keeps one reference and does not reorder:
 * picture N reads what N-1 wrote and writes the other buffer, so the pair
 * alternates.  The count is not a tuning knob -- it was taken to four to match
 * the vendor driver's own bookkeeping (MPP rotates through
 * H264E_MAX_REFS_CNT slots with `if (ctx->curr_idx > 3) ctx->curr_idx = 0`),
 * and every byte of every stream this driver produces came out identical:
 * same lengths, same distortion, same bitstream.  The pattern of costs that
 * prompted the change has a period of four either way.
 *
 * So it is back to two, because the second pair costs a picture's worth of
 * DMA memory per slot and buys nothing.
 */

#define RK3576_VEPU_RECN_SLOTS 2

static struct rk3576_vepu_recn_slot_s g_vepu_recn[RK3576_VEPU_RECN_SLOTS];
static struct rk3576_vepu510_recn_size_s g_vepu_recn_size;
static uint32_t g_vepu_recn_width;
static uint32_t g_vepu_recn_height;

/* Which slot the next job reconstructs into, and whether the other one still
 * holds a picture that can be predicted from.  Held across jobs deliberately:
 * this is the encoder's version of a decoded picture buffer, and its whole
 * content is "which picture is the reference".
 */

static uint32_t g_vepu_recn_next;
static bool g_vepu_recn_ref_valid;

/* What the last completed job reported about itself, which the next job's
 * anti-smear thresholds are programmed from.
 *
 * This is the one piece of the register image that comes from the hardware
 * rather than from configuration, so it cannot be derived on demand: it has
 * to be captured when the job finishes and kept until the next one starts.
 * The picture's own kind is part of it because the mode the encoder writes
 * depends on whether this picture or the one before it stood on its own.
 *
 * It is zeroed along with the reconstruction slots, which is the state MPP's
 * own context is in for its first task: nothing here is a special "no
 * previous picture" case, because MPP does not have one either.
 */

static struct rk3576_vepu510_feedback_s g_vepu_last_fb;

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
  int i;

  for (i = 0; i < RK3576_VEPU_RECN_SLOTS; i++)
    {
      if (g_vepu_recn[i].pixel != NULL)
        {
          rk3576_dma_free(g_vepu_recn[i].pixel, g_vepu_recn_size.pixel);
          g_vepu_recn[i].pixel = NULL;
        }

      if (g_vepu_recn[i].thumb != NULL)
        {
          rk3576_dma_free(g_vepu_recn[i].thumb, g_vepu_recn_size.thumb);
          g_vepu_recn[i].thumb = NULL;
        }

      if (g_vepu_recn[i].smear != NULL)
        {
          rk3576_dma_free(g_vepu_recn[i].smear, g_vepu_recn_size.smear);
          g_vepu_recn[i].smear = NULL;
        }
    }

  g_vepu_recn_width = 0;
  g_vepu_recn_height = 0;
  g_vepu_recn_next = 0;
  g_vepu_recn_ref_valid = false;
  memset(&g_vepu_last_fb, 0, sizeof(g_vepu_last_fb));
}

/****************************************************************************
 * Name: rk3576_vepu_recn_alloc
 *
 * Description:
 *   Make sure the reconstruction working sets match this picture, and
 *   describe both sides of the pair in frm.  The caller must hold the driver
 *   mutex.
 *
 *   idr says whether this picture is decoded without reference to anything.
 *   It matters here because such a picture is not allowed to read a
 *   reconstruction -- and because its arrival is what makes every earlier
 *   picture unavailable as a reference, so the slot that held one stops
 *   being one.
 *
 *   The buffers come from the DMA heap because the encoder writes them
 *   directly: they have to be physically contiguous and below 4 GB, which
 *   the MMU's pass-through route makes the whole requirement.
 *
 ****************************************************************************/

static int rk3576_vepu_recn_alloc(FAR struct rk3576_vepu510_frame_s *frm,
                                  bool idr)
{
  struct rk3576_vepu510_recn_size_s size;
  uint32_t curr;
  int i;

  if (g_vepu_recn[0].pixel != NULL && g_vepu_recn_width == frm->width &&
      g_vepu_recn_height == frm->height)
    {
      goto describe;
    }

  /* A different geometry: the old sets are the wrong size, so they are
   * released before new ones are taken.  Holding both would peak at twice
   * the footprint for no reason.
   */

  rk3576_vepu_recn_free();

  rk3576_vepu510_recn_size(frm->width, frm->height, &size);
  g_vepu_recn_size = size;

  for (i = 0; i < RK3576_VEPU_RECN_SLOTS; i++)
    {
      g_vepu_recn[i].pixel = rk3576_dma_alloc(size.pixel);
      if (g_vepu_recn[i].pixel == NULL)
        {
          goto errout_nomem;
        }

      g_vepu_recn[i].thumb = rk3576_dma_alloc(size.thumb);
      if (g_vepu_recn[i].thumb == NULL)
        {
          goto errout_nomem;
        }

      g_vepu_recn[i].smear = rk3576_dma_alloc(size.smear);
      if (g_vepu_recn[i].smear == NULL)
        {
          goto errout_nomem;
        }
    }

  g_vepu_recn_width = frm->width;
  g_vepu_recn_height = frm->height;

  vinfo("VEPU0: reconstruction sets for %ux%u: pixel %" PRIu32
        " (header %" PRIu32 "), thumb %" PRIu32 ", smear %" PRIu32
        ", slots %08" PRIxPTR "/%08" PRIxPTR ", x%d\n",
        (unsigned int)frm->width, (unsigned int)frm->height, size.pixel,
        size.header, size.thumb, size.smear, (uintptr_t)g_vepu_recn[0].pixel,
        (uintptr_t)g_vepu_recn[1].pixel, RK3576_VEPU_RECN_SLOTS);

describe:
  /* The published pointer is the virtual address, which for this heap is
   * also the physical one.  Stating that in one place beats scattering
   * casts through the register layer.
   */

  curr = g_vepu_recn_next;

  frm->recn.pixel_phys = (uint32_t)(uintptr_t)g_vepu_recn[curr].pixel;
  frm->recn.body_offset = g_vepu_recn_size.header;
  frm->recn.thumb_phys = (uint32_t)(uintptr_t)g_vepu_recn[curr].thumb;
  frm->recn.smear_phys = (uint32_t)(uintptr_t)g_vepu_recn[curr].smear;

  /* The other slot is the reference, if it still holds one.  A picture that
   * is not a reference and never reads one gets no reference described: the
   * register layer then leaves the read side alone rather than being handed
   * addresses it must not use.
   *
   * With two slots "the other one" and "the one the last job wrote" are the
   * same slot, because a job only ever writes the slot it is not reading.  So
   * the shift below is not a small ring buffer's arithmetic dressed up; it is
   * the statement that this job's output will overwrite the oldest picture,
   * which is what keeps the reference alive until the job has read it.
   */

  frm->ref_valid = g_vepu_recn_ref_valid && !idr;

  if (frm->ref_valid)
    {
      uint32_t prev =
          (curr + RK3576_VEPU_RECN_SLOTS - 1u) % RK3576_VEPU_RECN_SLOTS;

      frm->ref.pixel_phys = (uint32_t)(uintptr_t)g_vepu_recn[prev].pixel;
      frm->ref.body_offset = g_vepu_recn_size.header;
      frm->ref.thumb_phys = (uint32_t)(uintptr_t)g_vepu_recn[prev].thumb;
      frm->ref.smear_phys = (uint32_t)(uintptr_t)g_vepu_recn[prev].smear;
    }
  else
    {
      memset(&frm->ref, 0, sizeof(frm->ref));
    }

  return OK;

errout_nomem:
  _err("ERROR: VEPU0 out of DMA heap for the reconstruction sets "
       "(pixel %" PRIu32 ", thumb %" PRIu32 ", smear %" PRIu32 ", x%d)\n",
       size.pixel, size.thumb, size.smear, RK3576_VEPU_RECN_SLOTS);
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
                       FAR const struct rk3576_vepu510_slice_s *slice,
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

  if (frm == NULL || cfg == NULL || slice == NULL)
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

  /* The anti-smear thresholds are derived from what the previous picture
   * reported.  This is copied in rather than read from the globals by the
   * register layer, because the register layer takes one picture at a time
   * and has no notion of a sequence.
   */

  job.prev = g_vepu_last_fb;

  ret = rk3576_vepu_recn_alloc(&job, slice->idr);
  if (ret < 0)
    {
      goto errout_unlock;
    }

  /* A picture that predicts from another needs one to predict from, and the
   * driver is the only place that knows whether there is one.  Left
   * unchecked the hardware would happily encode a P picture whose reference
   * fetch reads whatever the read-side registers happened to name -- and it
   * would report success, so the first sign of trouble would be a decoder
   * showing noise.  Refusing is what turns that into an error at the place
   * the GOP state is wrong.
   */

  if (!slice->idr && !job.ref_valid)
    {
      _err("ERROR: VEPU0 asked for a P picture with no reference picture "
           "-- the stream has to restart with an IDR\n");
      ret = -EINVAL;
      goto errout_unlock;
    }

  /* Everything the encoder is told comes from here: the picture geometry and
   * both buffers, the coding tools, the syntax parameters and the slice
   * values.  This is the whole of the register knowledge; nothing below
   * needs to know what a block is called.
   */

  ret = rk3576_vepu510_regs_build(&g_vepu_regs, &job, cfg, slice);
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

  /* The reconstruction slot this job wrote now holds a picture, so it is the
   * one the next job reads and the other becomes the next write target.
   * Every picture this driver encodes is a reference picture, so the pair
   * simply alternates -- there is no picture here that a later one must not
   * use.
   */

  g_vepu_recn_next = (g_vepu_recn_next + 1u) % RK3576_VEPU_RECN_SLOTS;
  g_vepu_recn_ref_valid = true;

  /* Capture what this picture reported, for the next one's anti-smear
   * thresholds.  The macroblock count is not a status field -- it is the
   * picture's geometry, which the driver knows -- but MPP scales the
   * hardware's counts against it, so the two travel together.
   *
   * This happens only on success: a job that failed did not report anything a
   * later picture should be tuned from, and leaving the previous feedback in
   * place is exactly what MPP does with a re-encoded frame.
   */

  g_vepu_last_fb.frame_type =
      slice->idr ? RK3576_H264_SLICE_I : RK3576_H264_SLICE_P;
  g_vepu_last_fb.mb_num =
      ((frm->width + 15u) / 16u) * ((frm->height + 15u) / 16u);
  rk3576_vepu510_status_smear(
      rk3576_vepu_getreg(RK3576_VEPU510_ST_SMEAR_CNT_OFFSET),
      g_vepu_last_fb.smear_cnt);

errout_unlock:
  /* A failed job may have written anything into the slot it was using, and a
   * reference that is silently wrong is worse than one that is known to be
   * missing: it produces a stream that decodes to noise rather than an
   * error.  So the reference is dropped and the next P picture refuses to
   * run until an IDR has re-established one.  Note the slot is not advanced
   * either, so the damaged one is what that IDR overwrites.
   */

  if (ret < 0)
    {
      g_vepu_recn_ref_valid = false;
    }

  nxmutex_unlock(&g_vepu_lock);
  return ret;
}

#ifdef CONFIG_RK3576_VEPU_SELFTEST

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

/* Geometry of the sequence test, which is the same picture size asking a
 * different question: not "does it encode" but "does it predict".
 */

#define RK3576_VEPU_SEQ_W      320u
#define RK3576_VEPU_SEQ_H      240u
#define RK3576_VEPU_SEQ_FRAMES 8u
#define RK3576_VEPU_SEQ_GOP    2u
#define RK3576_VEPU_SEQ_SHIFT  4u
#define RK3576_VEPU_SEQ_DST    (1024u * 1024u)

/* How much of each frame the sequence test prints, in bytes.
 *
 * Enough for the slice header and the first macroblock, which is all it takes
 * to tell a picture that used its reference from one that did not: a P slice
 * begins with mb_skip_run, so a predicted picture starts with a long run of
 * skips and one that coded its macroblocks starts with a zero followed by an
 * intra type.  Bounded rather than complete because it has to be readable in
 * a log, and the answer is in the first few bytes of either kind.
 */

#define RK3576_VEPU_SEQ_PEEK 64u

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

/****************************************************************************
 * Name: rk3576_vepu_fill_gradient_shifted
 *
 * Description:
 *   The gradient, moved sideways by a whole number of pixels.
 *
 *   This exists because of what a still source cannot test.  Every check in
 *   the sequence test below is about prediction, and on a source that never
 *   changes a correct prediction and a costly one are the same thing: a frame
 *   that copied its reference exactly and a frame whose reference merely
 *   happened to be the right picture cost the same handful of bytes, and a
 *   reference that is wrong looks no different from one that is right.
 *
 *   Moving each frame a little makes the two cases separate behaviour rather
 *   than the same behaviour with different explanations: a frame that fails
 *   to predict spends an intra picture's worth of bits, and one whose
 *   reference is wrong decodes to a picture that is not the source.
 *
 *   The wrap is on the column index, not on the result, because that is the
 *   arithmetic the host-side source regenerator repeats when it checks a
 *   stream taken off the board.
 *
 ****************************************************************************/

static void rk3576_vepu_fill_gradient_shifted(FAR uint8_t *y, uint32_t width,
                                              uint32_t height, uint32_t frame)
{
  uint32_t row;
  uint32_t col;

  for (row = 0; row < height; row++)
    {
      for (col = 0; col < width; col++)
        {
          y[row * width + col] =
              (uint8_t)((row * 255u) / height +
                        (((col + frame * RK3576_VEPU_SEQ_SHIFT) % width) *
                         255u) /
                            width);
        }
    }
}

/****************************************************************************
 * Name: rk3576_vepu_selftest_peek
 *
 * Description:
 *   Print the first bytes of a frame as hex, so that what the encoder
 *   produced can be read back rather than inferred from its size.
 *
 *   Size alone cannot say why a picture is expensive.  A P picture that
 *   coded every macroblock intra and one that coded inter with useless
 *   motion vectors both come out at about an I picture's size, and the two
 *   have opposite causes: the first means the reference the encoder read was
 *   not the picture it was given, the second that the search or the syntax
 *   around it is wrong.  The slice header and the first macroblock type are
 *   what separate them, and this is what puts them in the log.
 *
 ****************************************************************************/

static void rk3576_vepu_selftest_peek(FAR const char *what,
                                      FAR const uint8_t *data, uint32_t len)
{
  char text[2u * RK3576_VEPU_SEQ_PEEK + 1u];
  uint32_t i;

  if (len > RK3576_VEPU_SEQ_PEEK)
    {
      len = RK3576_VEPU_SEQ_PEEK;
    }

  /* Each snprintf writes its own terminator over the previous one's, and the
   * final terminator is put back after the loop. */

  for (i = 0; i < len; i++)
    {
      snprintf(text + 2u * i, 3, "%02x", data[i]);
    }

  text[2u * i] = '\0';

  vinfo("VEPU0 sequence test: %s, first %" PRIu32 " bytes: %s\n", what, len,
        text);
}

/****************************************************************************
 * Name: rk3576_vepu_selftest_intra_blocks
 *
 * Description:
 *   How many macroblocks the encoder says it coded intra, out of the four
 *   counts the status block reports for the picture just finished.
 *
 *   This is the only place the choice between intra and inter coding of a
 *   finished picture is visible, and it is what separates two faults that a
 *   length cannot.  A picture that costs an I picture's worth of bits while
 *   claiming to be predicted is either a picture whose reference was useless
 *   to it -- in which case it codes itself intra -- or a picture that
 *   predicted badly, in which case it does not.  MPP reads the same fields
 *   for its rate control, and the count is bounded by the macroblock count,
 *   so a number outside that range is itself a fault worth seeing.
 *
 ****************************************************************************/

static uint32_t rk3576_vepu_selftest_intra_blocks(void)
{
  return (rk3576_vepu_getreg(RK3576_VEPU510_ST_PNUM_I32_OFFSET) +
          rk3576_vepu_getreg(RK3576_VEPU510_ST_PNUM_I16_OFFSET) +
          rk3576_vepu_getreg(RK3576_VEPU510_ST_PNUM_I8_OFFSET) +
          rk3576_vepu_getreg(RK3576_VEPU510_ST_PNUM_I4_OFFSET)) &
         RK3576_VEPU510_ST_PNUM_MASK;
}

/****************************************************************************
 * Name: rk3576_vepu_selftest_dump
 *
 * Description:
 *   Print a range of bytes as hex, one line per RK3576_VEPU_SEQ_DUMP_COLS,
 *   with the byte offset in front of each.
 *
 *   This exists so that a bitstream can be taken off the board and decoded
 *   somewhere else.  Everything else this test measures is the encoder's own
 *   account of what it produced -- a length, a distortion figure, a macroblock
 *   type read back out of the bytes -- and none of it can show whether a
 *   decoder reconstructs the pictures the encoder believes it reconstructed.
 *   That question needs an actual decoder, and an actual decoder needs the
 *   bytes.
 *
 *   The offsets are printed so that a capture which lost lines can be seen to
 *   have lost them rather than being silently short: the stream only decodes
 *   if every byte is present.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_VEPU_SEQ_DUMP

/* How many bytes of hex go on one line of the dump.  Chosen so a line stays
 * one log line in every terminal this will be read in; a wrapped line is a
 * line that cannot be counted, and counting them is what says whether the
 * capture arrived whole.
 */

#define RK3576_VEPU_SEQ_DUMP_COLS 64u

static void rk3576_vepu_selftest_dump(FAR const char *what,
                                      FAR const uint8_t *data, uint32_t len)
{
  char text[2u * RK3576_VEPU_SEQ_DUMP_COLS + 1u];
  uint32_t row;
  uint32_t i;

  _info("VEPU0 dump %s length %" PRIu32 " bytes, %" PRIu32 " lines\n", what,
        len,
        (len + RK3576_VEPU_SEQ_DUMP_COLS - 1u) / RK3576_VEPU_SEQ_DUMP_COLS);

  for (row = 0; row < len; row += RK3576_VEPU_SEQ_DUMP_COLS)
    {
      uint32_t n = len - row;

      if (n > RK3576_VEPU_SEQ_DUMP_COLS)
        {
          n = RK3576_VEPU_SEQ_DUMP_COLS;
        }

      for (i = 0; i < n; i++)
        {
          snprintf(text + 2u * i, 3, "%02x", data[row + i]);
        }

      text[2u * n] = '\0';

      _info("VEPU0 dump %s %06" PRIx32 " %s\n", what, row, text);
    }

  _info("VEPU0 dump %s end\n", what);
}

#endif /* CONFIG_RK3576_VEPU_SEQ_DUMP */

/****************************************************************************
 * Name: rk3576_vepu_selftest_sequence
 *
 * Description:
 *   Encode a sequence: an IDR, then P pictures, then another IDR, and so on,
 *   with each picture moved sideways from the one before it.
 *
 *   This is a separate test from the picture test rather than more frames
 *   inside it, because nearly every expectation there is an expectation
 *   about an I picture -- its NAL type, the start code ahead of it, how its
 *   size compares with the other pictures'.  A P picture has none of those
 *   properties, so folding it in would mean giving up the checks that were
 *   worth having.
 *
 *   The group length is two, which is the driver's default.  That makes
 *   every P picture here predict from an IDR's reconstruction rather than
 *   from another P picture, which is the case the default is chosen for --
 *   see RK3576_VEPU_DEFAULT_GOP, and chips/rk3576/vepu/README.md for what a
 *   longer group does.
 *
 *   The source moves, which is what makes the prediction check mean
 *   something.  On a still source the encoder is free to spend almost
 *   nothing on a P picture and be right by accident, so a cheap frame there
 *   is not evidence of anything; here a correct prediction costs a fraction
 *   of an IDR and a failed one costs about a whole IDR.
 *
 *   Must run before any other encode: it opens by asking for a P picture
 *   with no reference in the slots, which is a state the driver passes
 *   through only once.
 *
 * Returned Value:
 *   The number of failed checks, so the caller can add it to its own.
 *
 ****************************************************************************/

static int rk3576_vepu_selftest_sequence(void)
{
  struct rk3576_vepu510_frame_s frm;
  struct rk3576_h264_cfg_s cfg;
  struct rk3576_vepu510_slice_s slice;
  struct rk3576_vepu_result_s result;
  FAR uint8_t *src;
  FAR uint8_t *dst;
  uint32_t offset[RK3576_VEPU_SEQ_FRAMES];
  uint32_t length[RK3576_VEPU_SEQ_FRAMES];
  uint32_t intra[RK3576_VEPU_SEQ_FRAMES];
  uint32_t y_size = RK3576_VEPU_SEQ_W * RK3576_VEPU_SEQ_H;
  uint32_t src_size = y_size * 3u / 2u;
  uint32_t total = 0;
  uint32_t i;
  int bad = 0;
  int ret;

  src = rk3576_dma_alloc(src_size);
  dst = rk3576_dma_alloc(RK3576_VEPU_SEQ_DST);

  if (src == NULL || dst == NULL)
    {
      _err("ERROR: VEPU0 sequence test out of DMA heap (%" PRIu32 " + %" PRIu32
           " bytes)\n",
           src_size, RK3576_VEPU_SEQ_DST);
      bad++;
      goto errout;
    }

  memset(dst, 0, RK3576_VEPU_SEQ_DST);

  memset(&cfg, 0, sizeof(cfg));
  cfg.width = RK3576_VEPU_SEQ_W;
  cfg.height = RK3576_VEPU_SEQ_H;
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
  cfg.vui_en = 1;
  cfg.fps_num = 30;
  cfg.fps_den = 1;

  cfg.tune.scene_mode = 0;
  cfg.tune.atl_str = 1;
  cfg.tune.atr_str_i = 1;
  cfg.tune.atf_str = 1;
  cfg.tune.lambda_idx_i = 6;

  memset(&frm, 0, sizeof(frm));
  frm.src_fmt = RK3576_VEPU510_FMT_YUV420SP;
  frm.rbuv_swap = 0;
  frm.width = RK3576_VEPU_SEQ_W;
  frm.height = RK3576_VEPU_SEQ_H;
  frm.y_stride = RK3576_VEPU_SEQ_W;
  frm.v_stride = RK3576_VEPU_SEQ_H;
  frm.src_phys = (uint32_t)(uintptr_t)src;
  frm.src_size = src_size;
  frm.dst_phys = (uint32_t)(uintptr_t)dst;
  frm.dst_size = RK3576_VEPU_SEQ_DST;

  /* The guard: a P picture with nothing to predict from.
   *
   * Asked for before anything has been encoded, so the reference slots are
   * empty.  This is the one moment the state exists, and the driver has to
   * turn it into an error -- left to the hardware it would be a reference
   * fetch from wherever the read-side registers happened to point, reported
   * as a successful encode, and visible only as noise on a decoder.
   */

  memset(&slice, 0, sizeof(slice));
  slice.idr = 0;
  slice.frame_num = 1;
  slice.poc_lsb = 2;

  if (rk3576_vepu_encode(&frm, &cfg, &slice, &result) >= 0)
    {
      _err("ERROR: VEPU0 sequence test: a P picture with no reference was"
           " accepted\n");
      bad++;
    }

  for (i = 0; i < RK3576_VEPU_SEQ_FRAMES; i++)
    {
      uint32_t within = i % RK3576_VEPU_SEQ_GOP;
      uint32_t nal_type;
      uint32_t nal_ref_idc;
      uint32_t macroblocks = ((RK3576_VEPU_SEQ_W + 15u) / 16u) *
                             ((RK3576_VEPU_SEQ_H + 15u) / 16u);

      /* An IDR resets frame_num and the picture order count, and every other
       * picture of the group continues them.  That is the pair of rules the
       * driver's caller has to get right, and reproducing them here is what
       * makes this a sequence rather than a pile of pictures.
       */

      rk3576_vepu_fill_gradient_shifted(src, RK3576_VEPU_SEQ_W,
                                        RK3576_VEPU_SEQ_H, i);
      memset(src + y_size, 128, src_size - y_size);
      up_clean_dcache((uintptr_t)src, (uintptr_t)src + src_size);

      memset(&slice, 0, sizeof(slice));
      slice.idr = (within == 0);
      slice.frame_num = within;
      slice.poc_lsb = within * 2u;

      /* Consecutive IDRs are required to carry different idr_pic_id values
       * (H.264 7.4.3).  This test encodes several of them, so it is the place
       * that requirement is either met or missed.
       */

      slice.idr_pic_id = (i / RK3576_VEPU_SEQ_GOP) & 1u;

      offset[i] = total;
      frm.dst_offset = total;

      ret = rk3576_vepu_encode(&frm, &cfg, &slice, &result);
      if (ret < 0)
        {
          _err("ERROR: VEPU0 sequence test frame %" PRIu32 " (%s) failed:"
               " %d\n",
               i, slice.idr ? "IDR" : "P  ", ret);
          bad++;
          goto errout;
        }

      length[i] = result.bs_length;
      intra[i] = rk3576_vepu_selftest_intra_blocks();
      total += result.bs_length;

      vinfo("VEPU0 sequence test: frame %" PRIu32 " %s offset %6" PRIu32
            " length %6" PRIu32 " sse %10" PRIu32 " intra %" PRIu32 "/%" PRIu32
            "\n",
            i, slice.idr ? "IDR" : "P  ", offset[i], length[i], result.sse,
            intra[i], macroblocks);

      /* The start code is looked for where the frame was asked to begin,
       * not searched for: a search would find one wherever it happened to
       * be, which would make the offset an unchecked value.
       */

      if (!(dst[offset[i]] == 0 && dst[offset[i] + 1] == 0 &&
            dst[offset[i] + 2] == 0 && dst[offset[i] + 3] == 1))
        {
          _err("ERROR: VEPU0 sequence test frame %" PRIu32
               " has no start code at its offset\n",
               i);
          bad++;
          continue;
        }

      /* The two fields of the NAL header are the whole of what tells a
       * decoder whether this picture restarts the stream and whether it is
       * allowed to be predicted from.  Neither can be inferred from the
       * bitstream that follows, so both are read back here.
       */

      nal_ref_idc = (dst[offset[i] + 4] >> 5) & 0x3u;
      nal_type = dst[offset[i] + 4] & 0x1fu;

      if (nal_type != (slice.idr ? 5u : 1u))
        {
          _err("ERROR: VEPU0 sequence test frame %" PRIu32
               " has NAL type %" PRIu32 ", expected %u\n",
               i, nal_type, slice.idr ? 5u : 1u);
          bad++;
        }

      if (nal_ref_idc != (slice.idr ? 3u : 2u))
        {
          _err("ERROR: VEPU0 sequence test frame %" PRIu32
               " has nal_ref_idc %" PRIu32 ", expected %u\n",
               i, nal_ref_idc, slice.idr ? 3u : 2u);
          bad++;
        }

      if (length[i] == 0)
        {
          _err("ERROR: VEPU0 sequence test frame %" PRIu32
               " produced no bitstream\n",
               i);
          bad++;
        }
    }

  /* Did prediction do anything?
   *
   * A P picture whose reference is the picture before it, moved four pixels,
   * should cost a small fraction of an IDR.  One that costs about as much as
   * an IDR is a picture that did not use its reference -- and the intra count
   * says which of the two ways that happened: a picture that coded nearly
   * every macroblock intra decided its reference was no use to it, whereas
   * one that coded inter and still spent a full picture on it predicted badly.
   * The two have different causes, so the message names the one the counts
   * point at rather than guessing.
   *
   * The bound is deliberately loose.  The claim being tested is that the
   * reference was used at all, and that shows as an order of magnitude, not
   * as a few percent.
   */

  for (i = 0; i < RK3576_VEPU_SEQ_FRAMES; i++)
    {
      uint32_t macroblocks = ((RK3576_VEPU_SEQ_W + 15u) / 16u) *
                             ((RK3576_VEPU_SEQ_H + 15u) / 16u);

      if ((i % RK3576_VEPU_SEQ_GOP) == 0)
        {
          continue;
        }

      if (length[i] * 2u >= length[i - 1u])
        {
          _err("ERROR: VEPU0 sequence test frame %" PRIu32 " is %" PRIu32
               " bytes against its reference's %" PRIu32
               " -- predicting cost as much as not predicting"
               " (%" PRIu32 " of %" PRIu32 " macroblocks coded intra)\n",
               i, length[i], length[i - 1u], intra[i], macroblocks);
          bad++;
        }

      if (intra[i] > macroblocks)
        {
          _err("ERROR: VEPU0 sequence test frame %" PRIu32 " reports %" PRIu32
               " intra macroblocks of %" PRIu32 "\n",
               i, intra[i], macroblocks);
          bad++;
        }
    }

  /* One line, because this is the result of the test and not a trace of it.
   * The claim is that a P picture costs a fraction of the IDR it predicts
   * from, so that pair is what is worth reporting; the frame-by-frame sizes
   * are above, behind vinfo.
   *
   * The means are accumulated rather than indexed.  A summary that named
   * length[0] through length[7] would be wrong -- and wrong silently, in the
   * only output the test produces -- the first time the frame count or the
   * group length changed, and neither is fixed by anything this function
   * controls.
   */

  {
    uint32_t idr_n = 0;
    uint32_t idr_bytes = 0;
    uint32_t p_n = 0;
    uint32_t p_bytes = 0;

    for (i = 0; i < RK3576_VEPU_SEQ_FRAMES; i++)
      {
        if ((i % RK3576_VEPU_SEQ_GOP) == 0)
          {
            idr_n++;
            idr_bytes += length[i];
          }
        else
          {
            p_n++;
            p_bytes += length[i];
          }
      }

    _info("VEPU0 sequence test: %" PRIu32 " frames, %" PRIu32
          " bytes: IDR %" PRIu32 " B mean, P %" PRIu32 " B mean"
          " (%" PRIu32 "%% of an IDR)\n",
          RK3576_VEPU_SEQ_FRAMES, total, idr_n != 0 ? idr_bytes / idr_n : 0,
          p_n != 0 ? p_bytes / p_n : 0,
          idr_n != 0 && p_n != 0
              ? (100u * (p_bytes / p_n)) / (idr_bytes / idr_n)
              : 0);
  }

  /* What the encoder produced, so that a size can be turned into a reason.
   * The first group is enough: an IDR and the P picture that predicts from
   * it, which is the pair every later group repeats.
   */

  rk3576_vepu_selftest_peek("IDR frame 0", dst + offset[0], length[0]);
  rk3576_vepu_selftest_peek("P frame 1", dst + offset[1], length[1]);

#ifdef CONFIG_RK3576_VEPU_SEQ_DUMP
  rk3576_vepu_selftest_dump("seq", dst, total);
#endif

errout:
  if (src != NULL)
    {
      rk3576_dma_free(src, src_size);
    }

  if (dst != NULL)
    {
      rk3576_dma_free(dst, RK3576_VEPU_SEQ_DST);
    }

  return bad;
}

/* The picture test: static, gradient and noise sources, encoded independently
 * of one another, checked for the things a single picture can be wrong about.
 *
 * It says nothing about prediction, and cannot be run before the sequence
 * test has left the driver's reference state behind, so it goes second.
 */
static int rk3576_vepu_selftest_picture(void)
{
  struct rk3576_vepu510_frame_s frm;
  struct rk3576_h264_cfg_s cfg;
  struct rk3576_vepu510_slice_s slice;
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

      memset(&slice, 0, sizeof(slice));
      slice.idr = 1;
      slice.frame_num = 0;
      slice.poc_lsb = 0;
      slice.idr_pic_id = i & 1u;

      ret = rk3576_vepu_encode(&frm, &cfg, &slice, &result);
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

      vinfo("VEPU0 self-test: frame %" PRIu32 " %s offset %6" PRIu32
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
        vinfo("VEPU0 self-test: SPS %" PRIu32 " bytes, PPS %" PRIu32
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

/****************************************************************************
 * Name: rk3576_vepu_selftest
 *
 * Description:
 *   Run the encoder's bring-up acceptance tests.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   OK if every check passed, -EIO otherwise.
 *
 ****************************************************************************/

int rk3576_vepu_selftest(void)
{
  int bad = 0;

  /* The sequence test has to run before the picture test, and not merely
   * first for tidiness: what it observes is that a P picture predicts from
   * the reconstruction of the picture before it, and its first frame is
   * therefore an IDR that establishes that reference.  The picture test
   * leaves a reference behind too, of course, but it leaves the driver's
   * reference state pointing at whichever picture it happened to end on,
   * which is a state the sequence test would then be inheriting rather than
   * creating.
   */

  if (rk3576_vepu_selftest_sequence() < 0)
    {
      bad++;
    }

  if (rk3576_vepu_selftest_picture() < 0)
    {
      bad++;
    }

  if (bad > 0)
    {
      _err("ERROR: VEPU0 self-test: %d of 2 tests failed\n", bad);
      return -EIO;
    }

  return OK;
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
