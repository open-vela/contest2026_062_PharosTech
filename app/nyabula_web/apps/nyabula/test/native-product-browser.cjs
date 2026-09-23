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
const sessionFile=remote+'/product-browser-session.json';
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
    const command=`timeout --kill-after=5s 300s python3 ${remote}/product_web_sim.py ${remote}/sim/out/nuttx --output ${remote}/product-web-browser-evidence --browser-session ${sessionFile} --hold-seconds 240`;
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
      const page=await context.newPage(), errors=[], replies=[], connections=[];
      page.on('pageerror', e=>errors.push(e.message));
      page.on('websocket', ws=>{
        connections.push({event:'open',at:Date.now()});
        ws.on('close',()=>connections.push({event:'close',at:Date.now()}));
        ws.on('socketerror',error=>connections.push({event:'error',at:Date.now(),error}));
        ws.on('framereceived', frame=>{try{replies.push(JSON.parse(String(frame.payload)));}catch{}});
      });
      async function feature(label) {
        if(form==='phone' && await page.locator('.bar-btn').isVisible()) await page.locator('.bar-btn').click();
        await page.getByRole('button',{name:'功能',exact:true}).first().click();
        await page.locator('.fc-id').filter({has:page.locator('.fc-title',{hasText:new RegExp('^'+label+'$')})}).click();
        await page.locator('.feature').waitFor();
        await until(()=>page.locator('.fc').count().then(n=>n===0));
      }
      try {
        await page.goto('http://127.0.0.1:5180/connect');
        await page.getByLabel('设备 IP / 主机名').fill('127.0.0.1');
        await page.getByLabel('端口',{exact:true}).fill(String(port));
        await page.getByLabel(/原生 Core 访问令牌/).fill(meta.token);
        await page.getByRole('button',{name:'连接',exact:true}).click();
        await page.getByRole('button',{name:'完整控制'}).waitFor({timeout:20000});
        await page.getByText('眼睛服务未启动',{exact:true}).waitFor();
        if(await page.locator('.stage canvas').count()) throw new Error('Headless Core must not render a fabricated eye');
        await feature('记忆');
        const text='Browser Core memory '+form;
        await page.getByLabel('内容',{exact:true}).fill(text);
        await page.getByRole('button',{name:'记住',exact:true}).click();
        await page.locator('.mcard-text').filter({hasText:text}).waitFor();
        await feature('秒表');
        const newStopwatch = page.getByRole('button',{name:'新建秒表',exact:true});
        if(await newStopwatch.count()) await newStopwatch.click();
        await page.getByRole('button',{name:'开始',exact:true}).click();
        await page.getByRole('button',{name:'分段',exact:true}).waitFor();
        await delay(500);
        await page.getByRole('button',{name:'分段',exact:true}).click();
        await page.locator('.lap-row').waitFor();
        await feature('记忆');
        await page.locator('.mcard-text').filter({hasText:text}).waitFor();
        await feature('秒表');
        if (await page.locator('.frame .nk-header').count() !== 1) throw new Error('duplicate feature header');
        await page.getByRole('button',{name:'暂停',exact:true}).click();
        await page.getByRole('button',{name:'继续',exact:true}).waitFor();
        await page.locator('.lap-row').waitFor();
        await page.screenshot({path:path.join(output,form+'.png'),fullPage:true});
        const featureLabels=['音乐','音频路由','均衡器','低音炮','字幕','倒计时','秒表','闹钟','日历','睡眠定时','通话','在场','陪伴','天气','简报','任务','记忆','健康','电量','网络','设备','系统','隐私','身份','家居','休眠','配对'];
        for(const label of featureLabels) {
          await feature(label);
          await page.locator('.frame .nk-header-title').filter({hasText:new RegExp('^'+label+'$')}).waitFor();
          if(await page.locator('.frame .nk-header').count() !== 1) throw new Error('duplicate header: '+label);
        }
        if(form==='phone' && await page.locator('.bar-btn').isVisible()) await page.locator('.bar-btn').click();
        await page.getByRole('button',{name:'Nyabot',exact:true}).first().click();
        await page.getByText('还没有可用模型',{exact:true}).waitFor();
        await page.getByLabel('消息',{exact:true}).fill('Unsent draft '+form);
        if(await page.locator('.nyabot-workspace .composer').getByRole('button',{name:'发送',exact:true}).isEnabled()) throw new Error('unconfigured agent allowed send');
        await page.getByRole('button',{name:'任务',exact:true}).click();
        await page.getByText('暂无执行记录',{exact:true}).waitFor();
        await page.getByRole('button',{name:'聊天',exact:true}).click();
        if(await page.getByLabel('消息',{exact:true}).inputValue() !== 'Unsent draft '+form) throw new Error('draft lost during tab navigation');
        await page.screenshot({path:path.join(output,form+'-nyabot.png'),fullPage:true});
        await page.getByRole('button',{name:'能力与连接',exact:true}).click();
        await page.getByText('MCP · 连接外部服务',{exact:true}).waitFor();
        await page.getByText('MCP · 允许外部访问',{exact:true}).waitFor();
        await page.getByRole('button',{name:'配置',exact:true}).click();
        await page.getByText(/当前原生连接仅可查看配置/).waitFor();
        if(await page.getByLabel('替换密钥',{exact:true}).isEnabled()) throw new Error('plaintext UI enabled credential input');
        await page.screenshot({path:path.join(output,form+'-config.png'),fullPage:true});
        await page.getByRole('button',{name:'设置',exact:true}).first().click();
        await page.getByRole('heading',{name:'设备设置',exact:true}).waitFor();
        await page.getByRole('link',{name:/客户端设置/}).waitFor();
        await page.screenshot({path:path.join(output,form+'-settings.png'),fullPage:true});
        const operations=['memory.create','timer.create','timer.lap','timer.pause'];
        for(const topic of operations) if(!replies.some(e=>e.topic===topic&&e.type==='res')) throw new Error('No successful Core response for '+topic);
        if(errors.length) throw new Error(errors.join('\n'));
        results.push({form,operations,headersChecked:featureLabels.length,nyabotUnconfigured:true,draftRetained:true,settingsScope:true,errors});
      } catch(e) {
        await page.screenshot({path:path.join(output,form+'-failure.png'),fullPage:true});
        fs.writeFileSync(path.join(output,form+'-failure.json'),JSON.stringify({errors,replies,connections,forwardError},null,2));
        throw e;
      } finally { await context.close(); }
    }
    console.log('BROWSER_NATIVE_PRODUCT_PASS '+JSON.stringify(results));
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
