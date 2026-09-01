#!/bin/bash
# Build a KICKPI-K7 (RK3576) SD boot image with NuttX as BL33, entirely from
# rkbin official Rockchip binaries. No device-extracted blobs.
#
#   idbloader.img (DDR + U-Boot SPL)          <- rkbin boot_merger  RK3576MINIALL.ini
#   uboot_nuttx.img (FIT: nuttx + BL31 + OP-TEE) <- authored fit.its + rkbin ATF/OP-TEE
#   trust.img (BL31 + BL32)                   <- rkbin trust_merger RK3576TRUST.ini
#   sd_nuttx.img = GPT + idbloader@64 + uboot@16384 + trust@24576
#
# Verified on board 2026-07-04: boots SD -> SPL(FIT all segments OK, no bad hash)
# -> BL31 v1.24 -> OP-TEE v1.08 -> EL3 exit@EL2 -> NuttShell (NSH).
#
# Usage: ./build_sd.sh <nuttx.bin> <rkbin_dir> <out_dir>
#   Get <rkbin_dir> with ./fetch_rkbin.sh (or point at a full rkbin checkout).
set -Eeuo pipefail
# Report the failing line/command instead of exiting silently (set -e used to
# swallow the reason -- e.g. a merger hitting a read-only rkbin dir).
trap 'rc=$?; echo "ERROR: build_sd.sh failed at line ${LINENO} (exit ${rc}): ${BASH_COMMAND}" >&2; exit "${rc}"' ERR

die() { echo "ERROR: $*" >&2; exit 1; }

