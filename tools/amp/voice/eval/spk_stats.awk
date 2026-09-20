# SPDX-License-Identifier: Apache-2.0
#
# Re-scores the SPK_TRIAL lines of nyamp_speaker_file_test after removing
# trials whose ground-truth label cannot be trusted.
#
# Usage: awk -v clean=1 -v drop="0-four-speakers-zh" -f spk_stats.awk TRIALS
#
# Why this exists: the public recordings used as evidence are not labelled
# per passage.
# - 0-four-speakers-zh.wav is a diarization sample that was assembled from
#   the same three people the enrolment files come from (it scores 0.81 /
#   0.69 / 0.60 against leijun / liudehua / fangjun in three different
#   passages), so counting it as "impostor" is simply wrong.  drop= removes
#   files by substring.
# - lei-jun-test.wav is a product-launch recording; it contains a host,
#   audience and music.  With clean=1, runs of two or more consecutive target
#   chunks scoring under 0.30 are treated as "not the target speaker" and
#   removed.  An isolated low chunk is kept as a genuine false reject.
#   This cleaning uses the model under test, so the cleaned false-reject
#   numbers are OPTIMISTIC; both views are reported.

function flush_run(   i) {
  if (run_n >= 2) { for (i = 0; i < run_n; i++) bad[run_idx[i]] = 1 }
  run_n = 0
}

/^SPK_TRIAL/ {
  kind = ""; probe = ""; score = ""
  for (i = 2; i <= NF; i++) {
    split($i, kv, "=")
    if (kv[1] == "kind") kind = kv[2]
    else if (kv[1] == "probe") probe = kv[2]
    else if (kv[1] == "score") score = kv[2] + 0
  }
  if (drop != "" && index(probe, drop) > 0) next
  if (kind == "impostor") { imp[ni++] = score; next }
  split(probe, pp, "@")
  if (clean && pp[1] != last_file) { flush_run(); last_file = pp[1] }
  tgt[nt] = score
  if (clean) {
    if (score < 0.30) run_idx[run_n++] = nt
    else flush_run()
  }
  nt++
}

END {
  flush_run()
  kept = 0
  for (i = 0; i < nt; i++) if (!bad[i]) t[kept++] = tgt[i]
  tsum = 0; tmin = 2
  for (i = 0; i < kept; i++) { tsum += t[i]; if (t[i] < tmin) tmin = t[i] }
  isum = 0; imax = -2
  for (i = 0; i < ni; i++) { isum += imp[i]; if (imp[i] > imax) imax = imp[i] }
  printf "SPK_CLEAN clean=%d drop=%s target_n=%d (removed %d) target_mean=%.3f target_min=%.3f impostor_n=%d impostor_mean=%.3f impostor_max=%.3f\n",
    clean, drop, kept, nt - kept, tsum / kept, tmin, ni, isum / ni, imax
  best = 2
  for (s = -200; s <= 1000; s++) {
    th = s / 1000; fr = 0; fa = 0
    for (i = 0; i < kept; i++) if (t[i] < th) fr++
    for (i = 0; i < ni; i++) if (imp[i] >= th) fa++
    frr = fr / kept; far = fa / ni
    gap = frr > far ? frr - far : far - frr
    if (gap < best) { best = gap; eer = (frr + far) / 2; eth = th }
    if (s >= 300 && s <= 700 && s % 50 == 0)
      row[s] = sprintf("SPK_CLEAN_OPERATING threshold=%.2f false_reject=%.4f (%d/%d) false_accept=%.4f (%d/%d)",
                       th, frr, fr, kept, far, fa, ni)
  }
  printf "SPK_CLEAN_EER eer=%.4f threshold=%.3f small_sample=1\n", eer, eth
  for (s = 300; s <= 700; s += 50) print row[s]
}
