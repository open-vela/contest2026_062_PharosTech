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
#include <stdio.h>
#include <string.h>

#include <syslog.h>

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

/* Poll timeout for leaving Video mode (1 us per iteration).  20 ms is orders
 * of magnitude longer than a healthy transition (which is a few microseconds)
 * yet short enough that a wedged transmitter is caught before the caller's
 * panel read is attempted.  Exceeding it triggers the soft-reset fallback in
 * rk3576_mipi_dsi_disable_video(). */

#define RK3576_DSI_MODE_SWITCH_LOOPS (20000)

/* DSI2_PHY_LP2HS_MAN_CFG / HS2LP_MAN_CFG field (bits 28:0, 13.16 fixed). */

#define DSI2_PHY_LP2HS_TIME_MASK 0x1fffffffu
#define DSI2_PHY_HS2LP_TIME_MASK 0x1fffffffu

/* DSI2_INT_ST_TO (0x0410) timeout error flags (TRM 18.4.x). */

#define DSI2_INT_ST_TO_ERR_HSTX     (1u << 0) /* HS TX timeout */
#define DSI2_INT_ST_TO_ERR_HSTXRDY  (1u << 1) /* HS TX ready timeout */
#define DSI2_INT_ST_TO_ERR_LPRX     (1u << 2) /* LP RX timeout */
#define DSI2_INT_ST_TO_ERR_LPTXRDY  (1u << 3) /* LP TX data timeout */
#define DSI2_INT_ST_TO_ERR_LPTXTRIG (1u << 4) /* LP TX trigger timeout */
#define DSI2_INT_ST_TO_ERR_LPTXULPS (1u << 5) /* LP TX ULPS timeout */
#define DSI2_INT_ST_TO_ERR_BTA      (1u << 6) /* BTA timeout */

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

  /* Video timing as programmed by enable_video().  Kept so the horizontal
   * IPI timing + PHY_IPI_RATIO can be recomputed later
   * (rk3576_mipi_dsi_update_pixel_clock()) once the VOP has settled its
   * real dclk rate -- the nominal pixel_clock passed by the board is only
   * a request; the CRU divider chain usually lands on a nearby value.
   */

  struct rk3576_dsi_video_timing timing;
  bool timing_valid; /* timing above has been programmed */
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
static void rk3576_dsi_phy_link_cfg(FAR struct rk3576_dsi_s *priv);
static uint32_t rk3576_dsi_set_mode(uintptr_t base, uint32_t mode, int loops);
static void rk3576_dsi_rearm(FAR struct rk3576_dsi_s *priv);

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

  _err("wait cri idle timeout: DSI2_CORE_STATUS=0x%08x\n",
       (unsigned)rk3576_dsi_getreg(priv->base, RK3576_DSI2_CORE_STATUS));

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
 *   Transmit a long packet: stream the payload words to DSI2_CRI_TX_PLD
 *   first (one 32-bit word at a time), then write the packet header to
 *   DSI2_CRI_TX_HDR last, which triggers the CRI state machine to send the
 *   packet.  This "payload before header" order is required by the CRI: it
 *   consumes payload already staged in the FIFO once the header is written.
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

  /* Stream payload in 32-bit words (byte_0/1/2/3) BEFORE the header, so the
   * payload is staged in the CRI FIFO when the header triggers the send.
   */

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

  /* Build the long-packet header: word count in wc_msb/lsb, virtual
   * channel, data type, long-packet flag and TX mode (HS unless LPM).
   * Writing the header is what kicks off transmission.
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
 * Name: rk3576_dsi_phy_link_cfg
 *
 * Description:
 *   Configure the DSI-2 controller's PHY-facing link options that the CRI
 *   command path depends on but which are left at their reset values by
 *   rk3576_dsi_phy_power_up().  These are required for the very first DCS
 *   init command to complete:
 *
 *   - DSI2_PHY_CLK_CFG.phy_lptx_clk_div: the TX Escape clock.  Reset value
 *     0 turns the escape clock OFF (TRM 18.4.x: 5'b00000 = "phy_lptx_clk
 *     turned off").  The escape clock drives the LP TX driver and is the
 *     timebase for every controller timeout, so without it the CRI cannot
 *     complete even the LP->HS (SoT) handshake that precedes a command, and
 *     DSI2_CORE_STATUS.cri_busy never clears.  The Escape clock must be <=
 *     20 MHz (D-PHY spec is 20 MHz max), so divide sys_clk down to at most
 *     20 MHz: phy_lptx_clk = sys_clk / (2 * div).
 *
 *   - DSI2_PHY_CLK_CFG.clk_type: non-continuous clock lane by default
 *     (matches ILI9881D, whose clock lane returns to LP-11 after each HS
 *     burst per Table 46).  Set continuous_clk only for a panel that keeps
 *     HSCM for the whole frame.
 *
 *   - DSI2_DSI_GENERAL_CFG.BTA_EN / EOTP_TX_EN: Bus Turnaround (needed for
 *     DCS reads) and End-of-Transmission packet (long packets).
 *
 *   Called once from rk3576_mipi_dsi_initialize(), after the DCPHY is up
 *   but before the host enters Command mode.
 ****************************************************************************/

