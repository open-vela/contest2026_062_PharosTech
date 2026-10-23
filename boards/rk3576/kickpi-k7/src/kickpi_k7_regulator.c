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
 * configures and registers the 21 direct RK806 outputs (BUCK1-10, PLDO1-6,
 * NLDO1-5).  The parent names and the board voltage limits come from
 * g_kickpi_k7_rk806_cfg[] below.  The RK806_<OUT>_SUPPLY Kconfig symbols are
 * retained for now but are overridden by that table.
 *
 * The DDR rails (vddq_ddr_s0, vdd2_ddr_s3) are registered read-only: the
 * boot chain has already trained DRAM, so their ops deliberately provide no
 * enable/disable/set_voltage callback and NuttX never writes those PMIC
 * registers.  Their reported voltage is selected by the board DDR type
 * Kconfig (LPDDR4/LPDDR5).
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

/* DDR rail voltages are board-population dependent.  The rails are read-only
 * in NuttX: the boot chain has already trained DRAM, and changing VDDQ/VDD2
 * afterwards is not safe.  The value below is only reported through
 * regulator_get_voltage(); the driver never writes these rails.
 */

#if defined(CONFIG_KICKPI_K7_DDR_LPDDR4)
#define KICKPI_K7_VDDQ_DDR_UV 610000
#define KICKPI_K7_VDD2_DDR_UV 1100000
#else
#define KICKPI_K7_VDDQ_DDR_UV 510000
#define KICKPI_K7_VDD2_DDR_UV 1050000
#endif

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

/* Board constraints for the 21 RK806 outputs, taken from the official board
 * DTS.  The chip itself can span a wider range; min_uv/max_uv here are what
 * the load on this board is allowed to see.
 *
 * The parent names override the Kconfig RK806_*_SUPPLY defaults.  The Kconfig
 * symbols are kept for now and can be folded into this table in the later
 * configuration cleanup.
 *
 * Only the DDR rails are read-only.  The other rails stay controllable so
 * that the future consumer drivers (SDIO voltage switching, peripheral power
 * sequencing) can enable and disable them on demand.  The DDR rails are
 * additionally marked always-on in configs/dev/defconfig.
 */

static const struct rk806_rail_cfg
    g_kickpi_k7_rk806_cfg[RK806_NUM_OUTPUTS] = {
      [RK806_ID_BUCK1] = {
        .min_uv = 550000,
        .max_uv = 950000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK2] = {
        .min_uv = 550000,
        .max_uv = 950000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK3] = {
        .min_uv = 550000,
        .max_uv = 950000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK4] = {
        .min_uv = 3300000,
        .max_uv = 3300000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK5] = {
        .min_uv = 550000,
        .max_uv = 900000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK6] = {
        .min_uv = KICKPI_K7_VDDQ_DDR_UV,
        .max_uv = KICKPI_K7_VDDQ_DDR_UV,
        .readonly = true,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK7] = {
        .min_uv = 550000,
        .max_uv = 800000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK8] = {
        .min_uv = 1800000,
        .max_uv = 1800000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK9] = {
        .min_uv = KICKPI_K7_VDD2_DDR_UV,
        .max_uv = KICKPI_K7_VDD2_DDR_UV,
        .readonly = true,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_BUCK10] = {
        .min_uv = 550000,
        .max_uv = 1200000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_NLDO1] = {
        .min_uv = 750000,
        .max_uv = 750000,
        .supply = KICKPI_K7_RAIL_VCC_1V1_NLDO,
      },
      [RK806_ID_NLDO2] = {
        .min_uv = 850000,
        .max_uv = 850000,
        .supply = KICKPI_K7_RAIL_VCC_1V1_NLDO,
      },
      [RK806_ID_NLDO3] = {
        .min_uv = 837500,
        .max_uv = 837500,
        .supply = KICKPI_K7_RAIL_VCC_1V1_NLDO,
      },
      [RK806_ID_NLDO4] = {
        .min_uv = 850000,
        .max_uv = 850000,
        .supply = KICKPI_K7_RAIL_VCC_1V1_NLDO,
      },
      [RK806_ID_NLDO5] = {
        .min_uv = 750000,
        .max_uv = 750000,
        .supply = KICKPI_K7_RAIL_VCC_1V1_NLDO,
      },
      [RK806_ID_PLDO1] = {
        .min_uv = 1800000,
        .max_uv = 1800000,
        .supply = KICKPI_K7_RAIL_VCC_2V0_PLDO,
      },
      [RK806_ID_PLDO2] = {
        .min_uv = 1800000,
        .max_uv = 1800000,
        .supply = KICKPI_K7_RAIL_VCC_2V0_PLDO,
      },
      [RK806_ID_PLDO3] = {
        .min_uv = 1200000,
        .max_uv = 1200000,
        .supply = KICKPI_K7_RAIL_VCC_2V0_PLDO,
      },
      [RK806_ID_PLDO4] = {
        .min_uv = 3300000,
        .max_uv = 3300000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_PLDO5] = {
        .min_uv = 1800000,
        .max_uv = 3300000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
      [RK806_ID_PLDO6] = {
        .min_uv = 1800000,
        .max_uv = 1800000,
        .supply = KICKPI_K7_RAIL_VCC_SYS,
      },
    };

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int kickpi_k7_fixed_initialize(void)
{
  size_t i;

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

static int kickpi_k7_rk806_configure(void)
{
  unsigned int i;

  for (i = 0; i < RK806_NUM_OUTPUTS; i++)
    {
      int ret = rk806_rail_configure(i, &g_kickpi_k7_rk806_cfg[i]);

      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: failed to configure RK806 rail %u: %d\n", i,
                 ret);
          return ret;
        }
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

  /* Apply the board voltage limits, read-only policy and parent names before
   * the RK806 descriptors are registered.
   */

  ret = kickpi_k7_rk806_configure();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to configure RK806 rails\n");
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

  ret = rk806_initialize(i2c1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to init RK806\n");
      return ret;
    }

  return OK;
#endif
}
