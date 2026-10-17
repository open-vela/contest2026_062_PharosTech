# ##############################################################################
# apps/system/nbootctl/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
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
# ##############################################################################

if(CONFIG_SYSTEM_NBOOTCTL)
  set(NBOOTCTL_SRCS nbootctl_main.c nbootctl_bootctrl.c nbootctl_part.c)

  # The formatter is mkfatfs's.  A configuration without it keeps every other
  # command and leaves the board's weak format hook answering -ENOSYS.
  if(CONFIG_FSUTILS_MKFATFS)
    list(APPEND NBOOTCTL_SRCS nbootctl_format.c)
  endif()

  nuttx_add_application(
    MODULE ${CONFIG_SYSTEM_NBOOTCTL}
    NAME ${CONFIG_SYSTEM_NBOOTCTL_PROGNAME}
    SRCS ${NBOOTCTL_SRCS}
    STACKSIZE ${CONFIG_SYSTEM_NBOOTCTL_STACKSIZE}
    HEAPSIZE ${CONFIG_SYSTEM_NBOOTCTL_HEAPSIZE}
    PRIORITY ${CONFIG_SYSTEM_NBOOTCTL_PRIORITY})
endif()
