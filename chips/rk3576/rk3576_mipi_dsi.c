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

/* Samples for the fine lane-duty measurement in rk3576_mipi_dsi_get_health().
 *
 * Why a second, finer pass exists: the coarse pass samples the PHY lanes 16
 * times ~34 us apart, which is nearly a whole number of line periods (13.13
 * us), so those samples can land on the SAME phase of every line and then the
 * reported duty describes the sampling grid instead of the link.  That is not
 * hypothetical: byte-identical configurations have reported 78% in one window
 * and 100% in another.
 *
 * This pass varies the delay instead (3 us + a 19-step cycle), so the samples
 * spread over the line.
 *
 * WHAT IT MEASURES, stated precisely because an earlier version of this
 * comment over-claimed: the fraction of samples in which the data lanes are
 * OUT OF LP-11.  That is NOT the same as "in HS" -- stopstate 0 also covers
 * LP-00/LP-01/LP-10 and the entry/exit transitions -- so the value cannot be
 * used to size the transmitted packet (predicting 80% for a 720-pixel packet
 * and 88% for a whole line was wrong for exactly this reason; the scope and
 * this probe agreed at ~89% while the packet size was demonstrably NOT the
 * deciding factor).  What it IS good for, and why it stays: it is a stable,
 * reproducible discriminator between transmission modes on identical
 * hardware -- measured 88-93% for burst and 98% for every non-burst mode --
 * and an abrupt change in it within one boot means the link behaviour
 * changed. */

#define RK3576_DSI_DUTY_SAMPLES (512)

/* Bring-up diagnostic: sweep every (video transmission mode x clock lane
 * type) combination at dump time and report which one actually lets the PHY
 * lanes cycle back to LP-11.  THIS IS THROWAWAY PROBE CODE -- set to 0 (or
 * delete it) once the video link works.  See
 * rk3576_dsi_video_mode_sweep() for the rationale.
 *
 * *** DISABLED -- AND LEAVING IT ENABLED BREAKS A WORKING PANEL ***
 *
 * The sweep runs from the tail of rk3576_mipi_dsi_dump_video_status(), i.e.
 * during bring-up while the panel is ALREADY displaying the real video
 * stream.  It then rearms the host and drives eight deliberately-invalid
 * configurations into the panel before restoring the board settings.
 *
 * Observed on the kickpi-k7 panel (matching this exactly):
 *   - the panel DID light on the very first boot of the corrected firmware,
 *     showing striping/artefacts, then froze and decayed (darker corners --
 *     an LCD that stops being refreshed), and
 *   - neither a reset nor a power cycle could ever reproduce it again.
 *
 * That is precisely what this probe does: it feeds the panel garbage video
 * (the stripes), and it ends on a rearm() whose restore races with the still
 * running VOP, so whether the real stream survives is a coin flip -- which is
 * why it worked exactly once and never again.  Its non-reproducibility is a
 * property of the probe, not of the panel.
 *
 * Historical note: the sweep is what produced the "BURST is the only mode
 * that transmits" conclusion.  That measurement was taken with the WRONG
 * panel geometry (htotal = 823, not 4-pixel aligned, see the board file), so
 * the conclusion does not transfer to the corrected 780/1320 timing.  Re-run
 * the sweep deliberately (with the backlight off and never on a panel that
 * is already displaying) if a mode comparison is needed again. */

#define RK3576_DSI_VIDEO_MODE_SWEEP 0

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
  bool timing_valid;            /* timing above has been programmed */

  /* Control experiment: does the phy_tx_ready debug FSM move AT ALL while
   * the controller is transmitting successfully (Command-mode DCS path)?
   * If it does not, then "phy_tx_ready stuck at INIT" in video mode -- a
   * conclusion that has steered several rounds -- is an artifact of the
   * debug field, not a fault.  See rk3576_mipi_dsi_get_cmd_probe().
   */

  uint32_t cmd_probe_cmds;         /* CRI commands observed */
  uint32_t cmd_probe_txr_max;      /* highest current_state seen */
  uint32_t cmd_probe_txr_prev_max; /* highest previous_state seen */
  bool cmd_probe_txr_noninit;      /* caught outside INIT */
  bool cmd_probe_txr_prev_noninit; /* previous_state was ever non-zero */
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
static uint32_t rk3576_dsi_obs_fsm(uintptr_t base, uint32_t sel,
                                   FAR bool *unstable);
static void rk3576_dsi_cmd_probe(FAR struct rk3576_dsi_s *priv);

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
 *   packet.  This "payload before header" order matches the Rockchip
 *   reference driver (dw-mipi-dsi2-rockchip.c): the CRI consumes payload
 *   already staged in the FIFO once the header is written.
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

  /* Control experiment: watch the phy_tx_ready FSM while a packet that the
   * panel demonstrably receives is in flight.  See the description of
   * rk3576_mipi_dsi_get_cmd_probe(). */

  rk3576_dsi_cmd_probe(priv);

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

  rk3576_dsi_cmd_probe(priv);

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

  /* Probe: lock down the actual sys_clk (clk_dsihost0) rate.  Conclusion
   * from the bring-up rounds: sclk = 396 MHz is CORRECT, not a bug.  TRM
   * CLKSEL_CON151 resets clk_dsihost0_sel to 0b010 (clk_spll_mux) with
   * clk_dsihost0_div = 0x02 (/3), i.e. spll(1188M)/3 = 396 MHz -- the
   * Rockchip default sys_clk.  Reparenting to gpll would NOT raise it
   * (gpll and spll are both 1188 MHz) because the /3 divider remains.
   *
   * With sclk = 396 MHz: esc_div = ceil(396M/40M) = 10, phy_lptx_clk =
   * 396M/(2*10) = 19.8 MHz <= 20 MHz -> correct.  (The earlier
   * "expect esc_div=30 / ~400 MHz" note was based on a wrong assumption
   * and is superseded.) */

  syslog(LOG_INFO,
         "dsi-probe: sclk_rate=%u esc_div=%u "
         "(sclk=spll/3=396M is the correct TRM default; "
         "phy_lptx_clk=%u Hz, limit 20M)\n",
         (unsigned)sclk_rate, (unsigned)esc_div,
         (unsigned)(sclk_rate / (2u * esc_div)));

  /* clk_type for the panel.  ILI9881D (ILI9881D_spec.txt) is a
   * NON-continuous clock lane: Table 46 defines THS-EXIT as "time to drive
   * LP-11 after HS burst", i.e. the clock lane returns to LP-11 (LPM)
   * after every HS burst (Figure 5: HSCM => HS-0 => LP-11).  The reference
   * driver (dw-mipi-dsi2-rockchip.c) also forces non-continuous before the
   * initial deskew calibration.  A continuous clock lane leaves the lane in
   * HS (never dropping to LP-11), which on this IP stalls the Host↔PHY
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

  /* BTA (Bus Turnaround) for DCS reads, plus EoTp TX -- matching the reference
   * driver, which programs BTA_EN | EOTP_TX_EN and clears EOTP_TX_EN only for
   * panels that explicitly declare MIPI_DSI_MODE_NO_EOT_PACKET.
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
   * which is exactly 1/16 the lane HS data rate in DPHY mode (per the
   * reference driver dw_mipi_dsi2_lp2hs_or_hs2lp_cfg).  It is NOT the
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
 *   The reference driver (dw_mipi_dsi2_mode_set()) therefore resets the host
 *   on EVERY mode set: dw_mipi_dsi2_host_softrst() (SOFT_RESET pulse) plus a
 *   PWR_UP down/up cycle, followed by re-running phy_init() and landing in
 *   Command mode.  Only the soft resets can flush a wedged FIFO and force
 *   all six debug FSMs back to INIT.
 *
 *   Re-asserting PHY_MODE_CFG and the whole link config here is deliberate
 *   belt-and-braces: those registers are cheap to rewrite and MUST be right
 *   whether or not the reset above cleared them (the reference re-runs
 *   phy_init() for the same reason).
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

  /* 2. Cycle the core down and back up, as the reference does around every
   *    mode set. */

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

  /* 4. Manual timing + Command mode.  The reference reaches Command mode
   *    right after PWR_UP and only then asks for Video mode. */

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
   * configured, then release the soft resets (active-low).  This matches
   * the Rockchip reference driver (dw-mipi-dsi2.c): DSI2_PWR_UP is kept at
   * RESET until after the PHY is fully powered, and the soft-resets are
   * deasserted before the PHY configuration.  Releasing SYS/PHY/IPI resets
   * while the core is powered but before the PHY is brought up was found to
   * leave the HS-TX datapath in a stuck state (phy_txhs FIFO stopped, all
   * FSMs at INIT, cri_busy never clears).
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_PWR_UP, 0x0);

  /* CRU-level APB/presetn reset for the DSI host.  The reference driver
   * (dw_mipi_dsi2_host_softrst) asserts then deasserts its APB reset BEFORE
   * the SOFT_RESET pulse; both live in CRU_SOFTRST_CON64 (0x0B00):
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

  /* Pulse the soft resets low then release (active-low, same as the
   * reference driver's dw_mipi_dsi2_host_softrst). */

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
   * The PPI width is fixed to 16 bits by the RK3576 DCPHY (see the
   * Rockchip reference driver dw-mipi-dsi2-rockchip.c and TRM 21.6.4); a
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

  /* Power up the core.  Per the reference driver this must happen AFTER the
   * PHY interface is configured and the DCPHY is powered/locked, so the
   * controller's internal clock domain crossing (sys -> hstx) is
   * established against a live, stable PHY clock.
   */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_PWR_UP, DSI2_PWR_UP_PWR_UP);

  /* Timeout counters.  All DSI2_TIMEOUT_*_CFG counters reset to 0 (= disabled),
   * which is why INT_ST_TO stays 0 in a system that never enables them.
   *
   * The HS-TX-READY timeout is ENABLED here: err_to_hstxrdy means "I asked the
   * PHY to transmit in high speed and it never became ready", which is a PPI
   * handshake failure no working link produces.  It is a genuine fault report.
   *
   * The HS-TX timeout is deliberately LEFT DISABLED (0), matching the driver
   * this one is derived from, which never writes this register at all.  Reason,
   * learned the hard way: the counter ticks on phy_lptx_clk (~19.8 MHz) and the
   * field is 16 bits, so the largest window it can express is 0xffff = ~3.3 ms.
   * A normal non-burst video stream on a link with only a few percent of rate
   * margin has NO time to leave high speed per line, so the controller sends
   * whole frames as one continuous burst (measured: one LP-11 -> HS transition
   * per FRAME): every frame therefore contains an HS burst far longer than
   * 3.3 ms and err_to_hstx latches once per frame on a stream that is working
   * exactly as designed.  Enabling it turned a normal condition into what read
   * like a hardware fault for several rounds of this bring-up.  Enable it
   * deliberately when diagnosing a phantom "nothing is transmitted" symptom,
   * and interpret it as "an HS burst exceeded 3.3 ms", not as an error.
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
   * VC, but the reference driver (dw-mipi-dsi2.c) also writes
   * DSI2_DSI_VCID_CFG for the fixed host channel; leaving it at reset (0) can
   * misroute the command. */

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_DSI_VCID_CFG, 0x0);

  /* Do NOT enter AUTOCALC here.  Auto-Calculation mode "stops the data
   * reception from the IPI, CRI, and PRI interfaces" (TRM 18.3.1) while it
   * computes PHY timings, and in the manual-timing path there is no IPI
   * video frame to measure, so AUTOCALC never completes — MODE_STATUS stays
   * stuck at AUTOCALC (0x1) and every CRI command is silently dropped
   * (cri_busy clears instantly, the FIFO drains, but no state machine
   * consumes it and the lanes never leave stop-state).
   *
   * The reference driver reaches Command mode directly from pre_enable
   * without any AUTOCALC pass (it only runs AUTOCALC later, in enable(),
   * and only when auto_calc_mode is set).  The Host↔PHY deskew handshake is
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
 *   exactly 1/16 the lane high-speed data rate in DPHY mode (see the
 *   reference driver dw_mipi_dsi2_ipi_set / dw_mipi_dsi2_phy_ratio_cfg).
 *
 *   A horizontal interval of `pixels` pixels spans the time
 *   `pixels / pixel_clock` seconds, during which phy_hstx_clk ticks
 *   `pixels * phy_hstx_clk / pixel_clock` cycles:
 *
 *     time = pixels * (hs_rate / 16) / pixel_clock    (<< 16 fixed-point)
 *
 *   This is the reference driver's exact formula:
 *     hsa_time = DIV_ROUND_CLOSEST_ULL(hsa * phy_hs_clk << 16, pixel_clk)
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
   * Round to NEAREST, matching the reference driver
   * (dw_mipi_dsi2_ipi_set() uses DIV_ROUND_CLOSEST_ULL for HSA/HBP/HACT/
   * HLINE).  Plain truncation biases every horizontal interval low by up to
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
 * Name: rk3576_dsi_hstx_to_pixels
 *
 * Description:
 *   Inverse of rk3576_dsi_hstx_cycles(): convert a horizontal IPI timing
 *   register value back into pixels using the same clock it was programmed
 *   from, so a read-back can be checked against the panel's timing table.
 *
 *   Used only by the read-only bring-up dump: the round-trip must land on the
 *   panel's own pixel counts, which proves both that the register is not stale
 *   and that it describes the clock the pixel stream actually runs at.
 *
 * Input Parameters:
 *   regval      - Register value (13.16 fixed-point phy_hstx_clk cycles).
 *   hs_rate     - Lane high-speed rate in Hz.
 *   pixel_clock - Pixel clock the value was derived from, in Hz.
 *
 * Returned Value:
 *   The equivalent pixel count (rounded to nearest), or 0 when it cannot be
 *   computed.
 *
 ****************************************************************************/

