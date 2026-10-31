############################################################################
# boards/rk3576/drivers/drivers/rk806/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################
#
# Rockchip RK806 PMIC regulator driver build (CMake mode).
#
# RK806 is a multi-channel PMIC: multiple BUCK (DCDC) + LDO outputs, each
# independently switchable and voltage-adjustable. It is registered onto the
# NuttX regulator framework (include/nuttx/power/regulator.h).
#
# This is a kernel-side driver: injected into the kernel `drivers` library
# target (libdrivers.a), not apps.
#
############################################################################

# Enable config (Kconfig-configured) for the rk806 driver.
if(CONFIG_REGULATOR_RK806)
  target_sources(drivers PRIVATE rk806.c)
endif()
