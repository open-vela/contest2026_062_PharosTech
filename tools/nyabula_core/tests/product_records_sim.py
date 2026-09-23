#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise native Core product records and SQLite across sim restarts."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import sqlite3
import subprocess
import tempfile
import time


class Simulator:
    def __init__(self, binary, directory):
        self.root = Path(directory)
        self.log = bytearray()
        self.process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
                                        preexec_fn=lambda: os.set_blocking(0, False))
        self.drain(.5)
        self.command("mkdir /host")
        self.command(f"mount -t hostfs -o fs={self.root} /host")

    def drain(self, seconds=.1):
        end = time.monotonic() + seconds
        out = bytearray()
        while time.monotonic() < end:
            if select.select([self.process.stdout], [], [], .02)[0]:
                data = os.read(self.process.stdout.fileno(), 65536)
                if not data:
                    break
                self.log.extend(data)
                out.extend(data)
        return out.decode(errors="replace")

    def command(self, text):
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        out = self.drain(.15)
        end = time.monotonic() + 8
        while "nsh> " not in out and time.monotonic() < end:
            out += self.drain()
        if "nsh> " not in out:
            try:
                trace = subprocess.run(["gdb", "-batch", "-ex", "thread apply all bt", "-p", str(self.process.pid)], capture_output=True, timeout=10)
                self.log.extend(trace.stdout + trace.stderr)
            except (OSError, subprocess.TimeoutExpired) as error:
                self.log.extend(str(error).encode())
        assert "nsh> " in out, (text, self.process.poll(), out[-1000:])
        return out

    def request(self, topic, data, error=None):
        (self.root / "request.json").write_text(json.dumps(data), encoding="utf-8")
        out = self.command(f"nycore product {topic} /host/request.json")
        if error is not None:
            assert f"nycore: command failed: {error}" in out, out
            return None
        match = re.search(r"PRODUCT_RESULT (\{[^\r\n]*\})", out)
        assert match, out[-1500:]
        return json.loads(match[1])

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.drain()
        self.log.extend(f"\nSIM_EXIT_CODE {self.process.returncode}\n".encode())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix="nyabula-product-") as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            empty = sim.request("memory.list", {})
            assert empty == {"revision": 0, "items": []}, empty
            first = sim.request("memory.create", {"revision": 0, "record": {"text": "Remember tea", "tag": "daily", "source": "forged"}})
            assert first["revision"] == 1 and first["items"][0]["source"] == "nsh"
            record_id = first["items"][0]["id"]
            sim.request("memory.create", {"revision": 0, "record": {"text": "stale"}}, error=-116)
            sim.request("memory.create", {"revision": 1, "record": {"text": "bad", "__proto__": {}}}, error=-22)
            assert sim.request("memory.list", {}) == first
            updated = sim.request("memory.update", {"revision": 1, "id": record_id, "record": {"text": "Remember coffee", "tag": "daily"}})
            assert updated["revision"] == 2 and updated["items"][0]["id"] == record_id
            assert updated["items"][0]["at"] == first["items"][0]["at"]
            task = sim.request("task.create", {"revision": 0, "record": {"title": "Feed cat", "state": "queued", "progress": 0}})
            assert task["items"][0]["title"] == "Feed cat"
            sim.request("task.create", {"revision": 1, "record": {"title": "Bad", "state": "invented"}}, error=-22)
            sim.request("calendar.create", {"revision": 0, "record": {"title": "Wrong interval", "start_at": 100, "end_at": 50}}, error=-22)
            event = sim.request("calendar.create", {"revision": 0, "record": {"title": "Meeting", "start_at": 1800000000000, "remind_before_ms": 60000}})
            assert event["revision"] == 1
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / "serial.log").write_bytes(logs)

        sim = Simulator(args.binary.resolve(), directory)
        try:
            restored = sim.request("memory.list", {})
            assert restored == updated, (restored, updated)
            assert sim.request("task.list", {}) == task
            assert sim.request("calendar.list", {}) == event
            deleted = sim.request("memory.delete", {"revision": 2, "id": record_id})
            assert deleted == {"revision": 3, "items": []}
            again = sim.request("memory.create", {"revision": 3, "record": {"text": "New memory"}})
            assert again["items"][0]["id"] != record_id
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / "serial.log").write_bytes(logs)

        with sqlite3.connect(Path(directory) / "nyabula-product.sqlite") as db:
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            db.execute("UPDATE state SET v=? WHERE k=?", (b'{"schema":99}', "product/v1/memory"))

        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request("memory.list", {}, error=-74)
            assert sim.request("task.list", {}) == task
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / "serial.log").write_bytes(logs)
    (args.output / "serial.log").write_bytes(logs)
    print("PRODUCT_RECORDS_SIM_PASS CRUD CAS validation provenance restart integrity corruption-isolation")


if __name__ == "__main__":
    main()