static void rk3576_dsi_phy_link_cfg(FAR struct rk3576_dsi_s *priv)
{
  uint32_t sclk_rate;
  uint32_t esc_div;
  uint32_t clk_cfg;

  /* Escape clock: phy_lptx_clk = sys_clk / (2 * esc_div) <= 20 MHz. */

  sclk_rate = priv->sclk != NULL ? clk_get_rate(priv->sclk) : 0;

  /* ceil(sclk_rate / 40 MHz): phy_lptx_clk = sys_clk / (2 * esc_div) <= 20
   * MHz. */

  esc_div = (sclk_rate + 40000000u - 1u) / 40000000u;
  if (esc_div == 0)
    {
      esc_div = 1;
    }

  if (esc_div > 31)
    {
      esc_div = 31;
    }

  /* sclk = 396 MHz is the TRM default, not a bug: CLKSEL_CON151 resets
   * clk_dsihost0_sel to 0b010 (clk_spll_mux) with div = 0x02 (/3), i.e.
   * spll(1188M)/3.  Reparenting to gpll would not raise it (both PLLs are
   * 1188 MHz) because the /3 divider remains.  phy_lptx_clk is then
   * 396M/(2*10) = 19.8 MHz, within the 20 MHz limit.
   */

  /* clk_type for the panel.  ILI9881D (ILI9881D_spec.txt) is a
   * NON-continuous clock lane: Table 46 defines THS-EXIT as "time to drive
   * LP-11 after HS burst", i.e. the clock lane returns to LP-11 (LPM)
   * after every HS burst (Figure 5: HSCM => HS-0 => LP-11), and it must
   * therefore be non-continuous before the
   * initial deskew calibration as well.  A continuous clock lane leaves the
   * lane in HS (never dropping to LP-11), which on this IP stalls the Host↔PHY
   * TXPPI handshake AND -- per the panel -- breaks the per-line SoT/HSDT
   * handshake (all data-lane stopstate bits collapse to 0, observed when
   * continuous was force-tested).  continuous_clk is only honored for a
   * panel that truly keeps HSCM across the whole frame; ILI9881D does not.
   */

  clk_cfg = (uint32_t)esc_div << DSI2_PHY_CLK_LPTX_DIV_SHIFT;
  if (priv->cfg.continuous_clk)
    {
      /* Clock lane stays in HS for the whole frame (continuous).  Only for
       * panels that keep HSCM; ILI9881D is not one of them (see above). */
      clk_cfg |= DSI2_PHY_CLK_TYPE_CONTINUOUS;
    }
  else
    {
      clk_cfg |= DSI2_PHY_CLK_TYPE_NONCONTINUOUS;
    }

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_CLK_CFG, clk_cfg);

  /* BTA (Bus Turnaround) for DCS reads, plus EoTp TX.  EOTP_TX_EN is cleared
   * only for panels that explicitly declare MIPI_DSI_MODE_NO_EOT_PACKET.
   *
   * Why EoTp matters here specifically: with a CONTINUOUS clock lane (which is
   * what a burst-mode panel is driven with -- see the board's
   * KICKPI_K7_DSI_CONTINUOUS_CLK) the clock never returns to LP-11, so the
   * End-of-Transmission packet is the ONLY in-band marker that tells the panel
   * where a transmission ends.  Without it the panel has no framing reference
   * at all: the lanes transmit correctly and the panel still shows nothing.
   * With a NON-continuous clock the LP-11 return provides that marker, so EoTp
   * is merely optional there.
   *
   * NOTE: an earlier revision of this driver disabled EoTp because enabling it
   * appeared to stall the HS send FSM.  That observation was made on a
   * non-continuous clock in a configuration that has since been shown to be
   * broken in other ways (wrong htotal, wrong video mode), and it has never
   * been retested -- so it is not evidence against EoTp.  Recording it here so
   * the bit is not flipped back and forth without a reason.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_DSI_GENERAL_CFG,
                    DSI2_GENERAL_BTA_EN |
                        (priv->cfg.eotp ? DSI2_GENERAL_EOTP_TX_EN : 0u));

  /* phy_sys_ratio (DSI2_PHY_SYS_RATIO_MAN_CFG): HSTX clock / SYS clock,
   * expressed as 1 integral + 16 fractional bits ([16:0]).
   *
   * The DSI-2 controller has three clock domains: ipi_clk (pixels),
   * sys_clk (CRI/system) and phy_hstx_clk (PHY high-speed).  A command is
   * generated in the sys_clk domain (CRI) and consumed in the phy_hstx_clk
   * domain (phy_txhs FIFO).  The CDC handshake between those two domains
   * cannot complete without a correct phy_sys_ratio; the reset value 0
   * means "hstx/sys = 0", which stalls the command exactly at the
   * sys->hstx crossing (payload stuck in phy_txhs, cri_busy never clears,
   * no timeout interrupt because the SoT never starts).
   *
   * phy_hstx_clk follows the same DCPHY HSTX_CLK_SEL divider as the
   * LP2HS/HS2LP timings below (see rk3576_dcphy_configure_tx_clock_lane).
   */

  {
    uint64_t phy_hstx_clk = (uint64_t)priv->cfg.hs_rate / 16u;
    uint32_t sys_ratio;

    if (sclk_rate != 0)
      {
        sys_ratio = (uint32_t)((phy_hstx_clk << 16) / sclk_rate);
      }
    else
      {
        /* No sys_clk rate available: fall back to ratio = 1.0. */

        sys_ratio = 1u << 16;
      }

    sys_ratio &= DSI2_PHY_SYS_RATIO_MASK;
    rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG,
                      sys_ratio);
  }

  /* LP->HS / HS->LP switching times.  Reset value 0 leaves the controller
   * with no notion of how long the PHY takes to switch lane direction, so
   * the CRI command (which must switch LP->HS before its HS payload) stalls
   * in the PHY send stage.  Compute the times from the D-PHY standard
   * timings and express them as a 13.16 fixed-point count of phy_hstx_clk
   * periods, as required by TRM 18.4.x.
   *
   * phy_hstx_clk here is the DSI-2 host's internal high-speed TX clock,
   * which is exactly 1/16 the lane HS data rate in DPHY mode.  It is NOT the
   * DCPHY physical HSTX_CLK_SEL domain (/2 below 1500 Mbps, /16 above);
   * that is a separate, PHY-internal clock used for its own timing counters.
   */

  {
    uint64_t hstx_clk;
    uint64_t period_ps;
    uint64_t ui_ps;
    uint64_t hs_prepare_ps;
    uint64_t hs_zero_ps;
    uint64_t lp2hs_ps;
    uint64_t hs_trail_ps;
    uint64_t hs_exit_ps;
    uint64_t hs2lp_ps;
    uint32_t lp2hs_time;
    uint32_t hs2lp_time;

    /* phy_hstx_clk = lane HS data rate / 16 (DPHY mode). */

    hstx_clk = (uint64_t)priv->cfg.hs_rate / 16u;
    period_ps = 1000000000000ULL / hstx_clk;                /* ps per cycle */
    ui_ps = 1000000000000ULL / (uint64_t)priv->cfg.hs_rate; /* 1 UI, ps */

    /* D-PHY standard times (in ps): TLPX=50ns; THS-PREPARE=40ns+4UI;
     * THS-ZERO=105ns+6UI; THS-TRAIL=max(60ns+4UI, 8UI); THS-EXIT=100ns.
     */

    hs_prepare_ps = 40000ULL + 4u * ui_ps;
    hs_zero_ps = 105000ULL + 6u * ui_ps;
    lp2hs_ps = 50000ULL + hs_prepare_ps + hs_zero_ps;

    {
      uint64_t trail_8ui = 8u * ui_ps;
      uint64_t trail_60 = 60000ULL + 4u * ui_ps;

      hs_trail_ps = (trail_8ui > trail_60) ? trail_8ui : trail_60;
    }

    hs_exit_ps = 100000ULL;
    hs2lp_ps = hs_trail_ps + hs_exit_ps;

    lp2hs_time = (uint32_t)((lp2hs_ps << 16) / period_ps);
    hs2lp_time = (uint32_t)((hs2lp_ps << 16) / period_ps);

    rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_LP2HS_MAN_CFG,
                      lp2hs_time & DSI2_PHY_LP2HS_TIME_MASK);
    rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_HS2LP_MAN_CFG,
                      hs2lp_time & DSI2_PHY_HS2LP_TIME_MASK);

    /* The remaining LP/Escape timing registers (MAX_RD_T / ESC_CMD_T /
     * ESC_BYTE_T) are left at their reset value 0 by the DCPHY power-up
     * sequence, but the clock lane's low-power (LP-11 / Escape) state
     * machine depends on them: with zero timing the clock lane cannot
     * complete the HS->LP-11 transition, so phy_clk_stopstate stays 0 and
     * phy_tx_ready FSM never leaves INIT (all-black video).
     *
     * Program them from the D-PHY standard temps, expressed as a 13.16
     * fixed-point count of phy_hstx_clk (= hs_rate/16) periods, matching
     * the LP2HS/HS2LP units above.  MAX_RD_T is a plain integer count
     * (no fractional bits).
     *
     *   esc_cmd:  one Escape-mode command  -> 20 ns + 4*TLPX(~50ns)
     *             conservatively ~ 200 ns.
     *   esc_byte: one LP data byte at the 10 Mbps-ish LP rate -> ~800 ns.
     *   max_rd:   time to receive a maximum-size response packet (+ EoTp) ->
     *             a few us; use a generous 32 phy_hstx_clk cycles.
     */

    {
      uint64_t esc_cmd_ps = 200000ULL;  /* ~200 ns per Escape command */
      uint64_t esc_byte_ps = 800000ULL; /* ~800 ns per LP byte */
      uint32_t esc_cmd_t;
      uint32_t esc_byte_t;

      esc_cmd_t = (uint32_t)((esc_cmd_ps << 16) / period_ps);
      esc_byte_t = (uint32_t)((esc_byte_ps << 16) / period_ps);

      rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_ESC_CMD_T_MAN_CFG,
                        esc_cmd_t & DSI2_PHY_LP2HS_TIME_MASK);
      rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_ESC_BYTE_T_MAN_CFG,
                        esc_byte_t & DSI2_PHY_LP2HS_TIME_MASK);

      /* MAX_RD_T: integer count of phy_hstx_clk cycles (26:0, no fraction).
       * Use a generous value to cover the largest DCS read + EoTp. */

      rk3576_dsi_putreg(priv->base, RK3576_DSI2_PHY_MAX_RD_T_MAN_CFG, 32u);
    }
  }
}

