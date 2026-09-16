#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prove model tool -> Core -> real LVGL Eye service, with no fake success."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
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
    calls, observed = [], []

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append(request)
            results = [item for item in request['messages'] if item['role'] == 'tool']
            if not results:
                names = [tool['function']['name'] for tool in request['tools']]
                assert 'nyabula_expression' in names, names
                message = {'role': 'assistant', 'content': None, 'tool_calls': [{
                    'id': 'eye-happy', 'type': 'function', 'function': {
                        'name': 'nyabula_expression', 'arguments': '{"expression":"happy"}'}}]}
            else:
                observed.append(json.loads(results[-1]['content']))
                message = {'role': 'assistant', 'content': 'The real eyes are happy.'}
            body = json.dumps({'choices': [{'message': message,
                'finish_reason': 'tool_calls' if not results else 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='nyabot-eyes-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request('eyes.status', {}, error=-19)
            sim.request('eyes.expression', {'expression': 'happy'}, error=-19)
            sim.command('eyeprobe init')
            output = sim.command('nyabula_eye &') + sim.drain(2)
            assert 'Eye Engine attached' in output, output
            initial = sim.request('eyes.status', {})
            assert initial['expression'] == 'idle', initial
            deadline = time.monotonic() + 20
            while not sim.request('agent.status', {})['ready']:
                assert time.monotonic() < deadline
            sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                'port': str(server.server_port), 'model': 'fixture', 'key': 'synthetic-eye-key'})
            run = sim.request('agent.chat', {'requestId': 'happy-eyes',
                'conversationId': 'eyes', 'text': 'Show me happy eyes'})
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                finished = sim.request('agent.run.get', {'id': run['id']})
                assert finished['state'] != 'waiting_approval', finished
                if finished['state'] not in ('running', 'queued'):
                    assert finished['state'] == 'succeeded', finished
                    break
            else:
                raise AssertionError(finished)
            assert finished['steps'][0]['sideEffectApplied'], finished
            state = sim.request('eyes.status', {})
            assert state['expression'] == 'happy' and state['last_status'] == 0, state
            assert observed[0]['expression'] == 'happy' and observed[0]['last_status'] == 0, observed
            assert state['expression_owner']['source'] == 'nyabot', state
            sim.request('eyes.expression', {'expression': 'not-an-expression'}, error=-22)
            frame = sim.command('eyeprobe save /host')
            assert frame.count('EYE_FRAME') == 2, frame
            for path in Path(directory).glob('*.ppm'):
                shutil.copy2(path, args.output / path.name)
            (args.output / 'snapshot.json').write_text(json.dumps(state, indent=2))
        finally:
            sim.close()
            (args.output / 'serial.log').write_bytes(sim.log)
            server.shutdown()
            server.server_close()
    assert len(calls) == 2
    result = {'realAgentTool': True, 'realEyeService': True, 'displayConfirmed': True,
              'noRedundantApproval': True, 'unavailableError': True, 'invalidExpression': True}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_EYE_SIM_PASS', json.dumps(result))


if __name__ == '__main__':
    main()
