/****************************************************************************
 * drivers/drivers/sv6621/sv6621_core.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/kmalloc.h>
#include <nuttx/kthread.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "sv6621_core.h"
#include "sv6621_firmware.h"
#include "sv6621_regulatory.h"
#include "sv6621_sae_crypto.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_CORE_BSP_TIMEOUT_MS          2000
#define SV6621_CORE_WIFI_TIMEOUT_MS         2000
#define SV6621_CORE_SCAN_TIMEOUT_MS         10000
#define SV6621_CORE_CONNECT_TIMEOUT_MS      5000
#define SV6621_CORE_HANDSHAKE_TIMEOUT_MS    10000
#define SV6621_CORE_EVENT_BA_ACTION         13
#define SV6621_CORE_EVENT_CREDIT_UPDATE     16
#define SV6621_CORE_EVENT_MIC_FAILURE       17
#define SV6621_CORE_EVENT_THERMAL_WARN      18
#define SV6621_CORE_EVENT_CQM               20
#define SV6621_CORE_EVENT_UNPROTECTED_FRAME 21
#define SV6621_CORE_EVENT_CHANNEL_SWITCH    22
#define SV6621_CORE_EVENT_FW_RECOVERY       29
#define SV6621_CORE_MIC_FAILURE_SIZE        9
#define SV6621_CORE_CQM_EVENT_SIZE          11
#define SV6621_CORE_CHANNEL_EVENT_SIZE      11
#define SV6621_CORE_ROAM_MAX_COOLDOWN_MS    3600000
#define SV6621_CORE_WIFI_INSTANCE           0
#define SV6621_CORE_AP_INSTANCE             2

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_core_service_event(enum sv6621_service_event_e event,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg);
static void sv6621_core_rx_error(int error, FAR void *arg);
static void sv6621_core_command_receive_kick(FAR void *arg);
static void sv6621_core_command_error(int error, FAR void *arg);
static void
sv6621_core_ap_client(bool connected,
                      FAR const struct sv6621_ap_client_event_s *event,
                      FAR void *arg);
static void sv6621_core_command_event(uint8_t instance, uint8_t id,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg);
static void sv6621_core_scan_complete(int result, FAR void *arg);
static void sv6621_core_sched_scan_result(FAR const struct sv6621_bss_s *bss,
                                          FAR void *arg);
static void sv6621_core_sched_scan_complete(uint32_t request_id,
                                            FAR void *arg);
static void sv6621_core_scan_worker(FAR void *arg);
static void sv6621_core_station_event(bool connected, bool remote,
                                      uint16_t reason, FAR void *arg);
static bool sv6621_core_station_worker_stop(FAR struct sv6621_dev_s *dev,
                                            uint32_t generation);
static void sv6621_core_station_worker(FAR void *arg);
static void sv6621_core_data_input(FAR const struct sv6621_data_rx_s *rx,
                                   FAR void *arg);
#ifdef CONFIG_NET
static int
sv6621_core_ap_resolve_tx(FAR const uint8_t *frame, size_t length,
                          FAR struct sv6621_data_tx_context_s *context,
                          FAR void *arg);
#endif
static void sv6621_core_report(FAR struct sv6621_dev_s *dev,
                               enum sv6621_event_e event, FAR const void *data,
                               size_t length);
static void sv6621_core_event_worker(FAR void *arg);
static void sv6621_core_queue_fatal(FAR struct sv6621_dev_s *dev);
static void sv6621_core_queue_recovery(FAR struct sv6621_dev_s *dev,
                                       int error);
static int sv6621_core_recovery_thread(int argc, FAR char *argv[]);
static int sv6621_core_roam_thread(int argc, FAR char *argv[]);
static int sv6621_core_queue_roam_candidate(
    FAR struct sv6621_dev_s *dev,
    FAR const struct sv6621_scan_entry_s *candidate, uint32_t generation,
    int16_t signal_dbm);
static int
sv6621_core_roam_transaction(FAR struct sv6621_dev_s *dev,
                             FAR const struct sv6621_scan_entry_s *candidate,
                             uint32_t generation);
static void sv6621_core_recovery_worker(FAR void *arg);
static void sv6621_core_thermal_worker(FAR void *arg);
static void sv6621_core_security_worker(FAR void *arg);
static void sv6621_core_signal_worker(FAR void *arg);
static int
sv6621_core_start_roam_scan(FAR struct sv6621_dev_s *dev,
                            FAR const struct sv6621_signal_event_s *event);
static int
sv6621_core_connect_locked(FAR struct sv6621_dev_s *dev,
                           FAR const struct sv6621_connect_s *connection,
                           bool roaming);
static int sv6621_core_channel_switch(FAR struct sv6621_dev_s *dev,
                                      FAR const uint8_t *payload,
                                      size_t length);
static int sv6621_core_set_state(FAR struct sv6621_dev_s *dev,
                                 enum sv6621_state_e state, int error);
static int sv6621_core_ap_ready(FAR struct sv6621_dev_s *dev,
                                FAR const struct sv6621_ap_config_s *config);
static int sv6621_core_restore_station(FAR struct sv6621_dev_s *dev,
                                       int transaction_error);
#ifdef CONFIG_SV6621_PM
static int sv6621_core_pm_prepare(FAR struct pm_callback_s *callback,
                                  int domain, enum pm_state_e state);
static void sv6621_core_pm_notify(FAR struct pm_callback_s *callback,
                                  int domain, enum pm_state_e state);
static void sv6621_core_pm_resume_worker(FAR void *arg);
static int sv6621_core_pm_queue_resume(FAR struct sv6621_dev_s *dev);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_core_report
 ****************************************************************************/

static void sv6621_core_report(FAR struct sv6621_dev_s *dev,
                               enum sv6621_event_e event, FAR const void *data,
                               size_t length)
{
  if (dev->config.event != NULL)
    {
      dev->config.event(dev, event, data, length, dev->config.event_arg);
    }
}

/****************************************************************************
 * Name: sv6621_core_command_receive_kick
 ****************************************************************************/

static void sv6621_core_command_receive_kick(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  sv6621_rx_kick(&dev->rx);
}

/****************************************************************************
 * Name: sv6621_core_set_state
 ****************************************************************************/

static int sv6621_core_set_state(FAR struct sv6621_dev_s *dev,
                                 enum sv6621_state_e state, int error)
{
#ifdef CONFIG_SV6621_PM
  irqstate_t flags;
#endif
  int ret;

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  dev->status.state = state;
  dev->status.last_error = error;
  nxmutex_unlock(&dev->status_lock);
#ifdef CONFIG_SV6621_PM
  if (state == SV6621_STATE_OFF || state == SV6621_STATE_FAILED ||
      state == SV6621_STATE_WIFI_READY)
    {
      flags = spin_lock_irqsave(&dev->pm_lock);
      dev->pm_active = state == SV6621_STATE_WIFI_READY;
      spin_unlock_irqrestore(&dev->pm_lock, flags);
    }
#endif
  return 0;
}

static int sv6621_core_ap_ready(FAR struct sv6621_dev_s *dev,
                                FAR const struct sv6621_ap_config_s *config)
{
  uint32_t capability;
  size_t index;
  bool channel_allowed = false;
  int ret;

  if (config->ssid_length == 0 ||
      config->ssid_length > SV6621_SSID_MAX_LENGTH ||
      config->hidden_ssid > 2 || config->channel == 0 ||
      config->band > SV6621_BAND_5GHZ ||
      config->channel_width > SV6621_CHANNEL_WIDTH_160 ||
      config->beacon_interval == 0 || config->dtim_period == 0)
    {
      return -EINVAL;
    }

  if (config->security != SV6621_SECURITY_OPEN &&
      config->security != SV6621_SECURITY_WPA2_PSK &&
      config->security != SV6621_SECURITY_WPA3_SAE &&
      config->security != SV6621_SECURITY_WPA2_WPA3_PSK)
    {
      return -EOPNOTSUPP;
    }

  if ((config->security == SV6621_SECURITY_OPEN &&
       config->credential_length != 0) ||
      (config->security != SV6621_SECURITY_OPEN &&
       (config->credential_length < 8 || config->credential_length > 63)))
    {
      return -EINVAL;
    }

  for (index = 0; index < dev->scan_channel_count; index++)
    {
      FAR const struct sv6621_scan_channel_s *channel =
          &dev->scan_channels[index];

      if (channel->number == config->channel &&
          channel->band == (enum sv6621_scan_band_e)config->band &&
          (channel->flags &
           (SV6621_REGULATORY_FLAG_DFS | SV6621_REGULATORY_FLAG_NO_IR)) == 0)
        {
          channel_allowed = true;
          break;
        }
    }

  if (!channel_allowed)
    {
      return -EACCES;
    }

  if (config->band == SV6621_BAND_2GHZ)
    {
      capability = config->channel_width == SV6621_CHANNEL_WIDTH_20
                       ? SV6621_CONNECTION_BW_CAP_2GHZ_20MHZ
                       : SV6621_CONNECTION_BW_CAP_2GHZ_40MHZ;
      if (config->channel_width > SV6621_CHANNEL_WIDTH_40)
        {
          return -EOPNOTSUPP;
        }
    }
  else
    {
      static const uint32_t capabilities[] = {
        SV6621_CONNECTION_BW_CAP_5GHZ_20MHZ,
        SV6621_CONNECTION_BW_CAP_5GHZ_40MHZ,
        SV6621_CONNECTION_BW_CAP_5GHZ_80MHZ,
        SV6621_CONNECTION_BW_CAP_5GHZ_80P80MHZ,
        SV6621_CONNECTION_BW_CAP_5GHZ_160MHZ
      };

      capability = capabilities[config->channel_width];
    }

  if ((dev->wifi_info.bandwidth_capabilities & capability) == 0)
    {
      return -EOPNOTSUPP;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY || !dev->station_open ||
      !dev->ap_initialized)
    {
      ret = -ENETDOWN;
    }
  else if (dev->station_connected || dev->status.connected ||
           dev->status.ap_active || dev->scan_reporting || dev->suspended ||
           dev->roam_scan_pending || dev->roam_candidate_pending)
    {
      ret = -EBUSY;
    }
  else
    {
      ret = 0;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->scan.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->scan.active || dev->scan.stopping || dev->scan.recovery_pending)
    {
      ret = -EBUSY;
    }

  nxmutex_unlock(&dev->scan.lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->scheduled_scan.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->scheduled_scan.active)
    {
      ret = -EBUSY;
    }

  nxmutex_unlock(&dev->scheduled_scan.lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->station.state != SV6621_STATION_IDLE)
    {
      ret = -EBUSY;
    }

  nxmutex_unlock(&dev->station.lock);
  return ret;
}

static int sv6621_core_restore_station(FAR struct sv6621_dev_s *dev,
                                       int transaction_error)
{
  int close_ret;
  int open_ret;

#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, false, NULL);
#endif
  close_ret =
      sv6621_wifi_close_access_point(&dev->command, SV6621_CORE_AP_INSTANCE);
  open_ret = sv6621_wifi_open_station(&dev->command, dev->wifi_info.mac);
  if (open_ret == 0)
    {
      dev->station_open = true;
      return transaction_error;
    }

  dev->station_open = false;
  sv6621_core_queue_recovery(dev, open_ret);
  return close_ret < 0 ? close_ret : open_ret;
}

#ifdef CONFIG_SV6621_PM
/****************************************************************************
 * Name: sv6621_core_pm_queue_resume
 ****************************************************************************/

