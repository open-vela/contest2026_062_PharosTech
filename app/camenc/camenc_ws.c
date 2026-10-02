/****************************************************************************
 * app/camenc/camenc_ws.c
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

/****************************************************************************
 * A WebSocket server, in as much of the protocol as a one-way stream needs.
 *
 * The protocol has two halves.  The opening handshake is an HTTP request with
 * a few headers and a reply that proves the server understood them; after
 * that the connection stops being HTTP and becomes a sequence of frames.
 *
 * Only what a video stream uses is implemented:
 *
 *   - the handshake, with the key and the digest it is answered with;
 *   - text and binary frames, outbound, unmasked (a server never masks);
 *   - close and ping frames, inbound, because a browser sends both;
 *   - per-frame payload lengths of 7, 16 and 64 bits, so a large frame is
 *     not silently truncated.
 *
 * What is not implemented is the parts a one-way stream never reaches:
 * fragmentation of outbound frames, extensions, subprotocols, and masking of
 * anything the server sends.  Each of those is refused rather than ignored
 * where refusing is what the protocol says to do.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <crypto/sha1.h>

#include "camenc_ws.h"

/* This server polls: it calls accept() between frames and expects to be told
 * EAGAIN when nobody is trying to connect.  On NuttX that only happens with
 * CONFIG_NET_TCPBACKLOG, and with it off accept() does not merely behave
 * differently -- it waits, forever, on the first call, and the application
 * stops where it stands.
 *
 * The reason is in the two halves of the stack that look after a connection
 * arriving at a listening socket.  tcp_accept_connection() hands the
 * connection to accept() only if some task is blocked in accept() at that
 * moment; the branch that puts it on a backlog instead -- so that a task
 * which is busy, as a video encoder is busy, can pick it up later -- is
 * inside #ifdef CONFIG_NET_TCPBACKLOG.  And the branch in accept() that
 * returns EAGAIN instead of waiting is inside the same #ifdef.  Without the
 * option there is no backlog for a connection to wait in and no way to ask
 * whether one is waiting, so the two cannot be combined into a server.
 *
 * Which is worth a build error rather than a log that stops in the middle of
 * a line: a connection that arrives while no task is blocked in accept() is
 * dropped and the peer sees a refused or timed-out connection, which looks
 * like a network fault and is not one.
 */

#if defined(__NuttX__) && !defined(CONFIG_NET_TCPBACKLOG)
#error "camenc_ws polls for connections; enable CONFIG_NET_TCPBACKLOG"
#endif

/* The same assumption about the send side, and the one that is harder to
 * notice when it is wrong.
 *
 * This server queues a frame and expects send() to take what it can and give
 * back EAGAIN for the rest; ws_flush() is written around that and resends
 * from where it stopped.  NuttX only behaves that way when TCP write
 * buffering is on.  With it off, sends go inline and wait for the
 * acknowledgement -- and, unlike the receive path, the send path does not
 * consult the non-blocking flag at all.  tcp_send_unbuffered.c contains no
 * reference to O_NONBLOCK or to _SF_NONBLOCK anywhere, so every send() in
 * ws_flush() blocks until the data is acknowledged however the socket is
 * set.  The wait has no bound either: the default send timeout is zero,
 * which _SO_TIMEOUT() turns into UINT_MAX.
 *
 * The cost of getting this wrong is not a hang, which would be obvious.  It
 * is a frame rate that sags when the picture moves: bigger frames mean more
 * round trips, and the round trips are inside the loop that takes frames from
 * the camera.  With write buffering off, this server took the camera from
 * 62.5 fps to under 40 on a scene with motion in it, while the encoder itself
 * never used more than 1.5 ms of the 16 ms frame period.
 *
 * So it is a build error rather than a measurement: the failure mode is a
 * plausible-looking performance problem that no amount of reading the
 * application's own code explains.
 */

#if defined(__NuttX__) && defined(CONFIG_NET_TCP) && \
    !defined(CONFIG_NET_TCP_WRITE_BUFFERS)
#error \
    "camenc_ws needs non-blocking sends; enable CONFIG_NET_TCP_WRITE_BUFFERS"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The magic string from RFC 6455 section 4.2.2.  The accept key is the
 * SHA-1 of the client's key followed by this, base64-encoded; it exists so
 * that a reply cannot be forged by something that merely echoes a header.
 */

#define CAMENC_WS_GUID     "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define CAMENC_WS_OP_CONT  0x0
#define CAMENC_WS_OP_TEXT  0x1
#define CAMENC_WS_OP_BIN   0x2
#define CAMENC_WS_OP_CLOSE 0x8
#define CAMENC_WS_OP_PING  0x9
#define CAMENC_WS_OP_PONG  0xa

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The page a browser is given, and the whole of the client side.
 *
 * It is a MediaSource fed from a WebSocket, which is the shortest way to
 * play a fragmented MP4 in a browser: append what arrives, and remove what
 * has been played so the buffer does not grow without bound.  A live stream
 * has no duration, so nothing here seeks or ends.
 *
 * The codec string is filled in from the stream rather than written here,
 * because a MediaSource refuses data whose codec string does not match it.
 */

/* The page, in two pieces, with the codec string's place between them.
 *
 * There is no third piece and no buffer holding the whole of it.  The reply
 * used to be composed into one -- `char body[sizeof(g_page) + slack]`, sized
 * from the page so that the number could not drift -- and that is a stack
 * frame the size of the page.  The page grew, the frame grew with it, and it
 * passed the 8192-byte task stack and faulted on the first request: a crash
 * in the server, on the board, from an edit to some CSS.
 *
 * The mistake was not the size.  It was copying a constant in order to
 * change eleven bytes in the middle of it.  The page already lives in
 * read-only memory; the only thing needed from it in a buffer is the codec
 * string, which is a pointer and a length that are already there.  So the
 * three parts are queued in order into the buffer that is going to carry
 * them anyway, and nothing anywhere is the size of the page.
 *
 * That also settles two things the composition kept having to work around:
 * the length in the reply is exact rather than the length of a buffer that
 * was large enough, and the page is no longer a printf format string -- so
 * a per cent sign in it means a per cent sign, and does not have to be
 * written as two.
 */

/* The page's own look, and the two decisions in it worth writing down.
 *
 * The button carries an icon rather than a word because it opens the panel
 * and not the 3A controls: the panel is where controls live, and the 3A ones
 * are only the first of them.  A label naming the contents would be wrong the
 * first time anything else is put in there.
 *
 * The diagnostics sit along the bottom rather than across the top.  They are
 * a readout rather than a control, they are wide rather than tall, and along
 * the top they would compete with the picture for the eye -- which is
 * backwards, because the picture is the thing being looked at.
 *
 * The three custom widget styles are here rather than in a stylesheet
 * because there is no stylesheet: the page is one string in this file, so
 * that serving it is a write() and there is nothing else to keep in step.
 */

