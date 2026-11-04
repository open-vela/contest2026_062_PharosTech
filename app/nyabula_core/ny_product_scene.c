/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_scene.c
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

/* Device-owned scene bindings. Data keeps updating after a panel leaves. */
#include "ny_product.h"
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>
#endif

#define NY_PRODUCT_SCENE_SOURCE    "webui"
#define NY_PRODUCT_SCENE_PRIORITY  40
#define NY_PRODUCT_SCENE_PERIOD_MS 1000
#define NY_PRODUCT_SCENE_EQ_POINTS 10

struct ny_product_scene_def_s
{
  const char *name;
  const char *topic;
  const char *kind;
};
struct ny_product_scene_binding_s
{
  char type[24];
  char record[64];
  char principal[64];
  char style[12];
  char caption[48];
  char previous[48];
  char next[48];
  enum ny_product_role_e role;
  bool local;
  int offset;
};
static const struct ny_product_scene_def_s g_scene_defs[] = {
  { "music", "music.status", NULL },
  { "audio", "audio.status", NULL },
  { "eq", "dsp.status", NULL },
  { "subwoofer", "dsp.status", NULL },
  { "call", "bt.status", NULL },
  { "weather", "weather.get", NULL },
  { "timer", "timer.list", "countdown" },
  { "stopwatch", "timer.list", "stopwatch" },
  { "sleep_timer", "timer.list", "sleep" },
  { "alarm", "alarm.list", NULL },
  { "calendar", "calendar.list", NULL },
  { "task", "task.list", NULL },
  { "memory", "memory.list", NULL },
  { "briefing", "briefing.get", NULL },
  { "companion", "companion.get", NULL },
  { "network", "device.status", NULL },
  { "devices", "device.status", NULL },
  { "system", "device.status", NULL },
  { "caption", NULL, "input" },
  { "qr", NULL, "pairing" },
  { "battery", NULL, "hardware" },
  { "presence", NULL, "hardware" },
  { "health", NULL, "hardware" },
  { "identity", NULL, "hardware" },
  { "privacy", NULL, "hardware" },
  { "home", NULL, "hardware" }
};
static mutex_t g_scene_lock = NXMUTEX_INITIALIZER;
static struct ny_product_scene_binding_s g_scene_binding;
static bool g_scene_active;
static bool g_scene_shown;
static int g_scene_error;
static uint64_t g_scene_next;
static uint64_t g_scene_updated;
static char *g_scene_last;

static const char *ny_product_scene_text(const cJSON *root, const char *key);
static double ny_product_scene_number(const cJSON *root, const char *key);
static bool ny_product_scene_bool(const cJSON *root, const char *key);
static void ny_product_scene_copy(char *out, size_t size, const char *text);
static bool ny_product_scene_add_text(cJSON *root, const char *key,
                                      const char *text);
static const struct ny_product_scene_def_s *
ny_product_scene_find(const char *name);
static int
ny_product_scene_read(const struct ny_product_scene_binding_s *binding,
                      const char *topic, cJSON **result);
static const cJSON *
ny_product_scene_record(struct ny_product_scene_binding_s *binding,
                        const struct ny_product_scene_def_s *def,
                        const cJSON *data);
static double ny_product_scene_eq(const cJSON *bands, double hz);
static int ny_product_scene_build(struct ny_product_scene_binding_s *binding,
                                  cJSON **result);
static int ny_product_scene_submit(const char *action, const char *type,
                                   const char *style, const cJSON *payload);
static cJSON *ny_product_scene_status(void);