static int sv6621_core_pm_queue_resume(FAR struct sv6621_dev_s *dev)
{
  irqstate_t flags;
  int ret;

  flags = spin_lock_irqsave(&dev->pm_lock);
  if (!dev->pm_suspended || dev->pm_resume_queued)
    {
      spin_unlock_irqrestore(&dev->pm_lock, flags);
      return 0;
    }

  dev->pm_resume_queued = true;
  spin_unlock_irqrestore(&dev->pm_lock, flags);

  ret = work_queue(LPWORK, &dev->pm_resume_work, sv6621_core_pm_resume_worker,
                   dev, 0);
  if (ret < 0)
    {
      flags = spin_lock_irqsave(&dev->pm_lock);
      dev->pm_resume_queued = false;
      spin_unlock_irqrestore(&dev->pm_lock, flags);
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_core_pm_resume_worker
 ****************************************************************************/

static void sv6621_core_pm_resume_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  irqstate_t flags;

  (void)sv6621_resume(dev);

  flags = spin_lock_irqsave(&dev->pm_lock);
  dev->pm_suspended = false;
  dev->pm_resume_queued = false;
  spin_unlock_irqrestore(&dev->pm_lock, flags);
}

/****************************************************************************
 * Name: sv6621_core_pm_prepare
 ****************************************************************************/

static int sv6621_core_pm_prepare(FAR struct pm_callback_s *callback,
                                  int domain, enum pm_state_e state)
{
  FAR struct sv6621_dev_s *dev =
      container_of(callback, struct sv6621_dev_s, pm_callback);
  irqstate_t flags;
  bool active;
  bool suspended;

  if (domain != PM_IDLE_DOMAIN)
    {
      return 0;
    }

  if (state == PM_SLEEP)
    {
      /* pm_changestate() invokes callbacks with interrupts disabled while
       * holding the domain spinlock.  Firmware I/O and mutex acquisition
       * are therefore forbidden here.  A thread-context coordinator must
       * complete sv6621_suspend() before allowing system deep sleep.
       */

      flags = spin_lock_irqsave(&dev->pm_lock);
      active = dev->pm_active;
      suspended = dev->pm_suspended;
      spin_unlock_irqrestore(&dev->pm_lock, flags);
      return !active || suspended ? 0 : -EBUSY;
    }

  /* This is either a higher-power transition or rollback after another
   * driver rejected sleep.  Resume outside the PM callback context.
   */

  sv6621_core_pm_queue_resume(dev);
  return 0;
}

/****************************************************************************
 * Name: sv6621_core_pm_notify
 ****************************************************************************/

static void sv6621_core_pm_notify(FAR struct pm_callback_s *callback,
                                  int domain, enum pm_state_e state)
{
  FAR struct sv6621_dev_s *dev =
      container_of(callback, struct sv6621_dev_s, pm_callback);

  if (domain == PM_IDLE_DOMAIN && state == PM_RESTORE)
    {
      sv6621_core_pm_queue_resume(dev);
    }
}
#endif

/****************************************************************************
 * Name: sv6621_core_event_worker
 ****************************************************************************/

static void sv6621_core_event_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  uint32_t generation;

  for (;;)
    {
      int error;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      generation = dev->fatal_generation;
      error = dev->status.last_error;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_FATAL, &error, sizeof(error));

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (generation == dev->fatal_generation)
        {
          dev->fatal_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_queue_fatal
 ****************************************************************************/

static void sv6621_core_queue_fatal(FAR struct sv6621_dev_s *dev)
{
  bool queue = false;
  int ret;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  dev->fatal_generation++;
  if (!dev->fatal_work_scheduled)
    {
      dev->fatal_work_scheduled = true;
      queue = true;
    }

  nxmutex_unlock(&dev->status_lock);
  if (!queue)
    {
      return;
    }

  ret = work_queue(LPWORK, &dev->event_work, sv6621_core_event_worker, dev, 0);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->fatal_work_scheduled = false;
      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_queue_recovery
 ****************************************************************************/

static void sv6621_core_queue_recovery(FAR struct sv6621_dev_s *dev, int error)
{
  bool queue = false;
  int ret;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  if (dev->status.state != SV6621_STATE_OFF &&
      dev->status.state != SV6621_STATE_STOPPING &&
      dev->status.state != SV6621_STATE_FAILED && !dev->recovery_shutdown)
    {
      if (!dev->recovery_pending)
        {
          dev->recovery_pending = true;
          dev->status.last_error = error;
        }

      if (!dev->recovery_running)
        {
          dev->recovery_running = true;
          queue = true;
        }
    }

  nxmutex_unlock(&dev->status_lock);
  if (!queue)
    {
      return;
    }

  ret = nxsem_post(&dev->recovery_sem);
  if (ret < 0)
    {
      if (nxmutex_lock(&dev->status_lock) >= 0)
        {
          dev->recovery_pending = false;
          dev->recovery_running = false;
          dev->status.state = SV6621_STATE_FAILED;
          dev->status.last_error = ret;
          nxmutex_unlock(&dev->status_lock);
        }

      sv6621_core_queue_fatal(dev);
    }
}

/****************************************************************************
 * Name: sv6621_core_recovery_thread
 ****************************************************************************/

static int sv6621_core_recovery_thread(int argc, FAR char *argv[])
{
  FAR struct sv6621_dev_s *dev =
      (FAR struct sv6621_dev_s *)(uintptr_t)strtoull(argv[1], NULL, 16);

  (void)argc;

  for (;;)
    {
      if (nxsem_wait_uninterruptible(&dev->recovery_sem) < 0)
        {
          continue;
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          continue;
        }

      if (dev->recovery_shutdown)
        {
          nxmutex_unlock(&dev->status_lock);
          break;
        }

      nxmutex_unlock(&dev->status_lock);
      sv6621_core_recovery_worker(dev);
    }

  nxsem_post(&dev->recovery_exit_sem);
  return 0;
}

/****************************************************************************
 * Name: sv6621_core_roam_thread
 ****************************************************************************/

static int sv6621_core_roam_thread(int argc, FAR char *argv[])
{
  FAR struct sv6621_dev_s *dev =
      (FAR struct sv6621_dev_s *)(uintptr_t)strtoull(argv[1], NULL, 16);

  (void)argc;

  for (;;)
    {
      struct sv6621_roam_candidate_s event;
      struct sv6621_scan_entry_s candidate;
      uint32_t generation;
      int ret;

      if (nxsem_wait_uninterruptible(&dev->roam_sem) < 0)
        {
          continue;
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          continue;
        }

      if (dev->roam_shutdown)
        {
          nxmutex_unlock(&dev->status_lock);
          break;
        }

      if (!dev->roam_candidate_pending)
        {
          nxmutex_unlock(&dev->status_lock);
          continue;
        }

      candidate = dev->roam_candidate;
      generation = dev->roam_candidate_generation;
      event.candidate = candidate.bss;
      event.current_signal_dbm = dev->roam_candidate_signal_dbm;
      event.gain_db = (uint8_t)(dev->roam_candidate.bss.signal_dbm -
                                dev->roam_candidate_signal_dbm);
      dev->roam_candidate_pending = false;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_ROAM_CANDIDATE, &event,
                         sizeof(event));
      ret = sv6621_core_roam_transaction(dev, &candidate, generation);
      if (ret < 0 && ret != -ECANCELED && ret != -EALREADY)
        {
          sv6621_core_queue_recovery(dev, ret);
        }
    }

  nxsem_post(&dev->roam_exit_sem);
  return 0;
}

/****************************************************************************
 * Name: sv6621_core_queue_roam_candidate
 ****************************************************************************/

static int sv6621_core_queue_roam_candidate(
    FAR struct sv6621_dev_s *dev,
    FAR const struct sv6621_scan_entry_s *candidate, uint32_t generation,
    int16_t signal_dbm)
{
  int ret;

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->roam_shutdown || dev->roam_candidate_pending)
    {
      nxmutex_unlock(&dev->status_lock);
      return -EBUSY;
    }

  dev->roam_candidate = *candidate;
  dev->roam_candidate_generation = generation;
  dev->roam_candidate_signal_dbm = signal_dbm;
  dev->roam_candidate_pending = true;
  nxmutex_unlock(&dev->status_lock);

  ret = nxsem_post(&dev->roam_sem);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      if (dev->roam_candidate_pending &&
          dev->roam_candidate_generation == generation)
        {
          dev->roam_candidate_pending = false;
        }

      nxmutex_unlock(&dev->status_lock);
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_core_roam_transaction
 ****************************************************************************/

static int
sv6621_core_roam_transaction(FAR struct sv6621_dev_s *dev,
                             FAR const struct sv6621_scan_entry_s *candidate,
                             uint32_t generation)
{
  struct sv6621_connect_s connection;
  struct sv6621_connect_s policy_request;
  struct sv6621_connect_s rollback;
  struct sv6621_scan_entry_s previous;
  struct sv6621_roam_result_s event;
#ifdef CONFIG_NET
  struct sv6621_data_tx_context_s context;
#endif
  bool inserted;
  bool attempted = false;
  int ret;

  memset(&event, 0, sizeof(event));

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->roam_shutdown || !dev->roam_policy.enabled ||
      dev->status.state != SV6621_STATE_WIFI_READY ||
      !dev->station_connected || dev->station_generation != generation)
    {
      ret = -ECANCELED;
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  if (memcmp(candidate->bss.bssid, dev->station.target.bss.bssid,
             SV6621_MAC_LENGTH) == 0)
    {
      ret = -EALREADY;
      nxmutex_unlock(&dev->station.lock);
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  policy_request = dev->station.request;
  connection = policy_request;
  rollback = policy_request;
  previous = dev->station.target;
  memcpy(event.old_bssid, previous.bss.bssid, SV6621_MAC_LENGTH);
  memcpy(event.new_bssid, candidate->bss.bssid, SV6621_MAC_LENGTH);
  memcpy(connection.bssid, candidate->bss.bssid, SV6621_MAC_LENGTH);
  connection.bssid_valid = true;
  connection.channel = candidate->bss.channel;
  nxmutex_unlock(&dev->station.lock);
  nxmutex_unlock(&dev->status_lock);

  ret = sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_ROAM, true);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  attempted = true;
  ret = sv6621_core_connect_locked(dev, &connection, true);
  if (ret < 0)
    {
      int roam_error = ret;

      event.result = roam_error;
      sv6621_wpa_cancel(&dev->wpa, roam_error);
      sv6621_station_reset(&dev->station, roam_error);
      ret = sv6621_scan_cache_store(&dev->scan.cache, &previous, &inserted);
      if (ret < 0)
        {
          event.rollback_result = ret;
          goto unlock_lifecycle;
        }

      memcpy(rollback.bssid, previous.bss.bssid, SV6621_MAC_LENGTH);
      rollback.bssid_valid = true;
      rollback.channel = previous.bss.channel;
      ret = sv6621_core_connect_locked(dev, &rollback, false);
      event.rollback_result = ret;
      if (ret < 0)
        {
          goto unlock_lifecycle;
        }

      event.restored = true;
    }

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  dev->station.request = policy_request;
  nxmutex_unlock(&dev->station.lock);

#ifdef CONFIG_NET
  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  context.peer_index = dev->station.peer.peer_index;
  context.multicast_index = dev->station.peer.multicast_index;
  context.instance = dev->station.peer.instance;
  context.lmac_id = dev->station.peer.lmac_id;
  context.tid = 0;
  nxmutex_unlock(&dev->station.lock);
  sv6621_network_set_link(&dev->network, true, &context);
#endif

  ret = sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_ROAM, false);
#ifdef CONFIG_NET
  if (ret == 0)
    {
      sv6621_network_credit_available(&dev->network);
    }
#endif

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  if (attempted)
    {
      if (event.result == 0 && ret < 0)
        {
          event.result = ret;
        }

      sv6621_core_report(dev, SV6621_EVENT_ROAM_COMPLETE, &event,
                         sizeof(event));
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_core_thermal_worker
 ****************************************************************************/

static void sv6621_core_thermal_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  uint32_t generation;

  for (;;)
    {
      struct sv6621_thermal_s thermal;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      generation = dev->thermal_generation;
      thermal.transmit_blocked = dev->thermal_blocked;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_THERMAL_CHANGED, &thermal,
                         sizeof(thermal));

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (generation == dev->thermal_generation)
        {
          dev->thermal_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_security_worker
 ****************************************************************************/

static void sv6621_core_security_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  for (;;)
    {
      struct sv6621_mic_failure_s failure;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (dev->security_tail == dev->security_head)
        {
          dev->security_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      failure = dev->security_events[dev->security_tail];
      dev->security_tail =
          (dev->security_tail + 1) % SV6621_CORE_SECURITY_EVENT_DEPTH;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_MIC_FAILURE, &failure,
                         sizeof(failure));
    }
}

/****************************************************************************
 * Name: sv6621_core_valid_unicast_address
 ****************************************************************************/

static bool
sv6621_core_valid_unicast_address(FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  uint8_t aggregate = 0;
  size_t index;

  if ((address[0] & 1) != 0)
    {
      return false;
    }

  for (index = 0; index < SV6621_MAC_LENGTH; index++)
    {
      aggregate |= address[index];
    }

  return aggregate != 0;
}

/****************************************************************************
 * Name: sv6621_core_start_roam_scan
 ****************************************************************************/

static int
sv6621_core_start_roam_scan(FAR struct sv6621_dev_s *dev,
                            FAR const struct sv6621_signal_event_s *event)
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  size_t ssid_length;
  int ret;

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY ||
      !dev->station_connected || dev->roam_scan_pending || dev->scan_reporting)
    {
      ret = -EBUSY;
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  if (dev->station.request.bssid_valid ||
      dev->station.request.ssid_length == 0)
    {
      ret = -EOPNOTSUPP;
      nxmutex_unlock(&dev->station.lock);
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  ssid_length = dev->station.request.ssid_length;
  memcpy(ssid, dev->station.request.ssid, ssid_length);
  nxmutex_unlock(&dev->station.lock);

  dev->roam_scan_generation = dev->station_generation;
  dev->roam_scan_signal_dbm = event->signal_dbm;
  dev->roam_scan_pending = true;
  nxmutex_unlock(&dev->status_lock);

  ret =
      sv6621_scan_controller_begin(&dev->scan, dev->scan_channels,
                                   dev->scan_channel_count, ssid, ssid_length);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->roam_scan_pending = false;
      nxmutex_unlock(&dev->status_lock);
    }

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_core_signal_worker
 ****************************************************************************/

static void sv6621_core_signal_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  for (;;)
    {
      struct sv6621_signal_event_s event;
      bool roam_enabled;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (dev->signal_tail == dev->signal_head)
        {
          dev->signal_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      event = dev->signal_events[dev->signal_tail];
      dev->signal_tail =
          (dev->signal_tail + 1) % SV6621_CORE_SIGNAL_EVENT_DEPTH;
      if (!dev->station_connected || nxmutex_lock(&dev->station.lock) < 0)
        {
          nxmutex_unlock(&dev->status_lock);
          continue;
        }

      if ((event.status == SV6621_SIGNAL_HIGH ||
           event.status == SV6621_SIGNAL_TDLS_LOSS) &&
          memcmp(event.bssid, dev->station.target.bss.bssid,
                 SV6621_MAC_LENGTH) != 0)
        {
          nxmutex_unlock(&dev->station.lock);
          nxmutex_unlock(&dev->status_lock);
          continue;
        }

      nxmutex_unlock(&dev->station.lock);
      dev->status.signal_dbm = event.signal_dbm;
      roam_enabled = dev->roam_policy.enabled;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_SIGNAL_CHANGED, &event,
                         sizeof(event));

      if ((event.status == SV6621_SIGNAL_LOW ||
           event.status == SV6621_SIGNAL_BEACON_LOSS) &&
          roam_enabled && sv6621_core_valid_unicast_address(event.bssid))
        {
          (void)sv6621_core_start_roam_scan(dev, &event);
        }
    }
}

/****************************************************************************
 * Name: sv6621_core_recovery_worker
 ****************************************************************************/

static void sv6621_core_recovery_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  int ret;

  for (;;)
    {
      enum sv6621_state_e state = SV6621_STATE_RECOVERING;
      uint16_t disconnect_reason = 0;
      bool report_disconnect = false;
#ifdef CONFIG_SV6621_BLUETOOTH
      bool restart_bluetooth;
#endif
      int error = -EIO;

      ret = nxmutex_lock(&dev->lifecycle_lock);
      if (ret < 0)
        {
          return;
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          nxmutex_unlock(&dev->lifecycle_lock);
          return;
        }

      if (dev->recovery_shutdown || !dev->recovery_pending ||
          dev->status.state == SV6621_STATE_OFF ||
          dev->status.state == SV6621_STATE_STOPPING ||
          dev->status.state == SV6621_STATE_FAILED)
        {
          dev->recovery_pending = false;
          dev->recovery_running = false;
          nxmutex_unlock(&dev->status_lock);
          nxmutex_unlock(&dev->lifecycle_lock);
          return;
        }

      error = dev->status.last_error;
#ifdef CONFIG_SV6621_BLUETOOTH
      restart_bluetooth = sv6621_bluetooth_is_started(dev);
#endif
      dev->recovery_pending = false;
      dev->status.state = SV6621_STATE_RECOVERING;
      dev->status.recovery_count++;
      if (dev->station_connected || dev->status.connected)
        {
          report_disconnect = !dev->station_work_scheduled;
          dev->station_connected = false;
          dev->station_reason = disconnect_reason;
          dev->station_generation++;
          dev->roam_scan_pending = false;
          dev->roam_candidate_ticks_valid = false;
        }

      dev->status.connected = false;
      dev->status.ap_active = false;
      dev->status.ap_client_count = 0;
      memset(dev->status.bssid, 0, sizeof(dev->status.bssid));
      memset(dev->status.ssid, 0, sizeof(dev->status.ssid));
      dev->status.ssid_length = 0;
      dev->status.channel = 0;
      dev->status.signal_dbm = 0;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state,
                         sizeof(state));
      sv6621_core_report(dev, SV6621_EVENT_RECOVERY_STARTED, &error,
                         sizeof(error));

#ifdef CONFIG_NET
      sv6621_network_set_link(&dev->network, false, NULL);
#endif
      if (dev->ap_initialized)
        {
          sv6621_ap_reset(&dev->ap);
        }

      sv6621_sched_scan_cancel(&dev->scheduled_scan);
      sv6621_scan_controller_cancel(&dev->scan);
      sv6621_wpa_cancel(&dev->wpa, error);
      sv6621_station_reset(&dev->station, error);
      if (report_disconnect)
        {
          sv6621_core_report(dev, SV6621_EVENT_DISCONNECTED,
                             &disconnect_reason, sizeof(disconnect_reason));
        }

      sv6621_command_cancel(&dev->command, error);
#ifdef CONFIG_SV6621_BLUETOOTH
      sv6621_bluetooth_offline(dev, error);
#endif
      sv6621_data_reset_credits(&dev->data);
      sv6621_data_set_tx_block(&dev->data, UINT8_MAX, false);
      sv6621_rx_stop(&dev->rx);
      sv6621_data_reset_fragments(&dev->data);
      sv6621_data_reset_ba(&dev->data);
      dev->suspended = false;
      dev->station_open = false;

      if (dev->transport_open)
        {
          dev->config.transport->ops->close(dev->config.transport);
          dev->transport_open = false;
        }

      if (dev->powered)
        {
          dev->config.board_ops->power_off(dev->config.board_arg);
          dev->powered = false;
        }

      sv6621_service_reset(&dev->service);
      sv6621_core_set_state(dev, SV6621_STATE_FAILED, error);
      nxmutex_unlock(&dev->lifecycle_lock);

      ret = sv6621_start(dev);
#ifdef CONFIG_SV6621_BLUETOOTH
      if (ret == 0 && restart_bluetooth)
        {
          ret = sv6621_start_bluetooth(dev);
        }
#endif
      if (ret == 0)
        {
          sv6621_core_report(dev, SV6621_EVENT_RECOVERY_COMPLETE, NULL, 0);
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (dev->recovery_shutdown || !dev->recovery_pending ||
          dev->status.state == SV6621_STATE_OFF ||
          dev->status.state == SV6621_STATE_STOPPING ||
          dev->status.state == SV6621_STATE_FAILED)
        {
          dev->recovery_pending = false;
          dev->recovery_running = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_service_event
 ****************************************************************************/

static void sv6621_core_service_event(enum sv6621_service_event_e event,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (event != SV6621_SERVICE_EVENT_ASSERT &&
      event != SV6621_SERVICE_EVENT_DUMP_COMPLETE)
    {
      return;
    }

  sv6621_core_queue_recovery(dev, -EIO);
  (void)payload;
  (void)length;
}

/****************************************************************************
 * Name: sv6621_core_rx_error
 ****************************************************************************/

static void sv6621_core_rx_error(int error, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  sv6621_core_queue_recovery(dev, error);
}

/****************************************************************************
 * Name: sv6621_core_command_error
 ****************************************************************************/

static void sv6621_core_command_error(int error, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  sv6621_core_queue_recovery(dev, error);
}

static void
sv6621_core_ap_client(bool connected,
                      FAR const struct sv6621_ap_client_event_s *event,
                      FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  if (!dev->status.ap_active)
    {
      nxmutex_unlock(&dev->status_lock);
      return;
    }

  if (connected)
    {
      if (dev->status.ap_client_count < UINT8_MAX)
        {
          dev->status.ap_client_count++;
        }
    }
  else if (dev->status.ap_client_count != 0)
    {
      dev->status.ap_client_count--;
    }

  nxmutex_unlock(&dev->status_lock);
  sv6621_core_report(dev,
                     connected ? SV6621_EVENT_AP_CLIENT_CONNECTED
                               : SV6621_EVENT_AP_CLIENT_DISCONNECTED,
                     event, sizeof(*event));
}

/****************************************************************************
 * Name: sv6621_core_channel_switch
 ****************************************************************************/

static int sv6621_core_channel_switch(FAR struct sv6621_dev_s *dev,
                                      FAR const uint8_t *payload,
                                      size_t length)
{
#ifdef CONFIG_NET
  struct sv6621_data_tx_context_s context;
#endif
  uint8_t channel;
  uint8_t band;
  bool connected;
  int ret;

  if (length != SV6621_CORE_CHANNEL_EVENT_SIZE || payload[0] > 1 ||
      payload[2] == 0 || payload[6] > SV6621_BAND_5GHZ)
    {
      return -EPROTO;
    }

  if (payload[0] != 0)
    {
      ret = sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_CHANNEL,
                                     true);
#ifdef CONFIG_NET
      if (ret >= 0)
        {
          sv6621_network_set_link(&dev->network, false, NULL);
        }
#endif
      return ret;
    }

  channel = payload[2];
  band = payload[6];
  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  connected = dev->station_connected;
  if (connected)
    {
      ret = nxmutex_lock(&dev->station.lock);
      if (ret < 0)
        {
          nxmutex_unlock(&dev->status_lock);
          return ret;
        }

      dev->status.channel = channel;
      dev->status.band = (enum sv6621_band_e)band;
      dev->station.target.bss.channel = channel;
      dev->station.target.bss.band = (enum sv6621_band_e)band;
#ifdef CONFIG_NET
      context.peer_index = dev->station.peer.peer_index;
      context.multicast_index = dev->station.peer.multicast_index;
      context.instance = dev->station.peer.instance;
      context.lmac_id = dev->station.peer.lmac_id;
      context.tid = 0;
#endif
      nxmutex_unlock(&dev->station.lock);
    }

  nxmutex_unlock(&dev->status_lock);
  ret = sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_CHANNEL,
                                 false);
#ifdef CONFIG_NET
  if (ret >= 0 && connected)
    {
      sv6621_network_set_link(&dev->network, true, &context);
      sv6621_network_credit_available(&dev->network);
    }
#endif
  return ret;
}

/****************************************************************************
 * Name: sv6621_core_command_event
 ****************************************************************************/

static void sv6621_core_command_event(uint8_t instance, uint8_t id,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (id == SV6621_CORE_EVENT_CREDIT_UPDATE && length >= 4)
    {
      uint16_t lmac0 = payload[0] | ((uint16_t)payload[1] << 8);
      uint16_t lmac1 = payload[2] | ((uint16_t)payload[3] << 8);

      sv6621_data_add_credits(&dev->data, lmac0, lmac1);
#ifdef CONFIG_NET
      sv6621_network_credit_available(&dev->network);
#endif
      return;
    }

  if (id == SV6621_CORE_EVENT_THERMAL_WARN)
    {
      bool queue = false;
      bool blocked;
      int ret;

      if (length != 1)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      blocked = payload[0] == 0;
      if (sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_THERMAL,
                                   blocked) < 0)
        {
          sv6621_core_queue_recovery(dev, -EIO);
          return;
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      dev->thermal_blocked = blocked;
      dev->thermal_generation++;
      if (!dev->thermal_work_scheduled)
        {
          dev->thermal_work_scheduled = true;
          queue = true;
        }

      nxmutex_unlock(&dev->status_lock);

#ifdef CONFIG_NET
      if (!blocked)
        {
          sv6621_network_credit_available(&dev->network);
        }
#endif
      if (queue)
        {
          ret = work_queue(LPWORK, &dev->thermal_work,
                           sv6621_core_thermal_worker, dev, 0);
          if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
            {
              dev->thermal_work_scheduled = false;
              nxmutex_unlock(&dev->status_lock);
            }
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_MIC_FAILURE)
    {
      struct sv6621_mic_failure_s failure;
      uint8_t next;
      bool overflow = false;
      bool queue = false;
      int ret;

      if (length != SV6621_CORE_MIC_FAILURE_SIZE)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      failure.group_key = payload[0] != 0;
      failure.key_index = payload[1];
      failure.lmac_id = payload[2];
      memcpy(failure.address, payload + 3, SV6621_MAC_LENGTH);
      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      dev->status.mic_failures++;
      next = (dev->security_head + 1) % SV6621_CORE_SECURITY_EVENT_DEPTH;
      if (next == dev->security_tail)
        {
          dev->status.mic_failures_dropped++;
          overflow = true;
        }
      else
        {
          dev->security_events[dev->security_head] = failure;
          dev->security_head = next;
          if (!dev->security_work_scheduled)
            {
              dev->security_work_scheduled = true;
              queue = true;
            }
        }

      nxmutex_unlock(&dev->status_lock);
      if (overflow)
        {
          sv6621_core_queue_recovery(dev, -ENOBUFS);
          return;
        }

      if (queue)
        {
          ret = work_queue(LPWORK, &dev->security_work,
                           sv6621_core_security_worker, dev, 0);
          if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
            {
              uint8_t dropped = dev->security_head >= dev->security_tail
                                    ? dev->security_head - dev->security_tail
                                    : SV6621_CORE_SECURITY_EVENT_DEPTH -
                                          dev->security_tail +
                                          dev->security_head;

              dev->security_work_scheduled = false;
              dev->status.mic_failures_dropped += dropped;
              dev->security_tail = dev->security_head;
              nxmutex_unlock(&dev->status_lock);
              sv6621_core_queue_recovery(dev, ret);
            }
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_CQM)
    {
      struct sv6621_signal_event_s event;
      uint8_t next;
      bool queue = false;
      uint8_t status;
      int ret;

      if (length != SV6621_CORE_CQM_EVENT_SIZE)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      status = payload[0];
      if (status < SV6621_SIGNAL_LOW || status > SV6621_SIGNAL_TDLS_LOSS ||
          payload[10] > SV6621_BAND_5GHZ)
        {
          return;
        }

      event.status = (enum sv6621_signal_status_e)status;
      event.signal_dbm = (int16_t)(payload[1] | ((uint16_t)payload[2] << 8));
      memcpy(event.bssid, payload + 3, SV6621_MAC_LENGTH);
      event.channel = payload[9];
      event.band = (enum sv6621_band_e)payload[10];
      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (!dev->station_connected)
        {
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      ret = nxmutex_lock(&dev->station.lock);
      if (ret < 0)
        {
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      if ((event.status == SV6621_SIGNAL_HIGH ||
           event.status == SV6621_SIGNAL_TDLS_LOSS) &&
          memcmp(event.bssid, dev->station.target.bss.bssid,
                 SV6621_MAC_LENGTH) != 0)
        {
          nxmutex_unlock(&dev->station.lock);
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->station.lock);
      next = (dev->signal_head + 1) % SV6621_CORE_SIGNAL_EVENT_DEPTH;
      if (next == dev->signal_tail)
        {
          dev->signal_tail =
              (dev->signal_tail + 1) % SV6621_CORE_SIGNAL_EVENT_DEPTH;
          dev->status.signal_events_dropped++;
        }

      dev->signal_events[dev->signal_head] = event;
      dev->signal_head = next;
      if (!dev->signal_work_scheduled)
        {
          dev->signal_work_scheduled = true;
          queue = true;
        }

      nxmutex_unlock(&dev->status_lock);
      if (queue)
        {
          ret = work_queue(LPWORK, &dev->signal_work,
                           sv6621_core_signal_worker, dev, 0);
          if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
            {
              uint8_t dropped = dev->signal_head >= dev->signal_tail
                                    ? dev->signal_head - dev->signal_tail
                                    : SV6621_CORE_SIGNAL_EVENT_DEPTH -
                                          dev->signal_tail + dev->signal_head;

              dev->signal_work_scheduled = false;
              dev->status.signal_events_dropped += dropped;
              dev->signal_tail = dev->signal_head;
              nxmutex_unlock(&dev->status_lock);
            }
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_UNPROTECTED_FRAME)
    {
      /* This port does not negotiate PMF or WAPI.  Do not reinject an
       * exceptional unprotected frame into either the network or station
       * state machine.
       */

      if (nxmutex_lock(&dev->status_lock) >= 0)
        {
          dev->status.unprotected_frames++;
          nxmutex_unlock(&dev->status_lock);
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_FW_RECOVERY)
    {
      bool blocked;

      if (length != 1)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      blocked = payload[0] == 0;
      if (sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_RECOVERY,
                                   blocked) < 0)
        {
          sv6621_core_queue_recovery(dev, -EIO);
          return;
        }

#ifdef CONFIG_NET
      if (!blocked)
        {
          sv6621_network_credit_available(&dev->network);
        }
#endif
      return;
    }

  if (id == SV6621_CORE_EVENT_CHANNEL_SWITCH)
    {
      int ret = sv6621_core_channel_switch(dev, payload, length);

      if (ret < 0)
        {
          sv6621_core_queue_recovery(dev, ret);
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_BA_ACTION)
    {
      (void)sv6621_data_ba_event(&dev->data, payload, length);
      return;
    }

  if (dev->ap_initialized && sv6621_ap_is_active(&dev->ap) &&
      (id == SV6621_AP_EVENT_RX_MGMT || id == SV6621_AP_EVENT_DEL_STA ||
       id == SV6621_AP_EVENT_MGMT_TX_STATUS))
    {
      int ret = sv6621_ap_queue_event(&dev->ap, instance, id, payload, length);

      if (ret < 0)
        {
          sv6621_core_queue_recovery(dev, ret);
        }

      return;
    }

  sv6621_scan_command_event(instance, id, payload, length, &dev->scan);
  sv6621_sched_scan_command_event(instance, id, payload, length,
                                  &dev->scheduled_scan);
  sv6621_station_command_event(instance, id, payload, length, &dev->station);
}

/****************************************************************************
 * Name: sv6621_core_station_event
 ****************************************************************************/

static void sv6621_core_station_event(bool connected, bool remote,
                                      uint16_t reason, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  bool queue = false;
  int ret;

  sv6621_data_reset_fragments(&dev->data);
  sv6621_data_reset_ba(&dev->data);
  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  dev->station_connected = connected;
  dev->station_remote_disconnect = !connected && remote;
  dev->station_reason = reason;
  dev->station_generation++;
  if (!connected)
    {
      dev->roam_scan_pending = false;
      dev->roam_candidate_ticks_valid = false;
    }
  if (!dev->station_work_scheduled)
    {
      dev->station_work_scheduled = true;
      queue = true;
    }

  nxmutex_unlock(&dev->status_lock);
  if (!queue)
    {
      return;
    }

  ret = work_queue(LPWORK, &dev->station_work, sv6621_core_station_worker, dev,
                   0);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->station_work_scheduled = false;
      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_station_worker_stop
 ****************************************************************************/

static bool sv6621_core_station_worker_stop(FAR struct sv6621_dev_s *dev,
                                            uint32_t generation)
{
  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return true;
    }

  if (generation != dev->station_generation)
    {
      nxmutex_unlock(&dev->status_lock);
      return false;
    }

  dev->station_work_scheduled = false;
  nxmutex_unlock(&dev->status_lock);
  return true;
}

/****************************************************************************
 * Name: sv6621_core_station_worker
 ****************************************************************************/

static void sv6621_core_station_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  uint32_t generation;

  for (;;)
    {
      struct sv6621_status_s status;
      struct sv6621_roam_policy_s roam_policy;
      bool event_current = false;
#ifdef CONFIG_NET
      struct sv6621_data_tx_context_s context;
      struct sv6621_link_stats_s link_stats;
      bool link_ready = false;
#endif
      uint16_t reason;
      bool remote_disconnect;
      bool station_ready = false;
      bool connected;
      int station_ret = 0;
#ifdef CONFIG_NET
      int network_ret = 0;
#endif

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      generation = dev->station_generation;
      connected = dev->station_connected;
      remote_disconnect = dev->station_remote_disconnect;
      reason = dev->station_reason;
      if (connected)
        {
          station_ret = nxmutex_lock(&dev->station.lock);
          if (station_ret >= 0)
            {
              memcpy(dev->status.bssid, dev->station.target.bss.bssid,
                     SV6621_MAC_LENGTH);
              dev->status.channel = dev->station.target.bss.channel;
              dev->status.band = dev->station.target.bss.band;
              dev->status.signal_dbm = dev->station.target.bss.signal_dbm;
              memcpy(dev->status.ssid, dev->station.target.bss.ssid,
                     dev->station.target.bss.ssid_length);
              dev->status.ssid_length = dev->station.target.bss.ssid_length;
              station_ready = true;
#ifdef CONFIG_NET
              context.peer_index = dev->station.peer.peer_index;
              context.multicast_index = dev->station.peer.multicast_index;
              context.instance = dev->station.peer.instance;
              context.lmac_id = dev->station.peer.lmac_id;
              context.tid = 0;
              link_ready = true;
#endif
              nxmutex_unlock(&dev->station.lock);
            }
        }
      else if (!connected)
        {
          memset(dev->status.bssid, 0, sizeof(dev->status.bssid));
          memset(dev->status.ssid, 0, sizeof(dev->status.ssid));
          dev->status.ssid_length = 0;
          dev->status.channel = 0;
          dev->status.signal_dbm = 0;
        }

      dev->status.connected = false;
      status = dev->status;
      roam_policy = dev->roam_policy;
      nxmutex_unlock(&dev->status_lock);
#ifdef CONFIG_NET
      if (connected && !station_ready)
        {
          sv6621_core_queue_recovery(dev,
                                     station_ret < 0 ? station_ret : -EIO);
          if (sv6621_core_station_worker_stop(dev, generation))
            {
              return;
            }

          continue;
        }
#endif
#ifdef CONFIG_NET
      if (connected && link_ready)
        {
          network_ret = sv6621_stats_query(&dev->command, context.instance,
                                           status.bssid, &link_stats);
          if (network_ret < 0)
            {
              if (nxmutex_lock(&dev->status_lock) >= 0)
                {
                  event_current = generation == dev->station_generation &&
                                  dev->station_connected;
                  nxmutex_unlock(&dev->status_lock);
                }

              if (event_current)
                {
                  sv6621_core_queue_recovery(dev, network_ret);
                  if (sv6621_core_station_worker_stop(dev, generation))
                    {
                      return;
                    }
                }

              continue;
            }

          network_ret = sv6621_network_sync_multicast(&dev->network);
          if (network_ret == 0)
            {
              network_ret =
                  sv6621_network_sync_link_addresses(&dev->network, &context);
            }

          if (network_ret < 0)
            {
              if (nxmutex_lock(&dev->status_lock) >= 0)
                {
                  event_current = generation == dev->station_generation &&
                                  dev->station_connected;
                  nxmutex_unlock(&dev->status_lock);
                }

              if (event_current)
                {
                  sv6621_core_queue_recovery(dev, network_ret);
                  if (sv6621_core_station_worker_stop(dev, generation))
                    {
                      return;
                    }
                }

              continue;
            }

          if (nxmutex_lock(&dev->status_lock) < 0)
            {
              return;
            }

          event_current =
              generation == dev->station_generation && dev->station_connected;
          if (event_current)
            {
              dev->status.signal_dbm = link_stats.signal_dbm;
              dev->status.connected = true;
              status = dev->status;
              sv6621_network_set_link(&dev->network, true, &context);
            }

          nxmutex_unlock(&dev->status_lock);
          if (!event_current)
            {
              continue;
            }
        }
      else
        {
          sv6621_network_set_link(&dev->network, false, NULL);
        }
#endif
      if (!connected)
        {
          if (remote_disconnect)
            {
              int cleanup_ret;

              cleanup_ret = nxmutex_lock(&dev->lifecycle_lock);
              if (cleanup_ret < 0)
                {
                  sv6621_core_queue_recovery(dev, cleanup_ret);
                  if (sv6621_core_station_worker_stop(dev, generation))
                    {
                      return;
                    }

                  continue;
                }

              if (nxmutex_lock(&dev->status_lock) >= 0)
                {
                  event_current = generation == dev->station_generation &&
                                  !dev->station_connected &&
                                  dev->station_remote_disconnect;
                  nxmutex_unlock(&dev->status_lock);
                }

              if (event_current)
                {
                  cleanup_ret = sv6621_connection_disconnect(
                      &dev->command, SV6621_CONNECTION_DISCONNECT_ONLY, true,
                      reason, NULL, 0);
                }

              nxmutex_unlock(&dev->lifecycle_lock);
              if (!event_current)
                {
                  continue;
                }

              if (cleanup_ret < 0)
                {
                  sv6621_core_queue_recovery(dev, cleanup_ret);
                  if (sv6621_core_station_worker_stop(dev, generation))
                    {
                      return;
                    }

                  continue;
                }

              sv6621_wpa_disconnected(&dev->wpa, -ECONNRESET);
            }

          (void)sv6621_data_set_tx_block(&dev->data,
                                         SV6621_DATA_TX_BLOCK_CHANNEL, false);
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      event_current = generation == dev->station_generation &&
                      dev->station_connected == connected;
      status = dev->status;
      nxmutex_unlock(&dev->status_lock);
      if (!event_current)
        {
          continue;
        }

      if (connected)
        {
          if (roam_policy.enabled)
            {
              (void)sv6621_set_signal_threshold(dev, roam_policy.threshold_dbm,
                                                roam_policy.hysteresis_db);
            }

          sv6621_core_report(dev, SV6621_EVENT_CONNECTED, &status,
                             sizeof(status));
        }
      else
        {
          sv6621_core_report(dev, SV6621_EVENT_DISCONNECTED, &reason,
                             sizeof(reason));
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (generation == dev->station_generation)
        {
          dev->station_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_data_input
 ****************************************************************************/

static void sv6621_core_data_input(FAR const struct sv6621_data_rx_s *rx,
                                   FAR void *arg)
{
#ifdef CONFIG_NET
  FAR struct sv6621_dev_s *dev = arg;
  struct sv6621_data_tx_context_s context;
  bool deliver_local = true;
  bool forward = false;

  if (dev->ap_initialized && sv6621_ap_is_active(&dev->ap) &&
      sv6621_ap_validate_rx(&dev->ap, rx) < 0)
    {
      return;
    }

  if (dev->ap_initialized && sv6621_ap_is_active(&dev->ap) &&
      sv6621_ap_forward_policy(&dev->ap, rx, &forward, &deliver_local) == 0 &&
      forward)
    {
      if (sv6621_ap_resolve_tx(&dev->ap, rx->frame, rx->frame_length,
                               &context) < 0 ||
          sv6621_network_forward(&dev->network, rx->frame, rx->frame_length) <
              0)
        {
          deliver_local = true;
        }
    }

  if (deliver_local)
    {
      sv6621_network_input(rx, &dev->network);
    }
#else
  (void)rx;
  (void)arg;
#endif
}

static void sv6621_core_eapol_input(FAR const struct sv6621_data_rx_s *rx,
                                    FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (dev->ap_initialized && sv6621_ap_is_active(&dev->ap) &&
      rx->instance_valid && rx->instance == dev->ap.context.instance)
    {
      sv6621_ap_eapol_input(rx, &dev->ap);
    }
  else
    {
      sv6621_wpa_input(rx, &dev->wpa);
    }
}

#ifdef CONFIG_NET
static int
sv6621_core_ap_resolve_tx(FAR const uint8_t *frame, size_t length,
                          FAR struct sv6621_data_tx_context_s *context,
                          FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  return sv6621_ap_resolve_tx(&dev->ap, frame, length, context);
}
#endif

/****************************************************************************
 * Name: sv6621_core_scan_complete
 ****************************************************************************/

static void sv6621_core_scan_complete(int result, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  int ret;

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return;
    }

  if (dev->scan_reporting)
    {
      nxmutex_unlock(&dev->status_lock);
      return;
    }

  dev->scan_reporting = true;
  dev->scan_result = result;
  nxmutex_unlock(&dev->status_lock);
  ret = work_queue(LPWORK, &dev->scan_work, sv6621_core_scan_worker, dev, 0);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->scan_reporting = false;
      dev->roam_scan_pending = false;
      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_scan_worker
 ****************************************************************************/

static void sv6621_core_scan_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  FAR struct sv6621_bss_s *entries;
  struct sv6621_scan_entry_s roam_entry;
  struct sv6621_connect_s request;
  struct sv6621_roam_policy_s roam_policy;
  uint8_t current_bssid[SV6621_MAC_LENGTH];
  uint32_t roam_generation = 0;
  size_t count = SV6621_SCAN_CACHE_CAPACITY;
  size_t index;
  int16_t roam_signal_dbm = 0;
  bool roam_scan = false;
  int result;
  int ret;

  entries = kmm_malloc(sizeof(*entries) * SV6621_SCAN_CACHE_CAPACITY);
  if (entries == NULL)
    {
      count = 0;
      result = -ENOMEM;
    }
  else
    {
      ret = sv6621_scan_cache_snapshot(&dev->scan.cache, entries, &count);
      result = ret < 0 ? ret : dev->scan_result;
    }

  for (index = 0; index < count; index++)
    {
      sv6621_core_report(dev, SV6621_EVENT_SCAN_RESULT, &entries[index],
                         sizeof(entries[index]));
    }

  if (nxmutex_lock(&dev->status_lock) >= 0)
    {
      roam_scan = dev->roam_scan_pending;
      roam_generation = dev->roam_scan_generation;
      roam_signal_dbm = dev->roam_scan_signal_dbm;
      roam_policy = dev->roam_policy;
      dev->scan_reporting = false;
      dev->roam_scan_pending = false;
      nxmutex_unlock(&dev->status_lock);
    }

  if (roam_scan && result == 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      if (dev->station_connected &&
          dev->station_generation == roam_generation &&
          nxmutex_lock(&dev->station.lock) >= 0)
        {
          request = dev->station.request;
          memcpy(current_bssid, dev->station.target.bss.bssid,
                 sizeof(current_bssid));
          nxmutex_unlock(&dev->station.lock);
          nxmutex_unlock(&dev->status_lock);

          ret = sv6621_scan_cache_find_roam_candidate(
              &dev->scan.cache, &request, current_bssid, roam_signal_dbm,
              roam_policy.minimum_gain_db, &roam_entry);
          if (ret == 0)
            {
              clock_t now = clock_systime_ticks();

              if (nxmutex_lock(&dev->status_lock) >= 0)
                {
                  bool current = dev->station_connected &&
                                 dev->station_generation == roam_generation &&
                                 dev->roam_policy.enabled;
                  bool cooldown_elapsed =
                      !dev->roam_candidate_ticks_valid ||
                      now - dev->roam_candidate_ticks >=
                          MSEC2TICK(dev->roam_policy.cooldown_ms);

                  if (current && cooldown_elapsed)
                    {
                      dev->roam_candidate_ticks = now;
                      dev->roam_candidate_ticks_valid = true;
                    }

                  nxmutex_unlock(&dev->status_lock);
                  if (current && cooldown_elapsed)
                    {
                      (void)sv6621_core_queue_roam_candidate(
                          dev, &roam_entry, roam_generation, roam_signal_dbm);
                    }
                }
            }
        }
      else
        {
          nxmutex_unlock(&dev->status_lock);
        }
    }

  kmm_free(entries);

  sv6621_core_report(dev, SV6621_EVENT_SCAN_COMPLETE, &result, sizeof(result));
}

/****************************************************************************
 * Name: sv6621_core_sched_scan_result
 ****************************************************************************/

static void sv6621_core_sched_scan_result(FAR const struct sv6621_bss_s *bss,
                                          FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  sv6621_core_report(dev, SV6621_EVENT_SCAN_RESULT, bss, sizeof(*bss));
}

/****************************************************************************
 * Name: sv6621_core_sched_scan_complete
 ****************************************************************************/

static void sv6621_core_sched_scan_complete(uint32_t request_id, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  sv6621_core_report(dev, SV6621_EVENT_SCHEDULED_SCAN_RESULTS, &request_id,
                     sizeof(request_id));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_create(FAR const struct sv6621_config_s *config,
                  FAR struct sv6621_dev_s **dev_out)
{
  FAR struct sv6621_dev_s *dev;
  char thread_arg[2 + sizeof(uintptr_t) * 2 + 1];
  FAR char *thread_argv[2];
  int ret;

  if (config == NULL || dev_out == NULL || config->transport == NULL ||
      config->board_ops == NULL || config->board_ops->power_on == NULL ||
      config->board_ops->power_off == NULL ||
      config->board_ops->load_address == NULL ||
      config->board_ops->store_address == NULL || config->iram.data == NULL ||
      config->iram.length == 0 || config->dram.data == NULL ||
      config->dram.length == 0 || config->nvram.data == NULL ||
      config->nvram.length == 0 || config->calibration.data == NULL ||
      config->calibration.length == 0 || config->regulatory == NULL)
    {
      return -EINVAL;
    }

  if ((config->regulatory_domains == NULL) !=
      (config->regulatory_domain_count == 0))
    {
      return -EINVAL;
    }

#ifdef CONFIG_SV6621_PM
  if ((config->system_suspend.wake_flags & ~SV6621_WAKE_ALL) != 0 ||
      (!config->system_suspend.wake_enabled &&
       config->system_suspend.wake_flags != 0))
    {
      return -EINVAL;
    }
#endif

  ret = sv6621_transport_validate(config->transport);
  if (ret < 0)
    {
      return ret;
    }

  *dev_out = NULL;
  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      return -ENOMEM;
    }

  dev->config = *config;
  dev->regulatory = *config->regulatory;
  dev->roam_policy.enabled = true;
  dev->roam_policy.threshold_dbm = SV6621_ROAM_DEFAULT_THRESHOLD_DBM;
  dev->roam_policy.hysteresis_db = SV6621_ROAM_DEFAULT_HYSTERESIS_DB;
  dev->roam_policy.minimum_gain_db = SV6621_ROAM_DEFAULT_MINIMUM_GAIN_DB;
  dev->roam_policy.cooldown_ms = SV6621_ROAM_DEFAULT_COOLDOWN_MS;
  dev->status.state = SV6621_STATE_OFF;
  ret = sv6621_regulatory_scan_channels(
      &dev->regulatory, dev->scan_channels,
      SV6621_REGULATORY_SCAN_CHANNEL_CAPACITY, &dev->scan_channel_count);
  if (ret < 0)
    {
      goto free_device;
    }

  ret = nxmutex_init(&dev->lifecycle_lock);
  if (ret < 0)
    {
      goto free_device;
    }

  ret = nxmutex_init(&dev->status_lock);
  if (ret < 0)
    {
      goto destroy_lifecycle_lock;
    }

  ret = sv6621_packet_router_init(&dev->router);
  if (ret < 0)
    {
      goto destroy_status_lock;
    }

  ret = sv6621_tx_init(&dev->tx, config->transport);
  if (ret < 0)
    {
      goto deinit_router;
    }

  ret = sv6621_data_init(&dev->data, &dev->router, &dev->tx,
                         sv6621_core_data_input, dev);
  if (ret < 0)
    {
      goto deinit_tx;
    }

  ret = sv6621_scan_controller_init(&dev->scan, &dev->command, &dev->rx,
                                    SV6621_CORE_SCAN_TIMEOUT_MS,
                                    sv6621_core_scan_complete, dev);
  if (ret < 0)
    {
      goto deinit_data;
    }

  ret = sv6621_sched_scan_init(&dev->scheduled_scan, &dev->command,
                               &dev->scan.cache, sv6621_core_sched_scan_result,
                               sv6621_core_sched_scan_complete, dev);
  if (ret < 0)
    {
      goto deinit_scan;
    }

  ret = sv6621_command_engine_init(&dev->command, sv6621_tx_command_sender,
                                   &dev->tx, sv6621_core_command_receive_kick,
                                   dev, sv6621_core_command_event, dev,
                                   sv6621_core_command_error, dev);
  if (ret < 0)
    {
      goto deinit_scheduled_scan;
    }

  ret = sv6621_station_init(&dev->station, &dev->command, &dev->scan,
                            sv6621_core_station_event, dev);
  if (ret < 0)
    {
      goto deinit_command;
    }

  ret = sv6621_wpa_init(&dev->wpa, &dev->command, &dev->station);
  if (ret < 0)
    {
      goto deinit_station;
    }

  sv6621_data_set_eapol_input(&dev->data, sv6621_core_eapol_input, dev);
  ret = sv6621_service_init(&dev->service, sv6621_core_service_event, dev);
  if (ret < 0)
    {
      goto deinit_wpa;
    }

  ret = sv6621_rx_init(&dev->rx, config->transport, &dev->router,
                       sv6621_core_rx_error, dev);
  if (ret < 0)
    {
      goto deinit_service;
    }

  ret =
      sv6621_packet_subscribe(&dev->router, SV6621_CHANNEL_LOOPCHECK,
                              sv6621_service_channel_consumer, &dev->service);
  if (ret < 0)
    {
      goto deinit_rx;
    }

  ret =
      sv6621_packet_subscribe(&dev->router, SV6621_CHANNEL_WIFI_COMMAND,
                              sv6621_command_channel_consumer, &dev->command);
  if (ret < 0)
    {
      goto unsubscribe_service;
    }

#ifdef CONFIG_SV6621_BLUETOOTH
  ret = sv6621_bluetooth_attach(dev);
  if (ret < 0)
    {
      goto unsubscribe_command;
    }
#endif

  ret = nxsem_init(&dev->recovery_sem, 0, 0);
  if (ret < 0)
    {
#ifdef CONFIG_SV6621_BLUETOOTH
      goto detach_bluetooth;
#else
      goto unsubscribe_command;
#endif
    }

  ret = nxsem_init(&dev->recovery_exit_sem, 0, 0);
  if (ret < 0)
    {
      goto destroy_recovery_sem;
    }

  snprintf(thread_arg, sizeof(thread_arg), "%" PRIxPTR, (uintptr_t)dev);
  thread_argv[0] = thread_arg;
  thread_argv[1] = NULL;
  ret = kthread_create("sv6621_recovery", CONFIG_SV6621_RECOVERY_PRIO,
                       CONFIG_SV6621_RECOVERY_STACK,
                       sv6621_core_recovery_thread, thread_argv);
  if (ret <= 0)
    {
      ret = ret < 0 ? ret : -ECHILD;
      goto destroy_recovery_exit_sem;
    }

  ret = nxsem_init(&dev->roam_sem, 0, 0);
  if (ret < 0)
    {
      goto stop_recovery_thread;
    }

  ret = nxsem_init(&dev->roam_exit_sem, 0, 0);
  if (ret < 0)
    {
      goto destroy_roam_sem;
    }

  ret = kthread_create("sv6621_roam", CONFIG_SV6621_RECOVERY_PRIO,
                       CONFIG_SV6621_RECOVERY_STACK, sv6621_core_roam_thread,
                       thread_argv);
  if (ret <= 0)
    {
      ret = ret < 0 ? ret : -ECHILD;
      goto destroy_roam_exit_sem;
    }

#ifdef CONFIG_SV6621_PM
  spin_lock_init(&dev->pm_lock);
  dev->pm_callback.prepare = sv6621_core_pm_prepare;
  dev->pm_callback.notify = sv6621_core_pm_notify;
  ret = pm_register(&dev->pm_callback);
  if (ret < 0)
    {
      goto stop_roam_thread;
    }

  dev->pm_registered = true;
#endif

  *dev_out = dev;
  return 0;

#ifdef CONFIG_SV6621_PM
stop_roam_thread:
  dev->roam_shutdown = true;
  nxsem_post(&dev->roam_sem);
  nxsem_wait_uninterruptible(&dev->roam_exit_sem);
#endif
destroy_roam_exit_sem:
  nxsem_destroy(&dev->roam_exit_sem);
destroy_roam_sem:
  nxsem_destroy(&dev->roam_sem);
stop_recovery_thread:
  dev->recovery_shutdown = true;
  nxsem_post(&dev->recovery_sem);
  nxsem_wait_uninterruptible(&dev->recovery_exit_sem);
destroy_recovery_exit_sem:
  nxsem_destroy(&dev->recovery_exit_sem);
destroy_recovery_sem:
  nxsem_destroy(&dev->recovery_sem);
#ifdef CONFIG_SV6621_BLUETOOTH
detach_bluetooth:
  sv6621_bluetooth_detach(dev);
#endif
unsubscribe_command:
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_WIFI_COMMAND,
                            sv6621_command_channel_consumer, &dev->command);
unsubscribe_service:
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_LOOPCHECK,
                            sv6621_service_channel_consumer, &dev->service);
deinit_rx:
  sv6621_rx_deinit(&dev->rx);
deinit_service:
  sv6621_service_deinit(&dev->service);
deinit_wpa:
  sv6621_data_set_eapol_input(&dev->data, NULL, NULL);
  sv6621_wpa_deinit(&dev->wpa);
deinit_station:
  sv6621_station_deinit(&dev->station);
deinit_command:
  sv6621_command_engine_deinit(&dev->command);
deinit_scheduled_scan:
  sv6621_sched_scan_deinit(&dev->scheduled_scan);
deinit_scan:
  sv6621_scan_controller_deinit(&dev->scan);
deinit_data:
  sv6621_data_deinit(&dev->data);
deinit_tx:
  sv6621_tx_deinit(&dev->tx);
deinit_router:
  sv6621_packet_router_deinit(&dev->router);
destroy_status_lock:
  nxmutex_destroy(&dev->status_lock);
destroy_lifecycle_lock:
  nxmutex_destroy(&dev->lifecycle_lock);
free_device:
  kmm_free(dev);
  return ret;
}

void sv6621_destroy(FAR struct sv6621_dev_s *dev)
{
  if (dev == NULL)
    {
      return;
    }

#ifdef CONFIG_SV6621_PM
  if (dev->pm_registered)
    {
      pm_unregister(&dev->pm_callback);
      dev->pm_registered = false;
    }

  work_cancel_sync(LPWORK, &dev->pm_resume_work);
#endif

  dev->config.event = NULL;
  if (nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->recovery_shutdown = true;
      dev->roam_shutdown = true;
      nxmutex_unlock(&dev->status_lock);
    }

  sv6621_stop(dev);
  nxsem_post(&dev->roam_sem);
  nxsem_wait_uninterruptible(&dev->roam_exit_sem);
  nxsem_post(&dev->recovery_sem);
  nxsem_wait_uninterruptible(&dev->recovery_exit_sem);
  work_cancel_sync(LPWORK, &dev->thermal_work);
  work_cancel_sync(LPWORK, &dev->security_work);
  work_cancel_sync(LPWORK, &dev->signal_work);
  work_cancel_sync(LPWORK, &dev->event_work);
  work_cancel_sync(LPWORK, &dev->scan_work);
  work_cancel_sync(LPWORK, &dev->station_work);
#ifdef CONFIG_SV6621_BLUETOOTH
  sv6621_bluetooth_detach(dev);
#endif
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_WIFI_COMMAND,
                            sv6621_command_channel_consumer, &dev->command);
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_LOOPCHECK,
                            sv6621_service_channel_consumer, &dev->service);
  sv6621_rx_deinit(&dev->rx);
#ifdef CONFIG_NET
  sv6621_network_deinit(&dev->network);
#endif
  sv6621_service_deinit(&dev->service);
  sv6621_data_set_eapol_input(&dev->data, NULL, NULL);
  sv6621_wpa_deinit(&dev->wpa);
  sv6621_station_deinit(&dev->station);
  if (dev->ap_initialized)
    {
      sv6621_ap_deinit(&dev->ap);
    }

  sv6621_sched_scan_deinit(&dev->scheduled_scan);
  sv6621_scan_controller_deinit(&dev->scan);
  sv6621_command_engine_deinit(&dev->command);
  sv6621_data_deinit(&dev->data);
  sv6621_tx_deinit(&dev->tx);
  sv6621_packet_router_deinit(&dev->router);
  nxsem_destroy(&dev->roam_exit_sem);
  nxsem_destroy(&dev->roam_sem);
  nxsem_destroy(&dev->recovery_exit_sem);
  nxsem_destroy(&dev->recovery_sem);
  nxmutex_destroy(&dev->status_lock);
  nxmutex_destroy(&dev->lifecycle_lock);
  kmm_free(dev);
}

int sv6621_start(FAR struct sv6621_dev_s *dev)
{
  enum sv6621_state_e state;
  bool rx_started = false;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  state = dev->status.state;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_WIFI_READY)
    {
      ret = 0;
      goto unlock_lifecycle;
    }

  if (state != SV6621_STATE_OFF && state != SV6621_STATE_FAILED)
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

  ret = sv6621_core_set_state(dev, SV6621_STATE_STARTING, 0);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto fail;
    }

  dev->thermal_blocked = false;
  nxmutex_unlock(&dev->status_lock);

  ret = dev->config.transport->ops->open(dev->config.transport);
  if (ret < 0)
    {
      goto fail;
    }

  dev->transport_open = true;
  ret = dev->config.board_ops->power_on(dev->config.board_arg);
  if (ret < 0)
    {
      goto fail;
    }

  dev->powered = true;
  ret = dev->config.transport->ops->enumerate(dev->config.transport);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_service_reset(&dev->service);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_command_reset(&dev->command);
  if (ret < 0)
    {
      goto fail;
    }

  sv6621_data_reset_credits(&dev->data);
  sv6621_data_reset_fragments(&dev->data);
  sv6621_data_reset_ba(&dev->data);
  ret = sv6621_data_set_tx_block(&dev->data, UINT8_MAX, false);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_firmware_download(
      dev->config.transport, dev->config.iram.data, dev->config.iram.length,
      dev->config.dram.data, dev->config.dram.length, dev->config.nvram.data,
      dev->config.nvram.length);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_rx_start(&dev->rx);
  if (ret < 0)
    {
      goto fail;
    }

  rx_started = true;

  ret = sv6621_service_wait_bsp(&dev->service, SV6621_CORE_BSP_TIMEOUT_MS);
  if (ret < 0)
    {
      uint8_t intx = 0;
      uint8_t fifo = 0;
      int intx_ret;
      int fifo_ret;

      intx_ret = dev->config.transport->ops->read_byte(
          dev->config.transport, SV6621_SDIO_FUNCTION_CONTROL, 0x05, &intx);
      fifo_ret = dev->config.transport->ops->read_byte(
          dev->config.transport, SV6621_SDIO_FUNCTION_CONTROL, 0x181, &fifo);
      syslog(LOG_ERR,
             "SV6621 BSP wait: ret=%d intx=%02x/%d fifo=%02x/%d"
             " irq=%" PRIu32 " bursts=%" PRIu32 " packets=%" PRIu32
             " errors=%" PRIu32 "\n",
             ret, intx, intx_ret, fifo, fifo_ret, dev->rx.stats.interrupts,
             dev->rx.stats.bursts, dev->rx.stats.packets,
             dev->rx.stats.transport_errors);
      goto fail;
    }

  ret = sv6621_core_set_state(dev, SV6621_STATE_BSP_READY, 0);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_core_set_state(dev, SV6621_STATE_WIFI_STARTING, 0);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_service_start_wifi(&dev->service, dev->config.transport,
                                  SV6621_CORE_WIFI_TIMEOUT_MS);
  if (ret < 0)
    {
      uint8_t intx = 0;
      uint8_t fifo = 0;

      dev->config.transport->ops->read_byte(
          dev->config.transport, SV6621_SDIO_FUNCTION_CONTROL, 0x05, &intx);
      dev->config.transport->ops->read_byte(
          dev->config.transport, SV6621_SDIO_FUNCTION_CONTROL, 0x181, &fifo);
      syslog(LOG_ERR,
             "SV6621 WiFi ready wait failed: ret=%d intx=%02x fifo=%02x"
             " irq=%" PRIu32 " bursts=%" PRIu32 " packets=%" PRIu32 "\n",
             ret, intx, fifo, dev->rx.stats.interrupts, dev->rx.stats.bursts,
             dev->rx.stats.packets);
      goto fail;
    }

  ret = sv6621_wifi_sync_versions(&dev->command);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      goto fail;
    }

  ret = sv6621_wifi_get_info(&dev->command, dev->config.board_ops,
                             dev->config.board_arg, &dev->wifi_info);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      goto fail;
    }

  if (!dev->ap_initialized)
    {
      ret =
          sv6621_ap_init(&dev->ap, &dev->command, dev->wifi_info.max_stations,
                         dev->wifi_info.mac, sv6621_core_command_error, dev,
                         sv6621_core_ap_client, dev);
      if (ret < 0)
        {
          goto fail;
        }

      dev->ap_initialized = true;
    }

  ret = sv6621_station_configure_ht(
      &dev->station, dev->wifi_info.ht_capabilities,
      dev->wifi_info.ht_extended_capabilities,
      dev->wifi_info.ht_ampdu_parameters, dev->wifi_info.ht_tx_mcs,
      dev->wifi_info.ht_rx_mcs);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_station_configure_vht(
      &dev->station, dev->wifi_info.vht_capabilities,
      dev->wifi_info.vht_tx_mcs, dev->wifi_info.vht_rx_mcs);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_station_configure_bandwidth(
      &dev->station, dev->wifi_info.bandwidth_capabilities);
  if (ret < 0)
    {
      goto fail;
    }

  sv6621_data_set_pn_reuse(&dev->data, (dev->wifi_info.private_capabilities &
                                        SV6621_WIFI_PRIVATE_PN_REUSE) != 0);

  ret = sv6621_wifi_configure_baseline(&dev->command);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_wifi_download_calibration(&dev->command,
                                         dev->config.calibration.data,
                                         dev->config.calibration.length);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      goto fail;
    }

  ret = sv6621_regulatory_set_domain(&dev->command, &dev->regulatory);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_wifi_open_station(&dev->command, dev->wifi_info.mac);
  if (ret < 0)
    {
      goto fail;
    }

  dev->station_open = true;

#ifdef CONFIG_NET
  if (!dev->network.registered)
    {
      ret = sv6621_network_init(&dev->network, dev, &dev->data, &dev->command,
                                dev->wifi_info.max_multicast_addresses,
                                dev->wifi_info.mac);
      if (ret < 0)
        {
          goto fail;
        }
    }

  ret = sv6621_network_sync_multicast(&dev->network);
  if (ret < 0)
    {
      goto fail;
    }
#endif

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto fail;
    }

  memcpy(dev->status.mac, dev->wifi_info.mac, SV6621_MAC_LENGTH);
  nxmutex_unlock(&dev->status_lock);

  ret = sv6621_core_set_state(dev, SV6621_STATE_WIFI_READY, 0);
  if (ret < 0)
    {
      goto fail;
    }

  nxmutex_unlock(&dev->lifecycle_lock);
  state = SV6621_STATE_WIFI_READY;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));

  return 0;

