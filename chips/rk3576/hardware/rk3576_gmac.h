/****************************************************************************
 * chips/rk3576/hardware/rk3576_gmac.h
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
 * RK3576 Gigabit Ethernet MAC (DWC EQOS, "dwmac-4.20a") -- register map.
 *
 * Only the registers this driver touches are listed, and every offset here
 * carries the TRM chapter it came from.  The controller is the Synopsys
 * DWC_ether_qos block, whose register names are the ones the TRM uses:
 *
 *   MAC  registers  0x0000..0x03FF   (configuration, filtering, MDIO)
 *   MTL  registers  0x0D00..0x0D3F   (queue thresholds)
 *   DMA  registers  0x1000..0x117F   (channel 0 only)
 *
 * The MAC's own "GMAC" prefix in the TRM is dropped here to keep the names
 * short; RK3576_GMAC_ prefixes the rest.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_GMAC_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_GMAC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MAC registers (TRM Part 2, ch. 8.4.3) ***********************************/

#define RK3576_GMAC_MAC_CONFIGURATION 0x0000
#define RK3576_GMAC_MAC_PACKET_FILTER 0x0008
#define RK3576_GMAC_MAC_RXQ_CTRL0     0x00a0
#define RK3576_GMAC_MAC_VERSION       0x0110
#define RK3576_GMAC_MAC_HW_FEATURE0   0x011c
#define RK3576_GMAC_MAC_HW_FEATURE1   0x0120
#define RK3576_GMAC_MAC_MDIO_ADDRESS  0x0200
#define RK3576_GMAC_MAC_MDIO_DATA     0x0204
#define RK3576_GMAC_MAC_ADDRESS0_HIGH 0x0300
#define RK3576_GMAC_MAC_ADDRESS0_LOW  0x0304

/* MAC_CONFIGURATION, 0x0000 */

#define RK3576_GMAC_CONFIG_RE     (1u << 0)  /* Receiver enable      */
#define RK3576_GMAC_CONFIG_TE     (1u << 1)  /* Transmitter enable   */
#define RK3576_GMAC_CONFIG_DM     (1u << 13) /* Full duplex          */
#define RK3576_GMAC_CONFIG_FES    (1u << 14) /* Fast (100M) speed    */
#define RK3576_GMAC_CONFIG_PS     (1u << 15) /* Port select: 1 = MII */
#define RK3576_GMAC_CONFIG_JE     (1u << 16) /* Jumbo frame enable   */
#define RK3576_GMAC_CONFIG_JD     (1u << 17) /* Jabber disable       */
#define RK3576_GMAC_CONFIG_WD     (1u << 19) /* Watchdog disable     */
#define RK3576_GMAC_CONFIG_ACS    (1u << 20) /* Auto pad/CRC strip   */
#define RK3576_GMAC_CONFIG_CST    (1u << 21) /* CRC strip            */
#define RK3576_GMAC_CONFIG_S2KP   (1u << 22) /* Support 2K packets   */
#define RK3576_GMAC_CONFIG_GPSLCE (1u << 23) /* Giant packet limit   */
#define RK3576_GMAC_CONFIG_IPC    (1u << 27) /* Checksum offload     */
#define RK3576_GMAC_CONFIG_ARPEN  (1u << 31) /* ARP offload          */

/* MAC_PACKET_FILTER, 0x0008.
 *
 * The extracted TRM text loses the bit rows for this register's low bits: the
 * field names run "DBF, PM, DAIF, HMC, HUC, PR" but only four bit rows
 * (3, 2, 1, 0) survive.  PM is the one that matters -- bit 2, "Pass All
 * Multicast", reset 0 -- and every candidate layout agrees neither PM nor PR
 * can break unicast reception, so a wrong guess costs multicast and nothing
 * else.  If IGMP or IPv6 neighbour discovery ever stops working, this is the
 * first register to look at.
 */

#define RK3576_GMAC_FILTER_PR   (1u << 0)  /* Promiscuous          */
#define RK3576_GMAC_FILTER_DAIF (1u << 1)  /* DA inverse filter    */
#define RK3576_GMAC_FILTER_PM   (1u << 2)  /* Pass all multicast   */
#define RK3576_GMAC_FILTER_DBF  (1u << 3)  /* Disable broadcast    */
#define RK3576_GMAC_FILTER_RA   (1u << 31) /* Receive all         */

