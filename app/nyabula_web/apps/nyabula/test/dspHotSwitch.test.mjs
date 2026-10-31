import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const source=readFileSync(new URL('../src/views/services/features/DspControl.vue',import.meta.url),'utf8').replace(/\r\n/g,'\n');
const body=source.match(/async function toggleBass\(\)\{([\s\S]*?)\n\}\nfunction toggleEq/)[1];
function fixture(fail=false,dirtyBefore=false){
  const draft={value:{bassEnabled:false,bassLowpassHz:240}};
  const status={value:{config:{bassEnabled:true}}};
  const liveBusy={value:false},dirty={value:dirtyBefore},error={value:''};
  const requests=[];let cancelled=0;
  const session={request:async(topic,data)=>{requests.push({topic,data});if(fail)throw Error('device busy');return {config:{bassEnabled:false}};}};
  const accept=s=>{status.value=s;};
  const run=new Function('draft','status','liveBusy','dirty','error','session','accept','cancelPreview','describeError',`return async function(){${body}}`)(draft,status,liveBusy,dirty,error,session,accept,()=>cancelled++,e=>e.message);
  return {run,draft,status,liveBusy,dirty,error,requests,get cancelled(){return cancelled;}};
}
test('hot switch sends only the mode, preserving unsaved calibration',async()=>{
  const f=fixture(false,true);await f.run();
  assert.deepEqual(f.requests,[{topic:'dsp.config',data:{bassEnabled:false,persist:false}}]);
  assert.equal(f.draft.value.bassLowpassHz,240);assert.equal(f.dirty.value,true);
  assert.equal(f.liveBusy.value,false);assert.equal(f.cancelled,1);
});
test('rejected switch restores displayed mode and retains dirty state',async()=>{
  const f=fixture(true,false);await f.run();assert.equal(f.draft.value.bassEnabled,true);
  assert.equal(f.error.value,'device busy');assert.equal(f.dirty.value,false);assert.equal(f.liveBusy.value,false);
});
test('playing does not disable the switch; an in-flight request does',async()=>{
  const tag=source.match(/<MdSwitch v-model="draft\.bassEnabled"[^>]+>/)[0];
  assert.doesNotMatch(tag,/status\.running/);assert.match(tag,/liveBusy/);assert.match(tag,/toggleBass/);
  const f=fixture();f.liveBusy.value=true;await f.run();assert.equal(f.requests.length,0);
});
