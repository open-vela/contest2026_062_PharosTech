/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_dsp.c
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

#include "ny_product_dsp.h"
#include "ny_audio_stream.h"
#include "ny_dsp_output.h"
#include "ny_product_store.h"
#include "nyadsp.h"
#ifdef CONFIG_NYABULA_CORE_BT
#include "ny_product_bt.h"
#endif
#ifdef CONFIG_NYABULA_CORE_VOICE
#include "ny_voice.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NY_DSP_FRAMES          256
#define NY_DSP_STACK           32768
#define NY_DSP_DOMAIN          "dsp"
#define NY_DSP_PRESETS_DOMAIN  "dsp-presets"
#define NY_DSP_PRESETS_MAX     8
#define NY_DSP_PRESET_NAME_MAX 48
#define NY_DSP_TRACK_MAX       128
#define NY_DSP_WAV_HEADER_SIZE 44

/* Request serialization must never be acquired by the audio producer. */
static mutex_t g_dsp_request_lock = NXMUTEX_INITIALIZER;
static mutex_t g_dsp_lock = NXMUTEX_INITIALIZER;
static struct nyadsp_s *g_dsp_prepared;
static struct ny_product_dsp_settings_s g_dsp_settings;
static bool g_dsp_loaded;
static struct ny_product_dsp_settings_s g_dsp_saved_settings;
static bool g_dsp_settings_saved = true;
static const char *const g_dsp_filter_names[] = { "off", "peak", "lowShelf",
                                                  "highShelf" };
static const unsigned int g_dsp_legacy_bands[NYADSP_EQ_BANDS] = { 2, 5, 8,
                                                                  12 };
static bool g_dsp_output_owned;
static char g_dsp_source[24];
static bool g_dsp_running;
static bool g_dsp_requested;
static bool g_dsp_stopping;
static bool g_dsp_joinable;
static bool g_dsp_shutdown;
static pthread_t g_dsp_thread;
static uint32_t g_dsp_revision;
static uint64_t g_dsp_frames;
static int g_dsp_error;
static float g_dsp_peak;
static double g_dsp_clock_ppm;
static char g_dsp_track[NY_DSP_TRACK_MAX];
static uint32_t g_dsp_usb_starved;
static size_t g_dsp_satellite_queued;
static size_t g_dsp_bass_queued;

static int ny_dsp_number(const cJSON *data, const char *name, float lower,
                         float upper, float *value);
static int ny_dsp_parse(const cJSON *data,
                        struct ny_product_dsp_settings_s *settings);
static cJSON *ny_dsp_config(const struct ny_product_dsp_settings_s *settings);
static int ny_dsp_load(void);
static cJSON *ny_dsp_status(void);
static void ny_dsp_reset_eq(struct ny_product_dsp_settings_s *settings);
static int ny_dsp_presets(const char *topic, const cJSON *data,
                          cJSON **result);
static uint32_t ny_dsp_le32(const unsigned char *p);
static int ny_dsp_open(const char *name, size_t *remaining);
static void *ny_dsp_worker(void *arg);

/****************************************************************************
 * Name: ny_dsp_number
 ****************************************************************************/

static int ny_dsp_number(const cJSON *data, const char *name, float lower,
                         float upper, float *value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, name);
  if (item == NULL)
    {
      return 0;
    }

  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
      item->valuedouble < lower || item->valuedouble > upper)
    {
      return -EINVAL;
    }

  *value = item->valuedouble;
  return 0;
}

/****************************************************************************
 * Name: ny_dsp_parse
 ****************************************************************************/

static int ny_dsp_parse(const cJSON *data,
                        struct ny_product_dsp_settings_s *settings)
{
  const cJSON *item;
  const char *arrays[] = { "eqDb", "outputGainDb", "delayMs", "invert" };
  int count[] = { NYADSP_EQ_BANDS, NYADSP_OUTPUTS, NYADSP_OUTPUTS,
                  NYADSP_OUTPUTS };
  int group;
  int index;
  float common = settings->filter.crossover_hz;

  if (!cJSON_IsObject(data) ||
      ny_dsp_number(data, "masterDb", -30, 0,
                    &settings->filter.master_gain_db) < 0 ||
      ny_dsp_number(data, "crossoverHz", 40, 500, &common) < 0)
    {
      return -EINVAL;
    }

  /* A legacy cutoff sets both branches. Explicit branch values win when
   * a new client includes the legacy field in a complete configuration.
   */

