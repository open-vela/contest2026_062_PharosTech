/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_media.c
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

#include "ny_product.h"
#include "ny_product_store.h"
#include <errno.h>
#include <nuttx/config.h>
#include <string.h>
#ifdef CONFIG_NYABULA_CORE_MEDIA
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <nuttx/audio/audio.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <system/nxplayer.h>
#include <unistd.h>
#ifdef CONFIG_NYABULA_CORE_AUDIO
#include "ny_product_audio.h"
#endif

#define NY_MEDIA_TRACK_MAX   96
#define NY_MEDIA_WAIT_MS     5000
#define NY_MEDIA_START_MS    1000
#define NY_MEDIA_LIBRARY_MAX 64
#define NY_MEDIA_PROBE_MS    2000

struct ny_media_wave_s
{
  uint32_t rate;
  uint32_t bytes;
  uint16_t channels;
  uint16_t bits;
  uint64_t duration;
};

struct ny_media_job_s
{
  bool busy;
  bool executing;
  bool done;
  bool abandoned;
  char action[24];
  char name[NY_MEDIA_TRACK_MAX];
  char device[128];
  int volume;
  bool muted;
  int error;
};

static mutex_t g_media_lock = NXMUTEX_INITIALIZER;
static struct ny_media_job_s g_media_job;
static struct nxplayer_s *g_media_player;
static char g_media_track[NY_MEDIA_TRACK_MAX];
static char g_media_device[128] = CONFIG_NYABULA_CORE_MEDIA_DEVICE;
static int g_media_state = NXPLAYER_STATE_IDLE;
static int g_media_volume = 40;
static bool g_media_muted;
static bool g_media_volume_supported;
static uint64_t g_media_probe_after;
static int g_media_error;
static uint64_t g_media_elapsed;
static uint64_t g_media_duration;
static uint64_t g_media_sampled;
static bool g_media_preferences_loaded;
static bool g_media_settings_saved = true;

static uint32_t ny_media_le32(const unsigned char *bytes);
static int ny_media_wave(const char *name, struct ny_media_wave_s *wave);
static cJSON *ny_media_status(void);
static int ny_media_library(cJSON **result);
static bool ny_media_name(const char *name);
static bool ny_media_volume_support(const char *device);
static void ny_media_volume_probe(void);
static bool ny_media_codec(const char *device);
static void ny_media_levels_sync(void);
static int ny_media_execute(const struct ny_media_job_s *job);
static int ny_media_preferences_load(void);
static int ny_media_preferences_save(void);

/****************************************************************************
 * Name: ny_media_preferences_load
 ****************************************************************************/

static int ny_media_preferences_load(void)
{
  if (g_media_preferences_loaded)
    return 0;
  cJSON *root = NULL;
  uint64_t revision;
  int ret = ny_product_store_read("media", &root, &revision);
  if (ret == 0 && root != NULL)
    {
      const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
      const cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
      const cJSON *muted = cJSON_GetObjectItemCaseSensitive(root, "muted");
      const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
      if (!cJSON_IsNumber(schema) || schema->valueint != 1 ||
          !cJSON_IsNumber(volume) || !isfinite(volume->valuedouble) ||
          volume->valuedouble < 0 || volume->valuedouble > 100 ||
          floor(volume->valuedouble) != volume->valuedouble ||
          !cJSON_IsBool(muted) || !cJSON_IsString(device) ||
          strncmp(device->valuestring, "/dev/audio/", 11) ||
          strlen(device->valuestring) >= sizeof(g_media_device))
        ret = -EBADMSG;
      else
        {
          g_media_volume = volume->valueint;
          g_media_muted = cJSON_IsTrue(muted);
          snprintf(g_media_device, sizeof(g_media_device), "%s",
                   device->valuestring);
        }
    }
  cJSON_Delete(root);
  if (ret == 0)
    g_media_preferences_loaded = true;
  return ret;
}

/****************************************************************************
 * Name: ny_media_preferences_save
 ****************************************************************************/

