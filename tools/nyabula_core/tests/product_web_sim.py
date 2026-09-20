#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native Core WS: headless operation, client isolation, CAS and detached jobs."""
import argparse
import json
from pathlib import Path
import secrets
import socket
import tempfile
import time
from eye_ws_integration import Peer
from product_records_sim import Simulator


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--browser-session", type=Path)
    parser.add_argument("--hold-seconds", type=int, default=180)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nyabula-product-web-") as directory:
        token = secrets.token_hex(32)
        (Path(directory) / "token").write_text(token)
        reserve = socket.socket()
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
        reserve.close()
        sim = Simulator(args.binary.resolve(), directory)
        peers = []

        def connect():
            peer = Peer(port)
            peers.append(peer)
            hello = peer.request("sys.hello", {"token": token})
            assert hello["type"] == "res" and "core.product-v1" in hello["data"]["capabilities"], hello
            return peer

        def call(peer, topic, data):
            reply = peer.request(topic, data)
            assert reply["type"] == "res", reply
            return reply["data"]

        try:
            sim.request("system.time.set", {"unix_ms": int(time.time() * 1000)})
            launch = f"nyabula_web {port} /host/token http://127.0.0.1:5180 &"
            sim.command(launch)
            time.sleep(.15)
            first, second = connect(), connect()
            info = call(first, "sys.info", {})
            assert info["clock_valid"] and info["memory"]["total"] > 0
            original = call(first, "memory.list", {})
            saved = call(second, "memory.create", {"revision": original["revision"], "record": {"text": "Shared record"}})
            conflict = first.request("memory.create", {"revision": original["revision"], "record": {"text": "Stale writer"}})
            assert conflict["type"] == "err" and conflict["data"]["code"] == "ECONFLICT", conflict
            assert call(first, "memory.list", {}) == saved
            current = call(first, "timer.list", {})
            countdown = call(first, "timer.create", {"revision": current["revision"], "kind": "countdown", "duration_ms": 500, "label": "Disconnect"})
            first.close()
            peers.remove(first)
            time.sleep(1)
            finished = call(second, "timer.list", {})
            assert finished["revision"] > countdown["revision"] and finished["items"][0]["status"] == "finished"
            live = call(second, "timer.create", {"revision": finished["revision"], "kind": "countdown", "duration_ms": 20000, "label": "Web restart"})
            live_id = live["items"][-1]["id"]
            sim.command("nycore web-stop")
            second.close()
            peers.clear()
            time.sleep(.4)
            assert sim.request("product.status", {})["running"]
            sim.command(launch)
            time.sleep(.15)
            reconnected = connect()
            restored = call(reconnected, "timer.list", {})
            item = next(row for row in restored["items"] if row["id"] == live_id)
            assert item["status"] == "running" and 0 < item["remaining_ms"] < 20000
            assert call(reconnected, "memory.list", {}) == saved
            for _ in range(3):
                connect()
            try:
                extra = Peer(port)
                extra.close()
                raise AssertionError("client limit was not enforced")
            except (ConnectionError, ConnectionResetError):
                pass
            for peer in peers:
                peer.close()
            peers.clear()
            if args.browser_session:
                args.browser_session.write_text(json.dumps({"port": port, "token": token}))
                args.browser_session.chmod(0o600)
                print("BROWSER_SESSION_READY", flush=True)
                end = time.monotonic() + min(args.hold_seconds, 300)
                while time.monotonic() < end and args.browser_session.exists():
                    sim.drain(.1)
            sim.command("nycore web-stop")
            sim.request("product.stop", {})
            print("PRODUCT_WEB_SIM_PASS headless multi-client CAS detached-timer web-restart client-limit")
        finally:
            if args.browser_session:
                args.browser_session.unlink(missing_ok=True)
            for peer in peers:
                peer.close()
            sim.close()
            (args.output / "serial.log").write_bytes(sim.log)


if __name__ == "__main__":
    main()