  if (cJSON_GetObjectItemCaseSensitive(data, "crossoverHz") != NULL)
    {
      settings->crossover.satellite_highpass_hz = common;
      settings->crossover.bass_lowpass_hz = common;
    }

  if (ny_dsp_number(data, "satelliteHighpassHz", 40, 500,
                    &settings->crossover.satellite_highpass_hz) < 0 ||
      ny_dsp_number(data, "bassLowpassHz", 40, 500,
                    &settings->crossover.bass_lowpass_hz) < 0)
    {
      return -EINVAL;
    }

  settings->filter.crossover_hz = settings->crossover.satellite_highpass_hz;

  item = cJSON_GetObjectItemCaseSensitive(data, "eqEnabled");
  if (item != NULL)
    {
      if (!cJSON_IsBool(item))
        {
          return -EINVAL;
        }
      settings->eq_enabled = cJSON_IsTrue(item);
    }

  item = cJSON_GetObjectItemCaseSensitive(data, "bassEnabled");
  if (item != NULL)
    {
      if (!cJSON_IsBool(item))
        {
          return -EINVAL;
        }

      settings->bass = cJSON_IsTrue(item);
    }

  item = cJSON_GetObjectItemCaseSensitive(data, "bypass");
  if (item != NULL)
    {
      if (!cJSON_IsBool(item))
        {
          return -EINVAL;
        }

      settings->filter.bypass = cJSON_IsTrue(item);
    }

  for (group = 0; group < 4; group++)
    {
      const cJSON *array =
          cJSON_GetObjectItemCaseSensitive(data, arrays[group]);
      if (array == NULL)
        {
          continue;
        }

      if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != count[group])
        {
          return -EINVAL;
        }

      for (index = 0; index < count[group]; index++)
        {
          double value;
          item = cJSON_GetArrayItem(array, index);
          if (group == 3)
            {
              if (!cJSON_IsBool(item))
                {
                  return -EINVAL;
                }

              settings->filter.output[index].polarity =
                  cJSON_IsTrue(item) ? -1 : 1;
              continue;
            }

          if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble))
            {
              return -EINVAL;
            }

          value = item->valuedouble;
          if ((group == 0 && (value < -12 || value > 12)) ||
              (group == 1 && (value < -24 || value > 6)) ||
              (group == 2 && (value < 0 || value > 100)))
            {
              return -EINVAL;
            }

          if (group == 0)
            {
              settings->input[g_dsp_legacy_bands[index]].gain_db = value;
            }
          else if (group == 1)
            {
              settings->filter.output[index].gain_db = value;
            }
          else
            {
              settings->filter.output[index].delay_frames =
                  (uint32_t)lround(value * 48.0);
            }
        }
    }

  for (group = 0; group < 2; group++)
    {
      int bands_count = group == 0 ? NYADSP_EQ_BANDS : NYADSP_INPUT_EQ_BANDS;
      item = cJSON_GetObjectItemCaseSensitive(data, group == 0 ? "eqBands"
                                                               : "eqBands16");
      if (item != NULL)
        {
          if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != bands_count)
            {
              return -EINVAL;
            }

          for (index = 0; index < bands_count; index++)
            {
              int target = group == 0 ? g_dsp_legacy_bands[index] : index;
              const cJSON *band = cJSON_GetArrayItem(item, index);
              const cJSON *enabled =
                  cJSON_GetObjectItemCaseSensitive(band, "enabled");
              const cJSON *type =
                  cJSON_GetObjectItemCaseSensitive(band, "type");
              struct nyadsp_eq_s *eq = &settings->input[target];
              int kind;
              if (!cJSON_IsObject(band) || !cJSON_IsBool(enabled) ||
                  !cJSON_IsString(type) ||
                  ny_dsp_number(band, "frequencyHz", 20, 18000,
                                &eq->frequency_hz) < 0 ||
                  ny_dsp_number(band, "q", 0.2, 10, &eq->q) < 0 ||
                  ny_dsp_number(band, "gainDb", -12, 12, &eq->gain_db) < 0)
                {
                  return -EINVAL;
                }

              for (kind = NYADSP_FILTER_PEAK; kind <= NYADSP_FILTER_HIGH_SHELF;
                   kind++)
                {
                  if (!strcmp(type->valuestring, g_dsp_filter_names[kind]))
                    break;
                }

              if (kind > NYADSP_FILTER_HIGH_SHELF)
                {
                  return -EINVAL;
                }

              settings->kinds[target] = kind;
              settings->enabled[target] = cJSON_IsTrue(enabled);
            }
        }
    }

  return 0;
}

