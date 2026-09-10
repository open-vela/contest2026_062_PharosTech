/****************************************************************************
 * app/nyabula/src/nyabula_core.cxx
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
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/nyabula_core.h"

#define NYABULA_CORE_QUEUE_SIZE 32
#define NYABULA_CORE_SLOT_COUNT 8

struct nyabula_core_expression_slot_s
{
  bool used;
  char source[NYABULA_CORE_SOURCE_MAX];
  uint8_t priority;
  uint64_t sequence;
  uint64_t expires_at_ms;
  enum nyabula_eye_expression_e expression;
  uint32_t transition_ms;
};

struct nyabula_core_scene_slot_s
{
  bool used;
  char source[NYABULA_CORE_SOURCE_MAX];
  uint8_t priority;
  uint64_t sequence;
  uint64_t expires_at_ms;
  struct nyabula_eye_scene_request_s request;
};

struct nyabula_core_s
{
  struct nyabula_eye_engine_s *eye_engine;
  sem_t lock;
  struct nyabula_core_command_s queue[NYABULA_CORE_QUEUE_SIZE];
  size_t queue_head;
  size_t queue_tail;
  size_t queue_depth;
  struct nyabula_core_expression_slot_s expressions[NYABULA_CORE_SLOT_COUNT];
  struct nyabula_core_scene_slot_s scenes[NYABULA_CORE_SLOT_COUNT];
  struct nyabula_core_snapshot_s snapshot;
  uint64_t sequence;
  int expression_winner;
  int scene_winner;
};

static uint64_t nyabula_core_now_ms(void);
static int nyabula_core_lock(struct nyabula_core_s *core);
static void nyabula_core_copy_string(char *dest, size_t size,
                                     const char *source);
static void nyabula_core_set_result(struct nyabula_core_s *core,
                                    const char *request_id, int status,
                                    const char *error);
static uint64_t nyabula_core_expiry(uint64_t now, uint32_t lease_ms);
static bool nyabula_core_expired(uint64_t expires_at_ms, uint64_t now);
static int nyabula_core_find_expression_slot(struct nyabula_core_s *core,
                                             const char *source);
static int nyabula_core_find_scene_slot(struct nyabula_core_s *core,
                                        const char *source);
static int nyabula_core_select_expression(struct nyabula_core_s *core,
                                          uint64_t now);
static int nyabula_core_select_scene(struct nyabula_core_s *core,
                                     uint64_t now);
static void nyabula_core_fill_owner(struct nyabula_core_owner_s *owner,
                                    const char *source, uint8_t priority,
                                    uint64_t sequence, uint64_t expires_at_ms,
                                    uint64_t now);
static void nyabula_core_apply_winners(struct nyabula_core_s *core,
                                       uint64_t now);
static void nyabula_core_release(struct nyabula_core_s *core,
                                 const char *source, uint8_t domains);
static int nyabula_core_process(struct nyabula_core_s *core,
                                const struct nyabula_core_command_s *command,
                                uint64_t now);

static uint64_t nyabula_core_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int nyabula_core_lock(struct nyabula_core_s *core)
{
  int ret;

  do
    {
      ret = sem_wait(&core->lock);
    }
  while (ret < 0 && errno == EINTR);

  return ret < 0 ? -errno : 0;
}

static void nyabula_core_copy_string(char *dest, size_t size,
                                     const char *source)
{
  if (size == 0)
    {
      return;
    }

  snprintf(dest, size, "%s", source == NULL ? "" : source);
}

static void nyabula_core_set_result(struct nyabula_core_s *core,
                                    const char *request_id, int status,
                                    const char *error)
{
  nyabula_core_copy_string(core->snapshot.last_request_id,
                           sizeof(core->snapshot.last_request_id), request_id);
  core->snapshot.last_status = status;
  nyabula_core_copy_string(core->snapshot.last_error,
                           sizeof(core->snapshot.last_error), error);
  core->snapshot.revision++;
}

static uint64_t nyabula_core_expiry(uint64_t now, uint32_t lease_ms)
{
  return lease_ms == 0 ? 0 : now + lease_ms;
}

static bool nyabula_core_expired(uint64_t expires_at_ms, uint64_t now)
{
  return expires_at_ms != 0 && expires_at_ms <= now;
}

static int nyabula_core_find_expression_slot(struct nyabula_core_s *core,
                                             const char *source)
{
  int free_slot = -1;
  int oldest_slot = 0;

  for (int i = 0; i < NYABULA_CORE_SLOT_COUNT; i++)
    {
      if (core->expressions[i].used &&
          strcmp(core->expressions[i].source, source) == 0)
        {
          return i;
        }

      if (!core->expressions[i].used && free_slot < 0)
        {
          free_slot = i;
        }

      if (core->expressions[i].sequence <
          core->expressions[oldest_slot].sequence)
        {
          oldest_slot = i;
        }
    }

  return free_slot >= 0 ? free_slot : oldest_slot;
}

static int nyabula_core_find_scene_slot(struct nyabula_core_s *core,
                                        const char *source)
{
  int free_slot = -1;
  int oldest_slot = 0;

  for (int i = 0; i < NYABULA_CORE_SLOT_COUNT; i++)
    {
      if (core->scenes[i].used && strcmp(core->scenes[i].source, source) == 0)
        {
          return i;
        }

      if (!core->scenes[i].used && free_slot < 0)
        {
          free_slot = i;
        }

      if (core->scenes[i].sequence < core->scenes[oldest_slot].sequence)
        {
          oldest_slot = i;
        }
    }

  return free_slot >= 0 ? free_slot : oldest_slot;
}

static int nyabula_core_select_expression(struct nyabula_core_s *core,
                                          uint64_t now)
{
  int winner = -1;

  for (int i = 0; i < NYABULA_CORE_SLOT_COUNT; i++)
    {
      if (core->expressions[i].used &&
          nyabula_core_expired(core->expressions[i].expires_at_ms, now))
        {
          core->expressions[i].used = false;
        }

      if (!core->expressions[i].used)
        {
          continue;
        }

      if (winner < 0 ||
          core->expressions[i].priority > core->expressions[winner].priority ||
          (core->expressions[i].priority ==
               core->expressions[winner].priority &&
           core->expressions[i].sequence > core->expressions[winner].sequence))
        {
          winner = i;
        }
    }

  return winner;
}

static int nyabula_core_select_scene(struct nyabula_core_s *core, uint64_t now)
{
  int winner = -1;

  for (int i = 0; i < NYABULA_CORE_SLOT_COUNT; i++)
    {
      if (core->scenes[i].used &&
          nyabula_core_expired(core->scenes[i].expires_at_ms, now))
        {
          core->scenes[i].used = false;
        }

      if (!core->scenes[i].used)
        {
          continue;
        }

      if (winner < 0 ||
          core->scenes[i].priority > core->scenes[winner].priority ||
          (core->scenes[i].priority == core->scenes[winner].priority &&
           core->scenes[i].sequence > core->scenes[winner].sequence))
        {
          winner = i;
        }
    }

  return winner;
}

static void nyabula_core_fill_owner(struct nyabula_core_owner_s *owner,
                                    const char *source, uint8_t priority,
                                    uint64_t sequence, uint64_t expires_at_ms,
                                    uint64_t now)
{
  memset(owner, 0, sizeof(*owner));
  nyabula_core_copy_string(owner->source, sizeof(owner->source), source);
  owner->priority = priority;
  owner->sequence = sequence;
  owner->active = true;
  if (expires_at_ms > now)
    {
      uint64_t remaining = expires_at_ms - now;
      owner->lease_remaining_ms =
          remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    }
}

static void nyabula_core_apply_winners(struct nyabula_core_s *core,
                                       uint64_t now)
{
  int expression = nyabula_core_select_expression(core, now);
  int scene = nyabula_core_select_scene(core, now);

  if (expression != core->expression_winner)
    {
      enum nyabula_eye_expression_e value = NYABULA_EYE_EXPRESSION_IDLE;
      uint32_t transition_ms = 250;

      if (expression >= 0)
        {
          value = core->expressions[expression].expression;
          transition_ms = core->expressions[expression].transition_ms;
        }

      nyabula_eye_engine_set_expression(core->eye_engine, value,
                                        transition_ms);
      core->expression_winner = expression;
      core->snapshot.expression = value;
      core->snapshot.revision++;
    }

  if (expression >= 0)
    {
      nyabula_core_fill_owner(&core->snapshot.expression_owner,
                              core->expressions[expression].source,
                              core->expressions[expression].priority,
                              core->expressions[expression].sequence,
                              core->expressions[expression].expires_at_ms,
                              now);
    }
  else
    {
      memset(&core->snapshot.expression_owner, 0,
             sizeof(core->snapshot.expression_owner));
    }

  if (scene != core->scene_winner)
    {
      if (scene < 0)
        {
          nyabula_eye_engine_hide_scene(core->eye_engine);
          core->snapshot.scene = NYABULA_EYE_SCENE_NONE;
          memset(&core->snapshot.scene_payload, 0,
                 sizeof(core->snapshot.scene_payload));
        }
      else
        {
          nyabula_eye_engine_show_scene(core->eye_engine,
                                        &core->scenes[scene].request);
          core->snapshot.scene = core->scenes[scene].request.scene;
          core->snapshot.scene_style = core->scenes[scene].request.style;
          core->snapshot.scene_payload = core->scenes[scene].request.payload;
        }

      core->scene_winner = scene;
      core->snapshot.revision++;
    }

  if (scene >= 0)
    {
      nyabula_core_fill_owner(
          &core->snapshot.scene_owner, core->scenes[scene].source,
          core->scenes[scene].priority, core->scenes[scene].sequence,
          core->scenes[scene].expires_at_ms, now);
    }
  else
    {
      memset(&core->snapshot.scene_owner, 0,
             sizeof(core->snapshot.scene_owner));
    }
}

static void nyabula_core_release(struct nyabula_core_s *core,
                                 const char *source, uint8_t domains)
{
  for (int i = 0; i < NYABULA_CORE_SLOT_COUNT; i++)
    {
      if ((domains & NYABULA_CORE_DOMAIN_EXPRESSION) != 0 &&
          core->expressions[i].used &&
          strcmp(core->expressions[i].source, source) == 0)
        {
          core->expressions[i].used = false;
        }

      if ((domains & NYABULA_CORE_DOMAIN_SCENE) != 0 && core->scenes[i].used &&
          strcmp(core->scenes[i].source, source) == 0)
        {
          core->scenes[i].used = false;
        }
    }
}

static int nyabula_core_process(struct nyabula_core_s *core,
                                const struct nyabula_core_command_s *command,
                                uint64_t now)
{
  int slot;

  switch (command->action)
    {
      case NYABULA_CORE_ACTION_EXPRESSION:
        slot = nyabula_core_find_expression_slot(core, command->source);
        if (slot == core->expression_winner)
          {
            core->expression_winner = -2;
          }

        memset(&core->expressions[slot], 0, sizeof(core->expressions[slot]));
        core->expressions[slot].used = true;
        nyabula_core_copy_string(core->expressions[slot].source,
                                 sizeof(core->expressions[slot].source),
                                 command->source);
        core->expressions[slot].priority = command->priority;
        core->expressions[slot].sequence = ++core->sequence;
        core->expressions[slot].expires_at_ms =
            nyabula_core_expiry(now, command->lease_ms);
        core->expressions[slot].expression =
            command->data.expression.expression;
        core->expressions[slot].transition_ms =
            command->data.expression.transition_ms;
        break;

      case NYABULA_CORE_ACTION_BLINK:
        {
          int ret = nyabula_eye_engine_blink(core->eye_engine,
                                             command->data.blink.eyes);
          if (ret < 0)
            {
              return ret;
            }

          core->snapshot.blink_eyes = command->data.blink.eyes;
          core->snapshot.blink_nonce++;
          core->snapshot.revision++;
          break;
        }

      case NYABULA_CORE_ACTION_GAZE:
        return nyabula_eye_engine_set_gaze(
            core->eye_engine, command->data.gaze.x, command->data.gaze.y,
            command->data.gaze.hold_ms);

      case NYABULA_CORE_ACTION_AUTO_BLINK:
        nyabula_eye_engine_set_auto_blink(core->eye_engine,
                                          command->data.auto_blink.enabled);
        core->snapshot.auto_blink = command->data.auto_blink.enabled;
        core->snapshot.revision++;
        break;

      case NYABULA_CORE_ACTION_AMBIENT_LIGHT:
        nyabula_eye_engine_set_ambient_light(
            core->eye_engine, command->data.ambient_light.level);
        core->snapshot.ambient_light = command->data.ambient_light.level;
        core->snapshot.revision++;
        break;

      case NYABULA_CORE_ACTION_IRIS_COLOR:
        nyabula_eye_engine_set_iris_color(core->eye_engine,
                                          command->data.iris_color.eyes,
                                          command->data.iris_color.rgb);
        if ((command->data.iris_color.eyes & NYABULA_EYE_MASK_LEFT) != 0)
          {
            core->snapshot.iris_rgb[NYABULA_EYE_LEFT] =
                command->data.iris_color.rgb;
          }

        if ((command->data.iris_color.eyes & NYABULA_EYE_MASK_RIGHT) != 0)
          {
            core->snapshot.iris_rgb[NYABULA_EYE_RIGHT] =
                command->data.iris_color.rgb;
          }

        core->snapshot.revision++;
        break;

      case NYABULA_CORE_ACTION_SCENE_SHOW:
        slot = nyabula_core_find_scene_slot(core, command->source);
        if (slot == core->scene_winner)
          {
            core->scene_winner = -2;
          }

        memset(&core->scenes[slot], 0, sizeof(core->scenes[slot]));
        core->scenes[slot].used = true;
        nyabula_core_copy_string(core->scenes[slot].source,
                                 sizeof(core->scenes[slot].source),
                                 command->source);
        core->scenes[slot].priority = command->priority;
        core->scenes[slot].sequence = ++core->sequence;
        core->scenes[slot].expires_at_ms =
            nyabula_core_expiry(now, command->lease_ms);
        core->scenes[slot].request = command->data.scene_show.request;
        break;

      case NYABULA_CORE_ACTION_SCENE_UPDATE:
        slot = nyabula_core_find_scene_slot(core, command->source);
        if (!core->scenes[slot].used ||
            strcmp(core->scenes[slot].source, command->source) != 0)
          {
            return -ENOENT;
          }

        core->scenes[slot].request.payload =
            command->data.scene_update.payload;
        core->scenes[slot].priority = command->priority;
        core->scenes[slot].sequence = ++core->sequence;
        core->scenes[slot].expires_at_ms =
            nyabula_core_expiry(now, command->lease_ms);
        if (slot == core->scene_winner)
          {
            int ret = nyabula_eye_engine_update_scene(
                core->eye_engine, &core->scenes[slot].request.payload);
            if (ret < 0)
              {
                return ret;
              }

            core->snapshot.scene_payload = core->scenes[slot].request.payload;
            core->snapshot.revision++;
          }
        break;

      case NYABULA_CORE_ACTION_SCENE_HIDE:
        /* Hiding ends this source's scene ownership.  It must not leave a
         * persistent tombstone that suppresses lower-priority sources. */

        nyabula_core_release(core, command->source, NYABULA_CORE_DOMAIN_SCENE);
        break;

      case NYABULA_CORE_ACTION_RELEASE:
        nyabula_core_release(core, command->source,
                             command->data.release.domains);
        break;

      case NYABULA_CORE_ACTION_RESET:
        memset(core->expressions, 0, sizeof(core->expressions));
        memset(core->scenes, 0, sizeof(core->scenes));
        nyabula_eye_engine_set_auto_blink(core->eye_engine, true);
        nyabula_eye_engine_set_ambient_light(core->eye_engine, 1.0f);
        nyabula_eye_engine_set_iris_color(core->eye_engine,
                                          NYABULA_EYE_MASK_BOTH, 0x56ffb2);
        core->snapshot.auto_blink = true;
        core->snapshot.blink_eyes = NYABULA_EYE_MASK_BOTH;
        core->snapshot.ambient_light = 1.0f;
        core->snapshot.iris_rgb[NYABULA_EYE_LEFT] = 0x56ffb2;
        core->snapshot.iris_rgb[NYABULA_EYE_RIGHT] = 0x56ffb2;
        core->expression_winner = -2;
        core->scene_winner = -2;
        break;

      default:
        return -EINVAL;
    }

  return 0;
}

