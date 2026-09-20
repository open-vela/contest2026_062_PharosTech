/****************************************************************************
 * tools/amp/nyampd/nyampd_kws_sherpa.cpp
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

/* The real keyword spotter: tools/amp/voice on the sherpa-onnx runtime.
 * Kept in a file of its own so nyampd_core links without that runtime.
 */

#include "nyamp_kws.h"
#include "nyampd_kws.h"

namespace nyamp
{
namespace
{

class SherpaKwsBackend final : public KwsBackend
{
public:
  models::Status Load(const KwsModel &model) override
  {
    /* A zero keeps the default, which is the operating point the host
     * evaluation chose (tools/amp/voice/README.md), not the library's.
     */
    voice::KwsConfig config;
    config.model_directory = model.directory;
    config.keywords_file = model.keywords_file;
    if (model.threshold > 0.0f)
      {
        config.keywords_threshold = model.threshold;
      }

    if (model.score > 0.0f)
      {
        config.keywords_score = model.score;
      }

    if (model.max_active_paths != 0)
      {
        config.max_active_paths = model.max_active_paths;
      }

    if (model.num_trailing_blanks != 0)
      {
        config.num_trailing_blanks = model.num_trailing_blanks;
      }

    return spotter_.Load(config);
  }

  void Unload() override { spotter_.Unload(); }

  models::Status Accept(const float *samples, std::size_t count,
                        const Sink &sink, const models::Stop &stop) override
  {
    return spotter_.AcceptWaveform(
        samples, count,
        [&sink](const voice::KwsDetection &detection) {
          KwsHit hit;
          hit.label = detection.keyword;
          hit.keyword_id = detection.keyword_id;
          hit.has_offsets = detection.has_offsets;
          hit.has_score = detection.score >= 0.0f;
          hit.score = detection.score;
          hit.start = detection.start_sample;
          hit.end = detection.end_sample;
          hit.trigger = detection.trigger_sample;
          return sink(hit);
        },
        stop);
  }

  models::Status Reset() override { return spotter_.Reset(); }

  std::vector<std::string> Labels() const override
  {
    return spotter_.labels();
  }

private:
  voice::KeywordSpotter spotter_;
};

} // namespace

std::unique_ptr<KwsBackend> CreateSherpaKwsBackend()
{
  return std::make_unique<SherpaKwsBackend>();
}

} // namespace nyamp
