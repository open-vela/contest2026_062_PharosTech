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

#include <debug.h>
#include <errno.h>
#include <nuttx/config.h>
#include <stdint.h>

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
 * Name: _get_i2c_clock_gate_register
 *
 * Description:
 *   Resolve the CRU gate register address and bit-offset for a given I2C
 *   bus.  Each I2C controller has two independently-gated clocks:
 *   - pclk:  APB bus interface clock (register access).
 *   - clk:   SCL functional clock (I2C bus timing).
 *
 *   Most I2C buses (1-9) live in the main CRU address space with pclk in
 *   GATE_CON(12) and clk split across GATE_CON(12)/GATE_CON(13).  I2C0 is
 *   the exception — its clock gates reside in the PMU1 CRU power domain
 *   (PMU1CRU_GATE_CON(5)).
 *
 * Input Parameters:
 *   i2c_bus_id    - I2C bus index (0 ~ 9).  Out-of-range returns -EINVAL.
 *   p_pclk_reg    - [out] Absolute address of the pclk gate register.
 *                   May be NULL.
 *   p_clk_reg     - [out] Absolute address of the clk gate register.
 *                   May be NULL.
 *   p_pclk_offset - [out] Bit position of the pclk gate within the
 *                   register.  May be NULL.
 *   p_clk_offset  - [out] Bit position of the clk gate within the
 *                   register.  May be NULL.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL if the bus ID is not recognized.
 *
 ****************************************************************************/

static int _get_i2c_clock_gate_register(uint16_t i2c_bus_id,
                                        unsigned long *p_pclk_reg,
                                        unsigned long *p_clk_reg,
                                        uint8_t *p_pclk_offset,
                                        uint8_t *p_clk_offset)
{
  uint8_t clk_offset, pclk_offset;
  unsigned long pclk_reg_offset = RK3576_CRU_GATE_CON(12);
  unsigned long clk_reg_offset = RK3576_CRU_GATE_CON(12);
  unsigned long base = RK3576_CRU_ADDR;

  switch (i2c_bus_id)
    {
      case 0:
        /* unlike others,
         * i2c0 clock register is in PMU1 CRU
         */
        base = RK3576_PMU1_CRU_ADDR;
        pclk_reg_offset = RK3576_PMU1CRU_GATE_CON(5);
        clk_reg_offset = RK3576_PMU1CRU_GATE_CON(5);
        pclk_offset = 1;
        clk_offset = 2;
        break;
      case 1:
        pclk_offset = 0;
        clk_offset = 12;
        break;
      case 2:
        pclk_offset = 1;
        clk_offset = 13;
        break;
      case 3:
        pclk_offset = 2;
        clk_offset = 14;
        break;
      case 4:
        pclk_offset = 3;
        clk_offset = 15;
        break;
      case 5:
        clk_reg_offset = RK3576_CRU_GATE_CON(13);
        pclk_offset = 4;
        clk_offset = 0;
        break;
      case 6:
        clk_reg_offset = RK3576_CRU_GATE_CON(13);
        pclk_offset = 5;
        clk_offset = 1;
        break;
      case 7:
        clk_reg_offset = RK3576_CRU_GATE_CON(13);
        pclk_offset = 6;
        clk_offset = 2;
        break;
      case 8:
        clk_reg_offset = RK3576_CRU_GATE_CON(13);
        pclk_offset = 7;
        clk_offset = 3;
        break;
      case 9:
        clk_reg_offset = RK3576_CRU_GATE_CON(13);
        pclk_offset = 8;
        clk_offset = 4;
        break;
      default:
        _err("CRU: Invalid i2c bus id %u", i2c_bus_id);
        return -EINVAL;
    }

  if (p_pclk_reg)
    {
      *p_pclk_reg = pclk_reg_offset + base;
    }

  if (p_clk_reg)
    {
      *p_clk_reg = clk_reg_offset + base;
    }

  if (p_pclk_offset)
    {
      *p_pclk_offset = pclk_offset;
    }

  if (p_clk_offset)
    {
      *p_clk_offset = clk_offset;
    }

  return OK;
}

