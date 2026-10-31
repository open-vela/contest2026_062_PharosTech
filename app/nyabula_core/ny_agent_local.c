/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_local.c
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

/* The on-device model as one more backend of the agent's model router.
 *
 * The agent framework speaks the OpenAI chat-completions format to whatever
 * backend the router picks.  The model in the compute domain is reached
 * through a transport registered for a reserved host name, so routing,
 * failover, tool execution and approvals are the code the cloud backends
 * already exercise; only the last hop differs.
 *
 * What differs is what a 1B model with a 2048-token window can be asked to
 * do.  Measured on the board it chats well, and with exactly one tool on
 * offer it reads a parameter out of a sentence; shown a tool table -- the
 * owner's 1070-token one or a 4-tool one -- it describes the call instead of
 * making it.  So a request is never forwarded as it came.  It becomes one
 * of three things:
 *
 *   a tool call made by rule    the last message is the owner's and
 *                               ny_agent_local_intent.c recognises a
 *                               command.  The model is consulted only for a
 *                               slot the rules could not read, with that one
 *                               tool forced.  The response is synthesized
 *                               here; the framework executes it, approvals
 *                               included, as if a cloud model had asked.
 *   one sentence about a result the last message is a tool result.  It is
 *                               rendered to a line of plain text by rule and
 *                               sent as it is: the model alters numbers
 *                               when asked to rephrase them.
 *   chat                        a persona, the text turns of the history
 *                               that fit, and no tools.
 */

#include "ny_agent.h"
#include <nuttx/config.h>

#if defined(CONFIG_NYABULA_CORE_AGENT) && defined(CONFIG_NYABULA_CORE_COMPUTE)

#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "ny_agent_local_intent.h"
#include "ny_compute.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* Reserved: ".local" never resolves through DNS, so a request for it that
 * escaped the transport could not reach a third party.
 */

#define NY_AGENT_LOCAL_HOST    "nyabula.local"
#define NY_AGENT_LOCAL_PATH    "/v1/chat/completions"
#define NY_AGENT_LOCAL_PORT    "1"
#define NY_AGENT_LOCAL_KEY     "on-device" /* The framework wants one */
#define NY_AGENT_LOCAL_SLOT    (LLM_ROUTER_MAX_BACKENDS - 1)
#define NY_AGENT_LOCAL_WAIT    180000
#define NY_AGENT_LOCAL_TRIMS   8
#define NY_AGENT_LOCAL_CALLER  "nyabot-local"

/* New tokens per kind of call.  A forced call is one short JSON object and
 * a summary is one sentence; letting either run on only costs the owner
 * seconds at 25 tokens a second.
 */

#define NY_AGENT_LOCAL_TOKENS_CHAT   384
#define NY_AGENT_LOCAL_TOKENS_FORCED 64

/* History kept for chat: about 1200 tokens, estimated at 2.5 bytes a token
 * for mixed Chinese and English.  With the persona and the answer that
 * stays inside the window; -E2BIG trimming covers a wrong estimate.
 */

#define NY_AGENT_LOCAL_HISTORY  3000
#define NY_AGENT_LOCAL_WISHES   360 /* Of the profile's free-text wishes */
#define NY_AGENT_LOCAL_FACTS    3   /* Parallel results worth one sentence */

/* No product setting holds a time zone: the panel sends the browser's
 * offset with each alarm, briefing and companion schedule.  The newest of
 * those is used; with none, the device is assumed to be where it was built.
 */

#define NY_AGENT_LOCAL_UTC_OFFSET 480

/* Marks the owner's tool table.  An external conversation carries its own
 * granted list and gets chat only: rules act with the owner's authority.
 */

#define NY_AGENT_LOCAL_OWNER_TOOL "nyabula_mcp_catalog"

static uint32_t g_agent_local_serial;

static const char *ny_agent_local_text(const cJSON *object, const char *key);
static bool ny_agent_local_owner(const cJSON *tools);
static int ny_agent_local_ask(const char *topic, cJSON **result);
static int ny_agent_local_offset(const cJSON *alarms);
static void ny_agent_local_persona(bool owner, bool chat, char *text,
                                   size_t size);
static bool ny_agent_local_add(cJSON *messages, const char *role,
                               const char *content);
static bool ny_agent_local_trim(cJSON *messages);
static int ny_agent_local_generate(cJSON *root, size_t tokens,
                                   const char *label, char **response);
