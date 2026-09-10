/****************************************************************************
 * app/nyabula/include/nyabula_eye_engine.h
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

#ifndef __APP_NYABULA_INCLUDE_NYABULA_EYE_ENGINE_H
#define __APP_NYABULA_INCLUDE_NYABULA_EYE_ENGINE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum nyabula_eye_id_e
{
  NYABULA_EYE_LEFT = 0,
  NYABULA_EYE_RIGHT,
  NYABULA_EYE_COUNT
};

enum nyabula_eye_mask_e
{
  NYABULA_EYE_MASK_LEFT = 1 << NYABULA_EYE_LEFT,
  NYABULA_EYE_MASK_RIGHT = 1 << NYABULA_EYE_RIGHT,
  NYABULA_EYE_MASK_BOTH = NYABULA_EYE_MASK_LEFT | NYABULA_EYE_MASK_RIGHT
};

enum nyabula_eye_expression_e
{
  NYABULA_EYE_EXPRESSION_IDLE = 0,
  NYABULA_EYE_EXPRESSION_CURIOUS,
  NYABULA_EYE_EXPRESSION_HAPPY,
  NYABULA_EYE_EXPRESSION_PROCESSING,
  NYABULA_EYE_EXPRESSION_STAR,
  NYABULA_EYE_EXPRESSION_HEART,
  NYABULA_EYE_EXPRESSION_SLEEPY,
  NYABULA_EYE_EXPRESSION_SLEEP,
  NYABULA_EYE_EXPRESSION_ANGRY,
  NYABULA_EYE_EXPRESSION_SAD,
  NYABULA_EYE_EXPRESSION_SURPRISE,
  NYABULA_EYE_EXPRESSION_DIZZY,
  NYABULA_EYE_EXPRESSION_DERP,
  NYABULA_EYE_EXPRESSION_COUNT
};

enum nyabula_eye_scene_e
{
  NYABULA_EYE_SCENE_NONE = 0,
  NYABULA_EYE_SCENE_MUSIC,
  NYABULA_EYE_SCENE_TIMER,
  NYABULA_EYE_SCENE_WEATHER,
  NYABULA_EYE_SCENE_BATTERY,
  NYABULA_EYE_SCENE_ALARM,
  NYABULA_EYE_SCENE_CALL,
  NYABULA_EYE_SCENE_TASK,
  NYABULA_EYE_SCENE_STOPWATCH,
  NYABULA_EYE_SCENE_CALENDAR,
  NYABULA_EYE_SCENE_SLEEP_TIMER,
  NYABULA_EYE_SCENE_NETWORK,
  NYABULA_EYE_SCENE_AUDIO,
  NYABULA_EYE_SCENE_EQ,
  NYABULA_EYE_SCENE_CAPTION,
  NYABULA_EYE_SCENE_BRIEFING,
  NYABULA_EYE_SCENE_PRIVACY,
  NYABULA_EYE_SCENE_IDENTITY,
  NYABULA_EYE_SCENE_MEMORY,
  NYABULA_EYE_SCENE_DEVICES,
  NYABULA_EYE_SCENE_SYSTEM,
  NYABULA_EYE_SCENE_HEALTH,
  NYABULA_EYE_SCENE_PRESENCE,
  NYABULA_EYE_SCENE_COMPANION,
  NYABULA_EYE_SCENE_HOME,
  NYABULA_EYE_SCENE_SUBWOOFER,
  NYABULA_EYE_SCENE_COUNT
};

enum nyabula_eye_scene_style_e
{
  NYABULA_EYE_SCENE_STYLE_FULL = 0,
  NYABULA_EYE_SCENE_STYLE_MINIMAL
};

enum nyabula_eye_weather_e
{
  NYABULA_EYE_WEATHER_SUNNY = 0,
  NYABULA_EYE_WEATHER_CLOUDY,
  NYABULA_EYE_WEATHER_RAIN,
  NYABULA_EYE_WEATHER_STORM,
  NYABULA_EYE_WEATHER_SNOW,
  NYABULA_EYE_WEATHER_FOG
};

enum nyabula_eye_music_view_e
{
  NYABULA_EYE_MUSIC_SPECTRUM = 0,
  NYABULA_EYE_MUSIC_LYRICS
};

enum nyabula_eye_battery_state_e
{
  NYABULA_EYE_BATTERY_CHARGING = 0,
  NYABULA_EYE_BATTERY_LOW,
  NYABULA_EYE_BATTERY_FULL,
  NYABULA_EYE_BATTERY_HOT,
  NYABULA_EYE_BATTERY_DOCK
};

enum nyabula_eye_alarm_copy_e
{
  NYABULA_EYE_ALARM_COPY_NAME = 0,
  NYABULA_EYE_ALARM_COPY_REMINDER,
  NYABULA_EYE_ALARM_COPY_NONE
};

enum nyabula_eye_call_state_e
{
  NYABULA_EYE_CALL_INCOMING = 0,
  NYABULA_EYE_CALL_ACTIVE,
  NYABULA_EYE_CALL_ENDED
};

enum nyabula_eye_task_state_e
{
  NYABULA_EYE_TASK_RUNNING = 0,
  NYABULA_EYE_TASK_QUEUED,
  NYABULA_EYE_TASK_CONFIRM,
  NYABULA_EYE_TASK_DONE,
  NYABULA_EYE_TASK_FAILED
};

enum nyabula_eye_network_state_e
{
  NYABULA_EYE_NETWORK_WIFI = 0,
  NYABULA_EYE_NETWORK_BLUETOOTH,
  NYABULA_EYE_NETWORK_OFFLINE
};

enum nyabula_eye_audio_route_e
{
  NYABULA_EYE_AUDIO_SPEAKER = 0,
  NYABULA_EYE_AUDIO_HEADPHONES,
  NYABULA_EYE_AUDIO_BOTH,
  NYABULA_EYE_AUDIO_MUTE
};

enum nyabula_eye_eq_view_e
{
  NYABULA_EYE_EQ_PROFILE = 0,
  NYABULA_EYE_EQ_CALIBRATING
};

#define NYABULA_EYE_TEXT_SHORT  24
#define NYABULA_EYE_TEXT_MEDIUM 48
#define NYABULA_EYE_EQ_BANDS    10

struct nyabula_eye_scene_payload_s
{
  enum nyabula_eye_weather_e weather;
  enum nyabula_eye_music_view_e music_view;
  enum nyabula_eye_battery_state_e battery_state;
  enum nyabula_eye_alarm_copy_e alarm_copy;
  enum nyabula_eye_call_state_e call_state;
  enum nyabula_eye_task_state_e task_state;
  enum nyabula_eye_network_state_e network_state;
  enum nyabula_eye_audio_route_e audio_route;
  enum nyabula_eye_eq_view_e eq_view;
  uint32_t duration_ms;
  uint32_t position_ms;
  uint32_t remaining_ms;
  uint32_t elapsed_ms;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t percent;
  uint8_t device_count;
  uint8_t briefing_index;
  uint8_t briefing_count;
  float temperature_c;
  float feels_like_c;
  float humidity_percent;
  float wind_kph;
  float visibility_km;
  float distance_m;
  float heart_rate_bpm;
  float crossover_hz;
  float progress;
  float eq_bands[NYABULA_EYE_EQ_BANDS];
  bool active;
  bool playing;
  bool privacy_camera;
  bool privacy_microphone;
  bool signal_good;
  char title[NYABULA_EYE_TEXT_MEDIUM];
  char subtitle[NYABULA_EYE_TEXT_MEDIUM];
  char detail[NYABULA_EYE_TEXT_MEDIUM];
  char value[NYABULA_EYE_TEXT_SHORT];
  char previous_line[NYABULA_EYE_TEXT_MEDIUM];
  char current_line[NYABULA_EYE_TEXT_MEDIUM];
  char next_line[NYABULA_EYE_TEXT_MEDIUM];
};

struct nyabula_eye_scene_request_s
{
  enum nyabula_eye_scene_e scene;
  enum nyabula_eye_scene_style_e style;
  struct nyabula_eye_scene_payload_s payload;
};

struct nyabula_eye_engine_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

struct nyabula_eye_engine_s *nyabula_eye_engine_create(lv_obj_t *parent);
struct nyabula_eye_engine_s *
nyabula_eye_engine_create_dual(lv_obj_t *left_parent, lv_obj_t *right_parent);
void nyabula_eye_engine_destroy(struct nyabula_eye_engine_s *engine);

int nyabula_eye_engine_set_expression(struct nyabula_eye_engine_s *engine,
                                      enum nyabula_eye_expression_e expression,
                                      uint32_t transition_ms);

int nyabula_eye_engine_blink(struct nyabula_eye_engine_s *engine,
                             enum nyabula_eye_mask_e eyes);

int nyabula_eye_engine_set_gaze(struct nyabula_eye_engine_s *engine, float x,
                                float y, uint32_t hold_ms);

void nyabula_eye_engine_set_auto_blink(struct nyabula_eye_engine_s *engine,
                                       bool enabled);

void nyabula_eye_engine_set_ambient_light(struct nyabula_eye_engine_s *engine,
                                          float level);

void nyabula_eye_engine_set_iris_color(struct nyabula_eye_engine_s *engine,
                                       enum nyabula_eye_mask_e eyes,
                                       uint32_t rgb);

int nyabula_eye_engine_show_scene(
    struct nyabula_eye_engine_s *engine,
    const struct nyabula_eye_scene_request_s *request);

int nyabula_eye_engine_update_scene(
    struct nyabula_eye_engine_s *engine,
    const struct nyabula_eye_scene_payload_s *payload);

int nyabula_eye_engine_hide_scene(struct nyabula_eye_engine_s *engine);

enum nyabula_eye_scene_e
nyabula_eye_engine_get_scene(const struct nyabula_eye_engine_s *engine);

enum nyabula_eye_expression_e
nyabula_eye_engine_get_expression(const struct nyabula_eye_engine_s *engine);

#ifdef __cplusplus
}
#endif

#endif /* __APP_NYABULA_INCLUDE_NYABULA_EYE_ENGINE_H */
