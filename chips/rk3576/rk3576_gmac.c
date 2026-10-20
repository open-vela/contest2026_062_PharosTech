/****************************************************************************
 * chips/rk3576/rk3576_gmac.c
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
 * Three layers, bottom up, each readable on its own:
 *
 *   platform   the CRU clocks, the SDGMAC_GRF RGMII mode, the VCCIO delay
 *              lines and the controller reset.  Every write here fails
 *              silently if it is wrong -- a wrong gate bit or a wrong GRF
 *              offset produces no fault, only a controller that reads zero --
 *              so each one is read back and logged.
 *   data       one descriptor ring each way, the interrupt, and the
 *              ownership rules that go with them.
 *   netdev     the NuttX lower half (netdev_lowerhalf), which owns RX
 *              deferral, TX polling, quota, carrier and statistics.
 *
 * The PHY code is in this file on purpose: that is the shape every NuttX MAC
 * driver has (imx9_enet, imxrt_enet, stm32_ethernet, sam_emac, ...), because
 * the management bus belongs to the MAC and clause 22 is the same for every
 * part.  What differs between parts arrives as data, in the struct phy_desc_s
 * the board supplies -- exactly as imx9's BOARD_ENETn_PHY_LIST does.  NuttX
 *has no PHY driver layer to hang a separate driver on: <nuttx/net/phy.h> is
 *the SIGIO link-notification service and nothing else.
 *
 * The two instances share nothing but the CRU gate register (different bits)
 * and the clock tree; the MDIO buses are separate, so the two PHYs may -- and
 * do -- answer at the same address.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <net/ethernet.h>
#include <net/if.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/net/mii.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include "arm64_arch.h"
#include "chip.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_gmac.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_addrenv.h"
#include "rk3576_gmac.h"

#ifdef CONFIG_RK3576_GMAC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* This driver's only hard requirement on the network buffer pool.
 *
 * The RX descriptor's buffer is the netpkt's own data area -- there is no
 * bounce buffer and nothing is copied -- so the buffer size programmed into
 * the DMA is CONFIG_IOB_BUFSIZE itself.  It has to hold a maximum Ethernet
 * frame (1518 bytes, including the VLAN header the MAC may pass) and it has
 * to be a whole DMA beat.  The ethernet configuration uses 1536, which is
 * both; 1516, the value the camera configuration uses, is not a multiple of
 * 16 and would have to move before this driver could be enabled there.
 */

#if CONFIG_IOB_BUFSIZE < 1518
#error "CONFIG_RK3576_GMAC: CONFIG_IOB_BUFSIZE must hold a full frame"
#endif

#if (CONFIG_IOB_BUFSIZE % 16) != 0
#error "CONFIG_RK3576_GMAC: CONFIG_IOB_BUFSIZE must be a whole AXI beat"
#endif

#define GM_BUFSIZE CONFIG_IOB_BUFSIZE

/* The ring is packed at the hardware's descriptor size: descriptor n lives at
 * base + 16n and the stride is not ours to choose.  Padding each descriptor
 * out to a whole D-cache line was tried -- for cache operations that could not
 * disturb a neighbour -- and it cost exactly one frame per direction.  The DMA
 * walked base, base+16, base+32, found the zero padding where it expected the
 * next descriptor's OWN bit, reported TBU and suspended, while the driver
 * happily queued frames at base+64, base+128 that the DMA never looked at. The
 * symptom is a port that trains its link, sends one frame, receives one frame,
 * and then reports "transmit buffer unavailable" forever.
 *
 * Cache safety comes from the discipline in gm_desc_sync()/gm_desc_inval()
 * instead: a line is only ever dirty between writing a descriptor and cleaning
 * it, so an invalidate that shares its line with a neighbour drops clean
 * copies and cannot clobber a status the DMA has written.
 */

#define GM_DESC_SIZE   16
#define GM_DESC_STRIDE GM_DESC_SIZE
#define GM_TX_DESC     CONFIG_RK3576_GMAC_TX_DESC
#define GM_RX_DESC     CONFIG_RK3576_GMAC_RX_DESC

/* Quota is what netpkt_alloc()/netpkt_get() will hand out for a direction.
 *
 * The transmit quota is the ring depth: a slot holds one netpkt from submit
 * until the completion interrupt frees it.
 *
 * The receive quota has to be larger than the ring, because a completed frame
 * is replaced *before* it is handed up: receive() allocates the buffer for the
 * descriptor's next use while the received packet is still held here, and the
 * framework only returns that packet's quota after receive() has returned. Set
 * it to the ring depth alone and the last allocation can never succeed, so
 * every received frame is dropped on the floor with no error anywhere -- the
 * ring stays armed, the link comes up, ARP still works out of the
 * pending-queue path, and IP never sees a packet.  Other drivers spell this as
 * "ring depth + spare" for the same reason (e1000's E1000_RX_QUOTA).
 */

#define GM_TX_QUOTA GM_TX_DESC
#define GM_RX_SPARE 4
#define GM_RX_QUOTA (GM_RX_DESC + GM_RX_SPARE)

/* The controller's aliveness test (TRM Part 2, MAC_VERSION). */

#define GM_MAC_VERSION 0x00003051

/* Clause 22 pieces <nuttx/net/mii.h> does not carry.
 *
 * The 10/100 register set and every BMCR/BMSR bit come from that header.  What
 * it has no names for is the 1000BASE-T control/status pair (registers 9 and
 * 10) and the four bits in them that say what gigabit mode was agreed -- the
 * only place a gigabit result appears, because registers 4 and 5 describe
 * 10/100 only.
 */

#define GM_PHY_ANER     0x06 /* autonegotiation expansion    */
#define GM_PHY_CTRL1000 0x09 /* 1000BASE-T control           */
#define GM_PHY_STAT1000 0x0a /* 1000BASE-T status            */

/* Register 10's 1000BASE-T result bits: bit 11 full duplex, bit 10 half.  Note
 * that register 9 puts the advertisement in *different* bits (9 and 10), which
 * is the kind of pair that gets written from memory and then read wrong.
 */

#define GM_PHY_LPA_1000FULL (1 << 11) /* reg 10 bit 11 */
#define GM_PHY_LPA_1000HALF (1 << 10) /* reg 10 bit 10 */

/* All four 10/100 modes and the IEEE 802.3 selector.  Pause is deliberately
 * not advertised: the MAC's flow control is not programmed either, and
 * offering a capability the MAC will not honour is worse than not having it.
 */

#define GM_PHY_ANAR_10_100                                     \
  (MII_ADVERTISE_10BASETXHALF | MII_ADVERTISE_10BASETXFULL |   \
   MII_ADVERTISE_100BASETXHALF | MII_ADVERTISE_100BASETXFULL | \
   MII_ADVERTISE_8023)

#define GM_PHY_ADDR_NONE 0xff

/* PHYID2's low four bits are the revision, so only the rest is compared -- the
 * same masking imx9_enet does.
 */

#define GM_PHY_ID2_MASK 0xfff0

/* Timeouts: microsecond loop counters, and milliseconds for the work queue. */

#define GM_MDIO_TIMEOUT  100000
#define GM_RESET_TIMEOUT 100000
#define GM_LINK_POLL_MS  1000
#define GM_RXRETRY_MS    10

/* Interrupts: the individual bits, which the TRM and the DWC databook agree
 * on.  See the note in hardware/rk3576_gmac.h about the two summary enables.
 */

#define GM_IRQ_ENABLE                                             \
  (RK3576_GMAC_CH0INT_TIE | RK3576_GMAC_CH0INT_RIE |              \
   RK3576_GMAC_CH0INT_RBUE | RK3576_GMAC_CH0INT_FBEE |            \
   RK3576_GMAC_CH0INT_AIE_TRM | RK3576_GMAC_CH0INT_AIE_DATABOOK | \
   RK3576_GMAC_CH0INT_NIE_TRM | RK3576_GMAC_CH0INT_NIE_DATABOOK)

/* Every status bit a completed transfer or an error can set. */

#define GM_IRQ_STATUS                                                         \
  (RK3576_GMAC_CH0STS_TI | RK3576_GMAC_CH0STS_TBU | RK3576_GMAC_CH0STS_RI |   \
   RK3576_GMAC_CH0STS_RBU | RK3576_GMAC_CH0STS_RPS | RK3576_GMAC_CH0STS_RWT | \
   RK3576_GMAC_CH0STS_ETI | RK3576_GMAC_CH0STS_ERI | RK3576_GMAC_CH0STS_FBE | \
   RK3576_GMAC_CH0STS_CDE | RK3576_GMAC_CH0STS_AIS |                          \
   RK3576_GMAC_CH0STS_AIS_DATABOOK | RK3576_GMAC_CH0STS_NIS |                 \
   RK3576_GMAC_CH0STS_NIS_DATABOOK)

/* Soft reset bits in CRU_SOFTRST_CON42.  Only the two per-port bits of each
 * controller are touched: the sdgmac_biu and sdgmac_grf bits are shared with
 * the other port and with the register file this driver programs.
 */

#define GM_SOFTRST_CON       42
#define GM_SOFTRST_ARESET(n) (1u << (7 + (n))) /* aresetn_gmac0 / gmac1 */
#define GM_SOFTRST_PRESET(n) (1u << (9 + (n))) /* presetn_gmac0 / gmac1 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One descriptor.  Only the words this driver reads or writes are named; the
 * rest of the 16 bytes is written by the DMA.
 */

struct rk3576_gmac_desc_s
{
  uint32_t des0; /* Buffer 1 address pointer             */
  uint32_t des1; /* Reserved (32-bit addressing)         */
  uint32_t des2; /* TX: BUF1L; RX: reserved              */
  uint32_t des3; /* Control/status, including OWN        */
};

/* What one read of the PHY's link state produced, kept raw as well as decoded:
 * the raw words are what a bring-up log needs when the decoded answer is "no
 * link".
 */

struct gm_phy_status_s
{
  uint16_t bmsr;
  uint16_t lpa; /* 0 until it has been read */
};

/* Driver state.  The lower half comes first: the netdev layer casts between
 * struct netdev_lowerhalf_s and this, so `dev` must stay at offset zero.
 */

struct rk3576_gmac_s
{
  struct netdev_lowerhalf_s dev;

  FAR const struct rk3576_gmac_config_s *cfg;
  uint8_t intf;
  uintptr_t base;
  int irq;

  FAR struct clk_s *aclk;   /* aclk_gmac<n>                           */
  FAR struct clk_s *pclk;   /* pclk_gmac<n>                           */
  FAR struct clk_s *clk125; /* clk_gmac<n>_125m_src, the RGMII clock  */
  FAR struct clk_s *grfclk; /* pclk_sdgmac_grf: GMACn_CON's own clock  */

