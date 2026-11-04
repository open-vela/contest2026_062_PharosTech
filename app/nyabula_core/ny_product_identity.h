/****************************************************************************
 * app/nyabula_core/ny_product_identity.h
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

#ifndef __APP_NYABULA_CORE_NY_PRODUCT_IDENTITY_H
#define __APP_NYABULA_CORE_NY_PRODUCT_IDENTITY_H
#include "ny_product.h"
#define NY_IDENTITY_DIM 128
void ny_product_identity_observe(const int16_t *feature, unsigned int faces,
                                 uint64_t timestamp);
void ny_product_identity_clear_live(void);
void ny_product_identity_faces(unsigned int faces, uint64_t timestamp);
int ny_product_identity_request(const struct ny_product_caller_s *caller,
                                const char *topic, const cJSON *data,
                                cJSON **result);
#endif
