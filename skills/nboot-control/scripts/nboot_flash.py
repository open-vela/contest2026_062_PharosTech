#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""N-Boot / KICKPI-K7 刷写桥 — stage over fastboot, persist with mmc write,
verify with the bootloader's own crc32.

为什么存在
==========
eMMC 空白时 `nboot_storage_check_layout()` (arch/arm/mach-rockchip/rk3576/
nboot_storage.c) 会拒绝一切 `oem board:flash:*`：它要求 uboot/trust/bootctrl/
nuttx_a/nuttx_b 的 **名称、起始扇区、扇区数三者精确匹配** 才认这个介质。
空白盘没有分区表 → 全部 fastboot 写路径直接 `FAIL invalid recovery partition
layout`。要先有分区表才能写，而写分区表本身没有 fastboot 命令。

MiniLoader 更难：它落在 LBA 64，不属于任何 GPT 分区（分区表最多覆盖 LBA 34
起的空间之外，而 loader 在保护性 MBR 之后、第一个分区之前），fastboot 的
`flash <part>` 按名字找 GPT 分区，够不到它。

rockusb 通道也不通：N-Boot 的 USB gadget 用 CONFIG_USB_GADGET_VENDOR_NUM
= 0x18d1 / PRODUCT_NUM = 0xd00d，而 Rockchip 的 upgrade_tool / RKDevTool 按
VID 0x2207 过滤，永远看不到这颗设备。

本工具绕开全部三条限制
======================
`fastboot stage FILE` 把任意文件下载到 CONFIG_FASTBOOT_BUF_ADDR (0x60000000)，
不要求目标介质有任何分区表。退出 fastboot 后 RAM 内容保留，再从板内控制台
`mmc write <addr> <lba> <cnt>` 写到任意 LBA —— 包括不属于任何分区的 LBA 64。

校验用板子自己的 `crc32 <addr> <count>`（纯内存 CRC32），与 PC 端 zlib.crc32
逐字节同源，所以是全量校验而非抽样。

全程不需要 PC 管理员权限。

用法
====
  nboot_flash.py --port COM11 write 0x40 FILE           # 写入并校验
  nboot_flash.py --port COM11 verify 0x40 FILE          # 只校验现有内容
  nboot_flash.py --port COM11 read 0 4                  # dump 扇区(hex)

LBA 接受 0x 前缀（与 N-Boot 的习惯一致）。
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import time
import zlib

import serial

RAM_STAGE = 0x60000000
BLKSZ = 512

# fastboot 的子进程链路过不了非 ASCII 路径（实测中文目录名会变成 `????`
# 然后 `cannot load`）。分块文件一律先落在这个纯 ASCII 的临时目录里。
STAGE_DIR = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "nboot-stage"


def log(msg):
    sys.stderr.write("[nboot] %s\n" % msg)
    sys.stderr.flush()


class Console:
    """N-Boot 串口控制台。单进程独占，避免与 fastboot 抢端口。

    U-Boot 只认 CR 作为行结束，不认 CRLF。用 settle+drain 而不是固定
    长等待，让慢命令（mmc write）自己决定何时有输出。
    """

    def __init__(self, port, baud=1500000):
        self.dev = serial.Serial(port, baud, timeout=0.2)

    def close(self):
        try:
            self.dev.close()
        except Exception:
            pass

    def drain(self, seconds=0.5):
        end = time.monotonic() + seconds
        out = bytearray()
        while time.monotonic() < end:
            chunk = self.dev.read(8192)
            if chunk:
                out.extend(chunk)
            else:
                time.sleep(0.02)
        return bytes(out)

    def clear(self):
        self.dev.reset_input_buffer()

    def send(self, text, wait=1.0, settle=0.3):
        self.clear()
        self.dev.write(text.encode() + b"\r")
        self.dev.flush()
        time.sleep(settle)
        return self.drain(wait)

    def until(self, marker, timeout=30.0, poll=0.2):
        """读到 marker 出现或超时。用于等 mmc write 这类耗时命令。"""
        end = time.monotonic() + timeout
        out = bytearray()
        while time.monotonic() < end:
            chunk = self.dev.read(8192)
            if chunk:
                out.extend(chunk)
                if marker in out:
                    return bytes(out), True
            else:
                time.sleep(poll)
        return bytes(out), False


class Fastboot:
    def __init__(self, timeout=120):
        self.timeout = timeout

    def _run(self, *args, timeout=None):
        try:
            p = subprocess.run(["fastboot"] + list(args), capture_output=True,
                               text=True, timeout=timeout or self.timeout)
        except subprocess.TimeoutExpired:
            return 124, "", "timeout"
        return p.returncode, p.stdout, p.stderr

    def devices(self):
        rc, out, _ = self._run("devices", timeout=15)
        return [l.split("\t")[0] for l in out.splitlines() if l.strip()]

    def wait(self, seconds=20):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.devices():
                return True
            time.sleep(1)
        return False

    def stage(self, path):
        rc, out, err = self._run("stage", path)
        return rc, (out + err).strip()


