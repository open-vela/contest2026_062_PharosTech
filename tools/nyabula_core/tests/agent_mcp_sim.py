#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real NuttX Core, actual HTTP MCP peer, synthetic model, no paid services."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
from product_records_sim import Simulator


class McpFixture:
    def __init__(self):
        self.effects = []
        self.changed = False
        self.failure = False
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.requests.append(request['method'])
                assert self.headers.get('Authorization') == 'Bearer mcp-fixture-secret'
                assert self.headers.get('MCP-Protocol-Version') == '2025-11-25'
                method = request['method']
                if method != 'initialize':
                    assert self.headers.get('Mcp-Session-Id') == 'fixture-session'
                if method == 'notifications/initialized':
                    self.send_response(202)
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                if method == 'initialize':
                    result = {'protocolVersion': '2025-11-25', 'capabilities': {
                        'tools': {}, 'resources': {}, 'prompts': {}}, 'serverInfo': {'name': 'Fixture', 'version': '1'}}
                elif method == 'tools/list':
                    result = {'tools': [{'name': 'send', 'description': 'Controlled fixture; not a real message.',
                        'inputSchema': {'type': 'object', 'properties': {'text': {'type': 'string'}},
                                        'additionalProperties': owner.changed}}]}
                elif method == 'resources/list':
                    result = {'resources': [{'uri': 'fixture://status', 'name': 'Status'}]}
                elif method == 'prompts/list':
                    result = {'prompts': [{'name': 'fixture-summary', 'description': 'Controlled prompt'}]}
                elif method == 'tools/call':
                    assert request['params']['name'] == 'send'
                    owner.effects.append(request['params']['arguments'])
                    if owner.failure:
                        self.send_response(503)
                        self.send_header('Content-Length', '0')
                        self.end_headers()
                        return
                    result = {'content': [{'type': 'text', 'text': 'Fixture effect recorded'}]}
                elif method == 'resources/read':
                    result = {'contents': [{'uri': 'fixture://status', 'text': 'fixture-ready'}]}
                elif method == 'prompts/get':
                    result = {'messages': [{'role': 'user', 'content': {'type': 'text', 'text': 'Summarize fixture'}}]}
                else:
                    raise AssertionError(method)
                encoded = json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Mcp-Session-Id', 'fixture-session')
                self.send_header('Content-Length', str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.server.server_port}/mcp'

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def configure(self, sim):
        source = sim.request('agent.mcp.out.list', {})
        source = sim.request('agent.mcp.out.save', {'id': 'fixture', 'title': 'MCP fixture',
            'url': self.url, 'secret': 'mcp-fixture-secret', 'revision': source['revision']})
        assert 'mcp-fixture-secret' not in json.dumps(source)
        return sim.request('agent.mcp.out.refresh', {'id': 'fixture', 'revision': source['revision']})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = McpFixture()
    generation = 2
    answers = {}

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            text = next(m['content'] for m in reversed(request['messages']) if m['role'] == 'user')
            results = [m for m in request['messages'] if m['role'] == 'tool']
            if not results:
                arguments = {'topic': 'agent.mcp.out.call', 'arguments': {
                    'server': 'fixture', 'kind': 'tools', 'name': 'send',
                    'generation': generation, 'arguments': {'text': text}}}
                name = 'nyabula_action'
                if text == 'catalog':
                    name = 'nyabula_mcp_catalog'
                    arguments = {'server': 'fixture', 'kind': 'tools', 'name': 'send'}
                message = {'role': 'assistant', 'content': None, 'tool_calls': [{
                    'id': 'fixture-one', 'type': 'function', 'function': {
                        'name': name, 'arguments': json.dumps(arguments)}}]}
            else:
                answers[text] = json.loads(results[-1]['content'])
                message = {'role': 'assistant', 'content': 'Observed MCP result'}
            body = json.dumps({'choices': [{'message': message,
                'finish_reason': 'stop' if results else 'tool_calls'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    logs = bytearray()
    try:
        with tempfile.TemporaryDirectory(prefix='nyabot-mcp-sim-') as directory:
            sim = Simulator(args.binary.resolve(), directory)
            try:
                sim.request('system.time.set', {'unix_ms': int(time.time() * 1000)})
                end = time.monotonic() + 15
                while not sim.request('agent.status', {})['ready']:
                    assert time.monotonic() < end
                sim.request('agent.config.set', {'host': '127.0.0.1', 'port': str(server.server_port),
                    'path': '/v1/chat/completions', 'model': 'fixture', 'key': 'synthetic-fixture-only'})
                source = fixture.configure(sim)
                generation = source['items'][0]['generation']
                assert source['items'][0]['grants'] == []
                sim.request('agent.mcp.out.call', {}, error=-38)

                def grant(enabled):
                    state = sim.request('agent.mcp.out.list', {})
                    return sim.request('agent.mcp.out.grant', {'id': 'fixture', 'revision': state['revision'],
                        'generation': generation, 'grants': [{'kind': 'tools', 'name': 'send'}] if enabled else []})

                def state(run, waiting=False):
                    end = time.monotonic() + 15
                    while time.monotonic() < end:
                        current = sim.request('agent.run.get', {'id': run['id']})
                        if waiting and current['state'] == 'waiting_approval': return current
                        if current['state'] not in ('queued', 'running', 'waiting_approval', 'cancelling'): return current
                    raise AssertionError(current)

                grant(True)
                catalog = state(sim.request('agent.chat', {'requestId': 'catalog', 'conversationId': 'catalog', 'text': 'catalog'}))
                assert catalog['state'] == 'succeeded', catalog
                assert answers['catalog']['items'][0]['entry']['name'] == 'send', answers
                for case in ('deny', 'allow', 'revoke', 'drift', 'uncertain'):
                    grant(True)
                    before = len(fixture.effects)
                    run = sim.request('agent.chat', {'requestId': case, 'conversationId': case, 'text': case})
                    waiting = state(run, True)
                    assert waiting['state'] == 'waiting_approval', waiting
                    assert len(fixture.effects) == before
                    assert waiting['steps'][0]['arguments']['arguments']['text'] == case
                    if case == 'revoke': grant(False)
                    fixture.changed = case == 'drift'
                    fixture.failure = case == 'uncertain'
                    sim.request('agent.approval.decide', {'runId': run['id'], 'step': 0,
                        'decision': 'deny' if case == 'deny' else 'allow'})
                    done = state(run)
                    assert len(fixture.effects) == before + int(case in ('allow', 'uncertain')), done
                    if case == 'uncertain':
                        assert done['steps'][0]['sideEffectUncertain'], done
                        assert 'sideEffectApplied' not in done['steps'][0], done
                        assert answers[case]['sideEffectUncertain'] and 'sideEffectApplied' not in answers[case]
                    elif case == 'allow': assert done['steps'][0]['sideEffectApplied'], done
                    else: assert not done['steps'][0].get('sideEffectApplied'), done
                saved = sim.request('agent.mcp.out.list', {})
                saved_runs = sim.request('agent.runs.list', {})
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
            sim = Simulator(args.binary.resolve(), directory)
            try:
                assert sim.request('agent.mcp.out.list', {}) == saved
                assert sim.request('agent.runs.list', {}) == saved_runs
                assert len(fixture.effects) == 2
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
        result = {'native_http_initialize_catalog': True, 'catalog_tool': True,
                  'approval_allow_deny': True, 'revoke_before_dispatch': True, 'schema_drift': True,
                  'uncertain_no_retry': True, 'sql_restart': True, 'effects': len(fixture.effects)}
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result))
    finally:
        fixture.close()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
