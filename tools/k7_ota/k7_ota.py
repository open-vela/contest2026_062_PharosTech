# -*- coding: utf-8 -*-
"""KICKPI-K7 板上固件热更新 PC 端一键工具 (K7 OTA Pro)。

两条传输通道:
  ADB(默认,USB 高速 ~2.5MB/s):
    adb push <img> /tmp/fw.img → adb shell k7flash(护栏落盘+校验+重启)。
    需板上已运行 adbd(nsh: adbd &)且 USB OTG 口连 PC。
  Ymodem(备用,串口 1.5Mbaud):
    串口触发 rb 收包 → k7flash。ADB 不可用/指定 --ymodem 时走这条。
    进 Ymodem 前自动发 usbsw off 让板下 USB 总线(host 流量会搅坏传输),
    并 adb kill-server 静默 PC 侧链路。

用法:
  python k7_ota.py --img uboot_nuttx.img                 # ADB,失败自动回落 Ymodem(需 --port)
  python k7_ota.py --img uboot_nuttx.img --port COM4     # 同上,含串口回落/收尾
  python k7_ota.py --img uboot_nuttx.img --port COM4 --ymodem   # 强制串口 Ymodem
  (固件 = build_sd 产出的 uboot_nuttx.img,即 FIT。不是整盘 sd_*.img!)

依赖: pyserial(仅 Ymodem/串口收尾用)、adb 在 PATH(仅 ADB 通道用)。
"""
import argparse
import os
import subprocess
import sys
import time

SOH = 0x01   # 128 字节块
STX = 0x02   # 1024 字节块
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRC = 0x43   # 'C',接收方请求 CRC 模式


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if (crc & 0x8000) else (crc << 1) & 0xffff
    return crc & 0xffff


