#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Official ai_agent loop via Core: local synthetic model, no paid service."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import secrets
import socket
from pathlib import Path
import tempfile
import threading
import time
from product_records_sim import Simulator
from eye_ws_integration import Peer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--browser-session', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    calls = []
    mcp_fixture = None

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            text = next(m['content'] for m in reversed(request['messages']) if m['role'] == 'user')
            calls.append(text)
            if text == 'skill fixture':
                system = request['messages'][0]['content']
                assert 'fixture-skill' in system and 'owner approval' in system, system
                assert 'no permission needed' not in system
                results = [m for m in request['messages'] if m['role'] == 'tool']
                if not results:
                    message = {'role': 'assistant', 'content': None, 'tool_calls': [
                        {'id': 'skill-read', 'type': 'function', 'function': {
                            'name': 'nyabula_skill_read', 'arguments': json.dumps({'id': 'fixture-skill'})}}]}
                else:
                    skill = json.loads(results[-1]['content'])
                    assert skill['content'] == '# Fixture\nUse the approved Core tools only.', skill
                    message = {'role': 'assistant', 'content': 'Read enabled skill from Core'}
                body = json.dumps({'choices': [{'message': message, 'finish_reason': 'tool_calls' if not results else 'stop'}]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if text.startswith('browser-write '):
                results = [m for m in request['messages'] if m['role'] == 'tool']
                if len(results) < 2:
                    name = 'nyabula_read' if not results else 'nyabula_action'
                    arguments = {'topic': 'memory.list'} if not results else {
                        'topic': 'memory.create', 'arguments': {'revision': json.loads(results[-1]['content'])['revision'],
                            'record': {'text': text}}}
                    body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': None,
                        'tool_calls': [{'id': 'browser-' + str(len(results)), 'type': 'function', 'function': {
                            'name': name, 'arguments': json.dumps(arguments)}}]}, 'finish_reason': 'tool_calls'}]}).encode()
                else:
                    result = json.loads(results[-1]['content'])
                    answer = 'Approved Core write: ' + text if 'items' in result else 'Core write was not applied'
                    body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': answer},
                                                    'finish_reason': 'stop'}]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if text in ('approved write', 'denied write', 'cancelled write'):
                tool_results = [m for m in request['messages'] if m['role'] == 'tool']
                if not tool_results:
                    revision = 1 if text == 'approved write' else 2
                    body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': None,
                        'tool_calls': [{'id': 'write-one', 'type': 'function', 'function': {'name': 'nyabula_action',
                        'arguments': json.dumps({'topic': 'memory.create', 'arguments': {'revision': revision,
                            'record': {'text': 'Approval memory fixture'}}})}}]}, 'finish_reason': 'tool_calls'}]}).encode()
                else:
                    response = json.loads(tool_results[-1]['content'])
                    if text == 'approved write':
                        assert response['items'][-1]['text'] == 'Approval memory fixture', response
                    else:
                        assert response['ok'] is False and response['sideEffectApplied'] is False, response
                    body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Write result observed'},
                                                    'finish_reason': 'stop'}]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if text == 'tool fixture':
                assert any(m.get('content') == 'Controlled reply: normal fixture' for m in request['messages'])
                tool_results = [m for m in request['messages'] if m['role'] == 'tool']
                if not tool_results:
                    assert any(t['function']['name'] == 'nyabula_read' for t in request['tools'])
                    body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': None,
                        'tool_calls': [{'id': 'tool-one', 'type': 'function', 'function': {'name': 'nyabula_read',
                        'arguments': json.dumps({'topic': 'memory.list'})}}]}, 'finish_reason': 'tool_calls'}]}).encode()
                else:
                    record = json.loads(tool_results[-1]['content'])
                    assert record['items'][0]['text'] == 'Core memory fixture', record
                    body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Read actual Core memory'},
                                                    'finish_reason': 'stop'}]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if text == 'delayed fixture':
                time.sleep(3)
            status = 503 if text == 'failure fixture' else 200
            body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Controlled reply: ' + text},
                                            'finish_reason': 'stop'}]}).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    logs = bytearray()
    try:
        with tempfile.TemporaryDirectory(prefix='nyabot-sim-') as directory:
            if args.browser_session:
                from media_runtime_sim import write_wave
                (Path(directory) / 'media').mkdir()
                (Path(directory) / 'audio-out').mkdir()
                write_wave(Path(directory) / 'media/browser.wav', 120)

            def start():
                sim = Simulator(args.binary.resolve(), directory)
                sim.request('system.time.set', {'unix_ms': int(time.time() * 1000)})
                end = time.monotonic() + 15
                while time.monotonic() < end:
                    status = sim.request('agent.status', {})
                    if status['ready']:
                        return sim, status
                    assert not status['lastError'], status
                raise AssertionError('Agent startup deadline')

            sim, status = start()
            try:
                assert not status['configured'] and not status['streaming'], status
                sim.request('agent.chat', {'requestId': 'empty', 'conversationId': 'chat-test', 'text': 'hello'}, error=-61)
                assert sim.request('agent.runs.list', {}) == {'runs': [], 'revision': 0}
                assert calls == []
                catalog = sim.request('agent.skills.list', {})
                assert len(catalog['items']) == 10 and catalog['revision'] == 0, catalog
                assert all(not s['enabled'] and s['source'] == 'builtin' for s in catalog['items'])
                assert 'Translate' in sim.request('agent.skills.get', {'id': 'translate'})['content']
                sim.request('agent.skills.read', {'id': 'translate'}, error=-13)
                sim.request('agent.skills.delete', {'id': 'translate', 'revision': 0}, error=-1)
                skill_data = {'id': 'fixture-skill', 'title': 'Fixture skill', 'content': '# Fixture\nUse the approved Core tools only.'}
                sim.request('agent.skills.save', {**skill_data, 'id': '../bad', 'revision': 0}, error=-22)
                catalog = sim.request('agent.skills.save', {**skill_data, 'revision': 0})
                assert catalog['revision'] == 1 and not catalog['items'][-1]['enabled']
                sim.request('agent.skills.enable', {'id': 'fixture-skill', 'enabled': True, 'revision': 0}, error=-116)
                catalog = sim.request('agent.skills.enable', {'id': 'fixture-skill', 'enabled': True, 'revision': 1})
                assert sim.request('agent.skills.read', {'id': 'fixture-skill'})['content'] == skill_data['content']
                capabilities = sim.request('agent.capabilities', {})['items']
                assert next(c for c in capabilities if c['id'] == 'core-read')['enabled']
                incoming = next(c for c in capabilities if c['id'] == 'mcp-server')
                assert incoming['compiled'] and incoming['enabled'] and incoming['reason'] == 'core_scoped_mcp_in'
                assert not sim.request('agent.mcp.in.list', {})['enabled']
                configuration = {'host': '127.0.0.1', 'port': str(server.server_port),
                                 'path': '/v1/chat/completions', 'model': 'controlled', 'key': 'synthetic-fixture-only'}
                configured = sim.request('agent.config.set', configuration)
                assert configured['keySet'] and configured['canSave'], configured
                assert 'synthetic-fixture-only' not in json.dumps(configured)
                assert sim.request('agent.status', {})['configured']
                sim.request('agent.config.set', {**configuration, 'host': 'bad\r\nhost'}, error=-22)
                blocked = Path(directory) / 'ai-agent/config/config.json.tmp'
                blocked.mkdir()
                try:
                    sim.request('agent.config.set', {**configuration, 'model': 'must-not-apply'}, error=-5)
                    assert sim.request('agent.config.get', {})['model'] == 'controlled'
                finally:
                    blocked.rmdir()
                token = secrets.token_hex(32)
                (Path(directory) / 'token').write_text(token)
                reserve = socket.socket()
                reserve.bind(('127.0.0.1', 0))
                web_port = reserve.getsockname()[1]
                reserve.close()
                sim.command(f'nyabula_web {web_port} /host/token http://127.0.0.1:5180 &')
                peer = Peer(web_port)
                try:
                    assert peer.request('sys.hello', {'token': token})['type'] == 'res'
                    assert peer.request('agent.config.get', {})['data']['canSave']
                    applied = peer.request('agent.config.set', configuration)
                    assert applied['type'] == 'res' and applied['data']['keySet'], applied
                    assert configuration['key'] not in json.dumps(applied)
                finally:
                    peer.close()
                    sim.command('nycore web-stop')
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)

            sim, status = start()
            try:
                assert status['configured'], status
                if args.browser_session:
                    from agent_mcp_sim import McpFixture
                    from agent_mqtt_sim import Broker
                    from agent_weixin_sim import WeixinFixture
                    from agent_feishu_sim import FeishuFixture
                    from agent_node_sim import GatewayFixture
                    mqtt_fixture = Broker()
                    weixin_fixture = WeixinFixture()
                    feishu_fixture = FeishuFixture()
                    node_fixture = GatewayFixture()
                    sim.request('agent.channels.weixin.save', {'host': '127.0.0.1', 'port': str(weixin_fixture.port), 'token': ''})
                    sim.request('agent.channels.feishu.save', {'appId': 'cli_browser', 'secret': 'synthetic-feishu-browser', 'fixturePort': str(feishu_fixture.port)})
                    sim.request('agent.node.save', {'host': '127.0.0.1', 'port': str(node_fixture.port), 'tls': False, 'token': 'synthetic-node-browser'})
                    mcp_fixture = McpFixture()
                    mcp_fixture.configure(sim)
                    for form in ('desktop', 'phone'):
                        incoming = sim.request('agent.mcp.in.list', {})
                        sim.request('agent.mcp.in.save', {'revision': incoming['revision'],
                            'id': 'browser-' + form, 'title': 'Browser incoming ' + form,
                            'expiresAt': int(time.time() * 1000) + 3600000, 'scopes': [],
                            'token': ('1234567890abcdef' if form == 'desktop' else 'abcdef1234567890') * 4})
                    token = secrets.token_hex(32)
                    (Path(directory) / 'token').write_text(token)
                    reserve = socket.socket()
                    reserve.bind(('127.0.0.1', 0))
                    port = reserve.getsockname()[1]
                    reserve.close()
                    sim.command(f'nyabula_web {port} /host/token http://127.0.0.1:5180 &')
                    args.browser_session.write_text(json.dumps({'port': port, 'token': token,
                                                               'mqttPort': mqtt_fixture.port, 'weixinPort': weixin_fixture.port,
                                                               'nodePort': node_fixture.port}))
                    args.browser_session.chmod(0o600)
                    print('BROWSER_SESSION_READY', flush=True)
                    end = time.monotonic() + 300
                    while args.browser_session.exists() and time.monotonic() < end:
                        sim.drain(.1)
                    sim.command('nycore web-stop')
                    mqtt_fixture.server.close()
                    weixin_fixture.close()
                    feishu_fixture.close()
                    node_fixture.close()
                    return

                def finish(run):
                    end = time.monotonic() + 15
                    while time.monotonic() < end:
                        current = sim.request('agent.run.get', {'id': run['id']})
                        if current['state'] not in ('queued', 'running', 'waiting_approval', 'cancelling'):
                            return current
                    raise AssertionError(('Run deadline', current))

                request = {'requestId': 'request-one', 'conversationId': 'chat-test', 'text': 'normal fixture'}
                first = sim.request('agent.chat', request)
                complete = finish(first)
                assert complete['state'] == 'succeeded', complete
                assert complete['reply'] == 'Controlled reply: normal fixture', complete
                assert sim.request('agent.chat', request) == complete
                assert calls == ['normal fixture'], calls
                sim.request('agent.chat', {**request, 'text': 'different'}, error=-17)
                sim.request('memory.create', {'revision': 0, 'record': {'text': 'Core memory fixture'}})
                tool = finish(sim.request('agent.chat', {**request, 'requestId': 'tool', 'text': 'tool fixture'}))
                assert tool['state'] == 'succeeded' and tool['reply'] == 'Read actual Core memory', tool
                assert tool['steps'][0]['state'] == 'succeeded' and tool['steps'][0]['topic'] == 'memory.list', tool
                failed = finish(sim.request('agent.chat', {**request, 'requestId': 'failure', 'text': 'failure fixture'}))
                assert failed['state'] == 'failed', failed
                for text, decision in [('approved write', 'allow'), ('denied write', 'deny'), ('cancelled write', 'cancel')]:
                    before = sim.request('memory.list', {})
                    proposed = sim.request('agent.chat', {'requestId': text.replace(' ', '-'),
                        'conversationId': text.replace(' ', '-'), 'text': text})
                    end = time.monotonic() + 10
                    while time.monotonic() < end:
                        waiting = sim.request('agent.run.get', {'id': proposed['id']})
                        if waiting['state'] == 'waiting_approval':
                            break
                    assert waiting['state'] == 'waiting_approval', waiting
                    assert sim.request('memory.list', {}) == before
                    if decision == 'cancel':
                        sim.request('agent.cancel', {'id': proposed['id']})
                    else:
                        sim.request('agent.approval.decide', {'runId': proposed['id'], 'step': 0, 'decision': decision})
                    completed = finish(proposed)
                    after = sim.request('memory.list', {})
                    if decision == 'allow':
                        assert completed['state'] == 'succeeded' and completed['steps'][0]['sideEffectApplied'], completed
                        assert len(after['items']) == len(before['items']) + 1
                    else:
                        assert completed['state'] == ('cancelled' if decision == 'cancel' else 'failed'), completed
                        assert after == before
                pending = sim.request('agent.chat', {**request, 'requestId': 'cancel', 'text': 'delayed fixture'})
                time.sleep(.2)
                cancelling = sim.request('agent.cancel', {'id': pending['id']})
                assert cancelling['state'] == 'cancelling', cancelling
                cancelled = finish(pending)
                assert cancelled['state'] == 'cancelled', cancelled
                saved = sim.request('agent.runs.list', {})
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)

            sim, status = start()
            try:
                assert sim.request('agent.runs.list', {}) == saved
                assert len(calls) == 10, calls
                memory = sim.request('memory.list', {})
                sim.request('agent.conversation.delete', {'conversationId': 'chat-test', 'revision': saved['revision'] - 1}, error=-116)
                deleted = sim.request('agent.conversation.delete', {'conversationId': 'chat-test', 'revision': saved['revision']})
                assert deleted['deleted'] == 4, deleted
                remaining = sim.request('agent.runs.list', {})
                tombstones = [r for r in remaining['runs'] if r['state'] == 'deleted']
                assert len(tombstones) == 4 and all('text' not in r and 'reply' not in r for r in tombstones)
                assert sim.request('memory.list', {}) == memory
                sim.request('agent.chat', request, error=-114)
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
            sim, status = start()
            try:
                sim.request('agent.chat', request, error=-114)
                assert sim.request('memory.list', {}) == memory
                assert len(calls) == 10
                catalog = sim.request('agent.skills.list', {})
                assert catalog['revision'] == 2 and catalog['items'][-1]['enabled'], catalog
                skill_run = finish(sim.request('agent.chat', {'requestId': 'skill-read-final',
                    'conversationId': 'skills', 'text': 'skill fixture'}))
                assert skill_run['state'] == 'succeeded' and skill_run['reply'] == 'Read enabled skill from Core', skill_run
                assert skill_run['steps'][0]['topic'] == 'agent.skills.read'
                updated = sim.request('agent.skills.save', {**skill_data, 'revision': 2})
                assert not updated['items'][-1]['enabled']
                sim.request('agent.skills.read', {'id': 'fixture-skill'}, error=-13)
                removed = sim.request('agent.skills.delete', {'id': 'fixture-skill', 'revision': updated['revision']})
                assert len(removed['items']) == 10
                sim.request('agent.skills.get', {'id': 'fixture-skill'}, error=-2)
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
        result = {'unconfigured': True, 'official_loop': True, 'idempotent': True,
                  'failure': True, 'cancel_confirmed': True, 'restart_history': True,
                  'shared_core_memory_tool': True, 'core_history_context': True,
                  'config_atomic': True, 'owner_web_config': True, 'approval_allow_deny_cancel': True,
                  'delete_preserves_records_and_deduplication': True,
                  'skills_crud_restart_context_tool': True,
                  'model_requests': len(calls)}
        (args.output / 'result.json').write_text(json.dumps(result, indent=2))
        print('NYABOT_SIM_PASS ' + json.dumps(result))
    finally:
        if args.browser_session:
            args.browser_session.unlink(missing_ok=True)
        if mcp_fixture:
            mcp_fixture.close()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
