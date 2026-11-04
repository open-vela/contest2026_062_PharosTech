/****************************************************************************
 * app/nyabula_core/ny_voice_probe.c
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

#include "ny_voice_probe.h"
#include <errno.h>
#include <string.h>

/****************************************************************************
 * Name: ny_voice_probe_begin
 ****************************************************************************/
int ny_voice_probe_begin(struct ny_voice_probe_s *p,
                         enum ny_voice_probe_kind_e kind, uint32_t duration_ms,
                         uint64_t now)
{
  if (kind != NY_VOICE_PROBE_WAKE && kind != NY_VOICE_PROBE_MIC)
    return -EINVAL;
  if (duration_ms < 1000 || duration_ms > 30000)
    return -EINVAL;
  if (p->active)
    return -EBUSY;
  uint32_t id = p->id + 1;
  memset(p, 0, sizeof(*p));
  p->id = id;
  p->kind = kind;
  p->duration_ms = duration_ms;
  p->active = true;
  p->prepare_until = now + NY_VOICE_PROBE_PREPARE_MS;
  return 0;
}

/****************************************************************************
 * Name: ny_voice_probe_tick
 ****************************************************************************/
bool ny_voice_probe_tick(struct ny_voice_probe_s *p, uint64_t now, bool ready,
                         int error)
{
  if (!p->active)
    return false;
  if (error < 0)
    {
      p->error = error;
      p->active = false;
      return true;
    }
  if (!p->ready && ready)
    {
      p->ready = true;
      p->started_ms = now;
      p->until_ms = now + p->duration_ms;
    }
  if (!p->ready && now >= p->prepare_until)
    {
      p->error = -ETIMEDOUT;
      p->active = false;
      return true;
    }
  if (p->ready && now >= p->until_ms)
    {
      p->active = false;
      return true;
    }
  return false;
}

/****************************************************************************
 * Name: ny_voice_probe_cancel
 ****************************************************************************/
void ny_voice_probe_cancel(struct ny_voice_probe_s *p)
{
  if (p->active)
    {
      p->cancelled = true;
      p->active = false;
    }
}

/****************************************************************************
 * Name: ny_voice_probe_feed
 ****************************************************************************/
void ny_voice_probe_feed(struct ny_voice_probe_s *p, const int16_t *samples,
                         size_t count, uint32_t rms)
{
  if (!p->active || !p->ready)
    return;
  if (rms > p->max_rms)
    p->max_rms = rms;
  for (size_t i = 0; i < count; i++)
    {
      int32_t sample = samples[i];
      uint32_t magnitude = sample < 0 ? -sample : sample;
      if (magnitude > p->peak)
        p->peak = magnitude;
      if (magnitude >= 32760)
        p->clipped++;
      p->squares += (int64_t)sample * sample;
    }
  p->samples += count;
}

/****************************************************************************
 * Name: ny_voice_probe_hit
 ****************************************************************************/
void ny_voice_probe_hit(struct ny_voice_probe_s *p, const char *word,
                        size_t length)
{
  if (!p->active || !p->ready || p->kind != NY_VOICE_PROBE_WAKE)
    return;
  p->detections++;
  if (length >= sizeof(p->word))
    length = sizeof(p->word) - 1;
  memcpy(p->word, word, length);
  p->word[length] = '\0';
}
