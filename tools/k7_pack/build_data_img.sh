#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build the initial /data FAT image for the Nyabula eMMC package.
#   build_data_img.sh <template_dir> <web_dist_dir|-> <version> <out.img> [size_mib]
set -euo pipefail
if [ $# -lt 4 ]; then
  echo "usage: build_data_img.sh <template_dir> <web_dist_dir|-> <version> <out.img> [size_mib]" >&2
  exit 1
fi
TEMPLATE=$1; WEB=$2; VER=$3; OUT=$4; SIZE=${5:-64}
command -v mkfs.fat >/dev/null || { echo "mkfs.fat missing" >&2; exit 1; }
command -v mcopy   >/dev/null || { echo "mtools (mcopy) missing" >&2; exit 1; }
stage=$(mktemp -d); trap 'rm -rf "$stage"' EXIT
cp -a "$TEMPLATE"/. "$stage"/
if [ "$WEB" != "-" ] && [ -d "$WEB" ]; then
  mkdir -p "$stage/www"; cp -a "$WEB"/. "$stage/www"/
fi
mkdir -p "$stage/nyabula"
printf '{"version":"%s","built":"%s"}\n' "$VER" "$(date -u +%FT%TZ)" > "$stage/nyabula/build.json"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE" status=none
mkfs.fat -F 32 -n DATA "$OUT" >/dev/null
( cd "$stage" && find . -mindepth 1 -maxdepth 1 -print0 | xargs -0 -I{} mcopy -s -i "$OUT" {} :: )
echo "data image: $OUT ($SIZE MiB)"
