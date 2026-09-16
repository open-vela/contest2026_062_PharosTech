#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Core timers continue without a browser and recover using wall-clock anchors."""
import argparse
from pathlib import Path
import tempfile
import time
from product_records_sim import Simulator


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix="nyabula-timers-") as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            state = sim.request("system.time.set", {"unix_ms": int(time.time() * 1000)})
            assert state["clock_valid"]
            assert sim.request("product.start", {})["running"]
            initial = sim.request("timer.list", {})
            timer = sim.request("timer.create", {"revision": initial["revision"], "kind": "countdown", "label": "Tea", "duration_ms": 700})
            timer_id = timer["items"][0]["id"]
            time.sleep(1.2)
            finished = sim.request("timer.list", {})
            assert finished["revision"] > timer["revision"], finished
            assert finished["items"][0]["status"] == "finished"
            timer = sim.request("timer.create", {"revision": finished["revision"], "kind": "countdown", "label": "Pause", "duration_ms": 5000})
            pause_id = timer["items"][-1]["id"]
            paused = sim.request("timer.pause", {"revision": timer["revision"], "id": pause_id})
            held = paused["items"][-1]["remaining_ms"]
            time.sleep(.3)
            assert sim.request("timer.list", {})["items"][-1]["remaining_ms"] == held
            resumed = sim.request("timer.resume", {"revision": paused["revision"], "id": pause_id})
            assert resumed["items"][-1]["status"] == "running"
            watch = sim.request("timer.create", {"revision": resumed["revision"], "kind": "stopwatch", "label": "Lap"})
            watch_id = watch["items"][-1]["id"]
            time.sleep(.2)
            lap = sim.request("timer.lap", {"revision": watch["revision"], "id": watch_id})
            assert lap["items"][-1]["laps"][0] > 100
            paused_watch = sim.request("timer.pause", {"revision": lap["revision"], "id": watch_id})
            watch_elapsed = paused_watch["items"][-1]["elapsed_ms"]
            timer = sim.request("timer.create", {"revision": paused_watch["revision"], "kind": "countdown", "label": "Restart", "duration_ms": 30000})
            restart_id = timer["items"][-1]["id"]
            remaining = timer["items"][-1]["remaining_ms"]
            assert not sim.request("product.stop", {})["running"]
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / "serial.log").write_bytes(logs)

        time.sleep(.6)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request("system.time.set", {"unix_ms": int(time.time() * 1000)})
            sim.request("product.start", {})
            restored = sim.request("timer.list", {})
            rows = {row["id"]: row for row in restored["items"]}
            assert rows[timer_id]["status"] == "finished"
            assert rows[watch_id]["elapsed_ms"] == watch_elapsed
            assert 0 < rows[restart_id]["remaining_ms"] < remaining - 400
            assert rows[restart_id]["status"] == "running"
            sim.request("product.stop", {})
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / "serial.log").write_bytes(logs)

        sim = Simulator(args.binary.resolve(), directory)
        try:
            unclocked = sim.request("timer.list", {})
            row = next(row for row in unclocked["items"] if row["id"] == restart_id)
            assert row["status"] == "waiting-clock" and row["recovery"] == "clock-unavailable"
            sim.request("product.start", {})
            sim.request("system.time.set", {"unix_ms": int(time.time() * 1000)})
            time.sleep(.2)
            recovered = sim.request("timer.list", {})
            row = next(row for row in recovered["items"] if row["id"] == restart_id)
            assert row["status"] == "running" and row["remaining_ms"] > 0
            sim.request("product.stop", {})
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / "serial.log").write_bytes(logs)
    print("PRODUCT_TIMERS_SIM_PASS autonomous-expiry pause resume stopwatch-lap restart clock-unavailable")


if __name__ == "__main__":
    main()
