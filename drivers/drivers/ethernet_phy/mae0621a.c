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
#define MAE0621A_PAGE_LED 0xd04
#define MAE0621A_PHYCR1   0x18
#define MAE0621A_PHYCR2   0x19
#define MAE0621A_PHYSR    0x1a
#define MAE0621A_INSR     0x1d
#define MAE0621A_LCR      0x10

/* The remaining pages the datasheet documents, so that this part's whole
 * register space can be compared against the vendor kernel's, which is the
 * only configuration on this board known to train a link.
 */

#define MAE0621A_PAGE_SCR  0xa46 /* PHYSCR  0x14 */
#define MAE0621A_PAGE_LED2 0xd04 /* EEELCR 0x11 */
#define MAE0621A_PAGE_MII  0xd08 /* MIICR   0x15 */
#define MAE0621A_PAGE_INTB 0xd40 /* INTBCR 0x16 */
#define MAE0621A_PHYSCR    0x14
#define MAE0621A_EEELCR    0x11
#define MAE0621A_MIICR     0x15
#define MAE0621A_INTBCR    0x16
#define MAE0621A_PHYCR2    0x19

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

/* How long the software reset may take to clear, and how long the analog side
 * is given afterwards.  The vendor kernel gives its generic reset this long,
 * and this part is a reason to: a PHY whose reference clock is not running
 * never clears the bit, so the timeout doubles as the test for whether the
 * part is running its reset sequence at all.
 */

#define MAE0621A_RESET_SETTLE_MS 100

/* The analog side of this part is not described by any published register
 * documentation: it lives in a set of vendor pages that have to be written in
 * one particular order before autonegotiation means anything.  Without them
 * the part answers MDIO, a software reset completes, every documented register
 * reads its default -- and the copper side never starts, which looks like a
 * PHY with no reference clock.
 *
 * The sequence below is the board vendor's, from the patched kernel they ship
 * for this board
 * (patch/kernel/rk35xx-vendor-6.1/004-add-PHY-for-MAE0621A-Q2C):
 * maxio_mae0621a_config_init() for PHY ID 0x7b744411, which is what this board
 * has.  The order is theirs, including the two indirect MMD accesses through
 * registers 13 and 14, which look odd out of context and are left as they are
 * rather than tidied into something less recognisable.
 *
 * Two things are deliberately not copied: their 0x0de1 write to ANAR (pause
 * advertisement, which this MAC does not implement) -- the advertisement is
 * written by the advertise hook instead -- and their status workaround, which
 * restarts autonegotiation when it fails to complete.
 */

struct mae0621a_page_reg_s
{
  uint16_t page;
  uint8_t regaddr;
  uint16_t value;
};

static const struct mae0621a_page_reg_s g_mae0621a_analog_init[] = {
  { 0x0d92, 0x02, 0x200a }, /* clock mode / analog enable */
  { 0x0da0, 0x10, 0x0c13 }, { 0x0000, 0x0d, 0x0007 }, /* MMD access control:
                                                         address, DEVAD 7 */
  { 0x0000, 0x0e, 0x003c }, /* MMD 7 register 0x3c address */
  { 0x0000, 0x0d, 0x4007 }, /* MMD access control: data, DEVAD 7 */
  { 0x0000, 0x0e, 0x0000 }, /* MMD 7[0x3c] = 0 */
  { 0x0d96, 0x13, 0x07bc }, { 0x0d8f, 0x08, 0x2500 }, { 0x0d90, 0x02, 0x1555 },
  { 0x0d90, 0x05, 0x2b15 }, { 0x0d92, 0x14, 0x000a }, { 0x0d91, 0x07, 0x5b00 },
  { 0x0d8f, 0x00, 0x0300 }, { 0x0d92, 0x0a, 0x8506 }, { 0x0d91, 0x06, 0x6870 },
  { 0x0d91, 0x01, 0x0940 }, { 0x0da0, 0x13, 0x1303 }, { 0x0d97, 0x0c, 0x0177 },
  { 0x0d97, 0x0b, 0x09a9 }, { 0x0a42, 0x12, 0x0028 }, /* interrupt enable: link
                                                         change, autoneg done
                                                       */
  { 0x0000, 0x00, 0x9140 }, /* BMCR: reset with autoneg and 1000M set */
};

/* The analog-to-digital converter self check: four samples, and a failure is
 * any sample whose low nine bits are zero. */

#define MAE0621A_ADC_SAMPLES 4
#define MAE0621A_ADC_MASK    0x01ff
#define MAE0621A_ADC_BASE    0xf908

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
 * Name: mae0621a_dump
 *
 * Description:
 *   One line of the vendor registers, because clause 22's own registers only
 *   say "no link" and the reasons live in the vendor page: PHYSR reports the
 *   real-time link and the MDI/MDIX state the part actually settled on, and
 *   PHYCR1/PHYCR2 carry the power saving and clock bits.
 *
 ****************************************************************************/

