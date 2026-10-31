/****************************************************************************
 * app/nyabula_core/ny_voice_sm.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <string.h>

#include "ny_voice_sm.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void ny_voice_sm_act(struct ny_voice_sm_result_s *result,
                            enum ny_voice_action_e type);
static void ny_voice_sm_eyes(struct ny_voice_sm_result_s *result,
                             enum ny_voice_eyes_e eyes);
static void ny_voice_sm_enter(struct ny_voice_sm_s *sm,
                              enum ny_voice_state_e state, uint64_t now_ms);
static void ny_voice_sm_forget(struct ny_voice_sm_s *sm,
                               struct ny_voice_sm_result_s *result);
static void ny_voice_sm_listen(struct ny_voice_sm_s *sm,
                               struct ny_voice_sm_result_s *result,
                               uint64_t now_ms, bool attach);
static void ny_voice_sm_speak(struct ny_voice_sm_s *sm,
                              struct ny_voice_sm_result_s *result,
                              uint64_t now_ms, enum ny_voice_speech_e speech);
static void ny_voice_sm_rest(struct ny_voice_sm_s *sm,
                             struct ny_voice_sm_result_s *result,
                             uint64_t now_ms);
static void ny_voice_sm_abandon(struct ny_voice_sm_s *sm,
                                struct ny_voice_sm_result_s *result);
static void ny_voice_sm_end(struct ny_voice_sm_s *sm,
                            struct ny_voice_sm_result_s *result,
                            uint64_t now_ms);
static void ny_voice_sm_listening(struct ny_voice_sm_s *sm,
                                  const struct ny_voice_sm_event_s *event,
                                  struct ny_voice_sm_result_s *result);
static void ny_voice_sm_thinking(struct ny_voice_sm_s *sm,
                                 const struct ny_voice_sm_event_s *event,
                                 struct ny_voice_sm_result_s *result);
static void ny_voice_sm_speaking(struct ny_voice_sm_s *sm,
                                 const struct ny_voice_sm_event_s *event,
                                 struct ny_voice_sm_result_s *result);
static void ny_voice_sm_idle(struct ny_voice_sm_s *sm,
                             const struct ny_voice_sm_event_s *event,
                             struct ny_voice_sm_result_s *result);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ny_voice_sm_act(struct ny_voice_sm_result_s *result,
                            enum ny_voice_action_e type)
{
  if (result->count < NY_VOICE_SM_MAX_ACTIONS)
    {
      result->actions[result->count++].type = type;
    }
}

static void ny_voice_sm_eyes(struct ny_voice_sm_result_s *result,
                             enum ny_voice_eyes_e eyes)
{
  ny_voice_sm_act(result, NY_VOICE_ACT_EYES);
  result->actions[result->count - 1].eyes = eyes;
}

static void ny_voice_sm_enter(struct ny_voice_sm_s *sm,
                              enum ny_voice_state_e state, uint64_t now_ms)
{
  sm->state = state;
  sm->since_ms = now_ms;
  sm->end_sent_ms = 0;
  sm->asr_started = false;
  sm->end_wanted = false;
}

static void ny_voice_sm_forget(struct ny_voice_sm_s *sm,
                               struct ny_voice_sm_result_s *result)
{
  if (sm->pending)
    {
      sm->pending = false;
      ny_voice_sm_act(result, NY_VOICE_ACT_AGENT_FORGET);
    }
}

static void ny_voice_sm_listen(struct ny_voice_sm_s *sm,
                               struct ny_voice_sm_result_s *result,
                               uint64_t now_ms, bool attach)
{
  /* A new command supersedes a run that still waits for the panel: the run
   * stays where it is, only nobody will speak its outcome any more.
   */

  ny_voice_sm_forget(sm, result);
  ny_voice_sm_enter(sm, NY_VOICE_LISTENING, now_ms);
  ny_voice_sm_eyes(result, NY_VOICE_EYES_LISTENING);
  ny_voice_sm_act(result, NY_VOICE_ACT_ASR_START);
  result->actions[result->count - 1].attach = attach;
}

static void ny_voice_sm_speak(struct ny_voice_sm_s *sm,
                              struct ny_voice_sm_result_s *result,
                              uint64_t now_ms, enum ny_voice_speech_e speech)
{
  ny_voice_sm_enter(sm, NY_VOICE_SPEAKING, now_ms);
  sm->speech = speech;

  /* The short cues are over before an expression change would show. */

  if (speech == NY_VOICE_SPEECH_REPLY || speech == NY_VOICE_SPEECH_TEXT ||
      speech == NY_VOICE_SPEECH_APPROVAL)
    {
      ny_voice_sm_eyes(result, NY_VOICE_EYES_SPEAKING);
    }

  ny_voice_sm_act(result, NY_VOICE_ACT_SPEAK);
  result->actions[result->count - 1].speech = speech;
}

