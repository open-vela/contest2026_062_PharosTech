/****************************************************************************
 * chips/rk3576/rk3576_csi_host.h
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
 * RK3576 MIPI CSI HOST driver -- private interface for the capture path.
 *
 * This is the middle stage of the capture pipeline:
 *
 *   D-PHY RX  ->  CSI HOST  ->  VICAP  ->  DDR
 *
 * The CSI HOST parses the CSI-2 protocol (packet split, header ECC, payload
 * CRC, lane merge) and hands VICAP a parallel pixel stream.  It has no
 * buffers, no DMA and no notion of a frame, so there is nothing here to
 * start or stop per capture: the block is brought up once, and VICAP gates
 * its own capture enable to control the stream.
 *
 * The interface is a plain function API rather than an ops table, matching
 * the other RK3576 blocks (DCPHY, DSI, VOP): the consumer is the VICAP
 * driver in the same chip directory, and there is exactly one instance of
 * each block in a given build.
 *
 * Only CSI HOST0 is supported, because that is the one wired to the DCPHY
 * RX (slave) lanes.  HOST1..HOST4 are fed by the RX-only CSIDPHY0/1, which
 * need a different PHY driver.
 *
 * Interrupts are deliberately NOT used.  The block has no interrupt status
 * register of its own -- the error registers are the status and MSK1/MSK2
 * are the per-bit enables -- and the TRM does not document how ERR1 is
 * cleared (it is marked read-only, which for a level-triggered GIC line
 * would mean an unacknowledgeable interrupt).  Masking every error bit and
 * reading the error registers on demand is both safe and sufficient: the
 * audio-visual signal that matters, "a frame finished", comes from VICAP.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_CSI_HOST_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_CSI_HOST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_MIPI_CSI

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Capture-path configuration for one CSI HOST instance. */

struct rk3576_csi_config
{
  uint8_t host;     /* CSI HOST instance index; only 0 is supported */
  uint8_t lanes;    /* Active data lanes, 1..4 */
  uint32_t hs_rate; /* Lane high-speed data rate in Hz (drives the RX PHY) */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csi_host_initialize
 *
 * Description:
 *   Bring up the CSI HOST and the D-PHY RX lanes that feed it:
 *
 *     1. enable the HOST's APB and interface clocks and release its
 *        presetn,
 *     2. power on the DCPHY RX lanes for the requested lane count and rate,
 *     3. program N_LANES (only writable while the lanes are in the stop
 *        state, which is why the PHY is brought up first),
 *     4. select CSI-2 over D-PHY in CONTROL,
 *     5. mask every error interrupt (see the file comment) and clear the
 *        error state,
 *     6. release CSI2_RESETN.
 *
 *   Exactly one instance may be initialised at a time; a second call
 *   returns -EBUSY.
 *
 * Input Parameters:
 *   config - Host index, lane count and lane rate.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_csi_host_initialize(FAR const struct rk3576_csi_config *config);

/****************************************************************************
 * Name: rk3576_csi_host_uninitialize
 *
 * Description:
 *   Reverse of rk3576_csi_host_initialize(): put the controller back into
 *   reset, mask the error interrupts, power down the RX lanes and release
 *   the clocks.  Safe to call when not initialised.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_csi_host_uninitialize(void);

/****************************************************************************
 * Name: rk3576_csi_host_set_lane_rate
 *
 * Description:
 *   Change the rate the D-PHY RX lanes run at, leaving the controller and
 *   everything else about it alone.
 *
 *   The lane rate is the one thing a capture mode can change that the
 *   receiver cares about but the controller does not: the lane count, the
 *   CSI-2 mode and the error masks are all the same afterwards, and only the
 *   PHY's per-rate timing parameters differ.  So this is a power cycle of the
 *   RX lanes rather than a bring-up, and it exists so that a mode change does
 *   not have to take the whole receive chain down and build it again.
 *
 *   The controller is held in reset across the change, and the new rate is
 *   only accepted while the lanes are in the stop state -- which means the
 *   sensor must not be streaming.  Closing the capture device is what
 *   guarantees that in practice.
 *
 * Input Parameters:
 *   hs_rate - New per-lane high-speed data rate in Hz.  This is twice the
 *             MIPI link frequency, because the link is double data rate.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.  -ENODEV if the
 *   controller has not been initialised.
 *
 ****************************************************************************/

int rk3576_csi_host_set_lane_rate(uint32_t hs_rate);

/****************************************************************************
 * Name: rk3576_csi_host_get_phy_state / _get_err1 / _get_err2
 *
 * Description:
 *   Read the D-PHY lane state and the two error registers.
 *
 *   PHY_STATE is the only place the RX clock lane's stop state is visible
 *   (the DCPHY has no slave clock lane status bit).  A healthy idle link
 *   shows stopstateclk and stopstatedata set and rxactivehs clear.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   The raw register value, or 0 if the driver is not initialised.
 *
 ****************************************************************************/

uint32_t rk3576_csi_host_get_phy_state(void);
uint32_t rk3576_csi_host_get_err1(void);
uint32_t rk3576_csi_host_get_err2(void);

/****************************************************************************
 * Name: rk3576_csi_host_lanes_in_stop_state
 *
 * Description:
 *   Report whether the clock lane and every enabled data lane are in the
 *   D-PHY stop state (LP-11), i.e. the sensor is idle but the link is
 *   electrically alive.  This is the first bring-up checkpoint: it proves
 *   the PHY lanes are powered and the sensor is driving them.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   true when every lane is in the stop state.
 *
 ****************************************************************************/

bool rk3576_csi_host_lanes_in_stop_state(void);

/****************************************************************************
 * Name: rk3576_csi_host_is_receiving
 *
 * Description:
 *   Report whether any data lane is actively receiving high-speed data,
 *   i.e. the sensor is streaming.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   true when at least one data lane reports HS activity.
 *
 ****************************************************************************/

bool rk3576_csi_host_is_receiving(void);

/****************************************************************************
 * Name: rk3576_csi_host_read_header
 *
 * Description:
 *   Read back one of the four most recently received packet headers.
 *   This is the cheapest way to prove the sensor is sending the virtual
 *   channel and data type that VICAP is set up to accept (for example
 *   VC 0 / RAW10 0x2b), without instrumenting the sensor at all.
 *
 * Input Parameters:
 *   index      - 0..3; 0 is the most recent packet, 3 the oldest.
 *   vc         - Receives the virtual channel (may be NULL).
 *   dt         - Receives the CSI-2 data type (may be NULL).
 *   word_count - Receives the payload word count (may be NULL).
 *
 * Returned Value:
 *   OK on success; -EINVAL if index is out of range or the driver is not
 *   initialised.
 *
 ****************************************************************************/

int rk3576_csi_host_read_header(uint8_t index, FAR uint8_t *vc,
                                FAR uint8_t *dt, FAR uint16_t *word_count);

/****************************************************************************
 * Name: rk3576_csi_host_dump
 *
 * Description:
 *   Log the controller version, lane configuration, PHY state, both error
 *   registers and the last four packet headers.  Intended for bring-up
 *   diagnosis and for a board-level sanity command.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_csi_host_dump(void);

#endif /* CONFIG_RK3576_MIPI_CSI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_CSI_HOST_H */
