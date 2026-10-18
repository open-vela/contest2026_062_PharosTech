/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_ethernet.c
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
 * KICKPI-K7 on-board dual Gigabit Ethernet.
 *
 * Both RJ45s are populated, each behind its own Maxio MAE0621A:
 *
 *   GMAC0 (0x2A220000) ── MAE0621A ── RJ45
 *   GMAC1 (0x2A230000) ── MAE0621A ── RJ45
 *
 * This file is the board's half of the port: which pads carry which RGMII
 * signal and at which alternate function, which GPIO resets which PHY, the
 * RGMII delay taps the vendor DTB specifies, and the MAC addresses.  All of
 * it comes from the vendor Debian image's DTB (all twelve of them agree) and
 * from the schematic, and none of it is inferred from the TRM.
 *
 * The two pin sets are disjoint -- different banks and different alternate
 * functions -- which is what makes two instances safe at the pinmux level.
 * The *resets* are crossed relative to the data pins: gmac0's data is on
 * bank 3 but its reset is GPIO2_B5 on bank 2, and gmac1's is the other way
 * round.  A copied "GMACn" net name is the only clue to which controller a
 * reset belongs to; the bank is not.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/net/mii.h>

#include <ethernet_phy.h>

#include <nuttx/arch.h>

#include "chip.h"
#include "hardware/rk3576_gmac.h"
#include "hardware/rk3576_gpio.h"
#include "hardware/rk3576_memorymap.h"
#include "kickpi_k7.h"
#include "rk3576_gmac.h"
#include "rk3576_gpio.h"

#ifdef CONFIG_KICKPI_K7_ETHERNET

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RGMII delay-line registers in the VCCIO IOC.  The KICKPI-K7 muxes pin
 * group m0 on both controllers, so only these two are ever written; the m1
 * pair (+0x040C and +0x0414) belongs to the pad sets this board does not use.
 */

#define KICKPI_K7_ETH0_IOC_OFFSET RK3576_VCCIO_IOC_MISC_CON2
#define KICKPI_K7_ETH1_IOC_OFFSET RK3576_VCCIO_IOC_MISC_CON4

/* PHY reset lines, active low, from the schematic's GMAC0_RSTn / GMAC1_RSTn
 * nets and the DTB's reset-gpios.
 */

#define KICKPI_K7_ETH0_PHY_RESET (GPIO_PORT2 | GPIO_PIN_B5) /* GPIO2_B5 */
#define KICKPI_K7_ETH1_PHY_RESET (GPIO_PORT3 | GPIO_PIN_A3) /* GPIO3_A3 */

/* The MDIO address both PHYs answer at: 0.
 *
 * The strap block's own table says 1 (PHYAD0 pulled up, PHYAD1/2 down), and
 * the first bring-up run measured 0 -- address 1 was tried first and did not
 * answer, the bus scan found the part at 0.  The vendor DTB agrees with the
 * hardware (`reg = <0>` on both phy@1 nodes), so 0 is what the descriptor says
 * and the scan is left as the fallback rather than as the way it is found.
 *
 * Both PHYs are wired identically, which is not a conflict: each controller
 * has its own MDIO bus.
 */

#define KICKPI_K7_ETH_PHYADDR 0

/* The PHY's MDI pairing, per port, from the board's choice of how the port is
 * wired against whatever it will be plugged into.
 */

#if defined(CONFIG_KICKPI_K7_ETHERNET_MDI0_MDI)
#define KICKPI_K7_ETH0_MDI_MODE ETHERNET_PHY_MDI_MDI
#elif defined(CONFIG_KICKPI_K7_ETHERNET_MDI0_MDIX)
#define KICKPI_K7_ETH0_MDI_MODE ETHERNET_PHY_MDI_MDIX
#else
#define KICKPI_K7_ETH0_MDI_MODE ETHERNET_PHY_MDI_AUTO
#endif

