/****************************************************************************
 * tools/amp/voice/nyamp_voice.h
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

#ifndef __TOOLS_AMP_VOICE_NYAMP_VOICE_H
#define __TOOLS_AMP_VOICE_NYAMP_VOICE_H

#include <cstdint>
#include <functional>

#ifdef NYAMP_VOICE_WITH_MODELS
#include "nyamp_models.h"
#endif

namespace nyamp::voice
{

constexpr std::uint32_t kInterfaceVersion = 1;
constexpr std::uint32_t kSampleRate = 16000;

// The voice layer is built before tools/amp/models is merged, so it cannot
// depend on that header unconditionally.  The enumerators are kept in the
// same order as nyamp::models::Status, which in turn matches the negated
// nyamp_model_status_e wire values, so no layer ever needs a translation
// table.  Once both trees live together, define NYAMP_VOICE_WITH_MODELS and
// the two types become one.

#ifdef NYAMP_VOICE_WITH_MODELS
using Status = nyamp::models::Status;
#else
enum class Status
{
  kOk,
  kInvalid,
  kNotReady,
  kBusy,
  kStaleGeneration,
  kDuplicate,
  kCancelled,
  kDeadline,
  kBackendError,
  kUnsupported,
  kConsumerStopped
};
#endif

// Same cooperative-stop convention as nyamp::models::Stop: the owner folds
// cancel and deadline into one predicate, the backend polls it between
// inference steps and never pre-empts a step that is already running.
using Stop = std::function<bool()>;

// Wire value of a status (nyamp_model_status_e): zero or negative.
constexpr std::int32_t WireStatus(Status status)
{
  return -static_cast<std::int32_t>(status);
}

} // namespace nyamp::voice

#endif // __TOOLS_AMP_VOICE_NYAMP_VOICE_H
