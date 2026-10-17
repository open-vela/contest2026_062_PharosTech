/* Production WebUI -> SSH loopback forward -> real NuttX Core integration.
 * Usage: node native-eye-browser.cjs <bundled-node-modules> <output-dir> */
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');
const net=require('node:net');
const {spawn,execFile}=require('node:child_process');
const {promisify}=require('node:util');
const {createRequire}=require('node:module');
const runtime=createRequire(path.join(process.argv[2],'playwright/package.json'));
const {chromium}=runtime('playwright');
const exec=promisify(execFile);
const output=path.resolve(process.argv[3]);
const dist=path.resolve(__dirname,'../dist');
const remote='/root/openvela/core-eye-20260910';
const sessionFile=remote+'/eye-ws-browser-session.json';
// The simulator host is private infrastructure: it comes from the
// environment (NYABULA_SIM_SSH=user@host, NYABULA_SIM_SSH_PORT,
// NYABULA_SIM_SSH_KEY), never from the repository.
if(!process.env.NYABULA_SIM_SSH) { console.error('set NYABULA_SIM_SSH=user@host'); process.exit(2); }
const sshArgs=['-p',process.env.NYABULA_SIM_SSH_PORT||'22',...(process.env.NYABULA_SIM_SSH_KEY?['-i',process.env.NYABULA_SIM_SSH_KEY]:[]),'-o','IdentitiesOnly=yes','-o','ConnectTimeout=10'];
const destination=process.env.NYABULA_SIM_SSH;
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,timeout=15000) {
  const end=Date.now()+timeout;
  while(Date.now()<end) { if(await fn()) return; await delay(50); }
  throw new Error('condition deadline');
}