/****************************************************************************
 * Name: rk3576_dsi_rearm
 *
 * Description:
 *   Return the DSI-2 datapath to a known-clean state and put the host back
 *   into Command mode, WITHOUT touching the DCPHY (the PHY stays powered and
 *   locked; only the host-side data paths are reset).
 *
 *   Why this is required before every video entry, not just at probe time:
 *
 *   A video burst that starts and never finishes leaves `phy_txhs` holding
 *   data the PHY never accepts.  TRM 18.3.1.1 (Idle Mode) states that an
 *   operating-mode change is only accepted once "all the remaining packets
 *   from these sources are sent, and their respective FIFOs are empty";
 *   while the FIFO cannot drain, MODE_CTRL is simply ignored -- the host is
 *   wedged in Video mode and cannot even be asked to go back to Command
 *   mode.  Observed exactly that: MODE_STATUS stayed 3 after a
 *   (1000000 x 1 us) poll of MODE_CTRL=COMMAND.
 *
 *   The host is therefore reset on EVERY mode set: a SOFT_RESET pulse plus a
 *   PWR_UP down/up cycle, followed by a re-run of the PHY init and landing in
 *   Command mode.  Only the soft resets can flush a wedged FIFO and force
 *   all six debug FSMs back to INIT.
 *
 *   Re-asserting PHY_MODE_CFG and the whole link config here is deliberate
 *   belt-and-braces: those registers are cheap to rewrite and MUST be right
 *   whether or not the reset above cleared them.
 *
 * Input Parameters:
 *   priv - Driver state.  Caller must hold priv->lock.
 *
 ****************************************************************************/