static uint32_t rk3576_dsi_hstx_to_pixels(uint32_t regval, uint32_t hs_rate,
                                          uint32_t pixel_clock)
{
  uint64_t phy_hstx_clk;

  if (pixel_clock == 0)
    {
      return 0;
    }

  phy_hstx_clk = (uint64_t)hs_rate / 16u;
  if (phy_hstx_clk == 0)
    {
      return 0;
    }

  /* pixels = (regval / 2^16) * pixel_clock / phy_hstx_clk.  Keep the (often
   * fractional) ratio instead of truncating regval >> 16 first: at 13.16
   * resolution the fractional part is worth up to a whole pixel here. */

  return (uint32_t)((((uint64_t)regval * pixel_clock) + (phy_hstx_clk << 15)) /
                    (phy_hstx_clk << 16));
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

  regval = rk3576_dsi_hstx_cycles(timing->hactive, priv->cfg.hs_rate,
                                  pixel_clock);
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

  /* PHY_IPI_RATIO = (hs_rate / 16) / (pixel_clock / 4).  Rounded to nearest
   * like the reference driver (DIV_ROUND_CLOSEST_ULL): this value is the
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
 *   horizontal timing and the CDC ratio of a LIVE pixel datapath asynchronously
 *   (a race, see the long note in that function).  Call it before the host
 *   enters Video mode if it is ever needed again.
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

  ret = rk3576_dsi_program_ipi_h_timing(priv, &priv->timing,
                                        pixel_clock_hz);

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
   * re-programming for video (reference driver behaviour on every mode
   * set).  This is what recovers a wedged previous video session: once an
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
   * link (CRI) is fully up.  This mirrors the reference driver's
   * dw_mipi_dsi2_ipi_color_coding_cfg().
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
   * NOT the panel pixel clock but the IPI clock = dclk_core = crtc_clock/4,
   * per the reference driver dw_mipi_dsi2_get_mipi_pixel_clk():
   *
   *   (Video Timing Pixel Rate) / 4 = MIPI Pixel Clock = dclk_out = dclk_core
   *   dsi2->mipi_pixel_rate = (mode->crtc_clock * MSEC_PER_SEC) / (4 * k)
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

  /* Pixels per video packet -- HACTIVE, exactly as the reference driver
   * programs it.
   *
   * This is a REVERT of our own change (0), and the reason is worth keeping:
   * the TRM (18.4.3) says max_pix_pkt is "used in Video mode (for non-burst
   * modes) and Data Stream mode", that "a value of 0 or bigger than the HACT
   * pixels will originate one single video packet per line", and (18.2.1)
   * that "the minimum value for the max_pix_pkt field should be 48 pixels".
   * We read the first sentence as "0 means one packet per line" and wrote 0
   * everywhere.
   *
   * The reference driver writes mode->hdisplay here -- i.e. HACTIVE, one line
   * per packet by SIZE rather than by special case:
   *
   *     val = mode->hdisplay;
   *     regmap_write(DSI2_IPI_PIX_PKT_CFG, MAX_PIX_PKT(val));
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
   * So: the reference driver's value, and it is also what the other two
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
         (unsigned)(priv->cfg.eotp ? 1 : 0), (unsigned)lanes,
         (unsigned)bpp, (unsigned)timing->pixel_clock,
         (unsigned)priv->cfg.hs_rate);

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

    syslog(LOG_INFO,
           "dsi: MODE_STATUS=%u (expect VIDEO=%u) after %d polls\n",
           (unsigned)(rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 0x7),
           (unsigned)DSI2_MODE_VIDEO, poll);

    /* Dump the DSI-side video-path state so the black-screen diagnosis can
     * tell "never entered video mode" (MODE_STATUS != 3) apart from
     * "entered video but the pixel stream is not flowing" (IPI_BUSY /
     * IPI FIFO empty / PHY interrupt).  All read-only; no behaviour change.
     */

    syslog(LOG_INFO,
           "dsi-dump: MODE_STATUS=%08x CORE_STATUS=%08x MANUAL_MODE=%08x "
           "(pre-VOP: zeros expected)\n",
           rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS),
           rk3576_dsi_getreg(base, RK3576_DSI2_CORE_STATUS),
           rk3576_dsi_getreg(base, RK3576_DSI2_MANUAL_MODE_CFG));
    syslog(LOG_INFO,
           "dsi-dump: INT_ST_PHY=%08x INT_ST_TO=%08x INT_ST_IPI=%08x "
           "INT_ST_FIFO=%08x\n",
           rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_PHY),
           rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_TO),
           rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_IPI),
           rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_FIFO));
    syslog(LOG_INFO,
           "dsi-dump: VID_TX_CFG=%08x IPI_COLOR_MAN=%08x IPI_PIX_PKT=%08x "
           "PHY_IPI_RATIO=%08x\n",
           rk3576_dsi_getreg(base, RK3576_DSI2_DSI_VID_TX_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_COLOR_MAN_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_PIX_PKT_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG));

    /* Read back the horizontal IPI timing and convert it BACK to pixels with
     * the same clock the driver programmed it from.  Two things this catches
     * that no other line can:
     *
     *   - a mismatch between the pixel clock the registers describe and the
     *     one the pixel stream actually runs at (the board requests a rate the
     *     CRU hits exactly, and rk3576_vop_enable_clocks() warns if it does
     *     not -- this closes the loop from the DSI side);
     *   - a stale or mis-programmed register, since the round-trip must land
     *     on the panel's own pixel counts (20/30/720/780).
     *
     * It prints on every call, so a log with several rearm cycles carries the
     * proof that the register set is IDENTICAL across the states being
     * compared (the DSI's behaviour has been observed to change between them
     * with no register difference at all).
     */

    {
      uint32_t pixel_clock = priv->timing_valid ? priv->timing.pixel_clock
                                                : priv->cfg.hs_rate;

      syslog(LOG_INFO,
             "dsi-dump: IPI_H HSA=%08x HBP=%08x HACT=%08x HLINE=%08x -> "
             "pixels %u/%u/%u/%u (expect %u/%u/%u/%u)\n",
             rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HSA_MAN_CFG),
             rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HBP_MAN_CFG),
             rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HACT_MAN_CFG),
             rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HLINE_MAN_CFG),
             rk3576_dsi_hstx_to_pixels(
                 rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HSA_MAN_CFG),
                 priv->cfg.hs_rate, pixel_clock),
             rk3576_dsi_hstx_to_pixels(
                 rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HBP_MAN_CFG),
                 priv->cfg.hs_rate, pixel_clock),
             rk3576_dsi_hstx_to_pixels(
                 rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HACT_MAN_CFG),
                 priv->cfg.hs_rate, pixel_clock),
             rk3576_dsi_hstx_to_pixels(
                 rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HLINE_MAN_CFG),
                 priv->cfg.hs_rate, pixel_clock),
             (unsigned)priv->timing.hsync_len,
             (unsigned)priv->timing.hback_porch,
             (unsigned)priv->timing.hactive,
             (unsigned)(priv->timing.hactive + priv->timing.hsync_len +
                        priv->timing.hback_porch +
                        priv->timing.hfront_porch));
    }

    /* What the CONTROLLER measures from the incoming stream.
     *
     * The _AUTO registers are the read-only counterparts of the _MAN_CFG ones
     * and the TRM describes them as the period "as seen by the controller":
     * they are filled by the auto-calculation path.  We run in manual mode
     * (MANUAL_MODE=1, as the reference driver does), so they may legitimately
     * stay 0 here -- but if they carry values, they are the only direct view
     * of the SOURCE side (what the VOP presents on the IPI) that this driver
     * has ever had, and they would say whether the controller sees the same
     * geometry we programmed into the manual registers.  Printing them costs
     * two lines and can settle the question in one boot; nothing writes them.
     */

    syslog(LOG_INFO,
           "dsi-dump: IPI_H_AUTO HSA=%08x HBP=%08x HACT=%08x HLINE=%08x\n",
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HSA_AUTO),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HBP_AUTO),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HACT_AUTO),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HLINE_AUTO));

    /* The VERTICAL IPI timing, read back and checked the same way the
     * horizontal timing above is.
     *
     * Why this was missing and why it matters: the horizontal registers were
     * given a round-trip self-check several rounds ago and it caught real
     * mistakes, but the vertical ones were only ever WRITTEN.  They decide
     * how many lines the controller puts in a frame and where the vertical
     * sync and porches fall -- i.e. they decide the FRAME BOUNDARY the panel
     * synchronises to.  A wrong value here produces exactly the signature this
     * bring-up has: well-formed packets, a clean link, a panel that reports no
     * errors, and an image that never appears (or appears once and then never
     * updates, because the frames cannot be told apart).
     *
     * Masks matter here too, and they differ per field in the TRM
     * (VSA/VBP 10 bits, VACT 14, VFP 13), so a mask mistake would silently
     * truncate 1280 lines to 256 -- which is why the check prints the decoded
     * line counts rather than just the raw words. */

    {
      uint32_t vsa = rk3576_dsi_getreg(base,
                                       RK3576_DSI2_IPI_VID_VSA_MAN_CFG) &
                      DSI2_IPI_VSA_LINES_MASK;
      uint32_t vbp = rk3576_dsi_getreg(base,
                                       RK3576_DSI2_IPI_VID_VBP_MAN_CFG) &
                      DSI2_IPI_VBP_LINES_MASK;
      uint32_t vact = rk3576_dsi_getreg(base,
                                        RK3576_DSI2_IPI_VID_VACT_MAN_CFG) &
                       DSI2_IPI_VACT_LINES_MASK;
      uint32_t vfp = rk3576_dsi_getreg(base,
                                       RK3576_DSI2_IPI_VID_VFP_MAN_CFG) &
                      DSI2_IPI_VFP_LINES_MASK;
      uint32_t total = vsa + vbp + vact + vfp;
      uint32_t want_total = (uint32_t)priv->timing.vsync_len +
                            priv->timing.vback_porch +
                            priv->timing.vactive +
                            priv->timing.vfront_porch;

      syslog(LOG_INFO,
             "dsi-dump: IPI_V VSA=%u VBP=%u VACT=%u VFP=%u total=%u lines "
             "(expect %u %u %u %u total %u)\n",
             (unsigned)vsa, (unsigned)vbp, (unsigned)vact, (unsigned)vfp,
             (unsigned)total, (unsigned)priv->timing.vsync_len,
             (unsigned)priv->timing.vback_porch, (unsigned)priv->timing.vactive,
             (unsigned)priv->timing.vfront_porch, (unsigned)want_total);

      if (vact != priv->timing.vactive || total != want_total)
        {
          syslog(LOG_ERR,
                 "WARNING: DSI IPI VERTICAL timing does NOT match the "
                 "configured mode -- the controller will build frames the "
                 "panel cannot frame\n");
        }
    }

    syslog(LOG_INFO,
           "dsi-dump: IPI_V_AUTO VSA=%08x VBP=%08x VACT=%08x VFP=%08x\n",
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VSA_AUTO),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VBP_AUTO),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VACT_AUTO),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VFP_AUTO));

    syslog(LOG_INFO, "dsi-dump: VO0_GRF_SOC_CON10=%08x\n",
           getreg32(RK3576_VO0_GRF_ADDR + RK3576_VO0_GRF_SOC_CON10_OFF));

    /* --- Probe D: VOP->DSI routing / dclk gating.  Decisive for "VOP is
     * scanning but ipi_data FIFO stays empty": if grf_mipi_ch_sel (SOC_CON9
     * bit8) is 1, the MIPI IPI is fed from the EBC instead of the VOP;
     * if grf_ebc_dclk2dsihost_disable (SOC_CON13 bit9) is 1, the dclk into
     * the DSI host is gated off.  Also decode the mipi mode/polarity/enable
     * (SOC_CON13 bits 3:0). --- */

    syslog(LOG_INFO,
           "dsi-probe-D: VO0_GRF_SOC_CON9=%08x (mipi_ch_sel[8]=%u "
           "hdmi_ch_sel[9]=%u edp_ch_sel[10]=%u; 1=EBC/wrong-src)\n",
           getreg32(RK3576_VO0_GRF_ADDR + RK3576_VO0_GRF_SOC_CON9_OFF),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON9_OFF) >> 8) & 1),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON9_OFF) >> 9) & 1),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON9_OFF) >> 10) & 1));

    syslog(LOG_INFO,
           "dsi-probe-D: VO0_GRF_SOC_CON13=%08x "
           "(ebc_dclk2dsihost_disable[9]=%u mipi_mode[3]=%u "
           "hsync_pol[2]=%u vsync_pol[1]=%u 1to4_en[0]=%u)\n",
           getreg32(RK3576_VO0_GRF_ADDR + RK3576_VO0_GRF_SOC_CON13_OFF),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON13_OFF) >> 9) & 1),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON13_OFF) >> 3) & 1),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON13_OFF) >> 2) & 1),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON13_OFF) >> 1) & 1),
           (unsigned)((getreg32(RK3576_VO0_GRF_ADDR +
                                RK3576_VO0_GRF_SOC_CON13_OFF) >> 0) & 1));

    /* FSM observation probe: drive DSI2_OBS_FSM_STATUS_SEL to each of the
     * six state machines and read back DSI2_OBS_FSM_STATUS so we can tell
     * where the video pixel stream stalls.  The decisive pair is
     * ipi_vid_fsm (0x0) — stuck at INIT (cur=0) means the VOP never
     * delivered a video line into the IPI; a non-zero advancing state means
     * the pixel stream entered the IPI but stalls later (sys_main /
     * phy_tx_ready).  All read-only diagnostics; the selector is restored
     * to ipi_vid afterwards. */

    {
      static const char *const fsm_names[] = {
        "ipi_vid", "ipi_auto_calc", "sys_main",
        "sys_cmd", "sys_pkt_build", "phy_tx_ready",
      };
      uint32_t fsm_raw[6];
      int fsm_i;

      for (fsm_i = 0; fsm_i < 6; fsm_i++)
        {
          rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FSM_STATUS_SEL,
                            (uint32_t)fsm_i & 0xf);
          fsm_raw[fsm_i] =
            rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FSM_STATUS);
        }

      syslog(LOG_INFO,
             "dsi-dump: FSM(pre-VOP) ipi_vid=%08x ipi_auto_calc=%08x "
             "sys_main=%08x sys_cmd=%08x sys_pkt=%08x phy_tx_ready=%08x\n",
             fsm_raw[0], fsm_raw[1], fsm_raw[2], fsm_raw[3], fsm_raw[4],
             fsm_raw[5]);

      /* Decode ipi_vid_fsm and phy_tx_ready_fsm specially: cur = the raw
       * register's current_state nibble (bits [8:12] per TRM naming is
       * approximate; dump the whole word above is authoritative).  Emit a
       * compact human-readable line for quick scan. */

      syslog(LOG_INFO,
             "dsi-dump: FSM decode ipi_vid(cur=%u stuck=%u) "
             "sys_main(cur=%u) phy_tx_ready(cur=%u stuck=%u)\n",
             (unsigned)((fsm_raw[0] >> DSI2_OBS_FSM_CUR_STATE_SHIFT) & 0x1f),
             (unsigned)((fsm_raw[0] & DSI2_OBS_FSM_STUCK) != 0),
             (unsigned)((fsm_raw[2] >> DSI2_OBS_FSM_CUR_STATE_SHIFT) & 0x1f),
             (unsigned)((fsm_raw[5] >> DSI2_OBS_FSM_CUR_STATE_SHIFT) & 0x1f),
             (unsigned)((fsm_raw[5] & DSI2_OBS_FSM_STUCK) != 0));

      (void)fsm_names; /* Names kept for a future per-FSM labeled dump. */

      /* Restore the selector to ipi_vid (its reset default). */

      rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FSM_STATUS_SEL,
                        DSI2_OBS_FSM_SEL_IPI_VID);
    }

    /* IPI data-FIFO water-level probe.  Unlike INT_ST_IPI (which only
     * reports *errors*) or the FSM observation port (whose current_state_cnt
     * reads back 0 even for a live command path), the OBS_FIFO observation
     * port reports the actual word count + empty/almost_empty/half_full/
     * almost_full/full flags of the selected FIFO in real time.  Selecting
     * ipi_data_fifo (0x3) and sampling it twice a short interval apart
     * definitively splits the two failure classes:
     *
     *   - word_cnt stays 0 + empty=1 across both samples: no pixel ever
     *     entered the IPI -> the break is upstream (VOP -> DSI physical
     *     pixel-clock/data link).
     *   - word_cnt > 0 (or half_full=1) on either sample: pixels ARE
     *     entering the IPI -> the break is in the DSI -> PHY send path.
     *
     * The other FIFOs (cmd_wr_hdr/pld, phy_txhs) are sampled too so the
     * command-path FIFOs act as a positive control proving the observation
     * port itself is live.  All read-only diagnostics. */

    {
      static const char *const fifo_names[] = {
        "cmd_rd_pld", "cmd_wr_hdr", "cmd_wr_pld",
        "ipi_data", "ipi_event", "phy_txhs",
      };
      uint32_t fifo_a[6];
      uint32_t fifo_b[6];
      int fifo_i;

      for (fifo_i = 0; fifo_i < 6; fifo_i++)
        {
          rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                            (uint32_t)fifo_i & 0xf);
          fifo_a[fifo_i] =
            rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS);
        }

      up_udelay(100); /* ~2 video lines @16M pixel clock */

      for (fifo_i = 0; fifo_i < 6; fifo_i++)
        {
          rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                            (uint32_t)fifo_i & 0xf);
          fifo_b[fifo_i] =
            rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS);
        }

      for (fifo_i = 0; fifo_i < 6; fifo_i++)
        {
          uint32_t wa = (fifo_a[fifo_i] & DSI2_OBS_FIFO_WORD_CNT_MASK) >>
                        DSI2_OBS_FIFO_WORD_CNT_SHIFT;
          uint32_t wb = (fifo_b[fifo_i] & DSI2_OBS_FIFO_WORD_CNT_MASK) >>
                        DSI2_OBS_FIFO_WORD_CNT_SHIFT;

          syslog(LOG_INFO,
                 "dsi-dump: FIFO(pre-VOP)[%s] cnt=%u->%u "
                 "empty=%u ae=%u hf=%u af=%u full=%u\n",
                 fifo_names[fifo_i], wa, wb,
                 (unsigned)(fifo_b[fifo_i] & DSI2_OBS_FIFO_EMPTY) != 0,
                 (unsigned)(fifo_b[fifo_i] & DSI2_OBS_FIFO_ALMOST_EMPTY) != 0,
                 (unsigned)(fifo_b[fifo_i] & DSI2_OBS_FIFO_HALF_FULL) != 0,
                 (unsigned)(fifo_b[fifo_i] & DSI2_OBS_FIFO_ALMOST_FULL) != 0,
                 (unsigned)(fifo_b[fifo_i] & DSI2_OBS_FIFO_FULL) != 0);
        }
    }

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
 * Name: rk3576_mipi_dsi_dump_video_status
 *
 * Description:
 *   Probe E: sample the DSI IPI receive path *while the VOP is scanning*
 *   (i.e. after rk3576_vop_initialize has started the pixel stream).  The
 *   enable_video() dump above runs BEFORE the VOP is up, so its empty
 *   ipi_data FIFO / INIT ipi_vid_fsm are expected, not diagnostic.  This
 *   function must be called again after the VOP begins scanning to answer
 *   the real question: do pixels physically reach the DSI IPI?
 *
 *   Read-only, and deliberately sampled DENSELY rather than as a single
 *   point/pair, because every previous misdiagnosis in this bring-up came
 *   from single-shot sampling of a signal that legitimately cycles:
 *     - CORE_STATUS (ipi_busy bit8 / ipi_fifos_not_empty bit9)
 *     - ipi_data / ipi_event / phy_txhs FIFO water levels: sampled 64 times
 *       over ~2 ms, reported as max + "was ever non-empty".  The IPI data
 *       FIFO drains once per video line, so a two-sample pair can read
 *       "empty" twice on a perfectly healthy stream.
 *     - ipi_vid / phy_tx_ready FSMs, decoded with the TRM layout
 *       (cnt[31:16] / prev[12:8] / stuck[5] / state[4:0]); cnt == 0xffff
 *       with stuck == 1 is the "wedged" signature.
 *     - IPI timing registers converted back to pixels for a self-check.
 *     - PHY_STATUS sampled 800 times on a 5 us grid over ~4 ms; a coarse
 *       400 us grid aliases against the ~52 us line period and can read a
 *       healthy toggling clock lane as "never in LP-11".
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dsi_obs_fsm
 *
 * Description:
 *   Read one of the six debug FSMs of the DSI-2 controller.
 *
 *   DSI2_OBS_FSM_STATUS_SEL is written from the APB/sys_clk side, while the
 *   observed FSM runs in its own clock domain (ipi_clk for ipi_vid_fsm,
 *   phy_hstx_clk for phy_tx_ready_fsm).  The selection therefore has to
 *   cross a CDC before it can appear in DSI2_OBS_FSM_STATUS.  Reading it in
 *   the very next APB access (as an earlier revision of this probe did) can
 *   return the previously selected FSM's value, or 0 -- which is
 *   indistinguishable from a genuine "this FSM is at INIT" reading.  Wait a
 *   few microseconds and sample twice; *unstable reports disagreement so
 *   the log can flag an untrustworthy sample instead of silently treating
 *   it as a measurement.
 *
 * Input Parameters:
 *   base     - DSI-2 register base.
 *   sel      - DSI2_OBS_FSM_SEL_*.
 *   unstable - Optional out: set to true when the two samples disagree.
 *
 * Returned Value:
 *   DSI2_OBS_FSM_STATUS for the requested selector.
 *
 ****************************************************************************/

