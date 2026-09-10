# SPDX-License-Identifier: Apache-2.0
include $(APPDIR)/Make.defs

CSRCS = src/nyabula_eye_engine.c src/nyabula_eye_renderer_lvgl.c
CSRCS += src/nyabula_eye_service.c src/nyabula_eye_json.c
CSRCS += src/generated/nyabula_eye_icons.c
CSRCS += $(wildcard src/generated/fonts/*.c)
CXXSRCS = src/nyabula_core.cxx
CXXFLAGS += -std=c++17 -fno-exceptions -fno-rtti

ifeq ($(CONFIG_NYABULA_EYE_DISPLAY),y)
PROGNAME = nyabula_eye
PRIORITY = $(CONFIG_NYABULA_EYE_PRIORITY)
STACKSIZE = $(CONFIG_NYABULA_EYE_STACKSIZE)
MODULE = $(CONFIG_NYABULA_EYE_DISPLAY)
MAINSRC = src/nyabula_eye_main.c
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/graphics/nyabula_display
endif

include $(APPDIR)/Application.mk