/* MAC_RXQ_CTRL0, 0x00A0: RXQ0EN in bits [1:0], 2 = enabled for generic
 * (non-AV, non-DCB) traffic, which is what a single-queue NIC wants.
 */

#define RK3576_GMAC_RXQ_CTRL0_RXQ0EN_S       0
#define RK3576_GMAC_RXQ_CTRL0_RXQ0EN_M       (3u << 0)
#define RK3576_GMAC_RXQ_CTRL0_RXQ0EN_GENERIC (2u << 0)

/* MAC_MDIO_ADDRESS, 0x0200.
 *
 * CR is the CSR-clock divider: MDC = CSR / (2 * (CR + 1)) -- no, the TRM's
 * table is direct: 0000 = /42, 0001 = /62, 0010 = /16, 0011 = /26,
 * 0100 = /102, 0101 = /124, 0110 = /204, 0111 = /324.  /324 keeps MDC inside
 * the PHY's 2.5 MHz ceiling for any CSR clock below ~800 MHz, which is what
 * bring-up wants: reading the PHY must not depend on knowing the CSR rate.
 */

#define RK3576_GMAC_MDIO_ADDRESS_GB        (1u << 0)
#define RK3576_GMAC_MDIO_ADDRESS_C45E      (1u << 1)
#define RK3576_GMAC_MDIO_ADDRESS_GOC_S     2
#define RK3576_GMAC_MDIO_ADDRESS_GOC_M     (3u << 2)
#define RK3576_GMAC_MDIO_ADDRESS_GOC_WRITE (1u << 2)
#define RK3576_GMAC_MDIO_ADDRESS_GOC_READ  (3u << 2)
#define RK3576_GMAC_MDIO_ADDRESS_SKAP      (1u << 4)
#define RK3576_GMAC_MDIO_ADDRESS_CR_S      8
#define RK3576_GMAC_MDIO_ADDRESS_CR_M      (0xfu << 8)
#define RK3576_GMAC_MDIO_ADDRESS_CR_324    (7u << 8)
#define RK3576_GMAC_MDIO_ADDRESS_NTC_S     12
#define RK3576_GMAC_MDIO_ADDRESS_RDA_S     16
#define RK3576_GMAC_MDIO_ADDRESS_PA_S      21
#define RK3576_GMAC_MDIO_ADDRESS_BTB       (1u << 26)
#define RK3576_GMAC_MDIO_ADDRESS_PSE       (1u << 27)

#define RK3576_GMAC_MDIO_DATA_GD_M         0x0000ffff

/* MAC_ADDRESS0_HIGH / _LOW, 0x0300 / 0x0304 */

#define RK3576_GMAC_ADDR_HIGH_AE (1u << 31) /* Address enable       */
#define RK3576_GMAC_ADDR_HIGH_M  0x0000ffff
#define RK3576_GMAC_ADDR_LOW_M   0xffffffff

/* MTL registers (TRM Part 2, ch. 8.5) *************************************/

#define RK3576_GMAC_MTL_TXQ0_OPERATION_MODE 0x0d00
#define RK3576_GMAC_MTL_RXQ0_OPERATION_MODE 0x0d30

/* MTL_TXQ0_OPERATION_MODE, 0x0D00 */

#define RK3576_GMAC_MTL_TXQ0_FTQ          (1u << 0) /* Flush TX queue       */
#define RK3576_GMAC_MTL_TXQ0_TSF          (1u << 1) /* Store and forward    */
#define RK3576_GMAC_MTL_TXQ0_TXQEN_S      2
#define RK3576_GMAC_MTL_TXQ0_TXQEN_M      (3u << 2)
#define RK3576_GMAC_MTL_TXQ0_TXQEN_ENABLE (2u << 2)
#define RK3576_GMAC_MTL_TXQ0_TTC_S        4
#define RK3576_GMAC_MTL_TXQ0_TQS_S        16
#define RK3576_GMAC_MTL_TXQ0_TQS_M        (0x3fu << 16)

/* MTL_RXQ0_OPERATION_MODE, 0x0D30 */

