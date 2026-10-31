#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real NuttX ingress chat isolation with a controlled, deliberately hostile model."""
import argparse
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import json
from pathlib import Path
import socket
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
    calls, errors = [], []
    entered, release = threading.Event(), threading.Event()

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append(request)
            messages = request['messages']
            text = next(m['content'] for m in reversed(messages) if m['role'] == 'user')
            try:
                if not text.startswith('owner'):
                    encoded = json.dumps(request)
                    assert 'owner-secret' not in encoded and 'private-skill' not in messages[0]['content'], encoded
                    assert 'nyabula_action' not in json.dumps(request.get('tools', []))
                    assert 'memory.list' not in json.dumps(request.get('tools', []))
                    if text.startswith('client-b'):
                        assert 'client-a' not in encoded, encoded
                results = [m for m in messages if m['role'] == 'tool']
                if text.startswith('attack-') and not results:
                    name, arguments = {
                        'attack-write': ('nyabula_action', {'topic': 'memory.create', 'arguments': {'revision': 0, 'record': {'text': 'forged'}}}),
                        'attack-memory': ('nyabula_read', {'topic': 'memory.list'}),
                        'attack-skill': ('nyabula_skill_read', {'id': 'private-skill'}),
                        'attack-catalog': ('nyabula_mcp_catalog', {}),
                        'attack-time': ('nyabula_read', {'topic': 'system.time.get'}),
                    }[text]
                    message = {'role': 'assistant', 'content': None, 'tool_calls': [
                        {'id': 'attack', 'type': 'function', 'function': {'name': name, 'arguments': json.dumps(arguments)}}]}
                else:
                    if results:
                        result = json.loads(results[-1]['content'])
                        assert result.get('clock_valid') if text == 'attack-time' else result.get('error', 0) < 0, result
                    if text == 'hold':
                        entered.set()
                        assert release.wait(10), 'Release deadline'
                    message = {'role': 'assistant', 'content': 'reply:' + text}
            except Exception as error:
                errors.append(repr(error))
                message = {'role': 'assistant', 'content': 'fixture assertion failed'}
            body = json.dumps({'choices': [{'message': message, 'finish_reason': 'tool_calls' if 'tool_calls' in message else 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    tokens = {'a': '0123456789abcdef' * 4, 'b': 'fedcba9876543210' * 4}
    with tempfile.TemporaryDirectory(prefix='nyabot-mcp-chat-') as directory:
        sim = Simulator(args.binary.resolve(), directory)

        def call(name, arguments, client='a', denied=False):
            body = json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                               'params': {'name': name, 'arguments': arguments}})
            with closing(http.client.HTTPConnection('127.0.0.1', port, timeout=8)) as conn:
                conn.request('POST', '/mcp', body, {'Authorization': 'Bearer ' + tokens[client],
                    'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream',
                    'MCP-Protocol-Version': '2025-11-25'})
                response = conn.getresponse()
                content = response.read()
                assert response.status == 200, (response.status, content)
            result = json.loads(content)['result']
            assert result['isError'] == denied, result
            return None if denied else json.loads(result['content'][0]['text'])

        def finish(run):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                current = sim.request('agent.run.get', {'id': run['id']})
                if current['state'] not in ('queued', 'running', 'cancelling', 'waiting_approval'):
                    return current
            raise AssertionError(current)

        def save(client, scopes):
            state = sim.request('agent.mcp.in.list', {})
            return sim.request('agent.mcp.in.save', {'revision': state['revision'], 'id': client,
                'title': client, 'expiresAt': int(time.time() * 1000) + 600000,
                'token': tokens[client], 'scopes': scopes})

        try:
            sim.request('system.time.set', {'unix_ms': int(time.time() * 1000)})
            deadline = time.monotonic() + 15
            while not sim.request('agent.status', {})['ready']:
                assert time.monotonic() < deadline
            sim.request('agent.config.set', {'host': '127.0.0.1', 'port': str(server.server_port),
                'path': '/v1/chat/completions', 'model': 'controlled', 'key': 'synthetic-fixture-only'})
            sim.request('agent.skills.save', {'revision': 0, 'id': 'private-skill', 'title': 'private-skill', 'content': 'owner-secret'})
            sim.request('agent.skills.enable', {'revision': 1, 'id': 'private-skill', 'enabled': True})
            owner = finish(sim.request('agent.chat', {'requestId': 'same', 'conversationId': 'same', 'text': 'owner-secret'}))
            assert owner['state'] == 'succeeded', owner
            scopes = ['nyabula_chat', 'nyabula_run', 'nyabula_cancel', 'nyabula_time']
            save('a', scopes)
            save('b', scopes)
            state = sim.request('agent.mcp.in.list', {})
            sim.request('agent.mcp.in.enable', {'revision': state['revision'], 'enabled': True})
            sim.command(f'nyabula_mcp {port} &')
            request = {'requestId': 'same', 'conversationId': 'same', 'text': 'client-a'}
            first = finish(call('nyabula_chat', request))
            assert first['state'] == 'succeeded', first
            count = len(calls)
            assert call('nyabula_chat', request)['id'] == first['id']
            assert len(calls) == count
            second = finish(call('nyabula_chat', {**request, 'text': 'client-b'}, client='b'))
            assert second['state'] == 'succeeded' and first['remotePrincipal'] != second['remotePrincipal']
            assert call('nyabula_run', {'id': first['id']}) == first
            for name in ('nyabula_run', 'nyabula_cancel'):
                call(name, {'id': first['id']}, client='b', denied=True)
                call(name, {'id': owner['id']}, denied=True)
            for index, text in enumerate(('attack-write', 'attack-memory', 'attack-skill', 'attack-catalog', 'attack-time')):
                run = finish(call('nyabula_chat', {**request, 'requestId': 'attack-' + str(index), 'text': text}))
                assert run['state'] == ('succeeded' if text == 'attack-time' else 'failed'), run
            assert sim.request('memory.list', {})['items'] == []
            held = call('nyabula_chat', {**request, 'requestId': 'hold', 'text': 'hold'})
            assert entered.wait(5)
            save('a', ['nyabula_run', 'nyabula_cancel'])
            release.set()
            cancelled = finish(held)
            assert cancelled['state'] == 'cancelled', cancelled
            call('nyabula_chat', {**request, 'requestId': 'denied'}, denied=True)
            assert not errors, errors
            result = {'principalIsolation': True, 'historyAndContextIsolation': True,
                      'ownerRunDenied': True, 'crossClientRunCancelDenied': True,
                      'idempotentChat': True, 'hostileToolsDenied': True,
                      'grantedTimeRead': True, 'revocationCancels': True,
                      'modelRequests': len(calls)}
            (args.output / 'result.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(result))
        finally:
            (args.output / 'model-requests.json').write_text(json.dumps(calls, indent=2))
            (args.output / 'fixture-errors.json').write_text(json.dumps(errors, indent=2))
            release.set()
            sim.close()
            (args.output / 'serial.log').write_bytes(sim.log)
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    main()
