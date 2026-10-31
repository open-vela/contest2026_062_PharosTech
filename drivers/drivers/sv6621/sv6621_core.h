/****************************************************************************
 * drivers/drivers/sv6621/sv6621_core.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CORE_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CORE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/clock.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#ifdef CONFIG_SV6621_PM
#include <nuttx/power/pm.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#endif

#include "sv6621.h"
#include "sv6621_ap.h"
#ifdef CONFIG_SV6621_BLUETOOTH
#include "sv6621_bluetooth.h"
#endif
#include "sv6621_command.h"
#include "sv6621_data.h"
#include "sv6621_network.h"
#include "sv6621_packet.h"
#include "sv6621_power.h"
#include "sv6621_regulatory.h"
#include "sv6621_rx.h"
#include "sv6621_scan.h"
#include "sv6621_sched_scan.h"
#include "sv6621_service.h"
#include "sv6621_signal.h"
#include "sv6621_station.h"
#include "sv6621_stats.h"
#include "sv6621_tx.h"
#include "sv6621_wifi.h"
#include "sv6621_wpa_handshake.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_CORE_SECURITY_EVENT_DEPTH 8
#define SV6621_CORE_SIGNAL_EVENT_DEPTH   8

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_dev_s
{
  struct sv6621_config_s config;
  mutex_t lifecycle_lock;
  mutex_t status_lock;
  struct sv6621_status_s status;
  struct sv6621_packet_router_s router;
  struct sv6621_tx_s tx;
  struct sv6621_data_s data;
#ifdef CONFIG_NET
  struct sv6621_network_s network;
#endif
  struct sv6621_command_engine_s command;
  struct sv6621_ap_s ap;
  struct sv6621_scan_s scan;
  struct sv6621_sched_scan_s scheduled_scan;
  struct sv6621_station_s station;
  struct sv6621_wpa_s wpa;
  struct sv6621_service_s service;
  struct sv6621_rx_s rx;
  struct sv6621_wifi_info_s wifi_info;
  struct sv6621_regulatory_domain_s regulatory;
  struct sv6621_scan_channel_s
      scan_channels[SV6621_REGULATORY_SCAN_CHANNEL_CAPACITY];
  size_t scan_channel_count;
  struct work_s event_work;
  struct work_s thermal_work;
  struct work_s security_work;
  struct work_s signal_work;
  struct work_s scan_work;
  struct work_s station_work;
  uint32_t fatal_generation;
  bool fatal_work_scheduled;
  int scan_result;
  uint16_t station_reason;
  uint32_t station_generation;
  bool station_connected;
  bool station_remote_disconnect;
  bool station_work_scheduled;
  bool scan_reporting;
  sem_t recovery_sem;
  sem_t recovery_exit_sem;
  bool recovery_pending;
  bool recovery_running;
  bool recovery_shutdown;
  sem_t roam_sem;
  sem_t roam_exit_sem;
  bool roam_shutdown;
  uint32_t thermal_generation;
  bool thermal_blocked;
  bool thermal_work_scheduled;
  struct sv6621_mic_failure_s
      security_events[SV6621_CORE_SECURITY_EVENT_DEPTH];
  uint8_t security_head;
  uint8_t security_tail;
  bool security_work_scheduled;
  struct sv6621_signal_event_s signal_events[SV6621_CORE_SIGNAL_EVENT_DEPTH];
  uint8_t signal_head;
  uint8_t signal_tail;
  bool signal_work_scheduled;
  uint32_t roam_scan_generation;
  int16_t roam_scan_signal_dbm;
  bool roam_scan_pending;
  struct sv6621_roam_policy_s roam_policy;
  struct sv6621_scan_entry_s roam_candidate;
  uint32_t roam_candidate_generation;
  int16_t roam_candidate_signal_dbm;
  bool roam_candidate_pending;
  clock_t roam_candidate_ticks;
  bool roam_candidate_ticks_valid;
  bool suspended;
  bool powered;
  bool transport_open;
  bool station_open;
  bool ap_initialized;
#ifdef CONFIG_SV6621_PM
  struct pm_callback_s pm_callback;
  struct work_s pm_resume_work;
  spinlock_t pm_lock;
  bool pm_registered;
  bool pm_active;
  bool pm_suspended;
  bool pm_resume_queued;
#endif
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_scan_selected(FAR struct sv6621_dev_s *dev,
                         FAR const struct sv6621_scan_channel_s *channels,
                         size_t channel_count, FAR const uint8_t *ssid,
                         size_t ssid_length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CORE_H */
