/****************************************************************************
 * app/nyabula/src/nyabula_eye_wire.c
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

#include "../include/nyabula_eye_wire.h"
#include <errno.h>
#include <stdio.h>

struct nyabula_eye_wire_name_s
{
  const char *name;
  int value;
};

static const char *
nyabula_eye_wire_name(const struct nyabula_eye_wire_name_s *table,
                      size_t count, int value);
static cJSON *nyabula_eye_wire_payload_json(
    const struct nyabula_eye_scene_payload_s *payload);
static cJSON *
nyabula_eye_wire_owner_json(const struct nyabula_core_owner_s *owner);

static const struct nyabula_eye_wire_name_s g_nyabula_expressions[] = {
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

static const struct nyabula_eye_wire_name_s g_nyabula_scenes[] = {
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

static const struct nyabula_eye_wire_name_s g_nyabula_styles[] = {
  { "full", NYABULA_EYE_SCENE_STYLE_FULL },
  { "minimal", NYABULA_EYE_SCENE_STYLE_MINIMAL },
};

static const struct nyabula_eye_wire_name_s g_nyabula_weather[] = {
  { "sunny", NYABULA_EYE_WEATHER_SUNNY },
  { "cloudy", NYABULA_EYE_WEATHER_CLOUDY },
  { "rain", NYABULA_EYE_WEATHER_RAIN },
  { "storm", NYABULA_EYE_WEATHER_STORM },
  { "snow", NYABULA_EYE_WEATHER_SNOW },
  { "fog", NYABULA_EYE_WEATHER_FOG },
};

static const struct nyabula_eye_wire_name_s g_nyabula_music_views[] = {
  { "spectrum", NYABULA_EYE_MUSIC_SPECTRUM },
  { "lyrics", NYABULA_EYE_MUSIC_LYRICS },
};

static const struct nyabula_eye_wire_name_s g_nyabula_battery_states[] = {
  { "charging", NYABULA_EYE_BATTERY_CHARGING },
  { "low", NYABULA_EYE_BATTERY_LOW },
  { "full", NYABULA_EYE_BATTERY_FULL },
  { "hot", NYABULA_EYE_BATTERY_HOT },
  { "dock", NYABULA_EYE_BATTERY_DOCK },
};

static const struct nyabula_eye_wire_name_s g_nyabula_alarm_copies[] = {
  { "name", NYABULA_EYE_ALARM_COPY_NAME },
  { "reminder", NYABULA_EYE_ALARM_COPY_REMINDER },
  { "none", NYABULA_EYE_ALARM_COPY_NONE },
};

static const struct nyabula_eye_wire_name_s g_nyabula_call_states[] = {
  { "incoming", NYABULA_EYE_CALL_INCOMING },
  { "active", NYABULA_EYE_CALL_ACTIVE },
  { "ended", NYABULA_EYE_CALL_ENDED },
};

static const struct nyabula_eye_wire_name_s g_nyabula_task_states[] = {
  { "running", NYABULA_EYE_TASK_RUNNING },
  { "queued", NYABULA_EYE_TASK_QUEUED },
  { "confirm", NYABULA_EYE_TASK_CONFIRM },
  { "done", NYABULA_EYE_TASK_DONE },
  { "failed", NYABULA_EYE_TASK_FAILED },
};

static const struct nyabula_eye_wire_name_s g_nyabula_network_states[] = {
  { "wifi", NYABULA_EYE_NETWORK_WIFI },
  { "bluetooth", NYABULA_EYE_NETWORK_BLUETOOTH },
  { "offline", NYABULA_EYE_NETWORK_OFFLINE },
};

static const struct nyabula_eye_wire_name_s g_nyabula_audio_routes[] = {
  { "speaker", NYABULA_EYE_AUDIO_SPEAKER },
  { "headphones", NYABULA_EYE_AUDIO_HEADPHONES },
  { "both", NYABULA_EYE_AUDIO_BOTH },
  { "mute", NYABULA_EYE_AUDIO_MUTE },
};

static const struct nyabula_eye_wire_name_s g_nyabula_eq_views[] = {
  { "profile", NYABULA_EYE_EQ_PROFILE },
  { "calibrating", NYABULA_EYE_EQ_CALIBRATING },
};

#define NYABULA_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static const char *
nyabula_eye_wire_name(const struct nyabula_eye_wire_name_s *table,
                      size_t count, int value)
{
  for (size_t i = 0; i < count; i++)
    {
      if (table[i].value == value)
        {
          return table[i].name;
        }
    }

  return "unknown";
}

static cJSON *nyabula_eye_wire_payload_json(
    const struct nyabula_eye_scene_payload_s *payload)
{
  cJSON *json = cJSON_CreateObject();
  cJSON *bands = cJSON_AddArrayToObject(json, "eq_bands");

#define NYABULA_JSON_ENUM(field, table)                                    \
  cJSON_AddStringToObject(json, #field,                                    \
                          nyabula_eye_wire_name(table,                     \
                                                NYABULA_ARRAY_SIZE(table), \
                                                payload->field))
  NYABULA_JSON_ENUM(weather, g_nyabula_weather);
  NYABULA_JSON_ENUM(music_view, g_nyabula_music_views);
  NYABULA_JSON_ENUM(battery_state, g_nyabula_battery_states);
  NYABULA_JSON_ENUM(alarm_copy, g_nyabula_alarm_copies);
  NYABULA_JSON_ENUM(call_state, g_nyabula_call_states);
  NYABULA_JSON_ENUM(task_state, g_nyabula_task_states);
  NYABULA_JSON_ENUM(network_state, g_nyabula_network_states);
  NYABULA_JSON_ENUM(audio_route, g_nyabula_audio_routes);
  NYABULA_JSON_ENUM(eq_view, g_nyabula_eq_views);
#undef NYABULA_JSON_ENUM

#define NYABULA_JSON_NUMBER(field) \
  cJSON_AddNumberToObject(json, #field, payload->field)
  NYABULA_JSON_NUMBER(duration_ms);
  NYABULA_JSON_NUMBER(position_ms);
  NYABULA_JSON_NUMBER(remaining_ms);
  NYABULA_JSON_NUMBER(elapsed_ms);
  NYABULA_JSON_NUMBER(year);
  NYABULA_JSON_NUMBER(month);
  NYABULA_JSON_NUMBER(day);
  NYABULA_JSON_NUMBER(hour);
  NYABULA_JSON_NUMBER(minute);
  NYABULA_JSON_NUMBER(percent);
  NYABULA_JSON_NUMBER(device_count);
  NYABULA_JSON_NUMBER(briefing_index);
  NYABULA_JSON_NUMBER(briefing_count);
  NYABULA_JSON_NUMBER(temperature_c);
  NYABULA_JSON_NUMBER(feels_like_c);
  NYABULA_JSON_NUMBER(humidity_percent);
  NYABULA_JSON_NUMBER(wind_kph);
  NYABULA_JSON_NUMBER(visibility_km);
  NYABULA_JSON_NUMBER(distance_m);
  NYABULA_JSON_NUMBER(heart_rate_bpm);
  NYABULA_JSON_NUMBER(crossover_hz);
  NYABULA_JSON_NUMBER(progress);
#undef NYABULA_JSON_NUMBER

  cJSON_AddBoolToObject(json, "active", payload->active);
  cJSON_AddBoolToObject(json, "playing", payload->playing);
  cJSON_AddBoolToObject(json, "privacy_camera", payload->privacy_camera);
  cJSON_AddBoolToObject(json, "privacy_microphone",
                        payload->privacy_microphone);
  cJSON_AddBoolToObject(json, "signal_good", payload->signal_good);

#define NYABULA_JSON_TEXT(field) \
  cJSON_AddStringToObject(json, #field, payload->field)
  NYABULA_JSON_TEXT(title);
  NYABULA_JSON_TEXT(subtitle);
  NYABULA_JSON_TEXT(detail);
  NYABULA_JSON_TEXT(value);
  NYABULA_JSON_TEXT(previous_line);
  NYABULA_JSON_TEXT(current_line);
  NYABULA_JSON_TEXT(next_line);
#undef NYABULA_JSON_TEXT

  for (int i = 0; i < NYABULA_EYE_EQ_BANDS; i++)
    {
      cJSON_AddItemToArray(bands, cJSON_CreateNumber(payload->eq_bands[i]));
    }

  return json;
}

static cJSON *
nyabula_eye_wire_owner_json(const struct nyabula_core_owner_s *owner)
{
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "active", owner->active);
  cJSON_AddStringToObject(json, "source", owner->source);
  cJSON_AddNumberToObject(json, "priority", owner->priority);
  cJSON_AddNumberToObject(json, "lease_remaining_ms",
                          owner->lease_remaining_ms);
  cJSON_AddNumberToObject(json, "sequence", owner->sequence);
  return json;
}

/****************************************************************************
 * Name: nyabula_eye_wire_snapshot
 ****************************************************************************/

