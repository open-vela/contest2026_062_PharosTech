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

/* Channel selector for rk3576_saradc_initialize().  Each call initialises
 * exactly one channel as an independent adc_dev_s.  A channel may only be
 * initialised once; the driver performs a mutual-exclusion check and fails
 * a second initialisation of the same channel.
 */

enum rk3576_saradc_ch_e
{
  RK3576_SARADC_CH0 = 0,
  RK3576_SARADC_CH1,
  RK3576_SARADC_CH2,
  RK3576_SARADC_CH3,
  RK3576_SARADC_CH4,
  RK3576_SARADC_CH5,
  RK3576_SARADC_CH6,
  RK3576_SARADC_CH7
};

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
 *   Initialize exactly one SARADC channel as an independent adc_dev_s and
 *   return it for registration with adc_register().  All channels share a
 *   single SARADC controller core: the clock handles, the conversion clock
 *   rate (CONFIG_RK3576_SARADC_CLK_RATE, fixed at build time), and the CRU
 *   soft-reset are applied once when the first channel is initialised.  The
 *   controller registers are serialised with a mutex because the hardware
 *   converts channels one at a time.
 *
 *   A given channel may only be initialised once; a second call for the
 *   same channel returns NULL to avoid two adc_dev_s instances racing on
 *   the same channel's data register.
 *
 *   The board logic is responsible for muxing any analog input pins before
 *   conversion (the SARADC pads are dedicated analog inputs; no pinctrl is
 *   required, but external pin configuration may be board-specific).
 *
 * Input Parameters:
 *   channel - The channel to initialise (RK3576_SARADC_CH0 .. CH7).
 *
 * Returned Value:
 *   A pointer to the adc_dev_s on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct adc_dev_s *
rk3576_saradc_initialize(enum rk3576_saradc_ch_e channel);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_SARADC */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SARADC_H */
