/****************************************************************************
 * app/nyabula/src/nyabula_eye_renderer_lvgl.c
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "generated/fonts/nyabula_eye_fonts.h"
#include "generated/nyabula_eye_icons.h"
#include "nyabula_eye_internal.h"

#define W                360
#define H                360
#define R                178.0f
#define CY               180.0f
#define PI               3.14159265358979323846f
#define FIBERS           48
#define ZCOUNT           16
#define FONT_CACHE_COUNT 32
#define TEXT_CACHE_COUNT 32
#define TEXT_CACHE_BYTES 96
#define BASE_CACHE_COUNT 2
#define HEART_SAMPLES    96
#define LID_SAMPLES      400
#define LID_OFFSET       200

struct scene_point_s;

struct transform_s
{
  float cx;
  float cy;
  float sx;
  float sy;
  float sn;
  float cs;
};

struct eye_s
{
  struct transform_s t;
  const struct nyabula_eye_frame_s *f;
  float ir;
  float gx;
  float gy;
  float lt;
  float lb;
  float slant;
  float curve;
  float top_y[LID_SAMPLES];
  float bottom_y[LID_SAMPLES];
  int id;
};

struct z_s
{
  float ox;
  float y;
  float vy;
  float sway;
  float phase;
  float size;
  float life;
  float rotation;
  int eye;
  bool active;
};

enum font_family_e
{
  FONT_FAMILY_TITLE = 0,
  FONT_FAMILY_BODY,
  FONT_FAMILY_ENGLISH
};

struct font_cache_s
{
  lv_font_t *font;
  uint16_t size;
  uint8_t family;
};

struct text_cache_s
{
  const lv_font_t *font;
  uint32_t age;
  int width;
  char text[TEXT_CACHE_BYTES];
  bool valid;
};

struct fiber_s
{
  float cosine;
  float sine;
  float inner_radius;
  float outer_radius;
};

struct base_cache_s
{
  lv_obj_t *canvas;
  lv_draw_buf_t *buffer;
  uint32_t iris_rgb;
  uint32_t age;
  int16_t gaze_x;
  int16_t gaze_y;
  uint16_t iris_radius;
  uint16_t scale_y;
  uint16_t glow;
  bool valid;
};

struct page_state_s
{
  lv_area_t dirty;
  uint32_t iris_rgb;
  int16_t gaze_x;
  int16_t gaze_y;
  uint16_t iris_radius;
  uint16_t scale_y;
  uint16_t glow;
  bool valid;
};

struct nyabula_eye_renderer_s
{
  lv_obj_t *canvas[NYABULA_EYE_COUNT];
  lv_obj_t *mask_canvas;
  lv_draw_buf_t *buffer[NYABULA_EYE_COUNT][2];
  lv_draw_buf_t *mask_buffer;
  struct base_cache_s base_cache[BASE_CACHE_COUNT];
  struct page_state_s page_state[NYABULA_EYE_COUNT][2];
  lv_image_dsc_t mask_image;
  lv_draw_buf_t *draw;
  lv_layer_t layer;
  lv_vector_dsc_t *vector;
  lv_vector_path_t *path;
  lv_layer_t cache_layer;
  lv_vector_dsc_t *cache_vector;
  lv_vector_path_t *cache_path;
  lv_layer_t mask_layer;
  lv_vector_dsc_t *mask_vector;
  lv_vector_path_t *mask_path;
  lv_fpoint_t icon_points[2048];
  bool icon_moves[2048];
  lv_fpoint_t heart_points[HEART_SAMPLES];
  struct fiber_s fibers[FIBERS];
  struct z_s z[ZCOUNT];
  struct text_cache_s text_cache[TEXT_CACHE_COUNT];
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  struct font_cache_s font_cache[FONT_CACHE_COUNT];
#endif
  uint32_t random;
  uint32_t base_cache_clock;
  uint32_t text_cache_clock;
  uint32_t perf_start;
  uint32_t render_total;
  uint32_t build_total;
  uint32_t raster_total;
  uint32_t copy_total;
  uint32_t flush_total;
  uint32_t frames;
  uint32_t shared_frames;
  uint32_t reused_eyes;
  uint32_t base_cache_hits;
  uint32_t base_cache_builds;
  uint8_t index[NYABULA_EYE_COUNT];
  struct nyabula_eye_frame_s last_frame[NYABULA_EYE_COUNT];
  bool last_frame_valid[NYABULA_EYE_COUNT];
  bool mask_ready;
  float znext;
  float last_time;
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  lv_font_t *font_title_20;
  lv_font_t *font_title_28;
  lv_font_t *font_title_42;
  lv_font_t *font_body_14;
  lv_font_t *font_body_18;
  lv_font_t *font_english_18;
  lv_font_t *font_english_42;
  lv_font_t *font_english_72;
  lv_font_t *font_english_96;
  lv_font_t *font_english_119;
#endif
};

static bool in_lids(const struct eye_s *e, float sx, float sy);
static void iris(struct nyabula_eye_renderer_s *r, const struct eye_s *e);

#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
static lv_font_t *renderer_create_font(const char *filename, uint32_t size);
#endif

static const lv_font_t *renderer_font(struct nyabula_eye_renderer_s *r,
                                      const lv_font_t *fallback);
static const lv_font_t *scene_font(struct nyabula_eye_renderer_s *r,
                                   enum font_family_e family, uint16_t size,
                                   const lv_font_t *fallback);
static uint32_t utf8_next(const char *text, uint32_t *offset);
static float clampf(float value, float low, float high);
static lv_opa_t vector_opa(float opacity);
static void vector_identity(struct nyabula_eye_renderer_s *r);
static void vector_eye_transform(struct nyabula_eye_renderer_s *r,
                                 const struct eye_s *e);
static void vector_fill(struct nyabula_eye_renderer_s *r, uint32_t color,
                        float opacity);
static void vector_stroke(struct nyabula_eye_renderer_s *r, uint32_t color,
                          float opacity, float width);
static uint8_t channel(uint32_t color, int shift, float scale);
static uint32_t shade(uint32_t color, float scale);
static void to_screen(const struct transform_s *t, float x, float y, float *sx,
                      float *sy);
static void to_local(const struct transform_s *t, float x, float y, float *lx,
                     float *ly);
static int text_width(struct nyabula_eye_renderer_s *r, const lv_font_t *font,
                      const char *text);
static void text_center_at(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, const lv_font_t *font,
                           const char *text, float center_x, float center_y,
                           uint32_t color, float opacity);
static float eye_globe_radius(void);
static void clamp_to_eye_globe(float *x, float *y, float inset);
static void text_center(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e, const lv_font_t *font,
                        const char *text, float center_y, uint32_t color,
                        float opacity);
static float cubic(float a, float b, float c, float d, float t);
static float lid_y(float x, float extent, float left, float right, float bulge,
                   bool top);
static bool in_lids(const struct eye_s *e, float sx, float sy);
static void disk(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float x, float y, float radius, uint32_t color, float opacity,
                 bool lid_clip);
static void line(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float x1, float y1, float x2, float y2, float width,
                 uint32_t color, float opacity, bool lid_clip);
static void arc(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                float cx, float cy, float radius, float start, float sweep,
                float width, uint32_t color, float opacity);
static void base(struct nyabula_eye_renderer_s *r, const struct eye_s *e);
static lv_draw_buf_t *base_cache_get(struct nyabula_eye_renderer_s *r,
                                     const struct eye_s *e);
static bool page_base_matches(const struct page_state_s *state,
                              const struct eye_s *e);
static void page_remember_base(struct page_state_s *state,
                               const struct eye_s *e);
static void page_remember_dynamic_area(struct page_state_s *state,
                                       const struct eye_s *e, bool simple);
static lv_area_t page_dirty_union(const struct page_state_s *first,
                                  const struct page_state_s *second);
static void iris(struct nyabula_eye_renderer_s *r, const struct eye_s *e);
static void ellipse_path(struct nyabula_eye_renderer_s *r, float cx, float cy,
                         float rx, float ry, float rotation);
static void ellipse(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                    float cx, float cy, float rx, float ry, float rotation,
                    uint32_t color, float opacity);
static void pupil(struct nyabula_eye_renderer_s *r, const struct eye_s *e);
static void star(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float cx, float cy, float radius, float rotation,
                 uint32_t color, float opacity);
static void heart(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                  float cx, float cy, float radius, uint32_t color,
                  float opacity);
static lv_fpoint_t icon_point(const struct nyabula_eye_icon_s *icon,
                              float scale, int32_t x, int32_t y);
static void icon_append_path(struct nyabula_eye_renderer_s *r,
                             const struct nyabula_eye_icon_s *icon,
                             float size);
static void icon_fill(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                      const struct nyabula_eye_icon_s *icon, float size,
                      uint32_t color, float opacity);
static lv_fpoint_t icon_curve_point(const lv_fpoint_t *start,
                                    const lv_fpoint_t *control1,
                                    const lv_fpoint_t *control2,
                                    const lv_fpoint_t *end, float amount,
                                    bool cubic_curve);
static int icon_flatten(const struct nyabula_eye_icon_s *icon, float size,
                        lv_fpoint_t *points, bool *moves, int capacity);
static void icon_stroke(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_icon_s *icon, float size,
                        uint32_t color, float opacity, float reveal);
static void icon(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 const char *name, float size, uint32_t color, float opacity,
                 float reveal);
static void icon_at(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                    const char *name, float size, float x, float y,
                    float rotation, uint32_t color, float opacity,
                    float reveal);
static void icon_group_at(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e, const char *name, float size,
                          float x, float y, float rotation, uint32_t color,
                          float opacity, float reveal);
static void icon_outline(struct nyabula_eye_renderer_s *r,
                         const struct eye_s *e, const char *name, float size,
                         float width, uint32_t color, float opacity);
static void filled_rect(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e, float left, float top,
                        float right, float bottom, uint32_t color,
                        float opacity);
static const char *scene_text(const char *value, const char *fallback);
static uint32_t scene_color(const struct eye_s *e);
static void scene_backdrop(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, bool minimal, float opacity);
static void scene_dial(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       uint32_t color, float progress, float opacity);
static void rounded_rect_outline(struct nyabula_eye_renderer_s *r,
                                 const struct eye_s *e, float left, float top,
                                 float right, float bottom, float radius,
                                 float width, uint32_t color, float opacity);
static void scene_eq_curve(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, uint32_t color,
                           float seconds, float opacity, bool calibrating);
static void scene_play_triangle(struct nyabula_eye_renderer_s *r,
                                const struct eye_s *e, uint32_t color,
                                float opacity);
static void scene_append_cubic(struct scene_point_s *points, int *count,
                               struct scene_point_s start,
                               struct scene_point_s control1,
                               struct scene_point_s control2,
                               struct scene_point_s end);
static void scene_fill_polygon(struct nyabula_eye_renderer_s *r,
                               const struct eye_s *e,
                               const struct scene_point_s *points, int count,
                               uint32_t top_color, uint32_t bottom_color,
                               float opacity);
static void scene_stroke_points(struct nyabula_eye_renderer_s *r,
                                const struct eye_s *e,
                                const struct scene_point_s *points, int count,
                                float width, uint32_t color, float opacity);
static int scene_cloud_points(struct scene_point_s *points, float drift,
                              float offset_y, float scale);
static void scene_draw_cloud(struct nyabula_eye_renderer_s *r,
                             const struct eye_s *e, uint32_t color,
                             float drift, float offset_y, float scale,
                             float opacity);
static void
scene_music_progress(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                     const struct nyabula_eye_scene_payload_s *payload,
                     float seconds, float opacity);
static void scene_music_icon(struct nyabula_eye_renderer_s *r,
                             const struct eye_s *e, bool playing,
                             uint32_t color, float opacity, float scale);
static void
scene_music_title(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                  const struct nyabula_eye_scene_payload_s *payload,
                  uint32_t color, float opacity);
static void scene_music_time(struct nyabula_eye_renderer_s *r,
                             const struct eye_s *e,
                             const struct nyabula_eye_scene_payload_s *payload,
                             float seconds, uint32_t color, float opacity);
static void
scene_music_lyrics(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                   const struct nyabula_eye_scene_payload_s *payload,
                   uint32_t color, float opacity);
static void scene_music_spectrum(struct nyabula_eye_renderer_s *r,
                                 const struct eye_s *e, float seconds,
                                 uint32_t color, float opacity);
static void scene_music(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity);
static float scene_music_lerp(float from, float to, float progress);
static bool
scene_music_lyrics_changed(const struct nyabula_eye_scene_payload_s *from,
                           const struct nyabula_eye_scene_payload_s *to);
static void
scene_music_lyrics_transition(struct nyabula_eye_renderer_s *r,
                              const struct eye_s *e,
                              const struct nyabula_eye_scene_payload_s *from,
                              const struct nyabula_eye_scene_payload_s *to,
                              uint32_t color, float progress, float opacity);
static void
scene_music_transition(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *from,
                       const struct nyabula_eye_scene_payload_s *to,
                       float seconds, float progress, float opacity);
static void scene_timer(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity);
static float scene_ease(float value);
static void scene_draw_drop(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, float x, float y,
                            float size, float tilt, uint32_t color,
                            float opacity);
static void scene_draw_rain(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, uint32_t color,
                            float seconds, bool storm, float opacity);
static void scene_draw_snow(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, uint32_t color,
                            float seconds, float opacity);
static void scene_draw_fog(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, uint32_t color,
                           float seconds, float opacity);
static void scene_weather(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e,
                          const struct nyabula_eye_scene_payload_s *payload,
                          float seconds, float opacity);
static void scene_battery(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e,
                          const struct nyabula_eye_scene_payload_s *payload,
                          float opacity, float reveal);
static void scene_alarm(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity);
static void scene_call(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *payload,
                       float seconds, float opacity, float reveal);
static void scene_task(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *payload,
                       float seconds, float opacity, float reveal);
static void
scene_draw_content(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                   enum nyabula_eye_scene_e scene,
                   const struct nyabula_eye_scene_payload_s *payload,
                   float seconds, float opacity, float reveal, bool minimal);
static void overlays(struct nyabula_eye_renderer_s *r, const struct eye_s *e);
static void highlights(struct nyabula_eye_renderer_s *r,
                       const struct eye_s *e);
static bool lids_are_open(const struct eye_s *e);
static void lids(struct nyabula_eye_renderer_s *r, const struct eye_s *e);
static float randomf(struct nyabula_eye_renderer_s *r);
static void update_z(struct nyabula_eye_renderer_s *r,
                     const struct nyabula_eye_frame_s *f);
static void prepare_lid_clip(struct eye_s *e);
static void draw_z(struct nyabula_eye_renderer_s *r, struct eye_s *e);
static void prepare(struct eye_s *e, const struct nyabula_eye_frame_s *f,
                    int id);
static void prepare_scene(struct eye_s *e,
                          const struct nyabula_eye_frame_s *frame, int id);
static void prepare_scene_motion(struct eye_s *motion,
                                 const struct eye_s *scene_eye, float opacity,
                                 bool incoming);
static void prepare_scene_lids(struct eye_s *e,
                               const struct nyabula_eye_frame_s *frame, int id,
                               float lid);
static float scene_lid_boundary(const float samples[LID_SAMPLES], float x);
static void apply_scene_lid_mask(lv_draw_buf_t *buffer, struct eye_s *eye);
static void render_scene_lid_mask(struct nyabula_eye_renderer_s *r,
                                  const struct nyabula_eye_frame_s *frame,
                                  const struct eye_s *scene_eye, int id);
static bool render_scene(struct nyabula_eye_renderer_s *r,
                         const struct nyabula_eye_frame_s *frame, int id);
static bool frames_can_share_pixels(
    const struct nyabula_eye_frame_s frames[NYABULA_EYE_COUNT]);
static bool
scene_eye_is_time_independent(const struct nyabula_eye_scene_frame_s *scene,
                              int id);
static bool frame_is_time_independent(const struct nyabula_eye_frame_s *frame,
                                      int id);
static bool frame_pixels_unchanged(struct nyabula_eye_renderer_s *r,
                                   const struct nyabula_eye_frame_s *frame,
                                   int id);
static void remember_frame(struct nyabula_eye_renderer_s *r,
                           const struct nyabula_eye_frame_s *frame, int id);
static void report(struct nyabula_eye_renderer_s *r, uint32_t render_ms);
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
static lv_font_t *renderer_create_font(const char *filename, uint32_t size);
static void renderer_load_fonts(struct nyabula_eye_renderer_s *r);
static void renderer_unload_fonts(struct nyabula_eye_renderer_s *r);
#endif

static const lv_font_t *renderer_font(struct nyabula_eye_renderer_s *r,
                                      const lv_font_t *fallback)
{
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
#define DYNAMIC_FONT(name)                                        \
  if (fallback == &nyabula_font_##name && r->font_##name != NULL) \
    {                                                             \
      return r->font_##name;                                      \
    }

  DYNAMIC_FONT(title_20);
  DYNAMIC_FONT(title_28);
  DYNAMIC_FONT(title_42);
  DYNAMIC_FONT(body_14);
  DYNAMIC_FONT(body_18);
  DYNAMIC_FONT(english_18);
  DYNAMIC_FONT(english_42);
  DYNAMIC_FONT(english_72);
  DYNAMIC_FONT(english_96);
  DYNAMIC_FONT(english_119);
#undef DYNAMIC_FONT
#else
  (void)r;
#endif
  return fallback;
}

static const lv_font_t *scene_font(struct nyabula_eye_renderer_s *r,
                                   enum font_family_e family, uint16_t size,
                                   const lv_font_t *fallback)
{
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  const char *filename;
  int index;

  for (index = 0; index < FONT_CACHE_COUNT; index++)
    {
      if (r->font_cache[index].font != NULL &&
          r->font_cache[index].family == family &&
          r->font_cache[index].size == size)
        {
          return r->font_cache[index].font;
        }
    }

  filename = family == FONT_FAMILY_TITLE  ? "AlimamaShuHeiTi.ttf"
             : family == FONT_FAMILY_BODY ? "MiSans-Semibold.ttf"
                                          : "Tinos-Bold.ttf";
  for (index = 0; index < FONT_CACHE_COUNT; index++)
    {
      if (r->font_cache[index].font == NULL)
        {
          lv_font_t *font = renderer_create_font(filename, size);
          if (font != NULL)
            {
              r->font_cache[index].font = font;
              r->font_cache[index].family = family;
              r->font_cache[index].size = size;
              return font;
            }

          break;
        }
    }
#else
  (void)r;
  (void)family;
  (void)size;
#endif
  return renderer_font(r, fallback);
}

static uint32_t utf8_next(const char *text, uint32_t *offset)
{
  const uint8_t *bytes = (const uint8_t *)text;
  uint32_t first = bytes[(*offset)++];

  if (first < 0x80)
    {
      return first;
    }

  if ((first & 0xe0) == 0xc0)
    {
      uint32_t value = (first & 0x1f) << 6;
      return value | (bytes[(*offset)++] & 0x3f);
    }

  if ((first & 0xf0) == 0xe0)
    {
      uint32_t value = (first & 0x0f) << 12;
      value |= (bytes[(*offset)++] & 0x3f) << 6;
      return value | (bytes[(*offset)++] & 0x3f);
    }

  if ((first & 0xf8) == 0xf0)
    {
      uint32_t value = (first & 0x07) << 18;
      value |= (bytes[(*offset)++] & 0x3f) << 12;
      value |= (bytes[(*offset)++] & 0x3f) << 6;
      return value | (bytes[(*offset)++] & 0x3f);
    }

  return 0xfffd;
}

static float clampf(float value, float low, float high)
{
  return value < low ? low : value > high ? high : value;
}

static lv_opa_t vector_opa(float opacity)
{
  return (lv_opa_t)lroundf(clampf(opacity, 0.0f, 1.0f) * LV_OPA_COVER);
}

static void vector_identity(struct nyabula_eye_renderer_s *r)
{
  lv_vector_dsc_identity(r->vector);
}

static void vector_eye_transform(struct nyabula_eye_renderer_s *r,
                                 const struct eye_s *e)
{
  lv_matrix_t matrix;

  lv_matrix_identity(&matrix);
  matrix.m[0][0] = e->t.sx * e->t.cs;
  matrix.m[0][1] = -e->t.sy * e->t.sn;
  matrix.m[0][2] = e->t.cx;
  matrix.m[1][0] = e->t.sx * e->t.sn;
  matrix.m[1][1] = e->t.sy * e->t.cs;
  matrix.m[1][2] = e->t.cy;
  lv_vector_dsc_set_transform(r->vector, &matrix);
}

static void vector_fill(struct nyabula_eye_renderer_s *r, uint32_t color,
                        float opacity)
{
  lv_vector_dsc_set_fill_color(r->vector, lv_color_hex(color));
  lv_vector_dsc_set_fill_opa(r->vector, vector_opa(opacity));
  lv_vector_dsc_set_stroke_opa(r->vector, LV_OPA_TRANSP);
  lv_vector_dsc_add_path(r->vector, r->path);
}

static void vector_stroke(struct nyabula_eye_renderer_s *r, uint32_t color,
                          float opacity, float width)
{
  lv_vector_dsc_set_fill_opa(r->vector, LV_OPA_TRANSP);
  lv_vector_dsc_set_stroke_color(r->vector, lv_color_hex(color));
  lv_vector_dsc_set_stroke_opa(r->vector, vector_opa(opacity));
  lv_vector_dsc_set_stroke_width(r->vector, width);
  lv_vector_dsc_set_stroke_cap(r->vector, LV_VECTOR_STROKE_CAP_ROUND);
  lv_vector_dsc_set_stroke_join(r->vector, LV_VECTOR_STROKE_JOIN_ROUND);
  lv_vector_dsc_set_stroke_dash(r->vector, NULL, 0);
  lv_vector_dsc_add_path(r->vector, r->path);
}

static uint8_t channel(uint32_t color, int shift, float scale)
{
  return (uint8_t)clampf(((color >> shift) & 255u) * scale, 0.0f, 255.0f);
}

static uint32_t shade(uint32_t color, float scale)
{
  return ((uint32_t)channel(color, 16, scale) << 16) |
         ((uint32_t)channel(color, 8, scale) << 8) | channel(color, 0, scale);
}

static void to_screen(const struct transform_s *t, float x, float y, float *sx,
                      float *sy)
{
  x *= t->sx;
  y *= t->sy;
  *sx = t->cx + x * t->cs - y * t->sn;
  *sy = t->cy + x * t->sn + y * t->cs;
}

static void to_local(const struct transform_s *t, float x, float y, float *lx,
                     float *ly)
{
  float dx = x - t->cx;
  float dy = y - t->cy;
  *lx = (dx * t->cs + dy * t->sn) / t->sx;
  *ly = (-dx * t->sn + dy * t->cs) / t->sy;
}

static int text_width(struct nyabula_eye_renderer_s *r, const lv_font_t *font,
                      const char *text)
{
  lv_font_glyph_dsc_t descriptor;
  uint32_t offset = 0;
  size_t length = strlen(text);
  int width = 0;
  int index;

  if (length < TEXT_CACHE_BYTES)
    {
      for (index = 0; index < TEXT_CACHE_COUNT; index++)
        {
          struct text_cache_s *entry = &r->text_cache[index];

          if (entry->valid && entry->font == font &&
              strcmp(entry->text, text) == 0)
            {
              entry->age = ++r->text_cache_clock;
              return entry->width;
            }
        }
    }

  while (text[offset] != '\0')
    {
      uint32_t next_offset;
      uint32_t codepoint = utf8_next(text, &offset);
      uint32_t next;
      next_offset = offset;
      next = text[next_offset] == '\0' ? 0 : utf8_next(text, &next_offset);
      if (lv_font_get_glyph_dsc(font, &descriptor, codepoint, next))
        {
          width += descriptor.adv_w;
        }
    }

  if (length < TEXT_CACHE_BYTES)
    {
      struct text_cache_s *entry = &r->text_cache[0];

      for (index = 0; index < TEXT_CACHE_COUNT; index++)
        {
          if (!r->text_cache[index].valid)
            {
              entry = &r->text_cache[index];
              break;
            }

          if (r->text_cache[index].age < entry->age)
            {
              entry = &r->text_cache[index];
            }
        }

      entry->font = font;
      entry->width = width;
      entry->age = ++r->text_cache_clock;
      entry->valid = true;
      memcpy(entry->text, text, length + 1);
    }

  return width;
}

static void text_center_at(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, const lv_font_t *font,
                           const char *text, float center_x, float center_y,
                           uint32_t color, float opacity)
{
  lv_draw_label_dsc_t descriptor;
  lv_area_t area;
  float screen_x;
  float screen_y;
  int width;

  font = renderer_font(r, font);
  width = text_width(r, font, text);
  to_screen(&e->t, center_x, center_y, &screen_x, &screen_y);
  area.x1 = lroundf(screen_x - width * 0.5f);
  area.y1 = lroundf(screen_y - font->line_height * 0.5f);
  area.x2 = area.x1 + width + 1;
  area.y2 = area.y1 + font->line_height + 1;

  lv_draw_label_dsc_init(&descriptor);
  descriptor.text = text;
  descriptor.font = font;
  descriptor.color = lv_color_hex(color);
  descriptor.opa = vector_opa(opacity);
  descriptor.align = LV_TEXT_ALIGN_CENTER;
  lv_draw_label(&r->layer, &descriptor, &area);
}

static float eye_globe_radius(void) { return R - 2.0f; }

static void clamp_to_eye_globe(float *x, float *y, float inset)
{
  float limit = fmaxf(0.0f, eye_globe_radius() - inset);
  float length_squared = *x * *x + *y * *y;

  if (length_squared > limit * limit && length_squared > 0.0f)
    {
      float scale = limit / sqrtf(length_squared);
      *x *= scale;
      *y *= scale;
    }
}

static void text_center(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e, const lv_font_t *font,
                        const char *text, float center_y, uint32_t color,
                        float opacity)
{
  text_center_at(r, e, font, text, 0.0f, center_y, color, opacity);
}

static float cubic(float a, float b, float c, float d, float t)
{
  float u = 1.0f - t;
  return u * u * u * a + 3.0f * u * u * t * b + 3.0f * u * t * t * c +
         t * t * t * d;
}

static float lid_y(float x, float extent, float left, float right, float bulge,
                   bool top)
{
  float x0 = top ? extent * 1.3f : -extent * 1.3f;
  float x1 = top ? extent * 0.4f : -extent * 0.45f;
  float x2 = top ? -extent * 0.4f : extent * 0.45f;
  float x3 = top ? -extent * 1.3f : extent * 1.3f;
  float y0 = top ? right : left;
  float y3 = top ? left : right;
  float low = 0.0f;
  float high = 1.0f;
  int i;

  for (i = 0; i < 9; i++)
    {
      float mid = (low + high) * 0.5f;
      bool forward = top ? cubic(x0, x1, x2, x3, mid) > x
                         : cubic(x0, x1, x2, x3, mid) < x;
      if (forward)
        {
          low = mid;
        }
      else
        {
          high = mid;
        }
    }

  return cubic(y0, y0 + bulge, y3 + bulge, y3, (low + high) * 0.5f);
}

static bool in_lids(const struct eye_s *e, float sx, float sy)
{
  float x;
  float y;
  int index;

  to_local(&e->t, sx, sy, &x, &y);
  index = (int)roundf(x) + LID_OFFSET;
  if ((unsigned int)index >= LID_SAMPLES)
    {
      return true;
    }

  return y <= e->top_y[index] || y >= e->bottom_y[index];
}

static void disk(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float x, float y, float radius, uint32_t color, float opacity,
                 bool lid_clip)
{
  lv_fpoint_t center = { x, y };

  (void)e;
  (void)lid_clip;
  lv_vector_path_clear(r->path);
  lv_vector_path_append_circle(r->path, &center, radius, radius);
  vector_identity(r);
  vector_fill(r, color, opacity);
}

static void line(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float x1, float y1, float x2, float y2, float width,
                 uint32_t color, float opacity, bool lid_clip)
{
  float sx1;
  float sy1;
  float sx2;
  float sy2;
  lv_fpoint_t start;
  lv_fpoint_t end;

  lv_vector_path_clear(r->path);
  if (lid_clip)
    {
      bool drawing = false;
      int index;

      for (index = 0; index <= 32; index++)
        {
          float amount = index / 32.0f;
          float x = x1 + (x2 - x1) * amount;
          float y = y1 + (y2 - y1) * amount;
          bool visible;

          to_screen(&e->t, x, y, &sx1, &sy1);
          visible = in_lids(e, sx1, sy1);
          start = (lv_fpoint_t){ sx1, sy1 };
          if (visible && !drawing)
            {
              lv_vector_path_move_to(r->path, &start);
            }
          else if (visible)
            {
              lv_vector_path_line_to(r->path, &start);
            }

          drawing = visible;
        }
    }
  else
    {
      to_screen(&e->t, x1, y1, &sx1, &sy1);
      to_screen(&e->t, x2, y2, &sx2, &sy2);
      start = (lv_fpoint_t){ sx1, sy1 };
      end = (lv_fpoint_t){ sx2, sy2 };
      lv_vector_path_move_to(r->path, &start);
      lv_vector_path_line_to(r->path, &end);
    }

  vector_identity(r);
  vector_stroke(r, color, opacity, width);
}

static void arc(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                float cx, float cy, float radius, float start, float sweep,
                float width, uint32_t color, float opacity)
{
  lv_fpoint_t center = { cx, cy };

  lv_vector_path_clear(r->path);
  lv_vector_path_append_arc(r->path, &center, radius, start * 180.0f / PI,
                            sweep * 180.0f / PI, false);
  vector_eye_transform(r, e);
  vector_stroke(r, color, opacity, width);
}

static void base(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  lv_gradient_stop_t glow_stops[2];
  lv_gradient_stop_t iris_stops[4];
  lv_fpoint_t center;
  float globe_radius = eye_globe_radius();

  memset(glow_stops, 0, sizeof(glow_stops));
  glow_stops[0].color = lv_color_hex(e->f->iris_rgb);
  glow_stops[0].opa = vector_opa(e->f->glow * 0.175f);
  glow_stops[0].frac = 0;
  glow_stops[1].color = lv_color_hex(e->f->iris_rgb);
  glow_stops[1].opa = LV_OPA_TRANSP;
  glow_stops[1].frac = 255;

  center = (lv_fpoint_t){ 0.0f, 0.0f };
  lv_vector_path_clear(r->path);
  lv_vector_path_append_circle(r->path, &center, R, R);
  vector_eye_transform(r, e);
  lv_vector_dsc_set_stroke_opa(r->vector, LV_OPA_TRANSP);
  lv_vector_dsc_set_fill_opa(r->vector, LV_OPA_COVER);
  lv_vector_dsc_set_fill_radial_gradient(r->vector, 0.0f, 0.0f, R);
  lv_vector_dsc_set_fill_gradient_color_stops(r->vector, glow_stops, 2);
  lv_vector_dsc_set_fill_gradient_spread(r->vector,
                                         LV_VECTOR_GRADIENT_SPREAD_PAD);
  lv_vector_dsc_add_path(r->vector, r->path);

  memset(iris_stops, 0, sizeof(iris_stops));
  iris_stops[0].color = lv_color_hex(shade(e->f->iris_rgb, 1.25f));
  iris_stops[0].opa = LV_OPA_COVER;
  iris_stops[0].frac = 0;
  iris_stops[1].color = lv_color_hex(e->f->iris_rgb);
  iris_stops[1].opa = LV_OPA_COVER;
  iris_stops[1].frac = 140;
  iris_stops[2].color = lv_color_hex(shade(e->f->iris_rgb, 0.55f));
  iris_stops[2].opa = LV_OPA_COVER;
  iris_stops[2].frac = 217;
  iris_stops[3].color = lv_color_hex(shade(e->f->iris_rgb, 0.30f));
  iris_stops[3].opa = LV_OPA_COVER;
  iris_stops[3].frac = 255;

  /* The globe stays fixed. Gaze changes the projected lighting and the
   * internal features; translating this circle would make a flat iris disc
   * slide beyond the physical eye rim. */

  center = (lv_fpoint_t){ 0.0f, 0.0f };
  lv_vector_path_clear(r->path);
  lv_vector_path_append_circle(r->path, &center, globe_radius, globe_radius);
  vector_eye_transform(r, e);
  lv_vector_dsc_set_fill_opa(r->vector, LV_OPA_COVER);
  lv_vector_dsc_set_fill_radial_gradient(r->vector, 0.0f, 0.0f, globe_radius);
  lv_vector_dsc_set_fill_gradient_color_stops(r->vector, iris_stops, 4);
  lv_vector_dsc_set_fill_gradient_spread(r->vector,
                                         LV_VECTOR_GRADIENT_SPREAD_PAD);
  lv_vector_dsc_add_path(r->vector, r->path);
}

