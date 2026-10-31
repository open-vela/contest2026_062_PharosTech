/****************************************************************************
 * tools/amp/models/nyamp_streaming.h
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

#ifndef __TOOLS_AMP_MODELS_NYAMP_STREAMING_H
#define __TOOLS_AMP_MODELS_NYAMP_STREAMING_H

#include "nyamp_models.h"

#include <cstddef>
#include <memory>
#include <string>

namespace nyamp::models
{

// Incremental speech recognition.
//
// Backend::Run takes one complete input, which suits a file but not a
// microphone: the audio of an utterance does not exist yet when recognition
// has to start.  A stream is fed as the samples arrive and can be asked for
// its current hypothesis at any time.
//
// One thread owns a stream; nothing here is called concurrently.  A stream
// must be destroyed before the backend that made it is unloaded.

class AsrStream
{
public:
  virtual ~AsrStream() = default;

  // Normalized mono float32 at kAsrSampleRate.
  virtual void Accept(const float *samples, std::size_t count) = 0;

  // Decode whatever the accepted audio allows.  Returns false when `stop`
  // asked for it or the runtime failed; the stream is then only good for
  // destruction.
  virtual bool Decode(const Stop &stop) = 0;

  // The whole hypothesis so far, not a delta.  A transducer may rewrite
  // earlier tokens, so the caller compares with what it published last.
  virtual bool Text(std::string *text) = 0;

  // The decoder's own opinion that the utterance is over (trailing
  // silence).  Advisory: the stream keeps accepting audio either way.
  virtual bool Endpoint() = 0;

  // No more audio will come; the next Decode flushes the tail.
  virtual void InputFinished() = 0;
};

class AsrStreamBackend
{
public:
  virtual ~AsrStreamBackend() = default;
  virtual Status Load(const std::string &model_directory) = 0;
  virtual void Unload() = 0;
  virtual std::unique_ptr<AsrStream> CreateStream() = 0;
};

// Link nyamp_sherpa and its external runtime to get this one.
std::unique_ptr<AsrStreamBackend> CreateSherpaStreamBackend();

} // namespace nyamp::models

#endif // __TOOLS_AMP_MODELS_NYAMP_STREAMING_H
