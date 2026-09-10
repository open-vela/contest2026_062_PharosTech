/****************************************************************************
 * app/nyabula/include/nyabula_core.h
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

#ifndef __APP_NYABULA_INCLUDE_NYABULA_CORE_H
#define __APP_NYABULA_INCLUDE_NYABULA_CORE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nyabula_eye_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NYABULA_CORE_SOURCE_MAX     64
#define NYABULA_CORE_REQUEST_ID_MAX 40
#define NYABULA_CORE_ERROR_MAX      96

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum nyabula_core_action_e
{
  NYABULA_CORE_ACTION_EXPRESSION = 0,
  NYABULA_CORE_ACTION_BLINK,
  NYABULA_CORE_ACTION_GAZE,
  NYABULA_CORE_ACTION_AUTO_BLINK,
  NYABULA_CORE_ACTION_AMBIENT_LIGHT,
  NYABULA_CORE_ACTION_IRIS_COLOR,
  NYABULA_CORE_ACTION_SCENE_SHOW,
  NYABULA_CORE_ACTION_SCENE_UPDATE,
  NYABULA_CORE_ACTION_SCENE_HIDE,
  NYABULA_CORE_ACTION_RELEASE,
  NYABULA_CORE_ACTION_RESET
};

enum nyabula_core_domain_e
{
  NYABULA_CORE_DOMAIN_EXPRESSION = 1 << 0,
  NYABULA_CORE_DOMAIN_SCENE = 1 << 1,
  NYABULA_CORE_DOMAIN_ALL =
      NYABULA_CORE_DOMAIN_EXPRESSION | NYABULA_CORE_DOMAIN_SCENE
};

struct nyabula_core_command_s
{
  enum nyabula_core_action_e action;
  char source[NYABULA_CORE_SOURCE_MAX];
  char request_id[NYABULA_CORE_REQUEST_ID_MAX];
  uint8_t priority;
  uint32_t lease_ms;

  union
  {
    struct
    {
      enum nyabula_eye_expression_e expression;
      uint32_t transition_ms;
    } expression;

    struct
    {
      enum nyabula_eye_mask_e eyes;
    } blink;

    struct
    {
      float x;
      float y;
      uint32_t hold_ms;
    } gaze;

    struct
    {
      bool enabled;
    } auto_blink;

    struct
    {
      float level;
    } ambient_light;

    struct
    {
      enum nyabula_eye_mask_e eyes;
      uint32_t rgb;
    } iris_color;

    struct
    {
      struct nyabula_eye_scene_request_s request;
    } scene_show;

    struct
    {
      struct nyabula_eye_scene_payload_s payload;
    } scene_update;

    struct
    {
      uint8_t domains;
    } release;
  } data;
};

struct nyabula_core_owner_s
{
  char source[NYABULA_CORE_SOURCE_MAX];
  uint8_t priority;
  uint32_t lease_remaining_ms;
  uint64_t sequence;
  bool active;
};

struct nyabula_core_snapshot_s
{
  uint64_t revision;
  uint64_t uptime_ms;
  size_t queue_depth;
  enum nyabula_eye_expression_e expression;
  struct nyabula_core_owner_s expression_owner;
  enum nyabula_eye_scene_e scene;
  enum nyabula_eye_scene_style_e scene_style;
  struct nyabula_eye_scene_payload_s scene_payload;
  struct nyabula_core_owner_s scene_owner;
  uint64_t blink_nonce;
  enum nyabula_eye_mask_e blink_eyes;
  bool auto_blink;
  float ambient_light;
  uint32_t iris_rgb[NYABULA_EYE_COUNT];
  char last_request_id[NYABULA_CORE_REQUEST_ID_MAX];
  int last_status;
  char last_error[NYABULA_CORE_ERROR_MAX];
};

struct nyabula_core_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

struct nyabula_core_s *
nyabula_core_create(struct nyabula_eye_engine_s *eye_engine);

void nyabula_core_destroy(struct nyabula_core_s *core);

int nyabula_core_submit(struct nyabula_core_s *core,
                        const struct nyabula_core_command_s *command);

void nyabula_core_tick(struct nyabula_core_s *core);

int nyabula_core_get_snapshot(struct nyabula_core_s *core,
                              struct nyabula_core_snapshot_s *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* __APP_NYABULA_INCLUDE_NYABULA_CORE_H */