/****************************************************************************
 * Name: ny_dsp_config
 ****************************************************************************/

static cJSON *ny_dsp_config(const struct ny_product_dsp_settings_s *settings)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *eq;
  cJSON *bands;
  cJSON *expanded;
  cJSON *gains;
  cJSON *delays;
  cJSON *invert;
  int i;
  int legacy = 0;

  if (root == NULL)
    {
      return NULL;
    }

  eq = cJSON_AddArrayToObject(root, "eqDb");
  bands = cJSON_AddArrayToObject(root, "eqBands");
  expanded = cJSON_AddArrayToObject(root, "eqBands16");
  gains = cJSON_AddArrayToObject(root, "outputGainDb");
  delays = cJSON_AddArrayToObject(root, "delayMs");
  invert = cJSON_AddArrayToObject(root, "invert");
  if (eq == NULL || bands == NULL || expanded == NULL || gains == NULL ||
      delays == NULL || invert == NULL ||
      !cJSON_AddBoolToObject(root, "bassEnabled", settings->bass) ||
      !cJSON_AddBoolToObject(root, "eqEnabled", settings->eq_enabled) ||
      !cJSON_AddBoolToObject(root, "bypass", settings->filter.bypass) ||
      !cJSON_AddNumberToObject(root, "masterDb",
                               settings->filter.master_gain_db) ||
      !cJSON_AddNumberToObject(root, "crossoverHz",
                               settings->filter.crossover_hz) ||
      !cJSON_AddNumberToObject(root, "satelliteHighpassHz",
                               settings->crossover.satellite_highpass_hz) ||
      !cJSON_AddNumberToObject(root, "bassLowpassHz",
                               settings->crossover.bass_lowpass_hz))
    {
      cJSON_Delete(root);
      return NULL;
    }

  for (i = 0; i < NYADSP_INPUT_EQ_BANDS; i++)
    {

      cJSON *band = cJSON_CreateObject();
      if (band == NULL ||
          !cJSON_AddNumberToObject(band, "frequencyHz",
                                   settings->input[i].frequency_hz) ||
          !cJSON_AddNumberToObject(band, "q", settings->input[i].q) ||
          !cJSON_AddNumberToObject(band, "gainDb",
                                   settings->input[i].gain_db) ||
          !cJSON_AddBoolToObject(band, "enabled", settings->enabled[i]) ||
          !cJSON_AddStringToObject(band, "type",
                                   g_dsp_filter_names[settings->kinds[i]]) ||
          !cJSON_AddItemToArray(expanded, band))
        {
          cJSON_Delete(band);
          cJSON_Delete(root);
          return NULL;
        }
      if (legacy < NYADSP_EQ_BANDS && i == g_dsp_legacy_bands[legacy])
        {
          if (!cJSON_AddItemToArray(bands, cJSON_Duplicate(band, true)) ||
              !cJSON_AddItemToArray(
                  eq, cJSON_CreateNumber(settings->input[i].gain_db)))
            {
              cJSON_Delete(root);
              return NULL;
            }
          legacy++;
        }
    }

  for (i = 0; i < NYADSP_OUTPUTS; i++)
    {
      if (!cJSON_AddItemToArray(
              gains, cJSON_CreateNumber(settings->filter.output[i].gain_db)) ||
          !cJSON_AddItemToArray(
              delays, cJSON_CreateNumber(
                          settings->filter.output[i].delay_frames / 48.0)) ||
          !cJSON_AddItemToArray(
              invert,
              cJSON_CreateBool(settings->filter.output[i].polarity == -1)))
        {
          cJSON_Delete(root);
          return NULL;
        }
    }

  return root;
}

/****************************************************************************
 * Name: ny_dsp_reset_eq
 ****************************************************************************/

static void ny_dsp_reset_eq(struct ny_product_dsp_settings_s *settings)
{
  static const float frequencies[] = { 31.5f, 50,   80,    125,  200,  300,
                                       500,   800,  1200,  2000, 3150, 4500,
                                       6000,  9000, 12500, 16000 };
  int i;
  settings->eq_enabled = true;
  for (i = 0; i < NYADSP_INPUT_EQ_BANDS; i++)
    {
      settings->input[i].frequency_hz = frequencies[i];
      settings->input[i].q = 1.4f;
      settings->input[i].gain_db = 0;
      settings->kinds[i] = NYADSP_FILTER_PEAK;
      settings->enabled[i] = true;
    }
}

/****************************************************************************
 * Name: ny_dsp_load
 ****************************************************************************/

