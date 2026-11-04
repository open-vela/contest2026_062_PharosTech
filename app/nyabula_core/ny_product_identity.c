/****************************************************************************
 * app/nyabula_core/ny_product_identity.c
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

#include "ny_product_identity.h"
#include "ny_product_privacy.h"
#include "ny_product_store.h"
#include "ny_product_vision.h"
#include <errno.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <stdio.h>
#include <string.h>

#define NY_IDENTITY_MAX       16
#define NY_IDENTITY_NAME      63
#define NY_IDENTITY_SAMPLES   6
#define NY_IDENTITY_SAMPLE_MS 400
#define NY_IDENTITY_ENROLL_MS 30000
#define NY_IDENTITY_RECENT_MS 3000
#define NY_IDENTITY_MATCH     .55
#define NY_IDENTITY_MARGIN    .08

static mutex_t g_identity_lock = NXMUTEX_INITIALIZER;
static cJSON *g_identity_record;
static uint64_t g_identity_revision;
static uint64_t g_identity_seen[NY_IDENTITY_MAX];
static double g_identity_scores[NY_IDENTITY_MAX];
static unsigned int g_identity_faces;
static uint64_t g_identity_observed;
static const char *g_identity_phase = "idle";
static const char *g_identity_reason = "";
static char g_identity_name[NY_IDENTITY_NAME + 1];
static int g_identity_error;
static unsigned int g_identity_samples;
static uint64_t g_identity_started;
static uint64_t g_identity_deadline, g_identity_last_sample,
    g_identity_expected;
static int16_t g_identity_reference[NY_IDENTITY_DIM];
static double g_identity_sum[NY_IDENTITY_DIM];

static int ny_identity_load(void);
static bool ny_identity_name(const char *name);
static bool ny_identity_vector(const cJSON *array, int16_t *vector);
static double ny_identity_cosine(const int16_t *a, const int16_t *b);
static int ny_identity_save(cJSON *record);
static void ny_identity_fail(const char *reason, int error);
static cJSON *ny_identity_status(void);

/****************************************************************************
 * Name: ny_identity_name
 ****************************************************************************/
static bool ny_identity_name(const char *name)
{
  if (!name || !name[0] || strlen(name) > NY_IDENTITY_NAME)
    return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    if (*p < 0x20 || *p == 0x7f)
      return false;
  return true;
}

/****************************************************************************
 * Name: ny_identity_vector
 ****************************************************************************/
static bool ny_identity_vector(const cJSON *array, int16_t *vector)
{
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != NY_IDENTITY_DIM)
    return false;
  double energy = 0;
  for (int i = 0; i < NY_IDENTITY_DIM; i++)
    {
      const cJSON *v = cJSON_GetArrayItem(array, i);
      if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
          v->valuedouble < -32767 || v->valuedouble > 32767 ||
          floor(v->valuedouble) != v->valuedouble)
        return false;
      vector[i] = (int16_t)v->valueint;
      energy += (double)vector[i] * vector[i];
    }
  return energy > 800000000.0 && energy < 1300000000.0;
}

/****************************************************************************
 * Name: ny_identity_load
 ****************************************************************************/
static int ny_identity_load(void)
{
  if (g_identity_record)
    return 0;
  cJSON *record = NULL;
  uint64_t revision = 0;
  int ret = ny_product_store_read("identity", &record, &revision);
  if (ret < 0)
    return ret;
  if (!record)
    {
      record = cJSON_CreateObject();
      if (!record || !cJSON_AddNumberToObject(record, "schema", 1) ||
          !cJSON_AddArrayToObject(record, "members"))
        {
          cJSON_Delete(record);
          return -ENOMEM;
        }
    }
  cJSON *members = cJSON_GetObjectItemCaseSensitive(record, "members");
  const cJSON *schema = cJSON_GetObjectItemCaseSensitive(record, "schema");
  if (!cJSON_IsNumber(schema) || schema->valuedouble != 1 ||
      !cJSON_IsArray(members) || cJSON_GetArraySize(members) > NY_IDENTITY_MAX)
    {
      cJSON_Delete(record);
      return -EINVAL;
    }
  for (int i = 0; i < cJSON_GetArraySize(members); i++)
    {
      cJSON *member = cJSON_GetArrayItem(members, i);
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(member, "id");
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(member, "name");
      int16_t vector[NY_IDENTITY_DIM];
      if (!cJSON_IsString(id) || strlen(id->valuestring) > 31 ||
          !id->valuestring[0] || !cJSON_IsString(name) ||
          !ny_identity_name(name->valuestring) ||
          !ny_identity_vector(
              cJSON_GetObjectItemCaseSensitive(member, "feature"), vector))
        {
          cJSON_Delete(record);
          return -EINVAL;
        }
    }
  g_identity_record = record;
  g_identity_revision = revision;
  return 0;
}

