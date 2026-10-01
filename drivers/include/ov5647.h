/****************************************************************************
 * drivers/drivers/ov5647/ov5647.h
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
 * OmniVision OV5647 MIPI CSI-2 camera sensor -- public interface.
 ****************************************************************************/

#ifndef __DRIVERS_PLATFORM_DRIVERS_OV5647_OV5647_H
#define __DRIVERS_PLATFORM_DRIVERS_OV5647_OV5647_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/imgsensor.h>

#ifdef CONFIG_VIDEO_OV5647

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ov5647_initialize
 *
 * Description:
 *   Bind the OV5647 sensor driver to an I2C (SCCB) bus.  This only records
 *   the bus and the address; no hardware access happens here, so the board
 *   may call it before the sensor's power or clock is up.  The first real
 *   transaction is the chip-id probe in is_available(), which the capture
 *   framework performs when the device is registered.
 *
 * Input Parameters:
 *   i2c - The SCCB bus.  The driver keeps the pointer for the lifetime of
 *         the capture device; the board must not release it.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int ov5647_initialize(FAR struct i2c_master_s *i2c);

/****************************************************************************
 * Name: ov5647_sensor
 *
 * Description:
 *   Return the sensor handle to pass to capture_register().  Valid only
 *   after a successful ov5647_initialize().
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   The sensor handle, or NULL if the driver has not been initialised.
 *
 ****************************************************************************/

FAR struct imgsensor_s *ov5647_sensor(void);

#endif /* CONFIG_VIDEO_OV5647 */
#endif /* __DRIVERS_PLATFORM_DRIVERS_OV5647_OV5647_H */
