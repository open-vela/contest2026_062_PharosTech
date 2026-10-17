/****************************************************************************
 * app/nyabula_core/ny_web_eyeprobe_page.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __NYABULA_CORE_NY_WEB_EYEPROBE_PAGE_H
#define __NYABULA_CORE_NY_WEB_EYEPROBE_PAGE_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The page behind /eyeprobe: the two eye screens and nothing else.  It is
 * part of the firmware and not of the panel files so that it is there on a
 * board whose data partition is empty, and it needs nothing but itself: no
 * script, style or font is fetched.  The decoder is the one the format in
 * nyabula_eye_mirror.h implies; the panel has the same in TypeScript
 * (app/nyabula_web/apps/nyabula/src/lib/eyeMirror.ts).
 *
 * Only single quotes inside, so that it reads as what it is.
 */

#define NY_WEB_EYEPROBE_PAGE \
  "<!doctype html>\n" \
  "<html lang='zh-CN'><head><meta charset='utf-8'>\n" \
  "<meta name='viewport' content='width=device-width,initial-scale=1'>\n" \
  "<title>Nyabula 眼睛屏幕</title>\n" \
  "<style>\n" \
  "html,body{height:100%;margin:0;background:#000;color:#9aa;" \
  "font:14px/1.6 system-ui,sans-serif}\n" \
  "body{display:flex;flex-direction:column;align-items:center;" \
  "justify-content:center;gap:18px}\n" \
  ".e{display:flex;gap:4vw;align-items:center;justify-content:center;" \
  "width:100%}\n" \
  "canvas{width:min(44vw,76vh);aspect-ratio:1;border-radius:50%;" \
  "background:#050505;box-shadow:0 0 0 2px #1d1d1d}\n" \
  "p{margin:0;text-align:center}a{color:#6fd}\n" \
  "</style></head><body>\n" \
  "<div class='e'><canvas id='l' width='360' height='360'></canvas>" \
  "<canvas id='r' width='360' height='360'></canvas></div>\n" \
  "<p><span id='s'>正在连接…</span> · <a href='/'>打开控制面板</a></p>\n" \
  "<script>\n" \
  "(function(){\n" \
  "var cv=[document.getElementById('l'),document.getElementById('r')];\n" \
  "var cx=[cv[0].getContext('2d'),cv[1].getContext('2d')];\n" \
  "var st=document.getElementById('s'),im=[null,null],dirty=[0,0];\n" \
  "var ctl=null,run=0,half=0,timer=0;\n" \
  "function say(t){st.textContent=t}\n" \
  "function put(p,o,v){var r=v>>11,g=v>>5&63,b=v&31;\n" \
  " p[o]=r<<3|r>>2;p[o+1]=g<<2|g>>4;p[o+2]=b<<3|b>>2}\n" \
  "function apply(e,key,w,h,d){\n" \
  " var i,m=im[e];\n" \
  " if(key||!m||m.width!==w||m.height!==h){\n" \
  "  if(!key)throw new Error('difference without a page');\n" \
  "  cv[e].width=w;cv[e].height=h;m=im[e]=cx[e].createImageData(w,h);\n" \
  "  for(i=0;i<m.data.length;i+=4){m.data[i]=m.data[i+1]=m.data[i+2]=0;" \
  "m.data[i+3]=255}}\n" \
  " var p=m.data,a=0,x=0,n=w*h;\n" \
  " while(a<d.length){\n" \
  "  var op=d[a++],c=(op&63)+1,k=op>>6,v;\n" \
  "  if((op&63)===63){c=d[a]|d[a+1]<<8;a+=2}\n" \
  "  if(!c||x+c>n)throw new Error('run past the page');\n" \
  "  if(k===1){v=d[a]|d[a+1]<<8;a+=2;for(i=0;i<c;i++)put(p,(x+i)*4,v)}\n" \
  "  else if(k===2){if(a+c*2>d.length)throw new Error('short literal');\n" \
  "   for(i=0;i<c;i++,a+=2)put(p,(x+i)*4,d[a]|d[a+1]<<8)}\n" \
  "  else if(k!==0)throw new Error('unknown operation');\n" \
  "  x+=c}\n" \
  " if(x!==n)throw new Error('page not covered');\n" \
  " dirty[e]=1}\n" \
  "function draw(){for(var e=0;e<2;e++)if(dirty[e]&&im[e]){dirty[e]=0;" \
  "cx[e].putImageData(im[e],0,0)}requestAnimationFrame(draw)}\n" \
  "function later(t){say(t);clearTimeout(timer);var mine=run;\n" \
  " timer=setTimeout(function(){if(mine===run)start()},2000)}\n" \
  "function stop(){run++;clearTimeout(timer);if(ctl)ctl.abort();ctl=null}\n" \
  "function start(){\n" \
  " stop();var mine=run,got=0,frames=0,since=Date.now();\n" \
  " var buf=new Uint8Array(0);im=[null,null];ctl=new AbortController();\n" \
  " say('正在连接…');\n" \
  " fetch('/eyeprobe/stream?scale='+(half?2:1),{signal:ctl.signal," \
  "cache:'no-store'}).then(function(res){\n" \
  "  if(mine!==run)return;\n" \
  "  if(res.status===503)return later('已有两个页面在观看，稍后自动重试');\n" \
  "  if(!res.ok||!res.body)return later('设备返回 '+res.status);\n" \
  "  var rd=res.body.getReader();\n" \
  "  function pump(){return rd.read().then(function(x){\n" \
  "   if(mine!==run)return;\n" \
  "   if(x.done){if(!got&&!half){half=1;return start()}\n" \
  "    return later('画面中断，正在重连…')}\n" \
  "   var j=new Uint8Array(buf.length+x.value.length);\n" \
  "   j.set(buf);j.set(x.value,buf.length);\n" \
  "   var a=0,dv=new DataView(j.buffer);\n" \
  "   while(j.length-a>=16){\n" \
  "    if(j[a]!==78||j[a+1]!==69||j[a+2]!==77||j[a+3]!==49)" \
  "throw new Error('not a record');\n" \
  "    var len=dv.getUint32(a+12,true);if(j.length-a-16<len)break;\n" \
  "    if(j[a+4]===0&&j[a+5]<2){apply(j[a+5],j[a+6]&1," \
  "dv.getUint16(a+8,true),dv.getUint16(a+10,true)," \
  "j.subarray(a+16,a+16+len));got++;frames++}\n" \
  "    a+=16+len}\n" \
  "   buf=j.slice(a);\n" \
  "   var now=Date.now();if(now-since>=1000){\n" \
  "    say(Math.round(frames/2/((now-since)/1000))+' 帧/秒'+" \
  "(half?' · 流畅':''));frames=0;since=now}\n" \
  "   return pump()})}\n" \
  "  return pump()\n" \
  " }).catch(function(){if(mine===run)later('连接中断，正在重连…')})}\n" \
  "document.addEventListener('visibilitychange',function(){\n" \
  " if(document.hidden){stop();say('已暂停')}else start()});\n" \
  "requestAnimationFrame(draw);start();\n" \
  "})();\n" \
  "</script></body></html>\n"

#endif /* __NYABULA_CORE_NY_WEB_EYEPROBE_PAGE_H */