static void ny_voice_sm_rest(struct ny_voice_sm_s *sm,
                             struct ny_voice_sm_result_s *result,
                             uint64_t now_ms)
{
  ny_voice_sm_enter(sm, NY_VOICE_IDLE, now_ms);
  ny_voice_sm_eyes(result, NY_VOICE_EYES_RESTORE);
}

/****************************************************************************
 * Name: ny_voice_sm_abandon
 *
 * Description:
 *   Stop whatever the current state has in flight.  Shared by DISABLE,
 *   CANCEL and barge-in.
 *
 ****************************************************************************/

static void ny_voice_sm_abandon(struct ny_voice_sm_s *sm,
                                struct ny_voice_sm_result_s *result)
{
  switch (sm->state)
    {
      case NY_VOICE_LISTENING:
        ny_voice_sm_act(result, NY_VOICE_ACT_ASR_CANCEL);
        break;

      case NY_VOICE_THINKING:
        ny_voice_sm_act(result, NY_VOICE_ACT_AGENT_CANCEL);
        break;

      case NY_VOICE_SPEAKING:
        ny_voice_sm_act(result, NY_VOICE_ACT_SPEAK_CANCEL);
        break;

      default:
        break;
    }
}

static void ny_voice_sm_end(struct ny_voice_sm_s *sm,
                            struct ny_voice_sm_result_s *result,
                            uint64_t now_ms)
{
  if (sm->end_sent_ms != 0)
    {
      return;
    }

  if (!sm->asr_started)
    {
      /* END names a request; it is sent as soon as there is one. */

      sm->end_wanted = true;
      return;
    }

  sm->end_sent_ms = now_ms != 0 ? now_ms : 1;
  ny_voice_sm_act(result, NY_VOICE_ACT_ASR_END);
}

static void ny_voice_sm_listening(struct ny_voice_sm_s *sm,
                                  const struct ny_voice_sm_event_s *event,
                                  struct ny_voice_sm_result_s *result)
{
  uint64_t elapsed = event->now_ms - sm->since_ms;

  switch (event->type)
    {
      case NY_VOICE_EV_ASR_STARTED:

        /* The clocks of a command start when the recogniser listens, not
         * when the wake word fired: a first wake loads the model first.
         */

        sm->asr_started = true;
        sm->since_ms = event->now_ms;
        if (sm->end_wanted)
          {
            ny_voice_sm_end(sm, result, event->now_ms);
          }

        break;

      case NY_VOICE_EV_ASR_FAILED:
        ny_voice_sm_speak(sm, result, event->now_ms, NY_VOICE_SPEECH_ERROR);
        break;

      case NY_VOICE_EV_ASR_ENDPOINT:

        /* The recognizer also calls a long silence with nothing decoded
         * an endpoint.  That is the owner drawing breath after the wake
         * word, not the end of a command: no_speech_ms decides there.
         */

        if (event->heard_speech)
          {
            ny_voice_sm_end(sm, result, event->now_ms);
          }

        break;

      case NY_VOICE_EV_ASR_FINISHED:
        if (!event->ok || event->empty)
          {
            ny_voice_sm_speak(sm, result, event->now_ms,
                              NY_VOICE_SPEECH_NOT_HEARD);
          }
        else
          {
            ny_voice_sm_enter(sm, NY_VOICE_THINKING, event->now_ms);
            ny_voice_sm_eyes(result, NY_VOICE_EYES_THINKING);
            ny_voice_sm_act(result, NY_VOICE_ACT_AGENT_SUBMIT);
          }

        break;

      case NY_VOICE_EV_TICK:
        if (!sm->asr_started)
          {
            if (elapsed >= sm->config.asr_start_ms)
              {
                ny_voice_sm_act(result, NY_VOICE_ACT_ASR_CANCEL);
                ny_voice_sm_speak(sm, result, event->now_ms,
                                  NY_VOICE_SPEECH_ERROR);
              }
          }
        else if (sm->end_sent_ms != 0)
          {
            /* FINISH is never shed by the service; if it still does not
             * come the request is given up rather than the robot staying
             * deaf in LISTENING for good.
             */

            if (event->now_ms - sm->end_sent_ms >= sm->config.asr_finish_ms)
              {
                ny_voice_sm_act(result, NY_VOICE_ACT_ASR_CANCEL);
                ny_voice_sm_speak(sm, result, event->now_ms,
                                  NY_VOICE_SPEECH_NOT_HEARD);
              }
          }
        else if ((event->heard_speech &&
                  event->silence_ms >= sm->config.trailing_ms) ||
                 (!event->heard_speech &&
                  elapsed >= sm->config.no_speech_ms) ||
                 elapsed >= sm->config.max_listen_ms)
          {
            ny_voice_sm_end(sm, result, event->now_ms);
          }

        break;

      case NY_VOICE_EV_LINK_LOST:

        /* The request died with the daemon that held it. */

        ny_voice_sm_rest(sm, result, event->now_ms);
        break;

      case NY_VOICE_EV_WAKE:
      case NY_VOICE_EV_LISTEN:
        break; /* Already listening. */

      default:
        result->accepted = false;
        break;
    }
}

