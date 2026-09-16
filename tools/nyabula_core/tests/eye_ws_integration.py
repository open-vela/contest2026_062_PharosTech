#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real NuttX sim + Core + Eye + WS; only physical LCDs use probe sinks.

Run on the designated builder with SIM_NETUSRSOCK and EXAMPLES_EYEPROBE.
No dependency on the Go Simulator; no hardware writes or external listener.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import socket
import struct
import subprocess
import tempfile
import time


class Peer:
    def __init__(self, port, origin="http://127.0.0.1:5180", key=None):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.socket.settimeout(3)
        key = key or base64.b64encode(secrets.token_bytes(16)).decode()
        self.socket.sendall((f"GET /nyalink HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                             f"Origin: {origin}\r\nUpgrade: websocket\r\n"
                             f"Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                             f"Sec-WebSocket-Key: {key}\r\n\r\n").encode())
        reply = b""
        while not reply.endswith(b"\r\n\r\n"):
            part = self.socket.recv(1)
            if not part:
                self.socket.close()
                raise ConnectionError("upgrade rejected")
            reply += part
        expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert b"101 Switching Protocols" in reply and expected in reply, reply
        self.states = []

    def close(self):
        self.socket.close()

    def send(self, payload, opcode=1, final=True, masked=True):
        mask = b"\x11\x22\x33\x44"
        flags = 128 if masked else 0
        head = bytes([(128 if final else 0) | opcode])
        if len(payload) < 126:
            head += bytes([flags | len(payload)])
        else:
            head += bytes([flags | 126]) + struct.pack("!H", len(payload))
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            head += mask
        self.socket.sendall(head + payload)

    def exact(self, size):
        out = b""
        while len(out) < size:
            part = self.socket.recv(size - len(out))
            if not part:
                raise ConnectionError("peer closed")
            out += part
        return out

    def receive(self):
        first, second = self.exact(2)
        assert not second & 128
        length = second & 127
        if length == 126:
            length = struct.unpack("!H", self.exact(2))[0]
        data = self.exact(length)
        if first & 15 == 10:
            return {"type": "pong", "data": data}
        assert first == 129, first
        result = json.loads(data)
        if result.get("topic") == "eye.state":
            self.states.append(result["data"])
        return result

    def request(self, topic, data, request_id="test", fragmented=False):
        payload = json.dumps({"v": 1, "type": "req", "id": request_id, "topic": topic, "data": data}).encode()
        if fragmented:
            self.send(payload[:17], final=False)
            self.send(b"interleaved", opcode=9)
            self.send(payload[17:], opcode=0)
        else:
            self.send(payload)
        while True:
            reply = self.receive()
            if reply.get("id") == request_id:
                return reply

    def state(self, condition=lambda s: True):
        end = time.monotonic() + 4
        while time.monotonic() < end:
            result = self.receive()
            if result.get("topic") == "eye.state" and condition(result["data"]):
                return result["data"]
        raise AssertionError("state deadline")


def main():
    args = argparse.ArgumentParser()
    args.add_argument("binary", type=Path)
    args.add_argument("--output", type=Path, required=True)
    args.add_argument("--browser-session", type=Path)
    args.add_argument("--hold-seconds", type=int, default=0)
    options = args.parse_args()
    options.output.mkdir(parents=True, exist_ok=True)
    # Reserve a free host port; sim usrsock uses that same native namespace.
    reserve = socket.socket()
    reserve.bind(("127.0.0.1", 0))
    port = reserve.getsockname()[1]
    reserve.close()
    log = bytearray()
    with tempfile.TemporaryDirectory(prefix="nyabula-eye-ws-") as directory:
        token = secrets.token_hex(32)
        token_file = Path(directory) / "token"
        token_file.write_text(token)
        token_file.chmod(0o600)
        process = subprocess.Popen([str(options.binary.resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
                                   preexec_fn=lambda: os.set_blocking(0, False))

        def drain(seconds=.1):
            end = time.monotonic() + seconds
            out = bytearray()
            while time.monotonic() < end:
                if select.select([process.stdout], [], [], .03)[0]:
                    data = os.read(process.stdout.fileno(), 65536)
                    if not data:
                        break
                    out.extend(data)
                    log.extend(data)
            return out.decode(errors="replace")

        def command(text, wait=.3):
            process.stdin.write((text + "\n").encode())
            process.stdin.flush()
            out = drain(wait)
            end = time.monotonic() + 8
            while "nsh> " not in out and time.monotonic() < end:
                out += drain()
            assert "nsh> " in out, (text, out)
            return out

        peer = None
        checks = []
        try:
            drain(.5)
            command("mkdir /host")
            command(f"mount -t hostfs -o fs={directory} /host")
            command("eyeprobe init")
            assert "Eye Engine attached" in command("nyabula_eye &", 2)
            start = command(f"nyabula_eye_ws {port} /host/token http://127.0.0.1:5180 &", 1)
            assert "Nyabula Eye WS:" in start, start
            try:
                Peer(port, "http://untrusted.invalid")
                raise AssertionError("foreign Origin accepted")
            except ConnectionError:
                checks.append("origin-denied")
            try:
                Peer(port, key="!" * 22 + "==")
                raise AssertionError("invalid WebSocket key accepted")
            except ConnectionError:
                checks.append("invalid-key-denied")
            peer = Peer(port)
            reply = peer.request("sys.hello", {"token": "0" * 64})
            assert reply["type"] == "err" and reply["data"]["code"] == "EACCES", reply
            peer.close()
            checks.append("token-denied")
            peer = Peer(port)
            assert peer.request("sys.hello", {"token": token})["type"] == "res"
            initial = peer.state()
            assert initial["schema"] == "nyabula.eye.v1"
            assert peer.request("sys.ping", {}, fragmented=True)["type"] == "res"
            checks.append("fragmented-text-interleaved-ping")
            for mode in ["idle", "curious", "happy", "processing", "star", "heart", "sleepy", "sleep", "angry", "sad", "surprise", "dizzy", "derp"]:
                reply = peer.request("eyes.expression", {"expression": mode, "priority": 255, "source": "forged"}, mode)
                assert reply["data"]["queued"] is True, reply
                state = peer.state(lambda s: s["last_request_id"] == mode)
                assert state["expression"] == mode and state["last_status"] == 0, state
                assert state["expression_owner"]["source"] == "webui"
                assert state["expression_owner"]["priority"] == 40
            checks.append("expressions-13-auth-derived-owner")
            assert peer.request("eyes.gaze", {"x": .65, "y": -.4, "hold_ms": 250}, "gaze")["type"] == "res"
            state = peer.state(lambda s: s["last_request_id"] == "gaze" and s["gaze_active"])
            assert abs(state["gaze_x"] - .65) < 1e-5 and abs(state["gaze_y"] + .4) < 1e-5
            assert state["gaze_until_ms"] > state["uptime_ms"]
            expired = peer.state(lambda s: not s["gaze_active"])
            assert expired["seq"] > state["seq"]
            checks.append("gaze-target-and-expiry")
            assert peer.request("eyes.gaze", {"x": 2, "y": 0, "hold_ms": 100})["type"] == "err"
            assert peer.request("core.reset", {})["data"]["code"] == "ENOTFOUND"
            checks.append("range-and-admin-denied")
            peer.request("eyes.scene.show", {"scene": "caption", "style": "minimal", "payload": {"current_line": "Hello Nyabula"}}, "caption")
            state = peer.state(lambda s: s["last_request_id"] == "caption")
            assert state["scene"] == "caption" and state["scene_payload"]["current_line"] == "Hello Nyabula"
            (options.output / "snapshot.json").write_text(json.dumps(state, indent=2))
            peer.close()
            peer = Peer(port)
            peer.request("sys.hello", {"token": token})
            restored = peer.state()
            assert restored["scene"] == "caption", restored
            assert restored["scene_since_ms"] == state["scene_since_ms"]
            checks.append("reconnect-authoritative-snapshot")
            frame = command("eyeprobe save /host", .5)
            assert frame.count("EYE_FRAME") == 2, frame
            checks.append("real-renderer-dual-lcd-sinks")
            peer.close()
            peer = Peer(port)
            peer.send(b"{}", masked=False)
            try:
                peer.receive()
                raise AssertionError("unmasked client frame accepted")
            except (ConnectionError, ConnectionResetError):
                checks.append("unmasked-denied")
            peer.close()
            peer = None
            print("NATIVE_EYE_WS_PASS " + " ".join(checks))
            (options.output / "result.json").write_text(json.dumps({"passed": checks}, indent=2))
            if options.browser_session and options.hold_seconds > 0:
                options.browser_session.write_text(json.dumps({"port": port, "token": token}))
                options.browser_session.chmod(0o600)
                print("BROWSER_SESSION_READY", flush=True)
                end = time.monotonic() + min(options.hold_seconds, 300)
                while time.monotonic() < end and options.browser_session.exists():
                    drain(.1)
        finally:
            if options.browser_session:
                options.browser_session.unlink(missing_ok=True)
            if peer:
                peer.close()
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            drain(.1)
            (options.output / "serial.log").write_bytes(log)
            print(log.decode(errors="replace")[-1600:])


if __name__ == "__main__":
    main()
