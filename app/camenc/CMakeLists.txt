############################################################################
# apps/examples/camenc/CMakeLists.txt
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

if(CONFIG_EXAMPLES_CAMENC)
  # camenc_ws.c embeds web/page.html with an inline .incbin directive.  That
  # resolves its path relative to the compiler's working directory, so point
  # the source directory in to let the same relative name "web/page.html"
  # resolve here as it does in the make build.
  nuttx_add_application(
    MODULE
    ${CONFIG_EXAMPLES_CAMENC}
    NAME
    ${CONFIG_EXAMPLES_CAMENC_PROGNAME}
    SRCS
    camenc_main.c
    camenc_stream.c
    camenc_ws.c
    camenc_3a.c
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/web/page.html
    STACKSIZE
    ${CONFIG_EXAMPLES_CAMENC_STACKSIZE}
    PRIORITY
    ${CONFIG_EXAMPLES_CAMENC_PRIORITY})

  # The page above is a build input of camenc_ws.c that no tool can see.
  # .incbin is an assembler directive inside a C file, so the compiler's own
  # dependency scan finds nothing, and the DEPENDS argument above lists the
  # page for the build system's benefit rather than the object's.
  #
  # Without this, editing web/page.html changes nothing: the page is compiled
  # in, and the object that carries it is only rebuilt when its C source
  # moves.  The firmware then keeps serving the previous page, which looks
  # exactly like a browser holding on to a cached one and is not.
  set_property(
    SOURCE camenc_ws.c
    APPEND
    PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/page.html)
endif()