static int ny_agent_local_force(const char *utterance,
                                struct ny_agent_local_intent_s *intent);
static int ny_agent_local_gather(const struct ny_agent_local_intent_s *intent,
                                 struct ny_agent_local_context_s *context,
                                 char *track, size_t size);
static int ny_agent_local_command(const char *utterance, char **response);
static int ny_agent_local_report(const cJSON *messages, int last, bool owner,
                                 char **response);
static int ny_agent_local_chat(const cJSON *messages, bool owner,
                               char **response);
static char *ny_agent_local_error(const char *message);
static int ny_agent_local_transport(const char *request, char **response,
                                    int *status, void *arg);

/****************************************************************************
 * Name: ny_agent_local_text
 ****************************************************************************/

static const char *ny_agent_local_text(const cJSON *object, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

/****************************************************************************
 * Name: ny_agent_local_owner
 ****************************************************************************/

static bool ny_agent_local_owner(const cJSON *tools)
{
  const cJSON *tool;
  cJSON_ArrayForEach(tool, tools)
  {
    const cJSON *function = cJSON_GetObjectItemCaseSensitive(tool, "function");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(
        function ? function : tool, "name");
    if (cJSON_IsString(name) &&
        !strcmp(name->valuestring, NY_AGENT_LOCAL_OWNER_TOOL))
      return true;
  }
  return false;
}

/****************************************************************************
 * Name: ny_agent_local_ask
 * Description: Read a product topic the way the tool executor would, as the
 *   owner.  Only reads go through here; every write is a tool call the
 *   framework executes, so approvals and the run's step log stay in force.
 ****************************************************************************/

static int ny_agent_local_ask(const char *topic, cJSON **result)
{
  const struct ny_product_caller_s caller = { NY_AGENT_LOCAL_CALLER,
                                              NY_PRODUCT_OWNER, false };
  cJSON *empty = cJSON_CreateObject();
  int ret = empty ? ny_product_request(&caller, topic, empty, result)
                  : -ENOMEM;
  cJSON_Delete(empty);
  if (ret == 0 && !cJSON_IsObject(*result))
    {
      cJSON_Delete(*result);
      *result = NULL;
      ret = -EBADMSG;
    }
  return ret;
}

/****************************************************************************
 * Name: ny_agent_local_offset
 * Description: Minutes east of UTC.  `alarms` is an alarm.list result the
 *   caller already has, or NULL to fetch one.
 ****************************************************************************/

static int ny_agent_local_offset(const cJSON *alarms)
{
  static const char *const topics[] = { "briefing.get", "companion.get" };
  cJSON *fetched = NULL;
  double offset = NAN;
  if (!alarms && ny_agent_local_ask("alarm.list", &fetched) == 0)
    alarms = fetched;
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(alarms, "items");
  const cJSON *newest = cJSON_GetArrayItem(items,
                                           cJSON_GetArraySize(items) - 1);
  const cJSON *value =
      cJSON_GetObjectItemCaseSensitive(newest, "utc_offset_minutes");
  if (cJSON_IsNumber(value))
    offset = value->valuedouble;
  cJSON_Delete(fetched);

  /* Both schedules default to 0 before the owner saves them, so a zero
   * there says nothing about where the device is.
   */

  for (size_t i = 0; !isfinite(offset) && i < 2; i++)
    {
      cJSON *settings = NULL;
      if (ny_agent_local_ask(topics[i], &settings) == 0)
        {
          value = cJSON_GetObjectItemCaseSensitive(settings,
                                                   "utc_offset_minutes");
          if (cJSON_IsNumber(value) && value->valuedouble != 0)
            offset = value->valuedouble;
        }
      cJSON_Delete(settings);
    }
  return isfinite(offset) && offset >= -720 && offset <= 840
             ? (int)offset
             : NY_AGENT_LOCAL_UTC_OFFSET;
}

/****************************************************************************
 * Name: ny_agent_local_persona
 * Description: The system prompt, in sentences.  Given the profile as JSON
 *   the model recited the JSON back, so each field becomes a clause and
 *   nothing in the prompt looks like data.
 ****************************************************************************/

static void ny_agent_local_persona(bool owner, bool chat, char *text,
                                   size_t size)
{
  static const char *const tones[][2] = {
    { "warm", "温暖、体贴，像家人一样陪伴" },
    { "playful", "活泼俏皮，可以撒娇" },
    { "concise", "简洁直接，先说结论" },
    { "calm", "平静舒缓，不急不躁" }
  };
  cJSON *profile = NULL;
  char piece[NY_AGENT_LOCAL_WISHES + 1];
  int used;
  if (!owner)
    {
      snprintf(text, size,
               "你是 Nyabot，正在一段独立的外部对话里回答访客。你只知道这段"
               "对话里的内容，不能查看主人的私人数据，也不能操作设备。用简短"
               "自然的口语回答，对方用什么语言你就用什么语言。");
      return;
    }
  if (ny_agent_profile("agent.profile.get", NULL, &profile) < 0)
    profile = NULL;
  const char *name = ny_agent_local_text(profile, "catName");
  const char *master = ny_agent_local_text(profile, "ownerName");
  const char *tone = ny_agent_local_text(profile, "tone");
  const char *language = ny_agent_local_text(profile, "language");
  const char *wishes = ny_agent_local_text(profile, "instructions");
  const char *style = tone[0] ? tone : tones[0][1];
  for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++)
    if (!strcmp(tone, tones[i][0]))
      style = tones[i][1];
  ny_agent_local_intent_copy(piece, 97, name[0] ? name : "Nyabula",
                             strlen(name[0] ? name : "Nyabula"));
  used = snprintf(text, size,
                  "你是%s，住在 Nyabula 猫咪机器人里的陪伴伙伴，完全在这台"
                  "设备上运行。",
                  piece);
  if (master[0] && (size_t)used < size)
    {
      ny_agent_local_intent_copy(piece, 97, master, strlen(master));
      used += snprintf(text + used, size - used, "你的主人叫%s。", piece);
    }
  if ((size_t)used < size)
    {
      ny_agent_local_intent_copy(piece, 97, style, strlen(style));
      used += snprintf(text + used, size - used, "说话风格：%s。", piece);
    }
  if (wishes[0] && (size_t)used < size)
    {
      ny_agent_local_intent_copy(piece, sizeof(piece), wishes,
                                 strlen(wishes));
      used += snprintf(text + used, size - used, "主人还希望：%s。", piece);
    }
  if (language[0] && strncmp(language, "zh", 2) && (size_t)used < size)
    {
      ny_agent_local_intent_copy(piece, 33, language, strlen(language));
      used += snprintf(text + used, size - used, "默认用 %s 回答。", piece);
    }
  if ((size_t)used < size)
    used += snprintf(text + used, size - used,
                     "用简短自然的口语回答，主人用什么语言你就用什么语言；"
                     "不要输出 JSON、代码或函数调用。");

  /* Commands the rules missed arrive here as chat.  Without this the model
   * cheerfully reports timers it never set.
   */

  if (chat && (size_t)used < size)
    snprintf(text + used, size - used,
             "你自己不能操作设备：如果主人想定时、设闹钟、调音量、放音乐或"
             "记事情，而对话里没有给出执行结果，就请主人换个更直接的说法，"
             "例如“定一个五分钟的计时器”，不要假装已经做了。");
  cJSON_Delete(profile);
}