/****************************************************************************
 * Name: ny_identity_cosine
 ****************************************************************************/
static double ny_identity_cosine(const int16_t *a, const int16_t *b)
{
  double dot = 0, aa = 0, bb = 0;
  for (int i = 0; i < NY_IDENTITY_DIM; i++)
    {
      dot += (double)a[i] * b[i];
      aa += (double)a[i] * a[i];
      bb += (double)b[i] * b[i];
    }
  return aa > 0 && bb > 0 ? fmax(-1, fmin(1, dot / sqrt(aa * bb))) : -1;
}

/****************************************************************************
 * Name: ny_identity_save
 ****************************************************************************/
static int ny_identity_save(cJSON *record)
{
  uint64_t revision = 0;
  int ret = ny_product_store_write("identity", record, g_identity_revision,
                                   &revision);
  if (ret == 0)
    {
      cJSON_Delete(g_identity_record);
      g_identity_record = record;
      g_identity_revision = revision;
    }
  else
    cJSON_Delete(record);
  return ret;
}

/****************************************************************************
 * Name: ny_identity_fail
 ****************************************************************************/
static void ny_identity_fail(const char *reason, int error)
{
  g_identity_phase = "error";
  g_identity_reason = reason;
  g_identity_error = error;
  memset(g_identity_reference, 0, sizeof(g_identity_reference));
  memset(g_identity_sum, 0, sizeof(g_identity_sum));
}

/****************************************************************************
 * Name: ny_product_identity_observe
 ****************************************************************************/
void ny_product_identity_observe(const int16_t *feature, unsigned int faces,
                                 uint64_t timestamp)
{
  nxmutex_lock(&g_identity_lock);
  if (ny_identity_load() < 0)
    {
      nxmutex_unlock(&g_identity_lock);
      return;
    }
  g_identity_observed = timestamp;
  g_identity_faces = faces;
  cJSON *members =
      cJSON_GetObjectItemCaseSensitive(g_identity_record, "members");
  int count = cJSON_GetArraySize(members), best = -1;
  double best_score = -1, second = -1;
  if (feature)
    for (int i = 0; i < count; i++)
      {
        int16_t vector[NY_IDENTITY_DIM];
        ny_identity_vector(cJSON_GetObjectItemCaseSensitive(
                               cJSON_GetArrayItem(members, i), "feature"),
                           vector);
        double score = ny_identity_cosine(feature, vector);
        if (score > best_score)
          {
            second = best_score;
            best_score = score;
            best = i;
          }
        else if (score > second)
          second = score;
      }
  if (best >= 0 && best_score >= NY_IDENTITY_MATCH &&
      best_score - second >= NY_IDENTITY_MARGIN)
    {
      g_identity_seen[best] = timestamp;
      g_identity_scores[best] = best_score;
    }
  if (strcmp(g_identity_phase, "collecting"))
    {
      nxmutex_unlock(&g_identity_lock);
      return;
    }
  uint64_t now = ny_product_time_ms(true);
  if (timestamp < g_identity_started)
    goto out;
  if (now >= g_identity_deadline)
    {
      ny_identity_fail("timeout", -ETIMEDOUT);
      goto out;
    }
  if (faces != 1)
    {
      g_identity_reason = "one_face";
      goto out;
    }
  if (!feature)
    {
      g_identity_reason = "closer";
      goto out;
    }
  if (g_identity_samples &&
      timestamp - g_identity_last_sample < NY_IDENTITY_SAMPLE_MS)
    goto out;
  if (g_identity_samples &&
      ny_identity_cosine(feature, g_identity_reference) < NY_IDENTITY_MATCH)
    {
      ny_identity_fail("face_changed", -EINVAL);
      goto out;
    }
  if (!g_identity_samples)
    memcpy(g_identity_reference, feature, sizeof(g_identity_reference));
  if (best_score > .75)
    {
      ny_identity_fail("duplicate", -EEXIST);
      goto out;
    }
  for (int i = 0; i < NY_IDENTITY_DIM; i++)
    g_identity_sum[i] += feature[i];
  g_identity_samples++;
  g_identity_last_sample = timestamp;
  g_identity_reason = "hold_still";
  if (g_identity_samples == NY_IDENTITY_SAMPLES)
    {
      if (g_identity_expected != g_identity_revision)
        {
          ny_identity_fail("changed", -ESTALE);
          goto out;
        }
      double energy = 0;
      for (int i = 0; i < NY_IDENTITY_DIM; i++)
        energy += g_identity_sum[i] * g_identity_sum[i];
      if (energy < 1)
        {
          ny_identity_fail("quality", -EINVAL);
          goto out;
        }
      cJSON *next = cJSON_Duplicate(g_identity_record, true),
            *entry = cJSON_CreateObject();
      cJSON *vector = entry ? cJSON_AddArrayToObject(entry, "feature") : NULL;
      char id[32];
      snprintf(id, sizeof(id), "person-%llu",
               (unsigned long long)(g_identity_revision + 1));
      bool ok = next && entry && vector &&
                cJSON_AddStringToObject(entry, "id", id) &&
                cJSON_AddStringToObject(entry, "name", g_identity_name);
      double scale = 32767 / sqrt(energy);
      for (int i = 0; ok && i < NY_IDENTITY_DIM; i++)
        {
          cJSON *number =
              cJSON_CreateNumber(lround(g_identity_sum[i] * scale));
          if (!number || !cJSON_AddItemToArray(vector, number))
            {
              cJSON_Delete(number);
              ok = false;
            }
        }
      if (!ok || !cJSON_AddItemToArray(
                     cJSON_GetObjectItemCaseSensitive(next, "members"), entry))
        {
          cJSON_Delete(entry);
          cJSON_Delete(next);
          ny_identity_fail("storage", -ENOMEM);
          goto out;
        }
      int ret = ny_identity_save(next);
      if (ret < 0)
        {
          ny_identity_fail("storage", ret);
          goto out;
        }
      g_identity_phase = "saved";
      g_identity_reason = "";
      g_identity_error = 0;
      memset(g_identity_reference, 0, sizeof(g_identity_reference));
      memset(g_identity_sum, 0, sizeof(g_identity_sum));
    }
out:
  nxmutex_unlock(&g_identity_lock);
}

