/****************************************************************************
 * tools/amp/models/nyamp_backends.h
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

#ifndef __TOOLS_AMP_MODELS_NYAMP_BACKENDS_H
#define __TOOLS_AMP_MODELS_NYAMP_BACKENDS_H

#include "nyamp_models.h"

namespace nyamp::models
{

// Link only the matching optional backend target and its external runtime.
std::unique_ptr<Backend> CreateSherpaBackend();
std::unique_ptr<Backend> CreateRkllmBackend();
std::unique_ptr<Backend> CreateMeloBackend();

} // namespace nyamp::models

#endif // __TOOLS_AMP_MODELS_NYAMP_BACKENDS_H
