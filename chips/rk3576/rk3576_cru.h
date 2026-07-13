/****************************************************************************
 * chips/rk3576/rk3576_cru.h
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
 * RK3576 Clock & Reset Unit (CRU) driver public API.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CRU_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CRU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef enum
{
  RK3576_CLOCK_SOURCE_XIN_OSC0_FUNC = 0x000,

  RK3576_CLOCK_SOURCE_GPLL_MUX = 0x100,
  RK3576_CLOCK_SOURCE_GPLL_DIV2_SRC,
  RK3576_CLOCK_SOURCE_GPLL_DIV3_SRC,
  RK3576_CLOCK_SOURCE_GPLL_DIV4_SRC,
  RK3576_CLOCK_SOURCE_GPLL_DIV6_SRC,
  RK3576_CLOCK_SOURCE_GPLL_DIV8_SRC,

  RK3576_CLOCK_SOURCE_CPLL_MUX = 0x200,
  RK3576_CLOCK_SOURCE_CPLL_DIV2_SRC,
  RK3576_CLOCK_SOURCE_CPLL_DIV4_SRC,
  RK3576_CLOCK_SOURCE_CPLL_DIV10_SRC,
  RK3576_CLOCK_SOURCE_CPLL_DIV20_SRC,

  /* TODO: add more when needed */

} rk3576_clock_source_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_cru_set_i2c_clock_selection
 *
 * Description:
 *   Program the clock source selection for a given I2C bus.
 *
 * Input Parameters:
 *   i2c_bus_id - I2C bus index (0 ~ 9).
 *   sel        - Desired clock source, one of:
 *                RK3576_CLOCK_SOURCE_GPLL_DIV6_SRC  (00b)
 *                RK3576_CLOCK_SOURCE_CPLL_DIV10_SRC (01b)
 *                RK3576_CLOCK_SOURCE_CPLL_DIV20_SRC (10b)
 *                RK3576_CLOCK_SOURCE_XIN_OSC0_FUNC  (11b)
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_cru_set_i2c_clock_selection(uint16_t i2c_bus_id,
                                       rk3576_clock_source_t sel);

/****************************************************************************
 * Name: rk3576_cru_get_i2c_clock_selection
 *
 * Description:
 *   Read back the currently configured clock source selection for a given
 *   I2C bus.
 *
 * Input Parameters:
 *   i2c_bus_id - I2C bus index (0 ~ 9).
 *   p_sel      - [out] Receives the current clock source selection.
 *                May be NULL.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_cru_get_i2c_clock_selection(uint16_t i2c_bus_id,
                                       rk3576_clock_source_t *p_sel);

/****************************************************************************
 * Name: rk3576_cru_set_i2c_clock_gate
 *
 * Description:
 *   Enable or disable the I2C peripheral clock (pclk) and/or functional
 *   clock (clk) gate for the specified I2C bus.
 *
 * Input Parameters:
 *   i2c_bus_id - I2C bus index (e.g. 0 for I2C0, 1 for I2C1, …).
 *   pclk_en    - true  → enable pclk (APB bus clock gate open).
 *                false → disable (gate) pclk.
 *   clk_en     - true  → enable SCL functional clock gate.
 *                false → disable (gate) the functional clock.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_cru_set_i2c_clock_gate(uint16_t i2c_bus_id, bool pclk_en,
                                  bool clk_en);

/****************************************************************************
 * Name: rk3576_cru_get_i2c_clock_gate
 *
 * Description:
 *   Query the current gate state of the I2C pclk and functional clock for
 *   a given I2C bus.
 *
 * Input Parameters:
 *   i2c_bus_id  - I2C bus index (e.g. 0 for I2C0, 1 for I2C1, …).
 *   p_pclk_en   - [out] Receives true if pclk is currently enabled,
 *                 false if gated.  May be NULL if not needed.
 *   p_clk_en    - [out] Receives true if the functional clock is currently
 *                 enabled, false if gated.  May be NULL if not needed.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_cru_get_i2c_clock_gate(uint16_t i2c_bus_id, bool *p_pclk_en,
                                  bool *p_clk_en);

/****************************************************************************
 * Name: rk3576_cru_set_pwm_clock_gate
 *
 * Description:
 *   Enable or disable the PWM peripheral clocks (pclk, clk_pwm,
 *   clk_pwm_rc, clk_pwm_osc) for the specified PWM controller.
 *
 *   - pclk:          APB bus interface clock. Must be enabled for any
 *                    register access to the PWM controller.
 *   - clk_pwm:       Primary PWM functional clock, used to generate the
 *                    PWM output waveform.
 *   - clk_pwm_rc:    Alternative clock source from the internal RC
 *                    oscillator.  Can be used as a low-power or fallback
 *                    clock for PWM output timing.
 *   - clk_pwm_osc:   Alternative clock source from an oscillator
 *                    Can be used as a low-power or fallback
 *                    clock for PWM output timing.
 *
 *   Typically, the caller enables pclk + one of {clk_pwm, clk_pwm_rc,
 *   clk_pwm_osc} depending on the desired PWM clock source.
 *
 * Input Parameters:
 *   pwm_controller_id - PWM controller index (0 ~ 2).
 *   pclk_en           - true → enable pclk (APB bus clock gate open).
 *                       false → disable (gate) pclk.
 *   clk_pwm_en        - true → enable primary PWM functional clock.
 *                       false → disable (gate) clk_pwm.
 *   clk_pwm_rc_en     - true → enable RC oscillator alternative clock.
 *                       false → disable (gate) clk_pwm_rc.
 *   clk_pwm_osc_en    - true → enable external oscillator alternative
 *                       clock.  false → disable (gate) clk_pwm_osc.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_cru_set_pwm_clock_gate(uint16_t pwm_controller_id, bool pclk_en,
                                  bool clk_pwm_en, bool clk_pwm_rc_en,
                                  bool clk_pwm_osc_en);

/****************************************************************************
 * Name: rk3576_cru_get_pwm_clock_gate
 *
 * Description:
 *   Query the current gate state of the PWM peripheral clocks (pclk,
 *   clk_pwm, clk_pwm_rc, clk_pwm_osc) for the specified PWM controller.
 *
 * Input Parameters:
 *   pwm_controller_id - PWM controller index (0 ~ 2).
 *   p_pclk_en         - [out] Receives true if pclk is currently enabled,
 *                       false if gated.  May be NULL.
 *   p_clk_pwm_en      - [out] Receives true if the primary PWM functional
 *                       clock is currently enabled, false if gated.
 *                       May be NULL.
 *   p_clk_pwm_rc_en   - [out] Receives true if the RC oscillator
 *                       alternative clock is currently enabled, false if
 *                       gated.  May be NULL.
 *   p_clk_pwm_osc_en  - [out] Receives true if the external oscillator
 *                       alternative clock is currently enabled, false if
 *                       gated.  May be NULL.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_cru_get_pwm_clock_gate(uint16_t pwm_controller_id, bool *p_pclk_en,
                                  bool *p_clk_pwm_en, bool *p_clk_pwm_rc_en,
                                  bool *p_clk_pwm_osc_en);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CRU_H */