static lv_draw_buf_t *base_cache_get(struct nyabula_eye_renderer_s *r,
                                     const struct eye_s *e)
{
  struct base_cache_s *entry = NULL;
  lv_vector_dsc_t *main_vector;
  lv_vector_path_t *main_path;
  uint16_t glow;
  int16_t gaze_x;
  int16_t gaze_y;
  uint16_t iris_radius;
  uint16_t scale_y;
  int index;

  if (fabsf(e->t.sn) > 0.0001f || fabsf(e->t.cs - 1.0f) > 0.0001f)
    {
      return NULL;
    }

  glow = (uint16_t)lroundf(clampf(e->f->glow, 0.0f, 1.0f) * 4095.0f);
  gaze_x = (int16_t)lroundf(e->gx * 16.0f);
  gaze_y = (int16_t)lroundf(e->gy * 16.0f);
  iris_radius = (uint16_t)lroundf(e->ir * 16.0f);
  scale_y = (uint16_t)lroundf(clampf(e->t.sy, 0.0f, 1.0f) * 4095.0f);
  for (index = 0; index < BASE_CACHE_COUNT; index++)
    {
      struct base_cache_s *candidate = &r->base_cache[index];

      if (candidate->valid && candidate->iris_rgb == e->f->iris_rgb &&
          candidate->glow == glow && candidate->gaze_x == gaze_x &&
          candidate->gaze_y == gaze_y &&
          candidate->iris_radius == iris_radius &&
          candidate->scale_y == scale_y)
        {
          candidate->age = ++r->base_cache_clock;
          r->base_cache_hits++;
          return candidate->buffer;
        }

      if (entry == NULL || !candidate->valid || candidate->age < entry->age)
        {
          entry = candidate;
          if (!candidate->valid)
            {
              break;
            }
        }
    }

  if (entry == NULL || entry->canvas == NULL || entry->buffer == NULL)
    {
      return NULL;
    }

  main_vector = r->vector;
  main_path = r->path;
  lv_canvas_fill_bg(entry->canvas, lv_color_black(), LV_OPA_COVER);
  lv_canvas_init_layer(entry->canvas, &r->cache_layer);
  if (r->cache_vector == NULL)
    {
      r->cache_vector = lv_vector_dsc_create(&r->cache_layer);
    }

  if (r->cache_path == NULL)
    {
      r->cache_path = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_HIGH);
    }

  r->vector = r->cache_vector;
  r->path = r->cache_path;
  if (r->vector == NULL || r->path == NULL)
    {
      lv_canvas_finish_layer(entry->canvas, &r->cache_layer);
      r->vector = main_vector;
      r->path = main_path;
      return NULL;
    }

  base(r, e);
  iris(r, e);
  arc(r, e, 0.0f, 0.0f, R - 0.8f, 0.0f, PI * 2.0f, 2.0f, 0x3c4655, 0.45f);
  lv_draw_vector(r->vector);
  lv_canvas_finish_layer(entry->canvas, &r->cache_layer);
  lv_draw_buf_flush_cache(entry->buffer, NULL);
  r->vector = main_vector;
  r->path = main_path;
  entry->iris_rgb = e->f->iris_rgb;
  entry->glow = glow;
  entry->gaze_x = gaze_x;
  entry->gaze_y = gaze_y;
  entry->iris_radius = iris_radius;
  entry->scale_y = scale_y;
  entry->age = ++r->base_cache_clock;
  entry->valid = true;
  r->base_cache_builds++;
  return entry->buffer;
}

static bool page_base_matches(const struct page_state_s *state,
                              const struct eye_s *e)
{
  return state->valid && state->iris_rgb == e->f->iris_rgb &&
         state->glow ==
             (uint16_t)lroundf(clampf(e->f->glow, 0.0f, 1.0f) * 4095.0f) &&
         state->gaze_x == (int16_t)lroundf(e->gx * 16.0f) &&
         state->gaze_y == (int16_t)lroundf(e->gy * 16.0f) &&
         state->iris_radius == (uint16_t)lroundf(e->ir * 16.0f) &&
         state->scale_y ==
             (uint16_t)lroundf(clampf(e->t.sy, 0.0f, 1.0f) * 4095.0f);
}

static void page_remember_base(struct page_state_s *state,
                               const struct eye_s *e)
{
  state->iris_rgb = e->f->iris_rgb;
  state->glow = (uint16_t)lroundf(clampf(e->f->glow, 0.0f, 1.0f) * 4095.0f);
  state->gaze_x = (int16_t)lroundf(e->gx * 16.0f);
  state->gaze_y = (int16_t)lroundf(e->gy * 16.0f);
  state->iris_radius = (uint16_t)lroundf(e->ir * 16.0f);
  state->scale_y = (uint16_t)lroundf(clampf(e->t.sy, 0.0f, 1.0f) * 4095.0f);
  state->valid = true;
}

static void page_remember_dynamic_area(struct page_state_s *state,
                                       const struct eye_s *e, bool simple)
{
  float rx;
  float ry;
  float px;
  float py;
  float left;
  float top;
  float right;
  float bottom;
  float radius;

  if (!simple)
    {
      state->dirty = (lv_area_t){ 0, 0, W - 1, H - 1 };
      return;
    }

  rx = e->ir * e->f->pupil_width * 0.80f;
  ry = e->ir * e->f->pupil_height * 0.84f;
  px = e->gx;
  py = e->gy;
  clamp_to_eye_globe(&px, &py, fmaxf(rx, ry) + 1.0f);
  left = px - rx;
  right = px + rx;
  top = py - ry;
  bottom = py + ry;

  radius = e->ir * 0.13f;
  left = fminf(left, -e->ir * 0.33f + e->gx * 0.55f - radius);
  right = fmaxf(right, -e->ir * 0.33f + e->gx * 0.55f + radius);
  top = fminf(top, -e->ir * 0.38f + e->gy * 0.55f - radius);
  bottom = fmaxf(bottom, -e->ir * 0.38f + e->gy * 0.55f + radius);

  radius = e->ir * 0.05f;
  left = fminf(left, e->ir * 0.30f + e->gx * 0.60f - radius);
  right = fmaxf(right, e->ir * 0.30f + e->gx * 0.60f + radius);
  top = fminf(top, e->ir * 0.24f + e->gy * 0.60f - radius);
  bottom = fmaxf(bottom, e->ir * 0.24f + e->gy * 0.60f + radius);

  state->dirty.x1 = (int32_t)floorf(e->t.cx + left - 4.0f);
  state->dirty.x2 = (int32_t)ceilf(e->t.cx + right + 4.0f);
  state->dirty.y1 = (int32_t)floorf(e->t.cy + top * e->t.sy - 4.0f);
  state->dirty.y2 = (int32_t)ceilf(e->t.cy + bottom * e->t.sy + 4.0f);
  state->dirty.x1 = state->dirty.x1 < 0 ? 0 : state->dirty.x1;
  state->dirty.y1 = state->dirty.y1 < 0 ? 0 : state->dirty.y1;
  state->dirty.x2 = state->dirty.x2 >= W ? W - 1 : state->dirty.x2;
  state->dirty.y2 = state->dirty.y2 >= H ? H - 1 : state->dirty.y2;
}

static lv_area_t page_dirty_union(const struct page_state_s *first,
                                  const struct page_state_s *second)
{
  lv_area_t area;

  area.x1 =
      first->dirty.x1 < second->dirty.x1 ? first->dirty.x1 : second->dirty.x1;
  area.y1 =
      first->dirty.y1 < second->dirty.y1 ? first->dirty.y1 : second->dirty.y1;
  area.x2 =
      first->dirty.x2 > second->dirty.x2 ? first->dirty.x2 : second->dirty.x2;
  area.y2 =
      first->dirty.y2 > second->dirty.y2 ? first->dirty.y2 : second->dirty.y2;
  return area;
}

static void iris(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  uint32_t color = shade(e->f->iris_rgb, 1.6f);
  int i;

  for (i = 0; i < FIBERS; i++)
    {
      const struct fiber_s *fiber = &r->fibers[i];
      float r1 = e->ir * fiber->inner_radius;
      float r2 = e->ir * fiber->outer_radius;
      float x1 = e->gx + fiber->cosine * r1;
      float y1 = e->gy + fiber->sine * r1;
      float x2 = e->gx * 0.3f + fiber->cosine * r2;
      float y2 = e->gy * 0.3f + fiber->sine * r2;

      clamp_to_eye_globe(&x1, &y1, 1.0f);
      clamp_to_eye_globe(&x2, &y2, 1.0f);
      line(r, e, x1, y1, x2, y2, 1.0f, color, 0.16f, false);
    }
}

static void ellipse_path(struct nyabula_eye_renderer_s *r, float cx, float cy,
                         float rx, float ry, float rotation)
{
  const float kappa = 0.5522847498307936f;
  static const float cosine[5] = { 1.0f, 0.0f, -1.0f, 0.0f, 1.0f };
  static const float sine[5] = { 0.0f, 1.0f, 0.0f, -1.0f, 0.0f };
  float sn = rotation == 0.0f ? 0.0f : sinf(rotation);
  float cs = rotation == 0.0f ? 1.0f : cosf(rotation);
  lv_fpoint_t axis_x = { rx * cs, rx * sn };
  lv_fpoint_t axis_y = { -ry * sn, ry * cs };
  lv_fpoint_t p0 = { cx + axis_x.x, cy + axis_x.y };
  lv_fpoint_t c1;
  lv_fpoint_t c2;
  lv_fpoint_t p;
  int segment;

  lv_vector_path_clear(r->path);
  lv_vector_path_move_to(r->path, &p0);
  for (segment = 0; segment < 4; segment++)
    {
      float ca0 = cosine[segment];
      float sa0 = sine[segment];
      float ca1 = cosine[segment + 1];
      float sa1 = sine[segment + 1];
      lv_fpoint_t start = { cx + axis_x.x * ca0 + axis_y.x * sa0,
                            cy + axis_x.y * ca0 + axis_y.y * sa0 };

      c1 = (lv_fpoint_t){ start.x + kappa * (-axis_x.x * sa0 + axis_y.x * ca0),
                          start.y +
                              kappa * (-axis_x.y * sa0 + axis_y.y * ca0) };
      p = (lv_fpoint_t){ cx + axis_x.x * ca1 + axis_y.x * sa1,
                         cy + axis_x.y * ca1 + axis_y.y * sa1 };
      c2 = (lv_fpoint_t){ p.x - kappa * (-axis_x.x * sa1 + axis_y.x * ca1),
                          p.y - kappa * (-axis_x.y * sa1 + axis_y.y * ca1) };
      lv_vector_path_cubic_to(r->path, &c1, &c2, &p);
    }

  lv_vector_path_close(r->path);
}

