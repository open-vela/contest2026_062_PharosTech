/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_wdt.c
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
 * RK3576 WDT (TRM Part1 Ch15) lower-half driver.  Implements the NuttX
 * struct watchdog_lowerhalf_s so the upper-half (drivers/timers/watchdog.c)
 * can register a /dev/watchdogN device.
 *
 * The WDT is a 32-bit down-counter.  This driver implements the non-secure
 * instance (RK3576_WDT_NS), whose counting clock is the fixed 24 MHz
 * oscillator; because that rate is fixed and the WDT is initialized before
 * the clock tree, it is held as a compile-time constant rather than queried
 * through the CLK framework.  A timeout period is selected through a coarse
 * 16-entry table (WDT_TORR); the selected code only becomes effective on
 * the next kick.  The watchdog is kicked by writing the key 0x76 to
 * WDT_CRR.
 *
 * Response mode is fixed to "system reset" (WDT_CR.resp_mode = 0): on
 * timeout the SoC is reset.  Because the WDT enable bit can only be cleared
 * by a system reset, stop() reports -ENOSYS once the timer has started —
 * this matches other NuttX lower-half drivers (e.g. imxrt_wdog.c).
 *
 * The board is responsible for registering the returned handle with
 * watchdog_register(DEVPATH, lower) (e.g. from board_late_initialize()).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/spinlock.h>
#include <nuttx/timers/watchdog.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_wdt.h"
#include "rk3576_wdt.h"

#ifdef CONFIG_RK3576_WDT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Select the path to the registered watchdog timer device. */

#ifdef CONFIG_WATCHDOG_DEVPATH
#define RK3576_WDT_DEVNAME CONFIG_WATCHDOG_DEVPATH
#else
#define RK3576_WDT_DEVNAME RK3576_WDT_DEVPATH
#endif

/* Milli-seconds per second (for timeout conversion). */

#define RK3576_WDT_MSEC_PER_SEC 1000UL

/* Default timeout (ms) applied by rk3576_wdt_initialize() before any
 * WDIOC_SETTIMEOUT is issued.  Kept as a driver constant rather than a
 * Kconfig option: it is only a fallback for the case where the watchdog
 * is started before user space sets a timeout, and application code is
 * expected to always program an explicit timeout.  30 s balances boot
 * headroom against reset protection.
 */

#define RK3576_WDT_DEFAULT_TIMEOUT_MS 30000UL

/* Clamp timeout so the TORR table can always represent it.  The largest
 * table code (15) reloads 0x7fffffff counts; the smallest (0) reloads
 * 0xffff.  Computed against the selected counting clock.
 */

#define RK3576_WDT_MIN_COUNT 0xffffu
#define RK3576_WDT_MAX_COUNT 0x7fffffffu

/* ---- Global-reset routing for the WDT (TRM Part1) -------------------
 *
 * A WDT timeout alone does NOT reset the SoC.  The WDT reset output is
 * merely a request; it must be routed to the CRU global soft reset by
 * setting two enable bits (both default to 0 = disabled):
 *
 *   1. CRU_GLBRST_ST_NCLR[glbrst_wdtns_rst] - the WDT_NS instance this
 *      driver uses is allowed to drive the CRU global reset.
 *
 *   2. CRU_GLB_RST_CON[wdt_trig_glbrst_en]  - WDT (any instance) is
 *      allowed to trigger the CRU global soft reset.
 *
 * These CRU fields are normal RW (no hiword write-mask); we use read-
 * modify-write so we never clobber bits configured by the bootloader for
 * other reset sources.
 * -------------------------------------------------------------------- */

/* CRU_GLB_RST_CON bit 6: wdt_trig_glbrst_en (WDT triggers global reset). */

#define RK3576_CRU_GLB_RST_CON_WDT_TRIG_GLBRST_EN (1 << 6)

/* CRU_GLBRST_ST_NCLR[glbrst_wdtns_rst] - the WDT_NS instance may drive the
 * CRU global soft reset.  This is the only instance the driver uses (the
 * other hardware instances are not reachable/usable from NuttX).
 */

#define RK3576_CRU_GLBRST_ST_NCLR_WDT_NS (1 << 12) /* glbrst_wdtns_rst */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Lower-half driver instance.  The ops pointer must be the first member so
 * a struct watchdog_lowerhalf_s pointer can be up-cast to this structure.
 */

