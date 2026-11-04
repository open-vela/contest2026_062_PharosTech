/****************************************************************************
 * app/nyabula_core/ny_product_privacy.h
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

#ifndef __APP_NYABULA_CORE_NY_PRODUCT_PRIVACY_H
#define __APP_NYABULA_CORE_NY_PRODUCT_PRIVACY_H

#include <nuttx/config.h>
#include <stdbool.h>

#ifdef CONFIG_NYABULA_CORE_PRODUCT
#include "ny_product.h"
bool ny_product_privacy_camera_allowed(void);
bool ny_product_privacy_mic_allowed(void);
int ny_product_privacy_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result);
#else
static inline bool ny_product_privacy_camera_allowed(void) { return true; }
static inline bool ny_product_privacy_mic_allowed(void) { return true; }
#endif
#endif
