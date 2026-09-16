#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify signed OpenClaw node handshake and real Core command RPCs."""
import argparse
import base64
import ctypes
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


class GatewayFixture:
    def __init__(self):
        self.ws = None
        self.lock = threading.Lock()
        self.connected = threading.Event()
        self.replies = {}
        self.connects = []
        self.errors = []
        self.counter = 0
        self.reject_next = False
        fixture = self
        sodium = ctypes.CDLL('libsodium.so.23')
        sodium.crypto_sign_verify_detached.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p]
        sodium.crypto_sign_verify_detached.restype = ctypes.c_int

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                key = self.headers['Sec-WebSocket-Key']
                accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                self.send_response(101)
                self.send_header('Upgrade', 'websocket')
                self.send_header('Connection', 'Upgrade')
                self.send_header('Sec-WebSocket-Accept', accept)
                self.end_headers()
                self.wfile.flush()
                fixture.ws = self.connection
                stamp = int(time.time() * 1000)
                nonce = 'controlled-nonce-' + str(stamp)
                fixture.send({'type': 'event', 'event': 'connect.challenge', 'payload': {'nonce': nonce, 'ts': stamp}})
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
                        mask = self.rfile.read(4) if header[1] & 128 else None
                        raw = self.rfile.read(length)
                        if mask:
                            raw = bytes(value ^ mask[i % 4] for i, value in enumerate(raw))
                        if header[0] & 15 != 1:
                            continue
                        frame = json.loads(raw)
                        if frame['method'] == 'connect':
                            params = frame['params']
                            fixture.connects.append(params)
                            device = params['device']
                            public = base64.urlsafe_b64decode(device['publicKey'] + '===')
                            signature = base64.urlsafe_b64decode(device['signature'] + '===')
                            assert params['client']['id'] == 'node-host' and params['role'] == 'node' and params['client']['mode'] == 'node'
                            assert params['minProtocol'] == 3 and params['maxProtocol'] == 4
                            assert device['id'] == hashlib.sha256(public).hexdigest()
                            assert device['nonce'] == nonce and device['signedAt'] == stamp
                            signed = '|'.join(['v3', device['id'], 'node-host', 'node', 'node', '', str(stamp), params['auth']['token'], nonce, 'openvela', 'robot']).encode()
                            assert sodium.crypto_sign_verify_detached(ctypes.create_string_buffer(signature), ctypes.create_string_buffer(signed), len(signed), ctypes.create_string_buffer(public)) == 0
                            assert 'nyabula.chat' in params['commands'] and 'system.run' not in params['commands']
                            assert len(params['commands']) == 7
                            if fixture.reject_next:
                                fixture.reject_next = False
                                fixture.send({'type': 'res', 'id': frame['id'], 'ok': False,
                                    'error': {'code': 'NOT_PAIRED', 'message': 'Pairing required', 'details': {'code': 'PAIRING_REQUIRED'}}})
                                fixture.connected.set()
                                continue
                            fixture.send({'type': 'res', 'id': frame['id'], 'ok': True, 'payload': {
                                'type': 'hello-ok', 'protocol': 4, 'server': {'version': 'controlled', 'connId': 'fixture'},
                                'features': {'methods': ['node.invoke.result'], 'events': ['node.invoke.request']},
                                'snapshot': {}, 'policy': {'maxPayload': 8192, 'maxBufferedBytes': 8192, 'tickIntervalMs': 15000},
                                'auth': {'role': 'node', 'scopes': [], 'deviceToken': 'synthetic-device-token'}}})
                            fixture.connected.set()
                        elif frame['method'] == 'node.invoke.result':
                            payload = frame['params']
                            fixture.replies[payload['id']].put(payload)
                            fixture.send({'type': 'res', 'id': frame['id'], 'ok': True, 'payload': {}})
                except (OSError, EOFError):
                    pass
                except Exception as error:
                    fixture.errors.append(repr(error))
                finally:
                    if fixture.ws is self.connection:
                        fixture.ws = None
                    self.close_connection = True

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.port = self.server.server_port
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def send(self, value):
        raw = json.dumps(value).encode()
        header = b'\x81' + (bytes([len(raw)]) if len(raw) < 126 else b'\x7e' + struct.pack('!H', len(raw)))
        with self.lock:
            assert self.ws is not None
            self.ws.sendall(header + raw)

    def invoke(self, command, arguments):
        self.counter += 1
        identifier = 'invoke-' + str(self.counter)
        answer = queue.Queue()
        self.replies[identifier] = answer
        self.send({'type': 'event', 'event': 'node.invoke.request', 'payload': {
            'id': identifier, 'nodeId': 'fixture-node', 'command': command, 'paramsJSON': json.dumps(arguments)}})
        response = answer.get(timeout=15)
        return response

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
    gateway = GatewayFixture()
    calls = []
    slow_started = threading.Event()

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            calls.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
            if calls[-1]['messages'][-1]['content'] == 'slow':
                slow_started.set()
                time.sleep(1)
            body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Reply through OpenClaw Core'}, 'finish_reason': 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    model = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix='nyabot-node-') as directory:
        for iteration in range(2):
            sim = Simulator(args.binary.resolve(), directory)
            try:
                deadline = time.monotonic() + 20
                while not sim.request('agent.status', {})['ready']:
                    assert time.monotonic() < deadline
                sim.request('agent.node.stop', {})
                if not iteration:
                    sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                        'port': str(model.server_port), 'model': 'fixture', 'key': 'synthetic-node-model'})
                    sim.request('agent.node.save', {'host': '127.0.0.1', 'port': str(gateway.port), 'tls': False, 'token': 'synthetic-gateway-token'})
                else:
                    saved = sim.request('agent.node.get', {})
                    assert saved['deviceTokenSet'] and saved['deviceId'] and not saved['running'], saved
                    sim.request('agent.node.save', {'host': saved['host'], 'port': saved['port'], 'tls': False, 'token': ''})
                gateway.connected.clear()
                sim.request('agent.node.start', {})
                assert gateway.connected.wait(20), gateway.errors
                deadline = time.monotonic() + 15
                while True:
                    current = sim.request('agent.node.get', {})
                    if current['ready'] and current['deviceTokenSet']:
                        break
                    assert time.monotonic() < deadline, current
                assert current['protocol'] == 4 and 'synthetic-device-token' not in json.dumps(current)
                reply = gateway.invoke('nyabula.status', {})
                assert reply['ok'] and json.loads(reply['payloadJSON'])['configured'], reply
                read = gateway.invoke('nyabula.read', {'topic': 'memory.list'})
                assert read['ok'] and json.loads(read['payloadJSON'])['items'] == [], read
                submitted = gateway.invoke('nyabula.chat', {'requestId': 'node-' + str(iteration), 'conversationId': 'node-chat', 'text': 'hello'})
                assert submitted['ok'], submitted
                run = json.loads(submitted['payloadJSON'])
                deadline = time.monotonic() + 20
                while True:
                    response = gateway.invoke('nyabula.run.get', {'id': run['id']})
                    assert response['ok'], response
                    finished = json.loads(response['payloadJSON'])
                    if finished['state'] not in ('queued', 'running'):
                        assert finished['state'] == 'succeeded' and finished['reply'] == 'Reply through OpenClaw Core', finished
                        break
                    assert time.monotonic() < deadline
                rejected = gateway.invoke('system.run', {'command': 'echo unsupported'})
                assert not rejected['ok'], rejected
                if not iteration:
                    pending = gateway.invoke('nyabula.chat', {'requestId': 'node-cancel', 'conversationId': 'node-cancel', 'text': 'slow'})
                    assert pending['ok'] and slow_started.wait(10), pending
                    pending_id = json.loads(pending['payloadJSON'])['id']
                    cancel = gateway.invoke('nyabula.cancel', {'id': pending_id})
                    assert cancel['ok'], cancel
                    deadline = time.monotonic() + 15
                    while True:
                        outcome = gateway.invoke('nyabula.run.get', {'id': pending_id})
                        current_run = json.loads(outcome['payloadJSON'])
                        if current_run['state'] == 'cancelled':
                            break
                        assert time.monotonic() < deadline, current_run
                sim.request('agent.node.stop', {})
                deadline = time.monotonic() + 15
                while sim.request('agent.node.get', {})['running']:
                    assert time.monotonic() < deadline
                if not iteration:
                    gateway.reject_next = True
                    gateway.connected.clear()
                    sim.request('agent.node.start', {})
                    assert gateway.connected.wait(20), gateway.errors
                    deadline = time.monotonic() + 15
                    while True:
                        denied = sim.request('agent.node.get', {})
                        if denied['gatewayError'] == 'PAIRING_REQUIRED':
                            assert not denied['ready'], denied
                            break
                        assert time.monotonic() < deadline, denied
                    sim.request('agent.node.stop', {})
                    deadline = time.monotonic() + 15
                    while sim.request('agent.node.get', {})['running']:
                        assert time.monotonic() < deadline
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
    gateway.close()
    model.shutdown()
    model.server_close()
    assert len(calls) == 3 and not gateway.errors, gateway.errors
    assert len(gateway.connects) == 3
    assert gateway.connects[0]['device']['id'] == gateway.connects[-1]['device']['id']
    assert gateway.connects[-1]['auth']['token'] == 'synthetic-device-token'
    result = {'signedDeviceIdentity': True, 'protocol4': True, 'coreCommands': True,
              'asyncChatRun': True, 'restartIdentityAndToken': True, 'stop': True,
              'remoteCancel': True, 'pairingRequiredState': True,
              'modelCalls': len(calls), 'productionGateway': False}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_NODE_SIM_PASS', json.dumps(result))


if __name__ == '__main__':
    main()
