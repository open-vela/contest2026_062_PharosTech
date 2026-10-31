#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Native signed authoring, UI actions, failed-upgrade data recovery and restart."""
import argparse
import json
from pathlib import Path
import tempfile
import time
from product_records_sim import Simulator

SOURCE = """
import * as storage from '@nyabula/storage';
import * as ui from '@nyabula/ui';
function render(){
  ui.render({nyaui:1,page:{title:'Invoice fixture'},root:{type:'column',children:[
    {type:'text',id:'total',props:{text:storage.get('total')||'0'}},
    {type:'textField',id:'amount',props:{label:'Amount',value:''}},
    {type:'textField',id:'unit',props:{label:'Unit',value:'GB'}},
    {type:'button',id:'add',props:{text:'Record amount'},on:{tap:{command:'add'}}}
  ]}});
}
export default {
  onStart(){if(unescape(encodeURIComponent('猫🐈')).length!==7)throw Error('UTF-8 byte count');render()},
  onCommand(command,args,values){
    if(command!=='add')throw Error('Unknown command');
    if(values.unit!=='GB')throw Error('Missing browser default');
    const amount=Number(values.amount);
    if(!Number.isFinite(amount)||amount<=0)throw Error('Invalid amount');
    storage.put('total',String(Number(storage.get('total')||'0')+amount));
    render();
  }
};
"""

def total(sim):
    tree = sim.request('ui.tree', {'pluginId': 'agent-fixture'})
    return tree['root']['children'][0]['props']['text']

def ready(sim):
    for _ in range(40):
        state = sim.request('agent.status', {})
        if state['ready']:
            return
        time.sleep(.1)
    raise AssertionError(state)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    logs = bytearray()
    with tempfile.TemporaryDirectory(prefix='ny-author-') as directory:
        root = Path(directory)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            ready(sim)
            policy = sim.request('plugin.author.contract', {})
            assert not policy['enabled'] and policy['sourceLimit'] == 8192
            request = dict(id='agent-fixture', name='Invoice fixture',
                           source=SOURCE, permissions=['ui.page', 'storage.read', 'storage.write', 'core.log'],
                           baseVersion='')
            sim.request('plugin.author.deploy', request, error=-13)
            sim.request('plugin.author.policy.set', {'enabled': True})
            oversized = sim.request('plugin.author.deploy', dict(request, source='x' * 8193), error=-7)
            assert oversized['sourceBytes'] == 8193 and '8192' in oversized['errorText']
            sim.request('plugin.author.deploy', dict(request, id='../escape'), error=-22)
            sim.request('plugin.author.deploy',
                        dict(request, permissions=['ui.page', 'audio.capture']), error=-13)
            first = sim.request('plugin.author.deploy', request)
            assert first['startupVerified'] and first['state'] == 'running', first
            assert total(sim) == '0'
            invalid = sim.request('plugin.author.test', {'id': 'agent-fixture',
                'event': {'componentId': 'add', 'event': 'tap',
                          'values': {'amount': 35, 'sizeUnit': 'GiB'}}}, error=-22)
            assert not invalid['completed'] and 'Unknown input field: sizeUnit' in invalid['errorText']
            assert total(sim) == '0'
            sim.request('plugin.author.test', {'id': 'agent-fixture',
                        'event': {'componentId': 'add', 'event': 'tap', 'values': {'amount': 35}}})
            for _ in range(30):
                if total(sim) == '35':
                    break
                time.sleep(.05)
            assert total(sim) == '35'
            failed_event = sim.request('plugin.author.test', {'id': 'agent-fixture',
                'event': {'componentId': 'add', 'event': 'tap', 'values': {'amount': -1}}}, error=-14)
            assert not failed_event['completed'] and 'Invalid amount' in failed_event['errorText']
            sim.request('plugin.start', {'pluginId': 'agent-fixture'})
            assert total(sim) == '35'
            sim.request('plugin.author.deploy', request, error=-116)
            sim.request('plugin.revoke', {'pluginId': 'agent-fixture', 'permission': 'core.log'})
            failing = "import * as storage from '@nyabula/storage'; export default {onStart(){storage.put('total','999'); throw Error('trial failed')}};"
            sim.request('plugin.author.deploy', dict(request, source=failing,
                        baseVersion=first['version']), error=-14)
            assert total(sim) == '35'
            installed = sim.request('plugin.list', {})['plugins'][0]
            assert next(p for p in installed['permissions'] if p['name'] == 'core.log')['granted'] is False
            current = sim.request('plugin.author.inspect', {'id': 'agent-fixture'})
            assert current['version'] == first['version'] and current['source'] == SOURCE
            next_source = SOURCE.replace('Invoice fixture', 'Invoice fixture v2')
            second = sim.request('plugin.author.deploy', dict(request,
                        name='Invoice fixture v2', source=next_source, baseVersion=first['version']))
            assert second['version'] != first['version'] and total(sim) == '35'
            sim.request('plugin.author.policy.set', {'enabled': False})
            sim.request('plugin.author.deploy', dict(request, baseVersion=second['version']), error=-13)
            assert total(sim) == '35'
            source_path = root / 'apps/agent-fixture/versions' / second['version'] / 'source.js'
            saved = source_path.read_bytes()
            source_path.write_bytes(saved + b'\n// tampered\n')
            sim.request('plugin.author.inspect', {'id': 'agent-fixture'}, error=-13)
            source_path.write_bytes(saved)
            assert sim.request('plugin.author.inspect', {'id': 'agent-fixture'})['source'] == next_source
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
        sim = Simulator(args.binary.resolve(), directory)
        try:
            ready(sim)
            assert not sim.request('plugin.author.policy.get', {})['enabled']
            sim.request('plugin.start', {'pluginId': 'agent-fixture'})
            assert total(sim) == '35'
            current = sim.request('plugin.author.inspect', {'id': 'agent-fixture'})
            assert current['version'] == second['version']
            (args.output / 'result.json').write_text(json.dumps({
                'firstVersion': first['version'], 'secondVersion': second['version'],
                'valueAfterFailureAndRestart': total(sim), 'signatureTamperRejected': True,
                'forbiddenPermissionRejected': True, 'policyDisabledPreserved': True
            }, indent=2))
        finally:
            sim.close()
            logs.extend(sim.log)
            (args.output / 'serial.log').write_bytes(logs)
    print('NATIVE_AUTHOR_SIGNATURE_POLICY_UI_ROLLBACK_STORAGE_RESTART_PASS')

if __name__ == '__main__':
    main()