static const char *ny_product_scene_text(const cJSON *root, const char *key)
{
  const char *text =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, key));
  return text != NULL ? text : "";
}
static double ny_product_scene_number(const cJSON *root, const char *key)
{
  return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, key));
}
static bool ny_product_scene_bool(const cJSON *root, const char *key)
{
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, key));
}
static void ny_product_scene_copy(char *out, size_t size, const char *text)
{
  size_t length = strlen(text);
  if (length >= size)
    {
      length = size - 1;
      while (length && ((unsigned char)text[length] & 0xc0) == 0x80)
        length--;
    }
  memcpy(out, text, length);
  out[length] = '\0';
}
static bool ny_product_scene_add_text(cJSON *root, const char *key,
                                      const char *text)
{
  char clipped[48];
  ny_product_scene_copy(clipped, !strcmp(key, "value") ? 24 : sizeof(clipped),
                        text);
  return cJSON_AddStringToObject(root, key, clipped) != NULL;
}
static const struct ny_product_scene_def_s *
ny_product_scene_find(const char *name)
{
  for (size_t i = 0; i < sizeof(g_scene_defs) / sizeof(g_scene_defs[0]); i++)
    if (!strcmp(name, g_scene_defs[i].name))
      return &g_scene_defs[i];
  return NULL;
}
static int
ny_product_scene_read(const struct ny_product_scene_binding_s *binding,
                      const char *topic, cJSON **result)
{
  struct ny_product_caller_s caller = { binding->principal, binding->role,
                                        binding->local };
  cJSON *request = cJSON_CreateObject();
  if (request == NULL)
    return -ENOMEM;
  if (!strcmp(topic, "weather.get"))
    cJSON_AddStringToObject(request, "section", "now");
  int ret = ny_product_request(&caller, topic, request, result);
  cJSON_Delete(request);
  return ret;
}
static const cJSON *
ny_product_scene_record(struct ny_product_scene_binding_s *binding,
                        const struct ny_product_scene_def_s *def,
                        const cJSON *data)
{
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
  const cJSON *row;
  const cJSON *chosen = NULL;
  double earliest = INFINITY;
  double latest = -INFINITY;
  int best_rank = 10;
  cJSON_ArrayForEach(row, items)
  {
    if (def->kind && strcmp(ny_product_scene_text(row, "kind"), def->kind))
      continue;
    if (binding->record[0])
      {
        if (!strcmp(binding->record, ny_product_scene_text(row, "id")))
          return row;
        continue;
      }
    if (!strcmp(def->name, "alarm"))
      {
        if (!strcmp(ny_product_scene_text(row, "status"), "ringing"))
          {
            chosen = row;
            break;
          }
        double next = ny_product_scene_number(row, "next_at");
        if (next > 0 && next < earliest)
          {
            chosen = row;
            earliest = next;
          }
      }
    else if (!strcmp(def->name, "calendar"))
      {
        double next = ny_product_scene_number(row, "start_at");
        if (next >= floor(((double)ny_product_time_ms(false) +
                           binding->offset * 60000.0) /
                          86400000.0) *
                            86400000.0 -
                        binding->offset * 60000.0 &&
            next < earliest)
          {
            chosen = row;
            earliest = next;
          }
      }
    else if (!strcmp(def->name, "task"))
      {
        const char *state = ny_product_scene_text(row, "state");
        int rank = !strcmp(state, "running")   ? 0
                   : !strcmp(state, "confirm") ? 1
                   : !strcmp(state, "queued")  ? 2
                                               : 3;
        if (rank < best_rank)
          {
            chosen = row;
            best_rank = rank;
          }
      }
    else if (!strcmp(def->name, "memory"))
      {
        double at = ny_product_scene_number(row, "at");
        if (!chosen || (isfinite(at) && at >= latest))
          {
            chosen = row;
            latest = at;
          }
      }
    else if (!chosen ||
             !strcmp(ny_product_scene_text(chosen, "status"), "finished") ||
             strcmp(ny_product_scene_text(row, "status"), "finished"))
      chosen = row;
  }
  if (chosen)
    ny_product_scene_copy(binding->record, sizeof(binding->record),
                          ny_product_scene_text(chosen, "id"));
  return chosen;
}

/* Evaluate the same RBJ biquads used by the configured equalizer. */
static double ny_product_scene_eq(const cJSON *bands, double hz)
{
  double response = 0;
  const cJSON *band;
  cJSON_ArrayForEach(band, bands)
  {
    if (!ny_product_scene_bool(band, "enabled"))
      continue;
    double f = ny_product_scene_number(band, "frequencyHz");
    double q = ny_product_scene_number(band, "q");
    double g = ny_product_scene_number(band, "gainDb");
    if (!(f > 0 && f < 24000 && q > 0 && isfinite(g)))
      continue;
    double w = 2 * 3.141592653589793 * f / 48000;
    double c = cos(w), a = pow(10, g / 40), alpha = sin(w) / (2 * q);
    double beta = 2 * sqrt(a) * alpha;
    double b0, b1, b2, a0, a1, a2;
    const char *type = ny_product_scene_text(band, "type");
    if (!strcmp(type, "lowShelf"))
      {
        b0 = a * ((a + 1) - (a - 1) * c + beta);
        b1 = 2 * a * ((a - 1) - (a + 1) * c);
        b2 = a * ((a + 1) - (a - 1) * c - beta);
        a0 = (a + 1) + (a - 1) * c + beta;
        a1 = -2 * ((a - 1) + (a + 1) * c);
        a2 = (a + 1) + (a - 1) * c - beta;
      }
    else if (!strcmp(type, "highShelf"))
      {
        b0 = a * ((a + 1) + (a - 1) * c + beta);
        b1 = -2 * a * ((a - 1) + (a + 1) * c);
        b2 = a * ((a + 1) + (a - 1) * c - beta);
        a0 = (a + 1) - (a - 1) * c + beta;
        a1 = 2 * ((a - 1) - (a + 1) * c);
        a2 = (a + 1) - (a - 1) * c - beta;
      }
    else
      {
        b0 = 1 + alpha * a;
        b1 = -2 * c;
        b2 = 1 - alpha * a;
        a0 = 1 + alpha / a;
        a1 = -2 * c;
        a2 = 1 - alpha / a;
      }
    w = 2 * 3.141592653589793 * hz / 48000;
    double nr = b0 + b1 * cos(w) + b2 * cos(2 * w),
           ni = b1 * sin(w) + b2 * sin(2 * w);
    double dr = a0 + a1 * cos(w) + a2 * cos(2 * w),
           di = a1 * sin(w) + a2 * sin(2 * w);
    double den = dr * dr + di * di, num = nr * nr + ni * ni;
    if (den > 0 && num > 0)
      response += 10 * log10(num / den);
  }
  return response;
}