  uint8_t phyaddr; /* where the PHY answered                 */

  /* The last raw PHY snapshot printed while the link is down, so that a cable
   * being plugged in, or autonegotiation getting somewhere, shows up in the
   * log once per change instead of never -- or every second.
   */

  /* Set when the RX ring ran out of buffers, so the complaint is made once
   * rather than once per frame.
   */

  bool rxstarved;

  uint16_t trace_bmsr;
  uint16_t trace_lpa;
  bool trace_valid;
  bool trace_settled;   /* the second, delayed snapshot has been taken */
  uint16_t trace_polls; /* polls since ifup, for that second snapshot   */

  bool started;      /* rings armed and the MAC enabled        */
  uint16_t hw_speed; /* what the MAC is currently programmed   */
  bool hw_duplex;    /*   with                                 */
  bool carrier;      /* last carrier state reported upward     */

  FAR struct rk3576_gmac_desc_s *txring;
  FAR struct rk3576_gmac_desc_s *rxring;
  FAR netpkt_t *txpkt[GM_TX_DESC];
  FAR netpkt_t *rxpkt[GM_RX_DESC];
  uint32_t txnow;  /* first free TX slot                     */
  uint32_t txdone; /* first completed TX slot                */
  uint32_t rxnow;  /* next RX descriptor to hand upward      */

