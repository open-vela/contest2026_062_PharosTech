/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent.h
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

#ifndef __NYABULA_CORE_NY_AGENT_H
#define __NYABULA_CORE_NY_AGENT_H

#include "ny_product.h"

int ny_agent_request(const struct ny_product_caller_s *caller,
                     const char *topic, const cJSON *data, cJSON **result);
cJSON *ny_agent_capabilities(void);
int ny_agent_builtin(const char *topic, const cJSON *data, cJSON **result);
int ny_agent_channels(const char *topic, const cJSON *data, cJSON **result);
int ny_agent_weixin(const char *topic, const cJSON *data, cJSON **result);
void ny_agent_weixin_tick(void);
int ny_agent_feishu(const char *topic, const cJSON *data, cJSON **result);
void ny_agent_feishu_tick(void);
int ny_agent_node(const char *topic, const cJSON *data, cJSON **result);
void ny_agent_node_tick(void);
void ny_agent_channels_tick(void);
void ny_agent_mqtt_receive(const char *chat, const char *request,
                           const char *text);
void ny_agent_channel_receive(const char *channel, const char *chat,
                              const char *request, const char *context,
                              const char *text);
int ny_agent_channel_send(const char *channel, const char *chat,
                          const char *context, const char *text);
int ny_agent_profile(const char *topic, const cJSON *data, cJSON **result);
int ny_agent_skills(const char *topic, const cJSON *data, cJSON **result);
int ny_agent_context(char *buf, size_t size);
int ny_agent_automation(const struct ny_product_caller_s *caller,
                        const char *topic, const cJSON *data, cJSON **result);
int ny_agent_automation_tick(void);
int ny_agent_config(const struct ny_product_caller_s *caller,
                    const char *topic, const cJSON *data, cJSON **result);

/* Private ingress entry; the transport must hold its authorization lock. */
int ny_agent_remote_request(const char *principal, const char *topic,
                            const cJSON *data, cJSON **result);

#endif