static int ny_product_scene_build(struct ny_product_scene_binding_s *binding,
                                  cJSON **result)
{
  const struct ny_product_scene_def_s *def =
      ny_product_scene_find(binding->type);
  cJSON *data = NULL, *extra = NULL;
  cJSON *p = cJSON_CreateObject();
  bool valid = p != NULL;
  int ret = 0;
  char text[96];
  *result = NULL;
  if (!p)
    return -ENOMEM;
  if (!def || (!def->topic && strcmp(def->name, "caption")))
    {
      ret = -ENOTSUP;
      goto done;
    }
  if (def->topic)
    {
      ret = ny_product_scene_read(binding, def->topic, &data);
      if (ret < 0)
        goto done;
    }
#define NUM(k, v)                                             \
  do                                                          \
    {                                                         \
      double number = (v);                                    \
      if (!isfinite(number))                                  \
        {                                                     \
          ret = -EBADMSG;                                     \
          goto done;                                          \
        }                                                     \
      valid = valid && cJSON_AddNumberToObject(p, k, number); \
    }                                                         \
  while (0)
#define TXT(k, v)                                          \
  do                                                       \
    {                                                      \
      valid = valid && ny_product_scene_add_text(p, k, v); \
    }                                                      \
  while (0)
#define BOOL(k, v)                                     \
  do                                                   \
    {                                                  \
      valid = valid && cJSON_AddBoolToObject(p, k, v); \
    }                                                  \
  while (0)
  BOOL("live_data", true);
  const char *type = def->name;
  if (!strcmp(type, "caption"))
    {
      if (!binding->caption[0])
        {
          ret = -ENODATA;
          goto done;
        }
      TXT("current_line", binding->caption);
      TXT("previous_line", binding->previous);
      TXT("next_line", binding->next);
    }
  else if (def->kind || !strcmp(type, "alarm") || !strcmp(type, "calendar") ||
           !strcmp(type, "task") || !strcmp(type, "memory"))
    {
      const cJSON *row = ny_product_scene_record(binding, def, data);
      if (!row)
        {
          ret = -ENODATA;
          goto done;
        }
      if (def->kind)
        {
          bool running =
              !strcmp(ny_product_scene_text(row, "status"), "running");
          if (!strcmp(ny_product_scene_text(row, "status"), "waiting-clock"))
            {
              ret = -EAGAIN;
              goto done;
            }
          NUM("duration_ms", ny_product_scene_number(row, "duration_ms"));
          NUM("remaining_ms", ny_product_scene_number(row, "remaining_ms"));
          NUM("elapsed_ms", ny_product_scene_number(row, "elapsed_ms"));
          BOOL("active", running);
          TXT("title", ny_product_scene_text(row, "label"));
          TXT("detail",
              running ? "设备计时中"
              : !strcmp(ny_product_scene_text(row, "status"), "finished")
                  ? "时间到"
                  : "已暂停");
        }
      else if (!strcmp(type, "alarm"))
        {
          unsigned hour, minute;
          if (sscanf(ny_product_scene_text(row, "time"), "%u:%u", &hour,
                     &minute) != 2 ||
              hour > 23 || minute > 59)
            {
              ret = -EBADMSG;
              goto done;
            }
          NUM("hour", hour);
          NUM("minute", minute);
          TXT("title", ny_product_scene_text(row, "label"));
          TXT("alarm_copy", "name");
          BOOL("active",
               !strcmp(ny_product_scene_text(row, "status"), "ringing"));
        }
      else if (!strcmp(type, "calendar"))
        {
          double start = ny_product_scene_number(row, "start_at");
          if (!isfinite(start))
            {
              ret = -EBADMSG;
              goto done;
            }
          time_t seconds = (time_t)(start / 1000) + binding->offset * 60;
          struct tm tm;
          if (!gmtime_r(&seconds, &tm))
            {
              ret = -ERANGE;
              goto done;
            }
          NUM("year", tm.tm_year + 1900);
          NUM("month", tm.tm_mon + 1);
          NUM("day", tm.tm_mday);
          NUM("hour", tm.tm_hour);
          NUM("minute", tm.tm_min);
          TXT("title", ny_product_scene_text(row, "title"));
          TXT("detail",
              tm.tm_hour == 0 && tm.tm_min == 0 ? "全天日程" : "设备日程");
        }
      else if (!strcmp(type, "task"))
        {
          const char *state = ny_product_scene_text(row, "state");
          double progress = ny_product_scene_number(row, "progress");
          TXT("title", ny_product_scene_text(row, "title"));
          TXT("task_state", !strcmp(state, "cancelled") ? "failed" : state);
          if (isfinite(progress))
            {
              NUM("progress", fmin(1, fmax(0, progress / 100)));
              snprintf(text, sizeof(text), "%.0f%%",
                       fmin(100, fmax(0, progress)));
              TXT("value", text);
            }
          BOOL("active", !isfinite(progress) && !strcmp(state, "running"));
        }
      else
        {
          const char *text_value = ny_product_scene_text(row, "text");
          const char *tag = ny_product_scene_text(row, "tag");
          const char *label = !strcmp(tag, "daily")    ? "日常"
                              : !strcmp(tag, "family") ? "家人"
                              : !strcmp(tag, "pet")    ? "猫咪"
                              : !strcmp(tag, "todo")   ? "待办"
                              : !strcmp(tag, "fun")    ? "趣事"
                                                       : tag;
          char first[48], second[48];
          ny_product_scene_copy(first, sizeof(first), text_value);
          ny_product_scene_copy(second, sizeof(second),
                                text_value + strlen(first));
          TXT("title", first);
          TXT("subtitle", second[0] ? second : label);
          TXT("detail", second[0] ? label : "设备记忆");
        }
    }
  else if (!strcmp(type, "system") || !strcmp(type, "network") ||
           !strcmp(type, "devices"))
    {
      const cJSON *network = cJSON_GetObjectItemCaseSensitive(data, "network");
      const cJSON *interfaces =
          cJSON_GetObjectItemCaseSensitive(network, "interfaces");
      const cJSON *row;
      const cJSON *online = NULL;
      int networks = 0, audio = 0, storage = 0, camera = 0, lighting = 0;
      cJSON_ArrayForEach(
          row, interfaces) if (ny_product_scene_bool(row, "up") &&
                               !ny_product_scene_bool(row, "loopback"))
      {
        online = row;
        networks++;
      }
      cJSON_ArrayForEach(
          row,
          cJSON_GetObjectItemCaseSensitive(
              data, "audioDevices")) if (ny_product_scene_bool(row,
                                                               "available"))
          audio++;
      cJSON_ArrayForEach(
          row,
          cJSON_GetObjectItemCaseSensitive(
              data, "storage")) if (ny_product_scene_bool(row, "available"))
          storage++;
      cJSON_ArrayForEach(
          row, cJSON_GetObjectItemCaseSensitive(
                   data,
                   "peripherals")) if (ny_product_scene_bool(row, "available"))
      {
        const char *kind = ny_product_scene_text(row, "kind");
        if (!strcmp(kind, "camera"))
          camera++;
        if (!strcmp(kind, "lighting"))
          lighting++;
      }
      if (!strcmp(type, "devices"))
        {
          NUM("device_count", audio + storage + networks + camera + lighting);
          snprintf(text, sizeof(text), "%d",
                   audio + storage + networks + camera + lighting);
          TXT("title", text);
          TXT("subtitle", "个硬件在线");
          snprintf(text, sizeof(text), "音频 %d · 存储 %d · 网络 %d", audio,
                   storage, networks);
          if (camera || lighting)
            snprintf(text, sizeof(text),
                     "音频 %d 存储 %d 网络 %d 摄影 %d 灯光 %d", audio, storage,
                     networks, camera, lighting);
          TXT("detail", text);
        }
      else if (!strcmp(type, "network"))
        {
          TXT("network_state", online ? "wifi" : "offline");
          TXT("title",
              online ? ny_product_scene_text(online, "ssid") : "未连接");
          TXT("detail", online ? ny_product_scene_text(online, "ipv4") : "");
          BOOL("signal_good", online != NULL);
        }
      else
        {
          const cJSON *cpu = cJSON_GetObjectItemCaseSensitive(data, "cpu"),
                      *mem = cJSON_GetObjectItemCaseSensitive(data, "memory");
          double percent = ny_product_scene_number(cpu, "percent"),
                 total = ny_product_scene_number(mem, "totalBytes"),
                 used = ny_product_scene_number(mem, "usedBytes");
          if (ny_product_scene_bool(cpu, "available") && isfinite(percent))
            {
              snprintf(text, sizeof(text), "CPU %.0f%%", percent);
              NUM("percent", round(percent));
            }
          else
            snprintf(text, sizeof(text), "OPENVELA");
          TXT("title", text);
          snprintf(text, sizeof(text), "内存 %.0f%% · 运行 %.0f 分",
                   total > 0 ? 100 * used / total : 0,
                   floor(ny_product_scene_number(data, "uptimeMs") / 60000));
          TXT("subtitle", text);
          TXT("detail", ny_product_scene_text(data, "boardModel"));
        }
    }
  else if (!strcmp(type, "audio"))
    {
      const cJSON *out = cJSON_GetObjectItemCaseSensitive(data, "output");
      bool mute = ny_product_scene_bool(out, "muted");
      const char *route = ny_product_scene_text(out, "activeRoute");
      TXT("audio_route", mute                          ? "mute"
                         : !strcmp(route, "headphone") ? "headphones"
                                                       : route);
      NUM("percent", mute ? 0 : ny_product_scene_number(out, "volume"));
      TXT("title", mute ? "已静音" : "设备音频输出");
    }
  else if (!strcmp(type, "eq") || !strcmp(type, "subwoofer"))
    {
      const cJSON *c = cJSON_GetObjectItemCaseSensitive(data, "config");
      if (!cJSON_IsObject(c))
        {
          ret = -EBADMSG;
          goto done;
        }
      bool bypass = ny_product_scene_bool(c, "bypass");
      if (!strcmp(type, "subwoofer"))
        {
          NUM("crossover_hz", ny_product_scene_number(c, "crossoverHz"));
          snprintf(text, sizeof(text), "%.0f Hz",
                   ny_product_scene_number(c, "crossoverHz"));
          TXT("subtitle", text);
          BOOL("active", !bypass && ny_product_scene_bool(c, "bassEnabled") &&
                             ny_product_scene_bool(data, "bassPresent"));
          TXT("title", ny_product_scene_bool(data, "bassPresent")
                           ? "USB 低音"
                           : "USB 未连接");
          snprintf(text, sizeof(text), "高通 %.0f / 低通 %.0f Hz",
                   ny_product_scene_number(c, "satelliteHighpassHz"),
                   ny_product_scene_number(c, "bassLowpassHz"));
          TXT("detail", text);
        }
      else
        {
          bool on = !bypass && ny_product_scene_bool(c, "eqEnabled");
          const cJSON *bands =
              cJSON_GetObjectItemCaseSensitive(c, "eqBands16");
          if (!cJSON_IsArray(bands))
            bands = cJSON_GetObjectItemCaseSensitive(c, "eqBands");
          cJSON *values = cJSON_AddArrayToObject(p, "eq_bands");
          valid = valid && values;
          for (int i = 0; i < NY_PRODUCT_SCENE_EQ_POINTS && valid; i++)
            {
              double hz =
                  20 * pow(1000, (double)i / (NY_PRODUCT_SCENE_EQ_POINTS - 1));
              valid = cJSON_AddItemToArray(
                  values,
                  cJSON_CreateNumber(on ? ny_product_scene_eq(bands, hz) : 0));
            }
          TXT("eq_view", "profile");
          TXT("title", on ? "当前均衡器" : "原声直通");
          snprintf(text, sizeof(text), "总增益 %.1f dB",
                   ny_product_scene_number(c, "masterDb"));
          TXT("detail", text);
        }
    }
  else if (!strcmp(type, "music"))
    {
      bool playing = !strcmp(ny_product_scene_text(data, "state"), "playing");
      TXT("title", ny_product_scene_text(data, "track")[0]
                       ? ny_product_scene_text(data, "track")
                       : "未播放");
      BOOL("playing", playing);
      NUM("position_ms", ny_product_scene_number(data, "elapsedMs"));
      NUM("duration_ms", ny_product_scene_number(data, "durationMs"));
      int got = ny_product_scene_read(binding, "dsp.status", &extra);
      double peak =
          got == 0 ? ny_product_scene_number(extra, "outputPeak") : NAN;
      bool measured = got == 0 && ny_product_scene_bool(extra, "running") &&
                      playing && isfinite(peak);
      BOOL("signal_good", measured);
      NUM("percent", measured ? round(fmin(1, fmax(0, peak)) * 100) : 0);
    }
  else if (!strcmp(type, "call"))
    {
      const cJSON *h = cJSON_GetObjectItemCaseSensitive(data, "hfp");
      const char *state = ny_product_scene_text(h, "call");
      bool active = strcmp(state, "idle") != 0;
      TXT("call_state", !strcmp(state, "incoming") ? "incoming"
                        : active                   ? "active"
                                                   : "ended");
      BOOL("active", active);
      TXT("title", !strcmp(state, "incoming") ? "来电"
                   : active                   ? "正在通话"
                                              : "暂无通话");
      TXT("subtitle", ny_product_scene_text(h, "number"));
      TXT("detail", ny_product_scene_text(h, "number")[0]
                        ? ny_product_scene_text(h, "number")
                    : active ? "蓝牙通话"
                             : "等待来电");
      TXT("value", active ? "HFP" : "IDLE");
    }
  else if (!strcmp(type, "weather"))
    {
      const cJSON *temperature =
          cJSON_GetObjectItemCaseSensitive(data, "temperature");
      double t = ny_product_scene_number(temperature, "value");
      if (!isfinite(t))
        {
          ret = -ENODATA;
          goto done;
        }
      NUM("temperature_c", t);
      const cJSON *condition =
                      cJSON_GetObjectItemCaseSensitive(data, "condition"),
                  *place = cJSON_GetObjectItemCaseSensitive(data, "location");
      const char *code = ny_product_scene_text(condition, "code");
      int wmo = !strncmp(code, "wmo-", 4) ? atoi(code + 4) : -1;
      const char *kind = wmo == 0 || wmo == 1     ? "sunny"
                         : wmo == 45 || wmo == 48 ? "fog"
                         : wmo >= 95              ? "storm"
                         : (wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86
                             ? "snow"
                         : wmo >= 51 ? "rain"
                                     : "cloudy";
      if (wmo < 0)
        {
          int condition_code = atoi(code);
          kind = condition_code == 100 || condition_code == 150   ? "sunny"
                 : condition_code >= 302 && condition_code <= 304 ? "storm"
                 : condition_code >= 300 && condition_code < 400  ? "rain"
                 : condition_code >= 400 && condition_code < 500  ? "snow"
                 : condition_code >= 500 && condition_code < 600  ? "fog"
                                                                  : "cloudy";
        }
      TXT("weather", kind);
      TXT("title", ny_product_scene_text(place, "name"));
      TXT("subtitle", ny_product_scene_text(condition, "text"));
      double n = ny_product_scene_number(data, "humidity");
      if (isfinite(n))
        NUM("humidity_percent", n);
      n = ny_product_scene_number(
          cJSON_GetObjectItemCaseSensitive(data, "feelsLike"), "value");
      if (isfinite(n))
        NUM("feels_like_c", n);
      const cJSON *wind = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(data, "wind"), "speed");
      n = ny_product_scene_number(wind, "value");
      if (isfinite(n))
        {
          if (!strcmp(ny_product_scene_text(wind, "unit"), "m/s"))
            n *= 3.6;
          NUM("wind_kph", n);
        }
    }
  else if (!strcmp(type, "briefing"))
    {
      const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
      int count = cJSON_GetArraySize(items);
      double selected = ny_product_scene_number(data, "index");
      int index = isfinite(selected) ? (int)selected : 0;
      if (count <= 0)
        {
          ret = -ENODATA;
          goto done;
        }
      if (index < 0 || index >= count)
        index = 0;
      const cJSON *item = cJSON_GetArrayItem(items, index);
      TXT("title", ny_product_scene_text(item, "title"));
      TXT("detail", ny_product_scene_text(item, "text"));
      NUM("briefing_index", index + 1);
      NUM("briefing_count", count);
      BOOL("playing", ny_product_scene_bool(data, "playing"));
    }
  else if (!strcmp(type, "companion"))
    {
      bool on = ny_product_scene_bool(data, "enabled"),
           quiet = ny_product_scene_bool(data, "quiet_now");
      BOOL("active", on && !quiet);
      TXT("title", "陪伴");
      TXT("subtitle", !on     ? "主动陪伴已关闭"
                      : quiet ? "免打扰中"
                              : "主动陪伴已开启");
      snprintf(text, sizeof(text), "今日 %.0f / %.0f 次",
               ny_product_scene_number(data, "daily_count"),
               ny_product_scene_number(data, "daily_limit"));
      TXT("detail", text);
    }
  if (!valid)
    ret = -ENOMEM;
done:
  cJSON_Delete(data);
  cJSON_Delete(extra);
  if (ret < 0)
    cJSON_Delete(p);
  else
    *result = p;
  return ret;
#undef NUM
#undef TXT
#undef BOOL
}

