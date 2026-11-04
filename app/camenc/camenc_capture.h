/****************************************************************************
 * app/camenc/camenc_capture.h
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

#ifndef __APP_CAMENC_CAMENC_CAPTURE_H
#define __APP_CAMENC_CAMENC_CAPTURE_H

#include "camenc_stream.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All callbacks run on the capture owner's task. Buffers are borrowed until
 * the callback returns; consumers must copy, never retain a DMA pointer.
 * Callbacks must not wait for a network peer or for inference.
 */

struct camenc_capture_s
{
  uint32_t mode;
  int frames;
  int qp;
  int gop;
  const char *file;
  void *arg;
  camenc_emit_fn emit;
  int (*frame)(void *arg, const uint8_t *nv12, size_t size, uint32_t width,
               uint32_t height, uint32_t stride);
  bool (*stop)(void *arg);
};

/* Blocking run, without a listening socket. Returns a negative errno on
 * failure, or -EBUSY if the demo or another owner already has the camera.
 * A stop request finishes the in-flight encode before releasing the stream,
 * mappings and device descriptors.
 */

int camenc_capture_run(const struct camenc_capture_s *capture);

#endif
