# ##############################################################################
# boards/rk3576/drivers/drivers/ws2812/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# WS2812 addressable LED driver — kernel-side character driver.
#
# Compiled into libdrivers.a (the `drivers` kernel library).  In CMake mode the
# upstream `drivers/platform` source-compilation channel is not implemented
# (nuttx/CMakeLists.txt only symlinks board drivers into
# $BINDIR/drivers/platform for Kconfig; nuttx/drivers' nuttx_add_subdirectory()
# globs the *source tree* and never reaches that build-tree symlink).  So we
# inject the source into the `drivers` target directly here; this CMakeLists is
# reached from the board's CMakeLists (see boards/rk3576/kickpi-k7/CMakeLists).
###############################################################################

if(CONFIG_WS2812_CHARDEV)
  target_sources(drivers PRIVATE ws2812.c)
endif()
