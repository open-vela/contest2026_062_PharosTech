# drivers/drivers/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Kernel-side platform driver layer.  Reached from the board's CMakeLists (via
# the boards/rk3576/drivers symlink).  Recursively include every driver
# subdirectory, each of which injects its own source into the `drivers` kernel
# library target.

nuttx_add_subdirectory()

# Public header directory shared by all drivers: boards/rk3576/drivers/include/
# (exposed once here, not in every driver's CMakeLists.txt).  Works for every
# current and future driver because the driver's own `*.c` files (compiled into
# the `drivers` kernel library) and the kernel-side board code (compiled into
# `board`) both inherit these paths through nuttx's NUTTX_INCLUDE_DIRECTORIES.
#
# Guard on the master switch CONFIG_NYABULA_CUSTOM_DRIVERS: as long as any
# driver in this set is enabled, expose the shared include dir.  Each
# individual driver's enable option is defined in its own Kconfig.
if(CONFIG_NYABULA_CUSTOM_DRIVERS)
  set_property(
    TARGET nuttx
    APPEND
    PROPERTY NUTTX_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/../include)
endif()