static void rk3576_dsi_rearm(FAR struct rk3576_dsi_s *priv)
{
  uintptr_t base = priv->base;
  uint32_t phy_mode;
  uint32_t mode;

  /* 1. Pulse the three data-process soft resets (active low).  This is the
   *    only available watchdog against a stuck HS transmission: it flushes
   *    every FIFO and returns all FSMs to INIT.
   */

  rk3576_dsi_putreg(base, RK3576_DSI2_SOFT_RESET, 0x0);
  up_udelay(100);
  rk3576_dsi_putreg(base, RK3576_DSI2_SOFT_RESET,
                    DSI2_SOFT_RESET_SYS_RSTN | DSI2_SOFT_RESET_PHY_RSTN |
                        DSI2_SOFT_RESET_IPI_RSTN);

  /* 2. Cycle the core down and back up, which every mode set requires. */

  rk3576_dsi_putreg(base, RK3576_DSI2_PWR_UP, 0x0);
  up_udelay(100);
  rk3576_dsi_putreg(base, RK3576_DSI2_PWR_UP, DSI2_PWR_UP_PWR_UP);

  /* 3. Re-assert the PHY interface config + link config (clk_type, escape
   *    clock divider, both ratios, LP2HS/HS2LP, BTA/EoTp). */

  phy_mode = DSI2_PHY_MODE_PPI_WIDTH_16 |
             DSI2_PHY_MODE_PHY_LANES(priv->cfg.lanes) |
             DSI2_PHY_MODE_PHY_TYPE_DPHY;
  rk3576_dsi_putreg(base, RK3576_DSI2_PHY_MODE_CFG, phy_mode);

  rk3576_dsi_phy_link_cfg(priv);

  /* 4. Manual timing + Command mode.  Command mode is reached
   *    right after PWR_UP, and only then is Video mode asked for. */

  rk3576_dsi_putreg(base, RK3576_DSI2_MANUAL_MODE_CFG, DSI2_MANUAL_MODE_EN);

  mode = rk3576_dsi_set_mode(base, DSI2_MODE_COMMAND, RK3576_DSI_POLL_LOOPS);
  if (mode != DSI2_MODE_COMMAND)
    {
      syslog(LOG_WARNING,
             "dsi-rearm: MODE_STATUS=%u after COMMAND request (expected %u) "
             "-- the datapath may still be wedged\n",
             (unsigned)mode, (unsigned)DSI2_MODE_COMMAND);
    }

  priv->mode = RK3576_DSI_MODE_COMMAND;
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

  /* Obtain the controller clocks (registered by rk3576_clk_tree.c).
   *
   * NOTE: clk_dsihost0_sel's reset parent is clk_spll_mux (sel=0b010), but
   * clk_spll (and clk_vpll/clk_bpll/clk_lpll) are not yet registered in the
   * clock tree, so clk_get_rate() on clk_dsihost0 reads back 0 even though
   * the hardware clock is running.  A zero sys_clk breaks the CRI escape
   * clock derivation below (phy_lptx_clk_div).  Until those PLLs are
   * modelled, reparent the DSI sclk mux onto clk_gpll (1188 MHz), which IS
   * registered, so clk_get_rate() returns the true rate.
   */

  {
    struct clk_s *dsi_sel = clk_get("clk_dsihost0_sel");
    struct clk_s *gpll = clk_get("clk_gpll");

    if (dsi_sel != NULL && gpll != NULL)
      {
        ret = clk_set_parent(dsi_sel, gpll);
        if (ret < 0)
          {
            gerr("ERROR: DSI failed to reparent sclk onto gpll: %d\n", ret);
          }
      }
  }

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

  /* Keep the core held in reset while the PHY interface and DCPHY are
   * configured, then release the soft resets (active-low).  DSI2_PWR_UP is
   * kept at RESET until after the PHY is fully powered, and the soft resets
   * are deasserted before the PHY configuration.  Releasing SYS/PHY/IPI
   * resets while the core is powered but before the PHY is brought up was
   * found to leave the HS-TX datapath in a stuck state (phy_txhs FIFO
   * stopped, all FSMs at INIT, cri_busy never clears).
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_PWR_UP, 0x0);

  /* CRU-level APB/presetn reset for the DSI host.  The APB reset is asserted
   * then deasserted BEFORE the SOFT_RESET pulse; both live in
   * CRU_SOFTRST_CON64 (0x0B00):
   *   bit5 resetn_dsihost0  — functional reset ("when high, reset")
   *   bit4 presetn_dsihost0 — APB reset ("when high, reset")
   * Skipping this leaves the internal command/tx FSMs (sys_cmd, phy_tx_ready)
   * stuck at INIT: APB writes to the CRI registers are accepted and the FIFO
   * drains, but no state machine ever consumes the command, so the lanes
   * never leave stop-state.  Hiword-mask write: bit 16+N makes bit N writable.
   */

  {
    uintptr_t crurst = RK3576_CRU_ADDR + 0x0B00;
    uint32_t rstmask = (1u << 5) | (1u << 4);

    putreg32((rstmask << 16) | rstmask, crurst); /* assert */
    up_udelay(20);
    putreg32(rstmask << 16, crurst); /* deassert */
    up_udelay(20);
  }

  /* Pulse the soft resets low then release (active-low). */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_SOFT_RESET, 0x0);
  up_udelay(100);

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
   * The PPI width is fixed to 16 bits by the RK3576 DCPHY (TRM 21.6.4); a
   * mismatch here leaves the HS payload stranded in the phy_txhs FIFO.
   * The lane count comes from the caller's configuration.
   */

  phy_mode = DSI2_PHY_MODE_PPI_WIDTH_16 |
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

  /* Program the PHY-facing link options required by the CRI command path
   * (escape clock, lane clock mode, BTA/EoTp).  Must happen after the DCPHY
   * is up but before Command mode, so the first DCS init command can be
   * transmitted.
   */

  rk3576_dsi_phy_link_cfg(priv);

  /* Force manual timing mode.  In Command mode the controller ignores IPI
   * (there is no video frame), so the auto-calculation path can never
   * measure the PHY timings it needs; manual mode makes the controller use
   * the MAN_CFG registers (escape clock, LP2HS/HS2LP ratios) programmed
   * above instead.  Without this, a CRI command's HS payload stalls in the
   * phy_txhs FIFO and cri_busy never clears.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_MANUAL_MODE_CFG,
                    DSI2_MANUAL_MODE_EN);

  /* Power up the core.  This must happen AFTER the
   * PHY interface is configured and the DCPHY is powered/locked, so the
   * controller's internal clock domain crossing (sys -> hstx) is
   * established against a live, stable PHY clock.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_PWR_UP, DSI2_PWR_UP_PWR_UP);

  /* Timeout counters.  All DSI2_TIMEOUT_*_CFG counters reset to 0 (=
   * disabled), which is why INT_ST_TO stays 0 in a system that never enables
   * them.
   *
   * The HS-TX-READY timeout is ENABLED here: err_to_hstxrdy means "I asked the
   * PHY to transmit in high speed and it never became ready", which is a PPI
   * handshake failure no working link produces.  It is a genuine fault report.
   *
   * The HS-TX timeout is deliberately LEFT DISABLED (0).  Reason,
   * learned the hard way: the counter ticks on phy_lptx_clk (~19.8 MHz) and
   * the field is 16 bits, so the largest window it can express is 0xffff =
   * ~3.3 ms. A normal non-burst video stream on a link with only a few percent
   * of rate margin has NO time to leave high speed per line, so the controller
   * sends whole frames as one continuous burst (measured: one LP-11 -> HS
   * transition per FRAME): every frame therefore contains an HS burst far
   * longer than 3.3 ms and err_to_hstx latches once per frame on a stream that
   * is working exactly as designed.  Enabling it turned a normal condition
   * into what read like a hardware fault for several rounds of this bring-up.
   * Enable it deliberately when diagnosing a phantom "nothing is transmitted"
   * symptom, and interpret it as "an HS burst exceeded 3.3 ms", not as an
   * error.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_TIMEOUT_HSTX_CFG, 0);
  rk3576_dsi_putreg(priv->base, RK3576_DSI2_TIMEOUT_HSTXRDY_CFG, 0xffff);
  rk3576_dsi_putreg(priv->base, RK3576_DSI2_TIMEOUT_LPTXRDY_CFG, 0xffff);

  /* Unmask the timeout interrupt report bits (write 1 to unmask). */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_INT_MASK_TO,
                    (1u << 1)       /* err_to_hstxrdy */
                        | (1u << 0) /* err_to_hstx */
                        | (1u << 3) /* err_to_lptxrdy */);

  /* Program the TX virtual channel.  The CRI header carries a per-message
   * VC, and DSI2_DSI_VCID_CFG carries the fixed host channel; leaving it at
   * reset (0) can misroute the command. */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_DSI_VCID_CFG, 0x0);

  /* Do NOT enter AUTOCALC here.  Auto-Calculation mode "stops the data
   * reception from the IPI, CRI, and PRI interfaces" (TRM 18.3.1) while it
   * computes PHY timings, and in the manual-timing path there is no IPI
   * video frame to measure, so AUTOCALC never completes — MODE_STATUS stays
   * stuck at AUTOCALC (0x1) and every CRI command is silently dropped
   * (cri_busy clears instantly, the FIFO drains, but no state machine
   * consumes it and the lanes never leave stop-state).
   *
   * Command mode is reached directly, without any AUTOCALC pass.  The
   * Host<->PHY deskew handshake is
   * established by the CRU APB reset done above plus the DCPHY startup
   * sequence, not by AUTOCALC.
   */

  /* Enter Command mode and wait for MODE_STATUS to actually settle. */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_MODE_CTRL, DSI2_MODE_COMMAND);

  {
    uint32_t mode;
    int poll;

    for (poll = 0; poll < RK3576_DSI_POLL_LOOPS; poll++)
      {
        mode = rk3576_dsi_getreg(priv->base, RK3576_DSI2_MODE_STATUS) & 0x7;
        if (mode == DSI2_MODE_COMMAND)
          {
            break;
          }

        up_udelay(1);
      }

    if (poll == RK3576_DSI_POLL_LOOPS)
      {
        gerr("ERROR: DSI failed to enter Command mode (MODE_STATUS=0x%x)\n",
             (unsigned)mode);
      }
  }

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
 *   The DSI-2 controller measures every IPI horizontal-timing parameter
 *   (HSA/HBP/HACT/HLINE) in cycles of *phy_hstx_clk*, where phy_hstx_clk is
 *   exactly 1/16 the lane high-speed data rate in DPHY mode.
 *
 *   A horizontal interval of `pixels` pixels spans the time
 *   `pixels / pixel_clock` seconds, during which phy_hstx_clk ticks
 *   `pixels * phy_hstx_clk / pixel_clock` cycles:
 *
 *     time = pixels * (hs_rate / 16) / pixel_clock    (<< 16 fixed-point)
 *
 *   The previous implementation computed `pixels * bpp / lanes` (serial bit
 *   clocks) instead, which is off by the (bpp / 16) factor — a 16x error at
 *   bpp=24 — corrupting every horizontal blanking boundary so the video
 *   state machine can never lock onto a line.
 *
 *   Returns UINT32_MAX if the integral part would overflow the 13-bit field
 *   (>= 8192 cycles) — callers must treat that as a timing-programming
 *   error.
 ****************************************************************************/

