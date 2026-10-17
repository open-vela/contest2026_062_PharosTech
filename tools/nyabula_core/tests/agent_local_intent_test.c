/* SPDX-License-Identifier: Apache-2.0 */
/* Host tests of the on-device intent rules: the real ny_agent_local_intent.c
 * linked with cJSON and nothing else.
 *
 *   agent_local_intent_test               the tables below
 *   agent_local_intent_test fuzz N SEED   N generated inputs through
 *                                         detect + call + reply + fill + fact
 *
 * A table row states the contract as the board sees it.  Where the current
 * behaviour is a judgement call rather than a requirement the row is marked
 * DEBATABLE, so that a change of mind shows up as one edited line.
 */

#include "ny_agent_local_intent.h"
#include "ny_utf8.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(list) (sizeof(list) / sizeof((list)[0]))
#define KINDS       ((int)NY_AGENT_LOCAL_TASK + 1)

#define CHECK(condition, ...)                 \
  do                                          \
    {                                         \
      g_checks++;                             \
      if (!(condition))                       \
        {                                     \
          g_failures++;                       \
          printf("FAIL line %d: ", __LINE__); \
          printf(__VA_ARGS__);                \
          printf("\n");                       \
        }                                     \
    }                                         \
  while (0)

static unsigned long g_checks;
static unsigned long g_failures;
static unsigned long g_utterances;
static unsigned long g_seen[KINDS];

/* The context every call is rendered against unless a case says otherwise:
 * 10:00 in the morning, UTC+8, list revision 42, volume 50.
 */

static const struct ny_agent_local_context_s g_context = { 42,    480,
                                                           600,   50,
                                                           false, "song.mp3" };

/****************************************************************************
 * Helpers
 ****************************************************************************/

static bool valid(const char *text)
{
  return ny_utf8_valid((const unsigned char *)text, strlen(text));
}

static void detect(const char *utterance,
                   struct ny_agent_local_intent_s *intent)
{
  ny_agent_local_intent_detect(utterance, intent);
  g_utterances++;
  if ((int)intent->kind >= 0 && (int)intent->kind < KINDS)
    g_seen[intent->kind]++;
}

static const char *kind_name(const struct ny_agent_local_intent_s *intent)
{
  return ny_agent_local_intent_name(intent->kind);
}

static const char *text_of(const cJSON *object, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "(not a string)";
}

