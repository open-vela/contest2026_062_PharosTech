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
 * The code here only registers the regulators that are direct RK806
 * outputs (BUCK1-10, PLDO1-6, NLDO1-5).  The board schematic has some
 * additional power rails that are not registered yet; leaving them
 * unregistered is harmless for now.
 *
 * Each RK806 output can have a parent ("supply") regulator.  The parent is
 * the rail that feeds the corresponding RK806 input (VCC1..VCC14/VCCA),
 * which on a real board may itself be a controllable regulator (e.g. an
 * external buck feeding the PMIC) rather than a fixed always-on rail.
 * These parents are NOT RK806 outputs, so they are not registered by
 * rk806_initialize(); they must be registered separately here.
 *
 * If such a parent regulator is ever needed, register it in this file
 * (kickpi_k7_regulator.c) and then give its name in the matching
 * RK806_<OUT>_SUPPLY entry in the board defconfig
 * (configs/dev/defconfig), e.g.:
 *
 *   CONFIG_RK806_BUCK1_SUPPLY="<parent-name>"
 *   CONFIG_RK806_NLDO1_2_3_SUPPLY="<parent-name>"   (shared input VCC13)
 *   CONFIG_RK806_NLDO4_5_SUPPLY="<parent-name>"     (shared input VCC14)
 *   CONFIG_RK806_PLDO1_2_3_SUPPLY="<parent-name>"   (shared input VCC11)
 *   CONFIG_RK806_PLDO4_5_SUPPLY="<parent-name>"     (shared input VCC12)
 *   CONFIG_RK806_PLDO6_SUPPLY="<parent-name>"       (input VCCA)
 *
 * IMPORTANT: the parent regulator MUST be registered onto the regulator
 * framework BEFORE rk806_initialize() is called.  When a supply_name is
 * non-empty, the framework resolves it lazily via regulator_get(name) at
 * enable time; if the parent has not been registered first, that lookup
 * fails and rk806_initialize() / the enable path returns an error.  So any
 * parent registration code must go above the rk806_initialize() call below.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include "kickpi_k7.h"

#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk806.h"

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
  /* I2C1 M0 for RK806 PMIC */
  rk3576_config_gpio(GPIO_PORT0 | GPIO_PIN_B2 | GPIO_ALT | GPIO_AF11);
  rk3576_config_gpio(GPIO_PORT0 | GPIO_PIN_B3 | GPIO_ALT | GPIO_AF11);

  struct i2c_master_s *i2c1 = rk3576_i2c_initialize(1);
  if (!i2c1)
    {
      syslog(LOG_ERR, "Failed to init I2C1\n");
      return -EIO;
    }

  int ret = rk806_initialize(i2c1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to init RK806\n");
      return ret;
    }

  return OK;
#endif
}
