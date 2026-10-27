/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_regulator.c
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
 * Notes on the RK806 regulator tree
 *
 * This file registers the four fixed input rails that feed the PMIC
 * (vcc12v_dcin -> vcc_sys -> vcc_2v0_pldo_s3 / vcc_1v1_nldo_s3), then
 * registers the 21 direct RK806 outputs (BUCK1-10, PLDO1-6, NLDO1-5).  The
 * complete board description -- names, voltage limits, parent supplies and
 * boot flags -- lives in g_kickpi_k7_rk806_cfg[] below.
 *
 * The DDR rails (vddq_ddr_s0, vdd2_ddr_s3) are set by external feedback
 * resistors populated per memory type, not by the PMIC VSEL register.  They
 * are registered read-only: their ops deliberately provide no
 * enable/disable/set_voltage callback and NuttX never writes those PMIC
 * registers.  Because software cannot know the actual voltage, their
 * regulator_get_voltage() reports -ENODATA and the board table carries no
 * voltage value for them.
 *
 * IMPORTANT: parent rails MUST be registered onto the regulator framework
 * BEFORE rk806_initialize() is called.  A boot_on/always_on descriptor
 * resolves its supply through regulator_get() during registration; if the
 * parent has not been registered first, that lookup fails and
 * rk806_initialize() / the enable path returns an error.  The order in
 * kickpi_k7_regulator_initialize() enforces this.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stddef.h>
#include <sys/param.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/power/regulator.h>

#include "kickpi_k7.h"

#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk806.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK806 control bus: I2C1 default signal group (DTS i2c1m0-xfer).  Both
 * pads are in the GPIO0 bank and select alternate function 11.
 */

#define KICKPI_K7_I2C1_SCL_PIN (GPIO_PORT0 | GPIO_PIN_B2)
#define KICKPI_K7_I2C1_SDA_PIN (GPIO_PORT0 | GPIO_PIN_B3)
#define KICKPI_K7_I2C1_AF      11

/* Fixed rails that feed the RK806.  These names must match the regulator
 * names used by the official board DTS; they are also the parent names
 * referenced by the per-rail table below.
 */

#define KICKPI_K7_RAIL_VCC12V_DCIN  "vcc12v_dcin"
#define KICKPI_K7_RAIL_VCC_SYS      "vcc_sys"
#define KICKPI_K7_RAIL_VCC_2V0_PLDO "vcc_2v0_pldo_s3"
#define KICKPI_K7_RAIL_VCC_1V1_NLDO "vcc_1v1_nldo_s3"

/* DDR rail voltages are set by external feedback resistors (BUCK6/BUCK9 FB
 * pin, not the PMIC VSEL register).  The rails are read-only in NuttX: the
 * boot chain has already trained DRAM, and changing VDDQ/VDD2 afterwards is
 * not safe.  Software cannot know the actual voltage, so these rails report
 * -ENODATA and carry no voltage value at all.
 */

#ifdef CONFIG_KICKPI_K7_REGULATOR

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The fixed input rails have no software-controllable enable.  Their ops
 * deliberately provide no enable/disable/set_voltage callback, so both
 * registration and later regulator_enable()/disable() calls leave the
 * hardware untouched.
 */

static int kickpi_k7_fixed_list_voltage(FAR struct regulator_dev_s *rdev,
                                        unsigned int selector)
{
  if (selector != 0)
    {
      return -EINVAL;
    }

  return (int)rdev->desc->min_uv;
}

static int kickpi_k7_fixed_get_voltage(FAR struct regulator_dev_s *rdev)
{
  return (int)rdev->desc->min_uv;
}

static const struct regulator_ops_s g_kickpi_k7_fixed_ops = {
  .list_voltage = kickpi_k7_fixed_list_voltage,
  .get_voltage = kickpi_k7_fixed_get_voltage,
};

/* Registration order is also the parent-before-child order: rk806
 * registration resolves each supply through regulator_get(), so a parent
 * must already be on the regulator list.
 */