static double number_of(const cJSON *object, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

/* The call for an intent, parsed.  *tool is what _call() reported. */

static cJSON *call_of(const struct ny_agent_local_intent_s *intent,
                      const struct ny_agent_local_context_s *context,
                      const char **tool)
{
  char *encoded = ny_agent_local_intent_call(intent, context, tool);
  cJSON *parsed = encoded ? cJSON_Parse(encoded) : NULL;
  free(encoded);
  return parsed;
}

/****************************************************************************
 * Intent kinds without slots: reads, lists, mute, transport control, chat
 ****************************************************************************/

struct kind_case_s
{
  const char *utterance;
  enum ny_agent_local_intent_e kind;
  bool complete;
};

static const struct kind_case_s g_kind_cases[] = {
  /* Time and date */

  { "现在几点了", NY_AGENT_LOCAL_TIME, true },
  { "几点啦", NY_AGENT_LOCAL_TIME, true },
  { "现在几点钟了？", NY_AGENT_LOCAL_TIME, true },
  { "报时", NY_AGENT_LOCAL_TIME, true },
  { "what time is it", NY_AGENT_LOCAL_TIME, true },
  { "What's the time", NY_AGENT_LOCAL_TIME, true },
  { "今天几号", NY_AGENT_LOCAL_DATE, true },
  { "今天星期几呀？", NY_AGENT_LOCAL_DATE, true },
  { "今天周几", NY_AGENT_LOCAL_DATE, true },
  { "what day is it today", NY_AGENT_LOCAL_DATE, true },
  { "what's the date today", NY_AGENT_LOCAL_DATE, true },

  /* Weather.  "怎么样" is an opinion cue everywhere else; here it asks. */

  { "今天天气怎么样", NY_AGENT_LOCAL_WEATHER, true },
  { "明天会下雨吗", NY_AGENT_LOCAL_WEATHER, true },
  { "出门要带伞吗", NY_AGENT_LOCAL_WEATHER, true },
  { "外面冷不冷", NY_AGENT_LOCAL_WEATHER, true },
  { "今天气温多少度", NY_AGENT_LOCAL_WEATHER, true },
  { "how's the weather today", NY_AGENT_LOCAL_WEATHER, true },
  { "is it going to rain tomorrow", NY_AGENT_LOCAL_WEATHER, true },
  { "what's the weather like", NY_AGENT_LOCAL_WEATHER, true }, /* FIXED */
  { "我喜欢下雨天", NY_AGENT_LOCAL_CHAT, false },
  { "i like rainy weather", NY_AGENT_LOCAL_CHAT, false },

  /* Device status */

  { "设备状态", NY_AGENT_LOCAL_STATUS, true },
  { "内存还剩多少", NY_AGENT_LOCAL_STATUS, true },
  { "你的ip地址是多少", NY_AGENT_LOCAL_STATUS, true },
  { "wifi连上了吗", NY_AGENT_LOCAL_STATUS, true },
  { "运行了多久了", NY_AGENT_LOCAL_STATUS, true },

  /* Lists */

  { "有哪些计时器", NY_AGENT_LOCAL_TIMER_LIST, true },
  { "计时器还剩多久", NY_AGENT_LOCAL_TIMER_LIST, true },
  { "看看倒计时", NY_AGENT_LOCAL_TIMER_LIST, true },
  { "list timers", NY_AGENT_LOCAL_TIMER_LIST, true },
  { "有几个闹钟", NY_AGENT_LOCAL_ALARM_LIST, true },
  { "看看我的闹钟", NY_AGENT_LOCAL_ALARM_LIST, true },
  { "show my alarms", NY_AGENT_LOCAL_ALARM_LIST, true },
  { "what alarms do i have", NY_AGENT_LOCAL_ALARM_LIST, true },
  { "我有哪些待办", NY_AGENT_LOCAL_TASK_LIST, true },
  { "看看待办", NY_AGENT_LOCAL_TASK_LIST, true },
  { "待办列表", NY_AGENT_LOCAL_TASK_LIST, true },
  { "还有什么待办", NY_AGENT_LOCAL_TASK_LIST, true },
  { "有没有待办", NY_AGENT_LOCAL_TASK_LIST, true },
  { "what's on my todo list", NY_AGENT_LOCAL_TASK_LIST, true },
  { "what did i add to my todo list", NY_AGENT_LOCAL_TASK_LIST, true },

  /* Mute and unmute */

  { "静音", NY_AGENT_LOCAL_MUTE, true },
  { "别出声", NY_AGENT_LOCAL_MUTE, true },
  { "mute", NY_AGENT_LOCAL_MUTE, true },
  { "取消静音", NY_AGENT_LOCAL_UNMUTE, true },
  { "不要静音", NY_AGENT_LOCAL_UNMUTE, true },
  { "别静音了", NY_AGENT_LOCAL_UNMUTE, true },
  { "恢复声音", NY_AGENT_LOCAL_UNMUTE, true },
  { "unmute", NY_AGENT_LOCAL_UNMUTE, true },

  /* Transport control */

  { "暂停", NY_AGENT_LOCAL_MUSIC_PAUSE, true },
  { "暂停音乐", NY_AGENT_LOCAL_MUSIC_PAUSE, true },
  { "先暂停一下", NY_AGENT_LOCAL_MUSIC_PAUSE, true },
  { "pause the music", NY_AGENT_LOCAL_MUSIC_PAUSE, true },
  { "暂停计时器", NY_AGENT_LOCAL_CHAT, false }, /* Not the player's */
  { "继续播放", NY_AGENT_LOCAL_MUSIC_RESUME, true },
  { "接着放", NY_AGENT_LOCAL_MUSIC_RESUME, true },
  { "继续放歌", NY_AGENT_LOCAL_MUSIC_RESUME, true },
  { "resume", NY_AGENT_LOCAL_MUSIC_RESUME, true },
  { "停止播放", NY_AGENT_LOCAL_MUSIC_STOP, true },
  { "别放了", NY_AGENT_LOCAL_MUSIC_STOP, true },
  { "关掉音乐", NY_AGENT_LOCAL_MUSIC_STOP, true },
  { "不听了", NY_AGENT_LOCAL_MUSIC_STOP, true },
  { "stop the music", NY_AGENT_LOCAL_MUSIC_STOP, true },

  /* Must stay chat: opinions and knowledge */

  { "你觉得计时器这个发明怎么样", NY_AGENT_LOCAL_CHAT, false },
  { "闹钟是谁发明的", NY_AGENT_LOCAL_CHAT, false },
  { "待办是什么意思", NY_AGENT_LOCAL_CHAT, false },
  { "你觉得待办多吗", NY_AGENT_LOCAL_CHAT, false },
  { "讲讲音乐的历史", NY_AGENT_LOCAL_CHAT, false },
  { "音乐是什么", NY_AGENT_LOCAL_CHAT, false },
  { "你为什么生气", NY_AGENT_LOCAL_CHAT, false },
  { "why is the alarm so loud", NY_AGENT_LOCAL_CHAT, false },
  { "tell me about timers", NY_AGENT_LOCAL_CHAT, false },
  { "what is a timer", NY_AGENT_LOCAL_CHAT, false },  /* FIXED */
  { "what is an alarm", NY_AGENT_LOCAL_CHAT, false }, /* FIXED */
  { "What's a todo", NY_AGENT_LOCAL_CHAT, false },    /* FIXED */

  /* Must stay chat: questions about features */

  { "你有什么功能", NY_AGENT_LOCAL_CHAT, false },
  { "你都会些什么", NY_AGENT_LOCAL_CHAT, false },
  { "计时器怎么用", NY_AGENT_LOCAL_CHAT, false },
  { "你支持计时器吗", NY_AGENT_LOCAL_CHAT, false },
  { "音量现在是多少", NY_AGENT_LOCAL_CHAT, false },
  { "音量最大是多少", NY_AGENT_LOCAL_CHAT, false }, /* FIXED: set 100 */
  { "what can you do", NY_AGENT_LOCAL_CHAT, false },
  { "do you have a timer", NY_AGENT_LOCAL_CHAT, false },

  /* Must stay chat: narration and reported speech (all FIXED) */

  { "我昨天定了个闹钟没响", NY_AGENT_LOCAL_CHAT, false },
  { "我昨天定了五分钟的计时器忘了关", NY_AGENT_LOCAL_CHAT, false },
  { "小明说他定了三个闹钟", NY_AGENT_LOCAL_CHAT, false },
  { "我上次定的计时器呢", NY_AGENT_LOCAL_CHAT, false },
  { "昨晚我把音量调到最大了", NY_AGENT_LOCAL_CHAT, false },
  { "他说“定个五分钟的计时器”", NY_AGENT_LOCAL_CHAT, false },
  { "书上说“计时五分钟”就够了", NY_AGENT_LOCAL_CHAT, false },
  { "她说：“把音量调到最大”", NY_AGENT_LOCAL_CHAT, false },
  { "妈妈说把音量调到最大才听得清", NY_AGENT_LOCAL_CHAT, false },
  { "昨天你没提醒我吃药", NY_AGENT_LOCAL_CHAT, false },
  { "I set a timer yesterday and forgot about it", NY_AGENT_LOCAL_CHAT,
    false },
  { "she said set an alarm for 7 am", NY_AGENT_LOCAL_CHAT, false },

  /* Must stay chat: a quantity or a mood with no order next to it */

  { "我跑了五分钟", NY_AGENT_LOCAL_CHAT, false },
  { "煮蛋要十分钟", NY_AGENT_LOCAL_CHAT, false },
  { "这首歌有三分钟", NY_AGENT_LOCAL_CHAT, false },
  { "我七点起床", NY_AGENT_LOCAL_CHAT, false },
  { "七点", NY_AGENT_LOCAL_CHAT, false },
  { "闹钟响了", NY_AGENT_LOCAL_CHAT, false },
  { "计时器响了", NY_AGENT_LOCAL_CHAT, false },
  { "计时器", NY_AGENT_LOCAL_CHAT, false },
  { "我今天很生气", NY_AGENT_LOCAL_CHAT, false },
  { "我很开心", NY_AGENT_LOCAL_CHAT, false },
  { "你开心吗", NY_AGENT_LOCAL_CHAT, false },
  { "我要去睡觉了", NY_AGENT_LOCAL_CHAT, false },
  { "你的声音真好听，我今年30岁", NY_AGENT_LOCAL_CHAT, false },
  { "今天地铁上太吵了，我都没法看书", NY_AGENT_LOCAL_CHAT, false },
  { "我觉得闹钟太吵了", NY_AGENT_LOCAL_CHAT, false },
  { "我想听故事", NY_AGENT_LOCAL_CHAT, false },
  { "给我讲个笑话", NY_AGENT_LOCAL_CHAT, false },
  { "播放新闻", NY_AGENT_LOCAL_CHAT, false },
  { "记者来了", NY_AGENT_LOCAL_CHAT, false },
  { "你记住了吗", NY_AGENT_LOCAL_CHAT, false },
  { "我记得你", NY_AGENT_LOCAL_CHAT, false },
  { "记得关灯", NY_AGENT_LOCAL_CHAT, false },
  { "do you remember me", NY_AGENT_LOCAL_CHAT, false },
  { "你会提醒我吗", NY_AGENT_LOCAL_CHAT, false },
  { "提醒我", NY_AGENT_LOCAL_CHAT, false },
  { "start", NY_AGENT_LOCAL_CHAT, false }, /* Not the expression "star" */

  /* Must stay chat: small talk and noise */

  { "hello", NY_AGENT_LOCAL_CHAT, false },
  { "how are you", NY_AGENT_LOCAL_CHAT, false },
  { "good morning", NY_AGENT_LOCAL_CHAT, false },
  { "thanks", NY_AGENT_LOCAL_CHAT, false },
  { "i love you", NY_AGENT_LOCAL_CHAT, false },
  { "tell me a joke", NY_AGENT_LOCAL_CHAT, false },
  { "你好呀", NY_AGENT_LOCAL_CHAT, false },
  { "今天好累啊", NY_AGENT_LOCAL_CHAT, false },
  { "", NY_AGENT_LOCAL_CHAT, false },
  { "   ", NY_AGENT_LOCAL_CHAT, false },
  { "\t\n", NY_AGENT_LOCAL_CHAT, false },
  { "😀😀😀", NY_AGENT_LOCAL_CHAT, false },
  { "。。。", NY_AGENT_LOCAL_CHAT, false },
  { "计时五分钟\xff", NY_AGENT_LOCAL_CHAT, false },       /* Invalid UTF-8 */
  { "\xe8\xae\xa1\xe6\x97", NY_AGENT_LOCAL_CHAT, false }, /* Cut sequence */

  /* DEBATABLE: a question about a feature that names the verb is answered
   * with the slot question ("闹钟要定在几点呢？") rather than by the model.
   * A guard would also catch the polite order "你能帮我定个闹钟吗".
   */

  { "你能定闹钟吗", NY_AGENT_LOCAL_ALARM, false },
  { "你会定计时器吗", NY_AGENT_LOCAL_TIMER, false },
  { "can you set alarms", NY_AGENT_LOCAL_ALARM, false },
  { "how do i set an alarm", NY_AGENT_LOCAL_ALARM, false },
  { "他让我定个闹钟", NY_AGENT_LOCAL_ALARM, false },

  /* DEBATABLE: clearing or completing to-dos is not supported; both get
   * the "要添加什么待办呢？" question.
   */

  { "清空待办", NY_AGENT_LOCAL_TASK, false },
  { "完成待办", NY_AGENT_LOCAL_TASK, false },

  /* DEBATABLE: an English bare hour is not a time without am/pm/o'clock. */

  { "wake me up at 6", NY_AGENT_LOCAL_CHAT, false },

  /* DEBATABLE: a complaint about the alarm's loudness turns it down. */

  { "闹钟太吵了", NY_AGENT_LOCAL_VOLUME_DOWN, true },

  /* DEBATABLE: a remark that names the weather reads it (harmless). */

  { "the weather is nice", NY_AGENT_LOCAL_WEATHER, true }
};

/****************************************************************************
 * Timers: numerals and durations
 ****************************************************************************/

struct timer_case_s
{
  const char *utterance;
  uint32_t seconds;
  bool complete;
  const char *label;
};

static const struct timer_case_s g_timer_cases[] = {
  { "定个五分钟的计时器", 300, true, "" },
  { "计时十五分钟", 900, true, "" },
  { "倒计时二十分钟", 1200, true, "" },
  { "定时两个小时", 7200, true, "" },
  { "计时一个半小时", 5400, true, "" },
  { "计时1个半小时", 5400, true, "" },
  { "计时一小时半", 5400, true, "" },
  { "计时半小时", 1800, true, "" },
  { "计时半个小时", 1800, true, "" },
  { "计时三分半", 210, true, "" },
  { "计时三分半钟", 210, true, "" },
  { "计时90秒", 90, true, "" },
  { "计时1.5小时", 5400, true, "" },
  { "计时2.5分钟", 150, true, "" },
  { "定个一小时二十分钟的计时器", 4800, true, "" },
  { "计时1小时30分", 5400, true, "" },
  { "计时1小时二十分钟", 4800, true, "" }, /* Mixed numerals */
  { "计时两小时30分钟", 9000, true, "" },  /* Mixed numerals */
  { "计时两分三十秒", 150, true, "" },
  { "计时两小时零五分", 7500, true, "" },
  { "计时一百二十分钟", 7200, true, "" },
  { "计时一刻钟", 900, true, "" },
  { "计时三刻钟", 2700, true, "" },
  { "计时二十五秒钟", 25, true, "" },
  { "计时24小时", 86400, true, "" },
  { "计时五分钟，谢谢", 300, true, "" },
  { "请帮我计时十分钟好吗", 600, true, "" },
  { "来个三分钟的倒计时吧", 180, true, "" },
  { "帮我倒计时 10 分钟", 600, true, "" },
  { "计时五分钟😀", 300, true, "" },
  { "set a timer for 5 minutes", 300, true, "" },
  { "Set a 10 minute timer", 600, true, "" },
  { "start a timer for 1 hour and 30 minutes", 5400, true, "" },
  { "timer 45 seconds", 45, true, "" },
  { "set a timer for 2 hrs", 7200, true, "" },
  { "计时 5 min", 300, true, "" },

  /* A countdown with a label */

  { "十分钟后提醒我关火", 600, true, "关火" },
  { "半小时后提醒我关火", 1800, true, "关火" },
  { "二十分钟后提醒我去接孩子", 1200, true, "接孩子" },
  { "一个小时后提醒我，记得喝水", 3600, true, "喝水" },
  { "十分钟以后叫我", 600, true, "" },
  { "五分钟后叫我", 300, true, "" },
  { "3分钟后提醒我", 180, true, "" },
  { "30秒后告诉我", 30, true, "" },
  { "过十分钟提醒我收衣服", 600, true, "收衣服" }, /* FIXED: was a to-do */
  { "十分钟后提醒我交昨天的作业", 600, true, "交昨天的作业" },
  { "remind me in 20 minutes to call mom", 1200, true, "call mom" },
  { "in 10 minutes remind me to stretch", 600, true, "stretch" },

  /* DEBATABLE: with the duration last the label is lost. */

  { "remind me to stretch in 10 minutes", 600, true, "" },

  /* No usable duration: the owner or the model is asked. */

  { "帮我定个计时器", 0, false, "" },
  { "帮我开个计时器", 0, false, "" },
  { "开始计时", 0, false, "" },
  { "计时", 0, false, "" },
  { "定个计时器二十", 0, false, "" },
  { "计时零分钟", 0, false, "" },
  { "计时0秒", 0, false, "" },
  { "计时三天", 0, false, "" },
  { "set a timer for an hour", 0, false, "" },

  /* Read, and refused as too long for a timer */

  { "计时25小时", 90000, false, "" }
};

/****************************************************************************
 * Alarms: clock times
 ****************************************************************************/

struct alarm_case_s
{
  const char *utterance;
  bool complete;
  int hour;
  int minute;
  bool ambiguous;
  unsigned int repeat;
  bool far_date;
  const char *label;
};

static const struct alarm_case_s g_alarm_cases[] = {
  /* No part of the day: the hour is kept as said and flagged ambiguous;
   * _call() settles it against the clock (see g_resolve_cases).
   */

  { "定个七点半的闹钟", true, 7, 30, true, 0, false, "" },
  { "定个七点的闹钟", true, 7, 0, true, 0, false, "" },
  { "闹钟七点", true, 7, 0, true, 0, false, "" },
  { "十点一刻叫我", true, 10, 15, true, 0, false, "" },
  { "定个三点三刻的闹钟", true, 3, 45, true, 0, false, "" },
  { "定个八点整的闹钟", true, 8, 0, true, 0, false, "" },
  { "定个八点钟的闹钟", true, 8, 0, true, 0, false, "" },
  { "定个三点十分的闹钟", true, 3, 10, true, 0, false, "" },
  { "定个10点05的闹钟", true, 10, 5, true, 0, false, "" },
  { "定个十点零五分的闹钟", true, 10, 5, true, 0, false, "" },
  { "定个十二点的闹钟", true, 12, 0, true, 0, false, "" },
  { "设一个 6:30 的闹钟", true, 6, 30, true, 0, false, "" },
  { "定个7：30的闹钟", true, 7, 30, true, 0, false, "" },
  { "定个12:00的闹钟", true, 12, 0, true, 0, false, "" },
  { "set an alarm for 7 o'clock", true, 7, 0, true, 0, false, "" },
  { "你能帮我定个七点的闹钟吗", true, 7, 0, true, 0, false, "" },
  { "八点之前叫我", true, 8, 0, true, 0, false, "" },

  /* 24-hour values are what they say. */

  { "定个19:45的闹钟", true, 19, 45, false, 0, false, "" },
  { "定个23:59的闹钟", true, 23, 59, false, 0, false, "" },
  { "定个0:30的闹钟", true, 0, 30, false, 0, false, "" },
  { "定个零点的闹钟", true, 0, 0, false, 0, false, "" },
  { "定个24点的闹钟", true, 0, 0, false, 0, false, "" },

  /* A part of the day */

  { "定个下午三点的闹钟", true, 15, 0, false, 0, false, "" },
  { "下午三点十分叫我", true, 15, 10, false, 0, false, "" },
  { "明天下午3点半提醒我开会", true, 15, 30, false, 0, false, "开会" },
  { "下午三点四十五分提醒我开会", true, 15, 45, false, 0, false, "开会" },
  { "晚上十点提醒我睡觉", true, 22, 0, false, 0, false, "睡觉" },
  { "闹钟定在晚上9点", true, 21, 0, false, 0, false, "" },
  { "今晚十一点提醒我关灯", true, 23, 0, false, 0, false, "关灯" },
  { "晚上十二点的闹钟", true, 0, 0, false, 0, false, "" },
  { "晚上一点的闹钟", true, 1, 0, false, 0, false, "" },
  { "半夜十二点的闹钟", true, 0, 0, false, 0, false, "" },
  { "深夜十一点的闹钟", true, 23, 0, false, 0, false, "" },
  { "中午十二点的闹钟", true, 12, 0, false, 0, false, "" },
  { "中午一点叫我", true, 13, 0, false, 0, false, "" },
  { "定个凌晨十二点的闹钟", true, 0, 0, false, 0, false, "" },
  { "凌晨三点叫醒我", true, 3, 0, false, 0, false, "" },
  { "明早七点半叫我起床", true, 7, 30, false, 0, false, "起床" },
  { "明天早上8点叫我", true, 8, 0, false, 0, false, "" },
  { "帮我定一个明天早上六点五十的闹钟", true, 6, 50, false, 0, false, "" },
  { "午夜的闹钟", true, 0, 0, false, 0, false, "" },
  { "set an alarm for 7 pm", true, 19, 0, false, 0, false, "" },
  { "set an alarm for 7:30 am", true, 7, 30, false, 0, false, "" },
  { "set an alarm for 8 p.m.", true, 20, 0, false, 0, false, "" },
  { "set an alarm for 12 am", true, 0, 0, false, 0, false, "" },
  { "set an alarm for 12 pm", true, 12, 0, false, 0, false, "" },
  { "set an alarm for noon", true, 12, 0, false, 0, false, "" },
  { "wake me up at 6 am", true, 6, 0, false, 0, false, "" },

  /* What the alarm is for settles the half of the day. */

  { "七点叫我起床", true, 7, 0, false, 0, false, "起床" },
  { "六点提醒我吃晚饭", true, 18, 0, false, 0, false, "吃晚饭" },
  { "十点提醒我睡觉", true, 22, 0, false, 0, false, "睡觉" },

  /* Repeats */

  { "每天早上七点叫我", true, 7, 0, false, 0x7f, false, "" },
  { "每天晚上十点半提醒我吃药", true, 22, 30, false, 0x7f, false, "吃药" },
  { "天天早上六点叫醒我", true, 6, 0, false, 0x7f, false, "" },
  { "工作日早上八点的闹钟", true, 8, 0, false, 0x3e, false, "" },
  { "周末上午十点的闹钟", true, 10, 0, false, 0x41, false, "" },
  { "every day at 7 am wake me up", true, 7, 0, false, 0x7f, false, "" },
  { "alarm at 6:15 am on weekdays", true, 6, 15, false, 0x3e, false, "" },

  /* A day the alarm record cannot hold: read, flagged, not created. */

  { "下周一早上八点叫我", false, 8, 0, false, 0, true, "" },
  { "后天七点叫我", false, 7, 0, true, 0, true, "" },
  { "三月五号早上八点叫我", false, 8, 0, false, 0, true, "" },
  { "set an alarm for 7 am on friday", false, 7, 0, false, 0, true, "" },

  /* FIXED: 月 and 号 in the label are not a date. */

  { "晚上十点提醒我看月亮", true, 22, 0, false, 0, false, "看月亮" },
  { "晚上八点提醒我取快递号码", true, 20, 0, false, 0, false, "取快递号码" },

  /* No usable time */

  { "帮我定个闹钟", false, -1, -1, false, 0, false, "" },
  { "定个25点的闹钟", false, -1, -1, false, 0, false, "" },
  { "定个7:65的闹钟", false, -1, -1, false, 0, false, "" }
};

/* An ambiguous hour is the next one to come round; with no clock it is
 * taken as said.
 */

struct resolve_case_s
{
  const char *utterance;
  int now_minutes;
  const char *time;
};

static const struct resolve_case_s g_resolve_cases[] = {
  { "定个七点的闹钟", 600, "19:00" },   /* 10:00 now */
  { "定个七点的闹钟", 1200, "07:00" },  /* 20:00 now */
  { "定个七点的闹钟", 360, "07:00" },   /* 06:00 now */
  { "定个七点的闹钟", 420, "19:00" },   /* 07:00 now: not "in 24 hours" */
  { "定个七点的闹钟", 1140, "07:00" },  /* 19:00 now */
  { "定个七点的闹钟", -1, "07:00" },    /* Clock not set */
  { "定个七点半的闹钟", 449, "07:30" }, /* 07:29 now */
  { "定个七点半的闹钟", 450, "19:30" },
  { "定个十二点的闹钟", 600, "12:00" },
  { "定个十二点的闹钟", 780, "00:00" }, /* 13:00 now */
  { "定个十二点半的闹钟", 0, "00:30" },
  { "下午三点叫我", 1200, "15:00" } /* Not ambiguous: unaffected */
};

/****************************************************************************
 * Volume
 ****************************************************************************/

struct volume_case_s
{
  const char *utterance;
  enum ny_agent_local_intent_e kind;
  int percent; /* Level for SET, step for UP and DOWN, -1 for the rest */
  bool complete;
  int before;
  bool muted_before;
  int after;
  bool muted_after;
};

static const struct volume_case_s g_volume_cases[] = {
  /* Absolute */

  { "音量调到30", NY_AGENT_LOCAL_VOLUME_SET, 30, true, 50, false, 30, false },
  { "音量调到 60%", NY_AGENT_LOCAL_VOLUME_SET, 60, true, 50, false, 60,
    false },
  { "音量60％", NY_AGENT_LOCAL_VOLUME_SET, 60, true, 50, false, 60, false },
  { "音量调到百分之五十", NY_AGENT_LOCAL_VOLUME_SET, 50, true, 20, false, 50,
    false },
  { "音量调到百分之百", NY_AGENT_LOCAL_VOLUME_SET, 100, true, 20, false, 100,
    false },
  { "音量调到一百", NY_AGENT_LOCAL_VOLUME_SET, 100, true, 20, false, 100,
    false },
  { "音量开到八十", NY_AGENT_LOCAL_VOLUME_SET, 80, true, 20, false, 80,
    false },
  { "声音调成二十", NY_AGENT_LOCAL_VOLUME_SET, 20, true, 50, false, 20,
    false },
  { "把声音设为45", NY_AGENT_LOCAL_VOLUME_SET, 45, true, 50, false, 45,
    false },
  { "把声音加到80", NY_AGENT_LOCAL_VOLUME_SET, 80, true, 50, false, 80,
    false },
  { "音量30", NY_AGENT_LOCAL_VOLUME_SET, 30, true, 50, false, 30, false },
  { "音量调到0", NY_AGENT_LOCAL_VOLUME_SET, 0, true, 50, false, 0, false },
  { "把音量调到最大", NY_AGENT_LOCAL_VOLUME_SET, 100, true, 50, false, 100,
    false },
  { "音量调到一半", NY_AGENT_LOCAL_VOLUME_SET, 50, true, 80, false, 50,
    false },
  { "volume 40", NY_AGENT_LOCAL_VOLUME_SET, 40, true, 50, false, 40, false },
  { "volume to 25", NY_AGENT_LOCAL_VOLUME_SET, 25, true, 50, false, 25,
    false },
  { "set volume to 70 percent", NY_AGENT_LOCAL_VOLUME_SET, 70, true, 50, false,
    70, false },
  { "Set the volume at 80", NY_AGENT_LOCAL_VOLUME_SET, 80, true, 50, false, 80,
    false },

  /* An absolute level while muted also unmutes. */

  { "音量调到30", NY_AGENT_LOCAL_VOLUME_SET, 30, true, 50, true, 30, false },

  /* "最小" is the quietest audible level, not silence. */

  { "音量调到最小", NY_AGENT_LOCAL_VOLUME_SET, 10, true, 50, false, 10,
    false },

  /* Out of range or unreadable: the level is asked for. */

  { "音量调到150", NY_AGENT_LOCAL_VOLUME_SET, 150, false, 50, false, 0,
    false },
  { "调一下音量", NY_AGENT_LOCAL_VOLUME_SET, -1, false, 50, false, 0, false },
  { "音量调到三点", NY_AGENT_LOCAL_VOLUME_SET, -1, false, 50, false, 0,
    false },

  /* Relative, default step */

  { "声音大一点", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65, false },
  { "大声点", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65, false },
  { "音量调高", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65, false },
  { "音量调高一点", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65, false },
  { "把声音调大一点", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65,
    false },
  { "声音太小了", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65, false },
  { "turn it up", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65, false },
  { "turn up the volume", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65,
    false },
  { "louder", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, false, 65, false },
  { "声音小一点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35, false },
  { "小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35, false },
  { "太吵了", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35, false },
  { "太吵了小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35,
    false },
  { "声音太大了", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35, false },
  { "声音轻一点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35, false },
  { "turn it down", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35,
    false },
  { "volume down", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, false, 35,
    false },

  /* Relative, a step that was said */

  { "音量调低20", NY_AGENT_LOCAL_VOLUME_DOWN, 20, true, 50, false, 30, false },
  { "音量降低10", NY_AGENT_LOCAL_VOLUME_DOWN, 10, true, 50, false, 40, false },
  { "音量提高二十", NY_AGENT_LOCAL_VOLUME_UP, 20, true, 50, false, 70, false },
  { "音量调大5", NY_AGENT_LOCAL_VOLUME_UP, 5, true, 50, false, 55, false },
  { "turn down the volume by 10", NY_AGENT_LOCAL_VOLUME_DOWN, 10, true, 50,
    false, 40, false },

  /* FIXED: these set the level to 10 / 20 instead of moving it. */

  { "音量增加10", NY_AGENT_LOCAL_VOLUME_UP, 10, true, 50, false, 60, false },
  { "音量加10", NY_AGENT_LOCAL_VOLUME_UP, 10, true, 50, false, 60, false },
  { "音量减少20", NY_AGENT_LOCAL_VOLUME_DOWN, 20, true, 50, false, 30, false },
  { "声音减10", NY_AGENT_LOCAL_VOLUME_DOWN, 10, true, 50, false, 40, false },

  /* Clamping.  Up stops at 100.  Down stops at 5, the quietest level that
   * is still heard, and never raises a level that is already below it.
   */

  { "大声点", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 95, false, 100, false },
  { "大声点", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 100, false, 100, false },
  { "音量提高二十", NY_AGENT_LOCAL_VOLUME_UP, 20, true, 85, false, 100,
    false },
  { "小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 20, false, 5, false },
  { "小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 10, false, 5, false },
  { "小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 5, false, 5, false },
  { "小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 3, false, 3, false },
  { "小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 0, false, 0, false },

  /* Louder unmutes; quieter leaves the mute as it was. */

  { "大声点", NY_AGENT_LOCAL_VOLUME_UP, 15, true, 50, true, 65, false },
  { "小声点", NY_AGENT_LOCAL_VOLUME_DOWN, 15, true, 50, true, 35, true },

  /* Mute keeps the level. */

  { "静音", NY_AGENT_LOCAL_MUTE, -1, true, 37, false, 37, true },
  { "mute", NY_AGENT_LOCAL_MUTE, -1, true, 37, true, 37, true },
  { "取消静音", NY_AGENT_LOCAL_UNMUTE, -1, true, 37, true, 37, false },
  { "unmute", NY_AGENT_LOCAL_UNMUTE, -1, true, 0, true, 0, false }
};

/****************************************************************************
 * Expressions
 ****************************************************************************/

struct expression_case_s
{
  const char *utterance;
  const char *expression; /* NULL: recognised, which one is asked for */
};

static const struct expression_case_s g_expression_cases[] = {
  { "表情变回正常", "idle" },
  { "表情切回默认", "idle" },
  { "换个平静的表情", "idle" },
  { "做个好奇的表情", "curious" },
  { "做个疑惑的表情", "curious" },
  { "换个开心的表情", "happy" },
  { "请把表情换成开心", "happy" },
  { "笑一个", "happy" },
  { "给我笑一个", "happy" },
  { "笑一下", "happy" },
  { "Smile!", "happy" },
  { "show me a happy face", "happy" },
  { "换个思考的表情", "processing" },
  { "make a thinking face", "processing" },
  { "眼睛变成星星眼", "star" },
  { "做个崇拜的表情", "star" },
  { "set expression to star", "star" },
  { "卖个萌", "heart" },
  { "比个心", "heart" },
  { "眼睛变成爱心", "heart" },
  { "来个犯困的表情", "sleepy" },
  { "表情换成sleepy", "sleepy" }, /* Not "sleep" */
  { "做个打瞌睡的表情", "sleepy" },
  { "表情换成睡觉", "sleep" },
  { "装睡", "sleep" },
  { "睡觉吧", "sleep" },
  { "睡吧", "sleep" },
  { "go to sleep", "sleep" },
  { "做个生气的表情", "angry" },
  { "假装很愤怒", "angry" },
  { "make an angry face", "angry" },
  { "表情变成难过", "sad" },
  { "做个哭的表情", "sad" },
  { "make a sad face", "sad" },
  { "假装很惊讶", "surprise" },
  { "做一个吃惊的表情", "surprise" },
  { "show a surprised face", "surprise" },
  { "做个晕的表情", "dizzy" },
  { "make a dizzy face", "dizzy" },
  { "做个发呆的表情", "derp" },
  { "do a derp face", "derp" },
  { "换个表情", NULL },
  { "show me an expression", NULL }
};

/* DEBATABLE misses, kept as chat on purpose by the narrow rules: there is
 * no 表情 / 眼睛 / 脸 frame, or no verb the rule knows.
 */

static const char *const g_expression_misses[] = {
  "给我一个星星眼", "来个爱心眼",   "表情恢复正常", "变个脸",
  "哭一个",         "look curious", "be happy"
};

/****************************************************************************
 * Memory and to-do text
 ****************************************************************************/

struct text_case_s
{
  const char *utterance;
  enum ny_agent_local_intent_e kind;
  bool complete;
  const char *text;
};

static const struct text_case_s g_text_cases[] = {
  /* Remember: the cue and the filler around it go, the words stay. */

  { "记住我喜欢吃草莓", NY_AGENT_LOCAL_REMEMBER, true, "我喜欢吃草莓" },
  { "记一下车停在B2", NY_AGENT_LOCAL_REMEMBER, true, "车停在B2" },
  { "帮我记一下明天要交水电费", NY_AGENT_LOCAL_REMEMBER, true,
    "明天要交水电费" },
  { "请记住，我的生日是五月三号", NY_AGENT_LOCAL_REMEMBER, true,
    "我的生日是五月三号" },
  { "帮我记住WiFi密码是12345678", NY_AGENT_LOCAL_REMEMBER, true,
    "WiFi密码是12345678" }, /* Case kept */
  { "记住，我对花生过敏。", NY_AGENT_LOCAL_REMEMBER, true, "我对花生过敏" },
  { "记住：门禁密码 2580！", NY_AGENT_LOCAL_REMEMBER, true, "门禁密码 2580" },
  { "帮我记下来我的车牌号是粤B12345", NY_AGENT_LOCAL_REMEMBER, true,
    "我的车牌号是粤B12345" },
  { "麻烦你帮我记一下，周三要开会", NY_AGENT_LOCAL_REMEMBER, true,
    "周三要开会" },
  { "嗯，记住我老婆生日是六月一日", NY_AGENT_LOCAL_REMEMBER, true,
    "我老婆生日是六月一日" },
  { "我要你记住我叫小明", NY_AGENT_LOCAL_REMEMBER, true, "我叫小明" },
  { "记一下今天天气很好", NY_AGENT_LOCAL_REMEMBER, true, "今天天气很好" },
  { "记住我昨天把钥匙放在抽屉里了", NY_AGENT_LOCAL_REMEMBER, true,
    "我昨天把钥匙放在抽屉里了" }, /* Narration is fine inside a memory */
  { "记住定个闹钟这句话", NY_AGENT_LOCAL_REMEMBER, true, "定个闹钟这句话" },
  { "remember that my keys are in the drawer", NY_AGENT_LOCAL_REMEMBER, true,
    "my keys are in the drawer" },
  { "Remember I like tea", NY_AGENT_LOCAL_REMEMBER, true, "I like tea" },
  { "Please remember that I parked on level 3", NY_AGENT_LOCAL_REMEMBER, true,
    "I parked on level 3" },
  { "记住", NY_AGENT_LOCAL_REMEMBER, false, "" },
  { "记一下", NY_AGENT_LOCAL_REMEMBER, false, "" },
  { "帮我记", NY_AGENT_LOCAL_REMEMBER, false, "" },

  /* DEBATABLE: 帮我记 + one character reads as a memory too short to keep. */

  { "帮我记账", NY_AGENT_LOCAL_REMEMBER, false, "账" },

  /* To-do, item after the noun */

  { "加个待办：周五交报告", NY_AGENT_LOCAL_TASK, true, "周五交报告" },
  { "待办加一条：周五交报告", NY_AGENT_LOCAL_TASK, true, "周五交报告" },
  { "待办加一条，周五交报告。", NY_AGENT_LOCAL_TASK, true, "周五交报告" },
  { "待办：记得买菜", NY_AGENT_LOCAL_TASK, true, "记得买菜" },
  { "待办 记得买菜", NY_AGENT_LOCAL_TASK, true, "记得买菜" },
  { "待办：加班", NY_AGENT_LOCAL_TASK, true, "加班" },
  { "待办：记账", NY_AGENT_LOCAL_TASK, true, "记账" },
  { "待办：个人总结", NY_AGENT_LOCAL_TASK, true, "个人总结" },
  { "待办事项里加一个个人所得税申报", NY_AGENT_LOCAL_TASK, true,
    "个人所得税申报" },
  { "帮我记个待办，给妈妈打电话", NY_AGENT_LOCAL_TASK, true, "给妈妈打电话" },
  { "新增待办 加班写代码", NY_AGENT_LOCAL_TASK, true, "加班写代码" },
  { "加待办 取快递", NY_AGENT_LOCAL_TASK, true, "取快递" },
  { "添加待办事项：预约体检", NY_AGENT_LOCAL_TASK, true, "预约体检" },
  { "新建一个待办，买猫粮", NY_AGENT_LOCAL_TASK, true, "买猫粮" },
  { "todo: call the dentist", NY_AGENT_LOCAL_TASK, true, "call the dentist" },
  { "add a todo: renew passport", NY_AGENT_LOCAL_TASK, true,
    "renew passport" },
  { "New todo Buy Cat Food", NY_AGENT_LOCAL_TASK, true, "Buy Cat Food" },

  /* To-do, item in front of the noun */

  { "把买牛奶加到待办里", NY_AGENT_LOCAL_TASK, true, "买牛奶" },
  { "将取快递加入待办事项", NY_AGENT_LOCAL_TASK, true, "取快递" },
  { "请把遛狗放进待办里面", NY_AGENT_LOCAL_TASK, true, "遛狗" },
  { "把买牛奶加到待办列表里", NY_AGENT_LOCAL_TASK, true,
    "买牛奶" }, /* FIXED: read the list instead */
  { "add buy milk to my todo list", NY_AGENT_LOCAL_TASK, true,
    "buy milk" }, /* FIXED: read the list instead */
  { "add call mom to my to-do list", NY_AGENT_LOCAL_TASK, true,
    "call mom" }, /* FIXED */

  /* DEBATABLE: quotation marks around the item are kept. */

  { "把“周五交报告”加到待办", NY_AGENT_LOCAL_TASK, true, "“周五交报告”" },

  /* 提醒我 with neither a time nor a duration is a to-do. */

  { "提醒我周五交报告", NY_AGENT_LOCAL_TASK, true, "周五交报告" },
  { "提醒我买牛奶", NY_AGENT_LOCAL_TASK, true, "买牛奶" },
  { "明天提醒我交房租", NY_AGENT_LOCAL_TASK, true, "交房租" },
  { "记得提醒我喝水", NY_AGENT_LOCAL_TASK, true, "喝水" },
  { "别忘了提醒我带伞", NY_AGENT_LOCAL_TASK, true, "带伞" },
  { "remind me to water the plants", NY_AGENT_LOCAL_TASK, true,
    "water the plants" },

  { "加个待办", NY_AGENT_LOCAL_TASK, false, "" },
  { "帮我记个待办", NY_AGENT_LOCAL_TASK, false, "" },
  { "待办", NY_AGENT_LOCAL_TASK, false, "" }
};

/****************************************************************************
 * Music
 ****************************************************************************/

static const struct text_case_s g_music_cases[] = {
  { "放音乐", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "放首歌", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "放一首歌", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "来首歌", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "我想听歌", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "我想听音乐", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "播放音乐", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "play music", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "play some music", NY_AGENT_LOCAL_MUSIC_PLAY, true, "" },
  { "播放周杰伦的晴天", NY_AGENT_LOCAL_MUSIC_PLAY, true, "周杰伦的晴天" },
  { "播放 晴天", NY_AGENT_LOCAL_MUSIC_PLAY, true, "晴天" },
  { "来一首小星星", NY_AGENT_LOCAL_MUSIC_PLAY, true, "小星星" },
  { "我想听周杰伦", NY_AGENT_LOCAL_MUSIC_PLAY, true, "周杰伦" },
  { "播放昨天那首歌", NY_AGENT_LOCAL_MUSIC_PLAY, true, "昨天那首歌" },
  { "Play some Jazz", NY_AGENT_LOCAL_MUSIC_PLAY, true, "Jazz" }
};

/****************************************************************************
 * Name: test_kinds
 ****************************************************************************/

static void test_kinds(void)
{
  for (size_t i = 0; i < COUNT(g_kind_cases); i++)
    {
      const struct kind_case_s *c = &g_kind_cases[i];
      struct ny_agent_local_intent_s intent;
      detect(c->utterance, &intent);
      CHECK(intent.kind == c->kind, "[%s] is %s, expected %s", c->utterance,
            kind_name(&intent), ny_agent_local_intent_name(c->kind));
      if (intent.kind == c->kind)
        CHECK(intent.complete == c->complete, "[%s] complete=%d", c->utterance,
              intent.complete);
      if (intent.kind == NY_AGENT_LOCAL_CHAT)
        {
          const char *tool = NULL;
          char *call = ny_agent_local_intent_call(&intent, &g_context, &tool);
          CHECK(call == NULL, "[%s] chat rendered a call", c->utterance);
          CHECK(!intent.complete && intent.text[0] == 0 &&
                    intent.seconds == 0 && intent.percent == -1 &&
                    intent.hour == -1 && intent.expression == NULL,
                "[%s] chat left slots behind", c->utterance);
          free(call);
        }
    }

  /* NULL is nothing said. */

  struct ny_agent_local_intent_s intent;
  detect(NULL, &intent);
  CHECK(intent.kind == NY_AGENT_LOCAL_CHAT, "NULL utterance");
}

/****************************************************************************
 * Name: test_timers
 ****************************************************************************/

static void test_timers(void)
{
  for (size_t i = 0; i < COUNT(g_timer_cases); i++)
    {
      const struct timer_case_s *c = &g_timer_cases[i];
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      detect(c->utterance, &intent);
      CHECK(intent.kind == NY_AGENT_LOCAL_TIMER, "[%s] is %s, expected timer",
            c->utterance, kind_name(&intent));
      if (intent.kind != NY_AGENT_LOCAL_TIMER)
        continue;
      CHECK(intent.seconds == c->seconds, "[%s] seconds=%u, expected %u",
            c->utterance, (unsigned)intent.seconds, (unsigned)c->seconds);
      CHECK(intent.complete == c->complete, "[%s] complete=%d", c->utterance,
            intent.complete);
      CHECK(!strcmp(intent.text, c->label), "[%s] label=[%s], expected [%s]",
            c->utterance, intent.text, c->label);
      if (!intent.complete)
        continue;

      /* {"topic":"timer.create","arguments":{"revision":42,
       *  "kind":"countdown","label":"...","duration_ms":...}}
       */

      cJSON *call = call_of(&intent, &g_context, &tool);
      const cJSON *arguments =
          cJSON_GetObjectItemCaseSensitive(call, "arguments");
      CHECK(call != NULL && !strcmp(tool, "nyabula_action"), "[%s] tool",
            c->utterance);
      CHECK(cJSON_GetArraySize(call) == 2 &&
                !strcmp(text_of(call, "topic"), "timer.create"),
            "[%s] topic", c->utterance);
      CHECK(cJSON_IsObject(arguments) && cJSON_GetArraySize(arguments) == 4,
            "[%s] argument count", c->utterance);
      CHECK(number_of(arguments, "revision") == 42, "[%s] revision",
            c->utterance);
      CHECK(!strcmp(text_of(arguments, "kind"), "countdown"), "[%s] kind",
            c->utterance);
      CHECK(!strcmp(text_of(arguments, "label"), c->label), "[%s] label",
            c->utterance);
      CHECK(number_of(arguments, "duration_ms") == c->seconds * 1000.0,
            "[%s] duration_ms=%f", c->utterance,
            number_of(arguments, "duration_ms"));
      CHECK(cJSON_GetObjectItemCaseSensitive(arguments, "record") == NULL,
            "[%s] timer has no record", c->utterance);
      cJSON_Delete(call);
    }
}

/****************************************************************************
 * Name: test_alarms
 ****************************************************************************/

static void test_alarms(void)
{
  static const char *const days[] = { "sun", "mon", "tue", "wed",
                                      "thu", "fri", "sat" };
  struct ny_agent_local_context_s context = g_context;
  context.utc_offset_minutes = -300;
  context.revision = 9;
  context.now_minutes = -1; /* The hour is rendered as it was read */
  for (size_t i = 0; i < COUNT(g_alarm_cases); i++)
    {
      const struct alarm_case_s *c = &g_alarm_cases[i];
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      char time[8];
      detect(c->utterance, &intent);
      CHECK(intent.kind == NY_AGENT_LOCAL_ALARM, "[%s] is %s, expected alarm",
            c->utterance, kind_name(&intent));
      if (intent.kind != NY_AGENT_LOCAL_ALARM)
        continue;
      CHECK(intent.complete == c->complete, "[%s] complete=%d", c->utterance,
            intent.complete);
      CHECK(intent.hour == c->hour && intent.minute == c->minute,
            "[%s] time %d:%d, expected %d:%d", c->utterance, intent.hour,
            intent.minute, c->hour, c->minute);
      CHECK(intent.ambiguous == c->ambiguous, "[%s] ambiguous=%d",
            c->utterance, intent.ambiguous);
      CHECK(intent.repeat == c->repeat, "[%s] repeat=%#x", c->utterance,
            intent.repeat);
      CHECK(intent.far_date == c->far_date, "[%s] far_date=%d", c->utterance,
            intent.far_date);
      CHECK(!strcmp(intent.text, c->label), "[%s] label=[%s], expected [%s]",
            c->utterance, intent.text, c->label);
      if (!intent.complete)
        continue;

      /* {"topic":"alarm.create","arguments":{"revision":9,"record":{
       *  "time":"HH:MM","label":"...","enabled":true,"repeat":[...],
       *  "utc_offset_minutes":-300}}}
       */

      cJSON *call = call_of(&intent, &context, &tool);
      const cJSON *arguments =
          cJSON_GetObjectItemCaseSensitive(call, "arguments");
      const cJSON *record =
          cJSON_GetObjectItemCaseSensitive(arguments, "record");
      const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(record, "repeat");
      int listed = 0;
      snprintf(time, sizeof(time), "%02d:%02d", c->hour, c->minute);
      CHECK(call != NULL && !strcmp(tool, "nyabula_action"), "[%s] tool",
            c->utterance);
      CHECK(cJSON_GetArraySize(call) == 2 &&
                !strcmp(text_of(call, "topic"), "alarm.create"),
            "[%s] topic", c->utterance);
      CHECK(cJSON_GetArraySize(arguments) == 2 &&
                number_of(arguments, "revision") == 9,
            "[%s] revision", c->utterance);
      CHECK(cJSON_IsObject(record) && cJSON_GetArraySize(record) == 5,
            "[%s] record fields", c->utterance);
      CHECK(!strcmp(text_of(record, "time"), time), "[%s] time=%s",
            c->utterance, text_of(record, "time"));
      CHECK(!strcmp(text_of(record, "label"), c->label), "[%s] label",
            c->utterance);
      CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "enabled")),
            "[%s] enabled", c->utterance);
      CHECK(number_of(record, "utc_offset_minutes") == -300, "[%s] offset",
            c->utterance);
      CHECK(cJSON_IsArray(repeat), "[%s] repeat is an array", c->utterance);
      for (int day = 0; day < 7; day++)
        if (c->repeat & (1u << day))
          {
            const cJSON *name = cJSON_GetArrayItem(repeat, listed++);
            CHECK(cJSON_IsString(name) &&
                      !strcmp(name->valuestring, days[day]),
                  "[%s] repeat day %d", c->utterance, day);
          }
      CHECK(cJSON_GetArraySize(repeat) == listed, "[%s] repeat count",
            c->utterance);
      cJSON_Delete(call);
    }

  for (size_t i = 0; i < COUNT(g_resolve_cases); i++)
    {
      const struct resolve_case_s *c = &g_resolve_cases[i];
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      context.now_minutes = c->now_minutes;
      detect(c->utterance, &intent);
      cJSON *call = call_of(&intent, &context, &tool);
      const cJSON *record = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(call, "arguments"), "record");
      CHECK(!strcmp(text_of(record, "time"), c->time),
            "[%s] at minute %d rings %s, expected %s", c->utterance,
            c->now_minutes, text_of(record, "time"), c->time);
      cJSON_Delete(call);
    }
}

/****************************************************************************
 * Name: test_volume
 ****************************************************************************/

static void test_volume(void)
{
  for (size_t i = 0; i < COUNT(g_volume_cases); i++)
    {
      const struct volume_case_s *c = &g_volume_cases[i];
      struct ny_agent_local_context_s context = g_context;
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      context.volume = c->before;
      context.muted = c->muted_before;
      detect(c->utterance, &intent);
      CHECK(intent.kind == c->kind, "[%s] is %s, expected %s", c->utterance,
            kind_name(&intent), ny_agent_local_intent_name(c->kind));
      if (intent.kind != c->kind)
        continue;
      CHECK(intent.complete == c->complete, "[%s] complete=%d", c->utterance,
            intent.complete);
      if (c->complete || c->percent != -1)
        CHECK(intent.percent == c->percent, "[%s] percent=%d, expected %d",
              c->utterance, intent.percent, c->percent);
      if (!intent.complete)
        continue;

      /* {"topic":"music.volume","arguments":{"volume":N,"muted":B}} */

      cJSON *call = call_of(&intent, &context, &tool);
      const cJSON *arguments =
          cJSON_GetObjectItemCaseSensitive(call, "arguments");
      const cJSON *muted =
          cJSON_GetObjectItemCaseSensitive(arguments, "muted");
      CHECK(call != NULL && !strcmp(tool, "nyabula_music"), "[%s] tool",
            c->utterance);
      CHECK(cJSON_GetArraySize(call) == 2 &&
                !strcmp(text_of(call, "topic"), "music.volume"),
            "[%s] topic", c->utterance);
      CHECK(cJSON_GetArraySize(arguments) == 2, "[%s] argument count",
            c->utterance);
      CHECK(number_of(arguments, "volume") == c->after,
            "[%s] from %d gives %.0f, expected %d", c->utterance, c->before,
            number_of(arguments, "volume"), c->after);
      CHECK(cJSON_IsBool(muted) && cJSON_IsTrue(muted) == c->muted_after,
            "[%s] muted", c->utterance);
      cJSON_Delete(call);
    }
}

/****************************************************************************
 * Name: test_expressions
 ****************************************************************************/

static void test_expressions(void)
{
  static const char *const all[] = {
    "idle",  "curious", "happy", "processing", "star",  "heart", "sleepy",
    "sleep", "angry",   "sad",   "surprise",   "dizzy", "derp"
  };
  bool reached[COUNT(all)] = { false };
  for (size_t i = 0; i < COUNT(g_expression_cases); i++)
    {
      const struct expression_case_s *c = &g_expression_cases[i];
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      detect(c->utterance, &intent);
      CHECK(intent.kind == NY_AGENT_LOCAL_EXPRESSION,
            "[%s] is %s, expected expression", c->utterance,
            kind_name(&intent));
      if (intent.kind != NY_AGENT_LOCAL_EXPRESSION)
        continue;
      if (c->expression == NULL)
        {
          char *call = ny_agent_local_intent_call(&intent, &g_context, &tool);
          CHECK(!intent.complete && intent.expression == NULL,
                "[%s] should ask which expression", c->utterance);
          CHECK(call == NULL, "[%s] rendered a call without an expression",
                c->utterance);
          free(call);
          continue;
        }
      CHECK(intent.complete && intent.expression &&
                !strcmp(intent.expression, c->expression),
            "[%s] expression=%s, expected %s", c->utterance,
            intent.expression ? intent.expression : "(null)", c->expression);
      for (size_t j = 0; j < COUNT(all); j++)
        if (intent.expression && !strcmp(intent.expression, all[j]))
          reached[j] = true;

      /* {"expression":"happy"}: no topic, no arguments. */

      cJSON *call = call_of(&intent, &g_context, &tool);
      CHECK(call != NULL && !strcmp(tool, "nyabula_expression"), "[%s] tool",
            c->utterance);
      CHECK(cJSON_GetArraySize(call) == 1 &&
                !strcmp(text_of(call, "expression"), c->expression),
            "[%s] call shape", c->utterance);
      cJSON_Delete(call);
    }
  for (size_t j = 0; j < COUNT(all); j++)
    CHECK(reached[j], "no utterance reaches the expression %s", all[j]);
  for (size_t i = 0; i < COUNT(g_expression_misses); i++)
    {
      struct ny_agent_local_intent_s intent;
      detect(g_expression_misses[i], &intent);
      CHECK(intent.kind == NY_AGENT_LOCAL_CHAT, "[%s] is %s (was a miss)",
            g_expression_misses[i], kind_name(&intent));
    }
}

/****************************************************************************
 * Name: test_texts
 ****************************************************************************/

static void test_texts(void)
{
  for (size_t i = 0; i < COUNT(g_text_cases); i++)
    {
      const struct text_case_s *c = &g_text_cases[i];
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      bool memory = c->kind == NY_AGENT_LOCAL_REMEMBER;
      detect(c->utterance, &intent);
      CHECK(intent.kind == c->kind, "[%s] is %s, expected %s", c->utterance,
            kind_name(&intent), ny_agent_local_intent_name(c->kind));
      if (intent.kind != c->kind)
        continue;
      CHECK(!strcmp(intent.text, c->text), "[%s] text=[%s], expected [%s]",
            c->utterance, intent.text, c->text);
      CHECK(intent.complete == c->complete, "[%s] complete=%d", c->utterance,
            intent.complete);
      if (!intent.complete)
        continue;

      /* {"topic":"memory.create","arguments":{"revision":42,
       *  "record":{"text":"..."}}}
       * {"topic":"task.create","arguments":{"revision":42,
       *  "record":{"title":"...","state":"queued"}}}
       */

      cJSON *call = call_of(&intent, &g_context, &tool);
      const cJSON *arguments =
          cJSON_GetObjectItemCaseSensitive(call, "arguments");
      const cJSON *record =
          cJSON_GetObjectItemCaseSensitive(arguments, "record");
      CHECK(call != NULL && !strcmp(tool, "nyabula_action"), "[%s] tool",
            c->utterance);
      CHECK(cJSON_GetArraySize(call) == 2 &&
                !strcmp(text_of(call, "topic"),
                        memory ? "memory.create" : "task.create"),
            "[%s] topic", c->utterance);
      CHECK(cJSON_GetArraySize(arguments) == 2 &&
                number_of(arguments, "revision") == 42,
            "[%s] revision", c->utterance);
      CHECK(cJSON_GetArraySize(record) == (memory ? 1 : 2), "[%s] record",
            c->utterance);
      CHECK(!strcmp(text_of(record, memory ? "text" : "title"), c->text),
            "[%s] record text", c->utterance);
      if (!memory)
        CHECK(!strcmp(text_of(record, "state"), "queued"), "[%s] state",
              c->utterance);
      cJSON_Delete(call);
    }
}

/****************************************************************************
 * Name: test_lengths
 * Description: The limits: 511 bytes of utterance, 160 of title, 96 of
 *   label, all cut between characters.
 ****************************************************************************/

static void test_lengths(void)
{
  struct ny_agent_local_intent_s intent;
  char utterance[1024];
  char expected[1024];
  size_t used;

  /* 记住 + 505 ASCII bytes = 511: the longest utterance that is read. */

  used = (size_t)snprintf(utterance, sizeof(utterance), "记住");
  memset(utterance + used, 'a', 505);
  utterance[used + 505] = 0;
  detect(utterance, &intent);
  CHECK(intent.kind == NY_AGENT_LOCAL_REMEMBER && intent.complete &&
            strlen(intent.text) == 505,
        "511-byte utterance: %s, %zu bytes of text", kind_name(&intent),
        strlen(intent.text));

  /* One more byte is a story, not a command. */

  utterance[used + 505] = 'a';
  utterance[used + 506] = 0;
  detect(utterance, &intent);
  CHECK(intent.kind == NY_AGENT_LOCAL_CHAT, "512-byte utterance is %s",
        kind_name(&intent));

  /* Very long input */

  memset(utterance, 'x', sizeof(utterance) - 1);
  utterance[sizeof(utterance) - 1] = 0;
  detect(utterance, &intent);
  CHECK(intent.kind == NY_AGENT_LOCAL_CHAT, "1023-byte utterance is %s",
        kind_name(&intent));

  /* A to-do title of 60 three-byte characters is cut to 53: 159 bytes, the
   * last whole character under 160.
   */

  used = (size_t)snprintf(utterance, sizeof(utterance), "待办：");
  expected[0] = 0;
  for (int i = 0; i < 60; i++)
    {
      strcat(utterance, "猫");
      if (i < 53)
        strcat(expected, "猫");
    }
  detect(utterance, &intent);
  CHECK(intent.kind == NY_AGENT_LOCAL_TASK && intent.complete &&
            !strcmp(intent.text, expected),
        "long title: %zu bytes, expected %zu", strlen(intent.text),
        strlen(expected));
  CHECK(valid(intent.text), "long title is cut inside a character");

  /* The same with one ASCII byte in front: 1 + 53 * 3 = 160 fits whole. */

  snprintf(utterance, sizeof(utterance), "待办：a");
  snprintf(expected, sizeof(expected), "a");
  for (int i = 0; i < 60; i++)
    {
      strcat(utterance, "猫");
      if (i < 53)
        strcat(expected, "猫");
    }
  detect(utterance, &intent);
  CHECK(!strcmp(intent.text, expected) && strlen(intent.text) == 160,
        "long title, shifted: %zu bytes", strlen(intent.text));

  /* A timer label of 40 characters is cut to 32: 96 bytes. */

  snprintf(utterance, sizeof(utterance), "十分钟后提醒我");
  expected[0] = 0;
  for (int i = 0; i < 40; i++)
    {
      strcat(utterance, "喵");
      if (i < 32)
        strcat(expected, "喵");
    }
  detect(utterance, &intent);
  CHECK(intent.kind == NY_AGENT_LOCAL_TIMER && intent.seconds == 600 &&
            !strcmp(intent.text, expected),
        "long label: %zu bytes", strlen(intent.text));

  /* Four-byte characters: 96 / 4 = 24 whole ones. */

  snprintf(utterance, sizeof(utterance), "晚上十点提醒我");
  expected[0] = 0;
  for (int i = 0; i < 30; i++)
    {
      strcat(utterance, "😀");
      if (i < 24)
        strcat(expected, "😀");
    }
  detect(utterance, &intent);
  CHECK(intent.kind == NY_AGENT_LOCAL_ALARM && !strcmp(intent.text, expected),
        "long emoji label: %zu bytes", strlen(intent.text));

  /* _copy() itself */

  char out[8];
  CHECK(ny_agent_local_intent_copy(out, 0, "abc", 3) == 0, "copy size 0");
  CHECK(ny_agent_local_intent_copy(out, 1, "abc", 3) == 0 && out[0] == 0,
        "copy size 1");
  CHECK(ny_agent_local_intent_copy(out, 8, "abc", 3) == 3 &&
            !strcmp(out, "abc"),
        "copy that fits");
  CHECK(ny_agent_local_intent_copy(out, 8, "abc", 2) == 2 &&
            !strcmp(out, "ab"),
        "copy of a prefix");
  CHECK(ny_agent_local_intent_copy(out, 8, "猫猫猫", 9) == 6 &&
            !strcmp(out, "猫猫"),
        "copy cut on a 3-byte boundary");
  CHECK(ny_agent_local_intent_copy(out, 6, "a😀😀", 9) == 5 &&
            !strcmp(out, "a😀"),
        "copy cut on a 4-byte boundary");
  CHECK(ny_agent_local_intent_copy(out, 4, "😀", 4) == 0 && out[0] == 0,
        "copy with no room for one character");
}

/****************************************************************************
 * Name: test_music
 ****************************************************************************/

static void test_music(void)
{
  struct ny_agent_local_context_s context = g_context;
  struct ny_agent_local_intent_s intent;
  const char *tool = NULL;
  char name[64];
  for (size_t i = 0; i < COUNT(g_music_cases); i++)
    {
      const struct text_case_s *c = &g_music_cases[i];
      detect(c->utterance, &intent);
      CHECK(intent.kind == c->kind && intent.complete, "[%s] is %s",
            c->utterance, kind_name(&intent));
      CHECK(!strcmp(intent.text, c->text), "[%s] hint=[%s], expected [%s]",
            c->utterance, intent.text, c->text);

      /* {"topic":"music.play","arguments":{"name":"song.mp3"}} */

      cJSON *call = call_of(&intent, &g_context, &tool);
      const cJSON *arguments =
          cJSON_GetObjectItemCaseSensitive(call, "arguments");
      CHECK(call != NULL && !strcmp(tool, "nyabula_music") &&
                cJSON_GetArraySize(call) == 2 &&
                !strcmp(text_of(call, "topic"), "music.play") &&
                cJSON_GetArraySize(arguments) == 1 &&
                !strcmp(text_of(arguments, "name"), "song.mp3"),
            "[%s] call shape", c->utterance);
      cJSON_Delete(call);
    }

  /* Without a track there is nothing to play. */

  context.track = NULL;
  detect("放音乐", &intent);
  char *encoded = ny_agent_local_intent_call(&intent, &context, &tool);
  CHECK(encoded == NULL, "music.play without a track rendered a call");
  free(encoded);

  /* Pause, resume and stop carry an empty arguments object. */

  static const char *const controls[][2] = { { "暂停音乐", "music.pause" },
                                             { "继续播放", "music.resume" },
                                             { "停止播放", "music.stop" } };
  for (size_t i = 0; i < COUNT(controls); i++)
    {
      detect(controls[i][0], &intent);
      cJSON *call = call_of(&intent, &g_context, &tool);
      const cJSON *arguments =
          cJSON_GetObjectItemCaseSensitive(call, "arguments");
      CHECK(call != NULL && !strcmp(tool, "nyabula_music") &&
                cJSON_GetArraySize(call) == 2 &&
                !strcmp(text_of(call, "topic"), controls[i][1]) &&
                cJSON_IsObject(arguments) &&
                cJSON_GetArraySize(arguments) == 0,
            "[%s] call shape", controls[i][0]);
      cJSON_Delete(call);
    }

  /* Picking a file for the hint */

  cJSON *library =
      cJSON_Parse("{\"items\":[{\"name\":\"notes.txt\",\"supported\":false},"
                  "{\"name\":\"Sunny Day.mp3\",\"supported\":true},"
                  "{\"name\":\"周杰伦 - 晴天.flac\",\"supported\":true},"
                  "{\"name\":\"Jazz Night.mp3\",\"supported\":true}]}");
  cJSON *texts = cJSON_Parse(
      "{\"items\":[{\"name\":\"notes.txt\",\"supported\":false}]}");
  cJSON *empty = cJSON_Parse("{\"items\":[]}");
  CHECK(ny_agent_local_intent_track(library, "", name, sizeof(name)) == 0 &&
            !strcmp(name, "Sunny Day.mp3"),
        "no hint plays the first supported file, got [%s]", name);
  CHECK(ny_agent_local_intent_track(library, NULL, name, sizeof(name)) == 0,
        "NULL hint");
  CHECK(ny_agent_local_intent_track(library, "晴天", name, sizeof(name)) ==
                0 &&
            !strcmp(name, "周杰伦 - 晴天.flac"),
        "hint 晴天 picked [%s]", name);
  CHECK(ny_agent_local_intent_track(library, "sunny", name, sizeof(name)) ==
                0 &&
            !strcmp(name, "Sunny Day.mp3"),
        "lower-case hint");

  /* FIXED: the hint is the owner's own spelling ("Play some Jazz" gives
   * "Jazz"); only the file name was folded, so it never matched.
   */

  detect("Play some Jazz", &intent);
  CHECK(ny_agent_local_intent_track(library, intent.text, name,
                                    sizeof(name)) == 0 &&
            !strcmp(name, "Jazz Night.mp3"),
        "hint [%s] with capitals", intent.text);
  CHECK(ny_agent_local_intent_track(library, "小星星", name, sizeof(name)) ==
            -ESRCH,
        "a piece that is not there");
  CHECK(ny_agent_local_intent_track(texts, "", name, sizeof(name)) == -ENOENT,
        "nothing playable");
  CHECK(ny_agent_local_intent_track(empty, "", name, sizeof(name)) == -ENOENT,
        "empty library");
  CHECK(ny_agent_local_intent_track(NULL, "", name, sizeof(name)) == -ENOENT,
        "no library");
  CHECK(ny_agent_local_intent_track(library, "", name, 8) == -ENOENT,
        "no file name fits 8 bytes");
  cJSON_Delete(library);
  cJSON_Delete(texts);
  cJSON_Delete(empty);
}

/****************************************************************************
 * Name: test_reads
 ****************************************************************************/

static void test_reads(void)
{
  static const struct
  {
    const char *utterance;
    const char *topic;
  } cases[] = {
    { "现在几点了", "system.time.get" }, { "今天几号", "system.time.get" },
    { "今天天气怎么样", "weather.get" }, { "设备状态", "device.status" },
    { "有哪些计时器", "timer.list" },    { "有几个闹钟", "alarm.list" },
    { "我有哪些待办", "task.list" }
  };
  for (size_t i = 0; i < COUNT(cases); i++)
    {
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      detect(cases[i].utterance, &intent);

      /* {"topic":"..."} and nothing else: nyabula_read takes one key. */

      cJSON *call = call_of(&intent, &g_context, &tool);
      CHECK(call != NULL && !strcmp(tool, "nyabula_read") &&
                cJSON_GetArraySize(call) == 1 &&
                !strcmp(text_of(call, "topic"), cases[i].topic),
            "[%s] call shape", cases[i].utterance);
      cJSON_Delete(call);
    }
}

/****************************************************************************
 * Name: test_names
 ****************************************************************************/

static void test_names(void)
{
  static const char *const names[] = {
    "chat",       "time",       "date",        "weather",      "status",
    "timer.list", "alarm.list", "task.list",   "expression",   "timer",
    "alarm",      "volume.set", "volume.up",   "volume.down",  "mute",
    "unmute",     "music.play", "music.pause", "music.resume", "music.stop",
    "remember",   "task"
  };
  CHECK(COUNT(names) == (size_t)KINDS, "name table size");
  for (int kind = 0; kind < KINDS; kind++)
    {
      const char *list =
          ny_agent_local_intent_revision((enum ny_agent_local_intent_e)kind);
      const char *expected = kind == NY_AGENT_LOCAL_TIMER      ? "timer.list"
                             : kind == NY_AGENT_LOCAL_ALARM    ? "alarm.list"
                             : kind == NY_AGENT_LOCAL_REMEMBER ? "memory.list"
                             : kind == NY_AGENT_LOCAL_TASK     ? "task.list"
                                                               : NULL;
      CHECK(!strcmp(
                ny_agent_local_intent_name((enum ny_agent_local_intent_e)kind),
                names[kind]),
            "name of kind %d", kind);
      CHECK((list == NULL) == (expected == NULL) &&
                (list == NULL || !strcmp(list, expected)),
            "revision list of %s", names[kind]);
    }
  CHECK(!strcmp(ny_agent_local_intent_name((enum ny_agent_local_intent_e)99),
                "?"),
        "name of an unknown kind");
}

/****************************************************************************
 * Name: test_fill
 * Description: The forced model call: which tool is offered, and what is
 *   believed of the answer.
 ****************************************************************************/

struct fill_case_s
{
  const char *utterance;
  const char *offered; /* Tool the model is forced to call, or NULL */
  const char *name;    /* Tool the model answered with */
  const char *arguments;
  int result;
  uint32_t seconds;
  int percent;
  int hour;
  int minute;
  const char *expression;
};

static const struct fill_case_s g_fill_cases[] = {
  /* Expression: the 13 names, any case, and the measured near misses. */

  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"happy\"}", 0, 0, -1, -1, -1, "happy" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"Sleepy\"}", 0, 0, -1, -1, -1, "sleepy" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"anger\"}", 0, 0, -1, -1, -1, "angry" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"ANGER\"}", 0, 0, -1, -1, -1, "angry" },
  { "换个表情", "set_expression", "set_expression", "{\"expression\":\"joy\"}",
    0, 0, -1, -1, -1, "happy" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"sadness\"}", 0, 0, -1, -1, -1, "sad" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"tired\"}", 0, 0, -1, -1, -1, "sleepy" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"asleep\"}", 0, 0, -1, -1, -1, "sleep" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"shocked\"}", 0, 0, -1, -1, -1, "surprise" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"confused\"}", 0, 0, -1, -1, -1, "dizzy" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"curiosity\"}", 0, 0, -1, -1, -1, "curious" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"stars\"}", 0, 0, -1, -1, -1, "star" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"cute\"}", 0, 0, -1, -1, -1, "heart" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"default\"}", 0, 0, -1, -1, -1, "idle" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"thinking\"}", 0, 0, -1, -1, -1, "processing" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"silly\"}", 0, 0, -1, -1, -1, "derp" },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"banana\"}", -EINVAL, 0, -1, -1, -1, NULL },
  { "换个表情", "set_expression", "set_expression", "{\"expression\":\"\"}",
    -EINVAL, 0, -1, -1, -1, NULL },
  { "换个表情", "set_expression", "set_expression", "{\"expression\":7}",
    -EINVAL, 0, -1, -1, -1, NULL },
  { "换个表情", "set_expression", "set_expression",
    "{\"expression\":\"happyhappyhappyhappyhappyhappyhappy\"}", -EINVAL, 0, -1,
    -1, -1, NULL },
  { "换个表情", "set_expression", "set_expression", "{}", -EINVAL, 0, -1, -1,
    -1, NULL },
  { "换个表情", "set_expression", "set_expression", "[\"happy\"]", -EINVAL, 0,
    -1, -1, -1, NULL },
  { "换个表情", "set_expression", "set_timer", "{\"minutes\":5}", -EINVAL, 0,
    -1, -1, -1, NULL },
  { "换个表情", "set_expression", NULL, "{\"expression\":\"happy\"}", -EINVAL,
    0, -1, -1, -1, NULL },

  /* Timer: minutes as a number or as a string, 1 second to 24 hours. */

  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":20}", 0, 1200,
    -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":\"2.5\"}", 0,
    150, -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":1440}", 0, 86400,
    -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":1441}", -EINVAL,
    0, -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":0}", -EINVAL, 0,
    -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":-5}", -EINVAL, 0,
    -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":1e300}", -EINVAL,
    0, -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":\"soon\"}",
    -EINVAL, 0, -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":\"\"}", -EINVAL,
    0, -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"minutes\":null}", -EINVAL,
    0, -1, -1, -1, NULL },
  { "定个计时器二十", "set_timer", "set_timer", "{\"seconds\":30}", -EINVAL, 0,
    -1, -1, -1, NULL },

  /* No quantity in the words: the model is not asked to invent one. */

  { "帮我定个计时器", NULL, "set_timer", "{\"minutes\":5}", -EINVAL, 0, -1, -1,
    -1, NULL },

  /* A duration that was read and refused is not read again. */

  { "计时25小时", NULL, "set_timer", "{\"minutes\":5}", -EINVAL, 90000, -1, -1,
    -1, NULL },

  /* DEBATABLE: "三天" has a numeral and no unit the rules know, so the model
   * is asked for minutes; whether it says 4320 (refused) or 3 is its call.
   */

  { "计时三天", "set_timer", "set_timer", "{\"minutes\":4320}", -EINVAL, 0, -1,
    -1, -1, NULL },

  /* Alarm: 24-hour HH:MM and nothing after it. */

  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"07:30\"}", 0, 0,
    -1, 7, 30, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"7:05\"}", 0, 0,
    -1, 7, 5, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"23:59\"}", 0, 0,
    -1, 23, 59, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"00:00\"}", 0, 0,
    -1, 0, 0, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"24:00\"}",
    -EINVAL, 0, -1, -1, -1, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"12:60\"}",
    -EINVAL, 0, -1, -1, -1, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"7:30pm\"}",
    -EINVAL, 0, -1, -1, -1, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"seven\"}",
    -EINVAL, 0, -1, -1, -1, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":730}", -EINVAL, 0,
    -1, -1, -1, NULL },
  { "定个25点的闹钟", "set_alarm", "set_alarm", "{}", -EINVAL, 0, -1, -1, -1,
    NULL },

  /* DEBATABLE: seconds after the minutes are refused, not dropped. */

  { "定个25点的闹钟", "set_alarm", "set_alarm", "{\"time\":\"07:30:00\"}",
    -EINVAL, 0, -1, -1, -1, NULL },

  /* No numeral, or a far date: the owner is asked, not the model. */

  { "帮我定个闹钟", NULL, "set_alarm", "{\"time\":\"07:30\"}", -EINVAL, 0, -1,
    -1, -1, NULL },
  { "下周一早上八点叫我", NULL, "set_alarm", "{\"time\":\"07:30\"}", -EINVAL,
    0, -1, 8, 0, NULL },

  /* Volume: a whole number from 0 to 100. */

  { "音量调到150", "set_volume", "set_volume", "{\"percent\":30}", 0, 0, 30,
    -1, -1, NULL },
  { "音量调到150", "set_volume", "set_volume", "{\"percent\":\"40\"}", 0, 0,
    40, -1, -1, NULL },
  { "音量调到150", "set_volume", "set_volume", "{\"percent\":0}", 0, 0, 0, -1,
    -1, NULL },
  { "音量调到150", "set_volume", "set_volume", "{\"percent\":100}", 0, 0, 100,
    -1, -1, NULL },
  { "音量调到150", "set_volume", "set_volume", "{\"percent\":101}", -EINVAL, 0,
    150, -1, -1, NULL },
  { "音量调到150", "set_volume", "set_volume", "{\"percent\":-1}", -EINVAL, 0,
    150, -1, -1, NULL },
  { "音量调到150", "set_volume", "set_volume", "{\"percent\":30.5}", -EINVAL,
    0, 150, -1, -1, NULL },
  { "调一下音量", NULL, "set_volume", "{\"percent\":30}", -EINVAL, 0, -1, -1,
    -1, NULL },

  /* Kinds the model is never asked about */

  { "记住", NULL, "remember", "{\"text\":\"x\"}", -EINVAL, 0, -1, -1, -1,
    NULL },
  { "加个待办", NULL, "add_task", "{\"title\":\"x\"}", -EINVAL, 0, -1, -1, -1,
    NULL },
  { "你好呀", NULL, "set_timer", "{\"minutes\":5}", -EINVAL, 0, -1, -1, -1,
    NULL }
};

static void test_fill(void)
{
  for (size_t i = 0; i < COUNT(g_fill_cases); i++)
    {
      const struct fill_case_s *c = &g_fill_cases[i];
      struct ny_agent_local_intent_s intent;
      const char *offered = "unset";
      cJSON *arguments = cJSON_Parse(c->arguments);
      detect(c->utterance, &intent);
      const char *table = ny_agent_local_intent_forced(&intent, &offered);
      CHECK((table != NULL) == (c->offered != NULL), "[%s] forced table %s",
            c->utterance, table ? "offered" : "withheld");
      if (table)
        {
          /* The table is one OpenAI function whose name is *name. */

          cJSON *tools = cJSON_Parse(table);
          const cJSON *function = cJSON_GetObjectItemCaseSensitive(
              cJSON_GetArrayItem(tools, 0), "function");
          CHECK(cJSON_IsArray(tools) && cJSON_GetArraySize(tools) == 1 &&
                    !strcmp(text_of(cJSON_GetArrayItem(tools, 0), "type"),
                            "function") &&
                    c->offered && !strcmp(offered, c->offered) &&
                    !strcmp(text_of(function, "name"), offered) &&
                    cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(
                        function, "parameters")),
                "[%s] forced table shape, tool %s", c->utterance, offered);
          cJSON_Delete(tools);
        }
      int result = ny_agent_local_intent_fill(&intent, c->name, arguments);
      CHECK(result == c->result, "[%s] fill %s %s = %d, expected %d",
            c->utterance, c->name ? c->name : "(null)", c->arguments, result,
            c->result);
      CHECK(intent.complete == (c->result == 0), "[%s] %s complete=%d",
            c->utterance, c->arguments, intent.complete);
      CHECK(intent.seconds == c->seconds, "[%s] %s seconds=%u", c->utterance,
            c->arguments, (unsigned)intent.seconds);
      CHECK(intent.percent == c->percent, "[%s] %s percent=%d", c->utterance,
            c->arguments, intent.percent);
      CHECK(intent.hour == c->hour && intent.minute == c->minute,
            "[%s] %s time %d:%d", c->utterance, c->arguments, intent.hour,
            intent.minute);
      CHECK((intent.expression == NULL) == (c->expression == NULL) &&
                (c->expression == NULL ||
                 !strcmp(intent.expression, c->expression)),
            "[%s] %s expression=%s", c->utterance, c->arguments,
            intent.expression ? intent.expression : "(null)");
      if (result == 0)
        {
          /* What was filled in renders like what was read. */

          const char *tool = NULL;
          cJSON *call = call_of(&intent, &g_context, &tool);
          CHECK(call != NULL, "[%s] %s: filled intent renders no call",
                c->utterance, c->arguments);
          cJSON_Delete(call);
        }
      cJSON_Delete(arguments);
    }

  /* A filled alarm is no longer ambiguous: the model said which half. */

  struct ny_agent_local_intent_s intent;
  cJSON *arguments = cJSON_Parse("{\"time\":\"19:00\"}");
  detect("定个七点的闹钟", &intent);
  CHECK(intent.ambiguous, "七点 is ambiguous");
  CHECK(ny_agent_local_intent_fill(&intent, "set_alarm", arguments) == 0 &&
            !intent.ambiguous && intent.hour == 19,
        "fill clears the ambiguity");
  cJSON_Delete(arguments);

  /* Every kind has something to ask, and none of it reads as a refusal. */

  for (int kind = 0; kind < KINDS; kind++)
    {
      memset(&intent, 0, sizeof(intent));
      intent.kind = (enum ny_agent_local_intent_e)kind;
      const char *question = ny_agent_local_intent_question(&intent);
      CHECK(question && question[0] && valid(question) &&
                !strstr(question, "无法") && !strstr(question, "cannot"),
            "question for kind %d", kind);
    }
  detect("计时25小时", &intent);
  CHECK(strstr(ny_agent_local_intent_question(&intent), "24 小时") != NULL,
        "the question for a 25-hour timer names the limit");
  detect("下周一早上八点叫我", &intent);
  CHECK(strstr(ny_agent_local_intent_question(&intent), "重复") != NULL,
        "the question for a far date says what alarms can do");
}