/****************************************************************************
 * Name: ny_agent_local_add
 ****************************************************************************/

static bool ny_agent_local_add(cJSON *messages, const char *role,
                               const char *content)
{
  cJSON *message = cJSON_CreateObject();
  if (message && cJSON_AddStringToObject(message, "role", role) &&
      cJSON_AddStringToObject(message, "content", content) &&
      cJSON_AddItemToArray(messages, message))
    return true;
  cJSON_Delete(message);
  return false;
}

/****************************************************************************
 * Name: ny_agent_local_trim
 * Description: Drop the oldest exchange.  Returns false when none is left.
 *
 *   An exchange is dropped whole, up to the next user message: an assistant
 *   turn without its question is a prompt the chat template was never
 *   trained on.
 ****************************************************************************/

static bool ny_agent_local_trim(cJSON *messages)
{
  int users = 0;
  const cJSON *item;
  cJSON_ArrayForEach(item, messages)
  {
    if (!strcmp(ny_agent_local_text(item, "role"), "user"))
      users++;
  }

  /* The last user message is the question being asked. */

  if (users < 2)
    return false;
  for (;;)
    {
      cJSON_DeleteItemFromArray(messages, 1);
      const cJSON *next = cJSON_GetArrayItem(messages, 1);
      if (!next || !strcmp(ny_agent_local_text(next, "role"), "user"))
        break;
    }
  return cJSON_GetArraySize(messages) > 1;
}