static const struct regulator_desc_s g_kickpi_k7_fixed_rails[] = {
  {
      .name = KICKPI_K7_RAIL_VCC12V_DCIN,
      .n_voltages = 1,
      .min_uv = 12000000,
      .max_uv = 12000000,
      .boot_on = 1,
      .always_on = 1,
  },
  {
      .name = KICKPI_K7_RAIL_VCC_SYS,
      .n_voltages = 1,
      .min_uv = 5000000,
      .max_uv = 5000000,
      .boot_on = 1,
      .always_on = 1,
      .supply_name = KICKPI_K7_RAIL_VCC12V_DCIN,
  },
  {
      .name = KICKPI_K7_RAIL_VCC_2V0_PLDO,
      .n_voltages = 1,
      .min_uv = 2000000,
      .max_uv = 2000000,
      .boot_on = 1,
      .always_on = 1,
      .supply_name = KICKPI_K7_RAIL_VCC_SYS,
  },
  {
      .name = KICKPI_K7_RAIL_VCC_1V1_NLDO,
      .n_voltages = 1,
      .min_uv = 1100000,
      .max_uv = 1100000,
      .boot_on = 1,
      .always_on = 1,
      .supply_name = KICKPI_K7_RAIL_VCC_SYS,
  },
};

/* Board description for the 21 RK806 outputs, taken from the official board
 * DTS.  The chip itself can span a wider range; min_uv/max_uv here are what
 * the load on this board is allowed to see.
 *
 * READBACK rails use the PMIC's internal feedback and the normal regulator
 * operations.  The DDR rails (BUCK6/BUCK9) use external feedback resistors
 * populated per memory type: their VSEL registers do not reflect the output,
 * so they are UNKNOWN and read-only.  Because software cannot know their
 * voltage, their min_uv/max_uv are zero and regulator_get_voltage() returns
 * -ENODATA.
 *
 * The boot flags follow the board DTS: every output except vdd_npu_s0 and
 * vdd_gpu_s0 is marked regulator-always-on there.  Those rails therefore use
 * KICKPI_K7_RK806_RAIL_AO(), or KICKPI_K7_RK806_RAIL_UNKNOWN() for the two
 * external-feedback DDR rails; the NPU and GPU rails are boot-on only in the
 * DTS and stay controllable.
 *
 * always_on is not redundant with boot_on.  boot_on only switches the rail on
 * once, during regulator_register(); it does not stop a later
 * regulator_enable()/regulator_disable() pair from reaching the PMIC, because
 * regulator_disable() acts when use_count falls from 1 to 0.  Removing a CPU,
 * logic, DRAM or main I/O supply that way takes the board down, so the rails
 * the board DTS marks always-on must never be disableable.
 */

/* The fields shared by every READBACK rail.  KICKPI_K7_RK806_RAIL() and
 * KICKPI_K7_RK806_RAIL_AO() differ only in the always_on flag, so the shared
 * part is written once here.
 */

#define KICKPI_K7_RK806_RAIL_FIELDS(_name, _supply, _min, _max, _vs) \
  .name = _name, .supply = _supply, .min_uv = _min, .max_uv = _max,  \
  .voltage_source = _vs, .boot_on = true

/* Controllable READBACK rail: boot-on only in the board DTS. */

#define KICKPI_K7_RK806_RAIL(_name, _supply, _min, _max)    \
  {                                                         \
    KICKPI_K7_RK806_RAIL_FIELDS(_name, _supply, _min, _max, \
                                RK806_VOLTAGE_READBACK)     \
  }

/* Always-on READBACK rail: the board DTS marks it regulator-always-on, so it
 * is enabled at registration and regulator_disable() never reaches the PMIC.
 */

#define KICKPI_K7_RK806_RAIL_AO(_name, _supply, _min, _max) \
  {                                                         \
    KICKPI_K7_RK806_RAIL_FIELDS(_name, _supply, _min, _max, \
                                RK806_VOLTAGE_READBACK),    \
        .always_on = true                                   \
  }

/* Read-only UNKNOWN rail (external feedback, no usable VSEL register).  Also
 * always-on: enable/disable have no callback to reach, but saying so keeps the
 * descriptor consistent with the board DTS.
 */

#define KICKPI_K7_RK806_RAIL_UNKNOWN(_name, _supply)                          \
  {                                                                           \
    KICKPI_K7_RK806_RAIL_FIELDS(_name, _supply, 0, 0, RK806_VOLTAGE_UNKNOWN), \
        .always_on = true                                                     \
  }

