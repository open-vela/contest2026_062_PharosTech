/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_mcp_wire.h
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

#ifndef __NYABULA_CORE_NY_AGENT_MCP_WIRE_H
#define __NYABULA_CORE_NY_AGENT_MCP_WIRE_H

#include <netutils/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#define NY_MCP_PROTOCOL      "2025-11-25"
#define NY_MCP_WIRE_CAPACITY 16384

struct ny_mcp_peer_s
{
  char host[254];
  char port[6];
  char path[513];
  char bearer[520];
  char session[129];
  unsigned int sequence;
  bool fixture;
  bool tools;
  bool resources;
  bool prompts;
};

int ny_mcp_peer_configure(struct ny_mcp_peer_s *peer, const char *url,
                          const char *secret);
int ny_mcp_peer_initialize(struct ny_mcp_peer_s *peer);
int ny_mcp_peer_catalog(struct ny_mcp_peer_s *peer, cJSON **catalog);
int ny_mcp_peer_request(struct ny_mcp_peer_s *peer, const char *method,
                        const cJSON *params, cJSON **result);

#endif
