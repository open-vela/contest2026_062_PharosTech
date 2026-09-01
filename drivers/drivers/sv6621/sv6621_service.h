/****************************************************************************
 * drivers/drivers/sv6621/sv6621_service.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SERVICE_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SERVICE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621_transport.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_service_event_e
{
  SV6621_SERVICE_EVENT_BSP_READY = 0,
  SV6621_SERVICE_EVENT_WIFI_READY,
  SV6621_SERVICE_EVENT_BT_READY,
  SV6621_SERVICE_EVENT_ASSERT,
  SV6621_SERVICE_EVENT_DUMP_COMPLETE
};

typedef void (*sv6621_service_event_t)(enum sv6621_service_event_e event,
                                       FAR const uint8_t *payload,
                                       size_t length, FAR void *arg);

struct sv6621_service_status_s
{
  bool bsp_ready;
  bool wifi_ready;
  bool bt_ready;
  int failure;
};

struct sv6621_service_s
{
  mutex_t lock;
  sem_t bsp_completion;
  sem_t wifi_completion;
  sem_t bt_completion;
  struct sv6621_service_status_s status;
  sv6621_service_event_t event;
  FAR void *event_arg;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_service_init(FAR struct sv6621_service_s *service,
                        sv6621_service_event_t event, FAR void *event_arg);
void sv6621_service_deinit(FAR struct sv6621_service_s *service);
int sv6621_service_reset(FAR struct sv6621_service_s *service);
int sv6621_service_get_status(FAR struct sv6621_service_s *service,
                              FAR struct sv6621_service_status_s *status);
int sv6621_service_wait_bsp(FAR struct sv6621_service_s *service,
                            uint32_t timeout_ms);
int sv6621_service_start_wifi(FAR struct sv6621_service_s *service,
                              FAR struct sv6621_transport_s *transport,
                              uint32_t timeout_ms);
int sv6621_service_start_bluetooth(FAR struct sv6621_service_s *service,
                                   FAR struct sv6621_transport_s *transport,
                                   uint32_t timeout_ms);
int sv6621_service_stop_bluetooth(FAR struct sv6621_service_s *service,
                                  FAR struct sv6621_transport_s *transport);
void sv6621_service_channel_consumer(uint8_t channel,
                                     FAR const uint8_t encoded[4],
                                     FAR const uint8_t *payload, size_t length,
                                     FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SERVICE_H */
