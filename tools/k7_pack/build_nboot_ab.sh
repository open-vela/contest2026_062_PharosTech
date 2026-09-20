#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build KICKPI-K7 N-Boot + NuttX A/B release artifacts.

set -Eeuo pipefail

trap 'status=$?; echo "ERROR: build_nboot_ab.sh:${LINENO}: ${BASH_COMMAND}" >&2; exit "$status"' ERR

die()
{
  echo "ERROR: $*" >&2
  exit 1
}

abspath()
{
  (cd "$(dirname "$1")" && printf '%s/%s' "$(pwd)" "$(basename "$1")")
}

[ "$#" -eq 5 ] || die \
  "usage: build_nboot_ab.sh <nuttx.bin> <nboot_dir> <rkbin_dir> <out_dir> <sd|emmc>"
[ -r "$1" ] || die "NuttX image is not readable: $1"
[ -d "$2" ] || die "N-Boot release directory is missing: $2"
[ -d "$3" ] || die "rkbin directory is missing: $3"

NUTTX=$(abspath "$1")
NBOOT_DIR=$(cd "$2" && pwd)
RKBIN=$(cd "$3" && pwd)
TARGET=$5
[ "$TARGET" = sd ] || [ "$TARGET" = emmc ] || die \
  "target must be sd or emmc: $TARGET"
mkdir -p "$4"
OUT=$(cd "$4" && pwd)
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BOOTCTRL="$SCRIPT_DIR/../k7_abpack/bootctrl.py"
NBOOT_PROPER="$NBOOT_DIR/nboot-kickpi-k7.bin"
NBOOT_DTB="$NBOOT_DIR/nboot-kickpi-k7.dtb"
MKIMAGE="$RKBIN/tools/mkimage"
BL31="$RKBIN/bin/rk35/rk3576_bl31_v1.24.elf"
BL32="$RKBIN/bin/rk35/rk3576_bl32_v1.08.bin"

UBOOT_START=16384
UBOOT_SECTORS=8192
TRUST_START=24576
TRUST_SECTORS=8192
BOOTCTRL_START=32768
BOOTCTRL_SECTORS=2048
NUTTX_A_START=36864
NUTTX_B_START=167936
NUTTX_SECTORS=131072
AMP_A_START=299008
AMP_B_START=1347584
AMP_SECTORS=1048576
# Configuration lives in its own partition, ahead of data, so that wiping
# the bulk store does not also wipe provisioning.  Losing data costs the
# models, which an OTA can put back; losing wifi.json sends the device
# back to AP mode and needs a human.  The two deserve different lifetimes.
CONFIG_START=2396160
CONFIG_SECTORS=65536
DATA_START=2461696

for tool in dd dtc fdtget python3 sha256sum truncate; do
  command -v "$tool" >/dev/null 2>&1 || die "missing host tool: $tool"
done
if [ "$TARGET" = sd ]; then
  for tool in mkfs.fat sgdisk; do
    command -v "$tool" >/dev/null 2>&1 || die "missing host tool: $tool"
  done
fi
for file in "$NBOOT_PROPER" "$NBOOT_DTB" "$BOOTCTRL" "$MKIMAGE" \
            "$RKBIN/tools/boot_merger" "$RKBIN/tools/trust_merger" \
            "$RKBIN/RKBOOT/RK3576MINIALL.ini" \
            "$RKBIN/RKTRUST/RK3576TRUST.ini" "$BL31" "$BL32"; do
  [ -r "$file" ] || die "required input is not readable: $file"
done

[ "$(stat -c %s "$NUTTX")" -le $((64 * 1024 * 1024)) ] ||
  die "NuttX image exceeds its 64 MiB slot"
[ "$(stat -c %s "$NBOOT_PROPER")" -le $((2 * 1024 * 1024)) ] ||
  die "N-Boot proper exceeds 2 MiB"
[ "$(od -An -N4 -tx1 "$NBOOT_DTB" | tr -d ' \n')" = "d00dfeed" ] ||
  die "N-Boot DTB has invalid magic"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

