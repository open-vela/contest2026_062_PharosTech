/****************************************************************************
 * app/nyabula/src/nyabula_eye_json.c
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

#include "../include/nyabula_eye_service.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <netutils/cJSON.h>
#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nyabula_eye_json_name_s
{
  const char *name;
  int value;
};

static const struct nyabula_eye_json_name_s g_nyabula_expressions[] = {
  { "idle", NYABULA_EYE_EXPRESSION_IDLE },
  { "curious", NYABULA_EYE_EXPRESSION_CURIOUS },
  { "happy", NYABULA_EYE_EXPRESSION_HAPPY },
  { "processing", NYABULA_EYE_EXPRESSION_PROCESSING },
  { "star", NYABULA_EYE_EXPRESSION_STAR },
  { "heart", NYABULA_EYE_EXPRESSION_HEART },
  { "sleepy", NYABULA_EYE_EXPRESSION_SLEEPY },
  { "sleep", NYABULA_EYE_EXPRESSION_SLEEP },
  { "angry", NYABULA_EYE_EXPRESSION_ANGRY },
  { "sad", NYABULA_EYE_EXPRESSION_SAD },
  { "surprise", NYABULA_EYE_EXPRESSION_SURPRISE },
  { "dizzy", NYABULA_EYE_EXPRESSION_DIZZY },
  { "derp", NYABULA_EYE_EXPRESSION_DERP },
};

static const struct nyabula_eye_json_name_s g_nyabula_scenes[] = {
  { "none", NYABULA_EYE_SCENE_NONE },
  { "music", NYABULA_EYE_SCENE_MUSIC },
  { "timer", NYABULA_EYE_SCENE_TIMER },
  { "weather", NYABULA_EYE_SCENE_WEATHER },
  { "battery", NYABULA_EYE_SCENE_BATTERY },
  { "alarm", NYABULA_EYE_SCENE_ALARM },
  { "call", NYABULA_EYE_SCENE_CALL },
  { "task", NYABULA_EYE_SCENE_TASK },
  { "stopwatch", NYABULA_EYE_SCENE_STOPWATCH },
  { "calendar", NYABULA_EYE_SCENE_CALENDAR },
  { "sleep_timer", NYABULA_EYE_SCENE_SLEEP_TIMER },
  { "network", NYABULA_EYE_SCENE_NETWORK },
  { "audio", NYABULA_EYE_SCENE_AUDIO },
  { "eq", NYABULA_EYE_SCENE_EQ },
  { "caption", NYABULA_EYE_SCENE_CAPTION },
  { "briefing", NYABULA_EYE_SCENE_BRIEFING },
  { "privacy", NYABULA_EYE_SCENE_PRIVACY },
  { "identity", NYABULA_EYE_SCENE_IDENTITY },
  { "memory", NYABULA_EYE_SCENE_MEMORY },
  { "devices", NYABULA_EYE_SCENE_DEVICES },
  { "system", NYABULA_EYE_SCENE_SYSTEM },
  { "health", NYABULA_EYE_SCENE_HEALTH },
  { "presence", NYABULA_EYE_SCENE_PRESENCE },
  { "companion", NYABULA_EYE_SCENE_COMPANION },
  { "home", NYABULA_EYE_SCENE_HOME },
  { "subwoofer", NYABULA_EYE_SCENE_SUBWOOFER },
};

static const struct nyabula_eye_json_name_s g_nyabula_styles[] = {
  { "full", NYABULA_EYE_SCENE_STYLE_FULL },
  { "minimal", NYABULA_EYE_SCENE_STYLE_MINIMAL },
};

static const struct nyabula_eye_json_name_s g_nyabula_weather[] = {
  { "sunny", NYABULA_EYE_WEATHER_SUNNY },
  { "cloudy", NYABULA_EYE_WEATHER_CLOUDY },
  { "rain", NYABULA_EYE_WEATHER_RAIN },
  { "storm", NYABULA_EYE_WEATHER_STORM },
  { "snow", NYABULA_EYE_WEATHER_SNOW },
  { "fog", NYABULA_EYE_WEATHER_FOG },
};

static const struct nyabula_eye_json_name_s g_nyabula_music_views[] = {
  { "spectrum", NYABULA_EYE_MUSIC_SPECTRUM },
  { "lyrics", NYABULA_EYE_MUSIC_LYRICS },
};

static const struct nyabula_eye_json_name_s g_nyabula_battery_states[] = {
  { "charging", NYABULA_EYE_BATTERY_CHARGING },
  { "low", NYABULA_EYE_BATTERY_LOW },
  { "full", NYABULA_EYE_BATTERY_FULL },
  { "hot", NYABULA_EYE_BATTERY_HOT },
  { "dock", NYABULA_EYE_BATTERY_DOCK },
};

static const struct nyabula_eye_json_name_s g_nyabula_alarm_copies[] = {
  { "name", NYABULA_EYE_ALARM_COPY_NAME },
  { "reminder", NYABULA_EYE_ALARM_COPY_REMINDER },
  { "none", NYABULA_EYE_ALARM_COPY_NONE },
};

static const struct nyabula_eye_json_name_s g_nyabula_call_states[] = {
  { "incoming", NYABULA_EYE_CALL_INCOMING },
  { "active", NYABULA_EYE_CALL_ACTIVE },
  { "ended", NYABULA_EYE_CALL_ENDED },
};

static const struct nyabula_eye_json_name_s g_nyabula_task_states[] = {
  { "running", NYABULA_EYE_TASK_RUNNING },
  { "queued", NYABULA_EYE_TASK_QUEUED },
  { "confirm", NYABULA_EYE_TASK_CONFIRM },
  { "done", NYABULA_EYE_TASK_DONE },
  { "failed", NYABULA_EYE_TASK_FAILED },
};

static const struct nyabula_eye_json_name_s g_nyabula_network_states[] = {
  { "wifi", NYABULA_EYE_NETWORK_WIFI },
  { "bluetooth", NYABULA_EYE_NETWORK_BLUETOOTH },
  { "offline", NYABULA_EYE_NETWORK_OFFLINE },
};

static const struct nyabula_eye_json_name_s g_nyabula_audio_routes[] = {
  { "speaker", NYABULA_EYE_AUDIO_SPEAKER },
  { "headphones", NYABULA_EYE_AUDIO_HEADPHONES },
  { "both", NYABULA_EYE_AUDIO_BOTH },
  { "mute", NYABULA_EYE_AUDIO_MUTE },
};

static const struct nyabula_eye_json_name_s g_nyabula_eq_views[] = {
  { "profile", NYABULA_EYE_EQ_PROFILE },
  { "calibrating", NYABULA_EYE_EQ_CALIBRATING },
};

#define NYABULA_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int nyabula_eye_json_validate_numbers(const cJSON *object);
/* Check representation bounds before converting JSON doubles to C fields. */

