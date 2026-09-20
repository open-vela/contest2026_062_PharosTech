#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Prints how many prompt tokens a tool list costs inside the 2048-token window.
# Uses the C++ implementation itself (it is proven identical to the reference
# by the golden test), so it also runs on the board.
#
#   measure_tool_cost.sh <nyamp_chat_cli> <tokenizer.json> <tools.json>...

set -euo pipefail

cli=$1
tokenizer=$2
shift 2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

count() {
  "$cli" prompt "$tokenizer" "$1" --reference --quiet | sed -n 's/.*prompt_tokens=\([0-9]*\).*/\1/p'
}

printf '{"messages":[{"role":"user","content":"hi"}]}' > "$work/none.json"
base=$(count "$work/none.json")
echo "no tools: $base tokens (BOS + user turn + generation prompt)"

for tools in "$@"; do
  printf '{"messages":[{"role":"user","content":"hi"}],"tools":%s}' "$(cat "$tools")" > "$work/with.json"
  total=$(count "$work/with.json")
  echo "$tools: prompt $total tokens, tool block costs $((total - base))"
done
