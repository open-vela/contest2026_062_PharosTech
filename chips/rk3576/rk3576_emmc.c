/****************************************************************************
 * chips/rk3576/rk3576_emmc.c
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
 * RK3576 eMMC host controller driver -- Synopsys DesignWare Cortex MSHC
 * (dwcmshc), which presents a standard SDHCI 3.0 register block.  Implements
 * the NuttX struct sdio_dev_s so the generic drivers/mmcsd/mmcsd_sdio.c stack
 * drives it.  The data path supports SDHCI ADMA2 with transparent PIO
 * fallback for buffers that cannot be addressed safely by 32-bit ADMA2.
 * HS400/CQE/DLL support uses the Rockchip vendor area and is separate.
 *
 * The eMMC is a non-removable, loader-configured boot device: the bootloader
 * has already ungated the CRU clock domain and configured the pin IOMUX, so
 * this driver adds no CRU or pinctrl code (HOSTVER/CAP read back valid with
 * the loader configuration only).
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
#include <sys/types.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mmcsd.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_emmc.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_emmc.h"

#ifdef CONFIG_RK3576_EMMC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Single eMMC host on the RK3576. */

#define RK3576_EMMC_NHOSTS 1

/* Card clock parents provided by RK3576 CRU. */
#define RK3576_EMMC_GPLL_FREQ     1188000000
#define RK3576_EMMC_OSC_FREQ      24000000

#define RK3576_EMMC_CRU_CLKSEL    89
#define RK3576_EMMC_CRU_DIV_SHIFT 8
#define RK3576_EMMC_CRU_DIV_MASK  (0x3f << 8)
#define RK3576_EMMC_CRU_SEL_SHIFT 14
#define RK3576_EMMC_CRU_SEL_MASK  (3 << 14)
#define RK3576_EMMC_CRU_SEL_GPLL  0
#define RK3576_EMMC_CRU_SEL_OSC   2

/* Target card clock for each stage. */

#define RK3576_EMMC_ID_FREQ    400000    /* Identification mode (<400KHz) */
#define RK3576_EMMC_XFER_FREQ  52000000  /* MMC High Speed clock request */
#define RK3576_EMMC_HS200_FREQ 200000000 /* MMC HS200 clock request */
#define RK3576_EMMC_HS400_FREQ 200000000 /* MMC HS400 clock request */

/* Timeout control: data timeout counter = TMCLK * 2^(13 + value); 0x0e is the
 * maximum, giving the longest tolerated data/busy timeout.
 */

#define RK3576_EMMC_TOUTCTRL_MAX 0x0e

/* Extended CSD fields used to select the next high-speed mode. */

#define RK3576_EMMC_EXTCSD_STROBE_SUPPORT 184
#define RK3576_EMMC_EXTCSD_HS_TIMING      185
#define RK3576_EMMC_EXTCSD_REV            192
#define RK3576_EMMC_EXTCSD_DEVICE_TYPE    196

/* Bus width selected on widebus(true).  The board wires all eight data lines,
 * and the generic mmcsd layer negotiates 8-bit MMC operation.
 */

#define RK3576_EMMC_WIDEBITS EMMC_HOSTCTRL1_DWIDTH8

/* Busy-wait loop limit for register self-clear / present-state polling. */

#define RK3576_EMMC_SPIN           1000000
#define RK3576_EMMC_TUNING_RETRIES 128
#define RK3576_EMMC_TUNING_SIZE    128

#ifdef CONFIG_SDIO_DMA
#define RK3576_EMMC_ADMA_NDESC    16
#define RK3576_EMMC_ADMA_BUFSZ    65536
#define RK3576_EMMC_ADMA_BOUNDARY (128 * 1024 * 1024)
#define RK3576_EMMC_ADMA_MAXXFR   (512 * 1024)
#define RK3576_EMMC_ADMA_LIMIT    UINT64_C(0x100000000)

#define EMMC_ADMA2_VALID          (1 << 0)
#define EMMC_ADMA2_END            (1 << 1)
#define EMMC_ADMA2_ACT_TRAN       (2 << 4)
#endif

/* Packed interrupt sets: the SDHCI normal (16-bit) and error (16-bit) status
 * live in separate registers, so a single 32-bit value carries the normal
 * bits in [15:0] and the error bits in [31:16].
 */

#define EMMC_ERR_SHIFT    16
#define EMMC_NPART(x)     ((uint16_t)((x)&0xffff))
#define EMMC_EPART(x)     ((uint16_t)(((x) >> EMMC_ERR_SHIFT) & 0xffff))
#define EMMC_MKERR(e)     ((uint32_t)(e) << EMMC_ERR_SHIFT)

#define EMMC_CMDDONE_INTS (EMMC_NINT_CMDDONE)
#define EMMC_XFRDONE_INTS (EMMC_NINT_XFERDONE)
#define EMMC_RXRDY_INT    (EMMC_NINT_BUFRDRDY)
#define EMMC_TXRDY_INT    (EMMC_NINT_BUFWRRDY)

#define EMMC_RESPERR_INTS                                                 \
  EMMC_MKERR(EMMC_EINT_CMDTIMEOUT | EMMC_EINT_CMDCRC | EMMC_EINT_CMDEND | \
             EMMC_EINT_CMDIDX)
#define EMMC_DATAERR_INTS                                                 \
  EMMC_MKERR(EMMC_EINT_DATTIMEOUT | EMMC_EINT_DATCRC | EMMC_EINT_DATEND | \
             EMMC_EINT_ADMA)

/* Clear-all mask for the status registers (write-1-to-clear). */

#define EMMC_INT_CLRALL 0xffff

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* RK3576 eMMC host private state.  The first member is sdio_dev_s so a
 * struct sdio_dev_s pointer can be up-cast to this structure.
 */

struct rk3576_emmc_dev_s
{
  struct sdio_dev_s dev; /* Standard SDIO interface (must be first) */

  uintptr_t base; /* Controller register base address */
  int irq;        /* Controller interrupt number */

  /* Event wait support */

  sem_t waitsem;                       /* Wait-for-event semaphore */
  struct wdog_s waitwdog;              /* Wait timeout watchdog */
  volatile sdio_eventset_t waitevents; /* Enabled wait event set */
  volatile sdio_eventset_t wkupevent;  /* Wakeup event set */

  /* Media change callback */

  sdio_eventset_t cbevents; /* Enabled callback event set */
  worker_t callback;        /* Registered callback function */
  void *cbarg;              /* Callback argument */
  struct work_s cbwork;     /* Callback work queue item */

  /* Data transfer (PIO) state */

  uint32_t *buffer;            /* Transfer buffer (32bit aligned) */
  volatile size_t remaining;   /* Remaining byte count */
  volatile uint32_t xfrints;   /* Signal-enable set during data transfer */
  volatile uint32_t waitints;  /* Signal-enable set while waiting on a cmd */
  uint32_t blocksize;          /* Current block size (from blocksetup) */
  uint16_t xfermode;           /* Prepared mode for the next data command */
  uint32_t lastcmd;            /* Last command encoding for diagnostics */
  uint32_t lastarg;            /* Last command argument for diagnostics */
  uint8_t extcsd_rev;          /* Extended CSD revision */
  uint8_t device_type;         /* Extended CSD supported timing modes */
  uint8_t strobe_support;      /* Enhanced strobe support */
  bool dll_ready;              /* Rockchip high-speed DLL is locked */
  volatile bool tuning_active; /* CMD21 tuning is in progress */

#ifdef CONFIG_SDIO_DMA
  volatile bool dma_active; /* Current transfer uses ADMA2 */
  bool dma_read;            /* ADMA direction: true = card to memory */
  uintptr_t dma_buffer;     /* DMA buffer start */
  size_t dma_length;        /* DMA buffer length */
  uint8_t *dma_bounce_dest; /* Unaligned read destination */
#endif
};

#ifdef CONFIG_SDIO_DMA
struct rk3576_emmc_adma2_desc_s
{
  uint16_t attr;
  uint16_t length;
  uint32_t address;
};
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register access */

static inline uint8_t rk3576_emmc_getreg8(struct rk3576_emmc_dev_s *priv,
                                          unsigned int offset);
static inline uint16_t rk3576_emmc_getreg16(struct rk3576_emmc_dev_s *priv,
                                            unsigned int offset);
static inline uint32_t rk3576_emmc_getreg32(struct rk3576_emmc_dev_s *priv,
                                            unsigned int offset);
static inline void rk3576_emmc_putreg8(struct rk3576_emmc_dev_s *priv,
                                       unsigned int offset, uint8_t value);
static inline void rk3576_emmc_putreg16(struct rk3576_emmc_dev_s *priv,
                                        unsigned int offset, uint16_t value);
static inline void rk3576_emmc_putreg32(struct rk3576_emmc_dev_s *priv,
                                        unsigned int offset, uint32_t value);

/* Low-level helpers */

static void rk3576_emmc_setsigen(struct rk3576_emmc_dev_s *priv);
static uint32_t rk3576_emmc_setcruclock(uint32_t freq);
static void rk3576_emmc_resetlines(struct rk3576_emmc_dev_s *priv,
                                   uint8_t lines);
static void rk3576_emmc_setclock(struct rk3576_emmc_dev_s *priv,
                                 uint32_t freq);
static int rk3576_emmc_configdll(struct rk3576_emmc_dev_s *priv, bool hs400);
static void rk3576_emmc_configwaitints(struct rk3576_emmc_dev_s *priv,
                                       uint32_t waitints,
                                       sdio_eventset_t waitevents,
                                       sdio_eventset_t wkupevent);
