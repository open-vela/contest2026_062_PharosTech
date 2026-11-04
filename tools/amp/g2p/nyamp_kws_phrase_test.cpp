/****************************************************************************
 * tools/amp/g2p/nyamp_kws_phrase_test.cpp
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
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

int main(int argc, char **argv)
{
  char name[] = "/tmp/nyamp-wake-phrase-XXXXXX";
  const char *dir = mkdtemp(name);
  assert(dir != nullptr);
  const std::string root = dir;
  std::ofstream(root + "/tokens.txt")
      << "_ 0\nn 1\ni 2\nh 3\nao 4\nx 5\niao 6\nm 7\nhh 8\nah 9\nl 10\now "
         "11\nk 12\nih 13\nt 14\niy 15\n. 16\n, 17\n! 18\n? 19\nSP 20\nUNK "
         "21\n' 22\n- 23\n… 24\n";
  std::ofstream(root + "/lexicon.txt")
      << "你好 n i h ao 3 3 3 3\n小 x iao 3 3\n喵 m iao 1 1\nhello hh ah l ow "
         "7 8 7 9\nkitty k ih t iy 7 9 7 8\n";
  std::ofstream(root + "/kws.txt")
      << "n 0\nǐ 1\nh 2\nǎo 3\nx 4\niǎo 5\nm 6\niāo 7\nHH 8\nAH0 9\nL 10\nOW1 "
         "11\nK 12\nIH1 13\nT 14\nIY0 15\n";
  std::string line, error;
  assert(nyamp::CompileWakePhrase(root, root + "/kws.txt", "你好小喵", &line,
                                  &error));
  assert(line == "n ǐ h ǎo x iǎo m iāo @custom");
  assert(nyamp::CompileWakePhrase(root, root + "/kws.txt", "hello kitty",
                                  &line, &error));
  assert(line == "HH AH0 L OW1 K IH1 T IY0 @custom");
  assert(!nyamp::CompileWakePhrase(root, root + "/kws.txt", "你好🙂",
                                   &line, &error));
  assert(
      !nyamp::CompileWakePhrase(root, root + "/kws.txt", "", &line, &error));
  std::puts("KWS_PHRASE_PASS chinese_tones english_stress no_silent_drops");
  if (argc == 3)
    {
      assert(nyamp::CompileWakePhrase(argv[1], argv[2], "小喵小喵", &line,
                                      &error));
      std::printf("KWS_PHRASE_REAL_ASSETS_PASS %s\n", line.c_str());
    }
  unlink((root + "/tokens.txt").c_str());
  unlink((root + "/lexicon.txt").c_str());
  unlink((root + "/kws.txt").c_str());
  rmdir(root.c_str());
}
