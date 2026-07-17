# -*- coding: utf-8 -*-
"""KICKPI-K7 板上固件热更新 PC 端一键工具 (K7 OTA Pro)。

设计目标: 快、稳、不把板子刷坏、不把板子锁死。

默认通道 = ADB(USB), 稳健流程:
  1. adb push <img> /tmp/fw.img
  2. 板上 ls 回读 /tmp/fw.img 实际字节, 与本地文件逐字节大小比对;
     不一致 = 传输被截断 -> 删除板上残file, 重试 push (绝不刷截断的镜像)。
  3. 大小一致才 adb shell k7flash /tmp/fw.img(护栏落盘+校验+重启)。
push 慢 (换线/换口后 USB 可能降速到 KB 级) 时给足超时并重试, 不会误判失败。

Ymodem(串口) 只在显式 --ymodem 时才走, 且只有它才会发 usbsw off。
默认通道绝不自动回退到 Ymodem —— 那条路会 usbsw off 把 adb 切走, 一旦
Ymodem 再失败(1.5Mbaud CH340 边际不可靠)板子就卡在 rb 接收态, 串口失同步,
只能断电。历史教训, 见 PR 讨论。

用法:
  python k7_ota.py --img uboot_nuttx.img                # ADB(默认, 推荐)
  python k7_ota.py --img uboot_nuttx.img --port COM4    # 刷完再经串口拉 adbd
  python k7_ota.py --img uboot_nuttx.img --retries 5    # 弱链路多给几次重试
  python k7_ota.py --img uboot_nuttx.img --ymodem --port COM4   # 显式串口 Ymodem

依赖: adb 在 PATH(ADB 通道); pyserial(仅 --ymodem / --port 拉 adbd 用)。
固件 = build_sd 产出的 uboot_nuttx.img(FIT), 不是整盘 sd_*.img!
"""
import argparse
import os
import subprocess
import sys
import time

SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRC = 0x43

BOARD_IMG = "/tmp/fw.img"


# ---------------------------------------------------------------------------
# ADB channel (default, robust)
# ---------------------------------------------------------------------------

def _adb(args, timeout=120):
    """跑一条 adb 命令, 返回 (rc, 文本)。adb 不存在=(127,''); 超时=(124,'')。"""
    try:
        r = subprocess.run(["adb"] + args, capture_output=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).decode(errors="replace")
    except FileNotFoundError:
        return 127, ""
    except subprocess.TimeoutExpired:
        return 124, ""


def adb_available():
    rc, out = _adb(["devices"], timeout=15)
    if rc != 0:
        return False
    return any(l.strip().endswith("device") for l in out.splitlines()[1:])


def adb_board_filesize(path):
    """回读板上文件字节数; 取不到返回 None。

    NuttX 的 ls -l 形如: `-rwxrwxrwx     1876480 /tmp/fw.img`
    取行内第一个纯数字 token, 兼容不同对齐。
    """
    rc, out = _adb(["shell", "ls -l %s" % path], timeout=30)
    if rc not in (0, 124):
        return None
    for line in out.splitlines():
        if path.split("/")[-1] not in line:
            continue
        for tok in line.split():
            if tok.isdigit():
                return int(tok)
    return None


def adb_push_verified(img, retries):
    """push + 板上大小回读校验, 不一致就重试。全部失败返回 False。"""
    local_size = os.path.getsize(img)
    for attempt in range(1, retries + 1):
        print("[1/3] adb push (%d/%d) %s -> %s ..." %
              (attempt, retries, img, BOARD_IMG))
        # 慢速 USB 下 1.8MB 可能要几百秒; 给足 10 分钟, 别误判超时。
        rc, out = _adb(["push", img, BOARD_IMG], timeout=600)
        last = out.strip().splitlines()[-1] if out.strip() else ""
        if last:
            print("  " + last)

        board_size = adb_board_filesize(BOARD_IMG)
        if board_size == local_size:
            print("  size OK: %d bytes(板上=本地)。" % local_size)
            return True

        print("  size 不符(本地=%d 板上=%s) —— 传输不完整, 清理后重试。" %
              (local_size, board_size))
        _adb(["shell", "rm -f %s" % BOARD_IMG], timeout=30)
        time.sleep(1.0)

    print("  push 校验连续 %d 次失败。检查 USB 线/口、adbd 是否在跑。" % retries)
    return False