static int nyabula_eye_json_validate_numbers(const cJSON *object)
{
  static const struct
  {
    const char *name;
    double maximum;
  } ranges[] = {
    { "priority", UINT8_MAX },
    { "lease_ms", UINT32_MAX },
    { "transition_ms", UINT32_MAX },
    { "hold_ms", UINT32_MAX },
    { "duration_ms", UINT32_MAX },
    { "position_ms", UINT32_MAX },
    { "remaining_ms", UINT32_MAX },
    { "elapsed_ms", UINT32_MAX },
    { "year", UINT16_MAX },
    { "month", UINT8_MAX },
    { "day", UINT8_MAX },
    { "hour", UINT8_MAX },
    { "minute", UINT8_MAX },
    { "percent", UINT8_MAX },
    { "device_count", UINT8_MAX },
    { "briefing_index", UINT8_MAX },
    { "briefing_count", UINT8_MAX },
  };

  for (const cJSON *item = object; item != NULL; item = item->next)
    {
      if (cJSON_IsNumber(item))
        {
          double value = item->valuedouble;
          if (!isfinite(value) || value < -FLT_MAX || value > FLT_MAX)
            {
              return -ERANGE;
            }

          for (size_t i = 0;
               item->string != NULL && i < NYABULA_ARRAY_SIZE(ranges); i++)
            {
              if (strcmp(item->string, ranges[i].name) == 0 &&
                  (value < 0 || value > ranges[i].maximum ||
                   floor(value) != value))
                {
                  return -ERANGE;
                }
            }
        }

      if (item->child != NULL &&
          nyabula_eye_json_validate_numbers(item->child) < 0)
        {
          return -ERANGE;
        }
    }

  return 0;
}