  struct work_s linkwork; /* PHY state machine, on LPWORK           */
  struct work_s rxretry;  /* re-arm a stalled RX ring               */
  spinlock_t lock;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int gm_transmit(FAR struct netdev_lowerhalf_s *dev, FAR netpkt_t *pkt);
static FAR netpkt_t *gm_receive(FAR struct netdev_lowerhalf_s *dev);
static int gm_ifup(FAR struct netdev_lowerhalf_s *dev);
static int gm_ifdown(FAR struct netdev_lowerhalf_s *dev);
static void gm_reclaim(FAR struct netdev_lowerhalf_s *dev);
static void gm_link_work(FAR void *arg);
static void gm_rx_retry(FAR void *arg);
#ifdef CONFIG_NET_MCASTGROUP
static int gm_addmac(FAR struct netdev_lowerhalf_s *dev,
                     FAR const uint8_t *mac);
static int gm_rmmac(FAR struct netdev_lowerhalf_s *dev,
                    FAR const uint8_t *mac);
#endif
#ifdef CONFIG_NETDEV_IOCTL
static int gm_ioctl(FAR struct netdev_lowerhalf_s *dev, int cmd,
                    unsigned long arg);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The two controllers, so that a board-side PHY driver can reach a port's
 * management bus by index without holding an opaque handle -- the PHY driver
 * must not include this header, and the board is the only place that knows
 * which index belongs to which port.
 */

static FAR struct rk3576_gmac_s *g_gm_port[2];

static const struct netdev_ops_s g_gm_ops = {
  .ifup = gm_ifup,
  .ifdown = gm_ifdown,
  .transmit = gm_transmit,
  .receive = gm_receive,
#ifdef CONFIG_NET_MCASTGROUP
  .addmac = gm_addmac,
  .rmmac = gm_rmmac,
#endif
#ifdef CONFIG_NETDEV_IOCTL
  .ioctl = gm_ioctl,
#endif
  .reclaim = gm_reclaim,
};

/****************************************************************************
 * Private Functions: register access and cache maintenance
 ****************************************************************************/

static inline uint32_t gm_getreg(FAR struct rk3576_gmac_s *priv,
                                 uint32_t offset)
{
  return getreg32(priv->base + offset);
}

static inline void gm_putreg(FAR struct rk3576_gmac_s *priv, uint32_t offset,
                             uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: gm_desc
 *
 * Description:
 *   Address of descriptor `index` in a ring.  One descriptor per cache line,
 *   so this is the only place that knows the stride.
 *
 ****************************************************************************/

static FAR struct rk3576_gmac_desc_s *
gm_desc(FAR struct rk3576_gmac_desc_s *ring, uint32_t index)
{
  return (FAR struct rk3576_gmac_desc_s *)((uintptr_t)ring +
                                           index * GM_DESC_STRIDE);
}

/****************************************************************************
 * Name: gm_desc_sync / gm_desc_inval
 *
 * Description:
 *   Hand a descriptor to the DMA / take one back.
 *
 *   Writing a descriptor and then cleaning its line is the only ordering rule
 *   here: after the clean the line is clean in the cache, so a later
 *   invalidate cannot discard a CPU write -- it can only drop a stale cached
 *   copy.  That is what makes it safe to re-read a descriptor the DMA has not
 *   touched yet and still see our own OWN bit.
 *
 ****************************************************************************/

static inline void gm_desc_sync(FAR struct rk3576_gmac_desc_s *desc)
{
  uintptr_t addr = (uintptr_t)desc;

  up_clean_dcache(addr, addr + GM_DESC_STRIDE);
}

static inline void gm_desc_inval(FAR struct rk3576_gmac_desc_s *desc)
{
  uintptr_t addr = (uintptr_t)desc;

  up_invalidate_dcache(addr, addr + GM_DESC_STRIDE);
}

/****************************************************************************
 * Name: gm_buf_clean / gm_buf_inval
 *
 * Description:
 *   Data-path cache maintenance.
 *
 *   The MAC does not snoop, so the contract is explicit maintenance on every
 *   buffer in both directions.  The buffers live in the shared IOB pool and
 *   are not cache-line aligned, which is fine: the arch's range helpers
 *   clean-and-invalidate a partial first or last line rather than
 *   invalidating it, so the iob header sharing that line survives.
 *
 ****************************************************************************/

static inline void gm_buf_clean(FAR void *buf, size_t len)
{
  uintptr_t addr = (uintptr_t)buf;

  up_clean_dcache(addr, addr + len);
}

static inline void gm_buf_inval(FAR void *buf, size_t len)
{
  uintptr_t addr = (uintptr_t)buf;

  up_invalidate_dcache(addr, addr + len);
}

/****************************************************************************
 * Name: gm_grf_update
 *
 * Description:
 *   Write a field in one of the hiword-write-enable registers (SDGMAC_GRF
 *   and VCCIO_IOC): bits [31:16] of the write gate the corresponding bit of
 *   [15:0].  Read before, write, read after, and say so if the value did not
 *   stick -- silence is the failure mode these registers are designed to
 *   produce.
 *
 ****************************************************************************/

static int gm_grf_update(FAR struct rk3576_gmac_s *priv, uint32_t reg,
                         uint32_t mask, uint32_t value, FAR const char *what)
{
  uint32_t before;
  uint32_t after;

  before = getreg32(reg) & mask;
  putreg32(((mask & 0xffffu) << 16) | (value & mask), reg);
  after = getreg32(reg) & mask;

  if (after != (value & mask))
    {
      syslog(LOG_ERR,
             "gmac%d: %s did not stick: 0x%04" PRIx32 " -> 0x%04" PRIx32
             ", wanted 0x%04" PRIx32 "\n",
             priv->intf, what, before, after, value & mask);
      return -EIO;
    }

  if (before != after)
    {
      syslog(LOG_INFO, "gmac%d: %s 0x%04" PRIx32 " -> 0x%04" PRIx32 "\n",
             priv->intf, what, before, after);
    }

  return OK;
}

/****************************************************************************
 * Private Functions: MDIO and the PHY
 ****************************************************************************/

/****************************************************************************
 * Name: gm_mdio_wait
 *
 * Description:
 *   Wait for the MDIO bus to go idle.  GB is self-clearing, so a stuck GB is
 *   a controller that is not clocked -- worth a timeout rather than a hang.
 *
 ****************************************************************************/

static int gm_mdio_wait(FAR struct rk3576_gmac_s *priv)
{
  uint32_t timeout;

  for (timeout = GM_MDIO_TIMEOUT; timeout > 0; timeout--)
    {
      if ((gm_getreg(priv, RK3576_GMAC_MAC_MDIO_ADDRESS) &
           RK3576_GMAC_MDIO_ADDRESS_GB) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: gm_mdio_read / gm_mdio_write
 *
 * Description:
 *   Clause 22 access over the controller's own MDIO bus.  The CSR-clock
 *   divider is fixed at /324, which keeps MDC under the PHY's 2.5 MHz ceiling
 *   for any CSR clock below ~800 MHz -- bring-up must not depend on knowing
 *   the CSR rate, so the slowest divider is the right one to start with.
 *
 ****************************************************************************/

static int gm_mdio_read(FAR struct rk3576_gmac_s *priv, uint8_t phyaddr,
                        uint8_t regaddr, FAR uint16_t *value)
{
  int ret;

  ret = gm_mdio_wait(priv);
  if (ret < 0)
    {
      return ret;
    }

  gm_putreg(priv, RK3576_GMAC_MAC_MDIO_ADDRESS,
            RK3576_GMAC_MDIO_ADDRESS_CR_324 |
                RK3576_GMAC_MDIO_ADDRESS_GOC_READ |
                RK3576_GMAC_MDIO_ADDRESS_GB |
                ((uint32_t)phyaddr << RK3576_GMAC_MDIO_ADDRESS_PA_S) |
                ((uint32_t)regaddr << RK3576_GMAC_MDIO_ADDRESS_RDA_S));

  ret = gm_mdio_wait(priv);
  if (ret < 0)
    {
      return ret;
    }

  *value =
      gm_getreg(priv, RK3576_GMAC_MAC_MDIO_DATA) & RK3576_GMAC_MDIO_DATA_GD_M;
  return OK;
}

static int gm_mdio_write(FAR struct rk3576_gmac_s *priv, uint8_t phyaddr,
                         uint8_t regaddr, uint16_t value)
{
  int ret;

  ret = gm_mdio_wait(priv);
  if (ret < 0)
    {
      return ret;
    }

  gm_putreg(priv, RK3576_GMAC_MAC_MDIO_DATA, value);
  gm_putreg(priv, RK3576_GMAC_MAC_MDIO_ADDRESS,
            RK3576_GMAC_MDIO_ADDRESS_CR_324 |
                RK3576_GMAC_MDIO_ADDRESS_GOC_WRITE |
                RK3576_GMAC_MDIO_ADDRESS_GB |
                ((uint32_t)phyaddr << RK3576_GMAC_MDIO_ADDRESS_PA_S) |
                ((uint32_t)regaddr << RK3576_GMAC_MDIO_ADDRESS_RDA_S));

  return gm_mdio_wait(priv);
}

/****************************************************************************
 * Name: gm_phy_present
 *
 * Description:
 *   Is the described PHY at this address?
 *
 *   Two kinds of part, and the descriptor says which.  One has PHY ID
 *   registers (2 and 3): id1 is compared whole and id2 with its revision
 *   nibble masked off, the way imx9_enet does it.  The other has none -- the
 *   KICKPI-K7's MAE0621A is one, its register map does not list registers 2
 *   and 3 at all -- and is identified by its status register instead:
 *   readable, with the extended-capability bit set, and neither all-ones nor
 *   all-zeros, which is what an address with nothing behind it reads back as.
 *
 *   A descriptor with id1 == 0 and id2 == 0 means "no PHY ID registers" and
 *   nothing else; that is the one piece of meaning struct phy_desc_s does not
 *   spell out.
 *
 ****************************************************************************/

static bool gm_phy_present(FAR struct rk3576_gmac_s *priv, uint8_t phyaddr)
{
  FAR const struct phy_desc_s *desc = priv->cfg->phy;
  uint16_t bmsr = 0xffff;
  uint16_t id1;
  uint16_t id2;
  int attempt;

  /* Not one read: the first management transaction after the controller or the
   * PHY comes out of reset can fail, and this board demonstrated it -- with
   * the strapped address tried first it was the one that failed, and the part
   * was found at the address tried second.  Three attempts 10 ms apart is
   * cheap next to a port that will not come up.
   */

  for (attempt = 0; attempt < 3; attempt++)
    {
      if (attempt > 0)
        {
          up_mdelay(10);
        }

      if (gm_mdio_read(priv, phyaddr, MII_MSR, &bmsr) < 0)
        {
          continue;
        }

      if (bmsr != 0xffff && bmsr != 0x0000)
        {
          break;
        }
    }

  if (bmsr == 0xffff || bmsr == 0x0000)
    {
      return false;
    }

  /* A descriptor with no ID means the board says this part has no ID
   * registers to compare -- then a readable status register with the extended
   * capability bit is the best evidence of a PHY that there is.
   */

  if (desc->id1 == 0 && desc->id2 == 0)
    {
      return (bmsr & MII_MSR_EXTCAP) != 0;
    }

  if (gm_mdio_read(priv, phyaddr, MII_PHYID1, &id1) < 0 ||
      gm_mdio_read(priv, phyaddr, MII_PHYID2, &id2) < 0)
    {
      return false;
    }

  return id1 == desc->id1 &&
         (id2 & GM_PHY_ID2_MASK) == (desc->id2 & GM_PHY_ID2_MASK);
}

/****************************************************************************
 * Name: gm_phy_probe
 *
 * Description:
 *   Resolve the PHY's MDIO address: the descriptor's address range first,
 *   then the rest of the bus.
 *
 *   The range is where the board's strap resistors put the part.  The scan is
 *   what keeps a mis-strapped board usable instead of dead, and the log says
 *   which address answered.  The two buses are separate, so the two ports may
 *   share an answer -- and on this board they do.
 *
 *   The KICKPI-K7 also shows why the scan is not decoration: the strap block
 *   selects address 1, but the MAE0621A's PHYCR1 "PHYAD_0 Enable" defaults to
 *   set -- "a command with PHY address = 0 is valid, MDC/MDIO will respond" --
 *   so address 0 answers as well, which is why the vendor device tree says
 *   reg = <0> for a PHY strapped to 1.
 *
 ****************************************************************************/

static int gm_phy_probe(FAR struct rk3576_gmac_s *priv)
{
  FAR const struct phy_desc_s *desc = priv->cfg->phy;
  uint16_t bmsr = 0;
  uint16_t discard;
  uint8_t addr;
  int i;

  if (desc->clause != 0 && desc->clause != 22)
    {
      syslog(LOG_ERR,
             "gmac%d: %s is clause %u, only clause 22 is implemented\n",
             priv->intf, desc->name, desc->clause);
      return -ENOTSUP;
    }

  /* Prime the management engine: the first transaction after a reset can be
   * the one that fails, and it is better spent on nothing than on the address
   * the part is expected to answer at.
   */

  discard = 0;
  if (gm_mdio_read(priv, 0, MII_MSR, &discard) < 0)
    {
      /* Expected on the first transaction after a reset; the walk below is
       * what decides whether a PHY is there.
       */
    }

  /* Then walk the bus starting at the descriptor's address and wrapping.  The
   * descriptor's address is *not* skipped later in the walk: it gets a second
   * chance, because a single failed read is not evidence that nothing is
   * there.
   */

  for (i = 0; i < 32; i++)
    {
      addr = (uint8_t)((desc->address_lo + i) & 0x1f);

      if (gm_phy_present(priv, addr))
        {
          goto found;
        }
    }

  syslog(LOG_ERR, "gmac%d: %s did not answer on the MDIO bus\n", priv->intf,
         desc->name);
  priv->phyaddr = GM_PHY_ADDR_NONE;
  return -ENODEV;

found:
  priv->phyaddr = addr;

  /* Read the status twice: the link bit latches low, so the first read after
   * a link loss still says "up".
   */

  gm_mdio_read(priv, addr, MII_MSR, &bmsr);
  gm_mdio_read(priv, addr, MII_MSR, &bmsr);

  {
    uint16_t id1 = 0;
    uint16_t id2 = 0;

    gm_mdio_read(priv, addr, MII_PHYID1, &id1);
    gm_mdio_read(priv, addr, MII_PHYID2, &id2);

    syslog(LOG_INFO, "gmac%d: %s at address %u, bmsr 0x%04x id 0x%04x%04x\n",
           priv->intf, desc->name, addr, bmsr, id1, id2);
  }

  return OK;
}

/****************************************************************************
 * Name: gm_phy_start
 *
 * Description:
 *   Let the board's PHY driver configure the part and write what it can do,
 *   then restart autonegotiation.  Called when the interface comes up, which
 *is the first moment the part may be written.
 *
 *   The two hooks are plain function pointers so that this driver keeps no
 *   knowledge of any particular PHY: the board puts them in its configuration,
 *   and it is the only place that sees both this header and the PHY driver's.
 *   Without them the port still works as a clause 22 port -- autonegotiation
 *is restarted with whatever the part advertises by default.
 *
 ****************************************************************************/

static int gm_phy_start(FAR struct rk3576_gmac_s *priv)
{
  FAR const struct rk3576_gmac_config_s *cfg = priv->cfg;
  int ret;

  if (cfg->phy_init != NULL)
    {
      ret = cfg->phy_init(cfg->phy_ctx);
      if (ret < 0)
        {
          syslog(LOG_ERR, "gmac%d: PHY init failed: %d\n", priv->intf, ret);
          return ret;
        }
    }

  if (cfg->phy_advertise != NULL)
    {
      ret = cfg->phy_advertise(cfg->phy_ctx);
      if (ret < 0)
        {
          syslog(LOG_ERR, "gmac%d: PHY advertise failed: %d\n", priv->intf,
                 ret);
          return ret;
        }
    }

  return gm_mdio_write(priv, priv->phyaddr, MII_MCR,
                       MII_MCR_ANENABLE | MII_MCR_ANRESTART);
}

/****************************************************************************
 * Name: gm_phy_link
 *
 * Description:
 *   Read the link state and, once autonegotiation has finished, what was
 *   agreed.
 *
 *   Two ways to find that out, and the descriptor picks one.  A part with its
 *   own status register (status != 0xffff) is decoded with the descriptor's
 *   masks, the way imx9_enet does it.  Everything else uses clause 22:
 *   1000BASE-T from register 10 -- the only place a gigabit result appears --
 *   and 10/100 from the link partner's advertisement and our own.
 *
 * Returned Value:
 *   OK with *up set or clear, -EAGAIN while the link is up but negotiation has
 *   not finished, or a negated errno if the part could not be read.
 *
 ****************************************************************************/

static int gm_phy_link(FAR struct rk3576_gmac_s *priv,
                       FAR struct gm_phy_status_s *st, bool *up,
                       FAR uint16_t *speed, FAR bool *full_duplex)
{
  FAR const struct phy_desc_s *desc = priv->cfg->phy;
  uint16_t bmsr;
  uint16_t status;
  uint16_t lpa;
  uint16_t anar;
  uint16_t mask;
  int ret;

  *up = false;
  *speed = 0;
  *full_duplex = false;
  st->bmsr = 0;
  st->lpa = 0;

  ret = gm_mdio_read(priv, priv->phyaddr, MII_MSR, &bmsr);
  if (ret < 0)
    {
      return ret;
    }

  ret = gm_mdio_read(priv, priv->phyaddr, MII_MSR, &bmsr);
  if (ret < 0)
    {
      return ret;
    }

  st->bmsr = bmsr;

  if ((bmsr & MII_MSR_LINKSTATUS) == 0)
    {
      return OK;
    }

  *up = true;

  if ((bmsr & MII_MSR_ANEGCOMPLETE) == 0)
    {
      return -EAGAIN;
    }

  if (desc->status != 0xffff)
    {
      ret = gm_mdio_read(priv, priv->phyaddr, (uint8_t)desc->status, &status);
      if (ret < 0)
        {
          return ret;
        }

      mask = desc->speed_mask;
      *full_duplex = (status & desc->duplex) != 0;

      if ((status & mask) == desc->mbps1000)
        {
          *speed = 1000;
        }
      else if ((status & mask) == desc->mbps100)
        {
          *speed = 100;
        }
      else if ((status & mask) == desc->mbps10)
        {
          *speed = 10;
        }
      else
        {
          return -EAGAIN;
        }

      return OK;
    }

  ret = gm_mdio_read(priv, priv->phyaddr, GM_PHY_STAT1000, &status);
  if (ret < 0)
    {
      return ret;
    }

  if ((status & (GM_PHY_LPA_1000FULL | GM_PHY_LPA_1000HALF)) != 0)
    {
      *speed = 1000;
      *full_duplex = (status & GM_PHY_LPA_1000FULL) != 0;
      return OK;
    }

  ret = gm_mdio_read(priv, priv->phyaddr, MII_LPA, &lpa);
  if (ret < 0)
    {
      return ret;
    }

  st->lpa = lpa;

  ret = gm_mdio_read(priv, priv->phyaddr, MII_ADVERTISE, &anar);
  if (ret < 0)
    {
      return ret;
    }

  /* Only what both sides offered can be used. */

  lpa &= anar & GM_PHY_ANAR_10_100;

  if (lpa & MII_ADVERTISE_100BASETXFULL)
    {
      *speed = 100;
      *full_duplex = true;
    }
  else if (lpa & MII_ADVERTISE_100BASETXHALF)
    {
      *speed = 100;
      *full_duplex = false;
    }
  else if (lpa & MII_ADVERTISE_10BASETXFULL)
    {
      *speed = 10;
      *full_duplex = true;
    }
  else if (lpa & MII_ADVERTISE_10BASETXHALF)
    {
      *speed = 10;
      *full_duplex = false;
    }
  else
    {
      /* Link is up but nothing in common: keep polling. */

      return -EAGAIN;
    }

  return OK;
}

/****************************************************************************
 * Name: gm_phy_trace
 *
 * Description:
 *   Print the PHY's raw state, including the two registers that say whether
 *   anything is out there: BMCR (is autonegotiation enabled, is the part
 *   powered down or isolated) and LPA (did a link partner advertise anything
 *   at all -- zero means nothing was heard).
 *
 *   Called from the link poll only when the decoded state says "down" and the
 *   BMSR/LPA pair has changed since the last print, so a board with no cable
 *   logs once and then stays quiet, while plugging one in, or autonegotiation
 *   making progress, always shows up.
 *
 ****************************************************************************/

static void gm_phy_trace(FAR struct rk3576_gmac_s *priv,
                         FAR const struct gm_phy_status_s *st)
{
  uint16_t bmcr = 0;
  uint16_t anar = 0;
  uint16_t stat1000 = 0;
  uint16_t aner = 0;
  uint16_t ctrl1000 = 0;

  gm_mdio_read(priv, priv->phyaddr, MII_MCR, &bmcr);
  gm_mdio_read(priv, priv->phyaddr, MII_ADVERTISE, &anar);
  gm_mdio_read(priv, priv->phyaddr, GM_PHY_STAT1000, &stat1000);
  gm_mdio_read(priv, priv->phyaddr, GM_PHY_ANER, &aner);
  gm_mdio_read(priv, priv->phyaddr, GM_PHY_CTRL1000, &ctrl1000);

  /* ANER says whether the partner was heard at all and whether parallel
   * detection faulted; ctrl1000 is what we advertise for gigabit.
   */

  syslog(LOG_INFO,
         "gmac%d: no link: bmcr 0x%04x bmsr 0x%04x anar 0x%04x lpa 0x%04x "
         "stat1000 0x%04x aner 0x%04x ctrl1000 0x%04x\n",
         priv->intf, bmcr, st->bmsr, anar, st->lpa, stat1000, aner, ctrl1000);
}

/****************************************************************************
 * Private Functions: platform layer
 ****************************************************************************/

/****************************************************************************
 * Name: gm_clocks
 *
 * Description:
 *   Open the clock path and set the RGMII line clock to 125 MHz.
 *
 *   Four clocks, and they are three different things:
 *
 *     aclk/pclk_gmac<n>      the bus clocks; without them no register answers
 *     clk_gmac<n>_125m_src   the MAC's transmit clock.  The RGMII TXC the SoC
 *                            drives to the PHY comes from here, and it has to
 *                            be generated on the SoC because the route that
 *                            would have brought the PHY's own 125 MHz back --
 *                            PHY CLKOUT to GMACn_MCLKINOUT_M0 -- is populated
 *                            with NC resistors on this board.  This is *not*
 *                            the PHY's 25 MHz reference: each PHY has its own
 *                            crystal and takes nothing from the SoC.
 *     pclk_sdgmac_grf        the register file clock for GMACn_CON
 *
 *   Enabling a leaf is enough to raise its ancestors -- NuttX recurses into
 *   clk->parent -- which is why pclk_sdgmac_root and aclk_sdgmac_root do not
 *   appear here.  pclk_sdgmac_grf is the exception: it has no child, so
 *   nothing else would ever enable it.
 *
 *   The 125 MHz clock is a divider off CPLL with no parent mux, so its rate
 *   depends on what CPLL happens to be; the reset divider of 8 only gives
 *   125 MHz if CPLL is 1 GHz.  Set it and report what comes back instead of
 *   trusting the reset value.
 *
 ****************************************************************************/

static int gm_clocks(FAR struct rk3576_gmac_s *priv)
{
  char name[32];
  uint32_t rate;
  int ret;

  snprintf(name, sizeof(name), "aclk_gmac%d", priv->intf);
  priv->aclk = clk_get(name);
  snprintf(name, sizeof(name), "pclk_gmac%d", priv->intf);
  priv->pclk = clk_get(name);
  snprintf(name, sizeof(name), "clk_gmac%d_125m_src", priv->intf);
  priv->clk125 = clk_get(name);
  priv->grfclk = clk_get("pclk_sdgmac_grf");

  if (priv->aclk == NULL || priv->pclk == NULL || priv->clk125 == NULL ||
      priv->grfclk == NULL)
    {
      syslog(LOG_ERR, "gmac%d: a clock is missing from the tree\n",
             priv->intf);
      return -ENODEV;
    }

  ret = clk_set_rate(priv->clk125, 125000000);
  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: clk_set_rate(125m) failed: %d\n", priv->intf,
             ret);
      return ret;
    }

  ret = clk_enable(priv->grfclk);
  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: clk_enable(pclk_sdgmac_grf) failed: %d\n",
             priv->intf, ret);
      return ret;
    }

  ret = clk_enable(priv->pclk);
  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: clk_enable(pclk_gmac) failed: %d\n", priv->intf,
             ret);
      return ret;
    }

  ret = clk_enable(priv->aclk);
  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: clk_enable(aclk_gmac) failed: %d\n", priv->intf,
             ret);
      return ret;
    }

  ret = clk_enable(priv->clk125);
  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: clk_enable(125m) failed: %d\n", priv->intf,
             ret);
      return ret;
    }

  rate = clk_get_rate(priv->clk125);
  if (rate != 125000000)
    {
      /* Not fatal, but it has to be loud.  The clock is a divider-only clock
       * off CPLL, whose rate is whatever the bootloader left there; if it is
       * not a whole fraction of 125 MHz there is no divider that reaches it,
       * and the port will not train at gigabit however right everything else
       * is.  Refusing to register would hide that behind "no interface",
       * which is a worse report than this line.
       */

      syslog(LOG_ERR,
             "gmac%d: RGMII line clock is %" PRIu32
             " Hz, not 125 MHz: CPLL cannot be divided to 125 MHz, so 1000M "
             "will not train\n",
             priv->intf, rate);
    }

  return OK;
}

