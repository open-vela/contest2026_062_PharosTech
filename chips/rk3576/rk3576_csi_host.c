/****************************************************************************
 * chips/rk3576/rk3576_csi_host.c
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
 * RK3576 MIPI CSI HOST driver.
 *
 * Brings up the CSI-2 protocol parser and the DCPHY RX lanes that feed it,
 * for the capture path:
 *
 *   D-PHY RX  ->  CSI HOST  ->  VICAP  ->  DDR
 *
 * The controller itself is trivial -- thirteen registers, no DMA, no
 * buffers, no frame concept.  The work is in the ordering, which the TRM's
 * Chapter 19 application note states explicitly: N_LANES may only be
 * changed while the D-PHY lane is in the stop state, and CSI2_RESETN must
 * be released after the lane count is programmed.  The PHY is therefore
 * brought up first, while the controller is still held in reset.
 *
 * Interrupts are not used; see rk3576_csi_host.h for why.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>

#include "arm64_arch.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_csi_host.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_csi_host.h"
#include "rk3576_mipi_dcphy.h"

#ifdef CONFIG_RK3576_MIPI_CSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Only CSI HOST0 is wired to the DCPHY, so that is the only instance this
 * driver supports.  HOST1/2 hang off CSIDPHY0 and HOST3/4 off CSIDPHY1.
 */

#define RK3576_CSIHOST_SUPPORTED 0

/* CRU reset bit for presetn_csi_host_0: GATE_CON54 bit 4 gates the APB
 * clock, SOFTRST_CON54 bit 4 resets the same block.  The bits are "when
 * high, reset", so releasing means writing 0 through the hiword mask.
 */

#define RK3576_CSIHOST_PRESETN_CON 54
#define RK3576_CSIHOST_PRESETN_BIT 4

/* Every error bit masked.  See the file header for why the errors are not
 * used as interrupts.
 */

#define RK3576_CSIHOST_MSK_ALL    0xffffffffu

