#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: [NYAMP_KERNEL_CONFIG=validated.config] $0" \
       "KERNEL_SOURCE OUTPUT_DIR [BASE_DTS]" >&2
  exit 2
fi

kernel=$(readlink -f "$1")
output=$(mkdir -p "$2" && readlink -f "$2")
base_dts=${3:-rk3576-kickpi-k7-android.dts}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
dts_dir="$kernel/arch/arm64/boot/dts/rockchip"
generated_dts="$dts_dir/rk3576-kickpi-k7-nyabula-amp.dts"
installed_dtsi="$dts_dir/rk3576-kickpi-k7-nyabula-amp.dtsi"

# The shared-memory driver is built in, so it is installed into the kernel
# tree for the duration of the build and removed again by the cleanup trap.
installed_driver="$kernel/drivers/misc/nyamp_shmem.c"
installed_uapi="$kernel/drivers/misc/nyamp_shmem_uapi.h"
misc_makefile="$kernel/drivers/misc/Makefile"
misc_kconfig="$kernel/drivers/misc/Kconfig"

if [[ ! -f "$kernel/Makefile" || ! -f "$dts_dir/$base_dts" ]]; then
  echo "invalid kernel source or missing base DTS: $dts_dir/$base_dts" >&2
  exit 2
fi

cross_compile=${CROSS_COMPILE:-aarch64-linux-gnu-}
if ! command -v "${cross_compile}gcc" >/dev/null; then
  echo "missing cross compiler: ${cross_compile}gcc" >&2
  exit 2
fi

if [[ -e "$generated_dts" || -L "$generated_dts" ||
      -e "$installed_dtsi" || -L "$installed_dtsi" ||
      -e "$installed_driver" || -L "$installed_driver" ||
      -e "$installed_uapi" || -L "$installed_uapi" ]]; then
  echo "refusing to overwrite existing AMP device-tree or driver inputs" >&2
  exit 2
fi

cleanup()
{
  rm -f -- "$generated_dts" "$installed_dtsi" "$installed_driver" \
           "$installed_uapi"

  # The driver is built in, so its Kconfig and Makefile hooks have to be
  # removed again or the next unrelated kernel build would still reference
  # a source file that is no longer there.
  if [[ -n "${misc_makefile_backup:-}" && -f "$misc_makefile_backup" ]]; then
    mv -f -- "$misc_makefile_backup" "$misc_makefile"
  fi

  # The Kconfig entry used to be left behind.  A later build whose .config
  # still carried CONFIG_NYAMP_SHMEM=y then failed with "no rule to make
  # nyamp_shmem.o", because the option survived while the source did not.
  if [[ -n "${misc_kconfig_backup:-}" && -f "$misc_kconfig_backup" ]]; then
    mv -f -- "$misc_kconfig_backup" "$misc_kconfig"
  fi
}
trap cleanup EXIT

cp "$script_dir/rk3576-kickpi-k7-amp.dtsi" "$installed_dtsi"
printf '#include "%s"\n#include "%s"\n' \
  "$base_dts" "$(basename "$installed_dtsi")" > "$generated_dts"

# Install the shared-memory driver into the kernel tree.  CONFIG_MODULES is
# off in this profile, so the driver has to be built in and therefore has to
# live in the tree rather than be loaded later.
cp "$script_dir/nyamp_shmem.c" "$installed_driver"
cp "$script_dir/nyamp_shmem_uapi.h" "$installed_uapi"

if ! grep -q 'nyamp_shmem' "$misc_makefile"; then
  misc_makefile_backup="${misc_makefile}.nyamp-backup"
  cp "$misc_makefile" "$misc_makefile_backup"
  printf 'obj-$(CONFIG_NYAMP_SHMEM)\t+= nyamp_shmem.o\n' >> "$misc_makefile"
fi

if ! grep -q 'NYAMP_SHMEM' "$misc_kconfig"; then
  misc_kconfig_backup="${misc_kconfig}.nyamp-backup"
  cp "$misc_kconfig" "$misc_kconfig_backup"
  cat >> "$misc_kconfig" <<'KCONFIG'

config NYAMP_SHMEM
	tristate "Nyabula AMP shared-memory region"
	depends on ROCKCHIP_AMP
	help
	  Map the reserved shared region used to exchange audio and image
	  payloads with the openvela control domain.  The messages carry only
	  a descriptor; the bytes live in this region, which both domains map
	  non-cacheable so neither has to maintain cache state for the other.

