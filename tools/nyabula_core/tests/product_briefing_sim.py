#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Briefing generation uses only durable sources and playback survives Web exit."""
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
    now = 1789257600000
    with tempfile.TemporaryDirectory(prefix='nyabula-briefing-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request('system.time.set', {'unix_ms': now})
            sim.request('task.create', {'revision':0, 'record':{'title':'Feed cat', 'state':'queued'}})
            sim.request('task.create', {'revision':1, 'record':{'title':'Done item', 'state':'done'}})
            sim.request('calendar.create', {'revision':0, 'record':{'title':'Standup', 'start_at':now + 3600000}})
            sim.request('calendar.create', {'revision':1, 'record':{'title':'Later', 'start_at':now + 172800000}})
            sim.request('memory.create', {'revision':0, 'record':{'text':'User-confirmed tea preference'}})
            empty = sim.request('briefing.get', {})
            assert empty['items'] == [] and empty['revision'] == 0
            state = sim.request('briefing.generate', {'revision':0})
            text = ' '.join(item['text'] for item in state['items'])
            sources = {item['source'] for item in state['items']}
            assert 'Feed cat' in text and 'Done item' not in text
            assert 'Standup' in text and 'Later' not in text
            assert 'User-confirmed tea preference' in text
            assert {'calendar', 'tasks', 'memory'} <= sources
            sim.request('briefing.generate', {'revision':0}, error=-116)
            state = sim.request('briefing.start', {'revision':state['revision']})
            assert state['playing'] and state['index'] == 0
            sim.request('product.start', {})
            time.sleep(1.2)
            projected = sim.request('briefing.get', {})
            # The display service is not attached in this headless profile;
            # playback remains pending instead of falsely marking a card shown.
            assert projected['playing'] and projected['index'] == 0
            sim.request('product.stop', {})
            stopped = sim.request('briefing.stop', {'revision':projected['revision']})
            scheduled = sim.request('briefing.configure', {'revision':stopped['revision'],
                'schedule_enabled':True, 'morning_minute':480,
                'evening_minute':1200, 'utc_offset_minutes':0})
            sim.request('system.time.set', {'unix_ms':now + 86400000 + 480 * 60000})
            sim.request('product.start', {})
            time.sleep(1.2)
            projected = sim.request('briefing.get', {})
            assert projected['playing'] and projected['generated_at'] > state['generated_at']
            assert projected['last_slot'].endswith('-0')
            sim.request('product.stop', {})
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            restored = sim.request('briefing.get', {})
            assert restored['items'] == projected['items'] and restored['playing']
            stopped = sim.request('briefing.stop', {'revision':restored['revision']})
            assert not stopped['playing']
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
    print('PRODUCT_BRIEFING_SIM_PASS durable-sources filters CAS schedule playback restart')


if __name__ == '__main__':
    main()
