/****************************************************************************
 * chips/rk3576/rk3576_mipi_dsi.h
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
 * RK3576 MIPI DSI-2 host controller driver public interface.
 *
 * Implements the NuttX generic MIPI DSI framework (struct mipi_dsi_host /
 * struct mipi_dsi_host_ops) on top of the RK3576 DSI-2 controller.  The
 * board layer calls rk3576_mipi_dsi_initialize() to obtain the host, then
 * optionally registers it with mipi_dsi_host_register() so that LCD panel
 * drivers can bind to it via mipi_dsi_device.
 *
 * The DSI driver owns the whole link lifecycle, including the DCPHY: board
 * code never talks to the PHY directly.  rk3576_mipi_dsi_initialize()
 * brings up the DCPHY and enters Command mode, so the panel DCS init
 * sequence can be transmitted over the (live) D-PHY lanes immediately.
 * rk3576_mipi_dsi_enable_video() then programs the IPI (Image Pixel
 * Interface) timing and transitions the host to Video mode, where pixel
 * data from the VOP is streamed to the panel.
 *
 * Command-mode transfer (DCS init / generic read-write) is handled through
 * the DSI-2 Command Interface (CRI) register bank and is usable in both
 * Command and Video operating states.
 *
 * The VOP (video processor) that feeds the IPI is outside the scope of
 * this driver and is wired up by a separate framebuffer/VOP driver.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/video/mipi_dsi.h>

#ifdef CONFIG_RK3576_MIPI_DSI

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* DSI-2 host one-time configuration (passed to
 * rk3576_mipi_dsi_initialize()).  These describe the PHY/link and are
 * fixed for the life of the driver instance.
 */

struct rk3576_dsi_config
{
  uint8_t lanes;      /* D-PHY data lanes in use (1..4). */
  uint8_t format;     /* MIPI_DSI_FMT_* pixel format (RGB888/666/565). */
  uint8_t video_mode; /* DSI2_VID_MODE_* video-mode packet type. */
  bool continuous_clk; /* true = clock lane stays in HS (continuous) for
                         * panels that keep HSCM across the whole frame.
                         * ILI9881D is NON-continuous: its clock lane
                         * returns to LP-11 after every HS burst (Table 46
                         * THS-EXIT), so it must be false for that panel. */
  uint32_t hs_rate;   /* Requested lane high-speed data rate in Hz
                       * (80 Mbps .. 2.5 Gbps). */
  bool eotp;          /* true = transmit EoTp at the end of each HS burst
                       * (DSI2_DSI_GENERAL_CFG.eotp_tx_en).
                       *
                       * The reference value for THIS panel IC is FALSE:
                       * Rockchip's own DTS for an ILI9881D panel sets
                       * MIPI_DSI_MODE_EOT_PACKET, which Linux documents as
                       * "disable EoT packets in HS mode".  Boards that do
                       * not set it (the mainline sync-pulse ILI9881C panels)
                       * get EoTp enabled.  Since EoTp is optional in D-PHY
                       * and changes what a receiver sees at the end of every
                       * HS burst, both values are worth having.
                       *
                       * Zero-initialised rk3576_dsi_config therefore means
                       * "no EoTp", matching Rockchip. */
};

/* Video-mode timing, programmed into the DSI-2 IPI timing registers by
 * rk3576_mipi_dsi_enable_video().  Naming follows drm_display_mode for
 * consistency with the VOP integration.
 */

struct rk3576_dsi_video_timing
{
  uint32_t hactive;      /* Horizontal active pixels. */
  uint32_t hfront_porch; /* Horizontal front porch (HFP). */
  uint32_t hback_porch;  /* Horizontal back porch (HBP). */
  uint32_t hsync_len;    /* Horizontal sync width (HSA). */
  uint32_t vactive;      /* Vertical active lines. */
  uint32_t vfront_porch; /* Vertical front porch (VFP). */
  uint32_t vback_porch;  /* Vertical back porch (VBP). */
  uint32_t vsync_len;    /* Vertical sync width (VSA). */
  uint32_t pixel_clock;  /* Pixel clock in Hz. */
};

/* Compact, repeatable health snapshot of the DSI video path.
 *
 * Unlike rk3576_mipi_dsi_dump_video_status() (a verbose one-shot bring-up
 * dump), this is cheap enough to poll periodically, so the caller can detect
 * THAT the video stream died and WHEN.  That matters because a DSI panel
 * which lights up and then fades away is the signature of a pixel stream
 * that stopped: an LCD that is not refreshed decays, fastest in the corners.
 */