/****************************************************************************
 * Name: ny_agent_local_generate
 * Description: One completion for {"messages":[system, ...]}, dropping the
 *   oldest exchange for as long as the compute domain answers that the
 *   prompt does not fit.
 ****************************************************************************/

static int ny_agent_local_generate(cJSON *root, size_t tokens,
                                   const char *label, char **response)
{
  cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
  int ret = 0;
  *response = NULL;
  for (int attempt = 0; ret == 0; attempt++)
    {
      struct ny_compute_chat_stats_s stats;
      char *body = cJSON_PrintUnformatted(root);
      if (!body)
        return -ENOMEM;
      memset(&stats, 0, sizeof(stats));
      ret = ny_compute_chat(body, tokens, NY_COMPUTE_CHAT_GUARD_UNTRUSTED,
                            response, &stats, NY_AGENT_LOCAL_WAIT);
      free(body);
      if (ret == 0)
        {
          syslog(LOG_INFO,
                 "nyagent: on-device %s: %lu prompt + %lu new tokens, "
                 "prefill %lu ms, decode %lu ms, %d exchanges dropped\n",
                 label, (unsigned long)stats.prompt_tokens,
                 (unsigned long)stats.completion_tokens,
                 (unsigned long)stats.prefill_ms,
                 (unsigned long)stats.decode_ms, attempt);
          return *response ? 0 : -EIO;
        }

      /* Too long for the window: forget the oldest exchange and ask again.
       * Anything else is not something a shorter prompt would cure.
       */

      if (ret != -E2BIG || attempt >= NY_AGENT_LOCAL_TRIMS ||
          !ny_agent_local_trim(messages))
        break;
      ret = 0;
    }
  free(*response);
  *response = NULL;
  return ret;
}

/****************************************************************************
 * Name: ny_agent_local_force
 * Description: Have the model read the one slot the rules could not.
 *
 *   The shape that was measured to work and no other: a single tool, no
 *   system prompt, and the instruction to call it appended to the owner's
 *   own sentence.  What comes back is checked by
 *   ny_agent_local_intent_fill() before any of it is believed.
 ****************************************************************************/

static int ny_agent_local_force(const char *utterance,
                                struct ny_agent_local_intent_s *intent)
{
  const char *name;
  const char *table = ny_agent_local_intent_forced(intent, &name);
  char content[NY_AGENT_LOCAL_UTTERANCE_MAX + 96];
  char *response = NULL;
  if (!table)
    return -ENOTSUP;
  snprintf(content, sizeof(content),
           "%s（必须调用 %s 工具，不要自行回答。）", utterance, name);
  cJSON *root = cJSON_CreateObject();
  cJSON *messages = root ? cJSON_AddArrayToObject(root, "messages") : NULL;
  cJSON *tools = cJSON_Parse(table);
  int ret = messages && tools &&
                    ny_agent_local_add(messages, "user", content) &&
                    cJSON_AddItemToObject(root, "tools", tools)
                ? 0
                : -ENOMEM;
  if (ret < 0)
    cJSON_Delete(tools);
  else
    ret = ny_agent_local_generate(root, NY_AGENT_LOCAL_TOKENS_FORCED,
                                  "slot", &response);
  cJSON_Delete(root);
  if (ret < 0)
    return ret;
  root = cJSON_Parse(response);
  free(response);
  const cJSON *function = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetArrayItem(
          cJSON_GetObjectItemCaseSensitive(
              cJSON_GetObjectItemCaseSensitive(
                  cJSON_GetArrayItem(
                      cJSON_GetObjectItemCaseSensitive(root, "choices"), 0),
                  "message"),
              "tool_calls"),
          0),
      "function");
  const cJSON *arguments =
      cJSON_GetObjectItemCaseSensitive(function, "arguments");
  cJSON *parsed = cJSON_IsString(arguments)
                      ? cJSON_Parse(arguments->valuestring)
                      : NULL;
  ret = ny_agent_local_intent_fill(intent,
                                   ny_agent_local_text(function, "name"),
                                   parsed ? parsed : arguments);
  syslog(LOG_INFO, "nyagent: on-device slot for %s: %s\n",
         ny_agent_local_intent_name(intent->kind),
         ret == 0 ? "accepted" : "rejected");
  cJSON_Delete(parsed);
  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_agent_local_gather
 * Description: What the tool call needs that only the product knows: the
 *   revision a create is checked against, the volume a relative change
 *   starts from, a file to play, the time zone an alarm is meant in.
 ****************************************************************************/

