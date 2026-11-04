/****************************************************************************
 * app/nyabula_core/ny_product_camera_gaze.c
 *
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


#include "ny_product_camera_gaze.h"
#include "ny_camera_gaze.h"
#include "ny_product_store.h"
#include <errno.h>
#include <math.h>
#include <string.h>

#define NY_CAMERA_GAZE_DOMAIN "camera-gaze"
#define NY_CAMERA_GAZE_WIDTH  640
#define NY_CAMERA_GAZE_HEIGHT 480
#define NY_CAMERA_GAZE_MODE   6
#define NY_CAMERA_GAZE_MAX_RMS 0.20
#define NY_CAMERA_GAZE_MAX_ERROR 0.40
#define NY_CAMERA_GAZE_POINT_HOLD_MS 1000
#define NY_CAMERA_GAZE_MANUAL_MAX_MS 5000

static int ny_camera_gaze_number(const cJSON *data, const char *key,
                                  double low, double high, double *value);
static int ny_camera_gaze_parse(const cJSON *data,
                                struct ny_camera_gaze_s *mapping);
static cJSON *ny_camera_gaze_result(const cJSON *record, uint64_t revision,
                                    const struct ny_camera_gaze_s *mapping);
static int ny_camera_gaze_apply(const struct ny_product_caller_s *caller,
                                 double x, double y, uint32_t hold_ms,
                                 cJSON **result);

/****************************************************************************
 * Name: ny_camera_gaze_number
 ****************************************************************************/

static int ny_camera_gaze_number(const cJSON *data, const char *key,
                                  double low, double high, double *value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
      item->valuedouble < low || item->valuedouble > high)
    return -EINVAL;
  *value = item->valuedouble;
  return 0;
}

/****************************************************************************
 * Name: ny_camera_gaze_parse
 ****************************************************************************/

static int ny_camera_gaze_parse(const cJSON *data,
                                struct ny_camera_gaze_s *mapping)
{
  struct ny_camera_gaze_point_s points[NY_CAMERA_GAZE_POINTS];
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "points");
  double value;
  if (ny_camera_gaze_number(data, "width", NY_CAMERA_GAZE_WIDTH,
                            NY_CAMERA_GAZE_WIDTH, &value) < 0 ||
      ny_camera_gaze_number(data, "height", NY_CAMERA_GAZE_HEIGHT,
                            NY_CAMERA_GAZE_HEIGHT, &value) < 0 ||
      ny_camera_gaze_number(data, "mode", NY_CAMERA_GAZE_MODE,
                            NY_CAMERA_GAZE_MODE, &value) < 0 ||
      !cJSON_IsArray(items) || cJSON_GetArraySize(items) != NY_CAMERA_GAZE_POINTS)
    return -EINVAL;
  for (int i = 0; i < NY_CAMERA_GAZE_POINTS; i++)
    {
      const cJSON *point = cJSON_GetArrayItem(items, i);
      if (ny_camera_gaze_number(point, "u", 0, 1, &points[i].u) < 0 ||
          ny_camera_gaze_number(point, "v", 0, 1, &points[i].v) < 0 ||
          ny_camera_gaze_number(point, "x", -1, 1, &points[i].x) < 0 ||
          ny_camera_gaze_number(point, "y", -1, 1, &points[i].y) < 0)
        return -EINVAL;
    }
  return ny_camera_gaze_fit(points, mapping);
}

/****************************************************************************
 * Name: ny_camera_gaze_result
 ****************************************************************************/

static cJSON *ny_camera_gaze_result(const cJSON *record, uint64_t revision,
                                    const struct ny_camera_gaze_s *mapping)
{
  cJSON *result = record == NULL ? cJSON_CreateObject() :
                                  cJSON_Duplicate(record, true);
  if (result == NULL)
    return NULL;
  bool ok = cJSON_AddNumberToObject(result, "revision", revision) != NULL &&
            cJSON_AddBoolToObject(result, "calibrated", mapping != NULL) != NULL;
  if (mapping != NULL)
    {
      ok &= cJSON_AddNumberToObject(result, "rmsError", mapping->rms) != NULL;
      ok &= cJSON_AddNumberToObject(result, "maxError", mapping->maximum) != NULL;
      ok &= cJSON_AddBoolToObject(result, "acceptable",
              mapping->rms <= NY_CAMERA_GAZE_MAX_RMS &&
              mapping->maximum <= NY_CAMERA_GAZE_MAX_ERROR) != NULL;
    }
  if (!ok)
    {
      cJSON_Delete(result);
      return NULL;
    }
  return result;
}

/****************************************************************************
 * Name: ny_camera_gaze_apply
 ****************************************************************************/

static int ny_camera_gaze_apply(const struct ny_product_caller_s *caller,
                                 double x, double y, uint32_t hold_ms,
                                 cJSON **result)
{
  cJSON *params = cJSON_CreateObject();
  if (params == NULL || cJSON_AddNumberToObject(params, "x", x) == NULL ||
      cJSON_AddNumberToObject(params, "y", y) == NULL ||
      cJSON_AddNumberToObject(params, "hold_ms", hold_ms) == NULL)
    {
      cJSON_Delete(params);
      return -ENOMEM;
    }
  int ret = ny_product_eyes_request(caller, "eyes.gaze", params, result);
  cJSON_Delete(params);
  return ret;
}

