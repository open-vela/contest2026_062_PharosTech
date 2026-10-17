#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# != 1 ]]; then
  echo "usage: $0 NUTTX_SOURCE_DIRECTORY" >&2
  exit 2
fi

source_dir=$(realpath "$1")
patch_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Order matters: the self-routing change sits on top of the base AMP patch.
for patch_file in "$patch_dir/gicv2-amp.patch"                   "$patch_dir/gicv2-amp-selfroute.patch"; do
  name=$(basename -- "$patch_file")
  if git -C "$source_dir" apply --reverse --check "$patch_file" 2>/dev/null; then
    echo "$name is already applied"
  else
    git -C "$source_dir" apply --check "$patch_file"
    git -C "$source_dir" apply "$patch_file"
    echo "Applied $name to $source_dir"
  fi
done
