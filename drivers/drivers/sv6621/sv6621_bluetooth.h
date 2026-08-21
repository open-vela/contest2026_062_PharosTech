/****************************************************************************
 * drivers/drivers/sv6621/sv6621_bluetooth.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_BLUETOOTH_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_BLUETOOTH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_SV6621_BLUETOOTH

#include "sv6621.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_dev_s;

int sv6621_bluetooth_attach(FAR struct sv6621_dev_s *dev);
void sv6621_bluetooth_detach(FAR struct sv6621_dev_s *dev);
int sv6621_bluetooth_start(FAR struct sv6621_dev_s *dev,
                           FAR const struct sv6621_firmware_s *nvram,
                           int device_id);
bool sv6621_bluetooth_is_started(FAR struct sv6621_dev_s *dev);
void sv6621_bluetooth_offline(FAR struct sv6621_dev_s *dev, int error);

#endif /* CONFIG_SV6621_BLUETOOTH */
#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_BLUETOOTH_H */