static void ellipse(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                    float cx, float cy, float rx, float ry, float rotation,
                    uint32_t color, float opacity)
{
  ellipse_path(r, cx, cy, rx, ry, rotation);
  vector_eye_transform(r, e);
  vector_fill(r, color, opacity);
}

static void pupil(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  float rx = e->ir * e->f->pupil_width * 0.80f;
  float ry = e->ir * e->f->pupil_height * 0.84f;
  float px = e->gx;
  float py = e->gy;
  lv_gradient_stop_t stops[3];

  /* Keep the complete antialiased outline inside the fixed globe without
   * changing the pupil dimensions defined by the Web Demo. */

  clamp_to_eye_globe(&px, &py, fmaxf(rx, ry) + 1.0f);
  if (rx > 0.01f && ry > 0.01f)
    {
      memset(stops, 0, sizeof(stops));
      stops[0].color = lv_color_hex(0x101216);
      stops[0].opa = LV_OPA_COVER;
      stops[0].frac = 0;
      stops[1].color = lv_color_hex(0x05070a);
      stops[1].opa = LV_OPA_COVER;
      stops[1].frac = 204;
      stops[2].color = lv_color_hex(0x000000);
      stops[2].opa = LV_OPA_COVER;
      stops[2].frac = 255;

      ellipse_path(r, px, py, rx, ry, 0.0f);
      vector_eye_transform(r, e);
      lv_vector_dsc_set_fill_opa(r->vector, LV_OPA_COVER);
      lv_vector_dsc_set_fill_radial_gradient(r->vector, px, py, fmaxf(rx, ry));
      lv_vector_dsc_set_fill_gradient_color_stops(r->vector, stops, 3);
      lv_vector_dsc_set_fill_gradient_spread(r->vector,
                                             LV_VECTOR_GRADIENT_SPREAD_PAD);
      lv_vector_dsc_set_stroke_color(
          r->vector, lv_color_hex(shade(e->f->iris_rgb, 1.7f)));
      lv_vector_dsc_set_stroke_opa(r->vector, vector_opa(0.5f));
      lv_vector_dsc_set_stroke_width(r->vector, e->ir * 0.02f);
      lv_vector_dsc_add_path(r->vector, r->path);
    }
}

static void star(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float cx, float cy, float radius, float rotation,
                 uint32_t color, float opacity)
{
  int i;

  lv_vector_path_clear(r->path);
  for (i = 0; i < 10; i++)
    {
      float a = rotation + i * PI / 5.0f;
      float pr = (i & 1) ? radius * 0.45f : radius;
      lv_fpoint_t point = { cx + cosf(a) * pr, cy + sinf(a) * pr };
      if (i == 0)
        {
          lv_vector_path_move_to(r->path, &point);
        }
      else
        {
          lv_vector_path_line_to(r->path, &point);
        }
    }

  lv_vector_path_close(r->path);
  vector_eye_transform(r, e);
  vector_fill(r, color, opacity);
}

static void heart(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                  float cx, float cy, float radius, uint32_t color,
                  float opacity)
{
  int sample;

  lv_vector_path_clear(r->path);
  for (sample = 0; sample < HEART_SAMPLES; sample++)
    {
      lv_fpoint_t point = { cx + r->heart_points[sample].x * radius,
                            cy + r->heart_points[sample].y * radius };
      if (sample == 0)
        {
          lv_vector_path_move_to(r->path, &point);
        }
      else
        {
          lv_vector_path_line_to(r->path, &point);
        }
    }

  lv_vector_path_close(r->path);
  vector_eye_transform(r, e);
  vector_fill(r, color, opacity);
}

static lv_fpoint_t icon_point(const struct nyabula_eye_icon_s *icon,
                              float scale, int32_t x, int32_t y)
{
  float half_width = icon->width * 0.5f;
  float half_height = icon->height * 0.5f;

  return (lv_fpoint_t){ (x - half_width) * scale, (y - half_height) * scale };
}

static void icon_append_path(struct nyabula_eye_renderer_s *r,
                             const struct nyabula_eye_icon_s *icon, float size)
{
  float scale = R * size / icon->width;
  int index;

  lv_vector_path_clear(r->path);
  for (index = 0; index < icon->command_count; index++)
    {
      const struct nyabula_eye_icon_command_s *command =
          &icon->commands[index];
      lv_fpoint_t p1 = icon_point(icon, scale, command->x1, command->y1);
      lv_fpoint_t p2 = icon_point(icon, scale, command->x2, command->y2);
      lv_fpoint_t p3 = icon_point(icon, scale, command->x3, command->y3);

      switch (command->operation)
        {
          case NYABULA_EYE_ICON_MOVE:
            lv_vector_path_move_to(r->path, &p1);
            break;
          case NYABULA_EYE_ICON_LINE:
            lv_vector_path_line_to(r->path, &p1);
            break;
          case NYABULA_EYE_ICON_QUAD:
            lv_vector_path_quad_to(r->path, &p1, &p2);
            break;
          case NYABULA_EYE_ICON_CUBIC:
            lv_vector_path_cubic_to(r->path, &p1, &p2, &p3);
            break;
          case NYABULA_EYE_ICON_CLOSE:
            lv_vector_path_close(r->path);
            break;
          default:
            break;
        }
    }
}

static void icon_fill(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                      const struct nyabula_eye_icon_s *icon, float size,
                      uint32_t color, float opacity)
{
  icon_append_path(r, icon, size);

  vector_eye_transform(r, e);
  lv_vector_dsc_set_fill_rule(r->vector, LV_VECTOR_FILL_NONZERO);
  vector_fill(r, color, opacity);
}

static lv_fpoint_t icon_curve_point(const lv_fpoint_t *start,
                                    const lv_fpoint_t *control1,
                                    const lv_fpoint_t *control2,
                                    const lv_fpoint_t *end, float amount,
                                    bool cubic_curve)
{
  float inverse = 1.0f - amount;
  lv_fpoint_t point;

  if (cubic_curve)
    {
      point.x = inverse * inverse * inverse * start->x +
                3.0f * inverse * inverse * amount * control1->x +
                3.0f * inverse * amount * amount * control2->x +
                amount * amount * amount * end->x;
      point.y = inverse * inverse * inverse * start->y +
                3.0f * inverse * inverse * amount * control1->y +
                3.0f * inverse * amount * amount * control2->y +
                amount * amount * amount * end->y;
    }
  else
    {
      point.x = inverse * inverse * start->x +
                2.0f * inverse * amount * control1->x +
                amount * amount * end->x;
      point.y = inverse * inverse * start->y +
                2.0f * inverse * amount * control1->y +
                amount * amount * end->y;
    }

  return point;
}

static int icon_flatten(const struct nyabula_eye_icon_s *icon, float size,
                        lv_fpoint_t *points, bool *moves, int capacity)
{
  float scale = R * size / icon->width;
  lv_fpoint_t current = { 0.0f, 0.0f };
  lv_fpoint_t origin = current;
  int count = 0;
  int index;

  for (index = 0; index < icon->command_count && count < capacity; index++)
    {
      const struct nyabula_eye_icon_command_s *command =
          &icon->commands[index];
      lv_fpoint_t p1 = icon_point(icon, scale, command->x1, command->y1);
      lv_fpoint_t p2 = icon_point(icon, scale, command->x2, command->y2);
      lv_fpoint_t p3 = icon_point(icon, scale, command->x3, command->y3);
      int step;

      if (command->operation == NYABULA_EYE_ICON_MOVE)
        {
          current = p1;
          origin = p1;
          points[count] = p1;
          moves[count++] = true;
        }
      else if (command->operation == NYABULA_EYE_ICON_LINE)
        {
          current = p1;
          points[count] = p1;
          moves[count++] = false;
        }
      else if (command->operation == NYABULA_EYE_ICON_QUAD ||
               command->operation == NYABULA_EYE_ICON_CUBIC)
        {
          lv_fpoint_t start = current;
          bool cubic_curve = command->operation == NYABULA_EYE_ICON_CUBIC;
          lv_fpoint_t end = cubic_curve ? p3 : p2;

          for (step = 1; step <= 16 && count < capacity; step++)
            {
              points[count] = icon_curve_point(&start, &p1, &p2, &end,
                                               step / 16.0f, cubic_curve);
              moves[count++] = false;
            }

          current = end;
        }
      else if (command->operation == NYABULA_EYE_ICON_CLOSE)
        {
          current = origin;
          points[count] = origin;
          moves[count++] = false;
        }
    }

  return count;
}

static void icon_stroke(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_icon_s *icon, float size,
                        uint32_t color, float opacity, float reveal)
{
  lv_fpoint_t *points = r->icon_points;
  bool *moves = r->icon_moves;
  float total = 0.0f;
  float visible;
  float walked = 0.0f;
  int count;
  int index;

  if (reveal <= 0.0f || opacity <= 0.0f)
    {
      return;
    }

  if (reveal >= 0.9999f)
    {
      icon_append_path(r, icon, size);
      vector_eye_transform(r, e);
      vector_stroke(r, color, opacity, R * 0.028f);
      return;
    }

  count = icon_flatten(icon, size, points, moves, 2048);
  for (index = 1; index < count; index++)
    {
      if (!moves[index])
        {
          total += hypotf(points[index].x - points[index - 1].x,
                          points[index].y - points[index - 1].y);
        }
    }

  visible = total * clampf(reveal, 0.0f, 1.0f);
  lv_vector_path_clear(r->path);
  for (index = 0; index < count && walked < visible; index++)
    {
      if (index == 0 || moves[index])
        {
          lv_vector_path_move_to(r->path, &points[index]);
        }
      else
        {
          float dx = points[index].x - points[index - 1].x;
          float dy = points[index].y - points[index - 1].y;
          float length = hypotf(dx, dy);
          float part =
              clampf((visible - walked) / fmaxf(length, 0.001f), 0.0f, 1.0f);
          lv_fpoint_t end = { points[index - 1].x + dx * part,
                              points[index - 1].y + dy * part };

          lv_vector_path_line_to(r->path, &end);
          walked += length;
        }
    }

  vector_eye_transform(r, e);
  vector_stroke(r, color, opacity, R * 0.028f);
}

static void icon(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 const char *name, float size, uint32_t color, float opacity,
                 float reveal)
{
  const struct nyabula_eye_icon_s *asset = nyabula_eye_icon_find(name);
  float fill = clampf((reveal - 0.46f) / 0.54f, 0.0f, 1.0f);

  if (asset == NULL)
    {
      return;
    }

  icon_stroke(r, e, asset, size, color, opacity * (0.88f - fill * 0.68f),
              reveal);
  if (fill > 0.0f)
    {
      icon_fill(r, e, asset, size, color, opacity * 0.92f * fill);
    }
}

static void icon_at(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                    const char *name, float size, float x, float y,
                    float rotation, uint32_t color, float opacity,
                    float reveal)
{
  struct eye_s placed = *e;
  float cosine = rotation == 0.0f ? 1.0f : cosf(rotation);
  float sine = rotation == 0.0f ? 0.0f : sinf(rotation);
  float old_cosine = e->t.cs;
  float old_sine = e->t.sn;

  to_screen(&e->t, x, y, &placed.t.cx, &placed.t.cy);
  placed.t.cs = old_cosine * cosine - old_sine * sine;
  placed.t.sn = old_sine * cosine + old_cosine * sine;
  icon(r, &placed, name, size, color, opacity, reveal);
}

static void icon_group_at(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e, const char *name, float size,
                          float x, float y, float rotation, uint32_t color,
                          float opacity, float reveal)
{
  const struct nyabula_eye_icon_s *asset = nyabula_eye_icon_find(name);
  struct eye_s placed = *e;
  float cosine = rotation == 0.0f ? 1.0f : cosf(rotation);
  float sine = rotation == 0.0f ? 0.0f : sinf(rotation);
  float old_cosine = e->t.cs;
  float old_sine = e->t.sn;
  float fill = clampf((reveal - 0.46f) / 0.54f, 0.0f, 1.0f);

  if (asset == NULL)
    {
      return;
    }

  to_screen(&e->t, x, y, &placed.t.cx, &placed.t.cy);
  placed.t.cs = old_cosine * cosine - old_sine * sine;
  placed.t.sn = old_sine * cosine + old_cosine * sine;

  if (fill < 1.0f)
    {
      icon_stroke(r, &placed, asset, size, color, opacity * (1.0f - fill),
                  reveal);
    }

  if (fill > 0.0f)
    {
      icon_fill(r, &placed, asset, size, color, opacity * fill);
    }
}

static void icon_outline(struct nyabula_eye_renderer_s *r,
                         const struct eye_s *e, const char *name, float size,
                         float width, uint32_t color, float opacity)
{
  const struct nyabula_eye_icon_s *asset = nyabula_eye_icon_find(name);
  if (asset == NULL)
    {
      return;
    }

  icon_append_path(r, asset, size);
  vector_eye_transform(r, e);
  vector_stroke(r, color, opacity, width);
}

static void filled_rect(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e, float left, float top,
                        float right, float bottom, uint32_t color,
                        float opacity)
{
  lv_fpoint_t point = { left, top };

  lv_vector_path_clear(r->path);
  lv_vector_path_move_to(r->path, &point);
  point = (lv_fpoint_t){ right, top };
  lv_vector_path_line_to(r->path, &point);
  point = (lv_fpoint_t){ right, bottom };
  lv_vector_path_line_to(r->path, &point);
  point = (lv_fpoint_t){ left, bottom };
  lv_vector_path_line_to(r->path, &point);
  lv_vector_path_close(r->path);
  vector_eye_transform(r, e);
  vector_fill(r, color, opacity);
}

static const char *scene_text(const char *value, const char *fallback)
{
  return value[0] == '\0' ? fallback : value;
}

static uint32_t scene_color(const struct eye_s *e)
{
  return shade(e->f->iris_rgb, 1.75f);
}

static void scene_backdrop(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, bool minimal, float opacity)
{
  float center_x;
  float center_y;

  if (minimal)
    {
      return;
    }

  to_screen(&e->t, 0.0f, 0.0f, &center_x, &center_y);
  disk(r, e, center_x, center_y, R * 0.93f, 0x000307, opacity, false);
}

static void scene_dial(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       uint32_t color, float progress, float opacity)
{
  arc(r, e, 0.0f, 0.0f, R * 0.70f, -PI * 0.5f, PI * 2.0f, R * 0.035f, color,
      opacity * 0.17f);
  if (progress > 0.0f)
    {
      arc(r, e, 0.0f, 0.0f, R * 0.70f, -PI * 0.5f,
          clampf(progress, 0.0f, 1.0f) * PI * 2.0f, R * 0.035f, color,
          opacity * 0.82f);
    }
}

static void rounded_rect_outline(struct nyabula_eye_renderer_s *r,
                                 const struct eye_s *e, float left, float top,
                                 float right, float bottom, float radius,
                                 float width, uint32_t color, float opacity)
{
  line(r, e, left + radius, top, right - radius, top, width, color, opacity,
       false);
  line(r, e, left + radius, bottom, right - radius, bottom, width, color,
       opacity, false);
  line(r, e, left, top + radius, left, bottom - radius, width, color, opacity,
       false);
  line(r, e, right, top + radius, right, bottom - radius, width, color,
       opacity, false);
  arc(r, e, left + radius, top + radius, radius, PI, PI * 0.5f, width, color,
      opacity);
  arc(r, e, right - radius, top + radius, radius, -PI * 0.5f, PI * 0.5f, width,
      color, opacity);
  arc(r, e, right - radius, bottom - radius, radius, 0.0f, PI * 0.5f, width,
      color, opacity);
  arc(r, e, left + radius, bottom - radius, radius, PI * 0.5f, PI * 0.5f,
      width, color, opacity);
}

static void scene_eq_curve(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, uint32_t color,
                           float seconds, float opacity, bool calibrating)
{
  float previous_x = -R * 0.62f;
  float previous_y = R * -0.04f;
  int index;

  for (index = -2; index <= 2; index++)
    {
      line(r, e, -R * 0.62f, index * R * 0.16f, R * 0.62f, index * R * 0.16f,
           R * 0.010f, color, opacity * 0.14f, false);
    }

  for (index = 1; index <= 48; index++)
    {
      float x = R * (-0.62f + index / 48.0f * 1.24f);
      float nx = x / R;
      float y = R * (-0.04f - expf(-powf((nx + 0.28f) * 4.0f, 2.0f)) * 0.16f +
                     expf(-powf((nx - 0.22f) * 5.0f, 2.0f)) * 0.11f);
      line(r, e, previous_x, previous_y, x, y, R * 0.025f, color,
           opacity * 0.88f, false);
      previous_x = x;
      previous_y = y;
    }

  if (calibrating)
    {
      float sweep = fmodf(seconds * 0.20f, 1.0f) * R * 1.24f - R * 0.62f;
      for (index = 0; index < 10; index++)
        {
          float amount = (index + 1) / 10.0f;
          filled_rect(r, e, sweep - R * 0.15f + index * R * 0.015f, -R * 0.42f,
                      sweep - R * 0.15f + (index + 1) * R * 0.015f, R * 0.42f,
                      color, opacity * 0.34f * amount);
        }
    }
}

static void scene_play_triangle(struct nyabula_eye_renderer_s *r,
                                const struct eye_s *e, uint32_t color,
                                float opacity)
{
  int y;
  int top = (int)floorf(-R * 0.27f);
  int middle = (int)lroundf(-R * 0.08f);
  int bottom = (int)ceilf(R * 0.11f);
  float left = -R * 0.105f;
  float tip = R * 0.205f;

  for (y = top; y <= bottom; y++)
    {
      float progress;
      float right;

      if (y <= middle)
        {
          progress = (float)(y - top) / (float)(middle - top);
        }
      else
        {
          progress = (float)(bottom - y) / (float)(bottom - middle);
        }

      right = left + (tip - left) * clampf(progress, 0.0f, 1.0f);
      filled_rect(r, e, left, y, right, y + 1.0f, color, opacity);
    }
}

struct scene_point_s
{
  float x;
  float y;
};

static void scene_append_cubic(struct scene_point_s *points, int *count,
                               struct scene_point_s start,
                               struct scene_point_s control1,
                               struct scene_point_s control2,
                               struct scene_point_s end)
{
  int index;

  for (index = 1; index <= 12; index++)
    {
      float t = index / 12.0f;
      float u = 1.0f - t;
      points[*count].x = u * u * u * start.x + 3.0f * u * u * t * control1.x +
                         3.0f * u * t * t * control2.x + t * t * t * end.x;
      points[*count].y = u * u * u * start.y + 3.0f * u * u * t * control1.y +
                         3.0f * u * t * t * control2.y + t * t * t * end.y;
      (*count)++;
    }
}

static void scene_fill_polygon(struct nyabula_eye_renderer_s *r,
                               const struct eye_s *e,
                               const struct scene_point_s *points, int count,
                               uint32_t top_color, uint32_t bottom_color,
                               float opacity)
{
  float min_y = points[0].y;
  float max_y = points[0].y;
  int index;
  lv_gradient_stop_t stops[2];

  for (index = 1; index < count; index++)
    {
      min_y = fminf(min_y, points[index].y);
      max_y = fmaxf(max_y, points[index].y);
    }

  lv_vector_path_clear(r->path);
  for (index = 0; index < count; index++)
    {
      lv_fpoint_t point = { points[index].x, points[index].y };
      if (index == 0)
        {
          lv_vector_path_move_to(r->path, &point);
        }
      else
        {
          lv_vector_path_line_to(r->path, &point);
        }
    }

  lv_vector_path_close(r->path);
  memset(stops, 0, sizeof(stops));
  stops[0].color = lv_color_hex(top_color);
  stops[0].opa = vector_opa(opacity);
  stops[0].frac = 0;
  stops[1].color = lv_color_hex(bottom_color);
  stops[1].opa = vector_opa(opacity);
  stops[1].frac = 255;
  vector_eye_transform(r, e);
  lv_vector_dsc_set_stroke_opa(r->vector, LV_OPA_TRANSP);
  lv_vector_dsc_set_fill_opa(r->vector, LV_OPA_COVER);
  lv_vector_dsc_set_fill_linear_gradient(r->vector, 0.0f, min_y, 0.0f, max_y);
  lv_vector_dsc_set_fill_gradient_color_stops(r->vector, stops, 2);
  lv_vector_dsc_set_fill_gradient_spread(r->vector,
                                         LV_VECTOR_GRADIENT_SPREAD_PAD);
  lv_vector_dsc_add_path(r->vector, r->path);
}

static void scene_stroke_points(struct nyabula_eye_renderer_s *r,
                                const struct eye_s *e,
                                const struct scene_point_s *points, int count,
                                float width, uint32_t color, float opacity)
{
  int index;

  for (index = 1; index < count; index++)
    {
      line(r, e, points[index - 1].x, points[index - 1].y, points[index].x,
           points[index].y, width, color, opacity, false);
    }
}

static int scene_cloud_points(struct scene_point_s *points, float drift,
                              float offset_y, float scale)
{
  struct scene_point_s current = { -R * 0.46f, R * 0.18f };
  int count = 0;

#define CLOUD_POINT(x, y) \
  ((struct scene_point_s){ drift + R * (x)*scale, offset_y + R * (y)*scale })
#define CLOUD_CUBIC(c1x, c1y, c2x, c2y, ex, ey)                          \
  do                                                                     \
    {                                                                    \
      struct scene_point_s end = CLOUD_POINT(ex, ey);                    \
      scene_append_cubic(points, &count, current, CLOUD_POINT(c1x, c1y), \
                         CLOUD_POINT(c2x, c2y), end);                    \
      current = end;                                                     \
    }                                                                    \
  while (0)

  current = CLOUD_POINT(-0.46f, 0.18f);
  points[count++] = current;
  CLOUD_CUBIC(-0.49f, 0.05f, -0.43f, -0.15f, -0.24f, -0.18f);
  CLOUD_CUBIC(-0.18f, -0.43f, 0.13f, -0.48f, 0.24f, -0.22f);
  CLOUD_CUBIC(0.40f, -0.21f, 0.49f, -0.08f, 0.48f, 0.09f);
  CLOUD_CUBIC(0.48f, 0.18f, 0.41f, 0.22f, 0.30f, 0.22f);
  points[count++] = CLOUD_POINT(-0.39f, 0.22f);
  CLOUD_CUBIC(-0.43f, 0.22f, -0.46f, 0.21f, -0.46f, 0.18f);

#undef CLOUD_CUBIC
#undef CLOUD_POINT
  return count;
}