static uint32_t rk3576_dsi_hstx_cycles(uint32_t pixels, uint32_t hs_rate,
                                       uint32_t pixel_clock)
{
  uint64_t phy_hstx_clk;
  uint64_t cycles;

  if (pixel_clock == 0)
    {
      return UINT32_MAX;
    }

  phy_hstx_clk = (uint64_t)hs_rate / 16u;

  /* cycles = pixels * phy_hstx_clk / pixel_clock, as a 13.16 fixed-point
   * value.  Compute with the << 16 applied before the divide so the (often
   * fractional) ratio is not truncated to zero.  pixels <= ~8192, hs_rate
   * <= a few GHz, so the product stays well within uint64.
   *
   * Round to NEAREST.  Plain truncation biases every horizontal interval low
   * by up to
   * one phy_hstx_clk cycle, which shifts the blanking boundaries the video
   * FSM locks onto; at 24 MHz that is ~42 ns per interval, i.e. a
   * measurable fraction of the line's blanking window.
   */

  cycles = (((uint64_t)pixels * phy_hstx_clk * 65536u) + (pixel_clock / 2u)) /
           pixel_clock;

  /* The result carries the << 16 fixed-point shift already.  The register
   * has 13 integral bits ([29:16]); reject any value whose integral part
   * would overflow them (>= 2^13 -> fixed-point >= 2^29).
   */

  if ((cycles >> 16) >= (1u << 13))
    {
      gerr("ERROR: DSI horizontal time overflow (%llu cycles)\n",
           (unsigned long long)(cycles >> 16));
      return UINT32_MAX;
    }

  return (uint32_t)cycles;
}

/****************************************************************************
 * Name: rk3576_dsi_program_ipi_h_timing
 *
 * Description:
 *   Program the IPI horizontal timing registers (HSA/HBP/HACT/HLINE) and
 *   PHY_IPI_RATIO from the given pixel clock.
 *
 *   Both must be derived from the SAME, ACTUAL pixel clock: the horizontal
 *   registers are phy_hstx_clk cycle counts of the video-timing intervals
 *   (interval = pixels / pixel_clock), while PHY_IPI_RATIO is the
 *   fixed-point ratio (hs_rate/16) / ipi_clk with ipi_clk = pixel_clock/4
 *   (RK3576 VOP 4:1 pixel-shift -- see the caller's ratio comment).
 *
 *   Programming them from different numbers silently breaks the CDC
 *   handshake between the IPI (pixel) and PHY-HSTX clock domains, because
 *   the controller compares the incoming pixel stream against these
 *   registers to decide when to emit a video packet.
 *
 *   Called twice in the driver's life:
 *     1. enable_video(): with the board's nominal pixel_clock, so the IPI
 *        is fully programmed before entering Video mode.
 *     2. rk3576_mipi_dsi_update_pixel_clock(): with the VOP's REAL dclk
 *        rate once the CRU divider chain has settled, correcting the
 *        nominal-vs-actual mismatch (e.g. gpll 1188M/19 = 62.526 MHz
 *        instead of the requested 64 MHz).
 *
 *   Input Parameters:
 *     priv        - Driver state (locked by caller).
 *     timing      - Panel timing (pixel counts / line counts).
 *     pixel_clock - ACTUAL pixel clock in Hz (must be non-zero).
 *
 *   Returned Value:
 *     OK on success; -EINVAL if a horizontal interval overflows the 13-bit
 *     fixed-point integral field.
 *
 ****************************************************************************/

