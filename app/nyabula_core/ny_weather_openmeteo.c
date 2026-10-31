/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_weather_openmeteo.c
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

#include "ny_weather_openmeteo.h"
#include "infra/vela_tls.h"
#include "ny_product_store.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NY_OPENMETEO_BODY_LIMIT 24576

static const char *ny_openmeteo_string(const cJSON *root, const char *key);
static int ny_openmeteo_get(const cJSON *config, const char *host,
                            const char *path, cJSON **result);
static int ny_openmeteo_location(const cJSON *config, cJSON **result);
static double ny_openmeteo_number(const cJSON *root, const char *key,
                                  int index);
static bool ny_openmeteo_quantity(cJSON *root, const char *key, double value,
                                  const char *unit);
static cJSON *ny_openmeteo_condition(double code);
static bool ny_openmeteo_time(cJSON *root, const char *key, double stamp);
static cJSON *ny_openmeteo_section(void);
static int ny_openmeteo_normalize(const cJSON *response, cJSON **sections);

/****************************************************************************
 * Name: ny_openmeteo_string
 ****************************************************************************/

static const char *ny_openmeteo_string(const cJSON *root, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

/****************************************************************************
 * Name: ny_openmeteo_get
 ****************************************************************************/

static int ny_openmeteo_get(const cJSON *config, const char *host,
                            const char *path, cJSON **result)
{
  char *body = calloc(1, NY_OPENMETEO_BODY_LIMIT);
  if (!body)
    return -ENOMEM;
  vela_header_t headers[] = { { "Accept", "application/json" },
                              { "Accept-Encoding", "identity" },
                              { NULL, NULL } };
  struct vela_http_response_s response;
  int ret;
#ifdef CONFIG_AI_AGENT_SIM_HTTP_FIXTURE
  const char *port = ny_openmeteo_string(config, "fixture_port");
  if (port[0])
    ret = vela_http_loopback_get_once(port, path, headers, body,
                                      NY_OPENMETEO_BODY_LIMIT, &response);
  else
#endif
    {
      (void)config;
      ret = vela_https_get_once(host, "443", path, headers, body,
                                NY_OPENMETEO_BODY_LIMIT, &response);
    }
  if (ret == 200)
    {
      ret = ny_product_json_check(body, response.body_length);
      if (ret == 0)
        {
          *result = cJSON_Parse(body);
          if (!cJSON_IsObject(*result) ||
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(*result, "error")))
            {
              cJSON_Delete(*result);
              *result = NULL;
              ret = -EBADMSG;
            }
        }
    }
  else
    ret = ret > 0 ? -ret : -EIO;
  free(body);
  return ret;
}

/****************************************************************************
 * Name: ny_openmeteo_location
 ****************************************************************************/

