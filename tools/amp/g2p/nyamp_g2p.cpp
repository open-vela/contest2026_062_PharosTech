/****************************************************************************
 * tools/amp/g2p/nyamp_g2p.cpp
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

#include "nyamp_g2p.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace nyamp
{

namespace
{

/****************************************************************************
 * UTF-8
 ****************************************************************************/

/* All text logic runs on code points: hanzi are 3 bytes, so byte offsets
 * would make "one character" windows (max-match, DP) needlessly fiddly.
 * Invalid bytes are skipped and counted instead of aborting, because the
 * text comes from an LLM / the network and must never crash the daemon.
 */

std::size_t DecodeUtf8(std::string_view in, std::u32string *out)
{
  std::size_t invalid = 0;
  std::size_t i = 0;

  out->reserve(out->size() + in.size());
  while (i < in.size())
    {
      const unsigned char b0 = static_cast<unsigned char>(in[i]);
      std::size_t need;
      char32_t cp;

      if (b0 < 0x80)
        {
          out->push_back(b0);
          i++;
          continue;
        }
      else if ((b0 & 0xe0) == 0xc0)
        {
          need = 1;
          cp = b0 & 0x1f;
        }
      else if ((b0 & 0xf0) == 0xe0)
        {
          need = 2;
          cp = b0 & 0x0f;
        }
      else if ((b0 & 0xf8) == 0xf0)
        {
          need = 3;
          cp = b0 & 0x07;
        }
      else
        {
          invalid++;
          i++;
          continue;
        }

      if (i + need >= in.size())
        {
          /* Truncated sequence at the end of the buffer. */

          invalid++;
          i++;
          continue;
        }

      bool ok = true;
      for (std::size_t k = 1; k <= need; k++)
        {
          const unsigned char b = static_cast<unsigned char>(in[i + k]);
          if ((b & 0xc0) != 0x80)
            {
              ok = false;
              break;
            }

          cp = (cp << 6) | (b & 0x3f);
        }

      /* Overlong forms and surrogates are rejected so a crafted input
       * cannot alias an ASCII control or punctuation character.
       */

      if (ok && ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
                 (need == 3 && cp < 0x10000) || cp > 0x10ffff ||
                 (cp >= 0xd800 && cp <= 0xdfff)))
        {
          ok = false;
        }

      if (!ok)
        {
          invalid++;
          i++;
          continue;
        }

      out->push_back(cp);
      i += need + 1;
    }

  return invalid;
}