static uint32_t rk3576_dsi_obs_fsm(uintptr_t base, uint32_t sel,
                                   FAR bool *unstable)
{
  uint32_t first;
  uint32_t second;

  rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FSM_STATUS_SEL, sel);
  up_udelay(2);
  first = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FSM_STATUS);
  up_udelay(2);
  second = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FSM_STATUS);

  if (unstable != NULL)
    {
      *unstable = (first != second);
    }

  return second;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_obs_fsm
 *
 * Description:
 *   Public one-shot read of a debug FSM (DSI2_OBS_FSM_SEL_*).  Exists so the
 *   board can take a short LADDER of samples (e.g. eight readings ~3 ms apart)
 *   and tell "this FSM is cycling" from "this FSM is frozen", which a single
 *   sample per health window cannot decide: the state field alone looks the
 *   same in both cases, and the hardware's own 'stuck' bit is latched, so it
 *   stays set long after the FSM started moving again.
 *
 * Input Parameters:
 *   sel - DSI2_OBS_FSM_SEL_*.
 *
 * Returned Value:
 *   DSI2_OBS_FSM_STATUS for the requested FSM, or 0 before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_obs_fsm(uint32_t sel)
{
  struct rk3576_dsi_s *priv = &g_dsi;

  if (!priv->initialized)
    {
      return 0;
    }

  return rk3576_dsi_obs_fsm(priv->base, sel, NULL);
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_obs_fifo
 *
 * Description:
 *   Public one-shot read of a debug FIFO level (DSI2_OBS_FIFO_SEL_*).  See the
 *   prototype's comment for why a ladder of these is worth more than a peak.
 *
 * Input Parameters:
 *   sel - DSI2_OBS_FIFO_SEL_*.
 *
 * Returned Value:
 *   DSI2_OBS_FIFO_STATUS for the requested FIFO, or 0 before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_obs_fifo(uint32_t sel)
{
  struct rk3576_dsi_s *priv = &g_dsi;
  uint32_t st;

  if (!priv->initialized)
    {
      return 0;
    }

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_OBS_FIFO_STATUS_SEL, sel);
  up_udelay(2);
  st = rk3576_dsi_getreg(priv->base, RK3576_DSI2_OBS_FIFO_STATUS);

  return st;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_read_to_status
 *
 * Description:
 *   Read (and thereby clear) DSI2_INT_ST_TO.  See the prototype's comment for
 *   why the same latch is worth sampling repeatedly.
 *
 * Returned Value:
 *   DSI2_INT_ST_TO, or 0 before rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_read_to_status(void)
{
  struct rk3576_dsi_s *priv = &g_dsi;

  if (!priv->initialized)
    {
      return 0;
    }

  return rk3576_dsi_getreg(priv->base, RK3576_DSI2_INT_ST_TO);
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_set_manual_mode
 *
 * Description:
 *   Switch the IPI timing source between the register bank (manual) and the
 *   controller's own measurement of the incoming stream (auto).  See the
 *   prototype's comment for why the auto path is only meaningful with a live
 *   pixel stream.
 *
 * Input Parameters:
 *   enable - true: manual, false: auto.
 *
 * Returned Value:
 *   OK on success; -ENODEV before rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_set_manual_mode(bool enable)
{
  struct rk3576_dsi_s *priv = &g_dsi;

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  rk3576_dsi_putreg(priv->base, RK3576_DSI2_MANUAL_MODE_CFG,
                    enable ? DSI2_MANUAL_MODE_EN : 0u);

  return OK;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_read_ipi_view
 *
 * Description:
 *   Read the manual registers, the auto (measured) registers and the status
 *   words that describe which mode is in force.  See the prototype.
 *
 * Input Parameters:
 *   view - Destination snapshot (must not be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_read_ipi_view(FAR struct rk3576_dsi_ipi_view_s *view)
{
  struct rk3576_dsi_s *priv = &g_dsi;
  uintptr_t base;

  if (view == NULL)
    {
      return -EINVAL;
    }

  memset(view, 0, sizeof(*view));

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  base = priv->base;

  view->hsa_man   = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HSA_MAN_CFG);
  view->hbp_man   = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HBP_MAN_CFG);
  view->hact_man  = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HACT_MAN_CFG);
  view->hline_man = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HLINE_MAN_CFG);

  view->hsa_auto  = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HSA_AUTO);
  view->hbp_auto  = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HBP_AUTO);
  view->hact_auto = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HACT_AUTO);
  view->hline_auto = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HLINE_AUTO);

  view->vsa_auto  = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VSA_AUTO);
  view->vbp_auto  = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VBP_AUTO);
  view->vact_auto = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VACT_AUTO);
  view->vfp_auto  = rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VFP_AUTO);

  view->manual_mode = rk3576_dsi_getreg(base, RK3576_DSI2_MANUAL_MODE_CFG);
  view->mode_status = rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS);
  view->vid_tx_cfg  = rk3576_dsi_getreg(base, RK3576_DSI2_DSI_VID_TX_CFG);
  view->phy_ipi_ratio =
      rk3576_dsi_getreg(base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG);

  return OK;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_read_int_latches
 *
 * Description:
 *   Read all six DSI2_INT_ST_* groups.  See the prototype's comment for why the
 *   IPI group matters even though it is not a "link" error group.
 *
 * Input Parameters:
 *   l - Destination snapshot (must not be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_read_int_latches(FAR struct rk3576_dsi_int_latches_s *l)
{
  struct rk3576_dsi_s *priv = &g_dsi;
  uintptr_t base;

  if (l == NULL)
    {
      return -EINVAL;
    }

  memset(l, 0, sizeof(*l));

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  base = priv->base;

  l->main = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_MAIN);
  l->ipi  = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_IPI);
  l->fifo = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_FIFO);
  l->phy  = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_PHY);
  l->ack  = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_ACK);
  l->to   = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_TO);

  return OK;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_count_lane_episodes
 *
 * Description:
 *   Tight-loop sampler for DSI2_PHY_STATUS.  See the prototype's comment for
 *   what the two counts mean and how to read them.
 *
 * Input Parameters:
 *   samples    - Reads to take (clamped to 1..65535).
 *   hs_samples - Optional out: samples with the data lanes out of LP-11.
 *   clk_hs     - Optional out: samples with the clock lane out of LP-11.
 *
 * Returned Value:
 *   Number of LP-11 -> HS transitions on the data lanes.
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_count_lane_episodes(uint32_t samples,
                                            FAR uint32_t *hs_samples,
                                            FAR uint32_t *clk_hs)
{
  struct rk3576_dsi_s *priv = &g_dsi;
  const uint32_t data_mask = DSI2_PHY_STATUS_PHY_L0_STOPSTATE |
                             DSI2_PHY_STATUS_PHY_L1_STOPSTATE |
                             DSI2_PHY_STATUS_PHY_L2_STOPSTATE |
                             DSI2_PHY_STATUS_PHY_L3_STOPSTATE;
  uint32_t hs = 0;
  uint32_t clk = 0;
  uint32_t edges = 0;
  bool prev_hs = false;
  uint32_t i;

  if (hs_samples != NULL)
    {
      *hs_samples = 0;
    }

  if (clk_hs != NULL)
    {
      *clk_hs = 0;
    }

  if (!priv->initialized)
    {
      return 0;
    }

  if (samples == 0)
    {
      samples = 1;
    }
  else if (samples > 65535u)
    {
      samples = 65535u;
    }

  for (i = 0; i < samples; i++)
    {
      uint32_t st = rk3576_dsi_getreg(priv->base, RK3576_DSI2_PHY_STATUS);
      bool now_hs = (st & data_mask) != data_mask;

      if (now_hs)
        {
          hs++;

          if (!prev_hs)
            {
              edges++;
            }
        }

      if ((st & DSI2_PHY_STATUS_PHY_CLK_STOPSTATE) == 0)
        {
          clk++;
        }

      prev_hs = now_hs;
    }

  if (hs_samples != NULL)
    {
      *hs_samples = hs;
    }

  if (clk_hs != NULL)
    {
      *clk_hs = clk;
    }

  return edges;
}