def flash_adb(img, retries):
    """ADB 通道: 校验式 push + k7flash。返回 True=成功。"""
    if not adb_push_verified(img, retries):
        return False

    print("[2/3] adb shell k7flash %s (落盘+校验+重启) ..." % BOARD_IMG)
    # k7flash 校验通过后 3 秒重启, adb 会随重启断开——断开不算失败,
    # 以板上回显 "write + verify passed" 为准。
    rc, out = _adb(["shell", "k7flash %s" % BOARD_IMG], timeout=180)
    for line in out.strip().splitlines():
        print("  " + line)

    if "write + verify passed" in out:
        print("[3/3] 校验通过, 板子重启中。")
        return True
    if rc in (124, 255) and not out.strip():
        # 连接随重启断开、无回显 —— k7flash 一般已完成, 以串口日志为准。
        print("[3/3] 连接随重启断开(无回显), 请以串口日志确认。")
        return True

    print("  k7flash 未报校验通过(rc=%d), 判为失败。板上镜像已 push, 可重试。" % rc)
    return False


# ---------------------------------------------------------------------------
# Ymodem channel (explicit --ymodem only; the ONLY path that does usbsw off)
# ---------------------------------------------------------------------------

def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if (crc & 0x8000) \
                else (crc << 1) & 0xffff
    return crc & 0xffff


