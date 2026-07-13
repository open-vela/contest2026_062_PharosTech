/****************************************************************************
 * chips/rk3576/rk3576_cru.c
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
 * RK3576 Clock & Reset Unit (CRU) driver.
 *
 * Provides unified clock gating, source select, divider programming, and
 * soft-reset management for all RK3576 peripherals.  Consolidates CRU
 * register knowledge that was previously scattered across individual
 * peripheral drivers (see rk3576_i2c.c rk3576_i2c_clk_enable as the
 * original ad-hoc example).
 *
 * The driver does NOT manage the PLL tree (CPLL/GPLL/NPLL/… are assumed
 * to be configured by the bootloader or a future clock-framework
 * integration).  It also does NOT implement clock-rate voting or
 * reference-counting at this stage.
 *
 * Reference: Rockchip RK3576 TRM, Chapter 5 "Clock & Reset Unit".
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "rk3576_cru.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_cru_initialize
 ****************************************************************************/

int rk3576_cru_initialize(void)
{
  /* TODO: populate when the public API is fleshed out */

  return OK;
}
