/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_local_intent.c
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

/* Rules in front of a 1B model.
 *
 * Measured on the board, the model chats well and reads a number out of a
 * sentence reliably, but it cannot choose between tools: offered a table it
 * announces the call in prose, and as a classifier it is right one time in
 * three.  So the choice is made here, by keyword and pattern, and the rules
 * are deliberately narrow: a command that is missed becomes ordinary chat,
 * which is harmless, while a remark that is mistaken for a command sets a
 * timer nobody asked for.  Every pattern therefore wants an action cue next
 * to the noun, and opinion questions are turned away before the action
 * intents are tried at all.
 *
 * Matching is byte-wise on UTF-8, which is sound for whole words: no UTF-8
 * sequence is a suffix or a prefix of another one.  Where a scan has to move
 * character by character it steps over whole sequences.
 */

#include "ny_agent_local_intent.h"
#include "ny_utf8.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NY_INTENT_COUNT(list) (sizeof(list) / sizeof((list)[0]))
#define NY_INTENT_ANY(text, list) \
  (ny_intent_find(text, list, NY_INTENT_COUNT(list), NULL) != NULL)

#define NY_INTENT_LABEL_MAX  96     /* ny_product_timers.c, _alarms.c */
#define NY_INTENT_TITLE_MAX  160    /* ny_product_records.c: tasks */
#define NY_INTENT_TIMER_MAX  86400u /* A longer wait is an alarm */
#define NY_INTENT_QUIET_MIN  5      /* "Quieter" is not "silent" */
#define NY_INTENT_LIST_SHOWN 4

/* A slice of the utterance.  `lower` is what the rules read; `original` has
 * the same byte offsets and is what the owner's words are copied from.
 */

struct ny_intent_view_s
{
  const char *lower;
  const char *original;
};

enum ny_intent_period_e
{
  NY_INTENT_PERIOD_NONE = 0,
  NY_INTENT_PERIOD_DAWN,
  NY_INTENT_PERIOD_AM,
  NY_INTENT_PERIOD_NOON,
  NY_INTENT_PERIOD_PM,
  NY_INTENT_PERIOD_NIGHT,
  NY_INTENT_PERIOD_MIDNIGHT
};

struct ny_intent_word_s
{
  const char *word;
  int value;
};

struct ny_intent_mood_s
{
  const char *word;
  const char *expression;
};

static const char *const g_intent_days[] = { "sun", "mon", "tue", "wed",
                                             "thu", "fri", "sat" };

/* The 13 names nyabula_expression accepts, with what the fact line calls
 * them.
 */

static const struct ny_intent_mood_s g_intent_expressions[] = {
  { "idle", "平静" },       { "curious", "好奇" },  { "happy", "开心" },
  { "processing", "思考" }, { "star", "星星眼" },   { "heart", "爱心" },
  { "sleepy", "犯困" },     { "sleep", "睡觉" },    { "angry", "生气" },
  { "sad", "难过" },        { "surprise", "惊讶" }, { "dizzy", "晕乎乎" },
  { "derp", "呆萌" }
};

/* Longest and most specific first: the first hit wins, and "sleepy" must be
 * seen before "sleep", "星星眼" before anything shorter.
 */

static const struct ny_intent_mood_s g_intent_moods[] = {
  { "星星眼", "star" },
  { "星星", "star" },
  { "崇拜", "star" },
  { "卖个萌", "heart" },
  { "卖萌", "heart" },
  { "爱心", "heart" },
  { "比个心", "heart" },
  { "比心", "heart" },
  { "喜欢", "heart" },
  { "爱你", "heart" },
  { "开心", "happy" },
  { "高兴", "happy" },
  { "快乐", "happy" },
  { "笑", "happy" },
  { "生气", "angry" },
  { "愤怒", "angry" },
  { "发火", "angry" },
  { "凶", "angry" },
  { "难过", "sad" },
  { "伤心", "sad" },
  { "委屈", "sad" },
  { "哭", "sad" },
  { "打瞌睡", "sleepy" },
  { "瞌睡", "sleepy" },
  { "犯困", "sleepy" },
  { "困", "sleepy" },
  { "睡觉", "sleep" },
  { "睡着", "sleep" },
  { "装睡", "sleep" },
  { "闭眼", "sleep" },
  { "惊讶", "surprise" },
  { "吃惊", "surprise" },
  { "震惊", "surprise" },
  { "惊喜", "surprise" },
  { "晕", "dizzy" },
  { "好奇", "curious" },
  { "疑惑", "curious" },
  { "发呆", "derp" },
  { "呆", "derp" },
  { "傻", "derp" },
  { "思考", "processing" },
  { "想一想", "processing" },
  { "正常", "idle" },
  { "默认", "idle" },
  { "平静", "idle" },
  { "恢复", "idle" },
  { "sleepy", "sleepy" },
  { "sleep", "sleep" },
  { "happy", "happy" },
  { "smile", "happy" },
  { "angry", "angry" },
  { "mad", "angry" },
  { "sad", "sad" },
  { "cry", "sad" },
  { "heart", "heart" },
  { "love", "heart" },
  { "surprised", "surprise" },
  { "surprise", "surprise" },
  { "dizzy", "dizzy" },
  { "star", "star" },
  { "curious", "curious" },
  { "derp", "derp" },
  { "silly", "derp" },
  { "thinking", "processing" },
  { "idle", "idle" },
  { "normal", "idle" },
  { "neutral", "idle" }
};

/* What the model answers when it means one of the 13 but says it its own
 * way; "anger" for 生气 is the measured one.
 */

static const struct ny_intent_mood_s g_intent_near[] = {
  { "anger", "angry" },
  { "mad", "angry" },
  { "happiness", "happy" },
  { "joy", "happy" },
  { "smile", "happy" },
  { "sadness", "sad" },
  { "cry", "sad" },
  { "love", "heart" },
  { "cute", "heart" },
  { "tired", "sleepy" },
  { "drowsy", "sleepy" },
  { "asleep", "sleep" },
  { "sleeping", "sleep" },
  { "surprised", "surprise" },
  { "shock", "surprise" },
  { "shocked", "surprise" },
  { "confused", "dizzy" },
  { "curiosity", "curious" },
  { "stars", "star" },
  { "neutral", "idle" },
  { "normal", "idle" },
  { "default", "idle" },
  { "thinking", "processing" },
  { "silly", "derp" }
};

static const struct ny_intent_word_s g_intent_periods[] = {
  { "凌晨", NY_INTENT_PERIOD_DAWN },     { "早上", NY_INTENT_PERIOD_AM },
  { "早晨", NY_INTENT_PERIOD_AM },       { "上午", NY_INTENT_PERIOD_AM },
  { "明早", NY_INTENT_PERIOD_AM },       { "今早", NY_INTENT_PERIOD_AM },
  { "清晨", NY_INTENT_PERIOD_AM },       { "一早", NY_INTENT_PERIOD_AM },
  { "大早", NY_INTENT_PERIOD_AM },       { "中午", NY_INTENT_PERIOD_NOON },
  { "正午", NY_INTENT_PERIOD_NOON },     { "下午", NY_INTENT_PERIOD_PM },
  { "午后", NY_INTENT_PERIOD_PM },       { "傍晚", NY_INTENT_PERIOD_PM },
  { "晚上", NY_INTENT_PERIOD_NIGHT },    { "今晚", NY_INTENT_PERIOD_NIGHT },
  { "明晚", NY_INTENT_PERIOD_NIGHT },    { "晚间", NY_INTENT_PERIOD_NIGHT },
  { "夜里", NY_INTENT_PERIOD_NIGHT },    { "夜晚", NY_INTENT_PERIOD_NIGHT },
  { "夜间", NY_INTENT_PERIOD_NIGHT },    { "半夜", NY_INTENT_PERIOD_MIDNIGHT },
  { "午夜", NY_INTENT_PERIOD_MIDNIGHT }, { "深夜", NY_INTENT_PERIOD_MIDNIGHT }
};

/* Opinion and knowledge questions.  They mention the nouns the action rules
 * look for ("你觉得计时器这个发明怎么样") and must reach the model as chat.
 * The read intents are exempt: "今天天气怎么样" is a request.
 */

static const char *const g_intent_discussion[] = {
  "你觉得", "你认为",   "怎么看", "怎么样",   "为什么",       "为啥",
  "什么是", "是什么",   "是谁",   "谁发明",   "发明",         "原理",
  "历史",   "什么意思", "啥意思", "介绍一下", "讲讲",         "说说",
  "聊聊",   "what is",  "why",    "how does", "do you think", "tell me about"
};

static const char *const g_intent_trailing[] = {
  "。", ".",    "！",     "!",    "？",     "?",  "~",    "～",    "，",
  ",",  " ",    "吧",     "哦",   "啊",     "呀", "啦",   "哈",    "嘛",
  "呢", "好吗", "可以吗", "行吗", "好不好", "吗", "谢谢", "please"
};

static const char *const g_intent_label_leading[] = {
  "的时候", "的",       "，",     ",",    " ",  "：", ":",  "记得",
  "别忘了", "不要忘了", "别忘记", "一下", "去", "要", "该", "to "
};

static const char *const g_intent_remind[] = {
  "提醒我",    "叫醒我",     "叫我",    "喊我",   "告诉我",
  "remind me", "wake me up", "wake me", "tell me"
};

static const char *const g_intent_list_cues[] = {
  "有哪些", "有什么", "哪些",   "有几个",   "几个",     "列表", "查看",
  "看看",   "看一下", "看下",   "还有多久", "还要多久", "还剩", "剩多少",
  "剩多久", "有没有", "多少个", "list",     "show",     "what"
};

static const char *const g_intent_forced_timer =
    "[{\"type\":\"function\",\"function\":{\"name\":\"set_timer\","
    "\"description\":\"Start a countdown timer.\",\"parameters\":{"
    "\"type\":\"object\",\"properties\":{\"minutes\":{\"type\":\"number\","
    "\"description\":\"Length in minutes\"}},\"required\":[\"minutes\"]}}}]";

static const char *const g_intent_forced_alarm =
    "[{\"type\":\"function\",\"function\":{\"name\":\"set_alarm\","
    "\"description\":\"Set an alarm clock.\",\"parameters\":{"
    "\"type\":\"object\",\"properties\":{\"time\":{\"type\":\"string\","
    "\"description\":\"24-hour HH:MM\"}},\"required\":[\"time\"]}}}]";

static const char *const g_intent_forced_volume =
    "[{\"type\":\"function\",\"function\":{\"name\":\"set_volume\","
    "\"description\":\"Set the speaker volume.\",\"parameters\":{"
    "\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\","
    "\"description\":\"0-100\"}},\"required\":[\"percent\"]}}}]";

static const char *const g_intent_forced_expression =
    "[{\"type\":\"function\",\"function\":{\"name\":\"set_expression\","
    "\"description\":\"Set the cat eye expression.\",\"parameters\":{"
    "\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\","
    "\"enum\":[\"idle\",\"curious\",\"happy\",\"processing\",\"star\","
    "\"heart\",\"sleepy\",\"sleep\",\"angry\",\"sad\",\"surprise\",\"dizzy\","
    "\"derp\"]}},\"required\":[\"expression\"]}}}]";

static bool ny_intent_letter(char c);
static size_t ny_intent_step(const char *text);
static bool ny_intent_starts(const char *text, const char *word);
static const char *ny_intent_find(const char *text, const char *const *words,
                                  size_t count, size_t *length);
static const char *ny_intent_skip(const char *text, const char *const *words,
                                  size_t count);
static size_t ny_intent_digit(const char *text, int *value);
static bool ny_intent_numeral_before(const char *text, const char *at);
static size_t ny_intent_number(const char *text, double *value, bool *chinese);
static bool ny_intent_numerals(const char *text);
static size_t ny_intent_unit(const char *text, const char *const *units,
                             size_t count);
static size_t ny_intent_component(const char *text, double *seconds, int *rank,
                                  bool *bare);
static bool ny_intent_duration(const char *text, uint32_t *seconds,
                               const char **begin, const char **end);
static enum ny_intent_period_e ny_intent_period(const char *text,
                                                const char *at);
static bool ny_intent_hour(int *hour, enum ny_intent_period_e period);
static bool ny_intent_clock(const char *text,
                            struct ny_agent_local_intent_s *intent,
                            const char **end);
