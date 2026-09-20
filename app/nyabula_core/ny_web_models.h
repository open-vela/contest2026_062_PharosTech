/****************************************************************************
 * app/nyabula_core/ny_web_models.h
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

#ifndef __NYABULA_CORE_NY_WEB_MODELS_H
#define __NYABULA_CORE_NY_WEB_MODELS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Where model files live.  They are third-party files the owner brings: the
 * firmware ships none, and nothing outside this directory is ever touched.
 */

#define NY_WEB_MODELS_ROOT "/data/models"

/* The volume that directory is on, for the space that is left. */

#define NY_WEB_MODELS_VOLUME "/data"

/* The longest "<kind>/<relative name>" a client may name. */

#define NY_WEB_MODELS_PATH_MAX 96

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Named by their tags, so that the transport that only hands requests on
 * does not need the product and JSON headers for it.
 */

struct cJSON;
struct ny_product_caller_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Whether a request head is for this module: its target is under /models. */

bool ny_web_models_claims(const char *head);

/* Answer one request under /models and return.  body and body_length are
 * the part of the request body that was read together with the head.
 */

int ny_web_models_serve(int fd, const char *head, const void *body,
                        size_t body_length, const char *pair_token);

/* The models.* topics of the authenticated socket.  -ENOSYS for any other
 * topic, like every other product module.
 */

int ny_web_models_request(const struct ny_product_caller_s *caller,
                          const char *topic, const struct cJSON *data,
                          struct cJSON **result);

#endif /* __NYABULA_CORE_NY_WEB_MODELS_H */