static const char g_page_head[] =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\">"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">\n"
    "<title>camenc</title></head>\n"
    "<style>\n"
    ":root{\n"
    "  "
    "--bg:#0f0f13;--panel:#16161c;--line:#26262f;--fg:#d4d4dd;--dim:#82828f;\n"
    "  --accent:#7ccfff;--track:#2c2c36;--chip:#1b1b22;\n"
    "  --sans:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;\n"
    "  --mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;\n"
    "  --dbg:84px;--side:327px;\n"
    "}\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;overflow:hidden;background:var(--bg);color:var(--fg);"
    "font-family:var(--sans);font-size:13px;line-height:1.5}\n"
    "#b{position:fixed;top:8px;left:8px;z-index:12;width:34px;height:34px;"
    "padding:0;display:flex;align-items:center;justify-content:center;"
    "border:1px solid var(--line);border-radius:9px;background:var(--chip);"
    "color:var(--dim);cursor:pointer}\n"
    "#b:hover{color:var(--fg);border-color:#3a3a47}\n"
    "#b:active{transform:scale(.95)}\n"
    "#p{position:fixed;top:0;bottom:0;left:0;width:var(--side);"
    "z-index:8;background:var(--panel);border-right:1px solid var(--line);"
    "padding:52px 14px 14px;overflow:auto}\n"
    "#p.off{display:none}\n"
    "#p h3{margin:22px 0 10px;padding-top:16px;"
    "border-top:1px solid var(--line);font-size:11px;font-weight:600;"
    "letter-spacing:.09em;text-transform:uppercase;color:var(--dim)}\n"
    "#p h3:first-of-type{margin-top:0;padding-top:0;border-top:0}\n"
    "#mo{padding:7px 9px;margin:0 0 4px;border:1px solid var(--line);"
    "border-radius:7px;background:var(--chip);color:var(--dim);font-size:11px;"
    "font-family:var(--mono)}\n"
    ".r{display:flex;align-items:center;gap:10px;margin-bottom:9px}\n"
    ".r span{flex:0 0 66px;color:var(--dim);font-size:12px}\n"
    ".r b{flex:0 0 46px;text-align:right;font-family:var(--mono);"
    "font-size:12px;font-variant-numeric:tabular-nums}\n"
    ".n{margin:-4px 0 0 76px;color:var(--dim);font-size:11px;"
    "font-family:var(--mono)}\n"
    ".foot{margin-top:20px;color:#5f5f6b;font-size:10px;line-height:1.6}\n"
    "input[type=range]{-webkit-appearance:none;appearance:none;flex:1 1 auto;"
    "min-width:0;height:14px;margin:0;background:transparent;cursor:pointer}\n"
    "input[type=range]::-webkit-slider-runnable-track{height:3px;"
    "border-radius:2px;background:var(--track)}\n"
    "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;"
    "width:12px;height:12px;margin-top:-4.5px;border:0;border-radius:50%;"
    "background:var(--accent)}\n"
    "input[type=range]::-moz-range-track{height:3px;border-radius:2px;"
    "background:var(--track)}\n"
    "input[type=range]::-moz-range-thumb{width:12px;height:12px;border:0;"
    "border-radius:50%;background:var(--accent)}\n"
    "input[type=range]:focus{outline:none}\n"
    "input[type=range]:focus-visible::-webkit-slider-thumb{"
    "box-shadow:0 0 0 3px #7ccfff44}\n"
    "input[type=checkbox]{-webkit-appearance:none;appearance:none;"
    "flex:0 0 auto;width:16px;height:16px;margin:0;border:1px solid #3b3b47;"
    "border-radius:5px;background:#1c1c23;cursor:pointer}\n"
    "input[type=checkbox]:checked{background-color:var(--accent);"
    "border-color:var(--accent);background-image:url(\"data:image/svg+xml,"
    "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'%3E%3Cpath "
    "d='M3.5 8.6l3 3 6-7' fill='none' stroke='%230f0f13' stroke-width='2.6' "
    "stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E\");"
    "background-repeat:no-repeat;background-position:center;"
    "background-size:11px}\n"
    "input:disabled{opacity:.4;cursor:not-allowed}\n"
    "#s{position:fixed;left:var(--side);right:0;bottom:0;height:var(--dbg);"
    "z-index:9;background:#0b0b0e;border-top:1px solid var(--line);"
    "padding:6px 11px;overflow:auto;white-space:pre-wrap;"
    "font-family:var(--mono);font-size:11px;color:var(--dim);"
    "transition:left .18s ease}\n"
    "#s.full{left:0}\n"
    "#w{position:fixed;top:0;right:0;bottom:var(--dbg);left:var(--side);"
    "display:flex;align-items:center;justify-content:center;padding:10px;"
    "transition:left .18s ease}\n"
    "#w.full{left:0}\n"
    "#v{display:block;max-width:100%;max-height:100%;border-radius:6px;"
    "background:#000}\n"
    "</style></head>\n"
    "<body>\n"
    "<button id=b onclick=toggle() aria-label=\"toggle sidebar\" "
    "aria-expanded=\"true\" title=\"toggle sidebar\">\n"
    "<svg viewBox=\"0 0 16 16\" width=\"16\" height=\"16\" fill=\"none\" "
    "stroke=\"currentColor\" stroke-width=\"1.6\" stroke-linecap=\"round\">"
    "<path d=\"M2 4h12M2 8h12M2 12h12\"/></svg></button>\n"
    "<div id=p>\n"
    "<div id=mo>-</div>\n"
    "<h3>exposure and gain</h3>\n"
    "<div class=r><span>exposure</span>"
    "<input id=es type=range><b id=ev>-</b></div>\n"
    "<div class=r><span>gain</span>"
    "<input id=gs type=range><b id=gv>-</b></div>\n"
    "<div class=r><span>automatic</span>"
    "<input id=ea type=checkbox checked></div>\n"
    "<div class=n id=lv>level -</div>\n"
    "<h3>white balance</h3>\n"
    "<div class=r><span>red</span>"
    "<input id=wr type=range><b id=wrv>-</b></div>\n"
    "<div class=r><span>green</span>"
    "<input id=wg type=range><b id=wgv>-</b></div>\n"
    "<div class=r><span>blue</span>"
    "<input id=wb type=range><b id=wbv>-</b></div>\n"
    "<div class=r><span>automatic</span>"
    "<input id=wa type=checkbox checked></div>\n"
    "<div class=n id=wv>-</div>\n"
    "<div class=foot>gains are 8-bit fixed point, 256 = 1.0x, and green is "
    "the reference the other two are compared with.</div>\n"
    "</div>\n"
    "<div id=w><video id=v autoplay muted playsinline></video></div>\n"
    "<div id=s>connecting...</div>\n"
    "<script>\n"
    "const v=document.getElementById('v'),s=document.getElementById('s');\n"
    "let sb=null,q=[],rx=0,msgs=0,err='',note='';\n"
    "let ws=null,st={},have3a=false,show=true;\n"
    "const HOLD={};\n"
    "const ms=new MediaSource();\n"
    "v.src=URL.createObjectURL(ms);\n"
    "/* How far behind the newest data the picture is allowed to fall before\n"
    "   the playhead is brought forward. Half a second keeps the picture\n"
    "   close to live while leaving enough in hand not to stutter. */\n"
    "const MAXLAG=0.6;\n"
    "function say(){\n"
    "  const b=sb?sb.buffered:null;\n"
    "  let br='-',lag='-';\n"
    "  if(b&&b.length){\n"
    "    const e=b.end(b.length-1);\n"
    "    br=b.start(0).toFixed(2)+'..'+e.toFixed(2)+' ('+b.length+')';\n"
    "    lag=(e-v.currentTime).toFixed(2)+'s'+\n"
    "      (e-v.currentTime>MAXLAG?' LATE':'');\n"
    "  }\n"
    "  s.textContent='msgs='+msgs+' rx='+rx+'B q='+q.length+\n"
    "    ' lag='+lag+'\\n'+\n"
    "    'sb='+(sb?sb.readyState:'none')+' upd='+(sb?sb.updating:'-')+\n"
    "    ' ms='+ms.readyState+' buffered='+br+'\\n'+\n"
    "    't='+v.currentTime.toFixed(2)+' vw='+v.videoWidth+'x'+\n"
    "    v.videoHeight+' rs='+v.readyState+' paused='+v.paused+'\\n'+\n"
    "    'verr='+(v.error?v.error.code+':'+v.error.message:'-')+\n"
    "    ' err='+(err||'-')+(note?' note='+note:'');\n"
    "}\n"
    "function pump(){\n"
    "  if(!sb||sb.updating||!q.length)return;\n"
    "  try{\n"
    "    const b=sb.buffered;\n"
    "    if(b.length){\n"
    "      const end=b.end(b.length-1);\n"
    "      /* A live stream does not start at zero for a client that joined\n"
    "         late: the first fragment it was given carries the timestamp of\n"
    "         the frame it was, which is however long the stream had been\n"
    "         running. The player, meanwhile, sits at zero -- where there is\n"
    "         no data and so nothing to show, which is a black picture that\n"
    "         looks like a broken stream and is not one. Put it where the\n"
    "         data is, which is what any live player has to do. */\n"
    "      if(v.currentTime<b.start(0)){\n"
    "        v.currentTime=b.start(0);\n"
    "        note='seek to '+b.start(0).toFixed(2);\n"
    "      }\n"
    "      /* And it does not wait. A browser plays at exactly real time, so\n"
    "         anything that makes it stall -- a slow append, a busy machine,\n"
    "         a burst of frames -- is never made up: the picture simply runs\n"
    "         later and later, and stays there. Bringing the playhead "
    "forward\n"
    "         when the backlog grows is the difference between a stream that\n"
    "         ends up a second late for ever and one that is about as late "
    "as\n"
    "         it was when it started. */\n"
    "      else if(end-v.currentTime>MAXLAG){\n"
    "        const behind=end-v.currentTime;\n"
    "        v.currentTime=end-MAXLAG/2;\n"
    "        note='caught up from '+behind.toFixed(2)+'s';\n"
    "      }\n"
    "      v.play().catch(e=>{note='play:'+e.name;});\n"
    "      if(v.currentTime-b.start(0)>4){\n"
    "        note='removing';\n"
    "        sb.remove(0,v.currentTime-1);\n"
    "        return;\n"
    "      }\n"
    "    }\n"
    "    const f=q.shift();\n"
    "    sb.appendBuffer(f);\n"
    "    note='appended '+f.length+'B';\n"
    "  }catch(e){err=e.name+': '+e.message;}\n"
    "  say();\n"
    "}\n"
    "ms.addEventListener('sourceopen',()=>{\n"
    "  try{\n"
    "    sb=ms.addSourceBuffer('video/mp4; codecs=\"";

