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
#include <sys/param.h>
#include <sys/stat.h>
#include <system/nxplayer.h>
#include <unistd.h>
#ifdef CONFIG_NYABULA_CORE_AUDIO
#include "ny_product_audio.h"
#endif
#ifdef CONFIG_NYABULA_CORE_BT
#include "ny_product_bt.h"
#endif
#ifdef CONFIG_NYABULA_CORE_VOICE
#include "ny_voice.h"
#endif

#define NY_MEDIA_TRACK_MAX   96
#define NY_MEDIA_WAIT_MS     5000
#define NY_MEDIA_START_MS    1000
#define NY_MEDIA_LIBRARY_MAX 64
#define NY_MEDIA_PROBE_MS    2000
#define NY_MEDIA_WAVE_HEADER 44 /* RIFF + fmt(16) + data chunk header */

/* The alert chime.  The leading dot keeps it out of music.library. */

#define NY_MEDIA_CHIME         ".nyabula-chime.wav"
#define NY_MEDIA_CHIME_RATE    44100
#define NY_MEDIA_CHIME_HZ      880
#define NY_MEDIA_CHIME_BEEPS   3
#define NY_MEDIA_CHIME_ON_MS   160
#define NY_MEDIA_CHIME_OFF_MS  110
#define NY_MEDIA_CHIME_FADE_MS 12
#define NY_MEDIA_CHIME_LEVEL   14000
#define NY_MEDIA_CHIME_GAP_MS  1200

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
static int g_media_alert = NY_PRODUCT_MEDIA_ALERT_OFF;
static bool g_media_alert_silence;
static bool g_media_sleep;

#ifdef CONFIG_NYABULA_CORE_VOICE
/* The player holds the microphone shut while a track runs.  Only the media
 * tick reads and clears it, and only ny_media_start sets it, both on the
 * same thread, so it needs no lock.
 */

static bool g_media_claimed;
#endif
static uint64_t g_media_alert_next;

static uint32_t ny_media_le32(const unsigned char *bytes);
static void ny_media_put_le32(unsigned char *bytes, uint32_t value);
static int ny_media_chime_create(void);
static int ny_media_start(const char *name);
static void ny_media_halt(void);
static void ny_media_alert_tick(int state, uint64_t now);
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

          /* /dev/audio/pcm0 is the PCM decoder in front of the codec: it
           * reads the WAV header itself and rejects a stream that starts at
           * the samples ("Invalid PCM WAV file"), after which the player
           * never reaches PLAYING.  It only knows the canonical 44-byte
           * header, so that is the only layout worth accepting here, and
           * the descriptor goes back to the start of the file.
           */

          if (position != NY_MEDIA_WAVE_HEADER || lseek(fd, 0, SEEK_SET) < 0)
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
  cJSON_AddBoolToObject(result, "alert",
                        g_media_alert != NY_PRODUCT_MEDIA_ALERT_OFF ||
                            (g_media_state != NXPLAYER_STATE_IDLE &&
                             !strcmp(g_media_track, NY_MEDIA_CHIME)));

  /* What is on the speaker right now, whoever put it there. */

  const char *source = g_media_state == NXPLAYER_STATE_IDLE     ? NULL
                       : !strcmp(g_media_track, NY_MEDIA_CHIME) ? "alert"
                                                                : "flash";
#ifdef CONFIG_NYABULA_CORE_BT
  if (source == NULL)
    source = ny_product_bt_speaker_source();
#endif
  cJSON_AddStringToObject(result, "source", source ? source : "none");
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
 * Name: ny_media_put_le32
 ****************************************************************************/

static void ny_media_put_le32(unsigned char *bytes, uint32_t value)
{
  bytes[0] = value & 0xff;
  bytes[1] = (value >> 8) & 0xff;
  bytes[2] = (value >> 16) & 0xff;
  bytes[3] = (value >> 24) & 0xff;
}

