/****************************************************************************
 * app/nyabula_core/ny_camera_png.h
 *
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
 *
 ****************************************************************************/

#ifndef __APP_NYABULA_CORE_NY_CAMERA_PNG_H
#define __APP_NYABULA_CORE_NY_CAMERA_PNG_H
#include <stddef.h>
#include <stdint.h>

/* Encode studio-range Rec.601 NV12, as produced by rk3576_vicap. The output
 * belongs to the caller (free). factor is 1 for a photo, 2 for a vision input.
 */

int ny_camera_png(const uint8_t *nv12, size_t size, uint32_t width,
                  uint32_t height, uint32_t stride, unsigned int factor,
                  uint8_t **output, size_t *length);
#endif
