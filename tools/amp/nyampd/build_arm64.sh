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

# The speech runtimes are external too, and each pair is optional:
#   NYAMP_SHERPA_INCLUDE / NYAMP_SHERPA_LIBRARY   sherpa-onnx C API (c-api.h,
#       libsherpa-onnx-c-api.so): streaming ASR and the wake word
#   NYAMP_ORT_INCLUDE / NYAMP_ORT_LIBRARY         onnxruntime_c_api.h,
#       libonnxruntime.so          } together: MeloTTS, CPU prefix +
#   NYAMP_RKNN_INCLUDE / NYAMP_RKNN_LIBRARY       rknn_api.h, librknnrt.so
#                                  }           NPU vocoder
# A service whose runtime is missing is compiled in all the same, answers its
# opcodes unsupported and leaves its HEALTH capability bit clear.
dynamic=0
for name in NYAMP_SHERPA_INCLUDE NYAMP_SHERPA_LIBRARY NYAMP_ORT_INCLUDE \
            NYAMP_ORT_LIBRARY NYAMP_RKNN_INCLUDE NYAMP_RKNN_LIBRARY; do
  if [[ -n "${!name:-}" ]]; then
    arguments+=("-D$name=${!name}")
    dynamic=1
  fi
done

if [[ -n "$rkllm_root" ]]; then
  # The vendor runtime is a shared object, so a static link is impossible.
  # The deployed image must carry librkllmrt.so and its own dependencies.
  arguments+=("-DNYAMP_RKLLM_ROOT=$rkllm_root")
  dynamic=1
fi

if [[ "$dynamic" == 1 ]]; then
  # The libraries are found through the image's /usr/lib, never through the
  # build machine's directories, which a RUNPATH would otherwise record.
  arguments+=(-DCMAKE_SKIP_RPATH=ON)
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

if [[ "$dynamic" == 0 && "$description" != *"statically linked"* ]]; then
  echo "nyampd must be statically linked without the vendor runtime" >&2
  exit 1
fi

echo "created $output/nyampd"