struct rk3576_dsi_health_s
{
  uint32_t mode_status;     /* DSI2_MODE_STATUS (3 = video mode). */
  uint32_t vid_tx_cfg;      /* Live DSI2_DSI_VID_TX_CFG -- the ACTUAL video
                             * transmission mode ([1:0] vid_mode_type).
                             * Reported so an observation can prove which mode
                             * was in force while it was taken: a "mode
                             * comparison" where every entry silently kept the
                             * previous mode is worse than no comparison at
                             * all, because it looks like evidence. */
  uint32_t phy_clk_cfg;     /* Live DSI2_PHY_CLK_CFG -- bit0 is the ACTUAL
                             * clock-lane type (0 = continuous).  Same reason
                             * as vid_tx_cfg. */
  uint32_t core_status;     /* Raw CORE_STATUS: ipi_busy[8] etc. */
  uint32_t int_st_to;       /* Timeout flags latched SINCE THE PREVIOUS CALL
                             * (INT_ST_TO is read-clear).  err_hstx = "the HS
                             * burst never finished". */
  uint32_t ipi_data_max;    /* Peak ipi_data FIFO level over the sample. */
  uint32_t ipi_data_nonempty_hits; /* Samples in which ipi_data was NOT empty.
                               * Divide by the number of samples to get the
                               * fraction of the observation window during
                               * which the IPI still held unconsumed pixels:
                               * near 0 = the packet builder keeps up with the
                               * source, near the maximum = the pixels are
                               * piling up (the payload is not being turned
                               * into line packets). */
  uint32_t phy_txhs_nonempty_hits; /* Same, for the PHY TX FIFO.  In a HEALTHY
                               * burst this is expected to be HIGH (the IPI
                               * fills the FIFO faster than the link drains it,
                               * which is the point of the FIFO), so read it
                               * together with phy_txhs_full_hits: non-empty
                               * most of the time AND full most of the time is
                               * the "nobody is draining" signature, whereas
                               * non-empty often with full rarely is normal
                               * flow. */
  uint32_t ipi_event_max;   /* Peak ipi_event FIFO level over the sample.  The
                             * event FIFO carries the per-line/per-frame sync
                             * events the IPI derives from the VOP timing; a
                             * permanently empty event FIFO while pixels ARE
                             * accumulating means the controller never saw a
                             * line boundary and therefore never built a
                             * packet. */
  bool ipi_event_ever;
  uint32_t phy_txhs_max;    /* Peak phy_txhs FIFO level over the sample. */
  uint32_t phy_txhs_full_hits; /* Samples that saw the TX FIFO full/almost
                                * full.  Combined with phy_txhs_max this is
                                * the "the PHY is not draining" signature. */
  uint32_t phy_status;        /* Last DSI2_PHY_STATUS sample. */
  uint32_t data_hs_hits;      /* Of the 16 samples, how many had at least one
                               * data lane OUT of LP-11 (= transmitting HS).
                               * 0 = the data lanes never transmitted. */
  uint32_t clk_hs_hits;       /* Same, for the clock lane. */
  uint32_t duty_hits;         /* Of the DUTY samples, how many found the
                               * data lanes OUT of LP-11 (HS). */
  uint32_t duty_total;        /* Number of duty samples taken. */
  uint32_t fsm_state[4];      /* Current state of the four FSMs along the
                               * video TX chain, in this order:
                               *   [0] ipi_vid
                               *   [1] sys_main
                               *   [2] sys_pkt_build
                               *   [3] phy_tx_ready
                               * The chain runs 0 -> 3, so comparing them
                               * localises a stall: the first FSM that is not
                               * making progress is the stage that is stuck. */
  bool fsm_stuck[4];          /* Per-FSM "stuck" flag (TRM bit5).
                               *
                               * CAUTION: this flag being set does NOT mean
                               * the FSM is wedged.  It has been observed set
                               * on ipi_vid while the video path was measurably
                               * healthy at every other level (lane duty at the
                               * expected burst fraction, both FIFOs cycling,
                               * the two-sided VOP-off experiment passing, no
                               * timeout latched) and clear in at least one
                               * state that was NOT healthy.  Treat it as
                               * "the FSM repeated a state", nothing more. */
  bool fsm_saturated[4];      /* Per-FSM counter pinned at 0xffff. */
  uint32_t fsm_raw[4];        /* RAW DSI2_OBS_FSM_STATUS words for the same
                               * four FSMs, in the same order.  Kept next to
                               * the decoded fields because the decode depends
                               * on the TRM field layout: a decoding mistake
                               * silently turns a wedged FSM into "idle at
                               * INIT" (that exact mistake was made once with
                               * CUR_STATE_SHIFT=8 and cost several rounds).
                               * The raw word can always be re-decoded later:
                               * [31:16] cnt, [12:8] previous_state, [5]
                               * stuck, [4:0] current_state. */
  bool ipi_data_ever;       /* The ipi_data FIFO held something. */
  bool phy_txhs_ever;       /* The phy_txhs FIFO held something. */
  bool video_mode;          /* mode_status == Video. */
  bool ipi_busy;            /* Pixels are reaching the IPI. */
  bool ipi_fifos_not_empty; /* The IPI still holds packets. */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mipi_dsi_initialize
 *
 * Description:
 *   Initialize the RK3576 DSI-2 host controller and return the host to be
 *   registered with the NuttX MIPI DSI framework.
 *
 *   The RK3576 has a single DSI-2 host, so the driver is a singleton.
 *   The DCPHY must have been initialized beforehand (rk3576_dcphy_init()).
 *
 * Input Parameters:
 *   config - Link/PHY configuration (lanes, format, video mode, hs_rate).
 *            Must not be NULL.
 *
 * Returned Value:
 *   A non-NULL struct mipi_dsi_host * on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct mipi_dsi_host *
rk3576_mipi_dsi_initialize(FAR const struct rk3576_dsi_config *config);

/****************************************************************************
 * Name: rk3576_mipi_dsi_enable_video
 *
 * Description:
 *   Program the DSI-2 controller for Video mode with the given panel
 *   timing, then power on the DCPHY and switch the host into Video mode.
 *   This must be called after the DCS init sequence (if any) has been
 *   sent and the panel is ready to receive pixel data.
 *
 * Input Parameters:
 *   host   - DSI host returned by rk3576_mipi_dsi_initialize().
 *   timing - Panel video timing.  Must not be NULL.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mipi_dsi_enable_video(
    FAR struct mipi_dsi_host *host,
    FAR const struct rk3576_dsi_video_timing *timing);

/****************************************************************************
 * Name: rk3576_mipi_dsi_disable_video
 *
 * Description:
 *   Put the DSI-2 host back into Idle mode and power off the DCPHY.
 *
 * Input Parameters:
 *   host - DSI host returned by rk3576_mipi_dsi_initialize().
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mipi_dsi_disable_video(FAR struct mipi_dsi_host *host);

/****************************************************************************
 * Name: rk3576_mipi_dsi_update_pixel_clock
 *
 * Description:
 *   Re-derive the IPI horizontal timing (HSA/HBP/HACT/HLINE) and
 *   PHY_IPI_RATIO from the VOP's REAL pixel clock, once the CRU divider
 *   chain has settled.
 *
 *   The board passes a pixel clock to enable_video(), but the CRU can only
 *   select an integer divider from the parent PLL, and the divider search
 *   picks the largest divisor whose output is still <= the request.  So the
 *   achieved dclk_vp0 can be several percent BELOW the request, and then the
 *   IPI horizontal timing and PHY_IPI_RATIO are computed against a clock the
 *   pixel stream never runs at (the controller would judge "one line ended"
 *   at the wrong instant and mis-size its CDC ratio).
 *
 *   The board therefore requests a value the CRU hits EXACTLY (see
 *   KICKPI_K7_MIPI_DSI_PIXCLK), and rk3576_vop_enable_clocks() logs a
 *   warning if the achieved rate still differs from the request.  This
 *   function remains the escape hatch for the case where an exact request is
 *   impossible.
 *
 *   NOT called from rk3576_vop_enable_clocks() any more: doing so rewrote the
 *   horizontal timing and the CDC ratio of the pixel datapath asynchronously
 *   WHILE the stream was live, which is a race (see the long note in that
 *   function).  If it must be called, call it before the host enters Video
 *   mode.
 *
 *   Safe to call at any time once the host is in Video mode: the IPI timing
 *   registers are plain RW in manual mode and take effect without leaving
 *   Video mode.
 *
 * Input Parameters:
 *   pixel_clock_hz - Actual VOP pixel clock (crtc clock) in Hz.
 *
 * Returned Value:
 *   OK on success; -EPERM if the host is not in Video mode; -EINVAL on a
 *   bad clock or timing overflow.
 *
 ****************************************************************************/

int rk3576_mipi_dsi_update_pixel_clock(uint32_t pixel_clock_hz);

/* What the controller says about the IPI timing: the MANUAL registers we wrote,
 * and the read-only AUTO registers the controller fills when it is allowed to
 * MEASURE the incoming stream instead. */

struct rk3576_dsi_ipi_view_s
{
  uint32_t hsa_man;
  uint32_t hbp_man;
  uint32_t hact_man;
  uint32_t hline_man;
  uint32_t hsa_auto;
  uint32_t hbp_auto;
  uint32_t hact_auto;
  uint32_t hline_auto;
  uint32_t vsa_auto;
  uint32_t vbp_auto;
  uint32_t vact_auto;
  uint32_t vfp_auto;
  uint32_t manual_mode;   /* DSI2_MANUAL_MODE_CFG.manual_mode_en */
  uint32_t mode_status;   /* DSI2_MODE_STATUS (3 = video) */
  uint32_t vid_tx_cfg;    /* DSI2_DSI_VID_TX_CFG */
  uint32_t phy_ipi_ratio; /* DSI2_PHY_IPI_RATIO_MAN_CFG */
};

/****************************************************************************
 * Name: rk3576_mipi_dsi_set_manual_mode
 *
 * Description:
 *   Select HOW the IPI derives its line/frame timing:
 *
 *     1 (manual) - use the register bank (DSI2_IPI_VID_*_MAN_CFG).  What this
 *                  driver has always done, because in Command mode there is
 *                  no frame for the controller to measure.
 *     0 (auto)   - per TRM 18.4.3, the timing "parameters are automatically
 *                  calculated by measuring times and lines in the NEXT
 *                  FRAME".  That only has a meaning while a pixel stream is
 *                  running, which is exactly the situation a Video-mode host
 *                  is in.
 *
 *   Why this is worth having as a callable: it is the only way to make the
 *   controller declare what IT measures on the IPI (see
 *   rk3576_mipi_dsi_read_ipi_view()), and a comparison of the two modes is the
 *   one experiment that tests the units and the values of the manual timing
 *   registers against hardware instead of against a formula.
 *
 *   Changing it does not by itself leave Video mode, but the auto path needs
 *   frames to measure, so the caller must wait a few frame periods before
 *   reading the AUTO registers.
 *
 * Input Parameters:
 *   enable - true: manual (register bank), false: auto (measure the stream).
 *
 * Returned Value:
 *   OK on success; -ENODEV before rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_set_manual_mode(bool enable);

/****************************************************************************
 * Name: rk3576_mipi_dsi_read_ipi_view
 *
 * Description:
 *   Read back what the controller believes about the IPI timing: the MANUAL
 *   registers (our own values, as a round-trip check), the AUTO registers (the
 *   controller's MEASUREMENT of the incoming stream -- zero in manual mode,
 *   which is not an error), and the three status words that say which mode is
 *   in force while the reading was taken.
 *
 * Input Parameters:
 *   view - Destination snapshot (must not be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_read_ipi_view(FAR struct rk3576_dsi_ipi_view_s *view);

/****************************************************************************
 * Name: rk3576_mipi_dsi_dump_video_status
 *
 * Description:
 *   Bring-up diagnostic (read-only): sample the DSI IPI receive path while
 *   the VOP is scanning, to answer whether pixels physically reach the DSI
 *   IPI.  Must be called AFTER rk3576_vop_initialize() has started the
 *   pixel stream (the enable_video() dump runs before the VOP is up, so its
 *   empty ipi_data FIFO / INIT ipi_vid_fsm are expected, not diagnostic).
 *
 *   A positive ipi_data FIFO count or ipi_busy=1 proves the pixel stream
 *   reached the IPI (break is downstream in the PHY send path); all-zero
 *   again proves the break is upstream (VOP -> DSI physical link).
 *
 ****************************************************************************/

void rk3576_mipi_dsi_dump_video_status(void);

/****************************************************************************
 * Name: rk3576_mipi_dsi_get_health
 *
 * Description:
 *   Take a compact, repeatable snapshot of the DSI video path for polling.
 *   Answers "did the pixel stream die, and when?", which the one-shot
 *   dump_video_status() cannot.  Reading it consumes INT_ST_TO (it is
 *   read-clear), so each latched timeout is reported exactly once.
 *
 * Input Parameters:
 *   health - Destination snapshot (must not be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_get_health(FAR struct rk3576_dsi_health_s *health);

/****************************************************************************
 * Name: rk3576_mipi_dsi_read_to_status
 *
 * Description:
 *   Read (and thereby clear) DSI2_INT_ST_TO, the read-clear timeout latch.
 *
 *   Same bits as rk3576_dsi_health_s.int_st_to, but callable on its own so a
 *   caller can SAMPLE IT REPEATEDLY INSIDE ONE OBSERVATION WINDOW.  That is
 *   what turns "a timeout happened at some point in the last 250 ms" into a
 *   RATE, and the rate is what separates three very different situations:
 *
 *     fires on essentially every read  -> the HS transmission times out
 *                                         continuously (a packet that can
 *                                         never complete)
 *     fires in a minority of reads     -> occasional event, e.g. one per
 *                                         stream entry or per re-arm
 *     never fires                      -> whatever is wrong is not reported
 *                                         by the controller's own timeout
 *
 * Returned Value:
 *   DSI2_INT_ST_TO, or 0 before rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_read_to_status(void);

/****************************************************************************
 * Name: rk3576_mipi_dsi_obs_fsm
 *
 * Description:
 *   Read one of the six debug FSMs (DSI2_OBS_FSM_SEL_*), raw.
 *
 *   The health snapshot already reports the four video-chain FSMs once per
 *   window, which is enough to say WHICH stage looks stalled but not whether
 *   that stage is actually frozen.  A short ladder of raw samples does: a
 *   state field that varies over ~20 ms is a cycling FSM, while eight identical
 *   readings are a frozen one.  That distinction matters here because the
 *   hardware's own 'stuck' bit is latched and therefore keeps reading 1 even
 *   after the FSM has moved on.
 *
 * Input Parameters:
 *   sel - DSI2_OBS_FSM_SEL_*.
 *
 * Returned Value:
 *   DSI2_OBS_FSM_STATUS for the requested FSM, or 0 before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_obs_fsm(uint32_t sel);

/****************************************************************************
 * Name: rk3576_mipi_dsi_obs_fifo
 *
 * Description:
 *   Read DSI2_OBS_FIFO_STATUS for one of the six debug FIFOs (the
 *   DSI2_OBS_FIFO_SEL_* selectors), returning the raw word: the word count in
 *   DSI2_OBS_FIFO_WORD_CNT_SHIFT/MASK and the empty/half/full flags below it.
 *
 *   Unlike the peak values inside rk3576_dsi_health_s, this samples ONE
 *   instant, so a caller can build a LADDER of readings (e.g. every 10 ms) and
 *   watch whether a level decays or stands still.  That distinction is not
 *   available from a peak: an input FIFO whose peak looks modest can still be
 *   frozen solid, and a frozen FIFO plus an idle packet builder is the
 *   signature of "the pixels are never turned into line packets".
 *
 *   The selector needs a CDC before the observed value appears, so the helper
 *   waits a couple of microseconds after selecting, exactly as the FSM
 *   observation helper does.
 *
 * Input Parameters:
 *   sel - DSI2_OBS_FIFO_SEL_*.
 *
 * Returned Value:
 *   DSI2_OBS_FIFO_STATUS for the requested FIFO, or 0 before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_obs_fifo(uint32_t sel);

/* Observation-port selectors used by the two helpers above and by
 * rk3576_mipi_dsi_read_to_status().  Re-exported here so a board-level probe
 * can name a FIFO or an FSM without including the header that carries the
 * register offsets -- the selectors are part of the controller's *debug*
 * interface, not of any board's register knowledge.  Values are the TRM's
 * DSI2_OBS_FIFO_STATUS_SEL / DSI2_OBS_FSM_STATUS_SEL encodings. */

#define RK3576_DSI_OBS_FIFO_CMD_RD_PLD 0x0
#define RK3576_DSI_OBS_FIFO_CMD_WR_HDR 0x1
#define RK3576_DSI_OBS_FIFO_CMD_WR_PLD 0x2
#define RK3576_DSI_OBS_FIFO_IPI_DATA   0x3
#define RK3576_DSI_OBS_FIFO_IPI_EVENT  0x4
#define RK3576_DSI_OBS_FIFO_PHY_TXHS   0x5

/* Bit layout of the word rk3576_mipi_dsi_obs_fifo() returns: the selected
 * FIFO's CURRENT word count in the high half, its level flags in the low
 * bits.  The count is what matters here -- a peak hides a frozen level, a
 * single reading shows it. */

#define RK3576_DSI_OBS_FIFO_WORD_CNT_SHIFT 16
#define RK3576_DSI_OBS_FIFO_WORD_CNT_MASK  (0xffffu << 16)
#define RK3576_DSI_OBS_FIFO_EMPTY          (1u << 0)
#define RK3576_DSI_OBS_FIFO_FULL           (1u << 4)

#define RK3576_DSI_OBS_FSM_IPI_VID     0x0
#define RK3576_DSI_OBS_FSM_SYS_MAIN    0x2
#define RK3576_DSI_OBS_FSM_SYS_PKT     0x4
#define RK3576_DSI_OBS_FSM_PHY_TX_READY 0x5

/* Bit layout of the word rk3576_mipi_dsi_obs_fsm() returns (TRM DSI2_OBS_FSM_
 * STATUS): the CURRENT state in the low 5 bits, the PREVIOUS state in bits
 * 12:8, and -- in bits 31:16 -- how many clock cycles the FSM has been in that
 * state.
 *
 * The cycle count is the field that matters, and it is the one this project
 * has never looked at: it RESETS every time the state changes, so a ladder of
 * counts that varies wildly proves the FSM is cycling, while a count that only
 * grows proves it is frozen.  The 'stuck' bit below cannot answer that -- it
 * is latched, so it keeps reading 1 long after the FSM started moving again,
 * which is why twelve windows of "iv=08!" said nothing at all.
 *
 * PREV_STATE_SHIFT is 8 with a 5-bit mask.  It is NOT (1 << 8) | (0x1f << 3):
 * bits 7:6 are reserved, so the mask must be shifted, not the field width. */

#define RK3576_DSI_OBS_FSM_CNT_SHIFT     16
#define RK3576_DSI_OBS_FSM_CNT_MASK      (0xffffu << 16)
#define RK3576_DSI_OBS_FSM_PREV_SHIFT    8
#define RK3576_DSI_OBS_FSM_PREV_MASK     (0x1fu << 8)
#define RK3576_DSI_OBS_FSM_STUCK         (1u << 5)
#define RK3576_DSI_OBS_FSM_CUR_MASK      0x1fu
#define RK3576_DSI_OBS_FSM_CUR_SHIFT     0

struct rk3576_dsi_int_latches_s
{
  uint32_t main;  /* DSI2_INT_ST_MAIN: one summary bit per error group. */
  uint32_t ipi;   /* DSI2_INT_ST_IPI: err_ipi_dtype[0] is the telling one. */
  uint32_t fifo;  /* DSI2_INT_ST_FIFO. */
  uint32_t phy;   /* DSI2_INT_ST_PHY. */
  uint32_t ack;   /* DSI2_INT_ST_ACK. */
  uint32_t to;    /* DSI2_INT_ST_TO: HS/LP timeout flags. */
};

/* DSI2_INT_ST_IPI bits.  err_ipi_dtype is the one worth naming: the TRM says
 * it fires when "no DSI-2 data type is a direct match for the choice made by
 * the pair ipi_format and ipi_depth", after which the controller SILENTLY
 * falls back to Packed Pixel Stream 24-bit.  That is a pixel-format fault that
 * produces perfectly legal-looking traffic, so it can only be seen by reading
 * this latch -- and the driver never did. */

#define RK3576_DSI_INT_IPI_ERR_DTYPE        (1u << 0)
#define RK3576_DSI_INT_IPI_ERR_CMD_TIME     (1u << 1)
#define RK3576_DSI_INT_IPI_ERR_CMD_OVFL     (1u << 2)

/* DSI2_INT_ST_MAIN summary bits (bit n = "group n latched something"). */

#define RK3576_DSI_INT_MAIN_PHY             (1u << 0)
#define RK3576_DSI_INT_MAIN_TO              (1u << 1)
#define RK3576_DSI_INT_MAIN_ACK             (1u << 2)
#define RK3576_DSI_INT_MAIN_IPI             (1u << 3)
#define RK3576_DSI_INT_MAIN_FIFO            (1u << 4)
#define RK3576_DSI_INT_MAIN_PRI             (1u << 5)
#define RK3576_DSI_INT_MAIN_CRI             (1u << 6)

/****************************************************************************
 * Name: rk3576_mipi_dsi_read_int_latches
 *
 * Description:
 *   Read all six DSI2_INT_ST_* groups at once.  Every one of them is
 *   read-clear, so this both reports and consumes: a caller must treat the
 *   result as "what latched since the previous call".
 *
 *   Why all six and not just the timeout group: the video path's failure modes
 *   are reported in different groups, and the interesting ones are not errors
 *   at all in the usual sense -- err_ipi_dtype means the controller accepted
 *   the configuration but silently substituted a different pixel format, which
 *   is exactly the kind of fault that leaves every "is the link healthy?" test
 *   passing while the picture is wrong.  A caller that only polls timeouts
 *   cannot see it.
 *
 * Input Parameters:
 *   l - Destination snapshot (must not be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

int rk3576_mipi_dsi_read_int_latches(FAR struct rk3576_dsi_int_latches_s *l);

/****************************************************************************
 * Name: rk3576_mipi_dsi_count_lane_episodes
 *
 * Description:
 *   Sample DSI2_PHY_STATUS back-to-back (a tight register loop, no delays) and
 *   count how many times the DATA lanes LEAVE low-power (LP-11) and how many
 *   samples found them out of it.
 *
 *   This is the one measurement that separates the two shapes a video stream
 *   can have on the wire, and the picture cannot distinguish them:
 *
 *     one LP-11 episode per LINE  -> the transmitter emits a burst per line:
 *                                    the panel gets a per-line edge to
 *                                    re-synchronise on, which a panel with no
 *                                    frame memory needs in order to frame a
 *                                    video stream at all
 *     one LP-11 episode per FRAME -> the transmitter starts one long HS burst
 *                                    per frame and never returns to LP between
 *                                    lines: there is no per-line edge, and
 *                                    such a panel receives a continuous HS
 *                                    stream it cannot structure into lines
 *
 *   Read it as a RATIO: the caller knows how many lines elapsed while the loop
 *   ran (the VOP's scan counter is the natural clock for that), so
 *   episodes == lines means per-line, episodes << lines means per-frame.
 *
 *   Note the deliberate asymmetry: ANY data lane out of LP-11 counts as "HS".
 *   That under-counts episodes if the lanes leave HS one at a time, and it can
 *   never invent an episode, so the count is a lower bound -- which is the safe
 *   direction for the conclusion above.
 *
 * Input Parameters:
 *   samples    - Number of PHY_STATUS reads to take (clamped to 1..65535).
 *   hs_samples - Optional out: samples in which the data lanes were NOT all in
 *                LP-11.
 *   clk_hs     - Optional out: samples in which the CLOCK lane was NOT in
 *                LP-11 (a continuous clock lane reads 100%, an inter-burst
 *                clock reads like the data lanes).
 *
 * Returned Value:
 *   Number of LP-11 -> HS transitions observed, or 0 if the driver is not up.
 *
 ****************************************************************************/

uint32_t rk3576_mipi_dsi_count_lane_episodes(uint32_t samples,
                                            FAR uint32_t *hs_samples,
                                            FAR uint32_t *clk_hs);

/****************************************************************************
 * Name: rk3576_mipi_dsi_get_cmd_probe
 *
 * Description:
 *   Report the "is the phy_tx_ready FSM observable at all?" control
 *   experiment.
 *
 *   The motivation: in video mode the driver samples
 *   DSI2_OBS_FSM_SEL_PHY_TX_READY and always reads state 0, which has been
 *   interpreted for many rounds as "the PPI readiness handshake never
 *   starts".  That interpretation is only valid if the same field moves
 *   when this controller DOES transmit successfully -- and the Command-mode
 *   DCS path demonstrably does (the panel latches its init sequence and
 *   answers bus-turnaround reads).
 *
 *   So the driver samples the FSM around every CRI command it sends:
 *
 *     txr_max > 0        -> the field is live; a permanently-0 reading in
 *                           video mode is then a REAL stall, not an artifact.
 *     txr_max == 0 over  -> the field carries no information for this
 *       many commands       controller/firmware, and every earlier
 *                           "phy_tx_ready stuck at INIT" conclusion must be
 *                           discarded.  That is the expensive kind of wrong:
 *                           it aimed the investigation at the Host<->PHY PPI
 *                           handshake and kept it there.
 *
 *   previous_state is reported too: an FSM that entered and left INIT shows
 *   a non-zero previous_state, which is even more robust than catching the
 *   short HS burst while it is in flight.
 *
 * Input Parameters:
 *   probe - Destination snapshot (must not be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a NULL pointer; -ENODEV before
 *   rk3576_mipi_dsi_initialize().
 *
 ****************************************************************************/

struct rk3576_dsi_cmd_probe_s
{
  uint32_t cmds;             /* CRI commands observed since init. */
  uint32_t txr_max;          /* Highest phy_tx_ready current_state seen. */
  uint32_t txr_prev_max;     /* Highest previous_state seen for that FSM. */
  bool txr_noninit;          /* The FSM was caught outside INIT. */
  bool txr_prev_noninit;     /* Its previous_state was ever non-zero. */
};

void rk3576_mipi_dsi_get_cmd_probe(FAR struct rk3576_dsi_cmd_probe_s *probe);

/****************************************************************************
 * Name: rk3576_mipi_dsi_set_link_mode
 *
 * Description:
 *   Change the video transmission mode and/or the clock-lane type used by
 *   rk3576_mipi_dsi_enable_video().
 *
 *   Writes NO hardware register: it only updates the stored configuration, so
 *   it takes effect on the next enable_video().  The caller must therefore
 *   run a disable_video()/enable_video() cycle afterwards.  That is
 *   deliberate:
 *
 *     - the whole video register set (VID_TX_CFG, IPI horizontal timing,
 *       PHY_IPI_RATIO, PHY_CLK_CFG) derives from these fields, so they must be
 *       re-programmed together instead of being poked one at a time;
 *     - Video mode must be entered with the pixel datapath already running
 *       (see the board's bring-up note), and the disable/enable cycle
 *       guarantees exactly that ordering.
 *
 *   The lane rate is deliberately NOT switchable here.  hs_rate is baked into
 *   the DCPHY PLL when the PHY is powered on, and the PHY is brought up once,
 *   in initialize(); changing the rate afterwards would leave the DPHY and the
 *   controller's hstx-cycle registers disagreeing.  Keeping it fixed also
 *   makes a mode/clock-type comparison a clean single-variable experiment.
 *
 *   Refused (with a warning, leaving the configuration unchanged) while the
 *   host is in Video mode: half-applying the change would have the controller
 *   describing one mode while transmitting another.
 *
 * Input Parameters:
 *   host           - DSI host.
 *   video_mode     - DSI2_VID_MODE_* (burst / non-burst sync pulses / events).
 *   continuous_clk - true to keep the clock lane in HS for the whole frame.
 *
 ****************************************************************************/

void rk3576_mipi_dsi_set_link_mode(FAR struct mipi_dsi_host *host,
                                   uint8_t video_mode, bool continuous_clk);

/****************************************************************************
 * Name: rk3576_mipi_dsi_set_eotp
 *
 * Description:
 *   Enable or disable EoTp (End of Transmission) transmission in high speed,
 *   i.e. DSI2_DSI_GENERAL_CFG.eotp_tx_en.
 *
 *   Why this is a runtime knob rather than a constant: it is one of the very
 *   few host-side bits for which a REFERENCE VALUE EXISTS FOR THIS PANEL IC,
 *   and the reference value is not what this driver does.
 *
 *     Rockchip's own DTS for an ILI9881D panel
 *       (arch/arm/boot/dts/rv1126-evb-v10.dtsi, "ilitek,ili9881d"):
 *         dsi,flags = <MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
 *                      MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET>;
 *     and in Linux, MIPI_DSI_MODE_EOT_PACKET is defined as
 *         "disable EoT packets in HS mode"        (drm_mipi_dsi.h, BIT(9))
 *     so their configuration is BURST with EoTp DISABLED.
 *
 *   This driver has always written BTA_EN | EOTP_TX_EN, i.e. EoTp enabled, and
 *   every mode test it has ever run therefore also had EoTp on.  "Burst with
 *   EoTp off" -- the combination Rockchip ships for this panel IC -- has never
 *   been measured on this board.  EoTp is optional in D-PHY but it is a real
 *   difference on the wire (it appends a transmission-end sequence to every HS
 *   burst), and a receiver that does not expect it can mis-frame a stream while
 *   still reporting no CRC/ECC errors.
 *
 *   Takes effect on the next enable_video(), like the mode/clock pairing: the
 *   general config is programmed as part of the link bring-up.
 *
 * Input Parameters:
 *   host   - Host returned by rk3576_mipi_dsi_initialize().
 *   enable - true to transmit EoTp, false to leave it out.
 *
 ****************************************************************************/

void rk3576_mipi_dsi_set_eotp(FAR struct mipi_dsi_host *host, bool enable);

#endif /* CONFIG_RK3576_MIPI_DSI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_MIPI_DSI_H */
