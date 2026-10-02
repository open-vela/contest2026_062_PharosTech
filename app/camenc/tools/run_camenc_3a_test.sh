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
# The module includes <nuttx/config.h> like the rest of the firmware, and the
# loop reads nothing out of it: it is arithmetic over a measurement and
# nothing else.  So the test supplies an empty one rather than needing a
# configured build tree, which is what lets it run on any machine with a
# compiler.
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

gcc -std=gnu11 -O1 -Wall -Wextra -DFAR= \
    -I"$out/stub" \
    -I"$src" \
    -I"$root/drivers/include" \
    -o "$out/test_camenc_3a" \
    "$here/test_camenc_3a.c" \
    "$src/camenc_3a.c" || exit 1

"$out/test_camenc_3a" "$@"
