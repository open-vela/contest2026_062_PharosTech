/****************************************************************************
 * app/nyabula/src/nyabula_eye_internal.h
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

#ifndef __APP_NYABULA_SRC_NYABULA_EYE_INTERNAL_H
#define __APP_NYABULA_SRC_NYABULA_EYE_INTERNAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <lvgl/lvgl.h>

#include "../include/nyabula_eye_engine.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct nyabula_eye_scene_frame_s
{
  enum nyabula_eye_scene_e scene;
  enum nyabula_eye_scene_e previous_scene;
  enum nyabula_eye_scene_style_e style;
  enum nyabula_eye_scene_style_e previous_style;
  struct nyabula_eye_scene_payload_s payload;
  struct nyabula_eye_scene_payload_s previous_payload;
  float alpha;
  float previous_alpha;
  float lid;
  float reveal;
  float scene_seconds;
  float previous_scene_seconds;
};

struct nyabula_eye_frame_s
{
  float pupil_width;
  float pupil_height;
  float lid_top;
  float lid_bottom;
  float lid_slant;
  float bottom_curve;
  float gaze_x;
  float gaze_y;
  float iris_scale;
  float glow;
  float overlay;
  float squint;
  float derp;
  float zzz_mask;
  float global_seconds;
  float mode_seconds;
  uint32_t iris_rgb;
  enum nyabula_eye_expression_e expression;
  struct nyabula_eye_scene_frame_s scene;
};

struct nyabula_eye_renderer_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

struct nyabula_eye_renderer_s *
nyabula_eye_renderer_create(lv_obj_t *left_parent, lv_obj_t *right_parent);
void nyabula_eye_renderer_destroy(struct nyabula_eye_renderer_s *renderer);

/* Advance the sleep particles (z-z-z) once per animation tick.  This is
 * stateful (r->last_time / r->znext) and must be called exactly once per
 * tick from the engine, independent of how many eyes are rendered that
 * tick.  A representative frame (any eye) supplies global_seconds. */
void nyabula_eye_renderer_update_particles(
    struct nyabula_eye_renderer_s *renderer,
    const struct nyabula_eye_frame_s *frame);

/* Render a single eye (id == NYABULA_EYE_LEFT or NYABULA_EYE_RIGHT) into
 * its own canvas and invalidate only that canvas.  Call this once per eye
 * at the moment that eye's display is about to be refreshed (TE-aligned by
 * nyabula_display), so each eye is composited with the latest frame data
 * instead of sharing one stale frame snapshot. */
void nyabula_eye_renderer_render_eye(struct nyabula_eye_renderer_s *renderer,
                                     int id,
                                     const struct nyabula_eye_frame_s *frame);

#endif /* __APP_NYABULA_SRC_NYABULA_EYE_INTERNAL_H */