static int nyabula_eye_json_lookup(const struct nyabula_eye_json_name_s *table,
                                   size_t count, const char *name);
static cJSON *nyabula_eye_json_object_item(cJSON *object, const char *name);
static const char *nyabula_eye_json_string(cJSON *object, const char *name,
                                           const char *fallback);
static double nyabula_eye_json_number(cJSON *object, const char *name,
                                      double fallback);
static bool nyabula_eye_json_boolean(cJSON *object, const char *name,
                                     bool fallback);
static void nyabula_eye_json_copy(char *dest, size_t size, const char *source);
static int nyabula_eye_json_enum(cJSON *object, const char *name,
                                 const struct nyabula_eye_json_name_s *table,
                                 size_t count, int fallback);
static void
nyabula_eye_json_parse_payload(cJSON *object,
                               struct nyabula_eye_scene_payload_s *payload);

static int nyabula_eye_json_lookup(const struct nyabula_eye_json_name_s *table,
                                   size_t count, const char *name)
{
  for (size_t i = 0; i < count; i++)
    {
      if (strcmp(table[i].name, name) == 0)
        {
          return table[i].value;
        }
    }

  return -1;
}

static cJSON *nyabula_eye_json_object_item(cJSON *object, const char *name)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return item;
}

static const char *nyabula_eye_json_string(cJSON *object, const char *name,
                                           const char *fallback)
{
  cJSON *item = nyabula_eye_json_object_item(object, name);
  return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring
                                                           : fallback;
}