/****************************************************************************
 * Name: ny_media_chime_create
 * Description: Synthesise the alert chime into the media directory.
 *
 *   The tone is generated on the device rather than shipped: a repository
 *   of sources has no place for a binary sound, and a file lets the alert
 *   reuse the one playback path that is known to work instead of growing a
 *   second, streaming one.
 ****************************************************************************/

static int ny_media_chime_create(void)
{
  const uint32_t period = NY_MEDIA_CHIME_ON_MS + NY_MEDIA_CHIME_OFF_MS;
  const uint32_t frames = (uint32_t)((uint64_t)NY_MEDIA_CHIME_RATE *
                                     NY_MEDIA_CHIME_BEEPS * period / 1000);
  const uint32_t bytes = frames * 4;
  unsigned char header[44];
  static int16_t block[2048];
  char path[256];
  char partial[264];
  int length = snprintf(path, sizeof(path), "%s/%s",
                        CONFIG_NYABULA_CORE_MEDIA_ROOT, NY_MEDIA_CHIME);
  if (length < 0 || (size_t)length >= sizeof(path))
    return -ENAMETOOLONG;
  snprintf(partial, sizeof(partial), "%s.tmp", path);
  mkdir(CONFIG_NYABULA_CORE_MEDIA_ROOT, 0755);
  int fd = open(partial, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -errno;
  memcpy(header, "RIFF", 4);
  ny_media_put_le32(header + 4, 36 + bytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  ny_media_put_le32(header + 16, 16);
  header[20] = 1; /* PCM */
  header[21] = 0;
  header[22] = 2; /* Stereo: the format every track played so far has had */
  header[23] = 0;
  ny_media_put_le32(header + 24, NY_MEDIA_CHIME_RATE);
  ny_media_put_le32(header + 28, NY_MEDIA_CHIME_RATE * 4);
  header[32] = 4;
  header[33] = 0;
  header[34] = 16;
  header[35] = 0;
  memcpy(header + 36, "data", 4);
  ny_media_put_le32(header + 40, bytes);
  int ret = write(fd, header, sizeof(header)) == sizeof(header) ? 0 : -EIO;
  uint32_t filled = 0;
  for (uint32_t frame = 0; ret == 0 && frame < frames; frame++)
    {
      uint32_t ms = (uint32_t)((uint64_t)frame * 1000 / NY_MEDIA_CHIME_RATE);
      uint32_t within = ms % period;
      float gain = 0;
      if (within < NY_MEDIA_CHIME_ON_MS)
        {
          /* Ramp both edges: a tone that starts or stops at full level
           * clicks.
           */

          uint32_t edge = within < NY_MEDIA_CHIME_ON_MS - within
                              ? within
                              : NY_MEDIA_CHIME_ON_MS - within;
          gain = edge >= NY_MEDIA_CHIME_FADE_MS
                     ? 1.0f
                     : (float)edge / NY_MEDIA_CHIME_FADE_MS;
        }
      float phase = 2.0f * (float)M_PI * NY_MEDIA_CHIME_HZ *
                    (float)(frame % NY_MEDIA_CHIME_RATE) / NY_MEDIA_CHIME_RATE;
      int16_t sample = (int16_t)(sinf(phase) * gain * NY_MEDIA_CHIME_LEVEL);
      block[filled++] = sample;
      block[filled++] = sample;
      if (filled == nitems(block) || frame + 1 == frames)
        {
          ssize_t size = filled * sizeof(block[0]);
          if (write(fd, block, size) != size)
            ret = -EIO;
          filled = 0;
        }
    }
  if (close(fd) < 0 && ret == 0)
    ret = -errno;
  if (ret == 0 && rename(partial, path) < 0)
    ret = -errno;
  if (ret < 0)
    unlink(partial);
  return ret;
}

/****************************************************************************
 * Name: ny_media_speaker_release
 * Description: Give the microphone back once the player is done with the
 *   codec.  Paired with the claim ny_media_start takes; doing nothing when
 *   voice is not in the build keeps the call sites free of #ifdef.
 ****************************************************************************/

static void ny_media_speaker_release(void)
{
#ifdef CONFIG_NYABULA_CORE_VOICE
  if (g_media_claimed)
    {
      g_media_claimed = false;
      ny_voice_speaker_release();
    }
#endif
}

/****************************************************************************
 * Name: ny_media_start
 * Description: Start one track of the media directory on an idle player.
 ****************************************************************************/

static int ny_media_start(const char *name)
{
  struct ny_media_wave_s wave;
  int fd = ny_media_wave(name, &wave);
  if (fd < 0)
    return fd;
#ifdef CONFIG_NYABULA_CORE_BT
  /* One owner of the speaker at a time: Bluetooth music lets go of the
   * device before the player opens it (for good when this is a track,
   * until the chime is over when it is the alert), and the tick gives the
   * speaker back once the player is idle.  Only a phone call says no.
   */

  int claim = ny_product_bt_speaker_claim(!strcmp(name, NY_MEDIA_CHIME));
  if (claim < 0)
    {
      close(fd);
      return claim;
    }
#endif
#ifdef CONFIG_NYABULA_CORE_VOICE
  /* The wake word holds the microphone open, and the ES8388 refuses a
   * playback format that differs from the capture one while it is: music
   * is 44.1 kHz, capture is 16 kHz, so without this the player never left
   * IDLE and every track came back as EIO once voice was switched on.
   */

  int listening = ny_voice_speaker_claim();
  if (listening < 0)
    {
      close(fd);
      return listening;
    }

  g_media_claimed = true;
#endif
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
      ny_media_speaker_release();
      return ret;
    }
  uint64_t deadline = ny_product_time_ms(true) + NY_MEDIA_START_MS;
  while (nxplayer_getstate(g_media_player) == NXPLAYER_STATE_IDLE &&
         ny_product_time_ms(true) < deadline)
    usleep(10000);
  if (nxplayer_getstate(g_media_player) == NXPLAYER_STATE_IDLE)
    {
      ny_media_speaker_release();
      return -EIO;
    }
  nxmutex_lock(&g_media_lock);
  snprintf(g_media_track, sizeof(g_media_track), "%s", name);
  g_media_duration = wave.duration;
  g_media_elapsed = 0;
  g_media_sampled = ny_product_time_ms(true);
  g_media_state = NXPLAYER_STATE_PLAYING;
  g_media_volume_supported = supported;
  nxmutex_unlock(&g_media_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_media_halt
 ****************************************************************************/

static void ny_media_halt(void)
{
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  if (g_media_player)
    nxplayer_stop(g_media_player);
#endif
}

/****************************************************************************
 * Name: ny_media_alert_tick
 * Description: Act on the alert and sleep requests other services left.
 *
 *   Runs on the media tick because that is the only thread that touches the
 *   player.  A request made over the panel wins over an alert for the tick
 *   it executes in; the alert simply tries again 100 ms later.
 ****************************************************************************/

static void ny_media_alert_tick(int state, uint64_t now)
{
  nxmutex_lock(&g_media_lock);
  int alert = g_media_alert;
  bool silence = g_media_alert_silence;
  bool asleep = g_media_sleep;
  bool busy = g_media_job.busy && !g_media_job.done;
  bool chime = !strcmp(g_media_track, NY_MEDIA_CHIME);
  g_media_alert_silence = false;
  g_media_sleep = false;
  nxmutex_unlock(&g_media_lock);
  if (busy)
    {
      /* Keep the requests for the next tick rather than dropping them. */

      nxmutex_lock(&g_media_lock);
      g_media_alert_silence |= silence;
      g_media_sleep |= asleep;
      nxmutex_unlock(&g_media_lock);
      return;
    }
  if (!g_media_player)
    g_media_player = nxplayer_create();
  if (!g_media_player)
    return;
  if (state != NXPLAYER_STATE_IDLE)
    {
      /* A sleep timer ends music, never an alert; silencing ends an alert,
       * never music; and a wanted alert takes the speaker from music.
       */

      if (chime ? (silence && alert == NY_PRODUCT_MEDIA_ALERT_OFF)
                : (asleep || alert != NY_PRODUCT_MEDIA_ALERT_OFF))
        ny_media_halt();
      return;
    }
  if (alert == NY_PRODUCT_MEDIA_ALERT_OFF || now < g_media_alert_next)
    return;
#ifdef CONFIG_NYABULA_CORE_BT
  /* A call keeps the speaker.  The alert is not spent on a start that
   * cannot work: it stays wanted and sounds when the call is over.
   */

  if (ny_product_bt_call_active())
    return;
#endif
  int ret = ny_media_start(NY_MEDIA_CHIME);
  if (ret == -ENOENT || ret == -ENOTDIR)
    {
      /* FAT answers ENOTDIR, not ENOENT, while the media directory itself
       * is still missing.
       */

      ret = ny_media_chime_create();
      if (ret == 0)
        ret = ny_media_start(NY_MEDIA_CHIME);
    }
  nxmutex_lock(&g_media_lock);
  if (ret < 0)
    {
      /* Without a usable speaker an alert must not spin on the device. */

      g_media_error = ret;
      g_media_alert_next = now + 10000;
    }
  else
    g_media_alert_next = now + g_media_duration + NY_MEDIA_CHIME_GAP_MS;
  if (g_media_alert == NY_PRODUCT_MEDIA_ALERT_ONCE)
    g_media_alert = NY_PRODUCT_MEDIA_ALERT_OFF;
  nxmutex_unlock(&g_media_lock);
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
    return state != NXPLAYER_STATE_IDLE ? -EBUSY : ny_media_start(job->name);
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

  /* The claim ny_media_start took lasts as long as the track does: the
   * microphone comes back when the player falls idle, not a tick earlier,
   * or the codec would change format under a track that is still playing.
   */

  if (state == NXPLAYER_STATE_IDLE)
    {
      ny_media_speaker_release();
    }

  ny_media_alert_tick(state, now);
#ifdef CONFIG_NYABULA_CORE_BT
  if (state == NXPLAYER_STATE_IDLE && !execute)
    {
      nxmutex_lock(&g_media_lock);
      bool wanted = g_media_alert != NY_PRODUCT_MEDIA_ALERT_OFF;
      nxmutex_unlock(&g_media_lock);
      if (!wanted && (!g_media_player || nxplayer_getstate(g_media_player) ==
                                             NXPLAYER_STATE_IDLE))
        ny_product_bt_speaker_release();
    }
#endif
  return ret;
#else
  return 0;
#endif
}

/****************************************************************************
 * Name: ny_product_media_alert
 ****************************************************************************/

void ny_product_media_alert(int mode)
{
#ifdef CONFIG_NYABULA_CORE_MEDIA
  nxmutex_lock(&g_media_lock);
  if (mode == NY_PRODUCT_MEDIA_ALERT_OFF)
    g_media_alert_silence = true;
  else
    g_media_alert_next = 0;

  /* A single chime must not end an alarm that is still ringing. */

  if (mode != NY_PRODUCT_MEDIA_ALERT_ONCE ||
      g_media_alert != NY_PRODUCT_MEDIA_ALERT_LOOP)
    g_media_alert = mode;
  nxmutex_unlock(&g_media_lock);
#else
  (void)mode;
#endif
}

/****************************************************************************
 * Name: ny_product_media_sleep
 ****************************************************************************/

void ny_product_media_sleep(void)
{
#ifdef CONFIG_NYABULA_CORE_MEDIA
  nxmutex_lock(&g_media_lock);
  g_media_sleep = true;
  nxmutex_unlock(&g_media_lock);
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
