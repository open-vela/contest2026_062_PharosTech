/****************************************************************************
 * app/nyabula/src/nyabula_eye_engine.c
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

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nyabula_eye_internal.h"

#define NYABULA_EYE_TICK_MS            15
#define NYABULA_EYE_BLINK_DURATION_MS  238
#define NYABULA_EYE_FIRST_BLINK_MS     2500
#define NYABULA_EYE_SLEEP_CLOSE_MS     1800
#define NYABULA_EYE_SACCADE_FIRST_MS   1200
#define NYABULA_EYE_DEFAULT_IRIS_RGB   0x38e06e
#define NYABULA_EYE_DEFAULT_LIGHT      0.55f
#define NYABULA_EYE_MAX_FRAME_DELTA_MS 50
#define NYABULA_EYE_SCENE_CLOSE_MS     380
#define NYABULA_EYE_SCENE_OPEN_MS      520
#define NYABULA_EYE_SCENE_FADE_MS      320

enum nyabula_eye_scene_phase_e
{
  NYABULA_EYE_SCENE_PHASE_NONE = 0,
  NYABULA_EYE_SCENE_PHASE_CLOSING_IN,
  NYABULA_EYE_SCENE_PHASE_CLOSING_SWITCH,
  NYABULA_EYE_SCENE_PHASE_VISIBLE,
  NYABULA_EYE_SCENE_PHASE_CLOSING_OUT,
  NYABULA_EYE_SCENE_PHASE_REOPENING_IN,
  NYABULA_EYE_SCENE_PHASE_REOPENING_OUT
};

struct nyabula_eye_engine_s
{
  struct nyabula_eye_renderer_s *renderer;
  struct nyabula_eye_frame_s current;
  struct nyabula_eye_frame_s target;
  enum nyabula_eye_expression_e expression;
  lv_timer_t *timer;
  uint32_t start_tick;
  uint32_t last_tick;
  uint32_t mode_start;
  uint32_t next_blink;
  uint32_t blink_start;
  uint32_t next_saccade;
  uint32_t gaze_release;
  uint32_t random_state;
  uint32_t iris_rgb[NYABULA_EYE_COUNT];
  enum nyabula_eye_mask_e blink_eyes;
  float explicit_gaze_x;
  float explicit_gaze_y;
  float saccade_x;
  float saccade_y;
  float ambient_light;
  float sleep_start_top;
  float sleep_start_bottom;
  float sleep_end_top;
  float sleep_end_bottom;
  float zzz_mask;
  struct nyabula_eye_scene_request_s scene;
  struct nyabula_eye_scene_request_s previous_scene;
  struct nyabula_eye_scene_request_s pending_scene;
  struct nyabula_eye_scene_payload_s payload_from;
  enum nyabula_eye_scene_phase_e scene_phase;
  uint32_t scene_phase_start;
  uint32_t scene_start;
  uint32_t previous_scene_start;
  uint32_t scene_fade_start;
  uint32_t payload_animation_start;
  float scene_lid;
  bool blink_active;
  bool auto_blink;
  bool explicit_gaze;
  bool scene_pending;
  bool payload_animation_active;
};

static float nyabula_eye_engine_clamp(float value, float minimum,
                                      float maximum);
static float nyabula_eye_engine_lerp(float from, float to, float progress);
static float nyabula_eye_engine_bezier(float progress);
static bool nyabula_eye_engine_payload_visual_change(
    const struct nyabula_eye_scene_payload_s *from,
    const struct nyabula_eye_scene_payload_s *to);
static uint32_t nyabula_eye_engine_lerp_u32(uint32_t from, uint32_t to,
                                            float progress);
static void nyabula_eye_engine_interpolate_payload(
    struct nyabula_eye_engine_s *engine,
    struct nyabula_eye_scene_payload_s *payload);
static float nyabula_eye_engine_follow(float current, float target,
                                       float speed, float delta_seconds);
static uint32_t nyabula_eye_engine_random(struct nyabula_eye_engine_s *engine);
static float
nyabula_eye_engine_random_unit(struct nyabula_eye_engine_s *engine);
static void
nyabula_eye_engine_reset_target(struct nyabula_eye_engine_s *engine);
static void
nyabula_eye_engine_apply_expression(struct nyabula_eye_engine_s *engine,
                                    float mode_seconds);
static void
nyabula_eye_engine_follow_target(struct nyabula_eye_engine_s *engine,
                                 float delta_seconds);
static void
nyabula_eye_engine_update_saccade(struct nyabula_eye_engine_s *engine,
                                  uint32_t elapsed, float global_seconds);
static void
nyabula_eye_engine_start_auto_blink(struct nyabula_eye_engine_s *engine,
                                    uint32_t elapsed);
static float
nyabula_eye_engine_blink_amount(struct nyabula_eye_engine_s *engine);
static void nyabula_eye_engine_apply_blink(struct nyabula_eye_frame_s *frame,
                                           float amount);
static float nyabula_eye_engine_scene_progress(uint32_t started,
                                               uint32_t duration_ms);
static void
nyabula_eye_engine_finish_scene_close(struct nyabula_eye_engine_s *engine);
static void
nyabula_eye_engine_update_scene_frame(struct nyabula_eye_engine_s *engine,
                                      struct nyabula_eye_scene_frame_s *frame);
static void nyabula_eye_engine_animation_cb(lv_timer_t *timer);

static float nyabula_eye_engine_clamp(float value, float minimum,
                                      float maximum)
{
  if (value < minimum)
    {
      return minimum;
    }

  if (value > maximum)
    {
      return maximum;
    }

  return value;
}

static float nyabula_eye_engine_lerp(float from, float to, float progress)
{
  return from + (to - from) * progress;
}

static float nyabula_eye_engine_bezier(float progress)
{
  float t = nyabula_eye_engine_clamp(progress, 0.0f, 1.0f);
  float u = 1.0f - t;

  return 3.0f * u * u * t * 0.08f + 3.0f * u * t * t * 0.92f + t * t * t;
}

static bool nyabula_eye_engine_payload_visual_change(
    const struct nyabula_eye_scene_payload_s *from,
    const struct nyabula_eye_scene_payload_s *to)
{
  return from->weather != to->weather || from->music_view != to->music_view ||
         from->battery_state != to->battery_state ||
         from->alarm_copy != to->alarm_copy ||
         from->call_state != to->call_state ||
         from->task_state != to->task_state ||
         from->network_state != to->network_state ||
         from->audio_route != to->audio_route ||
         from->eq_view != to->eq_view || from->year != to->year ||
         from->month != to->month || from->day != to->day ||
         from->hour != to->hour || from->minute != to->minute ||
         from->active != to->active || from->playing != to->playing ||
         from->privacy_camera != to->privacy_camera ||
         from->privacy_microphone != to->privacy_microphone ||
         from->signal_good != to->signal_good ||
         strcmp(from->title, to->title) != 0 ||
         strcmp(from->subtitle, to->subtitle) != 0 ||
         strcmp(from->detail, to->detail) != 0 ||
         strcmp(from->value, to->value) != 0 ||
         strcmp(from->previous_line, to->previous_line) != 0 ||
         strcmp(from->current_line, to->current_line) != 0 ||
         strcmp(from->next_line, to->next_line) != 0;
}

static uint32_t nyabula_eye_engine_lerp_u32(uint32_t from, uint32_t to,
                                            float progress)
{
  return (uint32_t)lroundf(
      nyabula_eye_engine_lerp((float)from, (float)to, progress));
}

static void nyabula_eye_engine_interpolate_payload(
    struct nyabula_eye_engine_s *engine,
    struct nyabula_eye_scene_payload_s *payload)
{
  const struct nyabula_eye_scene_payload_s *from = &engine->payload_from;
  const struct nyabula_eye_scene_payload_s *to = &engine->scene.payload;
  float progress;
  int index;

  *payload = *to;
  if (!engine->payload_animation_active)
    {
      return;
    }

  progress = nyabula_eye_engine_bezier(nyabula_eye_engine_clamp(
      (float)lv_tick_elaps(engine->payload_animation_start) /
          NYABULA_EYE_SCENE_FADE_MS,
      0.0f, 1.0f));
  payload->duration_ms = nyabula_eye_engine_lerp_u32(
      from->duration_ms, to->duration_ms, progress);
  payload->position_ms = nyabula_eye_engine_lerp_u32(
      from->position_ms, to->position_ms, progress);
  payload->remaining_ms = nyabula_eye_engine_lerp_u32(
      from->remaining_ms, to->remaining_ms, progress);
  payload->elapsed_ms =
      nyabula_eye_engine_lerp_u32(from->elapsed_ms, to->elapsed_ms, progress);
  payload->percent = (uint8_t)nyabula_eye_engine_lerp_u32(
      from->percent, to->percent, progress);
  payload->device_count = (uint8_t)nyabula_eye_engine_lerp_u32(
      from->device_count, to->device_count, progress);
  payload->briefing_index = (uint8_t)nyabula_eye_engine_lerp_u32(
      from->briefing_index, to->briefing_index, progress);
  payload->briefing_count = (uint8_t)nyabula_eye_engine_lerp_u32(
      from->briefing_count, to->briefing_count, progress);
  payload->temperature_c = nyabula_eye_engine_lerp(
      from->temperature_c, to->temperature_c, progress);
  payload->feels_like_c =
      nyabula_eye_engine_lerp(from->feels_like_c, to->feels_like_c, progress);
  payload->humidity_percent = nyabula_eye_engine_lerp(
      from->humidity_percent, to->humidity_percent, progress);
  payload->wind_kph =
      nyabula_eye_engine_lerp(from->wind_kph, to->wind_kph, progress);
  payload->visibility_km = nyabula_eye_engine_lerp(
      from->visibility_km, to->visibility_km, progress);
  payload->distance_m =
      nyabula_eye_engine_lerp(from->distance_m, to->distance_m, progress);
  payload->heart_rate_bpm = nyabula_eye_engine_lerp(
      from->heart_rate_bpm, to->heart_rate_bpm, progress);
  payload->crossover_hz =
      nyabula_eye_engine_lerp(from->crossover_hz, to->crossover_hz, progress);
  payload->progress =
      nyabula_eye_engine_lerp(from->progress, to->progress, progress);
  for (index = 0; index < NYABULA_EYE_EQ_BANDS; index++)
    {
      payload->eq_bands[index] = nyabula_eye_engine_lerp(
          from->eq_bands[index], to->eq_bands[index], progress);
    }

  if (progress >= 1.0f)
    {
      engine->payload_animation_active = false;
    }
}

static float nyabula_eye_engine_follow(float current, float target,
                                       float speed, float delta_seconds)
{
  float next = nyabula_eye_engine_lerp(current, target,
                                       1.0f - expf(-speed * delta_seconds));

  /* Stop subpixel interpolation tails once their largest possible screen
   * displacement is well below the antialiasing sample resolution. */

  return fabsf(target - next) < 0.00001f ? target : next;
}