static bool ny_intent_percent(const char *text, const char *after,
                              size_t reach, int *percent);
static void ny_intent_extract(const struct ny_intent_view_s *view,
                              const char *begin, const char *end,
                              const char *const *leading, size_t count,
                              char *out, size_t limit);
static const char *ny_intent_mood(const char *text);
static const char *ny_intent_expression(const char *name);
static bool ny_intent_remember(const struct ny_intent_view_s *view,
                               struct ny_agent_local_intent_s *intent);
static bool ny_intent_task(const struct ny_intent_view_s *view,
                           struct ny_agent_local_intent_s *intent);
static bool ny_intent_lists(const char *text,
                            struct ny_agent_local_intent_s *intent);
static bool ny_intent_volume(const char *text,
                             struct ny_agent_local_intent_s *intent);
static bool ny_intent_schedule(const struct ny_intent_view_s *view,
                               struct ny_agent_local_intent_s *intent);
static bool ny_intent_music(const struct ny_intent_view_s *view,
                            struct ny_agent_local_intent_s *intent);
static bool ny_intent_face(const char *text,
                           struct ny_agent_local_intent_s *intent);
static bool ny_intent_reminder(const struct ny_intent_view_s *view,
                               struct ny_agent_local_intent_s *intent);
static bool ny_intent_reads(const char *text,
                            struct ny_agent_local_intent_s *intent);
static void ny_intent_reset(struct ny_agent_local_intent_s *intent,
                            bool numerals);
static void ny_intent_alarm_time(const struct ny_agent_local_intent_s *intent,
                                 int now_minutes, int *hour, int *minute);
static void ny_intent_civil(int64_t seconds, int *year, int *month, int *day,
                            int *weekday, int *hour, int *minute);
static void ny_intent_span(double seconds, char *out, size_t size);
static const char *ny_intent_text(const cJSON *object, const char *key);
static double ny_intent_value(const cJSON *object, const char *key);
static void ny_intent_attempt(const char *tool, const cJSON *call, char *out,
                              size_t size);
static const char *ny_intent_reason(int error, char *buffer, size_t size);
static void ny_intent_fact_read(const char *topic, const cJSON *result,
                                int utc_offset_minutes, char *fact,
                                size_t size);

/****************************************************************************
 * Name: ny_intent_letter
 ****************************************************************************/

static bool ny_intent_letter(char c) { return c >= 'a' && c <= 'z'; }

/****************************************************************************
 * Name: ny_intent_step
 * Description: Bytes in the character at `text`.  NUL is not a continuation
 *   byte, so a cut-off sequence cannot walk past the terminator.
 ****************************************************************************/

static size_t ny_intent_step(const char *text)
{
  size_t length = 1;
  while (((unsigned char)text[length] & 0xc0) == 0x80)
    length++;
  return length;
}

/****************************************************************************
 * Name: ny_intent_starts
 ****************************************************************************/

static bool ny_intent_starts(const char *text, const char *word)
{
  return strncmp(text, word, strlen(word)) == 0;
}

/****************************************************************************
 * Name: ny_intent_find
 * Description: The first word of the list that occurs in the text.
 *
 *   An English word has to stand alone: "star" is an expression, "start" is
 *   not.  Chinese has no word boundaries to check.
 ****************************************************************************/

static const char *ny_intent_find(const char *text, const char *const *words,
                                  size_t count, size_t *length)
{
  for (size_t i = 0; i < count; i++)
    {
      size_t size = strlen(words[i]);
      const char *hit = text;
      while ((hit = strstr(hit, words[i])) != NULL)
        {
          bool inside = (ny_intent_letter(words[i][0]) && hit > text &&
                         ny_intent_letter(hit[-1])) ||
                        (ny_intent_letter(words[i][size - 1]) &&
                         ny_intent_letter(hit[size]));
          if (!inside)
            {
              if (length)
                *length = size;
              return hit;
            }
          hit += size;
        }
    }
  return NULL;
}

/****************************************************************************
 * Name: ny_intent_skip
 * Description: Past every leading occurrence of the listed words.
 ****************************************************************************/

static const char *ny_intent_skip(const char *text, const char *const *words,
                                  size_t count)
{
  for (size_t i = 0; i < count;)
    {
      if (ny_intent_starts(text, words[i]))
        {
          text += strlen(words[i]);
          i = 0;
        }
      else
        i++;
    }
  return text;
}

/****************************************************************************
 * Name: ny_intent_digit
 ****************************************************************************/

static size_t ny_intent_digit(const char *text, int *value)
{
  static const char *const digits[] = { "零", "一", "二", "三", "四",
                                        "五", "六", "七", "八", "九" };
  for (int i = 0; i < 10; i++)
    if (ny_intent_starts(text, digits[i]))
      {
        *value = i;
        return strlen(digits[i]);
      }
  if (ny_intent_starts(text, "两"))
    {
      *value = 2;
      return strlen("两");
    }
  if (ny_intent_starts(text, "〇"))
    {
      *value = 0;
      return strlen("〇");
    }
  return 0;
}

/****************************************************************************
 * Name: ny_intent_numeral_before
 * Description: True when `at` is in the middle of a number, which is not a
 *   place to start reading one: the 五 of 二十五, the 5 of 15.
 ****************************************************************************/

static bool ny_intent_numeral_before(const char *text, const char *at)
{
  int value;
  if (at > text && ((at[-1] >= '0' && at[-1] <= '9') || at[-1] == '.'))
    return true;
  return at - text >= 3 &&
         (ny_intent_digit(at - 3, &value) == 3 ||
          ny_intent_starts(at - 3, "十") || ny_intent_starts(at - 3, "百"));
}

/****************************************************************************
 * Name: ny_intent_number
 * Description: Arabic ("90", "1.5") or Chinese ("十五", "一百二十", "两")
 *   up to 999.  Returns the bytes read, 0 when there is no number here.
 *
 *   百分之 is left alone: it introduces a percentage, and reading its 百 as
 *   a hundred would turn "百分之三十" into a hundred minutes.
 ****************************************************************************/

static size_t ny_intent_number(const char *text, double *value, bool *chinese)
{
  const char *p = text;
  if (*p >= '0' && *p <= '9')
    {
      double number = 0;
      int digits = 0;
      while (*p >= '0' && *p <= '9' && digits++ < 6)
        number = number * 10 + (*p++ - '0');
      if (*p >= '0' && *p <= '9')
        return 0;
      if (*p == '.' && p[1] >= '0' && p[1] <= '9')
        {
          double scale = 0.1;
          for (p++; *p >= '0' && *p <= '9'; p++, scale /= 10)
            number += (*p - '0') * scale;
        }
      *value = number;
      if (chinese)
        *chinese = false;
      return (size_t)(p - text);
    }

  int total = 0;
  int current = -1;
  for (;;)
    {
      int digit;
      size_t size = ny_intent_digit(p, &digit);
      if (ny_intent_starts(p, "百分"))
        break;
      if (size)
        current = current < 0 ? digit : current * 10 + digit;
      else if (ny_intent_starts(p, "十") || ny_intent_starts(p, "百"))
        {
          size = strlen("十");
          total += (current < 0 ? 1 : current) *
                   (ny_intent_starts(p, "十") ? 10 : 100);
          current = -1;
        }
      else
        break;
      p += size;
      if (total > 999 || current > 999)
        return 0;
    }
  if (p == text)
    return 0;
  *value = total + (current < 0 ? 0 : current);
  if (chinese)
    *chinese = true;
  return (size_t)(p - text);
}

/****************************************************************************
 * Name: ny_intent_numerals
 * Description: Whether the words hold any quantity at all.
 *
 *   The forced model call reads a slot the rules could not; asked to read a
 *   duration out of "帮我定个计时器" it invents one.  No quantity in the
 *   words means the owner is asked instead.
 ****************************************************************************/

static bool ny_intent_numerals(const char *text)
{
  static const char *const words[] = { "两",     "二",     "三",   "四",
                                       "五",     "六",     "七",   "八",
                                       "九",     "十",     "百",   "半",
                                       "一点",   "一分",   "一秒", "一小",
                                       "一个小", "一个半", "一刻", "一个钟" };
  for (const char *p = text; *p; p++)
    if (*p >= '0' && *p <= '9')
      return true;
  return NY_INTENT_ANY(text, words);
}

/****************************************************************************
 * Name: ny_intent_unit
 ****************************************************************************/

static size_t ny_intent_unit(const char *text, const char *const *units,
                             size_t count)
{
  for (size_t i = 0; i < count; i++)
    {
      size_t size = strlen(units[i]);
      if (!strncmp(text, units[i], size) &&
          !(ny_intent_letter(units[i][0]) && ny_intent_letter(text[size])))
        return size;
    }
  return 0;
}

/****************************************************************************
 * Name: ny_intent_component
 * Description: One "number unit" piece of a duration at `text`: 三分半,
 *   一个半小时, 半小时, 一刻钟, 90秒, 5 minutes.  rank: 3 hours, 2 minutes,
 *   1 seconds.  *bare reports a lone 分, which is also how clock minutes are
 *   written.
 ****************************************************************************/

static size_t ny_intent_component(const char *text, double *seconds, int *rank,
                                  bool *bare)
{
  static const char *const hours[] = { "小时", "钟头", "hours",
                                       "hour", "hrs",  "hr" };
  static const char *const minutes[] = { "分钟",   "分",   "minutes",
                                         "minute", "mins", "min" };
  static const char *const secs[] = { "秒钟",   "秒",   "seconds",
                                      "second", "secs", "sec" };
  const char *p = text;
  double number = 0;
  bool half = false;
  size_t size;
  *bare = false;
  if (ny_intent_starts(p, "半"))
    {
      p += strlen("半");
      if (ny_intent_starts(p, "个"))
        p += strlen("个");
      half = true;
    }
  else
    {
      size = ny_intent_number(p, &number, NULL);
      if (size == 0)
        return 0;
      p += size;
      if (ny_intent_starts(p, "刻钟"))
        {
          *seconds = number * 900;
          *rank = 2;
          return (size_t)(p - text) + strlen("刻钟");
        }
      if (ny_intent_starts(p, "个"))
        p += strlen("个");
      if (ny_intent_starts(p, "半"))
        {
          p += strlen("半");
          half = true;
        }
    }
  while (*p == ' ')
    p++;
  if ((size = ny_intent_unit(p, hours, NY_INTENT_COUNT(hours))) != 0)
    {
      p += size;
      if (!half && ny_intent_starts(p, "半"))
        {
          p += strlen("半");
          half = true;
        }
      *seconds = (number + (half ? 0.5 : 0)) * 3600;
      *rank = 3;
    }
  else if ((size = ny_intent_unit(p, minutes, NY_INTENT_COUNT(minutes))) != 0)
    {
      *bare = size == strlen("分") && ny_intent_starts(p, "分");
      p += size;
      if (*bare && ny_intent_starts(p, "之"))
        return 0; /* 三分之一 is a fraction */
      if (!half && ny_intent_starts(p, "半"))
        {
          p += strlen("半");
          half = true;
          *bare = false;
          if (ny_intent_starts(p, "钟"))
            p += strlen("钟");
        }
      *seconds = (number + (half ? 0.5 : 0)) * 60;
      *rank = 2;
    }
  else if ((size = ny_intent_unit(p, secs, NY_INTENT_COUNT(secs))) != 0 &&
           !half)
    {
      p += size;
      *seconds = number;
      *rank = 1;
    }
  else
    return 0;
  return (size_t)(p - text);
}

/****************************************************************************
 * Name: ny_intent_duration
 * Description: The first duration in the text, as the sum of its pieces in
 *   falling order of unit ("1小时30分", "两分三十秒").
 ****************************************************************************/

