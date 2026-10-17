#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise model backend CRUD, profile selection and persistence on NuttX."""
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
    calls = []
    logs = bytearray()

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append(request['model'])
            body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Model: ' + request['model']},
                                           'finish_reason': 'stop'}], 'usage': {'prompt_tokens': 12, 'completion_tokens': 4, 'total_tokens': 16}}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='nyabot-router-') as directory:
        def start():
            sim = Simulator(args.binary.resolve(), directory)
            sim.request('system.time.set', {'unix_ms': int(time.time() * 1000)})
            deadline = time.monotonic() + 15
            while not sim.request('agent.status', {})['ready']:
                assert time.monotonic() < deadline
            return sim

        def close(sim):
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)

        def chat(sim, identity, model):
            run = sim.request('agent.chat', {'requestId': identity, 'conversationId': identity, 'text': 'hello ' + identity})
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                current = sim.request('agent.run.get', {'id': run['id']})
                if current['state'] not in ('queued', 'running'):
                    assert current['state'] == 'succeeded' and current['reply'] == 'Model: ' + model, current
                    return
            raise AssertionError(current)

        sim = start()
        try:
            assert not sim.request('agent.status', {})['configured']
            assert sim.request('agent.config.router.get', {})['backends'] == []
            base = {'host': '127.0.0.1', 'path': '/v1/chat/completions', 'port': str(server.server_port),
                    'key': 'synthetic-router-key', 'priority': 20, 'enabled': True}
            low = {**base, 'index': 0, 'model': 'fixture-eco', 'cost_tier': 0}
            high = {**base, 'index': 3, 'model': 'fixture-premium', 'cost_tier': 3}
            sim.request('agent.config.router.save', low)
            state = sim.request('agent.config.router.save', high)
            assert [b['index'] for b in state['backends']] == [0, 3]
            assert 'synthetic-router-key' not in json.dumps(state)
            assert sim.request('agent.status', {})['configured']
            sim.request('agent.config.router.profile', {'profile': 'eco'})
            chat(sim, 'eco-request', 'fixture-eco')
            sim.request('agent.config.router.profile', {'profile': 'premium'})
            chat(sim, 'premium-request', 'fixture-premium')
            state = sim.request('agent.config.router.get', {})
            assert all(b['total_calls'] == 1 and b['total_prompt_tokens'] == 12 for b in state['backends']), state
            sim.request('agent.config.router.save', {**high, 'enabled': False, 'key': ''})
            state = sim.request('agent.config.router.get', {})
            assert state['backends'][1]['status'] == 'paused' and state['backends'][1]['keySet']
            chat(sim, 'paused-request', 'fixture-eco')
        finally:
            close(sim)
        sim = start()
        try:
            state = sim.request('agent.config.router.get', {})
            assert state['profile'] == 'premium' and not state['backends'][1]['enabled'], state
            chat(sim, 'restart-request', 'fixture-eco')
            state = sim.request('agent.config.router.delete', {'index': 3})
            assert [b['index'] for b in state['backends']] == [0]
            sim.request('agent.config.router.profile', {'profile': 'auto'})
            chat(sim, 'auto-request', 'fixture-eco')
            state = sim.request('agent.config.router.delete', {'index': 0})
            assert state['backends'] == []
            result = {'backendCrud': True, 'ecoPremiumSelection': True, 'priority20Works': True,
                      'pauseAndKeyRetention': True, 'restartPersistence': True,
                      'actualMetrics': True, 'modelCalls': calls}
            (args.output / 'result.json').write_text(json.dumps(result, indent=2))
            print('NYABOT_ROUTER_SIM_PASS', json.dumps(result))
        finally:
            close(sim)
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    main()
