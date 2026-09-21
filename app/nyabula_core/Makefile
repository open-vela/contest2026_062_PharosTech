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
ifeq ($(CONFIG_NYABULA_CORE_AGENT),y)
PROGNAME += nyabula_mcp
PRIORITY += 100
STACKSIZE += 16384
MODULE += y
MAINSRC += ny_mcp_main.c
endif
ifeq ($(CONFIG_NYABULA_CORE_WEB),y)
PROGNAME += nyabula_web
PRIORITY += 100
STACKSIZE += 16384
MODULE += y
MAINSRC += ny_web_main.c
ifeq ($(CONFIG_NYABULA_EYE_WS),y)
PROGNAME += nyabula_eye_ws
PRIORITY += 100
STACKSIZE += 16384
MODULE += y
MAINSRC += ny_eye_ws_compat_main.c
endif
endif
CSRCS   = ny_broker.c ny_capability.c ny_health.c ny_manifest.c ny_module.c
CSRCS  += ny_package.c
CSRCS  += ny_permission.c
CSRCS  += ny_provider.c
CSRCS  += ny_runtime.c ny_revocation.c ny_scheduler.c
CSRCS  += ny_wasm.c
ifeq ($(CONFIG_NYABULA_CORE_AGENT),y)
CSRCS += ny_agent.c ny_agent_capabilities.c ny_agent_config.c ny_agent_skills.c ny_agent_automation.c ny_agent_mcp_wire.c ny_agent_mcp.c ny_agent_mcp_in.c
CSRCS += ny_agent_profile.c ny_agent_channels.c ny_agent_builtin.c ny_agent_weixin.c ny_agent_feishu.c ny_agent_node.c
CSRCS += ny_agent_local.c ny_agent_local_intent.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
CSRCS += ny_agent_mcp_server.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/src
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
endif
ifeq ($(CONFIG_NYABULA_CORE_WEB),y)
CSRCS += ny_web.c ny_web_auth.c ny_web_http.c ny_websocket.c
CSRCS += ny_web_mirror.c
endif

ifeq ($(CONFIG_NYABULA_CORE_PRODUCT),y)
CSRCS += ny_product_store.c ny_product_records.c ny_product_cli.c ny_product.c
CSRCS += ny_product_clock.c ny_sntp.c
CSRCS += ny_product_timers.c ny_product_runtime.c ny_product_eyes.c ny_product_device.c ny_product_media.c ny_product_maintenance.c
CSRCS += ny_product_alarms.c ny_product_notifications.c ny_product_briefing.c
ifeq ($(CONFIG_NYABULA_CORE_AUDIO),y)
CSRCS += ny_product_audio.c
endif
ifeq ($(CONFIG_NYABULA_CORE_LIGHT),y)
CSRCS += ny_product_light.c ny_light_math.c
endif
ifeq ($(CONFIG_NYABULA_CORE_BT),y)
# The Bluetooth host exports no include path of its own; these are the ones
# its public headers need.  The SBC library adds its own through Make.defs.
CSRCS += ny_product_bt.c ny_product_bt_audio.c ny_audio_stream.c ny_pcm.c ny_sbc.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/zblue/zblue/port/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/zblue/zblue/port/include/zephyr
endif
ifeq ($(CONFIG_NYABULA_CORE_NETWORK),y)
CSRCS += ny_product_network.c
endif
ifeq ($(CONFIG_NYABULA_CORE_TIMESYNC),y)
CSRCS += ny_product_timesync.c
endif
ifeq ($(CONFIG_NYABULA_CORE_AGENT),y)
CSRCS += ny_product_companion.c
endif
ifeq ($(CONFIG_NYABULA_CORE_WEATHER),y)
CSRCS += ny_product_weather.c
endif
ifeq ($(CONFIG_NYABULA_CORE_MODELS),y)
CSRCS += ny_web_models.c
endif
ifeq ($(CONFIG_NYABULA_CORE_OTA),y)
# nbootctl lives where the manifest links it, not beside this directory:
# "../nbootctl" is only true in the team repository checkout.
NYCORE_NBOOTCTL_DIR = $(APPDIR)/system/nbootctl
CSRCS += ny_web_ota.c
CFLAGS += ${INCDIR_PREFIX}$(NYCORE_NBOOTCTL_DIR)
# A flat build links every built-in application into one image, so when the
# nbootctl command is built in its bootctrl and partition objects already
# provide these symbols.  Compiling them a second time here would put two
# definitions of every nbootctl_* function into the same link.
ifneq ($(CONFIG_SYSTEM_NBOOTCTL),y)
CSRCS += nbootctl_bootctrl.c nbootctl_part.c
VPATH += :$(NYCORE_NBOOTCTL_DIR)
DEPPATH += --dep-path $(NYCORE_NBOOTCTL_DIR)
endif
endif
endif

ifeq ($(CONFIG_NYABULA_CORE_COMPUTE),y)
# The wire codec is shared with the Linux half of the link and lives with it
# in the team repository.  This directory is reached through the manifest's
# symbolic link, so the path is taken from the resolved location.
NYCORE_NYAMP_DIR = $(realpath $(CURDIR))/../../tools/amp/protocol
CSRCS += ny_compute.c
CFLAGS += ${INCDIR_PREFIX}$(NYCORE_NYAMP_DIR)
# A flat build links every built-in application into one image: when the
# nyampctl diagnostic is built in, its copy of the codec already provides
# these symbols.
ifneq ($(CONFIG_EXAMPLES_NYAMPCTL),y)
CSRCS += nyamp_protocol.c
VPATH += :$(NYCORE_NYAMP_DIR)
DEPPATH += --dep-path $(NYCORE_NYAMP_DIR)
endif
ifeq ($(CONFIG_NYABULA_CORE_VOICE),y)
# The voice chain: wake word, recognition and speech run on the compute
# domain; these own the microphone, the speaker and the turn.
CSRCS += ny_voice.c ny_voice_audio.c ny_voice_dsp.c ny_voice_sm.c ny_voice_wire.c
endif
endif

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