struct rk3576_wdt_s
{
  FAR const struct watchdog_ops_s *ops; /* Lower-half operations   */
  uintptr_t base;                       /* Register base (WDT_NS)  */
  uint32_t clk_hz;                      /* Counting clock (Hz)     */
  uint32_t min_timeout_ms;              /* Smallest representable   */
  uint32_t max_timeout_ms;              /* Largest representable    */
  uint32_t timeout;                     /* Current timeout (ms)    */
  bool started;                         /* WDT has been enabled    */
  spinlock_t lock;                      /* Protects register HW    */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_wdt_getreg(FAR struct rk3576_wdt_s *priv,
                                  unsigned int off);
static void rk3576_wdt_putreg(FAR struct rk3576_wdt_s *priv, unsigned int off,
                              uint32_t val);

static int rk3576_wdt_start(FAR struct watchdog_lowerhalf_s *lower);
static int rk3576_wdt_stop(FAR struct watchdog_lowerhalf_s *lower);
static int rk3576_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower);
static int rk3576_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                                FAR struct watchdog_status_s *status);
static int rk3576_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout);

/* Timeout (ms) <-> TORR table helpers. */

static uint32_t rk3576_wdt_ms_to_count(uint32_t clk_hz, uint32_t timeout_ms);
static bool rk3576_wdt_count_to_curr(FAR struct rk3576_wdt_s *priv,
                                     uint32_t *torr, uint32_t *timeout_ms);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Counting clock frequency for the WDT_NS instance implemented here (TRM
 * Part1 Ch15.1: the WDT counter clock can be chosen from a 24 MHz
 * oscillator or a 32 kHz clock).  WDT_NS's counting clock is hard-wired to
 * the 24 MHz oscillator (xin_osc0); it is a FIXED source that the driver
 * neither switches nor queries through the NuttX CLK framework at runtime
 * (the WDT is intentionally initialized before rk3576_clk_tree_initialize),
 * so it is a compile-time constant.  No frequency is asserted for the
 * PMU-domain WDT (not implemented, see rk3576_wdt_initialize), as the TRM
 * documents no fixed rate for its deep-sleep source.
 */

#define RK3576_WDT_OSC_HZ 24000000u /* 24 MHz oscillator (WDT_NS clock) */

/* Lower-half operations.  capture/ioctl are not implemented in this first
 * revision: the WDT runs in system-reset mode (resp_mode = 0) and all
 * config/status is covered by the supported methods.
 */

static const struct watchdog_ops_s g_rk3576_wdt_ops = {
  .start = rk3576_wdt_start,
  .stop = rk3576_wdt_stop,
  .keepalive = rk3576_wdt_keepalive,
  .getstatus = rk3576_wdt_getstatus,
  .settimeout = rk3576_wdt_settimeout,
  .capture = NULL,
  .ioctl = NULL,
};

/* Driver state for the single implemented instance (WDT_NS). */

static struct rk3576_wdt_s g_rk3576_wdt;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_wdt_getreg(FAR struct rk3576_wdt_s *priv,
                                  unsigned int off)
{
  return getreg32(priv->base + off);
}

