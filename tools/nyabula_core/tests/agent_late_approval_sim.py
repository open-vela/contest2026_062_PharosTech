#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Approval after step 16 remains usable within the expanded 32-step run."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
from product_records_sim import Simulator


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        results = [m for m in request['messages'] if m['role'] == 'tool']
        count = len(results)
        if count < 16:
            name, arguments = 'nyabula_skill_read', {'id': 'late-approval', 'offset': count}
        elif count == 16:
            name, arguments = 'nyabula_read', {'topic': 'memory.list'}
        elif count == 17:
            name, arguments = 'nyabula_action', {'topic': 'memory.create', 'arguments': {
                'revision': json.loads(results[-1]['content'])['revision'],
                'record': {'text': 'Late approval fixture'}}}
        else:
            result = json.loads(results[-1]['content'])
            assert result['items'][-1]['text'] == 'Late approval fixture', result
        message = {'role': 'assistant', 'content': 'Late approval applied'}
        if count < 18:
            message = {'role': 'assistant', 'content': '', 'tool_calls': [{
                'id': 'step-' + str(count), 'type': 'function', 'function': {
                    'name': name, 'arguments': json.dumps(arguments)}}]}
        body = json.dumps({'choices': [{'message': message,
            'finish_reason': 'tool_calls' if count < 18 else 'stop'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='ny-late-approval-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            for _ in range(50):
                if sim.request('agent.status', {})['ready']:
                    break
                time.sleep(.05)
            catalog = sim.request('agent.skills.save', {'id': 'late-approval',
                'title': 'Approval fixture', 'content': 'Use exact owner approval. ' * 4, 'revision': 0})
            sim.request('agent.skills.enable', {'id': 'late-approval', 'enabled': True,
                'revision': catalog['revision']})
            sim.request('agent.config.set', {'host': '127.0.0.1', 'port': str(server.server_port),
                'path': '/v1/chat/completions', 'model': 'controlled', 'key': 'synthetic-fixture-only'})
            submitted = sim.request('agent.chat', {'conversationId': 'late-approval',
                'requestId': 'late-approval', 'text': 'Controlled late approval fixture.'})
            def wait(states):
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    run = sim.request('agent.run.get', {'id': submitted['id']})
                    if run['state'] in states:
                        return run
                    assert run['state'] not in ('failed', 'cancelled'), run
                    time.sleep(.05)
                raise AssertionError(run)
            pending = wait(('waiting_approval',))
            assert len(pending['steps']) == 18 and pending['steps'][17]['state'] == 'pending', pending
            assert not sim.request('memory.list', {})['items']
            sim.request('agent.approval.decide', {'runId': submitted['id'], 'step': 32, 'decision': 'allow'}, error=-22)
            sim.request('agent.approval.decide', {'runId': submitted['id'], 'step': 17, 'decision': 'allow'})
            completed = wait(('succeeded',))
            assert sim.request('memory.list', {})['items'][0]['text'] == 'Late approval fixture'
            (args.output / 'result.json').write_text(json.dumps(completed, indent=2))
        finally:
            sim.close()
            (args.output / 'serial.log').write_bytes(sim.log)
            server.shutdown()
    print('NATIVE_AGENT_STEP17_APPROVAL_AND_STEP32_REJECTION_PASS')


if __name__ == '__main__':
    main()