/****************************************************************************
 * Name: gm_reset
 *
 * Description:
 *   Reset the controller through CRU_SOFTRST_CON42 and verify it came back.
 *
 *   **Deliberately no DMA software reset.**  The DWC databook's SWR bit
 *   (DMA_MODE[0]) "is automatically cleared after the reset operation is
 *   complete in all DWC_ether_qos clock domains", and the TRM adds that "it
 *   is essential that all PHY input clocks ... are present for software reset
 *   completion".  In RGMII that includes RXC, which the PHY drives and which
 *   may not be running before the link has trained -- the first bring-up run
 *   hung exactly there.  The CRU reset needs no IP clock at all, resets the
 *   whole block including the register file, and is what the vendor DTB's
 *   `resets` property names.  SWR would be the tool for a warm restart with
 *   the link already up; nothing here needs one.
 *
 *   The two CRU bits are the per-port ones only.  The sdgmac_biu and
 *   sdgmac_grf bits in the same register are shared with the other port and
 *   with the register file this driver programs, so they are left to whatever
 *   the bootloader decided.  Both bits are written with the TRM's own reading
 *   -- "when high, reset relative logic", so 1 asserts and 0 releases -- and
 *   the reset value being 0 is what makes that the only reading that leaves
 *   the block usable out of reset.
 *
 ****************************************************************************/

