/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_weather.c
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

#include "infra/vela_tls.h"
#include "ny_product.h"
#include "ny_product_store.h"
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NY_WEATHER_BODY_LIMIT    65536
#define NY_WEATHER_POLL_MS       600000ULL
#define NY_WEATHER_SECTION_COUNT 4

static mutex_t g_weather_lock = NXMUTEX_INITIALIZER;
static cJSON *g_weather_config;
static uint64_t g_weather_revision;
static uint64_t g_weather_generation;
static uint64_t g_weather_job_generation;
static uint64_t g_weather_next;
static bool g_weather_running;
static int g_weather_error;
static int g_weather_errors[NY_WEATHER_SECTION_COUNT];
static const char *g_weather_sections[] = { "now", "daily", "hourly",
                                            "alerts" };
static const char *g_weather_domains[] = { "weather-now", "weather-daily",
                                           "weather-hourly",
                                           "weather-alerts" };

static const char *ny_weather_text(const cJSON *row, const char *key);
static bool ny_weather_host(const char *host);
static int ny_weather_load(void);
static cJSON *ny_weather_status(void);
static int ny_weather_fetch(const cJSON *config, const char *path,
                            cJSON **result);
static int ny_weather_location(const cJSON *config, cJSON **result);
static uint64_t ny_weather_date(const char *text);
static int ny_weather_alerts(const cJSON *root);
static int ny_weather_worker(int argc, char **argv);

/****************************************************************************
 * Name: ny_weather_text
 ****************************************************************************/

static const char *ny_weather_text(const cJSON *row, const char *key)
{
  const char *value =
      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, key));
  return value ? value : "";
}

/****************************************************************************
 * Name: ny_weather_host
 ****************************************************************************/

static bool ny_weather_host(const char *host)
{
  const char *suffix = ".qweatherapi.com";
  size_t length = strlen(host);
  if (length <= strlen(suffix) || length > 128 ||
      strcmp(host + length - strlen(suffix), suffix))
    return false;
  bool label = false;
  for (const char *p = host; *p; p++)
    {
      if (*p == '.')
        {
          if (!label)
            return false;
          label = false;
        }
      else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
               *p == '-')
        label = true;
      else
        return false;
    }
  return label;
}

/****************************************************************************
 * Name: ny_weather_load
 ****************************************************************************/

static int ny_weather_load(void)
{
  if (g_weather_config)
    return 0;
  cJSON *config = NULL;
  uint64_t revision;
  int ret = ny_product_store_read("weather-config", &config, &revision);
  if (ret < 0)
    return ret;
  if (!config)
    {
      config = cJSON_CreateObject();
      if (!config || !cJSON_AddBoolToObject(config, "enabled", false) ||
          !cJSON_AddStringToObject(config, "host", "") ||
          !cJSON_AddStringToObject(config, "key", "") ||
          !cJSON_AddStringToObject(config, "city", "") ||
          !cJSON_AddStringToObject(config, "province", ""))
        {
          cJSON_Delete(config);
          return -ENOMEM;
        }
    }
  if (!cJSON_IsObject(config) ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(config, "enabled")) ||
      strlen(ny_weather_text(config, "city")) > 96 ||
      strlen(ny_weather_text(config, "province")) > 96 ||
      strlen(ny_weather_text(config, "key")) > 128 ||
      (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(config, "enabled")) &&
       (!ny_weather_host(ny_weather_text(config, "host")) ||
        !ny_weather_text(config, "key")[0] ||
        !ny_weather_text(config, "city")[0])))
    {
      cJSON_Delete(config);
      return -EBADMSG;
    }
  g_weather_config = config;
  g_weather_revision = revision;
  return 0;
}

/****************************************************************************
 * Name: ny_weather_status
 ****************************************************************************/