static int rk3576_dsi_program_ipi_h_timing(
    FAR struct rk3576_dsi_s *priv,
    FAR const struct rk3576_dsi_video_timing *timing, uint32_t pixel_clock)
{
  uintptr_t base = priv->base;
  uint32_t htotal;
  uint32_t regval;

  if (pixel_clock == 0)
    {
      return -EINVAL;
    }

  htotal = timing->hactive + timing->hsync_len + timing->hback_porch +
           timing->hfront_porch;

  regval = rk3576_dsi_hstx_cycles(timing->hsync_len, priv->cfg.hs_rate,
                                  pixel_clock);
  if (regval == UINT32_MAX)
    {
      return -EINVAL;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HSA_MAN_CFG, regval);

  regval = rk3576_dsi_hstx_cycles(timing->hback_porch, priv->cfg.hs_rate,
                                  pixel_clock);
  if (regval == UINT32_MAX)
    {
      return -EINVAL;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HBP_MAN_CFG, regval);

  regval =
      rk3576_dsi_hstx_cycles(timing->hactive, priv->cfg.hs_rate, pixel_clock);
  if (regval == UINT32_MAX)
    {
      return -EINVAL;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HACT_MAN_CFG, regval);

  regval = rk3576_dsi_hstx_cycles(htotal, priv->cfg.hs_rate, pixel_clock);
  if (regval == UINT32_MAX)
    {
      return -EINVAL;
    }

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_HLINE_MAN_CFG, regval);

  /* PHY_IPI_RATIO = (hs_rate / 16) / (pixel_clock / 4).  Rounded to nearest:
   * this value is the
   * only thing telling the controller how many phy_hstx_clk cycles one IPI
   * clock period is worth, so a systematic downward bias makes the IPI and
   * PHY domains drift apart across a line.
   */

  {
    uint64_t phy_hstx_clk = (uint64_t)priv->cfg.hs_rate / 16u;
    uint64_t ipi_clk = (uint64_t)pixel_clock / 4u;

    if (ipi_clk != 0)
      {
        uint64_t ipi_ratio = ((phy_hstx_clk << 16) + (ipi_clk / 2u)) / ipi_clk;
        rk3576_dsi_putreg(base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG,
                          (uint32_t)ipi_ratio & DSI2_PHY_IPI_RATIO_MASK);
      }
  }

  return OK;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_update_pixel_clock
 *
 * Description:
 *   Re-derive the IPI horizontal timing and PHY_IPI_RATIO from the VOP's
 *   REAL pixel clock once the CRU divider chain has settled.
 *
 *   The board passes a pixel clock to enable_video(), but clk_set_rate() on
 *   dclk_vp0 can only pick an integer divider from its parent PLL, and the
 *   divider search keeps the largest divisor whose output is still <= the
 *   request, so the achieved rate can be several percent BELOW the request
 *   (requesting 62 MHz from gpll 1188 MHz lands on 1188/20 = 59.4 MHz, not
 *   the 1188/19 = 62.526 MHz the older logs show -- that measurement was
 *   taken while the board still requested 64 MHz).  Programming the DSI from
 *   the nominal value then leaves the controller comparing the incoming pixel
 *   stream against a timing/ratio that does not match the real clock, so it
 *   judges "the line ended" at the wrong instant and mis-sizes the
 *   IPI<->PHY CDC ratio.
 *
 *   The board's fix is to request a value the CRU hits EXACTLY (see
 *   KICKPI_K7_MIPI_DSI_PIXCLK); rk3576_vop_enable_clocks() additionally logs
 *   a warning whenever the achieved rate still differs from the request.
 *   This function is the escape hatch for the case where an exact request is
 *   not available.
 *
 *   NOT wired to rk3576_vop_enable_clocks() any more -- doing that rewrote the
 *   horizontal timing and the CDC ratio of a LIVE pixel datapath
 *asynchronously (a race, see the long note in that function).  Call it before
 *the host enters Video mode if it is ever needed again.
 *
 *   Safe to call at any time once the host is in Video mode; the IPI timing
 *   registers are plain RW and (manual mode) take effect without leaving
 *   Video mode.
 *
 *   Input Parameters:
 *     pixel_clock_hz - Actual VOP pixel clock (crtc clock) in Hz.
 *
 *   Returned Value:
 *     OK on success; -EPERM if the host is not in Video mode; -EINVAL on a
 *     bad clock or timing overflow.
 *
 ****************************************************************************/