static int ny_dsp_load(void)
{
  cJSON *saved = NULL;
  uint64_t revision;
  struct ny_product_dsp_settings_s settings;
  int ret;

  if (g_dsp_loaded)
    {
      return 0;
    }

  memset(&settings, 0, sizeof(settings));
  nyadsp_default_config(&settings.filter);
  settings.filter.master_gain_db = -12;
  settings.filter.crossover_hz = 120;
  settings.crossover.satellite_highpass_hz = 120;
  settings.crossover.bass_lowpass_hz = 120;
  ny_dsp_reset_eq(&settings);

  ret = ny_product_store_read(NY_DSP_DOMAIN, &saved, &revision);
  if (ret == 0 && saved != NULL)
    {
      ret = ny_dsp_parse(saved, &settings);
    }

  cJSON_Delete(saved);
  if (ret == 0)
    {
      g_dsp_settings = settings;
      g_dsp_saved_settings = settings;
      g_dsp_settings_saved = true;
      g_dsp_loaded = true;
      g_dsp_revision++;
    }

  return ret;
}

/****************************************************************************
 * Name: ny_dsp_status
 ****************************************************************************/

static cJSON *ny_dsp_status(void)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *config = ny_dsp_config(&g_dsp_settings);
  if (root == NULL || config == NULL ||
      !cJSON_AddItemToObject(root, "config", config))
    {
      cJSON_Delete(root);
      cJSON_Delete(config);
      return NULL;
    }

  cJSON_AddBoolToObject(root, "running", g_dsp_running || g_dsp_output_owned);
  cJSON_AddStringToObject(root, "source", g_dsp_source);
  cJSON_AddBoolToObject(root, "settingsSaved", g_dsp_settings_saved);
  cJSON_AddBoolToObject(root, "stopping", g_dsp_stopping);
  cJSON_AddStringToObject(root, "track", g_dsp_track);
  cJSON_AddNumberToObject(root, "sampleRate", NYADSP_SAMPLE_RATE);
  cJSON_AddNumberToObject(root, "processedFrames", g_dsp_frames);
  cJSON_AddNumberToObject(root, "outputPeak", g_dsp_peak);
  cJSON_AddNumberToObject(root, "usbClockPpm", g_dsp_clock_ppm);
  cJSON_AddNumberToObject(root, "usbUnderruns", g_dsp_usb_starved);
  cJSON_AddNumberToObject(root, "satelliteQueuedFrames",
                          g_dsp_satellite_queued);
  cJSON_AddNumberToObject(root, "bassQueuedFrames", g_dsp_bass_queued);
  cJSON_AddNumberToObject(
      root, "queueSkewMs",
      ((double)g_dsp_bass_queued - g_dsp_satellite_queued) * 1000 /
          NYADSP_SAMPLE_RATE);
  cJSON_AddNumberToObject(root, "lastError", g_dsp_error);
  cJSON_AddStringToObject(root, "satelliteDevice",
                          CONFIG_NYABULA_CORE_MEDIA_DEVICE);
  cJSON_AddStringToObject(root, "bassDevice",
                          CONFIG_NYABULA_CORE_DSP_BASS_DEVICE);
  cJSON_AddBoolToObject(root, "bassPresent",
                        access(CONFIG_NYABULA_CORE_DSP_BASS_DEVICE, F_OK) ==
                            0);
  cJSON_AddNumberToObject(root, "revision", g_dsp_revision);
  return root;
}

/****************************************************************************
 * Name: ny_dsp_le32 / ny_dsp_open
 ****************************************************************************/

static uint32_t ny_dsp_le32(const unsigned char *p)
{
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static int ny_dsp_open(const char *name, size_t *remaining)
{
  unsigned char header[NY_DSP_WAV_HEADER_SIZE];
  struct stat info;
  char path[PATH_MAX];
  int fd;

  if (name == NULL || !*name || strlen(name) >= NY_DSP_TRACK_MAX ||
      strchr(name, '/') || strchr(name, '\\') || name[0] == '.')
    {
      return -EINVAL;
    }

  if (snprintf(path, sizeof(path), "%s/%s", CONFIG_NYABULA_CORE_MEDIA_ROOT,
               name) >= sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      return -errno;
    }

  /* Match the existing media catalog's canonical PCM WAV contract. */

  if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) ||
      read(fd, header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVEfmt ", 8) ||
      ny_dsp_le32(header + 16) != 16 || header[20] != 1 || header[21] != 0 ||
      header[22] != 2 || header[23] != 0 ||
      ny_dsp_le32(header + 24) != NYADSP_SAMPLE_RATE || header[32] != 4 ||
      header[33] != 0 || header[34] != 16 || header[35] != 0 ||
      memcmp(header + 36, "data", 4) || ny_dsp_le32(header + 40) % 4 != 0 ||
      ny_dsp_le32(header + 40) > (uint64_t)info.st_size - sizeof(header))
    {
      close(fd);
      return -ENOTSUP;
    }

  *remaining = ny_dsp_le32(header + 40);
  return fd;
}

