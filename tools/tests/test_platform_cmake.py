#!/usr/bin/env python3
# tools/tests/test_platform_cmake.py
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Exercise the K7 platform entry without a target toolchain or firmware."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(k7_platform_contract C)
add_library(drivers STATIC EXCLUDE_FROM_ALL)
add_library(board STATIC EXCLUDE_FROM_ALL)
add_library(nuttx INTERFACE)
function(nuttx_add_subdirectory)
  file(GLOB entries RELATIVE "${CMAKE_CURRENT_LIST_DIR}"
       "${CMAKE_CURRENT_LIST_DIR}/*/CMakeLists.txt")
  foreach(entry ${entries})
    get_filename_component(directory "${entry}" DIRECTORY)
    add_subdirectory("${directory}")
  endforeach()
endfunction()
set(CONFIG_NYABULA_CUSTOM_DRIVERS y)
set(CONFIG_LCD_ST77916 y)
set(CONFIG_RTC_PCF8563 y)
set(CONFIG_SV6621 y)
set(NUTTX_BOARD_DIR "${TEAM_ROOT}/boards/rk3576/kickpi-k7")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/drivers")
if(EMPTY_KCONFIG)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/drivers/platform")
  file(WRITE "${CMAKE_BINARY_DIR}/drivers/platform/Kconfig" "")
else()
  file(CREATE_LINK "${TEAM_ROOT}/drivers"
       "${CMAKE_BINARY_DIR}/drivers/platform" SYMBOLIC)
endif()
if(MAKE_ARTIFACT)
  add_subdirectory("${TEAM_ROOT}/drivers" "make-platform")
endif()
add_subdirectory("${NUTTX_BOARD_DIR}" "board")
get_target_property(sources drivers SOURCES)
foreach(expected st77916.c pcf8563.c sv6621_core.c)
  set(count 0)
  foreach(source ${sources})
    get_filename_component(name "${source}" NAME)
    if(name STREQUAL expected)
      math(EXPR count "${count} + 1")
    endif()
  endforeach()
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "${expected} registered ${count} times")
  endif()
endforeach()
'''


class PlatformCMakeTest(unittest.TestCase):
    def configure(self, *options):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / 'CMakeLists.txt').write_text(PROJECT, encoding='utf-8')
            return subprocess.run(
                ['cmake', '-S', str(source), '-B', str(source / 'out'),
                 f'-DTEAM_ROOT={ROOT.as_posix()}', *options],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_fresh_source_tree_registers_drivers(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_make_artifact_does_not_duplicate_drivers(self):
        result = self.configure('-DMAKE_ARTIFACT=ON')
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_empty_platform_kconfig_is_rejected(self):
        result = self.configure('-DEMPTY_KCONFIG=ON')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('K7 platform Kconfig is not bound', result.stdout)


if __name__ == '__main__':
    unittest.main()
