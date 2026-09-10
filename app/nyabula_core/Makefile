############################################################################
# packages/demos/contest2026_062_nyabula_core/Makefile
# SPDX-License-Identifier: Apache-2.0
############################################################################
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
############################################################################

include $(APPDIR)/Make.defs

ifeq ($(CONFIG_NYABULA_CORE_EYE),y)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/graphics/nyabula_eye/include
endif

PROGNAME  = nycore
PRIORITY  = $(CONFIG_NYABULA_CORE_PRIORITY)
STACKSIZE = $(CONFIG_NYABULA_CORE_STACKSIZE)
MODULE    = $(CONFIG_CONTEST2026_062_NYABULA_CORE)

# Mirror the CMake include directories: libsodium and WAMR do not export
# their headers globally under the legacy Make build.

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/libsodium/libsodium/src/libsodium/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/interpreters/wamr/wamr/core/iwasm/include

MAINSRC = nycore_main.c
CSRCS   = ny_broker.c ny_capability.c ny_health.c ny_manifest.c ny_module.c
CSRCS  += ny_package.c
CSRCS  += ny_permission.c
CSRCS  += ny_provider.c
CSRCS  += ny_runtime.c ny_revocation.c ny_scheduler.c
CSRCS  += ny_wasm.c

ifeq ($(CONFIG_NYABULA_CORE_HTTP),y)
CSRCS += ny_http.c ny_broker_http.c
endif

ifeq ($(CONFIG_NYABULA_CORE_STATE_SQLITE),y)
NYCORE_SQLITE_DIR = $(CURDIR)/.sqlite
CSRCS += ny_state.c sqlite3.c
VPATH += $(NYCORE_SQLITE_DIR)
CFLAGS += ${INCDIR_PREFIX}$(NYCORE_SQLITE_DIR)
$(NYCORE_SQLITE_DIR)/sqlite3.c_CFLAGS += -DSQLITE_OS_UNIX=1 -DSQLITE_THREADSAFE=0
$(NYCORE_SQLITE_DIR)/sqlite3.c_CFLAGS += -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL
$(NYCORE_SQLITE_DIR)/sqlite3.c_CFLAGS += -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_TEMP_STORE=3
$(NYCORE_SQLITE_DIR)/sqlite3.c_CFLAGS += -DSQLITE_DEFAULT_MEMSTATUS=0
$(NYCORE_SQLITE_DIR)/sqlite3.c_CFLAGS += -Wno-unused-variable -Wno-undef -Wno-unused-function -Wno-shadow
context::
	$(Q) python3 $(CURDIR)/prepare_sqlite.py --output $(NYCORE_SQLITE_DIR)
endif

ifeq ($(CONFIG_NYABULA_CORE_REQUIRE_SIGNATURE),y)
CSRCS  += ny_signature.c
endif

include $(APPDIR)/Application.mk