#if defined(CONFIG_KICKPI_K7_ETHERNET_MDI1_MDI)
#define KICKPI_K7_ETH1_MDI_MODE ETHERNET_PHY_MDI_MDI
#elif defined(CONFIG_KICKPI_K7_ETHERNET_MDI1_MDIX)
#define KICKPI_K7_ETH1_MDI_MODE ETHERNET_PHY_MDI_MDIX
#else
#define KICKPI_K7_ETH1_MDI_MODE ETHERNET_PHY_MDI_AUTO
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kickpi_k7_eth_pin_s
{
  gpio_pinset_t pinset;
  uint8_t af;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int kickpi_k7_eth0_pinmux(void);
static int kickpi_k7_eth1_pinmux(void);
static void kickpi_k7_eth0_phy_reset(bool assert);
static void kickpi_k7_eth1_phy_reset(bool assert);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* GMAC0 on the eth0m0 group: GPIO bank 3, alternate function 3.
 *
 * The pin *set* and the alternate function are the DTB's; the per-pin signal
 * is the TRM's IOMUX table, and the schematic confirms the split.  The DTB's
 * rockchip,pins cells are not in signal order -- eth0m0-tx_bus2 is <11 13 12>
 * -- so this table is written by signal, which is the only ordering that can
 * be checked against the schematic:
 *
 *   MDC 3A6, MDIO 3A5, TXCTL 3B3, TXD0 3B5, TXD1 3B4, TXD2 3C3, TXD3 3C2,
 *   TXCLK 3B6, RXCTL 3A7, RXD0 3B2, RXD1 3B1, RXD2 3D3, RXD3 3D2, RXCLK 3D1.
 */

/* The schematic's clock option for this board is Option 1: a fitted 25 MHz
 * crystal per PHY, with the SoC's alternative (the R6710/R6713 path from
 * GMACn_MCLKINOUT_M0 to the PHY's CLKOUT) marked NC.  So neither the SoC's
 * refclko25m output nor those pads are used, and nothing here muxes them.
 */

static const struct kickpi_k7_eth_pin_s g_eth0_pins[] = {
  { GPIO_PORT3 | GPIO_PIN_A6, 3 }, /* MDC    */
  { GPIO_PORT3 | GPIO_PIN_A5, 3 }, /* MDIO   */
  { GPIO_PORT3 | GPIO_PIN_B3, 3 }, /* TXCTL  */
  { GPIO_PORT3 | GPIO_PIN_B5, 3 }, /* TXD0   */
  { GPIO_PORT3 | GPIO_PIN_B4, 3 }, /* TXD1   */
  { GPIO_PORT3 | GPIO_PIN_C3, 3 }, /* TXD2   */
  { GPIO_PORT3 | GPIO_PIN_C2, 3 }, /* TXD3   */
  { GPIO_PORT3 | GPIO_PIN_B6, 3 }, /* TXCLK  */
  { GPIO_PORT3 | GPIO_PIN_A7, 3 }, /* RXCTL  */
  { GPIO_PORT3 | GPIO_PIN_B2, 3 }, /* RXD0   */
  { GPIO_PORT3 | GPIO_PIN_B1, 3 }, /* RXD1   */
  { GPIO_PORT3 | GPIO_PIN_D3, 3 }, /* RXD2   */
  { GPIO_PORT3 | GPIO_PIN_D2, 3 }, /* RXD3   */
  { GPIO_PORT3 | GPIO_PIN_D1, 3 }, /* RXCLK  */
};

/* GMAC1 on the eth1m0 group: GPIO bank 2, alternate function 2.
 *
 *   MDC 2D4, MDIO 2D5, TXCTL 2D0, TXD0 2C6, TXD1 2C7, TXD2 2C3, TXD3 2C4,
 *   TXCLK 2C5, RXCTL 2D3, RXD0 2D1, RXD1 2D2, RXD2 2C0, RXD3 2C1, RXCLK 2C2.
 */

static const struct kickpi_k7_eth_pin_s g_eth1_pins[] = {
  { GPIO_PORT2 | GPIO_PIN_D4, 2 }, /* MDC    */
  { GPIO_PORT2 | GPIO_PIN_D5, 2 }, /* MDIO   */
  { GPIO_PORT2 | GPIO_PIN_D0, 2 }, /* TXCTL  */
  { GPIO_PORT2 | GPIO_PIN_C6, 2 }, /* TXD0   */
  { GPIO_PORT2 | GPIO_PIN_C7, 2 }, /* TXD1   */
  { GPIO_PORT2 | GPIO_PIN_C3, 2 }, /* TXD2   */
  { GPIO_PORT2 | GPIO_PIN_C4, 2 }, /* TXD3   */
  { GPIO_PORT2 | GPIO_PIN_C5, 2 }, /* TXCLK  */
  { GPIO_PORT2 | GPIO_PIN_D3, 2 }, /* RXCTL  */
  { GPIO_PORT2 | GPIO_PIN_D1, 2 }, /* RXD0   */
  { GPIO_PORT2 | GPIO_PIN_D2, 2 }, /* RXD1   */
  { GPIO_PORT2 | GPIO_PIN_C0, 2 }, /* RXD2   */
  { GPIO_PORT2 | GPIO_PIN_C1, 2 }, /* RXD3   */
  { GPIO_PORT2 | GPIO_PIN_C2, 2 }, /* RXCLK  */
};

/* The addresses are locally administered, because nothing on this board
 * supplies one: the vendor DTB sets no mac-address, no nvmem-cells and no
 * local-mac-address, and the RK3576 GMAC has no OTP MAC.  They are Kconfig
 * values so that a cluster of boards can be told apart without a rebuild of
 * anything else.
 */

static uint8_t g_eth0_mac[6] = { 0x02, 0x52, 0x4b, 0x37, 0x76, 0x00 };

static uint8_t g_eth1_mac[6] = { 0x02, 0x52, 0x4b, 0x37, 0x76, 0x01 };

/* Handles for the two PHY reset lines.  They are claimed once and kept: the
 * GPIO driver allows a pin to be claimed exactly once, and the port is
 * brought up again on every ifup.
 */

static FAR struct gpio_dev_s *g_eth0_reset;
static FAR struct gpio_dev_s *g_eth1_reset;

static bool g_eth0_muxed;
static bool g_eth1_muxed;

/* The PHY on both ports, in the form NuttX describes one
 * (struct phy_desc_s, <nuttx/net/mii.h>).
 *
 * The ID is measured, 0x7b744411, not taken from the datasheet -- the
 * MAE0621A's register map does not list registers 2 and 3, which had been read
 * as "this part has no ID registers".  It has them.  The revision is the low
 * nibble of the second word, so only the rest is compared.
 *
 * There is no vendor status register worth reading, so status is 0xffff and
 * the driver uses clause 22's own registers (BMSR, and the 1000BASE-T pair) to
 * find the link.
 *
 * The address is 0, which is measured rather than inferred: PHYCR1's "PHYAD_0
 * Enable" defaults to set, so a command to address 0 is answered, and on this
 * board it is the only address either part answers at (the first bring-up run
 * tried the strap table's 1 first and got nothing).  The vendor DTB says
 * reg = <0> for both, which agrees.  The driver still scans the rest of the
 * bus when this address does not answer, so a mis-strapped board is usable
 * rather than dead.  The two MDIO buses are separate, so both ports may use
 * it.
 */

static const struct phy_desc_s g_eth0_phy = {
  .name = "MAE0621A",
  .id1 = 0x7b74,
  .id2 = 0x4410,    /* revision stripped                    */
  .status = 0xffff, /* no vendor status register            */
  .address_lo = KICKPI_K7_ETH_PHYADDR,
  .address_high = 0xffff, /* one address: address_lo              */
  .clause = 22,
};

static const struct phy_desc_s g_eth1_phy = {
  .name = "MAE0621A",
  .id1 = 0x7b74,
  .id2 = 0x4410,    /* revision stripped                    */
  .status = 0xffff, /* no vendor status register            */
  .address_lo = KICKPI_K7_ETH_PHYADDR,
  .address_high = 0xffff, /* one address: address_lo              */
  .clause = 22,
};

/* The PHY driver's view of the two ports.  The management bus is reached
 * through the controller's own accessors, and the port index is carried in the
 * opaque context: this file is the only place that knows a port number and a
 * PHY instance belong together, which is what keeps the chip and the driver
 * trees from including each other.
 */

static const int g_eth_intf[2] = { RK3576_GMAC0, RK3576_GMAC1 };

static int kickpi_k7_eth_mdio_read(FAR void *ctx, uint8_t phyaddr,
                                   uint8_t regaddr, FAR uint16_t *value)
{
  return rk3576_gmac_mdio_read(*(FAR const int *)ctx, phyaddr, regaddr, value);
}

static int kickpi_k7_eth_mdio_write(FAR void *ctx, uint8_t phyaddr,
                                    uint8_t regaddr, uint16_t value)
{
  return rk3576_gmac_mdio_write(*(FAR const int *)ctx, phyaddr, regaddr,
                                value);
}

static const struct ethernet_phy_s g_eth0_phy_dev = {
  .name = "MAE0621A",
  .ops = &g_mae0621a_ops,
  .mdio = {
    .read = kickpi_k7_eth_mdio_read,
    .write = kickpi_k7_eth_mdio_write,
    .ctx = (FAR void *)&g_eth_intf[0],
  },
  .addr = KICKPI_K7_ETH_PHYADDR,
  .mdi_mode = KICKPI_K7_ETH0_MDI_MODE,
};

static const struct ethernet_phy_s g_eth1_phy_dev = {
  .name = "MAE0621A",
  .ops = &g_mae0621a_ops,
  .mdio = {
    .read = kickpi_k7_eth_mdio_read,
    .write = kickpi_k7_eth_mdio_write,
    .ctx = (FAR void *)&g_eth_intf[1],
  },
  .addr = KICKPI_K7_ETH_PHYADDR,
  .mdi_mode = KICKPI_K7_ETH1_MDI_MODE,
};

static const struct rk3576_gmac_config_s g_eth0_config = {
  .ioc_offset = KICKPI_K7_ETH0_IOC_OFFSET,
  .tx_delay = CONFIG_KICKPI_K7_ETHERNET_TX_DELAY0,
  .rx_delay = 0,
  .tx_delay_ena = true,
  .rx_delay_ena = false, /* the PHY's RXDLY strap adds it instead */
  .mac = g_eth0_mac,
  .pinmux = kickpi_k7_eth0_pinmux,
  .phy_reset = kickpi_k7_eth0_phy_reset,
  .phy = &g_eth0_phy,
  .phy_init = ethernet_phy_init,
  .phy_advertise = ethernet_phy_advertise,
  .phy_ctx = (FAR void *)&g_eth0_phy_dev,
};

static const struct rk3576_gmac_config_s g_eth1_config = {
  .ioc_offset = KICKPI_K7_ETH1_IOC_OFFSET,
  .tx_delay = CONFIG_KICKPI_K7_ETHERNET_TX_DELAY1,
  .rx_delay = 0,
  .tx_delay_ena = true,
  .rx_delay_ena = false,
  .mac = g_eth1_mac,
  .pinmux = kickpi_k7_eth1_pinmux,
  .phy_reset = kickpi_k7_eth1_phy_reset,
  .phy = &g_eth1_phy,
  .phy_init = ethernet_phy_init,
  .phy_advertise = ethernet_phy_advertise,
  .phy_ctx = (FAR void *)&g_eth1_phy_dev,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_eth_parse_mac
 *
 * Description:
 *   Read a "xx:xx:xx:xx:xx:xx" MAC address out of a Kconfig string.
 *
 *   A reject rather than a guess: a bad address that silently becomes the
 *   driver's fallback would show up as two boards unable to talk, which is a
 *   much longer hunt than a boot-time error line.
 *
 ****************************************************************************/

static bool kickpi_k7_eth_parse_mac(FAR const char *str, FAR uint8_t *mac)
{
  int i;

  if (str == NULL || strlen(str) != 17)
    {
      return false;
    }

  for (i = 0; i < 6; i++)
    {
      unsigned int byte;

      if (sscanf(str + i * 3, "%2x", &byte) != 1)
        {
          return false;
        }

      if (i < 5 && str[i * 3 + 2] != ':')
        {
          return false;
        }

      mac[i] = (uint8_t)byte;
    }

  /* The all-zero address is not a unicast address anybody owns, and a
   * multicast address can never be a source: both are configuration
   * mistakes, not board variants.
   */

  if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0)
    {
      return false;
    }

  return (mac[0] & 1) == 0;
}

/****************************************************************************
 * Name: kickpi_k7_eth_mux_group
 *
 * Description:
 *   Mux one RGMII pin group and leave every pad without a pull, which is
 *   what the DTB's bias-disable says and what the PHY's strap resistors
 *   require: all six MAE0621A strap pins are PHY-to-SoC directions, so at
 *   strap-sampling time the SoC is an input and must not pull against the
 *   board's 4.7 kOhm straps.
 *
 *   rk3576_gpio_get() already resets a claimed pin to AF0, input and no
 *   pull, so the pull is right before this runs; the mux is what is left.
 *   Called more than once is harmless, but the claim it does is not, so the
 *   groups are muxed once.
 *
 ****************************************************************************/

static int kickpi_k7_eth_mux_group(FAR const struct kickpi_k7_eth_pin_s *pins,
                                   size_t npins, FAR bool *done,
                                   FAR const char *name)
{
  FAR struct gpio_dev_s *handle;
  size_t i;
  int ret;

  if (*done)
    {
      return OK;
    }

  for (i = 0; i < npins; i++)
    {
      ret = rk3576_gpio_get(pins[i].pinset, &handle);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s pin %zu could not be claimed: %d\n", name,
                 i, ret);
          return ret;
        }

      rk3576_gpio_set_af(handle, pins[i].af);
    }

  *done = true;
  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_eth0_pinmux / kickpi_k7_eth1_pinmux
 ****************************************************************************/