static void rk3576_emmc_configxfrints(struct rk3576_emmc_dev_s *priv,
                                      uint32_t xfrints);
static void rk3576_emmc_endwait(struct rk3576_emmc_dev_s *priv,
                                sdio_eventset_t wkupevent);
static void rk3576_emmc_eventtimeout(wdparm_t arg);
static void rk3576_emmc_recvfifo(struct rk3576_emmc_dev_s *priv);
static void rk3576_emmc_sendfifo(struct rk3576_emmc_dev_s *priv);
static void rk3576_emmc_callback(struct rk3576_emmc_dev_s *priv);

#ifdef CONFIG_SDIO_DMA
static void rk3576_emmc_dma_disable(struct rk3576_emmc_dev_s *priv);
static bool rk3576_emmc_dma_ok(const uint8_t *buffer, size_t buflen);
static int rk3576_emmc_dma_setup(struct rk3576_emmc_dev_s *priv,
                                 const uint8_t *buffer, size_t buflen,
                                 bool write);
#endif

/* Interrupt handling */

static int rk3576_emmc_interrupt(int irq, void *context, void *arg);

/* sdio_dev_s methods */

static void rk3576_emmc_reset(struct sdio_dev_s *dev);
static sdio_capset_t rk3576_emmc_capabilities(struct sdio_dev_s *dev);
static sdio_statset_t rk3576_emmc_status(struct sdio_dev_s *dev);
static void rk3576_emmc_widebus(struct sdio_dev_s *dev, bool enable);
static void rk3576_emmc_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate);
static int rk3576_emmc_attach(struct sdio_dev_s *dev);

static int rk3576_emmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                               uint32_t arg);
static int rk3576_emmc_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                 size_t nbytes);
static int rk3576_emmc_sendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                                 size_t nbytes);
static int rk3576_emmc_cancel(struct sdio_dev_s *dev);

static int rk3576_emmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd);
static int rk3576_emmc_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                                 uint32_t *rshort);
static int rk3576_emmc_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t rlong[4]);

static void rk3576_emmc_waitenable(struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset, uint32_t timeout);
static sdio_eventset_t rk3576_emmc_eventwait(struct sdio_dev_s *dev);
static void rk3576_emmc_callbackenable(struct sdio_dev_s *dev,
                                       sdio_eventset_t eventset);
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int rk3576_emmc_registercallback(struct sdio_dev_s *dev,
                                        worker_t callback, void *arg);
#endif
static void rk3576_emmc_gotextcsd(struct sdio_dev_s *dev,
                                  const uint8_t *buffer);
static int rk3576_emmc_execute_tuning(struct sdio_dev_s *dev, uint32_t cmd);
static int rk3576_emmc_hs400_enhanced_strobe(struct sdio_dev_s *dev,
                                             bool enable);

#ifdef CONFIG_SDIO_DMA
#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
static int rk3576_emmc_dmapreflight(struct sdio_dev_s *dev,
                                    const uint8_t *buffer, size_t buflen);
#endif
static int rk3576_emmc_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                    size_t buflen);
static int rk3576_emmc_dmasendsetup(struct sdio_dev_s *dev,
                                    const uint8_t *buffer, size_t buflen);
#endif
#ifdef CONFIG_SDIO_BLOCKSETUP
static void rk3576_emmc_blocksetup(struct sdio_dev_s *dev,
                                   unsigned int blocklen,
                                   unsigned int nblocks);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Shared sdio_dev_s operations template. */

static const struct sdio_dev_s g_rk3576_emmc_ops = {
  .reset = rk3576_emmc_reset,
  .capabilities = rk3576_emmc_capabilities,
  .status = rk3576_emmc_status,
  .widebus = rk3576_emmc_widebus,
  .clock = rk3576_emmc_clock,
  .attach = rk3576_emmc_attach,
  .sendcmd = rk3576_emmc_sendcmd,
  .recvsetup = rk3576_emmc_recvsetup,
  .sendsetup = rk3576_emmc_sendsetup,
  .cancel = rk3576_emmc_cancel,
  .waitresponse = rk3576_emmc_waitresponse,
  .recv_r1 = rk3576_emmc_recvshort,
  .recv_r2 = rk3576_emmc_recvlong,
  .recv_r3 = rk3576_emmc_recvshort,
  .recv_r4 = rk3576_emmc_recvshort,
  .recv_r5 = rk3576_emmc_recvshort,
  .recv_r6 = rk3576_emmc_recvshort,
  .recv_r7 = rk3576_emmc_recvshort,
  .waitenable = rk3576_emmc_waitenable,
  .eventwait = rk3576_emmc_eventwait,
  .callbackenable = rk3576_emmc_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  .registercallback = rk3576_emmc_registercallback,
#endif
  .gotextcsd = rk3576_emmc_gotextcsd,
  .execute_tuning = rk3576_emmc_execute_tuning,
  .hs400_enhanced_strobe = rk3576_emmc_hs400_enhanced_strobe,
#ifdef CONFIG_SDIO_DMA
#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
  .dmapreflight = rk3576_emmc_dmapreflight,
#endif
  .dmarecvsetup = rk3576_emmc_dmarecvsetup,
  .dmasendsetup = rk3576_emmc_dmasendsetup,
#endif
#ifdef CONFIG_SDIO_BLOCKSETUP
  .blocksetup = rk3576_emmc_blocksetup,
#endif
};

/* Per-slot register base and IRQ (indexed by slot number). */

static const struct rk3576_emmc_cfg_s
{
  uintptr_t base;
  int irq;
} g_emmc_cfg[RK3576_EMMC_NHOSTS] = {
  {
      .base = RK3576_EMMC_ADDR, .irq = RK3576_IRQ_EMMC, /* slot 0: eMMC */
  },
};

/* Host instances (ops copied from the template at initialize time). */

static struct rk3576_emmc_dev_s g_emmc_hosts[RK3576_EMMC_NHOSTS];
static uint8_t g_emmc_tuning[RK3576_EMMC_TUNING_SIZE] aligned_data(64);

#ifdef CONFIG_SDIO_DMA
static struct rk3576_emmc_adma2_desc_s
    g_emmc_adma_descs[RK3576_EMMC_ADMA_NDESC] aligned_data(64);
#ifdef CONFIG_RK3576_DMA_ALLOC
static uint8_t *g_emmc_dma_bounce;
#endif
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_emmc_getreg8 / getreg16 / getreg32 / putreg8 / putreg16 /
 *       putreg32
 *
 * Description:
 *   Controller register read/write helpers.  The SDHCI block mixes 8/16/32-bit
 *   registers, so access must use the matching width.
 ****************************************************************************/

static inline uint8_t rk3576_emmc_getreg8(struct rk3576_emmc_dev_s *priv,
                                          unsigned int offset)
{
  return getreg8(priv->base + offset);
}

static inline uint16_t rk3576_emmc_getreg16(struct rk3576_emmc_dev_s *priv,
                                            unsigned int offset)
{
  return getreg16(priv->base + offset);
}

static inline uint32_t rk3576_emmc_getreg32(struct rk3576_emmc_dev_s *priv,
                                            unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void rk3576_emmc_putreg8(struct rk3576_emmc_dev_s *priv,
                                       unsigned int offset, uint8_t value)
{
  putreg8(value, priv->base + offset);
}

static inline void rk3576_emmc_putreg16(struct rk3576_emmc_dev_s *priv,
                                        unsigned int offset, uint16_t value)
{
  putreg16(value, priv->base + offset);
}

static inline void rk3576_emmc_putreg32(struct rk3576_emmc_dev_s *priv,
                                        unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: rk3576_emmc_setsigen
 *
 * Description:
 *   Write the SDHCI signal-enable registers (0x38/0x3a) from the combined
 *   wait + transfer interrupt sets.  The status-enable registers (0x34/0x36)
 *   are held at 0xffff (see reset) so the status bits always latch; the
 *   signal-enable registers are what actually route the IRQ line, so they
 *   carry only the events currently of interest.
 ****************************************************************************/

static void rk3576_emmc_setsigen(struct rk3576_emmc_dev_s *priv)
{
  uint32_t combined = priv->xfrints | priv->waitints;

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSIGEN, EMMC_NPART(combined));
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSIGEN, EMMC_EPART(combined));
}

/****************************************************************************
 * Name: rk3576_emmc_setcruclock
 ****************************************************************************/

static uint32_t rk3576_emmc_setcruclock(uint32_t freq)
{
  uintptr_t regaddr =
      RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(RK3576_EMMC_CRU_CLKSEL);
  uint32_t parent;
  uint32_t select;
  uint32_t divisor;
  uint32_t value;

  if (freq <= RK3576_EMMC_ID_FREQ)
    {
      parent = RK3576_EMMC_OSC_FREQ;
      select = RK3576_EMMC_CRU_SEL_OSC;
    }
  else
    {
      parent = RK3576_EMMC_GPLL_FREQ;
      select = RK3576_EMMC_CRU_SEL_GPLL;
    }

  divisor = (parent + freq - 1) / freq;
  if (divisor < 1)
    {
      divisor = 1;
    }
  else if (divisor > 64)
    {
      divisor = 64;
    }

  value = ((RK3576_EMMC_CRU_DIV_MASK | RK3576_EMMC_CRU_SEL_MASK) << 16) |
          (select << RK3576_EMMC_CRU_SEL_SHIFT) |
          ((divisor - 1) << RK3576_EMMC_CRU_DIV_SHIFT);
  putreg32(value, regaddr);
  return parent / divisor;
}