static int ny_agent_local_gather(const struct ny_agent_local_intent_s *intent,
                                 struct ny_agent_local_context_s *context,
                                 char *track, size_t size)
{
  const char *topic = ny_agent_local_intent_revision(intent->kind);
  cJSON *result = NULL;
  int ret = 0;
  memset(context, 0, sizeof(*context));
  context->now_minutes = -1;
  context->utc_offset_minutes = NY_AGENT_LOCAL_UTC_OFFSET;
  if (topic)
    {
      ret = ny_agent_local_ask(topic, &result);
      const cJSON *revision =
          cJSON_GetObjectItemCaseSensitive(result, "revision");
      if (ret == 0 && !cJSON_IsNumber(revision))
        ret = -EBADMSG;
      if (ret == 0)
        context->revision = revision->valuedouble;
      if (ret == 0 && intent->kind == NY_AGENT_LOCAL_ALARM)
        {
          uint64_t now = ny_product_time_ms(false);
          context->utc_offset_minutes = ny_agent_local_offset(result);
          if (ny_product_clock_valid())
            context->now_minutes =
                (int)(((int64_t)(now / 60000) +
                       context->utc_offset_minutes) % 1440);
        }
    }
  else if (intent->kind == NY_AGENT_LOCAL_MUSIC_PLAY)
    {
      ret = ny_agent_local_ask("music.library", &result);
      if (ret == 0)
        ret = ny_agent_local_intent_track(result, intent->text, track, size);
      context->track = track;
    }
  else if (intent->kind == NY_AGENT_LOCAL_VOLUME_UP ||
           intent->kind == NY_AGENT_LOCAL_VOLUME_DOWN ||
           intent->kind == NY_AGENT_LOCAL_MUTE ||
           intent->kind == NY_AGENT_LOCAL_UNMUTE)
    {
      ret = ny_agent_local_ask("music.status", &result);
      const cJSON *volume = cJSON_GetObjectItemCaseSensitive(result, "volume");
      if (ret == 0 && !cJSON_IsNumber(volume))
        ret = -EBADMSG;
      if (ret == 0)
        {
          context->volume = (int)volume->valuedouble;
          context->muted = cJSON_IsTrue(
              cJSON_GetObjectItemCaseSensitive(result, "muted"));
        }
    }
  cJSON_Delete(result);
  return ret;
}

/****************************************************************************
 * Name: ny_agent_local_command
 * Description: The owner's last message as a tool call, if it is one.
 *   Returns 1 with *response set, 0 for "this is chat", or a negated errno.
 *
 *   A command that cannot be turned into a call is answered in words -- a
 *   question, or what was missing -- and never handed to the chat path,
 *   where the model would claim to have done it.
 ****************************************************************************/

static int ny_agent_local_command(const char *utterance, char **response)
{
  struct ny_agent_local_intent_s *intent = malloc(sizeof(*intent));
  struct ny_agent_local_context_s context;
  char track[256];
  char words[160];
  const char *tool = NULL;
  const char *say = NULL;
  char *arguments = NULL;
  uint64_t unique;
  if (!intent)
    return -ENOMEM;
  ny_agent_local_intent_detect(utterance, intent);
  if (intent->kind == NY_AGENT_LOCAL_CHAT)
    {
      free(intent);
      return 0;
    }
  if (!intent->complete && ny_agent_local_force(utterance, intent) < 0)
    say = ny_agent_local_intent_question(intent);
  if (!say)
    {
      int ret = ny_agent_local_gather(intent, &context, track, sizeof(track));
      if (ret == 0)
        arguments = ny_agent_local_intent_call(intent, &context, &tool);
      if (ret == -ENOENT && intent->kind == NY_AGENT_LOCAL_MUSIC_PLAY)
        say = "曲库里还没有可以播放的音乐，先在面板里上传几首吧。";
      else if (ret == -ESRCH && intent->kind == NY_AGENT_LOCAL_MUSIC_PLAY)
        {
          char name[97];
          ny_agent_local_intent_copy(name, sizeof(name), intent->text,
                                     strlen(intent->text));
          snprintf(words, sizeof(words), "曲库里没有找到“%s”。", name);
          say = words;
        }
      else if (ret < 0 || !arguments)
        {
          snprintf(words, sizeof(words),
                   "这件事现在做不了：设备没有给出需要的数据（错误码 %d）。",
                   ret < 0 ? ret : -ENOMEM);
          say = words;
        }
    }
  unique = (ny_product_time_ms(true) << 12) | (++g_agent_local_serial & 0xfff);
  syslog(LOG_INFO, "nyagent: on-device rule %s -> %s\n",
         ny_agent_local_intent_name(intent->kind),
         say ? "asked the owner" : tool);
  *response = ny_agent_local_intent_reply(say ? NULL : tool, arguments, say,
                                          unique);
  free(arguments);
  free(intent);
  return *response ? 1 : -ENOMEM;
}