static int ny_media_preferences_save(void)
{
  cJSON *previous = NULL;
  uint64_t revision;
  int ret = ny_product_store_read("media", &previous, &revision);
  cJSON_Delete(previous);
  cJSON *root = cJSON_CreateObject();
  bool valid = root != NULL;
  valid &= cJSON_AddNumberToObject(root, "schema", 1) != NULL;
  valid &= cJSON_AddNumberToObject(root, "volume", g_media_volume) != NULL;
  valid &= cJSON_AddBoolToObject(root, "muted", g_media_muted) != NULL;
  valid &= cJSON_AddStringToObject(root, "device", g_media_device) != NULL;
  if (ret == 0)
    ret = valid ? ny_product_store_write("media", root, revision, &revision)
                : -ENOMEM;
  cJSON_Delete(root);
  g_media_settings_saved = ret == 0;
  return ret;
}

/****************************************************************************
 * Name: ny_media_le32
 ****************************************************************************/

static uint32_t ny_media_le32(const unsigned char *bytes)
{
  return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
         (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/****************************************************************************
 * Name: ny_media_name
 ****************************************************************************/

static bool ny_media_name(const char *name)
{
  return name && name[0] && strlen(name) < NY_MEDIA_TRACK_MAX &&
         !strchr(name, '/') && !strchr(name, '\\') && strcmp(name, ".") &&
         strcmp(name, "..");
}

/****************************************************************************
 * Name: ny_media_wave
 ****************************************************************************/

static int ny_media_wave(const char *name, struct ny_media_wave_s *wave)
{
  if (!ny_media_name(name))
    return -EINVAL;
  char path[256];
  int length = snprintf(path, sizeof(path), "%s/%s",
                        CONFIG_NYABULA_CORE_MEDIA_ROOT, name);
  if (length < 0 || (size_t)length >= sizeof(path))
    return -ENAMETOOLONG;
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -errno;
  struct stat info;
  unsigned char header[16];
  int ret = -ENOTSUP;
  bool format = false;
  memset(wave, 0, sizeof(*wave));
  if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) ||
      read(fd, header, 12) != 12 || memcmp(header, "RIFF", 4) ||
      memcmp(header + 8, "WAVE", 4))
    goto failed;
  for (int i = 0; i < 64; i++)
    {
      if (read(fd, header, 8) != 8)
        goto failed;
      uint32_t size = ny_media_le32(header + 4);
      off_t position = lseek(fd, 0, SEEK_CUR);
      if (position < 0 || (uint64_t)position + size > (uint64_t)info.st_size)
        goto failed;
      if (!memcmp(header, "fmt ", 4))
        {
          if (size < 16 || read(fd, header, 16) != 16 || header[0] != 1 ||
              header[1] != 0)
            goto failed;
          wave->channels = header[2] | (uint16_t)header[3] << 8;
          wave->rate = ny_media_le32(header + 4);
          wave->bits = header[14] | (uint16_t)header[15] << 8;
          format = wave->channels >= 1 && wave->channels <= 2 &&
                   wave->bits == 16 && wave->rate >= 8000 &&
                   wave->rate <= 192000;
        }
      else if (!memcmp(header, "data", 4))
        {
          /* This initial path supports a final PCM data chunk, not trailers.
           */
          if (!format || !size || size % (wave->channels * 2) ||
              (uint64_t)position + size != (uint64_t)info.st_size)
            goto failed;
          wave->bytes = size;
          wave->duration =
              (uint64_t)size * 1000 / (wave->rate * wave->channels * 2);
          return fd;
        }
      if (lseek(fd, position + size + (size & 1), SEEK_SET) < 0)
        goto failed;
    }
failed:
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: ny_media_status
 ****************************************************************************/

static cJSON *ny_media_status(void)
{
  cJSON *result = cJSON_CreateObject();
  if (result == NULL)
    return NULL;
  cJSON_AddStringToObject(result, "state",
                          g_media_state == NXPLAYER_STATE_PLAYING  ? "playing"
                          : g_media_state == NXPLAYER_STATE_PAUSED ? "paused"
                                                                   : "idle");
  cJSON_AddStringToObject(result, "track", g_media_track);
  cJSON_AddStringToObject(result, "device", g_media_device);
  cJSON_AddStringToObject(result, "root", CONFIG_NYABULA_CORE_MEDIA_ROOT);
  cJSON_AddNumberToObject(result, "volume", g_media_volume);
  cJSON_AddBoolToObject(result, "muted", g_media_muted);
  cJSON_AddBoolToObject(result, "volumeSupported", g_media_volume_supported);
  cJSON_AddBoolToObject(result, "seekSupported", false);
  cJSON_AddBoolToObject(result, "settingsSaved", g_media_settings_saved);
  cJSON_AddBoolToObject(result, "busy", g_media_job.busy && !g_media_job.done);
  cJSON_AddNumberToObject(result, "elapsedMs", g_media_elapsed);
  cJSON_AddNumberToObject(result, "durationMs", g_media_duration);
  cJSON_AddNumberToObject(result, "sampledAtMs", g_media_sampled);
  cJSON_AddNumberToObject(result, "lastError", g_media_error);
  return result;
}

/****************************************************************************
 * Name: ny_media_library
 ****************************************************************************/

static int ny_media_library(cJSON **result)
{
  *result = cJSON_CreateObject();
  cJSON *items = cJSON_AddArrayToObject(*result, "items");
  if (*result == NULL || items == NULL)
    return -ENOMEM;
  cJSON_AddStringToObject(*result, "root", CONFIG_NYABULA_CORE_MEDIA_ROOT);
  DIR *directory = opendir(CONFIG_NYABULA_CORE_MEDIA_ROOT);
  if (!directory)
    {
      cJSON_AddBoolToObject(*result, "available", false);
      cJSON_AddNumberToObject(*result, "error", -errno);
      return 0;
    }
  cJSON_AddBoolToObject(*result, "available", true);
  struct dirent *entry;
  int count = 0;
  while ((entry = readdir(directory)) != NULL && count < NY_MEDIA_LIBRARY_MAX)
    {
      if (entry->d_name[0] == '.' || !ny_media_name(entry->d_name))
        continue;
      struct ny_media_wave_s wave;
      int fd = ny_media_wave(entry->d_name, &wave);
      cJSON *item = cJSON_CreateObject();
      if (!item)
        {
          if (fd >= 0)
            close(fd);
          break;
        }
      cJSON_AddStringToObject(item, "name", entry->d_name);
      cJSON_AddBoolToObject(item, "supported", fd >= 0);
      if (fd >= 0)
        {
          close(fd);
          cJSON_AddNumberToObject(item, "durationMs", wave.duration);
          cJSON_AddNumberToObject(item, "sampleRate", wave.rate);
          cJSON_AddNumberToObject(item, "channels", wave.channels);
          cJSON_AddNumberToObject(item, "bits", wave.bits);
        }
      else
        cJSON_AddNumberToObject(item, "error", fd);
      cJSON_AddItemToArray(items, item);
      count++;
    }
  closedir(directory);
  return 0;
}

/****************************************************************************
 * Name: ny_media_volume_support
 ****************************************************************************/

static bool ny_media_volume_support(const char *device)
{
  int fd = open(device, O_RDONLY);
  if (fd < 0)
    return false;
  /* A feature-unit query is sub-typed AUDIO_FU_UNDEF.  That is the same
   * value as AUDIO_TYPE_QUERY, so the older spelling here asked the right
   * question; it was only ever asked too late (see ny_media_volume_probe).
   */

  struct audio_caps_s caps = { .ac_len = sizeof(caps),
                               .ac_type = AUDIO_TYPE_FEATURE,
                               .ac_subtype = AUDIO_FU_UNDEF };
  int ret = ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)(uintptr_t)&caps);
  close(fd);
  return ret >= 0 && (caps.ac_controls.hw[0] & AUDIO_FU_VOLUME) != 0;
}