abspath() { (cd "$(dirname "$1")" && printf '%s/%s' "$(pwd)" "$(basename "$1")"); }
[ $# -eq 3 ] || die "usage: build_sd.sh <nuttx.bin> <rkbin_dir> <out_dir>"
[ -r "$1" ] || die "nuttx.bin not readable: $1"
[ -d "$2" ] || die "rkbin dir not found: $2 (run ./fetch_rkbin.sh first)"
NUTTX=$(abspath "$1")
RKBIN=$(cd "$2" && pwd)
mkdir -p "$3" 2>/dev/null || die "cannot create out dir: $3"
OUT=$(cd "$3" && pwd)
[ -w "$OUT" ] || die "out dir not writable: $OUT"
MKIMAGE=$RKBIN/tools/mkimage

# Pinned rkbin versions (match RK3576MINIALL.ini / RK3576TRUST.ini).
BL31=$RKBIN/bin/rk35/rk3576_bl31_v1.24.elf
BL32=$RKBIN/bin/rk35/rk3576_bl32_v1.08.bin

# Preflight: host tools + rkbin inputs, with a clear message for each.
for t in dtc sgdisk dd; do
  command -v "$t" >/dev/null 2>&1 || die "missing host tool: $t"
done
for f in "$MKIMAGE" "$RKBIN/tools/boot_merger" "$RKBIN/tools/trust_merger" \
         "$BL31" "$BL32" "$RKBIN/RKBOOT/RK3576MINIALL.ini" \
         "$RKBIN/RKTRUST/RK3576TRUST.ini"; do
  [ -e "$f" ] || die "missing rkbin file: $f (re-run ./fetch_rkbin.sh)"
done

cd "$OUT"

# --- 1. split BL31 elf PT_LOAD segments -> atf-N.bin ---------------------------
# SoC-fixed load addresses (verified identical to the vendor FIT atf-1/2/3):
#   0x40060000 entry/firmware (atf-1) | 0x400f0000 (atf-2) | 0x3fe70000 (atf-3)
# Offsets/sizes below are the elf's PT_LOAD file offset/FileSiz (readelf -l).
dd if="$BL31" of=atf-1.bin bs=1 skip=$((0x20000)) count=$((0x1e020)) status=none
dd if="$BL31" of=atf-2.bin bs=1 skip=$((0x40000)) count=$((0x5000))  status=none
dd if="$BL31" of=atf-3.bin bs=1 skip=$((0x10000)) count=$((0x4000))  status=none
cp "$BL32" optee.bin
cp "$NUTTX" nuttx.bin

# --- 2. dummy control fdt (nuttx does not use it; the FIT config wants one) ----
echo '/dts-v1/; / { compatible = "rockchip,rk3576"; };' | dtc -O dtb -o dummy.dtb 2>/dev/null

# --- 3. author FIT and pack ----------------------------------------------------
# IMPORTANT: no `-E` (external data). Embedded data keeps the FIT self-consistent;
# mkimage recomputes every node hash. This is what avoids the atf-3 "Bad hash"
# that plagued the old surgical vendor-FIT patch (see README).
cat > fit.its <<'ITS'
/dts-v1/;
/ {
    description = "NuttX BL33 + rkbin ATF/OP-TEE for RK3576 (KICKPI-K7)";
    #address-cells = <1>;
    images {
        /* ★ 顺序敏感:atf-1/2/3/optee/fdt(小、定长)排在前,offset 固定;
         * uboot(=nuttx,大、变长)排【最后】。否则 nuttx 变大会把 atf-3 的
         * data-position 往后推,SPL 加载 atf-3 到 0x3fe70000(SRAM)时落点与
         * SPL 缓冲重叠自覆盖 → atf-3 校验/加载失败 → BL31 起不来无限重启。
         */

        atf-1 { description = "ARM Trusted Firmware"; data = /incbin/("atf-1.bin");
            type = "firmware"; arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
            load = <0x40060000>; entry = <0x40060000>; hash { algo = "sha256"; }; };
        atf-2 { description = "ARM Trusted Firmware"; data = /incbin/("atf-2.bin");
            type = "firmware"; arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
            load = <0x400f0000>; hash { algo = "sha256"; }; };
        atf-3 { description = "ARM Trusted Firmware"; data = /incbin/("atf-3.bin");
            type = "firmware"; arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
            load = <0x3fe70000>; hash { algo = "sha256"; }; };
        optee { description = "OP-TEE"; data = /incbin/("optee.bin");
            type = "firmware"; arch = "arm64"; os = "op-tee"; compression = "none";
            load = <0x48400000>; entry = <0x48400000>; hash { algo = "sha256"; }; };
        fdt { description = "dummy fdt"; data = /incbin/("dummy.dtb");
            type = "flat_dt"; arch = "arm64"; compression = "none"; hash { algo = "sha256"; }; };
        uboot { description = "NuttX (BL33)"; data = /incbin/("nuttx.bin");
            type = "standalone"; arch = "arm64"; compression = "none";
            load = <0x40200000>; entry = <0x40200000>; hash { algo = "sha256"; }; };
    };
    configurations {
        default = "conf";
        conf { description = "rk3576 nuttx"; firmware = "atf-1";
            /* ★ 加载顺序敏感:SPL 按 loadables 顺序逐个加载。uboot(=nuttx,大)
             * 必须排【最后】——否则大 nuttx 先加载会越界覆盖 SPL 后续读 fdt/
             * 加载 atf-3 的工作内存,导致 "No matching DT" + 漏加载 atf-3/optee
             * → BL31 缺参数无限重启。小 nuttx 侥幸不越界,大 nuttx 暴露。
             */
            loadables = "atf-2", "atf-3", "optee", "uboot"; fdt = "fdt"; };
    };
};
ITS
# -E: 外部数据(FIT 只留小 header,各段 data 附在 header 之后按 offset 单独读)。
# 关键:大 nuttx 时,内嵌 FIT 会让整个大 uboot data 进 SPL 的 SRAM 处理区 → 加载
# atf-3(0x3fe70000 SRAM)时越界覆盖 SPL 工作内存 → "No matching DT" → 崩。-E 让 SPL
# 只读小 header,不把大 data 塞进 SRAM,避免越界(vendor/手术法就是 -E 才能带大镜像)。
# -p 0x1000: 各段 data 4KB 对齐,布局稳定(避免 M2 当年 -E 无对齐导致 atf-3 落点漂移)。
"$MKIMAGE" -E -p 0x1000 -f fit.its uboot_nuttx.img >/dev/null
python3 - uboot_nuttx.img <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as image:
    header = image.read(8)

if len(header) != 8:
    raise SystemExit("ERROR: generated FIT is truncated")

magic, header_size = struct.unpack(">II", header)
if magic != 0xd00dfeed:
    raise SystemExit("ERROR: generated image is not a FIT")
if header_size > 64 * 1024:
    raise SystemExit(
        "ERROR: generated FIT embeds payloads; mkimage -E was not applied"
    )
PY
echo "FIT: uboot_nuttx.img $(stat -c%s uboot_nuttx.img) bytes"

# --- 4. idbloader + trust from rkbin -----------------------------------------
# The mergers read ini paths relative to the rkbin root AND write temp/output
# files back into it (tools/*.bin, rk3576_idblock_*.img, trust.img). Running
# them directly in $RKBIN therefore fails when it is read-only or root-owned
# (the original silent-exit-as-non-root bug). Run them in a writable copy under
# $OUT instead, so $RKBIN is never written to.
WORK="$OUT/rkbin_work"
rm -rf "$WORK"
cp -r "$RKBIN" "$WORK"
chmod -R u+w "$WORK"
( cd "$WORK" && ./tools/boot_merger  RKBOOT/RK3576MINIALL.ini )
( cd "$WORK" && ./tools/trust_merger RKTRUST/RK3576TRUST.ini )
cp "$WORK"/rk3576_idblock_*.img idbloader.img
cp "$WORK"/trust.img trust.img
rm -rf "$WORK"

# --- 5. assemble SD image (Rockchip standard sector offsets) -------------------
IMG=sd_nuttx.img
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
sgdisk -og "$IMG" >/dev/null 2>&1
# Fixed disk/partition GUIDs -> stable GPT (the FIT header still carries a build
# timestamp, so the whole image is not bit-reproducible; the payloads are).
sgdisk -U 4b494b50-4937-4b37-b364-72616d626f6f "$IMG" >/dev/null 2>&1
sgdisk -n 1:16384:24575 -c 1:uboot  -t 1:8300 -u 1:4b494b50-0001-4b37-b364-72616d626f6f "$IMG" >/dev/null
sgdisk -n 2:24576:32767 -c 2:trust  -t 2:8300 -u 2:4b494b50-0002-4b37-b364-72616d626f6f "$IMG" >/dev/null
sgdisk -n 3:32768:0     -c 3:rootfs -t 3:8300 -u 3:4b494b50-0003-4b37-b364-72616d626f6f "$IMG" >/dev/null
dd if=idbloader.img   of="$IMG" bs=512 seek=64    conv=notrunc status=none
dd if=uboot_nuttx.img of="$IMG" bs=512 seek=16384 conv=notrunc status=none
dd if=trust.img       of="$IMG" bs=512 seek=24576 conv=notrunc status=none
echo "OK -> $OUT/$IMG ($(stat -c%s "$IMG") bytes)"