dd if="$BL31" of="$WORK/atf-1.bin" bs=1 skip=$((0x20000)) \
  count=$((0x1e020)) status=none
dd if="$BL31" of="$WORK/atf-2.bin" bs=1 skip=$((0x40000)) \
  count=$((0x5000)) status=none
dd if="$BL31" of="$WORK/atf-3.bin" bs=1 skip=$((0x10000)) \
  count=$((0x4000)) status=none
cp "$BL32" "$WORK/optee.bin"
cp "$NBOOT_DTB" "$WORK/kickpi-k7.dtb"
cat "$NBOOT_PROPER" "$NBOOT_DTB" > "$WORK/u-boot.bin"

cat > "$WORK/nboot.its" <<'ITS'
/dts-v1/;
/ {
  description = "Nyabula N-Boot for KICKPI-K7";
  #address-cells = <1>;
  images {
    atf-1 { data = /incbin/("atf-1.bin"); type = "firmware";
      arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
      load = <0x40060000>; entry = <0x40060000>;
      hash { algo = "sha256"; }; };
    atf-2 { data = /incbin/("atf-2.bin"); type = "firmware";
      arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
      load = <0x400f0000>; hash { algo = "sha256"; }; };
    atf-3 { data = /incbin/("atf-3.bin"); type = "firmware";
      arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
      load = <0x3fe70000>; hash { algo = "sha256"; }; };
    optee { data = /incbin/("optee.bin"); type = "firmware";
      arch = "arm64"; os = "op-tee"; compression = "none";
      load = <0x48400000>; entry = <0x48400000>;
      hash { algo = "sha256"; }; };
    fdt { data = /incbin/("kickpi-k7.dtb"); type = "flat_dt";
      arch = "arm64"; compression = "none";
      hash { algo = "sha256"; }; };
    uboot { data = /incbin/("u-boot.bin"); type = "standalone";
      arch = "arm64"; compression = "none";
      load = <0x40200000>; entry = <0x40200000>;
      hash { algo = "sha256"; }; };
  };
  configurations {
    default = "conf";
    conf { description = "rk3576-evb"; firmware = "atf-1";
      loadables = "atf-2", "atf-3", "optee", "uboot"; fdt = "fdt"; };
  };
};
ITS

# Vendor SPL reads external FIT data in storage blocks. New mkimage versions
# accept an explicit alignment; the pinned rkbin version uses 512 bytes by
# default but does not implement -B.
MKIMAGE_ALIGNMENT=()
if "$MKIMAGE" -h 2>&1 | grep -q -- '-B'; then
  MKIMAGE_ALIGNMENT=(-B 0x200)
fi
(cd "$WORK" && "$MKIMAGE" -E "${MKIMAGE_ALIGNMENT[@]}" -p 0x1000 \
  -f nboot.its nboot.fit >/dev/null)
# The pinned rkbin mkimage leaks its mmap address into the FIT reservation
# entry. N-Boot does not consume that reservation, so terminate the map at its
# first entry to make release output reproducible.
dd if=/dev/zero of="$WORK/nboot.fit" bs=1 seek=40 count=16 \
  conv=notrunc status=none
