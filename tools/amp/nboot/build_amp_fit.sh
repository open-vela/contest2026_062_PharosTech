#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 5 || $# -gt 6 ]]; then
  echo "usage: $0 LINUX_IMAGE K7_DTB INITRAMFS OPENVELA_BIN OPENVELA_CONFIG [OUTPUT.itb]" >&2
  exit 2
fi

kernel=$(readlink -f "$1")
fdt=$(readlink -f "$2")
ramdisk=$(readlink -f "$3")
openvela=$(readlink -f "$4")
config=$(readlink -f "$5")
output=${6:-nyabula-amp.itb}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

for input in "$kernel" "$fdt" "$ramdisk" "$openvela" "$config"; do
  if [[ ! -f "$input" ]]; then
    echo "missing input: $input" >&2
    exit 2
  fi
done

for tool in mkimage fdtget gzip python3; do
  if ! command -v "$tool" >/dev/null; then
    echo "missing tool: $tool" >&2
    exit 2
  fi
done

python3 "$script_dir/../linux/camera_ownership.py" check-dtb "$fdt"

python3 "$script_dir/../validate_amp_layout.py" --config "$config" --dtb "$fdt" \
  --linux-image "$kernel" --nuttx-image "$openvela"

if [[ $(od -An -tx1 -j56 -N4 "$kernel" | tr -d ' \n') != 41524d64 ]]; then
  echo "Linux Image header magic is invalid" >&2
  exit 1
fi

if [[ $(od -An -tx1 -N4 "$fdt" | tr -d ' \n') != d00dfeed ]]; then
  echo "K7 DTB magic is invalid" >&2
  exit 1
fi

gzip -t "$ramdisk"

if [[ $(od -An -tx1 -j56 -N4 "$openvela" | tr -d ' \n') != 41524d64 ]]; then
  echo "openvela Image header magic is invalid" >&2
  exit 1
fi

check_size()
{
  local path=$1
  local limit=$2
  local label=$3
  local size

  size=$(stat -c%s "$path")
  if (( size <= 0 || size > limit )); then
    echo "$label is empty or exceeds its load window" >&2
    exit 1
  fi
}

check_size "$kernel" 0x05000000 "Linux Image"
check_size "$fdt" 0x01000000 "K7 DTB"
check_size "$ramdisk" 0x10000000 "initramfs"
check_size "$openvela" 0x01000000 "openvela image"

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
its="$work/nyabula-amp.its"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}

{
  printf '/dts-v1/;\n\n/ {\n'
  printf '  description = "Nyabula KICKPI-K7 AMP";\n'
  printf '  #address-cells = <2>;\n\n'
  printf '  images {\n'
  printf '    linux {\n'
  printf '      description = "RK3576 Linux compute domain";\n'
  printf '      data = /incbin/("%s");\n' "$kernel"
  printf '      type = "kernel"; arch = "arm64"; os = "linux";\n'
  printf '      compression = "none";\n'
  printf '      cpu = <0x100>;\n'
  printf '      load = <0x0 0x42000000>; entry = <0x0 0x42000000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '    fdt {\n'
  printf '      description = "KICKPI-K7 AMP device tree";\n'
  printf '      data = /incbin/("%s");\n' "$fdt"
  printf '      type = "flat_dt"; arch = "arm64"; compression = "none";\n'
  printf '      load = <0x0 0x4f000000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '    ramdisk {\n'
  printf '      description = "Nyabula AMP initramfs";\n'
  printf '      data = /incbin/("%s");\n' "$ramdisk"
  printf '      type = "ramdisk"; arch = "arm64"; os = "linux";\n'
  printf '      compression = "gzip"; load = <0x0 0x50000000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '    openvela {\n'
  printf '      description = "openvela four-A53 control domain";\n'
  printf '      data = /incbin/("%s");\n' "$openvela"
  printf '      type = "firmware"; arch = "arm64"; compression = "none";\n'
  printf '      cpu = <0x0>;\n'
  printf '      load = <0x0 0x4a400000>; entry = <0x0 0x4a400000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '  };\n\n'
  printf '  configurations {\n'
  printf '    default = "conf";\n'
  printf '    conf {\n'
  printf '      nyabula,amp-abi = <2>;\n'
  printf '      kernel = "linux"; fdt = "fdt"; ramdisk = "ramdisk";\n'
  printf '      loadables = "openvela";\n'
  printf '    };\n'
  printf '  };\n'
  printf '};\n'
} > "$its"

mkimage -E -B 0x200 -p 0x1000 -f "$its" "$output" >/dev/null

[[ $(fdtget -t x "$output" /images/linux load) == "0 42000000" ]]
[[ $(fdtget -t x "$output" /images/openvela load) == "0 4a400000" ]]
[[ $(fdtget -t x "$output" /images/openvela entry) == "0 4a400000" ]]
[[ $(fdtget -t x "$output" /images/openvela cpu) == "0" ]]
[[ $(fdtget -t x "$output" /images/linux cpu) == "100" ]]

echo "created $output ($(stat -c%s "$output") bytes)"
echo "Offline artifact only: requires the new 4+4 bootamp handoff and GIC/transport validation."
sha256sum "$output"