static const char g_page_tail[] =
    "\"');\n"

    "  }catch(e){err='addSourceBuffer: '+e.name+': "
    "'+e.message;say();return;}\n"
    "  sb.mode='segments';\n"
    "  sb.addEventListener('updateend',()=>{note='updateend';pump();});\n"
    "  sb.addEventListener('error',()=>{err='sourcebuffer error';say();});\n"
    "  sb.addEventListener('abort',()=>{err='sourcebuffer abort';say();});\n"
    "  /* Assigned, not declared: the control panel below the fold of this\n"
    "     script sends on the same socket, so it has to be reachable from\n"
    "     outside this handler. */\n"
    "  ws=new WebSocket('ws://'+location.host+'/stream');\n"
    "  ws.binaryType='arraybuffer';\n"
    "  ws.onopen=()=>{note='ws open';say();};\n"
    "  /* Two kinds of message come back on this one socket: the video, as\n"
    "     binary, and the settings, as text. They have to be told apart here\n"
    "     or a status line is fed to the video decoder as a segment, which\n"
    "     is the kind of mistake that looks like a broken stream. */\n"
    "  ws.onmessage=e=>{\n"
    "    if(typeof e.data==='string'){ontext(e.data);return;}\n"
    "    q.push(new Uint8Array(e.data));\n"
    "    rx+=e.data.byteLength;msgs++;say();pump();};\n"
    "  ws.onerror=()=>{err='ws error';say();};\n"
    "  ws.onclose=e=>{err='ws closed code='+e.code;say();};\n"
    "});\n"
    "ms.addEventListener('sourceclose',()=>{err='media source "
    "closed';say();});\n"
    "setInterval(say,500);\n"
    "/* The 3A controls.\n"
    "\n"
    "   The server sends a status line whenever the settings move, and the\n"
    "   sliders follow it: a slider nobody is touching is a display of what\n"
    "   the loop is doing, not a copy of what was last sent to it. That is\n"
    "   what makes an automatic loop visible rather than mysterious.\n"
    "\n"
    "   The one that is being dragged is left alone, because a value being\n"
    "   held should not be pulled out from under the finger by a status that\n"
    "   was computed before the drag. It catches up when the pointer is\n"
    "   released.\n"
    "\n"
    "   Moving a slider switches that loop to manual, which is what a user\n"
    "   means by moving it, and is the difference between a control and a\n"
    "   display: nothing is worse than a slider that springs back. */\n"
    "const $=i=>document.getElementById(i);\n"
    "function send(m){if(ws&&ws.readyState===1)ws.send(m);}\n"
    "/* One place where a command is named.  These names and the names the\n"
    "   board accepts are two halves of one protocol, and a script can only\n"
    "   check them against each other if each is written down once. */\n"
    "/* Slider, command and status field, in one table: a gain's name in the\n"
    "   report is the name that sets it, so the two cannot drift apart. The\n"
    "   mapping is a table rather than a function because a script can read\n"
    "   a table to check the page and the board still agree. */\n"
    "const CMD={es:'e',gs:'g',wr:'kr',wg:'kg',wb:'kb'};\n"
    "function cmd(n,v){send(n+' '+v);}\n"
    "function toggle(){show=!show;$('p').classList.toggle('off',!show);\n"
    "  /* The picture and the diagnostics both begin where the panel ends, "
    "so\n"
    "     that the panel runs the whole height of the window and those two\n"
    "     share the space beside it.  They move together, which is why one\n"
    "     class drives both. */\n"
    "  $('w').classList.toggle('full',!show);\n"
    "  $('s').classList.toggle('full',!show);\n"
    "  /* The button says what it does, and now says whether it is open.\n"
    "     A toggle whose state is only visible in the thing it toggles is\n"
    "     invisible to anything reading the page rather than looking at it. "
    "*/\n"
    "  $('b').setAttribute('aria-expanded',show);}\n"
    "/* The three white balance gains are the numbers the demosaicer is\n"
    "   handed, 256 being unity, and there is deliberately no colour\n"
    "   temperature in front of them. A kelvin would have to come from a\n"
    "   calibration of this sensor against a known lamp; camenc_3a.h says\n"
    "   at length why what was here before instead was a number that could\n"
    "   not be derived from the gains and so could only ever report what\n"
    "   was last typed into it. */\n"
    "const WBS=['wr','wg','wb'];\n"
    "function ontext(t){\n"
    "  if(t.slice(0,3)!=='3a ')return;\n"
    "  const o={};\n"
    "  for(const f of t.slice(3).split(' ')){\n"
    "    const i=f.indexOf('=');\n"
    "    if(i>0)o[f.slice(0,i)]=+f.slice(i+1);\n"
    "  }\n"
    "  st=o;have3a=true;\n"
    "  /* A status line means the board has a control plane, whatever the\n"
    "     panel said before it arrived. The lookup below may have given up\n"
    "     waiting and disabled everything. */\n"
    "  SL.concat(['ea','wa']).forEach(i=>{$(i).disabled=false;});\n"
    "  draw();\n"
    "}\n"
    "const STN=['moving','settled','settled, out of range','by hand'];\n"
    "function draw(){\n"
    "  if(!have3a)return;\n"
    "  const ae=st.ae===1,aw=st.aw===1;\n"
    "  "
    "if(!HOLD.es){$('es').min=st.emin;$('es').max=st.emax;$('es').value=st.e;}"
    "\n"
    "  "
    "if(!HOLD.gs){$('gs').min=st.gmin;$('gs').max=st.gmax;$('gs').value=st.g;}"
    "\n"
    "  /* The gain sliders, bounded by what the driver will accept rather\n"
    "     than by what the automatic loop keeps to: those are different\n"
    "     things, and the narrower of the two is a policy this page has no\n"
    "     business enforcing. */\n"
    "  WBS.forEach((id,k)=>{const el=$(id);\n"
    "    el.min=st.wbmin;el.max=st.wbmax;\n"
    "    if(!HOLD[id])el.value=[st.kr,st.kg,st.kb][k];});\n"
    "  $('ea').checked=ae;$('wa').checked=aw;\n"
    "  /* The sliders stay live while a loop is automatic, and that is the\n"
    "     point of them: they show what the loop has decided, and grabbing\n"
    "     one is how a user says \"not this, this\". Disabling them while\n"
    "     automatic would leave the checkbox as the only way to take a loop\n"
    "     over, which is the thing the slider is for. */\n"
    "  $('ev').textContent=st.e;$('gv').textContent=st.g;\n"
    "  $('wrv').textContent=st.kr;$('wgv').textContent=st.kg;\n"
    "  $('wbv').textContent=st.kb;\n"
    "  /* What the three gains amount to, as ratios against green, which is\n"
    "     the reference the other two are there to be compared with. It is\n"
    "     arithmetic on the numbers beside it rather than a model of a\n"
    "     lamp, so it cannot disagree with them. */\n"
    "  $('wv').textContent=st.kg?'r/g '+(st.kr/st.kg).toFixed(3)+\n"
    "    '   b/g '+(st.kb/st.kg).toFixed(3):'-';\n"
    "  $('lv').textContent='level '+st.lv+' / '+st.tg;\n"
    "  $('mo').textContent=(ae?'auto':'manual')+' exposure, '+\n"
    "    (aw?'auto':'manual')+' white balance, '+\n"
    "    (STN[st.st]||'-');\n"
    "}\n"
    "const SL=['es','gs'].concat(WBS);\n"
    "SL.forEach(id=>{\n"
    "  const el=$(id);\n"
    "  el.oninput=()=>{\n"
    "    /* The value is echoed into the state as well as sent, so that the\n"
    "       redraw below shows what was just asked for rather than what the\n"
    "       board last reported.  Without it a slider moved by the keyboard\n"
    "       -- which is an input with no pointer pressed -- would be written\n"
    "       back to the old value on the next line. */\n"
    "    const v=+el.value;\n"
    "    if(id==='es'||id==='gs'){st.ae=0;}else{st.aw=0;}\n"
    "    st[CMD[id]]=v;\n"
    "    cmd(CMD[id],v);\n"
    "    draw();\n"
    "  };\n"
    "  el.onpointerdown=()=>{HOLD[id]=1;};\n"
    "  ['pointerup','pointercancel','blur'].forEach(e=>\n"
    "    el.addEventListener(e,()=>{HOLD[id]=0;draw();}));\n"
    "});\n"
    "$('ea').onchange=()=>{st.ae=$('ea').checked?1:0;cmd('ae',st.ae);draw();};"
    "\n"
    "$('wa').onchange=()=>{st.aw=$('wa').checked?1:0;cmd('aw',st.aw);draw();};"
    "\n"
    "/* A board whose capture driver has no 3A control plane never sends a\n"
    "   status line, and the panel must say so rather than sit there looking\n"
    "   like a control that does nothing. */\n"
    "setTimeout(()=>{\n"
    "  if(have3a)return;\n"
    "  $('mo').textContent='no 3A control plane on this board';\n"
    "  SL.concat(['ea','wa']).forEach(i=>{$(i).disabled=true;});\n"
    "},2500);\n"
    "</script></body></html>\n";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ws_base64
 *
 * Description:
 *   Encode a digest for the handshake reply.
 *
 *   The C library's base64 support writes to a stream for logging rather
 *   than to a buffer, so the twenty lines are written out here instead of
 *   pulling in a stream to write into a string.
 *
 ****************************************************************************/

