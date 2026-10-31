#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Full HTTP response >8 KiB, reasoning echo and native Agent-owned deployment."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
from product_records_sim import Simulator

SOURCE = "import * as ui from '@nyabula/ui';\n/*" + '\\"' * 2500 + "*/\n" + """
export default {onStart(){ui.render({nyaui:1,page:{title:'Payload fixture'},
root:{type:'text',props:{text:'Large payload verified'}}})}};
"""
REASONING = 'Controlled reasoning fixture. ' * 400
calls = []
class Model(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        plugin = next(t['function'] for t in request['tools'] if t['function']['name'] == 'nyabula_plugin')
        fields = plugin['parameters']['properties']['arguments']['properties']
        assert 'componentId' in fields['event']['properties']
        assert fields['event']['required'] == ['event']
        calls.append(request)
        if len(calls) == 1:
            arguments = {'operation': 'deploy', 'arguments': {
                'id': 'agent-large-fixture', 'name': 'Payload fixture', 'source': SOURCE,
                'permissions': ['ui.page'], 'baseVersion': ''}}
            message = {'role': 'assistant', 'content': '', 'reasoning_content': REASONING,
                       'tool_calls': [{'id': 'large-call', 'type': 'function', 'function': {
                           'name': 'nyabula_plugin', 'arguments': json.dumps(arguments)}}]}
            finish = 'tool_calls'
        else:
            previous = next(m for m in request['messages'] if m.get('tool_calls'))
            assert previous['reasoning_content'] == REASONING
            output = next(m['content'] for m in reversed(request['messages']) if m.get('role') == 'tool')
            result = json.loads(output)
            if len(calls) == 2:
                assert result['startupVerified'] and result['state'] == 'running', result
                message = {'role': 'assistant', 'content': '', 'reasoning_content': REASONING,
                           'tool_calls': [{'id': 'inspect-call', 'type': 'function', 'function': {
                               'name': 'nyabula_plugin', 'arguments': json.dumps({
                                   'operation': 'inspect', 'arguments': {'id': 'agent-large-fixture'}})}}]}
                finish = 'tool_calls'
            else:
                assert len(output.encode()) > 8192 and result['source'] == SOURCE, result
                message, finish = {'role': 'assistant', 'content': 'Installed and inspected fixture.'}, 'stop'
        body = json.dumps({'choices': [{'message': message, 'finish_reason': finish}]}).encode()
        if len(calls) == 1: assert len(body) > 8192
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        for offset in range(0, len(body), 997):
            self.wfile.write(body[offset:offset + 997])
            self.wfile.flush()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='ny-large-response-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            for _ in range(50):
                if sim.request('agent.status', {})['ready']: break
                time.sleep(.05)
            sim.request('agent.config.set', {'host': '127.0.0.1', 'port': str(server.server_port),
                'path': '/v1/chat/completions', 'model': 'controlled', 'key': 'synthetic-fixture-only'})
            sim.request('plugin.author.policy.set', {'enabled': True})
            submitted = sim.request('agent.chat', {'conversationId': 'large-response',
                'requestId': 'large-response', 'text': 'Controlled large deployment fixture.'})
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                run = sim.request('agent.run.get', {'id': submitted['id']})
                if run['state'] in ('succeeded', 'failed'): break
                time.sleep(.1)
            assert run['state'] == 'succeeded', run
            assert run['steps'][0]['topic'] == 'plugin.author.deploy'
            assert sim.request('plugin.author.inspect', {'id': 'agent-large-fixture'})['source'] == SOURCE
            tree = sim.request('ui.tree', {'pluginId': 'agent-large-fixture'})
            assert tree['root']['props']['text'] == 'Large payload verified'
            (args.output / 'result.json').write_text(json.dumps(run, indent=2))
        finally:
            sim.close()
            (args.output / 'serial.log').write_bytes(sim.log)
    server.shutdown()
    assert len(calls) == 3
    print('NATIVE_AGENT_LARGE_RESPONSE_REASONING_ECHO_SIGNED_DEPLOY_INSPECT_PASS')

if __name__ == '__main__': main()
