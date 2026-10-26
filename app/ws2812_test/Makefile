############################################################################
# app/ws2812_test/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# WS2812 LED strip smoke test, built as the "ws2812_test" NSH built-in.
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_EXAMPLES_WS2812_TEST_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_WS2812_TEST_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_WS2812_TEST_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_WS2812_TEST)

MAINSRC = ws2812_test_main.c

include $(APPDIR)/Application.mk
