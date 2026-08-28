/****************************************************************************
 * chips/rk3576/rk3576_mipi_dsi.c
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
 * RK3576 MIPI DSI-2 host controller driver.
 *
 * Implements the NuttX generic MIPI DSI framework (struct mipi_dsi_host /
 * struct mipi_dsi_host_ops) on top of the RK3576 DSI-2 controller
 * (TRM Part 2, Chapter 18).
 *
 * The primary operating mode is Video mode: rk3576_mipi_dsi_enable_video()
 * programs the IPI (Image Pixel Interface) timing registers and switches
 * the host into Video mode (DSI2_MODE_CTRL = 0x3), where pixel data
 * received from the VOP is streamed out to the panel.
 *
 * Command transfer uses the DSI-2 Command Interface (CRI) register bank and
 * remains available as a secondary path (panel DCS init / generic
 * read-write); it is kept as "future support" for Command-mode operation:
 *   - Write: program DSI2_CRI_TX_HDR (header) then stream payload words to
 *     DSI2_CRI_TX_PLD.
 *   - Read:  set the read-request bit in the header, then poll
 *     DSI2_CORE_STATUS.cri_rd_data_avail and collect DSI2_CRI_RX_HDR/PLD.
 *
 * The PHY (DCPHY) is a separate driver; this host driver calls into it
 * through the rk3576_mipi_dcphy.h function API.
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

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>
#include <nuttx/video/mipi_display.h>
#include <nuttx/video/mipi_dsi.h>

#include "arm64_arch.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_mipi_dsi.h"
#include "rk3576_mipi_dcphy.h"
#include "rk3576_mipi_dsi.h"

#ifdef CONFIG_RK3576_MIPI_DSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Poll timeout for CRI command completion. */

#define RK3576_DSI_POLL_LOOPS (1000000)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* DSI-2 host operating mode.  The enum mirrors the controller's own
 * DSI2_MODE_CTRL operating modes (Command / Video), while also reflecting
 * the link lifecycle the driver owns, including the DCPHY: the PHY is
 * powered up as soon as the host enters Command mode (on initialize) so
 * that the DCS init sequence can be sent, and only later transitions to
 * Video mode on request.  Board code never touches the PHY directly.
 */

enum rk3576_dsi_mode_e
{
  RK3576_DSI_MODE_OFF = 0, /* Not initialized */
  RK3576_DSI_MODE_COMMAND, /* Command mode: PHY powered, DCS usable */
  RK3576_DSI_MODE_VIDEO,   /* Video mode: pixel stream active */
};