def parse_int(text):
    return int(text, 0)


def board_crc32(con, addr, size):
    """让板子算 RAM 里的 CRC32，返回 int 或 None。

    U-Boot 的输出形如：
        crc32 for 60000000 ... 60001fff ==> ccce3aa5
    地址段随长度变化，所以只锚定末尾的 `==>` 结果。
    """
    out = con.send("crc32 0x%x 0x%x" % (addr, size), wait=1.5)
    m = re.search(rb"==>\s*([0-9a-fA-F]+)", out)
    if not m:
        log("  could not parse crc32 output: %r" % out[-200:])
        return None
    return int(m.group(1), 16)


def file_crc32(path, size=None):
    crc = 0
    total = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            if size is not None and total + len(chunk) > size:
                chunk = chunk[:size - total]
            crc = zlib.crc32(chunk, crc)
            total += len(chunk)
            if size is not None and total >= size:
                break
    return crc & 0xFFFFFFFF


def _read_sectors_to(con, lba, sectors, addr):
    """把介质上的 sectors 读进 RAM 的 addr，等 U-Boot 报告完成。"""
    con.clear()
    con.dev.write(("mmc read 0x%x 0x%x 0x%x\r" % (addr, lba, sectors)).encode())
    con.dev.flush()
    out, ok = con.until(b"blocks read: OK", timeout=30)
    if not ok:
        log("  mmc read did not complete: %r" % out[-200:])
    return ok


def cmd_write(args):
    """stage 到 RAM → 校验 RAM → 退出 fastboot → mmc write → 回读校验 CRC32。

    大于 `max-download-size`（实测 0x04000000 = 64 MiB）的文件必须分块：
    fastboot 的下载缓冲装不下，`stage` 会直接报 cannot load。分块同时让每块
    的校验独立成立，出错时能指出是第几块坏了。
    """
    STAGE_DIR.mkdir(parents=True, exist_ok=True)
    size = os.path.getsize(args.file)
    sectors = (size + BLKSZ - 1) // BLKSZ
    lba = parse_int(args.lba)
    chunk_sectors = args.chunk_bytes // BLKSZ
    expect = file_crc32(args.file)
    log("file=%s size=%d crc32=0x%08x" % (args.file, size, expect))
    log("target: mmc %d, LBA 0x%x..0x%x (%d sectors)"
        % (args.dev, lba, lba + sectors - 1, sectors))
    if args.check_only:
        log("check-only: nothing written")
        return 0

    con = Console(args.port, args.baud)
    try:
        chunk = 0
        done = 0
        while done < sectors:
            take = min(chunk_sectors, sectors - done)
            chunk += 1
            offset = done * BLKSZ
            piece = _extract(args.file, offset,
                             min(take * BLKSZ, size - offset))
            staged = STAGE_DIR / ("chunk-%d.bin" % chunk)
            staged.write_bytes(piece)

            log("chunk %d: LBA 0x%x..0x%x (%d sectors, %d bytes)"
                % (chunk, lba + done, lba + done + take - 1, take, len(piece)))

            # --- 进 fastboot，下载本块 ---
            con.send("fastboot usb 0", wait=1.5)
            fb = Fastboot()
            if not fb.wait(20):
                log("ERROR: no fastboot device appeared")
                return 1
            rc, msg = fb.stage(str(staged))
            log("  %s" % msg.rstrip())
            if rc != 0:
                log("ERROR: stage failed on chunk %d" % chunk)
                return 1

            # --- 退出 fastboot，RAM 保留 ---
            con.clear()
            con.dev.write(b" ")          # 任意键中止 fastboot
            con.dev.flush()
            out, ok = con.until(b"N-Boot>", timeout=15)
            if not ok:
                log("ERROR: console did not return: %r" % out[-200:])
                return 1

            # --- 校验下载到 RAM 的内容（全量，非抽样）---
            crc = board_crc32(con, RAM_STAGE, len(piece))
            if crc is None:
                return 1
            if crc != (zlib.crc32(piece) & 0xFFFFFFFF):
                log("ERROR: staged copy differs from the host file (chunk %d)"
                    % chunk)
                return 1

            # --- 写盘 ---
            con.send("mmc dev %d" % args.dev, wait=1.0)
            con.clear()
            con.dev.write(("mmc write 0x%x 0x%x 0x%x\r"
                           % (RAM_STAGE, lba + done, take)).encode())
            con.dev.flush()
            out, ok = con.until(b"blocks written: OK", timeout=180)
            if not ok:
                sys.stdout.buffer.write(out[-400:])
                log("ERROR: mmc write did not report OK on chunk %d" % chunk)
                return 1

            # --- 从介质回读并校验本块 ---
            ok = _read_sectors_to(con, lba + done, take, 0x52000000)
            if not ok:
                return 1
            crc = board_crc32(con, 0x52000000, len(piece))
            if crc is None:
                return 1
            want = zlib.crc32(piece) & 0xFFFFFFFF
            if crc != want:
                log("ERROR: read-back differs (chunk %d): got 0x%08x want 0x%08x"
                    % (chunk, crc, want))
                return 1
            log("  chunk %d VERIFY OK" % chunk)
            done += take

        log("all %d chunks written and verified" % chunk)
        return 0
    finally:
        con.close()


