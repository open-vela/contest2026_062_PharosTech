/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_light.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __NYABULA_CORE_NY_PRODUCT_LIGHT_H
#define __NYABULA_CORE_NY_PRODUCT_LIGHT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_product.h"

#ifdef CONFIG_NYABULA_CORE_LIGHT

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* light.status and light.config; -ENOSYS for any other topic. */

int ny_product_light_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result);

/* Called by the product worker every tick.  Samples a few times a second
 * and returns at once in between; a conversion is a few microseconds.
 */

int ny_product_light_tick(void);
void ny_product_light_shutdown(void);

/* Add "light": {"level", "dilation"} to a sys.info reply when there is a
 * reading.  False only when out of memory.
 */

bool ny_product_light_describe(cJSON *root);

#endif /* CONFIG_NYABULA_CORE_LIGHT */
#endif /* __NYABULA_CORE_NY_PRODUCT_LIGHT_H */
