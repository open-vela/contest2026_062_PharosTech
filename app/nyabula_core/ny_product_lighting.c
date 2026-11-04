/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_lighting.c
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

#include "ny_product_lighting.h"
#include "ny_lighting_effects.h"
#include "ny_product_store.h"
#ifdef CONFIG_NYABULA_CORE_DSP
#include "ny_product_dsp.h"
#endif
#ifdef CONFIG_NYABULA_CORE_VOICE
#include "ny_voice.h"
#endif
#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NY_LIGHTING_DEVICE    "/dev/ws2812"
#define NY_LIGHTING_DOMAIN    "lighting"
#define NY_LIGHTING_PERIOD_US 50000
#define NY_LIGHTING_STACK     8192
#define NY_LIGHTING_CHANGE_MS 220

enum ny_lighting_phase_e
{
  NY_LIGHTING_BOOT_WAIT,
  NY_LIGHTING_BOOT,
  NY_LIGHTING_READY,
  NY_LIGHTING_FADING_SLEEP,
  NY_LIGHTING_ASLEEP,
  NY_LIGHTING_WAKING
};

/* Requests serialize persistence; only the worker owns the device file. */
static mutex_t g_lighting_request_lock = NXMUTEX_INITIALIZER;
static mutex_t g_lighting_lock = NXMUTEX_INITIALIZER;
static pthread_t g_lighting_thread;
static bool g_lighting_joinable;
static bool g_lighting_running;
static bool g_lighting_stopping;
static bool g_lighting_loaded;
static bool g_lighting_available;
static bool g_lighting_boot_seen;
static bool g_lighting_force_black;
static bool g_lighting_sleep_input;
static int g_lighting_error;
static uint64_t g_lighting_store_revision;
static uint64_t g_lighting_revision;
static uint64_t g_lighting_applied;
static uint64_t g_lighting_frames;
static uint64_t g_lighting_phase_since;
static uint64_t g_lighting_changed;
static uint64_t g_lighting_last_write;
static enum ny_lighting_phase_e g_lighting_phase;
static struct ny_lighting_group_s g_lighting_groups[NY_LIGHTING_GROUPS];
static struct ny_lighting_input_s g_lighting_input;
static uint32_t g_lighting_output[NY_LIGHTING_PIXELS];
static uint32_t g_lighting_from[NY_LIGHTING_PIXELS];
static char g_lighting_preset[16] = "custom";
static char g_lighting_source[24];
static float g_lighting_envelope;

static const char *ny_lighting_text(const cJSON *root, const char *key);
static int ny_lighting_lookup(const char *name, const char *const *names,
                              size_t count);
static int ny_lighting_integer(const cJSON *root, const char *key, int low,
                               int high, int *out);
static int ny_lighting_parse_group(const cJSON *data, unsigned int index,
                                   struct ny_lighting_group_s *group,
                                   bool stored);
static cJSON *
ny_lighting_config_json(const struct ny_lighting_group_s *groups);
static int ny_lighting_load(void);
static cJSON *ny_lighting_status(void);
static int ny_lighting_preset(const char *name,
                              struct ny_lighting_group_s *groups);
static void ny_lighting_inputs(struct ny_lighting_input_s *input, bool *asleep,
                               bool *eye_ready, char *source, size_t size);
static void ny_lighting_update(uint64_t now,
                               const struct ny_lighting_input_s *input,
                               bool asleep, bool eye_ready, uint32_t *pixels);
static void *ny_lighting_worker(void *arg);

/****************************************************************************
 * Name: ny_lighting_text
 ****************************************************************************/
static const char *ny_lighting_text(const cJSON *root, const char *key)
{
  const char *text =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, key));
  return text ? text : "";
}

/****************************************************************************
 * Name: ny_lighting_lookup
 ****************************************************************************/
static int ny_lighting_lookup(const char *name, const char *const *names,
                              size_t count)
{
  for (size_t i = 0; i < count; i++)
    if (!strcmp(name, names[i]))
      return (int)i;
  return -EINVAL;
}

/****************************************************************************
 * Name: ny_lighting_integer
 ****************************************************************************/
static int ny_lighting_integer(const cJSON *root, const char *key, int low,
                               int high, int *out)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!value)
    return 0;
  if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
      value->valuedouble < low || value->valuedouble > high ||
      floor(value->valuedouble) != value->valuedouble)
    return -ERANGE;
  *out = (int)value->valuedouble;
  return 1;
}

