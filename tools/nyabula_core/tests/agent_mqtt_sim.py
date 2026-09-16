#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise real MQTT packets, native Agent routing and channel restarts."""
import argparse
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


def packet(kind, body=b''):
    length = len(body)
    encoded = bytearray([kind])
    while True:
        digit, length = length % 128, length // 128
        encoded.append(digit | (128 if length else 0))
        if not length:
            return bytes(encoded) + body


def string(value):
    value = value.encode()
    return struct.pack('!H', len(value)) + value


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError()
        data.extend(part)
    return bytes(data)


class Broker:
    def __init__(self):
        self.server = socket.socket()
        self.server.bind(('127.0.0.1', 0))
        self.port = self.server.getsockname()[1]
        self.server.listen(4)
        self.client = None
        self.lock = threading.Lock()
        self.subscribed = threading.Event()
        self.responses = queue.Queue()
        self.connections = 0
        threading.Thread(target=self.run, daemon=True).start()

    def send(self, data):
        with self.lock:
            assert self.client is not None
            self.client.sendall(data)

    def run(self):
        while True:
            try:
                client, _ = self.server.accept()
            except OSError:
                return
            self.client = client
            self.connections += 1
            try:
                while True:
                    kind = exact(client, 1)[0]
                    size, shift = 0, 0
                    while True:
                        digit = exact(client, 1)[0]
                        size += (digit & 127) << shift
                        if not digit & 128:
                            break
                        shift += 7
                    body = exact(client, size)
                    if kind >> 4 == 1:
                        self.send(packet(0x20, b'\0\0'))
                    elif kind >> 4 == 8:
                        self.send(packet(0x90, body[:2] + b'\1'))
                        self.subscribed.set()
                    elif kind >> 4 == 3:
                        length = struct.unpack('!H', body[:2])[0]
                        offset = length + 2
                        if (kind >> 1) & 3:
                            self.send(packet(0x40, body[offset:offset + 2]))
                            offset += 2
                        self.responses.put(json.loads(body[offset:]))
                    elif kind >> 4 == 12:
                        self.send(packet(0xd0))
            except (OSError, EOFError):
                pass
            finally:
                client.close()
                self.client = None

    def publish(self, value):
        payload = value if isinstance(value, str) else json.dumps(value)
        self.send(packet(0x30, string('nyabot/in') + payload.encode()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    broker = Broker()
    calls = []

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append(request)
            body = json.dumps({'choices': [{'message': {'role': 'assistant',
                'content': 'MQTT response ' + 'x' * 1300}, 'finish_reason': 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix='nyabot-mqtt-') as directory:
        for iteration in range(2):
            sim = Simulator(args.binary.resolve(), directory)
            try:
                deadline = time.monotonic() + 20
                while not sim.request('agent.status', {})['ready']:
                    assert time.monotonic() < deadline
                if not iteration:
                    sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                        'port': str(server.server_port), 'model': 'fixture', 'key': 'synthetic-mqtt-key'})
                    saved = sim.request('agent.channels.mqtt.save', {
                        'broker': f'127.0.0.1:{broker.port}', 'clientId': 'nyabot-test',
                        'topicIn': 'nyabot/in', 'topicOut': 'nyabot/out',
                        'username': 'fixture', 'password': 'synthetic-mqtt-password'})
                    assert saved['passwordSet'] and 'password' not in saved
                else:
                    saved = sim.request('agent.channels.mqtt.get', {})
                    assert saved['passwordSet'] and saved['clientId'] == 'nyabot-test'
                    assert not saved['requested'] and not saved['running']
                for cycle in range(2 if not iteration else 1):
                    broker.subscribed.clear()
                    sim.request('agent.channels.mqtt.start', {})
                    assert broker.subscribed.wait(15), sim.drain()
                    deadline = time.monotonic() + 15
                    while not sim.request('agent.channels.mqtt.get', {})['connected']:
                        assert time.monotonic() < deadline
                    message = {'chat_id': 'test', 'request_id': f'mqtt-{iteration}-{cycle}', 'content': 'hello'}
                    broker.publish(message)
                    reply = broker.responses.get(timeout=20)
                    assert reply['chat_id'] == 'test' and len(reply['content']) > 1300, reply
                    count = len(calls)
                    broker.publish(message)
                    assert broker.responses.get(timeout=15) == reply
                    assert len(calls) == count, 'Duplicate request called model again'
                    broker.publish(reply)
                    broker.publish({'chat_id': 'test', 'content': 'x' * 2049})
                    rejected = broker.responses.get(timeout=15)
                    assert '(-22)' in rejected['content'] and len(calls) == count, rejected
                    if iteration:
                        broker.publish('plain text hello')
                        plain = broker.responses.get(timeout=20)
                        assert plain['chat_id'] == 'default' and plain['content'].startswith('MQTT response')
                    sim.request('agent.channels.mqtt.stop', {})
                    deadline = time.monotonic() + 15
                    while sim.request('agent.channels.mqtt.get', {})['running']:
                        assert time.monotonic() < deadline
                runs = sim.request('agent.runs.list', {})['runs']
                assert all(run['caller'] == 'channel-mqtt' and run['state'] == 'succeeded' for run in runs)
                assert all(run['conversationId'] in ('mqtt_test', 'mqtt_default') for run in runs)
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
        assert len(calls) == 4
        assert any(message['role'] == 'assistant' for message in calls[-2]['messages']), calls[-2]
    server.shutdown()
    server.server_close()
    broker.server.close()
    result = {'realMqttPackets': True, 'nativeAgent': True, 'durableHistory': True,
              'deduplication': True, 'channelRestart': True, 'configPersistence': True,
              'plainText': True, 'invalidMessageReply': True,
              'modelCalls': len(calls), 'brokerConnections': broker.connections}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_MQTT_SIM_PASS', json.dumps(result))


if __name__ == '__main__':
    main()