/****************************************************************************
 * Name: rk3576_dsi_cmd_probe
 *
 * Description:
 *   Sample the phy_tx_ready FSM right after a CRI packet header has been
 *   written, i.e. while a transmission is in flight, and accumulate the
 *   observation.  See rk3576_mipi_dsi_get_cmd_probe() for the motivation.
 *
 *   The sampling must be tight: a 2-byte DCS command spends only a couple of
 *   microseconds in HS, so each pass changes the selector, reads the status
 *   register twice back-to-back (the first read can be stale right after the
 *   selector write, which crosses a clock domain) and then waits 1 us.  Six
 *   passes per command, across the ~200 commands of the panel init sequence,
 *   is a few thousand samples -- the FSM only has to leave INIT for a moment
 *   to be seen.
 *
 *   Cost is ~10 us per command and it is applied AFTER the packet is already
 *   in flight, so it can neither corrupt the command nor shorten the panel's
 *   per-command settle delays (those are applied by the caller afterwards).
 *
 *   Deliberately NOT called from the bus-turnaround read path: there the
 *   sampling would sit between the header write and the first
 *   cri_rd_data_avail poll, adding latency inside the spin that waits for the
 *   panel's answer.  The long/short write paths are the ones the whole init
 *   sequence uses, and every one of them is an HS transmission the panel
 *   demonstrably receives, which is exactly the control this experiment
 *   needs.
 *
 ****************************************************************************/

static void rk3576_dsi_cmd_probe(FAR struct rk3576_dsi_s *priv)
{
  uintptr_t base = priv->base;
  int n;

  priv->cmd_probe_cmds++;

  for (n = 0; n < 6; n++)
    {
      uint32_t v;
      uint32_t state;
      uint32_t prev;

      rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FSM_STATUS_SEL,
                        DSI2_OBS_FSM_SEL_PHY_TX_READY);

      /* The first read after a selector change can still report the
       * previous FSM, so sample twice and keep the second. */

      (void)rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FSM_STATUS);
      v = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FSM_STATUS);

      state = v & DSI2_OBS_FSM_CUR_STATE_MASK;
      prev = (v & DSI2_OBS_FSM_PREV_STATE_MASK) >>
             DSI2_OBS_FSM_PREV_STATE_SHIFT;

      if (state > priv->cmd_probe_txr_max)
        {
          priv->cmd_probe_txr_max = state;
        }

      if (prev > priv->cmd_probe_txr_prev_max)
        {
          priv->cmd_probe_txr_prev_max = prev;
        }

      if (state != 0)
        {
          priv->cmd_probe_txr_noninit = true;
        }

      if (prev != 0)
        {
          priv->cmd_probe_txr_prev_noninit = true;
        }

      up_udelay(1);
    }

  /* Leave the selector at the value the rest of the driver assumes. */

  rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FSM_STATUS_SEL,
                    DSI2_OBS_FSM_SEL_IPI_VID);
}

/****************************************************************************
 * Name: rk3576_dsi_set_mode
 ****************************************************************************/

static uint32_t rk3576_dsi_set_mode(uintptr_t base, uint32_t mode,
                                    int loops)
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
 * Name: rk3576_dsi_video_mode_sweep
 *
 * Description:
 *   Bring-up experiment: try every combination of video transmission mode
 *   (non-burst/sync-events, burst, non-burst/sync-pulses) and clock-lane
 *   type (continuous / non-continuous), and report -- per combination --
 *   whether the PHY lane really cycles between LP-11 and HS.
 *
 *   Why this exists: with the current configuration the pixel stream does
 *   reach the PHY (`phy_txhs` non-empty) and the data lines do carry MIPI
 *   traffic (scope-confirmed), but the lanes NEVER return to LP-11
 *   (clk_stopstate high=0 over 800 samples) and the controller's own
 *   err_to_hstx timeout fires -- i.e. one HS burst that never ends.  A
 *   panel with a NON-continuous clock lane cannot re-synchronise per line
 *   in that state, which is exactly "backlight on, screen black".
 *
 *   Which of the two knobs is responsible cannot be decided by reading the
 *   registers (all the timing/ratio registers self-check as correct), so it
 *   has to be measured.  Doing it here, in one boot, saves a flash cycle
 *   per guess and guarantees every combination is exercised on IDENTICAL
 *   silicon state.
 *
 *   Each combination gets a genuine COMMAND -> VIDEO cycle (VID_TX_CFG is
 *   documented as a video-mode entry option, and the controller is
 *   guaranteed to sequence mode changes through Idle automatically), so it
 *   starts from the same state a fresh boot would.
 *
 *   Reading the result: the clock lane must be in LP-11 for
 *   (htotal-hactive)/htotal = 12.5% of the time in non-continuous mode, so
 *   "clk_high" should land near samples/8 with clk_toggles > 0.  In
 *   continuous mode the clock lane stays in HS and instead the DATA lanes
 *   are the ones that must cycle (data_low > 0 and data_high > 0).
 *
 *   This is throwaway diagnostic code -- delete once the video link works.
 *
 ****************************************************************************/

#define RK3576_DSI_SWEEP_SAMPLES 300 /* x 5 us = 1.5 ms ~ 114 lines */

#if RK3576_DSI_VIDEO_MODE_SWEEP

