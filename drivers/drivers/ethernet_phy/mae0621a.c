/****************************************************************************
 * drivers/drivers/ethernet_phy/mae0621a.c
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
 * Maxio MAE0621A gigabit PHY: the part-specific half of a clause 22 driver.
 *
 * What is here is what a MAC driver has no name for: the vendor register
 *pages, the link-down power saving pair that has to stay clear, the MDI
 *pairing bits that only take effect across a reset, and the rule that the
 *advertisement comes from the part's own capability registers rather than from
 *a constant.
 *
 * The generic half -- clause 22's BMSR/LPA/1000BASE-T status, the address
 *walk, and the link state machine -- stays with the MAC, as it does in the
 *rest of the tree.
 *
 * Register references are to the MAE0621A datasheet, v1.1 (2024-8):
 *   8.4.1  BMCR (0x00), 8.4.3 ANAR (0x04), 8.4.8 GBCR (0x09),
 *   8.4.12 GBESR (0x0f), 8.4.14 PHYCR1 (page 0xa43, 0x18),
 *   8.4.19 LCR (page 0xd04, 0x10).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/net/mii.h>

#include <ethernet_phy.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The register that selects a vendor page, and the pages this part has. */

#define MAE0621A_PAGSR    0x1f
#define MAE0621A_PAGE_STD 0x0000
#define MAE0621A_PAGE_PHY 0xa43
#define MAE0621A_PHYCR1   0x18
#define MAE0621A_PHYCR2   0x19

/* PHYCR1 bits.  Bits 7, 6, 2 and 1 are never wanted and are cleared on every
 * write: bit 7 asserts CRS on transmit, bit 6 makes a part strapped to address
 * 0 latch the first non-zero address it sees, and bits 2 and 1 are the
 * link-down power saving pair -- with the PLL off inside it the copper side
 * cannot send or hear a link pulse while MDIO still answers perfectly.
 * Clearing them also repairs a register left holding all ones.
 */

#define MAE0621A_PHYCR1_MDI_MANUAL (1u << 9)
#define MAE0621A_PHYCR1_MDI_MDI    (1u << 8)
#define MAE0621A_PHYCR1_CLEAR      ((1u << 7) | (1u << 6) | (1u << 2) | (1u << 1))

/* Register 9's 1000BASE-T bits are not where register 10's are, which is the
 * kind of pair that gets written from memory and then read wrong: register 9
 * advertises full duplex in bit 9 and half duplex in bit 10, with bits 11 and
 * 12 choosing master/slave by hand; register 10 reports the partner's choice
 * in bits 11 and 10.
 */

#define MAE0621A_CTRL1000_ADV_FULL (1 << 9)
#define MAE0621A_CTRL1000_ADV_HALF (1 << 10)

/* The extended status register (0x0f): what the part says it can do at
 * gigabit.
 */

#define MAE0621A_GBESR_1000T_FULL (1 << 13)
#define MAE0621A_GBESR_1000T_HALF (1 << 12)

/* All four 10/100 modes and the 802.3 selector.  Pause is deliberately not
 * advertised: the MAC's flow control is not programmed either, and offering a
 * capability the MAC will not honour is worse than not having it.
 */

#define MAE0621A_ANAR_10_100                                   \
  (MII_ADVERTISE_10BASETXHALF | MII_ADVERTISE_10BASETXFULL |   \
   MII_ADVERTISE_100BASETXHALF | MII_ADVERTISE_100BASETXFULL | \
   MII_ADVERTISE_8023)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int mae0621a_read(FAR struct ethernet_phy_s *phy, uint8_t regaddr,
                         FAR uint16_t *value)
{
  return phy->mdio.read(phy->mdio.ctx, phy->addr, regaddr, value);
}

static int mae0621a_write(FAR struct ethernet_phy_s *phy, uint8_t regaddr,
                          uint16_t value)
{
  return phy->mdio.write(phy->mdio.ctx, phy->addr, regaddr, value);
}

/****************************************************************************
 * Name: mae0621a_page_read / mae0621a_page_write
 *
 * Description:
 *   Read or write one register in a vendor page: select it through PAGSR,
 *   access the register, and always put the standard register set back -- the
 *   MAC's link poll keeps running and must never find a vendor page selected.
 *
 *   Reads are validated and retried: a page switch is exactly the kind of
 *   access that has come back all-ones on this hardware (the first management
 *   transaction after a reset can fail), and a read that is not checked turns
 *   into a write of 0xffff a moment later.  The part also needs a moment after
 *   PAGSR changes before the page answers.
 *
 ****************************************************************************/

static int mae0621a_page_read(FAR struct ethernet_phy_s *phy, uint16_t page,
                              uint8_t regaddr, FAR uint16_t *value)
{
  int attempt;
  int ret;

  for (attempt = 0; attempt < 3; attempt++)
    {
      if (attempt > 0)
        {
          usleep(10 * 1000);
        }

      ret = mae0621a_write(phy, MAE0621A_PAGSR, page);
      if (ret < 0)
        {
          continue;
        }

      usleep(100);

      ret = mae0621a_read(phy, regaddr, value);

      if (mae0621a_write(phy, MAE0621A_PAGSR, MAE0621A_PAGE_STD) < 0)
        {
          continue;
        }

      if (ret == 0 && *value != 0xffff && *value != 0x0000)
        {
          return 0;
        }
    }

  return -EIO;
}