int rk3576_mipi_dsi_update_pixel_clock(uint32_t pixel_clock_hz)
{
  FAR struct rk3576_dsi_s *priv = &g_dsi;
  uint32_t nominal;
  int ret;

  if (!priv->initialized || priv->mode != RK3576_DSI_MODE_VIDEO ||
      !priv->timing_valid)
    {
      return -EPERM;
    }

  if (pixel_clock_hz == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  nominal = priv->timing.pixel_clock;
  priv->timing.pixel_clock = pixel_clock_hz;

  ret = rk3576_dsi_program_ipi_h_timing(priv, &priv->timing, pixel_clock_hz);

  syslog(LOG_INFO,
         "dsi: pixel clock synced %u -> %u Hz (ipi=%u Hz) -> HSA=%08x "
         "HBP=%08x HACT=%08x HLINE=%08x PHY_IPI_RATIO=%08x\n",
         (unsigned)nominal, (unsigned)pixel_clock_hz,
         (unsigned)(pixel_clock_hz / 4u),
         rk3576_dsi_getreg(priv->base, RK3576_DSI2_IPI_VID_HSA_MAN_CFG),
         rk3576_dsi_getreg(priv->base, RK3576_DSI2_IPI_VID_HBP_MAN_CFG),
         rk3576_dsi_getreg(priv->base, RK3576_DSI2_IPI_VID_HACT_MAN_CFG),
         rk3576_dsi_getreg(priv->base, RK3576_DSI2_IPI_VID_HLINE_MAN_CFG),
         rk3576_dsi_getreg(priv->base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG));

  nxmutex_unlock(&priv->lock);
  return ret;
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

  /* Video mode is entered from Command mode, at which point the DCPHY is
   * already powered (brought up by initialize()).  This keeps the PHY
   * lifecycle inside the DSI driver and decouples it from the video timing
   * programming -- the DCS init sequence runs earlier, in Command mode, over
   * the same power link.
   *
   * Re-entering from VIDEO is also accepted, because the rearm below brings
   * the datapath back to Command mode unconditionally: refusing here would
   * make a wedged session (which IS stuck in VIDEO, see the rearm's comment)
   * permanently unrecoverable, and re-enable is exactly what a caller does
   * to recover.
   */

  if (priv->mode != RK3576_DSI_MODE_COMMAND &&
      priv->mode != RK3576_DSI_MODE_VIDEO)
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

  /* Remember the timing: the horizontal IPI programming below is redone
   * later with the VOP's REAL pixel clock once the CRU divider chain has
   * settled (see rk3576_mipi_dsi_update_pixel_clock()).
   */

  memcpy(&priv->timing, timing, sizeof(priv->timing));
  priv->timing_valid = true;

  base = priv->base;

  /* Reset the host-side datapath and return to Command mode before
   * re-programming for video.  This is what recovers a wedged previous video
   * session: once an
   * HS burst has started and never finished, the FIFOs never drain and
   * TRM 18.3.1.1 makes the controller ignore any mode change -- so without
   * this the host would stay locked in the previous Video state and every
   * register written below would be applied to a wedged datapath.
   */

  rk3576_dsi_rearm(priv);

  /* Configure the IPI color depth and pixel format. */

  color = rk3576_dsi_color_depth(priv->cfg.format) | DSI2_IPI_COLOR_FORMAT_RGB;
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_COLOR_MAN_CFG, color);

  /* Program the matching IPI color depth in the VO0 GRF.  The DSI-2 IPI
   * pixel interface is gated/configured through VO0_GRF_SOC_CON10
   * (IPI_COLOR_DEPTH bits [11:8]): without it the VOP's pixel stream is
   * sampled at the wrong width and the video path stalls even though the
   * controller-side IPI_COLOR_MAN_CFG above is correct and the DCS command
   * link (CRI) is fully up.
   */

  {
    uint32_t grf_depth = 0;

    switch (rk3576_dsi_color_depth(priv->cfg.format))
      {
        case DSI2_IPI_COLOR_DEPTH_565:
          grf_depth = RK3576_VO0_GRF_IPI_DEPTH_565;
          break;

        case DSI2_IPI_COLOR_DEPTH_6:
          grf_depth = RK3576_VO0_GRF_IPI_DEPTH_6;
          break;

        case DSI2_IPI_COLOR_DEPTH_8:
        default:
          grf_depth = RK3576_VO0_GRF_IPI_DEPTH_8;
          break;
      }

    putreg32(RK3576_GRF_HWM(RK3576_VO0_GRF_IPI_DEPTH_MASK) | grf_depth,
             RK3576_VO0_GRF_ADDR + RK3576_VO0_GRF_SOC_CON10_OFF);
  }

  /* Program the horizontal timing (fixed-point phy_hstx_clk cycles) and
   * PHY_IPI_RATIO from the board's nominal pixel clock.  64 MHz is only a
   * request: the CRU divider chain will land on the nearest achievable
   * rate, so this is corrected later by
   * rk3576_mipi_dsi_update_pixel_clock() with the real dclk rate.  The
   * ratio must always be derived from the same clock as the horizontal
   * registers (both are done inside the helper).
   *
   * CRITICAL (RK3576 VOP 4:1 pixel-shift): PHY_IPI_RATIO's denominator is
   * NOT the panel pixel clock but the IPI clock = dclk_core = crtc_clock/4:
   *
   *   (Video Timing Pixel Rate) / 4 = MIPI Pixel Clock = dclk_out = dclk_core
   *
   * Using the raw pixel_clock (a 4x error) leaves the CDC handshake
   * between the IPI (pixel) and PHY-HSTX domains permanently misaligned:
   * the video stream stalls at the clock-domain crossing (IPI_BUSY never
   * sets) while the CRI command path -- which does not depend on this
   * ratio -- keeps working.  Correct value for kickpi-k7 (hs_rate=384M,
   * pixel=64M):  (384M/16) / (64M/4) = 24M / 16M = 1.5 = 0x18000
   * (an error of 0x6000 = 0.375 = 24M/64M is the old broken value).
   */

  ret = rk3576_dsi_program_ipi_h_timing(priv, timing, timing->pixel_clock);
  if (ret < 0)
    {
      gerr("ERROR: DSI IPI horizontal timing overflow\n");
      goto errout_unlock;
    }

  /* program_ipi_h_timing() also wrote PHY_IPI_RATIO (same clock source).
   * PHY_SYS_RATIO uses the same numerator over sys_clk, so it is done
   * separately here. */

  {
    uint64_t phy_hstx_clk = (uint64_t)priv->cfg.hs_rate / 16u;

    if (priv->sclk != NULL)
      {
        uint32_t sclk_rate = clk_get_rate(priv->sclk);

        if (sclk_rate != 0)
          {
            uint64_t sys_ratio = (phy_hstx_clk << 16) / sclk_rate;
            rk3576_dsi_putreg(base, RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG,
                              (uint32_t)sys_ratio & DSI2_PHY_SYS_RATIO_MASK);
          }
      }
  }

  /* Program the vertical timing (in lines). */

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VSA_MAN_CFG,
                    timing->vsync_len & DSI2_IPI_VSA_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VBP_MAN_CFG,
                    timing->vback_porch & DSI2_IPI_VBP_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VACT_MAN_CFG,
                    timing->vactive & DSI2_IPI_VACT_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VFP_MAN_CFG,
                    timing->vfront_porch & DSI2_IPI_VFP_LINES_MASK);

  /* Pixels per video packet -- HACTIVE.
   *
   * This is a REVERT of our own change (0), and the reason is worth keeping:
   * the TRM (18.4.3) says max_pix_pkt is "used in Video mode (for non-burst
   * modes) and Data Stream mode", that "a value of 0 or bigger than the HACT
   * pixels will originate one single video packet per line", and (18.2.1)
   * that "the minimum value for the max_pix_pkt field should be 48 pixels".
   * We read the first sentence as "0 means one packet per line" and wrote 0
   * everywhere.
   *
   * A size limit of 0 is the ambiguous way to say the same thing and may
   * easily mean "no limit" to the hardware, in which case the packet is
   * sized by the LINE instead of the ACTIVE WINDOW -- 780 pixels instead of
   * 720 on this panel.  That is not a cosmetic difference: 60 extra pixels
   * per line make the panel's pixel counter drift 60 pixels per line, the
   * picture shears diagonally, and the image stays wrong no matter what is
   * painted into the framebuffer (each frame it re-syncs at the frame start,
   * so the shear is static).  The scope evidence points that way: the data
   * lane is in HS for ~89% of a 13.4 us line, whereas a 720-pixel packet
   * plus the blanking should leave HS at ~80% and LP at ~20% (~2.7 us).
   *
   * So HACTIVE is written, and it is also what the other two
   * paths in THIS file already write (the FSM sweep and the vertical-timing
   * refresh), which means the running configuration was the odd one out.
   */

  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_PIX_PKT_CFG,
                    (uint32_t)timing->hactive & DSI2_IPI_PIX_PKT_MAX_MASK);

  /* Configure the video-mode transmission type.
   *
   * vid_mode_type (TRM 18.3.1.2): 0 = non-burst with sync pulses,
   * 1 = non-burst with sync events, 2 = burst.  Every blk_*_hs_en bit is
   * left 0 so the controller is allowed to return to low power in each
   * blanking region, which a NON-continuous clock lane requires.
   *
   * The mode is a BOARD property (like the porches), but it is logged here
   * together with the clock-lane type: a "still black" report is only
   * interpretable when the log states which of the three video modes and
   * which clock-lane behaviour the firmware actually programmed, instead of
   * leaving the reader to guess from a source tree that may have moved on.
   */

  regval = (uint32_t)priv->cfg.video_mode & DSI2_VID_TX_VID_MODE_TYPE_MASK;
  rk3576_dsi_putreg(base, RK3576_DSI2_DSI_VID_TX_CFG, regval);

  syslog(LOG_INFO,
         "dsi: VIDEO CFG mode_type=%u (%s) continuous_clk=%u eotp=%u lanes=%u "
         "bpp=%u pixel_clock=%u hs_rate=%u\n",
         (unsigned)regval,
         regval == 0   ? "non-burst/sync-pulses"
         : regval == 1 ? "non-burst/sync-events"
                       : "burst",
         (unsigned)(priv->cfg.continuous_clk ? 1 : 0),
         (unsigned)(priv->cfg.eotp ? 1 : 0), (unsigned)lanes, (unsigned)bpp,
         (unsigned)timing->pixel_clock, (unsigned)priv->cfg.hs_rate);

  /* Use manual timing (the MAN_CFG timing registers programmed above). */

  rk3576_dsi_putreg(base, RK3576_DSI2_MANUAL_MODE_CFG, DSI2_MANUAL_MODE_EN);

  /* Switch the host into Video mode.  The DCPHY is already powered (it was
   * brought up when the host entered Command mode), so no PHY programming
   * happens here.
   */

  rk3576_dsi_putreg(base, RK3576_DSI2_MODE_CTRL, DSI2_MODE_VIDEO);
  priv->mode = RK3576_DSI_MODE_VIDEO;

  /* Poll MODE_STATUS until the state machine actually settles into Video
   * mode.  Unlike Command-mode entry (which only emits an error), a failure
   * here means the pixel stream will never be consumed: the VOP keeps
   * scanning but the DSI-2 IPI state machine stays in a prior mode, so the
   * panel stays black while the DCS command link remains functional.
   */

  {
    uint32_t mode;
    int poll;

    for (poll = 0; poll < RK3576_DSI_POLL_LOOPS; poll++)
      {
        mode = rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 0x7;
        if (mode == DSI2_MODE_VIDEO)
          {
            break;
          }

        up_udelay(1);
      }

    syslog(LOG_INFO, "dsi: MODE_STATUS=%u (expect VIDEO=%u) after %d polls\n",
           (unsigned)(rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 0x7),
           (unsigned)DSI2_MODE_VIDEO, poll);

    if (poll == RK3576_DSI_POLL_LOOPS)
      {
        gerr("ERROR: DSI failed to enter Video mode (MODE_STATUS=0x%x)\n",
             (unsigned)mode);
      }
  }

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_dsi_set_mode
 ****************************************************************************/