static int kickpi_k7_eth0_pinmux(void)
{
  return kickpi_k7_eth_mux_group(g_eth0_pins,
                                 sizeof(g_eth0_pins) / sizeof(g_eth0_pins[0]),
                                 &g_eth0_muxed, "GMAC0");
}

static int kickpi_k7_eth1_pinmux(void)
{
  return kickpi_k7_eth_mux_group(g_eth1_pins,
                                 sizeof(g_eth1_pins) / sizeof(g_eth1_pins[0]),
                                 &g_eth1_muxed, "GMAC1");
}

/****************************************************************************
 * Name: kickpi_k7_eth_reset
 *
 * Description:
 *   Drive one PHY reset line.  `assert` means "hold the PHY in reset", which
 *   is the line low.
 *
 ****************************************************************************/

static int kickpi_k7_eth_reset(FAR struct gpio_dev_s **handle,
                               gpio_pinset_t pinset, bool assert)
{
  int ret;

  if (*handle == NULL)
    {
      ret = rk3576_gpio_get(pinset, handle);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: PHY reset gpio %u could not be claimed: "
                 "%d\n",
                 pinset, ret);
          return ret;
        }

      rk3576_gpio_set_mode(*handle, RK3576_GPIO_OUTPUT);
    }

  rk3576_gpio_write_bit(*handle, !assert);

  /* Nothing is read back here.  EXT_PORT does not reliably reflect a pin that
   * is driven as an output on this part -- a check on it fired on a reset that
   * demonstrably worked, because the PHY's BMCR came back at its reset default
   * on the next ifup.  What proves the reset reaches the part is that default,
   * not a pad level.
   */

  return OK;
}

