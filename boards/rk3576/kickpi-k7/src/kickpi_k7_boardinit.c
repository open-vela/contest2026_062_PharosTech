/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_boardinit.c
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
 * Included Files
 ****************************************************************************/

#include "kickpi_k7.h"
#include "rk3576_clk_tree.h"
#include "rk3576_gpio.h"
#include <nuttx/board.h>
#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <syslog.h>

#ifdef CONFIG_RK3576_SDMMC
#include "rk3576_sdmmc.h"
#include <nuttx/mmcsd.h>
#endif

#ifdef CONFIG_RK3576_EMMC
#include "rk3576_emmc.h"
#include <nuttx/mmcsd.h>
#endif

#if defined(CONFIG_RK3576_SDMMC) && defined(CONFIG_GPT_PARTITION)
#include <nuttx/fs/partition.h>
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#if defined(CONFIG_RK3576_SDMMC) && defined(CONFIG_GPT_PARTITION)

#define KICKPI_K7_SDMMC_DEVNAME "/dev/mmcsd0"

/****************************************************************************
 * Name: kickpi_k7_partition_handler
 *
 * Description:
 *   Called by parse_block_partition() for each valid GPT partition on
 *   the sdmmc device.  The GPT layout (see build_sd.sh) is
 *   index 0 = uboot (BL33), index 1 = trust, index 2 = rootfs.
 ****************************************************************************/

static void kickpi_k7_partition_handler(FAR struct partition_s *part,
                                        FAR void *arg)
{
  char devname[32];

  /* TODO: Match partitions by name (part->name) instead of index.
   *       For example: if (strcmp(part->name, "rootfs") != 0) return;
   *       This avoids mis-registration when GPT layout changes and
   *       prevents exposing boot/trust partitions as writable (0660).
   */

  if (part->index < 9)
    {
      sprintf(devname, KICKPI_K7_SDMMC_DEVNAME "p%lu", part->index + 1);
      register_blockpartition(devname, 0660, KICKPI_K7_SDMMC_DEVNAME,
                              part->firstblock, part->nblocks);
      syslog(LOG_INFO, "INFO: partition %s firstblock=%lu nblocks=%lu\n",
             devname, (unsigned long)part->firstblock,
             (unsigned long)part->nblocks);
    }
}

#endif /* CONFIG_RK3576_SDMMC && CONFIG_GPT_PARTITION */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_memory_initialize
 *
 * Description:
 *   All RK3576 architectures must provide the following entry point.  This
 *   entry point is called early in the initialization before memory has
 *   been configured.  This board-specific function is responsible for
 *   configuring any on-board memories.
 *
 *   Logic in rk3576_memory_initialize must be careful to avoid using any
 *   global variables because those will be uninitialized at the time this
 *   function is called.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_memory_initialize(void)
{
  /* SDRAM was already init by bootloader in supported configuration */
}

/****************************************************************************
 * Name: rk3576_board_initialize
 *
 * Description:
 *   All RK3576 architectures must provide the following entry point.  This
 *   entry point is called in the initialization phase -- after
 *   rk3576_memory_initialize and after all memory has been configured and
 *   mapped but before any devices have been initialized.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_board_initialize(void)
{
#ifdef CONFIG_ARCH_LEDS
  /* Configure on-board LEDs if LED support has been selected. */

  /* board_autoled_initialize(); */
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   If CONFIG_BOARD_LATE_INITIALIZE is selected, then an additional
 *   initialization call will be performed in the boot-up sequence to a
 *   function called board_late_initialize(). board_late_initialize() will be
 *   called immediately after up_initialize() is called and just before the
 *   initial application is started.  This additional initialization phase
 *   may be used, for example, to initialize board-specific device drivers.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
  /* Register the RK3576 clock tree with the NuttX CLK framework.
   * Must be done before any peripheral driver calls clk_get().
   * This cannot run in arm64_chip_boot() because clk_register()
   * depends on kmm_zalloc(), which requires the kernel heap to
   * be initialized (done in nx_start() -> memory_initialize()).
   */

  rk3576_clk_tree_initialize();

  /* Perform board initialization */

#ifdef CONFIG_DEV_GPIO
  /* setup gpio driver */
  rk3576_gpio_init();

  /* register LED GPIO pin */
  rk3576_gpio_register(GPIO_PORT0 | GPIO_PIN_B4 | GPIO_OUTPUT);
#endif

#ifdef CONFIG_RK3576_SDMMC
  /* Initialize the SD card slot (SDMMC0).  The SD card is an
   * optional peripheral: on failure only warn, do not block the boot (booting
   * to NSH must succeed even with no card inserted).
   */

  {
    FAR struct sdio_dev_s *sdmmc = rk3576_sdmmc_initialize(0);
    if (sdmmc == NULL)
      {
        syslog(LOG_ERR, "ERROR: rk3576_sdmmc_initialize failed\n");
      }
    else if (mmcsd_slotinitialize(0, sdmmc) < 0)
      {
        syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize failed\n");
      }
  }

#ifdef CONFIG_GPT_PARTITION
  /* Parse the GPT and register each partition as /dev/mmcsd0pN.  The rootfs
   * FAT is mounted explicitly (e.g. on /data) when needed, not here.
   */

  {
    int ret = parse_block_partition(KICKPI_K7_SDMMC_DEVNAME,
                                    kickpi_k7_partition_handler, NULL);
    if (ret < 0)
      {
        syslog(LOG_WARNING,
               "WARNING: parse " KICKPI_K7_SDMMC_DEVNAME " GPT failed: %d\n",
               ret);
      }
  }
#endif
#endif

#ifdef CONFIG_RK3576_EMMC
  /* Initialize the on-board eMMC (dwcmshc / SDHCI) as /dev/mmcsd1. */

  {
    FAR struct sdio_dev_s *emmc = rk3576_emmc_initialize(0);
    if (emmc == NULL)
      {
        syslog(LOG_ERR, "ERROR: rk3576_emmc_initialize failed\n");
      }
    else if (mmcsd_slotinitialize(1, emmc) < 0)
      {
        syslog(LOG_ERR, "ERROR: eMMC mmcsd_slotinitialize failed\n");
      }
  }
#endif

#ifdef CONFIG_KICKPI_K7_AUDIO
  /* Register the on-board ES8388 PCM device before the initial application
   * starts.  The initializer is idempotent, so board utilities may call it
   * again safely.
   */

  {
    int ret = kickpi_k7_audio_initialize();
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: kickpi_k7_audio_initialize failed: %d\n",
               ret);
      }
  }
#endif
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */
