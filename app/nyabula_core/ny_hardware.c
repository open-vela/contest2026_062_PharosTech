/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_hardware.c
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

#include <errno.h>
#include <math.h>
#include <netutils/cJSON.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ny_broker.h"
#include "ny_manifest.h"
#ifdef CONFIG_NYABULA_CORE_PRODUCT
#include "ny_product.h"
#ifdef CONFIG_NYABULA_CORE_NAS
#include "ny_nas.h"
#endif
#endif
#if defined(CONFIG_NYABULA_CORE_VOICE) && defined(CONFIG_NYABULA_CORE_AUDIO)
#include "ny_voice.h"
#include <mbedtls/base64.h>
#endif

#define NY_HARDWARE_REQUEST_LIMIT   8192
#define NY_HARDWARE_CAPTURE_SAMPLES 1600

struct ny_hardware_route_s
{
  const char *topic;
  uint64_t permissions;
};

static const struct ny_hardware_route_s g_hardware_routes[] = {
  { "audio.status", NY_PERMISSION_AUDIO_READ },
  { "audio.volume", NY_PERMISSION_AUDIO_CONTROL },
  { "audio.channel", NY_PERMISSION_AUDIO_CONTROL },
  { "audio.output.route", NY_PERMISSION_AUDIO_CONTROL },
  { "audio.input.route",
    NY_PERMISSION_AUDIO_CONTROL | NY_PERMISSION_AUDIO_CAPTURE },
  { "audio.mic.gain",
    NY_PERMISSION_AUDIO_CONTROL | NY_PERMISSION_AUDIO_CAPTURE },
  { "audio.mic.mute",
    NY_PERMISSION_AUDIO_CONTROL | NY_PERMISSION_AUDIO_CAPTURE },
  { "audio.capture", NY_PERMISSION_AUDIO_CAPTURE },
  { "music.library", NY_PERMISSION_AUDIO_READ },
  { "music.status", NY_PERMISSION_AUDIO_READ },
  { "music.play", NY_PERMISSION_AUDIO_PLAY },
  { "music.pause", NY_PERMISSION_AUDIO_PLAY },
  { "music.resume", NY_PERMISSION_AUDIO_PLAY },
  { "music.stop", NY_PERMISSION_AUDIO_PLAY },
  { "dsp.status", NY_PERMISSION_AUDIO_READ },
  { "dsp.config", NY_PERMISSION_AUDIO_CONTROL | NY_PERMISSION_USB_ACCESS },
  { "dsp.play", NY_PERMISSION_AUDIO_PLAY | NY_PERMISSION_USB_ACCESS },
  { "dsp.stop", NY_PERMISSION_AUDIO_PLAY | NY_PERMISSION_USB_ACCESS },
  { "bt.status", NY_PERMISSION_BLUETOOTH_READ },
  { "bt.devices", NY_PERMISSION_BLUETOOTH_READ },
  { "bt.discoverable", NY_PERMISSION_BLUETOOTH_CONNECT },
  { "bt.pair.confirm", NY_PERMISSION_BLUETOOTH_CONNECT },
  { "bt.connect", NY_PERMISSION_BLUETOOTH_CONNECT },
  { "bt.disconnect", NY_PERMISSION_BLUETOOTH_CONNECT },
  { "bt.forget", NY_PERMISSION_BLUETOOTH_CONNECT },
  { "bt.media.play",
    NY_PERMISSION_BLUETOOTH_CONNECT | NY_PERMISSION_AUDIO_PLAY },
  { "bt.media.pause",
    NY_PERMISSION_BLUETOOTH_CONNECT | NY_PERMISSION_AUDIO_PLAY },
  { "bt.media.stop",
    NY_PERMISSION_BLUETOOTH_CONNECT | NY_PERMISSION_AUDIO_PLAY },
  { "bt.media.next",
    NY_PERMISSION_BLUETOOTH_CONNECT | NY_PERMISSION_AUDIO_PLAY },
  { "bt.media.prev",
    NY_PERMISSION_BLUETOOTH_CONNECT | NY_PERMISSION_AUDIO_PLAY },
  { "bt.call.answer",
    NY_PERMISSION_BLUETOOTH_CALL | NY_PERMISSION_AUDIO_CAPTURE },
  { "bt.call.reject", NY_PERMISSION_BLUETOOTH_CALL },
  { "bt.call.hangup", NY_PERMISSION_BLUETOOTH_CALL },
  { "usb.devices", NY_PERMISSION_USB_ACCESS },
  { "camera.capture", NY_PERMISSION_CAMERA_CAPTURE },
  { "weather.status", NY_PERMISSION_WEATHER_READ },
  { "weather.get", NY_PERMISSION_WEATHER_READ },
  { "weather.query", NY_PERMISSION_WEATHER_READ },
  { "sys.info", NY_PERMISSION_DEVICE_READ },
  { "device.status", NY_PERMISSION_DEVICE_READ },
  { "nas.status", NY_PERMISSION_NAS_READ },
  { "nas.devices", NY_PERMISSION_USB_ACCESS },
  { "nas.list", NY_PERMISSION_NAS_READ },
  { "nas.configure", NY_PERMISSION_NAS_MANAGE },
  { "nas.mkdir", NY_PERMISSION_NAS_WRITE }
};

#ifdef CONFIG_NYABULA_CORE_PRODUCT
static int ny_hardware_usb(const struct ny_product_caller_s *caller,
                           const cJSON *data, cJSON **result);
static int ny_hardware_capture(const struct ny_product_caller_s *caller,
                               const cJSON *data, cJSON **result);

/****************************************************************************
 * Name: ny_hardware_usb
 ****************************************************************************/

static int ny_hardware_usb(const struct ny_product_caller_s *caller,
                           const cJSON *data, cJSON **result)
{
  cJSON *status = NULL;
  cJSON *devices;
  cJSON *item;
  int index;
  int ret = ny_product_device_request(caller, "device.status", data, &status);
  if (ret < 0)
    {
      cJSON_Delete(status);
      return ret;
    }

  devices = cJSON_DetachItemFromObjectCaseSensitive(status, "audioDevices");
  cJSON_Delete(status);
  if (!cJSON_IsArray(devices))
    {
      cJSON_Delete(devices);
      return -ENODEV;
    }

  for (index = cJSON_GetArraySize(devices) - 1; index >= 0; index--)
    {
      item = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetArrayItem(devices, index), "path");
      if (!cJSON_IsString(item) ||
          strncmp(item->valuestring, "/dev/audio/usb", 14) != 0)
        {
          cJSON_DeleteItemFromArray(devices, index);
        }
    }

#ifdef CONFIG_NYABULA_CORE_NAS
  cJSON *storage = NULL;
  if (ny_nas_request(caller, "nas.devices", data, &storage) == 0)
    {
      cJSON *volumes = cJSON_GetObjectItemCaseSensitive(storage, "devices");
      const cJSON *volume;
      cJSON_ArrayForEach(volume, volumes)
      {
        cJSON *copy = cJSON_Duplicate(volume, true);
        if (copy)
          {
            cJSON_AddStringToObject(copy, "kind", "storage");
            cJSON_AddItemToArray(devices, copy);
          }
      }
    }
  cJSON_Delete(storage);
#endif

  *result = cJSON_CreateObject();
  if (*result == NULL || !cJSON_AddItemToObject(*result, "devices", devices))
    {
      cJSON_Delete(devices);
      return -ENOMEM;
    }

#ifdef CONFIG_NYABULA_CORE_NAS
  return cJSON_AddStringToObject(*result, "scope", "usb") ? 0 : -ENOMEM;
#else
  return cJSON_AddStringToObject(*result, "scope", "usb-audio") ? 0 : -ENOMEM;
#endif
}

/****************************************************************************
 * Name: ny_hardware_capture
 ****************************************************************************/