/****************************************************************************
 * Name: ny_agent_local_report
 * Description: The last message is a tool result: say it in one sentence.
 *
 *   The request is built afresh -- persona, the owner's words, the result as
 *   a line of text -- because the tool_calls/tool pair the framework
 *   appended is exactly the transcript the model cannot read.  The call has
 *   already happened, so a model failure here is not an error: the owner
 *   gets the plain line instead.
 ****************************************************************************/

static int ny_agent_local_report(const cJSON *messages, int last, bool owner,
                                 char **response)
{
  char *facts = malloc(NY_AGENT_LOCAL_FACTS * NY_AGENT_LOCAL_FACT_MAX + 16);
  char *prompt = malloc(NY_AGENT_LOCAL_FACTS * NY_AGENT_LOCAL_FACT_MAX + 64);
  int first = last;
  int offset = -1;
  size_t used = 0;
  bool failed = false;
  bool applied = false;
  int ret = facts && prompt ? 0 : -ENOMEM;
  while (ret == 0 && first > 0 &&
         !strcmp(ny_agent_local_text(cJSON_GetArrayItem(messages, first - 1),
                                     "role"),
                 "tool"))
    first--;
  if (last - first >= NY_AGENT_LOCAL_FACTS)
    first = last - NY_AGENT_LOCAL_FACTS + 1;
  if (ret == 0)
    facts[0] = 0;
  for (int index = first; ret == 0 && index <= last; index++)
    {
      const cJSON *message = cJSON_GetArrayItem(messages, index);
      const char *id = ny_agent_local_text(message, "tool_call_id");
      const cJSON *function = NULL;

      /* The call this result answers is in the assistant turn before it. */

      for (int back = first - 1; back >= 0 && !function; back--)
        {
          const cJSON *call;
          cJSON_ArrayForEach(call,
                             cJSON_GetObjectItemCaseSensitive(
                                 cJSON_GetArrayItem(messages, back),
                                 "tool_calls"))
          {
            if (!strcmp(ny_agent_local_text(call, "id"), id))
              function = cJSON_GetObjectItemCaseSensitive(call, "function");
          }
        }
      const char *tool = ny_agent_local_text(function, "name");
      if (offset < 0 && !strcmp(tool, "nyabula_read") && owner)
        offset = ny_agent_local_offset(NULL);
      if (strcmp(tool, "nyabula_read"))
        applied = true;
      if (used)
        used += (size_t)snprintf(facts + used, 8, "；");
      failed |= ny_agent_local_intent_fact(
          tool, ny_agent_local_text(function, "arguments"),
          ny_agent_local_text(message, "content"),
          offset < 0 ? NY_AGENT_LOCAL_UTC_OFFSET : offset, facts + used,
          NY_AGENT_LOCAL_FACT_MAX);
      used += strlen(facts + used);
    }
  if (ret == 0)
    {
      /* The fact is the answer.  Asked to phrase it, the model changed it:
       * "14:05" came back as "14:00" and "a 5 minute timer" as "5 timers of
       * five minutes" (board, 2026-09-20), in every prompt shape tried.  A
       * sentence that is plain but right beats a warm one that is wrong, so
       * the model is kept to chat and to reading slots.
       */

      snprintf(prompt,
               NY_AGENT_LOCAL_FACTS * NY_AGENT_LOCAL_FACT_MAX + 64, "%s%s。",
               failed ? "抱歉，" : applied ? "好的～" : "", facts);
      *response = ny_agent_local_intent_reply(
          NULL, NULL, prompt,
          (ny_product_time_ms(true) << 12) |
              (++g_agent_local_serial & 0xfff));
      ret = *response ? 0 : -ENOMEM;
    }
  free(facts);
  free(prompt);
  return ret;
}