KCONFIG
fi

# NYAMP_KERNEL_CONFIG selects a complete, already validated .config instead
# of deriving one from the vendor defconfig plus nyabula_amp.fragment.
#
# The images that actually ran RKLLM, sherpa and the camera on the board were
# not built from the fragment: their configuration was grown by hand on the
# K7 SDK tree (6.1.75) and additionally enables CPU_FREQ (the measured LLM
# numbers assume schedutil on the A72s), SUSPEND, MMC/VFAT and OV5647.  The
# fragment profile has never been booted with the model runtimes, so a rebuild
# that must behave like the running image has to start from that image's own
# configuration, recovered with:
#
#   scripts/extract-ikconfig Image > validated.config
#
# Only CONFIG_NYAMP_SHMEM is forced on top of it.
seed_config=${NYAMP_KERNEL_CONFIG:-}
if [[ -n "$seed_config" ]]; then
  if [[ ! -f "$seed_config" ]]; then
    echo "NYAMP_KERNEL_CONFIG does not exist: $seed_config" >&2
    exit 2
  fi
  cp "$seed_config" "$output/.config"
  "$kernel/scripts/config" --file "$output/.config" --enable NYAMP_SHMEM
else
  make -C "$kernel" O="$output" ARCH=arm64 \
    CROSS_COMPILE="$cross_compile" rockchip_linux_defconfig
  fragments=()
  if [[ -f "$kernel/arch/arm64/configs/rockchip_amp.config" ]]; then
    fragments+=("$kernel/arch/arm64/configs/rockchip_amp.config")
  fi
  fragments+=("$script_dir/nyabula_amp.fragment")
  "$kernel/scripts/kconfig/merge_config.sh" -m -O "$output" \
    "$output/.config" "${fragments[@]}"
fi
make -C "$kernel" O="$output" ARCH=arm64 \
  CROSS_COMPILE="$cross_compile" olddefconfig

if [[ -n "$seed_config" ]]; then
  # olddefconfig silently resolves anything the tree does not know.  A seed
  # taken from a different kernel version would therefore still "work" while
  # quietly dropping drivers, so any drift beyond the one forced option is
  # reported rather than hidden.
  echo "config drift against $seed_config:"
  "$kernel/scripts/diffconfig" "$seed_config" "$output/.config" || true
fi

for option in ROCKCHIP_AMP ROCKCHIP_MBOX RPMSG_ROCKCHIP_MBOX RPMSG_VIRTIO RPMSG_NS \
              RPMSG_CHAR RPMSG_CTRL ROCKCHIP_RKNPU BLK_DEV_INITRD NULL_TTY \
              NYAMP_SHMEM; do
  if ! grep -q "^CONFIG_${option}=y$" "$output/.config"; then
    echo "required option was not selected: CONFIG_$option" >&2
    exit 1
  fi
done

# CPU_FREQ and SUSPEND are refused only for the fragment profile.  The
# validated seed enables both, and it is the configuration every board run
# with the model runtimes has used; rejecting them here would make the script unable to rebuild the very image
# it is supposed to reproduce.  Everything else stays forbidden in both modes:
# those options let Linux touch hardware the control domain owns.
unsafe_options=(FIQ_DEBUGGER CMDLINE_FORCE CPU_IDLE ARM_ROCKCHIP_DMC_DEVFREQ
                HIBERNATION ROCKCHIP_SUSPEND_MODE INPUT HID BATTERY_RK817
                CHARGER_RK817 RK_HEADSET)
if [[ -z "$seed_config" ]]; then
  unsafe_options+=(CPU_FREQ SUSPEND)
fi

for option in "${unsafe_options[@]}"; do
  if grep -q "^CONFIG_${option}=y$" "$output/.config"; then
    echo "unsafe AMP option remained enabled: CONFIG_$option" >&2
    exit 1
  fi
done

targets=(Image rockchip/rk3576-kickpi-k7-nyabula-amp.dtb)
if grep -q '^CONFIG_MODULES=y$' "$output/.config"; then
  targets+=(modules)
fi

make -C "$kernel" O="$output" ARCH=arm64 \
  CROSS_COMPILE="$cross_compile" -j"${JOBS:-4}" "${targets[@]}"

echo "kernel: $output/arch/arm64/boot/Image"
echo "dtb: $output/arch/arm64/boot/dts/rockchip/"\
"rk3576-kickpi-k7-nyabula-amp.dtb"
