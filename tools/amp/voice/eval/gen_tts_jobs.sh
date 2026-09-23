#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Writes the TTS job lists for the SYNTHETIC wake-word evidence set.
# Usage: gen_tts_jobs.sh OUTPUT_ROOT
# Produces OUTPUT_ROOT/jobs-{kokoro,melo,aishell3}.tsv whose wav paths live
# under OUTPUT_ROOT/raw/{pos_zh,pos_en,neg}/.
#
# Why these choices:
# - Kokoro multi-lang v1.1 is the only small offline model that speaks mixed
#   Chinese/English with many voices (3 English, 100 Chinese), which is the
#   closest stand-in for Chinese users saying an English product name.
# - Melo zh_en adds one more voice with a different vocoder.
# - AISHELL-3 VITS is Chinese-only and 8 kHz, so it says the transliteration
#   and is reported separately as a band-limited stress condition.
# - Confusable negatives share a prefix ("你好 ...", "open ...") because a
#   keyword spotter fails first on partial matches, not on unrelated speech.

set -eu
root=$1
mkdir -p "$root/raw/pos_zh" "$root/raw/pos_en" "$root/raw/neg"

kokoro_sids="0 1 2 3 6 9 12 15 18 21 24 27 30 33 36 39 42 45 48 51 54 57 \
58 61 64 67 70 73 76 79 82 85 88 91 94 97 100"
speeds="0.85 1.0 1.2"

: > "$root/jobs-kokoro.tsv"
for sid in $kokoro_sids; do
  for speed in $speeds; do
    tag=$(printf 'kokoro-s%03d-v%s' "$sid" "$(echo "$speed" | tr -d .)")
    printf '%s\t%s\t%s\t%s\n' "$sid" "$speed" \
      "$root/raw/pos_zh/$tag-zhA.wav" "你好，open vela" >> "$root/jobs-kokoro.tsv"
    printf '%s\t%s\t%s\t%s\n' "$sid" "$speed" \
      "$root/raw/pos_zh/$tag-zhB.wav" "你好open vela" >> "$root/jobs-kokoro.tsv"
    printf '%s\t%s\t%s\t%s\n' "$sid" "$speed" \
      "$root/raw/pos_en/$tag-enA.wav" "Hello, open vela" >> "$root/jobs-kokoro.tsv"
  done
done

# Nobody has fixed how "vela" is pronounced.  Kokoro's lexicon says "VEE-la"
# and Melo's says "VEH-la", so the two engines already cover two readings;
# these extra jobs add "VAY-la" (respelled so espeak-ng produces it) and the
# Mandarin reading "wei la" that many Chinese users will actually say.
: > "$root/jobs-kokoro-variants.tsv"
for sid in $kokoro_sids; do
  tag=$(printf 'kokoro-s%03d-v10' "$sid")
  printf '%s\t1.0\t%s\t%s\n' "$sid" "$root/raw/pos_zh/$tag-zhC.wav" \
    "你好，open 维拉" >> "$root/jobs-kokoro-variants.tsv"
  printf '%s\t1.0\t%s\t%s\n' "$sid" "$root/raw/pos_zh/$tag-zhD.wav" \
    "你好，open vayla" >> "$root/jobs-kokoro-variants.tsv"
  printf '%s\t1.0\t%s\t%s\n' "$sid" "$root/raw/pos_en/$tag-enD.wav" \
    "Hello, open vayla" >> "$root/jobs-kokoro-variants.tsv"
done

n=0
while IFS= read -r text; do
  n=$((n + 1))
  for sid in 0 2 9 24 45 61 79 97; do
    printf '%s\t1.0\t%s\t%s\n' "$sid" \
      "$(printf '%s/raw/neg/kokoro-s%03d-t%02d.wav' "$root" "$sid" "$n")" \
      "$text" >> "$root/jobs-kokoro.tsv"
  done
done <<'EOF'
你好
你好小爱同学
你好世界，今天天气不错
你好欧文，好久不见
你好，open the door
我们一起打开 open source 的大门
你好维拉，吃饭了吗
今天我们来聊一聊开源操作系统
hello open ai
open the window please
hello everyone, welcome to the villa
你好，欧佩克宣布减产
hello, open the velvet box
你好，我们去开会吧
EOF

: > "$root/jobs-melo.tsv"
for speed in 0.8 0.9 1.0 1.1 1.2 1.3; do
  tag="melo-s000-v$(echo "$speed" | tr -d .)"
  printf '0\t%s\t%s\t%s\n' "$speed" "$root/raw/pos_zh/$tag-zhA.wav" \
    "你好，open vela" >> "$root/jobs-melo.tsv"
  printf '0\t%s\t%s\t%s\n' "$speed" "$root/raw/pos_zh/$tag-zhB.wav" \
    "你好open vela" >> "$root/jobs-melo.tsv"
  printf '0\t%s\t%s\t%s\n' "$speed" "$root/raw/pos_en/$tag-enA.wav" \
    "hello, open vela" >> "$root/jobs-melo.tsv"
done

: > "$root/jobs-aishell3.tsv"
mkdir -p "$root/raw/pos_zh8k"
sid=0
while [ "$sid" -lt 174 ]; do
  printf '%s\t1.0\t%s\t%s\n' "$sid" \
    "$(printf '%s/raw/pos_zh8k/aishell3-s%03d-v10-zhT.wav' "$root" "$sid")" \
    "你好，欧喷维拉" >> "$root/jobs-aishell3.tsv"
  sid=$((sid + 6))
done