/****************************************************************************
 * Name: ny_agent_local_chat
 * Description: Persona plus the text turns that fit, newest first.
 *
 *   Tool calls, tool results and the framework's own system text are left
 *   out: they are what the model imitates badly, and the persona replaces
 *   the cloud-sized system prompt.
 ****************************************************************************/

static int ny_agent_local_chat(const cJSON *messages, bool owner,
                               char **response)
{
  char *persona = malloc(2048);
  cJSON *root = cJSON_CreateObject();
  cJSON *fresh = root ? cJSON_AddArrayToObject(root, "messages") : NULL;
  int count = cJSON_GetArraySize(messages);
  size_t bytes = 0;
  int start = count;
  int ret = persona && fresh ? 0 : -ENOMEM;
  for (int index = count - 1; ret == 0 && index >= 0; index--)
    {
      const cJSON *message = cJSON_GetArrayItem(messages, index);
      const char *role = ny_agent_local_text(message, "role");
      const char *content = ny_agent_local_text(message, "content");
      bool user = !strcmp(role, "user");
      if ((!user && strcmp(role, "assistant")) || !content[0] ||
          cJSON_GetObjectItemCaseSensitive(message, "tool_calls"))
        continue;

      /* The question itself always goes, whatever its size. */

      if (start != count && bytes + strlen(content) > NY_AGENT_LOCAL_HISTORY)
        break;
      bytes += strlen(content);
      if (user)
        start = index; /* An exchange begins with its question */
    }
  if (ret == 0 && start == count)
    ret = -EINVAL;
  if (ret == 0)
    {
      ny_agent_local_persona(owner, true, persona, 2048);
      if (!ny_agent_local_add(fresh, "system", persona))
        ret = -ENOMEM;
    }
  for (int index = start; ret == 0 && index < count; index++)
    {
      const cJSON *message = cJSON_GetArrayItem(messages, index);
      const char *role = ny_agent_local_text(message, "role");
      const char *content = ny_agent_local_text(message, "content");
      if ((strcmp(role, "user") && strcmp(role, "assistant")) ||
          !content[0] ||
          cJSON_GetObjectItemCaseSensitive(message, "tool_calls"))
        continue;
      if (!ny_agent_local_add(fresh, role, content))
        ret = -ENOMEM;
    }
  if (ret == 0)
    ret = ny_agent_local_generate(root, NY_AGENT_LOCAL_TOKENS_CHAT, "chat",
                                  response);
  cJSON_Delete(root);
  free(persona);
  return ret;
}

/****************************************************************************
 * Name: ny_agent_local_error
 ****************************************************************************/

static char *ny_agent_local_error(const char *message)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *error = root ? cJSON_AddObjectToObject(root, "error") : NULL;
  char *encoded = NULL;
  if (error && cJSON_AddStringToObject(error, "message", message) &&
      cJSON_AddStringToObject(error, "type", "on_device"))
    encoded = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return encoded;
}

/****************************************************************************
 * Name: ny_agent_local_transport
 ****************************************************************************/

static int ny_agent_local_transport(const char *request, char **response,
                                    int *status, void *arg)
{
  (void)arg;
  *response = NULL;
  *status = 503;
  cJSON *root = cJSON_Parse(request);
  const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
  bool owner =
      ny_agent_local_owner(cJSON_GetObjectItemCaseSensitive(root, "tools"));
  int last = cJSON_GetArraySize(messages) - 1;
  int ret = cJSON_IsArray(messages) && last >= 0 ? 0 : -EINVAL;

  /* The framework closes some runs with a system hint after the result. */

  while (ret == 0 && last > 0 &&
         !strcmp(ny_agent_local_text(cJSON_GetArrayItem(messages, last),
                                     "role"),
                 "system"))
    last--;
  if (ret == 0)
    {
      const cJSON *message = cJSON_GetArrayItem(messages, last);
      const char *role = ny_agent_local_text(message, "role");
      if (!strcmp(role, "tool"))
        ret = ny_agent_local_report(messages, last, owner, response);
      else
        {
          ret = owner && !strcmp(role, "user")
                    ? ny_agent_local_command(
                          ny_agent_local_text(message, "content"), response)
                    : 0;
          if (ret == 0)
            ret = ny_agent_local_chat(messages, owner, response);
          else if (ret > 0)
            ret = 0;
        }
    }
  cJSON_Delete(root);
  if (ret < 0 || !*response)
    {
      /* Answer as a failing HTTP endpoint would, so the router counts the
       * failure and fails over to a cloud backend when one is configured.
       */

      char reason[64];
      if (ret == 0)
        ret = -EIO;
      snprintf(reason, sizeof(reason), "on-device model unavailable (%d)",
               ret);
      syslog(LOG_WARNING, "nyagent: %s\n", reason);
      free(*response);
      *response = ny_agent_local_error(reason);
      *status = ret == -E2BIG ? 413 : 503;
      return *response ? OK : ERROR;
    }
  *status = 200;
  return OK;
}


