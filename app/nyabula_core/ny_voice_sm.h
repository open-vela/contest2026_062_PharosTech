/****************************************************************************
 * app/nyabula_core/ny_voice_sm.h
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

#ifndef __NYABULA_CORE_NY_VOICE_SM_H
#define __NYABULA_CORE_NY_VOICE_SM_H

/****************************************************************************
 * The voice turn as a pure transition function.
 *
 *   OFF -> IDLE -> LISTENING -> THINKING -> SPEAKING -> IDLE
 *
 * ny_voice_sm_step() takes the machine, one event and the time, and returns
 * the next state plus an ordered list of actions for ny_voice.c to carry
 * out.  It touches no device, no lock and no clock, so a scripted event
 * list on the host exercises every path, including the ones a board makes
 * hard to reach: a compute domain that restarts in the middle of a turn, an
 * ASR request whose FINISH never comes, a wake word on top of a reply.
 *
 * The machine does not hold text.  The transcript and the reply belong to
 * the service; the machine only needs to know whether they are empty.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_VOICE_SM_MAX_ACTIONS 6

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum ny_voice_state_e
{
  NY_VOICE_OFF = 0,
  NY_VOICE_IDLE,
  NY_VOICE_LISTENING,
  NY_VOICE_THINKING,
  NY_VOICE_SPEAKING
};

enum ny_voice_event_e
{
  NY_VOICE_EV_TICK = 0, /* Time passed; carries the VAD's view.       */
  NY_VOICE_EV_ENABLE,
  NY_VOICE_EV_DISABLE,
  NY_VOICE_EV_WAKE,          /* KWS DETECTED.                              */
  NY_VOICE_EV_LISTEN,        /* voice.listen: push-to-talk from the panel. */
  NY_VOICE_EV_CANCEL,        /* voice.cancel.                              */
  NY_VOICE_EV_SAY,           /* voice.say.                                 */
  NY_VOICE_EV_ASR_STARTED,   /* The ASR request was accepted.              */
  NY_VOICE_EV_ASR_FAILED,    /* It could not be started.                   */
  NY_VOICE_EV_ASR_ENDPOINT,  /* PARTIAL with the ENDPOINT flag.            */
  NY_VOICE_EV_ASR_FINISHED,  /* FINISH; `ok` and `empty` describe it.      */
  NY_VOICE_EV_AGENT_REPLY,   /* The run ended; `ok`, `empty`.              */
  NY_VOICE_EV_AGENT_PENDING, /* The run waits for the owner's approval.    */
  NY_VOICE_EV_AGENT_BUSY,    /* Another run holds the agent.               */
  NY_VOICE_EV_AGENT_FAILED,  /* The turn could not be submitted.           */
  NY_VOICE_EV_SPEAK_DONE,    /* Playback ended, cancelled or failed.       */
  NY_VOICE_EV_LINK_LOST      /* New compute generation, or no link.        */
};

enum ny_voice_action_e
{
  NY_VOICE_ACT_NONE = 0,
  NY_VOICE_ACT_ASR_START, /* `attach` tells whether a wake word led.    */
  NY_VOICE_ACT_ASR_END,
  NY_VOICE_ACT_ASR_CANCEL,
  NY_VOICE_ACT_AGENT_SUBMIT,
  NY_VOICE_ACT_AGENT_CANCEL,
  NY_VOICE_ACT_AGENT_FORGET, /* Stop following a run awaiting approval.    */
  NY_VOICE_ACT_SPEAK,        /* `speech` tells what.                       */
  NY_VOICE_ACT_SPEAK_CANCEL,
  NY_VOICE_ACT_EYES /* `eyes` tells which expression.             */
};