/****************************************************************************
 * Name: rk3576_emmc_setclock
 *
 * Description:
 *   Set the card clock through CRU.  Rockchip dwcmshc does not use the SDHCI
 *   frequency divider, so CLKCTRL selects the undivided CRU clock.
 ****************************************************************************/

static void rk3576_emmc_setclock(struct rk3576_emmc_dev_s *priv, uint32_t freq)
{
  uint32_t divided;
  uint16_t clk;
  int i;

  /* 1) Disable the SD clock, keep the internal clock running. */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_CLKCTRL, EMMC_CLKCTRL_INTLEN);

  if (freq == 0)
    {
      return;
    }

  /* 2) Program the external card clock and use SDHCI divider zero. */

  divided = rk3576_emmc_setcruclock(freq);

  /* 3) Program the divider and wait for the internal clock to stabilise. */

  clk = EMMC_CLKCTRL_INTLEN;
  rk3576_emmc_putreg16(priv, RK3576_EMMC_CLKCTRL, clk);

  for (i = 0; i < RK3576_EMMC_SPIN; i++)
    {
      if ((rk3576_emmc_getreg16(priv, RK3576_EMMC_CLKCTRL) &
           EMMC_CLKCTRL_INTSTABLE) != 0)
        {
          break;
        }
    }

  /* 4) Enable the SD clock to the card. */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_CLKCTRL, clk | EMMC_CLKCTRL_SDCLKEN);

  mcinfo("eMMC clock requested=%" PRIu32 " configured=%" PRIu32
         " hostctrl1=%02x\n",
         freq, divided, rk3576_emmc_getreg8(priv, RK3576_EMMC_HOSTCTRL1));
}

/****************************************************************************
 * Name: rk3576_emmc_configdll
 ****************************************************************************/

static int rk3576_emmc_configdll(struct rk3576_emmc_dev_s *priv, bool hs400)
{
  uint16_t savedclock;
  uint32_t value;
  int i;

  savedclock = rk3576_emmc_getreg16(priv, RK3576_EMMC_CLKCTRL);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_CLKCTRL, 0);

  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLCTRL, 1 << 1);
  up_udelay(1);
  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLCTRL, 0);

  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLRXCLK, EMMC_DLL_DLYENA);
  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLCTRL,
                       EMMC_DLLCTRL_START_POINT | EMMC_DLLCTRL_INCREMENT |
                           EMMC_DLLCTRL_START);

  for (i = 0; i < RK3576_EMMC_SPIN; i++)
    {
      value = rk3576_emmc_getreg32(priv, RK3576_EMMC_VENDOR_DLLSTATUS0);
      if ((value & EMMC_DLL_STATUS_LOCKED) != 0 &&
          (value & EMMC_DLL_STATUS_TIMEOUT) == 0)
        {
          break;
        }
    }

  if (i >= RK3576_EMMC_SPIN)
    {
      syslog(LOG_ERR,
             "ERROR: eMMC DLL lock timed out status=%08" PRIx32
             " clksel89=%08" PRIx32 "\n",
             value, getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(89)));
      rk3576_emmc_putreg16(priv, RK3576_EMMC_CLKCTRL, savedclock);
      return -ETIMEDOUT;
    }

  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_ATCTRL,
                       (1 << 16) | (3 << 17) | (3 << 19));
  rk3576_emmc_putreg32(
      priv, RK3576_EMMC_VENDOR_DLLTXCLK,
      EMMC_DLL_DLYENA | EMMC_DLL_TAP_FROM_SW | EMMC_DLL_RXCLK_NOINVERTER |
          (hs400 ? EMMC_DLL_HS400_TXCLK_TAP : EMMC_DLL_HS200_TXCLK_TAP));
  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLSTRBIN,
                       EMMC_DLL_DLYENA | EMMC_DLL_TAP_FROM_SW |
                           EMMC_DLL_STRBIN_TAP);
  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLCMDOUT,
                       hs400 ? EMMC_DLL_DLYENA | EMMC_DLL_TAP_FROM_SW |
                                   EMMC_DLL_CMDOUT_SRC_CLK_NEG |
                                   EMMC_DLL_CMDOUT_EN_SRC_CLK_NEG |
                                   EMMC_DLL_HS400_CMDOUT_TAP
                             : 0);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_CLKCTRL, savedclock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_resetlines
 ****************************************************************************/

static void rk3576_emmc_resetlines(struct rk3576_emmc_dev_s *priv,
                                   uint8_t lines)
{
  int i;

  rk3576_emmc_putreg8(priv, RK3576_EMMC_SWRESET, lines);
  for (i = 0; i < RK3576_EMMC_SPIN; i++)
    {
      if ((rk3576_emmc_getreg8(priv, RK3576_EMMC_SWRESET) & lines) == 0)
        {
          return;
        }
    }

  syslog(LOG_ERR, "ERROR: eMMC line reset timed out mask=%02x\n", lines);
}

/****************************************************************************
 * Name: rk3576_emmc_configwaitints
 *
 * Description:
 *   Atomically configure the wait event set and the wait signal-enable bits.
 ****************************************************************************/

static void rk3576_emmc_configwaitints(struct rk3576_emmc_dev_s *priv,
                                       uint32_t waitints,
                                       sdio_eventset_t waitevents,
                                       sdio_eventset_t wkupevent)
{
  irqstate_t flags;

  flags = enter_critical_section();
  priv->waitevents = waitevents;
  priv->wkupevent = wkupevent;
  priv->waitints = waitints;
  rk3576_emmc_setsigen(priv);
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: rk3576_emmc_configxfrints
 *
 * Description:
 *   Configure the data-transfer signal-enable bits (kept separate from the
 *   command-wait bits so each can be cleared independently).
 ****************************************************************************/

static void rk3576_emmc_configxfrints(struct rk3576_emmc_dev_s *priv,
                                      uint32_t xfrints)
{
  irqstate_t flags;

  flags = enter_critical_section();
  priv->xfrints = xfrints;
  rk3576_emmc_setsigen(priv);
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: rk3576_emmc_endwait
 *
 * Description:
 *   End one event wait in interrupt context: stop the watchdog, disable the
 *   wait interrupts and wake the waiting thread.
 ****************************************************************************/

static void rk3576_emmc_endwait(struct rk3576_emmc_dev_s *priv,
                                sdio_eventset_t wkupevent)
{
  wd_cancel(&priv->waitwdog);
  rk3576_emmc_configwaitints(priv, 0, 0, wkupevent);
  nxsem_post(&priv->waitsem);
}

/****************************************************************************
 * Name: rk3576_emmc_eventtimeout
 *
 * Description:
 *   Wait-timeout watchdog callback (software timeout).
 ****************************************************************************/

static void rk3576_emmc_eventtimeout(wdparm_t arg)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)arg;

  if ((priv->waitevents & SDIOWAIT_TIMEOUT) != 0)
    {
      syslog(LOG_ERR,
             "ERROR: emmc timeout cmd=%08" PRIx32 " arg=%08" PRIx32
             " present=%08" PRIx32 " nint=%04x eint=%04x xfer=%04x"
             " hwcmd=%04x remain=%lu block=%lu wait=%08" PRIx32 "\n",
             priv->lastcmd, priv->lastarg,
             rk3576_emmc_getreg32(priv, RK3576_EMMC_PRESENT),
             rk3576_emmc_getreg16(priv, RK3576_EMMC_NINTSTS),
             rk3576_emmc_getreg16(priv, RK3576_EMMC_EINTSTS),
             rk3576_emmc_getreg16(priv, RK3576_EMMC_XFERMODE),
             rk3576_emmc_getreg16(priv, RK3576_EMMC_CMD),
             (unsigned long)priv->remaining, (unsigned long)priv->blocksize,
             priv->waitevents);
      rk3576_emmc_endwait(priv, SDIOWAIT_TIMEOUT);
      mcerr("ERROR: Event wait timed out\n");
    }
}

/****************************************************************************
 * Name: rk3576_emmc_recvfifo
 *
 * Description:
 *   Drain the SDHCI buffer data port into the receive buffer (PIO), while the
 *   host reports buffer-read-enable and the caller still expects data.
 ****************************************************************************/

static void rk3576_emmc_recvfifo(struct rk3576_emmc_dev_s *priv)
{
  /* A buffer-read-ready interrupt means one whole block is already in the
   * buffer port; read the block out unconditionally (do NOT gate on the
   * BUFRDEN present-state bit -- on this dwcmshc the bit's per-word timing
   * does not track a word-at-a-time poll, and gating causes a short/zero
   * drain).  This matches the polled bring-up probe.
   */

  size_t chunk = priv->blocksize ? priv->blocksize : priv->remaining;
  while (chunk >= sizeof(uint32_t) && priv->remaining >= sizeof(uint32_t))
    {
      *priv->buffer++ = rk3576_emmc_getreg32(priv, RK3576_EMMC_BUFFER);
      priv->remaining -= sizeof(uint32_t);
      chunk -= sizeof(uint32_t);
    }
}

/****************************************************************************
 * Name: rk3576_emmc_sendfifo
 *
 * Description:
 *   Fill the SDHCI buffer data port from the send buffer (PIO), while the host
 *   reports buffer-write-enable and data remains.
 ****************************************************************************/

