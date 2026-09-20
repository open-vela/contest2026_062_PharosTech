/****************************************************************************
 * tools/amp/voice/nyamp_speaker.h
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

#ifndef __TOOLS_AMP_VOICE_NYAMP_SPEAKER_H
#define __TOOLS_AMP_VOICE_NYAMP_SPEAKER_H

#include "nyamp_voice.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nyamp::voice
{

// Below half a second the model still returns a vector, but the host
// evaluation shows it carries almost no speaker information, so such input
// is refused instead of producing a confident-looking score.
constexpr std::size_t kSpeakerMinSamples = kSampleRate / 2;

// Matches the ASR request bound; the embedding is one inference over the
// whole segment, so its memory grows with the length.
constexpr std::size_t kSpeakerMaxSamples = kSampleRate * 60;

// A unit-length speaker embedding.  Every vector that leaves this module is
// L2-normalized, so cosine similarity is a plain dot product everywhere.
using Embedding = std::vector<float>;

struct SpeakerConfig
{
  // Directory holding speaker.onnx, from the manifest-verified model store.
  std::string model_directory;
  std::int32_t num_threads = 1;
};

class SpeakerEmbedder
{
public:
  SpeakerEmbedder();
  ~SpeakerEmbedder();
  SpeakerEmbedder(const SpeakerEmbedder &) = delete;
  SpeakerEmbedder &operator=(const SpeakerEmbedder &) = delete;

  Status Load(const SpeakerConfig &config);
  void Unload();
  std::size_t dim() const;

  // Normalized mono float32 at 16 kHz.  The extraction is a single model
  // run, so stop is honoured before it starts and after it ends; a result
  // computed for a cancelled request is discarded, never returned.
  Status Embed(const float *samples, std::size_t count, Embedding *embedding,
               const Stop &stop);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Cosine similarity in [-1, 1]; returns -2 when the inputs cannot be
// compared (different or zero dimension, zero norm), which is below any
// threshold and therefore always a rejection.
float Cosine(const Embedding &a, const Embedding &b);

struct EnrolPolicy
{
  // The owner says the wake phrase four or five times.  Three consistent
  // takes are the fewest that still average out one noisy take.
  std::size_t min_kept = 3;
  std::size_t max_segments = 8;

  // A take whose mean cosine to the other takes is below this is treated as
  // someone else, a cough or a truncated capture, and is left out of the
  // voiceprint.  The value is a host-evaluation result; see README.md.
  float min_mean_cosine = 0.45f;

  // A take is also left out when its mean cosine is this far below the
  // median take, even if it clears the absolute bar.  Someone else saying
  // the same phrase can score above any absolute bar that genuine short
  // takes still pass, but not close to how well the owner agrees with
  // himself.  Zero disables the rule.  Unlike the absolute bar it never
  // fails an enrolment; it only prunes while more than min_kept remain.
  float max_below_median = 0.15f;
};

struct EnrolResult
{
  Status status = Status::kInvalid;
  Embedding voiceprint;
  // One flag per added segment, in Add order.
  std::vector<bool> kept;
  // Mean pairwise cosine of the kept segments: how self-consistent the
  // enrolment was.  The control domain may show it as enrolment quality.
  float cohesion = 0.0f;
};

// Accumulates per-take embeddings in the compute domain for one enrolment
// dialogue.  Nothing here touches storage: the finished voiceprint is handed
// to the control domain, which is the only side that persists it.
class Enrolment
{
public:
  Status Add(const Embedding &embedding, const EnrolPolicy &policy);
  void Clear();
  std::size_t size() const;
  EnrolResult Commit(const EnrolPolicy &policy) const;

private:
  std::vector<Embedding> segments_;
};

// Score of a probe against a stored voiceprint; the caller compares it with
// its own threshold so the policy stays in the control domain.
float Verify(const Embedding &voiceprint, const Embedding &probe);

// Voiceprint container, little-endian:
//   u32 magic "NYVP", u16 version, u16 dim, u32 model_tag, u16 segments,
//   u16 reserved, then dim IEEE half floats.
// Half precision is used because 192 float32 values (768 bytes) would not
// fit one RPMsg payload, while 192 half floats plus the header are 400
// bytes; the cosine error this introduces is measured by the file test and
// is far below the score spread between speakers.
constexpr std::uint32_t kTemplateMagic = 0x5056594eU;
constexpr std::uint16_t kTemplateVersion = 1;
constexpr std::size_t kTemplateHeaderSize = 16;

struct TemplateInfo
{
  // Identifies the embedding model; a voiceprint made by another model is
  // not comparable and must be rejected rather than scored.
  std::uint32_t model_tag = 0;
  std::uint16_t segments = 0;
};

std::vector<std::uint8_t> PackTemplate(const Embedding &voiceprint,
                                       const TemplateInfo &info);
Status UnpackTemplate(const std::uint8_t *bytes, std::size_t size,
                      Embedding *voiceprint, TemplateInfo *info);

} // namespace nyamp::voice

#endif // __TOOLS_AMP_VOICE_NYAMP_SPEAKER_H
