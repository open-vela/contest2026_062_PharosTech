/****************************************************************************
 * app/nyabula_core/ny_product_intercom.h
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


#ifndef __APP_NYABULA_CORE_NY_PRODUCT_INTERCOM_H
#define __APP_NYABULA_CORE_NY_PRODUCT_INTERCOM_H
#include "ny_product.h"

struct ny_intercom_client_s
{
  uint64_t id;
  uint64_t revision;
  uint64_t sequence;
};

int ny_product_intercom_request(const struct ny_product_caller_s *caller,
                                 struct ny_intercom_client_s *client,
                                 const char *topic, const cJSON *data,
                                 cJSON **result);
int ny_product_intercom_next(struct ny_intercom_client_s *client,
                              const char **topic, cJSON **result);
void ny_product_intercom_close(struct ny_intercom_client_s *client);
void ny_product_intercom_shutdown(void);
#endif
