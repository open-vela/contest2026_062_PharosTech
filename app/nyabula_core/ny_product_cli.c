/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_cli.c
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

#include "ny_product.h"
#include "ny_product_store.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NY_PRODUCT_CLI_LIMIT        4096
#define NY_PRODUCT_CLI_SOURCE_LIMIT 24576

/****************************************************************************
 * Name: ny_product_cli
 ****************************************************************************/

int ny_product_cli(const char *topic, const char *path)
{
  const struct ny_product_caller_s caller = { "nsh", NY_PRODUCT_OWNER, true };
  size_t limit = strcmp(topic, "agent.skills.save") == 0 ||
                         strcmp(topic, "plugin.author.deploy") == 0
                     ? NY_PRODUCT_CLI_SOURCE_LIMIT
                     : NY_PRODUCT_CLI_LIMIT;
  char *input;
  FILE *stream = fopen(path, "rb");
  cJSON *data;
  cJSON *result = NULL;
  char *output;
  size_t length;
  int ret;

  if (stream == NULL)
    {
      return -errno;
    }

  input = malloc(limit + 1);
  if (input == NULL)
    {
      fclose(stream);
      return -ENOMEM;
    }

  length = fread(input, 1, limit + 1, stream);
  ret = ferror(stream) ? -EIO : 0;
  fclose(stream);
  if (ret < 0 || length > limit)
    {
      free(input);
      return ret < 0 ? ret : -E2BIG;
    }

  ret = ny_product_json_check(input, length);
  if (ret < 0)
    {
      free(input);
      return ret;
    }

  input[length] = '\0';
  data = cJSON_ParseWithOpts(input, NULL, true);
  free(input);
  if (data == NULL)
    {
      return -EINVAL;
    }

  ret = ny_product_request(&caller, topic, data, &result);
  cJSON_Delete(data);
  if (result != NULL)
    {
      output = cJSON_PrintUnformatted(result);
      if (output == NULL)
        {
          if (ret == 0)
            ret = -ENOMEM;
        }
      else
        {
          printf("PRODUCT_RESULT %s\n", output);
          cJSON_free(output);
        }
    }

  cJSON_Delete(result);
  return ret;
}