static int ny_openmeteo_location(const cJSON *config, cJSON **result)
{
  char path[1024];
  const char *city = ny_openmeteo_string(config, "city");
  const char *province = ny_openmeteo_string(config, "province");
  if (!city[0] || strlen(city) > 96 || strlen(province) > 96)
    return -EINVAL;
  size_t used = snprintf(path, sizeof(path),
                         "/v1/search?count=1&language=zh&format=json&name=");
  const char *parts[] = { city, province };
  for (size_t i = 0; i < 2; i++)
    {
      if (i && !province[0])
        break;
      if (i)
        used += snprintf(path + used, sizeof(path) - used, "%%2C");
      for (const unsigned char *p = (const unsigned char *)parts[i]; *p; p++)
        {
          if (used + 4 >= sizeof(path))
            return -E2BIG;
          used += snprintf(path + used, sizeof(path) - used, "%%%02X", *p);
        }
    }
  cJSON *response = NULL;
  int ret = ny_openmeteo_get(config, "geocoding-api.open-meteo.com", path,
                             &response);
  if (ret < 0)
    return ret;
  const cJSON *place = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(response, "results"), 0);
  double latitude = ny_openmeteo_number(place, "latitude", -1);
  double longitude = ny_openmeteo_number(place, "longitude", -1);
  if (!cJSON_IsObject(place))
    ret = -ENOENT;
  else if (!isfinite(latitude) || latitude < -90 || latitude > 90 ||
           !isfinite(longitude) || longitude < -180 || longitude > 180 ||
           !ny_openmeteo_string(place, "name")[0])
    ret = -EBADMSG;
  else
    {
      char lat[32], lon[32];
      snprintf(lat, sizeof(lat), "%.5f", latitude);
      snprintf(lon, sizeof(lon), "%.5f", longitude);
      *result = cJSON_CreateObject();
      bool valid =
          *result &&
          cJSON_AddStringToObject(*result, "name",
                                  ny_openmeteo_string(place, "name")) &&
          cJSON_AddStringToObject(*result, "lat", lat) &&
          cJSON_AddStringToObject(*result, "lon", lon) &&
          cJSON_AddStringToObject(*result, "tz",
                                  ny_openmeteo_string(place, "timezone")) &&
          cJSON_AddStringToObject(*result, "country",
                                  ny_openmeteo_string(place, "country")) &&
          cJSON_AddStringToObject(*result, "adm1",
                                  ny_openmeteo_string(place, "admin1"));
      ret = valid ? 0 : -ENOMEM;
    }
  cJSON_Delete(response);
  return ret;
}

/****************************************************************************
 * Name: ny_openmeteo_number
 ****************************************************************************/

static double ny_openmeteo_number(const cJSON *root, const char *key,
                                  int index)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
  if (index >= 0)
    value = cJSON_GetArrayItem(value, index);
  return cJSON_IsNumber(value) ? value->valuedouble : NAN;
}

/****************************************************************************
 * Name: ny_openmeteo_quantity
 ****************************************************************************/

static bool ny_openmeteo_quantity(cJSON *root, const char *key, double value,
                                  const char *unit)
{
  if (!isfinite(value))
    return false;
  cJSON *quantity = cJSON_AddObjectToObject(root, key);
  return quantity && cJSON_AddNumberToObject(quantity, "value", value) &&
         cJSON_AddStringToObject(quantity, "unit", unit);
}

/****************************************************************************
 * Name: ny_openmeteo_condition
 ****************************************************************************/

static cJSON *ny_openmeteo_condition(double code)
{
  if (!isfinite(code) || code < 0 || code > 99 || floor(code) != code)
    return NULL;
  int value = (int)code;
  const char *text = value == 0                   ? "晴"
                     : value == 1                 ? "大部晴朗"
                     : value == 2                 ? "多云"
                     : value == 3                 ? "阴"
                     : value == 45 || value == 48 ? "雾"
                     : value == 51 || value == 53 || value == 55 ? "毛毛雨"
                     : value == 56 || value == 57 || value == 66 || value == 67
                         ? "冻雨"
                     : value == 61                               ? "小雨"
                     : value == 63                               ? "中雨"
                     : value == 65                               ? "大雨"
                     : value == 71                               ? "小雪"
                     : value == 73                               ? "中雪"
                     : value == 75                               ? "大雪"
                     : value == 77                               ? "雪粒"
                     : value == 80 || value == 81 || value == 82 ? "阵雨"
                     : value == 85 || value == 86                ? "阵雪"
                     : value == 95 || value == 97                ? "雷暴"
                     : value == 96 || value == 99 ? "雷暴伴冰雹"
                                                  : "未知天气";
  char encoded[16];
  snprintf(encoded, sizeof(encoded), "wmo-%d", value);
  cJSON *condition = cJSON_CreateObject();
  if (!condition || !cJSON_AddStringToObject(condition, "code", encoded) ||
      !cJSON_AddStringToObject(condition, "text", text))
    {
      cJSON_Delete(condition);
      return NULL;
    }
  return condition;
}

/****************************************************************************
 * Name: ny_openmeteo_time
 ****************************************************************************/

