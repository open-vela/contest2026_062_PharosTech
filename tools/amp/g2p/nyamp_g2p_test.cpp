/****************************************************************************
 * tools/amp/g2p/nyamp_g2p_test.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* usage: nyamp_g2p_test [<assets-dir> [<golden-dir>]]
 *
 * The model assets are third-party files that are not committed, so they
 * come from argv[1] or NYAMP_G2P_ASSETS.  Without them only the asset-free
 * normaliser tests run and the process exits with 77, which CTest reports
 * as "skipped" instead of a false "passed".
 *
 * <golden-dir> (or NYAMP_G2P_GOLDEN) holds X.BIN / TONES.BIN as written by
 * real_fixture.py.  The same ids are embedded below, so the golden check
 * runs even when only the assets are available; the files, when given,
 * prove the embedded copy is not stale.
 */

#include "nyamp_g2p.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using nyamp::MeloFrontend;
using Ids = std::vector<std::int64_t>;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond) \
  do \
    { \
      g_checks++; \
      if (!(cond)) \
        { \
          g_failures++; \
          std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } \
  while (0)

void CheckEq(const std::string &got, const std::string &want, int line)
{
  g_checks++;
  if (got != want)
    {
      g_failures++;
      std::printf("FAIL line %d:\n  got : %s\n  want: %s\n", line,
                  got.c_str(), want.c_str());
    }
}

#define CHECK_NORM(input, want) \
  CheckEq(MeloFrontend::Normalize(input), want, __LINE__)

const char kGoldenText[] =
    "你好，我是星喵。忙了一天，辛苦啦。要不要休息一会儿？";

/* Decoded from real-fixture/X.BIN and TONES.BIN (little-endian int64). */

const std::int64_t kGoldenX[] =
{
  0, 62, 0, 40, 0, 37, 0, 16, 0, 106, 0, 97, 0, 65, 0, 77, 0, 52, 0, 98,
  0, 50, 0, 60, 0, 46, 0, 107, 0, 60, 0, 15, 0, 59, 0, 26, 0, 99, 0, 40,
  0, 78, 0, 44, 0, 106, 0, 98, 0, 49, 0, 57, 0, 82, 0, 59, 0, 8, 0, 107,
  0, 99, 0, 16, 0, 19, 0, 82, 0, 99, 0, 16, 0, 98, 0, 53, 0, 98, 0, 40,
  0, 99, 0, 40, 0, 37, 0, 89, 0, 3, 0, 32, 0, 104, 0,
};

const std::int64_t kGoldenTones[] =
{
  0, 3, 0, 3, 0, 3, 0, 3, 0, 0, 0, 3, 0, 3, 0, 4, 0, 4, 0, 1,
  0, 1, 0, 1, 0, 1, 0, 0, 0, 2, 0, 2, 0, 5, 0, 5, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 3, 0, 3, 0, 5, 0, 5, 0, 0,
  0, 4, 0, 4, 0, 2, 0, 2, 0, 4, 0, 4, 0, 1, 0, 1, 0, 1, 0, 1,
  0, 1, 0, 1, 0, 4, 0, 4, 0, 5, 0, 5, 0, 0, 0,
};

template <std::size_t N>
Ids ToIds(const std::int64_t (&array)[N])
{
  return Ids(array, array + N);
}

/* Returns the first differing index, or -1 when equal. */

long FirstDiff(const Ids &a, const Ids &b)
{
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();

  for (std::size_t i = 0; i < n; i++)
    {
      if (a[i] != b[i])
        {
          return static_cast<long>(i);
        }
    }

  return a.size() == b.size() ? -1 : static_cast<long>(n);
}

bool ReadInt64File(const std::string &path, Ids *out)
{
  std::ifstream in(path, std::ios::binary);
  unsigned char raw[8];

  if (!in)
    {
      return false;
    }

  out->clear();
  while (in.read(reinterpret_cast<char *>(raw), 8))
    {
      /* Assemble explicitly so the test is right on any host endianness. */

      std::uint64_t v = 0;
      for (int k = 7; k >= 0; k--)
        {
          v = (v << 8) | raw[k];
        }

      out->push_back(static_cast<std::int64_t>(v));
    }

  return in.gcount() == 0;
}

/* Symbols of all utterances back to back, blanks removed. */

void Flatten(const std::vector<MeloFrontend::Utterance> &utts, Ids *phonemes,
             Ids *tones)
{
  phonemes->clear();
  tones->clear();
  for (const auto &u : utts)
    {
      for (std::size_t i = 1; i < u.phonemes.size(); i += 2)
        {
          phonemes->push_back(u.phonemes[i]);
          tones->push_back(u.tones[i]);
        }
    }
}

/* The add_blank layout every utterance must obey. */

bool WellFormed(const MeloFrontend::Utterance &u)
{
  if (u.phonemes.size() != 2 * u.symbols + 1 ||
      u.tones.size() != u.phonemes.size() || u.symbols == 0)
    {
      return false;
    }

  for (std::size_t i = 0; i < u.phonemes.size(); i++)
    {
      if (i % 2 == 0 ? (u.phonemes[i] != 0 || u.tones[i] != 0)
                     : u.phonemes[i] == 0)
        {
          return false;
        }
    }

  return true;
}

long PeakRssKb()
{
  std::ifstream status("/proc/self/status");
  std::string line;

  while (std::getline(status, line))
    {
      if (line.compare(0, 6, "VmHWM:") == 0)
        {
          return std::atol(line.c_str() + 6);
        }
    }

  return -1; /* Not Linux: the memory check is skipped. */
}

/****************************************************************************
 * Asset-free tests
 ****************************************************************************/

void TestNormalize()
{
  CHECK_NORM("", "");
  CHECK_NORM("你好", "你好");

  /* Cardinals */

  CHECK_NORM("0", "零");
  CHECK_NORM("10", "十");
  CHECK_NORM("15", "十五");
  CHECK_NORM("110", "一百一十");
  CHECK_NORM("1005", "一千零五");
  CHECK_NORM("1050", "一千零五十");
  CHECK_NORM("10010", "一万零一十");
  CHECK_NORM("12345", "一万二千三百四十五");
  CHECK_NORM("100000", "十万");
  CHECK_NORM("100000000", "一亿");
  CHECK_NORM("1,234,567", "一百二十三万四千五百六十七");
  CHECK_NORM("第1，2，3名", "第一，二，三名");
  CHECK_NORM("1,2,3", "一,二,三");

  /* Identifiers are read digit by digit. */

  CHECK_NORM("13800138000", "一三八零零一三八零零零");
  CHECK_NORM("007", "零零七");
  CHECK_NORM("K7和RK3576", "K七和RK三五七六");

  /* Decimals, percentages, fractions, signs */

  CHECK_NORM("3.14", "三点一四");
  CHECK_NORM("0.5", "零点五");
  CHECK_NORM("1.2.3", "一点二点三");
  CHECK_NORM("45%", "百分之四十五");
  CHECK_NORM("99.9％", "百分之九十九点九");
  CHECK_NORM("1/2", "二分之一");
  CHECK_NORM("气温-5度", "气温负五度");
  CHECK_NORM("3-5天", "三-五天");
  CHECK_NORM("1+1=2", "一加一等于二");

  /* Years, dates, times */

  CHECK_NORM("2026年9月20日", "二零二六年九月二十日");
  CHECK_NORM("2026-09-20", "二零二六年九月二十日");
  CHECK_NORM("2026/9/20", "二零二六年九月二十日");
  CHECK_NORM("5000年", "五千年");
  CHECK_NORM("3点15分", "三点十五分");
  CHECK_NORM("15:30", "十五点三十分");
  CHECK_NORM("9:05", "九点零五分");
  CHECK_NORM("8:00", "八点整");
  CHECK_NORM("２：３０", "两点三十分");
  CHECK_NORM("23:59:59", "二十三点五十九分五十九秒");
  CHECK_NORM("比分3:2", "比分三:二");

  /* Units and currency */

  CHECK_NORM("23.5℃", "二十三点五摄氏度");
  CHECK_NORM("5km", "五公里");
  CHECK_NORM("5 kg", "五公斤");
  CHECK_NORM("60km/h", "六十公里每小时");
  CHECK_NORM("5G网络", "五G网络");
  CHECK_NORM("5 meters", "五 meters");
  CHECK_NORM("¥100", "一百元");
  CHECK_NORM("$5", "五美元");

  /* "两" in front of measure words, but not in ordinals. */

  CHECK_NORM("2个", "两个");
  CHECK_NORM("2点", "两点");
  CHECK_NORM("2月", "二月");
  CHECK_NORM("2年级", "二年级");
  CHECK_NORM("12个", "十二个");

  /* Garbage in must not throw or loop. */

  CHECK_NORM("9999999999999999999999", "九九九九九九九九九九九九九九九九九九九九九九");
  CHECK(!MeloFrontend::Normalize("\xff\xfe 12:").empty());
  CHECK(MeloFrontend::Normalize("2026-13-40").find("年") ==
        std::string::npos);
}

/****************************************************************************
 * Tests that need the model assets
 ****************************************************************************/

void TestGolden(const MeloFrontend &fe, const std::string &golden_dir)
{
  const Ids want_x = ToIds(kGoldenX);
  const Ids want_tones = ToIds(kGoldenTones);
  const auto utts = fe.Process(kGoldenText, 512);

  CHECK(want_x.size() == 95 && want_tones.size() == 95);
  CHECK(utts.size() == 1);
  if (utts.size() == 1)
    {
      const long dx = FirstDiff(utts[0].phonemes, want_x);
      const long dt = FirstDiff(utts[0].tones, want_tones);

      if (dx >= 0 || dt >= 0)
        {
          std::printf("golden mismatch: first phoneme diff %ld, first tone "
                      "diff %ld\n", dx, dt);
        }

      CHECK(dx < 0);
      CHECK(dt < 0);
      CHECK(utts[0].symbols == 47);
      CHECK(utts[0].dropped == 0);
      CHECK(!utts[0].over_budget);
      CHECK(utts[0].text == kGoldenText);
    }

  if (!golden_dir.empty())
    {
      Ids file_x;
      Ids file_tones;

      CHECK(ReadInt64File(golden_dir + "/X.BIN", &file_x));
      CHECK(ReadInt64File(golden_dir + "/TONES.BIN", &file_tones));
      CHECK(file_x == want_x);
      CHECK(file_tones == want_tones);
      if (utts.size() == 1)
        {
          CHECK(utts[0].phonemes == file_x);
          CHECK(utts[0].tones == file_tones);
        }

      std::printf("golden files checked: %s\n", golden_dir.c_str());
    }
  else
    {
      std::printf("golden files not given, embedded ids only\n");
    }
}

void TestSegmenters(const std::string &assets)
{
  std::string error;
  MeloFrontend::Options options;

  /* One utterance per sentence: same symbols, three utterances. */

  options.merge_sentences = false;
  auto split = MeloFrontend::Load(assets, &error, options);
  CHECK(split != nullptr);
  if (split)
    {
      const auto utts = split->Process(kGoldenText, 512);
      Ids x;
      Ids tones;
      Ids want_x;
      Ids want_tones;

      CHECK(utts.size() == 3);
      Flatten(utts, &x, &tones);
      for (std::size_t i = 1; i < 95; i += 2)
        {
          want_x.push_back(kGoldenX[i]);
          want_tones.push_back(kGoldenTones[i]);
        }

      CHECK(x == want_x);
      CHECK(tones == want_tones);
      for (const auto &u : utts)
        {
          CHECK(WellFormed(u));
        }
    }

  /* Forward maximum matching is the sherpa-onnx behaviour.  It is known
   * to read "要不要" as "要不/要" (bu4): same phonemes as the golden ids
   * and exactly one syllable (two symbols) with a different tone.  The
   * check pins that difference so a silent change of either segmenter
   * shows up here.
   */

  options = MeloFrontend::Options();
  options.segmenter = MeloFrontend::Segmenter::kForwardMaxMatch;
  auto fmm = MeloFrontend::Load(assets, &error, options);
  CHECK(fmm != nullptr);
  if (fmm)
    {
      const auto utts = fmm->Process(kGoldenText, 512);

      CHECK(fmm->active_segmenter() ==
            MeloFrontend::Segmenter::kForwardMaxMatch);
      CHECK(fmm->load_stats().jieba_entries == 0);
      CHECK(utts.size() == 1);
      if (utts.size() == 1)
        {
          std::size_t diffs = 0;

          CHECK(utts[0].phonemes == ToIds(kGoldenX));
          CHECK(utts[0].tones.size() == 95);
          for (std::size_t i = 0; i < utts[0].tones.size() && i < 95; i++)
            {
              if (utts[0].tones[i] != kGoldenTones[i])
                {
                  diffs++;
                  CHECK(utts[0].tones[i] == 4 && kGoldenTones[i] == 2);
                }
            }

          CHECK(diffs == 2);
          std::printf("fmm vs golden: first tone diff at %ld\n",
                      FirstDiff(utts[0].tones, ToIds(kGoldenTones)));
        }
    }

  /* Regressions found while comparing against sherpa-onnx and jieba:
   *  - "不对" must stay one lexicon word (bu2); a pure max-probability
   *    segmenter cuts it into "不" + "对" and loses the sandhi (bu4).
   *  - "银行行长" is "银行" + "行长" (hang2 / hang2 zhang3).
   *  - "嗯" is not in the lexicon and is read as its homophone "恩".
   */

  auto standard = MeloFrontend::Load(assets, &error);
  CHECK(standard != nullptr);
  if (standard)
    {
      const auto wrong = standard->Process("这样不对", 0);
      CHECK(wrong.size() == 1);
      if (wrong.size() == 1 && wrong[0].symbols == 8)
        {
          CHECK(wrong[0].tones[2 * 4 + 1] == 2);
        }

      const auto bank = standard->Process("银行行长", 0);
      const auto words = standard->Process("银行 行长", 0);
      CHECK(bank.size() == 1 && words.size() == 1);
      if (bank.size() == 1 && words.size() == 1)
        {
          CHECK(bank[0].phonemes == words[0].phonemes);
          CHECK(bank[0].tones == words[0].tones);
        }

      const auto hmm = standard->Process("嗯，好的", 0);
      const auto homophone = standard->Process("恩，好的", 0);
      CHECK(hmm.size() == 1 && homophone.size() == 1);
      if (hmm.size() == 1 && homophone.size() == 1)
        {
          CHECK(hmm[0].phonemes == homophone[0].phonemes);
          CHECK(hmm[0].dropped == 0);
        }
    }

  /* A missing asset is an error string, not a crash. */

  error.clear();
  CHECK(MeloFrontend::Load(assets + "/no-such-dir", &error) == nullptr);
  CHECK(!error.empty());
}

void TestNumbersAndDates(const MeloFrontend &fe)
{
  /* Every hanzi the normaliser can emit must exist in the lexicon:
   * nothing may be dropped, and digits must sound like the hanzi reading.
   */

  static const char *const kPairs[][2] =
  {
    {"今天是2026年9月20日，气温23.5度，湿度45%。",
     "今天是二零二六年九月二十日，气温二十三点五度，湿度百分之四十五。"},
    {"现在是下午3点15分，会议15:30开始。",
     "现在是下午三点十五分，会议十五点三十分开始。"},
    {"一共有12345个苹果，价格是3.14元。",
     "一共有一万二千三百四十五个苹果，价格是三点一四元。"},
    {"跑了5km，花了¥100，还剩1/2，零下-5℃，1+1=2，8:00，2个",
     "跑了五公里，花了一百元，还剩二分之一，零下负五摄氏度，"
     "一加一等于二，八点整，两个"},
    {"60km/h 3m/s 5cm 5mm 5kg 5mg 5g 5ml 5L 5ms 5s 5min 5h 5Hz 5kHz 5MHz "
     "5GHz 5V 5W 5kW 5mAh 5mA 5dB 5° 5‰ $5 €5 £5",
     "六十公里每小时 三米每秒 五厘米 五毫米 五公斤 五毫克 五克 五毫升 五升 "
     "五毫秒 五秒 五分钟 五小时 五赫兹 五千赫 五兆赫 五吉赫 五伏 五瓦 五千瓦 "
     "五毫安时 五毫安 五分贝 五度 千分之五 五美元 五欧元 五英镑"},
  };

  for (const auto &pair : kPairs)
    {
      const auto a = fe.Process(pair[0], 0);
      const auto b = fe.Process(pair[1], 0);

      CHECK(a.size() == 1 && b.size() == 1);
      if (a.size() == 1 && b.size() == 1)
        {
          if (a[0].phonemes != b[0].phonemes || a[0].dropped != 0)
            {
              std::printf("number case: %s\n  normalized: %s\n", pair[0],
                          a[0].text.c_str());
            }

          CHECK(a[0].phonemes == b[0].phonemes);
          CHECK(a[0].tones == b[0].tones);
          CHECK(a[0].dropped == 0);
          CHECK(b[0].dropped == 0);
        }
    }
}

void TestEnglish(const MeloFrontend &fe)
{
  const auto mixed = fe.Process("我喜欢用 openvela 和 WiFi，hello world！", 0);

  CHECK(mixed.size() == 1);
  if (mixed.size() == 1)
    {
      bool chinese = false;
      bool english = false;

      for (std::int64_t t : mixed[0].tones)
        {
          chinese |= t >= 1 && t <= 5;
          english |= t >= 7;
        }

      CHECK(chinese && english);
      CHECK(mixed[0].dropped == 0);
      CHECK(WellFormed(mixed[0]));
    }

  /* Case does not matter for ordinary words. */

  const auto lower = fe.Process("hello world", 0);
  const auto title = fe.Process("Hello World", 0);
  CHECK(lower.size() == 1 && title.size() == 1);
  if (lower.size() == 1 && title.size() == 1)
    {
      /* hh ah l ow + w er l d */

      CHECK(lower[0].symbols == 8);
      CHECK(lower[0].phonemes == title[0].phonemes);
    }

  /* The alias makes the wake word two real words, not eight letters. */

  const auto wake = fe.Process("openvela", 0);
  const auto words = fe.Process("open vela", 0);
  CHECK(wake.size() == 1 && words.size() == 1);
  if (wake.size() == 1 && words.size() == 1)
    {
      CHECK(wake[0].phonemes == words[0].phonemes);
    }

  /* Unknown words fall back to letters; acronyms are always letters. */

  const auto unknown = fe.Process("qzxv", 0);
  const auto letters = fe.Process("q z x v", 0);
  CHECK(unknown.size() == 1 && letters.size() == 1);
  if (unknown.size() == 1 && letters.size() == 1)
    {
      CHECK(unknown[0].phonemes == letters[0].phonemes);
      CHECK(unknown[0].dropped == 0);
    }

  /* "AI" is the letters ey + ay (ids 33, 18), neither the lexicon word
   * "ai" (ay) nor the article "a" (ah).
   */

  const auto acronym = fe.Process("AI", 0);
  const auto usb = fe.Process("USB", 0);
  const auto letters_usb = fe.Process("u s b", 0);
  CHECK(acronym.size() == 1 && usb.size() == 1 && letters_usb.size() == 1);
  if (acronym.size() == 1 && usb.size() == 1 && letters_usb.size() == 1)
    {
      CHECK(acronym[0].phonemes == (Ids{0, 33, 0, 18, 0}));
      CHECK(usb[0].phonemes == letters_usb[0].phonemes);
    }

  /* Compounds, contractions, possessives and hyphens produce speech. */

  for (const char *text : {"bluetooth", "don't", "星喵's", "cat's", "wi-fi",
                           "zzyzx's", "state-of-the-art"})
    {
      const auto utts = fe.Process(text, 0);
      CHECK(utts.size() == 1);
      if (utts.size() == 1)
        {
          CHECK(WellFormed(utts[0]));
        }
    }

  const auto compound = fe.Process("bluetooth", 0);
  const auto pieces = fe.Process("blue tooth", 0);
  if (compound.size() == 1 && pieces.size() == 1)
    {
      CHECK(compound[0].phonemes == pieces[0].phonemes);
    }
}

void TestPunctuation(const MeloFrontend &fe)
{
  /* Nothing speakable -> no utterance at all (no silent synthesis). */

  CHECK(fe.Process("", 512).empty());
  CHECK(fe.Process("   \n\t  ", 512).empty());
  CHECK(fe.Process("！！！。。。？", 512).empty());
  CHECK(fe.Process("“”（）【】", 512).empty());

  const auto plain = fe.Process("你好！", 0);
  const auto noisy = fe.Process("，，“你好”！！！", 0);
  const auto ascii = fe.Process("你好!", 0);
  CHECK(plain.size() == 1 && noisy.size() == 1 && ascii.size() == 1);
  if (plain.size() == 1 && noisy.size() == 1 && ascii.size() == 1)
    {
      CHECK(plain[0].symbols == 5); /* n i h ao ! */
      CHECK(plain[0].phonemes == noisy[0].phonemes);
      CHECK(plain[0].phonemes == ascii[0].phonemes);
      CHECK(noisy[0].dropped == 0);
    }

  /* Line ends close sentences and get a full stop when none is there. */

  const auto lines = fe.Process("你好\n再见", 0);
  const auto stops = fe.Process("你好。再见", 0);
  CHECK(lines.size() == 1 && stops.size() == 1);
  if (lines.size() == 1 && stops.size() == 1)
    {
      CHECK(lines[0].phonemes == stops[0].phonemes);
    }

  /* Enumeration comma, colon and semicolon all read as ",". */

  const auto commas = fe.Process("苹果，香蕉，橘子，葡萄", 0);
  const auto others = fe.Process("苹果、香蕉：橘子；葡萄", 0);
  CHECK(commas.size() == 1 && others.size() == 1);
  if (commas.size() == 1 && others.size() == 1)
    {
      CHECK(commas[0].phonemes == others[0].phonemes);
    }

  /* A dot inside a token is not a sentence end. */

  const auto url = fe.Process("openvela.com", 0);
  CHECK(url.size() == 1);

  /* Unknown code points are dropped and counted, never fatal. */

  const std::uint64_t before = fe.dropped_total();
  const auto emoji = fe.Process("你好\xf0\x9f\x98\xba@\xff世界", 0);
  CHECK(emoji.size() == 1);
  if (emoji.size() == 1)
    {
      const auto clean = fe.Process("你好世界", 0);

      CHECK(emoji[0].dropped == 3); /* emoji, '@', invalid byte */
      CHECK(clean.size() == 1 &&
            clean[0].phonemes == emoji[0].phonemes);
    }

  CHECK(fe.dropped_total() == before + 3);

  /* Only dropped characters: counted, but no utterance. */

  CHECK(fe.Process("\xf0\x9f\x98\xba\xf0\x9f\x98\xba", 512).empty());
  CHECK(fe.dropped_total() == before + 5);
}

void TestSplitter(const MeloFrontend &fe, const std::string &assets)
{
  const std::size_t budget = fe.SymbolBudget(512);

  CHECK(budget >= 47); /* The golden sentence must fit one bucket. */
  CHECK(fe.SymbolBudget(0) == 0);
  CHECK(fe.SymbolBudget(1) == 1);

  /* Very long input: many sentences. */

  std::string text;
  for (int k = 0; k < 2000; k++)
    {
      text += "今天天气真不错，我们一起去公园散步吧。";
      text += "Hello world, this is a test! ";
      text += "现在是15:30，气温23.5度？\n";
    }

  const auto packed = fe.Process(text, 512);
  const auto whole = fe.Process(text, 0);
  Ids px;
  Ids pt;
  Ids wx;
  Ids wt;

  CHECK(whole.size() == 1);
  CHECK(packed.size() > 2000);
  Flatten(packed, &px, &pt);
  Flatten(whole, &wx, &wt);

  /* Splitting must neither lose nor reorder a single symbol. */

  CHECK(px == wx);
  CHECK(pt == wt);
  for (const auto &u : packed)
    {
      CHECK(WellFormed(u));
      CHECK(!u.over_budget);
      CHECK(u.symbols <= budget);
    }

  /* A sentence over budget is cut at commas, and only there. */

  const std::string clause = "今天天气真不错我们一起去公园散步吧";
  const std::string sentence =
      clause + "，" + clause + "，" + clause + "，" + clause + "。";
  const auto clauses = fe.Process(sentence, 512);

  CHECK(clauses.size() >= 2);
  for (std::size_t k = 0; k < clauses.size(); k++)
    {
      const auto &u = clauses[k];
      const std::int64_t last = u.phonemes[u.phonemes.size() - 2];

      CHECK(!u.over_budget);
      CHECK(u.symbols <= budget);

      /* 106 is "," and 107 is ".": every piece ends at punctuation. */

      CHECK(last == (k + 1 == clauses.size() ? 107 : 106));
    }

  /* No punctuation at all: split between words by default ... */

  std::string run;
  for (int k = 0; k < 30; k++)
    {
      run += clause;
    }

  const auto by_words = fe.Process(run, 512);
  const auto run_whole = fe.Process(run, 0);
  CHECK(by_words.size() > 1 && run_whole.size() == 1);
  Flatten(by_words, &px, &pt);
  Flatten(run_whole, &wx, &wt);
  CHECK(px == wx);
  CHECK(pt == wt);
  for (const auto &u : by_words)
    {
      CHECK(!u.over_budget);
      CHECK(u.symbols <= budget);
    }

  /* ... and returned whole but flagged when that is disabled.  Never
   * truncated: the flagged utterance still has every symbol.
   */

  std::string error;
  MeloFrontend::Options options;
  options.split_at_words = false;
  auto strict = MeloFrontend::Load(assets, &error, options);
  CHECK(strict != nullptr);
  if (strict)
    {
      const auto flagged = strict->Process(run + "。你好。", 512);

      CHECK(flagged.size() == 2);
      if (flagged.size() == 2)
        {
          CHECK(flagged[0].over_budget);
          CHECK(flagged[0].symbols == run_whole[0].symbols + 1);
          CHECK(WellFormed(flagged[0]));
          CHECK(!flagged[1].over_budget);
        }
    }

  /* The budget is a parameter: a small bucket gives smaller utterances. */

  const auto small = fe.Process(kGoldenText, 128);
  CHECK(small.size() > 1);
  for (const auto &u : small)
    {
      CHECK(u.symbols <= fe.SymbolBudget(128));
    }
}

void TestSandhi(const std::string &assets)
{
  std::string error;
  MeloFrontend::Options options;

  options.yi_bu_sandhi = true;
  auto fe = MeloFrontend::Load(assets, &error, options);
  CHECK(fe != nullptr);
  if (!fe)
    {
      return;
    }

  /* 一 + tone 1 -> yi4; inside numbers and ordinals it stays yi1. */

  const auto day = fe->Process("忙了一天", 0);
  CHECK(day.size() == 1);
  if (day.size() == 1 && day[0].symbols == 8)
    {
      CHECK(day[0].tones[2 * 4 + 1] == 4);
      CHECK(day[0].tones[2 * 5 + 1] == 4);
    }

  /* "一十一": neither the middle nor the final 一 may change. */

  const auto number = fe->Process("二百一十一", 0);
  const auto ordinal = fe->Process("第一天", 0);
  const auto weekday = fe->Process("星期一去", 0);
  CHECK(number.size() == 1 && ordinal.size() == 1 && weekday.size() == 1);
  if (number.size() == 1 && number[0].symbols == 10)
    {
      CHECK(number[0].tones[2 * 4 + 1] == 1);
      CHECK(number[0].tones[2 * 8 + 1] == 1);
    }

  /* Word-final 一 keeps yi1 although a tone-4 syllable follows. */

  if (weekday.size() == 1 && weekday[0].symbols == 8)
    {
      CHECK(weekday[0].tones[2 * 4 + 1] == 1);
    }

  /* 不 + tone 4 -> bu2, otherwise unchanged. */

  const auto bu = fe->Process("不去不来", 0);
  CHECK(bu.size() == 1);
  if (bu.size() == 1 && bu[0].symbols == 8)
    {
      CHECK(bu[0].tones[1] == 2);
      CHECK(bu[0].tones[2 * 4 + 1] == 4);
    }

  if (ordinal.size() == 1 && ordinal[0].symbols == 6)
    {
      CHECK(ordinal[0].tones[2 * 2 + 1] == 1);
    }
}

} /* namespace */