static void rk3576_emmc_sendfifo(struct rk3576_emmc_dev_s *priv)
{
  while (priv->remaining >= sizeof(uint32_t) &&
         (rk3576_emmc_getreg32(priv, RK3576_EMMC_PRESENT) &
          EMMC_PRESENT_BUFWREN) != 0)
    {
      rk3576_emmc_putreg32(priv, RK3576_EMMC_BUFFER, *priv->buffer++);
      priv->remaining -= sizeof(uint32_t);
    }
}

/****************************************************************************
 * Name: rk3576_emmc_callback
 *
 * Description:
 *   If the conditions are met, schedule the media-change callback onto the
 *   work queue.  The eMMC is non-removable, so this fires at most once (at
 *   callbackenable time) to report the fixed "inserted" state.
 ****************************************************************************/

static void rk3576_emmc_callback(struct rk3576_emmc_dev_s *priv)
{
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  if (priv->callback)
    {
      sdio_statset_t status = rk3576_emmc_status(&priv->dev);
      bool present = (status & SDIO_STATUS_PRESENT) != 0;

      if ((present && (priv->cbevents & SDIOMEDIA_INSERTED) != 0) ||
          (!present && (priv->cbevents & SDIOMEDIA_EJECTED) != 0))
        {
          priv->cbevents = 0;
          work_queue(HPWORK, &priv->cbwork, priv->callback, priv->cbarg, 0);
        }
    }
#endif
}

/****************************************************************************
 * Name: rk3576_emmc_interrupt
 *
 * Description:
 *   Controller interrupt handler: handles command done/response errors, PIO
 *   data movement and transfer done/errors.  The SDHCI normal and error
 *   status registers are read and cleared (write-1-to-clear).
 ****************************************************************************/

static int rk3576_emmc_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)arg;
  uint16_t nint;
  uint16_t eint;

  UNUSED(irq);
  UNUSED(context);

  nint = rk3576_emmc_getreg16(priv, RK3576_EMMC_NINTSTS);
  eint = rk3576_emmc_getreg16(priv, RK3576_EMMC_EINTSTS);

  /* Clear the latched status (write 1 to clear). */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSTS, nint);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSTS, eint);

  /* --- PIO data movement --- */

  if ((nint & EMMC_RXRDY_INT) != 0 && priv->buffer != NULL)
    {
      /* Drain the ready block; the transfer completes on XFERDONE. */

      rk3576_emmc_recvfifo(priv);

      /* During SDHCI tuning, dwcmshc completes each tuning iteration with
       * Buffer Read Ready but no normal Transfer Complete interrupt.
       */

      if (priv->tuning_active && priv->remaining == 0)
        {
          priv->buffer = NULL;
          rk3576_emmc_configxfrints(priv, 0);
          if ((priv->waitevents & SDIOWAIT_TRANSFERDONE) != 0)
            {
              rk3576_emmc_endwait(priv, SDIOWAIT_TRANSFERDONE);
            }
        }
    }

  if ((nint & EMMC_TXRDY_INT) != 0 && priv->buffer != NULL)
    {
      rk3576_emmc_sendfifo(priv);
    }

  /* --- Data transfer error --- */

  if ((eint & EMMC_EPART(EMMC_DATAERR_INTS)) != 0)
    {
      syslog(LOG_ERR,
             "ERROR: emmc data irq cmd=%08" PRIx32 " arg=%08" PRIx32
             " present=%08" PRIx32 " nint=%04x eint=%04x remain=%lu"
             " admaerr=%02x admaaddr=%08" PRIx32 "\n",
             priv->lastcmd, priv->lastarg,
             rk3576_emmc_getreg32(priv, RK3576_EMMC_PRESENT), nint, eint,
             (unsigned long)priv->remaining,
             rk3576_emmc_getreg8(priv, RK3576_EMMC_ADMAERR),
             rk3576_emmc_getreg32(priv, RK3576_EMMC_ADMAADDR));
#ifdef CONFIG_SDIO_DMA
      rk3576_emmc_dma_disable(priv);
#endif
      priv->remaining = 0;
      priv->buffer = NULL;
      rk3576_emmc_configxfrints(priv, 0);
      rk3576_emmc_resetlines(priv, EMMC_SWRESET_DAT);

      if ((priv->waitevents & (SDIOWAIT_TRANSFERDONE | SDIOWAIT_ERROR)) != 0)
        {
          rk3576_emmc_endwait(priv, SDIOWAIT_ERROR);
        }
    }

  /* --- Data transfer done --- */

  else if ((nint & EMMC_NINT_XFERDONE) != 0 && (priv->buffer != NULL
#ifdef CONFIG_SDIO_DMA
                                                || priv->dma_active
#endif
                                                ))
    {
      /* Drain any tail still in the buffer port.  On this dwcmshc XFERDONE can
       * post together with the final buffer-read-ready window, so read the
       * remaining words out unconditionally (the block is already buffered),
       * matching the polled probe rather than gating on BUFRDEN.  XFERDONE is
       * the single completion point.
       *
       * The priv->buffer != NULL guard is essential: a spurious/leftover
       * XFERDONE with no active transfer must NOT complete a wait, or it wakes
       * the next command's waiter before its data has arrived (observed:
       * EXT_CSD read back as zero).
       */

#ifdef CONFIG_SDIO_DMA
      if (priv->dma_active)
        {
          if (priv->dma_read)
            {
              up_invalidate_dcache(priv->dma_buffer,
                                   priv->dma_buffer + priv->dma_length);
              if (priv->dma_bounce_dest != NULL)
                {
                  memcpy(priv->dma_bounce_dest, (const void *)priv->dma_buffer,
                         priv->dma_length);
                }
            }

          rk3576_emmc_dma_disable(priv);
        }
      else
#endif
        while (priv->remaining >= sizeof(uint32_t))
          {
            *priv->buffer++ = rk3576_emmc_getreg32(priv, RK3576_EMMC_BUFFER);
            priv->remaining -= sizeof(uint32_t);
          }

      priv->remaining = 0;
      priv->buffer = NULL;
      rk3576_emmc_configxfrints(priv, 0);

      if ((priv->waitevents & SDIOWAIT_TRANSFERDONE) != 0)
        {
          rk3576_emmc_endwait(priv, SDIOWAIT_TRANSFERDONE);
        }
    }

  /* --- Command response error --- */

  if ((eint & EMMC_EPART(EMMC_RESPERR_INTS)) != 0)
    {
      if ((priv->waitevents &
           (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE | SDIOWAIT_ERROR)) != 0)
        {
          rk3576_emmc_endwait(priv, SDIOWAIT_ERROR);
        }
    }

  /* --- Command done --- */

  else if ((nint & EMMC_NINT_CMDDONE) != 0)
    {
      if ((priv->waitevents & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE)) != 0)
        {
          rk3576_emmc_endwait(priv,
                              priv->waitevents &
                                  (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE));
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_reset
 *
 * Description:
 *   Software-reset the host, bring up the internal + card clock and bus power,
 *   latch all status bits and restore the identification-mode defaults.  Adds
 *   no CRU/pinctrl setup: the eMMC is a non-removable, loader-configured boot
 *   device whose clock domain and pins are already up.
 ****************************************************************************/

static void rk3576_emmc_reset(struct sdio_dev_s *dev)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  irqstate_t flags;
  int i;

  flags = enter_critical_section();

  /* Software-reset the whole host and wait for the bit to self-clear. */

  rk3576_emmc_putreg8(priv, RK3576_EMMC_SWRESET, EMMC_SWRESET_ALL);
  for (i = 0; i < RK3576_EMMC_SPIN; i++)
    {
      if ((rk3576_emmc_getreg8(priv, RK3576_EMMC_SWRESET) &
           EMMC_SWRESET_ALL) == 0)
        {
          break;
        }
    }

  /* Internal clock on, wait until stable. */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_CLKCTRL, EMMC_CLKCTRL_INTLEN);
  for (i = 0; i < RK3576_EMMC_SPIN; i++)
    {
      if ((rk3576_emmc_getreg16(priv, RK3576_EMMC_CLKCTRL) &
           EMMC_CLKCTRL_INTSTABLE) != 0)
        {
          break;
        }
    }

  /* Bus power on at 1.8 V (eMMC VCCQ). */

  rk3576_emmc_putreg8(priv, RK3576_EMMC_PWRCTRL,
                      EMMC_PWRCTRL_1V8 | EMMC_PWRCTRL_ON);

  /* Data timeout to maximum, default to 1-bit bus for identification. */

  rk3576_emmc_putreg8(priv, RK3576_EMMC_TOUTCTRL, RK3576_EMMC_TOUTCTRL_MAX);
  rk3576_emmc_putreg8(priv, RK3576_EMMC_HOSTCTRL1, 0);

  /* Clear then latch all normal + error status.  The status-enable registers
   * MUST be 0xffff or the status bits never latch and a completed command
   * reads as if nothing happened.  The signal-enable registers stay clear
   * here (armed per-wait) so no spurious IRQ fires yet.
   */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSTS, EMMC_INT_CLRALL);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSTS, EMMC_INT_CLRALL);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTEN, EMMC_INT_CLRALL);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTEN, EMMC_INT_CLRALL);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSIGEN, 0);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSIGEN, 0);

  /* Reset the driver software state. */

  priv->buffer = NULL;
  priv->remaining = 0;
  priv->xfrints = 0;
  priv->waitints = 0;
  priv->waitevents = 0;
  priv->wkupevent = 0;
  priv->blocksize = 0;
  priv->xfermode = 0;
  priv->dll_ready = false;
  priv->tuning_active = false;
#ifdef CONFIG_SDIO_DMA
  priv->dma_active = false;
  priv->dma_read = false;
  priv->dma_buffer = 0;
  priv->dma_length = 0;
  priv->dma_bounce_dest = NULL;