static uint32_t nyabula_eye_engine_random(struct nyabula_eye_engine_s *engine)
{
  engine->random_state = engine->random_state * 1664525u + 1013904223u;
  return engine->random_state;
}

static float
nyabula_eye_engine_random_unit(struct nyabula_eye_engine_s *engine)
{
  return (float)(nyabula_eye_engine_random(engine) >> 8) / 16777215.0f;
}

static void
nyabula_eye_engine_reset_target(struct nyabula_eye_engine_s *engine)
{
  float darkness = 1.0f - engine->ambient_light;
  float pupil_mix = darkness * darkness * 0.3f + darkness * 0.7f;

  memset(&engine->target, 0, sizeof(engine->target));
  engine->target.pupil_width =
      nyabula_eye_engine_lerp(0.10f, 0.95f, pupil_mix);
  engine->target.pupil_height =
      nyabula_eye_engine_lerp(0.72f, 0.95f, darkness);
  engine->target.iris_scale = 1.0f;
  engine->target.glow = 0.5f;
}

static void
nyabula_eye_engine_apply_expression(struct nyabula_eye_engine_s *engine,
                                    float mode_seconds)
{
  struct nyabula_eye_frame_s *frame = &engine->target;

  nyabula_eye_engine_reset_target(engine);

  switch (engine->expression)
    {
      case NYABULA_EYE_EXPRESSION_IDLE:
        frame->pupil_width *= 1.0f + sinf(mode_seconds * 1.1f) * 0.04f;
        break;

      case NYABULA_EYE_EXPRESSION_CURIOUS:
        frame->pupil_width = fminf(0.9f, frame->pupil_width * 1.5f + 0.25f);
        frame->pupil_height = 0.95f;
        frame->iris_scale = 1.06f;
        frame->glow = 0.75f;
        break;

      case NYABULA_EYE_EXPRESSION_HAPPY:
        frame->bottom_curve = 1.0f;
        frame->lid_bottom = 0.28f;
        frame->squint = 0.15f;
        frame->pupil_width = fminf(0.85f, frame->pupil_width + 0.2f);
        frame->glow = 0.85f;
        break;

      case NYABULA_EYE_EXPRESSION_PROCESSING:
        frame->pupil_width = 0.95f;
        frame->pupil_height = 0.95f;
        frame->overlay = 1.0f;
        frame->glow = 0.6f;
        break;

      case NYABULA_EYE_EXPRESSION_STAR:
      case NYABULA_EYE_EXPRESSION_HEART:
        frame->pupil_width = 0.0f;
        frame->pupil_height = 0.0f;
        frame->overlay = 1.0f;
        frame->iris_scale = 1.08f;
        frame->glow = 1.0f;
        frame->bottom_curve = 0.5f;
        frame->lid_bottom = 0.1f;
        break;

      case NYABULA_EYE_EXPRESSION_SLEEPY:
        frame->lid_top = 0.55f;
        frame->lid_bottom = 0.15f;
        frame->pupil_width = fmaxf(frame->pupil_width, 0.5f);
        frame->gaze_y = 0.25f;
        frame->glow = 0.3f;
        break;

      case NYABULA_EYE_EXPRESSION_SLEEP:
        {
          float close = nyabula_eye_engine_bezier(nyabula_eye_engine_clamp(
              mode_seconds * 1000.0f / NYABULA_EYE_SLEEP_CLOSE_MS, 0.0f,
              1.0f));

          frame->lid_top = nyabula_eye_engine_lerp(
              engine->sleep_start_top, engine->sleep_end_top, close);
          frame->lid_bottom = nyabula_eye_engine_lerp(
              engine->sleep_start_bottom, engine->sleep_end_bottom, close);
          frame->glow = nyabula_eye_engine_lerp(
              0.4f, 0.15f, fminf(1.0f, mode_seconds / 2.0f));
          frame->overlay = close > 0.98f ? 1.0f : 0.0f;
          frame->gaze_y = 0.2f;
        }
        break;

      case NYABULA_EYE_EXPRESSION_ANGRY:
        frame->lid_top = 0.32f;
        frame->lid_slant = 1.0f;
        frame->pupil_width = 0.13f;
        frame->pupil_height = 0.8f;
        frame->squint = 0.2f;
        frame->glow = 0.7f;
        break;

      case NYABULA_EYE_EXPRESSION_SAD:
        frame->lid_top = 0.3f;
        frame->lid_slant = -0.9f;
        frame->lid_bottom = 0.12f;
        frame->pupil_width = fminf(0.9f, frame->pupil_width + 0.3f);
        frame->gaze_y = 0.3f;
        frame->glow = 0.35f;
        frame->overlay = mode_seconds > 1.2f ? 1.0f : 0.0f;
        break;

      case NYABULA_EYE_EXPRESSION_SURPRISE:
        frame->pupil_width = 0.98f;
        frame->pupil_height = 0.98f;
        frame->iris_scale = mode_seconds < 0.25f ? 1.2f : 1.14f;
        frame->glow = 1.0f;
        break;

      case NYABULA_EYE_EXPRESSION_DIZZY:
        frame->pupil_width = 0.0f;
        frame->pupil_height = 0.0f;
        frame->overlay = 1.0f;
        frame->lid_top = 0.15f;
        frame->lid_slant = 0.3f;
        break;

      case NYABULA_EYE_EXPRESSION_DERP:
        frame->derp = 1.0f;
        frame->pupil_width = 0.8f;
        frame->pupil_height = 0.86f;
        frame->lid_top = 0.0f;
        frame->glow = 0.45f;
        frame->gaze_x = sinf(mode_seconds * 0.7f) * 0.05f;
        break;

      default:
        break;
    }
}

