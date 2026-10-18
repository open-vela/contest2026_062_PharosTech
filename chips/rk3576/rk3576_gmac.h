/****************************************************************************
 * chips/rk3576/rk3576_gmac.h
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
 * RK3576 Gigabit Ethernet MAC (DWC EQOS) driver.
 *
 * The driver owns everything that belongs to the controller: the CRU clocks,
 * the SDGMAC_GRF RGMII mode registers, the VCCIO RGMII delay lines, the MDIO
 * bus and the PHY state machine, the descriptor rings, and the netdev lower
 * half.  What it does *not* own is what belongs to the board: which pins carry
 * the RGMII signals, and the PHY reset line.  Those come in through the
 * configuration structure below, which the board layer fills in.
 *
 * Two instances, one file, from the first line: GMAC0 and GMAC1 are separate
 * controllers with separate MDIO buses, and nothing but the CRU gate register
 * and the clock tree is shared.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_GMAC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_GMAC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/net/mii.h>

#ifdef CONFIG_RK3576_GMAC

/* Controller instance indices, used as the first argument of
 * rk3576_gmac_initialize().  They index the CRU clock names, the base
 * addresses, the interrupt numbers and the SDGMAC_GRF/VCCIO registers, so
 * they are the controller index and not the port number.
 */

#define RK3576_GMAC0 0
#define RK3576_GMAC1 1

/* How the PHY's MDI pairs are paired: leave it to the PHY's own crossover
 * detection, or force one side.  Forcing is what a board needs when the part's
 * automatic crossover does not advance -- two identical PHYs back to back are
 * the classic case, and so is a partner that never sends link pulses.
 */

enum rk3576_gmac_mdi_e
{
  RK3576_GMAC_MDI_AUTO = 0, /* the PHY decides (PHYCR1 bit 9 = 0)      */
  RK3576_GMAC_MDI_MDI,      /* force MDI   (PHYCR1 bit 9 = 1, bit 8=1) */
  RK3576_GMAC_MDI_MDIX,     /* force MDIX  (PHYCR1 bit 9 = 1, bit 8=0) */
};

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Everything the driver needs to know about the board's port.
 *
 * The RGMII delay taps are board facts that cannot be discovered at run time
 * -- they come from the vendor DTB -- so they are passed in rather than probed
 * and guessed.  The callbacks are the board's half of the bring-up.
 */

struct rk3576_gmac_config_s
{
  /* RGMII delay lines, in 1/8 ns taps.  VCCIO_IOC MISC_CON offset differs
   * per controller *and* per pin group; the board passes the offset of the
   * group it actually muxed.
   */

  uint16_t ioc_offset;
  uint8_t tx_delay;
  uint8_t rx_delay;
  bool tx_delay_ena;
  bool rx_delay_ena;

  /* The port's own MAC address.  Nothing on this board supplies one -- no
   * DTB property, no OTP -- so it is a board decision and it must be in
   * place before the interface is brought up.
   */

  FAR const uint8_t *mac;

  /* The PHY on this port, described the way NuttX describes one: identity,
   * the address range to look in, and -- for parts that have one -- the
   * vendor status register and how to decode it.  See struct phy_desc_s in
   * <nuttx/net/mii.h>, and imx9_enet's BOARD_ENETn_PHY_LIST for the same use.
   *
   * Two fields carry meaning that header does not spell out, and both are
   * used by this board:
   *
   *   id1 == 0 && id2 == 0   the part has no PHY ID registers, so it is
   *                          identified by its status register alone (the
   *                          MAE0621A's register map has no 2 or 3)
   *   status == 0xffff       the part has no vendor status register, so
   *                          clause 22's own registers are read instead
   */

  FAR const struct phy_desc_s *phy;

  /* One of enum rk3576_gmac_mdi_e.  Applied after the PHY is reset and before
   * autonegotiation is restarted, because PHYCR1's MDI bits only take effect
   * across a reset.
   */

  uint8_t mdi_mode;

  /* Board callbacks.  Both are required: without the pads muxed and the PHY
   * out of reset, nothing below works.
   *
   *   pinmux     mux the RGMII pad group (idempotent: called at boot and on
   *              every ifup)
   *   phy_reset  drive PHYRSTB, true = hold in reset, false = release
   */

  CODE int (*pinmux)(void);
  CODE void (*phy_reset)(bool assert);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gmac_initialize
 *
 * Description:
 *   Bring the platform layer up (clocks, SDGMAC_GRF, RGMII delays, controller
 *   reset), verify that the controller answers, probe the PHY over MDIO, and
 *   register the netdev -- but do not start the DMA rings.  Those come up in
 *   ifup, on the thread that runs `ifup eth0`.
 *
 *   Called once per controller, from board_late_initialize().
 *
 * Input Parameters:
 *   intf - RK3576_GMAC0 or RK3576_GMAC1
 *   cfg  - board description of this port (see above).  The driver keeps the
 *          pointer, so the structure must stay valid for the life of the
 *          port: a `static const` in the board file is what it is for.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_gmac_initialize(int intf,
                           FAR const struct rk3576_gmac_config_s *cfg);

#endif /* CONFIG_RK3576_GMAC */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_GMAC_H */