#endif

  /* Identification-mode initial clock (<400KHz). */

  rk3576_emmc_setclock(priv, RK3576_EMMC_ID_FREQ);

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: rk3576_emmc_capabilities
 *
 * Description:
 *   Report host capabilities: 8-bit MMC High Speed bus and ADMA2 data path.
 *   DMABEFOREWRITE makes mmcsd run SENDSETUP before the write command, which
 *   is required on SDHCI because the transfer mode must be programmed before
 *   the command register write.
 ****************************************************************************/

static sdio_capset_t rk3576_emmc_capabilities(struct sdio_dev_s *dev)
{
  sdio_capset_t caps = 0;

  UNUSED(dev);

  /* The generic MMC layer switches the card to EXT_CSD BUS_WIDTH=8 before
   * asking the host to enable wide-bus operation.  Match that width here.
   */
  caps |= SDIO_CAPS_8BIT;
  caps |= SDIO_CAPS_MMC_HS_MODE;
  caps |= SDIO_CAPS_MMC_HS200_MODE;
  caps |= SDIO_CAPS_MMC_HS400_MODE;
  caps |= SDIO_CAPS_MMC_ENHANCED_STROBE;
  caps |= SDIO_CAPS_DMABEFOREWRITE;
#ifdef CONFIG_SDIO_DMA
  caps |= SDIO_CAPS_DMASUPPORTED;
#endif

  return caps;
}

/****************************************************************************
 * Name: rk3576_emmc_status
 *
 * Description:
 *   Return card present/write-protect status.  The eMMC is soldered and
 *   non-removable, so it is always reported present and never write-protected.
 ****************************************************************************/

static sdio_statset_t rk3576_emmc_status(struct sdio_dev_s *dev)
{
  UNUSED(dev);
  return SDIO_STATUS_PRESENT;
}

/****************************************************************************
 * Name: rk3576_emmc_widebus
 *
 * Description:
 *   Select the data bus width in HOSTCTRL1.  Wide mode is 8-bit to match the
 *   MMC_SWITCH issued by the generic mmcsd layer.
 ****************************************************************************/

static void rk3576_emmc_widebus(struct sdio_dev_s *dev, bool enable)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint8_t hc;

  hc = rk3576_emmc_getreg8(priv, RK3576_EMMC_HOSTCTRL1);
  hc &= ~(EMMC_HOSTCTRL1_DWIDTH4 | EMMC_HOSTCTRL1_DWIDTH8);

  if (enable)
    {
      hc |= RK3576_EMMC_WIDEBITS;
    }

  rk3576_emmc_putreg8(priv, RK3576_EMMC_HOSTCTRL1, hc);
}

/****************************************************************************
 * Name: rk3576_emmc_clock
 *
 * Description:
 *   Set the card clock and host timing for the requested stage.  The generic
 *   MMC layer switches EXT_CSD HS_TIMING before requesting the transfer
 *   clock, so CLOCK_MMC_TRANSFER selects SDHCI High Speed timing.  With the
 *   reported 200 MHz base clock, the programmed integer divider corresponds
 *   to 50 MHz for the 52 MHz request.
 ****************************************************************************/

static void rk3576_emmc_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint32_t freq;
  uint16_t hc2;
  uint8_t hc;

  hc = rk3576_emmc_getreg8(priv, RK3576_EMMC_HOSTCTRL1);
  hc &= ~EMMC_HOSTCTRL1_HISPD;
  hc2 = rk3576_emmc_getreg16(priv, RK3576_EMMC_HOSTCTRL2);
  hc2 &= ~(EMMC_HOSTCTRL2_UHSMASK | EMMC_HOSTCTRL2_EXEC_TUNING |
           EMMC_HOSTCTRL2_TUNED_CLK);

  switch (rate)
    {
      default:
      case CLOCK_SDIO_DISABLED:
        freq = 0;
        break;

      case CLOCK_IDMODE:
        freq = RK3576_EMMC_ID_FREQ;
        break;

      case CLOCK_MMC_TRANSFER:
        hc |= EMMC_HOSTCTRL1_HISPD;
        freq = RK3576_EMMC_XFER_FREQ;
        break;

      case CLOCK_MMC_HS200:
        hc |= EMMC_HOSTCTRL1_HISPD;
        hc2 |= EMMC_HOSTCTRL2_HS200 | EMMC_HOSTCTRL2_V18;
        freq = RK3576_EMMC_HS200_FREQ;
        break;

      case CLOCK_MMC_HS400:
        hc |= EMMC_HOSTCTRL1_HISPD;
        hc2 |= EMMC_HOSTCTRL2_HS400 | EMMC_HOSTCTRL2_V18;
        freq = RK3576_EMMC_HS400_FREQ;
        break;

      case CLOCK_SD_TRANSFER_1BIT:
      case CLOCK_SD_TRANSFER_4BIT:
        freq = RK3576_EMMC_XFER_FREQ;
        break;
    }

  rk3576_emmc_putreg8(priv, RK3576_EMMC_HOSTCTRL1, hc);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_HOSTCTRL2, hc2);
  rk3576_emmc_setclock(priv, freq);

  if (rate == CLOCK_MMC_HS200 || rate == CLOCK_MMC_HS400)
    {
      priv->dll_ready =
          rk3576_emmc_configdll(priv, rate == CLOCK_MMC_HS400) == OK;
      if (rate == CLOCK_MMC_HS400)
        {
          uint32_t emmcctrl;

          emmcctrl = rk3576_emmc_getreg32(priv, RK3576_EMMC_VENDOR_EMMCCTRL);
          emmcctrl |= EMMC_VENDOR_CARD_IS_EMMC;
          rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_EMMCCTRL, emmcctrl);
        }
    }
  else
    {
      priv->dll_ready = false;
      rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLCTRL,
                           EMMC_DLLCTRL_BYPASS | EMMC_DLLCTRL_START);
      rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLTXCLK, 0);
      rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_DLLCMDOUT, 0);
    }
}

/****************************************************************************
 * Name: rk3576_emmc_attach
 *
 * Description:
 *   Attach and enable the controller interrupt.
 ****************************************************************************/

static int rk3576_emmc_attach(struct sdio_dev_s *dev)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  int ret;

  ret = irq_attach(priv->irq, rk3576_emmc_interrupt, priv);
  if (ret < 0)
    {
      mcerr("ERROR: irq_attach failed irq=%d ret=%d\n", priv->irq, ret);
      return ret;
    }

  /* Disable all interrupt signals, clear pending status, then open the IRQ. */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSIGEN, 0);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSIGEN, 0);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSTS, EMMC_INT_CLRALL);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSTS, EMMC_INT_CLRALL);

  up_enable_irq(priv->irq);
  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_sendcmd
 *
 * Description:
 *   Issue a command.  Builds the 16-bit SDHCI command register from the NuttX
 *   command encoding and starts it with the mandated write sequence
 *   (ARG1 -> XFERMODE -> CMD).  For data commands the transfer mode has
 *   already been programmed by recv/sendsetup and is left intact; for
 *   non-data commands the transfer mode is cleared.  The command register
 *   write (16-bit to 0x0e) is what actually launches the command.
 ****************************************************************************/

static int rk3576_emmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                               uint32_t arg)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint32_t cmdidx;
  uint16_t regval;
  bool data;
  bool needdat;
  int i;

  cmdidx = (cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT;
  data = (cmd & MMCSD_DATAXFR_MASK) != 0;
  priv->lastcmd = cmd;
  priv->lastarg = arg;

  /* A command that uses the DAT line for data, or one that reports busy on
   * DAT (R1b), must wait for the DAT line to be free before it is issued.
   */

  needdat = data || (cmd & MMCSD_RESPONSE_MASK) == MMCSD_R1B_RESPONSE;
  regval = (uint16_t)(cmdidx << EMMC_CMD_INDEX_SHIFT);

  /* Response type: pick the response length and enable the CRC/index checks
   * appropriate for each response class.
   */

  switch (cmd & MMCSD_RESPONSE_MASK)
    {
      case MMCSD_NO_RESPONSE:
        regval |= EMMC_CMD_RESP_NONE;
        break;

      case MMCSD_R2_RESPONSE:
        /* 136-bit response, CRC checked, index not present. */

        regval |= EMMC_CMD_RESP_LONG | EMMC_CMD_CRCEN;
        break;

      case MMCSD_R3_RESPONSE:
      case MMCSD_R4_RESPONSE:
        /* Short OCR-class response: no CRC, no index check. */

        regval |= EMMC_CMD_RESP_SHORT;
        break;

      case MMCSD_R1B_RESPONSE:
        /* Short response with busy on the DAT line, CRC + index checked. */

        regval |= EMMC_CMD_RESP_SHORT_BUSY | EMMC_CMD_CRCEN | EMMC_CMD_IDXEN;
        break;

      default:
        /* R1/R5/R6/R7: short response, CRC + index checked. */

        regval |= EMMC_CMD_RESP_SHORT | EMMC_CMD_CRCEN | EMMC_CMD_IDXEN;
        break;
    }

  if (data)
    {
      regval |= EMMC_CMD_DATA;
    }

  /* Wait for the command (and, for data commands, the DAT) line to be free. */

  for (i = 0; i < RK3576_EMMC_SPIN; i++)
    {
      uint32_t present = rk3576_emmc_getreg32(priv, RK3576_EMMC_PRESENT);
      uint32_t busy =
          EMMC_PRESENT_CMDINHIBIT | (needdat ? EMMC_PRESENT_DATINHIBIT : 0);

      if ((present & busy) == 0)
        {
          break;
        }
    }

  if (i >= RK3576_EMMC_SPIN)
    {
      mcerr("ERROR: Command line busy cmd=%08" PRIx32 "\n", cmd);
      return -EBUSY;
    }

  /* Clear stale command status, write the argument, (clear the transfer mode
   * for non-data commands), then launch the command.
   */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSTS, EMMC_CMDDONE_INTS);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSTS,
                       EMMC_EPART(EMMC_RESPERR_INTS));

  rk3576_emmc_putreg32(priv, RK3576_EMMC_ARG1, arg);

  if (data)
    {
      rk3576_emmc_putreg16(priv, RK3576_EMMC_XFERMODE, priv->xfermode);
    }
  else
    {
      rk3576_emmc_putreg16(priv, RK3576_EMMC_XFERMODE, 0);
    }

  rk3576_emmc_putreg16(priv, RK3576_EMMC_CMD, regval);
  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_recvsetup
 *
 * Description:
 *   PIO receive setup: program block size/count and the read transfer mode,
 *   register the buffer and arm the buffer-read + transfer-done interrupts.
 *   Called by mmcsd before the read command, so the transfer mode is in place
 *   when sendcmd writes the command register.
 ****************************************************************************/

