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

#ifdef CONFIG_RK3576_SARADC

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
 *   The board logic is responsible for muxing any analog input pins before
 *   conversion (the SARADC pads are dedicated analog inputs; no pinctrl is
 *   required, but external pin configuration may be board-specific).
 *
 * Input Parameters:
 *   None (RK3576 has a single SARADC controller).
 *
 * Returned Value:
 *   A pointer to the adc_dev_s on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct adc_dev_s *rk3576_saradc_initialize(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_SARADC */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SARADC_H */