static void rk3576_dsi_video_mode_sweep(void)
{
  /* Each sweep point is a complete video-entry configuration.  The three
   * knobs are the only ones that (a) differ between this board and the
   * mainline ILI9881D panel drivers, or (b) rest on an unverified
   * assumption in our own code:
   *
   *   mode   - DSI2_VID_TX_CFG.vid_mode_type.  Mainline uses BURST for the
   *            720x1280/4-lane/RGB888 panels and SYNC_PULSE for others;
   *            NO mainline ILI9881D panel uses sync-events (our value).
   *   cont   - PHY_CLK_CFG.clk_type.  Only 2 of 8 mainline ILI9881D panels
   *            request a NON-continuous clock; the 720x1280 parts use a
   *            CONTINUOUS one.  Ours is non-continuous.
   *   ratio  - PHY_IPI_RATIO.  Our value assumes the IPI clock really is
   *            pixel_clock/4 (the reference's crtc_clock/4).  That is the
   *            one number everything else was derived from; if it is wrong
   *            the controller waits for pixels that never arrive on
   *            schedule and the packet never completes.
   */

  static const struct
  {
    uint8_t mode;  /* vid_mode_type: 0=sync-pulses 1=sync-events 2=burst */
    uint8_t cont;  /* clk_type: 0=continuous 1=non-continuous */
    uint8_t ratio; /* PHY_IPI_RATIO scaling: 0=as programmed 1=/2 2=x2 */
    FAR const char *tag;
  } sweep[] =
  {
    /* [0] must always mirror the board's own configuration: it doubles as a
     * self-check that the sweep's restore path reproduces it exactly. */
    { 2, 0, 0, "board: BURST + CONTINUOUS clk (ACTIVE)" },
    { 2, 1, 0, "BURST, non-cont clk" },
    { 1, 1, 0, "sync-events, non-cont clk" },
    { 1, 0, 0, "sync-events, CONTINUOUS clk" },
    { 0, 1, 0, "sync-pulses, non-cont clk" },
    { 0, 0, 0, "sync-pulses, CONTINUOUS clk" },
    { 2, 0, 1, "board, PHY_IPI_RATIO / 2" },
    { 2, 0, 2, "board, PHY_IPI_RATIO x 2" },
  };

  struct rk3576_dsi_s *priv = &g_dsi;
  uintptr_t base = priv->base;
  uint32_t lptx_div;
  uint32_t color;
  int ret;
  int c;

  if (!priv->initialized || !priv->timing_valid)
    {
      return;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return;
    }

  lptx_div = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_CLK_CFG) &
             DSI2_PHY_CLK_LPTX_DIV_MASK;
  color = rk3576_dsi_color_depth(priv->cfg.format) | DSI2_IPI_COLOR_FORMAT_RGB;

  /* A diagnostic HS-TX timeout: 0x1388 = 5000 phy_lptx_clk cycles
   * (~252 us at the 19.8 MHz escape clock, ~19 video lines).  This is far
   * above a healthy burst (~11 us) yet short enough that "the burst never
   * ended" is reported within this function's settle window -- with the
   * production 0xffff (~3.3 ms) value such a stall is barely observable.
   */

  rk3576_dsi_putreg(base, RK3576_DSI2_TIMEOUT_HSTX_CFG, 0x1388);

  syslog(LOG_INFO,
         "dsi-sweep: ===== video mode x clock lane sweep (%u combos) =====\n",
         (unsigned)(sizeof(sweep) / sizeof(sweep[0])));
  syslog(LOG_INFO,
         "dsi-sweep: columns: mode=vid_mode_type (0=sync-pulses 1=sync-events "
         "2=burst) cont=clk_type (0=continuous 1=non-cont) M=mode_status "
         "clkHi/lanes=clock lane LP-11 hits clkTog=transitions "
         "datLo=all-data-driving hits datHi=all-data-in-LP-11 hits "
         "txhs=phy_txhs word count ipiv=ipi_vid_fsm TO=int_st_to\n");

  for (c = 0; c < (int)(sizeof(sweep) / sizeof(sweep[0])); c++)
    {
      uint32_t clk_high = 0;
      uint32_t clk_toggles = 0;
      uint32_t data_high = 0;
      uint32_t data_low = 0;
      uint32_t last;
      uint32_t txhs_a;
      uint32_t txhs_b;
      uint32_t int_to;
      uint32_t ms;
      uint32_t core;
      uint32_t n;

      /* Force a CLEAN datapath: soft reset + PWR_UP cycle + Command mode.
       *
       * A plain MODE_CTRL=COMMAND request is NOT sufficient and was
       * observed to fail outright: once an HS burst has started and never
       * finished, phy_txhs cannot drain, and TRM 18.3.1.1 only accepts a
       * mode change once every FIFO is empty -- so the request is ignored
       * and MODE_STATUS stays at VIDEO forever.  Only the soft resets can
       * flush a wedged datapath and return all six FSMs to INIT.
       */

      rk3576_dsi_rearm(priv);

      if ((rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 7u) !=
          DSI2_MODE_COMMAND)
        {
          syslog(LOG_WARNING,
                 "dsi-sweep: [%d] %s: rearm left MODE_STATUS=%u "
                 "(expected COMMAND=%u), skipping\n",
                 c, sweep[c].tag,
                 (unsigned)(rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) &
                            7u),
                 (unsigned)DSI2_MODE_COMMAND);
          continue;
        }

      /* Re-program the whole video entry from scratch -- the same register
       * set enable_video() writes -- plus this point's overrides.  Must
       * happen AFTER the rearm, since configuration written before a reset
       * cannot be assumed to survive it. */

      rk3576_dsi_putreg(base, RK3576_DSI2_PHY_CLK_CFG,
                        lptx_div | (sweep[c].cont
                                        ? DSI2_PHY_CLK_TYPE_NONCONTINUOUS
                                        : DSI2_PHY_CLK_TYPE_CONTINUOUS));
      rk3576_dsi_putreg(base, RK3576_DSI2_IPI_COLOR_MAN_CFG, color);

      if (rk3576_dsi_program_ipi_h_timing(priv, &priv->timing,
                                          priv->timing.pixel_clock) < 0)
        {
          syslog(LOG_WARNING,
                 "dsi-sweep: [%d] %s: IPI horizontal timing overflow, "
                 "skipping\n",
                 c, sweep[c].tag);
          continue;
        }

      /* PHY_IPI_RATIO is written by program_ipi_h_timing() above, so the
       * scaling override has to come after it. */

      if (sweep[c].ratio != 0)
        {
          uint32_t r = rk3576_dsi_getreg(
              base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG);

          r = (sweep[c].ratio == 1) ? (r / 2u) : (r * 2u);
          rk3576_dsi_putreg(base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG,
                            r & DSI2_PHY_IPI_RATIO_MASK);
        }

      rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VSA_MAN_CFG,
                        priv->timing.vsync_len & DSI2_IPI_VSA_LINES_MASK);
      rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VBP_MAN_CFG,
                        priv->timing.vback_porch & DSI2_IPI_VBP_LINES_MASK);
      rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VACT_MAN_CFG,
                        priv->timing.vactive & DSI2_IPI_VACT_LINES_MASK);
      rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VFP_MAN_CFG,
                        priv->timing.vfront_porch & DSI2_IPI_VFP_LINES_MASK);
      rk3576_dsi_putreg(base, RK3576_DSI2_IPI_PIX_PKT_CFG,
                        priv->timing.hactive & DSI2_IPI_PIX_PKT_MAX_MASK);
      rk3576_dsi_putreg(base, RK3576_DSI2_DSI_VID_TX_CFG,
                        (uint32_t)sweep[c].mode &
                            DSI2_VID_TX_VID_MODE_TYPE_MASK);
      rk3576_dsi_putreg(base, RK3576_DSI2_MANUAL_MODE_CFG,
                        DSI2_MANUAL_MODE_EN);

      ms = rk3576_dsi_set_mode(base, DSI2_MODE_VIDEO, 20000);
      if (ms != DSI2_MODE_VIDEO)
        {
          syslog(LOG_WARNING,
                 "dsi-sweep: [%d] %s: could not reach VIDEO "
                 "(MODE_STATUS=%u), skipping\n",
                 c, sweep[c].tag, (unsigned)ms);
          continue;
        }

      priv->mode = RK3576_DSI_MODE_VIDEO;

      /* Let the new configuration settle for a few frames. */

      up_mdelay(5);

      txhs_a = 0;
      {
        uint32_t st;

        rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                          DSI2_OBS_FIFO_SEL_PHY_TXHS);
        up_udelay(2);
        st = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS);
        txhs_a = (st & DSI2_OBS_FIFO_WORD_CNT_MASK) >>
                 DSI2_OBS_FIFO_WORD_CNT_SHIFT;
      }

      last = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS);

      for (n = 0; n < RK3576_DSI_SWEEP_SAMPLES; n++)
        {
          uint32_t cur = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS);

          if (cur & DSI2_PHY_STATUS_PHY_CLK_STOPSTATE)
            {
              clk_high++;
            }

          if ((cur ^ last) & DSI2_PHY_STATUS_PHY_CLK_STOPSTATE)
            {
              clk_toggles++;
            }

          last = cur;

          if ((cur & 0x00001e00u) == 0x00001e00u)
            {
              data_high++;
            }
          else
            {
              data_low++;
            }

          up_udelay(5);
        }

      rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                        DSI2_OBS_FIFO_SEL_PHY_TXHS);
      up_udelay(2);
      txhs_b = (rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS) &
                DSI2_OBS_FIFO_WORD_CNT_MASK) >>
               DSI2_OBS_FIFO_WORD_CNT_SHIFT;

      int_to = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_TO); /* RC */
      core = rk3576_dsi_getreg(base, RK3576_DSI2_CORE_STATUS);

      syslog(LOG_INFO,
             "dsi-sweep: [%d] mode=%u cont=%u ratio=%u \"%s\" M=%u "
             "clkHi=%u/%u clkTog=%u datLo=%u datHi=%u txhs=%u..%u ipiv=%08x "
             "TO=%02x (hstx=%u hstxrdy=%u) ipibs=%u -> %s\n",
             c, (unsigned)sweep[c].mode, (unsigned)sweep[c].cont,
             (unsigned)sweep[c].ratio, sweep[c].tag,
             (unsigned)(rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 7u),
             (unsigned)clk_high, (unsigned)RK3576_DSI_SWEEP_SAMPLES,
             (unsigned)clk_toggles, (unsigned)data_low, (unsigned)data_high,
             (unsigned)txhs_a, (unsigned)txhs_b,
             rk3576_dsi_obs_fsm(base, DSI2_OBS_FSM_SEL_IPI_VID, NULL),
             (unsigned)int_to, (unsigned)(int_to & 1u),
             (unsigned)((int_to >> 1) & 1u),
             (unsigned)((core >> 8) & 1u),
             (clk_toggles > 0 && clk_high > 0 &&
              clk_high < RK3576_DSI_SWEEP_SAMPLES)
                 ? "CLOCK LANE CYCLES LP-11 <== HEALTHY"
                 : (data_low > 0 && data_high > 0)
                       ? "data lanes cycle, clock lane does not"
                       : "lanes NEVER return to LP-11");
    }

  /* Restore the board's configured video mode + clock lane type and put the
   * controller back into Video mode so the rest of the driver's probes (and
   * the running system) see the same state they had before the sweep.
   *
   * The rearm is mandatory here for the same reason as inside the loop: the
   * last sweep point may well have left the datapath wedged, and a wedged
   * datapath ignores mode changes. */

  rk3576_dsi_rearm(priv);

  rk3576_dsi_putreg(base, RK3576_DSI2_PHY_CLK_CFG,
                    lptx_div | (priv->cfg.continuous_clk
                                    ? DSI2_PHY_CLK_TYPE_CONTINUOUS
                                    : DSI2_PHY_CLK_TYPE_NONCONTINUOUS));
  rk3576_dsi_putreg(base, RK3576_DSI2_DSI_VID_TX_CFG,
                    (uint32_t)priv->cfg.video_mode &
                        DSI2_VID_TX_VID_MODE_TYPE_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_COLOR_MAN_CFG, color);
  rk3576_dsi_program_ipi_h_timing(priv, &priv->timing,
                                  priv->timing.pixel_clock);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VSA_MAN_CFG,
                    priv->timing.vsync_len & DSI2_IPI_VSA_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VBP_MAN_CFG,
                    priv->timing.vback_porch & DSI2_IPI_VBP_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VACT_MAN_CFG,
                    priv->timing.vactive & DSI2_IPI_VACT_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_VID_VFP_MAN_CFG,
                    priv->timing.vfront_porch & DSI2_IPI_VFP_LINES_MASK);
  rk3576_dsi_putreg(base, RK3576_DSI2_IPI_PIX_PKT_CFG,
                    priv->timing.hactive & DSI2_IPI_PIX_PKT_MAX_MASK);
  rk3576_dsi_set_mode(base, DSI2_MODE_VIDEO, 20000);
  priv->mode = RK3576_DSI_MODE_VIDEO;

  rk3576_dsi_putreg(base, RK3576_DSI2_TIMEOUT_HSTX_CFG, 0xffff);

  syslog(LOG_INFO,
         "dsi-sweep: ===== restored vid_mode_type=%u clk_type=%u "
         "(MODE_STATUS=%u PHY_STATUS=%08x VID_TX_CFG=%08x PHY_CLK_CFG=%08x "
         "PHY_IPI_RATIO=%08x) =====\n",
         (unsigned)(priv->cfg.video_mode & DSI2_VID_TX_VID_MODE_TYPE_MASK),
         (unsigned)rk3576_dsi_getreg(base, RK3576_DSI2_PHY_CLK_CFG) & 1u,
         (unsigned)(rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS) & 7u),
         rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS),
         rk3576_dsi_getreg(base, RK3576_DSI2_DSI_VID_TX_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_PHY_CLK_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG));

  nxmutex_unlock(&priv->lock);
}

