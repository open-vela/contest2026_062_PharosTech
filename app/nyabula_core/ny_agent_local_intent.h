/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_local_intent.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
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

/* The part of the on-device transport that needs neither the board nor the
 * model: what the owner asked for, which tool call that is, and what a tool
 * result means in one line of plain text.  Nothing here touches the product
 * services, so the same file is compiled and exercised on the host.
 */

#ifndef __NYABULA_CORE_NY_AGENT_LOCAL_INTENT_H
#define __NYABULA_CORE_NY_AGENT_LOCAL_INTENT_H

#include <netutils/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longer than this is a story, not a command.  It also bounds every
 * synthesized argument object well under the executor's 2048-byte input.
 */

#define NY_AGENT_LOCAL_UTTERANCE_MAX 512
#define NY_AGENT_LOCAL_TEXT_MAX      NY_AGENT_LOCAL_UTTERANCE_MAX
#define NY_AGENT_LOCAL_FACT_MAX      640
#define NY_AGENT_LOCAL_RAW_MAX       300 /* Of a result no rule understands */
#define NY_AGENT_LOCAL_VOLUME_STEP   15

enum ny_agent_local_intent_e
{
  NY_AGENT_LOCAL_CHAT = 0,
  NY_AGENT_LOCAL_TIME,
  NY_AGENT_LOCAL_DATE,
  NY_AGENT_LOCAL_WEATHER,
  NY_AGENT_LOCAL_STATUS,
  NY_AGENT_LOCAL_TIMER_LIST,
  NY_AGENT_LOCAL_ALARM_LIST,
  NY_AGENT_LOCAL_TASK_LIST,
  NY_AGENT_LOCAL_EXPRESSION,
  NY_AGENT_LOCAL_TIMER,
  NY_AGENT_LOCAL_ALARM,
  NY_AGENT_LOCAL_VOLUME_SET,
  NY_AGENT_LOCAL_VOLUME_UP,
  NY_AGENT_LOCAL_VOLUME_DOWN,
  NY_AGENT_LOCAL_MUTE,
  NY_AGENT_LOCAL_UNMUTE,
  NY_AGENT_LOCAL_MUSIC_PLAY,
  NY_AGENT_LOCAL_MUSIC_PAUSE,
  NY_AGENT_LOCAL_MUSIC_RESUME,
  NY_AGENT_LOCAL_MUSIC_STOP,
  NY_AGENT_LOCAL_REMEMBER,
  NY_AGENT_LOCAL_TASK
};

struct ny_agent_local_intent_s
{
  enum ny_agent_local_intent_e kind;
  bool complete;  /* Every slot the tool call needs was found.           */
  bool numerals;  /* The words carry a quantity a model could read.      */
  bool far_date;  /* An alarm for a day alarms cannot express.           */
  bool ambiguous; /* Clock time with no am/pm hint: 7 is 07:00 or 19:00. */
  uint32_t seconds;       /* TIMER                                       */
  int percent;            /* VOLUME_SET level; VOLUME_UP/DOWN step       */
  int hour;               /* ALARM                                       */
  int minute;
  unsigned int repeat;    /* ALARM: bit 0 is Sunday                      */
  const char *expression; /* EXPRESSION: one of the 13 names             */
  char text[NY_AGENT_LOCAL_TEXT_MAX]; /* Label, memory, title, track hint */
};

/* What only the running product knows; the transport fills it in. */

struct ny_agent_local_context_s
{
  double revision;        /* Of the list the create is checked against   */
  int utc_offset_minutes;
  int now_minutes;        /* Local minute of the day; -1: clock not set  */
  int volume;
  bool muted;
  const char *track;      /* MUSIC_PLAY: a file name from the library    */
};

/* Never fails: what it cannot place is NY_AGENT_LOCAL_CHAT. */

void ny_agent_local_intent_detect(const char *utterance,
                                  struct ny_agent_local_intent_s *intent);
const char *ny_agent_local_intent_name(enum ny_agent_local_intent_e kind);

/* The *.list topic whose "revision" the create needs, or NULL. */

const char *ny_agent_local_intent_revision(enum ny_agent_local_intent_e kind);

/* The one-tool table for the forced model call that reads a missing slot,
 * or NULL when the model has nothing to read it from.
 */

const char *ny_agent_local_intent_forced(
    const struct ny_agent_local_intent_s *intent, const char **name);
int ny_agent_local_intent_fill(struct ny_agent_local_intent_s *intent,
                               const char *name, const cJSON *arguments);
const char *ny_agent_local_intent_question(
    const struct ny_agent_local_intent_s *intent);

/* Pick a playable file for a spoken hint; -ENOENT when the library has
 * nothing to play.
 */

int ny_agent_local_intent_track(const cJSON *library, const char *hint,
                                char *name, size_t size);

/* The arguments the executor accepts, malloc'd; *tool names the tool. */

char *ny_agent_local_intent_call(
    const struct ny_agent_local_intent_s *intent,
    const struct ny_agent_local_context_s *context, const char **tool);

/* A chat.completion body, malloc'd: a tool call when `tool` is set, else
 * `text` as the assistant's answer.
 */

char *ny_agent_local_intent_reply(const char *tool, const char *arguments,
                                  const char *text, uint64_t unique);

/* One line of plain text for a tool result.  Returns true when the line
 * reports a failure.
 */

bool ny_agent_local_intent_fact(const char *tool, const char *arguments,
                                const char *result, int utc_offset_minutes,
                                char *fact, size_t size);

/* snprintf("%s") that never cuts a UTF-8 sequence; returns bytes written. */

size_t ny_agent_local_intent_copy(char *out, size_t size, const char *text,
                                  size_t length);

#endif