/****************************************************************************
 * Name: ny_lighting_parse_group
 ****************************************************************************/
static int ny_lighting_parse_group(const cJSON *data, unsigned int index,
                                   struct ny_lighting_group_s *group,
                                   bool stored)
{
  int value = group->brightness;
  int ret = ny_lighting_integer(data, "brightness", 0,
                                NY_LIGHTING_MAX_BRIGHTNESS, &value);
  if (ret < 0)
    return ret;
  group->brightness = value;
  if (cJSON_GetObjectItemCaseSensitive(data, "brightnessDelta"))
    {
      if (stored || cJSON_GetObjectItemCaseSensitive(data, "brightness"))
        return -EINVAL;
      ret = ny_lighting_integer(data, "brightnessDelta",
                                -NY_LIGHTING_MAX_BRIGHTNESS,
                                NY_LIGHTING_MAX_BRIGHTNESS, &value);
      if (ret < 0)
        return ret;
      value += group->brightness;
      group->brightness = value < 0 ? 0
                          : value > NY_LIGHTING_MAX_BRIGHTNESS
                              ? NY_LIGHTING_MAX_BRIGHTNESS
                              : value;
    }
  value = group->speed;
  ret = ny_lighting_integer(data, "speed", 1, 3, &value);
  if (ret < 0)
    return ret;
  group->speed = value;
  const cJSON *color = cJSON_GetObjectItemCaseSensitive(data, "color");
  if (color)
    {
      const char *s = cJSON_GetStringValue(color);
      if (!s || strlen(s) != 7 || s[0] != '#' ||
          strspn(s + 1, "0123456789abcdefABCDEF") != 6)
        return -EINVAL;
      group->color = strtoul(s + 1, NULL, 16);
      if (!stored)
        group->color_mode = NY_LIGHTING_FIXED;
    }
  const cJSON *mode = cJSON_GetObjectItemCaseSensitive(data, "colorMode");
  if (mode)
    {
      value =
          ny_lighting_lookup(ny_lighting_text(data, "colorMode"),
                             g_ny_lighting_colors, NY_LIGHTING_COLOR_COUNT);
      if (value < 0 || (index >= 2 && value != NY_LIGHTING_FIXED))
        return -EINVAL;
      group->color_mode = value;
    }
  const cJSON *effect = cJSON_GetObjectItemCaseSensitive(data, "effect");
  if (effect)
    {
      value =
          ny_lighting_lookup(ny_lighting_text(data, "effect"),
                             g_ny_lighting_effects, NY_LIGHTING_EFFECT_COUNT);
      if (value < 0 || !ny_lighting_effect_allowed(index, value))
        return -EINVAL;
      group->effect = value;
    }
  const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
  if (enabled && !cJSON_IsBool(enabled))
    return -EINVAL;
  if (enabled && !stored)
    group->enabled = cJSON_IsTrue(enabled);
  if (stored)
    group->enabled = false;
  return 0;
}

/****************************************************************************
 * Name: ny_lighting_config_json
 ****************************************************************************/
static cJSON *ny_lighting_config_json(const struct ny_lighting_group_s *groups)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *items = root ? cJSON_AddArrayToObject(root, "groups") : NULL;
  bool ok = items != NULL;
  for (unsigned int i = 0; i < NY_LIGHTING_GROUPS && ok; i++)
    {
      cJSON *row = cJSON_CreateObject();
      char color[8];
      snprintf(color, sizeof(color), "#%06lx",
               (unsigned long)(groups[i].color & 0xffffff));
      ok = row && cJSON_AddStringToObject(row, "id", g_ny_lighting_ids[i]) &&
           cJSON_AddStringToObject(row, "color", color) &&
           cJSON_AddNumberToObject(row, "brightness", groups[i].brightness) &&
           cJSON_AddNumberToObject(row, "speed", groups[i].speed) &&
           cJSON_AddStringToObject(row, "effect",
                                   g_ny_lighting_effects[groups[i].effect]) &&
           cJSON_AddStringToObject(row, "colorMode",
                                   g_ny_lighting_colors[groups[i].color_mode]);
      if (!ok || !cJSON_AddItemToArray(items, row))
        {
          cJSON_Delete(row);
          ok = false;
        }
    }
  if (!ok)
    {
      cJSON_Delete(root);
      return NULL;
    }
  return root;
}

