# drivers/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# SoC-family (platform) drivers.  This directory is symlinked into the kernel
# build as $BINDIR/drivers/platform (CMake mode) / nuttx/drivers/platform
# (Makefile mode) via CONFIG_SPECIFIC_DRIVERS + BOARD_DRIVERS_DIR.  In CMake
# mode it is reached from the board's CMakeLists (see
# boards/rk3576/kickpi-k7/CMakeLists.txt) and recurses into the driver layer.

nuttx_add_subdirectory()