static size_t ws_base64(FAR const uint8_t *in, size_t len, FAR char *out)
{
  static const char tab[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  size_t i;
  size_t n = 0;

  for (i = 0; i + 2 < len; i += 3)
    {
      uint32_t v =
          ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];

      out[n++] = tab[(v >> 18) & 0x3f];
      out[n++] = tab[(v >> 12) & 0x3f];
      out[n++] = tab[(v >> 6) & 0x3f];
      out[n++] = tab[v & 0x3f];
    }

  if (i < len)
    {
      uint32_t v = (uint32_t)in[i] << 16;

      out[n++] = tab[(v >> 18) & 0x3f];

      if (i + 1 < len)
        {
          v |= (uint32_t)in[i + 1] << 8;
          out[n++] = tab[(v >> 12) & 0x3f];
          out[n++] = tab[(v >> 6) & 0x3f];
        }
      else
        {
          out[n++] = tab[(v >> 12) & 0x3f];
          out[n++] = '=';
        }

      out[n++] = '=';
    }

  out[n] = '\0';
  return n;
}

/****************************************************************************
 * Name: ws_head_len
 *
 * Description:
 *   Where a request's headers end: the offset just past the blank line that
 *   terminates them, or zero if it is not all there yet.
 *
 *   The two uses of this differ.  Before dispatching, zero means "wait for
 *   more".  After a request has been answered, the same offset says how much
 *   of the buffer was that request, so that what follows it -- frames, on a
 *   connection that has just been upgraded -- is not read as though the
 *   request were still in front of them.
 *
 ****************************************************************************/