static void
nyabula_eye_engine_follow_target(struct nyabula_eye_engine_s *engine,
                                 float delta_seconds)
{
  struct nyabula_eye_frame_s *current = &engine->current;
  const struct nyabula_eye_frame_s *target = &engine->target;

#define FOLLOW(field, speed)                                                \
  current->field = nyabula_eye_engine_follow(current->field, target->field, \
                                             speed, delta_seconds)

  FOLLOW(pupil_width, 8.0f);
  FOLLOW(pupil_height, 8.0f);
  FOLLOW(lid_top, 22.0f);
  FOLLOW(lid_bottom, 22.0f);
  FOLLOW(lid_slant, 6.0f);
  FOLLOW(bottom_curve, 6.0f);
  FOLLOW(gaze_x, 10.0f);
  FOLLOW(gaze_y, 10.0f);
  FOLLOW(iris_scale, 7.0f);
  FOLLOW(glow, 4.0f);
  FOLLOW(overlay, 6.0f);
  FOLLOW(squint, 8.0f);
  FOLLOW(derp, 5.0f);

#undef FOLLOW
}

static void
nyabula_eye_engine_update_saccade(struct nyabula_eye_engine_s *engine,
                                  uint32_t elapsed, float global_seconds)
{
  if (engine->explicit_gaze && elapsed >= engine->gaze_release)
    {
      engine->explicit_gaze = false;
    }

  if (!engine->explicit_gaze &&
      (engine->expression == NYABULA_EYE_EXPRESSION_IDLE ||
       engine->expression == NYABULA_EYE_EXPRESSION_CURIOUS ||
       engine->expression == NYABULA_EYE_EXPRESSION_HAPPY) &&
      elapsed >= engine->next_saccade)
    {
      engine->next_saccade =
          elapsed + 1200 +
          (uint32_t)(nyabula_eye_engine_random_unit(engine) * 2800.0f);
      engine->saccade_x =
          (nyabula_eye_engine_random_unit(engine) * 2.0f - 1.0f) * 0.55f;
      engine->saccade_y =
          (nyabula_eye_engine_random_unit(engine) * 2.0f - 1.0f) * 0.35f;
      if (nyabula_eye_engine_random_unit(engine) < 0.3f)
        {
          engine->saccade_x = 0.0f;
          engine->saccade_y = 0.0f;
        }
    }

  if (engine->expression == NYABULA_EYE_EXPRESSION_SLEEPY)
    {
      engine->saccade_x = sinf(global_seconds * 0.4f) * 0.15f;
      engine->saccade_y = 0.2f;
    }
  else if (engine->expression == NYABULA_EYE_EXPRESSION_PROCESSING)
    {
      engine->saccade_x = 0.0f;
      engine->saccade_y = -0.1f;
    }
  else if (engine->expression == NYABULA_EYE_EXPRESSION_DIZZY ||
           engine->expression == NYABULA_EYE_EXPRESSION_DERP)
    {
      engine->saccade_x = 0.0f;
      engine->saccade_y = 0.0f;
    }
}

