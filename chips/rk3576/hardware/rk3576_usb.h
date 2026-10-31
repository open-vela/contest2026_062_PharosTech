/****************************************************************************
 * chips/rk3576/hardware/rk3576_usb.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_USB_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_USB_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Synopsys DesignWare USB3 (DWC3) register offsets and bit fields shared by
 * the RK3576 USB0 device controller and USB1 host controller.  Offsets are
 * relative to the selected controller base.
 */

/* Global register block (xHCI-relative offsets, device-mode subset) ******/

#define DWC3_GSBUSCFG0     0xc100
#define DWC3_GCTL          0xc110
#define DWC3_GSTS          0xc118
#define DWC3_GUCTL1        0xc11c
#define DWC3_GSNPSID       0xc120
#define DWC3_GUCTL         0xc12c
#define DWC3_GFLADJ        0xc630
#define DWC3_GUSB2PHYCFG   0xc200
#define DWC3_GUSB3PIPECTL  0xc2c0
#define DWC3_GTXFIFOSIZ(n) (0xc300 + ((n) << 2))
#define DWC3_GRXFIFOSIZ(n) (0xc380 + ((n) << 2))
#define DWC3_GEVNTADRLO    0xc400
#define DWC3_GEVNTADRHI    0xc404
#define DWC3_GEVNTSIZ      0xc408
#define DWC3_GEVNTCOUNT    0xc40c

#define DWC3_DCFG          0xc700
#define DWC3_DCTL          0xc704
#define DWC3_DEVTEN        0xc708
#define DWC3_DSTS          0xc70c
#define DWC3_DALEPENA      0xc720
#define DWC3_DEPCMDPAR2(n) (0xc800 + ((n)*0x10))
#define DWC3_DEPCMDPAR1(n) (0xc804 + ((n)*0x10))
#define DWC3_DEPCMDPAR0(n) (0xc808 + ((n)*0x10))
#define DWC3_DEPCMD(n)     (0xc80c + ((n)*0x10))

/* Register fields ********************************************************/

#define GCTL_CORESOFTRESET            (1u << 11)
#define GCTL_PRTCAPDIR_SHIFT          12
#define GCTL_PRTCAPDIR_MASK           (3u << 12)
#define GCTL_PRTCAP_HOST              (1u << 12)
#define GCTL_PRTCAP_DEVICE            (2u << 12)
#define GCTL_SOFITPSYNC               (1u << 10)
#define GCTL_DSBLCLKGTNG              (1u << 0)

#define GUCTL1_TX_IPGAP_LINECHECK_DIS (1u << 28)
#define GUCTL1_PARKMODE_DISABLE_SS    (1u << 17)
#define GUCTL1_PARKMODE_DISABLE_HS    (1u << 16)

#define GUCTL_REFCLKPER_SHIFT         22
#define GUCTL_REFCLKPER_MASK          (0x3ffu << GUCTL_REFCLKPER_SHIFT)

#define GFLADJ_REFCLK_FLADJ_SHIFT     8
#define GFLADJ_REFCLK_FLADJ_MASK      (0x3fffu << GFLADJ_REFCLK_FLADJ_SHIFT)
#define GFLADJ_240MHZDECR_SHIFT       24
#define GFLADJ_240MHZDECR_MASK        (0x7fu << GFLADJ_240MHZDECR_SHIFT)
#define GFLADJ_240MHZDECR_PLS1        (1u << 31)

#define GUSB2PHYCFG_PHYSOFTRST        (1u << 31)
#define GUSB2PHYCFG_SUSPHY            (1u << 6)
#define GUSB2PHYCFG_PHYIF             (1u << 3)
#define GUSB2PHYCFG_ENBLSLPM          (1u << 8)
#define GUSB2PHYCFG_TRDTIM_MASK       (0xfu << 10)
#define GUSB2PHYCFG_TRDTIM(n)         ((uint32_t)(n) << 10)
#define GUSB2PHYCFG_U2FREECLK         (1u << 30)

#define GUSB3PIPECTL_PHYSOFTRST       (1u << 31)
#define GUSB3PIPECTL_DISRXDETINP3     (1u << 28)
#define GUSB3PIPECTL_DEPOCHANGE       (1u << 18)
#define GUSB3PIPECTL_SUSPEND          (1u << 17)

#define DCFG_DEVSPD_MASK              (7u << 0)
#define DCFG_DEVSPD_HS                (0u << 0)
#define DCFG_DEVADDR_SHIFT            3
#define DCFG_DEVADDR_MASK             (0x7fu << 3)

#define DCTL_RUNSTOP                  (1u << 31)
#define DCTL_CSFTRST                  (1u << 30)

#define DEVTEN_DISCONNEVTEN           (1u << 0)
#define DEVTEN_USBRSTEN               (1u << 1)
#define DEVTEN_CONNECTDONEEN          (1u << 2)
#define DEVTEN_ULSTCNGEN              (1u << 3)
#define DEVTEN_WKUPEVTEN              (1u << 4)
#define DEVTEN_U3L2L1SUSPEN           (1u << 6)

#define DSTS_CONNECTSPD_MASK          (7u << 0)
#define DSTS_SPEED_HS                 0
#define DSTS_SPEED_FS                 1

/* Endpoint commands (DEPCMD.CmdTyp) **************************************/

#define DEPCMD_SETEPCONFIG       1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL          4
#define DEPCMD_CLEARSTALL        5
#define DEPCMD_STARTTRANSFER     6
#define DEPCMD_UPDATETRANSFER    7
#define DEPCMD_ENDTRANSFER       8
#define DEPCMD_DEPSTARTCFG       9

#define DEPCMD_CMDIOC            (1u << 8)
#define DEPCMD_CMDACT            (1u << 10)
#define DEPCMD_HIPRI_FORCERM     (1u << 11)
#define DEPCMD_STATUS_SHIFT      12
#define DEPCMD_STATUS_MASK       (0xfu << 12)
#define DEPCMD_PARAM_SHIFT       16
#define DEPCMD_PARAM_MASK        (0x7fffu << 16)

/* DEPCFG parameter fields ************************************************/

#define DEPCFG_P0_ACTION_INIT   (0u << 30)
#define DEPCFG_P0_ACTION_MODIFY (2u << 30)
#define DEPCFG_P0_TYPE(t)       ((uint32_t)(t) << 1) /* usb ep type */
#define DEPCFG_P0_MPS(m)        ((uint32_t)(m) << 3)
#define DEPCFG_P0_FIFONUM(f)    ((uint32_t)(f) << 17)

#define DEPCFG_P1_XFERCMPLEN    (1u << 8)
#define DEPCFG_P1_XFERINPROGEN  (1u << 9)
#define DEPCFG_P1_XFERNRDYEN    (1u << 10)
#define DEPCFG_P1_EPNUM(phy)    ((uint32_t)(phy) << 25)

/* TRB descriptor *********************************************************/

#define TRB_CTRL_HWO         (1u << 0)
#define TRB_CTRL_LST         (1u << 1)
#define TRB_CTRL_CSP         (1u << 3)
#define TRB_CTRL_TYPE(t)     ((uint32_t)(t) << 4)
#define TRB_CTRL_ISP_IMI     (1u << 10)
#define TRB_CTRL_IOC         (1u << 11)

#define TRB_TYPE_NORMAL      1
#define TRB_TYPE_CTL_SETUP   2
#define TRB_TYPE_CTL_STATUS2 3 /* status, no data stage */
#define TRB_TYPE_CTL_STATUS3 4 /* status, after data stage */
#define TRB_TYPE_CTL_DATA    5

#define TRB_SIZE_MASK        0x00ffffffu

/* Events *****************************************************************/

#define EVT_IS_DEVT(e)        (((e)&1u) != 0)

#define DEVT_TYPE(e)          (((e) >> 8) & 0xfu)
#define DEVT_DISCONN          0
#define DEVT_USBRST           1
#define DEVT_CONNECTDONE      2
#define DEVT_ULSTCHNG         3
#define DEVT_WKUPEVT          4
#define DEVT_SUSPEND          6

#define DEPEVT_PHYEP(e)       (((e) >> 1) & 0x1fu)
#define DEPEVT_TYPE(e)        (((e) >> 6) & 0xfu)
#define DEPEVT_STATUS(e)      (((e) >> 12) & 0xfu)
#define DEPEVT_XFERCOMPLETE   1
#define DEPEVT_XFERINPROGRESS 2
#define DEPEVT_XFERNOTREADY   3
#define DEPEVT_EPCMDCMPLT     7

/* XferNotReady status[1:0] for control endpoints */

#define DEPEVT_NRDY_CTL_DATA   1
#define DEPEVT_NRDY_CTL_STATUS 2

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_USB_H */