/****************************************************************************
 * Name: ny_media_volume_probe
 *
 * Description:
 *   The capability used to be asked only by music.play and music.output, so
 *   a device that had not played anything yet reported no volume control
 *   and refused music.volume.  Ask from the status path instead, until the
 *   node answers; it may not be registered yet right after boot.  Called
 *   with g_media_lock held.
 *
 ****************************************************************************/

static void ny_media_volume_probe(void)
{
  uint64_t now = ny_product_time_ms(true);
  if (g_media_volume_supported || now < g_media_probe_after)
    return;
  g_media_probe_after = now + NY_MEDIA_PROBE_MS;
  g_media_volume_supported = ny_media_volume_support(g_media_device);
}

/****************************************************************************
 * Name: ny_media_codec
 *
 * Description:
 *   Whether the output is the codec whose levels the audio service owns.
 *   Any other node (USB audio) keeps the player's own volume path.
 *
 ****************************************************************************/

static bool ny_media_codec(const char *device)
{
#ifdef CONFIG_NYABULA_CORE_AUDIO
  return strcmp(device, NY_PRODUCT_AUDIO_OUTPUT) == 0;
#else
  (void)device;
  return false;
#endif
}

/****************************************************************************
 * Name: ny_media_levels_sync
 *
 * Description:
 *   audio.volume and audioctl move the codec volume without going through
 *   here: report, and start the next track at, what the codec really has.
 *
 ****************************************************************************/

