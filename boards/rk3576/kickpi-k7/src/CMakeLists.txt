# SPDX-License-Identifier: Apache-2.0
set(SRCS kickpi_k7_boardinit.c kickpi_k7_appinit.c kickpi_k7_reset.c)

if(CONFIG_KICKPI_K7_AUDIO)
  list(APPEND SRCS kickpi_k7_audio.c)
endif()

if(CONFIG_KICKPI_K7_WIFI)
  list(APPEND SRCS kickpi_k7_wifi.c kickpi_k7_sv6621_transport.c)
  target_compile_options(
    board
    PRIVATE
      "-Wa,-I${NUTTX_BOARD_DIR}/../../../drivers/drivers/sv6621/firmware")
endif()

if(CONFIG_KICKPI_K7_STORAGE_AUTOMOUNT)
  list(APPEND SRCS kickpi_k7_storage.c)
endif()

if(CONFIG_KICKPI_K7_RTC)
  list(APPEND SRCS kickpi_k7_rtc.c)
endif()

target_sources(board PRIVATE ${SRCS})

if(CONFIG_ETC_ROMFS)
  nuttx_add_romfs(
    NAME
    etc
    MOUNTPOINT
    etc
    RCSRCS
    etc/init.d/rcS
    etc/init.d/rc.sysinit
    PATH
    ${CMAKE_CURRENT_BINARY_DIR}/etc)

  target_link_libraries(board PRIVATE romfs_etc)
endif()
