/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_lighting_effects.c
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

#include "ny_lighting_effects.h"
#include <math.h>
#include <string.h>

#define NY_LIGHTING_PI 3.14159265358979323846f
const char *const g_ny_lighting_ids[NY_LIGHTING_GROUPS] = { "left_ear",
                                                            "right_ear",
                                                            "chest", "base" };
const char *const g_ny_lighting_effects[NY_LIGHTING_EFFECT_COUNT] = {
  "steady", "breathe", "heartbeat", "aurora", "orbit", "music", "status"
};
const char *const g_ny_lighting_colors[NY_LIGHTING_COLOR_COUNT] = {
  "fixed", "iris", "complement"
};
const uint8_t g_ny_lighting_first[NY_LIGHTING_GROUPS] = { 0, 1, 2, 10 };
const uint8_t g_ny_lighting_count[NY_LIGHTING_GROUPS] = { 1, 1, 8, 23 };

static float ny_lighting_clamp(float value);
static float ny_lighting_ease(float value);
static uint32_t ny_lighting_scale(uint32_t color, float value);
static uint32_t ny_lighting_hsv(float hue, float saturation, float value);

/****************************************************************************
 * Name: ny_lighting_clamp
 ****************************************************************************/
static float ny_lighting_clamp(float value)
{
  return !isfinite(value) || value < 0 ? 0 : value > 1 ? 1 : value;
}

/****************************************************************************
 * Name: ny_lighting_ease
 ****************************************************************************/
static float ny_lighting_ease(float value)
{
  value = ny_lighting_clamp(value);
  return value * value * (3 - 2 * value);
}

/****************************************************************************
 * Name: ny_lighting_scale
 ****************************************************************************/
static uint32_t ny_lighting_scale(uint32_t color, float value)
{
  value = ny_lighting_clamp(value);
  uint32_t out = 0;
  for (unsigned int shift = 0; shift < 24; shift += 8)
    out |= (uint32_t)lroundf(((color >> shift) & 255) * value) << shift;
  return out;
}

/****************************************************************************
 * Name: ny_lighting_mix
 ****************************************************************************/
uint32_t ny_lighting_mix(uint32_t from, uint32_t to, float amount)
{
  amount = ny_lighting_ease(amount);
  uint32_t out = 0;
  for (unsigned int shift = 0; shift < 24; shift += 8)
    {
      float a = (from >> shift) & 255;
      float b = (to >> shift) & 255;
      out |= (uint32_t)lroundf(a + (b - a) * amount) << shift;
    }
  return out;
}

/****************************************************************************
 * Name: ny_lighting_hsv
 ****************************************************************************/
static uint32_t ny_lighting_hsv(float hue, float saturation, float value)
{
  hue = fmodf(hue + 360, 360) / 60;
  float chroma = value * saturation;
  float x = chroma * (1 - fabsf(fmodf(hue, 2) - 1));
  float m = value - chroma;
  float r = 0, g = 0, b = 0;
  if (hue < 1)
    {
      r = chroma;
      g = x;
    }
  else if (hue < 2)
    {
      r = x;
      g = chroma;
    }
  else if (hue < 3)
    {
      g = chroma;
      b = x;
    }
  else if (hue < 4)
    {
      g = x;
      b = chroma;
    }
  else if (hue < 5)
    {
      r = x;
      b = chroma;
    }
  else
    {
      r = chroma;
      b = x;
    }
  return ((uint32_t)lroundf((r + m) * 255) << 16) |
         ((uint32_t)lroundf((g + m) * 255) << 8) |
         (uint32_t)lroundf((b + m) * 255);
}

/****************************************************************************
 * Name: ny_lighting_complement
 ****************************************************************************/
