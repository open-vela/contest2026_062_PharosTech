#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the initramfs for the compute profile: nyampd linked against the
# vendor model runtimes, plus every shared library that implies.
#
# build_minimal_initramfs.sh cannot produce this image.  It insists on a
# statically linked nyampd, and a static nyampd is by construction one without
# the RKLLM backend (the vendor runtime only ships as a shared object), so an
# image made that way answers every LLM request "unsupported".  The
# shared-memory bring-up image regressed in exactly this way: it carried a
# static nyampd, so the LLM service that had worked one image earlier was
# gone while librkllmrt.so still sat unused next to the daemon.  This script
# takes the dynamic daemon, refuses one
# that lacks the backend, and collects the library closure itself so the image
# cannot be missing a dependency that only shows up as a respawn loop on a
# board without a console.
#
# Vendor runtimes are never part of this repository.  They are looked up in
# the directories named by NYAMP_RUNTIME_LIB_DIRS on the build machine.

set -euo pipefail
umask 022

if [[ $# -lt 3 ]]; then
  cat >&2 <<USAGE
usage: NYAMP_RUNTIME_LIB_DIRS=dir[:dir...] $0 \\
         BUSYBOX NYAMPD OUTPUT.cpio.gz [EXTRA_ELF...]

  BUSYBOX    statically linked AArch64 busybox
  NYAMPD     dynamically linked AArch64 nyampd built with NYAMP_RKLLM_ROOT
  EXTRA_ELF  further executables (installed to /usr/sbin) or shared
             libraries (installed to /usr/lib), e.g. the speech runtimes that
             nyampd does not link yet, or nyamp_shmem_test.  Write
             /usr/bin=PATH to install an executable into another directory
             under its own name.
USAGE
  exit 2
fi

busybox=$(readlink -f "$1")
nyampd=$(readlink -f "$2")
output=$3
shift 3
extras=("$@")
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cross_compile=${CROSS_COMPILE:-aarch64-linux-gnu-}

for tool in cpio gzip file "${cross_compile}readelf" "${cross_compile}gcc"; do
  if ! command -v "$tool" >/dev/null; then
    echo "missing tool: $tool" >&2
    exit 2
  fi
done

description=$(file -Lb "$busybox")
if [[ ! -x "$busybox" || "$description" != *"ARM aarch64"* ||
      "$description" != *"statically linked"* ]]; then
  echo "busybox must be a statically linked AArch64 ELF: $description" >&2
  exit 2
fi

description=$(file -Lb "$nyampd")
if [[ ! -x "$nyampd" || "$description" != *"ARM aarch64"* ||
      "$description" != *"dynamically linked"* ]]; then
  echo "nyampd must be a dynamically linked AArch64 ELF: $description" >&2
  exit 2
fi

needed()
{
  "${cross_compile}readelf" -d "$1" 2>/dev/null |
    sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p'
}

# The whole point of this profile is the model service.  A daemon that does
# not pull in the runtime was configured without NYAMP_RKLLM_ROOT.
if ! needed "$nyampd" | grep -qx 'librkllmrt.so'; then
  echo "nyampd was built without the RKLLM backend (no librkllmrt.so)" >&2
  exit 2
fi

# A RUNPATH pointing into the build machine is harmless on the board but it
# is also how a daemon ends up working only where it was built.
if "${cross_compile}readelf" -d "$nyampd" | grep -Eq 'R(UN)?PATH'; then
  echo "nyampd carries an RPATH/RUNPATH; configure with" \
       "-DCMAKE_SKIP_RPATH=ON" >&2
  exit 2
fi

# Library search order: the vendor directories first, then the C and C++
# runtime of the very toolchain nyampd was linked with, so the symbol
# versions the daemon asks for are the ones the image provides.
search=()
IFS=: read -r -a vendor_dirs <<< "${NYAMP_RUNTIME_LIB_DIRS:-}"
for dir in "${vendor_dirs[@]}"; do
  [[ -n "$dir" ]] && search+=("$(readlink -f "$dir")")
done
for probe in libc.so.6 libstdc++.so.6 libgcc_s.so.1 libgomp.so.1; do
  path=$("${cross_compile}gcc" -print-file-name="$probe")
  if [[ "$path" == /* ]]; then
    search+=("$(dirname "$(readlink -f "$path")")")
  fi
done

locate()
{
  local dir
  for dir in "${search[@]}"; do
    if [[ -f "$dir/$1" ]]; then
      readlink -f "$dir/$1"
      return 0
    fi
  done
  return 1
}

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
epoch=${SOURCE_DATE_EPOCH:-0}

# mktemp creates the directory 0700 and cpio records that as the mode of "/".
# Everything runs as root today, but a root directory nobody else can enter
# is a trap for the first service that drops privileges.
chmod 0755 "$work"

# One library directory.  /lib is a link to it because the ELF interpreter
# path (/lib/ld-linux-aarch64.so.1) is fixed at link time, and keeping two
# real directories is how the previous images came to carry libstdc++ twice.
mkdir -p "$work"/{bin,sbin,etc,proc,sys,dev,run,tmp,usr/bin,usr/sbin,usr/lib}
ln -s usr/lib "$work/lib"

cp "$busybox" "$work/bin/busybox"
cp "$script_dir/init" "$work/init"
cp "$script_dir/mount-data" "$work/usr/sbin/mount-data"
cp "$nyampd" "$work/usr/sbin/nyampd"
chmod 0755 "$work/init" "$work/bin/busybox" "$work/usr/sbin/mount-data" \
           "$work/usr/sbin/nyampd"

for applet in sh mount mkdir echo; do
  ln -s busybox "$work/bin/$applet"
done
ln -s ../bin/busybox "$work/sbin/mdev"
ln -s ../bin/busybox "$work/sbin/init"

# mount-data has to be listed before the daemon, but it is "once", not
# "wait": BusyBox init starts it and moves on, so nyampd is not held up by
# the twenty seconds it polls for a card that the product split never shows.
{
  sed '/^::respawn:/,$d' "$script_dir/inittab"
  echo '::once:/usr/sbin/mount-data'
  sed -n '/^::respawn:/,$p' "$script_dir/inittab"
} > "$work/etc/inittab"

queue=("$nyampd")
for extra in "${extras[@]}"; do
  # "/usr/bin=path" places a tool under the name it was built with.  Daemons
  # and self-tests default to /usr/sbin with dashes, matching what earlier
  # images called them, but a measurement tool such as nyamp_chat_cli is
  # looked up by the name its README uses.
  destination=
  if [[ "$extra" == /*=* ]]; then
    destination=${extra%%=*}
    extra=${extra#*=}
  fi
  extra=$(readlink -f "$extra")
  description=$(file -Lb "$extra")
  if [[ "$description" != *"ARM aarch64"* ]]; then
    echo "not an AArch64 ELF: $extra ($description)" >&2
    exit 2
  fi
  if [[ -n "$destination" ]]; then
    mkdir -p "$work$destination"
    install -m 0755 "$extra" "$work$destination/$(basename "$extra")"
  elif [[ "$description" == *"shared object"* &&
        "$description" != *"interpreter"* ]]; then
    install -m 0644 "$extra" "$work/usr/lib/$(basename "$extra")"
  else
    install -m 0755 "$extra" "$work/usr/sbin/$(basename "$extra" |
                                               tr '_' '-')"
  fi
  queue+=("$extra")
done

# Walk DT_NEEDED breadth first.  Anything unresolved is fatal here, because
# on the board it would only surface as nyampd being respawned forever.
while [[ ${#queue[@]} -gt 0 ]]; do
  current=${queue[0]}
  queue=("${queue[@]:1}")
  while read -r name; do
    [[ -z "$name" || -e "$work/usr/lib/$name" ]] && continue
    if ! path=$(locate "$name"); then
      echo "unresolved dependency of $(basename "$current"): $name" >&2
      exit 1
    fi
    install -m 0644 "$path" "$work/usr/lib/$name"
    queue+=("$path")
  done < <(needed "$current")
done

# The interpreter is not a DT_NEEDED entry of anything.
interpreter=$("${cross_compile}readelf" -l "$nyampd" |
              sed -n 's/.*program interpreter: \(.*\)\]$/\1/p')
if [[ "$interpreter" != /lib/* ]]; then
  echo "unexpected ELF interpreter: $interpreter" >&2
  exit 1
fi
if ! path=$(locate "$(basename "$interpreter")"); then
  echo "ELF interpreter not found: $interpreter" >&2
  exit 1
fi
install -m 0755 "$path" "$work/usr/lib/$(basename "$interpreter")"

find "$work" -exec touch -h -d "@$epoch" {} +

(
  cd "$work"
  find . -print0 | LC_ALL=C sort -z | \
    cpio --null -o -H newc --quiet --reproducible --owner=0:0
) | gzip -n -9 > "$output"

# Record exactly what went in.  The vendor libraries are identified by hash
# only; their provenance and licences are tracked outside the repository.
(
  cd "$work"
  find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
  find . -type l -printf '%p -> %l\n' | LC_ALL=C sort
) > "$output.contents"

echo "created $output ($(stat -c%s "$output") bytes," \
     "$(du -sk "$work" | cut -f1) KiB unpacked)"