static void scene_draw_cloud(struct nyabula_eye_renderer_s *r,
                             const struct eye_s *e, uint32_t color,
                             float drift, float offset_y, float scale,
                             float opacity)
{
  struct scene_point_s points[80];
  int count = scene_cloud_points(points, drift + R * 0.035f * scale,
                                 offset_y + R * 0.035f * scale, scale);

  scene_fill_polygon(r, e, points, count, shade(color, 0.38f),
                     shade(color, 0.38f), opacity * 0.55f);
  count = scene_cloud_points(points, drift, offset_y, scale);
  scene_fill_polygon(r, e, points, count, shade(color, 1.06f),
                     shade(color, 0.72f), opacity * 0.96f);
  scene_stroke_points(r, e, points, count, R * 0.014f, shade(color, 1.22f),
                      opacity * 0.28f);
}

static void
scene_music_progress(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                     const struct nyabula_eye_scene_payload_s *payload,
                     float seconds, float opacity)
{
  uint32_t color = scene_color(e);
  float duration =
      payload->duration_ms == 0 ? 235.0f : payload->duration_ms / 1000.0f;
  float position = payload->duration_ms == 0 ? fmodf(seconds, duration)
                                             : payload->position_ms / 1000.0f;
  float progress = clampf(position / duration, 0.0f, 1.0f);
  if (e->id != NYABULA_EYE_LEFT)
    {
      return;
    }

  arc(r, e, 0.0f, 0.0f, R * 0.89f, -PI * 0.5f, PI * 2.0f, R * 0.032f,
      shade(color, 0.38f), opacity * 0.12f);
  arc(r, e, 0.0f, 0.0f, R * 0.89f, -PI * 0.5f, progress * PI * 2.0f,
      R * 0.032f, color, opacity * 0.92f);
  ellipse(r, e, cosf(-PI * 0.5f + progress * PI * 2.0f) * R * 0.89f,
          sinf(-PI * 0.5f + progress * PI * 2.0f) * R * 0.89f, R * 0.024f,
          R * 0.024f, 0.0f, color, opacity * 0.95f);
}

static void scene_music_icon(struct nyabula_eye_renderer_s *r,
                             const struct eye_s *e, bool playing,
                             uint32_t color, float opacity, float scale)
{
  struct eye_s scaled = *e;

  scaled.t.sx *= scale;
  scaled.t.sy *= scale;
  if (playing)
    {
      line(r, &scaled, -R * 0.09f, -R * 0.20f, -R * 0.09f, R * 0.20f,
           R * 0.065f, color, opacity * 0.94f, false);
      line(r, &scaled, R * 0.09f, -R * 0.20f, R * 0.09f, R * 0.20f, R * 0.065f,
           color, opacity * 0.94f, false);
    }
  else
    {
      scene_play_triangle(r, &scaled, color, opacity * 0.94f);
    }
}

static void
scene_music_title(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                  const struct nyabula_eye_scene_payload_s *payload,
                  uint32_t color, float opacity)
{
  text_center(r, e,
              scene_font(r, FONT_FAMILY_ENGLISH, 14, &nyabula_font_english_18),
              scene_text(payload->title, "NYABULA MIX"), R * 0.36f, color,
              opacity * 0.50f);
}

static void scene_music_time(struct nyabula_eye_renderer_s *r,
                             const struct eye_s *e,
                             const struct nyabula_eye_scene_payload_s *payload,
                             float seconds, uint32_t color, float opacity)
{
  float duration =
      payload->duration_ms == 0 ? 235.0f : payload->duration_ms / 1000.0f;
  float position = payload->duration_ms == 0 ? fmodf(seconds, duration)
                                             : payload->position_ms / 1000.0f;
  char value[32];

  snprintf(value, sizeof(value), "%02u:%02u  /  %02u:%02u",
           (unsigned int)position / 60, (unsigned int)position % 60,
           (unsigned int)duration / 60, (unsigned int)duration % 60);
  text_center(r, e,
              scene_font(r, FONT_FAMILY_ENGLISH, 16, &nyabula_font_english_18),
              value, R * 0.49f, color, opacity * 0.76f);
}

static void
scene_music_lyrics(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                   const struct nyabula_eye_scene_payload_s *payload,
                   uint32_t color, float opacity)
{
  text_center(r, e, &nyabula_font_body_18,
              scene_text(payload->previous_line, "灯火落进夜里"), -R * 0.30f,
              color, opacity * 0.22f);
  text_center(r, e, &nyabula_font_title_28,
              scene_text(payload->current_line, "我听见你"), -R * 0.03f, color,
              opacity * 0.94f);
  text_center(r, e, &nyabula_font_body_18,
              scene_text(payload->next_line, "轻轻回应"), R * 0.27f, color,
              opacity * 0.30f);
}

static void scene_music_spectrum(struct nyabula_eye_renderer_s *r,
                                 const struct eye_s *e, float seconds,
                                 uint32_t color, float opacity)
{
  int index;

  text_center(r, e, &nyabula_font_title_20, "频谱", -R * 0.38f, color,
              opacity * 0.32f);
  for (index = 0; index < 9; index++)
    {
      float wave = 0.5f + 0.5f * sinf(seconds * (3.4f + (index % 3) * 0.24f) +
                                      index * 0.88f);
      float envelope = 0.58f + 0.42f * sinf((index + 1) / 10.0f * PI);
      float height = R * (0.13f + 0.49f * wave * envelope);
      float x = (index - 4) * R * 0.107f;
      line(r, e, x, -height * 0.5f + R * 0.10f, x, height * 0.5f + R * 0.10f,
           R * 0.062f, color, opacity * (0.48f + wave * 0.46f), false);
    }
}

static void scene_music(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity)
{
  uint32_t color = scene_color(e);

  if (e->id == NYABULA_EYE_LEFT)
    {
      scene_music_progress(r, e, payload, seconds, opacity);
      scene_music_icon(r, e, payload->playing, color, opacity, 1.0f);
      scene_music_title(r, e, payload, color, opacity);
      scene_music_time(r, e, payload, seconds, color, opacity);
    }
  else if (payload->music_view == NYABULA_EYE_MUSIC_LYRICS)
    {
      scene_music_lyrics(r, e, payload, color, opacity);
    }
  else
    {
      scene_music_spectrum(r, e, seconds, color, opacity);
    }
}

static float scene_music_lerp(float from, float to, float progress)
{
  return from + (to - from) * progress;
}

static bool
scene_music_lyrics_changed(const struct nyabula_eye_scene_payload_s *from,
                           const struct nyabula_eye_scene_payload_s *to)
{
  return strcmp(from->previous_line, to->previous_line) != 0 ||
         strcmp(from->current_line, to->current_line) != 0 ||
         strcmp(from->next_line, to->next_line) != 0;
}

static void
scene_music_lyrics_transition(struct nyabula_eye_renderer_s *r,
                              const struct eye_s *e,
                              const struct nyabula_eye_scene_payload_s *from,
                              const struct nyabula_eye_scene_payload_s *to,
                              uint32_t color, float progress, float opacity)
{
  const char *old_top = scene_text(from->previous_line, "灯火落进夜里");
  const char *old_middle = scene_text(from->current_line, "我听见你");
  const char *old_bottom = scene_text(from->next_line, "轻轻回应");
  const char *new_top = scene_text(to->previous_line, "灯火落进夜里");
  const char *new_middle = scene_text(to->current_line, "我听见你");
  const char *new_bottom = scene_text(to->next_line, "轻轻回应");
  float top_y = scene_music_lerp(-R * 0.30f, -R * 0.03f, progress);
  float middle_y = scene_music_lerp(-R * 0.03f, R * 0.27f, progress);
  float bottom_y = scene_music_lerp(R * 0.27f, R * 0.52f, progress);
  float enter = clampf((progress - 0.08f) / 0.92f, 0.0f, 1.0f);
  float incoming_y;

  enter = enter * enter * (3.0f - 2.0f * enter);
  incoming_y = scene_music_lerp(-R * 0.52f, -R * 0.30f, enter);

  text_center(r, e, &nyabula_font_body_18, old_bottom, bottom_y, color,
              opacity * 0.30f * (1.0f - progress));
  if (strcmp(old_middle, new_bottom) == 0)
    {
      const lv_font_t *font =
          progress < 0.5f ? &nyabula_font_title_28 : &nyabula_font_body_18;
      float alpha = scene_music_lerp(0.94f, 0.30f, progress);

      text_center(r, e, font, old_middle, middle_y, color, opacity * alpha);
    }
  else
    {
      text_center(r, e, &nyabula_font_title_28, old_middle, middle_y, color,
                  opacity * 0.94f * (1.0f - progress));
      text_center(r, e, &nyabula_font_body_18, new_bottom, middle_y, color,
                  opacity * 0.30f * progress);
    }

  if (strcmp(old_top, new_middle) == 0)
    {
      const lv_font_t *font =
          progress < 0.5f ? &nyabula_font_body_18 : &nyabula_font_title_28;
      float alpha = scene_music_lerp(0.22f, 0.94f, progress);

      text_center(r, e, font, old_top, top_y, color, opacity * alpha);
    }
  else
    {
      text_center(r, e, &nyabula_font_body_18, old_top, top_y, color,
                  opacity * 0.22f * (1.0f - progress));
      text_center(r, e, &nyabula_font_title_28, new_middle, top_y, color,
                  opacity * 0.94f * progress);
    }

  text_center(r, e, &nyabula_font_body_18, new_top, incoming_y, color,
              opacity * 0.30f * enter);
}

static void
scene_music_transition(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *from,
                       const struct nyabula_eye_scene_payload_s *to,
                       float seconds, float progress, float opacity)
{
  struct nyabula_eye_scene_payload_s displayed = *to;
  uint32_t color = scene_color(e);

  displayed.duration_ms = (uint32_t)lroundf(
      scene_music_lerp(from->duration_ms, to->duration_ms, progress));
  displayed.position_ms = (uint32_t)lroundf(
      scene_music_lerp(from->position_ms, to->position_ms, progress));
  if (e->id == NYABULA_EYE_LEFT)
    {
      scene_music_progress(r, e, &displayed, seconds, opacity);
      if (from->playing != to->playing)
        {
          scene_music_icon(r, e, from->playing, color,
                           opacity * (1.0f - progress),
                           0.78f + 0.22f * (1.0f - progress));
          scene_music_icon(r, e, to->playing, color, opacity * progress,
                           0.78f + 0.22f * progress);
        }
      else
        {
          scene_music_icon(r, e, to->playing, color, opacity, 1.0f);
        }

      if (strcmp(from->title, to->title) != 0)
        {
          scene_music_title(r, e, from, color, opacity * (1.0f - progress));
          scene_music_title(r, e, to, color, opacity * progress);
        }
      else
        {
          scene_music_title(r, e, to, color, opacity);
        }

      scene_music_time(r, e, &displayed, seconds, color, opacity);
    }
  else if (to->music_view == NYABULA_EYE_MUSIC_LYRICS &&
           scene_music_lyrics_changed(from, to))
    {
      scene_music_lyrics_transition(r, e, from, to, color, progress, opacity);
    }
  else if (to->music_view == NYABULA_EYE_MUSIC_LYRICS)
    {
      scene_music_lyrics(r, e, to, color, opacity);
    }
  else
    {
      scene_music_spectrum(r, e, seconds, color, opacity);
    }
}

static void scene_timer(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity)
{
  uint32_t color = scene_color(e);
  float total =
      payload->duration_ms == 0 ? 300.0f : payload->duration_ms / 1000.0f;
  float remaining = payload->remaining_ms == 0
                        ? fmaxf(0.0f, total - seconds)
                        : payload->remaining_ms / 1000.0f;
  unsigned int rounded = ceilf(remaining);
  char value[12];

  snprintf(value, sizeof(value), "%02u",
           e->id == NYABULA_EYE_LEFT ? rounded / 60 : rounded % 60);
  text_center(r, e, &nyabula_font_english_119, value, -R * 0.03f, color,
              opacity * (remaining <= 10.0f
                             ? 0.82f + sinf(seconds * 8.0f) * 0.18f
                             : 0.96f));
  text_center(r, e, &nyabula_font_body_18,
              e->id == NYABULA_EYE_LEFT ? "分钟" : "秒钟", R * 0.34f, color,
              opacity * 0.48f);
  arc(r, e, 0.0f, 0.0f, R * 0.72f, -PI * 0.5f, PI * 2.0f, R * 0.035f, color,
      opacity * 0.22f);
  arc(r, e, 0.0f, 0.0f, R * 0.72f, -PI * 0.5f,
      clampf(remaining / total, 0.0f, 1.0f) * PI * 2.0f, R * 0.035f, color,
      opacity * 0.82f);
  text_center(r, e, &nyabula_font_body_14,
              scene_text(payload->detail, "计时中"), R * 0.49f, color,
              opacity * 0.34f);
}

static float scene_ease(float value)
{
  float t = clampf(value, 0.0f, 1.0f);
  float u = 1.0f - t;
  return 3.0f * u * u * t * 0.08f + 3.0f * u * t * t * 0.92f + t * t * t;
}

static void scene_draw_drop(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, float x, float y,
                            float size, float tilt, uint32_t color,
                            float opacity)
{
  struct scene_point_s points[52];
  struct scene_point_s current = { 0.0f, -size * 0.72f };
  int count = 0;
  int index;
  float sine = sinf(tilt);
  float cosine = cosf(tilt);

  points[count++] = current;
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){ size * 0.08f, -size * 0.46f },
                     (struct scene_point_s){ size * 0.32f, -size * 0.12f },
                     (struct scene_point_s){ size * 0.32f, size * 0.10f });
  current = points[count - 1];
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){ size * 0.32f, size * 0.40f },
                     (struct scene_point_s){ size * 0.17f, size * 0.58f },
                     (struct scene_point_s){ 0.0f, size * 0.58f });
  current = points[count - 1];
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){ -size * 0.17f, size * 0.58f },
                     (struct scene_point_s){ -size * 0.32f, size * 0.40f },
                     (struct scene_point_s){ -size * 0.32f, size * 0.10f });
  current = points[count - 1];
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){ -size * 0.32f, -size * 0.12f },
                     (struct scene_point_s){ -size * 0.08f, -size * 0.46f },
                     (struct scene_point_s){ 0.0f, -size * 0.72f });

  for (index = 0; index < count; index++)
    {
      float px = points[index].x;
      float py = points[index].y;
      points[index].x = x + px * cosine - py * sine;
      points[index].y = y + px * sine + py * cosine;
    }

  scene_fill_polygon(r, e, points, count, color, color, opacity);
}

static void scene_draw_rain(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, uint32_t color,
                            float seconds, bool storm, float opacity)
{
  static const float rain_x[] = { -0.34f, -0.18f, -0.02f, 0.15f, 0.31f };
  static const float storm_x[] = { -0.39f, -0.27f, -0.14f, 0.0f,
                                   0.13f,  0.26f,  0.39f };
  const float *positions = storm ? storm_x : rain_x;
  int count = storm ? 7 : 5;
  float drift = sinf(seconds * 0.72f) * R * 0.025f;
  float center_y = -R * 0.09f;
  int index;

  for (index = 0; index < count; index++)
    {
      float phase =
          fmodf(seconds * ((storm ? 0.78f : 0.48f) + index * 0.018f) +
                    index * 0.193f,
                1.0f);
      float y = R * (-0.10f + powf(phase, 1.55f) * 0.86f);
      float fade_in = scene_ease(phase / 0.12f);
      float fade_out = 1.0f - scene_ease((y / R - 0.64f) / 0.08f);
      float alpha = fade_in * fade_out * (0.60f + (index % 2) * 0.22f);

      scene_draw_drop(r, e, R * positions[index] + drift, center_y + y,
                      R * (storm ? 0.042f : 0.048f), storm ? -0.18f : -0.12f,
                      color, opacity * alpha);
      if (y >= R * 0.72f)
        {
          float ripple = clampf((y / R - 0.72f) / 0.04f, 0.0f, 1.0f);
          float radius = R * (0.018f + ripple * 0.08f);
          arc(r, e, R * positions[index] + drift, center_y + R * 0.74f, radius,
              0.0f, PI * 2.0f, R * 0.012f, color,
              opacity * (1.0f - ripple) * 0.28f);
        }
    }

  if (storm)
    {
      float flash = fmodf(seconds + 1.37f, 6.4f);
      if (flash < 0.30f)
        {
          struct scene_point_s bolt[28];
          struct scene_point_s current = { -R * 0.07f, center_y - R * 0.13f };
          float alpha = sinf(flash / 0.30f * PI);
          int points = 0;

          bolt[points++] = current;
          scene_append_cubic(
              bolt, &points, current,
              (struct scene_point_s){ R * 0.10f, center_y + R * 0.04f },
              (struct scene_point_s){ -R * 0.10f, center_y + R * 0.20f },
              (struct scene_point_s){ R * 0.02f, center_y + R * 0.34f });
          current = bolt[points - 1];
          scene_append_cubic(
              bolt, &points, current,
              (struct scene_point_s){ R * 0.15f, center_y + R * 0.47f },
              (struct scene_point_s){ -R * 0.02f, center_y + R * 0.57f },
              (struct scene_point_s){ R * 0.08f, center_y + R * 0.68f });
          scene_stroke_points(r, e, bolt, points, R * 0.038f,
                              shade(color, 1.42f), opacity * alpha * 0.94f);
        }
    }

  scene_draw_cloud(r, e, color, drift, center_y, storm ? 0.96f : 1.0f,
                   opacity);
}

static void scene_draw_snow(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, uint32_t color,
                            float seconds, float opacity)
{
  int index;

  for (index = 0; index < 12; index++)
    {
      float phase = fmodf(
          seconds * (0.16f + (index % 3) * 0.018f) + index * 0.083f, 1.0f);
      float x = R * (-0.46f + fmodf(index * 0.37f, 1.0f) * 0.92f) +
                sinf(seconds * 1.3f + index) * R * 0.035f;
      float y = -R * 0.06f + R * (-0.12f + phase * 0.90f);
      float alpha = scene_ease(phase / 0.12f) *
                    (1.0f - scene_ease((phase - 0.84f) / 0.16f));
      int arm;

      for (arm = 0; arm < 3; arm++)
        {
          float angle = seconds * 0.4f + index + arm * PI / 3.0f;
          float dx = cosf(angle) * R * 0.028f;
          float dy = sinf(angle) * R * 0.028f;
          line(r, e, x - dx, y - dy, x + dx, y + dy, R * 0.012f, color,
               opacity * alpha * 0.78f, false);
        }
    }

  scene_draw_cloud(r, e, color, sinf(seconds * 0.5f) * R * 0.018f, -R * 0.06f,
                   0.94f, opacity);
}

static void scene_draw_fog(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, uint32_t color,
                           float seconds, float opacity)
{
  int index;

  for (index = 0; index < 7; index++)
    {
      float y = R * (-0.50f + index * 0.16f);
      float shift = sinf(seconds * 0.35f + index * 0.9f) * R * 0.10f;
      float half = R * (0.26f + (index % 3) * 0.10f);
      struct scene_point_s points[14];
      int count = 0;

      points[count++] = (struct scene_point_s){ -half + shift, y };
      scene_append_cubic(
          points, &count, points[0],
          (struct scene_point_s){ -half * 0.35f + shift, y - R * 0.035f },
          (struct scene_point_s){ half * 0.35f + shift, y + R * 0.035f },
          (struct scene_point_s){ half + shift, y });
      scene_stroke_points(r, e, points, count,
                          R * (0.018f + (index % 2) * 0.008f), color,
                          opacity * (0.22f + index * 0.055f));
    }
}

