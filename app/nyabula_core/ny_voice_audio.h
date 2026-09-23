/****************************************************************************
 * app/nyabula_core/ny_voice_audio.h
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

#ifndef __NYABULA_CORE_NY_VOICE_AUDIO_H
#define __NYABULA_CORE_NY_VOICE_AUDIO_H

/****************************************************************************
 * A live 16 bit PCM stream on a NuttX audio node, for the voice chain: the
 * microphone that never stops, and speech that is synthesized while it
 * plays.  The file player cannot do either -- it ends a stream at the first
 * short read -- so the nodes are driven with the audio ioctls directly.
 *
 * The Bluetooth service carries a wrapper of the same kind
 * (ny_audio_stream.c); it is compiled only with NYABULA_CORE_BT and was
 * still changing when this was written, so the voice chain does not depend
 * on it.  The two are candidates for a merge.
 *
 * One thread per stream; nothing here is locked.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_voice_audio_s; /* Opaque */

struct ny_voice_audio_config_s
{
  const char *path; /* /dev/audio/...                                    */
  bool capture;
  bool wav; /* Playback through the PCM decoder node, which takes
             * the stream format from a WAV header: one is sent
             * ahead of the samples.
             */
  uint32_t rate;
  uint8_t channels;
  size_t chunk; /* Bytes handed to the driver at a time.  The capture
                 * driver fills a whole buffer before returning it, so
                 * this is the capture latency.
                 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ny_voice_audio_open
 *
 * Description:
 *   Reserve and configure the node.  Capture starts at once, playback with
 *   the first full chunk.  -EBUSY when another player holds the node, or
 *   when the codec's other direction runs a different rate, width or channel
 *   count (es8388_check_peer_format).
 *
 ****************************************************************************/

int ny_voice_audio_open(struct ny_voice_audio_s **audio,
                        const struct ny_voice_audio_config_s *config);

/* Bytes read, fewer than asked (possibly 0) on a timeout, or a negated
 * errno; -EPIPE once the driver has ended the stream.
 */

ssize_t ny_voice_audio_read(struct ny_voice_audio_s *audio, void *pcm,
                            size_t bytes, int timeout_ms);

/* Bytes taken.  When every chunk is queued the call waits for the driver to
 * return one, which is what paces a producer to the codec clock.
 */

ssize_t ny_voice_audio_write(struct ny_voice_audio_s *audio, const void *pcm,
                             size_t bytes, int timeout_ms);

/* Playback: bytes written that the driver has not played yet. */

size_t ny_voice_audio_queued(struct ny_voice_audio_s *audio);

/* Playback: pad the chunk being filled with silence, queue it and wait
 * until everything has been played.
 */

int ny_voice_audio_drain(struct ny_voice_audio_s *audio, int timeout_ms);

/* Stop, take the chunks back from the driver, release the node.  Safe with
 * NULL.  Whatever is still queued is dropped, which is what a barge-in
 * wants.
 */

void ny_voice_audio_close(struct ny_voice_audio_s *audio);

#endif /* __NYABULA_CORE_NY_VOICE_AUDIO_H */