cJSON *nyabula_eye_wire_snapshot(const struct nyabula_core_snapshot_s *s)
{
  cJSON *json = cJSON_CreateObject();
  cJSON *iris;
  if (json == NULL || s == NULL)
    {
      cJSON_Delete(json);
      return NULL;
    }

  cJSON_AddStringToObject(json, "schema", "nyabula.eye.v1");
  cJSON_AddNumberToObject(json, "seq", s->revision);
  cJSON_AddNumberToObject(json, "uptime_ms", s->uptime_ms);
  cJSON_AddNumberToObject(json, "expression_since_ms", s->expression_since_ms);
  cJSON_AddNumberToObject(json, "scene_since_ms", s->scene_since_ms);
  cJSON_AddStringToObject(
      json, "expression",
      nyabula_eye_wire_name(g_nyabula_expressions,
                            NYABULA_ARRAY_SIZE(g_nyabula_expressions),
                            s->expression));
  cJSON_AddStringToObject(
      json, "scene",
      nyabula_eye_wire_name(g_nyabula_scenes,
                            NYABULA_ARRAY_SIZE(g_nyabula_scenes), s->scene));
  cJSON_AddStringToObject(
      json, "scene_style",
      nyabula_eye_wire_name(g_nyabula_styles,
                            NYABULA_ARRAY_SIZE(g_nyabula_styles),
                            s->scene_style));
  cJSON_AddItemToObject(json, "scene_payload",
                        nyabula_eye_wire_payload_json(&s->scene_payload));
  cJSON_AddItemToObject(json, "expression_owner",
                        nyabula_eye_wire_owner_json(&s->expression_owner));
  cJSON_AddItemToObject(json, "scene_owner",
                        nyabula_eye_wire_owner_json(&s->scene_owner));
  cJSON_AddNumberToObject(json, "gaze_x", s->gaze_x);
  cJSON_AddNumberToObject(json, "gaze_y", s->gaze_y);
  cJSON_AddNumberToObject(json, "gaze_until_ms", s->gaze_until_ms);
  cJSON_AddBoolToObject(json, "gaze_active", s->gaze_active);
  cJSON_AddNumberToObject(json, "blink_nonce", s->blink_nonce);
  cJSON_AddNumberToObject(json, "blink_eyes", s->blink_eyes);
  cJSON_AddBoolToObject(json, "auto_blink", s->auto_blink);
  cJSON_AddNumberToObject(json, "ambient_light", s->ambient_light);
  iris = cJSON_AddArrayToObject(json, "iris_rgb");
  cJSON_AddItemToArray(iris, cJSON_CreateNumber(s->iris_rgb[0]));
  cJSON_AddItemToArray(iris, cJSON_CreateNumber(s->iris_rgb[1]));
  cJSON_AddStringToObject(json, "last_request_id", s->last_request_id);
  cJSON_AddNumberToObject(json, "last_status", s->last_status);
  cJSON_AddStringToObject(json, "last_error", s->last_error);
  return json;
}