static cJSON *ny_weather_status(void)
{
  cJSON *root = cJSON_Duplicate(g_weather_config, true);
  if (!root)
    return NULL;
  cJSON_DeleteItemFromObjectCaseSensitive(root, "key");
  cJSON_DeleteItemFromObjectCaseSensitive(root, "fixture_port");
  cJSON *errors = cJSON_AddObjectToObject(root, "errors");
  bool ok =
      errors &&
      cJSON_AddBoolToObject(
          root, "key_set", ny_weather_text(g_weather_config, "key")[0] != 0) &&
      cJSON_AddNumberToObject(root, "revision", g_weather_revision) &&
      cJSON_AddBoolToObject(root, "refreshing", g_weather_running) &&
      cJSON_AddNumberToObject(root, "last_error", g_weather_error) &&
      cJSON_AddNumberToObject(root, "poll_minutes",
                              NY_WEATHER_POLL_MS / 60000) &&
      cJSON_AddBoolToObject(root, "clock_valid", ny_product_clock_valid());
  for (int i = 0; ok && i < NY_WEATHER_SECTION_COUNT; i++)
    ok = cJSON_AddNumberToObject(errors, g_weather_sections[i],
                                 g_weather_errors[i]) != NULL;
  if (!ok)
    {
      cJSON_Delete(root);
      return NULL;
    }
  return root;
}

/****************************************************************************
 * Name: ny_weather_fetch
 ****************************************************************************/

static int ny_weather_fetch(const cJSON *config, const char *path,
                            cJSON **result)
{
  *result = NULL;
  char *wire = malloc(NY_WEATHER_BODY_LIMIT);
  if (!wire)
    return -ENOMEM;
  vela_header_t headers[] = { { "X-QW-Api-Key",
                                ny_weather_text(config, "key") },
                              { "Accept-Encoding", "identity" },
                              { "Accept", "application/json" },
                              { NULL, NULL } };
  struct vela_http_response_s response;
  int ret;
#ifdef CONFIG_AI_AGENT_SIM_HTTP_FIXTURE
  const char *port = ny_weather_text(config, "fixture_port");
  if (port[0])
    ret = vela_http_loopback_get_once(port, path, headers, wire,
                                      NY_WEATHER_BODY_LIMIT, &response);
  else
#endif
    ret = vela_https_get_once(ny_weather_text(config, "host"), "443", path,
                              headers, wire, NY_WEATHER_BODY_LIMIT, &response);
  if (ret != 200)
    {
      ret = ret > 0 ? -ret : -EIO;
      goto out;
    }
  size_t length = response.body_length;
  const char *json = wire;
  ret = ny_product_json_check(json, length);
  if (ret < 0)
    goto out;
  *result = cJSON_Parse(json);
  if (!cJSON_IsObject(*result))
    {
      cJSON_Delete(*result);
      *result = NULL;
      ret = -EBADMSG;
    }
out:
  free(wire);
  return ret;
}

/****************************************************************************
 * Name: ny_weather_location
 ****************************************************************************/

static int ny_weather_location(const cJSON *config, cJSON **result)
{
  char path[1024];
  const char *parts[] = { ny_weather_text(config, "city"),
                          ny_weather_text(config, "province") };
  size_t used = snprintf(path, sizeof(path),
                         "/geo/v2/city/lookup?lang=zh&number=1&location=");
  for (int part = 0; part < 2; part++)
    {
      if (part)
        used += snprintf(path + used, sizeof(path) - used, "&adm=");
      for (const unsigned char *p = (const unsigned char *)parts[part]; *p;
           p++)
        {
          if (used + 4 >= sizeof(path))
            return -E2BIG;
          used += snprintf(path + used, sizeof(path) - used, "%%%02X", *p);
        }
    }
  cJSON *root = NULL;
  int ret = ny_weather_fetch(config, path, &root);
  if (ret < 0)
    return ret;
  cJSON *location = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(root, "location"), 0);
  if (strcmp(ny_weather_text(root, "code"), "200") ||
      !cJSON_IsObject(location))
    ret = -ENODATA;
  else
    {
      *result = cJSON_Duplicate(location, true);
      ret = *result ? 0 : -ENOMEM;
    }
  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_weather_date
 * Description: API requests use UTC; accept ISO minute or second precision.
 ****************************************************************************/

