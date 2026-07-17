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
 *   Program the clock source selection for a given I2C bus.  Each I2C
 *   controller's functional clock can be sourced from one of four options:
 *   GPLL divided by 6, CPLL divided by 10, CPLL divided by 20, or the
 *   XIN_OSC0 external oscillator.
 *
 *   The register write uses the "write-enable" mask (upper 16 bits) to
 *   ensure only the target selection field is modified atomically.
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
 *   I2C bus.  The 2-bit field is extracted from the appropriate CLKSEL_CON
 *   register and decoded into the corresponding rk3576_clock_source_t
 *   enumeration value.
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
 *   clock (clk) gate for the specified I2C bus.  On RK3576, writing 0 to
 *   the gate bit enables the clock, writing 1 disables it — the hardware
 *   uses active-low gating semantics.  The "write-enable" mask bit (offset
 *   + 16) is always set so that only the target bit is affected.
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
 *   a given I2C bus.  Because the hardware uses active-low gating (0 =
 *   enabled, 1 = disabled), the returned boolean values indicate the
 *   logical "enabled" state (true = clock running).
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

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CRU_H */