static const struct rk806_rail_cfg g_kickpi_k7_rk806_cfg[RK806_NUM_OUTPUTS] = {
  [RK806_ID_BUCK1] = KICKPI_K7_RK806_RAIL_AO(
      "vdd_cpu_big_s0", KICKPI_K7_RAIL_VCC_SYS, 550000, 950000),
  [RK806_ID_BUCK2] = KICKPI_K7_RK806_RAIL("vdd_npu_s0", KICKPI_K7_RAIL_VCC_SYS,
                                          550000, 950000),
  [RK806_ID_BUCK3] = KICKPI_K7_RK806_RAIL_AO(
      "vdd_cpu_lit_s0", KICKPI_K7_RAIL_VCC_SYS, 550000, 950000),
  [RK806_ID_BUCK4] = KICKPI_K7_RK806_RAIL_AO(
      "vcc_3v3_s3", KICKPI_K7_RAIL_VCC_SYS, 3300000, 3300000),
  [RK806_ID_BUCK5] = KICKPI_K7_RK806_RAIL("vdd_gpu_s0", KICKPI_K7_RAIL_VCC_SYS,
                                          550000, 900000),
  [RK806_ID_BUCK6] =
      KICKPI_K7_RK806_RAIL_UNKNOWN("vddq_ddr_s0", KICKPI_K7_RAIL_VCC_SYS),
  [RK806_ID_BUCK7] = KICKPI_K7_RK806_RAIL_AO(
      "vdd_logic_s0", KICKPI_K7_RAIL_VCC_SYS, 550000, 800000),
  [RK806_ID_BUCK8] = KICKPI_K7_RK806_RAIL_AO(
      "vcc_1v8_s3", KICKPI_K7_RAIL_VCC_SYS, 1800000, 1800000),
  [RK806_ID_BUCK9] =
      KICKPI_K7_RK806_RAIL_UNKNOWN("vdd2_ddr_s3", KICKPI_K7_RAIL_VCC_SYS),
  [RK806_ID_BUCK10] = KICKPI_K7_RK806_RAIL_AO(
      "vdd_ddr_s0", KICKPI_K7_RAIL_VCC_SYS, 550000, 1200000),
  [RK806_ID_NLDO1] = KICKPI_K7_RK806_RAIL_AO(
      "vdd_0v75_s3", KICKPI_K7_RAIL_VCC_1V1_NLDO, 750000, 750000),
  [RK806_ID_NLDO2] = KICKPI_K7_RK806_RAIL_AO(
      "vdda_ddr_pll_s0", KICKPI_K7_RAIL_VCC_1V1_NLDO, 850000, 850000),
  [RK806_ID_NLDO3] = KICKPI_K7_RK806_RAIL_AO(
      "vdda0v75_hdmi_s0", KICKPI_K7_RAIL_VCC_1V1_NLDO, 837500, 837500),
  [RK806_ID_NLDO4] = KICKPI_K7_RK806_RAIL_AO(
      "vdda_0v85_s0", KICKPI_K7_RAIL_VCC_1V1_NLDO, 850000, 850000),
  [RK806_ID_NLDO5] = KICKPI_K7_RK806_RAIL_AO(
      "vdda_0v75_s0", KICKPI_K7_RAIL_VCC_1V1_NLDO, 750000, 750000),
  [RK806_ID_PLDO1] = KICKPI_K7_RK806_RAIL_AO(
      "vcca_1v8_s0", KICKPI_K7_RAIL_VCC_2V0_PLDO, 1800000, 1800000),
  [RK806_ID_PLDO2] = KICKPI_K7_RK806_RAIL_AO(
      "vcca1v8_pldo2_s0", KICKPI_K7_RAIL_VCC_2V0_PLDO, 1800000, 1800000),
  [RK806_ID_PLDO3] = KICKPI_K7_RK806_RAIL_AO(
      "vdda_1v2_s0", KICKPI_K7_RAIL_VCC_2V0_PLDO, 1200000, 1200000),
  [RK806_ID_PLDO4] = KICKPI_K7_RK806_RAIL_AO(
      "vcca_3v3_s0", KICKPI_K7_RAIL_VCC_SYS, 3300000, 3300000),
  [RK806_ID_PLDO5] = KICKPI_K7_RK806_RAIL_AO(
      "vccio_sd_s0", KICKPI_K7_RAIL_VCC_SYS, 1800000, 3300000),
  [RK806_ID_PLDO6] = KICKPI_K7_RK806_RAIL_AO(
      "vcca1v8_pldo6_s3", KICKPI_K7_RAIL_VCC_SYS, 1800000, 1800000),
};