/****************************************************************************
 * Name: ny_dsp_worker
 ****************************************************************************/

static void *ny_dsp_worker(void *arg)
{
  struct ny_audio_stream_s *output = NULL;
  struct ny_audio_stream_config_s stream;
  int16_t input[NY_DSP_FRAMES * 2];
  size_t remaining;
  bool voice = false;
  bool bluetooth = false;
  int fd = -1;
  int ret = 0;
  (void)arg;
  fd = ny_dsp_open(g_dsp_track, &remaining);
  if (fd < 0)
    {
      ret = fd;
      goto out;
    }

#ifdef CONFIG_NYABULA_CORE_BT
  ret = ny_product_bt_speaker_claim(false);
  bluetooth = ret == 0;
  if (ret < 0)
    {
      goto out;
    }
#endif
#ifdef CONFIG_NYABULA_CORE_VOICE
  ret = ny_voice_speaker_claim();
  voice = true;
  if (ret < 0)
    {
      goto out;
    }
#endif
  memset(&stream, 0, sizeof(stream));
  stream.dsp_source = "test";
  stream.path = CONFIG_NYABULA_CORE_MEDIA_DEVICE;
  stream.wav = true;
  stream.rate = NYADSP_SAMPLE_RATE;
  stream.channels = 2;
  stream.chunk = sizeof(input);
  stream.buffers = 8;
  ret = ny_audio_stream_open(&output, &stream);
  if (ret < 0)
    {
      goto out;
    }

  while (remaining > 0)
    {
      bool stop;
      ssize_t got;
      size_t bytes = remaining < sizeof(input) ? remaining : sizeof(input);
      nxmutex_lock(&g_dsp_lock);
      stop = g_dsp_stopping;
      nxmutex_unlock(&g_dsp_lock);
      if (stop)
        {
          break;
        }

      got = read(fd, input, bytes);
      if (got <= 0 || got % 4 != 0)
        {
          ret = got < 0 ? -errno : -EIO;
          break;
        }

      remaining -= got;
      ssize_t written = ny_audio_stream_write(output, input, got, 100);
      if (written != got)
        {
          ret = written < 0 ? written : -ETIMEDOUT;
          break;
        }
    }

  if (ret == 0 && remaining == 0)
    {
      ret = ny_audio_stream_drain(output, 500);
    }

out:
  ny_audio_stream_close(output);
  if (fd >= 0)
    {
      close(fd);
    }

#ifdef CONFIG_NYABULA_CORE_VOICE
  if (voice)
    {
      ny_voice_speaker_release();
    }
#else
  (void)voice;
#endif
#ifdef CONFIG_NYABULA_CORE_BT
  if (bluetooth)
    {
      ny_product_bt_speaker_release();
    }
#else
  (void)bluetooth;
#endif
  nxmutex_lock(&g_dsp_lock);
  g_dsp_error = ret;
  g_dsp_running = false;
  g_dsp_stopping = false;
  nxmutex_unlock(&g_dsp_lock);
  return NULL;
}

/****************************************************************************
 * Name: ny_dsp_presets
 ****************************************************************************/