int main(int argc, char **argv)
{
  std::string assets = argc > 1 ? argv[1] : "";
  std::string golden = argc > 2 ? argv[2] : "";

  if (assets.empty() && std::getenv("NYAMP_G2P_ASSETS") != nullptr)
    {
      assets = std::getenv("NYAMP_G2P_ASSETS");
    }

  if (golden.empty() && std::getenv("NYAMP_G2P_GOLDEN") != nullptr)
    {
      golden = std::getenv("NYAMP_G2P_GOLDEN");
    }

  TestNormalize();
  if (assets.empty())
    {
      std::printf("%d checks, %d failures; model assets not given "
                  "(argv[1] or NYAMP_G2P_ASSETS): asset tests SKIPPED\n",
                  g_checks, g_failures);
      return g_failures != 0 ? 1 : 77;
    }

  std::string error;
  const auto fe = MeloFrontend::Load(assets, &error);
  if (!fe)
    {
      std::printf("FAIL: cannot load assets: %s\n", error.c_str());
      return 1;
    }

  const auto &stats = fe->load_stats();
  const long peak_kb = PeakRssKb();
  std::printf("load_ms=%.1f lexicon=%zu rejected=%zu jieba=%zu "
              "peak_rss_kb=%ld budget(512)=%zu\n",
              stats.load_ms, stats.lexicon_entries, stats.lexicon_rejected,
              stats.jieba_entries, peak_kb, fe->SymbolBudget(512));

  /* Requirements: well under 100 MB and about a second.  The time bound
   * is loose because the build machine is shared and often swapping.
   */

  CHECK(stats.lexicon_entries > 190000);
  CHECK(stats.load_ms < 5000);
  CHECK(peak_kb < 100 * 1024);
  CHECK(fe->active_segmenter() == MeloFrontend::Segmenter::kFewestWords);

  TestGolden(*fe, golden);
  TestSegmenters(assets);
  TestNumbersAndDates(*fe);
  TestEnglish(*fe);
  TestPunctuation(*fe);
  TestSplitter(*fe, assets);
  TestSandhi(assets);

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures != 0 ? 1 : 0;
}