/****************************************************************************
 * Name: ny_product_identity_clear_live
 ****************************************************************************/
void ny_product_identity_clear_live(void)
{
  nxmutex_lock(&g_identity_lock);
  g_identity_faces = 0;
  g_identity_observed = 0;
  memset(g_identity_seen, 0, sizeof(g_identity_seen));
  memset(g_identity_scores, 0, sizeof(g_identity_scores));
  if (!strcmp(g_identity_phase, "collecting"))
    ny_identity_fail("stopped", -ECANCELED);
  nxmutex_unlock(&g_identity_lock);
}

/****************************************************************************
 * Name: ny_identity_status
 ****************************************************************************/
static cJSON *ny_identity_status(void)
{
  uint64_t now = ny_product_time_ms(true);
  if (!strcmp(g_identity_phase, "collecting") && now >= g_identity_deadline)
    ny_identity_fail("timeout", -ETIMEDOUT);
  cJSON *out = cJSON_CreateObject();
  cJSON *list = out ? cJSON_AddArrayToObject(out, "members") : NULL;
  bool recent =
      g_identity_observed && now - g_identity_observed < NY_IDENTITY_RECENT_MS;
  bool ok =
      list && cJSON_AddNumberToObject(out, "revision", g_identity_revision) &&
      cJSON_AddStringToObject(out, "phase", g_identity_phase) &&
      cJSON_AddStringToObject(out, "reason", g_identity_reason) &&
      cJSON_AddNumberToObject(out, "lastError", g_identity_error) &&
      cJSON_AddNumberToObject(out, "samples", g_identity_samples) &&
      cJSON_AddNumberToObject(out, "required", NY_IDENTITY_SAMPLES) &&
      cJSON_AddStringToObject(out, "enrolling", g_identity_name) &&
      cJSON_AddNumberToObject(out, "faces", recent ? g_identity_faces : 0);
  cJSON *members =
      cJSON_GetObjectItemCaseSensitive(g_identity_record, "members");
  for (int i = 0; ok && i < cJSON_GetArraySize(members); i++)
    {
      cJSON *source = cJSON_GetArrayItem(members, i),
            *row = cJSON_CreateObject();
      if (!row)
        {
          ok = false;
          break;
        }
      bool present = recent && g_identity_faces > 0 && g_identity_seen[i] &&
                     now - g_identity_seen[i] < NY_IDENTITY_RECENT_MS;
      ok =
          cJSON_AddStringToObject(
              row, "id",
              cJSON_GetObjectItemCaseSensitive(source, "id")->valuestring) &&
          cJSON_AddStringToObject(
              row, "name",
              cJSON_GetObjectItemCaseSensitive(source, "name")->valuestring) &&
          cJSON_AddBoolToObject(row, "recent", present) &&
          cJSON_AddNumberToObject(
              row, "seenAgoMs",
              g_identity_seen[i] ? (double)(now - g_identity_seen[i]) : -1) &&
          cJSON_AddNumberToObject(row, "confidence",
                                  present ? g_identity_scores[i] : 0);
      if (!ok || !cJSON_AddItemToArray(list, row))
        {
          cJSON_Delete(row);
          ok = false;
        }
    }
  if (!ok)
    {
      cJSON_Delete(out);
      return NULL;
    }
  return out;
}

/****************************************************************************
 * Name: ny_product_identity_request
 ****************************************************************************/