/****************************************************************************
 * Name: ny_lighting_load
 ****************************************************************************/
static int ny_lighting_load(void)
{
  if (g_lighting_loaded)
    return 0;
  cJSON *root = NULL;
  uint64_t revision;
  int ret = ny_product_store_read(NY_LIGHTING_DOMAIN, &root, &revision);
  if (ret < 0)
    return ret;
  struct ny_lighting_group_s groups[NY_LIGHTING_GROUPS];
  ny_lighting_defaults(groups);
  if (root)
    {
      const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "groups");
      if (!cJSON_IsArray(items) ||
          cJSON_GetArraySize(items) != NY_LIGHTING_GROUPS)
        ret = -EIO;
      for (int i = 0; i < NY_LIGHTING_GROUPS && ret == 0; i++)
        {
          const cJSON *row = cJSON_GetArrayItem(items, i);
          if (strcmp(ny_lighting_text(row, "id"), g_ny_lighting_ids[i]) ||
              ny_lighting_parse_group(row, i, &groups[i], true) < 0)
            ret = -EIO;
        }
    }
  cJSON_Delete(root);
  if (ret == 0)
    {
      nxmutex_lock(&g_lighting_lock);
      memcpy(g_lighting_groups, groups, sizeof(groups));
      g_lighting_store_revision = revision;
      g_lighting_revision = revision;
      g_lighting_loaded = true;
      nxmutex_unlock(&g_lighting_lock);
    }
  return ret;
}

/****************************************************************************
 * Name: ny_lighting_status
 ****************************************************************************/
static cJSON *ny_lighting_status(void)
{
  static const char *const phases[] = { "starting", "boot",   "ready",
                                        "sleeping", "asleep", "waking" };
  static const char *const activities[] = { "idle", "listening", "thinking",
                                            "speaking", "error" };
  nxmutex_lock(&g_lighting_lock);
  cJSON *root = ny_lighting_config_json(g_lighting_groups);
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "groups");
  bool ok =
      root && cJSON_AddBoolToObject(root, "available", g_lighting_available) &&
      cJSON_AddBoolToObject(root, "running", g_lighting_running) &&
      cJSON_AddBoolToObject(root, "sleeping", g_lighting_sleep_input) &&
      cJSON_AddStringToObject(root, "phase", phases[g_lighting_phase]) &&
      cJSON_AddStringToObject(root, "preset", g_lighting_preset) &&
      cJSON_AddStringToObject(root, "activity",
                              activities[g_lighting_input.activity]) &&
      cJSON_AddNumberToObject(root, "maxBrightness",
                              NY_LIGHTING_MAX_BRIGHTNESS) &&
      cJSON_AddNumberToObject(root, "revision", g_lighting_revision) &&
      cJSON_AddNumberToObject(root, "appliedRevision", g_lighting_applied) &&
      cJSON_AddNumberToObject(root, "frames", g_lighting_frames) &&
      cJSON_AddNumberToObject(root, "lastWriteMs", g_lighting_last_write) &&
      cJSON_AddNumberToObject(root, "lastError", g_lighting_error) &&
      cJSON_AddBoolToObject(root, "musicActive", g_lighting_input.music) &&
      cJSON_AddNumberToObject(root, "musicLevel",
                              g_lighting_input.music ? g_lighting_input.level
                                                     : 0) &&
      cJSON_AddStringToObject(root, "musicSource", g_lighting_source);
  cJSON *pixels = ok ? cJSON_AddArrayToObject(root, "pixels") : NULL;
  ok = ok && pixels;
  for (unsigned int i = 0; i < NY_LIGHTING_PIXELS && ok; i++)
    {
      cJSON *pixel = cJSON_CreateNumber(g_lighting_output[i]);
      ok = pixel && cJSON_AddItemToArray(pixels, pixel);
      if (!ok)
        cJSON_Delete(pixel);
    }
  for (unsigned int i = 0; i < NY_LIGHTING_GROUPS && ok; i++)
    {
      cJSON *row = cJSON_GetArrayItem(items, i);
      uint32_t rgb = g_lighting_output[g_ny_lighting_first[i]];
      char color[8];
      snprintf(color, sizeof(color), "#%06lx",
               (unsigned long)(rgb & 0xffffff));
      ok = cJSON_AddBoolToObject(row, "enabled",
                                 g_lighting_groups[i].enabled) &&
           cJSON_AddNumberToObject(row, "count", g_ny_lighting_count[i]) &&
           cJSON_AddStringToObject(row, "outputColor", color);
    }
  nxmutex_unlock(&g_lighting_lock);
  if (!ok)
    {
      cJSON_Delete(root);
      return NULL;
    }
  return root;
}