static bool ny_intent_duration(const char *text, uint32_t *seconds,
                               const char **begin, const char **end)
{
  static const char *const joints[] = { "零", "又", " ", "and " };
  for (const char *p = text; *p; p += ny_intent_step(p))
    {
      double total = 0;
      int rank = 4;
      const char *q = p;
      const char *last = p;
      if (ny_intent_numeral_before(text, p))
        continue;
      for (;;)
        {
          double value;
          int unit;
          bool bare;
          size_t size = ny_intent_component(q, &value, &unit, &bare);
          if (size == 0 || unit >= rank)
            break;

          /* 三点十分: the 十分 belongs to a clock time. */

          if (bare && rank == 4 && p - text >= 3 &&
              (ny_intent_starts(p - 3, "点") || ny_intent_starts(p - 3, "點")))
            break;
          total += value;
          rank = unit;
          last = q + size;
          q = ny_intent_skip(last, joints, NY_INTENT_COUNT(joints));
        }
      if (last != p && total >= 1 && total <= 604800)
        {
          *seconds = (uint32_t)(total + 0.5);
          if (begin)
            *begin = p;
          if (end)
            *end = last;
          return true;
        }
    }
  return false;
}

/****************************************************************************
 * Name: ny_intent_period
 * Description: The part of the day named just before `at` ("明天早上 7 点",
 *   "晚上的十点"): the nearest one that ends within three characters.
 ****************************************************************************/

static enum ny_intent_period_e ny_intent_period(const char *text,
                                                const char *at)
{
  const char *nearest = NULL;
  enum ny_intent_period_e period = NY_INTENT_PERIOD_NONE;
  for (size_t i = 0; i < NY_INTENT_COUNT(g_intent_periods); i++)
    {
      size_t size = strlen(g_intent_periods[i].word);
      const char *hit = text;
      while ((hit = strstr(hit, g_intent_periods[i].word)) != NULL &&
             hit + size <= at)
        {
          if (nearest == NULL || hit + size > nearest)
            {
              nearest = hit + size;
              period = (enum ny_intent_period_e)g_intent_periods[i].value;
            }
          hit += size;
        }
    }
  return nearest != NULL && at - nearest <= 9 ? period : NY_INTENT_PERIOD_NONE;
}

/****************************************************************************
 * Name: ny_intent_hour
 * Description: A spoken hour and a part of the day as a 24-hour value.
 *   晚上十二点 and 半夜十二点 are midnight, 中午十二点 is noon, 晚上一点 is
 *   one in the morning.
 ****************************************************************************/

static bool ny_intent_hour(int *hour, enum ny_intent_period_e period)
{
  int h = *hour;
  switch (period)
    {
      case NY_INTENT_PERIOD_DAWN:
        h = h == 12 ? 0 : h;
        break;
      case NY_INTENT_PERIOD_NOON:
        h = h <= 3 ? h + 12 : h;
        break;
      case NY_INTENT_PERIOD_PM:
        h = h < 12 ? h + 12 : h;
        break;
      case NY_INTENT_PERIOD_NIGHT:
        h = h == 12 ? 0 : h >= 6 && h <= 11 ? h + 12 : h;
        break;
      case NY_INTENT_PERIOD_MIDNIGHT:
        h = h == 12 ? 0 : h >= 9 && h <= 11 ? h + 12 : h;
        break;
      default:
        break;
    }
  if (h == 24)
    h = 0;
  *hour = h;
  return h >= 0 && h <= 23;
}

/****************************************************************************
 * Name: ny_intent_clock
 * Description: The first clock time: 十点半, 下午三点一刻, 7:30, 明早七点半,
 *   晚上12点, 8 pm; or a bare 中午 / 午夜.
 *
 *   点 is also "a bit" (大声一点, 早一点睡).  That reading is taken when the
 *   number is a lone 一, nothing a clock would say follows, and no part of
 *   the day comes before it.
 ****************************************************************************/

static bool ny_intent_clock(const char *text,
                            struct ny_agent_local_intent_s *intent,
                            const char **end)
{
  for (const char *p = text; *p; p += ny_intent_step(p))
    {
      double number;
      double minutes = 0;
      bool chinese;
      bool suffix = false;
      bool bare = false;
      enum ny_intent_period_e period;
      size_t size;
      if (ny_intent_numeral_before(text, p))
        continue;
      size = ny_intent_number(p, &number, &chinese);
      if (size == 0 || number > 24 || floor(number) != number)
        continue;
      const char *q = p + size;
      period = ny_intent_period(text, p);
      if (!chinese && (*q == ':' || ny_intent_starts(q, "：")) &&
          q[*q == ':' ? 1 : 3] >= '0' && q[*q == ':' ? 1 : 3] <= '5' &&
          q[*q == ':' ? 2 : 4] >= '0' && q[*q == ':' ? 2 : 4] <= '9')
        {
          q += *q == ':' ? 1 : 3;
          minutes = (q[0] - '0') * 10 + (q[1] - '0');
          q += 2;
          if (*q >= '0' && *q <= '9')
            continue;
          suffix = true;
        }
      else if (ny_intent_starts(q, "点") || ny_intent_starts(q, "點"))
        {
          q += 3;
          if (ny_intent_starts(q, "半"))
            {
              minutes = 30;
              q += strlen("半");
              suffix = true;
            }
          else if (ny_intent_starts(q, "一刻") || ny_intent_starts(q, "三刻"))
            {
              minutes = ny_intent_starts(q, "一") ? 15 : 45;
              q += strlen("一刻");
              suffix = true;
            }
          else if (ny_intent_starts(q, "整") || ny_intent_starts(q, "钟"))
            {
              q += 3;
              if (ny_intent_starts(q, "整"))
                q += strlen("整");
              suffix = true;
            }
          else if ((size = ny_intent_number(q, &minutes, NULL)) != 0)
            {
              if (minutes > 59 || floor(minutes) != minutes)
                continue;
              q += size;
              if (ny_intent_starts(q, "分"))
                q += strlen("分");
              if (ny_intent_starts(q, "钟"))
                q += strlen("钟");
              suffix = true;
            }
          if (!suffix && period == NY_INTENT_PERIOD_NONE && chinese &&
              number == 1)
            continue;
        }
      else if (chinese)
        continue;
      else
        {
          const char *r = q;
          while (*r == ' ')
            r++;
          bare = !ny_intent_starts(r, "o'clock");
          if (!bare)
            q = r + strlen("o'clock");
        }

      /* "7 pm", "7:30 a.m.": the only thing that makes a bare number a
       * time, and the last word on which half of the day is meant.
       */

      const char *r = q;
      while (*r == ' ')
        r++;
      bool morning = ny_intent_starts(r, "am") || ny_intent_starts(r, "a.m");
      bool evening = ny_intent_starts(r, "pm") || ny_intent_starts(r, "p.m");
      if ((morning || evening) && !ny_intent_letter(r[r[1] == '.' ? 3 : 2]))
        {
          period = evening        ? NY_INTENT_PERIOD_PM
                   : number == 12 ? NY_INTENT_PERIOD_DAWN
                                  : NY_INTENT_PERIOD_AM;
          q = r + (r[1] == '.' ? 3 : 2);
        }
      else if (bare)
        continue;

      int hour = (int)number;
      if (!ny_intent_hour(&hour, period))
        continue;
      intent->hour = hour;
      intent->minute = (int)minutes;
      intent->ambiguous =
          period == NY_INTENT_PERIOD_NONE && number >= 1 && number <= 12;
      if (end)
        *end = q;
      return true;
    }

  static const char *const noon[] = { "中午", "正午", "noon" };
  static const char *const midnight[] = { "午夜", "半夜", "midnight" };
  size_t size = 0;
  const char *hit = ny_intent_find(text, noon, NY_INTENT_COUNT(noon), &size);
  bool twelve = hit != NULL;
  if (!hit)
    hit = ny_intent_find(text, midnight, NY_INTENT_COUNT(midnight), &size);
  if (!hit)
    return false;
  intent->hour = twelve ? 12 : 0;
  intent->minute = 0;
  intent->ambiguous = false;
  if (end)
    *end = hit + size;
  return true;
}

/****************************************************************************
 * Name: ny_intent_percent
 * Description: 百分之三十, 30%, 最大, 一半, or the first plain number that
 *   starts within `reach` bytes of `after` and is not the start of a time or
 *   a duration.  The reach keeps "你的声音真好听，我今年30岁" out.
 ****************************************************************************/

static bool ny_intent_percent(const char *text, const char *after,
                              size_t reach, int *percent)
{
  static const char *const highest[] = { "最大", "最高", "最响", "max" };
  static const char *const lowest[] = { "最小", "最低", "最轻", "min" };
  static const char *const middle[] = { "一半", "中等", "适中", "中间" };
  static const char *const units[] = { "点", "分", "秒", "小时", "个",
                                       "首", "下", "些", "次" };
  double number;
  size_t size;
  const char *hit = strstr(text, "百分之");
  if (strstr(text, "百分百"))
    {
      *percent = 100;
      return true;
    }
  if (hit &&
      (size = ny_intent_number(hit + strlen("百分之"), &number, NULL)) != 0)
    {
      *percent = (int)number;
      return floor(number) == number && number <= 100;
    }
  if (NY_INTENT_ANY(text, highest))
    {
      *percent = 100;
      return true;
    }
  if (NY_INTENT_ANY(text, lowest))
    {
      /* The quietest level that can still be heard; silence is "mute". */

      *percent = 10;
      return true;
    }
  if (NY_INTENT_ANY(text, middle))
    {
      *percent = 50;
      return true;
    }
  for (const char *p = after; *p && (size_t)(p - after) <= reach;
       p += ny_intent_step(p))
    {
      if (ny_intent_numeral_before(text, p) ||
          (size = ny_intent_number(p, &number, NULL)) == 0)
        continue;
      if (ny_intent_unit(p + size, units, NY_INTENT_COUNT(units)))
        {
          p += size - 1;
          continue;
        }
      *percent = (int)number;
      return floor(number) == number && number <= 100;
    }
  return false;
}

/****************************************************************************
 * Name: ny_intent_extract
 * Description: Copy the owner's own words for [begin, end) -- end may be
 *   NULL for "to the end" -- without the filler around them, cut at a
 *   character boundary.
 ****************************************************************************/

static void ny_intent_extract(const struct ny_intent_view_s *view,
                              const char *begin, const char *end,
                              const char *const *leading, size_t count,
                              char *out, size_t limit)
{
  if (end == NULL)
    end = begin + strlen(begin);
  if (leading)
    {
      const char *skipped = ny_intent_skip(begin, leading, count);
      begin = skipped < end ? skipped : end;
    }
  for (size_t i = 0; i < NY_INTENT_COUNT(g_intent_trailing);)
    {
      size_t size = strlen(g_intent_trailing[i]);
      if ((size_t)(end - begin) >= size &&
          !strncmp(end - size, g_intent_trailing[i], size))
        {
          end -= size;
          i = 0;
        }
      else
        i++;
    }
  ny_agent_local_intent_copy(
      out,
      limit + 1 < NY_AGENT_LOCAL_TEXT_MAX ? limit + 1
                                          : NY_AGENT_LOCAL_TEXT_MAX,
      view->original + (begin - view->lower), (size_t)(end - begin));
}

/****************************************************************************
 * Name: ny_intent_mood
 ****************************************************************************/

static const char *ny_intent_mood(const char *text)
{
  for (size_t i = 0; i < NY_INTENT_COUNT(g_intent_moods); i++)
    {
      const char *word = g_intent_moods[i].word;
      if (ny_intent_find(text, &word, 1, NULL))
        return g_intent_moods[i].expression;
    }
  return NULL;
}

/****************************************************************************
 * Name: ny_intent_expression
 * Description: The canonical pointer for one of the 13 names, else NULL.
 ****************************************************************************/

static const char *ny_intent_expression(const char *name)
{
  for (size_t i = 0; i < NY_INTENT_COUNT(g_intent_expressions); i++)
    if (!strcmp(name, g_intent_expressions[i].word))
      return g_intent_expressions[i].word;
  return NULL;
}

/****************************************************************************
 * Name: ny_intent_remember
 * Description: 记住 / 记一下 / 帮我记 / 请记住 ... at the head of the
 *sentence; the rest is the memory.  Tried first because the rest may say
 *anything
 *   ("记一下今天天气很好" is not a weather request).
 *
 *   The model never produced usable text for this tool, so there is no
 *   model fallback: an empty remainder asks the owner.
 ****************************************************************************/

