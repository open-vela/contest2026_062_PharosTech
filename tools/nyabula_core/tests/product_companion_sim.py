#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Companion policy is opt-in, persisted, quiet-hours gated and bounded."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
from product_records_sim import Simulator


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    logs=bytearray(); noon=1789300800000
    class Model(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            request=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            prompt=next(row['content'] for row in reversed(request['messages']) if row['role']=='user')
            assert 'Never claim to see the user' in prompt
            body=json.dumps({'choices':[{'message':{'role':'assistant','content':'你好，今天也慢慢来。'},'finish_reason':'stop'}]}).encode()
            self.send_response(200); self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
    server=ThreadingHTTPServer(('127.0.0.1',0),Model)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='nyabula-companion-') as directory:
        sim=Simulator(args.binary.resolve(),directory)
        try:
            sim.request('system.time.set',{'unix_ms':noon})
            state=sim.request('companion.get',{})
            assert not state['enabled'] and state['daily_count']==0 and state['daily_limit']==3
            payload=dict(revision=state['revision'],enabled=True,mode='interactive',quiet_start=720,
                         quiet_end=780,utc_offset_minutes=0,minimum_interval_minutes=30,daily_limit=2)
            state=sim.request('companion.configure',payload)
            assert state['enabled'] and state['quiet_now']
            sim.request('companion.run',{'revision':state['revision']},error=-11)
            sim.request('companion.configure',dict(payload,revision=0),error=-116)
            state=sim.request('companion.configure',dict(payload,revision=state['revision'],quiet_start=1320,quiet_end=480))
            assert not state['quiet_now'] and state['daily_limit']==2
            state=sim.request('companion.run',{'revision':state['revision']})
            sim.request('product.start',{})
            time.sleep(1.3)
            state=sim.request('companion.get',{})
            # No LLM provider is configured in this fixture: fail closed and retry later.
            assert state['daily_count']==0 and state['last_error'] < 0 and state['next_at']>noon
            for _ in range(30):
                agent=sim.request('agent.status',{})
                if agent['ready']: break
                time.sleep(.1)
            assert agent['ready']
            configured=sim.request('agent.config.set', {'host':'127.0.0.1','port':str(server.server_port),
                'path':'/v1/chat/completions','model':'controlled','key':'fixture-only'})
            assert configured['keySet'] and 'fixture-only' not in json.dumps(configured)
            state=sim.request('companion.run',{'revision':state['revision']})
            for _ in range(80):
                time.sleep(.15)
                state=sim.request('companion.get',{})
                notices=sim.request('notification.list',{})
                if notices['items']: break
            assert state['daily_count']==1 and not state['pending_run']
            assert notices['items'][0]['source']=='companion'
            assert notices['items'][0]['title']=='你好，今天也慢慢来。'
            sim.request('product.stop',{})
        finally:
            sim.close(); logs.extend(sim.log); (args.output/'serial.log').write_bytes(logs)
        sim=Simulator(args.binary.resolve(),directory)
        try:
            sim.request('system.time.set',{'unix_ms':noon+10000})
            state=sim.request('companion.get',{})
            assert state['enabled'] and state['mode']=='interactive' and state['daily_limit']==2
            assert state['daily_count']==1 and not state['pending_run']
        finally:
            sim.close(); logs.extend(sim.log); (args.output/'serial.log').write_bytes(logs)
    server.shutdown()
    print('PRODUCT_COMPANION_SIM_PASS opt-in CAS quiet-hours limits model-notice fail-closed restart')


if __name__=='__main__': main()