/****************************************************************************
 * Name: ny_lighting_preset
 ****************************************************************************/
static int ny_lighting_preset(const char *name,
                              struct ny_lighting_group_s *groups)
{
  ny_lighting_defaults(groups);
  for (unsigned int i = 0; i < NY_LIGHTING_GROUPS; i++)
    groups[i].enabled = true;
  if (!strcmp(name, "calm"))
    {
      for (unsigned int i = 0; i < NY_LIGHTING_GROUPS; i++)
        {
          groups[i].effect = NY_LIGHTING_BREATHE;
          groups[i].speed = 1;
        }
      groups[2].effect = NY_LIGHTING_HEARTBEAT;
      groups[2].brightness = 10;
      groups[3].color = 0xffbf8c;
      groups[3].brightness = 8;
    }
  else if (!strcmp(name, "aurora"))
    {
      groups[0].color_mode = groups[1].color_mode = NY_LIGHTING_COMPLEMENT;
      groups[0].effect = groups[1].effect = NY_LIGHTING_BREATHE;
      groups[2].color = 0xc49aff;
      groups[2].brightness = 8;
      groups[3].effect = NY_LIGHTING_AURORA;
      groups[3].color = 0x638cff;
      groups[3].brightness = 15;
      groups[3].speed = 1;
    }
  else if (!strcmp(name, "focus"))
    {
      groups[0].brightness = groups[1].brightness = 8;
      groups[2].effect = NY_LIGHTING_STATUS;
      groups[3].color = 0xffd29a;
      groups[3].brightness = 8;
    }
  else if (!strcmp(name, "party"))
    {
      for (unsigned int i = 0; i < NY_LIGHTING_GROUPS; i++)
        {
          groups[i].effect = NY_LIGHTING_MUSIC;
          groups[i].brightness = 20;
        }
      groups[2].color = 0xff78a1;
      groups[3].color = 0x5ccfff;
      groups[3].brightness = 25;
    }
  else
    return -EINVAL;
  return 0;
}

/****************************************************************************
 * Name: ny_product_lighting_request
 ****************************************************************************/