def _wait_byte(ser, targets, timeout=10):
    """等到 targets 里的某个字节,返回它;超时返回 None。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = ser.read(1)
        if b and b[0] in targets:
            return b[0]
    return None


def _send_block(ser, seq, payload):
    """发一个 Ymodem 块(payload 已按 128/1024 补齐),等 ACK。"""
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
    name = b"fw.img"   # 固定名,与板上 k7flash /tmp/fw.img 对齐(rb 按此名存)

    # 等接收方 'C'(CRC 模式请求)
    if _wait_byte(ser, (CRC,), timeout=30) is None:
        raise RuntimeError("未收到接收方 'C',ymodem 未就绪")

    # 块0:文件名 + 大小
    b0 = name + b"\x00" + str(len(data)).encode() + b"\x00"
    b0 = b0.ljust(128, b"\x00")
    _send_block(ser, 0, b0)
    if _wait_byte(ser, (CRC,), timeout=10) is None:
        raise RuntimeError("块0 后未收到 'C'")

    # 数据块(1K)
    seq = 1
    off = 0
    total = len(data)
    while off < total:
        chunk = data[off:off + 1024]
        chunk = chunk.ljust(1024, b"\x1a")   # CPMEOF 填充
        _send_block(ser, seq, chunk)
        off += 1024
        seq = (seq + 1) & 0xff
        sys.stdout.write("\r  发送 %d/%d 字节" % (min(off, total), total))
        sys.stdout.flush()
    print("")

    # EOT
    for _ in range(3):
        ser.write(bytes([EOT]))
        if _wait_byte(ser, (ACK,), timeout=10) == ACK:
            break

    # 结束:全零块0
    _wait_byte(ser, (CRC,), timeout=10)
    _send_block(ser, 0, b"\x00" * 128)


def _cmd(ser, line, wait=1.0):
    ser.reset_input_buffer()
    ser.write(line.encode() + b"\r\n")
    time.sleep(wait)


def _adb(args, timeout=120):
    """跑一条 adb 命令,返回 (rc, 输出文本)。adb 不存在返回 (127, '')。"""
    try:
        r = subprocess.run(["adb"] + args, capture_output=True,
                           timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).decode(errors="replace")
    except FileNotFoundError:
        return 127, ""
    except subprocess.TimeoutExpired:
        return 124, ""


def adb_available():
    rc, out = _adb(["devices"], timeout=15)
    if rc != 0:
        return False
    lines = [l for l in out.splitlines()[1:] if l.strip().endswith("device")]
    return len(lines) > 0


def flash_adb(img):
    """ADB 通道: push + shell k7flash。返回 True=成功。"""
    print("[1/3] adb push %s -> /tmp/fw.img ..." % img)
    rc, out = _adb(["push", img, "/tmp/fw.img"])
    print("  " + out.strip().splitlines()[-1] if out.strip() else "")
    if rc != 0:
        print("  push 失败(rc=%d)" % rc)
        return False

    print("[2/3] adb shell k7flash /tmp/fw.img (落盘+校验+重启) ...")

    # k7flash 校验通过后 3 秒重启,adb 连接会随重启断开——断开不算失败,
    # 以板上回显里的校验结果为准。
    rc, out = _adb(["shell", "k7flash /tmp/fw.img"], timeout=180)
    for line in out.strip().splitlines():
        print("  " + line)
    if "write + verify passed" in out:
        print("[3/3] 校验通过,板子重启中。")
        return True
    if rc != 0 and not out.strip():
        print("  连接随重启断开(无回显),请以串口日志确认。")
        return True

    print("  k7flash 未报校验通过,判为失败。")
    return False


def flash_ymodem(port, baud, img):
    """Ymodem 备用通道(原始流程 + CAN 自愈重试)。"""
    import serial

    # 板下 USB 总线 + PC 侧静默 adb:host 的 USB 流量打印会搅坏 Ymodem。
    _adb(["kill-server"], timeout=15)

    ser = serial.Serial(port, baud, timeout=0.2)
    print("[1/4] 触发板上接收 (usbsw off; cd /tmp; rb) ...")

    # Make sure the console is idle at nsh first: send a bare newline and
    # drain any stale output (a half-finished previous command or a still-
    # running 'rb' would eat the handshake and hang the flash).
    ser.write(b"\r\n")
    time.sleep(0.3)
    ser.reset_input_buffer()

    _cmd(ser, "usbsw off", 0.4)   # 没这命令也无妨,nsh 报 not found 而已
    _cmd(ser, "cd /tmp", 0.4)
    ser.reset_input_buffer()
    ser.write(b"rb\r\n")

    # Give rb a moment to start and emit its first 'C' before we begin.
    time.sleep(0.8)

    print("[2/4] Ymodem 发送 %s ..." % img)
    for attempt in range(3):
        try:
            ymodem_send(ser, img)
            break
        except RuntimeError as e:
            if attempt == 2:
                raise
            # A failed transfer leaves the board's rb receiver waiting and
            # capturing the console, so a plain re-`rb` gets eaten as data.
            # Cancel rb the Ymodem way (8x CAN + 8x backspace) to drop back
            # to nsh, drain, then re-arm rb and retry.
            print("  传输失败(%s),CAN 取消 rb 回 nsh 后重试 ..." % e)
            ser.write(bytes([CAN] * 8 + [0x08] * 8))
            ser.flush()
            time.sleep(0.6)
            ser.reset_input_buffer()
            ser.write(b"\r\n")
            time.sleep(0.3)
            _cmd(ser, "cd /tmp", 0.4)
            ser.reset_input_buffer()
            ser.write(b"rb\r\n")
            time.sleep(0.8)

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
    """重启后经串口把 adbd 拉起来,让下一次 OTA 继续走 ADB。"""
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
        print("adbd 已拉起,下次 OTA 可直接走 ADB。")
    except Exception as e:
        print("串口拉 adbd 失败(%s),需要时手动 nsh: adbd &" % e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="串口(COMx)。Ymodem 通道与重启后拉 adbd 需要")
    ap.add_argument("--baud", type=int, default=1500000)
    ap.add_argument("--img", required=True, help="uboot_nuttx.img (FIT)")
    ap.add_argument("--ymodem", action="store_true",
                    help="强制走串口 Ymodem(需 --port)")
    ap.add_argument("--no-adbd-restart", action="store_true",
                    help="刷完不经串口自动拉起 adbd")
    a = ap.parse_args()

    if not os.path.isfile(a.img):
        sys.exit("固件不存在: %s" % a.img)

    use_adb = not a.ymodem
    if use_adb and not adb_available():
        print("ADB 通道不可用(无设备或无 adb),回落串口 Ymodem。")
        use_adb = False

    if use_adb:
        if flash_adb(a.img):
            if a.port and not a.no_adbd_restart:
                restart_adbd(a.port, a.baud)
            return
        print("ADB 通道失败,回落串口 Ymodem。")

    if not a.port:
        sys.exit("需要 --port 才能走串口 Ymodem。")

    flash_ymodem(a.port, a.baud, a.img)
    if not a.no_adbd_restart:
        restart_adbd(a.port, a.baud)


if __name__ == "__main__":
    main()
