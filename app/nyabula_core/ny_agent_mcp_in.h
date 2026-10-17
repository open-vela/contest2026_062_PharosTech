/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_mcp_in.h
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

#ifndef __NYABULA_CORE_NY_AGENT_MCP_IN_H
#define __NYABULA_CORE_NY_AGENT_MCP_IN_H

#include "ny_product.h"

int ny_agent_mcp_in(const struct ny_product_caller_s *caller,
                    const char *topic, const cJSON *data, cJSON **result);

/* Private transport entry. Returns an HTTP status, never a Core owner role.
 * Only the fixed read-only tool table can reach product services.
 */
int ny_mcp_in_rpc(const char *token, const cJSON *request, cJSON **reply);
int ny_mcp_in_serve(int argc, char **argv);
unsigned int ny_mcp_in_listener(void);
bool ny_mcp_in_active(const char *principal);
char *ny_mcp_in_model_tools(const char *principal);
int ny_mcp_in_read(const char *principal, const char *topic, cJSON **result);

#endif
