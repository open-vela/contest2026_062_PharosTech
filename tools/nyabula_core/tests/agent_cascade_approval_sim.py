#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise AUTO model escalation with real Core approval and workspace tools."""
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
    parser.add_argument('--cases', default='allow_text,allow_null,deny,cancel,read,text,error,single,plain_claim')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    calls = []
    cases = {}

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            operation = next(m['content'] for m in reversed(request['messages']) if m['role'] == 'user')
            results = [m for m in request['messages'] if m['role'] == 'tool']
            calls.append({'case': operation, 'model': request['model'], 'hasToolResult': bool(results),
                          'offeredTools': [t['function']['name'] for t in request.get('tools', [])]})
            status = 200
            if operation == 'plain_claim':
                message = {'role': 'assistant', 'content': 'Submitted for approval.'}
            elif request['model'] == 'fixture-eco':
                message = {'role': 'assistant', 'content': 'I cannot complete this here.'}
            elif operation == 'error':
                status = 503
                message = {'role': 'assistant', 'content': 'synthetic unavailable'}
            elif operation == 'text':
                message = {'role': 'assistant', 'content': 'No operation requested.'}
            elif not results:
                write = operation != 'read'
                arguments = {'name': 'write_file', 'arguments': {
                    'path': operation + '.txt', 'content': 'CASCADE_APPROVED'}} if write else {
                    'name': 'list_dir', 'arguments': {'prefix': ''}}
                if write:
                    arguments = {'topic': 'agent.tools.call', 'arguments': arguments}
                message = {'role': 'assistant',
                    'content': None if operation == 'allow_null' else 'Submitted for approval.',
                    'tool_calls': [{'id': 'cascade-' + operation, 'type': 'function', 'function': {
                        'name': 'nyabula_action' if write else 'nyabula_tool',
                        'arguments': json.dumps(arguments)}}]}
            else:
                if operation == 'allow_null':
                    previous = next(m for m in reversed(request['messages']) if m.get('tool_calls'))
                    assert previous.get('reasoning_content') == 'SYNTHETIC_REASONING_NOT_FOR_LOG'
                message = {'role': 'assistant', 'content': 'Tool result received.'}
            if operation == 'allow_null' and message.get('tool_calls'):
                message['reasoning_content'] = 'SYNTHETIC_REASONING_NOT_FOR_LOG'
            body = json.dumps({'choices': [{'message': message,
                'finish_reason': 'tool_calls' if message.get('tool_calls') else 'stop'}],
                'usage': {'prompt_tokens': 12, 'completion_tokens': 4, 'total_tokens': 16}}).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for operation in args.cases.split(','):
            with tempfile.TemporaryDirectory(prefix='nyabot-cascade-') as directory:
                sim = Simulator(args.binary.resolve(), directory)
                current = None
                observed_approval = False
                decision_sent = False
                try:
                    deadline = time.monotonic() + 20
                    while not sim.request('agent.status', {})['ready']:
                        assert time.monotonic() < deadline
                    catalog = sim.request('agent.tools.catalog', {})
                    workspace = Path(directory) / Path(catalog['workspace']).relative_to('/host')
                    assert workspace.resolve().is_relative_to(Path(directory).resolve())
                    base = {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                            'port': str(server.server_port), 'key': 'synthetic-cascade-key',
                            'priority': 20, 'enabled': True}
                    sim.request('agent.config.router.save', {**base, 'index': 0,
                        'model': 'fixture-eco', 'cost_tier': 0})
                    if operation != 'single':
                        sim.request('agent.config.router.save', {**base, 'index': 3,
                            'model': 'fixture-premium', 'cost_tier': 3})
                    sim.request('agent.config.router.profile', {'profile': 'auto'})
                    run = sim.request('agent.chat', {'requestId': 'cascade-' + operation,
                        'conversationId': 'cascade-' + operation, 'text': operation})
                    destination = workspace / (operation + '.txt')
                    deadline = time.monotonic() + 25
                    while time.monotonic() < deadline:
                        current = sim.request('agent.run.get', {'id': run['id']})
                        if current['state'] == 'waiting_approval':
                            observed_approval = True
                            assert operation in {'allow_text', 'allow_null', 'deny', 'cancel'}
                            if not decision_sent:
                                assert not destination.exists(), 'Side effect occurred before approval'
                                assert current['steps'][0]['state'] == 'pending'
                                if operation == 'cancel':
                                    sim.request('agent.cancel', {'id': run['id']})
                                else:
                                    sim.request('agent.approval.decide', {'runId': run['id'], 'step': 0,
                                        'decision': 'deny' if operation == 'deny' else 'allow'})
                                decision_sent = True
                        elif current['state'] not in {'queued', 'running', 'cancelling'}:
                            break
                    else:
                        raise AssertionError(current)
                    case_calls = [c for c in calls if c['case'] == operation]
                    assert case_calls[0]['model'] == 'fixture-eco', case_calls
                    if operation not in {'single', 'plain_claim'}:
                        assert case_calls[1]['model'] == 'fixture-premium', case_calls
                    expected = 'failed' if operation == 'deny' else 'cancelled' if operation == 'cancel' else 'succeeded'
                    assert current['state'] == expected, current
                    if operation in {'allow_text', 'allow_null', 'deny', 'cancel'}:
                        assert observed_approval, 'Escalated tool call was dropped: ' + json.dumps(current)
                    if operation.startswith('allow_'):
                        assert destination.read_text() == 'CASCADE_APPROVED'
                        assert current['steps'][0]['state'] == 'succeeded'
                    else:
                        assert not destination.exists()
                    if operation == 'read':
                        assert len(current['steps']) == 1 and current['steps'][0]['state'] == 'succeeded'
                    if operation in {'text', 'error', 'single', 'plain_claim'}:
                        assert not current.get('steps')
                        expected_reply = 'No operation requested.' if operation == 'text' else 'Submitted for approval.' if operation == 'plain_claim' else 'I cannot complete this here.'
                        assert current['reply'] == expected_reply
                    trace = [line for line in sim.log.decode(errors='replace').splitlines() if 'Tool trace:' in line]
                    assert trace, 'Missing protocol-only diagnostics'
                    assert all('synthetic-cascade-key' not in line and 'Submitted for approval.' not in line
                               and 'SYNTHETIC_REASONING_NOT_FOR_LOG' not in line for line in trace)
                    if operation == 'plain_claim':
                        assert len(case_calls) == 1 and any('wire=0 structured=0 total=0 finish_tools=0 finish_stop=1' in line for line in trace), trace
                    if operation in {'allow_text', 'allow_null', 'deny', 'cancel', 'read'}:
                        assert any('wire=1 structured=1 total=1' in line for line in trace), trace
                    metrics = sim.request('agent.config.router.get', {})
                    if operation in {'allow_text', 'allow_null', 'read', 'text'}:
                        for backend in metrics['backends']:
                            actual = sum(c['model'] == backend['model'] for c in case_calls)
                            assert backend['total_calls'] == actual, (backend, actual)
                    cases[operation] = {'state': current['state'], 'approvalObserved': observed_approval,
                        'calls': case_calls, 'protocolTrace': trace, 'steps': current.get('steps', []), 'metrics': metrics}
                    print('CASCADE_CASE_PASS', operation, flush=True)
                finally:
                    sim.close()
                    (args.output / (operation + '.log')).write_bytes(sim.log)
                    (args.output / (operation + '.json')).write_text(json.dumps({
                        'run': current, 'approvalObserved': observed_approval,
                        'calls': [c for c in calls if c['case'] == operation]}, indent=2))
    finally:
        server.shutdown()
        server.server_close()
    (args.output / 'result.json').write_text(json.dumps(cases, indent=2))
    print('NYABOT_CASCADE_APPROVAL_SIM_PASS', ','.join(cases), flush=True)


if __name__ == '__main__':
    main()
