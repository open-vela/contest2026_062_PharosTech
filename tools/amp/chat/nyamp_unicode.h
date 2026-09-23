/****************************************************************************
 * tools/amp/chat/nyamp_unicode.h
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

// Unicode classes needed by the pre-tokenizer regex.  The tables are measured
// from the reference regex engine (see tests/gen_unicode_tables.py) instead
// of linking ICU: three range lists cost ~7 KB, ICU costs ~30 MB.

#ifndef NYAMP_UNICODE_H
#define NYAMP_UNICODE_H

#include <cstdint>

namespace nyamp
{

enum UnicodeClass : uint8_t
{
  kClassLetter = 1,  // \p{L}
  kClassNumber = 2,  // \p{N}
  kClassSpace = 4,   // \s
  kClassNewline = 8, // \r or \n
};

// Bit set of UnicodeClass for one code point.
uint8_t unicode_classify(uint32_t cp);

// Python str.isprintable() for one code point; drives repr() escaping.
bool unicode_is_python_printable(uint32_t cp);

} // namespace nyamp

#endif // NYAMP_UNICODE_H