static uint64_t ny_weather_date(const char *text)
{
  struct tm value = { 0 };
  int year, month, day, hour, minute, second = 0, used = 0;
  if (sscanf(text, "%4d-%2d-%2dT%2d:%2d%n", &year, &month, &day, &hour,
             &minute, &used) != 5)
    return 0;
  if (text[used] == ':')
    {
      int count = 0;
      if (sscanf(text + used, ":%2d%n", &second, &count) != 1)
        return 0;
      used += count;
    }
  int offset = 0;
  if (strcmp(text + used, "Z"))
    {
      int hours, minutes, count = 0;
      char sign = text[used];
      if ((sign != '+' && sign != '-') ||
          sscanf(text + used + 1, "%2d:%2d%n", &hours, &minutes, &count) !=
              2 ||
          text[used + 1 + count] || hours > 14 || minutes > 59)
        return 0;
      offset = (hours * 60 + minutes) * (sign == '+' ? 1 : -1);
    }
  if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour > 23 || minute > 59 || second > 59)
    return 0;
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  return (uint64_t)(timegm(&value) - offset * 60) * 1000;
}

/****************************************************************************
 * Name: ny_weather_alerts
 ****************************************************************************/

static int ny_weather_alerts(const cJSON *root)
{
  const cJSON *alerts = cJSON_GetObjectItemCaseSensitive(root, "alerts");
  if (!cJSON_IsArray(alerts) || cJSON_GetArraySize(alerts) > 32)
    return -EBADMSG;
  const cJSON *row;
  int error = 0;
  cJSON_ArrayForEach(row, alerts)
  {
    const char *id = ny_weather_text(row, "id");
    const char *type = ny_weather_text(
        cJSON_GetObjectItemCaseSensitive(row, "messageType"), "code");
    uint64_t expires = ny_weather_date(ny_weather_text(row, "expireTime"));
    if (!id[0] || strlen(id) > 80 || !expires ||
        (strcmp(type, "alert") && strcmp(type, "update") &&
         strcmp(type, "cancel")))
      {
        error = -EBADMSG;
        continue;
      }
    if (expires <= ny_product_time_ms(false))
      continue;
    bool superseded = false;
    const cJSON *other;
    cJSON_ArrayForEach(other, alerts)
    {
      const cJSON *previous;
      cJSON_ArrayForEach(
          previous, cJSON_GetObjectItemCaseSensitive(
                        cJSON_GetObjectItemCaseSensitive(other, "messageType"),
                        "supersedes")) if (cJSON_IsString(previous) &&
                                           !strcmp(previous->valuestring, id))
          superseded = true;
    }
    if (superseded)
      continue;
    char key[97];
    snprintf(key, sizeof(key), "qweather:%s", id);
    int ret = ny_product_notification_post(
        key, "weather", ny_weather_text(row, "headline"), expires);
    if (ret < 0)
      error = ret;
  }
  return error;
}

/****************************************************************************
 * Name: ny_weather_worker
 ****************************************************************************/