#define RK3576_CSIHOST_POLL_LOOPS 1000000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_csi_host_s
{
  mutex_t lock;     /* Serialises bring-up / teardown */
  uintptr_t base;   /* CSI HOST instance base */
  uint8_t host;     /* Instance index */
  uint8_t lanes;    /* Active data lanes */
  uint32_t hs_rate; /* Lane rate in Hz */
  bool initialized; /* Controller out of reset */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_csi_host_s g_csi_host = {
  .lock = NXMUTEX_INITIALIZER,
};

/* Instance base addresses, indexed by host number. */

static const uintptr_t g_rk3576_csihost_base[] = {
  RK3576_CSIHOST0_ADDR, RK3576_CSIHOST1_ADDR, RK3576_CSIHOST2_ADDR,
  RK3576_CSIHOST3_ADDR, RK3576_CSIHOST4_ADDR,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csi_host_getreg / putreg
 ****************************************************************************/

static inline uint32_t rk3576_csi_host_getreg(uintptr_t base, uint32_t offset)
{
  return getreg32(base + offset);
}

static inline void rk3576_csi_host_putreg(uintptr_t base, uint32_t offset,
                                          uint32_t value)
{
  putreg32(value, base + offset);
}

/****************************************************************************
 * Name: rk3576_csi_host_presetn_release
 *
 * Description:
 *   Release presetn_csi_host_n (CRU SOFTRST_CON54 bit n, "when high,
 *   reset").  Only the release side is written: the block is not running
 *   yet, so there is no in-flight state to tear down.
 *
 ****************************************************************************/

static void rk3576_csi_host_presetn_release(void)
{
  uint32_t reg =
      RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_CSIHOST_PRESETN_CON);

  putreg32(1u << (16 + RK3576_CSIHOST_PRESETN_BIT), reg);
  up_udelay(10);
}

/****************************************************************************
 * Name: rk3576_csi_host_clear_errors
 *
 * Description:
 *   Clear the error state.
 *
 *   The TRM marks ERR1 read-only and only documents low bits of ERR2 as
 *   writable, so this writes the observed value back to both: where the
 *   register is write-one-to-clear it clears, and where it is read-only the
 *   write is discarded.  The result is checked and logged rather than
 *   assumed -- whether ERR1 actually clears is one of the things the first
 *   board run establishes.
 *
 ****************************************************************************/

static void rk3576_csi_host_clear_errors(uintptr_t base)
{
  uint32_t err1 = rk3576_csi_host_getreg(base, RK3576_CSIHOST_ERR1);
  uint32_t err2 = rk3576_csi_host_getreg(base, RK3576_CSIHOST_ERR2);

  if (err1 != 0 || err2 != 0)
    {
      _info("CSI HOST%u: clearing error state ERR1=%08" PRIx32
            " ERR2=%08" PRIx32 "\n",
            (unsigned int)((base - g_rk3576_csihost_base[0]) / 0x10000u), err1,
            err2);
    }

  rk3576_csi_host_putreg(base, RK3576_CSIHOST_ERR1, err1);
  rk3576_csi_host_putreg(base, RK3576_CSIHOST_ERR2, err2);

  err1 = rk3576_csi_host_getreg(base, RK3576_CSIHOST_ERR1);
  if (err1 != 0)
    {
      _warn("CSI HOST: ERR1 does not clear (reads back %08" PRIx32
            "); it is read-only on this SoC, so use the masked "
            "interrupts-off poll path instead\n",
            err1);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csi_host_initialize
 ****************************************************************************/

int rk3576_csi_host_initialize(FAR const struct rk3576_csi_config *config)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  struct clk_s *pclk;
  struct clk_s *iclk;
  char name[24];
  uint8_t lanes;
  int ret;

  if (config == NULL ||
      config->host >= (uint8_t)(sizeof(g_rk3576_csihost_base) /
                                sizeof(g_rk3576_csihost_base[0])))
    {
      return -EINVAL;
    }

  if (config->lanes < 1 || config->lanes > 4)
    {
      return -EINVAL;
    }

  if (config->host != RK3576_CSIHOST_SUPPORTED)
    {
      _err("CSI HOST%u has no PHY driver (only HOST0, fed by the DCPHY RX "
           "lanes, is supported)\n",
           config->host);
      return -ENODEV;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  lanes = config->lanes;
  priv->host = config->host;
  priv->base = g_rk3576_csihost_base[config->host];
  priv->hs_rate = config->hs_rate;

  /* HOST APB clock (pclk_csi_host_n, GATE_CON54[4+n]) and the interface
   * clock (iclk_csihost0, GATE_CON54[11], fed by iclk_csihost01).
   */

  snprintf(name, sizeof(name), "pclk_csi_host_%u", (unsigned int)config->host);

  pclk = clk_get(name);
  if (pclk == NULL)
    {
      _err("ERROR: CSI HOST failed to get %s\n", name);
      ret = -ENODEV;
      goto errout_unlock;
    }

  iclk = clk_get("iclk_csihost0");
  if (iclk == NULL)
    {
      _err("ERROR: CSI HOST failed to get iclk_csihost0\n");
      ret = -ENODEV;
      goto errout_unlock;
    }

  ret = clk_enable(pclk);
  if (ret < 0)
    {
      _err("ERROR: CSI HOST failed to enable %s: %d\n", name, ret);
      goto errout_unlock;
    }

  ret = clk_enable(iclk);
  if (ret < 0)
    {
      _err("ERROR: CSI HOST failed to enable iclk_csihost0: %d\n", ret);
      clk_disable(pclk);
      goto errout_unlock;
    }

  /* Take the controller out of its APB reset.  It stays held in its CSI-2
   * reset (CSI2_RESETN, which resets to 0) until the very end.
   */

  rk3576_csi_host_presetn_release();

  /* Power on the D-PHY RX lanes first.  N_LANES below may only be written
   * while the lanes are in the stop state, and the lane count is what the
   * PHY is configured for, so the PHY has to be up before the controller
   * is let out of reset.
   */

  ret =
      rk3576_dcphy_rx_power_on(lanes, (config->hs_rate + 999999u) / 1000000u);
  if (ret < 0)
    {
      _err("ERROR: CSI HOST failed to power on the DCPHY RX lanes: %d\n", ret);
      clk_disable(iclk);
      clk_disable(pclk);
      goto errout_unlock;
    }

  /* N_LANES is encoded as "count - 1": 0 = 1 lane, 3 = 4 lanes. */

  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_N_LANES,
                         (uint32_t)(lanes - 1));

  /* CSI-2 over a D-PHY: clear sw_dsi_en and sw_cphy_en.  Writing the whole
   * register with its reset value keeps the (unused) datatype fields at
   * their documented reset values and clears sw_debug_en, which is a test
   * mode that VICAP would have to match.
   */

  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_CONTROL,
                         RK3576_CSIHOST_CONTROL_RESET);

  /* Mask every error source and drop any stale error state. */

  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_MSK1,
                         RK3576_CSIHOST_MSK_ALL);
  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_MSK2,
                         RK3576_CSIHOST_MSK_ALL);

  rk3576_csi_host_clear_errors(priv->base);

  /* Finally let the controller run. */

  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_CSI2_RESETN,
                         RK3576_CSIHOST_CSI2_RESETN_RELEASE);
  up_udelay(10);

  priv->lanes = lanes;
  priv->initialized = true;

  _info("CSI HOST%u: up, %u lanes, %" PRIu32 " Hz/lane, PHY_STATE=%08" PRIx32
        "\n",
        (unsigned int)config->host, (unsigned int)lanes, config->hs_rate,
        rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_PHY_STATE));

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_csi_host_uninitialize
 ****************************************************************************/