static double nyabula_eye_json_number(cJSON *object, const char *name,
                                      double fallback)
{
  cJSON *item = nyabula_eye_json_object_item(object, name);
  return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool nyabula_eye_json_boolean(cJSON *object, const char *name,
                                     bool fallback)
{
  cJSON *item = nyabula_eye_json_object_item(object, name);
  return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void nyabula_eye_json_copy(char *dest, size_t size, const char *source)
{
  size_t length;
  if (size == 0)
    {
      return;
    }

  source = source == NULL ? "" : source;
  length = strlen(source);
  if (length >= size)
    {
      length = size - 1;
      while (length > 0 && ((uint8_t)source[length] & 0xc0) == 0x80)
        {
          length--;
        }
    }

  memcpy(dest, source, length);
  dest[length] = '\0';
}

static int nyabula_eye_json_enum(cJSON *object, const char *name,
                                 const struct nyabula_eye_json_name_s *table,
                                 size_t count, int fallback)
{
  const char *value = nyabula_eye_json_string(object, name, NULL);
  int result;

  if (value == NULL)
    {
      return fallback;
    }

  result = nyabula_eye_json_lookup(table, count, value);
  return result < 0 ? fallback : result;
}

static void
nyabula_eye_json_parse_payload(cJSON *object,
                               struct nyabula_eye_scene_payload_s *payload)
{
  cJSON *bands;

  memset(payload, 0, sizeof(*payload));
  payload->weather = nyabula_eye_json_enum(
      object, "weather", g_nyabula_weather,
      NYABULA_ARRAY_SIZE(g_nyabula_weather), NYABULA_EYE_WEATHER_SUNNY);
  payload->music_view = nyabula_eye_json_enum(
      object, "music_view", g_nyabula_music_views,
      NYABULA_ARRAY_SIZE(g_nyabula_music_views), NYABULA_EYE_MUSIC_SPECTRUM);
  payload->battery_state =
      nyabula_eye_json_enum(object, "battery_state", g_nyabula_battery_states,
                            NYABULA_ARRAY_SIZE(g_nyabula_battery_states),
                            NYABULA_EYE_BATTERY_CHARGING);
  payload->alarm_copy = nyabula_eye_json_enum(
      object, "alarm_copy", g_nyabula_alarm_copies,
      NYABULA_ARRAY_SIZE(g_nyabula_alarm_copies), NYABULA_EYE_ALARM_COPY_NAME);
  payload->call_state = nyabula_eye_json_enum(
      object, "call_state", g_nyabula_call_states,
      NYABULA_ARRAY_SIZE(g_nyabula_call_states), NYABULA_EYE_CALL_INCOMING);
  payload->task_state = nyabula_eye_json_enum(
      object, "task_state", g_nyabula_task_states,
      NYABULA_ARRAY_SIZE(g_nyabula_task_states), NYABULA_EYE_TASK_RUNNING);
  payload->network_state = nyabula_eye_json_enum(
      object, "network_state", g_nyabula_network_states,
      NYABULA_ARRAY_SIZE(g_nyabula_network_states), NYABULA_EYE_NETWORK_WIFI);
  payload->audio_route = nyabula_eye_json_enum(
      object, "audio_route", g_nyabula_audio_routes,
      NYABULA_ARRAY_SIZE(g_nyabula_audio_routes), NYABULA_EYE_AUDIO_SPEAKER);
  payload->eq_view = nyabula_eye_json_enum(
      object, "eq_view", g_nyabula_eq_views,
      NYABULA_ARRAY_SIZE(g_nyabula_eq_views), NYABULA_EYE_EQ_PROFILE);

#define NYABULA_PAYLOAD_NUMBER(field) \
  payload->field = nyabula_eye_json_number(object, #field, 0)
  NYABULA_PAYLOAD_NUMBER(duration_ms);
  NYABULA_PAYLOAD_NUMBER(position_ms);
  NYABULA_PAYLOAD_NUMBER(remaining_ms);
  NYABULA_PAYLOAD_NUMBER(elapsed_ms);
  NYABULA_PAYLOAD_NUMBER(year);
  NYABULA_PAYLOAD_NUMBER(month);
  NYABULA_PAYLOAD_NUMBER(day);
  NYABULA_PAYLOAD_NUMBER(hour);
  NYABULA_PAYLOAD_NUMBER(minute);
  NYABULA_PAYLOAD_NUMBER(percent);
  NYABULA_PAYLOAD_NUMBER(device_count);
  NYABULA_PAYLOAD_NUMBER(briefing_index);
  NYABULA_PAYLOAD_NUMBER(briefing_count);
  NYABULA_PAYLOAD_NUMBER(temperature_c);
  NYABULA_PAYLOAD_NUMBER(feels_like_c);
  NYABULA_PAYLOAD_NUMBER(humidity_percent);
  NYABULA_PAYLOAD_NUMBER(wind_kph);
  NYABULA_PAYLOAD_NUMBER(visibility_km);
  NYABULA_PAYLOAD_NUMBER(distance_m);
  NYABULA_PAYLOAD_NUMBER(heart_rate_bpm);
  NYABULA_PAYLOAD_NUMBER(crossover_hz);
  NYABULA_PAYLOAD_NUMBER(progress);
#undef NYABULA_PAYLOAD_NUMBER

  payload->active = nyabula_eye_json_boolean(object, "active", false);
  payload->playing = nyabula_eye_json_boolean(object, "playing", false);
  payload->privacy_camera =
      nyabula_eye_json_boolean(object, "privacy_camera", false);
  payload->privacy_microphone =
      nyabula_eye_json_boolean(object, "privacy_microphone", false);
  payload->signal_good =
      nyabula_eye_json_boolean(object, "signal_good", false);

#define NYABULA_PAYLOAD_TEXT(field)                             \
  nyabula_eye_json_copy(payload->field, sizeof(payload->field), \
                        nyabula_eye_json_string(object, #field, ""))
  NYABULA_PAYLOAD_TEXT(title);
  NYABULA_PAYLOAD_TEXT(subtitle);
  NYABULA_PAYLOAD_TEXT(detail);
  NYABULA_PAYLOAD_TEXT(value);
  NYABULA_PAYLOAD_TEXT(previous_line);
  NYABULA_PAYLOAD_TEXT(current_line);
  NYABULA_PAYLOAD_TEXT(next_line);
#undef NYABULA_PAYLOAD_TEXT

  bands = nyabula_eye_json_object_item(object, "eq_bands");
  if (cJSON_IsArray(bands))
    {
      for (int i = 0; i < NYABULA_EYE_EQ_BANDS; i++)
        {
          cJSON *band = cJSON_GetArrayItem(bands, i);
          payload->eq_bands[i] = cJSON_IsNumber(band) ? band->valuedouble : 0;
        }
    }
}

int nyabula_eye_json_parse_command(cJSON *json,
                                   struct nyabula_core_command_s *command,
                                   char *error, size_t error_size)
{
  const char *action;
  cJSON *params;
  int value;

  memset(command, 0, sizeof(*command));
  if (!cJSON_IsObject(json))
    {
      snprintf(error, error_size, "request body must be an object");
      return -EINVAL;
    }

  if (nyabula_eye_json_validate_numbers(json) < 0)
    {
      snprintf(error, error_size,
               "numeric field is outside its representation");
      return -ERANGE;
    }

  action = nyabula_eye_json_string(json, "action", NULL);
  params = nyabula_eye_json_object_item(json, "params");
  if (action == NULL || !cJSON_IsObject(params))
    {
      snprintf(error, error_size, "action and params object are required");
      return -EINVAL;
    }

  nyabula_eye_json_copy(command->source, sizeof(command->source), "core");
  nyabula_eye_json_copy(command->request_id, sizeof(command->request_id),
                        nyabula_eye_json_string(json, "id", ""));
  value = nyabula_eye_json_number(json, "priority", 50);
  if (value < 0 || value > 255)
    {
      snprintf(error, error_size, "priority must be between 0 and 255");
      return -ERANGE;
    }

  command->priority = value;
  double lease_ms = nyabula_eye_json_number(json, "lease_ms", 0);
  if (command->source[0] == '\0' || lease_ms < 0 || lease_ms > UINT32_MAX)
    {
      snprintf(error, error_size,
               "source must be non-empty and lease_ms must be uint32");
      return -ERANGE;
    }

  command->lease_ms = lease_ms;

  if (strcmp(action, "eyes.expression") == 0)
    {
      const char *name = nyabula_eye_json_string(params, "expression", NULL);
      value = name == NULL
                  ? -1
                  : nyabula_eye_json_lookup(
                        g_nyabula_expressions,
                        NYABULA_ARRAY_SIZE(g_nyabula_expressions), name);
      if (value < 0)
        {
          snprintf(error, error_size, "unknown expression");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_EXPRESSION;
      command->data.expression.expression = value;
      command->data.expression.transition_ms =
          nyabula_eye_json_number(params, "transition_ms", 280);
    }
  else if (strcmp(action, "eyes.blink") == 0 ||
           strcmp(action, "eyes.iris") == 0)
    {
      const char *eyes = nyabula_eye_json_string(params, "eyes", "both");
      if (strcmp(eyes, "left") != 0 && strcmp(eyes, "right") != 0 &&
          strcmp(eyes, "both") != 0)
        {
          snprintf(error, error_size, "eyes must be left, right or both");
          return -EINVAL;
        }

      enum nyabula_eye_mask_e mask =
          strcmp(eyes, "left") == 0    ? NYABULA_EYE_MASK_LEFT
          : strcmp(eyes, "right") == 0 ? NYABULA_EYE_MASK_RIGHT
                                       : NYABULA_EYE_MASK_BOTH;
      if (strcmp(action, "eyes.blink") == 0)
        {
          command->action = NYABULA_CORE_ACTION_BLINK;
          command->data.blink.eyes = mask;
        }
      else
        {
          const char *rgb = nyabula_eye_json_string(params, "rgb", NULL);
          char *end;
          unsigned long color;
          if (rgb == NULL)
            {
              snprintf(error, error_size, "rgb hex string is required");
              return -EINVAL;
            }

          color = strtoul(rgb[0] == '#' ? rgb + 1 : rgb, &end, 16);
          if (*end != '\0' || color > 0xffffff)
            {
              snprintf(error, error_size, "rgb must be a 24-bit hex value");
              return -EINVAL;
            }

          command->action = NYABULA_CORE_ACTION_IRIS_COLOR;
          command->data.iris_color.eyes = mask;
          command->data.iris_color.rgb = color;
        }
    }
  else if (strcmp(action, "eyes.gaze") == 0)
    {
      command->action = NYABULA_CORE_ACTION_GAZE;
      command->data.gaze.x = nyabula_eye_json_number(params, "x", 0);
      command->data.gaze.y = nyabula_eye_json_number(params, "y", 0);
      command->data.gaze.hold_ms =
          nyabula_eye_json_number(params, "hold_ms", 1000);
      if (command->data.gaze.x < -1 || command->data.gaze.x > 1 ||
          command->data.gaze.y < -1 || command->data.gaze.y > 1)
        {
          snprintf(error, error_size, "gaze x and y must be between -1 and 1");
          return -ERANGE;
        }
    }
  else if (strcmp(action, "eyes.auto_blink") == 0)
    {
      command->action = NYABULA_CORE_ACTION_AUTO_BLINK;
      command->data.auto_blink.enabled =
          nyabula_eye_json_boolean(params, "enabled", true);
    }
  else if (strcmp(action, "eyes.ambient") == 0)
    {
      command->action = NYABULA_CORE_ACTION_AMBIENT_LIGHT;
      command->data.ambient_light.level =
          nyabula_eye_json_number(params, "level", 1);
      if (command->data.ambient_light.level < 0 ||
          command->data.ambient_light.level > 1)
        {
          snprintf(error, error_size, "ambient level must be between 0 and 1");
          return -ERANGE;
        }
    }
  else if (strcmp(action, "eyes.scene.show") == 0)
    {
      const char *scene = nyabula_eye_json_string(params, "scene", NULL);
      const char *style = nyabula_eye_json_string(params, "style", "full");
      cJSON *payload = nyabula_eye_json_object_item(params, "payload");
      int scene_value = scene == NULL
                            ? -1
                            : nyabula_eye_json_lookup(
                                  g_nyabula_scenes,
                                  NYABULA_ARRAY_SIZE(g_nyabula_scenes), scene);
      int style_value = nyabula_eye_json_lookup(
          g_nyabula_styles, NYABULA_ARRAY_SIZE(g_nyabula_styles), style);
      if (scene_value <= NYABULA_EYE_SCENE_NONE || style_value < 0 ||
          !cJSON_IsObject(payload))
        {
          snprintf(error, error_size,
                   "valid scene, style and payload required");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_SCENE_SHOW;
      command->data.scene_show.request.scene = scene_value;
      command->data.scene_show.request.style = style_value;
      nyabula_eye_json_parse_payload(
          payload, &command->data.scene_show.request.payload);
    }
  else if (strcmp(action, "eyes.scene.update") == 0)
    {
      cJSON *payload = nyabula_eye_json_object_item(params, "payload");
      if (!cJSON_IsObject(payload))
        {
          snprintf(error, error_size, "payload object is required");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_SCENE_UPDATE;
      nyabula_eye_json_parse_payload(payload,
                                     &command->data.scene_update.payload);
    }
  else if (strcmp(action, "eyes.scene.hide") == 0)
    {
      command->action = NYABULA_CORE_ACTION_SCENE_HIDE;
    }
  else if (strcmp(action, "core.release") == 0)
    {
      const char *domain = nyabula_eye_json_string(params, "domain", "all");
      if (strcmp(domain, "expression") != 0 && strcmp(domain, "scene") != 0 &&
          strcmp(domain, "all") != 0)
        {
          snprintf(error, error_size,
                   "domain must be expression, scene or all");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_RELEASE;
      command->data.release.domains =
          strcmp(domain, "expression") == 0 ? NYABULA_CORE_DOMAIN_EXPRESSION
          : strcmp(domain, "scene") == 0    ? NYABULA_CORE_DOMAIN_SCENE
                                            : NYABULA_CORE_DOMAIN_ALL;
    }
  else if (strcmp(action, "core.reset") == 0)
    {
      command->action = NYABULA_CORE_ACTION_RESET;
    }
  else
    {
      snprintf(error, error_size, "unknown action: %s", action);
      return -EINVAL;
    }

  return 0;
}
