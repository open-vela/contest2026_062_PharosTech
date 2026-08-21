/****************************************************************************
 * drivers/drivers/sv6621/sv6621_service.c
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

#include <nuttx/clock.h>

#include <errno.h>
#include <string.h>
#include <syslog.h>

#include "sv6621_protocol.h"
#include "sv6621_service.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SERVICE_WIFI_START 0x01
#define SV6621_SERVICE_BT_START   0x04
#define SV6621_SERVICE_BT_STOP    0x08

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_bsp_ready[] = "trunk_W";
static const uint8_t g_sv6621_wifi_ready[] = "WIFIREADY";
static const uint8_t g_sv6621_bt_ready[] = "BTREADY";
static const uint8_t g_sv6621_assert[] = "BSPASSERT";
static const uint8_t g_sv6621_dump_complete[] = "DUMPDONE";

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool sv6621_service_contains(FAR const uint8_t *payload, size_t length,
                                    FAR const uint8_t *token,
                                    size_t token_length);
static void sv6621_service_log_message(int priority,
                                       FAR const uint8_t *message,
                                       size_t message_length);
static int sv6621_service_wait(FAR struct sv6621_service_s *service,
                               FAR sem_t *completion, FAR bool *ready,
                               uint32_t timeout_ms);
static void sv6621_service_publish(FAR struct sv6621_service_s *service,
                                   enum sv6621_service_event_e event,
                                   FAR const uint8_t *payload, size_t length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_service_contains
 ****************************************************************************/

