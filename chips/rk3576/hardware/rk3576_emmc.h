/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_emmc.h
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
 * The RK3576 eMMC host (mmc@2a330000) is a Synopsys DesignWare Cortex MSHC
 * (dwcmshc), which presents a standard SDHCI 3.0 register block plus a
 * Rockchip vendor area at offset 0x500 (eMMC control, DLL for HS400).  The
 * offsets/bits below are the SDHCI 3.0 host specification; only the vendor
 * area (RK3576_EMMC_VENDOR_*) is Rockchip-specific.  Bring-up uses the
 * standard block only; the DLL/HS400 vendor path is deferred.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_EMMC_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_EMMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SDHCI 3.0 Register Offsets (from RK3576_EMMC_ADDR) *********************/

#define RK3576_EMMC_SDMA       0x000 /* SDMA system address / arg2 (32) */
#define RK3576_EMMC_BLOCKSIZE  0x004 /* Block size (16)                 */
#define RK3576_EMMC_BLOCKCOUNT 0x006 /* Block count (16)                */
#define RK3576_EMMC_ARG1       0x008 /* Argument 1 (32)                 */
#define RK3576_EMMC_XFERMODE   0x00c /* Transfer mode (16)              */
#define RK3576_EMMC_CMD        0x00e /* Command (16)                    */
#define RK3576_EMMC_RESP0      0x010 /* Response 0 (32)                 */
#define RK3576_EMMC_RESP1      0x014 /* Response 1 (32)                 */
#define RK3576_EMMC_RESP2      0x018 /* Response 2 (32)                 */
#define RK3576_EMMC_RESP3      0x01c /* Response 3 (32)                 */
#define RK3576_EMMC_BUFFER     0x020 /* Buffer data port (32)           */
#define RK3576_EMMC_PRESENT    0x024 /* Present state (32)              */
#define RK3576_EMMC_HOSTCTRL1  0x028 /* Host control 1 (8)              */
#define RK3576_EMMC_PWRCTRL    0x029 /* Power control (8)               */
#define RK3576_EMMC_BLKGAPCTRL 0x02a /* Block gap control (8)           */
#define RK3576_EMMC_WAKECTRL   0x02b /* Wakeup control (8)              */
#define RK3576_EMMC_CLKCTRL    0x02c /* Clock control (16)              */
#define RK3576_EMMC_TOUTCTRL   0x02e /* Timeout control (8)             */
#define RK3576_EMMC_SWRESET    0x02f /* Software reset (8)              */
#define RK3576_EMMC_NINTSTS    0x030 /* Normal interrupt status (16)    */
#define RK3576_EMMC_EINTSTS    0x032 /* Error interrupt status (16)     */
#define RK3576_EMMC_NINTEN     0x034 /* Normal int status enable (16)   */
#define RK3576_EMMC_EINTEN     0x036 /* Error int status enable (16)    */
#define RK3576_EMMC_NINTSIGEN  0x038 /* Normal int signal enable (16)   */
#define RK3576_EMMC_EINTSIGEN  0x03a /* Error int signal enable (16)    */
#define RK3576_EMMC_AC12ERR    0x03c /* Auto CMD12 error status (16)    */
#define RK3576_EMMC_HOSTCTRL2  0x03e /* Host control 2 (16)             */
#define RK3576_EMMC_CAP0       0x040 /* Capabilities lower (32)         */
#define RK3576_EMMC_CAP1       0x044 /* Capabilities upper (32)         */
#define RK3576_EMMC_ADMAERR    0x054 /* ADMA error status (8)           */
#define RK3576_EMMC_ADMAADDR   0x058 /* ADMA system address (64)        */
#define RK3576_EMMC_HOSTVER    0x0fe /* Host controller version (16)    */

/* Rockchip dwcmshc vendor area (deferred; HS400/DLL only) ****************/

#define RK3576_EMMC_VENDOR_EMMCCTRL 0x52c /* eMMC control (card_is_emmc) */
#define RK3576_EMMC_VENDOR_ATCTRL   0x540 /* AT (tuning) control             */
#define RK3576_EMMC_VENDOR_DLLCTRL  0x800 /* DLL control (HS400) */

/* SWRESET (0x2f) -- software reset bits *********************************/

#define EMMC_SWRESET_ALL (1 << 0) /* Reset the whole host         */
#define EMMC_SWRESET_CMD (1 << 1) /* Reset the command line       */
#define EMMC_SWRESET_DAT (1 << 2) /* Reset the data line          */

/* PWRCTRL (0x29) -- power control **************************************/

#define EMMC_PWRCTRL_ON  (1 << 0)   /* SD bus power on              */
#define EMMC_PWRCTRL_1V8 (0x5 << 1) /* Bus voltage 1.8 V           */
#define EMMC_PWRCTRL_3V3 (0x7 << 1) /* Bus voltage 3.3 V           */

/* CLKCTRL (0x2c) -- internal clock + SD clock **************************/