static bool ny_openmeteo_time(cJSON *root, const char *key, double stamp)
{
  if (!isfinite(stamp) || stamp < 1577836800 || stamp > 4133980800)
    return false;
  time_t seconds = (time_t)stamp;
  struct tm utc;
  char text[32];
  gmtime_r(&seconds, &utc);
  if (!strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc))
    return false;
  return cJSON_AddStringToObject(root, key, text) != NULL;
}

/****************************************************************************
 * Name: ny_openmeteo_section
 ****************************************************************************/

static cJSON *ny_openmeteo_section(void)
{
  cJSON *section = cJSON_CreateObject();
  cJSON *metadata = cJSON_AddObjectToObject(section, "metadata");
  cJSON *attributions = cJSON_AddArrayToObject(metadata, "attributions");
  if (!section || !metadata || !attributions ||
      !cJSON_AddItemToArray(attributions,
                            cJSON_CreateString("https://open-meteo.com/")) ||
      !cJSON_AddItemToArray(
          attributions,
          cJSON_CreateString("Weather data: Open-Meteo (CC BY 4.0)")) ||
      !cJSON_AddStringToObject(section, "provider", "open-meteo") ||
      !cJSON_AddStringToObject(section, "dataKind", "modelled"))
    {
      cJSON_Delete(section);
      return NULL;
    }
  return section;
}

/****************************************************************************
 * Name: ny_openmeteo_normalize
 ****************************************************************************/

