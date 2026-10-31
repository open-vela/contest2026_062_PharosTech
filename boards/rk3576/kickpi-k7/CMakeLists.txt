# SPDX-License-Identifier: Apache-2.0

# NuttX reads this sibling directory before expanding Kconfig. A broken
# checkout link must not silently turn the product drivers into undefined
# symbols through NuttX's empty platform Kconfig fallback.
set(platform_dir "${CMAKE_CURRENT_LIST_DIR}/../drivers")
if(NOT EXISTS "${platform_dir}/Kconfig")
  message(FATAL_ERROR "Missing K7 platform drivers: ${platform_dir}")
endif()
file(REAL_PATH "${platform_dir}/Kconfig" platform_kconfig)
file(REAL_PATH "${CMAKE_BINARY_DIR}/drivers/platform/Kconfig" bound_kconfig)
if(NOT platform_kconfig STREQUAL bound_kconfig)
  message(FATAL_ERROR "K7 platform Kconfig is not bound to this board")
endif()

# The upstream driver glob walks the source tree, not the build-tree link
# used for Kconfig. Reach the platform sources explicitly on fresh builds.
add_subdirectory("${platform_dir}" "${CMAKE_CURRENT_BINARY_DIR}/platform")

set_property(GLOBAL PROPERTY LD_SCRIPT "${NUTTX_BOARD_DIR}/scripts/dramboot.ld")
add_subdirectory(src)