static void scene_weather(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e,
                          const struct nyabula_eye_scene_payload_s *payload,
                          float seconds, float opacity)
{
  static const char *names[] = {
    "晴朗", "多云", "小雨", "雷雨", "降雪", "有雾"
  };
  static const int temperatures[] = { 28, 21, 23, 19, 1, 16 };
  static const char *left_labels[] = { "紫外", "湿度", "湿度",
                                       "风速", "湿度", "能见" };
  static const char *left_values[] = { "4", "66%", "78%", "24", "91%", "0.8" };
  static const char *right_labels[] = { "体感", "体感", "体感",
                                        "湿度", "体感", "湿度" };
  static const char *right_values[] = { "29°", "20°", "22°",
                                        "86%", "-2°", "95%" };
  uint32_t color = scene_color(e);
  enum nyabula_eye_weather_e weather = payload->weather;
  char value[20];
  char left_value[16];
  char right_value[16];

  if (e->id == NYABULA_EYE_RIGHT)
    {
      int temperature = payload->temperature_c == 0.0f
                            ? temperatures[weather]
                            : (int)lroundf(payload->temperature_c);
      const char *left_value_text = left_values[weather];
      const char *right_value_text = right_values[weather];

      if (weather == NYABULA_EYE_WEATHER_STORM && payload->wind_kph > 0.0f)
        {
          snprintf(left_value, sizeof(left_value), "%d",
                   (int)lroundf(payload->wind_kph));
          left_value_text = left_value;
        }
      else if (weather == NYABULA_EYE_WEATHER_FOG &&
               payload->visibility_km > 0.0f)
        {
          int tenths = (int)lroundf(payload->visibility_km * 10.0f);
          snprintf(left_value, sizeof(left_value), "%d.%d", tenths / 10,
                   abs(tenths % 10));
          left_value_text = left_value;
        }
      else if (weather != NYABULA_EYE_WEATHER_SUNNY &&
               payload->humidity_percent > 0.0f)
        {
          snprintf(left_value, sizeof(left_value), "%d%%",
                   (int)lroundf(payload->humidity_percent));
          left_value_text = left_value;
        }

      if (weather == NYABULA_EYE_WEATHER_STORM &&
          payload->humidity_percent > 0.0f)
        {
          snprintf(right_value, sizeof(right_value), "%d%%",
                   (int)lroundf(payload->humidity_percent));
          right_value_text = right_value;
        }
      else if (payload->feels_like_c != 0.0f)
        {
          snprintf(right_value, sizeof(right_value), "%d°",
                   (int)lroundf(payload->feels_like_c));
          right_value_text = right_value;
        }

      arc(r, e, 0.0f, 0.0f, R * 0.70f, PI * 0.83f, PI * 1.34f, R * 0.024f,
          color, opacity * 0.16f);
      arc(r, e, 0.0f, 0.0f, R * 0.70f, PI * 0.83f, PI * 0.78f, R * 0.024f,
          color, opacity * 0.76f);
      snprintf(value, sizeof(value), "%d°", temperature);
      text_center(
          r, e,
          scene_font(r, FONT_FAMILY_ENGLISH, 85, &nyabula_font_english_96),
          value, -R * 0.11f, color, opacity * 0.98f);
      text_center(r, e,
                  scene_font(r, FONT_FAMILY_TITLE, 20, &nyabula_font_title_20),
                  names[weather], R * 0.23f, color, opacity * 0.64f);
      text_center_at(
          r, e, scene_font(r, FONT_FAMILY_BODY, 14, &nyabula_font_body_14),
          left_labels[weather], -R * 0.23f, R * 0.39f, color, opacity * 0.40f);
      text_center_at(
          r, e, scene_font(r, FONT_FAMILY_BODY, 14, &nyabula_font_body_14),
          right_labels[weather], R * 0.23f, R * 0.39f, color, opacity * 0.40f);
      text_center_at(
          r, e,
          scene_font(r, FONT_FAMILY_ENGLISH, 18, &nyabula_font_english_18),
          left_value_text, -R * 0.23f, R * 0.50f, color, opacity * 0.72f);
      text_center_at(
          r, e,
          scene_font(r, FONT_FAMILY_ENGLISH, 18, &nyabula_font_english_18),
          right_value_text, R * 0.23f, R * 0.50f, color, opacity * 0.72f);
      return;
    }

  if (weather == NYABULA_EYE_WEATHER_SUNNY)
    {
      int index;
      for (index = 0; index < 12; index++)
        {
          float angle = index * PI / 6.0f + seconds * 0.06f;
          line(r, e, cosf(angle) * R * 0.48f, sinf(angle) * R * 0.48f,
               cosf(angle) * R * 0.62f, sinf(angle) * R * 0.62f, R * 0.026f,
               color, opacity * 0.42f, false);
        }
      disk(r, e, e->t.cx, e->t.cy,
           R * 0.36f * (1.0f + sinf(seconds * 1.2f) * 0.025f),
           shade(color, 0.70f), opacity, false);
      disk(r, e, e->t.cx - R * 0.08f, e->t.cy - R * 0.10f, R * 0.23f,
           shade(color, 1.28f), opacity * 0.82f, false);
      return;
    }

  if (weather == NYABULA_EYE_WEATHER_FOG)
    {
      scene_draw_fog(r, e, color, seconds, opacity * 0.92f);
      return;
    }

  if (weather == NYABULA_EYE_WEATHER_CLOUDY)
    {
      float drift = sinf(seconds * 0.35f) * R * 0.035f;
      scene_draw_cloud(r, e, shade(color, 0.65f), -R * 0.17f - drift,
                       -R * 0.18f, 0.72f, opacity * 0.34f);
      scene_draw_cloud(r, e, color, R * 0.08f + drift, R * 0.12f, 1.0f,
                       opacity);
    }
  else if (weather == NYABULA_EYE_WEATHER_RAIN ||
           weather == NYABULA_EYE_WEATHER_STORM)
    {
      scene_draw_rain(r, e, color, seconds,
                      weather == NYABULA_EYE_WEATHER_STORM, opacity);
    }
  else if (weather == NYABULA_EYE_WEATHER_SNOW)
    {
      scene_draw_snow(r, e, color, seconds, opacity);
    }
}

static void scene_battery(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e,
                          const struct nyabula_eye_scene_payload_s *payload,
                          float opacity, float reveal)
{
  static const char *icons[] = { "battery_charging", "battery_low",
                                 "battery_full", "battery_hot",
                                 "battery_dock" };
  static const char *titles[] = { "充电中", "电量不足", "已充满", "暂停充电",
                                  "正在底座充电" };
  static const char *details[] = { "约 2 小时 14 分", "约 18 分钟",
                                   "可以出发啦", "温度过高",
                                   "磁吸底座已连接" };
  uint32_t color = scene_color(e);
  unsigned int percent = payload->percent;
  char value[8];

  if (percent == 0 && payload->battery_state != NYABULA_EYE_BATTERY_LOW)
    {
      percent = payload->battery_state == NYABULA_EYE_BATTERY_FULL ? 100 : 68;
    }

  if (e->id == NYABULA_EYE_LEFT)
    {
      arc(r, e, 0.0f, 0.0f, R * 0.58f, -PI * 0.5f, PI * 2.0f, R * 0.055f,
          color, opacity * 0.20f);
      arc(r, e, 0.0f, 0.0f, R * 0.58f, -PI * 0.5f,
          percent / 100.0f * PI * 2.0f, R * 0.055f, color, opacity * 0.92f);
      icon(r, e, icons[payload->battery_state], 0.78f, color, opacity, reveal);
    }
  else
    {
      snprintf(value, sizeof(value), "%u%%", percent);
      text_center(r, e, &nyabula_font_english_96, value, -R * 0.10f, color,
                  opacity * 0.98f);
      text_center(r, e, &nyabula_font_title_20,
                  scene_text(payload->title, titles[payload->battery_state]),
                  R * 0.22f, color, opacity * 0.52f);
      text_center(r, e, &nyabula_font_body_18,
                  scene_text(payload->detail, details[payload->battery_state]),
                  R * 0.42f, color, opacity * 0.72f);
    }
}

static void scene_alarm(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity)
{
  uint32_t color = scene_color(e);
  char value[8];

  if (e->id == NYABULA_EYE_LEFT)
    {
      float shake = sinf(seconds * 14.0f) * R * 0.004f *
                    (0.5f + 0.5f * sinf(seconds * 4.8f));
      int index;
      for (index = 0; index < 3; index++)
        {
          float phase = fmodf(seconds - index * 0.30f, 1.6f);
          if (phase >= 0.0f && phase < 0.8f)
            {
              float p = phase / 0.8f;
              float eased = scene_ease(p);
              arc(r, e, shake, 0.0f, R * (0.34f + eased * 0.34f), 0.0f,
                  PI * 2.0f, R * 0.014f, color,
                  opacity * sinf(p * PI) * 0.28f);
            }
        }
      arc(r, e, shake, R * 0.04f, R * 0.32f, 0.0f, PI * 2.0f, R * 0.08f, color,
          opacity * 0.96f);
      line(r, e, shake, -R * 0.12f, shake, R * 0.04f, R * 0.08f, color,
           opacity, false);
      line(r, e, shake, R * 0.04f, shake + R * 0.08f, R * 0.12f, R * 0.08f,
           color, opacity, false);
      line(r, e, shake - R * 0.28f, -R * 0.36f, shake - R * 0.40f, -R * 0.24f,
           R * 0.08f, color, opacity, false);
      line(r, e, shake + R * 0.40f, -R * 0.24f, shake + R * 0.28f, -R * 0.36f,
           R * 0.08f, color, opacity, false);
      line(r, e, shake - R * 0.225f, R * 0.308f, shake - R * 0.32f, R * 0.40f,
           R * 0.08f, color, opacity, false);
      line(r, e, shake + R * 0.226f, R * 0.307f, shake + R * 0.32f, R * 0.40f,
           R * 0.08f, color, opacity, false);
    }
  else
    {
      snprintf(value, sizeof(value), "%02u:%02u",
               payload->hour == 0 ? 7 : payload->hour,
               payload->minute == 0 ? 30 : payload->minute);
      text_center(r, e, &nyabula_font_english_72, value, -R * 0.07f, color,
                  opacity * 0.98f);
      if (payload->alarm_copy != NYABULA_EYE_ALARM_COPY_NONE)
        {
          text_center(r, e, &nyabula_font_body_18,
                      payload->alarm_copy == NYABULA_EYE_ALARM_COPY_REMINDER
                          ? scene_text(payload->detail, "喝水 · 吃药")
                          : scene_text(payload->title, "早安，Nyabula"),
                      R * 0.26f, color, opacity * 0.62f);
        }
    }
}

static void scene_call(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *payload,
                       float seconds, float opacity, float reveal)
{
  uint32_t color = scene_color(e);

  if (e->id == NYABULA_EYE_LEFT)
    {
      if (payload->call_state == NYABULA_EYE_CALL_ENDED)
        {
          icon(r, e, "call_end", 1.10f, color, opacity * 0.72f, reveal);
        }
      else
        {
          int index;
          for (index = 0; index < 3; index++)
            {
              float p = fmodf(seconds - index * 0.30f, 1.6f) / 0.8f;
              if (p >= 0.0f && p <= 1.0f)
                {
                  arc(r, e, 0.0f, 0.0f, R * (0.34f + p * 0.34f), 0.0f,
                      PI * 2.0f, R * 0.014f, color,
                      opacity * sinf(p * PI) * 0.28f);
                }
            }
          icon_outline(r, e, "phone_call", 0.912f, R * 0.076f, color,
                       opacity * (0.82f + 0.18f * sinf(seconds * 4.4f)));
        }
    }
  else
    {
      const char *title = payload->call_state == NYABULA_EYE_CALL_ENDED
                              ? "通话结束"
                              : scene_text(payload->title, "BeaconCat");
      const char *detail =
          payload->call_state == NYABULA_EYE_CALL_ACTIVE
              ? scene_text(payload->detail, "00:42 · 通话中")
          : payload->call_state == NYABULA_EYE_CALL_ENDED
              ? scene_text(payload->detail, "通话时长 03:18")
              : scene_text(payload->value, "+86 138 0000 0620");
      const lv_font_t *title_font =
          payload->call_state == NYABULA_EYE_CALL_ENDED
              ? &nyabula_font_title_42
              : &nyabula_font_english_42;

      text_center(r, e, title_font, title, -R * 0.11f, color,
                  opacity * (payload->call_state == NYABULA_EYE_CALL_ENDED
                                 ? 0.58f
                                 : 0.98f));
      text_center(r, e, &nyabula_font_body_18, detail, R * 0.17f, color,
                  opacity * 0.48f);
    }
}

static void scene_task(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *payload,
                       float seconds, float opacity, float reveal)
{
  static const float progress[] = { 0.62f, 0.14f, 0.46f, 1.0f, 0.68f };
  static const char *values[] = { "3/5", "1/5", "2/5", "5/5", "3/5" };
  static const char *labels[] = { "执行中", "排队中", "等待确认", "已完成",
                                  "执行失败" };
  uint32_t color = scene_color(e);
  float task_progress = payload->progress > 0.0f
                            ? clampf(payload->progress, 0.0f, 1.0f)
                            : progress[payload->task_state];

  if (e->id == NYABULA_EYE_LEFT)
    {
      if (payload->task_state == NYABULA_EYE_TASK_RUNNING)
        {
          const int nodes = 6;
          float rotation = seconds * 0.72f;
          float active = fmodf(seconds * 2.4f, nodes);
          int index;

          for (index = 0; index < nodes; index++)
            {
              float angle = rotation + index * PI * 2.0f / nodes;
              float next = rotation + (index + 1) * PI * 2.0f / nodes;

              line(r, e, cosf(angle) * R * 0.48f, sinf(angle) * R * 0.48f,
                   cosf(next) * R * 0.48f, sinf(next) * R * 0.48f, R * 0.018f,
                   color, opacity * (0.16f + index * 0.025f), false);
            }

          for (index = 0; index < nodes; index++)
            {
              float angle = rotation + index * PI * 2.0f / nodes;
              float distance = fabsf(index - active);
              float alpha;
              float radius;
              float sx;
              float sy;

              distance = fminf(distance, nodes - distance);
              alpha = clampf(0.32f + (1.0f - distance) * 0.55f, 0.28f, 0.95f);
              radius = R * (distance < 0.7f ? 0.064f : 0.036f);
              to_screen(&e->t, cosf(angle) * R * 0.48f,
                        sinf(angle) * R * 0.48f, &sx, &sy);
              disk(r, e, sx, sy, radius, color, opacity * alpha, false);
            }

          disk(r, e, e->t.cx, e->t.cy,
               R * 0.082f * (0.90f + 0.10f * sinf(seconds * 4.2f)), color,
               opacity * 0.94f, false);
        }
      else if (payload->task_state == NYABULA_EYE_TASK_QUEUED)
        {
          int index;

          icon_at(r, e, "task_queued", 0.88f, 0.0f, -R * 0.07f,
                  sinf(seconds * 2.1f) * 0.055f, color, opacity, reveal);
          for (index = 0; index < 3; index++)
            {
              bool active = (int)floorf(seconds * 2.2f) % 3 == index;
              float sx;
              float sy;

              to_screen(&e->t, (index - 1) * R * 0.16f, R * 0.48f, &sx, &sy);
              disk(r, e, sx, sy, R * (active ? 0.035f : 0.025f), color,
                   opacity * (active ? 0.92f : 0.24f), false);
            }
        }
      else if (payload->task_state == NYABULA_EYE_TASK_CONFIRM)
        {
          float breathe = 0.96f + 0.04f * sinf(seconds * 2.8f);
          float offset = R * (0.50f + 0.025f * sinf(seconds * 2.8f));

          icon(r, e, "task_confirm", 1.02f * breathe, color, opacity, reveal);
          line(r, e, -offset, -R * 0.18f, -offset, R * 0.18f, R * 0.022f,
               color, opacity * 0.46f, false);
          line(r, e, offset, -R * 0.18f, offset, R * 0.18f, R * 0.022f, color,
               opacity * 0.46f, false);
        }
      else if (payload->task_state == NYABULA_EYE_TASK_DONE)
        {
          float phase = clampf(seconds / 0.78f, 0.0f, 1.0f);

          icon(r, e, "task_done", 1.12f, color, opacity, reveal);
          if (phase < 1.0f)
            {
              float eased = scene_ease(phase);
              arc(r, e, 0.0f, 0.0f, R * (0.30f + 0.35f * eased), 0.0f,
                  PI * 2.0f, R * 0.022f, color,
                  opacity * (1.0f - eased) * 0.48f);
            }
        }
      else
        {
          float phase = fmodf(seconds, 2.4f);
          float shake = phase < 0.34f ? sinf(phase * 58.0f) *
                                            (1.0f - phase / 0.34f) * R * 0.020f
                                      : 0.0f;

          icon_at(r, e, "task_failed", 1.04f, shake, 0.0f, 0.0f, color,
                  opacity * (0.78f + 0.14f * sinf(seconds * 2.6f)), reveal);
          if (phase < 0.62f)
            {
              float p = phase / 0.62f;
              arc(r, e, 0.0f, 0.0f, R * (0.39f + 0.17f * scene_ease(p)), 0.0f,
                  PI * 2.0f, R * 0.018f, color, opacity * (1.0f - p) * 0.28f);
            }
        }
    }
  else
    {
      enum nyabula_eye_task_state_e state = payload->task_state;

      arc(r, e, 0.0f, 0.0f, R * 0.55f, -PI * 0.5f, PI * 2.0f, R * 0.05f, color,
          opacity * 0.18f);
      arc(r, e, 0.0f, 0.0f, R * 0.55f, -PI * 0.5f, task_progress * PI * 2.0f,
          R * 0.05f, color, opacity * 0.88f);
      text_center(r, e, &nyabula_font_english_72,
                  scene_text(payload->value, values[state]), -R * 0.04f, color,
                  opacity * 0.98f);
      text_center(r, e, &nyabula_font_title_20,
                  scene_text(payload->title, labels[state]), R * 0.29f, color,
                  opacity * 0.52f);
    }
}