static int rk3576_emmc_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                 size_t nbytes)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint32_t blksz;
  uint32_t nblocks;
  uint16_t mode;

  DEBUGASSERT(buffer != NULL && nbytes > 0);
  DEBUGASSERT(((uintptr_t)buffer & 3) == 0); /* 32-bit aligned */

  blksz = priv->blocksize ? priv->blocksize : nbytes;
  nblocks = blksz ? (nbytes / blksz) : 1;

#ifdef CONFIG_SDIO_DMA
  rk3576_emmc_dma_disable(priv);
#endif

  priv->buffer = (uint32_t *)buffer;
  priv->remaining = nbytes;

  /* Clear stale data status. */

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSTS,
                       EMMC_NINT_XFERDONE | EMMC_NINT_BUFRDRDY);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSTS,
                       EMMC_EPART(EMMC_DATAERR_INTS));

  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKSIZE, (uint16_t)blksz);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKCOUNT, (uint16_t)nblocks);

  /* Single-block reads use only the read-direction select (matching the
   * proven bring-up probe); block-count + multi-block select are added only
   * for multi-block transfers.  Enabling block-count on a single-block read
   * makes this dwcmshc withhold the data phase (observed: CMD17 DAT timeout).
   */

  mode = EMMC_XFERMODE_DTDSEL;
  if (nblocks > 1)
    {
      mode |= EMMC_XFERMODE_BCEN | EMMC_XFERMODE_MSBSEL;
    }

  priv->xfermode = mode;
  rk3576_emmc_putreg16(priv, RK3576_EMMC_XFERMODE, mode);

  rk3576_emmc_configxfrints(priv, EMMC_RXRDY_INT | EMMC_XFRDONE_INTS |
                                      EMMC_DATAERR_INTS);
  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_sendsetup
 *
 * Description:
 *   PIO send setup: program block size/count and the write transfer mode,
 *   register the buffer and arm the buffer-write + transfer-done interrupts.
 *   No FIFO prefill is done (unlike dw_mmc): on SDHCI the buffer-write-ready
 *   interrupt fires only after the write command starts the data phase, so
 *   the data is pushed from the ISR.  DMABEFOREWRITE guarantees this setup
 *   runs before sendcmd writes the command register.
 ****************************************************************************/

static int rk3576_emmc_sendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                                 size_t nbytes)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint32_t blksz;
  uint32_t nblocks;
  uint16_t mode;

  DEBUGASSERT(buffer != NULL && nbytes > 0);
  DEBUGASSERT(((uintptr_t)buffer & 3) == 0);

  blksz = priv->blocksize ? priv->blocksize : nbytes;
  nblocks = blksz ? (nbytes / blksz) : 1;

#ifdef CONFIG_SDIO_DMA
  rk3576_emmc_dma_disable(priv);
#endif

  priv->buffer = (uint32_t *)buffer;
  priv->remaining = nbytes;

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSTS,
                       EMMC_NINT_XFERDONE | EMMC_NINT_BUFWRRDY);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSTS,
                       EMMC_EPART(EMMC_DATAERR_INTS));

  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKSIZE, (uint16_t)blksz);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKCOUNT, (uint16_t)nblocks);

  mode = EMMC_XFERMODE_BCEN;
  if (nblocks > 1)
    {
      mode |= EMMC_XFERMODE_MSBSEL;
    }

  priv->xfermode = mode;
  rk3576_emmc_putreg16(priv, RK3576_EMMC_XFERMODE, mode);

  rk3576_emmc_configxfrints(priv, EMMC_TXRDY_INT | EMMC_XFRDONE_INTS |
                                      EMMC_DATAERR_INTS);
  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_cancel
 *
 * Description:
 *   Cancel an outstanding data transfer: disable interrupts, stop the
 *   watchdog and clear software state.
 ****************************************************************************/

static int rk3576_emmc_cancel(struct sdio_dev_s *dev)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;

  rk3576_emmc_configxfrints(priv, 0);
  rk3576_emmc_configwaitints(priv, 0, 0, 0);
  wd_cancel(&priv->waitwdog);

  priv->buffer = NULL;
  priv->remaining = 0;
#ifdef CONFIG_SDIO_DMA
  rk3576_emmc_dma_disable(priv);
#endif
  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_waitresponse
 *
 * Description:
 *   Poll-wait until the command completes, checking for command timeout/CRC
 *   errors.  Used by mmcsd for the polled command path.
 ****************************************************************************/