static bool ny_intent_remember(const struct ny_intent_view_s *view,
                               struct ny_agent_local_intent_s *intent)
{
  static const char *const fillers[] = {
    "请",       "麻烦",   "你", "帮我",    "给我",     "帮忙",
    "我想让你", "我要你", "要", "那",      "嗯",       "好",
    "，",       ",",      " ",  "please ", "can you ", "could you "
  };
  static const char *const cues[] = { "记住", "记一下",         "记下来",
                                      "记下", "记着",           "记好",
                                      "记",   "remember that ", "remember " };
  static const char *const questions[] = { "吗",     "吗？", "吗?",  "没",
                                           "么",     "没有", "没？", "什么",
                                           "什么？", "什么?" };
  static const char *const leading[] = { "一下", "了", "：",   ":",     "，",
                                         ",",    " ",  "这个", "这件事" };
  const char *text = view->lower;
  const char *p = ny_intent_skip(text, fillers, NY_INTENT_COUNT(fillers));
  size_t size = 0;
  for (size_t i = 0; i < NY_INTENT_COUNT(cues) && size == 0; i++)
    if (ny_intent_starts(p, cues[i]))
      size = strlen(cues[i]);

  /* A lone 记 is only a cue with the helper in front: 帮我记 yes, 记者 no. */

  if (size == 0 || (size == strlen("记") && ny_intent_starts(p, "记") &&
                    (p - text < 6 || !ny_intent_starts(p - 6, "帮我"))))
    return false;
  if (strstr(text, "待办"))
    return false; /* 帮我记个待办 is a task */
  for (size_t i = 0; i < NY_INTENT_COUNT(questions); i++)
    {
      size_t tail = strlen(questions[i]);
      size_t length = strlen(text);
      if (length >= tail && !strcmp(text + length - tail, questions[i]))
        return false; /* 你记住了吗 asks, it does not tell */
    }
  intent->kind = NY_AGENT_LOCAL_REMEMBER;
  ny_intent_extract(view, p + size, NULL, leading, NY_INTENT_COUNT(leading),
                    intent->text, NY_AGENT_LOCAL_TEXT_MAX - 1);
  intent->complete = strlen(intent->text) >= 4;
  return true;
}

/****************************************************************************
 * Name: ny_intent_task
 * Description: 待办 / todo with the item after it ("加个待办：周五交报告") or
 *   in front of it ("把买牛奶加到待办里").
 ****************************************************************************/

static bool ny_intent_task(const struct ny_intent_view_s *view,
                           struct ny_agent_local_intent_s *intent)
{
  static const char *const nouns[] = { "待办事项", "待办", "to-do", "todo" };
  /* What may stand between the noun and the item: "待办加一条：周五交报告"
   * (board, 2026-09-20) kept "加一条：" in the title before the verbs were
   * listed.  Bare 加, 记 and bare classifiers are left out on purpose: an
   * item may begin with them ("待办：记得买菜").  Longer forms first; the
   * skip is greedy.
   */

  static const char *const leading[] = {
    "清单",   "列表",     "里面",     "里",       "中",     "再添加",
    "再加上", "添加",     "增加",     "新增",     "新建",   "加上",
    "记上",   "再加一条", "再加一项", "再加一个", "加一条", "加一项",
    "加一个", "记一条",   "记一项",   "记一个",   "加个",   "记个",
    "一条",   "一项",     "一个",     "：",       ":",      "，",
    ",",      " ",        "是",       "为",       "叫",     "list"
  };
  static const char *const fronts[] = { "把",   "将", "请", "帮我",
                                        "给我", "，", ",",  " " };
  static const char *const moves[] = { "添加到", "加到", "加进",  "加入",
                                       "放到",   "放进", "记到",  "记进",
                                       "写到",   "写进", "to my", "to the" };
  const char *text = view->lower;
  size_t size = 0;
  const char *noun =
      ny_intent_find(text, nouns, NY_INTENT_COUNT(nouns), &size);
  if (!noun)
    return false;
  if (NY_INTENT_ANY(text, g_intent_list_cues))
    {
      intent->kind = NY_AGENT_LOCAL_TASK_LIST;
      intent->complete = true;
      return true;
    }
  if (NY_INTENT_ANY(text, g_intent_discussion))
    return false;
  intent->kind = NY_AGENT_LOCAL_TASK;
  ny_intent_extract(view, noun + size, NULL, leading, NY_INTENT_COUNT(leading),
                    intent->text, NY_INTENT_TITLE_MAX);
  if (strlen(intent->text) < 4)
    {
      const char *move =
          ny_intent_find(text, moves, NY_INTENT_COUNT(moves), NULL);
      intent->text[0] = 0;
      if (move && move < noun)
        ny_intent_extract(view, text, move, fronts, NY_INTENT_COUNT(fronts),
                          intent->text, NY_INTENT_TITLE_MAX);
    }
  intent->complete = strlen(intent->text) >= 4;
  return true;
}

/****************************************************************************
 * Name: ny_intent_lists
 ****************************************************************************/

static bool ny_intent_lists(const char *text,
                            struct ny_agent_local_intent_s *intent)
{
  static const char *const timers[] = { "计时器", "倒计时", "timers",
                                        "timer" };
  static const char *const alarms[] = { "闹钟", "alarms", "alarm" };
  if (!NY_INTENT_ANY(text, g_intent_list_cues))
    return false;
  if (NY_INTENT_ANY(text, timers))
    intent->kind = NY_AGENT_LOCAL_TIMER_LIST;
  else if (NY_INTENT_ANY(text, alarms))
    intent->kind = NY_AGENT_LOCAL_ALARM_LIST;
  else
    return false;
  intent->complete = true;
  return true;
}

/****************************************************************************
 * Name: ny_intent_volume
 * Description: Mute, unmute, an absolute level, or louder / quieter.
 *
 *   "太吵了小声点" is here and not with the model, which answered 0 for it:
 *   a relative change needs the current level, and that is the product's to
 *   know, not the model's.
 ****************************************************************************/

static bool ny_intent_volume(const char *text,
                             struct ny_agent_local_intent_s *intent)
{
  static const char *const sounds[] = { "音量", "声音", "volume" };
  static const char *const unmute[] = { "取消静音", "解除静音", "关闭静音",
                                        "关掉静音", "别静音",   "不要静音",
                                        "恢复声音", "打开声音", "unmute" };
  static const char *const mute[] = { "静音", "别出声", "不要出声", "mute" };
  static const char *const louder[] = { "大声点",     "大声一点", "大点声",
                                        "大声些",     "声音大点", "louder",
                                        "turn it up", "volume up" };
  static const char *const quieter[] = { "小声点",       "小声一点",
                                         "小点声",       "小声些",
                                         "声音小点",     "quieter",
                                         "turn it down", "volume down" };
  static const char *const higher[] = { "大一点", "大一些",  "大点", "调高",
                                        "调大",   "高一点",  "高点", "响一点",
                                        "响点",   "加大",    "增大", "提高",
                                        "太小",   "turn up", "up" };
  static const char *const lower[] = {
    "小一点", "小一些", "小点",      "调低", "调小", "低一点",
    "低点",   "轻一点", "轻点",      "减小", "降低", "太大",
    "太响",   "太吵",   "turn down", "down"
  };
  static const char *const targets[] = { "到",      "成",   "为",   "至",
                                         "%",       "％",   "百分", "最大",
                                         "最小",    "最高", "最低", "一半",
                                         "percent", "to",   "at" };
  static const char *const verbs[] = { "调",  "设",     "改",     "开到",
                                       "set", "change", "adjust", "turn" };
  static const char *const deltas[] = { "增加", "减少", "提高", "降低",
                                        "调高", "调低", "调大", "调小",
                                        "加",   "减",   "by" };
  size_t size = 0;
  const char *sound =
      ny_intent_find(text, sounds, NY_INTENT_COUNT(sounds), &size);
  bool up =
      NY_INTENT_ANY(text, louder) || (sound && NY_INTENT_ANY(text, higher));
  bool down = NY_INTENT_ANY(text, quieter) ||
              (sound && NY_INTENT_ANY(text, lower)) ||
              (strstr(text, "太吵") && strlen(text) <= 18);
  intent->complete = true;
  if (NY_INTENT_ANY(text, unmute))
    intent->kind = NY_AGENT_LOCAL_UNMUTE;
  else if (NY_INTENT_ANY(text, mute))
    intent->kind = NY_AGENT_LOCAL_MUTE;
  else if (sound && NY_INTENT_ANY(text, targets) &&
           ny_intent_percent(text, sound + size, NY_AGENT_LOCAL_TEXT_MAX,
                             &intent->percent))
    intent->kind = NY_AGENT_LOCAL_VOLUME_SET;
  else if (up || down)
    {
      /* Both at once ("太小了还是太大了") is not a command. */

      if (up && down && !NY_INTENT_ANY(text, quieter) &&
          !NY_INTENT_ANY(text, louder))
        return false;
      double step;
      size_t length = 0;
      const char *delta =
          ny_intent_find(text, deltas, NY_INTENT_COUNT(deltas), &length);
      intent->kind = NY_INTENT_ANY(text, quieter) || (down && !up)
                         ? NY_AGENT_LOCAL_VOLUME_DOWN
                         : NY_AGENT_LOCAL_VOLUME_UP;
      intent->percent = NY_AGENT_LOCAL_VOLUME_STEP;
      if (delta)
        {
          delta += length;
          while (*delta == ' ')
            delta++;
          size_t digits = ny_intent_number(delta, &step, NULL);
          if (digits && step >= 1 && step <= 100 && floor(step) == step &&
              !ny_intent_starts(delta + digits, "点"))
            intent->percent = (int)step;
        }
    }
  else if (sound && ny_intent_percent(text, sound + size, 9, &intent->percent))
    intent->kind = NY_AGENT_LOCAL_VOLUME_SET; /* 音量30, volume 40 */
  else if (sound && NY_INTENT_ANY(text, verbs))
    {
      intent->kind = NY_AGENT_LOCAL_VOLUME_SET;
      intent->complete = false;
    }
  else
    return false;
  return true;
}

/****************************************************************************
 * Name: ny_intent_schedule
 * Description: A countdown (a duration) or an alarm (a clock time).
 *
 *   "十分钟后提醒我关火" is a countdown with a label; "晚上十点提醒我睡觉" is
 *   an alarm with one; "提醒我周五交报告" has neither and is left for the
 *   task rule.
 ****************************************************************************/

static bool ny_intent_schedule(const struct ny_intent_view_s *view,
                               struct ny_agent_local_intent_s *intent)
{
  static const char *const timers[] = { "计时器",    "倒计时", "定时", "计时",
                                        "countdown", "timers", "timer" };
  static const char *const alarms[] = { "闹钟", "闹铃", "alarms", "alarm" };
  static const char *const wakes[] = { "叫醒", "wake" };
  static const char *const actions[] = { "定",     "设",     "开",    "帮我",
                                         "来个",   "来一个", "建",    "加",
                                         "启动",   "开始",   "start", "set",
                                         "create", "add",    "new" };
  static const char *const afters[] = { "后",   "之后",   "以后",     "过后",
                                        "钟后", " later", " from now" };
  static const char *const every[] = { "每天",      "天天",     "每日",
                                       "every day", "everyday", "daily" };
  static const char *const weekdays[] = { "工作日", "weekdays" };
  static const char *const weekend[] = { "周末", "weekends", "weekend" };
  static const char *const far[] = {
    "后天",     "下周",   "下星期",  "下个星期",  "下礼拜",   "下个月",
    "明年",     "周一",   "周二",    "周三",      "周四",     "周五",
    "周六",     "周日",   "周天",    "星期",      "礼拜",     "号",
    "月",       "monday", "tuesday", "wednesday", "thursday", "friday",
    "saturday", "sunday"
  };
  static const char *const mornings[] = { "起床", "早饭", "早餐",   "上班",
                                          "上学", "晨跑", "morning" };
  static const char *const evenings[] = { "晚饭", "晚餐", "下班", "下午茶" };
  static const char *const nights[] = { "睡觉", "夜宵", "睡" };
  const char *text = view->lower;
  const char *span = NULL;
  const char *span_end = NULL;
  const char *clock_end = NULL;
  size_t size = 0;
  const char *remind = ny_intent_find(text, g_intent_remind,
                                      NY_INTENT_COUNT(g_intent_remind), &size);
  bool timer = NY_INTENT_ANY(text, timers);
  bool alarm = NY_INTENT_ANY(text, alarms);
  bool action = NY_INTENT_ANY(text, actions);
  bool duration = ny_intent_duration(text, &intent->seconds, &span, &span_end);

