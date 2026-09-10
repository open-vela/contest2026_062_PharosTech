# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(nycore_wamr_configuration C)

set(NYCORE_TEST_CONTEXT "1" CACHE STRING "WAMR-owned context setting")
if(NOT NYCORE_TEST_CONTEXT MATCHES "^[01]$")
  message(FATAL_ERROR "NYCORE_TEST_CONTEXT must be 0 or 1")
endif()

set(CONFIG_CONTEST2026_062_NYABULA_CORE ON)
set(CONFIG_NYABULA_CORE_WAMR ON)

# Represent the dependency before loading the real Core build description.
# Both WAMR configurations must remain owned by WAMR, not its consumers.
add_library(wamr STATIC wamr_config.c)
target_compile_definitions(wamr PRIVATE
  WASM_ENABLE_MODULE_INST_CONTEXT=${NYCORE_TEST_CONTEXT}
  NYCORE_TEST_EXPECTED_CONTEXT=${NYCORE_TEST_CONTEXT})
target_compile_options(wamr PRIVATE -Werror)

function(nuttx_add_application)
  cmake_parse_arguments(APP "" "" "DEPENDS" ${ARGN})
  if(NOT "wamr" IN_LIST APP_DEPENDS)
    message(FATAL_ERROR "Core must depend on the WAMR target")
  endif()
endfunction()

include(${CMAKE_CURRENT_LIST_DIR}/../../../../app/nyabula_core/CMakeLists.txt)