uint32_t ny_lighting_complement(uint32_t rgb)
{
  float r = ((rgb >> 16) & 255) / 255.0f;
  float g = ((rgb >> 8) & 255) / 255.0f;
  float b = (rgb & 255) / 255.0f;
  float hi = fmaxf(r, fmaxf(g, b)), lo = fminf(r, fminf(g, b));
  float delta = hi - lo;
  if (delta < 0.00001f)
    return rgb & 0xffffff;
  float hue = hi == r   ? 60 * fmodf((g - b) / delta, 6)
              : hi == g ? 60 * ((b - r) / delta + 2)
                        : 60 * ((r - g) / delta + 4);
  return ny_lighting_hsv(hue + 180, delta / hi, hi);
}

/****************************************************************************
 * Name: ny_lighting_defaults
 ****************************************************************************/
void ny_lighting_defaults(
    struct ny_lighting_group_s groups[NY_LIGHTING_GROUPS])
{
  static const uint32_t colors[NY_LIGHTING_GROUPS] = { 0x6fd3ff, 0x6fd3ff,
                                                       0xffad69, 0x8d8cff };
  memset(groups, 0, sizeof(*groups) * NY_LIGHTING_GROUPS);
  for (unsigned int i = 0; i < NY_LIGHTING_GROUPS; i++)
    {
      groups[i].color = colors[i];
      groups[i].brightness = 12;
      groups[i].speed = 2;
      groups[i].effect = NY_LIGHTING_STEADY;
      groups[i].color_mode = i < 2 ? NY_LIGHTING_IRIS : NY_LIGHTING_FIXED;
    }
}

/****************************************************************************
 * Name: ny_lighting_effect_allowed
 ****************************************************************************/
bool ny_lighting_effect_allowed(unsigned int group,
                                enum ny_lighting_effect_e effect)
{
  return group < NY_LIGHTING_GROUPS && effect < NY_LIGHTING_EFFECT_COUNT &&
         effect >= NY_LIGHTING_STEADY &&
         ((effect != NY_LIGHTING_AURORA && effect != NY_LIGHTING_ORBIT) ||
          group == 3) &&
         (effect != NY_LIGHTING_STATUS || group == 2);
}

/****************************************************************************
 * Name: ny_lighting_render
 ****************************************************************************/
void ny_lighting_render(
    const struct ny_lighting_group_s groups[NY_LIGHTING_GROUPS],
    const struct ny_lighting_input_s *input, uint64_t elapsed_ms,
    uint32_t pixels[NY_LIGHTING_PIXELS])
{
  memset(pixels, 0, NY_LIGHTING_PIXELS * sizeof(*pixels));
  for (unsigned int group = 0; group < NY_LIGHTING_GROUPS; group++)
    {
      const struct ny_lighting_group_s *cfg = &groups[group];
      if (!cfg->enabled || cfg->brightness == 0)
        continue;
      float limit = fminf(cfg->brightness, NY_LIGHTING_MAX_BRIGHTNESS) / 100;
      float speed = cfg->speed == 1 ? 0.6f : cfg->speed == 3 ? 1.6f : 1;
      float t = (elapsed_ms % 3600000) / 1000.0f * speed;
      uint32_t color = cfg->color;
      if (group < 2 && cfg->color_mode != NY_LIGHTING_FIXED)
        color = cfg->color_mode == NY_LIGHTING_IRIS
                    ? input->iris[group]
                    : ny_lighting_complement(input->iris[group]);
      for (unsigned int i = 0; i < g_ny_lighting_count[group]; i++)
        {
          float position = (float)i / g_ny_lighting_count[group];
          float level = 1;
          uint32_t pixel = color;
          switch (cfg->effect)
            {
              case NY_LIGHTING_BREATHE:
                level =
                    0.10f + 0.90f * powf(0.5f - 0.5f * cosf(t * 1.5f), 1.5f);
                break;
              case NY_LIGHTING_HEARTBEAT:
                {
                  float phase = fmodf(t, 2.8f);
                  level = 0.06f +
                          0.94f * expf(-powf((phase - 0.35f) / 0.13f, 2)) +
                          0.55f * expf(-powf((phase - 0.68f) / 0.18f, 2));
                }
                break;
              case NY_LIGHTING_AURORA:
                {
                  float wave =
                      0.5f +
                      0.5f * sinf(position * 2 * NY_LIGHTING_PI - t * 0.65f);
                  pixel = ny_lighting_mix(color, ny_lighting_complement(color),
                                          wave);
                  level = 0.25f +
                          0.75f * (0.5f +
                                   0.5f * sinf(position * 4 * NY_LIGHTING_PI +
                                               t * 0.45f));
                }
                break;
              case NY_LIGHTING_ORBIT:
                {
                  float distance = fmodf(position - t / 4 + 1000, 1);
                  distance = fminf(distance, 1 - distance);
                  level = 0.03f + 0.97f * expf(-powf(distance / 0.13f, 2));
                }
                break;
              case NY_LIGHTING_MUSIC:
                level = input->music ? ny_lighting_clamp(input->level) : 0;
                if (group >= 2)
                  level *= ny_lighting_clamp((level - position) * 4 + 0.25f);
                break;
              case NY_LIGHTING_STATUS:
                if (input->activity == NY_LIGHTING_LISTENING)
                  {
                    pixel = 0x55cfff;
                    level =
                        0.2f +
                        0.8f * powf(0.5f + 0.5f * sinf(t * 3 - i * 0.7f), 2);
                  }
                else if (input->activity == NY_LIGHTING_THINKING)
                  {
                    pixel = 0xa98dff;
                    level = 0.2f + 0.6f * (0.5f - 0.5f * cosf(t * 2.4f));
                  }
                else if (input->activity == NY_LIGHTING_SPEAKING)
                  {
                    pixel = 0x5aefbb;
                    level = 0.18f + 0.82f * ny_lighting_clamp(input->level);
                  }
                else if (input->activity == NY_LIGHTING_ERROR)
                  {
                    pixel = 0xff8e4d;
                    level = 0.2f + 0.5f * (0.5f - 0.5f * cosf(t * 2));
                  }
                else
                  level = 0.12f;
                break;
              default:
                break;
            }
          pixels[g_ny_lighting_first[group] + i] =
              ny_lighting_scale(pixel, limit * ny_lighting_clamp(level));
        }
    }
}

