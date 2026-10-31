#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise durable Core scheduling and the actual official agent loop."""
import argparse
import json
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from product_records_sim import Simulator

parser = argparse.ArgumentParser()
parser.add_argument('binary', type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
calls = []

class Model(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = next(m['content'] for m in reversed(request['messages']) if m['role'] == 'user')
        calls.append(text)
        if text == 'slow automation fixture':
            time.sleep(3)
        tools = [m for m in request['messages'] if m['role'] == 'tool']
        if text == 'approved automation' and not tools:
            message = {'role': 'assistant', 'content': None, 'tool_calls': [{'id': 'automatic-write', 'type': 'function',
                'function': {'name': 'nyabula_action', 'arguments': json.dumps({'topic': 'memory.create',
                    'arguments': {'revision': 0, 'record': {'text': 'Approved scheduled memory'}}})}}]}
        else:
            message = {'role': 'assistant', 'content': 'HEARTBEAT_OK' if text.startswith('Proactive check.') else 'Completed: ' + text}
        body = json.dumps({'choices': [{'message': message, 'finish_reason': 'tool_calls' if message.get('tool_calls') else 'stop'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
threading.Thread(target=server.serve_forever, daemon=True).start()
logs = bytearray()
try:
    with tempfile.TemporaryDirectory(prefix='nyabot-automation-') as directory:
        def start():
            instance = Simulator(args.binary.resolve(), directory)
            instance.request('system.time.set', {'unix_ms': int(time.time()*1000)})
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                state = instance.request('agent.status', {})
                if state['ready']:
                    instance.request('product.start', {})
                    time.sleep(1.2)
                    return instance
            raise AssertionError('Agent startup timeout')

        sim = start()
        def rules():
            return sim.request('agent.automation.list', {})
        def change(topic, data):
            return sim.request('agent.automation.' + topic, {**data, 'revision': rules()['revision']})
        def rule(identity):
            return next(r for r in rules()['items'] if r['id'] == identity)
        def wait_for(fn, description):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                value = fn()
                if value:
                    return value
                time.sleep(.1)
            raise AssertionError(description)
        def run_state(identity, expected):
            r = rule(identity)
            if not r.get('lastRun'):
                return None
            run = sim.request('agent.run.get', {'id': r['lastRun']})
            return run if run['state'] == expected else None
        try:
            assert rules() == {'items': [], 'revision': 0}
            base = {'id': 'one', 'title': 'One shot', 'prompt': 'one shot fixture', 'kind': 'at',
                    'atMs': int(time.time()*1000)+60000, 'intervalSeconds': 0}
            saved = change('save', base)
            assert not saved['items'][0]['enabled']
            sim.request('agent.automation.save', {**base, 'revision': 0}, error=-116)
            sim.request('agent.automation.save', {**base, 'id': '../bad', 'revision': saved['revision']}, error=-22)
            change('run', {'id': 'one'})
            wait_for(lambda: rule('one').get('error') == -61, 'Unconfigured pending was not retained')
            assert calls == []
            original_request = rule('one')['requestId']
            sim.close(); logs.extend(sim.log)
            sim = start()
            assert rule('one')['pending'] and rule('one')['requestId'] == original_request
            sim.request('agent.config.set', {'host': '127.0.0.1', 'port': str(server.server_port),
                'path': '/v1/chat/completions', 'model': 'controlled', 'key': 'synthetic-fixture-only'})
            completed = wait_for(lambda: run_state('one', 'succeeded'), 'Pending run not dispatched')
            assert completed['reply'] == 'Completed: one shot fixture'
            time.sleep(2)
            assert calls == ['one shot fixture']
            periodic = {**base, 'id': 'periodic', 'kind': 'every', 'intervalSeconds': 30, 'prompt': 'periodic fixture'}
            change('save', periodic)
            change('enable', {'id': 'periodic', 'enabled': True})
            due = rule('periodic')['nextAt']
            sim.request('system.time.set', {'unix_ms': due+10})
            wait_for(lambda: run_state('periodic', 'succeeded'), 'Recurring deadline did not fire')
            change('enable', {'id': 'periodic', 'enabled': False})
            assert calls.count('periodic fixture') == 1
            at = sim.request('system.time.get', {})['unix_ms'] + 3000
            change('save', {**base, 'id': 'timed', 'atMs': at, 'prompt': 'timed fixture'})
            change('enable', {'id': 'timed', 'enabled': True})
            sim.request('system.time.set', {'unix_ms': at + 10})
            wait_for(lambda: run_state('timed', 'succeeded'), 'One-shot deadline did not fire')
            assert not rule('timed')['enabled']
            change('save', {**periodic, 'id': 'heartbeat', 'kind': 'heartbeat', 'prompt': 'Check for changes'})
            change('run', {'id': 'heartbeat'})
            heartbeat = wait_for(lambda: run_state('heartbeat', 'succeeded'), 'Heartbeat did not run')
            assert heartbeat['reply'] == 'HEARTBEAT_OK'
            change('save', {**periodic, 'id': 'approval', 'prompt': 'approved automation'})
            change('run', {'id': 'approval'})
            waiting = wait_for(lambda: run_state('approval', 'waiting_approval'), 'Scheduled write bypassed approval')
            assert sim.request('memory.list', {})['items'] == []
            sim.request('agent.approval.decide', {'runId': waiting['id'], 'step': 0, 'decision': 'allow'})
            wait_for(lambda: run_state('approval', 'succeeded'), 'Approved scheduled write failed')
            assert sim.request('memory.list', {})['items'][0]['text'] == 'Approved scheduled memory'
            blocker = sim.request('agent.chat', {'requestId': 'slow-one', 'conversationId': 'slow', 'text': 'slow automation fixture'})
            change('run', {'id': 'periodic'})
            change('enable', {'id': 'periodic', 'enabled': False})
            wait_for(lambda: sim.request('agent.run.get', {'id': blocker['id']})['state'] == 'succeeded', 'Blocker did not finish')
            time.sleep(1.2)
            assert calls.count('periodic fixture') == 1, calls
            real_now = int(time.time()*1000)
            sim.request('system.time.set', {'unix_ms': real_now})
            change('save', {**base, 'id': 'missed', 'atMs': real_now + 5000, 'prompt': 'must not run after offline'})
            change('enable', {'id': 'missed', 'enabled': True})
            snapshot = rules()
        finally:
            sim.close(); logs.extend(sim.log)
        time.sleep(5.2)
        sim = start()
        try:
            restored = rules()
            assert [r for r in restored['items'] if r['id'] != 'missed'] == [r for r in snapshot['items'] if r['id'] != 'missed']
            assert rule('missed')['state'] == 'missed' and not rule('missed')['enabled'], restored
            assert sim.request('memory.list', {})['items'][0]['text'] == 'Approved scheduled memory'
            count = len(calls)
            time.sleep(2)
            assert len(calls) == count
            change('delete', {'id': 'approval'})
            assert sim.request('agent.run.get', {'id': waiting['id']})['state'] == 'succeeded'
            assert sim.request('memory.list', {})['items'][0]['text'] == 'Approved scheduled memory'
        finally:
            sim.close(); logs.extend(sim.log)
    result = {'unconfigured_pending': True, 'one_shot_no_repeat': True, 'recurring': True,
              'heartbeat': True, 'approval_required': True, 'pause_pending': True,
              'restart_no_replay': True, 'offline_missed_skipped': True, 'delete_preserves_runs': True, 'model_requests': len(calls)}
    (args.output/'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_AUTOMATION_PASS ' + json.dumps(result))
finally:
    (args.output/'serial.log').write_bytes(logs)
    server.shutdown(); server.server_close()
