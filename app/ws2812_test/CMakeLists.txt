# ##############################################################################
# app/ws2812_test/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# WS2812 LED strip smoke test, built as the "ws2812_test" NSH built-in.
###############################################################################

if(CONFIG_EXAMPLES_WS2812_TEST)
  nuttx_add_application(
    MODULE
    ${CONFIG_EXAMPLES_WS2812_TEST}
    NAME
    ${CONFIG_EXAMPLES_WS2812_TEST_PROGNAME}
    SRCS
    ws2812_test_main.c
    STACKSIZE
    ${CONFIG_EXAMPLES_WS2812_TEST_STACKSIZE}
    PRIORITY
    ${CONFIG_EXAMPLES_WS2812_TEST_PRIORITY})
endif()