static bool sv6621_service_contains(FAR const uint8_t *payload, size_t length,
                                    FAR const uint8_t *token,
                                    size_t token_length)
{
  size_t offset;

  if (token_length == 0 || token_length > length)
    {
      return false;
    }

  for (offset = 0; offset <= length - token_length; offset++)
    {
      if (memcmp(payload + offset, token, token_length) == 0)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: sv6621_service_log_message
 ****************************************************************************/

static void sv6621_service_log_message(int priority,
                                       FAR const uint8_t *message,
                                       size_t message_length)
{
  char text[96];
  size_t index;
  size_t count =
      message_length < sizeof(text) - 1 ? message_length : sizeof(text) - 1;

  for (index = 0; index < count; index++)
    {
      text[index] = message[index] >= 0x20 && message[index] < 0x7f
                        ? message[index]
                        : '.';
    }

  text[count] = '\0';
  syslog(priority, "SV6621 firmware: %s\n", text);
}

/****************************************************************************
 * Name: sv6621_service_wait
 ****************************************************************************/

static int sv6621_service_wait(FAR struct sv6621_service_s *service,
                               FAR sem_t *completion, FAR bool *ready,
                               uint32_t timeout_ms)
{
  int ret;

  if (service == NULL || completion == NULL || ready == NULL ||
      timeout_ms == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&service->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (*ready)
    {
      nxmutex_unlock(&service->lock);
      return 0;
    }

  if (service->status.failure < 0)
    {
      ret = service->status.failure;
      nxmutex_unlock(&service->lock);
      return ret;
    }

  nxmutex_unlock(&service->lock);
  ret = nxsem_tickwait(completion, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      return ret == -ETIMEDOUT ? -ETIMEDOUT : ret;
    }

  ret = nxmutex_lock(&service->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (*ready)
    {
      ret = 0;
    }
  else
    {
      ret = service->status.failure;
      if (ret >= 0)
        {
          ret = -EIO;
        }
    }

  nxmutex_unlock(&service->lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_service_publish
 ****************************************************************************/

static void sv6621_service_publish(FAR struct sv6621_service_s *service,
                                   enum sv6621_service_event_e event,
                                   FAR const uint8_t *payload, size_t length)
{
  sv6621_service_event_t callback;
  FAR void *arg;

  if (nxmutex_lock(&service->lock) < 0)
    {
      return;
    }

  callback = service->event;
  arg = service->event_arg;
  nxmutex_unlock(&service->lock);
  if (callback != NULL)
    {
      callback(event, payload, length, arg);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_service_init(FAR struct sv6621_service_s *service,
                        sv6621_service_event_t event, FAR void *event_arg)
{
  int ret;

  if (service == NULL)
    {
      return -EINVAL;
    }

  memset(service, 0, sizeof(*service));
  ret = nxmutex_init(&service->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxsem_init(&service->bsp_completion, 0, 0);
  if (ret < 0)
    {
      nxmutex_destroy(&service->lock);
      return ret;
    }

  ret = nxsem_init(&service->wifi_completion, 0, 0);
  if (ret < 0)
    {
      nxsem_destroy(&service->bsp_completion);
      nxmutex_destroy(&service->lock);
      return ret;
    }

  ret = nxsem_init(&service->bt_completion, 0, 0);
  if (ret < 0)
    {
      nxsem_destroy(&service->wifi_completion);
      nxsem_destroy(&service->bsp_completion);
      nxmutex_destroy(&service->lock);
      return ret;
    }

  service->event = event;
  service->event_arg = event_arg;
  return 0;
}

void sv6621_service_deinit(FAR struct sv6621_service_s *service)
{
  if (service != NULL)
    {
      nxsem_destroy(&service->bt_completion);
      nxsem_destroy(&service->wifi_completion);
      nxsem_destroy(&service->bsp_completion);
      nxmutex_destroy(&service->lock);
    }
}

int sv6621_service_reset(FAR struct sv6621_service_s *service)
{
  int ret;

  if (service == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&service->lock);
  if (ret < 0)
    {
      return ret;
    }

  memset(&service->status, 0, sizeof(service->status));
  nxsem_reset(&service->bsp_completion, 0);
  nxsem_reset(&service->wifi_completion, 0);
  nxsem_reset(&service->bt_completion, 0);
  nxmutex_unlock(&service->lock);
  return 0;
}

int sv6621_service_get_status(FAR struct sv6621_service_s *service,
                              FAR struct sv6621_service_status_s *status)
{
  int ret;

  if (service == NULL || status == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&service->lock);
  if (ret < 0)
    {
      return ret;
    }

  *status = service->status;
  nxmutex_unlock(&service->lock);
  return 0;
}

int sv6621_service_wait_bsp(FAR struct sv6621_service_s *service,
                            uint32_t timeout_ms)
{
  if (service == NULL)
    {
      return -EINVAL;
    }

  return sv6621_service_wait(service, &service->bsp_completion,
                             &service->status.bsp_ready, timeout_ms);
}

int sv6621_service_start_wifi(FAR struct sv6621_service_s *service,
                              FAR struct sv6621_transport_s *transport,
                              uint32_t timeout_ms)
{
  struct sv6621_service_status_s status;
  int ret;

  if (service == NULL || timeout_ms == 0)
    {
      return -EINVAL;
    }

  ret = sv6621_transport_validate(transport);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_service_get_status(service, &status);
  if (ret < 0 || status.wifi_ready)
    {
      return ret;
    }

  if (status.failure < 0)
    {
      return status.failure;
    }

  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_AP_TO_CP_IRQ,
                                   SV6621_SERVICE_WIFI_START);
  if (ret < 0)
    {
      return ret;
    }

  return sv6621_service_wait(service, &service->wifi_completion,
                             &service->status.wifi_ready, timeout_ms);
}

int sv6621_service_start_bluetooth(FAR struct sv6621_service_s *service,
                                   FAR struct sv6621_transport_s *transport,
                                   uint32_t timeout_ms)
{
  struct sv6621_service_status_s status;
  int ret;

  if (service == NULL || timeout_ms == 0)
    {
      return -EINVAL;
    }

  ret = sv6621_transport_validate(transport);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_service_get_status(service, &status);
  if (ret < 0 || status.bt_ready)
    {
      return ret;
    }

  if (!status.bsp_ready)
    {
      return -EAGAIN;
    }

  if (status.failure < 0)
    {
      return status.failure;
    }

  nxsem_reset(&service->bt_completion, 0);
  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_AP_TO_CP_IRQ,
                                   SV6621_SERVICE_BT_START);
  if (ret < 0)
    {
      return ret;
    }

  return sv6621_service_wait(service, &service->bt_completion,
                             &service->status.bt_ready, timeout_ms);
}

int sv6621_service_stop_bluetooth(FAR struct sv6621_service_s *service,
                                  FAR struct sv6621_transport_s *transport)
{
  int ret;

  if (service == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_transport_validate(transport);
  if (ret < 0)
    {
      return ret;
    }

  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_AP_TO_CP_IRQ,
                                   SV6621_SERVICE_BT_STOP);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&service->lock);
  if (ret < 0)
    {
      return ret;
    }

  service->status.bt_ready = false;
  nxsem_reset(&service->bt_completion, 0);
  nxmutex_unlock(&service->lock);
  return 0;
}

void sv6621_service_channel_consumer(uint8_t channel,
                                     FAR const uint8_t encoded[4],
                                     FAR const uint8_t *payload, size_t length,
                                     FAR void *arg)
{
  FAR struct sv6621_service_s *service = arg;
  FAR const uint8_t *message;
  size_t message_length;
  enum sv6621_service_event_e event;
  FAR sem_t *completion = NULL;
  FAR bool *ready = NULL;
  bool failure = false;

  (void)encoded;

  if (service == NULL || payload == NULL ||
      channel != SV6621_CHANNEL_LOOPCHECK ||
      length <= SV6621_RX_LINK_HEADER_SIZE)
    {
      return;
    }

  message = payload + SV6621_RX_LINK_HEADER_SIZE;
  message_length = length - SV6621_RX_LINK_HEADER_SIZE;

  if (sv6621_service_contains(message, message_length, g_sv6621_assert,
                              sizeof(g_sv6621_assert) - 1))
    {
      event = SV6621_SERVICE_EVENT_ASSERT;
      failure = true;
    }
  else if (sv6621_service_contains(message, message_length,
                                   g_sv6621_dump_complete,
                                   sizeof(g_sv6621_dump_complete) - 1))
    {
      event = SV6621_SERVICE_EVENT_DUMP_COMPLETE;
      failure = true;
    }
  else if (sv6621_service_contains(message, message_length, g_sv6621_bsp_ready,
                                   sizeof(g_sv6621_bsp_ready) - 1))
    {
      event = SV6621_SERVICE_EVENT_BSP_READY;
      completion = &service->bsp_completion;
      ready = &service->status.bsp_ready;
    }
  else if (sv6621_service_contains(message, message_length,
                                   g_sv6621_wifi_ready,
                                   sizeof(g_sv6621_wifi_ready) - 1))
    {
      event = SV6621_SERVICE_EVENT_WIFI_READY;
      completion = &service->wifi_completion;
      ready = &service->status.wifi_ready;
    }
  else if (sv6621_service_contains(message, message_length, g_sv6621_bt_ready,
                                   sizeof(g_sv6621_bt_ready) - 1))
    {
      event = SV6621_SERVICE_EVENT_BT_READY;
      completion = &service->bt_completion;
      ready = &service->status.bt_ready;
    }
  else
    {
#ifdef CONFIG_DEBUG_WIRELESS_INFO
      sv6621_service_log_message(LOG_DEBUG, message, message_length);
#endif
      return;
    }

  sv6621_service_log_message(failure ? LOG_ERR : LOG_INFO, message,
                             message_length);

  if (nxmutex_lock(&service->lock) < 0)
    {
      return;
    }

  if (failure)
    {
      service->status.failure = -EIO;
      nxsem_post(&service->bsp_completion);
      nxsem_post(&service->wifi_completion);
      nxsem_post(&service->bt_completion);
    }
  else if (!*ready)
    {
      *ready = true;
      if (completion != NULL)
        {
          nxsem_post(completion);
        }
    }

  nxmutex_unlock(&service->lock);
  sv6621_service_publish(service, event, message, message_length);
}
