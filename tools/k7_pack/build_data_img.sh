#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build the initial /data FAT image for the Nyabula eMMC package.
#   build_data_img.sh <template_dir> <web_dist_dir|-> <version> <out.img> [size_mib]
#   env MODELS_DIR=<dir>  -> copied to /models (TTS rknn, face onnx ...)
#
# The size is a floor, not a target.  When the payload does not fit in the
# requested size the image grows to fit it, so adding a model cannot
# silently produce a truncated image the way a fixed 128 MiB did.  A
# caller that really wants a fixed size can pass one, but it will be
# refused rather than honoured if the payload exceeds it.
set -euo pipefail
if [ $# -lt 4 ]; then
  echo "usage: build_data_img.sh <template_dir> <web_dist_dir|-> <version> <out.img> [size_mib]" >&2
  exit 1
fi
TEMPLATE=$1; WEB=$2; VER=$3; OUT=$4; FLOOR_MIB=${5:-64}
command -v mkfs.fat >/dev/null || { echo "mkfs.fat missing" >&2; exit 1; }
command -v mcopy   >/dev/null || { echo "mtools (mcopy) missing" >&2; exit 1; }
stage=$(mktemp -d); trap 'rm -rf "$stage"' EXIT
cp -a "$TEMPLATE"/. "$stage"/
if [ "$WEB" != "-" ] && [ -d "$WEB" ]; then
  mkdir -p "$stage/www"; cp -a "$WEB"/. "$stage/www"/
fi
if [ -n "${MODELS_DIR:-}" ] && [ -d "$MODELS_DIR" ]; then
  mkdir -p "$stage/models"; cp -a "$MODELS_DIR"/. "$stage/models"/
fi
mkdir -p "$stage/nyabula" "$stage/music"
printf '{"version":"%s","built":"%s"}\n' "$VER" "$(date -u +%FT%TZ)" > "$stage/nyabula/build.json"

# Check the model files are structurally whole before packing them.
#
# Reading the image back afterwards (below) only proves the copy was
# faithful -- it cannot tell a complete file from one that was truncated
# at the source.  A 38 MB ONNX cut down to 9 MB copies perfectly and
# hashes identically at both ends, which is exactly how a broken face
# model reached a shipping image.  So check the container itself: an ONNX
# graph and an RKNN blob both declare their own length up front, and the
# file has to be at least that long.
if [ -d "$stage/models" ]; then
  python3 - "$stage/models" <<'MODELS' || exit 1
import sys, pathlib

bad = False
for path in sorted(pathlib.Path(sys.argv[1]).rglob("*")):
    if not path.is_file():
        continue
    size = path.stat().st_size
    head = path.read_bytes()[:16]
    if path.suffix.lower() == ".onnx":
        # protobuf: ir_version as field 1 varint, then a length-delimited
        # graph field 7 whose declared length must fit in the file.
        declared = None
        i = 0
        while i < len(head):
            tag = head[i]; i += 1
            field, wire = tag >> 3, tag & 7
            if wire == 0:
                v = 0; shift = 0
                while i < len(head):
                    c = head[i]; i += 1
                    v |= (c & 0x7f) << shift; shift += 7
                    if not c & 0x80:
                        break
            elif wire == 2:
                ln = 0; shift = 0
                while i < len(head):
                    c = head[i]; i += 1
                    ln |= (c & 0x7f) << shift; shift += 7
                    if not c & 0x80:
                        break
                if field == 7:
                    declared = ln
                break
            else:
                break
        if declared is not None and declared + 8 > size:
            print("model is truncated: %s declares %d bytes of graph, file is %d"
                  % (path.name, declared, size), file=sys.stderr)
            bad = True
    elif path.suffix.lower() in (".rknn", ".rkn"):
        if head[:4] != b"RKNN":
            print("model is not an RKNN blob: %s" % path.name, file=sys.stderr)
            bad = True
if bad:
    sys.exit(1)
MODELS
fi

# Size the image from the payload.  FAT32 overhead is the two allocation
# tables plus the reserved area; 10% headroom over the payload covers them
# and the cluster slack of many small files.
payload=$(du -sb --apparent-size "$stage" | cut -f1)
need_mib=$(( (payload + payload / 10 + 1048575) / 1048576 ))
SIZE=$FLOOR_MIB
if [ "$need_mib" -gt "$SIZE" ]; then
  SIZE=$need_mib
  echo "data image grown to ${SIZE} MiB for a ${payload}-byte payload" >&2
fi

# The filesystem stays FAT32 regardless of size: this image seeds a
# partition that the board later sees as ~30 GB, and the on-device mount
# has to keep working.  Below about 64 MiB mkfs.fat warns that a FAT32
# geometry that small is unusual, so hold the floor there rather than
# switching variants -- the warning is about efficiency, not correctness,
# and changing the variant would be a real compatibility risk.
if [ "$SIZE" -lt 64 ]; then
  SIZE=64
fi

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE" status=none
mkfs.fat -F 32 -n DATA "$OUT" >/dev/null
( cd "$stage" && find . -mindepth 1 -maxdepth 1 -print0 | xargs -0 -I{} mcopy -s -i "$OUT" {} :: )

# Read every staged file back out of the image and compare, so a copy that
# went wrong on the way in cannot pass unnoticed.
#
# This runs as its own script rather than inline: a failure inside a
# pipeline or a subshell would not reach the caller, and this check is
# only worth having if it can actually stop the build.
verify=$(mktemp -d)
trap 'rm -rf "$stage" "$verify"' EXIT
cat > "$verify/check.sh" <<'CHECK'
#!/bin/bash
set -euo pipefail
stage=$1; image=$2; work=$3
cd "$stage"
find . -type f | sed 's|^\./||' > "$work/list"
while IFS= read -r rel; do
  mkdir -p "$work/out/$(dirname "$rel")"
  if ! mcopy -o -i "$image" "::${rel}" "$work/out/$rel" >/dev/null 2>&1; then
    echo "data image verification failed: cannot read back $rel" >&2
    exit 1
  fi
  staged=$(sha256sum "$stage/$rel" | cut -d' ' -f1)
  inimage=$(sha256sum "$work/out/$rel" | cut -d' ' -f1)
  if [ "$staged" != "$inimage" ]; then
    echo "data image verification failed: $rel" >&2
    echo "  staged   $staged" >&2
    echo "  in image $inimage" >&2
    exit 1
  fi
done < "$work/list"
CHECK
chmod +x "$verify/check.sh"
bash "$verify/check.sh" "$stage" "$OUT" "$verify"

echo "data image: $OUT ($SIZE MiB, payload $payload bytes verified)"
