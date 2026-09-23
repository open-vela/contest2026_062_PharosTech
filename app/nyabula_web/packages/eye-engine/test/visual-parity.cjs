/* Pixel parity against the original demo. No reference source is changed.
 * NOTE: the expression eye (iris base, fibers, pupil clamp, overlays, lids,
 * gaze range) now follows the device renderer nyabula_eye_renderer_lvgl.c
 * instead of the demo, and everything is drawn at the device's 178/180 eye
 * radius. Every case is therefore EXPECTED to differ from the demo; use this
 * script to eyeball the differences, not as a pass/fail gate.
 * Usage: node visual-parity.cjs <bundled-node-modules> <output-directory> */
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { createRequire } = require('node:module');
const workspace = path.resolve(__dirname, '../../../..');
const runtime = createRequire(path.join(process.argv[2], 'playwright/package.json'));
const { chromium } = runtime('playwright');
const sharp = runtime('sharp');
const { build } = require(path.join(workspace, 'Web/node_modules/.pnpm/node_modules/esbuild'));
const output = path.resolve(process.argv[3]);
const demo = fs.readFileSync(path.join(workspace, '工具/cat_eyes_demo.html'), 'utf8');

async function main() {
  fs.mkdirSync(output, { recursive: true });
  const bundle = await build({ entryPoints: [path.join(__dirname, '../src/engine.ts')], bundle: true,
    format: 'iife', globalName: 'Nyabula', write: false });
  const css = demo.match(/<style>([\s\S]*?)<\/style>/)[1];
  const harness = `<style>${css} #stage {width:100vw;height:100vh}</style><canvas id="stage"></canvas><script src="/engine.js"></script>
    <script>window.engine = new Nyabula.EyeEngine(document.querySelector('canvas'), {background:'#000', attachPointer:false});
    Promise.all([document.fonts.load('32px Noto Sans CJK SC','中文标题'), document.fonts.load('600 24px Noto Sans CJK SC','中文正文')]).then(() => { engine.start(); window.__ready=true; });</script>`;
  const server = http.createServer((req, res) => {
    if (req.url === '/reference') return res.end(demo);
    if (req.url === '/engine') return res.end(harness);
    if (req.url === '/engine.js') { res.setHeader('Content-Type', 'text/javascript'); return res.end(bundle.outputFiles[0].contents); }
    const assets = { '/icons/material_icon_paths.js':'icons/material_icon_paths.js' };
    if (assets[req.url]) return res.end(fs.readFileSync(path.join(workspace, '工具', assets[req.url])));
    res.statusCode=404; res.end();
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({ headless: true, channel: 'chrome' });
  const results = [];
  try {
    const modes = ['idle','curious','happy','processing','star','heart','sleepy','sleep','angry','sad','surprise','dizzy','derp'];
    const scenes = ['music','timer','weather','battery','alarm','call','task','stopwatch','calendar','sleep-timer','network','audio','eq','caption','briefing','privacy','identity','memory','devices','system','health','presence','companion','home','subwoofer'];
    const cases = [...modes.map(mode=>({name:mode,mode})), ...scenes.flatMap(scene=>['full','minimal'].map(style=>({name:`${scene}-${style}`,scene,style}))),
      {name:'dark-odd-gaze',mode:'idle',light:0,odd:true,gaze:[.7,-.5]},
      {name:'bright-gaze',mode:'idle',light:100,gaze:[-.7,.5]},
      {name:'blink',mode:'idle',blink:true}];
    for (const test of cases) {
      const mode = test.name;
      const contexts = await Promise.all([0,1].map(() => browser.newContext({viewport:{width:800,height:480},deviceScaleFactor:1})));
      try {
        for (const context of contexts) await context.addInitScript(() => {
          let now=0, seed=12345, frames=[];
          performance.now=()=>now;
          Date.now=()=>1700000000000+now;
          Math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/4294967296);
          window.requestAnimationFrame=cb=>(frames.push(cb),frames.length);
          window.cancelAnimationFrame=()=>{};
          window.__advance=t=>{now=t; const callbacks=frames; frames=[]; callbacks.forEach(cb=>cb(now));};
        });
        const pages = await Promise.all(contexts.map(c=>c.newPage()));
        await Promise.all(pages.map((p,i)=>p.goto(base+(i?'/engine':'/reference'))));
        await Promise.all(pages.map(p=>p.waitForFunction(()=>window.__ready)));
        await pages[0].evaluate(t=>{
          if(t.mode) document.querySelector(`[data-mode="${t.mode}"]`).click();
          else { document.querySelector(`[data-scene-style="${t.style}"]`).click(); document.querySelector(`[data-scene="${t.scene}"]`).click(); }
          if(t.light !== undefined) { const el=document.querySelector('#light');el.value=t.light;el.oninput(); }
          if(t.odd) document.querySelector('[data-c="odd"]').click();
          if(t.gaze) document.querySelector('canvas').dispatchEvent(new PointerEvent('pointerdown',{clientX:400+t.gaze[0]*400,clientY:480*.46+t.gaze[1]*240}));
          if(t.blink) document.querySelector('#blinkBtn').click();
        }, test);
        await pages[1].evaluate(t=>{
          if(t.mode) window.engine.setMode(t.mode); else window.engine.showScene(t.scene,t.style);
          if(t.light !== undefined) window.engine.setLight(t.light/100);
          if(t.odd) window.engine.setIris('#3bb7ff','#ffb830');
          if(t.gaze) window.engine.lookAt(...t.gaze);
          if(t.blink) window.engine.blink();
        }, test);
        for (let frame=0;frame<=180;frame++) {
          await Promise.all(pages.map(p=>p.evaluate(t=>window.__advance(t),frame*1000/60)));
          if (!(test.blink ? [5,8,12,15,60,180] : [15,60,180]).includes(frame)) continue;
          const pngs=await Promise.all(pages.map(p=>p.evaluate(()=>document.querySelector('canvas').toDataURL().split(',')[1]).then(s=>Buffer.from(s,'base64'))));
          const raw=await Promise.all(pngs.map(p=>sharp(p).ensureAlpha().raw().toBuffer()));
          if (raw[0].length !== raw[1].length) throw new Error('Canvas dimensions differ');
          let changed=0, maxDelta=0;
          for(let i=0;i<raw[0].length;i+=4) {
            let delta=0; for(let c=0;c<4;c++) delta=Math.max(delta,Math.abs(raw[0][i+c]-raw[1][i+c]));
            if(delta) changed++; maxDelta=Math.max(maxDelta,delta);
          }
          results.push({mode,frame,changed,maxDelta});
          if(changed || frame===60) {
            fs.writeFileSync(path.join(output,`${mode}-${frame}-reference.png`),pngs[0]);
            fs.writeFileSync(path.join(output,`${mode}-${frame}-client.png`),pngs[1]);
          }
        }
      } finally { await Promise.all(contexts.map(c=>c.close())); }
    }
  } finally { await browser.close(); server.close(); }
  fs.writeFileSync(path.join(output,'results.json'),JSON.stringify(results,null,2));
  console.log(JSON.stringify(results));
  if(results.some(r=>r.changed)) process.exitCode=1;
}
main().catch(e=>{console.error(e);process.exitCode=1;});
