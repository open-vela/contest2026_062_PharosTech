/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product.h
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

#ifndef __NYABULA_CORE_NY_PRODUCT_H
#define __NYABULA_CORE_NY_PRODUCT_H
#include <netutils/cJSON.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ny_product_role_e
{
  NY_PRODUCT_GUEST = 0,
  NY_PRODUCT_FAMILY,
  NY_PRODUCT_OWNER
};

struct ny_product_caller_s
{
  const char *id;
  enum ny_product_role_e role;
  bool local_transport;
};

/* Results are owned by the caller. Inputs are borrowed. */
int ny_product_records_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result);
int ny_product_cli(const char *topic, const char *path);
uint64_t ny_product_time_ms(bool monotonic);
int ny_product_request(const struct ny_product_caller_s *caller,
                       const char *topic, const cJSON *data, cJSON **result);
int ny_product_timers_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result);
int ny_product_timers_tick(void);
int ny_product_alarms_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result);
int ny_product_alarms_tick(void);
int ny_product_notification_post(const char *id, const char *source,
                                 const char *title, uint64_t expires);
int ny_product_notifications_request(const struct ny_product_caller_s *caller,
                                     const char *topic, const cJSON *data,
                                     cJSON **result);
int ny_product_notifications_tick(void);
int ny_product_briefing_request(const struct ny_product_caller_s *caller,
                                const char *topic, const cJSON *data,
                                cJSON **result);
int ny_product_briefing_tick(void);
#ifdef CONFIG_NYABULA_CORE_AGENT
int ny_product_companion_request(const struct ny_product_caller_s *caller,
                                 const char *topic, const cJSON *data,
                                 cJSON **result);
int ny_product_companion_tick(void);
#endif
#ifdef CONFIG_NYABULA_CORE_WEATHER
int ny_product_weather_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result);
int ny_product_weather_tick(void);
void ny_product_weather_shutdown(void);
#endif
int ny_product_media_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result);
int ny_product_media_tick(void);
void ny_product_media_shutdown(void);

/* Audible alerts raised by other product services.  They only record what
 * is wanted; the media tick owns the player and acts on it, so callers may
 * hold their own locks.
 */

#define NY_PRODUCT_MEDIA_ALERT_OFF  0 /* Silence an alert that is sounding */
#define NY_PRODUCT_MEDIA_ALERT_ONCE 1 /* One chime: a countdown ran out */
#define NY_PRODUCT_MEDIA_ALERT_LOOP       \
  2 /* Chime until switched off: an alarm \
     */

void ny_product_media_alert(int mode);
void ny_product_media_sleep(void);
int ny_product_device_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result);
int ny_product_maintenance_request(const struct ny_product_caller_s *caller,
                                   const char *topic, const cJSON *data,
                                   cJSON **result);
#ifdef CONFIG_NYABULA_CORE_NETWORK
int ny_product_network_request(const struct ny_product_caller_s *caller,
                               const char *topic, const cJSON *data,
                               cJSON **result);
int ny_product_network_tick(void);
int ny_product_network_link(char *ssid, size_t size, int *rssi);
int ny_product_network_name(char *out, size_t size);
void ny_product_network_shutdown(void);
#endif
#ifdef CONFIG_NYABULA_CORE_AUDIO
/* The store-free level accessors are in ny_product_audio.h. */
int ny_product_audio_request(const struct ny_product_caller_s *caller,
                             const char *topic, const cJSON *data,
                             cJSON **result);
int ny_product_audio_tick(void);
#endif
int ny_product_eyes_request(const struct ny_product_caller_s *caller,
                            const char *topic, const cJSON *data,
                            cJSON **result);
int ny_product_start(void);
int ny_product_stop(void);
cJSON *ny_product_runtime_status(void);

#endif