enum ny_voice_speech_e
{
  NY_VOICE_SPEECH_REPLY = 0, /* The agent's reply.                         */
  NY_VOICE_SPEECH_TEXT,      /* voice.say.                                 */
  NY_VOICE_SPEECH_NOT_HEARD, /* Short cue: nothing was understood.         */
  NY_VOICE_SPEECH_APPROVAL,  /* Fixed sentence: confirm on the panel.      */
  NY_VOICE_SPEECH_BUSY,      /* Short cue: the agent is taken.             */
  NY_VOICE_SPEECH_ERROR      /* Short cue: the turn failed.                */
};

enum ny_voice_eyes_e
{
  NY_VOICE_EYES_RESTORE = 0, /* idle      */
  NY_VOICE_EYES_LISTENING,   /* curious   */
  NY_VOICE_EYES_THINKING,    /* processing */
  NY_VOICE_EYES_SPEAKING     /* happy     */
};

struct ny_voice_sm_config_s
{
  uint32_t max_listen_ms; /* A command is cut off after this long.     */
  uint32_t trailing_ms;   /* Silence that ends a command.              */
  uint32_t no_speech_ms;  /* Nothing at all was said.                  */
  uint32_t asr_start_ms;  /* ASR start (incl. a first load) must end.  */
  uint32_t asr_finish_ms; /* FINISH must follow END within this.       */
  uint32_t think_ms;      /* The agent's time for a turn.              */
  uint32_t approval_ms;   /* How long a pending run is followed.       */
  uint32_t speak_ms;      /* Playback must end within this.            */
};

struct ny_voice_sm_event_s
{
  enum ny_voice_event_e type;
  uint64_t now_ms;
  bool ok;             /* ASR_FINISHED, AGENT_REPLY, SPEAK_DONE.        */
  bool empty;          /* ASR_FINISHED, AGENT_REPLY: no text.           */
  bool heard_speech;   /* TICK: the VAD saw speech since ASR started.   */
  uint32_t silence_ms; /* TICK: since the VAD last saw speech.          */
};

struct ny_voice_sm_action_s
{
  enum ny_voice_action_e type;
  enum ny_voice_speech_e speech;
  enum ny_voice_eyes_e eyes;
  bool attach;
};

struct ny_voice_sm_s
{
  struct ny_voice_sm_config_s config;
  enum ny_voice_state_e state;
  uint64_t since_ms;    /* When the state was entered.                   */
  uint64_t end_sent_ms; /* LISTENING: when ASR_END went out, else 0.     */
  uint64_t pending_ms;  /* When a run began to wait for approval.        */
  bool asr_started;     /* LISTENING: the request exists.                */
  bool end_wanted;      /* LISTENING: END is due once it exists.         */
  bool pending;         /* A run awaiting approval is being followed.    */
  enum ny_voice_speech_e speech; /* SPEAKING: what.                        */
};

struct ny_voice_sm_result_s
{
  bool accepted; /* False: the event had no meaning in this state. */
  unsigned int count;
  struct ny_voice_sm_action_s actions[NY_VOICE_SM_MAX_ACTIONS];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

void ny_voice_sm_defaults(struct ny_voice_sm_config_s *config);
void ny_voice_sm_init(struct ny_voice_sm_s *sm,
                      const struct ny_voice_sm_config_s *config);
void ny_voice_sm_step(struct ny_voice_sm_s *sm,
                      const struct ny_voice_sm_event_s *event,
                      struct ny_voice_sm_result_s *result);
const char *ny_voice_sm_state_name(enum ny_voice_state_e state);

/****************************************************************************
 * Name: ny_voice_sm_streaming
 *
 * Description:
 *   Whether captured audio goes to the wake word stream in this state.
 *   `duplex` says that the codec can capture while it plays; `barge_in`
 *   that the owner accepts the robot hearing itself while it speaks.
 *
 ****************************************************************************/

bool ny_voice_sm_streaming(const struct ny_voice_sm_s *sm, bool duplex,
                           bool barge_in);

#ifdef __cplusplus
}
#endif

#endif /* __NYABULA_CORE_NY_VOICE_SM_H */