static int ny_openmeteo_normalize(const cJSON *response, cJSON **sections)
{
  *sections = cJSON_CreateArray();
  if (!*sections)
    return -ENOMEM;
  for (size_t i = 0; i < 4; i++)
    {
      cJSON *section = ny_openmeteo_section();
      if (!section || !cJSON_AddItemToArray(*sections, section))
        {
          cJSON_Delete(section);
          return -ENOMEM;
        }
    }
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(response, "current");
  cJSON *now = cJSON_GetArrayItem(*sections, 0);
  cJSON *condition =
      ny_openmeteo_condition(ny_openmeteo_number(current, "weather_code", -1));
  cJSON *wind = cJSON_AddObjectToObject(now, "wind");
  cJSON *direction = cJSON_AddObjectToObject(wind, "direction");
  double humidity = ny_openmeteo_number(current, "relative_humidity_2m", -1);
  double degrees = ny_openmeteo_number(current, "wind_direction_10m", -1);
  static const char *const compass[] = { "N", "NE", "E", "SE",
                                         "S", "SW", "W", "NW" };
  bool valid =
      isfinite(humidity) && humidity >= 0 && humidity <= 100 &&
      isfinite(degrees) && degrees >= 0 && degrees <= 360 &&
      ny_openmeteo_quantity(now, "temperature",
                            ny_openmeteo_number(current, "temperature_2m", -1),
                            "°C") &&
      ny_openmeteo_quantity(
          now, "feelsLike",
          ny_openmeteo_number(current, "apparent_temperature", -1), "°C") &&
      ny_openmeteo_quantity(
          now, "pressure",
          ny_openmeteo_number(current, "surface_pressure", -1), "hPa") &&
      cJSON_AddNumberToObject(now, "humidity", humidity) &&
      ny_openmeteo_quantity(wind, "speed",
                            ny_openmeteo_number(current, "wind_speed_10m", -1),
                            "km/h") &&
      direction &&
      cJSON_AddStringToObject(direction, "compass",
                              compass[((int)(degrees + 22.5) / 45) % 8]) &&
      ny_openmeteo_time(now, "forecastTime",
                        ny_openmeteo_number(current, "time", -1));
  if (!condition || !cJSON_AddItemToObject(now, "condition", condition))
    {
      cJSON_Delete(condition);
      valid = false;
    }
  if (!valid)
    return -EBADMSG;
  for (int section = 1; section <= 2; section++)
    {
      bool daily = section == 1;
      const cJSON *input = cJSON_GetObjectItemCaseSensitive(
          response, daily ? "daily" : "hourly");
      const cJSON *times = cJSON_GetObjectItemCaseSensitive(input, "time");
      cJSON *items = cJSON_AddArrayToObject(
          cJSON_GetArrayItem(*sections, section), daily ? "days" : "hours");
      int count = cJSON_GetArraySize(times);
      if (!items || count < 1 || count > (daily ? 7 : 24))
        return -EBADMSG;
      for (int i = 0; i < count; i++)
        {
          cJSON *item = cJSON_CreateObject();
          if (!item || !cJSON_AddItemToArray(items, item))
            {
              cJSON_Delete(item);
              return -ENOMEM;
            }
          valid = ny_openmeteo_time(
              item, daily ? "forecastStartTime" : "forecastTime",
              ny_openmeteo_number(input, "time", i));
          condition = ny_openmeteo_condition(
              ny_openmeteo_number(input, "weather_code", i));
          cJSON *target =
              daily ? cJSON_AddObjectToObject(item, "daytime") : item;
          if (!condition || !target ||
              !cJSON_AddItemToObject(target, "condition", condition))
            {
              cJSON_Delete(condition);
              valid = false;
            }
          if (daily)
            {
              cJSON *astro = cJSON_AddObjectToObject(item, "astro");
              valid &= ny_openmeteo_quantity(
                  item, "temperatureMax",
                  ny_openmeteo_number(input, "temperature_2m_max", i), "°C");
              valid &= ny_openmeteo_quantity(
                  item, "temperatureMin",
                  ny_openmeteo_number(input, "temperature_2m_min", i), "°C");
              if (astro)
                {
                  /* Polar day/night may have no sunrise or sunset. */
                  double sunrise = ny_openmeteo_number(input, "sunrise", i);
                  double sunset = ny_openmeteo_number(input, "sunset", i);
                  if (isfinite(sunrise) && sunrise > 0)
                    valid &= ny_openmeteo_time(astro, "sunrise", sunrise);
                  if (isfinite(sunset) && sunset > 0)
                    valid &= ny_openmeteo_time(astro, "sunset", sunset);
                }
            }
          else
            valid &= ny_openmeteo_quantity(
                item, "temperature",
                ny_openmeteo_number(input, "temperature_2m", i), "°C");
          if (!valid)
            return -EBADMSG;
        }
    }
  cJSON *alerts = cJSON_GetArrayItem(*sections, 3);
  if (!cJSON_AddBoolToObject(alerts, "supported", false) ||
      !cJSON_AddArrayToObject(alerts, "alerts"))
    return -ENOMEM;
  return 0;
}

/****************************************************************************
 * Name: ny_weather_openmeteo
 ****************************************************************************/

int ny_weather_openmeteo(const cJSON *config, cJSON **location,
                         cJSON **sections)
{
  *location = NULL;
  *sections = NULL;
  int ret = ny_openmeteo_location(config, location);
  cJSON *response = NULL;
  if (ret == 0)
    {
      char path[1024];
      snprintf(
          path, sizeof(path),
          "/v1/forecast?latitude=%s&longitude=%s"
          "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
          "weather_code,wind_speed_10m,wind_direction_10m,surface_pressure"
          "&hourly=temperature_2m,weather_code"
          "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,"
          "sunset"
          "&forecast_days=7&forecast_hours=24&timezone=auto&timeformat="
          "unixtime",
          ny_openmeteo_string(*location, "lat"),
          ny_openmeteo_string(*location, "lon"));
      ret = ny_openmeteo_get(config, "api.open-meteo.com", path, &response);
    }
  if (ret == 0)
    ret = ny_openmeteo_normalize(response, sections);
  cJSON_Delete(response);
  if (ret < 0)
    {
      cJSON_Delete(*location);
      cJSON_Delete(*sections);
      *location = NULL;
      *sections = NULL;
    }
  return ret;
}
