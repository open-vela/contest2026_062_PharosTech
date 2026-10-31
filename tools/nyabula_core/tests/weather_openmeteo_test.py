#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the real public weather provider with deterministic HTTPS fixtures."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
parser.add_argument('agent', type=Path)
parser.add_argument('cjson', type=Path)
args = parser.parse_args()
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ny_weather_openmeteo.c"
static char *geo, *forecast;
static int mode;
int ny_product_json_check(const char *text, size_t size)
{
  return text && size && size < 24576 ? 0 : -EINVAL;
}
int vela_https_get_once(const char *host, const char *port, const char *path,
    const vela_header_t *headers, char *body, size_t capacity,
    struct vela_http_response_s *response)
{
  assert(!strcmp(port, "443"));
  for (const vela_header_t *h = headers; h->name; h++)
    assert(!strcmp(h->name, "Accept") || !strcmp(h->name, "Accept-Encoding"));
  bool location = !strcmp(host, "geocoding-api.open-meteo.com");
  assert(location || !strcmp(host, "api.open-meteo.com"));
  if (location) assert(strstr(path, "%E9%A6%99%E6%B8%AF"));
  else assert(strstr(path, "timeformat=unixtime") && strstr(path, "forecast_hours=24"));
  cJSON *data = cJSON_Parse(location ? geo : forecast);
  assert(data);
  if (location && mode == 2) {
    cJSON *place = cJSON_GetArrayItem(cJSON_GetObjectItem(data, "results"), 0);
    cJSON_ReplaceItemInObject(place, "latitude", cJSON_CreateNull());
  }
  if (location && mode == 3)
    cJSON_ReplaceItemInObject(data, "results", cJSON_CreateArray());
  if (!location && mode == 1)
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(data, "current"),
                              "temperature_2m", cJSON_CreateNull());
  if (!location && mode == 4)
    cJSON_DeleteItemFromArray(cJSON_GetObjectItem(cJSON_GetObjectItem(data,
                                "hourly"), "temperature_2m"), 23);
  assert(cJSON_PrintPreallocated(data, body, capacity, false));
  response->body_length = strlen(body);
  cJSON_Delete(data);
  return mode == 5 ? 503 : 200;
}
static char *read_fixture(const char *path)
{
  FILE *file = fopen(path, "rb"); assert(file);
  char *text = calloc(1, 24000); assert(text);
  assert(fread(text, 1, 23999, file) > 0);
  fclose(file); return text;
}
int main(int argc, char **argv)
{
  assert(argc == 3);
  geo = read_fixture(argv[1]); forecast = read_fixture(argv[2]);
  cJSON *config = cJSON_Parse("{\"city\":\"香港\",\"key\":\"must-never-send\"}");
  cJSON *location = NULL, *sections = NULL;
  assert(ny_weather_openmeteo(config, &location, &sections) == 0);
  assert(!strcmp(ny_openmeteo_string(location, "name"), "Hong Kong fixture"));
  cJSON *now = cJSON_GetArrayItem(sections, 0);
  assert(ny_openmeteo_number(cJSON_GetObjectItem(now, "temperature"), "value", -1) == -3.5);
  assert(!strcmp(ny_openmeteo_string(cJSON_GetObjectItem(now, "condition"), "code"), "wmo-95"));
  assert(!strcmp(ny_openmeteo_string(now, "forecastTime"), "2026-10-22T18:00:00Z"));
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetArrayItem(sections, 1), "days")) == 7);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetArrayItem(sections, 2), "hours")) == 24);
  assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(sections, 3), "supported")));
  cJSON_Delete(location); cJSON_Delete(sections);
  for (mode = 1; mode <= 5; mode++) {
    int ret = ny_weather_openmeteo(config, &location, &sections);
    assert(ret == (mode == 3 ? -ENOENT : mode == 5 ? -503 : -EBADMSG));
    assert(!location && !sections);
  }
  cJSON_Delete(config); free(geo); free(forecast);
  puts("OPENMETEO_UNITS_TIME_FORECAST_MISSING_DATA_NO_CREDENTIALS_PASS");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ny-openmeteo-') as directory:
    root = Path(directory)
    (root / 'netutils').mkdir()
    (root / 'netutils/cJSON.h').write_text('#include <cJSON.h>\n')
    geo = {'results': [{'name': 'Hong Kong fixture', 'latitude': 22.3,
                       'longitude': 114.2, 'timezone': 'Asia/Hong_Kong'}]}
    forecast = {
        'current': {'time': 1792692000, 'temperature_2m': -3.5,
                    'relative_humidity_2m': 85, 'apparent_temperature': -8,
                    'weather_code': 95, 'wind_speed_10m': 12,
                    'wind_direction_10m': 270, 'surface_pressure': 1005},
        'daily': {'time': [1792692000 + i * 86400 for i in range(7)],
                  'weather_code': [2] * 7, 'temperature_2m_max': [5] * 7,
                  'temperature_2m_min': [-3] * 7, 'sunrise': [None] * 7,
                  'sunset': [None] * 7},
        'hourly': {'time': [1792692000 + i * 3600 for i in range(24)],
                   'weather_code': [61] * 24, 'temperature_2m': [1] * 24},
    }
    (root / 'geo.json').write_text(json.dumps(geo))
    (root / 'forecast.json').write_text(json.dumps(forecast))
    (root / 'test.c').write_text(code, encoding='utf-8')
    for name, flags in [('plain', ['-O2']), ('sanitized',
            ['-g', '-O1', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all'])]:
        binary = root / name
        subprocess.run(['cc', '-D_DEFAULT_SOURCE', '-std=c11', '-Wall',
                        '-Wextra', '-Werror', *flags, '-I' + str(root),
                        '-I' + str(args.core), '-I' + str(args.agent / 'src'),
                        '-I' + str(args.cjson), str(root / 'test.c'),
                        str(args.cjson / 'cJSON.c'), '-lm', '-o', str(binary)],
                       check=True)
        subprocess.run([str(binary), str(root / 'geo.json'),
                        str(root / 'forecast.json')], check=True)
