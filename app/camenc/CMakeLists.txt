############################################################################
# apps/system/camenc/CMakeLists.txt
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

# The muxer is built as a library of its own so that it can be reused, and
# so that its independence from the application around it is visible in the
# build rather than only asserted in a comment.  It depends on nothing but
# the C library, which is what lets the same file be compiled on a host.

if(CONFIG_SYSTEM_CAMENC)
  nuttx_add_application(
    MODULE
    ${CONFIG_SYSTEM_CAMENC}
    NAME
    ${CONFIG_SYSTEM_CAMENC_PROGNAME}
    SRCS
    camenc_main.c
    camenc_stream.c
    camenc_ws.c
    STACKSIZE
    ${CONFIG_SYSTEM_CAMENC_STACKSIZE}
    PRIORITY
    ${CONFIG_SYSTEM_CAMENC_PRIORITY})
endif()
