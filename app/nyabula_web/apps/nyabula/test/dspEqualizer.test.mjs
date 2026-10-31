import test from 'node:test';
import assert from 'node:assert/strict';
import {flatBands,bandResponse,eqResponse,eqPeak,eqPreset,validBands,presetMasterDb,expandLegacyBands} from '../src/lib/dspEqualizer.ts';
test('flat, off and exact peaking center',()=>{
  const bands=flatBands(); assert.equal(eqResponse(bands,1000),0);
  bands[0].gainDb=6; assert.ok(Math.abs(bandResponse(bands[0],bands[0].frequencyHz)-6)<1e-8);
  bands[0].enabled=false;assert.equal(eqResponse(bands,80),0);
});
test('shelves approach the specified edge gain and have half gain at center',()=>{
  for(const type of ['lowShelf','highShelf']) {
    const b={frequencyHz:1000,q:.707,gainDb:6,enabled:true,type};
    assert.ok(Math.abs(bandResponse(b,1000)-3)<1e-8);
    assert.ok(Math.abs(bandResponse(b,type==='lowShelf'?20:20000)-6)<.01);
    assert.ok(Math.abs(bandResponse(b,type==='lowShelf'?20000:20))<.01);
  }
});
test('presets remain valid; narrow overlapping bands count toward headroom',()=>{
  for(const p of ['flat','warm','voice','soft','rich','balanced','mild'])assert.ok(validBands(eqPreset(p)));
  const bands=flatBands().map(b=>({...b,frequencyHz:1337,q:10,gainDb:12}));
  assert.ok(Math.abs(eqPeak(bands)-192)<1e-6);
  bands[0].q=0;assert.equal(validBands(bands),false);
});

test('16 bands retain old four-band positions and preset headroom',()=>{
 const old=[80,300,1200,6000].map((frequencyHz,i)=>({frequencyHz,q:1,gainDb:i,enabled:true,type:'peak'}));
 const upgraded=expandLegacyBands(old);
 assert.equal(upgraded.length,16);[2,5,8,12].forEach((p,i)=>assert.deepEqual(upgraded[p],old[i]));
 for(const id of ['rich','balanced','mild']) {const b=eqPreset(id);assert.equal(b.length,16);assert.ok(eqPeak(b)+presetMasterDb(b)<=-1);console.log(id,'masterDb',presetMasterDb(b),'peakDb',eqPeak(b).toFixed(2));}
 assert.ok(eqResponse(eqPreset('rich'),80)>eqResponse(eqPreset('balanced'),80)+2);
 assert.ok(eqResponse(eqPreset('mild'),8000)<eqResponse(eqPreset('balanced'),8000)-3);
});
