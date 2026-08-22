/****************************************************************************
 * drivers/drivers/sv6621/sv6621_bluetooth.c
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
 * Transport for the Bluetooth controller inside the SV6621 combo.
 *
 * Bluetooth shares the Wi-Fi SDIO function and packet window.  Channels 2,
 * 3, 4 and 5 carry H4 command/event, SCO, ISO and ACL frames respectively.
 * CP receive packets contain a 12-byte link header before the H4 frame;
 * host transmit packets contain the H4 frame directly.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_SV6621_BLUETOOTH

#include <debug.h>
#include <errno.h>
#include <string.h>

#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>
#include <nuttx/wireless/bluetooth/bt_driver.h>

#include "sv6621_bluetooth.h"
#include "sv6621_core.h"
#include "sv6621_packet.h"
#include "sv6621_protocol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_BT_LINK_HEADER_SIZE          12
#define SV6621_BT_H4_COMMAND                0x01
#define SV6621_BT_H4_ACL                    0x02
#define SV6621_BT_H4_SCO                    0x03
#define SV6621_BT_H4_EVENT                  0x04
#define SV6621_BT_H4_ISO                    0x05
#define SV6621_BT_EVENT_COMMAND_COMPLETE    0x0e

#define SV6621_BT_OPCODE_RESET              0x0c03
#define SV6621_BT_OPCODE_READ_LOCAL_VERSION 0x1001
#define SV6621_BT_OPCODE_DOWNLOAD_NV        0xfc80

#define SV6621_BT_CONTROLLER_6160_LITE      0x5302
#define SV6621_BT_NV_BLOCK_SIZE             252
#define SV6621_BT_NV_HEADER_SIZE            4
#define SV6621_BT_NV_RECORD_HEADER          3
#define SV6621_BT_NV_LOG_TAG                0x05
#define SV6621_BT_COMMAND_TIMEOUT_MS        1000
#define SV6621_BT_READY_TIMEOUT_MS          3000
#define SV6621_BT_STOP_GRACE_MS             250
#define SV6621_BT_MAX_FRAME                 2048
#define SV6621_BT_TX_CAPACITY               2560

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sv6621_bluetooth_s
{
  struct bt_driver_s driver;
  FAR struct sv6621_dev_s *dev;
  mutex_t command_lock;
  mutex_t state_lock;
  sem_t command_completion;
  uint16_t pending_opcode;
  uint16_t controller_revision;
  int command_result;
  bool command_pending;
  bool synchronization_initialized;
  bool subscribed;
  bool registered;
  bool initialized;
  bool opened;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct sv6621_bluetooth_s g_sv6621_bluetooth;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sv6621_bluetooth_open(FAR struct bt_driver_s *driver);
static int sv6621_bluetooth_send(FAR struct bt_driver_s *driver,
                                 enum bt_buf_type_e type, FAR void *data,
                                 size_t length);
static void sv6621_bluetooth_close(FAR struct bt_driver_s *driver);
static void sv6621_bluetooth_receive(uint8_t channel,
                                     FAR const uint8_t encoded[4],
                                     FAR const uint8_t *payload, size_t length,
                                     FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t sv6621_bluetooth_get_le16(FAR const uint8_t *value)
{
  return (uint16_t)value[0] | (uint16_t)value[1] << 8;
}

static int sv6621_bluetooth_h4_length(FAR const uint8_t *frame,
                                      size_t available)
{
  size_t header;
  size_t payload;

  if (frame == NULL || available == 0)
    {
      return -EINVAL;
    }

  switch (frame[0])
    {
      case SV6621_BT_H4_EVENT:
        header = 2;
        if (available < 1 + header)
          {
            return -EMSGSIZE;
          }

        payload = frame[2];
        break;

      case SV6621_BT_H4_ACL:
        header = 4;
        if (available < 1 + header)
          {
            return -EMSGSIZE;
          }

        payload = sv6621_bluetooth_get_le16(frame + 3);
        break;

      case SV6621_BT_H4_SCO:
        header = 3;
        if (available < 1 + header)
          {
            return -EMSGSIZE;
          }

        payload = frame[3];
        break;

      case SV6621_BT_H4_ISO:
        header = 4;
        if (available < 1 + header)
          {
            return -EMSGSIZE;
          }

        payload = sv6621_bluetooth_get_le16(frame + 3) & 0x3fff;
        break;

      default:
        return -EPROTO;
    }

  if (1 + header + payload > available ||
      1 + header + payload > SV6621_BT_MAX_FRAME)
    {
      return -EMSGSIZE;
    }

  return (int)(1 + header + payload);
}

static int sv6621_bluetooth_transmit(FAR struct sv6621_bluetooth_s *bluetooth,
                                     uint8_t channel, FAR const uint8_t *frame,
                                     size_t length)
{
  uint8_t packet[SV6621_BT_TX_CAPACITY];
  size_t packet_length;
  int ret;

  ret = sv6621_packet_build(channel, frame, length, packet, sizeof(packet),
                            &packet_length);
  if (ret < 0)
    {
      return ret;
    }

  return sv6621_tx_send(&bluetooth->dev->tx, packet, packet_length);
}

static int
sv6621_bluetooth_send_command(FAR struct sv6621_bluetooth_s *bluetooth,
                              uint16_t opcode, FAR const uint8_t *parameters,
                              size_t parameter_length)
{
  uint8_t frame[4 + UINT8_MAX];
  int ret;

  if (parameter_length > UINT8_MAX ||
      (parameter_length > 0 && parameters == NULL))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&bluetooth->command_lock);
  if (ret < 0)
    {
      return ret;
    }

  frame[0] = SV6621_BT_H4_COMMAND;
  frame[1] = opcode & 0xff;
  frame[2] = opcode >> 8;
  frame[3] = (uint8_t)parameter_length;
  if (parameter_length > 0)
    {
      memcpy(frame + 4, parameters, parameter_length);
    }

  nxsem_reset(&bluetooth->command_completion, 0);
  ret = nxmutex_lock(&bluetooth->state_lock);
  if (ret < 0)
    {
      nxmutex_unlock(&bluetooth->command_lock);
      return ret;
    }

  bluetooth->pending_opcode = opcode;
  bluetooth->command_result = -EINPROGRESS;
  bluetooth->command_pending = true;
  nxmutex_unlock(&bluetooth->state_lock);

  ret = sv6621_bluetooth_transmit(bluetooth, SV6621_CHANNEL_BT_COMMAND, frame,
                                  parameter_length + 4);
  if (ret >= 0)
    {
      ret = nxsem_tickwait(&bluetooth->command_completion,
                           MSEC2TICK(SV6621_BT_COMMAND_TIMEOUT_MS));
    }

  if (nxmutex_lock(&bluetooth->state_lock) >= 0)
    {
      if (ret >= 0)
        {
          ret = bluetooth->command_result;
        }

      bluetooth->command_pending = false;
      nxmutex_unlock(&bluetooth->state_lock);
    }

  nxmutex_unlock(&bluetooth->command_lock);
  return ret;
}

static int
sv6621_bluetooth_download_nv(FAR struct sv6621_bluetooth_s *bluetooth,
                             FAR const struct sv6621_firmware_s *nvram)
{
  uint8_t parameters[2 + SV6621_BT_NV_BLOCK_SIZE];
  size_t input;
  size_t output;
  uint8_t page;
  int ret;

  if (nvram == NULL || nvram->data == NULL ||
      nvram->length <= SV6621_BT_NV_HEADER_SIZE ||
      memcmp(nvram->data, "NVDS", SV6621_BT_NV_HEADER_SIZE) != 0)
    {
      return -EINVAL;
    }

  input = SV6621_BT_NV_HEADER_SIZE;
  output = 0;
  page = 0;
  while (input < nvram->length)
    {
      size_t record_length;

      if (nvram->length - input < SV6621_BT_NV_RECORD_HEADER)
        {
          return -EPROTO;
        }

      record_length =
          (size_t)nvram->data[input + 2] + SV6621_BT_NV_RECORD_HEADER;
      if (record_length > nvram->length - input ||
          record_length > SV6621_BT_NV_BLOCK_SIZE)
        {
          return -EPROTO;
        }

      if (output > 0 && output + record_length > SV6621_BT_NV_BLOCK_SIZE)
        {
          parameters[0] = page++;
          parameters[1] = (uint8_t)output;
          ret = sv6621_bluetooth_send_command(
              bluetooth, SV6621_BT_OPCODE_DOWNLOAD_NV, parameters, output + 2);
          if (ret < 0)
            {
              return ret;
            }

          output = 0;
        }

      memcpy(parameters + 2 + output, nvram->data + input, record_length);
      if (nvram->data[input] == SV6621_BT_NV_LOG_TAG &&
          record_length > SV6621_BT_NV_RECORD_HEADER)
        {
          parameters[2 + output + SV6621_BT_NV_RECORD_HEADER] = 1;
        }

      input += record_length;
      output += record_length;
    }

  if (output == 0)
    {
      return -EPROTO;
    }

  parameters[0] = page;
  parameters[1] = (uint8_t)output;
  return sv6621_bluetooth_send_command(bluetooth, SV6621_BT_OPCODE_DOWNLOAD_NV,
                                       parameters, output + 2);
}

static bool
sv6621_bluetooth_complete_command(FAR struct sv6621_bluetooth_s *bluetooth,
                                  FAR const uint8_t *frame, size_t length)
{
  uint16_t opcode;
  int result;

  if (length < 7 || frame[0] != SV6621_BT_H4_EVENT ||
      frame[1] != SV6621_BT_EVENT_COMMAND_COMPLETE || frame[2] < 4)
    {
      return false;
    }

  opcode = sv6621_bluetooth_get_le16(frame + 4);
  if (nxmutex_lock(&bluetooth->state_lock) < 0)
    {
      return false;
    }

  if (!bluetooth->command_pending || bluetooth->pending_opcode != opcode)
    {
      nxmutex_unlock(&bluetooth->state_lock);
      return false;
    }

  result = frame[6] == 0 ? 0 : -EIO;
  if (result == 0 && opcode == SV6621_BT_OPCODE_READ_LOCAL_VERSION &&
      length >= 10)
    {
      bluetooth->controller_revision = sv6621_bluetooth_get_le16(frame + 8);
    }

  bluetooth->command_result = result;
  bluetooth->command_pending = false;
  nxmutex_unlock(&bluetooth->state_lock);
  nxsem_post(&bluetooth->command_completion);
  return true;
}

static enum bt_buf_type_e sv6621_bluetooth_buffer_type(uint8_t h4_type)
{
  if (h4_type == SV6621_BT_H4_ACL)
    {
      return BT_ACL_IN;
    }

  if (h4_type == SV6621_BT_H4_ISO)
    {
      return BT_ISO_IN;
    }

  if (h4_type == SV6621_BT_H4_SCO)
    {
      return BT_SCO_IN;
    }

  return BT_EVT;
}

static void sv6621_bluetooth_receive(uint8_t channel,
                                     FAR const uint8_t encoded[4],
                                     FAR const uint8_t *payload, size_t length,
                                     FAR void *arg)
{
  FAR struct sv6621_bluetooth_s *bluetooth = arg;
  FAR const uint8_t *frame;
  size_t remaining;

  (void)encoded;

  if (bluetooth == NULL || payload == NULL ||
      length <= SV6621_BT_LINK_HEADER_SIZE)
    {
      return;
    }

  frame = payload + SV6621_BT_LINK_HEADER_SIZE;
  remaining = length - SV6621_BT_LINK_HEADER_SIZE;
  while (remaining > 0)
    {
      enum bt_buf_type_e type;
      int frame_length = sv6621_bluetooth_h4_length(frame, remaining);

      if (frame_length < 0)
        {
          wlerr("ERROR: malformed SV6621 Bluetooth packet on channel %u\n",
                channel);
          return;
        }

      if (!sv6621_bluetooth_complete_command(bluetooth, frame,
                                             (size_t)frame_length))
        {
          type = sv6621_bluetooth_buffer_type(frame[0]);
          if (bluetooth->registered && bluetooth->opened &&
              bluetooth->driver.receive != NULL)
            {
              (void)bluetooth->driver.receive(&bluetooth->driver, type,
                                              (FAR uint8_t *)frame + 1,
                                              (size_t)frame_length - 1);
            }
        }

      frame += frame_length;
      remaining -= frame_length;
    }
}

static int sv6621_bluetooth_open(FAR struct bt_driver_s *driver)
{
  FAR struct sv6621_bluetooth_s *bluetooth =
      (FAR struct sv6621_bluetooth_s *)driver;

  if (bluetooth == NULL || !bluetooth->initialized)
    {
      return -ENODEV;
    }

  bluetooth->opened = true;
  return 0;
}

static void sv6621_bluetooth_close(FAR struct bt_driver_s *driver)
{
  FAR struct sv6621_bluetooth_s *bluetooth =
      (FAR struct sv6621_bluetooth_s *)driver;

  if (bluetooth != NULL)
    {
      bluetooth->opened = false;
    }
}

static int sv6621_bluetooth_send(FAR struct bt_driver_s *driver,
                                 enum bt_buf_type_e type, FAR void *data,
                                 size_t length)
{
  FAR struct sv6621_bluetooth_s *bluetooth =
      (FAR struct sv6621_bluetooth_s *)driver;
  uint8_t frame[1 + SV6621_BT_MAX_FRAME];
  uint8_t channel;
  int ret;

  if (bluetooth == NULL || data == NULL || length == 0 ||
      length > SV6621_BT_MAX_FRAME)
    {
      return -EINVAL;
    }

  if (!bluetooth->initialized)
    {
      return -ENODEV;
    }

  switch (type)
    {
      case BT_CMD:
        frame[0] = SV6621_BT_H4_COMMAND;
        channel = SV6621_CHANNEL_BT_COMMAND;
        break;

      case BT_ACL_OUT:
        frame[0] = SV6621_BT_H4_ACL;
        channel = SV6621_CHANNEL_BT_DATA;
        break;

      case BT_ISO_OUT:
        frame[0] = SV6621_BT_H4_ISO;
        channel = SV6621_CHANNEL_BT_ISO;
        break;

      case BT_SCO_OUT:
        frame[0] = SV6621_BT_H4_SCO;
        channel = SV6621_CHANNEL_BT_AUDIO;
        break;

      default:
        return -ENOTSUP;
    }

  memcpy(frame + 1, data, length);
  ret = sv6621_bluetooth_transmit(bluetooth, channel, frame, length + 1);
  if (ret < 0)
    {
      return ret;
    }

  return (int)length;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_bluetooth_attach(FAR struct sv6621_dev_s *dev)
{
  FAR struct sv6621_bluetooth_s *bluetooth = &g_sv6621_bluetooth;
  static const uint8_t channels[] = {
    SV6621_CHANNEL_BT_COMMAND,
    SV6621_CHANNEL_BT_AUDIO,
    SV6621_CHANNEL_BT_ISO,
    SV6621_CHANNEL_BT_DATA,
  };
  size_t index;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  if (bluetooth->dev != NULL)
    {
      return bluetooth->dev == dev ? 0 : -EBUSY;
    }

  bluetooth->dev = dev;
  if (!bluetooth->synchronization_initialized)
    {
      bluetooth->driver.head_reserve = 0;
      bluetooth->driver.open = sv6621_bluetooth_open;
      bluetooth->driver.send = sv6621_bluetooth_send;
      bluetooth->driver.close = sv6621_bluetooth_close;

      ret = nxmutex_init(&bluetooth->command_lock);
      if (ret < 0)
        {
          bluetooth->dev = NULL;
          return ret;
        }

      ret = nxmutex_init(&bluetooth->state_lock);
      if (ret < 0)
        {
          nxmutex_destroy(&bluetooth->command_lock);
          bluetooth->dev = NULL;
          return ret;
        }

      ret = nxsem_init(&bluetooth->command_completion, 0, 0);
      if (ret < 0)
        {
          nxmutex_destroy(&bluetooth->state_lock);
          nxmutex_destroy(&bluetooth->command_lock);
          bluetooth->dev = NULL;
          return ret;
        }

      bluetooth->synchronization_initialized = true;
    }

  for (index = 0; index < sizeof(channels); index++)
    {
      ret = sv6621_packet_subscribe(&dev->router, channels[index],
                                    sv6621_bluetooth_receive, bluetooth);
      if (ret < 0)
        {
          while (index > 0)
            {
              index--;
              sv6621_packet_unsubscribe(&dev->router, channels[index],
                                        sv6621_bluetooth_receive, bluetooth);
            }

          bluetooth->dev = NULL;
          return ret;
        }
    }

  bluetooth->subscribed = true;
  return 0;
}

void sv6621_bluetooth_detach(FAR struct sv6621_dev_s *dev)
{
  FAR struct sv6621_bluetooth_s *bluetooth = &g_sv6621_bluetooth;
  static const uint8_t channels[] = {
    SV6621_CHANNEL_BT_COMMAND,
    SV6621_CHANNEL_BT_AUDIO,
    SV6621_CHANNEL_BT_ISO,
    SV6621_CHANNEL_BT_DATA,
  };
  size_t index;

  if (dev == NULL || bluetooth->dev != dev)
    {
      return;
    }

  if (bluetooth->subscribed)
    {
      for (index = 0; index < sizeof(channels); index++)
        {
          sv6621_packet_unsubscribe(&bluetooth->dev->router, channels[index],
                                    sv6621_bluetooth_receive, bluetooth);
        }
    }

  bluetooth->subscribed = false;
  bluetooth->initialized = false;
  bluetooth->opened = false;
  bluetooth->command_pending = false;
  bluetooth->dev = NULL;
}

int sv6621_bluetooth_start(FAR struct sv6621_dev_s *dev,
                           FAR const struct sv6621_firmware_s *nvram,
                           int device_id)
{
  FAR struct sv6621_bluetooth_s *bluetooth = &g_sv6621_bluetooth;
  int ret;

  if (dev == NULL || bluetooth->dev != dev || device_id < 0)
    {
      return -EINVAL;
    }

  if (bluetooth->initialized)
    {
      return 0;
    }

  ret = sv6621_service_start_bluetooth(&bluetooth->dev->service,
                                       bluetooth->dev->config.transport,
                                       SV6621_BT_READY_TIMEOUT_MS);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_bluetooth_send_command(
      bluetooth, SV6621_BT_OPCODE_READ_LOCAL_VERSION, NULL, 0);
  if (ret < 0 ||
      bluetooth->controller_revision != SV6621_BT_CONTROLLER_6160_LITE)
    {
      return ret < 0 ? ret : -ENODEV;
    }

  ret = sv6621_bluetooth_download_nv(bluetooth, nvram);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_bluetooth_send_command(bluetooth, SV6621_BT_OPCODE_RESET, NULL,
                                      0);
  if (ret < 0)
    {
      return ret;
    }

  if (!bluetooth->registered)
    {
      ret = bt_driver_register_with_id(&bluetooth->driver, device_id);
      if (ret < 0)
        {
          return ret;
        }

      bluetooth->registered = true;
    }

  bluetooth->initialized = true;
  wlinfo("SV6621 Bluetooth controller ready: revision=0x%04x\n",
         bluetooth->controller_revision);
  return 0;
}

int sv6621_bluetooth_stop(FAR struct sv6621_dev_s *dev)
{
  FAR struct sv6621_bluetooth_s *bluetooth = &g_sv6621_bluetooth;
  int ret;

  if (dev == NULL || bluetooth->dev != dev)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&bluetooth->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!bluetooth->initialized)
    {
      nxmutex_unlock(&bluetooth->state_lock);
      return 0;
    }

  if (bluetooth->opened || bluetooth->command_pending)
    {
      nxmutex_unlock(&bluetooth->state_lock);
      return -EBUSY;
    }

  ret = sv6621_service_stop_bluetooth(&bluetooth->dev->service,
                                      bluetooth->dev->config.transport);
  if (ret < 0)
    {
      nxmutex_unlock(&bluetooth->state_lock);
      return ret;
    }

  nxsig_usleep(SV6621_BT_STOP_GRACE_MS * 1000);
  bluetooth->initialized = false;
  bluetooth->controller_revision = 0;
  nxmutex_unlock(&bluetooth->state_lock);
  return 0;
}

bool sv6621_bluetooth_is_started(FAR struct sv6621_dev_s *dev)
{
  FAR struct sv6621_bluetooth_s *bluetooth = &g_sv6621_bluetooth;
  bool started = false;

  if (dev != NULL && bluetooth->dev == dev &&
      nxmutex_lock(&bluetooth->state_lock) >= 0)
    {
      started = bluetooth->initialized;
      nxmutex_unlock(&bluetooth->state_lock);
    }

  return started;
}

void sv6621_bluetooth_offline(FAR struct sv6621_dev_s *dev, int error)
{
  FAR struct sv6621_bluetooth_s *bluetooth = &g_sv6621_bluetooth;
  static uint8_t hardware_error[] = { 0x10, 0x01, 0x00 };
  bool notify = false;

  if (dev == NULL || bluetooth->dev != dev)
    {
      return;
    }

  if (nxmutex_lock(&bluetooth->state_lock) >= 0)
    {
      notify = bluetooth->initialized && bluetooth->opened &&
               bluetooth->driver.receive != NULL;
      bluetooth->initialized = false;
      bluetooth->command_result = error < 0 ? error : -EIO;
      if (bluetooth->command_pending)
        {
          bluetooth->command_pending = false;
          nxsem_post(&bluetooth->command_completion);
        }

      nxmutex_unlock(&bluetooth->state_lock);
    }

  if (notify)
    {
      (void)bluetooth->driver.receive(&bluetooth->driver, BT_EVT,
                                      hardware_error, sizeof(hardware_error));
    }
}

#endif /* CONFIG_SV6621_BLUETOOTH */