static void
nyabula_eye_engine_start_auto_blink(struct nyabula_eye_engine_s *engine,
                                    uint32_t elapsed)
{
  float random_delay =
      2200.0f + nyabula_eye_engine_random_unit(engine) * 4500.0f;

  engine->blink_active = true;
  engine->blink_eyes = NYABULA_EYE_MASK_BOTH;
  engine->blink_start = lv_tick_get();
  if (nyabula_eye_engine_random_unit(engine) < 0.18f)
    {
      random_delay = 350.0f;
    }

  engine->next_blink = elapsed + (uint32_t)random_delay;
}

static float
nyabula_eye_engine_blink_amount(struct nyabula_eye_engine_s *engine)
{
  uint32_t elapsed;
  float progress;

  if (!engine->blink_active)
    {
      return 0.0f;
    }

  elapsed = lv_tick_elaps(engine->blink_start);
  if (elapsed >= NYABULA_EYE_BLINK_DURATION_MS)
    {
      engine->blink_active = false;
      return 0.0f;
    }

  progress = (float)elapsed / (float)NYABULA_EYE_BLINK_DURATION_MS;
  if (progress < 0.46f)
    {
      return nyabula_eye_engine_bezier(progress / 0.46f);
    }

  return 1.0f - nyabula_eye_engine_bezier((progress - 0.46f) / 0.54f);
}

static void nyabula_eye_engine_apply_blink(struct nyabula_eye_frame_s *frame,
                                           float amount)
{
  float lower_target = fmaxf(0.14f, frame->lid_bottom);
  float upper_target = fmaxf(frame->lid_top, 1.0f - lower_target);

  frame->lid_top =
      nyabula_eye_engine_lerp(frame->lid_top, upper_target, amount);
  frame->lid_bottom =
      nyabula_eye_engine_lerp(frame->lid_bottom, lower_target, amount);
  frame->lid_slant *= 1.0f - amount;
  frame->bottom_curve *= 1.0f - amount;
}