extern "C" struct nyabula_core_s *
nyabula_core_create(struct nyabula_eye_engine_s *eye_engine)
{
  struct nyabula_core_s *core;

  if (eye_engine == NULL)
    {
      return NULL;
    }

  core = static_cast<struct nyabula_core_s *>(calloc(1, sizeof(*core)));
  if (core == NULL)
    {
      return NULL;
    }

  if (sem_init(&core->lock, 0, 1) < 0)
    {
      free(core);
      return NULL;
    }

  core->eye_engine = eye_engine;
  core->expression_winner = -1;
  core->scene_winner = -1;
  core->snapshot.expression = NYABULA_EYE_EXPRESSION_IDLE;
  core->snapshot.scene = NYABULA_EYE_SCENE_NONE;
  core->snapshot.blink_eyes = NYABULA_EYE_MASK_BOTH;
  core->snapshot.auto_blink = true;
  core->snapshot.ambient_light = 1.0f;
  core->snapshot.iris_rgb[NYABULA_EYE_LEFT] = 0x56ffb2;
  core->snapshot.iris_rgb[NYABULA_EYE_RIGHT] = 0x56ffb2;
  return core;
}

extern "C" void nyabula_core_destroy(struct nyabula_core_s *core)
{
  if (core != NULL)
    {
      sem_destroy(&core->lock);
      free(core);
    }
}

