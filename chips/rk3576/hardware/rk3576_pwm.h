/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_pwm.h
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
 * The RK3576 PWM is a Rockchip PWM v4 block: each channel is an independent
 * 0x1000 register window.  The SoC has three controllers:
 *
 *   PWM0 (0x27330000) – 2 channels, PMU1CRU domain
 *   PWM1 (0x2ADD0000) – 6 channels, CRU domain
 *   PWM2 (0x2ADE0000) – 8 channels, CRU domain
 *
 * Registers marked HIWORD are write-masked: the upper 16 bits are a
 * per-bit write-enable for the lower 16 bits.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PWM_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PWM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base + per-channel stride. */

#define RK3576_PWM_CH_STRIDE    0x1000

/* Number of channels per controller. */

#define RK3576_PWM_NSLOTS       8      /* Max channels (PWM2), for array sizing */

/* Controller enumeration (used as array index and rk3576_pwm_initialize param). */

#define RK3576_PWM0             0
#define RK3576_PWM1             1
#define RK3576_PWM2             2
#define RK3576_PWM_NCTRL        3      /* Total number of controllers         */

/* clk_osc_pwm1 root frequency (crystal, fixed). */

#define RK3576_PWM_OSC_HZ       24000000

/* Register offsets (per channel window) ********************************/

#define RK3576_PWM_VERSION      0x0000  /* Version ID (RO)               */
#define RK3576_PWM_ENABLE       0x0004  /* Enable (HIWORD)               */
#define RK3576_PWM_CLK_CTRL     0x0008  /* Clock control (HIWORD)        */
#define RK3576_PWM_CTRL         0x000c  /* Control (HIWORD)              */
#define RK3576_PWM_PERIOD       0x0010  /* Period, in clock cycles       */
#define RK3576_PWM_DUTY         0x0014  /* Duty, in clock cycles         */
#define RK3576_PWM_OFFSET       0x0018  /* Phase offset, in clock cycles */
#define RK3576_PWM_CNT          0x0024  /* Counter (RO, debug)           */

/* PWM_ENABLE (0x04) ***************************************************/

#define PWM_ENABLE_CLK_EN       (1 << 0)  /* PWM clock enable            */
#define PWM_ENABLE_EN           (1 << 1)  /* PWM enable                  */
#define PWM_ENABLE_CTRL_UPDATE  (1 << 2)  /* Latch CTRL/PERIOD/DUTY (W1T)*/
#define PWM_ENABLE_FORCE_CLK    (1 << 3)  /* Force clock always on       */
#define PWM_ENABLE_CNT_RD       (1 << 5)  /* Counter read enable (debug) */

/* PWM_CLK_CTRL (0x08) ************************************************/

#define PWM_CLK_PRESCALE_SHIFT  0         /* [2:0]  input /= 2^prescale  */
#define PWM_CLK_SCALE_SHIFT     4         /* [12:4] then /= 2*scale      */
#define PWM_CLK_SRC_SEL_SHIFT   13        /* [14:13] 0=PLL 1=osc 2=RC    */
#define PWM_CLK_SRC_OSC         1

/* PWM_CTRL (0x0c) ***************************************************/

#define PWM_CTRL_MODE_SHIFT     0         /* [1:0]                       */
#define PWM_CTRL_MODE_ONESHOT   0
#define PWM_CTRL_MODE_CONTINUOUS 1
#define PWM_CTRL_DUTY_POL       (1 << 2)  /* duty-cycle output polarity  */
#define PWM_CTRL_INACTIVE_POL   (1 << 3)  /* inactive output polarity    */

/* HIWORD write helper: set the given low-16 bits with their write mask. */

#define PWM_HIWORD(bits)        (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define PWM_HIWORD_CLR(bits)    ((uint32_t)(bits) << 16)

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PWM_H */
