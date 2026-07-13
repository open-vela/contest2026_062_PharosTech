# SPDX-License-Identifier: Apache-2.0
set(SRCS kickpi_k7_boardinit.c kickpi_k7_appinit.c kickpi_k7_reset.c)

if(CONFIG_KICKPI_K7_DMA_PROBE)
  list(APPEND SRCS kickpi_k7_dmaprobe.c)
endif()

target_sources(board PRIVATE ${SRCS})
