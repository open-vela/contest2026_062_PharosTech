#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify owner personality reaches the real model request and survives reboot."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
from product_records_sim import Simulator


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    calls, errors, logs = [], [], bytearray()
    profile = {'catName': 'Test Cat', 'ownerName': 'Test Owner', 'language': 'en',
               'tone': 'concise', 'instructions': 'Start with the fixture conclusion.'}

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append(request)
            prompt = request['messages'][0]['content']
            for key, value in profile.items():
                if json.dumps(key) + ':' + json.dumps(value) not in prompt:
                    errors.append(key)
            body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Profile received'}, 'finish_reason': 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='nyabot-profile-') as directory:
        for iteration in range(2):
            sim = Simulator(args.binary.resolve(), directory)
            try:
                sim.request('system.time.set', {'unix_ms': int(time.time() * 1000)})
                deadline = time.monotonic() + 15
                while not sim.request('agent.status', {})['ready']:
                    assert time.monotonic() < deadline
                if not iteration:
                    initial = sim.request('agent.profile.get', {})
                    assert initial['revision'] == 0 and initial['catName'] == 'Nyabula'
                    sim.request('agent.profile.set', {**profile, 'revision': initial['revision']})
                    sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                        'port': str(server.server_port), 'model': 'fixture', 'key': 'synthetic-profile-key'})
                saved = sim.request('agent.profile.get', {})
                assert all(saved[key] == value for key, value in profile.items()), saved
                run = sim.request('agent.chat', {'requestId': 'profile-' + str(iteration),
                    'conversationId': 'profile', 'text': 'hello'})
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    current = sim.request('agent.run.get', {'id': run['id']})
                    if current['state'] not in ('queued', 'running'):
                        assert current['state'] == 'succeeded', current
                        break
                else:
                    raise AssertionError(current)
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
        server.shutdown()
        server.server_close()
        assert len(calls) == 2 and not errors, errors
        result = {'profileCrud': True, 'realModelContext': True, 'restartPersistence': True, 'modelCalls': len(calls)}
        (args.output / 'result.json').write_text(json.dumps(result, indent=2))
        print('NYABOT_PROFILE_SIM_PASS', json.dumps(result))


if __name__ == '__main__':
    main()
