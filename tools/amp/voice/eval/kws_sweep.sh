#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# One row per (model, keywords, threshold, boost, audio set).
# Usage: kws_sweep.sh EVAL_ROOT TEST_BINARY MODEL_NAME KEYWORDS THRESHOLDS SCORES SETS
#   e.g. kws_sweep.sh $R $R/build-x64/nyamp_kws_file_test zhen-c16 \
#          zhen-all "0.1 0.25" "1.0 2.0" "pos_zh neg_real"
#
# Positives report files_detected/files (detection rate); negatives report
# detections and detections per hour (false accepts).  Every positive here is
# SYNTHETIC speech; the table is a ranking tool, not a product measurement.
# KWS_EXTRA passes further test options, e.g. "--max-active-paths 16".

set -eu
root=$1
test=$2
model=$3
keywords=$4
thresholds=$5
scores=$6
sets=$7

for threshold in $thresholds; do
  for score in $scores; do
    for set in $sets; do
      line=$("$test" "$root/mdl/$model" --keywords "$root/keywords/$keywords.txt" \
        --threshold "$threshold" --score "$score" --quiet ${KWS_EXTRA:-} \
        --list "$root/lists/$set.txt" 2> /dev/null | grep '^KWS_SUMMARY')
      echo "$line" | awk -v m="$model" -v k="$keywords" -v s="$set" \
        -v x="${KWS_EXTRA:-}" '{
        for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
        printf "KWS_ROW model=%s keywords=%s extra=[%s] threshold=%s score=%s set=%s files=%d files_detected=%d rate=%.3f detections=%d per_hour=%.2f audio_s=%.0f rtf=%.4f rss_peak_kb=%d\n",
          m, k, x, v["threshold"], v["score"], s, v["files"], v["files_detected"],
          v["files_detected"] / v["files"], v["detections"],
          v["detections"] * 3600 / v["audio_s"], v["audio_s"], v["rtf"], v["rss_peak_kb"]
      }'
    done
  done
done