  /* 计时 is a verb on its own; 计时器 is a noun and wants one. */

  bool verb = false;
  for (const char *hit = text; (hit = strstr(hit, "计时")) != NULL; hit += 6)
    if (!ny_intent_starts(hit + 6, "器"))
      verb = true;
  verb = verb || strstr(text, "倒计时") != NULL;

  if (duration &&
      (timer || alarm ||
       (remind && (ny_intent_unit(span_end, afters, NY_INTENT_COUNT(afters)) ||
                   (span - text >= 3 && ny_intent_starts(span - 3, "in "))))))
    {
      intent->kind = NY_AGENT_LOCAL_TIMER;
      intent->complete = intent->seconds <= NY_INTENT_TIMER_MAX;
      if (remind)
        {
          const char *from =
              remind + size > span_end ? remind + size : span_end;
          from += ny_intent_unit(from, afters, NY_INTENT_COUNT(afters));
          if (ny_intent_find(from, g_intent_remind,
                             NY_INTENT_COUNT(g_intent_remind), &size) == from)
            from += size;
          ny_intent_extract(view, from, NULL, g_intent_label_leading,
                            NY_INTENT_COUNT(g_intent_label_leading),
                            intent->text, NY_INTENT_LABEL_MAX);
        }
      return true;
    }
  if (!duration && (verb || (timer && action)) && !alarm)
    {
      intent->kind = NY_AGENT_LOCAL_TIMER;
      return true;
    }

  bool wake = NY_INTENT_ANY(text, wakes);
  if (!alarm && !remind && !wake)
    return false;
  if (!ny_intent_clock(text, intent, &clock_end))
    {
      if (!alarm || !action)
        return false;
      intent->kind = NY_AGENT_LOCAL_ALARM;
      return true;
    }
  intent->kind = NY_AGENT_LOCAL_ALARM;
  intent->complete = true;
  if (NY_INTENT_ANY(text, every))
    intent->repeat = 0x7f;
  else if (NY_INTENT_ANY(text, weekdays))
    intent->repeat = 0x3e;
  else if (NY_INTENT_ANY(text, weekend))
    intent->repeat = 0x41;
  else
    intent->far_date = NY_INTENT_ANY(text, far);

  /* A date the alarm record cannot hold is not something to guess at. */

  intent->complete = !intent->far_date;
  if (remind)
    {
      const char *from = remind + size > clock_end ? remind + size : clock_end;
      ny_intent_extract(view, from, NULL, g_intent_label_leading,
                        NY_INTENT_COUNT(g_intent_label_leading), intent->text,
                        NY_INTENT_LABEL_MAX);
    }

  /* "七点叫我起床" names no half of the day but implies one.  What is left
   * ambiguous is settled against the clock when the call is built.
   */

  if (intent->ambiguous)
    {
      enum ny_intent_period_e period =
          NY_INTENT_ANY(text, mornings)   ? NY_INTENT_PERIOD_AM
          : NY_INTENT_ANY(text, evenings) ? NY_INTENT_PERIOD_PM
          : NY_INTENT_ANY(text, nights)   ? NY_INTENT_PERIOD_NIGHT
                                          : NY_INTENT_PERIOD_NONE;
      if (period != NY_INTENT_PERIOD_NONE)
        {
          ny_intent_hour(&intent->hour, period);
          intent->ambiguous = false;
        }
    }
  return true;
}

/****************************************************************************
 * Name: ny_intent_music
 ****************************************************************************/

static bool ny_intent_music(const struct ny_intent_view_s *view,
                            struct ny_agent_local_intent_s *intent)
{
  static const char *const stops[] = {
    "停止播放", "停止音乐", "关掉音乐",       "关闭音乐",   "把音乐关",
    "关音乐",   "别放了",   "不要放了",       "别唱了",     "不听了",
    "音乐停",   "停止放歌", "stop the music", "stop music", "stop playing"
  };
  static const char *const resumes[] = { "继续播放", "继续放",   "接着放",
                                         "接着播",   "恢复播放", "继续音乐",
                                         "resume" };
  static const char *const pauses[] = { "暂停", "pause" };
  static const char *const media[] = { "音乐", "播放", "歌", "music", "song" };
  static const char *const others[] = { "计时", "闹钟", "timer", "alarm" };
  static const char *const plain[] = { "放音乐",         "放歌",
                                       "听歌",           "听音乐",
                                       "放点音乐",       "play music",
                                       "play some music" };
  static const char *const plays[] = { "播放", "放一首", "放首",   "来一首",
                                       "来首", "放一下", "放个",   "放点",
                                       "放些", "我想听", "我要听", "想听",
                                       "play " };
  static const char *const leading[] = { "一首",  "一下", "一个", "一点", "首",
                                         "个",    "点",   "些",   "的",   " ",
                                         "some ", "a ",   "the " };
  static const char *const generic[] = { "音乐",  "歌",   "歌曲", "首歌",
                                         "music", "song", "songs" };
  static const char *const spoken[] = { "故事", "笑话",  "新闻", "天气",
                                        "你",   "story", "joke", "news" };
  const char *text = view->lower;
  size_t size = 0;
  const char *play;
  intent->complete = true;
  if (NY_INTENT_ANY(text, stops))
    intent->kind = NY_AGENT_LOCAL_MUSIC_STOP;
  else if (NY_INTENT_ANY(text, resumes))
    intent->kind = NY_AGENT_LOCAL_MUSIC_RESUME;
  else if (NY_INTENT_ANY(text, pauses) && !NY_INTENT_ANY(text, others) &&
           (NY_INTENT_ANY(text, media) || strlen(text) <= 15))
    intent->kind = NY_AGENT_LOCAL_MUSIC_PAUSE;
  else if (NY_INTENT_ANY(text, plain))
    intent->kind = NY_AGENT_LOCAL_MUSIC_PLAY;
  else if ((play = ny_intent_find(text, plays, NY_INTENT_COUNT(plays),
                                  &size)) != NULL)
    {
      if (NY_INTENT_ANY(play + size, spoken))
        return false; /* 我想听故事 is for the model to tell */
      intent->kind = NY_AGENT_LOCAL_MUSIC_PLAY;
      ny_intent_extract(view, play + size, NULL, leading,
                        NY_INTENT_COUNT(leading), intent->text,
                        NY_INTENT_LABEL_MAX);
      for (size_t i = 0; i < NY_INTENT_COUNT(generic); i++)
        if (!strcmp(intent->text, generic[i]))
          intent->text[0] = 0;
    }
  else
    return false;
  return true;
}

/****************************************************************************
 * Name: ny_intent_face
 * Description: An expression needs a frame ("表情", "眼睛") with a verb, or
 *   one of the idioms that only ever mean the face ("卖个萌", "笑一个").
 *   "我今天很生气" has an emotion and neither, and stays chat.
 ****************************************************************************/

static bool ny_intent_face(const char *text,
                           struct ny_agent_local_intent_s *intent)
{
  static const char *const frames[] = { "表情", "眼睛",       "眼神",
                                        "脸",   "expression", "face" };
  static const char *const strict[] = { "表情", "expression" };
  static const char *const acts[] = { "换",  "变",   "做", "来",   "摆",
                                      "切",  "给我", "露", "显示", "装",
                                      "演",  "扮",   "要", "show", "make",
                                      "set", "give", "do" };
  static const char *const idioms[] = { "卖个萌", "卖萌", "笑一个", "笑一下",
                                        "比个心", "比心", "装睡",   "smile" };
  static const char *const stage[] = { "假装", "装作",   "装出",
                                       "表演", "演一个", "演个",
                                       "做出", "扮个",   "扮演" };
  static const char *const bedtime[] = { "睡觉吧", "睡吧", "去睡觉",
                                         "go to sleep" };
  const char *mood = ny_intent_mood(text);
  bool frame = NY_INTENT_ANY(text, frames);
  bool act = NY_INTENT_ANY(text, acts);
  intent->complete = true;
  if (mood && ((frame && act) || NY_INTENT_ANY(text, idioms) ||
               NY_INTENT_ANY(text, stage)))
    intent->expression = mood;
  else if (NY_INTENT_ANY(text, bedtime) && !strstr(text, "我"))
    intent->expression = "sleep";
  else if (NY_INTENT_ANY(text, strict) && act)
    intent->complete = false; /* 换个表情: which one is the model's guess */
  else
    return false;
  intent->kind = NY_AGENT_LOCAL_EXPRESSION;
  return true;
}

/****************************************************************************
 * Name: ny_intent_reminder
 * Description: 提醒我 with neither a time nor a duration is a to-do.
 ****************************************************************************/

static bool ny_intent_reminder(const struct ny_intent_view_s *view,
                               struct ny_agent_local_intent_s *intent)
{
  static const char *const cues[] = { "提醒我", "remind me" };
  size_t size = 0;
  const char *cue =
      ny_intent_find(view->lower, cues, NY_INTENT_COUNT(cues), &size);
  if (!cue)
    return false;
  ny_intent_extract(view, cue + size, NULL, g_intent_label_leading,
                    NY_INTENT_COUNT(g_intent_label_leading), intent->text,
                    NY_INTENT_TITLE_MAX);

  /* "你会提醒我吗" leaves nothing to be reminded of: that is a question. */

  if (strlen(intent->text) < 4 || !strcmp(intent->text, "吗"))
    {
      intent->text[0] = 0;
      return false;
    }
  intent->kind = NY_AGENT_LOCAL_TASK;
  intent->complete = true;
  return true;
}

/****************************************************************************
 * Name: ny_intent_reads
 ****************************************************************************/

static bool ny_intent_reads(const char *text,
                            struct ny_agent_local_intent_s *intent)
{
  static const char *const times[] = {
    "几点了",     "几点钟",    "几点啦",       "几点呢",     "现在几点",
    "现在是几点", "现在时间",  "当前时间",     "什么时间了", "现在什么时间",
    "报时",       "what time", "current time", "the time"
  };
  static const char *const dates[] = { "几号",        "星期几",     "周几",
                                       "礼拜几",      "今天日期",   "什么日期",
                                       "几月几",      "今天是哪天", "哪一天",
                                       "what day",    "what date",  "the date",
                                       "today's date" };
  static const char *const weather[] = { "天气", "气温", "weather",
                                         "forecast" };
  static const char *const sky[] = { "下雨",   "下雪", "带伞", "冷不冷",
                                     "热不热", "rain", "snow", "umbrella" };
  static const char *const moment[] = { "今天",     "明天", "现在",
                                        "外面",     "今晚", "会不会",
                                        "要不要",   "出门", "today",
                                        "tomorrow", "now",  "outside" };
  static const char *const taste[] = { "喜欢", "讨厌", "like" };
  static const char *const status[] = {
    "设备状态", "系统状态",      "运行状态",      "你的状态",   "内存",
    "存储空间", "剩余空间",      "磁盘",          "运行了多久", "开机多久",
    "运行多久", "ip地址",        "ip 地址",       "网络状态",   "wifi",
    "wi-fi",    "device status", "system status", "uptime",     "cpu",
    "memory"
  };
  intent->complete = true;
  if (NY_INTENT_ANY(text, times))
    intent->kind = NY_AGENT_LOCAL_TIME;
  else if (NY_INTENT_ANY(text, dates))
    intent->kind = NY_AGENT_LOCAL_DATE;
  else if (!NY_INTENT_ANY(text, taste) &&
           (NY_INTENT_ANY(text, weather) ||
            (NY_INTENT_ANY(text, sky) && NY_INTENT_ANY(text, moment))))
    intent->kind = NY_AGENT_LOCAL_WEATHER;
  else if (NY_INTENT_ANY(text, status))
    intent->kind = NY_AGENT_LOCAL_STATUS;
  else
    return false;
  return true;
}

/****************************************************************************
 * Name: ny_agent_local_intent_copy
 ****************************************************************************/

size_t ny_agent_local_intent_copy(char *out, size_t size, const char *text,
                                  size_t length)
{
  if (size == 0)
    return 0;
  if (length > size - 1)
    {
      length = size - 1;
      while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80)
        length--;
    }
  memcpy(out, text, length);
  out[length] = 0;
  return length;
}

