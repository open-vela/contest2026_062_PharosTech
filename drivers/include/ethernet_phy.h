/****************************************************************************
 * drivers/include/ethernet_phy.h
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

#ifndef __DRIVERS_INCLUDE_ETHERNET_PHY_H
#define __DRIVERS_INCLUDE_ETHERNET_PHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Clause 22 register access, supplied by whoever owns the MAC.
 *
 * These two signatures are a *mirror* of the functions a MAC driver exposes
 * for its management bus -- see the matching note in
 * chips/rk3576/rk3576_gmac.h.  The mirror is the price of the two layers not
 * including each other: the board is the only place that sees both, and it
 * assigns the MAC's functions to these fields.  Change one side and the other
 * has to change with it.
 */

struct ethernet_phy_mdio_s
{
  CODE int (*read)(FAR void *ctx, uint8_t phyaddr, uint8_t regaddr,
                   FAR uint16_t *value);
  CODE int (*write)(FAR void *ctx, uint8_t phyaddr, uint8_t regaddr,
                    uint16_t value);
  FAR void *ctx;
};

/* How the PHY's twisted pairs are paired.  Left to the part, its crossover
 * detection decides; forced, it stays one way across a reset.  Forcing MDIX
 * pairs a board the way a switch port is paired, which is what an endpoint
 * needs when its automatic crossover does not advance.
 */

enum ethernet_phy_mdi_e
{
  ETHERNET_PHY_MDI_AUTO = 0,
  ETHERNET_PHY_MDI_MDI,
  ETHERNET_PHY_MDI_MDIX
};

struct ethernet_phy_s;

struct ethernet_phy_ops_s
{
  /* Configure the part after it has been reset and before autonegotiation is
   * started: the register bits that are specific to this part and that a
   * generic clause 22 driver has no name for.
   */

  CODE int (*init)(FAR struct ethernet_phy_s *phy);

  /* Write what the part can do, before the MAC restarts autonegotiation.
   * Deriving it from the part's own capability registers rather than from a
   * constant is what keeps a descriptor from advertising a mode the part
   * cannot enter.
   */

  CODE int (*advertise)(FAR struct ethernet_phy_s *phy);
};

struct ethernet_phy_s
{
  FAR const char *name;
  FAR const struct ethernet_phy_ops_s *ops;

  /* Management bus, filled in by the board. */

  struct ethernet_phy_mdio_s mdio;

  /* The address the part answered at, and one of enum ethernet_phy_mdi_e. */

  uint8_t addr;
  uint8_t mdi_mode;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The two entry points a MAC driver's configuration refers to.
 *
 * Both take `void *` and cast it back to `struct ethernet_phy_s *` inside, on
 * purpose: it lets the chip layer hold plain function pointers in its config
 * without including this header, so neither layer includes the other and only
 * the board -- which sees both -- joins them up.  The parameter is always the
 * `phy` a board passed in its config, never anything else.
 */

int ethernet_phy_init(FAR void *ctx);
int ethernet_phy_advertise(FAR void *ctx);

/* Implementations for boards to point at. */

extern const struct ethernet_phy_ops_s g_mae0621a_ops;

#endif /* __DRIVERS_INCLUDE_ETHERNET_PHY_H */