static uint32_t rk3576_dsi_set_mode(uintptr_t base, uint32_t mode, int loops)
{
  rk3576_dsi_putreg(base, RK3576_DSI2_MODE_CTRL, mode);

  while (loops-- > 0)
    {
      if ((rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 0x7u) == mode)
        {
          break;
        }

      up_udelay(1);
    }

  return rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 0x7u;
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
   *
   * A plain MODE_CTRL write is NOT sufficient on its own, and assuming it was
   * is what made the panel unreadable in an earlier round.  TRM 18.3.1.1 only
   * accepts an operating-mode change once every in-flight packet has been
   * sent AND the corresponding FIFOs are empty; while the video transmitter
   * still has packets queued the request is ignored, the host silently stays
   * in Video mode, and the CRI command path stays unusable -- so every
   * subsequent panel read fails and reports 0xff.  (Observed: the caller
   * logged "wait cri idle timeout: CORE_STATUS=0x00010103" for every single
   * register read after the transition, i.e. core_busy + core_fifos_not_empty
   * + cri_busy + cri_fifos_not_empty all still set.)
   *
   * So: request the transition, POLL for it to actually happen, and fall back
   * to the soft-reset sequence (rk3576_dsi_rearm()) if it does not -- that is
   * the only mechanism that can flush a wedged FIFO.  The fallback is
   * deliberately conservative: on a healthy switch the poll succeeds in a few
   * microseconds and nothing is reset.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_MODE_CTRL, DSI2_MODE_COMMAND);

  {
    /* Bits that must clear for the controller to be genuinely idle: the
     * packet builder and IPI (video side) plus the core FIFOs.  CRI/PRI bits
     * are reported but not required -- a pending read reply must not keep the
     * mode change hostage. */

    const uint32_t idle_mask =
        DSI2_CORE_STATUS_CORE_BUSY | DSI2_CORE_STATUS_CORE_FIFOS_NOT_EMPTY |
        DSI2_CORE_STATUS_IPI_BUSY | DSI2_CORE_STATUS_IPI_FIFOS_NOT_EMPTY;

    uint32_t mode_status = 0;
    uint32_t core_status = 0;
    int poll;

    for (poll = 0; poll < RK3576_DSI_MODE_SWITCH_LOOPS; poll++)
      {
        core_status = rk3576_dsi_getreg(priv->base, RK3576_DSI2_CORE_STATUS);
        mode_status =
            rk3576_dsi_getreg(priv->base, RK3576_DSI2_MODE_STATUS) & 0x7u;

        if (mode_status == DSI2_MODE_COMMAND && (core_status & idle_mask) == 0)
          {
            break;
          }

        up_udelay(1);
      }

    if (poll == RK3576_DSI_MODE_SWITCH_LOOPS)
      {
        /* The IDLE condition was never met, and the useful half of that
         * statement is which part failed.  MODE_STATUS is reported first
         * because it is routinely ALREADY Command: the mode change is
         * accepted immediately, while the IPI-side busy bits keep the rest
         * of this poll from completing (in Command mode the IPI FIFOs can
         * legitimately still hold the pixels video mode left behind).
         * Saying "refused" in that case is simply wrong and sends the reader
         * looking for a mode problem that does not exist. */

        if (mode_status == DSI2_MODE_COMMAND)
          {
            syslog(LOG_WARNING,
                   "dsi: in Command mode, but the datapath still reports "
                   "CORE_STATUS=%08x after %d us (IPI FIFOs not drained); "
                   "rearming so the CRI is usable\n",
                   core_status, RK3576_DSI_MODE_SWITCH_LOOPS);
          }
        else
          {
            syslog(LOG_WARNING,
                   "dsi: Command mode not reached (MODE_STATUS=%u "
                   "CORE_STATUS=%08x after %d us) -- the IPI is not "
                   "draining (TRM 18.3.1.1); rearming the datapath\n",
                   (unsigned)mode_status, core_status,
                   RK3576_DSI_MODE_SWITCH_LOOPS);
          }

        rk3576_dsi_rearm(priv);

        mode_status =
            rk3576_dsi_getreg(priv->base, RK3576_DSI2_MODE_STATUS) & 0x7u;

        if (mode_status != DSI2_MODE_COMMAND)
          {
            syslog(LOG_ERR,
                   "dsi: still not in Command mode after the soft reset "
                   "(MODE_STATUS=%u) -- the CRI will not work\n",
                   (unsigned)mode_status);
            ret = -EBUSY;
            goto errout_unlock;
          }
      }
  }

  priv->mode = RK3576_DSI_MODE_COMMAND;

  nxmutex_unlock(&priv->lock);
  return OK;

errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
}

#endif /* CONFIG_RK3576_MIPI_DSI */
