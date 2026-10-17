/****************************************************************************
 * drivers/drivers/sv6621/sv6621_network.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_NETWORK_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_NETWORK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NET

#include <nuttx/net/netdev.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621.h"
#include "sv6621_command.h"
#include "sv6621_data.h"
#include "sv6621_ioctl.h"
#include "sv6621_offload.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_NETWORK_RX_DEPTH           32
#define SV6621_NETWORK_FORWARD_DEPTH      8
#define SV6621_NETWORK_MULTICAST_CAPACITY 32

/****************************************************************************

 * * Public Types

 * ****************************************************************************/

typedef int (*sv6621_network_tx_resolver_t)(
    FAR const uint8_t *frame, size_t length,
    FAR struct sv6621_data_tx_context_s *context, FAR void *arg);

struct sv6621_network_s
{
  struct net_driver_s dev;
  spinlock_t lock;
  struct work_s rx_work;
  struct work_s tx_work;
  struct work_s forward_work;
  struct work_s multicast_work;
  FAR struct sv6621_data_s *data;
  FAR struct sv6621_command_engine_s *command;
  struct sv6621_ioctl_s ioctl;
  struct sv6621_data_tx_context_s tx_context;
  sv6621_network_tx_resolver_t tx_resolver;
  FAR void *tx_resolver_arg;
  struct sv6621_offload_addresses_s applied_addresses;
  bool registered;
  bool interface_up;
  bool link_up;
  bool rx_scheduled;
  bool tx_scheduled;
  bool tx_reschedule;
  bool forward_scheduled;
  bool multicast_scheduled;
  bool addresses_applied;
  uint8_t rx_head;
  uint8_t rx_tail;
  uint8_t forward_head;
  uint8_t forward_tail;
  uint8_t multicast_count;
  uint8_t multicast_limit;
  uint32_t multicast_generation;
  uint32_t multicast_applied_generation;
  uint32_t address_epoch;
  uint8_t applied_address_instance;
  uint8_t multicast[SV6621_NETWORK_MULTICAST_CAPACITY][SV6621_MAC_LENGTH];
  uint16_t rx_length[SV6621_NETWORK_RX_DEPTH];
  uint8_t rx_frame[SV6621_NETWORK_RX_DEPTH][MAX_NETDEV_PKTSIZE];
  uint16_t forward_length[SV6621_NETWORK_FORWARD_DEPTH];
  uint8_t forward_frame[SV6621_NETWORK_FORWARD_DEPTH][MAX_NETDEV_PKTSIZE];
  uint8_t tx_frame[MAX_NETDEV_PKTSIZE] aligned_data(4);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_network_init(FAR struct sv6621_network_s *network,
                        FAR struct sv6621_dev_s *owner,
                        FAR struct sv6621_data_s *data,
                        FAR struct sv6621_command_engine_s *command,
                        uint8_t multicast_limit,
                        FAR const uint8_t mac[SV6621_MAC_LENGTH]);
void sv6621_network_deinit(FAR struct sv6621_network_s *network);
int sv6621_network_sync_multicast(FAR struct sv6621_network_s *network);
int sv6621_network_sync_addresses(FAR struct sv6621_network_s *network);
int sv6621_network_sync_link_addresses(
    FAR struct sv6621_network_s *network,
    FAR const struct sv6621_data_tx_context_s *context);
void sv6621_network_set_link(
    FAR struct sv6621_network_s *network, bool link_up,
    FAR const struct sv6621_data_tx_context_s *context);
void sv6621_network_set_tx_resolver(FAR struct sv6621_network_s *network,
                                    sv6621_network_tx_resolver_t resolver,
                                    FAR void *arg);
void sv6621_network_credit_available(FAR struct sv6621_network_s *network);
int sv6621_network_forward(FAR struct sv6621_network_s *network,
                           FAR const uint8_t *frame, size_t length);
void sv6621_network_input(FAR const struct sv6621_data_rx_s *rx,
                          FAR void *arg);

#endif /* CONFIG_NET */
#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_NETWORK_H */