static void ny_media_levels_sync(void)
{
#ifdef CONFIG_NYABULA_CORE_AUDIO
  struct ny_product_audio_levels_s levels;
  if (ny_media_codec(g_media_device) && ny_product_audio_levels(&levels) == 0)
    {
      g_media_volume = levels.volume;
      g_media_muted = levels.muted;
    }
#endif
}

/****************************************************************************
 * Name: ny_media_execute
 ****************************************************************************/

static int ny_media_execute(const struct ny_media_job_s *job)
{
  if (!g_media_player)
    g_media_player = nxplayer_create();
  if (!g_media_player)
    return -ENOMEM;
  int state = nxplayer_getstate(g_media_player);
  if (!strcmp(job->action, "music.play"))
    {
      if (state != NXPLAYER_STATE_IDLE)
        return -EBUSY;
      struct ny_media_wave_s wave;
      int fd = ny_media_wave(job->name, &wave);
      if (fd < 0)
        return fd;
      int ret = nxplayer_setdevice(g_media_player, g_media_device);
      bool supported = ny_media_volume_support(g_media_device);
#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
      if (ret == 0 && supported)
        {
          /* nxplayer writes the volume it holds to the device when a track
           * starts, so it has to hold the one the codec was last given.
           */

          nxmutex_lock(&g_media_lock);
          ny_media_levels_sync();
          int level = g_media_muted ? 0 : g_media_volume * 10;
          nxmutex_unlock(&g_media_lock);
          ret = nxplayer_setvolume(g_media_player, level);
        }
#endif
      if (ret == 0)
        ret = nxplayer_playpcmfd(g_media_player, fd, wave.channels, wave.bits,
                                 wave.rate);
      if (ret < 0)
        {
          close(fd);
          return ret;
        }
      uint64_t deadline = ny_product_time_ms(true) + NY_MEDIA_START_MS;
      while (nxplayer_getstate(g_media_player) == NXPLAYER_STATE_IDLE &&
             ny_product_time_ms(true) < deadline)
        usleep(10000);
      if (nxplayer_getstate(g_media_player) == NXPLAYER_STATE_IDLE)
        return -EIO;
      nxmutex_lock(&g_media_lock);
      snprintf(g_media_track, sizeof(g_media_track), "%s", job->name);
      g_media_duration = wave.duration;
      g_media_elapsed = 0;
      g_media_sampled = ny_product_time_ms(true);
      g_media_state = NXPLAYER_STATE_PLAYING;
      g_media_volume_supported = supported;
      nxmutex_unlock(&g_media_lock);
      return 0;
    }
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
  if (!strcmp(job->action, "music.pause"))
    return state == NXPLAYER_STATE_PLAYING ? nxplayer_pause(g_media_player)
                                           : -EALREADY;
  if (!strcmp(job->action, "music.resume"))
    return state == NXPLAYER_STATE_PAUSED ? nxplayer_resume(g_media_player)
                                          : -EALREADY;
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  if (!strcmp(job->action, "music.stop"))
    return nxplayer_stop(g_media_player);
#endif
  if (!strcmp(job->action, "music.output"))
    {
      if (state != NXPLAYER_STATE_IDLE)
        return -EBUSY;
      int ret = nxplayer_setdevice(g_media_player, job->device);
      if (ret == 0)
        {
          bool supported = ny_media_volume_support(job->device);
          nxmutex_lock(&g_media_lock);
          snprintf(g_media_device, sizeof(g_media_device), "%s", job->device);
          g_media_volume_supported = supported;
          ret = ny_media_preferences_save();
          nxmutex_unlock(&g_media_lock);
        }
      return ret;
    }
#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
  if (!strcmp(job->action, "music.volume"))
    {
      bool codec = ny_media_codec(g_media_device);
      if (!codec && !g_media_volume_supported)
        return -ENOTSUP;
      int ret = 0;
#ifdef CONFIG_NYABULA_CORE_AUDIO
      /* nxplayer only reaches the device while it holds one; the audio
       * service reaches the codec at any time and keeps audio.status, the
       * panel and audioctl on the same number.
       */

      if (codec)
        ret = ny_product_audio_set_levels(job->volume, job->muted ? 1 : 0,
                                          NY_PRODUCT_AUDIO_KEEP);
#endif
      if (ret == 0)
        ret = nxplayer_setvolume(g_media_player,
                                 job->muted ? 0 : job->volume * 10);
      if (ret == 0)
        {
          nxmutex_lock(&g_media_lock);
          g_media_volume = job->volume;
          g_media_muted = job->muted;
          ret = ny_media_preferences_save();
          nxmutex_unlock(&g_media_lock);
        }
      return ret;
    }
#endif
  return -ENOSYS;
}
#endif