static int ny_product_scene_submit(const char *action, const char *type,
                                   const char *style, const cJSON *payload)
{
#ifdef CONFIG_NYABULA_CORE_EYE
  cJSON *command = cJSON_CreateObject(), *params = cJSON_CreateObject();
  if (!command || !params)
    {
      cJSON_Delete(command);
      cJSON_Delete(params);
      return -ENOMEM;
    }
  cJSON_AddItemToObject(command, "params", params);
  bool valid =
      cJSON_AddStringToObject(command, "source", NY_PRODUCT_SCENE_SOURCE) &&
      cJSON_AddStringToObject(command, "id", "scene-data") &&
      cJSON_AddStringToObject(command, "action", action) &&
      cJSON_AddNumberToObject(command, "priority",
                              NY_PRODUCT_SCENE_PRIORITY) &&
      cJSON_AddNumberToObject(command, "lease_ms", 0);
  if (type)
    valid = valid && cJSON_AddStringToObject(params, "scene", type) &&
            cJSON_AddStringToObject(params, "style", style) &&
            cJSON_AddItemToObject(params, "payload",
                                  cJSON_Duplicate(payload, true));
  char *json = valid ? cJSON_PrintUnformatted(command) : NULL;
  int ret = json ? nyabula_eye_service_submit(NY_PRODUCT_SCENE_SOURCE, json,
                                              strlen(json))
                 : -ENOMEM;
  cJSON_free(json);
  cJSON_Delete(command);
  return ret;
#else
  (void)action;
  (void)type;
  (void)style;
  (void)payload;
  return -ENOTSUP;
#endif
}
static cJSON *ny_product_scene_status(void)
{
  cJSON *status = cJSON_CreateObject();
  if (status)
    {
      cJSON_AddBoolToObject(status, "active", g_scene_active);
      cJSON_AddBoolToObject(status, "shown", g_scene_shown);
      cJSON_AddStringToObject(status, "scene", g_scene_binding.type);
      cJSON_AddStringToObject(status, "recordId", g_scene_binding.record);
      cJSON_AddNumberToObject(status, "lastError", g_scene_error);
      cJSON_AddNumberToObject(status, "updatedAtMs", g_scene_updated);
    }
  return status;
}