/****************************************************************************
 * Name: ny_agent_local_init
 ****************************************************************************/

int ny_agent_local_init(void)
{
  return llm_set_local_transport(NY_AGENT_LOCAL_HOST,
                                 ny_agent_local_transport, NULL) == OK
             ? 0
             : -EIO;
}

/****************************************************************************
 * Name: ny_agent_local
 * Description: agent.config.ondevice.get / .set {enabled, priority?}
 *
 *   The backend occupies the router's last slot so that enabling it never
 *   overwrites an endpoint the owner typed in; a slot that holds something
 *   else is reported rather than taken.
 ****************************************************************************/

int ny_agent_local(const char *topic, const cJSON *data, cJSON **result)
{
  llm_backend_t backend;
  bool present;
  bool ours;
  memset(&backend, 0, sizeof(backend));
  present = llm_router_get_backend(NY_AGENT_LOCAL_SLOT, &backend) == 0 &&
            backend.host[0] != 0;
  ours = present && !strcmp(backend.host, NY_AGENT_LOCAL_HOST);
  if (!strcmp(topic, "agent.config.ondevice.set"))
    {
      const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
      const cJSON *priority =
          cJSON_GetObjectItemCaseSensitive(data, "priority");
      if (!cJSON_IsBool(enabled) ||
          (priority && (!cJSON_IsNumber(priority) ||
                        priority->valuedouble < 0 ||
                        priority->valuedouble > 100)))
        return -EINVAL;
      if (present && !ours)
        return -EEXIST;
      memset(&backend, 0, sizeof(backend));
      snprintf(backend.host, sizeof(backend.host), "%s",
               NY_AGENT_LOCAL_HOST);
      snprintf(backend.path, sizeof(backend.path), "%s",
               NY_AGENT_LOCAL_PATH);
      snprintf(backend.port, sizeof(backend.port), "%s",
               NY_AGENT_LOCAL_PORT);
      snprintf(backend.api_key, sizeof(backend.api_key), "%s",
               NY_AGENT_LOCAL_KEY);
      snprintf(backend.model, sizeof(backend.model), "%s",
               CONFIG_NYABULA_CORE_COMPUTE_LLM);
      backend.priority = priority ? (int)priority->valuedouble : 0;
      backend.cost_tier = 0; /* Free: the eco profile prefers it */
      backend.enabled = cJSON_IsTrue(enabled);
      if (llm_router_set_backend(NY_AGENT_LOCAL_SLOT, &backend) != 0)
        return -EIO;
      ours = true;
    }
  else if (strcmp(topic, "agent.config.ondevice.get"))
    return -ENOSYS;
  *result = cJSON_CreateObject();
  if (!*result ||
      !cJSON_AddBoolToObject(*result, "available", !present || ours) ||
      !cJSON_AddBoolToObject(*result, "enabled", ours && backend.enabled) ||
      !cJSON_AddNumberToObject(*result, "slot", NY_AGENT_LOCAL_SLOT) ||
      !cJSON_AddNumberToObject(*result, "priority",
                               ours ? backend.priority : 0) ||
      !cJSON_AddStringToObject(*result, "model",
                               CONFIG_NYABULA_CORE_COMPUTE_LLM) ||
      !cJSON_AddNumberToObject(*result, "failures",
                               ours ? backend.total_failures : 0) ||
      !cJSON_AddNumberToObject(*result, "calls",
                               ours ? backend.total_calls : 0) ||
      !cJSON_AddNumberToObject(*result, "latencyMs",
                               ours ? backend.avg_latency_ms : 0))
    {
      cJSON_Delete(*result);
      *result = NULL;
      memset(&backend, 0, sizeof(backend));
      return -ENOMEM;
    }
  memset(&backend, 0, sizeof(backend));
  return 0;
}

#endif /* CONFIG_NYABULA_CORE_AGENT && CONFIG_NYABULA_CORE_COMPUTE */