static float nyabula_eye_engine_scene_progress(uint32_t started,
                                               uint32_t duration_ms)
{
  return nyabula_eye_engine_bezier(nyabula_eye_engine_clamp(
      (float)lv_tick_elaps(started) / (float)duration_ms, 0.0f, 1.0f));
}

static void
nyabula_eye_engine_finish_scene_close(struct nyabula_eye_engine_s *engine)
{
  uint32_t now = lv_tick_get();

  if (engine->scene_phase == NYABULA_EYE_SCENE_PHASE_CLOSING_OUT)
    {
      memset(&engine->previous_scene, 0, sizeof(engine->previous_scene));
      memset(&engine->scene, 0, sizeof(engine->scene));
      engine->scene_phase = NYABULA_EYE_SCENE_PHASE_REOPENING_OUT;
    }
  else
    {
      memset(&engine->previous_scene, 0, sizeof(engine->previous_scene));
      engine->scene = engine->pending_scene;
      engine->scene_start = now;
      engine->scene_fade_start = now;
      engine->scene_pending = false;
      engine->scene_phase =
          engine->scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL
              ? NYABULA_EYE_SCENE_PHASE_VISIBLE
              : NYABULA_EYE_SCENE_PHASE_REOPENING_IN;
    }

  engine->scene_phase_start = now;
}

static void
nyabula_eye_engine_update_scene_frame(struct nyabula_eye_engine_s *engine,
                                      struct nyabula_eye_scene_frame_s *frame)
{
  uint32_t fade_elapsed;
  float progress;

  if (engine->scene_phase == NYABULA_EYE_SCENE_PHASE_CLOSING_IN ||
      engine->scene_phase == NYABULA_EYE_SCENE_PHASE_CLOSING_SWITCH ||
      engine->scene_phase == NYABULA_EYE_SCENE_PHASE_CLOSING_OUT)
    {
      progress = nyabula_eye_engine_scene_progress(engine->scene_phase_start,
                                                   NYABULA_EYE_SCENE_CLOSE_MS);
      engine->scene_lid = progress;
      if (progress >= 1.0f)
        {
          nyabula_eye_engine_finish_scene_close(engine);
        }
    }
  else if (engine->scene_phase == NYABULA_EYE_SCENE_PHASE_REOPENING_IN ||
           engine->scene_phase == NYABULA_EYE_SCENE_PHASE_REOPENING_OUT)
    {
      progress = nyabula_eye_engine_scene_progress(engine->scene_phase_start,
                                                   NYABULA_EYE_SCENE_OPEN_MS);
      engine->scene_lid = 1.0f - progress;
      if (progress >= 1.0f)
        {
          bool keep_scene =
              engine->scene_phase == NYABULA_EYE_SCENE_PHASE_REOPENING_IN;
          if (!keep_scene)
            {
              memset(&engine->scene, 0, sizeof(engine->scene));
              memset(&engine->previous_scene, 0,
                     sizeof(engine->previous_scene));
            }

          engine->scene_phase = keep_scene ? NYABULA_EYE_SCENE_PHASE_VISIBLE
                                           : NYABULA_EYE_SCENE_PHASE_NONE;
          engine->scene_lid = 0.0f;
        }
    }

  memset(frame, 0, sizeof(*frame));
  frame->scene = engine->scene.scene;
  frame->previous_scene = engine->previous_scene.scene;
  frame->style = engine->scene.style;
  frame->previous_style = engine->previous_scene.style;
  frame->payload = engine->scene.payload;
  frame->previous_payload = engine->previous_scene.payload;
  nyabula_eye_engine_interpolate_payload(engine, &frame->payload);
  frame->lid = engine->scene_lid;
  frame->scene_seconds = (float)lv_tick_elaps(engine->scene_start) / 1000.0f;
  frame->previous_scene_seconds =
      (float)lv_tick_elaps(engine->previous_scene_start) / 1000.0f;
  frame->reveal = nyabula_eye_engine_scene_progress(engine->scene_start, 720);
  if (engine->scene_phase == NYABULA_EYE_SCENE_PHASE_VISIBLE &&
      engine->previous_scene.scene != NYABULA_EYE_SCENE_NONE)
    {
      fade_elapsed = lv_tick_elaps(engine->scene_fade_start);
      frame->alpha = nyabula_eye_engine_bezier(nyabula_eye_engine_clamp(
          (float)fade_elapsed / NYABULA_EYE_SCENE_FADE_MS, 0.0f, 1.0f));
      frame->previous_alpha = 1.0f - frame->alpha;
      if (frame->alpha >= 1.0f)
        {
          memset(&engine->previous_scene, 0, sizeof(engine->previous_scene));
          frame->previous_scene = NYABULA_EYE_SCENE_NONE;
          frame->previous_alpha = 0.0f;
        }
    }
  else if (engine->scene_phase == NYABULA_EYE_SCENE_PHASE_REOPENING_OUT &&
           engine->scene.scene != NYABULA_EYE_SCENE_NONE &&
           engine->scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL)
    {
      fade_elapsed = lv_tick_elaps(engine->scene_phase_start);
      frame->alpha =
          1.0f - nyabula_eye_engine_bezier(nyabula_eye_engine_clamp(
                     (float)fade_elapsed / (NYABULA_EYE_SCENE_OPEN_MS * 0.86f),
                     0.0f, 1.0f));
      frame->previous_alpha = 0.0f;
    }
  else if (engine->scene_phase == NYABULA_EYE_SCENE_PHASE_VISIBLE &&
           engine->scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL)
    {
      fade_elapsed = lv_tick_elaps(engine->scene_fade_start);
      frame->alpha = nyabula_eye_engine_bezier(nyabula_eye_engine_clamp(
          (float)fade_elapsed / NYABULA_EYE_SCENE_FADE_MS, 0.0f, 1.0f));
      frame->previous_alpha = 0.0f;
    }
  else
    {
      frame->alpha = 1.0f;
      frame->previous_alpha = 0.0f;
    }
}