/****************************************************************************
 * Name: _get_i2c_clock_sel_register
 *
 * Description:
 *   Resolve the CRU clock source selection register address and bit-offset
 *   for a given I2C bus.  Each I2C controller has a 2-bit field in the
 *   CLKSEL_CON registers that selects among four possible clock sources
 *   (GPLL/6, CPLL/10, CPLL/20, or XIN_OSC0).
 *
 *   Most I2C buses (1-8) use CLKSEL_CON(57) with consecutive 2-bit slots.
 *   I2C0 lives in the PMU1 CRU power domain (PMU1CRU_CLKSEL_CON(6)).
 *   I2C9 uses CLKSEL_CON(58).
 *
 * Input Parameters:
 *   i2c_bus_id  - I2C bus index (0 ~ 9).  Out-of-range returns -EINVAL.
 *   p_sel_reg   - [out] Absolute address of the clock selection register.
 *                 May be NULL.
 *   p_sel_offset - [out] Bit position of the 2-bit selector field within
 *                 the register.  May be NULL.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL if the bus ID is not recognized.
 *
 ****************************************************************************/

static int _get_i2c_clock_sel_register(uint16_t i2c_bus_id,
                                       unsigned long *p_sel_reg,
                                       uint8_t *p_sel_offset)
{
  uint8_t sel_offset;
  unsigned long sel_reg_offset = RK3576_CRU_CLKSEL_CON(57);
  unsigned long base = RK3576_CRU_ADDR;

  switch (i2c_bus_id)
    {
      case 0:
        base = RK3576_PMU1_CRU_ADDR;
        sel_reg_offset = RK3576_PMU1CRU_CLKSEL_CON(6);
        sel_offset = 7;
        break;
      case 1:
        sel_offset = 0;
        break;
      case 2:
        sel_offset = 2;
        break;
      case 3:
        sel_offset = 4;
        break;
      case 4:
        sel_offset = 6;
        break;
      case 5:
        sel_offset = 8;
        break;
      case 6:
        sel_offset = 10;
        break;
      case 7:
        sel_offset = 12;
        break;
      case 8:
        sel_offset = 14;
        break;
      case 9:
        sel_reg_offset = RK3576_CRU_CLKSEL_CON(58);
        sel_offset = 0;
        break;
      default:
        _err("CRU: Invalid i2c bus id %u", i2c_bus_id);
        return -EINVAL;
    }

  if (p_sel_reg)
    {
      *p_sel_reg = sel_reg_offset + base;
    }

  if (p_sel_offset)
    {
      *p_sel_offset = sel_offset;
    }

  return OK;
}

/****************************************************************************
 * Name: _get_pwm_clock_gate_reg
 *
 * Description:
 *   Resolve the CRU gate register address and bit-offsets for a given PWM
 *   controller.  Each PWM controller has up to four independently-gated
 *   clocks:
 *   - pclk:       APB bus interface clock (register access).
 *   - clk:        Primary PWM functional clock (clk_pwm).
 *   - osc_clk:    Alternative clock source from external oscillator
 *                 (clk_pwm_osc), used as a low-power or fallback clock
 *                 for PWM output.
 *   - rc_clk:     Alternative clock source from internal RC oscillator
 *                 (clk_pwm_rc), used as a low-power or fallback clock
 *                 for PWM output.
 *
 *   PWM0 lives in the PMU1 CRU power domain (PMU1CRU_GATE_CON(4)/(5)).
 *   PWM1 uses GATE_CON(16), PWM2 uses GATE_CON(20).  For PWM1/2 the RC
 *   clock gate shares the same register as the other clocks.
 *
 * Input Parameters:
 *   pwm_controller_id - PWM controller index (0 ~ 2).  Out-of-range
 *                       returns -EINVAL.
 *   p_reg             - [out] Absolute address of the gate register
 *                       (pclk/clk/osc).  May be NULL.
 *   p_rc_reg          - [out] Absolute address of the RC clock gate
 *                       register.  May be NULL.
 *   p_pclk_offset     - [out] Bit position of pclk gate.  May be NULL.
 *   p_clk_offset      - [out] Bit position of clk_pwm gate.  May be NULL.
 *   p_osc_clk_offset  - [out] Bit position of clk_pwm_osc gate.
 *                       May be NULL.
 *   p_rc_clk_offset   - [out] Bit position of clk_pwm_rc gate.
 *                       May be NULL.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL if the PWM controller ID is not recognized.
 *
 ****************************************************************************/