/****************************************************************************
 * Name: test_reply
 ****************************************************************************/

static void test_reply(void)
{
  const char *arguments = "{\"topic\":\"timer.list\"}";
  char *first = ny_agent_local_intent_reply("nyabula_read", arguments, NULL,
                                            0x1234abcdULL);
  char *second = ny_agent_local_intent_reply("nyabula_read", arguments, NULL,
                                             0x1234abceULL);
  char *plain = ny_agent_local_intent_reply(NULL, NULL, "要计时多久呢？", 7);
  char *bare = ny_agent_local_intent_reply("nyabula_read", NULL, NULL, 8);
  char *silent = ny_agent_local_intent_reply(NULL, NULL, NULL, 9);
  cJSON *a = cJSON_Parse(first);
  cJSON *b = cJSON_Parse(second);
  cJSON *c = cJSON_Parse(plain);
  cJSON *d = cJSON_Parse(bare);
  cJSON *e = cJSON_Parse(silent);
  const cJSON *choice =
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(a, "choices"), 0);
  const cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
  const cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
  const cJSON *call = cJSON_GetArrayItem(calls, 0);
  const cJSON *function = cJSON_GetObjectItemCaseSensitive(call, "function");
  const cJSON *usage = cJSON_GetObjectItemCaseSensitive(a, "usage");
  CHECK(a && b && c && d && e, "replies parse");

  /* A tool call */

  CHECK(!strcmp(text_of(a, "id"), "chatcmpl-rule-1234abcd"), "id %s",
        text_of(a, "id"));
  CHECK(!strcmp(text_of(a, "object"), "chat.completion"), "object");
  CHECK(!strcmp(text_of(a, "model"), "on-device-rules"), "model");
  CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(a, "choices")) ==
            1,
        "one choice");
  CHECK(number_of(choice, "index") == 0, "choice index");
  CHECK(!strcmp(text_of(choice, "finish_reason"), "tool_calls"),
        "finish_reason");
  CHECK(!strcmp(text_of(message, "role"), "assistant"), "role");
  CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(message, "content")),
        "content is null beside a tool call");
  CHECK(cJSON_IsArray(calls) && cJSON_GetArraySize(calls) == 1,
        "one tool call");
  CHECK(!strcmp(text_of(call, "id"), "call_1234abcd"), "call id %s",
        text_of(call, "id"));
  CHECK(!strcmp(text_of(call, "type"), "function"), "call type");
  CHECK(!strcmp(text_of(function, "name"), "nyabula_read"), "function name");

  /* arguments is the JSON text, a string, not an object. */

  CHECK(!strcmp(text_of(function, "arguments"), arguments),
        "function arguments");
  CHECK(number_of(usage, "prompt_tokens") == 0 &&
            number_of(usage, "completion_tokens") == 0 &&
            number_of(usage, "total_tokens") == 0,
        "usage");

  /* Different serials, different ids */

  CHECK(strcmp(text_of(a, "id"), text_of(b, "id")) != 0, "completion ids");
  const cJSON *other = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(
              cJSON_GetArrayItem(
                  cJSON_GetObjectItemCaseSensitive(b, "choices"), 0),
              "message"),
          "tool_calls"),
      0);
  CHECK(strcmp(text_of(call, "id"), text_of(other, "id")) != 0, "call ids");

  /* A plain answer */

  choice =
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(c, "choices"), 0);
  message = cJSON_GetObjectItemCaseSensitive(choice, "message");
  CHECK(!strcmp(text_of(message, "content"), "要计时多久呢？"), "content");
  CHECK(!strcmp(text_of(message, "role"), "assistant"), "plain role");
  CHECK(cJSON_GetObjectItemCaseSensitive(message, "tool_calls") == NULL,
        "a plain answer has no tool_calls");
  CHECK(!strcmp(text_of(choice, "finish_reason"), "stop"), "plain finish");
  CHECK(!strcmp(text_of(c, "id"), "chatcmpl-rule-7"), "plain id");

  /* Missing pieces have defaults. */

  function = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetArrayItem(
          cJSON_GetObjectItemCaseSensitive(
              cJSON_GetObjectItemCaseSensitive(
                  cJSON_GetArrayItem(
                      cJSON_GetObjectItemCaseSensitive(d, "choices"), 0),
                  "message"),
              "tool_calls"),
          0),
      "function");
  CHECK(!strcmp(text_of(function, "arguments"), "{}"), "default arguments");
  message = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(e, "choices"), 0),
      "message");
  CHECK(!strcmp(text_of(message, "content"), ""), "default content");

  /* The largest serial still fits the id buffer. */

  char *large = ny_agent_local_intent_reply(NULL, NULL, "x", UINT64_MAX);
  cJSON *f = cJSON_Parse(large);
  CHECK(!strcmp(text_of(f, "id"), "chatcmpl-rule-ffffffffffffffff"),
        "largest id %s", text_of(f, "id"));
  cJSON_Delete(a);
  cJSON_Delete(b);
  cJSON_Delete(c);
  cJSON_Delete(d);
  cJSON_Delete(e);
  cJSON_Delete(f);
  free(first);
  free(second);
  free(plain);
  free(bare);
  free(silent);
  free(large);
}

