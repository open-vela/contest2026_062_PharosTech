/****************************************************************************
 * chips/rk3576/rk3576_emmc.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_EMMC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_EMMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sdio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Host slot for rk3576_emmc_initialize().  The RK3576 has a single eMMC
 * host (mmc@2a330000, a Synopsys dwcmshc / SDHCI 3.0 controller).
 */

#define RK3576_EMMC_SLOT 0 /* On-board eMMC (dwcmshc, mmc@2a330000) */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_emmc_initialize
 *
 * Description:
 *   Initialize the RK3576 eMMC host and return an sdio_dev_s that the mmcsd
 *   layer can bind and enumerate.
 *
 * Input Parameters:
 *   slotno - Host slot: must be RK3576_EMMC_SLOT (single host).
 *
 * Returned Value:
 *   On success returns an sdio_dev_s pointer, on failure returns NULL.
 *
 ****************************************************************************/

struct sdio_dev_s *rk3576_emmc_initialize(int slotno);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_EMMC_H */