int ny_product_lighting_request(const struct ny_product_caller_s *caller,
                                const char *topic, const cJSON *data,
                                cJSON **result)
{
  if (strncmp(topic, "lighting.", 9))
    return -ENOSYS;
  bool read = !strcmp(topic, "lighting.status"),
       preset = !strcmp(topic, "lighting.preset");
  if (!read && !preset && strcmp(topic, "lighting.set"))
    return -ENOSYS;
  if (!cJSON_IsObject(data) || !result)
    return -EINVAL;
  if (!read && caller->role < NY_PRODUCT_FAMILY)
    return -EACCES;
  nxmutex_lock(&g_lighting_request_lock);
  int ret = ny_lighting_load();
  if (ret < 0)
    goto out;
  if (!read)
    {
      const cJSON *field;
      cJSON_ArrayForEach(field, data)
      {
        const char *key = field->string;
        if (!key ||
            (strcmp(key, "revision") &&
             (preset ? strcmp(key, "name")
                     : strcmp(key, "group") && strcmp(key, "enabled") &&
                           strcmp(key, "color") && strcmp(key, "brightness") &&
                           strcmp(key, "brightnessDelta") &&
                           strcmp(key, "effect") && strcmp(key, "colorMode") &&
                           strcmp(key, "speed"))))
          {
            ret = -EINVAL;
            goto out;
          }
      }
      struct ny_lighting_group_s candidate[NY_LIGHTING_GROUPS];
      nxmutex_lock(&g_lighting_lock);
      memcpy(candidate, g_lighting_groups, sizeof(candidate));
      bool available = g_lighting_available && g_lighting_running;
      bool sleeping = g_lighting_sleep_input;
      uint64_t revision = g_lighting_revision;
      nxmutex_unlock(&g_lighting_lock);
      if (!available)
        {
          ret = -ENODEV;
          goto out;
        }
      const cJSON *expected =
          cJSON_GetObjectItemCaseSensitive(data, "revision");
      if (expected &&
          (!cJSON_IsNumber(expected) || expected->valuedouble != revision))
        {
          ret = -ESTALE;
          goto out;
        }
      bool off =
          cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(data, "enabled"));
      if (sleeping && (preset || !off))
        {
          ret = -EBUSY;
          goto out;
        }
      if (preset)
        ret = ny_lighting_preset(ny_lighting_text(data, "name"), candidate);
      else
        {
          const char *name = ny_lighting_text(data, "group");
          bool all = !strcmp(name, "all"), ears = !strcmp(name, "ears");
          int selected =
              ny_lighting_lookup(name, g_ny_lighting_ids, NY_LIGHTING_GROUPS);
          if (!all && !ears && selected < 0)
            {
              ret = -EINVAL;
              goto out;
            }
          if (cJSON_GetArraySize(data) <= (expected ? 2 : 1))
            {
              ret = -EINVAL;
              goto out;
            }
          for (unsigned int i = 0; i < NY_LIGHTING_GROUPS && ret == 0; i++)
            if (all || (ears && i < 2) || (int)i == selected)
              ret = ny_lighting_parse_group(data, i, &candidate[i], false);
        }
      if (ret < 0)
        goto out;
      cJSON *saved = ny_lighting_config_json(candidate);
      if (!saved)
        {
          ret = -ENOMEM;
          goto out;
        }
      /* Persist preferences only. Enabled flags are deliberately absent. */
      ret = ny_product_store_write(NY_LIGHTING_DOMAIN, saved,
                                   g_lighting_store_revision,
                                   &g_lighting_store_revision);
      cJSON_Delete(saved);
      if (ret < 0)
        goto out;
      nxmutex_lock(&g_lighting_lock);
      memcpy(g_lighting_from, g_lighting_output, sizeof(g_lighting_from));
      memcpy(g_lighting_groups, candidate, sizeof(candidate));
      g_lighting_changed = ny_product_time_ms(true);
      g_lighting_revision++;
      g_lighting_boot_seen = true;
      if (g_lighting_phase == NY_LIGHTING_BOOT ||
          g_lighting_phase == NY_LIGHTING_BOOT_WAIT)
        g_lighting_phase = NY_LIGHTING_READY;
      if (off && !strcmp(ny_lighting_text(data, "group"), "all"))
        g_lighting_force_black = true;
      snprintf(g_lighting_preset, sizeof(g_lighting_preset), "%s",
               preset ? ny_lighting_text(data, "name") : "custom");
      nxmutex_unlock(&g_lighting_lock);
    }
  *result = ny_lighting_status();
  if (!*result)
    ret = -ENOMEM;