#define RK3576_GMAC_MTL_RXQ0_RSF        (1u << 5) /* Store and forward    */
#define RK3576_GMAC_MTL_RXQ0_DIS_TCP_EF (1u << 6)
#define RK3576_GMAC_MTL_RXQ0_EHFC       (1u << 7) /* HW flow control      */
#define RK3576_GMAC_MTL_RXQ0_RFA_S      8
#define RK3576_GMAC_MTL_RXQ0_RFA_M      (0x3fu << 8)
#define RK3576_GMAC_MTL_RXQ0_RFD_S      14
#define RK3576_GMAC_MTL_RXQ0_RFD_M      (0x3fu << 14)
#define RK3576_GMAC_MTL_RXQ0_RQS_S      20
#define RK3576_GMAC_MTL_RXQ0_RQS_M      (0x7fu << 20)

/* DMA registers (TRM Part 2, ch. 8.6).  Only channel 0 is used: the DTB
 * instantiates queue0 alone, so channels 1..7 are never touched.
 */

#define RK3576_GMAC_DMA_MODE                 0x1000
#define RK3576_GMAC_DMA_SYSBUS_MODE          0x1004
#define RK3576_GMAC_DMA_INTERRUPT_STATUS     0x1008
#define RK3576_GMAC_DMA_CH0_CONTROL          0x1100
#define RK3576_GMAC_DMA_CH0_TX_CONTROL       0x1104
#define RK3576_GMAC_DMA_CH0_RX_CONTROL       0x1108
#define RK3576_GMAC_DMA_CH0_TXDESC_LIST_ADDR 0x1114
#define RK3576_GMAC_DMA_CH0_RXDESC_LIST_ADDR 0x111c
#define RK3576_GMAC_DMA_CH0_TXDESC_TAIL_PTR  0x1120
#define RK3576_GMAC_DMA_CH0_RXDESC_TAIL_PTR  0x1128
#define RK3576_GMAC_DMA_CH0_TXDESC_RING_LEN  0x112c
#define RK3576_GMAC_DMA_CH0_RXDESC_RING_LEN  0x1130
#define RK3576_GMAC_DMA_CH0_INTERRUPT_ENABLE 0x1134
#define RK3576_GMAC_DMA_CH0_STATUS           0x1160

/* DMA_MODE, 0x1000 */

#define RK3576_GMAC_DMA_MODE_SWR (1u << 0) /* Software reset       */

/* DMA_SYSBUS_MODE, 0x1004.  Only the burst lengths the vendor DTB names
 * (blen = 4/8/16) are programmed; the AXI outstanding-request limits it also
 * names are left at their reset values, because the extracted TRM text
 * interleaves that field's bit rows with its neighbours' and a wrong guess
 * there buys nothing but a mystery.
 */

#define RK3576_GMAC_SYSBUS_BLEN4  (1u << 1)
#define RK3576_GMAC_SYSBUS_BLEN8  (1u << 2)
#define RK3576_GMAC_SYSBUS_BLEN16 (1u << 3)
#define RK3576_GMAC_SYSBUS_EAME   (1u << 18) /* AXI master error rsp */

/* DMA_CH0_CONTROL, 0x1100 */

#define RK3576_GMAC_CH0CTRL_PBLX8 (1u << 16) /* 8x programmable burst */

/* DMA_CH0_TX_CONTROL, 0x1104 */

#define RK3576_GMAC_CH0TX_ST      (1u << 0) /* Start transmission   */
#define RK3576_GMAC_CH0TX_OSP     (1u << 4) /* Operate on 2nd pkt   */
#define RK3576_GMAC_CH0TX_TXPBL_S 16
#define RK3576_GMAC_CH0TX_TXPBL_M (0x3fu << 16)

/* DMA_CH0_RX_CONTROL, 0x1108 */

#define RK3576_GMAC_CH0RX_SR      (1u << 0) /* Start receive        */
#define RK3576_GMAC_CH0RX_RBSZ_S  1
#define RK3576_GMAC_CH0RX_RBSZ_M  (0x3fffu << 1)
#define RK3576_GMAC_CH0RX_RXPBL_S 16
#define RK3576_GMAC_CH0RX_RXPBL_M (0x3fu << 16)
#define RK3576_GMAC_CH0RX_RPF     (1u << 31) /* Rx packet flush      */

/* DMA_CH0_INTERRUPT_ENABLE, 0x1134.
 *
 * The interrupt-status bits, and please read this before changing them.
 *
 * The TRM puts the two summary enables at bits 15 (NIE) and 14 (AIE), and
 * says bits 31:16 of this register are read-only.  The DWC databook the
 * mainline stmmac driver was written against puts them one bit higher, at 16
 * and 15.  Both documents describe the same block, and nothing in the tree
 * can settle it without hardware, so the driver writes *both* candidate
 * positions: whichever is real takes effect, and the other is either the
 * neighbouring summary enable or a read-only reserved bit.
 *
 * The individual bits (TIE 0, RIE 6, RBUE 7, FBE 12) agree in both
 * documents and are what the handler actually acts on.
 */

