/****************************************************************************
 * tools/amp/g2p/nyamp_kws_phrase.cpp
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

#include "nyamp_kws_phrase.h"
#include "nyamp_g2p.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace nyamp
{
namespace
{
bool WakeText(const std::string &text)
{
  bool speech = false;
  for (std::size_t i = 0; i < text.size();)
    {
      const auto lead = static_cast<unsigned char>(text[i]);
      if (lead < 128)
        {
          if (lead != ' ' && !(lead >= 'a' && lead <= 'z') &&
              !(lead >= 'A' && lead <= 'Z'))
            return false;
          speech |= lead != ' ';
          i++;
          continue;
        }
      std::size_t count = lead >= 0xc2 && lead <= 0xdf   ? 2
                          : lead >= 0xe0 && lead <= 0xef ? 3
                          : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                         : 0;
      if (count == 0 || i + count > text.size())
        return false;
      std::uint32_t cp = lead & (count == 2 ? 31 : count == 3 ? 15 : 7);
      for (std::size_t j = 1; j < count; j++)
        {
          auto byte = static_cast<unsigned char>(text[i + j]);
          if ((byte & 0xc0) != 0x80)
            return false;
          cp = (cp << 6) | (byte & 63);
        }
      if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) ||
          (count == 4 && cp < 0x10000) || cp > 0x10ffff)
        return false;
      if (!((cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x4e00 && cp <= 0x9fff) ||
            (cp >= 0x20000 && cp <= 0x2fa1f)))
        return false;
      speech = true;
      i += count;
    }
  return speech;
}

std::string KwsPhone(std::string phone, std::int64_t tone)
{
  if (tone >= 7)
    {
      std::transform(
          phone.begin(), phone.end(), phone.begin(),
          [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      if (tone >= 8 && tone <= 10)
        phone += std::to_string(tone - 8);
      return phone;
    }
  if (phone == "i0")
    phone = "i";
  if (phone.find('v') != std::string::npos)
    phone.replace(phone.find('v'), 1, "ü");
  if (tone < 1 || tone > 4)
    return phone;
  static const char *const vowels[] = { "a", "e", "i", "o", "u", "ü" };
  static const char *const marks[][4] = {
    { "ā", "á", "ǎ", "à" }, { "ē", "é", "ě", "è" }, { "ī", "í", "ǐ", "ì" },
    { "ō", "ó", "ǒ", "ò" }, { "ū", "ú", "ǔ", "ù" }, { "ǖ", "ǘ", "ǚ", "ǜ" }
  };
  std::size_t position = std::string::npos;
  unsigned int vowel = 0;
  if ((position = phone.find('a')) != std::string::npos)
    vowel = 0;
  else if ((position = phone.find('e')) != std::string::npos)
    vowel = 1;
  else if ((position = phone.find("ou")) != std::string::npos)
    vowel = 3;
  else
    for (unsigned int i = 0; i < 6; i++)
      {
        const auto at = phone.rfind(vowels[i]);
        if (at != std::string::npos &&
            (position == std::string::npos || at > position))
          {
            position = at;
            vowel = i;
          }
      }
  if (position != std::string::npos)
    phone.replace(position, std::string(vowels[vowel]).size(),
                  marks[vowel][tone - 1]);
  return phone;
}
}

bool CompileWakePhrase(const std::string &g2p_directory,
                       const std::string &kws_tokens, const std::string &text,
                       std::string *line, std::string *error)
{
  if (line == nullptr || error == nullptr)
    return false;
  if (!WakeText(text))
    {
      *error = "use Chinese or English letters and spaces for the wake phrase";
      line->clear();
      return false;
    }
  line->clear();
  error->clear();
  std::ifstream inventory(kws_tokens);
  if (!inventory)
    {
      *error = "keyword token inventory is missing";
      return false;
    }
  std::unordered_set<std::string> allowed;
  std::string row;
  while (std::getline(inventory, row))
    {
      std::istringstream words(row);
      std::string token;
      if (words >> token)
        allowed.insert(token);
    }
  auto frontend = MeloFrontend::Load(g2p_directory, error);
  if (!frontend)
    return false;
  const auto utterances = frontend->Process(text, 0);
  for (const auto &utterance : utterances)
    {
      if (utterance.dropped != 0 ||
          utterance.phonemes.size() != utterance.tones.size())
        {
          *error = "wake phrase contains unsupported characters";
          return false;
        }
      for (std::size_t i = 0; i < utterance.phonemes.size(); i++)
        {
          if (utterance.phonemes[i] == 0)
            continue;
          const auto symbol = frontend->SymbolName(utterance.phonemes[i]);
          if (symbol == "SP" || symbol == "." || symbol == "," ||
              symbol == "!" || symbol == "?" || symbol == "…" ||
              symbol == "'" || symbol == "-")
            continue;
          const auto phone = KwsPhone(symbol, utterance.tones[i]);
          if (allowed.find(phone) == allowed.end())
            {
              *error = "wake phrase pronunciation is not in the keyword model";
              return false;
            }
          if (!line->empty())
            *line += ' ';
          *line += phone;
        }
    }
  if (line->empty())
    {
      *error = "wake phrase contains no supported speech";
      return false;
    }
  *line += " @custom";
  return true;
}
}
