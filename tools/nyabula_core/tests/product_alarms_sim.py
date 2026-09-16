#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise persisted alarm occurrences without a browser."""
import argparse
from pathlib import Path
import tempfile
import time
from product_records_sim import Simulator


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    # Sunday 2026-09-13 00:00 UTC.
    sunday = 1789257600000
    with tempfile.TemporaryDirectory(prefix='nyabula-alarms-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request('system.time.set', {'unix_ms': sunday})
            state = sim.request('alarm.list', {})
            record = dict(time='00:01', label='Wake', repeat=[], enabled=True,
                          utc_offset_minutes=0)
            state = sim.request('alarm.create', {'revision': state['revision'], 'record': record})
            aid = state['items'][0]['id']
            assert state['items'][0]['next_at'] == sunday + 60000, state
            sim.request('alarm.create', {'revision': 0, 'record': record}, error=-116)
            sim.request('alarm.create', {'revision': state['revision'], 'record': dict(record, time='24:00')}, error=-22)
            sim.request('alarm.create', {'revision': state['revision'], 'record': dict(record, repeat=['mon', 'mon'])}, error=-22)
            state = sim.request('alarm.create', {'revision': state['revision'], 'record': dict(record, repeat=['mon'])})
            assert state['items'][1]['next_at'] == sunday + 86460000, state
            sim.request('product.start', {})
            sim.request('system.time.set', {'unix_ms': sunday + 61000})
            time.sleep(.3)
            state = sim.request('alarm.list', {})
            row = state['items'][0]
            assert row['status'] == 'ringing' and not row['enabled']
            revision = state['revision']
            time.sleep(.2)
            assert sim.request('alarm.list', {})['revision'] == revision
            sim.request('product.stop', {})
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            state = sim.request('alarm.list', {})
            assert state['items'][0]['status'] == 'ringing'
            sim.request('system.time.set', {'unix_ms': sunday + 62000})
            state = sim.request('alarm.snooze', {'revision': state['revision'], 'id': aid})
            due = state['items'][0]['next_at']
            assert state['items'][0]['status'] == 'snoozed'
            sim.request('product.start', {})
            sim.request('system.time.set', {'unix_ms': due + 1000})
            time.sleep(.3)
            state = sim.request('alarm.list', {})
            assert state['items'][0]['status'] == 'ringing'
            state = sim.request('alarm.dismiss', {'revision': state['revision'], 'id': aid})
            assert state['items'][0]['status'] == 'dismissed'
            sim.request('system.time.set', {'unix_ms': sunday + 86460000 + 600000})
            time.sleep(.3)
            state = sim.request('alarm.list', {})
            assert state['items'][1]['status'] == 'missed'
            assert state['items'][1]['next_at'] == sunday + 8 * 86400000 + 60000
            sim.request('product.stop', {})
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
    print('PRODUCT_ALARMS_SIM_PASS CRUD CAS validation repeat autonomous restart snooze missed')


if __name__ == '__main__':
    main()
