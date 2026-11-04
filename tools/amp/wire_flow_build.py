#!/usr/bin/env python3
# tools/amp/wire_flow_build.py
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

"""Build wire-test harnesses against the daemon's production dependency graph."""

from pathlib import Path
import subprocess


def nyamp_build_wire_harness(repo, root, sources):
    """Link fixture sources/objects to nyampd_core without copying its sources."""
    daemon = Path(repo) / 'tools/amp/nyampd'
    source_paths = '\n'.join(f'  [=[{Path(p).as_posix()}]=]' for p in sources)
    (root / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.16)\n'
        'project(nyamp_wire_flow LANGUAGES C CXX)\n'
        f'add_subdirectory([=[{daemon.as_posix()}]=] daemon EXCLUDE_FROM_ALL)\n'
        f'add_executable(harness\n{source_paths}\n)\n'
        'target_compile_features(harness PRIVATE cxx_std_17)\n'
        'target_compile_options(harness PRIVATE -Wall -Wextra -Werror)\n'
        'target_link_libraries(harness PRIVATE nyampd_core)\n',
        encoding='utf-8')
    build = root / 'build'
    subprocess.run(['cmake', '-S', str(root), '-B', str(build)], check=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'harness',
                    '-j2'], check=True)
    return build / 'harness'
