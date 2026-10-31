/****************************************************************************
 * tools/amp/g2p/nyamp_g2p.h
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

/* Text front end (text normalisation + word segmentation + G2P + sentence
 * splitter) for the MeloTTS zh_en VITS model as packaged by sherpa-onnx.
 *
 * The target is a Linux aarch64 daemon without Python, so everything the
 * Python/sherpa-onnx front ends do with jieba, OpenFst and regexes is
 * re-implemented here on top of the C++17 standard library only.
 */

#ifndef __TOOLS_AMP_G2P_NYAMP_G2P_H
#define __TOOLS_AMP_G2P_NYAMP_G2P_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nyamp
{

class MeloFrontend
{
public:
  enum class Segmenter
  {
    /* kFewestWords when dict/jieba.dict.utf8 is present, else
     * kForwardMaxMatch.
     */

    kAuto,

    /* Forward maximum matching over lexicon.txt (window of 10 characters).
     * This is what sherpa-onnx >= 1.12 does (phrase-matcher.cc), so it is
     * the mode to use when bit-parity with sherpa-onnx matters.
     */

    kForwardMaxMatch,

    /* Fewest words over the union of the jieba dictionary and the
     * lexicon, ties broken by the jieba unigram probability.  It resolves
     * overlaps such as "要不要" -> "要/不要" (bu2) that forward matching
     * gets wrong ("要不/要", bu4), and it is what the golden fixture
     * encodes.
     */

    kFewestWords,
  };

  struct Options
  {
    Segmenter segmenter = Segmenter::kAuto;

    /* Latent frames consumed per phoneme symbol (blanks NOT counted).  The
     * golden fixture measures 342 frames / 47 symbols = 7.28; host runs of
     * the full model on 21 utterances gave 6.8..7.9 for 45..76 symbols and
     * up to 8.1 for 30..40 symbols (README.md has the table).  9.0 yields a
     * budget of 56 symbols for the 512 frame bucket, i.e. >= 14 % headroom
     * over the worst measured ratio.  It is a heuristic: the authoritative
     * check is the latent length the encoder actually returns.
     */

    double frames_per_symbol = 9.0;

    /* Pack consecutive short sentences into one utterance while they fit
     * the budget.  The vocoder bucket has a fixed cost, so fewer, fuller
     * utterances are cheaper; it is also how the golden fixture was made
     * (three sentences, one utterance).
     */

    bool merge_sentences = true;

    /* When a clause without any comma is still over budget, split it
     * between words instead of refusing it.  Prosody at such a cut is
     * worse than at punctuation, but the text is still spoken.
     */

    bool split_at_words = true;

    /* Tone sandhi for stand-alone "一" / "不" (multi-character lexicon
     * entries already carry their sandhi).  Off by default because neither
     * sherpa-onnx nor the golden fixture applies it ("一天" stays yi1).
     */

    bool yi_bu_sandhi = false;
  };

  struct Utterance
  {
    /* Model inputs "x" and "tones": blank (0) interspersed, 2 * n + 1. */

    std::vector<std::int64_t> phonemes;
    std::vector<std::int64_t> tones;

    /* Normalised text this utterance was produced from (for logs). */

    std::string text;

    /* Number of phoneme symbols before blank insertion (budget unit). */

    std::size_t symbols = 0;

    /* Code points that produced no phoneme (emoji, rare hanzi, ...). */

    std::size_t dropped = 0;

    /* True when the utterance could not be split below the budget.  The
     * ids are complete (never truncated); the caller must refuse it or
     * synthesise it through a path without the fixed bucket.
     */

    bool over_budget = false;
  };

  struct LoadStats
  {
    std::size_t tokens = 0;
    std::size_t lexicon_entries = 0;
    std::size_t lexicon_rejected = 0; /* malformed / unknown symbol lines */
    std::size_t jieba_entries = 0;
    double load_ms = 0;
  };

  ~MeloFrontend();

  /* dir must contain tokens.txt and lexicon.txt; dir/dict/jieba.dict.utf8
   * is optional (see Segmenter).  Returns nullptr and fills *error on
   * failure.
   */

  static std::unique_ptr<MeloFrontend> Load(const std::string &dir,
                                            std::string *error);
  static std::unique_ptr<MeloFrontend>
  Load(const std::string &dir, std::string *error, const Options &options);

  /* max_frames_hint is the latent frame capacity of the synthesis bucket
   * (512 for the masked vocoder); 0 disables the budget.  Thread safe.
   */

  std::vector<Utterance> Process(const std::string &utf8_text,
                                 std::size_t max_frames_hint) const;

  /* Phoneme symbols allowed per utterance for a given frame capacity. */

  std::size_t SymbolBudget(std::size_t max_frames_hint) const;

  /* Text normalisation only (numbers, dates, times, units -> hanzi).  It
   * needs no model assets, hence static.
   */

  static std::string Normalize(const std::string &utf8_text);

  const LoadStats &load_stats() const;
  Segmenter active_segmenter() const;

  /* Dropped code points summed over every Process() call, for metrics. */

  std::uint64_t dropped_total() const;

private:
  struct Impl;
  explicit MeloFrontend(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} /* namespace nyamp */

#endif /* __TOOLS_AMP_G2P_NYAMP_G2P_H */