static int ny_weather_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  nxmutex_lock(&g_weather_lock);
  cJSON *config = cJSON_Duplicate(g_weather_config, true);
  uint64_t generation = g_weather_job_generation;
  uint64_t config_revision = g_weather_revision;
  nxmutex_unlock(&g_weather_lock);
  cJSON *location = NULL;
  int error = config ? ny_weather_location(config, &location) : -ENOMEM;
  if (error < 0)
    goto done;
  char *end_lat;
  char *end_lon;
  double latitude = strtod(ny_weather_text(location, "lat"), &end_lat);
  double longitude = strtod(ny_weather_text(location, "lon"), &end_lon);
  if (*end_lat || *end_lon || !isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180)
    {
      error = -EBADMSG;
      goto done;
    }
  for (int i = 0; i < NY_WEATHER_SECTION_COUNT; i++)
    {
      nxmutex_lock(&g_weather_lock);
      bool stale = generation != g_weather_generation;
      nxmutex_unlock(&g_weather_lock);
      if (stale)
        break;
      char path[192];
      const char *endpoint = i == 0 ? "current" : i == 1 ? "daily" : "hourly";
      if (i == 3)
        snprintf(path, sizeof(path),
                 "/weatheralert/v1/current/%.2f/%.2f?lang=zh", latitude,
                 longitude);
      else
        snprintf(path, sizeof(path), "/weather/v1/%s/%.2f/%.2f?lang=zh%s",
                 endpoint, latitude, longitude,
                 i == 1   ? "&days=7"
                 : i == 2 ? "&hours=24"
                          : "");
      cJSON *data = NULL;
      int ret = ny_weather_fetch(config, path, &data);
      if (!ret)
        {
          if ((i == 0 && !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(
                             data, "temperature"))) ||
              (i == 1 && !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(
                             data, "days"))) ||
              (i == 2 && !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(
                             data, "hours"))) ||
              (i == 3 && !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(
                             data, "alerts"))))
            ret = -EBADMSG;
        }
      nxmutex_lock(&g_weather_lock);
      if (generation == g_weather_generation)
        {
          if (!ret)
            {
              cJSON *old = NULL;
              uint64_t revision;
              ret =
                  ny_product_store_read(g_weather_domains[i], &old, &revision);
              cJSON_Delete(old);
              cJSON *place = cJSON_Duplicate(location, true);
              if (!place || !cJSON_AddItemToObject(data, "location", place))
                {
                  cJSON_Delete(place);
                  ret = -ENOMEM;
                }
              if (!ret &&
                  (!cJSON_AddNumberToObject(data, "fetched_at",
                                            ny_product_time_ms(false)) ||
                   !cJSON_AddNumberToObject(data, "config_revision",
                                            config_revision)))
                ret = -ENOMEM;
              if (!ret)
                ret = ny_product_store_write(g_weather_domains[i], data,
                                             revision, &revision);
              if (!ret && i == 3)
                ret = ny_weather_alerts(data);
            }
          g_weather_errors[i] = ret;
          if (ret < 0)
            error = ret;
        }
      nxmutex_unlock(&g_weather_lock);
      cJSON_Delete(data);
    }
done:
  cJSON_Delete(location);
  cJSON_Delete(config);
  nxmutex_lock(&g_weather_lock);
  if (generation == g_weather_generation)
    g_weather_error = error;
  g_weather_running = false;
  nxmutex_unlock(&g_weather_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_product_weather_request
 ****************************************************************************/

int ny_product_weather_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result)
{
  if (strncmp(topic, "weather.", 8))
    return -ENOSYS;
  if (caller->role < NY_PRODUCT_FAMILY)
    return -EACCES;
  const char *op = topic + 8;
  if (strcmp(op, "status") && strcmp(op, "get") && strcmp(op, "configure") &&
      strcmp(op, "refresh"))
    return -ENOSYS;
  if ((!strcmp(op, "configure") || !strcmp(op, "refresh")) &&
      (caller->role != NY_PRODUCT_OWNER || !caller->local_transport))
    return -EACCES;
  int ret = nxmutex_lock(&g_weather_lock);
  if (ret < 0)
    return ret;
  ret = ny_weather_load();
  if (ret < 0)
    goto out;
  if (!strcmp(op, "get"))
    {
      int section;
      const char *selected = ny_weather_text(data, "section");
      if (!selected[0])
        selected = "now";
      for (section = 0; section < NY_WEATHER_SECTION_COUNT; section++)
        if (!strcmp(selected, g_weather_sections[section]))
          break;
      if (section == NY_WEATHER_SECTION_COUNT)
        {
          ret = -EINVAL;
          goto out;
        }
      uint64_t revision;
      ret =
          ny_product_store_read(g_weather_domains[section], result, &revision);
      if (ret < 0)
        goto out;
      if (*result && cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
                         *result, "config_revision")) != g_weather_revision)
        {
          cJSON_Delete(*result);
          *result = NULL;
        }
      if (!*result)
        *result = cJSON_CreateObject();
      if (!*result || !cJSON_AddNumberToObject(*result, "last_error",
                                               g_weather_errors[section]))
        ret = -ENOMEM;
      goto out;
    }
  if (!strcmp(op, "configure"))
    {
      double expected = cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(data, "revision"));
      if (!isfinite(expected) || expected < 0 || floor(expected) != expected)
        {
          ret = -EINVAL;
          goto out;
        }
      if (expected != g_weather_revision)
        {
          ret = -ESTALE;
          goto out;
        }
      const char *host = ny_weather_text(data, "host");
      const char *city = ny_weather_text(data, "city");
      const char *province = ny_weather_text(data, "province");
      const cJSON *provided_key =
          cJSON_GetObjectItemCaseSensitive(data, "key");
      const char *key = provided_key
                            ? cJSON_GetStringValue(provided_key)
                            : ny_weather_text(g_weather_config, "key");
      if (!ny_weather_host(host) || !city[0] || strlen(city) > 96 ||
          strlen(province) > 96 || !key || strlen(key) > 128 ||
          !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(data, "enabled")))
        {
          ret = -EINVAL;
          goto out;
        }
      for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        if (*p < 33 || *p > 126)
          {
            ret = -EINVAL;
            goto out;
          }
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "enabled")) &&
          !key[0])
        {
          ret = -EINVAL;
          goto out;
        }
      cJSON *candidate = cJSON_CreateObject();
      bool ok =
          candidate && cJSON_AddStringToObject(candidate, "host", host) &&
          cJSON_AddStringToObject(candidate, "key", key) &&
          cJSON_AddStringToObject(candidate, "city", city) &&
          cJSON_AddStringToObject(candidate, "province", province) &&
          cJSON_AddBoolToObject(
              candidate, "enabled",
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "enabled")));
#ifdef CONFIG_AI_AGENT_SIM_HTTP_FIXTURE
      const char *port = ny_weather_text(data, "fixture_port");
      if (port[0])
        {
          if (strlen(port) > 5 || strspn(port, "0123456789") != strlen(port) ||
              atoi(port) < 1 || atoi(port) > 65535)
            ok = false;
          else
            ok =
                ok && cJSON_AddStringToObject(candidate, "fixture_port", port);
        }