/****************************************************************************
 * Name: test_facts
 ****************************************************************************/

struct fact_case_s
{
  const char *tool;
  const char *arguments; /* NULL with `utterance`: rendered by _call() */
  const char *utterance;
  const char *result;
  int utc_offset_minutes;
  bool failed;
  const char *fact;
};

static const struct fact_case_s g_fact_cases[] = {
  /* Time: 2026-09-19 12:00 UTC is a Saturday. */

  { "nyabula_read", NULL, "现在几点了",
    "{\"unix_ms\":1789819200000,\"clock_valid\":true}", 480, false,
    "现在是 2026-09-19 20:00（周六）" },
  { "nyabula_read", NULL, "现在几点了",
    "{\"unix_ms\":1789819200000,\"clock_valid\":true}", 0, false,
    "现在是 2026-09-19 12:00（周六）" },
  { "nyabula_read", NULL, "今天星期几",
    "{\"unix_ms\":1789842600000,\"clock_valid\":true}", 480, false,
    "现在是 2026-09-20 02:30（周日）" }, /* 18:30 UTC: tomorrow in UTC+8 */
  { "nyabula_read", NULL, "现在几点了",
    "{\"unix_ms\":1789819200000,\"clock_valid\":true}", -780, false,
    "现在是 2026-09-18 23:00（周五）" }, /* Yesterday in UTC-13 */
  { "nyabula_read", NULL, "现在几点了",
    "{\"unix_ms\":1709251140000,\"clock_valid\":true}", 330, false,
    "现在是 2024-03-01 05:29（周五）" }, /* Leap day 23:59 UTC, UTC+5:30 */
  { "nyabula_read", NULL, "现在几点了",
    "{\"unix_ms\":946684800000,\"clock_valid\":true}", 0, false,
    "现在是 2000-01-01 00:00（周六）" },
  { "nyabula_read", NULL, "现在几点了",
    "{\"unix_ms\":1789819200000,\"clock_valid\":false}", 480, false,
    "设备的时钟还没有校准，现在不知道准确时间" },
  { "nyabula_read", NULL, "现在几点了", "{\"clock_valid\":true}", 480, false,
    "设备的时钟还没有校准，现在不知道准确时间" },

  /* FIXED: a time no calendar holds was cast out of range (undefined
   * behaviour); it now reads as a clock that is not set.
   */

  { "nyabula_read", NULL, "现在几点了",
    "{\"unix_ms\":1e300,\"clock_valid\":true}", 480, false,
    "设备的时钟还没有校准，现在不知道准确时间" },

  /* Weather */

  { "nyabula_read", NULL, "今天天气怎么样",
    "{\"temperature\":{\"value\":23.5},\"location\":{\"name\":\"深圳\"},"
    "\"condition\":{\"text\":\"多云\"},\"last_error\":0}",
    480, false, "深圳 现在多云，气温 23.5°C" },
  { "nyabula_read", NULL, "今天天气怎么样",
    "{\"temperature\":{\"value\":-3},\"location\":{\"name\":\"哈尔滨\"},"
    "\"condition\":{\"text\":\"小雪\"},\"last_error\":-110}",
    480, false, "哈尔滨 现在小雪，气温 -3.0°C（上次刷新失败，数据可能过时）" },
  { "nyabula_read", NULL, "今天天气怎么样", "{\"configured\":false}", 480,
    false, "还没有拿到天气数据（天气服务没有配置，或者没有联网）" },

  /* Device status */

  { "nyabula_read", NULL, "设备状态",
    "{\"uptimeMs\":3723000,\"memory\":{\"freeBytes\":104857600},"
    "\"cpu\":{\"percent\":12},\"network\":{\"interfaces\":["
    "{\"ipv4\":\"127.0.0.1\"},{\"ipv4\":\"192.168.1.5\",\"ssid\":\"Home\"}]}}",
    480, false,
    "设备已运行 1 小时 2 分钟 3 秒，空闲内存 100 MB，CPU 占用 12%，"
    "网络 Home 192.168.1.5" },

  /* FIXED: a status without uptimeMs cast NaN to an integer (undefined
   * behaviour); it now reads as 0.
   */

  { "nyabula_read", NULL, "设备状态", "{}", 480, false, "设备已运行 0 秒" },

  /* Lists */

  { "nyabula_read", NULL, "有哪些计时器",
    "{\"revision\":3,\"items\":["
    "{\"label\":\"关火\",\"status\":\"running\",\"remaining_ms\":270000},"
    "{\"label\":\"\",\"status\":\"paused\",\"remaining_ms\":60000},"
    "{\"label\":\"面\",\"status\":\"finished\",\"remaining_ms\":0}]}",
    480, false,
    "现在有 3 个计时器：关火 剩余 4 分钟 30 秒；计时器 已暂停，剩余 1 分钟；"
    "面 已经结束" },
  { "nyabula_read", NULL, "有哪些计时器", "{\"revision\":3,\"items\":[]}", 480,
    false, "现在没有计时器" },
  { "nyabula_read", NULL, "有几个闹钟",
    "{\"items\":[{\"time\":\"07:30\",\"label\":\"起床\",\"enabled\":true},"
    "{\"time\":\"22:00\",\"label\":\"\",\"enabled\":false}]}",
    480, false, "现在有 2 个闹钟：07:30 起床；22:00（已关闭）" },
  { "nyabula_read", NULL, "有几个闹钟", "{\"items\":[]}", 480, false,
    "现在没有闹钟" },
  { "nyabula_read", NULL, "我有哪些待办",
    "{\"items\":[{\"title\":\"买牛奶\",\"state\":\"queued\"},"
    "{\"title\":\"旧的\",\"state\":\"done\"},"
    "{\"title\":\"不做了\",\"state\":\"cancelled\"}]}",
    480, false, "现在有 1 个待办：买牛奶" },
  { "nyabula_read", NULL, "我有哪些待办",
    "{\"items\":[{\"title\":\"一\",\"state\":\"queued\"},"
    "{\"title\":\"二\",\"state\":\"queued\"},"
    "{\"title\":\"三\",\"state\":\"running\"},"
    "{\"title\":\"四\",\"state\":\"queued\"},"
    "{\"title\":\"五\",\"state\":\"queued\"},"
    "{\"title\":\"六\",\"state\":\"queued\"}]}",
    480, false, "现在有 6 个待办：一；二；三；四；还有 2 个没有列出" },
  { "nyabula_read", NULL, "我有哪些待办",
    "{\"items\":[{\"title\":\"旧的\",\"state\":\"done\"}]}", 480, false,
    "现在没有待办" },

  /* Creates: the sentence is the request, read back from its arguments. */

  { "nyabula_action", NULL, "定个五分钟的计时器",
    "{\"revision\":8,\"items\":[]}", 480, false, "已经创建 5 分钟的计时器" },
  { "nyabula_action", NULL, "十分钟后提醒我关火", "{\"revision\":8}", 480,
    false, "已经创建 10 分钟的计时器（关火）" },
  { "nyabula_action", NULL, "计时一个半小时", "{}", 480, false,
    "已经创建 1 小时 30 分钟的计时器" },
  { "nyabula_action", NULL, "计时三分半", "{}", 480, false,
    "已经创建 3 分钟 30 秒的计时器" },
  { "nyabula_action", NULL, "计时90秒", "{}", 480, false,
    "已经创建 1 分钟 30 秒的计时器" },
  { "nyabula_action", NULL, "明早七点半叫我起床", "{\"revision\":2}", 480,
    false, "已经创建 07:30 的闹钟（起床）" },
  { "nyabula_action", NULL, "每天早上七点叫我", "{}", 480, false,
    "已经创建 07:00 的每天重复闹钟" },
  { "nyabula_action", NULL, "工作日早上八点的闹钟", "{}", 480, false,
    "已经创建 08:00 的按周重复闹钟" },
  { "nyabula_action", NULL, "记住我喜欢吃草莓", "{}", 480, false,
    "已经记住“我喜欢吃草莓”" },
  { "nyabula_action", NULL, "待办加一条：周五交报告", "{}", 480, false,
    "已经添加待办“周五交报告”" },

  /* Music and the face */

  { "nyabula_music", NULL, "音量调到30", "{\"volume\":30}", 480, false,
    "已经把音量调到 30%" },
  { "nyabula_music", NULL, "静音", "{\"muted\":true}", 480, false,
    "已经静音" },
  { "nyabula_music", NULL, "取消静音", "{}", 480, false,
    "已经把音量调到 50%" },
  { "nyabula_music", NULL, "放音乐", "{\"track\":\"Sunny Day.mp3\"}", 480,
    false, "正在播放“Sunny Day.mp3”" },
  { "nyabula_music", NULL, "放音乐", "{\"state\":\"playing\"}", 480, false,
    "正在播放“song.mp3”" },
  { "nyabula_music", NULL, "暂停音乐", "{}", 480, false, "已经暂停播放" },
  { "nyabula_music", NULL, "继续播放", "{}", 480, false, "已经继续播放" },
  { "nyabula_music", NULL, "停止播放", "{}", 480, false, "已经停止播放" },
  { "nyabula_expression", NULL, "换个开心的表情", "{\"ok\":true}", 480, false,
    "已经把表情换成“开心”" },
  { "nyabula_expression", NULL, "做个晕的表情", "{}", 480, false,
    "已经把表情换成“晕乎乎”" },
  { "nyabula_expression", "{\"expression\":\"wink\"}", NULL, "{}", 480, false,
    "已经把表情换成“wink”" },

  /* Failures: the executor's envelope, by error code */

  { "nyabula_action", NULL, "定个五分钟的计时器",
    "{\"ok\":false,\"error\":-13}", 480, true,
    "创建 5 分钟的计时器没有成功：主人没有批准，所以没有执行" },
  { "nyabula_action", NULL, "明早七点半叫我起床",
    "{\"ok\":false,\"error\":-110,\"message\":\"approval timed out\"}", 480,
    true,
    "创建 07:30 的闹钟（起床）没有成功：等主人批准等到超时了，没有执行" },
  { "nyabula_action", NULL, "记住我喜欢吃草莓",
    "{\"ok\":false,\"error\":-116}", 480, true,
    "记住“我喜欢吃草莓”没有成功：数据刚刚在别处被改过，这次没有写入，"
    "需要再说一次" },
  { "nyabula_action", NULL, "待办加一条：周五交报告",
    "{\"ok\":false,\"error\":-28}", 480, true,
    "添加待办“周五交报告”没有成功：数量已经到上限，没有创建" },
  { "nyabula_music", NULL, "音量调到30", "{\"ok\":false,\"error\":-16}", 480,
    true, "把音量调到 30%没有成功：设备正忙，没有执行" },
  { "nyabula_music", NULL, "放音乐", "{\"ok\":false,\"error\":-2}", 480, true,
    "播放“song.mp3”没有成功：没有找到需要的数据" },
  { "nyabula_expression", NULL, "换个开心的表情",
    "{\"ok\":false,\"error\":-38}", 480, true,
    "把表情换成“开心”没有成功：这台设备没有启用这个功能" },
  { "nyabula_read", NULL, "今天天气怎么样", "{\"ok\":false,\"error\":-125}",
    480, true, "读取 weather.get没有成功：这次操作被取消了，没有执行" },
  { "nyabula_action", NULL, "定个五分钟的计时器",
    "{\"ok\":false,\"error\":-71}", 480, true,
    "创建 5 分钟的计时器没有成功：设备返回了错误码 -71" },
  { "nyabula_action", NULL, "定个五分钟的计时器", "{\"ok\":false}", 480, true,
    "创建 5 分钟的计时器没有成功：设备返回了错误码 -5" },

  /* FIXED: an error code outside int was cast out of range (undefined
   * behaviour); it now reads as -EIO like a missing one.
   */

  { "nyabula_action", NULL, "定个五分钟的计时器",
    "{\"ok\":false,\"error\":-1e300}", 480, true,
    "创建 5 分钟的计时器没有成功：设备返回了错误码 -5" },
  { "nyabula_action", NULL, "定个五分钟的计时器",
    "{\"ok\":false,\"error\":-13,\"sideEffectUncertain\":true}", 480, true,
    "创建 5 分钟的计时器：结果不确定，需要主人自己确认一下" },

  /* The write landed and only the read-back failed: that happened. */

  { "nyabula_action", NULL, "定个五分钟的计时器",
    "{\"ok\":false,\"error\":-5,\"sideEffectApplied\":true}", 480, false,
    "已经创建 5 分钟的计时器" },

  /* Not ours to paraphrase, or not understood: the first bytes as they
   * came.  DEBATABLE: a result that is not the failure envelope is never
   * reported as a failure, whatever its text says.
   */

  { "nyabula_action",
    "{\"topic\":\"agent.mcp.out.call\",\"arguments\":{\"name\":\"x\"}}", NULL,
    "{\"content\":\"42\"}", 480, false,
    "工具 nyabula_action 返回：{\"content\":\"42\"}" },
  { "get_stock_price", "{\"symbol\":\"X\"}", NULL, "{\"price\":12.5}", 480,
    false, "工具 get_stock_price 返回：{\"price\":12.5}" },
  { "nyabula_read", NULL, "现在几点了", "error: boom", 480, false,
    "工具 nyabula_read 返回：error: boom" },
  { "nyabula_read", "{\"topic\":\"agent.profile.get\"}", NULL,
    "{\"name\":\"Nya\"}", 480, false,
    "工具 nyabula_read 返回：{\"name\":\"Nya\"}" },
  { "nyabula_read", "not json", NULL, "[1,2,3]", 480, false,
    "工具 nyabula_read 返回：[1,2,3]" },
  { NULL, NULL, NULL, NULL, 480, false, "工具  返回：" },
  { "nyabula_action", NULL, NULL, "", 480, false,
    "工具 nyabula_action 返回：" }
};

