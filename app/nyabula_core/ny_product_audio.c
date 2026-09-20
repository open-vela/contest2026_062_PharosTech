/****************************************************************************
 * app/nyabula_core/ny_product_audio.c
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

/* The ES8388 codec as the panel and the agent see it:
 *
 *   audio.status        any role  routes, levels and the jack, as they are
 *   audio.output.route  family    {"route": auto|headphones|speaker|both|off}
 *   audio.volume        family    {"volume": 0..100, "muted": bool}, either
 *   audio.input.route   family    {"route": main|headset|both|off}
 *   audio.mic.gain      family    {"gain": 0..100}
 *   audio.mic.mute      family    {"muted": bool}
 *   audio.channel       family    {"mono","swap","invertLeft","invertRight"}
 *
 * Every setter answers with the status that results from it.
 *
 * Routes and channel processing are read back from the driver on every
 * status, so a change made by audioctl shows up here.  Volume, mute and
 * microphone gain cannot be read back: the driver keeps them to itself.
 * This file is therefore their only record, and every writer (the topics
 * above, music.volume, audioctl) goes through ny_product_audio_set_levels().
 *
 * Levels are written on a short-lived descriptor of their own, not on the
 * player's.  The audio upper half forwards a feature-unit configure without
 * touching the stream state, which is what lets the volume move whether or
 * not something is playing.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_product_audio.h"
#include "ny_product.h"
#include "ny_product_store.h"

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8388.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_AUDIO_DOMAIN "audio"
#define NY_AUDIO_SCHEMA 1

/* AUDIO_FU_VOLUME runs 0..1000 and the panel 0..100. */

#define NY_AUDIO_VOLUME_SCALE 10

/* The driver takes the microphone gain in dB and only has 3 dB steps. */

#define NY_AUDIO_GAIN_DB_MAX  24
#define NY_AUDIO_GAIN_DB_STEP 3

/* What the driver programs at start-up, so that a device which never had a
 * preference stored reports what it actually plays at.
 */

#define NY_AUDIO_DEFAULT_VOLUME \
  (CONFIG_ES8388_OUTPUT_INITVOLUME / NY_AUDIO_VOLUME_SCALE)
#define NY_AUDIO_DEFAULT_GAIN \
  (CONFIG_ES8388_INPUT_INITVOLUME / NY_AUDIO_VOLUME_SCALE)

/* The codec nodes are registered by board initialization, which may finish
 * after the product worker starts: ask often at first, then rarely.
 */

#define NY_AUDIO_RESTORE_FAST_MS 2000
#define NY_AUDIO_RESTORE_SLOW_MS 10000
#define NY_AUDIO_RESTORE_FAST    15
#define NY_AUDIO_STORE_RETRY_MS  5000

/* Set before the store was read: the stored record must not undo these. */

#define NY_AUDIO_EARLY_VOLUME (1 << 0)
#define NY_AUDIO_EARLY_MUTED  (1 << 1)
#define NY_AUDIO_EARLY_GAIN   (1 << 2)
#define NY_AUDIO_EARLY_OUTPUT (1 << 3)
#define NY_AUDIO_EARLY_INPUT  (1 << 4)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_audio_prefs_s
{
  int volume;
  bool muted;
  int gain;

  /* A direction group is only restored at start-up once the owner chose
   * something in it.  Until then the board's own default route stands.
   */

  bool output_set;
  enum es8388_output_route_e route;
  bool mono;
  bool swap;
  bool invert_left;
  bool invert_right;

  bool input_set;
  enum es8388_input_route_e input_route;
  bool mic_muted;
};

