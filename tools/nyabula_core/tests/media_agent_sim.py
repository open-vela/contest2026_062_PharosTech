#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify a model starts and stops native playback through Core tools."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
from media_runtime_sim import write_wave
from product_records_sim import Simulator


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    calls, seen = [], []

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            calls.append(request)
            text = next(item['content'] for item in reversed(request['messages']) if item['role'] == 'user')
            results = [item for item in request['messages'] if item['role'] == 'tool']
            if not results:
                name = 'nyabula_music' if text == 'stop' else 'nyabula_read'
                arguments = {'topic': 'music.stop', 'arguments': {}} if text == 'stop' else {'topic': 'music.library'}
            elif text != 'stop' and len(results) == 1:
                library = json.loads(results[-1]['content'])
                assert library['items'][0]['name'] == 'model.wav', library
                name, arguments = 'nyabula_music', {'topic': 'music.play', 'arguments': {'name': 'model.wav'}}
            else:
                seen.append(json.loads(results[-1]['content']))
                name, arguments = None, None
            message = {'role': 'assistant', 'content': 'Device playback updated'} if not name else {
                'role': 'assistant', 'content': None, 'tool_calls': [{'id': 'media-' + str(len(calls)),
                'type': 'function', 'function': {'name': name, 'arguments': json.dumps(arguments)}}]}
            body = json.dumps({'choices': [{'message': message, 'finish_reason': 'tool_calls' if name else 'stop'}]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='nyabot-media-') as directory:
        root = Path(directory)
        (root / 'media').mkdir()
        (root / 'audio-out').mkdir()
        pcm = write_wave(root / 'media/model.wav', 12)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            deadline = time.monotonic() + 20
            while not sim.request('agent.status', {})['ready']:
                assert time.monotonic() < deadline
            sim.request('agent.config.set', {'host': '127.0.0.1', 'path': '/v1/chat/completions',
                'port': str(server.server_port), 'model': 'fixture', 'key': 'synthetic-media-key'})
            for action in ('play', 'stop'):
                run = sim.request('agent.chat', {'requestId': 'media-' + action, 'conversationId': 'media-' + action, 'text': action})
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    current = sim.request('agent.run.get', {'id': run['id']})
                    assert current['state'] != 'waiting_approval', current
                    if current['state'] not in ('queued', 'running'):
                        assert current['state'] == 'succeeded' and current['steps'][-1]['sideEffectApplied'], current
                        break
                else:
                    raise AssertionError(current)
                state = sim.request('music.status', {})
                assert state['state'] == ('playing' if action == 'play' else 'idle'), state
                if action == 'play':
                    sim.drain(.25)
                    output = root / 'audio-out/pcm_test_16000_1_16.pcm'
                    played = output.read_bytes()
                    assert played and played == pcm[:len(played)]
            assert len(calls) == 5 and [item['state'] for item in seen] == ['playing', 'idle'], seen
        finally:
            sim.close()
            (args.output / 'serial.log').write_bytes(sim.log)
            server.shutdown()
            server.server_close()
    print('MEDIA_AGENT_SIM_PASS actualLibrary modelPlay modelStop pcmOutput noRedundantApproval')


if __name__ == '__main__':
    main()