static void rk3576_wdt_putreg(FAR struct rk3576_wdt_s *priv, unsigned int off,
                              uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_wdt_ms_to_count
 *
 * Description:
 *   Convert a timeout in milliseconds into a raw down-counter reload value
 *   for the configured counting clock.
 ****************************************************************************/

static uint32_t rk3576_wdt_ms_to_count(uint32_t clk_hz, uint32_t timeout_ms)
{
  uint64_t count;

  count = (uint64_t)timeout_ms * clk_hz;
  count /= RK3576_WDT_MSEC_PER_SEC;

  return (uint32_t)count;
}

/****************************************************************************
 * Name: rk3576_wdt_count_to_curr
 *
 * Description:
 *   Map a desired timeout (ms) onto the closest TORR table code that can
 *   represent it without truncation, and report the actual timeout the
 *   hardware will enforce.
 *
 * Input Parameters:
 *   priv       - The lower-half state
 *   torr       - Location to receive the TORR timeout_period code
 *   timeout_ms - On input, the requested timeout; on output, the resolved
 *                timeout the hardware will actually enforce.
 *
 * Returned Values:
 *   True if a representable code was found; false if out of range.
 ****************************************************************************/

static bool rk3576_wdt_count_to_curr(FAR struct rk3576_wdt_s *priv,
                                     uint32_t *torr, uint32_t *timeout_ms)
{
  uint32_t want;   /* Desired raw counter value                  */
  uint32_t period; /* TORR code candidate                        */

  want = rk3576_wdt_ms_to_count(priv->clk_hz, *timeout_ms);

  /* Out of range: too short or too long for the TORR table. */

  if (want < RK3576_WDT_MIN_COUNT)
    {
      return false;
    }

  if (want > RK3576_WDT_MAX_COUNT)
    {
      return false;
    }

  /* Choose the smallest code whose reload value is >= wanted count.
   * count_max(p) = (1 << p) - 1 -> need such that (1 << p) - 1 >= want,
   * i.e. 1 << p >= want + 1.
   */

  for (period = 0; period <= 15; period++)
    {
      if (WDT_TORR_CNT_MAX(period) >= want)
        {
          break;
        }
    }

  if (period > 15)
    {
      return false;
    }

  *torr = period;

  /* Report the timeout the selected code actually enforces. */

  *timeout_ms = (uint32_t)((uint64_t)WDT_TORR_CNT_MAX(period) *
                           RK3576_WDT_MSEC_PER_SEC / priv->clk_hz);

  return true;
}

/****************************************************************************
 * Name: rk3576_wdt_start
 *
 * Description:
 *   Start the watchdog timer, resetting the time to the current timeout.
 ****************************************************************************/

static int rk3576_wdt_start(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  irqstate_t flags;
  uint32_t period;
  uint32_t actual;

  /* Range-check / resolve the current timeout to a TORR code.  The
   * requested timeout is passed in/out through 'actual'.
   */

  actual = priv->timeout;

  if (!rk3576_wdt_count_to_curr(priv, &period, &actual))
    {
      wderr("WDT: timeout %u ms not representable\n",
            (unsigned int)priv->timeout);
      return -ERANGE;
    }

  flags = spin_lock_irqsave(&priv->lock);

  /* Route the WDT reset output to the CRU global soft reset.  Both the
   * CRU_GLBRST_ST_NCLR[glbrst_wdtns_rst] bit and the CRU_GLB_RST_CON
   * [wdt_trig_glbrst_en] master switch default to 0, i.e. without them the
   * WDT timeout would NOT reboot the SoC.  Read-modify-write so we
   * preserve bootloader config for the other reset sources.
   */

  {
    uint32_t glbrst;
    uint32_t glb_rst;

    glbrst = getreg32(RK3576_CRU_ADDR + RK3576_CRU_GLBRST_ST_NCLR);
    glbrst |= RK3576_CRU_GLBRST_ST_NCLR_WDT_NS;
    putreg32(glbrst, RK3576_CRU_ADDR + RK3576_CRU_GLBRST_ST_NCLR);

    glb_rst = getreg32(RK3576_CRU_ADDR + RK3576_CRU_GLB_RST_CON);
    glb_rst |= RK3576_CRU_GLB_RST_CON_WDT_TRIG_GLBRST_EN;
    putreg32(glb_rst, RK3576_CRU_ADDR + RK3576_CRU_GLB_RST_CON);
  }

  /* Select the timeout period (effective on next restart). */

  rk3576_wdt_putreg(priv, RK3576_WDT_TORR, WDT_TORR_TIMEOUT_PERIOD(period));

  /* Default reset pulse length (8 pclk), response mode = reset, enable. */

  rk3576_wdt_putreg(priv, RK3576_WDT_CR,
                    WDT_CR_RST_PLUSE_LEN_DEFAULT | WDT_CR_ENABLE);

  /* Restart the counter from the newly-selected timeout period. */

  rk3576_wdt_putreg(priv, RK3576_WDT_CRR, WDT_CRR_KICK_KEY);

  priv->started = true;
  priv->timeout = actual;

  spin_unlock_irqrestore(&priv->lock, flags);

  wdinfo("WDT started, timeout %u ms\n", (unsigned int)actual);
  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_stop
 *
 * Description:
 *   Stop the watchdog timer.  The RK3576 WDT enable bit can only be cleared
 *   by a system reset, so a started watchdog cannot be stopped in software.
 ****************************************************************************/

static int rk3576_wdt_stop(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;

  if (priv->started)
    {
      wderr("WDT: cannot stop once started (enable only clear by reset)\n");
      return -ENOSYS;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_keepalive
 *
 * Description:
 *   Reset the watchdog timer to the current timeout value, preventing any
 *   imminent timeout ("ping the dog").
 ****************************************************************************/

static int rk3576_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);
  rk3576_wdt_putreg(priv, RK3576_WDT_CRR, WDT_CRR_KICK_KEY);
  spin_unlock_irqrestore(&priv->lock, flags);

  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_getstatus
 *
 * Description:
 *   Get the current watchdog timer status.
 ****************************************************************************/

static int rk3576_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                                FAR struct watchdog_status_s *status)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  irqstate_t flags;
  uint32_t curr;

  DEBUGASSERT(status != NULL);

  memset(status, 0, sizeof(*status));

  status->flags = WDFLAGS_RESET; /* WDT resets the system on expiry */

  if (priv->started)
    {
      status->flags |= WDFLAGS_ACTIVE;
    }

  status->timeout = priv->timeout;

  /* Current down-counter value (0 if not yet started). */

  flags = spin_lock_irqsave(&priv->lock);
  curr = rk3576_wdt_getreg(priv, RK3576_WDT_CCVR);
  spin_unlock_irqrestore(&priv->lock, flags);

  status->timeleft =
      (uint32_t)((uint64_t)curr * RK3576_WDT_MSEC_PER_SEC / priv->clk_hz);

  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_settimeout
 *
 * Description:
 *   Set a new timeout value (in ms) and reset the watchdog timer.  The new
 *   TORR code takes effect on the next restart, so the counter is kicked.
 ****************************************************************************/

static int rk3576_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  irqstate_t flags;
  uint32_t period;
  uint32_t actual;

  /* The requested timeout is passed in/out through 'actual'. */

  actual = timeout;

  if (!rk3576_wdt_count_to_curr(priv, &period, &actual))
    {
      wderr("WDT: timeout %u ms out of range [%u, %u]\n",
            (unsigned int)timeout, (unsigned int)priv->min_timeout_ms,
            (unsigned int)priv->max_timeout_ms);
      return -ERANGE;
    }

  priv->timeout = actual;

  flags = spin_lock_irqsave(&priv->lock);

  rk3576_wdt_putreg(priv, RK3576_WDT_TORR, WDT_TORR_TIMEOUT_PERIOD(period));

  /* Reload the counter so the new period takes effect now. */

  if (priv->started)
    {
      rk3576_wdt_putreg(priv, RK3576_WDT_CRR, WDT_CRR_KICK_KEY);
    }

  spin_unlock_irqrestore(&priv->lock, flags);

  wdinfo("WDT timeout set to %u ms\n", (unsigned int)actual);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_wdt_initialize
 *
 * Description:
 *   Initialize the watchdog timer instance and return a lower-half handle
 *   for the board to pass to watchdog_register().  The initial state is
 *   disabled.
 *
 *   This driver implements only RK3576_WDT_NS (the non-secure watchdog,
 *   24 MHz).  It is the natural watchdog for NuttX kernel/user space: it
 *   needs no CRU staging by software, its pclk/tclk gates are opened by
 *   the bootloader, and its counting clock is a fixed 24 MHz.
 *
 *   RK3576_WDT_PMU is accepted as an input only so callers keep a
 *   future-proof API, but it is NOT implemented: initializing it returns
 *   NULL.  The PMU WDT would need extra support this driver does not
 *   provide (its counting clock/gates live on the PMU1CRU and would have
 *   to be staged here before the clock tree runs, and the deep-sleep
 *   source frequency is not documented) - add that support if the PMU WDT
 *   is ever needed, e.g. to keep the watchdog counting while the SoC is
 *   asleep.
 *
 *   The remaining hardware instances (WDT_S, NPU/DDR/BUS) can never be
 *   reached from this non-secure NuttX build (WDT_S is secure-world-only;
 *   NPU/DDR/BUS reset their subordinate MCUs, not the main CPU), so they
 *   are not part of this API and are rejected.
 *
 * Input Parameters:
 *   instance - WDT instance index: RK3576_WDT_NS to use, RK3576_WDT_PMU
 *              reserved (returns NULL, not implemented).
 *
 * Returned Values:
 *   A watchdog_lowerhalf_s handle on success; NULL if the requested
 *   instance is not implemented or the index is invalid.
 ****************************************************************************/

FAR struct watchdog_lowerhalf_s *rk3576_wdt_initialize(int instance)
{
  FAR struct rk3576_wdt_s *priv = &g_rk3576_wdt;

  /* RK3576_WDT_PMU is a reserved interface; it is not implemented. */

  if (instance == RK3576_WDT_PMU)
    {
      wderr("WDT: PMU instance not implemented (only NS is supported)\n");
      return NULL;
    }

  /* Only RK3576_WDT_NS is actually implemented. */

  if (instance != RK3576_WDT_NS)
    {
      wderr("WDT: invalid instance %d (only RK3576_WDT_NS is supported)\n",
            instance);
      return NULL;
    }

  priv->ops = &g_rk3576_wdt_ops;
  priv->base = RK3576_WDT_NS_ADDR;
  priv->clk_hz = RK3576_WDT_OSC_HZ;

  /* Timeout range, derived from the NS counting clock (24 MHz). */

  priv->min_timeout_ms = (uint32_t)((uint64_t)RK3576_WDT_MIN_COUNT *
                                    RK3576_WDT_MSEC_PER_SEC / priv->clk_hz);
  priv->max_timeout_ms = (uint32_t)((uint64_t)RK3576_WDT_MAX_COUNT *
                                    RK3576_WDT_MSEC_PER_SEC / priv->clk_hz);

  priv->started = false;
  priv->timeout = RK3576_WDT_DEFAULT_TIMEOUT_MS;

  spin_lock_init(&priv->lock);

  return (FAR struct watchdog_lowerhalf_s *)priv;
}

#endif /* CONFIG_RK3576_WDT */
