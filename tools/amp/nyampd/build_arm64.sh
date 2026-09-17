#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
output=${1:-out/nyampd-arm64}
rkllm_root=${NYAMP_RKLLM_ROOT:-}

# The Linux compute domain keeps its vendor libraries outside this repository.
# Point NYAMP_RKLLM_ROOT at a directory holding rkllm.h and librkllmrt.so to
# build the model service in; without it the daemon still serves health/info
# and answers every LLM request as unsupported.
arguments=(
  -DCMAKE_SYSTEM_NAME=Linux
  -DCMAKE_SYSTEM_PROCESSOR=aarch64
  "-DCMAKE_C_COMPILER=${CC:-aarch64-linux-gnu-gcc}"
  "-DCMAKE_CXX_COMPILER=${CXX:-aarch64-linux-gnu-g++}"
  -DCMAKE_BUILD_TYPE=MinSizeRel
)

if [[ -n "$rkllm_root" ]]; then
  # The vendor runtime is a shared object, so a static link is impossible.
  # The deployed image must carry librkllmrt.so and its own dependencies.
  arguments+=("-DNYAMP_RKLLM_ROOT=$rkllm_root")
else
  arguments+=(-DCMAKE_EXE_LINKER_FLAGS=-static)
fi

cmake -S "$source_dir" -B "$output" "${arguments[@]}"
cmake --build "$output" -j"${JOBS:-4}"

description=$(file -Lb "$output/nyampd")
if [[ "$description" != *"ARM aarch64"* ]]; then
  echo "unexpected nyampd output: $description" >&2
  exit 1
fi

if [[ -z "$rkllm_root" && "$description" != *"statically linked"* ]]; then
  echo "nyampd must be statically linked without the vendor runtime" >&2
  exit 1
fi

echo "created $output/nyampd"
