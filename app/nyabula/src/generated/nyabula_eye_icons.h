/****************************************************************************
 * app/nyabula/src/generated/nyabula_eye_icons.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#ifndef __APP_NYABULA_SRC_GENERATED_NYABULA_EYE_ICONS_H
#define __APP_NYABULA_SRC_GENERATED_NYABULA_EYE_ICONS_H

#include <stdint.h>

enum nyabula_eye_icon_operation_e
{
  NYABULA_EYE_ICON_MOVE = 0,
  NYABULA_EYE_ICON_LINE,
  NYABULA_EYE_ICON_QUAD,
  NYABULA_EYE_ICON_CUBIC,
  NYABULA_EYE_ICON_CLOSE
};

struct nyabula_eye_icon_command_s
{
  uint8_t operation;
  int32_t x1;
  int32_t y1;
  int32_t x2;
  int32_t y2;
  int32_t x3;
  int32_t y3;
};

struct nyabula_eye_icon_s
{
  const char *name;
  const struct nyabula_eye_icon_command_s *commands;
  uint16_t command_count;
  uint32_t width;
  uint32_t height;
};

const struct nyabula_eye_icon_s *nyabula_eye_icon_find(const char *name);

#endif
