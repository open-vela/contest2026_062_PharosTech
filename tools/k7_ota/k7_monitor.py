# -*- coding: utf-8 -*-
"""K7 串口常驻监视器 — 一个进程独占端口全程,穿过重启持续抓日志 + 可交互。

解决的痛点: reboot 后抓不到启动日志。以前用 serial_send 发命令再 serial_console
抓,两次开关口之间有交接缝隙(Windows 串口独占 + 释放延迟),重启输出正好落缝里丢了;
软复位(PSCI)偶尔还会瞬断 CH340。本脚本全程只开一次口,发完命令继续读,口断了自动
重连,所以能完整抓到 reset 后的启动日志。

用法:
  # 交互模式(像 nsh 终端): 键入即发,整屏实时显示,Ctrl-C 退出
  python k7_monitor.py --port COM8

  # 发命令后持续抓 N 秒再退出(给 Claude 自动抓启动日志用)
  python k7_monitor.py --port COM8 --cmd reboot --duration 25

  # 只听不发,持续抓(配合手动按 reset)
  python k7_monitor.py --port COM8 --duration 25

  # 落盘到指定文件(默认自动写 串口日志/monitor_时间.log)
  python k7_monitor.py --port COM8 --cmd reboot --duration 25 --log boot.log

要点:
  - 全程单进程独占端口,无交接缝隙。
  - 读到 SerialException(口被 reset 拔断) → 自动 close + 重开同名口,不丢后续启动日志。
  - --duration 到点自动退出(自动化抓取);不给则跑到 Ctrl-C(交互)。
  - 所有收到的字节实时打印 + 追加进日志文件(行首带时间戳)。
"""
import argparse
import datetime as dt
import os
import sys
import threading
import time

import serial
from serial.tools import list_ports


def find_ch340():
    for p in list_ports.comports():
        d = (p.description or "").upper()
        if "CH340" in d or "USB-SERIAL" in d:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=1500000)
    ap.add_argument("--cmd", help="连上后先发这条命令(如 reboot)")
    ap.add_argument("--duration", type=float,
                    help="抓多少秒后自动退出;不给则跑到 Ctrl-C(交互)")
    ap.add_argument("--log", help="日志文件路径(默认 串口日志/monitor_时间.log)")
    a = ap.parse_args()

    port = a.port or find_ch340()
    if not port:
        print("未找到 CH340,用 --port 指定。现有端口:", file=sys.stderr)
        for p in list_ports.comports():
            print("  ", p.device, p.description, file=sys.stderr)
        sys.exit(1)

    if a.log:
        logpath = a.log
    else:
        logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "..", "串口日志")
        os.makedirs(logdir, exist_ok=True)
        logpath = os.path.join(
            logdir, "monitor_%s.log" % dt.datetime.now().strftime("%Y%m%d_%H%M%S"))
    logf = open(logpath, "ab")

    stop = threading.Event()
    holder = {"ser": None}          # 当前串口对象(重连时替换)

    def open_port():
        """阻塞式重开同名口,直到成功或 stop。"""
        while not stop.is_set():
            try:
                s = serial.Serial(port, a.baud, timeout=0.1)
                sys.stderr.write("\n[k7_monitor] 已连接 %s @ %d\n" % (port, a.baud))
                sys.stderr.flush()
                return s
            except Exception:
                time.sleep(0.3)
        return None

    def reader():
        pending_ts = True
        while not stop.is_set():
            s = holder["ser"]
            if s is None:
                holder["ser"] = open_port()
                continue
            try:
                data = s.read(4096)
            except Exception:
                # 口被 reset 拔断 -> 重开,后续启动日志接着抓
                sys.stderr.write("\n[k7_monitor] 端口中断,重连中...\n")
                sys.stderr.flush()
                try:
                    s.close()
                except Exception:
                    pass
                holder["ser"] = None
                continue
            if not data:
                continue
            # 落盘(行首时间戳)
            out = bytearray()
            for b in data:
                if pending_ts:
                    out += dt.datetime.now().strftime("[%H:%M:%S.%f] ")[:-4].encode()
                    out += b" "
                    pending_ts = False
                out.append(b)
                if b == 0x0A:
                    pending_ts = True
            logf.write(out)
            logf.flush()
            # 实时显示
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    # 等首次连上
    t0 = time.time()
    while holder["ser"] is None and time.time() - t0 < 5:
        time.sleep(0.05)

    def send(line):
        s = holder["ser"]
        if s is None:
            sys.stderr.write("[k7_monitor] 口未就绪,丢弃: %s\n" % line)
            return
        try:
            s.write(line.encode() + b"\r\n")
        except Exception:
            sys.stderr.write("[k7_monitor] 发送失败(口中断)\n")

    if a.cmd:
        time.sleep(0.2)
        send(a.cmd)

    try:
        if a.duration:
            time.sleep(a.duration)
        else:
            # 交互: 键入即发
            for line in sys.stdin:
                send(line.rstrip("\n"))
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        time.sleep(0.2)
        try:
            if holder["ser"]:
                holder["ser"].close()
        except Exception:
            pass
        logf.close()
        sys.stderr.write("\n[k7_monitor] 结束,日志: %s\n" % logpath)


if __name__ == "__main__":
    main()
