# ##############################################################################
# packages/demos/contest2026_062_nyabula_core/CMakeLists.txt
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

if(CONFIG_CONTEST2026_062_NYABULA_CORE)
  set(NYABULA_CORE_SRCS
      nycore_main.c
      ny_broker.c
      ny_capability.c
      ny_health.c
      ny_manifest.c
      ny_module.c
      ny_package.c
      ny_permission.c
      ny_provider.c
      ny_runtime.c
      ny_revocation.c
      ny_scheduler.c
      ny_wasm.c)
  set(NYABULA_CORE_DEPENDS libqjs)
  if(CONFIG_NYABULA_CORE_AGENT)
    list(APPEND NYABULA_CORE_SRCS ny_agent.c ny_agent_capabilities.c ny_agent_config.c ny_agent_skills.c ny_agent_automation.c ny_agent_mcp_wire.c ny_agent_mcp.c ny_agent_mcp_in.c)
    list(APPEND NYABULA_CORE_SRCS ny_agent_profile.c ny_agent_channels.c ny_agent_builtin.c ny_agent_weixin.c ny_agent_feishu.c ny_agent_node.c)
    list(APPEND NYABULA_CORE_SRCS ny_product_companion.c)
    list(APPEND NYABULA_CORE_DEPENDS apps_ai_agent libsodium)
    list(APPEND NYABULA_CORE_SRCS ny_agent_mcp_server.c)
    set(NYABULA_AGENT_INCLUDES ${NUTTX_APPS_DIR}/../packages/ai_agent/include
        ${NUTTX_APPS_DIR}/../packages/ai_agent/src
        ${NUTTX_APPS_DIR}/netutils/cjson/cJSON)
  endif()
  if(CONFIG_NYABULA_CORE_WEB)
    list(APPEND NYABULA_CORE_SRCS ny_web.c ny_web_http.c ny_websocket.c)
  endif()
  if(CONFIG_NYABULA_CORE_PRODUCT)
    list(APPEND NYABULA_CORE_SRCS ny_product_store.c ny_product_records.c
         ny_product_cli.c ny_product.c ny_product_timers.c ny_product_runtime.c
         ny_product_alarms.c ny_product_notifications.c ny_product_briefing.c
         ny_product_eyes.c ny_product_device.c ny_product_media.c)
    if(CONFIG_NYABULA_CORE_NETWORK)
      list(APPEND NYABULA_CORE_SRCS ny_product_network.c)
    endif()
  endif()
  if(CONFIG_NYABULA_CORE_EYE)
    list(APPEND NYABULA_CORE_DEPENDS nyabula_eye)
  endif()
  if(CONFIG_NYABULA_CORE_WEATHER)
    list(APPEND NYABULA_CORE_SRCS ny_product_weather.c)
  endif()

  if(CONFIG_NYABULA_CORE_HTTP)
    list(APPEND NYABULA_CORE_SRCS ny_http.c ny_broker_http.c)
  endif()

  if(CONFIG_NYABULA_CORE_STATE_SQLITE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(NYCORE_SQLITE_DIR ${CMAKE_CURRENT_BINARY_DIR}/sqlite)
    execute_process(
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/prepare_sqlite.py
              --output ${NYCORE_SQLITE_DIR}
              --cache ${CMAKE_CURRENT_LIST_DIR}/.sqlite
      RESULT_VARIABLE NYCORE_SQLITE_RESULT)
    if(NOT NYCORE_SQLITE_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to prepare pinned SQLite sources")
    endif()
    nuttx_add_library(nycore_sqlite STATIC ${NYCORE_SQLITE_DIR}/sqlite3.c)
    target_include_directories(nycore_sqlite PUBLIC ${NYCORE_SQLITE_DIR})
    target_compile_definitions(nycore_sqlite PRIVATE
      SQLITE_OS_UNIX=1 SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION
      SQLITE_OMIT_WAL SQLITE_MAX_MMAP_SIZE=0 SQLITE_TEMP_STORE=3
      SQLITE_DEFAULT_MEMSTATUS=0)
    target_compile_options(nycore_sqlite PRIVATE -Wno-unused-variable
      -Wno-undef -Wno-unused-function -Wno-shadow)
    list(APPEND NYABULA_CORE_SRCS ny_state.c)
    list(APPEND NYABULA_CORE_DEPENDS nycore_sqlite)
  endif()

  if(CONFIG_NYABULA_CORE_WAMR)
    list(APPEND NYABULA_CORE_DEPENDS wamr)
  endif()

  if(CONFIG_NYABULA_CORE_REQUIRE_SIGNATURE)
    list(APPEND NYABULA_CORE_SRCS ny_signature.c)
    list(APPEND NYABULA_CORE_DEPENDS libsodium)
  endif()

  nuttx_add_application(
    NAME
    nycore
    PRIORITY
    ${CONFIG_NYABULA_CORE_PRIORITY}
    STACKSIZE
    ${CONFIG_NYABULA_CORE_STACKSIZE}
    SRCS
    ${NYABULA_CORE_SRCS}
    INCLUDE_DIRECTORIES
    ${NYABULA_AGENT_INCLUDES}
    ${NYCORE_SQLITE_DIR}
    ${NUTTX_APPS_DIR}/crypto/libsodium/libsodium/src/libsodium/include
    ${NUTTX_APPS_DIR}/interpreters/wamr/wamr/core/iwasm/include
    DEPENDS
    ${NYABULA_CORE_DEPENDS})

  if(CONFIG_NYABULA_CORE_WEB)
    nuttx_add_application(NAME nyabula_web PRIORITY 100 STACKSIZE 16384
      SRCS ny_web_main.c DEPENDS apps_nycore)
    if(CONFIG_NYABULA_EYE_WS)
      nuttx_add_application(NAME nyabula_eye_ws PRIORITY 100 STACKSIZE 16384
        SRCS ny_eye_ws_compat_main.c DEPENDS apps_nycore)
    endif()
  endif()
  if(CONFIG_NYABULA_CORE_AGENT)
    nuttx_add_application(NAME nyabula_mcp PRIORITY 100 STACKSIZE 16384
      SRCS ny_mcp_main.c DEPENDS apps_nycore)
  endif()
endif()