static void test_facts(void)
{
  char fact[NY_AGENT_LOCAL_FACT_MAX];

  /* The table spells error codes as numbers, the way they arrive in JSON;
   * these are the Linux values, which NuttX shares.
   */

  CHECK(EACCES == 13 && ETIMEDOUT == 110 && ESTALE == 116 && ENOSPC == 28 &&
            EBUSY == 16 && ENOENT == 2 && ENOSYS == 38 && ECANCELED == 125 &&
            EIO == 5,
        "this host numbers errno differently; the fact table needs a port");
  for (size_t i = 0; i < COUNT(g_fact_cases); i++)
    {
      const struct fact_case_s *c = &g_fact_cases[i];
      const char *tool = NULL;
      char *rendered = NULL;
      if (c->utterance)
        {
          struct ny_agent_local_intent_s intent;
          detect(c->utterance, &intent);
          rendered = ny_agent_local_intent_call(&intent, &g_context, &tool);
          CHECK(rendered != NULL && !strcmp(tool, c->tool),
                "fact %zu: [%s] renders for %s", i, c->utterance,
                tool ? tool : "(null)");
        }
      memset(fact, 'Z', sizeof(fact));
      bool failed = ny_agent_local_intent_fact(
          c->tool, rendered ? rendered : c->arguments, c->result,
          c->utc_offset_minutes, fact, sizeof(fact));
      CHECK(!strcmp(fact, c->fact), "fact %zu: [%s], expected [%s]", i, fact,
            c->fact);
      CHECK(failed == c->failed, "fact %zu: [%s] failed=%d", i, fact, failed);

      /* What the transport prefixes with "抱歉，" has to say why. */

      if (failed)
        CHECK(strstr(fact, "没有成功") || strstr(fact, "不确定"),
              "fact %zu: a failure that does not say so: [%s]", i, fact);
      free(rendered);
    }

  /* Something no rule understands is passed on cut to 300 bytes, between
   * characters: 2 + 99 * 3 = 299.
   */

  char result[700];
  char expected[700];
  snprintf(result, sizeof(result), "ab");
  snprintf(expected, sizeof(expected), "工具 other 返回：ab");
  for (int i = 0; i < 200; i++)
    {
      strcat(result, "猫");
      if (i < 99)
        strcat(expected, "猫");
    }
  bool failed =
      ny_agent_local_intent_fact("other", "{}", result, 0, fact, sizeof(fact));
  CHECK(!failed && !strcmp(fact, expected) && valid(fact),
        "long unknown result: %zu bytes, expected %zu", strlen(fact),
        strlen(expected));

  /* A long label inside a fact is cut whole as well. */

  char arguments[1200];
  snprintf(arguments, sizeof(arguments),
           "{\"topic\":\"memory.create\",\"arguments\":{\"revision\":1,"
           "\"record\":{\"text\":\"");
  for (int i = 0; i < 300; i++)
    strcat(arguments, "喵");
  strcat(arguments, "\"}}}");
  ny_agent_local_intent_fact("nyabula_action", arguments, "{}", 0, fact,
                             sizeof(fact));
  CHECK(valid(fact) && !strncmp(fact, "已经记住“喵", 16) &&
            strlen(fact) == strlen("已经记住“”") + 300,
        "long memory in a fact: %zu bytes", strlen(fact));

  /* FIXED: a call or a result written by somebody else (a cloud backend, an
   * MCP tool) may put text of any length anywhere.  "%.48s" and friends
   * count bytes and cut inside a character; the fact then was not UTF-8 and
   * so was not valid JSON content either.
   */

  static const char *const hostile[][3] = {
    { "nyabula_read",
      "{\"topic\":\"a猫猫猫猫猫猫猫猫猫猫猫猫猫猫猫猫猫猫猫\"}",
      "{\"ok\":false,\"error\":-5}" },
    { "x自定义工具自定义工具自定义工具自定义工具", "{\"topic\":\"x\"}",
      "{\"ok\":false,\"error\":-5}" },
    { "自定义工具自定义工具自定义工具自定义工具", "{}", "plain" },
    { "nyabula_read", "{\"topic\":\"alarm.list\"}",
      "{\"items\":[{\"time\":\"猫猫猫\",\"label\":\"\",\"enabled\":true}]}" },
    { "nyabula_read", "{\"topic\":\"device.status\"}",
      "{\"uptimeMs\":1000,\"network\":{\"interfaces\":[{\"ipv4\":"
      "\"猫猫猫猫猫猫猫猫猫\",\"ssid\":\"x\"}]}}" }
  };
  for (size_t i = 0; i < COUNT(hostile); i++)
    {
      ny_agent_local_intent_fact(hostile[i][0], hostile[i][1], hostile[i][2],
                                 0, fact, sizeof(fact));
      CHECK(valid(fact), "hostile %zu: fact is not UTF-8: [%s]", i, fact);
    }
  snprintf(arguments, sizeof(arguments), "{\"expression\":\"a");
  for (int i = 0; i < 300; i++)
    strcat(arguments, "喵");
  strcat(arguments, "\"}");
  ny_agent_local_intent_fact("nyabula_expression", arguments, "{}", 0, fact,
                             sizeof(fact));
  CHECK(valid(fact), "long expression name: fact is not UTF-8");
  snprintf(arguments, sizeof(arguments),
           "{\"topic\":\"alarm.create\",\"arguments\":{\"record\":{\"time\":"
           "\"ab");
  for (int i = 0; i < 300; i++)
    strcat(arguments, "喵");
  strcat(arguments, "\"}}}");
  ny_agent_local_intent_fact("nyabula_action", arguments, "{}", 0, fact,
                             sizeof(fact));
  CHECK(valid(fact), "long alarm time: fact is not UTF-8");

  /* A small buffer is respected. */

  for (size_t size = 1; size <= 40; size++)
    {
      memset(fact, 'Z', sizeof(fact));
      ny_agent_local_intent_fact("nyabula_read", "{\"topic\":\"timer.list\"}",
                                 "{\"items\":[]}", 0, fact, size);
      CHECK(strlen(fact) < size && fact[size] == 'Z',
            "fact wrote past a %zu-byte buffer", size);
    }
}

