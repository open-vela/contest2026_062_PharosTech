/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_utf8.h
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

#ifndef __NYABULA_CORE_NY_UTF8_H
#define __NYABULA_CORE_NY_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Name: ny_utf8_valid
 ****************************************************************************/

static inline bool ny_utf8_valid(const unsigned char *data, size_t length)
{
  size_t i = 0;
  while (i < length)
    {
      uint32_t code = data[i++];
      uint32_t minimum;
      unsigned int count;
      if (code < 0x80)
        {
          continue;
        }

      if (code >= 0xc2 && code <= 0xdf)
        {
          count = 1;
          minimum = 0x80;
          code &= 0x1f;
        }
      else if (code >= 0xe0 && code <= 0xef)
        {
          count = 2;
          minimum = 0x800;
          code &= 0x0f;
        }
      else if (code >= 0xf0 && code <= 0xf4)
        {
          count = 3;
          minimum = 0x10000;
          code &= 7;
        }
      else
        {
          return false;
        }

      if (count > length - i)
        {
          return false;
        }

      while (count-- > 0)
        {
          if ((data[i] & 0xc0) != 0x80)
            {
              return false;
            }

          code = (code << 6) | (data[i++] & 0x3f);
        }

      if (code < minimum || code > 0x10ffff ||
          (code >= 0xd800 && code <= 0xdfff))
        {
          return false;
        }
    }

  return true;
}

#endif
