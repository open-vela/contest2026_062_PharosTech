#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real NuttX loopback MCP server: scopes, isolation, rotation, and restart."""
import argparse
from contextlib import closing
import http.client
import json
from pathlib import Path
import socket
import sqlite3
import tempfile
import time
from product_records_sim import Simulator

TOKEN_A = '0123456789abcdef' * 4
TOKEN_B = 'fedcba9876543210' * 4
TOKEN_C = 'abcdef0123456789' * 4


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    durations = []
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]

    def rpc(method='tools/list', params=None, token=TOKEN_A, identity=2147483659,
            status=200, headers=None, verb='POST', raw=None):
        request = {'jsonrpc': '2.0', 'method': method}
        if identity is not None:
            request['id'] = identity
        if params is not None:
            request['params'] = params
        body = json.dumps(request) if raw is None else raw
        head = {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json',
                'Accept': 'application/json, text/event-stream',
                'MCP-Protocol-Version': '2025-11-25'}
        if headers:
            head.update(headers)
        started = time.monotonic()
        with closing(http.client.HTTPConnection('127.0.0.1', port, timeout=8)) as connection:
            connection.request(verb, '/mcp', body, head)
            reply = connection.getresponse()
            content = reply.read()
            assert reply.status == status, (reply.status, content, method)
            assert reply.getheader('Cache-Control') == 'no-store'
        durations.append((time.monotonic() - started) * 1000)
        response = json.loads(content) if content else None
        if response and 'id' in response and raw is None:
            assert response['id'] == identity, response
        return response

    def wire(headers):
        with socket.create_connection(('127.0.0.1', port), timeout=8) as connection:
            connection.sendall(('POST /mcp HTTP/1.1\r\n' + headers + '\r\n\r\n').encode())
            return connection.recv(1024).decode().split('\r\n')[0]

    with tempfile.TemporaryDirectory(prefix='nyabot-mcp-in-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            now = int(time.time() * 1000)
            sim.request('system.time.set', {'unix_ms': now})
            initial = sim.request('agent.mcp.in.list', {})
            assert not initial['enabled'] and initial['clients'] == [] and initial['listenerPort'] == 0
            sim.command(f'nyabula_mcp {port} &')
            rpc(status=503)
            enabled = sim.request('agent.mcp.in.enable', {'revision': initial['revision'], 'enabled': True})
            rpc(status=401)
            client = {'id': 'test-client', 'title': 'Controlled local client',
                      'expiresAt': now + 600000, 'scopes': ['nyabula_time'], 'token': TOKEN_A}
            saved = sim.request('agent.mcp.in.save', {**client, 'revision': enabled['revision']})
            assert saved['listenerPort'] == port and saved['clients'][0]['scopes'] == ['nyabula_time']
            assert 'hash' not in saved['clients'][0] and TOKEN_A not in json.dumps(saved)
            sim.request('agent.mcp.in.save', {**client, 'revision': enabled['revision']}, error=-116)
            sim.request('agent.mcp.in.save', {**client, 'scopes': ['agent.approval.decide'],
                                             'revision': saved['revision']}, error=-22)
            result = rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                         'clientInfo': {'name': 'fixture', 'version': '1'}}, identity='string:exact')['result']
            assert result['capabilities'] == {'tools': {}} and result['protocolVersion'] == '2025-11-25'
            rpc('notifications/initialized', identity=None, status=202)
            catalog = rpc()['result']['tools']
            assert [tool['name'] for tool in catalog] == ['nyabula_time']
            assert catalog[0]['annotations']['readOnlyHint']
            out = rpc('tools/call', {'name': 'nyabula_time', 'arguments': {}})['result']
            assert not out['isError']
            assert json.loads(out['content'][0]['text'])['clock_valid']
            for name in ['nyabula_tasks', 'agent.chat', 'agent.approval.decide',
                         'agent.mcp.out.call', 'memory.list', 'shell']:
                denied = rpc('tools/call', {'name': name, 'arguments': {}})['result']
                assert denied['isError'], name
            assert rpc('tools/call', {'name': 'nyabula_time', 'arguments': {'topic': 'memory.list'}})['result']['isError']
            assert rpc('resources/list')['error']['code'] == -32601
            assert rpc('tools/list', {'cursor': 'unsupported'})['error']['code'] == -32602
            rpc(token=TOKEN_B, status=401)
            rpc(headers={'Origin': 'https://attacker.test'}, status=403)
            rpc(headers={'Host': 'attacker.test'}, status=403)
            rpc(headers={'MCP-Protocol-Version': '2024-11-05'}, status=400)
            rpc(verb='GET', status=405)
            rpc('ping', headers={'X-Padding': 'x' * 1024})
            rpc(raw='[]')
            rpc(raw=b'{"jsonrpc":"2.0","method":"ping","id":"\xff"}', status=400)
            rpc(raw=r'{"jsonrpc":"2.0","method":"ping","id":"nul\u0000tail"}', status=400)
            assert '400' in wire(f'Host: 127.0.0.1:{port}\r\nAuthorization: Bearer {TOKEN_A}\r\nAuthorization: Bearer {TOKEN_B}')
            assert '400' in wire(f'Host: 127.0.0.1:{port}\r\nTransfer-Encoding: chunked')
            before = sim.request('agent.mcp.in.list', {})
            assert any(event['status'] == -13 for event in before['audit'])
            assert all('arguments' not in event and 'token' not in event for event in before['audit'])
            client.pop('token')
            revoked = sim.request('agent.mcp.in.save', {**client, 'scopes': [], 'revision': before['revision']})
            assert rpc()['result']['tools'] == []
            assert rpc('tools/call', {'name': 'nyabula_time'})['result']['isError']
            before = sim.request('agent.mcp.in.list', {})
            rotated = sim.request('agent.mcp.in.save', {**client, 'token': TOKEN_B, 'revision': before['revision']})
            rpc(token=TOKEN_A, status=401)
            assert rpc(token=TOKEN_B)['result']['tools'][0]['name'] == 'nyabula_time'
            sim.request('agent.mcp.in.save', {**client, 'id': 'duplicate-token', 'token': TOKEN_B,
                                             'revision': rotated['revision']}, error=-17)
            client2 = {**client, 'id': 'second-client', 'title': 'Tasks only',
                       'scopes': ['nyabula_tasks'], 'token': TOKEN_C}
            saved = sim.request('agent.mcp.in.save', {**client2, 'revision': rotated['revision']})
            assert rpc(token=TOKEN_C)['result']['tools'][0]['name'] == 'nyabula_tasks'
            assert rpc('tools/call', {'name': 'nyabula_time'}, token=TOKEN_C)['result']['isError']
            records = sim.request('task.create', {'revision': 0, 'record': {'title': 'Read-only fixture', 'state': 'queued'}})
            read = rpc('tools/call', {'name': 'nyabula_tasks'}, token=TOKEN_C)['result']
            assert not read['isError'] and json.loads(read['content'][0]['text']) == records
            before = sim.request('agent.mcp.in.list', {})
            off = sim.request('agent.mcp.in.enable', {'revision': before['revision'], 'enabled': False})
            rpc(token=TOKEN_B, status=503)
            sim.request('agent.mcp.in.enable', {'revision': off['revision'], 'enabled': True})
            sim.command('nyabula_mcp stop')
            time.sleep(.2)
            assert sim.request('agent.mcp.in.list', {})['listenerPort'] == 0
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)

        with sqlite3.connect(Path(directory) / 'nyabula-product.sqlite') as database:
            stored = database.execute('SELECT v FROM state WHERE k=?', ('product/v1/mcp-in',)).fetchone()[0]
            stored = stored.decode() if isinstance(stored, bytes) else stored
            assert TOKEN_A not in stored and TOKEN_B not in stored and TOKEN_C not in stored
            assert '"hash"' in stored
        sim = Simulator(args.binary.resolve(), directory)
        try:
            sim.request('system.time.set', {'unix_ms': now})
            state = sim.request('agent.mcp.in.list', {})
            assert state['enabled'] and len(state['clients']) == 2 and state['listenerPort'] == 0
            sim.command(f'nyabula_mcp {port} &')
            rpc(token=TOKEN_A, status=401)
            rpc(token=TOKEN_B)
            sim.request('agent.mcp.in.delete', {'id': 'test-client', 'revision': state['revision']})
            rpc(token=TOKEN_B, status=401)
            rpc(token=TOKEN_C)
            sim.request('system.time.set', {'unix_ms': now + 700000})
            rpc(token=TOKEN_C, status=401)
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
    result = {'real_http': True, 'initialize_exact_ids': True, 'scope_isolation': True,
              'readonly_core_data': True, 'no_owner_or_write_access': True,
              'origin_host_header_guards': True, 'rotation_revoke_expiry': True,
              'sqlite_hash_only_restart': True, 'audit': True, 'emergency_off': True}
    result['rpc_max_ms'] = round(max(durations), 2)
    result['rpc_mean_ms'] = round(sum(durations) / len(durations), 2)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_MCP_IN_SIM_PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