/****************************************************************************
 * Fuzzing
 ****************************************************************************/

static uint64_t g_random = 88172645463325252ULL;

static uint32_t rnd(void)
{
  g_random ^= g_random << 13;
  g_random ^= g_random >> 7;
  g_random ^= g_random << 17;
  return (uint32_t)(g_random >> 16);
}

static const char *const g_fuzz_words[] = { "计时",
                                            "计时器",
                                            "倒计时",
                                            "闹钟",
                                            "分钟",
                                            "分",
                                            "小时",
                                            "秒",
                                            "半",
                                            "个",
                                            "点",
                                            "一刻",
                                            "三刻",
                                            "刻钟",
                                            "整",
                                            "十",
                                            "百",
                                            "两",
                                            "零",
                                            "一",
                                            "二",
                                            "九",
                                            "百分之",
                                            "百分百",
                                            "音量",
                                            "声音",
                                            "调到",
                                            "大一点",
                                            "小声点",
                                            "静音",
                                            "提醒我",
                                            "叫我",
                                            "叫醒",
                                            "待办",
                                            "待办事项",
                                            "记住",
                                            "帮我记",
                                            "记",
                                            "：",
                                            "，",
                                            "。",
                                            "“",
                                            "”",
                                            "后",
                                            "之后",
                                            "过",
                                            "明早",
                                            "晚上",
                                            "下午",
                                            "凌晨",
                                            "中午",
                                            "半夜",
                                            "每天",
                                            "工作日",
                                            "周末",
                                            "下周",
                                            "号",
                                            "月",
                                            "表情",
                                            "眼睛",
                                            "换",
                                            "开心",
                                            "星星眼",
                                            "播放",
                                            "放首",
                                            "暂停",
                                            "继续播放",
                                            "停止播放",
                                            "加到",
                                            "列表",
                                            "昨天",
                                            "他说",
                                            "你觉得",
                                            "怎么样",
                                            "几点了",
                                            "天气",
                                            "am",
                                            "pm",
                                            "a.m.",
                                            "p.m",
                                            "o'clock",
                                            ":",
                                            "：",
                                            "%",
                                            "％",
                                            "timer",
                                            "alarm",
                                            "volume",
                                            "remind me",
                                            "in ",
                                            " later",
                                            "todo",
                                            "to my",
                                            "list",
                                            "what",
                                            "remember ",
                                            "play ",
                                            "set",
                                            "up",
                                            "down",
                                            "by",
                                            "minutes",
                                            "hour",
                                            "sec",
                                            " ",
                                            "0",
                                            "5",
                                            "12",
                                            "24",
                                            "59",
                                            "60",
                                            "100",
                                            "999",
                                            "999999",
                                            "9999999",
                                            "1.5",
                                            "0.0001",
                                            ".",
                                            "😀",
                                            "é",
                                            "\xef\xbb\xbf",
                                            "\xe2\x80\x8b" };

