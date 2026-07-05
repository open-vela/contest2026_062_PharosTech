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
 *   复位开发板。CONFIG_BOARDCTL_RESET 打开时由板级逻辑提供。
 *   RK3576 走 PSCI(up_systemreset 在 arch/arm64 的 arm64_cpu_psci.c 实现),
 *   触发系统热重启,重新从 MiniLoader/BL33 引导,使新固件生效。
 *
 * Input Parameters:
 *   status - 复位事件携带的状态信息,板级自定义,未用时传 0。
 *
 * Returned Value:
 *   若函数返回,说明未能复位;返回值为板级特定的失败原因。
 *
 ****************************************************************************/

int board_reset(int status)
{
  up_systemreset();
  return 0;
}

#endif /* CONFIG_BOARDCTL_RESET */