def _wait_byte(ser, targets, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = ser.read(1)
        if b and b[0] in targets:
            return b[0]
    return None


def _send_block(ser, seq, payload):
    n = len(payload)
    head = STX if n == 1024 else SOH
    frame = bytes([head, seq & 0xff, (~seq) & 0xff]) + payload
    c = crc16(payload)
    frame += bytes([(c >> 8) & 0xff, c & 0xff])
    for _ in range(10):
        ser.write(frame)
        r = _wait_byte(ser, (ACK, NAK, CAN), timeout=10)
        if r == ACK:
            return True
        if r == CAN:
            raise RuntimeError("接收方取消(CAN)")
    raise RuntimeError("块 %d 重试超限" % seq)


def ymodem_send(ser, filepath):
    data = open(filepath, "rb").read()
    name = b"fw.img"
    if _wait_byte(ser, (CRC,), timeout=30) is None:
        raise RuntimeError("未收到接收方 'C', ymodem 未就绪")
    b0 = (name + b"\x00" + str(len(data)).encode() + b"\x00").ljust(128, b"\x00")
    _send_block(ser, 0, b0)
    if _wait_byte(ser, (CRC,), timeout=10) is None:
        raise RuntimeError("块0 后未收到 'C'")
    seq, off, total = 1, 0, len(data)
    while off < total:
        chunk = data[off:off + 1024].ljust(1024, b"\x1a")
        _send_block(ser, seq, chunk)
        off += 1024
        seq = (seq + 1) & 0xff
        sys.stdout.write("\r  发送 %d/%d 字节" % (min(off, total), total))
        sys.stdout.flush()
    print("")
    for _ in range(3):
        ser.write(bytes([EOT]))
        if _wait_byte(ser, (ACK,), timeout=10) == ACK:
            break
    _wait_byte(ser, (CRC,), timeout=10)
    _send_block(ser, 0, b"\x00" * 128)


def _cmd(ser, line, wait=1.0):
    ser.reset_input_buffer()
    ser.write(line.encode() + b"\r\n")
    time.sleep(wait)


def cancel_rb(ser):
    """把板上可能残留的 rb 接收态取消回 nsh(8xCAN + 8x退格)。"""
    ser.write(bytes([CAN] * 8 + [0x08] * 8))
    ser.flush()
    time.sleep(0.6)
    ser.reset_input_buffer()
    ser.write(b"\r\n")
    time.sleep(0.3)


def flash_ymodem(port, baud, img):
    """Ymodem 备用通道(显式 --ymodem)。1.5Mbaud CH340 边际不可靠, 慎用。"""
    import serial

    print("⚠ Ymodem 通道: 会 usbsw off 切走 adb; 1.5Mbaud CH340 可能不稳。")
    _adb(["kill-server"], timeout=15)

    ser = serial.Serial(port, baud, timeout=0.2)
    print("[1/4] 触发板上接收 (usbsw off; cd /tmp; rb) ...")
    ser.write(b"\r\n")
    time.sleep(0.3)
    ser.reset_input_buffer()
    _cmd(ser, "usbsw off", 0.4)
    _cmd(ser, "cd /tmp", 0.4)
    ser.reset_input_buffer()
    ser.write(b"rb\r\n")
    time.sleep(0.8)

    print("[2/4] Ymodem 发送 %s ..." % img)
    ok = False
    try:
        for attempt in range(3):
            try:
                ymodem_send(ser, img)
                ok = True
                break
            except RuntimeError as e:
                print("  传输失败(%s), CAN 取消 rb 回 nsh 后重试 ..." % e)
                cancel_rb(ser)
                _cmd(ser, "cd /tmp", 0.4)
                ser.reset_input_buffer()
                ser.write(b"rb\r\n")
                time.sleep(0.8)
    finally:
        if not ok:
            # 无论如何把板子从 rb 接收态救回 nsh, 别锁死。
            print("  Ymodem 失败, 取消 rb 让板子回到 nsh(避免锁死)。")
            cancel_rb(ser)
            _cmd(ser, "usbsw on", 0.4)
            ser.close()
            raise SystemExit("Ymodem 传输失败。板子已救回 nsh, 建议改用默认 ADB 通道。")

    print("[3/4] 触发 k7flash 落盘+校验+重启 ...")
    _cmd(ser, "k7flash /tmp/fw.img", 0.5)
    print("[4/4] 板上回显(10s):")
    t0 = time.time()
    while time.time() - t0 < 10:
        d = ser.read(4096)
        if d:
            sys.stdout.buffer.write(d)
            sys.stdout.buffer.flush()
    ser.close()
    print("\n完成。板子应已重启到新固件。")


def restart_adbd(port, baud):
    """重启后经串口把 adbd 拉起来, 让下一次 OTA 继续走 ADB。"""
    import serial
    print("等板子重启完(15s)后经串口重启 adbd ...")
    time.sleep(15)
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
        ser.write(b"\r\n")
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"adbd &\r\n")
        time.sleep(1.0)
        ser.close()
        print("adbd 已拉起, 下次 OTA 可直接走 ADB。")
    except Exception as e:
        print("串口拉 adbd 失败(%s), 需要时手动 nsh: adbd &" % e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", required=True, help="uboot_nuttx.img (FIT)")
    ap.add_argument("--port", help="串口(COMx)。--ymodem 与刷完拉 adbd 需要")
    ap.add_argument("--baud", type=int, default=1500000)
    ap.add_argument("--retries", type=int, default=3,
                    help="ADB push 校验失败的重试次数(默认 3)")
    ap.add_argument("--ymodem", action="store_true",
                    help="显式走串口 Ymodem(需 --port; 会 usbsw off, 慎用)")
    ap.add_argument("--no-adbd-restart", action="store_true",
                    help="刷完不经串口自动拉起 adbd")
    a = ap.parse_args()

    if not os.path.isfile(a.img):
        sys.exit("固件不存在: %s" % a.img)

    if a.ymodem:
        if not a.port:
            sys.exit("--ymodem 需要 --port。")
        flash_ymodem(a.port, a.baud, a.img)
        if not a.no_adbd_restart:
            restart_adbd(a.port, a.baud)
        return

    # 默认: ADB 通道(不自动回退 Ymodem, 避免锁板)。
    if not adb_available():
        sys.exit("ADB 无设备。检查 USB 线/口、板上 adbd 是否在跑(nsh: adbd &)。"
                 "\n确要走串口请显式加 --ymodem --port COMx。")

    if flash_adb(a.img, a.retries):
        if a.port and not a.no_adbd_restart:
            restart_adbd(a.port, a.baud)
        return

    sys.exit("ADB 刷写失败。镜像可能已 push 到板上, 直接重跑本命令重试即可。"
             "\n(未触碰 usbsw, 板子仍在 adb 可达状态。)")


if __name__ == "__main__":
    main()