int rk3576_csi_host_uninitialize(void)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  char name[24];
  struct clk_s *pclk;
  struct clk_s *iclk;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  /* Hold the controller in reset and mask the errors before anything else,
   * so no packet can arrive while the PHY is being taken down.
   */

  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_CSI2_RESETN, 0);
  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_MSK1,
                         RK3576_CSIHOST_MSK_ALL);
  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_MSK2,
                         RK3576_CSIHOST_MSK_ALL);

  ret = rk3576_dcphy_rx_power_off();
  if (ret < 0)
    {
      _err("WARNING: CSI HOST failed to power off the DCPHY RX lanes: %d\n",
           ret);
    }

  snprintf(name, sizeof(name), "pclk_csi_host_%u", (unsigned int)priv->host);

  pclk = clk_get(name);
  if (pclk != NULL)
    {
      clk_disable(pclk);
    }

  iclk = clk_get("iclk_csihost0");
  if (iclk != NULL)
    {
      clk_disable(iclk);
    }

  priv->initialized = false;
  priv->lanes = 0;

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_csi_host_set_lane_rate
 ****************************************************************************/

int rk3576_csi_host_set_lane_rate(uint32_t hs_rate)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t mbps = (hs_rate + 999999u) / 1000000u;
  int ret;

  if (hs_rate == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODEV;
    }

  /* The controller is held in reset and its errors are masked while the PHY
   * is being reprogrammed, so nothing arrives half-decoded.  This is the
   * same order the bring-up uses, for the same reason: the controller must
   * not be listening to a lane whose timing parameters are mid-change.
   */

  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_CSI2_RESETN, 0);
  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_MSK1,
                         RK3576_CSIHOST_MSK_ALL);
  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_MSK2,
                         RK3576_CSIHOST_MSK_ALL);

  /* Down and up through the D-PHY's own power-off/power-on pair, which is
   * where the per-rate timing parameters (settle, dlysel) are programmed.
   * Only those two registers change between rates; the lane count, the
   * lane enables, the BIAS block and the CSI-2 mode selection are all the
   * same afterwards, which is why this is a rate change and not a bring-up.
   *
   * A rate change is only legal while the sensor is not driving the lanes:
   * the data lanes have to be in the stop state for the PHY to accept new
   * timing parameters, and the caller is the one that knows whether the
   * sensor is streaming.  The capture device has to be closed, in practice,
   * because its stream is what starts the sensor.
   */

  ret = rk3576_dcphy_rx_power_off();
  if (ret < 0)
    {
      _err("WARNING: CSI HOST could not power down the DCPHY RX lanes: %d\n",
           ret);
    }

  ret = rk3576_dcphy_rx_power_on(priv->lanes, mbps);
  if (ret < 0)
    {
      _err("ERROR: CSI HOST failed to bring the DCPHY RX lanes back up at"
           " %" PRIu32 " Mbps: %d\n",
           mbps, ret);
      goto errout;
    }

  priv->hs_rate = hs_rate;

  rk3576_csi_host_clear_errors(priv->base);
  rk3576_csi_host_putreg(priv->base, RK3576_CSIHOST_CSI2_RESETN,
                         RK3576_CSIHOST_CSI2_RESETN_RELEASE);
  up_udelay(10);

  _info("CSI HOST%u: %u lanes at %" PRIu32 " Hz/lane, PHY_STATE=%08" PRIx32
        "\n",
        (unsigned int)priv->host, (unsigned int)priv->lanes, hs_rate,
        rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_PHY_STATE));

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_csi_host_get_phy_state / _get_err1 / _get_err2
 ****************************************************************************/

uint32_t rk3576_csi_host_get_phy_state(void)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t value = 0;

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return 0;
    }

  if (priv->initialized)
    {
      value = rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_PHY_STATE);
    }

  nxmutex_unlock(&priv->lock);
  return value;
}

uint32_t rk3576_csi_host_get_err1(void)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t value = 0;

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return 0;
    }

  if (priv->initialized)
    {
      value = rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_ERR1);
    }

  nxmutex_unlock(&priv->lock);
  return value;
}

uint32_t rk3576_csi_host_get_err2(void)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t value = 0;

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return 0;
    }

  if (priv->initialized)
    {
      value = rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_ERR2);
    }

  nxmutex_unlock(&priv->lock);
  return value;
}

/****************************************************************************
 * Name: rk3576_csi_host_lanes_in_stop_state
 ****************************************************************************/