static int ny_dsp_presets(const char *topic, const cJSON *data, cJSON **result)
{
  cJSON *bank = NULL;
  cJSON *item;
  cJSON *entry;
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(data, "name");
  uint64_t revision;
  int index = 0;
  int found = -1;
  int ret = ny_product_store_read(NY_DSP_PRESETS_DOMAIN, &bank, &revision);
  bool listing = strcmp(topic, "dsp.presets") == 0;

  if (ret < 0)
    {
      return ret;
    }
  if (bank == NULL)
    {
      bank = cJSON_CreateObject();
    }
  if (bank == NULL)
    {
      return -ENOMEM;
    }
  item = cJSON_GetObjectItemCaseSensitive(bank, "items");
  if (item == NULL)
    {
      item = cJSON_AddArrayToObject(bank, "items");
    }
  if (!cJSON_IsArray(item))
    {
      ret = -EBADMSG;
      goto out;
    }
  if (listing)
    {
      *result = bank;
      return 0;
    }
  if (!cJSON_IsString(name) || !name->valuestring[0] ||
      strlen(name->valuestring) > NY_DSP_PRESET_NAME_MAX)
    {
      ret = -EINVAL;
      goto out;
    }
  for (const unsigned char *ch = (const unsigned char *)name->valuestring; *ch;
       ch++)
    {
      if (*ch < 32 || *ch == 127)
        {
          ret = -EINVAL;
          goto out;
        }
    }
  cJSON_ArrayForEach(entry, item)
  {
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(entry, "name");
    if (cJSON_IsString(label) &&
        !strcmp(label->valuestring, name->valuestring))
      {
        found = index;
        break;
      }
    index++;
  }
  if (!strcmp(topic, "dsp.preset.save"))
    {
      cJSON *config;
      cJSON *next;
      const cJSON *overwrite =
          cJSON_GetObjectItemCaseSensitive(data, "overwrite");
      if (found >= 0 && !cJSON_IsTrue(overwrite))
        {
          ret = -EEXIST;
          goto out;
        }
      if (found < 0 && cJSON_GetArraySize(item) >= NY_DSP_PRESETS_MAX)
        {
          ret = -ENOSPC;
          goto out;
        }
      nxmutex_lock(&g_dsp_lock);
      config = ny_dsp_config(&g_dsp_settings);
      nxmutex_unlock(&g_dsp_lock);
      next = cJSON_CreateObject();
      if (config == NULL || next == NULL)
        {
          cJSON_Delete(config);
          cJSON_Delete(next);
          ret = -ENOMEM;
          goto out;
        }
      /* Presets own the input EQ and its headroom, never output calibration.
       */
      cJSON_DeleteItemFromObjectCaseSensitive(config, "eqDb");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "eqBands");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "bassEnabled");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "bypass");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "crossoverHz");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "satelliteHighpassHz");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "bassLowpassHz");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "outputGainDb");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "delayMs");
      cJSON_DeleteItemFromObjectCaseSensitive(config, "invert");
      if (!cJSON_AddStringToObject(next, "name", name->valuestring) ||
          !cJSON_AddItemToObject(next, "config", config))
        {
          cJSON_Delete(config);
          cJSON_Delete(next);
          ret = -ENOMEM;
          goto out;
        }
      bool added = found >= 0 ? cJSON_ReplaceItemInArray(item, found, next)
                              : cJSON_AddItemToArray(item, next);
      if (!added)
        {
          cJSON_Delete(next);
          ret = -ENOMEM;
          goto out;
        }
    }
  else if (!strcmp(topic, "dsp.preset.delete"))
    {
      if (found < 0)
        {
          ret = -ENOENT;
          goto out;
        }
      cJSON_DeleteItemFromArray(item, found);
    }
  else
    {
      ret = -ENOSYS;
      goto out;
    }
  ret =
      ny_product_store_write(NY_DSP_PRESETS_DOMAIN, bank, revision, &revision);
  if (ret == 0)
    {
      *result = bank;
      return 0;
    }
out:
  cJSON_Delete(bank);
  return ret;
}

/****************************************************************************
 * Name: ny_product_dsp_request
 ****************************************************************************/