static void ny_voice_sm_thinking(struct ny_voice_sm_s *sm,
                                 const struct ny_voice_sm_event_s *event,
                                 struct ny_voice_sm_result_s *result)
{
  switch (event->type)
    {
      case NY_VOICE_EV_AGENT_REPLY:
        ny_voice_sm_speak(sm, result, event->now_ms,
                          event->ok && !event->empty ? NY_VOICE_SPEECH_REPLY
                                                     : NY_VOICE_SPEECH_ERROR);
        break;

      case NY_VOICE_EV_AGENT_PENDING:

        /* Approval is given on the panel, never by voice: a voice cannot
         * be told from the owner's.  The run is followed so that its
         * outcome is still spoken once the owner has decided.
         */

        sm->pending = true;
        sm->pending_ms = event->now_ms;
        ny_voice_sm_speak(sm, result, event->now_ms, NY_VOICE_SPEECH_APPROVAL);
        break;

      case NY_VOICE_EV_AGENT_BUSY:
        ny_voice_sm_speak(sm, result, event->now_ms, NY_VOICE_SPEECH_BUSY);
        break;

      case NY_VOICE_EV_AGENT_FAILED:
        ny_voice_sm_speak(sm, result, event->now_ms, NY_VOICE_SPEECH_ERROR);
        break;

      case NY_VOICE_EV_TICK:
        if (event->now_ms - sm->since_ms >= sm->config.think_ms)
          {
            ny_voice_sm_act(result, NY_VOICE_ACT_AGENT_CANCEL);
            ny_voice_sm_speak(sm, result, event->now_ms,
                              NY_VOICE_SPEECH_ERROR);
          }

        break;

      case NY_VOICE_EV_WAKE:
      case NY_VOICE_EV_LISTEN:

        /* The owner asks again instead of waiting for the answer. */

        ny_voice_sm_act(result, NY_VOICE_ACT_AGENT_CANCEL);
        ny_voice_sm_listen(sm, result, event->now_ms,
                           event->type == NY_VOICE_EV_WAKE);
        break;

      case NY_VOICE_EV_LINK_LOST:
        break; /* The agent may be answering from the cloud. */

      default:
        result->accepted = false;
        break;
    }
}

static void ny_voice_sm_speaking(struct ny_voice_sm_s *sm,
                                 const struct ny_voice_sm_event_s *event,
                                 struct ny_voice_sm_result_s *result)
{
  switch (event->type)
    {
      case NY_VOICE_EV_SPEAK_DONE:
        ny_voice_sm_rest(sm, result, event->now_ms);
        break;

      case NY_VOICE_EV_WAKE:
      case NY_VOICE_EV_LISTEN:

        /* Barge-in: the reply is dropped where it stands. */

        ny_voice_sm_act(result, NY_VOICE_ACT_SPEAK_CANCEL);
        ny_voice_sm_listen(sm, result, event->now_ms,
                           event->type == NY_VOICE_EV_WAKE);
        break;

      case NY_VOICE_EV_LINK_LOST:
        ny_voice_sm_act(result, NY_VOICE_ACT_SPEAK_CANCEL);
        ny_voice_sm_rest(sm, result, event->now_ms);
        break;

      case NY_VOICE_EV_TICK:
        if (event->now_ms - sm->since_ms >= sm->config.speak_ms)
          {
            ny_voice_sm_act(result, NY_VOICE_ACT_SPEAK_CANCEL);
            ny_voice_sm_rest(sm, result, event->now_ms);
          }

        break;

      case NY_VOICE_EV_AGENT_REPLY:
      case NY_VOICE_EV_AGENT_FAILED:

        /* The followed run ended while its approval sentence still plays;
         * two voices at once help nobody, the panel shows the outcome.
         */

        sm->pending = false;
        break;

      default:
        result->accepted = false;
        break;
    }
}