#define RK3576_GMAC_CH0INT_TIE          (1u << 0)
#define RK3576_GMAC_CH0INT_TBUE         (1u << 2)
#define RK3576_GMAC_CH0INT_RIE          (1u << 6)
#define RK3576_GMAC_CH0INT_RBUE         (1u << 7)
#define RK3576_GMAC_CH0INT_RSE          (1u << 8)
#define RK3576_GMAC_CH0INT_RWTE         (1u << 9)
#define RK3576_GMAC_CH0INT_ETIE         (1u << 10)
#define RK3576_GMAC_CH0INT_ERIE         (1u << 11)
#define RK3576_GMAC_CH0INT_FBEE         (1u << 12)
#define RK3576_GMAC_CH0INT_CDEE         (1u << 13)
#define RK3576_GMAC_CH0INT_AIE_TRM      (1u << 14)
#define RK3576_GMAC_CH0INT_NIE_TRM      (1u << 15)
#define RK3576_GMAC_CH0INT_AIE_DATABOOK (1u << 15)
#define RK3576_GMAC_CH0INT_NIE_DATABOOK (1u << 16)

/* DMA_CH0_STATUS, 0x1160.  Write 1 to clear. */

#define RK3576_GMAC_CH0STS_TI           (1u << 0) /* Transmit complete    */
#define RK3576_GMAC_CH0STS_TBU          (1u << 2)
#define RK3576_GMAC_CH0STS_RI           (1u << 6) /* Receive complete     */
#define RK3576_GMAC_CH0STS_RBU          (1u << 7) /* Rx buffer unavailable */
#define RK3576_GMAC_CH0STS_RPS          (1u << 8) /* Rx process stopped   */
#define RK3576_GMAC_CH0STS_RWT          (1u << 9)
#define RK3576_GMAC_CH0STS_ETI          (1u << 10)
#define RK3576_GMAC_CH0STS_ERI          (1u << 11)
#define RK3576_GMAC_CH0STS_FBE          (1u << 12) /* Fatal bus error      */
#define RK3576_GMAC_CH0STS_CDE          (1u << 13)
#define RK3576_GMAC_CH0STS_AIS          (1u << 14)
#define RK3576_GMAC_CH0STS_NIS          (1u << 15)
#define RK3576_GMAC_CH0STS_AIS_DATABOOK (1u << 15)
#define RK3576_GMAC_CH0STS_NIS_DATABOOK (1u << 16)

/* Every bit a completed transfer sets; the handler reaps on these alone. */

#define RK3576_GMAC_CH0STS_ERRORS                                             \
  (RK3576_GMAC_CH0STS_FBE | RK3576_GMAC_CH0STS_CDE | RK3576_GMAC_CH0STS_ERI | \
   RK3576_GMAC_CH0STS_RPS | RK3576_GMAC_CH0STS_RWT | RK3576_GMAC_CH0STS_ERI)

/* DMA_INTERRUPT_STATUS, 0x1008: bit 16 is "channel 0 has a pending
 * interrupt", which the DMA raises on the OR of the channel's status bits.
 */

#define RK3576_GMAC_DMAINT_DC0IS (1u << 16)

/* Descriptors (TRM Part 2, ch. 8.6.3 and 8.6.8) **************************
 *
 * Four 32-bit words.  TX read format:  des0 = buffer 1 address,
 * des2[13:0] = buffer 1 length, des3 = control/status.  RX read format:
 * des0 = buffer 1 address, des3 = control/status.  The same words are
 * written back by the DMA, which clears OWN and puts the received frame
 * length in des3[14:0].
 */

#define RK3576_GMAC_DESC_SIZE 16

/* TDES2 carries the buffer length in [13:0] and, in bit 31, "interrupt on
 * completion".  Without that bit a descriptor completes silently: the frame
 * goes out, no transmit interrupt follows, the ring never recycles and the
 * port stops transmitting after its quota of frames.
 */

#define RK3576_GMAC_TDES2_IOC   (1u << 31)
#define RK3576_GMAC_TDES2_B1L_M 0x00003fff