static int gm_reset(FAR struct rk3576_gmac_s *priv)
{
  uintptr_t cru = RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(GM_SOFTRST_CON);
  uint32_t bits =
      GM_SOFTRST_ARESET(priv->intf) | GM_SOFTRST_PRESET(priv->intf);
  uint32_t after;

  putreg32((bits << 16) | bits, cru);
  up_udelay(10);
  putreg32(bits << 16, cru);
  up_udelay(1000);

  after = getreg32(cru) & bits;
  if (after != 0)
    {
      syslog(LOG_ERR, "gmac%d: reset bits 0x%04" PRIx32 " did not release\n",
             priv->intf, after);
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: gm_rgmii_mode
 *
 * Description:
 *   Select RGMII, the CRU as the TX clock source, and ungate the memory
 *   clock.  The TX clock divider is not set here: it follows the negotiated
 *   speed (see gm_set_speed).
 *
 ****************************************************************************/

static int gm_rgmii_mode(FAR struct rk3576_gmac_s *priv)
{
  uint32_t reg =
      RK3576_SDGMAC_GRF_ADDR + RK3576_SDGMAC_GRF_GMAC_CON(priv->intf);

  return gm_grf_update(
      priv, reg,
      RK3576_SDGMAC_CON_RMII_MODE | RK3576_SDGMAC_CON_TXCLK_SEL |
          RK3576_SDGMAC_CON_CLK_GATING_EN | RK3576_SDGMAC_CON_RMII_CLK_GATE,
      0, "SDGMAC_GRF mode");
}

/****************************************************************************
 * Name: gm_delays
 *
 * Description:
 *   Program the RGMII TX/RX delay lines in the VCCIO IOC.
 *
 *   This is the only RGMII timing knob software owns -- the PHY's side is
 *   strap-only -- and it is what makes `rgmii-rxid` work on this board: the
 *   PHY adds the RX delay in its own pins, so the SoC's RX delay stays off
 *   and the SoC supplies the TX delay.  Counting either delay twice is the
 *   kind of mistake that still links at 1000M and drops most frames.
 *
 ****************************************************************************/

static int gm_delays(FAR struct rk3576_gmac_s *priv)
{
  uint32_t reg = RK3576_VCCIO_IOC_ADDR + priv->cfg->ioc_offset;
  uint32_t mask = RK3576_VCCIO_DLY_TX_ENA | RK3576_VCCIO_DLY_TX_NUM_M |
                  RK3576_VCCIO_DLY_RX_ENA | RK3576_VCCIO_DLY_RX_NUM_M;
  uint32_t value = 0;

  /* The offset and the taps come from the board, so they are checked rather
   * than trusted: an offset that is not one of the four delay-line registers
   * would quietly write some other VCCIO configuration, and a tap count over
   * 127 would be silently truncated to the seven bits the field has.
   */

  switch (priv->cfg->ioc_offset)
    {
      case RK3576_VCCIO_IOC_MISC_CON2:
      case RK3576_VCCIO_IOC_MISC_CON3:
      case RK3576_VCCIO_IOC_MISC_CON4:
      case RK3576_VCCIO_IOC_MISC_CON5:
        break;

      default:
        syslog(LOG_ERR, "gmac%d: 0x%03x is not an RGMII delay register\n",
               priv->intf, priv->cfg->ioc_offset);
        return -EINVAL;
    }

  if (priv->cfg->tx_delay > 127 || priv->cfg->rx_delay > 127)
    {
      syslog(LOG_ERR,
             "gmac%d: %u/%u delay taps do not fit the 7-bit delay lines\n",
             priv->intf, priv->cfg->tx_delay, priv->cfg->rx_delay);
      return -EINVAL;
    }

  if (priv->cfg->tx_delay_ena)
    {
      value |= RK3576_VCCIO_DLY_TX_ENA |
               ((uint32_t)priv->cfg->tx_delay << RK3576_VCCIO_DLY_TX_NUM_S);
    }

  if (priv->cfg->rx_delay_ena)
    {
      value |= RK3576_VCCIO_DLY_RX_ENA |
               ((uint32_t)priv->cfg->rx_delay << RK3576_VCCIO_DLY_RX_NUM_S);
    }

  return gm_grf_update(priv, reg, mask, value, "VCCIO RGMII delay");
}

/****************************************************************************
 * Name: gm_set_speed
 *
 * Description:
 *   Program the MAC's speed/duplex and the RGMII TX clock divider.  Both
 *   have to move together: the MAC expects a 125 MHz TX clock at 1000M and
 *   derives the slower GMII rates from it internally, while the RGMII
 *   module's TXC output is divided in SDGMAC_GRF -- 25 MHz at 100M, 2.5 MHz
 *   at 10M.
 *
 *   Called with the MAC's transmitter and receiver disabled.
 *
 ****************************************************************************/

static int gm_set_speed(FAR struct rk3576_gmac_s *priv, uint16_t speed,
                        bool full_duplex)
{
  uint32_t config;
  uint32_t div;

  config = gm_getreg(priv, RK3576_GMAC_MAC_CONFIGURATION);
  config &= ~(RK3576_GMAC_CONFIG_PS | RK3576_GMAC_CONFIG_FES |
              RK3576_GMAC_CONFIG_DM);

  if (full_duplex)
    {
      config |= RK3576_GMAC_CONFIG_DM;
    }

  switch (speed)
    {
      case 1000:
        /* PS = 0: GMII, 1000M.  TXC stays at 125 MHz. */

        div = RK3576_SDGMAC_CON_TXCLK_DIV_125M;
        break;

      case 100:
        config |= RK3576_GMAC_CONFIG_PS | RK3576_GMAC_CONFIG_FES;
        div = RK3576_SDGMAC_CON_TXCLK_DIV_25M;
        break;

      case 10:
        config |= RK3576_GMAC_CONFIG_PS;
        div = RK3576_SDGMAC_CON_TXCLK_DIV_2P5M;
        break;

      default:
        return -EINVAL;
    }

  gm_putreg(priv, RK3576_GMAC_MAC_CONFIGURATION, config);

  priv->hw_speed = speed;
  priv->hw_duplex = full_duplex;

  return gm_grf_update(
      priv, RK3576_SDGMAC_GRF_ADDR + RK3576_SDGMAC_GRF_GMAC_CON(priv->intf),
      RK3576_SDGMAC_CON_TXCLK_DIV_M, div, "SDGMAC_GRF TX clock divider");
}

/****************************************************************************
 * Name: gm_mac_address
 *
 * Description:
 *   Program MAC address 0 and the receive filter.
 *
 *   Multicast is accepted wholesale (PM), which makes addmac/rmmac no-ops
 *   that succeed: the frame is already accepted by the time the stack asks
 *   for it.  Unicast is perfect-filtered on address 0, broadcast passes.
 *
 ****************************************************************************/

static void gm_mac_address(FAR struct rk3576_gmac_s *priv)
{
  FAR const uint8_t *mac = priv->cfg->mac;

  gm_putreg(priv, RK3576_GMAC_MAC_ADDRESS0_HIGH,
            RK3576_GMAC_ADDR_HIGH_AE | ((uint32_t)mac[5] << 8) |
                (uint32_t)mac[4]);
  gm_putreg(priv, RK3576_GMAC_MAC_ADDRESS0_LOW,
            ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
                ((uint32_t)mac[1] << 8) | (uint32_t)mac[0]);

  gm_putreg(priv, RK3576_GMAC_MAC_PACKET_FILTER, RK3576_GMAC_FILTER_PM);
}

/****************************************************************************
 * Private Functions: DMA rings
 ****************************************************************************/

/****************************************************************************
 * Name: gm_rx_arm_one
 *
 * Description:
 *   Give descriptor `index` a fresh netpkt and hand it back to the DMA.
 *
 *   The netpkt's own data area *is* the DMA buffer -- no bounce buffer, no
 *   copy -- which is the whole reason this driver is written against the
 *   lower half.
 *
 ****************************************************************************/

static int gm_rx_arm_one(FAR struct rk3576_gmac_s *priv, uint32_t index)
{
  FAR struct rk3576_gmac_desc_s *desc = gm_desc(priv->rxring, index);
  FAR netpkt_t *pkt;

  pkt = netpkt_alloc(&priv->dev, NETPKT_RX);
  if (pkt == NULL)
    {
      return -ENOMEM;
    }

  priv->rxpkt[index] = pkt;

  desc->des0 = (uint32_t)up_addrenv_va_to_pa(netpkt_getdata(&priv->dev, pkt));
  desc->des1 = 0;
  desc->des2 = 0;

  /* RDES3[30] is the receive descriptor's interrupt enable: the TRM says the
   * RI status bit is not set for a packet whose descriptor leaves it clear
   * ("because of the setting of Interrupt Enable bit ... RDES3[30]").  Without
   * it the ring silently fills: frames land in the descriptors, no interrupt
   * announces them, and the only wakeup is the RBU the DMA raises when it
   * finally reaches the tail -- which drains whatever accumulated and then
   * stops for good.  That looked like "the first ping receives nothing, the
   * second delivers GM_RX_DESC - 1 frames at once and nothing after".
   */

  desc->des3 =
      RK3576_GMAC_RDES3_OWN | RK3576_GMAC_RDES3_IOC | RK3576_GMAC_RDES3_BUF1V;

  gm_desc_sync(desc);
  return OK;
}

/****************************************************************************
 * Name: gm_rings_alloc
 *
 * Description:
 *   Allocate the two descriptor rings and fill the RX one.
 *
 ****************************************************************************/

static int gm_rings_alloc(FAR struct rk3576_gmac_s *priv)
{
  uint32_t i;

  /* Keep the ring itself line-aligned: the individual descriptors are packed,
   * but the first one should not share a line with whatever precedes it.
   */

  priv->txring = kmm_memalign(64, GM_DESC_STRIDE * GM_TX_DESC);
  priv->rxring = kmm_memalign(64, GM_DESC_STRIDE * GM_RX_DESC);

  if (priv->txring == NULL || priv->rxring == NULL)
    {
      syslog(LOG_ERR, "gmac%d: descriptor rings: out of memory\n", priv->intf);
      return -ENOMEM;
    }

  memset(priv->txring, 0, GM_DESC_STRIDE * GM_TX_DESC);
  memset(priv->rxring, 0, GM_DESC_STRIDE * GM_RX_DESC);
  memset(priv->txpkt, 0, sizeof(priv->txpkt));
  memset(priv->rxpkt, 0, sizeof(priv->rxpkt));

  priv->txnow = 0;
  priv->txdone = 0;
  priv->rxnow = 0;

  for (i = 0; i < GM_RX_DESC; i++)
    {
      if (gm_rx_arm_one(priv, i) < 0)
        {
          syslog(LOG_ERR, "gmac%d: RX ring: only %" PRIu32 " of %u buffers\n",
                 priv->intf, i, GM_RX_DESC);

          /* Whatever was armed stays armed; the caller unwinds. */

          return -ENOMEM;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: gm_rings_free
 *
 * Description:
 *   Release every packet the rings still hold and the rings themselves.  The
 *   DMA must already be stopped: a buffer is never freed while the MAC owns
 *   the descriptor that points at it.
 *
 ****************************************************************************/

static void gm_rings_free(FAR struct rk3576_gmac_s *priv)
{
  uint32_t i;

  for (i = 0; i < GM_TX_DESC; i++)
    {
      if (priv->txpkt[i] != NULL)
        {
          netpkt_free(&priv->dev, priv->txpkt[i], NETPKT_TX);
          priv->txpkt[i] = NULL;
        }
    }

  for (i = 0; i < GM_RX_DESC; i++)
    {
      if (priv->rxpkt[i] != NULL)
        {
          netpkt_free(&priv->dev, priv->rxpkt[i], NETPKT_RX);
          priv->rxpkt[i] = NULL;
        }
    }

  if (priv->txring != NULL)
    {
      kmm_free(priv->txring);
      priv->txring = NULL;
    }

  if (priv->rxring != NULL)
    {
      kmm_free(priv->rxring);
      priv->rxring = NULL;
    }
}

/****************************************************************************
 * Name: gm_dma_setup
 *
 * Description:
 *   Program the channel: burst sizes, RX buffer size, the two ring bases and
 *   their lengths, then the RX tail pointer.
 *
 ****************************************************************************/

static void gm_dma_setup(FAR struct rk3576_gmac_s *priv)
{
  /* Burst lengths the vendor DTB names.  The outstanding-request limits it
   * also names are left at their reset values -- see the note in
   * hardware/rk3576_gmac.h.
   */

  gm_putreg(priv, RK3576_GMAC_DMA_SYSBUS_MODE,
            RK3576_GMAC_SYSBUS_BLEN4 | RK3576_GMAC_SYSBUS_BLEN8 |
                RK3576_GMAC_SYSBUS_BLEN16);

  /* 16-beat bursts in both directions, no 8xPBL. */

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL,
            16u << RK3576_GMAC_CH0TX_TXPBL_S);
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL,
            (16u << RK3576_GMAC_CH0RX_RXPBL_S) |
                ((uint32_t)GM_BUFSIZE << RK3576_GMAC_CH0RX_RBSZ_S));

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_TXDESC_LIST_ADDR,
            (uint32_t)up_addrenv_va_to_pa(priv->txring));
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_RXDESC_LIST_ADDR,
            (uint32_t)up_addrenv_va_to_pa(priv->rxring));

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_TXDESC_RING_LEN, GM_TX_DESC - 1);
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_RXDESC_RING_LEN, GM_RX_DESC - 1);

  /* The RX tail pointer names the last descriptor the DMA may use.  Pointing
   * it at the end of the ring implies "all of them", which is the same value
   * as "none", so the last descriptor is held back and the rest are live.
   */

  gm_putreg(
      priv, RK3576_GMAC_DMA_CH0_RXDESC_TAIL_PTR,
      (uint32_t)up_addrenv_va_to_pa(gm_desc(priv->rxring, GM_RX_DESC - 1)));
}

