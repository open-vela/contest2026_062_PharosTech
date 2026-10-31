import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';
import { createSSRApp, h } from 'vue';
import { renderToString } from 'vue/server-renderer';

test('independent cutoffs render both controls and the actual overlap', async () => {
  const server = await createServer({ configFile:false, plugins:[vue()], server:{middlewareMode:true} });
  try {
    const {default:CrossoverControls} = await server.ssrLoadModule('/src/views/services/features/CrossoverControls.vue');
    const render = (high, low, disabled=false) => renderToString(createSSRApp({render:()=>h(CrossoverControls,{satelliteHighpassHz:high,bassLowpassHz:low,disabled})}));
    const overlap = await render(250,500);
    assert.equal((overlap.match(/type="range"/g)||[]).length,2);
    assert.equal((overlap.match(/max="500"/g)||[]).length,2);
    assert.match(overlap,/value="250"/); assert.match(overlap,/value="500"/);
    assert.match(overlap,/板载高通 Hz/); assert.match(overlap,/低音低通 Hz/);
    assert.match(overlap,/重叠频段 · 250–500 Hz/);
    assert.match(await render(250,250),/共同分频点 · 250 Hz/);
    assert.match(await render(500,250),/两路截止点之间有间隔 · 250–500 Hz/);
    assert.equal(((await render(250,500,true)).match(/ disabled/g)||[]).length,2);
  } finally { await server.close(); }
});