/****************************************************************************
 * Name: ny_product_media_request
 ****************************************************************************/

int ny_product_media_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result)
{
  if (strncmp(topic, "music.", 6))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
#ifndef CONFIG_NYABULA_CORE_MEDIA
  (void)data;
  (void)result;
  return -ENOSYS;
#else
  if (!strcmp(topic, "music.library"))
    return ny_media_library(result);
  nxmutex_lock(&g_media_lock);
  int preferences = ny_media_preferences_load();
  if (preferences < 0)
    {
      nxmutex_unlock(&g_media_lock);
      return preferences;
    }
  if (!strcmp(topic, "music.status"))
    {
      ny_media_volume_probe();
      ny_media_levels_sync();
      *result = ny_media_status();
      nxmutex_unlock(&g_media_lock);
      return *result ? 0 : -ENOMEM;
    }
  if (g_media_job.busy)
    {
      nxmutex_unlock(&g_media_lock);
      return -EBUSY;
    }
  struct ny_media_job_s job = { .busy = true };
  snprintf(job.action, sizeof(job.action), "%s", topic);
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(data, "name");
  const cJSON *volume = cJSON_GetObjectItemCaseSensitive(data, "volume");
  const cJSON *muted = cJSON_GetObjectItemCaseSensitive(data, "muted");
  const cJSON *device = cJSON_GetObjectItemCaseSensitive(data, "device");
  int ret = 0;
  if (!strcmp(topic, "music.play"))
    {
      if (!cJSON_IsString(name) || !ny_media_name(name->valuestring))
        ret = -EINVAL;
      else
        snprintf(job.name, sizeof(job.name), "%s", name->valuestring);
    }
  else if (!strcmp(topic, "music.volume"))
    {
      if (!cJSON_IsNumber(volume) || !isfinite(volume->valuedouble) ||
          volume->valuedouble < 0 || volume->valuedouble > 100 ||
          floor(volume->valuedouble) != volume->valuedouble ||
          (muted && !cJSON_IsBool(muted)))
        ret = -EINVAL;
      else
        {
          job.volume = volume->valueint;
          job.muted = cJSON_IsTrue(muted);
        }
    }
  else if (!strcmp(topic, "music.output"))
    {
      if (!cJSON_IsString(device) ||
          strncmp(device->valuestring, "/dev/audio/", 11) ||
          strlen(device->valuestring) >= sizeof(job.device) ||
          strchr(device->valuestring + 11, '/'))
        ret = -EINVAL;
      else
        snprintf(job.device, sizeof(job.device), "%s", device->valuestring);
    }
  else if (strcmp(topic, "music.pause") && strcmp(topic, "music.resume") &&
           strcmp(topic, "music.stop"))
    ret = -ENOSYS;
  if (ret < 0)
    {
      nxmutex_unlock(&g_media_lock);
      return ret;
    }
  g_media_job = job;
  nxmutex_unlock(&g_media_lock);
  ret = ny_product_start();
  if (ret < 0)
    {
      nxmutex_lock(&g_media_lock);
      g_media_job.busy = false;
      nxmutex_unlock(&g_media_lock);
      return ret;
    }
  uint64_t deadline = ny_product_time_ms(true) + NY_MEDIA_WAIT_MS;
  for (;;)
    {
      nxmutex_lock(&g_media_lock);
      if (g_media_job.done)
        {
          ret = g_media_job.error;
          g_media_job.busy = false;
          *result = ny_media_status();
          nxmutex_unlock(&g_media_lock);
          return *result ? ret : -ENOMEM;
        }
      if (ny_product_time_ms(true) >= deadline)
        {
          g_media_job.abandoned = true;
          nxmutex_unlock(&g_media_lock);
          return -ETIMEDOUT;
        }
      nxmutex_unlock(&g_media_lock);
      usleep(10000);
    }
#endif
}