int ny_product_identity_request(const struct ny_product_caller_s *caller,
                                const char *topic, const cJSON *data,
                                cJSON **result)
{
  bool status =
      !strcmp(topic, "identity.status") || !strcmp(topic, "presence.status");
  bool enroll = !strcmp(topic, "identity.enroll.start"),
       cancel = !strcmp(topic, "identity.enroll.cancel");
  bool remove = !strcmp(topic, "identity.remove"),
       rename = !strcmp(topic, "identity.rename");
  if (!status && !enroll && !cancel && !remove && !rename)
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  if (enroll && (!ny_product_privacy_camera_allowed() ||
                 !ny_product_vision_identifying()))
    return -EACCES;
  nxmutex_lock(&g_identity_lock);
  int ret = ny_identity_load();
  if (ret < 0)
    goto out;
  if (cancel)
    {
      g_identity_phase = "idle";
      g_identity_reason = "";
      g_identity_error = 0;
      g_identity_samples = 0;
      memset(g_identity_sum, 0, sizeof(g_identity_sum));
      memset(g_identity_reference, 0, sizeof(g_identity_reference));
    }
  if (enroll || remove || rename)
    {
      const cJSON *revision =
          cJSON_GetObjectItemCaseSensitive(data, "revision");
      if (!cJSON_IsNumber(revision) || !isfinite(revision->valuedouble) ||
          revision->valuedouble < 0 ||
          revision->valuedouble > 9007199254740991.0 ||
          floor(revision->valuedouble) != revision->valuedouble)
        {
          ret = -EINVAL;
          goto out;
        }
      if ((uint64_t)revision->valuedouble != g_identity_revision)
        {
          ret = -ESTALE;
          goto out;
        }
    }
  if (enroll)
    {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(data, "name");
      if (!cJSON_IsString(name) || !ny_identity_name(name->valuestring))
        {
          ret = -EINVAL;
          goto out;
        }
      if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(
              g_identity_record, "members")) >= NY_IDENTITY_MAX)
        {
          ret = -ENOSPC;
          goto out;
        }
      if (!strcmp(g_identity_phase, "collecting"))
        {
          ret = -EBUSY;
          goto out;
        }
      snprintf(g_identity_name, sizeof(g_identity_name), "%s",
               name->valuestring);
      g_identity_phase = "collecting";
      g_identity_reason = "one_face";
      g_identity_samples = 0;
      g_identity_error = 0;
      g_identity_expected = g_identity_revision;
      g_identity_started = ny_product_time_ms(true);
      g_identity_deadline = g_identity_started + NY_IDENTITY_ENROLL_MS;
      g_identity_last_sample = 0;
      memset(g_identity_sum, 0, sizeof(g_identity_sum));
    }
  if (remove || rename)
    {
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "id"),
                  *name = cJSON_GetObjectItemCaseSensitive(data, "name");
      if (!cJSON_IsString(id) ||
          (rename &&
           (!cJSON_IsString(name) || !ny_identity_name(name->valuestring))))
        {
          ret = -EINVAL;
          goto out;
        }
      cJSON *next = cJSON_Duplicate(g_identity_record, true);
      if (!next)
        {
          ret = -ENOMEM;
          goto out;
        }
      cJSON *members = cJSON_GetObjectItemCaseSensitive(next, "members");
      int index = -1;
      for (int i = 0; i < cJSON_GetArraySize(members); i++)
        if (!strcmp(cJSON_GetObjectItemCaseSensitive(
                        cJSON_GetArrayItem(members, i), "id")
                        ->valuestring,
                    id->valuestring))
          index = i;
      if (index < 0)
        {
          cJSON_Delete(next);
          ret = -ENOENT;
          goto out;
        }
      if (remove)
        cJSON_DeleteItemFromArray(members, index);
      else if (!cJSON_SetValuestring(
                   cJSON_GetObjectItemCaseSensitive(
                       cJSON_GetArrayItem(members, index), "name"),
                   name->valuestring))
        {
          cJSON_Delete(next);
          ret = -ENOMEM;
          goto out;
        }
      ret = ny_identity_save(next);
      if (ret < 0)
        goto out;
      if (remove)
        {
          memset(g_identity_seen, 0, sizeof(g_identity_seen));
          memset(g_identity_scores, 0, sizeof(g_identity_scores));
        }
    }
  *result = ny_identity_status();
  if (!*result)
    ret = -ENOMEM;
out:
  nxmutex_unlock(&g_identity_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_product_identity_faces
 ****************************************************************************/
void ny_product_identity_faces(unsigned int faces, uint64_t timestamp)
{
  nxmutex_lock(&g_identity_lock);
  g_identity_faces = faces;
  g_identity_observed = timestamp;
  nxmutex_unlock(&g_identity_lock);
}
