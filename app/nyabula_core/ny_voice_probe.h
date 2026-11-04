/****************************************************************************
 * app/nyabula_core/ny_voice_probe.h
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

#ifndef __NYABULA_CORE_NY_VOICE_PROBE_H
#define __NYABULA_CORE_NY_VOICE_PROBE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NY_VOICE_PROBE_LABEL_MAX  48
#define NY_VOICE_PROBE_PREPARE_MS 30000

enum ny_voice_probe_kind_e
{
  NY_VOICE_PROBE_NONE,
  NY_VOICE_PROBE_WAKE,
  NY_VOICE_PROBE_MIC
};
struct ny_voice_probe_s
{
  enum ny_voice_probe_kind_e kind;
  bool active;
  bool ready;
  bool cancelled;
  uint32_t id;
  uint32_t duration_ms;
  uint64_t prepare_until;
  uint64_t started_ms;
  uint64_t until_ms;
  uint64_t samples;
  uint64_t squares;
  uint32_t peak;
  uint32_t max_rms;
  uint32_t clipped;
  uint32_t detections;
  char word[NY_VOICE_PROBE_LABEL_MAX];
  int error;
};
int ny_voice_probe_begin(struct ny_voice_probe_s *probe,
                         enum ny_voice_probe_kind_e kind, uint32_t duration_ms,
                         uint64_t now);
bool ny_voice_probe_tick(struct ny_voice_probe_s *probe, uint64_t now,
                         bool ready, int error);
void ny_voice_probe_cancel(struct ny_voice_probe_s *probe);
void ny_voice_probe_feed(struct ny_voice_probe_s *probe,
                         const int16_t *samples, size_t count, uint32_t rms);
void ny_voice_probe_hit(struct ny_voice_probe_s *probe, const char *word,
                        size_t length);
#endif
