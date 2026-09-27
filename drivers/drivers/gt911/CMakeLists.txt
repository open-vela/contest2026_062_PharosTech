############################################################################
# drivers/drivers/gt911/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Goodix GT911 touch controller driver, compiled into the kernel-side
# libdrivers.a target (the `drivers` target) only when enabled.
############################################################################

if(CONFIG_INPUT_GT911)
  target_sources(drivers PRIVATE gt911.c)
endif()
