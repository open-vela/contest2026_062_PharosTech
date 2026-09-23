/****************************************************************************
 * tools/amp/chat/nyamp_unicode.cpp
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

#include "nyamp_unicode.h"

#include <cstddef>

namespace nyamp
{

namespace
{

#include "nyamp_unicode_tables.inc"

template <size_t N> bool in_ranges(const uint32_t (&table)[N][2], uint32_t cp)
{
  size_t lo = 0;
  size_t hi = N;
  while (lo < hi)
    {
      size_t mid = (lo + hi) / 2;
      if (cp < table[mid][0])
        {
          hi = mid;
        }
      else if (cp > table[mid][1])
        {
          lo = mid + 1;
        }
      else
        {
          return true;
        }
    }
  return false;
}

// ASCII is most of every prompt's scaffolding, so it bypasses the binary
// search.  Built once from the same tables so the two can never disagree.
struct AsciiClasses
{
  uint8_t value[128];
  AsciiClasses()
  {
    for (uint32_t c = 0; c < 128; ++c)
      {
        uint8_t bits = 0;
        if (in_ranges(kUnicodeLetter, c))
          bits |= kClassLetter;
        if (in_ranges(kUnicodeNumber, c))
          bits |= kClassNumber;
        if (in_ranges(kUnicodeSpace, c))
          bits |= kClassSpace;
        if (c == '\r' || c == '\n')
          bits |= kClassNewline;
        value[c] = bits;
      }
  }
};

} // namespace

uint8_t unicode_classify(uint32_t cp)
{
  static const AsciiClasses ascii;
  if (cp < 128)
    return ascii.value[cp];
  // The common CJK block is one compare away; without this shortcut every
  // Chinese character would pay ten binary-search steps.
  if (cp >= 0x4E00 && cp <= 0x9FFF)
    return kClassLetter;
  if (in_ranges(kUnicodeLetter, cp))
    return kClassLetter;
  if (in_ranges(kUnicodeNumber, cp))
    return kClassNumber;
  if (in_ranges(kUnicodeSpace, cp))
    return kClassSpace;
  return 0;
}

bool unicode_is_python_printable(uint32_t cp)
{
  if (cp < 0x80)
    return cp >= 0x20 && cp != 0x7F;
  return !in_ranges(kPythonNonPrintable, cp);
}

} // namespace nyamp
