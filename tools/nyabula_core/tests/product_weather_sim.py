#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""QWeather fixtures: gzip, persistent dedupe, failure retention and config."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
from product_records_sim import Simulator


class Fixture(BaseHTTPRequestHandler):
    mode = 'alert'
    paths = []

    def log_message(self, *_):
        pass

    def do_GET(self):
        Fixture.paths.append(self.path)
        assert self.headers.get('X-QW-Api-Key') == 'fixture-only-key'
        assert self.headers.get('Accept-Encoding') == 'identity'
        assert 'key=' not in self.path
        data = {'metadata': {'attributions': ['Fixture; not a real warning']}}
        code = 200
        if self.path.startswith('/geo/'):
            data = {'code': '200', 'location': [{'name': 'Dalian fixture', 'lat': '38.91', 'lon': '121.62', 'tz': 'Asia/Shanghai'}]}
        elif self.path.startswith('/weather/v1/current/'):
            data.update(temperature={'value': 20, 'unit': '°C'}, condition={'code': '101', 'text': 'Cloudy fixture'})
        elif self.path.startswith('/weather/v1/daily/'):
            data['days'] = []
        elif self.path.startswith('/weather/v1/hourly/'):
            data['hours'] = []
        elif self.path.startswith('/weatheralert/'):
            if Fixture.mode == 'failure':
                code = 503
            elif Fixture.mode == 'empty':
                data['metadata']['zeroResult'] = True
                data['alerts'] = []
            else:
                data['alerts'] = [{'id': 'test-' + Fixture.mode, 'headline': 'Fixture ' + Fixture.mode,
                    'description': 'Test data only', 'messageType': {'code': Fixture.mode,
                    'supersedes': [] if Fixture.mode == 'alert' else ['test-alert']},
                    'expireTime': '2026-09-14T08:00+08:00'}]
        else:
            code = 404
        payload = json.dumps(data).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Encoding', 'identity')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def wait_done(sim):
    for _ in range(80):
        time.sleep(.15)
        state = sim.request('weather.status', {})
        now = sim.request('weather.get', {'section': 'now'})
        if not state['refreshing'] and now.get('fetched_at'):
            return state
    raise AssertionError(state)


def refresh(sim):
    before = sim.request('weather.get', {'section': 'now'}).get('fetched_at', 0)
    sim.request('weather.refresh', {})
    for _ in range(80):
        state = wait_done(sim)
        if sim.request('weather.get', {'section': 'now'}).get('fetched_at', 0) > before:
            return state
    raise AssertionError('refresh did not advance')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    server = ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='nyabula-weather-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request('system.time.set', {'unix_ms': 1789257600000})
            config = dict(revision=0, host='test.re.qweatherapi.com', key='fixture-only-key',
                          city='大连', province='辽宁', enabled=True, fixture_port=str(server.server_port))
            state = sim.request('weather.configure', config)
            assert state['key_set'] and 'key' not in state and 'fixture_port' not in state
            sim.request('weather.configure', dict(config, revision=state['revision'], host='qweatherapi.com.attacker.test'), error=-22)
            sim.request('product.start', {})
            state = wait_done(sim)
            assert state['last_error'] == 0, state
            assert sim.request('weather.get', {'section': 'now'})['temperature']['value'] == 20
            first = sim.request('notification.list', {})
            assert len(first['items']) == 1, first
            refresh(sim)
            assert len(sim.request('notification.list', {})['items']) == 1
            Fixture.mode = 'update'
            refresh(sim)
            assert len(sim.request('notification.list', {})['items']) == 2
            saved = sim.request('weather.get', {'section': 'alerts'})
            Fixture.mode = 'failure'
            state = refresh(sim)
            assert state['errors']['alerts'] == -503, state
            stale = sim.request('weather.get', {'section': 'alerts'})
            assert stale['alerts'] == saved['alerts'] and stale['fetched_at'] == saved['fetched_at']
            Fixture.mode = 'cancel'
            refresh(sim)
            assert len(sim.request('notification.list', {})['items']) == 3
            sim.request('product.stop', {})
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request('system.time.set', {'unix_ms': 1789257610000})
            sim.request('product.start', {})
            state = wait_done(sim)
            assert state['key_set'] and 'key' not in state
            assert len(sim.request('notification.list', {})['items']) == 3
            Fixture.mode = 'empty'
            refresh(sim)
            assert sim.request('weather.get', {'section': 'alerts'})['alerts'] == []
            assert len(sim.request('notification.list', {})['items']) == 3
            sim.request('product.stop', {})
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
    server.shutdown()
    print('PRODUCT_WEATHER_SIM_PASS identity host-guard key-redaction autonomous dedupe update cancel failure-retention restart')


if __name__ == '__main__':
    main()