static void nyabula_eye_engine_animation_cb(lv_timer_t *timer)
{
  struct nyabula_eye_engine_s *engine = lv_timer_get_user_data(timer);
  struct nyabula_eye_frame_s frames[NYABULA_EYE_COUNT];
  struct nyabula_eye_scene_frame_s scene_frame;
  uint32_t now = lv_tick_get();
  uint32_t delta_ms = lv_tick_elaps(engine->last_tick);
  uint32_t elapsed = lv_tick_elaps(engine->start_tick);
  float delta_seconds;
  float global_seconds = (float)elapsed / 1000.0f;
  float mode_seconds = (float)lv_tick_elaps(engine->mode_start) / 1000.0f;
  float blink;
  float zzz_target;
  int eye_id;

  if (delta_ms > NYABULA_EYE_MAX_FRAME_DELTA_MS)
    {
      delta_ms = NYABULA_EYE_MAX_FRAME_DELTA_MS;
    }

  engine->last_tick = now;
  delta_seconds = (float)delta_ms / 1000.0f;
  nyabula_eye_engine_apply_expression(engine, mode_seconds);
  nyabula_eye_engine_update_saccade(engine, elapsed, global_seconds);
  nyabula_eye_engine_update_scene_frame(engine, &scene_frame);

  if (engine->explicit_gaze)
    {
      engine->target.gaze_x = engine->explicit_gaze_x;
      engine->target.gaze_y = engine->explicit_gaze_y;
    }
  else
    {
      engine->target.gaze_x = nyabula_eye_engine_clamp(
          engine->target.gaze_x + engine->saccade_x, -1.0f, 1.0f);
      engine->target.gaze_y = nyabula_eye_engine_clamp(
          engine->target.gaze_y + engine->saccade_y, -1.0f, 1.0f);
    }

  nyabula_eye_engine_follow_target(engine, delta_seconds);
  if (engine->auto_blink && !engine->blink_active &&
      engine->scene_phase == NYABULA_EYE_SCENE_PHASE_NONE &&
      engine->expression != NYABULA_EYE_EXPRESSION_SLEEP &&
      elapsed >= engine->next_blink)
    {
      nyabula_eye_engine_start_auto_blink(engine, elapsed);
    }

  blink = nyabula_eye_engine_blink_amount(engine);
  zzz_target = engine->expression == NYABULA_EYE_EXPRESSION_SLEEP
                   ? nyabula_eye_engine_clamp(engine->current.lid_top +
                                                  engine->current.lid_bottom,
                                              0.0f, 1.0f)
                   : 0.0f;
  engine->zzz_mask = nyabula_eye_engine_follow(engine->zzz_mask, zzz_target,
                                               22.0f, delta_seconds);

  for (eye_id = 0; eye_id < NYABULA_EYE_COUNT; eye_id++)
    {
      frames[eye_id] = engine->current;
      frames[eye_id].expression = engine->expression;
      frames[eye_id].global_seconds = global_seconds;
      frames[eye_id].mode_seconds = mode_seconds;
      frames[eye_id].zzz_mask = engine->zzz_mask;
      frames[eye_id].iris_rgb = engine->iris_rgb[eye_id];
      frames[eye_id].scene = scene_frame;
      if (engine->blink_active && (engine->blink_eyes & (1 << eye_id)) != 0)
        {
          nyabula_eye_engine_apply_blink(&frames[eye_id], blink);
        }
    }

  nyabula_eye_renderer_render(engine->renderer, frames);
}

struct nyabula_eye_engine_s *
nyabula_eye_engine_create_dual(lv_obj_t *left_parent, lv_obj_t *right_parent)
{
  struct nyabula_eye_engine_s *engine;
  int eye_id;

  if (left_parent == NULL || right_parent == NULL)
    {
      return NULL;
    }

  engine = calloc(1, sizeof(*engine));
  if (engine == NULL)
    {
      return NULL;
    }

  engine->renderer = nyabula_eye_renderer_create(left_parent, right_parent);
  if (engine->renderer == NULL)
    {
      free(engine);
      return NULL;
    }

  engine->expression = NYABULA_EYE_EXPRESSION_IDLE;
  engine->ambient_light = NYABULA_EYE_DEFAULT_LIGHT;
  engine->auto_blink = true;
  engine->blink_eyes = NYABULA_EYE_MASK_BOTH;
  engine->random_state = 0x4e796162u;
  engine->sleep_end_top = 0.86f;
  engine->sleep_end_bottom = 0.14f;
  engine->start_tick = lv_tick_get();
  engine->last_tick = engine->start_tick;
  engine->mode_start = engine->start_tick;
  engine->scene_start = engine->start_tick;
  engine->previous_scene_start = engine->start_tick;
  engine->scene_fade_start = engine->start_tick;
  engine->scene_phase_start = engine->start_tick;
  engine->next_blink = NYABULA_EYE_FIRST_BLINK_MS;
  engine->next_saccade = NYABULA_EYE_SACCADE_FIRST_MS;

  for (eye_id = 0; eye_id < NYABULA_EYE_COUNT; eye_id++)
    {
      engine->iris_rgb[eye_id] = NYABULA_EYE_DEFAULT_IRIS_RGB;
    }

  nyabula_eye_engine_apply_expression(engine, 0.0f);
  engine->current = engine->target;
  engine->timer = lv_timer_create(nyabula_eye_engine_animation_cb,
                                  NYABULA_EYE_TICK_MS, engine);
  if (engine->timer == NULL)
    {
      nyabula_eye_engine_destroy(engine);
      return NULL;
    }

  nyabula_eye_engine_animation_cb(engine->timer);
  return engine;
}