out:
  nxmutex_unlock(&g_lighting_request_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_lighting_inputs
 ****************************************************************************/
static void ny_lighting_inputs(struct ny_lighting_input_s *input, bool *asleep,
                               bool *eye_ready, char *source, size_t size)
{
  (void)size;
  memset(input, 0, sizeof(*input));
  input->iris[0] = input->iris[1] = 0x56ffb2;
  *asleep = false;
  *eye_ready = true;
  source[0] = 0;
#ifdef CONFIG_NYABULA_CORE_EYE
  struct nyabula_core_snapshot_s eyes;
  *eye_ready = nyabula_eye_service_snapshot(&eyes) == 0;
  if (*eye_ready)
    {
      memcpy(input->iris, eyes.iris_rgb, sizeof(input->iris));
      *asleep = eyes.expression == NYABULA_EYE_EXPRESSION_SLEEP;
    }
#endif
  float peak = 0;
#ifdef CONFIG_NYABULA_CORE_DSP
  bool playing = ny_product_dsp_meter(&peak, source, size);
  input->music = playing && strcmp(source, "tts") != 0;
#endif
  float target = isfinite(peak) ? fminf(1, sqrtf(fmaxf(0, peak) * 8)) : 0;
  g_lighting_envelope = target > g_lighting_envelope
                            ? target
                            : target + (g_lighting_envelope - target) * 0.72f;
  input->level = g_lighting_envelope;
#ifdef CONFIG_NYABULA_CORE_VOICE
  const char *state = ny_voice_activity();
  input->activity = !strcmp(state, "listening")  ? NY_LIGHTING_LISTENING
                    : !strcmp(state, "thinking") ? NY_LIGHTING_THINKING
                    : !strcmp(state, "speaking") ? NY_LIGHTING_SPEAKING
                    : !strcmp(state, "error")    ? NY_LIGHTING_ERROR
                                                 : NY_LIGHTING_IDLE;
#endif
}

/****************************************************************************
 * Name: ny_lighting_update
 ****************************************************************************/
static void ny_lighting_update(uint64_t now,
                               const struct ny_lighting_input_s *input,
                               bool asleep, bool eye_ready, uint32_t *pixels)
{
  g_lighting_input = *input;
  g_lighting_sleep_input = asleep;
  if (g_lighting_phase == NY_LIGHTING_BOOT_WAIT && eye_ready)
    {
      g_lighting_phase = NY_LIGHTING_BOOT;
      g_lighting_phase_since = now;
    }
  if (asleep && g_lighting_phase != NY_LIGHTING_ASLEEP &&
      g_lighting_phase != NY_LIGHTING_FADING_SLEEP)
    {
      memcpy(g_lighting_from, g_lighting_output, sizeof(g_lighting_from));
      g_lighting_phase = NY_LIGHTING_FADING_SLEEP;
      g_lighting_phase_since = now;
    }
  else if (!asleep && (g_lighting_phase == NY_LIGHTING_ASLEEP ||
                       g_lighting_phase == NY_LIGHTING_FADING_SLEEP))
    {
      memcpy(g_lighting_from, g_lighting_output, sizeof(g_lighting_from));
      g_lighting_phase = NY_LIGHTING_WAKING;
      g_lighting_phase_since = now;
    }
  uint64_t elapsed = now - g_lighting_phase_since;
  if (g_lighting_phase == NY_LIGHTING_BOOT && elapsed >= NY_LIGHTING_BOOT_MS)
    {
      g_lighting_phase = NY_LIGHTING_READY;
      g_lighting_boot_seen = true;
    }
  if (g_lighting_phase == NY_LIGHTING_FADING_SLEEP &&
      elapsed >= NY_LIGHTING_SLEEP_MS)
    g_lighting_phase = NY_LIGHTING_ASLEEP;
  if (g_lighting_phase == NY_LIGHTING_WAKING && elapsed >= 400)
    g_lighting_phase = NY_LIGHTING_READY;
  if (g_lighting_force_black || g_lighting_phase == NY_LIGHTING_BOOT_WAIT ||
      g_lighting_phase == NY_LIGHTING_ASLEEP)
    memset(pixels, 0, NY_LIGHTING_PIXELS * sizeof(*pixels));
  else if (g_lighting_phase == NY_LIGHTING_BOOT)
    ny_lighting_boot(input->iris, elapsed, pixels);
  else if (g_lighting_phase == NY_LIGHTING_FADING_SLEEP)
    ny_lighting_sleep(g_lighting_from, elapsed, pixels);
  else
    {
      ny_lighting_render(g_lighting_groups, input, now, pixels);
      float blend =
          g_lighting_phase == NY_LIGHTING_WAKING
              ? elapsed / 400.0f
              : (now - g_lighting_changed) / (float)NY_LIGHTING_CHANGE_MS;
      if (blend < 1)
        for (unsigned int i = 0; i < NY_LIGHTING_PIXELS; i++)
          pixels[i] = ny_lighting_mix(g_lighting_from[i], pixels[i], blend);
    }
}

/****************************************************************************
 * Name: ny_lighting_worker
 ****************************************************************************/
static void *ny_lighting_worker(void *arg)
{
  (void)arg;
  int fd = open(NY_LIGHTING_DEVICE, O_WRONLY);
  int error = fd < 0 ? -errno : 0;
  uint32_t pixels[NY_LIGHTING_PIXELS] = { 0 };
  nxmutex_lock(&g_lighting_lock);
  g_lighting_available = fd >= 0;
  g_lighting_error = error;
  nxmutex_unlock(&g_lighting_lock);
  bool first = true;
  for (;;)
    {
      nxmutex_lock(&g_lighting_lock);
      bool stop = g_lighting_stopping;
      nxmutex_unlock(&g_lighting_lock);
      if (stop)
        break;
      struct ny_lighting_input_s input;
      bool asleep, ready;
      char source[24];
      ny_lighting_inputs(&input, &asleep, &ready, source, sizeof(source));
      nxmutex_lock(&g_lighting_lock);
      uint64_t now = ny_product_time_ms(true);
      ny_lighting_update(now, &input, asleep, ready, pixels);
      snprintf(g_lighting_source, sizeof(g_lighting_source), "%s", source);
      bool changed = first || g_lighting_force_black ||
                     g_lighting_error != 0 ||
                     memcmp(pixels, g_lighting_output, sizeof(pixels)) != 0;
      if (!changed && fd >= 0)
        g_lighting_applied = g_lighting_revision;
      uint64_t revision = g_lighting_revision;
      nxmutex_unlock(&g_lighting_lock);
      if (changed && fd >= 0)
        {
          /* This fixed 33-pixel frame takes about 1.6 ms. Keep tasks from
           * starving its polled FIFO; hardware interrupts remain enabled.
           * Nothing that waits for storage or input belongs in this window.
           */
          sched_lock();
          ssize_t written = write(fd, pixels, sizeof(pixels));
          error = written < 0                         ? -errno
                  : (size_t)written != sizeof(pixels) ? -EIO
                                                      : 0;
          sched_unlock();
          nxmutex_lock(&g_lighting_lock);
          g_lighting_error = error;
          if (error == 0)
            {
              memcpy(g_lighting_output, pixels, sizeof(pixels));
              g_lighting_frames++;
              g_lighting_last_write = now;
              g_lighting_applied = revision;
              if (revision == g_lighting_revision)
                g_lighting_force_black = false;
              first = false;
            }
          nxmutex_unlock(&g_lighting_lock);
        }
      usleep(NY_LIGHTING_PERIOD_US);
    }
  if (fd >= 0)
    {
      memset(pixels, 0, sizeof(pixels));
      sched_lock();
      ssize_t written = write(fd, pixels, sizeof(pixels));
      error = written < 0                         ? -errno
              : (size_t)written != sizeof(pixels) ? -EIO
                                                  : 0;
      sched_unlock();
      close(fd);
    }
  nxmutex_lock(&g_lighting_lock);
  g_lighting_running = false;
  g_lighting_available = false;
  g_lighting_error = error;
  if (!error)
    memset(g_lighting_output, 0, sizeof(g_lighting_output));
  nxmutex_unlock(&g_lighting_lock);
  return NULL;
}

/****************************************************************************
 * Name: ny_product_lighting_start
 ****************************************************************************/
int ny_product_lighting_start(void)
{
  nxmutex_lock(&g_lighting_request_lock);
  if (g_lighting_joinable)
    {
      nxmutex_unlock(&g_lighting_request_lock);
      return 0;
    }
  int ret = ny_lighting_load();
  if (ret < 0)
    {
      nxmutex_unlock(&g_lighting_request_lock);
      return ret;
    }
  nxmutex_lock(&g_lighting_lock);
  for (unsigned int i = 0; i < NY_LIGHTING_GROUPS; i++)
    g_lighting_groups[i].enabled = false;
  g_lighting_stopping = false;
  g_lighting_running = true;
  g_lighting_force_black = true;
  g_lighting_phase =
      g_lighting_boot_seen ? NY_LIGHTING_READY : NY_LIGHTING_BOOT_WAIT;
  g_lighting_phase_since = ny_product_time_ms(true);
  g_lighting_envelope = 0;
  nxmutex_unlock(&g_lighting_lock);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, NY_LIGHTING_STACK);
  ret = pthread_create(&g_lighting_thread, &attr, ny_lighting_worker, NULL);
  pthread_attr_destroy(&attr);
  nxmutex_lock(&g_lighting_lock);
  g_lighting_joinable = ret == 0;
  if (ret != 0)
    {
      g_lighting_running = false;
      g_lighting_error = -ret;
    }
  nxmutex_unlock(&g_lighting_lock);
  nxmutex_unlock(&g_lighting_request_lock);
  return ret ? -ret : 0;
}

/****************************************************************************
 * Name: ny_product_lighting_shutdown
 ****************************************************************************/
void ny_product_lighting_shutdown(void)
{
  nxmutex_lock(&g_lighting_request_lock);
  nxmutex_lock(&g_lighting_lock);
  bool joinable = g_lighting_joinable;
  g_lighting_stopping = true;
  nxmutex_unlock(&g_lighting_lock);
  if (joinable)
    pthread_join(g_lighting_thread, NULL);
  nxmutex_lock(&g_lighting_lock);
  g_lighting_joinable = false;
  nxmutex_unlock(&g_lighting_lock);
  nxmutex_unlock(&g_lighting_request_lock);
}
