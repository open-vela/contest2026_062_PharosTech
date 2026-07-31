/****************************************************************************
 * chips/rk3576/rk3576_sdmmc.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SDMMC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SDMMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sdio.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef CODE void (*rk3576_sdmmc_media_callback_t)(FAR void *arg,
                                                   bool inserted);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_sdmmc_initialize
 *
 * Description:
 *   Initialize the RK3576 SDMMC (SD card) host and return an sdio_dev_s for
 *   the mmcsd layer to mount.
 *
 * Input Parameters:
 *   slotno - Slot number (RK3576 has only one SD card slot, use 0).
 *
 * Returned Value:
 *   On success returns an sdio_dev_s pointer, on failure returns NULL.
 *
 ****************************************************************************/

FAR struct sdio_dev_s *rk3576_sdmmc_initialize(int slotno);

/****************************************************************************
 * Name: rk3576_sdmmc_register_media_callback
 *
 * Description:
 *   Register a board-level listener for SD card insertion and removal.  This
 *   listener is independent of the callback reserved by the generic mmcsd
 *   layer.  The callback runs in interrupt context and must only schedule
 *   deferred work.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 ****************************************************************************/

int rk3576_sdmmc_register_media_callback(
    FAR struct sdio_dev_s *dev, rk3576_sdmmc_media_callback_t callback,
    FAR void *arg);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SDMMC_H */
