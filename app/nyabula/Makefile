# SPDX-License-Identifier: Apache-2.0
include $(APPDIR)/Make.defs

CSRCS = src/nyabula_eye_engine.c src/nyabula_eye_renderer_lvgl.c
CSRCS += src/nyabula_eye_service.c src/nyabula_eye_json.c
CSRCS += src/generated/nyabula_eye_icons.c
CSRCS += $(wildcard src/generated/fonts/*.c)
CXXSRCS = src/nyabula_core.cxx
CXXFLAGS += -std=c++17 -fno-exceptions -fno-rtti

include $(APPDIR)/Application.mk
