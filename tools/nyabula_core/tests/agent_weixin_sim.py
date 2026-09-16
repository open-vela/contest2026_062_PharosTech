#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Controlled iLink HTTP login/poll/send -> actual Core Agent integration."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import queue
import tempfile
import threading
import time
from product_records_sim import Simulator


class WeixinFixture:
    def __init__(self):
        self.messages = queue.Queue()
        self.sent = queue.Queue()
        self.polls = 0
        self.login_polls = 0
        self.requests = []
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
                fixture.requests.append(('GET', self.path))
                if self.path.startswith('/ilink/bot/get_bot_qrcode'):
                    fixture.login_polls = 0
                    self.reply({'qrcode': 'fixture-qr', 'qrcode_img_content': 'https://example.invalid/controlled-weixin-login'})
                elif self.path.startswith('/ilink/bot/get_qrcode_status'):
                    fixture.login_polls += 1
                    status = ['wait', 'scaned', 'confirmed'][min(2, fixture.login_polls - 1)]
                    self.reply({'status': status, 'bot_token': 'synthetic-weixin-token', 'baseurl': 'https://127.0.0.1/'})
                else:
                    self.reply({'error': 'unknown endpoint'})

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                fixture.requests.append(('POST', self.path))
                if self.headers.get('Authorization') != 'Bearer synthetic-weixin-token':
                    self.reply({'ret': -14})
                elif self.path == '/ilink/bot/getupdates':
                    fixture.polls += 1
                    try:
                        message = fixture.messages.get(timeout=.3)
                        messages = [message]
                    except queue.Empty:
                        messages = []
                    self.reply({'ret': 0, 'msgs': messages, 'get_updates_buf': str(fixture.polls)})
                elif self.path == '/ilink/bot/sendmessage':
                    fixture.sent.put(request['msg'])
                    self.reply({'ret': 0})
                else:
                    self.reply({'ret': -1})

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.port = self.server.server_port
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = WeixinFixture()
    calls = []

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            calls.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
            body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': 'Weixin reply from real Core'}, 'finish_reason': 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    model = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix='nyabot-weixin-') as directory:
        for iteration in range(2):
            sim = Simulator(args.binary.resolve(), directory)
            try:
                deadline = time.monotonic() + 20
                while not sim.request('agent.status', {})['ready']:
                    assert time.monotonic() < deadline
                if not iteration:
                    sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                        'port': str(model.server_port), 'model': 'fixture', 'key': 'synthetic-weixin-model'})
                    sim.request('agent.channels.weixin.save', {'host': '127.0.0.1', 'port': str(fixture.port), 'token': ''})
                    qr = sim.request('agent.channels.weixin.login', {})
                    assert qr['qrContent'].startswith('https://example.invalid/'), qr
                    for expected in (0, 2, 1):
                        result = sim.request('agent.channels.weixin.login.poll', {})
                        assert result['loginState'] == expected, result
                    assert result['tokenSet'] and 'synthetic-weixin-token' not in json.dumps(result), result
                else:
                    state = sim.request('agent.channels.weixin.get', {})
                    assert state['tokenSet'] and not state['running'], state
                sim.request('agent.channels.weixin.start', {})
                deadline = time.monotonic() + 15
                while not sim.request('agent.channels.weixin.get', {})['connected']:
                    assert time.monotonic() < deadline
                context = 'context-' + str(iteration) + '-' + 'x' * 300
                message = {'from_user_id': 'owner@example.im', 'message_id': 'message-' + str(iteration),
                    'message_type': 1, 'message_state': 2, 'context_token': context,
                    'item_list': [{'type': 1, 'text_item': {'text': 'Hello from Weixin'}}]}
                fixture.messages.put(message)
                reply = fixture.sent.get(timeout=20)
                assert reply['to_user_id'] == message['from_user_id'] and reply['context_token'] == context, reply
                assert reply['item_list'][0]['text_item']['text'] == 'Weixin reply from real Core', reply
                fixture.messages.put({**message, 'context_token': context + '-new'})
                replay = fixture.sent.get(timeout=15)
                assert replay['context_token'] == context + '-new', replay
                assert len(calls) == iteration + 1, len(calls)
                records = sim.request('agent.runs.list', {})
                assert context not in json.dumps(records)
                assert all(run['channel'] == 'weixin' and run['state'] == 'succeeded' for run in records['runs']), records
                sim.request('agent.channels.weixin.stop', {})
                deadline = time.monotonic() + 15
                while sim.request('agent.channels.weixin.get', {})['running']:
                    assert time.monotonic() < deadline
            finally:
                sim.close()
                logs.extend(sim.log)
                (args.output / 'serial.log').write_bytes(logs)
    fixture.close()
    model.shutdown()
    model.server_close()
    assert len(calls) == 2
    assert any(message['role'] == 'assistant' for message in calls[-1]['messages'])
    result = {'qrLoginStates': True, 'realHttpPollingAndSend': True, 'coreModel': True,
              'fullReplyContext': True, 'durableDedup': True, 'restartHistory': True,
              'stop': True, 'modelCalls': len(calls), 'productionWeixin': False}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print('NYABOT_WEIXIN_SIM_PASS', json.dumps(result))


if __name__ == '__main__':
    main()