fail:
#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, false, NULL);
#endif
  if (dev->station_open)
    {
      sv6621_wifi_close_station(&dev->command);
      dev->station_open = false;
    }

  if (rx_started)
    {
      sv6621_rx_stop(&dev->rx);
    }

  if (dev->transport_open)
    {
      dev->config.transport->ops->close(dev->config.transport);
      dev->transport_open = false;
    }

  if (dev->powered)
    {
      dev->config.board_ops->power_off(dev->config.board_arg);
      dev->powered = false;
    }

  sv6621_core_set_state(dev, SV6621_STATE_FAILED, ret);
  nxmutex_unlock(&dev->lifecycle_lock);
  state = SV6621_STATE_FAILED;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  sv6621_core_report(dev, SV6621_EVENT_FATAL, &ret, sizeof(ret));
  return ret;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

#ifdef CONFIG_SV6621_BLUETOOTH
int sv6621_start_bluetooth(FAR struct sv6621_dev_s *dev)
{
  enum sv6621_state_e state;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return ret;
    }

  state = dev->status.state;
  nxmutex_unlock(&dev->status_lock);
  if (state != SV6621_STATE_WIFI_READY)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return -EAGAIN;
    }

  ret = sv6621_bluetooth_start(dev, &dev->config.bluetooth_nv,
                               dev->config.bluetooth_device_id);
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