/****************************************************************************
 * Name: gm_tx_reap
 *
 * Description:
 *   Free the transmit packets the DMA has finished with.  Called from the
 *   interrupt and from reclaim, with the driver lock held.
 *
 ****************************************************************************/

static void gm_tx_reap(FAR struct rk3576_gmac_s *priv)
{
  FAR struct rk3576_gmac_desc_s *desc;

  while (priv->txnow != priv->txdone)
    {
      desc = gm_desc(priv->txring, priv->txdone);
      gm_desc_inval(desc);

      if ((desc->des3 & RK3576_GMAC_TDES3_OWN) != 0)
        {
          break;
        }

      netpkt_free(&priv->dev, priv->txpkt[priv->txdone], NETPKT_TX);
      priv->txpkt[priv->txdone] = NULL;
      priv->txdone = (priv->txdone + 1) % GM_TX_DESC;
    }
}

/****************************************************************************
 * Private Functions: interrupt
 ****************************************************************************/

static int gm_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3576_gmac_s *priv = arg;
  uint32_t status;
  bool txdone = false;
  bool rxready = false;
  irqstate_t flags;

  UNUSED(irq);
  UNUSED(context);

  flags = spin_lock_irqsave(&priv->lock);

  status = gm_getreg(priv, RK3576_GMAC_DMA_CH0_STATUS);
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_STATUS, status & GM_IRQ_STATUS);

  if ((status & (RK3576_GMAC_CH0STS_FBE | RK3576_GMAC_CH0STS_CDE |
                 RK3576_GMAC_CH0STS_RPS)) != 0)
    {
      syslog(LOG_ERR, "gmac%d: DMA fault, status 0x%08" PRIx32 "\n",
             priv->intf, status);
      NETDEV_RXERRORS(&priv->dev.netdev);
    }

  if ((status & RK3576_GMAC_CH0STS_TI) != 0)
    {
      gm_tx_reap(priv);
      txdone = true;
    }

  if ((status & (RK3576_GMAC_CH0STS_RI | RK3576_GMAC_CH0STS_RBU)) != 0)
    {
      rxready = true;
    }

  spin_unlock_irqrestore(&priv->lock, flags);

  /* Both of these only queue work (or post a semaphore), which is what makes
   * them safe from an interrupt.  The carrier notifier is not: it takes the
   * network lock, so it runs from gm_link_work instead.
   */

  if (txdone)
    {
      netdev_lower_txdone(&priv->dev);
    }

  if (rxready)
    {
      netdev_lower_rxready(&priv->dev);
    }

  return OK;
}

/****************************************************************************
 * Private Functions: netdev operations
 ****************************************************************************/

/****************************************************************************
 * Name: gm_transmit
 *
 * Description:
 *   Hand one packet to the DMA.  The packet belongs to the driver until the
 *   TX-complete interrupt frees it, which is why it is stored in the ring's
 *   packet table before the descriptor is published.
 *
 ****************************************************************************/

static int gm_transmit(FAR struct netdev_lowerhalf_s *dev, FAR netpkt_t *pkt)
{
  FAR struct rk3576_gmac_s *priv = (FAR struct rk3576_gmac_s *)dev;
  FAR struct rk3576_gmac_desc_s *desc;
  size_t len = netpkt_getdatalen(dev, pkt);
  uint32_t next;
  irqstate_t flags;
  bool full;

  if (len == 0 || len > GM_BUFSIZE)
    {
      NETDEV_TXERRORS(&dev->netdev);
      return -EINVAL;
    }

  if (!IFF_IS_RUNNING(dev->netdev.d_flags))
    {
      return -ENETDOWN;
    }

  flags = spin_lock_irqsave(&priv->lock);

  next = (priv->txnow + 1) % GM_TX_DESC;
  full = (next == priv->txdone);
  if (full)
    {
      /* Ring full: take back what has already completed before telling the
       * stack there is no room.
       */

      gm_tx_reap(priv);
      full = (next == priv->txdone);
    }

  if (full)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      return -ENOMEM;
    }

  desc = gm_desc(priv->txring, priv->txnow);
  priv->txpkt[priv->txnow] = pkt;

  desc->des0 = (uint32_t)up_addrenv_va_to_pa(netpkt_getdata(dev, pkt));
  desc->des1 = 0;
  desc->des2 =
      RK3576_GMAC_TDES2_IOC | ((uint32_t)len & RK3576_GMAC_TDES2_B1L_M);
  desc->des3 = RK3576_GMAC_TDES3_OWN | RK3576_GMAC_TDES3_FD |
               RK3576_GMAC_TDES3_LD | ((uint32_t)len & RK3576_GMAC_TDES3_FL_M);

  gm_buf_clean(netpkt_getdata(dev, pkt), len);
  gm_desc_sync(desc);

  /* Publish the descriptor into the ring exactly the way the receive side
   * does it: advance the tail pointer to the descriptor that was just made
   * valid.  Pointing it at the next, still-unowned descriptor instead leaves
   * the transmit FSM looking at a descriptor nobody owns, and it stops there
   * for good -- the TRM's TBU entry lists "advance the Transmit Descriptor
   * Tail Pointer" as the way to resume, and the tail the hardware is told
   * about has to be a descriptor it may actually use.
   */

  priv->txnow = next;

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_TXDESC_TAIL_PTR,
            (uint32_t)up_addrenv_va_to_pa(desc));

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

/****************************************************************************
 * Name: gm_receive
 *
 * Description:
 *   Take one completed packet off the RX ring and return it to the stack.
 *
 *   The frame is already in the netpkt that goes upward, so what is left is
 *   to give the descriptor a new buffer.  That allocation can fail -- the IOB
 *   pool is shared with the whole system -- and when it does the ring must
 *   not be advanced: the descriptor keeps its completed frame, the ring
 *   stalls, and a delayed retry re-arms it.  Without that retry the ring
 *   would stay stalled forever, because a stalled RX ring generates no
 *   further interrupts.
 *
 ****************************************************************************/

static FAR netpkt_t *gm_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3576_gmac_s *priv = (FAR struct rk3576_gmac_s *)dev;
  FAR struct rk3576_gmac_desc_s *desc;
  FAR netpkt_t *pkt;
  uint32_t index;
  uint32_t status;
  uint32_t len;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);

  for (;;)
    {
      index = priv->rxnow;
      desc = gm_desc(priv->rxring, index);
      gm_desc_inval(desc);

      if ((desc->des3 & RK3576_GMAC_RDES3_OWN) != 0)
        {
          spin_unlock_irqrestore(&priv->lock, flags);
          return NULL;
        }

      /* Keep the DMA's status word: re-arming the descriptor below writes
       * over it.
       */

      status = desc->des3;
      len = status & RK3576_GMAC_RDES3_PL_M;
      pkt = priv->rxpkt[index];

      gm_buf_inval(netpkt_getdata(dev, pkt),
                   len <= GM_BUFSIZE ? len : GM_BUFSIZE);

      /* Give the descriptor a new buffer before the frame is handed up: if
       * that fails, the frame stays where it is and the ring waits.
       */

      if (gm_rx_arm_one(priv, index) < 0)
        {
          if (!priv->rxstarved)
            {
              priv->rxstarved = true;
              syslog(LOG_ERR,
                     "gmac%d: out of buffers for the RX ring; frames stay in "
                     "the ring until one is free\n",
                     priv->intf);
            }

          if (work_available(&priv->rxretry))
            {
              work_queue(LPWORK, &priv->rxretry, gm_rx_retry, priv,
                         MSEC2TICK(GM_RXRETRY_MS));
            }

          spin_unlock_irqrestore(&priv->lock, flags);
          return NULL;
        }

      priv->rxnow = (index + 1) % GM_RX_DESC;

      gm_putreg(priv, RK3576_GMAC_DMA_CH0_RXDESC_TAIL_PTR,
                (uint32_t)up_addrenv_va_to_pa(desc));

      /* No StartRx command here.  The TRM's RBU entry says that a receive
       * suspended on a descriptor it does not own is resumed by "advancing the
       * Receive Descriptor Tail Pointer" in ring mode, and only asks for a
       * receive poll demand when the tail pointer is *not* used.  Issuing
       * StartRx per frame instead reset the receive process on every packet:
       * the ring delivered GM_RX_DESC - 1 frames -- exactly up to the tail it
       * was given at ifup -- and then stopped receiving altogether.
       */

      if ((status & RK3576_GMAC_RDES3_ES) != 0 || len == 0 || len > GM_BUFSIZE)
        {
          /* A damaged or impossible frame: count it and look at the next
           * descriptor rather than stopping the poll.
           */

          NETDEV_RXERRORS(&dev->netdev);
          netpkt_free(dev, pkt, NETPKT_RX);
          continue;
        }

      spin_unlock_irqrestore(&priv->lock, flags);

      netpkt_setdatalen(dev, pkt, len);

      return pkt;
    }
}

/****************************************************************************
 * Name: gm_rx_retry
 *
 * Description:
 *   Re-arm a stalled RX ring and ask the stack to poll it again.
 *
 ****************************************************************************/

static void gm_rx_retry(FAR void *arg)
{
  FAR struct rk3576_gmac_s *priv = arg;

  if (priv->started)
    {
      netdev_lower_rxready(&priv->dev);
    }
}

/****************************************************************************
 * Name: gm_reclaim
 *
 * Description:
 *   The TX quota is exhausted, so collect what the DMA has finished and let
 *   the upper half try again.
 *
 ****************************************************************************/

static void gm_reclaim(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3576_gmac_s *priv = (FAR struct rk3576_gmac_s *)dev;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);
  gm_tx_reap(priv);
  spin_unlock_irqrestore(&priv->lock, flags);
}

#ifdef CONFIG_NET_MCASTGROUP
/****************************************************************************
 * Name: gm_addmac / gm_rmmac
 *
 * Description:
 *   Multicast is accepted wholesale at the MAC (see gm_mac_address), so a
 *   filter request has nothing left to do.  Succeeding is the truth: the
 *   frame is already being received.
 *
 ****************************************************************************/

static int gm_addmac(FAR struct netdev_lowerhalf_s *dev,
                     FAR const uint8_t *mac)
{
  UNUSED(dev);
  UNUSED(mac);
  return OK;
}

static int gm_rmmac(FAR struct netdev_lowerhalf_s *dev, FAR const uint8_t *mac)
{
  UNUSED(dev);
  UNUSED(mac);
  return OK;
}
#endif