static int rk3576_emmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  bool crccheck;
  uint16_t nint;
  uint16_t eint;
  int i;

  crccheck = (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R3_RESPONSE &&
             (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R4_RESPONSE;

  for (i = 0; i < RK3576_EMMC_SPIN; i++)
    {
      eint = rk3576_emmc_getreg16(priv, RK3576_EMMC_EINTSTS);

      if ((eint & EMMC_EINT_CMDTIMEOUT) != 0)
        {
          mcerr("ERROR: Response timed out cmd=%08" PRIx32 "\n", cmd);
          return -ETIMEDOUT;
        }

      if (crccheck && (eint & (EMMC_EINT_CMDCRC | EMMC_EINT_CMDEND |
                               EMMC_EINT_CMDIDX)) != 0)
        {
          mcerr("ERROR: Response error cmd=%08" PRIx32 " eint=%04x\n", cmd,
                eint);
          return -EIO;
        }

      nint = rk3576_emmc_getreg16(priv, RK3576_EMMC_NINTSTS);
      if ((nint & EMMC_NINT_CMDDONE) != 0)
        {
          return OK;
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_emmc_recvshort
 *
 * Description:
 *   Read a short (R1/R3/R4/R5/R6/R7) command response from RESP0.
 ****************************************************************************/

static int rk3576_emmc_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                                 uint32_t *rshort)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint16_t eint = rk3576_emmc_getreg16(priv, RK3576_EMMC_EINTSTS);

  if ((eint & EMMC_EINT_CMDTIMEOUT) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((cmd & MMCSD_RESPONSE_MASK) != MMCSD_R3_RESPONSE &&
      (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R4_RESPONSE &&
      (eint & (EMMC_EINT_CMDCRC | EMMC_EINT_CMDEND | EMMC_EINT_CMDIDX)) != 0)
    {
      return -EIO;
    }

  if (rshort != NULL)
    {
      *rshort = rk3576_emmc_getreg32(priv, RK3576_EMMC_RESP0);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_recvlong
 *
 * Description:
 *   Read a long (R2, 136-bit CID/CSD) command response.  SDHCI strips the CRC
 *   and stores the card response bits [127:8] right-justified in RESP3:RESP0
 *   (RESP0[31:0] = card[39:8] ... RESP3[23:0] = card[127:104]).  The mmcsd
 *   stack expects rlong[] to hold the card register with bit 127 at
 *   rlong[0] bit 31, so the four words are shifted left by 8 and reassembled;
 *   the stripped CRC byte reads back as zero in rlong[3] bits [7:0].
 ****************************************************************************/

static int rk3576_emmc_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t rlong[4])
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint16_t eint = rk3576_emmc_getreg16(priv, RK3576_EMMC_EINTSTS);
  uint32_t resp0;
  uint32_t resp1;
  uint32_t resp2;
  uint32_t resp3;

  UNUSED(cmd);

  if ((eint & EMMC_EINT_CMDTIMEOUT) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((eint & (EMMC_EINT_CMDCRC | EMMC_EINT_CMDEND)) != 0)
    {
      return -EIO;
    }

  if (rlong != NULL)
    {
      resp0 = rk3576_emmc_getreg32(priv, RK3576_EMMC_RESP0);
      resp1 = rk3576_emmc_getreg32(priv, RK3576_EMMC_RESP1);
      resp2 = rk3576_emmc_getreg32(priv, RK3576_EMMC_RESP2);
      resp3 = rk3576_emmc_getreg32(priv, RK3576_EMMC_RESP3);

      rlong[0] = (resp3 << 8) | (resp2 >> 24);
      rlong[1] = (resp2 << 8) | (resp1 >> 24);
      rlong[2] = (resp1 << 8) | (resp0 >> 24);
      rlong[3] = (resp0 << 8);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_emmc_waitenable
 *
 * Description:
 *   Enable a set of wait events, arm the matching signal-enable bits and start
 *   the software timeout watchdog.
 ****************************************************************************/

static void rk3576_emmc_waitenable(struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset, uint32_t timeout)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint32_t waitints = 0;

  if ((eventset & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE)) != 0)
    {
      waitints |= EMMC_CMDDONE_INTS | EMMC_RESPERR_INTS;
    }

  if ((eventset & SDIOWAIT_TRANSFERDONE) != 0)
    {
      waitints |= EMMC_XFRDONE_INTS | EMMC_DATAERR_INTS;
    }

  rk3576_emmc_configwaitints(priv, waitints, eventset, 0);

  if ((eventset & SDIOWAIT_TIMEOUT) != 0 && timeout > 0)
    {
      int ret = wd_start(&priv->waitwdog, MSEC2TICK(timeout),
                         rk3576_emmc_eventtimeout, (wdparm_t)priv);
      if (ret < 0)
        {
          mcerr("ERROR: wd_start failed ret=%d\n", ret);
        }
    }
}

/****************************************************************************
 * Name: rk3576_emmc_eventwait
 *
 * Description:
 *   Block waiting for one of the enabled events (or a timeout).  Returns the
 *   wakeup event set.
 ****************************************************************************/

static sdio_eventset_t rk3576_emmc_eventwait(struct sdio_dev_s *dev)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  sdio_eventset_t wkupevent;

  for (;;)
    {
      nxsem_wait_uninterruptible(&priv->waitsem);

      wkupevent = priv->wkupevent;
      if (wkupevent != 0)
        {
          break;
        }
    }

  rk3576_emmc_configwaitints(priv, 0, 0, 0);
  wd_cancel(&priv->waitwdog);
  priv->wkupevent = 0;

  return wkupevent;
}

/****************************************************************************
 * Name: rk3576_emmc_callbackenable
 ****************************************************************************/

static void rk3576_emmc_callbackenable(struct sdio_dev_s *dev,
                                       sdio_eventset_t eventset)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;

  priv->cbevents = eventset;
  rk3576_emmc_callback(priv);
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
/****************************************************************************
 * Name: rk3576_emmc_registercallback
 ****************************************************************************/

static int rk3576_emmc_registercallback(struct sdio_dev_s *dev,
                                        worker_t callback, void *arg)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;

  priv->cbevents = 0;
  priv->cbarg = arg;
  priv->callback = callback;
  return OK;
}
#endif

/****************************************************************************
 * Name: rk3576_emmc_gotextcsd
 *
 * Description:
 *   Receive EXT CSD notification.  The eMMC EXT_CSD is read through the normal
 *   data path, so nothing extra is required here.
 ****************************************************************************/

static void rk3576_emmc_gotextcsd(struct sdio_dev_s *dev,
                                  const uint8_t *buffer)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;

  priv->extcsd_rev = buffer[RK3576_EMMC_EXTCSD_REV];
  priv->device_type = buffer[RK3576_EMMC_EXTCSD_DEVICE_TYPE];
  priv->strobe_support = buffer[RK3576_EMMC_EXTCSD_STROBE_SUPPORT];

  mcinfo("eMMC EXT_CSD rev=%u device_type=%02x hs_timing=%u"
         " strobe=%02x\n",
         priv->extcsd_rev, priv->device_type,
         buffer[RK3576_EMMC_EXTCSD_HS_TIMING], priv->strobe_support);
}

/****************************************************************************
 * Name: rk3576_emmc_execute_tuning
 ****************************************************************************/

static int rk3576_emmc_execute_tuning(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  sdio_eventset_t event;
  uint32_t response;
  uint16_t hc2;
  int ret = -EIO;
  int i;

  if (cmd != MMC_CMD21)
    {
      return -EINVAL;
    }

  if (!priv->dll_ready)
    {
      return -EIO;
    }

  mcinfo("eMMC HS200 tuning start hostctrl2=%04x\n",
         rk3576_emmc_getreg16(priv, RK3576_EMMC_HOSTCTRL2));

  hc2 = rk3576_emmc_getreg16(priv, RK3576_EMMC_HOSTCTRL2);
  hc2 |= EMMC_HOSTCTRL2_EXEC_TUNING;
  hc2 &= ~EMMC_HOSTCTRL2_TUNED_CLK;
  rk3576_emmc_putreg16(priv, RK3576_EMMC_HOSTCTRL2, hc2);
  priv->tuning_active = true;

  for (i = 0; i < RK3576_EMMC_TUNING_RETRIES; i++)
    {
      priv->blocksize = RK3576_EMMC_TUNING_SIZE;
      rk3576_emmc_waitenable(
          dev, SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT | SDIOWAIT_ERROR, 100);
      ret = rk3576_emmc_recvsetup(dev, g_emmc_tuning, RK3576_EMMC_TUNING_SIZE);
      if (ret < 0)
        {
          break;
        }

      ret = rk3576_emmc_sendcmd(dev, cmd, 0);
      if (ret == OK)
        {
          ret = rk3576_emmc_waitresponse(dev, cmd);
        }

      if (ret == OK)
        {
          ret = rk3576_emmc_recvshort(dev, cmd, &response);
        }

      if (ret == OK)
        {
          event = rk3576_emmc_eventwait(dev);
          if ((event & SDIOWAIT_TRANSFERDONE) == 0)
            {
              ret = -EIO;
            }
        }

      hc2 = rk3576_emmc_getreg16(priv, RK3576_EMMC_HOSTCTRL2);
      if ((hc2 & EMMC_HOSTCTRL2_EXEC_TUNING) == 0)
        {
          priv->tuning_active = false;
          if ((hc2 & EMMC_HOSTCTRL2_TUNED_CLK) != 0)
            {
              mcinfo("eMMC HS200 tuning complete iterations=%d"
                     " hostctrl2=%04x dll=%08" PRIx32 " clksel89=%08" PRIx32
                     "\n",
                     i + 1, hc2,
                     rk3576_emmc_getreg32(priv, RK3576_EMMC_VENDOR_DLLSTATUS0),
                     getreg32(RK3576_CRU_ADDR +
                              RK3576_CRU_CLKSEL_CON(RK3576_EMMC_CRU_CLKSEL)));
              rk3576_emmc_resetlines(priv, EMMC_SWRESET_DAT);
              return OK;
            }

          return -EIO;
        }

      if (ret < 0)
        {
          rk3576_emmc_cancel(dev);
        }
    }

  hc2 = rk3576_emmc_getreg16(priv, RK3576_EMMC_HOSTCTRL2);
  hc2 &= ~(EMMC_HOSTCTRL2_EXEC_TUNING | EMMC_HOSTCTRL2_TUNED_CLK);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_HOSTCTRL2, hc2);
  priv->tuning_active = false;
  syslog(LOG_ERR, "ERROR: eMMC HS200 tuning exhausted ret=%d hostctrl2=%04x\n",
         ret, hc2);
  return ret < 0 ? ret : -EIO;
}

/****************************************************************************
 * Name: rk3576_emmc_hs400_enhanced_strobe
 ****************************************************************************/

static int rk3576_emmc_hs400_enhanced_strobe(struct sdio_dev_s *dev,
                                             bool enable)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;
  uint32_t value;

  if (enable && (!priv->dll_ready || priv->strobe_support == 0))
    {
      return -EIO;
    }

  value = rk3576_emmc_getreg32(priv, RK3576_EMMC_VENDOR_EMMCCTRL);
  value |= EMMC_VENDOR_CARD_IS_EMMC;
  if (enable)
    {
      value |= EMMC_VENDOR_ENHANCED_STROBE;
    }
  else
    {
      value &= ~EMMC_VENDOR_ENHANCED_STROBE;
    }

  rk3576_emmc_putreg32(priv, RK3576_EMMC_VENDOR_EMMCCTRL, value);
  mcinfo("eMMC HS400 enhanced strobe %s hostctrl2=%04x"
         " emmcctrl=%08" PRIx32 " txclk=%08" PRIx32 " cmdout=%08" PRIx32
         " strbin=%08" PRIx32 "\n",
         enable ? "enabled" : "disabled",
         rk3576_emmc_getreg16(priv, RK3576_EMMC_HOSTCTRL2), value,
         rk3576_emmc_getreg32(priv, RK3576_EMMC_VENDOR_DLLTXCLK),
         rk3576_emmc_getreg32(priv, RK3576_EMMC_VENDOR_DLLCMDOUT),
         rk3576_emmc_getreg32(priv, RK3576_EMMC_VENDOR_DLLSTRBIN));
  return OK;
}

#ifdef CONFIG_SDIO_DMA

/****************************************************************************
 * Name: rk3576_emmc_dma_disable
 ****************************************************************************/

static void rk3576_emmc_dma_disable(struct rk3576_emmc_dev_s *priv)
{
  uint8_t hc;

  hc = rk3576_emmc_getreg8(priv, RK3576_EMMC_HOSTCTRL1);
  hc &= ~(3 << EMMC_HOSTCTRL1_DMASEL_SHIFT);
  rk3576_emmc_putreg8(priv, RK3576_EMMC_HOSTCTRL1, hc);

  priv->dma_active = false;
  priv->dma_read = false;
  priv->dma_buffer = 0;
  priv->dma_length = 0;
  priv->dma_bounce_dest = NULL;
}

/****************************************************************************
 * Name: rk3576_emmc_dma_ok
 ****************************************************************************/