/****************************************************************************
 * Name: ny_intent_reset
 ****************************************************************************/

static void ny_intent_reset(struct ny_agent_local_intent_s *intent,
                            bool numerals)
{
  memset(intent, 0, sizeof(*intent));
  intent->numerals = numerals;
  intent->percent = -1;
  intent->hour = -1;
  intent->minute = -1;
}

/****************************************************************************
 * Name: ny_agent_local_intent_detect
 ****************************************************************************/

void ny_agent_local_intent_detect(const char *utterance,
                                  struct ny_agent_local_intent_s *intent)
{
  char lower[NY_AGENT_LOCAL_UTTERANCE_MAX];
  struct ny_intent_view_s view = { lower, utterance };
  size_t length = utterance ? strlen(utterance) : 0;
  bool numerals;
  bool found;
  ny_intent_reset(intent, false);
  if (length == 0 || length >= sizeof(lower) ||
      !ny_utf8_valid((const unsigned char *)utterance, length))
    return;
  for (size_t i = 0; i <= length; i++)
    lower[i] = utterance[i] >= 'A' && utterance[i] <= 'Z'
                   ? (char)(utterance[i] - 'A' + 'a')
                   : utterance[i];
  numerals = ny_intent_numerals(lower);

  /* Free text first: what follows 记住 or 待办 may mention anything.  Then
   * the reads that share a noun with an action ("有哪些闹钟"), then the
   * actions unless the sentence is an opinion question, then plain reads.
   * A rule that declines may have scribbled on the slots, so each one
   * starts from a clean intent.
   */

#define NY_INTENT_TRY(rule) (ny_intent_reset(intent, numerals), (rule))
  found = NY_INTENT_TRY(ny_intent_remember(&view, intent)) ||
          NY_INTENT_TRY(ny_intent_task(&view, intent)) ||
          NY_INTENT_TRY(ny_intent_lists(lower, intent));
  if (!found && !NY_INTENT_ANY(lower, g_intent_discussion))
    found = NY_INTENT_TRY(ny_intent_volume(lower, intent)) ||
            NY_INTENT_TRY(ny_intent_schedule(&view, intent)) ||
            NY_INTENT_TRY(ny_intent_music(&view, intent)) ||
            NY_INTENT_TRY(ny_intent_face(lower, intent)) ||
            NY_INTENT_TRY(ny_intent_reminder(&view, intent));
  if (!found)
    found = NY_INTENT_TRY(ny_intent_reads(lower, intent));
#undef NY_INTENT_TRY
  if (!found)
    ny_intent_reset(intent, numerals);
}

/****************************************************************************
 * Name: ny_agent_local_intent_name
 ****************************************************************************/

const char *ny_agent_local_intent_name(enum ny_agent_local_intent_e kind)
{
  static const char *const names[] = {
    "chat",       "time",       "date",        "weather",      "status",
    "timer.list", "alarm.list", "task.list",   "expression",   "timer",
    "alarm",      "volume.set", "volume.up",   "volume.down",  "mute",
    "unmute",     "music.play", "music.pause", "music.resume", "music.stop",
    "remember",   "task"
  };
  return (size_t)kind < NY_INTENT_COUNT(names) ? names[kind] : "?";
}

/****************************************************************************
 * Name: ny_agent_local_intent_revision
 ****************************************************************************/

const char *ny_agent_local_intent_revision(enum ny_agent_local_intent_e kind)
{
  return kind == NY_AGENT_LOCAL_TIMER      ? "timer.list"
         : kind == NY_AGENT_LOCAL_ALARM    ? "alarm.list"
         : kind == NY_AGENT_LOCAL_REMEMBER ? "memory.list"
         : kind == NY_AGENT_LOCAL_TASK     ? "task.list"
                                           : NULL;
}

/****************************************************************************
 * Name: ny_agent_local_intent_forced
 * Description: The single tool the model is made to call.
 *
 *   Only the slots it was measured to read: minutes, a clock time, a
 *   percentage, an expression.  It failed on free text (remember) and on
 *   tools without parameters, so those never come here.
 ****************************************************************************/

const char *
ny_agent_local_intent_forced(const struct ny_agent_local_intent_s *intent,
                             const char **name)
{
  switch (intent->kind)
    {
      case NY_AGENT_LOCAL_TIMER:

        /* A duration the rules did read and refused is not the model's to
         * read again.
         */

        *name = "set_timer";
        return intent->numerals && intent->seconds == 0 ? g_intent_forced_timer
                                                        : NULL;
      case NY_AGENT_LOCAL_ALARM:
        *name = "set_alarm";
        return intent->numerals && !intent->far_date ? g_intent_forced_alarm
                                                     : NULL;
      case NY_AGENT_LOCAL_VOLUME_SET:
        *name = "set_volume";
        return intent->numerals ? g_intent_forced_volume : NULL;
      case NY_AGENT_LOCAL_EXPRESSION:
        *name = "set_expression";
        return g_intent_forced_expression;
      default:
        *name = NULL;
        return NULL;
    }
}

/****************************************************************************
 * Name: ny_agent_local_intent_fill
 * Description: Take the slot from the forced call if it is believable.
 ****************************************************************************/

int ny_agent_local_intent_fill(struct ny_agent_local_intent_s *intent,
                               const char *name, const cJSON *arguments)
{
  const char *expected;
  if (!ny_agent_local_intent_forced(intent, &expected) || !name ||
      strcmp(name, expected) || !cJSON_IsObject(arguments))
    return -EINVAL;
  if (intent->kind == NY_AGENT_LOCAL_EXPRESSION)
    {
      char word[32];
      const char *said = ny_intent_text(arguments, "expression");
      size_t length = strlen(said);
      if (length == 0 || length >= sizeof(word))
        return -EINVAL;
      for (size_t i = 0; i <= length; i++)
        word[i] =
            said[i] >= 'A' && said[i] <= 'Z' ? (char)(said[i] + 32) : said[i];
      intent->expression = ny_intent_expression(word);
      for (size_t i = 0;
           !intent->expression && i < NY_INTENT_COUNT(g_intent_near); i++)
        if (!strcmp(word, g_intent_near[i].word))
          intent->expression = g_intent_near[i].expression;
      if (!intent->expression)
        return -EINVAL;
    }
  else if (intent->kind == NY_AGENT_LOCAL_ALARM)
    {
      const char *time = ny_intent_text(arguments, "time");
      int hour;
      int minute;
      char tail;
      if (sscanf(time, "%2d:%2d%c", &hour, &minute, &tail) != 2 || hour < 0 ||
          hour > 23 || minute < 0 || minute > 59)
        return -EINVAL;
      intent->hour = hour;
      intent->minute = minute;
      intent->ambiguous = false;
    }
  else
    {
      /* Numbers arrive as numbers or as strings, depending on the day. */

      bool timer = intent->kind == NY_AGENT_LOCAL_TIMER;
      const cJSON *item = cJSON_GetObjectItemCaseSensitive(
          arguments, timer ? "minutes" : "percent");
      double number = cJSON_IsNumber(item) ? item->valuedouble
                      : cJSON_IsString(item) && item->valuestring[0]
                          ? strtod(item->valuestring, NULL)
                          : NAN;
      if (!isfinite(number))
        return -EINVAL;
      if (timer)
        {
          if (number * 60 < 1 || number * 60 > NY_INTENT_TIMER_MAX)
            return -EINVAL;
          intent->seconds = (uint32_t)(number * 60 + 0.5);
        }
      else
        {
          if (number < 0 || number > 100 || floor(number) != number)
            return -EINVAL;
          intent->percent = (int)number;
        }
    }
  intent->complete = true;
  return 0;
}

/****************************************************************************
 * Name: ny_agent_local_intent_question
 * Description: What to ask when neither the rules nor the model have the
 *   slot.  Worded without "无法" / "I cannot": the agent loop reads those as
 *   a weak answer and retries on a premium backend.
 ****************************************************************************/

const char *
ny_agent_local_intent_question(const struct ny_agent_local_intent_s *intent)
{
  switch (intent->kind)
    {
      case NY_AGENT_LOCAL_TIMER:
        return intent->seconds > NY_INTENT_TIMER_MAX
                   ? "计时器最长可以定 24 小时，更久的话我帮你定个闹钟吧，"
                     "告诉我几点就好。"
                   : "要计时多久呢？比如说“计时五分钟”。";
      case NY_AGENT_LOCAL_ALARM:
        return intent->far_date
                   ? "我现在只会定最近一次响的闹钟，或者每天、工作日、周末"
                     "重复的闹钟。换个说法试试，比如“明早七点叫我”。"
                   : "闹钟要定在几点呢？比如说“明早七点半叫我”。";
      case NY_AGENT_LOCAL_VOLUME_SET:
        return "想把音量调到多少呢？说一个 0 到 100 之间的数字就行。";
      case NY_AGENT_LOCAL_EXPRESSION:
        return "想看什么表情呢？开心、生气、难过、卖萌、惊讶、犯困，"
               "我都会哦。";
      case NY_AGENT_LOCAL_REMEMBER:
        return "要我记住什么呢？直接说“记住”加上内容就好。";
      case NY_AGENT_LOCAL_TASK:
        return "要添加什么待办呢？比如说“加个待办：周五交报告”。";
      default:
        return "我没有听清楚，可以再说一遍吗？";
    }
}

/****************************************************************************
 * Name: ny_agent_local_intent_track
 * Description: Match the hint against the library's file names, ignoring
 *   ASCII case; no hint plays the first supported file.  -ENOENT: nothing
 *   playable at all.  -ESRCH: the named piece is not there -- playing
 *   something else instead would be answering a different request.
 ****************************************************************************/

int ny_agent_local_intent_track(const cJSON *library, const char *hint,
                                char *name, size_t size)
{
  const cJSON *item;
  bool any = false;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(library, "items"))
  {
    char lower[256];
    const char *file = ny_intent_text(item, "name");
    size_t length = strlen(file);
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "supported")) ||
        length == 0 || length >= sizeof(lower) || length >= size)
      continue;
    any = true;
    for (size_t i = 0; i <= length; i++)
      lower[i] =
          file[i] >= 'A' && file[i] <= 'Z' ? (char)(file[i] + 32) : file[i];
    if (!hint || !hint[0] || strstr(lower, hint))
      {
        memcpy(name, file, length + 1);
        return 0;
      }
  }
  return any ? -ESRCH : -ENOENT;
}

/****************************************************************************
 * Name: ny_intent_alarm_time
 * Description: "七点" with no half of the day is the next seven o'clock to
 *   come round, which is what a person setting an alarm means.  Without a
 *   clock there is nothing to compare with and the words are taken as said.
 ****************************************************************************/

static void ny_intent_alarm_time(const struct ny_agent_local_intent_s *intent,
                                 int now_minutes, int *hour, int *minute)
{
  *hour = intent->hour;
  *minute = intent->minute;
  if (!intent->ambiguous || now_minutes < 0)
    return;
  int early = (intent->hour % 12) * 60 + intent->minute;
  int late = early + 720;
  int wait_early = (early - now_minutes + 1440) % 1440;
  int wait_late = (late - now_minutes + 1440) % 1440;
  if (wait_early == 0)
    wait_early = 1440;
  if (wait_late == 0)
    wait_late = 1440;
  *hour = (wait_early <= wait_late ? early : late) / 60;
}

/****************************************************************************
 * Name: ny_agent_local_intent_call
 * Description: The exact input ny_agent_tool_execute() accepts: one key for
 *   nyabula_read and nyabula_expression, topic plus arguments for
 *   nyabula_action and nyabula_music.
 ****************************************************************************/