static void kickpi_k7_eth0_phy_reset(bool assert)
{
  kickpi_k7_eth_reset(&g_eth0_reset, KICKPI_K7_ETH0_PHY_RESET, assert);
}

static void kickpi_k7_eth1_phy_reset(bool assert)
{
  kickpi_k7_eth_reset(&g_eth1_reset, KICKPI_K7_ETH1_PHY_RESET, assert);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_ethernet_initialize
 *
 * Description:
 *   Register the two on-board wired ports.  The chip driver does the
 *   platform bring-up, verifies the controller answers and probes the PHY;
 *   the rings and the PHY's autonegotiation come up later, on `ifup`.
 *
 *   A failure on one port only logs: the other port, and the rest of the
 *   board, must not depend on both being populated.
 *
 * Returned Value:
 *   Zero (OK) if every enabled port was registered; a negated errno
 *   otherwise.
 *
 ****************************************************************************/

int kickpi_k7_ethernet_initialize(void)
{
  uint8_t mac[6];
  int ret;

  if (!kickpi_k7_eth_parse_mac(CONFIG_KICKPI_K7_ETHERNET_MACADDR0, mac))
    {
      syslog(LOG_ERR,
             "ERROR: CONFIG_KICKPI_K7_ETHERNET_MACADDR0 is not a "
             "unicast MAC address: \"%s\"\n",
             CONFIG_KICKPI_K7_ETHERNET_MACADDR0);
      return -EINVAL;
    }

  memcpy(g_eth0_mac, mac, sizeof(g_eth0_mac));

  ret = rk3576_gmac_initialize(RK3576_GMAC0, &g_eth0_config);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GMAC0 initialization failed: %d\n", ret);
    }

#ifdef CONFIG_KICKPI_K7_ETHERNET_PORT1
  if (!kickpi_k7_eth_parse_mac(CONFIG_KICKPI_K7_ETHERNET_MACADDR1, mac))
    {
      syslog(LOG_ERR,
             "ERROR: CONFIG_KICKPI_K7_ETHERNET_MACADDR1 is not a "
             "unicast MAC address: \"%s\"\n",
             CONFIG_KICKPI_K7_ETHERNET_MACADDR1);
      return -EINVAL;
    }

  memcpy(g_eth1_mac, mac, sizeof(g_eth1_mac));

  {
    int ret1 = rk3576_gmac_initialize(RK3576_GMAC1, &g_eth1_config);
    if (ret1 < 0)
      {
        syslog(LOG_ERR, "ERROR: GMAC1 initialization failed: %d\n", ret1);
        if (ret == OK)
          {
            ret = ret1;
          }
      }
  }
#endif

  return ret;
}

#endif /* CONFIG_KICKPI_K7_ETHERNET */