int ny_product_scene_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result)
{
  if (strncmp(topic, "scene.", 6))
    return -ENOSYS;
  if (caller->role < NY_PRODUCT_FAMILY)
    return -EACCES;
  if (strcmp(topic, "scene.show") && strcmp(topic, "scene.hide") &&
      strcmp(topic, "scene.list") && strcmp(topic, "scene.status"))
    return -ENOSYS;
  nxmutex_lock(&g_scene_lock);
  int ret = 0;
  if (!strcmp(topic, "scene.list"))
    {
      *result = cJSON_CreateObject();
      cJSON *items = *result ? cJSON_AddArrayToObject(*result, "items") : NULL;
      if (!items)
        {
          ret = -ENOMEM;
          goto out;
        }
      for (size_t i = 0; i < sizeof(g_scene_defs) / sizeof(g_scene_defs[0]);
           i++)
        {
          struct ny_product_scene_binding_s b = { 0 };
          cJSON *payload = NULL;
          ny_product_scene_copy(b.principal, sizeof(b.principal), caller->id);
          b.role = caller->role;
          b.local = caller->local_transport;
          double zone = ny_product_scene_number(data, "utcOffsetMinutes");
          if (isfinite(zone) && zone >= -840 && zone <= 840 &&
              floor(zone) == zone)
            b.offset = (int)zone;
          ny_product_scene_copy(b.type, sizeof(b.type), g_scene_defs[i].name);
          int code = ny_product_scene_build(&b, &payload);
          cJSON_Delete(payload);
          cJSON *entry = cJSON_CreateObject();
          if (!entry)
            {
              ret = -ENOMEM;
              goto out;
            }
          cJSON_AddStringToObject(entry, "scene", b.type);
          cJSON_AddBoolToObject(entry, "available", code == 0);
          cJSON_AddNumberToObject(entry, "error", code);
          cJSON_AddStringToObject(entry, "source",
                                  g_scene_defs[i].topic
                                      ? g_scene_defs[i].topic
                                      : g_scene_defs[i].kind);
          cJSON_AddItemToArray(items, entry);
        }
      goto out;
    }
  if (!strcmp(topic, "scene.hide"))
    {
      ret = ny_product_scene_submit("eyes.scene.hide", NULL, NULL, NULL);
      if (ret == 0)
        {
          g_scene_active = false;
          g_scene_shown = false;
          cJSON_free(g_scene_last);
          g_scene_last = NULL;
        }
    }
  else if (!strcmp(topic, "scene.show"))
    {
      struct ny_product_scene_binding_s b = { 0 };
      const char *name = ny_product_scene_text(data, "scene"),
                 *record = ny_product_scene_text(data, "recordId"),
                 *style = ny_product_scene_text(data, "style");
      if (!ny_product_scene_find(name) || strlen(record) >= sizeof(b.record) ||
          (*style && strcmp(style, "full") && strcmp(style, "minimal")))
        {
          ret = -EINVAL;
          goto out;
        }
      const cJSON *zone =
          cJSON_GetObjectItemCaseSensitive(data, "utcOffsetMinutes");
      double offset =
          zone ? ny_product_scene_number(data, "utcOffsetMinutes") : 0;
      if (!isfinite(offset))
        {
          ret = -EINVAL;
          goto out;
        }
      if (offset < -840 || offset > 840 || floor(offset) != offset)
        {
          ret = -EINVAL;
          goto out;
        }
      b.offset = (int)offset;
      b.role = caller->role;
      b.local = caller->local_transport;
      ny_product_scene_copy(b.principal, sizeof(b.principal), caller->id);
      ny_product_scene_copy(b.type, sizeof(b.type), name);
      ny_product_scene_copy(b.record, sizeof(b.record), record);
      if (!record[0] && g_scene_active &&
          !strcmp(name, g_scene_binding.type) &&
          !strcmp(caller->id, g_scene_binding.principal))
        ny_product_scene_copy(b.record, sizeof(b.record),
                              g_scene_binding.record);
      ny_product_scene_copy(b.style, sizeof(b.style), *style ? style : "full");
      const cJSON *manual = cJSON_GetObjectItemCaseSensitive(data, "payload");
      ny_product_scene_copy(b.caption, sizeof(b.caption),
                            ny_product_scene_text(manual, "current_line"));
      ny_product_scene_copy(b.previous, sizeof(b.previous),
                            ny_product_scene_text(manual, "previous_line"));
      ny_product_scene_copy(b.next, sizeof(b.next),
                            ny_product_scene_text(manual, "next_line"));
      cJSON *payload = NULL;
      ret = ny_product_scene_build(&b, &payload);
      if (ret == 0)
        ret = ny_product_scene_submit("eyes.scene.show", b.type, b.style,
                                      payload);
      if (ret == 0)
        {
          g_scene_binding = b;
          g_scene_active = true;
          g_scene_shown = true;
          g_scene_error = 0;
          g_scene_next = ny_product_time_ms(true) + NY_PRODUCT_SCENE_PERIOD_MS;
          g_scene_updated = ny_product_time_ms(true);
          cJSON_free(g_scene_last);
          g_scene_last = cJSON_PrintUnformatted(payload);
        }
      cJSON_Delete(payload);
    }
  if (ret == 0)
    {
      *result = ny_product_scene_status();
      if (!*result)
        ret = -ENOMEM;
    }
out:
  nxmutex_unlock(&g_scene_lock);
  return ret;
}

