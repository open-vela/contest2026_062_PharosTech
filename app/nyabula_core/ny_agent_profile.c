/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_profile.c
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

#include "ny_agent.h"
#include "ny_product_store.h"
#include <errno.h>
#include <math.h>
#include <string.h>

/****************************************************************************
 * Name: ny_agent_profile
 ****************************************************************************/

int ny_agent_profile(const char *topic, const cJSON *data, cJSON **result)
{
  bool save = !strcmp(topic, "agent.profile.set");
  if (!save && strcmp(topic, "agent.profile.get"))
    return -ENOSYS;
  cJSON *root = NULL;
  uint64_t revision;
  int ret = ny_product_store_read("agent-profile", &root, &revision);
  if (ret < 0)
    return ret;
  if (!root)
    root = cJSON_Parse(
        "{\"catName\":\"Nyabula\",\"ownerName\":\"\","
        "\"language\":\"zh-CN\",\"tone\":\"warm\",\"instructions\":\"\"}");
  if (!root)
    return -ENOMEM;
  if (save)
    {
      const cJSON *expected =
          cJSON_GetObjectItemCaseSensitive(data, "revision");
      if (!cJSON_IsNumber(expected) || !isfinite(expected->valuedouble) ||
          expected->valuedouble != (double)revision)
        ret = -ESTALE;
      static const char *const fields[] = { "catName", "ownerName", "language",
                                            "tone", "instructions" };
      for (size_t i = 0; ret == 0 && i < sizeof(fields) / sizeof(fields[0]);
           i++)
        {
          const cJSON *value =
              cJSON_GetObjectItemCaseSensitive(data, fields[i]);
          if (!cJSON_IsString(value) ||
              strlen(value->valuestring) > (i == 4 ? 2048u : 96u) ||
              ((i == 0 || i == 2 || i == 3) && !value->valuestring[0]))
            ret = -EINVAL;
          else
            {
              cJSON_DeleteItemFromObjectCaseSensitive(root, fields[i]);
              if (!cJSON_AddStringToObject(root, fields[i],
                                           value->valuestring))
                ret = -ENOMEM;
            }
        }
      if (ret == 0)
        ret =
            ny_product_store_write("agent-profile", root, revision, &revision);
    }
  if (ret == 0 && !cJSON_AddNumberToObject(root, "revision", revision))
    ret = -ENOMEM;
  if (ret < 0)
    cJSON_Delete(root);
  else
    *result = root;
  return ret;
}
