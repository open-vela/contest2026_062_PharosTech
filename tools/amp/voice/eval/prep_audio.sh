#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Turns the raw TTS output and the downloaded public recordings into the
# 16 kHz mono s16 files the tests read, and writes the file lists.
# Usage: prep_audio.sh EVAL_ROOT
#
# Why the padding: a microphone stream always has audio before the wake
# phrase, while a TTS file starts on the first phoneme.  0.4 s of silence in
# front gives the streaming encoder the left context it has in real use;
# without it the first syllable is decoded from a cold state and detection
# rates are pessimistic for a reason that does not exist on the device.
#
# Why the noisy copies: clean TTS is the easiest possible input.  Pink noise
# at roughly 10 dB SNR (relative to the utterance mean level) is a crude
# stand-in for a fan or a room; it is NOT a far-field or reverberation test.

set -eu
root=$1
raw=$root/tts/raw
out=$root/wav16
lists=$root/lists
mkdir -p "$lists" "$out/noise"

convert() {
  # $1 input, $2 output
  ffmpeg -nostdin -loglevel error -y -i "$1" \
    -af "adelay=400:all=1,apad=pad_dur=0.4" -ar 16000 -ac 1 -c:a pcm_s16le "$2"
}

mean_db() {
  ffmpeg -nostdin -i "$1" -af volumedetect -f null - 2>&1 |
    sed -n 's/.*mean_volume: \(-\{0,1\}[0-9.]*\) dB.*/\1/p'
}

noise=$out/noise/pink.wav
if [ ! -f "$noise" ]; then
  ffmpeg -nostdin -loglevel error -y -f lavfi \
    -i "anoisesrc=color=pink:amplitude=0.5:duration=12:sample_rate=16000:seed=20260920" \
    -ac 1 -c:a pcm_s16le "$noise"
fi
noise_db=$(mean_db "$noise")

for set in pos_zh pos_en pos_zh8k neg neg_hard; do
  [ -d "$raw/$set" ] || continue
  mkdir -p "$out/$set"
  for f in "$raw/$set"/*.wav; do
    target=$out/$set/$(basename "$f")
    [ -f "$target" ] || convert "$f" "$target"
  done
  ls "$out/$set"/*.wav > "$lists/$set.txt"
done

for set in pos_zh pos_en; do
  mkdir -p "$out/${set}_snr10"
  for f in "$out/$set"/*.wav; do
    target=$out/${set}_snr10/$(basename "$f")
    [ -f "$target" ] && continue
    speech_db=$(mean_db "$f")
    gain=$(awk -v s="$speech_db" -v n="$noise_db" 'BEGIN { print s - 10 - n }')
    ffmpeg -nostdin -loglevel error -y -i "$f" -stream_loop -1 -i "$noise" \
      -filter_complex "[1]volume=${gain}dB[n];[0][n]amix=inputs=2:duration=first:normalize=0" \
      -ar 16000 -ac 1 -c:a pcm_s16le "$target"
  done
  ls "$out/${set}_snr10"/*.wav > "$lists/${set}_snr10.txt"
done

# Public human recordings used as negatives.  Two of the ASR samples are
# 24 kHz, so everything is passed through the same resampler.
mkdir -p "$out/neg_real"
for f in "$root"/neg/*.wav "$root"/spk/*.wav \
  "$root"/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01/test_wavs/*.wav \
  "$root"/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/test_wavs/*.wav; do
  target=$out/neg_real/$(basename "$(dirname "$f")")-$(basename "$f")
  [ -f "$target" ] ||
    ffmpeg -nostdin -loglevel error -y -i "$f" -ar 16000 -ac 1 -c:a pcm_s16le "$target"
done
ls "$out/neg_real"/*.wav > "$lists/neg_real.txt"

for list in "$lists"/*.txt; do
  seconds=$(while IFS= read -r f; do
    ffprobe -v error -show_entries format=duration -of csv=p=0 "$f"
  done < "$list" | awk '{ s += $1 } END { printf "%.1f", s }')
  echo "AUDIO_SET list=$(basename "$list" .txt) files=$(wc -l < "$list") seconds=$seconds"
done
