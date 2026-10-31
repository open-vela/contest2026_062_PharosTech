#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real Feishu HTTP + WebSocket/protobuf fixtures around native Core."""
import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import queue
import socket
import struct
import tempfile
import threading
import time
from product_records_sim import Simulator


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def number(field, value):
    return varint(field << 3) + varint(value)


def blob(field, value):
    if isinstance(value, str):
        value = value.encode()
    return varint((field << 3) | 2) + varint(len(value)) + value


class FeishuFixture:
    def __init__(self):
        self.sent = queue.Queue()
        self.connected = threading.Event()
        self.ws = None
        self.connections = 0
        self.acks = 0
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, data):
                body = json.dumps(data).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path.startswith('/ws'):
                    key = self.headers['Sec-WebSocket-Key']
                    accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                    self.send_response(101)
                    self.send_header('Upgrade', 'websocket')
                    self.send_header('Connection', 'Upgrade')
                    self.send_header('Sec-WebSocket-Accept', accept)
                    self.end_headers()
                    self.wfile.flush()
                    fixture.ws = self.connection
                    fixture.connections += 1
                    fixture.connected.set()
                    try:
                        while True:
                            header = self.rfile.read(2)
                            if len(header) != 2:
                                break
                            length = header[1] & 127
                            if length == 126:
                                length = struct.unpack('!H', self.rfile.read(2))[0]
                            elif length == 127:
                                length = struct.unpack('!Q', self.rfile.read(8))[0]
                            if header[1] & 128:
                                self.rfile.read(4)
                            self.rfile.read(length)
                            fixture.acks += 1
                    except OSError:
                        pass
                    finally:
                        if fixture.ws is self.connection:
                            fixture.ws = None
                    self.close_connection = True
                else:
                    self.reply({'code': 0, 'bot': {'open_id': 'ou_fixture_bot'}})

            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                if 'tenant_access_token' in self.path:
                    self.reply({'code': 0, 'tenant_access_token': 'synthetic-feishu-access', 'expire': 7200})
                elif self.path == '/callback/ws/endpoint':
                    self.reply({'code': 0, 'data': {'URL': 'wss://127.0.0.1/ws?service_id=1'}})
                elif self.path.startswith('/open-apis/im/v1/messages'):
                    fixture.sent.put(data)
                    self.reply({'code': 0, 'data': {'message_id': 'om_reply'}})
                else:
                    self.reply({'code': 0, 'data': {}})

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.port = self.server.server_port
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def publish(self, identifier, text):
        event = {'schema': '2.0', 'header': {'event_id': 'event-' + identifier, 'event_type': 'im.message.receive_v1'},
            'event': {'sender': {'sender_type': 'user', 'sender_id': {'open_id': 'ou_owner'}},
                'message': {'message_id': identifier, 'chat_id': 'oc_fixture', 'chat_type': 'p2p',
                    'message_type': 'text', 'content': json.dumps({'text': text})}}}
        payload = (number(1, 1) + number(3, 1) + number(4, 1) +
            blob(5, blob(1, 'type') + blob(2, 'event')) +
            blob(5, blob(1, 'message_id') + blob(2, identifier)) + blob(8, json.dumps(event)))
        frame = b'\x82' + (bytes([len(payload)]) if len(payload) < 126 else b'\x7e' + struct.pack('!H', len(payload))) + payload
        assert self.ws is not None
        self.ws.sendall(frame)

    def close(self):
        if self.ws:
            try:
                self.ws.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.server.shutdown()
        self.server.server_close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = FeishuFixture()
    calls = []

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            calls.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
            body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Feishu reply from Core'}, 'finish_reason': 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    model = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix='nyabot-feishu-') as directory:
        for iteration in range(2):
            sim = Simulator(args.binary.resolve(), directory)
            try:
                deadline = time.monotonic() + 20
                while not sim.request('agent.status', {})['ready']:
                    assert time.monotonic() < deadline
                if not iteration:
                    sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                        'port': str(model.server_port), 'model': 'fixture', 'key': 'synthetic-feishu-model'})
                    saved = sim.request('agent.channels.feishu.save', {'appId': 'cli_fixture', 'secret': 'synthetic-feishu-secret', 'fixturePort': str(fixture.port)})
                    assert saved['secretSet'] and 'synthetic-feishu-secret' not in json.dumps(saved), saved
                else:
                    state = sim.request('agent.channels.feishu.get', {})
                    assert state['appId'] == 'cli_fixture' and state['secretSet'] and not state['running'], state
                fixture.connected.clear()
                sim.request('agent.channels.feishu.start', {})
                assert fixture.connected.wait(15), sim.drain()
                deadline = time.monotonic() + 15
                while not sim.request('agent.channels.feishu.get', {})['connected']:
                    assert time.monotonic() < deadline
                fixture.publish('om_' + str(iteration), 'Hello from Feishu')
                reply = fixture.sent.get(timeout=20)
                assert reply['receive_id'] == 'oc_fixture' and json.loads(reply['content'])['text'] == 'Feishu reply from Core', reply
                fixture.publish('om_' + str(iteration), 'Hello from Feishu')
                assert fixture.sent.get(timeout=15) == reply
                assert len(calls) == iteration + 1
                records = sim.request('agent.runs.list', {})
                assert all(run['channel'] == 'feishu' and run['state'] == 'succeeded' for run in records['runs']), records
                sim.request('agent.channels.feishu.stop', {})
                deadline = time.monotonic() + 15
                while sim.request('agent.channels.feishu.get', {})['running']:
                    assert time.monotonic() < deadline
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
    fixture.close()
    model.shutdown()
    model.server_close()
    assert len(calls) == 2 and fixture.connections == 2 and fixture.acks >= 4
    assert any(message['role'] == 'assistant' for message in calls[-1]['messages'])
    result = {'httpTokenAndSend': True, 'realWebSocketProtobuf': True, 'coreModel': True,
              'durableDedup': True, 'restartHistory': True, 'stop': True,
              'modelCalls': len(calls), 'productionFeishu': False}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_FEISHU_SIM_PASS', json.dumps(result))


if __name__ == '__main__':
    main()