int sv6621_stop_bluetooth(FAR struct sv6621_dev_s *dev)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_bluetooth_stop(dev);
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}
#endif

int sv6621_stop(FAR struct sv6621_dev_s *dev)
{
  enum sv6621_state_e state;
  uint32_t recovery_count;
  bool ap_was_active;
  int ap_ret = 0;
  int scheduled_scan_ret = 0;
  int scan_ret = 0;
  int close_ret = 0;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return ret;
    }

  state = dev->status.state;
  recovery_count = dev->status.recovery_count;
  dev->recovery_pending = false;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_OFF)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return 0;
    }

  sv6621_core_set_state(dev, SV6621_STATE_STOPPING, 0);
#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, false, NULL);
#endif
  ap_was_active = dev->ap_initialized && sv6621_ap_is_active(&dev->ap);
  scheduled_scan_ret = sv6621_sched_scan_cancel(&dev->scheduled_scan);
  scan_ret = sv6621_scan_controller_cancel(&dev->scan);
  sv6621_station_disconnect(&dev->station, 3);
  sv6621_wpa_disconnected(&dev->wpa, -ESHUTDOWN);
  sv6621_station_reset(&dev->station, -ESHUTDOWN);
  if (ap_was_active)
    {
      ap_ret = sv6621_ap_disable(&dev->ap);
      if (ap_ret < 0)
        {
          sv6621_ap_reset(&dev->ap);
        }

      close_ret = sv6621_wifi_close_access_point(&dev->command,
                                                 SV6621_CORE_AP_INSTANCE);
      dev->station_open = false;
    }
  else if (dev->station_open)
    {
      close_ret = sv6621_wifi_close_station(&dev->command);
      dev->station_open = false;
    }

  sv6621_command_cancel(&dev->command, -ESHUTDOWN);