static bool rk3576_emmc_dma_ok(const uint8_t *buffer, size_t buflen)
{
  uintptr_t address = (uintptr_t)buffer;
  uintptr_t cursor = address;
  size_t remaining = buflen;
  size_t linesize = up_get_dcache_linesize();
  unsigned int ndescs = 0;

  if (linesize == 0)
    {
      linesize = 64;
    }

  if (buffer == NULL || buflen == 0 || (address & (linesize - 1)) != 0 ||
      (buflen & (linesize - 1)) != 0 || buflen > RK3576_EMMC_ADMA_MAXXFR ||
      (uint64_t)address + buflen > RK3576_EMMC_ADMA_LIMIT)
    {
      return false;
    }

  while (remaining > 0)
    {
      size_t boundary = RK3576_EMMC_ADMA_BOUNDARY -
                        (cursor & (RK3576_EMMC_ADMA_BOUNDARY - 1));
      size_t segment = remaining;

      if (segment > RK3576_EMMC_ADMA_BUFSZ)
        {
          segment = RK3576_EMMC_ADMA_BUFSZ;
        }

      if (segment > boundary)
        {
          segment = boundary;
        }

      if (++ndescs > RK3576_EMMC_ADMA_NDESC)
        {
          return false;
        }

      cursor += segment;
      remaining -= segment;
    }

  return true;
}

/****************************************************************************
 * Name: rk3576_emmc_dma_setup
 ****************************************************************************/

static int rk3576_emmc_dma_setup(struct rk3576_emmc_dev_s *priv,
                                 const uint8_t *buffer, size_t buflen,
                                 bool write)
{
  uintptr_t address = (uintptr_t)buffer;
  size_t remaining = buflen;
  uint32_t blksz;
  uint32_t nblocks;
  uint16_t mode;
  uint8_t hc;
  int index = 0;

  while (remaining > 0)
    {
      size_t boundary = RK3576_EMMC_ADMA_BOUNDARY -
                        (address & (RK3576_EMMC_ADMA_BOUNDARY - 1));
      size_t segment = remaining;
      uint16_t attr = EMMC_ADMA2_VALID | EMMC_ADMA2_ACT_TRAN;

      if (segment > RK3576_EMMC_ADMA_BUFSZ)
        {
          segment = RK3576_EMMC_ADMA_BUFSZ;
        }

      if (segment > boundary)
        {
          segment = boundary;
        }

      if (segment == remaining)
        {
          attr |= EMMC_ADMA2_END;
        }

      g_emmc_adma_descs[index].attr = attr;
      g_emmc_adma_descs[index].length =
          segment == RK3576_EMMC_ADMA_BUFSZ ? 0 : (uint16_t)segment;
      g_emmc_adma_descs[index].address = (uint32_t)address;

      address += segment;
      remaining -= segment;
      index++;
    }

  up_clean_dcache((uintptr_t)g_emmc_adma_descs,
                  (uintptr_t)&g_emmc_adma_descs[index]);

  if (write)
    {
      up_clean_dcache((uintptr_t)buffer, (uintptr_t)buffer + buflen);
    }
  else
    {
      up_invalidate_dcache((uintptr_t)buffer, (uintptr_t)buffer + buflen);
    }

  priv->buffer = NULL;
  priv->remaining = 0;
  priv->dma_active = true;
  priv->dma_read = !write;
  priv->dma_buffer = (uintptr_t)buffer;
  priv->dma_length = buflen;

  blksz = priv->blocksize ? priv->blocksize : buflen;
  nblocks = blksz ? buflen / blksz : 1;

  rk3576_emmc_putreg16(priv, RK3576_EMMC_NINTSTS,
                       EMMC_NINT_XFERDONE | EMMC_NINT_DMAINT);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_EINTSTS,
                       EMMC_EPART(EMMC_DATAERR_INTS));
  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKSIZE, (uint16_t)blksz);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKCOUNT, (uint16_t)nblocks);
  rk3576_emmc_putreg32(priv, RK3576_EMMC_ADMAADDR,
                       (uint32_t)(uintptr_t)g_emmc_adma_descs);

  hc = rk3576_emmc_getreg8(priv, RK3576_EMMC_HOSTCTRL1);
  hc &= ~(3 << EMMC_HOSTCTRL1_DMASEL_SHIFT);
  hc |= EMMC_HOSTCTRL1_DMA_ADMA2;
  rk3576_emmc_putreg8(priv, RK3576_EMMC_HOSTCTRL1, hc);

  mode = EMMC_XFERMODE_DMAEN;
  if (!write)
    {
      mode |= EMMC_XFERMODE_DTDSEL;
    }

  if (nblocks > 1)
    {
      mode |= EMMC_XFERMODE_BCEN | EMMC_XFERMODE_MSBSEL;
    }

  priv->xfermode = mode;
  rk3576_emmc_putreg16(priv, RK3576_EMMC_XFERMODE, mode);
  rk3576_emmc_configxfrints(priv, EMMC_XFRDONE_INTS | EMMC_DATAERR_INTS);
  return OK;
}

#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
/****************************************************************************
 * Name: rk3576_emmc_dmapreflight
 *
 * Description:
 *   Always accept the request here.  The generic MMC layer does not retry a
 *   failed DMA preflight through PIO, so the setup methods select ADMA2 or
 *   PIO themselves.
 ****************************************************************************/

static int rk3576_emmc_dmapreflight(struct sdio_dev_s *dev,
                                    const uint8_t *buffer, size_t buflen)
{
  UNUSED(dev);
  UNUSED(buffer);
  UNUSED(buflen);
  return OK;
}
#endif

/****************************************************************************
 * Name: rk3576_emmc_dmarecvsetup / rk3576_emmc_dmasendsetup
 *
 * Description:
 *   Select ADMA2 for cache-line-aligned low-4G buffers within descriptor
 *   capacity.  Unaligned reads use a DMA-safe bounce buffer when available;
 *   other requests transparently use the proven PIO path.
 ****************************************************************************/

static int rk3576_emmc_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                    size_t buflen)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;

  if (!rk3576_emmc_dma_ok(buffer, buflen))
    {
#ifdef CONFIG_RK3576_DMA_ALLOC
      if (g_emmc_dma_bounce != NULL &&
          rk3576_emmc_dma_ok(g_emmc_dma_bounce, buflen))
        {
          priv->dma_bounce_dest = buffer;
          return rk3576_emmc_dma_setup(priv, g_emmc_dma_bounce, buflen, false);
        }
#endif

      return rk3576_emmc_recvsetup(dev, buffer, buflen);
    }

  priv->dma_bounce_dest = NULL;
  return rk3576_emmc_dma_setup(priv, buffer, buflen, false);
}

static int rk3576_emmc_dmasendsetup(struct sdio_dev_s *dev,
                                    const uint8_t *buffer, size_t buflen)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;

  if (!rk3576_emmc_dma_ok(buffer, buflen))
    {
      return rk3576_emmc_sendsetup(dev, buffer, buflen);
    }

  return rk3576_emmc_dma_setup(priv, buffer, buflen, true);
}

#endif /* CONFIG_SDIO_DMA */

#ifdef CONFIG_SDIO_BLOCKSETUP
/****************************************************************************
 * Name: rk3576_emmc_blocksetup
 *
 * Description:
 *   Record the block size and preset BLOCKSIZE/BLOCKCOUNT.  The recv/send
 *   setup paths derive the block count from this recorded size.
 ****************************************************************************/

static void rk3576_emmc_blocksetup(struct sdio_dev_s *dev,
                                   unsigned int blocklen, unsigned int nblocks)
{
  struct rk3576_emmc_dev_s *priv = (struct rk3576_emmc_dev_s *)dev;

  priv->blocksize = blocklen;
  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKSIZE, (uint16_t)blocklen);
  rk3576_emmc_putreg16(priv, RK3576_EMMC_BLOCKCOUNT, (uint16_t)nblocks);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_emmc_initialize
 *
 * Description:
 *   Initialize the RK3576 eMMC host and return an sdio_dev_s for the mmcsd
 *   layer to bind and enumerate.
 *
 * Input Parameters:
 *   slotno - Host slot: must be RK3576_EMMC_SLOT (single host).
 *
 * Returned Value:
 *   On success returns an sdio_dev_s pointer, on failure returns NULL.
 ****************************************************************************/

struct sdio_dev_s *rk3576_emmc_initialize(int slotno)
{
  struct rk3576_emmc_dev_s *priv;

  DEBUGASSERT(slotno >= 0 && slotno < RK3576_EMMC_NHOSTS);

  priv = &g_emmc_hosts[slotno];
  priv->dev = g_rk3576_emmc_ops; /* copy the shared ops template */
  priv->base = g_emmc_cfg[slotno].base;
  priv->irq = g_emmc_cfg[slotno].irq;

#if defined(CONFIG_SDIO_DMA) && defined(CONFIG_RK3576_DMA_ALLOC)
  if (g_emmc_dma_bounce == NULL)
    {
      g_emmc_dma_bounce = rk3576_dma_alloc(RK3576_EMMC_ADMA_MAXXFR);
      if (g_emmc_dma_bounce == NULL)
        {
          mcwarn("WARNING: eMMC DMA bounce allocation failed\n");
        }
    }
#endif

  nxmutex_init(&priv->dev.mutex);
  nxsem_init(&priv->waitsem, 0, 0);
  wd_init(&priv->waitwdog);

  /* Reset the controller to a known state. */

  rk3576_emmc_reset(&priv->dev);

  mcinfo("RK3576 eMMC initialization complete base=%08" PRIxPTR " irq=%d\n",
         priv->base, priv->irq);

  return &priv->dev;
}

#endif /* CONFIG_RK3576_EMMC */