bool rk3576_csi_host_lanes_in_stop_state(void)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t state;
  uint32_t want;
  bool result;
  unsigned int lane;

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return false;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return false;
    }

  state = rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_PHY_STATE);

  want = RK3576_CSIHOST_PHY_STATE_STOPSTATECLK;

  for (lane = 0; lane < priv->lanes; lane++)
    {
      want |= 1u << (RK3576_CSIHOST_PHY_STATE_STOPSTATEDATA_BASE + lane);
    }

  result = (state & want) == want;

  nxmutex_unlock(&priv->lock);
  return result;
}

/****************************************************************************
 * Name: rk3576_csi_host_is_receiving
 ****************************************************************************/

bool rk3576_csi_host_is_receiving(void)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t state;
  bool result;
  unsigned int lane;

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return false;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return false;
    }

  state = rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_PHY_STATE);
  result = false;

  for (lane = 0; lane < priv->lanes; lane++)
    {
      if ((state &
           (1u << (RK3576_CSIHOST_PHY_STATE_RXACTIVEHS_SHIFT + lane))) != 0)
        {
          result = true;
          break;
        }
    }

  nxmutex_unlock(&priv->lock);
  return result;
}

/****************************************************************************
 * Name: rk3576_csi_host_read_header
 ****************************************************************************/

int rk3576_csi_host_read_header(uint8_t index, FAR uint8_t *vc,
                                FAR uint8_t *dt, FAR uint16_t *word_count)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t value;
  int ret = OK;

  if (index > 3)
    {
      return -EINVAL;
    }

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return -EBUSY;
    }

  if (!priv->initialized)
    {
      ret = -EINVAL;
      goto out;
    }

  value =
      rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_HEADER_0 + index * 4u);

  if (vc != NULL)
    {
      *vc = (uint8_t)((value & RK3576_CSIHOST_HEADER_VC_MASK) >>
                      RK3576_CSIHOST_HEADER_VC_SHIFT);
    }

  if (dt != NULL)
    {
      *dt = (uint8_t)(value & RK3576_CSIHOST_HEADER_DATA_TYPE_MASK);
    }

  if (word_count != NULL)
    {
      *word_count =
          (uint16_t)((value & RK3576_CSIHOST_HEADER_WORD_COUNT_MASK) >>
                     RK3576_CSIHOST_HEADER_WORD_COUNT_SHIFT);
    }

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_csi_host_dump
 ****************************************************************************/

void rk3576_csi_host_dump(void)
{
  struct rk3576_csi_host_s *priv = &g_csi_host;
  uint32_t state;
  unsigned int i;

  if (nxmutex_lock(&priv->lock) < 0)
    {
      return;
    }

  if (!priv->initialized)
    {
      _info("CSI HOST: not initialised\n");
      nxmutex_unlock(&priv->lock);
      return;
    }

  state = rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_PHY_STATE);

  _info("CSI HOST%u: VERSION=%08" PRIx32 " N_LANES=%" PRIu32
        " CONTROL=%08" PRIx32 "\n",
        (unsigned int)priv->host,
        rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_VERSION),
        rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_N_LANES) & 3u,
        rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_CONTROL));

  _info("CSI HOST: PHY_STATE=%08" PRIx32
        " (stopstateclk=%u stopstatedata=0x%x rxactivehs=0x%x "
        "rxclkactivehs=%u)\n",
        state, (unsigned int)((state >> 10) & 1u),
        (unsigned int)((state >> RK3576_CSIHOST_PHY_STATE_STOPSTATEDATA_BASE) &
                       0xfu),
        (unsigned int)((state >> RK3576_CSIHOST_PHY_STATE_RXACTIVEHS_SHIFT) &
                       0xfu),
        (unsigned int)((state >> 8) & 1u));

  _info("CSI HOST: ERR1=%08" PRIx32 " ERR2=%08" PRIx32 "\n",
        rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_ERR1),
        rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_ERR2));

  for (i = 0; i < 4; i++)
    {
      uint32_t value =
          rk3576_csi_host_getreg(priv->base, RK3576_CSIHOST_HEADER_0 + i * 4u);

      _info("CSI HOST: HEADER_%u vc=%" PRIu32 " dt=0x%" PRIx32 " wc=%" PRIu32
            "\n",
            i, (value >> RK3576_CSIHOST_HEADER_VC_SHIFT) & 0x3u,
            value & RK3576_CSIHOST_HEADER_DATA_TYPE_MASK,
            (value >> RK3576_CSIHOST_HEADER_WORD_COUNT_SHIFT) & 0xffffu);
    }

  nxmutex_unlock(&priv->lock);
}

#endif /* CONFIG_RK3576_MIPI_CSI */
