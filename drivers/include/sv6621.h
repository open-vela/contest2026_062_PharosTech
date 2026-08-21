/****************************************************************************
 * drivers/include/sv6621.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_INCLUDE_SV6621_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_INCLUDE_SV6621_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NET
#include <nuttx/net/ioctl.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_MAC_LENGTH                   6
#define SV6621_SSID_MAX_LENGTH              32
#define SV6621_KEY_MAX_LENGTH               64
#define SV6621_REGULATORY_MAX_RULES         8
#define SV6621_DRIVER_STATS_VERSION         1
#define SV6621_ROAM_DEFAULT_THRESHOLD_DBM   (-70)
#define SV6621_ROAM_DEFAULT_HYSTERESIS_DB   40
#define SV6621_ROAM_DEFAULT_MINIMUM_GAIN_DB 8
#define SV6621_ROAM_DEFAULT_COOLDOWN_MS     15000
#define SV6621_SCHED_SCAN_MAX_SSIDS         10
#define SV6621_SCHED_SCAN_MAX_MATCHES       10
#define SV6621_SCHED_SCAN_MAX_PLANS         4
#define SV6621_SCHED_SCAN_MAX_IE_LENGTH     512

#ifdef CONFIG_NET
#define SV6621IOC_GET_DRIVER_STATS SIOCDEVPRIVATE
#define SV6621IOC_GET_LINK_STATS   (SIOCDEVPRIVATE + 1)
#endif

#define SV6621_REGULATORY_FLAG_NO_OFDM    (1 << 0)
#define SV6621_REGULATORY_FLAG_NO_OUTDOOR (1 << 3)
#define SV6621_REGULATORY_FLAG_DFS        (1 << 4)
#define SV6621_REGULATORY_FLAG_NO_IR      (1 << 7)
#define SV6621_REGULATORY_FLAG_AUTO_BW    (1 << 11)

#define SV6621_WAKE_DISCONNECT            (1 << 0)
#define SV6621_WAKE_MAGIC_PACKET          (1 << 1)
#define SV6621_WAKE_GTK_REKEY_FAILURE     (1 << 2)
#define SV6621_WAKE_EAP_IDENTITY_REQUEST  (1 << 3)
#define SV6621_WAKE_FOUR_WAY_HANDSHAKE    (1 << 4)
#define SV6621_WAKE_RFKILL_RELEASE        (1 << 5)
#define SV6621_WAKE_ALL                   0x003f

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_dev_s;
struct sv6621_transport_s;

typedef void (*sv6621_transport_irq_t)(FAR void *arg);

struct sv6621_transport_ops_s
{
  int (*open)(FAR struct sv6621_transport_s *transport);
  int (*enumerate)(FAR struct sv6621_transport_s *transport);
  void (*close)(FAR struct sv6621_transport_s *transport);
  int (*read_byte)(FAR struct sv6621_transport_s *transport, uint8_t function,
                   uint32_t address, FAR uint8_t *value);
  int (*write_byte)(FAR struct sv6621_transport_s *transport, uint8_t function,
                    uint32_t address, uint8_t value);
  int (*read)(FAR struct sv6621_transport_s *transport, uint8_t function,
              uint32_t address, bool increment, FAR void *buffer,
              size_t length);
  int (*write)(FAR struct sv6621_transport_s *transport, uint8_t function,
               uint32_t address, bool increment, FAR const void *buffer,
               size_t length);
  int (*attach_irq)(FAR struct sv6621_transport_s *transport,
                    sv6621_transport_irq_t handler, FAR void *arg);
  int (*enable_irq)(FAR struct sv6621_transport_s *transport, bool enable);
  int (*ack_irq)(FAR struct sv6621_transport_s *transport);
};

struct sv6621_transport_s
{
  FAR const struct sv6621_transport_ops_s *ops;
  FAR void *priv;
};

enum sv6621_state_e
{
  SV6621_STATE_OFF = 0,
  SV6621_STATE_STARTING,
  SV6621_STATE_BSP_READY,
  SV6621_STATE_WIFI_STARTING,
  SV6621_STATE_WIFI_READY,
  SV6621_STATE_SUSPENDED,
  SV6621_STATE_RECOVERING,
  SV6621_STATE_STOPPING,
  SV6621_STATE_FAILED
};

enum sv6621_security_e
{
  SV6621_SECURITY_OPEN = 0,
  SV6621_SECURITY_WPA2_PSK,
  SV6621_SECURITY_WPA3_SAE,
  SV6621_SECURITY_WPA2_WPA3_PSK,
  SV6621_SECURITY_LEGACY
};

enum sv6621_band_e
{
  SV6621_BAND_2GHZ = 0,
  SV6621_BAND_5GHZ
};

enum sv6621_channel_width_e
{
  SV6621_CHANNEL_WIDTH_20,
  SV6621_CHANNEL_WIDTH_40,
  SV6621_CHANNEL_WIDTH_80,
  SV6621_CHANNEL_WIDTH_80P80,
  SV6621_CHANNEL_WIDTH_160
};

enum sv6621_event_e
{
  SV6621_EVENT_STATE_CHANGED = 0,
  SV6621_EVENT_SCAN_RESULT,
  SV6621_EVENT_SCAN_COMPLETE,
  SV6621_EVENT_CONNECTED,
  SV6621_EVENT_DISCONNECTED,
  SV6621_EVENT_RECOVERY_STARTED,
  SV6621_EVENT_RECOVERY_COMPLETE,
  SV6621_EVENT_THERMAL_CHANGED,
  SV6621_EVENT_MIC_FAILURE,
  SV6621_EVENT_SIGNAL_CHANGED,
  SV6621_EVENT_FATAL,
  SV6621_EVENT_ROAM_CANDIDATE,
  SV6621_EVENT_ROAM_COMPLETE,
  SV6621_EVENT_SCHEDULED_SCAN_RESULTS,
  SV6621_EVENT_AP_STARTED,
  SV6621_EVENT_AP_STOPPED,
  SV6621_EVENT_AP_CLIENT_CONNECTED,
  SV6621_EVENT_AP_CLIENT_DISCONNECTED
};

enum sv6621_signal_status_e
{
  SV6621_SIGNAL_LOW = 1,
  SV6621_SIGNAL_HIGH,
  SV6621_SIGNAL_BEACON_LOSS,
  SV6621_SIGNAL_TDLS_LOSS
};

struct sv6621_signal_event_s
{
  enum sv6621_signal_status_e status;
  int16_t signal_dbm;
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint8_t channel;
  enum sv6621_band_e band;
};

struct sv6621_thermal_s
{
  bool transmit_blocked;
};

struct sv6621_mic_failure_s
{
  uint8_t address[SV6621_MAC_LENGTH];
  uint8_t key_index;
  uint8_t lmac_id;
  bool group_key;
};

struct sv6621_firmware_s
{
  FAR const uint8_t *data;
  size_t length;
};

struct sv6621_regulatory_rule_s
{
  uint8_t start_channel;
  uint8_t channel_span;
  int8_t max_power_dbm;
  int8_t max_antenna_gain_dbi;
  uint32_t flags;
};

struct sv6621_regulatory_domain_s
{
  char country[2];
  uint8_t rule_count;
  struct sv6621_regulatory_rule_s rules[SV6621_REGULATORY_MAX_RULES];
};

struct sv6621_bss_s
{
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  uint8_t channel;
  enum sv6621_band_e band;
  enum sv6621_security_e security;
  int16_t signal_dbm;
};

struct sv6621_roam_candidate_s
{
  struct sv6621_bss_s candidate;
  int16_t current_signal_dbm;
  uint8_t gain_db;
};

struct sv6621_roam_result_s
{
  uint8_t old_bssid[SV6621_MAC_LENGTH];
  uint8_t new_bssid[SV6621_MAC_LENGTH];
  int32_t result;
  int32_t rollback_result;
  bool restored;
};

struct sv6621_roam_policy_s
{
  bool enabled;
  int8_t threshold_dbm;
  uint8_t hysteresis_db;
  uint8_t minimum_gain_db;
  uint32_t cooldown_ms;
};

struct sv6621_sched_scan_ssid_s
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t length;
};

struct sv6621_sched_scan_channel_s
{
  uint8_t number;
  enum sv6621_band_e band;
  bool passive;
};

struct sv6621_sched_scan_match_s
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint16_t ssid_length;
  uint8_t bssid[SV6621_MAC_LENGTH];
  int32_t rssi_threshold_dbm;
};

struct sv6621_sched_scan_plan_s
{
  uint32_t interval_seconds;
  uint32_t iterations;
};

struct sv6621_sched_scan_request_s
{
  uint32_t request_id;
  uint32_t flags;
  int32_t minimum_rssi_dbm;
  uint32_t delay_seconds;
  uint8_t random_address[SV6621_MAC_LENGTH];
  uint8_t random_address_mask[SV6621_MAC_LENGTH];
  bool relative_rssi_set;
  int8_t relative_rssi_db;
  uint8_t scan_width;
  FAR const struct sv6621_sched_scan_ssid_s *ssids;
  size_t ssid_count;
  FAR const struct sv6621_sched_scan_channel_s *channels;
  size_t channel_count;
  FAR const struct sv6621_sched_scan_match_s *matches;
  size_t match_count;
  FAR const struct sv6621_sched_scan_plan_s *plans;
  size_t plan_count;
  FAR const uint8_t *information_elements;
  size_t information_element_length;
};

struct sv6621_connect_s
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  uint8_t bssid[SV6621_MAC_LENGTH];
  bool bssid_valid;
  uint8_t channel;
  enum sv6621_security_e security;
  uint8_t credential[SV6621_KEY_MAX_LENGTH];
  uint8_t credential_length;
};

struct sv6621_ap_config_s
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  uint8_t hidden_ssid;
  uint8_t channel;
  enum sv6621_channel_width_e channel_width;
  uint8_t center_channel1;
  uint8_t center_channel2;
  enum sv6621_band_e band;
  uint16_t beacon_interval;
  uint8_t dtim_period;
  enum sv6621_security_e security;
  bool isolate;
  uint8_t credential[SV6621_KEY_MAX_LENGTH];
  uint8_t credential_length;
};

struct sv6621_ap_client_event_s
{
  uint8_t address[SV6621_MAC_LENGTH];
  uint16_t aid;
  uint16_t reason;
};

struct sv6621_suspend_s
{
  bool wake_enabled;
  uint16_t wake_flags;
};

struct sv6621_tx_rate_s
{
  uint8_t flags;
  uint8_t mcs;
  uint16_t legacy_100kbps;
  uint8_t nss;
  uint8_t bandwidth;
  uint8_t guard_interval;
};

struct sv6621_rx_rate_s
{
  uint8_t mode;
  uint8_t rate;
  uint8_t nss;
  uint8_t bandwidth;
  uint8_t guard_interval;
  uint8_t resource_unit;
  uint8_t dcm;
  uint8_t snr_db;
  uint16_t rssi;
};

struct sv6621_link_stats_s
{
  struct sv6621_tx_rate_s tx;
  struct sv6621_rx_rate_s rx;
  uint32_t tx_bitrate_100kbps;
  int8_t signal_dbm;
  int8_t noise_dbm;
  uint8_t tx_success_percent;
  uint8_t tx_airtime_percent;
  uint8_t rx_airtime_percent;
  uint32_t tx_failed;
};

struct sv6621_driver_stats_s
{
  uint16_t version;
  uint16_t size;
  uint32_t commands;
  uint32_t command_timeouts;
  uint32_t command_cancelled;
  uint32_t command_malformed;
  uint32_t stale_acknowledgements;
  uint32_t missed_events;
  uint32_t rx_interrupts;
  uint32_t rx_bursts;
  uint32_t rx_packets;
  uint32_t rx_malformed_bursts;
  uint32_t rx_transport_errors;
  uint32_t tx_packets;
  uint64_t tx_bytes;
  uint32_t tx_transport_errors;
  uint32_t tx_doorbell_errors;
  uint32_t data_received;
  uint32_t data_received_bytes;
  uint32_t data_malformed;
  uint32_t data_transmitted;
  uint32_t data_transmitted_bytes;
  uint32_t data_transmit_errors;
  uint32_t credit_starvations;
  uint32_t fragments;
  uint32_t fragment_drops;
  uint32_t fragment_pn_drops;
  uint32_t reassembled;
  uint32_t reorder_duplicates;
  uint32_t reorder_stale;
  uint32_t reorder_allocation_failures;
  uint32_t reorder_timeouts;
  uint32_t reorder_schedule_errors;
  uint32_t recovery_count;
  uint32_t unprotected_frames;
  uint32_t mic_failures;
  uint32_t mic_failures_dropped;
  uint32_t signal_events_dropped;
};

struct sv6621_status_s
{
  enum sv6621_state_e state;
  bool connected;
  bool ap_active;
  uint8_t ap_client_count;
  uint8_t mac[SV6621_MAC_LENGTH];
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  uint8_t channel;
  enum sv6621_band_e band;
  int16_t signal_dbm;
  uint32_t recovery_count;
  uint32_t unprotected_frames;
  uint32_t mic_failures;
  uint32_t mic_failures_dropped;
  uint32_t signal_events_dropped;
  int last_error;
};

typedef void (*sv6621_event_t)(FAR struct sv6621_dev_s *dev,
                               enum sv6621_event_e event, FAR const void *data,
                               size_t length, FAR void *arg);

struct sv6621_board_ops_s
{
  int (*power_on)(FAR void *arg);
  void (*power_off)(FAR void *arg);
  int (*load_address)(FAR void *arg, uint8_t address[SV6621_MAC_LENGTH]);
  int (*store_address)(FAR void *arg,
                       FAR const uint8_t address[SV6621_MAC_LENGTH]);
};

struct sv6621_config_s
{
  FAR struct sv6621_transport_s *transport;
  FAR const struct sv6621_board_ops_s *board_ops;
  FAR void *board_arg;
  struct sv6621_firmware_s iram;
  struct sv6621_firmware_s dram;
  struct sv6621_firmware_s nvram;
  struct sv6621_firmware_s calibration;
#ifdef CONFIG_SV6621_BLUETOOTH
  struct sv6621_firmware_s bluetooth_nv;
  int bluetooth_device_id;
#endif
  FAR const struct sv6621_regulatory_domain_s *regulatory;
  FAR const struct sv6621_regulatory_domain_s *regulatory_domains;
  size_t regulatory_domain_count;
#ifdef CONFIG_SV6621_PM
  struct sv6621_suspend_s system_suspend;
#endif
  sv6621_event_t event;
  FAR void *event_arg;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
extern "C" {
#endif

int sv6621_create(FAR const struct sv6621_config_s *config,
                  FAR struct sv6621_dev_s **dev);
void sv6621_destroy(FAR struct sv6621_dev_s *dev);
int sv6621_start(FAR struct sv6621_dev_s *dev);
int sv6621_stop(FAR struct sv6621_dev_s *dev);
int sv6621_get_status(FAR struct sv6621_dev_s *dev,
                      FAR struct sv6621_status_s *status);
int sv6621_get_link_stats(FAR struct sv6621_dev_s *dev,
                          FAR struct sv6621_link_stats_s *stats);
int sv6621_get_driver_stats(FAR struct sv6621_dev_s *dev,
                            FAR struct sv6621_driver_stats_s *stats);
int sv6621_set_country(FAR struct sv6621_dev_s *dev,
                       FAR const char country[2]);
int sv6621_get_country(FAR struct sv6621_dev_s *dev, FAR char country[3]);
int sv6621_scan(FAR struct sv6621_dev_s *dev);
int sv6621_connect(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_connect_s *request);
int sv6621_disconnect(FAR struct sv6621_dev_s *dev, uint16_t reason);
int sv6621_set_signal_threshold(FAR struct sv6621_dev_s *dev,
                                int32_t threshold_dbm, uint8_t hysteresis_db);
int sv6621_set_roam_policy(FAR struct sv6621_dev_s *dev,
                           FAR const struct sv6621_roam_policy_s *policy);
int sv6621_start_scheduled_scan(
    FAR struct sv6621_dev_s *dev,
    FAR const struct sv6621_sched_scan_request_s *request);
int sv6621_stop_scheduled_scan(FAR struct sv6621_dev_s *dev);
int sv6621_start_ap(FAR struct sv6621_dev_s *dev,
                    FAR const struct sv6621_ap_config_s *config);
int sv6621_stop_ap(FAR struct sv6621_dev_s *dev);
int sv6621_rekey_ap(FAR struct sv6621_dev_s *dev);
int sv6621_suspend(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_suspend_s *config);
int sv6621_resume(FAR struct sv6621_dev_s *dev);
#ifdef CONFIG_SV6621_BLUETOOTH
int sv6621_start_bluetooth(FAR struct sv6621_dev_s *dev);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_INCLUDE_SV6621_H */