for node in atf-1 atf-2 atf-3 optee fdt uboot; do
  position=$(fdtget -tx "$WORK/nboot.fit" "/images/$node" data-position)
  [ $((16#$position % 512)) -eq 0 ] || die \
    "FIT payload $node is not 512-byte aligned: 0x$position"
done
[ "$(stat -c %s "$WORK/nboot.fit")" -le $((4 * 1024 * 1024)) ] ||
  die "N-Boot FIT exceeds its 4 MiB region"
cp "$WORK/nboot.fit" "$OUT/nboot.img"
truncate -s $((4 * 1024 * 1024)) "$OUT/nboot.img"

RKBIN_WORK="$WORK/rkbin"
cp -r "$RKBIN" "$RKBIN_WORK"
chmod -R u+w "$RKBIN_WORK"
(cd "$RKBIN_WORK" && ./tools/boot_merger RKBOOT/RK3576MINIALL.ini >/dev/null)
(cd "$RKBIN_WORK" && ./tools/trust_merger RKTRUST/RK3576TRUST.ini >/dev/null)
cp "$RKBIN_WORK"/rk3576_idblock_*.img "$WORK/idbloader.img"
# boot_merger writes two things.  The idblock image is what sits on the
# medium (the SD image below dd's it into place); the merged loader, with
# the "LDR " header and usbplug inside, is what RKDevTool is given as the
# Loader.  Handing it the idblock instead -- as this script did -- leaves a
# package that cannot be flashed at all.
cp "$RKBIN_WORK"/rk3576_spl_loader_*.bin "$WORK/loader.bin"
[ "$(head -c 4 "$WORK/loader.bin")" = "LDR " ] || die "boot_merger did not produce a merged loader"
cp "$RKBIN_WORK/trust.img" "$WORK/trust.img"

# An AMP image that is packed but not recorded here has priority 0: N-Boot
# skips it and the board comes up in the plain firmware, without the compute
# domain -- no voice, no on-device model -- and nothing says why.
BOOTCTRL_AMP=()
if [ -n "${AMP_ITB:-}" ] && [ -f "$AMP_ITB" ]; then
  BOOTCTRL_AMP=(--amp-a "$AMP_ITB" --amp-b "$AMP_ITB")
fi
python3 "$BOOTCTRL" init --output "$WORK/bootctrl.bin" \
  --nuttx-a "$NUTTX" --nuttx-b "$NUTTX" \
  ${BOOTCTRL_AMP[@]+"${BOOTCTRL_AMP[@]}"}
python3 "$BOOTCTRL" inspect "$WORK/bootctrl.bin" >/dev/null

if [ "$TARGET" = emmc ]; then
  PACKAGE="$OUT/nyabula-k7-emmc"
  IMAGE_DIR="$PACKAGE/Image"

  rm -rf "$PACKAGE"
  mkdir -p "$IMAGE_DIR"
  cp "${LOADER_BIN:-$WORK/loader.bin}" "$IMAGE_DIR/MiniLoaderAll.bin"
  cp "$OUT/nboot.img" "$IMAGE_DIR/uboot.img"
  cp "$WORK/trust.img" "$IMAGE_DIR/trust.img"
  cp "$WORK/bootctrl.bin" "$IMAGE_DIR/bootctrl.img"
  cp "$NUTTX" "$IMAGE_DIR/nuttx_a.img"
  cp "$NUTTX" "$IMAGE_DIR/nuttx_b.img"

  cat > "$IMAGE_DIR/parameter.txt" <<'PARAMETER'
FIRMWARE_VER: 1.0
MACHINE_MODEL: KICKPI-K7
MACHINE_ID: 007
MANUFACTURER: Pharos Tech
MAGIC: 0x5041524B
ATAG: 0x00200800
MACHINE: 0xffffffff
CHECK_MASK: 0x80
PWR_HLD: 0,0,A
TYPE: GPT
PARAMETER
  printf 'CMDLINE:mtdparts=rk29xxnand:' >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(uboot),' \
    "$UBOOT_SECTORS" "$UBOOT_START" >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(trust),' \
    "$TRUST_SECTORS" "$TRUST_START" >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(bootctrl),' \
    "$BOOTCTRL_SECTORS" "$BOOTCTRL_START" >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(nuttx_a),' \
    "$NUTTX_SECTORS" "$NUTTX_A_START" >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(nuttx_b),' \
    "$NUTTX_SECTORS" "$NUTTX_B_START" >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(amp_a),' \
    "$AMP_SECTORS" "$AMP_A_START" >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(amp_b),' \
    "$AMP_SECTORS" "$AMP_B_START" >> "$IMAGE_DIR/parameter.txt"
  printf '0x%08x@0x%08x(config),' \
    "$CONFIG_SECTORS" "$CONFIG_START" >> "$IMAGE_DIR/parameter.txt"
  printf '%s@0x%08x(data:grow)\n' - "$DATA_START" \
    >> "$IMAGE_DIR/parameter.txt"

  cat > "$PACKAGE/package-file" <<'PACKAGE_FILE'
package-file package-file
bootloader Image/MiniLoaderAll.bin
parameter Image/parameter.txt
uboot Image/uboot.img
trust Image/trust.img
bootctrl Image/bootctrl.img
nuttx_a Image/nuttx_a.img
nuttx_b Image/nuttx_b.img
PACKAGE_FILE
  if [ -n "${DATA_IMG:-}" ] && [ -f "$DATA_IMG" ]; then
    cp "$DATA_IMG" "$IMAGE_DIR/data.img"
    echo "data Image/data.img" >> "$PACKAGE/package-file"
  fi
  if [ -n "${CONFIG_IMG:-}" ] && [ -f "$CONFIG_IMG" ]; then
    cp "$CONFIG_IMG" "$IMAGE_DIR/config.img"
    echo "config Image/config.img" >> "$PACKAGE/package-file"
  fi
  if [ -n "${AMP_ITB:-}" ] && [ -f "$AMP_ITB" ]; then
    cp "$AMP_ITB" "$IMAGE_DIR/amp_a.img"; cp "$AMP_ITB" "$IMAGE_DIR/amp_b.img"
    printf "amp_a Image/amp_a.img\namp_b Image/amp_b.img\n" >> "$PACKAGE/package-file"
  fi

  cat > "$PACKAGE/README.txt" <<'README'
KICKPI-K7 Nyabula eMMC partition package

Use RKDevTool Download Image mode. Load Image/MiniLoaderAll.bin as Loader,
then load Image/parameter.txt and the named partition images (or import
package-file directly). Partitions listed in package-file carry payload; any
of amp_a / amp_b / config / data missing there are created empty by
parameter.txt.

config.img seeds /config: provisioning state, device identity and persona.
It is deliberately separate from data.img so that wiping the bulk store
(models, music) cannot also wipe provisioning -- losing the models costs an
OTA, losing the wifi settings needs a human on site.

data.img seeds /data: models/ (llm, asr, kws, tts), www/ (the panel the
device serves) and agent/ca.pem (the roots an online model is verified
against).  amp_a / amp_b carry the AMP image and are recorded in bootctrl, so
the board comes up in the AMP domain; nuttx_a / nuttx_b are the plain
firmware N-Boot falls back to.
README

  (cd "$PACKAGE" && sha256sum package-file README.txt Image/* > SHA256SUMS)
  printf 'OK: %s\n' "$PACKAGE"
  exit 0
fi

IMAGE="$OUT/nyabula-k7-sd.img"
truncate -s 4G "$IMAGE"
DATA_END=$((4 * 1024 * 1024 * 1024 / 512 - 2049))
sgdisk -og "$IMAGE" >/dev/null
sgdisk -U 4b374142-0000-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 1:"$UBOOT_START":$((UBOOT_START + UBOOT_SECTORS - 1)) \
  -c 1:uboot -t 1:8300 \
  -u 1:4b374142-0001-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 2:"$TRUST_START":$((TRUST_START + TRUST_SECTORS - 1)) \
  -c 2:trust -t 2:8300 \
  -u 2:4b374142-0002-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 3:"$BOOTCTRL_START":$((BOOTCTRL_START + BOOTCTRL_SECTORS - 1)) \
  -c 3:bootctrl -t 3:8300 \
  -u 3:4b374142-0003-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 4:"$NUTTX_A_START":$((NUTTX_A_START + NUTTX_SECTORS - 1)) \
  -c 4:nuttx_a -t 4:8300 \
  -u 4:4b374142-0004-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 5:"$NUTTX_B_START":$((NUTTX_B_START + NUTTX_SECTORS - 1)) \
  -c 5:nuttx_b -t 5:8300 \
  -u 5:4b374142-0005-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 6:"$AMP_A_START":$((AMP_A_START + AMP_SECTORS - 1)) \
  -c 6:amp_a -t 6:8300 \
  -u 6:4b374142-0006-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 7:"$AMP_B_START":$((AMP_B_START + AMP_SECTORS - 1)) \
  -c 7:amp_b -t 7:8300 \
  -u 7:4b374142-0007-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 8:"$CONFIG_START":$((CONFIG_START + CONFIG_SECTORS - 1)) \
  -c 8:config -t 8:0700 \
  -u 8:4b374142-0008-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 9:"$DATA_START":"$DATA_END" -c 9:data -t 9:0700 \
  -u 9:4b374142-0009-4000-8000-000000000002 "$IMAGE" >/dev/null

dd if="$WORK/idbloader.img" of="$IMAGE" bs=512 seek=64 \
  conv=notrunc status=none
if [ "$(stat -c %s "$WORK/idbloader.img")" -le $((1024 * 512)) ]; then
  dd if="$WORK/idbloader.img" of="$IMAGE" bs=512 seek=1088 \
    conv=notrunc status=none
fi
dd if="$OUT/nboot.img" of="$IMAGE" bs=512 seek="$UBOOT_START" \
  conv=notrunc status=none
dd if="$WORK/trust.img" of="$IMAGE" bs=512 seek="$TRUST_START" \
  conv=notrunc status=none
dd if="$WORK/bootctrl.bin" of="$IMAGE" bs=512 seek="$BOOTCTRL_START" \
  conv=notrunc status=none
dd if="$NUTTX" of="$IMAGE" bs=512 seek="$NUTTX_A_START" \
  conv=notrunc status=none
dd if="$NUTTX" of="$IMAGE" bs=512 seek="$NUTTX_B_START" \
  conv=notrunc status=none

# Both store partitions get a filesystem.  When a seed image was supplied
# it is written as-is, so a disk built here carries the same content the
# eMMC package does; otherwise an empty filesystem is created so the board
# can mount the partition on first boot without formatting it itself.
#
# Both are checked for size before they go in.  A seed larger than its
# partition would be truncated by dd -- silently, since dd does not report
# a short write -- and the resulting filesystem would be corrupt.
CONFIG_BYTES=$((CONFIG_SECTORS * 512))
if [ -n "${CONFIG_IMG:-}" ] && [ -f "$CONFIG_IMG" ]; then
  seed=$(stat -c %s "$CONFIG_IMG")
  [ "$seed" -le "$CONFIG_BYTES" ] || die \
    "config seed is ${seed} bytes, partition holds ${CONFIG_BYTES}"
  cp "$CONFIG_IMG" "$WORK/config.fat"
  truncate -s "$CONFIG_BYTES" "$WORK/config.fat"
else
  truncate -s "$CONFIG_BYTES" "$WORK/config.fat"
  mkfs.fat --invariant -F 32 -S 512 -n NYACONF "$WORK/config.fat" >/dev/null
fi
dd if="$WORK/config.fat" of="$IMAGE" bs=512 seek="$CONFIG_START" \
  conv=notrunc,sparse status=none

DATA_SECTORS=$((DATA_END - DATA_START + 1))
DATA_BYTES=$((DATA_SECTORS * 512))
if [ -n "${DATA_IMG:-}" ] && [ -f "$DATA_IMG" ]; then
  seed=$(stat -c %s "$DATA_IMG")
  [ "$seed" -le "$DATA_BYTES" ] || die \
    "data seed is ${seed} bytes, partition holds ${DATA_BYTES}"
  cp "$DATA_IMG" "$WORK/data.fat"
  truncate -s "$DATA_BYTES" "$WORK/data.fat"
else
  truncate -s "$DATA_BYTES" "$WORK/data.fat"
  mkfs.fat --invariant -F 32 -S 512 -n NYABULA "$WORK/data.fat" >/dev/null
fi
dd if="$WORK/data.fat" of="$IMAGE" bs=512 seek="$DATA_START" \
  conv=notrunc,sparse status=none
sgdisk -v "$IMAGE"

sha256sum "$NUTTX" "$OUT/nboot.img" "$IMAGE" > "$OUT/SHA256SUMS"
printf 'OK: %s\n' "$IMAGE"
