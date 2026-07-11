/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_emmcprobe.c
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
 * Bring-up self-test (milestone M1) for the on-board eMMC (mmc@2a330000, a
 * Synopsys dwcmshc / SDHCI 3.0 host).  Verifies, on real hardware, that the
 * SDHCI register map is correct and the controller is clocked before the
 * full sdio_dev_s driver is written: reads the host version + capabilities,
 * software-resets the host, brings up the internal + card clock and bus
 * power, then enumerates the eMMC device with CMD0 (GO_IDLE) and CMD1
 * (SEND_OP_COND) and prints the returned OCR.  No data transfer, no DMA,
 * no HS400/DLL -- those come once M1 is proven.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "rk3576_gpio.h"
#include "hardware/rk3576_emmc.h"
#include "kickpi_k7.h"

#ifdef CONFIG_KICKPI_K7_EMMC_PROBE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EMMC_BASE   RK3576_EMMC_ADDR

/* eMMC CMD1 argument: sector addressing + all voltage window (0x40FF8000).
 * Bit30 = sector mode (>2 GB), bits[23:8] = 2.7-3.6 V window.
 */

#define EMMC_OCR_ARG  0x40ff8000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint8_t  rd8(uintptr_t o)  { return *(volatile uint8_t  *)(EMMC_BASE + o); }
static inline uint16_t rd16(uintptr_t o) { return *(volatile uint16_t *)(EMMC_BASE + o); }
static inline uint32_t rd32(uintptr_t o) { return *(volatile uint32_t *)(EMMC_BASE + o); }
static inline void wr8(uintptr_t o, uint8_t v)   { *(volatile uint8_t  *)(EMMC_BASE + o) = v; }
static inline void wr16(uintptr_t o, uint16_t v) { *(volatile uint16_t *)(EMMC_BASE + o) = v; }
static inline void wr32(uintptr_t o, uint32_t v) { *(volatile uint32_t *)(EMMC_BASE + o) = v; }

/****************************************************************************
 * Name: emmc_send_cmd
 *
 * Description:
 *   Issue a single command with no data.  Waits for command-inhibit to
 *   clear, clears the interrupt status, writes ARG1 then XFERMODE|CMD, and
 *   polls the normal/error interrupt status for completion.  Returns 0 on
 *   command-complete, -1 on error/timeout.
 ****************************************************************************/

static int emmc_send_cmd(uint8_t index, uint32_t arg, uint16_t cmdflags)
{
  uint16_t cmd = ((uint16_t)index << EMMC_CMD_INDEX_SHIFT) | cmdflags;
  int i;

  /* Wait for the CMD line to be free. */

  for (i = 0; i < 100000; i++)
    {
      if ((rd32(RK3576_EMMC_PRESENT) & EMMC_PRESENT_CMDINHIBIT) == 0)
        {
          break;
        }
    }

  /* Clear any pending interrupt status. */

  wr16(RK3576_EMMC_NINTSTS, 0xffff);
  wr16(RK3576_EMMC_EINTSTS, 0xffff);

  /* Argument, transfer-mode (0, no data), then the command.  The 16-bit write
   * to the CMD register (0x0e) is what starts the command on the dwcmshc host.
   */

  wr32(RK3576_EMMC_ARG1, arg);
  wr16(RK3576_EMMC_XFERMODE, 0);
  wr16(RK3576_EMMC_CMD, cmd);

  /* Poll for command complete or error. */

  for (i = 0; i < 500000; i++)
    {
      uint16_t nint = rd16(RK3576_EMMC_NINTSTS);

      if (nint & EMMC_NINT_ERR)
        {
          return -1;
        }

      if (nint & EMMC_NINT_CMDDONE)
        {
          return 0;
        }
    }

  return -1;
}

/****************************************************************************
 * Name: emmc_read_block
 *
 * Description:
 *   Read one 512-byte block at the given (sector) address into buf[128] via
 *   PIO (the SDHCI buffer data port).  No DMA.  Returns 0 on success, a
 *   negative value on command/data error or timeout.
 ****************************************************************************/

