/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/src/kickpi_k7.h
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

#ifndef __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H
#define __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#ifndef __ASSEMBLY__

struct sdio_dev_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_KICKPI_K7_STORAGE_AUTOMOUNT
int kickpi_k7_storage_initialize(FAR struct sdio_dev_s *sdmmc,
                                 FAR struct sdio_dev_s *emmc);
#endif

#ifdef CONFIG_KICKPI_K7_USBHOST
int kickpi_k7_usbhost_initialize(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H */
