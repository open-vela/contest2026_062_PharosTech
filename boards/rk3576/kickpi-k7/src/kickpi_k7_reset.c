/****************************************************************************
 * boards/arm64/rk3576/kickpi-k7/src/kickpi_k7_reset.c
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

#include <nuttx/config.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>

#ifdef CONFIG_BOARDCTL_RESET

/****************************************************************************
 * Public functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_reset
 *
 * Description:
 *   Reset the board.  Provided by the board logic when CONFIG_BOARDCTL_RESET
 *   is enabled.  On the RK3576 this goes through PSCI (up_systemreset() is
 *   implemented in arch/arm64 arm64_cpu_psci.c), triggering a warm system
 *   restart that re-boots from the MiniLoader/BL33 so new firmware takes
 *   effect.
 *
 * Input Parameters:
 *   status - Status information carried by the reset event, board-defined;
 *            pass 0 when unused.
 *
 * Returned Value:
 *   If this function returns, the reset failed; the return value is a
 *   board-specific failure reason.
 *
 ****************************************************************************/

int board_reset(int status)
{
  up_systemreset();
  return 0;
}

#endif /* CONFIG_BOARDCTL_RESET */
