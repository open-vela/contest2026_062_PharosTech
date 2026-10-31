#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent skill bodies, UTF-8 paging, edit deactivation and legacy import."""
import argparse
import json
from pathlib import Path
import sqlite3
import tempfile
from product_records_sim import Simulator

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    body = '猫' * 2000 + '\nFollow actual tool results.'
    with tempfile.TemporaryDirectory(prefix='ny-skills-') as directory:
        sim = Simulator(args.binary.resolve(), directory)
        try:
            listed = sim.request('agent.skills.list', {})
            assert listed['contentLimit'] == 8192
            for i in range(5):
                listed = sim.request('agent.skills.save', {'id': 'large-' + str(i), 'title': 'Large skill ' + str(i),
                    'content': body, 'revision': listed['revision']})
                assert all('content' not in row and 'contentVersion' not in row for row in listed['items'])
            listed = sim.request('agent.skills.enable', {'id': 'large-0', 'enabled': True, 'revision': listed['revision']})
            sim.request('agent.skills.read', {'id': 'large-0', 'offset': 1}, error=-22)
            first = sim.request('agent.skills.read', {'id': 'large-0'})
            assert first['totalBytes'] == len(body.encode()) and first['nextOffset'] <= 4096
            second = sim.request('agent.skills.read', {'id': 'large-0', 'offset': first['nextOffset']})
            assert first['content'] + second['content'] == body and 'nextOffset' not in second
            sim.request('agent.skills.save', {'id': 'large-0', 'title': 'Stale', 'content': 'wrong', 'revision': 0}, error=-116)
            assert sim.request('agent.skills.get', {'id': 'large-0'})['content'] == body
            listed = sim.request('agent.skills.save', {'id': 'large-0', 'title': 'Updated',
                'content': body + '\nupdated', 'revision': listed['revision']})
            sim.request('agent.skills.read', {'id': 'large-0'}, error=-13)
        finally:
            sim.close()
            logs.extend(sim.log)
        database = Path(directory) / 'nyabula-product.sqlite'
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT count(*) FROM state WHERE k LIKE 'skills/body/%'").fetchone()[0] == 5
            raw = db.execute("SELECT v FROM state WHERE k='product/v1/skills'").fetchone()[0]
            value = json.loads(raw)
            value['value']['items'].append({'id': 'legacy', 'title': 'Legacy', 'source': 'custom',
                                           'enabled': True, 'content': 'Legacy inline text.'})
            value['revision'] += 1
            db.execute("UPDATE state SET v=? WHERE k='product/v1/skills'", (json.dumps(value).encode(),))
        sim = Simulator(args.binary.resolve(), directory)
        try:
            assert sim.request('agent.skills.get', {'id': 'large-4'})['content'] == body
            legacy = sim.request('agent.skills.read', {'id': 'legacy'})
            assert legacy['content'] == 'Legacy inline text.'
            listed = sim.request('agent.skills.save', {'id': 'legacy', 'title': 'Migrated',
                'content': body, 'revision': legacy['revision']})
            assert sim.request('agent.skills.get', {'id': 'legacy'})['content'] == body
            assert not next(item for item in listed['items'] if item['id'] == 'legacy')['enabled']
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
    print('SKILLS_BODY_QUOTA_UTF8_PAGING_CAS_DEACTIVATION_LEGACY_RESTART_PASS')

if __name__ == '__main__': main()