async function main() {
  fs.mkdirSync(output,{recursive:true});
  const server=http.createServer((req,res)=>{
    let pathname=decodeURIComponent(new URL(req.url,'http://localhost').pathname);
    if(pathname.startsWith('/api/')) {res.statusCode=404;return res.end('{}');}
    let file=path.resolve(dist,'.'+pathname);
    if(!file.startsWith(dist+path.sep)) file=path.join(dist,'index.html');
    if(!fs.existsSync(file) || !fs.statSync(file).isFile()) file=path.join(dist,'index.html');
    const mime={'.js':'text/javascript','.css':'text/css','.html':'text/html','.ttf':'font/ttf','.svg':'image/svg+xml'};
    res.setHeader('Content-Type',mime[path.extname(file)]??'application/octet-stream');
    fs.createReadStream(file).pipe(res);
  });
  await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(5180,'127.0.0.1',resolve);});
  let nativeLog='', fixture,forward,browser;
  const results=[];
  try {
    const command=`cmake --build ${remote}/sim/out -j2 > ${remote}/eye-ws-build.log 2>&1 && timeout --kill-after=5s 240s python3 ${remote}/eye_ws_integration.py ${remote}/sim/out/nuttx --output ${remote}/eye-ws-evidence --browser-session ${sessionFile} --hold-seconds 180`;
    fixture=spawn('ssh',[...sshArgs,destination,command],{windowsHide:true});
    fixture.stdout.on('data',b=>{nativeLog+=b.toString();});
    fixture.stderr.on('data',b=>{nativeLog+=b.toString();});
    await until(()=>{if(fixture.exitCode!==null) throw new Error('native fixture failed: '+nativeLog);return nativeLog.includes('BROWSER_SESSION_READY');},90000);
    const meta=JSON.parse((await exec('ssh',[...sshArgs,destination,`cat ${sessionFile}`],{windowsHide:true})).stdout);
    const reserve=net.createServer();
    await new Promise(r=>reserve.listen(0,'127.0.0.1',r));
    const port=reserve.address().port;
    await new Promise(r=>reserve.close(r));
    let forwardError='';
    forward=spawn('ssh',[...sshArgs,'-v','-o','ConnectionAttempts=3','-o','ExitOnForwardFailure=yes','-N','-L',`127.0.0.1:${port}:127.0.0.1:${meta.port}`,destination],{windowsHide:true});
    forward.stderr.on('data',b=>{forwardError+=b.toString();});
    await until(()=>{
      if(forward.exitCode!==null) throw new Error('SSH forward failed: '+forwardError);
      return forwardError.includes('Local forwarding listening on 127.0.0.1 port');
    },45000);
    browser=await chromium.launch({channel:'chrome',headless:true});
    for(const form of ['desktop','phone']) {
      const context=await browser.newContext(form==='phone'?{viewport:{width:390,height:844},isMobile:true,hasTouch:true}:{viewport:{width:1440,height:1000}});
      const page=await context.newPage();
      const states=[],errors=[],commands=[];
      await page.addInitScript(()=>{
        window.__eyePointers=[];
        for(const type of ['pointerdown','pointermove','pointerup','pointercancel']) window.addEventListener(type,e=>{
          window.__eyePointers.push({type,x:e.clientX,y:e.clientY,primary:e.isPrimary,button:e.button,pointerType:e.pointerType,target:e.target.tagName});
        });
      });
      page.on('pageerror',e=>errors.push(e.message));
      page.on('websocket',ws=>{
        ws.on('framereceived',frame=>{try{const e=JSON.parse(String(frame.payload));if(e.topic==='eye.state') states.push(e.data);}catch{}});
        ws.on('framesent',frame=>{try{const e=JSON.parse(String(frame.payload));if(e.topic==='eyes.gaze') commands.push(e.data);}catch{}});
      });
      await page.goto('http://127.0.0.1:5180/connect');
      await page.getByLabel('设备 IP / 主机名').fill('127.0.0.1');
      await page.getByLabel('端口',{exact:true}).fill(String(port));
      await page.getByLabel(/原生 Core 访问令牌/).fill(meta.token);
      await page.getByRole('button',{name:'连接',exact:true}).click();
      try { await page.getByRole('button',{name:'完整控制'}).click({timeout:20000}); }
      catch(e) {
        await page.screenshot({path:path.join(output,`${form}-connect-failure.png`),fullPage:true});
        fs.writeFileSync(path.join(output,'connect-failure.json'),JSON.stringify({form,errors,states},null,2));
        throw e;
      }
      const toggle=page.getByRole('button',{name:/逗猫棒/});
      await toggle.click();
      await until(()=>toggle.getAttribute('aria-pressed').then(v=>v==='true'));
      const canvas=page.locator('.stage canvas').first();
      const box=await canvas.boundingBox();
      if(!box || box.width<100) throw new Error('missing eye canvas');
      const target={x:box.x+box.width*.78,y:box.y+box.height*.24};
      const before=states.length;
      if(form==='desktop') await page.mouse.move(target.x,target.y);
      else {
        const cdp=await context.newCDPSession(page);
        await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:box.x+box.width*.5,y:box.y+box.height*.5}]});
        await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[target]});
        try {
          await until(()=>states.slice(before).some(s=>s.gaze_active && s.gaze_x>.4 && s.gaze_y<-.3));
        } catch(e) {
          fs.writeFileSync(path.join(output,'touch-failure.json'),JSON.stringify({box,target,commands,states,pointers:await page.evaluate(()=>window.__eyePointers)},null,2));
          await page.screenshot({path:path.join(output,'touch-failure.png'),fullPage:true});
          throw e;
        }
        await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
      }
      await until(()=>states.slice(before).some(s=>s.gaze_active && s.gaze_x>.4 && s.gaze_y<-.3));
      if(!commands.some(c=>c.x>.4 && c.y<-.3)) throw new Error('no real gaze request');
      await delay(250);
      await page.screenshot({path:path.join(output,`${form}.png`),fullPage:true});
      await toggle.click();
      await until(()=>states.at(-1)?.gaze_active===false);
      const persisted=await page.evaluate(()=>Object.values(localStorage));
      if(persisted.some(v=>v.includes(meta.token))) throw new Error('manual token persisted');
      if(errors.length) throw new Error(errors.join('\n'));
      results.push({form,gazeRequests:commands.length,coreStates:states.length,errors});
      await context.close();
    }
    console.log('BROWSER_NATIVE_EYE_PASS '+JSON.stringify(results));
    fs.writeFileSync(path.join(output,'result.json'),JSON.stringify(results,null,2));
  } finally {
    await browser?.close();
    forward?.kill();
    await exec('ssh',[...sshArgs,destination,`rm -f ${sessionFile}`],{windowsHide:true}).catch(()=>{});
    if(fixture) await until(()=>fixture.exitCode!==null,10000).catch(()=>{});
    fs.writeFileSync(path.join(output,'native.log'),nativeLog);
    server.close();
  }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