#endif /* RK3576_DSI_VIDEO_MODE_SWEEP */

void rk3576_mipi_dsi_dump_video_status(void)
{
  struct rk3576_dsi_s *priv = &g_dsi;
  uintptr_t base = priv->base;
  uint32_t core_status;
  uint32_t fifo_max[3];
  uint32_t fifo_ever[3];
  uint32_t fifo_full[3];
  uint32_t fifo_min[3];
  uint32_t fsm[RK3576_DSI2_NUM_OBS_FSMS];
  bool fsm_unstable[RK3576_DSI2_NUM_OBS_FSMS];
  uint32_t int_st_ipi;
  uint32_t int_st_to;
  uint32_t int_st_phy;
  uint32_t int_mask_to;
  uint32_t mode_status;

  if (!priv->initialized || priv->mode != RK3576_DSI_MODE_VIDEO)
    {
      return;
    }

  core_status = rk3576_dsi_getreg(base, RK3576_DSI2_CORE_STATUS);
  mode_status = rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS);
  int_st_ipi = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_IPI);

  /* DSI2_INT_ST_TO / INT_ST_PHY are read-clear: this read consumes whatever
   * the hardware latched up to now.  That is intentional here (the stall is
   * already established), and err_to_hstxrdy is the single most valuable
   * signal available -- it is the controller itself reporting "I asked the
   * PHY to transmit in HS and it never became ready". */

  int_st_to = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_TO);
  int_st_phy = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_PHY);
  int_mask_to = rk3576_dsi_getreg(base, RK3576_DSI2_INT_MASK_TO);

  /* Repeatedly sample the three video-path FIFOs over ~2 ms (a few video
   * lines).  A single PAIR of samples cannot distinguish "the FIFO was
   * drained between the two reads" from "the FIFO never held anything":
   * the IPI data FIFO legitimately cycles empty -> non-empty -> empty once
   * per line (each line's packet is built and handed on within one line
   * time), so a pair of reads 200 us apart can easily land empty twice
   * while the stream is perfectly healthy -- and previous rounds misread
   * exactly that as "no pixels".  The reliable criteria are the MAX water
   * level and whether the FIFO was EVER non-empty across the window:
   *
   *   ipi_data  max > 0  -> pixels DO reach the IPI (break is downstream)
   *   ipi_data  always 0 -> no pixel ever entered the IPI (break upstream)
   *   phy_txhs  max > 0  -> the IPI handed packets to the PHY TX FIFO
   *   phy_txhs  always 0 -> nothing left the IPI: the send side is stalled
   *                         (this is the "phy_tx_ready FSM stuck at INIT"
   *                         hypothesis, now testable directly)
   *   ipi_event sees sync/blanking events only (data lane toggles identically
   *   for a black and a white framebuffer).
   */

  {
    static const uint8_t fifo_sel[3] = {
      DSI2_OBS_FIFO_SEL_IPI_DATA,
      DSI2_OBS_FIFO_SEL_IPI_EVENT,
      DSI2_OBS_FIFO_SEL_PHY_TXHS,
    };
    int f;
    int n;

    for (f = 0; f < 3; f++)
      {
        fifo_max[f] = 0;
        fifo_ever[f] = 0;
        fifo_full[f] = 0;
        fifo_min[f] = 0xffffu;
      }

    for (n = 0; n < 64; n++)
      {
        for (f = 0; f < 3; f++)
          {
            uint32_t st;

            rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                              (uint32_t)fifo_sel[f]);

            /* Same CDC caveat as rk3576_dsi_obs_fsm(): let the newly written
             * selector propagate before sampling the status. */

            up_udelay(2);
            st = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS);

            {
              uint32_t cnt = (st & DSI2_OBS_FIFO_WORD_CNT_MASK) >>
                             DSI2_OBS_FIFO_WORD_CNT_SHIFT;

              if (cnt > fifo_max[f])
                {
                  fifo_max[f] = cnt;
                }

              if (cnt < fifo_min[f])
                {
                  fifo_min[f] = cnt;
                }

              if (st & (DSI2_OBS_FIFO_FULL | DSI2_OBS_FIFO_ALMOST_FULL))
                {
                  fifo_full[f]++;
                }

              if ((st & DSI2_OBS_FIFO_EMPTY) == 0 || cnt != 0)
                {
                  fifo_ever[f] = 1;
                }
            }
          }

        up_udelay(30); /* 64 * 30us ~ 1.9 ms ~ 3+ video lines */
      }
  }

  /* Read ALL SIX debug FSMs, not just ipi_vid + phy_tx_ready.  The video
   * TX chain is ipi_vid_fsm -> sys_pkt_build_fsm -> sys_main_fsm ->
   * phy_tx_ready_fsm, so a stall can sit anywhere along it; reporting only
   * the two ends cannot tell "the IPI side never delivered a line" apart
   * from "the IPI delivered, but the SYS packet builder never consumed the
   * data FIFO".  With all six states plus the per-FSM cycle counter the
   * break can be located to a single stage. */

  {
    int s;

    for (s = 0; s < RK3576_DSI2_NUM_OBS_FSMS; s++)
      {
        fsm[s] = rk3576_dsi_obs_fsm(base, (uint32_t)s, &fsm_unstable[s]);
      }
  }

  syslog(LOG_INFO,
         "vop-scanning: DSI CORE_STATUS=%08x (ipi_busy[8]=%u "
         "ipi_fifos_not_empty[9]=%u core_busy[0]=%u) MODE_STATUS=%08x "
         "INT_ST_IPI=%08x\n",
         core_status,
         (unsigned)((core_status >> 8) & 1),
         (unsigned)((core_status >> 9) & 1),
         (unsigned)(core_status & 1),
         mode_status, int_st_ipi);

  /* The controller's own timeout verdict.  err_to_hstxrdy is the hardware
   * equivalent of "phy_tx_ready never came": it latches when an HS
   * transmission was requested and PHY_TX_READY was not asserted within
   * DSI2_TIMEOUT_HSTXRDY_CFG phy_lptx_clk cycles.  If it is CLEAR while the
   * pixel stream is stalled, the controller never even got as far as
   * requesting HS -- which moves the fault to whatever feeds it (the SYS
   * packet builder / IPI FSM), not to the PPI handshake. */

  syslog(LOG_INFO,
         "vop-scanning: DSI INT_ST_TO=%08x (err_hstx[0]=%u err_hstxrdy[1]=%u "
         "err_lptxrdy[3]=%u) INT_MASK_TO=%08x INT_ST_PHY=%08x "
         "[RC: this read consumes them]\n",
         int_st_to,
         (unsigned)((int_st_to >> 0) & 1),
         (unsigned)((int_st_to >> 1) & 1),
         (unsigned)((int_st_to >> 3) & 1),
         int_mask_to, int_st_phy);

  /* Core configuration snapshot: a stalled video path is only meaningful if
   * these are the values we think we programmed. */

  syslog(LOG_INFO,
         "vop-scanning: DSI PWR_UP=%08x SOFT_RESET=%08x GENERAL_CFG=%08x "
         "MANUAL_MODE=%08x VCID=%08x\n",
         rk3576_dsi_getreg(base, RK3576_DSI2_PWR_UP),
         rk3576_dsi_getreg(base, RK3576_DSI2_SOFT_RESET),
         rk3576_dsi_getreg(base, RK3576_DSI2_DSI_GENERAL_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_MANUAL_MODE_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_DSI_VCID_CFG));
  syslog(LOG_INFO,
         "vop-scanning: DSI TIMEOUT_HSTX=%08x TIMEOUT_HSTXRDY=%08x "
         "IPI_COLOR=%08x VID_TX_CFG=%08x PIX_PKT=%08x\n",
         rk3576_dsi_getreg(base, RK3576_DSI2_TIMEOUT_HSTX_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_TIMEOUT_HSTXRDY_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_IPI_COLOR_MAN_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_DSI_VID_TX_CFG),
         rk3576_dsi_getreg(base, RK3576_DSI2_IPI_PIX_PKT_CFG));

  syslog(LOG_INFO,
         "vop-scanning: DSI FIFO(min/max/ever/full_hits) over 64 samples "
         "~2ms:\n");
  syslog(LOG_INFO,
         "vop-scanning:   ipi_data  min=%u max=%u ever=%u full_hits=%u\n",
         (unsigned)fifo_min[0], (unsigned)fifo_max[0],
         (unsigned)fifo_ever[0], (unsigned)fifo_full[0]);
  syslog(LOG_INFO,
         "vop-scanning:   ipi_event min=%u max=%u ever=%u full_hits=%u\n",
         (unsigned)fifo_min[1], (unsigned)fifo_max[1],
         (unsigned)fifo_ever[1], (unsigned)fifo_full[1]);
  syslog(LOG_INFO,
         "vop-scanning:   phy_txhs  min=%u max=%u ever=%u full_hits=%u "
         "(ever=0 -> nothing ever left the IPI; max at/near the FIFO depth "
         "with full_hits>0 -> the PHY is not draining the TX FIFO)\n",
         (unsigned)fifo_min[2], (unsigned)fifo_max[2],
         (unsigned)fifo_ever[2], (unsigned)fifo_full[2]);

  /* Decode both FSMs with the TRM's authoritative field layout
   * (DSI2_OBS_FSM_STATUS): [31:16] current_state_cnt, [12:8]
   * previous_state, [5] stuck, [4:0] current_state.  cnt saturated at
   * 0xffff together with stuck=1 is the signature of a wedged FSM.
   *
   * Printed with the counter so a genuinely advancing FSM can be told from
   * one that is merely caught mid-walk, and with a '!' when the two samples
   * taken 2 us apart disagreed (selector CDC not settled -> discard).
   */

  {
    static FAR const char *const names[RK3576_DSI2_NUM_OBS_FSMS] = {
      "ipi_vid", "ipi_auto_calc", "sys_main", "sys_cmd", "sys_pkt_build",
      "phy_tx_ready"
    };
    char line[224];
    int n = 0;
    int s;

    for (s = 0; s < RK3576_DSI2_NUM_OBS_FSMS; s++)
      {
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      "%s%s=%08x(c=%u st=%u s=%u%s)", s == 0 ? "" : " ",
                      names[s], (unsigned)fsm[s],
                      (unsigned)((fsm[s] >> DSI2_OBS_FSM_CNT_SHIFT) & 0xffff),
                      (unsigned)((fsm[s] & DSI2_OBS_FSM_STUCK) != 0),
                      (unsigned)(fsm[s] & DSI2_OBS_FSM_STATE_MASK),
                      fsm_unstable[s] ? "!" : "");
      }

    syslog(LOG_INFO, "vop-scanning: DSI FSM(all) %s\n", line);
  }

  /* --- Probe H: IPI timing register readback + self-check.  The
   * enable_video() path programs HSA/HBP/HACT/HLINE in phy_hstx_clk cycles
   * (13.16 fixed point) derived from the pixel clock, and
   * rk3576_mipi_dsi_update_pixel_clock() re-derives them once the VOP's
   * real dclk is known.  Because those registers only store cycles, a
   * nominal-vs-actual clock mismatch is INVISIBLE in the raw values; this
   * probe therefore converts them back to pixels using the clock the driver
   * currently believes in, so the numbers can be compared directly against
   * the panel timing (25/26/720/823 for kickpi-k7).
   *
   *   - converted values == 25/26/720/823  -> the IPI timing matches the
   *     pixel clock the driver is using (self-consistent).
   *   - they are off by the dclk ratio (e.g. HACT ~= 705 instead of 720)
   *     -> the driver is still on a stale clock; compare "pixel_clk=" here
   *     against the VOP's dclk_vp0 in the vop-dump line.
   */

  {
    syslog(LOG_INFO,
           "vop-scanning: IPI_TIMING HSA=%08x HBP=%08x HACT=%08x "
           "HLINE=%08x\n",
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HSA_MAN_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HBP_MAN_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HACT_MAN_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_HLINE_MAN_CFG));
    syslog(LOG_INFO,
           "vop-scanning: IPI_TIMING VSA=%08x VBP=%08x VACT=%08x "
           "VFP=%08x\n",
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VSA_MAN_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VBP_MAN_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VACT_MAN_CFG),
           rk3576_dsi_getreg(base, RK3576_DSI2_IPI_VID_VFP_MAN_CFG));

    {
      uint32_t pc = priv->timing.pixel_clock;
      uint32_t hstx = priv->cfg.hs_rate / 16u;

      if (pc != 0 && hstx != 0)
        {
          uint32_t hsa = rk3576_dsi_getreg(base,
                                           RK3576_DSI2_IPI_VID_HSA_MAN_CFG);
          uint32_t hbp = rk3576_dsi_getreg(base,
                                           RK3576_DSI2_IPI_VID_HBP_MAN_CFG);
          uint32_t hact = rk3576_dsi_getreg(base,
                                            RK3576_DSI2_IPI_VID_HACT_MAN_CFG);
          uint32_t hline = rk3576_dsi_getreg(base,
                                             RK3576_DSI2_IPI_VID_HLINE_MAN_CFG);
          uint32_t ratio = rk3576_dsi_getreg(
              base, RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG);

          /* The registers hold a 13.16 fixed-point cycle count.  Convert
           * the WHOLE fixed-point value back to pixels, e.g.
           *   pixels = reg * pixel_clock / (hstx * 65536)
           * (truncating the fraction first, as an earlier revision of this
           * probe did, loses up to 1-2 pixels and prints false mismatches).
           */

          syslog(LOG_INFO,
                 "vop-scanning: IPI_TIMING self-check pixel_clk=%u "
                 "hstx_clk=%u -> HSA=%upx HBP=%upx HACT=%upx HLINE=%upx "
                 "(panel wants 25/26/720/823) PHY_IPI_RATIO=%08x "
                 "(expect ~0x%08x for this pixel clock)\n",
                 (unsigned)pc, (unsigned)hstx,
                 (unsigned)((((uint64_t)hsa * pc) + ((uint64_t)hstx << 15)) /
                            ((uint64_t)hstx << 16)),
                 (unsigned)((((uint64_t)hbp * pc) + ((uint64_t)hstx << 15)) /
                            ((uint64_t)hstx << 16)),
                 (unsigned)((((uint64_t)hact * pc) + ((uint64_t)hstx << 15)) /
                            ((uint64_t)hstx << 16)),
                 (unsigned)((((uint64_t)hline * pc) + ((uint64_t)hstx << 15)) /
                            ((uint64_t)hstx << 16)),
                 ratio,
                 (unsigned)(((uint64_t)hstx << 16) / (pc / 4u)));
        }
    }
  }

  /* --- Probe F: PHY lane state.  phy_tx_ready_fsm stuck at INIT means the
   * controller never saw the PHY report LP-11 (stopstate), which is the
   * precondition for starting any HS burst.  DSI2_PHY_STATUS reports the
   * per-lane stopstate + direction directly; DSI2_PHY_CLK_CFG shows whether
   * the clock lane is continuous (which can prevent it dropping to LP-11)
   * and the echo clock divider; DSI2_PHY_MODE_CFG confirms PPI width /
   * lane count / PHY type.  All read-only diagnostics. --- */

  {
    uint32_t phy_status = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS);
    uint32_t phy_clk_cfg = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_CLK_CFG);
    uint32_t phy_mode = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_MODE_CFG);

    syslog(LOG_INFO,
           "vop-scanning: DSI PHY_STATUS=%08x (clk_stopstate[8]=%u "
           "l0[9]=%u l1[10]=%u l2[11]=%u l3[12]=%u dir[0]=%u "
           "ulpsactivenot[20:16]=%02x) stopstate_all=%u "
           "(1 on every lane = the PHY is idle in LP-11 and has NOT left "
           "stopstate -> no HS burst is in flight at this instant)\n",
           phy_status,
           (unsigned)((phy_status >> 8) & 1),
           (unsigned)((phy_status >> 9) & 1),
           (unsigned)((phy_status >> 10) & 1),
           (unsigned)((phy_status >> 11) & 1),
           (unsigned)((phy_status >> 12) & 1),
           (unsigned)(phy_status & 1),
           (unsigned)((phy_status >> 16) & 0x1f),
           (unsigned)(((phy_status & DSI2_PHY_STATUS_STOPSTATE_MASK) ==
                       DSI2_PHY_STATUS_STOPSTATE_MASK)
                          ? 1
                          : 0));

    syslog(LOG_INFO,
           "vop-scanning: DSI PHY_CLK_CFG=%08x "
           "(clk_type[0]=%u, 0=continuous 1=non-cont, lptx_div[12:8]=%u) "
           "PHY_MODE_CFG=%08x (ppi_width[9:8]=%u lanes[5:4]=%u "
           "type[0]=%u)\n",
           phy_clk_cfg,
           (unsigned)(phy_clk_cfg & 1),
           (unsigned)((phy_clk_cfg >> 8) & 0x1f),
           phy_mode,
           (unsigned)((phy_mode >> 8) & 3),
           (unsigned)((phy_mode >> 4) & 3),
           (unsigned)(phy_mode & 1));
  }

  /* Restore the FSM selector to ipi_vid (its reset default). */

  rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FSM_STATUS_SEL,
                    DSI2_OBS_FSM_SEL_IPI_VID);

  /* --- Probe I: DCPHY state.  Everything above lives in the DSI *host*;
   * this dumps the PHY that actually drives the wires: PLL dividers (i.e.
   * the real lane rate), per-lane enable/ready, the HS drive-strength code
   * (clock lane 52 ohm vs data lanes 39 ohm) and all LP/HS timing counters
   * plus the escape-clock divider.  A wrong T_HS_EXIT / T_HS_TRAIL /
   * escape-clock value is what prevents a lane from returning to LP-11,
   * which is precisely the "all lanes stuck out of stopstate" symptom seen
   * while the pixel stream is clearly flowing.  Read-only. --- */

  rk3576_dcphy_dump();

  /* --- Probe G: is the clock/data lane REALLY stuck in HS, or just sampled
   * during HACT?
   *
   * Sampling layout matters here.  With clk_type=1 (non-continuous) and
   * every blk_*_hs_en=0, the clock lane MUST return to LP-11 during every
   * line's blanking interval, i.e. it is in LP-11 for roughly
   * (htotal - hactive)/htotal = (823-720)/823 = 12.5% of the time.  The IPI
   * is fed at dclk/4 = 15.6 MHz and a line is 823 pixels, so a line lasts
   * ~52.6 us -- a 400 us sampling grid ALIASES against that (it lands at
   * nearly the same phase every 8th line), which is exactly how a healthy
   * toggling lane can read back as "high=0 toggles=0" and be mistaken for a
   * fault.  Sample densely (5 us steps) over ~4 ms instead, and count the
   * *data* lane stopstates too: a genuinely framed video stream drives
   * data lanes out of LP-11 during HACT and back for blanking, so all four
   * lanes showing the same toggle behaviour as the clock lane is the
   * healthy signature.
   *
   * Decisive readings (clk_high counts samples in which the clock lane
   * stopstate bit was SET, i.e. the lane was in LP-11):
   *   0 < clk_high < samples && toggles > 0
   *                             -> the clock lane cycles LP-11 <-> HS as a
   *                                non-continuous video clock must: the PHY
   *                                is genuinely transmitting.
   *   clk_high == 0              -> never seen in LP-11: the lane is stuck
   *                                in HS (or the PPI read path is dead).
   *   clk_high == samples        -> ALWAYS in LP-11: the lane never left
   *                                stopstate, i.e. NOT ONE HS burst has
   *                                been sent in the whole window.  This is
   *                                the mirror image of the case above and
   *                                an earlier revision of this probe
   *                                mislabelled it as "healthy toggle".
   *                                An ideal non-continuous clock lane sits
   *                                in LP-11 for only
   *                                (htotal-hactive)/htotal = 12.5% of the
   *                                time, so ~samples/8 hits are expected.
   */

  {
    uint32_t clk_high = 0;
    uint32_t clk_toggles = 0;
    uint32_t last_clk = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS);
    uint32_t lane_high[4] = { 0, 0, 0, 0 };
    uint32_t samples = 0;
    int n;
    int i;

    for (n = 0; n < 800; n++)
      {
        uint32_t cur = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS);

        samples++;

        if (cur & DSI2_PHY_STATUS_PHY_CLK_STOPSTATE)
          {
            clk_high++;
          }

        if ((cur ^ last_clk) & DSI2_PHY_STATUS_PHY_CLK_STOPSTATE)
          {
            clk_toggles++;
          }

        last_clk = cur;

        for (i = 0; i < 4; i++)
          {
            if (cur & (DSI2_PHY_STATUS_PHY_L0_STOPSTATE << i))
              {
                lane_high[i]++;
              }
          }

        up_udelay(5); /* 800 * 5us = 4 ms ~ 76 video lines */
      }

    syslog(LOG_INFO,
           "dsi-probe-G: clk_stopstate sampled=%u high=%u toggles=%u "
           "(expected ~%u hits if the clock lane really cycles HS/LP-11; "
           "high=0 -> stuck in HS, high=samples -> NEVER left LP-11, "
           "i.e. no HS burst was ever sent)\n",
           samples, clk_high, clk_toggles, (unsigned)(samples / 8u));

    syslog(LOG_INFO,
           "dsi-probe-G: data_lane_stopstate_high l0=%u l1=%u l2=%u l3=%u "
           "(of %u samples; expected ~%u: data lanes leave LP-11 only for "
           "the active line.  ==samples on all four = the data lanes never "
           "transmitted either)\n",
           (unsigned)lane_high[0], (unsigned)lane_high[1],
           (unsigned)lane_high[2], (unsigned)lane_high[3], samples,
           (unsigned)(samples / 8u));
  }

  /* --- Sweep: the decisive experiment.  Everything above describes the
   * board's configured video mode; this section MEASURES the alternative
   * (mode_type, clk_type) combinations to find out which one lets the PHY
   * lanes cycle back to LP-11.  Must run last, because it temporarily
   * leaves Video mode. --- */

