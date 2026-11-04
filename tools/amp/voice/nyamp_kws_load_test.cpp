/****************************************************************************
 * tools/amp/voice/nyamp_kws_load_test.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
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

#include "nyamp_kws.h"
#include <algorithm>
#include <cstdio>

int main(int argc, char **argv)
{
  if (argc != 3) return 2;
  nyamp::voice::KwsConfig config;
  config.model_directory = argv[1];
  config.keywords_file = argv[2];
  for (int custom = 0; custom < 2; ++custom)
    {
      if (custom)
        config.extra_keywords = "x i\u01ceo m i\u0101o x i\u01ceo m i\u0101o @custom";
      nyamp::voice::KeywordSpotter spotter;
      if (spotter.Load(config) != nyamp::voice::Status::kOk)
        {
          std::fprintf(stderr, "KWS_LOAD_FAILED custom=%d\n", custom);
          return 1;
        }
      const auto labels = spotter.labels();
      if (labels.size() != static_cast<unsigned int>(custom ? 3 : 2) ||
          (custom && std::find(labels.begin(), labels.end(), "custom") == labels.end()))
        return 1;
      std::printf("KWS_REAL_LOAD_PASS custom=%d labels=%zu\n", custom, labels.size());
    }
  return 0;
}