int ny_product_dsp_request(const struct ny_product_caller_s *caller,
                           const char *topic, const cJSON *data,
                           cJSON **result)
{
  struct nyadsp_s *prepared = NULL;
  struct nyadsp_s *retired = NULL;
  int ret;
  if (strncmp(topic, "dsp.", 4) != 0)
    {
      return -ENOSYS;
    }

  if (caller->role < NY_PRODUCT_FAMILY)
    {
      return -EACCES;
    }

  nxmutex_lock(&g_dsp_request_lock);
  nxmutex_lock(&g_dsp_lock);
  ret = ny_dsp_load();
  if (ret < 0)
    {
      goto out;
    }

  if (!strcmp(topic, "dsp.presets") || !strncmp(topic, "dsp.preset.", 11))
    {
      nxmutex_unlock(&g_dsp_lock);
      ret = ny_dsp_presets(topic, data, result);
      nxmutex_lock(&g_dsp_lock);
      goto out;
    }

  if (strcmp(topic, "dsp.config") == 0)
    {
      struct ny_product_dsp_settings_s next = g_dsp_settings;
      cJSON *saved = NULL;
      cJSON *value;
      uint64_t revision;
      const cJSON *persist = cJSON_GetObjectItemCaseSensitive(data, "persist");
      const cJSON *restore = cJSON_GetObjectItemCaseSensitive(data, "restore");
      const cJSON *reset = cJSON_GetObjectItemCaseSensitive(data, "resetEq");
      bool restoring = cJSON_IsTrue(restore);
      bool saving = !restoring && (persist == NULL || cJSON_IsTrue(persist));
      if ((persist != NULL && !cJSON_IsBool(persist)) ||
          (restore != NULL && !cJSON_IsBool(restore)) ||
          (reset != NULL && !cJSON_IsBool(reset)))
        {
          ret = -EINVAL;
          goto out;
        }
      if (restoring)
        {
          next = g_dsp_saved_settings;
          ret = 0;
        }
      else
        {
          if (cJSON_IsTrue(reset))
            {
              ny_dsp_reset_eq(&next);
            }
          ret = ny_dsp_parse(data, &next);
        }
      if (ret < 0)
        {
          goto out;
        }

      /* Build filters and persist without stopping the real-time producer.
       * Publish only after both have succeeded, keeping failed saves atomic.
       */

      nxmutex_unlock(&g_dsp_lock);
      prepared = ny_dsp_output_create(&next);
      ret = prepared == NULL ? -ENOMEM : 0;
      if (saving && ret == 0)
        {
          value = ny_dsp_config(&next);
          ret = ny_product_store_read(NY_DSP_DOMAIN, &saved, &revision);
          cJSON_Delete(saved);
          if (ret == 0)
            {
              ret = value ? ny_product_store_write(NY_DSP_DOMAIN, value,
                                                   revision, &revision)
                          : -ENOMEM;
            }

          cJSON_Delete(value);
        }
      nxmutex_lock(&g_dsp_lock);
      if (ret == 0)
        {
          retired = g_dsp_prepared;
          g_dsp_prepared = g_dsp_output_owned ? prepared : NULL;
          if (g_dsp_output_owned)
            {
              prepared = NULL;
            }
          g_dsp_settings = next;
          g_dsp_settings_saved = saving || restoring;
          if (saving)
            {
              g_dsp_saved_settings = next;
            }
          g_dsp_revision++;
        }
    }
  else if (strcmp(topic, "dsp.play") == 0)
    {
      const cJSON *track = cJSON_GetObjectItemCaseSensitive(data, "track");
      size_t remaining;
      int fd;

      if (g_dsp_running || g_dsp_shutdown)
        {
          ret = -EBUSY;
          goto out;
        }

      if (!cJSON_IsString(track))
        {
          ret = -EINVAL;
          goto out;
        }

      fd = ny_dsp_open(track->valuestring, &remaining);
      if (fd < 0)
        {
          ret = fd;
          goto out;
        }

      close(fd);
      strlcpy(g_dsp_track, track->valuestring, sizeof(g_dsp_track));
      g_dsp_running = true;
      g_dsp_requested = true;
      g_dsp_stopping = false;
      g_dsp_frames = 0;
      g_dsp_error = 0;
      g_dsp_peak = 0;
      g_dsp_clock_ppm = 0;
      g_dsp_usb_starved = 0;
      g_dsp_satellite_queued = 0;
      g_dsp_bass_queued = 0;
    }
  else if (strcmp(topic, "dsp.stop") == 0)
    {
      g_dsp_stopping = g_dsp_running;
    }
  else if (strcmp(topic, "dsp.status") != 0)
    {
      ret = -ENOSYS;
    }

  if (ret == 0)
    {
      *result = ny_dsp_status();
      ret = *result == NULL ? -ENOMEM : 0;
    }

out:
  nxmutex_unlock(&g_dsp_lock);
  nyadsp_destroy(prepared);
  nyadsp_destroy(retired);
  nxmutex_unlock(&g_dsp_request_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_dsp_tick
 *
 * Description:
 *   The persistent product task owns the player thread. Requesters such as
 *   the CLI and plugins may exit immediately after queueing playback.
 ****************************************************************************/

int ny_product_dsp_tick(void)
{
  pthread_attr_t attr;
  int ret = 0;

  nxmutex_lock(&g_dsp_lock);
  if (g_dsp_requested)
    {
      g_dsp_requested = false;
      if (g_dsp_shutdown || g_dsp_stopping)
        {
          g_dsp_running = false;
          g_dsp_stopping = false;
        }
      else
        {
          if (g_dsp_joinable)
            {
              pthread_join(g_dsp_thread, NULL);
              g_dsp_joinable = false;
            }

          pthread_attr_init(&attr);
          pthread_attr_setstacksize(&attr, NY_DSP_STACK);
          ret = pthread_create(&g_dsp_thread, &attr, ny_dsp_worker, NULL);
          pthread_attr_destroy(&attr);
          g_dsp_joinable = ret == 0;
          if (ret != 0)
            {
              g_dsp_running = false;
              g_dsp_error = -ret;
            }
        }
    }

  nxmutex_unlock(&g_dsp_lock);
  return -ret;
}

/****************************************************************************
 * Name: ny_product_dsp_shutdown
 ****************************************************************************/

void ny_product_dsp_shutdown(void)
{
  bool join;
  pthread_t thread;
  nxmutex_lock(&g_dsp_lock);
  g_dsp_stopping = g_dsp_running;
  if (g_dsp_requested)
    {
      g_dsp_requested = false;
      g_dsp_running = false;
      g_dsp_stopping = false;
    }
  join = g_dsp_joinable;
  thread = g_dsp_thread;
  g_dsp_joinable = false;
  g_dsp_shutdown = true;
  nxmutex_unlock(&g_dsp_lock);
  if (join)
    {
      pthread_join(thread, NULL);
    }

  nxmutex_lock(&g_dsp_lock);
  g_dsp_shutdown = false;
  nxmutex_unlock(&g_dsp_lock);
}

/****************************************************************************
 * Name: ny_product_dsp_acquire
 ****************************************************************************/

int ny_product_dsp_acquire(const char *source,
                           struct ny_product_dsp_settings_s *settings,
                           uint32_t *revision)
{
  int ret;
  nxmutex_lock(&g_dsp_lock);
  ret = ny_dsp_load();
  if (ret == 0 && g_dsp_output_owned)
    {
      ret = -EBUSY;
    }

  if (ret == 0)
    {
      g_dsp_output_owned = true;
      strlcpy(g_dsp_source, source, sizeof(g_dsp_source));
      g_dsp_frames = 0;
      g_dsp_peak = 0;
      g_dsp_error = 0;
      g_dsp_clock_ppm = 0;
      g_dsp_usb_starved = 0;
      g_dsp_satellite_queued = 0;
      g_dsp_bass_queued = 0;
      *settings = g_dsp_settings;
      *revision = g_dsp_revision;
    }

  nxmutex_unlock(&g_dsp_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_dsp_snapshot
 ****************************************************************************/

void ny_product_dsp_snapshot(struct ny_product_dsp_settings_s *settings,
                             uint32_t *revision)
{
  nxmutex_lock(&g_dsp_lock);
  *settings = g_dsp_settings;
  *revision = g_dsp_revision;
  nxmutex_unlock(&g_dsp_lock);
}

/****************************************************************************
 * Name: ny_product_dsp_take_update
 *
 * Description:
 *   Transfer a fully prepared filter to the sole output owner. No allocation,
 *   coefficient calculation or storage I/O runs on the audio thread.
 ****************************************************************************/

struct nyadsp_s *
ny_product_dsp_take_update(struct ny_product_dsp_settings_s *settings,
                           uint32_t *revision)
{
  struct nyadsp_s *prepared;
  nxmutex_lock(&g_dsp_lock);
  prepared = g_dsp_prepared;
  g_dsp_prepared = NULL;
  *settings = g_dsp_settings;
  *revision = g_dsp_revision;
  nxmutex_unlock(&g_dsp_lock);
  return prepared;
}

/****************************************************************************
 * Name: ny_product_dsp_progress
 ****************************************************************************/

void ny_product_dsp_progress(size_t frames, float peak, double ppm,
                             uint32_t starved, size_t satellite_frames,
                             size_t bass_frames)
{
  nxmutex_lock(&g_dsp_lock);
  g_dsp_frames += frames;
  g_dsp_peak = peak;
  g_dsp_clock_ppm = ppm;
  g_dsp_usb_starved = starved;
  g_dsp_satellite_queued = satellite_frames;
  g_dsp_bass_queued = bass_frames;
  nxmutex_unlock(&g_dsp_lock);
}

/****************************************************************************
 * Name: ny_product_dsp_release
 ****************************************************************************/

void ny_product_dsp_release(int error)
{
  struct nyadsp_s *prepared;
  nxmutex_lock(&g_dsp_lock);
  prepared = g_dsp_prepared;
  g_dsp_prepared = NULL;
  g_dsp_output_owned = false;
  g_dsp_source[0] = '\0';
  g_dsp_error = error;
  nxmutex_unlock(&g_dsp_lock);
  nyadsp_destroy(prepared);
}