struct rk3576_dsi_s
{
  struct mipi_dsi_host host;    /* Must be first */
  mutex_t lock;                 /* Serializes CRI command transfer */
  uintptr_t base;               /* DSI2 controller base (0x27D80000) */
  struct clk_s *sclk;           /* clk_dsihost0 functional clock */
  struct clk_s *pclk;           /* pclk_dsihost0 APB clock */
  struct rk3576_dsi_config cfg; /* Link/PHY configuration */
  bool initialized;             /* Core powered up + clocks enabled once */
  enum rk3576_dsi_mode_e mode;  /* Current DSI2_MODE_CTRL operating mode */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_dsi_attach(FAR struct mipi_dsi_host *host,
                             FAR struct mipi_dsi_device *device);
static int rk3576_dsi_detach(FAR struct mipi_dsi_host *host,
                             FAR struct mipi_dsi_device *device);
static ssize_t rk3576_dsi_transfer(FAR struct mipi_dsi_host *host,
                                   FAR const struct mipi_dsi_msg *msg);
static int rk3576_dsi_phy_power_up(FAR struct rk3576_dsi_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct mipi_dsi_host_ops g_rk3576_dsi_ops = {
  rk3576_dsi_attach, rk3576_dsi_detach, rk3576_dsi_transfer
};

/* The RK3576 has a single DSI-2 host, so the driver state is a static
 * singleton.
 */

static struct rk3576_dsi_s g_dsi = {
  .base = RK3576_DSIHOST_ADDR,
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dsi_getreg / putreg / modifyreg
 ****************************************************************************/

static inline uint32_t rk3576_dsi_getreg(uintptr_t base, uint32_t offset)
{
  return getreg32(base + offset);
}

static inline void rk3576_dsi_putreg(uintptr_t base, uint32_t offset,
                                     uint32_t value)
{
  putreg32(value, base + offset);
}

/****************************************************************************
 * Name: rk3576_dsi_wait_cri_idle
 *
 * Description:
 *   Poll DSI2_CORE_STATUS.cri_busy until the command interface is idle.
 ****************************************************************************/

static int rk3576_dsi_wait_cri_idle(struct rk3576_dsi_s *priv)
{
  int loops = RK3576_DSI_POLL_LOOPS;

  while (loops-- > 0)
    {
      if ((rk3576_dsi_getreg(priv->base, RK3576_DSI2_CORE_STATUS) &
           DSI2_CORE_STATUS_CRI_BUSY) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_dsi_wait_cri_rd_data
 *
 * Description:
 *   Poll DSI2_CORE_STATUS.cri_rd_data_avail until read data is available.
 ****************************************************************************/

static int rk3576_dsi_wait_cri_rd_data(struct rk3576_dsi_s *priv)
{
  int loops = RK3576_DSI_POLL_LOOPS;

  while (loops-- > 0)
    {
      if ((rk3576_dsi_getreg(priv->base, RK3576_DSI2_CORE_STATUS) &
           DSI2_CORE_STATUS_CRI_RD_DATA_AVAIL) != 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_dsi_msg_is_read
 *
 * Description:
 *   Return true if the message is a read (has a receive buffer).
 ****************************************************************************/

static bool rk3576_dsi_msg_is_read(FAR const struct mipi_dsi_msg *msg)
{
  return (msg->rx_buf != NULL);
}

/****************************************************************************
 * Name: rk3576_dsi_attach
 ****************************************************************************/

static int rk3576_dsi_attach(FAR struct mipi_dsi_host *host,
                             FAR struct mipi_dsi_device *device)
{
  struct rk3576_dsi_s *priv = (struct rk3576_dsi_s *)host;

  DEBUGASSERT(priv != NULL && device != NULL);

  /* Nothing to program per-device here since the DSI-2 host carries a
   * single virtual-channel identifier in DSI2_DSI_VCID_CFG and the
   * per-message VC is embedded in the CRI header.  The device is simply
   * linked to the host.
   */

  device->host = host;
  return OK;
}

/****************************************************************************
 * Name: rk3576_dsi_detach
 ****************************************************************************/

static int rk3576_dsi_detach(FAR struct mipi_dsi_host *host,
                             FAR struct mipi_dsi_device *device)
{
  DEBUGASSERT(host != NULL && device != NULL);

  if (device->host != host)
    {
      return -EINVAL;
    }

  device->host = NULL;
  return OK;
}

/****************************************************************************
 * Name: rk3576_dsi_write_long
 *
 * Description:
 *   Transmit a long packet: write the header then stream payload words to
 *   DSI2_CRI_TX_PLD, one 32-bit word (up to 4 payload bytes) at a time.
 ****************************************************************************/

static ssize_t rk3576_dsi_write_long(struct rk3576_dsi_s *priv,
                                     uint8_t channel, uint8_t dtype,
                                     FAR const uint8_t *payload, size_t len,
                                     bool use_lpm)
{
  uintptr_t base = priv->base;
  uint32_t hdr;
  size_t remaining = len;
  size_t offset = 0;
  int ret;

  /* Wait for the CRI to be idle before starting a new command. */

  ret = rk3576_dsi_wait_cri_idle(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Build the long-packet header: word count in wc_msb/lsb, virtual
   * channel, data type, long-packet flag and TX mode (HS unless LPM).
   */

  hdr = (uint32_t)(channel & DSI2_CRI_TX_HDR_VC_MASK)
        << DSI2_CRI_TX_HDR_VC_SHIFT;
  hdr |= (uint32_t)(dtype & DSI2_CRI_TX_HDR_DT_MASK)
         << DSI2_CRI_TX_HDR_DT_SHIFT;
  hdr |= ((uint32_t)(len >> 8) & 0xff) << DSI2_CRI_TX_HDR_WC_MSB_SHIFT;
  hdr |= ((uint32_t)(len >> 0) & 0xff) << DSI2_CRI_TX_HDR_WC_LSB_SHIFT;
  hdr |= DSI2_CRI_TX_HDR_LONG;

  if (use_lpm)
    {
      hdr |= DSI2_CRI_TX_HDR_TX_MODE;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_CRI_TX_HDR, hdr);

  /* Stream payload in 32-bit words (byte_0/1/2/3). */

  while (remaining > 0)
    {
      uint32_t word = 0;
      int nbytes = remaining < 4 ? remaining : 4;
      int i;

      for (i = 0; i < nbytes; i++)
        {
          word |= (uint32_t)payload[offset + i] << (8 * i);
        }

      rk3576_dsi_putreg(base, RK3576_DSI2_CRI_TX_PLD, word);

      offset += nbytes;
      remaining -= nbytes;
    }

  /* Wait for the write to drain. */

  ret = rk3576_dsi_wait_cri_idle(priv);
  if (ret < 0)
    {
      return ret;
    }

  return len;
}

/****************************************************************************
 * Name: rk3576_dsi_write_short
 *
 * Description:
 *   Transmit a short packet (0-2 data bytes carried in the header's
 *   wc_msb/wc_lsb fields).
 ****************************************************************************/

static ssize_t rk3576_dsi_write_short(struct rk3576_dsi_s *priv,
                                      uint8_t channel, uint8_t dtype,
                                      FAR const uint8_t *data, size_t len,
                                      bool use_lpm)
{
  uintptr_t base = priv->base;
  uint32_t hdr;
  int ret;

  ret = rk3576_dsi_wait_cri_idle(priv);
  if (ret < 0)
    {
      return ret;
    }

  hdr = (uint32_t)(channel & DSI2_CRI_TX_HDR_VC_MASK)
        << DSI2_CRI_TX_HDR_VC_SHIFT;
  hdr |= (uint32_t)(dtype & DSI2_CRI_TX_HDR_DT_MASK)
         << DSI2_CRI_TX_HDR_DT_SHIFT;

  if (use_lpm)
    {
      hdr |= DSI2_CRI_TX_HDR_TX_MODE;
    }

  /* Short packet data: data0 -> wc_lsb, data1 -> wc_msb. */

  if (len > 0)
    {
      hdr |= (uint32_t)data[0] << DSI2_CRI_TX_HDR_WC_LSB_SHIFT;
    }

  if (len > 1)
    {
      hdr |= (uint32_t)data[1] << DSI2_CRI_TX_HDR_WC_MSB_SHIFT;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_CRI_TX_HDR, hdr);

  ret = rk3576_dsi_wait_cri_idle(priv);
  if (ret < 0)
    {
      return ret;
    }

  return len;
}

/****************************************************************************
 * Name: rk3576_dsi_read
 *
 * Description:
 *   Issue a read request through the CRI and collect the returned payload
 *   into rx_buf.  The word count for the response is taken from the RX
 *   header (or capped by rx_len).
 *
 *   Limitation: only a single read-request parameter byte (carried in
 *   wc_lsb) is supported, which covers DCS register reads (single-byte
 *   address).  Generic reads with a 2-byte parameter
 *   (MIPI_DSI_GENERIC_READ_2_PARAM) are not supported here; the second
 *   parameter byte (wc_msb) would be silently dropped.
 ****************************************************************************/

static ssize_t rk3576_dsi_read(struct rk3576_dsi_s *priv, uint8_t channel,
                               uint8_t dtype, uint8_t param,
                               FAR uint8_t *rx_buf, size_t rx_len,
                               bool use_lpm)
{
  uintptr_t base = priv->base;
  uint32_t hdr;
  uint32_t rx_hdr;
  uint16_t wc;
  size_t remaining;
  size_t offset = 0;
  int ret;

  ret = rk3576_dsi_wait_cri_idle(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Build the read-request header.  For a read the short-packet data field
   * carries the read parameter (e.g. a DCS register address).
   */

  hdr = (uint32_t)(channel & DSI2_CRI_TX_HDR_VC_MASK)
        << DSI2_CRI_TX_HDR_VC_SHIFT;
  hdr |= (uint32_t)(dtype & DSI2_CRI_TX_HDR_DT_MASK)
         << DSI2_CRI_TX_HDR_DT_SHIFT;
  hdr |= (uint32_t)param << DSI2_CRI_TX_HDR_WC_LSB_SHIFT;
  hdr |= DSI2_CRI_TX_HDR_RD;

  if (use_lpm)
    {
      hdr |= DSI2_CRI_TX_HDR_TX_MODE;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_CRI_TX_HDR, hdr);

  /* Wait for read data availability. */

  ret = rk3576_dsi_wait_cri_rd_data(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Read the response header. */

  rx_hdr = rk3576_dsi_getreg(base, RK3576_DSI2_CRI_RX_HDR);

  /* The response type is reported in the header's data_type field [5:0]:
   *   - short read response (1 byte): 0x11 / 0x21
   *   - short read response (2 bytes): 0x12 / 0x22
   *   - long read response:            0x1a / 0x1c
   *
   * Do NOT use the word-count fields to distinguish short from long: for a
   * short response those fields carry the returned data bytes (data0 in
   * wc_lsb, data1 in wc_msb), which may be any value (including 0), not a
   * word count.
   */

  switch (rx_hdr & DSI2_CRI_TX_HDR_DT_MASK)
    {
      case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
      case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
        {
          uint8_t lo = (uint8_t)(rx_hdr >> DSI2_CRI_TX_HDR_WC_LSB_SHIFT);

          if (rx_len > 0)
            {
              rx_buf[0] = lo;
            }

          return 1;
        }

      case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
      case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
        {
          uint8_t lo = (uint8_t)(rx_hdr >> DSI2_CRI_TX_HDR_WC_LSB_SHIFT);
          uint8_t hi = (uint8_t)(rx_hdr >> DSI2_CRI_TX_HDR_WC_MSB_SHIFT);

          if (rx_len > 0)
            {
              rx_buf[0] = lo;
            }

          if (rx_len > 1)
            {
              rx_buf[1] = hi;
            }

          return 2;
        }

      case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
      case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
        break;

      default:
        gerr("ERROR: DSI unexpected read response data type 0x%02x\n",
             rx_hdr & DSI2_CRI_TX_HDR_DT_MASK);
        return -EIO;
    }

  /* Long response: word count in wc_msb/wc_lsb, payload in CRI_RX_PLD. */

  wc = (uint16_t)(((rx_hdr >> DSI2_CRI_TX_HDR_WC_MSB_SHIFT) & 0xff) << 8) |
       (uint16_t)((rx_hdr >> DSI2_CRI_TX_HDR_WC_LSB_SHIFT) & 0xff);

  remaining = rx_len < wc ? rx_len : wc;

  while (remaining > 0)
    {
      uint32_t word = rk3576_dsi_getreg(base, RK3576_DSI2_CRI_RX_PLD);
      int nbytes = remaining < 4 ? remaining : 4;
      int i;

      for (i = 0; i < nbytes; i++)
        {
          rx_buf[offset + i] = (uint8_t)(word >> (8 * i));
        }

      offset += nbytes;
      remaining -= nbytes;
    }

  return rx_len < wc ? rx_len : wc;
}

/****************************************************************************
 * Name: rk3576_dsi_transfer
 ****************************************************************************/

static ssize_t rk3576_dsi_transfer(FAR struct mipi_dsi_host *host,
                                   FAR const struct mipi_dsi_msg *msg)
{
  struct rk3576_dsi_s *priv = (struct rk3576_dsi_s *)host;
  uint8_t channel;
  uint8_t dtype;
  bool is_read;
  ssize_t ret;

  DEBUGASSERT(priv != NULL && msg != NULL);

  /* Serialize access to the shared CRI command interface: a transfer is a
   * multi-step header/payload/poll sequence that must not be interleaved
   * with a concurrent transfer from another thread.
   */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* The CRI command path needs a powered (Command/Video) link.  The
   * controller is always powered after initialize() transitions it to
   * Command mode, so reject transfers only before that point.
   */

  if (priv->mode != RK3576_DSI_MODE_COMMAND &&
      priv->mode != RK3576_DSI_MODE_VIDEO)
    {
      ret = -EPERM;
      goto errout_unlock;
    }

  channel = msg->channel;
  dtype = msg->type;
  is_read = rk3576_dsi_msg_is_read(msg);

  /* Map the NuttX LPM flag onto the DSI-2 CRI low-power TX mode. */

  bool use_lpm = (msg->flags & MIPI_DSI_MSG_USE_LPM) != 0;

  /* Dispatch on packet format. */

  if (is_read)
    {
      uint8_t param = 0;

      /* Short read: the request parameter is in tx_buf[0] if present. */

      if (msg->tx_buf != NULL && msg->tx_len > 0)
        {
          param = ((FAR const uint8_t *)msg->tx_buf)[0];
        }

      ret = rk3576_dsi_read(priv, channel, dtype, param, msg->rx_buf,
                            msg->rx_len, use_lpm);
    }
  else if (mipi_dsi_packet_format_is_long(dtype))
    {
      ret = rk3576_dsi_write_long(priv, channel, dtype, msg->tx_buf,
                                  msg->tx_len, use_lpm);
    }
  else
    {
      ret = rk3576_dsi_write_short(priv, channel, dtype, msg->tx_buf,
                                   msg->tx_len, use_lpm);
    }

  nxmutex_unlock(&priv->lock);
  return ret;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_dsi_phy_power_up
 *
 * Description:
 *   Bring up the DCPHY (init + power on) on behalf of the DSI host.  This
 *   encapsulates all PHY-facing detail inside the DSI driver: board code
 *   only talks to the DSI host API and never to the DCPHY directly.
 *
 *   Called once from rk3576_mipi_dsi_initialize() when the host first
 *   enters Command mode, so that the panel DCS init sequence can be
 *   transmitted over the (now live) D-PHY lanes.
 *
 * Input Parameters:
 *   priv - DSI driver instance (lock held by caller).
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_dsi_phy_power_up(FAR struct rk3576_dsi_s *priv)
{
  int ret;

  ret = rk3576_dcphy_init();
  if (ret < 0)
    {
      gerr("ERROR: DSI failed to init DCPHY: %d\n", ret);
      return ret;
    }

  ret =
      rk3576_dcphy_power_on((uint8_t)priv->cfg.lanes, true, priv->cfg.hs_rate);
  if (ret < 0)
    {
      gerr("ERROR: DSI failed to power on DCPHY: %d\n", ret);
      rk3576_dcphy_power_off();
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mipi_dsi_initialize
 ****************************************************************************/

FAR struct mipi_dsi_host *
rk3576_mipi_dsi_initialize(FAR const struct rk3576_dsi_config *config)
{
  struct rk3576_dsi_s *priv = &g_dsi;
  uint32_t regval;
  uint32_t phy_mode;
  int ret;

  DEBUGASSERT(config != NULL);

  /* Validate the configuration before touching any register: a lanes value
   * outside 1..4 would corrupt the PHY_LANES field (which encodes n-1) in
   * DSI2_PHY_MODE_CFG.
   */

  if (config == NULL || config->lanes < 1 || config->lanes > 4)
    {
      return NULL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return NULL;
    }

  /* Exclusive initialization: reject a second init (do not re-run the
   * clock enable / reset / PHY-mode sequence on an already-initialized
   * controller).
   */

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return NULL;
    }

  /* Obtain the controller clocks (registered by rk3576_clk_tree.c). */

  priv->sclk = clk_get("clk_dsihost0");
  if (priv->sclk == NULL)
    {
      gerr("ERROR: DSI failed to get clk_dsihost0\n");
      goto errout_unlock;
    }

  priv->pclk = clk_get("pclk_dsihost0");
  if (priv->pclk == NULL)
    {
      gerr("ERROR: DSI failed to get pclk_dsihost0\n");
      goto errout_unlock;
    }

  ret = clk_enable(priv->sclk);
  if (ret < 0)
    {
      gerr("ERROR: DSI failed to enable clk_dsihost0: %d\n", ret);
      goto errout_unlock;
    }

  ret = clk_enable(priv->pclk);
  if (ret < 0)
    {
      gerr("ERROR: DSI failed to enable pclk_dsihost0: %d\n", ret);
      goto errout_disable_sclk;
    }

  /* Power up the core and release the soft-resets. */

  regval = rk3576_dsi_getreg(priv->base, RK3576_DSI2_PWR_UP);
  regval |= DSI2_PWR_UP_PWR_UP;
  rk3576_dsi_putreg(priv->base, RK3576_DSI2_PWR_UP, regval);

  /* Release all soft resets (active-low). */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_SOFT_RESET,
                    DSI2_SOFT_RESET_SYS_RSTN | DSI2_SOFT_RESET_PHY_RSTN |
                        DSI2_SOFT_RESET_IPI_RSTN);

  /* Configure the PHY interface.
   *
   * PHY type is fixed to D-PHY for now: C-PHY panels are rare, and C-PHY
   * needs a different lane/trio configuration plus PHY PLL setup
   * (see TRM 21.6.4.2).  If C-PHY support is ever required, extend this
   * site (and the DCPHY driver) instead of relying on register defaults.
   *
   * The PPI width is 32-bit (matches the VOP 32-bit pixel interface); the
   * lane count comes from the caller's configuration.
   */

  phy_mode = DSI2_PHY_MODE_PPI_WIDTH_32 |
             DSI2_PHY_MODE_PHY_LANES(config->lanes) |
             DSI2_PHY_MODE_PHY_TYPE_DPHY;
  rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_MODE_CFG, phy_mode);

  /* Save the link/PHY configuration for later use. */

  priv->cfg = *config;

  /* Bring up the DCPHY (init + power on) so the panel DCS init sequence
   * can be transmitted immediately after this call.  All PHY detail is
   * encapsulated in rk3576_dsi_phy_power_up().
   */

  ret = rk3576_dsi_phy_power_up(priv);
  if (ret < 0)
    {
      gerr("ERROR: DSI failed to power up DCPHY: %d\n", ret);
      goto errout_disable_pclk;
    }

  /* Enter Command mode: this is the default operating state where CRI
   * command transfers (DCS init / reads) work over the powered PHY.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_MODE_CTRL, DSI2_MODE_COMMAND);

  priv->host.bus = 0;
  priv->host.ops = &g_rk3576_dsi_ops;
  priv->mode = RK3576_DSI_MODE_COMMAND;
  priv->initialized = true;

  nxmutex_unlock(&priv->lock);
  return &priv->host;

errout_disable_pclk:
  clk_disable(priv->pclk);
errout_disable_sclk:
  clk_disable(priv->sclk);
errout_unlock:
  nxmutex_unlock(&priv->lock);
  return NULL;
}

/****************************************************************************
 * Name: rk3576_dsi_color_depth
 *
 * Description:
 *   Map the NuttX MIPI_DSI_FMT_* pixel format to the DSI2 IPI color depth
 *   field value (DSI2_IPI_COLOR_DEPTH_*).
 *
 *   Note: DSI2_IPI_COLOR_MAN_CFG's ipi_depth is a *bits-per-channel* value
 *   (5-6-5 / 6 / 8 per TRM 18.4.3), NOT bits-per-pixel.  This is
 *   orthogonal to the on-wire bits-per-pixel used for the horizontal
 *   timing math (mipi_dsi_pixel_format_to_bpp(): RGB666 loose = 24 bpp,
 *   RGB666 packed = 18 bpp).  Both loose and packed RGB666 have a 6-bit
 *   color depth, so they map to the same ipi_depth here; the loose/packed
 *   distinction is handled at the VOP pixel-packing layer, not by this
 *   register.
 ****************************************************************************/

static uint32_t rk3576_dsi_color_depth(uint8_t format)
{
  switch (format)
    {
      case MIPI_DSI_FMT_RGB565:
        return DSI2_IPI_COLOR_DEPTH_565;

      case MIPI_DSI_FMT_RGB666:
      case MIPI_DSI_FMT_RGB666_PACKED:
        return DSI2_IPI_COLOR_DEPTH_6;

      case MIPI_DSI_FMT_RGB888:
      default:
        return DSI2_IPI_COLOR_DEPTH_8;
    }
}

/****************************************************************************
 * Name: rk3576_dsi_hstx_cycles
 *
 * Description:
 *   Convert a horizontal count expressed in pixels into phy_hstx_clk
 *   cycles, as a fixed-point value with 13 integral and 16 fractional bits
 *   (the format expected by the DSI2 IPI horizontal-timing registers).
 *
 *   One pixel occupies (bpp / lanes) bit-clocks on the serial interface:
 *   the panel provides hactive/hfp/hbp/hsync in pixels and bpp bits per
 *   pixel, spread across `lanes` data lanes.
 *
 *   Returns UINT32_MAX if the integral part would overflow the 13-bit field
 *   (>= 8192 cycles) — callers must treat that as a timing-programming
 *   error.
 ****************************************************************************/

static uint32_t rk3576_dsi_hstx_cycles(uint32_t pixels, uint32_t lanes,
                                       uint32_t bpp)
{
  uint64_t cycles;

  cycles = (uint64_t)pixels * bpp / lanes;

  /* 13 integral bits: reject values >= 2^13 that would corrupt the
   * reserved bits [31:30] (or wrap the 32-bit register).
   */

  if (cycles >= (1u << 13))
    {
      gerr("ERROR: DSI horizontal time overflow (%llu cycles)\n",
           (unsigned long long)cycles);
      return UINT32_MAX;
    }

  return (uint32_t)(cycles << 16);
}

/****************************************************************************
 * Name: rk3576_dsi_enable_video
 *
 * Description:
 *   Program the DSI-2 IPI timing and color registers for the given panel
 *   timing, power on the DCPHY at the configured high-speed rate, then
 *   switch the host into Video mode.
 ****************************************************************************/

int rk3576_mipi_dsi_enable_video(
    FAR struct mipi_dsi_host *host,
    FAR const struct rk3576_dsi_video_timing *timing)
{
  struct rk3576_dsi_s *priv = (struct rk3576_dsi_s *)host;
  uintptr_t base;
  uint32_t lanes;
  uint32_t bpp;
  uint32_t htotal;
  uint32_t regval;
  uint32_t color;
  int ret;

  DEBUGASSERT(priv != NULL && timing != NULL);

  if (!priv->initialized)
    {
      return -EPERM;
    }

  /* Serialize with concurrent CRI command transfers, which share the
   * host's register bank (and the DCPHY).  Mode/timing changes must not
   * race a command transfer.
   */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Video mode may only be entered from Command mode, at which point the
   * DCPHY is already powered (brought up by initialize()).  This keeps the
   * PHY lifecycle inside the DSI driver and decouples it from the video
   * timing programming -- the DCS init sequence runs earlier, in Command
   * mode, over the same power link.
   */

  if (priv->mode != RK3576_DSI_MODE_COMMAND)
    {
      ret = -EBUSY;
      goto errout_unlock;
    }

  lanes = priv->cfg.lanes;
  bpp = (uint32_t)mipi_dsi_pixel_format_to_bpp(priv->cfg.format);
  if (bpp == 0 || lanes == 0 || lanes > 4)
    {
      ret = -EINVAL;
      goto errout_unlock;
    }

  /* Horizontal total = HACT + HSA + HBP + HFP, in pixels. */

  htotal = timing->hactive + timing->hsync_len + timing->hback_porch +
           timing->hfront_porch;

  base = priv->base;

  /* Configure the IPI color depth and pixel format. */

  color = rk3576_dsi_color_depth(priv->cfg.format) | DSI2_IPI_COLOR_FORMAT_RGB;
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_COLOR_MAN_CFG, color);

  /* Program the horizontal timing (fixed-point phy_hstx_clk cycles).
   * Each conversion is checked for 13-bit integral overflow.
   */

  regval = rk3576_dsi_hstx_cycles(timing->hsync_len, lanes, bpp);
  if (regval == UINT32_MAX)
    {
      ret = -EINVAL;
      goto errout_unlock;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HSA_MAN_CFG, regval);

  regval = rk3576_dsi_hstx_cycles(timing->hback_porch, lanes, bpp);
  if (regval == UINT32_MAX)
    {
      ret = -EINVAL;
      goto errout_unlock;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HBP_MAN_CFG, regval);

  regval = rk3576_dsi_hstx_cycles(timing->hactive, lanes, bpp);
  if (regval == UINT32_MAX)
    {
      ret = -EINVAL;
      goto errout_unlock;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HACT_MAN_CFG, regval);

  regval = rk3576_dsi_hstx_cycles(htotal, lanes, bpp);
  if (regval == UINT32_MAX)
    {
      ret = -EINVAL;
      goto errout_unlock;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HLINE_MAN_CFG, regval);

  /* Program the vertical timing (in lines). */

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VSA_MAN_CFG,
                    timing->vsync_len & DSI2_IPI_VSA_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VBP_MAN_CFG,
                    timing->vback_porch & DSI2_IPI_VBP_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VACT_MAN_CFG,
                    timing->vactive & DSI2_IPI_VACT_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VFP_MAN_CFG,
                    timing->vfront_porch & DSI2_IPI_VFP_LINES_MASK);

  /* Non-burst mode: one video packet per line (max_pix_pkt = hactive). */

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_PIX_PKT_CFG,
                    timing->hactive & DSI2_IPI_PIX_PKT_MAX_MASK);

  /* Configure the video-mode transmission type. */

  regval = (uint32_t)priv->cfg.video_mode & DSI2_VID_TX_VID_MODE_TYPE_MASK;
  rk3576_dsi_putreg(base, RK3576_DSI2_DSI_VID_TX_CFG, regval);

  /* Program the HSTX/IPI and HSTX/SYS clock ratios (fixed-point 6.16),
   * which let the controller synchronize its three clock domains (sys_clk,
   * ipi_clk = pixel clock, phy_hstx_clk = bit clock).
   *
   *   phy_ipi_ratio = hs_rate / pixel_clock
   *   phy_sys_ratio = hs_rate / sclk
   */

  if (timing->pixel_clock != 0)
    {
      uint64_t ipi_ratio =
          ((uint64_t)priv->cfg.hs_rate << 16) / timing->pixel_clock;
      rk3576_dsi_putreg(base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG,
                        (uint32_t)ipi_ratio & DSI2_PHY_IPI_RATIO_MASK);
    }

  if (priv->sclk != NULL)
    {
      uint32_t sclk_rate = clk_get_rate(priv->sclk);

      if (sclk_rate != 0)
        {
          uint64_t sys_ratio = ((uint64_t)priv->cfg.hs_rate << 16) / sclk_rate;
          rk3576_dsi_putreg(base, RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG,
                            (uint32_t)sys_ratio & DSI2_PHY_SYS_RATIO_MASK);
        }
    }

  /* Use manual timing (the MAN_CFG timing registers programmed above). */

  rk3576_dsi_putreg(base, RK3576_DSI2_MANUAL_MODE_CFG, DSI2_MANUAL_MODE_EN);

  /* Switch the host into Video mode.  The DCPHY is already powered (it was
   * brought up when the host entered Command mode), so no PHY programming
   * happens here.
   */

  rk3576_dsi_putreg(base, RK3576_DSI2_MODE_CTRL, DSI2_MODE_VIDEO);
  priv->mode = RK3576_DSI_MODE_VIDEO;

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_disable_video
 ****************************************************************************/

int rk3576_mipi_dsi_disable_video(FAR struct mipi_dsi_host *host)
{
  struct rk3576_dsi_s *priv = (struct rk3576_dsi_s *)host;
  int ret;

  DEBUGASSERT(priv != NULL);

  if (!priv->initialized)
    {
      return -EPERM;
    }

  /* Serialize with concurrent command transfers (see enable_video). */

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->mode != RK3576_DSI_MODE_VIDEO)
    {
      ret = -EBUSY;
      goto errout_unlock;
    }

  /* Return to Command mode so that DCS commands remain usable.  The DCPHY
   * stays powered (it is owned by the DSI driver and only taken down on a
   * full shutdown); low-power policy can be added later if needed.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_MODE_CTRL, DSI2_MODE_COMMAND);
  priv->mode = RK3576_DSI_MODE_COMMAND;

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

#endif /* CONFIG_RK3576_MIPI_DSI */
