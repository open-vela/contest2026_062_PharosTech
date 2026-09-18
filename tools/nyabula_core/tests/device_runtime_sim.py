#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare native device diagnostics with runtime facts and an Agent tool call."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
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
    seen = []

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            results = [item for item in request['messages'] if item['role'] == 'tool']
            if not results:
                message = {'role': 'assistant', 'content': None, 'tool_calls': [{
                    'id': 'device-read', 'type': 'function', 'function': {
                        'name': 'nyabula_read', 'arguments': '{"topic":"device.status"}'}}]}
            else:
                seen.append(json.loads(results[-1]['content']))
                message = {'role': 'assistant', 'content': 'Read actual NuttX device status'}
            body = json.dumps({'choices': [{'message': message, 'finish_reason': 'tool_calls' if not results else 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='nyabula-device-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            first = sim.request('device.status', {})
            assert first['os'] == 'NuttX' and first['simulator'], first
            memory = first['memory']
            assert 0 < memory['usedBytes'] < memory['totalBytes'] and memory['freeBytes'] > 0, memory
            assert first['cpu']['available'] and 0 <= first['cpu']['percent'] <= 100, first['cpu']
            volume = next(item for item in first['storage'] if item['path'] == '/host')
            host = os.statvfs(directory)
            assert volume['available'] and volume['totalBytes'] == host.f_blocks * host.f_frsize, volume
            assert 0 <= volume['freeBytes'] <= volume['totalBytes']
            assert first['network']['available'] and isinstance(first['network']['interfaces'], list), first['network']
            # The socket-proxy simulator need not register local netdevs.
            for interface in first['network']['interfaces']:
                assert interface['name'] and interface['index'] > 0, interface
            assert all(item['available'] and item.get('output') and not item.get('input') for item in first['audioDevices']), first['audioDevices']
            deadline = time.monotonic() + 20
            while not sim.request('agent.status', {})['ready']:
                assert time.monotonic() < deadline
            sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                'port': str(server.server_port), 'model': 'fixture', 'key': 'synthetic-device-key'})
            run = sim.request('agent.chat', {'requestId': 'device-read', 'conversationId': 'device-read', 'text': 'Read the device status'})
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                current = sim.request('agent.run.get', {'id': run['id']})
                if current['state'] not in ('queued', 'running'):
                    assert current['state'] == 'succeeded', current
                    break
            else:
                raise AssertionError(current)
            assert len(seen) == 1 and seen[0]['memory']['totalBytes'] == memory['totalBytes'], seen
            second = sim.request('device.status', {})
            assert second['uptimeMs'] > first['uptimeMs']
            (args.output / 'snapshot.json').write_text(json.dumps(second, indent=2))
        finally:
            sim.close()
            (args.output / 'serial.log').write_bytes(sim.log)
            server.shutdown()
            server.server_close()
    print('DEVICE_RUNTIME_SIM_PASS actualMemory schedulerSample hostStorage runtimeInterfaceEnumeration noFakeAudio agentTool')


if __name__ == '__main__':
    main()