struct nyabula_eye_engine_s *nyabula_eye_engine_create(lv_obj_t *parent)
{
  return nyabula_eye_engine_create_dual(parent, parent);
}

void nyabula_eye_engine_destroy(struct nyabula_eye_engine_s *engine)
{
  if (engine == NULL)
    {
      return;
    }

  if (engine->timer != NULL)
    {
      lv_timer_delete(engine->timer);
    }

  nyabula_eye_renderer_destroy(engine->renderer);
  free(engine);
}

int nyabula_eye_engine_set_expression(struct nyabula_eye_engine_s *engine,
                                      enum nyabula_eye_expression_e expression,
                                      uint32_t transition_ms)
{
  enum nyabula_eye_expression_e previous;

  if (engine == NULL || expression < NYABULA_EYE_EXPRESSION_IDLE ||
      expression >= NYABULA_EYE_EXPRESSION_COUNT)
    {
      return -EINVAL;
    }

  previous = engine->expression;
  if (expression == NYABULA_EYE_EXPRESSION_SLEEP &&
      previous != NYABULA_EYE_EXPRESSION_SLEEP)
    {
      engine->sleep_start_top = engine->current.lid_top;
      engine->sleep_start_bottom = engine->current.lid_bottom;
      engine->sleep_end_bottom = fmaxf(0.14f, engine->sleep_start_bottom);
      engine->sleep_end_top =
          fmaxf(engine->sleep_start_top, 1.0f - engine->sleep_end_bottom);
    }

  engine->expression = expression;
  engine->mode_start = lv_tick_get();
  if (expression == NYABULA_EYE_EXPRESSION_SURPRISE)
    {
      engine->blink_active = false;
      engine->next_blink = lv_tick_elaps(engine->start_tick) + 3000;
    }

  /* The reference uses property-specific exponential following. */

  (void)transition_ms;
  return 0;
}

int nyabula_eye_engine_blink(struct nyabula_eye_engine_s *engine,
                             enum nyabula_eye_mask_e eyes)
{
  uint32_t elapsed;

  if (engine == NULL || eyes == 0 || (eyes & ~NYABULA_EYE_MASK_BOTH) != 0)
    {
      return -EINVAL;
    }

  elapsed = lv_tick_elaps(engine->start_tick);
  engine->blink_active = true;
  engine->blink_eyes = eyes;
  engine->blink_start = lv_tick_get();
  engine->next_blink =
      elapsed + 2200 +
      (uint32_t)(nyabula_eye_engine_random_unit(engine) * 4500.0f);
  return 0;
}

int nyabula_eye_engine_set_gaze(struct nyabula_eye_engine_s *engine, float x,
                                float y, uint32_t hold_ms)
{
  if (engine == NULL)
    {
      return -EINVAL;
    }

  engine->explicit_gaze_x = nyabula_eye_engine_clamp(x, -1.0f, 1.0f);
  engine->explicit_gaze_y = nyabula_eye_engine_clamp(y, -1.0f, 1.0f);
  engine->gaze_release = lv_tick_elaps(engine->start_tick) + hold_ms;
  engine->explicit_gaze = hold_ms != 0;
  return 0;
}

void nyabula_eye_engine_set_auto_blink(struct nyabula_eye_engine_s *engine,
                                       bool enabled)
{
  if (engine != NULL)
    {
      engine->auto_blink = enabled;
    }
}

void nyabula_eye_engine_set_ambient_light(struct nyabula_eye_engine_s *engine,
                                          float level)
{
  if (engine != NULL)
    {
      engine->ambient_light = nyabula_eye_engine_clamp(level, 0.0f, 1.0f);
    }
}

void nyabula_eye_engine_set_iris_color(struct nyabula_eye_engine_s *engine,
                                       enum nyabula_eye_mask_e eyes,
                                       uint32_t rgb)
{
  int eye_id;

  if (engine == NULL || eyes == 0 || (eyes & ~NYABULA_EYE_MASK_BOTH) != 0)
    {
      return;
    }

  for (eye_id = 0; eye_id < NYABULA_EYE_COUNT; eye_id++)
    {
      if ((eyes & (1 << eye_id)) != 0)
        {
          engine->iris_rgb[eye_id] = rgb & 0xffffffu;
        }
    }
}