static const char *const g_fuzz_bad[] = {
  "\xff",         "\xc0\x80",         "\xe4\xb8", "\xf0\x9f\x98", "\x80",
  "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe8",     "\x01",         "\x7f"
};

static const char *const g_fuzz_tools[] = {
  "nyabula_read",       "nyabula_action", "nyabula_music",
  "nyabula_expression", "other",          ""
};

static const char *const g_fuzz_keys[] = { "ok",
                                           "error",
                                           "sideEffectApplied",
                                           "sideEffectUncertain",
                                           "items",
                                           "topic",
                                           "arguments",
                                           "record",
                                           "label",
                                           "time",
                                           "repeat",
                                           "text",
                                           "title",
                                           "name",
                                           "volume",
                                           "muted",
                                           "duration_ms",
                                           "expression",
                                           "unix_ms",
                                           "clock_valid",
                                           "temperature",
                                           "value",
                                           "location",
                                           "condition",
                                           "last_error",
                                           "uptimeMs",
                                           "memory",
                                           "freeBytes",
                                           "cpu",
                                           "percent",
                                           "network",
                                           "interfaces",
                                           "ipv4",
                                           "ssid",
                                           "state",
                                           "status",
                                           "remaining_ms",
                                           "enabled",
                                           "track",
                                           "supported",
                                           "minutes",
                                           "revision" };