static void ny_voice_sm_idle(struct ny_voice_sm_s *sm,
                             const struct ny_voice_sm_event_s *event,
                             struct ny_voice_sm_result_s *result)
{
  switch (event->type)
    {
      case NY_VOICE_EV_WAKE:
      case NY_VOICE_EV_LISTEN:
        ny_voice_sm_listen(sm, result, event->now_ms,
                           event->type == NY_VOICE_EV_WAKE);
        break;

      case NY_VOICE_EV_SAY:
        ny_voice_sm_speak(sm, result, event->now_ms, NY_VOICE_SPEECH_TEXT);
        break;

      case NY_VOICE_EV_AGENT_REPLY:
        if (!sm->pending)
          {
            result->accepted = false;
            break;
          }

        sm->pending = false;
        if (event->ok && !event->empty)
          {
            ny_voice_sm_speak(sm, result, event->now_ms,
                              NY_VOICE_SPEECH_REPLY);
          }

        break;

      case NY_VOICE_EV_AGENT_FAILED:
        sm->pending = false;
        break;

      case NY_VOICE_EV_TICK:
        if (sm->pending &&
            event->now_ms - sm->pending_ms >= sm->config.approval_ms)
          {
            ny_voice_sm_forget(sm, result);
          }

        break;

      case NY_VOICE_EV_LINK_LOST:
        break;

      default:
        result->accepted = false;
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ny_voice_sm_defaults(struct ny_voice_sm_config_s *config)
{
  config->max_listen_ms = 8000;
  config->trailing_ms = 1600;
  config->no_speech_ms = 5000;
  config->asr_start_ms = 90000;
  config->asr_finish_ms = 6000;
  config->think_ms = 120000;
  config->approval_ms = 600000;
  config->speak_ms = 180000;
}

void ny_voice_sm_init(struct ny_voice_sm_s *sm,
                      const struct ny_voice_sm_config_s *config)
{
  memset(sm, 0, sizeof(*sm));
  sm->config = *config;
  sm->state = NY_VOICE_OFF;
}

void ny_voice_sm_step(struct ny_voice_sm_s *sm,
                      const struct ny_voice_sm_event_s *event,
                      struct ny_voice_sm_result_s *result)
{
  memset(result, 0, sizeof(*result));
  result->accepted = true;

  /* The three events that mean the same in every state. */

  if (event->type == NY_VOICE_EV_DISABLE)
    {
      if (sm->state != NY_VOICE_OFF)
        {
          ny_voice_sm_abandon(sm, result);
          ny_voice_sm_forget(sm, result);
          if (sm->state != NY_VOICE_IDLE)
            {
              ny_voice_sm_eyes(result, NY_VOICE_EYES_RESTORE);
            }

          ny_voice_sm_enter(sm, NY_VOICE_OFF, event->now_ms);
        }

      return;
    }

  if (event->type == NY_VOICE_EV_ENABLE)
    {
      if (sm->state == NY_VOICE_OFF)
        {
          ny_voice_sm_enter(sm, NY_VOICE_IDLE, event->now_ms);
        }

      return;
    }

  if (event->type == NY_VOICE_EV_CANCEL && sm->state != NY_VOICE_OFF)
    {
      ny_voice_sm_abandon(sm, result);
      ny_voice_sm_forget(sm, result);
      if (sm->state != NY_VOICE_IDLE)
        {
          ny_voice_sm_rest(sm, result, event->now_ms);
        }

      return;
    }

  switch (sm->state)
    {
      case NY_VOICE_IDLE:
        ny_voice_sm_idle(sm, event, result);
        break;

      case NY_VOICE_LISTENING:
        ny_voice_sm_listening(sm, event, result);
        break;

      case NY_VOICE_THINKING:
        ny_voice_sm_thinking(sm, event, result);
        break;

      case NY_VOICE_SPEAKING:
        ny_voice_sm_speaking(sm, event, result);
        break;

      default:
        result->accepted = false;
        break;
    }
}

const char *ny_voice_sm_state_name(enum ny_voice_state_e state)
{
  static const char *const names[] = { "off", "idle", "listening", "thinking",
                                       "speaking" };

  return (unsigned int)state < sizeof(names) / sizeof(names[0]) ? names[state]
                                                                : "unknown";
}

bool ny_voice_sm_streaming(const struct ny_voice_sm_s *sm, bool duplex,
                           bool barge_in)
{
  switch (sm->state)
    {
      case NY_VOICE_IDLE:
      case NY_VOICE_LISTENING:
      case NY_VOICE_THINKING:
        return true;

      case NY_VOICE_SPEAKING:

        /* Without echo cancellation the microphone hears the reply.  It is
         * streamed only when the codec can do both at once and the owner
         * chose barge-in over the risk of the robot waking itself.
         */

        return duplex && barge_in;

      default:
        return false;
    }
}
