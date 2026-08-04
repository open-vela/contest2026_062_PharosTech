############################################################################
# boards/arm64/rk3576/kickpi_k7/src/Makefile
#
# SPDX-License-Identifier: Apache-2.0
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

include $(TOPDIR)/Make.defs

CSRCS = kickpi_k7_boardinit.c
CSRCS += kickpi_k7_appinit.c
CSRCS += kickpi_k7_reset.c

# Makefile (legacy) build mode: expose the platform driver's public header
# directory (boards/rk3576/drivers/include, shared by all platform drivers) so
# this kernel-side board code can #include "<driver>.h".  Both this board code
# and the drivers now live on the kernel side; the drivers are found via
# BOARD_DRIVERS_DIR ($(BOARD_DIR)/../drivers, symlinked to the shared drivers
# tree).  Guard on the master switch CONFIG_NYABULA_CUSTOM_DRIVERS.  (CMake
# mode exposes it via NUTTX_INCLUDE_DIRECTORIES instead of this file.)
ifeq ($(CONFIG_NYABULA_CUSTOM_DRIVERS),y)
  CFLAGS += ${INCDIR_PREFIX}"$(BOARD_DIR)$(DELIM)..$(DELIM)drivers$(DELIM)include"
endif

ifeq ($(CONFIG_KICKPI_K7_AUDIO),y)
CSRCS += kickpi_k7_audio.c
endif

ifeq ($(CONFIG_KICKPI_K7_STORAGE_AUTOMOUNT),y)
CSRCS += kickpi_k7_storage.c
endif

ifeq ($(CONFIG_ETC_ROMFS),y)
  RCSRCS = etc/init.d/rc.sysinit etc/init.d/rcS
endif

include $(TOPDIR)/boards/Board.mk
