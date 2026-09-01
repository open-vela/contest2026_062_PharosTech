/****************************************************************************
 * chips/rk3576/rk3576_tsadc.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_TSADC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_TSADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sensors/sensor.h>

#ifdef CONFIG_RK3576_TSADC

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One per-channel on-die temperature sensor published by the driver.  Each
 * entry pairs the sensor lower-half handle with the device node path to
 * publish it as (e.g. "/dev/uorb/sensor_soc_center").  The board layer is
 * expected to call sensor_custom_register() on each entry. */

struct rk3576_tsadc_sensor_s
{
  FAR struct sensor_lowerhalf_s *lower; /* Lower-half sensor handle        */
  FAR const char *name;                 /* Device node path to register    */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_tsadc_initialize
 *
 * Description:
 *   Initialize the RK3576 TSADC, run it in auto mode, and hand back the
 *   array of per-channel on-die temperature sensors.  The board layer is
 *   responsible for calling sensor_custom_register() on each entry to
 *   publish the corresponding /dev/uorb/sensor_soc_* node.  The returned
 *   array is owned by the driver and valid for the system lifetime.
 *
 * Input Parameters:
 *   sensors - Output: pointer to the array of sensor descriptors.
 *   num     - Output: number of descriptors (i.e. number of TSADC channels).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 ****************************************************************************/

int rk3576_tsadc_initialize(FAR struct rk3576_tsadc_sensor_s **sensors,
                            FAR int *num);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_TSADC */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_TSADC_H */
