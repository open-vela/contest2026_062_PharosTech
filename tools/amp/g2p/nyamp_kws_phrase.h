/****************************************************************************
 * tools/amp/g2p/nyamp_kws_phrase.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __TOOLS_AMP_G2P_NYAMP_KWS_PHRASE_H
#define __TOOLS_AMP_G2P_NYAMP_KWS_PHRASE_H
#include <string>
namespace nyamp
{
// Convert ordinary text using the existing Melo lexicon. Validate every
// generated phone against the KWS inventory; never drop unknown words.
bool CompileWakePhrase(const std::string &g2p_directory,
                       const std::string &kws_tokens, const std::string &text,
                       std::string *line, std::string *error);
}
#endif