#undef KICKPI_K7_RK806_RAIL_UNKNOWN
#undef KICKPI_K7_RK806_RAIL_AO
#undef KICKPI_K7_RK806_RAIL
#undef KICKPI_K7_RK806_RAIL_FIELDS

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int kickpi_k7_fixed_initialize(void)
{
  size_t i;

  /* Stop at the first failure here, unlike rk806_initialize(): these four
   * rails are a parent chain (vcc12v_dcin -> vcc_sys -> vcc_2v0_pldo_s3 /
   * vcc_1v1_nldo_s3), so a rail whose parent is missing cannot register
   * either, and continuing would only repeat the same failure further down.
   */

  for (i = 0; i < nitems(g_kickpi_k7_fixed_rails); i++)
    {
      if (regulator_register(&g_kickpi_k7_fixed_rails[i],
                             &g_kickpi_k7_fixed_ops, NULL) == NULL)
        {
          syslog(LOG_ERR, "ERROR: failed to register fixed rail %s\n",
                 g_kickpi_k7_fixed_rails[i].name);
          return -ENODEV;
        }

      syslog(LOG_INFO, "Registered fixed regulator %s (%u uV)\n",
             g_kickpi_k7_fixed_rails[i].name,
             g_kickpi_k7_fixed_rails[i].min_uv);
    }

  return OK;
}

#endif /* CONFIG_KICKPI_K7_REGULATOR */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_regulator_initialize
 *
 * Description:
 *   Initialize the on-board RK806 PMIC, which is wired to the I2C1 bus
 *   (I2C1 M0 signal group).  This function is guarded by
 *   CONFIG_KICKPI_K7_REGULATOR (which itself requires CONFIG_REGULATOR_RK806
 *   and CONFIG_RK3576_I2C).
 *
 *   See the "Notes on the RK806 regulator tree" block above: any parent
 *   ("supply") regulator for the RK806 outputs must be registered here,
 *   before rk806_initialize() is called.
 *
 * Returned Value:
 *   Zero (OK) on success, or a negated errno on failure.
 *
 ****************************************************************************/

int kickpi_k7_regulator_initialize(void)
{
#ifndef CONFIG_KICKPI_K7_REGULATOR
  return -ENODEV;
#else
  FAR struct gpio_dev_s *scl;
  FAR struct gpio_dev_s *sda;
  FAR struct i2c_master_s *i2c1;
  int ret;

  /* Register the fixed input rails first: rk806_initialize() resolves each
   * PMIC output's parent through regulator_get(), and the parent must
   * already be on the regulator list.
   */

  ret = kickpi_k7_fixed_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to init fixed regulators\n");
      return ret;
    }

  /* Mux the I2C1 signal group.  The handles are deliberately not put():
   * once muxed, the pads belong to the I2C controller for as long as it
   * runs.
   */

  ret = rk3576_gpio_get(KICKPI_K7_I2C1_SCL_PIN, &scl);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to claim I2C1 SCL (GPIO0_B2): %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_af(scl, KICKPI_K7_I2C1_AF);

  ret = rk3576_gpio_get(KICKPI_K7_I2C1_SDA_PIN, &sda);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to claim I2C1 SDA (GPIO0_B3): %d\n", ret);
      return ret;
    }

  rk3576_gpio_set_af(sda, KICKPI_K7_I2C1_AF);

  i2c1 = rk3576_i2c_initialize(1);
  if (i2c1 == NULL)
    {
      syslog(LOG_ERR, "Failed to init I2C1\n");
      return -EIO;
    }

  ret = rk806_initialize(i2c1, g_kickpi_k7_rk806_cfg, RK806_NUM_OUTPUTS);
  if (ret < 0)
    {
      /* Not fatal: rk806_initialize() reports a failure for a single rail
       * while the others stay registered, so the tree is partial rather than
       * absent (see the contract in rk806.h).
       */

      syslog(LOG_ERR, "RK806: rail tree incomplete (%d), some rails are up\n",
             ret);
      return ret;
    }

  return OK;
#endif
}
