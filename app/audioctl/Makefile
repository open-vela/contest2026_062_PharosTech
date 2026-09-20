############################################################################
# apps/system/audioctl/Makefile
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

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_SYSTEM_AUDIOCTL_PROGNAME)
PRIORITY  = $(CONFIG_SYSTEM_AUDIOCTL_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_AUDIOCTL_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_AUDIOCTL)

MAINSRC = audioctl_main.c

# Volume, mute and microphone gain go through the Nyabula audio service when
# it is built in (the driver has no readback, the service keeps the record).
# Its header lives where the manifest links Core, not beside this directory.
ifeq ($(CONFIG_NYABULA_CORE_AUDIO),y)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/demos/contest2026_062_nyabula_core
endif

include $(APPDIR)/Application.mk