static void mae0621a_dump(FAR struct ethernet_phy_s *phy)
{
  uint16_t cr1 = 0;
  uint16_t cr2 = 0;
  uint16_t sr = 0;
  uint16_t lcr = 0;
  uint16_t insr = 0;
  uint16_t bmcr = 0;
  uint16_t bmsr = 0;
  uint16_t anar = 0;
  uint16_t stat1000 = 0;
  uint16_t estatus = 0;
  uint16_t scr = 0;
  uint16_t eelcr = 0;
  uint16_t miicr = 0;
  uint16_t intbcr = 0;

  mae0621a_read(phy, MII_MCR, &bmcr);
  mae0621a_read(phy, MII_MSR, &bmsr);
  mae0621a_page_read(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYCR1, &cr1);
  mae0621a_page_read(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYCR2, &cr2);
  mae0621a_page_read(phy, MAE0621A_PAGE_PHY, MAE0621A_PHYSR, &sr);
  mae0621a_page_read(phy, MAE0621A_PAGE_PHY, MAE0621A_INSR, &insr);
  mae0621a_page_read(phy, MAE0621A_PAGE_LED, MAE0621A_LCR, &lcr);

  /* INSR is sticky: bit 0 says autonegotiation ended in an error, bit 3 that
   * it completed, bit 4 that the link changed.  All zero means the state
   * machine never got anywhere, which is a different fault from one that ran
   * and failed.
   */

  mae0621a_read(phy, MII_ADVERTISE, &anar);
  mae0621a_read(phy, MII_MSSTATUS, &stat1000);
  mae0621a_read(phy, MII_ESTATUS, &estatus);
  mae0621a_page_read(phy, MAE0621A_PAGE_SCR, MAE0621A_PHYSCR, &scr);
  mae0621a_page_read(phy, MAE0621A_PAGE_LED2, MAE0621A_EEELCR, &eelcr);
  mae0621a_page_read(phy, MAE0621A_PAGE_MII, MAE0621A_MIICR, &miicr);
  mae0621a_page_read(phy, MAE0621A_PAGE_INTB, MAE0621A_INTBCR, &intbcr);

  syslog(LOG_INFO,
         "%s: bmcr 0x%04x bmsr 0x%04x phycr1 0x%04x phycr2 0x%04x "
         "physr 0x%04x insr 0x%04x lcr 0x%04x\n",
         phy->name, bmcr, bmsr, cr1, cr2, sr, insr, lcr);

  /* The rest of the documented space, so that this driver's view can be
   * diffed against the vendor kernel's on the same board: anar and the pause
   * bits, the 1000BASE-T status, and the four small vendor registers that
   * nobody has looked at yet.
   */

  syslog(LOG_INFO,
         "%s: anar 0x%04x stat1000 0x%04x estatus 0x%04x physcr 0x%04x "
         "eelcr 0x%04x miicr 0x%04x intbcr 0x%04x\n",
         phy->name, anar, stat1000, estatus, scr, eelcr, miicr, intbcr);
}

/****************************************************************************
 * Name: mae0621a_page_read_raw
 *
 * Description:
 *   One page read with no validation and no retry.  The validating version
 *   exists because an all-ones read turns into a write of 0xffff if it is not
 *   caught; a diagnostic that wants to *see* a zero -- the analog converter
 *   reporting nothing is exactly that -- cannot use it.
 *
 ****************************************************************************/

static int mae0621a_page_read_raw(FAR struct ethernet_phy_s *phy,
                                  uint16_t page, uint8_t regaddr,
                                  FAR uint16_t *value)
{
  int ret;

  ret = mae0621a_write(phy, MAE0621A_PAGSR, page);
  if (ret < 0)
    {
      return ret;
    }

  usleep(100);

  ret = mae0621a_read(phy, regaddr, value);

  mae0621a_write(phy, MAE0621A_PAGSR, MAE0621A_PAGE_STD);
  return ret;
}

/****************************************************************************
 * Name: mae0621a_analog_init
 *
 * Description:
 *   Write the vendor's analog calibration sequence.  A read-back of every
 *   value is not attempted: the pages are mostly write-only trim values, and
 *   the self check that follows is what says whether they took.
 *
 ****************************************************************************/