/****************************************************************************
 * Name: ny_product_camera_gaze_request
 ****************************************************************************/

int ny_product_camera_gaze_request(const struct ny_product_caller_s *caller,
                                    const char *topic, const cJSON *data,
                                    cJSON **result)
{
  bool get = !strcmp(topic, "camera.calibration.get");
  bool fit = !strcmp(topic, "camera.calibration.fit");
  bool set = !strcmp(topic, "camera.calibration.set");
  bool clear = !strcmp(topic, "camera.calibration.clear");
  bool point = !strcmp(topic, "camera.point");
  bool aim = !strcmp(topic, "camera.aim");
  if (!get && !fit && !set && !clear && !point && !aim)
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  if (aim)
    {
      double x;
      double y;
      double hold = NY_CAMERA_GAZE_POINT_HOLD_MS;
      if (ny_camera_gaze_number(data, "x", -1, 1, &x) < 0 ||
          ny_camera_gaze_number(data, "y", -1, 1, &y) < 0)
        return -EINVAL;
      if (cJSON_GetObjectItemCaseSensitive(data, "hold_ms") != NULL &&
          (ny_camera_gaze_number(data, "hold_ms", 0,
                                  NY_CAMERA_GAZE_MANUAL_MAX_MS, &hold) < 0 ||
           floor(hold) != hold))
        return -EINVAL;
      return ny_camera_gaze_apply(caller, x, y, hold, result);
    }

  cJSON *record = NULL;
  uint64_t revision = 0;
  struct ny_camera_gaze_s mapping;
  int ret = ny_product_store_read(NY_CAMERA_GAZE_DOMAIN, &record, &revision);
  if (ret == -ENOENT)
    ret = 0;
  if (ret < 0)
    goto out;

  if (set || fit)
    {
      ret = ny_camera_gaze_parse(data, &mapping);
      if (ret < 0)
        goto out;
      cJSON_Delete(record);
      record = cJSON_CreateObject();
      if (record == NULL ||
          cJSON_AddNumberToObject(record, "width", NY_CAMERA_GAZE_WIDTH) == NULL ||
          cJSON_AddNumberToObject(record, "height", NY_CAMERA_GAZE_HEIGHT) == NULL ||
          cJSON_AddNumberToObject(record, "mode", NY_CAMERA_GAZE_MODE) == NULL)
        {
          ret = -ENOMEM;
          goto out;
        }
      cJSON *points = cJSON_Duplicate(
          cJSON_GetObjectItemCaseSensitive(data, "points"), true);
      if (points == NULL || !cJSON_AddItemToObject(record, "points", points))
        {
          cJSON_Delete(points);
          ret = -ENOMEM;
          goto out;
        }
      if (set && (mapping.rms > NY_CAMERA_GAZE_MAX_RMS ||
                  mapping.maximum > NY_CAMERA_GAZE_MAX_ERROR))
        {
          ret = -ERANGE;
          goto out;
        }
    }
  if (set || clear)
    {
      double expected;
      ret = ny_camera_gaze_number(data, "revision", 0, 9007199254740991.0,
                                   &expected);
      if (ret < 0 || floor(expected) != expected)
        {
          ret = -EINVAL;
          goto out;
        }
      if (clear)
        {
          cJSON_Delete(record);
          record = cJSON_CreateObject();
          if (record == NULL)
            {
              ret = -ENOMEM;
              goto out;
            }
        }
      ret = ny_product_store_write(NY_CAMERA_GAZE_DOMAIN, record, expected,
                                    &revision);
      if (ret < 0)
        goto out;
    }

  bool calibrated = record != NULL &&
                    ny_camera_gaze_parse(record, &mapping) == 0;
  if (point)
    {
      double u;
      double v;
      double x;
      double y;
      double dimension;
      if (!calibrated)
        {
          ret = -ENODATA;
          goto out;
        }
      if (ny_camera_gaze_number(data, "u", 0, 1, &u) < 0 ||
          ny_camera_gaze_number(data, "v", 0, 1, &v) < 0 ||
          ny_camera_gaze_number(data, "width", NY_CAMERA_GAZE_WIDTH,
                                NY_CAMERA_GAZE_WIDTH, &dimension) < 0 ||
          ny_camera_gaze_number(data, "height", NY_CAMERA_GAZE_HEIGHT,
                                NY_CAMERA_GAZE_HEIGHT, &dimension) < 0 ||
          ny_camera_gaze_number(data, "mode", NY_CAMERA_GAZE_MODE,
                                NY_CAMERA_GAZE_MODE, &dimension) < 0)
        {
          ret = -EINVAL;
          goto out;
        }
      ret = ny_camera_gaze_map(&mapping, u, v, &x, &y);
      if (ret < 0)
        goto out;
      ret = ny_camera_gaze_apply(caller, x, y,
                                 NY_CAMERA_GAZE_POINT_HOLD_MS, result);
      goto out;
    }
  *result = ny_camera_gaze_result(record, revision,
                                  calibrated ? &mapping : NULL);
  ret = *result == NULL ? -ENOMEM : 0;
out:
  cJSON_Delete(record);
  return ret;
}