#ifdef CONFIG_NETDEV_IOCTL
/****************************************************************************
 * Name: gm_ioctl
 *
 * Description:
 *   SIOCGMIIPHY / SIOCGMIIREG / SIOCSMIIREG.  The register access goes
 *   straight to the MDIO bus at the address the caller asked for, because
 *   that is what a raw register ioctl means; the PHY layer is for the traffic
 *   the driver itself generates.
 *
 ****************************************************************************/

static int gm_ioctl(FAR struct netdev_lowerhalf_s *dev, int cmd,
                    unsigned long arg)
{
  FAR struct rk3576_gmac_s *priv = (FAR struct rk3576_gmac_s *)dev;
  FAR struct mii_ioctl_data_s *mii = (FAR struct mii_ioctl_data_s *)arg;
  uint16_t value;

  switch (cmd)
    {
      case SIOCGMIIPHY:
        mii->phy_id = priv->phyaddr;
        return OK;

      case SIOCGMIIREG:
        if (gm_mdio_read(priv, (uint8_t)mii->phy_id, (uint8_t)mii->reg_num,
                         &value) < 0)
          {
            return -EIO;
          }

        mii->val_out = value;
        return OK;

      case SIOCSMIIREG:
        return gm_mdio_write(priv, (uint8_t)mii->phy_id, (uint8_t)mii->reg_num,
                             mii->val_in);

      default:
        return -ENOTTY;
    }
}
#endif

/****************************************************************************
 * Private Functions: MAC start/stop
 ****************************************************************************/

/****************************************************************************
 * Name: gm_mac_start
 *
 * Description:
 *   Bring the MAC and the DMA up in the order the controller expects: MTL
 *   queues first, then the MAC's address, filter and configuration, then the
 *   DMA rings, then the enables.
 *
 ****************************************************************************/

static void gm_mac_start(FAR struct rk3576_gmac_s *priv)
{
  uint32_t hwfeat;
  uint32_t txqsz;
  uint32_t rxqsz;

  /* MTL: store-and-forward in both directions, one TX queue enabled.
   *
   * The queue size fields live in these same two registers and are not
   * decoration.  TQS/RQS say how many 256-byte blocks of the FIFO the queue
   * may use, and with store-and-forward on, the MTL has to hold a whole frame
   * before it starts transmitting -- so a queue smaller than the frame means
   * the frame can never be sent: the FIFO fills, the transmit underflows, the
   * frame leaves truncated and the peer drops it.  Reset leaves both fields at
   * zero, which the TRM reads as 256 bytes, and writing these registers
   * without them -- as this did -- puts them back there.  The result was a
   * port on which ping worked up to a 224-byte frame and silently lost
   * everything longer: the sender saw a completed descriptor, the receiver saw
   * a bad frame, and no counter anywhere moved.
   *
   * So derive them from what the hardware reports in MAC_HW_FEATURE1 rather
   * than guessing: Log2(bytes)-7, per direction, read-only.
   */

  hwfeat = gm_getreg(priv, RK3576_GMAC_MAC_HW_FEATURE1);
  txqsz = ((hwfeat & RK3576_GMAC_HWFEAT1_TXFIFOSZ_M) >>
           RK3576_GMAC_HWFEAT1_TXFIFOSZ_S);
  rxqsz = ((hwfeat & RK3576_GMAC_HWFEAT1_RXFIFOSZ_M) >>
           RK3576_GMAC_HWFEAT1_RXFIFOSZ_S);

  /* In 256-byte blocks, and inside what each field can hold: the two fields
   * are not the same width, and a 16 KB TX FIFO is one block more than TQS can
   * express.  The shift is bounded first so that a field the hardware reports
   * as reserved cannot turn into an undefined shift.
   */

  txqsz = txqsz > 10 ? 10 : txqsz;
  rxqsz = rxqsz > 10 ? 10 : rxqsz;

  txqsz = (1u << (txqsz + 7)) / 256;
  if (txqsz > (RK3576_GMAC_MTL_TXQ0_TQS_M >> RK3576_GMAC_MTL_TXQ0_TQS_S))
    {
      txqsz = RK3576_GMAC_MTL_TXQ0_TQS_M >> RK3576_GMAC_MTL_TXQ0_TQS_S;
    }

  rxqsz = (1u << (rxqsz + 7)) / 256;
  if (rxqsz > (RK3576_GMAC_MTL_RXQ0_RQS_M >> RK3576_GMAC_MTL_RXQ0_RQS_S))
    {
      rxqsz = RK3576_GMAC_MTL_RXQ0_RQS_M >> RK3576_GMAC_MTL_RXQ0_RQS_S;
    }

  gm_putreg(priv, RK3576_GMAC_MTL_TXQ0_OPERATION_MODE,
            RK3576_GMAC_MTL_TXQ0_TSF | RK3576_GMAC_MTL_TXQ0_TXQEN_ENABLE |
                (txqsz << RK3576_GMAC_MTL_TXQ0_TQS_S));
  gm_putreg(priv, RK3576_GMAC_MTL_RXQ0_OPERATION_MODE,
            RK3576_GMAC_MTL_RXQ0_RSF |
                (rxqsz << RK3576_GMAC_MTL_RXQ0_RQS_S));

  /* MAC: RX queue 0 enabled for generic traffic. */

  gm_putreg(priv, RK3576_GMAC_MAC_RXQ_CTRL0,
            RK3576_GMAC_RXQ_CTRL0_RXQ0EN_GENERIC);
  gm_mac_address(priv);

  /* Strip the pad and the CRC on receive: the stack wants the frame, not the
   * wire.  Jabber disabled, jumbo and 2K packets off.
   */

  gm_putreg(priv, RK3576_GMAC_MAC_CONFIGURATION,
            RK3576_GMAC_CONFIG_JD | RK3576_GMAC_CONFIG_ACS |
                RK3576_GMAC_CONFIG_CST);

  /* Until the link settles the MAC runs at gigabit; gm_link_work corrects
   * this as soon as the PHY says what was negotiated.
   */

  gm_set_speed(priv, 1000, true);

  gm_dma_setup(priv);

  /* Clear anything a previous session left pending, then enable. */

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_STATUS, GM_IRQ_STATUS);
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_INTERRUPT_ENABLE, GM_IRQ_ENABLE);

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL,
            gm_getreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL) |
                RK3576_GMAC_CH0RX_SR);
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL,
            gm_getreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL) |
                RK3576_GMAC_CH0TX_ST);

  gm_putreg(priv, RK3576_GMAC_MAC_CONFIGURATION,
            gm_getreg(priv, RK3576_GMAC_MAC_CONFIGURATION) |
                RK3576_GMAC_CONFIG_RE | RK3576_GMAC_CONFIG_TE);
}

/****************************************************************************
 * Name: gm_mac_stop
 *
 * Description:
 *   Stop the DMA first, then the MAC, then wait for the receiver to leave the
 *   running state so that no descriptor is still being written when the
 *   buffers are freed.
 *
 ****************************************************************************/

static void gm_mac_stop(FAR struct rk3576_gmac_s *priv)
{
  uint32_t timeout;

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_INTERRUPT_ENABLE, 0);
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL,
            gm_getreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL) &
                ~RK3576_GMAC_CH0TX_ST);
  gm_putreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL,
            gm_getreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL) &
                ~RK3576_GMAC_CH0RX_SR);

  gm_putreg(priv, RK3576_GMAC_MAC_CONFIGURATION,
            gm_getreg(priv, RK3576_GMAC_MAC_CONFIGURATION) &
                ~(RK3576_GMAC_CONFIG_RE | RK3576_GMAC_CONFIG_TE));

  /* The RX process stopped bit says the DMA has let go of the ring. */

  for (timeout = GM_RESET_TIMEOUT; timeout > 0; timeout--)
    {
      if ((gm_getreg(priv, RK3576_GMAC_DMA_CH0_STATUS) &
           RK3576_GMAC_CH0STS_RPS) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  gm_putreg(priv, RK3576_GMAC_DMA_CH0_STATUS, GM_IRQ_STATUS);
}

/****************************************************************************
 * Private Functions: link state machine
 ****************************************************************************/

/****************************************************************************
 * Name: gm_link_work
 *
 * Description:
 *   Poll the PHY once a second and tell the stack when the carrier or the
 *   negotiated rate changes.
 *
 *   The carrier notifier takes the per-device network lock, so this cannot
 *   run in an interrupt: it is a work item on the low-priority queue, which
 *   is the queue the lower half's `priority` field names.
 *
 ****************************************************************************/

static void gm_link_work(FAR void *arg)
{
  FAR struct rk3576_gmac_s *priv = arg;
  struct gm_phy_status_s st;
  bool up = false;
  bool full_duplex = false;
  bool report = false;
  uint16_t speed = 0;
  uint32_t config;
  int ret;
  irqstate_t flags;

  if (!priv->started)
    {
      return;
    }

  ret = gm_phy_link(priv, &st, &up, &speed, &full_duplex);

  /* While the link is down, say what the PHY says -- but only when it has
   * changed.  A board with no cable prints one line and then nothing; a cable
   * being plugged in, or autonegotiation getting further, prints a new one.
   * Without this the only visible symptom of "no link" is the silence.
   */

  if (ret == -EAGAIN || (ret == OK && !up))
    {
      bool trace = !priv->trace_valid || st.bmsr != priv->trace_bmsr ||
                   st.lpa != priv->trace_lpa;

      /* One more snapshot a few seconds in, whether or not anything changed:
       * some of the vendor state settles on that timescale, and it is exactly
       * that state a silent link needs explained.
       */

      if (!trace && !priv->trace_settled && priv->trace_polls >= 5)
        {
          trace = true;
          priv->trace_settled = true;
        }

      priv->trace_polls++;

      if (trace)
        {
          gm_phy_trace(priv, &st);
          priv->trace_bmsr = st.bmsr;
          priv->trace_lpa = st.lpa;
          priv->trace_valid = true;
        }

      /* -EAGAIN: still negotiating, or a link with nothing in common. */

      goto rearm;
    }

  priv->trace_valid = false;

  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: PHY read failed: %d\n", priv->intf, ret);
      goto rearm;
    }

  flags = spin_lock_irqsave(&priv->lock);

  if (up)
    {
      if (speed != priv->hw_speed || full_duplex != priv->hw_duplex)
        {
          /* A rate or duplex change needs the MAC reprogrammed, which the
           * controller wants done with the receiver and transmitter off.
           */

          config = gm_getreg(priv, RK3576_GMAC_MAC_CONFIGURATION);
          gm_putreg(priv, RK3576_GMAC_MAC_CONFIGURATION,
                    config & ~(RK3576_GMAC_CONFIG_RE | RK3576_GMAC_CONFIG_TE));
          gm_set_speed(priv, speed, full_duplex);
          gm_putreg(priv, RK3576_GMAC_MAC_CONFIGURATION,
                    gm_getreg(priv, RK3576_GMAC_MAC_CONFIGURATION) |
                        RK3576_GMAC_CONFIG_RE | RK3576_GMAC_CONFIG_TE);
        }

      report = !priv->carrier;
      priv->carrier = true;
    }
  else
    {
      report = priv->carrier;
      priv->carrier = false;
    }

  spin_unlock_irqrestore(&priv->lock, flags);

  if (report)
    {
      if (up)
        {
          syslog(LOG_INFO, "gmac%d: link up %uM %s duplex\n", priv->intf,
                 (unsigned int)speed, full_duplex ? "full" : "half");
          netdev_lower_carrier_on(&priv->dev);
        }
      else
        {
          syslog(LOG_INFO, "gmac%d: link down\n", priv->intf);
          netdev_lower_carrier_off(&priv->dev);
        }
    }

rearm:
  if (priv->started)
    {
      work_queue(LPWORK, &priv->linkwork, gm_link_work, priv,
                 MSEC2TICK(GM_LINK_POLL_MS));
    }
}