void AppendUtf8(std::string *out, char32_t cp)
{
  if (cp < 0x80)
    {
      out->push_back(static_cast<char>(cp));
    }
  else if (cp < 0x800)
    {
      out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  else if (cp < 0x10000)
    {
      out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  else
    {
      out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string EncodeUtf8(const std::u32string &in, std::size_t begin,
                       std::size_t end)
{
  std::string out;

  out.reserve((end - begin) * 3);
  for (std::size_t i = begin; i < end; i++)
    {
      AppendUtf8(&out, in[i]);
    }

  return out;
}

/****************************************************************************
 * Character classes
 ****************************************************************************/

bool IsDigit(char32_t c)
{
  return c >= U'0' && c <= U'9';
}

bool IsAsciiAlpha(char32_t c)
{
  return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

bool IsAsciiAlnum(char32_t c)
{
  return IsDigit(c) || IsAsciiAlpha(c);
}

bool IsHan(char32_t c)
{
  return (c >= 0x4e00 && c <= 0x9fff) || (c >= 0x3400 && c <= 0x4dbf) ||
         (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x2fa1f);
}

bool IsSpace(char32_t c)
{
  return c == U' ' || c == U'\t' || c == U'\r' || c == U'\f' ||
         c == U'\v' || c == 0x00a0 || c == 0x3000;
}

bool Contains(const char32_t *set, char32_t c)
{
  for (; *set != 0; set++)
    {
      if (*set == c)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Text normalisation (numbers, dates, times, units -> hanzi)
 ****************************************************************************/

/* sherpa-onnx does this step with OpenFst rule files (date.fst, number.fst,
 * phone.fst).  An FST runtime is a heavy dependency for a handful of
 * patterns a companion robot actually says, so the patterns are coded by
 * hand.  Whatever is not recognised degrades to reading digits as plain
 * cardinals, never to an error.
 */

const char32_t kHanDigits[] = U"零一二三四五六七八九";

std::u32string ReadDigits(const std::u32string &digits)
{
  std::u32string out;

  for (char32_t d : digits)
    {
      out.push_back(kHanDigits[d - U'0']);
    }

  return out;
}

std::u32string ReadSection(unsigned value)
{
  static const char32_t *const kUnits[] = {U"千", U"百", U"十", U""};
  static const unsigned kPlace[] = {1000, 100, 10, 1};
  std::u32string out;
  bool zero = false;

  for (int k = 0; k < 4; k++)
    {
      const unsigned d = value / kPlace[k] % 10;
      if (d == 0)
        {
          /* An inner run of zeros is spoken once ("一千零五"), a trailing
           * run not at all ("一千五百").
           */

          zero = !out.empty();
          continue;
        }

      if (zero)
        {
          out += U"零";
          zero = false;
        }

      out.push_back(kHanDigits[d]);
      out += kUnits[k];
    }

  return out;
}

/* digits: 1..16 ASCII digits without a leading zero (caller guarantees). */

std::u32string ReadCardinal(const std::u32string &digits)
{
  static const char32_t *const kBig[] = {U"", U"万", U"亿", U"万亿"};
  std::u32string out;
  bool pending_zero = false;
  const std::size_t sections = (digits.size() + 3) / 4;

  for (std::size_t s = 0; s < sections; s++)
    {
      const std::size_t end = digits.size() - (sections - 1 - s) * 4;
      const std::size_t begin = end >= 4 ? end - 4 : 0;
      unsigned value = 0;

      for (std::size_t k = begin; k < end; k++)
        {
          value = value * 10 + static_cast<unsigned>(digits[k] - U'0');
        }

      if (value == 0)
        {
          pending_zero = !out.empty();
          continue;
        }

      if (!out.empty() && (pending_zero || value < 1000))
        {
          out += U"零";
        }

      out += ReadSection(value);
      out += kBig[sections - 1 - s];
      pending_zero = false;
    }

  if (out.empty())
    {
      return U"零";
    }

  /* 10..19 (and 10万..19万) are "十五", not "一十五", at the very front. */

  if (out.size() >= 2 && out[0] == U'一' && out[1] == U'十')
    {
      out.erase(0, 1);
    }

  return out;
}

/* Long digit strings (phone numbers, ids) and strings with a leading zero
 * are identifiers rather than quantities, so they are read digit by digit.
 */

std::u32string ReadNumber(const std::u32string &digits)
{
  if (digits.size() >= 11 || (digits.size() > 1 && digits[0] == U'0'))
    {
      return ReadDigits(digits);
    }

  return ReadCardinal(digits);
}

unsigned long ToValue(const std::u32string &digits)
{
  unsigned long v = 0;

  for (char32_t d : digits)
    {
      v = v * 10 + static_cast<unsigned long>(d - U'0');
      if (v > 100000000ul)
        {
          break; /* Only used for small range checks. */
        }
    }

  return v;
}

std::u32string StripLeadingZeros(const std::u32string &digits)
{
  std::size_t k = 0;

  while (k + 1 < digits.size() && digits[k] == U'0')
    {
      k++;
    }

  return digits.substr(k);
}

/* Reads [*pos, ...) while digits, at most max_len; returns the run. */

std::u32string TakeDigits(const std::u32string &s, std::size_t *pos,
                          std::size_t max_len)
{
  std::u32string run;

  while (*pos < s.size() && IsDigit(s[*pos]) && run.size() < max_len)
    {
      run.push_back(s[*pos]);
      (*pos)++;
    }

  return run;
}

struct UnitName
{
  const char32_t *ascii;
  const char32_t *hanzi;
};

/* Case sensitive on purpose: "5g" is grams while "5G" is a network. */

const UnitName kUnits[] =
{
  {U"km/h", U"公里每小时"}, {U"m/s", U"米每秒"}, {U"km", U"公里"},
  {U"KM", U"公里"},         {U"cm", U"厘米"},    {U"mm", U"毫米"},
  {U"m", U"米"},            {U"kg", U"公斤"},    {U"KG", U"公斤"},
  {U"mg", U"毫克"},         {U"g", U"克"},       {U"ml", U"毫升"},
  {U"mL", U"毫升"},         {U"L", U"升"},       {U"ms", U"毫秒"},
  {U"s", U"秒"},            {U"min", U"分钟"},   {U"h", U"小时"},
  {U"kHz", U"千赫"},        {U"MHz", U"兆赫"},   {U"GHz", U"吉赫"},
  {U"Hz", U"赫兹"},         {U"V", U"伏"},       {U"kW", U"千瓦"},
  {U"W", U"瓦"},            {U"mAh", U"毫安时"}, {U"mA", U"毫安"},
  {U"dB", U"分贝"},         {U"℃", U"摄氏度"},
  {U"°C", U"摄氏度"},  {U"°", U"度"},
};

/* Measure words in front of which a bare "2" is "两", not "二". */

const char32_t kLiangMeasures[] =
    U"个只次天位条张本件块杯种名台部点岁秒遍趟句首颗篇头匹辆架间周年";

std::size_t U32Len(const char32_t *s)
{
  std::size_t n = 0;

  while (s[n] != 0)
    {
      n++;
    }

  return n;
}

bool StartsWith(const std::u32string &s, std::size_t pos,
                const char32_t *prefix)
{
  const std::size_t n = U32Len(prefix);

  return pos + n <= s.size() && s.compare(pos, n, prefix) == 0;
}

/* Converts the number starting at s[i] (a digit) and appends the reading to
 * *out.  Returns the index of the first unconsumed code point.
 */

std::size_t ConvertNumber(const std::u32string &s, std::size_t i,
                          const char32_t *currency, std::u32string *out)
{
  const std::size_t n = s.size();
  std::size_t j = i;
  std::u32string intpart = TakeDigits(s, &j, n);

  /* Digits glued to a Latin prefix are model names ("K7", "RK3576",
   * "mp3"), which people read digit by digit.
   */

  if (i > 0 && IsAsciiAlpha(s[i - 1]))
    {
      *out += ReadDigits(intpart);
      return j;
    }

  /* 2026-09-20, 2026/9/20, 2026.09.20 */

  if (intpart.size() == 4 && j < n &&
      (s[j] == U'-' || s[j] == U'/' || s[j] == U'.'))
    {
      const char32_t sep = s[j];
      std::size_t k = j + 1;
      const std::u32string month = TakeDigits(s, &k, 2);

      if (!month.empty() && k < n && s[k] == sep)
        {
          k++;
          const std::u32string day = TakeDigits(s, &k, 2);
          const unsigned long mv = ToValue(month);
          const unsigned long dv = day.empty() ? 0 : ToValue(day);

          if (mv >= 1 && mv <= 12 && dv >= 1 && dv <= 31 &&
              !(k < n && IsDigit(s[k])))
            {
              *out += ReadDigits(intpart) + U"年";
              *out += ReadCardinal(StripLeadingZeros(month)) + U"月";
              *out += ReadCardinal(StripLeadingZeros(day)) + U"日";
              return k;
            }
        }
    }

  /* 15:30, 9:05, 23:59:59.  Minutes must be two digits so that scores and
   * ratios such as "3:2" are left alone.
   */

  if (intpart.size() <= 2 && j < n && s[j] == U':')
    {
      std::size_t k = j + 1;
      const std::u32string minute = TakeDigits(s, &k, 2);

      if (minute.size() == 2 && !(k < n && IsDigit(s[k])) &&
          ToValue(intpart) <= 24 && ToValue(minute) < 60)
        {
          std::u32string second;

          if (k + 2 < n && s[k] == U':' && IsDigit(s[k + 1]))
            {
              std::size_t k2 = k + 1;
              const std::u32string sec = TakeDigits(s, &k2, 2);

              if (sec.size() == 2 && !(k2 < n && IsDigit(s[k2])) &&
                  ToValue(sec) < 60)
                {
                  second = sec;
                  k = k2;
                }
            }

          const unsigned long hv = ToValue(intpart);
          const unsigned long mv = ToValue(minute);

          *out += hv == 2 ? std::u32string(U"两")
                          : ReadCardinal(StripLeadingZeros(intpart));
          *out += U"点";
          if (mv == 0 && second.empty())
            {
              *out += U"整";
            }
          else
            {
              if (mv < 10)
                {
                  *out += U"零";
                }

              if (mv != 0)
                {
                  *out += ReadCardinal(StripLeadingZeros(minute));
                }

              *out += U"分";
            }

          if (!second.empty())
            {
              if (ToValue(second) < 10)
                {
                  *out += U"零";
                }

              if (ToValue(second) != 0)
                {
                  *out += ReadCardinal(StripLeadingZeros(second));
                }

              *out += U"秒";
            }

          return k;
        }
    }

  /* 1,234,567 -- only the strict 3-digit grouping, so that enumerations
   * like "1,2,3" keep their commas (and their pauses).
   */

  if (intpart.size() <= 3 && intpart[0] != U'0' && j < n && s[j] == U',')
    {
      std::u32string joined = intpart;
      std::size_t k = j;
      bool ok = false;

      while (k < n && s[k] == U',')
        {
          std::size_t k2 = k + 1;
          const std::u32string group = TakeDigits(s, &k2, 4);

          if (group.size() != 3)
            {
              ok = false;
              break;
            }

          joined += group;
          k = k2;
          ok = true;
        }

      if (ok)
        {
          intpart = joined;
          j = k;
        }
    }

  /* A four digit number in front of "年" is a year, read digit by digit
   * ("二零二六年").  Restricted to 1000..2999 so that durations such as
   * "5000年" stay cardinals.
   */

  if (intpart.size() == 4 && j < n && s[j] == U'年' &&
      (intpart[0] == U'1' || intpart[0] == U'2'))
    {
      *out += ReadDigits(intpart);
      return j;
    }

  /* a/b -> "b分之a" */

  if (intpart.size() <= 4 && j + 1 < n && s[j] == U'/' && IsDigit(s[j + 1]))
    {
      std::size_t k = j + 1;
      const std::u32string den = TakeDigits(s, &k, 5);

      if (den.size() <= 4 && ToValue(den) != 0 &&
          !(k < n && (IsDigit(s[k]) || s[k] == U'/')))
        {
          *out += ReadNumber(StripLeadingZeros(den)) + U"分之" +
                  ReadNumber(StripLeadingZeros(intpart));
          return k;
        }
    }

  /* Integer part, then every ".digits" group read digit by digit.  More
   * than one group is a version / IP address ("1.2.3" -> 一点二点三).
   */

  std::u32string body = ReadNumber(intpart);
  bool has_fraction = false;
  std::size_t k = j;

  while (k + 1 < n && s[k] == U'.' && IsDigit(s[k + 1]))
    {
      k++;
      body += U"点";
      body += ReadDigits(TakeDigits(s, &k, n));
      has_fraction = true;
    }

  if (k < n && (s[k] == U'%' || s[k] == 0x2030))
    {
      *out += s[k] == U'%' ? U"百分之" : U"千分之";
      *out += body;
      return k + 1;
    }

  /* Unit directly after the number (one optional space).  The character
   * after the unit must not be alphanumeric, otherwise "5 meters" would
   * become "五米eters".
   */

  {
    const std::size_t u = (k < n && s[k] == U' ') ? k + 1 : k;
    const UnitName *best = nullptr;
    std::size_t best_len = 0;

    for (const UnitName &unit : kUnits)
      {
        const std::size_t len = U32Len(unit.ascii);

        if (len > best_len && StartsWith(s, u, unit.ascii) &&
            !(u + len < n && IsAsciiAlnum(s[u + len])))
          {
            best = &unit;
            best_len = len;
          }
      }

    if (best != nullptr)
      {
        *out += body;
        *out += best->hanzi;
        return u + best_len;
      }
  }

  if (!has_fraction && intpart == U"2" && k < n &&
      Contains(kLiangMeasures, s[k]))
    {
      /* "2年级" is an ordinal and "2点5" a decimal: both keep "二". */

      const bool ordinal = s[k] == U'年' && k + 1 < n && s[k + 1] == U'级';
      const bool decimal = s[k] == U'点' && k + 1 < n && IsDigit(s[k + 1]);

      if (!ordinal && !decimal)
        {
          body = U"两";
        }
    }

  *out += body;
  if (currency != nullptr && !(k < n && (s[k] == U'元' || s[k] == U'块')))
    {
      *out += currency;
    }

  return k;
}

const char32_t *CurrencyName(char32_t c)
{
  switch (c)
    {
      case 0x00a5: /* yen sign */
      case 0xffe5: /* fullwidth yen sign */
        return U"元";
      case U'$':
        return U"美元";
      case 0x20ac:
        return U"欧元";
      case 0x00a3:
        return U"英镑";
      default:
        return nullptr;
    }
}

/* True when the nearest non-space neighbour in direction dir is a digit. */

bool DigitNeighbour(const std::u32string &s, std::size_t i, int dir)
{
  std::size_t k = i;

  for (; ; )
    {
      if (dir < 0)
        {
          if (k == 0)
            {
              return false;
            }

          k--;
        }
      else
        {
          k++;
          if (k >= s.size())
            {
              return false;
            }
        }

      if (s[k] != U' ')
        {
          return IsDigit(s[k]);
        }
    }
}

const char32_t kKeepFullwidth[] = U"，！？；（）";

std::u32string NormalizeU32(const std::u32string &raw)
{
  std::u32string s;

  /* Fold fullwidth ASCII variants so one set of rules serves "１５：３０".
   * The Chinese sentence marks and parentheses are kept: they need no rule
   * here, the utterance text stays as typed for the logs, and a folded "，"
   * could act as a thousands separator.
   */

  s.reserve(raw.size());
  for (char32_t c : raw)
    {
      if (c >= 0xff01 && c <= 0xff5e && !Contains(kKeepFullwidth, c))
        {
          c -= 0xfee0;
        }
      else if (c == 0x3007)
        {
          c = U'零';
        }

      s.push_back(c);
    }

  std::u32string out;
  const std::size_t n = s.size();
  std::size_t i = 0;

  out.reserve(n + n / 4);
  while (i < n)
    {
      const char32_t c = s[i];

      if (IsDigit(c))
        {
          i = ConvertNumber(s, i, nullptr, &out);
        }
      else if ((c == U'-' || c == 0x2212) && i + 1 < n && IsDigit(s[i + 1]) &&
               !(i > 0 && (IsAsciiAlnum(s[i - 1]) || s[i - 1] == U')' ||
                           s[i - 1] == U'%')))
        {
          /* A minus sign only when nothing number-like precedes it, so
           * ranges ("3-5") and dates keep their hyphen.
           */

          out += U"负";
          i++;
        }
      else if (CurrencyName(c) != nullptr && i + 1 < n && IsDigit(s[i + 1]))
        {
          i = ConvertNumber(s, i + 1, CurrencyName(c), &out);
        }
      else if ((c == U'+' || c == U'=' || c == 0x00d7 || c == 0x00f7) &&
               (DigitNeighbour(s, i, -1) || DigitNeighbour(s, i, 1)))
        {
          out += c == U'+' ? U"加" : c == U'=' ? U"等于"
                 : c == 0x00d7 ? U"乘" : U"除以";
          i++;
        }
      else
        {
          out.push_back(c);
          i++;
        }
    }

  return out;
}

/****************************************************************************
 * Compact string -> uint32 hash table
 ****************************************************************************/

/* std::unordered_map<std::string, ...> costs ~80 bytes of node, bucket and
 * heap-string overhead per entry; with 196 k lexicon words plus 349 k jieba
 * words that alone would be ~45 MB on a board whose RAM is shared with the
 * NPU runtime.  Keys live back to back in one arena and the open-addressing
 * slots hold 4-byte indices, which brings both tables to ~15 MB.
 */

class StringTable
{
public:
  void Reserve(std::size_t entries, std::size_t bytes)
  {
    entries_.reserve(entries);
    arena_.reserve(bytes);
    Rehash(SlotsFor(entries));
  }

  /* Returns false (and keeps the first value) on a duplicate key. */

  bool Insert(std::string_view key, std::uint32_t value)
  {
    if (key.empty() || key.size() > 0xffff)
      {
        return false;
      }

    if ((entries_.size() + 1) * 2 > slots_.size())
      {
        Rehash(SlotsFor((entries_.size() + 1) * 2));
      }

    std::size_t slot = Hash(key) & (slots_.size() - 1);
    while (slots_[slot] != 0)
      {
        if (KeyOf(entries_[slots_[slot] - 1]) == key)
          {
            return false;
          }

        slot = (slot + 1) & (slots_.size() - 1);
      }

    Entry e;
    e.offset = static_cast<std::uint32_t>(arena_.size());
    e.value = value;
    e.length = static_cast<std::uint16_t>(key.size());
    arena_.append(key.data(), key.size());
    entries_.push_back(e);
    slots_[slot] = static_cast<std::uint32_t>(entries_.size());
    return true;
  }

  bool Find(std::string_view key, std::uint32_t *value) const
  {
    if (slots_.empty() || key.empty())
      {
        return false;
      }

    std::size_t slot = Hash(key) & (slots_.size() - 1);
    while (slots_[slot] != 0)
      {
        const Entry &e = entries_[slots_[slot] - 1];
        if (KeyOf(e) == key)
          {
            *value = e.value;
            return true;
          }

        slot = (slot + 1) & (slots_.size() - 1);
      }

    return false;
  }

  bool Has(std::string_view key) const
  {
    std::uint32_t unused;
    return Find(key, &unused);
  }

  std::size_t size() const
  {
    return entries_.size();
  }

  void Shrink()
  {
    entries_.shrink_to_fit();
    arena_.shrink_to_fit();
  }

private:
  struct Entry
  {
    std::uint32_t offset;
    std::uint32_t value;
    std::uint16_t length;
  };

  static std::size_t SlotsFor(std::size_t entries)
  {
    std::size_t slots = 1024;

    while (slots < entries * 2)
      {
        slots *= 2;
      }

    return slots;
  }

  static std::uint32_t Hash(std::string_view key)
  {
    std::uint32_t h = 2166136261u; /* FNV-1a */

    for (char c : key)
      {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
      }

    /* Final avalanche: FNV's low bits are weak for 3-byte hanzi keys and
     * the slot index uses exactly those bits.
     */

    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
  }

  std::string_view KeyOf(const Entry &e) const
  {
    return std::string_view(arena_.data() + e.offset, e.length);
  }

  void Rehash(std::size_t slots)
  {
    if (slots <= slots_.size())
      {
        return;
      }

    slots_.assign(slots, 0);
    for (std::size_t i = 0; i < entries_.size(); i++)
      {
        std::size_t slot = Hash(KeyOf(entries_[i])) & (slots - 1);
        while (slots_[slot] != 0)
          {
            slot = (slot + 1) & (slots - 1);
          }

        slots_[slot] = static_cast<std::uint32_t>(i + 1);
      }
  }

  std::string arena_;
  std::vector<Entry> entries_;
  std::vector<std::uint32_t> slots_;
};

/****************************************************************************
 * File helpers
 ****************************************************************************/

bool ReadFile(const std::string &path, std::string *data)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);

  if (!in)
    {
      return false;
    }

  const std::streamoff size = in.tellg();
  if (size < 0)
    {
      return false;
    }

  data->resize(static_cast<std::size_t>(size));
  in.seekg(0);
  in.read(&(*data)[0], size);
  return static_cast<std::streamoff>(in.gcount()) == size;
}

/* Calls fn(line) for every line, without the trailing CR/LF. */

template <typename Fn>
void ForEachLine(const std::string &data, Fn fn)
{
  std::size_t pos = 0;

  while (pos < data.size())
    {
      std::size_t end = data.find('\n', pos);
      if (end == std::string::npos)
        {
          end = data.size();
        }

      std::size_t stop = end;
      if (stop > pos && data[stop - 1] == '\r')
        {
          stop--;
        }

      fn(std::string_view(data.data() + pos, stop - pos));
      pos = end + 1;
    }
}

void SplitFields(std::string_view line, std::vector<std::string_view> *out)
{
  std::size_t i = 0;

  out->clear();
  while (i < line.size())
    {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        {
          i++;
        }

      std::size_t j = i;
      while (j < line.size() && line[j] != ' ' && line[j] != '\t')
        {
          j++;
        }

      if (j > i)
        {
          out->push_back(line.substr(i, j - i));
        }

      i = j;
    }
}

bool ParseUint(std::string_view text, unsigned long max, unsigned long *out)
{
  unsigned long v = 0;

  if (text.empty())
    {
      return false;
    }

  for (char c : text)
    {
      if (c < '0' || c > '9')
        {
          return false;
        }

      v = v * 10 + static_cast<unsigned long>(c - '0');
      if (v > max)
        {
          return false;
        }
    }

  *out = v;
  return true;
}

/****************************************************************************
 * Punctuation
 ****************************************************************************/

enum : std::uint8_t
{
  kBreakNone = 0,
  kBreakClause = 1,   /* May split here when a sentence is over budget. */
  kBreakSentence = 2, /* Always a candidate utterance boundary. */
};

struct PunctInfo
{
  const char *symbol;
  std::uint8_t brk;
};

/* Symbol mapping follows MeloTTS text/chinese.py rep_map (the model was
 * trained on exactly these replacements); the break class implements the
 * splitter policy: 。！？；!?; first, then ，,、 (and their kin).
 */

bool LookupPunct(char32_t c, PunctInfo *info)
{
  switch (c)
    {
      case 0x3002: /* 。 */
        *info = {".", kBreakSentence};
        return true;
      case U'!':
      case 0xff01: /* ！ */
        *info = {"!", kBreakSentence};
        return true;
      case U'?':
      case 0xff1f: /* ？ */
        *info = {"?", kBreakSentence};
        return true;
      case U';':
      case 0xff1b: /* ； */
        *info = {",", kBreakSentence};
        return true;
      case U',':
      case 0xff0c: /* ， */
      case 0x3001: /* 、 */
      case U':':
        *info = {",", kBreakClause};
        return true;
      case 0x00b7: /* · between transliterated names */
        *info = {",", kBreakNone};
        return true;
      case 0x2026: /* … */
      case 0x22ef:
        *info = {"\xe2\x80\xa6", kBreakClause};
        return true;
      case 0x2014: /* — */
      case 0x2013:
      case U'~':
      case 0x301c:
        *info = {"-", kBreakClause};
        return true;
      case U'-':
        *info = {"-", kBreakNone};
        return true;
      default:
        return false;
    }
}

/* Characters that carry no sound and are not worth a "dropped" report:
 * quotes, brackets, markdown decoration from LLM output, joiners.  MeloTTS
 * maps quotes/brackets to the "'" symbol; sherpa-onnx drops them.  Dropping
 * is kept because it costs no symbols from the tight frame budget.
 */

bool IsIgnorable(char32_t c)
{
  static const char32_t kSet[] =
      U"\"'()[]{}<>*_#`|\\^"
      U"‘’“”「」『』"
      U"《》〈〉【】〔〕（）";

  /* Invisible code points are compared by value: as literals they could
   * not be reviewed.  ZWSP/ZWNJ/ZWJ, variation selector 16 (emoji), BOM.
   */

  if ((c >= 0x200b && c <= 0x200d) || c == 0xfe0e || c == 0xfe0f ||
      c == 0xfeff)
    {
      return true;
    }

  /* Emoji and pictographs.  A model told to leave them out still emits one
   * now and then, and an unknown symbol costs a "dropped" report and, in a
   * long reply, the frame budget.  Dingbats, miscellaneous symbols, the
   * supplemental planes, regional indicators and the skin tone modifiers.
   */

  if ((c >= 0x2190 && c <= 0x2bff) || (c >= 0x2e00 && c <= 0x2e7f) ||
      (c >= 0x1f000 && c <= 0x1faff) || (c >= 0x1f1e6 && c <= 0x1f1ff) ||
      (c >= 0xe0020 && c <= 0xe007f))
    {
      return true;
    }

  return Contains(kSet, c);
}

/****************************************************************************
 * English helpers
 ****************************************************************************/

struct Alias
{
  const char *word;
  const char *reading; /* Space separated lexicon words. */
};

/* Product vocabulary that is neither in the lexicon nor decomposable by the
 * generic compound splitter, and that would otherwise be spelled letter by
 * letter ("double-u i f i").  "openvela" is in here explicitly because the
 * contest wake phrase contains it and must not depend on a heuristic.
 */

const Alias kAliases[] =
{
  {"wifi", "why fi"},
  {"openvela", "open vela"},
};

} /* namespace */

/****************************************************************************
 * MeloFrontend::Impl
 ****************************************************************************/

struct MeloFrontend::Impl
{
  struct Unit
  {
    std::uint32_t sym_begin;
    std::uint32_t sym_end;
    std::uint32_t text_begin;
    std::uint32_t text_end;
    char32_t han; /* The hanzi when the unit is a single hanzi, else 0. */
    std::uint8_t brk;
    bool punct;
  };

  struct Work
  {
    std::u32string text;
    std::vector<std::uint8_t> syms;
    std::vector<std::uint8_t> tones;
    std::vector<Unit> units;
    std::vector<std::uint32_t> dropped_at; /* Ascending text positions. */
    bool speech_in_sentence = false;
  };

  struct Range
  {
    std::size_t begin;
    std::size_t end;
  };

  /* Longest word tried by both segmenters.  Same window as sherpa-onnx's
   * phrase matcher and also the longest hanzi entry in lexicon.txt.
   */

  static constexpr std::size_t kMaxWordChars = 10;

  Options options;
  LoadStats stats;
  Segmenter active = Segmenter::kForwardMaxMatch;

  std::unordered_map<std::string, std::uint8_t> tokens;
  StringTable lexicon;             /* word -> offset into pool */
  std::vector<std::uint8_t> pool;  /* n, n symbol ids, n tones */
  StringTable jieba;               /* word -> float log-probability bits */
  float min_logp = -20.0f;
  mutable std::atomic<std::uint64_t> dropped_total{0};

  bool LoadTokens(const std::string &path, std::string *error);
  bool LoadLexicon(const std::string &path, std::string *error);
  bool LoadJieba(const std::string &path);

  bool AppendPron(std::string_view key, Work *work) const;
  void AddUnit(Work *work, std::size_t sym_begin, std::size_t text_begin,
               std::size_t text_end, char32_t han) const;
  void AddPunct(Work *work, const PunctInfo &info, std::size_t text_begin,
                std::size_t text_end) const;

  void SegmentHan(Work *work, std::size_t begin, std::size_t end) const;
  void ForwardMatch(Work *work, const std::string &bytes,
                    const std::vector<std::uint32_t> &offsets,
                    std::size_t base, std::size_t begin, std::size_t end,
                    bool inside_word) const;

  bool EnglishPron(const std::string &word, bool all_caps, Work *work) const;
  bool Spell(const std::string &word, Work *work) const;
  void EmitEnglish(Work *work, std::size_t begin, std::size_t end) const;

  void Tokenize(Work *work) const;
  void ApplySandhi(Work *work) const;

  std::size_t Symbols(const Work &work, Range r) const;
  void Pack(const Work &work, Range range, int level, std::size_t budget,
            std::vector<Range> *out) const;
};

bool MeloFrontend::Impl::LoadTokens(const std::string &path,
                                    std::string *error)
{
  std::string data;

  if (!ReadFile(path, &data))
    {
      *error = "cannot read " + path;
      return false;
    }

  bool ok = true;
  ForEachLine(data, [&](std::string_view line)
    {
      /* "symbol id"; split at the LAST space so a symbol could in
       * principle be a space itself.
       */

      const std::size_t sp = line.rfind(' ');
      unsigned long id;

      if (sp == std::string_view::npos || sp == 0)
        {
          return;
        }

      /* Ids are stored as uint8_t in the pronunciation pool. */

      if (!ParseUint(line.substr(sp + 1), 255, &id))
        {
          ok = false;
          return;
        }

      tokens.emplace(std::string(line.substr(0, sp)),
                     static_cast<std::uint8_t>(id));
    });

  if (!ok || tokens.empty())
    {
      *error = "malformed tokens file " + path;
      return false;
    }

  /* Id 0 must be the blank, because blank interspersing hard-codes 0 the
   * same way the model export does.
   */

  const auto blank = tokens.find("_");
  if (blank == tokens.end() || blank->second != 0)
    {
      *error = "tokens.txt: blank symbol '_' is not id 0";
      return false;
    }

  stats.tokens = tokens.size();
  return true;
}

bool MeloFrontend::Impl::LoadLexicon(const std::string &path,
                                     std::string *error)
{
  std::string data;

  if (!ReadFile(path, &data))
    {
      *error = "cannot read " + path;
      return false;
    }

  lexicon.Reserve(200000, data.size() / 3);
  pool.reserve(data.size() / 3);

  std::vector<std::string_view> fields;
  std::vector<std::uint8_t> record;
  ForEachLine(data, [&](std::string_view line)
    {
      SplitFields(line, &fields);
      if (fields.empty())
        {
          return;
        }

      /* word, n symbols, n tones */

      if (fields.size() < 3 || (fields.size() - 1) % 2 != 0 ||
          (fields.size() - 1) / 2 > 255)
        {
          stats.lexicon_rejected++;
          return;
        }

      const std::size_t count = (fields.size() - 1) / 2;
      record.clear();
      record.push_back(static_cast<std::uint8_t>(count));
      for (std::size_t k = 0; k < count; k++)
        {
          const auto it = tokens.find(std::string(fields[1 + k]));
          if (it == tokens.end())
            {
              stats.lexicon_rejected++;
              return;
            }

          record.push_back(it->second);
        }

      for (std::size_t k = 0; k < count; k++)
        {
          unsigned long tone;
          if (!ParseUint(fields[1 + count + k], 255, &tone))
            {
              stats.lexicon_rejected++;
              return;
            }

          record.push_back(static_cast<std::uint8_t>(tone));
        }

      if (lexicon.Insert(fields[0], static_cast<std::uint32_t>(pool.size())))
        {
          pool.insert(pool.end(), record.begin(), record.end());
        }
    });

  if (lexicon.size() == 0)
    {
      *error = "no usable entry in " + path;
      return false;
    }

  lexicon.Shrink();
  pool.shrink_to_fit();
  stats.lexicon_entries = lexicon.size();
  return true;
}

bool MeloFrontend::Impl::LoadJieba(const std::string &path)
{
  std::string data;

  if (!ReadFile(path, &data))
    {
      return false;
    }

  /* Two passes: probabilities need the grand total first. */

  double total = 0;
  std::vector<std::string_view> fields;
  ForEachLine(data, [&](std::string_view line)
    {
      unsigned long freq;

      SplitFields(line, &fields);
      if (fields.size() >= 2 && ParseUint(fields[1], 4000000000ul, &freq))
        {
          total += static_cast<double>(freq);
        }
    });

  if (total <= 0)
    {
      return false;
    }

  jieba.Reserve(360000, data.size() * 2 / 3);
  std::u32string decoded;
  ForEachLine(data, [&](std::string_view line)
    {
      unsigned long freq;

      SplitFields(line, &fields);
      if (fields.size() < 2 || !ParseUint(fields[1], 4000000000ul, &freq) ||
          freq == 0)
        {
          return;
        }

      /* Only hanzi words inside the matching window can ever be asked
       * for; the rest would just hold memory.
       */

      decoded.clear();
      if (DecodeUtf8(fields[0], &decoded) != 0 || decoded.empty() ||
          decoded.size() > kMaxWordChars ||
          !std::all_of(decoded.begin(), decoded.end(), IsHan))
        {
          return;
        }

      const float logp =
          static_cast<float>(std::log(static_cast<double>(freq) / total));
      std::uint32_t bits;
      std::memcpy(&bits, &logp, sizeof(bits));
      jieba.Insert(fields[0], bits);
    });

  jieba.Shrink();
  min_logp = static_cast<float>(std::log(1.0 / total));
  stats.jieba_entries = jieba.size();
  return jieba.size() != 0;
}

/****************************************************************************
 * Unit construction
 ****************************************************************************/

bool MeloFrontend::Impl::AppendPron(std::string_view key, Work *work) const
{
  std::uint32_t offset;

  if (!lexicon.Find(key, &offset))
    {
      return false;
    }

  const std::size_t count = pool[offset];
  const std::uint8_t *ids = &pool[offset + 1];

  work->syms.insert(work->syms.end(), ids, ids + count);
  work->tones.insert(work->tones.end(), ids + count, ids + 2 * count);
  return true;
}

void MeloFrontend::Impl::AddUnit(Work *work, std::size_t sym_begin,
                                 std::size_t text_begin, std::size_t text_end,
                                 char32_t han) const
{
  Unit u;

  u.sym_begin = static_cast<std::uint32_t>(sym_begin);
  u.sym_end = static_cast<std::uint32_t>(work->syms.size());
  u.text_begin = static_cast<std::uint32_t>(text_begin);
  u.text_end = static_cast<std::uint32_t>(text_end);
  u.han = han;
  u.brk = kBreakNone;
  u.punct = false;
  work->units.push_back(u);
  work->speech_in_sentence = true;
}

void MeloFrontend::Impl::AddPunct(Work *work, const PunctInfo &info,
                                  std::size_t text_begin,
                                  std::size_t text_end) const
{
  const auto it = tokens.find(info.symbol);

  if (it == tokens.end())
    {
      return;
    }

  /* Punctuation before any speech in a sentence would only synthesise a
   * pause in front of nothing ("？！" after a split, a leading comma, or a
   * line of "。。。"), and would produce utterances that are pure silence.
   */

  if (!work->speech_in_sentence)
    {
      return;
    }

  /* "！！！" is one "!": the model never saw repeated marks in training and
   * every repeat would cost budget.  A repeat may still upgrade the break.
   */

  if (!work->units.empty() && work->units.back().punct &&
      work->syms[work->units.back().sym_begin] == it->second)
    {
      Unit &last = work->units.back();
      last.text_end = static_cast<std::uint32_t>(text_end);
      last.brk = std::max(last.brk, info.brk);
      if (last.brk == kBreakSentence)
        {
          work->speech_in_sentence = false;
        }

      return;
    }

  Unit u;
  u.sym_begin = static_cast<std::uint32_t>(work->syms.size());
  work->syms.push_back(it->second);
  work->tones.push_back(0);
  u.sym_end = static_cast<std::uint32_t>(work->syms.size());
  u.text_begin = static_cast<std::uint32_t>(text_begin);
  u.text_end = static_cast<std::uint32_t>(text_end);
  u.han = 0;
  u.brk = info.brk;
  u.punct = true;
  work->units.push_back(u);
  if (info.brk == kBreakSentence)
    {
      work->speech_in_sentence = false;
    }
}

/****************************************************************************
 * Chinese word segmentation
 ****************************************************************************/

void MeloFrontend::Impl::ForwardMatch(
    Work *work, const std::string &bytes,
    const std::vector<std::uint32_t> &offsets, std::size_t base,
    std::size_t begin, std::size_t end, bool inside_word) const
{
  std::size_t i = begin;

  while (i < end)
    {
      std::size_t len = std::min(kMaxWordChars, end - i);

      for (; len >= 1; len--)
        {
          const std::string_view key(bytes.data() + offsets[i],
                                     offsets[i + len] - offsets[i]);
          const std::size_t sym_begin = work->syms.size();

          if (AppendPron(key, work))
            {
              /* A single hanzi that ENDS a longer jieba word ("星期一",
               * "统一") is not followed by anything inside its word, so
               * it must not be offered to the sandhi pass.
               */

              const bool word_final =
                  inside_word && i + len == end && end - begin > 1;

              AddUnit(work, sym_begin, base + i, base + i + len,
                      len == 1 && !word_final ? work->text[base + i] : 0);
              break;
            }
        }

      if (len == 0)
        {
          /* Interjections the lexicon lacks are read as their homophone,
           * the same substitution MeloTTS and sherpa-onnx hard-code.
           * "嗯" is the one that matters: it is frequent in chat replies.
           */

          const char32_t c = work->text[base + i];
          const char *homophone = c == U'嗯' ? "恩" : c == U'呣' ? "母"
                                  : nullptr;
          const std::size_t sym_begin = work->syms.size();

          if (homophone != nullptr && AppendPron(homophone, work))
            {
              AddUnit(work, sym_begin, base + i, base + i + 1, 0);
            }
          else
            {
              /* Hanzi without a lexicon entry (rare / variant form). */

              work->dropped_at.push_back(
                  static_cast<std::uint32_t>(base + i));
            }

          len = 1;
        }

      i += len;
    }
}

void MeloFrontend::Impl::SegmentHan(Work *work, std::size_t begin,
                                    std::size_t end) const
{
  const std::size_t n = end - begin;
  std::string bytes;
  std::vector<std::uint32_t> offsets(n + 1);

  /* One UTF-8 copy of the run plus per-character byte offsets, so every
   * candidate word is a string_view and lookups allocate nothing.
   */

  bytes.reserve(n * 3);
  for (std::size_t i = 0; i < n; i++)
    {
      offsets[i] = static_cast<std::uint32_t>(bytes.size());
      AppendUtf8(&bytes, work->text[begin + i]);
    }

  offsets[n] = static_cast<std::uint32_t>(bytes.size());

  if (active != Segmenter::kFewestWords)
    {
      ForwardMatch(work, bytes, offsets, begin, 0, n, false);
      return;
    }

  /* Shortest path, right to left: the fewest words win and only ties are
   * decided by the jieba unigram probability.
   *
   * Plain max-probability (jieba MPSegment) was tried first and rejected:
   * it cuts "不对" (freq 695) into the far more frequent "不" + "对", which
   * throws away the lexicon entry that carries the sandhi (bu2 dui4).  For
   * G2P a multi-character lexicon word is always the better reading, so
   * word count comes first.  Probability is still what resolves overlaps
   * of equal length: "要不/要" and "要/不要" are both two words, and
   * P(不要) >> P(要不) picks the right one where forward matching cannot.
   *
   * Candidates are jieba words with their corpus probability plus
   * lexicon-only words at the floor probability.
   */

  std::vector<float> best(n + 1, 0.0f);
  std::vector<std::uint32_t> words(n + 1, 0);
  std::vector<std::uint8_t> step(n + 1, 1);

  for (std::size_t i = n; i-- > 0; )
    {
      const std::size_t max_len = std::min(kMaxWordChars, n - i);
      float top = -std::numeric_limits<float>::infinity();
      std::uint32_t fewest = std::numeric_limits<std::uint32_t>::max();

      for (std::size_t len = 1; len <= max_len; len++)
        {
          const std::string_view key(bytes.data() + offsets[i],
                                     offsets[i + len] - offsets[i]);
          std::uint32_t bits;
          float logp;

          if (jieba.Find(key, &bits))
            {
              std::memcpy(&logp, &bits, sizeof(logp));
            }
          else if (len == 1 || lexicon.Has(key))
            {
              logp = min_logp;
            }
          else
            {
              continue;
            }

          const std::uint32_t count = words[i + len] + 1;
          const float score = logp + best[i + len];

          if (count < fewest || (count == fewest && score > top))
            {
              fewest = count;
              top = score;
              step[i] = static_cast<std::uint8_t>(len);
            }
        }

      best[i] = top;
      words[i] = fewest;
    }

  for (std::size_t i = 0; i < n; i += step[i])
    {
      const std::size_t len = step[i];
      const std::string_view key(bytes.data() + offsets[i],
                                 offsets[i + len] - offsets[i]);
      const std::size_t sym_begin = work->syms.size();

      if (AppendPron(key, work))
        {
          AddUnit(work, sym_begin, begin + i, begin + i + len,
                  len == 1 ? work->text[begin + i] : 0);
        }
      else
        {
          /* A jieba word the lexicon does not know ("一天"): pronounce it
           * from the longest lexicon pieces inside the word.
           */

          ForwardMatch(work, bytes, offsets, begin, i, i + len, true);
        }
    }
}

/****************************************************************************
 * English
 ****************************************************************************/

bool MeloFrontend::Impl::Spell(const std::string &word, Work *work) const
{
  bool any = false;

  for (char c : word)
    {
      if (c == 'a' && AppendPron("a(2)", work))
        {
          /* The plain entry "a" is the article (ah); the letter NAME (ey)
           * is the CMUdict variant "a(2)".  Every other letter has its
           * name as the main entry.
           */

          any = true;
        }
      else if (c >= 'a' && c <= 'z')
        {
          any |= AppendPron(std::string_view(&c, 1), work);
        }
    }

  return any;
}

bool MeloFrontend::Impl::EnglishPron(const std::string &word, bool all_caps,
                                     Work *work) const
{
  for (const Alias &alias : kAliases)
    {
      if (word == alias.word)
        {
          const std::size_t mark = work->syms.size();
          std::vector<std::string_view> parts;
          bool ok = true;

          SplitFields(alias.reading, &parts);
          for (std::string_view part : parts)
            {
              ok = ok && AppendPron(part, work);
            }

          if (ok)
            {
              return true;
            }

          /* A different lexicon may lack a part: undo and fall through. */

          work->syms.resize(mark);
          work->tones.resize(mark);
        }
    }

  /* Short all-caps tokens are acronyms (AI, NPU, USB).  The lexicon often
   * holds an unrelated word under the same letters ("ai" -> "ay").
   */

  if (all_caps && word.size() >= 2 && word.size() <= 4)
    {
      return Spell(word, work);
    }

  if (AppendPron(word, work))
    {
      return true;
    }

  const std::size_t hyphen = word.find('-');
  if (hyphen != std::string::npos)
    {
      const bool a = EnglishPron(word.substr(0, hyphen), false, work);
      const bool b = EnglishPron(word.substr(hyphen + 1), false, work);
      return a || b;
    }

  const std::size_t apostrophe = word.find('\'');
  if (apostrophe != std::string::npos)
    {
      const std::string head = word.substr(0, apostrophe);
      const std::string tail = word.substr(apostrophe + 1);

      if (tail == "s" && !head.empty())
        {
          /* Possessive of a word the lexicon only has in base form. */

          const auto s = tokens.find("s");
          const bool ok = EnglishPron(head, false, work);

          if (ok && s != tokens.end())
            {
              work->syms.push_back(s->second);
              work->tones.push_back(7); /* Lexicon tone of a consonant. */
            }

          return ok;
        }

      const bool a = !head.empty() && EnglishPron(head, false, work);
      const bool b = !tail.empty() && EnglishPron(tail, false, work);
      return a || b;
    }

  /* Unknown compound ("bluetooth" -> "blue" + "tooth"): fewest lexicon
   * pieces of at least three letters.  Shorter pieces are excluded because
   * almost any letter pair is some lexicon entry, which would turn every
   * unknown word into confident nonsense instead of honest spelling.
   */

  const std::size_t n = word.size();
  if (n >= 6 && n <= 32)
    {
      constexpr unsigned kNone = 255;
      constexpr unsigned kMaxPieces = 3;
      std::vector<std::uint8_t> count(n + 1, kNone);
      std::vector<std::uint8_t> piece(n + 1, 0);

      count[n] = 0;
      for (std::size_t i = n - 3 + 1; i-- > 0; )
        {
          for (std::size_t len = n - i; len >= 3; len--)
            {
              if (count[i + len] != kNone &&
                  count[i + len] + 1u < count[i] &&
                  lexicon.Has(std::string_view(word.data() + i, len)))
                {
                  count[i] = static_cast<std::uint8_t>(count[i + len] + 1);
                  piece[i] = static_cast<std::uint8_t>(len);
                }
            }
        }

      if (count[0] != kNone && count[0] <= kMaxPieces)
        {
          for (std::size_t i = 0; i < n; i += piece[i])
            {
              AppendPron(std::string_view(word.data() + i, piece[i]), work);
            }

          return true;
        }
    }

  return Spell(word, work);
}

void MeloFrontend::Impl::EmitEnglish(Work *work, std::size_t begin,
                                     std::size_t end) const
{
  std::string word;
  bool all_caps = true;

  for (std::size_t i = begin; i < end; i++)
    {
      const char32_t c = work->text[i];

      if (c >= U'A' && c <= U'Z')
        {
          word.push_back(static_cast<char>(c - U'A' + U'a'));
        }
      else if (c >= U'a' && c <= U'z')
        {
          word.push_back(static_cast<char>(c));
          all_caps = false;
        }
      else
        {
          word.push_back(c == U'-' ? '-' : '\'');
          all_caps = false;
        }
    }

  const std::size_t sym_begin = work->syms.size();
  if (EnglishPron(word, all_caps, work))
    {
      AddUnit(work, sym_begin, begin, end, 0);
    }
  else
    {
      for (std::size_t i = begin; i < end; i++)
        {
          work->dropped_at.push_back(static_cast<std::uint32_t>(i));
        }
    }
}

/****************************************************************************
 * Tokenizer: normalised text -> units
 ****************************************************************************/

void MeloFrontend::Impl::Tokenize(Work *work) const
{
  const std::u32string &t = work->text;
  const std::size_t n = t.size();
  std::size_t i = 0;

  while (i < n)
    {
      const char32_t c = t[i];
      PunctInfo info;

      if (IsHan(c))
        {
          std::size_t j = i + 1;
          while (j < n && IsHan(t[j]))
            {
              j++;
            }

          SegmentHan(work, i, j);
          i = j;
        }
      else if (IsAsciiAlpha(c))
        {
          /* Apostrophes and hyphens belong to the word only when letters
           * follow ("don't", "wi-fi"), not as quotes or dashes.
           */

          std::size_t j = i + 1;
          while (j < n &&
                 (IsAsciiAlpha(t[j]) ||
                  ((t[j] == U'\'' || t[j] == 0x2019 || t[j] == U'-') &&
                   j + 1 < n && IsAsciiAlpha(t[j + 1]))))
            {
              j++;
            }

          EmitEnglish(work, i, j);
          i = j;
        }
      else if (c == U'\n')
        {
          /* A line end closes the sentence even without punctuation
           * (titles, list items); MeloTTS maps it to "." as well.
           */

          if (work->speech_in_sentence)
            {
              if (!work->units.empty() && work->units.back().punct)
                {
                  work->units.back().brk = kBreakSentence;
                  work->speech_in_sentence = false;
                }
              else
                {
                  AddPunct(work, {".", kBreakSentence}, i, i);
                }
            }

          i++;
        }
      else if (IsSpace(c))
        {
          i++;
        }
      else if (c == U'.')
        {
          std::size_t j = i + 1;
          while (j < n && t[j] == U'.')
            {
              j++;
            }

          if (j - i >= 2)
            {
              AddPunct(work, {"\xe2\x80\xa6", kBreakClause}, i, j);
            }
          else if (i > 0 && IsAsciiAlnum(t[i - 1]) && j < n &&
                   IsAsciiAlnum(t[j]))
            {
              /* "openvela.com", "e.g": a separator, not a sentence end. */
            }
          else
            {
              AddPunct(work, {".", kBreakSentence}, i, j);
            }

          i = j;
        }
      else if (LookupPunct(c, &info))
        {
          AddPunct(work, info, i, i + 1);
          i++;
        }
      else if (IsIgnorable(c))
        {
          i++;
        }
      else
        {
          work->dropped_at.push_back(static_cast<std::uint32_t>(i));
          i++;
        }
    }
}

/* Only the two characters whose tone depends on the NEXT syllable and that
 * are frequent enough to be audible when wrong.  Multi-character lexicon
 * entries already contain their sandhi, so only single-hanzi units change.
 */

void MeloFrontend::Impl::ApplySandhi(Work *work) const
{
  static const char32_t kNumerals[] = U"零一二三四五六七八九十百千万亿两第点";
  std::vector<Unit> &units = work->units;

  for (std::size_t k = 0; k + 1 < units.size(); k++)
    {
      const Unit &u = units[k];
      const Unit &next = units[k + 1];

      if ((u.han != U'一' && u.han != U'不') || next.punct ||
          next.text_begin != u.text_end || next.sym_begin == next.sym_end)
        {
          continue;
        }

      /* Tone of the following syllable; its first symbol carries it. */

      const std::uint8_t next_tone = work->tones[next.sym_begin];
      std::uint8_t tone = 0;

      if (u.han == U'不')
        {
          tone = next_tone == 4 ? 2 : 0;
        }
      else
        {
          /* "一" inside numbers, ordinals and dates keeps yi1. */

          const bool numeric =
              Contains(kNumerals, next.han) ||
              (k > 0 && units[k - 1].text_end == u.text_begin &&
               Contains(kNumerals, units[k - 1].han));

          if (!numeric)
            {
              tone = next_tone == 4 ? 2
                     : (next_tone >= 1 && next_tone <= 3) ? 4 : 0;
            }
        }

      if (tone != 0)
        {
          for (std::size_t s = u.sym_begin; s < u.sym_end; s++)
            {
              work->tones[s] = tone;
            }
        }
    }
}

/****************************************************************************
 * Sentence splitter
 ****************************************************************************/

std::size_t MeloFrontend::Impl::Symbols(const Work &work, Range r) const
{
  return r.begin == r.end ? 0
         : work.units[r.end - 1].sym_end - work.units[r.begin].sym_begin;
}

/* Greedy packing of "atoms" into utterances.  Atoms at level 2 are
 * sentences, at level 1 clauses, at level 0 single words.  An atom over the
 * budget is re-packed one level down; at the lowest allowed level it is
 * emitted whole, because a truncated sentence is worse than a refused one
 * and the caller can see the over_budget flag.
 */

void MeloFrontend::Impl::Pack(const Work &work, Range range, int level,
                              std::size_t budget,
                              std::vector<Range> *out) const
{
  const int min_level = options.split_at_words ? 0 : 1;
  const bool merge = level < 2 || options.merge_sentences;
  Range current = {range.begin, range.begin};
  std::size_t atom_begin = range.begin;

  auto flush = [&]()
    {
      if (current.end > current.begin)
        {
          out->push_back(current);
        }
    };

  for (std::size_t k = range.begin; k < range.end; k++)
    {
      /* At word level a punctuation mark stays glued to the word before it,
       * so that no utterance starts with a comma.
       */

      const bool boundary =
          k + 1 == range.end ||
          (work.units[k].brk >= level &&
           !(level == 0 && work.units[k + 1].punct));

      if (!boundary)
        {
          continue;
        }

      const Range atom = {atom_begin, k + 1};
      const std::size_t size = Symbols(work, atom);
      atom_begin = k + 1;

      if (budget != 0 && size > budget)
        {
          flush();
          current = {atom.end, atom.end};
          if (level > min_level)
            {
              Pack(work, atom, level - 1, budget, out);
            }
          else
            {
              out->push_back(atom);
            }

          continue;
        }

      if (current.end > current.begin && merge &&
          (budget == 0 || Symbols(work, current) + size <= budget))
        {
          current.end = atom.end;
        }
      else
        {
          flush();
          current = atom;
        }
    }

  flush();
}

/****************************************************************************
 * Public interface
 ****************************************************************************/

MeloFrontend::MeloFrontend(std::unique_ptr<Impl> impl)
  : impl_(std::move(impl))
{
}

MeloFrontend::~MeloFrontend() = default;

std::unique_ptr<MeloFrontend> MeloFrontend::Load(const std::string &dir,
                                                 std::string *error)
{
  return Load(dir, error, Options());
}

std::unique_ptr<MeloFrontend> MeloFrontend::Load(const std::string &dir,
                                                 std::string *error,
                                                 const Options &options)
{
  const auto start = std::chrono::steady_clock::now();
  std::unique_ptr<Impl> impl(new Impl);
  std::string local_error;

  if (error == nullptr)
    {
      error = &local_error;
    }

  impl->options = options;
  if (!(options.frames_per_symbol > 0))
    {
      *error = "frames_per_symbol must be positive";
      return nullptr;
    }

  if (!impl->LoadTokens(dir + "/tokens.txt", error) ||
      !impl->LoadLexicon(dir + "/lexicon.txt", error))
    {
      return nullptr;
    }

  if (options.segmenter != Segmenter::kForwardMaxMatch)
    {
      if (impl->LoadJieba(dir + "/dict/jieba.dict.utf8"))
        {
          impl->active = Segmenter::kFewestWords;
        }
      else if (options.segmenter == Segmenter::kFewestWords)
        {
          *error = "cannot read " + dir + "/dict/jieba.dict.utf8";
          return nullptr;
        }
    }

  impl->stats.load_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start).count();
  return std::unique_ptr<MeloFrontend>(new MeloFrontend(std::move(impl)));
}

std::size_t MeloFrontend::SymbolBudget(std::size_t max_frames_hint) const
{
  if (max_frames_hint == 0)
    {
      return 0;
    }

  const double budget =
      static_cast<double>(max_frames_hint) / impl_->options.frames_per_symbol;

  /* Never 0: that value means "unlimited" to the packer. */

  return budget < 1.0 ? 1 : static_cast<std::size_t>(budget);
}

std::string MeloFrontend::Normalize(const std::string &utf8_text)
{
  std::u32string raw;

  DecodeUtf8(utf8_text, &raw);
  const std::u32string normalized = NormalizeU32(raw);
  return EncodeUtf8(normalized, 0, normalized.size());
}

std::vector<MeloFrontend::Utterance>
MeloFrontend::Process(const std::string &utf8_text,
                      std::size_t max_frames_hint) const
{
  std::vector<Utterance> result;
  Impl::Work work;
  std::u32string raw;

  const std::size_t invalid = DecodeUtf8(utf8_text, &raw);
  work.text = NormalizeU32(raw);
  impl_->Tokenize(&work);
  if (impl_->options.yi_bu_sandhi)
    {
      impl_->ApplySandhi(&work);
    }

  const std::size_t dropped = invalid + work.dropped_at.size();
  impl_->dropped_total.fetch_add(dropped, std::memory_order_relaxed);

  /* A trailing clause mark with nothing after it ("你好，") stays: it is
   * what the user typed and it shapes the final intonation.
   */

  const std::size_t budget = SymbolBudget(max_frames_hint);
  std::vector<Impl::Range> ranges;
  impl_->Pack(work, {0, work.units.size()}, 2, budget, &ranges);

  std::size_t next_drop = 0;
  result.reserve(ranges.size());
  for (std::size_t r = 0; r < ranges.size(); r++)
    {
      const Impl::Unit &first = work.units[ranges[r].begin];
      const Impl::Unit &last = work.units[ranges[r].end - 1];
      Utterance utt;

      utt.symbols = last.sym_end - first.sym_begin;
      utt.over_budget = budget != 0 && utt.symbols > budget;
      utt.text = EncodeUtf8(work.text, first.text_begin, last.text_end);

      /* add_blank = 1 in the model metadata: a blank before the first
       * symbol and after every symbol, for phonemes and tones alike.
       */

      utt.phonemes.assign(2 * utt.symbols + 1, 0);
      utt.tones.assign(2 * utt.symbols + 1, 0);
      for (std::size_t k = 0; k < utt.symbols; k++)
        {
          utt.phonemes[2 * k + 1] = work.syms[first.sym_begin + k];
          utt.tones[2 * k + 1] = work.tones[first.sym_begin + k];
        }

      /* Drops are attributed to the utterance whose text they precede;
       * the last utterance also takes whatever trails it.
       */

      const bool is_last = r + 1 == ranges.size();
      while (next_drop < work.dropped_at.size() &&
             (is_last || work.dropped_at[next_drop] < last.text_end))
        {
          utt.dropped++;
          next_drop++;
        }

      if (r == 0)
        {
          utt.dropped += invalid;
        }

      result.push_back(std::move(utt));
    }

  return result;
}

const MeloFrontend::LoadStats &MeloFrontend::load_stats() const
{
  return impl_->stats;
}

MeloFrontend::Segmenter MeloFrontend::active_segmenter() const
{
  return impl_->active;
}

std::uint64_t MeloFrontend::dropped_total() const
{
  return impl_->dropped_total.load(std::memory_order_relaxed);
}

} /* namespace nyamp */
