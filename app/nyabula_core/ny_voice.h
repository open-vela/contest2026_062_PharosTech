/****************************************************************************
 * app/nyabula_core/ny_voice.h
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

#ifndef __NYABULA_CORE_NY_VOICE_H
#define __NYABULA_CORE_NY_VOICE_H

/****************************************************************************
 * The control-domain half of the voice assistant.
 *
 *   "ni hao openvela" -> listen -> transcribe -> agent -> speak
 *
 * The compute domain runs the models (wake word, ASR, TTS; see
 * tools/amp/nyampd); this side owns the microphone, the speaker, the turn
 * and the owner's agent:
 *
 *   ny_voice_capture  the microphone: S16 into a 4 s pre-roll ring, an
 *                     energy gate so that silence is not streamed for ever
 *   ny_voice_pump     ring -> CAPTURE-slot windows -> KWS_PUSH, DETECTED
 *   ny_voice_sm       the turn (ny_voice_sm.c), ASR attached to the wake
 *                     word stream, the agent run, model loading
 *   ny_voice_play     TTS sentence by sentence, PCM windows to the speaker
 *   ny_voice_eyes     expression requests: eyes.expression is a confirmed
 *                     call of up to a second and must never run on a thread
 *                     that moves audio
 *
 * The topic entry point (voice.*) is declared in ny_product.h with the other
 * topic handlers.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#ifdef CONFIG_NYABULA_CORE_VOICE

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: ny_voice_start / ny_voice_stop
 *
 * Description:
 *   Start or stop the voice task.  It runs for as long as the compute link
 *   does -- ny_compute_start() and ny_compute_stop() call these -- and sits
 *   idle, with every device closed, while voice is disabled (voice.enable,
 *   persisted, off until the owner turns it on).  Start is idempotent.
 *
 ****************************************************************************/

int ny_voice_start(void);
int ny_voice_stop(void);
bool ny_voice_running(void);

/****************************************************************************
 * Name: ny_voice_speaker_claim / ny_voice_speaker_release
 *
 * Description:
 *   For the other users of the codec.  The ES8388 driver refuses a playback
 *   format that differs from the capture format while the microphone is
 *   reserved, so whoever is about to open the speaker in a format of its
 *   own (the flash player, Bluetooth audio) calls claim first: the
 *   microphone is closed before claim returns, the wake word is deaf until
 *   release.  Both are cheap when voice is disabled or not running, and
 *   claim is not needed at all in the NYABULA_CORE_VOICE_CODEC_SHARED_44K
 *   arrangement as long as the other stream is 44.1 kHz stereo.
 *
 * Returned Value:
 *   claim: zero, or -ETIMEDOUT when the microphone did not close in time.
 *
 ****************************************************************************/

int ny_voice_speaker_claim(void);
void ny_voice_speaker_release(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_NYABULA_CORE_VOICE */
#endif /* __NYABULA_CORE_NY_VOICE_H */
