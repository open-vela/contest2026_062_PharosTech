/****************************************************************************
 * chips/rk3576/rk3576_saradc.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SARADC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SARADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/analog/adc.h>
#include <nuttx/config.h>

#include <stdint.h>

#ifdef CONFIG_RK3576_SARADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Per-channel enable bits for rk3576_saradc_initialize()'s channel_mask
 * parameter.  Combine channels with bitwise-OR, e.g.:
 *   SARADC_CH0 | SARADC_CH2 | SARADC_CH5
 * to convert exactly those channels on each trigger.  SARADC_CH_ALL enables
 * every channel.
 */

#define RK3576_SARADC_CH(n)  (1u << (n))

#define RK3576_SARADC_CH0    RK3576_SARADC_CH(0)
#define RK3576_SARADC_CH1    RK3576_SARADC_CH(1)
#define RK3576_SARADC_CH2    RK3576_SARADC_CH(2)
#define RK3576_SARADC_CH3    RK3576_SARADC_CH(3)
#define RK3576_SARADC_CH4    RK3576_SARADC_CH(4)
#define RK3576_SARADC_CH5    RK3576_SARADC_CH(5)
#define RK3576_SARADC_CH6    RK3576_SARADC_CH(6)
#define RK3576_SARADC_CH7    RK3576_SARADC_CH(7)

#define RK3576_SARADC_CH_ALL 0xffu

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_saradc_initialize
 *
 * Description:
 *   Initialize the RK3576 SAR-ADC controller, enable its clocks through
 *   the NuttX CLK framework (clk_saradc / pclk_saradc), and return a
 *   bound lower-half adc_dev_s for registration with adc_register().
 *
 *   The set of channels to convert on each ANIOC_TRIGGER and the conversion
 *   clock rate are fixed at initialization time; only the enabled channels
 *   are converted, so unused channels cost no CPU time.
 *
 *   The board logic is responsible for muxing any analog input pins before
 *   conversion (the SARADC pads are dedicated analog inputs; no pinctrl is
 *   required, but external pin configuration may be board-specific).
 *
 * Input Parameters:
 *   channel_mask - 8-bit mask of channels to enable (bit 0 = channel 0,
 *                  ... bit 7 = channel 7).
 *   clk_rate     - Desired conversion clock rate in Hz; clamped to the
 *                  TRM Chapter 18 limit of [1, 20000000] Hz.
 *
 * Returned Value:
 *   A pointer to the adc_dev_s on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct adc_dev_s *rk3576_saradc_initialize(uint8_t channel_mask,
                                               uint32_t clk_rate);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_SARADC */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SARADC_H */