static int mae0621a_analog_init(FAR struct ethernet_phy_s *phy)
{
  size_t i;
  int ret;

  for (i = 0;
       i < sizeof(g_mae0621a_analog_init) / sizeof(g_mae0621a_analog_init[0]);
       i++)
    {
      ret = mae0621a_page_write(phy, g_mae0621a_analog_init[i].page,
                                g_mae0621a_analog_init[i].regaddr,
                                g_mae0621a_analog_init[i].value);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "%s: analog init failed at %zu (page 0x%03x "
                 "reg 0x%02x): %d\n",
                 phy->name, i, g_mae0621a_analog_init[i].page,
                 g_mae0621a_analog_init[i].regaddr, ret);
          return ret;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: mae0621a_adc_check
 *
 * Description:
 *   Sample the analog front end four times and say whether it is producing
 *   anything.  Zero in the low nine bits means the converter is not running,
 *   which is what an uncalibrated or unpowered analog block looks like.
 *
 ****************************************************************************/

static int mae0621a_adc_check(FAR struct ethernet_phy_s *phy,
                              FAR uint16_t *samples)
{
  uint16_t value = 0;
  int i;

  mae0621a_page_write(phy, 0x0d96, 0x02, 0x1fff);
  mae0621a_page_write(phy, 0x0d96, 0x02, 0x1000);

  for (i = 0; i < MAE0621A_ADC_SAMPLES; i++)
    {
      mae0621a_page_write(phy, 0x0d8f, 0x0b, MAE0621A_ADC_BASE + i * 0x100);

      if (mae0621a_page_read_raw(phy, 0x0d92, 0x0b, &value) < 0)
        {
          return -EIO;
        }

      samples[i] = value;

      if ((value & MAE0621A_ADC_MASK) == 0)
        {
          return -EAGAIN;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: mae0621a_self_check
 *
 * Description:
 *   Run the analog check, and on a failure reset the part and try again, which
 *   is the vendor's own recovery: their probe allows fifty attempts because a
 *   part that has just been powered can need several.
 *
 *   The two trailing writes are theirs as well: they put the converter back to
 *   its normal setting and leave the part resetting into autonegotiation.
 *
 ****************************************************************************/

static void mae0621a_self_check(FAR struct ethernet_phy_s *phy, int attempts)
{
  uint16_t samples[MAE0621A_ADC_SAMPLES] = { 0 };

  int attempt;

  for (attempt = 0; attempt < attempts; attempt++)
    {
      if (mae0621a_adc_check(phy, samples) == 0)
        {
          syslog(LOG_INFO, "%s: analog self check passed after %d attempt%s\n",
                 phy->name, attempt + 1, attempt == 0 ? "" : "s");
          break;
        }

      if (attempt == 0)
        {
          /* Worth seeing: what the converter returned, and whether the
           * calibration values can be read back at all.  A calibration
           * register that reads back as written means the writes land and the
           * analog side is what is not running; one that reads back as
           * something else means the pages are not being reached.
           */

          uint16_t v1 = 0;
          uint16_t v2 = 0;
          uint16_t v3 = 0;

          mae0621a_page_read_raw(phy, 0x0d92, 0x02, &v1);
          mae0621a_page_read_raw(phy, 0x0d96, 0x13, &v2);
          mae0621a_page_read_raw(phy, 0x0d8f, 0x00, &v3);

          syslog(LOG_WARNING,
                 "%s: analog check failed: adc 0x%04x 0x%04x 0x%04x 0x%04x, "
                 "readback d92/2 0x%04x d96/13 0x%04x d8f/0 0x%04x\n",
                 phy->name, samples[0], samples[1], samples[2], samples[3], v1,
                 v2, v3);
        }

      /* Reset, wait, then come back up: the vendor's recovery step. */

      mae0621a_page_write(phy, 0x0000, 0x00, 0x1940);
      mae0621a_page_write(phy, 0x0000, MAE0621A_PAGSR, 0x0000);
      usleep(10 * 1000);
      mae0621a_page_write(phy, 0x0000, 0x00, 0x1140);
      mae0621a_page_write(phy, 0x0000, 0x00, 0x9140);
    }

  if (attempt >= attempts)
    {
      /* The vendor's own driver throws this result away and carries on, and
       * their board links anyway, so this cannot be fatal: the converter
       * needs the reference clock, and the reference clock needs the analog
       * bias, so on a part that has just been powered both can be true at
       * once without anything being wrong.
       */

      syslog(LOG_WARNING,
             "%s: analog self check failed %d times, carrying on anyway\n",
             phy->name, attempts);
    }

  mae0621a_page_write(phy, 0x0d96, 0x02, 0x0fff);
  mae0621a_page_write(phy, 0x0000, 0x00, 0x9140);
  mae0621a_page_write(phy, 0x0000, MAE0621A_PAGSR, 0x0000);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static int mae0621a_init(FAR struct ethernet_phy_s *phy)
{
  uint16_t cr1 = 0;
  int ret;

  ret = mae0621a_analog_init(phy);
  if (ret < 0)
    {
      mae0621a_dump(phy);
      return ret;
    }

  mae0621a_self_check(phy, 50);

  usleep(MAE0621A_RESET_SETTLE_MS * 1000);

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

  mae0621a_dump(phy);
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