#ifdef CONFIG_SV6621_BLUETOOTH
  sv6621_bluetooth_offline(dev, -ESHUTDOWN);
#endif
  sv6621_data_reset_credits(&dev->data);
  sv6621_data_set_tx_block(&dev->data, UINT8_MAX, false);
  sv6621_rx_stop(&dev->rx);
  sv6621_data_reset_fragments(&dev->data);
  sv6621_data_reset_ba(&dev->data);
  dev->suspended = false;
  if (dev->transport_open)
    {
      dev->config.transport->ops->close(dev->config.transport);
      dev->transport_open = false;
    }

  if (dev->powered)
    {
      dev->config.board_ops->power_off(dev->config.board_arg);
      dev->powered = false;
    }

  sv6621_service_reset(&dev->service);
  ret = nxmutex_lock(&dev->status_lock);
  if (ret >= 0)
    {
      memset(&dev->status, 0, sizeof(dev->status));
      dev->status.state = SV6621_STATE_OFF;
      dev->status.recovery_count = recovery_count;
      dev->thermal_blocked = false;
      nxmutex_unlock(&dev->status_lock);
    }

  nxmutex_unlock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  state = SV6621_STATE_OFF;
  if (ap_was_active)
    {
      sv6621_core_report(dev, SV6621_EVENT_AP_STOPPED, NULL, 0);
    }

  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  if (scan_ret < 0)
    {
      return scan_ret;
    }

  if (scheduled_scan_ret < 0)
    {
      return scheduled_scan_ret;
    }

  if (ap_ret < 0)
    {
      return ap_ret;
    }

  return close_ret < 0 ? close_ret : 0;
}

