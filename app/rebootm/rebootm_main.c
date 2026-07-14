/****************************************************************************
 * apps/examples/rebootm/rebootm_main.c
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
 * Reboot the RK3576 into a Rockchip boot mode.  Writes the boot-mode magic
 * into the PMU0_GRF OS register (the same protocol the vendor kernels use,
 * consumed by the boot loader on the next boot), then resets via boardctl.
 *
 *   rebootm loader   - reboot into the loader rockusb download mode (the
 *                      USB flashing path, replaces serial Ymodem)
 *   rebootm normal   - plain reboot (clears any stale mode magic first)
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/boardctl.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PMU0_GRF OS register holding the reboot mode magic (base 0x26024000,
 * offset 0x40, from the vendor device tree syscon-reboot-mode node).
 */

#define REBOOTM_PMU0_GRF_OS_REG  0x26024040ul

/* Rockchip boot mode magics ("RK" 0x5242C3xx, vendor device tree). */

#define REBOOTM_MODE_NORMAL      0x5242c300u
#define REBOOTM_MODE_LOADER      0x5242c301u

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  volatile uint32_t *osreg = (volatile uint32_t *)REBOOTM_PMU0_GRF_OS_REG;
  uint32_t mode;

  if (argc == 2 && strcmp(argv[1], "loader") == 0)
    {
      mode = REBOOTM_MODE_LOADER;
    }
  else if (argc == 2 && strcmp(argv[1], "normal") == 0)
    {
      mode = REBOOTM_MODE_NORMAL;
    }
  else
    {
      printf("usage: rebootm loader|normal\n"
             "  loader: reboot into rockusb USB download mode\n"
             "  normal: plain reboot\n");
      return 1;
    }

  *osreg = mode;
  printf("rebootm: boot mode 0x%08lx written (readback 0x%08lx), "
         "rebooting ...\n", (unsigned long)mode, (unsigned long)*osreg);
  fflush(stdout);

#ifdef CONFIG_BOARDCTL_RESET
  boardctl(BOARDIOC_RESET, 0);
#else
  printf("rebootm: BOARDCTL_RESET not enabled, reset manually\n");
#endif
  return 0;
}