#define EMMC_CLKCTRL_INTLEN            (1 << 0) /* Internal clock enable        */
#define EMMC_CLKCTRL_INTSTABLE         (1 << 1) /* Internal clock stable        */
#define EMMC_CLKCTRL_SDCLKEN           (1 << 2) /* SD clock enable              */
#define EMMC_CLKCTRL_CLKGENSEL         (1 << 5) /* Clock generator select       */
#define EMMC_CLKCTRL_SDCLKFREQ_SHIFT   8 /* SDCLK freq select [15:8]     */
#define EMMC_CLKCTRL_SDCLKFREQUP_SHIFT 6 /* Upper freq select [7:6]      */

/* HOSTCTRL1 (0x28) -- host control 1 **********************************/

#define EMMC_HOSTCTRL1_DWIDTH4      (1 << 1) /* 4-bit data width             */
#define EMMC_HOSTCTRL1_HISPD        (1 << 2) /* High-speed enable            */
#define EMMC_HOSTCTRL1_DMASEL_SHIFT 3        /* DMA select [4:3]             */
#define EMMC_HOSTCTRL1_DMA_SDMA     (0 << 3) /* SDMA                         */
#define EMMC_HOSTCTRL1_DMA_ADMA2    (2 << 3) /* 32-bit ADMA2                 */
#define EMMC_HOSTCTRL1_DWIDTH8      (1 << 5) /* 8-bit data width (eMMC)      */

/* PRESENT (0x24) -- present state *************************************/

#define EMMC_PRESENT_CMDINHIBIT (1 << 0)  /* Command inhibit (CMD)        */
#define EMMC_PRESENT_DATINHIBIT (1 << 1)  /* Command inhibit (DAT)        */
#define EMMC_PRESENT_DATBUSY    (1 << 2)  /* DAT line active              */
#define EMMC_PRESENT_WRXFERACT  (1 << 8)  /* Write transfer active        */
#define EMMC_PRESENT_RDXFERACT  (1 << 9)  /* Read transfer active         */
#define EMMC_PRESENT_BUFWREN    (1 << 10) /* Buffer write enable          */
#define EMMC_PRESENT_BUFRDEN    (1 << 11) /* Buffer read enable           */

/* XFERMODE (0x0c) -- transfer mode ***********************************/

#define EMMC_XFERMODE_DMAEN  (1 << 0) /* DMA enable                   */
#define EMMC_XFERMODE_BCEN   (1 << 1) /* Block count enable           */
#define EMMC_XFERMODE_AC12EN (1 << 2) /* Auto CMD12 enable            */
#define EMMC_XFERMODE_DTDSEL (1 << 4) /* 1 = read (card to host)      */
#define EMMC_XFERMODE_MSBSEL (1 << 5) /* Multi-block select           */

/* CMD (0x0e) -- command register *************************************/

#define EMMC_CMD_RESP_NONE       (0 << 0) /* No response                  */
#define EMMC_CMD_RESP_LONG       (1 << 0) /* 136-bit response             */
#define EMMC_CMD_RESP_SHORT      (2 << 0) /* 48-bit response              */
#define EMMC_CMD_RESP_SHORT_BUSY (3 << 0) /* 48-bit response with busy    */
#define EMMC_CMD_CRCEN           (1 << 3) /* Command CRC check enable     */
#define EMMC_CMD_IDXEN           (1 << 4) /* Command index check enable   */
#define EMMC_CMD_DATA            (1 << 5) /* Data present                 */
#define EMMC_CMD_INDEX_SHIFT     8        /* Command index [13:8]         */

/* NINTSTS (0x30) / NINTEN (0x34) -- normal interrupt bits ************/

#define EMMC_NINT_CMDDONE  (1 << 0)  /* Command complete             */
#define EMMC_NINT_XFERDONE (1 << 1)  /* Transfer complete            */
#define EMMC_NINT_BGEVENT  (1 << 2)  /* Block gap event              */
#define EMMC_NINT_DMAINT   (1 << 3)  /* DMA interrupt                */
#define EMMC_NINT_BUFWRRDY (1 << 4)  /* Buffer write ready           */
#define EMMC_NINT_BUFRDRDY (1 << 5)  /* Buffer read ready            */
#define EMMC_NINT_ERR      (1 << 15) /* Error interrupt              */

/* EINTSTS (0x32) / EINTEN (0x36) -- error interrupt bits ************/

#define EMMC_EINT_CMDTIMEOUT (1 << 0) /* Command timeout error        */
#define EMMC_EINT_CMDCRC     (1 << 1) /* Command CRC error            */
#define EMMC_EINT_CMDEND     (1 << 2) /* Command end bit error        */
#define EMMC_EINT_CMDIDX     (1 << 3) /* Command index error          */
#define EMMC_EINT_DATTIMEOUT (1 << 4) /* Data timeout error           */
#define EMMC_EINT_DATCRC     (1 << 5) /* Data CRC error               */
#define EMMC_EINT_DATEND     (1 << 6) /* Data end bit error           */
#define EMMC_EINT_ADMA       (1 << 9) /* ADMA error                   */
#define EMMC_EINT_ALL        0xffff   /* All error bits               */

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_EMMC_H */