struct ny_audio_state_s
{
  struct ny_audio_prefs_s prefs;
  bool loaded;            /* The store was read */
  bool restored;          /* The preferences reached the codec */
  bool dirty;             /* The preferences differ from the store */
  bool saved;             /* The last write to the store succeeded */
  uint8_t early;          /* NY_AUDIO_EARLY_* */
  unsigned int attempts;  /* Failed restores so far */
  uint64_t restore_after; /* Monotonic ms before which not to restore */
  uint64_t store_after;   /* Monotonic ms before which not to use the store */
  uint64_t revision;      /* Store revision of the record */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static const char *ny_audio_output_name(enum es8388_output_route_e route);
static const char *ny_audio_input_name(enum es8388_input_route_e route);
static int ny_audio_name_index(const char *const *names, size_t count,
                               const cJSON *item);
static bool ny_audio_percent(const cJSON *item, int *value);
static int ny_audio_flag(const cJSON *data, const char *key, bool *value);
static int ny_audio_gain_db(int gain);
static int ny_audio_open(const char *path);
static int ny_audio_feature(int fd, uint16_t unit, uint16_t value);
static int ny_audio_push_volume(int fd, const struct ny_audio_prefs_s *prefs,
                                bool mute_unit);
static int ny_audio_push_output(int fd, const struct ny_audio_prefs_s *prefs);
static int ny_audio_push_input(int fd, const struct ny_audio_prefs_s *prefs);
static void ny_audio_decode(const cJSON *root);
static void ny_audio_load(void);
static void ny_audio_save(void);
static int ny_audio_restore(void);
static void ny_audio_ensure(void);
static int ny_audio_levels_apply(int volume, int muted, int gain);
static int ny_audio_output_apply(const cJSON *data, bool channel);
static int ny_audio_input_apply(const cJSON *data, bool route);
static cJSON *ny_audio_status(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *const g_audio_output_names[] = {
  [ES8388_OUTPUT_ROUTE_NONE] = "off",
  [ES8388_OUTPUT_ROUTE_LINE1] = "headphones",
  [ES8388_OUTPUT_ROUTE_LINE2] = "speaker",
  [ES8388_OUTPUT_ROUTE_BOTH] = "both",
  [ES8388_OUTPUT_ROUTE_AUTO] = "auto",
};

static const char *const g_audio_input_names[] = {
  [ES8388_INPUT_ROUTE_NONE] = "off",
  [ES8388_INPUT_ROUTE_LINE1] = "headset",
  [ES8388_INPUT_ROUTE_LINE2] = "main",
  [ES8388_INPUT_ROUTE_BOTH] = "both",
};

#define NY_AUDIO_OUTPUT_NAMES \
  (sizeof(g_audio_output_names) / sizeof(g_audio_output_names[0]))
#define NY_AUDIO_INPUT_NAMES \
  (sizeof(g_audio_input_names) / sizeof(g_audio_input_names[0]))

static mutex_t g_audio_lock = NXMUTEX_INITIALIZER;

static struct ny_audio_state_s g_audio =
{
  .prefs =
  {
    .volume      = NY_AUDIO_DEFAULT_VOLUME,
    .gain        = NY_AUDIO_DEFAULT_GAIN,
    .route       = ES8388_OUTPUT_ROUTE_AUTO,
    .input_route = ES8388_INPUT_ROUTE_LINE2,
  },
  .saved = true,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_audio_output_name
 ****************************************************************************/

static const char *ny_audio_output_name(enum es8388_output_route_e route)
{
  return (size_t)route < NY_AUDIO_OUTPUT_NAMES ? g_audio_output_names[route]
                                               : "off";
}

/****************************************************************************
 * Name: ny_audio_input_name
 ****************************************************************************/

static const char *ny_audio_input_name(enum es8388_input_route_e route)
{
  return (size_t)route < NY_AUDIO_INPUT_NAMES ? g_audio_input_names[route]
                                              : "off";
}

/****************************************************************************
 * Name: ny_audio_name_index
 *
 * Description:
 *   The position of a JSON string in a table of route names, which is the
 *   driver's enumeration value for it, or -EINVAL.
 *
 ****************************************************************************/

static int ny_audio_name_index(const char *const *names, size_t count,
                               const cJSON *item)
{
  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      return -EINVAL;
    }

  for (size_t i = 0; i < count; i++)
    {
      if (strcmp(item->valuestring, names[i]) == 0)
        {
          return (int)i;
        }
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: ny_audio_percent
 ****************************************************************************/

static bool ny_audio_percent(const cJSON *item, int *value)
{
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
      item->valuedouble < 0 || item->valuedouble > 100 ||
      floor(item->valuedouble) != item->valuedouble)
    {
      return false;
    }

  *value = (int)item->valuedouble;
  return true;
}

/****************************************************************************
 * Name: ny_audio_flag
 *
 * Description:
 *   Read an optional boolean member.
 *
 * Returned Value:
 *   1 when present, 0 when absent, -EINVAL when it is not a boolean.
 *
 ****************************************************************************/

static int ny_audio_flag(const cJSON *data, const char *key, bool *value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);
  if (item == NULL)
    {
      return 0;
    }

  if (!cJSON_IsBool(item))
    {
      return -EINVAL;
    }

  *value = cJSON_IsTrue(item);
  return 1;
}

/****************************************************************************
 * Name: ny_audio_gain_db
 *
 * Description:
 *   0..100 onto the nine PGA steps the driver has (0, 3, ... 24 dB), to the
 *   nearest step: 0..6 is 0 dB, 50 is 12 dB, 94..100 is 24 dB.
 *
 ****************************************************************************/

static int ny_audio_gain_db(int gain)
{
  int steps = NY_AUDIO_GAIN_DB_MAX / NY_AUDIO_GAIN_DB_STEP;
  return (gain * steps + 50) / 100 * NY_AUDIO_GAIN_DB_STEP;
}

/****************************************************************************
 * Name: ny_audio_open
 ****************************************************************************/

static int ny_audio_open(const char *path)
{
  int fd = open(path, O_RDWR);
  if (fd < 0)
    {
      /* The web layer reports -ENOENT as an unknown topic, which the panel
       * takes for a firmware without this service.  A node that is not
       * registered yet is a device that is not there.
       */

      return errno == ENOENT ? -ENODEV : -errno;
    }

  return fd;
}

/****************************************************************************
 * Name: ny_audio_feature
 ****************************************************************************/

static int ny_audio_feature(int fd, uint16_t unit, uint16_t value)
{
  struct audio_caps_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len = sizeof(struct audio_caps_s);
  desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  desc.caps.ac_format.hw = unit;
  desc.caps.ac_controls.hw[0] = value;
  return ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&desc) < 0
             ? -errno
             : 0;
}

/****************************************************************************
 * Name: ny_audio_push_volume
 *
 * Description:
 *   Mute is carried by the volume, not by the mute unit alone: the driver
 *   uses the DAC mute as its own pop guard and releases it whenever a stream
 *   arms, which would silently end a mute set while idle.  The volume it
 *   remembers and re-applies.  The mute unit is still sent so that a mute
 *   during playback is immediate rather than a ramp.
 *
 ****************************************************************************/

static int ny_audio_push_volume(int fd, const struct ny_audio_prefs_s *prefs,
                                bool mute_unit)
{
  uint16_t level =
      prefs->muted ? 0 : (uint16_t)(prefs->volume * NY_AUDIO_VOLUME_SCALE);
  int ret = ny_audio_feature(fd, AUDIO_FU_VOLUME, level);
  if (ret == 0 && mute_unit)
    {
      ret = ny_audio_feature(fd, AUDIO_FU_MUTE, prefs->muted ? 1 : 0);
      if (ret == -ENOTTY)
        {
          /* Built without the mute unit: the volume already did it. */

          ret = 0;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: ny_audio_push_output
 *
 * Description:
 *   SET replaces the whole direction group, so start from what the driver
 *   holds.
 *
 ****************************************************************************/

static int ny_audio_push_output(int fd, const struct ny_audio_prefs_s *prefs)
{
  struct es8388_control_s control;

  memset(&control, 0, sizeof(control));
  if (ioctl(fd, ES8388IOC_GET_CONTROL, (unsigned long)(uintptr_t)&control) < 0)
    {
      return -errno;
    }

  control.mask = ES8388_CONTROL_OUTPUT;
  control.route = prefs->route;
  control.mono = prefs->mono;
  control.swap = prefs->swap;
  control.invert_left = prefs->invert_left;
  control.invert_right = prefs->invert_right;
  return ioctl(fd, ES8388IOC_SET_CONTROL, (unsigned long)(uintptr_t)&control) <
                 0
             ? -errno
             : 0;
}

/****************************************************************************
 * Name: ny_audio_push_input
 ****************************************************************************/

static int ny_audio_push_input(int fd, const struct ny_audio_prefs_s *prefs)
{
  struct es8388_control_s control;

  memset(&control, 0, sizeof(control));
  if (ioctl(fd, ES8388IOC_GET_CONTROL, (unsigned long)(uintptr_t)&control) < 0)
    {
      return -errno;
    }

  control.mask = ES8388_CONTROL_INPUT;
  control.input_route = prefs->input_route;
  control.microphone_muted = prefs->mic_muted;
  return ioctl(fd, ES8388IOC_SET_CONTROL, (unsigned long)(uintptr_t)&control) <
                 0
             ? -errno
             : 0;
}

/****************************************************************************
 * Name: ny_audio_decode
 *
 * Description:
 *   Take a stored record, whole or not at all: half a record would restore
 *   a combination nobody chose.
 *
 ****************************************************************************/

static void ny_audio_decode(const cJSON *root)
{
  struct ny_audio_prefs_s prefs = g_audio.prefs;
  const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
  const cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "outputRoute");
  const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "inputRoute");
  bool valid = cJSON_IsNumber(schema) && schema->valueint == NY_AUDIO_SCHEMA;
  int volume = 0;
  int gain = 0;
  bool muted = false;

  valid = valid &&
          ny_audio_percent(cJSON_GetObjectItemCaseSensitive(root, "volume"),
                           &volume) &&
          ny_audio_percent(cJSON_GetObjectItemCaseSensitive(root, "gain"),
                           &gain) &&
          ny_audio_flag(root, "muted", &muted) == 1;

  if (valid && output != NULL)
    {
      int route = ny_audio_name_index(g_audio_output_names,
                                      NY_AUDIO_OUTPUT_NAMES, output);
      valid = route >= 0 && ny_audio_flag(root, "mono", &prefs.mono) == 1 &&
              ny_audio_flag(root, "swap", &prefs.swap) == 1 &&
              ny_audio_flag(root, "invertLeft", &prefs.invert_left) == 1 &&
              ny_audio_flag(root, "invertRight", &prefs.invert_right) == 1;
      if (valid)
        {
          prefs.route = (enum es8388_output_route_e)route;
          prefs.output_set = true;
        }
    }

  if (valid && input != NULL)
    {
      int route = ny_audio_name_index(g_audio_input_names,
                                      NY_AUDIO_INPUT_NAMES, input);
      valid =
          route >= 0 && ny_audio_flag(root, "micMuted", &prefs.mic_muted) == 1;
      if (valid)
        {
          prefs.input_route = (enum es8388_input_route_e)route;
          prefs.input_set = true;
        }
    }

  if (!valid)
    {
      return;
    }

  /* Whatever was set before the store could be read is newer than it. */

  if ((g_audio.early & NY_AUDIO_EARLY_VOLUME) == 0)
    {
      g_audio.prefs.volume = volume;
    }

  if ((g_audio.early & NY_AUDIO_EARLY_MUTED) == 0)
    {
      g_audio.prefs.muted = muted;
    }

  if ((g_audio.early & NY_AUDIO_EARLY_GAIN) == 0)
    {
      g_audio.prefs.gain = gain;
    }

  if ((g_audio.early & NY_AUDIO_EARLY_OUTPUT) == 0)
    {
      g_audio.prefs.output_set = prefs.output_set;
      g_audio.prefs.route = prefs.route;
      g_audio.prefs.mono = prefs.mono;
      g_audio.prefs.swap = prefs.swap;
      g_audio.prefs.invert_left = prefs.invert_left;
      g_audio.prefs.invert_right = prefs.invert_right;
    }

  if ((g_audio.early & NY_AUDIO_EARLY_INPUT) == 0)
    {
      g_audio.prefs.input_set = prefs.input_set;
      g_audio.prefs.input_route = prefs.input_route;
      g_audio.prefs.mic_muted = prefs.mic_muted;
    }
}

/****************************************************************************
 * Name: ny_audio_load
 *
 * Description:
 *   Read the stored preferences once.  Called with g_audio_lock held, and
 *   only from callers with a product-sized stack: the store is SQLite.
 *
 ****************************************************************************/

static void ny_audio_load(void)
{
  uint64_t now = ny_product_time_ms(true);
  uint64_t revision = 0;
  cJSON *root = NULL;
  int ret;

  if (g_audio.loaded || now < g_audio.store_after)
    {
      return;
    }

  ret = ny_product_store_read(NY_AUDIO_DOMAIN, &root, &revision);
  if (ret < 0 && ret != -EBADMSG)
    {
      g_audio.store_after = now + NY_AUDIO_STORE_RETRY_MS;
      return;
    }

  /* A record that does not parse leaves the defaults in charge. */

  if (ret == 0 && root != NULL)
    {
      ny_audio_decode(root);
    }

  cJSON_Delete(root);
  g_audio.revision = revision;
  g_audio.loaded = true;
}

/****************************************************************************
 * Name: ny_audio_save
 *
 * Description:
 *   Write the preferences if they changed.  Called with g_audio_lock held.
 *   A failure leaves them dirty for the worker tick to try again.
 *
 ****************************************************************************/

static void ny_audio_save(void)
{
  const struct ny_audio_prefs_s *prefs = &g_audio.prefs;
  uint64_t now = ny_product_time_ms(true);
  uint64_t revision = 0;
  cJSON *root;
  bool valid;
  int ret;

  if (!g_audio.dirty || !g_audio.loaded || now < g_audio.store_after)
    {
      return;
    }

  root = cJSON_CreateObject();
  valid = root != NULL &&
          cJSON_AddNumberToObject(root, "schema", NY_AUDIO_SCHEMA) != NULL &&
          cJSON_AddNumberToObject(root, "volume", prefs->volume) != NULL &&
          cJSON_AddBoolToObject(root, "muted", prefs->muted) != NULL &&
          cJSON_AddNumberToObject(root, "gain", prefs->gain) != NULL;
  if (valid && prefs->output_set)
    {
      const char *name = ny_audio_output_name(prefs->route);
      valid = cJSON_AddStringToObject(root, "outputRoute", name) != NULL &&
              cJSON_AddBoolToObject(root, "mono", prefs->mono) != NULL &&
              cJSON_AddBoolToObject(root, "swap", prefs->swap) != NULL &&
              cJSON_AddBoolToObject(root, "invertLeft", prefs->invert_left) !=
                  NULL &&
              cJSON_AddBoolToObject(root, "invertRight",
                                    prefs->invert_right) != NULL;
    }

  if (valid && prefs->input_set)
    {
      valid =
          cJSON_AddStringToObject(root, "inputRoute",
                                  ny_audio_input_name(prefs->input_route)) !=
              NULL &&
          cJSON_AddBoolToObject(root, "micMuted", prefs->mic_muted) != NULL;
    }

  ret = valid ? ny_product_store_write(NY_AUDIO_DOMAIN, root, g_audio.revision,
                                       &revision)
              : -ENOMEM;
  if (ret == -ESTALE)
    {
      /* Someone else wrote the record.  These preferences are what the
       * codec is playing with, so they win; only the revision was wrong.
       */

      cJSON *other = NULL;
      ret = ny_product_store_read(NY_AUDIO_DOMAIN, &other, &g_audio.revision);
      cJSON_Delete(other);
      if (ret == 0)
        {
          ret = ny_product_store_write(NY_AUDIO_DOMAIN, root, g_audio.revision,
                                       &revision);
        }
    }

  cJSON_Delete(root);
  g_audio.saved = ret == 0;
  if (ret == 0)
    {
      g_audio.revision = revision;
      g_audio.dirty = false;
    }
  else
    {
      g_audio.store_after = now + NY_AUDIO_STORE_RETRY_MS;
    }
}

/****************************************************************************
 * Name: ny_audio_restore
 *
 * Description:
 *   Give the codec the stored preferences.  Called with g_audio_lock held.
 *
 ****************************************************************************/

static int ny_audio_restore(void)
{
  const struct ny_audio_prefs_s *prefs = &g_audio.prefs;
  int ret;
  int fd;

  fd = ny_audio_open(NY_PRODUCT_AUDIO_OUTPUT);
  if (fd < 0)
    {
      return fd;
    }

  /* Releasing the DAC mute of an idle codec is the driver's business, not
   * ours: only send the mute unit when there is a mute to restore.
   */

  ret = ny_audio_push_volume(fd, prefs, prefs->muted);
  if (ret == 0 && prefs->output_set)
    {
      ret = ny_audio_push_output(fd, prefs);
    }

  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  fd = ny_audio_open(NY_PRODUCT_AUDIO_INPUT);
  if (fd < 0)
    {
      return fd;
    }

  ret = ny_audio_feature(fd, AUDIO_FU_INP_GAIN,
                         (uint16_t)ny_audio_gain_db(prefs->gain));
  if (ret == 0 && prefs->input_set)
    {
      ret = ny_audio_push_input(fd, prefs);
    }

  close(fd);
  return ret;
}

/****************************************************************************
 * Name: ny_audio_ensure
 *
 * Description:
 *   Load, then restore once.  Called with g_audio_lock held from the worker
 *   tick and ahead of every request, so that a request arriving before the
 *   first successful restore changes the restored state and not a default.
 *
 ****************************************************************************/

static void ny_audio_ensure(void)
{
  uint64_t now;

  ny_audio_load();
  now = ny_product_time_ms(true);
  if (!g_audio.loaded || g_audio.restored || now < g_audio.restore_after)
    {
      return;
    }

  if (ny_audio_restore() == 0)
    {
      g_audio.restored = true;
      return;
    }

  if (g_audio.attempts < NY_AUDIO_RESTORE_FAST)
    {
      g_audio.attempts++;
    }

  g_audio.restore_after = now + (g_audio.attempts < NY_AUDIO_RESTORE_FAST
                                     ? NY_AUDIO_RESTORE_FAST_MS
                                     : NY_AUDIO_RESTORE_SLOW_MS);
}

/****************************************************************************
 * Name: ny_audio_levels_apply
 *
 * Description:
 *   Called with g_audio_lock held.  Small-stack safe: no store access.
 *
 ****************************************************************************/

static int ny_audio_levels_apply(int volume, int muted, int gain)
{
  struct ny_audio_prefs_s next = g_audio.prefs;
  bool output =
      volume != NY_PRODUCT_AUDIO_KEEP || muted != NY_PRODUCT_AUDIO_KEEP;
  int ret = 0;
  int fd;

  if (volume > 100 || gain > 100 || muted > 1 ||
      volume < NY_PRODUCT_AUDIO_KEEP || gain < NY_PRODUCT_AUDIO_KEEP ||
      muted < NY_PRODUCT_AUDIO_KEEP ||
      (!output && gain == NY_PRODUCT_AUDIO_KEEP))
    {
      return -EINVAL;
    }

  if (volume != NY_PRODUCT_AUDIO_KEEP)
    {
      next.volume = volume;
    }

  if (muted != NY_PRODUCT_AUDIO_KEEP)
    {
      next.muted = muted != 0;
    }

  if (gain != NY_PRODUCT_AUDIO_KEEP)
    {
      next.gain = gain;
    }

  if (output)
    {
      fd = ny_audio_open(NY_PRODUCT_AUDIO_OUTPUT);
      if (fd < 0)
        {
          return fd;
        }

      ret = ny_audio_push_volume(fd, &next, true);
      close(fd);
    }

  if (ret == 0 && gain != NY_PRODUCT_AUDIO_KEEP)
    {
      fd = ny_audio_open(NY_PRODUCT_AUDIO_INPUT);
      if (fd < 0)
        {
          return fd;
        }

      ret = ny_audio_feature(fd, AUDIO_FU_INP_GAIN,
                             (uint16_t)ny_audio_gain_db(next.gain));
      close(fd);
    }

  if (ret < 0)
    {
      return ret;
    }

  g_audio.prefs = next;
  g_audio.dirty = true;
  g_audio.saved = false;
  if (!g_audio.loaded)
    {
      g_audio.early |=
          (volume != NY_PRODUCT_AUDIO_KEEP ? NY_AUDIO_EARLY_VOLUME : 0) |
          (muted != NY_PRODUCT_AUDIO_KEEP ? NY_AUDIO_EARLY_MUTED : 0) |
          (gain != NY_PRODUCT_AUDIO_KEEP ? NY_AUDIO_EARLY_GAIN : 0);
    }

  return 0;
}

/****************************************************************************
 * Name: ny_audio_output_apply
 *
 * Description:
 *   audio.output.route (channel false) or audio.channel (channel true).
 *   Called with g_audio_lock held.
 *
 ****************************************************************************/

static int ny_audio_output_apply(const cJSON *data, bool channel)
{
  struct ny_audio_prefs_s next = g_audio.prefs;
  struct es8388_control_s control;
  int ret;
  int fd;

  fd = ny_audio_open(NY_PRODUCT_AUDIO_OUTPUT);
  if (fd < 0)
    {
      return fd;
    }

  /* Start from the live group rather than from the preferences: audioctl
   * may have changed it since, and SET would silently undo that.
   */

  memset(&control, 0, sizeof(control));
  if (ioctl(fd, ES8388IOC_GET_CONTROL, (unsigned long)(uintptr_t)&control) < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  next.route = control.route;
  next.mono = control.mono;
  next.swap = control.swap;
  next.invert_left = control.invert_left;
  next.invert_right = control.invert_right;

  if (channel)
    {
      int mono = ny_audio_flag(data, "mono", &next.mono);
      int swap = ny_audio_flag(data, "swap", &next.swap);
      int left = ny_audio_flag(data, "invertLeft", &next.invert_left);
      int right = ny_audio_flag(data, "invertRight", &next.invert_right);
      ret = mono < 0 || swap < 0 || left < 0 || right < 0 ||
                    mono + swap + left + right == 0
                ? -EINVAL
                : 0;
    }
  else
    {
      ret =
          ny_audio_name_index(g_audio_output_names, NY_AUDIO_OUTPUT_NAMES,
                              cJSON_GetObjectItemCaseSensitive(data, "route"));
      if (ret >= 0)
        {
          next.route = (enum es8388_output_route_e)ret;
          ret = 0;
        }
    }

  if (ret == 0)
    {
      ret = ny_audio_push_output(fd, &next);
    }

  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  next.output_set = true;
  g_audio.prefs = next;
  g_audio.dirty = true;
  g_audio.saved = false;
  if (!g_audio.loaded)
    {
      g_audio.early |= NY_AUDIO_EARLY_OUTPUT;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_audio_input_apply
 *
 * Description:
 *   audio.input.route (route true) or audio.mic.mute (route false).
 *   Called with g_audio_lock held.
 *
 ****************************************************************************/

static int ny_audio_input_apply(const cJSON *data, bool route)
{
  struct ny_audio_prefs_s next = g_audio.prefs;
  struct es8388_control_s control;
  int ret;
  int fd;

  fd = ny_audio_open(NY_PRODUCT_AUDIO_INPUT);
  if (fd < 0)
    {
      return fd;
    }

  memset(&control, 0, sizeof(control));
  if (ioctl(fd, ES8388IOC_GET_CONTROL, (unsigned long)(uintptr_t)&control) < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  next.input_route = control.input_route;
  next.mic_muted = control.microphone_muted;

  if (route)
    {
      ret =
          ny_audio_name_index(g_audio_input_names, NY_AUDIO_INPUT_NAMES,
                              cJSON_GetObjectItemCaseSensitive(data, "route"));
      if (ret >= 0)
        {
          next.input_route = (enum es8388_input_route_e)ret;
          ret = 0;
        }
    }
  else
    {
      ret = ny_audio_flag(data, "muted", &next.mic_muted) == 1 ? 0 : -EINVAL;
    }

  if (ret == 0)
    {
      ret = ny_audio_push_input(fd, &next);
    }

  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  next.input_set = true;
  g_audio.prefs = next;
  g_audio.dirty = true;
  g_audio.saved = false;
  if (!g_audio.loaded)
    {
      g_audio.early |= NY_AUDIO_EARLY_INPUT;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_audio_status
 *
 * Description:
 *   Routes, channel processing and the jack come from the driver; the
 *   preferences only stand in for a direction whose node is missing.
 *   Called with g_audio_lock held.
 *
 ****************************************************************************/

static cJSON *ny_audio_status(void)
{
  const struct ny_audio_prefs_s *prefs = &g_audio.prefs;
  struct es8388_control_s out;
  struct es8388_control_s in;
  bool out_live = false;
  bool in_live = false;
  cJSON *root;
  cJSON *output;
  cJSON *input;
  bool valid;
  int fd;

  memset(&out, 0, sizeof(out));
  memset(&in, 0, sizeof(in));

  fd = ny_audio_open(NY_PRODUCT_AUDIO_OUTPUT);
  if (fd >= 0)
    {
      out_live = ioctl(fd, ES8388IOC_GET_CONTROL,
                       (unsigned long)(uintptr_t)&out) >= 0;
      close(fd);
    }

  fd = ny_audio_open(NY_PRODUCT_AUDIO_INPUT);
  if (fd >= 0)
    {
      in_live =
          ioctl(fd, ES8388IOC_GET_CONTROL, (unsigned long)(uintptr_t)&in) >= 0;
      close(fd);
    }

  if (!out_live)
    {
      out.route = prefs->route;
      out.active_route = ES8388_OUTPUT_ROUTE_NONE;
      out.mono = prefs->mono;
      out.swap = prefs->swap;
      out.invert_left = prefs->invert_left;
      out.invert_right = prefs->invert_right;
    }

  if (!in_live)
    {
      in.input_route = prefs->input_route;
      in.microphone_muted = prefs->mic_muted;
    }

  root = cJSON_CreateObject();
  output = root != NULL ? cJSON_AddObjectToObject(root, "output") : NULL;
  input = root != NULL ? cJSON_AddObjectToObject(root, "input") : NULL;
  valid =
      output != NULL && input != NULL &&
      cJSON_AddBoolToObject(root, "available", out_live) != NULL &&
      cJSON_AddBoolToObject(root, "inputAvailable", in_live) != NULL &&
      cJSON_AddBoolToObject(root, "restored", g_audio.restored) != NULL &&
      cJSON_AddBoolToObject(root, "settingsSaved", g_audio.saved) != NULL &&
      cJSON_AddNumberToObject(root, "revision", (double)g_audio.revision) !=
          NULL &&
      cJSON_AddStringToObject(output, "route",
                              ny_audio_output_name(out.route)) != NULL &&
      cJSON_AddStringToObject(output, "activeRoute",
                              ny_audio_output_name(out.active_route)) !=
          NULL &&
      cJSON_AddBoolToObject(output, "headphones", out.headphones_connected) !=
          NULL &&
      cJSON_AddNumberToObject(output, "volume", prefs->volume) != NULL &&
      cJSON_AddBoolToObject(output, "muted", prefs->muted) != NULL &&
      cJSON_AddBoolToObject(output, "mono", out.mono) != NULL &&
      cJSON_AddBoolToObject(output, "swap", out.swap) != NULL &&
      cJSON_AddBoolToObject(output, "invertLeft", out.invert_left) != NULL &&
      cJSON_AddBoolToObject(output, "invertRight", out.invert_right) != NULL &&
      cJSON_AddStringToObject(input, "route",
                              ny_audio_input_name(in.input_route)) != NULL &&
      cJSON_AddBoolToObject(input, "muted", in.microphone_muted) != NULL &&
      cJSON_AddNumberToObject(input, "gain", prefs->gain) != NULL &&
      cJSON_AddNumberToObject(input, "gainDb",
                              ny_audio_gain_db(prefs->gain)) != NULL;
  if (!valid)
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_product_audio_levels
 ****************************************************************************/

int ny_product_audio_levels(struct ny_product_audio_levels_s *levels)
{
  int ret;

  if (levels == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return ret;
    }

  levels->volume = g_audio.prefs.volume;
  levels->muted = g_audio.prefs.muted;
  levels->gain = g_audio.prefs.gain;
  levels->gain_db = ny_audio_gain_db(g_audio.prefs.gain);
  nxmutex_unlock(&g_audio_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_product_audio_set_levels
 ****************************************************************************/

int ny_product_audio_set_levels(int volume, int muted, int gain)
{
  int ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_audio_levels_apply(volume, muted, gain);
  nxmutex_unlock(&g_audio_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_audio_request
 ****************************************************************************/

int ny_product_audio_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result)
{
  bool status = strcmp(topic, "audio.status") == 0;
  int ret;

  if (!status && strcmp(topic, "audio.output.route") != 0 &&
      strcmp(topic, "audio.volume") != 0 &&
      strcmp(topic, "audio.input.route") != 0 &&
      strcmp(topic, "audio.mic.gain") != 0 &&
      strcmp(topic, "audio.mic.mute") != 0 &&
      strcmp(topic, "audio.channel") != 0)
    {
      return -ENOSYS;
    }

  if (!status && caller->role < NY_PRODUCT_FAMILY)
    {
      return -EACCES;
    }

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return ret;
    }

  ny_audio_ensure();

  if (strcmp(topic, "audio.output.route") == 0)
    {
      ret = ny_audio_output_apply(data, false);
    }
  else if (strcmp(topic, "audio.channel") == 0)
    {
      ret = ny_audio_output_apply(data, true);
    }
  else if (strcmp(topic, "audio.input.route") == 0)
    {
      ret = ny_audio_input_apply(data, true);
    }
  else if (strcmp(topic, "audio.mic.mute") == 0)
    {
      ret = ny_audio_input_apply(data, false);
    }
  else if (strcmp(topic, "audio.mic.gain") == 0)
    {
      int gain = 0;
      ret = ny_audio_percent(cJSON_GetObjectItemCaseSensitive(data, "gain"),
                             &gain)
                ? ny_audio_levels_apply(NY_PRODUCT_AUDIO_KEEP,
                                        NY_PRODUCT_AUDIO_KEEP, gain)
                : -EINVAL;
    }
  else if (strcmp(topic, "audio.volume") == 0)
    {
      const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, "volume");
      int volume = NY_PRODUCT_AUDIO_KEEP;
      bool muted = false;
      int present = ny_audio_flag(data, "muted", &muted);
      bool invalid = present < 0 || (item == NULL && present == 0) ||
                     (item != NULL && !ny_audio_percent(item, &volume));
      ret = invalid
                ? -EINVAL
                : ny_audio_levels_apply(volume,
                                        present == 1 ? (muted ? 1 : 0)
                                                     : NY_PRODUCT_AUDIO_KEEP,
                                        NY_PRODUCT_AUDIO_KEEP);
    }

  if (ret == 0)
    {
      ny_audio_save();
      *result = ny_audio_status();
      ret = *result != NULL ? 0 : -ENOMEM;
    }

  nxmutex_unlock(&g_audio_lock);

  /* -ENOSYS would hand a topic that is ours to the next product module. */

  return ret == -ENOSYS ? -ENODEV : ret;
}

/****************************************************************************
 * Name: ny_product_audio_tick
 *
 * Description:
 *   Called by the product worker.  A codec that is not there yet is not an
 *   error of the worker, so nothing is reported for it.
 *
 ****************************************************************************/

int ny_product_audio_tick(void)
{
  if (nxmutex_lock(&g_audio_lock) < 0)
    {
      return 0;
    }

  ny_audio_ensure();
  ny_audio_save();
  nxmutex_unlock(&g_audio_lock);
  return 0;
}