def _extract(path, offset, length):
    with open(path, "rb") as f:
        f.seek(offset)
        return f.read(length)


def cmd_verify(args):
    """只回读现有内容并校验，不写。"""
    size = os.path.getsize(args.file)
    sectors = (size + BLKSZ - 1) // BLKSZ
    lba = parse_int(args.lba)
    expect = file_crc32(args.file)
    con = Console(args.port, args.baud)
    try:
        if not _read_sectors_to(con, lba, sectors, 0x52000000):
            return 1
        crc = board_crc32(con, 0x52000000, size)
        log("media crc32 = 0x%08x  file crc32 = 0x%08x" % (crc or 0, expect))
        ok = crc == expect
        log("VERIFY %s" % ("OK" if ok else "MISMATCH"))
        return 0 if ok else 1
    finally:
        con.close()


def cmd_read(args):
    """dump 扇区（hex）到 stdout，用于确认目标介质。"""
    lba = parse_int(args.lba)
    con = Console(args.port, args.baud)
    try:
        con.send("mmc dev %d" % args.dev, wait=1.0)
        out, ok = con.until(b"blocks read: OK", timeout=30)
        del out
        con.clear()
        con.dev.write(("mmc read 0x52000000 0x%x 0x%x\r"
                       % (lba, args.sectors)).encode())
        con.dev.flush()
        out, ok = con.until(b"blocks read: OK", timeout=30)
        if not ok:
            log("ERROR: read failed")
            return 1
        out = con.send("md 0x52000000 0x%x" % (args.sectors * BLKSZ // 4),
                       wait=2.0)
        sys.stdout.buffer.write(out)
        return 0
    finally:
        con.close()


def cmd_gpt(args):
    """在板上写 GPT。

    布局串只能作为命令字面量传给 `gpt write`，而 1.5 Mbaud 的 CH340 链路会
    丢字符（实测 `0x100000` 变成 `0100000`），所以先 `setenv`，把串口回显
    读回来逐字符比对，确认板子收到的就是完整那一串，再 `gpt write`。
    不回读就执行等于闭眼写分区表。
    """
    layout = open(args.layout, "rb").read().split(b"\x00", 1)[0].decode()
    con = Console(args.port, args.baud)
    try:
        for attempt in range(1, args.retries + 1):
            con.send('setenv gptp "%s"' % layout, wait=1.5)
            out = con.send("printenv gptp", wait=1.5)
            echo = extract_env(out, "gptp")
            if echo == layout:
                log("layout verified on the board (attempt %d, %d bytes)"
                    % (attempt, len(layout)))
                break
            log("attempt %d: board received %d/%d bytes, retrying"
                % (attempt, len(echo) if echo is not None else -1, len(layout)))
        else:
            log("ERROR: could not get an intact layout string across")
            return 1

        con.clear()
        con.dev.write(b"gpt write mmc %d $gptp\r" % args.dev)
        con.dev.flush()
        out, ok = con.until(b"N-Boot>", timeout=60)
        sys.stdout.buffer.write(out)
        if b"Writing GPT" not in out or b"error" in out.lower():
            log("ERROR: gpt write did not succeed")
            return 1
        log("gpt write ok")
        return 0
    finally:
        con.close()


def extract_env(out, name):
    """从 `printenv <name>` 的输出里取出变量值。

    U-Boot 回显格式是 `gptp=<value>`；值里本身含 `=`，所以只按第一个分隔。
    """
    text = out.decode("utf-8", "replace")
    for line in text.splitlines():
        if line.startswith(name + "="):
            return line[len(name) + 1:].rstrip("\r")
    return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM11")
    ap.add_argument("--baud", type=int, default=1500000)
    ap.add_argument("--dev", type=int, default=1, help="mmc device (1=eMMC)")
    sub = ap.add_subparsers(dest="action", required=True)

    p = sub.add_parser("write")
    p.add_argument("lba")
    p.add_argument("file")
    p.add_argument("--check-only", action="store_true")
    p.add_argument("--chunk-bytes", type=lambda s: int(s, 0),
                   default=0x3C00000,
                   help="per-transfer size; stays under max-download-size "
                        "(0x04000000). Default 60 MiB.")
    p.set_defaults(func=cmd_write)

    p = sub.add_parser("verify")
    p.add_argument("lba")
    p.add_argument("file")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("read")
    p.add_argument("lba")
    p.add_argument("sectors", type=lambda s: int(s, 0))
    p.set_defaults(func=cmd_read)

    p = sub.add_parser("gpt", help="write the partition table")
    p.add_argument("layout", help="file holding the layout string")
    p.add_argument("--retries", type=int, default=8)
    p.set_defaults(func=cmd_gpt)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
