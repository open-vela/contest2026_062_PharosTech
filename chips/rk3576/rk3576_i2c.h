/****************************************************************************
 * chips/rk3576/rk3576_i2c.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_I2C_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_I2C_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

#ifdef CONFIG_RK3576_I2C

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3576_i2c_initialize
 *
 * Description:
 *   Initialize one RK3576 I2C controller and return its i2c_master_s.  The
 *   controller clock is ungated here; the caller (board logic) is
 *   responsible for muxing the SCL/SDA pins beforehand.
 *
 * Input Parameters:
 *   bus - The controller number (e.g. 2 for I2C2).
 *
 * Returned Value:
 *   A pointer to the i2c_master_s on success; NULL on failure.
 *
 ****************************************************************************/

struct i2c_master_s *rk3576_i2c_initialize(int bus);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_I2C */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_I2C_H */