int sv6621_get_status(FAR struct sv6621_dev_s *dev,
                      FAR struct sv6621_status_s *status)
{
  int ret;

  if (dev == NULL || status == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  *status = dev->status;
  nxmutex_unlock(&dev->status_lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_get_link_stats
 ****************************************************************************/

int sv6621_get_link_stats(FAR struct sv6621_dev_s *dev,
                          FAR struct sv6621_link_stats_s *stats)
{
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint8_t instance;
  int ret;

  if (dev == NULL || stats == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY || !dev->station_connected)
    {
      ret = -ENOTCONN;
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  memcpy(bssid, dev->status.bssid, sizeof(bssid));
  nxmutex_unlock(&dev->status_lock);

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  instance = dev->station.peer.instance;
  nxmutex_unlock(&dev->station.lock);
  ret = sv6621_stats_query(&dev->command, instance, bssid, stats);
  if (ret == 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->status.signal_dbm = stats->signal_dbm;
      nxmutex_unlock(&dev->status_lock);
    }

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_get_driver_stats
 ****************************************************************************/

int sv6621_get_driver_stats(FAR struct sv6621_dev_s *dev,
                            FAR struct sv6621_driver_stats_s *stats)
{
  int ret;

  if (dev == NULL || stats == NULL)
    {
      return -EINVAL;
    }

  memset(stats, 0, sizeof(*stats));
  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  stats->version = SV6621_DRIVER_STATS_VERSION;
  stats->size = sizeof(*stats);
  stats->commands = dev->command.stats.commands;
  stats->command_timeouts = dev->command.stats.timeouts;
  stats->command_cancelled = dev->command.stats.cancelled;
  stats->command_malformed = dev->command.stats.malformed;
  stats->stale_acknowledgements = dev->command.stats.stale_acknowledgements;
  stats->missed_events = dev->command.stats.missed_events;
  stats->rx_interrupts = dev->rx.stats.interrupts;
  stats->rx_bursts = dev->rx.stats.bursts;
  stats->rx_packets = dev->rx.stats.packets;
  stats->rx_malformed_bursts = dev->rx.stats.malformed_bursts;
  stats->rx_transport_errors = dev->rx.stats.transport_errors;
  stats->tx_packets = dev->tx.stats.packets;
  stats->tx_bytes = dev->tx.stats.bytes;
  stats->tx_transport_errors = dev->tx.stats.transport_errors;
  stats->tx_doorbell_errors = dev->tx.stats.doorbell_errors;
  stats->data_received = dev->data.stats.received;
  stats->data_received_bytes = dev->data.stats.received_bytes;
  stats->data_malformed = dev->data.stats.malformed;
  stats->data_transmitted = dev->data.stats.transmitted;
  stats->data_transmitted_bytes = dev->data.stats.transmitted_bytes;
  stats->data_transmit_errors = dev->data.stats.transmit_errors;
  stats->credit_starvations = dev->data.stats.credit_starvations;
  stats->fragments = dev->data.stats.fragments;
  stats->fragment_drops = dev->data.stats.fragment_drops;
  stats->fragment_pn_drops = dev->data.stats.fragment_pn_drops;
  stats->reassembled = dev->data.stats.reassembled;
  stats->reorder_duplicates = dev->data.stats.reorder_duplicates;
  stats->reorder_stale = dev->data.stats.reorder_stale;
  stats->reorder_allocation_failures =
      dev->data.stats.reorder_allocation_failures;
  stats->reorder_timeouts = dev->data.stats.reorder_timeouts;
  stats->reorder_schedule_errors = dev->data.stats.reorder_schedule_errors;
  stats->recovery_count = dev->status.recovery_count;
  stats->unprotected_frames = dev->status.unprotected_frames;
  stats->mic_failures = dev->status.mic_failures;
  stats->mic_failures_dropped = dev->status.mic_failures_dropped;
  stats->signal_events_dropped = dev->status.signal_events_dropped;
  nxmutex_unlock(&dev->status_lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_set_country
 ****************************************************************************/

int sv6621_set_country(FAR struct sv6621_dev_s *dev, FAR const char country[2])
{
  struct sv6621_scan_channel_s
      channels[SV6621_REGULATORY_SCAN_CHANNEL_CAPACITY];
  FAR const struct sv6621_regulatory_domain_s *domain = NULL;
  size_t channel_count;
  size_t index;
  char alpha2[2];
  int ret;

  if (dev == NULL || country == NULL)
    {
      return -EINVAL;
    }

  alpha2[0] = toupper((unsigned char)country[0]);
  alpha2[1] = toupper((unsigned char)country[1]);
  if (!((alpha2[0] == '0' && alpha2[1] == '0') ||
        (isalpha((unsigned char)alpha2[0]) &&
         isalpha((unsigned char)alpha2[1]))))
    {
      return -EINVAL;
    }

  for (index = 0; index < dev->config.regulatory_domain_count; index++)
    {
      FAR const struct sv6621_regulatory_domain_s *candidate =
          &dev->config.regulatory_domains[index];

      if (toupper((unsigned char)candidate->country[0]) == alpha2[0] &&
          toupper((unsigned char)candidate->country[1]) == alpha2[1])
        {
          domain = candidate;
          break;
        }
    }

  if (domain == NULL)
    {
      return -ENOENT;
    }

  ret = sv6621_regulatory_scan_channels(
      domain, channels, SV6621_REGULATORY_SCAN_CHANNEL_CAPACITY,
      &channel_count);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->regulatory.country[0] == alpha2[0] &&
      dev->regulatory.country[1] == alpha2[1])
    {
      ret = 0;
      goto unlock_country;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_country;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }
  else if (dev->station_connected)
    {
      ret = -EBUSY;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_country;
    }

  ret = sv6621_scan_controller_cancel(&dev->scan);
  if (ret < 0)
    {
      goto unlock_country;
    }

  ret = sv6621_regulatory_set_domain(&dev->command, domain);
  if (ret == 0)
    {
      dev->regulatory = *domain;
      memcpy(dev->scan_channels, channels, channel_count * sizeof(*channels));
      dev->scan_channel_count = channel_count;
    }

unlock_country:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_get_country
 ****************************************************************************/

int sv6621_get_country(FAR struct sv6621_dev_s *dev, FAR char country[3])
{
  int ret;

  if (dev == NULL || country == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  country[0] = dev->regulatory.country[0];
  country[1] = dev->regulatory.country[1];
  country[2] = '\0';
  nxmutex_unlock(&dev->lifecycle_lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_scan
 ****************************************************************************/

int sv6621_scan(FAR struct sv6621_dev_s *dev)
{
  if (dev == NULL)
    {
      return -EINVAL;
    }

  return sv6621_scan_selected(dev, dev->scan_channels, dev->scan_channel_count,
                              NULL, 0);
}

/****************************************************************************
 * Name: sv6621_scan_selected
 ****************************************************************************/

int sv6621_scan_selected(FAR struct sv6621_dev_s *dev,
                         FAR const struct sv6621_scan_channel_s *channels,
                         size_t channel_count, FAR const uint8_t *ssid,
                         size_t ssid_length)
{
  int ret;

  if (dev == NULL || channels == NULL || channel_count == 0 ||
      ssid_length > SV6621_SSID_MAX_LENGTH ||
      (ssid_length != 0 && ssid == NULL))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }
  else if (dev->scan_reporting)
    {
      ret = -EBUSY;
    }
  else
    {
      ret = 0;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_scan_controller_begin(&dev->scan, channels, channel_count, ssid,
                                     ssid_length);

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_core_connect_locked
 ****************************************************************************/

static int
sv6621_core_connect_locked(FAR struct sv6621_dev_s *dev,
                           FAR const struct sv6621_connect_s *connection,
                           bool roaming)
{
  FAR struct sv6621_scan_entry_s *target = NULL;
  uint8_t sae_pmk[SV6621_SAE_PMK_SIZE];
  uint8_t sae_pmkid[SV6621_SAE_PMKID_SIZE];
  enum sv6621_station_state_e expected_state =
      roaming ? SV6621_STATION_CONNECTED : SV6621_STATION_IDLE;
  bool wpa_prepared = false;
  int ret;

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->station.state != expected_state)
    {
      ret = -EBUSY;
    }

  nxmutex_unlock(&dev->station.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!roaming)
    {
      ret =
          sv6621_station_set_local_address(&dev->station, dev->wifi_info.mac);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (connection->security == SV6621_SECURITY_WPA2_PSK ||
      connection->security == SV6621_SECURITY_WPA2_WPA3_PSK)
    {
      target = kmm_malloc(sizeof(*target));
      if (target == NULL)
        {
          return -ENOMEM;
        }

      ret = sv6621_scan_cache_find(&dev->scan.cache, connection, target);
      if (ret < 0)
        {
          goto free_target;
        }

      ret = sv6621_wpa_prepare(&dev->wpa, connection, dev->wifi_info.mac,
                               target->bss.bssid);
      if (ret < 0)
        {
          goto free_target;
        }

      wpa_prepared = true;
      kmm_free(target);
      target = NULL;
    }

  ret = sv6621_station_connect(&dev->station, connection, roaming,
                               SV6621_CORE_CONNECT_TIMEOUT_MS);
  if (ret < 0)
    {
      goto cancel_wpa;
    }

  if (connection->security == SV6621_SECURITY_WPA3_SAE)
    {
      ret = sv6621_station_get_sae_pmk(&dev->station, sae_pmk, sae_pmkid);
      if (ret < 0)
        {
          goto cancel_wpa;
        }

      ret = sv6621_wpa_prepare_pmk(&dev->wpa, sae_pmk, SV6621_WPA_KEY_MGMT_SAE,
                                   dev->wifi_info.mac,
                                   dev->station.target.bss.bssid);
      sv6621_sae_zeroize(sae_pmk, sizeof(sae_pmk));
      sv6621_sae_zeroize(sae_pmkid, sizeof(sae_pmkid));
      if (ret < 0)
        {
          goto cancel_wpa;
        }

      wpa_prepared = true;
    }

  if (wpa_prepared)
    {
      ret = sv6621_wpa_run(&dev->wpa, &dev->station.peer,
                           SV6621_CORE_HANDSHAKE_TIMEOUT_MS);
      if (ret < 0)
        {
          int disconnect_ret = sv6621_station_disconnect(&dev->station, 1);

          if (disconnect_ret >= 0)
            {
              sv6621_wpa_disconnected(&dev->wpa, ret);
            }
          else
            {
              sv6621_wpa_cancel(&dev->wpa, ret);
            }

          wpa_prepared = false;
          goto cancel_wpa;
        }
    }

  return 0;

free_target:
  kmm_free(target);
cancel_wpa:
  if (wpa_prepared)
    {
      sv6621_wpa_cancel(&dev->wpa, ret);
    }

  return ret;
}

int sv6621_connect(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_connect_s *request)
{
  struct sv6621_connect_s resolved;
  FAR const struct sv6621_connect_s *connection = request;
  FAR struct sv6621_scan_entry_s *target = NULL;
  int ret;

  if (dev == NULL || request == NULL)
    {
      return -EINVAL;
    }

  if (request->security != SV6621_SECURITY_OPEN &&
      request->security != SV6621_SECURITY_WPA2_PSK &&
      request->security != SV6621_SECURITY_WPA2_WPA3_PSK &&
      request->security != SV6621_SECURITY_WPA3_SAE)
    {
      return -EOPNOTSUPP;
    }

  if (request->ssid_length == 0)
    {
      if (!request->bssid_valid)
        {
          return -EINVAL;
        }

      target = kmm_malloc(sizeof(*target));
      if (target == NULL)
        {
          return -ENOMEM;
        }

      ret = sv6621_scan_cache_find(&dev->scan.cache, request, target);
      if (ret < 0)
        {
          kmm_free(target);
          return ret;
        }

      if (target->bss.ssid_length == 0)
        {
          kmm_free(target);
          return -ENOENT;
        }

      resolved = *request;
      memcpy(resolved.ssid, target->bss.ssid, target->bss.ssid_length);
      resolved.ssid_length = target->bss.ssid_length;
      connection = &resolved;
      kmm_free(target);
      target = NULL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_scan_controller_cancel(&dev->scan);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_core_connect_locked(dev, connection, false);

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

int sv6621_disconnect(FAR struct sv6621_dev_s *dev, uint16_t reason)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return ret;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret >= 0)
    {
      ret = sv6621_station_disconnect(&dev->station, reason);
      if (ret >= 0)
        {
          sv6621_wpa_disconnected(&dev->wpa, -ENOTCONN);
        }
    }

  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_set_signal_threshold
 ****************************************************************************/

int sv6621_set_signal_threshold(FAR struct sv6621_dev_s *dev,
                                int32_t threshold_dbm, uint8_t hysteresis_db)
{
  uint8_t instance;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY || !dev->station_connected)
    {
      ret = -ENOTCONN;
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  nxmutex_unlock(&dev->status_lock);
  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  instance = dev->station.peer.instance;
  nxmutex_unlock(&dev->station.lock);
  ret = sv6621_signal_configure(&dev->command, instance, threshold_dbm,
                                hysteresis_db);

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_set_roam_policy
 ****************************************************************************/

int sv6621_set_roam_policy(FAR struct sv6621_dev_s *dev,
                           FAR const struct sv6621_roam_policy_s *policy)
{
  struct sv6621_roam_policy_s previous;
  uint8_t instance = 0;
  bool connected;
  int ret;

  if (dev == NULL || policy == NULL || policy->threshold_dbm > 0 ||
      policy->hysteresis_db > INT8_MAX || policy->minimum_gain_db == 0 ||
      policy->minimum_gain_db > INT8_MAX || policy->cooldown_ms == 0 ||
      policy->cooldown_ms > SV6621_CORE_ROAM_MAX_COOLDOWN_MS)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  previous = dev->roam_policy;
  connected = dev->station_connected;
  if (connected)
    {
      ret = nxmutex_lock(&dev->station.lock);
      if (ret < 0)
        {
          nxmutex_unlock(&dev->status_lock);
          goto unlock_lifecycle;
        }

      instance = dev->station.peer.instance;
      nxmutex_unlock(&dev->station.lock);
    }

  dev->roam_policy = *policy;
  dev->roam_scan_pending = false;
  dev->roam_candidate_ticks_valid = false;
  nxmutex_unlock(&dev->status_lock);

  if (connected && policy->enabled)
    {
      ret = sv6621_signal_configure(&dev->command, instance,
                                    policy->threshold_dbm,
                                    policy->hysteresis_db);
      if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
        {
          dev->roam_policy = previous;
          nxmutex_unlock(&dev->status_lock);
        }
    }
  else
    {
      ret = 0;
    }

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_start_scheduled_scan
 ****************************************************************************/

int sv6621_start_scheduled_scan(
    FAR struct sv6621_dev_s *dev,
    FAR const struct sv6621_sched_scan_request_s *request)
{
  int ret;

  if (dev == NULL || request == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret >= 0)
    {
      ret = sv6621_sched_scan_begin(&dev->scheduled_scan, request);
    }

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_stop_scheduled_scan
 ****************************************************************************/

int sv6621_stop_scheduled_scan(FAR struct sv6621_dev_s *dev)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_sched_scan_cancel(&dev->scheduled_scan);
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_start_ap
 ****************************************************************************/

int sv6621_start_ap(FAR struct sv6621_dev_s *dev,
                    FAR const struct sv6621_ap_config_s *config)
{
  bool report = false;
  int disable_ret;
  int ret;

  if (dev == NULL || config == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_core_ap_ready(dev, config);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, false, NULL);
#endif
  ret = sv6621_wifi_close_station(&dev->command);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  dev->station_open = false;
  ret = sv6621_wifi_open_access_point(&dev->command, SV6621_CORE_AP_INSTANCE,
                                      dev->wifi_info.mac);
  if (ret < 0)
    {
      ret = sv6621_core_restore_station(dev, ret);
      goto unlock_lifecycle;
    }

  ret = sv6621_ap_enable(&dev->ap, SV6621_CORE_AP_INSTANCE, config);
  if (ret < 0)
    {
      ret = sv6621_core_restore_station(dev, ret);
      goto unlock_lifecycle;
    }

#ifdef CONFIG_NET
  {
    struct sv6621_data_tx_context_s context;

    context.peer_index = dev->ap.context.multicast_index;
    context.multicast_index = dev->ap.context.multicast_index;
    context.instance = dev->ap.context.instance;
    context.lmac_id = dev->ap.context.lmac_id;
    context.tid = 0;
    sv6621_network_set_tx_resolver(&dev->network, sv6621_core_ap_resolve_tx,
                                   dev);
    sv6621_network_set_link(&dev->network, true, &context);
  }
#endif

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      disable_ret = sv6621_ap_disable(&dev->ap);
      if (disable_ret < 0)
        {
          sv6621_core_queue_recovery(dev, disable_ret);
        }

      ret = sv6621_core_restore_station(dev, ret);
      goto unlock_lifecycle;
    }

  dev->status.ap_active = true;
  dev->status.ap_client_count = 0;
  memcpy(dev->status.ssid, config->ssid, config->ssid_length);
  dev->status.ssid_length = config->ssid_length;
  dev->status.channel = config->channel;
  dev->status.band = config->band;
  nxmutex_unlock(&dev->status_lock);
  report = true;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  if (report)
    {
      sv6621_core_report(dev, SV6621_EVENT_AP_STARTED, config,
                         sizeof(*config));
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_stop_ap
 ****************************************************************************/

int sv6621_stop_ap(FAR struct sv6621_dev_s *dev)
{
  bool report = false;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }
  else if (!dev->status.ap_active)
    {
      ret = -ENOTCONN;
    }
  else
    {
      ret = 0;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_ap_disable(&dev->ap);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_core_restore_station(dev, 0);
#ifdef CONFIG_NET
  if (ret == 0)
    {
      ret = sv6621_network_sync_multicast(&dev->network);
      if (ret < 0)
        {
          sv6621_core_queue_recovery(dev, ret);
        }
    }
#endif

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      if (ret == 0)
        {
          ret = -EINTR;
        }

      sv6621_core_queue_recovery(dev, ret);
      goto unlock_lifecycle;
    }

  dev->status.ap_active = false;
  dev->status.ap_client_count = 0;
  memset(dev->status.ssid, 0, sizeof(dev->status.ssid));
  dev->status.ssid_length = 0;
  dev->status.channel = 0;
  dev->status.band = SV6621_BAND_2GHZ;
  nxmutex_unlock(&dev->status_lock);
  report = true;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  if (report)
    {
      sv6621_core_report(dev, SV6621_EVENT_AP_STOPPED, NULL, 0);
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_rekey_ap
 ****************************************************************************/

int sv6621_rekey_ap(FAR struct sv6621_dev_s *dev)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret == 0)
    {
      if (dev->status.state != SV6621_STATE_WIFI_READY)
        {
          ret = -ENETDOWN;
        }
      else if (!dev->status.ap_active)
        {
          ret = -ENOTCONN;
        }

      nxmutex_unlock(&dev->status_lock);
    }

  if (ret == 0)
    {
      ret = sv6621_ap_wpa_rekey(&dev->ap.wpa);
    }

  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_suspend
 ****************************************************************************/

int sv6621_suspend(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_suspend_s *config)
{
#ifdef CONFIG_SV6621_PM
  irqstate_t flags;
#endif
  enum sv6621_wpa_state_e wpa_state;
  enum sv6621_station_state_e station_state;
  enum sv6621_state_e state;
  bool scan_active;
  bool connected;
  int ret;

  if (dev == NULL || config == NULL ||
      (config->wake_flags & ~SV6621_WAKE_ALL) != 0 ||
      (!config->wake_enabled && config->wake_flags != 0))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  state = dev->status.state;
  connected = dev->station_connected;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_SUSPENDED)
    {
      ret = 0;
      goto unlock_lifecycle;
    }

  if (state != SV6621_STATE_WIFI_READY)
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

  ret = nxmutex_lock(&dev->scan.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  scan_active = dev->scan.active || dev->scan.stopping;
  nxmutex_unlock(&dev->scan.lock);

  ret = nxmutex_lock(&dev->wpa.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  wpa_state = dev->wpa.state;
  nxmutex_unlock(&dev->wpa.lock);

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  station_state = dev->station.state;
  nxmutex_unlock(&dev->station.lock);
  if (scan_active ||
      (wpa_state != SV6621_WPA_IDLE && wpa_state != SV6621_WPA_COMPLETE) ||
      (station_state != SV6621_STATION_IDLE &&
       station_state != SV6621_STATION_CONNECTED))
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

#ifdef CONFIG_NET
  if (connected)
    {
      ret = sv6621_network_sync_addresses(&dev->network);
      if (ret != 0)
        {
          ret = ret < 0 ? ret : -EREMOTEIO;
          goto unlock_lifecycle;
        }
    }
#endif

  ret = sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP, true);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_power_suspend(&dev->command, config->wake_enabled,
                             config->wake_flags);
  if (ret < 0)
    {
      sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP, false);
      goto unlock_lifecycle;
    }

  ret = sv6621_rx_suspend(&dev->rx);
  if (ret < 0)
    {
      int resume_ret = sv6621_power_resume(&dev->command);

      if (resume_ret != 0)
        {
          ret = resume_ret < 0 ? resume_ret : -EREMOTEIO;
        }

      sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP, false);
      goto unlock_lifecycle;
    }

  dev->suspended = true;
#ifdef CONFIG_SV6621_PM
  flags = spin_lock_irqsave(&dev->pm_lock);
  dev->pm_suspended = true;
  spin_unlock_irqrestore(&dev->pm_lock, flags);
#endif
  ret = sv6621_core_set_state(dev, SV6621_STATE_SUSPENDED, 0);
  nxmutex_unlock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  state = SV6621_STATE_SUSPENDED;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  return 0;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_resume
 ****************************************************************************/

int sv6621_resume(FAR struct sv6621_dev_s *dev)
{
  enum sv6621_state_e state;
#ifdef CONFIG_SV6621_PM
  irqstate_t flags;
#endif
  bool recover = false;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  state = dev->status.state;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_WIFI_READY)
    {
      ret = 0;
      goto unlock_lifecycle;
    }

  if (state != SV6621_STATE_SUSPENDED || !dev->suspended)
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

  ret = sv6621_rx_resume(&dev->rx);
  if (ret < 0)
    {
      recover = true;
      goto unlock_lifecycle;
    }

  ret = sv6621_power_resume(&dev->command);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      recover = true;
      goto unlock_lifecycle;
    }

  ret =
      sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP, false);
  if (ret < 0)
    {
      recover = true;
      goto unlock_lifecycle;
    }

  dev->suspended = false;
#ifdef CONFIG_SV6621_PM
  flags = spin_lock_irqsave(&dev->pm_lock);
  dev->pm_suspended = false;
  spin_unlock_irqrestore(&dev->pm_lock, flags);
#endif
  ret = sv6621_core_set_state(dev, SV6621_STATE_WIFI_READY, 0);
  nxmutex_unlock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_NET
  sv6621_network_credit_available(&dev->network);
#endif
  state = SV6621_STATE_WIFI_READY;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  return 0;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  if (recover)
    {
      sv6621_core_queue_recovery(dev, ret);
    }

  return ret;
}