char *
ny_agent_local_intent_call(const struct ny_agent_local_intent_s *intent,
                           const struct ny_agent_local_context_s *context,
                           const char **tool)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *arguments = NULL;
  cJSON *record = NULL;
  const char *topic = NULL;
  char *encoded = NULL;
  bool ok = root != NULL;
  bool create = false;
  int volume = context->volume;
  bool muted = false;
  *tool = "nyabula_read";
  switch (intent->kind)
    {
      case NY_AGENT_LOCAL_TIME:
      case NY_AGENT_LOCAL_DATE:
        topic = "system.time.get";
        break;
      case NY_AGENT_LOCAL_WEATHER:
        topic = "weather.get";
        break;
      case NY_AGENT_LOCAL_STATUS:
        topic = "device.status";
        break;
      case NY_AGENT_LOCAL_TIMER_LIST:
        topic = "timer.list";
        break;
      case NY_AGENT_LOCAL_ALARM_LIST:
        topic = "alarm.list";
        break;
      case NY_AGENT_LOCAL_TASK_LIST:
        topic = "task.list";
        break;
      case NY_AGENT_LOCAL_EXPRESSION:
        *tool = "nyabula_expression";
        ok = ok && intent->expression &&
             cJSON_AddStringToObject(root, "expression", intent->expression);
        break;
      case NY_AGENT_LOCAL_VOLUME_SET:
        *tool = "nyabula_music";
        topic = "music.volume";
        volume = intent->percent;
        break;
      case NY_AGENT_LOCAL_VOLUME_UP:
        *tool = "nyabula_music";
        topic = "music.volume";
        volume =
            volume + intent->percent > 100 ? 100 : volume + intent->percent;
        break;
      case NY_AGENT_LOCAL_VOLUME_DOWN:
        *tool = "nyabula_music";
        topic = "music.volume";
        volume =
            volume - intent->percent < NY_INTENT_QUIET_MIN
                ? (volume < NY_INTENT_QUIET_MIN ? volume : NY_INTENT_QUIET_MIN)
                : volume - intent->percent;
        muted = context->muted;
        break;
      case NY_AGENT_LOCAL_MUTE:
      case NY_AGENT_LOCAL_UNMUTE:
        *tool = "nyabula_music";
        topic = "music.volume";
        muted = intent->kind == NY_AGENT_LOCAL_MUTE;
        break;
      case NY_AGENT_LOCAL_MUSIC_PLAY:
        *tool = "nyabula_music";
        topic = "music.play";
        break;
      case NY_AGENT_LOCAL_MUSIC_PAUSE:
        *tool = "nyabula_music";
        topic = "music.pause";
        break;
      case NY_AGENT_LOCAL_MUSIC_RESUME:
        *tool = "nyabula_music";
        topic = "music.resume";
        break;
      case NY_AGENT_LOCAL_MUSIC_STOP:
        *tool = "nyabula_music";
        topic = "music.stop";
        break;
      case NY_AGENT_LOCAL_TIMER:
        topic = "timer.create";
        create = true;
        break;
      case NY_AGENT_LOCAL_ALARM:
        topic = "alarm.create";
        create = true;
        break;
      case NY_AGENT_LOCAL_REMEMBER:
        topic = "memory.create";
        create = true;
        break;
      case NY_AGENT_LOCAL_TASK:
        topic = "task.create";
        create = true;
        break;
      default:
        ok = false;
        break;
    }
  if (create)
    *tool = "nyabula_action";
  ok = ok && (!topic || cJSON_AddStringToObject(root, "topic", topic));
  if (ok && strcmp(*tool, "nyabula_read") &&
      strcmp(*tool, "nyabula_expression"))
    {
      arguments = cJSON_AddObjectToObject(root, "arguments");
      ok = arguments != NULL;
    }
  if (ok && create)
    ok = cJSON_AddNumberToObject(arguments, "revision", context->revision) !=
         NULL;
  if (ok && create && intent->kind != NY_AGENT_LOCAL_TIMER)
    {
      record = cJSON_AddObjectToObject(arguments, "record");
      ok = record != NULL;
    }
  if (ok && topic && !strcmp(topic, "music.volume"))
    ok = cJSON_AddNumberToObject(arguments, "volume", volume) &&
         cJSON_AddBoolToObject(arguments, "muted", muted);
  else if (ok && intent->kind == NY_AGENT_LOCAL_MUSIC_PLAY)
    ok = context->track &&
         cJSON_AddStringToObject(arguments, "name", context->track);
  else if (ok && intent->kind == NY_AGENT_LOCAL_TIMER)
    ok = cJSON_AddStringToObject(arguments, "kind", "countdown") &&
         cJSON_AddStringToObject(arguments, "label", intent->text) &&
         cJSON_AddNumberToObject(arguments, "duration_ms",
                                 (double)intent->seconds * 1000);
  else if (ok && intent->kind == NY_AGENT_LOCAL_REMEMBER)
    ok = cJSON_AddStringToObject(record, "text", intent->text) != NULL;
  else if (ok && intent->kind == NY_AGENT_LOCAL_TASK)
    ok = cJSON_AddStringToObject(record, "title", intent->text) &&
         cJSON_AddStringToObject(record, "state", "queued");
  else if (ok && intent->kind == NY_AGENT_LOCAL_ALARM)
    {
      char time[8];
      int hour;
      int minute;
      cJSON *repeat;
      ny_intent_alarm_time(intent, context->now_minutes, &hour, &minute);
      snprintf(time, sizeof(time), "%02d:%02d", hour % 24, minute % 60);
      ok = cJSON_AddStringToObject(record, "time", time) &&
           cJSON_AddStringToObject(record, "label", intent->text) &&
           cJSON_AddBoolToObject(record, "enabled", true) &&
           (repeat = cJSON_AddArrayToObject(record, "repeat")) != NULL;
      for (int day = 0; ok && day < 7; day++)
        if (intent->repeat & (1u << day))
          {
            cJSON *name = cJSON_CreateString(g_intent_days[day]);
            ok = name != NULL;
            if (ok && !cJSON_AddItemToArray(repeat, name))
              {
                cJSON_Delete(name);
                ok = false;
              }
          }
      ok = ok && cJSON_AddNumberToObject(record, "utc_offset_minutes",
                                         context->utc_offset_minutes);
    }
  if (ok)
    encoded = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return encoded;
}

/****************************************************************************
 * Name: ny_agent_local_intent_reply
 ****************************************************************************/

char *ny_agent_local_intent_reply(const char *tool, const char *arguments,
                                  const char *text, uint64_t unique)
{
  char id[48];
  cJSON *root = cJSON_CreateObject();
  cJSON *choices = root ? cJSON_AddArrayToObject(root, "choices") : NULL;
  cJSON *choice = cJSON_CreateObject();
  char *encoded = NULL;
  if (!choices || !choice || !cJSON_AddItemToArray(choices, choice))
    {
      cJSON_Delete(choice);
      cJSON_Delete(root);
      return NULL;
    }
  cJSON *message = cJSON_AddObjectToObject(choice, "message");
  cJSON *usage = cJSON_AddObjectToObject(root, "usage");
  bool ok = message && usage;
  snprintf(id, sizeof(id), "chatcmpl-rule-%" PRIx64, unique);
  ok = ok && cJSON_AddStringToObject(root, "id", id) &&
       cJSON_AddStringToObject(root, "object", "chat.completion") &&
       cJSON_AddStringToObject(root, "model", "on-device-rules") &&
       cJSON_AddNumberToObject(choice, "index", 0) &&
       cJSON_AddStringToObject(message, "role", "assistant") &&
       cJSON_AddNumberToObject(usage, "prompt_tokens", 0) &&
       cJSON_AddNumberToObject(usage, "completion_tokens", 0) &&
       cJSON_AddNumberToObject(usage, "total_tokens", 0);
  if (ok && tool)
    {
      cJSON *content = cJSON_AddNullToObject(message, "content");
      cJSON *calls = cJSON_AddArrayToObject(message, "tool_calls");
      cJSON *call = cJSON_CreateObject();
      if (!calls || !call || !cJSON_AddItemToArray(calls, call))
        {
          cJSON_Delete(call);
          call = NULL;
        }
      cJSON *function =
          call ? cJSON_AddObjectToObject(call, "function") : NULL;
      snprintf(id, sizeof(id), "call_%" PRIx64, unique);
      ok = content && function && cJSON_AddStringToObject(call, "id", id) &&
           cJSON_AddStringToObject(call, "type", "function") &&
           cJSON_AddStringToObject(function, "name", tool) &&
           cJSON_AddStringToObject(function, "arguments",
                                   arguments ? arguments : "{}") &&
           cJSON_AddStringToObject(choice, "finish_reason", "tool_calls");
    }
  else if (ok)
    ok = cJSON_AddStringToObject(message, "content", text ? text : "") &&
         cJSON_AddStringToObject(choice, "finish_reason", "stop");
  if (ok)
    encoded = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return encoded;
}

/****************************************************************************
 * Name: ny_intent_civil
 * Description: Days since 1970 to a calendar date, so the fact line does not
 *   depend on the C library's time zone state.
 ****************************************************************************/

static void ny_intent_civil(int64_t seconds, int *year, int *month, int *day,
                            int *weekday, int *hour, int *minute)
{
  int64_t days = seconds / 86400;
  int64_t rest = seconds % 86400;
  if (rest < 0)
    {
      rest += 86400;
      days--;
    }
  *hour = (int)(rest / 3600);
  *minute = (int)(rest % 3600 / 60);
  *weekday = (int)(((days % 7) + 11) % 7);
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  int64_t doe = days - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  *day = (int)(doy - (153 * mp + 2) / 5 + 1);
  *month = (int)(mp < 10 ? mp + 3 : mp - 9);
  *year = (int)(yoe + era * 400 + (*month <= 2 ? 1 : 0));
}

/****************************************************************************
 * Name: ny_intent_span
 ****************************************************************************/

static void ny_intent_span(double seconds, char *out, size_t size)
{
  unsigned long total = seconds < 0 ? 0 : (unsigned long)(seconds + 0.5);
  unsigned long hours = total / 3600;
  unsigned long minutes = total % 3600 / 60;
  unsigned long rest = total % 60;
  int used = 0;
  out[0] = 0;
  if (hours)
    used += snprintf(out + used, size - used, "%lu 小时", hours);
  if (minutes && (size_t)used < size)
    used += snprintf(out + used, size - used, "%s%lu 分钟", used ? " " : "",
                     minutes);
  if ((rest || !used) && (size_t)used < size)
    snprintf(out + used, size - used, "%s%lu 秒", used ? " " : "", rest);
}

/****************************************************************************
 * Name: ny_intent_text
 ****************************************************************************/

static const char *ny_intent_text(const cJSON *object, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

/****************************************************************************
 * Name: ny_intent_value
 ****************************************************************************/

static double ny_intent_value(const cJSON *object, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

/****************************************************************************
 * Name: ny_intent_attempt
 * Description: What the call set out to do, read back from its own
 *   arguments: "创建 5 分钟计时器".  The result of a create is the whole
 *   list, which says less about this request than the request does.
 ****************************************************************************/

static void ny_intent_attempt(const char *tool, const cJSON *call, char *out,
                              size_t size)
{
  const char *topic = ny_intent_text(call, "topic");
  const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(call, "arguments");
  const cJSON *record = cJSON_GetObjectItemCaseSensitive(arguments, "record");
  char piece[NY_AGENT_LOCAL_RAW_MAX + 1];
  char span[64];
  if (!strcmp(tool, "nyabula_expression"))
    {
      const char *name = ny_intent_text(call, "expression");
      const char *label = name;
      for (size_t i = 0; i < NY_INTENT_COUNT(g_intent_expressions); i++)
        if (!strcmp(name, g_intent_expressions[i].word))
          label = g_intent_expressions[i].expression;
      snprintf(out, size, "把表情换成“%s”", label);
    }
  else if (!strcmp(topic, "timer.create"))
    {
      const char *label = ny_intent_text(arguments, "label");
      ny_intent_span(ny_intent_value(arguments, "duration_ms") / 1000, span,
                     sizeof(span));
      ny_agent_local_intent_copy(piece, sizeof(piece), label, strlen(label));
      snprintf(out, size, "创建 %s的计时器%s%s%s", span, piece[0] ? "（" : "",
               piece, piece[0] ? "）" : "");
    }
  else if (!strcmp(topic, "alarm.create"))
    {
      const char *label = ny_intent_text(record, "label");
      int days = cJSON_GetArraySize(
          cJSON_GetObjectItemCaseSensitive(record, "repeat"));
      ny_agent_local_intent_copy(piece, sizeof(piece), label, strlen(label));
      snprintf(out, size, "创建 %s 的%s闹钟%s%s%s",
               ny_intent_text(record, "time"),
               days == 7  ? "每天重复"
               : days > 0 ? "按周重复"
                          : "",
               piece[0] ? "（" : "", piece, piece[0] ? "）" : "");
    }
  else if (!strcmp(topic, "memory.create") || !strcmp(topic, "task.create"))
    {
      const char *text =
          ny_intent_text(record, topic[0] == 'm' ? "text" : "title");
      ny_agent_local_intent_copy(piece, sizeof(piece), text, strlen(text));
      snprintf(out, size, "%s“%s”", topic[0] == 'm' ? "记住" : "添加待办",
               piece);
    }
  else if (!strcmp(topic, "music.play"))
    {
      const char *name = ny_intent_text(arguments, "name");
      ny_agent_local_intent_copy(piece, sizeof(piece), name, strlen(name));
      snprintf(out, size, "播放“%s”", piece);
    }
  else if (!strcmp(topic, "music.volume"))
    {
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(arguments, "muted")))
        snprintf(out, size, "静音");
      else
        snprintf(out, size, "把音量调到 %.0f%%",
                 ny_intent_value(arguments, "volume"));
    }
  else if (!strcmp(topic, "music.pause"))
    snprintf(out, size, "暂停播放");
  else if (!strcmp(topic, "music.resume"))
    snprintf(out, size, "继续播放");
  else if (!strcmp(topic, "music.stop"))
    snprintf(out, size, "停止播放");
  else if (!strcmp(tool, "nyabula_read"))
    snprintf(out, size, "读取 %.48s", topic);
  else
    snprintf(out, size, "执行 %.48s %.48s", tool, topic);
}