static void
scene_draw_content(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                   enum nyabula_eye_scene_e scene,
                   const struct nyabula_eye_scene_payload_s *payload,
                   float seconds, float opacity, float reveal, bool minimal)
{
  uint32_t color = scene_color(e);
  char value[32];
  int index;

  scene_backdrop(r, e, minimal, 0.68f * opacity);
  switch (scene)
    {
      case NYABULA_EYE_SCENE_MUSIC:
        scene_music(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_TIMER:
        scene_timer(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_WEATHER:
        scene_weather(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_BATTERY:
        scene_battery(r, e, payload, opacity, reveal);
        break;
      case NYABULA_EYE_SCENE_ALARM:
        scene_alarm(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_CALL:
        scene_call(r, e, payload, seconds, opacity, reveal);
        break;
      case NYABULA_EYE_SCENE_TASK:
        scene_task(r, e, payload, seconds, opacity, reveal);
        break;
      case NYABULA_EYE_SCENE_STOPWATCH:
        {
          unsigned int elapsed = payload->elapsed_ms == 0
                                     ? (unsigned int)(seconds * 1000.0f)
                                     : payload->elapsed_ms;
          unsigned int minutes = elapsed / 60000;
          unsigned int whole_seconds = elapsed / 1000 % 60;
          unsigned int tenths = elapsed / 100 % 10;

          scene_dial(r, e, color, (elapsed % 60000) / 60000.0f, opacity);
          if (e->id == NYABULA_EYE_LEFT)
            {
              snprintf(value, sizeof(value), "%02u", minutes);
              text_center(r, e,
                          scene_font(r, FONT_FAMILY_ENGLISH, 103,
                                     &nyabula_font_english_96),
                          value, -R * 0.04f, color, opacity * 0.98f);
              text_center(
                  r, e,
                  scene_font(r, FONT_FAMILY_BODY, 15, &nyabula_font_body_14),
                  "分钟", R * 0.34f, color, opacity * 0.44f);
            }
          else
            {
              snprintf(value, sizeof(value), "%02u.%u", whole_seconds, tenths);
              text_center(r, e,
                          scene_font(r, FONT_FAMILY_ENGLISH, 84,
                                     &nyabula_font_english_72),
                          value, -R * 0.04f, color, opacity * 0.98f);
              text_center(
                  r, e,
                  scene_font(r, FONT_FAMILY_BODY, 15, &nyabula_font_body_14),
                  "秒钟", R * 0.34f, color, opacity * 0.44f);
            }
        }
        break;
      case NYABULA_EYE_SCENE_CALENDAR:
        if (e->id == NYABULA_EYE_LEFT)
          {
            snprintf(value, sizeof(value), "%u",
                     payload->day == 0 ? 17 : payload->day);
            rounded_rect_outline(r, e, -R * 0.48f, -R * 0.48f, R * 0.48f,
                                 R * 0.48f, R * 0.10f, R * 0.025f, color,
                                 opacity * 0.24f);
            filled_rect(r, e, -R * 0.48f, -R * 0.29f, R * 0.48f, -R * 0.265f,
                        color, opacity * 0.42f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 89,
                                   &nyabula_font_english_96),
                        value, R * 0.05f, color, opacity * 0.98f);
            snprintf(value, sizeof(value), "%u 月 · 星期日",
                     payload->month == 0 ? 8 : payload->month);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 20, &nyabula_font_title_20),
                value, R * 0.36f, color, opacity * 0.52f);
          }
        else
          {
            snprintf(value, sizeof(value), "%02u:%02u",
                     payload->hour == 0 ? 9 : payload->hour,
                     payload->minute == 0 ? 30 : payload->minute);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 71,
                                   &nyabula_font_english_72),
                        value, -R * 0.18f, color, opacity * 0.98f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 25, &nyabula_font_title_28),
                scene_text(payload->title, "产品设计评审"), R * 0.13f, color,
                opacity * 0.67f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 15, &nyabula_font_body_14),
                scene_text(payload->detail, "还有 25 分钟"), R * 0.35f, color,
                opacity * 0.34f);
          }
        break;
      case NYABULA_EYE_SCENE_SLEEP_TIMER:
        if (e->id == NYABULA_EYE_LEFT)
          {
            static const float star_x[] = { -0.46f, 0.40f, 0.47f };
            static const float star_y[] = { -0.34f, -0.35f, 0.25f };
            static const float star_size[] = { 0.193f, 0.238f, 0.149f };

            icon_at(r, e, "moon", 1.04f, -R * 0.02f, R * 0.01f, 0.0f, color,
                    opacity, reveal);
            for (index = 0; index < 3; index++)
              {
                float pulse =
                    0.28f + 0.42f * (0.5f + 0.5f * sinf(seconds * 1.35f +
                                                        index * 1.9f));
                float rotation = sinf(seconds * 0.25f + index) * 0.10f;
                icon_group_at(r, e, "star", star_size[index],
                              R * star_x[index], R * star_y[index], rotation,
                              color, opacity * pulse, reveal);
              }
          }
        else
          {
            unsigned int remaining =
                payload->remaining_ms == 0
                    ? 30
                    : (payload->remaining_ms + 59999) / 60000;
            snprintf(value, sizeof(value), "%u", remaining);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 85,
                                   &nyabula_font_english_72),
                        value, -R * 0.12f, color, opacity * 0.98f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 19, &nyabula_font_title_20),
                "分钟", R * 0.18f, color, opacity * 0.55f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 14, &nyabula_font_body_14),
                "音乐结束后休眠", R * 0.36f, color, opacity * 0.30f);
          }
        break;
      case NYABULA_EYE_SCENE_NETWORK:
        if (e->id == NYABULA_EYE_LEFT)
          {
            const char *name =
                payload->network_state == NYABULA_EYE_NETWORK_WIFI ? "wifi"
                : payload->network_state == NYABULA_EYE_NETWORK_BLUETOOTH
                    ? "bluetooth"
                    : "wifi_off";
            icon(r, e, name, 1.18f, color,
                 opacity *
                     (payload->network_state == NYABULA_EYE_NETWORK_OFFLINE
                          ? 0.72f
                          : 1.0f),
                 reveal);
          }
        else
          {
            const char *title =
                payload->network_state == NYABULA_EYE_NETWORK_WIFI
                    ? "网络已连接"
                : payload->network_state == NYABULA_EYE_NETWORK_BLUETOOTH
                    ? "蓝牙已连接"
                    : "当前离线";
            const char *detail =
                payload->network_state == NYABULA_EYE_NETWORK_WIFI
                    ? "Nyabula · 5 GHz"
                : payload->network_state == NYABULA_EYE_NETWORK_BLUETOOTH
                    ? "BeaconPhone"
                    : "等待重新连接";
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 39, &nyabula_font_title_42),
                title, -R * 0.13f, color,
                opacity *
                    (payload->network_state == NYABULA_EYE_NETWORK_OFFLINE
                         ? 0.54f
                         : 0.98f));
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 16, &nyabula_font_body_18),
                detail, R * 0.16f, color, opacity * 0.44f);
            if (payload->network_state != NYABULA_EYE_NETWORK_OFFLINE)
              {
                float sx;
                float sy;
                float pulse = 0.55f + 0.35f * sinf(seconds * 2.2f);
                to_screen(&e->t, 0.0f, R * 0.38f, &sx, &sy);
                disk(r, e, sx, sy, R * 0.025f, color, opacity * pulse, false);
              }
          }
        break;
      case NYABULA_EYE_SCENE_AUDIO:
        if (e->id == NYABULA_EYE_LEFT)
          {
            static const char *names[] = { "speaker", "headphones",
                                           "audio_both", "audio_mute" };
            scene_dial(r, e, color,
                       payload->audio_route == NYABULA_EYE_AUDIO_MUTE ? 0.0f
                                                                      : 0.64f,
                       opacity);
            icon(r, e, names[payload->audio_route], 0.82f, color,
                 opacity * (payload->audio_route == NYABULA_EYE_AUDIO_MUTE
                                ? 0.64f
                                : 1.0f),
                 reveal);
          }
        else
          {
            static const char *titles[] = { "扬声器", "耳机", "同时输出",
                                            "已静音" };
            bool muted = payload->audio_route == NYABULA_EYE_AUDIO_MUTE;
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 39, &nyabula_font_title_42),
                titles[payload->audio_route], -R * 0.14f, color,
                opacity * (muted ? 0.48f : 0.98f));
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 64,
                                   &nyabula_font_english_72),
                        muted ? "—" : "64%", R * 0.14f, color,
                        opacity * 0.74f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 15, &nyabula_font_body_14),
                "媒体音量", R * 0.39f, color, opacity * 0.34f);
          }
        break;
      case NYABULA_EYE_SCENE_EQ:
        if (e->id == NYABULA_EYE_LEFT)
          {
            bool calibrating = payload->eq_view == NYABULA_EYE_EQ_CALIBRATING;
            if (calibrating)
              {
                float progress = fmodf(seconds * 0.13f, 1.0f);
                unsigned int frequency =
                    (unsigned int)roundf(20.0f * powf(1000.0f, progress));
                scene_dial(r, e, color, progress, opacity);
                snprintf(value, sizeof(value), "%u", frequency);
                text_center(r, e,
                            scene_font(r, FONT_FAMILY_ENGLISH, 59,
                                       &nyabula_font_english_72),
                            value, -R * 0.04f, color, opacity * 0.98f);
                text_center(
                    r, e,
                    scene_font(r, FONT_FAMILY_BODY, 15, &nyabula_font_body_14),
                    "Hz · 扫频", R * 0.28f, color, opacity * 0.42f);
              }
            else
              {
                bool has_bands = false;
                for (index = 0; index < NYABULA_EYE_EQ_BANDS; index++)
                  {
                    if (fabsf(payload->eq_bands[index]) > 0.001f)
                      {
                        has_bands = true;
                        break;
                      }
                  }

                for (index = 0; index < 7; index++)
                  {
                    float x = (index - 3) * R * 0.16f;
                    float level =
                        has_bands
                            ? clampf(fabsf(payload->eq_bands[index]) / 12.0f,
                                     0.0f, 1.0f)
                            : 0.5f +
                                  0.5f * sinf(index * 1.6f + seconds * 0.7f);
                    float height = R * (0.22f + 0.28f * level);
                    line(r, e, x, -height * 0.5f, x, height * 0.5f, R * 0.07f,
                         color, opacity * (0.30f + index * 0.07f), false);
                  }
              }
          }
        else
          {
            bool calibrating = payload->eq_view == NYABULA_EYE_EQ_CALIBRATING;
            struct eye_s shifted = *e;
            shifted.t.cy -= R * 0.08f;
            scene_eq_curve(r, &shifted, color, seconds, opacity, calibrating);
            text_center(r, e,
                        scene_font(r,
                                   calibrating ? FONT_FAMILY_TITLE
                                               : FONT_FAMILY_ENGLISH,
                                   calibrating ? 18 : 15,
                                   calibrating ? &nyabula_font_title_20
                                               : &nyabula_font_english_18),
                        calibrating ? "测量中 · 2/4" : "NYABULA FLAT",
                        R * 0.43f, color, opacity * 0.58f);
          }
        break;
      case NYABULA_EYE_SCENE_CAPTION:
        if (e->id == NYABULA_EYE_LEFT)
          {
            float last_x = -R * 0.58f;
            float last_y = 0.0f;
            for (index = 1; index <= 60; index++)
              {
                float x = R * (-0.58f + index / 60.0f * 1.16f);
                float envelope = sinf(index / 60.0f * PI);
                float y = sinf(index * 0.72f + seconds * 5.0f) * envelope * R *
                          0.25f;
                line(r, e, last_x, last_y, x, y, R * 0.025f, color,
                     opacity * 0.82f, false);
                last_x = x;
                last_y = y;
              }
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 16, &nyabula_font_title_20),
                "聆听中", R * 0.42f, color, opacity * 0.40f);
          }
        else
          {
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 17, &nyabula_font_body_18),
                scene_text(payload->previous_line, "我可以帮你"), -R * 0.27f,
                color, opacity * 0.28f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 32, &nyabula_font_title_28),
                scene_text(payload->current_line, "设置一个提醒"), 0.0f, color,
                opacity * 0.98f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 16, &nyabula_font_body_18),
                scene_text(payload->next_line, "字幕模式已开启"), R * 0.29f,
                color, opacity * 0.34f);
          }
        break;
      case NYABULA_EYE_SCENE_BRIEFING:
        if (e->id == NYABULA_EYE_LEFT)
          {
            float progress = fmodf(seconds * 0.16f, 1.0f);
            float last_x = -R * 0.32f;
            float last_y = 0.0f;

            arc(r, e, 0.0f, 0.0f, R * 0.48f, 0.0f, PI * 2.0f, R * 0.018f,
                color, opacity * 0.18f);
            for (index = 0; index < 3; index++)
              {
                float angle =
                    -PI * 0.5f + (index + progress) * PI * 2.0f / 3.0f;
                float sx;
                float sy;
                to_screen(&e->t, cosf(angle) * R * 0.48f,
                          sinf(angle) * R * 0.48f, &sx, &sy);
                disk(r, e, sx, sy, R * (index == 1 ? 0.065f : 0.035f), color,
                     opacity * (index == 1 ? 0.92f : 0.40f), false);
              }

            for (index = 1; index <= 40; index++)
              {
                float x = R * (-0.32f + index / 40.0f * 0.64f);
                float y = sinf(index * 0.66f + seconds * 4.0f) * R * 0.09f *
                          (1.0f - fabsf(index / 20.0f - 1.0f));
                line(r, e, last_x, last_y, x, y, R * 0.026f, color,
                     opacity * 0.78f, false);
                last_x = x;
                last_y = y;
              }
          }
        else
          {
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 39, &nyabula_font_title_42),
                scene_text(payload->title, "早间简报"), -R * 0.24f, color,
                opacity * 0.98f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 18, &nyabula_font_body_18),
                scene_text(payload->subtitle, "天气 · 日程 · 资讯"), R * 0.03f,
                color, opacity * 0.55f);
            snprintf(
                value, sizeof(value), "正在朗读  %u / %u",
                payload->briefing_index == 0 ? 1 : payload->briefing_index,
                payload->briefing_count == 0 ? 3 : payload->briefing_count);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 15, &nyabula_font_body_14),
                scene_text(payload->detail, value), R * 0.29f, color,
                opacity * 0.36f);
          }
        break;
      case NYABULA_EYE_SCENE_PRIVACY:
      case NYABULA_EYE_SCENE_IDENTITY:
      case NYABULA_EYE_SCENE_MEMORY:
      case NYABULA_EYE_SCENE_DEVICES:
      case NYABULA_EYE_SCENE_SYSTEM:
      case NYABULA_EYE_SCENE_HEALTH:
      case NYABULA_EYE_SCENE_PRESENCE:
      case NYABULA_EYE_SCENE_COMPANION:
      case NYABULA_EYE_SCENE_SUBWOOFER:
        if (e->id == NYABULA_EYE_LEFT)
          {
            if (scene == NYABULA_EYE_SCENE_COMPANION)
              {
                for (index = 3; index >= 0; index--)
                  {
                    float phase = fmodf(seconds, 6.8f) - index * 0.22f;
                    float breath =
                        phase >= 0.0f && phase < 1.05f ? phase / 1.05f
                        : phase < 1.30f                ? 1.0f
                        : phase < 2.15f ? 1.0f - (phase - 1.30f) / 0.85f
                                        : 0.0f;
                    float radius =
                        R * (0.22f + index * 0.11f) * (0.90f + breath * 0.24f);
                    disk(r, e, e->t.cx, e->t.cy, radius, color,
                         opacity * (0.05f + index * 0.032f + breath * 0.026f),
                         false);
                  }
                icon(r, e, "companion", 0.48f, color, opacity * 0.94f, reveal);
              }
            else
              {
                const char *name =
                    scene == NYABULA_EYE_SCENE_PRIVACY    ? "privacy"
                    : scene == NYABULA_EYE_SCENE_IDENTITY ? "identity"
                    : scene == NYABULA_EYE_SCENE_MEMORY   ? "memory"
                    : scene == NYABULA_EYE_SCENE_DEVICES  ? "devices"
                    : scene == NYABULA_EYE_SCENE_SYSTEM   ? "system"
                    : scene == NYABULA_EYE_SCENE_HEALTH   ? "heart_rate"
                    : scene == NYABULA_EYE_SCENE_PRESENCE ? "presence"
                                                          : "subwoofer";
                float pulse =
                    scene == NYABULA_EYE_SCENE_PRIVACY
                        ? 0.88f + 0.12f * sinf(seconds * 2.4f)
                    : scene == NYABULA_EYE_SCENE_MEMORY
                        ? 0.90f + 0.10f * sinf(seconds * 1.7f)
                    : scene == NYABULA_EYE_SCENE_HEALTH
                        ? 0.90f + 0.10f * sinf(seconds * 4.0f)
                    : scene == NYABULA_EYE_SCENE_SUBWOOFER
                        ? 0.84f + 0.16f * (0.5f + 0.5f * sinf(seconds * 4.4f))
                        : 1.0f;
                float size = scene == NYABULA_EYE_SCENE_PRIVACY ||
                                     scene == NYABULA_EYE_SCENE_MEMORY
                                 ? 1.16f
                                 : 1.18f;
                if (scene == NYABULA_EYE_SCENE_SUBWOOFER)
                  {
                    size += (0.5f + 0.5f * sinf(seconds * 4.4f)) * 0.025f;
                  }

                icon(r, e, name, size, color, opacity * pulse, reveal);
              }
          }
        else
          {
            const char *title;
            const char *subtitle;
            const char *detail;
            enum font_family_e title_family = FONT_FAMILY_TITLE;
            uint16_t title_size;
            uint16_t subtitle_size;
            uint16_t detail_size;
            float title_y;
            float subtitle_y;
            float detail_y;
            float title_alpha = 0.98f;
            float subtitle_alpha;
            float detail_alpha;

            switch (scene)
              {
                case NYABULA_EYE_SCENE_PRIVACY:
                  title =
                      payload->privacy_camera && payload->privacy_microphone
                          ? "摄像头 + 麦克风"
                      : payload->privacy_camera     ? "摄像头"
                      : payload->privacy_microphone ? "麦克风"
                                                    : "隐私设备";
                  subtitle = payload->active ? "正在使用" : "当前空闲";
                  detail = "本地处理 · 指示不可关闭";
                  title_size = 30;
                  subtitle_size = 25;
                  detail_size = 14;
                  title_y = -R * 0.20f;
                  subtitle_y = R * 0.06f;
                  detail_y = R * 0.31f;
                  subtitle_alpha = 0.72f;
                  detail_alpha = 0.34f;
                  break;
                case NYABULA_EYE_SCENE_IDENTITY:
                  title = "你好，主人";
                  subtitle = "主人身份已确认";
                  detail = "模板仅保存在本机";
                  title_size = 41;
                  subtitle_size = 19;
                  detail_size = 13;
                  title_y = -R * 0.15f;
                  subtitle_y = R * 0.14f;
                  detail_y = R * 0.34f;
                  subtitle_alpha = 0.58f;
                  detail_alpha = 0.30f;
                  break;
                case NYABULA_EYE_SCENE_MEMORY:
                  title = "你喜欢轻音乐";
                  subtitle = "准备记住";
                  detail = "等待语音确认";
                  title_size = 30;
                  subtitle_size = 16;
                  detail_size = 15;
                  title_y = -R * 0.04f;
                  subtitle_y = -R * 0.31f;
                  detail_y = R * 0.26f;
                  subtitle_alpha = 0.55f;
                  detail_alpha = 0.42f;
                  break;
                case NYABULA_EYE_SCENE_DEVICES:
                  snprintf(value, sizeof(value), "%u",
                           payload->device_count == 0 ? 2
                                                      : payload->device_count);
                  title = value;
                  subtitle = "台设备在线";
                  detail = "手机 · CODEX · 局域网 MCP";
                  title_family = FONT_FAMILY_ENGLISH;
                  title_size = 66;
                  subtitle_size = 20;
                  detail_size = 13;
                  title_y = -R * 0.18f;
                  subtitle_y = R * 0.08f;
                  detail_y = R * 0.31f;
                  subtitle_alpha = 0.58f;
                  detail_alpha = 0.32f;
                  break;
                case NYABULA_EYE_SCENE_SYSTEM:
                  title = "OPENVELA";
                  subtitle = "本地大脑就绪";
                  detail = "AMP · NPU 可用";
                  title_family = FONT_FAMILY_ENGLISH;
                  title_size = 39;
                  subtitle_size = 19;
                  detail_size = 14;
                  title_y = -R * 0.24f;
                  subtitle_y = R * 0.01f;
                  detail_y = R * 0.27f;
                  subtitle_alpha = 0.58f;
                  detail_alpha = 0.31f;
                  break;
                case NYABULA_EYE_SCENE_HEALTH:
                  snprintf(value, sizeof(value), "%d",
                           (int)lroundf(payload->heart_rate_bpm <= 0.0f
                                            ? 72.0f
                                            : payload->heart_rate_bpm));
                  title = value;
                  subtitle = payload->signal_good ? "次/分 · 信号良好"
                                                  : "次/分 · 信号较弱";
                  detail = "趋势参考 · 非医疗用途";
                  title_family = FONT_FAMILY_ENGLISH;
                  title_size = 85;
                  subtitle_size = 18;
                  detail_size = 12;
                  title_y = -R * 0.13f;
                  subtitle_y = R * 0.16f;
                  detail_y = R * 0.37f;
                  subtitle_alpha = 0.56f;
                  detail_alpha = 0.28f;
                  break;
                case NYABULA_EYE_SCENE_PRESENCE:
                  title = "用户在场";
                  {
                    float distance = payload->distance_m <= 0.0f
                                         ? 0.8f
                                         : payload->distance_m;
                    int tenths = (int)lroundf(distance * 10.0f);
                    snprintf(value, sizeof(value), "%d.%d m", tenths / 10,
                             abs(tenths % 10));
                  }
                  subtitle = value;
                  detail = "正在看向猫猫";
                  title_size = 39;
                  subtitle_size = 55;
                  detail_size = 13;
                  title_y = -R * 0.20f;
                  subtitle_y = R * 0.09f;
                  detail_y = R * 0.34f;
                  subtitle_alpha = 0.68f;
                  detail_alpha = 0.30f;
                  break;
                case NYABULA_EYE_SCENE_COMPANION:
                  title = "专注陪伴";
                  subtitle = "安静模式";
                  detail = "仅保留重要提醒";
                  title_size = 39;
                  subtitle_size = 16;
                  detail_size = 13;
                  title_y = -R * 0.17f;
                  subtitle_y = R * 0.10f;
                  detail_y = R * 0.32f;
                  subtitle_alpha = 0.50f;
                  detail_alpha = 0.27f;
                  break;
                default:
                  title = "2.1 已启用";
                  snprintf(value, sizeof(value), "%d Hz",
                           (int)lroundf(payload->crossover_hz <= 0.0f
                                            ? 80.0f
                                            : payload->crossover_hz));
                  subtitle = value;
                  detail = "低音在线 · LR 分频";
                  title_size = 48;
                  subtitle_size = 59;
                  detail_size = 13;
                  title_y = -R * 0.22f;
                  subtitle_y = R * 0.08f;
                  detail_y = R * 0.34f;
                  subtitle_alpha = 0.70f;
                  detail_alpha = 0.31f;
                  break;
              }

            text_center(r, e,
                        scene_font(r, title_family, title_size,
                                   title_family == FONT_FAMILY_ENGLISH
                                       ? &nyabula_font_english_42
                                       : &nyabula_font_title_42),
                        scene_text(payload->title, title), title_y, color,
                        opacity * title_alpha);
            text_center(r, e,
                        scene_font(r,
                                   scene == NYABULA_EYE_SCENE_PRESENCE ||
                                           scene == NYABULA_EYE_SCENE_SUBWOOFER
                                       ? FONT_FAMILY_BODY
                                       : FONT_FAMILY_TITLE,
                                   subtitle_size, &nyabula_font_title_20),
                        scene_text(payload->subtitle, subtitle), subtitle_y,
                        color, opacity * subtitle_alpha);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, detail_size,
                                   &nyabula_font_body_14),
                        scene_text(payload->detail, detail), detail_y, color,
                        opacity * detail_alpha);
          }
        break;
      case NYABULA_EYE_SCENE_HOME:
        if (e->id == NYABULA_EYE_LEFT)
          {
            line(r, e, -R * 0.48f, R * 0.12f, 0.0f, -R * 0.37f, R * 0.026f,
                 color, opacity * 0.78f, false);
            line(r, e, 0.0f, -R * 0.37f, R * 0.48f, R * 0.12f, R * 0.026f,
                 color, opacity * 0.78f, false);
            line(r, e, R * 0.48f, R * 0.12f, R * 0.37f, R * 0.12f, R * 0.026f,
                 color, opacity * 0.78f, false);
            line(r, e, R * 0.37f, R * 0.12f, R * 0.37f, R * 0.42f, R * 0.026f,
                 color, opacity * 0.78f, false);
            line(r, e, R * 0.37f, R * 0.42f, -R * 0.37f, R * 0.42f, R * 0.026f,
                 color, opacity * 0.78f, false);
            line(r, e, -R * 0.37f, R * 0.42f, -R * 0.37f, R * 0.12f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, -R * 0.37f, R * 0.12f, -R * 0.48f, R * 0.12f,
                 R * 0.026f, color, opacity * 0.78f, false);
            arc(r, e, 0.0f, R * 0.20f, R * 0.14f, PI, PI, R * 0.026f, color,
                opacity * 0.78f);
            line(r, e, R * 0.14f, R * 0.20f, R * 0.14f, R * 0.42f, R * 0.026f,
                 color, opacity * 0.78f, false);
            line(r, e, -R * 0.14f, R * 0.42f, -R * 0.14f, R * 0.20f,
                 R * 0.026f, color, opacity * 0.78f, false);
            {
              float sx;
              float sy;
              to_screen(&e->t, R * 0.34f, -R * 0.30f, &sx, &sy);
              disk(r, e, sx, sy, R * 0.055f, color,
                   opacity * (0.38f + 0.22f * sinf(seconds * 0.8f)), false);
            }
          }
        else
          {
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_TITLE, 39, &nyabula_font_title_42),
                "虚拟猫舍", -R * 0.23f, color, opacity * 0.98f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 20, &nyabula_font_body_18),
                "晴 · 午后", R * 0.03f, color, opacity * 0.58f);
            text_center(
                r, e,
                scene_font(r, FONT_FAMILY_BODY, 14, &nyabula_font_body_14),
                "双屏立体视窗预览", R * 0.29f, color, opacity * 0.30f);
          }
        break;
      default:
        break;
    }
}