int ny_product_scene_tick(void)
{
  nxmutex_lock(&g_scene_lock);
  uint64_t now = ny_product_time_ms(true);
  if (!g_scene_active || now < g_scene_next)
    {
      nxmutex_unlock(&g_scene_lock);
      return 0;
    }
  g_scene_next = now + NY_PRODUCT_SCENE_PERIOD_MS;
  cJSON *payload = NULL;
  int ret = ny_product_scene_build(&g_scene_binding, &payload);
  char *text = ret == 0 ? cJSON_PrintUnformatted(payload) : NULL;
  if (ret == 0 && !text)
    ret = -ENOMEM;
  if (ret == 0 &&
      (!g_scene_shown || !g_scene_last || strcmp(text, g_scene_last)))
    {
      ret = ny_product_scene_submit(
          g_scene_shown ? "eyes.scene.update" : "eyes.scene.show",
          g_scene_binding.type, g_scene_binding.style, payload);
      if (ret == 0)
        {
          g_scene_shown = true;
          cJSON_free(g_scene_last);
          g_scene_last = text;
          text = NULL;
          g_scene_updated = now;
        }
    }
  else if (ret < 0 && g_scene_shown)
    {
      ny_product_scene_submit("eyes.scene.hide", NULL, NULL, NULL);
      g_scene_shown = false;
    }
  g_scene_error = ret;
  cJSON_free(text);
  cJSON_Delete(payload);
  nxmutex_unlock(&g_scene_lock);
  return ret;
}

int ny_product_scene_manual_submit(const char *source, const char *json,
                                   size_t length)
{
#ifdef CONFIG_NYABULA_CORE_EYE
  nxmutex_lock(&g_scene_lock);
  int ret = nyabula_eye_service_submit(source, json, length);
  if (ret == 0 && !strcmp(source, NY_PRODUCT_SCENE_SOURCE))
    {
      g_scene_active = false;
      g_scene_shown = false;
      cJSON_free(g_scene_last);
      g_scene_last = NULL;
    }
  nxmutex_unlock(&g_scene_lock);
  return ret;
#else
  (void)source;
  (void)json;
  (void)length;
  return -ENOTSUP;
#endif
}

void ny_product_scene_shutdown(void)
{
  nxmutex_lock(&g_scene_lock);
  if (g_scene_shown)
    ny_product_scene_submit("eyes.scene.hide", NULL, NULL, NULL);
  g_scene_active = false;
  g_scene_shown = false;
  cJSON_free(g_scene_last);
  g_scene_last = NULL;
  nxmutex_unlock(&g_scene_lock);
}
