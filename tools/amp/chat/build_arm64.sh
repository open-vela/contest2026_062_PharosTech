#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
output=${1:-out/nyamp-chat-arm64}

# The library has no dependency beyond libstdc++, so the tools are linked
# fully static: the same binaries run on the Debian userland and inside the
# minimal initramfs of the compute domain, where no shared libstdc++ exists.
arguments=(
  -DCMAKE_SYSTEM_NAME=Linux
  -DCMAKE_SYSTEM_PROCESSOR=aarch64
  "-DCMAKE_CXX_COMPILER=${CXX:-aarch64-linux-gnu-g++}"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_EXE_LINKER_FLAGS=-static
)

# Release rather than nyampd's MinSizeRel: loading tokenizer.json and encoding
# the prompt sit on the latency path before the first generated token.  On
# the x86 build host -O2 measured ~30 % faster encoding and ~20 % faster
# loading than -Os, for about 100 KB more archive.
cmake -S "$source_dir" -B "$output" "${arguments[@]}"
cmake --build "$output" -j"${JOBS:-2}"

for binary in nyamp_chat_cli nyamp_chat_unit_test nyamp_chat_golden_test; do
  description=$(file -Lb "$output/$binary")
  if [[ "$description" != *"ARM aarch64"* ]]; then
    echo "unexpected $binary output: $description" >&2
    exit 1
  fi
  if [[ "$description" != *"statically linked"* ]]; then
    echo "$binary must be statically linked" >&2
    exit 1
  fi
done

echo "created $output/libnyamp_chat.a and tools"