static int emmc_read_block(uint32_t blkaddr, uint32_t *buf)
{
  int i;
  int j;

  /* Wait for the DAT line to be free. */

  for (i = 0; i < 100000; i++)
    {
      if ((rd32(RK3576_EMMC_PRESENT) & EMMC_PRESENT_DATINHIBIT) == 0)
        {
          break;
        }
    }

  wr16(RK3576_EMMC_BLOCKSIZE, 512);
  wr16(RK3576_EMMC_BLOCKCOUNT, 1);
  wr16(RK3576_EMMC_NINTSTS, 0xffff);
  wr16(RK3576_EMMC_EINTSTS, 0xffff);

  wr32(RK3576_EMMC_ARG1, blkaddr);
  wr16(RK3576_EMMC_XFERMODE, EMMC_XFERMODE_DTDSEL);   /* single-block read */
  wr16(RK3576_EMMC_CMD,
       (17 << EMMC_CMD_INDEX_SHIFT) | EMMC_CMD_RESP_SHORT | EMMC_CMD_CRCEN |
       EMMC_CMD_IDXEN | EMMC_CMD_DATA);

  /* Wait for command complete. */

  for (i = 0; i < 500000; i++)
    {
      uint16_t nint = rd16(RK3576_EMMC_NINTSTS);
      if (nint & EMMC_NINT_ERR)
        {
          return -1;
        }

      if (nint & EMMC_NINT_CMDDONE)
        {
          break;
        }
    }

  /* Wait for the read buffer to fill, then drain 128 words. */

  for (i = 0; i < 1000000; i++)
    {
      uint16_t nint = rd16(RK3576_EMMC_NINTSTS);
      if (nint & EMMC_NINT_ERR)
        {
          return -2;
        }

      if (nint & EMMC_NINT_BUFRDRDY)
        {
          break;
        }
    }

  for (j = 0; j < 128; j++)
    {
      buf[j] = rd32(RK3576_EMMC_BUFFER);
    }

  /* Wait for transfer complete. */

  for (i = 0; i < 1000000; i++)
    {
      uint16_t nint = rd16(RK3576_EMMC_NINTSTS);
      if (nint & EMMC_NINT_ERR)
        {
          return -3;
        }

      if (nint & EMMC_NINT_XFERDONE)
        {
          return 0;
        }
    }

  return -4;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void kickpi_k7_emmc_probe(void)
{
  static uint32_t blk[128];
  uint16_t ver;
  uint32_t cap0;
  uint32_t basemhz;
  int ready = 0;
  int i;

  syslog(LOG_ERR, "EMMCPROBE: ===== start (dwcmshc / SDHCI M1) =====\n");

  /* Read version + capabilities first: if the controller is not clocked by
   * the loader these read back as 0/0xffffffff and we know the CRU gate
   * still needs a driver.
   */

  ver  = rd16(RK3576_EMMC_HOSTVER);
  cap0 = rd32(RK3576_EMMC_CAP0);
  basemhz = (cap0 >> 8) & 0xff;   /* CAP0[15:8] = base clock in MHz */

  syslog(LOG_ERR, "EMMCPROBE: HOSTVER=%04x CAP0=%08" PRIx32 " baseclk=%uMHz\n",
         ver, cap0, (unsigned)basemhz);

  if (ver == 0 || ver == 0xffff)
    {
      syslog(LOG_ERR, "EMMCPROBE: controller not responding -- CRU eMMC clock"
             " likely gated (needs rk3576_cru_emmc_enable)\n");
      return;
    }

  /* Software-reset the whole host and wait for self-clear. */

  wr8(RK3576_EMMC_SWRESET, EMMC_SWRESET_ALL);
  for (i = 0; i < 100000; i++)
    {
      if ((rd8(RK3576_EMMC_SWRESET) & EMMC_SWRESET_ALL) == 0)
        {
          break;
        }
    }

  /* Internal clock on, wait until stable. */

  wr16(RK3576_EMMC_CLKCTRL, EMMC_CLKCTRL_INTLEN);
  for (i = 0; i < 100000; i++)
    {
      if (rd16(RK3576_EMMC_CLKCTRL) & EMMC_CLKCTRL_INTSTABLE)
        {
          break;
        }
    }

  /* Divided clock for identification (~400 kHz).  SDCLK = base / (2 * N).
   * Use N = 0xff (max 8-bit divider) -> base 200 MHz / 510 ~= 392 kHz.
   */

  wr16(RK3576_EMMC_CLKCTRL,
       EMMC_CLKCTRL_INTLEN | (0xff << EMMC_CLKCTRL_SDCLKFREQ_SHIFT));
  for (i = 0; i < 100000; i++)
    {
      if (rd16(RK3576_EMMC_CLKCTRL) & EMMC_CLKCTRL_INTSTABLE)
        {
          break;
        }
    }

  wr16(RK3576_EMMC_CLKCTRL,
       EMMC_CLKCTRL_INTLEN | (0xff << EMMC_CLKCTRL_SDCLKFREQ_SHIFT) |
       EMMC_CLKCTRL_SDCLKEN);

  /* Bus power on at 1.8 V (eMMC is a 1.8 V VCCQ part per the DTS
   * mmc-hs400-1_8v; the identification voltage window is still queried).
   */

  wr8(RK3576_EMMC_PWRCTRL, EMMC_PWRCTRL_1V8 | EMMC_PWRCTRL_ON);
  up_mdelay(10);

  /* Timeout control to maximum, 1-bit bus for identification. */

  wr8(RK3576_EMMC_TOUTCTRL, 0x0e);
  wr8(RK3576_EMMC_HOSTCTRL1, 0);

  /* Enable all normal + error interrupt STATUS bits.  Without this the
   * NINTSTS/EINTSTS bits never latch (status-enable gates the status
   * register), so a completed command is invisible.  Signal-enable is left
   * clear -- we poll the status, we do not take the IRQ.
   */

  wr16(RK3576_EMMC_NINTEN, 0xffff);
  wr16(RK3576_EMMC_EINTEN, 0xffff);

  /* Diagnostics: confirm the clock is enabled/stable, bus is powered and the
   * command line is free before issuing commands.
   */

  syslog(LOG_ERR, "EMMCPROBE: CLKCTRL=%04x PWRCTRL=%02x PRESENT=%08" PRIx32
         " HOSTCTRL2=%04x\n", rd16(RK3576_EMMC_CLKCTRL),
         rd8(RK3576_EMMC_PWRCTRL), rd32(RK3576_EMMC_PRESENT),
         rd16(RK3576_EMMC_HOSTCTRL2));

  /* CMD0 GO_IDLE_STATE (no response). */

  if (emmc_send_cmd(0, 0, EMMC_CMD_RESP_NONE) < 0)
    {
      syslog(LOG_ERR, "EMMCPROBE: CMD0 failed NINT=%04x EINT=%04x PRESENT=%08"
             PRIx32 "\n", rd16(RK3576_EMMC_NINTSTS),
             rd16(RK3576_EMMC_EINTSTS), rd32(RK3576_EMMC_PRESENT));
    }
  else
    {
      syslog(LOG_ERR, "EMMCPROBE: CMD0 ok\n");
    }

  up_mdelay(2);

  /* CMD1 SEND_OP_COND: poll until the card leaves busy (OCR bit31 = 1). */

  for (i = 0; i < 10; i++)
    {
      if (emmc_send_cmd(1, EMMC_OCR_ARG,
                        EMMC_CMD_RESP_SHORT) < 0)
        {
          syslog(LOG_ERR, "EMMCPROBE: CMD1 no response (try %d) EINT=%04x\n",
                 i, rd16(RK3576_EMMC_EINTSTS));
          up_mdelay(10);
          continue;
        }

      {
        uint32_t ocr = rd32(RK3576_EMMC_RESP0);

        if (ocr & (1u << 31))
          {
            syslog(LOG_ERR, "EMMCPROBE: CMD1 OCR=%08" PRIx32
                   " (ready, %s addressing)  <<< eMMC ALIVE\n", ocr,
                   (ocr & (1u << 30)) ? "sector" : "byte");
            ready = 1;
            break;
          }

        if ((i % 10) == 0)
          {
            syslog(LOG_ERR, "EMMCPROBE: CMD1 OCR=%08" PRIx32 " (busy)\n", ocr);
          }
      }

      up_mdelay(10);
    }

  if (!ready)
    {
      syslog(LOG_ERR, "EMMCPROBE: eMMC never left busy -- stop at M1\n");
      syslog(LOG_ERR, "EMMCPROBE: ===== done =====\n");
      return;
    }

  /* ----- M2: identification + select + single-block read ----- */

  /* CMD2 ALL_SEND_CID (R2, 136-bit).  Index check must be off for R2. */

  if (emmc_send_cmd(2, 0, EMMC_CMD_RESP_LONG | EMMC_CMD_CRCEN) < 0)
    {
      syslog(LOG_ERR, "EMMCPROBE: CMD2 (CID) failed EINT=%04x\n",
             rd16(RK3576_EMMC_EINTSTS));
    }
  else
    {
      syslog(LOG_ERR, "EMMCPROBE: CID=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " %08" PRIx32 "\n", rd32(RK3576_EMMC_RESP3),
             rd32(RK3576_EMMC_RESP2), rd32(RK3576_EMMC_RESP1),
             rd32(RK3576_EMMC_RESP0));
    }

  /* CMD3 SET_RELATIVE_ADDR: for eMMC the host assigns the RCA (use 1). */

  if (emmc_send_cmd(3, 1u << 16,
                    EMMC_CMD_RESP_SHORT | EMMC_CMD_CRCEN | EMMC_CMD_IDXEN) < 0)
    {
      syslog(LOG_ERR, "EMMCPROBE: CMD3 (set RCA) failed EINT=%04x\n",
             rd16(RK3576_EMMC_EINTSTS));
    }

  /* CMD9 SEND_CSD (R2, arg = RCA << 16). */

  if (emmc_send_cmd(9, 1u << 16, EMMC_CMD_RESP_LONG | EMMC_CMD_CRCEN) < 0)
    {
      syslog(LOG_ERR, "EMMCPROBE: CMD9 (CSD) failed EINT=%04x\n",
             rd16(RK3576_EMMC_EINTSTS));
    }
  else
    {
      syslog(LOG_ERR, "EMMCPROBE: CSD=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " %08" PRIx32 "\n", rd32(RK3576_EMMC_RESP3),
             rd32(RK3576_EMMC_RESP2), rd32(RK3576_EMMC_RESP1),
             rd32(RK3576_EMMC_RESP0));
    }

  /* CMD7 SELECT_CARD (R1b, arg = RCA << 16) -> transfer state. */

  if (emmc_send_cmd(7, 1u << 16,
                    EMMC_CMD_RESP_SHORT_BUSY | EMMC_CMD_CRCEN |
                    EMMC_CMD_IDXEN) < 0)
    {
      syslog(LOG_ERR, "EMMCPROBE: CMD7 (select) failed EINT=%04x\n",
             rd16(RK3576_EMMC_EINTSTS));
    }

  up_mdelay(2);

  /* CMD17 READ_SINGLE_BLOCK at sector 0 via PIO. */

  {
    int ret = emmc_read_block(0, blk);

    if (ret < 0)
      {
        syslog(LOG_ERR, "EMMCPROBE: read block 0 failed ret=%d NINT=%04x "
               "EINT=%04x\n", ret, rd16(RK3576_EMMC_NINTSTS),
               rd16(RK3576_EMMC_EINTSTS));
      }
    else
      {
        syslog(LOG_ERR, "EMMCPROBE: block0 [0..3]=%08" PRIx32 " %08" PRIx32
               " %08" PRIx32 " %08" PRIx32 " ... [127]=%08" PRIx32
               "  <<< DATA PATH OK\n", blk[0], blk[1], blk[2], blk[3],
               blk[127]);
      }
  }

  syslog(LOG_ERR, "EMMCPROBE: ===== done =====\n");
}

#endif /* CONFIG_KICKPI_K7_EMMC_PROBE */
