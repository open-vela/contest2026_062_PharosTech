# -*- coding: utf-8 -*-
"""KICKPI-K7 板上固件热更新 PC 端一键工具。

流程:串口触发板上 ymodem 接收(rb) → Ymodem-1K 发 uboot_nuttx.img 到 /tmp/fw.img
     → 触发 k7flash /tmp/fw.img(护栏落盘+校验+重启) → 打印板上回显。

用法:
  python k7_ota.py --port COM3 --img ..\\烧录\\AB\\out\\uboot_nuttx.img
  (固件 = build_sd 产出的 uboot_nuttx.img,即 FIT。不是整盘 sd_*.img!)

依赖: pyserial。协议自包含(手写 Ymodem-1K 发送),无需 lrzsz。
"""
import argparse
import os
import sys
import time

import serial

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=1500000)
    ap.add_argument("--img", required=True, help="uboot_nuttx.img (FIT)")
    a = ap.parse_args()

    if not os.path.isfile(a.img):
        sys.exit("固件不存在: %s" % a.img)

    ser = serial.Serial(a.port, a.baud, timeout=0.2)
    print("[1/4] 触发板上接收 (cd /tmp; rb) ...")
    _cmd(ser, "cd /tmp", 0.5)
    ser.write(b"rb\r\n")
    time.sleep(0.5)

    print("[2/4] Ymodem 发送 %s ..." % a.img)
    ymodem_send(ser, a.img)

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


if __name__ == "__main__":
    main()
