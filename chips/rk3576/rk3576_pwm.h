/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pwm.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_PWM_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_PWM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/timers/pwm.h>

#ifdef CONFIG_RK3576_PWM

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pwm_initialize
 *
 * Description:
 *   Return the lower-half handle for one PWM1 channel (0..5) for the board
 *   to register with pwm_register().  The board must mux the channel's
 *   output pin before the device is used.
 *
 * Input Parameters:
 *   channel - PWM1 channel index (0..5)
 *
 * Returned Value:
 *   A pwm_lowerhalf_s handle on success; NULL on an invalid channel.
 *
 ****************************************************************************/

struct pwm_lowerhalf_s *rk3576_pwm_initialize(int channel);

#endif /* CONFIG_RK3576_PWM */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_PWM_H */