static void overlays(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  float alpha = e->f->overlay;
  float time = e->f->global_seconds;
  uint32_t bright = shade(e->f->iris_rgb, 1.9f);
  if (alpha <= 0.02f)
    {
      return;
    }

  if (e->f->expression == NYABULA_EYE_EXPRESSION_PROCESSING)
    {
      float rotation = time * 2.6f;
      int i;
      for (i = 0; i < 14; i++)
        {
          float fade = 1.0f - (float)i / 14.0f;
          arc(r, e, e->gx, e->gy, e->ir * 0.46f, rotation - i * 0.16f - 0.17f,
              0.19f, e->ir * 0.055f * (1.0f - i / 14.0f * 0.6f), bright,
              powf(fade, 1.6f) * 0.95f * alpha);
        }

      {
        float sx;
        float sy;
        float breath = 0.7f + 0.3f * sinf(time * 4.0f);
        to_screen(&e->t, e->gx + cosf(rotation) * e->ir * 0.46f,
                  e->gy + sinf(rotation) * e->ir * 0.46f, &sx, &sy);
        disk(r, e, sx, sy, e->ir * 0.05f, 0xffffff, alpha, false);
        to_screen(&e->t, e->gx, e->gy, &sx, &sy);
        disk(r, e, sx, sy, e->ir * 0.055f * (1.0f + breath * 0.5f), bright,
             alpha * (0.55f + breath * 0.4f), false);
      }

      arc(r, e, e->gx, e->gy, e->ir * 0.62f, -rotation * 0.5f, 2.4f,
          e->ir * 0.02f, bright, alpha * 0.35f);
      {
        float duration = 1.0f / 0.9f;
        float rt = fmodf(e->f->mode_seconds, duration + 0.2f);
        if (rt < duration)
          {
            float p = rt / duration;
            arc(r, e, e->gx, e->gy, e->ir * (0.08f + p * 0.3f), 0.0f,
                PI * 2.0f, e->ir * 0.02f, bright, alpha * (1.0f - p) * 0.5f);
          }
      }
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_STAR)
    {
      float pulse = 1.0f + sinf(time * 5.0f) * 0.12f;
      star(r, e, e->gx, e->gy, e->ir * 0.5f * pulse,
           sinf(time * 1.5f) * 0.25f - PI * 0.5f, 0xffd94d, alpha);
      {
        int i;
        for (i = 0; i < 2; i++)
          {
            float a = time * 2.0f + i * 3.0f +
                      (e->id == NYABULA_EYE_LEFT ? -1.0f : 1.0f);
            star(r, e, e->gx + cosf(a) * e->ir * 0.75f,
                 e->gy + sinf(a) * e->ir * 0.60f, e->ir * 0.09f, a, 0xfff3b8,
                 alpha * (0.4f + 0.3f * sinf(time * 4.0f + i * 2.0f)));
          }
      }
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_HEART)
    {
      float beat = fmodf(time * 1.6f, 1.0f);
      float pulse =
          1.0f + (beat < 0.15f ? sinf(beat / 0.15f * PI) * 0.18f : 0.0f);
      heart(r, e, e->gx, e->gy, e->ir * 0.5f * pulse, 0xff4d79, alpha);
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_DIZZY)
    {
      float rotation = time * 4.0f * (e->id == NYABULA_EYE_LEFT ? 1 : -1);
      float a;
      lv_fpoint_t point = { e->gx, e->gy };

      lv_vector_path_clear(r->path);
      lv_vector_path_move_to(r->path, &point);
      for (a = 0.15f; a < PI * 5.0f; a += 0.15f)
        {
          float radius = e->ir * 0.55f * a / (PI * 5.0f);
          point.x = e->gx + cosf(a + rotation) * radius;
          point.y = e->gy + sinf(a + rotation) * radius;
          lv_vector_path_line_to(r->path, &point);
        }

      vector_eye_transform(r, e);
      vector_stroke(r, 0x0a0c10, alpha, e->ir * 0.09f);
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_SAD)
    {
      float shimmer = alpha * (0.5f + 0.2f * sinf(time * 2.0f));
      float start_y = e->gy + e->ir * 0.3f;
      float end_y = e->gy + e->ir * 0.98f;
      float angle = asinf(0.3f / 0.98f);
      lv_gradient_stop_t stops[2];
      int index;

      lv_vector_path_clear(r->path);
      for (index = 0; index <= 48; index++)
        {
          float a = angle + (PI - 2.0f * angle) * index / 48.0f;
          lv_fpoint_t point = { e->gx + cosf(a) * e->ir * 0.98f,
                                e->gy + sinf(a) * e->ir * 0.98f };
          if (index == 0)
            {
              lv_vector_path_move_to(r->path, &point);
            }
          else
            {
              lv_vector_path_line_to(r->path, &point);
            }
        }

      lv_vector_path_close(r->path);
      memset(stops, 0, sizeof(stops));
      stops[0].color = lv_color_hex(0xbee1ff);
      stops[0].opa = LV_OPA_TRANSP;
      stops[0].frac = 0;
      stops[1].color = lv_color_hex(0xbee1ff);
      stops[1].opa = vector_opa(shimmer * 0.55f);
      stops[1].frac = 255;
      vector_eye_transform(r, e);
      lv_vector_dsc_set_stroke_opa(r->vector, LV_OPA_TRANSP);
      lv_vector_dsc_set_fill_opa(r->vector, LV_OPA_COVER);
      lv_vector_dsc_set_fill_linear_gradient(r->vector, 0.0f, start_y, 0.0f,
                                             end_y);
      lv_vector_dsc_set_fill_gradient_color_stops(r->vector, stops, 2);
      lv_vector_dsc_set_fill_gradient_spread(r->vector,
                                             LV_VECTOR_GRADIENT_SPREAD_PAD);
      lv_vector_dsc_add_path(r->vector, r->path);
    }
}

static void highlights(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  ellipse(r, e, -e->ir * 0.33f + e->gx * 0.55f, -e->ir * 0.38f + e->gy * 0.55f,
          e->ir * 0.13f, e->ir * 0.10f, -0.5f, 0xffffff, 0.9f);
  ellipse(r, e, e->ir * 0.30f + e->gx * 0.6f, e->ir * 0.24f + e->gy * 0.6f,
          e->ir * 0.05f, e->ir * 0.05f, 0.0f, 0xffffff, 0.45f);
}

static bool lids_are_open(const struct eye_s *e)
{
  return fabsf(e->lt) < 0.0001f && fabsf(e->lb) < 0.0001f &&
         fabsf(e->slant) < 0.0001f && fabsf(e->curve) < 0.0001f;
}

static void lids(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  float extent = e->ir * 1.04f;
  float top = -extent + e->lt * extent * 2.0f;
  float left = top - e->slant * extent * 0.35f;
  float right = top + e->slant * extent * 0.35f;
  float top_bulge = extent * 0.16f * clampf(e->lt * 2.0f, 0.0f, 1.0f);
  float bottom = extent - e->lb * extent * 2.0f;
  float bottom_bulge = -e->curve * extent * 0.55f -
                       extent * 0.16f * clampf(e->lb * 2.0f, 0.0f, 1.0f);
  lv_fpoint_t point;
  lv_fpoint_t control1;
  lv_fpoint_t control2;

  lv_vector_path_clear(r->path);
  point = (lv_fpoint_t){ extent * 1.3f, right };
  lv_vector_path_move_to(r->path, &point);
  control1 = (lv_fpoint_t){ extent * 0.4f, right + top_bulge };
  control2 = (lv_fpoint_t){ -extent * 0.4f, left + top_bulge };
  point = (lv_fpoint_t){ -extent * 1.3f, left };
  lv_vector_path_cubic_to(r->path, &control1, &control2, &point);
  point = (lv_fpoint_t){ -R * 2.0f, -R * 2.0f };
  lv_vector_path_line_to(r->path, &point);
  point = (lv_fpoint_t){ R * 2.0f, -R * 2.0f };
  lv_vector_path_line_to(r->path, &point);
  lv_vector_path_close(r->path);

  point = (lv_fpoint_t){ -extent * 1.3f, bottom };
  lv_vector_path_move_to(r->path, &point);
  control1 = (lv_fpoint_t){ -extent * 0.45f, bottom + bottom_bulge };
  control2 = (lv_fpoint_t){ extent * 0.45f, bottom + bottom_bulge };
  point = (lv_fpoint_t){ extent * 1.3f, bottom };
  lv_vector_path_cubic_to(r->path, &control1, &control2, &point);
  point = (lv_fpoint_t){ R * 2.0f, R * 2.0f };
  lv_vector_path_line_to(r->path, &point);
  point = (lv_fpoint_t){ -R * 2.0f, R * 2.0f };
  lv_vector_path_line_to(r->path, &point);
  lv_vector_path_close(r->path);
  vector_eye_transform(r, e);
  vector_fill(r, 0x000000, 1.0f);
}

static float randomf(struct nyabula_eye_renderer_s *r)
{
  r->random = r->random * 1664525u + 1013904223u;
  return (float)((r->random >> 8) & 0xffffffu) / 16777215.0f;
}

static void update_z(struct nyabula_eye_renderer_s *r,
                     const struct nyabula_eye_frame_s *f)
{
  float dt = r->last_time == 0.0f ? 0.0f : f->global_seconds - r->last_time;
  int i;
  r->last_time = f->global_seconds;
  dt = clampf(dt, 0.0f, 0.05f);
  if (f->expression == NYABULA_EYE_EXPRESSION_SLEEP &&
      f->lid_top + f->lid_bottom > 0.92f)
    {
      r->znext -= dt;
      if (r->znext <= 0.0f)
        {
          r->znext = 0.8f + randomf(r) * 1.2f;
          for (i = 0; i < 2; i++)
            {
              int n;
              for (n = 0; n < ZCOUNT; n++)
                {
                  if (!r->z[n].active)
                    {
                      struct z_s *z = &r->z[n];
                      z->eye = i;
                      z->ox = (randomf(r) * 2.0f - 1.0f) * R * 0.55f;
                      z->y = R * 1.1f;
                      z->vy = -(R * 0.30f + randomf(r) * R * 0.15f);
                      z->sway = R * (0.08f + randomf(r) * 0.10f);
                      z->phase = randomf(r) * 7.0f;
                      z->size = R * (0.15f + randomf(r) * 0.15f);
                      z->active = true;
                      break;
                    }
                }
            }
        }
    }

  for (i = 0; i < ZCOUNT; i++)
    {
      if (r->z[i].active)
        {
          r->z[i].life += dt;
          r->z[i].y += r->z[i].vy * dt;
          if (r->z[i].y < -R * 1.2f ||
              f->expression != NYABULA_EYE_EXPRESSION_SLEEP)
            {
              r->z[i].active = false;
            }
        }
    }
}

static void prepare_lid_clip(struct eye_s *e)
{
  float extent = e->ir * 1.04f;
  float top = -extent + e->lt * extent * 2.0f;
  float left = top - e->slant * extent * 0.35f;
  float right = top + e->slant * extent * 0.35f;
  float top_bulge = extent * 0.16f * clampf(e->lt * 2.0f, 0.0f, 1.0f);
  float bottom = extent - e->lb * extent * 2.0f;
  float bottom_bulge = -e->curve * extent * 0.55f -
                       extent * 0.16f * clampf(e->lb * 2.0f, 0.0f, 1.0f);
  int index;

  for (index = 0; index < LID_SAMPLES; index++)
    {
      float x = index - LID_OFFSET;
      e->top_y[index] = lid_y(x, extent, left, right, top_bulge, true);
      e->bottom_y[index] =
          lid_y(x, extent, bottom, bottom, bottom_bulge, false);
    }
}

static void draw_z(struct nyabula_eye_renderer_s *r, struct eye_s *e)
{
  bool clip_ready = false;
  int i;

  for (i = 0; i < ZCOUNT; i++)
    {
      struct z_s *z = &r->z[i];
      if (z->active && z->eye == e->id)
        {
          if (!clip_ready)
            {
              prepare_lid_clip(e);
              clip_ready = true;
            }

          float p = clampf((R * 1.1f - z->y) / (R * 2.2f), 0.0f, 1.0f);
          float a = sinf(p * PI) * 0.85f;
          float x = z->ox + sinf(z->life * 2.0f + z->phase) * z->sway;
          float s = z->size * (1.0f + p * 0.5f);
          line(r, e, x - s * 0.3f, z->y - s * 0.4f, x + s * 0.3f,
               z->y - s * 0.4f, s * 0.11f, 0xa5c3ff, a, true);
          line(r, e, x + s * 0.3f, z->y - s * 0.4f, x - s * 0.3f,
               z->y + s * 0.4f, s * 0.11f, 0xa5c3ff, a, true);
          line(r, e, x - s * 0.3f, z->y + s * 0.4f, x + s * 0.3f,
               z->y + s * 0.4f, s * 0.11f, 0xa5c3ff, a, true);
        }
    }
}

static void prepare(struct eye_s *e, const struct nyabula_eye_frame_s *f,
                    int id)
{
  float side = id == NYABULA_EYE_LEFT ? -1.0f : 1.0f;
  float rotation = f->expression == NYABULA_EYE_EXPRESSION_DIZZY
                       ? sinf(f->global_seconds * 3.0f + side) * 0.06f
                       : 0.0f;
  memset(e, 0, sizeof(*e));
  e->f = f;
  e->id = id;
  e->t.cx = W * 0.5f;
  e->t.cy = CY;
  e->t.sx = 1.0f;
  e->t.sy = 1.0f - f->squint * 0.25f;
  e->t.sn = rotation == 0.0f ? 0.0f : sinf(rotation);
  e->t.cs = rotation == 0.0f ? 1.0f : cosf(rotation);
  e->ir = R * f->iris_scale;
  e->gx = f->gaze_x * R * 0.30f + side * f->derp * R * 0.24f;
  e->gy =
      f->gaze_y * R * 0.26f + side * f->derp * R * 0.11f + f->derp * R * 0.06f;
  e->lt = f->lid_top;
  e->lb = f->lid_bottom;
  e->slant = f->lid_slant * side * -1.0f;
  e->curve = f->bottom_curve;
}

static void prepare_scene(struct eye_s *e,
                          const struct nyabula_eye_frame_s *frame, int id)
{
  memset(e, 0, sizeof(*e));
  e->f = frame;
  e->id = id;
  e->t.cx = W * 0.5f;
  e->t.cy = CY;
  e->t.sx = 1.0f;
  e->t.sy = 1.0f;
  e->t.cs = 1.0f;
  e->ir = R * 1.08f;
}

static void prepare_scene_motion(struct eye_s *motion,
                                 const struct eye_s *scene_eye, float opacity,
                                 bool incoming)
{
  float visible = clampf(opacity, 0.0f, 1.0f);
  float scale = 0.78f + visible * 0.22f;

  (void)incoming;
  *motion = *scene_eye;
  motion->t.sx *= scale;
  motion->t.sy *= scale;
}

static void prepare_scene_lids(struct eye_s *e,
                               const struct nyabula_eye_frame_s *frame, int id,
                               float lid)
{
  prepare(e, frame, id);
  e->lt = e->lt + (0.86f - e->lt) * lid;
  e->lb = e->lb + (0.14f - e->lb) * lid;
  e->slant *= 1.0f - lid;
  e->curve *= 1.0f - lid;
}

static float scene_lid_boundary(const float samples[LID_SAMPLES], float x)
{
  float sample = x + LID_OFFSET;
  int index;

  if (sample <= 0.0f)
    {
      return samples[0];
    }

  if (sample >= LID_SAMPLES - 1)
    {
      return samples[LID_SAMPLES - 1];
    }

  index = (int)sample;
  return samples[index] +
         (samples[index + 1] - samples[index]) * (sample - index);
}

static void apply_scene_lid_mask(lv_draw_buf_t *buffer, struct eye_s *eye)
{
  float half_pixel = 0.5f / fmaxf(fabsf(eye->t.sy), 0.001f);
  uint32_t stride = buffer->header.stride;
  int x;
  int y;

  prepare_lid_clip(eye);
  lv_draw_buf_invalidate_cache(buffer, NULL);
  for (y = 0; y < H; y++)
    {
      lv_color32_t *pixels = (lv_color32_t *)(buffer->data + y * stride);

      for (x = 0; x < W; x++)
        {
          float local_x;
          float local_y;
          float top;
          float bottom;
          float coverage;
          uint16_t mask;

          if (pixels[x].alpha == LV_OPA_TRANSP)
            {
              continue;
            }

          to_local(&eye->t, x + 0.5f, y + 0.5f, &local_x, &local_y);
          top = scene_lid_boundary(eye->top_y, local_x);
          bottom = scene_lid_boundary(eye->bottom_y, local_x);
          coverage = clampf((top - local_y + half_pixel) / (half_pixel * 2.0f),
                            0.0f, 1.0f);
          coverage +=
              clampf((local_y - bottom + half_pixel) / (half_pixel * 2.0f),
                     0.0f, 1.0f);
          mask =
              (uint16_t)lroundf(clampf(coverage, 0.0f, 1.0f) * LV_OPA_COVER);
          pixels[x].alpha =
              (uint8_t)((pixels[x].alpha * mask + 127u) / LV_OPA_COVER);
        }
    }

  lv_draw_buf_flush_cache(buffer, NULL);
}

static void render_scene_lid_mask(struct nyabula_eye_renderer_s *r,
                                  const struct nyabula_eye_frame_s *frame,
                                  const struct eye_s *scene_eye, int id)
{
  const struct nyabula_eye_scene_frame_s *scene = &frame->scene;
  lv_vector_dsc_t *main_vector = r->vector;
  lv_vector_path_t *main_path = r->path;
  struct eye_s mask_eye;

  lv_canvas_fill_bg(r->mask_canvas, lv_color_black(), LV_OPA_TRANSP);
  lv_canvas_init_layer(r->mask_canvas, &r->mask_layer);
  if (r->mask_vector == NULL)
    {
      r->mask_vector = lv_vector_dsc_create(&r->mask_layer);
    }

  if (r->mask_path == NULL)
    {
      r->mask_path = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_HIGH);
    }

  r->vector = r->mask_vector;
  r->path = r->mask_path;
  prepare_scene_lids(&mask_eye, frame, id, scene->lid);
  if (r->vector != NULL && r->path != NULL)
    {
      lv_vector_dsc_set_blend_mode(r->vector, LV_VECTOR_BLEND_SRC_OVER);
      scene_draw_content(r, scene_eye, scene->scene, &scene->payload,
                         scene->scene_seconds, scene->alpha, scene->reveal,
                         true);
      lv_draw_vector(r->vector);
    }

  lv_canvas_finish_layer(r->mask_canvas, &r->mask_layer);
  apply_scene_lid_mask(r->mask_buffer, &mask_eye);
  r->vector = main_vector;
  r->path = main_path;
  r->mask_ready = true;
}

static bool render_scene(struct nyabula_eye_renderer_s *r,
                         const struct nyabula_eye_frame_s *frame, int id)
{
  const struct nyabula_eye_scene_frame_s *scene = &frame->scene;
  struct eye_s scene_eye;
  struct eye_s motion_eye;

  if (scene->scene == NYABULA_EYE_SCENE_NONE)
    {
      return false;
    }

  prepare_scene(&scene_eye, frame, id);
  if (scene->style == NYABULA_EYE_SCENE_STYLE_MINIMAL && scene->lid < 0.999f &&
      scene->alpha < 0.999f)
    {
      render_scene_lid_mask(r, frame, &scene_eye, id);
      return false;
    }

  if (scene->style == NYABULA_EYE_SCENE_STYLE_MINIMAL)
    {
      disk(r, &scene_eye, scene_eye.t.cx, scene_eye.t.cy, R, 0x000000, 1.0f,
           false);
    }

  if (scene->previous_scene == NYABULA_EYE_SCENE_MUSIC &&
      scene->scene == NYABULA_EYE_SCENE_MUSIC &&
      scene->previous_payload.music_view == scene->payload.music_view &&
      scene->previous_alpha > 0.001f)
    {
      scene_backdrop(r, &scene_eye,
                     scene->style == NYABULA_EYE_SCENE_STYLE_MINIMAL, 0.68f);
      scene_music_transition(r, &scene_eye, &scene->previous_payload,
                             &scene->payload, scene->scene_seconds,
                             scene->alpha, 1.0f);
    }
  else
    {
      if (scene->previous_scene != NYABULA_EYE_SCENE_NONE &&
          scene->previous_alpha > 0.001f)
        {
          prepare_scene_motion(&motion_eye, &scene_eye, scene->previous_alpha,
                               false);
          scene_draw_content(
              r, &motion_eye, scene->previous_scene, &scene->previous_payload,
              scene->previous_scene_seconds, scene->previous_alpha, 1.0f,
              scene->previous_style == NYABULA_EYE_SCENE_STYLE_MINIMAL);
        }

      prepare_scene_motion(&motion_eye, &scene_eye, scene->alpha, true);
      scene_draw_content(r, &motion_eye, scene->scene, &scene->payload,
                         scene->scene_seconds, scene->alpha, scene->reveal,
                         scene->style == NYABULA_EYE_SCENE_STYLE_MINIMAL);
    }
  if (scene->style == NYABULA_EYE_SCENE_STYLE_FULL && scene->lid > 0.001f)
    {
      struct eye_s mask_eye;
      prepare_scene_lids(&mask_eye, frame, id, scene->lid);
      lids(r, &mask_eye);
    }

  return true;
}

static bool frames_can_share_pixels(
    const struct nyabula_eye_frame_s frames[NYABULA_EYE_COUNT])
{
  const struct nyabula_eye_frame_s *frame = &frames[NYABULA_EYE_LEFT];

  if (memcmp(&frames[NYABULA_EYE_LEFT], &frames[NYABULA_EYE_RIGHT],
             sizeof(frames[0])) != 0 ||
      frame->scene.scene != NYABULA_EYE_SCENE_NONE ||
      frame->scene.previous_scene != NYABULA_EYE_SCENE_NONE ||
      fabsf(frame->lid_slant) > 0.0001f || fabsf(frame->derp) > 0.0001f)
    {
      return false;
    }

  return frame->expression != NYABULA_EYE_EXPRESSION_STAR &&
         frame->expression != NYABULA_EYE_EXPRESSION_DIZZY &&
         frame->expression != NYABULA_EYE_EXPRESSION_SLEEP;
}

