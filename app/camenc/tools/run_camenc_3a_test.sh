#!/usr/bin/env bash
# Build and run the camenc 3A loop test on the host.
#
# The loop is firmware, but it is also arithmetic, and the arithmetic can be
# checked here -- against a model of a scene rather than against a camera.
# What it is looking for is what a controller does over time, which is the
# part that cannot be seen by reading it.
#
# It lives beside the module it tests rather than under tools/, and nothing
# runs it for you: it is a thing to reach for while working on camenc_3a.c,
# not a gate.  A gate that has to be satisfied to merge a change is a gate
# that gets weakened by whoever is trying to merge one, and this loop is
# easier to reason about by running it than by watching it fail in CI.
#
# The module includes <nuttx/config.h> and <arch/chip/cam3a.h> like the rest
# of the firmware, and the loop reads nothing out of either: it is arithmetic
# over a measurement and nothing else.  So the test supplies an empty config.h
# and points <arch/chip> at the chip's real public include directory, rather
# than needing a configured build tree -- which is what lets it run on any
# machine with a compiler.
#
# Pass -v to print the trajectory of the first case.

set -u

here=$(cd "$(dirname "$0")" && pwd)   # app/camenc/tools
src=$(cd "$here/.." && pwd)            # app/camenc, the module under test
root=$(cd "$src/../.." && pwd)         # the repository root

if ! command -v gcc >/dev/null 2>&1; then
    echo "no host compiler -- nothing to run"
    exit 0
fi

out=$(mktemp -d) || exit 1
trap 'rm -rf "$out"' EXIT

mkdir -p "$out/stub/nuttx"
: > "$out/stub/nuttx/config.h"

# cam3a.h is the chip's public header.  On the target it is reached as
# <arch/chip/cam3a.h> (include/arch/chip is a symlink to chips/rk3576/include);
# stand the same name up here so the module compiles as it does in the
# firmware, without a configured build tree.
mkdir -p "$out/stub/arch"
ln -s "$root/chips/rk3576/include" "$out/stub/arch/chip"

gcc -std=gnu11 -O1 -Wall -Wextra -DFAR= \
    -I"$out/stub" \
    -I"$src" \
    -o "$out/test_camenc_3a" \
    "$here/test_camenc_3a.c" \
    "$src/camenc_3a.c" || exit 1

"$out/test_camenc_3a" "$@"