static int ny_hardware_capture(const struct ny_product_caller_s *caller,
                               const cJSON *data, cJSON **result)
{
#if defined(CONFIG_NYABULA_CORE_VOICE) && defined(CONFIG_NYABULA_CORE_AUDIO)
  cJSON *status = NULL;
  const cJSON *requested = cJSON_GetObjectItemCaseSensitive(data, "samples");
  const cJSON *input;
  const cJSON *muted;
  int16_t *samples;
  unsigned char *encoded;
  size_t encoded_size;
  size_t encoded_length;
  size_t count = NY_HARDWARE_CAPTURE_SAMPLES;
  int ret;

  if (requested != NULL)
    {
      if (!cJSON_IsNumber(requested) || !isfinite(requested->valuedouble) ||
          requested->valuedouble < 1 || requested->valuedouble > count ||
          floor(requested->valuedouble) != requested->valuedouble)
        {
          return -EINVAL;
        }

      count = requested->valueint;
    }

  ret = ny_product_audio_request(caller, "audio.status", data, &status);
  input = cJSON_GetObjectItemCaseSensitive(status, "input");
  muted = cJSON_GetObjectItemCaseSensitive(input, "muted");
  if (ret < 0 || !cJSON_IsFalse(muted))
    {
      cJSON_Delete(status);
      return ret < 0 ? ret : -EACCES;
    }

  cJSON_Delete(status);
  encoded_size = 4 * ((count * 2 + 2) / 3) + 1;
  samples = malloc(count * sizeof(*samples));
  encoded = malloc(encoded_size);
  if (samples == NULL || encoded == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = ny_voice_capture_snapshot(samples, count);
  if (ret <= 0)
    {
      ret = ret < 0 ? ret : -EAGAIN;
      goto out;
    }

  count = ret;
  /* Encode explicit little-endian PCM, independent of the host byte order. */
  for (size_t i = 0; i < count; i++)
    {
      uint16_t value = (uint16_t)samples[i];
      ((unsigned char *)samples)[2 * i] = value & 0xff;
      ((unsigned char *)samples)[2 * i + 1] = value >> 8;
    }

  ret = mbedtls_base64_encode(encoded, encoded_size, &encoded_length,
                              (const unsigned char *)samples, count * 2);
  if (ret != 0)
    {
      ret = -EIO;
      goto out;
    }

  *result = cJSON_CreateObject();
  ret = *result != NULL &&
                cJSON_AddStringToObject(*result, "format", "s16le") &&
                cJSON_AddNumberToObject(*result, "sampleRate", 16000) &&
                cJSON_AddNumberToObject(*result, "channels", 1) &&
                cJSON_AddNumberToObject(*result, "samples", count) &&
                cJSON_AddStringToObject(*result, "data", (const char *)encoded)
            ? 0
            : -ENOMEM;
out:
  if (samples != NULL)
    {
      memset(samples, 0, count * sizeof(*samples));
    }

  free(samples);
  free(encoded);
  return ret;
#else
  return -ENOTSUP;
#endif
}
#endif

/****************************************************************************
 * Name: ny_broker_hardware_request
 *
 * Description:
 *   Only named capabilities cross this boundary; a plugin cannot supply a
 *   product role or invoke an arbitrary owner topic. Check grants before any
 *   hardware service, including builds where that service is unavailable.
 ****************************************************************************/

int ny_broker_hardware_request(const struct ny_broker_client_s *client,
                               const char *topic, const char *json,
                               size_t length, char **response)
{
  uint64_t required = 0;
  size_t index;
  if (response == NULL || topic == NULL || json == NULL)
    {
      return -EINVAL;
    }

  *response = NULL;
  for (index = 0;
       index < sizeof(g_hardware_routes) / sizeof(g_hardware_routes[0]);
       index++)
    {
      if (strcmp(topic, g_hardware_routes[index].topic) == 0)
        {
          required = g_hardware_routes[index].permissions;
          break;
        }
    }

  if (required == 0 || client == NULL || client->id == NULL ||
      (client->permissions & required) != required)
    {
      return -EACCES;
    }

  if (length == 0 || length > NY_HARDWARE_REQUEST_LIMIT ||
      strlen(json) != length)
    {
      return -EINVAL;
    }

#ifndef CONFIG_NYABULA_CORE_PRODUCT
  return -ENOTSUP;
#else
  struct ny_product_caller_s caller = { client->id, NY_PRODUCT_OWNER, true };
  cJSON *data = cJSON_ParseWithOpts(json, NULL, true);
  cJSON *result = NULL;
  int ret;
  if (!cJSON_IsObject(data))
    {
      cJSON_Delete(data);
      return -EINVAL;
    }

  if (strcmp(topic, "usb.devices") == 0)
    {
      ret = ny_hardware_usb(&caller, data, &result);
    }
  else if (strcmp(topic, "audio.capture") == 0)
    {
      ret = ny_hardware_capture(&caller, data, &result);
    }
  else if (strcmp(topic, "camera.capture") == 0)
    {
      /* Camera capture belongs to openvela; no capture service is registered.
       */
      ret = -ENOTSUP;
    }
  else
    {
      ret = ny_product_request(&caller, topic, data, &result);
    }

  if (ret == 0 && strncmp(topic, "bt.", 3) == 0)
    {
      if ((client->permissions & NY_PERMISSION_BLUETOOTH_CALL) == 0)
        {
          cJSON_DeleteItemFromObjectCaseSensitive(result, "hfp");
        }

      if ((client->permissions & NY_PERMISSION_BLUETOOTH_CONNECT) == 0)
        {
          cJSON_DeleteItemFromObjectCaseSensitive(result, "pairing");
        }
    }

  if (ret == 0)
    {
      *response = cJSON_PrintUnformatted(result);
      if (*response == NULL)
        {
          ret = -ENOMEM;
        }
    }

  cJSON_Delete(data);
  cJSON_Delete(result);
  return ret == -ENOSYS ? -ENOTSUP : ret;
#endif
}