static size_t ws_head_len(FAR const uint8_t *buf, size_t len)
{
  static const char end[] = "\r\n\r\n";
  size_t i;

  for (i = 0; i + sizeof(end) - 1 <= len; i++)
    {
      if (memcmp(buf + i, end, sizeof(end) - 1) == 0)
        {
          return i + sizeof(end) - 1;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: ws_header
 *
 * Description:
 *   Find a header in a request, case-insensitively, and return its value
 *   with leading spaces removed.
 *
 *   The request is NUL-terminated by the caller, at the end of what has
 *   arrived, which is what makes walking it line by line safe.  Without
 *   that the walk runs on past the request into whatever the buffer held
 *   before, and a header that is not in this request can be found in the
 *   last one.
 *
 ****************************************************************************/

static FAR const char *ws_header(FAR const char *req, FAR const char *name)
{
  size_t n = strlen(name);
  FAR const char *p = req;

  while ((p = strchr(p, '\n')) != NULL)
    {
      p++;

      if (strncasecmp(p, name, n) == 0 && p[n] == ':')
        {
          p += n + 1;
          while (*p == ' ' || *p == '\t')
            {
              p++;
            }

          return p;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: ws_close_client
 *
 * Description:
 *   Release a client slot: close its socket and forget the connection, but
 *   keep the transmit buffer, which belongs to the slot rather than to any
 *   one connection.
 *
 ****************************************************************************/

static void ws_close_client(FAR struct camenc_ws_s *ws,
                            FAR struct camenc_ws_client_s *c);

/****************************************************************************
 * Name: ws_queue
 *
 * Description:
 *   Append bytes to a client's transmit buffer, dropping the client if they
 *   do not fit.
 *
 *   Dropping rather than waiting is the decision this whole module turns on.
 *   Waiting would mean the capture loop stops taking frames whenever one
 *   viewer is slow, so a slow viewer would stall the camera for everyone
 *   else; dropping costs that viewer a reconnect and costs the stream
 *   nothing.  It is only defensible because a client that comes back can be
 *   brought up to the stream again from the initialisation segment.
 *
 ****************************************************************************/

/* Whether a client's buffer has room for `len` more bytes.
 *
 * Asked before anything is queued rather than after, because a frame is two
 * writes -- a header and a payload -- and a header that was queued while its
 * payload was refused leaves the client reading the next frame's bytes as
 * this frame's contents.  Nothing recovers from that, so the all-or-nothing
 * question has to be asked first.
 */

static bool ws_room(FAR const struct camenc_ws_client_s *c, size_t len)
{
  return c->tx_len + len <= c->tx_size;
}

/* Say once, however many times it happens, that a client is not keeping up.
 * A line per frame for the rest of the run buries whatever else the log was
 * going to say, and the condition does not change frame to frame.
 */

static void ws_warn_full(FAR struct camenc_ws_s *ws,
                         FAR const struct camenc_ws_client_s *c, size_t len)
{
  if (!ws->warned_full)
    {
      _warn("CAMENC WS: a %zu-byte frame does not fit %zu of %zu bytes; that "
            "client is behind and some frames are being thrown away\n",
            len, c->tx_len, c->tx_size);
      ws->warned_full = true;
    }
}

static bool ws_queue(FAR struct camenc_ws_s *ws,
                     FAR struct camenc_ws_client_s *c, const uint8_t *data,
                     size_t len)
{
  if (!ws_room(c, len))
    {
      ws_warn_full(ws, c, len);
      return false;
    }

  memcpy(c->tx + c->tx_len, data, len);
  c->tx_len += len;

  /* Compact once everything queued has gone out, so that a long-lived
   * connection does not walk its buffer to the end and stop.
   */

  if (c->tx_sent == c->tx_len)
    {
      c->tx_sent = 0;
      c->tx_len = 0;
    }

  return true;
}

/****************************************************************************
 * Name: ws_frame
 *
 * Description:
 *   Queue one WebSocket frame: a two-byte header, an extended length if
 *the payload needs one, and the payload.
 *
 *   The mask bit is clear, as it must be for anything a server sends.
 *The opcode is the final one, because a frame that fits is never
 *fragmented.
 *
 ****************************************************************************/

static bool ws_frame(FAR struct camenc_ws_s *ws,
                     FAR struct camenc_ws_client_s *c, uint8_t opcode,
                     const uint8_t *data, size_t len)
{
  uint8_t hdr[10];
  size_t hlen;

  hdr[0] = (uint8_t)(0x80u | opcode); /* FIN, and this opcode */

  if (len < 126)
    {
      hdr[1] = (uint8_t)len;
      hlen = 2;
    }
  else if (len <= 0xffff)
    {
      hdr[1] = 126;
      hdr[2] = (uint8_t)(len >> 8);
      hdr[3] = (uint8_t)len;
      hlen = 4;
    }
  else
    {
      int i;

      hdr[1] = 127;

      for (i = 0; i < 8; i++)
        {
          hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
        }

      hlen = 10;
    }

  if (!ws_room(c, hlen + len))
    {
      /* Refused before the header, so the client's stream is still whole.
       * See ws_room.  Whether this costs it a frame or its connection is
       * the caller's decision.
       */

      ws_warn_full(ws, c, hlen + len);
      return false;
    }

  if (!ws_queue(ws, c, hdr, hlen))
    {
      return false;
    }

  return len == 0 || ws_queue(ws, c, data, len);
}

/****************************************************************************
 * Name: ws_flush
 *
 * Description:
 *   Write out as much of a client's buffer as the socket will take.
 *
 *   The socket is non-blocking, so a partial write is normal and the
 *rest waits for the next poll.
 *
 ****************************************************************************/

static void ws_flush(FAR struct camenc_ws_s *ws,
                     FAR struct camenc_ws_client_s *c)
{
  while (c->tx_sent < c->tx_len)
    {
      ssize_t n = send(c->fd, c->tx + c->tx_sent, c->tx_len - c->tx_sent, 0);

      if (n > 0)
        {
          c->tx_sent += (size_t)n;
          c->sent += (uint64_t)n;
          continue;
        }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
          return; /* the socket is full; the rest goes next time */
        }

      _warn("CAMENC WS: send failed: %d\n", errno);
      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }

  c->tx_sent = 0;
  c->tx_len = 0;

  /* A reply that is the whole of a connection is closed once it has
   * actually gone out, not when it was queued -- closing on the queue
   * would cut the reply off partway for anyone whose socket was full at
   * that moment.
   */

  if (c->closing)
    {
      ws_close_client(ws, c);
    }
}

/****************************************************************************
 * Name: ws_serve_*
 *
 * Description:
 *   The two things a plain HTTP request can be: a request for the page,
 *   or a request to upgrade.  Anything else gets a short answer saying so.
 *
 ****************************************************************************/

static void ws_serve_page(FAR struct camenc_ws_s *ws,
                          FAR struct camenc_ws_client_s *c)
{
  FAR const char *codec = ws->codec[0] != '\0' ? ws->codec : "avc1.42c016";
  char head[256];
  size_t clen = strlen(codec);
  size_t blen = sizeof(g_page_head) - 1u + clen + sizeof(g_page_tail) - 1u;
  int hlen;

  hlen = snprintf(head, sizeof(head),
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/html; charset=utf-8\r\n"
                  "Content-Length: %zu\r\n"
                  "Cache-Control: no-store\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  blen);
  if (hlen < 0 || (size_t)hlen >= sizeof(head))
    {
      return;
    }

  if (!ws_room(c, (size_t)hlen + blen))
    {
      /* The page is all this connection was for, so there is nothing to be
       * done for it but to let it go -- unlike a stream, which can be
       * degraded and still be a stream.
       */

      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }

  /* The reply in four pieces, all of them const and none of them a copy --
   * see the note above the page.  They are queued rather than written, so a
   * reply larger than one write is not a special case: it accumulates in
   * the client's own buffer exactly as a segment does.
   */

  if (!ws_queue(ws, c, (FAR const uint8_t *)head, (size_t)hlen) ||
      !ws_queue(ws, c, (FAR const uint8_t *)g_page_head,
                sizeof(g_page_head) - 1u) ||
      !ws_queue(ws, c, (FAR const uint8_t *)codec, clen) ||
      !ws_queue(ws, c, (FAR const uint8_t *)g_page_tail,
                sizeof(g_page_tail) - 1u))
    {
      /* ws_queue has already dropped the client it could not fit.  Stopping
       * here rather than queueing the rest is the difference between a
       * client that is gone and one being quietly filled with a reply
       * nobody will read.
       */

      return;
    }

  /* The page is all this connection was for.  It is written out by the next
   * flush and then closed, because a fresh connection is what the page's
   * WebSocket will make anyway.
   */

  c->closing = true;
  ws_flush(ws, c);
}

static void ws_serve_upgrade(FAR struct camenc_ws_s *ws,
                             FAR struct camenc_ws_client_s *c,
                             FAR const char *req)
{
  FAR const char *key = ws_header(req, "Sec-WebSocket-Key");
  char concat[128];
  char accept[64];
  char reply[256];
  SHA1_CTX sha;
  uint8_t digest[SHA1_DIGEST_LENGTH];
  int n;

  if (key == NULL)
    {
      /* A browser's upgrade always carries the key, so its absence means
       * the request was not what the server took it for.  What arrived
       * is then the whole of the question, so it is printed -- the
       * request line alone would not say whether the key was missing
       * from the request or lost between here and there.  Line breaks
       * are shown rather than obeyed, so that it stays one line in the
       * log.
       */

      char seen[320];
      size_t used;
      size_t total = strlen(req);

      for (used = 0; used < sizeof(seen) - 1 && used < total; used++)
        {
          char ch = req[used];

          seen[used] = (ch == '\r' || ch == '\n') ? '|' : ch;
        }

      seen[used] = '\0';

      _warn("CAMENC WS: upgrade without a key, %zu bytes: %s\n", total, seen);
      ws_close_client(ws, c);
      return;
    }

  /* The digest covers the client's key and the fixed string,
   * concatenated. The key is printed up to the line's end, so it has to
   * be copied out rather than pointed at.
   */

  {
    size_t i = 0;
    size_t k = 0;

    while (k < sizeof(concat) - sizeof(CAMENC_WS_GUID) - 1 && key[k] != '\0' &&
           key[k] != '\r' && key[k] != '\n')
      {
        concat[i++] = key[k++];
      }

    memcpy(concat + i, CAMENC_WS_GUID, sizeof(CAMENC_WS_GUID));
    i += sizeof(CAMENC_WS_GUID) - 1;

    sha1init(&sha);
    sha1update(&sha, concat, (unsigned int)i);
    sha1final(digest, &sha);
  }

  ws_base64(digest, sizeof(digest), accept);

  n = snprintf(reply, sizeof(reply),
               "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n"
               "\r\n",
               accept);
  if (n < 0 || (size_t)n >= sizeof(reply))
    {
      ws_close_client(ws, c);
      return;
    }

  if (!ws_queue(ws, c, (FAR const uint8_t *)reply, (size_t)n))
    {
      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }

  c->upgraded = true;
  c->have_init = false;
  c->waiting = true;
  ws->clients_served++;

  /* Give it the stream's own description if that is known, and nothing
   * else.
   *
   * The rest of what a decoder needs to start is a segment it may start on,
   * and the next one of those is at most one group of pictures away.  Waiting
   * for it rather than replaying the group in progress is the fix for a
   * client that could not connect at all; see the note on `waiting` in
   * camenc_ws.h.
   */

  if (ws->init_len > 0)
    {
      _info("CAMENC WS: new client, given the %zu-byte initialisation "
            "segment\n",
            ws->init_len);

      /* If even this cannot be queued, the client cannot be brought up at
       * all: it has no way to learn what the stream is, and waiting for an
       * initialisation segment that has already been published means
       * waiting for ever.
       */

      if (!ws_frame(ws, c, CAMENC_WS_OP_BIN, ws->init_seg, ws->init_len))
        {
          ws_close_client(ws, c);
          ws->clients_dropped++;
          return;
        }

      c->have_init = true;
    }
  else
    {
      _info("CAMENC WS: new client, waiting for the initialisation "
            "segment\n");
    }

  ws_flush(ws, c);
}

/****************************************************************************
 * Name: ws_handle_request
 ****************************************************************************/

static void ws_handle_request(FAR struct camenc_ws_s *ws,
                              FAR struct camenc_ws_client_s *c)
{
  FAR const char *upgrade = ws_header((FAR const char *)c->rx, "Upgrade");

  if (upgrade != NULL && strncasecmp(upgrade, "websocket", 9) == 0)
    {
      size_t used = ws_head_len(c->rx, c->rx_len);

      ws_serve_upgrade(ws, c, (FAR const char *)c->rx);

      /* The request has been answered, so it is consumed.  Anything
       * behind it on this connection is a frame, and reading from the
       * front of the buffer again would read the request as one.
       */

      if (used <= c->rx_len)
        {
          memmove(c->rx, c->rx + used, c->rx_len - used);
          c->rx_len -= used;
        }

      return;
    }

  if (strncmp((FAR const char *)c->rx, "GET ", 4) == 0)
    {
      ws_serve_page(ws, c);
      return;
    }

  {
    static const char notfound[] = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n"
                                   "\r\n";

    if (ws_queue(ws, c, (FAR const uint8_t *)notfound, sizeof(notfound) - 1))
      {
        c->closing = true;
        ws_flush(ws, c);
      }
    else
      {
        ws_close_client(ws, c);
      }
  }
}

/****************************************************************************
 * Name: ws_close_client
 ****************************************************************************/

static void ws_close_client(FAR struct camenc_ws_s *ws,
                            FAR struct camenc_ws_client_s *c)
{
  if (c->fd >= 0)
    {
      close(c->fd);
      c->fd = -1;
    }

  c->upgraded = false;
  c->closing = false;
  c->have_init = false;
  c->waiting = false;
  c->rx_len = 0;
  c->tx_len = 0;
  c->tx_sent = 0;

  (void)ws;
}

/****************************************************************************
 * Name: ws_read_client
 *
 * Description:
 *   Consume whatever a client has sent.
 *
 *   Before the handshake that means collecting a request; afterwards it
 *   means answering a close and ignoring a ping, and noticing when the
 *peer has gone.  Reading at all is what makes that last part work: a
 *socket that is never read reports a disconnection only when something
 *is finally written to it, which for a stream that has stopped for any
 *   reason is never.
 *
 ****************************************************************************/

static void ws_read_client(FAR struct camenc_ws_s *ws,
                           FAR struct camenc_ws_client_s *c)
{
  for (;;)
    {
      ssize_t n;

      /* The receive buffer keeps a byte back for the terminator, so that
       * what has arrived is always a string: see ws_header.
       */

      if (c->rx_len >= sizeof(c->rx) - 1)
        {
          /* A request this long is not a request.  A frame cannot reach
           * here because frames are consumed as they are read.
           */

          _warn("CAMENC WS: request too long, dropping the connection\n");
          ws_close_client(ws, c);
          ws->clients_dropped++;
          return;
        }

      n = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - 1 - c->rx_len, 0);

      if (n > 0)
        {
          c->rx_len += (size_t)n;
          c->rx[c->rx_len] = '\0';

          if (!c->upgraded)
            {
              if (ws_head_len(c->rx, c->rx_len) != 0)
                {
                  ws_handle_request(ws, c);
                  return;
                }

              continue;
            }

          /* Upgraded: everything received is a frame, and the only one
           * worth acting on is close.
           */

          {
            size_t i = 0;

            while (i + 2 <= c->rx_len)
              {
                uint8_t opcode = c->rx[i] & 0x0fu;
                uint8_t masked = c->rx[i + 1] & 0x80u;
                uint64_t len = c->rx[i + 1] & 0x7fu;
                size_t hdr = 2;

                if (len == 126)
                  {
                    if (i + 4 > c->rx_len)
                      {
                        break;
                      }

                    len = ((uint64_t)c->rx[i + 2] << 8) | c->rx[i + 3];
                    hdr = 4;
                  }
                else if (len == 127)
                  {
                    int k;

                    if (i + 10 > c->rx_len)
                      {
                        break;
                      }

                    len = 0;
                    for (k = 0; k < 8; k++)
                      {
                        len = (len << 8) | c->rx[i + 2 + k];
                      }

                    hdr = 10;
                  }

                if (masked)
                  {
                    hdr += 4;
                  }

                if (i + hdr + len > c->rx_len)
                  {
                    break;
                  }

                if (opcode == CAMENC_WS_OP_CLOSE)
                  {
                    _info("CAMENC WS: client closed\n");
                    ws_close_client(ws, c);
                    return;
                  }

                if (opcode == CAMENC_WS_OP_PING)
                  {
                    ws_frame(ws, c, CAMENC_WS_OP_PONG, c->rx + i + hdr,
                             (size_t)len);
                  }

                if (opcode == CAMENC_WS_OP_TEXT)
                  {
                    /* A text frame is a control message, and the server
                     * hands it on without reading it -- see
                     * camenc_ws_cmd_t.
                     *
                     * It arrives masked, as everything from a client must,
                     * so it is unmasked into a buffer of its own rather
                     * than in place: what follows this frame in the receive
                     * buffer belongs to the next one, and unmasking in place
                     * would leave it unreadable.
                     *
                     * A frame longer than the buffer is dropped rather than
                     * truncated.  Half a command is a different command, and
                     * running the first half of one would be worse than
                     * ignoring it.
                     */

                    if (len > CAMENC_WS_CMD_MAX)
                      {
                        _warn("CAMENC WS: ignoring a %zu-byte control "
                              "message\n",
                              (size_t)len);
                      }
                    else if (masked)
                      {
                        uint8_t text[CAMENC_WS_CMD_MAX + 1];
                        size_t k;

                        for (k = 0; k < (size_t)len; k++)
                          {
                            text[k] = c->rx[i + hdr + k] ^
                                      c->rx[i + hdr - 4 + (k & 3u)];
                          }

                        text[(size_t)len] = '\0';

                        if (ws->on_command != NULL)
                          {
                            ws->on_command((FAR const char *)text,
                                           ws->command_arg);
                          }
                      }
                  }

                i += hdr + (size_t)len;
              }

            if (i > 0)
              {
                memmove(c->rx, c->rx + i, c->rx_len - i);
                c->rx_len -= i;
              }

            return;
          }
        }

      if (n == 0)
        {
          _info("CAMENC WS: client went away\n");
          ws_close_client(ws, c);
          return;
        }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return;
        }

      _warn("CAMENC WS: recv failed: %d\n", errno);
      ws_close_client(ws, c);
      ws->clients_dropped++;
      return;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camenc_ws_start(FAR struct camenc_ws_s *ws, uint16_t port, size_t tx_size)
{
  struct sockaddr_in addr;
  int one = 1;
  int i;

  if (ws == NULL || tx_size <= CAMENC_WS_TX_HEADER)
    {
      return -EINVAL;
    }

  memset(ws, 0, sizeof(*ws));
  ws->listen_fd = -1;
  ws->tx_size = tx_size;

  /* Every descriptor says "none" before anything is allocated, because a
   * failure part-way through runs the teardown below, and zero is a
   * descriptor it would close -- the console.
   */

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      ws->clients[i].fd = -1;
      ws->clients[i].tx_size = tx_size;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      ws->clients[i].tx = malloc(tx_size);
      if (ws->clients[i].tx == NULL)
        {
          camenc_ws_stop(ws);
          return -ENOMEM;
        }
    }

  ws->init_seg = malloc(CAMENC_WS_INIT_MAX);
  if (ws->init_seg == NULL)
    {
      camenc_ws_stop(ws);
      return -ENOMEM;
    }

  ws->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (ws->listen_fd < 0)
    {
      int err = -errno;

      camenc_ws_stop(ws);
      return err;
    }

  setsockopt(ws->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(ws->listen_fd, (FAR struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      int err = -errno;

      _err("CAMENC WS: cannot bind port %u: %d\n", port, errno);
      camenc_ws_stop(ws);
      return err;
    }

  if (listen(ws->listen_fd, CAMENC_WS_MAX_CLIENTS) < 0)
    {
      int err = -errno;

      camenc_ws_stop(ws);
      return err;
    }

  if (fcntl(ws->listen_fd, F_SETFL,
            fcntl(ws->listen_fd, F_GETFL, 0) | O_NONBLOCK) < 0)
    {
      int err = -errno;

      _err("CAMENC WS: the listening socket will not go non-blocking: %d\n",
           errno);
      camenc_ws_stop(ws);
      return err;
    }

  ws->port = port;
  return 0;
}

void camenc_ws_set_codec(FAR struct camenc_ws_s *ws, FAR const char *codec)
{
  if (ws != NULL && codec != NULL)
    {
      strncpy(ws->codec, codec, sizeof(ws->codec) - 1);
      ws->codec[sizeof(ws->codec) - 1] = '\0';
    }
}

void camenc_ws_stop(FAR struct camenc_ws_s *ws)
{
  int i;

  if (ws == NULL)
    {
      return;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      if (ws->clients[i].fd >= 0)
        {
          close(ws->clients[i].fd);
          ws->clients[i].fd = -1;
        }

      free(ws->clients[i].tx);
      ws->clients[i].tx = NULL;
    }

  if (ws->listen_fd >= 0)
    {
      close(ws->listen_fd);
      ws->listen_fd = -1;
    }

  free(ws->init_seg);
  ws->init_seg = NULL;
  ws->init_len = 0;
}

void camenc_ws_poll(FAR struct camenc_ws_s *ws)
{
  struct sockaddr_in addr;
  socklen_t alen;
  int i;

  if (ws == NULL || ws->listen_fd < 0)
    {
      return;
    }

  /* Take whatever is waiting.  Non-blocking, so this is one accept per
   * connection and never a wait.
   */

  for (;;)
    {
      int fd;
      int slot;

      alen = sizeof(addr);
      fd = accept(ws->listen_fd, (FAR struct sockaddr *)&addr, &alen);

      if (fd < 0)
        {
          break;
        }

      for (slot = 0; slot < CAMENC_WS_MAX_CLIENTS; slot++)
        {
          if (ws->clients[slot].fd < 0)
            {
              break;
            }
        }

      if (slot == CAMENC_WS_MAX_CLIENTS)
        {
          _warn("CAMENC WS: no room for another client\n");
          close(fd);
          continue;
        }

      if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0)
        {
          _err("CAMENC WS: the connection will not go non-blocking: %d\n",
               errno);
          close(fd);
          continue;
        }

      /* Reset the connection's state, field by field.
       *
       * Not with memset: the slot owns its transmit buffer, which was
       * allocated when the server started and is meant to outlive any
       * one connection.  Clearing the whole struct would throw that
       * pointer away and leave the next write going to address zero.
       */

      {
        FAR struct camenc_ws_client_s *c = &ws->clients[slot];

        c->fd = fd;
        c->upgraded = false;
        c->closing = false;
        c->have_init = false;
        c->waiting = false;
        c->sent = 0;
        c->skipped = 0;
        c->rx_len = 0;
        c->tx_len = 0;
        c->tx_sent = 0;

        /* The receive buffer too, not just its length.  A request is
         * parsed as a string, so a byte left from the last connection is
         * a byte this one did not send -- and a header it does not have
         * can be found in it.
         */

        memset(c->rx, 0, sizeof(c->rx));
      }

      _info("CAMENC WS: client %d connected\n", slot);
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0)
        {
          continue;
        }

      ws_read_client(ws, c);

      if (c->fd < 0)
        {
          continue;
        }

      ws_flush(ws, c);
    }
}

int camenc_ws_publish(FAR struct camenc_ws_s *ws, enum camenc_seg_e seg,
                      const uint8_t *data, size_t len, bool key)
{
  int i;

  if (ws == NULL || data == NULL)
    {
      return -EINVAL;
    }

  /* Keep the initialisation segment, for clients that arrive after it.
   *
   * It is the one thing a late client cannot do without.  What is *not* kept
   * is the group of pictures in progress, and that is deliberate: a client
   * used to be given the last segment a stream may start on and everything
   * since, so that it could start immediately, and that cannot be made to
   * work.  The catch-up and the live frames share one transmit buffer, and
   * the live frames alone filled it in about three frames, so the client was
   * dropped while it was still completing its handshake and a browser
   * reported that it could not connect at all.
   *
   * A client waits for the next segment a stream may start on instead, which
   * is at most one group of pictures away.  See camenc_ws.h.
   */

  if (len + CAMENC_WS_TX_HEADER > ws->tx_size)
    {
      /* A segment no client can be sent.  Said once, because if the encoder
       * produces segments this size it produces them every time.
       */

      if (!ws->warned_oversize)
        {
          _warn("CAMENC WS: %zu-byte segments do not fit a %zu-byte "
                "buffer; no client can be served at this size\n",
                len, ws->tx_size);
          ws->warned_oversize = true;
        }
    }
  else if (seg == CAMENC_SEG_INIT && len <= CAMENC_WS_INIT_MAX)
    {
      memcpy(ws->init_seg, data, len);
      ws->init_len = len;
    }

  /* Then to everyone connected. */

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0 || !c->upgraded)
        {
          continue;
        }

      /* A client that has not been brought up to the stream yet is given
       * only the two things that can bring it up: the initialisation
       * segment, and then the first segment a stream may start on.  What
       * falls between them is dropped for that client rather than queued
       * for it -- queueing it is what used to fill the buffer.
       */

      if (!c->have_init)
        {
          if (seg != CAMENC_SEG_INIT)
            {
              continue;
            }

          c->have_init = true;
        }
      else if (c->waiting)
        {
          if (seg == CAMENC_SEG_INIT || !key)
            {
              continue;
            }

          c->waiting = false;
        }

      /* A frame that does not fit is thrown away for this client, which
       * keeps its connection.
       *
       * For a live stream that is the honest degradation.  The client is
       * behind, so what it needs is less data rather than all of it late:
       * dropping the frame loses one picture, and closing the connection
       * loses the viewer until it reconnects -- which for a moment of
       * backlog at the start of a stream is what used to happen, every
       * time, and looked like a server that refused connections.
       *
       * The caveat is P pictures.  A fragment whose pictures refer to the
       * one before it cannot be dropped on its own, because the fragments
       * that follow it refer to a picture the client never received.  Every
       * fragment is a key fragment here, which is why this is safe today;
       * when the encoder emits P pictures the flag that says so is what a
       * fix has to use -- the client has to be told to start again at the
       * next key fragment instead of being fed the ones in between.
       */

      if (!ws_frame(ws, c, CAMENC_WS_OP_BIN, data, len))
        {
          c->skipped++;
          continue;
        }

      ws_flush(ws, c);
    }

  return 0;
}

/****************************************************************************
 * Name: camenc_ws_report
 *
 * Description:
 *   Print one line about how the clients are doing.
 *
 *   `pending` is the interesting number.  It is data the server has accepted
 *   and the socket has not, so it says both whether the link is draining and
 *   how far behind the client is.  `sent` says how much has gone out, and
 *   `skipped` how much was thrown away for not fitting, which together
 *   answer the question the capture loop's own timings cannot: a browser
 *   that receives the stream and one that receives nothing cost the loop
 *   exactly the same.
 *
 ****************************************************************************/

void camenc_ws_report(FAR struct camenc_ws_s *ws)
{
  uint64_t pending = 0;
  uint64_t sent = 0;
  uint64_t skipped = 0;
  unsigned int live = 0;
  unsigned int i;

  if (ws == NULL)
    {
      return;
    }

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR const struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0)
        {
          continue;
        }

      live++;
      pending += (uint64_t)(c->tx_len - c->tx_sent);
      sent += c->sent;
      skipped += c->skipped;
    }

  _info("CAMENC WS: %u client(s), pending %llu, sent %llu, skipped %llu, "
        "served %" PRIu32 ", dropped %" PRIu32 "\n",
        live, pending, sent, skipped, ws->clients_served, ws->clients_dropped);
}

/****************************************************************************
 * Name: camenc_ws_status
 *
 * Description:
 *   Send one text frame to every client, as far as it fits.
 *
 *   The difference between this and camenc_ws_publish is the whole of the
 *   design.  A segment that does not fit its client drops that client,
 *   because a viewer that cannot keep up with a live stream is not being
 *   served by delaying the camera for it.  A status message is not the
 *   stream: it says what the loop is doing, and a viewer whose buffer happens
 *   to be full at that moment loses one readout and keeps its picture.
 *
 *   So the room is checked before the header is queued rather than by letting
 *   ws_queue fail on the payload.  A header that was queued and a payload
 *   that was not is not a dropped message but a corrupt stream: the client
 *   reads the next frame's bytes as this one's contents and never recovers.
 *   And ws_queue cannot simply be asked first, because by then the header is
 *   already in the buffer.
 *
 ****************************************************************************/

void camenc_ws_status(FAR struct camenc_ws_s *ws, FAR const char *text,
                      size_t len)
{
  size_t hlen;
  unsigned int i;

  if (ws == NULL || len > 0xffffu)
    {
      return;
    }

  hlen = len < 126u ? 2u : 4u;

  for (i = 0; i < CAMENC_WS_MAX_CLIENTS; i++)
    {
      FAR struct camenc_ws_client_s *c = &ws->clients[i];

      if (c->fd < 0 || !c->upgraded)
        {
          continue;
        }

      if (c->tx_len + hlen + len > c->tx_size)
        {
          continue;
        }

      ws_frame(ws, c, CAMENC_WS_OP_TEXT, (FAR const uint8_t *)text, len);
      ws_flush(ws, c);
    }
}

/****************************************************************************
 * Name: camenc_ws_set_command
 *
 * Description:
 *   Say where a control message from a client should be handed.
 *
 ****************************************************************************/

void camenc_ws_set_command(FAR struct camenc_ws_s *ws, camenc_ws_cmd_t fn,
                           FAR void *arg)
{
  if (ws != NULL)
    {
      ws->on_command = fn;
      ws->command_arg = arg;
    }
}
