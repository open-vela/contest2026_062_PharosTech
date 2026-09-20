#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise builtin workspace tools via the real model and approval path."""
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
    calls, outputs = [], {}
    actions = {
        'write': ('write_file', {'path': 'demo/notes.txt', 'content': 'Hello Nyabula'}, True),
        'edit': ('edit_file', {'path': 'demo/notes.txt', 'old_string': 'Hello', 'new_string': 'Happy'}, True),
        'read': ('read_file', {'path': 'demo/notes.txt'}, False),
        'list': ('list_dir', {'prefix': 'demo'}, False),
    }

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append(request)
            operation = next(item['content'] for item in reversed(request['messages']) if item['role'] == 'user')
            results = [item for item in request['messages'] if item['role'] == 'tool']
            if not results:
                name, arguments, write = actions[operation]
                tool = 'nyabula_action' if write else 'nyabula_tool'
                body = {'name': name, 'arguments': arguments}
                if write:
                    body = {'topic': 'agent.tools.call', 'arguments': body}
                message = {'role': 'assistant', 'content': None, 'tool_calls': [{
                    'id': 'builtin-' + operation, 'type': 'function',
                    'function': {'name': tool, 'arguments': json.dumps(body)}}]}
            else:
                outputs[operation] = json.loads(results[-1]['content'])
                message = {'role': 'assistant', 'content': operation + ' complete'}
            response = json.dumps({'choices': [{'message': message,
                'finish_reason': 'tool_calls' if not results else 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(response)))
            self.end_headers()
            self.wfile.write(response)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix='nyabot-builtin-') as directory:
        for iteration in range(2):
            sim = Simulator(args.binary.resolve(), directory)
            try:
                deadline = time.monotonic() + 20
                while not sim.request('agent.status', {})['ready']:
                    assert time.monotonic() < deadline
                catalog = sim.request('agent.tools.catalog', {})
                assert len(catalog['items']) == 9 and catalog['shellPolicy'] == 0, catalog
                if not iteration:
                    sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                        'port': str(server.server_port), 'model': 'fixture', 'key': 'synthetic-builtin-key'})
                    assert sim.request('agent.tools.providers.get', {}) == dict.fromkeys(['serp', 'exa', 'tavily', 'news'], False)
                    sim.request('agent.tools.providers.save', {'serp': 'synthetic-serp', 'news': 'synthetic-news'})
                    saved = sim.request('agent.tools.providers.save', {'serp': ''})
                    assert saved == {'serp': False, 'exa': False, 'tavily': False, 'news': True}, saved
                    sim.request('agent.tools.read', {'name': 'web_search', 'arguments': {'query': 'no provider configured'}}, error=-5)
                    for operation, (name, arguments, write) in actions.items():
                        run = sim.request('agent.chat', {'requestId': 'builtin-' + operation,
                            'conversationId': 'builtin-' + operation, 'text': operation})
                        approved = False
                        deadline = time.monotonic() + 20
                        while time.monotonic() < deadline:
                            current = sim.request('agent.run.get', {'id': run['id']})
                            if current['state'] == 'waiting_approval':
                                assert write, current
                                if not approved:
                                    if operation == 'write':
                                        assert not (Path(directory) / 'ai-agent/workspace/demo/notes.txt').exists()
                                    sim.request('agent.approval.decide', {'runId': run['id'], 'step': 0, 'decision': 'allow'})
                                    approved = True
                            elif current['state'] not in ('queued', 'running'):
                                assert current['state'] == 'succeeded', current
                                assert approved == write
                                break
                        else:
                            raise AssertionError(current)
                    assert outputs['read']['text'] == 'Happy Nyabula', outputs
                    assert 'notes.txt' in outputs['list']['text'], outputs
                    shell = sim.request('agent.tools.call', {'name': 'run_shell', 'arguments': {'command': 'uptime'}})
                    assert shell['ok'] and shell['text'], shell
                    sim.request('agent.tools.read', {'name': 'write_file', 'arguments': {'path': 'wrong.txt', 'content': 'no'}}, error=-13)
                else:
                    assert sim.request('agent.tools.providers.get', {})['news']
                    restored = sim.request('agent.tools.read', {'name': 'read_file', 'arguments': {'path': 'demo/notes.txt'}})
                    assert restored['text'] == 'Happy Nyabula', restored
                sim.request('agent.tools.read', {'name': 'read_file', 'arguments': {'path': '../config/config.json'}}, error=-22)
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
    server.shutdown()
    server.server_close()
    assert len(calls) == 8, len(calls)
    result = {'catalog': True, 'modelFileWriteEditReadList': True, 'writeApproval': True,
              'shellUptime': True, 'filePersistence': True, 'providerConfigPersistence': True,
              'missingSearchKeyError': True, 'modelCalls': len(calls), 'productionNetwork': False}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_BUILTIN_SIM_PASS', json.dumps(result))


if __name__ == '__main__':
    main()