static int mae0621a_page_write(FAR struct ethernet_phy_s *phy, uint16_t page,
                               uint8_t regaddr, uint16_t value)
{
  int ret;

  ret = mae0621a_write(phy, MAE0621A_PAGSR, page);
  if (ret < 0)
    {
      return ret;
    }

  ret = mae0621a_write(phy, regaddr, value);

  if (mae0621a_write(phy, MAE0621A_PAGSR, MAE0621A_PAGE_STD) < 0)
    {
      return -EIO;
    }

  return ret;
}

/****************************************************************************
 * Name: mae0621a_force_mdi
 *
 * Description:
 *   Force the MDI pairing.  PHYCR1's bits only take effect across a reset, so
 *   this writes them and then software-resets the part; autonegotiation
 *   restarts by itself after a reset, and the MAC has not written the
 *   advertisement yet, so nothing written there is lost.
 *
 ****************************************************************************/

static int mae0621a_force_mdi(FAR struct ethernet_phy_s *phy)
{
  uint16_t cr1 = 0;
  uint16_t bmcr = MII_MCR_RESET;
  uint16_t want;
  int i;
  int ret;

  ret = mae0621a_page_read(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYCR1, &cr1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: PHYCR1 unreadable, MDI mode left alone\n",
             phy->name);
      return ret;
    }

  cr1 &= ~MAE0621A_PHYCR1_CLEAR;
  cr1 |= MAE0621A_PHYCR1_MDI_MANUAL;

  if (phy->mdi_mode == ETHERNET_PHY_MDI_MDI)
    {
      cr1 |= MAE0621A_PHYCR1_MDI_MDI;
    }
  else
    {
      cr1 &= ~MAE0621A_PHYCR1_MDI_MDI;
    }

  want = cr1 & (MAE0621A_PHYCR1_MDI_MANUAL | MAE0621A_PHYCR1_MDI_MDI);

  ret = mae0621a_page_write(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYCR1, cr1);
  if (ret < 0)
    {
      return ret;
    }

  /* Read it back: a write that silently did not land would leave the port
   * paired the old way while the log claimed otherwise.
   */

  ret = mae0621a_page_read(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYCR1, &cr1);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO, "%s: MDI forced to %s (phycr1 0x%04x)\n", phy->name,
         phy->mdi_mode == ETHERNET_PHY_MDI_MDI ? "MDI" : "MDIX", cr1);

  if ((cr1 & (MAE0621A_PHYCR1_MDI_MANUAL | MAE0621A_PHYCR1_MDI_MDI)) != want)
    {
      syslog(LOG_ERR, "%s: PHYCR1 MDI bits did not take\n", phy->name);
      return -EIO;
    }

  ret = mae0621a_write(phy, MII_MCR, bmcr);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < 100; i++)
    {
      ret = mae0621a_read(phy, MII_MCR, &bmcr);
      if (ret < 0)
        {
          return ret;
        }

      if ((bmcr & MII_MCR_RESET) == 0)
        {
          return 0;
        }

      usleep(1000);
    }

  syslog(LOG_ERR, "%s: software reset never completed\n", phy->name);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static int mae0621a_init(FAR struct ethernet_phy_s *phy)
{
  int ret;

  if (phy->mdi_mode != ETHERNET_PHY_MDI_AUTO)
    {
      ret = mae0621a_force_mdi(phy);
      if (ret < 0)
        {
          return ret;
        }
    }
  else
    {
      /* Nothing to force, but the never-wanted bits still have to be clear:
       * a register left holding all ones would enable link-down power saving
       * with the PLL off and silence the copper side.
       */

      uint16_t cr1 = 0;

      ret = mae0621a_page_read(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYCR1, &cr1);
      if (ret < 0)
        {
          return ret;
        }

      if ((cr1 & MAE0621A_PHYCR1_CLEAR) != 0)
        {
          cr1 &= ~MAE0621A_PHYCR1_CLEAR;
          ret = mae0621a_page_write(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYCR1,
                                    cr1);
          if (ret < 0)
            {
              return ret;
            }

          syslog(LOG_INFO, "%s: PHYCR1 cleaned to 0x%04x\n", phy->name, cr1);
        }
    }

  return 0;
}

static int mae0621a_advertise(FAR struct ethernet_phy_s *phy)
{
  uint16_t gbesr = 0;
  uint16_t ctrl1000 = 0;
  int ret;

  ret = mae0621a_write(phy, MII_ADVERTISE, MAE0621A_ANAR_10_100);
  if (ret < 0)
    {
      return ret;
    }

  /* Advertise gigabit from what the part says it can do, not from memory: this
   * phy advertises 1000BASE-T full duplex only, and claiming half duplex as
   * well left the line negotiating a mode it cannot enter, which is a link
   * that never comes up rather than one that falls back.
   */

  if (mae0621a_read(phy, MII_ESTATUS, &gbesr) < 0)
    {
      gbesr = MAE0621A_GBESR_1000T_FULL;
    }

  if ((gbesr & MAE0621A_GBESR_1000T_FULL) != 0)
    {
      ctrl1000 |= MAE0621A_CTRL1000_ADV_FULL;
    }

  if ((gbesr & MAE0621A_GBESR_1000T_HALF) != 0)
    {
      ctrl1000 |= MAE0621A_CTRL1000_ADV_HALF;
    }

  return mae0621a_write(phy, MII_MSCONTROL, ctrl1000);
}

const struct ethernet_phy_ops_s g_mae0621a_ops = {
  .init = mae0621a_init,
  .advertise = mae0621a_advertise,
};