static const char *const g_fuzz_values[] = {
  "system.time.get",  "weather.get",  "device.status", "timer.list",
  "alarm.list",       "task.list",    "timer.create",  "alarm.create",
  "memory.create",    "task.create",  "music.play",    "music.volume",
  "music.pause",      "music.resume", "music.stop",    "music.output",
  "agent.tools.call", "done",         "cancelled",     "finished",
  "paused",           "running",      "07:30",         "25:99",
  "127.0.0.1",        "192.168.1.5",  "happy",         "anger",
  "sleepy",           "关火",         "起床😀",         ""
};

static const double g_fuzz_numbers[] = { 0,
                                         1,
                                         -1,
                                         0.5,
                                         5,
                                         13,
                                         -13,
                                         -110,
                                         100,
                                         101,
                                         1440,
                                         1441,
                                         1e9,
                                         -1e9,
                                         1e19,
                                         -1e19,
                                         1e300,
                                         -1e300,
                                         4e9,
                                         -5e9,
                                         1789819200000.0,
                                         -62135596800000.0,
                                         253402300800000.0,
                                         9e15,
                                         2147483648.0,
                                         -2147483649.0,
                                         0.0001,
                                         86400000,
                                         3723000 };

/* One random piece of text into out[used..], valid UTF-8 unless `bad`. */

static size_t fuzz_piece(char *out, size_t used, size_t capacity, bool bad)
{
  char number[32];
  const char *piece;
  uint32_t pick = rnd() % 100;
  if (bad && pick < 12)
    piece = g_fuzz_bad[rnd() % COUNT(g_fuzz_bad)];
  else if (pick < 45)
    piece = g_fuzz_words[rnd() % COUNT(g_fuzz_words)];
  else if (pick < 60)
    piece = g_kind_cases[rnd() % COUNT(g_kind_cases)].utterance;
  else if (pick < 70)
    piece = g_timer_cases[rnd() % COUNT(g_timer_cases)].utterance;
  else if (pick < 80)
    piece = g_alarm_cases[rnd() % COUNT(g_alarm_cases)].utterance;
  else if (pick < 86)
    piece = g_volume_cases[rnd() % COUNT(g_volume_cases)].utterance;
  else if (pick < 92)
    piece = g_text_cases[rnd() % COUNT(g_text_cases)].utterance;
  else if (pick < 96)
    piece = g_expression_cases[rnd() % COUNT(g_expression_cases)].utterance;
  else
    {
      snprintf(number, sizeof(number), "%u",
               rnd() % (rnd() % 2 ? 100 : 100000));
      piece = number;
    }
  size_t length = strlen(piece);
  if (used + length >= capacity)
    return used;
  memcpy(out + used, piece, length);
  return used + length;
}

static size_t fuzz_text(char *out, size_t capacity)
{
  size_t used = 0;
  uint32_t mode = rnd() % 10;
  if (mode == 0)
    {
      /* Noise */

      used = rnd() % (capacity - 1);
      for (size_t i = 0; i < used; i++)
        out[i] = (char)(rnd() % 255 + 1);
    }
  else
    {
      uint32_t pieces = 1 + rnd() % (mode == 1 ? 60 : 7);
      for (uint32_t i = 0; i < pieces; i++)
        used = fuzz_piece(out, used, capacity, mode >= 7);
      if (mode == 9 && used > 0)
        for (uint32_t i = 0; i <= rnd() % 4; i++)
          {
            /* Flip, drop the tail, or cut a hole. */

            size_t at = rnd() % used;
            uint32_t how = rnd() % 3;
            if (how == 0)
              out[at] = (char)(rnd() % 255 + 1);
            else if (how == 1)
              used = at + 1;
            else
              {
                size_t hole = rnd() % (used - at);
                memmove(out + at, out + at + hole, used - at - hole);
                used -= hole;
              }
          }
    }
  out[used] = 0;
  return used;
}

static cJSON *fuzz_json(int depth)
{
  char text[256];
  uint32_t pick = rnd() % (depth > 3 ? 6 : 10);
  if (pick == 0)
    return cJSON_CreateNumber(g_fuzz_numbers[rnd() % COUNT(g_fuzz_numbers)]);
  if (pick == 1)
    return cJSON_CreateNumber((double)(rnd() % 200) - 50);
  if (pick == 2)
    return cJSON_CreateBool(rnd() % 2);
  if (pick == 3)
    return cJSON_CreateString(g_fuzz_values[rnd() % COUNT(g_fuzz_values)]);
  if (pick == 4)
    {
      fuzz_text(text, rnd() % 4 ? 48 : sizeof(text));
      if (!valid(text))
        text[0] = 0;
      return cJSON_CreateString(text);
    }
  if (pick == 5)
    return cJSON_CreateNull();
  if (pick == 6)
    {
      cJSON *array = cJSON_CreateArray();
      for (uint32_t i = rnd() % 7; array && i > 0; i--)
        cJSON_AddItemToArray(array, fuzz_json(depth + 1));
      return array;
    }
  cJSON *object = cJSON_CreateObject();
  for (uint32_t i = rnd() % 8; object && i > 0; i--)
    {
      const char *key = g_fuzz_keys[rnd() % COUNT(g_fuzz_keys)];
      if (!cJSON_GetObjectItemCaseSensitive(object, key))
        cJSON_AddItemToObject(object, key, fuzz_json(depth + 1));
    }
  return object;
}

/* Exactly-sized heap copies, so that one byte too far is an ASan report. */

static char *fuzz_exact(const char *text)
{
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (!copy)
    abort();
  memcpy(copy, text, length + 1);
  return copy;
}

static char *fuzz_document(void)
{
  cJSON *tree = fuzz_json(rnd() % 3 ? 0 : 2);
  char *printed = tree ? cJSON_PrintUnformatted(tree) : NULL;
  char *exact;
  cJSON_Delete(tree);
  if (!printed)
    return fuzz_exact("{}");
  if (rnd() % 8 == 0 && printed[0])
    printed[rnd() % strlen(printed)] = (char)(rnd() % 255 + 1);
  exact = fuzz_exact(printed);
  free(printed);
  return exact;
}

#define FUZZ_REQUIRE(condition, what)                           \
  do                                                            \
    {                                                           \
      if (!(condition))                                         \
        {                                                       \
          printf("FUZZ FAIL %s\n  input: [%s]\n", what, shown); \
          g_failures++;                                         \
        }                                                       \
    }                                                           \
  while (0)

static void fuzz(unsigned long iterations, uint64_t seed)
{
  unsigned long commands = 0;
  unsigned long calls = 0;
  unsigned long facts = 0;
  unsigned long fills = 0;
  unsigned long done = 0;
  g_random = seed ? seed : 1;
  for (; done < iterations && g_failures < 20; done++)
    {
      char text[700];
      struct ny_agent_local_context_s context = {
        (double)(rnd() % 1000),  (int)(rnd() % 1681) - 840,
        (int)(rnd() % 1442) - 1, (int)(rnd() % 140) - 20,
        rnd() % 2 != 0,          rnd() % 5 ? "曲子.mp3" : NULL
      };
      struct ny_agent_local_intent_s intent;
      const char *tool = NULL;
      const char *name = NULL;
      fuzz_text(text, sizeof(text));
      char *utterance = fuzz_exact(text);
      const char *shown = utterance;
      bool input_valid = valid(utterance);
      ny_agent_local_intent_detect(utterance, &intent);
      FUZZ_REQUIRE((int)intent.kind >= 0 && (int)intent.kind < KINDS, "kind");
      FUZZ_REQUIRE(strlen(intent.text) < sizeof(intent.text), "text length");
      FUZZ_REQUIRE(valid(intent.text), "text is not UTF-8");
      FUZZ_REQUIRE(input_valid || intent.kind == NY_AGENT_LOCAL_CHAT,
                   "invalid UTF-8 became a command");
      FUZZ_REQUIRE(strlen(utterance) < NY_AGENT_LOCAL_UTTERANCE_MAX ||
                       intent.kind == NY_AGENT_LOCAL_CHAT,
                   "an over-long utterance became a command");
      FUZZ_REQUIRE(intent.kind != NY_AGENT_LOCAL_CHAT || !intent.complete,
                   "complete chat");
      if (intent.complete)
        {
          FUZZ_REQUIRE(intent.kind != NY_AGENT_LOCAL_TIMER ||
                           (intent.seconds >= 1 && intent.seconds <= 86400),
                       "timer out of range");
          FUZZ_REQUIRE(intent.kind != NY_AGENT_LOCAL_ALARM ||
                           (intent.hour >= 0 && intent.hour <= 23 &&
                            intent.minute >= 0 && intent.minute <= 59),
                       "alarm out of range");
          FUZZ_REQUIRE(intent.kind != NY_AGENT_LOCAL_VOLUME_SET ||
                           (intent.percent >= 0 && intent.percent <= 100),
                       "volume out of range");
          FUZZ_REQUIRE((intent.kind != NY_AGENT_LOCAL_VOLUME_UP &&
                        intent.kind != NY_AGENT_LOCAL_VOLUME_DOWN) ||
                           (intent.percent >= 1 && intent.percent <= 100),
                       "volume step out of range");
          FUZZ_REQUIRE(intent.kind != NY_AGENT_LOCAL_EXPRESSION ||
                           intent.expression != NULL,
                       "complete expression without a name");
        }
      if (intent.kind != NY_AGENT_LOCAL_CHAT)
        commands++;

      /* The forced call and a made-up answer to it */

      const char *table = ny_agent_local_intent_forced(&intent, &name);
      FUZZ_REQUIRE(table == NULL || name != NULL, "forced without a name");
      if (rnd() % 4 == 0)
        {
          struct ny_agent_local_intent_s filled = intent;
          char *document = fuzz_document();
          cJSON *arguments = cJSON_Parse(document);
          int result = ny_agent_local_intent_fill(
              &filled, rnd() % 3 ? name : "set_timer", arguments);
          FUZZ_REQUIRE(result == 0 || result == -EINVAL, "fill result");
          FUZZ_REQUIRE(result != 0 || filled.complete, "fill incomplete");
          if (result == 0)
            intent = filled;
          cJSON_Delete(arguments);
          free(document);
          fills++;
        }

      /* The call, the reply that carries it, and a fact about it */

      char *call = ny_agent_local_intent_call(&intent, &context, &tool);
      FUZZ_REQUIRE(tool != NULL, "call left the tool unset");
      if (call)
        {
          cJSON *parsed = cJSON_Parse(call);
          FUZZ_REQUIRE(cJSON_IsObject(parsed), "call is not a JSON object");
          FUZZ_REQUIRE(valid(call), "call is not UTF-8");
          FUZZ_REQUIRE(strlen(call) < 2048, "call exceeds the executor input");
          /* The transport renders complete intents only; an incomplete one
           * gets this far for the crash check and nothing more.
           */

          if (intent.complete && !strcmp(tool, "nyabula_music") && parsed &&
              !strcmp(text_of(parsed, "topic"), "music.volume"))
            {
              double volume = number_of(
                  cJSON_GetObjectItemCaseSensitive(parsed, "arguments"),
                  "volume");
              int low = context.volume < 0 ? context.volume : 0;
              int high = context.volume > 100 ? context.volume : 100;
              FUZZ_REQUIRE(volume >= low && volume <= high,
                           "volume left the range");
            }
          cJSON_Delete(parsed);
          calls++;
        }
      char *reply = ny_agent_local_intent_reply(
          call ? tool : NULL, call, call ? NULL : utterance,
          ((uint64_t)rnd() << 32) | rnd());
      if (input_valid || call)
        {
          cJSON *parsed = reply ? cJSON_Parse(reply) : NULL;
          FUZZ_REQUIRE(cJSON_IsObject(parsed), "reply is not a JSON object");
          cJSON_Delete(parsed);
        }
      free(reply);

      /* Facts: about this call with a made-up result, or made up whole, in
       * a buffer of exactly the size that is announced.
       */

      for (int round = 0; round < 2; round++)
        {
          size_t size = rnd() % 3 ? NY_AGENT_LOCAL_FACT_MAX
                                  : 1 + rnd() % NY_AGENT_LOCAL_FACT_MAX;
          char *arguments =
              round == 0 && call ? fuzz_exact(call) : fuzz_document();
          char *result = rnd() % 10 ? fuzz_document() : fuzz_exact(text);
          const char *used = round == 0 && call
                                 ? tool
                                 : g_fuzz_tools[rnd() % COUNT(g_fuzz_tools)];
          char *fact = malloc(size);
          if (!fact)
            abort();
          memset(fact, 'Z', size);
          ny_agent_local_intent_fact(rnd() % 50 ? used : NULL,
                                     rnd() % 50 ? arguments : NULL,
                                     rnd() % 50 ? result : NULL,
                                     (int)(rnd() % 1681) - 840, fact, size);
          shown = result;
          FUZZ_REQUIRE(memchr(fact, 0, size) != NULL, "fact not terminated");
          FUZZ_REQUIRE(size < 8 || fact[0] != 0, "empty fact");

          /* Valid text in, valid text out, when the buffer is the size the
           * transport really passes.
           */

          FUZZ_REQUIRE(size != NY_AGENT_LOCAL_FACT_MAX || !valid(arguments) ||
                           !valid(result) || valid(fact),
                       "fact is not UTF-8");
          if (size == NY_AGENT_LOCAL_FACT_MAX && valid(arguments) &&
              valid(result) && !valid(fact))
            printf("  arguments: [%s]\n  fact: [%s]\n", arguments, fact);
          shown = utterance;
          free(fact);
          free(arguments);
          free(result);
          facts++;
        }

      /* The library lookup with the hint as it was heard */

      if (rnd() % 16 == 0)
        {
          char *document = fuzz_document();
          cJSON *library = cJSON_Parse(document);
          char *file = malloc(24);
          if (!file)
            abort();
          int result =
              ny_agent_local_intent_track(library, intent.text, file, 24);
          FUZZ_REQUIRE(result == 0 || result == -ENOENT || result == -ESRCH,
                       "track result");
          FUZZ_REQUIRE(result != 0 || strlen(file) < 24, "track name length");
          cJSON_Delete(library);
          free(document);
          free(file);
        }
      free(call);
      free(utterance);
    }
  printf("fuzz: %lu inputs (seed %llu), %lu commands, %lu calls, %lu fills, "
         "%lu facts, %lu failures\n",
         done, (unsigned long long)seed, commands, calls, fills, facts,
         g_failures);
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, char **argv)
{
  if (argc >= 2 && !strcmp(argv[1], "fuzz"))
    {
      fuzz(argc >= 3 ? strtoul(argv[2], NULL, 10) : 200000,
           argc >= 4 ? strtoull(argv[3], NULL, 10) : 1);
      return g_failures ? 1 : 0;
    }
  test_names();
  test_kinds();
  test_timers();
  test_alarms();
  test_volume();
  test_expressions();
  test_texts();
  test_lengths();
  test_music();
  test_reads();
  test_fill();
  test_reply();
  test_facts();
  for (int kind = 0; kind < KINDS; kind++)
    CHECK(g_seen[kind] >= 3, "only %lu utterances reach the kind %s",
          g_seen[kind],
          ny_agent_local_intent_name((enum ny_agent_local_intent_e)kind));
  size_t rows = COUNT(g_kind_cases) + COUNT(g_timer_cases) +
                COUNT(g_alarm_cases) + COUNT(g_resolve_cases) +
                COUNT(g_volume_cases) + COUNT(g_expression_cases) +
                COUNT(g_expression_misses) + COUNT(g_text_cases) +
                COUNT(g_music_cases);
  CHECK(rows >= 120, "only %zu utterance rows", rows);
  printf("%s: %zu utterance rows (%lu detections with the fill, fact and "
         "length cases), %lu checks, %lu failures\n",
         g_failures ? "FAIL" : "PASS", rows, g_utterances, g_checks,
         g_failures);
  return g_failures ? 1 : 0;
}
