# SPDX-License-Identifier: Apache-2.0
set_property(GLOBAL PROPERTY LD_SCRIPT "${NUTTX_BOARD_DIR}/scripts/dramboot.ld")
add_subdirectory(src)