static bool
scene_eye_is_time_independent(const struct nyabula_eye_scene_frame_s *scene,
                              int id)
{
  const struct nyabula_eye_scene_payload_s *payload = &scene->payload;

  switch (scene->scene)
    {
      case NYABULA_EYE_SCENE_BATTERY:
      case NYABULA_EYE_SCENE_CALENDAR:
      case NYABULA_EYE_SCENE_AUDIO:
      case NYABULA_EYE_SCENE_IDENTITY:
      case NYABULA_EYE_SCENE_DEVICES:
      case NYABULA_EYE_SCENE_SYSTEM:
      case NYABULA_EYE_SCENE_PRESENCE:
        return true;
      case NYABULA_EYE_SCENE_MUSIC:
        return id == NYABULA_EYE_LEFT
                   ? payload->position_ms != 0
                   : payload->music_view == NYABULA_EYE_MUSIC_LYRICS;
      case NYABULA_EYE_SCENE_TIMER:
        return payload->remaining_ms > 10000;
      case NYABULA_EYE_SCENE_STOPWATCH:
        return payload->elapsed_ms != 0;
      case NYABULA_EYE_SCENE_WEATHER:
      case NYABULA_EYE_SCENE_ALARM:
      case NYABULA_EYE_SCENE_CALL:
      case NYABULA_EYE_SCENE_TASK:
      case NYABULA_EYE_SCENE_SLEEP_TIMER:
      case NYABULA_EYE_SCENE_CAPTION:
      case NYABULA_EYE_SCENE_BRIEFING:
      case NYABULA_EYE_SCENE_PRIVACY:
      case NYABULA_EYE_SCENE_MEMORY:
      case NYABULA_EYE_SCENE_HEALTH:
      case NYABULA_EYE_SCENE_COMPANION:
      case NYABULA_EYE_SCENE_HOME:
      case NYABULA_EYE_SCENE_SUBWOOFER:
        return id == NYABULA_EYE_RIGHT;
      case NYABULA_EYE_SCENE_NETWORK:
        return id == NYABULA_EYE_LEFT ||
               payload->network_state == NYABULA_EYE_NETWORK_OFFLINE;
      default:
        return false;
    }
}

static bool frame_is_time_independent(const struct nyabula_eye_frame_s *frame,
                                      int id)
{
  if (frame->scene.previous_scene != NYABULA_EYE_SCENE_NONE)
    {
      return false;
    }

  if (frame->scene.scene != NYABULA_EYE_SCENE_NONE)
    {
      return scene_eye_is_time_independent(&frame->scene, id);
    }

  switch (frame->expression)
    {
      case NYABULA_EYE_EXPRESSION_PROCESSING:
      case NYABULA_EYE_EXPRESSION_STAR:
      case NYABULA_EYE_EXPRESSION_HEART:
      case NYABULA_EYE_EXPRESSION_DIZZY:
      case NYABULA_EYE_EXPRESSION_SAD:
      case NYABULA_EYE_EXPRESSION_SLEEP:
        return false;
      default:
        return true;
    }
}

static bool frame_pixels_unchanged(struct nyabula_eye_renderer_s *r,
                                   const struct nyabula_eye_frame_s *frame,
                                   int id)
{
  const struct nyabula_eye_frame_s *last = &r->last_frame[id];

  if (!r->last_frame_valid[id] || !frame_is_time_independent(frame, id) ||
      frame->iris_rgb != last->iris_rgb)
    {
      return false;
    }

  if (frame->scene.scene != NYABULA_EYE_SCENE_NONE)
    {
      return frame->scene.scene == last->scene.scene &&
             frame->scene.style == last->scene.style &&
             frame->scene.alpha == last->scene.alpha &&
             frame->scene.lid == last->scene.lid &&
             frame->scene.reveal == last->scene.reveal &&
             memcmp(&frame->scene.payload, &last->scene.payload,
                    sizeof(frame->scene.payload)) == 0;
    }

  return frame->pupil_width == last->pupil_width &&
         frame->pupil_height == last->pupil_height &&
         frame->lid_top == last->lid_top &&
         frame->lid_bottom == last->lid_bottom &&
         frame->lid_slant == last->lid_slant &&
         frame->bottom_curve == last->bottom_curve &&
         frame->gaze_x == last->gaze_x && frame->gaze_y == last->gaze_y &&
         frame->iris_scale == last->iris_scale && frame->glow == last->glow &&
         frame->overlay == last->overlay && frame->squint == last->squint &&
         frame->derp == last->derp && frame->iris_rgb == last->iris_rgb &&
         frame->expression == last->expression;
}

static void remember_frame(struct nyabula_eye_renderer_s *r,
                           const struct nyabula_eye_frame_s *frame, int id)
{
  r->last_frame[id] = *frame;
  r->last_frame_valid[id] = frame_is_time_independent(frame, id);
}

static void report(struct nyabula_eye_renderer_s *r, uint32_t render_ms)
{
  r->render_total += render_ms;
  if (++r->frames == 300)
    {
      uint32_t elapsed = lv_tick_elaps(r->perf_start);
      LV_LOG_USER("nyabula_eye: %u.%u fps, %u.%02u ms dual vector; "
                  "%u.%02u build, %u.%02u raster, %u.%02u copy, "
                  "%u.%02u flush, %u%% shared, %u%% reused, "
                  "%u/%u base hit/build",
                  300000u / elapsed, (3000000u / elapsed) % 10u,
                  r->render_total / r->frames,
                  (r->render_total * 100u / r->frames) % 100u,
                  r->build_total / r->frames,
                  (r->build_total * 100u / r->frames) % 100u,
                  r->raster_total / r->frames,
                  (r->raster_total * 100u / r->frames) % 100u,
                  r->copy_total / r->frames,
                  (r->copy_total * 100u / r->frames) % 100u,
                  r->flush_total / r->frames,
                  (r->flush_total * 100u / r->frames) % 100u,
                  r->shared_frames * 100u / r->frames,
                  r->reused_eyes * 50u / r->frames, r->base_cache_hits,
                  r->base_cache_builds);
      (void)elapsed;
      r->frames = 0;
      r->render_total = 0;
      r->build_total = 0;
      r->raster_total = 0;
      r->copy_total = 0;
      r->flush_total = 0;
      r->shared_frames = 0;
      r->reused_eyes = 0;
      r->base_cache_hits = 0;
      r->base_cache_builds = 0;
      r->perf_start = lv_tick_get();
    }
}

#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
static lv_font_t *renderer_create_font(const char *filename, uint32_t size)
{
  char path[256];

  snprintf(path, sizeof(path), "%s/%s",
           CONFIG_CONTEST2026_062_NYABULA_FONT_ROOT, filename);
  return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                 size, LV_FREETYPE_FONT_STYLE_NORMAL);
}

static void renderer_load_fonts(struct nyabula_eye_renderer_s *r)
{
  r->font_title_20 = renderer_create_font("AlimamaShuHeiTi.ttf", 20);
  r->font_title_28 = renderer_create_font("AlimamaShuHeiTi.ttf", 28);
  r->font_title_42 = renderer_create_font("AlimamaShuHeiTi.ttf", 42);
  r->font_body_14 = renderer_create_font("MiSans-Semibold.ttf", 14);
  r->font_body_18 = renderer_create_font("MiSans-Semibold.ttf", 18);
  r->font_english_18 = renderer_create_font("Tinos-Bold.ttf", 18);
  r->font_english_42 = renderer_create_font("Tinos-Bold.ttf", 42);
  r->font_english_72 = renderer_create_font("Tinos-Bold.ttf", 72);
  r->font_english_96 = renderer_create_font("Tinos-Bold.ttf", 96);
  r->font_english_119 = renderer_create_font("Tinos-Bold.ttf", 119);
}

static void renderer_unload_fonts(struct nyabula_eye_renderer_s *r)
{
  int index;

#define DELETE_FONT(name)                          \
  do                                               \
    {                                              \
      if (r->font_##name != NULL)                  \
        {                                          \
          lv_freetype_font_delete(r->font_##name); \
        }                                          \
    }                                              \
  while (0)

  DELETE_FONT(title_20);
  DELETE_FONT(title_28);
  DELETE_FONT(title_42);
  DELETE_FONT(body_14);
  DELETE_FONT(body_18);
  DELETE_FONT(english_18);
  DELETE_FONT(english_42);
  DELETE_FONT(english_72);
  DELETE_FONT(english_96);
  DELETE_FONT(english_119);
#undef DELETE_FONT

  for (index = 0; index < FONT_CACHE_COUNT; index++)
    {
      if (r->font_cache[index].font != NULL)
        {
          lv_freetype_font_delete(r->font_cache[index].font);
          r->font_cache[index].font = NULL;
        }
    }
}
#endif

struct nyabula_eye_renderer_s *
nyabula_eye_renderer_create(lv_obj_t *left_parent, lv_obj_t *right_parent)
{
  struct nyabula_eye_renderer_s *r = calloc(1, sizeof(*r));
  lv_obj_t *parents[NYABULA_EYE_COUNT] = { left_parent, right_parent };
  int eye;
  int page;
  int sample;

  if (r == NULL)
    {
      return NULL;
    }

  if (left_parent == NULL || right_parent == NULL)
    {
      free(r);
      return NULL;
    }

  for (eye = 0; eye < FIBERS; eye++)
    {
      float angle = (float)eye / FIBERS * PI * 2.0f + sinf(eye * 7.0f) * 0.1f;

      r->fibers[eye].cosine = cosf(angle);
      r->fibers[eye].sine = sinf(angle);
      r->fibers[eye].inner_radius =
          0.28f + (float)((eye * 37) % 13) / 13.0f * 0.15f;
      r->fibers[eye].outer_radius =
          0.82f + (float)((eye * 53) % 7) / 7.0f * 0.14f;
    }

  for (sample = 0; sample < HEART_SAMPLES; sample++)
    {
      float angle = sample * PI * 2.0f / HEART_SAMPLES;
      float dx = cosf(angle);
      float dy = sinf(angle);
      float low = 0.0f;
      float high = 3.0f;
      int iteration;

      for (iteration = 0; iteration < 12; iteration++)
        {
          float candidate = (low + high) * 0.5f;
          float nx = dx * candidate;
          float ny = dy * candidate;
          float q = nx * nx + ny * ny - 1.0f;

          if (q * q * q - nx * nx * ny * ny * ny <= 0.0f)
            {
              low = candidate;
            }
          else
            {
              high = candidate;
            }
        }

      r->heart_points[sample].x = dx * low / 1.15f;
      r->heart_points[sample].y = -dy * low / 1.15f;
    }

  for (eye = 0; eye < NYABULA_EYE_COUNT; eye++)
    {
      for (page = 0; page < 2; page++)
        {
          r->buffer[eye][page] = lv_draw_buf_create(
              W, H, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
          if (r->buffer[eye][page] == NULL)
            {
              goto fail;
            }
        }

      r->canvas[eye] = lv_canvas_create(parents[eye]);
      if (r->canvas[eye] == NULL)
        {
          goto fail;
        }

      lv_canvas_set_draw_buf(r->canvas[eye], r->buffer[eye][0]);
      lv_obj_set_pos(r->canvas[eye], left_parent == right_parent ? eye * W : 0,
                     0);
    }

  r->mask_buffer =
      lv_draw_buf_create(W, H, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
  if (r->mask_buffer == NULL)
    {
      goto fail;
    }

  r->mask_canvas = lv_canvas_create(left_parent);
  if (r->mask_canvas == NULL)
    {
      goto fail;
    }

  lv_canvas_set_draw_buf(r->mask_canvas, r->mask_buffer);
  lv_draw_buf_to_image(r->mask_buffer, &r->mask_image);
  lv_obj_add_flag(r->mask_canvas, LV_OBJ_FLAG_HIDDEN);

  for (page = 0; page < BASE_CACHE_COUNT; page++)
    {
      r->base_cache[page].buffer =
          lv_draw_buf_create(W, H, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
      if (r->base_cache[page].buffer == NULL)
        {
          goto fail;
        }

      r->base_cache[page].canvas = lv_canvas_create(left_parent);
      if (r->base_cache[page].canvas == NULL)
        {
          goto fail;
        }

      lv_canvas_set_draw_buf(r->base_cache[page].canvas,
                             r->base_cache[page].buffer);
      lv_obj_add_flag(r->base_cache[page].canvas, LV_OBJ_FLAG_HIDDEN);
    }

  r->random = 0x5a7a5a7au;
  r->perf_start = lv_tick_get();
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  renderer_load_fonts(r);
#endif
  return r;

fail:
  for (page = 0; page < BASE_CACHE_COUNT; page++)
    {
      if (r->base_cache[page].canvas != NULL)
        {
          lv_obj_delete(r->base_cache[page].canvas);
        }

      if (r->base_cache[page].buffer != NULL)
        {
          lv_draw_buf_destroy(r->base_cache[page].buffer);
        }
    }

  if (r->mask_canvas != NULL)
    {
      lv_obj_delete(r->mask_canvas);
    }

  if (r->mask_buffer != NULL)
    {
      lv_draw_buf_destroy(r->mask_buffer);
    }

  for (eye = 0; eye < NYABULA_EYE_COUNT; eye++)
    {
      if (r->canvas[eye] != NULL)
        {
          lv_obj_delete(r->canvas[eye]);
        }

      for (page = 0; page < 2; page++)
        {
          if (r->buffer[eye][page] != NULL)
            {
              lv_draw_buf_destroy(r->buffer[eye][page]);
            }
        }
    }

  free(r);
  return NULL;
}

void nyabula_eye_renderer_destroy(struct nyabula_eye_renderer_s *r)
{
  int eye;
  int page;

  if (r != NULL)
    {
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
      renderer_unload_fonts(r);
#endif
      for (eye = 0; eye < NYABULA_EYE_COUNT; eye++)
        {
          lv_obj_delete(r->canvas[eye]);
          for (page = 0; page < 2; page++)
            {
              lv_draw_buf_destroy(r->buffer[eye][page]);
            }
        }

      lv_obj_delete(r->mask_canvas);
      lv_draw_buf_destroy(r->mask_buffer);
      if (r->path != NULL)
        {
          lv_vector_path_delete(r->path);
        }

      if (r->vector != NULL)
        {
          lv_vector_dsc_delete(r->vector);
        }

      if (r->cache_path != NULL)
        {
          lv_vector_path_delete(r->cache_path);
        }

      if (r->cache_vector != NULL)
        {
          lv_vector_dsc_delete(r->cache_vector);
        }

      if (r->mask_path != NULL)
        {
          lv_vector_path_delete(r->mask_path);
        }

      if (r->mask_vector != NULL)
        {
          lv_vector_dsc_delete(r->mask_vector);
        }

      for (page = 0; page < BASE_CACHE_COUNT; page++)
        {
          lv_obj_delete(r->base_cache[page].canvas);
          lv_draw_buf_destroy(r->base_cache[page].buffer);
        }

      free(r);
    }
}

void nyabula_eye_renderer_render(struct nyabula_eye_renderer_s *r,
                                 const struct nyabula_eye_frame_s frames[2])
{
  struct eye_s e;
  uint32_t start;
  bool share_pixels;
  int id;

  if (r == NULL || frames == NULL)
    {
      return;
    }

  start = lv_tick_get();
  update_z(r, &frames[0]);
  share_pixels = frames_can_share_pixels(frames);
  for (id = 0; id < NYABULA_EYE_COUNT; id++)
    {
      uint32_t eye_start = lv_tick_get();
      uint32_t stage_start;
      lv_draw_buf_t *base_buffer = NULL;
      struct page_state_s *page_state;
      bool minimal_exit;
      bool eye_content;
      bool open_lids = false;

      if (frame_pixels_unchanged(r, &frames[id], id))
        {
          r->reused_eyes++;
          continue;
        }

      r->index[id] ^= 1u;
      r->mask_ready = false;
      r->draw = r->buffer[id][r->index[id]];
      page_state = &r->page_state[id][r->index[id]];
      lv_canvas_set_draw_buf(r->canvas[id], r->draw);
      if (id == NYABULA_EYE_RIGHT && share_pixels)
        {
          const struct page_state_s *source_state =
              &r->page_state[NYABULA_EYE_LEFT][r->index[NYABULA_EYE_LEFT]];
          const lv_area_t *copy_area = NULL;
          lv_area_t dirty_union;

          prepare(&e, &frames[id], id);
          if (page_base_matches(page_state, &e) &&
              page_base_matches(source_state, &e))
            {
              dirty_union = page_dirty_union(page_state, source_state);
              copy_area = &dirty_union;
            }

          stage_start = lv_tick_get();
          lv_draw_buf_copy(
              r->draw, copy_area,
              r->buffer[NYABULA_EYE_LEFT][r->index[NYABULA_EYE_LEFT]],
              copy_area);
          r->copy_total += lv_tick_elaps(stage_start);
          stage_start = lv_tick_get();
          lv_obj_invalidate(r->canvas[id]);
          r->flush_total += lv_tick_elaps(stage_start);
          r->shared_frames++;
          *page_state =
              r->page_state[NYABULA_EYE_LEFT][r->index[NYABULA_EYE_LEFT]];
          remember_frame(r, &frames[id], id);
          continue;
        }

      minimal_exit =
          frames[id].scene.scene != NYABULA_EYE_SCENE_NONE &&
          frames[id].scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL &&
          frames[id].scene.lid < 0.999f && frames[id].scene.alpha < 0.999f;
      eye_content =
          frames[id].scene.scene == NYABULA_EYE_SCENE_NONE || minimal_exit;
      if (eye_content)
        {
          if (frames[id].scene.lid > 0.001f &&
              (frames[id].scene.scene == NYABULA_EYE_SCENE_NONE ||
               minimal_exit))
            {
              prepare_scene_lids(&e, &frames[id], id, frames[id].scene.lid);
            }
          else
            {
              prepare(&e, &frames[id], id);
            }

          base_buffer = base_cache_get(r, &e);
          open_lids = lids_are_open(&e);
        }

      if (base_buffer != NULL)
        {
          const lv_area_t *restore_area =
              page_base_matches(page_state, &e) ? &page_state->dirty : NULL;

          stage_start = lv_tick_get();
          lv_draw_buf_copy(r->draw, restore_area, base_buffer, restore_area);
          r->copy_total += lv_tick_elaps(stage_start);
        }
      else
        {
          lv_canvas_fill_bg(r->canvas[id], lv_color_black(), LV_OPA_COVER);
        }

      lv_canvas_init_layer(r->canvas[id], &r->layer);
      if (r->vector == NULL)
        {
          r->vector = lv_vector_dsc_create(&r->layer);
        }

      if (r->path == NULL)
        {
          r->path = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_HIGH);
        }

      if (r->vector == NULL || r->path == NULL)
        {
          if (r->path != NULL)
            {
              lv_vector_path_delete(r->path);
              r->path = NULL;
            }

          if (r->vector != NULL)
            {
              lv_vector_dsc_delete(r->vector);
              r->vector = NULL;
            }

          lv_canvas_finish_layer(r->canvas[id], &r->layer);
          continue;
        }

      if (frames[id].scene.scene != NYABULA_EYE_SCENE_NONE && !minimal_exit)
        {
          render_scene(r, &frames[id], id);
          prepare_scene(&e, &frames[id], id);
          arc(r, &e, 0.0f, 0.0f, R - 0.8f, 0.0f, PI * 2.0f, 2.0f, 0x3c4655,
              0.45f);
        }
      else
        {
          if (base_buffer == NULL)
            {
              base(r, &e);
            }

          if (base_buffer == NULL)
            {
              iris(r, &e);
            }
          pupil(r, &e);
          overlays(r, &e);
          highlights(r, &e);
          if (!open_lids)
            {
              lids(r, &e);
            }

          draw_z(r, &e);
          if (minimal_exit)
            {
              render_scene(r, &frames[id], id);
            }

          if (base_buffer == NULL || !open_lids)
            {
              arc(r, &e, 0.0f, 0.0f, R - 0.8f, 0.0f, PI * 2.0f, 2.0f, 0x3c4655,
                  0.45f);
            }
        }

      r->build_total += lv_tick_elaps(eye_start);
      stage_start = lv_tick_get();
      lv_draw_vector(r->vector);
      if (r->mask_ready)
        {
          lv_draw_image_dsc_t image_descriptor;
          lv_area_t area = { 0, 0, W - 1, H - 1 };

          lv_draw_image_dsc_init(&image_descriptor);
          image_descriptor.src = &r->mask_image;
          lv_draw_image(&r->layer, &image_descriptor, &area);
        }

      lv_canvas_finish_layer(r->canvas[id], &r->layer);
      r->raster_total += lv_tick_elaps(stage_start);
      stage_start = lv_tick_get();
      lv_draw_buf_flush_cache(r->draw, NULL);
      lv_obj_invalidate(r->canvas[id]);
      r->flush_total += lv_tick_elaps(stage_start);
      if (eye_content && fabsf(e.t.sn) <= 0.0001f &&
          fabsf(e.t.cs - 1.0f) <= 0.0001f)
        {
          bool simple = frames[id].scene.scene == NYABULA_EYE_SCENE_NONE &&
                        frames[id].expression == NYABULA_EYE_EXPRESSION_IDLE &&
                        open_lids && frames[id].overlay <= 0.02f;

          page_remember_base(page_state, &e);
          page_remember_dynamic_area(page_state, &e, simple);
        }
      else
        {
          page_state->valid = false;
        }

      remember_frame(r, &frames[id], id);
    }

  report(r, lv_tick_elaps(start));
}