/****************************************************************************
 * Name: ny_lighting_boot
 ****************************************************************************/
void ny_lighting_boot(const uint32_t iris[2], uint32_t elapsed_ms,
                      uint32_t pixels[NY_LIGHTING_PIXELS])
{
  memset(pixels, 0, NY_LIGHTING_PIXELS * sizeof(*pixels));
  if (elapsed_ms >= NY_LIGHTING_BOOT_MS)
    return;
  float tail = 1 - ny_lighting_ease(((float)elapsed_ms - 1700) / 900);
  for (unsigned int i = 0; i < NY_LIGHTING_PIXELS; i++)
    {
      float delay = i < 2    ? 100 + i * 160
                    : i < 10 ? 430 + (i - 2) * 45
                             : 650 + (i - 10) * 24;
      float rise = ny_lighting_ease(((float)elapsed_ms - delay) / 420);
      uint32_t color =
          i < 2    ? iris[i]
          : i < 10 ? 0xffba80
                   : ny_lighting_mix(iris[0], iris[1], (i - 10) / 22.0f);
      pixels[i] = ny_lighting_scale(color, 0.12f * rise * tail);
    }
}

/****************************************************************************
 * Name: ny_lighting_sleep
 ****************************************************************************/
void ny_lighting_sleep(const uint32_t from[NY_LIGHTING_PIXELS],
                       uint32_t elapsed_ms,
                       uint32_t pixels[NY_LIGHTING_PIXELS])
{
  for (unsigned int i = 0; i < NY_LIGHTING_PIXELS; i++)
    {
      float delay = i < 2    ? i * 80
                    : i < 10 ? 150 + (i - 2) * 20
                             : 330 + (i - 10) * 20;
      float level =
          elapsed_ms >= NY_LIGHTING_SLEEP_MS
              ? 0
              : 1 - ny_lighting_ease(((float)elapsed_ms - delay) / 500);
      pixels[i] = ny_lighting_scale(from[i], level);
    }
}
