/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_lighting_effects.h
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

#ifndef __NYABULA_CORE_NY_LIGHTING_EFFECTS_H
#define __NYABULA_CORE_NY_LIGHTING_EFFECTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NY_LIGHTING_GROUPS         4
#define NY_LIGHTING_PIXELS         33
#define NY_LIGHTING_MAX_BRIGHTNESS 100
#define NY_LIGHTING_BOOT_MS        2600
#define NY_LIGHTING_SLEEP_MS       1300

enum ny_lighting_effect_e
{
  NY_LIGHTING_STEADY,
  NY_LIGHTING_BREATHE,
  NY_LIGHTING_HEARTBEAT,
  NY_LIGHTING_AURORA,
  NY_LIGHTING_ORBIT,
  NY_LIGHTING_MUSIC,
  NY_LIGHTING_STATUS,
  NY_LIGHTING_EFFECT_COUNT
};
enum ny_lighting_color_e
{
  NY_LIGHTING_FIXED,
  NY_LIGHTING_IRIS,
  NY_LIGHTING_COMPLEMENT,
  NY_LIGHTING_COLOR_COUNT
};
enum ny_lighting_activity_e
{
  NY_LIGHTING_IDLE,
  NY_LIGHTING_LISTENING,
  NY_LIGHTING_THINKING,
  NY_LIGHTING_SPEAKING,
  NY_LIGHTING_ERROR
};
struct ny_lighting_group_s
{
  uint32_t color;
  uint8_t brightness;
  uint8_t speed;
  enum ny_lighting_effect_e effect;
  enum ny_lighting_color_e color_mode;
  bool enabled;
};
struct ny_lighting_input_s
{
  uint32_t iris[2];
  float level;
  bool music;
  enum ny_lighting_activity_e activity;
};
extern const char *const g_ny_lighting_ids[NY_LIGHTING_GROUPS];
extern const char *const g_ny_lighting_effects[NY_LIGHTING_EFFECT_COUNT];
extern const char *const g_ny_lighting_colors[NY_LIGHTING_COLOR_COUNT];
extern const uint8_t g_ny_lighting_first[NY_LIGHTING_GROUPS];
extern const uint8_t g_ny_lighting_count[NY_LIGHTING_GROUPS];
void ny_lighting_defaults(
    struct ny_lighting_group_s groups[NY_LIGHTING_GROUPS]);
bool ny_lighting_effect_allowed(unsigned int group,
                                enum ny_lighting_effect_e effect);
uint32_t ny_lighting_complement(uint32_t rgb);
uint32_t ny_lighting_mix(uint32_t from, uint32_t to, float amount);
void ny_lighting_render(
    const struct ny_lighting_group_s groups[NY_LIGHTING_GROUPS],
    const struct ny_lighting_input_s *input, uint64_t elapsed_ms,
    uint32_t pixels[NY_LIGHTING_PIXELS]);
void ny_lighting_boot(const uint32_t iris[2], uint32_t elapsed_ms,
                      uint32_t pixels[NY_LIGHTING_PIXELS]);
void ny_lighting_sleep(const uint32_t from[NY_LIGHTING_PIXELS],
                       uint32_t elapsed_ms,
                       uint32_t pixels[NY_LIGHTING_PIXELS]);
#endif