/****************************************************************************
 * Name: ny_product_media_tick
 ****************************************************************************/

int ny_product_media_tick(void)
{
#ifdef CONFIG_NYABULA_CORE_MEDIA
  struct ny_media_job_s job;
  nxmutex_lock(&g_media_lock);
  bool execute =
      g_media_job.busy && !g_media_job.executing && !g_media_job.done;
  if (execute)
    {
      g_media_job.executing = true;
      job = g_media_job;
    }
  nxmutex_unlock(&g_media_lock);
  int ret = execute ? ny_media_execute(&job) : 0;
  int state =
      g_media_player ? nxplayer_getstate(g_media_player) : NXPLAYER_STATE_IDLE;
  uint64_t now = ny_product_time_ms(true);
  nxmutex_lock(&g_media_lock);
  if (g_media_state == NXPLAYER_STATE_PLAYING && g_media_sampled &&
      now > g_media_sampled)
    {
      g_media_elapsed += now - g_media_sampled;
      if (g_media_elapsed > g_media_duration)
        g_media_elapsed = g_media_duration;
    }
  g_media_state = state;
  g_media_sampled = now;
  if (execute)
    {
      g_media_error = ret;
      g_media_job.error = ret;
      g_media_job.done = true;
      if (g_media_job.abandoned)
        g_media_job.busy = false;
    }
  nxmutex_unlock(&g_media_lock);
  return ret;
#else
  return 0;
#endif
}

/****************************************************************************
 * Name: ny_product_media_shutdown
 ****************************************************************************/

void ny_product_media_shutdown(void)
{
#ifdef CONFIG_NYABULA_CORE_MEDIA
  if (g_media_player)
    {
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
      nxplayer_stop(g_media_player);
#endif
      nxplayer_release(g_media_player);
      g_media_player = NULL;
    }
  nxmutex_lock(&g_media_lock);
  g_media_state = NXPLAYER_STATE_IDLE;
  if (g_media_job.busy)
    {
      g_media_job.error = -ECANCELED;
      g_media_job.done = true;
    }
  nxmutex_unlock(&g_media_lock);
#endif
}