#endif
      uint64_t revision;
      ret = ok ? ny_product_store_write("weather-config", candidate,
                                        g_weather_revision, &revision)
               : -EINVAL;
      if (!ret)
        {
          cJSON_Delete(g_weather_config);
          g_weather_config = candidate;
          candidate = NULL;
          g_weather_revision = revision;
          g_weather_generation++;
          g_weather_next = 0;
          g_weather_error = 0;
          memset(g_weather_errors, 0, sizeof(g_weather_errors));
        }
      cJSON_Delete(candidate);
      if (ret < 0)
        goto out;
    }
  if (!strcmp(op, "refresh"))
    {
      if (g_weather_running)
        {
          ret = -EBUSY;
          goto out;
        }
      if (!cJSON_IsTrue(
              cJSON_GetObjectItemCaseSensitive(g_weather_config, "enabled")))
        {
          ret = -ENODATA;
          goto out;
        }
      g_weather_next = 0;
    }
  *result = ny_weather_status();
  if (!*result)
    ret = -ENOMEM;
out:
  nxmutex_unlock(&g_weather_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_weather_tick
 ****************************************************************************/

int ny_product_weather_tick(void)
{
  int ret = nxmutex_lock(&g_weather_lock);
  if (ret < 0)
    return ret;
  ret = ny_weather_load();
  if (!ret && !g_weather_running &&
      ny_product_clock_valid() &&
      ny_product_time_ms(true) >= g_weather_next &&
      cJSON_IsTrue(
          cJSON_GetObjectItemCaseSensitive(g_weather_config, "enabled")))
    {
      g_weather_running = true;
      g_weather_job_generation = g_weather_generation;
      g_weather_next = ny_product_time_ms(true) + NY_WEATHER_POLL_MS;
      ret = task_create("nyweather", 80, 32768, ny_weather_worker, NULL);
      if (ret < 0)
        {
          ret = -errno;
          g_weather_running = false;
          g_weather_error = ret;
        }
      else
        ret = 0;
    }
  nxmutex_unlock(&g_weather_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_weather_shutdown
 ****************************************************************************/

void ny_product_weather_shutdown(void)
{
  nxmutex_lock(&g_weather_lock);
  g_weather_generation++;
  g_weather_next = 0;
  nxmutex_unlock(&g_weather_lock);
}