extern "C" int
nyabula_core_submit(struct nyabula_core_s *core,
                    const struct nyabula_core_command_s *command)
{
  int ret = 0;

  if (core == NULL || command == NULL || command->source[0] == '\0')
    {
      return -EINVAL;
    }

  ret = nyabula_core_lock(core);
  if (ret < 0)
    {
      return ret;
    }
  if (core->queue_depth >= NYABULA_CORE_QUEUE_SIZE)
    {
      ret = -EAGAIN;
    }
  else
    {
      core->queue[core->queue_tail] = *command;
      core->queue_tail = (core->queue_tail + 1) % NYABULA_CORE_QUEUE_SIZE;
      core->queue_depth++;
      core->snapshot.queue_depth = core->queue_depth;
    }

  sem_post(&core->lock);
  return ret;
}

extern "C" void nyabula_core_tick(struct nyabula_core_s *core)
{
  struct nyabula_core_command_s command;
  uint64_t now;

  if (core == NULL)
    {
      return;
    }

  if (nyabula_core_lock(core) < 0)
    {
      return;
    }
  now = nyabula_core_now_ms();
  core->snapshot.uptime_ms = now;

  while (core->queue_depth > 0)
    {
      int status;
      const char *error = "";

      command = core->queue[core->queue_head];
      core->queue_head = (core->queue_head + 1) % NYABULA_CORE_QUEUE_SIZE;
      core->queue_depth--;
      status = nyabula_core_process(core, &command, now);
      if (status < 0)
        {
          error = status == -ENOENT ? "source has no active scene"
                                    : "eye engine rejected command";
        }

      nyabula_core_set_result(core, command.request_id, status, error);
    }

  core->snapshot.queue_depth = core->queue_depth;
  nyabula_core_apply_winners(core, now);
  sem_post(&core->lock);
}

extern "C" int
nyabula_core_get_snapshot(struct nyabula_core_s *core,
                          struct nyabula_core_snapshot_s *snapshot)
{
  if (core == NULL || snapshot == NULL)
    {
      return -EINVAL;
    }

  int ret = nyabula_core_lock(core);
  if (ret < 0)
    {
      return ret;
    }
  *snapshot = core->snapshot;
  sem_post(&core->lock);
  return 0;
}
