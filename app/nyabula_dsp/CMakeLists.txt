# app/nyabula_dsp/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with this
# work for additional information regarding copyright ownership. The ASF
# licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations
# under the License.
# Standalone host build; this stage does not register a NuttX application.
cmake_minimum_required(VERSION 3.16)
project(nyadsp VERSION 0.1.0 LANGUAGES C)

add_library(nyadsp SHARED nyadsp.c)
target_include_directories(nyadsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(nyadsp PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED YES WINDOWS_EXPORT_ALL_SYMBOLS YES)
if(NOT MSVC)
  target_compile_options(nyadsp PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)
  target_link_libraries(nyadsp PRIVATE m)
endif()

include(CTest)
option(NYADSP_SANITIZE_TEST "Run native boundary tests with ASan/UBSan" OFF)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(nyadsp_boundary_test tests/test_core.c nyadsp.c)
  target_include_directories(nyadsp_boundary_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(nyadsp_boundary_test PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES)
  if(NOT MSVC)
    target_compile_options(nyadsp_boundary_test PRIVATE
      -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)
    target_link_libraries(nyadsp_boundary_test PRIVATE m)
    if(NYADSP_SANITIZE_TEST)
      target_compile_options(nyadsp_boundary_test PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(nyadsp_boundary_test PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME dsp_boundaries COMMAND nyadsp_boundary_test)
  add_test(NAME dsp_response_and_wav
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/nyabula_dsp/test_dsp.py"
      "$<TARGET_FILE:nyadsp>")
endif()