int nyabula_eye_engine_show_scene(
    struct nyabula_eye_engine_s *engine,
    const struct nyabula_eye_scene_request_s *request)
{
  uint32_t now;

  if (engine == NULL || request == NULL ||
      request->scene <= NYABULA_EYE_SCENE_NONE ||
      request->scene >= NYABULA_EYE_SCENE_COUNT ||
      request->style > NYABULA_EYE_SCENE_STYLE_MINIMAL)
    {
      return -EINVAL;
    }

  now = lv_tick_get();
  if (engine->expression == NYABULA_EYE_EXPRESSION_SLEEP)
    {
      engine->expression = NYABULA_EYE_EXPRESSION_IDLE;
      engine->mode_start = now;
      memset(&engine->previous_scene, 0, sizeof(engine->previous_scene));
      engine->scene = *request;
      engine->scene_start = now;
      engine->scene_fade_start = now;
      engine->scene_pending = false;
      engine->scene_phase = request->style == NYABULA_EYE_SCENE_STYLE_MINIMAL
                                ? NYABULA_EYE_SCENE_PHASE_VISIBLE
                                : NYABULA_EYE_SCENE_PHASE_REOPENING_IN;
      engine->scene_phase_start = now;
      engine->scene_lid = 1.0f;
      engine->blink_active = false;
      return 0;
    }

  if (engine->scene.scene != NYABULA_EYE_SCENE_NONE &&
      engine->scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL &&
      request->style == NYABULA_EYE_SCENE_STYLE_MINIMAL &&
      engine->scene_phase == NYABULA_EYE_SCENE_PHASE_VISIBLE)
    {
      engine->previous_scene = engine->scene;
      engine->previous_scene_start = engine->scene_start;
      engine->scene = *request;
      engine->scene_start = now;
      engine->scene_fade_start = now;
      return 0;
    }

  if (engine->scene.scene != NYABULA_EYE_SCENE_NONE &&
      engine->scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL &&
      engine->scene_phase == NYABULA_EYE_SCENE_PHASE_VISIBLE)
    {
      memset(&engine->previous_scene, 0, sizeof(engine->previous_scene));
      engine->scene = *request;
      engine->scene_start = now;
      engine->scene_fade_start = now;
      engine->scene_pending = false;
      engine->scene_phase = request->style == NYABULA_EYE_SCENE_STYLE_MINIMAL
                                ? NYABULA_EYE_SCENE_PHASE_VISIBLE
                                : NYABULA_EYE_SCENE_PHASE_REOPENING_IN;
      engine->scene_phase_start = now;
      engine->scene_lid = 1.0f;
      engine->blink_active = false;
      return 0;
    }

  engine->pending_scene = *request;
  engine->scene_pending = true;
  engine->scene_phase = engine->scene.scene == NYABULA_EYE_SCENE_NONE
                            ? NYABULA_EYE_SCENE_PHASE_CLOSING_IN
                            : NYABULA_EYE_SCENE_PHASE_CLOSING_SWITCH;
  engine->scene_phase_start = now;
  engine->blink_active = false;
  return 0;
}

int nyabula_eye_engine_update_scene(
    struct nyabula_eye_engine_s *engine,
    const struct nyabula_eye_scene_payload_s *payload)
{
  struct nyabula_eye_scene_payload_s displayed;
  uint32_t now;

  if (engine == NULL || payload == NULL ||
      engine->scene.scene == NYABULA_EYE_SCENE_NONE)
    {
      return -EINVAL;
    }

  now = lv_tick_get();
  nyabula_eye_engine_interpolate_payload(engine, &displayed);
  if (nyabula_eye_engine_payload_visual_change(&displayed, payload))
    {
      engine->previous_scene = engine->scene;
      engine->previous_scene.payload = displayed;
      engine->previous_scene_start = engine->scene_start;
      engine->scene_fade_start = now;
      engine->payload_animation_active = false;
    }
  else
    {
      engine->payload_from = displayed;
      engine->payload_animation_start = now;
      engine->payload_animation_active = true;
    }

  engine->scene.payload = *payload;
  return 0;
}

int nyabula_eye_engine_hide_scene(struct nyabula_eye_engine_s *engine)
{
  if (engine == NULL)
    {
      return -EINVAL;
    }

  if (engine->scene.scene == NYABULA_EYE_SCENE_NONE && !engine->scene_pending)
    {
      return 0;
    }

  engine->scene_pending = false;
  memset(&engine->pending_scene, 0, sizeof(engine->pending_scene));
  if (engine->scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL)
    {
      memset(&engine->previous_scene, 0, sizeof(engine->previous_scene));
      engine->scene_phase = NYABULA_EYE_SCENE_PHASE_REOPENING_OUT;
    }
  else
    {
      engine->scene_phase = NYABULA_EYE_SCENE_PHASE_CLOSING_OUT;
    }
  engine->scene_phase_start = lv_tick_get();
  if (engine->scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL)
    {
      engine->scene_lid = 1.0f;
    }

  return 0;
}

enum nyabula_eye_scene_e
nyabula_eye_engine_get_scene(const struct nyabula_eye_engine_s *engine)
{
  return engine == NULL ? NYABULA_EYE_SCENE_NONE : engine->scene.scene;
}

enum nyabula_eye_expression_e
nyabula_eye_engine_get_expression(const struct nyabula_eye_engine_s *engine)
{
  return engine == NULL ? NYABULA_EYE_EXPRESSION_IDLE : engine->expression;
}