#define RK3576_GMAC_TDES3_OWN   (1u << 31)
#define RK3576_GMAC_TDES3_CTXT  (1u << 30)
#define RK3576_GMAC_TDES3_FD    (1u << 29)
#define RK3576_GMAC_TDES3_LD    (1u << 28)
#define RK3576_GMAC_TDES3_FL_M  0x00007fff

#define RK3576_GMAC_RDES3_OWN   (1u << 31)
#define RK3576_GMAC_RDES3_IOC   (1u << 30)
#define RK3576_GMAC_RDES3_BUF2V (1u << 25)
#define RK3576_GMAC_RDES3_BUF1V (1u << 24)
#define RK3576_GMAC_RDES3_PL_M  0x00007fff
#define RK3576_GMAC_RDES3_ES    (1u << 15)

/* SDGMAC_GRF (TRM Part 1, ch. 5.15) **************************************
 *
 * The GMACn_CON registers are hiword-write-enable: bits [31:16] gate the
 * write of the corresponding bit in [15:0].
 */

#define RK3576_SDGMAC_GRF_GMAC0_CON   0x0020
#define RK3576_SDGMAC_GRF_GMAC1_CON   0x0024
#define RK3576_SDGMAC_GRF_GMAC_CON(n) (0x0020 + (n)*4)

/* The memory clock gate is active high: 0 leaves the controller clocked. */

#define RK3576_SDGMAC_CON_CLK_GATING_EN  (1u << 0)
#define RK3576_SDGMAC_CON_SBD_FLOWCTRL_S 1
#define RK3576_SDGMAC_CON_SBD_FLOWCTRL_M (3u << 1)
#define RK3576_SDGMAC_CON_RMII_MODE      (1u << 3) /* 1 = RMII, 0 = RGMII    */
#define RK3576_SDGMAC_CON_RMII_CLK_GATE  (1u << 4)
#define RK3576_SDGMAC_CON_TXCLK_DIV_S    5
#define RK3576_SDGMAC_CON_TXCLK_DIV_M    (3u << 5)
#define RK3576_SDGMAC_CON_TXCLK_DIV_2P5M (2u << 5)
#define RK3576_SDGMAC_CON_TXCLK_DIV_25M  (3u << 5)
#define RK3576_SDGMAC_CON_TXCLK_DIV_125M (0u << 5)
#define RK3576_SDGMAC_CON_TXCLK_SEL      (1u << 7) /* 0 = CRU, 1 = IO pad   */

/* RK3576_SDGMAC_HIWORD(v) is the write-enable half of such a write: every bit
 * set in the high half covers the same bit of the low half.
 */

#define RK3576_SDGMAC_HIWORD(v) ((uint32_t)(v) << 16)

/* VCCIO_IOC RGMII delay lines (TRM Part 1, ch. 5.9) ***********************
 *
 * One register per controller *and per pin group*; the KICKPI-K7 uses pin
 * group m0 for both ports, so only MISC_CON2 and MISC_CON4 are ever written.
 * Same layout in each, also hiword-write-enable:
 *
 *   [15]     rxclk_delay_ena
 *   [14:8]   rxclk_delayline_num
 *   [7]      txclk_delay_ena
 *   [6:0]    txclk_delayline_num
 */

#define RK3576_VCCIO_IOC_MISC_CON2 0x0408 /* GMAC0, pin group m0 */
#define RK3576_VCCIO_IOC_MISC_CON3 0x040c /* GMAC0, pin group m1 */
#define RK3576_VCCIO_IOC_MISC_CON4 0x0410 /* GMAC1, pin group m0 */
#define RK3576_VCCIO_IOC_MISC_CON5 0x0414 /* GMAC1, pin group m1 */

#define RK3576_VCCIO_DLY_TX_ENA    (1u << 7)
#define RK3576_VCCIO_DLY_TX_NUM_S  0
#define RK3576_VCCIO_DLY_TX_NUM_M  (0x7fu << 0)
#define RK3576_VCCIO_DLY_RX_ENA    (1u << 15)
#define RK3576_VCCIO_DLY_RX_NUM_S  8
#define RK3576_VCCIO_DLY_RX_NUM_M  (0x7fu << 8)

#define RK3576_VCCIO_DLY_HIWORD(v) ((uint32_t)(v) << 16)

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_GMAC_H */
