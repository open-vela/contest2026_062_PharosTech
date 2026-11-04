/****************************************************************************
 * app/nyabula_core/ny_product_camera.h
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


#ifndef __APP_NYABULA_CORE_NY_PRODUCT_CAMERA_H
#define __APP_NYABULA_CORE_NY_PRODUCT_CAMERA_H

#include "ny_product.h"

struct ny_camera_client_s
{
  bool active;
  uint32_t generation;
  uint64_t sequence;
  size_t offset;
};

/* Session-owned subscriptions: close on disconnect, logout or revocation.
 * next returns an owned JSON chunk, -EAGAIN if no frame is ready, or a
 * terminal capture error. Only the worker touches V4L2/DMA resources.
 */

int ny_product_camera_subscribe(struct ny_camera_client_s *client);
void ny_product_camera_close(struct ny_camera_client_s *client);
int ny_product_camera_next(struct ny_camera_client_s *client, cJSON **chunk);
int ny_product_camera_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result);
int ny_product_camera_photo(unsigned int factor, uint8_t **png,
                             size_t *length);
/* Copy the newest cached raw frame, never a DMA-owned buffer. The caller
 * holds a subscription and returns EAGAIN while the next frame is pending.
 */
int ny_product_camera_raw_next(struct ny_camera_client_s *client,
                               uint8_t *pixels, size_t capacity,
                               uint64_t *sequence, uint64_t *timestamp);
void ny_product_camera_shutdown(void);

#endif