#if RK3576_DSI_VIDEO_MODE_SWEEP
  rk3576_dsi_video_mode_sweep();
#endif
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_get_health
 *
 * Description:
 *   Take a compact, repeatable snapshot of the DSI video path so the caller
 *   can POLL it over time.
 *
 *   rk3576_mipi_dsi_dump_video_status() answers "is the stream alive right
 *   now?".  This answers "did it die, and when?" -- which a one-shot dump
 *   structurally cannot answer, and which is exactly the question raised by
 *   a panel that lights up and then fades because the pixel stream stopped
 *   (an LCD that is not refreshed decays, fastest in the corners).
 *
 *   Field meanings:
 *     video_mode          - DSI2_MODE_STATUS == Video (3)
 *     ipi_busy            - the IPI is receiving pixels (VOP -> DSI alive)
 *     ipi_fifos_not_empty - the IPI still holds packets
 *     ipi_data_max        - peak ipi_data FIFO level over ~0.5 ms of
 *                           sampling (0 while the VOP scans => the break is
 *                           upstream of the DSI)
 *     phy_txhs_max        - peak phy_txhs FIFO level (0 => nothing left the
 *                           IPI; non-zero => the PHY is being fed)
 *     int_st_to           - timeout flags latched SINCE THE PREVIOUS CALL
 *                           (INT_ST_TO is read-clear).  err_hstx latched at
 *                           the same time the stream vanishes is the
 *                           controller reporting "the HS burst never
 *                           finished" -- the signature of a wedged TX.
 *
 * Input Parameters:
 *   health - Destination snapshot.  Zeroed, then filled in.
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_get_health(FAR struct rk3576_dsi_health_s *health)
{
  struct rk3576_dsi_s *priv = &g_dsi;
  uintptr_t base = priv->base;
  uint32_t core_status;
  int n;

  if (health == NULL)
    {
      return -EINVAL;
    }

  memset(health, 0, sizeof(*health));

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  health->mode_status = rk3576_dsi_getreg(base, RK3576_DSI2_MODE_STATUS);

  /* Sample the two registers that say what the controller is ACTUALLY doing,
   * as opposed to what it was asked to do.  Reading these is what lets a
   * caller verify a mode change took effect instead of inferring it from the
   * fact that the change was requested. */

  health->vid_tx_cfg = rk3576_dsi_getreg(base, RK3576_DSI2_DSI_VID_TX_CFG);
  health->phy_clk_cfg = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_CLK_CFG);

  core_status = rk3576_dsi_getreg(base, RK3576_DSI2_CORE_STATUS);
  health->core_status = core_status;

  /* Reading INT_ST_TO also consumes it, so a poller sees each latched error
   * exactly once and can attribute it to the interval in which it appeared.
   */

  health->int_st_to = rk3576_dsi_getreg(base, RK3576_DSI2_INT_ST_TO);

  health->video_mode = (health->mode_status == DSI2_MODE_VIDEO);
  health->ipi_busy = (core_status & DSI2_CORE_STATUS_IPI_BUSY) != 0;
  health->ipi_fifos_not_empty =
      (core_status & DSI2_CORE_STATUS_IPI_FIFOS_NOT_EMPTY) != 0;

  /* Sample the two throughput-critical FIFOs.  A single pair of reads cannot
   * tell "drained between the two samples" from "never held anything" (the
   * IPI data FIFO legitimately cycles empty -> non-empty -> empty once per
   * line), so take a burst of samples and keep the peaks.  ~16 x 34 us
   * ~ 0.5 ms. */

  for (n = 0; n < 16; n++)
    {
      uint32_t st;
      uint32_t cnt;

      /* PHY lane states.  This is the only witness for "is the PHY actually
       * driving HS on the data lanes": stopstate == 0 means the lane is OUT
       * of LP-11, i.e. in a high-speed burst.  A data bus that never leaves
       * LP-11 while the TX FIFO sits full is a complete, self-consistent
       * picture of "nothing is being transmitted". */

      health->phy_status = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS);

      if (health->phy_status & DSI2_PHY_STATUS_PHY_CLK_STOPSTATE)
        {
          /* Still in LP-11 (idle) -- nothing to count. */
        }
      else
        {
          health->clk_hs_hits++;
        }

      if ((health->phy_status & (DSI2_PHY_STATUS_PHY_L0_STOPSTATE |
                                 DSI2_PHY_STATUS_PHY_L1_STOPSTATE |
                                 DSI2_PHY_STATUS_PHY_L2_STOPSTATE |
                                 DSI2_PHY_STATUS_PHY_L3_STOPSTATE)) !=
          (DSI2_PHY_STATUS_PHY_L0_STOPSTATE |
           DSI2_PHY_STATUS_PHY_L1_STOPSTATE |
           DSI2_PHY_STATUS_PHY_L2_STOPSTATE |
           DSI2_PHY_STATUS_PHY_L3_STOPSTATE))
        {
          health->data_hs_hits++;
        }

      rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                        DSI2_OBS_FIFO_SEL_IPI_DATA);
      up_udelay(2);
      st = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS);
      cnt = (st & DSI2_OBS_FIFO_WORD_CNT_MASK) >>
            DSI2_OBS_FIFO_WORD_CNT_SHIFT;
      if (cnt > health->ipi_data_max)
        {
          health->ipi_data_max = cnt;
        }

      if ((st & DSI2_OBS_FIFO_EMPTY) == 0 || cnt != 0)
        {
          health->ipi_data_ever = true;
          health->ipi_data_nonempty_hits++;
        }

      /* The event FIFO carries the sync events the IPI derives from the VOP
       * timing.  It is the discriminator for "the controller never saw a line
       * boundary": with pixels piling up in ipi_data while this FIFO stays
       * empty, no packet is ever built and nothing downstream can start. */

      rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                        DSI2_OBS_FIFO_SEL_IPI_EVENT);
      up_udelay(2);
      st = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS);
      cnt = (st & DSI2_OBS_FIFO_WORD_CNT_MASK) >>
            DSI2_OBS_FIFO_WORD_CNT_SHIFT;
      if (cnt > health->ipi_event_max)
        {
          health->ipi_event_max = cnt;
        }

      if ((st & DSI2_OBS_FIFO_EMPTY) == 0 || cnt != 0)
        {
          health->ipi_event_ever = true;
        }

      rk3576_dsi_putreg(base, RK3576_DSI2_OBS_FIFO_STATUS_SEL,
                        DSI2_OBS_FIFO_SEL_PHY_TXHS);
      up_udelay(2);
      st = rk3576_dsi_getreg(base, RK3576_DSI2_OBS_FIFO_STATUS);
      cnt = (st & DSI2_OBS_FIFO_WORD_CNT_MASK) >>
            DSI2_OBS_FIFO_WORD_CNT_SHIFT;
      if (cnt > health->phy_txhs_max)
        {
          health->phy_txhs_max = cnt;
        }

      if ((st & DSI2_OBS_FIFO_EMPTY) == 0 || cnt != 0)
        {
          health->phy_txhs_ever = true;
          health->phy_txhs_nonempty_hits++;
        }

      /* A TX FIFO that repeatedly reports full/almost-full while the peak
       * level sits at the FIFO depth is the PHY-not-draining signature (the
       * IPI has data but nobody is taking it), which is a different fault
       * from "the IPI never received pixels at all". */

      if (st & (DSI2_OBS_FIFO_FULL | DSI2_OBS_FIFO_ALMOST_FULL))
        {
          health->phy_txhs_full_hits++;
        }

      up_udelay(30);
    }

  /* --- Fine lane-duty measurement (see RK3576_DSI_DUTY_SAMPLES).
   *
   * Counts the samples in which the data lanes are OUT of LP-11.  Read it as
   * "how much of the line the DSI keeps the lanes busy", a stable per-mode
   * signature (burst 88-93%, non-burst 98% on this board), NOT as the packet
   * size -- see the note above the sample count for why the two are not the
   * same measurement.
   */

  for (n = 0; n < RK3576_DSI_DUTY_SAMPLES; n++)
    {
      uint32_t st = rk3576_dsi_getreg(base, RK3576_DSI2_PHY_STATUS);

      health->duty_total++;

      if ((st & (DSI2_PHY_STATUS_PHY_L0_STOPSTATE |
                 DSI2_PHY_STATUS_PHY_L1_STOPSTATE |
                 DSI2_PHY_STATUS_PHY_L2_STOPSTATE |
                 DSI2_PHY_STATUS_PHY_L3_STOPSTATE)) !=
          (DSI2_PHY_STATUS_PHY_L0_STOPSTATE |
           DSI2_PHY_STATUS_PHY_L1_STOPSTATE |
           DSI2_PHY_STATUS_PHY_L2_STOPSTATE |
           DSI2_PHY_STATUS_PHY_L3_STOPSTATE))
        {
          health->duty_hits++;
        }

      up_udelay(3 + ((n * 7u) % 19u));
    }

  /* Read the four FSMs along the video TX chain (ipi_vid -> sys_main ->
   * sys_pkt_build -> phy_tx_ready).  Knowing WHICH stage stopped is what turns
   * "the video path is stalled" into an actionable finding: the first stage
   * that is not advancing is the one that is wedged, and the FIFO/lane
   * evidence elsewhere in this snapshot then says whether its input was ready.
   *
   * Reported as (state, stuck, saturated): a counter saturated at 0xffff
   * together with the stuck bit is the hardware's own "I am going in circles"
   * signature.
   */

  {
    static const uint8_t fsm_sel[4] = {
      DSI2_OBS_FSM_SEL_IPI_VID,
      DSI2_OBS_FSM_SEL_SYS_MAIN,
      DSI2_OBS_FSM_SEL_SYS_PKT_BUILD,
      DSI2_OBS_FSM_SEL_PHY_TX_READY,
    };
    int k;

    for (k = 0; k < 4; k++)
      {
        uint32_t v = rk3576_dsi_obs_fsm(base, fsm_sel[k], NULL);

        health->fsm_raw[k] = v;
        health->fsm_state[k] = v & DSI2_OBS_FSM_CUR_STATE_MASK;
        health->fsm_stuck[k] = (v & DSI2_OBS_FSM_STUCK) != 0;
        health->fsm_saturated[k] =
            ((v & DSI2_OBS_FSM_CNT_MASK) >> DSI2_OBS_FSM_CNT_SHIFT) == 0xffffu;
      }
  }

  return OK;
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_get_cmd_probe
 *
 * Description:
 *   Hand back the accumulated result of the phy_tx_ready control experiment
 *   (see the prototype's comment in rk3576_mipi_dsi.h for why it matters).
 *
 * Input Parameters:
 *   probe - Destination snapshot (must not be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

void rk3576_mipi_dsi_get_cmd_probe(FAR struct rk3576_dsi_cmd_probe_s *probe)
{
  struct rk3576_dsi_s *priv = &g_dsi;

  if (probe == NULL)
    {
      return;
    }

  memset(probe, 0, sizeof(*probe));

  if (!priv->initialized)
    {
      return;
    }

  nxmutex_lock(&priv->lock);

  probe->cmds = priv->cmd_probe_cmds;
  probe->txr_max = priv->cmd_probe_txr_max;
  probe->txr_prev_max = priv->cmd_probe_txr_prev_max;
  probe->txr_noninit = priv->cmd_probe_txr_noninit;
  probe->txr_prev_noninit = priv->cmd_probe_txr_prev_noninit;

  nxmutex_unlock(&priv->lock);
}

/****************************************************************************
 * Name: rk3576_mipi_dsi_set_link_mode
 *
 * Description:
 *   Update the stored video-mode / clock-lane configuration for the NEXT
 *   rk3576_mipi_dsi_enable_video().  No register is written here; see the
 *   prototype's comment in rk3576_mipi_dsi.h for why the change must be
 *   applied through a disable/enable cycle instead of being poked directly.
 *
 ****************************************************************************/

void rk3576_mipi_dsi_set_link_mode(FAR struct mipi_dsi_host *host,
                                   uint8_t video_mode, bool continuous_clk)
{
  struct rk3576_dsi_s *priv = (struct rk3576_dsi_s *)host;

  DEBUGASSERT(priv != NULL);

  if (!priv->initialized)
    {
      return;
    }

  /* Refuse to touch the configuration while the stream is live: everything
   * derived from these two fields is programmed by enable_video(), so
   * half-applying them would leave the controller describing one mode while
   * transmitting another.  Callers must disable video first. */

  if (priv->mode == RK3576_DSI_MODE_VIDEO)
    {
      syslog(LOG_WARNING,
             "dsi: set_link_mode refused while in Video mode -- disable "
             "video first (the caller's disable/enable cycle applies the "
             "change)\n");
      return;
    }

  priv->cfg.video_mode = video_mode;
  priv->cfg.continuous_clk = continuous_clk;

  syslog(LOG_INFO,
         "dsi: link mode staged: video_mode=%u continuous_clk=%u (applied "
         "by the next enable_video)\n",
         (unsigned)video_mode, (unsigned)continuous_clk);
}

void rk3576_mipi_dsi_set_eotp(FAR struct mipi_dsi_host *host, bool enable)
{
  struct rk3576_dsi_s *priv = (struct rk3576_dsi_s *)host;

  DEBUGASSERT(priv != NULL);

  if (!priv->initialized)
    {
      return;
    }

  priv->cfg.eotp = enable;

  syslog(LOG_INFO,
         "dsi: EoTp staged: %s (written by the next enable_video)\n",
         enable ? "TRANSMITTED (eotp_tx_en=1)" : "omitted (eotp_tx_en=0)");
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
        mode_status = rk3576_dsi_getreg(priv->base, RK3576_DSI2_MODE_STATUS) &
                      0x7u;

        if (mode_status == DSI2_MODE_COMMAND &&
            (core_status & idle_mask) == 0)
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

        mode_status = rk3576_dsi_getreg(priv->base,
                                        RK3576_DSI2_MODE_STATUS) & 0x7u;

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
