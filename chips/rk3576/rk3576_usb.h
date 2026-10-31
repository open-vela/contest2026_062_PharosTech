/****************************************************************************
 * chips/rk3576/rk3576_usb.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_USB_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_USB_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

struct usbhost_connection_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_usb_initialize
 *
 * Description:
 *   Bring up the RK3576 USB OTG0 controller (Synopsys DWC3 at 0x23000000)
 *   in peripheral (device) mode and hook it into the NuttX USB device
 *   stack.  Called implicitly by usbdev_register() when the first class
 *   driver binds, but a board may call it early to fail fast.
 *
 * Returned Value:
 *   OK on success; a negated errno on failure (e.g. the core ID register
 *   does not read back as a DWC3).
 *
 ****************************************************************************/

int rk3576_usb_initialize(void);

/****************************************************************************
 * Name: rk3576_usbhost_initialize
 *
 * Description:
 *   Put USB1 into host mode and attach its xHCI register window and fixed
 *   interrupt to the NuttX USB host stack.
 *
 * Returned Value:
 *   A USB host connection on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct usbhost_connection_s *rk3576_usbhost_initialize(void);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_USB_H */
