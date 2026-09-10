############################################################################
# apps/graphics/nyabula_display/Makefile
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

# Nyabula Display LVGL Compatibility Layer

PROGNAME = nyabula_display
PRIORITY = $(CONFIG_NYABULA_DISPLAY_PRIORITY)
STACKSIZE = $(CONFIG_NYABULA_DISPLAY_STACKSIZE)
MODULE = $(CONFIG_GRAPHICS_NYABULA_DISPLAY)

MAINSRC = main.c
CSRCS = nyabula_display.c nyabula_dual_lcd.c nyabula_scheduler_blankgated.c nyabula_dual_demo.c

ifeq ($(CONFIG_NYABULA_DISPLAY_TE_SW),y)
CSRCS += nyabula_te_sw.c
endif

ifeq ($(CONFIG_NYABULA_DISPLAY_TE_GPIO),y)
CSRCS += nyabula_te_gpio.c
endif

CFLAGS += -DCONFIG_NYABULA_DISPLAY_BUFFER_SIZE=$(CONFIG_NYABULA_DISPLAY_BUFFER_SIZE)

include $(APPDIR)/Application.mk