/****************************************************************************
 * Name: ny_intent_reason
 ****************************************************************************/

static const char *ny_intent_reason(int error, char *buffer, size_t size)
{
  switch (-error)
    {
      case EACCES:
        return "主人没有批准，所以没有执行";
      case ETIMEDOUT:
        return "等主人批准等到超时了，没有执行";
      case ECANCELED:
        return "这次操作被取消了，没有执行";
      case ESTALE:
        return "数据刚刚在别处被改过，这次没有写入，需要再说一次";
      case EBUSY:
        return "设备正忙，没有执行";
      case ENOSPC:
        return "数量已经到上限，没有创建";
      case ENODATA:
      case ENOENT:
        return "没有找到需要的数据";
      case ENOSYS:
      case ENOTSUP:
        return "这台设备没有启用这个功能";
      default:
        snprintf(buffer, size, "设备返回了错误码 %d", error);
        return buffer;
    }
}

/****************************************************************************
 * Name: ny_intent_fact_read
 ****************************************************************************/

static void ny_intent_fact_read(const char *topic, const cJSON *result,
                                int utc_offset_minutes, char *fact,
                                size_t size)
{
  static const char *const weekdays[] = { "周日", "周一", "周二", "周三",
                                          "周四", "周五", "周六" };
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(result, "items");
  const cJSON *row;
  char piece[NY_INTENT_LABEL_MAX + 4];
  char list[NY_AGENT_LOCAL_FACT_MAX];
  int used = 0;
  int shown = 0;
  list[0] = 0;
  if (!strcmp(topic, "system.time.get"))
    {
      int year;
      int month;
      int day;
      int weekday;
      int hour;
      int minute;
      double unix_ms = ny_intent_value(result, "unix_ms");
      if (!cJSON_IsTrue(
              cJSON_GetObjectItemCaseSensitive(result, "clock_valid")) ||
          !isfinite(unix_ms))
        {
          snprintf(fact, size, "设备的时钟还没有校准，现在不知道准确时间");
          return;
        }
      ny_intent_civil((int64_t)(unix_ms / 1000) +
                          (int64_t)utc_offset_minutes * 60,
                      &year, &month, &day, &weekday, &hour, &minute);
      snprintf(fact, size, "现在是 %04d-%02d-%02d %02d:%02d（%s）", year,
               month, day, hour, minute, weekdays[weekday]);
    }
  else if (!strcmp(topic, "weather.get"))
    {
      double value = ny_intent_value(
          cJSON_GetObjectItemCaseSensitive(result, "temperature"), "value");
      if (!isfinite(value))
        {
          snprintf(fact, size,
                   "还没有拿到天气数据（天气服务没有配置，或者没有联网）");
          return;
        }
      const char *place = ny_intent_text(
          cJSON_GetObjectItemCaseSensitive(result, "location"), "name");
      const char *sky = ny_intent_text(
          cJSON_GetObjectItemCaseSensitive(result, "condition"), "text");
      ny_agent_local_intent_copy(piece, 49, place, strlen(place));
      ny_agent_local_intent_copy(list, 49, sky, strlen(sky));
      snprintf(fact, size, "%s 现在%s，气温 %.1f°C%s", piece, list, value,
               ny_intent_value(result, "last_error") != 0
                   ? "（上次刷新失败，数据可能过时）"
                   : "");
    }
  else if (!strcmp(topic, "device.status"))
    {
      char span[64];
      const cJSON *memory = cJSON_GetObjectItemCaseSensitive(result, "memory");
      const cJSON *cpu = cJSON_GetObjectItemCaseSensitive(result, "cpu");
      const cJSON *network =
          cJSON_GetObjectItemCaseSensitive(result, "network");
      ny_intent_span(ny_intent_value(result, "uptimeMs") / 1000, span,
                     sizeof(span));
      used = snprintf(fact, size, "设备已运行 %s", span);
      if (isfinite(ny_intent_value(memory, "freeBytes")) &&
          (size_t)used < size)
        used += snprintf(fact + used, size - used, "，空闲内存 %.0f MB",
                         ny_intent_value(memory, "freeBytes") / 1048576);
      if (isfinite(ny_intent_value(cpu, "percent")) && (size_t)used < size)
        used += snprintf(fact + used, size - used, "，CPU 占用 %.0f%%",
                         ny_intent_value(cpu, "percent"));
      cJSON_ArrayForEach(
          row, cJSON_GetObjectItemCaseSensitive(network, "interfaces"))
      {
        const char *address = ny_intent_text(row, "ipv4");
        if (!address[0] || !strncmp(address, "127.", 4) ||
            (size_t)used >= size)
          continue;
        const char *ssid = ny_intent_text(row, "ssid");
        ny_agent_local_intent_copy(piece, 33, ssid, strlen(ssid));
        used += snprintf(fact + used, size - used, "，网络 %s %.20s", piece,
                         address);
        break;
      }
    }
  else if (!strcmp(topic, "timer.list") || !strcmp(topic, "alarm.list") ||
           !strcmp(topic, "task.list"))
    {
      bool timers = !strcmp(topic, "timer.list");
      bool tasks = !strcmp(topic, "task.list");
      const char *noun = timers ? "计时器" : tasks ? "待办" : "闹钟";
      int count = 0;
      cJSON_ArrayForEach(row, items)
      {
        /* A finished to-do is history, not something still to do. */

        const char *state = ny_intent_text(row, "state");
        bool open =
            !tasks || (strcmp(state, "done") && strcmp(state, "cancelled"));
        if (!open)
          continue;
        count++;
        if (shown >= NY_INTENT_LIST_SHOWN)
          continue;
        const char *label = ny_intent_text(row, tasks ? "title" : "label");

        /* Four rows of at most 60 bytes of label fit the line whole, so no
         * snprintf below ever cuts a character in half.
         */

        ny_agent_local_intent_copy(piece, 61, label, strlen(label));
        if (timers)
          {
            char span[64];
            const char *status = ny_intent_text(row, "status");
            bool finished = !strcmp(status, "finished");
            ny_intent_span(ny_intent_value(row, "remaining_ms") / 1000, span,
                           sizeof(span));
            used += snprintf(list + used, sizeof(list) - used, "%s%s%s%s",
                             shown ? "；" : "", piece[0] ? piece : "计时器",
                             finished                    ? " 已经结束"
                             : !strcmp(status, "paused") ? " 已暂停，剩余 "
                                                         : " 剩余 ",
                             finished ? "" : span);
          }
        else if (tasks)
          used += snprintf(list + used, sizeof(list) - used, "%s%s",
                           shown ? "；" : "", piece);
        else
          used += snprintf(
              list + used, sizeof(list) - used, "%s%.5s%s%s%s",
              shown ? "；" : "", ny_intent_text(row, "time"),
              piece[0] ? " " : "", piece,
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "enabled"))
                  ? ""
                  : "（已关闭）");
        shown++;
      }
      if (count == 0)
        snprintf(fact, size, "现在没有%s", noun);
      else if (count > shown)
        snprintf(fact, size, "现在有 %d 个%s：%s；还有 %d 个没有列出", count,
                 noun, list, count - shown);
      else
        snprintf(fact, size, "现在有 %d 个%s：%s", count, noun, list);
    }
  else
    fact[0] = 0;
}

/****************************************************************************
 * Name: ny_agent_local_intent_fact
 * Description: A tool result as one line a 1B model can repeat without
 *   misreading it.  JSON goes in, a sentence comes out; what no rule
 *   understands is passed on as its first bytes rather than guessed at.
 ****************************************************************************/

bool ny_agent_local_intent_fact(const char *tool, const char *arguments,
                                const char *result, int utc_offset_minutes,
                                char *fact, size_t size)
{
  cJSON *call = arguments ? cJSON_Parse(arguments) : NULL;
  cJSON *outcome = result ? cJSON_Parse(result) : NULL;
  char attempt[NY_AGENT_LOCAL_RAW_MAX + 96];
  char reason[64];
  bool failed = false;
  bool read = tool && !strcmp(tool, "nyabula_read");
  fact[0] = 0;
  if (tool == NULL)
    tool = "";
  ny_intent_attempt(tool, call, attempt, sizeof(attempt));
  if (cJSON_IsObject(outcome) &&
      cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(outcome, "ok")) &&
      !cJSON_IsTrue(
          cJSON_GetObjectItemCaseSensitive(outcome, "sideEffectApplied")))
    {
      /* The executor's own failure envelope.  An applied side effect with a
       * failed read-back is still something that happened.
       */

      bool uncertain = cJSON_IsTrue(
          cJSON_GetObjectItemCaseSensitive(outcome, "sideEffectUncertain"));
      double error = ny_intent_value(outcome, "error");
      failed = true;
      if (uncertain)
        snprintf(fact, size, "%s：结果不确定，需要主人自己确认一下", attempt);
      else
        snprintf(fact, size, "%s没有成功：%s", attempt,
                 ny_intent_reason(isfinite(error) ? (int)error : -EIO, reason,
                                  sizeof(reason)));
    }
  else if (read && cJSON_IsObject(outcome))
    ny_intent_fact_read(ny_intent_text(call, "topic"), outcome,
                        utc_offset_minutes, fact, size);
  else if (cJSON_IsObject(call) && cJSON_IsObject(outcome) &&
           (!strcmp(tool, "nyabula_action") ||
            !strcmp(tool, "nyabula_music") ||
            !strcmp(tool, "nyabula_expression")))
    {
      const char *topic = ny_intent_text(call, "topic");
      if (!strcmp(topic, "agent.mcp.out.call") ||
          !strcmp(topic, "agent.tools.call") ||
          !strcmp(topic, "calendar.create") || !strcmp(topic, "music.output"))
        fact[0] = 0; /* Not ours to paraphrase */
      else if (!strcmp(topic, "music.play"))
        {
          /* The player reports what it actually opened. */

          const char *track = ny_intent_text(outcome, "track");
          if (!track[0])
            track = ny_intent_text(
                cJSON_GetObjectItemCaseSensitive(call, "arguments"), "name");
          ny_agent_local_intent_copy(attempt, 257, track, strlen(track));
          snprintf(fact, size, "正在播放“%s”", attempt);
        }
      else
        snprintf(fact, size, "已经%s", attempt);
    }
  if (fact[0] == 0)
    {
      size_t length = result ? strlen(result) : 0;
      int used = snprintf(fact, size, "工具 %.32s 返回：", tool);
      if ((size_t)used < size)
        ny_agent_local_intent_copy(fact + used,
                                   size - used > NY_AGENT_LOCAL_RAW_MAX + 1
                                       ? NY_AGENT_LOCAL_RAW_MAX + 1
                                       : size - used,
                                   result ? result : "", length);
    }
  cJSON_Delete(call);
  cJSON_Delete(outcome);
  return failed;
}