/****************************************************************************
 * Name: gm_board_power
 *
 * Description:
 *   The board's half of the bring-up: mux the RGMII pads, and take the PHY out
 *   of reset when `reset` says so.
 *
 *   The reset is *not* pulsed on every ifup.  On this part the crystal
 *   oscillator comes up with the supply, and a PHYRSTB pulse does not restart
 *   it: a port that is reset at ifup is left with its analog side -- and so
 *its reference clock -- stopped, while MDIO, the register file and the bias
 *   resistor all look perfectly healthy.  That is the state this board has
 *been stuck in, and the vendor kernel never gets there because it resets the
 *PHY once, when it probes it.
 *
 ****************************************************************************/

static int gm_board_power(FAR struct rk3576_gmac_s *priv, bool reset)
{
  int ret;

  if (priv->cfg->pinmux != NULL)
    {
      ret = priv->cfg->pinmux();
      if (ret < 0)
        {
          syslog(LOG_ERR, "gmac%d: pinmux failed: %d\n", priv->intf, ret);
          return ret;
        }
    }

  if (reset && priv->cfg->phy_reset != NULL)
    {
      /* PHYRSTB is active low: at least 10 ms asserted, then at least 72 ms
       * before the PHY's MDIO registers may be touched.  The vendor DTB's
       * 20 ms / 100 ms is within that.
       */

      priv->cfg->phy_reset(true);
      up_mdelay(20);
      priv->cfg->phy_reset(false);
      up_mdelay(100);
    }

  return OK;
}

/****************************************************************************
 * Name: gm_platform_init
 *
 * Description:
 *   Everything that has to be true before the controller can be talked to:
 *   clocks, pads, PHY out of reset, RGMII mode, delay lines.  Then the reset,
 *   and the aliveness read that says whether it all worked.
 *
 ****************************************************************************/

static int gm_platform_init(FAR struct rk3576_gmac_s *priv)
{
  uint32_t before;
  uint32_t after;
  int ret;

  ret = gm_clocks(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = gm_board_power(priv, true);
  if (ret < 0)
    {
      return ret;
    }

  /* The GRF and the delay lines are configured before the reset, not after:
   * they are part of the platform, and a controller reset does not touch
   * them.
   */

  ret = gm_rgmii_mode(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = gm_delays(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* The aliveness test, on both sides of the reset.  The pair is worth one
   * extra line while this is new: if the controller answers before the reset
   * and not after, the reset is what killed it, and if it never answers the
   * block was never powered or clocked.
   */

  before = gm_getreg(priv, RK3576_GMAC_MAC_VERSION);

  ret = gm_reset(priv);
  if (ret < 0)
    {
      return ret;
    }

  after = gm_getreg(priv, RK3576_GMAC_MAC_VERSION);
  syslog(LOG_INFO,
         "gmac%d: version 0x%08" PRIx32 " (0x%08" PRIx32 " before reset)\n",
         priv->intf, after, before);

  if (after != GM_MAC_VERSION)
    {
      syslog(LOG_ERR, "gmac%d: controller did not answer (expected 0x%08x)\n",
             priv->intf, GM_MAC_VERSION);
      return -ENODEV;
    }

  return OK;
}

/****************************************************************************
 * Name: gm_ifup
 *
 * Description:
 *   Bring the port up.  This runs on the thread that issues `ifup eth0` and
 *   may sleep, which is what MDIO, the PHY reset and the clock enables want.
 *
 ****************************************************************************/

static int gm_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3576_gmac_s *priv = (FAR struct rk3576_gmac_s *)dev;
  int ret;

  if (priv->started)
    {
      return OK;
    }

  /* The PHY is reset once, at boot.  Resetting it again here would leave the
   * crystal oscillator stopped: see gm_board_power().  A port whose PHY never
   * answered at boot is the one case where another pulse cannot make things
   * worse.
   */

  ret = gm_board_power(priv, priv->phyaddr == GM_PHY_ADDR_NONE);
  if (ret < 0)
    {
      return ret;
    }

  ret = gm_reset(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = gm_rgmii_mode(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Second chance for the PHY: the probe at boot may have run before the part
   * was ready, and this is the attempt that matters.
   */

  if (priv->phyaddr == GM_PHY_ADDR_NONE)
    {
      ret = gm_phy_probe(priv);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = gm_phy_start(priv);
  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: autonegotiation could not be started: %d\n",
             priv->intf, ret);
      return ret;
    }

  ret = gm_rings_alloc(priv);
  if (ret < 0)
    {
      gm_rings_free(priv);
      return ret;
    }

  priv->carrier = false;
  priv->trace_valid = false;
  priv->trace_settled = false;
  priv->trace_polls = 0;

  /* Attach the interrupt before the MAC's own interrupt enable is written,
   * so that nothing can be raised and left pending with no handler behind it.
   */

  irq_attach(priv->irq, gm_interrupt, priv);

  gm_mac_start(priv);
  priv->started = true;

  up_enable_irq(priv->irq);

  work_queue(LPWORK, &priv->linkwork, gm_link_work, priv, 0);
  return OK;
}

/****************************************************************************
 * Name: gm_ifdown
 *
 ****************************************************************************/

static int gm_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3576_gmac_s *priv = (FAR struct rk3576_gmac_s *)dev;
  bool carrier;

  if (!priv->started)
    {
      return OK;
    }

  priv->started = false;

  work_cancel(LPWORK, &priv->linkwork);
  work_cancel(LPWORK, &priv->rxretry);

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);

  gm_mac_stop(priv);
  gm_rings_free(priv);

  carrier = priv->carrier;
  priv->carrier = false;

  if (carrier)
    {
      netdev_lower_carrier_off(dev);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gmac_mdio_read / rk3576_gmac_mdio_write
 *
 * Description:
 *   Clause 22 access to a port's management bus, for the PHY driver that the
 *   board pointed its configuration at.  The signatures are the PHY
 *   interface's, with the port as a plain index for the same reason.
 *
 ****************************************************************************/

int rk3576_gmac_mdio_read(int intf, uint8_t phyaddr, uint8_t regaddr,
                          FAR uint16_t *value)
{
  if (intf != RK3576_GMAC0 && intf != RK3576_GMAC1)
    {
      return -EINVAL;
    }

  if (g_gm_port[intf] == NULL)
    {
      return -ENODEV;
    }

  return gm_mdio_read(g_gm_port[intf], phyaddr, regaddr, value);
}

int rk3576_gmac_mdio_write(int intf, uint8_t phyaddr, uint8_t regaddr,
                           uint16_t value)
{
  if (intf != RK3576_GMAC0 && intf != RK3576_GMAC1)
    {
      return -EINVAL;
    }

  if (g_gm_port[intf] == NULL)
    {
      return -ENODEV;
    }

  return gm_mdio_write(g_gm_port[intf], phyaddr, regaddr, value);
}

/****************************************************************************
 * Name: rk3576_gmac_initialize
 *
 * Description:
 *   Platform bring-up and registration.  Everything that must happen once
 *   happens here; everything that must happen per `ifup` happens in gm_ifup.
 *
 ****************************************************************************/

int rk3576_gmac_initialize(int intf,
                           FAR const struct rk3576_gmac_config_s *cfg)
{
  FAR struct rk3576_gmac_s *priv;
  int ret;

  if (cfg == NULL || cfg->mac == NULL || cfg->phy == NULL)
    {
      return -EINVAL;
    }

  if (intf != RK3576_GMAC0 && intf != RK3576_GMAC1)
    {
      return -EINVAL;
    }

  priv = kmm_zalloc(sizeof(struct rk3576_gmac_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->cfg = cfg;
  priv->intf = intf;
  priv->base = intf == RK3576_GMAC0 ? RK3576_GMAC0_ADDR : RK3576_GMAC1_ADDR;
  priv->irq =
      intf == RK3576_GMAC0 ? RK3576_IRQ_GMAC0_SBD : RK3576_IRQ_GMAC1_SBD;
  priv->phyaddr = GM_PHY_ADDR_NONE;
  spin_lock_init(&priv->lock);

  ret = gm_platform_init(priv);
  if (ret < 0)
    {
      goto errout;
    }

  /* A missing PHY must not stop the board from booting, and it must not stop
   * the port from existing: ifup probes again.
   */

  if (gm_phy_probe(priv) < 0)
    {
      syslog(LOG_WARNING, "gmac%d: no PHY at boot, will retry on ifup\n",
             intf);
    }

  memcpy(&priv->dev.netdev.d_mac.ether, cfg->mac, sizeof(struct ether_addr));

  priv->dev.ops = &g_gm_ops;
  priv->dev.quota[NETPKT_TX] = GM_TX_QUOTA;
  priv->dev.quota[NETPKT_RX] = GM_RX_QUOTA;
  priv->dev.rxtype = NETDEV_RX_WORK;
  priv->dev.priority = LPWORK;

  ret = netdev_lower_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      syslog(LOG_ERR, "gmac%d: netdev_lower_register failed: %d\n", intf, ret);
      goto errout;
    }

  g_gm_port[intf] = priv;

  syslog(LOG_INFO,
         "gmac%d: registered as %s, mac "
         "%02x:%02x:%02x:%02x:%02x:%02x\n",
         intf, priv->dev.netdev.d_ifname, cfg->mac[0], cfg->mac[1],
         cfg->mac[2], cfg->mac[3], cfg->mac[4], cfg->mac[5]);
  return OK;

errout:
  g_gm_port[intf] = NULL;
  kmm_free(priv);
  return ret;
}

#endif /* CONFIG_RK3576_GMAC */