static int
_get_pwm_clock_gate_reg(uint16_t pwm_controller_id, unsigned long *p_reg,
                        unsigned long *p_rc_reg, uint8_t *p_pclk_offset,
                        uint8_t *p_clk_offset, uint8_t *p_osc_clk_offset,
                        uint8_t *p_rc_clk_offset)
{
  unsigned long base = RK3576_CRU_ADDR;
  unsigned long reg_offset, rc_reg_offset;
  uint8_t pclk_offset, clk_offset;
  uint8_t osc_clk_offset, rc_clk_offset;

  switch (pwm_controller_id)
    {
      case 0:
        base = RK3576_PMU1_CRU_ADDR;
        reg_offset = RK3576_PMU1CRU_GATE_CON(4);
        pclk_offset = 11;
        clk_offset = 12;
        osc_clk_offset = 13;
        rc_reg_offset = RK3576_PMU1CRU_GATE_CON(5);
        rc_clk_offset = 7;
        break;
      case 1:
        reg_offset = RK3576_CRU_GATE_CON(16);
        rc_reg_offset = reg_offset;
        pclk_offset = 10;
        clk_offset = 11;
        osc_clk_offset = 13;
        rc_clk_offset = 15;
        break;
      case 2:
        reg_offset = RK3576_CRU_GATE_CON(20);
        rc_reg_offset = reg_offset;
        pclk_offset = 4;
        clk_offset = 5;
        osc_clk_offset = 7;
        rc_clk_offset = 6;
        break;
      default:
        _err("CRU: Invalid pwm controller id %u", pwm_controller_id);
        return -EINVAL;
    }

  if (p_reg)
    {
      *p_reg = reg_offset + base;
    }

  if (p_rc_reg)
    {
      *p_rc_reg = rc_reg_offset + base;
    }

  if (p_pclk_offset)
    {
      *p_pclk_offset = pclk_offset;
    }

  if (p_clk_offset)
    {
      *p_clk_offset = clk_offset;
    }

  if (p_osc_clk_offset)
    {
      *p_osc_clk_offset = osc_clk_offset;
    }

  if (p_rc_clk_offset)
    {
      *p_rc_clk_offset = rc_clk_offset;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

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
                                       rk3576_clock_source_t sel)
{
  unsigned long sel_reg;
  uint8_t sel_offset;
  uint32_t sel_bits;

  int ret = _get_i2c_clock_sel_register(i2c_bus_id, &sel_reg, &sel_offset);

  if (ret < 0)
    {
      return ret;
    }

  switch (sel)
    {
      case RK3576_CLOCK_SOURCE_GPLL_DIV6_SRC:
        sel_bits = 0b00;
        break;
      case RK3576_CLOCK_SOURCE_CPLL_DIV10_SRC:
        sel_bits = 0b01;
        break;
      case RK3576_CLOCK_SOURCE_CPLL_DIV20_SRC:
        sel_bits = 0b10;
        break;
      case RK3576_CLOCK_SOURCE_XIN_OSC0_FUNC:
        sel_bits = 0b11;
        break;
      default:
        _err("CRU: Invalid i2c bus clock selection %u", sel);
        return -EINVAL;
    }

  sel_bits <<= sel_offset;
  sel_bits |= (0b11 << (sel_offset + 16)); /* WE bits */

  putreg32(sel_bits, sel_reg);

  return OK;
}

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
                                       rk3576_clock_source_t *p_sel)
{
  unsigned long sel_reg;
  uint8_t sel_offset;
  uint32_t sel_bits;
  rk3576_clock_source_t sel;

  int ret = _get_i2c_clock_sel_register(i2c_bus_id, &sel_reg, &sel_offset);

  if (ret < 0)
    {
      return ret;
    }

  sel_bits = (getreg32(sel_reg) >> sel_offset) & 0b11;

  switch (sel_bits)
    {
      case 0b00:
        sel = RK3576_CLOCK_SOURCE_GPLL_DIV6_SRC;
        break;
      case 0b01:
        sel = RK3576_CLOCK_SOURCE_CPLL_DIV10_SRC;
        break;
      case 0b10:
        sel = RK3576_CLOCK_SOURCE_CPLL_DIV20_SRC;
        break;
      case 0b11:
        sel = RK3576_CLOCK_SOURCE_XIN_OSC0_FUNC;
        break;
    }

  if (p_sel)
    {
      *p_sel = sel;
    }

  return OK;
}

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
                                  bool clk_en)
{
  unsigned long pclk_reg, clk_reg;
  uint8_t pclk_offset, clk_offset;
  uint32_t pclk, clk;

  int ret = _get_i2c_clock_gate_register(i2c_bus_id, &pclk_reg, &clk_reg,
                                         &pclk_offset, &clk_offset);

  if (ret < 0)
    {
      return ret;
    }

  /* write 0 to enable clock, write 1 to disable */

  pclk = pclk_en ? 0 : (1 << pclk_offset);
  pclk |= (1 << (pclk_offset + 16)); /* WE bits */

  clk = clk_en ? 0 : (1 << clk_offset);
  clk |= (1 << (clk_offset + 16)); /* WE bits */

  /* if register is the same, merge write operation */
  if (pclk_reg == clk_reg)
    {
      putreg32(pclk | clk, pclk_reg);
    }
  else
    {
      putreg32(pclk, pclk_reg);
      putreg32(clk, clk_reg);
    }

  return OK;
}

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
                                  bool *p_clk_en)
{
  unsigned long pclk_reg, clk_reg;
  uint8_t pclk_offset, clk_offset;

  int ret = _get_i2c_clock_gate_register(i2c_bus_id, &pclk_reg, &clk_reg,
                                         &pclk_offset, &clk_offset);

  if (ret < 0)
    {
      return ret;
    }

  if (p_pclk_en)
    {
      /* 0 -> enabled ; 1 -> disabled */
      *p_pclk_en = (getreg32(pclk_reg) & (1 << pclk_offset)) == 0;
    }

  if (p_clk_en)
    {
      *p_clk_en = (getreg32(clk_reg) & (1 << clk_offset)) == 0;
    }

  return OK;
}

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
                                  bool clk_pwm_osc_en)
{
  unsigned long reg, rc_reg;
  uint8_t pclk_offset, clk_offset;
  uint8_t rc_clk_offset, osc_clk_offset;

  uint32_t reg_bits, rc_reg_bits;

  int ret =
      _get_pwm_clock_gate_reg(pwm_controller_id, &reg, &rc_reg, &pclk_offset,
                              &clk_offset, &osc_clk_offset, &rc_clk_offset);

  if (ret < 0)
    {
      return ret;
    }

  /* write 0 to enable clock, write 1 to disable */

  reg_bits = pclk_en ? 0 : (1 << pclk_offset);
  reg_bits |= clk_pwm_en ? 0 : (1 << clk_offset);
  reg_bits |= clk_pwm_osc_en ? 0 : (1 << osc_clk_offset);

  /* WE bits */

  reg_bits |= ((1 << pclk_offset) | (1 << clk_offset) | (1 << osc_clk_offset))
              << 16;

  rc_reg_bits = clk_pwm_rc_en ? 0 : (1 << rc_clk_offset);

  rc_reg_bits |= (1 << (rc_clk_offset + 16)); /* WE bits */

  /* if register is the same, merge write operation */
  if (reg == rc_reg)
    {
      putreg32(reg_bits | rc_reg_bits, reg);
    }
  else
    {
      putreg32(reg_bits, reg);
      putreg32(rc_reg_bits, rc_reg);
    }

  return OK;
}

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
                                  bool *p_clk_pwm_osc_en)
{
  unsigned long reg, rc_reg;
  uint8_t pclk_offset, clk_offset;
  uint8_t rc_clk_offset, osc_clk_offset;

  uint32_t reg_bits, rc_reg_bits;

  int ret =
      _get_pwm_clock_gate_reg(pwm_controller_id, &reg, &rc_reg, &pclk_offset,
                              &clk_offset, &osc_clk_offset, &rc_clk_offset);

  if (ret < 0)
    {
      return ret;
    }

  reg_bits = getreg32(reg);

  if (p_pclk_en)
    {
      /* 0 -> enabled ; 1 -> disabled */
      *p_pclk_en = (reg_bits & (1 << pclk_offset)) == 0;
    }

  if (p_clk_pwm_en)
    {
      *p_clk_pwm_en = (reg_bits & (1 << clk_offset)) == 0;
    }

  if (p_clk_pwm_osc_en)
    {
      *p_clk_pwm_osc_en = (reg_bits & (1 << osc_clk_offset)) == 0;
    }

  if (p_clk_pwm_rc_en)
    {
      rc_reg_bits = (rc_reg != reg) ? getreg32(rc_reg) : reg_bits;
      *p_clk_pwm_rc_en = (rc_reg_bits & (1 << rc_clk_offset)) == 0;
    }

  return OK;
}
