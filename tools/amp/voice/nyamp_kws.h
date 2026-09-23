/****************************************************************************
 * tools/amp/voice/nyamp_kws.h
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

#ifndef __TOOLS_AMP_VOICE_NYAMP_KWS_H
#define __TOOLS_AMP_VOICE_NYAMP_KWS_H

#include "nyamp_voice.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nyamp::voice
{

// Longest window one AcceptWaveform call may carry.  It matches the ASR
// window upper bound (one second) so a shared audio window never has to be
// split just for the keyword spotter.
constexpr std::size_t kKwsMaxWindowSamples = kSampleRate;

constexpr std::uint16_t kKwsUnknownKeyword = 0xffff;

struct KwsConfig
{
  // Directory holding encoder.onnx, decoder.onnx, joiner.onnx, tokens.txt
  // and keywords.txt.  Fixed names keep the manifest-verified directory the
  // only input, exactly as the ASR backend does.
  std::string model_directory;

  // Optional replacement for <model_directory>/keywords.txt.  The file test
  // uses it to sweep keyword variants without copying the model.  Every
  // line must end in an "@label" without spaces; Load refuses the file
  // otherwise, because an unlabelled line would have no stable keyword id.
  std::string keywords_file;

  // Defaults are the operating point chosen by the host evaluation in
  // README.md, not the sherpa-onnx library defaults.  The wake phrase
  // switches language in the middle, so its keyword path is long and the
  // free decoder keeps out-scoring it: with the library's four active
  // paths the phrase was pruned before it could complete (44 % of the
  // synthetic Chinese takes detected, 72 % with eight, 91 % with sixteen),
  // while sixteen paths cost under 4 % more CPU.  A boost above 2 combined
  // with a threshold above 0.25 lowered detection again, so neither is
  // "the higher the safer".
  float keywords_threshold = 0.10f;
  float keywords_score = 2.0f;
  std::int32_t num_trailing_blanks = 1;
  std::int32_t max_active_paths = 16;
  std::int32_t num_threads = 1;
};

struct KwsDetection
{
  // The "@label" of the matched keywords.txt line, UTF-8.
  std::string keyword;

  // Zero-based index of that label among the distinct labels of
  // keywords.txt, in order of first appearance.  Several lines may share a
  // label (pronunciation variants of one phrase), so the id names the
  // phrase, not the line.  The wire event carries this instead of making
  // the control domain compare strings.
  std::uint16_t keyword_id = kKwsUnknownKeyword;
  std::vector<std::string> tokens;

  // Offsets count samples accepted since Load (or the last Reset), so the
  // owner can map them onto its own window sequence.  They come from the
  // decoder's 40 ms token timestamps: start is the first token and end is
  // one frame past the last token, which is coarse but enough to cut the
  // wake phrase out for speaker verification.
  bool has_offsets = false;
  std::uint64_t start_sample = 0;
  std::uint64_t end_sample = 0;

  // Samples accepted when the trigger fired; always valid.
  std::uint64_t trigger_sample = 0;

  // sherpa-onnx 1.13.8 applies keywords_threshold internally and does not
  // export the acoustic probability through its C API.  The field is kept
  // so the wire format does not change when a later runtime exposes it;
  // until then it is negative, meaning "not available".
  float score = -1.0f;
};

// Returning false stops delivery, mirroring nyamp::models::EventSink.
using KwsSink = std::function<bool(const KwsDetection &)>;

// One always-on keyword stream.
//
// This is deliberately not a nyamp::models::Backend: a Backend receives one
// complete input and terminates, while a wake word listener never
// terminates and must keep decoder state across windows.  The lifecycle
// rules are the same, though: Load/AcceptWaveform/Reset/Unload belong to one
// owning thread, no worker thread is hidden, and no callback outlives the
// call that made it.
class KeywordSpotter
{
public:
  KeywordSpotter();
  ~KeywordSpotter();
  KeywordSpotter(const KeywordSpotter &) = delete;
  KeywordSpotter &operator=(const KeywordSpotter &) = delete;

  Status Load(const KwsConfig &config);
  void Unload();

  // Normalized mono float32 at 16 kHz.  Detections found while decoding
  // this window are delivered before the call returns.
  Status AcceptWaveform(const float *samples, std::size_t count,
                        const KwsSink &sink, const Stop &stop);

  // Drops decoder state and restarts the sample offset at zero.  Used when
  // the capture path had a gap, so stale context cannot complete a keyword
  // across the discontinuity.
  Status Reset();

  std::uint64_t accepted_samples() const;

  // Distinct labels in keyword-id order, for the control domain's LIST.
  const std::vector<std::string> &labels() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nyamp::voice

#endif // __TOOLS_AMP_VOICE_NYAMP_KWS_H
